// Menu-box width override, keyed on the DRAW-SITE CALLER — see box_autofit.h.
//
// Menu boxes are drawn by BOF4!0x00677E30, whose 3rd arg is the box WIDTH; the
// engine tiles the whole 9-slice (border + fill) to that width, so a box renders
// correctly at ANY width you pass it. This module hooks 0x677E30 and, for a box
// whose CALLER return address matches a rule in rect_widths.txt, overrides the
// width arg. The caller address is a fixed BOF4.exe .text address (no ASLR here),
// so a rule is STABLE across sessions — unlike the Rect Tuner's caller-chain sig.
//
// No text measurement needed (the game draws the box before measuring its text),
// so this sidesteps the ordering problem that killed the measure-based auto-fit.
//
// Workflow: enable box_autofit -> open a menu -> read the "[bw] box caller=0x.."
// line from d3d9_hook.log -> add "caller=0x.. width=N" to rect_widths.txt -> F5.

#include "hooks.h"
#include "hook_health.h"
#include "box_autofit.h"
#include "equip_menu_shift.h"   // F5 also re-applies the whole-menu equip shift

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <algorithm>
#include <mutex>
#include <unordered_set>
#include <windows.h>

extern "C" {
#include "minhook/MinHook.h"
}

static const uintptr_t ADDR_BOXDRAW  = 0x00677E30;   // 9-slice, width arg3 (fallback VA)
static const uintptr_t ADDR_BOXDRAW2 = 0x00677B50;   // 9-slice, width arg3 (busy, 140 callers)
static const uintptr_t ADDR_STRIP    = 0x00678890;   // horizontal tile-strip; count = (arg3>>8)&0xFF

// The two 9-slice drawers are located by prologue signature so the hooks
// survive EXE recompiles (verified relocating: E30 live 0x677E30 ->
// recompile 0x677CE0). BOXDRAW2 has a byte-identical twin elsewhere, so its
// signature reaches the one distinguishing global (0x95B6B9) and is thus
// live-unique; on a full recompile that global moves and the sig falls back
// to the hardcoded VA. NOTE: the caller-keyed width rules in rect_widths.txt
// are themselves build-specific .text addresses — after a recompile the user
// re-reads the [bw] log lines and updates those rules (existing workflow).
static const uint8_t SIG_BOXDRAW[] = {
    0x83,0xEC,0x24,0x8B,0x44,0x24,0x30,0x8B,0x54,0x24,0x34,
    0x81,0xE2,0xFF,0x00,0x00,0x00,0x53,0x8D,0x48,
};
static const char SIG_BOXDRAW_MASK[] = "xxxxxxxxxxxxxxxxxxxx";
static const uint8_t SIG_BOXDRAW2[] = {
    0x83,0xEC,0x1C,0x8B,0x4C,0x24,0x28,0x53,0x8B,0x5C,0x24,0x30,0x55,
    0x8D,0x41,0xF8,0x56,0x66,0x89,0x44,0x24,0x20,0x66,0x33,0xC0,0x8A,
    0xC3,0x33,0xED,0x83,0xC1,0xF0,0xBE,0xB9,0xB6,0x95,0x00,
};
static const char SIG_BOXDRAW2_MASK[] = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";

// Third drawer: the SELECTION-FRAME / highlight drawer 0x6786D0. Same arg layout
// as the 9-slice drawers (args[0]=x, args[1]=y, args[2]=width), so it reuses
// af_box_hook — this makes the menu selection-frame widths caller-keyed rules too.
static const uint8_t SIG_SELFRAME[] = {
    0x83,0xEC,0x0C,0x8B,0x4C,0x24,0x18,0xC6,0x44,0x24,0x1D,0xC0,0x66,0x89,0x4C,0x24,
};
static const char SIG_SELFRAME_MASK[] = "xxxxxxxxxxxxxxxx";
static const uintptr_t ADDR_SELFRAME_FB = 0x006786D0;

extern "C" void*    g_af_orig_box      = nullptr;
extern "C" void*    g_af_orig_box2     = nullptr;
extern "C" void*    g_af_orig_strip    = nullptr;
extern "C" void*    g_af_orig_selframe = nullptr;
extern "C" uint8_t  g_af_enabled       = 0;

struct WRule { uint32_t caller; int width; int maxonly; int count; int x; int y;
               int mw; int mx; int my;      // optional match filters (scope a shared caller)
               int dx; int dy; };           // relative nudge (F5-tunable): +x right, +y down
static std::vector<WRule> g_rules;
static std::mutex g_rules_mtx;

// Per-frame distinct box list for the F10 HUD. `strip` = drawn by 0x678890
// (value is a TILE COUNT, widen with count=); else `w` is a 9-slice pixel width.
//   w        = STOCK width the game pushed (arg3 before any override)
//   eff      = width actually passed on (stock -> rule -> live tune)
//   x, y     = position actually passed on (post rule dx/dy + live tune)
//   rule_w   = width from the rect_widths.txt rule, or -1 if this caller has none
//   rule_dx/dy = that rule's saved nudge, so a live nudge stacks on it correctly
//   plausible= arg3 looked like a real width. Implausible callers (garbage arg3,
//              e.g. several selection cursors) are still overridable and still
//              listed while the box tuner is open — that is how they get found.
struct BoxInfo { uint32_t caller; int x, y, w; int strip; int which;
                 int eff; int rule_w; int rule_dx, rule_dy; int plausible; };
