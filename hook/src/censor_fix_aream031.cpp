// censor_fix_aream031 — see censor_fix_aream031.h for the full design rationale.
// Runtime restore of the NA-cut AREAM031 violence beat on the GOG PC port.

#include "hooks.h"
#include "censor_fix_aream031.h"

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cstdio>

// ── VM interpreter fetch chokepoints (GOG BOF4.exe, base 0x400000) ─────────
// Each is the exact 6-byte instruction `mov eax,[esi+0xC0]`; the byte after is
// `mov cl,[eax]` (opcode fetch). Identical set to the validated vm_trace.
static constexpr uintptr_t SITE_MAIN  = 0x004BEF7C;   // main-loop reload -> 0x4BEF82
static constexpr uintptr_t SITE_ENTRY = 0x004BC7FD;   // fn-entry reload  -> 0x4BC803
static constexpr uintptr_t SITE_HIGH  = 0x004BEF8D;   // >=0xC0 path      -> 0x4BEF93
static const uint8_t FETCH_SIG[6] = {0x8B, 0x86, 0xC0, 0x00, 0x00, 0x00};

// Dispatch site — sanity signature only (NOT hooked); part of the abort gate so
// a shifted exe (e.g. a GOG update) is detected before we patch anything.
static constexpr uintptr_t DISPATCH_SITE = 0x004BC838;
static const uint8_t DISPATCH_SIG[7] = {0xFF, 0x24, 0x8D, 0xE0, 0xF3, 0x4B, 0x00};

// ── Data-driven divert engine ───────────────────────────────────────────────
// AREAM031's uncensor = THREE .data edits (confirmed statically vs JP + by the
// runtime execution trace); the pilot shipped only #1. All three restored here:
//   #1 @0x9241A5: INSERT the 24-byte violence beat (+ shared `18 12`) — codecave,
//                 resume 0x9241A7.  (Ursula's head-turn.)
//   #2 @0x924653: EN has an EXTRA `C4` opcode the uncut build lacks — pure SKIP
//                 (PC -> 0x924654). Trace: 0x924652(op17) -> 0x924653(opC4).
//   #3 @0x9246 8C: EN's `C4` reads censored operands `00 00 00 10 00 0F`; the uncut
//                 build reads `10 00 0F 00 23 0A` — corrected-C4 codecave (C4 len=7,
//                 trace-confirmed), resume 0x924696.
// Each divert: on the current ctx's PC hitting `trigger`, steer to a codecave
// (resume at CAVE_END) or directly to `resume` (pure skip). Stateless per ctx.
static const uint8_t CAVE1[] = {   // edit #1: JP 0x923F35..0x923F4E (26 B)
    0x18,0x0C, 0x08,0x05,0x01, 0xC1,0x0F,0x10,0x01, 0x42,0x07, 0x44,
    0xC2,0x61,0x02, 0xC1,0x0F,0x10,0x03, 0x08,0x06,0x01, 0x1B, 0x07, 0x18,0x12
};
static const uint8_t CAVE3[] = {   // edit #3: corrected C4 with uncut operands (7 B)
    0xC4, 0x10,0x00,0x0F,0x00,0x23,0x0A
};
// Censored bytes expected AT/around each trigger (abort gate, per divert):
static const uint8_t V1[] = {0x18,0x0A,0xD0,0x01,0x04,0x18,0x12,0x02,0x0B,0x03,0x18,0x12,0x01}; // @0x9241A0
static const uint8_t V2[] = {0x61,0x00,0x17,0xC4,0xC0,0x1E};                                     // @0x924650
static const uint8_t V3[] = {0x61,0x00,0xC4,0x00,0x00,0x00,0x10};                                // @0x92468A

// ── AREAM027 (group 1) ──────────────────────────────────────────────────────
// The NA cut of AREAM027 = ONE 28-byte "early scene-terminator" block INSERTED in
// the US build at VA 0x922906 that the uncut JP build lacks. Independently
// re-derived from both exes + both runtime traces (see the senior adjudication):
//   * R1 pre-block  (US 0x9227F0..0x922906) is byte-identical to JP at shift +0x258.
//   * The 28-byte block is present in US, absent in JP; every post-block pointer
//     shifts by +0x274 (=+0x258+0x1C) — the exact signature of ONE 28-byte insert
//     and no other content edit. R1 continuation and the R2 director choreo
//     (US 0x939A00.. @ +0x260) are byte-identical -> the whole sequence is present
//     in US, merely gated off.
//   * All 16 entry-table sub-block pointers (US 0x922E0C.. = JP 0x922B98.. + 0x274)
//     are present -> no missing targets.
//   * US trace: the block runs in ctx B555CC at sel 0x0B; op 0x19 @0x92290D then
//     op 0x3F @0x92290F RESET the selector 0x0B->0x00, op 0xC4 @0x922921 ends the
//     block -> fade. It NEVER reaches the 0x922922 continuation (max sel 0x0B).
//   * FIX = a pure SKIP: trigger 0x922906 -> resume **0x922922** (CORRECTED from an
//     earlier 0x922923, which SOFTLOCKED — see below). 0x922922 = op 0x17.
// CORRECTION (2026-08-22, from the softlock fix+trace): the scene selector is a
// COOPERATIVE BATON-PASS chain across ~7 ctx. Ground-truth opcode roles (from the
// US trace, NOT the earlier backwards reading): **op 0x17 = ADVANCE selector (+1)**
// (ctx B51440 seq388-405: every `17` bumps sel 0x08->09->0A->0B), **op 0x18 =
// WAIT until selector==operand** (that is exactly why B555CC PARKS on it). The
// diverted actor B555CC must run the 0x17 @0x922922 to push sel 0x0B->0x0C so the
// next link (ctx B51178, waiting on 0x0C) unblocks and the chain climbs to 0x21.
// Resuming at 0x922923 SKIPPED that increment -> sel stuck at 0x0B, B555CC parked
// forever on `18 0D` (wait sel==0x0D) -> SOFTLOCK (fix+trace: max sel 0x0B, B555CC
// @0x922923 op 0x18). So resume 0x922922: execute 0x17 (sel->0x0C) then fall into
// 0x922923 `18 0D` (wait 0x0D), matching JP ctx B5522C exactly.
// Pure skip: bytes=nullptr, no cave, nothing to relocate. Abort gate verifies the
// full 28-byte block so a modded/updated exe fails safe.
static const uint8_t VM027[] = {   // the 28-byte terminator block @0x922906
    0xC3,0x45,0x02,0x46,0x30,0xC3,0x31,0x19,0x00,0x3F,0x0F,0x3D,0x0E,0x14,
    0x48,0xFF,0x03,0x47,0x1B,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x81,0xC4
};

