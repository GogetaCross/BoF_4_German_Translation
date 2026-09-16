// name_slot_bp.cpp — TEMPORARY DIAGNOSTIC (page-guard variant).
//
// Goal: find the code that fills the [07:00] item-name substitution slot
// (0x00B56EBF, a 32-byte BSS scratch register) with a 14-char-truncated copy.
// Every static reference to 0x00B56EBF in BOF4.exe is a *reader* (the [07]
// control-code handlers), so the writer computes the address indirectly and
// can't be found by scanning.
//
// First attempt used a hardware data-write breakpoint (DR0); under Steam/Enigma
// the debug registers get cleared, so nothing fired. This variant uses a
// PAGE_GUARD watch instead: we mark the page containing slot 0 as guarded, and a
// vectored handler catches the guard fault, logs the writer's EIP when the fault
// is a WRITE inside slot 0, then single-steps past the instruction and re-arms
// the guard. Guarding a whole 4 KB page makes every access to it fault, so we
// only arm it ON DEMAND (press F4 right before triggering the trade message) and
// auto-disarm after the first slot-0 write is captured, keeping the slowdown to a
// brief window.
//
// Workflow: enter the shop, get ready to trade the equipped item -> press F4
// (log: "page-guard ARMED") -> do the trade so "kann nicht gehandelt werden!"
// shows -> the writer's EIP is logged and the guard auto-disarms.
//
// Delete this module (and its config flag) once the writer is found + patched.

#include "hooks.h"
#include "name_slot_bp.h"

#include <cstdint>
#include <cstdio>
#include <windows.h>

static constexpr uintptr_t SLOT0_VA   = 0x00B56EBF;              // [07:00] scratch
static constexpr uintptr_t SLOT0_END  = SLOT0_VA + 32;          // one 32-byte slot
static constexpr uintptr_t PAGE_BASE  = SLOT0_VA & ~(uintptr_t)0xFFF;
static constexpr SIZE_T    PAGE_SIZE  = 0x1000;
static constexpr int       ARM_VK     = VK_F4;                   // arm hotkey

static PVOID g_veh        = nullptr;
static bool  g_installed  = false;
static bool  g_armed      = false;   // page currently guarded
static bool  g_need_rearm = false;   // set between guard-fault and its single-step
static int   g_captures   = 0;

// Dedup writer EIPs so the log stays short.
static uintptr_t g_seen[16];
static int       g_seen_n = 0;
static bool seen_eip(uintptr_t eip) {
    for (int i = 0; i < g_seen_n; ++i) if (g_seen[i] == eip) return true;
    if (g_seen_n < (int)(sizeof(g_seen)/sizeof(g_seen[0]))) g_seen[g_seen_n++] = eip;
    return false;
}

static bool guard_on() {
    DWORD old = 0;
    return VirtualProtect((void*)PAGE_BASE, PAGE_SIZE,
                          PAGE_READWRITE | PAGE_GUARD, &old) != 0;
}
static void guard_off() {
    DWORD old = 0;
    VirtualProtect((void*)PAGE_BASE, PAGE_SIZE, PAGE_READWRITE, &old);
}

static LONG CALLBACK veh(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    CONTEXT* c = ep->ContextRecord;

    if (er->ExceptionCode == STATUS_GUARD_PAGE_VIOLATION) {
        // ExceptionInformation[0]=access (0 read,1 write,8 exec), [1]=address.
        uintptr_t addr = (uintptr_t)er->ExceptionInformation[1];
        if (addr < PAGE_BASE || addr >= PAGE_BASE + PAGE_SIZE)
            return EXCEPTION_CONTINUE_SEARCH;          // not our page
        bool is_write = (er->ExceptionInformation[0] == 1);
        // The guard is auto-cleared by this fault. Log the write to slot 0, then
        // single-step the faulting instruction and re-arm the guard afterwards.
        if (is_write && addr >= SLOT0_VA && addr < SLOT0_END) {
            uintptr_t eip = (uintptr_t)c->Eip;         // FAULT: EIP = the writer
            if (!seen_eip(eip)) {
                // Copier 0x629D50 has pushed ebx/esi/edi by now, so the stack is:
                // [esp]=ebx [esp+4]=esi [esp+8]=edi [esp+0xC]=RET(caller)
                // [esp+0x10]=arg1(slot) [esp+0x14]=arg2(count) [esp+0x18]=arg3(src)
                uint32_t sw[8] = {0}; SIZE_T g2 = 0;
                ReadProcessMemory(GetCurrentProcess(), (LPCVOID)c->Esp, sw,
                                  sizeof(sw), &g2);
                hook_log("[name_slot_bp] WRITE slot0+%u by EIP=0x%08X  count(edi)=%u  "
                         "CALLER(ret)=0x%08X  arg1_slot=0x%08X arg2_count=0x%08X "
                         "arg3_src=0x%08X  [esp..+0x1C]=%08X %08X %08X %08X %08X %08X %08X %08X\n",
                         (unsigned)(addr-SLOT0_VA), (unsigned)eip, (unsigned)c->Edi,
                         sw[3], sw[4], sw[5], sw[6],
                         sw[0],sw[1],sw[2],sw[3],sw[4],sw[5],sw[6],sw[7]);
                ++g_captures;
            }
            if (g_captures >= 1) {   // one-shot: stop the slowdown
                g_armed = false; g_need_rearm = false;
                hook_log("[name_slot_bp] captured — page-guard disarmed "
                         "(press F4 to re-arm)\n");
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
        // Re-arm after we step over the faulting instruction.
        if (g_armed) { g_need_rearm = true; c->EFlags |= 0x100 /*TF*/; }
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (er->ExceptionCode == EXCEPTION_SINGLE_STEP && g_need_rearm) {
        g_need_rearm = false;
        if (g_armed) guard_on();
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Dedicated polling thread — the device-Present tick doesn't run on this game's
// swap-chain present path, so we poll F4 here (same proven pattern as
// rect_hook's hotkey_thread) instead of relying on a per-frame callback.
static DWORD WINAPI arm_thread(LPVOID) {
    bool prev = false;
    for (;;) {
        bool down = (GetAsyncKeyState(ARM_VK) & 0x8000) != 0;
        if (down && !prev) {
            g_captures = 0; g_seen_n = 0;
            g_armed = guard_on();
            hook_log("[name_slot_bp] page-guard %s — trigger the message NOW\n",
                     g_armed ? "ARMED" : "ARM FAILED");
        }
        prev = down;
        Sleep(30);
    }
    return 0;
}

void name_slot_bp_install() {
    if (!g_cfg.name_slot_bp_enabled) return;
    g_veh = AddVectoredExceptionHandler(1 /*first*/, veh);
    g_installed = (g_veh != nullptr);
    if (g_installed)
        CloseHandle(CreateThread(nullptr, 0, arm_thread, nullptr, 0, nullptr));
    hook_log("[name_slot_bp] DIAGNOSTIC ready (page-guard on 0x%08X). VEH=%s. "
             "In the shop, press F4 to ARM, then trigger 'kann nicht gehandelt "
             "werden' — the writer's EIP is logged.\n",
             (unsigned)SLOT0_VA, g_veh ? "ok" : "FAIL");
}

// Present-path tick is unreliable on this game; polling moved to arm_thread.
void name_slot_bp_tick() {}
