// BoF4 BGM-loop diagnostic hooks (Phase 1: logging-only)
//
// See bgm_loop.h for design overview. Output goes to d3d9_hook.log via the
// shared hook_log() entry point.

#include "bgm_loop.h"
#include "hooks.h"
#include "MinHook.h"

#include <windows.h>
#include <psapi.h>
#include <intrin.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#pragma intrinsic(_ReturnAddress)
#pragma comment(lib, "psapi.lib")

// ── dshow.dll target VAs ──────────────────────────────────────────────────
// Discovered by static analysis (see music_loop/dshow_vtable.txt and
// project_bgm_loop.md). All are dshow.dll-internal — we resolve them at
// install time by adding the dshow.dll runtime base to the RVAs below.

static constexpr uintptr_t DSHOW_RVA_PLAY_OPEN  = 0x000C4110; // central ffmpeg
                                                              // open-stream
                                                              // wrapper.
static constexpr uintptr_t DSHOW_RVA_REPLAY_VT  = 0x000C1AF0; // 12-byte object
                                                              // vtable[16] —
                                                              // copies 200B
                                                              // descriptor +
                                                              // calls 4110.

// dinput.dll's GOG-customized CreateFileA wrapper. Takes 7 stdcall args
// (same shape as KERNEL32!CreateFileA), copies filename to a local buffer,
// calls kernel32!CreateFileA. We hook this so _ReturnAddress() lands inside
// dinput.dll's BGM streaming code (the kernel32-level hook only saw the
// wrapper's return slot at +0x60989, not the real caller).
static constexpr uintptr_t DINPUT_RVA_MYCREATEFILEA = 0x000608E0;

// BOF4.exe statically-linked CRT _fsopen-style function. The kernel32
// CreateFileA hook saw the call chain bottoming out at 0x29ECA0 because
// CaptureStackBackTrace can't unwind through the CRT's FPO frames.
// Hooking _fsopen at its entry lets us read [esp] directly — that's the
// BGM-side caller without any stack walking. __cdecl, args: (path, mode,
// shflag). Returns FILE*.
static constexpr uintptr_t BOF4_RVA_FSOPEN = 0x0029EC7F;

// fopen(path, mode) — thin wrapper that calls _fsopen(path, mode, 0x40).
// Hooking this gets us the BGM caller directly via _ReturnAddress().
static constexpr uintptr_t BOF4_RVA_FOPEN = 0x0029ECB0;

// 0x695030 = __stdcall int decode_frame(decoder, dest). Passing dest=NULL
// makes the decoder substitute its internal scratch buffer — output is
// discarded but the decoder's IMDCT state advances. We use this for
// "look-back" priming after our loop seek.
static constexpr uintptr_t BOF4_RVA_DECODE_FRAME = 0x00295030;

// Number of frames to decode-and-discard after our seek-on-loop, so the
// MP3 decoder's IMDCT state is settled before the engine reads "real"
// frames into its staging buffer. 2 is conservative; LAME bit-reservoir
// can reach back ~9 frames in extreme cases but BoF4's CBR 128k MP3s
// don't use deep reservoirs.
static constexpr int  BGM_PRIMING_SKIP_FRAMES = 2;

// ── BGM loop-fix hook targets ─────────────────────────────────────────────
// 0x40FC20 — BGM play entry. __cdecl(mp3_ptr, mp3_len, loop_flag) -> slot.
// 0x40FDB0 — per-slot decode loop. Contains the EOF + seek-to-0 bug.
// 0x40FDF8 — RA after `call 0x693F90` inside the EOF branch (used to filter).
// 0x693F90 — seek_decoder(ctx, position).
// 0xABC5D8 — base of slot table; each slot is 0x18 bytes; slot[i].decoder
//            lives at 0xABC5D8 + i*0x18 + 8 = 0xABC5E0 + i*0x18.
static constexpr uintptr_t BOF4_RVA_PLAY        = 0x0000FC20;
static constexpr uintptr_t BOF4_RVA_SEEK        = 0x00293F90;
static constexpr uintptr_t BOF4_RVA_EOF_CALL_RA = 0x0000FDF8;
static constexpr uintptr_t BOF4_RVA_SLOT_TABLE  = 0x006BC5D8;  // ABC5D8 - 0x400000
// Loader (0x66DA20) buffer table: `*(void**)(0xBAE2C0 + mode*8)` is the malloc'd
// buffer holding [N*16 track records][concatenated MP3 streams] (= a copy of the
// DAT from file offset 0x10). For a played track, mp3_ptr == buffer + record.c3.
// This is how we find ANY track's record — the old backward-walk from mp3_ptr
// only reached track 0's record (tracks 1+ have MB of MP3 between stream and
// table), which is why only the first track looped. RVA = 0xBAE2C0 - 0x400000.
static constexpr uintptr_t BOF4_RVA_BUF_TABLE   = 0x007AE2C0;
static constexpr int       BGM_MAX_MODE         = 16;   // buffer-table slots to scan
// 0x100C3A9F is mid-function; hooked indirectly via PostMessageA's IAT (the
// EOF block at that address is the only PostMessageA caller in dshow.dll's
// wrapper region).

