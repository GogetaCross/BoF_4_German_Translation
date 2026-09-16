// Title-menu label rect fix — widen (un-clip) and reposition the German menu
// labels by patching the hardcoded PSX GP0(0x64) packet-builder immediates in
// BOF4.exe .text at runtime. See menu_rect_fix.cpp. Gated by g_cfg.menu_rect_fix.
// The exe on disk is never modified. Driven by an editable sidecar
// (title_rects.txt next to BOF4.exe), auto-created with the original values.
#pragma once
void menu_rect_fix_apply();
