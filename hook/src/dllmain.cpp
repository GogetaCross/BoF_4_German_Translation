// BoF4 d3d9 proxy — DLL entry and Direct3DCreate9 trampoline.
//
// This DLL is dropped into the game folder as d3d9.dll. Windows loads it
// before the real system d3d9.dll (DLL search order). We then dynamically
// load the real one from System32, forward Direct3DCreate9, and install
// vtable hooks on the returned IDirect3D9 object.

#include "hooks.h"
#include <cstdio>
#include <cstdint>

HMODULE           g_real_d3d9            = nullptr;
Direct3DCreate9_t g_real_Direct3DCreate9 = nullptr;

static void load_real_d3d9() {
    char path[MAX_PATH];
    UINT n = GetSystemDirectoryA(path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 16) return;
    strcat_s(path, MAX_PATH, "\\d3d9.dll");

    g_real_d3d9 = LoadLibraryA(path);
    if (!g_real_d3d9) return;

    g_real_Direct3DCreate9 =
        (Direct3DCreate9_t)GetProcAddress(g_real_d3d9, "Direct3DCreate9");
}

BOOL WINAPI DllMain(HMODULE /*hModule*/, DWORD reason, LPVOID /*reserved*/) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        load_real_d3d9();
        // tex_dump/ and tex_inject/ are NOT created here. They used to be made
        // unconditionally at attach — before the config is even parsed — which
        // littered every install with two empty folders nobody asked for.
        // hook_bof4_install() now creates them only when their flag is on.
        hook_log("d3d9_hook v0.28 loaded  (real d3d9=%p, minhook=on, "
                 "fileio=on, init-dat-capture=on, text-resolver=on, "
                 "bgm-loop=on, subtitles=opt-in, tex-inject=opt-in)\n",
                 (void*)g_real_d3d9);
        if (!g_real_Direct3DCreate9) {
            hook_log("ERROR: could not resolve real Direct3DCreate9\n");
            return FALSE;
        }
        // Install inline hooks on BOF4.exe via MinHook. Runs AFTER the
        // log is open so any init errors are visible.
        hook_bof4_install();
        break;

    case DLL_PROCESS_DETACH:
        hook_bof4_shutdown();
        hook_log("d3d9_hook unloading\n");
        break;
    }
    return TRUE;
}

// Exported via d3d9.def (no __declspec(dllexport) here — would conflict
// with d3d9.h's DECLSPEC_IMPORT declaration if anyone re-includes it).
extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT sdk_version) {
    if (!g_real_Direct3DCreate9) return nullptr;
    IDirect3D9* real = g_real_Direct3DCreate9(sdk_version);
    hook_log("Direct3DCreate9(sdk=%u) -> %p\n", sdk_version, (void*)real);
    if (real) hook_d3d9(real);
    return real;
}
