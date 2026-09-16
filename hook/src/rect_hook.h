// Universal textured-rect inventory + un-clip. Hooks BOF4!0x502070, the submit
// routine every UI sprite passes through. See rect_hook.cpp.
#pragma once
void rect_hook_install();

// Per-frame tick for the inventory's rect-lifetime tracking (first/last frame +
// hit count) — call once per presented frame from the Present hook so the F8/F11
// inventory dump can flag SHORT-LIVED rects (the millisecond-flashing graphics).
void rect_hook_frame_tick();

// Formats this frame's box tiles + the text inside each (grouped by draw-site
// caller) for the F10 HUD, then clears the buffers. Used by the Rect Tuner HUD.
extern "C" int rect_box_text_hud(char* out, int cap);
