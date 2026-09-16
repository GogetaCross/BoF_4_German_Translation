// Soft-subtitle overlay for the BoF4 intro FMV. See subtitles.h for the
// integration contract. This file is a merge of the three source files from
// the standalone bof4-subtitles-hook (srt.cpp, overlay.cpp, file_hook.cpp),
// adapted to read its settings from the shared HookConfig (g_cfg) and to log
// through hook_log instead of the standalone logger. The movie-detection IAT
// patch is gone: the host taps subtitles_note_file_open/close into its
// existing CreateFile/CloseHandle trampolines.

#include "subtitles.h"
#include "hooks.h"          // g_cfg, hook_log

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace {

// ── Logging shim ──────────────────────────────────────────────────────────
// The standalone hook used BOF4SUB_LOG (added its own newline). hook_log does
// not, so format into a scratch buffer and emit one prefixed line.
void sublog(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    hook_log("subtitle: %s\n", buf);
}

// ── Config mirror ─────────────────────────────────────────────────────────
// Populated from g_cfg in subtitles_init(). Colors are 0xAARRGGBB.
struct SubCfg {
    bool         enabled        = false;
    std::wstring srtPath;                       // absolute, resolved
    std::wstring fontName       = L"Comic Sans MS";
    float        fontScale      = 1.2f;
    float        bottomMargin   = 0.10f;
    int          timeOffsetMs   = 0;
    unsigned int fontColor      = 0xFFFFFFFFu;
    unsigned int outlineColor   = 0xFF000000u;
    int          outlinePx      = 2;
    bool         shadowEnabled  = true;
    int          shadowOffsetX  = 3;
    int          shadowOffsetY  = 3;
    int          shadowSoftness = 2;
    unsigned int shadowColor    = 0xC0000000u;
};
SubCfg g_sub;
const SubCfg& Cfg() { return g_sub; }

// Directory that BOF4.exe lives in, for resolving relative SRT paths.
std::wstring InstallDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L".";
    wchar_t* slash = wcsrchr(buf, L'\\');
    if (slash) *slash = L'\0';
    return buf;
}

std::wstring ResolvePath(const std::wstring& p) {
    if (p.empty()) return p;
    if (p.size() >= 2 && (p[1] == L':' || p[0] == L'\\' || p[0] == L'/')) return p;
    return InstallDir() + L'\\' + p;
}

std::wstring WidenUtf8OrAnsi(const char* s) {
    if (!s || !*s) return std::wstring();
    int want = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, nullptr, 0);
    UINT cp = CP_UTF8;
    if (want <= 0) { cp = CP_ACP; want = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0); }
    std::wstring out;
    if (want > 0) {
        out.resize((size_t)want);
        MultiByteToWideChar(cp, 0, s, -1, out.data(), want);
        if (!out.empty() && out.back() == L'\0') out.pop_back();
    }
    return out;
}

// ════════════════════════════════════════════════════════════════════════
//  SRT parsing  (was srt.cpp)
// ════════════════════════════════════════════════════════════════════════

struct Cue {
    uint32_t     startMs;
    uint32_t     endMs;
    std::wstring text;
};

std::vector<Cue> s_cues;

std::string ReadFileBytes(const wchar_t* path) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        sublog("LoadSrt: CreateFile failed for \"%ls\" gle=%lu", path, GetLastError());
        return {};
    }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (1 << 24)) {
        sublog("LoadSrt: bad size %lld", (long long)sz.QuadPart);
        CloseHandle(h);
        return {};
    }
    std::string buf;
    buf.resize((size_t)sz.QuadPart);
    DWORD read = 0;
    BOOL ok = ReadFile(h, buf.data(), (DWORD)buf.size(), &read, nullptr);
    CloseHandle(h);
    if (!ok || read != buf.size()) return {};
    return buf;
}

std::wstring DecodeText(std::string_view bytes) {
    if (bytes.size() >= 3 &&
        (uint8_t)bytes[0] == 0xEF && (uint8_t)bytes[1] == 0xBB && (uint8_t)bytes[2] == 0xBF) {
        bytes.remove_prefix(3);
    }
    int want = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                   bytes.data(), (int)bytes.size(), nullptr, 0);
    UINT cp = CP_UTF8;
    if (want <= 0) {
        want = MultiByteToWideChar(CP_ACP, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
        cp = CP_ACP;
    }
    std::wstring out;
    if (want > 0) {
        out.resize((size_t)want);
        MultiByteToWideChar(cp, 0, bytes.data(), (int)bytes.size(), out.data(), want);
    }
    return out;
}

