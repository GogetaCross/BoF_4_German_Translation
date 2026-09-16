// Title-menu label rect fix.
//
// The title/main-menu word-graphics are drawn by per-label code blocks in
// BOF4.exe .text that build PSX GP0(0x64) textured-rect packets with hardcoded
// immediates:  mov [eax+8],(y<<16)|x   (position)
//              mov [eax+0xC],clut<<16 | (v<<8)|u   (atlas texcoord)
//              mov [eax+0x10],(h<<16)|w   (size; low16 w = the CLIP width)
// English-sized widths clip wider German graphics. This patches those immediates
// IN MEMORY at startup (the exe on disk is never modified), driven by the
// editable sidecar title_rects.txt next to BOF4.exe.
//
// Most labels have one shared width for both the normal and highlighted
// (selected) states — only the atlas row differs — so one width patch fixes
// both. OPTIONS is the exception: normal and highlighted are side-by-side in the
// atlas (u=124 / u=0 at v=96) with SEPARATE width writes, so it has two entries:
//   Options    = normal      (w@0x503165, uv@0x50315E)
//   OptionsSel = highlighted (w@0x503180, uv@0x503179; shares Options' position)
// To make "Optionen" wider than 124 you must relocate its atlas source to a free
// area (Twisted-Phoenix style) via u=/v= and repaint there, since v=96 is full.
//
// Sidecar line (positional OR key=value; comments with #):
//   <Name> <newW> [newX]
//   <Name> [w=N] [x=N] [dx=N] [y=N] [u=N] [v=N]
//     w  = new width (un-clip)            x  = absolute left x
//     dx = nudge x by N (after w/x/auto)  y  = new top y
//     u,v= atlas texcoord (relocate source; 0..255)
//   If w is set and x is not, x auto-recenters on the label's old centre.
// See project_title_menu_rects memory.

#include "hooks.h"
#include "menu_rect_fix.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

struct Ent {
    const char* name;
    uintptr_t w_va;   // low16 of size imm (0 = none)
    uintptr_t x_va;   // low16 of pos imm (x); y is at x_va+2 (0 = none)
    uintptr_t u_va;   // u byte of texcoord imm; v is at u_va+1 (0 = none)
    int oldW, oldX;
};

// Current GOG BOF4.exe — see project_title_menu_rects memory.
//
// NOTE (future-proofing): these are baked packet-builder immediates. On a
// full EXE recompile (e.g. the 2026-05 GOG build) the title-menu draw code is
// restructured — the per-label blocks become a jump table and the immediates
// vanish — so a byte-signature CANNOT relocate them. This module therefore
// keeps hardcoded VAs and relies on its ABORT guard below (live widths must
// match this table) to fail SAFE on any changed EXE. The future-proof path
// for title-menu un-clipping is the universal runtime rect_hook (0x00502070),
// which rewrites widths at packet-submit time with no baked immediates.
static const Ent ENTS[] = {
    { "PressStart", 0x00502D86, 0x00502D78, 0x00502D7F, 188, 165 },
    { "Button",     0x00502DB6, 0x00502DA8, 0x00502DAF, 122, 355 },
    { "NewGame",    0x005030F2, 0x005030D5, 0,          158, 132 }, // uv split norm/sel
    { "LoadGame",   0x0050312F, 0x00503112, 0,          170, 338 }, // uv split norm/sel
    { "Options",    0x00503165, 0x00503150, 0x0050315E, 124, 256 }, // normal
    { "OptionsSel", 0x00503180, 0,          0x00503179, 124, 256 }, // highlighted
};
static const int NENT = (int)(sizeof(ENTS) / sizeof(ENTS[0]));

