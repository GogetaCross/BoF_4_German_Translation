#pragma once
// Lifts the 14-char cap on item names inserted into message text via [07:00]
// (e.g. "Kaputtes Schwert kann nicht gehandelt werden!"). Patches the eight
// length-clamp loops (cmp ecx,0x0D) that feed the copier 0x629D50. See the .cpp.
// Gated by config `item_name_msg_uncap`.

void item_name_msg_uncap_install();
