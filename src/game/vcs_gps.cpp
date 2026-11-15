// Enhancements > Radar: GPS routes on the radar and the pause menu's map (docs/mods/GPS_ROUTE.md has the addresses and
// how they were found).
//
// Two routes at once, each in its target's colour:
// - the mission's: the nearest mission blip - a blip in use with no radar sprite (+0x29 == 0) that shows on the radar
//   (display >= 2); contacts and safehouses have sprites. Coordinate blips (type 4) give their position; car / char
//   blips (1 / 2) the entity's, through the script handle in +0x08 and the vehicle / ped pools (the radar itself reads
//   those live too: the entry's position stays 0).
// - the player's target: the game's own (the pause menu's map places it - the red square), a coordinate blip with
//   the target sprite 0x10 that FUN_00115030 makes (SetTargetBlip: type 4, colour 0, display 2).
// Each route is A* over the game's car path nodes (ThePaths: all 3079 car nodes are in memory at once), computed on
// the game thread at most every 0.75 s of game time or when its target moves; when the target is not reachable (the
// car graph has a few separate parts) it leads to the explored node nearest the target.
//
// Drawing: a hook on the entry of the radar's blip pass FUN_00113418, which both the HUD radar (0x321500, after the map
// tiles) and the pause map (FUN_0026BA60, radar +0x1AD9 = 1) call - so the lines lie over the map and under the blips
// and the player's arrow, and go through the HUD layer like the rest of the radar / menu. Each segment is one call of
// the game's own untextured quad routine FUN_003E00A8 (colour a0, corners f12..f19 in the PSP's 480x272 HUD units; it
// sets its render states itself). A guest function can't be called from inside a hook (the EE scheduler may unwind),
// so the calls are chained as tail calls: the hook sets the quad's arguments, ra = 0x113418 and pc = 0x3E00A8 and
// returns; the quad routine returns to 0x113418, the dispatcher calls this hook again, which draws the next quad, and
// after the last one puts a0 / ra back and runs the original blip pass. On the radar the lines are clipped to its unit
// circle in radar space; on the map the map's own scissor clips them.
//
// VCS_GPS=0/1/2 overrides the setting; VCS_GPS_HOOKS=0 installs nothing. `gps` -> ok mode targets nodes quads;
// `gps target X Y` / `gps clear` put / take a target blip as FUN_00115030 / ClearBlip leave it (tests).
#include "game/vcs_gps.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_control.h"
#include "settings/settings.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

namespace vcs::gps
{
    namespace
    {
        constexpr uint32_t kBlipPass = 0x00113418u;  // the radar's blips, north and player arrow (a0 = the radar)
        constexpr uint32_t kQuad = 0x003E00A8u;      // untextured quad: a0 colour (RGBA), f12..f19 corners (HUD units)
        constexpr uint32_t kRadarPtr = 0x0048F548u;  // CRadar instance
        constexpr uint32_t kHudFlags = 0x0048F3D0u;  // *(this) + 0x30: the "TV is 16:9" HUD squeeze (FUN_002AF658)
        constexpr uint32_t kPathsPtr = 0x00487490u;  // ThePaths
        constexpr uint32_t kPlayerPed = 0x004E4910u; // CWorld::Players[0].m_pPed
        constexpr uint32_t kVehiclePool = 0x00487A98u, kPedPool = 0x00487A94u; // {objects, flags, size}
        constexpr uint32_t kVehicleSize = 0x8C0u, kPedSize = 0xD20u;
        constexpr uint32_t kArea = 0x00489F7Cu;      // CGame::currArea: 0 outdoors
        constexpr uint32_t kTimeMs = 0x004CD104u;    // CTimer::m_snTimeInMilliseconds
        constexpr uint32_t kRadarCentre = 0x004D3E68u, kRadarSin = 0x004D3E70u, kRadarCos = 0x004D3E74u;
        constexpr uint32_t kHudScaleX = 0x0048A0D8u, kHudScaleY = 0x0048A0DCu; // HUD unit -> 640x448 pixel
        constexpr uint32_t kColour = 0x00062000u;    // guest scratch: the quad's RGBA (vcs_aim 0x60000, prompts 0x61000)
        constexpr uint32_t kPos = 0x30u, kPedVehicle = 0x480u;
        constexpr uint32_t kBlips = 0x280u, kBlipCount = 75u, kBlipSize = 0x30u;
        constexpr uint8_t kTargetSprite = 0x10u;      // the player's target (FUN_00115030)
        constexpr uint32_t kTargetRoute = 0xFF3C3CE6u; // its route: red as the target's square (bytes R, G, B, A)
        constexpr float kHideWithin = 20.0f; // m from the target
        // the straight legs off the road (player -> first node, last node -> target) only up to this long: a longer
        // one runs through walls and buildings (a target inside a walled yard, the player inside an interior's lot)
        constexpr float kOffRoad = 12.0f;
        // nearest node: height past kLevelSlack (slopes, the nodes' whole metres) counts kLevelWeight times as much as
        // the same distance across
        constexpr float kLevelWeight = 10.0f, kLevelSlack = 2.0f;
        constexpr float kLineWidth = 2.5f;   // pixels at 640x448
        constexpr size_t kMaxQuads = 400;

