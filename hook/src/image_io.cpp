// BoF4 d3d9 proxy — PNG dump + texture injection.
//
// Capture: hook writes tex_dump/<id>_<w>x<h>_<fmt>_<crc>.png.
// Inject:  if tex_inject/<crc>.png exists, its pixels are loaded, converted
//          to the target D3D format, and written back over the locked
//          memory before the game's UnlockRect completes.
//
// Both sides go through RGBA8 as the interchange format: we always convert
// D3D surface bytes to RGBA8 for PNG output, and always convert PNG RGBA8
// back to the target D3D format on inject.

#include "hooks.h"
#include <d3d9.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_FAILURE_STRINGS
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_JPEG
#define STBI_NO_BMP
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_GIF
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO  // we supply our own file writer
#include "stb_image_write.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <windows.h>   // FindFirstFile for the CRC preload scan

// ── D3DFORMAT helpers ──────────────────────────────────────────────────────
static const char* fmt_name(D3DFORMAT f) {
    switch (f) {
    case D3DFMT_A8R8G8B8:   return "A8R8G8B8";
    case D3DFMT_X8R8G8B8:   return "X8R8G8B8";
    case D3DFMT_R5G6B5:     return "R5G6B5";
    case D3DFMT_X1R5G5B5:   return "X1R5G5B5";
    case D3DFMT_A1R5G5B5:   return "A1R5G5B5";
    case D3DFMT_A4R4G4B4:   return "A4R4G4B4";
    case D3DFMT_X4R4G4B4:   return "X4R4G4B4";
    case D3DFMT_A8:         return "A8";
    case D3DFMT_L8:         return "L8";
    case D3DFMT_A8L8:       return "A8L8";
    case D3DFMT_P8:         return "P8";
    case D3DFMT_A8P8:       return "A8P8";
    case D3DFMT_R8G8B8:     return "R8G8B8";
    case D3DFMT_DXT1:       return "DXT1";
    case D3DFMT_DXT3:       return "DXT3";
    case D3DFMT_DXT5:       return "DXT5";
    default:                return "UNK";
    }
}

static bool is_compressed(D3DFORMAT f) {
    return f == D3DFMT_DXT1 || f == D3DFMT_DXT2 || f == D3DFMT_DXT3
        || f == D3DFMT_DXT4 || f == D3DFMT_DXT5;
}

static int bytes_per_pixel(D3DFORMAT f) {
    switch (f) {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:   return 4;
    case D3DFMT_R8G8B8:     return 3;
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4:
    case D3DFMT_X4R4G4B4:
    case D3DFMT_A8L8:
    case D3DFMT_A8P8:       return 2;
    case D3DFMT_A8:
    case D3DFMT_L8:
    case D3DFMT_P8:         return 1;
    default:                return 0;
    }
}

