// Soft-subtitle overlay for the BoF4 intro FMV (MOV\ZBOF4.DAT).
//
// Ported from the standalone bof4-subtitles-hook into the main resolver
// hook. This module owns three things that used to live in three files:
//   - SRT parsing                (was srt.cpp)
//   - the GDI->D3D9 text overlay  (was overlay.cpp)
//   - movie open/close detection  (was file_hook.cpp, minus the IAT patch)
//
// It does NOT install any hooks of its own. The host hook drives it:
//   * subtitles_init()              once at startup (after config parse)
//   * subtitles_note_file_open/close from the existing CreateFile/CloseHandle
//     trampolines, so the movie file's open/close is observed without a
//     second hook on those APIs.
//   * subtitles_on_present()        from the Device/SwapChain Present hook,
//     each frame, before delegating to the original Present.
//   * subtitles_on_lost_device()    from the Reset hook, before the inner
//     Reset, to release the D3DPOOL_DEFAULT overlay texture.
//
// All settings come from the shared `_d3d9_hook_config.txt` (subtitle_*
// keys parsed into g_cfg); there is no separate .ini.
#pragma once

// Opaque forward decls so this header doesn't need d3d9.h (mirrors hooks.h).
struct IDirect3DDevice9;
struct IDirect3DSwapChain9;

// Populate the local config mirror from g_cfg and load the SRT file named by
// `subtitle_srt`. Safe to call once; a no-op when subtitle_enabled is false.
void subtitles_init();

// True if the subtitle feature is enabled AND the SRT loaded with >=1 cue.
// Lets the host decide whether the Present/Reset/SwapChain hooks are worth
// installing at all.
bool subtitles_active();

// Called each frame from the Present hook BEFORE the original Present runs.
// Draws the cue active at the current movie time onto the backbuffer. `sc`
// may be null (Device::Present path); when non-null (SwapChain::Present) the
// overlay binds that swap chain's backbuffer. No-op when no movie is playing.
void subtitles_on_present(IDirect3DDevice9* dev, IDirect3DSwapChain9* sc);

// Called from the Reset hook before the inner Reset. Releases GPU resources
// that live in D3DPOOL_DEFAULT (otherwise Reset fails with INVALIDCALL).
void subtitles_on_lost_device();

// Movie-file taps, called from the host's CreateFile / CloseHandle hooks.
// `nameA` is the (ANSI) path passed to CreateFile; `handle` the result.
// Only paths ending in ZBOF4.DAT are tracked; everything else is ignored.
void subtitles_note_file_open(void* handle, const char* nameA);
void subtitles_note_file_close(void* handle);
