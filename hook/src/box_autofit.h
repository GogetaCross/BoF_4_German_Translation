// Auto-fit menu boxes to their (translated) text width.
//
// Menu boxes are drawn by BOF4!0x00677E30, whose 3rd arg is the box WIDTH —
// the engine tiles the whole 9-slice (border + fill) to that width, so a box
// renders correctly at ANY width you pass it (that's how the game natively
// draws boxes of different sizes). The width is normally a HARDCODED constant,
// while the item text is measured by BOF4!0x00629E90 right BEFORE the box is
// drawn (the measure is only used to center the text).
//
// This module captures that measured width and, at the box draw, grows the box
// to max(original, measured + padding) — so boxes German text overflows just
// fit themselves, border + fill together, no gap. Only GROWS (never shrinks),
// driven by the engine's own measurement + static hooks → stable every session,
// no caller-signature matching. Opt-in via config `box_autofit`; when off, NO
// hooks are installed (the DLL behaves identically to before). See box_autofit.cpp.

#pragma once

// Install the box-drawer hook (0x677E30) and load box_widths.txt. No-op unless
// box_autofit is enabled. `padding` is unused now (kept for call-site compat).
// Call after MinHook init.
void box_autofit_install(bool enabled, int padding);

// Re-read box_widths.txt (bound to F5 in-game).
void box_autofit_reload();

// Format the current frame's on-screen boxes (caller/pos/width) for the F10 HUD,
// clearing the frame buffer. Returns bytes written. Safe to call when disabled
// (writes an empty/"(none)" line). Used by the Rect Tuner HUD.
extern "C" int box_autofit_hud_text(char* out, int cap);

// ── In-game BOX TUNER ──────────────────────────────────────────────────────
// The F10 tuner's "box mode". This is NOT the rect tuner: it does not touch GP0
// packets at all. It rewrites the SAME width/x/y args that a rect_widths.txt
// `caller=... width=N` rule rewrites, so the whole 9-slice — border, fill and
// the semi-transparent dim — retiles, and the selection-frame drawer (0x6786D0)
// is a first-class citizen instead of being invisible to a packet-level editor.
// Save writes the rule back into rect_widths.txt, comments and all.
//
// THREADING: every function below except the *_hud_text/geom readers must be
// called on the RENDER thread (rect_tuner_on_present does exactly that), which
// is also the thread af_box_hook runs on — so the tune state needs no locks.

// Per-frame: snapshot the boxes drawn this frame for cycling, reset the
// accumulator. No-op when box_autofit is disabled. Call from Present.
void box_autofit_on_present();

void box_tuner_set_active(bool on);
bool box_tuner_active();
void box_tuner_cycle(int dir);                  // step the focus through the frame's boxes
void box_tuner_nudge(int dw, int dx, int dy);   // live width / position delta
bool box_tuner_save();                          // write the rule into rect_widths.txt + reload
bool box_tuner_clear();                         // comment the focused caller's rule out + reload
int  box_tuner_hud_text(char* out, int cap);    // HUD panel text for the focused box
// Effective on-screen geometry of the focused box, for the overlay caliper.
// x/w are the values actually passed to the drawer; ow is the stock width.
bool box_tuner_focus_geom(int* x, int* y, int* w, int* ow);
