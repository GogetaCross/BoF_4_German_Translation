// See hook_health.h for the why.

#include "hooks.h"
#include "hook_health.h"

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

struct HealthEntry {
    std::string feature;
    std::string note;
    enum class Kind { Ok, Failed, Disabled } kind;
};

std::mutex            g_health_mtx;
std::vector<HealthEntry> g_health_entries;

struct TextRange {
    uintptr_t base = 0;
    uintptr_t end  = 0;
    bool resolved  = false;
};

TextRange g_text;

// Locate BOF4.exe's .text section once. .text is what we scan because
// every patch site we care about is executable code; scanning the full
// SizeOfImage would also pick up matches inside .rdata/.data which is
// almost never what we want.
void ensure_text_range() {
    if (g_text.resolved) return;
    g_text.resolved = true;

    HMODULE h = GetModuleHandleA(nullptr);
    if (!h) return;
    uintptr_t base = (uintptr_t)h;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    const IMAGE_NT_HEADERS* nt =
        (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (memcmp(sec->Name, ".text", 5) == 0) {
            g_text.base = base + sec->VirtualAddress;
            g_text.end  = g_text.base + sec->Misc.VirtualSize;
            return;
        }
    }
}

// Format up to 16 bytes as "AA BB CC ..." into buf. Returns chars
// written (excluding NUL).
int format_hex(char* buf, size_t buf_sz, const uint8_t* p, size_t len) {
    int n = 0;
    for (size_t i = 0; i < len; ++i) {
        int w = snprintf(buf + n, buf_sz - n,
                         (i + 1 == len) ? "%02X" : "%02X ", p[i]);
        if (w <= 0 || (size_t)(n + w) >= buf_sz) break;
        n += w;
    }
    return n;
}

bool safe_read(uintptr_t va, void* dst, size_t len) {
    // Use ReadProcessMemory so we don't crash on no-access pages. This
    // never trips guard-page handlers because RPM goes through the
    // kernel.
    SIZE_T got = 0;
    BOOL ok = ReadProcessMemory(GetCurrentProcess(), (LPCVOID)va,
                                dst, len, &got);
    return ok && got == len;
}

// Masked variant of the .text scan. `mask[i]=='x'` means pattern[i]
// must match; any other char (conventionally '?') is a wildcard. This
// is what lets a signature survive an EXE recompile: we wildcard the
// operand bytes that move (rel32 call/jmp targets, absolute global
// addresses, patched bytes) and keep only the stable opcode skeleton.
int scan_masked(const uint8_t* pattern, const char* mask, size_t len,
                uintptr_t* out_hits, int max_hits) {
    ensure_text_range();
    if (!g_text.base || !g_text.end || len == 0) return -1;
    if (g_text.end <= g_text.base) return -1;

    int hits = 0;
    const uintptr_t end = g_text.end - len;
    for (uintptr_t p = g_text.base; p <= end; ++p) {
        const uint8_t* q = (const uint8_t*)p;
        bool ok = true;
        for (size_t i = 0; i < len; ++i) {
            if (mask[i] == 'x' && q[i] != pattern[i]) { ok = false; break; }
        }
        if (ok) {
            if (out_hits && hits < max_hits) out_hits[hits] = p;
            ++hits;
        }
    }
    return hits;
}

}  // namespace

void health_record(const char* feature, bool ok, const char* note) {
    std::lock_guard<std::mutex> lk(g_health_mtx);
    g_health_entries.push_back({
        feature ? feature : "?",
        note ? note : "",
        ok ? HealthEntry::Kind::Ok : HealthEntry::Kind::Failed,
    });
}

void health_record_disabled(const char* feature, const char* note) {
    std::lock_guard<std::mutex> lk(g_health_mtx);
    g_health_entries.push_back({
        feature ? feature : "?",
        note ? note : "",
        HealthEntry::Kind::Disabled,
    });
}

