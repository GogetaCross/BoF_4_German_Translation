// See cmdbox_width.h. Static patches for the field command menu: WIDTH (the 4
// shared "fully open" comparisons) and X-SHIFT (box origin + item coord table).
// Mirrors save_anywhere.cpp's VirtualProtect + health_check style.

#include "hooks.h"
#include "hook_health.h"
#include "cmdbox_width.h"

#include <cstdint>
#include <cstdio>
#include <windows.h>

// The field menu "box open" animation is a counter at [state+6]: it starts 0x0C
// and ticks +0x10 per frame until it latches at fully-open == 0x4C; the box is
// drawn at width == the counter, so raising the latch widens the box. THREE
// sibling handlers (the main command box 0x67D000, plus the Time and Zenny
// boxes) each run this same open-latch / close-drain logic against 0x4C, giving
// SIX "== 0x4C" counter compares. ALL SIX must move to the new width W together
// — this mirrors the German team's SGAMEN hack, which sets the box size at all
// its width sites. (Patching only the 2 main-box sites left the other four
// waiting for a 0x4C the counter never reaches → Time/Zenny never open and the
// menu input loop hangs = the freeze.) W MUST stay on the 0x0C+0x10 grid (low
// nibble 0xC: 0x5C=92, 0x6C=108, 0x7C=124) so +0x10 latches exactly and the
// stock close-special (0x40) still drains to 0.
//
// Every site is located by a byte SIGNATURE (resolve_sig) so it survives EXE
// recompiles; all verified UNIQUE. Hardcoded VAs are documented fallbacks.
static constexpr uint8_t ORIG_FULLOPEN = 0x4C;
static constexpr uint8_t ORIG_BOX_X    = 0x10;
static constexpr uint8_t ORIG_CUR_SUBX = 0x14;
static constexpr uint8_t ORIG_CUR_W    = 0x47;

// --- the six "fully-open == 0x4C" counter compares (imm8 to bump to W) -------
struct FoSite { const char* name; const uint8_t* sig; const char* mask;
                size_t len; ptrdiff_t off; uintptr_t fb; };
// main box, open latch:  3C 4C 74 07 04 10 88 46 06 5E C3      -> imm +1
static const uint8_t FO0[]={0x3C,0x4C,0x74,0x07,0x04,0x10,0x88,0x46,0x06,0x5E,0xC3};
// main box, close:       3C 4C 75 06 C6 46 06 40 EB 09         -> imm +1
static const uint8_t FO1[]={0x3C,0x4C,0x75,0x06,0xC6,0x46,0x06,0x40,0xEB,0x09};
// box#2 (Time) drain:    8A 48 06 74 19 80 F9 4C               -> imm +7
static const uint8_t FO2[]={0x8A,0x48,0x06,0x74,0x19,0x80,0xF9,0x4C};
// box#2 (Time) latch:    FE 40 03 C3 80 F9 4C                  -> imm +6
static const uint8_t FO3[]={0xFE,0x40,0x03,0xC3,0x80,0xF9,0x4C};
// box#3 (Zenny) drain:   8A 41 06 74 17 3C 4C                  -> imm +6
static const uint8_t FO4[]={0x8A,0x41,0x06,0x74,0x17,0x3C,0x4C};
// box#3 (Zenny) latch:   FE 41 03 C3 3C 4C                     -> imm +5
static const uint8_t FO5[]={0xFE,0x41,0x03,0xC3,0x3C,0x4C};
static const FoSite FO_SITES[] = {
    {"cmdbox.fullopen0", FO0, "xxxxxxxxxxx", sizeof(FO0), 1, 0x0067D03B},
    {"cmdbox.fullopen1", FO1, "xxxxxxxxxx",  sizeof(FO1), 1, 0x0067D1A0},
    {"cmdbox.fullopen2", FO2, "xxxxxxxx",    sizeof(FO2), 7, 0x0067D9FB},
    {"cmdbox.fullopen3", FO3, "xxxxxxx",     sizeof(FO3), 6, 0x0067DA12},
    {"cmdbox.fullopen4", FO4, "xxxxxxx",     sizeof(FO4), 6, 0x0067DB48},
    {"cmdbox.fullopen5", FO5, "xxxxxx",      sizeof(FO5), 5, 0x0067DB5F},
};
static constexpr int NUM_FULLOPEN = (int)(sizeof(FO_SITES)/sizeof(FO_SITES[0]));

