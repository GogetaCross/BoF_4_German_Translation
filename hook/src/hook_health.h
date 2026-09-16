// Diagnostic helpers for byte-pattern patches inside BOF4.exe.
//
// Why this exists: several runtime patches (save_anywhere,
// item_name_uncap, space_fallthrough, ...) bake in hard-coded VAs and
// expected byte values. When the EXE changes (e.g. a GOG update), the
// site shifts and the patch silently skips. The user is left guessing
// which feature broke and where the new address might be.
//
// This module adds two things on top of the per-feature log lines:
//
//   1. health_diagnose_mismatch() — on a byte-pattern mismatch, dumps
//      16 bytes around the expected VA and scans the EXE's .text
//      section for the original byte pattern. If exactly one match is
//      found, the log suggests "site likely moved to 0x%08X". If 0 or
//      >1 matches, it reports that so the user knows the auto-discovery
//      was inconclusive.
//
//   2. health_record() / health_report() — collects a single end-of-init
//      summary block listing every patch site as OK / FAIL / DISABLED.
//      This makes failed patches visible at a glance instead of being
//      buried among ~350 lines of init output.
//
// No automatic patching — discovery only. The user updates addresses
// manually after sighting the log.
#pragma once

#include <cstdint>
#include <cstddef>

void health_record(const char* feature, bool ok, const char* note);
void health_record_disabled(const char* feature, const char* note);

// Check `len` bytes at `expected_va` against `expected_bytes`. Returns
// true on match. On mismatch, logs a 16-byte hexdump around the VA and
// scans BOF4.exe's .text section for the original pattern (capped at 8
// reported matches). Caller is expected to ALSO call health_record()
// with their preferred status string.
bool health_check_bytes(const char* feature,
                        uintptr_t expected_va,
                        const uint8_t* expected_bytes,
                        size_t len);

// Standalone scanner — search BOF4.exe's .text section for `pattern`.
// Writes up to `max_hits` matching VAs into `out_hits` and returns the
// total match count (which may exceed max_hits). Returns -1 if the
// .text section couldn't be resolved.
int health_scan_text(const uint8_t* pattern, size_t len,
                     uintptr_t* out_hits, int max_hits);

// Signature-scan BOF4.exe's .text for a UNIQUE masked match of
// `pattern` (length `len`). `mask` is a same-length string of 'x'
// (byte must match) and '?' (wildcard). This is the future-proofing
// primitive: wildcard the operand bytes that shift across EXE builds
// (rel32 targets, absolute globals, bytes we patch) and keep only the
// stable opcode skeleton, so the site is re-found after any recompile.
//
// On exactly one match, returns (match VA + patch_off) and logs the
// resolved address. On 0 or >1 matches, logs the reason and returns 0
// — the caller should then skip installing that feature. `name` labels
// the log lines.
uintptr_t resolve_sig(const char* name,
                      const uint8_t* pattern, const char* mask,
                      size_t len, ptrdiff_t patch_off);

// Dump (up to) `len` bytes at `va` to the log as "AA BB CC ...".
// Safe on protected memory (read-only access; no VirtualProtect).
void health_log_bytes(const char* prefix, uintptr_t va, size_t len);

// Quiet, crash-safe probe: returns true iff the `len` bytes at `va` equal
// `expected`. Uses ReadProcessMemory so it never faults on encrypted/no-access
// pages (e.g. Steam/Enigma before .text is unpacked) — reads there simply
// return false. Logs nothing, so it is safe to call in a tight poll loop.
// Used by the Step-5 deferred-install readiness gate.
bool health_probe_bytes(uintptr_t va, const uint8_t* expected, size_t len);

// Final summary written from hook_bof4_install() after every feature
// has reported in. Idempotent — safe to call more than once.
void health_report();
