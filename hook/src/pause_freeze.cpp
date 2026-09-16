// pause_freeze.cpp — see pause_freeze.h.
#include "pause_freeze.h"
#include "hooks.h"       // hook_log

#include <windows.h>
#include <cstdint>
#include <atomic>
#include <cstring>

namespace {
    // PSX main RAM: 2 MB. Runtime host base pointer lives at .data [0x009156D0]
    // (static init 0x006A8000). host = base + (psx - 0x80000000).
    constexpr size_t      PSX_SIZE      = 0x200000;
    volatile uint32_t* const PSX_BASE_PTR = (volatile uint32_t*)0x009156D0;

    std::atomic<bool> g_on{false};           // armed OR holding
    std::atomic<bool> g_need_snapshot{false};// armed, snapshot not yet taken
    std::atomic<bool> g_holding{false};      // snapshot taken → clock should hold
    uint8_t*          g_snap = nullptr;      // 2 MB save-state buffer
    uintptr_t         g_base = 0;

    uintptr_t resolve_base() {
        uint32_t b = 0;
        __try { b = *PSX_BASE_PTR; } __except (EXCEPTION_EXECUTE_HANDLER) { b = 0; }
        if (!b) b = 0x006A8000;   // static fallback
        return (uintptr_t)b;
    }

    // How many bytes at the TOP of PSX RAM to leave untouched (the emulated PSX
    // stack lives there; restoring it mid-frame corrupts the active call chain the
    // recompiled code is returning through → crash). Everything BELOW this is game
    // data/heap — the animation state we actually want frozen.
    size_t exclude_top_bytes() {
        int kb = g_cfg.pause_stack_kb;
        if (kb < 0)   kb = 0;
        if (kb > 1024) kb = 1024;          // never leave <1 MB pinned
        return (size_t)kb * 1024;
    }

    // SEH-guarded memcpy (foreign memory — never let a bad read/write crash).
    bool copy_guarded(void* dst, const void* src, size_t n) {
        __try { memcpy(dst, src, n); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
}

void pause_freeze_set(bool on) {
    if (on) {
        if (!g_snap) {
            g_snap = (uint8_t*)VirtualAlloc(nullptr, PSX_SIZE,
                                            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!g_snap) {
                hook_log("[pause-freeze] VirtualAlloc(2MB) FAILED — freeze disabled\n");
                return;
            }
        }
        g_holding.store(false);
        g_need_snapshot.store(true);
        g_on.store(true);
        hook_log("[pause-freeze] ARMED — snapshotting PSX RAM next frame\n");
    } else {
        g_on.store(false);
        g_holding.store(false);
        g_need_snapshot.store(false);
        hook_log("[pause-freeze] OFF — game state resumes\n");
    }
}

bool pause_freeze_is_on()   { return g_on.load(std::memory_order_relaxed); }
bool pause_freeze_holding() { return g_holding.load(std::memory_order_relaxed); }

void pause_freeze_on_present() {
    if (!g_on.load(std::memory_order_relaxed) || !g_snap) return;
    if (!g_base) g_base = resolve_base();

    const size_t keep = PSX_SIZE - exclude_top_bytes();  // bytes we snapshot/restore

    if (g_need_snapshot.load(std::memory_order_relaxed)) {
        // Capture the frame we froze on. This frame ran on the NORMAL clock, so
        // its stored "last frame time" is a real pre-freeze value — that's what
        // keeps dt (= held_clock - last) a positive constant once we start
        // holding. Every later frame is pinned to this snapshot. We DELIBERATELY
        // skip the top stack region (see exclude_top_bytes).
        if (copy_guarded(g_snap, (const void*)g_base, keep)) {
            g_need_snapshot.store(false, std::memory_order_relaxed);
            g_holding.store(true, std::memory_order_relaxed);  // clock hooks hold now
            hook_log("[pause-freeze] snapshot captured @0x%08X (%u/%u KB, top %u KB "
                     "left live) — holding\n", (unsigned)g_base,
                     (unsigned)(keep / 1024), (unsigned)(PSX_SIZE / 1024),
                     (unsigned)(exclude_top_bytes() / 1024));
        }
        return;
    }
    // Pin: restore game data/heap to the snapshot AFTER this frame presented and
    // BEFORE the next update — but NOT the top stack region, which the recompiled
    // code is actively unwinding through right now. Every update then starts from
    // the same data state and sees the same (held clock − snapshot's last) dt →
    // the rendered frame is identical → nothing moves, draws keep flowing, F10 live.
    copy_guarded((void*)g_base, (const void*)g_snap, keep);
}

void pause_freeze_shutdown() {
    g_on.store(false);
    g_holding.store(false);
    if (g_snap) { VirtualFree(g_snap, 0, MEM_RELEASE); g_snap = nullptr; }
}
