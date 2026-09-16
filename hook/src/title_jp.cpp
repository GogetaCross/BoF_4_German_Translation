// ─────────────────────────────────────────────────────────────────────────────
// title_jp.cpp — render the Japanese title card on the Western build.
//
// WHY A DETOUR AND NOT AN IMMEDIATE PATCH
// The Western draw_title_logo has exactly FOUR primitive slots with a fixed
// tpage sequence (0x9A, 0x9C, a quad whose page is embedded, 0x9E). The JP
// build's list needs SIX rects — 1x page A, 3x page B, 2x page C — so there is
// no rewrite of the existing immediates that can express it: you would still be
// two rects short. Hence we replace the whole function and re-emit.
//
// WHAT WE EMIT
// Transcribed from the Japanese executable's own routines (draw_title_logo at
// JP 0x502B70, draw_copyright at JP 0x502CF0), not invented to fit. The JP build
// uses only plain GP0 0x64 rects — the rotated GP0 0x2C quad that assembles the
// Western "IV" tail is a Western-only construction and simply does not appear.
//
// The JP art must be present in DEMO2.DAT for this to draw anything sensible:
// records 0..5 (three 8bpp 256x256 pages + their three palettes) come from the
// Japanese DEMO2.DAT. Their op/param/decompSize are identical to the Western
// ones, so they splice in byte-for-byte with only the TOC offsets rebuilt.
//
// PALETTES: the JP logo uses THREE CLUTs (0x7C00 / 0x7C40 / 0x7C80) where the
// Western logo uses one (0x7C00). That is exactly why a plain DAT swap cannot
// work — the palette is chosen per-primitive on the draw side, not by the
// texture record.
//
// The queue-write global, the tpage setter and the submit routine are the
// game's own; we drive them exactly as the original code does, so packets stay
// in the engine's normal ordering.
// ─────────────────────────────────────────────────────────────────────────────
#include <windows.h>
#include <stdint.h>

#include "hooks.h"
#include "title_jp.h"
#include "minhook/MinHook.h"

// ── Western build addresses (guarded; see verify_site) ──────────────────────
static const uintptr_t VA_DRAW_LOGO   = 0x00502880;
static const uintptr_t VA_DRAW_COPYR  = 0x005029D0;
static const uintptr_t VA_TPAGE_SET   = 0x004108C0;
static const uintptr_t VA_SUBMIT      = 0x00502070;
static const uintptr_t VA_QUEUE_PTR   = 0x00B4EFA0;   // dword: current packet slot

// Byte guards — the exact immediates the Western routines write. All three are
// absent from the Japanese build (whose title code is restructured and sits at
// different addresses), so this fails safe there instead of corrupting it.
struct Guard { uintptr_t va; uint32_t want; };
static const Guard GUARDS[] = {
    { 0x005028B5, 0x0024002C },   // logo rect 1 position  (x=44,  y=36)
    { 0x005028C3, 0x01000100 },   // logo rect 1 size      (256 x 256)
    { 0x00502954, 0x7C0004CD },   // the rotated quad's clut/uv word
    { 0x005029AB, 0x012401A0 },   // logo rect 4 position  (x=416, y=292)
};

typedef void (__cdecl *tpage_fn)(void* queue, int a, int b, int tpage, int c);
typedef void (__cdecl *submit_fn)(int kind, int size);

static tpage_fn  g_set_tpage = (tpage_fn)VA_TPAGE_SET;
static submit_fn g_submit    = (submit_fn)VA_SUBMIT;

static void* g_orig_logo  = nullptr;
static void* g_orig_copyr = nullptr;
static bool  g_installed  = false;

// ── the Japanese draw list ─────────────────────────────────────────────────
// kind: 1 = logo layer, 3 = copyright layer (the engine's own layer ids).
struct Rect {
    int tpage;              // 0x9A/0x9C/0x9E = VRAM x 640/768/896 at y=256, 8bpp
    int x, y, w, h;         // screen
    int u, v;               // texel origin within the page
    int clut;               // CLUT VRAM coordinate
};

// JP draw_title_logo (JP 0x502B70). L3+L4 are the two stacked slabs that
// reassemble the gold "V" into one 68x145 column at x=542; L5+L6 are the brush
// subtitle and its trailing character.
static const Rect JP_LOGO[] = {
    { 0x9A,  30,  96, 256, 145,   0,   0, 0x7C00 },
    { 0x9C, 286,  96, 256, 145,   0,   0, 0x7C40 },
    { 0x9C, 542,  96,  68,  96, 120, 145, 0x7C40 },
    { 0x9C, 542, 192,  68,  49, 188, 145, 0x7C40 },
    { 0x9E, 171, 241, 256,  50,   0,   0, 0x7C80 },
    { 0x9E, 427, 241,  42,  50, 214,  50, 0x7C80 },
};

