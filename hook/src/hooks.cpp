// BoF4 d3d9 proxy — vtable-based hooks.
//
// Flow:
//   Direct3DCreate9()          -> we get IDirect3D9*
//     patch vtable[16] (CreateDevice) so every device-creation is logged
//
//   IDirect3D9::CreateDevice() -> we get IDirect3DDevice9*
//     patch vtable[23] (CreateTexture) so every texture is observed
//
//   IDirect3DDevice9::CreateTexture() -> we get IDirect3DTexture9*
//     patch vtable[19] (LockRect) and vtable[20] (UnlockRect)
//
//   LockRect/UnlockRect         -> record pointer at Lock, dump on Unlock
//
// Each vtable is shared across all instances of that COM class, so we
// only patch each vtable once. Subsequent instances use our hooks
// automatically.

#include "hooks.h"
#include "subtitles.h"
#include "rect_tuner.h"   // in-game Rect Tuner: per-frame HUD + flash (device Present)
#include "rect_hook.h"    // rect_hook_frame_tick() — per-frame lifetime counter
#include "name_slot_bp.h" // TEMP diagnostic: re-arm the [07] scratch write-watch
#include "ff_overlay.h"   // top-right fast-forward speed badge
#include "pause_freeze.h" // save-state "real pause" — RAM restore after Present
#include <d3d9.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <intrin.h>   // _AddressOfReturnAddress, __readfsdword

// ── Vtable indexes (from d3d9.h declaration order) ─────────────────────────
//   IUnknown has 3 methods (QueryInterface, AddRef, Release) at 0..2.
//
//   IDirect3D9 adds 14 methods after IUnknown (RegisterSoftwareDevice is 3,
//   GetAdapterCount 4, ..., GetAdapterMonitor 15, CreateDevice 16).
static constexpr int VT_D3D9_CREATEDEVICE   = 16;
//
//   IDirect3DDevice9 adds 117 methods after IUnknown. CreateTexture is
//   method 23 (0-indexed):
//     3 TestCooperativeLevel, 4 GetAvailableTextureMem,
//     5 EvictManagedResources, 6 GetDirect3D, 7 GetDeviceCaps,
//     8 GetDisplayMode, 9 GetCreationParameters, 10 SetCursorProperties,
//     11 SetCursorPosition, 12 ShowCursor, 13 CreateAdditionalSwapChain,
//     14 GetSwapChain, 15 GetNumberOfSwapChains, 16 Reset, 17 Present,
//     18 GetBackBuffer, 19 GetRasterStatus, 20 SetDialogBoxMode,
//     21 SetGammaRamp, 22 GetGammaRamp, 23 CreateTexture
static constexpr int VT_DEV9_CREATETEXTURE  = 23;
// IDirect3DDevice9::Present is vtable slot 17 (QI=0, AR=1, Rel=2, TestCoop=3,
// GetAvailableTextureMem=4, EvictManagedResources=5, GetDirect3D=6,
// GetDeviceCaps=7, GetDisplayMode=8, GetCreationParameters=9, SetCursor=10,
// SetCursorPosition=11, ShowCursor=12, CreateAdditionalSwapChain=13,
// GetSwapChain=14, GetNumberOfSwapChains=15, Reset=16, Present=17)
static constexpr int VT_DEV9_PRESENT        = 17;
// CreateAdditionalSwapChain=13, GetSwapChain=14, Reset=16 (see list above).
// Used by the subtitle overlay so it can also draw on the FMV swap-chain
// present path and release its D3DPOOL_DEFAULT texture before a Reset.
static constexpr int VT_DEV9_CREATEADDLSC   = 13;
static constexpr int VT_DEV9_GETSWAPCHAIN   = 14;
static constexpr int VT_DEV9_RESET          = 16;
// IDirect3DSwapChain9: QI=0, AR=1, Rel=2, Present=3.
static constexpr int VT_SC9_PRESENT         = 3;
//
//   Further into IDirect3DDevice9 (counting declaration order in d3d9.h):
//     ..., 64 GetTexture, 65 SetTexture, 66 GetTextureStageState, ...
//     ..., 80 GetNPatchMode, 81 DrawPrimitive, 82 DrawIndexedPrimitive,
//          83 DrawPrimitiveUP, 84 DrawIndexedPrimitiveUP, ...
//
//   We hook SetTexture (so we know which texture is bound when the game
//   issues a draw) and the two *UP variants (which pass vertex data
//   inline, so we can read the first vertex's X/Y without having to
//   track vertex buffers).
static constexpr int VT_DEV9_SETTEXTURE             = 65;
static constexpr int VT_DEV9_DRAWPRIMITIVE          = 81;
static constexpr int VT_DEV9_DRAWINDEXEDPRIMITIVE   = 82;
static constexpr int VT_DEV9_DRAWPRIMITIVEUP        = 83;
static constexpr int VT_DEV9_DRAWINDEXEDPRIMITIVEUP = 84;
static constexpr int VT_DEV9_SETSTREAMSOURCE        = 100;
static constexpr int VT_DEV9_UPDATESURFACE          = 30;
static constexpr int VT_DEV9_STRETCHRECT            = 34;
static constexpr int VT_DEV9_COLORFILL              = 35;
//
//   IDirect3DTexture9 (inherits IDirect3DBaseTexture9 : IDirect3DResource9).
//   IDirect3DResource9 adds 8 methods at 3..10, IDirect3DBaseTexture9 adds
//   6 methods at 11..16, IDirect3DTexture9 adds its own 5:
//     17 GetLevelDesc, 18 GetSurfaceLevel,
//     19 LockRect, 20 UnlockRect, 21 AddDirtyRect
static constexpr int VT_TEX9_LOCKRECT       = 19;
static constexpr int VT_TEX9_UNLOCKRECT     = 20;

// ── Typedefs for the hooked methods ────────────────────────────────────────
typedef HRESULT (STDMETHODCALLTYPE *CreateDevice_t)(
    IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
    D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);

typedef HRESULT (STDMETHODCALLTYPE *CreateTexture_t)(
    IDirect3DDevice9*, UINT, UINT, UINT, DWORD,
    D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);

typedef HRESULT (STDMETHODCALLTYPE *LockRect_t)(
    IDirect3DTexture9*, UINT, D3DLOCKED_RECT*, const RECT*, DWORD);

typedef HRESULT (STDMETHODCALLTYPE *UnlockRect_t)(
    IDirect3DTexture9*, UINT);

typedef HRESULT (STDMETHODCALLTYPE *Present_t)(
    IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);

typedef HRESULT (STDMETHODCALLTYPE *SetTexture_t)(
    IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);

typedef HRESULT (STDMETHODCALLTYPE *SetStreamSource_t)(
    IDirect3DDevice9*, UINT, IDirect3DVertexBuffer9*, UINT, UINT);

typedef HRESULT (STDMETHODCALLTYPE *DrawPrimitive_t)(
    IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);

typedef HRESULT (STDMETHODCALLTYPE *DrawIndexedPrimitive_t)(
    IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);

typedef HRESULT (STDMETHODCALLTYPE *DrawPrimitiveUP_t)(
    IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);

typedef HRESULT (STDMETHODCALLTYPE *DrawIndexedPrimitiveUP_t)(
    IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT,
    const void*, D3DFORMAT, const void*, UINT);

