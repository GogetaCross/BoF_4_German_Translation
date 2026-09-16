// BoF4 runtime code hooks.
//
// Infrastructure for MinHook-based inline function hooking inside
// BOF4.exe's process. This complements the d3d9 vtable hooks in hooks.cpp
// (which intercept D3D9 COM calls).
//
// v0.8 hooks installed:
//   - kernel32!CreateFileA   (log DAT/CFG opens, start handle tracking)
//   - kernel32!CreateFileW   (same, wide-char variant)
//   - kernel32!ReadFile      (log every read on a tracked handle)
//   - kernel32!SetFilePointer(log seeks on a tracked handle)
//   - kernel32!CloseHandle   (log close, drop from tracking map)
//
// A HANDLE -> {filename, current position} map lets every log line show
// which DAT/CFG was read, at what offset, and how many bytes. Filtering
// is done at CreateFile time so only "interesting" handles end up in the
// map — ReadFile and CloseHandle log nothing for the countless pipes,
// events, mutexes, etc. that the runtime also manages.

#include "hooks.h"
#include <windows.h>
#include <mmsystem.h>   // timeGetTime prototype (via winmm.lib)
#include <intrin.h>     // _ReturnAddress intrinsic
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// MinHook is C — include via extern "C".
extern "C" {
#include "minhook/MinHook.h"
}

#include "pause_freeze.h" // "real pause" via per-frame PSX-RAM save-state
#include "bgm_loop.h"  // BGM loop fix — honors col2_param loop points (dshow hooks)
#include "ff_overlay.h" // top-right fast-forward speed badge
#include "free_camera.h" // EXPERIMENTAL free field-camera rotation (GOG only)
#include "text_subst.h"     // Live text-substitution hook (renderer 0x00629470)
#include "text_resolver.h"  // Resolver hook on 0x00527970 (Phase 3)
#include "item_name_uncap.h" // Two-byte patch lifting [09] 12-char cap
#include "name_slot_bp.h"    // TEMP diagnostic: [07] item-name scratch write-watch
#include "item_name_msg_uncap.h" // Lift 14-char [07] message item-name cap
#include "equip_menu_shift.h" // Shift equip-skill Menü box+labels+cursor left
#include "save_anywhere.h"   // One-byte patch unlocking pause-menu Save
#include "hook_health.h"     // health_check_bytes() + end-of-init report
#include "hang_watchdog.h"   // dump every thread's position if the game stops presenting
#include "subtitles.h"       // soft-subtitle overlay for the intro FMV
#include "title_jp.h"        // Japanese title card on the Western build
#include "menu_rect_fix.h"   // un-clip German title-menu labels (.text imm patch)
#include "rect_hook.h"       // universal textured-rect inventory + un-clip (0x502070)
#include "rect_tuner.h"      // in-game visual rect editor (F10) — rides the 0x502070 hook
#include "tex_prov.h"        // DIAGNOSTIC: texture-upload provenance logger (LoadImage 0x4026E0)
#include "glyph_log.h"       // DIAGNOSTIC: per-glyph draw logger (0x006780E0), hold F6
#include "label_center.h"     // Permanent fix: center world-map location-name labels
#include "box_autofit.h"     // Auto-grow menu boxes to fit measured text (0x677E30 / 0x629E90)
#include "cmdbox_width.h"    // One-byte patch widening the field command menu box (0x0067D03B)
#include "cmdbox_asm.h"      // German SGAMEN .ASM applied to PSX RAM (execution test)
#include "cmdbox_labels.h"   // shift field cmd-menu label text + header graphic (data patch)
#include "recon_scan.h"      // Read-only Steam/Enigma viability probe (recon_scan=1)
#include "censor_fix_aream031.h"  // Restore NA-cut AREAM031 scene (B1' PC-remap + codecave)
#include "narration_uncap.h"      // Lift typewriter char-caps on AREAD152 ending narration
#include "dengeki_unlock.h"        // Restore the Japan-only Dengeki Store bonus area

// ── Feature flag storage (declared in hooks.h, defined here) ──────────────
HookConfig g_cfg      = {};   // all fields zero-init (false/0)
bool       g_ff_active = false;

// ── Fast-forward speed CYCLE (F12 steps 1x -> 2x -> 4x -> 8x -> 16x -> 1x) ──
// The active multiplier is chosen from this ladder rather than a single fixed
// scale. g_cfg.fast_forward_scale acts as the CAP: steps above it are dropped,
// so `fast_forward_scale = 8` cycles 1/2/4/8 and back. 1x means "off".
static std::atomic<float> g_ff_cur_scale{1.0f};   // current active multiplier
static float g_ff_steps[8];
static int   g_ff_nsteps = 0;
static int   g_ff_idx    = 0;

// PAUSE toggle — shares g_ff_cur_scale (the master scale). Press = drop to
// pause_scale (heavy slow-mo, NOT 0: the F10 tuner needs the game to keep
// re-submitting draws); press again = back to 1x.
static bool g_paused = false;

static inline float ff_scale() { return g_ff_cur_scale.load(std::memory_order_relaxed); }

// g_ff_cur_scale is the ONE master virtual-time scale shared by fast-forward
// (>1) and slow-mo/pause (<1, or 0 = freeze). The clock-scaling hooks apply
// whenever it's != 1.0; the wait-SKIP hooks only apply for >1 (speed-up).
static inline bool time_scaling_live() {
    return (g_cfg.fast_forward_enabled || g_cfg.pause_slowmo);
}

static void ff_build_steps() {
    static const float LADDER[] = { 1.0f, 2.0f, 4.0f, 8.0f, 16.0f };
    g_ff_nsteps = 0;
    float cap = g_cfg.fast_forward_scale;
    if (cap < 1.0f) cap = 1.0f;
    for (float s : LADDER)
        if (s <= cap + 0.01f && g_ff_nsteps < (int)(sizeof(g_ff_steps)/sizeof(g_ff_steps[0])))
            g_ff_steps[g_ff_nsteps++] = s;
    if (g_ff_nsteps == 0) g_ff_steps[g_ff_nsteps++] = 1.0f;
    g_ff_idx = 0;
    g_ff_cur_scale.store(1.0f, std::memory_order_relaxed);
    g_ff_active = false;
}

// ── Fast-forward: virtual-time offset for GetTickCount hook ───────────────
// When FF is active, each real-time delta is multiplied by (scale-1) and
// added to g_ff_offset. Virtual time reported to the game is always
// real_time + g_ff_offset — monotonic, continuous across toggles.
static DWORD g_ff_offset    = 0;
static DWORD g_ff_last_real = 0;
static bool  g_ff_time_init = false;

// Save-state PAUSE clock-hold. While pause_freeze_holding() the reported clock is
// pinned to a constant a hair (~1 frame) ABOVE what it was when the snapshot was
// taken, so the game's dt (= held − snapshot's stored last) is a positive constant
// → it keeps re-rendering (F10 stays live) but the state, restored each frame, is
// identical. ~34ms guarantees dt>0 even if the game stores last=now.
static const DWORD PAUSE_FRAME_MS = 34;   // ~1 frame @30fps
static bool  g_hold_init = false;
static DWORD g_hold_tick = 0;

typedef DWORD (WINAPI *GetTickCount_t)(void);
static GetTickCount_t g_orig_GetTickCount = nullptr;
static std::atomic<uint64_t> g_tick_call_count{0};

static DWORD WINAPI hook_GetTickCount(void) {
    DWORD now = g_orig_GetTickCount ? g_orig_GetTickCount() : GetTickCount();
    if (!g_ff_time_init) {
        g_ff_last_real = now;
        g_ff_time_init = true;
    }
    DWORD delta = now - g_ff_last_real;   // wraps correctly on 49.7-day overflow
    g_ff_last_real = now;
    float s = ff_scale();
    // Advance the virtual clock at s× the real rate. s>1 fast-forwards; s<1 slows.
    if (time_scaling_live() && s != 1.0f) {
        g_ff_offset += (DWORD)(int32_t)lroundf(delta * (s - 1.0f));
    }
    g_tick_call_count.fetch_add(1, std::memory_order_relaxed);
    DWORD reported = now + g_ff_offset;
    // PAUSE freeze: hold the clock constant once the snapshot exists.
    if (pause_freeze_holding()) {
        if (!g_hold_init) { g_hold_tick = reported + PAUSE_FRAME_MS; g_hold_init = true; }
        return g_hold_tick;
    }
    if (g_hold_init) {   // just released — rebase offset so time doesn't jump
        g_ff_offset = g_hold_tick - now;
        g_hold_init = false;
        reported = now + g_ff_offset;
    }
    return reported;
}

// MsgWaitForMultipleObjects — BoF4 imports this; likely used to wait for
// the vsync event between frames. When FF is active, short-circuit the
// wait so the game's main loop iterates as fast as the CPU allows.
typedef DWORD (WINAPI *MsgWaitForMultipleObjects_t)(
    DWORD, const HANDLE*, BOOL, DWORD, DWORD);
static MsgWaitForMultipleObjects_t g_orig_MsgWait = nullptr;
static std::atomic<uint64_t> g_mwait_call_count{0};
static std::atomic<uint64_t> g_mwait_skipped{0};

static DWORD WINAPI hook_MsgWaitForMultipleObjects(
    DWORD count, const HANDLE* handles, BOOL waitAll,
    DWORD ms, DWORD mask)
{
    g_mwait_call_count.fetch_add(1, std::memory_order_relaxed);
    if (g_ff_active && g_cfg.fast_forward_enabled && ms > 0
        && ms != INFINITE) {
        g_mwait_skipped.fetch_add(1, std::memory_order_relaxed);
        ms = 0;
    }
    return g_orig_MsgWait(count, handles, waitAll, ms, mask);
}

// ── Sleep + WaitForSingleObject: diagnostic + short-wait skip ────────────
// Used by the DDraw wrapper (and some older DX apps) to pace frames.
// Only short waits (1..100 ms) get short-circuited — INFINITE and longer
// timeouts are left alone so thread synchronization isn't broken.
typedef VOID  (WINAPI *Sleep_t)(DWORD);
typedef DWORD (WINAPI *WaitForSingleObject_t)(HANDLE, DWORD);
typedef DWORD (WINAPI *WaitForMultipleObjects_t)(
    DWORD, const HANDLE*, BOOL, DWORD);

static Sleep_t                   g_orig_Sleep   = nullptr;
static WaitForSingleObject_t     g_orig_WFSO    = nullptr;
static WaitForMultipleObjects_t  g_orig_WFMO    = nullptr;

static std::atomic<uint64_t> g_sleep_call_count{0};
static std::atomic<uint64_t> g_sleep_skipped{0};
static std::atomic<uint64_t> g_wfso_call_count{0};
static std::atomic<uint64_t> g_wfso_skipped{0};
static std::atomic<uint64_t> g_wfmo_call_count{0};
static std::atomic<uint64_t> g_wfmo_skipped{0};

static inline bool ff_should_skip_wait(DWORD ms) {
    // Skip only typical frame-pacing intervals. Preserves long waits
    // used for thread sync / file I/O.
    return g_ff_active && g_cfg.fast_forward_enabled
        && ms > 0 && ms <= 100;
}

static void WINAPI hook_Sleep(DWORD ms) {
    g_sleep_call_count.fetch_add(1, std::memory_order_relaxed);
    if (ff_should_skip_wait(ms)) {
        g_sleep_skipped.fetch_add(1, std::memory_order_relaxed);
        ms = 0;
    }
    g_orig_Sleep(ms);
}

static DWORD WINAPI hook_WaitForSingleObject(HANDLE h, DWORD ms) {
    g_wfso_call_count.fetch_add(1, std::memory_order_relaxed);
    if (ff_should_skip_wait(ms)) {
        g_wfso_skipped.fetch_add(1, std::memory_order_relaxed);
        ms = 0;
    }
    return g_orig_WFSO(h, ms);
}

static DWORD WINAPI hook_WaitForMultipleObjects(
    DWORD count, const HANDLE* handles, BOOL waitAll, DWORD ms)
{
    g_wfmo_call_count.fetch_add(1, std::memory_order_relaxed);
    if (ff_should_skip_wait(ms)) {
        g_wfmo_skipped.fetch_add(1, std::memory_order_relaxed);
        ms = 0;
    }
    return g_orig_WFMO(count, handles, waitAll, ms);
}

// ── timeGetTime (winmm) + QueryPerformanceCounter (kernel32) ─────────────
// The GoG ddraw wrapper imports both. Either could drive its internal
// frame pacing. We hook them diagnostically AND scale their output when
// FF is active so whatever the wrapper uses becomes time-warpable.
typedef DWORD (WINAPI *timeGetTime_t)(void);
typedef BOOL  (WINAPI *QueryPerformanceCounter_t)(LARGE_INTEGER*);

static timeGetTime_t              g_orig_timeGetTime = nullptr;
static QueryPerformanceCounter_t  g_orig_QPC         = nullptr;

static std::atomic<uint64_t> g_tgt_call_count{0};
static std::atomic<uint64_t> g_qpc_call_count{0};

// timeGetTime: same shape as GetTickCount (DWORD ms). Apply the same
// monotonic virtual-time offset.
static DWORD WINAPI hook_timeGetTime(void) {
    DWORD now = g_orig_timeGetTime ? g_orig_timeGetTime() : timeGetTime();
    g_tgt_call_count.fetch_add(1, std::memory_order_relaxed);
    // Held to the same constant as GetTickCount while paused (offset + rebase are
    // maintained by the GetTickCount hook, which the game calls every frame).
    if (pause_freeze_holding() && g_hold_init) return g_hold_tick;
    return now + g_ff_offset;
}

// QueryPerformanceCounter: 64-bit high-res counter. We need a separate
// offset in QPC ticks. The offset grows per-call as (scale-1) * delta,
// mirroring the GetTickCount approach but with higher resolution.
static std::atomic<int64_t> g_qpc_offset{0};
static int64_t g_qpc_last_real = 0;
static bool    g_qpc_init      = false;
static bool    g_qpc_hold_init = false;
static int64_t g_qpc_hold      = 0;
static int64_t g_qpc_freq      = 0;   // ticks/sec (for the ~1-frame margin)

static BOOL WINAPI hook_QueryPerformanceCounter(LARGE_INTEGER* out) {
    BOOL ok = g_orig_QPC(out);
    g_qpc_call_count.fetch_add(1, std::memory_order_relaxed);
    if (!ok || !out) return ok;

    int64_t now = out->QuadPart;
    if (!g_qpc_init) {
        g_qpc_last_real = now;
        g_qpc_init = true;
    }
    int64_t delta = now - g_qpc_last_real;
    g_qpc_last_real = now;
    float s = ff_scale();
    if (time_scaling_live() && s != 1.0f && delta > 0) {
        int64_t extra = (int64_t)((double)delta * (double)(s - 1.0f));
        g_qpc_offset.fetch_add(extra, std::memory_order_relaxed);
    }
    int64_t reported = now + g_qpc_offset.load(std::memory_order_relaxed);
    // PAUSE freeze: hold constant a ~1-frame margin above the snapshot instant.
    if (pause_freeze_holding()) {
        if (!g_qpc_hold_init) {
            if (!g_qpc_freq) { LARGE_INTEGER f; if (QueryPerformanceFrequency(&f)) g_qpc_freq = f.QuadPart; }
            g_qpc_hold = reported + (g_qpc_freq ? g_qpc_freq / 30 : 0);
            g_qpc_hold_init = true;
        }
        out->QuadPart = g_qpc_hold;
        return ok;
    }
    if (g_qpc_hold_init) {   // just released — rebase so time doesn't jump
        g_qpc_offset.store(g_qpc_hold - now, std::memory_order_relaxed);
        g_qpc_hold_init = false;
        reported = now + g_qpc_offset.load(std::memory_order_relaxed);
    }
    out->QuadPart = reported;
    return ok;
}

// Dedicated hotkey-polling thread. Runs independently of any rendering
// path — needed because the GoG ddraw wrapper doesn't reliably route
// through IDirect3DDevice9::Present, so hooking Present alone can miss
// frames entirely. A separate thread using GetAsyncKeyState works
// whether or not Present fires.
static HANDLE g_ff_thread = nullptr;
static volatile bool g_ff_thread_quit = false;

// XInput (gamepad) for the FF cycle — dynamically loaded so there's no hard
// dependency. Reads PHYSICAL controller state only (never consumes input, and is
// unaffected by the game's in-game button remapping). The button is configurable
// via fast_forward_pad_button ("L1","R1","L3","R3","BACK","START","A".."Y","DUP".."DRIGHT",
// "L2","R2", or a combo like "L1+R1").
namespace {
    struct FF_XI_GP { WORD wButtons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
    struct FF_XI_ST { DWORD pkt; FF_XI_GP gp; };
    typedef DWORD (WINAPI *FF_XInputGetState_t)(DWORD, FF_XI_ST*);
    FF_XInputGetState_t g_ff_xinput = nullptr;
    WORD g_ff_pad_mask = 0;   // required wButtons bits (ANDed)
    int  g_ff_pad_trig = 0;   // 0 none, 1 = LT/L2, 2 = RT/R2 (also required)

