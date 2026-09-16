// Phase 3 — Hook BOF4!0x00527C30 (page-render driver).
//
// Why the page driver and not 0x00527970 (one of the resolvers)?
// Static analysis found TWO functions in BOF4 that write a buffer pointer
// to dialog_state[+0x0C]:
//   1. 0x00527982 inside fn 0x00527970 (bytes "A3 0C EF B4 00")
//   2. 0x005288F0 inside fn 0x00527FA0 (same bytes, different fn)
// Different dialogs reach the renderer through different parents — the
// altar dialog observed in our last session was set up via the second
// writer (fn 0x00527FA0), not 0x00527970, so a hook on 0x00527970 missed.
//
// The page driver 0x00527C30 is downstream of BOTH writers — its prologue
// reads state[+0x0C] (`mov esi, [eax+0xC]`) and walks the typewriter from
// there. Hooking page-driver entry intercepts every dialog/menu/item-desc
// render regardless of which resolver populated dialog state.
//
// On entry to the page driver we read state[+0x0C], look the bytes up in
// the live table (loaded by text_subst.cpp), and if a translation exists
// we allocate a persistent heap buffer with the replacement bytes and
// rewrite state[+0x0C], [+0x10], and [+0x08] to point at it. The original
// page driver then executes with our buffer and the typewriter advances
// through it.
//
// On subsequent calls within the same dialog, state[+0x0C] already points
// at our heap buffer; the lookup naturally misses (the bytes there are
// the *replacement*, not any key) and we skip the override. So the hook
// is idempotent across page flips and per-frame re-issues.
//
// F8 reload calls text_resolver_on_reload() which clears the heap-buffer
// cache so the next dialog open allocates fresh from the just-rebuilt
// translation table.

#include "hooks.h"
#include "hook_health.h"
#include "text_resolver.h"
#include "text_subst.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>

extern "C" {
#include "minhook/MinHook.h"
}

// Page-render driver, located by signature so the hook survives EXE
// recompiles. Prologue: sub esp,0xC / mov eax,[global] / push ebx/ebp/esi /
// mov cx,[eax+0x24] / mov esi,[eax+0xC]. The global is wildcarded.
// Verified unique + relocating in both live and 2026-05 recompile builds
// (live 0x00527C30 -> recompile 0x00527F10).
static const uint8_t  SIG_PAGE_DRIVER[] = {
    0x83,0xEC,0x0C,0xA1,0,0,0,0,0x53,0x55,0x56,0x66,0x8B,0x48,0x24,0x8B,0x70,0x0C,
};
static const char     SIG_PAGE_DRIVER_MASK[] = "xxxx????xxxxxxxxxx";
static constexpr uintptr_t FALLBACK_PAGE_DRIVER = 0x00527C30;
// Absolute .data address of the dialog-state struct. Unlike the hook site
// (signature-relocated above), this global is NOT auto-derived: on a full
// EXE recompile the .data layout shifts and this address goes stale. That is
// SAFE here — every access below goes through seh_read_u32/seh_write_u32, so
// a stale address reads 0 (no substitution) instead of crashing. If a future
// recompile needs the length-uncap back, re-derive this one address (it is
// loaded near the page driver as an absolute mov) and update it.
static constexpr uintptr_t DIALOG_STATE_ADDR = 0x00B4EF00;
static constexpr uintptr_t DS_PAGE_START     = DIALOG_STATE_ADDR + 0x0C;
static constexpr uintptr_t DS_CURSOR         = DIALOG_STATE_ADDR + 0x10;
static constexpr uintptr_t DS_LO16_TAG       = DIALOG_STATE_ADDR + 0x08;

typedef void (__cdecl *page_driver_fn_t)(void);
static page_driver_fn_t g_orig_page_driver = nullptr;