static HMODULE   g_dshow      = nullptr;
static uintptr_t g_dshow_base = 0;
static uintptr_t g_dshow_end  = 0;

static HMODULE   g_dinput      = nullptr;
static uintptr_t g_dinput_base = 0;
static uintptr_t g_dinput_end  = 0;

static uintptr_t g_bof4_base   = 0;

// ── Detour signatures ─────────────────────────────────────────────────────
// 0x100C4110 is __thiscall (this in ECX, descriptor on stack, ret 4).
// MSVC __fastcall mirrors this for arg1 by putting it in ECX; we declare
// a dummy `edx_` so the third positional arg is the first stack arg.
typedef int   (__fastcall *fn_play_open_t)(void* this_, void* edx_,
                                            const void* descriptor);
// 0x100C1AF0 is __stdcall (this and descriptor BOTH on stack, ret 0xc).
// COM-style vtable dispatch: caller pushes `this` as a hidden first arg.
typedef int   (__stdcall  *fn_replay_vt_t)(void* this_,
                                            const void* descriptor, int arg3);

static fn_play_open_t g_orig_play_open = nullptr;
static fn_replay_vt_t g_orig_replay_vt = nullptr;

// ── PostMessageA hook (for EOF-block detection) ──────────────────────────
typedef BOOL (WINAPI *PostMessageA_t)(HWND, UINT, WPARAM, LPARAM);
static PostMessageA_t g_orig_PostMessageA = nullptr;

// ── dinput!MyCreateFileA wrapper ─────────────────────────────────────────
typedef HANDLE (WINAPI *MyCreateFileA_t)(LPCSTR, DWORD, DWORD,
                                          LPSECURITY_ATTRIBUTES,
                                          DWORD, DWORD, HANDLE);
static MyCreateFileA_t g_orig_my_createfilea = nullptr;

// ── BOF4!_fsopen ─────────────────────────────────────────────────────────
typedef void* (__cdecl *fsopen_t)(const char* path, const char* mode,
                                   int shflag);
static fsopen_t g_orig_fsopen = nullptr;

// ── BOF4!fopen ───────────────────────────────────────────────────────────
typedef void* (__cdecl *fopen_t)(const char* path, const char* mode);
static fopen_t g_orig_fopen = nullptr;

// ── BOF4!decode_frame ────────────────────────────────────────────────────
typedef int (__stdcall *decode_frame_t)(void* decoder, void* dest);
static decode_frame_t g_decode_frame = nullptr;  // resolved, not hooked

// ── BGM loop fix: play + seek ────────────────────────────────────────────
// 0x40FC20: __cdecl (caller cleans stack — bare `ret` at 0x40FD1C).
// 0x693F90: __stdcall (callee cleans 8 bytes — `ret 8` at 0x693FB2).
typedef int      (__cdecl   *bgm_play_t)(const void* mp3_ptr, int mp3_len,
                                          int loop_flag);
typedef int      (__stdcall *bgm_seek_t)(void* decoder_ctx, uint32_t position);
static bgm_play_t g_orig_bgm_play = nullptr;
static bgm_seek_t g_orig_bgm_seek = nullptr;

// Per-slot side-table: `g_loop_byte_per_slot[i]` is the byte offset of
// the col2_param-th MP3 frame (= the IDEAL loop-start position).
// `g_loop_seek_byte_per_slot[i]` is BGM_PRIMING_SKIP_FRAMES frames
// earlier — we seek there and decode-discard those frames so the
// decoder's IMDCT state is settled before the engine reads frame
// col2_param normally.
//
// Slots run 1..7 inside BOF4 (see 0x40FC20). Index 0 unused.
static uint32_t g_loop_byte_per_slot[8]      = { 0 };
static uint32_t g_loop_seek_byte_per_slot[8] = { 0 };
static std::atomic<uint64_t> g_n_play_hits  { 0 };
static std::atomic<uint64_t> g_n_seek_fixes { 0 };

