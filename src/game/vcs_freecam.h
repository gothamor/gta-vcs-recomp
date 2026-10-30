// The trainer's free camera (docs/TRAINER.md "Camera"): fly anywhere while the game keeps running and the player stands
// still.
//
// CCamera::Process (FUN_00378dd8) runs the active camera's update, CCam::Process (FUN_00294540), then
// FUN_002a2ed0 (its height routines) on it, and builds the frame's camera - matrix, frustum, culling, streaming - from
// what that CCam left in its source / front / up / FOV (game-internals.md 12b). While the free camera flies both are
// replaced for the active CCam: the free camera writes those fields itself and returns. Off, they run as before;
// VCS_FREECAM=0 does not hook them at all (the reference).
#pragma once

class PS2Runtime;

namespace vcs::freecam
{
    // Before runtime.run(): the hooks (applyVcs).
    void install(PS2Runtime &runtime);

    // Any thread: the trainer's Free camera is on and the game is in play - the pad then gives the game nothing and the
    // keyboard / mouse / gamepad fly the camera (game/vcs_input.h).
    bool flying();

    struct Pose
    {
        bool active = false; // the camera is ours (it has been updated since the free camera was turned on)
        float x = 0, y = 0, z = 0;
        float yaw = 0, pitch = 0; // radians: yaw from +x toward +y, pitch up (+)
        float fov = 70;           // degrees
    };
    Pose pose(); // any thread: as of the last camera update
}
