"""
BoF4 text extractor.
Finds the string-table entry in each DAT file and extracts all strings.

Structure discovered:
  Entry with offset table: first uint16 = total offset-table size (bytes).
  num_strings = offset_table_size / 2
  Strings start at offset_table_size within the entry.
  Strings are null-terminated ASCII.

Control codes (from bof4text.txt hacking guide):
  0-param (standalone):
    0x00 = end of string
    0x01 = newline
    0x02 = end of box, continue chain
    0x03 = active protagonist name
    0x06 = end colored text
    0x08 = unknown (0-param based on observed data)
    0x0B = pause
    0x0D = start animated text
  1-param (code + 1 byte):
    0x04 xx = character name (0x00-0x06 = party members)
    0x05 xx = start colored text
    0x07 xx = memory string placeholder
    0x0C xx = textbox style/position
    0x10 xx = typing on/off
    0x11 xx = unknown
    0x12 xx = unknown
    0x13 xx = unknown
    0x14 xx = option box action (usually followed by 0x0C xx)
    0x15 xx = unknown
    0x16 xx = timed text box duration
    0x19 xx = unknown
    0x1C xx = zenny counter position
    0x1E xx = unknown
  2-param (code + 2 bytes):
    0x09 xx xx = item name (item ID, category)
    0x17 xx xx = portrait (portrait ID, palette/placement)
    0x18 xx xx = textbox substitution (index, offset)
  Special 3-byte (code + 0x0F + effect):
    0x0E 0x0F xx = end animated text (effect code 0x00-0x0A)
"""
import struct
import os
import sys
import glob
import re

import uncensor_patch   # baked NA-uncensor dialogue restore (pre-pass)

# Prefer DAT_backup/ (untouched originals) over DAT/ (may contain repacked German text).
# This ensures text_dump always has the original English source strings.
_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
_BACKUP_DIR = os.path.join(_SCRIPT_DIR, "DAT_backup")
DAT_DIR = _BACKUP_DIR if os.path.isdir(_BACKUP_DIR) else os.path.join(_SCRIPT_DIR, "DAT")
OUT_DIR = os.path.join(os.path.dirname(__file__), "text_dump")
os.makedirs(OUT_DIR, exist_ok=True)


def read_uint32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def read_uint16(data, offset):
    return struct.unpack_from("<H", data, offset)[0]


# Control codes with 0 extra parameter bytes
_CTRL_0PARAM = frozenset([0x02, 0x03, 0x06, 0x08, 0x0B, 0x0D])
# Control codes with 2 extra parameter bytes
_CTRL_2PARAM = frozenset([0x09, 0x17, 0x18])
# 0x14 is variable-length: 2 extra bytes when param >= 0x80 (speaker code),
# 1 extra byte otherwise. Handled as a special case in decode_string.


# ── Japanese decode support ──────────────────────────────────────────────────
# The JP build encodes text with a custom single-byte-kana / two-byte-kanji
# scheme (NOT Shift-JIS). Character tables + the code ranges below are from
# glitch-in-the-herring/bof4-text-extractor (MIT); see jp_tables/LICENSE.
#   hiragana : byte 0x5E..0xAD  -> hiragana[byte-0x5E]   (80 glyphs)
#   katakana : byte 0xAE..0xFE  -> katakana[byte-0xAE]   (81 glyphs)
#   kanji    : lead 0x12/0x13, code=(lead<<8)|next, -> kanji[code-0x1200]
#   digits/upper ASCII pass through; 0x01..0x20 are engine control codes;
#   0x00 terminates.
JP_MODE = False
_JP_HIRA = None
_JP_KATA = None
_JP_KANJI = None
_JP_KJSTART = 0x1200


def _load_jp_tables():
    """Load the kana/kanji glyph tables from jp_tables/ (one entry per line).
    Cached. Raises a clear error if the tables are missing."""
    global _JP_HIRA, _JP_KATA, _JP_KANJI
    if _JP_HIRA is not None:
        return
    # When frozen into the editor .exe, jp_tables/ is bundled into _MEIPASS.
    _base = getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))
    tdir = os.path.join(_base, "jp_tables")

    def _load(name):
        with open(os.path.join(tdir, name), "r", encoding="utf-8") as f:
            return f.read().replace("\r", "").rstrip("\n").split("\n")
    _JP_HIRA = _load("hiragana.src")
    _JP_KATA = _load("katakana.src")
    _JP_KANJI = _load("kanji.src")


