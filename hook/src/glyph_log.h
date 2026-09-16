#pragma once
// DIAGNOSTIC: identify which subsystem draws a run of text. Does NOT hook
// anything itself — the shipped dialog_width thunk (thunk_width_fn) on the
// universal per-glyph metric routine 0x006780E0 calls glyph_log_record() once
// per glyph. While glyph_metric_log is on, it auto-snapshots the on-screen
// glyphs' (caller return-address, x, y, char) to glyph_metric.log every ~2.5 s.

void glyph_log_install();

// Called from thunk_width_fn: ra = caller return address, args = &arg0
// (args[0]=x, args[1]=y, args[3]=char). No-op unless glyph_metric_log is set.
extern "C" void __cdecl glyph_log_record(unsigned ra, unsigned* args);