// ── AREAD157 (group 2) — the Emperor confrontation ──────────────────────────
// The NA cut = ONE structural edit (proven by both exes + both runtime traces):
// the GATE ACTOR's continuation at JP 0x9014EB (42 bytes ending in C4) was replaced
// in US by a 19-byte despawn stub at 0x0090192B. That actor is the only one whose
// script is cut; the other 5 actors + the director + the throne/ascension endgame
// are all present and un-cut in US — they merely DEADLOCK (idle on op 0x18 selector
// waits) because the stub removed the gate actor's selector-advances (op 0x17). The
// 42 cut bytes are 100% build-invariant: NO absolute pointers, no message ids — only
// sel-advances/waits, small immediates, and native-call INDEX 9 (the index is the
// same in both builds; it resolves through the CURRENT scene descriptor's own
// handler array, which in US already holds the correct US handlers — sibling table
// analysis confirmed all 27+standalone exist in US). So the fix is a verbatim cave
// (like AREAM031): divert the gate actor at 0x0090192B into the 42 JP bytes; the
// cave self-terminates (C4), so no resume is needed (resume set to the stub's own
// terminating C4 @0x0090193D as a harmless fallback). Restoring the gate actor's
// sel-advances breaks the deadlock -> the director cascades sel 0x0F..0x17 and the
// 5 idle actors + the endgame run. Two Fou-Lu dialogue lines restored via the DAT.
static const uint8_t CAVE_D157[] = {   // JP 0x9014EB gate-actor continuation (42 B)
    0x17,0x18,0x10,0x42,0x05,0x44,0x18,0x11,0x0F,0x09,0x00,0x18,0x12,0xC0,
    0x0C,0xD0,0x04,0x02,0x17,0x08,0x0E,0x01,0x18,0x14,0x37,0x0F,0x08,0x10,
    0x00,0x1E,0x00,0x0B,0x0F,0x09,0x01,0x08,0x10,0x01,0x18,0x16,0x14,0xC4
};
static const uint8_t VD157[] = {   // the 19-byte despawn stub @0x0090192B (abort gate)
    0xC0,0x20,0x48,0x02,0x03,0x54,0x31,0xFF,0x5C,0x47,0x35,0x10,0x00,0x00,
    0x00,0x00,0x00,0x00,0xC4
};

struct Divert {
    uint32_t trigger;        // PC that triggers this divert
    const uint8_t* bytes;    // codecave bytes, or nullptr for a pure skip
    uint32_t nbytes;
    uint32_t resume;         // skip: PC=resume; cave: CAVE_END -> resume
    uint32_t vaddr;          // abort-gate: verify these censored bytes...
    const uint8_t* vbytes; uint32_t nvbytes;
    int      group;          // 0 = AREAM031, 1 = AREAM027 (independent flags)
    uint32_t cave_start, cave_end;   // runtime (0 = pure skip)
};
static Divert g_diverts[] = {
    {0x009241A5, CAVE1, (uint32_t)sizeof(CAVE1), 0x009241A7, 0x009241A0, V1, (uint32_t)sizeof(V1), 0, 0, 0},
    {0x00924653, nullptr, 0,                     0x00924654, 0x00924650, V2, (uint32_t)sizeof(V2), 0, 0, 0},
    {0x0092468C, CAVE3, (uint32_t)sizeof(CAVE3), 0x00924696, 0x0092468A, V3, (uint32_t)sizeof(V3), 0, 0, 0},
    {0x00922906, nullptr, 0,                     0x00922922, 0x00922906, VM027, (uint32_t)sizeof(VM027), 1, 0, 0},
    {0x0090192B, CAVE_D157, (uint32_t)sizeof(CAVE_D157), 0x0090193D, 0x0090192B, VD157, (uint32_t)sizeof(VD157), 2, 0, 0},
};
static constexpr int      NDIVERTS     = 5;

// Is the divert's owning scene enabled? Each scene has its own independent flag so
// AREAM027 (unverified in-game) never rides on the shipped AREAM031 default.
static inline bool group_on(int g) {
    return g == 0 ? g_cfg.censor_fix_aream031 :
           g == 1 ? g_cfg.censor_fix_aream027 :
                    g_cfg.censor_fix_aread157;
}
static constexpr uint32_t RESUME_RANGE = 64;   // range-resume insurance window

