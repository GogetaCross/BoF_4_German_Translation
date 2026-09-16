// Two-byte runtime patch lifting the 12-char limit on item names
// substituted into dialog via the [09:XX:YY] opcode (e.g. "Königsschwert"
// → "Königsschwer"). The PSX equivalent (SLUS_01324.ASM) bumps the same
// constant at two paired sites; we mirror that on PC.
//
// Patch sites in BOF4.exe — the imm8 byte of two `mov BYTE PTR [...], 0x0D`
// instructions, one inside each of the two [09:XX:YY] dispatch table
// handlers. The handler stores 0x0D (= 13) as the "remaining char count"
// budget for the item-name substitution; the typewriter loop decrements
// it per char and stops at 0, yielding the observed 12 visible chars.
//
//   TBL_F38[9] handler at 0x00527D3E ends with `mov [esp+0x12], 0x0D`
//     → VA 0x00527D5B is the imm8 byte
//   TBL_4D4[9] handler at 0x005282EA ends with `mov [edx+7],   0x0D`
//     → VA 0x00528328 is the imm8 byte
//
// PSX bumps 0x0C → 0x13 (= +7). PC base is 0x0D (one off from PSX's 0x0C);
// matching the +7 gain gives 0x14 (= 20 budget = 19 visible chars).
//
// Gated by `item_name_uncap` in _d3d9_hook_config.txt. Default ON.
#pragma once

void item_name_uncap_install();
