// ── In-game ImGui settings overlay (config_overlay.cpp) ────────────────────
//
// Press the toggle key (default: the physical top-left key — ^ on QWERTZ, ` / ~
// on QWERTY, matched by SCAN CODE 0x29 so it is layout-proof) to open a panel
// that live-toggles the hook's feature flags, remaps the Turbo-cycle key,
// rebinds the Rect-Tuner keys, and live-edits VWF glyph spacing. "Save" writes
// the changes back to _d3d9_hook_config.txt / rect_tuner_keys.txt, preserving
// every comment (only each key's value is spliced in place).
//
// Rendering rides the existing device/​swap-chain Present hooks (hooks.cpp), the
// same spot ff_overlay / subtitles / rect_tuner already draw. Input uses a
// WndProc subclass on g_dev_window (nothing else in the hook subclasses the
// window). While the panel is open, g_config_overlay_active suspends the
// GetAsyncKeyState poll threads (rect tuner + fast-forward) so keys typed into
// the panel don't also drive the game.
//
// Uses Dear ImGui v1.92.9 (MIT) vendored under src/imgui/.

#include "hooks.h"
#include "rect_tuner.h"

#include <windows.h>
#include <d3d9.h>
#include <atomic>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_win32.h"
#include "imgui/backends/imgui_impl_dx9.h"

// imgui_impl_win32.h keeps this forward declaration behind '#if 0' (to avoid
// pulling <windows.h> into the header); the docs say to copy it into the .cpp.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Device window captured at CreateDevice (hooks.cpp). Used as the ImGui HWND.
extern HWND g_dev_window;

// True while the panel is open — read by the poll threads (see hooks.h).
std::atomic<bool> g_config_overlay_active{ false };

// ───────────────────────────── module state ───────────────────────────────
static bool                s_inited       = false;
static bool                s_show         = false;
static bool                s_firstrun_checked = false;   // ran the first-launch check yet?
static bool                s_welcome          = false;   // show the first-launch welcome banner

// Absolute path of a sidecar file next to BOF4.exe (CWD isn't reliable at runtime).
static void overlay_sidecar(char* out, size_t n, const char* leaf) {
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char* s = strrchr(exe, '\\'); if (s) *(s + 1) = 0; else exe[0] = 0;
    snprintf(out, n, "%s%s", exe, leaf);
}
// First launch = the "seen" sentinel is absent. Auto-open the panel once and
// create the sentinel so it never auto-opens again.
static void first_run_check() {
    if (s_firstrun_checked) return;
    s_firstrun_checked = true;
    char path[MAX_PATH]; overlay_sidecar(path, sizeof(path), "_d3d9_hook_seen.txt");
    FILE* f = fopen(path, "rb");
    if (f) { fclose(f); return; }                 // already seen -> nothing to do
    s_show = true; s_welcome = true;              // first launch: greet the player
    g_config_overlay_active.store(true, std::memory_order_relaxed);
    FILE* w = fopen(path, "wb");
    if (w) { fputs("1\n", w); fclose(w); }
}
static HWND                s_hwnd         = nullptr;
static WNDPROC             s_orig_wndproc = nullptr;
static IDirect3DDevice9*   s_device       = nullptr;
static char                s_status[192]  = "";

// Key-capture handshake (WndProc thread <-> render thread).
static std::atomic<bool>   s_cap_active{ false };
static std::atomic<int>    s_cap_vk{ -1 };     // -1 none, -2 cancelled, >0 captured
static int                 s_cap_owner  = 0;   // render-thread only

// ── Low-level mouse hook ───────────────────────────────────────────────────
// The game reads the mouse by POLLING (GetAsyncKeyState / DirectInput), not via
// WndProc, so swallowing window messages can't hide a click from it. A WH_MOUSE_LL
// hook eats the button/wheel events at the source (before the game or async key
// state can see them) while the panel is open, and we feed those events straight
// into ImGui instead. Mouse MOVE is passed through so the OS cursor keeps moving
// (ImGui reads position via GetCursorPos). Runs on its own thread with a message
// pump (LL hooks require one) — same pattern as rect_tuner.cpp's wheel hook.
static HHOOK               s_mouse_hook   = nullptr;
static HANDLE              s_mouse_thread = nullptr;
static std::atomic<bool>   s_mb_state[5];      // L,R,M,X1,X2 physical down-state
static bool                s_mb_applied[5] = {};
static std::atomic<int>    s_wheel_ticks{ 0 };

static bool cfg_is_foreground() {
    DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid != 0 && pid == GetCurrentProcessId();
}