// ── Decode one source pixel to RGBA8 (R,G,B,A order in memory) ─────────────
static void decode_to_rgba(const uint8_t* row, UINT x, D3DFORMAT fmt,
                            uint8_t out[4])
{
    uint8_t r = 0, g = 0, b = 0, a = 255;
    switch (fmt) {
    case D3DFMT_A8R8G8B8: {
        uint32_t v = *((const uint32_t*)row + x);
        a = (uint8_t)((v >> 24) & 0xFF);
        r = (uint8_t)((v >> 16) & 0xFF);
        g = (uint8_t)((v >>  8) & 0xFF);
        b = (uint8_t)( v        & 0xFF);
        break;
    }
    case D3DFMT_X8R8G8B8: {
        uint32_t v = *((const uint32_t*)row + x);
        r = (uint8_t)((v >> 16) & 0xFF);
        g = (uint8_t)((v >>  8) & 0xFF);
        b = (uint8_t)( v        & 0xFF);
        a = 255;
        break;
    }
    case D3DFMT_R8G8B8: {
        const uint8_t* p = row + x * 3;
        b = p[0]; g = p[1]; r = p[2]; a = 255;
        break;
    }
    case D3DFMT_R5G6B5: {
        uint16_t v = *((const uint16_t*)row + x);
        r = (uint8_t)(((v >> 11) & 0x1F) * 255 / 31);
        g = (uint8_t)(((v >>  5) & 0x3F) * 255 / 63);
        b = (uint8_t)(( v        & 0x1F) * 255 / 31);
        a = 255;
        break;
    }
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5: {
        uint16_t v = *((const uint16_t*)row + x);
        a = (fmt == D3DFMT_A1R5G5B5) ? (uint8_t)((v >> 15) ? 255 : 0) : 255;
        r = (uint8_t)(((v >> 10) & 0x1F) * 255 / 31);
        g = (uint8_t)(((v >>  5) & 0x1F) * 255 / 31);
        b = (uint8_t)(( v        & 0x1F) * 255 / 31);
        break;
    }
    case D3DFMT_A4R4G4B4:
    case D3DFMT_X4R4G4B4: {
        uint16_t v = *((const uint16_t*)row + x);
        a = (fmt == D3DFMT_A4R4G4B4)
                ? (uint8_t)(((v >> 12) & 0xF) * 17) : 255;
        r = (uint8_t)(((v >>  8) & 0xF) * 17);
        g = (uint8_t)(((v >>  4) & 0xF) * 17);
        b = (uint8_t)(( v        & 0xF) * 17);
        break;
    }
    case D3DFMT_A8: {
        uint8_t v = row[x];
        // A8 has no RGB; show alpha as both alpha and luminance so the
        // PNG is visible when displayed flattened.
        r = g = b = v; a = v;
        break;
    }
    case D3DFMT_L8: {
        uint8_t v = row[x];
        r = g = b = v; a = 255;
        break;
    }
    case D3DFMT_A8L8: {
        uint16_t v = *((const uint16_t*)row + x);
        uint8_t l = (uint8_t)(v & 0xFF);
        r = g = b = l;
        a = (uint8_t)((v >> 8) & 0xFF);
        break;
    }
    case D3DFMT_P8: {
        uint8_t v = row[x];
        r = g = b = v; a = 255;
        break;
    }
    default: {
        r = g = b = 0; a = 255;
        break;
    }
    }
    out[0] = r; out[1] = g; out[2] = b; out[3] = a;
}

// ── Encode one RGBA8 pixel back to the destination D3D format ──────────────
// Writes to row + (format-specific offset). Returns false if the format
// isn't writable through this path.
static bool encode_from_rgba(uint8_t* row, UINT x, D3DFORMAT fmt,
                              const uint8_t in[4])
{
    uint8_t r = in[0], g = in[1], b = in[2], a = in[3];
    switch (fmt) {
    case D3DFMT_A8R8G8B8: {
        uint32_t v = ((uint32_t)a << 24) | ((uint32_t)r << 16)
                   | ((uint32_t)g <<  8) |  (uint32_t)b;
        *((uint32_t*)row + x) = v;
        return true;
    }
    case D3DFMT_X8R8G8B8: {
        uint32_t v = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        *((uint32_t*)row + x) = v;
        return true;
    }
    case D3DFMT_R8G8B8: {
        uint8_t* p = row + x * 3;
        p[0] = b; p[1] = g; p[2] = r;
        return true;
    }
    case D3DFMT_R5G6B5: {
        uint16_t v = (uint16_t)(((r >> 3) << 11)
                              | ((g >> 2) <<  5)
                              |  (b >> 3));
        *((uint16_t*)row + x) = v;
        return true;
    }
    case D3DFMT_X1R5G5B5: {
        uint16_t v = (uint16_t)(((r >> 3) << 10)
                              | ((g >> 3) <<  5)
                              |  (b >> 3));
        *((uint16_t*)row + x) = v;
        return true;
    }
    case D3DFMT_A1R5G5B5: {
        uint16_t v = (uint16_t)(((a >= 0x80 ? 1 : 0) << 15)
                              | ((r >> 3) << 10)
                              | ((g >> 3) <<  5)
                              |  (b >> 3));
        *((uint16_t*)row + x) = v;
        return true;
    }
    case D3DFMT_A4R4G4B4: {
        uint16_t v = (uint16_t)(((a >> 4) << 12)
                              | ((r >> 4) <<  8)
                              | ((g >> 4) <<  4)
                              |  (b >> 4));
        *((uint16_t*)row + x) = v;
        return true;
    }
    case D3DFMT_X4R4G4B4: {
        uint16_t v = (uint16_t)(((r >> 4) << 8)
                              | ((g >> 4) << 4)
                              |  (b >> 4));
        *((uint16_t*)row + x) = v;
        return true;
    }
    case D3DFMT_A8:
        row[x] = a;
        return true;
    case D3DFMT_L8:
        // Luminance from RGB
        row[x] = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
        return true;
    case D3DFMT_A8L8: {
        uint16_t l = (uint16_t)((r * 77 + g * 150 + b * 29) >> 8);
        uint16_t v = (uint16_t)((a << 8) | l);
        *((uint16_t*)row + x) = v;
        return true;
    }
    default:
        return false;
    }
}

