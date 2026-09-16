// In-game "Rect Tuner" — see rect_tuner.h for the pitch.
//
// SELECT by cycling: press + / - to step through the distinct rects on the
// current screen. The focused rect FLASHES (we recolour its own packet in the
// submit hook, so it lines up perfectly — no coordinate math). RESHAPE live with
// the arrow keys: move (arrows), resize (Shift+arrows), atlas u/v (Ctrl+arrows).
// Since the game hijacks arrows/numpad, the MOUSE WHEEL is the practical driver:
// wheel = move; Ctrl+wheel = shift atlas U/V left-right; Alt+wheel = U/V up-down.
// SAVE with Enter → one override line in rect_tuner.txt (read back by rect_hook's
// parse_overrides, so it applies immediately and every future run). Backspace
// clears the focused rect's saved line. F10 / Esc exit the mode.
//
// Threading: the 0x502070 submit hook and Present both run on the game's render
// thread — all frame-list / focus / delta state lives there, lock-free. The
// low-level keyboard hook runs on our own message-pump thread and only posts
// atomic "intents" (cycle steps, delta increments, save/clear/exit), consumed on
// the render thread in rect_tuner_on_present. Every foreign packet read/write is
// SEH-guarded exactly like rect_hook.cpp's write_pkt.

#include "rect_tuner.h"
#include "tex_prov.h"
#include "box_autofit.h"   // box-drawer caller list for the F10 HUD
#include "rect_hook.h"     // rect_box_text_hud: box + inner-text identifier
#include "hooks.h"

#include <windows.h>
#include <d3d9.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <atomic>
#include <vector>
#include <string>
#include <unordered_set>
#include <algorithm>

// From bof4_hooks.cpp: names the DAT backing the current scene (atlas label).
void bof4_scene_dat(char* out, size_t cap);
// From rect_hook.cpp: how many caller frames feed the draw-site signature.
extern int g_rect_sig_depth;

// Packet-write pointer: shared with rect_hook.cpp, which DERIVES the real
// address from the resolved submit site at install (this module rides that
// same hook, installed first). extern so both use the one derived value.
extern uintptr_t PKT_WRITE_PTR;

// ── input intents (written by kb thread, consumed on render thread) ──────────
static std::atomic<bool> g_active{false};
// LATCH: when true, on_present stops rebuilding g_current so a transient graphic's
// rects stay selectable/tunable after the game stops submitting them.
static std::atomic<bool> g_latched{false};
static bool g_latch_prev = false;   // render-thread edge tracker
// HUD panel position while active: 0 = top, 1 = bottom. F10 cycles
// OFF -> TOP -> BOTTOM -> OFF.
static std::atomic<int>  g_hud_pos{0};
static std::atomic<int>  in_cycle{0};
static std::atomic<int>  in_dx{0}, in_dy{0}, in_dw{0}, in_dh{0}, in_du{0}, in_dv{0};
static std::atomic<bool> in_save{false}, in_save_broad{false}, in_clear{false};

// BOX MODE. The tuner is two editors sharing one panel and one set of keys:
//   RECT mode — the original: rewrites a single GP0 packet.
//   BOX  mode — drives box_autofit: rewrites the BOX DRAWER's width/x/y args.
// They are not interchangeable. A menu box is a 9-slice (border tiles + fill +
// the semi-transparent dim) plus a separately-drawn selection cursor; editing
// one packet resizes one tile and leaves the rest behind, and the cursor drawer
// never produces a packet the rect tuner sees. Box mode moves the same lever a
// `caller=0x... width=N` line in rect_widths.txt moves, so the engine retiles
// the whole box itself — and it saves back into exactly that file.
static std::atomic<bool> g_box_mode{false};
static std::atomic<int>  in_box_toggle{0};

// ── render-thread-only state ─────────────────────────────────────────────────
// Identity = ORIGINAL on-screen position + atlas + DRAW-SITE SIGNATURE. Position
// is rock-stable frame to frame (the game submits each rect at the same x/y on a
// settled menu, and our tuning edits the packet AFTER we decode, so it never feeds
// back). The signature (caller_sig in rect_hook.cpp, now a CALL-validated shallow
// chain) says WHICH menu/box drew it — the discriminator position alone can't
// give. Position kept selection granular (grab one corner tile); the sig stops a
// save leaking to a DIFFERENT menu that happens to draw the identical tile at the
// identical spot. Together: deterministic cycling + per-draw-site save scope.
struct Ident { int x, y, u, v, clut, ow; unsigned sig; };
static inline bool ident_eq(const Ident& a, const TunerRect& r) {
    return a.x == r.x && a.y == r.y && a.u == (int)r.u && a.v == (int)r.v &&
           a.clut == (int)r.clut && a.ow == r.w && a.sig == r.sig;
}
static inline uint64_t ident_key(const TunerRect& r) {
    uint64_t k = 1469598103934665603ull;
    auto mix = [&](uint64_t x) { k ^= x; k *= 1099511628211ull; };
    mix((unsigned)(r.x & 0xFFFF)); mix((unsigned)(r.y & 0xFFFF));
    mix(r.u & 0xFF); mix(r.v & 0xFF); mix(r.clut & 0xFFFF); mix((unsigned)r.w & 0xFFFF);
    mix(r.sig);
    return k;
}
static std::vector<TunerRect>      g_building, g_current;   // distinct rects/frame
static std::unordered_set<uint64_t> g_build_seen;
static int   g_focus_index = 0;
static Ident g_focus{ -99999, -99999, -1, -1, -1, -1, 0 };
static bool  g_have_focus = false;
// transient (unsaved) reshape delta applied to the focused rect
static int   t_dx = 0, t_dy = 0, t_dw = 0, t_dh = 0, t_du = 0, t_dv = 0;
// snapshot of the focused rect this frame — feeds the HUD and the save writer.
static struct {
    bool valid;
    int  ox, oy, ow, oh, ou, ov, clut; unsigned caller0, sig, cmd;  // originals
    int  fx, fy, fw, fh, fu, fv;                                    // final written
} g_snap = {};
static unsigned g_flash = 0;   // frame counter for the flash pulse

