// free_camera.h — experimental FREE (smooth) field-camera rotation for BoF4.
// GOG-only, config-gated (`free_camera` flag, default OFF). Isolated so the
// Steam build stays behavior-identical while this is developed.
//
// The field camera is an ORBIT camera: [0xB352B0/B8] is the look-at target and
// the camera yaw [0xB352CE] is DERIVED via atan2 from where the camera sits
// relative to it (writers at BOF4!0x5EA217 / 0x5EA8C7). A turn tweens an orbit
// target angle [0xB352CC] over [0xB352C8] frames at step [0xB352CA]; the player
// rotate handler snaps that target to 90° increments — that snap is what we want
// to open up.
//
// Phase 1 (this build): a diagnostic thread that logs the camera-state globals
// whenever they change, so an in-game rotation reveals the exact mechanics. No
// behavior change to the game.
#pragma once

// Start the Phase-1 diagnostic (no-op unless g_cfg.free_camera). Safe to call
// once from the install path; spawns its own polling thread.
void free_camera_install();
void free_camera_shutdown();
