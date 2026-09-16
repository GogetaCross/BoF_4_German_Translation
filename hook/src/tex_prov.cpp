// Texture provenance — "which DAT recNN is this rect from?" for the Rect Tuner.
//
// The PC port has no texpage in the rect queue (confirmed 2026-08-01), so we can't
// turn a rect's page-relative u/v into an absolute VRAM x. But op1 texture uploads
// ARE observable: LoadImage 0x4026E0(RECT{s16 x,y,w,h}, src) blits 32x32-word
// blocks into emulated VRAM (base 0x00973FD4), and each op1 rec's VRAM rectangle
// is FIXED by its record `param`. So:
//   * an offline table (build_vram_rec_map.py -> vram_rec_map.tsv) lists every
//     op1 rec's VRAM rect: DAT, rec, x_word, y_row, w_word, h_row;
//   * at runtime we hook the upload and stamp a VRAM block-ownership grid
//     (last-writer-wins) with the (DAT, rec) that currently occupies each block;
//   * the tuner asks resolve(v) for the DAT/recs whose VRAM spans the focused
//     rect's v-band (both texpage-Y halves, since Y base is 0 or 256), plus the
//     full resident-DAT set. That's "source DAT + candidate recs" — the exact rec
//     within the band you confirm in the graphics tool.
//
// Enabled whenever rect_tuner or tex_prov_log is on. tex_prov_log additionally
// writes the raw upload log (tex_provenance.log) used to build/validate the table.

#include "hooks.h"
#include "tex_prov.h"
#include "minhook/MinHook.h"

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <string>
#include <vector>
#include <mutex>
#include <intrin.h>   // _ReturnAddress

void bof4_scene_dat(char* out, size_t cap);    // whitelisted scene DAT (bof4_hooks.cpp)
void bof4_recent_dat0(char* out, size_t cap);  // raw most-recent *.DAT (bof4_hooks.cpp)

static constexpr uintptr_t ADDR_LOADIMAGE = 0x004026E0;   // op1 VRAM upload

typedef void (__cdecl *PFN_LoadImage)(void* rect, void* src);
static PFN_LoadImage g_orig_loadimage = nullptr;

// ── offline VRAM->rec table (vram_rec_map.tsv, next to BOF4.exe) ──────────────
struct MapEnt { char dat[28]; int rec, x, y, w, h; };
static std::vector<MapEnt> g_map;

// ── VRAM block-ownership grid: 32px blocks over 1024x512 -> 32 cols x 16 rows ─
static constexpr int GC = 32, GR = 16, GN = GC * GR;
static char     g_owner_dat[GN][28];
static int      g_owner_rec[GN];
static unsigned g_owner_seq[GN];
static unsigned g_seq = 0;
static std::mutex g_mtx;

// raw upload log (tex_prov_log only)
static FILE* g_log = nullptr;
static int   g_log_count = 0;
static const int LOG_MAX = 8000;

static void exe_sidecar(char* out, size_t n, const char* leaf) {
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char* s = strrchr(exe, '\\'); if (s) *(s + 1) = 0; else exe[0] = 0;
    snprintf(out, n, "%s%s", exe, leaf);
}

static void load_map() {
    char path[MAX_PATH]; exe_sidecar(path, sizeof(path), "vram_rec_map.tsv");
    FILE* f = fopen(path, "r");
    if (!f) { hook_log("tex_prov: %s not found — run build_vram_rec_map.py "
                       "(HUD source line will be empty)\n", path); return; }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        MapEnt e{}; char dat[64];
        if (sscanf(line, "%63[^\t]\t%d\t%d\t%d\t%d\t%d", dat, &e.rec, &e.x, &e.y, &e.w, &e.h) == 6) {
            strncpy_s(e.dat, sizeof(e.dat), dat, _TRUNCATE);
            g_map.push_back(e);
        }
    }
    fclose(f);
    hook_log("tex_prov: loaded %zu op1-rec VRAM rects from vram_rec_map.tsv\n", g_map.size());
}

// Find the op1 rec of `dat` whose VRAM rect contains word (x,y). -1 if none.
static int map_rec_at(const char* dat, int x, int y) {
    for (const MapEnt& e : g_map)
        if (_stricmp(e.dat, dat) == 0 && x >= e.x && x < e.x + e.w &&
            y >= e.y && y < e.y + e.h)
            return e.rec;
    return -1;
}