        enum Mode
        {
            Off = 0,
            InVehicle = 1,
            Always = 2,
        };
        std::atomic<int> g_mode{Off};
        PS2Runtime::RecompiledFunction g_orig = nullptr;
        bool g_installed = false;

        struct Vec2
        {
            float x, y;
        };
        struct Quad
        {
            float c[8]; // corners in perimeter order, HUD units
            uint32_t rgba;
        };
        struct Route
        {
            std::vector<Vec2> nodes; // world x, y (without the player / the target)
            Vec2 target{0, 0};
            uint32_t at = 0;
            bool have = false;
        };

        // game thread
        Route g_mission, g_targetRoute;
        std::vector<Quad> g_quads;
        size_t g_next = 0;
        bool g_busy = false;
        uint32_t g_savedA0 = 0, g_savedRa = 0;
        std::atomic<bool> g_mapSeen{false};
        // control-channel requests (any thread -> game thread)
        std::mutex g_reqMutex;
        bool g_reqSet = false, g_reqClear = false;
        Vec2 g_reqAt{0, 0};
        std::atomic<int> g_lastNodes{0}, g_lastWpNodes{0}, g_lastQuads{0};
        std::atomic<float> g_lastTx{0}, g_lastTy{0}, g_lastWx{0}, g_lastWy{0};
        std::atomic<bool> g_lastWp{false};

        uint8_t *g_ram = nullptr;
        uint32_t rd32(uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, g_ram + (a & 0x01FFFFFFu), 4);
            return v;
        }
        uint16_t rd16(uint32_t a)
        {
            uint16_t v;
            std::memcpy(&v, g_ram + (a & 0x01FFFFFFu), 2);
            return v;
        }
        float rdf(uint32_t a)
        {
            float v;
            std::memcpy(&v, g_ram + (a & 0x01FFFFFFu), 4);
            return v;
        }
        uint8_t rd8(uint32_t a) { return g_ram[a & 0x01FFFFFFu]; }
        void wr32(uint32_t a, uint32_t v) { std::memcpy(g_ram + (a & 0x01FFFFFFu), &v, 4); }
        void wrf(uint32_t a, float v) { std::memcpy(g_ram + (a & 0x01FFFFFFu), &v, 4); }
        float bits(uint32_t u)
        {
            float f;
            std::memcpy(&f, &u, 4);
            return f;
        }

        // CPool::GetAt for a script handle (index << 8 | the slot's flag byte); 0 when it is gone
        uint32_t poolAt(uint32_t poolPtr, uint32_t size, uint32_t handle)
        {
            const uint32_t pool = rd32(poolPtr);
            const int32_t index = static_cast<int32_t>(handle) >> 8;
            if (!pool || index < 0 || index >= static_cast<int32_t>(rd32(pool + 8)))
                return 0;
            if (rd8(rd32(pool + 4) + index) != (handle & 0xFFu))
                return 0;
            return rd32(pool) + static_cast<uint32_t>(index) * size;
        }

