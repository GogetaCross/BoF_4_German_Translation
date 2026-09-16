// item_name_msg_uncap.cpp
//
// Lifts the 14-character cap on item names substituted into MESSAGE text via the
// [07:00] control code — e.g. "Kaputtes Schwe kann nicht gehandelt werden!"
// should read "Kaputtes Schwert kann nicht gehandelt werden!".
//
// This is a DIFFERENT cap from item_name_uncap (which lifts the [09:XX:YY]
// menu item-name budget). Traced via a runtime write-watch on the [07] scratch
// slot (0x00B56EC0, 32-byte stride): a family of eight "format message" routines
// copy the item name into the slot through the copier at 0x00629D50, each first
// clamping the name length with the idiom
//
//     xor  ecx,ecx
//   L:cmp  byte [ecx+eax],0
//     je   done
//     inc  ecx
//     cmp  ecx,0x0D          ; <-- the cap: stop counting at 13
//     jl   L
//   done:
//     inc  cl                ; count = min(len,13)+1  -> at most 14 copied
//     push <src>; push ecx (count); push <slot>; call 0x629D50
//
// The 32-byte slot (null-terminated by the copier) safely holds up to 31 chars,
// so we raise the clamp from 13 to NEW_CAP (30 -> up to 31 copied). All eight
// sites share the distinctive tail  83 F9 0D 7C ?? FE C1  (cmp ecx,0x0D / jl
// short / inc cl), which we scan for and patch. On a differently-built EXE the
// byte won't be 0x0D and each site fails its health check -> safe no-op.

#include "hooks.h"
#include "hook_health.h"
#include "item_name_msg_uncap.h"

#include <cstdint>
#include <cstdio>
#include <windows.h>

static constexpr uint8_t EXPECTED_ORIG = 0x0D;   // = 13 chars counted
static constexpr uint8_t NEW_CAP       = 0x1E;   // = 30 (slot holds 31 + null)

// Distinctive clamp-loop tail: cmp ecx,0x0D ; jl short ; inc cl.
// The jl displacement (byte 4) varies by loop-body size, so it's wildcarded.
static const uint8_t SIG[]      = { 0x83, 0xF9, 0x0D, 0x7C, 0x00, 0xFE, 0xC1 };
static const char    SIG_MASK[] = "xxxx?xx";
static constexpr size_t IMM_OFF = 2;             // the 0x0D byte within SIG

static bool patch_one(uintptr_t imm_va, int idx) {
    char tag[48];
    snprintf(tag, sizeof(tag), "item_name_msg_uncap.s%d", idx);
    uint8_t expected = EXPECTED_ORIG;
    if (!health_check_bytes(tag, imm_va, &expected, 1)) return false;
    DWORD old_prot = 0;
    if (!VirtualProtect((void*)imm_va, 1, PAGE_EXECUTE_READWRITE, &old_prot)) {
        hook_log("item_name_msg_uncap: VirtualProtect(0x%08X) failed: %lu\n",
                 (unsigned)imm_va, GetLastError());
        return false;
    }
    *(volatile uint8_t*)imm_va = NEW_CAP;
    DWORD ignore = 0;
    VirtualProtect((void*)imm_va, 1, old_prot, &ignore);
    hook_log("item_name_msg_uncap: site %d 0x%08X: 0x%02X -> 0x%02X "
             "(msg item-name length cap %u -> %u)\n",
             idx, (unsigned)imm_va, EXPECTED_ORIG, NEW_CAP,
             (unsigned)EXPECTED_ORIG, (unsigned)NEW_CAP);
    return true;
}

// Scan the main module's mapped image for every occurrence of SIG and patch the
// 0x0D byte. Robust to address shifts (finds sites by pattern, not fixed VA).
void item_name_msg_uncap_install() {
    if (!g_cfg.item_name_msg_uncap_enabled) {
        hook_log("item_name_msg_uncap: disabled in config — 14-char cap stays\n");
        health_record_disabled("item_name_msg_uncap", "config off");
        return;
    }
    HMODULE base = GetModuleHandleW(nullptr);
    if (!base) { health_record("item_name_msg_uncap", false, "no module base"); return; }
    const auto* dos = (const IMAGE_DOS_HEADER*)base;
    const auto* nt  = (const IMAGE_NT_HEADERS*)((const uint8_t*)base + dos->e_lfanew);
    uintptr_t img   = (uintptr_t)base;
    uintptr_t end   = img + nt->OptionalHeader.SizeOfImage - sizeof(SIG);

    int found = 0, ok = 0;
    for (uintptr_t p = img; p < end; ++p) {
        const uint8_t* q = (const uint8_t*)p;
        bool m = true;
        for (size_t i = 0; i < sizeof(SIG); ++i)
            if (SIG_MASK[i] == 'x' && q[i] != SIG[i]) { m = false; break; }
        if (!m) continue;
        ++found;
        if (patch_one(p + IMM_OFF, found)) ++ok;
        p += sizeof(SIG) - 1;
    }
    hook_log("item_name_msg_uncap: install %s — %d/%d clamp site(s) lifted "
             "13 -> %u chars\n",
             (found && ok == found) ? "complete" : (ok ? "PARTIAL" : "FAILED"),
             ok, found, (unsigned)NEW_CAP);
    char note[64];
    snprintf(note, sizeof(note), "%d/%d sites 13->%u", ok, found, (unsigned)NEW_CAP);
    health_record("item_name_msg_uncap", found && ok == found, note);
}