static LRESULT CALLBACK low_level_mouse(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && s_show && cfg_is_foreground()) {
        MSLLHOOKSTRUCT* m = (MSLLHOOKSTRUCT*)lParam;
        bool swallow = true;
        switch (wParam) {
            case WM_LBUTTONDOWN: s_mb_state[0].store(true);  break;
            case WM_LBUTTONUP:   s_mb_state[0].store(false); break;
            case WM_RBUTTONDOWN: s_mb_state[1].store(true);  break;
            case WM_RBUTTONUP:   s_mb_state[1].store(false); break;
            case WM_MBUTTONDOWN: s_mb_state[2].store(true);  break;
            case WM_MBUTTONUP:   s_mb_state[2].store(false); break;
            case WM_XBUTTONDOWN: { int b = (HIWORD(m->mouseData) == XBUTTON1) ? 3 : 4; s_mb_state[b].store(true);  break; }
            case WM_XBUTTONUP:   { int b = (HIWORD(m->mouseData) == XBUTTON1) ? 3 : 4; s_mb_state[b].store(false); break; }
            case WM_MOUSEWHEEL:  s_wheel_ticks.fetch_add((short)HIWORD(m->mouseData)); break;
            default:             swallow = false; break;   // moves etc. pass through
        }
        if (swallow) return 1;   // eaten: game + polling/DirectInput won't see it
    }
    return CallNextHookEx(s_mouse_hook, nCode, wParam, lParam);
}

static DWORD WINAPI mouse_thread_proc(LPVOID) {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&low_level_mouse, &self);
    s_mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, low_level_mouse, self, 0);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessage(&msg); }
    return 0;
}

// Push the LL-hook mouse button/wheel state into ImGui (render thread, before NewFrame).
static void feed_mouse_events() {
    ImGuiIO& io = ImGui::GetIO();
    for (int i = 0; i < 5; ++i) {
        bool s = s_mb_state[i].load(std::memory_order_relaxed);
        if (s != s_mb_applied[i]) { io.AddMouseButtonEvent(i, s); s_mb_applied[i] = s; }
    }
    int wt = s_wheel_ticks.exchange(0, std::memory_order_relaxed);
    if (wt) io.AddMouseWheelEvent(0.0f, (float)wt / (float)WHEEL_DELTA);
}

// ─────────────────────── _d3d9_hook_config.txt model ──────────────────────
// One entry per file line. For `key = value  # comment` lines we remember the
// byte span of the VALUE inside `raw`, so a save splices a new value in place
// and keeps the comment + spacing exactly. `buf` backs the Advanced-tab editor.
struct CfgLine {
    std::string raw;
    bool        kv    = false;
    std::string key;
    std::string val;
    size_t      vstart = 0, vend = 0;
    char        buf[192] = {};
};
static std::vector<CfgLine> s_model;
static bool                 s_model_loaded = false;

static void cfg_path(char* out, size_t n) {
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char* s = strrchr(exe, '\\'); if (s) *(s + 1) = 0; else exe[0] = 0;
    snprintf(out, n, "%s_d3d9_hook_config.txt", exe);
}

static void cfg_model_load() {
    s_model.clear();
    char path[MAX_PATH]; cfg_path(path, sizeof(path));
    FILE* f = fopen(path, "rb");
    if (!f) { s_model_loaded = true; return; }
    char buf[2048];
    while (fgets(buf, sizeof(buf), f)) {
        CfgLine L; L.raw = buf;
        const std::string& r = L.raw;
        size_t i = 0; while (i < r.size() && (r[i] == ' ' || r[i] == '\t')) ++i;
        size_t eq = r.find('=');
        if (i < r.size() && r[i] != '#' && r[i] != '\r' && r[i] != '\n' && eq != std::string::npos) {
            L.kv = true;
            // key = trimmed text before '='
            size_t ke = eq; while (ke > i && (r[ke-1] == ' ' || r[ke-1] == '\t')) --ke;
            L.key = r.substr(i, ke - i);
            // value span: after '=' skip spaces, stop at '#' or EOL (trim trailing ws)
            size_t p = eq + 1; while (p < r.size() && (r[p] == ' ' || r[p] == '\t')) ++p;
            size_t hash = r.find('#', p);
            size_t e = (hash == std::string::npos) ? r.size() : hash;
            while (e > p && (r[e-1] == ' ' || r[e-1] == '\t' || r[e-1] == '\r' || r[e-1] == '\n')) --e;
            L.vstart = p; L.vend = e;
            L.val = r.substr(p, e - p);
            strncpy_s(L.buf, sizeof(L.buf), L.val.c_str(), _TRUNCATE);
        }
        s_model.push_back(std::move(L));
    }
    fclose(f);
    s_model_loaded = true;
}