        // a blip colour -> the quad routine's RGBA bytes (as a little-endian word: R lowest)
        uint32_t blipColour(uint32_t c)
        {
            // the radar's colour index as in the PS2-era games; anything larger is RGBA as it is
            static const uint32_t table[] = {0xFF2A2AC8u, 0xFF46C846u, 0xFFE07C3Eu, 0xFFFFFFFFu,
                                             0xFF28C8F0u, 0xFFDC46C8u, 0xFFE6C83Cu};
            if (c < 7)
                return table[c];
            return (c >> 24) | ((c >> 8) & 0xFF00u) | ((c << 8) & 0xFF0000u) | 0xFF000000u;
        }

        uint32_t blipEntry(uint32_t radar, uint32_t i) { return radar + kBlips + i * kBlipSize; }
        bool blipInUse(uint32_t e) { return (rd32(e + 0x20) & 4u) != 0; }
        bool isTarget(uint32_t e) { return blipInUse(e) && rd32(e + 4) == 4u && rd8(e + 0x29) == kTargetSprite; }

        int findTarget(uint32_t radar)
        {
            for (uint32_t i = 0; i < kBlipCount; ++i)
                if (isTarget(blipEntry(radar, i)))
                    return static_cast<int>(i);
            return -1;
        }

        // tests only (`gps clear`): take the target blip out as the game's ClearBlip leaves an entry
        void clearTarget(uint32_t radar)
        {
            for (int i; (i = findTarget(radar)) >= 0;)
            {
                const uint32_t e = blipEntry(radar, static_cast<uint32_t>(i));
                wr32(e + 0x20, rd32(e + 0x20) & ~4u);
                wr32(e + 4, 0);
            }
            g_targetRoute = {};
        }

        // tests only (`gps target X Y`): the target blip as FUN_00115030 makes it - the pause map's own way to set it
        void setTarget(uint32_t radar, Vec2 at, float z)
        {
            clearTarget(radar);
            for (uint32_t i = 0; i < kBlipCount; ++i)
            {
                const uint32_t e = blipEntry(radar, i);
                if (blipInUse(e))
                    continue;
                uint8_t entry[0x30] = {};
                const float pos[4] = {at.x, at.y, z, 0.0f};
                const uint32_t head[3] = {0u, 4u, 0u};       // colour 0, coordinate blip, no entity
                const uint32_t flags = 0x56u;                // bits 1, 2 (in use), 4; display 2 (bits 5..7)
                const float scale = 1.0f;
                std::memcpy(entry, head, 12);
                std::memcpy(entry + 0x10, pos, 16);
                std::memcpy(entry + 0x24, &scale, 4);
                entry[0x28] = 1;
                entry[0x29] = kTargetSprite;
                std::memcpy(g_ram + (e & 0x01FFFFFFu), entry, 0x20);
                std::memcpy(g_ram + ((e + 0x24) & 0x01FFFFFFu), entry + 0x24, 0x0C);
                wr32(e + 0x20, flags); // the in-use flag last
                g_targetRoute = {};
                return;
            }
        }

        // DIRTY FIX (hand-placed route ends): where the road graph's nearest node to a blip is the wrong way in (a target
        // behind walls), the route leads to a hand-picked point instead. The blip itself is untouched; matching is by
        // the blip's position, within `radius` m.
        struct DirtyFixRouteEnd
        {
            const char *mission; // for the reader only
            Vec2 blip;           // about where the blip stands
            float radius;
            Vec2 end;            // where the route ends instead
            float endZ;
        };
        constexpr DirtyFixRouteEnd kDirtyFixRouteEnds[] = {
            // Mission 1 "Soldier" (Fort Baxter): "Go to your barracks" - the barracks' blip stands in the walled yard,
            // the nearest road leads to its back wall; the route ends at the yard's way in instead
            {"Soldier (first mission): barracks", {-1648.87f, -261.00f}, 25.0f, {-1733.14f, -314.18f}, 14.71f},
        };

