// dengeki_unlock — restore access to the Japan-only Dengeki Store bonus area
// (blue-Manillo shop + orange-Manillo lottery) on the PC port.
//
// The store's area data ships intact in the PC port (AREAD068 / AREAS052, in
// Japanese) but is unreachable because two persistent event flags are never set:
// the code path that would set them (0x0066E16F) is gated behind a Dengeki-enable
// condition ([0xB4EEE0]) that the cut PC/NA build never satisfies. The store's
// per-area handler (0x00438B60) and a second world node (0x004B6B60) therefore
// always take their "disable" branch and remove the store's entrance objects.
//
// The sanctioned PSX restoration hack (Navarchos/Ratty/FlamePurge) unlocks the
// store by setting exactly two save event-flag bits. Reverse-engineered, those are
// flag index 0x8D and 0xA5 on the persistent event-flag array 0x00B5672C
// (test_flag = 0x00655AD0, set_flag = 0x00655A80). Both are bit 5 of their byte,
// matching the PSX memcard patch's two `|= 0x20` writes at save offsets
// 0x10BD / 0x10C0 exactly.
//
// This module locates the flags build-independently by signature-scanning the
// dual set-site (which embeds both indices AND the array address), then keeps the
// two bits set from a light background thread so the unlock survives each
// save/load (which reloads the array from the — locked — saved state). Setting
// these flags only ENABLES the bonus area; it cannot corrupt progression. If the
// signature is not found (wrong/updated build) the module no-ops.
//
// Config key: `dengeki_store_unlock`.

#pragma once

void dengeki_unlock_install();
void dengeki_unlock_shutdown();
