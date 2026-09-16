// ff_overlay.cpp — see ff_overlay.h. A minimal, self-contained top-right speed
// badge for the fast-forward feature. Renders "> Nx" (with a translucent pill
// behind it) via GDI into an A8R8G8B8 texture, then blits it with a single
// DrawPrimitiveUP in the Present hook. Mirrors the proven state-block approach
// used by the subtitle overlay, but keeps its own texture/statics.
#include "ff_overlay.h"

#include <d3d9.h>
#include <windows.h>
#include <atomic>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cmath>

#include "hooks.h"        // hook_log

// ── overlay state ─────────────────────────────────────────────────────────
static std::atomic<float>    s_scale{1.0f};      // current multiplier
static std::atomic<uint64_t> s_flash_until{0};   // wall-clock ms (unhooked GetTickCount64)

static IDirect3DTexture9* s_tex   = nullptr;
static int                s_texW  = 0;
static int                s_texH  = 0;
static std::wstring       s_cached;              // text currently in s_tex

void ff_overlay_set(float scale) {
    s_scale.store(scale);
    // Flash even when returning to 1x so "1x" is briefly visible.
    s_flash_until.store(GetTickCount64() + 900);
}

void ff_overlay_on_lost_device() {
    if (s_tex) { s_tex->Release(); s_tex = nullptr; }
    s_texW = s_texH = 0;
    s_cached.clear();
}