        void dirtyFixMissionSoldierRouteEnd(Vec2 &target, float &targetZ)
        {
            for (const auto &f : kDirtyFixRouteEnds)
                if (std::hypot(target.x - f.blip.x, target.y - f.blip.y) <= f.radius)
                {
                    target = f.end;
                    targetZ = f.endZ;
                    return;
                }
        }

        // the mission target: nearest suitable blip (no sprite: not the player's target); false when there is none
        bool findMissionTarget(uint32_t radar, Vec2 player, Vec2 &target, float &targetZ, uint32_t &colour)
        {
            float best = 1e30f;
            bool found = false;
            for (uint32_t i = 0; i < kBlipCount; ++i)
            {
                const uint32_t e = blipEntry(radar, i);
                const uint32_t flags = rd32(e + 0x20);
                if (!(flags & 4u) || rd8(e + 0x29) != 0 || ((flags >> 5) & 7u) < 2)
                    continue;
                const uint32_t type = rd32(e + 4);
                Vec2 p;
                float z;
                if (type == 4u)
                    p = {rdf(e + 0x10), rdf(e + 0x14)}, z = rdf(e + 0x18);
                else if (type == 1u || type == 2u)
                {
                    const uint32_t ent = type == 1u ? poolAt(kVehiclePool, kVehicleSize, rd32(e + 8))
                                                    : poolAt(kPedPool, kPedSize, rd32(e + 8));
                    if (!ent)
                        continue;
                    p = {rdf(ent + kPos), rdf(ent + kPos + 4)}, z = rdf(ent + kPos + 8);
                }
                else
                    continue;
                const float d = (p.x - player.x) * (p.x - player.x) + (p.y - player.y) * (p.y - player.y);
                if (d < best)
                {
                    best = d;
                    target = p;
                    targetZ = z;
                    colour = rd32(e);
                    found = true;
                }
            }
            return found;
        }