static void* g_cave  = nullptr;   // one page holding all codecaves
static bool  g_armed = false;     // caves populated + safe to divert

static uint8_t g_saved[3][6];
static bool    g_site_hooked[3] = {false, false, false};
static const uintptr_t g_sites[3] = {SITE_MAIN, SITE_ENTRY, SITE_HIGH};

// ── Optional built-in tracer (censor_fix_trace) ─────────────────────────────
// The fix and the read-only vm_trace share the same 3 fetch sites, so to watch
// the scene WITH the fix active we fold a light tracer into the remap helper.
// Records the most-recent per-context PC transitions inside the record range,
// dumped to censor_fix_trace.txt on F6. Used to find the NEXT cut beat (an actor
// that parks in the fixed EN build where its JP twin continues).
static constexpr uint32_t REC_LO = 0x00920000, REC_HI = 0x00930000;
// Scene-discovery (read-only) window: the whole baked-script .data region, so a
// NEW scene's record (VA unknown) is captured wherever it lives. WIDENED
// 2026-08-27 for AREAD145, whose record sits at ~0x8FC6xx — BELOW the old
// 0x900000 floor (JP proved this). Now spans baked .data + emulated PSX RAM.
static constexpr uint32_t SCENE_LO = 0x00600000, SCENE_HI = 0x00C00000;
static uint32_t g_win_lo = REC_LO, g_win_hi = REC_HI;  // set per mode at install
static volatile bool g_trace_active = false;           // fix+trace OR scene-trace
static const char* g_trace_file = "censor_fix_trace.txt";
static constexpr uint32_t SEL_ADDR = 0x00B56658;   // EN selector [0x18]/[0x19]
static constexpr int EVC = 1 << 14;                 // ring (power of two)
struct FxEv { uint32_t pc, ctx, remapped; uint8_t op, sel; };
static FxEv g_ev[EVC];
static volatile long g_ev_head = 0;
static constexpr int FXCTX = 64;
static uint32_t g_lastpc_ctx[FXCTX], g_lastpc_val[FXCTX];
static volatile long g_nlast = 0;
static volatile bool g_trace_run = false;
static HANDLE g_trace_thread = nullptr;

// Scene-trace DIAG: stats over EVERY fetch while tracing (regardless of window),
// so one US capture tells us whether the VM reaches AREAD145's record at all and
// the exact PC span it runs. Answers "is the script gated upstream, or does it
// run (data cut)?" g_diag_all==0 in-scene => record never executed.
static volatile unsigned long long g_diag_all = 0;
static volatile uint32_t g_diag_min = 0xFFFFFFFF, g_diag_max = 0;

static void fx_trace_record(uint32_t ctx, uint32_t pc, uint32_t remapped) {
    long n = g_nlast, idx = -1;
    for (long i = 0; i < n && i < FXCTX; ++i)
        if (g_lastpc_ctx[i] == ctx) { idx = i; break; }
    if (idx < 0 && n < FXCTX) { idx = n; g_lastpc_ctx[n] = ctx; g_lastpc_val[n] = 0xFFFFFFFF; g_nlast = n + 1; }
    if (idx < 0 || pc == g_lastpc_val[idx]) return;
    g_lastpc_val[idx] = pc;
    long h = g_ev_head, e = h & (EVC - 1);
    g_ev[e].pc = pc; g_ev[e].ctx = ctx; g_ev[e].remapped = remapped;
    g_ev[e].op = *(volatile uint8_t*)pc; g_ev[e].sel = *(volatile uint8_t*)SEL_ADDR;
    g_ev_head = h + 1;
}