def _decode_string_jp(raw, start):
    """Decode a JP string. Control codes match the EN engine (language-neutral);
    only the character bytes use the kana/kanji tables."""
    _load_jp_tables()
    parts = []
    i = start
    n = len(raw)
    while i < n:
        b = raw[i]
        if b == 0x00 or b == 0x16:      # terminators (glitch: 0x00/0x16)
            break
        elif b == 0x12 or b == 0x13:    # kanji lead (must precede control test)
            if i + 1 < n:
                code = (b << 8) | raw[i + 1]
                idx = code - _JP_KJSTART
                if 0 <= idx < len(_JP_KANJI):
                    parts.append(_JP_KANJI[idx])
                else:
                    parts.append(f"{{K{code:04X}}}")
                i += 2
            else:
                parts.append(f"[{b:02X}]")
                i += 1
        elif b == 0x01:
            parts.append("\n"); i += 1
        elif b in _CTRL_0PARAM:
            parts.append(f"[{b:02X}]"); i += 1
        elif b == 0x0E:
            if i + 2 < n and raw[i + 1] == 0x0F:
                parts.append(f"[0E:0F:{raw[i+2]:02X}]"); i += 3
            elif i + 1 < n:
                parts.append(f"[0E:{raw[i+1]:02X}]"); i += 2
            else:
                parts.append("[0E]"); i += 1
        elif b in _CTRL_2PARAM:
            if i + 2 < n:
                parts.append(f"[{b:02X}:{raw[i+1]:02X}:{raw[i+2]:02X}]"); i += 3
            elif i + 1 < n:
                parts.append(f"[{b:02X}:{raw[i+1]:02X}]"); i += 2
            else:
                parts.append(f"[{b:02X}]"); i += 1
        elif b == 0x14:
            if i + 1 < n:
                p1 = raw[i + 1]
                if p1 >= 0x80 and i + 2 < n:
                    parts.append(f"[14:{p1:02X}:{raw[i+2]:02X}]"); i += 3
                else:
                    parts.append(f"[14:{p1:02X}]"); i += 2
            else:
                parts.append("[14]"); i += 1
        elif b < 0x20:
            if i + 1 < n:
                parts.append(f"[{b:02X}:{raw[i+1]:02X}]"); i += 2
            else:
                parts.append(f"[{b:02X}]"); i += 1
        elif 0x5E <= b <= 0xAD:         # hiragana
            idx = b - 0x5E
            parts.append(_JP_HIRA[idx] if idx < len(_JP_HIRA) else f"<{b:02X}>")
            i += 1
        elif 0xAE <= b <= 0xFE:         # katakana
            idx = b - 0xAE
            parts.append(_JP_KATA[idx] if idx < len(_JP_KATA) else f"<{b:02X}>")
            i += 1
        elif 0x20 <= b < 0x5E:          # digits / uppercase / punctuation (ASCII)
            parts.append(chr(b)); i += 1
        else:                            # 0xFF etc.
            parts.append(f"<{b:02X}>"); i += 1
    return "".join(parts), i + 1


def _looks_like_jp(sample):
    """Heuristic: does this byte sample look like a JP text pool? Counts bytes in
    the plausible JP-text set (kana/kanji-lead/control/digit/upper/null) and
    requires a strong majority PLUS some actual kana present."""
    if not sample:
        return False
    plausible = 0
    kana = 0
    for b in sample:
        if 0x5E <= b <= 0xFE:
            plausible += 1; kana += 1
        elif b == 0 or 0x01 <= b <= 0x20 or 0x30 <= b <= 0x39 \
                or 0x41 <= b <= 0x5A or b in (0x12, 0x13):
            plausible += 1
    return (plausible >= len(sample) * 0.85) and (kana >= len(sample) * 0.12)