// --- signatures for the other single-site patches ---------------------------
// box origin x: 6A 7A 50 6A 2C 6A 10 E8 ?? ?? ?? ??            -> imm at +6
static const uint8_t  SIG_BX[]   = {0x6A,0x7A,0x50,0x6A,0x2C,0x6A,0x10,0xE8,0,0,0,0};
static const char     SIG_BX_M[] = "xxxxxxxx????";
// cursor width: 6A 47 0F BE 46 0C C1 E0 02 66 8B 88            -> imm at +1
static const uint8_t  SIG_CW[]   = {0x6A,0x47,0x0F,0xBE,0x46,0x0C,0xC1,0xE0,0x02,0x66,0x8B,0x88};
static const char     SIG_CW_M[] = "xxxxxxxxxxxx";
// cursor sub-x: 66 83 EA 14 51 52 E8 ?? ?? ?? ?? 66 8B 46 12   -> imm at +3
static const uint8_t  SIG_CSX[]  = {0x66,0x83,0xEA,0x14,0x51,0x52,0xE8,0,0,0,0,0x66,0x8B,0x46,0x12};
static const char     SIG_CSX_M[]= "xxxxxxx????xxxx";

// Documented fallbacks (live BOF4.exe).
static constexpr uintptr_t FB_BOX_X       = 0x0067D016;
static constexpr uintptr_t FB_CURSOR_SUBX = 0x0067D17F;
static constexpr uintptr_t FB_CURSOR_W    = 0x0067D162;

// Resolve each site by signature (fallback to hardcoded VA on scan miss).
static uintptr_t rs(const char* n, const uint8_t* p, const char* m, size_t l,
                    ptrdiff_t off, uintptr_t fb) {
    uintptr_t va = resolve_sig(n, p, m, l, off);
    return va ? va : fb;
}
static uintptr_t VA_FULLOPEN(int i) {
    const FoSite& s = FO_SITES[i];
    return rs(s.name, s.sig, s.mask, s.len, s.off, s.fb);
}
static uintptr_t VA_BOX_X()       { return rs("cmdbox.box_x",   SIG_BX,  SIG_BX_M,  sizeof(SIG_BX),  6, FB_BOX_X); }
static uintptr_t VA_CURSOR_W()    { return rs("cmdbox.cursor_w",SIG_CW,  SIG_CW_M,  sizeof(SIG_CW),  1, FB_CURSOR_W); }
static uintptr_t VA_CURSOR_SUBX() { return rs("cmdbox.cursor_sx",SIG_CSX,SIG_CSX_M, sizeof(SIG_CSX), 3, FB_CURSOR_SUBX); }

static bool poke8(uintptr_t va, uint8_t v) {
    void* p = (void*)va; DWORD op = 0;
    if (!VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &op)) {
        hook_log("cmdbox: VirtualProtect(0x%08X) failed: %lu\n", (unsigned)va,
                 GetLastError());
        return false;
    }
    *(volatile uint8_t*)p = v;
    DWORD ig = 0; VirtualProtect(p, 1, op, &ig);
    return true;
}
static void install_width() {
    int W = g_cfg.cmdbox_width;
    if (W <= 0 || W == ORIG_FULLOPEN) {
        hook_log("cmdbox_width: disabled (=%d) — box stays stock width 0x%02X\n",
                 W, ORIG_FULLOPEN);
        health_record_disabled("cmdbox_width", "config off");
        return;
    }
    // Grid-only: low nibble must be 0xC so +0x10 open latches exactly and the
    // stock close-special (0x40) still drains to 0. Off-grid needs step/special
    // surgery that hits the 0x67DBxx gates → freeze; not worth it.
    if (W > 0xFC || W <= ORIG_FULLOPEN || (W & 0x0F) != 0x0C) {
        hook_log("cmdbox_width: IGNORED %d (0x%02X) — must be > 0x4C, <= 0xFC, "
                 "low nibble 0xC (0x5C=92, 0x6C=108, 0x7C=124)\n", W, W);
        health_record("cmdbox_width", false, "invalid value");
        return;
    }
    // Resolve sites by signature, then pre-flight: must still be 0x4C.
    uintptr_t sites[NUM_FULLOPEN];
    uint8_t exp = ORIG_FULLOPEN;
    for (int i = 0; i < NUM_FULLOPEN; ++i) {
        sites[i] = VA_FULLOPEN(i);
        if (!health_check_bytes("cmdbox_width", sites[i], &exp, 1)) {
            health_record("cmdbox_width", false, "byte mismatch");
            return;
        }
    }
    bool ok = true;
    for (int i = 0; i < NUM_FULLOPEN; ++i) ok = poke8(sites[i], (uint8_t)W) && ok;
    hook_log("cmdbox_width: install %s — fully-open value 0x%02X->0x%02X at %d "
             "sites (box width %d)\n", ok ? "complete" : "FAILED",
             ORIG_FULLOPEN, (unsigned)W, NUM_FULLOPEN, W);
    char note[64];
    snprintf(note, sizeof(note), "W=0x%02X @%d sites", (unsigned)W, NUM_FULLOPEN);
    health_record("cmdbox_width", ok, note);
}