static bool read16(uintptr_t va, uint16_t* out) {
    __try { *out = *(volatile uint16_t*)va; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}
static bool patchN(uintptr_t va, const void* src, int n) {
    DWORD oldp;
    if (!VirtualProtect((void*)va, n, PAGE_EXECUTE_READWRITE, &oldp)) return false;
    bool ok = true;
    __try { memcpy((void*)va, src, n); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    VirtualProtect((void*)va, n, oldp, &oldp);
    FlushInstructionCache(GetCurrentProcess(), (void*)va, n);
    return ok;
}
static bool patch16(uintptr_t va, uint16_t v) { return patchN(va, &v, 2); }
static bool patch8 (uintptr_t va, uint8_t  v) { return patchN(va, &v, 1); }

static void sidecar_path(char* out, size_t n) {
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char* slash = strrchr(exe, '\\');
    if (slash) *(slash + 1) = 0; else exe[0] = 0;
    // Shared with the universal rect_hook: name-keyed title lines and u/v-keyed
    // universal lines coexist in one file (each parser ignores the other's).
    snprintf(out, n, "%srect_widths.txt", exe);
}

void menu_rect_fix_apply() {
    if (!g_cfg.menu_rect_fix) return;

    char path[MAX_PATH]; sidecar_path(path, sizeof(path));
    FILE* f = fopen(path, "r");
    // rect_widths.txt is optional — only applied if the user has hand-authored one.
    // We no longer auto-create a template on startup.
    if (!f) return;

    // Sanity: live widths must still match the table (guards a changed exe).
    // Only on the FIRST apply — after that the live widths are our own patches,
    // so a hot-reload (F8) must skip this check or it would always abort.
    static bool applied_once = false;
    if (!applied_once) {
        int matched = 0, checkable = 0;
        for (int i = 0; i < NENT; ++i) {
            uint16_t w = 0; checkable++;
            if (read16(ENTS[i].w_va, &w) && (int)w == ENTS[i].oldW) matched++;
        }
        if (matched < checkable) {
            hook_log("menu_rect_fix: ABORT — %d/%d widths matched table; exe differs.\n",
                     matched, checkable);
            fclose(f); return;
        }
    }
    applied_once = true;

    int applied = 0; char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* p = line; while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == 0) continue;
        char name[64] = {0};
        int adv = 0;
        if (sscanf(p, "%63s%n", name, &adv) < 1) continue;
        p += adv;

        int w = -1, x = INT32_MIN, dx = 0, y = INT32_MIN, u = -1, v = -1;
        int pos_count = 0;   // for legacy positional w/x
        char tok[64];
        while (1) {
            adv = 0;
            if (sscanf(p, "%63s%n", tok, &adv) < 1) break;
            p += adv;
            if (tok[0] == '#') break;
            char* eq = strchr(tok, '=');
            if (eq) {
                *eq = 0; int val = (int)strtol(eq + 1, nullptr, 0);
                if      (!_stricmp(tok, "w"))  w = val;
                else if (!_stricmp(tok, "x"))  x = val;
                else if (!_stricmp(tok, "dx")) dx = val;
                else if (!_stricmp(tok, "y"))  y = val;
                else if (!_stricmp(tok, "u"))  u = val;
                else if (!_stricmp(tok, "v"))  v = val;
            } else {                          // legacy positional
                int val = (int)strtol(tok, nullptr, 0);
                if (pos_count == 0) w = val; else if (pos_count == 1) x = val;
                pos_count++;
            }
        }

        for (int i = 0; i < NENT; ++i) {
            if (_stricmp(name, ENTS[i].name) != 0) continue;
            const Ent& e = ENTS[i];
            bool changed = false; char note[96] = {0}; int nl = 0;

            if (w > 0 && w <= 0xFFFF && w != e.oldW) {
                if (patch16(e.w_va, (uint16_t)w)) { changed = true;
                    nl += snprintf(note+nl, sizeof(note)-nl, " w=%d", w); }
            }
            // resolve x: explicit, else auto-center if w changed; then +dx
            if (e.x_va) {
                int nx = INT32_MIN;
                if (x != INT32_MIN) nx = x;
                else if (w > 0 && w != e.oldW) nx = e.oldX + (e.oldW - w) / 2;
                if (dx) { if (nx == INT32_MIN) nx = e.oldX; nx += dx; }
                if (nx != INT32_MIN && nx != e.oldX && nx >= 0 && nx <= 0xFFFF) {
                    if (patch16(e.x_va, (uint16_t)nx)) { changed = true;
                        nl += snprintf(note+nl, sizeof(note)-nl, " x=%d", nx); }
                }
            }
            if (y != INT32_MIN && e.x_va && y >= 0 && y <= 0xFFFF) {
                if (patch16(e.x_va + 2, (uint16_t)y)) { changed = true;
                    nl += snprintf(note+nl, sizeof(note)-nl, " y=%d", y); }
            }
            if (u >= 0 && u <= 255 && e.u_va) {
                if (patch8(e.u_va, (uint8_t)u)) { changed = true;
                    nl += snprintf(note+nl, sizeof(note)-nl, " u=%d", u); }
            }
            if (v >= 0 && v <= 255 && e.u_va) {
                if (patch8(e.u_va + 1, (uint8_t)v)) { changed = true;
                    nl += snprintf(note+nl, sizeof(note)-nl, " v=%d", v); }
            }
            if (changed) { hook_log("menu_rect_fix: %-11s%s\n", e.name, note); applied++; }
            break;
        }
    }
    fclose(f);
    hook_log("menu_rect_fix: applied %d change(s) from %s\n", applied, path);
}
