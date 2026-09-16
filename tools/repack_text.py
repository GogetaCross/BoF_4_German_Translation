#!/usr/bin/env python3
"""
BoF4 Text Repacker - repack_text.py

Reads translated .txt files and injects the strings back into the DAT files.
Backs up originals to DAT_backup/ before overwriting.

Usage:
    py repack_text.py translations/AREAE030.txt
    py repack_text.py translations/*.txt --sync-shared

The source .DAT is loaded from DAT/<name>.DAT. The original is backed up to
DAT_backup/<name>.DAT (only once — existing backups are preserved).
The repacked result overwrites DAT/<name>.DAT directly.
"""

import sys
import os
import ast
import glob
import struct
import argparse
import re
import shutil
import json

import uncensor_patch   # baked NA-uncensor dialogue restore (pre-pass)


# ---------------------------------------------------------------------------
# String codec
# ---------------------------------------------------------------------------

_CTRL_TOKEN = re.compile(r'\[([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2})*)\]')

# BoF4 kana byte mapping: Unicode → game byte value
# Hiragana 0x5E-0xAD, Katakana 0xAE-0xFE (from bof4text.txt section 6)
_KANA_TO_BYTE = {}
_HIRAGANA = 'あいうえおかきくけこさしすせそたちつてとなにぬねのはひふへほまみむめもやゆよらりるれろわをんぁぃぅぇぉっゃゅょがぎぐげござじずぜぞだぢづでどばびぶべぼぱぴぷぺぽ'
_KATAKANA = 'アイウエオカキクケコサシスセソタチツテトナニヌネノハヒフヘホマミムメモヤユヨラリルレロワヲンァィゥェォッャュョガギグゲゴザジズゼゾダヂヅデドバビブベボパピプペポヴ'
for _i, _ch in enumerate(_HIRAGANA):
    _KANA_TO_BYTE[_ch] = 0x5E + _i
for _i, _ch in enumerate(_KATAKANA):
    _KANA_TO_BYTE[_ch] = 0xAE + _i

# German umlauts → game byte mapping.
#
# Single source of truth: vwf_config.txt next to this script. For each
# line of the form `0xCC = NAME : ...`, if NAME is exactly one non-ASCII
# character, that character maps to byte CC in the game text. So
# `0x3C = ä : 6 : 12` automatically wires "ä in editor → byte 0x3C in
# DAT" without touching this script.
#
# This lets the artist freely move umlauts between atlas slots
# (piggyback ASCII slots vs the original 0x88-0x8E Japanese-leftover
# slots, etc.) just by editing vwf_config.txt — no repacker change.

def _anchor_dir():
    """Base dir for the game-root walk. When frozen into the editor .exe by
    PyInstaller, __file__ points inside the temp _MEIPASS extraction, so fall
    back to the current working dir — the editor runs repack with cwd=game_dir,
    which walks to the game root immediately."""
    if getattr(sys, "frozen", False):
        return os.getcwd()
    return os.path.dirname(os.path.abspath(__file__))


def _walk_to_game_root(start):
    """Walk up from `start` until a folder with DAT/ or translations/ (the game
    root). repack_text.py now lives a few levels below it (in the editor folder),
    while vwf_config.txt stays at the game root (the d3d9 hook reads it there)."""
    d = os.path.abspath(start)
    for _ in range(8):
        if os.path.isdir(os.path.join(d, "DAT")) or \
           os.path.isdir(os.path.join(d, "translations")):
            return d
        p = os.path.dirname(d)
        if p == d:
            break
        d = p
    return os.path.abspath(start)

_VWF_CONFIG_PATH = os.path.join(
    _walk_to_game_root(_anchor_dir()),
    "vwf_config.txt")

# `0xXX = NAME :` — capture byte value and the NAME column verbatim
# (NAME is whatever non-whitespace token sits between '=' and ':').
_VWF_LINE_RE = re.compile(r"0x([0-9A-Fa-f]+)\s*=\s*(\S+)\s*:")


def _load_umlaut_map_from_vwf_config(path=_VWF_CONFIG_PATH):
    """Derive {Unicode char -> byte} from vwf_config.txt.

    Treats only single non-ASCII NAME entries as mappings; multi-char
    names like 'space', 'circ1' and single-ASCII names like 'A' are
    documentation and ignored. Returns {} on any failure (caller falls
    back to a hardcoded default)."""
    out = {}
    if not os.path.isfile(path):
        return out
    try:
        with open(path, encoding="utf-8") as f:
            for line in f:
                line = line.split("#", 1)[0].strip()
                if not line:
                    continue
                m = _VWF_LINE_RE.match(line)
                if not m:
                    continue
                byte = int(m.group(1), 16)
                name = m.group(2)
                if len(name) == 1 and not (0x20 <= ord(name) <= 0x7E):
                    out[name] = byte
    except Exception:
        return {}
    return out


_UMLAUT_TO_BYTE = _load_umlaut_map_from_vwf_config()
if not _UMLAUT_TO_BYTE:
    # Fallback if vwf_config.txt is missing or unparseable: legacy
    # piggyback mapping used until 2026-04-19.
    _UMLAUT_TO_BYTE = {
        'Ä': 0x24, 'Ü': 0x28, 'Ö': 0x29,
        'ä': 0x3C, 'ü': 0x3E, 'ö': 0x5B, 'ß': 0x5D,
    }

# Special font-slot glyphs discovered in the PC port's dialog font.
# At these byte positions the game font carries leftover Japanese/symbol
# glyphs instead of the usual ASCII characters.
#
# Entries that collide with the umlaut piggyback slots above (0x24,
# 0x3C, 0x3E, 0x5B, 0x5D) have been removed — those atlas slots now
# hold our umlaut pixels, so mapping the corresponding Japanese char
# there would render as the wrong umlaut.
#
# If you need to emit any arbitrary byte directly, use the control-code
# escape syntax: '[23]' emits byte 0x23, '[7E]' emits 0x7E, etc.
_SPECIAL_TO_BYTE = {
    '。': 0x23,   # ideographic full stop
    'Ｘ': 0x2A,   # fullwidth X  (normal 'X' still encodes as 0x58)
    '·': 0x40,    # middle dot (U+00B7, overrides Latin-1 fallback 0xB7)
    '～': 0x5E,   # fullwidth tilde
    'Ƶ': 0x5F,    # Latin capital Z with stroke
    'Ⅳ': 0x60,   # Roman numeral four
    '＜': 0x7B,   # fullwidth less-than
    '°': 0x7C,    # degree sign (U+00B0, overrides Latin-1 fallback 0xB0)
    '『': 0x7D,   # left white corner bracket
    '』': 0x7E,   # right white corner bracket
}


# Space-byte substitution. ASCII space (0x20) takes a hardcoded
# branch in the EN renderer that ignores TABLE_A — every 0x20
# renders at ~17 px in menus regardless of vwf_config. Workaround:
# emit a high byte instead, which goes through the TABLE_A read path
# (proven by umlauts at 0x90..0x96 working there), so its width is
# fully controlled by vwf_config's `0xNN = nbsp : 0 : N` line.
#
# SINGLE SOURCE OF TRUTH = the localization editor's config
# (localization_editor/configs/editor_config.json, key "space_byte", set via
# the toolbar "Advanced space" box). The editor passes `--space-byte 0xNN` on
# every repack it launches; standalone CLI runs read the SAME config so the two
# never diverge. `_editor_config_space_byte()` below performs that read. The
# chosen byte MUST have (a) a transparent atlas slot in INIT.DAT and (b) a
# matching `0xNN = nbsp : 0 : N` width line in vwf_config.txt.
#
# The CLI `--space-byte` accepts hex/int, or `none`/`off`/`legacy`/`0x20` to
# emit plain 0x20; `_parse_space_byte` normalizes it. None disables substitution.


def _parse_space_byte(value):
    """Parse a --space-byte / editor setting into an int byte or None.

    Accepts 'none'/'off'/'legacy'/'0x20'/'20'(->None means plain space),
    or any hex ('0x97') / decimal ('151') in 0x21..0xFF.
    """
    if value is None:
        return SPACE_REPLACEMENT_BYTE
    s = str(value).strip().lower()
    if s in ('none', 'off', 'legacy', '', '0x20', '20', '32'):
        return None
    try:
        b = int(s, 16) if s.startswith('0x') else int(s, 0)
    except ValueError:
        raise ValueError(f"invalid --space-byte {value!r} (use 0x97, an int, or none)")
    if not (0x21 <= b <= 0xFF):
        raise ValueError(f"--space-byte 0x{b:02X} out of range; must be 0x21..0xFF "
                         "(and 0x20 means 'none')")
    return b


# Last-resort fallback byte, used ONLY when the editor config is absent/unreadable
# (e.g. a fresh checkout before the editor has ever run). Not a behavioral
# override — the editor config wins whenever it exists.
_SPACE_BYTE_FALLBACK = 0x97


def _editor_config_space_byte():
    """Return the space-substitute byte from the localization editor's config
    (the single source of truth). Int byte, or None if configured off. Falls
    back to `_SPACE_BYTE_FALLBACK` only when no config / key is present."""
    # configs/ lives next to the editor. When frozen, the repack helper runs as
    # the editor's own .exe (re-exec), so sys.executable's dir is that folder.
    _cfg_base = (os.path.dirname(sys.executable) if getattr(sys, "frozen", False)
                 else os.path.dirname(os.path.abspath(__file__)))
    cfg_path = os.path.join(_cfg_base, "configs", "editor_config.json")
    try:
        with open(cfg_path, encoding="utf-8") as f:
            val = json.load(f).get("space_byte")
        if val is not None:
            return _parse_space_byte(val)
    except Exception:
        pass
    return _SPACE_BYTE_FALLBACK


# Active space byte: sourced from the editor config at import time. A --space-byte
# CLI arg (which the editor always passes) overrides it per run in main().
SPACE_REPLACEMENT_BYTE = _editor_config_space_byte()

# Per-ENTRY space-byte override: any (DAT-basename, entry-index) pair in this set
# is forced to plain ASCII 0x20 for spaces, bypassing the substitute byte. This
# existed because the in-battle combat-Help menu (AB000_00 entry 0 / BTLEND entry
# 4) failed to render entirely with the OLD 0xFF substitute (0xFF is out of the
# normal glyph range and choked that menu's text path).
#
# 2026-07-24: EMPTIED / reverted. The substitute byte is now 0x97 — a normal
# slot right next to the umlauts (0x90..0x96), not a fringe byte like 0xFF — so
# the combat-Help menu has a good chance of handling it fine and getting proper
# space widths. Left empty to let those entries use the configured byte too.
# If the in-battle Help menu breaks again under 0x97 during playtest, re-add the
# pair(s) here — e.g. LEGACY_SPACE_ENTRIES = {("AB000_00", 0), ("BTLEND", 4)}.
# Dengeki Store (AREAD068 e7 / AREAS052 e8): this area renders spaces as ASCII
# 0x20, not the 0xFF/0x97 glyph — force 0x20 for its text entry regardless of the
# global --space-byte (see project_dengeki_store).
LEGACY_SPACE_ENTRIES = {("AREAD068", 7), ("AREAS052", 8)}