        // A* over the car nodes; true and the route's nodes in `out`
        bool computeRoute(Vec2 from, float fromZ, Vec2 to, float toZ, std::vector<Vec2> &out)
        {
            const uint32_t paths = rd32(kPathsPtr);
            if (!paths)
                return false;
            const uint32_t nodes = rd32(paths), links = rd32(paths + 0x7BA0u);
            const uint32_t n = rd32(paths + 0x10u); // car nodes: indices 0..n-1
            if (!nodes || !links || n == 0 || n > 0x4000u)
                return false;
            std::vector<Vec2> pos(n);
            // the car nodes hold the boats' too: flags (+8) bit 8 (0x100), at sea level - whole separate parts of the
            // graph along the coast (docs/mods/GPS_ROUTE.md); a road route leaves them out
            std::vector<uint8_t> water(n);
            std::vector<float> height(n); // whole metres; FUN_0026B200: a negative byte is that - 100
            for (uint32_t i = 0; i < n; ++i)
            {
                const int8_t z = static_cast<int8_t>(rd8(nodes + i * 10u + 4u));
                height[i] = z >= 0 ? z : z - 100.0f;
                pos[i] = {static_cast<int16_t>(rd16(nodes + i * 10u)) * 0.125f,
                          static_cast<int16_t>(rd16(nodes + i * 10u + 2u)) * 0.125f};
                water[i] = (rd16(nodes + i * 10u + 8u) & 0x100u) != 0;
            }
            // in 3D where roads run over each other (the first mission's target on Fort Baxter's upper deck, a lower
            // road under it): the node on the target's / the player's own level wins. A height of about 0 is none -
            // the pause map's target (FUN_00115030) has z 0 - and then only x, y count.
            const auto nearest = [&](Vec2 p, float pz) {
                const bool level = std::fabs(pz) > 0.5f && pz > -99.0f;
                uint32_t bi = 0;
                float bd = 1e30f;
                for (uint32_t i = 0; i < n; ++i)
                {
                    if (water[i])
                        continue;
                    const float dz = level ? std::max(0.0f, std::fabs(height[i] - pz) - kLevelSlack) * kLevelWeight : 0.0f;
                    const float d = (pos[i].x - p.x) * (pos[i].x - p.x) + (pos[i].y - p.y) * (pos[i].y - p.y) + dz * dz;
                    if (d < bd)
                        bd = d, bi = i;
                }
                return bi;
            };
            const uint32_t s = nearest(from, fromZ), t = nearest(to, toZ);
            const auto h = [&](uint32_t i) { return std::hypot(pos[i].x - pos[t].x, pos[i].y - pos[t].y); };
            std::vector<float> g(n, 1e30f);
            std::vector<int32_t> prev(n, -1);
            std::vector<uint8_t> closed(n, 0);
            using Item = std::pair<float, uint32_t>;
            std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
            g[s] = 0;
            open.push({h(s), s});
            uint32_t bestNode = s;
            float bestH = h(s);
            while (!open.empty())
            {
                const uint32_t u = open.top().second;
                open.pop();
                if (closed[u])
                    continue;
                closed[u] = 1;
                if (const float hu = h(u); hu < bestH)
                    bestH = hu, bestNode = u;
                if (u == t)
                    break;
                const uint32_t node = nodes + u * 10u;
                const int32_t first = static_cast<int16_t>(rd16(node + 6u));
                const uint32_t count = rd16(node + 8u) & 0xFu;
                for (uint32_t k = 0; k < count; ++k)
                {
                    const uint32_t v = rd16(links + 2u * static_cast<uint32_t>(first + static_cast<int32_t>(k))) & 0x3FFFu;
                    if (v >= n || closed[v] || water[v])
                        continue;
                    const float d = g[u] + std::hypot(pos[u].x - pos[v].x, pos[u].y - pos[v].y);
                    if (d < g[v])
                    {
                        g[v] = d;
                        prev[v] = static_cast<int32_t>(u);
                        open.push({d + h(v), v});
                    }
                }
            }
            out.clear();
            for (int32_t i = static_cast<int32_t>(bestNode); i >= 0; i = prev[static_cast<uint32_t>(i)])
                out.push_back(pos[static_cast<uint32_t>(i)]);
            std::reverse(out.begin(), out.end());
            // the nearest node can be behind the player: start from the next one when that is nearer
            if (out.size() >= 2)
            {
                const Vec2 a = out[0], b = out[1];
                if (std::hypot(from.x - b.x, from.y - b.y) < std::hypot(a.x - b.x, a.y - b.y))
                    out.erase(out.begin());
            }
            return !out.empty();
        }

        void updateRoute(Route &r, Vec2 player, float playerZ, Vec2 target, float targetZ)
        {
            const uint32_t now = rd32(kTimeMs);
            const bool moved = std::hypot(target.x - r.target.x, target.y - r.target.y) > 5.0f;
            if (!r.have || moved || now - r.at >= 750u || now < r.at)
            {
                r.have = computeRoute(player, playerZ, target, targetZ, r.nodes);
                r.target = target;
                r.at = now;
            }
        }

        // world -> radar space (FUN_00114C98, scale 1): the radar is the unit circle; on the pause map the radar's
        // centre / range / rotation are the map's (FUN_00111130)
        Vec2 toRadar(uint32_t radar, Vec2 w)
        {
            const float range = rdf(radar + 0x1AC8u);
            const float vx = (w.x - rdf(kRadarCentre)) / range, vy = (w.y - rdf(kRadarCentre + 4)) / range;
            const float s = rdf(kRadarSin), c = rdf(kRadarCos);
            return {c * vx + s * vy, -s * vx + c * vy};
        }

        // clip the segment a-b to the circle of radius r; false when nothing is inside
        bool clipCircle(Vec2 &a, Vec2 &b, float r)
        {
            const float dx = b.x - a.x, dy = b.y - a.y;
            const float A = dx * dx + dy * dy, B = 2 * (a.x * dx + a.y * dy), C = a.x * a.x + a.y * a.y - r * r;
            if (A < 1e-12f)
                return C <= 0;
            const float disc = B * B - 4 * A * C;
            if (disc <= 0)
                return false;
            const float sq = std::sqrt(disc);
            const float t0 = std::max(0.0f, (-B - sq) / (2 * A)), t1 = std::min(1.0f, (-B + sq) / (2 * A));
            if (t0 >= t1)
                return false;
            const Vec2 o = a;
            a = {o.x + dx * t0, o.y + dy * t0};
            b = {o.x + dx * t1, o.y + dy * t1};
            return true;
        }