// ── SEH-only packet helpers (no C++ objects alongside __try) ─────────────────
// Rect packet layout depends on the GP0 command: textured (0x64-0x67) has a
// texcoord word so SIZE is at +0x10; flat (0x60-0x63) has none so SIZE is at
// +0x0C. Both read cmd from the live packet (+0x07) and branch accordingly.
struct PktVals { int x, y, w, h, u, v; };
static bool pkt_read(PktVals& o) {
    uint8_t* pkt = nullptr;
    __try { pkt = *(uint8_t**)PKT_WRITE_PTR; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!pkt) return false;
    __try {
        bool tex = (pkt[0x07] & 0x04) != 0;
        unsigned pos = *(uint32_t*)(pkt + 0x08);
        unsigned sz  = tex ? *(uint32_t*)(pkt + 0x10) : *(uint32_t*)(pkt + 0x0C);
        unsigned tx  = tex ? *(uint32_t*)(pkt + 0x0C) : 0;
        o.x = (int)(int16_t)(pos & 0xFFFF); o.y = (int)(int16_t)(pos >> 16);
        o.w = (int)(sz & 0xFFFF);           o.h = (int)(sz >> 16);
        o.u = tx & 0xFF;                    o.v = (tx >> 8) & 0xFF;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}
static void pkt_write(int x, int y, int w, int h, int u, int v) {
    uint8_t* pkt = nullptr;
    __try { pkt = *(uint8_t**)PKT_WRITE_PTR; } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!pkt) return;
    __try {
        bool tex = (pkt[0x07] & 0x04) != 0;
        *(uint16_t*)(pkt + 0x08) = (uint16_t)x;   *(uint16_t*)(pkt + 0x0A) = (uint16_t)y;
        int soff = tex ? 0x10 : 0x0C;
        *(uint16_t*)(pkt + soff)     = (uint16_t)w;
        *(uint16_t*)(pkt + soff + 2) = (uint16_t)h;
        if (tex) { *(uint8_t*)(pkt + 0x0C) = (uint8_t)u; *(uint8_t*)(pkt + 0x0D) = (uint8_t)v; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static void pkt_flash(uint8_t r, uint8_t g, uint8_t b) {
    // Colour lives at pkt+0x04..0x06 (BGR); the GP0 command byte at +0x07 stays.
    uint8_t* pkt = nullptr;
    __try { pkt = *(uint8_t**)PKT_WRITE_PTR; } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!pkt) return;
    __try {
        *(uint8_t*)(pkt + 0x04) = r; *(uint8_t*)(pkt + 0x05) = g; *(uint8_t*)(pkt + 0x06) = b;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ── per-rect (render thread), called from rect_hook.cpp h_submit ─────────────
void rect_tuner_on_submit(const TunerRect& r) {
    // Tuner OFF: do nothing — importantly, NO flash. (This is what makes the
    // toggle key truly stop the red blink; previously the last focus kept
    // flashing after exit.)
    if (!g_active.load(std::memory_order_relaxed)) return;

    // 1) Accumulate the frame's DISTINCT rects (for cycling / HUD count).
    uint64_t k = ident_key(r);
    if (g_build_seen.insert(k).second && g_building.size() < 8000)
        g_building.push_back(r);

    // In BOX mode the packet editor stands down entirely — no delta, no flash —
    // so the two editors never fight over the same frame. The rect list keeps
    // building, so switching back is instant.
    if (g_box_mode.load(std::memory_order_relaxed)) return;

    // 2) If this rect is the focused one, stack the transient delta on top of
    //    the LIVE packet (post rect_fix override) and flash it.
    if (!g_have_focus || !ident_eq(g_focus, r)) return;

    PktVals base;
    if (!pkt_read(base)) return;
    int fx = base.x + t_dx, fy = base.y + t_dy;
    int fw = base.w + t_dw, fh = base.h + t_dh;
    int fu = base.u + t_du, fv = base.v + t_dv;
    if (fw < 0) fw = 0; if (fh < 0) fh = 0;
    if (fu < 0) fu = 0; if (fu > 255) fu = 255;
    if (fv < 0) fv = 0; if (fv > 255) fv = 255;
    pkt_write(fx, fy, fw, fh, fu, fv);

    // Flash: pulse the modulation colour so the focused rect is unmistakable.
    if (((g_flash >> 3) & 1)) pkt_flash(0xFF, 0x40, 0x40);   // red tint
    else                      pkt_flash(0xFF, 0xFF, 0xFF);   // bright

    // Snapshot for the HUD + save writer (originals from r, finals from above).
    g_snap.valid = true;
    g_snap.ox = r.x; g_snap.oy = r.y; g_snap.ow = r.w; g_snap.oh = r.h;
    g_snap.ou = r.u; g_snap.ov = r.v; g_snap.clut = r.clut;
    g_snap.caller0 = r.caller0; g_snap.sig = r.sig; g_snap.cmd = r.cmd;
    g_snap.fx = fx; g_snap.fy = fy; g_snap.fw = fw; g_snap.fh = fh;
    g_snap.fu = fu; g_snap.fv = fv;
}

// ── save / clear rect_tuner.txt ──────────────────────────────────────────────
void rect_tuner_sidecar_path(char* out, size_t n) {
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char* s = strrchr(exe, '\\'); if (s) *(s + 1) = 0; else exe[0] = 0;
    snprintf(out, n, "%srect_tuner.txt", exe);
}

// True when the SAME draw-site (sig) emits the focused rect's atlas piece at
// MORE THAN ONE position this frame — e.g. a 9-slice box's four corner tiles, or
// the same label drawn twice inside one menu. Only THEN does the save need an
// mx=/my= scope to pin the one instance; the sig already isolates the menu, so a
// single-instance tune stays broad (follows the label if it moves).
static bool focus_instance_shared() {
    for (const TunerRect& t : g_current)
        if (t.sig == g_snap.sig &&
            (int)t.u == g_snap.ou && (int)t.v == g_snap.ov &&
            (int)t.clut == g_snap.clut && t.w == g_snap.ow &&
            (t.x != g_snap.ox || t.y != g_snap.oy))
            return true;
    return false;
}

// Build the u/v[/clut/ow]... match+new tokens for the focused rect's current
// tune. Width is saved absolute (w=); position relative (dx/dy) since UI labels
// are runtime-positioned; height/uv only when changed. An mx=/my= position scope
// is added only when the atlas is shared (so one box tile's tune never moves
// another tile — or another menu — that reuses the same blank atlas piece).
// broad=false: scope to this draw-site (sig= [+mx/my]) — isolated, but only
//   re-applies via that exact code path (can flash back during transitions).
// broad=true : atlas-only (u/v/clut/ow, NO sig, NO mx/my) — applies in EVERY
//   context that draws this graphic, so it survives screen changes / walking /
//   transitions. Trade-off: also affects any other menu reusing that atlas piece.
static std::string focus_line(bool broad) {
    if (!g_snap.valid) return "";
    char buf[256];
    int len;
    if (broad) {
        len = snprintf(buf, sizeof(buf), "u=%d v=%d clut=0x%04X ow=%d",
                       g_snap.ou, g_snap.ov, (unsigned)g_snap.clut, g_snap.ow);
    } else {
        // sig= is the primary scope (this menu's draw-site); u/v/clut/ow match the
        // atlas piece; mx/my pin one instance only when this site draws it twice.
        len = snprintf(buf, sizeof(buf), "sig=0x%08X u=%d v=%d clut=0x%04X ow=%d",
                       g_snap.sig, g_snap.ou, g_snap.ov, (unsigned)g_snap.clut, g_snap.ow);
        if (focus_instance_shared()) len += snprintf(buf + len, sizeof(buf) - len, " mx=%d my=%d", g_snap.ox, g_snap.oy);
    }
    if (g_snap.fw != g_snap.ow)  len += snprintf(buf + len, sizeof(buf) - len, " w=%d", g_snap.fw);
    if (g_snap.fh != g_snap.oh)  len += snprintf(buf + len, sizeof(buf) - len, " h=%d", g_snap.fh);
    if (g_snap.fx != g_snap.ox)  len += snprintf(buf + len, sizeof(buf) - len, " dx=%d", g_snap.fx - g_snap.ox);
    if (g_snap.fy != g_snap.oy)  len += snprintf(buf + len, sizeof(buf) - len, " dy=%d", g_snap.fy - g_snap.oy);
    if (g_snap.fu != g_snap.ou)  len += snprintf(buf + len, sizeof(buf) - len, " nu=%d", g_snap.fu);
    if (g_snap.fv != g_snap.ov)  len += snprintf(buf + len, sizeof(buf) - len, " nv=%d", g_snap.fv);
    return std::string(buf);
}

// True when a line's match keys equal the focused rect (so we can replace an
// earlier tune of the SAME rect instead of appending duplicates). Includes the
// mx/my position scope so two tiles sharing an atlas don't clobber each other.
static bool line_matches_focus(const char* line) {
    int u = -1, v = -1, clut = -1, ow = -2, mx = INT32_MIN, my = INT32_MIN;
    unsigned sig = 0;
    const char* p = line;
    char tok[64]; int adv;
    while (sscanf(p, "%63s%n", tok, &adv) == 1) {
        p += adv;
        char* eq = strchr(tok, '='); if (!eq) continue;
        *eq = 0; int val = (int)strtol(eq + 1, nullptr, 0);
        if      (!_stricmp(tok, "sig"))  sig = (unsigned)strtoul(eq + 1, nullptr, 0);
        else if (!_stricmp(tok, "u"))    u = val;
        else if (!_stricmp(tok, "v"))    v = val;
        else if (!_stricmp(tok, "clut")) clut = val;
        else if (!_stricmp(tok, "ow"))   ow = val;
        else if (!_stricmp(tok, "mx"))   mx = val;
        else if (!_stricmp(tok, "my"))   my = val;
    }
    // Our line carries mx/my only when this site draws the tile more than once; a
    // single-instance line has neither (INT32_MIN). sig + atlas + mx/my must all
    // agree so a re-save replaces exactly the right earlier line.
    // Legacy line from the pre-sig build (no sig token → sig==0): match on the
    // atlas alone so re-saving this piece REPLACES the old broad line (upgrading
    // it to a sig-scoped one) instead of leaving it to leak across menus.
    if (sig == 0)
        return u == g_snap.ou && v == g_snap.ov &&
               clut == g_snap.clut && ow == g_snap.ow;
    bool shared = focus_instance_shared();
    int our_mx = shared ? g_snap.ox : INT32_MIN;
    int our_my = shared ? g_snap.oy : INT32_MIN;
    return sig == g_snap.sig && u == g_snap.ou && v == g_snap.ov &&
           clut == g_snap.clut && ow == g_snap.ow && mx == our_mx && my == our_my;
}

// Rewrite rect_tuner.txt: keep every line except a prior tune of this same rect;
// then (unless clearing) append the new line with a scene-atlas comment.
static void save_or_clear_focus(bool clearing, bool broad) {
    if (!g_snap.valid) return;
    char path[MAX_PATH]; rect_tuner_sidecar_path(path, sizeof(path));

    std::vector<std::string> keep;
    FILE* f = fopen(path, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            char* p = line; while (*p == ' ' || *p == '\t') ++p;
            if (*p == '#' || *p == '\n' || *p == '\r' || *p == 0) { keep.push_back(line); continue; }
            char probe[512]; strncpy(probe, p, sizeof(probe) - 1); probe[sizeof(probe) - 1] = 0;
            if (line_matches_focus(probe)) continue;   // drop the old tune of this rect
            keep.push_back(line);
        }
        fclose(f);
    }

    f = fopen(path, "w");
    if (!f) { hook_log("[tuner] save: fopen(%s) for write failed\n", path); return; }
    if (keep.empty())
        fprintf(f, "# rect_tuner.txt - saved by the in-game Rect Tuner (F10).\n"
                   "# Universal grammar (same as rect_widths.txt); read by rect_fix/rect_tuner.\n");
    for (auto& s : keep) fputs(s.c_str(), f);
    if (!clearing) {
        std::string ln = focus_line(broad);
        if (!ln.empty()) {
            char scene[96]; bof4_scene_dat(scene, sizeof(scene));
            fprintf(f, "# %s  (w %d->%d)%s\n%s\n", scene, g_snap.ow, g_snap.fw,
                    broad ? "  [broad/persistent]" : "", ln.c_str());
        }
    }
    fclose(f);
    hook_log("[tuner] %s rect_tuner.txt (%s u=%d v=%d ow=%d -> w=%d)\n",
             clearing ? "cleared from" : "saved to", broad ? "broad" : "sig-scoped",
             g_snap.ou, g_snap.ov, g_snap.ow, g_snap.fw);
}

// From rect_hook.cpp: reload rect_widths.txt + rect_tuner.txt into the live set.
void rect_reload_overrides();

// ── HUD (self-contained; same D3D quad pattern as subtitles.cpp DrawQuad) ────
struct HudVertex { float x, y, z, rhw; D3DCOLOR color; float u, v; };
static constexpr DWORD kHudFVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;
static IDirect3DTexture9* s_hudTex = nullptr;
static int s_hudW = 0, s_hudH = 0;
static std::string s_hudCache;

static uint32_t* render_hud_argb(const char* text, int w, int h) {
    HDC screen = GetDC(nullptr); HDC dc = CreateCompatibleDC(screen); ReleaseDC(nullptr, screen);
    if (!dc) return nullptr;
    HFONT font = CreateFontA(15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        FF_DONTCARE | FIXED_PITCH, "Consolas");
    HFONT oldF = (HFONT)SelectObject(dc, font);
    BITMAPINFO bmi{}; bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w; bmi.bmiHeader.biHeight = -h; bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32; bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HBITMAP oldB = (HBITMAP)SelectObject(dc, dib);
    RECT full{ 0, 0, w, h };
    HBRUSH bg = CreateSolidBrush(RGB(16, 18, 28)); FillRect(dc, &full, bg); DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(235, 235, 245));
    RECT tr{ 8, 6, w - 8, h - 6 };
    DrawTextA(dc, text, -1, &tr, DT_LEFT | DT_TOP | DT_NOPREFIX | DT_EXPANDTABS);
    GdiFlush();
    uint32_t* out = new uint32_t[w * h];
    const uint32_t* src = (const uint32_t*)bits;
    for (int i = 0, n = w * h; i < n; ++i) { out[i] = 0xD8000000u | (src[i] & 0x00FFFFFFu); }
    SelectObject(dc, oldB); DeleteObject(dib);
    SelectObject(dc, oldF); DeleteObject(font); DeleteDC(dc);
    return out;
}

static void draw_hud(IDirect3DDevice9* dev, IDirect3DSwapChain9* sc, const char* text) {
    IDirect3DSurface9* bb = nullptr;
    if (sc) sc->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (!bb) dev->GetRenderTarget(0, &bb);
    if (!bb) return;
    D3DSURFACE_DESC d{}; if (FAILED(bb->GetDesc(&d))) { bb->Release(); return; }
    int bbW = (int)d.Width, bbH = (int)d.Height;

    const int W = 660, H = 120;   // compact: just the focus/rect info (box list removed)
    if (s_hudCache != text) {
        if (s_hudTex && (s_hudW != W || s_hudH != H)) { s_hudTex->Release(); s_hudTex = nullptr; }
        if (!s_hudTex) {
            if (FAILED(dev->CreateTexture(W, H, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8,
                    D3DPOOL_DEFAULT, &s_hudTex, nullptr)) || !s_hudTex) { bb->Release(); return; }
            s_hudW = W; s_hudH = H;
        }
        uint32_t* px = render_hud_argb(text, W, H);
        if (px) {
            D3DLOCKED_RECT lr{};
            if (SUCCEEDED(s_hudTex->LockRect(0, &lr, nullptr, D3DLOCK_DISCARD))) {
                for (int y = 0; y < H; ++y) memcpy((uint8_t*)lr.pBits + y * lr.Pitch, px + y * W, W * 4);
                s_hudTex->UnlockRect(0);
            }
            delete[] px;
        }
        s_hudCache = text;
    }
    if (!s_hudTex) { bb->Release(); return; }

    // Position: TOP (12px from top) or BOTTOM (12px above the bottom edge).
    const float x0 = 12.0f;
    const float y0 = (g_hud_pos.load(std::memory_order_relaxed) == 1)
                     ? (float)(bbH - H - 12) : 12.0f;
    const float x1 = x0 + W, y1 = y0 + H, o = -0.5f;
    HudVertex vtx[4] = {
        { x0 + o, y0 + o, 0, 1, 0xFFFFFFFF, 0, 0 },
        { x1 + o, y0 + o, 0, 1, 0xFFFFFFFF, 1, 0 },
        { x0 + o, y1 + o, 0, 1, 0xFFFFFFFF, 0, 1 },
        { x1 + o, y1 + o, 0, 1, 0xFFFFFFFF, 1, 1 },
    };
    IDirect3DStateBlock9* sb = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb)) || !sb) { bb->Release(); return; }
    dev->SetRenderTarget(0, bb); dev->SetDepthStencilSurface(nullptr);
    D3DVIEWPORT9 vp{ 0, 0, (DWORD)bbW, (DWORD)bbH, 0.0f, 1.0f }; dev->SetViewport(&vp);
    RECT scissor{ 0, 0, bbW, bbH }; dev->SetScissorRect(&scissor);
    dev->SetPixelShader(nullptr); dev->SetVertexShader(nullptr); dev->SetFVF(kHudFVF);
    dev->SetStreamSource(0, nullptr, 0, 0); dev->SetIndices(nullptr); dev->SetTexture(0, s_hudTex);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE); dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE); dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE); dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE); dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF); dev->SetRenderState(D3DRS_CLIPPING, TRUE);
    dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vtx, sizeof(HudVertex));
    sb->Apply(); sb->Release();
    bb->Release();
}