# ---------------------------------------------------------------------------
# COMPOSED-ROOT OVERRIDE (narrow allowlist, keyed by root offset)
# ---------------------------------------------------------------------------
# Multi-item roots are normally emitted as INDEPENDENT per-slot strings (each
# slot points at its own \0-terminated German), because the battle skill/item
# description reads a slot until \0 and would otherwise show the whole
# concatenated tail. BUT some menus (confirmed: the Master-learning screen)
# reach a skill's description by WALKING the `[16:NN]@` dividers from the root
# rather than following the slot pointer. For those the root MUST still be the
# composed multi-item block with its dividers intact, or the walk runs off the
# end of a divider-less German root and renders the wrong / blank text (e.g.
# "Benediction" showing up under "Raise Dead").
#
# Entries listed here (keyed by DAT basename + root offset) are rebuilt as the
# COMPOSED block (German segments glued with the pristine English `[16:NN]@`
# dividers; every slot re-anchored INTO it via the normal suffix resolver),
# instead of the independent-slot layout. Editing is unchanged — the per-slot
# German is composed automatically at repack time.
#
# SCOPED PER FILE (basename + offset): the shared magic/skill-description block
# lives at offset 0x2C66 in seven files. The menus that show it (Master-learning,
# Camp skills, worldmap/status, shops, battle) REACH a skill's description by
# WALKING the `[16:NN]@` dividers from the root — so each of those files needs
# the composed block, or deep segments render blank / under the wrong skill
# (e.g. "Benediction" appeared under "Raise Dead"). Composing simply restores
# the original English layout (root with dividers + slots pointing in) but with
# German text, which is exactly how the game reads it natively.
#
# All seven files that carry this block are listed below. Editing is unchanged —
# the per-slot German is composed automatically at repack time. If a NEW file or
# a different shared root turns up blank in a divider-walk menu, add its
# (basename, offset) pair here.
# NOTE: emptied 2026-07-26. The composed-root approach was aimed at the wrong
# block — the menus that showed blank descriptions read a DIFFERENT region
# (multi-item suffix slots), and the real bug was empty `\0` emission for slots
# pointing at a divider-after-divider (fixed in _slot_render_range /
# _leading_item_text). Kept as an available mechanism if a genuine divider-walk
# menu ever needs it. See [[project-multi-item-divider]].
COMPOSED_ROOT_OFFSETS = set()


def _is_composed_root(base_name, root_off) -> bool:
    """True if this (file, root) is on the composed-block allowlist."""
    return (base_name, root_off) in COMPOSED_ROOT_OFFSETS


# Typographic quote characters translators paste (Word/OS autocorrect, German
# „…“ / ‚…‘) that must normalise to the game's straight ASCII quotes at pack time.
# The font has only 0x22 (") and 0x27 ('); any other quote glyph would render
# wrong or (for non-Latin-1 ones like „ U+201E) abort the pack.
_SMART_DOUBLE = frozenset("“”„‟″＂")  # “ ” „ ‟ ″ ＂ -> "
_SMART_SINGLE = frozenset("‘’‚‛′")        # ‘ ’ ‚ ‛ ′ -> '


def encode_string(text: str) -> bytes:
    """
    Encode a decoded Python string back to raw BoF4 bytes (no null terminator).
    Supports:
      ' ' (ASCII space)         -> SPACE_REPLACEMENT_BYTE if set, else 0x20
      newline char (\\n)        -> 0x01
      '[XX]'                    -> byte 0xXX          (0-param code)
      '[XX:YY]'                 -> bytes 0xXX 0xYY    (1-param code)
      '[XX:YY:ZZ]'              -> bytes 0xXX 0xYY 0xZZ  (2-param code)
      Unicode hiragana/katakana -> game kana byte (0x5E-0xFE)
      German umlauts            -> piggyback ASCII slots (_UMLAUT_TO_BYTE)
      Special PC-port glyphs    -> discovered font slots (_SPECIAL_TO_BYTE)
      smart punctuation         -> ASCII equivalents ('...', "'", '"', '--', '-')
      other chars               -> ord(c) & 0xFF
    """
    result = bytearray()
    i = 0
    while i < len(text):
        c = text[i]
        if c == ' ' and SPACE_REPLACEMENT_BYTE is not None:
            # Route spaces through a high-byte char (set by fork) so
            # the renderer reads TABLE_A for advance instead of taking
            # the 0x20 hardcoded branch.
            result.append(SPACE_REPLACEMENT_BYTE)
            i += 1
        elif c == '\n':
            result.append(0x01)
            i += 1
        elif c == '[':
            m = _CTRL_TOKEN.match(text, i)
            if m:
                for part in m.group(1).split(':'):
                    result.append(int(part, 16))
                i = m.end()
            else:
                result.append(ord('['))
                i += 1
        elif c in _KANA_TO_BYTE:
            result.append(_KANA_TO_BYTE[c])
            i += 1
        elif c in _UMLAUT_TO_BYTE:
            result.append(_UMLAUT_TO_BYTE[c])
            i += 1
        elif c in _SPECIAL_TO_BYTE:
            result.append(_SPECIAL_TO_BYTE[c])
            i += 1
        elif c == '\u2026':  # ellipsis "..." -> three ASCII dots
            result.extend(b'...')
            i += 1
        elif c in _SMART_SINGLE:   # \u2018 \u2019 \u201A \u201B \u2032 (any smart single quote) -> ASCII '
            result.append(0x27)
            i += 1
        elif c in _SMART_DOUBLE:   # \u201C \u201D \u201E \u201F \u2033 \uFF02 (any smart double quote) -> ASCII "
            # The game font only has the straight ASCII quote 0x22. Translators
            # (esp. German \u201E\u2026\u201C) paste typographic quotes; normalise them all so
            # they render \u2014 and so the pack never errors on a non-Latin-1 quote.
            result.append(0x22)
            i += 1
        elif c == '\u2014':  # — (em dash) -> --
            result.extend(b'--')
            i += 1
        elif c == '\u2013':  # – (en dash) -> -
            result.append(0x2D)
            i += 1
        else:
            b = ord(c)
            if b > 255:
                raise ValueError(
                    f"Character U+{b:04X} ('{c}') cannot encode as single byte. "
                    f"Use ASCII/Latin-1 only."
                )
            result.append(b)
            i += 1
    return bytes(result)


# ---------------------------------------------------------------------------
# Dump parser
# ---------------------------------------------------------------------------

def parse_dump(path: str) -> dict:
    """
    Parse a text_dump / text_insert .txt file.

    Returns: {entry_idx: {str_offset_in_entry: python_string}}
    str_offset is the offset WITHIN the entry (as stored in the offset table).
    """
    result = {}
    current_idx = None
    current_strings = {}

    with open(path, encoding='utf-8') as f:
        for line in f:
            line = line.rstrip('\n')

            # "## Entry N @ 0xXXXX (size 0xYYYY)"
            m = re.match(
                r'## Entry (\d+) @ 0x([0-9A-Fa-f]+) \(size 0x([0-9A-Fa-f]+)\)',
                line
            )
            if m:
                if current_idx is not None and current_strings:
                    result[current_idx] = current_strings
                current_idx = int(m.group(1))
                current_strings = {}
                continue

            # "[0xXXXX] <repr>"
            m = re.match(r'\[0x([0-9A-Fa-f]+)\] (.+)$', line)
            if m and current_idx is not None:
                off = int(m.group(1), 16)
                repr_part = m.group(2).split('  # ')[0].rstrip()  # strip # hidden marker
                try:
                    text = ast.literal_eval(repr_part)
                except Exception:
                    text = repr_part   # fallback: raw text
                current_strings[off] = text

    if current_idx is not None and current_strings:
        result[current_idx] = current_strings

    return result


# ---------------------------------------------------------------------------
# Pool utilities
# ---------------------------------------------------------------------------

# 0-param control codes (no extra bytes consumed)
_CTRL_0P = frozenset([0x02, 0x03, 0x06, 0x08, 0x0B, 0x0D])
# 2-param control codes (consume 2 extra bytes)
_CTRL_2P = frozenset([0x09, 0x17, 0x18])
# 0x14 is variable-length — handled in _ctrl_advance below.


def _ctrl_advance(entry: bytes, pos: int) -> int:
    """Return how many bytes the control code at pos occupies (including the opcode byte)."""
    b = entry[pos]
    if b in _CTRL_0P:
        return 1
    if b == 0x0E and pos + 1 < len(entry) and entry[pos + 1] == 0x0F:
        return 3   # 0x0E 0x0F effect_byte
    if b in _CTRL_2P:
        return 3
    # 0x14 is variable-length: param >= 0x80 → 3 bytes, else 2 bytes
    if b == 0x14 and pos + 1 < len(entry) and entry[pos + 1] >= 0x80:
        return 3
    return 2   # default: opcode + 1 param byte


def scan_roots(entry: bytes, pool_start: int) -> list:
    """
    Scan the string pool sequentially to find all root string offsets
    (each starts immediately after the null terminator of the previous one).

    Returns a list of offsets relative to entry start, in pool order.
    """
    roots = []
    pos = pool_start
    while pos < len(entry):
        roots.append(pos)
        while pos < len(entry):
            b = entry[pos]
            if b == 0x00:
                pos += 1
                break
            elif b == 0x01:
                pos += 1
            elif b < 0x20:
                pos += _ctrl_advance(entry, pos)
            else:
                pos += 1
    return roots


def root_raw_bytes(entry: bytes, root_off: int) -> bytes:
    """Return raw bytes of one root string INCLUDING its null terminator."""
    pos = root_off
    while pos < len(entry):
        b = entry[pos]
        if b == 0x00:
            return entry[root_off : pos + 1]
        elif b == 0x01:
            pos += 1
        elif b < 0x20:
            pos += _ctrl_advance(entry, pos)
        else:
            pos += 1
    return entry[root_off:]   # no null found (truncated entry)


def _decode_for_compare(entry: bytes, off: int) -> str:
    """Decode a string at `off` in `entry` the same way extract_text.py does."""
    parts = []
    i = off
    while i < len(entry):
        b = entry[i]
        if b == 0x00:
            break
        elif b == 0x01:
            parts.append('\n')
            i += 1
        elif b in _CTRL_0P:
            parts.append(f'[{b:02X}]')
            i += 1
        elif b == 0x0E and i + 2 < len(entry) and entry[i + 1] == 0x0F:
            parts.append(f'[0E:0F:{entry[i+2]:02X}]')
            i += 3
        elif b in _CTRL_2P:
            if i + 2 < len(entry):
                parts.append(f'[{b:02X}:{entry[i+1]:02X}:{entry[i+2]:02X}]')
                i += 3
            else:
                parts.append(f'[{b:02X}]')
                i += 1
        elif b < 0x20:
            if i + 1 < len(entry):
                parts.append(f'[{b:02X}:{entry[i+1]:02X}]')
                i += 2
            else:
                parts.append(f'[{b:02X}]')
                i += 1
        else:
            parts.append(chr(b))
            i += 1
    return ''.join(parts)