// ─────────────── curated keys: which get serialized from g_cfg ─────────────
// Curated = handled by a dedicated tab (written from g_cfg on Save) OR deliberately
// hidden from the raw Advanced list. Everything else in the file shows in Advanced.
// (Always-on display fixes are internal defaults now and not listed here, so Save
//  never re-adds them to the file.)
static const char* const CURATED[] = {
    "config_gui","config_gui_hotkey",
    "console","unlock_vsync",
    "fast_forward","fast_forward_scale","fast_forward_hotkey","fast_forward_pad",
    "fast_forward_pad_button",
    "subtitle","subtitle_font_scale","subtitle_time_offset_ms",
    "censor_fix_aream031","censor_fix_aream027","censor_fix_aread157",
    "jp_title","dengeki_store_unlock",
};
static bool is_curated(const char* key) {
    for (const char* k : CURATED) if (_stricmp(k, key) == 0) return true;
    return false;
}

static std::string B(bool v)  { return v ? "true" : "false"; }
static std::string I(int v)   { char b[32]; snprintf(b, sizeof(b), "%d", v); return b; }
static std::string Ff(float v){ char b[32]; snprintf(b, sizeof(b), "%g", v); return b; }

// VK -> a token parse_vk() (bof4_hooks.cpp) round-trips. Falls back to "0xNN"
// (parse_vk now accepts a 0xNN raw code), so any remapped Turbo key persists.
static std::string vk_ff_token(int vk) {
    char b[16];
    if (vk >= 'A' && vk <= 'Z') { snprintf(b, sizeof(b), "%c", vk); return b; }
    if (vk >= '0' && vk <= '9') { snprintf(b, sizeof(b), "%c", vk); return b; }
    if (vk >= VK_F1 && vk <= VK_F12) { snprintf(b, sizeof(b), "F%d", vk - VK_F1 + 1); return b; }
    switch (vk) {
        case VK_INSERT: return "INSERT"; case VK_DELETE: return "DELETE";
        case VK_HOME:   return "HOME";   case VK_END:    return "END";
        case VK_TAB:    return "TAB";    case VK_PAUSE:  return "PAUSE";
        case VK_SCROLL: return "SCROLL";
    }
    snprintf(b, sizeof(b), "0x%02X", vk & 0xFF); return b;
}

// Current value of a curated key, formatted as it should appear in the file.
static std::string cfg_value_str(const char* k) {
    #define KV(name, expr) if (_stricmp(k, name) == 0) return (expr);
    KV("config_gui",            B(g_cfg.config_gui))
    KV("config_gui_hotkey",     (g_cfg.config_gui_vk == 0 ? std::string("default")
                                                          : vk_ff_token(g_cfg.config_gui_vk)))
    KV("jp_title",              B(g_cfg.jp_title))
    KV("dengeki_store_unlock",  B(g_cfg.dengeki_store_unlock))
    KV("censor_fix_aread157",   B(g_cfg.censor_fix_aread157))
    KV("console",               B(g_cfg.console_enabled))
    KV("dat_log",               B(g_cfg.dat_log))
    KV("tex_dump",              B(g_cfg.tex_dump_enabled))
    KV("tex_prov_log",          B(g_cfg.tex_prov_log))
    KV("rect_trace",            B(g_cfg.rect_trace))
    KV("box_autofit_log_all",   B(g_cfg.box_autofit_log_all))
    KV("text_render_log",       B(g_cfg.text_render_log))
    KV("censor_fix_trace",      B(g_cfg.censor_fix_trace))
    KV("unlock_vsync",          B(g_cfg.unlock_vsync))
    KV("fast_forward",          B(g_cfg.fast_forward_enabled))
    KV("fast_forward_scale",    Ff(g_cfg.fast_forward_scale))
    KV("fast_forward_hotkey",   vk_ff_token(g_cfg.fast_forward_vk))
    KV("fast_forward_pad",      B(g_cfg.fast_forward_pad))
    KV("fast_forward_pad_button", std::string(g_cfg.fast_forward_pad_button))
    KV("pause_slowmo",          B(g_cfg.pause_slowmo))
    KV("pause_scale",           Ff(g_cfg.pause_scale))
    KV("rect_tuner",            B(g_cfg.rect_tuner))
    KV("rect_fix",              B(g_cfg.rect_fix))
    KV("subtitle",              B(g_cfg.subtitle_enabled))
    KV("subtitle_font_scale",   Ff(g_cfg.subtitle_font_scale))
    KV("subtitle_time_offset_ms", I(g_cfg.subtitle_time_offset_ms))
    KV("space_fallthrough",     B(g_cfg.space_fallthrough))
    KV("smallfont_space_vwf",   B(g_cfg.smallfont_space_vwf_enabled))
    KV("menu_rect_fix",         B(g_cfg.menu_rect_fix))
    KV("box_autofit",           B(g_cfg.box_autofit))
    KV("box_autofit_pad",       I(g_cfg.box_autofit_pad))
    KV("save_anywhere",         B(g_cfg.save_anywhere_enabled))
    KV("item_name_uncap",       B(g_cfg.item_name_uncap_enabled))
    KV("item_name_uncap_value", I(g_cfg.item_name_uncap_value))
    KV("item_name_msg_uncap",   B(g_cfg.item_name_msg_uncap_enabled))
    KV("bgm_loop",              B(g_cfg.bgm_loop_enabled))
    KV("free_camera",           B(g_cfg.free_camera))
    KV("censor_fix_aream031",   B(g_cfg.censor_fix_aream031))
    KV("censor_fix_aream027",   B(g_cfg.censor_fix_aream027))
    KV("equip_menu_shift",      I(g_cfg.equip_menu_shift_px))
    KV("cmdbox_cursor_w",       I(g_cfg.cmdbox_cursor_w))
    KV("cmdbox_label_x",        I(g_cfg.cmdbox_label_x))
    KV("cmdbox_cmdgfx_x",       I(g_cfg.cmdbox_cmdgfx_x))
    #undef KV
    return "";
}