bool health_probe_bytes(uintptr_t va, const uint8_t* expected, size_t len) {
    uint8_t buf[32];
    if (len == 0 || len > sizeof(buf)) return false;
    if (!safe_read(va, buf, len)) return false;   // RPM: false, never faults
    return memcmp(buf, expected, len) == 0;
}

void health_log_bytes(const char* prefix, uintptr_t va, size_t len) {
    if (len > 64) len = 64;
    uint8_t tmp[64];
    if (!safe_read(va, tmp, len)) {
        hook_log("%s @ 0x%08X: <unreadable>\n", prefix, (unsigned)va);
        return;
    }
    char hex[3 * 64 + 1];
    format_hex(hex, sizeof(hex), tmp, len);
    hook_log("%s @ 0x%08X: %s\n", prefix, (unsigned)va, hex);
}

int health_scan_text(const uint8_t* pattern, size_t len,
                     uintptr_t* out_hits, int max_hits) {
    ensure_text_range();
    if (!g_text.base || !g_text.end || len == 0) return -1;
    if (g_text.end <= g_text.base) return -1;

    int hits = 0;
    const uintptr_t end = g_text.end - len;
    // Plain byte scan. .text on BOF4.exe is ~3 MB — a single linear
    // pass is ~milliseconds, fine for init-time.
    for (uintptr_t p = g_text.base; p <= end; ++p) {
        if (memcmp((const void*)p, pattern, len) == 0) {
            if (out_hits && hits < max_hits) out_hits[hits] = p;
            ++hits;
        }
    }
    return hits;
}

bool health_check_bytes(const char* feature,
                        uintptr_t expected_va,
                        const uint8_t* expected_bytes,
                        size_t len) {
    uint8_t actual[16];
    if (len > sizeof(actual)) len = sizeof(actual);

    if (!safe_read(expected_va, actual, len)) {
        hook_log("%s: <unreadable @ 0x%08X> — patch SKIPPED\n",
                 feature, (unsigned)expected_va);
        return false;
    }
    if (memcmp(actual, expected_bytes, len) == 0) {
        return true;
    }

    // Mismatch. Dump what we found and (best effort) what's around it.
    char got_hex[3 * 16 + 1];
    char exp_hex[3 * 16 + 1];
    format_hex(got_hex, sizeof(got_hex), actual, len);
    format_hex(exp_hex, sizeof(exp_hex), expected_bytes, len);
    hook_log(
        "%s: byte mismatch @ 0x%08X (got %s, expected %s)\n",
        feature, (unsigned)expected_va, got_hex, exp_hex);

    // 16-byte hexdump from VA-4 to VA+12 — helps sight-compare against
    // the old EXE.
    uintptr_t ctx_va = expected_va >= 4 ? expected_va - 4 : expected_va;
    uint8_t ctx[16];
    if (safe_read(ctx_va, ctx, sizeof(ctx))) {
        char ctx_hex[3 * 16 + 1];
        format_hex(ctx_hex, sizeof(ctx_hex), ctx, sizeof(ctx));
        hook_log("%s:   context 0x%08X..0x%08X = %s\n",
                 feature, (unsigned)ctx_va,
                 (unsigned)(ctx_va + sizeof(ctx)), ctx_hex);
    }

    // Scan .text for the original pattern. If 1 match -> suggest.
    // If 0 or >1 -> report ambiguity so the user knows the auto-find
    // didn't pin a single new address.
    enum { MAX = 8 };
    uintptr_t hits[MAX] = {0};
    int total = health_scan_text(expected_bytes, len, hits, MAX);
    if (total < 0) {
        hook_log("%s:   .text scan unavailable (couldn't resolve "
                 "section)\n", feature);
    } else if (total == 0) {
        hook_log("%s:   .text scan found 0 matches for the original "
                 "pattern — site was likely rewritten, not just moved\n",
                 feature);
    } else if (total == 1) {
        intptr_t delta = (intptr_t)hits[0] - (intptr_t)expected_va;
        hook_log(
            "%s:   .text scan: 1 match @ 0x%08X (delta %+d bytes) — "
            "site likely moved here\n",
            feature, (unsigned)hits[0], (int)delta);
    } else {
        // Multiple matches — list up to MAX so user can disambiguate.
        char buf[8 * 12 + 1];
        int n = 0;
        int show = total < MAX ? total : MAX;
        for (int i = 0; i < show; ++i) {
            int w = snprintf(buf + n, sizeof(buf) - n,
                             (i + 1 == show) ? "0x%08X" : "0x%08X ",
                             (unsigned)hits[i]);
            if (w <= 0 || (size_t)(n + w) >= sizeof(buf)) break;
            n += w;
        }
        hook_log(
            "%s:   .text scan: %d matches for the original pattern "
            "(%s%s) — ambiguous, inspect manually\n",
            feature, total, buf, (total > MAX) ? " ..." : "");
    }
    return false;
}