// SEH-only helpers (no C++ unwinding allowed alongside __try in the same fn).
static uint32_t seh_read_u32(uintptr_t addr) {
    uint32_t v = 0;
    __try { v = *(volatile uint32_t*)addr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { v = 0; }
    return v;
}
static void seh_write_u32(uintptr_t addr, uint32_t v) {
    __try { *(volatile uint32_t*)addr = v; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static void seh_write_u16(uintptr_t addr, uint16_t v) {
    __try { *(volatile uint16_t*)addr = v; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Each heap-cached translation. `key` is the original bytes that matched in
// the live table at lookup time (saved so F8 reload can re-find a fresh
// repl for the same key without re-probing orig_buf — the bytes there may
// have been overwritten by text_subst's in-place patcher). `bytes` is the
// rendered content; data() pointer must stay STABLE across F8 reloads so
// the engine's cached pointers don't dangle, hence we reserve up-front.
struct HeapBuf {
    std::vector<uint8_t> key;
    std::vector<uint8_t> bytes;
};

// Total heap-buffer size. Big enough to fit any realistic dialog
// translation. The buffer is always FULLY ZERO-INITIALIZED at allocation;
// content is overwritten on the first N bytes; bytes past content stay
// zero, so even far-overrun reads by the engine see silent NUL pages.
// data() pointer must stay STABLE across F8 reloads so the engine's cached
// pointers don't dangle, which is why we never resize/grow.
static constexpr size_t HEAP_BUF_BYTES = 8192;

static std::shared_mutex                                          g_heap_mtx;
static std::unordered_map<uint32_t, std::unique_ptr<HeapBuf>>     g_heap;
// Reverse map: heap_buf_data() value -> original buffer ptr. Used by the
// detour to detect "state[+0x0C] is already one of ours" and skip the
// override path (otherwise we'd recurse on every page driver re-entry).
static std::unordered_map<uint32_t, uint32_t>                     g_heap_value_to_orig;
static std::atomic<uint64_t>                                      g_calls{0};
static std::atomic<uint64_t>                                      g_swaps{0};

// Overwrite HeapBuf::bytes content with `repl`, zeroing any bytes past
// content end. Buffer must already be sized to HEAP_BUF_BYTES (see
// allocation in get_or_make). Returns true on success.
static bool fill_buf(HeapBuf* hb, const std::vector<uint8_t>& repl) {
    if (repl.size() + 1 > hb->bytes.size()) return false;  // need at least 1 NUL
    // Zero the entire buffer first so any uninitialized junk past prior
    // content is wiped — engine will read NULs everywhere past content.
    std::fill(hb->bytes.begin(), hb->bytes.end(), 0);
    if (!repl.empty()) {
        std::memcpy(hb->bytes.data(), repl.data(), repl.size());
    }
    return true;
}

static const HeapBuf* get_or_make(uint32_t orig_buf,
                                  const std::vector<uint8_t>& key,
                                  const std::vector<uint8_t>& repl) {
    {
        std::shared_lock lk(g_heap_mtx);
        auto it = g_heap.find(orig_buf);
        if (it != g_heap.end()) return it->second.get();
    }
    std::unique_lock lk(g_heap_mtx);
    auto it = g_heap.find(orig_buf);
    if (it != g_heap.end()) return it->second.get();
    auto hb = std::make_unique<HeapBuf>();
    hb->key = key;
    // Allocate the buffer pre-sized and pre-zeroed. Size is fixed for the
    // lifetime of the entry so data() never invalidates.
    hb->bytes.assign(HEAP_BUF_BYTES, 0);
    if (!fill_buf(hb.get(), repl)) {
        hook_log("text_resolver: replacement %zu bytes exceeds buffer %zu\n",
                 repl.size(), HEAP_BUF_BYTES);
        return nullptr;
    }
    uint32_t hb_data = (uint32_t)(uintptr_t)hb->bytes.data();
    g_heap_value_to_orig[hb_data] = orig_buf;
    auto* raw = hb.get();
    g_heap.emplace(orig_buf, std::move(hb));
    return raw;
}

// On F8: re-look-up each cached entry's saved key in the (just-rebuilt)
// live table. Update bytes IN-PLACE for any entry whose content changed.
// data() never moves (buffers are pre-sized) so the engine, which still
// reads from our heap buffer, sees the new content on its next loop
// iteration.
//
// To force the active dialog to redraw with new text mid-frame, we look
// for any heap buffer whose data() contains state[+0x0C] (i.e. page_start
// either equals our buffer start OR points somewhere mid-buffer because
// the engine flipped pages). For each updated buffer that's plausibly
// active, we reset state[+0x0C] AND state[+0x10] back to data() so the
// typewriter rewinds.
void text_resolver_on_reload() {
    uint32_t cur_pagestart = seh_read_u32(DS_PAGE_START);
    uint32_t cur_cursor    = seh_read_u32(DS_CURSOR);

    std::unique_lock lk(g_heap_mtx);
    int n_updated = 0, n_unchanged = 0, n_too_big = 0, n_no_match = 0;
    int n_active = 0;
    uint32_t active_hb_data = 0;
    bool any_updated = false;

    {
        std::shared_lock llk(g_live_mtx);
        for (auto& [orig_buf, hb_unique] : g_heap) {
            HeapBuf* hb = hb_unique.get();
            const std::vector<uint8_t>* new_repl = nullptr;
            if (!hb->key.empty()) {
                auto bit = g_live_index.find(hb->key[0]);
                if (bit != g_live_index.end()) {
                    for (uint32_t idx : bit->second) {
                        const auto& e = g_live_entries[idx];
                        if (e.orig.size() == hb->key.size() &&
                            std::memcmp(e.orig.data(), hb->key.data(),
                                        hb->key.size()) == 0) {
                            new_repl = &e.repl;
                            break;
                        }
                    }
                }
            }
            if (!new_repl) { n_no_match++; continue; }

            uint32_t hb_data = (uint32_t)(uintptr_t)hb->bytes.data();
            uint32_t hb_end  = hb_data + (uint32_t)hb->bytes.size();

            // Is this buffer the one currently active? page_start might be
            // exactly hb_data, or somewhere mid-buffer if the engine
            // advanced past a [02] page break.
            bool is_active = (cur_pagestart >= hb_data &&
                              cur_pagestart <  hb_end);

            // Up-to-date? Compare bytes against the new repl.
            bool bytes_match = (new_repl->size() < hb->bytes.size() &&
                                std::memcmp(hb->bytes.data(),
                                            new_repl->data(),
                                            new_repl->size()) == 0 &&
                                hb->bytes[new_repl->size()] == 0);
            if (bytes_match) {
                n_unchanged++;
                if (is_active) active_hb_data = hb_data;
                continue;
            }

            if (!fill_buf(hb, *new_repl)) { n_too_big++; continue; }
            n_updated++;
            any_updated = true;

            if (is_active) {
                // Rewind the typewriter to the start of our buffer so the
                // user sees the new text from the beginning.
                seh_write_u32(DS_PAGE_START, hb_data);
                seh_write_u32(DS_CURSOR,     hb_data);
                n_active++;
                active_hb_data = hb_data;
            }
        }
    }
    hook_log("text_resolver: F8 reload — updated=%d unchanged=%d "
             "no_match=%d too_big=%d active_redraw=%d "
             "[ds_pagestart=0x%08X cursor=0x%08X active_hb=0x%08X] "
             "(cache size %zu)\n",
             n_updated, n_unchanged, n_no_match, n_too_big, n_active,
             cur_pagestart, cur_cursor, active_hb_data, g_heap.size());
}

// SEH-safe probe of up to N bytes from p, stopping at NUL. Returns count
// captured (excluding any NUL).
static int probe_bytes(const void* p, uint8_t* out, int cap) {
    int n = 0;
    if (!p || cap <= 0) return 0;
    __try {
        const uint8_t* s = (const uint8_t*)p;
        for (; n < cap; ++n) {
            uint8_t b = s[n];
            out[n] = b;
            if (b == 0) break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return n;
}

// Look up the bytes at `orig_buf` in the live table. Requires STRICT
// equality: the NUL-terminated string at orig_buf must match the entry's
// orig bytes exactly. This is critical — without it, a 1-byte orig entry
// would match any buffer starting with that byte, and after our hook
// overrides state[+0x0C] to our heap buffer, the next page driver
// invocation would prefix-match our heap content and recurse forever.
static const LiveEntry* lookup_match(uint32_t orig_buf) {
    if (!orig_buf) return nullptr;
    uint8_t probe[1024];
    int probed = probe_bytes((const void*)(uintptr_t)orig_buf,
                             probe, (int)sizeof(probe));
    if (probed <= 0) return nullptr;
    bool nul_found = (probed < (int)sizeof(probe));
    if (!nul_found) return nullptr;  // be conservative on huge strings
    std::shared_lock lk(g_live_mtx);
    auto bit = g_live_index.find(probe[0]);
    if (bit == g_live_index.end()) return nullptr;
    for (uint32_t idx : bit->second) {
        const auto& e = g_live_entries[idx];
        if (e.orig.size() != (size_t)probed) continue;  // strict size match
        if (std::memcmp(probe, e.orig.data(), e.orig.size()) == 0) {
            return &e;
        }
    }
    return nullptr;
}


// Decode a probed dialog buffer into readable text with control codes shown as
// [XX]/[XX:YY]/[XX:YY:ZZ] escapes (grammar mirrors extract_text.py so it reads
// like text_dump / the editor). 0x01 newline -> literal "\n"; high bytes -> <XX>
// hex so umlaut/glyph-index bytes stay unambiguous. `n` = length excluding NUL.
static std::string decode_ctrl(const uint8_t* s, int n) {
    std::string out;
    char t[16];
    int i = 0;
    while (i < n) {
        uint8_t b = s[i];
        if (b == 0x00) break;
        if (b == 0x01) { out += "\\n"; i += 1; }
        else if (b==0x02||b==0x03||b==0x06||b==0x08||b==0x0B||b==0x0D) {
            snprintf(t, sizeof t, "[%02X]", b); out += t; i += 1;
        } else if (b == 0x0E) {                       // 0x0E 0x0F XX (end anim)
            if (i+2 < n && s[i+1]==0x0F) { snprintf(t,sizeof t,"[0E:0F:%02X]",s[i+2]); out+=t; i+=3; }
            else if (i+1 < n)            { snprintf(t,sizeof t,"[0E:%02X]",s[i+1]);    out+=t; i+=2; }
            else                         { out += "[0E]"; i += 1; }
        } else if (b==0x09||b==0x17||b==0x18) {        // 2-param
            if (i+2 < n)      { snprintf(t,sizeof t,"[%02X:%02X:%02X]",b,s[i+1],s[i+2]); out+=t; i+=3; }
            else if (i+1 < n) { snprintf(t,sizeof t,"[%02X:%02X]",b,s[i+1]);             out+=t; i+=2; }
            else              { snprintf(t,sizeof t,"[%02X]",b);                          out+=t; i+=1; }
        } else if (b == 0x14) {                        // face/portrait, var-length
            if (i+1 < n) {
                uint8_t p1 = s[i+1];
                if (p1 >= 0x80 && i+2 < n) { snprintf(t,sizeof t,"[14:%02X:%02X]",p1,s[i+2]); out+=t; i+=3; }
                else                       { snprintf(t,sizeof t,"[14:%02X]",p1);              out+=t; i+=2; }
            } else { out += "[14]"; i += 1; }
        } else if (b < 0x20) {                         // other 1-param control
            if (i+1 < n) { snprintf(t,sizeof t,"[%02X:%02X]",b,s[i+1]); out+=t; i+=2; }
            else         { snprintf(t,sizeof t,"[%02X]",b);             out+=t; i+=1; }
        } else if (b < 0x7F) { out += (char)b; i += 1; }         // plain ASCII
        else { snprintf(t,sizeof t,"<%02X>",b); out += t; i += 1; }  // high byte
    }
    return out;
}

// Print the buffer at `buf_va` to the debug console (once per distinct message —
// the page driver re-fires every frame while a dialog is up). Gated by the
// text_render_log config flag.
static uint64_t g_render_last_hash = 0;
static void render_log_capture(uint32_t buf_va, bool is_heap) {
    uint8_t buf[2048];
    int n = probe_bytes((const void*)(uintptr_t)buf_va, buf, (int)sizeof(buf));
    if (n <= 0) return;
    uint64_t h = 1469598103934665603ULL;             // FNV-1a
    for (int i = 0; i < n; ++i) { h ^= buf[i]; h *= 1099511628211ULL; }
    if (h == g_render_last_hash) return;             // same as last -> skip spam
    g_render_last_hash = h;
    std::string txt = decode_ctrl(buf, n);
    std::string hex; hex.reserve(n * 3);
    char t[8];
    for (int i = 0; i < n; ++i) { snprintf(t, sizeof t, "%02X ", buf[i]); hex += t; }
    hook_log("text_render_log: VA=0x%08X len=%d%s | %s | HEX: %s\n",
             buf_va, n, is_heap ? " [SUBST]" : "", txt.c_str(), hex.c_str());
}

extern "C" void __cdecl text_resolver_page_driver_detour(void) {
    uint64_t calls = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;

    // Read state[+0x0C] (page_start). If it already points at one of our
    // heap buffers (= we overrode it on a prior call), skip — looking up
    // our own replacement bytes would be wrong, and recursing would burn
    // memory and CPU.
    uint32_t orig_buf = seh_read_u32(DS_PAGE_START);
    bool orig_is_heap;
    {
        std::shared_lock lk(g_heap_mtx);
        orig_is_heap = g_heap_value_to_orig.find(orig_buf) != g_heap_value_to_orig.end();
    }
    // Diagnostic: print what's actually being rendered (source DAT bytes, or our
    // [SUBST] heap buffer when a live translation swapped in) to the console.
    if (g_cfg.text_render_log) render_log_capture(orig_buf, orig_is_heap);
    if (orig_is_heap) {
        g_orig_page_driver();
        return;
    }

    // Translation swap runs only under the real resolver config. When the hook
    // is installed purely for text_render_log (text_substitute/resolver off),
    // this is skipped and we only log above.
    const LiveEntry* le = (g_cfg.text_substitute_enabled &&
                           g_cfg.text_resolver_enabled)
                          ? lookup_match(orig_buf) : nullptr;
    if (le) {
        const HeapBuf* hb = get_or_make(orig_buf, le->orig, le->repl);
        if (hb) {
            uint32_t newptr = (uint32_t)(uintptr_t)hb->bytes.data();
            seh_write_u32(DS_PAGE_START, newptr);
            seh_write_u32(DS_CURSOR,     newptr);
            seh_write_u16(DS_LO16_TAG,   (uint16_t)(newptr & 0xFFFF));

            uint64_t swaps = g_swaps.fetch_add(1, std::memory_order_relaxed) + 1;
            if (swaps <= 8) {
                char op[64] = {}, np[64] = {};
                int onp = (int)(le->orig.size() < 60 ? le->orig.size() : 60);
                for (int i = 0; i < onp; ++i) {
                    uint8_t b = le->orig[i];
                    op[i] = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
                }
                op[onp] = 0;
                size_t nnp = le->repl.size() < 60 ? le->repl.size() : 60;
                for (size_t i = 0; i < nnp; ++i) {
                    uint8_t b = le->repl[i];
                    np[i] = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
                }
                np[nnp] = 0;
                hook_log("text_resolver: SWAP[%llu] orig=0x%08X heap=0x%08X "
                         "(%zu->%zu bytes) '%s' -> '%s'\n",
                         (unsigned long long)swaps, orig_buf, newptr,
                         le->orig.size(), le->repl.size(), op, np);
            }
        }
    }

    // Call the original page driver. It reads state[+0x0C] (now possibly
    // our heap pointer) and walks the typewriter from there.
    g_orig_page_driver();
}

// ── DIAGNOSTIC: first-chance crash logger (Dengeki multi-page substitution) ──
// Writes the faulting VA + instruction bytes + registers to dengeki_crash.log the
// first time an access violation fires, so we can pinpoint the engine function that
// crashes on a substituted multi-page ([02]) dialogue buffer. Writes directly with
// WriteFile (no hook_log/locks) so it is safe inside a VEH. Logs once, then lets the
// crash proceed (EXCEPTION_CONTINUE_SEARCH).
static volatile long g_crash_logged = 0;
static LONG WINAPI dengeki_crash_veh(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_PRIV_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW)
        return EXCEPTION_CONTINUE_SEARCH;                 // ignore benign/SEH probes
    if (InterlockedExchange(&g_crash_logged, 1)) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    uint32_t va = (uint32_t)(uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    uint8_t ib[16] = {0};
    __try { for (int i=0;i<16;++i) ib[i] = ((volatile uint8_t*)(uintptr_t)va)[i]; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    char buf[1024]; int n = 0;
    n += _snprintf(buf+n, sizeof buf-n,
        "\r\n=== DENGEKI CRASH ===\r\ncode=0x%08lX  faultVA=0x%08X\r\n", code, va);
    if (code==EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters>=2)
        n += _snprintf(buf+n, sizeof buf-n, "AV %s addr=0x%08X\r\n",
            ep->ExceptionRecord->ExceptionInformation[0] ? "WRITE":"READ",
            (uint32_t)ep->ExceptionRecord->ExceptionInformation[1]);
    n += _snprintf(buf+n, sizeof buf-n,
        "bytes @VA: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
        ib[0],ib[1],ib[2],ib[3],ib[4],ib[5],ib[6],ib[7],ib[8],ib[9],ib[10],ib[11]);
#ifdef _M_IX86
    n += _snprintf(buf+n, sizeof buf-n,
        "eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX\r\nesi=%08lX edi=%08lX ebp=%08lX esp=%08lX\r\n",
        c->Eax,c->Ebx,c->Ecx,c->Edx,c->Esi,c->Edi,c->Ebp,c->Esp);
#endif
    HANDLE h = CreateFileA("dengeki_crash.log", FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wr = 0; WriteFile(h, buf, (DWORD)n, &wr, nullptr); CloseHandle(h);
    }
    return EXCEPTION_CONTINUE_SEARCH;   // let the crash proceed after logging
}

void text_resolver_install() {
    // Install if the resolver is doing real translation work, OR just for the
    // diagnostic render log (which needs the page-driver hook to see buffers).
    bool for_subst = g_cfg.text_substitute_enabled && g_cfg.text_resolver_enabled;
    if (for_subst) {
        AddVectoredExceptionHandler(1, dengeki_crash_veh);  // DIAGNOSTIC (Dengeki)
        hook_log("text_resolver: crash logger armed -> dengeki_crash.log\n");
    }
    if (!for_subst && !g_cfg.text_render_log) {
        hook_log("text_resolver: text_substitute/resolver off and "
                 "text_render_log off — hook NOT installed\n");
        return;
    }
    if (!for_subst && g_cfg.text_render_log) {
        hook_log("text_resolver: installing for text_render_log ONLY "
                 "(no translation swap)\n");
    }
    uintptr_t addr = resolve_sig("text_resolver", SIG_PAGE_DRIVER,
                                 SIG_PAGE_DRIVER_MASK, sizeof(SIG_PAGE_DRIVER), 0);
    if (!addr) addr = FALLBACK_PAGE_DRIVER;
    MH_STATUS st = MH_CreateHook((LPVOID)addr,
                                 (LPVOID)text_resolver_page_driver_detour,
                                 (LPVOID*)&g_orig_page_driver);
    if (st != MH_OK) {
        hook_log("text_resolver: MH_CreateHook(0x%08X) failed: status=%d\n",
                 (unsigned)addr, (int)st);
        return;
    }
    st = MH_EnableHook((LPVOID)addr);
    if (st != MH_OK) {
        hook_log("text_resolver: MH_EnableHook failed: status=%d\n", (int)st);
        return;
    }
    hook_log("text_resolver: hook installed @ 0x%08X "
             "(page driver — covers both upstream resolver paths)\n",
             (unsigned)addr);
}
