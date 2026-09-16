// equip_menu_shift.cpp
//
// Shifts the equipment-skill "Menü" box (Benutzen / Anlegen), its labels, its
// title tab, the divider AND its selection cursor left by N pixels — the German
// box is widened (box_autofit) but also needs to move left to clear the layout,
// same idea as the field command menu's cmdbox_label_x.
//
// The whole sub-menu derives its X from word[esi+8] (the menu struct's x-origin);
// each element subtracts a fixed offset. Bumping every offset by the shift moves
// the element left by that many pixels, keeping box + text + cursor aligned. The
// two title-tab coords are `+6` and a `-8` lea disp, so those get -shift instead.
// Widths are handled separately by box_autofit (callers 0x681565 / 0x6819C1);
// this only touches X, so the two are independent.
//
// Draw function (0x681544..) and cursor function (0x6819B3..), old BOF4.exe:
//   box    0x68155C  sub ax,0x32   -> +shift   (border, drawer 0x677B50)
//   lbl1   0x681586  sub dx,0x2C   -> +shift   ("Benutzen", 0x629470)
//   lbl2   0x6815AF  sub cx,0x2C   -> +shift   ("Anlegen",  0x629470)
//   div    0x6815C8  sub ax,0x2E   -> +shift   (divider, 0x678890)
//   titleA 0x6815EE  lea [..-8]    -> -shift   (title tab right coord, 0x678A60)
//   titleB 0x6815F3  add ecx,6     -> -shift   (title tab left  coord, 0x678A60)
//   cursor 0x6819B6  sub cx,0x2E   -> +shift   (selection cursor, 0x6786D0)
//
// Health-checked per byte, so on a differently-built EXE each site fails safe.

#include "hooks.h"
#include "hook_health.h"
#include "equip_menu_shift.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <windows.h>

enum Kind { OFF, SIGNED_NEG };   // OFF: new=orig+shift ; SIGNED_NEG: new=(int8)orig-shift

struct Site { uintptr_t va; uint8_t orig; Kind kind; const char* tag; };

static const Site SITES[] = {
    { 0x0068155C, 0x32, OFF,        "box"    },
    { 0x00681586, 0x2C, OFF,        "label1" },
    { 0x006815AF, 0x2C, OFF,        "label2" },
    { 0x006815C8, 0x2E, OFF,        "divider"},
    { 0x006815EE, 0xF8, SIGNED_NEG, "titleA" }, // lea disp -8
    { 0x006815F3, 0x06, SIGNED_NEG, "titleB" }, // add ecx,6
    { 0x006819B6, 0x2E, OFF,        "cursor" },
};

// Write one byte. On the FIRST apply we health-check against the original; on a
// live re-apply the byte already holds a previous shift, so we skip the check and
// always recompute from the (constant) stored original — making reload idempotent.
static bool patch_byte(uintptr_t va, uint8_t expected, uint8_t newv,
                       const char* tag, bool check) {
    if (check) {
        char ftag[48];
        snprintf(ftag, sizeof(ftag), "equip_menu_shift.%s", tag);
        if (!health_check_bytes(ftag, va, &expected, 1)) return false;
    }
    DWORD old_prot = 0;
    if (!VirtualProtect((void*)va, 1, PAGE_EXECUTE_READWRITE, &old_prot)) {
        hook_log("equip_menu_shift: %s VirtualProtect(0x%08X) failed: %lu\n",
                 tag, (unsigned)va, GetLastError());
        return false;
    }
    *(volatile uint8_t*)va = newv;
    DWORD ignore = 0;
    VirtualProtect((void*)va, 1, old_prot, &ignore);
    return true;
}

// Apply the shift to all 7 sites, always computed from the stored originals so it
// is safe to call repeatedly. `first` gates the health check + verbose per-site log.
static int apply_shift(int s, bool first) {
    int ok = 0;
    for (const auto& st : SITES) {
        int nv = (st.kind == OFF) ? ((int)st.orig + s)
                                  : ((int)(int8_t)st.orig - s);
        if (nv < -128 || nv > 127) {   // must fit the single imm byte
            hook_log("equip_menu_shift: %s shift %d overflows imm8 (%d) — skipped\n",
                     st.tag, s, nv);
            continue;
        }
        if (patch_byte(st.va, st.orig, (uint8_t)nv, st.tag, first)) {
            ++ok;
            if (first) hook_log("equip_menu_shift: %s 0x%08X: 0x%02X -> 0x%02X\n",
                                st.tag, (unsigned)st.va, st.orig, (uint8_t)nv);
        }
    }
    return ok;
}

static int clamp_shift(int s) {
    if (s == 0) return 0;
    if (s < -96 || s > 96) { hook_log("equip_menu_shift: %d out of range (-96..96), using 30\n", s); return 30; }
    return s;
}

// The shift amount lives in rect_widths.txt (one file for all menu tuning) as a
// plain directive line `equip_menu_shift = N`. Falls back to the config value
// (g_cfg.equip_menu_shift_px) if the directive isn't present.
static int read_shift() {
    char path[MAX_PATH]; GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* bs = strrchr(path, '\\'); if (bs) *(bs + 1) = 0; else path[0] = 0;
    strncat(path, "rect_widths.txt", MAX_PATH - strlen(path) - 1);
    FILE* f = fopen(path, "r");
    if (!f) return g_cfg.equip_menu_shift_px;
    int s = g_cfg.equip_menu_shift_px;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* p = line; while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#') continue;
        if (_strnicmp(p, "equip_menu_shift", 16) != 0) continue;
        char* eq = strchr(p, '='); if (!eq) continue;
        s = (int)strtol(eq + 1, nullptr, 0);
        break;
    }
    fclose(f);
    return s;
}

void equip_menu_shift_install() {
    int s = clamp_shift(read_shift());
    if (s == 0) {
        hook_log("equip_menu_shift: disabled (equip_menu_shift=0)\n");
        health_record_disabled("equip_menu_shift", "shift=0");
        return;
    }
    int n = (int)(sizeof(SITES) / sizeof(SITES[0]));
    int ok = apply_shift(s, /*first=*/true);
    hook_log("equip_menu_shift: install %s — %d/%d site(s) shifted %d px left\n",
             (ok == n) ? "complete" : (ok ? "PARTIAL" : "FAILED"), ok, n, s);
    char note[64];
    snprintf(note, sizeof(note), "%d/%d sites, %dpx", ok, n, s);
    health_record("equip_menu_shift", ok == n, note);
}

// Re-read the `equip_menu_shift` directive from rect_widths.txt and re-apply.
// Called from the box_autofit F5 handler so the whole-menu shift tunes live.
void equip_menu_shift_reload() {
    int s = clamp_shift(read_shift());
    g_cfg.equip_menu_shift_px = s;
    int ok = apply_shift(s, /*first=*/false);
    hook_log("equip_menu_shift: F5 reload — %d site(s) re-applied at %d px\n", ok, s);
}