static bool cfg_model_save() {
    if (!s_model_loaded) cfg_model_load();
    // Splice each kv line's value: curated -> from g_cfg; other -> Advanced buf.
    std::vector<bool> present(sizeof(CURATED)/sizeof(CURATED[0]), false);
    for (auto& L : s_model) {
        if (!L.kv) continue;
        std::string nv;
        if (is_curated(L.key.c_str())) {
            nv = cfg_value_str(L.key.c_str());
            for (size_t c = 0; c < sizeof(CURATED)/sizeof(CURATED[0]); ++c)
                if (_stricmp(CURATED[c], L.key.c_str()) == 0) present[c] = true;
        } else {
            nv = L.buf;
        }
        if (nv.empty() && L.val.empty()) continue;
        L.raw  = L.raw.substr(0, L.vstart) + nv + L.raw.substr(L.vend);
        L.vend = L.vstart + nv.size();
        L.val  = nv;
        strncpy_s(L.buf, sizeof(L.buf), nv.c_str(), _TRUNCATE);
    }
    // Append any curated key the file didn't have (e.g. config_gui on old files).
    std::string extra;
    for (size_t c = 0; c < sizeof(CURATED)/sizeof(CURATED[0]); ++c) {
        if (present[c]) continue;
        std::string v = cfg_value_str(CURATED[c]);
        if (v.empty()) continue;
        extra += CURATED[c]; extra += " = "; extra += v; extra += "\n";
    }
    char path[MAX_PATH]; cfg_path(path, sizeof(path));
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    for (auto& L : s_model) fputs(L.raw.c_str(), f);
    if (!extra.empty()) {
        fputs("\n# --- keys added by the in-game config overlay ---\n", f);
        fputs(extra.c_str(), f);
    }
    fclose(f);
    return true;
}