def _ctrl_positions(buf: bytes, start: int, end: int) -> list:
    """Return list of (ctrl_byte, byte_pos) for every control code in buf[start:end].

    Tokens with multi-byte parameters are still listed once, at the opcode byte.
    Skips 0x01 (newline) and printable text. Used to anchor suffix offsets.
    """
    out = []
    pos = start
    while pos < end:
        b = buf[pos]
        if b == 0x00:
            break
        if b == 0x01:
            pos += 1
            continue
        if b < 0x20:
            out.append((b, pos))
            pos += _ctrl_advance(buf, pos)
            continue
        pos += 1
    return out


def _resolve_suffix_offset(old_parent: bytes, old_rel: int,
                            new_parent: bytes) -> tuple:
    """Map an old suffix offset (relative to old_parent start) onto new_parent.

    Strategy: count how many control-code tokens precede `old_rel` in old_parent
    and whether the suffix lands AT a control code or AFTER one. Find the
    same anchor in new_parent.

    Returns (new_rel, status) where status is 'ok', 'mismatch' (couldn't find
    matching anchor), or 'fallback' (legacy old_rel preserved).
    """
    old_ctrls = _ctrl_positions(old_parent, 0, len(old_parent))
    new_ctrls = _ctrl_positions(new_parent, 0, len(new_parent))

    # Anchor by SAME-BYTE-VALUE ordinal rather than global control index. A
    # boundary anchor (page-break `[16:NN]`, portrait `[09:..]`, etc.) is the
    # i-th control OF ITS OWN KIND; matching it to the i-th same-kind control
    # in the new parent survives intra-page edits that add/remove OTHER control
    # codes (e.g. a German page that drops a `[02]` paragraph break). The old
    # global-index match desynced every downstream page anchor whenever a
    # translated page changed its control-code count. See [[combat-help-split-bug]].
    def _same_kind_new_ctrl(b, k):
        ordinal = sum(1 for j in range(k + 1) if old_ctrls[j][0] == b)  # 1-based
        cnt = 0
        for nk, (nb, npos) in enumerate(new_ctrls):
            if nb == b:
                cnt += 1
                if cnt == ordinal:
                    return new_ctrls[nk]
        return None

    # Case A: suffix lands AT a control code (rel matches one of old_ctrls).
    for k, (b, pos) in enumerate(old_ctrls):
        if pos == old_rel:
            nc = _same_kind_new_ctrl(b, k)
            if nc is not None:
                return nc[1], 'ok'
            return old_rel, 'mismatch'

    # Case B: suffix lands AFTER a control code. Find the latest CTRL whose
    # end == old_rel (immediately before suffix).
    for k, (b, pos) in enumerate(old_ctrls):
        ctrl_len = _ctrl_advance(old_parent, pos)
        if pos + ctrl_len == old_rel:
            nc = _same_kind_new_ctrl(b, k)
            if nc is not None:
                new_pos = nc[1]
                new_len = _ctrl_advance(new_parent, new_pos)
                return new_pos + new_len, 'ok'
            return old_rel, 'mismatch'

    # Case D: suffix lands after a short run of printable separator bytes
    # that immediately follow a control code — e.g. the `@` block-separator
    # in dialog/item-desc entries:  …[16:5A]@<suffix-starts-here>…
    # Neither Case A nor Case B fires because the separator (printable
    # 0x20..0x7E) puts the suffix N bytes past the control-code end.
    #
    # We match by finding the same control code in new_parent and advancing
    # the same N printable bytes past it, then sanity-check that the byte
    # immediately before the new suffix position matches the byte
    # immediately before old_rel (the separator survived translation —
    # `@` is `@` in any language). N is small in practice (1..4); we cap
    # at 8 to avoid false positives where the suffix actually lives deep
    # inside a translated phrase.
    for k, (b, pos) in enumerate(old_ctrls):
        ctrl_len = _ctrl_advance(old_parent, pos)
        ctrl_end = pos + ctrl_len
        gap = old_rel - ctrl_end
        if gap <= 0 or gap > 8:
            continue
        # All bytes in (ctrl_end .. old_rel) must be printable text — no
        # other control codes between this ctrl and the suffix.
        if any(old_parent[i] < 0x20 for i in range(ctrl_end, old_rel)):
            continue
        nc = _same_kind_new_ctrl(b, k)   # same-kind ordinal (see Cases A/B)
        if nc is None:
            continue
        new_pos = nc[1]
        new_len = _ctrl_advance(new_parent, new_pos)
        candidate = new_pos + new_len + gap
        if candidate > len(new_parent) or old_rel == 0:
            continue
        # Validation: the byte immediately before the new suffix must equal
        # the byte immediately before the old suffix (i.e. the separator
        # character — `@`, `.`, etc. — is unchanged by translation).
        if new_parent[candidate - 1] != old_parent[old_rel - 1]:
            continue
        return candidate, 'ok'

    # Case C: suffix lands inside text (no nearby CTRL boundary).
    # Use legacy relative-offset behavior.
    return old_rel, 'fallback'


def _slot_is_pagebreak_delimited(en: bytes, slot_rel: int) -> bool:
    """True if the byte(s) immediately before `slot_rel` (skipping trailing
    ASCII spaces) are a `[16:NN]` page-break control code — the combat-help /
    tutorial compose pattern. Nested-tail suffixes (a slot pointing AT a
    portrait/wait code, e.g. after `[13:XX]`/`[10:01]`, inside one continuous
    string) return False so they take the normal suffix re-anchor path.
    Mirrors the separator detection in the compose loop."""
    if slot_rel <= 0 or slot_rel > len(en):
        return False
    j = slot_rel
    while j > 0 and en[j - 1] == 0x20:
        j -= 1
    return j >= 2 and en[j - 2] == 0x16


def find_parent_root(roots: list, off: int):
    """Return the root offset that contains `off` (last root <= off)."""
    parent = None
    for r in roots:
        if r <= off:
            parent = r
        else:
            break
    return parent


# ---------------------------------------------------------------------------
# Entry rebuilder
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Multi-item root composition
# ---------------------------------------------------------------------------
# Item descriptions in BoF4 PC are stored as ONE root containing multiple
# item descriptions glued together with `[16:NN]@` separators (where NN
# varies per category — 0x0D / 0x16 / 0xEF / 0x0E / 0x11 / 0x2B observed).
# The offset table contains one slot per item, pointing at that item's
# starting byte within the shared root. Example: AB000_00 entry 1 root @
# 0x0793 holds 12 consecutive item descriptions starting with "Healing Herb".
#
# The PSX side stores each item as a stand-alone string per slot, so the
# matcher produces one short German per PC suffix-slot. The repacker's
# normal flow only takes the ROOT slot's translation, which contains just
# ONE item; the suffix offsets within that short German root then land
# mid-word or past the null terminator, producing fragments in-game.
#
# `_compose_multi_item_root` rebuilds the multi-item root by interleaving
# each item's per-slot German with the original `[16:NN]@` separators
# preserved from the pristine English root. After composition the suffix
# offsets re-anchor naturally via `_resolve_suffix_offset`'s Case D
# (printable separator bytes after a control code).


# Item-divider trailing byte: `[16:NN]` followed by '@'(0x40) / 'A'(0x41) /
# 'B'(0x42) / 'C'(0x43). The trailing letter is a line/box index (0..3); a
# tally across all text_dump/ confirms exactly this set is used as item
# separators (@ 7238x, A 2016x, B 2023x, C 4x). Earlier code only handled
# @/A, which merged every B/C-delimited item (e.g. the faerie skill block).
_DIVIDER_TRAIL_BYTES = (0x40, 0x41, 0x42, 0x43)
_FIRST_DIVIDER_RE = re.compile(r'\[16:[0-9A-Fa-f]{2}\][@-C]')


def _leading_item_text(text: str) -> str:
    """Return only the leading-item portion of a translation.

    PSX-imported translations for SUFFIX slots often carry the WHOLE
    multi-item-concat string (the matcher stripped the same way for both
    sides, so the entire downstream concatenation matched). For
    composition we only want THIS slot's item — everything from the
    start up to (but not including) the next `[16:NN]@` divider.

    LEADING dividers are KEPT as a prefix: a slot can legitimately point AT a
    divider (e.g. `[16:37]ASteal…`), and the engine renders that divider. If we
    stripped it (old behavior: `search` matched at pos 0 -> returned ''), the
    slot became an empty string = blank in-game. So skip any leading
    consecutive dividers, then cut at the FIRST divider that follows content."""
    m = _FIRST_DIVIDER_RE.match(text)   # leading divider?
    if m:
        # Keep leading consecutive dividers as prefix; find the next divider
        # AFTER real content begins.
        i = 0
        while True:
            mm = _FIRST_DIVIDER_RE.match(text, i)
            if mm and mm.start() == i:
                i = mm.end()
            else:
                break
        m2 = _FIRST_DIVIDER_RE.search(text, i)
        return text[:m2.start()] if m2 else text
    m = _FIRST_DIVIDER_RE.search(text)
    return text[:m.start()] if m else text


def _find_item_segments(en_bytes: bytes) -> list:
    """Split a root's English bytes into (item_start, item_end, divider_end)
    triples by `[16:NN]@` (or `[16:NN]A`/`B`/`C`) dividers.

    Leading dividers (root starts with `[16:NN]@`) are absorbed into item 0
    as its prefix — keeping the slot at root+0 pointing at the divider.

    A divider is ONLY a `[16:NN]` control code IMMEDIATELY followed by a
    trailing box-index byte `@`/`A`/`B`/`C` (0x40-0x43). A bare `[16:NN]`
    with no such trail (e.g. `[16:0C]` page-breaks in the combat-help /
    tutorial blocks, which are trailed by a space) is a PAGE BREAK, not an
    item divider — treating it as one wrongly reshapes those roots into
    per-page slots and drops the translation for every slot but one. The
    trail requirement here matches `_leading_item_text`'s `_FIRST_DIVIDER_RE`
    so the two divider notions stay consistent.

    Returns [] if the bytes contain no inter-item dividers.
    """
    n = len(en_bytes)
    if n < 4:
        return []
    items = []
    i = 0
    # Skip leading `[16:NN]@` dividers — they form item 0's prefix. A bare
    # `[16:NN]` (no @/A/B/C trail) is content, not a divider, so don't skip it.
    while (i + 2 < n and en_bytes[i] == 0x16
           and en_bytes[i + 2] in _DIVIDER_TRAIL_BYTES):
        i += 3
    item_start = 0
    while i < n:
        if (i + 2 < n and en_bytes[i] == 0x16
                and en_bytes[i + 2] in _DIVIDER_TRAIL_BYTES):
            div_end = i + 3   # [16:NN] opcode+arg (2) + '@'/'A'/'B'/'C' (1)
            # Only treat as inter-item divider if there's content after
            if div_end < n:
                items.append((item_start, i, div_end))
                item_start = div_end
                i = div_end
            else:
                i = div_end
        else:
            i += 1
    if item_start < n:
        items.append((item_start, n, n))
    return items if len(items) >= 2 else []


