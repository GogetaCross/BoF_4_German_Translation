// Live text-substitution hook on the BoF4 engine renderer (0x00629470).
//
// Phase 1 (trace-only) confirmed:
//   - The renderer is reached via a naked-stub passthrough that is
//     calling-convention-agnostic (mirrors bof4_hooks.cpp:2484).
//   - The string buffer pointer arrives as the FIFTH stack DWORD (s4
//     in our snapshot) — confirmed by 'Südliche Wüste' and other
//     translated strings appearing exactly where expected.
//   - The function is called PER CHARACTER. The caller's outer loop
//     advances s4 by 1 byte between calls (verified for 'Ja\nNein').
//
// Phase 2 (this file) MVP — RAM-patch substitution:
//   The per-char calling pattern means a pure pointer-swap only affects
//   one character; the outer loop reverts to the original buffer for
//   the next char. So instead, when we recognize a buffer at s4, we
//   overwrite the buffer in-place (length-capped, can't grow past
//   original size — same constraint repack_text.py already enforces).
//   Subsequent per-char calls read from our patched buffer naturally.
//   Reverts on area reload because the DAT pool gets re-loaded fresh.
//
// Phase 3 — F8 hotkey:
//   Polls VK in g_cfg.text_substitute_hotkey on a background thread.
//   On rising edge, clears the patched-set so the next time the engine
//   renders the same string, the substitution table is re-applied.
//   This MVP only re-applies the SAME (hardcoded) table — the next
//   iteration adds disk-reload of a sidecar file built from
//   translations/*.json by a Python helper.
#include "hooks.h"
#include "hook_health.h"
#include "text_subst.h"
#include "text_resolver.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>
#include <windows.h>

extern "C" {
#include "minhook/MinHook.h"
}

// Engine text renderer, located by signature. Prologue: sub esp,0x20 /
// mov eax,[esp+0x30] / xor edx,edx / mov ecx,eax / mov dl,[esp+0x32].
// Unique on the live build; on a full renderer recompile the sig won't
// match and we fall back to the hardcoded VA (this whole VWF/render layer
// needs re-RE on a major update regardless — same as menu_space_vwf).
static const uint8_t SIG_RENDER_TEXT[] = {
    0x83,0xEC,0x20,0x8B,0x44,0x24,0x30,0x33,0xD2,0x8B,0xC8,0x8A,0x54,0x24,0x32,
};
static const char    SIG_RENDER_TEXT_MASK[] = "xxxxxxxxxxxxxxx";
static constexpr uintptr_t FALLBACK_RENDER_TEXT = 0x00629470;

typedef void (*raw_fn_t)();
static raw_fn_t g_orig_render_text = nullptr;

// ── ASCII-safe probe (used for log messages only) ────────────────────────
static int safe_text_probe(const void* p, char* out, int cap) {
    if (!p || cap < 1) { if (cap > 0) out[0] = 0; return -1; }
    int n = 0;
    __try {
        const unsigned char* s = (const unsigned char*)p;
        volatile unsigned char first = s[0];
        (void)first;
        for (; n < cap - 1; ++n) {
            unsigned char b = s[n];
            if (b == 0) break;
            out[n] = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
        }
        out[n] = 0;
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
        return -1;
    }
}

// ── Logging dedup + cap (for the trace path) ─────────────────────────────
static uint32_t g_seen_ptrs[256] = {};
static int      g_seen_idx = 0;

static bool seen_recently(uint32_t ptr) {
    for (int i = 0; i < 256; ++i) {
        if (g_seen_ptrs[i] == ptr) return true;
    }
    g_seen_ptrs[g_seen_idx] = ptr;
    g_seen_idx = (g_seen_idx + 1) & 255;
    return false;
}

static std::atomic<uint64_t> g_call_count{0};
static std::atomic<bool>     g_cap_warned{false};
static constexpr uint64_t TEXTREN_LOG_CAP = 100000;

// ── Hardcoded substitution table ─────────────────────────────────────────
// Empty by design — Phase 2 had test entries (Ryu→TST, Bist du sicher? →
// WIRKLICH ECHT?, etc.) used to prove the patch path. The live table
// from translations\.live\textsubst.bin now drives all real substitutions,
// so these are removed. The struct + scan loop stay so future per-build
// experiments (e.g. emergency overrides) can be added without rewiring.
struct SubstEntry {
    const uint8_t* orig;
    size_t         orig_len;
    const uint8_t* repl;
    size_t         repl_len;
    const char*    label;  // for log diagnostics
};

static const SubstEntry  g_subst_table[1] = {};   // [0] is sentinel, never read
static constexpr int     SUBST_TABLE_LEN = 0;