// Release the D3DPOOL_DEFAULT HUD texture before a device Reset (else Reset
// fails). Called from hook_Reset; the texture is lazily recreated next draw.
void rect_tuner_on_lost_device() {
    if (s_hudTex) { s_hudTex->Release(); s_hudTex = nullptr; }
    s_hudW = s_hudH = 0; s_hudCache.clear();
}

// ── per-frame (render thread), called from the Present hooks ─────────────────
void rect_tuner_set_latch(bool on) {
    g_latched.store(on, std::memory_order_relaxed);
    if (on) g_active.store(true, std::memory_order_relaxed);   // auto-show the tuner
}
bool rect_tuner_is_latched() { return g_latched.load(std::memory_order_relaxed); }

// ── overlay highlight boxes (render thread) ──────────────────────────────────
// When latched, the game no longer re-submits the focused rect, so the packet
// FLASH can't mark it. Instead we draw our OWN outline in backbuffer space. The
// game's UI is authored in a ~320x240 space (config tuner_psx_w/h) scaled to fill
// the backbuffer; map with that so the box lands on the frozen real graphic.
struct BoxVtx { float x, y, z, rhw; D3DCOLOR color; };
static constexpr DWORD kBoxFVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;

static void fill_screen_rect(IDirect3DDevice9* dev, float x, float y, float w, float h, D3DCOLOR c) {
    BoxVtx v[4] = {
        { x,     y,     0, 1, c }, { x + w, y,     0, 1, c },
        { x,     y + h, 0, 1, c }, { x + w, y + h, 0, 1, c },
    };
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(BoxVtx));
}

// Draw an outline (4 edges) for a PSX-space rect, mapped to the backbuffer.
static void draw_psx_outline(IDirect3DDevice9* dev, int bbW, int bbH,
                             int px, int py, int pw, int ph, D3DCOLOR c, float thick) {
    int psxw = g_cfg.tuner_psx_w > 0 ? g_cfg.tuner_psx_w : 320;
    int psxh = g_cfg.tuner_psx_h > 0 ? g_cfg.tuner_psx_h : 240;
    float sx = (float)bbW / (float)psxw, sy = (float)bbH / (float)psxh;
    float x = px * sx, y = py * sy, w = pw * sx, h = ph * sy;
    fill_screen_rect(dev, x,           y,            w,     thick, c); // top
    fill_screen_rect(dev, x,           y + h - thick,w,     thick, c); // bottom
    fill_screen_rect(dev, x,           y,            thick, h,     c); // left
    fill_screen_rect(dev, x + w - thick,y,           thick, h,     c); // right
}