static void install_shift() {
    int x = g_cfg.cmdbox_x;
    if (x == ORIG_BOX_X) {  // 16 = stock = off
        health_record_disabled("cmdbox_x", "config off (stock 16)");
        return;
    }
    if (x < 0 || x > 48) {
        hook_log("cmdbox_x: IGNORED %d — range 0..48 (16 = off)\n", x);
        health_record("cmdbox_x", false, "out of range");
        return;
    }
    int delta = x - ORIG_BOX_X;  // negative = shift left

    // Pre-flight box-x byte.
    uintptr_t va_box_x = VA_BOX_X();
    uint8_t exp = ORIG_BOX_X;
    if (!health_check_bytes("cmdbox_x", va_box_x, &exp, 1)) {
        health_record("cmdbox_x", false, "box-x mismatch");
        return;
    }
    bool ok = poke8(va_box_x, (uint8_t)x);

    // Make the selection cursor follow the box: cursor x = item.x - (0x14 - delta),
    // i.e. shift the `sub dx,0x14` immediate by the same delta the box moved. This
    // keeps the cursor aligned to the shifted box WITHOUT touching the shared item
    // text table. New immediate = 0x14 - delta (delta negative when moving left).
    int new_subx = (int)ORIG_CUR_SUBX - delta;
    if (new_subx >= 0 && new_subx <= 0x7F) {
        uintptr_t va_subx = VA_CURSOR_SUBX();
        uint8_t exp2 = ORIG_CUR_SUBX;
        if (health_check_bytes("cmdbox_x", va_subx, &exp2, 1))
            ok = poke8(va_subx, (uint8_t)new_subx) && ok;
    }
    hook_log("cmdbox_x: install %s — command BOX origin x 16->%d (delta %d), "
             "cursor x-offset 0x14->0x%02X; shared item table untouched\n",
             ok ? "complete" : "FAILED", x, delta, new_subx);
    char note[64];
    snprintf(note, sizeof(note), "box x=%d (delta %d)", x, delta);
    health_record("cmdbox_x", ok, note);
}

static void install_cursor_w() {
    int cw = g_cfg.cmdbox_cursor_w;
    if (cw <= 0 || cw == ORIG_CUR_W) {
        health_record_disabled("cmdbox_cursor_w", "config off (stock 0x47)");
        return;
    }
    if (cw < 0x20 || cw > 0xA0) {
        hook_log("cmdbox_cursor_w: IGNORED %d (0x%02X) — range 0x20..0xA0\n", cw, cw);
        health_record("cmdbox_cursor_w", false, "out of range");
        return;
    }
    uintptr_t va_cw = VA_CURSOR_W();
    uint8_t exp = ORIG_CUR_W;
    if (!health_check_bytes("cmdbox_cursor_w", va_cw, &exp, 1)) {
        health_record("cmdbox_cursor_w", false, "byte mismatch");
        return;
    }
    bool ok = poke8(va_cw, (uint8_t)cw);
    hook_log("cmdbox_cursor_w: install %s — cursor width 0x%02X->0x%02X\n",
             ok ? "complete" : "FAILED", ORIG_CUR_W, (unsigned)cw);
    char note[64];
    snprintf(note, sizeof(note), "cursor w=0x%02X", (unsigned)cw);
    health_record("cmdbox_cursor_w", ok, note);
}

void cmdbox_width_install() {
    install_width();
    install_shift();
    install_cursor_w();
}