bool ParseTimecode(const wchar_t* s, uint32_t* ms) {
    unsigned h=0, m=0, sec=0, mms=0;
    if (swscanf_s(s, L"%u:%u:%u,%u", &h, &m, &sec, &mms) == 4 ||
        swscanf_s(s, L"%u:%u:%u.%u", &h, &m, &sec, &mms) == 4) {
        *ms = ((h*3600u + m*60u + sec)*1000u) + mms;
        return true;
    }
    return false;
}

std::vector<std::wstring> SplitLines(const std::wstring& src) {
    std::vector<std::wstring> out;
    size_t i = 0;
    while (i <= src.size()) {
        size_t nl = src.find(L'\n', i);
        if (nl == std::wstring::npos) nl = src.size();
        std::wstring line = src.substr(i, nl - i);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        out.emplace_back(std::move(line));
        i = nl + 1;
    }
    return out;
}

bool LoadSrt(const wchar_t* path) {
    s_cues.clear();

    std::string bytes = ReadFileBytes(path);
    if (bytes.empty()) return false;
    std::wstring text = DecodeText(bytes);
    if (text.empty()) { sublog("LoadSrt: decode produced empty text"); return false; }

    auto lines = SplitLines(text);

    Cue cur{};
    enum { S_Idle, S_TextLine } state = S_Idle;

    auto flush = [&]() {
        if (state == S_TextLine && !cur.text.empty() && cur.endMs > cur.startMs) {
            while (!cur.text.empty() && (cur.text.back() == L'\n' || cur.text.back() == L'\r'))
                cur.text.pop_back();
            s_cues.push_back(std::move(cur));
        }
        cur = Cue{};
        state = S_Idle;
    };

    for (const auto& raw : lines) {
        size_t arrow = raw.find(L"-->");
        if (arrow != std::wstring::npos) {
            std::wstring leftRaw  = raw.substr(0, arrow);
            std::wstring rightRaw = raw.substr(arrow + 3);
            auto trimw = [](std::wstring& s) {
                while (!s.empty() && (s.front() == L' ' || s.front() == L'\t')) s.erase(s.begin());
                while (!s.empty() && (s.back()  == L' ' || s.back()  == L'\t')) s.pop_back();
            };
            trimw(leftRaw); trimw(rightRaw);
            size_t sp = rightRaw.find_first_of(L" \t");
            if (sp != std::wstring::npos) rightRaw.resize(sp);

            uint32_t a=0, b=0;
            if (ParseTimecode(leftRaw.c_str(), &a) && ParseTimecode(rightRaw.c_str(), &b)) {
                flush();
                cur.startMs = a;
                cur.endMs   = b;
                cur.text.clear();
                state = S_TextLine;
                continue;
            }
        }

        if (state == S_TextLine) {
            if (raw.empty()) {
                flush();
            } else {
                if (!cur.text.empty()) cur.text += L'\n';
                cur.text += raw;
            }
        }
    }
    flush();

    std::sort(s_cues.begin(), s_cues.end(),
              [](const Cue& a, const Cue& b) { return a.startMs < b.startMs; });

    sublog("LoadSrt: parsed %zu cues from \"%ls\"", s_cues.size(), path);
    if (!s_cues.empty()) {
        sublog("  first: [%ums .. %ums] \"%ls\"",
               s_cues.front().startMs, s_cues.front().endMs, s_cues.front().text.c_str());
        sublog("  last : [%ums .. %ums] \"%ls\"",
               s_cues.back().startMs, s_cues.back().endMs, s_cues.back().text.c_str());
    }
    return !s_cues.empty();
}

const Cue* ActiveCue(uint32_t elapsedMs) {
    for (const auto& c : s_cues) {
        if (elapsedMs < c.startMs) return nullptr;
        if (elapsedMs <= c.endMs)  return &c;
    }
    return nullptr;
}

// ════════════════════════════════════════════════════════════════════════
//  Overlay renderer  (was overlay.cpp)
// ════════════════════════════════════════════════════════════════════════

