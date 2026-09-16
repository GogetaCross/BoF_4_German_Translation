// narration_uncap.cpp — see narration_uncap.h.
//
// Mechanism (per handler, all cdecl `void(State*)`):
//   original:  cnt=[st+0xA]; if(cnt<CAP){ if(++[st+6]>3){ [st+6]=0; if(++cnt==CAP)
//                 [st+0xC0]++; [st+0xA]=cnt; } }
//              render line i with reveal = clamp(cnt - delay_i, 0..255)
//   ours:      identical, but CAP is replaced by a per-frame TARGET =
//                 delay_last + vis_len(resolve(last_msg)) + 1
//   so the PC advance ([st+0xC0]++) fires the moment the last line's reveal
//   budget covers its whole (translated) string — never before (no truncation),
//   never much after (no drag; at most one 4-frame tick of hold, as in stock).
//
// vis_len mirrors the stock text renderer's per-unit budget consumption
// (0x00629470): every processed byte costs 1, EXCEPT the name code 0x04 which
// costs 1 (the code) + the expanded name's glyphs. The byte after 0x04 is the
// name-slot index k (NOT a terminator); the name string lives at
// 0x00B55800 + 152*k (derived from the renderer's own name handler at
// 0x0062966C). We read the player's actual name length so pacing is exact.
//
// Function VAs are the canonical GOG build (BOF4.exe 7f83d1d5, ImageBase
// 0x400000; identical VAs on the Steam/Enigma port). resolve() = 0x005279A0,
// render() = 0x00629470 — both cdecl, caller-cleaned (verified from the callers'
// single `add esp,0x48` cleaning resolver arg + 5 renderer args per line).

#include "hooks.h"
#include "hook_health.h"
#include "narration_uncap.h"
#include "minhook/MinHook.h"

#include <cstdint>
#include <cstdio>
#include <windows.h>