static const int BOX_MAX = 96;
static BoxInfo g_frame[BOX_MAX];   static int g_frameN   = 0;   // accumulating this frame
static BoxInfo g_display[BOX_MAX]; static int g_displayN = 0;   // last frame's snapshot (stable for cycling)
static std::mutex g_frame_mtx;

// ── box tuner state (render thread only — see box_autofit.h) ───────────────
static bool     g_bt_active = false;
static uint32_t g_bt_sel    = 0;      // focused caller
static int      g_bt_dw = 0, g_bt_dx = 0, g_bt_dy = 0;   // unsaved live deltas

// FOCUS mode: show/highlight only ONE box at a time so you can pick the box you're
// navigating. g_bid_sel = caller of the focused box (0 = none/all). The focused
// box is visibly bulged (+HL) so you can see which physical box it is.
static volatile bool     g_bid_active = false;
static volatile uint32_t g_bid_sel    = 0;
static const int         BID_HL = 40;   // px the focused box grows, as a highlight

// cycle helpers operate on the last-frame snapshot (g_display), guarded by caller.
static void bid_cycle(int dir) {
    std::lock_guard<std::mutex> lk(g_frame_mtx);
    if (g_displayN == 0) return;
    int cur = -1;
    for (int i = 0; i < g_displayN; i++) if (g_display[i].caller == g_bid_sel) { cur = i; break; }
    int nxt = (cur < 0) ? 0 : ((cur + dir) % g_displayN + g_displayN) % g_displayN;
    g_bid_sel = g_display[nxt].caller;
}
static void bid_toggle() {
    g_bid_active = !g_bid_active;
    if (g_bid_active) {
        std::lock_guard<std::mutex> lk(g_frame_mtx);
        if (g_bid_sel == 0 && g_displayN > 0) g_bid_sel = g_display[0].caller;
    }
    hook_log("[bw] focus mode %s (sel caller=0x%08X)\n",
             g_bid_active ? "ON" : "OFF", (unsigned)g_bid_sel);
}

// Formats the boxes for the tuner HUD, then snapshots+clears the frame buffer.
extern "C" int box_autofit_hud_text(char* out, int cap) {
    std::lock_guard<std::mutex> lk(g_frame_mtx);
    // snapshot for cycling, then reset the accumulator
    g_displayN = g_frameN;
    for (int i = 0; i < g_frameN; i++) g_display[i] = g_frame[i];
    int len = 0;
    if (g_bid_active) {
        int idx = -1;
        for (int i = 0; i < g_frameN; i++) if (g_frame[i].caller == g_bid_sel) { idx = i; break; }
        len += snprintf(out + len, cap - len,
            "FOCUS BOX  [%d/%d]  (Ctrl+Numpad 4/6 cycle, 5 = show all)\n",
            idx < 0 ? 0 : idx + 1, g_frameN);
        if (idx >= 0) {
            const BoxInfo& b = g_frame[idx];
            len += snprintf(out + len, cap - len,
                "caller=0x%08X  @(%d,%d) %s=%d   <-- this box is bulged on screen\n"
                "put in rect_widths.txt:  caller=0x%08X %s=NN",
                b.caller, b.x, b.y, b.strip ? "tiles" : "w", b.w,
                b.caller, b.strip ? "count" : "width");
        } else
            len += snprintf(out + len, cap - len, "(focused box not on screen — cycle)");
    } else {
        len += snprintf(out + len, cap - len, "BOXES ON SCREEN  (Ctrl+Numpad 5 = focus one):");
        for (int i = 0; i < g_frameN && len < cap - 56; i++) {
            const BoxInfo& b = g_frame[i];
            len += snprintf(out + len, cap - len, "\ncaller=0x%08X  @(%d,%d) %s=%d",
                            b.caller, b.x, b.y, b.strip ? "tiles" : "w", b.w);
        }
        if (g_frameN == 0) len += snprintf(out + len, cap - len, "\n(no boxes seen this frame)");
    }
    g_frameN = 0;
    return len;
}