struct QuadVertex {
    float    x, y, z, rhw;
    D3DCOLOR color;
    float    u, v;
};
constexpr DWORD kQuadFVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

IDirect3DTexture9* s_tex        = nullptr;
int                s_texW       = 0;
int                s_texH       = 0;
int                s_bbW        = 0;
int                s_bbH        = 0;
std::wstring       s_cachedText;
bool               s_warnedOnce = false;

uint8_t* RenderTextMask(const wchar_t* text, int w, int h, int fontPx,
                        const wchar_t* fontName, int ox, int oy) {
    HDC screen = GetDC(nullptr);
    HDC memDC  = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!memDC) return nullptr;

    HFONT font = CreateFontW(
        fontPx, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, FF_DONTCARE | DEFAULT_PITCH, fontName);
    if (!font) { DeleteDC(memDC); return nullptr; }
    HFONT oldFont = (HFONT)SelectObject(memDC, font);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -h;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) { SelectObject(memDC, oldFont); DeleteObject(font); DeleteDC(memDC); return nullptr; }
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, dib);

    SetBkMode(memDC, TRANSPARENT);
    SetTextColor(memDC, RGB(255, 255, 255));
    RECT rr = { ox, oy, w + ox, h + oy };
    DrawTextW(memDC, text, -1, &rr, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
    GdiFlush();

    uint8_t* mask = new uint8_t[w * h];
    const uint32_t* src = reinterpret_cast<const uint32_t*>(bits);
    for (int i = 0, n = w * h; i < n; ++i) {
        uint32_t p = src[i];
        uint32_t b = (p      ) & 0xFFu;
        uint32_t g = (p >>  8) & 0xFFu;
        uint32_t r = (p >> 16) & 0xFFu;
        uint32_t a = r; if (g > a) a = g; if (b > a) a = b;
        mask[i] = (uint8_t)a;
    }

    SelectObject(memDC, oldBmp); DeleteObject(dib);
    SelectObject(memDC, oldFont); DeleteObject(font);
    DeleteDC(memDC);
    return mask;
}

void DilateMask(uint8_t* mask, int w, int h, int radius) {
    if (radius <= 0) return;
    std::vector<uint8_t> tmp((size_t)w * h, 0);
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = mask + y * w;
        uint8_t* dst = tmp.data() + y * w;
        for (int x = 0; x < w; ++x) {
            int x0 = std::max(0, x - radius);
            int x1 = std::min(w - 1, x + radius);
            uint8_t m = 0;
            for (int i = x0; i <= x1; ++i) if (row[i] > m) m = row[i];
            dst[x] = m;
        }
    }
    for (int y = 0; y < h; ++y) {
        uint8_t* dst = mask + y * w;
        int y0 = std::max(0, y - radius);
        int y1 = std::min(h - 1, y + radius);
        for (int x = 0; x < w; ++x) {
            uint8_t m = 0;
            for (int i = y0; i <= y1; ++i) {
                uint8_t v = tmp[i * w + x];
                if (v > m) m = v;
            }
            dst[x] = m;
        }
    }
}

inline void UnpackArgb(uint32_t c, int* a, int* r, int* g, int* b) {
    *a = (c >> 24) & 0xFF; *r = (c >> 16) & 0xFF; *g = (c >> 8) & 0xFF; *b = c & 0xFF;
}

void CompositeLayer(uint32_t* dst, const uint8_t* mask, int count, uint32_t argb) {
    int la, lr, lg, lb;
    UnpackArgb(argb, &la, &lr, &lg, &lb);
    if (la == 0) return;
    for (int i = 0; i < count; ++i) {
        int cov = mask[i];
        if (cov == 0) continue;
        int srcA = (la * cov + 127) / 255;
        if (srcA == 0) continue;

        int da, dr, dg, db;
        UnpackArgb(dst[i], &da, &dr, &dg, &db);

        int invA = 255 - srcA;
        int outA = srcA + (da * invA + 127) / 255;
        int outR = (lr * srcA + dr * invA + 127) / 255;
        int outG = (lg * srcA + dg * invA + 127) / 255;
        int outB = (lb * srcA + db * invA + 127) / 255;

        dst[i] = ((uint32_t)outA << 24) | ((uint32_t)outR << 16) |
                 ((uint32_t)outG << 8)  | (uint32_t)outB;
    }
}