def decode_string(raw, start):
    """Decode a null-terminated string with control codes."""
    if JP_MODE:
        return _decode_string_jp(raw, start)
    parts = []
    i = start
    while i < len(raw):
        b = raw[i]
        if b == 0x00:
            break
        elif b == 0x01:
            parts.append("\n")
            i += 1
        elif b in _CTRL_0PARAM:
            parts.append(f"[{b:02X}]")
            i += 1
        elif b == 0x0E:
            # Special: 0x0E 0x0F xx (end animated text)
            if i + 2 < len(raw) and raw[i + 1] == 0x0F:
                parts.append(f"[0E:0F:{raw[i+2]:02X}]")
                i += 3
            elif i + 1 < len(raw):
                parts.append(f"[0E:{raw[i+1]:02X}]")
                i += 2
            else:
                parts.append("[0E]")
                i += 1
        elif b in _CTRL_2PARAM:
            if i + 2 < len(raw):
                parts.append(f"[{b:02X}:{raw[i+1]:02X}:{raw[i+2]:02X}]")
                i += 3
            elif i + 1 < len(raw):
                parts.append(f"[{b:02X}:{raw[i+1]:02X}]")
                i += 2
            else:
                parts.append(f"[{b:02X}]")
                i += 1
        elif b == 0x14:
            # Variable-length: param >= 0x80 means 2 extra bytes (speaker),
            # param < 0x80 means 1 extra byte.
            if i + 1 < len(raw):
                p1 = raw[i + 1]
                if p1 >= 0x80 and i + 2 < len(raw):
                    parts.append(f"[14:{p1:02X}:{raw[i+2]:02X}]")
                    i += 3
                else:
                    parts.append(f"[14:{p1:02X}]")
                    i += 2
            else:
                parts.append("[14]")
                i += 1
        elif b < 0x20:
            # 1-param control code
            if i + 1 < len(raw):
                parts.append(f"[{b:02X}:{raw[i+1]:02X}]")
                i += 2
            else:
                parts.append(f"[{b:02X}]")
                i += 1
        else:
            parts.append(chr(b))
            i += 1
    return "".join(parts), i + 1  # return text and position after null terminator


def find_text_entry(data, toc_size):
    """
    Find entries that contain ASCII string tables.

    Two-tier detection:
      - f3 == 0 (dialog text): liberal check — count printable + nulls/control bytes
      - f3 != 0 (item names, menus, descriptions): strict check — 50%+ printable bytes
    """
    if toc_size < 16:
        return None
    num_entries = (toc_size - 16) // 16
    candidates = []
    for i in range(num_entries):
        base = 16 + i * 16
        if base + 16 > toc_size:
            break
        e_off = read_uint32(data, base)
        e_size = read_uint32(data, base + 4)
        e_f2 = read_uint32(data, base + 8)
        e_f3 = read_uint32(data, base + 12)

        if e_off + e_size > len(data) or e_size < 16:
            continue

        # f2 must be 0 — non-zero means compressed, GPU, or audio data
        if e_f2 != 0x00000000:
            continue

        first_val = read_uint16(data, e_off)
        if first_val < 4 or first_val >= e_size:
            continue
        if first_val % 2 != 0:
            # IMPLICIT-POOL_START SPECIAL CASE: a few NPC dialog entries
            # (AREAM005 e[7], AREAS010 e[7], AREAS012 e[7] — Coursair
            # village) have no explicit pool_start word. byte 0..0x1FF
            # IS the offset table (256 slots), pool starts at 0x200.
            # The "first word" is actually slot[0], which equals 0x0201
            # (pointing to the first character of the first string,
            # 1 byte past the leading [06] color-reset at 0x200).
            #
            # Detection: first_val == 0x0201 specifically AND slot[0..7]
            # all point into the implicit pool region [0x200, e_size).
            if first_val == 0x0201 and e_size > 0x220:
                ok = all(
                    0x200 <= read_uint16(data, e_off + s * 2) < e_size
                    for s in range(8))
                # And byte at 0x200 should look like text start (control
                # code or printable ASCII).
                if ok and (data[e_off + 0x200] < 0x20 or
                           0x20 <= data[e_off + 0x200] < 0x80):
                    candidates.append((i, e_off, e_size, 0x200))
            continue

        # Multi-level wrapper short-circuit: when pool_start <= 16 the flat
        # checks below would test the inner offset table as if it were the
        # string pool and reject (high-byte-rich uint16s). Detect wrappers
        # explicitly and accept — extract_strings_from_entry walks each
        # inner sub-block.
        if first_val <= 16 and _wrapper_sub_blocks(data, e_off, e_size):
            candidates.append((i, e_off, e_size, first_val))
            continue

        if e_f3 == 0x00000000:
            # Dialog text: check that pool region is mostly ASCII + control codes.
            num_strings = first_val // 2
            pool_bytes = e_size - first_val
            # Reject if average bytes per string is < 5 — indicates binary data
            # being misidentified (real text averages 10+ bytes per string)
            # Count UNIQUE pointer values — many entries are duplicates
            unique_ptrs = len(set(read_uint16(data, e_off + j*2)
                                  for j in range(num_strings)
                                  if e_off + j*2 + 1 < len(data)))
            if unique_ptrs > 0 and pool_bytes / max(unique_ptrs, 1) < 4:
                continue
            sample = data[e_off + first_val: e_off + first_val + 128]
            if JP_MODE:
                if _looks_like_jp(sample):
                    candidates.append((i, e_off, e_size, first_val))
                continue
            ascii_count = sum(1 for b in sample if 0x20 <= b < 0x7F or b in (0x00, 0x01, 0x0A, 0x0D))
            high_count = sum(1 for b in sample if b >= 0x80)
            if ascii_count >= len(sample) // 3 and high_count < len(sample) // 4:
                candidates.append((i, e_off, e_size, first_val))
        else:
            # Non-dialog (item names, menus, etc.): require 50%+ printable ASCII
            # Also reject entries with very short average string length (binary data)
            num_strings = first_val // 2
            pool_bytes = e_size - first_val
            # Count UNIQUE pointer values — many entries are duplicates
            unique_ptrs = len(set(read_uint16(data, e_off + j*2)
                                  for j in range(num_strings)
                                  if e_off + j*2 + 1 < len(data)))
            if unique_ptrs > 0 and pool_bytes / max(unique_ptrs, 1) < 4:
                continue
            found = False
            for offset in (0, 256, 512):
                probe = e_off + first_val + offset
                if probe + 64 > e_off + e_size:
                    break
                sample = data[probe: probe + 64]
                if JP_MODE:
                    if _looks_like_jp(sample):
                        found = True
                        break
                    continue
                printable = sum(1 for b in sample if 0x20 <= b < 0x7F)
                high = sum(1 for b in sample if b >= 0x80)
                if printable >= 32 and high < len(sample) // 4:
                    found = True
                    break
            if found:
                candidates.append((i, e_off, e_size, first_val))

    return candidates