// Fill a PSX-space rect (mapped to the backbuffer) — the caliper's building block.
static void fill_psx_rect(IDirect3DDevice9* dev, int bbW, int bbH,
                          int px, int py, int pw, int ph, D3DCOLOR c) {
    int psxw = g_cfg.tuner_psx_w > 0 ? g_cfg.tuner_psx_w : 320;
    int psxh = g_cfg.tuner_psx_h > 0 ? g_cfg.tuner_psx_h : 240;
    float sx = (float)bbW / (float)psxw, sy = (float)bbH / (float)psxh;
    fill_screen_rect(dev, px * sx, py * sy, pw * sx, ph * sy, c);
}

static void draw_focus_boxes(IDirect3DDevice9* dev, IDirect3DSwapChain9* sc) {
    const bool boxmode = g_box_mode.load(std::memory_order_relaxed);
    int bx = 0, by = 0, bw = 0, bow = 0;
    if (boxmode) { if (!box_tuner_focus_geom(&bx, &by, &bw, &bow)) return; }
    else if (!g_snap.valid) return;
    IDirect3DSurface9* bb = nullptr;
    if (sc) sc->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (!bb) dev->GetRenderTarget(0, &bb);
    if (!bb) return;
    D3DSURFACE_DESC d{}; if (FAILED(bb->GetDesc(&d))) { bb->Release(); return; }
    int bbW = (int)d.Width, bbH = (int)d.Height;

    IDirect3DStateBlock9* sb = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb)) || !sb) { bb->Release(); return; }
    dev->SetRenderTarget(0, bb); dev->SetDepthStencilSurface(nullptr);
    D3DVIEWPORT9 vp{ 0, 0, (DWORD)bbW, (DWORD)bbH, 0.0f, 1.0f }; dev->SetViewport(&vp);
    dev->SetPixelShader(nullptr); dev->SetVertexShader(nullptr); dev->SetFVF(kBoxFVF);
    dev->SetStreamSource(0, nullptr, 0, 0); dev->SetIndices(nullptr); dev->SetTexture(0, nullptr);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE); dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE); dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE); dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE); dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF); dev->SetRenderState(D3DRS_CLIPPING, TRUE);
    dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

    const float o = (g_flash & 4) ? 3.0f : 2.0f;   // gentle pulse
    if (boxmode) {
        // A CALIPER along the box's top edge, not an outline: the drawer hands us
        // x / y / width but never a height, and drawing a guessed rectangle would
        // be a lie about where the box ends. The span is the thing being edited
        // anyway. Dim cyan = the stock width the game pushed; bright yellow = the
        // width the drawer is being handed right now, with end ticks.
        D3DCOLOR live = (g_flash & 4) ? 0xFFFFF000u : 0xFFFFFF80u;
        fill_psx_rect(dev, bbW, bbH, bx,          by - 4, bow, 1, 0x8000E0FFu);
        fill_psx_rect(dev, bbW, bbH, bx,          by - 2, bw,  2, live);
        fill_psx_rect(dev, bbW, bbH, bx,          by - 2, 1,   10, live);
        fill_psx_rect(dev, bbW, bbH, bx + bw - 1, by - 2, 1,   10, live);
    } else {
        // Original position (dim cyan) then the edit target (bright yellow) on top.
        draw_psx_outline(dev, bbW, bbH, g_snap.ox, g_snap.oy, g_snap.ow, g_snap.oh, 0x8000E0FFu, 2.0f);
        draw_psx_outline(dev, bbW, bbH, g_snap.fx, g_snap.fy, g_snap.fw, g_snap.fh, 0xFFFFF000u, o);
    }

    sb->Apply(); sb->Release();
    bb->Release();
}

void rect_tuner_on_present(IDirect3DDevice9* dev, IDirect3DSwapChain9* sc) {
    // Snapshot this frame's box draws for box mode. Unconditional: the list has
    // to be one frame old and complete the instant you switch modes.
    box_autofit_on_present();

    bool latched = g_latched.load(std::memory_order_relaxed);
    // On the frame we FIRST latch, do one last swap so g_current captures the most
    // freshly-built frame (the transient graphic). While latched, DON'T rebuild —
    // keep that captured list selectable even though the game stops submitting.
    if (!latched) {
        g_current.swap(g_building);
        g_building.clear(); g_build_seen.clear();
    } else if (!g_latch_prev) {
        if (!g_building.empty()) g_current.swap(g_building);   // grab freshest frame
        g_building.clear(); g_build_seen.clear();
    } else {
        g_building.clear(); g_build_seen.clear();              // drain, keep g_current
    }
    g_latch_prev = latched;
    g_flash++;

    if (!g_active.load(std::memory_order_relaxed)) return;

    // ── BOX MODE ────────────────────────────────────────────────────────────
    // Toggling is consumed here (render thread) because every box_tuner_* call
    // must run on the same thread as af_box_hook — that is what lets the tune
    // state be plain ints instead of another set of locks.
    if (in_box_toggle.exchange(0, std::memory_order_relaxed)) {
        bool on = !box_tuner_active();
        box_tuner_set_active(on);
        g_box_mode.store(on, std::memory_order_relaxed);
        // Drop anything the other editor had queued so a mode switch is clean.
        in_cycle = in_dx = in_dy = in_dw = in_dh = in_du = in_dv = 0;
        in_save = in_save_broad = in_clear = false;
    }
    if (g_box_mode.load(std::memory_order_relaxed)) {
        // Same physical keys, box meanings. A box has exactly ONE size — its
        // width — so resize_mod (Shift) means the same as no modifier here
        // instead of being repurposed. Holding a modifier must never silently
        // multiply your step: uv_mod is the ONE coarse control, and it says so.
        //   move_left/right          -> width -1 / +1    (plain, and with Shift)
        //   uv_mod   + left/right    -> width -10 / +10  (the only x10)
        //   move_up/down             -> box y -1 / +1    (plain, and with Shift)
        //   uv_vert_mod + wheel      -> box x -1 / +1
        // The wheel rides move_left/right, so it scrubs width at 1px a notch.
        int c = in_cycle.exchange(0, std::memory_order_relaxed);
        if (c) box_tuner_cycle(c);
        int dw = in_dx.exchange(0, std::memory_order_relaxed)
               + in_dw.exchange(0, std::memory_order_relaxed)          // Shift: same, 1px
               + 10 * in_du.exchange(0, std::memory_order_relaxed);    // Ctrl: coarse
        int ny = in_dy.exchange(0, std::memory_order_relaxed)
               + in_dh.exchange(0, std::memory_order_relaxed);
        int nx = in_dv.exchange(0, std::memory_order_relaxed);
        if (dw || nx || ny) box_tuner_nudge(dw, nx, ny);
        // Both save flavours mean the same thing for a box: one caller-keyed rule.
        if (in_save.exchange(false, std::memory_order_relaxed) |
            in_save_broad.exchange(false, std::memory_order_relaxed)) box_tuner_save();
        if (in_clear.exchange(false, std::memory_order_relaxed)) box_tuner_clear();

        draw_focus_boxes(dev, sc);
        char btext[2048];
        int n = box_tuner_hud_text(btext, (int)sizeof(btext));
        if (n <= 0) snprintf(btext, sizeof(btext), "BOX TUNER   (no boxes seen)");
        draw_hud(dev, sc, btext);
        return;
    }

    // Sort into a STABLE spatial order (top-to-bottom, left-to-right) so cycling
    // is deterministic — +/- steps to a predictable neighbour and returns to the
    // same rect. (Positions are stable, so this order is identical every frame.)
    std::sort(g_current.begin(), g_current.end(), [](const TunerRect& a, const TunerRect& b) {
        if (a.y != b.y) return a.y < b.y;
        if (a.x != b.x) return a.x < b.x;
        if (a.u != b.u) return a.u < b.u;
        if (a.v != b.v) return a.v < b.v;
        if (a.w != b.w) return a.w < b.w;
        return a.sig < b.sig;
    });
    // Keep the focus index pinned to the SAME rect across frames (its position in
    // the sorted list can shift as rects come/go), by finding it by identity.
    if (g_have_focus) {
        for (int i = 0; i < (int)g_current.size(); i++)
            if (ident_eq(g_focus, g_current[i])) { g_focus_index = i; break; }
    }

    // Consume input intents (posted by the kb thread).
    int cyc = in_cycle.exchange(0, std::memory_order_relaxed);
    if (cyc && !g_current.empty()) {
        int n = (int)g_current.size();
        g_focus_index = ((g_focus_index + cyc) % n + n) % n;
        const TunerRect& fr = g_current[g_focus_index];
        g_focus = { fr.x, fr.y, (int)fr.u, (int)fr.v, (int)fr.clut, fr.w, fr.sig };
        g_have_focus = true;
        t_dx = t_dy = t_dw = t_dh = t_du = t_dv = 0;   // fresh rect: reset delta
    } else if (!g_have_focus && !g_current.empty()) {
        g_focus_index = 0; const TunerRect& fr = g_current[0];
        g_focus = { fr.x, fr.y, (int)fr.u, (int)fr.v, (int)fr.clut, fr.w, fr.sig };
        g_have_focus = true;
    }
    t_dx += in_dx.exchange(0, std::memory_order_relaxed);
    t_dy += in_dy.exchange(0, std::memory_order_relaxed);
    t_dw += in_dw.exchange(0, std::memory_order_relaxed);
    t_dh += in_dh.exchange(0, std::memory_order_relaxed);
    t_du += in_du.exchange(0, std::memory_order_relaxed);
    t_dv += in_dv.exchange(0, std::memory_order_relaxed);
    if (in_save.exchange(false, std::memory_order_relaxed)) {
        save_or_clear_focus(false, /*broad=*/false);
        rect_reload_overrides();
        t_dx = t_dy = t_dw = t_dh = t_du = t_dv = 0;   // persistent override now owns it
    }
    if (in_save_broad.exchange(false, std::memory_order_relaxed)) {
        save_or_clear_focus(false, /*broad=*/true);    // atlas-only: survives transitions
        rect_reload_overrides();
        t_dx = t_dy = t_dw = t_dh = t_du = t_dv = 0;
    }
    if (in_clear.exchange(false, std::memory_order_relaxed)) {
        save_or_clear_focus(true, /*broad=*/false);
        rect_reload_overrides();
        t_dx = t_dy = t_dw = t_dh = t_du = t_dv = 0;
    }

    // When LATCHED the game no longer re-submits the focused rect, so on_submit
    // won't refresh g_snap — synthesize it here from the focused captured rect +
    // the transient deltas, so the HUD, the save writer, and the overlay boxes all
    // reflect the current edit.
    if (latched && g_have_focus && g_focus_index >= 0 &&
        g_focus_index < (int)g_current.size()) {
        const TunerRect& fr = g_current[g_focus_index];
        int fx = fr.x + t_dx, fy = fr.y + t_dy, fw = (int)fr.w + t_dw, fh = (int)fr.h + t_dh;
        int fu = (int)fr.u + t_du, fv = (int)fr.v + t_dv;
        if (fw < 0) fw = 0; if (fh < 0) fh = 0;
        if (fu < 0) fu = 0; if (fu > 255) fu = 255;
        if (fv < 0) fv = 0; if (fv > 255) fv = 255;
        g_snap.valid = true;
        g_snap.ox = fr.x; g_snap.oy = fr.y; g_snap.ow = fr.w; g_snap.oh = fr.h;
        g_snap.ou = fr.u; g_snap.ov = fr.v; g_snap.clut = fr.clut;
        g_snap.caller0 = fr.caller0; g_snap.sig = fr.sig; g_snap.cmd = fr.cmd;
        g_snap.fx = fx; g_snap.fy = fy; g_snap.fw = fw; g_snap.fh = fh;
        g_snap.fu = fu; g_snap.fv = fv;
    }

    // Overlay highlight boxes — draw the focused rect's outline ourselves (the
    // packet flash can't when the game isn't re-submitting). Cheap; runs always so
    // it also helps locate rects live, but it's the ONLY marker while latched.
    draw_focus_boxes(dev, sc);

    // HUD text.
    char scene[96]; bof4_scene_dat(scene, sizeof(scene));
    char text[4096];
    if (g_have_focus && g_snap.valid) {
        // Source attribution: which DAT recNN's texture this rect samples (v-band
        // candidates from the VRAM->rec map; see tex_prov.cpp). Empty until the
        // map + uploads are present.
        char src[256] = {0};
        bool have_src = tex_prov_resolve(g_snap.ov, src, sizeof(src));
        snprintf(text, sizeof(text),
            "RECT TUNER%s   focus %d/%d   scene: %s\n"
            "cmd=0x%02X @(%d,%d)  u=%d v=%d clut=0x%04X ow=%d oh=%d  sig=%08X\n"
            "src (v=%d): %s\n"
            "now    w=%d h=%d   dx=%d dy=%d   nu=%d nv=%d\n"
            "wheel: plain=move  Ctrl=U/V left-right  Alt=U/V up-down  (Shift+key=resize)\n"
            "cycle move/+resize/+uv  save (uv_mod+save=broad) clear   box_mode=MENU BOXES",
            latched ? " [LATCHED]" : "",
            g_focus_index + 1, (int)g_current.size(), scene,
            g_snap.cmd, g_snap.ox, g_snap.oy, g_snap.ou, g_snap.ov, (unsigned)g_snap.clut, g_snap.ow, g_snap.oh, g_snap.sig,
            g_snap.ov, have_src ? src : "(no VRAM match — check vram_rec_map.tsv)",
            g_snap.fw, g_snap.fh, g_snap.fx - g_snap.ox, g_snap.fy - g_snap.oy,
            g_snap.fu, g_snap.fv);
    } else {
        snprintf(text, sizeof(text),
            "RECT TUNER   (no rect focused yet)   scene: %s\n"
            "%d rects on screen — press next/prev to focus one (it flashes)\n"
            "move / +resize_mod / +uv_mod = shape the focused rect\n"
            "box_mode key = switch to MENU BOX editing  (rebind in rect_tuner_keys.txt)",
            scene, (int)g_current.size());
    }
    // (Box-list section removed — the panel now shows only the focused-rect info.)
    draw_hud(dev, sc, text);
}