uint32_t* RenderTextToArgb(const wchar_t* text, int bbW, int bbH, int* outW, int* outH) {
    *outW = 0; *outH = 0;
    const SubCfg& cfg = Cfg();

    const int outlinePx   = std::clamp(cfg.outlinePx, 0, 32);
    const int shadowReach = cfg.shadowEnabled
        ? std::max(std::abs(cfg.shadowOffsetX), std::abs(cfg.shadowOffsetY)) + cfg.shadowSoftness
        : 0;
    const int pad = outlinePx + shadowReach + 2;
    const int texW = std::max(256, (bbW * 8) / 10) + 2 * pad;
    int fontPx = std::max(12, (int)(bbH * 0.05f * cfg.fontScale));

    HDC tmpDC = CreateCompatibleDC(nullptr);
    HFONT measureFont = CreateFontW(
        fontPx, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, FF_DONTCARE | DEFAULT_PITCH, cfg.fontName.c_str());
    HFONT oldMF = (HFONT)SelectObject(tmpDC, measureFont);
    RECT mr = { 0, 0, texW - 2*pad, 0 };
    DrawTextW(tmpDC, text, -1, &mr, DT_CENTER | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
    int textH = std::max(fontPx + 4, (int)(mr.bottom - mr.top));
    SelectObject(tmpDC, oldMF);
    DeleteObject(measureFont);
    DeleteDC(tmpDC);
    const int texH = textH + 2 * pad;
    const int N    = texW * texH;

    uint8_t* fill = RenderTextMask(text, texW, texH, fontPx, cfg.fontName.c_str(), 0, pad);
    if (!fill) return nullptr;

    uint8_t* outline = nullptr;
    if (outlinePx > 0 && (cfg.outlineColor >> 24) != 0) {
        outline = new uint8_t[N];
        memcpy(outline, fill, N);
        DilateMask(outline, texW, texH, outlinePx);
        for (int i = 0; i < N; ++i) {
            int v = (int)outline[i] - (int)fill[i];
            outline[i] = (uint8_t)(v < 0 ? 0 : v);
        }
    }

    uint8_t* shadow = nullptr;
    if (cfg.shadowEnabled && (cfg.shadowColor >> 24) != 0) {
        shadow = RenderTextMask(text, texW, texH, fontPx, cfg.fontName.c_str(),
                                cfg.shadowOffsetX, pad + cfg.shadowOffsetY);
        if (shadow && cfg.shadowSoftness > 0)
            DilateMask(shadow, texW, texH, cfg.shadowSoftness);
    }

    uint32_t* out = new uint32_t[N];
    memset(out, 0, (size_t)N * 4);
    if (shadow)  CompositeLayer(out, shadow,  N, cfg.shadowColor);
    if (outline) CompositeLayer(out, outline, N, cfg.outlineColor);
    CompositeLayer(out, fill, N, cfg.fontColor);

    delete[] shadow;
    delete[] outline;
    delete[] fill;

    *outW = texW; *outH = texH;
    return out;
}

IDirect3DSurface9* AcquireBackBuffer(IDirect3DDevice9* dev, IDirect3DSwapChain9* sc,
                                     int* w, int* h) {
    IDirect3DSurface9* surf = nullptr;
    if (sc) sc->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &surf);
    if (!surf) dev->GetRenderTarget(0, &surf);
    if (!surf) return nullptr;
    D3DSURFACE_DESC d{};
    if (FAILED(surf->GetDesc(&d))) { surf->Release(); return nullptr; }
    *w = (int)d.Width;
    *h = (int)d.Height;
    return surf;
}

bool UploadTexture(IDirect3DDevice9* dev, uint32_t* argb, int w, int h) {
    if (s_tex && (w != s_texW || h != s_texH)) { s_tex->Release(); s_tex = nullptr; }
    if (!s_tex) {
        HRESULT hr = dev->CreateTexture(
            w, h, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &s_tex, nullptr);
        if (FAILED(hr) || !s_tex) {
            if (!s_warnedOnce) sublog("CreateTexture failed hr=0x%08lx", hr);
            s_warnedOnce = true;
            return false;
        }
        s_texW = w; s_texH = h;
    }

    D3DLOCKED_RECT lr{};
    HRESULT hr = s_tex->LockRect(0, &lr, nullptr, D3DLOCK_DISCARD);
    if (FAILED(hr)) { sublog("Texture LockRect failed hr=0x%08lx", hr); return false; }
    uint8_t* dst = reinterpret_cast<uint8_t*>(lr.pBits);
    for (int y = 0; y < h; ++y) memcpy(dst + y * lr.Pitch, argb + y * w, w * 4);
    s_tex->UnlockRect(0);
    return true;
}

