// Read-only recon probe for the Steam (Enigma-packed) BOF4.exe. See recon_scan.h.
//
// The Steam exe is Enigma-*packed* (not code-virtualized): its first section has
// the SAME virtual layout as GOG's .text (VA 0x401000, Vsz 0x2A7000) and Enigma
// decrypts it back to the ORIGINAL addresses at runtime, with ImageBase 0x400000
// and no ASLR. So IF the underlying build equals GOG, our entire VA map transfers.
// This probe verifies that empirically without writing a single byte: it FNV-1a
// hashes the decrypted runtime .text in 64KB chunks and compares to the GOG
// reference baked into recon_data.h, plus dumps the exact bytes at our key patch
// VAs. All reads are wrapped in SEH so an Enigma guard/encrypted page logs as
// "unreadable" instead of crashing.

#include "hooks.h"
#include "recon_scan.h"
#include "recon_data.h"

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cstdio>

// FNV-1a 64 — must match the generator in the recon_data.h build script.
static uint64_t fnv1a(const uint8_t* p, uint32_t n) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (uint32_t i = 0; i < n; ++i) { h ^= p[i]; h *= 0x100000001b3ULL; }
    return h;
}

// Read n bytes from va into dst under SEH. Returns false if any byte faults
// (guard page / not-yet-decrypted). POD-only body so SEH is legal here.
static bool safe_read(uintptr_t va, uint8_t* dst, uint32_t n) {
    __try {
        for (uint32_t i = 0; i < n; ++i)
            dst[i] = *((volatile const uint8_t*)(va + i));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void do_scan(const char* label) {
    static uint8_t buf[RECON_CHUNK];

    hook_log("\n=== RECON SCAN [%s] (read-only Steam/Enigma probe) ===\n", label);
    hook_log("recon: runtime .text base 0x%08X, comparing vs GOG BOF4.exe "
             "(sha f925075d), %d chunks of 0x%X\n",
             (unsigned)RECON_TEXT_VA, RECON_NCHUNKS, (unsigned)RECON_CHUNK);

    int match = 0, mism = 0, unread = 0, zero = 0;
    const uint64_t FNV_ZERO64K = 0xEB05052EA5B62325ULL;  // FNV-1a of 64KB of 0x00
    for (int i = 0; i < RECON_NCHUNKS; ++i) {
        const ReconChunk& c = RECON_CHUNKS[i];
        if (!safe_read(c.va, buf, c.len)) {
            ++unread;
            hook_log("recon: chunk[%02d] va=0x%08X len=0x%05X  UNREADABLE "
                     "(guard/encrypted page)\n", i, c.va, c.len);
            continue;
        }
        uint64_t h = fnv1a(buf, c.len);
        if (h == c.fnv) {
            ++match;
        } else if (h == FNV_ZERO64K && c.len == RECON_CHUNK) {
            ++zero;  // page not yet decrypted/committed (all 0x00)
        } else {
            ++mism;
            hook_log("recon: chunk[%02d] va=0x%08X len=0x%05X  MISMATCH "
                     "(rt=0x%016llX exp=0x%016llX)\n",
                     i, c.va, c.len,
                     (unsigned long long)h, (unsigned long long)c.fnv);
        }
    }
    hook_log("recon: TEXT chunks  match=%d  mismatch=%d  zero(undecrypted)=%d  "
             "unreadable=%d  / %d\n",
             match, mism, zero, unread, RECON_NCHUNKS);

    hook_log("recon: --- spot byte checks at our patch VAs (rt vs GOG) ---\n");
    int spot_ok = 0;
    for (int i = 0; i < RECON_NSPOTS; ++i) {
        const ReconSpot& s = RECON_SPOTS[i];
        uint8_t rt[32];
        if (!safe_read(s.va, rt, 32)) {
            hook_log("recon: spot va=0x%08X  UNREADABLE\n", s.va);
            continue;
        }
        bool eq = (memcmp(rt, s.bytes, 32) == 0);
        if (eq) ++spot_ok;
        char rthex[64] = {0}, exhex[64] = {0};
        for (int b = 0; b < 16; ++b) {
            snprintf(rthex + b * 3, 4, "%02X ", rt[b]);
            snprintf(exhex + b * 3, 4, "%02X ", s.bytes[b]);
        }
        hook_log("recon: spot va=0x%08X  %s\n", s.va, eq ? "MATCH" : "DIFF");
        hook_log("        rt : %s\n", rthex);
        hook_log("        gog: %s\n", exhex);
    }

    hook_log("recon: spots match=%d/%d\n", spot_ok, RECON_NSPOTS);
    if (match == RECON_NCHUNKS)
        hook_log("recon: VERDICT [%s] — decrypted code is IDENTICAL to GOG. All "
                 "GOG hook VAs transfer to Steam; only anti-tamper-on-WRITE "
                 "remains to test.\n", label);
    else if (match > 0)
        hook_log("recon: VERDICT [%s] — PARTIAL match (%d/%d). Either a different "
                 "build (per-VA rebasing needed) or unpack still in progress.\n",
                 label, match, RECON_NCHUNKS);
    else if (zero > 0 || match == 0)
        hook_log("recon: VERDICT [%s] — no matches yet; %d chunks still zero "
                 "(undecrypted). Enigma unpack likely incomplete at this time — "
                 "see later snapshots.\n", label, zero);
    hook_log("=== RECON SCAN [%s] complete ===\n\n", label);
}

// Background driver: sample the image at init, then again on a delay so we catch
// the point where Enigma has finished decrypting .text. Reads only — never
// writes — so it stays anti-tamper-safe regardless of when it fires.
static DWORD WINAPI recon_thread(LPVOID) {
    const int waits_ms[] = { 3000, 5000, 7000, 15000, 30000 };
    const char* labels[] = { "t+3s", "t+8s", "t+15s", "t+30s", "t+60s" };
    for (int i = 0; i < 5; ++i) {
        Sleep(waits_ms[i]);
        do_scan(labels[i]);
    }
    hook_log("recon: delayed sampling done (last at ~t+60s). If any snapshot "
             "shows match=%d/%d, GOG hooks transfer.\n", RECON_NCHUNKS,
             RECON_NCHUNKS);
    return 0;
}

// ── Tamper test (recon_scan=2) ─────────────────────────────────────────────
// After unpack, apply the REAL save-anywhere byte patch (je 0x74 -> jmp 0xEB at
// 0x00675A38) and monitor it. This is the last gate: does Enigma run a periodic
// memory-checkup that reverts the write or kills the process? The patched byte
// is inert (only affects the pause-menu Save gate), so it is safe to leave live.
static const uintptr_t TAMPER_VA   = 0x00675A38;
static const uint8_t   TAMPER_ORIG = 0x74;  // je  rel8
static const uint8_t   TAMPER_NEW  = 0xEB;  // jmp rel8

static DWORD WINAPI tamper_thread(LPVOID) {
    hook_log("\nrecon: [tamper] waiting for unpack at 0x%08X (expect 0x%02X)...\n",
             (unsigned)TAMPER_VA, TAMPER_ORIG);
    uint8_t b = 0; int waited = 0;
    while (waited < 20000) {
        if (safe_read(TAMPER_VA, &b, 1) && b == TAMPER_ORIG) break;
        Sleep(200); waited += 200;
    }
    if (b != TAMPER_ORIG) {
        hook_log("recon: [tamper] target never reached 0x%02X (got 0x%02X after "
                 "%dms) — abort\n", TAMPER_ORIG, b, waited);
        return 0;
    }
    hook_log("recon: [tamper] unpacked after ~%dms; writing je->jmp (0x%02X->0x%02X)\n",
             waited, TAMPER_ORIG, TAMPER_NEW);
    DWORD op = 0;
    if (!VirtualProtect((void*)TAMPER_VA, 1, PAGE_EXECUTE_READWRITE, &op)) {
        hook_log("recon: [tamper] VirtualProtect FAILED: %lu (write path blocked)\n",
                 GetLastError());
        return 0;
    }
    *(volatile uint8_t*)TAMPER_VA = TAMPER_NEW;
    DWORD ig = 0; VirtualProtect((void*)TAMPER_VA, 1, op, &ig);
    uint8_t rb = 0; safe_read(TAMPER_VA, &rb, 1);
    hook_log("recon: [tamper] wrote 0x%02X, readback=0x%02X  %s\n",
             TAMPER_NEW, rb, rb == TAMPER_NEW ? "(write OK)" : "(WRITE BLOCKED)");

    // Monitor ~40s for an Enigma memory-checkup reverting the byte. If logging
    // simply stops, the checkup terminated the process instead (also a finding).
    int stable = 0;
    for (int i = 0; i < 20; ++i) {
        Sleep(2000);
        uint8_t cur = 0;
        if (!safe_read(TAMPER_VA, &cur, 1)) {
            hook_log("recon: [tamper] t+%02ds: UNREADABLE\n", (i + 1) * 2);
            continue;
        }
        if (cur == TAMPER_NEW) ++stable;
        hook_log("recon: [tamper] t+%02ds: byte=0x%02X  %s\n",
                 (i + 1) * 2, cur, cur == TAMPER_NEW ? "stable" : "REVERTED");
    }
    hook_log("recon: [tamper] monitor done — byte stayed patched %d/20 samples. "
             "If ~20/20 and game alive, Enigma has NO active memory-CRC on .text; "
             "our patches will hold. In-game check: open pause-menu Save OUTSIDE "
             "the world map — it should be selectable.\n", stable);
    return 0;
}

void recon_scan_run() {
    do_scan("init");
    static LONG started = 0;
    if (InterlockedExchange(&started, 1) == 0) {
        HANDLE h = CreateThread(nullptr, 0, recon_thread, nullptr, 0, nullptr);
        if (h) { CloseHandle(h); hook_log("recon: delayed sampler thread started "
                                          "(t+3s..t+60s)\n"); }
        else   { hook_log("recon: CreateThread failed: %lu\n", GetLastError()); }

        if (g_cfg.recon_scan >= 2) {
            HANDLE t = CreateThread(nullptr, 0, tamper_thread, nullptr, 0, nullptr);
            if (t) { CloseHandle(t);
                     hook_log("recon: TAMPER test armed (recon_scan=2) — will "
                              "write save-anywhere byte after unpack\n"); }
            else   { hook_log("recon: tamper CreateThread failed: %lu\n",
                              GetLastError()); }
        }
    }
}