// Compute the byte offset into an MPEG1 Layer 3 stream where frame index
// `target_frame` begins. Walks frames from `mp3` summing each frame's
// size. Returns 0 on parse failure (bad sync, unsupported format, OOB).
//
// MP3 frame size = (144 * bitrate_kbps * 1000 / sample_rate) + padding.
// BoF4 BGMs are all 128 kbps stereo 44.1 kHz with the padding bit set
// for one frame in 17 (frame size alternates between 417 and 418).
static uint32_t mp3_walk_to_frame(const uint8_t* mp3, int mp3_len,
                                  uint32_t target_frame) {
    static const int br_tab[16] = {
        0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0
    };
    static const int sr_tab[4] = { 44100, 48000, 32000, 0 };
    uint32_t off = 0;
    for (uint32_t i = 0; i < target_frame; ++i) {
        if ((int)(off + 4) > mp3_len) return 0;
        uint32_t hdr = ((uint32_t)mp3[off] << 24)
                     | ((uint32_t)mp3[off + 1] << 16)
                     | ((uint32_t)mp3[off + 2] << 8)
                     |  (uint32_t)mp3[off + 3];
        if ((hdr & 0xFFE00000u) != 0xFFE00000u) return 0;
        int version = (hdr >> 19) & 3;   // 3 = MPEG1
        int layer   = (hdr >> 17) & 3;   // 1 = Layer III
        if (version != 3 || layer != 1) return 0;
        int bri = (hdr >> 12) & 0xF;
        int sri = (hdr >> 10) & 3;
        int pad = (hdr >>  9) & 1;
        int br = br_tab[bri], sr = sr_tab[sri];
        if (br == 0 || sr == 0) return 0;
        uint32_t fsize = (uint32_t)((144 * br * 1000) / sr) + (uint32_t)pad;
        off += fsize;
    }
    return off;
}

