// Enhancements > Radar: GPS routes on the radar and the pause map to the mission's target and the player's own target
// (the game's, set on the pause map), as in the later games and the remasters (docs/mods/GPS_ROUTE.md).
#pragma once

class PS2Runtime;

namespace vcs::gps
{
    // Enhancements > Radar: "GPS route" - Off (default) / In a vehicle / Always.
    void registerSettings();
    // The hook on the radar's blip pass (FUN_00113418) that draws the route under the blips; the `gps` command.
    // VCS_GPS_HOOKS=0: none.
    void install(PS2Runtime &runtime);
}