// SEH-guarded read of the 4 s16 VRAM-rect fields.
static bool read_rect(const void* rectp, int* x, int* y, int* w, int* h) {
    if (!rectp) return false;
    __try {
        const int16_t* r = (const int16_t*)rectp;
        *x = r[0]; *y = r[1]; *w = r[2]; *h = r[3];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void __cdecl hook_loadimage(void* rect, void* src) {
    if (g_orig_loadimage) g_orig_loadimage(rect, src);   // forward first
    bool tracking = g_cfg.rect_tuner || g_cfg.tex_prov_log;
    if (!tracking) return;

    int x = 0, y = 0, w = 0, h = 0;
    if (!read_rect(rect, &x, &y, &w, &h)) return;
    char dat[28] = {0}; bof4_recent_dat0(dat, sizeof(dat));

    std::lock_guard<std::mutex> lk(g_mtx);
    // Stamp the block-ownership grid (this upload's 32x32 block).
    if (dat[0] && x >= 0 && y >= 0 && x < 1024 && y < 512) {
        int rec = map_rec_at(dat, x, y);
        int col = x / 32, row = y / 32;
        if (col >= 0 && col < GC && row >= 0 && row < GR) {
            int gi = row * GC + col;
            strncpy_s(g_owner_dat[gi], sizeof(g_owner_dat[gi]), dat, _TRUNCATE);
            g_owner_rec[gi] = rec;
            g_owner_seq[gi] = ++g_seq;
        }
    }
    // Raw upload log (validation / table building).
    if (g_cfg.tex_prov_log) {
        if (g_log_count++ > LOG_MAX) return;
        if (!g_log) {
            g_log = fopen("tex_provenance.log", "w");
            if (g_log) fprintf(g_log,
                "# LoadImage(0x4026E0) uploads. VRAM base 0x00973FD4 (word/row units).\n"
                "#  idx   x     y     w    h    src        ra        dat\n");
        }
        if (g_log) {
            unsigned ra = (unsigned)(uintptr_t)_ReturnAddress();
            fprintf(g_log, "  %-4d  %-5d %-5d %-4d %-4d  %p  0x%06X  %s\n",
                    g_log_count, x, y, w, h, src, ra, dat[0] ? dat : "(?)");
            fflush(g_log);
        }
    }
}

// Append "DAT recN" (once per distinct DAT/rec) to a bounded, comma-joined buffer.
static void append_owner(char* out, size_t n, size_t& len,
                         const char* dat, int rec, int maxItems, int& count) {
    if (count >= maxItems) return;
    // dedup within this call
    char frag[40];
    int stem = (int)(strchr(dat, '.') ? strchr(dat, '.') - dat : (int)strlen(dat));
    if (rec >= 0) snprintf(frag, sizeof(frag), "%.*s r%d", stem, dat, rec);
    else          snprintf(frag, sizeof(frag), "%.*s r?", stem, dat);
    // (caller already deduped by (dat,rec); no strstr guard — it would falsely
    //  swallow "r1" when "r15" is already present.)
    len += snprintf(out + len, (len < n ? n - len : 0), "%s%s", len ? "  " : "", frag);
    count++;
}

// Fill `out` with the source attribution for a focused rect whose atlas V is `v`.
// Line = candidate recs whose VRAM y-band matches v (both texpage-Y halves), most
// recently uploaded first. Returns false if we have nothing (map/uploads absent).
bool tex_prov_resolve(int v, char* out, size_t n) {
    if (!out || n == 0) return false;
    out[0] = 0;
    if (g_map.empty()) return false;
    std::lock_guard<std::mutex> lk(g_mtx);

    // v (texel row) -> the two candidate VRAM block-rows (texpage Y base 0 or 256).
    int br0 = (v & 0xFF) / 32;
    int br1 = (256 + (v & 0xFF)) / 32;

    // Collect owners in those two block-rows, sorted by recency (seq desc). Simple
    // selection since the set is tiny (<= 2 rows x 32 cols).
    struct Cand { char dat[28]; int rec; unsigned seq; };
    Cand c[GC * 2]; int nc = 0;
    for (int br : { br0, br1 }) {
        if (br < 0 || br >= GR) continue;
        for (int col = 0; col < GC; col++) {
            int gi = br * GC + col;
            if (!g_owner_seq[gi] || !g_owner_dat[gi][0]) continue;
            // dedup (dat,rec)
            bool dup = false;
            for (int i = 0; i < nc; i++)
                if (c[i].rec == g_owner_rec[gi] && _stricmp(c[i].dat, g_owner_dat[gi]) == 0) { dup = true; break; }
            if (dup) continue;
            strncpy_s(c[nc].dat, sizeof(c[nc].dat), g_owner_dat[gi], _TRUNCATE);
            c[nc].rec = g_owner_rec[gi]; c[nc].seq = g_owner_seq[gi]; nc++;
        }
    }
    // insertion sort by seq desc
    for (int i = 1; i < nc; i++) { Cand t = c[i]; int j = i - 1;
        while (j >= 0 && c[j].seq < t.seq) { c[j + 1] = c[j]; j--; } c[j + 1] = t; }

    size_t len = 0; int count = 0;
    for (int i = 0; i < nc; i++)
        append_owner(out, n, len, c[i].dat, c[i].rec, 6, count);
    return out[0] != 0;
}

void tex_prov_install() {
    if (!g_cfg.rect_tuner && !g_cfg.tex_prov_log) return;
    for (int i = 0; i < GN; i++) { g_owner_dat[i][0] = 0; g_owner_rec[i] = -1; g_owner_seq[i] = 0; }
    load_map();
    void* t = (void*)ADDR_LOADIMAGE;
    MH_STATUS st = MH_CreateHook(t, (void*)hook_loadimage, (void**)&g_orig_loadimage);
    if (st == MH_OK) st = MH_EnableHook(t);
    if (st == MH_OK)
        hook_log("tex_prov: LoadImage 0x%08X hooked (VRAM->rec provenance%s)\n",
                 (unsigned)ADDR_LOADIMAGE, g_cfg.tex_prov_log ? " + upload log" : "");
    else
        hook_log("tex_prov: FAILED to hook LoadImage 0x%08X (st=%d)\n",
                 (unsigned)ADDR_LOADIMAGE, (int)st);
}