// ── sidecar rect_widths.txt ────────────────────────────────────────────────
// ONE combined sidecar: box_autofit reads the SAME rect_widths.txt that rect_hook
// uses. Box-drawer lines (caller=+width=/count=) are used here; per-tile lines
// (caller=+u=/v=/w=) are ignored here and handled by rect_hook. One file to edit.
static void bw_path(char* out, size_t n) {
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char* s = strrchr(exe, '\\'); if (s) *(s + 1) = 0; else exe[0] = 0;
    snprintf(out, n, "%srect_widths.txt", exe);
}
static void bw_load(std::vector<WRule>& out) {
    char path[MAX_PATH]; bw_path(path, sizeof(path));
    FILE* f = fopen(path, "r");
    if (!f) return;   // rect_hook owns creating rect_widths.txt; nothing to load yet
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* p = line; while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == 0) continue;
        WRule r{ 0, -1, 0, -1, INT32_MIN, INT32_MIN, -1, INT32_MIN, INT32_MIN, 0, 0 };
        char tok[64]; int adv;
        while (sscanf(p, "%63s%n", tok, &adv) == 1) {
            p += adv;
            char* eq = strchr(tok, '='); if (!eq) continue; *eq = 0;
            if      (!_stricmp(tok, "caller")) r.caller = (uint32_t)strtoul(eq + 1, nullptr, 0);
            else if (!_stricmp(tok, "width"))  r.width  = (int)strtol(eq + 1, nullptr, 0);
            else if (!_stricmp(tok, "max"))    r.maxonly = (int)strtol(eq + 1, nullptr, 0);
            else if (!_stricmp(tok, "count"))  r.count  = (int)strtol(eq + 1, nullptr, 0);
            else if (!_stricmp(tok, "boxx"))   r.x      = (int)strtol(eq + 1, nullptr, 0);
            else if (!_stricmp(tok, "boxy"))   r.y      = (int)strtol(eq + 1, nullptr, 0);
            else if (!_stricmp(tok, "mw"))     r.mw     = (int)strtol(eq + 1, nullptr, 0);
            else if (!_stricmp(tok, "mx"))     r.mx     = (int)strtol(eq + 1, nullptr, 0);
            else if (!_stricmp(tok, "my"))     r.my     = (int)strtol(eq + 1, nullptr, 0);
            else if (!_stricmp(tok, "dx"))     r.dx     = (int)strtol(eq + 1, nullptr, 0);
            else if (!_stricmp(tok, "dy"))     r.dy     = (int)strtol(eq + 1, nullptr, 0);
        }
        if (r.caller && (r.width >= 0 || r.count >= 0 ||
                         r.x != INT32_MIN || r.y != INT32_MIN || r.dx || r.dy))
            out.push_back(r);
    }
    fclose(f);
    hook_log("box_autofit: loaded %zu width rule(s) from rect_widths.txt\n", out.size());
}
void box_autofit_reload() {
    std::vector<WRule> fresh; bw_load(fresh);
    std::lock_guard<std::mutex> lk(g_rules_mtx); g_rules.swap(fresh);
}