        // FUN_00114758's two branches: radar space -> HUD units
        constexpr float kMapK = 0.56f;            // 0x3F0F5C29
        const float kMapDx = bits(0xBED9E98Eu);   // the map's x origin in radar space
        const float kMapDy = bits(0xBDEECBFCu);   // and y
        Vec2 radarToUnits(uint32_t radar, bool map, Vec2 r)
        {
            const uint32_t hud = rd32(kHudFlags);
            const bool tv = hud && rd32(hud + 0x30u);
            if (map)
            {
                const float zoom = rdf(radar + 0x1AE4u) * kMapK;
                return {rdf(radar + 0x1ADCu) + (r.x - kMapDx) * zoom * (tv ? 1.0f : bits(0x3FAAAAABu)),
                        rdf(radar + 0x1AE0u) - (r.y - kMapDy) * zoom};
            }
            const float w = tv ? 48.0f : 64.0f, hh = 48.0f;   // FUN_0031F270 / FUN_0031F2C0
            const float x0 = 28.0f, y0 = 204.0f;              // FUN_0031F2D0 / FUN_0031F2E0
            return {x0 + w * 0.5f + r.x * w * 0.5f, y0 + hh * 0.5f - r.y * hh * 0.5f};
        }

        void addLine(uint32_t radar, bool map, Vec2 player, const std::vector<Vec2> &nodes, Vec2 target, uint32_t rgba)
        {
            std::vector<Vec2> line;
            line.reserve(nodes.size() + 2);
            const auto shortLeg = [](Vec2 a, Vec2 b) { return std::hypot(a.x - b.x, a.y - b.y) <= kOffRoad; };
            if (nodes.empty() || shortLeg(player, nodes.front()))
                line.push_back(player);
            line.insert(line.end(), nodes.begin(), nodes.end());
            if (nodes.empty() || shortLeg(target, nodes.back()))
                line.push_back(target);
            const float sx = rdf(kHudScaleX), sy = rdf(kHudScaleY);
            if (!(sx > 0.1f && sy > 0.1f))
                return;
            const float half = kLineWidth * 0.5f;
            for (size_t i = 0; i + 1 < line.size() && g_quads.size() < kMaxQuads; ++i)
            {
                Vec2 a = toRadar(radar, line[i]), b = toRadar(radar, line[i + 1]);
                if (!map && !clipCircle(a, b, 0.98f))
                    continue;
                const Vec2 ua = radarToUnits(radar, map, a), ub = radarToUnits(radar, map, b);
                // on the map: skip what is off the 480x272 screen (the map's scissor clips the rest)
                if (map && (std::max(ua.x, ub.x) < -8.0f || std::min(ua.x, ub.x) > 488.0f ||
                            std::max(ua.y, ub.y) < -8.0f || std::min(ua.y, ub.y) > 280.0f))
                    continue;
                Vec2 pa{ua.x * sx, ua.y * sy}, pb{ub.x * sx, ub.y * sy}; // pixels: an even width
                float dx = pb.x - pa.x, dy = pb.y - pa.y;
                const float len = std::hypot(dx, dy);
                if (len < 0.25f)
                    continue;
                dx /= len, dy /= len;
                pa = {pa.x - dx * half, pa.y - dy * half}; // overlap the joints by half the width
                pb = {pb.x + dx * half, pb.y + dy * half};
                const float nx = -dy * half, ny = dx * half;
                const Vec2 c[4] = {{pa.x + nx, pa.y + ny}, {pb.x + nx, pb.y + ny}, {pb.x - nx, pb.y - ny}, {pa.x - nx, pa.y - ny}};
                Quad q;
                for (int k = 0; k < 4; ++k)
                    q.c[k * 2] = c[k].x / sx, q.c[k * 2 + 1] = c[k].y / sy;
                q.rgba = rgba;
                g_quads.push_back(q);
            }
        }