// ── configurable key bindings (rect_tuner_keys.txt next to BOF4.exe) ─────────
// GetAsyncKeyState polling is the driver (the proven path — same as rect_hook's
// working F5-F8 hotkeys). NOTE: we deliberately do NOT install a key-swallow
// hook — swallowing the keys with WH_KEYBOARD_LL also stopped GetAsyncKeyState
// from seeing them (that was why +/-/arrows did nothing). Trade-off: the keys
// also reach the game, so bind the movement keys to something the game ignores
// (numpad works well) via rect_tuner_keys.txt.
// NOTE: field order here IS the index order kTunerActions exposes to the config
// overlay ((&g_keys.toggle)[i]). Append new bindings at the END, never insert.
struct TunerKeys {
    int toggle, next, prev, mleft, mright, mup, mdown,
        resize_mod, uv_mod, uv_vert_mod, vmove_mod, save, clear, exit,
        box_mode;
};
static TunerKeys g_keys = {
    VK_F10,                 // toggle
    VK_OEM_PLUS, VK_OEM_MINUS,       // next / prev  (the "=+" and "-_" keys)
    'F', 'H', 'T', 'G',     // move: F=left H=right T=up G=down — same physical
                            // spot on QWERTZ & QWERTY; 1px per press (see poll)
    VK_LSHIFT, VK_CONTROL, VK_MENU, VK_OEM_102,  // resize(LSHIFT) / uv(CTRL, U/V L-R) /
                            // uv_vert(ALT, U/V up-down w/wheel) / vmove(< key = wheel up-down)
    VK_RSHIFT, VK_BACK, VK_ESCAPE,       // save (RIGHT SHIFT — ENTER opens menus) / clear / exit
    'B'                     // box_mode: switch the panel between RECT and BOX
                            // editing. The F keys were full and the game claims
                            // several of them, so this rides a plain letter.
                            // NOT 'V' — V is the game's ABORT. Tuner keys are
                            // deliberately not swallowed, so every binding also
                            // reaches the game; rebind to a numpad key if B
                            // turns out to be taken too.
};

// Actions the mouse wheel can be bound to (wheel isn't a key, so it can't go in
// the VK table — it's captured by a low-level mouse hook instead).
enum TunerAction { ACT_NONE, ACT_MOVE_L, ACT_MOVE_R, ACT_MOVE_U, ACT_MOVE_D, ACT_NEXT, ACT_PREV };
static int g_wheel_up   = ACT_MOVE_L;   // wheel forward  -> move left  (default)
static int g_wheel_down = ACT_MOVE_R;   // wheel backward -> move right (default)
static int action_from_token(const char* t) {
    char u[24]; int i = 0; for (; t[i] && i < 23; ++i) u[i] = (char)tolower((unsigned char)t[i]); u[i] = 0;
    if (!strcmp(u, "move_left"))  return ACT_MOVE_L;
    if (!strcmp(u, "move_right")) return ACT_MOVE_R;
    if (!strcmp(u, "move_up"))    return ACT_MOVE_U;
    if (!strcmp(u, "move_down"))  return ACT_MOVE_D;
    if (!strcmp(u, "next"))       return ACT_NEXT;
    if (!strcmp(u, "prev"))       return ACT_PREV;
    if (!strcmp(u, "none") || !strcmp(u, "off")) return ACT_NONE;
    return -1;   // unknown -> leave default
}