// ── the hook body ─────────────────────────────────────────────────────────
// args[-1]=caller ret, args[0]=x, args[1]=y, args[2]=width(arg3).
extern "C" void __cdecl af_box_hook(uint32_t* args, int which) {
    // which: 0=0x677E30 box, 1=0x677B50 box, 2=0x6786D0 selection-frame/CURSOR.
    uint32_t caller = args[-1];
    int orig = (int)args[2];
    // Only touch calls that pass a PLAUSIBLE width as arg3 (some callers use
    // 0x677E30 with a non-width arg3 — skip those so we never corrupt them).
    bool plausible = (orig >= 4 && orig <= 1024);
    int rule_w = -1, rule_dx = 0, rule_dy = 0;
    // Apply focus-bulge / user width rule ALWAYS (user-specified — a rect_widths.txt
    // rule should take effect even if my width heuristic is unsure for that caller).
    // The old Ctrl+Numpad bulge stands down while the box tuner is driving this
    // caller — a +40 bulge would fight the width you are dialling in.
    if (g_bid_active && caller == g_bid_sel && !g_bt_active) {
        args[2] = (uint32_t)(orig + BID_HL);       // highlight the focused box (bulge)
    } else {
        std::lock_guard<std::mutex> lk(g_rules_mtx);
        for (const WRule& r : g_rules) {
            if (r.caller != caller) continue;
            // Optional scope filters — let one shared caller be targeted by the
            // specific box instance (match original width / x / y).
            if (r.mw >= 0        && orig        != r.mw) continue;
            if (r.mx != INT32_MIN && (int)args[0] != r.mx) continue;
            if (r.my != INT32_MIN && (int)args[1] != r.my) continue;
            if (r.width >= 0) {
                int nw = r.maxonly ? (r.width > orig ? r.width : orig) : r.width;
                args[2] = (uint32_t)nw;
                rule_w = nw;
            }
            if (r.x != INT32_MIN) args[0] = (uint32_t)r.x;   // absolute box X (left margin)
            if (r.y != INT32_MIN) args[1] = (uint32_t)r.y;   // absolute box Y
            // Relative nudge (F5-tunable). Applied on top of any absolute x/y.
            // Moves the box frame + cursor drawn by this caller; does NOT move
            // baked label text (that's a code-level shift, e.g. equip_menu_shift).
            if (r.dx) args[0] = (uint32_t)((int)args[0] + r.dx);
            if (r.dy) args[1] = (uint32_t)((int)args[1] + r.dy);
            rule_dx = r.dx; rule_dy = r.dy;
            break;
        }
    }
    // LIVE BOX TUNER: stack the unsaved delta on top of whatever the saved rule
    // produced, so what you see on screen is exactly what `save` will write.
    // Clamped so a wild scroll can't hand the drawer a nonsense width.
    if (g_bt_active && caller == g_bt_sel) {
        // ZERO DELTA MUST BE A NO-OP. Merely focusing a box may not change one
        // byte of what the drawer receives — the box list contains callers whose
        // arg3 is NOT a width (garbage/animated), and writing anything to those
        // corrupts a draw that was never a box. Only an actual keypress edits.
        if (g_bt_dw) {
            // Base = what the rule (or the game) already produced. EXCEPT for a
            // garbage-arg3 caller with no rule: that number is not a width, so
            // starting from it would put the first keypress at the clamp ceiling.
            // Seed those at 64 — a typical cursor width to dial from.
            int base = (rule_w < 0 && !plausible) ? 64 : (int)args[2];
            int w = base + g_bt_dw;
            if (w < 4) w = 4; if (w > 1024) w = 1024;
            args[2] = (uint32_t)w;
        }
        if (g_bt_dx) args[0] = (uint32_t)((int)args[0] + g_bt_dx);
        if (g_bt_dy) args[1] = (uint32_t)((int)args[1] + g_bt_dy);
    }
    // Record for the F10 focus list. Normally only calls whose arg3 looks like a
    // real width — but while the BOX TUNER is open, list the implausible ones too
    // (marked [?] in the HUD): several selection cursors push a garbage/animated
    // arg3 yet are still perfectly overridable, and that is how you find them.
    if (plausible || g_bt_active) {
        std::lock_guard<std::mutex> lk(g_frame_mtx);
        bool found = false;
        for (int i = 0; i < g_frameN; i++) if (g_frame[i].caller == caller) { found = true; break; }
        if (!found && g_frameN < BOX_MAX)
            g_frame[g_frameN++] = { caller, (int)args[0], (int)args[1], orig, 0, which,
                                    (int)args[2], rule_w, rule_dx, rule_dy, plausible ? 1 : 0 };
    }
    // VERBOSE diagnostic: log EVERY draw (no dedup) with real coords so a box that
    // only ever deduped on a bad frame can be identified. Capped so it can't run
    // away. Enable with box_autofit_log_all=true; disable after capturing.
    if (g_cfg.box_autofit_log_all) {
        // DEDUP: log each unique (caller,x,y,w,which) ONCE, not every frame — so the
        // log stays a clean unique set and never floods/caps (menus draw the same
        // boxes 60x/sec). Render-thread only, so no lock. Cap the set so a menu with
        // ever-changing garbage args can't grow it unbounded. Logs plausible AND
        // implausible calls (implausible = arg3 isn't a clean width, but override
        // still works — that's how we find garbage-arg cursors/boxes).
        static std::unordered_set<uint64_t> seen;
        uint64_t k = ((uint64_t)caller << 32)
                   ^ ((uint64_t)(uint32_t)args[0] * 0x9E3779B1u)
                   ^ ((uint64_t)(uint32_t)args[1] << 20)
                   ^ ((uint64_t)(uint32_t)orig)
                   ^ ((uint64_t)which << 3);
        if (seen.size() < 8000 && seen.insert(k).second) {
            hook_log("[bw-all] caller=0x%08X x=%d y=%d w=%d  drawer=%s%s\n",
                     caller, (int)args[0], (int)args[1], orig,
                     which == 2 ? "6786D0-CURSOR" : (which == 1 ? "677B50-box" : "677E30-box"),
                     plausible ? "" : "  <implausible-arg3>");
        }
    }
    // Diagnostic: log each distinct (caller, plausibility) once so boxes can be identified.
    uint32_t key = caller | (plausible ? 0x80000000u : 0u);
    static uint32_t seen[256]; static int nseen = 0;
    for (int i = 0; i < nseen; i++) if (seen[i] == key) return;
    if (nseen < 256) seen[nseen++] = key;
    hook_log("[bw] box caller=0x%08X x=%d y=%d w=%d%s\n",
             caller, (int)args[0], (int)args[1], orig,
             plausible ? "" : "  (arg3 not a width - not overridable)");
}

