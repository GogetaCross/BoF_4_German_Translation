// BoF4 d3d9 proxy — hook declarations
//
// Deliberately does NOT include d3d9.h so dllmain.cpp can re-declare
// Direct3DCreate9 as a dllexport without conflicting with d3d9.h's
// dllimport declaration. dllmain.cpp only needs opaque pointers;
// hooks.cpp and dumper.cpp include d3d9.h themselves.
#pragma once

#include <windows.h>
#include <cstdint>

// Forward declare as an opaque struct; the concrete COM interface is
// only used inside hooks.cpp / image_io.cpp where d3d9.h is included.
struct IDirect3D9;

typedef IDirect3D9* (WINAPI *Direct3DCreate9_t)(UINT);

extern HMODULE           g_real_d3d9;
extern Direct3DCreate9_t g_real_Direct3DCreate9;

// Install vtable hooks on the given IDirect3D9. Idempotent.
void hook_d3d9(IDirect3D9* obj);

// Append a formatted line to d3d9_hook.log in the game folder.
void hook_log(const char* fmt, ...);

// CRC32 of a byte buffer. Used to identify textures for inject matching.
uint32_t crc32_buf(const void* data, size_t len);

// Dump a texture's pixel data to tex_dump/<id>_<w>x<h>_<fmt>_<crc>.png
// Returns the CRC32 of the captured pixels (0 on failure).
uint32_t dump_texture_png(void* tex_ptr, UINT width, UINT height,
                          unsigned int format, INT pitch,
                          const void* pixels);

// Check for tex_inject/<crc8hex>.png. If present, load it, convert to
// the target D3D format, and overwrite the locked pixels in place.
// Returns true if an override was applied.
bool try_inject_texture(uint32_t crc, UINT width, UINT height,
                        unsigned int format, INT pitch, void* pixels);

// ── Feature flags ─────────────────────────────────────────────────────────
// Loaded once from `_d3d9_hook_config.txt` next to BOF4.exe. All default
// to OFF so a fresh install of the DLL is behavior-neutral; the user
// opts in by creating the config file.
struct HookConfig {
    bool  fast_forward_enabled;   // master switch: when false NONE of the
                                  // ff_* flags below take effect.
    float fast_forward_scale;     // time multiplier when FF is active (1..16)
    int   fast_forward_vk;        // virtual-key code for the hotkey
    bool  fast_forward_pad;       // enable gamepad FF cycling (XInput). Default on.
    char  fast_forward_pad_button[16]; // which physical button/combo cycles FF
                                  // (e.g. "L1", "R3", "BACK", "L2", "L1+R1").
                                  // Physical — unaffected by in-game remapping.

    // Per-API FF behavior. Each hook is only INSTALLED if its flag is
    // true at startup, so turning one off removes the hook entirely
    // (zero runtime cost). All default to true — current behavior.
    bool  ff_scale_gettickcount;  // GetTickCount + timeGetTime (share offset)
    bool  ff_scale_qpc;           // QueryPerformanceCounter
    bool  ff_skip_sleep;          // Sleep(1..100 ms) → Sleep(0)
    bool  ff_skip_wfso;           // WaitForSingleObject(_, 1..100 ms) → 0
    bool  ff_skip_wfmo;           // WaitForMultipleObjects(_, 1..100 ms) → 0
    bool  ff_skip_msgwait;        // MsgWaitForMultipleObjects(non-INF, !=0) → 0

    // PAUSE (heavy slow-motion). Rides the same virtual-clock the FF hooks own
    // (auto-installs GetTickCount + QPC scaling when on). One hotkey TOGGLES
    // between 1x and pause_scale. It is deliberately NOT a true 0.0 freeze: the
    // F10 rect tuner edits UI rects as the game RE-SUBMITS them each frame, and
    // a zero clock stops the game re-submitting (0x502070 never fires) — the
    // scene looks frozen but the tuner has nothing live to cycle/reshape. A
    // strong slow-mo (e.g. 1/8) keeps draws flowing so F10 stays fully live
    // while a transient element lingers on screen long enough to grab.
    bool  pause_slowmo;           // master switch for the pause feature
    int   pause_vk;               // freeze-lock hotkey (save-state real pause)
    int   pause_slow_vk;          // slow-mo hotkey (helper to line up a transient)
    float pause_scale;            // time scale while slow-mo (0<s<1; default 1/8)
    int   pause_stack_kb;         // KB at TOP of PSX RAM left un-restored (stack);
                                  // default 256. Raise if freeze still crashes,
                                  // lower if things still move while frozen.