void DrawQuad(IDirect3DDevice9* dev, IDirect3DSurface9* bb, int bbW, int bbH) {
    const float margin = std::max(4.0f, bbH * Cfg().bottomMargin);
    const float x0 = (bbW - s_texW) * 0.5f;
    const float y0 = bbH - s_texH - margin;
    const float x1 = x0 + s_texW;
    const float y1 = y0 + s_texH;

    const float o  = -0.5f;
    QuadVertex vtx[4] = {
        { x0 + o, y0 + o, 0.0f, 1.0f, 0xFFFFFFFF, 0.0f, 0.0f },
        { x1 + o, y0 + o, 0.0f, 1.0f, 0xFFFFFFFF, 1.0f, 0.0f },
        { x0 + o, y1 + o, 0.0f, 1.0f, 0xFFFFFFFF, 0.0f, 1.0f },
        { x1 + o, y1 + o, 0.0f, 1.0f, 0xFFFFFFFF, 1.0f, 1.0f },
    };

    IDirect3DStateBlock9* sb = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb)) || !sb) return;

    dev->SetRenderTarget(0, bb);
    dev->SetDepthStencilSurface(nullptr);

    D3DVIEWPORT9 vp{ 0, 0, (DWORD)bbW, (DWORD)bbH, 0.0f, 1.0f };
    dev->SetViewport(&vp);
    RECT scissor{ 0, 0, bbW, bbH };
    dev->SetScissorRect(&scissor);

    dev->SetPixelShader(nullptr);
    dev->SetVertexShader(nullptr);
    dev->SetFVF(kQuadFVF);
    dev->SetStreamSource(0, nullptr, 0, 0);
    dev->SetIndices(nullptr);
    dev->SetTexture(0, s_tex);

    dev->SetRenderState(D3DRS_LIGHTING,         FALSE);
    dev->SetRenderState(D3DRS_CULLMODE,         D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ZENABLE,          D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE,     FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND,         D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND,        D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE,  FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE,        FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE,    FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE,
        D3DCOLORWRITEENABLE_RED|D3DCOLORWRITEENABLE_GREEN|
        D3DCOLORWRITEENABLE_BLUE|D3DCOLORWRITEENABLE_ALPHA);
    dev->SetRenderState(D3DRS_CLIPPING,         TRUE);
    dev->SetRenderState(D3DRS_CLIPPLANEENABLE,  0);

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

    HRESULT hr = dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vtx, sizeof(QuadVertex));
    static int logged = 0;
    if (logged < 3) { sublog("overlay DrawPrimitiveUP hr=0x%08lx", hr); ++logged; }

    sb->Apply();
    sb->Release();
}

void OverlayDraw(IDirect3DDevice9* dev, IDirect3DSwapChain9* sc, const wchar_t* text) {
    if (!dev || !text || !*text) return;

    int bbW = 0, bbH = 0;
    IDirect3DSurface9* bb = AcquireBackBuffer(dev, sc, &bbW, &bbH);
    if (!bb || bbW <= 0 || bbH <= 0) {
        static bool warned = false;
        if (!warned) { sublog("AcquireBackBuffer failed sc=%p dev=%p", (void*)sc, (void*)dev); warned = true; }
        if (bb) bb->Release();
        return;
    }

    const bool sizeChanged = (bbW != s_bbW || bbH != s_bbH);
    if (sizeChanged) {
        sublog("overlay: backbuffer %dx%d (was %dx%d)", bbW, bbH, s_bbW, s_bbH);
        s_bbW = bbW; s_bbH = bbH; s_cachedText.clear();
    }

    if (s_cachedText != text) {
        int w = 0, h = 0;
        uint32_t* px = RenderTextToArgb(text, bbW, bbH, &w, &h);
        if (!px) { sublog("RenderTextToArgb failed"); bb->Release(); return; }
        bool ok = UploadTexture(dev, px, w, h);
        delete[] px;
        if (!ok) { bb->Release(); return; }
        sublog("overlay text uploaded: tex=%dx%d text=\"%ls\"", w, h, text);
        s_cachedText = text;
    }

    if (!s_tex) { bb->Release(); return; }
    DrawQuad(dev, bb, bbW, bbH);
    bb->Release();
}