def _is_multi_item_root(entry: bytes, root_off: int) -> bool:
    """Return True if this root contains `[16:NN]@`-delimited items."""
    raw = root_raw_bytes(entry, root_off)
    if not raw.endswith(b'\x00'):
        return False
    return bool(_find_item_segments(raw[:-1]))


def _slot_render_range(en_bytes: bytes, slot_rel: int):
    """Return (start, end) of exactly what the game renders when it points at
    `slot_rel`: any leading `[16:NN]X` divider(s) AT the slot are kept (they're
    format codes the engine draws through), then content, stopping at the next
    divider that follows content.

    This is what the engine actually does — critically it handles slots that
    point AT a divider which itself follows another divider (e.g. root bytes
    `…[16:1B]@[16:37]ASteal…` with a slot on the `[16:37]A`). The old
    `_item_for_slot_rel` treated the zero-length gap between two consecutive
    dividers as an EMPTY item, so such slots were emitted as a bare `\\0`
    (= blank description in-game). See [[project-multi-item-divider]].
    """
    n = len(en_bytes)
    i = slot_rel
    # Include leading consecutive `[16:NN]X` dividers at the slot.
    while i + 2 < n and en_bytes[i] == 0x16 and en_bytes[i + 2] in _DIVIDER_TRAIL_BYTES:
        i += 3
    # Consume content until the next divider (or end of root).
    while i < n:
        if i + 2 < n and en_bytes[i] == 0x16 and en_bytes[i + 2] in _DIVIDER_TRAIL_BYTES:
            break
        i += 1
    return (slot_rel, i)


def _item_for_slot_rel(entry: bytes, root_off: int, slot_rel: int):
    """Return the (start, end) byte range the game renders for a slot pointing
    at `slot_rel` within the English root (leading divider(s) + content up to the
    next divider). None only if the root isn't a proper \\0-terminated string."""
    raw = root_raw_bytes(entry, root_off)
    if not raw.endswith(b'\x00'):
        return None
    return _slot_render_range(raw[:-1], slot_rel)


def rebuild_entry(entry: bytes, replacements: dict, verbose: bool = True,
                  base_name: str = None) -> bytes:
    """
    Rebuild a text entry with replacement strings.

    replacements: {entry_offset: python_string}
        Only ROOT offsets should be translated; suffix offsets are recalculated
        automatically. If a suffix offset appears in replacements it is ignored
        with a warning (its parent root controls the content).

    Returns new entry bytes.
    """
    pool_start = struct.unpack_from('<H', entry, 0)[0]
    # IMPLICIT-POOL_START SPECIAL CASE: in AREAM005/S010/S012 entry 7,
    # the offset table starts at byte 0 (no explicit pool_start word)
    # and the pool begins at byte 0x200. The "first word" 0x0201 is
    # actually slot[0] (= first pointer). Detect and override.
    if pool_start == 0x0201 and len(entry) > 0x220:
        pool_start = 0x200
    num_offsets = pool_start // 2
    orig_offsets = [struct.unpack_from('<H', entry, i * 2)[0] for i in range(num_offsets)]

    roots = scan_roots(entry, pool_start)
    root_set = set(roots)

    # Identify multi-item roots and collect every slot pointing into each.
    # For these roots we abandon the shared-root + suffix-offset layout and
    # emit each slot as its OWN null-terminated string (Option B). The PC
    # renderer reads from a slot's offset until \0 and does NOT stop at
    # `[16:NN]@` separators (confirmed empirically), so the multi-item-
    # concatenated layout shipped in the original English DAT can't carry
    # variable-length German translations correctly. Independent per-slot
    # strings sidesteps the problem at the cost of pool growth (no shared
    # suffixes); the resulting entry still fits under the u16 cap when
    # each per-item German is stripped to its leading-item portion.
    sorted_roots_list = sorted(roots)
    multi_item_root_slots = {}     # root_off -> sorted list of slots pointing in
    multi_item_slots = set()       # union of all such slots
    for i, root_off in enumerate(sorted_roots_list):
        if _is_composed_root(base_name, root_off):
            continue   # handled by the composed-root branch, not independent slots
        if not _is_multi_item_root(entry, root_off):
            continue
        next_start = (sorted_roots_list[i + 1]
                      if i + 1 < len(sorted_roots_list)
                      else len(entry))
        slots = sorted(
            off for off in set(orig_offsets)
            if root_off <= off < next_start
        )
        if slots:
            multi_item_root_slots[root_off] = slots
            multi_item_slots.update(slots)

    # PAGE-BREAK COMPOSE ROOTS: a root that is NOT an `[16:NN]@` multi-item
    # block but whose offset table has several slots pointing into it at
    # `[16:NN]`-page-break boundaries (combat-help / tutorial blocks), where a
    # NON-ROOT slot carries its own translation. The normal root path ignores
    # suffix translations, so we'd silently drop the German on those page
    # slots. Instead compose the root from its per-page slots: page = that
    # slot's translation if present, else the pristine English page, with the
    # English `[16:NN] ` page separators preserved between pages. Each page
    # slot is registered in old_to_new directly so the suffix-resolver skips
    # it. Only fires when a suffix page is actually translated (root-only
    # translations still take the normal path, unchanged).
    compose_root_slots = {}    # root_off -> sorted slots (incl. root as page 0)
    compose_slots = set()      # union of non-root compose slots
    for i, root_off in enumerate(sorted_roots_list):
        if root_off in multi_item_root_slots:
            continue
        next_start = (sorted_roots_list[i + 1]
                      if i + 1 < len(sorted_roots_list)
                      else len(entry))
        slots = sorted(off for off in set(orig_offsets)
                       if root_off <= off < next_start)
        non_root = [s for s in slots if s != root_off]
        # GUARD 1 — page 0 must exist. The compose loop treats slots[0] as the
        # root's own start (page 0). That only holds when a pointer-table slot
        # actually points AT the root. Some dialog roots are ORPHANS: no slot
        # points at them (the engine reaches them by sequential continuation
        # from the previous string's \0), and only INTERIOR points are in the
        # offset table. Composing such a root starts at the first interior slot
        # and SILENTLY DROPS every byte before it — i.e. the whole first
        # sentence. Require the root itself to be a slot. (AREAE023 e12 0x0619:
        # slot 21 → [14:42], then the orphan cutscene root, then slots 0x0632…
        # are nested tails — composing dropped "Hold on to something!".)
        if root_off not in slots:
            continue
        # GUARD 2 — genuine `[16:NN]` page-break layout only. Compose is for
        # combat-help / tutorial blocks whose pages are delimited by `[16:NN]`
        # page-break codes. NESTED-TAIL suffixes (each slot points progressively
        # deeper into ONE continuous string, delimited by wait/clear codes like
        # `[13:XX]`/`[10:01]`) must instead flow through the normal root+suffix
        # re-anchor path — the root's full translation is authoritative and each
        # tail re-anchors into it by control-code ordinal.
        # A page slot only counts as genuinely translated if its replacement
        # DIFFERS from the pristine English at that offset. The export echoes
        # English text for every slot, including page anchors the user never
        # touched (e.g. the combat-help topics, where the whole block was
        # translated on the ROOT string as one multi-page value). Without this
        # differ-check, those English echoes would make compose fire and
        # rebuild the root from per-anchor English pages, discarding the root's
        # German. See [[combat-help-split-bug]].
        en_root = root_raw_bytes(entry, root_off)
        en_root = en_root[:-1] if en_root.endswith(b'\x00') else en_root
        if non_root and any(
                s in replacements and replacements[s].strip()
                and replacements[s] != _decode_for_compare(entry, s)
                and _slot_is_pagebreak_delimited(en_root, s - root_off)
                for s in non_root):
            compose_root_slots[root_off] = slots
            compose_slots.update(non_root)

    # Warn if any replacement targets a suffix (not a root) AND the content differs.
    # Suppress for multi-item suffix slots (they're emitted as independent
    # strings below) and for compose page slots (they're composed into the root).
    for off in replacements:
        if off not in root_set and off not in multi_item_slots \
                and off not in compose_slots:
            parent = find_parent_root(roots, off)
            # Composed-root sub-slots ARE used (glued into the root); don't warn.
            if _is_composed_root(base_name, parent):
                continue
            orig_text = _decode_for_compare(entry, off)
            if replacements[off] != orig_text:
                parent_str = f"{parent:#06x}" if parent is not None else "unknown"
                print(f"  WARNING: replacement at {off:#06x} is a SUFFIX of root {parent_str}. "
                      f"It will be ignored - translate the root string instead.")

    # Build new pool root by root, tracking old -> new offsets
    new_pool = bytearray()
    old_to_new = {}   # entry-relative offset -> new entry-relative offset
    # Track the new raw bytes for every root that was replaced or composed
    # so the suffix-resolver loop below can re-anchor against the FULL new
    # bytes (not just `encode_string(replacements[root])`, which for
    # composed multi-item roots covers only the first item).
    new_root_raw = {}   # root_off -> new raw bytes incl. trailing \0

    for root_off in roots:
        if _is_composed_root(base_name, root_off) and _is_multi_item_root(entry, root_off):
            # COMPOSED-ROOT PATH (narrow allowlist — see COMPOSED_ROOT_OFFSETS):
            # rebuild the multi-item block WITH its `[16:NN]@` dividers intact,
            # gluing each segment's per-slot German (English fallback where a
            # slot is untranslated) between the pristine dividers. The root is
            # emitted as ONE \0-terminated string; every slot pointing into it is
            # re-anchored by the normal suffix resolver below (Case D handles the
            # printable `@` that trails each divider). Needed for menus that walk
            # the dividers from the root instead of following the slot pointer.
            old_raw = root_raw_bytes(entry, root_off)
            en_bytes = old_raw[:-1] if old_raw.endswith(b'\x00') else old_raw
            segs = _find_item_segments(en_bytes)
            composed = bytearray()
            n_tr = 0
            for (start, end, div_end) in segs:
                slot_off = root_off + start
                if slot_off in replacements and replacements[slot_off].strip():
                    try:
                        composed += encode_string(
                            _leading_item_text(replacements[slot_off]))
                        n_tr += 1
                    except Exception:
                        composed += en_bytes[start:end]
                else:
                    composed += en_bytes[start:end]
                composed += en_bytes[end:div_end]     # pristine `[16:NN]@` divider
            composed += b'\x00'
            old_to_new[root_off] = pool_start + len(new_pool)
            new_root_raw[root_off] = bytes(composed)
            new_pool.extend(composed)
            if verbose:
                print(f"    [{root_off:#06x}] composed multi-item root: "
                      f"{len(segs)} segments ({n_tr} translated), dividers kept")
            continue

        if root_off in multi_item_root_slots:
            # MULTI-ITEM PATH: emit each slot pointing into this root as
            # its own independent null-terminated string (Option B).
            old_raw = root_raw_bytes(entry, root_off)
            en_bytes = old_raw[:-1] if old_raw.endswith(b'\x00') else old_raw
            for slot in multi_item_root_slots[root_off]:
                if slot in old_to_new:
                    continue   # already emitted (duplicate slot in offset table)
                old_to_new[slot] = pool_start + len(new_pool)
                if slot in replacements:
                    try:
                        ger_text = _leading_item_text(replacements[slot])
                        raw_s = encode_string(ger_text) + b'\x00'
                    except Exception:
                        # Fall back to the slot's English item bytes
                        rng = _item_for_slot_rel(entry, root_off,
                                                 slot - root_off)
                        raw_s = (en_bytes[rng[0]:rng[1]] if rng
                                 else en_bytes) + b'\x00'
                else:
                    rng = _item_for_slot_rel(entry, root_off,
                                             slot - root_off)
                    raw_s = (en_bytes[rng[0]:rng[1]] if rng
                             else en_bytes) + b'\x00'
                new_pool.extend(raw_s)
            if verbose:
                print(f"    [{root_off:#06x}] split into "
                      f"{len(multi_item_root_slots[root_off])} independent slots")
            continue

        if root_off in compose_root_slots:
            # PAGE-BREAK COMPOSE PATH: build ONE null-terminated root from the
            # per-page slots, substituting each page's translation where present
            # and preserving the English `[16:NN] ` separators between pages.
            slots = compose_root_slots[root_off]
            old_raw = root_raw_bytes(entry, root_off)
            en = old_raw[:-1] if old_raw.endswith(b'\x00') else old_raw
            rels = [s - root_off for s in slots]
            composed = bytearray()
            n_tr = 0
            for idx, s in enumerate(slots):
                # register this page slot's new absolute offset
                old_to_new[s] = pool_start + len(new_pool) + len(composed)
                start = rels[idx]
                if idx + 1 < len(slots):
                    boundary = rels[idx + 1]
                    # separator = `[16:NN]` + trailing space(s) immediately
                    # before the next page slot; content is everything before it
                    j = boundary
                    while j > start and en[j - 1] == 0x20:
                        j -= 1
                    if j >= start + 2 and en[j - 2] == 0x16:
                        sep_start = j - 2
                    else:
                        sep_start = boundary   # no recognizable separator
                    sep = en[sep_start:boundary]
                    eng_content = en[start:sep_start]
                else:
                    sep = b''
                    eng_content = en[start:]
                if s in replacements and replacements[s].strip():
                    try:
                        content = encode_string(replacements[s])
                        n_tr += 1
                    except Exception:
                        content = eng_content
                else:
                    content = eng_content
                composed += bytes(content) + sep
            composed += b'\x00'
            for s in slots:
                # already set above; keep root->composed mapping too
                pass
            new_root_raw[root_off] = bytes(composed)
            new_pool.extend(composed)
            if verbose:
                print(f"    [{root_off:#06x}] composed {len(slots)} page-slots "
                      f"({n_tr} translated, rest English)")
            continue

        new_root_off = pool_start + len(new_pool)
        old_to_new[root_off] = new_root_off

        if root_off in replacements:
            raw = encode_string(replacements[root_off]) + b'\x00'
            old_raw = root_raw_bytes(entry, root_off)
            new_root_raw[root_off] = raw
            if verbose and raw != old_raw:
                delta = len(raw) - len(old_raw)
                print(f"    [{root_off:#06x}] replaced: {len(old_raw)-1} -> {len(raw)-1} bytes "
                      f"({delta:+d})")
        else:
            raw = root_raw_bytes(entry, root_off)

        new_pool.extend(raw)

    # Resolve suffix offsets. Default behavior is `new_parent + rel` (preserves
    # the same byte distance), but when the parent was translated and the new
    # bytes differ we re-anchor the suffix by counting control-code tokens so
    # it still lands on the intended page-break / portrait boundary.
    # Multi-item slots already emitted as independent strings above; skip them.
    for off in set(orig_offsets):
        if off in root_set or off in old_to_new:
            continue
        if off in multi_item_slots:
            continue
        parent = find_parent_root(roots, off)
        if parent is None:
            print(f"  WARNING: offset {off:#06x} has no parent root; keeping as-is.")
            old_to_new[off] = off
            continue
        rel = off - parent
        new_parent_off = old_to_new[parent]

        if parent in new_root_raw:
            old_parent_raw = root_raw_bytes(entry, parent)
            new_parent_raw = new_root_raw[parent]
            if new_parent_raw == old_parent_raw:
                old_to_new[off] = new_parent_off + rel
            else:
                new_rel, status = _resolve_suffix_offset(
                    old_parent_raw, rel, new_parent_raw)
                old_to_new[off] = new_parent_off + new_rel
                if status == 'ok' and new_rel != rel:
                    if verbose:
                        print(f"    suffix {off:#06x} re-anchored: rel "
                              f"+{rel:#04x} -> +{new_rel:#04x} inside translated "
                              f"root {parent:#06x}")
                elif status != 'ok':
                    print(f"  WARNING: suffix {off:#06x} (rel +{rel:#04x} inside "
                          f"root {parent:#06x}) could not be re-anchored "
                          f"({status}). Falling back to legacy offset; if the "
                          f"control-code structure of the translation changed, "
                          f"this suffix may land on garbage bytes.")
        else:
            old_to_new[off] = new_parent_off + rel

    # Rebuild offset table
    new_entry = bytearray()
    for off in orig_offsets:
        new_off = old_to_new.get(off, off)
        if new_off > 0xFFFF:
            raise OverflowError(
                f"Offset {new_off:#x} exceeds uint16 max. Entry is too large to fit "
                f"in the offset table. Consider shorter translations."
            )
        new_entry.extend(struct.pack('<H', new_off))
    new_entry.extend(new_pool)

    return bytes(new_entry)


