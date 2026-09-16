// censor_fix_aream031 — runtime restore of the NA-cut AREAM031 violence beat.
//
// GOG-only, flag-gated (g_cfg.censor_fix_aream031, default OFF), exe file
// UNTOUCHED (runtime .data-VA remap + a data codecave; reversible by dropping
// the flag / redeploying the prior DLL).
//
// Mechanism = Strategy B1' (validated by EN + JP runtime traces + strict review;
// see scripts_for_removing_censoring/PLAN_uncensor_phaseC.md):
//   The scene actor's script parks at VA 0x9241A5 (opcode `18 12` = wait until
//   selector [0xB56658]==0x12) in the censored build, idling through the
//   sel==0x0C beat. The uncensored JP build inserts a 24-byte sel==0x0C
//   choreography before that wait. We host JP's exact 26-byte sequence
//   (24-byte insert + the shared `18 12` wait) in a data codecave and steer the
//   VM's per-context PC ([esi+0xC0]) through it at the fetch chokepoints:
//       PC == 0x9241A5                    -> CAVE_START     (divert into the beat)
//       CAVE_END <= PC < CAVE_START+64    -> 0x9241A7 + (PC-CAVE_END)  (resume)
//   Stateless per-context (acts only on the current ctx's own [esi+0xC0]); the
//   resume target 0x9241A7 != the divert 0x9241A5, so there is no re-entry loop,
//   and the cave lives far from the record so the resume test cannot false-hit
//   an un-diverted context. Save-safe: the script PC is reconstructed from the
//   record on scene load (proven: same save -> build-specific PCs), never
//   serialized.
//
// Verifies the full censored bytes at 0x9241A0, the three fetch chokepoints and
// the dispatch site before touching anything; aborts (installs nothing) on any
// mismatch or on codecave allocation failure.
#pragma once

// Allocate + populate the codecave, verify all sites, install the PC-remap
// hooks. No-op unless g_cfg.censor_fix_aream031 is true. Safe to call once at
// hook init (after the cave is built).
void censor_fix_aream031_install();

// Remove the hooks (restore original bytes) and release the codecave.
void censor_fix_aream031_shutdown();

// READ-ONLY scene discovery: install the same 3 universal VM fetch chokepoints
// with NO diverts and NO codecave, logging per-context PC changes across the
// whole baked-script .data region so a NEW censored scene's record + branch
// point can be located from one playthrough. No-op unless g_cfg.censor_scene_trace
// is true; skipped if censor_fix_aream031 is also on (they share the 3 sites).
// F6 dumps censor_scene_trace.txt. Teardown is handled by
// censor_fix_aream031_shutdown() (shared hook/thread state).
void censor_scene_trace_install();

// AREAD145 "bath scene" DIAGNOSTIC PROBE (confirm the phase-7 gate to ~99%
// before building the real fix). Two independent flag-gated parts:
//   g_cfg.censor_probe_aread145        = READ-ONLY phase/selector logger
//                                        (US or JP build; logs to d3d9_hook.log).
//   g_cfg.censor_probe_aread145_force  = US-only: force scene phase = 7 at the
//                                        gate (repoints the two deleted-branch
//                                        jumps to a minimal no-call codecave).
// THROWAWAY SAVE ONLY when forcing (no story-flag guard yet). No-op unless the
// respective flag is set. Independent of the AREAM031/027 VM hooks.
void censor_probe_aread145_install();
void censor_probe_aread145_shutdown();