void OverlayLoseDevice() {
    if (s_tex) { s_tex->Release(); s_tex = nullptr; }
    s_texW = s_texH = 0;
    s_cachedText.clear();
}

// ════════════════════════════════════════════════════════════════════════
//  Movie open/close detection  (was file_hook.cpp, minus the IAT patch)
// ════════════════════════════════════════════════════════════════════════

std::atomic<bool>     s_movieActive{false};
std::atomic<uint64_t> s_movieStartTick{0};
std::atomic<uint64_t> s_firstPresentTick{0};

std::mutex s_handleMu;
HANDLE     s_movieHandles[8] = { nullptr };

// 95s safety window in case a close is missed. Intro is 92s.
constexpr uint64_t kMovieMaxMs = 95000;

bool EndsWithMovieA(const char* path) {
    if (!path) return false;
    size_t n = strlen(path);
    static const char kNeedle[] = "ZBOF4.DAT";
    const size_t nn = sizeof(kNeedle) - 1;
    if (n < nn) return false;
    for (size_t i = 0; i < nn; ++i) {
        char a = (char)toupper((unsigned char)path[n - nn + i]);
        if (a != kNeedle[i]) return false;
    }
    return true;
}

void AddMovieHandle(HANDLE h) {
    std::lock_guard<std::mutex> lk(s_handleMu);
    for (HANDLE& slot : s_movieHandles) {
        if (slot == nullptr) { slot = h; break; }
    }
    if (!s_movieActive.load()) {
        s_movieStartTick.store(GetTickCount64());
        s_firstPresentTick.store(0);
        s_movieActive.store(true);
    }
}

bool RemoveMovieHandle(HANDLE h) {
    std::lock_guard<std::mutex> lk(s_handleMu);
    bool found = false, anyLeft = false;
    for (HANDLE& slot : s_movieHandles) {
        if (slot == h) { slot = nullptr; found = true; }
        else if (slot != nullptr) anyLeft = true;
    }
    return found && !anyLeft;
}

void MarkMovieEnded(const char* why) {
    if (!s_movieActive.exchange(false)) return;
    uint64_t elapsed = GetTickCount64() - s_movieStartTick.load();
    sublog("movie_end after %llums (%s)", (unsigned long long)elapsed, why);
}

bool MovieActive() {
    if (!s_movieActive.load(std::memory_order_relaxed)) return false;
    uint64_t elapsed = GetTickCount64() - s_movieStartTick.load();
    if (elapsed > kMovieMaxMs) { MarkMovieEnded("timeout"); return false; }
    return true;
}

uint32_t MovieElapsedMs() {
    if (!s_movieActive.load()) return 0;
    uint64_t anchor = s_firstPresentTick.load();
    if (anchor == 0) anchor = s_movieStartTick.load();
    uint64_t now = GetTickCount64();
    uint64_t elapsed = (now > anchor) ? (now - anchor) : 0;
    return (uint32_t)(elapsed > UINT32_MAX ? UINT32_MAX : elapsed);
}

void NotifyFirstPresent() {
    if (!s_movieActive.load(std::memory_order_relaxed)) return;
    uint64_t expected = 0;
    uint64_t now = GetTickCount64();
    if (s_firstPresentTick.compare_exchange_strong(expected, now)) {
        uint64_t decodeLatency = now - s_movieStartTick.load();
        sublog("first Present after movie_start at +%llums (anchor re-based to Present)",
               (unsigned long long)decodeLatency);
    }
}

}  // namespace

// ════════════════════════════════════════════════════════════════════════
//  Public API
// ════════════════════════════════════════════════════════════════════════