# ---------------------------------------------------------------------------
# Multi-level entry rebuilder (INIT.DAT item names)
# ---------------------------------------------------------------------------

def rebuild_multilevel_entry(entry: bytes, replacements: dict, verbose: bool = True) -> bytes:
    """
    Rebuild a multi-level pointer entry (e.g., INIT.DAT entry 6 with item names).

    Structure: 12 uint16 top-level pointers (alternating: category_offset, 0),
    each category is a standard offset-table + string-pool sub-entry.

    replacements: {absolute_entry_offset: translated_text}
    """
    num_top = struct.unpack_from('<H', entry, 0)[0] // 2
    top_ptrs = [struct.unpack_from('<H', entry, i * 2)[0] for i in range(num_top)]
    header_size = num_top * 2

    # Find category start offsets (non-zero, even-indexed pointers)
    cat_indices = []
    cat_offsets = []
    for i in range(0, num_top, 2):
        if top_ptrs[i] > 0:
            cat_indices.append(i)
            cat_offsets.append(top_ptrs[i])

    # Calculate category sizes
    all_boundaries = sorted(set(cat_offsets + [len(entry)]))
    cat_sizes = {}
    for co in cat_offsets:
        idx = all_boundaries.index(co)
        cat_sizes[co] = all_boundaries[idx + 1] - co

    # Rebuild each category
    new_categories = {}  # cat_offset -> new_bytes
    for co in cat_offsets:
        cs = cat_sizes[co]
        cat_entry = bytes(entry[co:co + cs])

        # Map translations: absolute entry offset → category-relative offset
        cat_replacements = {}
        for abs_off, trans_text in replacements.items():
            if co <= abs_off < co + cs:
                rel_off = abs_off - co
                cat_replacements[rel_off] = trans_text

        if cat_replacements:
            if verbose:
                print(f"  Category @ {co:#06x}: {len(cat_replacements)} translation(s)")
            new_cat = rebuild_entry(cat_entry, cat_replacements, verbose=verbose)
        else:
            new_cat = cat_entry

        new_categories[co] = new_cat

    # Reassemble: header + categories in order
    new_top_ptrs = list(top_ptrs)
    current_offset = header_size
    for ci in cat_indices:
        co = top_ptrs[ci]
        new_top_ptrs[ci] = current_offset
        current_offset += len(new_categories[co])

    new_entry = bytearray()
    for i in range(num_top):
        new_entry.extend(struct.pack('<H', new_top_ptrs[i]))
    for co in cat_offsets:
        new_entry.extend(new_categories[co])

    return bytes(new_entry)


# ---------------------------------------------------------------------------
# Enemy/boss name table rebuilder (AREAB*/BOSS*.DAT entries with the
# size==0x8E0 / f3==0x1F5800 tag — see extract_text.is_enemy_table_entry).
#
# These are NOT flat string-pool entries. Layout: 0xA0-byte attack-pattern
# header followed by 8 enemy records of 264 bytes each. The first 8 bytes
# of each record are the name (raw ASCII, null-padded short, no terminator
# if exactly 8 chars). All other bytes are stats / palette / behavior
# fields and MUST NOT be touched.
#
# Repack is therefore a fixed-position byte patch: leave the entry buffer
# alone, then for each (rel_offset, translation) write up to 8 ASCII bytes
# at rel_offset. Translations longer than 8 bytes are REJECTED with a
# warning (we don't silently truncate — that destroys translator work).
# ---------------------------------------------------------------------------

ENEMY_TABLE_SIZE       = 0x8E0
ENEMY_TABLE_F3         = 0x1F5800
ENEMY_TABLE_NAME_BYTES = 8


def is_enemy_table_entry(e_size: int, e_f2: int, e_f3: int) -> bool:
    """Mirrors extract_text.is_enemy_table_entry — keep in sync."""
    return e_f2 == 0 and e_size == ENEMY_TABLE_SIZE and e_f3 == ENEMY_TABLE_F3


def _printable(s: str) -> str:
    # Some original 8-byte slots contain non-text bytes (e.g. 0x94) that
    # latin1 maps to control codepoints. cp1252 (default Windows console
    # codepage) can't encode those, so a raw print() crashes. Escape any
    # non-ASCII / non-printable char to a \xNN form for diagnostics only.
    return s.encode('ascii', errors='backslashreplace').decode('ascii')


def patch_enemy_table_entry(entry: bytes, replacements: dict,
                            verbose: bool = True) -> bytes:
    """In-place 8-byte name patches into a copy of the entry buffer.

    replacements: {entry_relative_offset: python_string}
    Returns: new entry bytes of EXACTLY the same length as `entry`.
    Translations that encode to >8 bytes are skipped with a warning so
    the rest of the table still updates and the user keeps their input.
    """
    out = bytearray(entry)
    for rel_off, text in sorted(replacements.items()):
        if rel_off + ENEMY_TABLE_NAME_BYTES > len(out):
            print(f"  WARNING: enemy-name slot at {rel_off:#06x} is past "
                  f"entry end ({len(out):#x}); skipping")
            continue
        try:
            raw = encode_string(text)
        except ValueError as ex:
            print(f"  WARNING: enemy-name [{rel_off:#06x}] '{_printable(text)}' "
                  f"could not encode ({ex}); skipping — original bytes kept")
            continue
        if len(raw) > ENEMY_TABLE_NAME_BYTES:
            extra = len(raw) - ENEMY_TABLE_NAME_BYTES
            print(f"  WARNING: enemy-name [{rel_off:#06x}] '{_printable(text)}' "
                  f"is {len(raw)} bytes — exceeds {ENEMY_TABLE_NAME_BYTES}-byte "
                  f"slot by {extra}; skipping (original bytes kept). The "
                  f"editor accepted it because the cap is per-engine, not "
                  f"per-translator: reword to <={ENEMY_TABLE_NAME_BYTES} "
                  f"bytes to ship.")
            continue
        # Pad with nulls so the next field's first byte is preserved if the
        # new name is shorter than the old one.
        slot = raw + b'\x00' * (ENEMY_TABLE_NAME_BYTES - len(raw))
        old_slot = bytes(out[rel_off:rel_off + ENEMY_TABLE_NAME_BYTES])
        if slot == old_slot:
            continue
        out[rel_off:rel_off + ENEMY_TABLE_NAME_BYTES] = slot
        if verbose:
            old_name = old_slot.rstrip(b'\x00').decode('latin1', errors='replace')
            new_name = raw.decode('latin1', errors='replace')
            print(f"    [{rel_off:#06x}] '{_printable(old_name)}' -> "
                  f"'{_printable(new_name)}' "
                  f"({len(raw)}/{ENEMY_TABLE_NAME_BYTES} bytes)")
    return bytes(out)