namespace {

typedef char* (__cdecl* resolver_t)(int msg_id);
typedef void  (__cdecl* renderer_t)(int x, int y, int a2, int count, const char* s);

constexpr uintptr_t RESOLVER_VA  = 0x005279A0;
constexpr uintptr_t RENDERER_VA  = 0x00629470;
constexpr uintptr_t NAME_BUF_VA  = 0x00B55800;   // name slot 0; stride 152
constexpr unsigned  NAME_STRIDE  = 152;
constexpr int       NAME_FALLBACK = 6;           // BoF4 max name length

resolver_t g_resolve = reinterpret_cast<resolver_t>(RESOLVER_VA);
renderer_t g_render  = reinterpret_cast<renderer_t>(RENDERER_VA);

// Actual glyph count of the name in slot k (null-terminated in the name buffer).
int name_len(unsigned k) {
    if (k > 15) return NAME_FALLBACK;                    // implausible index
    const unsigned char* p =
        reinterpret_cast<const unsigned char*>(NAME_BUF_VA + NAME_STRIDE * k);
    int n = 0;
    while (n < 32 && p[n]) ++n;
    return n ? n : NAME_FALLBACK;                         // empty -> assume max
}

// Reveal budget the renderer needs to fully show a string (matches 0x629470).
int vis_len(const unsigned char* s) {
    if (!s) return 0;
    int u = 0;
    for (int guard = 0; guard < 8192; ++guard) {
        unsigned char b = *s++;
        if (b == 0x00) break;                            // real terminator
        if (b == 0x04) { unsigned k = *s++; u += 1 + name_len(k); continue; }
        u += 1;                                           // glyph / space / \x01
    }
    return u;
}

struct NarrDef {
    uintptr_t     va;
    int           nlines;
    uint8_t       msgid[3];
    uint8_t       delay[3];
    uint16_t      y[3];
    const uint8_t* sig;
    size_t        siglen;
    void*         detour;
    void*         orig;          // MinHook trampoline (created, never called)
};

// Core reimplementation, shared by all four handlers.
void narr_run(void* state, const NarrDef* d) {
    unsigned char* st = static_cast<unsigned char*>(state);

    // Resolve each line once; compute the dynamic advance target as the point at
    // which EVERY line has finished revealing — max over lines of
    // (delay_i + vis_len_i) + 1. (Using the last line alone would clip a middle
    // line that a translation made longer than the last.)
    char* line[3] = {nullptr, nullptr, nullptr};
    int target = 1;
    for (int i = 0; i < d->nlines; ++i) {
        line[i] = g_resolve(d->msgid[i]);
        int done = static_cast<int>(d->delay[i]) +
                   vis_len(reinterpret_cast<const unsigned char*>(line[i])) + 1;
        if (done > target) target = done;
    }
    if (target > 0xFF) target = 0xFF;           // byte counter ceiling: no softlock

    unsigned char cnt = st[0x0A];
    if (cnt < static_cast<unsigned char>(target)) {
        unsigned char sub = static_cast<unsigned char>(st[0x06] + 1);
        st[0x06] = sub;
        if (sub > 3) {                          // +1 revealed char every 4 ticks
            ++cnt;
            st[0x06] = 0;
            st[0x0A] = cnt;
            if (cnt == static_cast<unsigned char>(target))
                *reinterpret_cast<uint32_t*>(st + 0xC0) += 1;   // advance scene PC
        }
    }

    // Render each line — byte-identical args to the stock handler (X=0x38, a2=8).
    const int c = st[0x0A];
    for (int i = 0; i < d->nlines; ++i) {
        int rv = c - static_cast<int>(d->delay[i]);
        if (rv < 0) rv = 0; else if (rv > 0xFF) rv = 0xFF;
        g_render(0x38, d->y[i], 8, rv, line[i]);
    }
}

// Distinct cdecl detour per handler (MinHook needs a unique target each).
void __cdecl h0(void*); void __cdecl h1(void*);
void __cdecl h2(void*); void __cdecl h3(void*);

// Entry signatures: prologue + `mov cl,[eax+0xA]` + `cmp cl,ORIG_CAP`.
const uint8_t SIG0[] = {0x8B,0x44,0x24,0x04,0x53,0x56,0x57,0x8A,0x48,0x0A,0x80,0xF9,0x74};
const uint8_t SIG1[] = {0x8B,0x44,0x24,0x04,0x53,0x56,0x57,0x8A,0x48,0x0A,0x80,0xF9,0x4F};
const uint8_t SIG2[] = {0x8B,0x44,0x24,0x04,0x8A,0x48,0x0A,0x80,0xF9,0x38};
const uint8_t SIG3[] = {0x8B,0x44,0x24,0x04,0x8A,0x48,0x0A,0x80,0xF9,0x51};

NarrDef g_defs[4] = {
    // handler          n  msg ids          delays          Y positions       sig
    {0x0048DF00, 3, {0x40,0x41,0x42}, {0x00,0x16,0x5D}, {0x50,0x5E,0x88}, SIG0, sizeof SIG0, (void*)h0, nullptr},
    {0x0048DFD0, 3, {0x44,0x45,0x46}, {0x00,0x1C,0x35}, {0x60,0x6E,0x7C}, SIG1, sizeof SIG1, (void*)h1, nullptr},
    {0x0048E190, 2, {0x4F,0x50,0x00}, {0x00,0x15,0x00}, {0x60,0x6E,0x00}, SIG2, sizeof SIG2, (void*)h2, nullptr},
    {0x0046F300, 2, {0x51,0x52,0x00}, {0x00,0x23,0x00}, {0x60,0x6E,0x00}, SIG3, sizeof SIG3, (void*)h3, nullptr},
};

void __cdecl h0(void* s) { narr_run(s, &g_defs[0]); }
void __cdecl h1(void* s) { narr_run(s, &g_defs[1]); }
void __cdecl h2(void* s) { narr_run(s, &g_defs[2]); }
void __cdecl h3(void* s) { narr_run(s, &g_defs[3]); }

} // namespace

void narration_uncap_install() {
    int ok = 0;
    const int total = 4;
    for (int i = 0; i < total; ++i) {
        NarrDef& d = g_defs[i];
        char tag[40];
        snprintf(tag, sizeof tag, "narration_uncap.h%d", i);
        if (!health_check_bytes(tag, d.va, d.sig, d.siglen)) {
            health_record(tag, false, "sig mismatch (skipped)");
            continue;
        }
        if (MH_CreateHook((LPVOID)d.va, d.detour, &d.orig) == MH_OK &&
            MH_EnableHook((LPVOID)d.va) == MH_OK) {
            ++ok;
            health_record(tag, true, "advance-on-complete");
            hook_log("narration_uncap: handler %d @0x%08X hooked "
                     "(msg %#x, %d lines)\n",
                     i, (unsigned)d.va, d.msgid[0], d.nlines);
        } else {
            health_record(tag, false, "MinHook failed");
            hook_log("narration_uncap: MinHook FAILED @0x%08X\n", (unsigned)d.va);
        }
    }
    hook_log("narration_uncap: %d/%d ending-narration handlers now "
             "advance-on-complete (typewriter caps removed; no truncation, "
             "stock pacing)\n", ok, total);
}