uintptr_t resolve_sig(const char* name,
                      const uint8_t* pattern, const char* mask,
                      size_t len, ptrdiff_t patch_off) {
    enum { MAX = 8 };
    uintptr_t hits[MAX] = {0};
    int total = scan_masked(pattern, mask, len, hits, MAX);

    if (total < 0) {
        hook_log("%s: sig scan unavailable (.text unresolved)\n", name);
        return 0;
    }
    if (total == 0) {
        hook_log("%s: sig NOT FOUND (0 matches) — site rewritten or "
                 "signature wrong\n", name);
        return 0;
    }
    if (total > 1) {
        char buf[8 * 12 + 1];
        int n = 0;
        int show = total < MAX ? total : MAX;
        for (int i = 0; i < show; ++i) {
            int w = snprintf(buf + n, sizeof(buf) - n,
                             (i + 1 == show) ? "0x%08X" : "0x%08X ",
                             (unsigned)hits[i]);
            if (w <= 0 || (size_t)(n + w) >= sizeof(buf)) break;
            n += w;
        }
        hook_log("%s: sig AMBIGUOUS (%d matches: %s%s) — refusing, "
                 "widen the pattern\n",
                 name, total, buf, (total > MAX) ? " ..." : "");
        return 0;
    }

    uintptr_t va = (uintptr_t)((intptr_t)hits[0] + patch_off);
    hook_log("%s: sig FOUND @ 0x%08X (site 0x%08X)\n",
             name, (unsigned)hits[0], (unsigned)va);
    return va;
}

void health_report() {
    std::lock_guard<std::mutex> lk(g_health_mtx);
    if (g_health_entries.empty()) return;

    int n_ok = 0, n_fail = 0, n_disabled = 0;
    for (const auto& e : g_health_entries) {
        switch (e.kind) {
            case HealthEntry::Kind::Ok:       ++n_ok;       break;
            case HealthEntry::Kind::Failed:   ++n_fail;     break;
            case HealthEntry::Kind::Disabled: ++n_disabled; break;
        }
    }

    hook_log("===== HOOK HEALTH (ok=%d fail=%d disabled=%d) =====\n",
             n_ok, n_fail, n_disabled);
    // Failed first — they're what the user needs to act on.
    for (const auto& e : g_health_entries) {
        if (e.kind == HealthEntry::Kind::Failed) {
            hook_log("  [FAIL] %-22s %s\n",
                     e.feature.c_str(), e.note.c_str());
        }
    }
    for (const auto& e : g_health_entries) {
        if (e.kind == HealthEntry::Kind::Ok) {
            hook_log("  [ ok ] %-22s %s\n",
                     e.feature.c_str(), e.note.c_str());
        }
    }
    for (const auto& e : g_health_entries) {
        if (e.kind == HealthEntry::Kind::Disabled) {
            hook_log("  [ -- ] %-22s %s\n",
                     e.feature.c_str(), e.note.c_str());
        }
    }
    hook_log("===== end HOOK HEALTH =====\n");
}