void subtitles_init() {
    g_sub.enabled = g_cfg.subtitle_enabled;
    if (!g_sub.enabled) {
        hook_log("subtitle: disabled (subtitle_enabled=false)\n");
        return;
    }

    g_sub.fontName       = WidenUtf8OrAnsi(g_cfg.subtitle_font);
    if (g_sub.fontName.empty()) g_sub.fontName = L"Comic Sans MS";
    g_sub.fontScale      = g_cfg.subtitle_font_scale;
    g_sub.bottomMargin   = g_cfg.subtitle_bottom_margin;
    g_sub.timeOffsetMs   = g_cfg.subtitle_time_offset_ms;
    g_sub.fontColor      = g_cfg.subtitle_font_color;
    g_sub.outlineColor   = g_cfg.subtitle_outline_color;
    g_sub.outlinePx      = g_cfg.subtitle_outline_px;
    g_sub.shadowEnabled  = g_cfg.subtitle_shadow_enabled;
    g_sub.shadowOffsetX  = g_cfg.subtitle_shadow_offset_x;
    g_sub.shadowOffsetY  = g_cfg.subtitle_shadow_offset_y;
    g_sub.shadowSoftness = g_cfg.subtitle_shadow_softness;
    g_sub.shadowColor    = g_cfg.subtitle_shadow_color;

    // Clamp to sane ranges so a typo can't blow past the budget.
    if (g_sub.fontScale < 0.1f) g_sub.fontScale = 0.1f;
    if (g_sub.fontScale > 8.0f) g_sub.fontScale = 8.0f;
    if (g_sub.bottomMargin < 0.0f) g_sub.bottomMargin = 0.0f;
    if (g_sub.bottomMargin > 0.9f) g_sub.bottomMargin = 0.9f;
    if (g_sub.outlinePx < 0)  g_sub.outlinePx = 0;
    if (g_sub.outlinePx > 32) g_sub.outlinePx = 32;
    if (g_sub.shadowOffsetX < -64) g_sub.shadowOffsetX = -64;
    if (g_sub.shadowOffsetX >  64) g_sub.shadowOffsetX =  64;
    if (g_sub.shadowOffsetY < -64) g_sub.shadowOffsetY = -64;
    if (g_sub.shadowOffsetY >  64) g_sub.shadowOffsetY =  64;
    if (g_sub.shadowSoftness < 0)  g_sub.shadowSoftness = 0;
    if (g_sub.shadowSoftness > 32) g_sub.shadowSoftness = 32;

    std::wstring srt = WidenUtf8OrAnsi(
        (g_cfg.subtitle_srt[0]) ? g_cfg.subtitle_srt : "MOV\\ZBOF4.srt");
    g_sub.srtPath = ResolvePath(srt);

    hook_log("subtitle: enabled — srt=\"%ls\" font=\"%ls\" scale=%.2f "
             "margin=%.2f offset=%dms\n",
             g_sub.srtPath.c_str(), g_sub.fontName.c_str(),
             g_sub.fontScale, g_sub.bottomMargin, g_sub.timeOffsetMs);
    hook_log("subtitle:   font_color=0x%08X outline=%dpx/0x%08X "
             "shadow=%d off=(%d,%d) soft=%d/0x%08X\n",
             g_sub.fontColor, g_sub.outlinePx, g_sub.outlineColor,
             (int)g_sub.shadowEnabled, g_sub.shadowOffsetX, g_sub.shadowOffsetY,
             g_sub.shadowSoftness, g_sub.shadowColor);

    LoadSrt(g_sub.srtPath.c_str());
}

bool subtitles_active() {
    return g_sub.enabled && !s_cues.empty();
}

void subtitles_on_present(IDirect3DDevice9* dev, IDirect3DSwapChain9* sc) {
    if (!g_sub.enabled || !dev) return;
    if (!MovieActive()) return;
    NotifyFirstPresent();
    int32_t t = (int32_t)MovieElapsedMs() + g_sub.timeOffsetMs;
    if (t < 0) t = 0;
    const Cue* cue = ActiveCue((uint32_t)t);
    if (cue) OverlayDraw(dev, sc, cue->text.c_str());
}

void subtitles_on_lost_device() {
    OverlayLoseDevice();
}

void subtitles_note_file_open(void* handle, const char* nameA) {
    if (!g_sub.enabled) return;
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr) return;
    if (!EndsWithMovieA(nameA)) return;
    AddMovieHandle((HANDLE)handle);
    sublog("movie_start handle=%p path=\"%s\"", handle, nameA ? nameA : "?");
}

void subtitles_note_file_close(void* handle) {
    if (!g_sub.enabled) return;
    if (!s_movieActive.load(std::memory_order_relaxed)) return;
    if (RemoveMovieHandle((HANDLE)handle)) MarkMovieEnded("last handle closed");
}