// ── Per-context PC remap (called from the naked stubs with ctx = esi) ───────
// Pure function of the current context's own [esi+0xC0]; no shared state.
extern "C" void __cdecl censor_fix_remap(uint32_t ctx) {
    if (!g_armed && !g_trace_active) return;   // read-only scene-trace: no arm
    __try {
        uint32_t pc = *(volatile uint32_t*)(ctx + 0xC0);
        uint32_t remapped = 0;
        if (g_armed) {                          // divert only when the fix is armed
            for (int i = 0; i < NDIVERTS; ++i) {
                Divert& d = g_diverts[i];
                if (!group_on(d.group)) continue;        // scene's flag is off
                if (pc == d.trigger) {                   // enter cave, or skip
                    remapped = d.cave_start ? d.cave_start : d.resume;
                    *(volatile uint32_t*)(ctx + 0xC0) = remapped;
                    break;
                }
                if (d.cave_start && pc == d.cave_end) {   // exact: caves advance to
                    remapped = d.resume;                   // exactly cave_end (18 12 +2,
                    *(volatile uint32_t*)(ctx + 0xC0) = remapped;  // C4 +7 — both confirmed).
                    break;                                 // NOT a range: a wide window
                }                                          // overlapped the next cave and
                                                           // bounced the actor to garbage.
            }
        }
        if (g_trace_active) {
            // DIAG over every fetch (any VA) — learn if/where the record runs.
            g_diag_all++;
            if (pc < g_diag_min) g_diag_min = pc;
            if (pc > g_diag_max) g_diag_max = pc;
            if (pc >= g_win_lo && pc < g_win_hi)
                fx_trace_record(ctx, pc, remapped);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Bogus ctx during teardown — ignore.
    }
}

// ── Naked inline-hook stubs ────────────────────────────────────────────────
// Preserve all regs+flags, call the remap helper with esi, then RE-RUN the
// stolen `mov eax,[esi+0xC0]` (the helper may have just rewritten [esi+0xC0],
// so eax must be reloaded AFTER the call) and jmp back to site+6.
static void* g_ret_main  = (void*)(SITE_MAIN  + 6);   // 0x4BEF82
static void* g_ret_entry = (void*)(SITE_ENTRY + 6);   // 0x4BC803
static void* g_ret_high  = (void*)(SITE_HIGH  + 6);   // 0x4BEF93

__declspec(naked) static void stub_main() {
    __asm {
        pushad
        pushfd
        push esi
        call censor_fix_remap
        add  esp, 4
        popfd
        popad
        mov  eax, dword ptr [esi+0C0h]
        jmp  g_ret_main
    }
}
__declspec(naked) static void stub_entry() {
    __asm {
        pushad
        pushfd
        push esi
        call censor_fix_remap
        add  esp, 4
        popfd
        popad
        mov  eax, dword ptr [esi+0C0h]
        jmp  g_ret_entry
    }
}
__declspec(naked) static void stub_high() {
    __asm {
        pushad
        pushfd
        push esi
        call censor_fix_remap
        add  esp, 4
        popfd
        popad
        mov  eax, dword ptr [esi+0C0h]
        jmp  g_ret_high
    }
}

static void* const g_stubs[3] = { (void*)stub_main, (void*)stub_entry, (void*)stub_high };

// ── Verify helpers ──────────────────────────────────────────────────────────
static bool verify_bytes(uintptr_t va, const uint8_t* expect, size_t n,
                         const char* what) {
    __try {
        for (size_t i = 0; i < n; ++i) {
            if (((volatile uint8_t*)va)[i] != expect[i]) {
                hook_log("censor_fix: %s mismatch at 0x%08X+%zu (0x%02X != 0x%02X)"
                         " — aborting install (wrong build / already patched)\n",
                         what, (unsigned)va, i, ((uint8_t*)va)[i], expect[i]);
                return false;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        hook_log("censor_fix: %s read faulted at 0x%08X — aborting\n",
                 what, (unsigned)va);
        return false;
    }
    return true;
}

static bool patch_site(int idx) {
    uintptr_t site = g_sites[idx];
    void* p = (void*)site;
    DWORD old_prot = 0;
    if (!VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &old_prot)) {
        hook_log("censor_fix: VirtualProtect(0x%08X) failed: %lu\n",
                 (unsigned)site, GetLastError());
        return false;
    }
    for (int i = 0; i < 6; ++i) {
        if (((volatile uint8_t*)p)[i] != FETCH_SIG[i]) {
            hook_log("censor_fix: site 0x%08X mismatch at +%d (0x%02X != 0x%02X)"
                     " — skipping\n",
                     (unsigned)site, i, ((uint8_t*)p)[i], FETCH_SIG[i]);
            DWORD ig = 0; VirtualProtect(p, 6, old_prot, &ig);
            return false;
        }
    }
    for (int i = 0; i < 6; ++i) g_saved[idx][i] = ((uint8_t*)p)[i];
    int32_t rel = (int32_t)((uintptr_t)g_stubs[idx] - (site + 5));
    uint8_t patch[6] = {0xE9, 0,0,0,0, 0x90};
    memcpy(patch + 1, &rel, 4);
    memcpy(p, patch, 6);
    DWORD ig = 0; VirtualProtect(p, 6, old_prot, &ig);
    FlushInstructionCache(GetCurrentProcess(), p, 6);
    g_site_hooked[idx] = true;
    hook_log("censor_fix: hooked fetch site 0x%08X -> stub 0x%08X\n",
             (unsigned)site, (unsigned)(uintptr_t)g_stubs[idx]);
    return true;
}

static void unpatch_site(int idx) {
    if (!g_site_hooked[idx]) return;
    void* p = (void*)g_sites[idx];
    DWORD old_prot = 0;
    if (VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &old_prot)) {
        memcpy(p, g_saved[idx], 6);
        DWORD ig = 0; VirtualProtect(p, 6, old_prot, &ig);
        FlushInstructionCache(GetCurrentProcess(), p, 6);
    }
    g_site_hooked[idx] = false;
}

static void fx_trace_dump() {
    long head = g_ev_head, ne = head < EVC ? head : EVC;
    long start = head < EVC ? 0 : head;
    FILE* f = fopen(g_trace_file, "w");
    if (!f) { hook_log("censor_fix: could not open %s\n", g_trace_file); return; }
    if (g_armed)
        fprintf(f, "# AREAM031 fix+trace (fix ACTIVE, %d diverts). cave#1 0x%08X..0x%08X\n",
                NDIVERTS, g_diverts[0].cave_start, g_diverts[0].cave_end);
    else
        fprintf(f, "# scene-trace (READ-ONLY, no diverts) window 0x%08X..0x%08X\n",
                g_win_lo, g_win_hi);
    if (!g_armed) {
        if (g_diag_all)
            fprintf(f, "# DIAG: %llu total fetches (any VA); actual PC span "
                       "[0x%08X..0x%08X]. AREAD145 record = US 0x008FC655..0x008FCD43.\n",
                    g_diag_all, (unsigned)g_diag_min, (unsigned)g_diag_max);
        else
            fprintf(f, "# DIAG: 0 total fetches — script VM did not run during this "
                       "capture (wrong spot, or scene not VM-driven here).\n");
    }
    fprintf(f, "# most-recent %ld of %ld per-ctx PC changes: seq PC op sel ctx [remap]\n",
            ne, head);
    for (long i = 0; i < ne; ++i) {
        long e = (start + i) & (EVC - 1);
        if (g_ev[e].remapped)
            fprintf(f, "%5ld 0x%08X op=0x%02X sel=0x%02X ctx=0x%08X  -> 0x%08X\n",
                    i, g_ev[e].pc, g_ev[e].op, g_ev[e].sel, g_ev[e].ctx, g_ev[e].remapped);
        else
            fprintf(f, "%5ld 0x%08X op=0x%02X sel=0x%02X ctx=0x%08X\n",
                    i, g_ev[e].pc, g_ev[e].op, g_ev[e].sel, g_ev[e].ctx);
    }
    fclose(f);
    hook_log("censor_fix: wrote censor_fix_trace.txt (%ld events of %ld)\n", ne, head);
}

static DWORD WINAPI fx_trace_proc(LPVOID) {
    bool prev = false;
    while (g_trace_run) {
        bool down = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
        if (down && !prev) fx_trace_dump();
        prev = down;
        Sleep(30);
    }
    return 0;
}

void censor_fix_aream031_install() {
    // Shared engine for BOTH uncensor scenes; each has its own independent flag.
    // Install if EITHER is on. AREAM031 (group 0) ships ON; AREAM027 (group 1) is
    // opt-in. A group whose flag is off is neither verified nor diverted.
    if (!g_cfg.censor_fix_aream031 && !g_cfg.censor_fix_aream027 && !g_cfg.censor_fix_aread157) {
        hook_log("censor_fix: disabled (set censor_fix_aream031 and/or "
                 "censor_fix_aream027 and/or censor_fix_aread157 = true to enable)\n");
        return;
    }
    hook_log("censor_fix: enabled groups: AREAM031=%s AREAM027=%s AREAD157=%s\n",
             g_cfg.censor_fix_aream031 ? "on" : "off",
             g_cfg.censor_fix_aream027 ? "on" : "off",
             g_cfg.censor_fix_aread157 ? "on" : "off");

    // 1) Abort gate: verify the exe matches what each ENABLED divert was derived
    //    against (per-divert censored bytes) + the three fetch sites + the
    //    dispatch site. A mismatch (modded/updated exe) aborts the whole install.
    for (int i = 0; i < NDIVERTS; ++i)
        if (group_on(g_diverts[i].group) &&
            !verify_bytes(g_diverts[i].vaddr, g_diverts[i].vbytes,
                          g_diverts[i].nvbytes, "censored-record")) return;
    for (int i = 0; i < 3; ++i)
        if (!verify_bytes(g_sites[i], FETCH_SIG, 6, "fetch-site")) return;
    if (!verify_bytes(DISPATCH_SITE, DISPATCH_SIG, sizeof(DISPATCH_SIG),
                      "dispatch-site")) return;

    // 2) Allocate + populate ONE codecave page BEFORE any hook, packing every
    //    divert's cave bytes sequentially. Caves hold VM bytecode the interpreter
    //    READS (never x86-executed) → PAGE_READWRITE. Set each divert's cave_start/
    //    cave_end; pure-skip diverts keep cave_start=0.
    g_cave = VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_cave) {
        hook_log("censor_fix: VirtualAlloc(codecave) failed: %lu — aborting\n",
                 GetLastError());
        return;
    }
    uint8_t* base = (uint8_t*)g_cave; uint32_t off = 0;
    for (int i = 0; i < NDIVERTS; ++i) {
        Divert& d = g_diverts[i];
        d.cave_start = 0; d.cave_end = 0;
        if (!group_on(d.group)) continue;                 // scene disabled
        if (!d.bytes) continue;                            // pure skip (no cave)
        memcpy(base + off, d.bytes, d.nbytes);
        d.cave_start = (uint32_t)(uintptr_t)(base + off);
        d.cave_end   = d.cave_start + d.nbytes;
        off += d.nbytes + 4;   // small gap between caves
        hook_log("censor_fix: divert#%d [g%d] trigger 0x%08X -> cave 0x%08X..0x%08X resume 0x%08X\n",
                 i + 1, d.group, d.trigger, d.cave_start, d.cave_end, d.resume);
    }
    for (int i = 0; i < NDIVERTS; ++i)
        if (group_on(g_diverts[i].group) && !g_diverts[i].bytes)
            hook_log("censor_fix: divert#%d [g%d] trigger 0x%08X -> SKIP to 0x%08X\n",
                     i + 1, g_diverts[i].group, g_diverts[i].trigger, g_diverts[i].resume);

    // 3) Install the PC-remap hooks and arm the engine.
    int ok = 0;
    for (int i = 0; i < 3; ++i) if (patch_site(i)) ++ok;
    if (ok == 0) {
        hook_log("censor_fix: no fetch hooks installed — disarming\n");
        VirtualFree(g_cave, 0, MEM_RELEASE); g_cave = nullptr;
        return;
    }
    g_armed = true;
    hook_log("censor_fix: install complete (%d/3 hooks). AREAM031=%s AREAM027=%s. "
             "GOG-only, flag-gated; throwaway-save + play-through past the scene AND "
             "the following battle recommended.\n", ok,
             g_cfg.censor_fix_aream031 ? "ON" : "off",
             g_cfg.censor_fix_aream027 ? "ON" : "off");

    if (g_cfg.censor_fix_trace) {
        // WIDE window (whole baked-script region) so fix+trace covers BOTH M027's
        // primary record (~0x922xxx) AND its choreography region (~0x939xxx) —
        // needed to diagnose the M027 softlock (which context parks on which
        // selector value). M031's record (0x924xxx) is inside this too.
        g_win_lo = SCENE_LO; g_win_hi = SCENE_HI;
        g_trace_file = "censor_fix_trace.txt";
        g_trace_active = true;
        g_nlast = 0; g_ev_head = 0;
        g_trace_run = true;
        g_trace_thread = CreateThread(nullptr, 0, fx_trace_proc, nullptr, 0, nullptr);
        hook_log("censor_fix: fix+trace ON — play the scene, press F6 to dump "
                 "censor_fix_trace.txt (finds the next cut beat)\n");
    }
}

