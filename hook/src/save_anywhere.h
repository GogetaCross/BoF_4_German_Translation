// One-byte runtime patch removing the pause-menu "Save" item's
// area-restriction. Without it, pressing Confirm on Save outside the
// World Map plays the denied-buzzer SFX (0x205) and bails. The patch
// converts the deny-gate's `je 0x675a58` into `jmp 0x675a58`, so the
// Save Confirm path always falls through to the normal handler.
//
// Patch site in BOF4.exe:
//   VA 0x00675A38, byte:  0x74 (je rel8)  ->  0xEB (jmp rel8)
//   The 8-bit displacement (0x1E) is unchanged — same target either way.
//
// Surrounding instructions for reference:
//   0x00675A26  mov   ax, [0xbae300]       ; current area-ID
//   0x00675A2C  push  eax
//   0x00675A2D  call  0x00639300           ; "is_save_allowed_area?"
//   0x00675A35  cmp   eax, 1
//   0x00675A38  je    0x675a58             ; PATCH SITE — make this unconditional
//   0x00675A3A  cmp   [0xb507fc], 6        ; cursor on Save?
//   0x00675A41  jne   0x675a58
//   0x00675A43  push  0x205                ; deny buzzer
//   0x00675A48  call  0x0066d250
//   0x00675A57  ret
//   0x00675A58: ...normal pause-menu Confirm dispatch
//
// Gated by `save_anywhere` in _d3d9_hook_config.txt. Default OFF — the
// patch is a behavior change visible to the player, so the user opts in.
#pragma once

void save_anywhere_install();