def scan_pool_roots(entry, pool_start, e_size):
    """Sequential scan of the string pool; returns list of root offsets in order."""
    roots = []
    pos = pool_start
    while pos < e_size:
        roots.append(pos)
        while pos < e_size:
            b = entry[pos]
            if b == 0x00:
                pos += 1
                break
            elif b == 0x01:
                pos += 1
            elif b in _CTRL_0PARAM:
                pos += 1
            elif b == 0x0E:
                if pos + 2 < e_size and entry[pos + 1] == 0x0F:
                    pos += 3
                else:
                    pos += 2
            elif b in _CTRL_2PARAM:
                pos += 3
            elif b == 0x14:
                # Variable-length: param >= 0x80 → 3 bytes (speaker), else 2 bytes.
                # Without this, the third byte of [14:80:00] gets misread as a
                # null terminator and a fake root is created mid-string.
                if pos + 1 < e_size and entry[pos + 1] >= 0x80:
                    pos += 3
                else:
                    pos += 2
            elif b < 0x20:
                pos += 2
            else:
                pos += 1
    return roots


def _flat_scan_strings(entry, min_len=3):
    """Flat-scan for null-terminated ASCII strings in an entry.
    Used for multi-level entries (like INIT.DAT item names) where
    the simple offset-table approach doesn't work."""
    strings = {}
    pos = 0
    while pos < len(entry):
        # Find next null
        end = entry.find(b'\x00', pos)
        if end == -1:
            break
        seg = entry[pos:end]
        if len(seg) >= min_len:
            printable = sum(1 for b in seg if 0x20 <= b < 0x7F)
            if printable >= len(seg) * 0.6:
                text, _ = decode_string(entry, pos)
                if text and len(text) >= min_len:
                    strings[pos] = text
        pos = end + 1
    return strings