// ── CRC32 (standard polynomial 0xEDB88320) ─────────────────────────────────
uint32_t crc32_buf(const void* data, size_t len) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        init = true;
    }
    const uint8_t* p = (const uint8_t*)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

// ── Normalize transparent pixels ───────────────────────────────────────────
//
// BoF4 allocates 32x32 textures and only draws the glyph into part of it —
// the rest is uninitialized memory that changes every boot. This breaks
// CRC matching. Fix: zero out every pixel where alpha == 0 before both
// hashing and PNG output, so only the visible glyph contributes to the
// hash. (Alpha-zero pixels are invisible anyway, so this is lossless for
// rendering purposes.)
static void normalize_transparent(std::vector<uint8_t>& rgba) {
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
        if (rgba[i + 3] == 0) {
            rgba[i + 0] = 0;
            rgba[i + 1] = 0;
            rgba[i + 2] = 0;
        }
    }
}

// ── Shape-only hash ────────────────────────────────────────────────────────
//
// Even after normalizing transparent pixels, BoF4 still produces different
// RGB values for the same glyph across runs — possibly due to dynamic text
// coloring, sub-pixel rendering, or stale uninitialized bytes that happen
// to have alpha > 0. To get a truly stable identifier, hash ONLY the alpha
// channel (binary threshold: >0 or =0). Glyph identity is determined by
// shape, and two distinct font glyphs practically never share the same
// binary alpha mask.
static uint32_t shape_hash(const std::vector<uint8_t>& rgba) {
    // Pack alpha-threshold bits, then CRC the packed buffer.
    size_t n_px = rgba.size() / 4;
    std::vector<uint8_t> bits((n_px + 7) / 8, 0);
    for (size_t i = 0; i < n_px; ++i) {
        if (rgba[i * 4 + 3] > 0) {
            bits[i >> 3] |= (uint8_t)(1u << (i & 7));
        }
    }
    return crc32_buf(bits.data(), bits.size());
}

// ── Build an RGBA8 buffer from the locked surface ──────────────────────────
static std::vector<uint8_t> lock_to_rgba8(UINT width, UINT height,
                                           D3DFORMAT fmt, INT pitch,
                                           const void* pixels)
{
    std::vector<uint8_t> rgba((size_t)width * height * 4);
    const uint8_t* src = (const uint8_t*)pixels;
    for (UINT y = 0; y < height; ++y) {
        const uint8_t* row = src + (ptrdiff_t)y * pitch;
        uint8_t* dst = rgba.data() + (size_t)y * width * 4;
        for (UINT x = 0; x < width; ++x) {
            decode_to_rgba(row, x, fmt, dst + x * 4);
        }
    }
    return rgba;
}

// Writer callback for stb_image_write when STBI_WRITE_NO_STDIO is defined.
static void stbi_write_callback(void* context, void* data, int size) {
    FILE* f = (FILE*)context;
    if (f && size > 0) fwrite(data, 1, (size_t)size, f);
}

// ── Public: save an arbitrary RGBA8 buffer to a PNG file ──────────────────
// Bypasses the dump-dedupe cache entirely. Used by glyph_render.cpp and
// diagnostic code that needs a direct "write exactly this buffer" path.
bool save_rgba8_png(const char* path, int width, int height,
                    const void* rgba)
{
    if (!path || !rgba || width <= 0 || height <= 0) return false;
    FILE* f = nullptr;
    fopen_s(&f, path, "wb");
    if (!f) return false;
    int ok = stbi_write_png_to_func(&stbi_write_callback, f,
                                     width, height, 4,
                                     rgba, width * 4);
    fclose(f);
    return ok != 0;
}

// ── Public: CRC32 of a locked surface's SHAPE (alpha channel only) ─────────
// Used for re-upload matching: if the same texture is re-locked later, we
// need the same CRC the first dump produced so injection still fires.
// We hash on the binary alpha mask, ignoring RGB entirely — this is
// robust to dynamic text coloring, sub-pixel AA jitter, and any other
// RGB variation the game might introduce across runs.
uint32_t crc32_rgba_of_locked(UINT width, UINT height, unsigned int format_u,
                               INT pitch, const void* pixels)
{
    D3DFORMAT format = (D3DFORMAT)format_u;
    if (!pixels || width == 0 || height == 0) return 0;
    if (is_compressed(format)) return 0;
    std::vector<uint8_t> rgba = lock_to_rgba8(width, height, format,
                                               pitch, pixels);
    return shape_hash(rgba);
}

