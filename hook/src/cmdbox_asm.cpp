// cmdbox_asm.cpp — probe for the German SGAMEN .ASM hack against the port's
// emulated PSX RAM. The port keeps PSX RAM in .psxseg; the RUNTIME base pointer
// lives at .data [0x009156D0] (static init 0x006A8000). PSX addr A -> host
// base + (A - 0x80000000).
//
// This build is a READ-ONLY probe: press F2 with the command menu OPEN and it
// dumps the live bytes at each German site — the CODE sites (box-width addiu,
// box-X, icon/graphic positions) AND the DATA site (the label X/Y position
// table). That tells us definitively what is resident and whether the DATA
// patches (positions/icons) can be applied directly to PSX RAM.
#include "cmdbox_asm.h"
#include "hooks.h"        // g_cfg, hook_log

#include <windows.h>
#include <cstdint>

namespace {
    volatile uint32_t* const PSX_BASE_PTR = (volatile uint32_t*)0x009156D0;

    struct Site { const char* tag; uint32_t psx; int kind; }; // kind 0=code,1=data
    static const Site SITES[] = {
        {"width#1 addiu",        0x801AA2EC, 0},
        {"width#2 addiu",        0x801AA700, 0},
        {"box-X (main)",         0x801AB5B0, 0},
        {"cmd-graphic pos",      0x801AB6F4, 0},
        {"menu-icons pos",       0x801AB758, 0},
        {"sel-frame width",      0x801AB780, 0},
        {"time-box pos",         0x801AC4AC, 0},
        {"zenny-box pos",        0x801AC6CC, 0},
        {"LABEL POS TABLE",      0x801B95B8, 1},  // .orga 0xF5B8 — Objekte/Spezial X/Y
    };

    bool rd(uintptr_t va, uint8_t* out, int n) {
        __try { for (int i=0;i<n;i++) out[i]=((volatile uint8_t*)va)[i]; return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void dump() {
        uint32_t rbase = 0;
        __try { rbase = *PSX_BASE_PTR; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        hook_log("[cmdbox_asm] ==== PROBE (runtime PSX base [0x9156D0]=0x%08X) ====\n", rbase);
        if (!rbase) rbase = 0x006A8000;
        for (const Site& s : SITES) {
            uintptr_t host = (uintptr_t)(rbase + (s.psx - 0x80000000u));
            uint8_t b[16]; bool ok = rd(host, b, 16);
            if (!ok) { hook_log("  %-18s PSX %08X host %08X : (unreadable)\n",
                                s.tag, s.psx, (unsigned)host); continue; }
            char hex[16*3+1]; int p=0;
            for (int i=0;i<16;i++) p+=wsprintfA(hex+p, "%02X ", b[i]);
            const char* note = "";
            if (s.kind==0) {
                // MIPS addiu rX,r0,imm  = [imm_lo imm_hi 0xNN 0x24]
                if (b[3]==0x24) note = "  <== addiu (MIPS RESIDENT)";
                else if (b[0]||b[1]||b[2]||b[3]) note = "  <== non-zero (data?)";
                else note = "  (zero)";
            } else {
                note = (b[0]||b[1]||b[2]||b[3]) ? "  <== DATA RESIDENT (patchable)" : "  (zero)";
            }
            hook_log("  %-18s PSX %08X host %08X : %s%s\n", s.tag, s.psx, (unsigned)host, hex, note);
        }
        hook_log("[cmdbox_asm] ==== END PROBE ====\n");
    }

    HANDLE g_thread = nullptr;
    volatile bool g_quit = false;

    DWORD WINAPI thread_main(LPVOID) {
        hook_log("[cmdbox_asm] probe ready — OPEN the command menu, then press F2.\n");
        bool prev = false;
        while (!g_quit) {
            bool k = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
            if (k && !prev) dump();
            prev = k;
            Sleep(30);
        }
        return 0;
    }
}

void cmdbox_asm_install() {
    if (!g_cfg.cmdbox_asm) return;
    g_quit = false;
    g_thread = CreateThread(nullptr, 0, thread_main, nullptr, 0, nullptr);
    hook_log("[cmdbox_asm] read-only probe installed (F2 dumps PSX-RAM sites).\n");
}

void cmdbox_asm_shutdown() {
    g_quit = true;
    if (g_thread) { WaitForSingleObject(g_thread, 300); CloseHandle(g_thread); g_thread = nullptr; }
}