// Hook body for the tile-STRIP drawer 0x678890. "Width" here is a TILE COUNT in
// the high byte of arg3: count = (arg3>>8)&0xFF. We override that byte (keyed on
// caller, via `count=N` rules), record for the HUD (marked strip), and bulge on
// focus. Only touch plausible counts (1..255) so non-box strips are left alone.
extern "C" void __cdecl af_strip_hook(uint32_t* args) {
    uint32_t caller = args[-1];
    uint32_t arg3   = args[2];
    int cnt = (int)((arg3 >> 8) & 0xFF);
    if (cnt < 1) return;                     // count 0 = nothing drawn; ignore
    {   // record for HUD (distinct by caller), value = tile count, strip=1
        std::lock_guard<std::mutex> lk(g_frame_mtx);
        bool found = false;
        for (int i = 0; i < g_frameN; i++) if (g_frame[i].caller == caller) { found = true; break; }
        if (!found && g_frameN < BOX_MAX)
            g_frame[g_frameN++] = { caller, (int)args[0], (int)args[1], cnt, 1, 3,
                                    cnt, -1, 0, 0, 1 };
    }
    int newc = -1;
    if (g_bid_active && caller == g_bid_sel) {
        newc = cnt + 3;                      // highlight: +3 tiles so it clearly bulges
    } else {
        std::lock_guard<std::mutex> lk(g_rules_mtx);
        for (const WRule& r : g_rules) {
            if (r.caller != caller || r.count < 0) continue;
            newc = r.maxonly ? (r.count > cnt ? r.count : cnt) : r.count;
            break;
        }
    }
    if (newc >= 0) {
        if (newc > 255) newc = 255;
        args[2] = (arg3 & 0xFFFF00FFu) | ((uint32_t)(newc & 0xFF) << 8);  // only the count byte
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  BOX TUNER — the F10 tuner's box mode. See box_autofit.h for the contract.
//
//  This exists because the Rect Tuner CANNOT do this job. The rect tuner edits
//  one GP0 packet: the 9-slice's fill tiles and the semi-transparent dim are
//  separate packets that don't follow, and the selection-cursor drawer never
//  reaches it at all. Here we move the same lever a rect_widths.txt rule moves —
//  the drawer's own width/x/y args — so the whole box retiles the way the engine
//  intends, and cursors are just another caller.
// ═══════════════════════════════════════════════════════════════════════════

void box_autofit_on_present() {
    if (!g_af_enabled) return;
    std::lock_guard<std::mutex> lk(g_frame_mtx);
    // Keep the LAST NON-EMPTY frame. Menus have single frames where nothing is
    // drawn (transitions), and blanking the list there would drop your focus and
    // make the panel flicker. It also means a box stays tunable for a moment
    // after you leave its menu, which is handy, not harmful.
    if (g_frameN > 0) {
        g_displayN = g_frameN;
        for (int i = 0; i < g_frameN; i++) g_display[i] = g_frame[i];
        // Deterministic order for cycling. NOT spatial: several cursors push a
        // garbage/animated y, which would reshuffle the list every frame. The
        // caller address is a fixed .text address, so this order never moves.
        std::sort(g_display, g_display + g_displayN,
                  [](const BoxInfo& a, const BoxInfo& b) { return a.caller < b.caller; });
    }
    g_frameN = 0;
}

void box_tuner_set_active(bool on) {
    g_bt_active = on;
    g_bt_dw = g_bt_dx = g_bt_dy = 0;
    if (on) {
        std::lock_guard<std::mutex> lk(g_frame_mtx);
        // Land on a REAL box first. The list is sorted by caller address, and the
        // lowest one may well be a garbage-arg3 call — a confusing thing to open
        // on. Those stay reachable by cycling.
        if (g_bt_sel == 0 && g_displayN > 0) {
            g_bt_sel = g_display[0].caller;
            for (int i = 0; i < g_displayN; i++)
                if (g_display[i].plausible) { g_bt_sel = g_display[i].caller; break; }
        }
    }
    hook_log("[bt] box tuner %s (sel caller=0x%08X)\n", on ? "ON" : "OFF", (unsigned)g_bt_sel);
}
bool box_tuner_active() { return g_bt_active; }

void box_tuner_cycle(int dir) {
    std::lock_guard<std::mutex> lk(g_frame_mtx);
    if (g_displayN == 0) return;
    int cur = -1;
    for (int i = 0; i < g_displayN; i++) if (g_display[i].caller == g_bt_sel) { cur = i; break; }
    int nxt = (cur < 0) ? 0 : ((cur + dir) % g_displayN + g_displayN) % g_displayN;
    g_bt_sel = g_display[nxt].caller;
    g_bt_dw = g_bt_dx = g_bt_dy = 0;      // fresh box: drop the unsaved delta
}

void box_tuner_nudge(int dw, int dx, int dy) { g_bt_dw += dw; g_bt_dx += dx; g_bt_dy += dy; }

static bool bt_focus(BoxInfo& out) {
    std::lock_guard<std::mutex> lk(g_frame_mtx);
    for (int i = 0; i < g_displayN; i++)
        if (g_display[i].caller == g_bt_sel) { out = g_display[i]; return true; }
    return false;
}

bool box_tuner_focus_geom(int* x, int* y, int* w, int* ow) {
    BoxInfo b;
    if (!g_bt_active || !bt_focus(b)) return false;
    // Refuse to hand the overlay a nonsense span. Several callers push an
    // animated or garbage arg3/y; scaled into backbuffer space that becomes a
    // quad thousands of pixels across — exactly the kind of "graphics glitch" a
    // measuring marker has no business causing. Draw nothing instead.
    if (b.eff < 1 || b.eff > 1024) return false;
    if (b.x < -256 || b.x > 2048 || b.y < -256 || b.y > 2048) return false;
    if (x) *x = b.x; if (y) *y = b.y; if (w) *w = b.eff;
    // The stock span is only a reference line; if IT is garbage, fall back to the
    // live span so the dim bar simply sits under the bright one.
    if (ow) *ow = (b.w >= 1 && b.w <= 1024) ? b.w : b.eff;
    return true;
}

// ── rect_widths.txt round-trip ─────────────────────────────────────────────
// The file is HAND-WRITTEN and full of comments that explain each menu — so a
// save edits ONE line's tokens in place and leaves everything else byte-for-byte,
// trailing comment included. It never rewrites the file from parsed state.

static uint32_t bt_line_caller(const std::string& body) {
    const char* p = body.c_str();
    char tok[80]; int adv;
    while (sscanf(p, "%79s%n", tok, &adv) == 1) {
        p += adv;
        char* eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = 0;
        if (!_stricmp(tok, "caller")) return (uint32_t)strtoul(eq + 1, nullptr, 0);
    }
    return 0;
}

// Set / replace / remove `key=value` tokens in a rule body, preserving the order
// of every token we don't touch. An empty value removes the key.
static std::string bt_apply_tokens(const std::string& body,
        const std::vector<std::pair<std::string, std::string>>& kv) {
    std::vector<std::string> toks;
    std::string cur;
    for (char c : body) {
        if (c == ' ' || c == '\t') { if (!cur.empty()) { toks.push_back(cur); cur.clear(); } }
        else cur += c;
    }
    if (!cur.empty()) toks.push_back(cur);

    for (const auto& p : kv) {
        bool found = false;
        for (size_t i = 0; i < toks.size(); ) {
            std::string k = toks[i].substr(0, toks[i].find('='));
            if (_stricmp(k.c_str(), p.first.c_str()) == 0) {
                if (p.second.empty()) { toks.erase(toks.begin() + i); continue; }
                toks[i] = p.first + "=" + p.second;
                found = true;
            }
            ++i;
        }
        if (!found && !p.second.empty()) toks.push_back(p.first + "=" + p.second);
    }
    std::string out;
    for (size_t i = 0; i < toks.size(); i++) { if (i) out += " "; out += toks[i]; }
    return out;
}

// Split a raw file line into body / trailing comment / line ending.
static void bt_split(const std::string& raw, std::string& body,
                     std::string& comment, std::string& eol) {
    std::string s = raw;
    size_t e = s.size();
    while (e > 0 && (s[e - 1] == '\n' || s[e - 1] == '\r')) --e;
    eol = s.substr(e);
    if (eol.empty()) eol = "\n";
    s.resize(e);
    size_t h = s.find('#');
    if (h == std::string::npos) { comment.clear(); }
    else { comment = s.substr(h); s.resize(h); }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    body = s;
}

static std::string bt_join(const std::string& body, const std::string& comment,
                           const std::string& eol) {
    if (comment.empty()) return body + eol;
    std::string out = body;
    while (out.size() < 36) out += ' ';       // keep the file's comment column
    out += "  ";
    out += comment;
    return out + eol;
}

// remove=true comments the rule out (keeping its note) instead of writing values.
static bool bt_write_rule(uint32_t caller, int width, bool write_width,
                          int dx, int dy, bool remove) {
    char path[MAX_PATH]; bw_path(path, sizeof(path));

    // rect_widths.txt is hand-maintained and full of notes that took real work to
    // write. Keep ONE pristine copy from before this build ever touched it, so a
    // stray keypress in-game is always recoverable. FALSE = never overwrite it.
    {
        std::string bak = std::string(path) + ".bak_boxtuner";
        if (CopyFileA(path, bak.c_str(), TRUE))
            hook_log("[bt] first save this install — backed up rect_widths.txt to %s\n", bak.c_str());
    }

    std::vector<std::string> lines;
    FILE* f = fopen(path, "rb");
    if (f) {
        char ln[1024];
        while (fgets(ln, sizeof(ln), f)) lines.push_back(ln);
        fclose(f);
    }

    int active = -1, dormant = -1;
    for (int i = 0; i < (int)lines.size(); i++) {
        std::string body, comment, eol;
        bt_split(lines[i], body, comment, eol);
        const char* p = body.c_str();
        while (*p == ' ' || *p == '\t') ++p;
        if (*p) {                                    // a live rule line
            if (bt_line_caller(p) == caller) { active = i; break; }
            continue;
        }
        // A line we commented out earlier: "# caller=0x... width=NN  # note".
        // Revive it on the next save so the user's note comes back with it.
        if (dormant < 0 && !comment.empty()) {
            const char* c = comment.c_str();
            while (*c == '#' || *c == ' ' || *c == '\t') ++c;
            if (!strncmp(c, "caller=", 7) && bt_line_caller(c) == caller) dormant = i;
        }
    }

    if (remove) {
        if (active < 0) { hook_log("[bt] clear: no rule for caller=0x%08X\n", (unsigned)caller); return false; }
        std::string body, comment, eol;
        bt_split(lines[active], body, comment, eol);
        const char* p = body.c_str(); while (*p == ' ' || *p == '\t') ++p;
        lines[active] = std::string("#") + p +
                        (comment.empty() ? std::string("  # (cleared in-game)")
                                         : std::string("  ") + comment) + eol;
    } else {
        std::vector<std::pair<std::string, std::string>> kv;
        char num[24];
        if (write_width) { snprintf(num, sizeof(num), "%d", width); kv.push_back({ "width", num }); }
        snprintf(num, sizeof(num), "%d", dx); kv.push_back({ "dx", dx ? num : "" });
        snprintf(num, sizeof(num), "%d", dy); kv.push_back({ "dy", dy ? num : "" });

        int at = (active >= 0) ? active : dormant;
        if (at >= 0) {
            std::string body, comment, eol;
            bt_split(lines[at], body, comment, eol);
            std::string rule = body;
            if (active < 0) {                        // reviving: rule text is in the comment
                const char* c = comment.c_str();
                while (*c == '#' || *c == ' ' || *c == '\t') ++c;
                const char* note = strchr(c, '#');
                rule = note ? std::string(c, note - c) : std::string(c);
                comment = note ? std::string(note) : std::string();
                while (!rule.empty() && (rule.back() == ' ' || rule.back() == '\t')) rule.pop_back();
            }
            lines[at] = bt_join(bt_apply_tokens(rule, kv), comment, eol);
        } else {
            char head[40]; snprintf(head, sizeof(head), "caller=0x%08X", caller);
            std::string rule = bt_apply_tokens(head, kv);
            bool has_hdr = false;
            for (auto& l : lines) if (l.find("# --- added by the in-game Box Tuner") != std::string::npos) has_hdr = true;
            if (!lines.empty() && lines.back().find('\n') == std::string::npos) lines.back() += "\n";
            if (!has_hdr)
                lines.push_back("\n# --- added by the in-game Box Tuner (F10 -> box mode) ------------------\n");
            lines.push_back(bt_join(rule, "# tuned in-game", "\n"));
        }
    }

    // Write via a temp file + replace so an interrupted save can't truncate a
    // hand-maintained file.
    std::string tmp = std::string(path) + ".tmp";
    FILE* w = fopen(tmp.c_str(), "wb");
    if (!w) { hook_log("[bt] save: cannot write %s\n", tmp.c_str()); return false; }
    for (auto& l : lines) fputs(l.c_str(), w);
    fclose(w);
    if (!MoveFileExA(tmp.c_str(), path, MOVEFILE_REPLACE_EXISTING)) {
        hook_log("[bt] save: replace of %s failed (%lu)\n", path, GetLastError());
        DeleteFileA(tmp.c_str());
        return false;
    }
    return true;
}

bool box_tuner_save() {
    BoxInfo b;
    if (!bt_focus(b)) { hook_log("[bt] save: no box focused\n"); return false; }
    // b.eff already carries rule + live delta, and b.rule_dx/dy the rule's saved
    // nudge — so what the drawer got this frame is exactly what we persist.
    int dx = b.rule_dx + g_bt_dx, dy = b.rule_dy + g_bt_dy;
    bool write_width = (b.rule_w >= 0) || (g_bt_dw != 0);
    if (!bt_write_rule(b.caller, b.eff, write_width, dx, dy, false)) return false;
    hook_log("[bt] saved caller=0x%08X width=%d dx=%d dy=%d (stock %d) to rect_widths.txt\n",
             (unsigned)b.caller, b.eff, dx, dy, b.w);
    g_bt_dw = g_bt_dx = g_bt_dy = 0;      // the saved rule owns it now
    box_autofit_reload();
    return true;
}

bool box_tuner_clear() {
    BoxInfo b;
    if (!bt_focus(b)) return false;
    bool ok = bt_write_rule(b.caller, 0, false, 0, 0, true);
    g_bt_dw = g_bt_dx = g_bt_dy = 0;
    if (ok) { hook_log("[bt] cleared caller=0x%08X\n", (unsigned)b.caller); box_autofit_reload(); }
    return ok;
}

int box_tuner_hud_text(char* out, int cap) {
    std::lock_guard<std::mutex> lk(g_frame_mtx);
    int idx = -1;
    for (int i = 0; i < g_displayN; i++) if (g_display[i].caller == g_bt_sel) { idx = i; break; }
    int len = 0;
    if (idx < 0) {
        len += snprintf(out + len, cap - len,
            "BOX TUNER   %d box draw(s) this frame — cycle to focus one\n"
            "(open a menu first; the box list is built from the box/cursor drawers)",
            g_displayN);
        return len;
    }
    const BoxInfo& b = g_display[idx];
    const char* drv = (b.which == 2) ? "cursor 0x6786D0"
                    : (b.which == 1) ? "box 0x677B50" : "box 0x677E30";
    char rule[64];
    if (b.rule_w >= 0) snprintf(rule, sizeof(rule), "rule width=%d", b.rule_w);
    else               snprintf(rule, sizeof(rule), "no saved rule");
    len += snprintf(out + len, cap - len,
        "BOX TUNER   box %d/%d   %s%s\n"
        "caller=0x%08X   @(%d,%d)   stock w=%d  ->  NOW w=%d   [%s]\n"
        "unsaved: dw=%+d dx=%+d dy=%+d      saves as:  caller=0x%08X width=%d%s\n"
        "width -/+ : F/H or WHEEL = 1px   Ctrl+F/H = 10px   Shift = same as plain\n"
        "move box: T/G = y   Alt+wheel = x     save / clear / box_mode = back to rects",
        idx + 1, g_displayN, drv,
        b.plausible ? "" : "  [?] arg3 isn't a width — dialling from 64",
        (unsigned)b.caller, b.x, b.y, b.w, b.eff, rule,
        g_bt_dw, g_bt_dx, g_bt_dy, (unsigned)b.caller, b.eff,
        (b.rule_dx + g_bt_dx || b.rule_dy + g_bt_dy) ? " dx=.. dy=.." : "");
    return len;
}

// ── naked thunk ───────────────────────────────────────────────────────────
// Each thunk passes a drawer-id (2nd arg) so the log can tag CURSOR (0x6786D0)
// vs box draws. cdecl: 2nd arg pushed first, &arg1 pushed last; the extra push
// shifts the &arg1 offset by 4 (0x28 -> 0x2C). Caller cleans 8 bytes.
extern "C" __declspec(naked) void af_thunk_box() {
    __asm {
        pushad
        pushfd
        push 0                     // which = 0 (0x677E30 box)
        lea  eax, [esp + 0x2C]     // &arg1 (entry [esp+4] + pushad 32 + pushfd 4 + push 4)
        push eax
        call af_box_hook
        add  esp, 8
        popfd
        popad
        jmp  dword ptr [g_af_orig_box]
    }
}
extern "C" __declspec(naked) void af_thunk_box2() {
    __asm {
        pushad
        pushfd
        push 1                     // which = 1 (0x677B50 box)
        lea  eax, [esp + 0x2C]
        push eax
        call af_box_hook
        add  esp, 8
        popfd
        popad
        jmp  dword ptr [g_af_orig_box2]
    }
}
extern "C" __declspec(naked) void af_thunk_selframe() {
    __asm {
        pushad
        pushfd
        push 2                     // which = 2 (0x6786D0 selection frame / CURSOR)
        lea  eax, [esp + 0x2C]
        push eax
        call af_box_hook
        add  esp, 8
        popfd
        popad
        jmp  dword ptr [g_af_orig_selframe]
    }
}
extern "C" __declspec(naked) void af_thunk_strip() {
    __asm {
        pushad
        pushfd
        lea  eax, [esp + 0x28]
        push eax
        call af_strip_hook
        add  esp, 4
        popfd
        popad
        jmp  dword ptr [g_af_orig_strip]
    }
}

// ── F5 reload watcher ─────────────────────────────────────────────────────
static volatile bool g_bw_quit = false;
static DWORD WINAPI bw_reload_thread(LPVOID) {
    bool pF5 = false, pTog = false, pNext = false, pPrev = false;
    while (!g_bw_quit) {
        bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        bool f5   = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
        bool tog  = ctrl && (GetAsyncKeyState(VK_NUMPAD5) & 0x8000) != 0;   // toggle focus mode
        bool nxt  = ctrl && (GetAsyncKeyState(VK_NUMPAD6) & 0x8000) != 0;   // next box
        bool prv  = ctrl && (GetAsyncKeyState(VK_NUMPAD4) & 0x8000) != 0;   // prev box
        if (f5  && !pF5)  { box_autofit_reload(); equip_menu_shift_reload();
                            hook_log("[bw] F5: reloaded rect_widths.txt + equip_menu_shift\n"); }
        if (tog && !pTog) bid_toggle();
        if (nxt && !pNext) bid_cycle(+1);
        if (prv && !pPrev) bid_cycle(-1);
        pF5 = f5; pTog = tog; pNext = nxt; pPrev = prv;
        Sleep(40);
    }
    return 0;
}

// ── install ───────────────────────────────────────────────────────────────
void box_autofit_install(bool enabled, int /*padding*/) {
    if (!enabled) { hook_log("box_autofit: disabled — no hooks installed\n"); return; }
    g_af_enabled = 1;
    box_autofit_reload();
    uintptr_t va_box  = resolve_sig("box_autofit.E30", SIG_BOXDRAW,  SIG_BOXDRAW_MASK,  sizeof(SIG_BOXDRAW),  0);
    uintptr_t va_box2 = resolve_sig("box_autofit.B50", SIG_BOXDRAW2, SIG_BOXDRAW2_MASK, sizeof(SIG_BOXDRAW2), 0);
    uintptr_t va_self = resolve_sig("box_autofit.SEL", SIG_SELFRAME, SIG_SELFRAME_MASK, sizeof(SIG_SELFRAME), 0);
    if (!va_box)  va_box  = ADDR_BOXDRAW;
    if (!va_box2) va_box2 = ADDR_BOXDRAW2;
    if (!va_self) va_self = ADDR_SELFRAME_FB;
    struct { uintptr_t va; void* thunk; void** orig; const char* name; } sites[] = {
        { va_box,  (void*)af_thunk_box,      &g_af_orig_box,      "0x677E30 (9-slice)" },
        { va_box2, (void*)af_thunk_box2,     &g_af_orig_box2,     "0x677B50 (9-slice)" },
        { va_self, (void*)af_thunk_selframe, &g_af_orig_selframe, "0x6786D0 (sel-frame)" },
        // 0x678890 was NOT a box drawer — it draws the little menu-depth indicator
        // cubes in front of a label (count = number of cubes). Removed. (thunk/hook
        // kept as dead code but not installed.)
    };
    (void)ADDR_STRIP; (void)af_thunk_strip; (void)g_af_orig_strip;
    int ok = 0;
    for (auto& s : sites) {
        if (MH_CreateHook((LPVOID)s.va, s.thunk, s.orig) == MH_OK &&
            MH_EnableHook((LPVOID)s.va) == MH_OK) { ok++; }
        else hook_log("box_autofit: hook %s failed\n", s.name);
    }
    if (!ok) { hook_log("box_autofit: no box drawers hooked\n"); return; }
    CreateThread(nullptr, 0, bw_reload_thread, nullptr, 0, nullptr);
    hook_log("box_autofit: ENABLED (caller-keyed box widths). Open a menu, read [bw] "
             "lines, add caller/width rules to rect_widths.txt, F5 to reload.\n");
}