// ── CRC dedupe state ───────────────────────────────────────────────────────
// Every CRC we've already written to tex_dump/ (either this session or in a
// previous run whose files are still on disk). Lets us skip redundant PNG
// writes for the same glyph — the game re-creates a fresh D3D texture for
// every character upload, so without dedupe the folder fills up with
// hundreds of identical copies.
static std::mutex                     g_dumped_crcs_mtx;
static std::unordered_set<uint32_t>   g_dumped_crcs;
static bool                            g_dumped_crcs_loaded = false;

// Scan tex_dump/ once for existing PNG filenames of the form
// "NNNN_WxH_FMT_XXXXXXXX.png" and preload their 8-hex-digit CRC suffix into
// g_dumped_crcs. The matching pattern is "*_????????.png" where the 8 ?s
// are any single character — we validate them as hex in the loop.
static void preload_dumped_crcs_locked() {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA("tex_dump\\*_????????.png", &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    int loaded = 0;
    do {
        size_t len = strlen(fd.cFileName);
        if (len < 13) continue;             // 8 hex + "_.png" = 13 min
        const char* suffix = fd.cFileName + len - 13;
        if (suffix[0] != '_') continue;
        char hex[9] = {0};
        memcpy(hex, suffix + 1, 8);
        bool ok = true;
        for (int i = 0; i < 8; ++i) {
            char c = hex[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F'))) { ok = false; break; }
        }
        if (!ok) continue;
        uint32_t crc = (uint32_t)strtoul(hex, nullptr, 16);
        g_dumped_crcs.insert(crc);
        ++loaded;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    hook_log("  dedupe: preloaded %d existing CRCs from tex_dump\\\n", loaded);
}

// ── Public: dump to PNG ────────────────────────────────────────────────────
uint32_t dump_texture_png(void* tex_ptr, UINT width, UINT height,
                           unsigned int format_u, INT pitch,
                           const void* pixels)
{
    D3DFORMAT format = (D3DFORMAT)format_u;
    if (!pixels || width == 0 || height == 0) return 0;
    if (width > 4096 || height > 4096) return 0;  // sanity guard
    if (is_compressed(format)) {
        hook_log("  skip compressed tex %ux%u %s\n",
                 width, height, fmt_name(format));
        return 0;
    }

    // Convert to RGBA8. Normalize transparent pixels for cleaner PNG
    // output. Hash is computed on alpha-shape only so it's stable
    // across game boots even if RGB values drift.
    std::vector<uint8_t> rgba = lock_to_rgba8(width, height, format,
                                               pitch, pixels);
    normalize_transparent(rgba);
    uint32_t crc = shape_hash(rgba);

    // ── CRC dedupe ────────────────────────────────────────────────────
    // Return early if we (or a previous session) already wrote a PNG
    // with this CRC. The caller still gets the CRC back, so injection
    // and small-tex tracking continue to work — they just don't see a
    // new file appear in tex_dump/.
    {
        std::lock_guard<std::mutex> lk(g_dumped_crcs_mtx);
        if (!g_dumped_crcs_loaded) {
            preload_dumped_crcs_locked();
            g_dumped_crcs_loaded = true;
        }
        if (!g_dumped_crcs.insert(crc).second) {
            return crc;  // already on disk — skip the write
        }
    }

    static std::atomic<int> g_counter{0};
    int id = ++g_counter;

    char fname[MAX_PATH];
    _snprintf_s(fname, sizeof(fname), _TRUNCATE,
                "tex_dump\\%04d_%ux%u_%s_%08X.png",
                id, width, height, fmt_name(format), crc);

    FILE* f = nullptr;
    fopen_s(&f, fname, "wb");
    if (!f) {
        hook_log("  FAIL write %s\n", fname);
        return crc;
    }
    int ok = stbi_write_png_to_func(&stbi_write_callback, f,
                                     (int)width, (int)height, 4,
                                     rgba.data(), (int)(width * 4));
    fclose(f);
    // Count opaque pixels for debugging — same glyph should have the
    // same count across boots regardless of RGB values.
    int n_opaque = 0;
    for (size_t i = 0; i < rgba.size(); i += 4) {
        if (rgba[i + 3] > 0) ++n_opaque;
    }

    if (!ok) {
        hook_log("  FAIL encode %s\n", fname);
    } else {
        hook_log("  dumped #%04d %ux%u %s crc=%08X opaque=%d -> %s\n",
                 id, width, height, fmt_name(format), crc, n_opaque, fname);
    }
    return crc;
}

// Parse the 8-hex CRC out of a tex_inject filename. Accepts BOTH the bare
// form "XXXXXXXX.png" AND the dump-style form "NNNN_WxH_FMT_XXXXXXXX.png",
// so a file dumped to tex_dump/ can be edited and dropped straight into
// tex_inject/ with its original name — no rename needed. Returns true and
// sets *out on a match.
static bool inject_crc_from_name(const char* name, uint32_t* out) {
    size_t len = strlen(name);
    if (len < 12) return false;                       // 8 hex + ".png"
    if (_stricmp(name + len - 4, ".png") != 0) return false;
    const char* hex = name + len - 12;                // 8 hex before ".png"
    if (hex != name && hex[-1] != '_') return false;  // bare or "_XXXXXXXX"
    for (int i = 0; i < 8; ++i)
        if (!isxdigit((unsigned char)hex[i])) return false;
    char buf[9] = {0};
    memcpy(buf, hex, 8);
    *out = (uint32_t)strtoul(buf, nullptr, 16);
    return true;
}

// One-time index of tex_inject/*.png keyed by CRC. Built on first inject
// probe. Bare-CRC names win over dump-style names if both exist for a CRC.
static std::mutex                              g_inject_mtx;
static std::unordered_map<uint32_t, std::string> g_inject_map;
static bool                                     g_inject_loaded = false;

static void preload_inject_map_locked() {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA("tex_inject\\*.png", &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        uint32_t crc = 0;
        if (!inject_crc_from_name(fd.cFileName, &crc)) continue;
        char full[MAX_PATH];
        _snprintf_s(full, sizeof(full), _TRUNCATE,
                    "tex_inject\\%s", fd.cFileName);
        // Prefer a bare "XXXXXXXX.png" over a dump-style name for the same CRC.
        bool bare = (strlen(fd.cFileName) == 12);
        auto it = g_inject_map.find(crc);
        if (it == g_inject_map.end() || bare) g_inject_map[crc] = full;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    hook_log("  tex_inject: indexed %d file(s)\n", (int)g_inject_map.size());
}

bool try_inject_texture(uint32_t crc, UINT width, UINT height,
                         unsigned int format_u, INT pitch, void* pixels)
{
    D3DFORMAT format = (D3DFORMAT)format_u;
    if (!pixels || width == 0 || height == 0) return false;
    if (is_compressed(format)) return false;  // no DXT injection yet


    // Generic disk-PNG injection is opt-in via _d3d9_hook_config.txt.
    // When tex_inject is OFF, skip the lookup entirely so the hook is
    // zero-cost on an unused feature.
    if (!g_cfg.tex_inject_enabled) return false;

    // Look up a PNG for this CRC (accepts bare-CRC or dump-style filenames).
    char fname[MAX_PATH];
    {
        std::lock_guard<std::mutex> lk(g_inject_mtx);
        if (!g_inject_loaded) {
            preload_inject_map_locked();
            g_inject_loaded = true;
        }
        auto it = g_inject_map.find(crc);
        if (it == g_inject_map.end()) return false;
        _snprintf_s(fname, sizeof(fname), _TRUNCATE, "%s", it->second.c_str());
    }

    int w = 0, h = 0, comp = 0;
    uint8_t* png = stbi_load(fname, &w, &h, &comp, 4);
    if (!png) {
        hook_log("  INJECT FAIL load %s: %s\n", fname, stbi_failure_reason());
        return false;
    }
    if ((UINT)w != width || (UINT)h != height) {
        hook_log("  INJECT SKIP %s: size %dx%d vs texture %ux%u\n",
                 fname, w, h, width, height);
        stbi_image_free(png);
        return false;
    }

    uint8_t* dst = (uint8_t*)pixels;
    bool any_failed = false;
    for (UINT y = 0; y < height; ++y) {
        uint8_t* row = dst + (ptrdiff_t)y * pitch;
        const uint8_t* src = png + (size_t)y * w * 4;
        for (UINT x = 0; x < width; ++x) {
            if (!encode_from_rgba(row, x, format, src + x * 4)) {
                any_failed = true;
            }
        }
    }
    stbi_image_free(png);

    if (any_failed) {
        hook_log("  INJECT PARTIAL %s (unsupported format %s)\n",
                 fname, fmt_name(format));
        return true;
    }
    hook_log("  INJECTED %s -> %ux%u %s\n",
             fname, width, height, fmt_name(format));
    return true;
}