    void ff_load_xinput() {
        const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
        for (const char* n : dlls) {
            HMODULE h = LoadLibraryA(n);
            if (h) { g_ff_xinput = (FF_XInputGetState_t)GetProcAddress(h, "XInputGetState");
                     if (g_ff_xinput) return; }
        }
    }
    // One token -> a button bit (returns mask) or sets trigger (*trig). 0 = unknown.
    WORD ff_pad_token(const char* t, int* trig) {
        struct { const char* n; WORD m; } M[] = {
            {"L1",0x0100},{"LB",0x0100},{"LEFTSHOULDER",0x0100},
            {"R1",0x0200},{"RB",0x0200},{"RIGHTSHOULDER",0x0200},
            {"L3",0x0040},{"LEFTTHUMB",0x0040},{"LS",0x0040},
            {"R3",0x0080},{"RIGHTTHUMB",0x0080},{"RS",0x0080},
            {"BACK",0x0020},{"SELECT",0x0020},{"VIEW",0x0020},{"SHARE",0x0020},
            {"START",0x0010},{"OPTIONS",0x0010},{"MENU",0x0010},
            {"A",0x1000},{"CROSS",0x1000},{"B",0x2000},{"CIRCLE",0x2000},
            {"X",0x4000},{"SQUARE",0x4000},{"Y",0x8000},{"TRIANGLE",0x8000},
            {"DUP",0x0001},{"DDOWN",0x0002},{"DLEFT",0x0004},{"DRIGHT",0x0008},
        };
        for (auto& e : M) if (!_stricmp(t, e.n)) return e.m;
        if (!_stricmp(t,"L2")||!_stricmp(t,"LT")||!_stricmp(t,"LEFTTRIGGER"))  { *trig = 1; return 0; }
        if (!_stricmp(t,"R2")||!_stricmp(t,"RT")||!_stricmp(t,"RIGHTTRIGGER")) { *trig = 2; return 0; }
        return 0;
    }
    void ff_parse_pad_button() {
        g_ff_pad_mask = 0; g_ff_pad_trig = 0;
        const char* s = g_cfg.fast_forward_pad_button[0] ? g_cfg.fast_forward_pad_button : "L1";
        char buf[16]; strncpy_s(buf, sizeof(buf), s, _TRUNCATE);
        for (char* p = buf; *p; ) {
            while (*p==' '||*p=='+') ++p;
            char* tok = p;
            while (*p && *p!='+' && *p!=' ') ++p;
            char save = *p; *p = 0;
            if (tok[0]) g_ff_pad_mask |= ff_pad_token(tok, &g_ff_pad_trig);
            *p = save;
        }
        hook_log("bof4: FF gamepad button '%s' -> mask=0x%04X trig=%d\n",
                 s, g_ff_pad_mask, g_ff_pad_trig);
    }
    bool ff_pad_down() {
        if (!g_ff_xinput || (!g_ff_pad_mask && !g_ff_pad_trig)) return false;
        FF_XI_ST st{};
        for (DWORD i = 0; i < 4; ++i) {
            if (g_ff_xinput(i, &st) != 0) continue;   // not this slot
            bool ok = true;
            if (g_ff_pad_mask) ok &= ((st.gp.wButtons & g_ff_pad_mask) == g_ff_pad_mask);
            if (g_ff_pad_trig == 1) ok &= (st.gp.lt > 30);
            if (g_ff_pad_trig == 2) ok &= (st.gp.rt > 30);
            return ok;
        }
        return false;
    }
}

// ── FF gamepad button live remap (for the in-game config overlay) ──────────
// Defined at file scope (not in the anonymous namespace) so config_overlay.cpp
// can call them; they reach the file-local XInput state above.
bool ff_pad_capture(char* out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = 0;
    if (!g_ff_xinput) ff_load_xinput();
    if (!g_ff_xinput) return false;
    struct { WORD m; const char* n; } R[] = {
        {0x0100,"L1"},{0x0200,"R1"},{0x0040,"L3"},{0x0080,"R3"},
        {0x0020,"BACK"},{0x0010,"START"},
        {0x1000,"A"},{0x2000,"B"},{0x4000,"X"},{0x8000,"Y"},
        {0x0001,"DUP"},{0x0002,"DDOWN"},{0x0004,"DLEFT"},{0x0008,"DRIGHT"},
    };
    FF_XI_ST st{};
    for (DWORD i = 0; i < 4; ++i) {
        if (g_ff_xinput(i, &st) != 0) continue;   // slot not connected
        for (auto& e : R) if (st.gp.wButtons & e.m) { strncpy_s(out, cap, e.n, _TRUNCATE); return true; }
        if (st.gp.lt > 30) { strncpy_s(out, cap, "L2", _TRUNCATE); return true; }
        if (st.gp.rt > 30) { strncpy_s(out, cap, "R2", _TRUNCATE); return true; }
        return false;   // connected pad, nothing pressed
    }
    return false;
}
void ff_pad_set(const char* token) {
    if (!token || !*token) return;
    strncpy_s(g_cfg.fast_forward_pad_button, token, _TRUNCATE);
    if (!g_ff_xinput) ff_load_xinput();
    ff_parse_pad_button();
}
const char* ff_pad_current() {
    return g_cfg.fast_forward_pad_button[0] ? g_cfg.fast_forward_pad_button : "L1";
}

static DWORD WINAPI ff_poll_thread(LPVOID) {
    bool key_prev   = false;    // fast-forward key/pad edge
    bool freeze_prev = false;   // freeze-lock (save-state) key edge
    bool slow_prev  = false;    // slow-mo key edge
    if (g_cfg.fast_forward_pad) { ff_load_xinput(); ff_parse_pad_button(); }
    while (!g_ff_thread_quit) {
        // Freeze all hotkey handling while the in-game config overlay is open so
        // keys typed into the panel (incl. rebinding the Turbo key) don't also
        // cycle speed. Reset edges so closing the panel doesn't fire a stale one.
        if (g_config_overlay_active.load(std::memory_order_relaxed)) {
            key_prev = freeze_prev = slow_prev = false;
            Sleep(16);
            continue;
        }
        // ── Fast-forward key (speed UP) ──────────────────────────────────
        if (g_cfg.fast_forward_enabled) {
            bool key = (GetAsyncKeyState(g_cfg.fast_forward_vk) & 0x8000) != 0;
            if (g_cfg.fast_forward_pad && ff_pad_down()) key = true;   // gamepad also cycles
            if (key && !key_prev) {
                // Advance the ladder: 1x -> 2x -> 4x -> 8x -> 16x -> back to 1x.
                g_ff_idx = (g_ff_idx + 1) % g_ff_nsteps;
                float s = g_ff_steps[g_ff_idx];
                g_paused = false;   // leaving pause
                rect_tuner_set_latch(false);
                g_ff_cur_scale.store(s, std::memory_order_relaxed);
                g_ff_active = (s > 1.0f);
                ff_overlay_set(s);   // show the badge top-right
                hook_log("bof4: fast-forward -> %gx\n", (double)s);
            }
            key_prev = key;
        }
        // ── LATCH key (tuner rect latch ONLY — never touches the clock) ───────
        // Freezing the clock (scale 0) can hang/crash real-time minigames like
        // fishing, so we DON'T. The latch alone keeps the tuner's captured rect
        // list so a transient graphic stays selectable/tunable with F10 after it's
        // gone. To hold the graphic on screen while you latch, use the slow-mo key
        // first. 100% safe: touches only the tuner's own memory.
        if (g_cfg.pause_slowmo && g_cfg.pause_vk) {
            bool fk = (GetAsyncKeyState(g_cfg.pause_vk) & 0x8000) != 0;
            if (fk && !freeze_prev) {
                bool now_on = !rect_tuner_is_latched();
                rect_tuner_set_latch(now_on);      // auto-enables the tuner too
                hook_log("bof4: rect latch %s\n", now_on ? "ON" : "OFF");
            }
            freeze_prev = fk;
        }
        // ── Slow-mo key (helper: line up a transient before freezing) ─────
        if (g_cfg.pause_slowmo && g_cfg.pause_slow_vk) {
            bool sk = (GetAsyncKeyState(g_cfg.pause_slow_vk) & 0x8000) != 0;
            if (sk && !slow_prev) {
                rect_tuner_set_latch(false);   // leave freeze+latch
                g_paused = !g_paused;
                g_ff_idx = 0;
                float ps = g_cfg.pause_scale;
                if (!(ps > 0.0f) || ps >= 1.0f) ps = 0.125f;   // sane fallback
                float s = g_paused ? ps : 1.0f;
                g_ff_cur_scale.store(s, std::memory_order_relaxed);
                g_ff_active = false;   // slow-mo never skips waits
                ff_overlay_set(s);
                hook_log("bof4: slow-mo %s (scale %gx)\n",
                         g_paused ? "ON" : "OFF", (double)s);
            }
            slow_prev = sk;
        }
        Sleep(30);
    }
    return 0;
}

// Kept for binary compatibility with the Present hook (harmless no-op
// now that polling is thread-driven). Called from hooks.cpp but does
// nothing; the real toggle logic lives in ff_poll_thread.
void bof4_ff_tick() {
    // Intentionally empty — see ff_poll_thread above.
}

// ── Config loader: reads _d3d9_hook_config.txt ────────────────────────────
// One line per setting: `key = value`. Unknown keys are ignored.
// If the file is absent, all flags stay at their defaults (all false).
static int parse_vk(const char* val) {
    if (!val || !*val) return 0;
    if (_stricmp(val, "TAB")   == 0) return VK_TAB;
    if (_stricmp(val, "HOME")  == 0) return VK_HOME;
    if (_stricmp(val, "END")   == 0) return VK_END;
    if (_stricmp(val, "PAUSE") == 0 || _stricmp(val, "BREAK") == 0) return VK_PAUSE;
    if (_stricmp(val, "SCROLL")== 0 || _stricmp(val, "SCROLLLOCK") == 0) return VK_SCROLL;
    if (_stricmp(val, "INSERT")== 0 || _stricmp(val, "INS") == 0) return VK_INSERT;
    if (_stricmp(val, "DELETE")== 0 || _stricmp(val, "DEL") == 0) return VK_DELETE;
    if ((val[0] == 'F' || val[0] == 'f') && val[1] >= '0' && val[1] <= '9') {
        int n = atoi(val + 1);
        if (n >= 1 && n <= 12) return VK_F1 + (n - 1);
    }
    // Single A-Z / 0-9 key (VK codes == ASCII uppercase / digit).
    if (val[1] == 0) {
        char c = (char)toupper((unsigned char)val[0]);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return (int)(unsigned char)c;
    }
    // Raw code fallback ("0xNN" / decimal) — lets the in-game config overlay
    // persist any remapped hotkey even when it has no friendly name here.
    if (val[0] == '0' && (val[1] == 'x' || val[1] == 'X')) {
        int v = (int)strtol(val, nullptr, 16);
        if (v > 0 && v < 256) return v;
    }
    return 0;
}

static void trim(char* s) {
    if (!s) return;
    char* p = s;
    while (*p == ' ' || *p == '\t') ++p;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t'
                  || s[n-1] == '\r' || s[n-1] == '\n')) {
        s[--n] = 0;
    }
}

static bool parse_bool(const char* val) {
    return val && (_stricmp(val, "true") == 0
                || _stricmp(val, "1")    == 0
                || _stricmp(val, "yes")  == 0
                || _stricmp(val, "on")   == 0);
}

// Parse "RGB", "RRGGBB", or "AARRGGBB" (optional # or 0x prefix) into a
// 0xAARRGGBB value. A 6-digit value is treated as fully opaque. Falls back
// to `defv` when the string has no usable hex run. Mirrors the standalone
// subtitle hook's ParseColor so the same .ini values produce the same color.
static unsigned int parse_color(const char* s, unsigned int defv) {
    if (!s) return defv;
    while (*s == ' ' || *s == '\t' || *s == '#') ++s;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    unsigned int v = 0;
    int digits = 0;
    while (*s && digits < 8) {
        char c = (char)tolower((unsigned char)*s);
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else break;
        v = (v << 4) | (unsigned)d;
        ++digits;
        ++s;
    }
    if (digits == 3) {
        unsigned int r = (v >> 8) & 0xFu, g = (v >> 4) & 0xFu, b = v & 0xFu;
        return 0xFF000000u | (r << 20) | (r << 16) | (g << 12) | (g << 8) | (b << 4) | b;
    }
    if (digits == 6) return 0xFF000000u | v;  // RRGGBB -> opaque
    if (digits == 8) return v;                 // AARRGGBB
    return defv;
}

