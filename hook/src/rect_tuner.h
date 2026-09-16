// In-game "Rect Tuner": select a UI rectangle by cycling (+/-, it flashes),
// reshape it live with the arrow keys, and press Enter to persist the override
// to rect_tuner.txt. Built on top of the same BOF4!0x502070 submit hook that
// rect_hook.cpp already uses — see rect_tuner.cpp. Enabled by the `rect_tuner`
// config flag. Zero cost when the flag is off.
#pragma once
#include <cstddef>
#include <cstdint>

struct IDirect3DDevice9;
struct IDirect3DSwapChain9;

// One decoded textured rect, handed from rect_hook.cpp's h_submit to the tuner.
// Values are the ORIGINAL (pre-override) decode; the tuner re-reads the live
// packet itself to stack its delta on top of any rect_fix override.
struct TunerRect {
    int      x, y, w, h;      // original on-screen rect
    unsigned u, v, clut, cmd; // atlas source + GP0 command
    unsigned caller0, caller1;// nearest BOF4.exe caller EIPs (draw-site id)
    unsigned sig;             // hash of the whole caller chain — distinguishes
                              // WHICH box/menu drew the rect (granular selection)
};

void rect_tuner_install();                              // start input thread (if enabled)
void rect_tuner_on_submit(const TunerRect& r);          // per-rect, in h_submit
void rect_tuner_on_present(IDirect3DDevice9* dev,        // per-frame, in Present hooks
                           IDirect3DSwapChain9* sc);
void rect_tuner_on_lost_device();                        // release D3DPOOL_DEFAULT HUD tex (Reset)

// LATCH: freeze the tuner's captured rect list so a TRANSIENT graphic (on screen
// for only a fraction of a second — fishing catch/hook, etc.) stays selectable and
// tunable after it's gone. Toggled from bof4_hooks' hotkey thread; the pause
// hotkey pairs this with a plain scale-0 time-freeze so the real graphic also
// stays visible as the backdrop. Touches NO game memory.
void rect_tuner_set_latch(bool on);
bool rect_tuner_is_latched();

// Absolute path of rect_tuner.txt (next to BOF4.exe). Shared with rect_hook.cpp
// so parse_overrides can load the tuner's saved lines alongside rect_widths.txt.
void rect_tuner_sidecar_path(char* out, size_t n);

// ── Live key-binding access for the in-game config overlay ─────────────────
// The poll thread reads g_keys every iteration, so updating a binding takes
// effect immediately. Bindings are addressed by index (0..count-1); each has a
// stable config-file action name ("toggle","move_left",...) and a VK code.
int         rect_tuner_key_count();
const char* rect_tuner_key_action(int i);   // config-file action name, or "" if OOB
int         rect_tuner_key_vk(int i);        // current VK, or 0 if OOB
void        rect_tuner_key_set(int i, int vk);   // update binding live (no persist)
// Rewrite rect_tuner_keys.txt from the current bindings, preserving comments and
// the file's key order. Returns true on success.
bool        rect_tuner_keys_save();
// Convert a VK code to a token vk_from_token() understands (for display + save).
// Writes into out (cap bytes); falls back to "0xNN".
void        rect_tuner_vk_name(int vk, char* out, size_t cap);
