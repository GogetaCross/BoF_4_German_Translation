// cmdbox_labels.cpp — shift the field command-menu LABEL text + "Command" header
// graphic left, to follow the widened/left-shifted box (see cmdbox / box_autofit).
//
// The menu item loop at BOF4!0x67D0AC reads each item's on-screen position from a
// .data table: X = [esi*4 + 0x95BA7C], Y = [esi*4 + 0x95BA7E], esi = 0..5 for the
// six text labels (Objekte/Spezial/Ausrüstung/Status/Wechseln/Optionen); entry 6
// (0x95BA94) is the "Command" header graphic (special path at 0x67D08D). This is
// the PC equivalent of the German SGAMEN label table (.orga 0xF5B8, X=0x1D) plus
// the command-graphic position (X=0x16).
//
// A pure DATA patch (no code, no counter) — safe, revertable, and it can't affect
// navigation. The table base is resolved by a UNIQUE code signature so it survives
// EXE recompiles; a value-guard (stock X must be 0x26) fails safe if the layout
// changed. Config: cmdbox_label_x (labels), cmdbox_cmdgfx_x (header). 0 = off.
#include "cmdbox_labels.h"
#include "hooks.h"
#include "hook_health.h"

#include <windows.h>
#include <cstdint>
#include <cstdio>

namespace {
    constexpr uint16_t STOCK_X   = 0x26;   // stock label + header X
    constexpr uintptr_t FB_TABLE = 0x0095BA7C;   // documented fallback (live BOF4.exe)

    // Unique read at 0x67D0AC:  mov cx,[esi*4+Ytab]; mov dx,[esi*4+Xtab]
    // 66 8B 0C B5 <Ytab> 66 8B 14 B5 <Xtab>  -> Xtab disp32 at +12.
    const uint8_t SIG_TBL[] = {0x66,0x8B,0x0C,0xB5,0,0,0,0, 0x66,0x8B,0x14,0xB5,0,0,0,0};
    const char    SIG_TBL_M[] = "xxxx????xxxx????";

    bool rd16(uintptr_t va, uint16_t* out) {
        __try { *out = *(volatile uint16_t*)va; return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool poke16(uintptr_t va, uint16_t v) {
        DWORD op = 0;
        if (!VirtualProtect((void*)va, 2, PAGE_EXECUTE_READWRITE, &op)) return false;
        *(volatile uint16_t*)va = v;
        DWORD ig; VirtualProtect((void*)va, 2, op, &ig);
        return true;
    }

    // Resolve the X-table base: scan for the unique pair-read, read disp32 at +12.
    uintptr_t resolve_table() {
        uintptr_t va = resolve_sig("cmdbox_labels.tbl", SIG_TBL, SIG_TBL_M,
                                   sizeof(SIG_TBL), 12);   // -> address of the disp32
        if (va) {
            uint32_t disp = 0;
            __try { disp = *(volatile uint32_t*)va; } __except (EXCEPTION_EXECUTE_HANDLER) {}
            if (disp >= 0x008D4000 && disp < 0x00C78000) return disp;
        }
        return FB_TABLE;
    }
}

void cmdbox_labels_install() {
    int lx = g_cfg.cmdbox_label_x;
    int hx = g_cfg.cmdbox_cmdgfx_x;
    if (lx <= 0 && hx <= 0) {
        health_record_disabled("cmdbox_labels", "config off");
        return;
    }
    uintptr_t tbl = resolve_table();     // X at tbl + i*4 (Y at tbl+2 + i*4)

    // Seven item entries (0..6): Objekte/Spezial/Ausrüstung/Status/Wechseln/
    // Optionen + Speichern. Entry 6 (0x95BA94) IS Speichern (drawn via a
    // conditional path), NOT the "Command" header graphic — so it takes the same
    // label X as the rest. (cmdbox_cmdgfx_x is retained for a genuinely separate
    // header graphic if we ever locate it, but is NOT applied to entry 6.)
    // Value-guard: every entry must currently be stock X (0x26) or we skip.
    bool guard_ok = true;
    for (int i = 0; i < 7 && lx > 0; ++i) {
        uint16_t v; if (!rd16(tbl + i*4, &v) || v != STOCK_X) { guard_ok = false; break; }
    }
    if (!guard_ok) {
        hook_log("cmdbox_labels: ABORT — table @0x%08X not stock (X!=0x26); layout "
                 "differs, leaving labels alone.\n", (unsigned)tbl);
        health_record("cmdbox_labels", false, "value guard");
        return;
    }

    int n = 0;
    if (lx > 0 && lx <= 0x7F)
        for (int i = 0; i < 7; ++i) if (poke16(tbl + i*4, (uint16_t)lx)) ++n;

    hook_log("cmdbox_labels: all 7 label X 0x26->0x%02X (%d/7) @table 0x%08X\n",
             (unsigned)lx, n, (unsigned)tbl);
    char note[48];
    snprintf(note, sizeof(note), "lbl x=%d (7)", lx);
    health_record("cmdbox_labels", n > 0, note);
    (void)hx;   // cmdgfx_x reserved (separate header graphic not located here)
}