static void load_hook_config() {
    // Defaults: everything off EXCEPT the per-API FF flags, which are
    // all on by default. That way `fast_forward = true` alone gives the
    // user the fully-scaled behavior; flipping individual ff_* flags
    // off is a debugging knob.
    g_cfg.fast_forward_enabled = false;
    g_cfg.fast_forward_scale   = 4.0f;
    g_cfg.fast_forward_vk      = VK_F12;
    g_cfg.fast_forward_pad     = true;   // also cycle on a gamepad button
    strcpy_s(g_cfg.fast_forward_pad_button, "L1");   // default; remappable in config
    g_cfg.ff_scale_gettickcount = true;
    g_cfg.ff_scale_qpc          = true;
    g_cfg.ff_skip_sleep         = true;
    g_cfg.ff_skip_wfso          = true;
    g_cfg.ff_skip_wfmo          = true;
    g_cfg.ff_skip_msgwait       = true;
    g_cfg.pause_slowmo          = false;    // opt-in dev tool (see config)
    g_cfg.pause_vk              = VK_HOME;   // freeze-lock (save-state real pause)
    g_cfg.pause_slow_vk         = VK_END;    // slow-mo helper
    g_cfg.pause_scale           = 0.125f;    // 1/8 speed while slow-mo
    g_cfg.pause_stack_kb        = 256;       // (legacy save-state; unused now)
    g_cfg.tuner_psx_w           = 320;       // UI coord space for tuner overlay boxes
    g_cfg.tuner_psx_h           = 240;
    g_cfg.tex_dump_enabled     = false;
    g_cfg.tex_inject_enabled   = false;
    g_cfg.console_enabled      = false;
    g_cfg.unlock_vsync         = false;
    g_cfg.text_substitute_enabled = false;
    g_cfg.text_substitute_log     = false;
    g_cfg.text_substitute_hotkey  = 0;      // disabled (0 = no key). Was VK_F8 but
                                            // collided with the rect-tool dump and
                                            // the hot-reload wasn't behaving; feature
                                            // code kept, just unbound. Set a key in
                                            // _d3d9_hook_config.txt to re-enable.
    g_cfg.text_resolver_enabled   = true;   // ON by default — enables
                                            // length-uncapped translations.
    g_cfg.text_render_log         = false;  // DIAGNOSTIC render log (off by default)
    g_cfg.item_name_uncap_enabled = true;   // ON by default — lifts the
                                            // 12-char [09] item-name cap.
                                            // gog_already_lifted() pre-flight
                                            // in item_name_uncap_install
                                            // makes this safe on the GOG-
                                            // updated EXE (auto-skips when
                                            // cap is already 0xFF).
    g_cfg.item_name_uncap_value   = 20;     // Budget = 20 (= 19 visible).
    g_cfg.name_slot_bp_enabled    = false;  // TEMP DIAGNOSTIC — off by default.
    g_cfg.item_name_msg_uncap_enabled = true; // ON — lift 14-char [07] msg cap.
    g_cfg.smallfont_space_vwf_enabled = true; // ON — 16px space -> g_vwf_b16[0].
    g_cfg.equip_menu_shift_px     = 30;     // Shift equip-skill Menü box left 30px.
    g_cfg.save_anywhere_enabled   = false;  // Default OFF — visible
                                            // behavior change; opt-in.
    g_cfg.bgm_loop_enabled        = true;   // `bgm_loop`: THE loop fix. ON by
                                            // default — install + apply the
                                            // correct per-track loop point.
    g_cfg.bgm_loop_log            = false;  // `bgm_loop_log`: verbose diagnostics
                                            // + install-even-if-fix-off (A/B).
    g_cfg.space_fallthrough       = true;   // Default ON — 6-byte NOP at
                                            // 0x00629ACD so space chars
                                            // read width from TABLE_A.
    g_cfg.jp_title                = false;  // Default OFF — Japanese title card (user toggle).
    // German display fixes below are always-on internal defaults (no longer
    // config keys). They fail safe / no-op if their sidecar txt files are absent.
    g_cfg.menu_rect_fix           = true;   // Un-clip German title-menu labels.
    g_cfg.rect_trace              = false;  // Diagnostic capture hotkeys — off.
    g_cfg.rect_fix                = true;   // Universal runtime un-clip (rect_widths.txt).
    g_cfg.rect_tuner              = true;   // In-game Rect Tuner (F10) — harmless until used.
    g_cfg.dat_log                 = false;  // Default OFF — [dat-open] file trace.
    g_cfg.hang_watchdog           = true;   // Cheap: one sleeping thread; acts only on a real stall.
    g_cfg.tex_prov_log            = false;  // Default OFF — texture-upload provenance log.
    g_cfg.glyph_metric_log        = false;  // Default OFF — per-glyph draw logger (hold F6).
    g_cfg.box_autofit             = true;   // Always-on German fix — auto-grow menu boxes to fit text.
    g_cfg.box_autofit_pad         = 24;     // px added to measured width (icon + margins).
    g_cfg.box_autofit_log_all     = false;  // Default OFF — diagnostic [bw-all] box logging.
    g_cfg.cmdbox_width            = 0;      // Default OFF — field command menu box stock width.
    g_cfg.cmdbox_x                = 16;     // Default 16 (stock) = no shift.
    g_cfg.cmdbox_cursor_w         = 0x56;   // German main-menu selection-frame width (86).
    g_cfg.cmdbox_label_x          = 29;     // German cmd-menu label X (0x1D; stock 0x26).
    g_cfg.cmdbox_cmdgfx_x         = 22;     // German "Command" header graphic X (0x16; stock 38).
    g_cfg.recon_scan              = 0;      // Default 0 = normal. 1 = read-only Steam/Enigma probe.
    g_cfg.censor_fix_aream031     = true;   // Default ON — AREAM031 uncensor fix
                                            // (ships enabled; verifies+aborts on
                                            // mismatch, so safe as a default).
    g_cfg.censor_fix_aream027     = true;   // Default ON — AREAM027 uncensor fix (verifies+aborts on mismatch)
    g_cfg.censor_fix_aread157     = true;   // Default ON — AREAD157 (Emperor scene) uncensor
                                            // (gate-actor codecave; verifies+aborts on
                                            // mismatch). Verified in-game: decapitation +
                                            // throne ascension choreography + Yuna's box.
    g_cfg.censor_fix_trace        = false;  // Default OFF — uncensor fix+trace diagnostic.
    g_cfg.censor_scene_trace      = false;  // Default OFF — read-only scene discovery (AREAM027 etc.).
    g_cfg.censor_probe_aread145       = false;  // Default OFF — AREAD145 phase/selector logger.
    g_cfg.censor_probe_aread145_force = false;  // Default OFF — AREAD145 force phase=7 (throwaway save).
    g_cfg.dengeki_store_unlock        = true;   // Default ON — restore the Dengeki Store bonus area
                                                // (sets event flags 0x8D+0xA5; sig-verified, no-op on
                                                // mismatch; only ENABLES content, can't corrupt saves).

    g_cfg.config_gui              = true;   // In-game ImGui settings overlay (default ON).
    g_cfg.config_gui_vk           = 0;      // 0 = toggle via scan code 0x29 (^/~, layout-proof).
    g_cfg.free_camera             = true;   // Default ON — field-camera rotation (Q/E + right stick).
                                            // Sub-keys fall back internally (Q/E/F9, off in AREAE005).

    // Soft-subtitle overlay for the intro FMV. Defaults mirror the
    // standalone bof4-subtitles.ini. Master switch OFF so a fresh install
    // is behavior-neutral; user opts in.
    g_cfg.subtitle_enabled        = false;
    strcpy_s(g_cfg.subtitle_srt,  sizeof(g_cfg.subtitle_srt),  "MOV\\ZBOF4.srt");
    strcpy_s(g_cfg.subtitle_font, sizeof(g_cfg.subtitle_font), "Comic Sans MS");
    g_cfg.subtitle_font_scale     = 1.25f;
    g_cfg.subtitle_bottom_margin  = 0.10f;
    g_cfg.subtitle_time_offset_ms = 600;
    g_cfg.subtitle_font_color     = 0xFFFFFFFFu;  // opaque white
    g_cfg.subtitle_outline_color  = 0xFF000000u;  // opaque black
    g_cfg.subtitle_outline_px     = 2;
    g_cfg.subtitle_shadow_enabled = true;
    g_cfg.subtitle_shadow_offset_x= 3;
    g_cfg.subtitle_shadow_offset_y= 3;
    g_cfg.subtitle_shadow_softness= 2;
    g_cfg.subtitle_shadow_color   = 0xC0000000u;  // 75% black

    FILE* f = fopen("_d3d9_hook_config.txt", "rb");
    if (!f) {
        // Auto-create a documented default file so the user has
        // something to edit instead of guessing key names.
        static const char DEFAULT_CONFIG[] =
            "# BoF4 DLL Config\n"
            "#\n"
            "# key = value ; text after '#' is a comment. Keep this file next to BOF4.exe.\n"
            "# Display and localization fixes are built in and always on; only the real\n"
            "# options are listed here.\n"
            "\n"
            "# -- Speed --\n"
            "fast_forward            = true    # F12 cycles 1x / 2x / 4x / 8x / 16x\n"
            "fast_forward_scale      = 16.0    # top of the cycle\n"
            "fast_forward_hotkey     = F12\n"
            "fast_forward_pad        = true    # also cycle on a gamepad button\n"
            "fast_forward_pad_button = L1      # L1 R1 L2 R2 L3 R3 BACK START A B X Y D-pad, or e.g. L1+R1\n"
            "\n"
            "# -- Display --\n"
            "console                 = false   # debug console window\n"
            "unlock_vsync            = true    # remove the 30fps cap\n"
            "\n"
            "# -- Free camera (field) --\n"
            "free_camera             = true    # rotate the field camera (Q / E, or the right stick)\n"
            "free_camera_key_left    = Q\n"
            "free_camera_key_right   = E\n"
            "free_camera_toggle_key  = F9      # turn free-cam on/off in-game\n"
            "\n"
            "# -- Uncensor fix scenes --\n"
            "censor_fix_aream031     = true\n"
            "censor_fix_aream027     = true\n"
            "censor_fix_aread157     = true\n"
            "\n"
            "# -- Extras --\n"
            "jp_title                = false   # restore the Japanese title screen\n"
            "dengeki_store_unlock    = true    # restore the Japan-only Dengeki Store (shop + lottery)\n"
            "subtitle                = true    # German subtitles over the intro FMV\n"
            "save_anywhere           = false   # allow saving anywhere (testing)\n";
        FILE* wf = fopen("_d3d9_hook_config.txt", "wb");
        if (wf) {
            fwrite(DEFAULT_CONFIG, 1, sizeof(DEFAULT_CONFIG) - 1, wf);
            fclose(wf);
            hook_log("hook config: _d3d9_hook_config.txt not found - "
                     "auto-created with defaults (all features OFF)\n");
        } else {
            hook_log("hook config: _d3d9_hook_config.txt not found and "
                     "could not be created - using compiled defaults\n");
        }
        return;
    }
    char buf[256];
    while (fgets(buf, sizeof(buf), f)) {
        char* hash = strchr(buf, '#');
        if (hash) *hash = 0;
        char* eq = strchr(buf, '=');
        if (!eq) continue;
        *eq = 0;
        char* key = buf;
        char* val = eq + 1;
        trim(key); trim(val);
        if (!*key) continue;

        if      (_stricmp(key, "fast_forward")        == 0) g_cfg.fast_forward_enabled = parse_bool(val);
        else if (_stricmp(key, "fast_forward_scale")  == 0) { float x = (float)atof(val); if (x >= 1.0f && x <= 16.0f) g_cfg.fast_forward_scale = x; }
        else if (_stricmp(key, "fast_forward_hotkey") == 0) { int vk = parse_vk(val); if (vk) g_cfg.fast_forward_vk = vk; }
        else if (_stricmp(key, "fast_forward_pad")    == 0) g_cfg.fast_forward_pad = parse_bool(val);
        else if (_stricmp(key, "fast_forward_pad_button") == 0) strncpy_s(g_cfg.fast_forward_pad_button, val, _TRUNCATE);
        else if (_stricmp(key, "pause_slowmo")        == 0) g_cfg.pause_slowmo         = parse_bool(val);
        else if (_stricmp(key, "pause_hotkey")        == 0) { int vk = parse_vk(val); if (vk) g_cfg.pause_vk = vk; }
        else if (_stricmp(key, "pause_slow_hotkey")   == 0) { int vk = parse_vk(val); if (vk) g_cfg.pause_slow_vk = vk; }
        else if (_stricmp(key, "pause_scale")         == 0) { float x = (float)atof(val); if (x > 0.0f && x < 1.0f) g_cfg.pause_scale = x; }
        else if (_stricmp(key, "pause_stack_kb")      == 0) { int n = atoi(val); if (n >= 0 && n <= 1024) g_cfg.pause_stack_kb = n; }
        else if (_stricmp(key, "tuner_psx_w")         == 0) { int n = atoi(val); if (n > 0 && n <= 4096) g_cfg.tuner_psx_w = n; }
        else if (_stricmp(key, "tuner_psx_h")         == 0) { int n = atoi(val); if (n > 0 && n <= 4096) g_cfg.tuner_psx_h = n; }
        else if (_stricmp(key, "tex_dump")            == 0) g_cfg.tex_dump_enabled     = parse_bool(val);
        else if (_stricmp(key, "tex_inject")          == 0) g_cfg.tex_inject_enabled   = parse_bool(val);
        else if (_stricmp(key, "console")             == 0) g_cfg.console_enabled      = parse_bool(val);
        else if (_stricmp(key, "unlock_vsync")        == 0) g_cfg.unlock_vsync         = parse_bool(val);
        else if (_stricmp(key, "ff_scale_gettickcount") == 0) g_cfg.ff_scale_gettickcount = parse_bool(val);
        else if (_stricmp(key, "ff_scale_qpc")        == 0) g_cfg.ff_scale_qpc          = parse_bool(val);
        else if (_stricmp(key, "ff_skip_sleep")       == 0) g_cfg.ff_skip_sleep         = parse_bool(val);
        else if (_stricmp(key, "ff_skip_wfso")        == 0) g_cfg.ff_skip_wfso          = parse_bool(val);
        else if (_stricmp(key, "ff_skip_wfmo")        == 0) g_cfg.ff_skip_wfmo          = parse_bool(val);
        else if (_stricmp(key, "ff_skip_msgwait")     == 0) g_cfg.ff_skip_msgwait       = parse_bool(val);
        else if (_stricmp(key, "text_substitute")     == 0) g_cfg.text_substitute_enabled = parse_bool(val);
        else if (_stricmp(key, "text_substitute_log") == 0) g_cfg.text_substitute_log     = parse_bool(val);
        else if (_stricmp(key, "text_substitute_hotkey") == 0) { int vk = parse_vk(val); if (vk) g_cfg.text_substitute_hotkey = vk; }
        else if (_stricmp(key, "text_resolver")     == 0) g_cfg.text_resolver_enabled   = parse_bool(val);
        else if (_stricmp(key, "text_render_log")   == 0) g_cfg.text_render_log         = parse_bool(val);
        else if (_stricmp(key, "name_slot_bp")      == 0) g_cfg.name_slot_bp_enabled = parse_bool(val);
        else if (_stricmp(key, "item_name_msg_uncap") == 0) g_cfg.item_name_msg_uncap_enabled = parse_bool(val);
        else if (_stricmp(key, "smallfont_space_vwf") == 0) g_cfg.smallfont_space_vwf_enabled = parse_bool(val);
        else if (_stricmp(key, "equip_menu_shift")   == 0) g_cfg.equip_menu_shift_px = (int)strtol(val, nullptr, 0);
        else if (_stricmp(key, "item_name_uncap")   == 0) g_cfg.item_name_uncap_enabled = parse_bool(val);
        else if (_stricmp(key, "item_name_uncap_value") == 0) {
            int v = atoi(val);
            if (v >= 1 && v <= 255) g_cfg.item_name_uncap_value = v;
        }
        else if (_stricmp(key, "save_anywhere")     == 0) g_cfg.save_anywhere_enabled  = parse_bool(val);
        else if (_stricmp(key, "space_fallthrough") == 0) g_cfg.space_fallthrough      = parse_bool(val);
        // `bgm_loop` = the fix. Legacy aliases `bgm_log` and `bgm_loop_fix`
        // also map here so old configs keep enabling the fix.
        else if (_stricmp(key, "bgm_loop") == 0 || _stricmp(key, "bgm_log") == 0 ||
                 _stricmp(key, "bgm_loop_fix") == 0) g_cfg.bgm_loop_enabled = parse_bool(val);
        else if (_stricmp(key, "bgm_loop_log") == 0) g_cfg.bgm_loop_log = parse_bool(val);
        else if (_stricmp(key, "jp_title")          == 0) g_cfg.jp_title               = parse_bool(val);
        else if (_stricmp(key, "menu_rect_fix")     == 0) g_cfg.menu_rect_fix          = parse_bool(val);
        else if (_stricmp(key, "rect_trace")        == 0) g_cfg.rect_trace             = parse_bool(val);
        else if (_stricmp(key, "rect_fix")          == 0) g_cfg.rect_fix               = parse_bool(val);
        else if (_stricmp(key, "rect_tuner")        == 0) g_cfg.rect_tuner             = parse_bool(val);
        else if (_stricmp(key, "free_camera")       == 0) g_cfg.free_camera            = parse_bool(val);
        else if (_stricmp(key, "free_camera_speed") == 0) g_cfg.free_camera_speed      = (int)strtol(val, nullptr, 0);
        else if (_stricmp(key, "free_camera_off_areas") == 0) strncpy_s(g_cfg.free_camera_off_areas, val, _TRUNCATE);
        else if (_stricmp(key, "free_camera_key_left")  == 0) strncpy_s(g_cfg.free_camera_key_left, val, _TRUNCATE);
        else if (_stricmp(key, "free_camera_key_right") == 0) strncpy_s(g_cfg.free_camera_key_right, val, _TRUNCATE);
        else if (_stricmp(key, "free_camera_toggle_key")== 0) strncpy_s(g_cfg.free_camera_toggle_key, val, _TRUNCATE);
        else if (_stricmp(key, "tex_prov_log")      == 0) g_cfg.tex_prov_log           = parse_bool(val);
        else if (_stricmp(key, "glyph_metric_log")  == 0) g_cfg.glyph_metric_log       = parse_bool(val);
        else if (_stricmp(key, "box_autofit")       == 0) g_cfg.box_autofit            = parse_bool(val);
        else if (_stricmp(key, "box_autofit_pad")   == 0) { int v = atoi(val); if (v >= 0 && v <= 128) g_cfg.box_autofit_pad = v; }
        else if (_stricmp(key, "box_autofit_log_all")== 0) g_cfg.box_autofit_log_all = parse_bool(val);
        else if (_stricmp(key, "cmdbox_width")       == 0) { g_cfg.cmdbox_width = (int)strtol(val, nullptr, 0); }
        else if (_stricmp(key, "cmdbox_x")           == 0) { g_cfg.cmdbox_x        = (int)strtol(val, nullptr, 0); }
        else if (_stricmp(key, "cmdbox_cursor_w")    == 0) { g_cfg.cmdbox_cursor_w = (int)strtol(val, nullptr, 0); }
        else if (_stricmp(key, "cmdbox_asm")         == 0) g_cfg.cmdbox_asm = parse_bool(val);
        else if (_stricmp(key, "cmdbox_label_x")     == 0) g_cfg.cmdbox_label_x  = (int)strtol(val, nullptr, 0);
        else if (_stricmp(key, "cmdbox_cmdgfx_x")    == 0) g_cfg.cmdbox_cmdgfx_x = (int)strtol(val, nullptr, 0);
        else if (_stricmp(key, "recon_scan")         == 0) { g_cfg.recon_scan = (int)strtol(val, nullptr, 0); }
        else if (_stricmp(key, "censor_fix_aream031")== 0) g_cfg.censor_fix_aream031 = parse_bool(val);
        else if (_stricmp(key, "censor_fix_aream027")== 0) g_cfg.censor_fix_aream027 = parse_bool(val);
        else if (_stricmp(key, "censor_fix_aread157")== 0) g_cfg.censor_fix_aread157 = parse_bool(val);
        else if (_stricmp(key, "censor_fix_trace")   == 0) g_cfg.censor_fix_trace    = parse_bool(val);
        else if (_stricmp(key, "censor_scene_trace") == 0) g_cfg.censor_scene_trace  = parse_bool(val);
        else if (_stricmp(key, "censor_probe_aread145")      == 0) g_cfg.censor_probe_aread145       = parse_bool(val);
        else if (_stricmp(key, "censor_probe_aread145_force")== 0) g_cfg.censor_probe_aread145_force = parse_bool(val);
        else if (_stricmp(key, "dengeki_store_unlock")       == 0) g_cfg.dengeki_store_unlock        = parse_bool(val);
        else if (_stricmp(key, "dat_log")           == 0) g_cfg.dat_log               = parse_bool(val);
        else if (_stricmp(key, "hang_watchdog")     == 0) g_cfg.hang_watchdog         = parse_bool(val);
        else if (_stricmp(key, "config_gui")        == 0) g_cfg.config_gui            = parse_bool(val);
        else if (_stricmp(key, "config_gui_hotkey") == 0) {
            // 0 / "default" / the tilde-key names => scan-code 0x29 (^/~), else a VK.
            if (_stricmp(val, "default") == 0 || _stricmp(val, "grave") == 0 ||
                _stricmp(val, "tilde") == 0 || _stricmp(val, "console") == 0 ||
                strcmp(val, "^") == 0 || strcmp(val, "~") == 0 || strcmp(val, "`") == 0)
                g_cfg.config_gui_vk = 0;
            else
                g_cfg.config_gui_vk = parse_vk(val);
        }
        // ── Subtitle overlay (intro FMV) ──────────────────────────────────
        else if (_stricmp(key, "subtitle")            == 0) g_cfg.subtitle_enabled       = parse_bool(val);
        else if (_stricmp(key, "subtitle_srt")        == 0) strcpy_s(g_cfg.subtitle_srt,  sizeof(g_cfg.subtitle_srt),  val);
        else if (_stricmp(key, "subtitle_font")       == 0) strcpy_s(g_cfg.subtitle_font, sizeof(g_cfg.subtitle_font), val);
        else if (_stricmp(key, "subtitle_font_scale") == 0) { float x = (float)atof(val); if (x > 0.0f) g_cfg.subtitle_font_scale = x; }
        else if (_stricmp(key, "subtitle_bottom_margin") == 0) g_cfg.subtitle_bottom_margin = (float)atof(val);
        else if (_stricmp(key, "subtitle_time_offset_ms") == 0) g_cfg.subtitle_time_offset_ms = atoi(val);
        else if (_stricmp(key, "subtitle_font_color")    == 0) g_cfg.subtitle_font_color    = parse_color(val, 0xFFFFFFFFu);
        else if (_stricmp(key, "subtitle_outline_color") == 0) g_cfg.subtitle_outline_color = parse_color(val, 0xFF000000u);
        else if (_stricmp(key, "subtitle_outline_px")    == 0) g_cfg.subtitle_outline_px    = atoi(val);
        else if (_stricmp(key, "subtitle_shadow_enabled")  == 0) g_cfg.subtitle_shadow_enabled  = parse_bool(val);
        else if (_stricmp(key, "subtitle_shadow_offset_x") == 0) g_cfg.subtitle_shadow_offset_x = atoi(val);
        else if (_stricmp(key, "subtitle_shadow_offset_y") == 0) g_cfg.subtitle_shadow_offset_y = atoi(val);
        else if (_stricmp(key, "subtitle_shadow_softness") == 0) g_cfg.subtitle_shadow_softness = atoi(val);
        else if (_stricmp(key, "subtitle_shadow_color")    == 0) g_cfg.subtitle_shadow_color    = parse_color(val, 0xC0000000u);
    }
    fclose(f);

    // Allocate the debug console BEFORE the first hook_log call so the
    // initial diagnostic lines also land in the console.
    if (g_cfg.console_enabled) {
        if (AllocConsole()) {
            FILE* _unused = nullptr;
            freopen_s(&_unused, "CONOUT$", "w", stdout);
            freopen_s(&_unused, "CONOUT$", "w", stderr);
            SetConsoleTitleA("BoF4 d3d9 hook — debug console");
            // Line-buffered output so each hook_log line appears promptly.
            setvbuf(stdout, nullptr, _IOLBF, 1024);
        }
    }

    // Create the texture folders ONLY if their feature is actually enabled.
    // (These used to be made unconditionally in DllMain, which meant a plain
    // player install grew two empty folders it never had a use for.)
    if (g_cfg.tex_dump_enabled)   CreateDirectoryA("tex_dump",   nullptr);
    if (g_cfg.tex_inject_enabled) CreateDirectoryA("tex_inject", nullptr);

    hook_log("hook config: ff=%s (scale=%.1fx, vk=0x%02X), "
             "tex_dump=%s, tex_inject=%s, console=%s, unlock_vsync=%s\n",
             g_cfg.fast_forward_enabled ? "ON"  : "off",
             g_cfg.fast_forward_scale,
             g_cfg.fast_forward_vk,
             g_cfg.tex_dump_enabled     ? "ON"  : "off",
             g_cfg.tex_inject_enabled   ? "ON"  : "off",
             g_cfg.console_enabled      ? "ON"  : "off",
             g_cfg.unlock_vsync         ? "ON"  : "off");
}

// ── Cap total file-I/O log lines to avoid runaway log sizes ───────────────
static constexpr int  FILE_LOG_MAX = 100000;
static std::atomic<int> g_file_log_count{0};

static inline bool log_budget_ok() {
    return g_file_log_count.fetch_add(1, std::memory_order_relaxed) < FILE_LOG_MAX;
}

// ── Interesting-file filter ───────────────────────────────────────────────
// Restricts the tracked-handle map to files we actually care about, so the
// log doesn't fill up with kernel temp files, named pipes, system DLLs,
// etc. Matches by basename suffix/substring.
static bool has_ext(const char* base, const char* ext) {
    size_t lb = strlen(base), le = strlen(ext);
    return lb >= le && _stricmp(base + lb - le, ext) == 0;
}

// Which files this hook tracks. EXTENSION-GATED, deliberately.
//
// This used to be a substring test for "BOF4" anywhere in the basename, which
// also matched GOG GALAXY'S OWN LOG: "BOF4_overlay.log". That handle then went
// into g_tracked_files, so every write the Galaxy overlay made to its log — on
// its injection thread, while it was installing its own D3D hooks — was pulled
// through our mutexes and our log writer. Under Galaxy the game stopped making
// progress at exactly that call; launched straight from the exe (no overlay, no
// such file) it never happened. Only real game files qualify now.
// Second gate, so the next foreign component that happens to ship a .dat can't
// do the same thing: an ABSOLUTE path outside the game's own folder is not ours.
// The engine opens its files relatively ("DAT\INIT.DAT", CWD = the install dir)
// or under the install path; anything else belongs to somebody else's thread.
static const std::string& game_root() {
    static const std::string r = [] {
        char buf[MAX_PATH]; buf[0] = 0;
        GetModuleFileNameA(nullptr, buf, MAX_PATH);
        char* s = strrchr(buf, '\\');
        if (s) *(s + 1) = 0; else buf[0] = 0;
        return std::string(buf);
    }();
    return r;
}
static bool path_is_ours(const char* name) {
    bool absolute = (name[0] && name[1] == ':') || (name[0] == '\\' && name[1] == '\\');
    if (!absolute) return true;                       // relative -> CWD -> game dir
    const std::string& root = game_root();
    return !root.empty() &&
           _strnicmp(name, root.c_str(), root.size()) == 0;
}

static bool is_interesting_filename(const char* name) {
    if (!name || !*name) return false;
    if (!path_is_ours(name)) return false;
    const char* base = strrchr(name, '\\');
    if (base) ++base; else base = name;
    return has_ext(base, ".dat")
        || has_ext(base, ".cfg")
        || _stricmp(base, "BOF4.exe") == 0;
}

// ── Per-handle state ──────────────────────────────────────────────────────
struct TrackedFile {
    std::string name;   // short basename only, for readable logs
    uint64_t    pos;    // current file-pointer offset, updated by hooks
};

static std::mutex                                  g_files_mtx;
static std::unordered_map<HANDLE, TrackedFile>     g_tracked_files;

static void remember_handle(HANDLE h, const char* full_name) {
    if (h == nullptr || h == INVALID_HANDLE_VALUE) return;
    const char* base = strrchr(full_name, '\\');
    if (base) ++base; else base = full_name;
    std::lock_guard<std::mutex> lk(g_files_mtx);
    g_tracked_files[h] = TrackedFile{ std::string(base), 0 };
}

// Returns a COPY of the tracked file info (if present). Returning a copy
// avoids holding the mutex while we format + log.
static bool lookup_handle(HANDLE h, TrackedFile* out) {
    if (h == nullptr || h == INVALID_HANDLE_VALUE) return false;
    std::lock_guard<std::mutex> lk(g_files_mtx);
    auto it = g_tracked_files.find(h);
    if (it == g_tracked_files.end()) return false;
    *out = it->second;
    return true;
}

static void update_handle_pos(HANDLE h, uint64_t new_pos) {
    if (h == nullptr || h == INVALID_HANDLE_VALUE) return;
    std::lock_guard<std::mutex> lk(g_files_mtx);
    auto it = g_tracked_files.find(h);
    if (it != g_tracked_files.end()) {
        it->second.pos = new_pos;
    }
}

static void advance_handle_pos(HANDLE h, uint64_t delta) {
    if (h == nullptr || h == INVALID_HANDLE_VALUE) return;
    std::lock_guard<std::mutex> lk(g_files_mtx);
    auto it = g_tracked_files.find(h);
    if (it != g_tracked_files.end()) {
        it->second.pos += delta;
    }
}

static void forget_handle(HANDLE h) {
    if (h == nullptr || h == INVALID_HANDLE_VALUE) return;
    std::lock_guard<std::mutex> lk(g_files_mtx);
    g_tracked_files.erase(h);
}

// ── Known DAT file state ──────────────────────────────────────────────────
// Captures the runtime base pointer of the INIT.DAT buffer. BoF4 loads the
// entire INIT.DAT (~224 KB) into a single contiguous RAM buffer at startup;
// that buffer is never swapped or re-read from disk. Knowing its base lets
// downstream hooks do direct RAM access / patching of the item-name table,
// the font bitmaps, the VWF width table, etc. — without touching the DAT
// file itself.
static std::mutex g_known_dats_mtx;
static void*      g_init_dat_base = nullptr;

// Char-indexed width buffer used to redirect the two `movsx eax,
// [eax*2 + IMM32]` reads inside the text-width function at 0x00629E90.
// Stock IMM32s (0x00952DA1 / 0x00952E71) make those reads alias into
// TABLE_A's low slots when char >= 0x88, giving umlauts garbage widths
// in some message-box paths. Pointing both reads at &g_vwf_alt[1]
// instead lets the same `[char*2 + base]` formula resolve to
// `g_vwf_alt[char*2 + 1]` (= byte+1, advance) for every char value.
// Layout: g_vwf_alt[ch*2] = bearing, g_vwf_alt[ch*2 + 1] = advance.
static uint8_t    g_vwf_alt[0x200] = {};

// Private width table for the 16px font. That font read its widths from the
// stock TABLE_B (0x952DE0), which physically OVERLAPS TABLE_A — so its umlauts
// shared bytes with TABLE_A's punctuation and could not be set independently.
// We relocate the 16px font's 4 TABLE_B reads to THIS buffer (seeded with the
// stock TABLE_B bytes, so behavior is identical until the `[16px]` section of
// vwf_config.txt sets 16px-specific widths). (char-0x20)*2 layout like TABLE_B.
static uint8_t    g_vwf_b16[0x200] = {};

// ─── Dialog width twin-substitution ────────────────────────────────
// Function 0x006780E0 is the universal glyph-metric lookup used by
// all 13 text subsystems (including the dialog box that doesn't read
// TABLE_A/B/C/D). Instead of decoding its 5-table compound-lookup
// width algorithm, we hook it: when the caller asks for the metrics
// of one of our umlaut piggyback slots, we silently feed the function
// a sensibly-widthed ASCII twin char, so the returned metrics are
// right-sized for the umlaut glyph pixels we painted into that slot.
//
// The glyph bytes flow through the render pipeline unchanged; only
// the width lookup gets diverted. Side effect: anywhere that literally
// wants to display the ASCII char (e.g. a literal '$' in text) now
// shows the umlaut glyph AND uses the twin's width — but the user
// has already decided those ASCII chars are sacrificed for German
// umlaut encoding, so this is harmless.
static void* g_orig_width_fn = nullptr;

// Twin map: if incoming char is `from`, substitute with `to` before
// handing to the original width function.
//
// German umlauts now occupy 0x90-0x96 (post JP-atlas swap; vwf_config.txt
// drives the encoding). The dialog renderer's universal width function
// at 0x006780E0 has no width data for char codes ≥ 0x90, so without the
// twin substitution every umlaut renders at the cell-width fallback
// (24 px). The legacy piggyback positions (0x24/0x28/0x29/0x3C/0x3E/0x5B/
// 0x5D) are no longer used by the repacker — those ASCII slots are
// available again — so we remove their twins here.
//
// TODO: generalize for non-German languages (Portuguese ç/ã/õ at 0xA0+,
// etc.). Could be made config-driven by reading vwf_config.txt and
// matching each non-ASCII byte to a similarly-shaped ASCII twin.
static const struct WidthTwin {
    uint8_t from;
    uint8_t to;
} g_width_twins[] = {
    { 0x90, 0x41 },  // Ä -> A
    { 0x91, 0x55 },  // Ü -> U
    { 0x92, 0x4F },  // Ö -> O
    { 0x93, 0x61 },  // ä -> a
    { 0x94, 0x75 },  // ü -> u
    { 0x95, 0x6F },  // ö -> o
    { 0x96, 0x42 },  // ß -> B (similar width)
};