// ── GDI text -> ARGB (white text on a translucent dark pill) ───────────────
static uint32_t* render_argb(const wchar_t* text, int* outW, int* outH) {
    const int   fontPx = 30;
    const int   padX = 14, padY = 7;
    const wchar_t* fontName = L"Segoe UI";

    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return nullptr;
    HFONT font = CreateFontW(
        fontPx, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, FF_DONTCARE | DEFAULT_PITCH, fontName);
    if (!font) { DeleteDC(dc); return nullptr; }
    HFONT oldFont = (HFONT)SelectObject(dc, font);

    RECT mr = { 0, 0, 0, 0 };
    DrawTextW(dc, text, -1, &mr, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    int tw = mr.right - mr.left;
    int th = mr.bottom - mr.top;
    if (tw <= 0) tw = fontPx;
    if (th <= 0) th = fontPx;

    const int w = tw + 2 * padX;
    const int h = th + 2 * padY;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -h;   // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) { SelectObject(dc, oldFont); DeleteObject(font); DeleteDC(dc); return nullptr; }
    HBITMAP oldBmp = (HBITMAP)SelectObject(dc, dib);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT tr = { padX, padY, w - padX, h - padY };
    DrawTextW(dc, text, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    GdiFlush();

    const int   N   = w * h;
    uint32_t*   out = new uint32_t[N];
    const uint32_t bg = 0xB0141414u;         // translucent near-black pill
    const uint32_t* src = reinterpret_cast<const uint32_t*>(bits);
    for (int i = 0; i < N; ++i) {
        uint32_t p = src[i];
        uint32_t b = (p      ) & 0xFFu;
        uint32_t g = (p >>  8) & 0xFFu;
        uint32_t r = (p >> 16) & 0xFFu;
        uint32_t cov = r; if (g > cov) cov = g; if (b > cov) cov = b;  // text coverage
        if (cov == 0) { out[i] = bg; continue; }
        // Composite white text (opaque) over the pill by coverage.
        int ba = (bg >> 24) & 0xFF, br = (bg >> 16) & 0xFF, bgc = (bg >> 8) & 0xFF, bb = bg & 0xFF;
        int sa = 255, sr = 255, sg = 255, sb = 255;
        int inv = 255 - (int)cov;
        int oa = (sa * (int)cov + ba * inv + 127) / 255;
        int orr = (sr * (int)cov + br * inv + 127) / 255;
        int og = (sg * (int)cov + bgc * inv + 127) / 255;
        int ob = (sb * (int)cov + bb * inv + 127) / 255;
        out[i] = ((uint32_t)oa << 24) | ((uint32_t)orr << 16) | ((uint32_t)og << 8) | (uint32_t)ob;
    }

    SelectObject(dc, oldBmp); DeleteObject(dib);
    SelectObject(dc, oldFont); DeleteObject(font);
    DeleteDC(dc);

    *outW = w; *outH = h;
    return out;
}

static bool upload(IDirect3DDevice9* dev, uint32_t* argb, int w, int h) {
    if (s_tex && (w != s_texW || h != s_texH)) { s_tex->Release(); s_tex = nullptr; }
    if (!s_tex) {
        HRESULT hr = dev->CreateTexture(
            w, h, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &s_tex, nullptr);
        if (FAILED(hr) || !s_tex) return false;
        s_texW = w; s_texH = h;
    }
    D3DLOCKED_RECT lr{};
    if (FAILED(s_tex->LockRect(0, &lr, nullptr, D3DLOCK_DISCARD))) return false;
    uint8_t* dst = reinterpret_cast<uint8_t*>(lr.pBits);
    for (int y = 0; y < h; ++y) memcpy(dst + y * lr.Pitch, argb + y * w, (size_t)w * 4);
    s_tex->UnlockRect(0);
    return true;
}

struct FfVertex { float x, y, z, rhw; D3DCOLOR color; float u, v; };
static const DWORD kFfFVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

static void draw_quad(IDirect3DDevice9* dev, IDirect3DSwapChain9* sc) {
    // Backbuffer size (prefer the swap chain's when drawing via the SC path).
    IDirect3DSurface9* bb = nullptr;
    if (sc) sc->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (!bb) dev->GetRenderTarget(0, &bb);
    if (!bb) return;
    D3DSURFACE_DESC d{};
    if (FAILED(bb->GetDesc(&d))) { bb->Release(); return; }
    const int bbW = (int)d.Width, bbH = (int)d.Height;

    const float margin = 16.0f;
    const float x1 = bbW - margin;
    const float x0 = x1 - s_texW;
    const float y0 = margin;
    const float y1 = y0 + s_texH;
    const float o  = -0.5f;   // pixel-center correction

    FfVertex vtx[4] = {
        { x0 + o, y0 + o, 0.0f, 1.0f, 0xFFFFFFFF, 0.0f, 0.0f },
        { x1 + o, y0 + o, 0.0f, 1.0f, 0xFFFFFFFF, 1.0f, 0.0f },
        { x0 + o, y1 + o, 0.0f, 1.0f, 0xFFFFFFFF, 0.0f, 1.0f },
        { x1 + o, y1 + o, 0.0f, 1.0f, 0xFFFFFFFF, 1.0f, 1.0f },
    };

    IDirect3DStateBlock9* sb = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb)) || !sb) { bb->Release(); return; }

    // Render target and depth-stencil are NOT captured by state blocks in D3D9,
    // so save/restore them manually — otherwise the game's next frame renders to
    // the wrong target / with no depth buffer and the whole scene corrupts.
    IDirect3DSurface9* oldRT = nullptr;
    IDirect3DSurface9* oldDS = nullptr;
    dev->GetRenderTarget(0, &oldRT);
    dev->GetDepthStencilSurface(&oldDS);   // may leave oldDS null if none bound

    dev->SetRenderTarget(0, bb);
    dev->SetDepthStencilSurface(nullptr);
    D3DVIEWPORT9 vp{ 0, 0, (DWORD)bbW, (DWORD)bbH, 0.0f, 1.0f };
    dev->SetViewport(&vp);

    dev->SetPixelShader(nullptr);
    dev->SetVertexShader(nullptr);
    dev->SetFVF(kFfFVF);
    dev->SetStreamSource(0, nullptr, 0, 0);
    dev->SetIndices(nullptr);
    dev->SetTexture(0, s_tex);

    dev->SetRenderState(D3DRS_LIGHTING,          FALSE);
    dev->SetRenderState(D3DRS_CULLMODE,          D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ZENABLE,           D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE,      FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE,  TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND,          D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND,         D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE,   FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE,         FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE,     FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE,
        D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
        D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
    dev->SetRenderState(D3DRS_CLIPPING,          TRUE);
    dev->SetRenderState(D3DRS_CLIPPLANEENABLE,   0);

    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
    dev->SetTextureStageState(1, D3DTSS_COLOROP,   D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP,   D3DTOP_DISABLE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU,  D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV,  D3DTADDRESS_CLAMP);

    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vtx, sizeof(FfVertex));

    // Restore render target + depth-stencil (state block doesn't cover these).
    dev->SetRenderTarget(0, oldRT);
    dev->SetDepthStencilSurface(oldDS);   // OK if null — clears the DS binding
    if (oldRT) oldRT->Release();
    if (oldDS) oldDS->Release();

    sb->Apply();
    sb->Release();
    bb->Release();
}

void ff_overlay_on_present(IDirect3DDevice9* dev, IDirect3DSwapChain9* sc) {
    if (!dev) return;
    float scale = s_scale.load();
    // Persistent while time is off-normal (freeze scale==0 / slow-mo <1 / FF >1);
    // otherwise only during the brief flash.
    bool visible = (scale != 1.0f) || (GetTickCount64() < s_flash_until.load());
    if (!visible) return;

    wchar_t buf[32];
    // ASCII-only badge so there's no source-charset / font-glyph risk.
    if (scale == 0.0f) {
        swprintf(buf, 32, L"|| FROZEN");    // scale-0 freeze + rect latch
    } else if (scale < 1.0f) {
        swprintf(buf, 32, L"|| SLOW");      // slow-mo helper
    } else {
        // Fast-forward / 1x flash. Produces ">> 4x", ">> 16x", ">> 1x".
        swprintf(buf, 32, L">> %gx", (double)scale);
    }

    if (s_cached != buf || !s_tex) {
        int w = 0, h = 0;
        uint32_t* argb = render_argb(buf, &w, &h);
        if (argb) {
            if (upload(dev, argb, w, h)) s_cached = buf;
            delete[] argb;
        }
    }
    if (s_tex) draw_quad(dev, sc);
}