// token -> virtual-key. Accepts A-Z, 0-9, F1-F12, NUMPAD0-9, NUM+/NUM-, arrows,
// ENTER/ESC/BACKSPACE/SPACE/TAB/SHIFT/CTRL/ALT/INSERT/HOME/END/PAGEUP/PAGEDOWN,
// the OEM +/- keys, or a raw 0xNN / decimal code.
static int vk_from_token(const char* t) {
    char u[32]; int i = 0; for (; t[i] && i < 31; ++i) u[i] = (char)toupper((unsigned char)t[i]); u[i] = 0;
    if (u[0] == 0) return 0;
    if (u[1] == 0) { char c = u[0];
        if (c >= 'A' && c <= 'Z') return c;            // VK_A..VK_Z == 'A'..'Z'
        if (c >= '0' && c <= '9') return c;            // VK_0..VK_9 == '0'..'9'
        if (c == '<' || c == '>') return VK_OEM_102;   // the ISO <> key (QWERTZ)
    }
    if (u[0] == 'F' && (u[1] >= '1' && u[1] <= '9')) { int n = atoi(u + 1); if (n >= 1 && n <= 12) return VK_F1 + (n - 1); }
    if (!strncmp(u, "NUMPAD", 6) && u[6] >= '0' && u[6] <= '9') return VK_NUMPAD0 + (u[6] - '0');
    struct { const char* n; int v; } M[] = {
        {"LEFT",VK_LEFT},{"RIGHT",VK_RIGHT},{"UP",VK_UP},{"DOWN",VK_DOWN},
        {"ENTER",VK_RETURN},{"RETURN",VK_RETURN},{"ESC",VK_ESCAPE},{"ESCAPE",VK_ESCAPE},
        {"BACKSPACE",VK_BACK},{"BACK",VK_BACK},{"SPACE",VK_SPACE},{"TAB",VK_TAB},
        {"SHIFT",VK_SHIFT},{"CTRL",VK_CONTROL},{"CONTROL",VK_CONTROL},{"ALT",VK_MENU},
        {"LSHIFT",VK_LSHIFT},{"RSHIFT",VK_RSHIFT},{"LCTRL",VK_LCONTROL},{"RCTRL",VK_RCONTROL},
        {"INSERT",VK_INSERT},{"INS",VK_INSERT},{"DELETE",VK_DELETE},{"DEL",VK_DELETE},
        {"HOME",VK_HOME},{"END",VK_END},{"PAGEUP",VK_PRIOR},{"PGUP",VK_PRIOR},
        {"PAGEDOWN",VK_NEXT},{"PGDN",VK_NEXT},
        {"NUMADD",VK_ADD},{"NUMSUB",VK_SUBTRACT},{"NUMMUL",VK_MULTIPLY},{"NUMDIV",VK_DIVIDE},
        {"PLUS",VK_OEM_PLUS},{"MINUS",VK_OEM_MINUS},{"OEM_PLUS",VK_OEM_PLUS},{"OEM_MINUS",VK_OEM_MINUS},
        {"LBRACKET",VK_OEM_4},{"RBRACKET",VK_OEM_6},
        {"OEM_102",VK_OEM_102},{"LT",VK_OEM_102},{"ANGLE",VK_OEM_102},{"LESS",VK_OEM_102},
    };
    for (auto& m : M) if (!strcmp(u, m.n)) return m.v;
    return (int)strtol(u, nullptr, 0);                 // 0xNN / decimal
}

static void keys_sidecar_path(char* out, size_t n) {
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char* s = strrchr(exe, '\\'); if (s) *(s + 1) = 0; else exe[0] = 0;
    snprintf(out, n, "%srect_tuner_keys.txt", exe);
}
static void write_keys_template(const char* path) {
    FILE* f = fopen(path, "w"); if (!f) return;
    fprintf(f,
        "# rect_tuner_keys.txt - key bindings for the in-game Rect Tuner.\n"
        "# One binding per line: action = KEY. KEY may be a name (F10, LEFT, ENTER,\n"
        "# NUMPAD8, A, 5, PLUS, MINUS, INSERT, ...) or a raw code (0x79).\n"
        "# NOTE: tuner keys ALSO reach the game (no key-swallow). If the movement\n"
        "# keys move the game menu too, bind them to the numpad (game ignores it).\n"
        "#\n"
        "toggle     = F10        # enter/exit tuner\n"
        "next       = PLUS       # cycle to next rect ( = / + key )\n"
        "prev       = MINUS      # cycle to previous rect ( - key )\n"
        "move_left  = F          # F/H/T/G — same physical keys on QWERTZ & QWERTY.\n"
        "move_right = H          # Tap = 1px (precise); HOLD >~1s = auto-repeat for\n"
        "move_up    = T          # big shifts. The mouse wheel also scrubs fast.\n"
        "move_down  = G\n"
        "resize_mod = LSHIFT     # hold (LEFT SHIFT) with move keys to RESIZE (w/h)\n"
        "uv_mod     = CTRL       # hold with move keys / wheel to shift atlas U/V (left-right)\n"
        "uv_vert_mod= ALT        # hold with the WHEEL to shift atlas U/V up-down (v--/v++).\n"
        "vmove_mod  = OEM_102    # hold the < key (the <> / OEM_102 key) with the WHEEL to\n"
        "                        # move the rect UP/DOWN. Wheel alone still moves left/right.\n"
        "                        # Arrows/numpad are hijacked by the game, so ALT+wheel is\n"
        "                        # the way to move UV vertically; CTRL+wheel does horizontal.\n"
        "save       = RSHIFT     # RIGHT SHIFT — sig-scoped save (isolated to this menu).\n"
        "                        # (ENTER is avoided: it also confirms game menus.) Hold\n"
        "                        # uv_mod + save = BROAD save (atlas-only, persists\n"
        "                        # through screen transitions / walking).\n"
        "clear      = BACKSPACE\n"
        "exit       = ESC\n"
        "#\n"
        "# BOX MODE — press this while the tuner is open to switch the panel between\n"
        "# RECT editing (one GP0 packet) and BOX editing. They are different tools:\n"
        "# a menu box is a 9-slice (border tiles + fill + the semi-transparent dim)\n"
        "# plus a separately-drawn selection cursor, so packet editing resizes one\n"
        "# tile and leaves the rest behind — and the cursor drawer never even shows\n"
        "# up in the rect list. BOX mode rewrites the box DRAWER's width/x/y args,\n"
        "# the same lever a `caller=0x.. width=N` line in rect_widths.txt pulls, so\n"
        "# the engine retiles the whole box itself. Save writes that line back into\n"
        "# rect_widths.txt (in place, keeping your comments); clear comments it out.\n"
        "# In box mode a box has exactly ONE size (its width), so:\n"
        "#   move_left/right (F/H) or the WHEEL = width -/+ 1px. One notch, one\n"
        "#     pixel: the wheel is capped to a single step per event here, so a\n"
        "#     mouse driver that accelerates the wheel can't jump you 10px.\n"
        "#   uv_mod (Ctrl) + F/H = width -/+ 10px. This is the ONLY x10 control.\n"
        "#   resize_mod (Shift) does the SAME as no modifier - it never silently\n"
        "#     multiplies your step.\n"
        "#   move_up/down (T/G) = box y      uv_vert_mod (Alt) + wheel = box x\n"
        "#   next/prev = pick a box          save = write the rule to rect_widths\n"
        "box_mode   = B\n"
        "#\n"
        "# Mouse wheel -> an ACTION (not a key). Value is one of: move_left,\n"
        "# move_right, move_up, move_down, next, prev, none. Held resize_mod/uv_mod\n"
        "# apply, so wheel = resize/uv too. Special case: holding uv_vert_mod (ALT)\n"
        "# overrides the wheel action to shift atlas U/V vertically (v--/v++). The\n"
        "# wheel is swallowed while the tuner is active so the game doesn't scroll.\n"
        "wheel_up   = move_left\n"
        "wheel_down = move_right\n"
        "#\n"
        "# sig_depth = how many (CALL-validated) caller frames identify a rect's\n"
        "# draw-site (1-12). 0x502070 runs in a loop, so frames DEEPER than this\n"
        "# rect's real chain are stale leftovers that make focus/count wobble —\n"
        "# keep this SHALLOW. 4 is a good default. RAISE by 1 only if two different\n"
        "# menus still collide on a save; LOWER if focus flickers on some screens.\n"
        "sig_depth  = 4\n"
        "#\n"
        "# Example conflict-free movement on the numpad:\n"
        "#   move_left = NUMPAD4   move_right = NUMPAD6\n"
        "#   move_up   = NUMPAD8   move_down  = NUMPAD2\n");
    fclose(f);
}
static void load_keys() {
    char path[MAX_PATH]; keys_sidecar_path(path, sizeof(path));
    FILE* f = fopen(path, "r");
    if (!f) { write_keys_template(path);
        hook_log("[tuner] wrote default rect_tuner_keys.txt (edit + restart to rebind)\n"); return; }
    std::unordered_set<std::string> seen;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* p = line; while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == '\n' || *p == '\r' || !*p) continue;
        char key[32], val[32];
        if (sscanf(p, " %31[^= \t] = %31s", key, val) != 2) continue;
        for (char* c = key; *c; ++c) *c = (char)tolower((unsigned char)*c);
        seen.insert(key);
        // Wheel bindings map to an ACTION name, not a virtual-key.
        if (!strcmp(key, "wheel_up") || !strcmp(key, "wheel_down")) {
            int a = action_from_token(val);
            if (a >= 0) { if (key[6] == 'u') g_wheel_up = a; else g_wheel_down = a; }
            continue;
        }
        // Draw-site signature depth (isolation vs stability). Not a key/action.
        if (!strcmp(key, "sig_depth")) {
            int d = (int)strtol(val, nullptr, 0);
            if (d >= 1 && d <= 12) g_rect_sig_depth = d;
            continue;
        }
        int vk = vk_from_token(val);
        if (vk <= 0) continue;
        if      (!strcmp(key, "toggle"))     g_keys.toggle = vk;
        else if (!strcmp(key, "next"))       g_keys.next = vk;
        else if (!strcmp(key, "prev"))       g_keys.prev = vk;
        else if (!strcmp(key, "move_left"))  g_keys.mleft = vk;
        else if (!strcmp(key, "move_right")) g_keys.mright = vk;
        else if (!strcmp(key, "move_up"))    g_keys.mup = vk;
        else if (!strcmp(key, "move_down"))  g_keys.mdown = vk;
        else if (!strcmp(key, "resize_mod")) g_keys.resize_mod = vk;
        else if (!strcmp(key, "uv_mod"))     g_keys.uv_mod = vk;
        else if (!strcmp(key, "uv_vert_mod")) g_keys.uv_vert_mod = vk;
        else if (!strcmp(key, "vmove_mod"))  g_keys.vmove_mod = vk;
        else if (!strcmp(key, "save"))       g_keys.save = vk;
        else if (!strcmp(key, "clear"))      g_keys.clear = vk;
        else if (!strcmp(key, "exit"))       g_keys.exit = vk;
        else if (!strcmp(key, "box_mode"))   g_keys.box_mode = vk;
    }
    fclose(f);

    // Self-heal: append any recognized options this (older) file is missing, so
    // new settings added in a later build show up without deleting the file.
    static const struct { const char* name; const char* def; } EXPECT[] = {
        {"toggle","F10"}, {"next","PLUS"}, {"prev","MINUS"},
        {"move_left","F"}, {"move_right","H"}, {"move_up","T"}, {"move_down","G"},
        {"resize_mod","LSHIFT"}, {"uv_mod","CTRL"}, {"uv_vert_mod","ALT"}, {"vmove_mod","OEM_102"},
        {"save","RSHIFT"}, {"clear","BACKSPACE"}, {"exit","ESC"}, {"box_mode","B"},
        {"wheel_up","move_left"}, {"wheel_down","move_right"}, {"sig_depth","4"},
    };
    std::string add;
    for (auto& e : EXPECT) if (!seen.count(e.name)) { add += e.name; add += " = "; add += e.def; add += "\n"; }
    if (!add.empty()) {
        FILE* fa = fopen(path, "a");
        if (fa) { fputs("\n# --- options added by a newer build (defaults) ---\n", fa);
                  fputs(add.c_str(), fa); fclose(fa); }
        hook_log("[tuner] appended missing option(s) to %s\n", path);
    }
    hook_log("[tuner] key bindings loaded from %s\n", path);
}