typedef HRESULT (STDMETHODCALLTYPE *Reset_t)(
    IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

typedef HRESULT (STDMETHODCALLTYPE *GetSwapChain_t)(
    IDirect3DDevice9*, UINT, IDirect3DSwapChain9**);

typedef HRESULT (STDMETHODCALLTYPE *CreateAdditionalSwapChain_t)(
    IDirect3DDevice9*, D3DPRESENT_PARAMETERS*, IDirect3DSwapChain9**);

typedef HRESULT (STDMETHODCALLTYPE *SCPresent_t)(
    IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);

// Surface-copy blit path — the title menu is composited by ddraw.dll via these
// (it issues no per-label DrawPrimitive), so the source RECT width here is the
// clip we need to widen for German labels.
typedef HRESULT (STDMETHODCALLTYPE *StretchRect_t)(
    IDirect3DDevice9*, IDirect3DSurface9*, const RECT*,
    IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);
typedef HRESULT (STDMETHODCALLTYPE *UpdateSurface_t)(
    IDirect3DDevice9*, IDirect3DSurface9*, const RECT*,
    IDirect3DSurface9*, const POINT*);
typedef HRESULT (STDMETHODCALLTYPE *ColorFill_t)(
    IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, D3DCOLOR);

// ── Original function pointers (captured before we patch) ──────────────────
static CreateDevice_t            g_orig_CreateDevice            = nullptr;
static CreateTexture_t           g_orig_CreateTexture           = nullptr;
static LockRect_t                g_orig_LockRect                = nullptr;
static UnlockRect_t              g_orig_UnlockRect              = nullptr;
static Present_t                 g_orig_Present                 = nullptr;
static SetTexture_t              g_orig_SetTexture              = nullptr;
static SetStreamSource_t         g_orig_SetStreamSource         = nullptr;
static DrawPrimitive_t           g_orig_DrawPrimitive           = nullptr;
static DrawIndexedPrimitive_t    g_orig_DrawIndexedPrimitive    = nullptr;
static DrawPrimitiveUP_t         g_orig_DrawPrimitiveUP         = nullptr;
static DrawIndexedPrimitiveUP_t  g_orig_DrawIndexedPrimitiveUP  = nullptr;
static Reset_t                   g_orig_Reset                   = nullptr;
static GetSwapChain_t            g_orig_GetSwapChain            = nullptr;
static CreateAdditionalSwapChain_t g_orig_CreateAddlSC          = nullptr;
static SCPresent_t               g_orig_SCPresent               = nullptr;
static StretchRect_t             g_orig_StretchRect             = nullptr;
static UpdateSurface_t           g_orig_UpdateSurface           = nullptr;
static ColorFill_t               g_orig_ColorFill               = nullptr;
static bool                      g_patched_swapchain            = false;

// ── Patch-once flags per vtable ────────────────────────────────────────────
static bool g_patched_d3d9     = false;
static bool g_patched_device   = false;
static bool g_patched_texture  = false;

static std::mutex g_mtx;

// Locked-rect tracking: texture+level -> pointer recorded at LockRect time.
struct LockKey {
    IDirect3DTexture9* tex;
    UINT               level;
    bool operator==(const LockKey& o) const noexcept {
        return tex == o.tex && level == o.level;
    }
};
struct LockKeyHash {
    size_t operator()(const LockKey& k) const noexcept {
        return (size_t)k.tex ^ ((size_t)k.level * 0x9E3779B1u);
    }
};
static std::unordered_map<LockKey, D3DLOCKED_RECT, LockKeyHash> g_locked;

// Textures we've already dumped (so we only grab each one once).
static std::unordered_set<IDirect3DTexture9*> g_dumped;

// Small-texture bookkeeping for draw-position logging.
// We store CRC + size for every SMALL texture (<=64x64) so the draw hook
// can correlate an IDirect3DBaseTexture9* currently bound to SetTexture
// back to the CRC we'd dump/inject it at.
struct SmallTexInfo {
    uint32_t crc;
    UINT     width;
    UINT     height;
};
static std::unordered_map<IDirect3DTexture9*, SmallTexInfo> g_small_tex;

// Currently bound texture at sampler stage 0 — updated by hook_SetTexture.
static IDirect3DBaseTexture9* g_bound_tex = nullptr;

// ── D3D9 device + TTF texture swap (bof3ext-style) ────────────────────────
// Instead of modifying the game's texture pixels (which the GoG DDraw
// wrapper can overwrite from its internal software copy), we create our
// OWN D3D9 textures pre-filled with TTF glyphs and swap them in at
// SetTexture time. The wrapper never touches our textures because it
// didn't create them.
static IDirect3DDevice9*     g_device = nullptr;
// Module that created g_device. Later CreateDevice calls from a DIFFERENT module
// are an injected overlay's, not the game's — see hook_CreateDevice.
static HMODULE               g_device_owner = nullptr;
// Game texture → BoF4 char code (set during UnlockRect when F_B TLS is valid)
// BoF4 char code → our D3D9 texture with TTF pixels

// Currently bound vertex buffer at stream 0 — updated by hook_SetStreamSource.
// Captured so the DrawPrimitive hook can read the vertex the game is about
// to draw from.
static IDirect3DVertexBuffer9* g_bound_vb = nullptr;
static UINT g_bound_vb_offset = 0;
static UINT g_bound_vb_stride = 0;

// Cap on draw-log spam so a few seconds of gameplay don't balloon the log.
static constexpr int DRAW_LOG_MAX = 30000;
static std::atomic<int> g_draw_log_count{0};

// ── Stack backtrace for glyph-lock callers ────────────────────────────────
// Exists to find BoF4's text-draw function. At UnlockRect time, the game
// code that wrote the glyph pixels is somewhere up the call stack. If we
// log its return address (as an offset into BOF4.EXE) we can disassemble
// it in IDA/Ghidra and install a proper inline hook there — which gives
// us character codes before rasterization, just like bof3ext does.
//
// WHY A MANUAL STACK SCAN (instead of RtlCaptureStackBackTrace):
// BoF4 on PC runs through GoG's DDraw→D3D9 wrapper (ddraw.dll at base
// 0x18000000). Both the wrapper and BoF4 itself are compiled with /Oy
// (frame pointer omission) and lack x86 unwind metadata, so the Windows
// stack walker bails out after 2 frames — neither of which is in BoF4.
//
// Instead, we scan the raw stack memory upward from our hook's ESP,
// looking for any DWORD that lies inside BOF4.EXE and is preceded by a
// `call` opcode (E8 rel32 / FF 15 abs32 / FF /2 modrm). That filter is
// tight enough to reject false positives (random stack garbage that
// happens to look like a code pointer). Hits are real return addresses.

static uintptr_t                     g_bof4_base = 0;
static uintptr_t                     g_bof4_end  = 0;
static constexpr int                 GLYPH_BT_MAX_UNIQUE = 32;
static constexpr int                 GLYPH_BT_DIAG_MAX   = 10;
static std::mutex                    g_glyph_bt_mtx;
static std::unordered_set<uintptr_t> g_glyph_bt_seen;
static std::atomic<int>              g_glyph_bt_diag{0};

// Resolve BOF4.EXE base..end once via the PE header. No psapi dependency.
static void ensure_bof4_range() {
    if (g_bof4_end) return;
    HMODULE h = GetModuleHandleA(nullptr);
    if (!h) return;
    uintptr_t base = (uintptr_t)h;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    const IMAGE_NT_HEADERS* nt =
        (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    g_bof4_base = base;
    g_bof4_end  = base + nt->OptionalHeader.SizeOfImage;
    hook_log("BoF4 module range: %08X..%08X (size=0x%X)\n",
             (unsigned)g_bof4_base, (unsigned)g_bof4_end,
             (unsigned)nt->OptionalHeader.SizeOfImage);
}

static inline bool addr_in_bof4(uintptr_t a) {
    return a >= g_bof4_base && a < g_bof4_end;
}

// Resolve a code address to its owning module basename + rel offset.
// Returns nullptr on failure. Buffer must be at least 64 bytes.
static const char* module_of(uintptr_t addr, char* buf, size_t buf_sz) {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)addr, &mod) || !mod) {
        return nullptr;
    }
    char full[MAX_PATH] = {0};
    if (GetModuleFileNameA(mod, full, MAX_PATH) == 0) return nullptr;
    const char* base = strrchr(full, '\\');
    base = base ? base + 1 : full;
    _snprintf_s(buf, buf_sz, _TRUNCATE, "%s+%08X",
                base, (unsigned)(addr - (uintptr_t)mod));
    return buf;
}