// ── Live (dynamic) substitution table loaded from textsubst.bin ──────────
// File format produced by build_live_subst.py:
//   [u32 magic = 'TLBN']  [u32 version=1]  [u32 num_entries]
//   for each entry: [u16 orig_len] [u16 repl_len] [orig bytes] [repl bytes]
// We index by the FIRST byte of the original — most game strings start
// with a control byte or a capital letter, so the bucket distribution is
// reasonable. Inside a bucket we full-memcmp.
// LiveEntry + g_live_* are declared in text_subst.h and shared with
// text_resolver.cpp.
std::shared_mutex                                   g_live_mtx;
std::vector<LiveEntry>                              g_live_entries;
std::unordered_map<uint8_t, std::vector<uint32_t>>  g_live_index;

static constexpr uint32_t LIVE_MAGIC   = 0x4E424C54u;  // 'TLBN'
static constexpr uint32_t LIVE_VERSION = 1u;
static constexpr const char* LIVE_PATH = "translations\\.live\\textsubst.bin";

// Re-read translations\.live\textsubst.bin and atomically swap the in-
// memory table. Returns the new entry count, or -1 on parse failure.
// Safe to call from any thread.
static int load_live_table() {
    FILE* f = nullptr;
    fopen_s(&f, LIVE_PATH, "rb");
    if (!f) {
        hook_log("text_subst: %s not found — only hardcoded table active "
                 "(run `py build_live_subst.py` to generate)\n", LIVE_PATH);
        return -1;
    }
    uint32_t magic = 0, version = 0, count = 0;
    bool ok = (fread(&magic,   4, 1, f) == 1)
           && (fread(&version, 4, 1, f) == 1)
           && (fread(&count,   4, 1, f) == 1);
    if (!ok || magic != LIVE_MAGIC || version != LIVE_VERSION) {
        hook_log("text_subst: %s bad header (magic=0x%08X version=%u)\n",
                 LIVE_PATH, magic, version);
        fclose(f);
        return -1;
    }
    std::vector<LiveEntry> ents;
    ents.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint16_t olen = 0, rlen = 0;
        if (fread(&olen, 2, 1, f) != 1 || fread(&rlen, 2, 1, f) != 1) break;
        LiveEntry e;
        e.orig.resize(olen);
        e.repl.resize(rlen);
        if (olen && fread(e.orig.data(), 1, olen, f) != olen) break;
        if (rlen && fread(e.repl.data(), 1, rlen, f) != rlen) break;
        // No length filter here — entries with rlen > olen are valid for
        // text_resolver (which allocates its own heap buffer) and are
        // skipped at the in-place-patch step in try_substitute below.
        ents.push_back(std::move(e));
    }
    fclose(f);

    std::unordered_map<uint8_t, std::vector<uint32_t>> idx;
    for (uint32_t i = 0; i < ents.size(); ++i) {
        if (!ents[i].orig.empty())
            idx[ents[i].orig[0]].push_back(i);
    }

    int n;
    {
        std::unique_lock lk(g_live_mtx);
        g_live_entries.swap(ents);
        g_live_index.swap(idx);
        n = (int)g_live_entries.size();
    }
    hook_log("text_subst: loaded %d live pairs from %s\n", n, LIVE_PATH);
    return n;
}