# ── Multi-level wrapper helpers ──────────────────────────────────────────────
# Some DAT entries (e.g. AB000_00.DAT entry 0, INIT.DAT entry 6) wrap one or
# more flat-style text sub-entries behind a small outer pointer header
# (pool_start <= 32, alternating non-zero/zero u16 pointers). The flat-table
# heuristic in find_text_entry rejects them because the "pool" beginning at
# pool_start is actually the first inner offset table (high-byte-rich uint16s,
# fails the ASCII-density check). We detect them up-front and walk each
# sub-block's flat structure individually.
#
# Upper bound 32: INIT.DAT entry 6 uses a 12-pointer header (pool_start=0x18).
# A widened sweep across all DATs in DAT Backup/ found only that one new
# entry — no false positives elsewhere — so the wider range is safe.
def _wrapper_sub_blocks(data, e_off, e_size):
    """Return list of (sub_off_in_entry, sub_size) inner blocks if this
    entry looks like a multi-level wrapper, else None.

    Wrapper signature: pool_start in [4..32], even, with non-zero outer
    pointers strictly increasing, and each inner block starts with a
    plausible flat-entry offset table whose pool is ASCII-rich."""
    if e_size < 16:
        return None
    pool_start = read_uint16(data, e_off)
    if pool_start < 4 or pool_start > 32 or pool_start % 2 != 0:
        return None
    n_outer = pool_start // 2
    raw_ptrs = [read_uint16(data, e_off + i * 2) for i in range(n_outer)]
    nz = [p for p in raw_ptrs if p != 0]
    if not nz or nz != sorted(nz):
        return None
    if any(p < pool_start or p >= e_size for p in nz):
        return None
    bounds = nz + [e_size]
    blocks = []
    for i in range(len(nz)):
        sub_off  = bounds[i]
        sub_size = bounds[i + 1] - sub_off
        if sub_size < 16:
            return None
        # Inner block must itself be a valid flat-entry: pool_start
        # plausible, and pool ASCII-rich.
        sub_pool = read_uint16(data, e_off + sub_off)
        if sub_pool < 4 or sub_pool >= sub_size or sub_pool % 2 != 0:
            return None
        sample_start = e_off + sub_off + sub_pool
        sample_end   = min(sample_start + 128, e_off + sub_off + sub_size)
        if sample_end - sample_start < 32:
            return None
        sample = data[sample_start:sample_end]
        ascii_n = sum(1 for b in sample
                      if 0x20 <= b < 0x7F or b in (0x00, 0x01, 0x02, 0x06, 0x0B, 0x0D))
        high_n  = sum(1 for b in sample if b >= 0x80)
        if ascii_n < len(sample) // 3 or high_n >= len(sample) // 4:
            return None
        blocks.append((sub_off, sub_size))
    return blocks


def extract_strings_from_entry(data, e_off, e_size, pool_start):
    """
    Extract all strings from a string-pool entry.

    Returns (offsets, strings, hidden_count, suffix_of) where:
      offsets      – raw uint16 offset-table array
      strings      – {pool_offset: decoded_text} for ALL roots + suffixes
      hidden_count – number of sequential roots not in offset table
      suffix_of    – {pool_offset: parent_root_offset} for suffix (mid-string) offsets.
                     Suffix offsets point INTO the middle of a root string; only
                     the root needs translating — the repacker recalculates suffix
                     positions automatically.

    Multi-level wrappers (e.g. AB000_00 entry 0) are walked: each inner
    sub-block is extracted as a flat entry, and offsets are translated to
    be entry-relative (matches the existing rebuild_multilevel_entry
    contract in repack_text.py).
    """
    entry = data[e_off: e_off + e_size]
    num_offsets = pool_start // 2

    # Multi-level wrapper: walk each inner sub-block.
    sub_blocks = _wrapper_sub_blocks(data, e_off, e_size)
    if sub_blocks:
        all_offsets = []
        all_strings = {}
        all_suffix_of = {}
        all_hidden = 0
        for sub_off, sub_size in sub_blocks:
            sub_pool = read_uint16(data, e_off + sub_off)
            sub_offs, sub_strs, sub_hid, sub_suf = extract_strings_from_entry(
                data, e_off + sub_off, sub_size, sub_pool)
            for o in sub_offs:
                all_offsets.append(o + sub_off)
            for o, s in sub_strs.items():
                all_strings[o + sub_off] = s
            for o, parent in sub_suf.items():
                all_suffix_of[o + sub_off] = parent + sub_off
            all_hidden += sub_hid
        return all_offsets, all_strings, all_hidden, all_suffix_of

    # Multi-level entries that aren't text wrappers (rare — uses flat-scan
    # fallback).
    if num_offsets <= 16:
        strings = _flat_scan_strings(entry)
        return [], strings, 0, {}

    offsets = [read_uint16(entry, i * 2) for i in range(num_offsets)]
    offset_set = set(off for off in offsets if off < e_size)

    roots = scan_pool_roots(entry, pool_start, e_size)
    root_set = set(roots)

    strings   = {}
    suffix_of = {}   # offset -> parent root offset

    # Classify offset-table entries as root or suffix
    for off in sorted(offset_set):
        text, _ = decode_string(entry, off)
        if not text:
            continue
        strings[off] = text
        if off not in root_set:
            # Find the last sequential root before this offset
            parent = None
            for r in roots:
                if r <= off:
                    parent = r
                else:
                    break
            if parent is not None and parent != off:
                suffix_of[off] = parent

    # Hidden roots: sequential roots NOT in the offset table
    hidden_count = 0
    for root in roots:
        if root in offset_set or root in strings:
            continue
        text, _ = decode_string(entry, root)
        if text:
            strings[root] = text
            hidden_count += 1

    return offsets, strings, hidden_count, suffix_of


