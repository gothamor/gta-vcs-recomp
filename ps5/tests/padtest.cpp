// The PS5 controller path, checked on the build machine (python3 tools/ps5.py padtest; ps5/tests/CMakeLists.txt;
// docs/PS5.md): the real platform_ps5.cpp with the console's pad / user / TSC functions mocked below, the real bindings
// (src/game/vcs_input.cpp), settings and the runtime's pad backend and control channel. DualSense states in, the
// DualShock 2 report the game reads out: every button and stick, the triggers, Select on the touchpad (tap and hold),
// the menu chords kept from the game, a controller gone and back, the motors, the Controller page, and the pad as Dear
// ImGui's gamepad keys. The DualSense's button bits and ScePadData's head are ps5-plume-triangle's, verified on a
// console.
#include "game/vcs_input.h"
#include "platform/platform.h"
#include "runtime/ps2_pad.h"
#include "settings/settings.h"
#include "imgui.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <time.h>

// ---- the console's functions, mocked ---------------------------------------------------------------------------
struct PadData { uint32_t buttons; uint8_t lx, ly, rx, ry, l2, r2; uint8_t rest[246]; };
static PadData g_state{};
static int g_readResult = 0, g_openResult = 7, g_vibLarge = -1, g_vibSmall = -1, g_vibMode = -1, g_openedUser = -2, g_openCalls = 0;
static int g_initialUser = 0x10000001, g_foregroundUser = 0x10000002;
extern "C" {
int sceUserServiceInitialize(void *) { return 0; }
int sceUserServiceGetInitialUser(int32_t *u) { if (g_initialUser < 0) return -1; *u = g_initialUser; return 0; }
int sceUserServiceGetForegroundUser(int32_t *u) { *u = g_foregroundUser; return 0; }
struct SceUserServiceLoginUserIdList { int32_t userId[4]; };
int sceUserServiceGetLoginUserIdList(SceUserServiceLoginUserIdList *l) { l->userId[0] = 0x10000003; return 0; }
int scePadInit(void) { return 0; }
int scePadOpen(int32_t user, int32_t, int32_t, const void *) { ++g_openCalls; if (g_openResult >= 0) g_openedUser = user; return g_openResult; }
int scePadGetHandle(int32_t, int32_t, int32_t) { return g_openResult; }
int scePadReadState(int32_t h, void *d) { if (h != 7) return -1; std::memcpy(d, &g_state, sizeof(g_state)); return g_readResult; }
int scePadClose(int32_t) { return 0; }
int scePadSetVibrationMode(int32_t, int32_t m) { g_vibMode = m; return 0; }
struct ScePadVibrationParam { uint8_t largeMotor, smallMotor; };
int scePadSetVibration(int32_t, const ScePadVibrationParam *p) { g_vibLarge = p->largeMotor; g_vibSmall = p->smallMotor; return 0; }
int32_t sceAudioOutInit(void) { return 0; }
int32_t sceAudioOutOpen(int32_t, int32_t, int32_t, uint32_t, uint32_t, uint32_t) { return -1; }
int32_t sceAudioOutOutput(int32_t, const void *) { return 0; }
int32_t sceAudioOutClose(int32_t) { return 0; }
int sceSystemServiceHideSplashScreen(void) { return 0; }
uint64_t sceKernelGetTscFrequency(void) { return 1000000000ull; }
uint64_t sceKernelReadTsc(void) { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return uint64_t(t.tv_sec) * 1000000000ull + t.tv_nsec; }
struct Ps5NotifyRequest { char reserved[45]; char message[3075]; };
int sceKernelSendNotificationRequest(int, Ps5NotifyRequest *, size_t, int) { return 0; }
}

// ---- the rest of the game, which the controller path names but this test leaves out ----------------------------
namespace vcs::freecam
{
    bool flying() { return false; } // game/vcs_freecam.cpp (the trainer's free camera)
}
namespace ps2perf
{
    std::string summaryLine() { return {}; } // ps2_perf.cpp (the control channel's "perf")
}

