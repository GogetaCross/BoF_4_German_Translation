// See hang_watchdog.h for why this exists and the safety rules it keeps.
#include "hang_watchdog.h"
#include "hooks.h"

#include <windows.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <string>

// How long Present must stay frozen before we call it a hang. Long enough that
// a slow load, an alt-tab or a stalled FMV can't trip it.
static const int  IDLE_SECONDS  = 12;
static const int  MAX_REPORTS   = 3;
static const int  STACK_WORDS   = 256;   // dwords of each thread's stack to scan
static const int  MAX_FRAMES    = 12;    // code addresses reported per thread

struct ThreadSample {
    DWORD    tid;
    DWORD    eip, esp, ebp;
    bool     got_context;
    uint32_t stack[STACK_WORDS];
    int      stack_n;
};

// Resolve an address to "module+0xOFFSET". Done AFTER every thread is resumed —
// GetModuleHandleEx takes the loader lock, which a suspended thread may hold.
static bool describe_addr(uint32_t addr, char* out, size_t cap) {
    if (addr < 0x10000) return false;
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(uintptr_t)addr, &m) || !m)
        return false;
    char path[MAX_PATH] = "?";
    GetModuleFileNameA(m, path, MAX_PATH);
    const char* base = strrchr(path, '\\');
    base = base ? base + 1 : path;
    snprintf(out, cap, "%s+0x%X", base, (unsigned)(addr - (uintptr_t)m));
    return true;
}

// Is this address inside SOME loaded module's executable image? Used to filter
// a raw stack scan down to plausible return addresses.
static bool looks_like_code(uint32_t addr) {
    if (addr < 0x10000) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery((LPCVOID)(uintptr_t)addr, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    DWORD p = mbi.Protect & 0xFF;
    bool exec = (p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ ||
                 p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY);
    return exec && mbi.Type == MEM_IMAGE;
}

// Capture every other thread's position. NOTHING is logged in here.
static void sample_threads(std::vector<ThreadSample>& out) {
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    THREADENTRY32 te{}; te.dwSize = sizeof(te);
    if (!Thread32First(snap, &te)) { CloseHandle(snap); return; }
    do {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT,
                              FALSE, te.th32ThreadID);
        if (!h) continue;

        ThreadSample s{};
        s.tid = te.th32ThreadID;
        if (SuspendThread(h) != (DWORD)-1) {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (GetThreadContext(h, &ctx)) {
                s.got_context = true;
                s.eip = ctx.Eip; s.esp = ctx.Esp; s.ebp = ctx.Ebp;
                SIZE_T got = 0;
                // ReadProcessMemory, not a direct deref: a bad/uncommitted ESP
                // must fail cleanly, never raise inside a suspended-thread window.
                if (ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)ctx.Esp,
                                      s.stack, sizeof(s.stack), &got))
                    s.stack_n = (int)(got / sizeof(uint32_t));
            }
            ResumeThread(h);          // resume BEFORE anything else, always
        }
        CloseHandle(h);
        out.push_back(s);
    } while (Thread32Next(snap, &te));
    CloseHandle(snap);
}

static void report(int n, const char* why) {
    std::vector<ThreadSample> samples;
    sample_threads(samples);          // all threads resumed by the time this returns

    hook_log("\n===== HANG WATCHDOG report #%d (%s) =====\n"
             "presents=%llu  submits=%llu\n"
             "Every thread's position at the moment of sampling follows. The top\n"
             "line of each is where it IS; the rest is a raw stack scan filtered to\n"
             "addresses inside loaded images, so it can include stale frames.\n",
             n, why,
             (unsigned long long)g_present_call_count.load(std::memory_order_relaxed),
             (unsigned long long)g_submit_call_count.load(std::memory_order_relaxed));

    for (const ThreadSample& s : samples) {
        if (!s.got_context) { hook_log("  tid %5lu  <no context>\n", s.tid); continue; }
        char at[MAX_PATH + 32] = "?";
        describe_addr(s.eip, at, sizeof(at));
        hook_log("  tid %5lu  eip=0x%08X  %s\n", s.tid, s.eip, at);
        int shown = 0;
        for (int i = 0; i < s.stack_n && shown < MAX_FRAMES; ++i) {
            if (!looks_like_code(s.stack[i])) continue;
            char d[MAX_PATH + 32] = "?";
            if (!describe_addr(s.stack[i], d, sizeof(d))) continue;
            hook_log("               0x%08X  %s\n", s.stack[i], d);
            ++shown;
        }
    }
    hook_log("===== end HANG WATCHDOG report #%d =====\n\n", n);
}

// The automatic trigger has a BLIND SPOT, learned the hard way: a game that is
// stuck but still spinning its render loop keeps calling Present, so a
// "Present stopped" test never fires — and the log looks identical either way,
// because the [rect]/[dat-open] lines only print on FIRST sight of a command or
// file. So we also emit a HEARTBEAT (which distinguishes frozen from
// still-drawing at a glance) and accept a manual dump key for the case where
// only a human can tell the game is wedged.
static DWORD WINAPI watchdog_thread(LPVOID) {
    uint64_t last_p = 0;
    int  still = 0, reports = 0, beats = 0;
    bool started = false, key_prev = false;

    while (true) {
        Sleep(500);
        const uint64_t p = g_present_call_count.load(std::memory_order_relaxed);
        const uint64_t s = g_submit_call_count.load(std::memory_order_relaxed);

        // Manual dump: the friend presses this while it is visibly hung.
        bool key = (GetAsyncKeyState(VK_PAUSE) & 0x8000) != 0;
        if (key && !key_prev && reports < MAX_REPORTS)
            report(++reports, "manual — Pause key");
        key_prev = key;

        if ((beats % 20) == 0) {          // every ~10 s
            hook_log("hang_watchdog: heartbeat presents=%llu submits=%llu%s\n",
                     (unsigned long long)p, (unsigned long long)s,
                     p == last_p ? "   <-- Present is NOT advancing" : "");
        }
        ++beats;

        if (p > 0) started = true;
        if (started && reports < MAX_REPORTS) {
            if (p != last_p) { still = 0; }
            else if (++still >= IDLE_SECONDS * 2) {   // 500ms ticks
                report(++reports, "Present stalled");
                still = 0;
            }
        }
        last_p = p;
    }
    return 0;
}

void hang_watchdog_install() {
    if (!g_cfg.hang_watchdog) return;
    CreateThread(nullptr, 0, watchdog_thread, nullptr, 0, nullptr);
    hook_log("hang_watchdog: armed — heartbeat every 10 s (presents/submits). "
             "Auto-dump if Present stalls %d s; press PAUSE/BREAK any time to dump "
             "every thread's position on demand.\n", IDLE_SECONDS);
}
