// label_center.cpp — permanent fix: center world-map location-name labels.
//
// The world map draws location names (on-map markers via caller 0x0050D514 and
// the bottom-right callout via 0x0050CEF4) by centering the string in a box:
//   start-X = boxCenter - measure(str)/2,  then the VWF renderer 0x00629860
//   draws the glyphs left-to-right.
// After the German VWF work, measure()'s width no longer matches what the
// renderer actually draws (VWF-narrowed glyphs; the space 0x20 has advance 0 in
// the width table yet still renders with width), so labels sat left of centre —
// worse the more spaces a name had.
//
// Rather than predict the width from the (wrong) tables, we MEASURE what the
// renderer actually draws: while a label renders, a hook on the per-glyph
// enqueue 0x00410790 records the pen X of every glyph, giving the true visible
// extent. That real width is cached per string and used to re-center:
//   start-X = boxCenter - realWidth/2      (boxCenter = the caller's own center
//                                            = passed-X + measure/2)
// First sighting of a string uses the table estimate and records the true width;
// the label re-centers on its next redraw (labels redraw every frame).
//
// Always on (no config). Signature-independent: no-ops safely if the two hook
// targets aren't present. Applies only to the world-map label sites (return
// address 0x0050Cxxx-0x0050Dxxx) drawing a capitalised string.

#include "hooks.h"
#include "label_center.h"
#include "minhook/MinHook.h"

#include <cstdint>
#include <windows.h>
#include <intrin.h>   // _ReturnAddress

static constexpr uintptr_t ADDR_RENDERER = 0x00629860;   // VWF string renderer
static constexpr uintptr_t ADDR_GLYPHENQ = 0x00410790;   // per-glyph command enqueue
static void* g_orig_rend = nullptr;
static void* g_orig_enq  = nullptr;

static unsigned strhash(const char* s){ unsigned h=2166136261u; for(int i=0;i<40&&s[i];i++){h^=(unsigned char)s[i];h*=16777619u;} return h; }

// Table-based width estimate (per-glyph truncated advance × scale). Used only for
// the very first draw of a string, before its real width has been measured.
static int true_width(const char* s, float scale){
    const int8_t* adv=(const int8_t*)0x00952DE1; float x=0;
    for(int i=0;i<40&&s[i];i++){ unsigned char c=(unsigned char)s[i]; if(c>=0x20) x=(float)(int)(x+adv[(c-0x20)*2]*scale); }
    return (int)x;
}
// Advance (× scale) of the last drawable glyph — added to the measured
// left-to-left span so we center the full visible extent.
static int last_glyph_w(const char* s, float scale){
    const int8_t* adv=(const int8_t*)0x00952DE1; int last=0;
    for(int i=0;i<40&&s[i];i++){ unsigned char c=(unsigned char)s[i]; if(c>=0x20) last=adv[(c-0x20)*2]; }
    int w=(int)(last*scale); return w<0?0:w;
}

// measured real-width cache (string hash -> width)
struct WEnt { unsigned h; int w; };
static WEnt g_wcache[128]; static int g_wcache_n = 0;
static int  wcache_get(unsigned h){ for(int i=0;i<g_wcache_n;i++) if(g_wcache[i].h==h) return g_wcache[i].w; return -1; }
static void wcache_put(unsigned h,int w){ for(int i=0;i<g_wcache_n;i++) if(g_wcache[i].h==h){g_wcache[i].w=w;return;}
    if(g_wcache_n<128){g_wcache[g_wcache_n].h=h; g_wcache[g_wcache_n].w=w; g_wcache_n++;} }

// pen tracking: the renderer calls 0x410790 per glyph with edi = current pen X
static volatile bool g_measuring = false;
static volatile int  g_pen_first = 0, g_pen_last = 0; static volatile bool g_pen_got = false;

extern "C" void __cdecl label_track_pen(int pen){
    if(!g_measuring) return;
    if(!g_pen_got){ g_pen_first = pen; g_pen_got = true; }
    g_pen_last = pen;
}
__declspec(naked) static void hook_glyphenq(){
    __asm{
        pushad                 // [esp] = edi (the renderer's running pen X)
        mov eax, [esp]
        push eax
        call label_track_pen
        add esp, 4
        popad
        jmp [g_orig_enq]
    }
}

// C wrapper for the VWF string renderer (__cdecl; caller cleans 0x20).
//  a0=start-X  a1=Y  a2=color  a3=flags  a4=string ptr
typedef int (__cdecl *PFN_REND)(int,int,int,int,int,int,int,int);
extern "C" int __cdecl label_hook_renderer(int a0,int a1,int a2,int a3,int a4,int a5,int a6,int a7){
    unsigned ret = (unsigned)_ReturnAddress();
    const char* s = (const char*)a4;
    bool isLabel = (ret >= 0x0050C000 && ret < 0x0050E000) && s &&
                   (unsigned char)s[0] >= 'A' && (unsigned char)s[0] <= 'Z';
    if (!isLabel)
        return ((PFN_REND)g_orig_rend)(a0,a1,a2,a3,a4,a5,a6,a7);

    int a0_use = a0, realW = -1;
    __try {
        float scale = *(float*)0x008BE51C;
        typedef int (__cdecl *PFN_M)(const char*, int);
        int meas   = ((PFN_M)0x00629E90)(s, 0);           // width the caller centered with
        int center = a0 + meas / 2;                       // caller's intended (box) center
        realW = wcache_get(strhash(s));
        int useW = (realW >= 0) ? realW : true_width(s, scale);
        a0_use = center - useW / 2;                       // center the real width
    } __except (EXCEPTION_EXECUTE_HANDLER) { a0_use = a0; }

    g_pen_got = false; g_measuring = true;
    int rr = ((PFN_REND)g_orig_rend)(a0_use,a1,a2,a3,a4,a5,a6,a7);
    g_measuring = false;

    if (realW < 0 && g_pen_got) {                          // record the true width
        __try {
            float scale = *(float*)0x008BE51C;
            int real = (g_pen_last - g_pen_first) + last_glyph_w(s, scale);
            if (real > 0 && real < 2000) wcache_put(strhash(s), real);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return rr;
}

void label_center_install() {
    bool ok1 = MH_CreateHook((void*)ADDR_RENDERER, (void*)label_hook_renderer, (void**)&g_orig_rend) == MH_OK
            && MH_EnableHook((void*)ADDR_RENDERER) == MH_OK;
    bool ok2 = MH_CreateHook((void*)ADDR_GLYPHENQ, (void*)hook_glyphenq, (void**)&g_orig_enq) == MH_OK
            && MH_EnableHook((void*)ADDR_GLYPHENQ) == MH_OK;
    hook_log("label_center: world-map label centering %s (renderer 0x%08X %s, enqueue 0x%08X %s)\n",
             (ok1 && ok2) ? "installed" : "PARTIAL/FAILED",
             (unsigned)ADDR_RENDERER, ok1?"ok":"fail",
             (unsigned)ADDR_GLYPHENQ, ok2?"ok":"fail");
}