// Given the MP3 stream pointer passed to BOF4!0x40FC20, compute the
// byte offset (within that stream) where playback should resume on loop.
//
// In RAM, mp3_ptr points to MP3 data INSIDE a malloc'd buffer. The track
// record sits at some 16B-aligned offset BEFORE mp3_ptr. The buffer is
// laid out as: [track table N*16 bytes] [concatenated MP3 streams].
// (The master header is NOT in this buffer — 0x66DA20 copies from
// buffer+0x10 onward, dropping the master.)
//
// To locate the matching track record, scan backward from mp3_ptr in
// 16-byte strides looking for the unique fingerprint:
//   col2 byte 1 == 0x6E   (the BoF4 BGM magic byte)
//   col4 == mp3_len       (segment length matches the arg we got)
// The record's col2 bytes 2..3 (u16 LE) is then col2_param = loop start
// in MP3 frames.
static uint32_t compute_loop_byte_from_mp3_ptr(const uint8_t* mp3_ptr,
                                                int mp3_len) {
    if (!mp3_ptr || mp3_len <= 0) return 0;
    // Cheap MP3 sync check first (defends against bogus call shapes).
    __try {
        if ((mp3_ptr[0] != 0xFF) || ((mp3_ptr[1] & 0xE0) != 0xE0)) return 0;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    // Walk back up to 64 records (1 KB) looking for a matching track.
    __try {
        for (int back = 1; back <= 64; ++back) {
            const uint8_t* rec = mp3_ptr - back * 16;
            uint32_t c1 = *(const uint32_t*)(rec + 0);
            uint32_t c2 = *(const uint32_t*)(rec + 4);
            uint32_t c4 = *(const uint32_t*)(rec + 12);
            if ((c1 != 0x31 && c1 != 0x30)) continue;
            if (((c2 >> 8) & 0xFF) != 0x6E)  continue;
            if ((int)c4 != mp3_len)          continue;
            // Match. Pull col2_param (bytes 2..3 of c2, u16 LE).
            uint16_t col2_param = (uint16_t)((c2 >> 16) & 0xFFFF);
            if (col2_param == 0) return 0;
            return mp3_walk_to_frame(mp3_ptr, mp3_len, col2_param);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 0;
}

// Find the col2_param (loop-start frame) for the track whose MP3 stream is at
// `mp3_ptr` by locating its record via the loader's buffer table. For every
// registered buffer (0xBAE2C0 + mode*8), walk the track records at the front of
// the buffer; the played track's record is the one where `buffer + c3 ==
// mp3_ptr` (exact — c4==mp3_len is a cross-check). This works for ALL tracks,
// not just track 0. Returns true + sets *out_param on an exact match.
// `*out_rec_index` is filled for the log. All reads are SEH-guarded.
static bool find_col2_param_via_buftable(const uint8_t* mp3_ptr, int mp3_len,
                                         uint16_t* out_param,
                                         int* out_rec_index) {
    if (!g_bof4_base || !mp3_ptr) return false;
    const uint8_t* const* tbl =
        (const uint8_t* const*)(g_bof4_base + BOF4_RVA_BUF_TABLE);
    for (int mode = 0; mode < BGM_MAX_MODE; ++mode) {
        const uint8_t* buf = nullptr;
        // table stride is 8 bytes per mode (ptr + pad); index [mode*2].
        __try { buf = tbl[mode * 2]; }
        __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        if (!buf) continue;
        __try {
            for (int k = 0; k < 256; ++k) {
                const uint8_t* rec = buf + (size_t)k * 16;
                uint32_t c1 = *(const uint32_t*)(rec + 0);
                uint32_t c2 = *(const uint32_t*)(rec + 4);
                uint32_t c3 = *(const uint32_t*)(rec + 8);
                uint32_t c4 = *(const uint32_t*)(rec + 12);
                if (c1 != 0x30 && c1 != 0x31)   break;  // end of record table
                if (((c2 >> 8) & 0xFF) != 0x6E)  break;
                if (buf + c3 == mp3_ptr && (int)c4 == mp3_len) {
                    *out_param     = (uint16_t)((c2 >> 16) & 0xFFFF);
                    *out_rec_index = k;
                    return true;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
    }
    return false;
}

// Detour: BOF4!0x40FC20  __cdecl int play(mp3_ptr, mp3_len, loop_flag)
static int __cdecl hook_bgm_play(const void* mp3_ptr, int mp3_len,
                                  int loop_flag) {
    int slot = g_orig_bgm_play(mp3_ptr, mp3_len, loop_flag);
    ++g_n_play_hits;
    if (slot > 0 && slot < 8 && loop_flag) {
        // Find this track's col2_param (loop-start frame). PRIMARY: the
        // loader buffer table — works for every track. FALLBACK: the old
        // backward-walk from mp3_ptr (only reliable for track 0), kept in case
        // the buffer table isn't populated on some code path.
        uint32_t lb = 0, seek_lb = 0, col2_param = 0;
        int rec_index = -1;
        const char* via = "none";
        const uint8_t* p = (const uint8_t*)mp3_ptr;

        uint16_t param16 = 0;
        int found_idx = -1;
        if (find_col2_param_via_buftable(p, mp3_len, &param16, &found_idx)) {
            col2_param = param16;
            rec_index = found_idx;
            via = "buftable";
        } else {
            // Fallback: backward-walk (track-0-only). SEH-guarded.
            __try {
                bool sync_ok = (p && (p[0] == 0xFF) && ((p[1] & 0xE0) == 0xE0));
                if (sync_ok) {
                    for (int back = 1; back <= 64; ++back) {
                        const uint8_t* rec = p - back * 16;
                        uint32_t c1 = *(const uint32_t*)(rec + 0);
                        uint32_t c2 = *(const uint32_t*)(rec + 4);
                        uint32_t c4 = *(const uint32_t*)(rec + 12);
                        if ((c1 != 0x31 && c1 != 0x30)) continue;
                        if (((c2 >> 8) & 0xFF) != 0x6E) continue;
                        if ((int)c4 != mp3_len) continue;
                        col2_param = (c2 >> 16) & 0xFFFF;
                        rec_index = -back;   // negative = walk-back distance
                        via = "walkback";
                        break;
                    }
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                col2_param = 0;
            }
        }

        if (col2_param > 0) {
            __try {
                lb = mp3_walk_to_frame(p, mp3_len, col2_param);
                int skip = BGM_PRIMING_SKIP_FRAMES;
                if ((int)col2_param <= skip) skip = 0;
                seek_lb = (skip > 0)
                    ? mp3_walk_to_frame(p, mp3_len, col2_param - skip)
                    : lb;
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                lb = 0; seek_lb = 0;
            }
        }

        g_loop_byte_per_slot[slot]      = lb;
        g_loop_seek_byte_per_slot[slot] = seek_lb;
        if (g_cfg.bgm_loop_log)
            hook_log("[bgm-fix] play slot=%d  mp3=0x%p  len=%d  loop=%d  "
                     "via=%s rec_idx=%d  col2_param=%u  loop_byte=%u  "
                     "seek_byte=%u\n",
                     slot, mp3_ptr, mp3_len, loop_flag,
                     via, rec_index, (unsigned)col2_param,
                     (unsigned)lb, (unsigned)seek_lb);
    }
    return slot;
}

// Detour: BOF4!0x693F90  __stdcall int seek(decoder_ctx, position)
//
// We're aggressive here: log every call (rate-limited) plus override when
// called from the BGM EOF block. The diagnostic output tells us whether
// 0x693F90 is the right hook target, or whether the decoder loops via a
// different path (internal wrap, separate restart fn, DSound-only loop).
static int __stdcall hook_bgm_seek(void* decoder_ctx, uint32_t position) {
    void* ra = _ReturnAddress();
    uintptr_t expected_ra = g_bof4_base + BOF4_RVA_EOF_CALL_RA;
    bool overrode = false;
    uint32_t old_pos = position;

    // Try to override if this looks like the BGM EOF call.
    int matched_slot = 0;
    uint32_t slot_loop_byte = 0;
    if ((uintptr_t)ra == expected_ra && position == 0 && g_bof4_base) {
        const void* const* slots = (const void* const*)
            (g_bof4_base + BOF4_RVA_SLOT_TABLE);
        for (int i = 1; i < 8; ++i) {
            const void* dec = slots[i * 6 + 2];
            if (dec == decoder_ctx) {
                uint32_t seek_lb = g_loop_seek_byte_per_slot[i];
                slot_loop_byte = g_loop_byte_per_slot[i];
                // bgm_loop = the fix. When OFF (but logging on) we still take
                // the EOF path and log, but leave position=0 (stock
                // loop-to-start) so the difference is audible for an A/B pass.
                if (seek_lb > 0 && g_cfg.bgm_loop_enabled) {
                    ++g_n_seek_fixes;
                    position = seek_lb;     // seek to look-back frame
                    overrode = true;
                    matched_slot = i;
                }
                if (g_cfg.bgm_loop_log)
                    hook_log("[bgm-seek] EOF-path slot=%d  decoder=0x%p  fix=%s  "
                             "pos: %u -> %u  (target=%u, look-back=%d frames)\n",
                             i, decoder_ctx,
                             g_cfg.bgm_loop_enabled ? "ON" : "OFF",
                             (unsigned)old_pos,
                             (unsigned)position, (unsigned)slot_loop_byte,
                             BGM_PRIMING_SKIP_FRAMES);
                break;
            }
        }
        if (!overrode && g_cfg.bgm_loop_log) {
            hook_log("[bgm-seek] EOF-path NO MATCH  decoder=0x%p  pos=%u\n",
                     decoder_ctx, (unsigned)old_pos);
        }
    } else {
        // Diagnostic: log first ~30 non-EOF-path calls so we can see if
        // 0x693F90 is being called via OTHER paths during BGM loop.
        static std::atomic<int> n_other { 0 };
        int n = ++n_other;
        if (n <= 30 && g_cfg.bgm_loop_log) {
            uintptr_t rip = (uintptr_t)ra;
            uintptr_t off = g_bof4_base ? rip - g_bof4_base : 0;
            hook_log("[bgm-seek] call #%d  ra=0x%p (BOF4+0x%X)  "
                     "decoder=0x%p  pos=%u\n",
                     n, ra, (unsigned)off, decoder_ctx, (unsigned)position);
        }
    }
    int rc = g_orig_bgm_seek(decoder_ctx, position);

    // Priming look-back: after we re-positioned the decoder to N frames
    // before the loop start, run decode_frame(decoder, NULL) N times so
    // the IMDCT state warms up. Output goes to the decoder's internal
    // scratch (we pass dest=NULL) and is discarded. The next legitimate
    // decode from the engine then produces clean PCM at the desired
    // loop point.
    if (overrode && g_decode_frame) {
        for (int i = 0; i < BGM_PRIMING_SKIP_FRAMES; ++i) {
            int dr = g_decode_frame(decoder_ctx, nullptr);
            (void)dr;  // discard
        }
        if (g_cfg.bgm_loop_log)
            hook_log("[bgm-seek] primed slot=%d  consumed %d frame(s)\n",
                     matched_slot, BGM_PRIMING_SKIP_FRAMES);
    }
    return rc;
}

// ── Counters ──────────────────────────────────────────────────────────────
static std::atomic<uint64_t> g_n_play_open  { 0 };
static std::atomic<uint64_t> g_n_replay_vt  { 0 };
static std::atomic<uint64_t> g_n_eof_signal { 0 };
static std::atomic<uint64_t> g_n_postmsg_other { 0 };

// ── Helpers ───────────────────────────────────────────────────────────────

// Return a pointer to a null-terminated copy of the path field from the
// 200-byte PlayDescriptor. Bounded so we don't read past the struct on a
// malformed input. Caller must use immediately (returns a pointer to a
// thread-local buffer).
static const char* safe_path_from_descriptor(const void* desc) {
    static thread_local char buf[260];
    if (!desc) { buf[0] = 0; return buf; }
    const char* src = (const char*)desc;
    size_t n = 0;
    __try {
        for (; n < sizeof(buf) - 1; ++n) {
            char c = src[n];
            if (c == 0) break;
            // Strip non-ASCII so the log stays readable.
            buf[n] = (c >= 0x20 && c < 0x7F) ? c : '.';
        }
        buf[n] = 0;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        buf[0] = '?'; buf[1] = 0;
    }
    return buf;
}

// ── Detour: 0x100C4110 (open/play) ────────────────────────────────────────
static int __fastcall hook_play_open(void* this_, void* edx_,
                                     const void* descriptor) {
    uint64_t seq = ++g_n_play_open;
    DWORD t = GetTickCount();
    DWORD tid = GetCurrentThreadId();
    const char* path = safe_path_from_descriptor(descriptor);
    hook_log("[bgm %u t=%lu thr=%lu] PLAY_OPEN  this=0x%p  desc=0x%p  "
             "path=\"%s\"\n",
             (unsigned)seq, (unsigned long)t, (unsigned long)tid,
             this_, descriptor, path);
    int rc = g_orig_play_open(this_, edx_, descriptor);
    hook_log("[bgm %u t=%lu thr=%lu] play_open RETURNED rc=0x%X\n",
             (unsigned)seq, (unsigned long)GetTickCount(),
             (unsigned long)GetCurrentThreadId(), (unsigned)rc);
    return rc;
}

// ── Detour: 0x100C1AF0 (vtable[16] = "issue play on stream-control") ─────
static int __stdcall hook_replay_vt(void* this_,
                                    const void* descriptor, int arg3) {
    uint64_t seq = ++g_n_replay_vt;
    DWORD t = GetTickCount();
    DWORD tid = GetCurrentThreadId();
    const char* path = safe_path_from_descriptor(descriptor);
    hook_log("[bgm %u t=%lu thr=%lu] REPLAY_VT  this=0x%p  desc=0x%p  "
             "arg3=0x%X  path=\"%s\"\n",
             (unsigned)seq, (unsigned long)t, (unsigned long)tid,
             this_, descriptor, (unsigned)arg3, path);
    int rc = g_orig_replay_vt(this_, descriptor, arg3);
    hook_log("[bgm %u t=%lu thr=%lu] replay_vt RETURNED rc=0x%X\n",
             (unsigned)seq, (unsigned long)GetTickCount(),
             (unsigned long)GetCurrentThreadId(), (unsigned)rc);
    return rc;
}

// ── Detour: dinput!MyCreateFileA  (real BGM caller is up-stack) ─────────
// _ReturnAddress() lands inside the CRT's _open_osfhandle wrapper; the BGM
// streaming code is several frames above. CaptureStackBackTrace walks via
// Windows' SEH-aware unwinder, which works through FPO frames in the CRT.
//
// Annotate each frame with the module it falls in (BOF4.exe/dinput.dll/...)
// so we can pick out the first BGM-side frame visually.
static void log_frame(int i, void* addr) {
    uintptr_t a = (uintptr_t)addr;
    HMODULE m = nullptr;
    char modname[MAX_PATH] = "?";
    uintptr_t mod_base = 0;
    if (GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)addr, &m) && m) {
        char path[MAX_PATH];
        GetModuleFileNameA(m, path, MAX_PATH);
        const char* bn = strrchr(path, '\\');
        bn = bn ? bn + 1 : path;
        snprintf(modname, sizeof(modname), "%s", bn);
        mod_base = (uintptr_t)m;
    }
    hook_log("    [%2d] 0x%p  %s+0x%X\n", i, addr, modname,
             mod_base ? (unsigned)(a - mod_base) : 0);
}

static HANDLE WINAPI hook_my_createfilea(LPCSTR name, DWORD access,
                                         DWORD share,
                                         LPSECURITY_ATTRIBUTES sec,
                                         DWORD disposition,
                                         DWORD flags, HANDLE templ) {
    HANDLE h = g_orig_my_createfilea(name, access, share, sec,
                                     disposition, flags, templ);
    if (name) {
        const char* base = strrchr(name, '\\');
        base = base ? base + 1 : name;
        if ((base[0] == 'B' || base[0] == 'b') &&
            (base[1] == 'G' || base[1] == 'g') &&
            (base[2] == 'M' || base[2] == 'm')) {
            hook_log("[bgm-dinput-open] MyCreateFileA(\"%s\") -> h=0x%p\n"
                     "    stack trace (top is innermost — skip CRT frames "
                     "to find BGM code):\n",
                     name, h);
            void* frames[24];
            USHORT n = CaptureStackBackTrace(0, 24, frames, nullptr);
            for (USHORT i = 0; i < n; ++i) {
                log_frame(i, frames[i]);
            }
        }
    }
    return h;
}

// ── Detour: BOF4!_fsopen ─────────────────────────────────────────────────
// At entry, [esp] is the return address — direct path to the BGM-side
// caller, no unwinder needed.
static void* __cdecl hook_fsopen(const char* path, const char* mode,
                                 int shflag) {
    void* ra = _ReturnAddress();
    void* fp = g_orig_fsopen(path, mode, shflag);
    if (path) {
        const char* base = strrchr(path, '\\');
        base = base ? base + 1 : path;
        if ((base[0] == 'B' || base[0] == 'b') &&
            (base[1] == 'G' || base[1] == 'g') &&
            (base[2] == 'M' || base[2] == 'm')) {
            uintptr_t bof4_base = (uintptr_t)GetModuleHandleA(nullptr);
            uintptr_t rip = (uintptr_t)ra;
            hook_log("[bgm-fsopen] _fsopen(\"%s\", \"%s\", %d) -> 0x%p\n"
                     "    ra=0x%p (BOF4+0x%X)\n",
                     path, mode ? mode : "?", shflag, fp,
                     ra, (unsigned)(rip - bof4_base));
        }
    }
    return fp;
}

// ── Detour: BOF4!fopen — _ReturnAddress is the BGM-side caller ─────────
static void* __cdecl hook_fopen(const char* path, const char* mode) {
    void* ra = _ReturnAddress();
    void* fp = g_orig_fopen(path, mode);
    if (path) {
        const char* base = strrchr(path, '\\');
        base = base ? base + 1 : path;
        if ((base[0] == 'B' || base[0] == 'b') &&
            (base[1] == 'G' || base[1] == 'g') &&
            (base[2] == 'M' || base[2] == 'm')) {
            uintptr_t bof4_base = (uintptr_t)GetModuleHandleA(nullptr);
            uintptr_t rip = (uintptr_t)ra;
            hook_log("[bgm-fopen] fopen(\"%s\", \"%s\") -> 0x%p\n"
                     "    BGM-CALLER ra=0x%p (BOF4+0x%X)\n",
                     path, mode ? mode : "?", fp,
                     ra, (unsigned)(rip - bof4_base));
        }
    }
    return fp;
}

// ── Detour: PostMessageA, filtered by return-into-dshow ──────────────────
static BOOL WINAPI hook_PostMessageA(HWND hwnd, UINT msg,
                                     WPARAM wp, LPARAM lp) {
    // Read return address (caller's IP). For __stdcall on x86, the return
    // address is at [ebp+4] inside this function — but we use _ReturnAddress
    // intrinsic for portability across the trampoline.
    void* ret = _ReturnAddress();
    uintptr_t ra = (uintptr_t)ret;

    if (g_dshow_base && ra >= g_dshow_base && ra < g_dshow_end) {
        uint64_t seq = ++g_n_eof_signal;
        DWORD t = GetTickCount();
        DWORD tid = GetCurrentThreadId();
        hook_log("[bgm %u t=%lu thr=%lu] EOF_SIGNAL  via PostMessageA  "
                 "ra=0x%p (dshow+0x%X)  hwnd=0x%p  msg=0x%X  "
                 "wp=0x%p  lp=0x%p\n",
                 (unsigned)seq, (unsigned long)t, (unsigned long)tid,
                 ret, (unsigned)(ra - g_dshow_base),
                 hwnd, (unsigned)msg, (void*)wp, (void*)lp);
    } else {
        ++g_n_postmsg_other;
    }
    return g_orig_PostMessageA(hwnd, msg, wp, lp);
}

// ── Install ───────────────────────────────────────────────────────────────

static bool resolve_dshow_extent() {
    g_dshow = GetModuleHandleA("dshow.dll");
    if (!g_dshow) g_dshow = LoadLibraryA("dshow.dll");
    if (!g_dshow) {
        hook_log("bgm: dshow.dll not loaded — cannot install hooks "
                 "(GetLastError=%lu)\n", GetLastError());
        return false;
    }
    g_dshow_base = (uintptr_t)g_dshow;

    MODULEINFO mi{};
    if (GetModuleInformation(GetCurrentProcess(), g_dshow, &mi, sizeof(mi))) {
        g_dshow_end = g_dshow_base + mi.SizeOfImage;
    } else {
        // Fallback: assume the wrapper code fits in the documented ~24KB
        // window starting 0xC0000 above the base, plus a generous tail.
        g_dshow_end = g_dshow_base + 0x00200000;
    }
    hook_log("bgm: dshow.dll base=0x%p  end=0x%p  size=0x%X\n",
             (void*)g_dshow_base, (void*)g_dshow_end,
             (unsigned)(g_dshow_end - g_dshow_base));
    return true;
}

static bool resolve_dinput_extent() {
    g_dinput = GetModuleHandleA("dinput.dll");
    if (!g_dinput) g_dinput = LoadLibraryA("dinput.dll");
    if (!g_dinput) {
        hook_log("bgm: dinput.dll not loaded — skipping wrapper hook "
                 "(GetLastError=%lu)\n", GetLastError());
        return false;
    }
    g_dinput_base = (uintptr_t)g_dinput;
    MODULEINFO mi{};
    if (GetModuleInformation(GetCurrentProcess(), g_dinput, &mi, sizeof(mi))) {
        g_dinput_end = g_dinput_base + mi.SizeOfImage;
    } else {
        g_dinput_end = g_dinput_base + 0x100000;
    }
    hook_log("bgm: dinput.dll base=0x%p  end=0x%p  size=0x%X\n",
             (void*)g_dinput_base, (void*)g_dinput_end,
             (unsigned)(g_dinput_end - g_dinput_base));
    return true;
}

static bool install_one(const char* name, void* target, void* detour,
                        void** orig_out) {
    MH_STATUS st = MH_CreateHook(target, detour, orig_out);
    if (st != MH_OK) {
        hook_log("bgm: MH_CreateHook(%s @0x%p) failed: status=%d\n",
                 name, target, (int)st);
        return false;
    }
    st = MH_EnableHook(target);
    if (st != MH_OK) {
        hook_log("bgm: MH_EnableHook(%s @0x%p) failed: status=%d\n",
                 name, target, (int)st);
        return false;
    }
    hook_log("bgm: hooked %-12s @ 0x%p (orig trampoline @ 0x%p)\n",
             name, target, *orig_out);
    return true;
}

void bgm_loop_install() {
    // Caller (bof4_hooks.cpp) decides whether to call us — keep this
    // unconditional so the gate lives in one place. If you want a
    // runtime kill switch, add a g_cfg.bgm_loop_enabled check here.

    if (!resolve_dshow_extent()) return;

    // MinHook may not be initialized yet (bof4_hooks installs other
    // hooks before us). MH_Initialize is idempotent-by-result: the
    // second call returns MH_ERROR_ALREADY_INITIALIZED, which we ignore.
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        hook_log("bgm: MH_Initialize failed: status=%d\n", (int)st);
        return;
    }

    void* play_open = (void*)(g_dshow_base + DSHOW_RVA_PLAY_OPEN);
    void* replay_vt = (void*)(g_dshow_base + DSHOW_RVA_REPLAY_VT);

    install_one("PLAY_OPEN", play_open,  (void*)hook_play_open,
                (void**)&g_orig_play_open);
    install_one("REPLAY_VT", replay_vt,  (void*)hook_replay_vt,
                (void**)&g_orig_replay_vt);

    if (resolve_dinput_extent()) {
        void* my_cf = (void*)(g_dinput_base + DINPUT_RVA_MYCREATEFILEA);
        install_one("MyCreateFileA", my_cf, (void*)hook_my_createfilea,
                    (void**)&g_orig_my_createfilea);
    }

    // BOF4 statically-linked CRT _fsopen + fopen
    {
        g_bof4_base = (uintptr_t)GetModuleHandleA(nullptr);
        void* fsopen = (void*)(g_bof4_base + BOF4_RVA_FSOPEN);
        install_one("_fsopen", fsopen, (void*)hook_fsopen,
                    (void**)&g_orig_fsopen);
        void* fopen = (void*)(g_bof4_base + BOF4_RVA_FOPEN);
        install_one("fopen", fopen, (void*)hook_fopen,
                    (void**)&g_orig_fopen);

        // The actual loop-bug fix: capture loop point on play, and on
        // EOF-seek replace position=0 with the correct loop_byte.
        void* play = (void*)(g_bof4_base + BOF4_RVA_PLAY);
        install_one("bgm_play", play, (void*)hook_bgm_play,
                    (void**)&g_orig_bgm_play);
        void* seek = (void*)(g_bof4_base + BOF4_RVA_SEEK);
        install_one("bgm_seek", seek, (void*)hook_bgm_seek,
                    (void**)&g_orig_bgm_seek);
        // Resolve (don't hook) the decode_frame entry — we call it
        // ourselves after our seek to warm up the IMDCT state.
        g_decode_frame = (decode_frame_t)(g_bof4_base
                                          + BOF4_RVA_DECODE_FRAME);
        hook_log("bgm: resolved decode_frame @ 0x%p (priming-skip=%d frames)\n",
                 g_decode_frame, BGM_PRIMING_SKIP_FRAMES);
    }

    // PostMessageA via API hook (MinHook resolves the user32 function
    // by name and hooks its prologue).
    st = MH_CreateHookApi(L"user32.dll", "PostMessageA",
                         (void*)hook_PostMessageA,
                         (void**)&g_orig_PostMessageA);
    if (st == MH_OK) {
        st = MH_EnableHook(MH_ALL_HOOKS);
        if (st == MH_OK) {
            hook_log("bgm: hooked %-12s (filtered to dshow.dll callers)\n",
                     "PostMessageA");
        } else {
            hook_log("bgm: MH_EnableHook(PostMessageA) failed: status=%d\n",
                     (int)st);
        }
    } else {
        hook_log("bgm: MH_CreateHookApi(PostMessageA) failed: status=%d\n",
                 (int)st);
    }

    hook_log("bgm: install complete — Phase 1 logging is live\n");
}