// Validate that `ret_addr` looks like a return address: the bytes
// immediately preceding it in code memory should form a `call` opcode.
// This catches the three common x86 forms we'll encounter in BoF4.EXE:
//   E8 rel32                         → 5-byte direct call
//   FF 15 abs32                      → 6-byte call [disp32]
//   FF /2 modrm (+ optional sib/imm) → 2..7-byte call [reg]/[reg+disp]
static inline bool looks_like_return_addr(uintptr_t ret_addr) {
    if (ret_addr < g_bof4_base + 8 || ret_addr >= g_bof4_end) return false;
    const uint8_t* p = (const uint8_t*)ret_addr;
    // E8 rel32 (most common direct call)
    if (p[-5] == 0xE8) return true;
    // FF 15 <abs32> (call [imm32] — import thunks)
    if (p[-6] == 0xFF && p[-5] == 0x15) return true;
    // FF /2 forms. ModR/M.reg bits = 010 → (byte & 0x38) == 0x10.
    //   FF D0..D7      (2 bytes: call reg)
    //   FF 10..17      (2 bytes: call [reg])
    //   FF 50..57 ib   (3 bytes: call [reg+disp8])
    //   FF 90..97 id   (6 bytes: call [reg+disp32])
    //   with SIB (FF 14 sib / FF 54 sib ib / FF 94 sib id) add 1
    if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) return true;  // FF D0-D7, FF 10-17 (no disp)
    if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) return true;  // FF 50-57 ib
    if (p[-4] == 0xFF && (p[-3] & 0x38) == 0x10) return true;  // FF 14 sib
    if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) return true;  // FF 90-97 id
    if (p[-7] == 0xFF && (p[-6] & 0x38) == 0x10) return true;  // FF 94 sib id
    return false;
}

// Walk the caller's stack looking for DWORDs that lie inside BOF4.EXE
// and pass the call-opcode validator. Returns candidates in stack order
// (innermost / most-recent call first). Deduped.
static int scan_caller_stack(uintptr_t* out, int max_out) {
    if (!g_bof4_end || max_out <= 0) return 0;

    uintptr_t sp  = (uintptr_t)_AddressOfReturnAddress();
    // Stack top (highest address) lives at NT_TIB.StackBase = fs:[0x04] on
    // 32-bit x86. Read directly to avoid the forward-declared _TEB struct
    // being unusable without winternl.h.
    uintptr_t top = (uintptr_t)__readfsdword(0x04);
    if (top <= sp) return 0;
    // Safety cap: never scan more than 128 KB of stack.
    if (top - sp > 0x20000) top = sp + 0x20000;

    int found = 0;
    for (uintptr_t p = sp; p + 4 <= top && found < max_out; p += 4) {
        uintptr_t v = *(const uintptr_t*)p;
        if (v < g_bof4_base || v >= g_bof4_end) continue;
        if (!looks_like_return_addr(v)) continue;
        bool dup = false;
        for (int i = 0; i < found; ++i) {
            if (out[i] == v) { dup = true; break; }
        }
        if (!dup) out[found++] = v;
    }
    return found;
}

// Emit backtrace log lines for a glyph-upload. First GLYPH_BT_DIAG_MAX
// hits log the full raw candidate list; after that, we only log unique
// innermost callers (capped at GLYPH_BT_MAX_UNIQUE).
static void log_glyph_caller_backtrace() {
    ensure_bof4_range();
    if (!g_bof4_end) return;

    uintptr_t cands[16] = {0};
    int n = scan_caller_stack(cands, 16);

    g_glyph_bt_diag.fetch_add(1, std::memory_order_relaxed);

    if (n == 0) return;
    uintptr_t top_bof4 = cands[0];

    {
        std::lock_guard<std::mutex> lk(g_glyph_bt_mtx);
        if (g_glyph_bt_seen.size() >= GLYPH_BT_MAX_UNIQUE) return;
        if (!g_glyph_bt_seen.insert(top_bof4).second) return;
    }

    char line[1024];
    int k = _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "GLYPH_CALLER top=%08X (BOF4+%08X) chain=",
                        (unsigned)top_bof4,
                        (unsigned)(top_bof4 - g_bof4_base));
    if (k < 0) return;
    for (int i = 0; i < n && k < (int)sizeof(line) - 16; ++i) {
        int extra = _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE,
                                " +%08X", (unsigned)(cands[i] - g_bof4_base));
        if (extra <= 0) break;
        k += extra;
    }
    hook_log("%s\n", line);
}

// ── Logging ────────────────────────────────────────────────────────────────
void hook_log(const char* fmt, ...) {
    static HANDLE log = INVALID_HANDLE_VALUE;
    static std::mutex log_mtx;
    std::lock_guard<std::mutex> lk(log_mtx);

    if (log == INVALID_HANDLE_VALUE) {
        log = CreateFileA("d3d9_hook.log", FILE_APPEND_DATA,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          nullptr, OPEN_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
        if (log != INVALID_HANDLE_VALUE) {
            SetFilePointer(log, 0, nullptr, FILE_END);
        }
    }
    if (log == INVALID_HANDLE_VALUE) return;

    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;

    DWORD w = 0;
    WriteFile(log, buf, (DWORD)n, &w, nullptr);
    FlushFileBuffers(log);

    // Mirror to the debug console (if allocated via `console = true`
    // in _d3d9_hook_config.txt). This is how fast-forward toggle events
    // and other live diagnostics become visible without alt-tabbing.
    if (g_cfg.console_enabled) {
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        if (out && out != INVALID_HANDLE_VALUE) {
            DWORD cw = 0;
            WriteFile(out, buf, (DWORD)n, &cw, nullptr);
        }
    }
}

// ── Vtable helpers ─────────────────────────────────────────────────────────
static void* patch_vtable_slot(void** vtable, int idx, void* new_fn) {
    DWORD old_prot = 0;
    if (!VirtualProtect(&vtable[idx], sizeof(void*),
                        PAGE_EXECUTE_READWRITE, &old_prot)) {
        return nullptr;
    }
    void* old = vtable[idx];
    vtable[idx] = new_fn;
    VirtualProtect(&vtable[idx], sizeof(void*), old_prot, &old_prot);
    return old;
}

static void** vtable_of(void* com_obj) {
    return *(void***)com_obj;
}

// ── Hooked IDirect3DTexture9::LockRect ─────────────────────────────────────
// The IDirect3DTexture9 vtable lives in d3d9.dll and is SHARED PROCESS-WIDE, so
// these fire for every texture ANY component locks — including an injected
// overlay's UI textures, on its own thread. Taking our global g_mtx (and growing
// g_dumped) for foreign textures is pure contention against a subsystem we know
// nothing about. Both consumers are opt-in and default OFF, so cost nothing when
// off: bail before the lock unless one of them actually wants the pixels.
static inline bool tex_taps_wanted() {
    return g_cfg.tex_dump_enabled || g_cfg.tex_inject_enabled;
}

static HRESULT STDMETHODCALLTYPE hook_LockRect(
    IDirect3DTexture9* self, UINT level,
    D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags)
{
    HRESULT hr = g_orig_LockRect(self, level, locked, rect, flags);
    if (!tex_taps_wanted()) return hr;
    if (SUCCEEDED(hr) && locked && locked->pBits) {
        std::lock_guard<std::mutex> lk(g_mtx);
        // Only track level 0 (full-res image). Don't overwrite if already
        // dumped — saves CPU on repeated locks of the same font atlas.
        if (level == 0 && g_dumped.find(self) == g_dumped.end()) {
            g_locked[{self, level}] = *locked;
        }
    }
    return hr;
}

// ── Hooked IDirect3DTexture9::UnlockRect ───────────────────────────────────
//
// Order of operations:
//   1. If we recorded a LockRect for this (texture, level), read the
//      locked pixels.
//   2. Compute CRC32 of the pixel contents (always — matches the PNG
//      that was dumped on first upload so the user can target it).
//   3. First time this texture pointer is seen: write tex_dump/<...>.png
//   4. Always: check tex_inject/<crc>.png — if present, overwrite the
//      locked buffer with our replacement pixels IN PLACE before calling
//      the real UnlockRect. The GPU upload then uses our pixels.
//   5. Call the real UnlockRect.
static HRESULT STDMETHODCALLTYPE hook_UnlockRect(
    IDirect3DTexture9* self, UINT level)
{
    if (!tex_taps_wanted()) return g_orig_UnlockRect(self, level);
    if (level == 0) {
        std::unique_lock<std::mutex> lk(g_mtx);
        auto it = g_locked.find({self, level});
        bool have_lock = (it != g_locked.end());
        D3DLOCKED_RECT lr = have_lock ? it->second : D3DLOCKED_RECT{};
        if (have_lock) g_locked.erase(it);
        bool first_time = have_lock
                          && (g_dumped.find(self) == g_dumped.end());
        if (first_time) g_dumped.insert(self);
        lk.unlock();

        if (have_lock) {
            D3DSURFACE_DESC desc = {};
            if (SUCCEEDED(self->GetLevelDesc(0, &desc))) {
                uint32_t crc = 0;
                if (first_time) {
                    // Dump also returns the crc of the RGBA8-converted
                    // pixels, so later injections match even if the texture
                    // re-uploads at the same address.
                    // Gated on g_cfg.tex_dump_enabled so a default install
                    // doesn't spray PNGs into tex_dump/ on every upload.
                    // The CRC is still needed for injection matching below,
                    // so compute it via the light scan when dumping is off.
                    if (g_cfg.tex_dump_enabled) {
                        crc = dump_texture_png(self, desc.Width, desc.Height,
                                               (unsigned int)desc.Format,
                                               lr.Pitch, lr.pBits);
                    } else {
                        extern uint32_t crc32_rgba_of_locked(
                            UINT w, UINT h, unsigned int fmt,
                            INT pitch, const void* px);
                        crc = crc32_rgba_of_locked(desc.Width, desc.Height,
                                                   (unsigned int)desc.Format,
                                                   lr.Pitch, lr.pBits);
                    }
                } else {
                    // Non-first upload: recompute the CRC of current pixels
                    // so injection still fires. Do a light RGBA8 scan.
                    // Cost: only textures that get re-locked pay this.
                    extern uint32_t crc32_rgba_of_locked(
                        UINT w, UINT h, unsigned int fmt,
                        INT pitch, const void* px);
                    crc = crc32_rgba_of_locked(desc.Width, desc.Height,
                                               (unsigned int)desc.Format,
                                               lr.Pitch, lr.pBits);
                }
                if (crc) {
                    if (desc.Width <= 64 && desc.Height <= 64) {
                        std::lock_guard<std::mutex> lk2(g_mtx);
                        g_small_tex[self] = {crc, desc.Width, desc.Height};
                    }
                    // Still run the old injection path as fallback
                    // (covers tex_inject/*.png and non-F_B uploads)
                    try_inject_texture(crc, desc.Width, desc.Height,
                                       (unsigned int)desc.Format,
                                       lr.Pitch, lr.pBits);
                }
            }
        }
    }
    return g_orig_UnlockRect(self, level);
}

// ── Install hooks on a texture vtable ──────────────────────────────────────
static void hook_texture(IDirect3DTexture9* tex) {
    if (!tex || g_patched_texture) return;
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_patched_texture) return;

    void** vt = vtable_of(tex);
    g_orig_LockRect   = (LockRect_t)  patch_vtable_slot(
        vt, VT_TEX9_LOCKRECT,   (void*)hook_LockRect);
    g_orig_UnlockRect = (UnlockRect_t)patch_vtable_slot(
        vt, VT_TEX9_UNLOCKRECT, (void*)hook_UnlockRect);

    g_patched_texture = true;
    hook_log("  patched IDirect3DTexture9 vtable @ %p "
             "(LockRect=%p, UnlockRect=%p)\n",
             (void*)vt,
             (void*)g_orig_LockRect, (void*)g_orig_UnlockRect);
}