// Diagnostic counters — first 20 hook invocations are logged so we
// can confirm the hook fires and what chars flow through.
static LONG g_width_fn_calls   = 0;
static LONG g_width_fn_logged  = 0;
static LONG g_width_fn_subs    = 0;

// Called from the naked thunk. Overwrites the caller's arg4 low byte
// in place if it matches a piggyback slot.
extern "C" void __cdecl bof4_width_substitute(uint32_t* arg4_ptr) {
    LONG n = InterlockedIncrement(&g_width_fn_calls);
    uint32_t original = *arg4_ptr;
    uint8_t c = (uint8_t)(original & 0xFF);
    // SPACE-width trace (F3 diagnostic): note when the universal width fn is
    // asked for a space, so we can tell whether the on-screen menu widths its
    // spaces via 0x006780E0 vs a direct TABLE_A/B/C/F read. No-op when disarmed.
    bool substituted = false;
    uint8_t to = 0;
    for (size_t i = 0; i < sizeof(g_width_twins)/sizeof(g_width_twins[0]); i++) {
        if (c == g_width_twins[i].from) {
            *arg4_ptr = (original & 0xFFFFFF00u) | g_width_twins[i].to;
            substituted = true;
            to = g_width_twins[i].to;
            InterlockedIncrement(&g_width_fn_subs);
            break;
        }
    }
    // Counters only — per-call logging removed (was very noisy).
    InterlockedIncrement(&g_width_fn_logged);
    (void)n; (void)original; (void)c; (void)substituted; (void)to;
}

// Naked thunk. On entry: ESP = [ret_addr, arg1, arg2, arg3, arg4, ...].
// The char value is arg4 at [esp + 0x10] (that's where the original
// function's first-instruction `mov eax, [esp+0x1C]` post-`sub esp,0xC`
// reads from). We preserve all regs, hand &arg4 to the substitution
// helper, then tail-jmp to MinHook's trampoline for the original.
extern "C" __declspec(naked) void thunk_width_fn() {
    __asm {
        pushad                         // 32 bytes
        // DIAGNOSTIC glyph logger (no-op unless glyph_metric_log): log the
        // caller return address + &arg0 BEFORE substitution (original char).
        // ret_addr = [esp+32]; &arg0 = [esp+32+4].
        mov  ecx, [esp + 32]
        lea  edx, [esp + 32 + 4]
        push edx
        push ecx
        call glyph_log_record
        add  esp, 8
        // &arg4 (original) = [esp + 32 + 0x10]  (pushad=32, plus orig +0x10)
        lea  eax, [esp + 32 + 0x10]
        push eax
        call bof4_width_substitute
        add  esp, 4
        popad
        jmp  dword ptr [g_orig_width_fn]
    }
}

// (VWF-memory page guard retired — it caused New Game hangs because
// the VEH had to re-arm PAGE_GUARD after every fault including system
// DLL noise. If we ever need read-site capture again, prefer
// hardware breakpoints or INT3 at individual reader instructions
// instead of whole-page guards.)


#if 0
// ─── Text-function entry instrumentation (retired) ─────────────────
// Previously hooked the entry of 6 candidate text-rendering / width-
// measuring functions to identify the dialog renderer. None of them
// matched the message-box path, so removed to eliminate the per-call
// thunk overhead and the New Game slowdown it caused. Kept as #if 0
// for reference in case we need to re-instrument later.
static void*  g_orig_text_fn_0 = nullptr;  // 0x00628DD0
static void*  g_orig_text_fn_1 = nullptr;  // 0x00629470
static void*  g_orig_text_fn_2 = nullptr;  // 0x00629860
static void*  g_orig_text_fn_3 = nullptr;  // 0x00629E90
static void*  g_orig_text_fn_4 = nullptr;  // 0x0062A040
static void*  g_orig_text_fn_5 = nullptr;  // 0x0062A680
static LONG   g_text_fn_logged_highbyte[6] = {0,0,0,0,0,0};
static LONG   g_text_fn_calls[6]           = {0,0,0,0,0,0};

struct PushadRegs {
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
};

static void log_text_fn_entry(int idx, uint32_t esp_at_entry,
                              const PushadRegs* regs) {
    // ESP at entry points to the return address; pushed args start at
    // esp+4. Log the first 8 calls per function unconditionally and
    // the first high-byte-carrying call separately. Each call is
    // logged with the first 4 pushed dwords + registers + printable
    // decode of any value that looks like a string pointer.
    LONG n = InterlockedIncrement(&g_text_fn_calls[idx]);
    uint32_t a1 = *(uint32_t*)(esp_at_entry + 4);
    uint32_t a2 = *(uint32_t*)(esp_at_entry + 8);
    uint32_t a3 = *(uint32_t*)(esp_at_entry + 12);
    uint32_t a4 = *(uint32_t*)(esp_at_entry + 16);
    uint32_t a5 = *(uint32_t*)(esp_at_entry + 20);
    uint32_t a6 = *(uint32_t*)(esp_at_entry + 24);
    auto decode = [](uint32_t p) -> void {
        if (!p) return;
        uint8_t buf[24] = {0};
        if (!fa_safe_read((const void*)(uintptr_t)p,
                          sizeof(buf), buf))
            return;
        bool has_high = false;
        int printable = 0;
        for (int i = 0; i < 20; i++) {
            uint8_t b = buf[i];
            if (b == 0) break;
            if (b >= 0x80) has_high = true;
            if (b >= 0x20 && b < 0x7F) printable++;
        }
        if (printable < 2 && !has_high) return;
        char ascii[64]; int m = 0;
        for (int i = 0; i < 20 && buf[i] && m + 5 < (int)sizeof(ascii);
             i++) {
            uint8_t b = buf[i];
            if (b >= 0x20 && b < 0x7F) ascii[m++] = (char)b;
            else m += sprintf(ascii + m, "[%02X]", b);
        }
        ascii[m] = 0;
        hook_log("        -> 0x%08X %s \"%s\"\n",
                 p, has_high ? "HIGH" : "----", ascii);
    };
    bool is_highbyte = false;
    {
        for (uint32_t p : {a1, a2}) {
            uint8_t buf[24] = {0};
            if (!fa_safe_read((const void*)(uintptr_t)p,
                              sizeof(buf), buf)) continue;
            for (int i = 0; i < 20 && buf[i]; i++)
                if (buf[i] >= 0x80) { is_highbyte = true; break; }
            if (is_highbyte) break;
        }
    }
    // Log the first 8 calls per function unconditionally. After that,
    // log EVERY call carrying a high-byte (> 0x7F) character so we
    // capture umlauts — but cap such logs at 32 per function to keep
    // the file readable.
    bool log_it = (n <= 8);
    if (!log_it && is_highbyte) {
        LONG h = InterlockedIncrement(&g_text_fn_logged_highbyte[idx]);
        if (h <= 32) log_it = true;
    }
    if (!log_it) return;
    hook_log(
        "bof4: TEXT_FN[%d] call#%ld\n"
        "   args: %08X %08X %08X %08X %08X %08X%s\n"
        "   regs: eax=%08X ecx=%08X edx=%08X ebx=%08X "
        "esi=%08X edi=%08X ebp=%08X\n",
        idx, n,
        a1, a2, a3, a4, a5, a6, is_highbyte ? "  (HIGH)" : "",
        regs->eax, regs->ecx, regs->edx, regs->ebx,
        regs->esi, regs->edi, regs->ebp);
    // Decode every arg AND every non-trivial register that could be
    // a pointer to string/struct data.
    decode(a1); decode(a2); decode(a3); decode(a4);
    decode(a5); decode(a6);
    decode(regs->eax);
    decode(regs->ecx);
    decode(regs->edx);
    decode(regs->esi);
    decode(regs->edi);
}

// Naked thunks. On entry, ESP = return address (from caller's call
// instruction). We save all regs + flags, pass ESP+36 to the logger
// (so the logger sees the pre-call ESP), then tail-JMP to the
// MinHook trampoline. Tail-jump preserves ESP and the caller's
// pushed args for the real function.
// Each naked thunk: `pushad` saves 8 regs (32 bytes), `pushfd` saves
// flags (4 bytes). The 32-byte pushad block = PushadRegs layout, base
// at ESP after pushfd = ESP + 4. Original entry ESP = current + 36.
// Args to logger (cdecl, pushed right-to-left):
//   arg3 = PushadRegs*     (esp + 4 after pushfd)
//   arg2 = esp_at_entry    (esp + 36)
//   arg1 = idx (constant)
extern "C" __declspec(naked) void thunk_text_fn_0() {
    __asm {
        pushad
        pushfd
        lea  edx, [esp + 4]        ; &PushadRegs
        lea  eax, [esp + 36]       ; esp_at_entry
        push edx
        push eax
        push 0
        call log_text_fn_entry
        add  esp, 12
        popfd
        popad
        jmp  dword ptr [g_orig_text_fn_0]
    }
}
extern "C" __declspec(naked) void thunk_text_fn_1() {
    __asm {
        pushad
        pushfd
        lea  edx, [esp + 4]
        lea  eax, [esp + 36]
        push edx
        push eax
        push 1
        call log_text_fn_entry
        add  esp, 12
        popfd
        popad
        jmp  dword ptr [g_orig_text_fn_1]
    }
}
extern "C" __declspec(naked) void thunk_text_fn_2() {
    __asm {
        pushad
        pushfd
        lea  edx, [esp + 4]
        lea  eax, [esp + 36]
        push edx
        push eax
        push 2
        call log_text_fn_entry
        add  esp, 12
        popfd
        popad
        jmp  dword ptr [g_orig_text_fn_2]
    }
}
extern "C" __declspec(naked) void thunk_text_fn_3() {
    __asm {
        pushad
        pushfd
        lea  edx, [esp + 4]
        lea  eax, [esp + 36]
        push edx
        push eax
        push 3
        call log_text_fn_entry
        add  esp, 12
        popfd
        popad
        jmp  dword ptr [g_orig_text_fn_3]
    }
}
extern "C" __declspec(naked) void thunk_text_fn_4() {
    __asm {
        pushad
        pushfd
        lea  edx, [esp + 4]
        lea  eax, [esp + 36]
        push edx
        push eax
        push 4
        call log_text_fn_entry
        add  esp, 12
        popfd
        popad
        jmp  dword ptr [g_orig_text_fn_4]
    }
}
extern "C" __declspec(naked) void thunk_text_fn_5() {
    __asm {
        pushad
        pushfd
        lea  edx, [esp + 4]
        lea  eax, [esp + 36]
        push edx
        push eax
        push 5
        call log_text_fn_entry
        add  esp, 12
        popfd
        popad
        jmp  dword ptr [g_orig_text_fn_5]
    }
}
#endif  // retired TEXT_FN instrumentation

// Called from hook_ReadFile when we see INIT.DAT's first read with a
// valid destination buffer. Only captures once per session.
static void maybe_capture_init_dat_base(const TrackedFile& info,
                                         uint64_t pos_before, void* buf)
{
    if (pos_before != 0 || buf == nullptr) return;
    if (_stricmp(info.name.c_str(), "INIT.DAT") != 0) return;
    std::lock_guard<std::mutex> lk(g_known_dats_mtx);
    if (g_init_dat_base == nullptr) {
        g_init_dat_base = buf;
    }
}

// Forward decl — defined further down in the file. Called at the end of
// parse_and_dump_init_dat once we know INIT.DAT is fully resident.
void bof4_install_init_dat_guard();

// Parse the INIT.DAT TOC from the in-RAM buffer and log every entry with
// both its file offset and its runtime virtual address. Also dumps the
// whole loaded buffer to init_loaded.bin so it can be compared byte-for-
// byte against DAT_backup/INIT.DAT offline. Called from hook_CloseHandle
// when the INIT.DAT handle is closed (i.e. the game has finished loading
// the file into memory).
static void parse_and_dump_init_dat(const TrackedFile& info) {
    void* base_void;
    {
        std::lock_guard<std::mutex> lk(g_known_dats_mtx);
        base_void = g_init_dat_base;
    }
    if (!base_void) return;
    if (info.pos < 16) return;

    // Now that INIT.DAT is fully loaded, install the page-guard
    // watchpoint on its small uncompressed entries IF the user opted in
    // via guard_enable.txt. The TOC walk + init_loaded.bin dump were
    // removed — they were startup noise. Re-enable in a debug build if
    // needed.
    bof4_install_init_dat_guard();
}

// ──────────────────────────────────────────────────────────────────────────
// PAGE_GUARD watchpoint on INIT.DAT small uncompressed entries
// ──────────────────────────────────────────────────────────────────────────
//
// v0.13 safety rewrite:
//
// The v0.12 implementation used single-step re-arm to keep the guard
// firing on every access. That worked but crashed the game after ~5000
// exception handler trips in rapid succession — the game's main thread
// couldn't make forward progress under the exception storm, and if any
// page was left PAGE_GUARD-armed when the VEH was detached, subsequent
// reads raised an unhandled exception.
//
// The v0.13 approach is a **one-shot per page** design:
//
//   * PAGE_GUARD is naturally one-shot — when the CPU catches an access
//     to a guarded page, it auto-clears the flag and raises the
//     exception. On return from the handler, the page is normal RWX
//     and the instruction retries and succeeds.
//   * We DON'T re-arm. We log the EIP + offset once per hit and let the
//     page become unguarded. Subsequent reads from the same page are
//     uninstrumented but the game runs normally.
//   * Maximum data we can collect: one unique EIP per guarded page
//     (usually 1-2 per page actually, because the CPU might re-fault
//     on adjacent bytes before the whole page is warmed up).
//   * On shutdown we restore the page protection to what it was before
//     we installed PAGE_GUARD (saved at install time). This guarantees
//     no page is left armed after the DLL detaches.
//   * Install is **opt-in** via a marker file `guard_enable.txt` in the
//     game folder. Default = no guard = safe run.
//
// This gives us less data per session but zero crash risk. Run the
// game once with the marker file present, get a handful of EIPs,
// delete the file, then proceed with whatever other runs you need.

struct GuardEipStats {
    int      count;
    uint32_t first_offset;
    uint32_t min_offset;
    uint32_t max_offset;
};

static constexpr int GUARD_MAX_UNIQUE_EIPS = 32;
static constexpr int GUARD_MAX_TOTAL_HITS  = 256;  // very conservative cap

static std::mutex                               g_guard_mtx;
static void*                                    g_guard_start          = nullptr;
static size_t                                   g_guard_size           = 0;
static DWORD                                    g_guard_orig_protect   = 0;
static PVOID                                    g_guard_veh            = nullptr;
static bool                                     g_guard_installed      = false;
static int                                      g_guard_total_hits     = 0;
static std::unordered_map<void*, GuardEipStats> g_guard_eips;

static void log_guard_summary_locked() {
    if (g_guard_eips.empty() && g_guard_total_hits == 0) return;
    hook_log("\n=== GUARD summary ===\n");
    hook_log("  total hits: %d  unique EIPs: %zu\n",
             g_guard_total_hits, g_guard_eips.size());
    hook_log("%-12s  %-8s  %-10s  %-10s  %-10s\n",
             "eip", "hits", "first_off", "min_off", "max_off");
    std::vector<std::pair<void*, GuardEipStats>> sorted;
    sorted.reserve(g_guard_eips.size());
    for (auto& kv : g_guard_eips) sorted.push_back(kv);
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) {
                  return a.second.count > b.second.count;
              });
    for (const auto& kv : sorted) {
        const auto& s = kv.second;
        hook_log("  %p  %-8d  0x%06X    0x%06X    0x%06X\n",
                 kv.first, s.count, s.first_offset, s.min_offset, s.max_offset);
    }
    hook_log("\n");
}

// Forcefully strip PAGE_GUARD from the entire guarded range by restoring
// the original protection we captured at install time. Safe to call
// repeatedly. Must hold g_guard_mtx.
static void disarm_guard_locked() {
    if (!g_guard_start || g_guard_size == 0 || g_guard_orig_protect == 0) {
        return;
    }
    DWORD old = 0;
    // Clear PAGE_GUARD bit unconditionally from whatever protection is set.
    VirtualProtect(g_guard_start, g_guard_size,
                   g_guard_orig_protect, &old);
}

// Vectored exception handler. One-shot per page: logs the hit, does
// NOT re-arm. PAGE_GUARD auto-clears when the exception fires, so
// returning CONTINUE_EXECUTION retries the instruction and it succeeds.
static LONG NTAPI guard_veh(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != STATUS_GUARD_PAGE_VIOLATION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    void*    eip  = ep->ExceptionRecord->ExceptionAddress;
    uintptr_t addr = ep->ExceptionRecord->ExceptionInformation[1];
    uintptr_t gs   = (uintptr_t)g_guard_start;
    uintptr_t ge   = gs + g_guard_size;
    if (addr < gs || addr >= ge) {
        // Some other subsystem's PAGE_GUARD, not ours.
        return EXCEPTION_CONTINUE_SEARCH;
    }

    uint32_t off = 0;
    if (g_init_dat_base) {
        off = (uint32_t)(addr - (uintptr_t)g_init_dat_base);
    }

    {
        std::lock_guard<std::mutex> lk(g_guard_mtx);
        ++g_guard_total_hits;

        auto it = g_guard_eips.find(eip);
        if (it == g_guard_eips.end()) {
            if ((int)g_guard_eips.size() < GUARD_MAX_UNIQUE_EIPS) {
                GuardEipStats s{};
                s.count        = 1;
                s.first_offset = off;
                s.min_offset   = off;
                s.max_offset   = off;
                g_guard_eips.emplace(eip, s);
                hook_log("GUARD hit #%d: eip=%p offset=0x%06X (new eip)\n",
                         g_guard_total_hits, eip, off);
            }
        } else {
            GuardEipStats& s = it->second;
            ++s.count;
            if (off < s.min_offset) s.min_offset = off;
            if (off > s.max_offset) s.max_offset = off;
        }

        if (g_guard_total_hits >= GUARD_MAX_TOTAL_HITS) {
            // Hard cap — disarm completely so nothing else fires.
            hook_log("GUARD: total-hit cap reached (%d), disarming\n",
                     GUARD_MAX_TOTAL_HITS);
            disarm_guard_locked();
            log_guard_summary_locked();
            if (g_guard_veh) {
                RemoveVectoredExceptionHandler(g_guard_veh);
                g_guard_veh = nullptr;
            }
        }
    }

    // IMPORTANT: no TF flag, no re-arm. PAGE_GUARD is cleared by the
    // CPU on this hit already. The instruction will retry and succeed.
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Opt-in install — only runs if guard_enable.txt exists in the game
// folder. Default behavior = no guard = safe run. After collecting the
// data you want, delete the marker file.
void bof4_install_init_dat_guard() {
    // Check opt-in marker
    if (GetFileAttributesA("guard_enable.txt") == INVALID_FILE_ATTRIBUTES) {
        return;  // silent skip — default safe behavior
    }

    void* base;
    {
        std::lock_guard<std::mutex> lk(g_known_dats_mtx);
        base = g_init_dat_base;
    }
    if (!base) {
        hook_log("GUARD: marker present but INIT.DAT base not captured; "
                 "skipping install\n");
        return;
    }

    // Entries 0 (0x24A6), 2 (0x63AA), 3 (0x65AA), 4 (0x67AA), 5 (0x6CFE).
    // Last entry ends at 0x6CFE + 0x554 = 0x7252.
    const uint32_t region_start_off = 0x24A6;
    const uint32_t region_end_off   = 0x7252;

    uintptr_t raw_start = (uintptr_t)base + region_start_off;
    uintptr_t raw_end   = (uintptr_t)base + region_end_off;

    const SIZE_T page = 0x1000;
    uintptr_t aligned_start = raw_start & ~(page - 1);
    uintptr_t aligned_end   = (raw_end + page - 1) & ~(page - 1);

    g_guard_start = (void*)aligned_start;
    g_guard_size  = aligned_end - aligned_start;

    // Capture the original page protection so we can restore it cleanly
    // on disarm. VirtualQuery returns the protection of the *region*
    // containing our start pointer; since all 6 pages are in one commit
    // they share the same base protection.
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(g_guard_start, &mbi, sizeof(mbi))) {
        hook_log("GUARD: VirtualQuery failed err=%u\n", GetLastError());
        return;
    }
    g_guard_orig_protect = mbi.Protect;

    hook_log("GUARD install (opt-in via guard_enable.txt):\n");
    hook_log("  watching INIT.DAT entries 0-5 (small uncompressed)\n");
    hook_log("  raw range       : %p .. %p  (file 0x%06X .. 0x%06X)\n",
             (void*)raw_start, (void*)raw_end,
             region_start_off, region_end_off);
    hook_log("  page-aligned    : %p .. %p  (%zu bytes, %zu pages)\n",
             g_guard_start, (char*)g_guard_start + g_guard_size,
             g_guard_size, g_guard_size / page);
    hook_log("  original protect: 0x%X  (restored on disarm)\n",
             g_guard_orig_protect);
    hook_log("  mode: one-shot per page, no re-arm\n");
    hook_log("  caps: max_unique_eips=%d max_total_hits=%d\n",
             GUARD_MAX_UNIQUE_EIPS, GUARD_MAX_TOTAL_HITS);

    g_guard_veh = AddVectoredExceptionHandler(1, guard_veh);
    if (!g_guard_veh) {
        hook_log("GUARD: AddVectoredExceptionHandler failed err=%u\n",
                 GetLastError());
        return;
    }

    DWORD old = 0;
    if (!VirtualProtect(g_guard_start, g_guard_size,
                        g_guard_orig_protect | PAGE_GUARD, &old)) {
        hook_log("GUARD: initial arm failed err=%u\n", GetLastError());
        RemoveVectoredExceptionHandler(g_guard_veh);
        g_guard_veh = nullptr;
        return;
    }
    g_guard_installed = true;
    hook_log("GUARD armed (one-shot per page, no re-arm)\n\n");
}