# ---------------------------------------------------------------------------
# DAT patcher
# ---------------------------------------------------------------------------

def repack_dat(dat_path: str, dump_strings: dict, out_path: str, verbose: bool = True):
    """
    Load DAT, rebuild text entries from dump_strings, write to out_path.

    dump_strings: {entry_idx: {str_offset: python_string}}
    """
    with open(dat_path, 'rb') as f:
        dat = bytearray(f.read())

    # Materialise any baked NA-uncensor dialogue into the source DAT before the
    # normal rebuild, so the restored lines exist as distinct strings that any
    # translation for them then replaces normally. No-op if already present or
    # unpatched. Keeps DAT_backup pristine (censored). See uncensor_patch.py.
    dat = bytearray(uncensor_patch.apply(dat_path, dat))

    toc_size    = struct.unpack_from('<I', dat, 0)[0]
    num_entries = (toc_size - 16) // 16
    base_name   = os.path.splitext(os.path.basename(dat_path))[0]

    modified = 0

    for entry_idx, replacements in sorted(dump_strings.items()):
        if entry_idx >= num_entries:
            print(f"SKIP entry {entry_idx}: index beyond TOC ({num_entries} entries)")
            continue

        # Per-entry space-byte override (see LEGACY_SPACE_ENTRIES): the combat-
        # Help menu entries must encode spaces as ASCII 0x20, not the 0xFF glyph,
        # or the menu won't render. Force it here and restore after this entry so
        # every other entry keeps the global (0xFF) setting.
        global SPACE_REPLACEMENT_BYTE
        _saved_space = SPACE_REPLACEMENT_BYTE
        if (base_name, entry_idx) in LEGACY_SPACE_ENTRIES:
            SPACE_REPLACEMENT_BYTE = None

        toc_base = 16 + entry_idx * 16
        e_off  = struct.unpack_from('<I', dat, toc_base)[0]
        e_size = struct.unpack_from('<I', dat, toc_base + 4)[0]
        e_f2   = struct.unpack_from('<I', dat, toc_base + 8)[0]
        e_f3   = struct.unpack_from('<I', dat, toc_base + 12)[0]

        # f2 != 0 means compressed/GPU/audio — can't repack
        if e_f2 != 0:
            print(f"SKIP entry {entry_idx}: compressed or non-text entry "
                  f"(f2={e_f2:#010x})")
            continue

        print(f"\nEntry {entry_idx} @ {e_off:#010x}  size={e_size:#08x}  f3={e_f3:#08x}")
        entry = bytes(dat[e_off : e_off + e_size])

        # Enemy/boss name table (AREAB*/BOSS*.DAT) — fixed-stride struct
        # layout, NOT a flat string pool. Patches the 8-byte name slots
        # in place and leaves every other byte untouched.
        if is_enemy_table_entry(e_size, e_f2, e_f3):
            new_entry = patch_enemy_table_entry(entry, replacements,
                                                verbose=verbose)
        else:
            # Check if this is a multi-level pointer entry (e.g., INIT.DAT item names)
            pool_start = struct.unpack_from('<H', entry, 0)[0]
            # Implicit-pool_start special case (see rebuild_entry).
            if pool_start == 0x0201 and len(entry) > 0x220:
                pool_start = 0x200
            num_offsets = pool_start // 2
            if num_offsets <= 16:
                new_entry = rebuild_multilevel_entry(entry, replacements, verbose=verbose)
            else:
                new_entry = rebuild_entry(entry, replacements, verbose=verbose,
                                          base_name=base_name)
        # Restore the global space byte now that this entry's strings are encoded.
        if SPACE_REPLACEMENT_BYTE != _saved_space:
            print(f"  space: forced ASCII 0x20 for {base_name} entry {entry_idx} "
                  f"(0xFF breaks its menu — see LEGACY_SPACE_ENTRIES)")
        SPACE_REPLACEMENT_BYTE = _saved_space
        new_size  = len(new_entry)
        delta     = new_size - e_size

        print(f"  old={e_size:#x}  new={new_size:#x}  delta={delta:+d}")

        if new_size <= e_size:
            # Patch in-place; zero-pad any slack
            dat[e_off : e_off + new_size]    = new_entry
            dat[e_off + new_size : e_off + e_size] = bytes(e_size - new_size)
            print(f"  Written in-place (zero-padded {e_size - new_size} byte(s))")
        else:
            # Append at end of file and redirect the TOC pointer
            new_e_off = len(dat)
            dat.extend(new_entry)
            struct.pack_into('<I', dat, toc_base,     new_e_off)
            struct.pack_into('<I', dat, toc_base + 4, new_size)
            print(f"  Appended at {new_e_off:#010x}, TOC updated "
                  f"(old entry at {e_off:#010x} is now dead space)")

        modified += 1

    with open(out_path, 'wb') as f:
        f.write(dat)

    print(f"\nDone: {modified} entry/entries modified -> {out_path}")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

# Root game directory — find it by walking up from the script location until we see DAT/
def _find_game_dir():
    d = _anchor_dir()
    for _ in range(5):  # max 5 levels up
        if os.path.isdir(os.path.join(d, "DAT")):
            return d
        d = os.path.dirname(d)
    # Fallback: assume script is in game root
    return _anchor_dir()

GAME_DIR   = _find_game_dir()
DAT_DIR    = os.path.join(GAME_DIR, "DAT")
BACKUP_DIR = os.path.join(GAME_DIR, "DAT_backup")

# ── Dengeki Store portrait entries ──────────────────────────────────────────
# The store portrait is TWO raw graphics entries (art f3=0x1E080200 in AREAS052,
# CLUT f3=0x2FC00 in AREAD068) that don't exist in the pristine DAT_backup. A text
# repack rebuilds from DAT_backup and therefore drops them, so we re-add them from
# dengeki_assets/ after each repack of these two files. Idempotent (skips if the
# f3 entry is already present). See project_dengeki_store.
DENGEKI_ASSETS   = os.path.join(GAME_DIR, "dengeki_assets")
DENGEKI_PORTRAIT = {"AREAD068": "portrait_clut", "AREAS052": "portrait_art"}

def _add_dat_entry(dat_path, data, f2, f3):
    """Append a new TOC entry to a DAT (count+1, shift existing data +16, append
    the entry's bytes). Returns False if an entry with this f3 already exists."""
    d = bytearray(open(dat_path, "rb").read())
    ts = struct.unpack_from("<I", d, 0)[0]; n = (ts - 16) // 16
    ent = [list(struct.unpack_from("<IIII", d, 16 + i * 16)) for i in range(n)]
    if any(e[3] == f3 for e in ent):
        return False
    new_ts = ts + 16
    existing = d[ts:]
    out = bytearray(struct.pack("<I", new_ts) + d[4:16])
    ne = [(off + 16, sz, ef2, ef3) for (off, sz, ef2, ef3) in ent]
    ne.append((new_ts + len(existing), len(data), f2, f3))
    for e in ne:
        out += struct.pack("<IIII", *e)
    out += existing + data
    open(dat_path, "wb").write(out)
    return True

def _apply_dengeki_portrait(dat_path, base):
    """If `base` is a Dengeki Store DAT, re-add its saved portrait entry."""
    asset = DENGEKI_PORTRAIT.get(base.upper())
    if not asset:
        return
    binp = os.path.join(DENGEKI_ASSETS, asset + ".bin")
    metp = os.path.join(DENGEKI_ASSETS, asset + ".meta")
    if not (os.path.exists(binp) and os.path.exists(metp)):
        print(f"  NOTE: {base} portrait asset {asset}.bin missing in dengeki_assets/ "
              f"— portrait not re-applied")
        return
    data = open(binp, "rb").read()
    meta = {}
    for ln in open(metp):
        if "=" in ln:
            k, v = ln.strip().split("=")
            meta[k] = int(v, 16) if v.startswith("0x") else int(v)
    if _add_dat_entry(dat_path, data, meta["f2"], meta["f3"]):
        print(f"  re-applied Dengeki portrait ({asset}, f3={meta['f3']:#x})")

# Where repacked DATs are WRITTEN. Defaults to the live DAT/ (overwrite in place,
# the standalone-CLI behavior). main() overrides this with --out-dir so the editor
# can build into a separate folder and never touch the live game DATs. The text
# SOURCE is always DAT_backup/ regardless — only the destination changes.
OUT_DIR = DAT_DIR

# Optional German-GRAPHICS folder (--import-dir, e.g. graphics_to_edit/). After text
# is rebuilt from DAT_backup/, every NON-TEXT entry (textures/fonts) is OVERLAID from
# <IMPORT_DIR>/<name>.DAT into the output (see _merge_graphics_from_import). Text is
# never read from here, so it can't scramble strings even if these DATs carry German
# text. DATs not present here just keep their US graphics. None = off (no overlay).
IMPORT_DIR = None

# Pristine-text guard (shared with the editor). Lazily loaded once.
try:
    import pristine_guard as _pg
except Exception:
    _pg = None
_MANIFEST = None
_MANIFEST_LOADED = False
_PROCESSED = set()   # upper-case basenames repacked this run (for shared-block sync)


def _manifest_text_mismatch(dat_path, dat_name):
    """Short reason if dat_path's TEXT differs from the pristine manifest (it was
    text-modified), else None. None also when there's no manifest/guard or the DAT
    isn't in the manifest (unknown DATs are not blocked)."""
    global _MANIFEST, _MANIFEST_LOADED
    if _pg is None:
        return None
    if not _MANIFEST_LOADED:
        _MANIFEST = _pg.load_manifest()
        _MANIFEST_LOADED = True
    if not _MANIFEST:
        return None
    man = _MANIFEST.get("dats", {})
    key = next((k for k in man if k.lower() == dat_name.lower()), None)
    if key is None:
        return None
    hh, _ = _pg.dat_text_hash(dat_path)
    ok = (_pg.text_hash_ok(dat_name, hh, man[key])
          if hasattr(_pg, "text_hash_ok")
          else hh == man[key].get("text_sha1"))
    return None if ok else "text fingerprint differs from pristine"


