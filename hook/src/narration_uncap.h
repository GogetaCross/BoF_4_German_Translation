// narration_uncap — lift the hardcoded typewriter character caps on the
// AREAD152 ending-narration screens ("I realized then… / …no longer with us.",
// "But at the same time / …that it was still <hero>.", etc.).
//
// The four bespoke scene-VM opcode handlers (0x48DF00, 0x48DFD0, 0x48E190,
// 0x46F300) each drive a per-line typewriter off a byte counter [state+0xA]
// that is hard-capped (cmp cl,0x74 / 0x4f / 0x38 / 0x51) and advance the scene
// PC ([state+0xC0]) once the counter hits that cap. The caps were tuned to the
// exact length of the *English* lines, so longer German (or any other) text is
// permanently truncated mid-word (e.g. "…immer noch <hero> w|ar").
//
// Fix = advance-on-complete: we detour each handler and replace the fixed cap
// with a target computed at runtime from the LAST line's real visible length
// (glyphs + newline + expanded name, using the player's actual name length).
// The scene now advances the instant the last line finishes revealing — no
// truncation for any translation length, and the original snappy pacing is
// preserved (no post-typing hold). Signature-verified per handler; a
// non-matching EXE build simply leaves that handler untouched (safe no-op).
#pragma once

void narration_uncap_install();
