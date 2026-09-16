// Texture provenance (DIAGNOSTIC phase). Goal: eventually show, in the Rect
// Tuner HUD, which DAT + recNN a focused rect's texture came from — the same
// recNN the graphics tool (bof4_gui.py) edits. This first cut only LOGS the raw
// facts we need to build the resolver, so we can confirm the loader/upload
// addresses actually fire on the live exe and learn how the texture page is set:
//
//   * hooks LoadImage 0x4026E0(rect, src) — the op1 VRAM upload — and logs each
//     upload's VRAM rectangle (x,y,w,h) + the DAT that was most recently loaded,
//     to tex_provenance.log. That's the (VRAM region -> DAT) ground truth.
//   * the 0x502070 rect hook (rect_hook.cpp) additionally logs any texpage /
//     env packets (GP0 0xE0-0xE7) it sees, so we learn whether the rect's
//     texture-page base is visible in that queue (needed to map u/v -> VRAM).
//
// Gated entirely on the `tex_prov_log` config flag. Zero cost when off.
#pragma once

#include <cstddef>

void tex_prov_install();   // install the LoadImage upload tracker (rect_tuner | tex_prov_log)

// Source attribution for a focused rect whose atlas V-coord is `v`: fills `out`
// with the resident DAT recNN candidates whose VRAM y-band matches v (both
// texpage-Y halves), most-recently-uploaded first. Returns false if nothing is
// known yet (no map / no uploads). Called by the Rect Tuner HUD.
bool tex_prov_resolve(int v, char* out, size_t n);
