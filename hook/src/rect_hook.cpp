// Universal textured-rect inventory + un-clip.
//
// Every 2D UI sprite in BOF4 (title menu, status screen, shops, HUD, item menus)
// is submitted through ONE function: BOF4!0x502070 (3500+ call sites). The caller
// fills a 5-dword packet at the global write pointer [0x00B4EFA0], then calls
// 0x502070(layer, pktSize) which enqueues it and advances the pointer.
//
// Packet (GP0 0x64 textured rectangle, variable size — pktSize = 0x14 = 20 bytes):
//   +0x00 = 0x04000000            DMA tag (4 words follow)
//   +0x04 = 0x64808080            GP0 cmd 0x64 | colour     (cmd byte at +0x07)
//   +0x08 = (y<<16) | x           POSITION
//   +0x0C = (clut<<16)|(v<<8)|u   TEXCOORD / CLUT (atlas source)
//   +0x10 = (h<<16) | w           SIZE  (low16 w = the on-screen clip width)
//
// The width here is the exact same field the title-menu .text patch edits — but
// hooking the submit lets us read/rewrite it for EVERY label at once, with no
// per-VA hunting. Two opt-in modes (config in _d3d9_hook_config.txt):
//
//   rect_trace = true : log each unique textured rect (x/y/w/h + atlas u/v/clut)
//                       to d3d9_hook.log; press F9 to dump the full unique set to
//                       rect_inventory.txt. This is the automated label inventory.
//   rect_fix   = true : rewrite the width (and optionally x/y and atlas u/v) of any
//                       rect whose ORIGINAL source (u,v[,clut]) matches a line in
//                       the sidecar rect_widths.txt next to BOF4.exe.
//
// All foreign reads are SEH-guarded; original behaviour is preserved (we only edit
// the already-filled packet in place before forwarding the call).

#include "hooks.h"
#include "hook_health.h"
#include "rect_hook.h"
#include "rect_tuner.h"
#include "tex_prov.h"     // tex_prov_resolve(v) -> source "DAT recNN" for the report
#include "menu_rect_fix.h"
#include "minhook/MinHook.h"
#include <intrin.h>   // _AddressOfReturnAddress (caller-chain capture)

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <intrin.h>

// Provided by bof4_hooks.cpp: the SINGLE last-fired editable DAT (the menu's own
// DAT, e.g. SAGMEN*.DAT), filtered to the graphics_to_edit\ whitelist; falls back
// to the last *.DAT opened (tagged) if none editable. Names each scene's atlas.
void bof4_scene_dat(char* out, size_t cap);
// Re-scan graphics_to_edit\ for the editable-DAT whitelist (called on F7).
void bof4_reload_dat_whitelist(void);

// Universal textured-rect submit (GP0 0x64), located by signature so the
// hook survives EXE recompiles. Prologue: mov eax,[esp+4] / mov ecx,[global]
// / and eax,0xFF / push esi / push ecx / mov edx,[eax*4+global] / lea esi,
// [eax*4+global]. The three globals are wildcarded. Verified unique +
// relocating in both builds (live 0x00502070 -> recompile 0x005022F0).
static const uint8_t SIG_SUBMIT[] = {
    0x8B,0x44,0x24,0x04,0x8B,0x0D,0,0,0,0,0x25,0xFF,0x00,0x00,0x00,
    0x56,0x51,0x8B,0x14,0x85,0,0,0,0,0x8D,0x34,0x85,
};
static const char SIG_SUBMIT_MASK[] = "xxxxxx????xxxxxxxxxx????xxx";
static constexpr uintptr_t FALLBACK_SUBMIT = 0x00502070;
static uintptr_t ADDR_SUBMIT = FALLBACK_SUBMIT;   // resolved in rect_hook_install
// Global packet-write pointer. DERIVED at install from the resolved submit
// site (the `mov ecx,[global]` at submit+4 carries its imm32 at submit+6),
// so it relocates with the EXE alongside the hook. Falls back to the live
// build's address. Shared with rect_tuner.cpp (extern), which rides this hook.
uintptr_t PKT_WRITE_PTR = 0x00B4EFA0;

typedef int (__cdecl *submit_t)(int layer, int pktSize);
static submit_t g_orig_submit  = nullptr;

// ── decoded rect ────────────────────────────────────────────────────────────
#define RA_MAX 12
struct Rect { int x, y, w, h; unsigned u, v, clut, cmd, layer;
              unsigned ra[RA_MAX]; int nra; int scene;
              unsigned first_frame, last_frame, hits; };  // rect lifetime tracking

// Per-present frame counter (ticked from the Present hook via rect_hook_frame_tick).
// Inventory entries record first/last frame + hit count so the F8 dump can flag
// SHORT-LIVED rects — the millisecond-flashing graphics (fishing catch/hook, …).
static std::atomic<unsigned> g_frame{0};
static unsigned g_record_origin = 0;   // frame at last F11 clear (relative display)
void rect_hook_frame_tick() { g_frame.fetch_add(1, std::memory_order_relaxed); }

// FNV-1a over the nearest N callers = "which box/menu drew this". Used to (a)
// give the Rect Tuner granular selection and (b) let a saved override match only
// that draw site (so tuning one box doesn't move every same-atlas box). The two
// uses MUST hash identically — this single helper is the source of truth.
// Depth is tunable (rect_tuner_keys.txt `sig_depth`): the nearest frames are the
// shared tile/box drawer, so NEAR-IDENTICAL menus only diverge DEEPER — raise
// the depth to isolate them, lower it if a deep-frame scan wobble causes flicker.
// Default is SHALLOW (4) on purpose: 0x502070 is called in a tight loop, so the
// deep stack below THIS rect's real call chain holds STALE return addresses from
// earlier iterations that differ rect-to-rect — grabbing them made the signature
// (and the tuner's rect count / cycling) wobble. The nearest ~4 validated frames
// are the genuine draw-site chain (submit-call → tile drawer → menu layout), which
// is stable frame-to-frame AND distinguishes menus. Raise only if two different
// menus still collide; lower if focus flickers. 1..RA_MAX.
int g_rect_sig_depth = 4;   // set by the tuner's config loader; 1..RA_MAX
static inline unsigned caller_sig(const unsigned* ra, int nra) {
    unsigned sig = 2166136261u;
    int lim = g_rect_sig_depth; if (lim < 1) lim = 1; if (lim > RA_MAX) lim = RA_MAX;
    for (int i = 0; i < nra && i < lim; i++) { sig ^= ra[i]; sig *= 16777619u; }
    return sig;
}