// ── Hooked IDirect3DDevice9::CreateTexture ─────────────────────────────────
static HRESULT STDMETHODCALLTYPE hook_CreateTexture(
    IDirect3DDevice9* self, UINT width, UINT height, UINT levels,
    DWORD usage, D3DFORMAT format, D3DPOOL pool,
    IDirect3DTexture9** out_tex, HANDLE* shared)
{
    HRESULT hr = g_orig_CreateTexture(self, width, height, levels, usage,
                                      format, pool, out_tex, shared);
    if (SUCCEEDED(hr) && out_tex && *out_tex) {
        hook_texture(*out_tex);
    }
    return hr;
}

// ── Hooked IDirect3DDevice9::SetTexture ────────────────────────────────────
// bof3ext-style texture swap: when the game binds a glyph texture we
// know about, swap in our pre-rendered TTF D3D9 texture. The game's
// draw call then renders our TTF instead of the game's native glyph.
// This is immune to the GoG DDraw wrapper overwriting texture data
// because it never touches our textures — it only manages the game's.
static HRESULT STDMETHODCALLTYPE hook_SetTexture(
    IDirect3DDevice9* self, DWORD stage, IDirect3DBaseTexture9* tex)
{
    if (stage == 0) {
        g_bound_tex = tex;
    }
    return g_orig_SetTexture(self, stage, tex);
}

// ── Shared logic: log a draw call of the currently bound texture ───────────
//
// Vertex position assumption: d3d9 games from 2003 overwhelmingly use FVF
// formats where XYZ or XYZRHW is the first 12/16 bytes of each vertex, so
// the first two floats are x and y. If BoF4 uses a more exotic format, we
// can refine this later by also hooking SetFVF. For a diagnostic log of
// advance widths, the first-two-floats assumption is good enough.
static inline void log_draw_if_glyph(const void* vertex_data, UINT stride)
{
    if (!vertex_data || stride < 8 || !g_bound_tex) return;
    // Only log small textures (likely glyphs) that have a known CRC.
    auto it = g_small_tex.find(
        reinterpret_cast<IDirect3DTexture9*>(g_bound_tex));
    if (it == g_small_tex.end()) return;
    if (g_draw_log_count.load(std::memory_order_relaxed) >= DRAW_LOG_MAX) return;

    const float* fv = static_cast<const float*>(vertex_data);
    float x = fv[0];
    float y = fv[1];

    // Plausibility gate — reject obviously broken/shader-space values.
    if (x < -2000.0f || x > 10000.0f || y < -2000.0f || y > 10000.0f) return;

    g_draw_log_count.fetch_add(1, std::memory_order_relaxed);
    (void)it; (void)x; (void)y;
}

// ── sprite_trace: log every draw (gated) to find the menu-label quads ──────
// Diagnostic for the title-menu un-clip work. Logs, deduped per
// (texture, quantized v0 x/y, prim_count, tex-width), each draw's primitive
// type/count, vertex stride, bound-texture dims, the first two vertices'
// (x,y), and a hexdump of vertex 0 so the FVF/UV layout can be decoded
// offline. Reveals whether the menu is many small label quads (=> override
// here) or one fullscreen blit (=> labels composited upstream in DirectDraw).
static std::unordered_set<uint64_t> g_dtr_seen;
static std::atomic<int> g_dtr_count{0};
static constexpr int DTR_CAP = 2000;

static bool dtr_read_vb(UINT start_vertex, UINT stride, BYTE* out, UINT n) {
    if (!g_bound_vb || stride < 8) return false;
    UINT off = g_bound_vb_offset + start_vertex * stride;
    void* d = nullptr;
    if (FAILED(g_bound_vb->Lock(off, n, &d, D3DLOCK_READONLY)) || !d) return false;
    memcpy(out, d, n);
    g_bound_vb->Unlock();
    return true;
}