_WORD_RE = re.compile(r'[a-zA-Z]{3,}')


_COMMON_WORDS = frozenset([
    'the', 'and', 'you', 'for', 'that', 'with', 'this', 'have', 'from',
    'not', 'but', 'are', 'all', 'your', 'what', 'when', 'one', 'our',
    'out', 'can', 'will', 'about', 'who', 'get', 'she', 'him', 'her',
    'his', 'they', 'them', 'their', 'there', 'been', 'here', 'just',
    'like', 'know', 'want', 'into', 'some', 'then', 'now', 'how',
    'say', 'said', 'tell', 'take', 'come', 'make', 'back', 'look',
    'let', 'where', 'why', 'were', 'was', 'has', 'had', 'would',
    'could', 'should', 'more', 'much', 'very', 'too', 'only', 'even',
    'well', 'good', 'going', 'think', 'got', 'yes', 'see', 'before',
])


_CTRL_TOKEN_PAT = re.compile(r"\[[0-9A-Fa-f:]+\]")
_LONG_WORD_RE   = re.compile(r"[A-Za-z]{4,}")


def _entry_has_readable_text(strings):
    """Post-extraction check: strings must contain real English words,
    not just accidental letter sequences from binary data.

    Two passes:
      1. Common-word check (function words like 'the', 'you', 'and') —
         catches dialog/description text reliably.
      2. Label-table fallback for entries that have no function words
         but ARE real text (place names, menu items, button labels):
         accept if there are 10+ strings AND >=70% of them contain at
         least 3 alphabetic characters after stripping control codes.

    Without (2), entries like INIT.DAT entry 5 (place names — "South
    Desert", "Ludia Region", "Cancel", ":Camp", etc.) are silently
    dropped because they contain no function words. The label-table
    threshold (10 strings + 70% with 3+ letters) cleanly separates
    real label tables (~90-99% ratio) from binary command streams
    (~13-23% ratio in AREAD023/AREAD125 entry 3, which only ever look
    like text by accident)."""
    hits = 0
    for text in strings.values():
        lowered = text.lower()
        for m in re.finditer(r"[a-z']+", lowered):
            if m.group().strip("'") in _COMMON_WORDS:
                hits += 1
                if hits >= 3:
                    return True

    # Label-table fallback
    if len(strings) >= 10:
        labelish = 0
        for text in strings.values():
            stripped = _CTRL_TOKEN_PAT.sub("", text).replace("\n", "").strip()
            if sum(1 for c in stripped if c.isalpha()) >= 3:
                labelish += 1
        if labelish >= len(strings) * 0.7:
            return True

    # Short-entry English-text fallback. Signs and menu prompts with
    # few strings and no function words slip past Pass 1+2 (e.g.
    # AREAD049 "Wildlife Preserve / Endangered Species / Ahm Snake
    # Habitat / Patrol in effect", AREAD095/096/098/099 elevator
    # "Which floor do you want to go to?"). Accept when the decoded
    # content has at least 2 alphabetic words of >=4 letters AND on
    # average >=1 such word per string. The per-string average is
    # what separates real text from binary command streams (the
    # AREAB*/AREAD* entry-3 pattern with 1000s of control-byte
    # strings and only sporadic accidental letter sequences).
    total_words = 0
    for text in strings.values():
        stripped = _CTRL_TOKEN_PAT.sub("", text)
        total_words += len(_LONG_WORD_RE.findall(stripped))
    if total_words >= 2 and total_words / max(len(strings), 1) >= 1.0:
        return True

    return False


# File prefixes that contain binary data (animations, sprites, music, models)
# never actual text. Skipping them avoids false-positive text detection.
# NOTE: BOSS is filtered out for the regular text scan (no dialog/menu text),
# but enemy/boss NAME tables in BOSS*.DAT are still extracted via the
# tag-based detector in _find_enemy_tables — that path bypasses this filter.
_NON_TEXT_PREFIXES = frozenset([
    'ART', 'BGM', 'BMAGIC', 'BMAGXA', 'BPLAYER', 'BWORLD',
    'PLAYER', 'MAGIC', 'MDEMO', 'BDEMO', 'FLD_', 'RTEST',
    'BOSS',
    'COMMU01', 'COMMU02', 'COMMU04', 'COMMU05', 'COMMU06',
    'COMMU07', 'COMMU08', 'COMMU09A', 'COMMU09C', 'COMMUX',
])


