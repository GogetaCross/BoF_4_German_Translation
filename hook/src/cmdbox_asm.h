// cmdbox_asm.h — see cmdbox_asm.cpp. Test harness that applies the German
// SGAMEN .ASM command-box patches to the port's emulated PSX RAM, to determine
// whether the overlay MIPS is executed (dynarec) or dead (pure AOT). Gated on
// g_cfg.cmdbox_asm; no-op otherwise.
#pragma once

void cmdbox_asm_install();
void cmdbox_asm_shutdown();
