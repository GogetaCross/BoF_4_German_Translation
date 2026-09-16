#pragma once
// TEMPORARY DIAGNOSTIC — see name_slot_bp.cpp. Hardware write-watch on the
// [07:00] item-name scratch slot (0x00B56EBF) to locate the code that fills it
// with a 14-char-truncated item name. Gated by config `name_slot_bp`.

void name_slot_bp_install();
void name_slot_bp_tick();   // call once per frame (re-arms periodically)