    bool  tex_dump_enabled;       // write tex_dump/<...>.png on first upload
    bool  tex_inject_enabled;     // generic tex_inject/<crc>.png override
                                  // (TTF glyph override path always runs)
    bool  console_enabled;        // AllocConsole + mirror hook_log to stdout
    bool  unlock_vsync;           // force D3DPRESENT_INTERVAL_IMMEDIATE in
                                  // CreateDevice / Reset (bypasses the 30fps
                                  // cap on this PSX port)

    // Live text-substitution hook on the engine renderer 0x00629470.
    // Phase 1 ships trace-only; Phase 2 adds the substitution table;
    // Phase 3 adds the F8 reload hotkey. See text_subst.h.
    bool  text_substitute_enabled; // master switch for pointer rewrite
    bool  text_substitute_log;     // log every render call — used in
                                   // Phase 1 for arg discovery and later
                                   // for spotting untranslated strings
    int   text_substitute_hotkey;  // virtual-key code for the reload
                                   // hotkey (Phase 3; ignored in Phase 1)
    bool  text_resolver_enabled;   // Phase 3 page-driver hook on
                                   // 0x00527C30 — enables length-uncapped
                                   // translations via heap-buffer redirect.
                                   // Independent of text_substitute (the
                                   // in-place per-char patcher); turn off
                                   // if it causes glitches and only the
                                   // shorter, length-preserving in-place
                                   // path is desired.
    bool  text_render_log;         // DIAGNOSTIC: decode each rendered dialog
                                   // buffer (page driver) and print it to the
                                   // debug console with control codes shown as
                                   // [XX] escapes + raw hex. Deduped so each
                                   // message prints once. For comparing US vs a
                                   // translation / hunting duplicate strings.
    bool  item_name_uncap_enabled; // Two-byte patch lifting the 12-char
                                   // limit on [09:XX:YY] item-name
                                   // substitutions in dialog. Mirrors the
                                   // PSX SLUS_01324.ASM patch.
                                   // Off = leaves the original 12-char cap.
    int   item_name_uncap_value;   // Budget byte to write at the two
                                   // patch sites. Range 1..255. Engine
                                   // decrements it per rendered char and
                                   // stops at 0, so visible chars = value
                                   // - 1. Default 20 (= 19 visible chars,
                                   // matching PSX team's choice).
    bool  name_slot_bp_enabled;    // TEMP DIAGNOSTIC: HW write-watch on the
                                   // [07:00] item-name scratch (0x00B56EBF)
                                   // to find the 14-char-cap writer.
    bool  item_name_msg_uncap_enabled; // Lift the 14-char cap on item names
                                   // substituted into message text via [07:00]
                                   // (eight cmp ecx,0x0D clamp loops -> 0x1E).
    bool  smallfont_space_vwf_enabled; // 16px twin of menu_space_vwf: NOP the
                                   // `je 0x629a20` @0x629915 so space (0x20)
                                   // reads g_vwf_b16[0] instead of hardcoded
                                   // `add edi,8`. Needs 16px atlas slot 0 blank.
    int   equip_menu_shift_px;     // Shift equipment-skill "Menü" box+labels+
                                   // cursor left N px (0 = off). Default 30.
    bool  save_anywhere_enabled;   // One-byte patch at VA 0x00675A38 that
                                   // turns the pause-menu Save deny-gate
                                   // (`je`) into `jmp`, so pressing Save
                                   // outside the World Map no longer
                                   // plays the buzzer + bails. The Save
                                   // label may still render visually
                                   // greyed (separate render path), but
                                   // Confirm enters the save flow.
                                   // Default OFF — visible behavior
                                   // change; user opts in.

    bool  bgm_loop_enabled;        // config key `bgm_loop`: THE seamless-loop
                                   // fix. true = install the BGM hooks AND loop
                                   // each track to its authored point
                                   // (col2_param). false = stock loop-to-start.
                                   // This is the one intuitive on/off switch.
    bool  bgm_loop_log;            // config key `bgm_loop_log`: verbose per-track
                                   // diagnostics ([bgm-fix]/[bgm-seek] lines).
                                   // Also FORCES the hooks to install even when
                                   // bgm_loop is false, so you can hear stock
                                   // audio WITH the diagnostics for an A/B pass.
                                   // Default off (normal play stays quiet).
                                   // Default ON for backward compat — set
                                   // to false in _d3d9_hook_config.txt to
                                   // silence the `bgm:` log lines and skip
                                   // the dshow hooks entirely.

