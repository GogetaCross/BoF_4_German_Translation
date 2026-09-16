// glyph_log — DIAGNOSTIC: identify which subsystem draws a given run of text.
//
// 0x006780E0 is the universal per-glyph metric/draw routine (shared by every
// text subsystem — dialog, menus, HUD labels). It is ALREADY hooked by the
// shipped `dialog_width` twin-substitute thunk (thunk_width_fn in
// bof4_hooks.cpp), which corrects umlaut widths. We must NOT install a second
// MinHook on the same address (that blocks dialog_width -> umlauts break).
// Instead, thunk_width_fn calls glyph_log_record() directly, once per glyph.
//
// CUMULATIVE UNIQUE census. A per-frame "latest snapshot" kept catching only
// the world-map destination GRID (ra 0x682FDE), which redraws every frame,
// while the location-name label draws only briefly when it pops up. So instead
// we log every DISTINCT (ra, arg0..arg3) tuple exactly once, appending to
// glyph_metric.log. The grid then contributes ~12 lines and goes quiet; a label
// that appears even for a single frame is recorded permanently. Move the cursor
// around towns so each name label pops up at least once, then quit.
//
// The log path is ABSOLUTE (next to the EXE): the game changes its working
// directory after startup, so a relative fopen would land in the wrong place.
//
// Config key: glyph_metric_log = false (default). Needs no console.

#include "hooks.h"
#include "glyph_log.h"

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <cerrno>
#include <unordered_set>

static const int  MAX_UNIQUE = 20000;    // safety cap on distinct lines
static std::unordered_set<uint64_t> g_seen;
static FILE*      g_fp    = nullptr;
static int        g_count = 0;
static bool       g_first = true;
static std::mutex g_mtx;
static char       g_path[MAX_PATH] = {0};

static void resolve_path() {
    char exe[MAX_PATH];
    if (GetModuleFileNameA(nullptr, exe, MAX_PATH) == 0) { strcpy_s(g_path, "glyph_metric.log"); return; }
    char* s = strrchr(exe, '\\'); if (s) *(s + 1) = 0; else exe[0] = 0;
    snprintf(g_path, sizeof(g_path), "%sglyph_metric.log", exe);
}

// Called once per glyph from the shipped thunk_width_fn (bof4_hooks.cpp), BEFORE
// the umlaut substitution, with the caller's return address and a pointer to the
// argument list (args[0..]). Different call sites pack coords/char differently,
// so all args are logged raw for offline decoding.
extern "C" void __cdecl glyph_log_record(unsigned ra, unsigned* args) {
    if (!g_cfg.glyph_metric_log) return;
    std::lock_guard<std::mutex> lk(g_mtx);

    if (g_first) {
        g_first = false;
        if (!g_path[0]) resolve_path();
        g_fp = fopen(g_path, "w");
        if (!g_fp) { hook_log("glyph_log: FAILED to open %s (errno=%d)\n", g_path, errno); return; }
        fprintf(g_fp,
            "# 0x006780E0 per-glyph draws — cumulative UNIQUE (ra, arg0..arg3) tuples.\n"
            "# Each label = a run sharing one 'ra'. Grid/HUD that redraws every frame\n"
            "# appears once. Move cursor over towns so each name label pops up.\n"
            "#  idx   ra         arg0       arg1       arg2       arg3       arg4       arg5     a3lo\n");
        fflush(g_fp);
        hook_log("glyph_log: recording unique glyph draws to %s (ra=0x%06X first)\n", g_path, ra);
    }
    if (!g_fp || g_count >= MAX_UNIQUE) return;

    // Dedup key: ra + the four canonical args (coords/char live among these).
    uint64_t h = 1469598103934665603ULL;
    unsigned parts[5] = { ra, args[0], args[1], args[2], args[3] };
    for (unsigned p : parts) { h ^= p; h *= 1099511628211ULL; }
    if (!g_seen.insert(h).second) return;          // already logged this tuple

    unsigned a3lo = args[3] & 0xFF;
    char asc = (a3lo >= 0x20 && a3lo < 0x7F) ? (char)a3lo : '.';
    fprintf(g_fp, "  %-4d  0x%06X  0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X  %02X '%c'\n",
            g_count, ra, args[0], args[1], args[2], args[3], args[4], args[5], a3lo, asc);
    fflush(g_fp);
    g_count++;
}

void glyph_log_install() {
    if (!g_cfg.glyph_metric_log) return;
    resolve_path();
    hook_log("glyph_log: enabled — cumulative unique glyph-draw census to\n"
             "           %s (logs from dialog_width thunk, no extra hook)\n", g_path);
}
