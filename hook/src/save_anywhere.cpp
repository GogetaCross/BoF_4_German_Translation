// One-byte runtime patch unlocking the pause-menu Save item outside the
// World Map. See save_anywhere.h for the gate's full disasm.

#include "hooks.h"
#include "hook_health.h"
#include "save_anywhere.h"

#include <cstdint>
#include <cstdio>
#include <windows.h>

// The Save gate is now located by a byte SIGNATURE rather than a
// hardcoded VA, so the patch survives EXE recompiles (GOG updates,
// Steam/Enigma). The signature is the cmp/je gate skeleton:
//
//   83 F8 01           cmp  eax, 1
//   74 1E              je   +0x1E          <- byte we flip (je->jmp)
//   80 3D ?? ?? ?? ??  cmp  byte [global], ..   (global addr wildcarded)
//   06                 (imm)
//   75 15              jne  +0x15
//   68 05 02 00 00     push 0x205
//
// Verified UNIQUE in both the live build (site 0x00675A38) and the
// 2026-05 GOG recompile (site 0x006758E8) — the sig relocates onto the
// exact address the old hardcoded fallbacks documented.
static const uint8_t SIG_PATTERN[] = {
    0x83, 0xF8, 0x01, 0x74, 0x1E, 0x80, 0x3D, 0x00, 0x00, 0x00, 0x00,
    0x06, 0x75, 0x15, 0x68, 0x05, 0x02, 0x00, 0x00,
};
static const char SIG_MASK[]      = "xxxxxxx????xxxxxxxx";
static constexpr ptrdiff_t SIG_PATCH_OFF = 3;       // offset of the je

// Documented fallbacks (only used if the signature scan ever fails on
// a known build — that would be a signature bug, not a ship state):
//   live BOF4.exe   0x00675A38
//   2026-05 GOG     0x006758E8
static constexpr uintptr_t FALLBACK_SITE  = 0x00675A38;
static constexpr uint8_t   ORIGINAL_BYTE  = 0x74;   // je  rel8
static constexpr uint8_t   PATCHED_BYTE   = 0xEB;   // jmp rel8

static bool patch_byte(uintptr_t va, uint8_t expected, uint8_t newv) {
    // Pre-flight byte check via the health helper. On mismatch it
    // dumps context and scans .text for the original byte (cheap when
    // the pattern is one byte; the helper falls back to listing all
    // matches if the byte is too common to disambiguate).
    if (!health_check_bytes("save_anywhere", va, &expected, 1)) {
        return false;
    }
    void* p = (void*)va;
    DWORD old_prot = 0;
    if (!VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &old_prot)) {
        hook_log("save_anywhere: VirtualProtect(0x%08X) failed: %lu\n",
                 (unsigned)va, GetLastError());
        return false;
    }
    *(volatile uint8_t*)p = newv;
    DWORD ignore = 0;
    VirtualProtect(p, 1, old_prot, &ignore);
    hook_log("save_anywhere: 0x%08X: 0x%02X -> 0x%02X (je -> jmp, deny "
             "branch is now unreachable for the Save item)\n",
             (unsigned)va, expected, newv);
    return true;
}

void save_anywhere_install() {
    if (!g_cfg.save_anywhere_enabled) {
        hook_log("save_anywhere: disabled in config — pause-menu Save "
                 "still requires World Map / save-allowed area\n");
        health_record_disabled("save_anywhere", "config off");
        return;
    }

    // Locate the gate by signature; fall back to the documented VA only
    // if the scan fails (would indicate a broken signature on a known
    // build, not a normal ship path).
    uintptr_t site = resolve_sig("save_anywhere", SIG_PATTERN, SIG_MASK,
                                 sizeof(SIG_PATTERN), SIG_PATCH_OFF);
    if (!site) {
        hook_log("save_anywhere: signature scan failed — falling back to "
                 "hardcoded VA 0x%08X\n", (unsigned)FALLBACK_SITE);
        site = FALLBACK_SITE;
    }

    bool ok = patch_byte(site, ORIGINAL_BYTE, PATCHED_BYTE);
    hook_log("save_anywhere: install %s\n", ok ? "complete" : "FAILED");
    char note[64];
    snprintf(note, sizeof(note), "VA 0x%08X (je->jmp)", (unsigned)site);
    health_record("save_anywhere", ok, note);
}