// JP draw_copyright (JP 0x502CF0) — one continuous line, two rects.
static const Rect JP_COPYRIGHT[] = {
    { 0x9E,  70, 432, 254, 24,   0, 208, 0x7D00 },
    { 0x9E, 324, 432, 256, 24,   2, 232, 0x7D00 },
};

// Write one GP0 0x64 textured rect into the queue slot and hand it to the
// engine. Packet layout, read straight off the original code:
//   [0x00] 0x04000000              tag (4 payload dwords)
//   [0x04] 0x64808080              cmd 0x64 + neutral 0x808080 modulation
//   [0x08] (y << 16) | x           screen position
//   [0x0C] (clut << 16)|(v<<8)|u   palette + texel origin
//   [0x10] (h << 16) | w           extent
static void emit_rect(int kind, const Rect& r) {
    uint32_t* q = *(uint32_t**)VA_QUEUE_PTR;
    if (!q) return;
    q[0] = 0x04000000u;
    q[1] = 0x64808080u;
    q[2] = ((uint32_t)r.y << 16) | (uint32_t)(r.x & 0xFFFF);
    q[3] = ((uint32_t)r.clut << 16) | ((uint32_t)(r.v & 0xFF) << 8) | (uint32_t)(r.u & 0xFF);
    q[4] = ((uint32_t)r.h << 16) | (uint32_t)(r.w & 0xFFFF);
    g_submit(kind, 0x14);
}

static void emit_tpage(int kind, int tpage) {
    void* q = *(void**)VA_QUEUE_PTR;
    if (!q) return;
    g_set_tpage(q, 0, 0, tpage, 0);
    g_submit(kind, 0x0C);
}

// Emit a list, setting the texture page only when it actually changes — the
// same economy the original routines use.
static void emit_list(int kind, const Rect* list, int n) {
    int cur_tpage = -1;
    for (int i = 0; i < n; i++) {
        if (list[i].tpage != cur_tpage) {
            emit_tpage(kind, list[i].tpage);
            cur_tpage = list[i].tpage;
        }
        emit_rect(kind, list[i]);
    }
}

static void __cdecl h_draw_logo() {
    __try {
        emit_list(1, JP_LOGO, (int)(sizeof(JP_LOGO) / sizeof(JP_LOGO[0])));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Never let a title-screen draw take the process down.
    }
}

static void __cdecl h_draw_copyright() {
    __try {
        emit_list(3, JP_COPYRIGHT, (int)(sizeof(JP_COPYRIGHT) / sizeof(JP_COPYRIGHT[0])));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// ── install ────────────────────────────────────────────────────────────────
static bool verify_guards() {
    for (const auto& g : GUARDS) {
        uint32_t got = 0;
        __try { got = *(uint32_t*)(g.va + 3); }          // imm32 sits at insn+3
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        if (got != g.want) {
            hook_log("title_jp: guard FAILED at 0x%08X — got 0x%08X, want 0x%08X. "
                     "Not this build; leaving the title screen alone.\n",
                     (unsigned)g.va, got, g.want);
            return false;
        }
    }
    return true;
}

void title_jp_install() {
    if (!g_cfg.jp_title) return;
    if (!verify_guards()) return;

    struct { uintptr_t va; void* det; void** orig; const char* name; } sites[] = {
        { VA_DRAW_LOGO,  (void*)h_draw_logo,      &g_orig_logo,  "draw_title_logo"  },
        { VA_DRAW_COPYR, (void*)h_draw_copyright, &g_orig_copyr, "draw_copyright"   },
    };
    int ok = 0;
    for (auto& s : sites) {
        if (MH_CreateHook((LPVOID)s.va, s.det, s.orig) == MH_OK &&
            MH_EnableHook((LPVOID)s.va) == MH_OK) {
            ok++;
        } else {
            hook_log("title_jp: hook %s @0x%08X FAILED\n", s.name, (unsigned)s.va);
        }
    }
    if (ok != 2) {
        // Partial install would draw half a title screen — back the whole thing out.
        MH_DisableHook((LPVOID)VA_DRAW_LOGO);
        MH_DisableHook((LPVOID)VA_DRAW_COPYR);
        hook_log("title_jp: only %d/2 hooks installed — reverted, title unchanged\n", ok);
        return;
    }
    g_installed = true;
    hook_log("title_jp: ENABLED — Japanese title draw list (%d logo rects, "
             "%d copyright rects, CLUTs 0x7C00/0x7C40/0x7C80). Requires the JP "
             "DEMO2.DAT records 0..5 to be present, or this will draw Western "
             "art through Japanese coordinates.\n",
             (int)(sizeof(JP_LOGO) / sizeof(JP_LOGO[0])),
             (int)(sizeof(JP_COPYRIGHT) / sizeof(JP_COPYRIGHT[0])));
}

void title_jp_shutdown() {
    if (!g_installed) return;
    MH_DisableHook((LPVOID)VA_DRAW_LOGO);
    MH_DisableHook((LPVOID)VA_DRAW_COPYR);
    g_installed = false;
}
