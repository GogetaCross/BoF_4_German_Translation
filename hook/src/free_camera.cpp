// free_camera.cpp — see free_camera.h.
//
// EXPERIMENTAL free (smooth) field-camera rotation. GOG only, config-gated.
//
// Discovery (via rcos-tracer RE): the field camera is an orbit camera whose yaw
// is the 12-bit word at BOF4!0x00BAD7C6 (4096 = 360deg). It is read every frame
// by the camera-position/view math (e.g. 0x65EC54/0x65EC78 feed it to rcos), and
// during normal exploration NOTHING rewrites it while idle — the 90deg turn is
// just a tween that adds to it and snaps. So we can rotate freely by adding a
// small per-frame delta to 0xBAD7C6 ourselves.
//
//   Q / E                rotate the camera left / right (hold).
//   Right analog stick    rotate proportionally (partial tilt = slower). XInput.
//
// The game's own rotate button still works (and will snap from wherever we left
// the camera). Scripted/cutscene cameras overwrite 0xBAD7C6 themselves, so our
// input simply has no lasting effect there — safe.
#include "free_camera.h"
#include "hooks.h"        // g_cfg, hook_log
#include "hook_health.h"  // resolve_sig — future-proof the yaw address

#include <windows.h>
#include <cstdint>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace {
    // Camera yaw (word). Default = the known GOG/Steam-parity address; on install
    // we re-resolve it by signature so it survives EXE updates (see resolve_yaw).
    volatile uint16_t* g_yaw = (volatile uint16_t*)0x00BAD7C6;

    bool read_dword_safe(uintptr_t va, uint32_t* out) {
        __try { *out = *(const uint32_t*)va; return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // Future-proof: the yaw is a DATA address, so we can't scan for it directly.
    // Instead we scan .text for the unique instruction that READS it —
    //   movsx ecx,[yaw]; mov esi,eax; push ecx; movsx eax,[g2]; sar; imul;
    //   call rcos; movsx edx,[g3]   (at BOF4!0x65EC39)
    // with the two data displacements and the call rel32 wildcarded — then read
    // the yaw displacement out of the matched instruction.
    void resolve_yaw() {
        static const uint8_t pat[] = {
            0x0F,0xBF,0x0D, 0,0,0,0,          // movsx ecx, word [yaw]   (disp @ +3)
            0x8B,0xF0,                        // mov esi, eax
            0x51,                             // push ecx
            0x0F,0xBF,0x05, 0,0,0,0,          // movsx eax, word [g2]
            0xD1,0xF8,                        // sar eax, 1
            0x0F,0xAF,0xF0,                   // imul esi, eax
            0xE8, 0,0,0,0,                    // call rcos (rel32)
            0x0F,0xBF,0x15, 0,0,0,0 };        // movsx edx, word [g3]
        static const char msk[] = "xxx????xxxxxx????xxxxxx????xxx????";
        uintptr_t dispVA = resolve_sig("freecam_yaw", pat, msk, sizeof(pat), 3);
        if (dispVA) {
            uint32_t addr = 0;
            if (read_dword_safe(dispVA, &addr) && addr >= 0x008D4000 && addr < 0x00C78000) {
                g_yaw = (volatile uint16_t*)(uintptr_t)addr;
                hook_log("[freecam] yaw resolved by signature -> 0x%08X\n", addr);
                return;
            }
        }
        hook_log("[freecam] yaw signature miss; using fallback 0x%08X\n",
                 (unsigned)(uintptr_t)g_yaw);
    }

    // Per-tick step in 12-bit units (4096 = 360deg). 0x18 * 60Hz ~= 202deg/sec.
    int  g_step = 0x18;
    int  g_key_left   = 'Q';
    int  g_key_right  = 'E';
    int  g_key_toggle = VK_F9;
    volatile bool g_active = true;      // runtime on/off (starts on when installed)

    // Parse a key name -> virtual-key. Accepts A-Z, 0-9, F1-F12, a few names,
    // or a raw 0xNN / decimal code. Empty -> 0 (keep current default).
    int vk_from_name(const char* s) {
        if (!s || !s[0]) return 0;
        char u[16]; int i = 0;
        for (; s[i] && i < 15; ++i) u[i] = (char)toupper((unsigned char)s[i]);
        u[i] = 0;
        if (u[1] == 0) { char c = u[0];
            if (c >= 'A' && c <= 'Z') return c;
            if (c >= '0' && c <= '9') return c; }
        if (u[0] == 'F' && u[1]) { int n = atoi(u+1); if (n >= 1 && n <= 12) return VK_F1 + (n-1); }
        if (!strcmp(u,"TAB"))   return VK_TAB;
        if (!strcmp(u,"SPACE")) return VK_SPACE;
        if (!strcmp(u,"LSHIFT"))return VK_LSHIFT;
        if (!strcmp(u,"RSHIFT"))return VK_RSHIFT;
        if (!strcmp(u,"HOME"))  return VK_HOME;
        if (!strcmp(u,"END"))   return VK_END;
        return (int)strtol(u, nullptr, 0);
    }

    // ── XInput (right stick), loaded dynamically so we don't hard-depend on it ─
    struct XI_GAMEPAD { WORD wButtons; BYTE bLeftTrigger, bRightTrigger;
                        SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; };
    struct XI_STATE   { DWORD dwPacketNumber; XI_GAMEPAD Gamepad; };
    typedef DWORD (WINAPI *XInputGetState_t)(DWORD, XI_STATE*);
    XInputGetState_t g_XInputGetState = nullptr;
    const int RSTICK_DEADZONE = 8000;   // ~XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE

    void load_xinput() {
        const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
        for (const char* name : dlls) {
            HMODULE h = LoadLibraryA(name);
            if (h) {
                g_XInputGetState = (XInputGetState_t)GetProcAddress(h, "XInputGetState");
                if (g_XInputGetState) { hook_log("[freecam] XInput via %s\n", name); return; }
            }
        }
        hook_log("[freecam] XInput not available — keyboard only.\n");
    }

    // Right-stick X -> per-tick delta (proportional beyond the deadzone).
    int analog_delta() {
        if (!g_XInputGetState) return 0;
        XI_STATE st{};
        for (DWORD i = 0; i < 4; ++i) {           // first connected pad
            if (g_XInputGetState(i, &st) == 0 /*ERROR_SUCCESS*/) {
                SHORT rx = st.Gamepad.sThumbRX;
                int a = (rx < 0) ? -rx : rx;
                if (a <= RSTICK_DEADZONE) return 0;
                double mag = (double)(a - RSTICK_DEADZONE) / (32767.0 - RSTICK_DEADZONE);
                if (mag > 1.0) mag = 1.0;
                int mx = (g_step * 3) / 2;          // full tilt a touch faster than a key
                int d  = (int)(mag * mx + 0.5);
                return (rx < 0) ? -d : d;
            }
        }
        return 0;
    }

    // Disable free-cam in the current area if it's in the off-list (overworld).
    bool area_disabled() {
        char area[16]; bof4_current_area(area, sizeof(area));
        if (!area[0]) return false;
        const char* list = g_cfg.free_camera_off_areas[0]
                         ? g_cfg.free_camera_off_areas : "AREAE005";
        char buf[128]; strncpy_s(buf, sizeof(buf), list, _TRUNCATE);
        char* p = buf;
        while (*p) {
            while (*p == ' ' || *p == ',') ++p;
            char* s = p;
            while (*p && *p != ',' && *p != ' ') ++p;
            char save = *p; *p = 0;
            bool hit = (s[0] && _stricmp(s, area) == 0);
            *p = save;
            if (hit) return true;
        }
        return false;
    }

    HANDLE        g_thread = nullptr;
    volatile bool g_quit   = false;

    DWORD WINAPI poll_thread(LPVOID) {
        load_xinput();
        hook_log("[freecam] free rotation active: Q/E + right stick (yaw @0x%08X, "
                 "step=0x%X). Off in overworld + cutscenes.\n",
                 (unsigned)(uintptr_t)g_yaw, g_step);
        uint16_t last_set = 0; bool have_last = false; int suppress = 0;
        bool tog_prev = false;
        // Real-time pacing. Fast-forward hooks Sleep()/GetTickCount()/QPC and
        // skips short sleeps, so this loop spins much faster under turbo — which
        // used to multiply the rotation speed. GetTickCount64 is NOT hooked by
        // the ff module, so we advance the yaw by REAL elapsed time and carry a
        // fractional remainder: rotation speed is then constant at any turbo x.
        ULONGLONG last_ms = GetTickCount64();
        double    accum   = 0.0;
        while (!g_quit) {
            bool foreground = (GetForegroundWindow() != nullptr);
            // Runtime toggle (rising edge).
            bool tog = (GetAsyncKeyState(g_key_toggle) & 0x8000) != 0;
            if (tog && !tog_prev && foreground) {
                g_active = !g_active;
                hook_log("[freecam] toggled %s\n", g_active ? "ON" : "OFF");
            }
            tog_prev = tog;

            uint16_t cur = *g_yaw;
            // CUTSCENE / scripted-camera guard: during normal field idle nothing
            // rewrites the yaw, so if the GAME changed it since our last write, a
            // scripted/cutscene camera (or the game's own turn) is driving it —
            // back off for a short window so we don't fight it.
            if (have_last && cur != last_set) suppress = 30;   // ~0.5s

            int d = 0;
            if (GetAsyncKeyState(g_key_left)  & 0x8000) d -= g_step;
            if (GetAsyncKeyState(g_key_right) & 0x8000) d += g_step;
            if (d == 0) d = analog_delta();          // stick if no key held

            if (!g_active)      d = 0;               // runtime toggle off
            if (area_disabled()) d = 0;              // overworld etc.
            if (suppress > 0) { --suppress; d = 0; } // cutscene / yield window

            // Advance by real elapsed time. `d` is the legacy per-(1/60)s step,
            // so d*60 = units/sec; multiply by real dt and carry the remainder.
            ULONGLONG now = GetTickCount64();
            double dt = (double)(now - last_ms) / 1000.0;
            last_ms = now;
            if (dt > 0.1) dt = 0.1;                  // clamp after a stall/pause

            if (d != 0 && foreground) {
                accum += (double)d * 60.0 * dt;
                int step = (int)accum;               // truncates toward 0
                accum -= step;
                if (step != 0) {
                    uint16_t nv = (uint16_t)((cur + step) & 0x0FFF);
                    *g_yaw = nv; last_set = nv;
                } else {
                    last_set = cur;                  // sub-unit tick: track game changes
                }
            } else {
                accum = 0.0;
                last_set = cur;                      // track game-driven changes
            }
            have_last = true;
            Sleep(16);   // ~60 Hz (may be skipped under turbo; pacing is time-based)
        }
        return 0;
    }
}

void free_camera_install() {
    if (!g_cfg.free_camera) return;
    if (g_thread) return;
    resolve_yaw();   // signature-scan the yaw address (future-proof vs EXE updates)
    // Optional tuning from config (free_camera_speed), else default step.
    if (g_cfg.free_camera_speed > 0 && g_cfg.free_camera_speed <= 0x400)
        g_step = g_cfg.free_camera_speed;
    int k;
    if ((k = vk_from_name(g_cfg.free_camera_key_left))   > 0) g_key_left   = k;
    if ((k = vk_from_name(g_cfg.free_camera_key_right))  > 0) g_key_right  = k;
    if ((k = vk_from_name(g_cfg.free_camera_toggle_key)) > 0) g_key_toggle = k;
    hook_log("[freecam] keys: left=0x%02X right=0x%02X toggle=0x%02X\n",
             g_key_left, g_key_right, g_key_toggle);
    g_quit = false;
    g_thread = CreateThread(nullptr, 0, poll_thread, nullptr, 0, nullptr);
    hook_log("[freecam] free-rotation installed (free_camera=true).\n");
}

void free_camera_shutdown() {
    g_quit = true;
    if (g_thread) { WaitForSingleObject(g_thread, 200); CloseHandle(g_thread); g_thread = nullptr; }
}