        // the requests from the control channel (tests), on the game thread
        void takeRequests(uint32_t radar, float z)
        {
            std::lock_guard<std::mutex> lock(g_reqMutex);
            if (g_reqClear)
                clearTarget(radar);
            if (g_reqSet)
                setTarget(radar, g_reqAt, z);
            g_reqClear = g_reqSet = false;
        }

        // build this frame's quads
        void buildFrame(uint32_t radar)
        {
            g_quads.clear();
            const int mode = g_mode.load(std::memory_order_relaxed);
            if (mode == Off || !radar)
                return;
            const bool map = rd8(radar + 0x1AD9u) != 0;
            if (map)
                g_mapSeen.store(true, std::memory_order_relaxed);
            const uint32_t ped = rd32(kPlayerPed);
            if (!ped)
                return;
            const uint32_t vehicle = rd32(ped + kPedVehicle);
            const uint32_t at = vehicle ? vehicle : ped;
            const Vec2 player{rdf(at + kPos), rdf(at + kPos + 4)};
            const float playerZ = rdf(at + kPos + 8);
            takeRequests(radar, rdf(at + kPos + 8));
            // the player's target (the game's, set on the pause map)
            Vec2 tp{0, 0};
            float tpZ = 0;
            const int ti = findTarget(radar);
            if (ti >= 0)
            {
                const uint32_t e = blipEntry(radar, static_cast<uint32_t>(ti));
                tp = {rdf(e + 0x10), rdf(e + 0x14)};
                tpZ = rdf(e + 0x18);
            }
            g_lastWp.store(ti >= 0, std::memory_order_relaxed);
            g_lastWx.store(tp.x, std::memory_order_relaxed);
            g_lastWy.store(tp.y, std::memory_order_relaxed);
            if (rd32(kArea) != 0 || (!map && mode == InVehicle && !vehicle))
                return; // inside, or on foot with "In a vehicle" (the map shows the routes on foot too)
            Vec2 target{};
            float targetZ = 0;
            uint32_t colour = 0;
            if (findMissionTarget(radar, player, target, targetZ, colour))
            {
                dirtyFixMissionSoldierRouteEnd(target, targetZ); // DIRTY FIX: the first mission's barracks
                g_lastTx.store(target.x, std::memory_order_relaxed);
                g_lastTy.store(target.y, std::memory_order_relaxed);
                if (std::hypot(target.x - player.x, target.y - player.y) >= kHideWithin)
                {
                    updateRoute(g_mission, player, playerZ, target, targetZ);
                    g_lastNodes.store(static_cast<int>(g_mission.nodes.size()), std::memory_order_relaxed);
                    if (g_mission.have)
                        addLine(radar, map, player, g_mission.nodes, target, blipColour(colour));
                }
            }
            else
                g_mission = {};
            // the player's target last: over the mission's line where they share roads
            if (ti >= 0 && std::hypot(tp.x - player.x, tp.y - player.y) >= kHideWithin)
            {
                updateRoute(g_targetRoute, player, playerZ, tp, tpZ);
                g_lastWpNodes.store(static_cast<int>(g_targetRoute.nodes.size()), std::memory_order_relaxed);
                if (g_targetRoute.have)
                    addLine(radar, map, player, g_targetRoute.nodes, tp, kTargetRoute);
            }
            else if (ti < 0)
                g_targetRoute = {};
        }

        void setQuad(uint8_t *rdram, R5900Context *ctx, const Quad &q)
        {
            std::memcpy(rdram + kColour, &q.rgba, 4);
            // FUN_003E0368's vertex order: (f16, f17), (f18, f19), (f14, f15), (f12, f13) - a fan around the quad
            ctx->f[16] = q.c[0], ctx->f[17] = q.c[1];
            ctx->f[18] = q.c[2], ctx->f[19] = q.c[3];
            ctx->f[14] = q.c[4], ctx->f[15] = q.c[5];
            ctx->f[12] = q.c[6], ctx->f[13] = q.c[7];
            SET_GPR_U32(ctx, 4, kColour);
            SET_GPR_U32(ctx, 31, kBlipPass);
            ctx->pc = kQuad;
        }

