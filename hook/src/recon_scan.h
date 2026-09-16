#pragma once
// Read-only Steam/Enigma viability probe. When recon_scan=1, hook_bof4_install
// runs this INSTEAD of installing any hooks/patches: it only READS the decrypted
// runtime .text and compares it to the GOG BOF4.exe reference (recon_data.h),
// so it cannot trip Enigma's anti-tamper. Results go to d3d9_hook.log.
void recon_scan_run();
