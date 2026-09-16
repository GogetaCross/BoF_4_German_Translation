// Live text-substitution / text-trace hook on the BoF4 engine renderer
// at 0x00629470.
//
// Phase 1 (current): trace-only. When `text_substitute_log` is true, log
// the args of each render call to d3d9_hook.log so we can confirm which
// arg carries the string pointer before wiring substitution.
//
// Phase 2+ (future): build a hash-keyed translation table from
// translations\.live\*.bin and rewrite the string pointer on hits.
//
// Phase 3+ (future): F8 hotkey to rebuild the table from disk on demand.
#pragma once

#include <cstdint>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

// Install the MinHook trampoline on the engine text renderer.
// Idempotent. Safe to call after MH_Initialize() has succeeded.
// No-op (zero install cost) if both `text_substitute_enabled` and
// `text_substitute_log` are false in the config.
void text_subst_install();

// ── Live table (shared with text_resolver.cpp) ───────────────────────────
struct LiveEntry {
    std::vector<uint8_t> orig;
    std::vector<uint8_t> repl;
};
extern std::shared_mutex                                   g_live_mtx;
extern std::vector<LiveEntry>                              g_live_entries;
extern std::unordered_map<uint8_t, std::vector<uint32_t>>  g_live_index;