// ── READ-ONLY scene discovery (censor_scene_trace) ──────────────────────────
// Installs the SAME three universal VM fetch chokepoints as the fix, but arms NO
// diverts and allocates NO codecave — it only records per-context PC changes
// across the whole baked-script .data region (SCENE_LO..SCENE_HI). Purpose: find
// a NEW censored scene's script record + its branch/park point (e.g. AREAM027,
// whose VA we don't yet know) from ONE playthrough. Mutually exclusive with the
// AREAM031 fix (they share the 3 sites); if both flags are set the fix wins and
// this is skipped. Writes NOTHING to game state. F6 -> censor_scene_trace.txt.
void censor_scene_trace_install() {
    if (!g_cfg.censor_scene_trace) return;
    if (g_cfg.censor_fix_aream031 || g_cfg.censor_fix_aream027) {
        hook_log("censor_scene_trace: skipped — an uncensor fix is ON "
                 "(censor_fix_aream031=%d aream027=%d; they share the 3 fetch sites; "
                 "disable the fix(es) to scene-trace)\n",
                 g_cfg.censor_fix_aream031, g_cfg.censor_fix_aream027);
        return;
    }
    // Abort gate: the 3 fetch sites + dispatch must match (exe sanity). We do NOT
    // verify any censored-record bytes here — the target record is unknown.
    for (int i = 0; i < 3; ++i)
        if (!verify_bytes(g_sites[i], FETCH_SIG, 6, "fetch-site")) return;
    if (!verify_bytes(DISPATCH_SITE, DISPATCH_SIG, sizeof(DISPATCH_SIG),
                      "dispatch-site")) return;

    int ok = 0;
    for (int i = 0; i < 3; ++i) if (patch_site(i)) ++ok;   // g_armed stays false
    if (ok == 0) {
        hook_log("censor_scene_trace: no fetch hooks installed — aborting\n");
        return;
    }
    g_win_lo = SCENE_LO; g_win_hi = SCENE_HI;
    g_trace_file = "censor_scene_trace.txt";
    g_trace_active = true;
    g_nlast = 0; g_ev_head = 0;
    g_diag_all = 0; g_diag_min = 0xFFFFFFFF; g_diag_max = 0;
    g_trace_run = true;
    g_trace_thread = CreateThread(nullptr, 0, fx_trace_proc, nullptr, 0, nullptr);
    hook_log("censor_scene_trace: READ-ONLY (%d/3 hooks, window 0x%08X..0x%08X, "
             "no diverts). Play the scene, press F6 right at the cut to dump "
             "censor_scene_trace.txt.\n", ok, g_win_lo, g_win_hi);
}