# ── Enemy/boss name tables ───────────────────────────────────────────────────
# AREAB*.DAT and BOSS*.DAT entries with this exact size + f3 tag are packed
# enemy struct tables: 0xA0-byte header + 8 records of 264 bytes each. The
# first 8 bytes of each record are the enemy's name (raw ASCII, null-padded
# short, no terminator if exactly 8 chars). Layout verified universal across
# all 131 AREAB + 32 BOSS files (Agent A inventory sweep, 2026-05-02).
ENEMY_TABLE_SIZE       = 0x8e0
ENEMY_TABLE_F3         = 0x1f5800
ENEMY_TABLE_HEADER     = 0xA0
ENEMY_TABLE_STRIDE     = 0x108
ENEMY_TABLE_NAME_BYTES = 8
ENEMY_TABLE_SLOTS      = 8


def is_enemy_table_entry(e_size: int, e_f2: int, e_f3: int) -> bool:
    """Tag-based detector for the enemy/boss name table layout. Same check
    is used by repack_text.py and localization_editor.py — keep in sync."""
    return e_f2 == 0 and e_size == ENEMY_TABLE_SIZE and e_f3 == ENEMY_TABLE_F3


def _extract_enemy_names(data, e_off, e_size):
    """Walk the 8 fixed-stride enemy slots, extract each non-empty 8-byte
    name. Returns (offsets, strings, suffix_of) in the same shape as the
    flat-entry extractor (suffix_of is always {} — no offset table)."""
    offsets = []
    strings = {}
    for slot in range(ENEMY_TABLE_SLOTS):
        rel = ENEMY_TABLE_HEADER + slot * ENEMY_TABLE_STRIDE
        if rel + ENEMY_TABLE_NAME_BYTES > e_size:
            break
        raw = bytes(data[e_off + rel: e_off + rel + ENEMY_TABLE_NAME_BYTES])
        name = raw.rstrip(b'\x00')
        if not name:
            continue                    # empty slot (e.g. BOSS011 placeholder)
        # First byte must be uppercase A-Z, all bytes printable ASCII.
        if not (0x41 <= name[0] <= 0x5A):
            continue
        if not all(0x20 <= b < 0x7F for b in name):
            continue
        offsets.append(rel)
        strings[rel] = name.decode('latin1')
    return offsets, strings, {}


def _find_enemy_tables(data, toc_size):
    """Detect every enemy/boss name table in the TOC. Returns a list of
    result tuples in the same 9-element shape as regular extraction
    results, with the 9th element set to "enemy_table" so the writer
    can emit the marker comment."""
    if toc_size < 16:
        return []
    num_entries = (toc_size - 16) // 16
    out = []
    for i in range(num_entries):
        base = 16 + i * 16
        if base + 16 > toc_size:
            break
        e_off = read_uint32(data, base)
        e_size = read_uint32(data, base + 4)
        e_f2 = read_uint32(data, base + 8)
        e_f3 = read_uint32(data, base + 12)
        if not is_enemy_table_entry(e_size, e_f2, e_f3):
            continue
        if e_off + e_size > len(data):
            continue
        offsets, strings, suffix_of = _extract_enemy_names(data, e_off, e_size)
        if not strings:
            continue                    # all-zero slots (BOSS011)
        # pool_start is meaningless for fixed-stride tables; use 0 as sentinel.
        out.append((i, e_off, e_size, 0, offsets, strings, 0,
                    suffix_of, "enemy_table"))
    return out


def _is_non_text_file(basename):
    """Check if a file is known to NOT contain text."""
    name = basename.upper().replace('.DAT', '').replace('.EMI', '')
    for prefix in _NON_TEXT_PREFIXES:
        if name == prefix or name.startswith(prefix):
            return True
    return False