    // ── Soft-subtitle overlay for the intro FMV (MOV\ZBOF4.DAT) ──────────
    // Ported from the standalone bof4-subtitles-hook. When enabled, the
    // Present hook draws timed SRT cues over the movie. All settings live
    // in the shared config file under `subtitle_*` keys. See subtitles.h.
    bool         subtitle_enabled;        // master on/off for the overlay
    char         subtitle_srt[260];       // SRT path (rel = from BOF4.exe dir)
    char         subtitle_font[64];       // font family name (installed TTF)
    float        subtitle_font_scale;     // 1.0 = ~5% of backbuffer height
    float        subtitle_bottom_margin;  // fraction of backbuffer height
    int          subtitle_time_offset_ms; // +ve = earlier, -ve = later
    unsigned int subtitle_font_color;     // 0xAARRGGBB fill
    unsigned int subtitle_outline_color;  // 0xAARRGGBB halo
    int          subtitle_outline_px;     // halo thickness px (0 = none, max 32)
    bool         subtitle_shadow_enabled; // drop shadow on/off
    int          subtitle_shadow_offset_x;// shadow dx px (+ = right)
    int          subtitle_shadow_offset_y;// shadow dy px (+ = down)
    int          subtitle_shadow_softness;// shadow blur radius px (max 32)
    unsigned int subtitle_shadow_color;   // 0xAARRGGBB shadow

    bool  space_fallthrough;       // Six-byte NOP at VA 0x00629ACD that
                                   // disables the `je 0x629C54` jump
                                   // taking space chars (al==0x20) to the
                                   // jumptable of four hardcoded widths
                                   // (6/7/8/12 units, style-keyed). With
                                   // the jump NOPed, space falls through
                                   // to the normal table-read path that
                                   // every other glyph uses, so
                                   // vwf_config.txt's `0x20 = space : 0 : N`
                                   // controls space width everywhere
                                   // (dialog AND menus). Default ON.

    bool      jp_title;            // Render the JAPANESE title card: detour
                                   // draw_title_logo/draw_copyright and re-emit
                                   // the JP build's own primitives (6 rects, 3
                                   // CLUTs). Needs the JP DEMO2.DAT records 0..5
                                   // spliced in, or it draws Western art through
                                   // Japanese coordinates. Byte-guarded, so it
                                   // no-ops on any other build. Default OFF.

    bool      menu_rect_fix;       // Patch the title-menu label rect immediates
                                   // in BOF4.exe .text at startup (un-clip +
                                   // reposition German labels). Values from the
                                   // sidecar title_rects.txt next to BOF4.exe.
                                   // Exe on disk untouched. Default OFF.