void censor_fix_aream031_shutdown() {
    // Stop the trace hotkey thread first.
    g_trace_active = false;
    g_trace_run = false;
    if (g_trace_thread) {
        WaitForSingleObject(g_trace_thread, 200);
        CloseHandle(g_trace_thread);
        g_trace_thread = nullptr;
    }
    // Unhook first so no stub can run after the cave is freed, then disarm the
    // remap, then release the cave.
    for (int i = 0; i < 3; ++i) unpatch_site(i);
    g_armed = false;
    if (g_cave) { VirtualFree(g_cave, 0, MEM_RELEASE); g_cave = nullptr; }
}

// ═══════════════════════════════════════════════════════════════════════════
// AREAD145 "bath scene" DIAGNOSTIC PROBE (2026-08-27) — confirm the gate theory
// to ~99% before building the real fix. TWO independent, flag-gated parts:
//   censor_probe_aread145        = READ-ONLY phase/selector logger (US or JP).
//   censor_probe_aread145_force  = US-only: re-inject JP's deleted beat-7 branch.
// RE (verified this session, both exes): AREAD145's per-area handler (US 0x4539A0)
// writes a scene-phase byte (US 0x00B56657) = {0x0B,3,8,6,4} but NEVER 7 — the
// JP build's beat-7 branch (JP 0x00453B10; sets phase=7 -> bath scene) was
// deleted. In US the two conditional jumps 0x0045404C (je) / 0x00454063 (jne)
// fall through to a bare `pop ebx; ret` at 0x004540EE instead of that branch.
// FIRST iteration forced ONLY phase=7 (no commit) -> the trace showed phase DID
// become 7 but no actors spawned: the phase byte alone is inert; JP's branch
// also calls a scene-commit. So the force now installs a FAITHFUL translation of
// JP's whole beat-7 branch (guard event-flag[2]&&!flag[3]; clear selector; set
// bath positions; phase=7; id=0x11; call commit 0x00655C00) into a codecave and
// repoints the two US jumps to it. This IS the candidate ship fix; still
// flag-gated — test on a THROWAWAY SAVE, then strict review before default-on.
// ═══════════════════════════════════════════════════════════════════════════