def _ensure_full_backup():
    """Copy the entire DAT/ folder to DAT_backup/ on first repack.
    Only runs once — if DAT_backup/ already exists, it's skipped."""
    if os.path.isdir(BACKUP_DIR):
        return
    print(f"\n--- First repack: backing up entire DAT/ folder ---")
    print(f"  Copying DAT/ -> DAT_backup/ ...")
    shutil.copytree(DAT_DIR, BACKUP_DIR)
    count = len([f for f in os.listdir(BACKUP_DIR) if f.endswith('.DAT')])
    print(f"  Done: {count} DAT files backed up.")
    print(f"  Original English source preserved in DAT_backup/\n")


def process_one(dump_path: str, quiet: bool):
    dump_name = os.path.basename(dump_path)
    dat_name  = os.path.splitext(dump_name)[0] + '.DAT'
    dat_out   = os.path.join(OUT_DIR, dat_name)
    os.makedirs(OUT_DIR, exist_ok=True)

    # TEXT source is ALWAYS the pristine backup — never the (possibly German) live
    # DAT or graphics folder. This makes text impossible to scramble: the .txt keys
    # (extracted from DAT_backup/) always line up with the source's pristine root
    # layout. German GRAPHICS are layered back on afterwards by the overlay below.
    src_path = os.path.join(BACKUP_DIR, dat_name)
    if not os.path.exists(src_path):
        src_path = dat_out          # first-repack fallback (no backup yet)

    if not os.path.exists(src_path):
        print(f"SKIP {dump_name}: no matching source DAT found")
        return False

    # Guard: the pristine backup MUST really be pristine, or text scrambles. Verify
    # this DAT's backup text fingerprint against the shipped manifest; refuse if it
    # was modified (and name it) instead of silently corrupting.
    bad = _manifest_text_mismatch(src_path, dat_name)
    if bad:
        print(f"  ERROR: text source {os.path.relpath(src_path, GAME_DIR)} is NOT "
              f"pristine ({bad}). Repacking it would scramble text — skipping. "
              f"Restore a clean {dat_name} (see pristine_text_manifest).")
        return False

    print(f"\n=== {dump_name} ===")
    print(f"  source: {os.path.relpath(src_path, GAME_DIR)} (text)")

    dump_strings = parse_dump(dump_path)
    if not dump_strings:
        print("  No entries parsed - skipping.")
        return False

    # Repack to a temp file next to the output, then replace. The per-entry
    # 0x20 space override for combat-Help menus (LEGACY_SPACE_ENTRIES) is applied
    # inside repack_dat, which knows each entry's index.
    tmp_path = dat_out + '.tmp'
    repack_dat(src_path, dump_strings, tmp_path, verbose=not quiet)

    # INIT.DAT carries FOUR font atlases as TOC type-5 entries:
    #   entry 8 = 16x16, 9 = 24x24 (dialog), 10 = 12x12 (small), 11 = 64x64.
    # Text repack rebuilds from DAT_backup/INIT.DAT (original fonts) and would
    # otherwise wipe any custom glyphs (JP atlas / umlauts) the user has drawn
    # into the live DAT/INIT.DAT. Preserve ALL font entries from the live file
    # by copying them into the freshly-repacked tmp file before replacing the
    # original. Translations live in the OTHER entries and are kept as the text
    # repacker wrote them, so this keeps graphics AND translation intact.
    # OVERLAY: bring German graphics over the US ones. Pull every non-text entry
    # from the graphics folder (--import-dir, e.g. graphics_to_edit/) into the
    # text-repacked tmp. Only graphics are taken; the graphics DAT's text is ignored,
    # so it's safe even if that DAT happens to carry German text.
    gfx_path = os.path.join(IMPORT_DIR, dat_name) if IMPORT_DIR else None
    merged_graphics = False
    if gfx_path and os.path.exists(gfx_path):
        try:
            if _merge_graphics_from_import(gfx_path, tmp_path):
                print(f"  overlaid German graphics from "
                      f"{os.path.relpath(gfx_path, GAME_DIR)}")
            merged_graphics = True   # graphics folder supplied non-text entries (incl fonts)
        except Exception as e:
            print(f"  WARNING: graphics overlay from {dat_name} failed: {e}")

    # INIT.DAT custom fonts fallback: if the graphics folder didn't supply INIT,
    # pull the font atlases from the live DAT/INIT.DAT so custom glyphs survive.
    live_init = os.path.join(DAT_DIR, dat_name)
    if (not merged_graphics) and dat_name.upper() == 'INIT.DAT' and os.path.exists(live_init):
        for fe in INIT_FONT_ENTRIES:
            try:
                changed = _preserve_init_dat_font_entry(live_init, tmp_path, fe)
                if changed:
                    print(f"  preserved custom font (entry {fe}) from live "
                          f"{dat_name}")
            except Exception as e:
                print(f"  WARNING: could not preserve font entry {fe} from "
                      f"live {dat_name}: {e}")

    # Dengeki Store: re-add the portrait art/CLUT entry (dropped by the text
    # rebuild-from-backup) before the atomic replace, so a normal repack yields a
    # complete store — no separate build step needed.
    _apply_dengeki_portrait(tmp_path, os.path.splitext(dat_name)[0])

    # Write the repacked version to the output folder (live DAT/ by default).
    os.replace(tmp_path, dat_out)
    print(f"  Written: {os.path.relpath(dat_out, GAME_DIR)}")
    _PROCESSED.add(os.path.splitext(dat_name)[0].upper())
    return True


def _merge_graphics_from_import(import_path: str, repacked_path: str) -> bool:
    """OVERLAY: replace the repacked DAT's NON-TEXT entries (f2 != 0 = textures /
    compressed / audio) with the versions from import_path (the German-graphics
    folder), keeping the repacked TEXT entries (f2 == 0) untouched. So text comes
    from the pristine backup rebuild and graphics come from your edited DAT — text
    can never be scrambled by a graphics source. Rebuilds TOC + bodies in one pass
    (loader reads purely by TOC src+size). Entry COUNT must match (graphics injection
    never adds TOC entries). Returns True if anything changed."""
    import struct as _s

    def _toc(b):
        n = _s.unpack_from('<I', b, 0)[0] // 16
        return [list(_s.unpack_from('<IIII', b, i * 16)) for i in range(n)]

    rep = open(repacked_path, 'rb').read()
    imp = open(import_path, 'rb').read()
    rep_toc, imp_toc = _toc(rep), _toc(imp)
    n = len(rep_toc)
    out_data, out_meta, changed = [], [], False
    for i in range(n):
        r_off, r_sz, r_f2, r_f3 = rep_toc[i]
        rep_bytes = rep[r_off:r_off + r_sz]
        if i < len(imp_toc) and imp_toc[i][2] != 0:          # import entry is non-text
            i_off, i_sz, i_f2, i_f3 = imp_toc[i]
            data, meta = imp[i_off:i_off + i_sz], (i_f2, i_f3)
            if data != rep_bytes:
                changed = True
        else:
            data, meta = rep_bytes, (r_f2, r_f3)
        out_data.append(data); out_meta.append(meta)
    if not changed:
        return False
    header = bytearray(n * 16)
    body = bytearray()
    off = n * 16
    for i in range(n):
        data = out_data[i]; f2, f3 = out_meta[i]
        _s.pack_into('<IIII', header, i * 16, off, len(data), f2, f3)
        body += data
        off += len(data)
    open(repacked_path, 'wb').write(bytes(header) + bytes(body))
    return True


# INIT.DAT font atlases (TOC type-5 entries) preserved across text repacks.
# 8=16x16, 9=24x24 dialog, 10=12x12 small, 11=64x64 title. See the
# project_font_atlases memory for the layout + the (cell<<8)|idx TOC encoding.
INIT_FONT_ENTRIES = (8, 9, 10, 11)


def _preserve_init_dat_font_entry(live_dat_path: str, repacked_path: str,
                                   font_entry: int = 9) -> bool:
    """Copy `font_entry` from live_dat_path into repacked_path, expanding the
    repacked DAT's TOC/body if the live font entry is a different size. All
    other entries remain as the text repacker wrote them (translations).

    Returns True if the repacked file was modified (live font differed),
    False if the live and repacked entry were already byte-identical (no-op).
    """
    import struct as _struct

    def _parse_toc(buf: bytes):
        first_off = _struct.unpack_from('<I', buf, 0)[0]
        num = first_off // 16
        entries = []
        for i in range(num):
            entries.append(list(_struct.unpack_from('<IIII', buf, i * 16)))
        return first_off, entries

    live = open(live_dat_path, 'rb').read()
    rep  = bytearray(open(repacked_path, 'rb').read())

    _, live_toc = _parse_toc(live)
    rep_first_off, rep_toc = _parse_toc(rep)

    # Bounds: the entry must exist in both TOCs.
    if font_entry >= len(live_toc) or font_entry >= len(rep_toc):
        return False

    live_off, live_sz, live_tp, live_idx = live_toc[font_entry]
    rep_off,  rep_sz,  rep_tp,  rep_idx  = rep_toc[font_entry]

    live_font = live[live_off:live_off + live_sz]

    if live_sz == rep_sz:
        # Same size — skip the rewrite entirely if bytes already match, else
        # do an in-place byte copy (no TOC edits).
        if bytes(rep[rep_off:rep_off + rep_sz]) == live_font:
            return False
        rep[rep_off:rep_off + rep_sz] = live_font
        open(repacked_path, 'wb').write(bytes(rep))
        return True

    # Size differs — rebuild the DAT with font entry resized in place
    # and subsequent entries' offsets shifted by delta.
    delta = live_sz - rep_sz
    new_toc = [list(e) for e in rep_toc]
    new_toc[font_entry][1] = live_sz
    for i, e in enumerate(new_toc):
        if i == font_entry:
            continue
        if e[0] > rep_off:
            e[0] += delta

    header = bytearray()
    for off, sz, tp, idx in new_toc:
        header += _struct.pack('<IIII', off, sz, tp, idx)
    body_pre  = rep[rep_first_off : rep_off]
    body_post = rep[rep_off + rep_sz :]
    new_data  = bytes(header) + bytes(body_pre) + live_font + bytes(body_post)
    open(repacked_path, 'wb').write(new_data)
    return True


