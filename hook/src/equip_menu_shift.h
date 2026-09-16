#pragma once
// Shift the equipment-skill "Menü" box + labels + title + divider + selection
// cursor left by config `equip_menu_shift` pixels (default 30, 0 = off). Widths
// are handled by box_autofit; this only moves X. See the .cpp for the sites.

void equip_menu_shift_install();
// Re-read `equip_menu_shift` from _d3d9_hook_config.txt and re-apply the 7-byte
// patch from the stored originals — lets the whole-menu shift be tuned live (F5).
void equip_menu_shift_reload();