    bool      rect_trace;          // Hook the universal textured-rect submit
                                   // (BOF4!0x502070). Logs every UI sprite's
                                   // decoded x/y/w/h + atlas u/v/clut (deduped)
                                   // so ANY clipped label can be inventoried
                                   // without per-VA hunting. F9 dumps the full
                                   // unique set to rect_inventory.txt. Default OFF.
    bool      dat_log;             // Log every DAT/CFG the engine opens
    bool      hang_watchdog;       // Dump every thread's position if Present stalls
                                   // ([dat-open] lines) — handy for finding which
                                   // file backs a given menu/area. Independent of
                                   // rect_trace so it can stay on for a clean build.
    bool      rect_fix;            // Runtime un-clip of textured-rect labels:
                                   // rewrite the width (and optionally x) of any
                                   // rect whose source (u,v) matches an entry in
                                   // the sidecar rect_widths.txt next to BOF4.exe.
                                   // Universal (works on every screen). Installs
                                   // the 0x502070 hook (shared with rect_trace).
                                   // Default OFF.
    bool      rect_tuner;          // In-game Rect Tuner: press F10 to enter a mode
                                   // that cycles (+/-) through on-screen rects
                                   // (the focused one flashes), reshapes them live
                                   // with the arrow keys, and saves overrides to
                                   // rect_tuner.txt (auto-applied like rect_fix).
                                   // Installs the 0x502070 hook. Default OFF.
    int       tuner_psx_w;         // game UI coord space width (default 320) — maps
    int       tuner_psx_h;         // rect coords to the backbuffer for the tuner's
                                   // overlay highlight boxes. Tweak if boxes are
                                   // offset from the real graphic while latched.
    bool      free_camera;         // EXPERIMENTAL (GOG only): smooth/free field-camera
                                   // rotation (Q/E) instead of 90° steps, by driving
                                   // the yaw at 0xBAD7C6. Default OFF; free_camera.cpp.
    int       free_camera_speed;   // per-tick yaw step in 12-bit units (0 = default 0x18).
    char      free_camera_off_areas[128]; // comma-separated AREA names where free-cam
                                   // is disabled (default "AREAE005" = overworld).
    char      free_camera_key_left[16];   // rotate-left key  (default "Q")
    char      free_camera_key_right[16];  // rotate-right key (default "E")
    char      free_camera_toggle_key[16]; // enable/disable toggle (default "F9")
    bool      cmdbox_asm;          // TEST: apply the German SGAMEN .ASM patches to
                                   // PSX RAM (.psxseg) — proves whether the port
                                   // executes the overlay MIPS. Default OFF.
    int       cmdbox_label_x;      // field cmd-menu label text X (data @0x95BA7C).
                                   // 0 = off/stock (0x26=38); German = 0x1D=29.
    int       cmdbox_cmdgfx_x;     // "Command" header graphic X (@0x95BA94).
                                   // 0 = off/stock; German = 0x16=22.
    bool      tex_prov_log;        // DIAGNOSTIC: hook LoadImage 0x4026E0 to log each
                                   // texture upload's VRAM rect + source DAT to
                                   // tex_provenance.log, and log texpage (0xE0-0xE7)
                                   // packets seen at 0x502070. Feeds the "which DAT
                                   // recNN" resolver work. Default OFF.
    bool      glyph_metric_log;    // DIAGNOSTIC: hook the universal per-glyph
                                   // metric/draw routine 0x006780E0; while F6 is
                                   // held, log each glyph's (caller RA, x, y,
                                   // char) to glyph_metric.log. Used to identify
                                   // which subsystem draws a label + its start-X.
                                   // Default OFF.
    // (world-map location-name label centering is a permanent, always-on fix —
    //  see label_center.cpp; no config flag.)
    bool      box_autofit;         // Auto-grow menu boxes (drawn via 0x677E30) to
                                   // fit their measured text width, so German text
                                   // that overflowed a hardcoded box just fits —
                                   // border + fill scaled by the engine, no gap, no
                                   // caller-sig matching. Only grows, never shrinks.
                                   // Default OFF (no hooks installed when off).
    int       box_autofit_pad;     // Pixels added to the measured text width when
                                   // sizing the box (room for item icons + margins).
                                   // Default 24.
    bool      box_autofit_log_all; // DIAGNOSTIC: log EVERY box draw (no dedup) as
                                   // [bw-all], capped, so a box can be identified by
                                   // real coords across frames. Default OFF.
    int       cmdbox_width;        // Field COMMAND MENU box width (Objekte/Spezial/
                                   // Ausrüstung/…). Patches the open+close animation
                                   // (clamp+step+close) so the box latches at this
                                   // width. > 0x4C, <= 0xFC, NOT a multiple of 0x10.
                                   // 0/0x4C = off (stock 76). Border + interior fill
                                   // scale together — no gap. See cmdbox_width.h.
    int       cmdbox_x;            // Field COMMAND MENU box LEFT x (stock 16). Slides
                                   // the box origin (@0x67D016) AND the selection-cursor
                                   // x-offset (@0x67D17F) left/right by the same delta so
                                   // both track together, WITHOUT touching the shared item
                                   // text table. 16 = off. Range 0..48. See cmdbox_width.h.
    int       cmdbox_cursor_w;     // Field COMMAND MENU red selection-cursor WIDTH
                                   // (@0x67D162, stock 0x47=71). Widen to match a widened
                                   // box so the cursor spans "Ausrüstung". 0 = off (stock).
    int       recon_scan;          // Steam/Enigma viability probe. 1 = read-only compare
                                   // of the decrypted runtime .text against the GOG
                                   // reference, then RETURN before installing anything
                                   // (no patches → can't trip anti-tamper). 0 = off
                                   // (normal operation). See recon_scan.h.
                                   // Range 0x20..0xA0.
    bool  censor_fix_aream031;     // Restore the NA-cut AREAM031 violence beat
                                   // at runtime (B1' PC-remap + data codecave).
                                   // GOG-only, exe untouched, reversible. Verifies
                                   // all sites + aborts on mismatch. Default OFF.
                                   // See censor_fix_aream031.h.
    bool  censor_fix_aream027;     // Restore the NA-cut AREAM027 cutscene (sailors
                                   // + Ursula/Nina at the ship) at runtime. Single
                                   // pure-SKIP divert of a 28-byte early-terminator
                                   // block the US build inserts at VA 0x922906; the
                                   // full choreography is already present in US .data
                                   // and merely gated off by that block. Independent
                                   // of censor_fix_aream031 (different scene/VAs).
                                   // GOG-only, exe untouched, verifies the 28 bytes
                                   // + aborts on mismatch. Default OFF (unverified
                                   // in-game). See censor_fix_aream031.h.
    bool  censor_fix_aread157;     // Restore the NA-cut AREAD157 Emperor-confrontation
                                   // violence (Fou-Lu decapitates the Emperor, corpse,
                                   // throne). The scene stalls at beat 0x0E because the
                                   // one actor that advances the selector 0x0E->0x0F was
                                   // cut; the baked director drives every other beat, so
                                   // the fix bumps the selector at gate 0x0090192B. TEXT
                                   // (2 Fou-Lu lines) restored separately via the DAT.
                                   // Default OFF (unverified in-game).
    bool  censor_fix_trace;        // With the fix ON, also record per-context PC
                                   // transitions in the record range; F6 dumps
                                   // censor_fix_trace.txt. Diagnostic. Default OFF.
    bool  censor_scene_trace;      // READ-ONLY scene discovery: install the 3
                                   // universal VM fetch hooks but arm NO diverts,
                                   // logging per-ctx PC changes across the WHOLE
                                   // baked-script .data region. Used to locate a
                                   // NEW scene's record + branch point (e.g.
                                   // AREAM027). F6 -> censor_scene_trace.txt.
                                   // Mutually exclusive with censor_fix_aream031.
                                   // Default OFF.
    bool  censor_probe_aread145;   // READ-ONLY diagnostic: log the AREAD145
                                   // scene-phase byte + beat selector (US or JP)
                                   // to d3d9_hook.log. Default OFF.
    bool  censor_probe_aread145_force; // US-only diagnostic: force AREAD145 scene
                                   // phase = 7 at the deleted-branch gate to test
                                   // whether the bath-scene actors then spawn.
                                   // THROWAWAY SAVE ONLY. Default OFF.