def process_file(path):
    basename = os.path.basename(path)

    with open(path, "rb") as f:
        data = f.read()

    # Materialise any baked NA-uncensor dialogue into the DAT in memory so the
    # dump shows the uncut lines even from a pristine (censored) file. No-op on
    # DATs without a patch or already uncensored. See uncensor_patch.py.
    data = uncensor_patch.apply(basename, data)

    toc_size = read_uint32(data, 0)
    if toc_size > len(data) or toc_size < 16:
        return None

    # Enemy/boss name tables — extracted regardless of filename filter.
    # AREAB*/BOSS* contain no dialog text but DO contain enemy names that
    # the regular text scan would never reach (BOSS is in _NON_TEXT_PREFIXES).
    enemy_results = _find_enemy_tables(data, toc_size)
    enemy_entry_indices = {r[0] for r in enemy_results}

    # Skip the regular text scan for known-binary file prefixes; enemy
    # tables already collected above survive.
    if _is_non_text_file(basename):
        return enemy_results or None

    candidates = find_text_entry(data, toc_size)
    if not candidates and not enemy_results:
        return None

    results = list(enemy_results)
    for entry_idx, e_off, e_size, pool_start in (candidates or []):
        if entry_idx in enemy_entry_indices:
            continue                    # already handled as enemy table
        offsets, strings, hidden_count, suffix_of = extract_strings_from_entry(
            data, e_off, e_size, pool_start)
        if strings and _entry_has_readable_text(strings):
            results.append((entry_idx, e_off, e_size, pool_start,
                            offsets, strings, hidden_count, suffix_of, "flat"))

    return results or None


def main(dat_dir=None, out_dir=None, jp=False):
    # Source DAT folder + output text_dump folder. Default to the module-level
    # constants (CLI/standalone) but let callers (the localization editor's
    # "Dump Text" button) override both. jp=True decodes the Japanese build's
    # kana/kanji encoding instead of the PC Latin-1 text.
    global JP_MODE
    JP_MODE = bool(jp)
    if JP_MODE:
        _load_jp_tables()   # fail early if tables missing
    dat_dir = dat_dir or DAT_DIR
    out_dir = out_dir or OUT_DIR
    os.makedirs(out_dir, exist_ok=True)

    # Process all DAT files
    all_dat = sorted(glob.glob(os.path.join(dat_dir, "*.DAT")))
    total_strings = 0
    files_with_text = 0

    summary_lines = []

    for path in all_dat:
        results = process_file(path)
        if not results:
            continue

        basename = os.path.basename(path)
        files_with_text += 1
        out_path = os.path.join(out_dir, basename.replace(".DAT", ".txt"))

        with open(out_path, "w", encoding="utf-8") as out:
            out.write(f"# {basename}\n\n")
            for r in results:
                entry_idx, e_off, e_size, pool_start, offsets, strings, hidden_count, suffix_of = r[:8]
                kind = r[8] if len(r) > 8 else "flat"
                out.write(f"## Entry {entry_idx} @ 0x{e_off:X} (size 0x{e_size:X})\n")
                if kind == "enemy_table":
                    # Marker recognized by localization_editor (8-byte
                    # length warning) and repack_text (fixed-position
                    # patch path). Keep wording stable.
                    out.write(f"## enemy-name table (max {ENEMY_TABLE_NAME_BYTES} bytes per name)\n")
                out.write(f"## {len(strings)} unique strings, {len(offsets)} offset-table entries\n\n")
                for str_off in sorted(strings.keys()):
                    line = f"[0x{str_off:04X}] {repr(strings[str_off])}"
                    if str_off in suffix_of:
                        line += f"  # suffix:[0x{suffix_of[str_off]:04X}]"
                    out.write(line + "\n")
                out.write("\n")

        file_count = sum(len(r[5]) for r in results)
        file_hidden = sum(r[6] for r in results)
        _file_suffix = sum(len(r[7]) for r in results)
        total_strings += file_count
        summary_lines.append(f"  {basename}: {file_count} strings")

    print(f"Files with text: {files_with_text}")
    print(f"Total unique strings: {total_strings}")
    print("\nPer file:")
    for line in summary_lines[:30]:
        print(line)
    if len(summary_lines) > 30:
        print(f"  ... and {len(summary_lines)-30} more files")


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser(
        description="Extract BoF4 English source text from a DAT folder to "
                    ".txt files (one per DAT) in an output folder.")
    ap.add_argument("--dat-dir", default=None,
                    help="Source DAT folder (default: DAT_backup/ else DAT/).")
    ap.add_argument("--out-dir", default=None,
                    help="Output text_dump folder (default: ./text_dump).")
    ap.add_argument("--jp", action="store_true",
                    help="Decode the Japanese build's kana/kanji encoding "
                         "(uses jp_tables/). Default: PC/English Latin-1 text.")
    args = ap.parse_args()
    main(args.dat_dir, args.out_dir, jp=args.jp)
