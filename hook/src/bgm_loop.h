// BoF4 BGM-loop diagnostic hooks (Phase 1: logging-only)
//
// Hooks three sites inside dshow.dll to map the runtime control flow at the
// BGM loop seam:
//   1. 0x100C4110   — central play/open function (ffmpeg avformat_open_input
//                     wrapper). Called for every "play this BGM" issued by the
//                     game.
//   2. 0x100C1AF0   — vtable[16] of the 12-byte stream-control object. The
//                     game calls this to (re-)issue playback; it copies a
//                     200-byte descriptor and forwards to 0x100C4110.
//   3. 0x100C3A9F   — EOF block inside decoder thread (0x100C3500). Calls
//                     IsWindow + PostMessageA to notify the game the stream
//                     ran out, then loops back to retry the read.
//
// Each call writes a single line to d3d9_hook.log. From the temporal
// arrangement of those lines we can determine whether the game (a) re-opens
// via 0x100C4110, (b) seeks via some other vtable method, or (c) restarts
// via vtable[16] which then re-enters 0x100C4110.
//
// All hooks are gated on g_cfg.bgm_loop_enabled; if false the install is a
// no-op and we incur zero runtime cost.

#pragma once

// Install the bgm-log hooks. Idempotent. Safe to call before dshow.dll is
// loaded (we LoadLibraryA it ourselves).
void bgm_loop_install();