// Called from hook_bof4_shutdown. Always safe to call — restores the
// original page protection if the guard was installed, flushes the
// accumulated stats, and removes the exception handler.
static void bof4_uninstall_init_dat_guard() {
    std::lock_guard<std::mutex> lk(g_guard_mtx);
    if (g_guard_installed) {
        disarm_guard_locked();
        g_guard_installed = false;
    }
    log_guard_summary_locked();
    if (g_guard_veh) {
        RemoveVectoredExceptionHandler(g_guard_veh);
        g_guard_veh = nullptr;
    }
}

// ── VWF-memory page guard (diagnostic) ────────────────────────────────
// Watches the per-glyph advance tables (TABLE_A..E plus the dialog
// layout table) for memory READS via page-guard exceptions. Goal: find
// the read site(s) that drive width in code paths we haven't already
// redirected (e.g. the pause-menu item screen).
//
// Multiple non-contiguous pages are tracked, each as a separate guarded
// region with its own narrow address-range filter. Re-arms only after
// "interesting" hits (in-game-code EIPs touching our narrow ranges) to
// avoid an infinite re-arm loop with system-DLL memory scans of the
// same pages — that's the hang the previous single-region version hit.
//
// Opt-in via `vwf_guard_enable.txt` next to BOF4.exe. Permanently
// disarms after GUARD_VWF_MAX_HITS interesting hits.

struct VwfGuardRegion {
    void*       page_start;
    size_t      page_size;
    DWORD       orig_prot;
    uintptr_t   range_lo;
    uintptr_t   range_hi;
    const char* name;
};
static constexpr int VWF_GUARD_MAX = 4;
static VwfGuardRegion g_vwf_regions[VWF_GUARD_MAX] = {};
static int g_vwf_region_count = 0;
static PVOID g_vwf_guard_veh = nullptr;
static std::atomic<int> g_vwf_guard_hits{0};
static std::unordered_map<void*, int> g_vwf_eip_counts;
static std::mutex g_vwf_eips_mtx;
static constexpr int GUARD_VWF_MAX_HITS = 200;

// Only log EIPs that live inside BOF4.exe (0x00400000..0x00B00000-ish).
// System DLLs from above 0x6FFFFFFF (kernel32, ntdll, d3d9, etc.) are
// excluded — we only care about the game's own read sites.
static constexpr uintptr_t VWF_BOF4_LO = 0x00400000;
static constexpr uintptr_t VWF_BOF4_HI = 0x00B00000;

static const VwfGuardRegion* vwf_find_region(uintptr_t addr) {
    for (int i = 0; i < g_vwf_region_count; ++i) {
        const VwfGuardRegion& r = g_vwf_regions[i];
        uintptr_t ps = (uintptr_t)r.page_start;
        if (addr >= ps && addr < ps + r.page_size) return &r;
    }
    return nullptr;
}