// --- verified US addresses ---
static constexpr uint32_t D145_PHASE_US  = 0x00B56657;  // scene-phase byte
static constexpr uint32_t D145_SEL_US    = 0x00B56658;  // beat selector (0..0x0D)
static constexpr uint32_t D145_DESCPTR   = 0x00B570C0;  // current-area descriptor ptr
static constexpr uint32_t D145_DESC_US   = 0x008FD138;  // AREAD145 descriptor (US)
static constexpr uint32_t D145_JE_SITE   = 0x0045404C;  // 0F 84 9C 00 00 00 (je 0x4540EE)
static constexpr uint32_t D145_JNE_SITE  = 0x00454063;  // 0F 85 85 00 00 00 (jne 0x4540EE)
static constexpr uint32_t D145_DEADEND   = 0x004540EE;  // 5B C3 (pop ebx; ret)
static const uint8_t D145_JE_SIG[6]  = {0x0F,0x84,0x9C,0x00,0x00,0x00};
static const uint8_t D145_JNE_SIG[6] = {0x0F,0x85,0x85,0x00,0x00,0x00};
static const uint8_t D145_END_SIG[2] = {0x5B,0xC3};

// ---- read-only logger ----
static volatile bool g_d145_run = false;
static HANDLE        g_d145_thread = nullptr;

static DWORD WINAPI d145_log_proc(LPVOID) {
    // US (GOG) build only — this fork is the US build. (The JP handler is already
    // known by disassembly to write phase=7 at 0x453BA9; no JP logger needed.)
    const uint32_t pha = D145_PHASE_US;
    const uint32_t sla = D145_SEL_US;
    int last_phase = -1, last_sel = -1; uint32_t last_desc = 0xFFFFFFFF;
    hook_log("[d145] phase/selector logger active (US build). phase@0x%08X sel@0x%08X\n",
             pha, sla);
    while (g_d145_run) {
        __try {
            uint8_t ph = *(volatile uint8_t*)pha;
            uint8_t sl = *(volatile uint8_t*)sla;
            uint32_t desc = *(volatile uint32_t*)D145_DESCPTR;
            if (desc != last_desc) {
                if (desc == D145_DESC_US)
                    hook_log("[d145] entered AREAD145 (descriptor active) phase=%u\n", ph);
                last_desc = desc;
            }
            if ((int)ph != last_phase) {
                const char* here = (desc == D145_DESC_US) ? "  <IN-AREAD145>" : "";
                hook_log("[d145] PHASE %d -> %u (sel=%u)%s\n", last_phase, ph, sl, here);
                last_phase = ph;
            }
            if ((int)sl != last_sel) {
                if (sl != 0 || last_sel > 0)   // skip idle 0-noise; show the climb
                    hook_log("[d145] selector %d -> %u (phase=%u)\n", last_sel, sl, ph);
                last_sel = sl;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        Sleep(16);
    }
    return 0;
}

// ---- force cave (US only) ----
static uint8_t* g_d145_cave = nullptr;
static uint8_t  g_d145_je_saved[6], g_d145_jne_saved[6];
static bool     g_d145_je_hooked = false, g_d145_jne_hooked = false;

// Repoint a `0F 8x rel32` conditional jump's rel32 (at site+2) to `dst`.
static bool d145_repoint_jcc(uint32_t site, uint8_t* saved, uint32_t dst) {
    void* p = (void*)site; DWORD op = 0;
    if (!VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &op)) return false;
    for (int i = 0; i < 6; ++i) saved[i] = ((uint8_t*)p)[i];
    int32_t rel = (int32_t)(dst - (site + 6));
    memcpy((uint8_t*)p + 2, &rel, 4);
    DWORD ig = 0; VirtualProtect(p, 6, op, &ig);
    FlushInstructionCache(GetCurrentProcess(), p, 6);
    return true;
}
static void d145_restore_jcc(uint32_t site, const uint8_t* saved) {
    void* p = (void*)site; DWORD op = 0;
    if (VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &op)) {
        memcpy(p, saved, 6);
        DWORD ig = 0; VirtualProtect(p, 6, op, &ig);
        FlushInstructionCache(GetCurrentProcess(), p, 6);
    }
}