# ---------------------------------------------------------------------------
# Shared block sync (byte-identical entries across multiple DAT files)
# ---------------------------------------------------------------------------
#
# Multiple INDEPENDENT shared groups exist. Each group is a dict
# `{basename: (entry_idx, block_offset, block_size)}` of files whose
# named entry is byte-identical to every other entry in the same
# group. After repacking one member, the bytes are copied to every
# OTHER member of the SAME group (and only that group).
#
# `block_offset` in the tuple is documentation only — sync uses the
# live TOC's offset at runtime. `block_size` similarly comes from the
# live TOC. Only `entry_idx` is load-bearing.
SHARED_BLOCK_GROUPS = [
    # Group 0: item-description entries (f3=0x010000, size=0x5572).
    {
        'AB000_00': (1,  0x03FA1C, 0x5572),
        'CAMP':     (5,  0x04F26C, 0x5572),
        'COMMU03':  (4,  0x047DA2, 0x5572),
        'MASTER':   (4,  0x04C10F, 0x5572),
        'MSHOP':    (5,  0x043BAB, 0x5572),
        'SGAMEN':   (6,  0x04DEA0, 0x5572),
        'SHOP':     (5,  0x04C747, 0x5572),
    },
    # Group 1: Coursair-village NPC dialog (entry 7, implicit-pool
    # format — see project_implicit_pool_start memory).
    # Detected via scan_shared_blocks.py hash 4397bfe20315a591.
    {
        'AREAM005': (7,  0x4895A,  0x1808),
        'AREAS010': (7,  0x85203,  0x1808),
        'AREAS012': (7,  0x6C100,  0x1808),
    },
]

# Backward-compat alias — a flattened view used by older callers.
# Note: the FIRST occurrence of a basename wins (groups are processed
# in order). All current entries are uniquely keyed, so the flatten is
# lossless today.
SHARED_BLOCKS = {b: t for grp in SHARED_BLOCK_GROUPS for b, t in grp.items()}


def _find_block_group(source_base: str):
    """Return the dict-group that contains source_base, or None."""
    for grp in SHARED_BLOCK_GROUPS:
        if source_base in grp:
            return grp
    return None


# Locations the user unlocked in the editor for independent translation.
# Written by localization_editor.py as a flat list of "base|entrynum:offset".
# We only need the (base, entry_idx) granularity here: the DAT-block sync
# copies whole entries, so a single unlocked string protects its entire
# entry from being overwritten by a sibling's bytes.
_DUP_UNLOCK_PATH = os.path.join(GAME_DIR, "translations", "_duplicate_unlocks.json")


def _load_unlocked_block_entries():
    """Return a set of (base, entry_idx:int) that hold >=1 unlocked location."""
    out = set()
    if not os.path.exists(_DUP_UNLOCK_PATH):
        return out
    try:
        with open(_DUP_UNLOCK_PATH, encoding="utf-8") as f:
            data = json.load(f)
    except Exception:
        return out
    for item in data:
        base, sep, key = str(item).partition("|")
        if not sep or not key:
            continue
        ent = key.split(":", 1)[0]
        try:
            out.add((base, int(ent)))
        except ValueError:
            continue
    return out


def sync_shared_blocks(source_base: str):
    """After repacking source_base, copy its shared block to every
    OTHER member of the same shared group (and only that group)."""
    grp = _find_block_group(source_base)
    if grp is None:
        return

    src_idx, _, _ = grp[source_base]

    # Honor editor duplicate-unlocks: a file/entry the user marked as holding
    # an independent translation must neither push its divergent bytes onto
    # siblings nor be overwritten by a sibling's bytes.
    unlocked = _load_unlocked_block_entries()
    if (source_base, src_idx) in unlocked:
        print(f"  Skipped shared-block sync from {source_base} entry {src_idx}: "
              f"contains an unlocked (independent) translation.")
        return

    src_path = os.path.join(OUT_DIR, source_base + '.DAT')
    if not os.path.exists(src_path):              # not written to out dir? use live
        src_path = os.path.join(DAT_DIR, source_base + '.DAT')

    with open(src_path, 'rb') as f:
        src_data = f.read()

    # Read the repacked block from source — get actual offset from TOC
    toc_base = 16 + src_idx * 16
    actual_off = struct.unpack_from('<I', src_data, toc_base)[0]
    actual_size = struct.unpack_from('<I', src_data, toc_base + 4)[0]
    block = src_data[actual_off: actual_off + actual_size]

    synced = 0
    for target_base, (t_idx, _, _) in grp.items():
        if target_base == source_base:
            continue
        if (target_base, t_idx) in unlocked:
            print(f"  Skipped {target_base} entry {t_idx}: holds an unlocked "
                  f"(independent) translation — not overwriting.")
            continue
        t_path = os.path.join(OUT_DIR, target_base + '.DAT')
        processed = target_base.upper() in _PROCESSED
        # Seed: if this sibling was repacked THIS run, its output already has German
        # text + graphics — patch the shared block onto it. Otherwise start from the
        # PRISTINE backup (so we don't inherit stale/foreign bytes), patch the block,
        # then overlay its German graphics below.
        if processed and os.path.exists(t_path):
            seed = t_path
        elif os.path.exists(os.path.join(BACKUP_DIR, target_base + '.DAT')):
            seed = os.path.join(BACKUP_DIR, target_base + '.DAT')
        elif os.path.exists(os.path.join(DAT_DIR, target_base + '.DAT')):
            seed = os.path.join(DAT_DIR, target_base + '.DAT')
        else:
            continue

        with open(seed, 'rb') as f:
            t_data = bytearray(f.read())

        # Patch the block in-place (same size) or update TOC if different size
        t_toc_base = 16 + t_idx * 16
        t_old_off = struct.unpack_from('<I', t_data, t_toc_base)[0]
        t_old_size = struct.unpack_from('<I', t_data, t_toc_base + 4)[0]

        if actual_size <= t_old_size:
            t_data[t_old_off: t_old_off + actual_size] = block
            t_data[t_old_off + actual_size: t_old_off + t_old_size] = bytes(t_old_size - actual_size)
            struct.pack_into('<I', t_data, t_toc_base + 4, actual_size)
        else:
            new_off = len(t_data)
            t_data.extend(block)
            struct.pack_into('<I', t_data, t_toc_base, new_off)
            struct.pack_into('<I', t_data, t_toc_base + 4, actual_size)

        os.makedirs(OUT_DIR, exist_ok=True)
        with open(t_path, 'wb') as f:
            f.write(t_data)
        # Sibling seeded from pristine backup: layer its German graphics back on.
        if not processed and IMPORT_DIR:
            gfx = os.path.join(IMPORT_DIR, target_base + '.DAT')
            if os.path.exists(gfx):
                try:
                    _merge_graphics_from_import(gfx, t_path)
                except Exception as e:
                    print(f"  WARNING: graphics overlay (sync) {target_base} failed: {e}")
        print(f"  Synced shared block -> {os.path.relpath(t_path, GAME_DIR)}")
        synced += 1

    if synced:
        print(f"  Shared block synced to {synced} other DAT files.")


def main():
    ap = argparse.ArgumentParser(description='BoF4 text repacker')
    ap.add_argument('files', nargs='*',
                    help='text_insert/*.txt file(s) to repack. '
                         'If omitted, all .txt files in text_insert/ are processed.')
    ap.add_argument('--quiet', '-q', action='store_true',
                    help='Suppress per-string output')
    ap.add_argument('--sync-shared', action='store_true',
                    help='After repacking, copy shared blocks to sibling DATs')
    ap.add_argument('--out-dir', default=None,
                    help='Write repacked DATs here instead of overwriting the live '
                         'DAT/ folder. Source is still DAT_backup/. Siblings touched '
                         'by --sync-shared are seeded from DAT_backup/ if absent.')
    ap.add_argument('--import-dir', default=None,
                    help='German-GRAPHICS folder (e.g. graphics_to_edit/). Its non-text '
                         '(texture/font) entries are OVERLAID onto the text output so '
                         'German graphics survive. Text is rebuilt from DAT_backup/ and '
                         'never read from here. Off if unset.')
    ap.add_argument('--legacy-space', action='store_true',
                    help='Emit ASCII 0x20 for space (old behavior, '
                         'menu spaces render ~17 px). Same as --space-byte none.')
    ap.add_argument('--space-byte', default=None, metavar='VALUE',
                    help='Byte to emit for spaces so vwf_config controls their '
                         'width. Hex (0x97) or int; or none/off/legacy/0x20 to '
                         'emit plain ASCII 0x20. Default 0x97. The chosen byte '
                         'needs a transparent atlas slot + a matching '
                         'vwf_config "0xNN = nbsp : 0 : N" line.')
    args = ap.parse_args()

    global OUT_DIR, IMPORT_DIR
    if args.out_dir:
        OUT_DIR = os.path.abspath(args.out_dir)
        os.makedirs(OUT_DIR, exist_ok=True)
        print(f"[repack_text] output dir: {OUT_DIR}  (live DAT/ left untouched; "
              f"text source still DAT_backup/)")
    if args.import_dir:
        IMPORT_DIR = os.path.abspath(args.import_dir)
        print(f"[repack_text] graphics overlay dir: {IMPORT_DIR}  (non-text entries "
              f"overlaid onto the text output; text stays from DAT_backup/)")

    global SPACE_REPLACEMENT_BYTE
    if args.space_byte is not None:
        SPACE_REPLACEMENT_BYTE = _parse_space_byte(args.space_byte)
    if args.legacy_space:
        SPACE_REPLACEMENT_BYTE = None
    if SPACE_REPLACEMENT_BYTE is None:
        print("[repack_text] space substitution OFF: emitting ASCII 0x20 for ' '")
    else:
        print(f"[repack_text] space substitution: ' ' -> 0x{SPACE_REPLACEMENT_BYTE:02X} "
              f"(needs vwf_config '0x{SPACE_REPLACEMENT_BYTE:02X} = nbsp : 0 : N' "
              "for width + transparent atlas slot; --space-byte none to disable)")

    if args.files:
        dumps = [os.path.abspath(f) for f in args.files]
    else:
        dumps = sorted(glob.glob(os.path.join(os.getcwd(), '*.txt')))
        if not dumps:
            sys.exit(f"ERROR: no .txt files found in {os.getcwd()}")

    # Full backup of DAT/ on first ever repack
    _ensure_full_backup()

    repacked = []
    for d in dumps:
        if process_one(d, args.quiet):
            repacked.append(d)

    if args.sync_shared and repacked:
        print("\n--- Syncing shared blocks ---")
        for d in repacked:
            base = os.path.splitext(os.path.basename(d))[0]
            sync_shared_blocks(base)

    print(f"\nFinished: {len(repacked)}/{len(dumps)} file(s) repacked.")

    if repacked:
        print("\n--- Re-extracting text_dump to stay in sync ---")
        extract_path = os.path.join(GAME_DIR, "extract_text.py")
        if os.path.exists(extract_path):
            import subprocess
            subprocess.run([sys.executable, extract_path], cwd=GAME_DIR)
            print("text_dump refreshed.")
        else:
            print(f"  extract_text.py not found at {extract_path} — re-run manually.")


def _expand_globs_in_argv():
    """PowerShell/cmd don't expand `*` like bash does. Expand any
    glob patterns in sys.argv before argparse sees them."""
    import glob as _g
    new_argv = [sys.argv[0]]
    for a in sys.argv[1:]:
        if any(ch in a for ch in '*?[]'):
            matches = sorted(_g.glob(a))
            if matches:
                new_argv.extend(matches)
            else:
                new_argv.append(a)
        else:
            new_argv.append(a)
    sys.argv = new_argv


if __name__ == '__main__':
    _expand_globs_in_argv()
    main()