// ── Live key-binding access for the in-game config overlay ────────────────────
// g_keys is 14 ints in declaration order; expose them by index paired with the
// config-file action name. The poll thread reads g_keys live, so a set() here
// takes effect on the next poll iteration (no restart).
namespace {
    const char* const kTunerActions[] = {
        "toggle","next","prev","move_left","move_right","move_up","move_down",
        "resize_mod","uv_mod","uv_vert_mod","vmove_mod","save","clear","exit",
        "box_mode"
    };
    constexpr int kTunerKeyN = (int)(sizeof(kTunerActions)/sizeof(kTunerActions[0]));
}
int rect_tuner_key_count() { return kTunerKeyN; }
const char* rect_tuner_key_action(int i) {
    return (i >= 0 && i < kTunerKeyN) ? kTunerActions[i] : "";
}
int rect_tuner_key_vk(int i) {
    if (i < 0 || i >= kTunerKeyN) return 0;
    return (&g_keys.toggle)[i];
}
void rect_tuner_key_set(int i, int vk) {
    if (i < 0 || i >= kTunerKeyN) return;
    (&g_keys.toggle)[i] = vk;
}

// VK -> a token vk_from_token() round-trips. Falls back to "0xNN".
void rect_tuner_vk_name(int vk, char* out, size_t cap) {
    if (!out || cap == 0) return;
    if (vk >= 'A' && vk <= 'Z') { snprintf(out, cap, "%c", vk); return; }
    if (vk >= '0' && vk <= '9') { snprintf(out, cap, "%c", vk); return; }
    if (vk >= VK_F1 && vk <= VK_F12) { snprintf(out, cap, "F%d", vk - VK_F1 + 1); return; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { snprintf(out, cap, "NUMPAD%d", vk - VK_NUMPAD0); return; }
    struct { int v; const char* n; } M[] = {
        {VK_LEFT,"LEFT"},{VK_RIGHT,"RIGHT"},{VK_UP,"UP"},{VK_DOWN,"DOWN"},
        {VK_RETURN,"ENTER"},{VK_ESCAPE,"ESC"},{VK_BACK,"BACKSPACE"},{VK_SPACE,"SPACE"},
        {VK_TAB,"TAB"},{VK_SHIFT,"SHIFT"},{VK_CONTROL,"CTRL"},{VK_MENU,"ALT"},
        {VK_LSHIFT,"LSHIFT"},{VK_RSHIFT,"RSHIFT"},{VK_LCONTROL,"LCTRL"},{VK_RCONTROL,"RCTRL"},
        {VK_INSERT,"INSERT"},{VK_DELETE,"DELETE"},{VK_HOME,"HOME"},{VK_END,"END"},
        {VK_PRIOR,"PAGEUP"},{VK_NEXT,"PAGEDOWN"},
        {VK_ADD,"NUMADD"},{VK_SUBTRACT,"NUMSUB"},{VK_MULTIPLY,"NUMMUL"},{VK_DIVIDE,"NUMDIV"},
        {VK_OEM_PLUS,"PLUS"},{VK_OEM_MINUS,"MINUS"},{VK_OEM_4,"LBRACKET"},{VK_OEM_6,"RBRACKET"},
        {VK_OEM_102,"OEM_102"},
    };
    for (auto& m : M) if (m.v == vk) { snprintf(out, cap, "%s", m.n); return; }
    snprintf(out, cap, "0x%02X", vk & 0xFF);
}

// Rewrite rect_tuner_keys.txt from the live bindings, preserving comments/order.
bool rect_tuner_keys_save() {
    char path[MAX_PATH]; keys_sidecar_path(path, sizeof(path));
    // Read existing lines (or seed a template first so comments exist).
    FILE* fr = fopen(path, "rb");
    if (!fr) { write_keys_template(path); fr = fopen(path, "rb"); }
    std::vector<std::string> lines;
    if (fr) {
        char buf[1024];
        while (fgets(buf, sizeof(buf), fr)) lines.push_back(buf);
        fclose(fr);
    }
    auto token_for = [](const char* action, char* out, size_t cap) -> bool {
        for (int i = 0; i < kTunerKeyN; ++i)
            if (!strcmp(action, kTunerActions[i])) {
                rect_tuner_vk_name((&g_keys.toggle)[i], out, cap); return true;
            }
        return false;
    };
    // Rewrite the value of any action line we own; copy everything else verbatim.
    for (auto& ln : lines) {
        const char* p = ln.c_str();
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == '\r' || *p == '\n' || *p == 0) continue; // comment/blank
        const char* eq = strchr(p, '=');
        if (!eq) continue;
        char key[64]; int kl = (int)(eq - p); if (kl > 63) kl = 63;
        memcpy(key, p, kl); key[kl] = 0;
        for (int i = kl - 1; i >= 0 && (key[i]==' '||key[i]=='\t'); --i) key[i] = 0;
        char tok[32];
        if (!token_for(key, tok, sizeof(tok))) continue; // not one of ours (wheel_up/sig_depth/...)
        const char* hash = strchr(eq, '#');
        std::string rebuilt = key;
        rebuilt += " = "; rebuilt += tok;
        if (hash) { rebuilt += "  "; rebuilt += hash; }
        else      { rebuilt += "\n"; }
        if (!rebuilt.empty() && rebuilt.back() != '\n') rebuilt += "\n";
        ln = rebuilt;
    }
    FILE* fw = fopen(path, "wb");
    if (!fw) return false;
    for (auto& ln : lines) fputs(ln.c_str(), fw);
    fclose(fw);
    hook_log("[tuner] saved key bindings to %s\n", path);
    return true;
}