// ─────────────────────────── small UI helpers ─────────────────────────────
static void help_tip(const char* t) {
    if (t && *t && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", t);
}
// Checkbox bound to a bool, with tooltip + optional "(restart)" tag.
static void cb(const char* label, bool* v, const char* help, bool live = true) {
    ImGui::Checkbox(label, v);
    help_tip(help);
    if (!live) { ImGui::SameLine(); ImGui::TextDisabled("(restart)"); }
}

// "Press a key" capture button. Returns a captured VK (>0) once, else 0.
static int key_capture(const char* id_label, int id, int cur_vk) {
    int result = 0;
    bool active = s_cap_active.load(std::memory_order_relaxed) && s_cap_owner == id;
    char name[32]; rect_tuner_vk_name(cur_vk, name, sizeof(name));
    char btn[80];
    if (active) snprintf(btn, sizeof(btn), "press a key... (Esc=cancel)##cap%d", id);
    else        snprintf(btn, sizeof(btn), "%s##cap%d", name, id);
    if (ImGui::Button(btn, ImVec2(190, 0)) && !active) {
        s_cap_owner = id; s_cap_vk.store(-1, std::memory_order_relaxed);
        s_cap_active.store(true, std::memory_order_relaxed); active = true;
    }
    ImGui::SameLine(); ImGui::TextUnformatted(id_label);
    if (active) {
        int v = s_cap_vk.load(std::memory_order_relaxed);
        if (v == -2)      { s_cap_active.store(false, std::memory_order_relaxed); }
        else if (v > 0)   { result = v; s_cap_active.store(false, std::memory_order_relaxed); }
    }
    return result;
}

// ──────────────────────────────── tabs ────────────────────────────────────
static void tab_debug() {
    ImGui::TextWrapped("Diagnostic logging to d3d9_hook.log (and the console, if on). "
                       "Leave these OFF for clean play.");
    ImGui::Separator();
    cb("console (debug console window)", &g_cfg.console_enabled,
       "Allocate a console mirroring the log. Applied at startup.", false);
    cb("dat_log", &g_cfg.dat_log, "Log every DAT/CFG the engine opens.");
    cb("tex_dump", &g_cfg.tex_dump_enabled, "Dump each uploaded texture to tex_dump/.");
    cb("tex_prov_log", &g_cfg.tex_prov_log, "Texture provenance (which DAT/recNN) log.", false);
    cb("rect_trace", &g_cfg.rect_trace, "Log every UI rect submitted at 0x502070.", false);
    cb("box_autofit_log_all", &g_cfg.box_autofit_log_all,
       "Log every menu-box draw (needs box_autofit installed).", false);
    cb("text_render_log", &g_cfg.text_render_log, "Decode & print each rendered dialog buffer.", false);
    cb("censor_fix_trace", &g_cfg.censor_fix_trace, "Uncensor per-context PC trace (F6 dumps).");
    ImGui::Separator();
    cb("unlock_vsync", &g_cfg.unlock_vsync, "Remove the 30fps cap (needed for fast-forward).", false);
}

static void tab_turbo() {
    ImGui::TextWrapped("Fast-forward / Turbo. The hotkey CYCLES 1x -> 2x -> 4x -> 8x -> 16x -> 1x.");
    ImGui::Separator();
    cb("fast_forward (master enable)", &g_cfg.fast_forward_enabled,
       "Master switch for the speed hooks.");
    ImGui::SliderFloat("max scale", &g_cfg.fast_forward_scale, 1.0f, 16.0f, "%.0fx");
    help_tip("Top of the speed ladder.");
    ImGui::Spacing();
    ImGui::TextUnformatted("Turbo cycle key:");
    int nv = key_capture("(click, then press the new key)", 1, g_cfg.fast_forward_vk);
    if (nv > 0) {
        g_cfg.fast_forward_vk = nv;   // live: ff_poll_thread reads this each poll
        snprintf(s_status, sizeof(s_status), "Turbo key set to %s (live)", vk_ff_token(nv).c_str());
    }
    cb("fast_forward_pad (gamepad cycles too)", &g_cfg.fast_forward_pad,
       "Also cycle speed from a gamepad button (XInput).");
    ImGui::TextUnformatted("Turbo controller button:");
    {
        ImGuiIO& io = ImGui::GetIO();
        static bool s_pad_cap = false;
        // While capturing, suspend ImGui gamepad-nav so the press doesn't also
        // drive the menu; restore it otherwise (controller users can navigate).
        if (s_pad_cap) io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
        else           io.ConfigFlags |=  ImGuiConfigFlags_NavEnableGamepad;
        if (s_pad_cap) {
            char tok[16];
            if (ff_pad_capture(tok, sizeof(tok))) {
                ff_pad_set(tok);
                g_cfg.fast_forward_pad = true;   // capturing implies "use the pad"
                s_pad_cap = false;
                snprintf(s_status, sizeof(s_status), "Turbo pad button set to %s (live)", tok);
            }
            if (ImGui::Button("press a controller button... (click to cancel)##padcap", ImVec2(320, 0)))
                s_pad_cap = false;
        } else {
            char btn[48]; snprintf(btn, sizeof(btn), "%s##padcap", ff_pad_current());
            if (ImGui::Button(btn, ImVec2(320, 0))) s_pad_cap = true;
            ImGui::SameLine(); ImGui::TextUnformatted("(click, then press a controller button)");
        }
        ImGui::TextDisabled("Combos like L1+R1 are still supported via the config file.");
    }
}

static void tab_tuner() {
    ImGui::TextWrapped("In-game Rect Tuner. Enabling installs the tuner (needs a restart); "
                       "key rebinds below apply LIVE.");
    ImGui::Separator();
    cb("rect_tuner (enable the tuner)", &g_cfg.rect_tuner, "Press the toggle key in-game to tune UI rects.", false);
    cb("rect_fix (apply rect_widths.txt / rect_tuner.txt)", &g_cfg.rect_fix, "Runtime UI un-clip overrides.", false);
    ImGui::Separator();
    ImGui::TextUnformatted("Key bindings (live):");
    int n = rect_tuner_key_count();
    for (int i = 0; i < n; ++i) {
        int nv = key_capture(rect_tuner_key_action(i), 100 + i, rect_tuner_key_vk(i));
        if (nv > 0) {
            rect_tuner_key_set(i, nv);
            snprintf(s_status, sizeof(s_status), "%s rebound (live)", rect_tuner_key_action(i));
        }
    }
    ImGui::TextDisabled("Note: the < + wheel / Ctrl+wheel modifiers also work; wheel actions "
                        "stay in rect_tuner_keys.txt.");
}

static void tab_display() {
    ImGui::TextWrapped("Subtitles over the intro FMV (from MOV\\ZBOF4.srt).");
    ImGui::Separator();
    cb("subtitle (show intro-FMV subtitles)", &g_cfg.subtitle_enabled, "Overlay SRT cues on the intro movie.");
    ImGui::SliderFloat("font scale", &g_cfg.subtitle_font_scale, 0.5f, 3.0f, "%.2f");
    ImGui::SliderInt("timing offset (ms)", &g_cfg.subtitle_time_offset_ms, -3000, 3000);
    help_tip("Nudge subtitle timing earlier (-) or later (+).");
}

static void tab_fixes() {
    ImGui::TextWrapped("Gameplay / localization fixes. Most are applied once at startup, "
                       "so a change here needs a restart (marked).");
    ImGui::Separator();
    cb("menu_rect_fix", &g_cfg.menu_rect_fix, "Un-clip / reposition title-menu labels.", false);
    cb("box_autofit", &g_cfg.box_autofit, "Auto-grow menu boxes to fit text.", false);
    ImGui::SliderInt("box_autofit_pad", &g_cfg.box_autofit_pad, 0, 64);
    cb("save_anywhere", &g_cfg.save_anywhere_enabled, "Allow saving outside the world map.", false);
    cb("item_name_uncap", &g_cfg.item_name_uncap_enabled, "Lift the 12-char [09] item-name cap.", false);
    ImGui::SliderInt("item_name_uncap_value", &g_cfg.item_name_uncap_value, 1, 40);
    cb("item_name_msg_uncap", &g_cfg.item_name_msg_uncap_enabled, "Lift the 14-char [07] item-name cap.", false);
    cb("bgm_loop", &g_cfg.bgm_loop_enabled, "Seamless BGM looping.", false);
    cb("free_camera", &g_cfg.free_camera, "Experimental free field-camera rotation (GOG).", false);
    ImGui::Separator();
    cb("censor_fix_aream031", &g_cfg.censor_fix_aream031, "Restore NA-cut AREAM031 scene.", false);
    cb("censor_fix_aream027", &g_cfg.censor_fix_aream027, "Restore NA-cut AREAM027 scene.", false);
    ImGui::Separator();
    ImGui::SliderInt("equip_menu_shift", &g_cfg.equip_menu_shift_px, 0, 60);
    ImGui::SliderInt("cmdbox_cursor_w", &g_cfg.cmdbox_cursor_w, 0, 0xFC);
    ImGui::SliderInt("cmdbox_label_x", &g_cfg.cmdbox_label_x, 0, 48);
    ImGui::SliderInt("cmdbox_cmdgfx_x", &g_cfg.cmdbox_cmdgfx_x, 0, 48);
}

static void tab_vwf() {
    static int  s_ch = 'A', s_last = -1;
    static bool s_16 = false, s_16_last = false;
    static int  s_b = 0, s_a = 0;
    static bool s_bear = true;
    // Per-glyph ORIGINAL metrics, captured the first time each glyph is selected
    // (before any edit), so "Revert to original" can undo a live poke. Indexed
    // [16px?][byte]. s_seen guards the one-time capture.
    static int  s_orig_b[2][256] = {}, s_orig_a[2][256] = {};
    static bool s_seen[2][256]   = {};

    ImGui::TextWrapped("Live glyph spacing. Adjust the ADVANCE (space after a glyph) and "
                       "BEARING (left offset) of an EXISTING glyph and see it in-game "
                       "immediately - no reload. This does NOT add new glyphs. Edits apply "
                       "to memory instantly and are NOT written to any file; bake them with "
                       "the Font Editor's vwf_config.txt (use 'Copy vwf_config line').");
    ImGui::Separator();

    ImGui::SliderInt("glyph byte", &s_ch, 0x20, 0xFF, "0x%02X");
    unsigned c = (unsigned)s_ch;
    char disp[16]; snprintf(disp, sizeof(disp), "'%c'", (c >= 0x20 && c < 0x7F) ? (char)c : ' ');
    ImGui::SameLine(); ImGui::TextUnformatted(disp);
    ImGui::Checkbox("16px font (small)", &s_16);

    // On glyph/size change: read the live values for display, and snapshot the
    // ORIGINAL once (this runs on selection, before the user edits this glyph).
    if (s_ch != s_last || s_16 != s_16_last) {
        int b = 0, a = 0;
        if (vwf_live_read(s_ch, s_16, &b, &a)) {
            s_b = b; s_a = a;
            const int f = s_16 ? 1 : 0;
            if (!s_seen[f][s_ch]) { s_orig_b[f][s_ch] = b; s_orig_a[f][s_ch] = a; s_seen[f][s_ch] = true; }
        }
        s_last = s_ch; s_16_last = s_16;
    }
    const int f = s_16 ? 1 : 0;

    bool ed = false;
    ed |= ImGui::SliderInt("advance (spacing)", &s_a, 0, 80);
    ImGui::Checkbox("also set bearing", &s_bear);
    if (s_bear) ed |= ImGui::SliderInt("bearing (left offset)", &s_b, -16, 48);
    if (ed) {
        if (!vwf_live_poke(s_ch, s_b, s_a, s_bear, s_16))
            snprintf(s_status, sizeof(s_status), "VWF poke failed (unexpected exe base?)");
    }

    if (s_seen[f][s_ch])
        ImGui::TextDisabled("original: bearing %d, advance %d", s_orig_b[f][s_ch], s_orig_a[f][s_ch]);

    ImGui::Spacing();
    if (ImGui::Button("Revert to original")) {
        if (s_seen[f][s_ch]) {
            s_b = s_orig_b[f][s_ch]; s_a = s_orig_a[f][s_ch];
            vwf_live_poke(s_ch, s_b, s_a, true, s_16);   // restore bearing too
            snprintf(s_status, sizeof(s_status),
                     "Reverted 0x%02X to original (bearing %d, advance %d)", s_ch, s_b, s_a);
        } else {
            snprintf(s_status, sizeof(s_status), "No original captured for 0x%02X yet", s_ch);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Re-read from game")) { s_last = -1; }
    ImGui::SameLine();
    if (ImGui::Button("Copy vwf_config line")) {
        char line[64];
        snprintf(line, sizeof(line), "0x%02X = %c : %d : %d",
                 s_ch, (c >= 0x20 && c < 0x7F) ? (char)c : '?', s_b, s_a);
        ImGui::SetClipboardText(line);
        snprintf(s_status, sizeof(s_status), "Copied: %s", line);
    }
}

static void tab_advanced() {
    if (!s_model_loaded) cfg_model_load();
    ImGui::TextWrapped("Every remaining key in _d3d9_hook_config.txt. Edits here are written "
                       "to the file on Save and take effect on the next launch.");
    ImGui::Separator();
    ImGui::BeginChild("adv", ImVec2(0, 0), true);
    int id = 0;
    for (auto& L : s_model) {
        if (!L.kv || is_curated(L.key.c_str())) continue;
        ImGui::PushID(id++);
        // true/false -> checkbox, else text field
        if (_stricmp(L.buf, "true") == 0 || _stricmp(L.buf, "false") == 0) {
            bool bv = (_stricmp(L.buf, "true") == 0);
            if (ImGui::Checkbox(L.key.c_str(), &bv))
                strncpy_s(L.buf, sizeof(L.buf), bv ? "true" : "false", _TRUNCATE);
        } else {
            ImGui::SetNextItemWidth(140);
            ImGui::InputText("##v", L.buf, sizeof(L.buf));
            ImGui::SameLine(); ImGui::TextUnformatted(L.key.c_str());
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

static void tab_about() {
    ImGui::Text("BoF4 d3d9 hook - in-game config overlay");
    ImGui::Separator();
    ImGui::Text("Dear ImGui %s", IMGUI_VERSION);
    ImGui::Spacing();
    char key[32];
    if (g_cfg.config_gui_vk == 0) snprintf(key, sizeof(key), "^ / ~  (top-left key)");
    else rect_tuner_vk_name(g_cfg.config_gui_vk, key, sizeof(key));
    ImGui::Text("Toggle this panel: %s", key);
    ImGui::Spacing();
    ImGui::TextUnformatted("Rebind toggle key:");
    int nv = key_capture("(click, then press the new key)", 2, g_cfg.config_gui_vk);
    if (nv > 0) {
        g_cfg.config_gui_vk = nv;   // live: the WndProc reads this immediately
        snprintf(s_status, sizeof(s_status), "Overlay toggle set to %s (live) - Save to keep",
                 vk_ff_token(nv).c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset to ^ / ~")) {
        g_cfg.config_gui_vk = 0;
        snprintf(s_status, sizeof(s_status), "Overlay toggle reset to ^ / ~ (live) - Save to keep");
    }
    ImGui::Spacing();
    ImGui::TextWrapped("Save writes _d3d9_hook_config.txt and rect_tuner_keys.txt, preserving "
                       "your comments. Turbo-key and Rect-Tuner rebinds apply live; flags "
                       "tagged (restart) install at launch.");
}

static void build_ui() {
    ImGui::SetNextWindowSize(ImVec2(600, 560), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("BoF4 Hook Settings", &s_show)) {
        if (s_welcome) {
            char key[32];
            if (g_cfg.config_gui_vk == 0) snprintf(key, sizeof(key), "^ / ~  (top-left key)");
            else rect_tuner_vk_name(g_cfg.config_gui_vk, key, sizeof(key));
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 226, 120, 255));
            ImGui::TextWrapped("Welcome! Press  %s  any time to open this menu.", key);
            ImGui::PopStyleColor();
            ImGui::TextWrapped("You can rebind that toggle key in the About tab, and change the "
                               "Turbo / fast-forward key in the Turbo / FF tab. Click Save to keep changes.");
            if (ImGui::Button("Got it")) s_welcome = false;
            ImGui::Separator();
        }
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("Turbo / FF"))     { tab_turbo();    ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Intro Subtitles")){ tab_display();  ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Advanced"))       { tab_advanced(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("About"))          { tab_about();    ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
        // Retired tabs (debug / rect-tuner / fixes / VWF) kept in source for reuse.
        (void)&tab_debug; (void)&tab_tuner; (void)&tab_fixes; (void)&tab_vwf;
        ImGui::Separator();
        if (ImGui::Button("Save to config files")) {
            bool ok1 = cfg_model_save();
            bool ok2 = rect_tuner_keys_save();
            snprintf(s_status, sizeof(s_status), "%s  |  tuner keys: %s",
                     ok1 ? "config saved" : "config SAVE FAILED",
                     ok2 ? "saved" : "not saved");
        }
        ImGui::SameLine();
        if (ImGui::Button("Reload config file")) {
            cfg_model_load();
            snprintf(s_status, sizeof(s_status), "reloaded _d3d9_hook_config.txt from disk");
        }
        ImGui::SameLine();
        if (ImGui::Button("Close")) s_show = false;
        if (s_status[0]) { ImGui::SameLine(); ImGui::TextDisabled("%s", s_status); }
    }
    ImGui::End();
}

// ─────────────────────────── WndProc subclass ─────────────────────────────
static LRESULT CALLBACK overlay_wndproc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Key-capture takes priority so the pressed key is grabbed, not acted on.
    if (s_cap_active.load(std::memory_order_relaxed)) {
        if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
            int vk = (int)wParam;
            s_cap_vk.store(vk == VK_ESCAPE ? -2 : vk, std::memory_order_relaxed);
            return 0;
        }
        if (msg == WM_KEYUP || msg == WM_SYSKEYUP || msg == WM_CHAR) return 0;
    }

    // Toggle the panel (ignore auto-repeat via the previous-key-state bit 30).
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && !(lParam & (1 << 30))) {
        bool hit;
        if (g_cfg.config_gui_vk == 0) hit = (((lParam >> 16) & 0xFF) == 0x29);  // scan code ^/~
        else                          hit = ((int)wParam == g_cfg.config_gui_vk);
        if (hit) {
            s_show = !s_show;
            g_config_overlay_active.store(s_show, std::memory_order_relaxed);
            return 0;
        }
    }

    // While open, the overlay OWNS mouse + keyboard: feed ImGui, then swallow
    // every input message so the game (and wrapper) never sees a click or key.
    // Non-input messages still pass through to the game's WndProc.
    if (s_show) {
        ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
        switch (msg) {
            case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
            case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
            case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
            case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: case WM_SETCURSOR:
            case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_CHAR:
                return 1;   // swallow — do not forward to the game
            default: break;
        }
    }
    return CallWindowProc(s_orig_wndproc, hWnd, msg, wParam, lParam);
}

// ───────────────────────────── init / present ─────────────────────────────
static void ensure_init(IDirect3DDevice9* dev) {
    if (s_inited) return;
    HWND hwnd = g_dev_window ? g_dev_window : GetForegroundWindow();
    if (!hwnd || !dev) return;   // retry next frame

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;    // don't litter an imgui.ini in the game folder
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.MouseDrawCursor = true;   // the game may hide the OS cursor
    ImGui::StyleColorsDark();

    if (!ImGui_ImplWin32_Init(hwnd)) { ImGui::DestroyContext(); return; }
    if (!ImGui_ImplDX9_Init(dev))    { ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext(); return; }

    s_orig_wndproc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)overlay_wndproc);
    s_hwnd   = hwnd;
    s_device = dev;
    s_inited = true;
    cfg_model_load();
    // LL mouse hook thread (swallows clicks from the game while the panel is open).
    s_mouse_thread = CreateThread(nullptr, 0, mouse_thread_proc, nullptr, 0, nullptr);
}