    bool  dengeki_store_unlock;    // Restore the Japan-only Dengeki Store bonus
                                   // area (shop + lottery). Holds event flags
                                   // 0x8D+0xA5 on array 0xB5672C set (= the PSX
                                   // memcard patch). Store is JAPANESE until
                                   // localized. Sig-verified; no-op on mismatch.

    bool  config_gui;              // In-game ImGui settings overlay. Press the
                                   // toggle key (default: the ^/~ key, top-left,
                                   // works on QWERTZ & QWERTY) to open a panel that
                                   // toggles these flags, remaps the Turbo-cycle
                                   // key, and live-edits VWF glyph spacing. Save
                                   // writes back to _d3d9_hook_config.txt (comments
                                   // preserved). Default ON. See config_overlay.cpp.
    int   config_gui_vk;           // VK override for the overlay toggle. 0 = use the
                                   // physical top-left key by scan code 0x29 (^ on
                                   // QWERTZ, ` / ~ on QWERTY) so it is layout-proof.
};

// Runtime counters exposed for the diagnostic loop in bof4_hooks.cpp.
#include <atomic>
extern std::atomic<uint64_t> g_present_call_count;
extern std::atomic<uint64_t> g_submit_call_count;
extern HookConfig g_cfg;

// ── In-game ImGui config overlay (config_overlay.cpp) ─────────────────────
// True while the overlay panel is open. The GetAsyncKeyState poll threads
// (rect tuner, fast-forward) check this and skip their hotkeys so keys typed
// into the panel don't also drive the game. Defined in config_overlay.cpp.
extern std::atomic<bool> g_config_overlay_active;
// Called from the device Present hook each frame (before the real Present) and
// from the swap-chain Present path. Lazily initializes ImGui on first call.
void config_overlay_on_present(struct IDirect3DDevice9* dev);
// Called from hook_Reset BEFORE g_orig_Reset so ImGui releases its
// D3DPOOL_DEFAULT device objects; they are recreated on the next frame.
void config_overlay_on_lost_device();