static LONG NTAPI vwf_guard_veh(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    if (ep->ExceptionRecord->ExceptionCode !=
        STATUS_GUARD_PAGE_VIOLATION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    uintptr_t addr = ep->ExceptionRecord->ExceptionInformation[1];
    const VwfGuardRegion* region = vwf_find_region(addr);
    if (!region) return EXCEPTION_CONTINUE_SEARCH;

    void* eip = ep->ExceptionRecord->ExceptionAddress;
    uintptr_t eip_v = (uintptr_t)eip;
    bool of_interest =
        (addr >= region->range_lo && addr < region->range_hi) &&
        (eip_v >= VWF_BOF4_LO && eip_v < VWF_BOF4_HI);

    if (!of_interest) {
        // Intentionally DO NOT re-arm. PAGE_GUARD auto-cleared on this
        // fault, the access retries and succeeds, and the page stays
        // unguarded until we next observe an interesting hit (which
        // re-arms below). Avoids infinite re-arm/re-fault on system
        // DLLs that scan the page.
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    int n = g_vwf_guard_hits.fetch_add(1) + 1;
    {
        std::lock_guard<std::mutex> lk(g_vwf_eips_mtx);
        int& c = g_vwf_eip_counts[eip];
        c++;
        if (c <= 3) {
            hook_log(
                "VWF_GUARD hit #%d: eip=%p  addr=0x%08X (%s+0x%03X)\n",
                n, eip, (unsigned)addr, region->name,
                (unsigned)(addr - region->range_lo));
        }
    }
    if (n >= GUARD_VWF_MAX_HITS) {
        for (int i = 0; i < g_vwf_region_count; ++i) {
            DWORD old = 0;
            VirtualProtect(g_vwf_regions[i].page_start,
                           g_vwf_regions[i].page_size,
                           g_vwf_regions[i].orig_prot, &old);
        }
        hook_log(
            "VWF_GUARD: hit cap reached (%d), permanently disarmed\n",
            GUARD_VWF_MAX_HITS);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    // Re-arm just the hit region's page so subsequent reads keep
    // faulting.
    uintptr_t page_base = addr & ~(uintptr_t)0xFFF;
    DWORD old_prot = 0;
    VirtualProtect((void*)page_base, 0x1000,
                   region->orig_prot | PAGE_GUARD, &old_prot);
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void bof4_install_vwf_guard() {
    // Opt-in via marker file — default is safe (guard skipped).
    if (GetFileAttributesA("vwf_guard_enable.txt") ==
        INVALID_FILE_ATTRIBUTES) {
        return;
    }
    struct RawRange {
        uintptr_t   lo;
        uintptr_t   hi;
        const char* name;
    };
    // Each entry: narrow byte range inside one .data page. The whole
    // page gets PAGE_GUARD; the lo/hi pair filters the VEH so we only
    // log hits inside the actual table bytes (no neighbouring noise).
    static const RawRange ranges[] = {
        // TABLE_E — dialog/message-box width source. Char-indexed.
        { 0x0094CD3C, 0x0094CD3C + 0x200, "TABLE_E"   },
        // TABLE_F — agent-discovered candidate at 0x00950740, read by
        // paired site 0x005B1A80/0x005B1A8A (mode (char-0x20)*2 + IMM).
        // Not in any current redirect; possible menu source.
        { 0x00950740, 0x00950740 + 0x200, "TABLE_F"   },
        // TABLE_A/B/C/D — per-glyph advance + message-box mirrors.
        { 0x00952D00, 0x00953000,         "TABLE_A_D" },
        // Layout/secondary metric tables (dialog wrap pass).
        { 0x0095B750, 0x0095B900,         "LAYOUT"    },
    };
    constexpr SIZE_T page = 0x1000;
    int n = (int)(sizeof(ranges) / sizeof(ranges[0]));
    if (n > VWF_GUARD_MAX) n = VWF_GUARD_MAX;
    for (int i = 0; i < n; ++i) {
        uintptr_t aligned_start = ranges[i].lo & ~(page - 1);
        uintptr_t aligned_end   = (ranges[i].hi + page - 1) & ~(page - 1);
        void*  ps = (void*)aligned_start;
        size_t sz = aligned_end - aligned_start;
        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery(ps, &mbi, sizeof(mbi))) {
            hook_log("VWF_GUARD: VirtualQuery %p failed err=%u (%s)\n",
                     ps, GetLastError(), ranges[i].name);
            continue;
        }
        DWORD orig_prot = mbi.Protect;
        DWORD old = 0;
        if (!VirtualProtect(ps, sz, orig_prot | PAGE_GUARD, &old)) {
            hook_log("VWF_GUARD: initial arm %p failed err=%u (%s)\n",
                     ps, GetLastError(), ranges[i].name);
            continue;
        }
        g_vwf_regions[g_vwf_region_count++] = {
            ps, sz, orig_prot, ranges[i].lo, ranges[i].hi, ranges[i].name
        };
        hook_log(
            "VWF_GUARD armed %-10s page %p..%p (range 0x%08X..0x%08X)\n",
            ranges[i].name, ps, (char*)ps + sz,
            (unsigned)ranges[i].lo, (unsigned)ranges[i].hi);
    }
    if (g_vwf_region_count == 0) {
        hook_log("VWF_GUARD: no regions installed; not adding VEH\n");
        return;
    }
    g_vwf_guard_veh = AddVectoredExceptionHandler(1, vwf_guard_veh);
    if (!g_vwf_guard_veh) {
        hook_log(
            "VWF_GUARD: AddVEH failed err=%u — disarming all regions\n",
            GetLastError());
        for (int i = 0; i < g_vwf_region_count; ++i) {
            DWORD old = 0;
            VirtualProtect(g_vwf_regions[i].page_start,
                           g_vwf_regions[i].page_size,
                           g_vwf_regions[i].orig_prot, &old);
        }
        g_vwf_region_count = 0;
        return;
    }
    hook_log(
        "VWF_GUARD: VEH installed, %d region(s), cap %d hits; first 3 "
        "lines per unique EIP\n",
        g_vwf_region_count, GUARD_VWF_MAX_HITS);
}

static void bof4_uninstall_vwf_guard() {
    for (int i = 0; i < g_vwf_region_count; ++i) {
        DWORD old = 0;
        VirtualProtect(g_vwf_regions[i].page_start,
                       g_vwf_regions[i].page_size,
                       g_vwf_regions[i].orig_prot, &old);
    }
    g_vwf_region_count = 0;
    if (g_vwf_guard_veh) {
        RemoveVectoredExceptionHandler(g_vwf_guard_veh);
        g_vwf_guard_veh = nullptr;
    }
    std::lock_guard<std::mutex> lk(g_vwf_eips_mtx);
    if (g_vwf_eip_counts.empty()) return;
    hook_log("\n=== VWF_GUARD summary ===\n");
    std::vector<std::pair<void*, int>> sorted(
        g_vwf_eip_counts.begin(), g_vwf_eip_counts.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b){
                  return a.second > b.second;
              });
    for (const auto& kv : sorted) {
        hook_log("  eip=%p  hits=%d\n", kv.first, kv.second);
    }
    hook_log("\n");
}

// ── Recent-DAT ring (rect-hook scene clustering) ──────────────────────────
// The textured-rect inventory can't tell which DAT backs a screen — the GPU
// packet carries only u/v/clut, and the atlas was uploaded to VRAM earlier. So
// we remember the last few *.DAT basenames the engine opened; rect_hook stamps
// each scene with whatever was resident when its first label appeared, naming
// the atlas. Newest-first, distinct, mutex-guarded. Recorded only while the
// inventory or DAT trace is on (cheap, but no need otherwise).
static std::mutex g_recent_dat_mtx;
static char       g_recent_dat[6][64];
static int        g_recent_dat_n = 0;
static char       g_last_dat_any[64] = {0};   // last *.DAT opened, NO whitelist —
                                              // fallback when no editable DAT fired
static char       g_cur_area[16] = {0};       // last AREA*.DAT basename (no ext), UPPER

// Accessor (declared in hooks.h) — current field area for free_camera gating.
void bof4_current_area(char* out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = 0;
    std::lock_guard<std::mutex> lk(g_recent_dat_mtx);
    if (g_cur_area[0]) strncpy_s(out, cap, g_cur_area, _TRUNCATE);
}

// Whitelist = the DATs you're actively editing, i.e. every *.DAT in the game's
// graphics_to_edit\ folder. Only these get into the recent-DAT ring, so a scene's
// label names exactly the editable atlas that backs it (area/text/sound DATs that
// happen to open at the same time are filtered out). Rebuilt on each F7 so newly
// dropped-in DATs are picked up without a restart. If the folder is missing/empty
// the filter is OFF (every *.DAT is recorded) so behaviour degrades gracefully.
static std::mutex                       g_dat_wl_mtx;
static std::unordered_set<std::string>  g_dat_whitelist;
static bool                             g_dat_wl_loaded = false;

static void lower_copy(char* out, size_t cap, const char* in) {
    size_t i = 0;
    for (; in[i] && i < cap - 1; i++) {
        char c = in[i];
        out[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    out[i] = 0;
}

static void load_dat_whitelist_locked() {   // caller holds g_dat_wl_mtx
    g_dat_whitelist.clear();
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char* s = strrchr(exe, '\\'); if (s) *(s + 1) = 0; else exe[0] = 0;
    char pat[MAX_PATH];
    snprintf(pat, sizeof(pat), "%sgraphics_to_edit\\*.DAT", exe);  // FindFirstFile is case-insensitive
    WIN32_FIND_DATAA fd; HANDLE hf = FindFirstFileA(pat, &fd);
    if (hf != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            char low[64]; lower_copy(low, sizeof(low), fd.cFileName);
            g_dat_whitelist.insert(low);
        } while (FindNextFileA(hf, &fd));
        FindClose(hf);
    }
    g_dat_wl_loaded = true;
}

// Called from rect_hook.cpp on F7 so the whitelist tracks the folder live.
void bof4_reload_dat_whitelist() {
    std::lock_guard<std::mutex> lk(g_dat_wl_mtx);
    load_dat_whitelist_locked();
    hook_log("[rect] whitelist: %zu editable DAT(s) in graphics_to_edit\\\n",
             g_dat_whitelist.size());
}

static void record_recent_dat(const char* path) {
    if (!path) return;
    const char* b = strrchr(path, '\\'); b = b ? b + 1 : path;
    const char* b2 = strrchr(b, '/');    if (b2) b = b2 + 1;
    size_t len = strlen(b);
    if (len < 4 || _stricmp(b + len - 4, ".DAT") != 0) return;   // *.DAT only
    // Decide whitelist membership (graphics_to_edit\). Empty list = filter off.
    bool whitelisted;
    {
        std::lock_guard<std::mutex> lk(g_dat_wl_mtx);
        if (!g_dat_wl_loaded) load_dat_whitelist_locked();
        if (g_dat_whitelist.empty()) {
            whitelisted = true;
        } else {
            char low[64]; lower_copy(low, sizeof(low), b);
            whitelisted = g_dat_whitelist.find(low) != g_dat_whitelist.end();
        }
    }
    std::lock_guard<std::mutex> lk(g_recent_dat_mtx);
    strncpy_s(g_last_dat_any, sizeof(g_last_dat_any), b, _TRUNCATE);  // fallback (always)
    if (!whitelisted) return;     // not an editable DAT — keep out of the ring
    int found = -1;
    for (int i = 0; i < g_recent_dat_n; i++)
        if (!_stricmp(g_recent_dat[i], b)) { found = i; break; }
    int start = (found >= 0) ? found
                            : (g_recent_dat_n < 6 ? g_recent_dat_n++ : 5);
    for (int i = start; i > 0; --i)
        memcpy(g_recent_dat[i], g_recent_dat[i - 1], sizeof(g_recent_dat[0]));
    strncpy_s(g_recent_dat[0], sizeof(g_recent_dat[0]), b, _TRUNCATE);
}

// Reader used by rect_hook.cpp (forward-declared there): the SINGLE last-fired
// editable DAT (top of the whitelist ring) — for a menu that's the menu's own DAT
// (e.g. SAGMEN*.DAT), which is exactly the atlas we want. If no whitelisted DAT
// fired for this scene, fall back to the last *.DAT opened at all, tagged so it's
// clear it's a guess. Empty string only if nothing opened yet.
void bof4_scene_dat(char* out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = 0;
    std::lock_guard<std::mutex> lk(g_recent_dat_mtx);
    if (g_recent_dat_n > 0)
        strncpy_s(out, cap, g_recent_dat[0], _TRUNCATE);       // last whitelisted
    else if (g_last_dat_any[0])
        snprintf(out, cap, "%s (fallback)", g_last_dat_any);   // none editable
}

// Raw most-recent *.DAT opened (any, whitelist-independent). Used by tex_prov.cpp
// to attribute a VRAM upload to the DAT that was loading at the time.
void bof4_recent_dat0(char* out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = 0;
    std::lock_guard<std::mutex> lk(g_recent_dat_mtx);
    if (g_last_dat_any[0])
        strncpy_s(out, cap, g_last_dat_any, _TRUNCATE);
}

// ── CreateFileA hook ──────────────────────────────────────────────────────
typedef HANDLE (WINAPI *CreateFileA_t)(
    LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static CreateFileA_t g_orig_CreateFileA = nullptr;

static HANDLE WINAPI hook_CreateFileA(
    LPCSTR lpFileName,
    DWORD dwDesiredAccess,
    DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes,
    HANDLE hTemplateFile)
{
    HANDLE h = g_orig_CreateFileA(lpFileName, dwDesiredAccess, dwShareMode,
                                   lpSecurityAttributes, dwCreationDisposition,
                                   dwFlagsAndAttributes, hTemplateFile);
    // Subtitle overlay: note when the intro movie (ZBOF4.DAT) opens. Filters
    // internally, so non-movie opens cost a strlen + suffix check.
    subtitles_note_file_open(h, lpFileName);
    // Track the current field area (last AREA*.DAT opened) for free_camera gating.
    // Cheap prefix check; runs regardless of config flags.
    if (lpFileName && h != INVALID_HANDLE_VALUE) {
        const char* b = strrchr(lpFileName, '\\'); b = b ? b + 1 : lpFileName;
        if ((b[0]=='A'||b[0]=='a')&&(b[1]=='R'||b[1]=='r')&&
            (b[2]=='E'||b[2]=='e')&&(b[3]=='A'||b[3]=='a')) {
            std::lock_guard<std::mutex> lk(g_recent_dat_mtx);
            size_t i = 0;
            for (; b[i] && b[i] != '.' && i < sizeof(g_cur_area)-1; ++i)
                g_cur_area[i] = (char)toupper((unsigned char)b[i]);
            g_cur_area[i] = 0;
        }
    }
    if (lpFileName && is_interesting_filename(lpFileName)) {
        if (h != INVALID_HANDLE_VALUE) {
            remember_handle(h, lpFileName);
            // Remember it for rect_hook's scene clustering (which DAT backs a screen).
            if (g_cfg.rect_trace || g_cfg.dat_log || g_cfg.tex_prov_log) record_recent_dat(lpFileName);
            // DAT-open trace (gated on rect_trace): logs every DAT/CFG the engine
            // opens, so we can see which file backs a given menu/area. Grep
            // [dat-open] in d3d9_hook.log after entering the scene.
            if (g_cfg.dat_log)
                hook_log("[dat-open] CreateFileA(\"%s\")\n", lpFileName);
        }
        // BGM-loop diagnostics: log any BGM*.DAT open with the BOF4.exe
        // return address so we can find the audio-engine code path.
        const char* base = strrchr(lpFileName, '\\');
        base = base ? base + 1 : lpFileName;
        if ((base[0] == 'B' || base[0] == 'b') &&
            (base[1] == 'G' || base[1] == 'g') &&
            (base[2] == 'M' || base[2] == 'm')) {
            void* ra = _ReturnAddress();
            uintptr_t rip = (uintptr_t)ra;
            uintptr_t bof4_base = (uintptr_t)GetModuleHandleA(nullptr);
            // Resolve which module contains the return address. If ra is in
            // dynamically-allocated memory (packer, JIT, injected runtime),
            // GetModuleHandleEx returns nullptr and we fall back to
            // VirtualQuery to log the allocation base.
            HMODULE owner = nullptr;
            char modname[MAX_PATH] = "<unknown>";
            uintptr_t mod_base = 0;
            if (GetModuleHandleExA(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    (LPCSTR)ra, &owner) && owner) {
                GetModuleFileNameA(owner, modname, MAX_PATH);
                mod_base = (uintptr_t)owner;
            } else {
                MEMORY_BASIC_INFORMATION mbi{};
                if (VirtualQuery(ra, &mbi, sizeof(mbi)) == sizeof(mbi)) {
                    mod_base = (uintptr_t)mbi.AllocationBase;
                    snprintf(modname, sizeof(modname),
                             "<alloc base=0x%p state=0x%X protect=0x%X>",
                             mbi.AllocationBase,
                             (unsigned)mbi.State, (unsigned)mbi.Protect);
                }
            }
            hook_log("[bgm-fileopen] CreateFileA(\"%s\") -> h=0x%p\n"
                     "    ra=0x%p (BOF4+0x%X)  in: %s @ 0x%p (off+0x%X)\n",
                     lpFileName, h, ra,
                     (unsigned)(rip - bof4_base),
                     modname, (void*)mod_base,
                     (unsigned)(rip - mod_base));
        }
    }
    return h;
}

// ── CreateFileW hook ──────────────────────────────────────────────────────
typedef HANDLE (WINAPI *CreateFileW_t)(
    LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static CreateFileW_t g_orig_CreateFileW = nullptr;

static HANDLE WINAPI hook_CreateFileW(
    LPCWSTR lpFileName,
    DWORD dwDesiredAccess,
    DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes,
    HANDLE hTemplateFile)
{
    HANDLE h = g_orig_CreateFileW(lpFileName, dwDesiredAccess, dwShareMode,
                                   lpSecurityAttributes, dwCreationDisposition,
                                   dwFlagsAndAttributes, hTemplateFile);
    if (lpFileName) {
        // Narrow down to ASCII for logging + filtering. Game file names are
        // all ASCII so a lossy conversion is fine.
        char narrow[MAX_PATH] = {0};
        int n = WideCharToMultiByte(CP_ACP, 0, lpFileName, -1,
                                     narrow, MAX_PATH, nullptr, nullptr);
        // Subtitle overlay: note the intro movie opening (wide-char path).
        if (n > 0) subtitles_note_file_open(h, narrow);
        if (n > 0 && is_interesting_filename(narrow)) {
            if (h != INVALID_HANDLE_VALUE) {
                remember_handle(h, narrow);
                if (g_cfg.rect_trace || g_cfg.dat_log || g_cfg.tex_prov_log) record_recent_dat(narrow);
                if (g_cfg.dat_log)
                    hook_log("[dat-open] CreateFileW(\"%s\")\n", narrow);
            }
            const char* base = strrchr(narrow, '\\');
            base = base ? base + 1 : narrow;
            if ((base[0] == 'B' || base[0] == 'b') &&
                (base[1] == 'G' || base[1] == 'g') &&
                (base[2] == 'M' || base[2] == 'm')) {
                void* ra = _ReturnAddress();
                uintptr_t rip = (uintptr_t)ra;
                uintptr_t bof4_base = (uintptr_t)GetModuleHandleA(nullptr);
                hook_log("[bgm-fileopen] CreateFileW(\"%s\") -> h=0x%p  "
                         "ra=0x%p (BOF4+0x%X)\n",
                         narrow, h, ra,
                         (unsigned)(rip - bof4_base));
            }
        }
    }
    return h;
}

// ── ReadFile hook ─────────────────────────────────────────────────────────
typedef BOOL (WINAPI *ReadFile_t)(
    HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
static ReadFile_t g_orig_ReadFile = nullptr;

static BOOL WINAPI hook_ReadFile(
    HANDLE hFile,
    LPVOID lpBuffer,
    DWORD nNumberOfBytesToRead,
    LPDWORD lpNumberOfBytesRead,
    LPOVERLAPPED lpOverlapped)
{
    TrackedFile info;
    bool tracked = lookup_handle(hFile, &info);

    // Pre-read position snapshot for logging. Overlapped reads supply
    // their own offset; for non-overlapped reads we use our tracked pos.
    uint64_t pos_before = info.pos;
    if (lpOverlapped) {
        pos_before =
            ((uint64_t)lpOverlapped->OffsetHigh << 32) | lpOverlapped->Offset;
    }

    BOOL ok = g_orig_ReadFile(hFile, lpBuffer, nNumberOfBytesToRead,
                               lpNumberOfBytesRead, lpOverlapped);

    if (tracked) {
        DWORD bytes = lpNumberOfBytesRead ? *lpNumberOfBytesRead : 0;
        if (ok && bytes > 0 && !lpOverlapped) {
            // Non-overlapped reads advance the file pointer by `bytes`
            advance_handle_pos(hFile, bytes);
        }
        // Capture base pointer of known DATs on their first read.
        if (ok && bytes > 0) {
            maybe_capture_init_dat_base(info, pos_before, lpBuffer);
        }
    }

    return ok;
}

// ── SetFilePointer hook ───────────────────────────────────────────────────
typedef DWORD (WINAPI *SetFilePointer_t)(
    HANDLE, LONG, PLONG, DWORD);
static SetFilePointer_t g_orig_SetFilePointer = nullptr;

static DWORD WINAPI hook_SetFilePointer(
    HANDLE hFile,
    LONG lDistanceToMove,
    PLONG lpDistanceToMoveHigh,
    DWORD dwMoveMethod)
{
    TrackedFile info;
    bool tracked = lookup_handle(hFile, &info);

    DWORD low = g_orig_SetFilePointer(hFile, lDistanceToMove,
                                       lpDistanceToMoveHigh, dwMoveMethod);

    if (tracked && low != INVALID_SET_FILE_POINTER) {
        LONG high = lpDistanceToMoveHigh ? *lpDistanceToMoveHigh : 0;
        uint64_t new_pos = ((uint64_t)(uint32_t)high << 32)
                         |  (uint64_t)low;
        update_handle_pos(hFile, new_pos);
    }

    return low;
}

// ── CloseHandle hook ──────────────────────────────────────────────────────
// CloseHandle is called for every kind of kernel object. We only log if
// the handle is in our tracked-files map.
typedef BOOL (WINAPI *CloseHandle_t)(HANDLE);
static CloseHandle_t g_orig_CloseHandle = nullptr;

static BOOL WINAPI hook_CloseHandle(HANDLE hObject) {
    // Subtitle overlay: if this is the last movie-file handle closing, the
    // movie has ended — clear the overlay. Cheap no-op when no movie active.
    subtitles_note_file_close(hObject);
    TrackedFile info;
    bool tracked = lookup_handle(hObject, &info);
    if (tracked) {
        // If the game just finished loading INIT.DAT into RAM, parse its
        // TOC directly from the buffer. Runs exactly once per close
        // event. The parse itself is silent now; it only dumps
        // init_loaded.bin if the side-channel call path still uses it.
        if (_stricmp(info.name.c_str(), "INIT.DAT") == 0) {
            parse_and_dump_init_dat(info);
        }
        forget_handle(hObject);
    }
    return g_orig_CloseHandle(hObject);
}

// ── GDI font hooks ───────────────────────────────────────────────────────
// Log every GDI font creation so we can identify the typeface GOG's
// ddraw.dll uses for its runtime-rendered high-res font. Nothing is
// changed — we pass all args through to the original API.
typedef HFONT (WINAPI *CreateFontA_t)(int, int, int, int, int,
                                       DWORD, DWORD, DWORD,
                                       DWORD, DWORD, DWORD, DWORD,
                                       DWORD, LPCSTR);
static CreateFontA_t g_orig_CreateFontA = nullptr;

static HFONT WINAPI hook_CreateFontA(
    int nHeight, int nWidth, int nEscapement, int nOrientation,
    int fnWeight, DWORD fdwItalic, DWORD fdwUnderline, DWORD fdwStrikeOut,
    DWORD fdwCharSet, DWORD fdwOutputPrecision, DWORD fdwClipPrecision,
    DWORD fdwQuality, DWORD fdwPitchAndFamily, LPCSTR lpszFace)
{
    return g_orig_CreateFontA(nHeight, nWidth, nEscapement, nOrientation,
                              fnWeight, fdwItalic, fdwUnderline, fdwStrikeOut,
                              fdwCharSet, fdwOutputPrecision, fdwClipPrecision,
                              fdwQuality, fdwPitchAndFamily, lpszFace);
}

typedef HFONT (WINAPI *CreateFontIndirectA_t)(const LOGFONTA*);
static CreateFontIndirectA_t g_orig_CreateFontIndirectA = nullptr;

static HFONT WINAPI hook_CreateFontIndirectA(const LOGFONTA* lf) {
    return g_orig_CreateFontIndirectA(lf);
}

typedef int (WINAPI *AddFontResourceA_t)(LPCSTR);
static AddFontResourceA_t g_orig_AddFontResourceA = nullptr;

static int WINAPI hook_AddFontResourceA(LPCSTR path) {
    return g_orig_AddFontResourceA(path);
}

typedef int (WINAPI *AddFontResourceExA_t)(LPCSTR, DWORD, PVOID);
static AddFontResourceExA_t g_orig_AddFontResourceExA = nullptr;

static int WINAPI hook_AddFontResourceExA(LPCSTR path, DWORD fl, PVOID pv) {
    return g_orig_AddFontResourceExA(path, fl, pv);
}

// ── Install helpers ──────────────────────────────────────────────────────
static const char* mh_status(MH_STATUS s) {
    switch (s) {
    case MH_OK:                         return "OK";
    case MH_ERROR_ALREADY_INITIALIZED:  return "ALREADY_INITIALIZED";
    case MH_ERROR_NOT_INITIALIZED:      return "NOT_INITIALIZED";
    case MH_ERROR_ALREADY_CREATED:      return "ALREADY_CREATED";
    case MH_ERROR_NOT_CREATED:          return "NOT_CREATED";
    case MH_ERROR_ENABLED:              return "ENABLED";
    case MH_ERROR_DISABLED:             return "DISABLED";
    case MH_ERROR_NOT_EXECUTABLE:       return "NOT_EXECUTABLE";
    case MH_ERROR_UNSUPPORTED_FUNCTION: return "UNSUPPORTED_FUNCTION";
    case MH_ERROR_MEMORY_ALLOC:         return "MEMORY_ALLOC";
    case MH_ERROR_MEMORY_PROTECT:       return "MEMORY_PROTECT";
    case MH_ERROR_MODULE_NOT_FOUND:     return "MODULE_NOT_FOUND";
    case MH_ERROR_FUNCTION_NOT_FOUND:   return "FUNCTION_NOT_FOUND";
    default:                            return "UNKNOWN";
    }
}

static void report_bof4_module() {
    HMODULE mod = GetModuleHandleA(nullptr);
    if (!mod) {
        hook_log("bof4: GetModuleHandle(NULL) failed\n");
        return;
    }
    char name[MAX_PATH] = "?";
    GetModuleFileNameA(mod, name, MAX_PATH);
    const char* base = strrchr(name, '\\');
    if (base) ++base; else base = name;
    hook_log("bof4: main module '%s' base=%p\n", base, (void*)mod);
}

// Create-and-enable one hook with a consistent log line on failure.
template<typename Fn>
static bool install_hook(const char* module, const char* sym,
                         void* detour, Fn** orig_out) {
    HMODULE m = GetModuleHandleA(module);
    if (!m) {
        hook_log("bof4: GetModuleHandle(%s) failed\n", module);
        return false;
    }
    void* target = (void*)GetProcAddress(m, sym);
    if (!target) {
        hook_log("bof4: GetProcAddress(%s!%s) failed\n", module, sym);
        return false;
    }
    MH_STATUS st = MH_CreateHook(target, detour, (void**)orig_out);
    if (st != MH_OK) {
        hook_log("bof4: MH_CreateHook(%s) failed: %s\n", sym, mh_status(st));
        return false;
    }
    st = MH_EnableHook(target);
    if (st != MH_OK) {
        hook_log("bof4: MH_EnableHook(%s) failed: %s\n", sym, mh_status(st));
        return false;
    }
    hook_log("bof4: %s!%s hooked @ %p\n", module, sym, target);
    return true;
}

// ── (removed) BoF4 glyph/font-load diagnostic hooks ───────────────
// The F_B (0x408820) / F_A (0x4088D0) / glyph_src (0x403D20) /
// load_glyph_file (0x4025A0) hooks were reverse-engineering instrumentation
// from the font investigation (now concluded). They only wrote diagnostic
// dumps (font_atlas_log.txt, font_ram_dump.*, font_src_dump.bin,
// glyph_file_load_log.txt, glyph_file_dumps\) and were not part of the
// working localization (umlaut glyphs come from the INIT.DAT atlas; widths
// from the VWF tables + dialog_width hook). Removed 2026-08-03. The VWF
// config loader that used to live in install_loadglyph_hook() below is kept.

// ── signature table for the MinHook detour targets in this file ──────────
// Each detour is located by its function-prologue signature so the hook
// survives EXE recompiles. Resolved-vs-hardcoded status is verified in both
// the live build and the 2026-05 GOG recompile:
//   dialog_width  0x006780E0 -> recompile 0x00677F90  (relocates)
//   F_B loadglyph 0x00408820 -> recompile 0x00408820  (stable)
//   F_A upload    0x004088D0 -> recompile 0x004088D0  (stable)
//   glyph_src     0x00403D20 -> recompile 0x00403D20  (stable)
//   load_glyph    0x004025A0 -> recompile 0x004025A0  (stable)
namespace {
const uint8_t SIG_DIALOG_WIDTH[] = {
    0x83,0xEC,0x0C,0x8B,0x44,0x24,0x1C,0x53,0x25,0xFF,0x00,0x00,0x00,0x55,0xC1,0xE8,0x04,0x56,0x57,
};
// Resolve a detour target by signature, falling back to the hardcoded VA.
uintptr_t sig_addr(const char* name, const uint8_t* pat, const char* mask,
                   size_t len, uintptr_t fb) {
    uintptr_t va = resolve_sig(name, pat, mask, len, 0);
    return va ? va : fb;
}
}  // namespace

// Apply the VWF width patches (16px table relocation + vwf_config.txt +
// TABLE_E). Formerly this also installed the four glyph/font-load diagnostic
// hooks (removed 2026-08-03); only the load-bearing VWF work remains.
// ── Live single-glyph VWF poke (in-game config overlay) ───────────────────
// Mirrors the per-char writes in install_vwf_config() for ONE glyph so the
// overlay's VWF tab can nudge spacing and see it immediately (no game reload).
// Same tables, same VirtualProtect discipline, same indexing. Guards on the
// expected ImageBase so it never writes to a moved data address on a foreign EXE.
bool vwf_live_poke(int ch, int bearing, int advance, bool have_bearing, bool is16px) {
    if (ch < 0x20 || ch > 0xFF) return false;
    if ((uintptr_t)GetModuleHandleA(nullptr) != 0x00400000) return false;
    const uint8_t adv = (uint8_t)(advance & 0xFF);
    const uint8_t bea = (uint8_t)(bearing & 0xFF);
    const int idx0 = (ch - 0x20) * 2;   // TABLE_A / 16px layout: (ch-0x20)*2
    const int idx1 = idx0 + 1;
    if (idx1 >= 0x200) return false;

    if (is16px) {                       // private buffer — no VirtualProtect needed
        g_vwf_b16[idx1] = adv;
        if (have_bearing) g_vwf_b16[idx0] = bea;
        return true;
    }

    uint8_t* vwf  = (uint8_t*)0x00952EB0;   // TABLE_A  (ch-0x20)*2
    uint8_t* vwfE = (uint8_t*)0x0094CD3C;   // TABLE_E  ch*2 (dialog width source)
    DWORD opA = 0, opE = 0;
    const bool aOK = VirtualProtect(vwf,  0x200, PAGE_READWRITE, &opA) != 0;
    const bool eOK = VirtualProtect(vwfE, 0x200, PAGE_READWRITE, &opE) != 0;
    if (aOK) {
        vwf[idx1] = adv;
        if (have_bearing) vwf[idx0] = bea;
    }
    if (eOK) {
        const int e0 = ch * 2, e1 = e0 + 1;
        if (e1 < 0x200) { vwfE[e1] = adv; if (have_bearing) vwfE[e0] = bea; }
    }
    { const int a0 = ch * 2, a1 = a0 + 1;   // g_vwf_alt (alt-operand redirect)
      if (a1 < (int)sizeof(g_vwf_alt)) {
          g_vwf_alt[a1] = adv; if (have_bearing) g_vwf_alt[a0] = bea; } }
    DWORD tmp;
    if (aOK) VirtualProtect(vwf,  0x200, opA, &tmp);
    if (eOK) VirtualProtect(vwfE, 0x200, opE, &tmp);
    return aOK;
}

// Read the live width metrics for one glyph (for the overlay's initial values).
bool vwf_live_read(int ch, bool is16px, int* out_bearing, int* out_advance) {
    if (ch < 0x20 || ch > 0xFF) return false;
    if ((uintptr_t)GetModuleHandleA(nullptr) != 0x00400000) return false;
    const int idx0 = (ch - 0x20) * 2, idx1 = idx0 + 1;
    if (idx1 >= 0x200) return false;
    uint8_t b0 = 0, b1 = 0;
    if (is16px) { b0 = g_vwf_b16[idx0]; b1 = g_vwf_b16[idx1]; }
    else {
        __try {
            b0 = ((const volatile uint8_t*)0x00952EB0)[idx0];
            b1 = ((const volatile uint8_t*)0x00952EB0)[idx1];
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    if (out_bearing) *out_bearing = (b0 < 128) ? b0 : b0 - 256;
    if (out_advance) *out_advance = (b1 < 128) ? b1 : b1 - 256;
    return true;
}

static void install_vwf_config() {
    HMODULE exe = GetModuleHandleA(nullptr);
    if (!exe) {
        hook_log("bof4: VWF: GetModuleHandle(NULL) failed\n");
        return;
    }
    // The VWF patches use hardcoded VAs, so require the preferred ImageBase.
    if ((uintptr_t)exe != 0x00400000) {
        hook_log("bof4: VWF: unexpected BoF4 base %p (expected 0x00400000) "
                 "— skipping VWF patches\n", (void*)exe);
        return;
    }

    // VWF config-file loader: read vwf_config.txt from the game
    // folder and patch the VWF bytes for each listed char.
    // Formats (one line per char, # is a comment):
    //   "0xCC = name : advance"            (byte+1 only)
    //   "0xCC = name : bearing : advance"  (byte+0 AND byte+1)
    // byte+0 = left bearing: cursor is drawn at (cursor + byte+0).
    // byte+1 = advance: cursor moves forward by byte+1 after drawing.
    {
        // TABLE_B / TABLE_C mirror disabled: aliased writes still
        // corrupt the name-entry command boxes. Those boxes auto-size
        // their text so they don't need our width patching anyway.
        // TABLE_E is the MESSAGE-BOX / DIALOG width source — found
        // 2026-04-18 via x32dbg breakpoint + static analysis. Writing
        // to it has no aliasing (it's in a separate region at
        // 0x0094CD3C). This is what makes dialog umlauts render with
        // correct widths.
        constexpr bool PATCH_BEARING          = true;
        constexpr bool PATCH_TABLE_B          = false;
        constexpr bool PATCH_TABLE_C          = false;
        constexpr bool PATCH_TABLE_E          = true;   // dialog
        constexpr bool REDIRECT_ALT_OPERANDS  = true;

        constexpr uintptr_t VWF_TABLE_A = 0x00952EB0;
        constexpr uintptr_t VWF_TABLE_B = 0x00952DE0;
        constexpr uintptr_t VWF_TABLE_C = 0x00952D28;
        // TABLE_E: the dialog/message-box width source. Indexed by
        // char*2 (NOT (char-0x20)*2); chars 0x20+ have standard b0/b1
        // entries. Read by 0x00527E60 (movsx edx, byte [ecx*2 + ...])
        // with the accumulator at 0x00527EEF. Found by x32dbg memory
        // breakpoint on the dialog text buffer.
        constexpr uintptr_t VWF_TABLE_E = 0x0094CD3C;
        uint8_t* vwf  = (uint8_t*)VWF_TABLE_A;
        uint8_t* vwfB = (uint8_t*)VWF_TABLE_B;
        uint8_t* vwfC = (uint8_t*)VWF_TABLE_C;
        uint8_t* vwfE = (uint8_t*)VWF_TABLE_E;

        // ── 16px-font table relocation ──────────────────────────────────
        // Give the 16px font a PRIVATE copy of TABLE_B so its widths (incl.
        // umlauts) can be set independently of TABLE_A, which stock TABLE_B
        // overlaps. Copy the stock bytes, then repoint the 4 instructions that
        // read TABLE_B (found via xref: 0x6299E2/EE + 0x62A62B/37) at g_vwf_b16.
        // Behavior-neutral until the [16px] section writes 16px-specific values.
        //
        // These four disp32-operand rewrites (and the alt-operand redirects
        // below) are NOT signaturized: they live inside the VWF glyph renderer
        // (0x629xxx), which is fully recompiled on a major EXE update — so the
        // whole renderer patch set needs re-RE there regardless of address
        // relocation. Each rewrite instead self-guards: it only fires when the
        // current disp equals the expected stock TABLE_B address, else it SKIPs
        // (logged) — fail-safe on any changed EXE.
        // Set >0 iff the 16px reloc recognised the stock renderer (its reader
        // disps still point at TABLE_B). On a recompiled EXE the VWF tables move
        // and this stays 0 — we then SKIP every VWF table write / operand
        // redirect below, so we never write to a moved data address or into
        // recompiled code. (Dialog umlaut widths come from the dialog_width
        // HOOK, which relocates by signature and is unaffected.)
        int relocs = 0;
        {
            memcpy(g_vwf_b16, (const void*)VWF_TABLE_B, 0x200);
            struct { uintptr_t at; uintptr_t val; } bp[] = {
                { 0x006299E4, (uintptr_t)g_vwf_b16 + 0 },  // reader1 bearing disp
                { 0x006299EE, (uintptr_t)g_vwf_b16 + 1 },  // reader1 advance disp
                { 0x0062A62D, (uintptr_t)g_vwf_b16 + 0 },  // reader2 bearing disp
                { 0x0062A637, (uintptr_t)g_vwf_b16 + 1 },  // reader2 advance disp
            };
            for (auto& x : bp) {
                uint32_t cur = *(volatile uint32_t*)x.at;
                if (cur != (uint32_t)VWF_TABLE_B &&
                    cur != (uint32_t)VWF_TABLE_B + 1) {
                    hook_log("bof4: 16px reloc: unexpected disp 0x%08X @0x%08X "
                             "— SKIP\n", cur, (unsigned)x.at);
                    continue;
                }
                DWORD op = 0;
                if (VirtualProtect((void*)x.at, 4, PAGE_EXECUTE_READWRITE, &op)) {
                    *(volatile uint32_t*)x.at = (uint32_t)x.val;
                    DWORD t; VirtualProtect((void*)x.at, 4, op, &t);
                    FlushInstructionCache(GetCurrentProcess(), (void*)x.at, 4);
                    relocs++;
                }
            }
            hook_log("bof4: 16px font table relocated to %p (%d/4 reads "
                     "repointed off overlapping TABLE_B)\n", g_vwf_b16, relocs);
        }

        FILE* f = relocs > 0 ? fopen("vwf_config.txt", "rb") : nullptr;
        if (relocs == 0) {
            hook_log("bof4: VWF: renderer/table layout NOT the known build "
                     "(16px relocs 0/4) — SKIPPING all VWF table writes and "
                     "operand redirects to avoid touching moved data / "
                     "recompiled code. Dialog umlaut widths (dialog_width hook) "
                     "are unaffected. A recompiled EXE needs the VWF tables "
                     "re-derived.\n");
        } else if (!f) {
            hook_log("bof4: VWF: vwf_config.txt not found "
                     "(no VWF patches applied)\n");
        } else {
            DWORD old_prot = 0, old_protB = 0, old_protC = 0, old_protE = 0;
            int patched = 0;
            bool bOK = PATCH_TABLE_B &&
                (VirtualProtect(vwfB, 0x200,
                                PAGE_READWRITE, &old_protB) != 0);
            bool cOK = PATCH_TABLE_C &&
                (VirtualProtect(vwfC, 0x200,
                                PAGE_READWRITE, &old_protC) != 0);
            // TABLE_E covers 0x20..0xFF entries (2 bytes each = 448
            // bytes), plus some control-code metadata in 0x00..0x1F
            // range that we leave alone. A 0x200 region is sufficient.
            bool eOK = PATCH_TABLE_E &&
                (VirtualProtect(vwfE, 0x200,
                                PAGE_READWRITE, &old_protE) != 0);
            if (VirtualProtect(vwf, 0x200, PAGE_READWRITE, &old_prot)) {
                char line[256];
                int lineno = 0;
                bool in_16px = false;   // inside the [16px] section?
                while (fgets(line, sizeof(line), f)) {
                    lineno++;
                    // Strip leading whitespace
                    char* p = line;
                    while (*p == ' ' || *p == '\t') p++;
                    // Section header: [16px] switches subsequent lines to the
                    // 16px font's private table; any other [..] switches back.
                    if (*p == '[') {
                        in_16px = (_strnicmp(p, "[16px]", 6) == 0);
                        continue;
                    }
                    if (*p == '#' || *p == '\r' || *p == '\n' || *p == 0)
                        continue;
                    // Strip trailing comment so colons inside comments
                    // don't confuse the colon-count logic below.
                    char* hash = strchr(p, '#');
                    if (hash) *hash = 0;
                    unsigned int ch = 0;
                    int bearing = 0;
                    int adv = 0;
                    bool have_bearing = false;
                    char* eq = strchr(p, '=');
                    char* co1 = strchr(p, ':');
                    if (!eq || !co1 || co1 < eq) continue;
                    if (sscanf(p, "%x", &ch) != 1) continue;
                    char* co2 = strchr(co1 + 1, ':');
                    if (co2) {
                        if (sscanf(co1 + 1, "%d", &bearing) != 1) continue;
                        if (sscanf(co2 + 1, "%d", &adv) != 1) continue;
                        have_bearing = true;
                    } else {
                        if (sscanf(co1 + 1, "%d", &adv) != 1) continue;
                    }
                    if (ch < 0x20 || ch > 0xFF) continue;
                    int idx0 = (ch - 0x20) * 2;      // byte+0 = bearing
                    int idx1 = idx0 + 1;             // byte+1 = advance
                    if (idx1 < 0 || idx1 >= 0x200) continue;
                    // [16px] section: write ONLY the private 16px table
                    // (relocated off overlapping TABLE_B). Same layout.
                    if (in_16px) {
                        uint8_t o16 = g_vwf_b16[idx1];
                        g_vwf_b16[idx1] = (uint8_t)(adv & 0xFF);
                        if (have_bearing)
                            g_vwf_b16[idx0] = (uint8_t)(bearing & 0xFF);
                        hook_log("bof4: VWF16[ch 0x%02X] adv %d->%d\n",
                                 ch, o16, adv & 0xFF);
                        continue;
                    }
                    uint8_t old_b0 = vwf[idx0];
                    uint8_t old_b1 = vwf[idx1];
                    uint8_t new_b1 = (uint8_t)(adv & 0xFF);
                    vwf[idx1] = new_b1;
                    if (PATCH_BEARING && have_bearing) {
                        vwf[idx0] = (uint8_t)(bearing & 0xFF);
                    }
                    // Mirror to TABLE_B.
                    if (bOK) {
                        vwfB[idx1] = new_b1;
                        if (PATCH_BEARING && have_bearing) {
                            vwfB[idx0] = (uint8_t)(bearing & 0xFF);
                        }
                    }
                    // Mirror to TABLE_C (message-box subsystem).
                    if (cOK) {
                        vwfC[idx1] = new_b1;
                        if (PATCH_BEARING && have_bearing) {
                            vwfC[idx0] = (uint8_t)(bearing & 0xFF);
                        }
                    }
                    // Mirror to TABLE_E (dialog/message-box width source).
                    // This table is indexed by char value directly
                    // (NOT slot = char - 0x20), so the byte offsets are
                    // ch*2 and ch*2+1 rather than (ch-0x20)*2 +0/+1.
                    if (eOK) {
                        int e_idx0 = ch * 2;
                        int e_idx1 = e_idx0 + 1;
                        if (e_idx1 < 0x200) {
                            vwfE[e_idx1] = new_b1;
                            if (PATCH_BEARING && have_bearing) {
                                vwfE[e_idx0] = (uint8_t)(bearing & 0xFF);
                            }
                        }
                    }
                    // Mirror into g_vwf_alt (used by the alt-operand
                    // redirect, if enabled).
                    if (REDIRECT_ALT_OPERANDS) {
                        int ad0 = ch * 2;
                        int ad1 = ad0 + 1;
                        if (ad1 < (int)sizeof(g_vwf_alt)) {
                            g_vwf_alt[ad1] = new_b1;
                            if (PATCH_BEARING && have_bearing) {
                                g_vwf_alt[ad0] =
                                    (uint8_t)(bearing & 0xFF);
                            }
                        }
                    }
                    patched++;
                    bool changed = (old_b1 != new_b1) ||
                        (PATCH_BEARING && have_bearing &&
                         old_b0 != vwf[idx0]);
                    if (patched <= 5 || changed) {
                        hook_log(
                            "bof4: VWF[ch 0x%02X] b0 %d->%d  b1 %d->%d\n",
                            ch,
                            old_b0 < 128 ? old_b0 : old_b0 - 256,
                            vwf[idx0] < 128 ? vwf[idx0] : vwf[idx0] - 256,
                            old_b1 < 128 ? old_b1 : old_b1 - 256,
                            adv);
                    }
                    // Space (0x20) patching deferred — initial attempt at
                    // patching the four `add ebx, N` immediates inside
                    // the jumptable at 0x00629C54 produced layout-pass
                    // regressions (HP/MP digit overlap, "Lv. 6 9" gap).
                    // The cases 0..3 there are STYLE-keyed (selector at
                    // 0x629D00 indexed by ebp), not context-keyed, so
                    // patching them affects normal dialog measurement
                    // too. Investigate further before re-enabling. The
                    // sites are: 0x00628D63 (variant A imm8 8),
                    // 0x00629C6A/6F/74/79 (cases 0..3: 6,7,8,12),
                    // 0x008BE524 (float, currently 8.0). See log lines
                    // tagged SPACE-IMM / SPACE-FLT from the prior build.
                }
                DWORD tmp;
                VirtualProtect(vwf, 0x200, old_prot, &tmp);
                if (bOK) {
                    DWORD tmp2;
                    VirtualProtect(vwfB, 0x200, old_protB, &tmp2);
                }
                if (cOK) {
                    DWORD tmp3;
                    VirtualProtect(vwfC, 0x200, old_protC, &tmp3);
                }
                if (eOK) {
                    DWORD tmp4;
                    VirtualProtect(vwfE, 0x200, old_protE, &tmp4);
                }
                hook_log(
                    "bof4: VWF: applied %d patches from vwf_config.txt "
                    "(A=%s, B=%s, C=%s, E=%s)\n",
                    patched, "ok",
                    bOK ? "ok" : "off",
                    cOK ? "ok" : "off",
                    eOK ? "ok" : "off");
            } else {
                hook_log("bof4: VWF: VirtualProtect failed (err=%lu)\n",
                         GetLastError());
            }
            fclose(f);

            if (REDIRECT_ALT_OPERANDS) {
                // Each VWF reader instruction computes an address with
                // one of two indexing modes. We redirect its IMM32 to
                // point inside g_vwf_alt so the resulting address lands
                // on our own width byte. Layout of g_vwf_alt:
                //   g_vwf_alt[ch * 2]     = bearing
                //   g_vwf_alt[ch * 2 + 1] = advance
                //
                //   (A) reader does `[char*2 + IMM]`
                //       → IMM = &g_vwf_alt[0] for bearing, &g_vwf_alt[1]
                //         for advance.
                //   (B) reader does `[(char-0x20)*2 + IMM]` (register
                //       was decremented by 0x20 or shl'd after the sub)
                //       → IMM = &g_vwf_alt[0x40] for bearing,
                //         &g_vwf_alt[0x41] for advance.  The +0x40
                //         compensates for the missing *2*0x20.
                const uint32_t alt_b_A = (uint32_t)(uintptr_t)&g_vwf_alt[0];
                const uint32_t alt_a_A = (uint32_t)(uintptr_t)&g_vwf_alt[1];
                const uint32_t alt_b_B = (uint32_t)(uintptr_t)&g_vwf_alt[0x40];
                const uint32_t alt_a_B = (uint32_t)(uintptr_t)&g_vwf_alt[0x41];
                struct Redirect {
                    uintptr_t addr;
                    uint32_t  new_imm;
                    const char* desc;
                };
                Redirect redirects[] = {
                    // Function 0x00629E90 (mode A, char*2 indexing).
                    // Both branches read byte+1 (advance) only.
                    { 0x00629F21, alt_a_A, "629E90 BRANCH 1 advance" },
                    { 0x00629F37, alt_a_A, "629E90 BRANCH 2 advance" },
                    // Function 0x0062A040 redirects were tried but
                    // affected name-entry rendering without fixing
                    // dialog widths — disabled pending proper reverse
                    // engineering of the real dialog read site.
                };
                (void)alt_b_A; (void)alt_b_B; (void)alt_a_B;  // unused for now
                for (const auto& r : redirects) {
                    uint32_t* op = (uint32_t*)r.addr;
                    // Defense-in-depth: only rewrite if the current operand is
                    // a plausible VWF-table data address (stock reads point
                    // into 0x0094xxxx..0x0095xxxx). On a shifted/recompiled EXE
                    // these bytes are something else (e.g. code) — skip rather
                    // than corrupt. Already gated by relocs>0 above; this is a
                    // second belt.
                    uint32_t cur_op = *op;
                    if (cur_op < 0x00940000 || cur_op >= 0x00960000) {
                        hook_log("bof4: operand redirect @ 0x%08X: current "
                                 "operand 0x%08X not a table addr — SKIP\n",
                                 (unsigned)r.addr, cur_op);
                        continue;
                    }
                    DWORD op_prot = 0;
                    if (VirtualProtect(op, 4,
                                       PAGE_EXECUTE_READWRITE,
                                       &op_prot)) {
                        uint32_t old_op = *op;
                        *op = r.new_imm;
                        DWORD t;
                        VirtualProtect(op, 4, op_prot, &t);
                        FlushInstructionCache(
                            GetCurrentProcess(), op, 4);
                        hook_log(
                            "bof4: operand redirect @ 0x%08X: 0x%08X "
                            "-> 0x%08X  (%s)\n",
                            (unsigned)r.addr, old_op, r.new_imm, r.desc);
                    } else {
                        hook_log(
                            "bof4: operand redirect @ 0x%08X: "
                            "VirtualProtect failed (err=%lu)\n",
                            (unsigned)r.addr, GetLastError());
                    }
                }
            }
        }
    }

    // (removed 2026-08-03) The 0x004025A0 glyph-file loader diagnostic hook was
    // deleted along with the other font-investigation instrumentation.

    // DIAGNOSTIC: texture-upload provenance — hook LoadImage 0x4026E0 to log each
    // VRAM blit's rect + source DAT (only when tex_prov_log is set).
    tex_prov_install();

    // DIAGNOSTIC: per-glyph draw logger on 0x006780E0. While F6 is held, logs
    // each glyph's caller return-address + (x, y, char) to glyph_metric.log —
    // used to pin down which subsystem draws a label and its start-X. Default OFF.
    glyph_log_install();
    label_center_install();

    // (Previously installed TEXT_FN_0..5 entry hooks here for the
    // dialog-renderer hunt; none matched the message-box path and
    // the per-call thunks were causing a noticeable slowdown on New
    // Game, so they've been removed.)
}

// (The op-0x09 trace hooks for the parked 12-char investigation lived
// here previously. The actual fix shipped via item_name_uncap.cpp; the
// trace code was removed once the patch sites were confirmed.)

static void install_one(const char* label, uintptr_t addr,
                        void* stub, void** orig)
{
    MH_STATUS st = MH_CreateHook((void*)addr, stub, orig);
    if (st != MH_OK) {
        hook_log("bof4: MH_CreateHook(%s 0x%08X) failed: %s\n",
                 label, (unsigned)addr, mh_status(st));
        return;
    }
    st = MH_EnableHook((void*)addr);
    if (st != MH_OK) {
        hook_log("bof4: MH_EnableHook(%s) failed: %s\n",
                 label, mh_status(st));
        return;
    }
    hook_log("bof4: %s hook installed @ 0x%08X\n", label, (unsigned)addr);
}


// ── Public entry points ──────────────────────────────────────────────────
static void do_bof4_install();   // the real install sequence (below)

// Readiness probe for the deferred-install gate. Returns true once BOF4.exe's
// .text is decrypted and executable — detected by the F_A glyph-upload prologue
// (81 EC 98 00 00 00) at its stable VA. Crash-safe (RPM read): on Steam/Enigma
// before unpack, the page is encrypted/absent and this returns false.
static bool text_ready() {
    static const uint8_t FA_PROLOGUE[6] = { 0x81, 0xEC, 0x98, 0x00, 0x00, 0x00 };
    return health_probe_bytes(0x004088D0, FA_PROLOGUE, sizeof(FA_PROLOGUE));
}

// Poll until Enigma decrypts .text, then run the install. Caps at ~30 s and
// installs anyway (fail-open) so a probe that never matches can't leave the
// mod dead.
static DWORD WINAPI deferred_install_thread(LPVOID) {
    const int MAX_TRIES = 150;               // 150 * 200 ms = 30 s
    for (int i = 0; i < MAX_TRIES; ++i) {
        if (text_ready()) {
            hook_log("bof4: .text decrypted after ~%d ms — running deferred "
                     "install\n", i * 200);
            do_bof4_install();
            return 0;
        }
        Sleep(200);
    }
    hook_log("bof4: deferred-install TIMEOUT (~30 s) — readiness probe never "
             "matched; installing anyway (best effort)\n");
    do_bof4_install();
    return 0;
}

void hook_bof4_install() {
    // Parse the config file BEFORE any hook touches it (load_hook_config
    // itself is read-only w.r.t. hooks, but the log line belongs at the
    // top so users can see their settings took effect).
    load_hook_config();

    report_bof4_module();

    // Steam/Enigma viability recon: read-only compare of the decrypted runtime
    // .text against the GOG reference. Installs NOTHING and returns before
    // MinHook, so it cannot trip Enigma's anti-tamper-on-write. Enable with
    // recon_scan=1 in _d3d9_hook_config.txt (used on the Steam build only).
    if (g_cfg.recon_scan) {
        recon_scan_run();
        hook_log("bof4: recon_scan=1 — probe only, ALL hook installs skipped\n");
        return;
    }

    // Step 5 — deferred install for Steam/Enigma. Under the Enigma packer the
    // .text section is still encrypted at DLL-init and only decrypts ~3 s after
    // launch. Installing hooks against encrypted bytes would fail (or crash).
    // We gate on text_ready(): on GOG (no packer) it is true immediately and we
    // install inline; on Steam we spawn a worker that polls until .text unpacks,
    // then installs. text_ready() probes a stable prologue (F_A @0x004088D0,
    // identical VA in both known builds) with a crash-safe RPM read.
    if (text_ready()) {
        do_bof4_install();
    } else {
        hook_log("bof4: .text not yet decrypted (Steam/Enigma?) — deferring "
                 "install to a background poll thread\n");
        CreateThread(nullptr, 0, deferred_install_thread, nullptr, 0, nullptr);
    }
}

// The actual install sequence. Called inline on GOG, or from the deferred
// poll thread once Enigma has decrypted .text on Steam.
static void do_bof4_install() {
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK) {
        hook_log("bof4: MH_Initialize failed: %s\n", mh_status(st));
        return;
    }
    hook_log("bof4: MinHook initialized OK\n");

    // Fast-forward + pause/slow-mo: both ride the virtual-clock hooks below.
    // `fast_forward = false` disables the speed-up ladder; `pause_slowmo`
    // separately enables the slow-down/freeze ladder. The GetTickCount/QPC
    // hooks (and the hotkey thread) install if EITHER feature is on; the
    // wait-SKIP hooks are speed-up-only, so they stay under fast_forward.
    bool time_feat = g_cfg.fast_forward_enabled || g_cfg.pause_slowmo;
    if (time_feat) {
        if (g_cfg.fast_forward_enabled) {
            ff_build_steps();   // build the 1/2/4/8/16 ladder (capped at fast_forward_scale)
            char ladder[64] = {0};
            for (int i = 0; i < g_ff_nsteps; ++i) {
                char one[12]; snprintf(one, sizeof(one), "%s%gx",
                                       i ? "/" : "", (double)g_ff_steps[i]);
                strncat(ladder, one, sizeof(ladder) - strlen(ladder) - 1);
            }
            hook_log("bof4: fast-forward enabled — hotkey=0x%02X, cycle=%s\n",
                     g_cfg.fast_forward_vk, ladder);
        }
        if (g_cfg.pause_slowmo) {
            hook_log("bof4: pause enabled — freeze+latch vk=0x%02X (scale-0 freeze + "
                     "tuner rect latch, safe); slow-mo vk=0x%02X @ %gx\n",
                     g_cfg.pause_vk, g_cfg.pause_slow_vk, (double)g_cfg.pause_scale);
        }

        // Clock hooks — needed by BOTH features (pause forces them on).
        if (g_cfg.ff_scale_gettickcount || g_cfg.pause_slowmo) {
            install_hook("kernel32.dll", "GetTickCount",
                         (void*)hook_GetTickCount, &g_orig_GetTickCount);
            LoadLibraryA("winmm.dll");   // ensure winmm is loaded for timeGetTime
            install_hook("winmm.dll", "timeGetTime",
                         (void*)hook_timeGetTime, &g_orig_timeGetTime);
            hook_log("  clock scale: GetTickCount + timeGetTime hooked\n");
        }
        if (g_cfg.ff_scale_qpc || g_cfg.pause_slowmo) {
            install_hook("kernel32.dll", "QueryPerformanceCounter",
                         (void*)hook_QueryPerformanceCounter, &g_orig_QPC);
            hook_log("  clock scale: QueryPerformanceCounter hooked\n");
        }
        // Wait-SKIP hooks — speed-up only (fast-forward). Never installed for
        // pause alone: skipping waits would fight the slow-down.
        if (g_cfg.fast_forward_enabled) {
            if (g_cfg.ff_skip_sleep) {
                install_hook("kernel32.dll", "Sleep",
                             (void*)hook_Sleep, &g_orig_Sleep);
                hook_log("  ff_skip_sleep: Sleep hooked\n");
            }
            if (g_cfg.ff_skip_wfso) {
                install_hook("kernel32.dll", "WaitForSingleObject",
                             (void*)hook_WaitForSingleObject, &g_orig_WFSO);
                hook_log("  ff_skip_wfso: WaitForSingleObject hooked\n");
            }
            if (g_cfg.ff_skip_wfmo) {
                install_hook("kernel32.dll", "WaitForMultipleObjects",
                             (void*)hook_WaitForMultipleObjects, &g_orig_WFMO);
                hook_log("  ff_skip_wfmo: WaitForMultipleObjects hooked\n");
            }
            if (g_cfg.ff_skip_msgwait) {
                install_hook("user32.dll", "MsgWaitForMultipleObjects",
                             (void*)hook_MsgWaitForMultipleObjects, &g_orig_MsgWait);
                hook_log("  ff_skip_msgwait: MsgWaitForMultipleObjects hooked\n");
            }
        }

        // Spawn the independent hotkey polling thread. This is NOT
        // tied to the render loop, so it fires reliably regardless of
        // whether Present is hooked or whether the ddraw wrapper even
        // routes through D3D9. It polls BOTH the FF and pause hotkeys.
        g_ff_thread_quit = false;
        g_ff_thread = CreateThread(nullptr, 0, ff_poll_thread,
                                   nullptr, 0, nullptr);
        if (g_ff_thread) {
            hook_log("bof4: hotkey thread started (FF vk=0x%02X, pause vk=0x%02X, "
                     "poll 30ms)\n", g_cfg.fast_forward_vk, g_cfg.pause_vk);
        } else {
            hook_log("bof4: WARNING — failed to start hotkey thread "
                     "(GetLastError=%lu)\n", GetLastError());
        }
    }

    install_hook("kernel32.dll", "CreateFileA",
                 (void*)hook_CreateFileA, &g_orig_CreateFileA);
    install_hook("kernel32.dll", "CreateFileW",
                 (void*)hook_CreateFileW, &g_orig_CreateFileW);
    install_hook("kernel32.dll", "ReadFile",
                 (void*)hook_ReadFile, &g_orig_ReadFile);
    install_hook("kernel32.dll", "SetFilePointer",
                 (void*)hook_SetFilePointer, &g_orig_SetFilePointer);
    install_hook("kernel32.dll", "CloseHandle",
                 (void*)hook_CloseHandle, &g_orig_CloseHandle);

    hook_log("bof4: file I/O hooks installed (log cap = %d lines)\n",
             FILE_LOG_MAX);

    install_hook("gdi32.dll", "CreateFontA",
                 (void*)hook_CreateFontA, &g_orig_CreateFontA);
    install_hook("gdi32.dll", "CreateFontIndirectA",
                 (void*)hook_CreateFontIndirectA, &g_orig_CreateFontIndirectA);
    install_hook("gdi32.dll", "AddFontResourceA",
                 (void*)hook_AddFontResourceA, &g_orig_AddFontResourceA);
    install_hook("gdi32.dll", "AddFontResourceExA",
                 (void*)hook_AddFontResourceExA, &g_orig_AddFontResourceExA);

    install_vwf_config();


    // Universal glyph-metric lookup at 0x006780E0 — re-enabled.
    // Previously removed on the assumption TABLE_E (0x0094CD3C) covered
    // every char, but post JP-atlas swap the umlauts now sit at 0x90-0x96
    // and the engine's compound 5-table width algorithm has no entries
    // for those codes — every umlaut renders at the 24-px cell fallback.
    // The twin-substitute thunk below maps 0x90-0x96 to ASCII analogues
    // before the original function runs, so the engine returns sensible
    // widths and the painted umlaut glyph still flows through unchanged.
    install_one("dialog_width",
                sig_addr("dialog_width", SIG_DIALOG_WIDTH, "xxxxxxxxxxxxxxxxxxx",
                         sizeof(SIG_DIALOG_WIDTH), 0x006780E0),
                (void*)thunk_width_fn, &g_orig_width_fn);

    // F3-armed diagnostic: identifies which width source a menu reads for a
    // SPACE (TABLE_A/B/C/F via HW breakpoints, or 0x006780E0 via the width-fn
    // hook above). Writes to space_trace.log. See project_menu_space_hunt.

    // ── BGM-loop diagnostics (Phase 1) ────────────────────────────────────
    // Install hooks inside dshow.dll to trace the BGM loop seam. Output
    // goes to d3d9_hook.log alongside the rest. This adds three sites:
    // PLAY_OPEN (0x100C4110), REPLAY_VT (0x100C1AF0), and EOF_SIGNAL
    // (PostMessageA filtered to dshow.dll callers). See bgm_loop.cpp.
    // Install if the fix is on OR logging is requested (logging can run with
    // the fix off for an A/B comparison). The seek-override itself is gated on
    // bgm_loop_enabled inside the hook.
    if (g_cfg.bgm_loop_enabled || g_cfg.bgm_loop_log) {
        bgm_loop_install();
    } else {
        hook_log("bgm: bgm_loop=false (and bgm_loop_log=false) — hooks NOT "
                 "installed (stock BGM)\n");
    }

    // Live text-substitution / trace hook on 0x00629470. Skips install
    // entirely if substitute/log/render_trace flags are all false.
    text_subst_install();
    // Diagnostic per-character renderer trace — taps into text_subst's
    // existing hook + the F_B LoadGlyph hook. No-op when off.
    // Page-driver visible-char count fixup (German [04:NN] expansion).
    // Standalone MinHook on 0x00527C30; skips install when text_resolver
    // already owns the hook.

    // Soft-subtitle overlay for the intro FMV. Loads the SRT and mirrors
    // the subtitle_* config into the overlay module. The actual draw hooks
    // (Device/SwapChain Present + Reset) are installed lazily in hooks.cpp
    // when the game creates its D3D9 device. No-op when subtitle=false.
    subtitles_init();

    // Phase 3 resolver hook on 0x00527970 — universal "begin render text"
    // entrypoint. Swaps the buffer-pointer arg to our heap buffer when a
    // translation matches. Uses the same live table loaded by text_subst.
    text_resolver_install();

    // Two-byte patch lifting the 12-char limit on [09:XX:YY] item-name
    // substitutions in dialog. Mirrors PSX SLUS_01324.ASM.
    item_name_uncap_install();

    // TEMP DIAGNOSTIC: hardware write-watch on the [07:00] item-name scratch
    // slot (0x00B56EBF) to locate the 14-char-cap writer. No-op unless
    // name_slot_bp=true. Delete once the writer is found + patched.
    name_slot_bp_install();

    // Lift the 14-char cap on item names inserted into message text via [07:00]
    // (e.g. "Kaputtes Schwert kann nicht gehandelt werden!"). Patches the eight
    // cmp ecx,0x0D clamp loops that feed the [07] slot copier 0x629D50.
    item_name_msg_uncap_install();

    // Shift the equipment-skill "Menü" box + labels + title + divider + cursor
    // left (German widened box needs room). Width via box_autofit; X via this
    // 7-byte patch. No-op unless equip_menu_shift != 0.
    equip_menu_shift_install();

    // One-byte patch unlocking the pause-menu Save item outside the
    // World Map. See save_anywhere.h for the gate's full disasm.
    save_anywhere_install();

    // Auto-grow menu boxes to fit their (translated) text width. Opt-in;
    // installs NO hooks unless box_autofit=true, so default = unchanged behavior.
    box_autofit_install(g_cfg.box_autofit, g_cfg.box_autofit_pad);

    // One-byte patch widening the field command menu box (0x0067D03B). Opt-in;
    // no-op unless cmdbox_width is set to a valid grid value. (Old RE-based
    // approach — kept but scrapped in favour of the German-ASM path below.)
    cmdbox_width_install();

    // Faithful German SGAMEN .ASM applied to PSX RAM (execution test). No-op
    // unless cmdbox_asm=true.
    cmdbox_asm_install();

    // Shift the field command-menu label text + "Command" header graphic left to
    // follow the widened box. Data patch; no-op unless cmdbox_label_x/cmdgfx_x set.
    cmdbox_labels_install();

    // EXPERIMENTAL: aggressive multi-site space patch (Round 2).
    //   Round 1 (jumptable cases 0/1/2/3 = 6/7/8/12 all -> 8) broke HP/MP
    //   layout because cases 0/1 are style-keyed and used by the layout
    //   pass for non-space contexts. Round 2 only touches:
    //     - variant A imm8 (0x00628D63): smallfont's hardcoded `add ebx, 8`
    //     - case 3 imm8     (0x00629C79): jumptable wide case (was 0xC = 12)
    //   These two only fire on al==0x20, so digits/letters won't be touched.
    //   Both default to 3 (down from 8/12) — distinctive enough to see
    //   visually whether they affect item-menu space. User can iterate
    //   by editing the values in source + rebuild.
    {
        struct Patch { uintptr_t va; uint8_t expect; uint8_t value; const char* desc; };
        // All space-hunting patches PARKED. None worked to narrow
        // item-menu spaces from 17 px. See memory project_menu_space_hunt
        // for full investigation log. Next session: proper RE of renderer
        // 0x00629470 in Ghidra/IDA to find the actual `cursor +=` site
        // executed for space chars. Code below preserved for context.
    }

    if (g_cfg.space_fallthrough) {
        // Located by signature so it relocates across EXE builds. The gate
        // is  mov al,[edi] / cmp al,0x20 / je rel32 / test al,al  — we NOP
        // the 6-byte je. Verified unique on both builds (the sig lands on
        // 0x006299ED on the 2026-05 GOG recompile, matching the address
        // documented in project_gog_2026_05_update.md).
        static const uint8_t SPACE_JE_SIG[] = {
            0x8A, 0x07, 0x3C, 0x20, 0x0F, 0x84, 0, 0, 0, 0, 0x84, 0xC0,
        };
        static const char SPACE_JE_MASK[] = "xxxxxx????xx";
        uintptr_t SPACE_JE_VA = resolve_sig("space_fallthrough", SPACE_JE_SIG,
                                            SPACE_JE_MASK, sizeof(SPACE_JE_SIG), 4);
        if (!SPACE_JE_VA) SPACE_JE_VA = 0x00629ACD;   // documented fallback
        constexpr uint8_t EXPECTED[6] = { 0x0F, 0x84, 0x81, 0x01, 0x00, 0x00 };
        bool ok = false;
        if (health_check_bytes("space_fallthrough", SPACE_JE_VA,
                               EXPECTED, sizeof(EXPECTED))) {
            void* p = (void*)SPACE_JE_VA;
            DWORD op_prot = 0;
            if (!VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &op_prot)) {
                hook_log(
                    "bof4: space_fallthrough @ 0x%08X: VirtualProtect "
                    "failed err=%lu — patch SKIPPED\n",
                    (unsigned)SPACE_JE_VA, GetLastError());
            } else {
                memset(p, 0x90, 6);   // six NOPs
                DWORD t;
                VirtualProtect(p, 6, op_prot, &t);
                FlushInstructionCache(GetCurrentProcess(), p, 6);
                hook_log(
                    "bof4: space_fallthrough @ 0x%08X: je 0x629C54 -> "
                    "6x NOP. Space chars now read width from TABLE_A.\n",
                    (unsigned)SPACE_JE_VA);
                ok = true;
            }
        }
        char note[64];
        snprintf(note, sizeof(note), "VA 0x%08X (je 6B -> NOP)",
                 (unsigned)SPACE_JE_VA);
        health_record("space_fallthrough", ok, note);
    } else {
        health_record_disabled("space_fallthrough", "config off");
    }

    // ── menu_space_vwf ────────────────────────────────────────────────────
    // The item-menu renderer (BOF4!0x629xxx) dispatches char 0x20 at 0x629532
    // as `cmp eax,0x20 / ja 0x6296C4`: chars > 0x20 take the normal glyph path
    // (read TABLE_A, ×0.5 scale), but space (0x20, not ABOVE 0x20) falls into a
    // jump table whose handler adds a HARDCODED float (0x8BE524) straight to the
    // pen — a different unit than the glyph widths, so it never matches.
    // Flipping `ja` (0F 87) -> `jae` (0F 83) makes 0x20 ALSO take the glyph
    // path, so the space width comes from TABLE_A[0] (vwf_config `0x20 = space`)
    // scaled exactly like every other glyph. 1-byte patch, only char 0x20
    // affected (0x1F and below still hit the jump table). Requires the space
    // atlas slot (index 0) to be transparent — it is in the stock font.
    // Found via the F3 space-trace. See project_menu_space_hunt.
    {
        // Located by signature: cmp eax,0x20 / ja rel32 / xor edx,edx /
        // mov dl,[eax+tbl]. Wildcard the rel32 and the table disp so it
        // relocates across builds; unique on both live + 2026-05 recompile.
        static const uint8_t JAE_SIG[] = {
            0x83, 0xF8, 0x20, 0x0F, 0x87, 0, 0, 0, 0,
            0x33, 0xD2, 0x8A, 0x90, 0, 0, 0, 0,
        };
        static const char JAE_MASK[] = "xxxxx????xxxx????";
        uintptr_t SPACE_JAE_VA = resolve_sig("menu_space_vwf", JAE_SIG, JAE_MASK,
                                             sizeof(JAE_SIG), 3);   // the 0F 87
        if (!SPACE_JAE_VA) SPACE_JAE_VA = 0x00629535;   // documented fallback
        constexpr uint8_t EXPECT[2] = { 0x0F, 0x87 };
        bool ok2 = false;
        if (health_check_bytes("menu_space_vwf", SPACE_JAE_VA,
                               EXPECT, sizeof(EXPECT))) {
            void* p = (void*)(SPACE_JAE_VA + 1);   // the 0x87 byte
            DWORD op_prot = 0;
            if (VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &op_prot)) {
                *(volatile uint8_t*)p = 0x83;      // ja -> jae
                DWORD t; VirtualProtect(p, 1, op_prot, &t);
                FlushInstructionCache(GetCurrentProcess(), p, 1);
                hook_log("bof4: menu_space_vwf @ 0x%08X: ja -> jae. Space (0x20) "
                         "now reads TABLE_A[0] like a glyph (vwf_config drives "
                         "it, glyph-scaled).\n", (unsigned)SPACE_JAE_VA);
                ok2 = true;
            }
        }
        char nspc[48];
        snprintf(nspc, sizeof(nspc), "VA 0x%08X (ja->jae)", (unsigned)SPACE_JAE_VA);
        health_record("menu_space_vwf", ok2, nspc);
    }

    // ── smallfont_space_vwf ──────────────────────────────────────────────────
    // 16px-font twin of menu_space_vwf. The 16px renderer (BOF4!0x629xxx, the one
    // whose glyph path reads the relocated TABLE_B / g_vwf_b16) dispatches space
    // like this:  sub edx,0x1f / je 0x629a20 — char 0x20 is short-circuited to a
    // HARDCODED `add edi, 8` (0x629a20) and NEVER reaches the glyph/table path, so
    // `[16px] 0x20 = space` in vwf_config had no effect (that's the bug the user
    // hit: cranking 0x20 to 20/120 did nothing). Only 0x20 sets ZF at that je
    // (0x00/0x01 branch earlier), so NOP-ing the 6-byte `je` makes space fall
    // through to the normal glyph path -> index 0 -> g_vwf_b16[0] bearing +
    // g_vwf_b16[1] advance, ×0.5 like every other glyph. `add edi,8` @0x629a20
    // becomes dead (space-only target). Requires the 16px atlas slot 0 to be
    // transparent (stock font: it is). Reversible via `smallfont_space_vwf=false`.
    if (g_cfg.smallfont_space_vwf_enabled) {
        // Locate the `je 0x629a20`: sub edx,0x1f / je rel32 / cmp cl,0x7e / jb .
        // Wildcard only the je rel32 so it relocates across builds; the cmp/jb
        // tail keeps it unique.
        static const uint8_t SP16_SIG[] = {
            0x83, 0xEA, 0x1F, 0x0F, 0x84, 0, 0, 0, 0,
            0x80, 0xF9, 0x7E, 0x72,
        };
        static const char SP16_MASK[] = "xxxxx????xxxx";
        uintptr_t JE_VA = resolve_sig("smallfont_space_vwf", SP16_SIG, SP16_MASK,
                                      sizeof(SP16_SIG), 3);   // the 0F 84
        if (!JE_VA) JE_VA = 0x00629915;   // documented fallback
        constexpr uint8_t EXPECT[2] = { 0x0F, 0x84 };
        bool ok3 = false;
        if (health_check_bytes("smallfont_space_vwf", JE_VA, EXPECT, sizeof(EXPECT))) {
            void* p = (void*)JE_VA;                 // the 6-byte je 0F 84 xx xx xx xx
            DWORD op_prot = 0;
            if (VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &op_prot)) {
                memset(p, 0x90, 6);                 // NOP the je -> space falls through
                DWORD t; VirtualProtect(p, 6, op_prot, &t);
                FlushInstructionCache(GetCurrentProcess(), p, 6);
                hook_log("bof4: smallfont_space_vwf @ 0x%08X: NOP je. 16px space "
                         "(0x20) now reads g_vwf_b16[0] ([16px] 0x20 drives it, "
                         "glyph-scaled).\n", (unsigned)JE_VA);
                ok3 = true;
            }
        }
        char n16[48];
        snprintf(n16, sizeof(n16), "VA 0x%08X (NOP je)", (unsigned)JE_VA);
        health_record("smallfont_space_vwf", ok3, n16);
    } else {
        health_record_disabled("smallfont_space_vwf", "config off");
    }

    // Diagnostic page-guard on all known VWF tables (TABLE_A..E +
    // layout). Opt-in via vwf_guard_enable.txt next to BOF4.exe.
    bof4_install_vwf_guard();
    // Un-clip / reposition the German title-menu labels by patching the
    // hardcoded rect immediates in BOF4.exe .text (exe on disk untouched).
    menu_rect_fix_apply();

    // Japanese title card: replace the two title draw routines with the JP
    // build's own primitive list. Byte-guarded; no-ops on any other build.
    title_jp_install();

    // Universal textured-rect tools: one hook on BOF4!0x502070 (the submit used
    // by every UI sprite) inventories and/or un-clips ANY label on ANY screen.
    // Installs only when rect_trace, rect_fix or rect_tuner is enabled. See
    // rect_hook.cpp.
    rect_hook_install();

    // In-game visual rect editor (F10). Starts its own input thread only when
    // rect_tuner is enabled; the 0x502070 hook it rides on is installed by
    // rect_hook_install above (which now also fires for rect_tuner).
    rect_tuner_install();
    // Last: a passive stall detector. Costs one sleeping thread and does nothing
    // until Present has actually stopped for 12 s — see hang_watchdog.h.
    hang_watchdog_install();

    // EXPERIMENTAL free field-camera rotation (GOG only). Phase 1 = diagnostic
    // logger; no-op unless free_camera=true.
    free_camera_install();

    // Restore the NA-cut AREAM031 violence beat (B1' PC-remap + data codecave).
    // GOG-only, flag-gated (default OFF), exe untouched, verifies all sites.
    censor_fix_aream031_install();

    // READ-ONLY scene discovery (locate a NEW censored scene's script record +
    // branch point, e.g. AREAM027). No-op unless censor_scene_trace=true; skipped
    // if the AREAM031 fix is on (shares the 3 fetch sites). Writes nothing.
    censor_scene_trace_install();

    // AREAD145 "bath scene" diagnostic probe (phase/selector logger + optional
    // US force-phase=7). No-op unless censor_probe_aread145[_force]=true.
    censor_probe_aread145_install();

    // Lift the hardcoded typewriter char-caps on the AREAD152 ending narration
    // ("…that it was still <hero>." etc.) — the caps were tuned to the English
    // line lengths and truncate longer translations mid-word. Detours the four
    // narration opcode handlers to advance-on-complete (dynamic target from the
    // last line's real visible length). Signature-verified; safe no-op on a
    // non-matching EXE. Always on (no config key).
    narration_uncap_install();

    // Restore the Japan-only Dengeki Store bonus area (shop + lottery) by
    // holding its two persistent event flags (0x8D+0xA5 @0xB5672C) set — the
    // same unlock the PSX memcard patch performs. Signature-verified; no-op on
    // a non-matching EXE. Store text is JAPANESE until localized.
    dengeki_unlock_install();

    // End-of-init summary: lists every byte-pattern patch as
    // OK/FAIL/DISABLED so failures don't get lost in the ~350-line init
    // stream. When the EXE shifts (e.g. a GOG update), each failure
    // line already includes a hexdump + .text scan suggesting the new
    // address — see hook_health.cpp.
    health_report();
}

static void bof4_uninstall_init_dat_guard();  // defined above

void hook_bof4_shutdown() {
    // Stop the FF polling thread before tearing down MinHook so it can't
    // race us by calling into a disabled GetTickCount hook.
    if (g_ff_thread) {
        g_ff_thread_quit = true;
        WaitForSingleObject(g_ff_thread, 200);
        CloseHandle(g_ff_thread);
        g_ff_thread = nullptr;
    }
    bof4_uninstall_init_dat_guard();
    bof4_uninstall_vwf_guard();
    pause_freeze_shutdown();   // free the 2 MB save-state buffer
    censor_fix_aream031_shutdown();  // restore uncensor hooks + free codecave
    censor_probe_aread145_shutdown(); // restore AREAD145 gate jumps + free cave
    dengeki_unlock_shutdown();        // stop the Dengeki flag-hold thread
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}
