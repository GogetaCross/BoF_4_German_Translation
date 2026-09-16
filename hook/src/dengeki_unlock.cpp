// dengeki_unlock.cpp — see dengeki_unlock.h for the full rationale + RE notes.

#include "hooks.h"
#include "hook_health.h"
#include "dengeki_unlock.h"

#include <cstdint>
#include <cstdio>
#include <windows.h>

namespace {

// Dual set-site @0x0066E16F (canonical GOG BOF4.exe 7f83d1d5; identical VAs on the
// Steam/Enigma port). Encodes BOTH flag indices AND the event-flag array address:
//   68 8D 00 00 00        push 0x8D          ; flag index 1
//   68 2C 67 B5 00        push 0xB5672C      ; event-flag array
//   E8 rel32              call set_flag
//   68 A5 00 00 00        push 0xA5          ; flag index 2
//   68 2C 67 B5 00        push 0xB5672C
//   E8 rel32              call set_flag
// We read the immediates from the match, so a recompile that shifts VAs (but keeps
// the same constants) still resolves correctly.
const uint8_t SIG[] = {
    0x68, 0x8D, 0x00, 0x00, 0x00,   0x68, 0x2C, 0x67, 0xB5, 0x00,
    0xE8, 0x00, 0x00, 0x00, 0x00,   0x68, 0xA5, 0x00, 0x00, 0x00,
    0x68, 0x2C, 0x67, 0xB5, 0x00,   0xE8, 0x00, 0x00, 0x00, 0x00,
};
const char SIG_MASK[] = "xxxxx" "xxxxx" "x????" "xxxxx" "xxxxx" "x????";

// Resolved at install from the signature match (0 = not installed).
volatile uintptr_t g_array = 0;
volatile uint32_t  g_flag1 = 0, g_flag2 = 0;

volatile bool  g_run    = false;
HANDLE         g_thread = nullptr;

inline uint32_t rd32(uintptr_t va) { return *reinterpret_cast<volatile uint32_t*>(va); }

// Scan the main module's .text for SIG/SIG_MASK. Unlike resolve_sig (which needs a
// UNIQUE match), the set-flag idiom appears at BOTH Dengeki set-sites (0x66E16F and
// 0x66E294) with identical operands, so we accept multiple matches and only require
// that every match agrees on (array, flag1, flag2). Returns a match VA, or 0.
uintptr_t scan_setsite() {
    HMODULE base = GetModuleHandleW(nullptr);
    if (!base) return 0;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(base);
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(p);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const IMAGE_NT_HEADERS* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS*>(p + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    uintptr_t first = 0; int matches = 0;
    uint32_t a0 = 0, f10 = 0, f20 = 0;
    for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s) {
        if (!(sec[s].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const uint8_t* start = p + sec[s].VirtualAddress;
        size_t n = sec[s].Misc.VirtualSize;
        if (n < sizeof(SIG)) continue;
        for (size_t i = 0; i + sizeof(SIG) <= n; ++i) {
            bool ok = true;
            for (size_t j = 0; j < sizeof(SIG); ++j)
                if (SIG_MASK[j] == 'x' && start[i + j] != SIG[j]) { ok = false; break; }
            if (!ok) continue;
            uintptr_t m = reinterpret_cast<uintptr_t>(start + i);
            uint32_t a  = rd32(m + 6),  a2 = rd32(m + 21);
            uint32_t f1 = rd32(m + 1),  f2 = rd32(m + 16);
            if (a != a2) continue;                     // malformed — skip
            if (matches == 0) { first = m; a0 = a; f10 = f1; f20 = f2; }
            else if (a != a0 || f1 != f10 || f2 != f20) {
                hook_log("dengeki_unlock: inconsistent set-sites — aborting\n");
                return 0;
            }
            ++matches;
        }
    }
    if (matches) hook_log("dengeki_unlock: %d set-site(s) matched; using 0x%08X\n",
                          matches, (unsigned)first);
    return first;
}

// array[idx>>3] |= 1 << (idx&7)  — mirrors the game's set_flag (0x00655A80).
inline void set_bit(uintptr_t array, uint32_t idx) {
    volatile uint8_t* b = reinterpret_cast<volatile uint8_t*>(array + (idx >> 3));
    *b = static_cast<uint8_t>(*b | (1u << (idx & 7)));
}

DWORD WINAPI unlock_proc(LPVOID) {
    // Re-assert both flags a few times a second. The event-flag array is reloaded
    // from the (locked) saved state on every save/load, so a one-shot write would
    // be undone; polling keeps the store unlocked across loads. Cheap + crash-safe.
    while (g_run) {
        __try {
            if (g_array) { set_bit(g_array, g_flag1); set_bit(g_array, g_flag2); }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // Array not mapped yet / teardown — ignore and retry.
        }
        Sleep(33);
    }
    return 0;
}

} // namespace

void dengeki_unlock_install() {
    if (!g_cfg.dengeki_store_unlock) {
        hook_log("dengeki_unlock: disabled (set dengeki_store_unlock = true to "
                 "restore the Dengeki Store bonus area)\n");
        health_record_disabled("dengeki_unlock", "config off");
        return;
    }

    uintptr_t m = scan_setsite();
    if (!m) {
        hook_log("dengeki_unlock: set-site signature not found — wrong/updated "
                 "build; not installing (store stays locked)\n");
        health_record("dengeki_unlock", false, "sig not found");
        return;
    }

    // Pull both flag indices + the array address straight from the matched code.
    uint32_t flag1, flag2, array, array2;
    __try {
        flag1  = rd32(m + 1);
        array  = rd32(m + 6);
        flag2  = rd32(m + 16);
        array2 = rd32(m + 21);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        hook_log("dengeki_unlock: faulted reading operands @0x%08X — aborting\n",
                 (unsigned)m);
        health_record("dengeki_unlock", false, "operand read faulted");
        return;
    }

    // Sanity: same array both pushes, array in the game BSS window, flags plausible.
    if (array != array2 || array < 0x00B00000 || array >= 0x00C00000 ||
        flag1 > 0x400 || flag2 > 0x400) {
        hook_log("dengeki_unlock: operand sanity failed (array=0x%08X/0x%08X "
                 "flags=0x%X,0x%X) — aborting\n",
                 (unsigned)array, (unsigned)array2, flag1, flag2);
        health_record("dengeki_unlock", false, "operand sanity");
        return;
    }

    g_array = array; g_flag1 = flag1; g_flag2 = flag2;

    g_run = true;
    g_thread = CreateThread(nullptr, 0, unlock_proc, nullptr, 0, nullptr);
    if (!g_thread) {
        g_run = false; g_array = 0;
        hook_log("dengeki_unlock: CreateThread failed: %lu — aborting\n",
                 GetLastError());
        health_record("dengeki_unlock", false, "thread failed");
        return;
    }

    hook_log("dengeki_unlock: ACTIVE — holding event flags 0x%X + 0x%X on array "
             "0x%08X (Dengeki Store unlocked; matches the PSX memcard patch). "
             "Store is JAPANESE until localized; save while active to persist.\n",
             g_flag1, g_flag2, (unsigned)g_array);
    char note[80];
    snprintf(note, sizeof note, "flags 0x%X+0x%X @0x%08X",
             g_flag1, g_flag2, (unsigned)g_array);
    health_record("dengeki_unlock", true, note);
}

void dengeki_unlock_shutdown() {
    g_run = false;
    if (g_thread) {
        WaitForSingleObject(g_thread, 200);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    g_array = 0;
}