// ── Live VWF width poke (implemented in bof4_hooks.cpp) ────────────────────
// Write the width metrics for one glyph into the live VWF tables so spacing
// changes show immediately (no game reload). ch in 0x20..0xFF. `advance` is
// always written; `bearing` only when have_bearing is true. is16px targets the
// private 16px table (g_vwf_b16); otherwise TABLE_A + TABLE_E + g_vwf_alt.
// Returns false if the tables aren't available (unexpected ImageBase, etc.).
bool vwf_live_poke(int ch, int bearing, int advance, bool have_bearing, bool is16px);
// Read the current live width metrics for one glyph. Returns false if
// unavailable. bearing/advance are returned as signed (may be negative).
bool vwf_live_read(int ch, bool is16px, int* out_bearing, int* out_advance);

// ── Fast-forward gamepad button remap (config overlay) ────────────────────
// Poll for a single currently-pressed XInput button; write its token name into
// `out` ("A","L1","R2",...) and return true, or false if none pressed / no pad.
// Loads XInput on first call.
bool ff_pad_capture(char* out, size_t cap);
// Set the FF gamepad button from a token and re-parse so it takes effect live.
void ff_pad_set(const char* token);
// The currently configured FF gamepad button token (for display).
const char* ff_pad_current();

// Whether FF is currently toggled on (runtime state, independent of
// g_cfg.fast_forward_enabled). Meaningful only when FF is enabled.
extern bool g_ff_active;

// Called from the Present hook once per frame. Polls the hotkey, toggles
// g_ff_active on rising edge, and is a no-op when FF is disabled.
void bof4_ff_tick();

// ── MinHook-based runtime code hooks on BOF4.exe itself ───────────────────
// Called once from DllMain at DLL_PROCESS_ATTACH after the real d3d9 is
// resolved. Initializes MinHook and installs inline trampolines into
// BOF4.exe's code. See bof4_hooks.cpp for details.
void hook_bof4_install();
void hook_bof4_shutdown();

// Current field area = basename (no extension, upper-case) of the last AREA*.DAT
// the engine opened, e.g. "AREAE005". Empty until the first area loads. Used by
// free_camera to disable rotation on the overworld.
void bof4_current_area(char* out, size_t cap);

// ── Generic PNG helpers (image_io.cpp) ────────────────────────────────────
// Save an arbitrary RGBA8 buffer to `path` as a PNG. Returns true on
// success. Unlike dump_texture_png, this bypasses the dump-dedupe cache
// and always writes the file. Used by glyph_render.cpp for diagnostic
// output and by test code.
bool save_rgba8_png(const char* path, int width, int height,
                    const void* rgba);

// ── GDI-based TTF glyph renderer (glyph_render.cpp) ───────────────────────
// Rasterizes a single Unicode codepoint to an RGBA8 buffer using
// Windows GDI. The caller allocates `out_rgba` at (out_w * out_h * 4)
// bytes. Output matches BoF4's native glyph format:
//   - Visible pixels: RGB = glyph_color scaled by anti-aliased intensity,
//     Alpha = 255 (fully opaque).
//   - Fully transparent background: RGBA = 0,0,0,0.
//
// `font_name` must name an installed system font (e.g. "Arial",
// "Consolas"). `pixel_height` is the requested cell height in pixels;
// the actual glyph may be smaller. `rgb_color` is a 0x00RRGGBB value —
// e.g. 0xFFFFD4 for creamy white, 0xFFFFFF for pure white.
bool gdi_render_glyph(wchar_t ch, int pixel_height, const char* font_name,
                      uint32_t rgb_color,
                      uint8_t* out_rgba, int out_w, int out_h);

// ── ttf_override.txt + glyph_map.txt ──────────────────────────────────────
// Populated from disk on first use. `try_inject_texture` consults both
// before the generic tex_inject/<crc>.png path — see image_io.cpp.

// Resolve a BoF4 char code to a Unicode codepoint using the full lookup
// chain (char_map.txt → built-in letter table). Returns 0 if unmapped.
// Thread-safe (locks internally). Used by the SetTexture swap path.

// Render a TTF glyph for `ch` into the provided 32x32x4 RGBA buffer.
// Returns true on success. Thread-safe. Uses the cached super-sampled
// renderer from image_io.cpp.