        PS2_GUEST_CC void blipPass(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_ram = rdram;
            if (g_busy && GPR_U32(ctx, 31) == kBlipPass)
            {
                // back from a quad: the next, or the original pass with its own a0 / ra
                if (g_next < g_quads.size())
                {
                    setQuad(rdram, ctx, g_quads[g_next++]);
                    return;
                }
                g_busy = false;
                SET_GPR_U32(ctx, 4, g_savedA0);
                SET_GPR_U32(ctx, 31, g_savedRa);
                ctx->pc = kBlipPass;
                g_orig(rdram, ctx, runtime);
                return;
            }
            g_busy = false;
            const uint32_t radar = GPR_U32(ctx, 4);
            buildFrame(radar);
            g_lastQuads.store(static_cast<int>(g_quads.size()), std::memory_order_relaxed);
            if (g_quads.empty())
            {
                g_orig(rdram, ctx, runtime);
                return;
            }
            g_savedA0 = radar;
            g_savedRa = GPR_U32(ctx, 31);
            g_busy = true;
            g_next = 0;
            setQuad(rdram, ctx, g_quads[g_next++]);
        }
    }

    void registerSettings()
    {
        settings::Spec s;
        s.key = "enhance.gps_route";
        s.page = "Enhancements";
        s.section = "Radar";
        s.label = "GPS route";
        s.help = "Lines along the roads to the mission's target and to your own target (set on the pause menu's map), on "
                 "the radar and the map, as in the later games. Off (the original game), on the radar only while you "
                 "are in a vehicle, or always.";
        s.type = settings::Type::Choice;
        s.choices = {{"Off", Off}, {"In a vehicle", InVehicle}, {"Always", Always}};
        s.defaultValue = Off;
        s.env = "VCS_GPS";
        s.onChange = [](int v) { g_mode.store(v, std::memory_order_relaxed); };
        settings::add(s);
    }

    void install(PS2Runtime &runtime)
    {
        if (const char *env = std::getenv("VCS_GPS_HOOKS"); env && env[0] == '0')
            std::fprintf(stderr, "[gps] off (VCS_GPS_HOOKS=0)\n");
        else if ((g_orig = runtime.lookupFunction(kBlipPass)) && runtime.lookupFunction(kQuad))
        {
            runtime.replaceFunction(kBlipPass, &blipPass);
            g_installed = true;
        }
        else
            std::fprintf(stderr, "[gps] the radar's blip pass not found: no GPS route\n");

        // gps -> ok hooked mode target nodes player-target tnodes quads; gps target X Y; gps clear (tests)
        ps2control::addCommand("gps", [](const std::string &args, ps2control::Reply reply) {
            float x = 0, y = 0;
            if (std::sscanf(args.c_str(), " target %f %f", &x, &y) == 2)
            {
                std::lock_guard<std::mutex> lock(g_reqMutex);
                g_reqSet = true;
                g_reqAt = {x, y};
                return reply("ok");
            }
            if (args.find("clear") != std::string::npos)
            {
                std::lock_guard<std::mutex> lock(g_reqMutex);
                g_reqClear = true;
                return reply("ok");
            }
            char text[256];
            std::snprintf(text, sizeof(text),
                          "ok hooked=%d mode=%d mission=%.1f,%.1f nodes=%d target=%d:%.1f,%.1f tnodes=%d quads=%d map=%d",
                          g_installed ? 1 : 0, g_mode.load(), g_lastTx.load(), g_lastTy.load(), g_lastNodes.load(),
                          g_lastWp.load() ? 1 : 0, g_lastWx.load(), g_lastWy.load(), g_lastWpNodes.load(),
                          g_lastQuads.load(), g_mapSeen.load() ? 1 : 0);
            reply(text);
        });
    }
}
