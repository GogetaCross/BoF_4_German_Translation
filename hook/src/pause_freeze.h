// pause_freeze.h — "real pause" via a per-frame PSX-RAM save-state.
//
// A plain time-freeze can't coexist with the F10 rect tuner: the tuner edits UI
// rects only on frames where the game RE-SUBMITS them (the 0x502070 hook), and a
// frozen clock stops the game re-submitting. So instead we let the game keep
// running at full speed (renderer + tuner fully live) but SNAPSHOT the 2 MB of
// emulated PSX RAM the moment we pause and RE-APPLY it after every Present. The
// game then re-renders the exact same instant forever: on-screen nothing moves,
// yet every frame is a live re-submission the tuner can cycle and reshape.
//
// PSX RAM: runtime base at .data [0x009156D0] (static 0x006A8000), size 2 MB.
#pragma once

// Arm (true) / release (false) the freeze. On arm, the NEXT post-present frame is
// snapshotted (with its live pre-freeze timestamp), then every frame after that is
// pinned to that snapshot until released.
void pause_freeze_set(bool on);
bool pause_freeze_is_on();       // armed OR holding

// True only AFTER the snapshot has been captured — i.e. the clock hooks should now
// HOLD time at a fixed point. While merely armed (snapshot not yet taken) this is
// false so the snapshot frame runs on the normal clock (its stored "last frame
// time" must be a real pre-freeze value, else dt would pin to 0 and rendering
// would stop). See bof4_hooks.cpp clock hooks.
bool pause_freeze_holding();

// Call once per frame AFTER the real Present returns (render thread).
void pause_freeze_on_present();

// Free the snapshot buffer (DLL detach).
void pause_freeze_shutdown();