// True if the bytes just before `ret` encode a CALL — i.e. `ret` is a genuine
// return address, not a stray code-looking value (a saved register, a local, a
// function pointer) that merely lands in the .text range. This is what makes the
// scanned chain the REAL caller chain instead of noise: without it, near-top junk
// slipped in and the signature jittered. Checks the two common encodings:
//   E8 cd            call rel32           (byte at ret-5 == 0xE8)
//   FF /2  (2..7 B)  call r/m32           (0xFF opcode with ModRM.reg == 010b)
// `ret` is always inside mapped .text here, so reading ret-7..ret-1 is safe.
static inline bool is_call_return(uint32_t ret) {
    const uint8_t* p = (const uint8_t*)(uintptr_t)ret;
    if (p[-5] == 0xE8) return true;                       // call rel32
    for (int len = 2; len <= 7; ++len) {                 // call r/m32 (FF /2)
        if (p[-len] != 0xFF) continue;
        if (((p[-len + 1] >> 3) & 7) == 2) return true;  // ModRM.reg == /2
    }
    return false;
}

// Capture the call-stack return-address chain at enqueue time. Because 0x502070
// is the command-queue ENQUEUE, the menu-specific draw code is still on the
// stack here, so these EIPs identify WHICH menu/box issued the rect — the
// discriminator that on-screen position alone can't give. `sp` must be the
// detour's own _AddressOfReturnAddress() (points at our return slot); we scan
// upward for CALL-validated return addresses inside BOF4.exe's .text.
static void capture_callers(uint32_t* sp, unsigned* out, int* count) {
    *count = 0;
    __try {
        for (int i = 0; i < 256 && *count < RA_MAX; i++) {
            uint32_t v = sp[i];
            if (v >= 0x00401000u && v < 0x00700000u && is_call_return(v)) {
                if (*count == 0 || out[*count - 1] != v)   // drop consecutive dups
                    out[(*count)++] = v;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// One-time log of each distinct GP0 command byte seen at the submit — so an
// unidentified primitive (e.g. a menu's dark fill that never flashes) can be
// spotted. Gated on rect_trace to stay quiet during normal tuning.
static void diag_seen_cmd(unsigned cmd) {
    if (!g_cfg.rect_trace && !g_cfg.rect_tuner && !g_cfg.tex_prov_log) return;
    static bool seen[256] = { false };
    if (cmd < 256 && !seen[cmd]) {
        seen[cmd] = true;
        hook_log("[rect] GP0 cmd 0x%02X seen at 0x502070 "
                 "(0x60-63 flat rect, 0x64-67 textured rect, 0x20-3F poly, "
                 "0xE0-E7 env/texpage, other=?)\n", cmd);
    }
}

// DIAGNOSTIC: if a texpage / draw-env packet (GP0 0xE0-0xE7) flows through the
// submit queue, its low bits set the texture-page BASE that the following
// textured rects sample from — exactly what we need to map a rect's page-relative
// u/v to an absolute VRAM (x,y), and thus to an uploaded DAT rec. Log each
// distinct packet word once so we can see whether (and how) texpage is set here.
static void diag_texpage(const uint8_t* d, unsigned cmd) {
    if (!g_cfg.rect_trace && !g_cfg.rect_tuner && !g_cfg.tex_prov_log) return;
    if ((cmd & 0xF8) != 0xE0) return;
    static std::unordered_set<unsigned> seen;
    unsigned word = *(const uint32_t*)(d + 0x04);   // GP0 cmd|payload
    if (seen.insert(word).second) {
        // For 0xE1 (texpage): tp_x=(word&0xF)*64, tp_y=((word>>4)&1)*256, tp bpp=(word>>7)&3.
        unsigned tp_x = (word & 0xF) * 64, tp_y = ((word >> 4) & 1) * 256, bpp = (word >> 7) & 3;
        hook_log("[rect] TEXPAGE/env 0x%02X word=0x%08X  -> tp_x=%u tp_y=%u bppcode=%u "
                 "(0=4bpp 1=8bpp 2=15bpp)\n", cmd, word, tp_x, tp_y, bpp);
    }
}

static bool read_packet(Rect& r, int layer) {
    uint8_t* pkt = nullptr;
    __try { pkt = *(uint8_t**)PKT_WRITE_PTR; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!pkt) return false;
    uint8_t d[0x14];
    __try { memcpy(d, pkt, sizeof(d)); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    unsigned cmd = d[0x07];
    diag_seen_cmd(cmd);
    diag_texpage(d, cmd);
    // Variable-size RECTS: GP0 0x60..0x67.
    //   0x64..0x67 = TEXTURED (a texcoord word at +0x0C, SIZE at +0x10).
    //   0x60..0x63 = FLAT/monochrome (NO texcoord, SIZE at +0x0C) — a half-opaque
    //                dark box fill is a semi-transparent flat rect (typically 0x62).
    // low 2 bits are raw-texture/semi-transparent flags; bit 2 (0x04) = textured.
    if ((cmd & 0xF8) != 0x60) return false;
    bool textured = (cmd & 0x04) != 0;
    unsigned pos  = *(uint32_t*)(d + 0x08);
    unsigned size = textured ? *(uint32_t*)(d + 0x10) : *(uint32_t*)(d + 0x0C);
    unsigned tex  = textured ? *(uint32_t*)(d + 0x0C) : 0;
    r.x = (int)(int16_t)(pos & 0xFFFF);  r.y = (int)(int16_t)(pos >> 16);
    r.w = (int)(size & 0xFFFF);          r.h = (int)(size >> 16);
    r.u = tex & 0xFF;  r.v = (tex >> 8) & 0xFF;  r.clut = (tex >> 16) & 0xFFFF;
    r.cmd = cmd;       r.layer = (unsigned)layer;  r.nra = 0;  r.scene = 0;
    return true;
}

// ── inventory (rect_trace) ──────────────────────────────────────────────────
// Keyed by atlas identity (u,v,clut,w,h). The stored POSITION is updated on every
// sighting (last-seen wins): on a settled screen a label redraws at its resting
// spot every frame, so the dump shows true resting x/y — essential for mapping a
// label to a word by its on-screen position. F11 clears the inventory so you can
// snapshot one screen without stale entries from earlier (animating) frames.
static std::mutex g_mtx;
static std::unordered_map<uint64_t, size_t> g_idx;   // key -> index in g_inv
static std::vector<Rect> g_inv;                      // unique rects for F9 dump
// distinct on-screen (x,y) positions seen per atlas key — so a tile drawn at
// several spots (e.g. two "pts.") shows ALL of them in the dump, letting you
// write per-instance overrides with mx=/my=.
static std::unordered_map<uint64_t, std::vector<std::pair<int,int>>> g_pos;
static constexpr size_t MAX_INV = 8000;
static constexpr size_t MAX_POS = 8;   // cap positions per tile (anti-blowup)
// F7 increments this; each FRESH rect is stamped with the current value so the F9
// dump can group labels per scene. 0 = no scene tags used (dump stays ungrouped).
static volatile long g_scene_epoch = 0;
// Per-scene atlas label = the DAT(s) resident when the scene's first rect appeared
// (captured once per epoch from bof4_recent_dats). Guarded by g_mtx.
static std::unordered_map<int, std::string> g_scene_names;

static inline uint64_t inv_key(const Rect& r) {
    return ((uint64_t)(r.u & 0xFF) << 0) ^ ((uint64_t)(r.v & 0xFF) << 8) ^
           ((uint64_t)(r.clut & 0xFFFF) << 16) ^ ((uint64_t)(r.w & 0xFFFF) << 32) ^
           ((uint64_t)(r.h & 0xFFFF) << 48);
}

static void trace_rect(const Rect& r) {
    // Accumulate into the inventory only (the F8/F7 dumps read this). No per-rect
    // logging to d3d9_hook.log — that was diagnostic spam; the only trace kept is
    // the [dat-open] line in the CreateFile hooks (which DATs are loaded).
    uint64_t k = inv_key(r);
    unsigned now = g_frame.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g_mtx);
    auto it = g_idx.find(k);
    if (it == g_idx.end()) {
        if (g_inv.size() < MAX_INV) {
            g_idx[k] = g_inv.size();
            Rect rr = r; rr.scene = (int)g_scene_epoch;   // stamp first-seen scene
            rr.first_frame = rr.last_frame = now; rr.hits = 1;   // lifetime start
            if (g_scene_names.find(rr.scene) == g_scene_names.end()) {
                char dbuf[96]; bof4_scene_dat(dbuf, sizeof(dbuf));
                g_scene_names[rr.scene] = dbuf;           // last editable DAT backing this scene
            }
            g_inv.push_back(rr);
        }
    } else {
        Rect& e = g_inv[it->second];          // last-seen position wins
        e.x = r.x; e.y = r.y; e.layer = r.layer;
        e.last_frame = now; if (e.hits < 0xFFFFFFFFu) e.hits++;   // lifetime update
    }
    auto& pl = g_pos[k];                       // record distinct positions
    if (pl.size() < MAX_POS &&
        std::find(pl.begin(), pl.end(), std::make_pair(r.x, r.y)) == pl.end())
        pl.push_back({r.x, r.y});
}

static void clear_inventory() {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_idx.clear(); g_inv.clear(); g_pos.clear(); g_scene_names.clear();
    g_scene_epoch = 0;                                 // reset scene grouping too
    g_record_origin = g_frame.load(std::memory_order_relaxed);  // frames shown relative
    hook_log("[rect] F11: inventory cleared — re-accumulating from current screen "
             "(lifetime window starts now; trigger the transient, then F8 to dump)\n");
}

static void dump_inventory(const char* fname) {
    std::vector<Rect> snap;
    std::unordered_map<uint64_t, std::vector<std::pair<int,int>>> posSnap;
    std::unordered_map<int, std::string> nameSnap;
    int epoch;
    { std::lock_guard<std::mutex> lk(g_mtx); snap = g_inv; posSnap = g_pos;
      nameSnap = g_scene_names; epoch = (int)g_scene_epoch; }
    bool grouped = epoch > 0;     // scene tags were used (≥1 F7 press)
    std::sort(snap.begin(), snap.end(), [grouped](const Rect& a, const Rect& b){
        if (grouped && a.scene != b.scene) return a.scene < b.scene;
        if (a.v != b.v) return a.v < b.v;
        if (a.u != b.u) return a.u < b.u;
        return a.w < b.w;
    });
    FILE* f = fopen(fname, "w");
    if (!f) { hook_log("[rect] dump: fopen(%s) failed\n", fname); return; }
    fprintf(f, "# Universal textured-rect inventory (hook on BOF4!0x502070).\n"
               "# Each line is one unique on-screen rect. x/y = LAST-seen position\n"
               "# (settle the screen, press F11 to clear, then F8 to dump = resting pos).\n"
               "# F6 = tag a new screen while playing through; the dump then groups\n"
               "# labels under '# === scene N ===' headers (rename them freely — all\n"
               "# '#' lines are ignored by measure_atlas.py).\n"
               "# To un-clip a German label, copy its u/v (and clut if needed) into\n"
               "# rect_widths.txt with a new w=.\n"
               "#\n");

    // ── SHORT-LIVED (TRANSIENT) CANDIDATES ────────────────────────────────────
    // The millisecond-flashing graphics (fishing catch/hook, etc.) are captured
    // like everything else, but appear in only a FEW frames. We list the rects
    // with the lowest hit-count first — those bubble the flashes to the top so you
    // don't have to hunt through hundreds of persistent rects. Workflow: F11 to
    // clear on the idle screen, trigger the transient, then F8 to dump.
    {
        unsigned nowf   = g_frame.load(std::memory_order_relaxed);
        unsigned window = nowf - g_record_origin;            // frames since F11 clear
        std::vector<const Rect*> cand;
        for (const Rect& r : snap) {
            // "not omnipresent": drawn in clearly fewer frames than the window.
            bool transient = (window == 0) ? true
                           : ((uint64_t)r.hits * 100u < (uint64_t)window * 75u);
            if (transient) cand.push_back(&r);
        }
        std::sort(cand.begin(), cand.end(),
                  [](const Rect* a, const Rect* b){ return a->hits < b->hits; });
        fprintf(f, "# ============================================================\n"
                   "# SHORT-LIVED / TRANSIENT CANDIDATES  (window = %u frames since\n"
                   "# last F11 clear; rarest first). The flashing graphic is almost\n"
                   "# certainly near the TOP. 'src' = source atlas DAT recNN.\n"
                   "# Copy a line's u/v/clut/ow into rect_tuner.txt and add nu/nv/w/h.\n"
                   "# ============================================================\n"
                   "#  hits  frames(rel)   x     y     w    h    u    v    clut    cmd   src\n",
                   window);
        int shown = 0;
        for (const Rect* rp : cand) {
            if (shown++ >= 60) { fprintf(f, "#   ...(%zu more)\n", cand.size() - 60); break; }
            const Rect& r = *rp;
            char src[192] = {0};
            bool hs = tex_prov_resolve((int)r.v, src, sizeof(src));
            unsigned sig = caller_sig(r.ra, r.nra);
            fprintf(f, "  %-5u %5u-%-5u %-5d %-5d %-4d %-4d %-4u %-4u 0x%04X 0x%02X  %s\n",
                    r.hits, r.first_frame - g_record_origin, r.last_frame - g_record_origin,
                    r.x, r.y, r.w, r.h, r.u, r.v, r.clut, r.cmd,
                    hs ? src : "(no src — need rect_tuner + vram_rec_map.tsv)");
            fprintf(f, "#     u=%u v=%u clut=0x%04X ow=%d   (sig=0x%08X)\n",
                    r.u, r.v, r.clut, r.w, sig);
        }
        fprintf(f, "#\n# ---- FULL INVENTORY (all rects, by v/u) ----\n"
                   "#   x     y     w    h    u    v    clut    cmd  layer\n");
    }

    int cur_scene = -1;
    for (const Rect& r : snap) {
        if (grouped && r.scene != cur_scene) {
            cur_scene = r.scene;
            auto nit = nameSnap.find(cur_scene);
            if (nit != nameSnap.end() && !nit->second.empty())
                fprintf(f, "\n# ===== scene %d: %s =====\n", cur_scene, nit->second.c_str());
            else
                fprintf(f, "\n# ===== scene %d =====\n", cur_scene);
        }
        fprintf(f, "  %-5d %-5d %-4d %-4d %-4u %-4u 0x%04X  0x%02X  %u\n",
                r.x, r.y, r.w, r.h, r.u, r.v, r.clut, r.cmd, r.layer);
        if (r.nra > 0) {                            // caller chain → add caller=0x.. to gate
            fprintf(f, "#   callers:");
            for (int i = 0; i < r.nra; i++) fprintf(f, " 0x%06X", r.ra[i]);
            fprintf(f, "\n");
        }
        auto it = posSnap.find(inv_key(r));         // tile drawn at >1 position?
        if (it != posSnap.end() && it->second.size() > 1) {
            fprintf(f, "#   ^ drawn %zu×; positions:", it->second.size());
            for (auto& p : it->second) fprintf(f, " (%d,%d)", p.first, p.second);
            fprintf(f, "  -> split with mx=/my=\n");
        }
    }
    fclose(f);
    hook_log("[rect] dump: wrote %s (%zu unique rects)\n", fname, snap.size());
}

// ── overrides (rect_fix) ────────────────────────────────────────────────────
struct Ovr {
    int  mu, mv;        // match source u,v  (mv<0 / mu<0 = wildcard)
    int  mclut;         // match clut (-1 = any)
    int  mw;            // match original width (-1 = any) — disambiguates same u,v
    int  mx, my;        // match ON-SCREEN x/y (INT_MIN = any) — splits a tile drawn
                        //   at several positions (e.g. two "pts.") into per-spot rules
    unsigned mra;       // match CALLER EIP (0 = any) — gate on the menu/box draw
                        //   site so a coord-identical box in another menu is left
                        //   alone; matches if mra appears anywhere in r.ra[] chain
    unsigned msig;      // match caller-chain SIGNATURE (0 = any) — the strongest
                        //   draw-site key; equals caller_sig(r.ra). Written by the
                        //   Rect Tuner so tuning one box never touches another that
                        //   shares the same blank atlas tile.
    int  nw, nx, ny;    // new width / x / y   (INT_MIN = leave; nw -1 = leave)
    int  nh;            // new height          (-1 = leave) — for 9-slice box edges/fill
    int  ndx, ndy;      // relative nudge x/y  (0 = none) — applied after nx/ny
    int  ndw, ndh;      // relative grow w/h   (0 = none) — added to (nw/nh else original)
    int  nu, nv;        // new atlas u,v        (-1 = leave)
    int  copyx;         // INJECT: after this piece draws, re-emit a copy shifted
                        //   +copyx px in X (INT_MIN = off). Tiles a procedural
                        //   fill into a widened gap the engine won't fill itself.
    // COVER-THE-GAP: when this override matches, inject a FLAT rect (GP0 0x60
    //   opaque / 0x62 semi-transparent) of colour `fillcol` at (fillx,filly,
    //   fillw,fillh) right after this piece submits — painting over the dark
    //   "half-opaque" scene-dim that shows through a widened box's interior.
    //   Menu text/box border drawn later in the frame stay ON TOP. fillcol -1 = off.
    int  fillcol;       // 0xRRGGBB (-1 = off)
    int  fillx, filly, fillw, fillh;  // absolute screen rect for the fill
    int  fillsemi;      // 0 = opaque (0x60), 1 = semi-transparent (0x62)
};
static std::vector<Ovr> g_ovr;
static std::mutex g_ovr_mtx;     // guards g_ovr (render thread reads, F8 reloads)

static void sidecar_path(char* out, size_t n) {
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char* s = strrchr(exe, '\\'); if (s) *(s + 1) = 0; else exe[0] = 0;
    snprintf(out, n, "%srect_widths.txt", exe);
}

// Parse one override file (rect_widths.txt or rect_tuner.txt) APPENDING to out.
// Missing file = no-op (rect_tuner.txt won't exist until the tuner saves).
static void parse_override_file(const char* path, std::vector<Ovr>& out) {
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* p = line; while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == 0) continue;
        // Shared file with menu_rect_fix: a line whose FIRST token is not
        // key=value is a name-keyed title-menu line (e.g. "NewGame w=174") — it
        // belongs to menu_rect_fix, so skip the whole line here.
        { char first[64]; if (sscanf(p, "%63s", first) == 1 && !strchr(first, '=')) continue; }
        Ovr o; o.mu = o.mv = -1; o.mclut = -1; o.mw = -1; o.mx = INT32_MIN; o.my = INT32_MIN;
        o.mra = 0; o.msig = 0;
        o.nw = -1; o.nx = INT32_MIN; o.ny = INT32_MIN; o.nh = -1;
        o.ndx = 0; o.ndy = 0; o.ndw = 0; o.ndh = 0; o.nu = -1; o.nv = -1;
        o.copyx = INT32_MIN;
        o.fillcol = -1; o.fillx = 0; o.filly = 0; o.fillw = 0; o.fillh = 0; o.fillsemi = 0;
        char tok[64]; int adv;
        while (sscanf(p, "%63s%n", tok, &adv) == 1) {
            p += adv;
            if (tok[0] == '#') break;
            char* eq = strchr(tok, '='); if (!eq) continue;
            *eq = 0; int val = (int)strtol(eq + 1, nullptr, 0);
            if      (!_stricmp(tok, "u"))    o.mu = val;
            else if (!_stricmp(tok, "v"))    o.mv = val;
            else if (!_stricmp(tok, "clut")) o.mclut = val;
            else if (!_stricmp(tok, "ow"))   o.mw = val;
            else if (!_stricmp(tok, "mx"))   o.mx = val;
            else if (!_stricmp(tok, "my"))   o.my = val;
            else if (!_stricmp(tok, "caller")) o.mra = (unsigned)strtoul(eq + 1, nullptr, 0);
            else if (!_stricmp(tok, "sig"))  o.msig = (unsigned)strtoul(eq + 1, nullptr, 0);
            else if (!_stricmp(tok, "w"))    o.nw = val;
            else if (!_stricmp(tok, "h"))    o.nh = val;
            else if (!_stricmp(tok, "x"))    o.nx = val;
            else if (!_stricmp(tok, "y"))    o.ny = val;
            else if (!_stricmp(tok, "dx"))   o.ndx = val;
            else if (!_stricmp(tok, "dy"))   o.ndy = val;
            else if (!_stricmp(tok, "dw"))   o.ndw = val;
            else if (!_stricmp(tok, "dh"))   o.ndh = val;
            else if (!_stricmp(tok, "nu"))   o.nu = val;
            else if (!_stricmp(tok, "nv"))   o.nv = val;
            else if (!_stricmp(tok, "copyx")) o.copyx = val;
            else if (!_stricmp(tok, "fillcol")) o.fillcol = (int)strtoul(eq + 1, nullptr, 0);
            else if (!_stricmp(tok, "fillx")) o.fillx = val;
            else if (!_stricmp(tok, "filly")) o.filly = val;
            else if (!_stricmp(tok, "fillw")) o.fillw = val;
            else if (!_stricmp(tok, "fillh")) o.fillh = val;
            else if (!_stricmp(tok, "fillsemi")) o.fillsemi = val;
        }
        // Require at least one match key (u or v): a keyed line with only a new
        // width and no u/v would be a wildcard matching EVERY rect — reject it.
        bool has_match = (o.mu >= 0 || o.mv >= 0 || o.mra != 0 || o.msig != 0);
        bool has_new   = (o.nw >= 0 || o.nh >= 0 || o.nx != INT32_MIN || o.ny != INT32_MIN ||
                          o.ndx != 0 || o.ndy != 0 || o.ndw != 0 || o.ndh != 0 ||
                          o.nu >= 0 || o.nv >= 0 || o.copyx != INT32_MIN || o.fillcol >= 0);
        if (has_match && has_new)
            out.push_back(o);
    }
    fclose(f);
    hook_log("[rect] loaded overrides from %s (total now %zu)\n", path, out.size());
}

// Load both sidecars: the hand-curated rect_widths.txt and the tuner-written
// rect_tuner.txt (in-game Rect Tuner). Later files stack on top; apply_fix uses
// first-match-wins, so hand-authored rect_widths.txt lines take precedence over
// a tuner line for the same rect (edit the hand line to override a saved tune).
static void parse_overrides(std::vector<Ovr>& out) {
    // NOTE: we no longer auto-create a rect_widths.txt template on startup — the
    // file is optional and only read if the user has hand-authored one.
    char path[MAX_PATH]; sidecar_path(path, sizeof(path));
    parse_override_file(path, out);                       // rect_widths.txt (if present)
    char tpath[MAX_PATH]; rect_tuner_sidecar_path(tpath, sizeof(tpath));
    parse_override_file(tpath, out);                      // rect_tuner.txt (if any)
}

static void load_overrides() { parse_overrides(g_ovr); }

// F8 hot-reload: re-parse the sidecar into a fresh list and swap it in under the
// lock so the render thread picks up new widths/positions next frame (no restart).
void rect_reload_overrides() {
    std::vector<Ovr> fresh;
    parse_overrides(fresh);
    { std::lock_guard<std::mutex> lk(g_ovr_mtx); g_ovr.swap(fresh); }
}

// SEH-only packet write (no C++ objects here — MSVC forbids __try alongside a
// destructor-bearing local like lock_guard, so this is split from apply_fix).
static void write_pkt(const Ovr& o, const Rect& r) {
    uint8_t* pkt = nullptr;
    __try { pkt = *(uint8_t**)PKT_WRITE_PTR; } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!pkt) return;
    __try {
        // SIZE field @ +0x10 = (h<<16)|w. width=low16, height=high16. Each can be
        // set absolutely (w=/h=) and/or grown by a delta (dw=/dh=). The delta adds
        // to the absolute when given, else to the rect's own original size — handy
        // for 9-slice box edges/fill whose base size varies per instance.
        if (o.nw >= 0 || o.ndw)
            *(uint16_t*)(pkt + 0x10) = (uint16_t)((o.nw >= 0 ? o.nw : (int)r.w) + o.ndw); // width
        if (o.nh >= 0 || o.ndh)
            *(uint16_t*)(pkt + 0x12) = (uint16_t)((o.nh >= 0 ? o.nh : (int)r.h) + o.ndh); // height
        // x/y: absolute (x=/y=) sets the base, then dx/dy nudges relative to the
        // label's actual on-screen position (r.x/r.y) when no absolute given.
        if (o.nx != INT32_MIN || o.ndx)
            *(uint16_t*)(pkt + 0x08) = (uint16_t)((o.nx != INT32_MIN ? o.nx : r.x) + o.ndx);
        if (o.ny != INT32_MIN || o.ndy)
            *(uint16_t*)(pkt + 0x0A) = (uint16_t)((o.ny != INT32_MIN ? o.ny : r.y) + o.ndy);
        if (o.nu >= 0)             *(uint8_t*) (pkt + 0x0C) = (uint8_t)o.nu;        // u
        if (o.nv >= 0)             *(uint8_t*) (pkt + 0x0D) = (uint8_t)o.nv;        // v
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Pending injection: a packet to emit right AFTER the original submit. Two kinds:
//   raw=false → a shifted COPY of the just-edited packet (copyx tiling);
//   raw=true  → a self-contained FLAT fill rect (fillgap), emitted verbatim.
static struct { bool active; bool raw; uint8_t bytes[0x14]; int newx; } g_pending = {};

// Build a flat monochrome rect packet (GP0 0x60 opaque / 0x62 semi-transparent)
// matching the engine's own flat-rect layout (see 0x00410770 + read_packet):
//   [+3]=len(3 dwords)  [+4..6]=R,G,B  [+7]=cmd  [+8]=pos(x|y<<16)  [+0C]=size(w|h<<16)
static void build_fill_pkt(const Ovr& o, uint8_t* out14) {
    memset(out14, 0, 0x14);
    out14[3] = 0x03;                                   // payload length = 3 dwords
    out14[4] = (uint8_t)((o.fillcol >> 16) & 0xFF);    // R
    out14[5] = (uint8_t)((o.fillcol >> 8)  & 0xFF);    // G
    out14[6] = (uint8_t)( o.fillcol        & 0xFF);    // B
    out14[7] = o.fillsemi ? 0x62 : 0x60;               // flat rect cmd
    *(uint16_t*)(out14 + 0x08) = (uint16_t)o.fillx;    // x
    *(uint16_t*)(out14 + 0x0A) = (uint16_t)o.filly;    // y
    *(uint16_t*)(out14 + 0x0C) = (uint16_t)o.fillw;    // w
    *(uint16_t*)(out14 + 0x0E) = (uint16_t)o.fillh;    // h
}

// SEH-only: snapshot the current packet bytes (no C++ objects alongside __try).
static bool snapshot_pkt(uint8_t* out14) {
    uint8_t* pkt = nullptr;
    __try { pkt = *(uint8_t**)PKT_WRITE_PTR; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!pkt) return false;
    __try { memcpy(out14, pkt, 0x14); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}
// SEH-only: write the pending copy into the (advanced) write slot and submit it.
static void emit_pending(int layer, int pktSize) {
    uint8_t* pkt = nullptr;
    __try { pkt = *(uint8_t**)PKT_WRITE_PTR; } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!pkt) return;
    __try {
        memcpy(pkt, g_pending.bytes, 0x14);
        if (!g_pending.raw)                                    // copyx: shift X;
            *(uint16_t*)(pkt + 0x08) = (uint16_t)g_pending.newx;
        // raw (fillgap): emit the prebuilt flat rect verbatim.
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    g_orig_submit(layer, pktSize);
}

// Edit the already-filled packet in place to match an override (before forwarding).
static void apply_fix(const Rect& r) {
    std::lock_guard<std::mutex> lk(g_ovr_mtx);
    for (const Ovr& o : g_ovr) {
        if (o.mu >= 0 && (unsigned)o.mu != r.u) continue;
        if (o.mv >= 0 && (unsigned)o.mv != r.v) continue;
        if (o.mclut >= 0 && (unsigned)o.mclut != r.clut) continue;
        if (o.mw >= 0 && o.mw != r.w) continue;
        if (o.mx != INT32_MIN && o.mx != r.x) continue;   // per-instance split
        if (o.my != INT32_MIN && o.my != r.y) continue;
        if (o.msig != 0 && caller_sig(r.ra, r.nra) != o.msig) continue;  // draw-site scope
        if (o.mra != 0) {                                  // gate on draw-call site
            bool hit = false;
            for (int i = 0; i < r.nra; i++) if (r.ra[i] == o.mra) { hit = true; break; }
            if (!hit) continue;
        }
        write_pkt(o, r);
        if (o.fillcol >= 0 && o.fillw > 0 && o.fillh > 0) {
            build_fill_pkt(o, g_pending.bytes);  // cover-the-gap flat fill, post-submit
            g_pending.raw    = true;
            g_pending.active = true;
        } else if (o.copyx != INT32_MIN && snapshot_pkt(g_pending.bytes)) {
            g_pending.newx   = r.x + o.copyx;   // duplicate, shifted right, post-submit
            g_pending.raw    = false;
            g_pending.active = true;
        }
        return;   // first match wins — put mx/my rules BEFORE the catch-all
    }
}

// DIAGNOSTIC: box-frame tiles use CLUTs 0x3C41..0x3C46. Log the DISTINCT
// immediate caller (the function drawing the box) for each, so we can find the
// draw site of any menu box — including the ~100 that are drawn INLINE and never
// go through the 0x677B50/0x677E30 shared drawers. Dedup by (caller): one line
// per box-draw function. Gated on rect_tuner so it's only on while identifying.
static void diag_box_caller(const Rect& r) {
    if ((r.clut & 0xFFF8) != 0x3C40) return;     // 0x3C40..0x3C47 = box tileset cluts
    if (r.nra < 1) return;
    unsigned caller = r.ra[0];
    static std::unordered_set<unsigned> seen;
    if (!seen.insert(caller).second) return;
    const char* kind = (caller >= 0x00677A00 && caller <= 0x00679000) ? " (SHARED drawer region)"
                                                                      : " (inline/menu code)";
    hook_log("[boxdraw] caller=0x%08X  @(%d,%d) w=%d clut=0x%04X%s\n",
             caller, r.x, r.y, r.w, r.clut, kind);
}

// ── box + inner-text identification ──────────────────────────────────────────
// Both box tiles (clut 0x3C4x) and glyphs (GP0 cmd 0x84) flow through this same
// submit, so we can label each box by the text sitting inside it — no extra hook.
// Per frame: collect box tiles (with their draw-site caller) and glyphs (pos +
// char), then rect_box_text_hud() groups tiles by caller, reads the glyphs inside
// each group's bounds, and prints "caller=0x.. @(x,y) w=.. 'Ausrüstung'". That
// caller is stable across sessions → widen a single-tile box with a rect_widths
// line: `caller=0xADDR u=.. v=.. clut=0x.. w=N`.
struct BTBox { unsigned caller; int x, y, w, h; unsigned u, v, clut; };
struct BTGly { int x, y; char c; };
static std::vector<BTBox> g_bt_box;
static std::vector<BTGly> g_bt_gly;
static std::mutex g_bt_mtx;

static void bt_note_box(const Rect& r) {
    if ((r.clut & 0xFFF8) != 0x3C40) return;         // box tileset cluts 0x3C40..0x3C47
    // Use the immediate caller r.ra[0] — the ONLY reliable frame (deeper stack
    // frames here are noisy/stale, e.g. a leftover pointer into unrelated code).
    // For inline boxes this is the real draw site; for shared-drawer boxes it's
    // inside the drawer (identify those via box_autofit focus mode instead).
    std::lock_guard<std::mutex> lk(g_bt_mtx);
    if (g_bt_box.size() < 256)
        g_bt_box.push_back({ r.nra > 0 ? r.ra[0] : 0u, r.x, r.y, r.w, r.h, r.u, r.v, r.clut });
}
// SEH-only read (no C++ objects here — MSVC forbids __try alongside unwinding).
static bool bt_read_glyph(int* gx, int* gy, char* c) {
    uint8_t* pkt = nullptr;
    __try { pkt = *(uint8_t**)PKT_WRITE_PTR; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!pkt) return false;
    uint8_t d[0x18];
    __try { memcpy(d, pkt, sizeof(d)); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if ((d[0x07] & 0xFC) != 0x84) return false;      // glyph cmds 0x84..0x87
    // Glyph packets store x at +8 and y at +0xC as SEPARATE values (the renderer
    // 0x4084B0 fadd's them as floats) — NOT the packed y<<16|x that rects use.
    // Decode each as int-or-float: small magnitude = int coord, else float bits.
    auto dec = [](uint32_t raw) -> int {
        int32_t i = (int32_t)raw;
        if (i > -4096 && i < 4096) return i;         // plain integer coordinate
        float f; memcpy(&f, &raw, 4);
        if (f > -4096.0f && f < 4096.0f) return (int)f;  // float coordinate
        return i;
    };
    *gx = dec(*(uint32_t*)(d + 0x08));
    *gy = dec(*(uint32_t*)(d + 0x0C));
    unsigned gidx = d[0x16];                          // glyph index (= char - 0x20)
    *c = (gidx < 0x60) ? (char)(gidx + 0x20) : '?';
    return true;
}
static void bt_try_glyph() {
    int gx, gy; char c;
    if (!bt_read_glyph(&gx, &gy, &c)) return;
    std::lock_guard<std::mutex> lk(g_bt_mtx);
    if (g_bt_gly.size() < 1024) g_bt_gly.push_back({ gx, gy, c });
}

// Group boxes by caller, read the text inside each, format for the HUD; then clear.
extern "C" int rect_box_text_hud(char* out, int cap) {
    std::lock_guard<std::mutex> lk(g_bt_mtx);
    int len = 0;
    len += snprintf(out + len, cap - len, "BOXES + TEXT  (glyphs=%d)  widen via rect_widths.txt:",
                    (int)g_bt_gly.size());
    std::unordered_set<unsigned> done;
    int shown = 0;
    for (const BTBox& b : g_bt_box) {
        if (b.caller == 0 || !done.insert(b.caller).second) continue;
        // Skip the tile-strip / cube drawer 0x678890 (not a window box).
        if (b.caller >= 0x00678890 && b.caller <= 0x00678A80) continue;
        // union bounds of every tile from this caller
        int x0 = b.x, y0 = b.y, x1 = b.x + b.w, y1 = b.y + b.h;
        for (const BTBox& t : g_bt_box)
            if (t.caller == b.caller) {
                if (t.x < x0) x0 = t.x; if (t.y < y0) y0 = t.y;
                if (t.x + t.w > x1) x1 = t.x + t.w; if (t.y + t.h > y1) y1 = t.y + t.h;
            }
        if (y1 - y0 < 14) continue;          // skip thin strips/bars — keep real boxes
        // gather glyphs inside the bounds, sorted by ROW (y) then x, so multi-line
        // boxes read line-by-line (rows separated by '/') instead of x-interleaved.
        struct GX { int x, y; char c; } gg[64]; int gn = 0;
        for (const BTGly& g : g_bt_gly)
            if (g.c > 0x20 && g.x >= x0 - 4 && g.x <= x1 + 2 && g.y >= y0 - 4 && g.y <= y1 + 2 && gn < 64)
                gg[gn++] = { g.x, g.y, g.c };
        for (int a = 1; a < gn; a++) {       // sort by (y-band of 6px, then x)
            GX t = gg[a]; int j = a - 1;
            while (j >= 0 && (gg[j].y / 6 > t.y / 6 || (gg[j].y / 6 == t.y / 6 && gg[j].x > t.x)))
                { gg[j+1] = gg[j]; j--; }
            gg[j+1] = t;
        }
        char txt[56]; int tn = 0; int prow = gn ? gg[0].y / 6 : 0;
        for (int a = 0; a < gn && tn < (int)sizeof(txt) - 2; a++) {
            if (gg[a].y / 6 != prow) { txt[tn++] = '/'; prow = gg[a].y / 6; }
            txt[tn++] = gg[a].c;
        }
        txt[tn] = 0;
        if (len < cap - 96 && shown < 12) {
            len += snprintf(out + len, cap - len, "\ncaller=0x%08X u=%d v=%d clut=0x%04X w=%d '%s'",
                            b.caller, b.u, b.v, b.clut, x1 - x0, txt);
            shown++;
        }
    }
    if (shown == 0) len += snprintf(out + len, cap - len, "\n(no window boxes this frame)");
    g_bt_box.clear(); g_bt_gly.clear();
    return len;
}

// ── hook ────────────────────────────────────────────────────────────────────
static int __cdecl h_submit(int layer, int pktSize) {
    g_submit_call_count.fetch_add(1, std::memory_order_relaxed);
    if (g_cfg.rect_trace || g_cfg.rect_fix || g_cfg.rect_tuner || g_cfg.tex_prov_log) {
        uint32_t* sp = (uint32_t*)_AddressOfReturnAddress();  // our return slot
        if (g_cfg.rect_tuner) bt_try_glyph();   // glyphs fail read_packet; capture them here
        Rect r;
        if (read_packet(r, layer)) {
            capture_callers(sp, r.ra, &r.nra);
            if (g_cfg.rect_tuner) { diag_box_caller(r); bt_note_box(r); }
            if (g_cfg.rect_fix || g_cfg.rect_tuner) apply_fix(r);
            if (g_cfg.rect_tuner) {
                // Feed the tuner AFTER apply_fix so it reads the effective (post-
                // override) packet as its base and stacks its live delta on top.
                // sig identifies WHICH box/menu emitted this rect (see caller_sig).
                unsigned sig = caller_sig(r.ra, r.nra);
                TunerRect tr{ r.x, r.y, r.w, r.h, r.u, r.v, r.clut, r.cmd,
                              (r.nra > 0 ? r.ra[0] : 0u),
                              (r.nra > 1 ? r.ra[1] : 0u), sig };
                rect_tuner_on_submit(tr);
            }
            if (g_cfg.rect_trace) trace_rect(r);
        }
    }
    int ret = g_orig_submit(layer, pktSize);
    if (g_pending.active) {           // re-emit the shifted copy AFTER the original
        g_pending.active = false;
        emit_pending(layer, pktSize);
    }
    return ret;
}

// ── hotkeys: F5 = reload, F6 = new-screen tag, F7 = dump boxes, F8 = dump labels,
//            F11 = clear/re-snapshot ───────────────────────────────────────────
// (F9 is intentionally avoided — the game uses it for a quit-confirm popup.)
// Organic flow on each screen: F6 (mark) → F7 (boxes) → F8 (labels).
static volatile bool g_quit = false;
static void do_reload() {
    if (g_cfg.rect_fix)      rect_reload_overrides();   // universal lines (live)
    if (g_cfg.menu_rect_fix) menu_rect_fix_apply();     // title .text re-patch
    hook_log("[rect] F5: reloaded rect_widths.txt (rect_fix=%d menu_rect_fix=%d)\n",
             (int)g_cfg.rect_fix, (int)g_cfg.menu_rect_fix);
}
// F7: start a new scene group. Rects first seen from now on are stamped with the
// new epoch, so a single F9 dump (after playing through several screens) comes out
// grouped under per-scene headers instead of one flat blob.
static void do_scene_tag() {
    bof4_reload_dat_whitelist();                  // refresh editable-DAT list from disk
    long n = InterlockedIncrement(&g_scene_epoch);
    hook_log("[rect] F6: new-screen tag -> %ld (new labels grouped under this scene; "
             "F7 = boxes, F8 = labels)\n", n);
}
static DWORD WINAPI hotkey_thread(LPVOID) {
    bool prev5 = false, prev6 = false, prev7 = false, prev8 = false, prev11 = false;
    while (!g_quit) {
        bool now5  = (GetAsyncKeyState(VK_F5)  & 0x8000) != 0;
        bool now6  = (GetAsyncKeyState(VK_F6)  & 0x8000) != 0;
        bool now7  = (GetAsyncKeyState(VK_F7)  & 0x8000) != 0;
        bool now8  = (GetAsyncKeyState(VK_F8)  & 0x8000) != 0;
        bool now11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
        if (now5  && !prev5)  do_reload();                              // reload widths
        if (g_cfg.rect_trace) {
            if (now6  && !prev6)  do_scene_tag();                       // F6 = new screen
            if (now7  && !prev7)  dump_inventory("rect_boxes.txt");      // F7 = boxes
            if (now8  && !prev8)  dump_inventory("rect_inventory.txt");  // F8 = labels
            if (now11 && !prev11) clear_inventory();
        }
        prev5 = now5; prev6 = now6; prev7 = now7; prev8 = now8; prev11 = now11;
        Sleep(30);
    }
    return 0;
}

void rect_hook_install() {
    if (!g_cfg.rect_trace && !g_cfg.rect_fix && !g_cfg.rect_tuner && !g_cfg.tex_prov_log) {
        hook_log("rect_hook: disabled — not installed\n");
        return;
    }
    // The tuner rides this same hook: it applies its saved overrides (from
    // rect_tuner.txt, merged into rect_widths.txt loading below) and edits the
    // focused rect live, so treat rect_tuner like rect_fix for the override path.
    if (g_cfg.rect_fix || g_cfg.rect_tuner) load_overrides();

    { uintptr_t a = resolve_sig("rect_hook", SIG_SUBMIT, SIG_SUBMIT_MASK,
                                sizeof(SIG_SUBMIT), 0);
      if (a) {
          ADDR_SUBMIT = a;
          // Derive the packet-write global from the mov ecx,[imm32] whose
          // imm sits at submit+6 — so it relocates with the EXE. Sanity-gate
          // to a plausible .data address before trusting it.
          uint32_t derived = 0;
          __try { derived = *(uint32_t*)(ADDR_SUBMIT + 6); }
          __except (EXCEPTION_EXECUTE_HANDLER) { derived = 0; }
          if (derived >= 0x00600000 && derived < 0x01000000) {
              PKT_WRITE_PTR = derived;
              hook_log("rect_hook: packet-write ptr derived @ 0x%08X\n",
                       (unsigned)PKT_WRITE_PTR);
          } else {
              hook_log("rect_hook: packet-ptr derive rejected (0x%08X) — "
                       "keeping 0x%08X\n", derived, (unsigned)PKT_WRITE_PTR);
          }
      }
    }
    MH_STATUS st = MH_CreateHook((LPVOID)ADDR_SUBMIT, (LPVOID)h_submit, (LPVOID*)&g_orig_submit);
    if (st != MH_OK) { hook_log("rect_hook: MH_CreateHook(0x%08X) failed: %d\n", (unsigned)ADDR_SUBMIT, (int)st); return; }
    st = MH_EnableHook((LPVOID)ADDR_SUBMIT);
    if (st != MH_OK) { hook_log("rect_hook: MH_EnableHook failed: %d\n", (int)st); return; }

    if (g_cfg.rect_trace || g_cfg.rect_fix)
        CreateThread(nullptr, 0, hotkey_thread, nullptr, 0, nullptr);
    hook_log("rect_hook: submit 0x%08X hooked (trace=%d fix=%d). "
             "F5 = reload rect_widths.txt%s.\n",
             (unsigned)ADDR_SUBMIT, (int)g_cfg.rect_trace, (int)g_cfg.rect_fix,
             g_cfg.rect_trace ? ", F6 = new-screen tag, F7 = boxes, F8 = labels, "
                                "F11 = clear/re-snapshot" : "");
}