// ── input: GetAsyncKeyState polling (proven; no swallow — see note above) ─────
static bool game_foreground() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid != 0 && pid == GetCurrentProcessId();
}
static inline bool kd(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// Apply ONE step of an action, honoring the held resize/uv modifiers. Shared by
// the mouse-wheel hook (the keyboard poll inlines the same logic with its own
// hold-repeat throttling).
static void apply_action(int act) {
    bool resize = kd(g_keys.resize_mod), uv = kd(g_keys.uv_mod);
    switch (act) {
        case ACT_MOVE_L: if (uv) in_du--; else if (resize) in_dw--; else in_dx--; break;
        case ACT_MOVE_R: if (uv) in_du++; else if (resize) in_dw++; else in_dx++; break;
        case ACT_MOVE_U: if (uv) in_dv--; else if (resize) in_dh--; else in_dy--; break;
        case ACT_MOVE_D: if (uv) in_dv++; else if (resize) in_dh++; else in_dy++; break;
        case ACT_NEXT:   in_cycle++; break;
        case ACT_PREV:   in_cycle--; break;
        default: break;
    }
}

static DWORD WINAPI poll_thread(LPVOID) {
    hook_log("[tuner] poll thread started — press the toggle key in-game\n");
    bool pTog = false, pNext = false, pPrev = false,
         pSave = false, pClr = false, pExit = false, pBox = false;
    bool pL = false, pR = false, pU = false, pD = false;
    int  hL = 0, hR = 0, hU = 0, hD = 0;   // per-key hold-frame counters
    while (true) {
        if (!game_foreground()) { Sleep(60); pTog = false; continue; }
        // Suspend all tuner input while the config overlay is open (its own key
        // capture is remapping these bindings; polling them here would fight it).
        if (g_config_overlay_active.load(std::memory_order_relaxed)) {
            Sleep(30);
            pTog = pNext = pPrev = pSave = pClr = pExit = pBox = false;
            pL = pR = pU = pD = false; hL = hR = hU = hD = 0;
            continue;
        }

        bool tog = kd(g_keys.toggle);
        if (tog && !pTog) {
            // Cycle OFF -> TOP -> BOTTOM -> OFF. Editing stays active in both
            // TOP and BOTTOM; only the info panel moves, then closes.
            if (!g_active.load(std::memory_order_relaxed)) {
                g_active.store(true, std::memory_order_relaxed);
                g_hud_pos.store(0, std::memory_order_relaxed);   // -> TOP
                hook_log("[tuner] toggle -> ON (panel TOP)\n");
            } else if (g_hud_pos.load(std::memory_order_relaxed) == 0) {
                g_hud_pos.store(1, std::memory_order_relaxed);    // -> BOTTOM
                hook_log("[tuner] toggle -> ON (panel BOTTOM)\n");
            } else {
                g_active.store(false, std::memory_order_relaxed); // -> OFF
                hook_log("[tuner] toggle -> OFF\n");
            }
        }
        pTog = tog;

        if (g_active.load(std::memory_order_relaxed)) {
            bool resize = kd(g_keys.resize_mod), uv = kd(g_keys.uv_mod);

            bool nx = kd(g_keys.next), pv = kd(g_keys.prev);
            if (nx && !pNext) in_cycle++;
            if (pv && !pPrev) in_cycle--;
            pNext = nx; pPrev = pv;

            // Nudge: 1px on the initial press (precise single taps); if the key
            // is HELD past ~1s, auto-repeat 1px so a big shift (e.g. 100px)
            // doesn't need 100 taps. Poll is ~16ms, so 60 frames ≈ 1s, then 1px
            // every 2 frames (~30px/s). held_fire() returns true on the frames a
            // step should apply.
            auto held_fire = [](bool down, bool prev, int& hc) -> bool {
                if (!down) { hc = 0; return false; }
                if (!prev) { hc = 1; return true; }        // initial press -> 1px
                ++hc;
                return hc > 60 && ((hc - 60) % 2 == 0);    // held >~1s -> repeat
            };
            bool L = kd(g_keys.mleft), R = kd(g_keys.mright), U = kd(g_keys.mup), D = kd(g_keys.mdown);
            if (held_fire(L, pL, hL)) { if (uv) in_du--; else if (resize) in_dw--; else in_dx--; }
            if (held_fire(R, pR, hR)) { if (uv) in_du++; else if (resize) in_dw++; else in_dx++; }
            if (held_fire(U, pU, hU)) { if (uv) in_dv--; else if (resize) in_dh--; else in_dy--; }
            if (held_fire(D, pD, hD)) { if (uv) in_dv++; else if (resize) in_dh++; else in_dy++; }
            pL = L; pR = R; pU = U; pD = D;

            // save: plain = sig-scoped (isolated); with uv_mod held = BROAD save
            // (atlas-only, persists through screen transitions / walking).
            bool sv = kd(g_keys.save);
            if (sv && !pSave) {
                if (uv) in_save_broad.store(true, std::memory_order_relaxed);
                else    in_save.store(true, std::memory_order_relaxed);
            }
            pSave = sv;
            bool cl = kd(g_keys.clear); if (cl && !pClr)  in_clear.store(true, std::memory_order_relaxed);  pClr = cl;
            // Switch the panel between RECT and BOX editing (consumed on the
            // render thread — box_tuner_* is render-thread-only).
            bool bm = kd(g_keys.box_mode);
            if (bm && !pBox) in_box_toggle.fetch_add(1, std::memory_order_relaxed);
            pBox = bm;
            // ESC exit removed by request — F10 (toggle) is the sole on/off switch.
        (void)pExit;
        } else {
            pNext = pPrev = pSave = pClr = pExit = pBox = pL = pR = pU = pD = false;
        }
        Sleep(16);
    }
    return 0;
}

// ── mouse-wheel input (low-level mouse hook) ─────────────────────────────────
// The wheel is a delta event, not a pollable key state, so a WH_MOUSE_LL hook is
// the only way to read it. We only touch WM_MOUSEWHEEL (everything else passes
// straight through), and only while the tuner is active + the game is foreground.
// The event is swallowed then so the game menu doesn't also scroll.
static HHOOK g_mouseHook = nullptr;
static LRESULT CALLBACK LowLevelMouseProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && wp == WM_MOUSEWHEEL &&
        g_active.load(std::memory_order_relaxed) && game_foreground()) {
        const MSLLHOOKSTRUCT* m = (const MSLLHOOKSTRUCT*)lp;
        int delta = (int)(short)HIWORD(m->mouseData);     // +120 up, -120 down
        int steps = delta / WHEEL_DELTA; if (steps < 0) steps = -steps; if (steps < 1) steps = 1;
        // BOX MODE: exactly one step per wheel event, whatever the delta says.
        // Mouse drivers that accelerate the wheel (or map a modifier to "fast
        // scroll") send ONE event carrying a multiple of WHEEL_DELTA — which
        // turns into a ~10px jump here. A box width is dialled in single pixels,
        // so the notch, not the delta, is the unit.
        if (g_box_mode.load(std::memory_order_relaxed)) steps = 1;
        // uv_vert_mod (ALT) held -> the wheel shifts the ATLAS SOURCE vertically
        // (in_dv): forward = up (v--), backward = down (v++). This complements
        // uv_mod (CTRL) + wheel, which shifts it horizontally (in_du) via the
        // configured MOVE_L/MOVE_R action. Arrow/numpad keys are hijacked by the
        // game, so the wheel is the only usable driver here.
        if (kd(g_keys.uv_vert_mod)) {
            // ALT + wheel -> shift the ATLAS SOURCE vertically (v--/v++).
            for (int i = 0; i < steps; i++) { if (delta > 0) in_dv--; else in_dv++; }
        } else if (kd(g_keys.vmove_mod)) {
            // '<' + wheel -> MOVE the rect up/down (forward=up). Goes through
            // apply_action so it still composes with resize/uv mods, exactly like
            // the keyboard up/down. Wheel alone (below) stays left/right.
            for (int i = 0; i < steps; i++)
                apply_action(delta > 0 ? ACT_MOVE_U : ACT_MOVE_D);
        } else {
            int act = (delta > 0) ? g_wheel_up : g_wheel_down;
            for (int i = 0; i < steps; i++) apply_action(act);
        }
        return 1;                                         // swallow (no game scroll)
    }
    return CallNextHookEx(g_mouseHook, code, wp, lp);
}
static DWORD WINAPI mouse_thread(LPVOID) {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&LowLevelMouseProc, &self);
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, self, 0);
    if (!g_mouseHook) { hook_log("[tuner] mouse-wheel hook failed (%lu)\n", GetLastError()); return 0; }
    hook_log("[tuner] mouse-wheel hook installed\n");
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}

void rect_tuner_install() {
    if (!g_cfg.rect_tuner) return;
    load_keys();
    CreateThread(nullptr, 0, poll_thread, nullptr, 0, nullptr);   // keys (poll)
    CreateThread(nullptr, 0, mouse_thread, nullptr, 0, nullptr);  // wheel (LL hook)
    hook_log("rect_tuner: enabled — toggle key opens it in-game; edits -> rect_tuner.txt; "
             "bindings -> rect_tuner_keys.txt\n");
}