void config_overlay_on_present(IDirect3DDevice9* dev) {
    if (!g_cfg.config_gui || !dev) return;
    if (!s_inited) { ensure_init(dev); if (!s_inited) return; }
    if (dev != s_device) return;    // ignore a foreign device (shouldn't happen)
    first_run_check();              // auto-open + greet on the very first launch
    if (!s_show) return;            // hidden: toggle is handled in the WndProc

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    feed_mouse_events();   // inject LL-hook mouse buttons/wheel (WndProc ones are eaten)
    ImGui::NewFrame();
    build_ui();
    ImGui::Render();

    // Draw onto the backbuffer explicitly (RT isn't part of a D3D9 state block).
    IDirect3DSurface9* oldRT = nullptr;
    IDirect3DSurface9* bb    = nullptr;
    dev->GetRenderTarget(0, &oldRT);
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb)
        dev->SetRenderTarget(0, bb);
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    if (oldRT) { dev->SetRenderTarget(0, oldRT); oldRT->Release(); }
    if (bb) bb->Release();

    // Keep the active flag in sync if the window's [x] closed the panel.
    g_config_overlay_active.store(s_show, std::memory_order_relaxed);
}

void config_overlay_on_lost_device() {
    if (s_inited) ImGui_ImplDX9_InvalidateDeviceObjects();  // recreated on next NewFrame
}