// ---- the checks ------------------------------------------------------------------------------------------------
static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { if (cond) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

struct Report { uint16_t pressed; uint8_t rx, ry, lx, ly; };
static Report report()
{
    uint8_t d[32];
    PSPadBackend backend;
    backend.readState(0, 0, d, sizeof(d));
    return {static_cast<uint16_t>(~(d[2] | d[3] << 8)), d[4], d[5], d[6], d[7]};
}
static void pad(uint32_t buttons, uint8_t lx = 0x80, uint8_t ly = 0x80, uint8_t rx = 0x80, uint8_t ry = 0x80, uint8_t l2 = 0, uint8_t r2 = 0)
{
    g_state = {};
    g_state.buttons = buttons; g_state.lx = lx; g_state.ly = ly; g_state.rx = rx; g_state.ry = ry; g_state.l2 = l2; g_state.r2 = r2;
    platform::pumpEvents();
}
static void ms(int n) { std::this_thread::sleep_for(std::chrono::milliseconds(n)); }

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    vcs::input::registerSettings();
    settings::load("/nonexistent/vcs.ini");
    settings::applyAll();
    CHECK(platform::init(), "init");
    CHECK(g_openedUser == g_initialUser, "the initial user's pad opened (got 0x%x)", g_openedUser);
    CHECK(g_vibMode == 2, "vibration mode 2 (got %d)", g_vibMode);
    vcs::input::install();
    { // the font atlas the renderer's ImGui backend would build (no renderer in this test)
        unsigned char *pixels = nullptr;
        int fw = 0, fh = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &fw, &fh);
    }

    // DualSense bits (ps5-plume-triangle's table) -> DualShock 2 report bits (active low in the report)
    constexpr uint16_t kSelect = 0x0001, kL3 = 0x0002, kR3 = 0x0004, kStart = 0x0008, kUp = 0x0010, kRight = 0x0020,
                       kDown = 0x0040, kLeft = 0x0080, kL2 = 0x0100, kR2 = 0x0200, kL1 = 0x0400, kR1 = 0x0800,
                       kTriangle = 0x1000, kCircle = 0x2000, kCross = 0x4000, kSquare = 0x8000;
    struct { uint32_t ds; uint16_t ps2; const char *name; } map[] = {
        {0x4000, kCross, "Cross"}, {0x2000, kCircle, "Circle"}, {0x8000, kSquare, "Square"}, {0x1000, kTriangle, "Triangle"},
        {0x0400, kL1, "L1"}, {0x0800, kR1, "R1"}, {0x0100, kL2, "L2 (digital bit)"}, {0x0200, kR2, "R2 (digital bit)"},
        {0x0002, kL3, "L3"}, {0x0004, kR3, "R3"}, {0x0008, kStart, "Options -> Start"},
        {0x0010, kUp, "Up"}, {0x0040, kDown, "Down"}, {0x0080, kLeft, "Left"}, {0x0020, kRight, "Right"},
    };
    pad(0);
    Report r = report();
    CHECK(platform::gamepadConnected(), "connected");
    CHECK(r.pressed == 0 && r.lx == 0x80 && r.ly == 0x80 && r.rx == 0x80 && r.ry == 0x80, "neutral: %04x %02x %02x %02x %02x",
          r.pressed, r.lx, r.ly, r.rx, r.ry);
    for (auto &m : map)
    {
        pad(m.ds);
        r = report();
        CHECK(r.pressed == m.ps2, "%s: report %04x, want %04x", m.name, r.pressed, m.ps2);
    }
    pad(0x4000 | 0x8000 | 0x0800 | 0x0010);
    CHECK(report().pressed == (kCross | kSquare | kR1 | kUp), "Cross + Square + R1 + Up together");

    // sticks: the DualSense's 0..255 (128 the centre, y down) -> the PS2's (the same convention)
    pad(0, 0x00, 0x80, 0x80, 0x80); r = report(); CHECK(r.lx == 0x00 && r.ly == 0x80, "left stick left: lx %02x ly %02x", r.lx, r.ly);
    pad(0, 0xFF, 0x80, 0x80, 0x80); r = report(); CHECK(r.lx == 0xFF, "left stick right: lx %02x", r.lx);
    pad(0, 0x80, 0x00, 0x80, 0x80); r = report(); CHECK(r.ly == 0x00, "left stick up: ly %02x", r.ly);
    pad(0, 0x80, 0xFF, 0x80, 0x80); r = report(); CHECK(r.ly == 0xFF, "left stick down: ly %02x", r.ly);
    pad(0, 0x80, 0x80, 0x00, 0x80); r = report(); CHECK(r.rx == 0x00 && r.ry == 0x80, "right stick left: rx %02x ry %02x", r.rx, r.ry);
    pad(0, 0x80, 0x80, 0x80, 0xFF); r = report(); CHECK(r.rx == 0x80 && r.ry == 0xFF, "right stick down: rx %02x ry %02x", r.rx, r.ry);
    pad(0, 0x80, 0x80, 0x00, 0xFF); r = report(); CHECK(r.rx == 0x25 && r.ry == 0xD9, "right stick diagonal, round gate: rx %02x ry %02x", r.rx, r.ry);
    pad(0, 0x84, 0x7C, 0x83, 0x7D); r = report(); CHECK(r.lx == 0x80 && r.ly == 0x80 && r.rx == 0x80 && r.ry == 0x80, "resting drift in the deadzone");
    pad(0, 0xC0, 0x80, 0x80, 0x80); r = report(); CHECK(r.lx > 0x98 && r.lx < 0xC0, "half right: lx %02x (deadzone stretched)", r.lx);

    // analog triggers: pressed past Gamepad > Trigger press point (30%)
    pad(0, 0x80, 0x80, 0x80, 0x80, 0x40, 0x00); CHECK(!(report().pressed & kL2), "L2 at 25%%: not pressed");
    pad(0, 0x80, 0x80, 0x80, 0x80, 0x60, 0xFF); r = report(); CHECK((r.pressed & (kL2 | kR2)) == (kL2 | kR2), "L2 38%% / R2 full: %04x", r.pressed);

    // the touchpad is Select: a tap reaches the game as a short press once let go, a hold after 150 ms
    pad(0x100000); CHECK(!(report().pressed & kSelect), "touchpad just down: no Select yet");
    ms(40); pad(0); CHECK(report().pressed & kSelect, "touchpad tapped: Select");
    ms(130); pad(0); CHECK(!(report().pressed & kSelect), "the tap played out");
    pad(0x100000); report(); ms(170); pad(0x100000); CHECK(report().pressed == kSelect, "touchpad held: Select");
    pad(0); report(); ms(130); report();

    // a menu chord (touchpad + Options / R1 / L1): neither Select nor the partner reaches the game
    for (uint32_t partner : {0x0008u, 0x0800u, 0x0400u})
    {
        pad(0x100000); CHECK(report().pressed == 0, "chord: touchpad first");
        ms(50); pad(0x100000 | partner); CHECK(report().pressed == 0, "chord with %x: nothing to the game", partner);
        ms(200); pad(0x100000 | partner); CHECK(report().pressed == 0, "chord held: still nothing");
        pad(0); CHECK(report().pressed == 0, "chord let go: no Select tap");
        ms(130);
    }

    // the controller gone: all-zero reads, then a failed read (closed, opened again after a second)
    pad(0, 0, 0, 0, 0); r = report();
    CHECK(!platform::gamepadConnected() && r.pressed == 0 && r.lx == 0x80, "all-zero read: no controller, neutral report");
    g_readResult = -1; pad(0x4000); CHECK(!platform::gamepadConnected() && report().pressed == 0, "failed read: no controller");
    g_readResult = 0; pad(0x4000); CHECK(platform::gamepadConnected() && report().pressed == kCross, "reopened at once (a second since the last try)");
    g_readResult = -1; pad(0x4000); g_readResult = 0; const int opens = g_openCalls;
    pad(0x4000); CHECK(g_openCalls == opens && !platform::gamepadConnected(), "failed again: not reopened within a second");
    ms(1050); pad(0x4000); CHECK(g_openCalls > opens && platform::gamepadConnected() && report().pressed == kCross, "reopened, Cross works");

    // the game's motors -> the DualSense's (the large motor 0..255, the small one on / off)
    PSPadBackend::actuators(0, 128); pad(0); CHECK(g_vibLarge == 128 && g_vibSmall == 0, "large motor 128: %d %d", g_vibLarge, g_vibSmall);
    PSPadBackend::actuators(1, 0); pad(0); CHECK(g_vibLarge == 0 && g_vibSmall == 255, "small motor: %d %d", g_vibLarge, g_vibSmall);
    PSPadBackend::actuators(0, 0); pad(0); CHECK(g_vibLarge == 0 && g_vibSmall == 0, "motors off");
    settings::set("pad.vibration", 50); PSPadBackend::actuators(0, 200); pad(0); CHECK(g_vibLarge == 100, "vibration 50%%: %d", g_vibLarge);

    // the menus: the pad as Dear ImGui's gamepad keys (Cross activates, Circle backs out, L1 / R1 change pages)
    pad(0x4000 | 0x0400); platform::beginFrame();
    CHECK(ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown) && ImGui::IsKeyDown(ImGuiKey_GamepadL1) && !ImGui::IsKeyDown(ImGuiKey_GamepadFaceRight),
          "ImGui: Cross = FaceDown, L1 = GamepadL1");
    platform::endUiFrame();
    pad(0x2000, 0x80, 0x00); platform::beginFrame();
    CHECK(ImGui::IsKeyDown(ImGuiKey_GamepadFaceRight) && ImGui::IsKeyDown(ImGuiKey_GamepadLStickUp), "ImGui: Circle = FaceRight, stick up");
    CHECK(ImGui::GetIO().BackendFlags & ImGuiBackendFlags_HasGamepad, "ImGui knows there is a gamepad");
    platform::endUiFrame();
    int w = 0, h = 0; platform::outputSize(w, h); CHECK(w == 1920 && h == 1080, "the TV's size before the surface: %dx%d", w, h);
    const std::vector<std::string> pages = settings::pages();
    CHECK(pages.size() == 1 && pages[0] == "Controller", "the bindings' one page: Controller (%zu pages)", pages.size());

    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
