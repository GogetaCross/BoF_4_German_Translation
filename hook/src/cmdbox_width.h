// Runtime patches for the field COMMAND MENU (Objekte / Spezial / Ausrüstung /
// Status / Wechseln), drawn by BOF4!0x0067D000, so long German labels fit.
//
// WIDTH (cmdbox_width)
//   The box width is [state+6] (state = [0x00B573F0]) — a SHARED menu-transition
//   counter incremented only by 0x67D000 (start 0x0C, +0x10/frame, latch when
//   == 0x4C) and read by sibling sub-box drawers to sync. There are exactly 4
//   "fully open == 0x4C" comparisons across the cluster:
//       0x67D03B  open latch          (0x67D000)
//       0x67D1A0  close "fully open?" (0x67D000)
//       0x67DB49  close "fully open?" (0x67DBxx sub-box)
//       0x67DB60  content-draw gate   (0x67DBxx sub-box)
//   Changing ALL FOUR to a new fully-open value W keeps every open/close/gate
//   consistent (the freeze at W=0x54 was from patching only 2 of them — the
//   0x67DBxx close then decremented past 0 forever). W MUST stay on the
//   0x0C+0x10 grid (low nibble 0xC: 0x5C=92, 0x6C=108, 0x7C=124) so the +0x10
//   open latches exactly and the stock close-special (0x40, left untouched)
//   still drains to 0. Border + interior fill scale together — no gap.
//   0 or 0x4C = off (stock 76).
//
// SHIFT (cmdbox_x)
//   Slides the whole command menu left/right so a widened box grows into the
//   left margin instead of overlapping the neighbour on the right. Patches the
//   box origin (push 0x10 @0x67D016) and all 7 item x-coords + header anchor
//   (@0x95BA7C) by the same delta. Stock left x = 16; set lower to move left.
//   16 = off. Range 0..48.
#pragma once

void cmdbox_width_install();