// Spawn `py build_live_subst.py --quiet` and wait. Returns true on
// success (script exited 0 within 10s).
static bool spawn_sidecar_build() {
    char cmd[] = "cmd.exe /c py build_live_subst.py --quiet";
    STARTUPINFOA        si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    si.dwFlags     = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (!CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        hook_log("text_subst: CreateProcess(build_live_subst) failed: %lu\n",
                 GetLastError());
        return false;
    }
    DWORD waited = WaitForSingleObject(pi.hProcess, 10000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (waited != WAIT_OBJECT_0) {
        hook_log("text_subst: build_live_subst.py timed out (10s)\n");
        return false;
    }
    if (code != 0) {
        hook_log("text_subst: build_live_subst.py exit code %lu\n", code);
        return false;
    }
    return true;
}

// ── Patched-buffer set ───────────────────────────────────────────────────
// Tracks every buffer we've overwritten AND the bytes that were there
// before our patch. F8 reload restores those original bytes so the next
// render finds the original DAT bytes again — without this the in-RAM
// data drifts away from what the sidecar uses as substitution keys, and
// after one patch nothing else can ever apply to the same buffer.
struct PatchEntry {
    uint32_t              addr;
    std::vector<uint8_t>  original;  // bytes overwritten by our patch
};

static std::mutex                g_patched_mtx;
static std::vector<PatchEntry>   g_patched;

static bool already_patched(uint32_t ptr) {
    std::lock_guard<std::mutex> lk(g_patched_mtx);
    for (const auto& p : g_patched)
        if (p.addr == ptr) return true;
    return false;
}

// Save the bytes we're about to overwrite so F8 can roll them back.
static void mark_patched(uint32_t ptr, const uint8_t* original, size_t len) {
    PatchEntry e;
    e.addr = ptr;
    e.original.assign(original, original + len);
    std::lock_guard<std::mutex> lk(g_patched_mtx);
    g_patched.push_back(std::move(e));
}

// Forward decl — defined below patch_memory.
static bool patch_memory(void* dst, const void* src, size_t len);

// F8 entry point: write original bytes back into every patched buffer,
// then clear the set so a fresh render-pass can re-apply the new table.
static void revert_and_clear_patched_log() {
    int count = 0;
    std::vector<PatchEntry> to_revert;
    {
        std::lock_guard<std::mutex> lk(g_patched_mtx);
        to_revert.swap(g_patched);
        count = (int)to_revert.size();
    }
    // Restore outside the lock — patch_memory does VirtualProtect which
    // can be slow, and we don't want to block the hot path.
    for (auto& p : to_revert) {
        patch_memory((void*)(uintptr_t)p.addr,
                     p.original.data(), p.original.size());
    }
    hook_log("text_subst: F8 reload — reverted %d patched buffer(s) to "
             "original DAT bytes; next render re-applies the new table\n",
             count);
}

// ── Memory patch via VirtualProtect ──────────────────────────────────────
// DAT pools get loaded into normal heap pages (PAGE_READWRITE), but
// flipping protection ourselves makes the patch robust against any
// future allocator change. SEH wraps the actual write so a bad pointer
// doesn't take the game down.
static bool patch_memory(void* dst, const void* src, size_t len) {
    if (!len) return true;
    DWORD old = 0;
    if (!VirtualProtect(dst, len, PAGE_READWRITE, &old)) {
        hook_log("text_subst: VirtualProtect(%p, %zu) failed: err=%lu\n",
                 dst, len, GetLastError());
        return false;
    }
    bool ok = true;
    __try {
        memcpy(dst, src, len);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DWORD discard = 0;
    VirtualProtect(dst, len, old, &discard);
    if (!ok) {
        hook_log("text_subst: AV during memcpy(%p, ..., %zu)\n", dst, len);
    }
    return ok;
}

// ── SEH-isolated probe + memcmp helpers ──────────────────────────────────
// These exist solely because MSVC rule C2712 forbids __try in any function
// that also has C++ objects requiring unwinding (e.g. std::shared_lock).
// We keep these helpers POD-only so try_substitute can hold a shared_lock
// without conflict.
static int seh_read_byte(const void* p, size_t off) {
    __try {
        return ((const uint8_t*)p)[off];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Returns 0 on equal, nonzero on different, -1 on access violation.
static int seh_memcmp(const void* a, const void* b, size_t n) {
    if (!n) return 0;
    __try {
        return memcmp(a, b, n) == 0 ? 0 : 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// ── Page-break cache fixup ───────────────────────────────────────────────
//
// Background (deep RE notes; see project_live_text_hook.md): the BoF4 PC
// dialog system pre-decides "render N visible chars before paging" when
// each page starts. Our RAM patches change the buffer bytes but not that
// cached count, so length-changing edits before a [02] page-break leak
// chars from page 2 into page 1.
//
// Dialog state lives behind global pointer at 0x00B573F8 → struct fields:
//   +0x0C (uint8_t*) page_start  — current page's first byte in the
//                                   source buffer
//   +0x10 (uint8_t*) cursor      — typewriter cursor; on [02] hit the
//                                   advancer rewinds it to the [02] byte
//   +0x1D (uint8_t)  visible_ct  — visible chars rendered so far in the
//                                   current page (drives the per-frame
//                                   driver loop's exit condition)
//
// Visible vs silent (per the typewriter dispatch tables at 0x00527F38 /
// 0x005284D4 in BOF4.exe):
//   visible: 0x18, 0x1A..0xFF
//   silent : 0x00..0x17 (except 0x18), 0x19  (control codes / NUL)
//
// On a successful patch in [s4, s4+orig_len), if state.page_start lands
// inside that range, we re-scan from page_start, count visible chars
// until next [02], and write back state.visible_ct + state.cursor.
//
// Skipped silently when:
//   - the global pointer is null (no active dialog)
//   - page_start is outside our patched buffer (different system, e.g.
//     menus / battle text — they share the renderer but not this state)
//   - no [02] found within the buffer (last page, or our patched range
//     doesn't actually contain the page break)
static void fixup_dialog_pagebreak(uint32_t s4, size_t orig_len) {
    static constexpr uintptr_t STATE_PTR_GLOBAL = 0x00B573F8;

    uint32_t state_ptr = 0;
    uint32_t page_start = 0;
    uint32_t old_visible = 0;
    uint32_t old_cursor  = 0;

    __try {
        state_ptr = *(volatile uint32_t*)STATE_PTR_GLOBAL;
        if (!state_ptr) return;
        page_start = *(volatile uint32_t*)(state_ptr + 0x0C);
        old_cursor = *(volatile uint32_t*)(state_ptr + 0x10);
        old_visible = *(volatile uint8_t*)(state_ptr + 0x1D);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }

    // page_start must point INTO our patched buffer; otherwise some other
    // subsystem (menu, battle ticker) is currently driving the renderer
    // and we'd corrupt its counter.
    uint32_t buf_end = s4 + (uint32_t)orig_len;
    if (page_start < s4 || page_start >= buf_end) return;

    // Scan from page_start, counting visible chars and locating the [02]
    // page-break byte. Cap at buf_end and at 200 visible (state[+0x1D]
    // is uint8 — over 255 would wrap and break the driver loop).
    int visible = 0;
    uint32_t pb_addr = 0;
    __try {
        const uint8_t* p   = (const uint8_t*)(uintptr_t)page_start;
        const uint8_t* end = (const uint8_t*)(uintptr_t)buf_end;
        for (; p < end; ++p) {
            uint8_t b = *p;
            if (b == 0x02) {
                pb_addr = (uint32_t)(uintptr_t)p;
                break;
            }
            if (b == 0x18 || b >= 0x1A) {
                if (++visible >= 200) break;
            }
            // 0x00..0x17 (except 0x18) and 0x19 are silent — fall through.
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }

    if (!pb_addr) {
        // No [02] in range: probably the last page of the dialog. We
        // don't have a clean fix-up target so leave state alone — the
        // driver loop will simply render whatever count was already set,
        // and the next dialog open will recompute fresh from patched bytes.
        return;
    }

    __try {
        *(volatile uint8_t*) (state_ptr + 0x1D) = (uint8_t)visible;
        // Match the engine's convention: when the typewriter hits [02],
        // its handler does `dec [+0x10]` so the cursor lands at byte
        // BEFORE [02]. We mirror that so the next advancer tick (after
        // the user presses to advance pages) re-reads [02] from the new
        // position and dispatches normally.
        *(volatile uint32_t*)(state_ptr + 0x10) = pb_addr - 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }

    hook_log("text_subst: pagebreak fixup @0x%08X (page_start=0x%08X) "
             "visible: %u->%d  cursor: 0x%08X->0x%08X  [02]@0x%08X\n",
             s4, page_start, old_visible, visible,
             old_cursor, pb_addr - 1, pb_addr);
}

// True iff `s4` looks like a real string start AND the source buffer is
// exactly `orig_len` bytes (terminator at s4+orig_len). Wrapped in SEH
// so a bad pointer never AVs. Both checks together prevent the two
// flavors of false-positive that 4000+ table entries create:
//   - Mid-string matches from the per-char render loop (s4 advancing 1
//     byte at a time would otherwise hit any table entry whose orig
//     happens to equal a tail substring of the rendering buffer).
//   - Prefix collisions (table entry orig is a prefix of a longer real
//     buffer — patching would truncate the longer string).
// Returns -1 on AV (caller should treat as "do not patch").
//   0 = bad (continuation OR prefix collision) — do not patch
//   1 = good (real string boundary on both ends) — safe to patch
static int seh_is_safe_boundary(uint32_t s4, size_t orig_len) {
    __try {
        const uint8_t* p = (const uint8_t*)(uintptr_t)s4;
        // Byte at s4 + orig_len must be \0 — confirms the source string
        // ends exactly at our key length, ruling out prefix collisions.
        if (p[orig_len] != 0) return 0;
        // For SHORT keys (< 8 bytes) we also require p[-1] == 0 because
        // a mid-string per-char-continuation call can otherwise land on
        // a tiny tail that happens to align with our terminator. Real
        // dialog text in the table is far longer than 8 bytes; relaxing
        // the rule there lets slot-0 strings match (their p[-1] is part
        // of the entry's offset table, not \0), without exposing the
        // hot path to false positives on common short substrings.
        if (orig_len < 8 && p[-1] != 0) return 0;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// ── Substitution attempt for a single call ───────────────────────────────
// Called from the helper (which runs in the textren_log_helper SEH frame).
// Quietly does nothing on miss / already-patched / not-substitute.
static void try_substitute(uint32_t s4) {
    if (!g_cfg.text_substitute_enabled) return;
    if (!s4) return;
    if (already_patched(s4)) return;

    // First-byte probe with SEH so a bad pointer doesn't AV.
    int first_int = seh_read_byte((const void*)(uintptr_t)s4, 0);
    if (first_int < 0) return;
    uint8_t first = (uint8_t)first_int;

    // Hardcoded-table scan first.
    int matched = -1;
    for (int i = 0; i < SUBST_TABLE_LEN; ++i) {
        const SubstEntry& e = g_subst_table[i];
        if (!e.orig_len || e.orig[0] != first) continue;
        int rc = seh_memcmp((const void*)(uintptr_t)s4, e.orig, e.orig_len);
        if (rc < 0) return;          // AV — bad pointer
        if (rc == 0) {
            // Boundary check: only patch if s4 is a real string start
            // AND the buffer's length matches our key length.
            int safe = seh_is_safe_boundary(s4, e.orig_len);
            if (safe <= 0) return;   // not a clean boundary — bail
            matched = i;
            break;
        }
    }

    if (matched >= 0) {
        const SubstEntry& e = g_subst_table[matched];
        void* dst = (void*)(uintptr_t)s4;
        // Capture the original bytes BEFORE writing so F8 can roll back.
        // We just verified memcmp == 0, so e.orig is a faithful snapshot.
        mark_patched(s4, e.orig, e.orig_len);
        if (!patch_memory(dst, e.repl, e.repl_len)) return;
        if (e.repl_len < e.orig_len) {
            static const uint8_t ZEROS[64] = {};
            size_t pad = e.orig_len - e.repl_len;
            if (pad > sizeof(ZEROS)) pad = sizeof(ZEROS);
            patch_memory((void*)((uintptr_t)s4 + e.repl_len), ZEROS, pad);
        }
        char osaf[80] = {}, rsaf[80] = {};
        safe_text_probe(e.orig, osaf, sizeof(osaf));
        safe_text_probe(e.repl, rsaf, sizeof(rsaf));
        hook_log("text_subst: PATCHED [%s] @0x%08X '%s' -> '%s' "
                 "(%zu->%zu bytes)\n",
                 e.label, s4, osaf, rsaf, e.orig_len, e.repl_len);
        fixup_dialog_pagebreak(s4, e.orig_len);
        return;
    }

    // Live (.bin) table — indexed by first byte. We hold shared_lock
    // through the patch so the F8 reload can't swap the table out from
    // under us mid-patch (it'd dangle live_match->repl). Lock contention
    // is a non-issue because reload only takes the unique_lock briefly
    // for the std::vector::swap.
    std::shared_lock<std::shared_mutex> lk(g_live_mtx);
    auto it = g_live_index.find(first);
    if (it == g_live_index.end()) return;

    const LiveEntry* live_match = nullptr;
    for (uint32_t idx : it->second) {
        const LiveEntry& e = g_live_entries[idx];
        if (e.orig.empty()) continue;
        int rc = seh_memcmp((const void*)(uintptr_t)s4,
                            e.orig.data(), e.orig.size());
        if (rc < 0) return;     // AV walking past page end
        if (rc == 0) {
            int safe = seh_is_safe_boundary(s4, e.orig.size());
            if (safe <= 0) continue;   // try next bucket entry
            live_match = &e;
            break;
        }
    }
    if (!live_match) return;

    void* dst   = (void*)(uintptr_t)s4;
    size_t olen = live_match->orig.size();
    size_t rlen = live_match->repl.size();
    // Replacements longer than the original can't be in-place patched —
    // they'd corrupt adjacent bytes in the DAT pool. text_resolver covers
    // those by allocating a fresh heap buffer and rewriting the dialog
    // state pointers, so we just leave the original bytes in RAM here.
    if (rlen > olen) return;
    // Snapshot original bytes for F8 revert (we verified memcmp == 0
    // so live_match->orig is a faithful copy of what's at dst).
    mark_patched(s4, live_match->orig.data(), olen);
    if (!patch_memory(dst, live_match->repl.data(), rlen)) return;
    if (rlen < olen) {
        static const uint8_t ZEROS[256] = {};
        size_t pad_off = rlen;
        size_t pad_remaining = olen - rlen;
        while (pad_remaining) {
            size_t chunk = pad_remaining > sizeof(ZEROS)
                         ? sizeof(ZEROS) : pad_remaining;
            patch_memory((void*)((uintptr_t)s4 + pad_off), ZEROS, chunk);
            pad_off       += chunk;
            pad_remaining -= chunk;
        }
    }

    char osaf[48] = {}, rsaf[48] = {};
    safe_text_probe(live_match->orig.data(), osaf, sizeof(osaf));
    safe_text_probe(live_match->repl.data(), rsaf, sizeof(rsaf));
    hook_log("text_subst: PATCHED [live] @0x%08X '%s' -> '%s' "
             "(%zu->%zu bytes)\n", s4, osaf, rsaf, olen, rlen);
    // Drop the shared_lock before the fixup — it touches game memory, not
    // our live table, so there's no reason to keep readers waiting on the
    // odd unique_lock from a concurrent F8 reload.
    lk.unlock();
    fixup_dialog_pagebreak(s4, olen);
}

// ── Phase 1A canary: stack capture keyed on dialog_state[+0x0C] ──────────
// Hunts the resolver — the function that writes a buffer pointer to
// dialog_state[+0x0C] (page_start). On every renderer call we:
//   1. Read the dialog-state pointer from *(uint32_t*)0x00B573F8.
//   2. Read state[+0x0C] (page_start).
//   3. Probe bytes at page_start; if CANARY_NEEDLE is found within the first
//      ~80 bytes AND we haven't already captured this page_start, dump
//      RtlCaptureStackBackTrace + raw stack scan.
// This sidesteps the per-char nature of the renderer arg `s4` — page_start
// stays constant across the typewriter loop, so we log exactly once per
// distinct dialog page (deduped by page_start) and don't burn through any
// global cap. CANARY_DETAIL_MAX still caps total dumps as a safety belt.
static constexpr int           CANARY_DETAIL_MAX = 4;
static constexpr uintptr_t     DIALOG_STATE_GLOBAL = 0x00B573F8;
// "Altar" is in the altar-inspect dialog (AREAD047/048 entry 7 slot 0).
// Short ASCII — survives even if the engine prefixes the string with control
// codes for color/font. Edit if you want a different dialog as the trigger.
static constexpr const char*   CANARY_NEEDLE = "Altar";

static std::atomic<int>      g_canary_detail{0};
static std::atomic<uint32_t> g_canary_last_pagestart{0};

static bool canary_needle_in(const uint8_t* p, int n_read, const char* needle) {
    int nl = (int)std::strlen(needle);
    if (n_read < nl) return false;
    for (int i = 0; i <= n_read - nl; ++i) {
        if (std::memcmp(p + i, needle, nl) == 0) return true;
    }
    return false;
}

static void canary_capture(uint32_t s4) {
    if (g_canary_detail.load(std::memory_order_relaxed) >= CANARY_DETAIL_MAX)
        return;

    // Read dialog_state ptr, then state[+0x0C] = page_start. SEH-protected
    // because the global / struct may be uninitialized at startup.
    uint32_t state = 0, page_start = 0;
    __try {
        state = *(volatile uint32_t*)DIALOG_STATE_GLOBAL;
        if (!state) return;
        page_start = *(volatile uint32_t*)(state + 0x0C);
        if (!page_start) return;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }

    // Dedup: skip if we already captured this page_start.
    uint32_t prev = g_canary_last_pagestart.load(std::memory_order_relaxed);
    if (page_start == prev) return;

    char preview[80] = {};
    int n_preview = safe_text_probe((const void*)(uintptr_t)page_start,
                                    preview, sizeof(preview));
    if (!canary_needle_in((const uint8_t*)preview,
                          n_preview > 0 ? n_preview : 0,
                          CANARY_NEEDLE)) {
        return;
    }

    // Claim this page_start before incrementing detail count.
    if (!g_canary_last_pagestart.compare_exchange_strong(prev, page_start,
            std::memory_order_relaxed)) {
        return;  // someone else already claimed
    }
    int det = g_canary_detail.fetch_add(1, std::memory_order_relaxed);
    if (det >= CANARY_DETAIL_MAX) return;

    hook_log("CANARY-DETAIL[%d] page_start=0x%08X s4=0x%08X state=0x%08X "
             "'%s'\n",
             det + 1, page_start, s4, state, preview);

    // Dump dialog state struct contents — 64 bytes as 16 DWORDs. Other fields
    // may carry entry_idx/slot_idx or asset-id we'll need for sidecar v2.
    hook_log("  state struct dump (64 bytes from 0x%08X):\n", state);
    uint32_t state_words[16] = {};
    __try {
        const uint32_t* sw = (const uint32_t*)(uintptr_t)state;
        for (int i = 0; i < 16; ++i) state_words[i] = sw[i];
        for (int row = 0; row < 4; ++row) {
            hook_log("    +0x%02X: %08X %08X %08X %08X\n",
                     row * 16, state_words[row*4+0], state_words[row*4+1],
                     state_words[row*4+2], state_words[row*4+3]);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        hook_log("    (exception while reading struct)\n");
    }

    // The dialog state at +0x18, +0x1C, +0x20, +0x24 had pointer-shaped
    // values in the previous capture (0x00B506B8, 0x00B20034). Those likely
    // point at metadata structs holding the resolved (file/entry/slot) IDs.
    // Dump 32 bytes (8 DWORDs) at each non-null pointer offset that lands
    // inside a sane address range.
    auto dump_substruct = [&](int off, const char* label) {
        uint32_t p = state_words[off / 4];
        if (p < 0x00400000 || p >= 0x00C00000) return;  // skip non-pointers
        hook_log("  state[+0x%02X]=0x%08X %s sub-struct (32 bytes):\n",
                 off, p, label);
        __try {
            const uint32_t* sw = (const uint32_t*)(uintptr_t)p;
            hook_log("    +0x00: %08X %08X %08X %08X\n",
                     sw[0], sw[1], sw[2], sw[3]);
            hook_log("    +0x10: %08X %08X %08X %08X\n",
                     sw[4], sw[5], sw[6], sw[7]);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            hook_log("    (exception while reading sub-struct)\n");
        }
    };
    dump_substruct(0x18, "@");
    dump_substruct(0x1C, "@");
    dump_substruct(0x20, "@");
    dump_substruct(0x24, "@");

    void* frames[32] = {};
    USHORT n = RtlCaptureStackBackTrace(0, 32, frames, nullptr);
    hook_log("  backtrace (%u frames):\n", (unsigned)n);
    for (USHORT i = 0; i < n; ++i) {
        uintptr_t a = (uintptr_t)frames[i];
        const char* tag = (a >= 0x00401000 && a < 0x006A8000) ? "BOF4" : "ext ";
        hook_log("    bt[%02u] %s 0x%08zX\n", i, tag, (size_t)a);
    }

    // Raw stack scan — FPO fallback. Walk up to 1024 DWORDs above current ESP.
    // Only emit values inside BOF4 .text (0x00401000..0x006A8000), and tag
    // those whose preceding 5 bytes look like `E8 disp32` (= true return
    // addresses) with `RA`. Untagged hits are likely code-pointers in
    // arguments / locals.
    void* esp_now = nullptr;
    __asm { mov esp_now, esp }
    hook_log("  stack scan from esp=%p (.text only, RA = preceded by E8):\n",
             esp_now);
    uintptr_t* base = (uintptr_t*)esp_now;
    for (int i = 0; i < 1024; ++i) {
        uintptr_t v = 0;
        __try { v = base[i]; }
        __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        if (v < 0x00401000 || v >= 0x006A8000) continue;
        bool is_ra = false;
        __try {
            const uint8_t* p = (const uint8_t*)(uintptr_t)(v - 5);
            if (p[0] == 0xE8) is_ra = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { /* leave is_ra=false */ }
        hook_log("    sc esp+0x%03X = 0x%08zX %s\n",
                 i * 4, (size_t)v, is_ra ? "RA" : "  ");
    }
}

// ── __cdecl helper (called from the naked stub) ──────────────────────────
extern "C" void __cdecl textren_log_helper(uint32_t ret_addr,
                                           uint32_t s0, uint32_t s1,
                                           uint32_t s2, uint32_t s3,
                                           uint32_t s4, uint32_t s5,
                                           uint32_t ecx_in, uint32_t edx_in,
                                           uint32_t esi_in, uint32_t edi_in)
{
    // Phase 1A canary — first 8 fresh-string events get a stack dump.
    // Self-disables; 0 cost after that beyond the load+compare in the guard.
    canary_capture(s4);

    // Substitution runs FIRST so the trace-log line shows post-patch
    // bytes (= what the renderer is about to draw). Cheap when disabled.
    try_substitute(s4);

    if (!g_cfg.text_substitute_log) return;

    uint64_t calls = g_call_count.fetch_add(1, std::memory_order_relaxed);
    if (calls >= TEXTREN_LOG_CAP) {
        if (!g_cap_warned.exchange(true)) {
            hook_log("text_subst: log cap %llu reached — further trace "
                     "lines suppressed (substitute path still active)\n",
                     (unsigned long long)TEXTREN_LOG_CAP);
        }
        return;
    }

    if (seen_recently(s4)) return;

    char p0[97]={}, p1[97]={}, p2[97]={}, p3[97]={}, p4[97]={}, p5[97]={};
    char pe[97]={}, pd[97]={}, ps[97]={}, pi[97]={};
    int n0 = safe_text_probe((const void*)(uintptr_t)s0, p0, sizeof(p0));
    int n1 = safe_text_probe((const void*)(uintptr_t)s1, p1, sizeof(p1));
    int n2 = safe_text_probe((const void*)(uintptr_t)s2, p2, sizeof(p2));
    int n3 = safe_text_probe((const void*)(uintptr_t)s3, p3, sizeof(p3));
    int n4 = safe_text_probe((const void*)(uintptr_t)s4, p4, sizeof(p4));
    int n5 = safe_text_probe((const void*)(uintptr_t)s5, p5, sizeof(p5));
    int ne = safe_text_probe((const void*)(uintptr_t)ecx_in, pe, sizeof(pe));
    int nd = safe_text_probe((const void*)(uintptr_t)edx_in, pd, sizeof(pd));
    int ns = safe_text_probe((const void*)(uintptr_t)esi_in, ps, sizeof(ps));
    int ni = safe_text_probe((const void*)(uintptr_t)edi_in, pi, sizeof(pi));

    uint64_t frame = g_present_call_count.load(std::memory_order_relaxed);
    hook_log("textren #%llu f=%llu ret=0x%08X "
             "s0=0x%08X(%d)='%s' s1=0x%08X(%d)='%s' s2=0x%08X(%d)='%s' "
             "s3=0x%08X(%d)='%s' s4=0x%08X(%d)='%s' s5=0x%08X(%d)='%s' "
             "ecx=0x%08X(%d)='%s' edx=0x%08X(%d)='%s' "
             "esi=0x%08X(%d)='%s' edi=0x%08X(%d)='%s'\n",
             (unsigned long long)calls, (unsigned long long)frame,
             ret_addr,
             s0,n0,p0, s1,n1,p1, s2,n2,p2,
             s3,n3,p3, s4,n4,p4, s5,n5,p5,
             ecx_in,ne,pe, edx_in,nd,pd, esi_in,ns,ps, edi_in,ni,pi);
}

// ── Naked stub (preserves all registers, tail-jmps to original) ──────────
__declspec(naked) static void hook_render_text_stub() {
    __asm {
        push ecx
        push edx
        push esi
        push edi
        pushad
        pushfd

        push dword ptr [esp + 36]        ; edi_in
        push dword ptr [esp + 40 + 4]    ; esi_in
        push dword ptr [esp + 44 + 8]    ; edx_in
        push dword ptr [esp + 48 + 12]   ; ecx_in
        push dword ptr [esp + 76 + 16]   ; s5
        push dword ptr [esp + 72 + 20]   ; s4
        push dword ptr [esp + 68 + 24]   ; s3
        push dword ptr [esp + 64 + 28]   ; s2
        push dword ptr [esp + 60 + 32]   ; s1
        push dword ptr [esp + 56 + 36]   ; s0
        push dword ptr [esp + 52 + 40]   ; ret_addr
        call textren_log_helper
        add  esp, 44

        popfd
        popad
        pop edi
        pop esi
        pop edx
        pop ecx
        jmp dword ptr [g_orig_render_text]
    }
}

// ── F8 reload hotkey poll thread (mirrors ff_poll_thread) ────────────────
static HANDLE         g_reload_thread = nullptr;
static volatile bool  g_reload_thread_quit = false;

static DWORD WINAPI reload_poll_thread(LPVOID) {
    bool key_prev = false;
    while (!g_reload_thread_quit) {
        int vk = g_cfg.text_substitute_hotkey;
        bool key = (vk != 0) && (GetAsyncKeyState(vk) & 0x8000) != 0;  // 0 = unbound
        if (key && !key_prev) {
            // F8 workflow:
            //   1. Spawn `py build_live_subst.py` so the .bin reflects
            //      whatever's in translations\*.json RIGHT NOW.
            //   2. Re-read the .bin into our in-memory live table.
            //   3. Clear the patched-set so the engine's next render of
            //      any patched buffer re-applies the (possibly updated)
            //      translation.
            // The script run blocks the poll thread for ~1 second on a
            // full corpus rebuild; that's fine — the renderer hot path
            // doesn't touch the live table during the swap (shared_mutex
            // makes the readers wait at most for a quick std::vector::swap).
            hook_log("text_subst: F8 pressed — rebuilding live table...\n");
            bool built = spawn_sidecar_build();
            if (built) load_live_table();
            // Revert ALL existing patches first so RAM = original DAT
            // bytes again. The next render then re-applies whichever
            // entries the new table contains. Without this, a previously
            // patched buffer stays stuck on the OLD substitution forever.
            revert_and_clear_patched_log();
            // Also flush the resolver's heap-buffer cache so future calls
            // re-allocate fresh buffers from the new translation table.
            text_resolver_on_reload();
        }
        key_prev = key;
        Sleep(30);
    }
    return 0;
}

// ── Public install ───────────────────────────────────────────────────────
void text_subst_install() {
    if (!g_cfg.text_substitute_enabled &&
        !g_cfg.text_substitute_log) {
        hook_log("text_subst: text_substitute / text_substitute_log "
                 "both off - hook NOT installed\n");
        return;
    }

    uintptr_t ADDR_RENDER_TEXT = resolve_sig("text_subst", SIG_RENDER_TEXT,
                                             SIG_RENDER_TEXT_MASK,
                                             sizeof(SIG_RENDER_TEXT), 0);
    if (!ADDR_RENDER_TEXT) ADDR_RENDER_TEXT = FALLBACK_RENDER_TEXT;
    MH_STATUS st = MH_CreateHook((LPVOID)ADDR_RENDER_TEXT,
                                 (LPVOID)hook_render_text_stub,
                                 (LPVOID*)&g_orig_render_text);
    if (st != MH_OK) {
        hook_log("text_subst: MH_CreateHook(0x%08X) failed: status=%d\n",
                 (unsigned)ADDR_RENDER_TEXT, (int)st);
        return;
    }
    st = MH_EnableHook((LPVOID)ADDR_RENDER_TEXT);
    if (st != MH_OK) {
        hook_log("text_subst: MH_EnableHook failed: status=%d\n", (int)st);
        return;
    }
    hook_log("text_subst: trace hook installed @ 0x%08X "
             "(naked-stub passthrough; substitute=%s, log=%s, cap=%llu, "
             "hardcoded=%d entries)\n",
             (unsigned)ADDR_RENDER_TEXT,
             g_cfg.text_substitute_enabled ? "ON" : "off",
             g_cfg.text_substitute_log     ? "ON" : "off",
             (unsigned long long)TEXTREN_LOG_CAP,
             SUBST_TABLE_LEN);

    // Try to load the live table at startup so substitutions are active
    // on the first frame (no need to press F8 first). Missing file is
    // OK — only the hardcoded entries apply until the user runs
    // `py build_live_subst.py` once.
    if (g_cfg.text_substitute_enabled) {
        load_live_table();
    }

    // Start the reload-hotkey thread regardless of substitute_enabled,
    // so the user can toggle behavior without a relaunch later.
    g_reload_thread_quit = false;
    g_reload_thread = CreateThread(nullptr, 0, reload_poll_thread,
                                   nullptr, 0, nullptr);
    if (g_reload_thread) {
        hook_log("text_subst: reload hotkey thread started "
                 "(polling vk=0x%02X every 30ms)\n",
                 g_cfg.text_substitute_hotkey);
    } else {
        hook_log("text_subst: WARNING — failed to start reload thread "
                 "(GetLastError=%lu)\n", GetLastError());
    }
}