static bool d145_install_force() {
    // Verify the exact gate bytes before touching anything.
    if (!verify_bytes(D145_JE_SITE,  D145_JE_SIG,  6, "d145 je-site")  ||
        !verify_bytes(D145_JNE_SITE, D145_JNE_SIG, 6, "d145 jne-site") ||
        !verify_bytes(D145_DEADEND,  D145_END_SIG, 2, "d145 dead-end"))
        return false;

    // FAITHFUL re-injection of JP's deleted beat-7 branch (JP 0x00453B10),
    // instruction-for-instruction translated to US: BSS globals +0x3A0, .text
    // calls made position-independent (`mov edx,imm; call edx`; JP flag-test
    // 0x655980 -> US 0x655AD0, JP commit 0x655AB0 -> US 0x655C00, both +0x150).
    // Guarded by the real story condition (event-flag[2] set && event-flag[3]
    // clear on the per-area event array 0x00B566C4); when it fires it clears the
    // beat selector, sets the bath-scene actor positions (JP beat-7 values),
    // writes phase 0x00B56657 = 7 + scene id 0x00B50611 = 0x11, and calls the
    // scene-commit (0x00655C00) — the same idiom the US handler's own phase-6
    // block uses. Assembled + capstone-verified (scratchpad/asm_cave.py). Ends
    // `pop ebx; ret` (balances the handler's entry `push ebx`, like 0x004540EE).
    const uint8_t cave[] = {
        0x6A,0x02,0x68,0xC4,0x66,0xB5,0x00,0xBA,0xD0,0x5A,0x65,0x00,
        0xFF,0xD2,0x83,0xC4,0x08,0x84,0xC0,0x0F,0x84,0x9C,0x00,0x00,
        0x00,0x6A,0x03,0x68,0xC4,0x66,0xB5,0x00,0xBA,0xD0,0x5A,0x65,
        0x00,0xFF,0xD2,0x83,0xC4,0x08,0x84,0xC0,0x0F,0x85,0x83,0x00,
        0x00,0x00,0x80,0x0D,0xE4,0x57,0xB5,0x00,0x48,0x31,0xC0,0x6A,
        0x04,0xB9,0x00,0xFE,0xFF,0xFF,0x53,0x50,0xA2,0x5B,0x66,0xB5,
        0x00,0xA2,0x5A,0x66,0xB5,0x00,0xA2,0x59,0x66,0xB5,0x00,0xA2,
        0x58,0x66,0xB5,0x00,0x66,0xC7,0x05,0x30,0xB5,0xBA,0x00,0x00,
        0x1A,0x66,0xC7,0x05,0x34,0xB5,0xBA,0x00,0x00,0x18,0x66,0x89,
        0x0D,0x32,0xB5,0xBA,0x00,0x66,0xC7,0x05,0x38,0xB5,0xBA,0x00,
        0x40,0x0A,0x66,0x89,0x0D,0x3A,0xB5,0xBA,0x00,0x66,0xA3,0x3C,
        0xB5,0xBA,0x00,0x66,0xC7,0x05,0x46,0xB5,0xBA,0x00,0x00,0x10,
        0x66,0x89,0x1D,0x2E,0xB5,0xBA,0x00,0x88,0x1D,0x56,0x66,0xB5,
        0x00,0xC6,0x05,0x57,0x66,0xB5,0x00,0x07,0xC6,0x05,0x11,0x06,
        0xB5,0x00,0x11,0xBA,0x00,0x5C,0x65,0x00,0xFF,0xD2,0x83,0xC4,
        0x0C,0x5B,0xC3,
    };
    g_d145_cave = (uint8_t*)VirtualAlloc(nullptr, sizeof(cave),
                                         MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_d145_cave) { hook_log("[d145] cave VirtualAlloc failed\n"); return false; }
    memcpy(g_d145_cave, cave, sizeof(cave));
    FlushInstructionCache(GetCurrentProcess(), g_d145_cave, sizeof(cave));

    g_d145_je_hooked  = d145_repoint_jcc(D145_JE_SITE,  g_d145_je_saved,  (uint32_t)(uintptr_t)g_d145_cave);
    g_d145_jne_hooked = d145_repoint_jcc(D145_JNE_SITE, g_d145_jne_saved, (uint32_t)(uintptr_t)g_d145_cave);
    if (!g_d145_je_hooked || !g_d145_jne_hooked) {
        hook_log("[d145] jcc repoint FAILED (je=%d jne=%d) — reverting\n",
                 g_d145_je_hooked, g_d145_jne_hooked);
        if (g_d145_je_hooked)  { d145_restore_jcc(D145_JE_SITE,  g_d145_je_saved);  g_d145_je_hooked = false; }
        if (g_d145_jne_hooked) { d145_restore_jcc(D145_JNE_SITE, g_d145_jne_saved); g_d145_jne_hooked = false; }
        VirtualFree(g_d145_cave, 0, MEM_RELEASE); g_d145_cave = nullptr;
        return false;
    }
    hook_log("[d145] FORCE armed: je 0x%08X / jne 0x%08X -> beat-7 cave 0x%08X "
             "(%zuB, JP branch re-injected: guard flag[2]&&!flag[3], phase=7, "
             "commit 0x655C00). Test on a THROWAWAY SAVE.\n",
             D145_JE_SITE, D145_JNE_SITE, (unsigned)(uintptr_t)g_d145_cave, sizeof(cave));
    return true;
}

void censor_probe_aread145_install() {
    if (g_cfg.censor_probe_aread145) {
        g_d145_run = true;
        g_d145_thread = CreateThread(nullptr, 0, d145_log_proc, nullptr, 0, nullptr);
    }
    if (g_cfg.censor_probe_aread145_force)
        d145_install_force();
}

void censor_probe_aread145_shutdown() {
    g_d145_run = false;
    if (g_d145_thread) {
        WaitForSingleObject(g_d145_thread, 200);
        CloseHandle(g_d145_thread);
        g_d145_thread = nullptr;
    }
    if (g_d145_je_hooked)  { d145_restore_jcc(D145_JE_SITE,  g_d145_je_saved);  g_d145_je_hooked = false; }
    if (g_d145_jne_hooked) { d145_restore_jcc(D145_JNE_SITE, g_d145_jne_saved); g_d145_jne_hooked = false; }
    if (g_d145_cave) { VirtualFree(g_d145_cave, 0, MEM_RELEASE); g_d145_cave = nullptr; }
}