// SEH-isolated (no C++ unwinding here, so __try is legal).
static void dtr_tex_dims(IDirect3DBaseTexture9* bt,
                         UINT* tw, UINT* th, unsigned* fmt)
{
    *tw = 0; *th = 0; *fmt = 0;
    if (!bt) return;
    __try {
        if (bt->GetType() == D3DRTYPE_TEXTURE) {
            D3DSURFACE_DESC d;
            if (SUCCEEDED(((IDirect3DTexture9*)bt)->GetLevelDesc(0, &d))) {
                *tw = d.Width; *th = d.Height; *fmt = (unsigned)d.Format;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void d3d_trace_draw(const char* tag, D3DPRIMITIVETYPE pt, UINT prim_count,
                           const void* vtx, UINT stride, UINT start_vertex,
                           void* caller)
{
    return;  // sprite_trace removed - inert
    if (g_dtr_count.load(std::memory_order_relaxed) >= DTR_CAP) return;
    if (stride < 8 || stride > 256) return;

    // Texture dims first — these are available even when the VB can't be locked.
    // The title menu is composited by sampling the emulated-VRAM texture (a big,
    // >=256px surface), so flag those as [BIG] and always log them (the earlier
    // tracer dropped them because their default-pool VB won't read-lock).
    UINT tw = 0, th = 0; unsigned fmt = 0;
    IDirect3DBaseTexture9* bt = g_bound_tex;
    dtr_tex_dims(bt, &tw, &th, &fmt);
    bool isBig = (tw >= 256 || th >= 256);

    BYTE buf[256];
    UINT take = stride * 2; if (take > sizeof(buf)) take = sizeof(buf);
    const BYTE* v = nullptr;
    if (vtx) v = (const BYTE*)vtx;
    else if (dtr_read_vb(start_vertex, stride, buf, take)) v = buf;
    bool haveV = (v != nullptr);

    // Log EVERY draw (deduped by caller/tex/primcount below) so the menu's
    // label quads from ddraw.dll (base 0x18000000) are captured even when their
    // VB can't be locked. caller pinpoints the renderer routine; tex dims tell
    // us which is the atlas. (Volume is bounded by the dedup + DTR_CAP.)
    float x0=0,y0=0,x1=0,y1=0, u0=0,vv0=0,u1=0,vv1=0;
    if (haveV) {
        x0 = ((const float*)v)[0]; y0 = ((const float*)v)[1];
        if (x0 < -4000.f || x0 > 12000.f || y0 < -4000.f || y0 > 12000.f)
            haveV = false;             // implausible coords — still log caller/tex
    }
    // Vertex seen so far is XYZRHW(16) | DIFFUSE(4) | TEX1(8): UV at byte +20.
    if (haveV && stride >= 28) {
        u0 = ((const float*)(v + 20))[0]; vv0 = ((const float*)(v + 20))[1];
        if (take >= stride + 28) {
            x1 = ((const float*)(v + stride))[0]; y1 = ((const float*)(v + stride))[1];
            u1 = ((const float*)(v + stride + 20))[0]; vv1 = ((const float*)(v + stride + 20))[1];
        }
    }

    // Dedup by (texture, source-UV, quad width) — NOT by x0/y0, which are a
    // constant local-space corner (-0.5,-0.5) here, so several labels sharing
    // one atlas texture would otherwise collapse to a single logged line.
    uint64_t key = ((uint64_t)(uintptr_t)bt)
                 ^ ((uint64_t)(int)(u0 * 4096.f) << 8)
                 ^ ((uint64_t)(int)(vv0 * 4096.f) << 26)
                 ^ ((uint64_t)(int)x1 << 40)
                 ^ ((uint64_t)tw << 52) ^ ((uint64_t)prim_count << 60);
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_dtr_seen.count(key)) return;
        g_dtr_seen.insert(key);
    }
    g_dtr_count.fetch_add(1, std::memory_order_relaxed);

    char hex[32 * 3 + 1]; int hb = haveV ? (stride < 32 ? (int)stride : 32) : 0;
    for (int i = 0; i < hb; ++i) snprintf(hex + i * 3, 4, "%02X ", v[i]);
    hex[hb * 3 ? hb * 3 - 1 : 0] = 0;

    hook_log("[d3d %s]%s caller=0x%08X pt=%d prims=%u stride=%u tex=%p %ux%u fmt=%u  "
             "pos0=(%.1f,%.1f) pos1=(%.1f,%.1f) uv0=(%.3f,%.3f) uv1=(%.3f,%.3f)%s%s\n",
             tag, isBig ? " [BIG]" : "", (unsigned)(uintptr_t)caller, (int)pt,
             prim_count, stride, (void*)bt, tw, th, fmt,
             x0, y0, x1, y1, u0, vv0, u1, vv1,
             haveV ? "  v0=" : "  (verts unreadable)", haveV ? hex : "");
}

// ── Hooked IDirect3DDevice9::DrawPrimitiveUP ───────────────────────────────
static HRESULT STDMETHODCALLTYPE hook_DrawPrimitiveUP(
    IDirect3DDevice9* self, D3DPRIMITIVETYPE prim_type, UINT prim_count,
    const void* vertex_data, UINT vertex_stride)
{
    void* caller = _ReturnAddress();
    log_draw_if_glyph(vertex_data, vertex_stride);
    d3d_trace_draw("DPUP", prim_type, prim_count, vertex_data, vertex_stride, 0, caller);
    return g_orig_DrawPrimitiveUP(self, prim_type, prim_count,
                                   vertex_data, vertex_stride);
}

// ── Hooked IDirect3DDevice9::DrawIndexedPrimitiveUP ────────────────────────
static HRESULT STDMETHODCALLTYPE hook_DrawIndexedPrimitiveUP(
    IDirect3DDevice9* self, D3DPRIMITIVETYPE prim_type, UINT min_vtx_idx,
    UINT num_verts, UINT prim_count, const void* index_data,
    D3DFORMAT index_fmt, const void* vertex_data, UINT vertex_stride)
{
    void* caller = _ReturnAddress();
    log_draw_if_glyph(vertex_data, vertex_stride);
    d3d_trace_draw("DIPUP", prim_type, prim_count, vertex_data, vertex_stride, 0, caller);
    return g_orig_DrawIndexedPrimitiveUP(self, prim_type, min_vtx_idx,
                                          num_verts, prim_count, index_data,
                                          index_fmt, vertex_data, vertex_stride);
}

// ── Hooked IDirect3DDevice9::SetStreamSource ───────────────────────────────
// Records the currently bound vertex buffer at stream 0 so the
// DrawPrimitive hook can read from it.
static HRESULT STDMETHODCALLTYPE hook_SetStreamSource(
    IDirect3DDevice9* self, UINT stream_number,
    IDirect3DVertexBuffer9* stream_data, UINT offset, UINT stride)
{
    if (stream_number == 0) {
        g_bound_vb        = stream_data;
        g_bound_vb_offset = offset;
        g_bound_vb_stride = stride;
    }
    return g_orig_SetStreamSource(self, stream_number, stream_data,
                                   offset, stride);
}

// ── Read one vertex from the bound VB ──────────────────────────────────────
// Reads 8 bytes (two little-endian floats: X then Y) at the specified
// vertex slot. Returns true on success. Attempts a read-only Lock on the
// currently bound vertex buffer, which succeeds for managed-pool and
// dynamic default-pool VBs.
static bool read_bound_vb_xy(UINT vertex_slot, float* out_x, float* out_y)
{
    if (!g_bound_vb || g_bound_vb_stride < 8) return false;
    UINT byte_offset = g_bound_vb_offset
                     + vertex_slot * g_bound_vb_stride;
    void* data = nullptr;
    HRESULT hr = g_bound_vb->Lock(byte_offset, 8, &data, D3DLOCK_READONLY);
    if (FAILED(hr) || !data) {
        return false;
    }
    *out_x = *((const float*)data + 0);
    *out_y = *((const float*)data + 1);
    g_bound_vb->Unlock();
    return true;
}

// ── Shared: log a draw from the bound VB (DrawPrimitive path) ──────────────
static inline void log_draw_from_vb(UINT start_vertex)
{
    if (!g_bound_tex) return;
    auto it = g_small_tex.find(
        reinterpret_cast<IDirect3DTexture9*>(g_bound_tex));
    if (it == g_small_tex.end()) return;
    if (g_draw_log_count.load(std::memory_order_relaxed) >= DRAW_LOG_MAX) return;

    float x = 0.0f, y = 0.0f;
    if (!read_bound_vb_xy(start_vertex, &x, &y)) {
        // Count failures so we can tell if the VB pool prevents read-locking.
        static std::atomic<int> fails{0};
        fails.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (x < -2000.0f || x > 10000.0f || y < -2000.0f || y > 10000.0f) return;

    g_draw_log_count.fetch_add(1, std::memory_order_relaxed);
    (void)it; (void)x; (void)y;
}

// ── Hooked IDirect3DDevice9::DrawPrimitive ─────────────────────────────────
static HRESULT STDMETHODCALLTYPE hook_DrawPrimitive(
    IDirect3DDevice9* self, D3DPRIMITIVETYPE prim_type,
    UINT start_vertex, UINT prim_count)
{
    void* caller = _ReturnAddress();
    log_draw_from_vb(start_vertex);
    d3d_trace_draw("DP", prim_type, prim_count, nullptr, g_bound_vb_stride, start_vertex, caller);
    return g_orig_DrawPrimitive(self, prim_type, start_vertex, prim_count);
}

// ── Hooked IDirect3DDevice9::DrawIndexedPrimitive ──────────────────────────
static HRESULT STDMETHODCALLTYPE hook_DrawIndexedPrimitive(
    IDirect3DDevice9* self, D3DPRIMITIVETYPE prim_type,
    INT base_vertex_idx, UINT min_vtx_idx, UINT num_vertices,
    UINT start_idx, UINT prim_count)
{
    // The earliest vertex referenced is base_vertex_idx + min_vtx_idx;
    // negative base values are legal per the D3D9 spec.
    void* caller = _ReturnAddress();
    INT first = base_vertex_idx + (INT)min_vtx_idx;
    if (first < 0) first = 0;
    log_draw_from_vb((UINT)first);
    d3d_trace_draw("DIP", prim_type, prim_count, nullptr, g_bound_vb_stride, (UINT)first, caller);
    return g_orig_DrawIndexedPrimitive(self, prim_type, base_vertex_idx,
                                        min_vtx_idx, num_vertices,
                                        start_idx, prim_count);
}

// ── Surface-copy blit trace (StretchRect / UpdateSurface / ColorFill) ──────
// The title menu is composited by ddraw.dll through these, NOT DrawPrimitive.
// Each label is a source-RECT -> dest-RECT copy; the source RECT width is the
// clip we must widen for German. Logging-only (deduped); reveals the per-label
// rects + the surface dims so we can map them to the rec06 atlas.
static std::unordered_set<uint64_t> g_blt_seen;
static std::atomic<int> g_blt_count{0};
static constexpr int BLT_CAP = 800;

// SEH-isolated (no C++ objects so __try is legal here).
static void blt_surf_dims(IDirect3DSurface9* s, UINT* w, UINT* h, unsigned* fmt) {
    *w = 0; *h = 0; *fmt = 0;
    if (!s) return;
    __try {
        D3DSURFACE_DESC d;
        if (SUCCEEDED(s->GetDesc(&d))) { *w = d.Width; *h = d.Height; *fmt = (unsigned)d.Format; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static bool seh_read_rect(const RECT* r, int* l, int* t, int* rr, int* b) {
    if (!r) return false;
    __try { *l=r->left; *t=r->top; *rr=r->right; *b=r->bottom; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

static void blt_trace(const char* tag, void* caller,
                      IDirect3DSurface9* src, const RECT* sr,
                      IDirect3DSurface9* dst, const RECT* dr,
                      int dpx, int dpy, unsigned extra) {
    return;  // sprite_trace removed - inert
    if (g_blt_count.load(std::memory_order_relaxed) >= BLT_CAP) return;

    int sl=0,st=0,srr=0,sb=0; bool hasS = seh_read_rect(sr,&sl,&st,&srr,&sb);
    int dl=0,dt=0,drr=0,db=0; bool hasD = seh_read_rect(dr,&dl,&dt,&drr,&db);

    UINT sw=0,sh=0,dw=0,dh=0; unsigned sf=0,df=0;
    blt_surf_dims(src,&sw,&sh,&sf);
    blt_surf_dims(dst,&dw,&dh,&df);

    int sW = hasS ? (srr-sl) : -1, sH = hasS ? (sb-st) : -1;
    uint64_t key = ((uint64_t)(uintptr_t)src)
                 ^ ((uint64_t)(uintptr_t)dst << 1)
                 ^ ((uint64_t)(sl & 0xFFF) << 16) ^ ((uint64_t)(st & 0xFFF) << 28)
                 ^ ((uint64_t)(sW & 0xFFF) << 40) ^ ((uint64_t)(dl & 0xFFF) << 52);
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_blt_seen.count(key)) return;
        g_blt_seen.insert(key);
    }
    g_blt_count.fetch_add(1, std::memory_order_relaxed);

    hook_log("[blt %s] caller=0x%08X src=%p %ux%u f=%u srcRECT=%s[%d,%d,%d,%d] (w=%d h=%d)  "
             "dst=%p %ux%u f=%u dstRECT=%s[%d,%d,%d,%d] dpt=(%d,%d) extra=%u\n",
             tag, (unsigned)(uintptr_t)caller, (void*)src, sw, sh, sf,
             hasS?"":"NULL", sl,st,srr,sb, sW,sH,
             (void*)dst, dw, dh, df, hasD?"":"NULL", dl,dt,drr,db, dpx,dpy, extra);
}

static HRESULT STDMETHODCALLTYPE hook_StretchRect(
    IDirect3DDevice9* self, IDirect3DSurface9* src, const RECT* sr,
    IDirect3DSurface9* dst, const RECT* dr, D3DTEXTUREFILTERTYPE filter)
{
    void* caller = _ReturnAddress();
    blt_trace("StretchRect", caller, src, sr, dst, dr, 0, 0, (unsigned)filter);
    return g_orig_StretchRect(self, src, sr, dst, dr, filter);
}

static HRESULT STDMETHODCALLTYPE hook_UpdateSurface(
    IDirect3DDevice9* self, IDirect3DSurface9* src, const RECT* sr,
    IDirect3DSurface9* dst, const POINT* dpt)
{
    void* caller = _ReturnAddress();
    int dx=0, dy=0; __try { if (dpt) { dx=dpt->x; dy=dpt->y; } } __except (EXCEPTION_EXECUTE_HANDLER) {}
    blt_trace("UpdateSurface", caller, src, sr, dst, nullptr, dx, dy, 0);
    return g_orig_UpdateSurface(self, src, sr, dst, dpt);
}

static HRESULT STDMETHODCALLTYPE hook_ColorFill(
    IDirect3DDevice9* self, IDirect3DSurface9* surf, const RECT* rc, D3DCOLOR color)
{
    void* caller = _ReturnAddress();
    blt_trace("ColorFill", caller, nullptr, nullptr, surf, rc, 0, 0, (unsigned)color);
    return g_orig_ColorFill(self, surf, rc, color);
}

// ── Hooked IDirect3DSwapChain9::Present ────────────────────────────────────
// The intro FMV presents through the swap chain (its current render target is
// an offscreen surface), so the subtitle overlay needs a tap here too. We
// draw onto the swap chain's own backbuffer.
// Device window remembered at CreateDevice so the Present hook can measure the
// real (sized) client rect — at CreateDevice time the window is often still 0x0.
HWND g_dev_window = nullptr;

// True while we're inside the device Present (which forwards to the implicit
// swap-chain Present, also hooked). Lets hook_SCPresent skip the Rect Tuner when
// it's just the nested call, so the tuner's per-frame work runs exactly once —
// while still catching the case where the game presents via the swap chain only.
static std::atomic<bool> g_tuner_device_presenting{ false };

static HRESULT STDMETHODCALLTYPE hook_SCPresent(
    IDirect3DSwapChain9* sc, const RECT* src, const RECT* dst, HWND hwnd,
    const RGNDATA* dirty, DWORD flags)
{
    // ── WHOSE swap chain is this? ──────────────────────────────────────────
    // The IDirect3DSwapChain9 vtable lives in d3d9.dll and is SHARED
    // PROCESS-WIDE — one patch catches every swap chain in the process, not
    // just the game's. When an overlay is injected (GOG Galaxy's, Steam's,
    // Discord's) it creates its own device and presents its own swap chain,
    // and every one of those calls landed here: we would create state blocks,
    // switch render targets and draw our HUD/subtitles on a device we do not
    // own, from a thread we do not control. Forward anything that isn't the
    // game's device straight through, untouched.
    IDirect3DDevice9* owner = nullptr;
    if (FAILED(sc->GetDevice(&owner)) || !owner) {
        if (owner) owner->Release();
        return g_orig_SCPresent(sc, src, dst, hwnd, dirty, flags);
    }
    if (owner != g_device) {
        owner->Release();
        static bool warned = false;
        if (!warned) {
            warned = true;
            hook_log("d3d9: a swap chain from ANOTHER device is presenting through "
                     "our (process-wide) vtable patch — an overlay is injected. "
                     "Skipping all per-frame work on it.\n");
        }
        return g_orig_SCPresent(sc, src, dst, hwnd, dirty, flags);
    }
    owner->Release();        // the game's device outlives this call

    g_present_call_count.fetch_add(1, std::memory_order_relaxed);
    if (g_cfg.subtitle_enabled) {
        IDirect3DDevice9* dev = nullptr;
        if (SUCCEEDED(sc->GetDevice(&dev)) && dev) {
            subtitles_on_present(dev, sc);
            dev->Release();
        }
    }
    // Rect Tuner: run here too, but ONLY when this isn't the nested call from
    // hook_Present (which forwards to this swap-chain Present). That way the
    // tuner's once-per-frame work fires exactly once whether the game presents
    // via the device OR straight through the swap chain.
    if (g_cfg.rect_tuner && !g_tuner_device_presenting.load(std::memory_order_relaxed)) {
        IDirect3DDevice9* dev = nullptr;
        if (SUCCEEDED(sc->GetDevice(&dev)) && dev) {
            rect_tuner_on_present(dev, sc);
            dev->Release();
        }
    }
    // Fast-forward / pause badge — draw here too for the swap-chain-only present
    // path, but skip the nested call from hook_Present so it draws once/frame.
    if ((g_cfg.fast_forward_enabled || g_cfg.pause_slowmo) && !g_tuner_device_presenting.load(std::memory_order_relaxed)) {
        IDirect3DDevice9* dev = nullptr;
        if (SUCCEEDED(sc->GetDevice(&dev)) && dev) {
            ff_overlay_on_present(dev, sc);
            dev->Release();
        }
    }
    // Config overlay — swap-chain-only present path (skip the nested call).
    if (g_cfg.config_gui && !g_tuner_device_presenting.load(std::memory_order_relaxed)) {
        IDirect3DDevice9* dev = nullptr;
        if (SUCCEEDED(sc->GetDevice(&dev)) && dev) {
            config_overlay_on_present(dev);
            dev->Release();
        }
    }
    // Standalone swap-chain present (not nested in hook_Present): tick the rect
    // lifetime counter here so it advances once/frame on this path too.
    if ((g_cfg.rect_trace || g_cfg.rect_fix || g_cfg.rect_tuner) &&
        !g_tuner_device_presenting.load(std::memory_order_relaxed))
        rect_hook_frame_tick();
    return g_orig_SCPresent(sc, src, dst, hwnd, dirty, flags);
}

// Patch the swap chain vtable's Present slot once. The swap chain class shares
// one vtable across instances (same as the device), so a single patch covers
// all of them.
static void hook_swapchain(IDirect3DSwapChain9* sc) {
    if (!sc || g_patched_swapchain) return;
    void** vt = vtable_of(sc);
    void* prev = patch_vtable_slot(vt, VT_SC9_PRESENT, (void*)hook_SCPresent);
    if (prev && !g_orig_SCPresent) g_orig_SCPresent = (SCPresent_t)prev;
    g_patched_swapchain = true;
    hook_log("  patched IDirect3DSwapChain9 vtable @ %p (Present slot3=%p)\n",
             (void*)vt, (void*)g_orig_SCPresent);
}

// ── Hooked IDirect3DDevice9::GetSwapChain ──────────────────────────────────
static HRESULT STDMETHODCALLTYPE hook_GetSwapChain(
    IDirect3DDevice9* self, UINT idx, IDirect3DSwapChain9** out_sc)
{
    HRESULT hr = g_orig_GetSwapChain(self, idx, out_sc);
    if (SUCCEEDED(hr) && out_sc && *out_sc) hook_swapchain(*out_sc);
    return hr;
}

// ── Hooked IDirect3DDevice9::CreateAdditionalSwapChain ──────────────────────
static HRESULT STDMETHODCALLTYPE hook_CreateAddlSC(
    IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* pp, IDirect3DSwapChain9** out_sc)
{
    HRESULT hr = g_orig_CreateAddlSC(self, pp, out_sc);
    if (SUCCEEDED(hr) && out_sc && *out_sc) hook_swapchain(*out_sc);
    return hr;
}

// ── Hooked IDirect3DDevice9::Reset ─────────────────────────────────────────
// The subtitle overlay's texture lives in D3DPOOL_DEFAULT and must be released
// before a Reset or the Reset fails with D3DERR_INVALIDCALL.
static HRESULT STDMETHODCALLTYPE hook_Reset(
    IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* pp)
{
    subtitles_on_lost_device();
    if (g_cfg.rect_tuner) rect_tuner_on_lost_device();
    if (g_cfg.fast_forward_enabled || g_cfg.pause_slowmo) ff_overlay_on_lost_device();
    if (g_cfg.config_gui) config_overlay_on_lost_device();
    return g_orig_Reset(self, pp);
}

// ── Hooked IDirect3DDevice9::Present ───────────────────────────────────────
// Counts every call (FF diagnostic) and draws the subtitle overlay when the
// game presents straight through the device rather than the swap chain.
std::atomic<uint64_t> g_present_call_count{0};
// Ticks on every BOF4!0x502070 packet submit — the GAME's own render queue.
// Together with the Present count this separates "frozen" from "still drawing".
std::atomic<uint64_t> g_submit_call_count{0};

static HRESULT STDMETHODCALLTYPE hook_Present(
    IDirect3DDevice9* self,
    const RECT* src, const RECT* dst, HWND hwnd, const RGNDATA* dirty)
{
    // Same ownership rule as hook_SCPresent — cheap insurance in case this
    // vtable is shared too. Never do per-frame work on a foreign device.
    if (g_device && self != g_device)
        return g_orig_Present(self, src, dst, hwnd, dirty);

    g_present_call_count.fetch_add(1, std::memory_order_relaxed);
    if (g_cfg.subtitle_enabled) subtitles_on_present(self, nullptr);
    if (g_cfg.rect_tuner)       rect_tuner_on_present(self, nullptr);
    if (g_cfg.fast_forward_enabled || g_cfg.pause_slowmo) ff_overlay_on_present(self, nullptr);
    if (g_cfg.config_gui)       config_overlay_on_present(self);
    g_tuner_device_presenting.store(true, std::memory_order_relaxed);
    HRESULT hr = g_orig_Present(self, src, dst, hwnd, dirty);
    g_tuner_device_presenting.store(false, std::memory_order_relaxed);
    if (g_cfg.rect_trace || g_cfg.rect_fix || g_cfg.rect_tuner)
        rect_hook_frame_tick();   // device-present path: one lifetime tick/frame
    name_slot_bp_tick();          // no-op unless the diagnostic is armed
    return hr;
}

// ── Install hooks on a device vtable ───────────────────────────────────────
static void hook_device(IDirect3DDevice9* dev) {
    if (!dev || g_patched_device) return;
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_patched_device) return;

    void** vt = vtable_of(dev);
    g_orig_CreateTexture = (CreateTexture_t)patch_vtable_slot(
        vt, VT_DEV9_CREATETEXTURE, (void*)hook_CreateTexture);
    g_orig_SetTexture = (SetTexture_t)patch_vtable_slot(
        vt, VT_DEV9_SETTEXTURE, (void*)hook_SetTexture);
    g_orig_SetStreamSource = (SetStreamSource_t)patch_vtable_slot(
        vt, VT_DEV9_SETSTREAMSOURCE, (void*)hook_SetStreamSource);
    g_orig_DrawPrimitive = (DrawPrimitive_t)patch_vtable_slot(
        vt, VT_DEV9_DRAWPRIMITIVE, (void*)hook_DrawPrimitive);
    g_orig_DrawIndexedPrimitive = (DrawIndexedPrimitive_t)patch_vtable_slot(
        vt, VT_DEV9_DRAWINDEXEDPRIMITIVE, (void*)hook_DrawIndexedPrimitive);
    g_orig_DrawPrimitiveUP = (DrawPrimitiveUP_t)patch_vtable_slot(
        vt, VT_DEV9_DRAWPRIMITIVEUP, (void*)hook_DrawPrimitiveUP);
    g_orig_DrawIndexedPrimitiveUP = (DrawIndexedPrimitiveUP_t)patch_vtable_slot(
        vt, VT_DEV9_DRAWINDEXEDPRIMITIVEUP,
        (void*)hook_DrawIndexedPrimitiveUP);
    // Surface-copy blit path (the title menu's actual compositing route).
    g_orig_StretchRect = (StretchRect_t)patch_vtable_slot(
        vt, VT_DEV9_STRETCHRECT, (void*)hook_StretchRect);
    g_orig_UpdateSurface = (UpdateSurface_t)patch_vtable_slot(
        vt, VT_DEV9_UPDATESURFACE, (void*)hook_UpdateSurface);
    g_orig_ColorFill = (ColorFill_t)patch_vtable_slot(
        vt, VT_DEV9_COLORFILL, (void*)hook_ColorFill);

    // Install the Present hook when either consumer needs it:
    //   - fast-forward: counts calls to confirm the 30fps vsync gate
    //   - subtitles:    draws the FMV overlay each frame
    // Zero-cost when neither is enabled (hook not installed at all).
    if (g_cfg.fast_forward_enabled || g_cfg.subtitle_enabled || g_cfg.rect_tuner
        || g_cfg.pause_slowmo) {
        g_orig_Present = (Present_t)patch_vtable_slot(
            vt, VT_DEV9_PRESENT, (void*)hook_Present);
    }

    // Subtitle overlay, the Rect Tuner AND the fast-forward badge also need the
    // swap-chain present path (the intro FMV — and possibly the menus — present
    // through it) and a Reset tap to drop the D3DPOOL_DEFAULT overlay/HUD textures.
    if (g_cfg.subtitle_enabled || g_cfg.rect_tuner || g_cfg.fast_forward_enabled
        || g_cfg.pause_slowmo) {
        g_orig_Reset = (Reset_t)patch_vtable_slot(
            vt, VT_DEV9_RESET, (void*)hook_Reset);
        g_orig_GetSwapChain = (GetSwapChain_t)patch_vtable_slot(
            vt, VT_DEV9_GETSWAPCHAIN, (void*)hook_GetSwapChain);
        g_orig_CreateAddlSC = (CreateAdditionalSwapChain_t)patch_vtable_slot(
            vt, VT_DEV9_CREATEADDLSC, (void*)hook_CreateAddlSC);
        hook_log("  subtitle: hooked Reset=%p GetSwapChain=%p "
                 "CreateAddlSC=%p Present=%p\n",
                 (void*)g_orig_Reset, (void*)g_orig_GetSwapChain,
                 (void*)g_orig_CreateAddlSC, (void*)g_orig_Present);
        // Grab the implicit swap chain (index 0) via the captured original
        // (not our hook, to avoid re-entering it) and patch its vtable now.
        if (g_orig_GetSwapChain) {
            IDirect3DSwapChain9* sc = nullptr;
            HRESULT hr = g_orig_GetSwapChain(dev, 0, &sc);
            if (SUCCEEDED(hr) && sc) { hook_swapchain(sc); sc->Release(); }
            else hook_log("  subtitle: implicit GetSwapChain failed hr=0x%08lX\n", hr);
        }
    }

    g_patched_device = true;
    hook_log("  patched IDirect3DDevice9 vtable @ %p\n"
             "    CreateTexture=%p\n"
             "    SetTexture=%p\n"
             "    SetStreamSource=%p\n"
             "    DrawPrimitive=%p\n"
             "    DrawIndexedPrimitive=%p\n"
             "    DrawPrimitiveUP=%p\n"
             "    DrawIndexedPrimitiveUP=%p\n",
             (void*)vt, (void*)g_orig_CreateTexture,
             (void*)g_orig_SetTexture, (void*)g_orig_SetStreamSource,
             (void*)g_orig_DrawPrimitive, (void*)g_orig_DrawIndexedPrimitive,
             (void*)g_orig_DrawPrimitiveUP,
             (void*)g_orig_DrawIndexedPrimitiveUP);
}

// ── Hooked IDirect3D9::CreateDevice ────────────────────────────────────────
static HRESULT STDMETHODCALLTYPE hook_CreateDevice(
    IDirect3D9* self, UINT adapter, D3DDEVTYPE type, HWND focus,
    DWORD behavior, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out_dev)
{
    // Log the requested presentation interval so we can see how the game
    // (or GoG ddraw wrapper) asks to sync frames. Known values:
    //   D3DPRESENT_INTERVAL_DEFAULT   = 0x00000000  (= INTERVAL_ONE)
    //   D3DPRESENT_INTERVAL_ONE       = 0x00000001
    //   D3DPRESENT_INTERVAL_IMMEDIATE = 0x80000000  (no vsync)
    // WHOSE device is being created? The IDirect3D9 vtable is in d3d9.dll and
    // shared process-wide, so an injected overlay creating its own device lands
    // here too — and used to overwrite g_device with it AND get our vsync
    // override applied to it. Adopt the FIRST device, and after that only ones
    // created from the SAME module, so the game can still legitimately recreate
    // its device while a foreign one never takes over.
    HMODULE caller = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)_ReturnAddress(), &caller);
    const bool adopt = (g_device == nullptr) || (caller == g_device_owner);

    UINT orig_interval = pp ? pp->PresentationInterval : 0xFFFFFFFFu;
    if (pp && g_cfg.unlock_vsync && adopt) {
        pp->PresentationInterval = 0x80000000u;  // IMMEDIATE
    }
    HRESULT hr = g_orig_CreateDevice(self, adapter, type, focus, behavior,
                                     pp, out_dev);
    if (SUCCEEDED(hr) && out_dev && *out_dev) {
        hook_log("CreateDevice adapter=%u type=%d -> %p  "
                 "PresentationInterval=0x%08X%s\n",
                 adapter, (int)type, (void*)*out_dev, orig_interval,
                 (pp && g_cfg.unlock_vsync) ? " (overridden -> IMMEDIATE)" : "");
        // Aspect diagnosis: log the present target geometry. The 640x480 (4:3)
        // emulated framebuffer is scaled to the backbuffer/window here — a
        // non-4:3 target is what squishes circles into ellipses.
        if (pp) {
            HWND wnd = pp->hDeviceWindow ? pp->hDeviceWindow : focus;
            g_dev_window = wnd;
            RECT cr = {0,0,0,0};
            if (wnd) GetClientRect(wnd, &cr);
            int scrW = GetSystemMetrics(SM_CXSCREEN);
            int scrH = GetSystemMetrics(SM_CYSCREEN);
            int bbw = (int)pp->BackBufferWidth, bbh = (int)pp->BackBufferHeight;
            double bb_ar  = bbh ? (double)bbw / bbh : 0.0;
            int clw = cr.right - cr.left, clh = cr.bottom - cr.top;
            double cl_ar  = clh ? (double)clw / clh : 0.0;
            hook_log("  d3dpp: BackBuffer=%dx%d (AR=%.4f) fmt=%d count=%u "
                     "Windowed=%d SwapEffect=%d hDeviceWindow=%p refresh=%uHz\n"
                     "         window client=%dx%d (AR=%.4f)  screen=%dx%d (AR=%.4f)\n"
                     "         [4:3=1.3333  16:9=1.7778]  framebuffer is 640x480=1.3333\n",
                     bbw, bbh, bb_ar, (int)pp->BackBufferFormat,
                     pp->BackBufferCount, pp->Windowed, (int)pp->SwapEffect,
                     (void*)pp->hDeviceWindow,
                     pp->FullScreen_RefreshRateInHz,
                     clw, clh, cl_ar, scrW, scrH,
                     scrH ? (double)scrW / scrH : 0.0);
        }
        if (adopt) {
            g_device = *out_dev;
            g_device_owner = caller;
            hook_device(*out_dev);
        } else {
            char mod[MAX_PATH] = "?";
            if (caller) GetModuleFileNameA(caller, mod, MAX_PATH);
            hook_log("  ^ NOT the game's device (created by %s) — left completely "
                     "alone: no vtable patch, no vsync override, no per-frame work.\n",
                     mod);
        }
    }
    return hr;
}

// ── Install hooks on an IDirect3D9 vtable ──────────────────────────────────
void hook_d3d9(IDirect3D9* obj) {
    if (!obj || g_patched_d3d9) return;
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_patched_d3d9) return;

    void** vt = vtable_of(obj);
    g_orig_CreateDevice = (CreateDevice_t)patch_vtable_slot(
        vt, VT_D3D9_CREATEDEVICE, (void*)hook_CreateDevice);

    g_patched_d3d9 = true;
    hook_log("  patched IDirect3D9 vtable @ %p "
             "(CreateDevice=%p)\n",
             (void*)vt, (void*)g_orig_CreateDevice);
}
