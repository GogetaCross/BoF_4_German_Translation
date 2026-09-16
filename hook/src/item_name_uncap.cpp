// Test patch — see item_name_uncap.h for the rationale.

#include "hooks.h"
#include "hook_health.h"
#include "item_name_uncap.h"

#include <cstdint>
#include <cstdio>
#include <windows.h>

// Both patch bytes are the imm8 `0x0D` (=13, PC char budget). A bare
// 0x0D is far too common to scan for, so each site is located by a
// SIGNATURE over the surrounding instruction skeleton (the jmp rel32
// after the mov is wildcarded). On the GOG-updated / recompiled EXE the
// cap is already lifted to 0xFF and these signatures do NOT match — that
// build takes the gog_already_lifted() no-op path below, so a
// LIVE-only-unique signature is exactly what we want.
//
//   site1 (TBL_F38[9]):  C6 44 24 12 0D  mov byte [esp+0x12], 0x0D
//   site2 (TBL_4D4[9]):  C6 42 07 0D     mov byte [edx+7],    0x0D
static constexpr uint8_t   EXPECTED_ORIG = 0x0D;       // = 13 (PC base)

static const uint8_t SIG1[] = {
    0xC6, 0x44, 0x24, 0x12, 0x0D, 0xE9, 0x00, 0x00, 0x00, 0x00,
    0x8A, 0x56, 0x01, 0x46, 0x88, 0x54, 0x24, 0x14,
};
static const char        SIG1_MASK[]  = "xxxxxx????xxxxxxxx";
static constexpr ptrdiff_t SIG1_OFF   = 4;
static const uint8_t SIG2[] = {
    0xC6, 0x42, 0x07, 0x0D, 0xE9, 0x00, 0x00, 0x00, 0x00,
    0xC6, 0x40, 0x01, 0x00, 0xA1,
};
static const char        SIG2_MASK[]  = "xxxxx????xxxxx";
static constexpr ptrdiff_t SIG2_OFF   = 3;

// Documented fallbacks (live BOF4.exe) — used only if a signature scan
// fails on a build where the cap is NOT already lifted.
static constexpr uintptr_t FALLBACK_SITE_1 = 0x00527D5B;
static constexpr uintptr_t FALLBACK_SITE_2 = 0x00528328;

static bool patch_byte(uintptr_t va, uint8_t expected, uint8_t newv,
                       const char* tag) {
    char feature_tag[40];
    snprintf(feature_tag, sizeof(feature_tag), "item_name_uncap.%s", tag);
    if (!health_check_bytes(feature_tag, va, &expected, 1)) {
        return false;
    }
    void* p = (void*)va;
    DWORD old_prot = 0;
    if (!VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &old_prot)) {
        hook_log("item_name_uncap: %s VirtualProtect(0x%08X) failed: %lu\n",
                 tag, (unsigned)va, GetLastError());
        return false;
    }
    *(volatile uint8_t*)p = newv;
    DWORD ignore = 0;
    VirtualProtect(p, 1, old_prot, &ignore);
    hook_log("item_name_uncap: %s 0x%08X: 0x%02X -> 0x%02X (max char count "
             "%u -> %u)\n", tag, (unsigned)va, expected, newv,
             (unsigned)expected, (unsigned)newv);
    return true;
}

// Detect the GOG-updated BOF4.exe shape at site2. The post-update
// code is `mov word [ecx+eax*1], 0x00FF` whose 6-byte encoding is
// `66 C7 04 01 FF 00`. If we see this exact prefix at PATCH_SITE_2-4
// the cap has already been lifted to 0xFF (255) by Squaresoft/GOG and
// our 8-bit patch would only do harm by *reducing* the budget. Site1
// is similarly refactored beyond recognition. Return true if detected.
static bool gog_already_lifted() {
    constexpr uintptr_t SCAN_VA = FALLBACK_SITE_2 - 4;
    constexpr uint8_t EXPECTED_NEW[6] = {
        0x66, 0xC7, 0x04, 0x01, 0xFF, 0x00,
    };
    uint8_t cur[6];
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)SCAN_VA, cur,
                           sizeof(cur), &got) || got != sizeof(cur)) {
        return false;
    }
    return memcmp(cur, EXPECTED_NEW, sizeof(EXPECTED_NEW)) == 0;
}

void item_name_uncap_install() {
    if (!g_cfg.item_name_uncap_enabled) {
        hook_log("item_name_uncap: disabled in config — 12-char limit "
                 "stays in effect\n");
        health_record_disabled("item_name_uncap", "config off");
        return;
    }
    if (gog_already_lifted()) {
        hook_log("item_name_uncap: GOG-updated BOF4.exe — cap already "
                 "lifted to 0xFF by Squaresoft, our patch would REDUCE "
                 "the budget; skipping (config flag ignored on this EXE)\n");
        health_record_disabled("item_name_uncap",
                               "no-op on GOG-updated EXE (already 0xFF)");
        return;
    }
    int v = g_cfg.item_name_uncap_value;
    if (v < 1 || v > 255) {
        hook_log("item_name_uncap: item_name_uncap_value=%d out of range "
                 "(1..255), using 20\n", v);
        v = 20;
    }
    uint8_t new_limit = (uint8_t)v;

    uintptr_t site1 = resolve_sig("item_name_uncap.site1", SIG1, SIG1_MASK,
                                  sizeof(SIG1), SIG1_OFF);
    if (!site1) site1 = FALLBACK_SITE_1;
    uintptr_t site2 = resolve_sig("item_name_uncap.site2", SIG2, SIG2_MASK,
                                  sizeof(SIG2), SIG2_OFF);
    if (!site2) site2 = FALLBACK_SITE_2;

    bool ok1 = patch_byte(site1, EXPECTED_ORIG, new_limit, "site1");
    bool ok2 = patch_byte(site2, EXPECTED_ORIG, new_limit, "site2");
    hook_log("item_name_uncap: install %s (site1=%s site2=%s, budget=%d, "
             "visible chars max=%d)\n",
             (ok1 && ok2) ? "complete" : "PARTIAL/FAILED",
             ok1 ? "ok" : "fail",
             ok2 ? "ok" : "fail",
             v, v - 1);
    char note[80];
    snprintf(note, sizeof(note),
             "site1=0x%08X %s, site2=0x%08X %s, budget=%d",
             (unsigned)site1, ok1 ? "ok" : "FAIL",
             (unsigned)site2, ok2 ? "ok" : "FAIL", v);
    health_record("item_name_uncap", ok1 && ok2, note);
}
