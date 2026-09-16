#!/usr/bin/env python3
"""
Breath of Fire IV Localization Editor
PO-Edit-style translation tool for BoF4 text_dump files.

Features:
- Dual-panel source/translation view with Raw and Formatted tabs
- Cross-file duplicate detection and sync
- Glossary/terminology management with consistency warnings
- Dialog box mockup preview (approximates game text box)
- Progress tracking (per-file and overall)
- Validation (control code mismatch, byte limits)
- String status flags (draft/reviewed/final)
- Auto-save every 3 minutes
- Batch find & replace across all translations
- Search across all files with filter (all/untranslated/translated)
- Clean view (plain text without codes for machine translation)

Usage: py localization_editor.py
Translations saved to: translations/<filename>.json
Status saved to: translations/<filename>_status.json
Glossary: translations/glossary.json
"""

import tkinter as tk
from tkinter import ttk, messagebox, filedialog
import json, re, os, sys, time, subprocess, shutil, difflib


def _run_helper_and_exit():
    """Frozen-exe entrypoint for the bundled CLI helpers.

    When this editor is packaged as a single .exe, there is no python.exe to run
    `extract_text.py` / `repack_text.py` as separate processes. Instead the editor
    re-execs ITS OWN exe with `--run-helper <name> <args…>`; that invocation is
    caught here (before any Tk/GUI work) and dispatched into the bundled helper
    module's CLI, exactly as `python <name>.py <args…>` would have. Returns
    immediately (no-op) when not invoked this way, e.g. running from source."""
    if len(sys.argv) < 2 or sys.argv[1] != "--run-helper":
        return
    # In a windowed (--noconsole) PyInstaller build sys.stdout/stderr may be None,
    # which would make the helpers' print() calls crash. The parent captures our
    # output via redirected pipes (fd 1/2), so bind real text streams onto them.
    for _fd, _name in ((1, "stdout"), (2, "stderr")):
        if getattr(sys, _name, None) is None:
            try:
                setattr(sys, _name, os.fdopen(_fd, "w", encoding="utf-8",
                                              errors="replace", buffering=1))
            except Exception:
                import io
                setattr(sys, _name, io.StringIO())
    name = sys.argv[2] if len(sys.argv) > 2 else ""
    forwarded = sys.argv[3:]
    # Present the helper with a normal argv (argv[0] = the helper's name) so its
    # own argparse / __main__ logic sees exactly what it expects.
    sys.argv = [name + ".py"] + forwarded
    try:
        if name == "extract_text":
            import extract_text
            ap = _extract_argparser()
            a = ap.parse_args(forwarded)
            extract_text.main(a.dat_dir, a.out_dir, jp=a.jp)
        elif name == "repack_text":
            import repack_text
            repack_text._expand_globs_in_argv()
            repack_text.main()
        elif name == "selftest":
            # Verify bundled resources resolve inside the frozen exe.
            import pristine_guard
            man = pristine_guard.load_manifest()
            print("frozen      =", getattr(sys, "frozen", False))
            print("_MEIPASS    =", getattr(sys, "_MEIPASS", None))
            print("exe_dir     =", os.path.dirname(sys.executable))
            print("manifest    =", "OK (%d dats)" % len(man.get("dats", {}))
                  if man else "MISSING")
            import extract_text
            extract_text._load_jp_tables()
            print("jp_tables   = OK (%d kanji)" % len(extract_text._JP_KANJI))
        else:
            sys.stderr.write("unknown helper: %s\n" % name)
            sys.exit(2)
    except SystemExit:
        raise
    except Exception:
        import traceback
        traceback.print_exc()
        sys.exit(1)
    sys.exit(0)


def _extract_argparser():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--dat-dir", default=None)
    ap.add_argument("--out-dir", default=None)
    ap.add_argument("--jp", action="store_true")
    return ap


_run_helper_and_exit()

try:
    from PIL import Image, ImageTk
    _HAS_PIL = True
except ImportError:
    _HAS_PIL = False

# Pristine-DAT guard (manifest-based text verification for safe repacking).
try:
    import pristine_guard as _pg
except Exception:
    _pg = None

# GOG exe rollback (update 7 -> 6) via a bundled binary-delta patch.
try:
    import gog_rollback as _gogroll
except Exception:
    _gogroll = None

# Optional spell-checker + LibreOffice dictionary manager (see loc_spellcheck).
try:
    import loc_spellcheck as _spell
except Exception:
    _spell = None

# ── Paths ──────────────────────────────────────────────────────────────────────
# SCRIPT_DIR = this editor's own folder (localization_editor/tools/localization_editor/).
# Editor-owned assets + the repack/extract scripts live HERE.
# When frozen by PyInstaller (onefile), __file__ points into a temp _MEIPASS
# extraction dir, so anchor writable/game-relative paths (configs/, dictionaries/,
# game-root walk-up) on the real .exe folder instead.
if getattr(sys, "frozen", False):
    SCRIPT_DIR = os.path.dirname(sys.executable)
else:
    SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))


def _find_game_root(start):
    """Walk up from `start` until we reach the game root — the folder holding
    DAT/ or translations/ (this editor lives a few levels below it). Game DATA
    (translations, text_dump, DAT, DAT_backup, bof4_psx, vwf_config.txt) is
    resolved relative to GAME_ROOT so the editor works from its nested home.
    Falls back to `start`."""
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


GAME_ROOT     = _find_game_root(SCRIPT_DIR)
DUMP_DIR      = os.path.join(GAME_ROOT, "text_dump")
TRANS_DIR     = os.path.join(GAME_ROOT, "translations")
GLOSSARY_PATH = os.path.join(TRANS_DIR, "glossary.json")
# Persistent record of duplicate locations the user has chosen to "unlock"
# (edit independently from the shared primary). Stored as a flat list of
# "base|entrynum:offset" strings. Excluded from the translation-json
# enumeration loops below (see _is_aux_json).
DUP_UNLOCK_FILENAME = "_duplicate_unlocks.json"
DUP_UNLOCK_PATH     = os.path.join(TRANS_DIR, DUP_UNLOCK_FILENAME)
# Persistent record of user-chosen MASTERS for duplicate groups. By default the
# master of a shared-string group is its first occurrence (file order); this lets
# the user pick a DIFFERENT occurrence to be the one every duplicate sources from,
# and Sync All propagates FROM that chosen master. Stored as {raw: "base|key"}.
DUP_MASTER_FILENAME = "_duplicate_masters.json"
DUP_MASTER_PATH     = os.path.join(TRANS_DIR, DUP_MASTER_FILENAME)
# Monster (enemy) names live in the AREAB*.DAT enemy-name tables (the entry the
# extractor labels "enemy-name table (max 8 bytes per name)"). By default they are
# SEALED — locked as English so they're never accidentally translated or filled by
# duplicate-sync — and the translator UNSEALS a name to translate it on purpose.
# This stores the set of (base|key) the user has unsealed.
MONSTER_UNSEAL_FILENAME = "_monster_unsealed.json"
MONSTER_UNSEAL_PATH     = os.path.join(TRANS_DIR, MONSTER_UNSEAL_FILENAME)

# ── Uncensor-restored dialogue (AREAM027 sailors/Ursula ship scene) ───────────
# The NA release censored AREAM027 twice over: the scene script was cut, AND the
# message-offset table collapsed 10 dialogue slots (ids 0x46-0x4F) onto the single
# "you win" line while stripping the distinct lines from the string pool. The
# runtime uncensor fix (d3d9 hook flag `censor_fix_aream027`) restores the
# choreography, and AREAM027.DAT was repacked with the original English lines
# (recovered from the Navarchos uncensor patch). These slots render ONLY when the
# scene is played uncensored, so in the editor they are SEALED (locked as English)
# by default — this keeps them from confusing translators who aren't restoring the
# scene, and from being auto-filled/counted as normal TODO. The translator UNLOCKS
# a line to translate it on purpose; the choice persists (like monster unseals).
UNCENSOR_UNLOCK_FILENAME = "_uncensor_unlocked.json"
UNCENSOR_UNLOCK_PATH     = os.path.join(TRANS_DIR, UNCENSOR_UNLOCK_FILENAME)
# (base, key) of the restored uncensor-only dialogue slots (AREAM027 entry 8).
# Offsets are the repacked-pool offsets of the 9 restored lines (0x4F "you win"
# already existed and stays a normal entry). 0x29B7 is Ursula's pants-drop line
# ("You just want to see my butt, right?") — its portrait code was repaired from
# the truncated [14:44] to the correct 2-param [14:83:44] (see AREAM027 notes),
# which shifted the following slot 0x29BC -> 0x29E3.
UNCENSOR_ENTRIES = frozenset({
    ("AREAM027", "8:[0x27DF]"), ("AREAM027", "8:[0x280B]"),
    ("AREAM027", "8:[0x2865]"), ("AREAM027", "8:[0x28E7]"),
    ("AREAM027", "8:[0x2913]"), ("AREAM027", "8:[0x2941]"),
    ("AREAM027", "8:[0x2967]"), ("AREAM027", "8:[0x29B7]"),
    ("AREAM027", "8:[0x29E3]"),
    # AREAD145 — girls' bath (area 277). 10 uncensor-only dialogue lines (entry 7).
    # Text-bearing segments only (the head-only portrait segments carry no text).
    # Offsets = uncensor_patch.apply() append order (0x43 carries its portrait
    # switch inline, so it's one combined segment; 0x44/0x47 split head+text).
    # NOTE: slot 0x4D (0x1D0A) "We were able to find fresh water..." is a normal
    # scene line (byte-identical duplicate of 0x11A8), NOT uncensor-only — so it is
    # deliberately NOT sealed; it surfaces as an ordinary duplicate the translator
    # handles normally.
    ("AREAD145", "7:[0x1ABB]"), ("AREAD145", "7:[0x1AE0]"),
    ("AREAD145", "7:[0x1B31]"), ("AREAD145", "7:[0x1B99]"),
    ("AREAD145", "7:[0x1BB4]"), ("AREAD145", "7:[0x1C18]"),
    ("AREAD145", "7:[0x1C51]"), ("AREAD145", "7:[0x1C70]"),
    ("AREAD145", "7:[0x1CA5]"), ("AREAD145", "7:[0x1CE3]"),
    # AREAD157 — Emperor confrontation (area 289). ONE uncensor-only line (entry 9):
    # Yuna's "Well! Just as expected of the [weapon]..." box, which the NA build
    # collapsed onto its sibling 0x1E. Offset = uncensor_patch.apply() append point
    # (end of the pristine rec#9 pool). The rest of the cut was violence choreography
    # (d3d9 hook censor_fix_aread157), not text.
    ("AREAD157", "9:[0x0F61]"),
})

# ── Manual duplicate mappings ────────────────────────────────────────────────
# (base, "entrynum:[0xOFFSET]") -> (master_base, master_key).
# For slots whose SOURCE text DIFFERS from the master but which should
# nonetheless inherit the master's translation. The normal duplicate system
# groups by identical raw text; this covers the cases the game's data layout
# makes a slot fall through to another item's line.
#
# AB000_00 entry 1: three weapon-description slots (0x4A00 / 0x4A03 / 0x4A06)
# have NO description of their own — they are interior pointers into the
# killcount weapon's multi-item root and, via the divider fall-through, render
# the SAME line as the killcount weapon ("Powers up after 1000 kills.").
# In-game they are meant to show the elemental-ranged line at 0x0583
# ("Pwr[07:01] Wgt[07:02]  [07:06] Ranged" -> Fire/Water/Wind Ranged). So we
# point them at 0x0583: the translator writes it once there and these three
# inherit it. Locked by default; the standard Unlock button frees any of them
# for independent editing (persisted in _duplicate_unlocks.json like all dups).
MANUAL_DUP_MAP = {
    ("AB000_00", "1:[0x4A00]"): ("AB000_00", "1:[0x0583]"),
    ("AB000_00", "1:[0x4A03]"): ("AB000_00", "1:[0x0583]"),
    ("AB000_00", "1:[0x4A06]"): ("AB000_00", "1:[0x0583]"),
}
# master (base, key) -> [target (base, key), ...]  (reverse of MANUAL_DUP_MAP)
MANUAL_DUP_REVERSE = {}
for _t, _m in MANUAL_DUP_MAP.items():
    MANUAL_DUP_REVERSE.setdefault(_m, []).append(_t)


def _is_aux_json(fname):
    """True for json files in TRANS_DIR that are NOT per-file translations
    (status sidecars, glossary, the duplicate-unlock record). Centralises
    the exclusion used by every TRANS_DIR enumeration loop."""
    return (fname.endswith("_status.json")
            or fname.endswith("_notes.json")
            or fname == "glossary.json"
            or fname == DUP_UNLOCK_FILENAME
            or fname == DUP_MASTER_FILENAME
            or fname == MONSTER_UNSEAL_FILENAME
            or fname == UNCENSOR_UNLOCK_FILENAME)
EDITOR_CONFIG_DIR = os.path.join(SCRIPT_DIR, "configs")
CONFIG_PATH   = os.path.join(EDITOR_CONFIG_DIR, "editor_config.json")
# Migrate legacy config from old location (translations/editor_config.json).
_LEGACY_CONFIG_PATH = os.path.join(TRANS_DIR, "editor_config.json")
if os.path.isfile(_LEGACY_CONFIG_PATH) and not os.path.isfile(CONFIG_PATH):
    try:
        os.makedirs(EDITOR_CONFIG_DIR, exist_ok=True)
        os.replace(_LEGACY_CONFIG_PATH, CONFIG_PATH)
    except Exception:
        pass
# Hunspell dictionaries (LibreOffice) for the optional spell-checker, stored
# flat as <base>.aff / <base>.dic. Managed via the "Dictionaries" dialog.
DICT_DIR      = os.path.join(SCRIPT_DIR, "dictionaries")
MOCKUP_PATH   = os.path.join(SCRIPT_DIR, "message_box_mockup.png")
TEXTBOX_PATH  = os.path.join(SCRIPT_DIR, "textbox_empty.png")
ADVANCE_PATH  = os.path.join(SCRIPT_DIR, "textbox_advance_symbol.png")
PORTRAIT_PATH = os.path.join(SCRIPT_DIR, "portrait_mockup.png")
FONT_DIR          = os.path.join(SCRIPT_DIR, "font")
FONT_METRICS_PATH = os.path.join(FONT_DIR, "font_metrics.json")
VWF_CONFIG_PATH       = os.path.join(GAME_ROOT, "vwf_config.txt")
# Editor-only override. If this file exists, the localization editor
# uses it for preview/VWF-editor purposes. The original vwf_config.txt
# is never modified by the editor — the DLL and repacker still read it.
VWF_EDITOR_PATH       = os.path.join(EDITOR_CONFIG_DIR, "vwf_config_editor.txt")

# Char byte -> Unicode char.
# Auto-derived from vwf_config.txt so the editor stays in sync with
# whichever atlas slots the artist re-purposed for non-ASCII glyphs.
# Any line `0xCC = NAME : ...` where NAME is one non-ASCII character
# becomes a mapping (CC -> NAME). ASCII bytes 0x20..0x7E fall through
# to chr(byte) in `_byte_to_unicode` below.
_VWF_CONFIG_PATH_FOR_MAP = os.path.join(GAME_ROOT, "vwf_config.txt")
_VWF_LINE_RE_FOR_MAP = re.compile(r"0x([0-9A-Fa-f]+)\s*=\s*(\S+)\s*:")

def _load_umlaut_byte_to_unicode():
    out = {}
    if not os.path.isfile(_VWF_CONFIG_PATH_FOR_MAP):
        return out
    try:
        with open(_VWF_CONFIG_PATH_FOR_MAP, encoding="utf-8") as f:
            for line in f:
                line = line.split("#", 1)[0].strip()
                if not line:
                    continue
                m = _VWF_LINE_RE_FOR_MAP.match(line)
                if not m:
                    continue
                byte = int(m.group(1), 16)
                name = m.group(2)
                if len(name) == 1 and not (0x20 <= ord(name) <= 0x7E):
                    out[byte] = name
    except Exception:
        return {}
    return out

_UMLAUT_BYTE_TO_UNICODE = _load_umlaut_byte_to_unicode()
if not _UMLAUT_BYTE_TO_UNICODE:
    # Fallback if vwf_config.txt is missing — legacy mapping.
    _UMLAUT_BYTE_TO_UNICODE = {
        0x88: 'Ä', 0x89: 'Ü', 0x8A: 'Ö',
        0x8B: 'ä', 0x8C: 'ü', 0x8D: 'ö',
        0x8E: 'ß',
    }

# Reverse map for the [09:22:XX] glyph-emit warning: given a non-ASCII
# letter (Ä/Ö/Ü/ä/ö/ü/ß), what game-atlas byte should be in the opcode?
_UNICODE_TO_GAME_BYTE = {ch: b for b, ch in _UMLAUT_BYTE_TO_UNICODE.items()}
# Capital→capital and lower→capital both map to the capital atlas byte —
# the opcode emits a sentence-initial capital, so a German line starting
# with 'ä' still wants the 'Ä' atlas index (0x90 in stock config).
_UMLAUT_TO_CAPITAL = {
    'ä': 'Ä', 'ö': 'Ö', 'ü': 'Ü',
    'Ä': 'Ä', 'Ö': 'Ö', 'Ü': 'Ü',
    'ß': 'ß',  # no caps form — leave as-is
}
_GLYPH_EMIT_RE = re.compile(r"\[09:22:([0-9A-Fa-f]{2})\]")

def _byte_to_unicode(b):
    if b in _UMLAUT_BYTE_TO_UNICODE:
        return _UMLAUT_BYTE_TO_UNICODE[b]
    if 0x20 <= b <= 0x7E:
        return chr(b)
    return None

def _glyph_emit_char(inner):
    """If `inner` (upper colon-form, e.g. '09:22:44' or '18:22:44') is a 3-byte
    glyph-emit opcode — middle byte 0x22 — return the character its trailing
    byte draws from the font atlas (0x44 -> 'D', 0x90 -> 'Ä'), else None.
    The middle-byte==0x22 shape is the confirmed glyph-emit form; the leading
    byte (0x09 most common, 0x18 also seen) selects portrait/state side effects
    but does not change which glyph the last byte renders. Bytes with no atlas
    mapping (e.g. the 0x00-0x07 name-table refs) return None so the caller keeps
    its existing placeholder rendering. Uses the ASCII range straight and the
    umlaut atlas bytes from _byte_to_unicode; mirror vwf_config.txt here if the
    atlas layout ever diverges from ASCII in the 0x20-0x7E range."""
    parts = inner.split(':')
    if len(parts) == 3 and parts[1] == '22':
        try:
            return _byte_to_unicode(int(parts[2], 16))
        except ValueError:
            return None
    return None
DEFAULT_LINE_MAX_PX = 290   # game-pixel max line width (user-adjustable)
DEFAULT_SPACE_PX    = 8     # space advance in game pixels
os.makedirs(TRANS_DIR, exist_ok=True)
# Ensure the source-text dir exists too, so os.listdir(DUMP_DIR) never crashes
# when the editor runs from a copied/standalone location (the default
# GAME_ROOT/text_dump may not be present there). The user points 'Source…' at
# their real text_dump/ folder; that choice persists (see _apply_data_dirs).
os.makedirs(DUMP_DIR, exist_ok=True)
# Editor-owned config/dict folders (writes: editor_config.json, vwf_config_editor.txt,
# font_metrics.json, installed dictionaries) — create so first-run writes don't crash.
os.makedirs(EDITOR_CONFIG_DIR, exist_ok=True)
os.makedirs(DICT_DIR, exist_ok=True)

# ── Regex ──────────────────────────────────────────────────────────────────────
CTRL_RE  = re.compile(r'\[[0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2})*\]')
STR_RE   = re.compile(r"^(\[0x[0-9A-Fa-f]+\]) (['\"])(.*)\2$")

# ── Game text colors (from bof4text.txt section 4c) ────────────────────────────
TEXT_COLORS = {
    '01': '#AAAAAA',  # Grey
    '02': '#FF6666',  # Red
    '03': '#66DDDD',  # Cyan
    '04': '#66DD66',  # Green
    '05': '#FF99CC',  # Pink
    '06': '#DDDD66',  # Yellow
    '07': '#DD66DD',  # Magenta
    '08': '#FFFFFF',  # White
    '0D': '#66DDDD',  # Used for icons (ice etc.)
}

PARTY_NAMES = {
    0x00: 'Ryu', 0x01: 'Nina', 0x02: 'Cray', 0x03: 'Scias',
    0x04: 'Ursula', 0x05: 'Ershin', 0x06: 'Fou-Lu',
}

# [04:XX] is dual-purpose: a party-name ref for 0x00-0x06, but a GLYPH-EMIT
# (draw the atlas letter XX) for any byte that has a glyph — e.g. [04:66]='f'
# in "First", [04:6D]='m' in "müsst", [04:22]='"'. The emitted letter is
# content the translator freely changes, so for the code-check it's stripped
# from both sides (only the 0x00-0x06 name refs stay comparable, so a dropped
# name is still caught). Rendering still shows the real letter (see the 04:
# branches in _render_clean / _render_mockup_page / _render_fmt_editable).
_GLYPH04_RE = re.compile(r"\[04:([0-9A-Fa-f]{2})\]")

def _norm_glyph04(text):
    """Drop [04:XX] glyph-emit letters (XX>=0x20 with an atlas glyph) so they
    don't register as control-code mismatches; keep [04:00]..[04:06] name refs."""
    def _repl(m):
        try:
            b = int(m.group(1), 16)
        except ValueError:
            return m.group(0)
        return "" if (b not in PARTY_NAMES and _byte_to_unicode(b) is not None) \
            else m.group(0)
    return _GLYPH04_RE.sub(_repl, text)

# ── Control code labels (for Formatted source view) ────────────────────────────
CTRL_NAMES = {
    '02':    '[→]',
    '03':    '[HERO]',
    '06':    '[/col]',
    '08':    '[~]',
    '0B':    '[▶]',
    '0D':    '[ANIM]',
    '04:00': '[Ryu]',
    '04:01': '[Nina]',
    '04:02': '[Cray]',
    '04:03': '[Scias]',
    '04:04': '[Ursula]',
    '04:05': '[Ershin]',
    '04:06': '[Fou-Lu]',
    '05:01': '[grey▶]',
    '05:02': '[red▶]',
    '05:03': '[cyan▶]',
    '05:04': '[green▶]',
    '05:05': '[pink▶]',
    '05:06': '[yellow▶]',
    '05:07': '[mag▶]',
    '05:08': '[white▶]',
    '0C:00': '[↓mid]',
    '0C:01': '[mid]',
    '0C:02': '[↑mid]',
    '0C:03': '[↑L]',
    '0C:04': '[↑R]',
    '0C:05': '[↓L]',
    '0C:06': '[↓R]',
    '0E:0F': '[/ANIM]',
    '10:00': '[type:on]',
    '10:01': '[type:off]',
}

def load_translations_json(path):
    """Load a translations JSON, normalizing dict entries to plain strings.
    Handles both old format (plain strings) and new format from PSX extraction
    ({"source": ..., "translation": ...} dicts)."""
    with open(path, encoding="utf-8") as f:
        raw = json.load(f)
    return {k: (v.get("translation", "") if isinstance(v, dict) else v)
            for k, v in raw.items()}


def load_translations_full(path):
    """Load a translations JSON preserving the per-entry SOURCE text.

    Returns {key: {"source": <base-game text or None>, "translation": <target>}}.
    Legacy plain-string values become {"source": None, "translation": v}. The
    `source` field is what makes translations content-addressed: the editor pairs
    a stored translation to its current DAT row by matching this text, so
    translations survive DAT layout changes (see migrate_translations_source.py).
    """
    with open(path, encoding="utf-8") as f:
        raw = json.load(f)
    out = {}
    for k, v in raw.items():
        if isinstance(v, dict):
            out[k] = {"source": v.get("source"),
                      "translation": v.get("translation", "")}
        else:
            out[k] = {"source": None, "translation": v}
    return out


def write_translations_json(path, trans_map):
    """Write a `{key: translation_string}` map to a translations JSON in the
    content-addressed dict form, PRESERVING each key's existing `source` field
    from whatever is already on disk. Use this for any batch/side write so
    source survives (find/replace, Clean-PSX, cross-file propagation). Keys not
    on disk are written with source=None; they re-fill from the live row the
    next time that file is opened and saved in the editor."""
    existing = {}
    if os.path.exists(path):
        try:
            existing = load_translations_full(path)
        except Exception:
            existing = {}
    out = {k: {"source": existing.get(k, {}).get("source"), "translation": t}
           for k, t in trans_map.items()}
    with open(path, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, indent=2, sort_keys=True)


def ctrl_label(code: str) -> str:
    inner = code[1:-1].upper()
    if inner.startswith('0E:0F:'):
        suffix = inner[6:]
        return CTRL_NAMES.get('0E:0F', '[/ANIM]') + f':{suffix}]'
    return CTRL_NAMES.get(inner, code)

# ── German umlauts ────────────────────────────────────────────────────────────
# Umlauts are written as native UTF-8 characters into the exported .txt.
# repack_text.py encodes them at pack time to bytes 0x88..0x8E which address
# slots 104..110 of INIT.DAT entry 9 (the dialog font atlas). The glyphs
# must be drawn into the atlas via localization_editor/tools/font_editor/font_editor_gui.py and the
# per-char advance widths configured in vwf_config.txt (applied at runtime
# by the d3d9 hook DLL). Current byte-to-slot layout:
#   Ä 0x88 (104)   Ü 0x89 (105)   Ö 0x8A (106)
#   ä 0x8B (107)   ü 0x8C (108)   ö 0x8D (109)   ß 0x8E (110)

# ── String status flags ──────────────────────────────────────────────────────
STATUS_NONE     = ""
STATUS_DRAFT    = "draft"
STATUS_REVIEWED = "reviewed"
STATUS_FINAL    = "final"
STATUS_CYCLE    = [STATUS_NONE, STATUS_DRAFT, STATUS_REVIEWED, STATUS_FINAL]
STATUS_ICONS    = {
    STATUS_NONE:     ("", "#666666"),
    STATUS_DRAFT:    ("D", "#CCAA44"),
    STATUS_REVIEWED: ("R", "#44AACC"),
    STATUS_FINAL:    ("F", "#44CC44"),
}

# ── Dialog box constants (approximate game rendering) ──────────────────────────
DIALOG_CHARS_PER_LINE = 30   # approximate max chars per line with VWF
DIALOG_MAX_LINES      = 3    # max visible lines in one dialog box
MOCKUP_WIDTH          = 420  # legacy: kept for code paths that still reference it
# Pixel-perfect mockup: composite at native game resolution
# (textbox_empty.png is 480x104), then scale up with NEAREST so each
# game pixel becomes N screen pixels.
MOCKUP_NATIVE_W       = 480
MOCKUP_NATIVE_H       = 104
MOCKUP_DISPLAY_SCALE  = 2    # 1 = native size, 2 = 2x for readability
MOCKUP_TEXT_INSET     = 8    # left padding inside textbox (game pixels)
MOCKUP_TEXT_TOP       = 12   # top padding inside textbox (game pixels)
MOCKUP_LINE_HEIGHT    = 24   # game-pixel line height (matches glyph cell)

# ── Auto-save interval ────────────────────────────────────────────────────────
AUTOSAVE_INTERVAL_MS = 180_000  # 3 minutes

# ── Filter modes ──────────────────────────────────────────────────────────────
FILTER_ALL          = "all"
FILTER_UNTRANSLATED = "untranslated"
FILTER_TRANSLATED   = "translated"
FILTER_DRAFT        = "draft"
FILTER_REVIEWED     = "reviewed"
FILTER_FINAL        = "final"
FILTER_MISSING      = "missing_codes"
FILTER_EXTRA        = "extra_codes"
# Rows whose stored translation could not be re-attached because the base-game
# SOURCE text changed (a DAT/game update renamed/edited it). See _heal_translations.
FILTER_SOURCE_CHANGED = "source_changed"
# Rows carrying a translator note (see per-string notes feature).
FILTER_NOTED          = "noted"


# ── Text helpers ───────────────────────────────────────────────────────────────
def raw_to_display(raw: str) -> str:
    return raw.replace("\\n", "\n").replace("\\'", "'")

def display_to_raw(text: str) -> str:
    return text.replace("'", "\\'").replace("\n", "\\n")

def display_to_export(text: str) -> str:
    """Escape text for writing to a .txt file. Umlauts pass through as-is
    (UTF-8); repack_text.py encodes them to game bytes at pack time:
    Ä 0x88, Ü 0x89, Ö 0x8A, ä 0x8B, ü 0x8C, ö 0x8D, ß 0x8E."""
    return text.replace("'", "\\'").replace("\n", "\\n")

# Item divider for multi-item description blocks: `[16:NN]` + a trailing
# line/box index '@'/'A'/'B'/'C' (0x40..0x43). MUST match
# repack_text._FIRST_DIVIDER_RE. A tally across all dumps confirms exactly
# this set is used as item separators.
_MULTI_ITEM_SEP = re.compile(r"\[16:[0-9A-Fa-f]{2}\][@-C]")

# Trailing dump comment marking a suffix slot, written by extract_text as
# `  # suffix:[0xNNNN]`. Matched PRECISELY (not a generic ` # `) so it never
# truncates string content that legitimately contains " # " — e.g. "Damages
# change with # of faeries." was being chopped before its closing quote,
# failing STR_RE, and silently dropping the whole string from the editor.
_DUMP_SUFFIX_COMMENT_RE = re.compile(
    r"\s+#\s+suffix:\[0x[0-9A-Fa-f]+\]\s*$")


def _leading_item(raw: str) -> str:
    """The leading-item portion of a multi-item slot's raw: everything up to
    (not including) the NEXT `[16:NN]X` divider. Any divider(s) at the very
    start are absorbed as a prefix (they belong to this item — mirrors
    repack_text._find_item_segments' item-0 handling); without that a slot
    whose source begins with a divider would yield an empty leading item and
    mis-group with every other such slot. Returns `raw` unchanged when there
    is no further divider (e.g. the final item in the chain)."""
    start = 0
    while True:
        m0 = _MULTI_ITEM_SEP.match(raw, start)
        if not m0:
            break
        start = m0.end()
    m = _MULTI_ITEM_SEP.search(raw, start)
    return raw[:m.start()] if m else raw


def parse_dump(path: str):
    header, entries, cur = "", [], None
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("# "):
                header = line[2:].strip()
            elif line.startswith("## Entry"):
                if cur is not None:
                    entries.append(cur)
                m = re.match(r"## Entry (\d+) @ (0x[\dA-Fa-f]+) \(size (0x[\dA-Fa-f]+)\)", line)
                if m:
                    cur = dict(num=m.group(1), at=m.group(2), size=m.group(3),
                               unique=0, total=0, strings=[])
            elif line.startswith("## ") and cur is not None:
                m = re.match(r"## (\d+) unique strings, (\d+) offset-table entries", line)
                if m:
                    cur["unique"] = int(m.group(1))
                    cur["total"]  = int(m.group(2))
                # Per-entry hard byte cap marker emitted by extract_text.py
                # for enemy/boss name tables ("## enemy-name table (max 8
                # bytes per name)"). The validation pass uses max_byte_len
                # to flag too-long translations with a yellow remark; the
                # repacker enforces it independently and skips overflowing
                # slots, but we want the warning to surface in the editor
                # so the translator can reword in place.
                cap_m = re.match(r"## .*?\(max (\d+) bytes per name\)", line)
                if cap_m:
                    cur["max_byte_len"] = int(cap_m.group(1))
            elif line.startswith("[0x") and cur is not None:
                m_cmt = _DUMP_SUFFIX_COMMENT_RE.search(line)
                if m_cmt:
                    string_part = line[:m_cmt.start()]
                    comment     = m_cmt.group(0)
                else:
                    string_part = line
                    comment     = ""
                m = STR_RE.match(string_part)
                if m:
                    suffix_parent = None
                    sm = re.search(r'suffix:\[0x([0-9A-Fa-f]+)\]', comment)
                    if sm:
                        suffix_parent = f"[0x{sm.group(1).upper()}]"
                    cur["strings"].append({
                        "offset":    m.group(1),
                        "raw":       m.group(3),
                        "qchar":     m.group(2),
                        "suffix_of": suffix_parent,
                    })
    if cur is not None:
        entries.append(cur)
    if not header:
        header = os.path.basename(path)

    # Multi-item-root handling. A multi-item root packs several independent
    # descriptions glued with `[16:NN]X` separators; the offset table has one
    # slot per item (a sliding window into the shared bytes). repack_text
    # emits each such slot as its OWN null-terminated string (Option B), so
    # the per-slot translation IS used. Two consequences for the editor:
    #
    #  1. UNLOCK: suffix slots whose parent root carries the divider are
    #     cleared of `suffix_of` so each is independently editable.
    #  2. LEADING-ITEM VIEW: a slot's `raw` is the *whole* downstream tail
    #     (slot N + slot N+1 + ... glued). The game only renders this slot's
    #     leading item (up to the first divider), and that's all the user
    #     should translate. We replace `raw` with the leading item (keeping
    #     the full tail in `full_raw`). Because every later consumer — source
    #     display, validation, byte count, AND the shared-string duplicate
    #     index — now sees the leading item, slots that share an identical
    #     leading item (e.g. "Light healing of [07:00] target(s)." appearing
    #     at several offsets) collapse into ONE duplicate group: translate the
    #     primary once and it auto-propagates to the rest. Slots that genuinely
    #     need to differ can be split apart with the duplicate Unlock button.
    for ent in entries:
        by_off = {s["offset"]: s["raw"] for s in ent["strings"]}
        children_of = set(s["suffix_of"] for s in ent["strings"]
                          if s.get("suffix_of"))
        for s in ent["strings"]:
            parent_off = s.get("suffix_of")
            is_multi = False
            if parent_off:
                parent_raw = by_off.get(parent_off)
                if parent_raw and _MULTI_ITEM_SEP.search(parent_raw):
                    is_multi = True
                    s["suffix_of"] = None          # unlock
            elif (s["offset"] in children_of
                  and _MULTI_ITEM_SEP.search(s["raw"])):
                # The root of a multi-item group (has suffix children AND
                # carries a divider itself). Already editable; just trim its
                # source to its own leading item.
                is_multi = True
            if is_multi:
                s["multi_item"] = True
                s["full_raw"]   = s["raw"]
                s["raw"]        = _leading_item(s["raw"])
    return header, entries

def _extract_ctrl_codes(text: str) -> list:
    """Extract sorted list of control codes from a text string."""
    return sorted(CTRL_RE.findall(text))

# Button-prompt / special glyphs that the font atlas renders as icons and a
# translation must preserve. CTRL_RE misses these — they are not bracket codes.
# Two kinds live in the text:
#   * raw byte escapes \xNN  (e.g. \x7f, \x80..\x84 controller-button icons)
#   * the literal tilde '~'  (byte 0x7E) — also a button glyph in the atlas.
#     This is NOT a converted escape: extract_text.decode_string passes 0x7E
#     through as a plain char, so it shows/stores as a real '~'. We track it
#     here only so a dropped button prompt raises the same red warning.
# NOTE: \x7f is a genuine button glyph in the PC dialog text (e.g. "\x7f: Exit
# screen"), so it IS tracked. The \x7f->'"' normalization is a PSX-import-only
# concern and never touches the PC dumps this editor loads.
_SPECIAL_GLYPH_RE = re.compile(r"\\x[0-9A-Fa-f]{2}|~")

def _extract_special_glyphs(text: str) -> list:
    r"""Sorted button/special-glyph tokens in `text` — every \xNN escape
    (lower-cased) plus each literal '~'."""
    return sorted(m.group(0).lower() for m in _SPECIAL_GLYPH_RE.finditer(text))

def _strip_ctrl_codes(text: str) -> str:
    """Remove all control codes, returning plain visible text."""
    return CTRL_RE.sub('', text).replace('\\n', '\n').replace("\\'", "'")


def _search_norm(text: str, lower: bool = True) -> str:
    """Normalize text for forgiving search: drop `[XX:YY]` control codes, turn
    newlines into spaces, collapse runs of whitespace, lower-case. Lets a query
    like 'Effekt hält bis zur nächsten Rast an.' match a stored string
    'Effekt hält bis zur nächsten [05:04]Rast[06] an.' (or one split across a
    line break). Literal-with-codes searches still work via the raw match.

    Pass lower=False for a case-sensitive normalization (Match Case search)."""
    if not text:
        return ""
    t = _strip_ctrl_codes(text).replace("\n", " ")
    t = re.sub(r"\s+", " ", t).strip()
    return t.lower() if lower else t

def _is_ctrl_only_raw(raw: str) -> bool:
    """True if `raw` consists ONLY of control codes (no visible text).

    These have nothing to translate — the source can be copied verbatim
    into the translation. Examples: `[14:42]`, `[0C:06][17:41:00]`.
    A string with at least one printable char (letters, digits, quotes,
    even a lone period) is NOT ctrl-only.
    """
    if not raw:
        return False
    display = raw_to_display(raw)
    if not CTRL_RE.search(display):
        return False
    return _strip_ctrl_codes(display).strip() == ""


# ── Main application ───────────────────────────────────────────────────────────
class LocEditor(tk.Tk):
    BG      = "#2B2B2B"
    BG2     = "#3C3F41"
    BG3     = "#1E1E1E"
    FG      = "#BBBBBB"
    FG2     = "#DDDDDD"
    ACC_SRC = "#88AACC"
    ACC_TRA = "#88CC88"
    SEL     = "#2D6099"
    CTRL_FG = "#6A9FD8"
    AT_FG   = "#E08040"
    DONE_BG = "#1E3A1E"
    TODO_BG = "#3C3520"

    # Default row-tag colors. User overrides are stored in editor_config
    # under `tag_colors` and applied at startup; the "Colors…" toolbar
    # button opens a dialog that edits these and re-saves the config.
    DEFAULT_TAG_COLORS = {
        "done":     {"bg": "#1A1A1A", "fg": "#E8E8E8"},
        "todo":     {"bg": "#3C3520", "fg": "#CCCCAA"},
        "suffix":   {"bg": "#2B2020", "fg": "#776666"},
        "draft":    {"bg": "#3A3520", "fg": "#CCAA66"},
        "reviewed": {"bg": "#1E2A3A", "fg": "#88BBDD"},
        "final":    {"bg": "#1E3A1E", "fg": "#88DD88"},
    }

    def __init__(self):
        super().__init__()
        self.title("BoF IV Localization Editor")
        self.geometry("1500x980")
        self.minsize(900, 600)
        self.configure(bg=self.BG)
        self.protocol("WM_DELETE_WINDOW", self._on_close)

        # State
        self.current_file:      str  = ""
        self.file_header:       str  = ""
        self.entries:           list = []
        self.current_entry_idx: int  = 0
        self.current_str_idx:   int  = 0
        self.translations:      dict = {}
        self._dirty:            bool = False
        self._src_byte_count:   int  = 0
        self._shared_index:     dict = {}
        self._primary_loc:      dict = {}
        self._lead_index:       dict = {}   # (base, leading_item) -> locations
                                            # (multi-item slots; auto-fill empties)
        self._dump_mtimes:      dict = {}
        self._filter_mode:      str  = FILTER_ALL
        self._string_status:    dict = {}   # key -> status string
        self._notes:            dict = {}   # key -> translator note (per file)
        self._note_key                = None # row the note box is showing
        self._tm_entries:       list = []   # translation-memory (norm_src, src, tr)
        self._tm_word_index:    dict = {}   # word -> [tm entry idx] (fast filter)
        self._tm_dirty:         bool = True  # rebuild TM corpus on next suggest
        self._unlocked_ctrl_only: set = set()  # keys the user manually unlocked
                                                # (ctrl-only strings are auto-locked
                                                #  on load; override is in-memory only)
        self._unlocked_dups:    set = set()  # {(base, key)} duplicate locations the
                                             # user unlocked to edit independently
                                             # (persisted to _duplicate_unlocks.json)
        self._dup_masters:      dict = {}    # raw -> (base, key) user-chosen master
                                             # of a duplicate group (overrides the
                                             # default first-occurrence primary;
                                             # persisted to _duplicate_masters.json)
        self._monster_keys:     set = set()  # all (base, key) enemy-name-table strings
        self._unsealed_monsters: set = set() # (base, key) the user opted to translate
                                             # (persisted to _monster_unsealed.json)
        self._seal_monsters:    bool = True  # global default: keep monster names sealed
                                             # (from editor_config "seal_monster_names")
        self._unlocked_uncensor: set = set() # (base, key) uncensor-only slots the user
                                             # unlocked to translate (persisted to
                                             # _uncensor_unlocked.json). See UNCENSOR_ENTRIES.
        self._glossary:         dict = {}   # english_term -> german_term
        self._recently_edited:  list = []   # [(file, entry_num, offset, preview), ...]
        self._autosave_id:      str  = None
        self._overall_progress: dict = {}   # filename -> (done, total)
        self._config:           dict = {}   # editor config (game_dir, etc.)
        self._mockup_photo:     object = None  # keep reference to prevent GC
        self._portrait_photo:   object = None  # portrait image reference
        self._portrait_visible: bool = False
        self._last_trans_tab:   int  = 0       # track which translation tab was active
        # Identity of the string currently loaded into the translation widgets.
        # Used to gate _commit_current so it won't save stale/empty widget
        # contents against a string the widgets haven't been populated for yet
        # (happens on file/entry load, search jumps, and goto-primary).
        self._displayed_key:    tuple = (None, None, None)

        self._load_config()
        self._apply_data_dirs()     # honor persisted source/translations folders
        self._spell = None          # SpellManager (built by _init_spell)
        self._spell_after = None    # debounce id for live re-underline
        self._init_spell()
        self._load_glossary()
        self._load_dup_unlocks()
        self._load_dup_masters()
        self._seal_monsters = bool(self._config.get("seal_monster_names", True))
        self._load_monster_unseals()
        self._build_monster_index()
        self._load_uncensor_unlocks()
        self._load_game_font()
        self._load_tag_colors()
        self._build_ui()
        self._apply_treeview_style()
        self._load_file_list()
        self.after(100, self._init_shared_index)
        self._schedule_autosave()

    def _load_game_font(self):
        """Load BoF4 game font for the dialog mockup:
          1. Advances from vwf_config.txt — read directly from disk,
             no DLL hookup.
          2. Glyph PNGs from localization_editor/font/ — per-slot 24x24
             bitmaps exported by localization_editor/tools/font_editor/font_editor_gui.py.

        Each filename is `{slot_hex}_{name}.png`, where the slot hex
        = game char byte − 0x20. Umlauts at slots 0x68..0x6E map to
        ä/ö/ü/ß via _UMLAUT_BYTE_TO_UNICODE.
        """
        self._gfont_advance = {}
        self._gfont_bearing = {}
        self._gfont_images = {}
        self._gfont_pil = {}
        self._gfont_scale = 1.0
        self._game_line_max_px = DEFAULT_LINE_MAX_PX
        self._gfont_left = {}
        self._gfont_top = {}

        if not _HAS_PIL:
            return

        # 1) Parse vwf_config. Prefer the editor-only override at
        # localization_editor/configs/vwf_config_editor.txt if present;
        # fall back to the project-root vwf_config.txt otherwise. The
        # DLL + repacker always read the project-root file, so keeping
        # the editor copy separate lets the user tune preview widths
        # without affecting packing or the live in-game VWF.
        active_path = (VWF_EDITOR_PATH
                       if os.path.exists(VWF_EDITOR_PATH)
                       else VWF_CONFIG_PATH)
        self._active_vwf_path = active_path
        # Lines look like either:
        #      0x41 = A : 19            (advance only)
        #      0x41 = A : 3 : 17        (bearing : advance)
        # Bearing = byte+0 (draw offset), advance = byte+1 (cursor step).
        if os.path.exists(active_path):
            try:
                with open(active_path, encoding="utf-8") as f:
                    for line in f:
                        # Strip trailing comment first.
                        line = line.split("#", 1)[0].strip()
                        if not line:
                            continue
                        m = re.match(
                            r"0x([0-9A-Fa-f]+)\s*=\s*\S+\s*:\s*"
                            r"(-?\d+)(?:\s*:\s*(-?\d+))?",
                            line)
                        if not m:
                            continue
                        b = int(m.group(1), 16)
                        n1 = int(m.group(2))
                        n2 = m.group(3)
                        if n2 is not None:
                            bearing = n1
                            adv = int(n2)
                        else:
                            bearing = 0
                            adv = n1
                        uni = _byte_to_unicode(b)
                        if uni:
                            self._gfont_advance[uni] = adv
                            self._gfont_bearing[uni] = bearing
            except Exception:
                pass

        # 2) Load per-glyph PNGs from localization_editor/font/.
        # Filename format: `NN_name.png` where NN is the slot hex
        # (= char_byte - 0x20).
        if os.path.isdir(FONT_DIR):
            fname_re = re.compile(r"^([0-9A-Fa-f]{1,2})_.+\.png$")
            for fname in os.listdir(FONT_DIR):
                m = fname_re.match(fname)
                if not m:
                    continue
                slot = int(m.group(1), 16)
                char_byte = slot + 0x20
                uni = _byte_to_unicode(char_byte)
                if not uni:
                    continue
                png_path = os.path.join(FONT_DIR, fname)
                try:
                    img = Image.open(png_path).convert("RGBA")
                    # Find the leftmost column with any non-transparent
                    # pixel so we can render glyphs aligned by their
                    # visible left edge, not the PNG cell left edge.
                    w, h = img.size
                    px = img.load()
                    left = 0
                    for x in range(w):
                        col_has = False
                        for y in range(h):
                            if px[x, y][3] > 0:
                                col_has = True
                                break
                        if col_has:
                            left = x
                            break
                    self._gfont_pil[uni] = img
                    self._gfont_left[uni] = left
                    self._gfont_top[uni] = 0
                except Exception:
                    continue

    def _init_shared_index(self):
        self.status_var.set("Building shared string index...")
        self.update()
        self._build_shared_index()
        self._calculate_overall_progress()
        n = len(self._shared_index)
        self.status_var.set(f"Ready. {n} strings shared across files.")
        self._populate_tree()
        self._update_overall_progress_display()

    # ── UI construction ────────────────────────────────────────────────────────
    def _on_tb_scroll(self, lo, hi):
        """xscrollcommand for the middle action-button canvas: keep the scrollbar
        in sync and auto-hide it whenever the buttons fit (so it's invisible at
        normal resolution and only appears when they overflow on a small window)."""
        self._tb_hbar.set(lo, hi)
        if float(lo) <= 0.0 and float(hi) >= 1.0:
            if self._tb_hbar_shown:
                self._tb_hbar.pack_forget()
                self._tb_hbar_shown = False
        else:
            if not self._tb_hbar_shown:
                self._tb_hbar.pack(side=tk.BOTTOM, fill=tk.X)
                self._tb_hbar_shown = True

    def _sync_toolbar_scroll(self, event=None):
        """Keep the middle button canvas' scrollregion + height in step with the
        action_bar, stretching it to fill the canvas when the buttons fit (no
        scroll) or keeping its natural width and scrolling when they overflow."""
        c = self._tb_canvas
        c.configure(scrollregion=c.bbox("all"))
        req_h = self._tb_inner.winfo_reqheight()
        if c.winfo_height() != req_h:
            c.configure(height=req_h)
        want = max(c.winfo_width(), self._tb_inner.winfo_reqwidth())
        if c.itemcget(self._tb_win, "width") != str(want):
            c.itemconfigure(self._tb_win, width=want)

    def _build_ui(self):
        # ── Toolbar ──────────────────────────────────────────────────────────
        # File/Entry (left) and the Tools/Dictionaries/… group (right) stay
        # FIXED. Only the draggable action-button group in the MIDDLE scrolls
        # horizontally, so on screens narrower than the toolbar's natural width
        # (~1280px) no button is unreachable. A horizontal scrollbar under the
        # toolbar auto-appears only when the middle buttons overflow.
        tb_outer = tk.Frame(self, bg=self.BG2)
        tb_outer.pack(fill=tk.X)
        tb = tk.Frame(tb_outer, bg=self.BG2, pady=5)
        tb.pack(side=tk.TOP, fill=tk.X)

        def lbl(parent, text):
            tk.Label(parent, text=text, bg=self.BG2, fg=self.FG,
                     font=("Segoe UI", 9)).pack(side=tk.LEFT, padx=(8, 2))

        lbl(tb, "File:")
        self.file_var = tk.StringVar()
        self.file_cb  = ttk.Combobox(tb, textvariable=self.file_var,
                                      width=30, state="readonly",
                                      font=("Consolas", 9))
        self.file_cb.pack(side=tk.LEFT, padx=2)
        self.file_cb.bind("<<ComboboxSelected>>", self._on_file_select)
        self.file_cb.bind("<KeyPress>", self._on_file_key)
        # KeyRelease for Up/Down: readonly combobox cycles its value on
        # those keys natively; we hook the release to then load the file.
        self.file_cb.bind("<KeyRelease-Up>", self._on_file_arrow)
        self.file_cb.bind("<KeyRelease-Down>", self._on_file_arrow)
        self._file_key_buf = ""
        self._file_key_after = None
        self._last_loaded_file = None

        lbl(tb, "Entry:")
        self.entry_var = tk.StringVar()
        self.entry_cb  = ttk.Combobox(tb, textvariable=self.entry_var,
                                       width=35, state="readonly",
                                       font=("Consolas", 9))
        self.entry_cb.pack(side=tk.LEFT, padx=2)
        self.entry_cb.bind("<<ComboboxSelected>>", self._on_entry_select)

        # Directory picker buttons
        tk.Button(tb, text="Source\u2026", bg=self.BG2, fg="#8888AA",
                  relief=tk.FLAT, font=("Segoe UI", 8), padx=4,
                  command=self._pick_source_dir).pack(side=tk.RIGHT, padx=2)
        tk.Button(tb, text="Translations\u2026", bg=self.BG2, fg="#8888AA",
                  relief=tk.FLAT, font=("Segoe UI", 8), padx=4,
                  command=self._pick_trans_dir).pack(side=tk.RIGHT, padx=2)

        # (The old "Advanced space" toggle + substitute-byte box were removed:
        # 0x20 is now rendered at its proper VWF width by the engine renderer
        # patch, so ' ' always repacks as plain ASCII 0x20 \u2014 no glyph
        # substitution needed anymore.)

        # "Dictionaries" — spell-check manager (install/enable Hunspell dicts),
        # right-anchored in the same always-visible column as Advanced space.
        tk.Button(tb, text="Dictionaries", command=self._open_dictionaries,
                  bg=self.BG2, fg="#A0C0D0", relief=tk.FLAT,
                  font=("Segoe UI", 8), padx=4).pack(side=tk.RIGHT, padx=2)

        # "Tools" — launch the companion GUIs (each runs as its own process so
        # it doesn't block the editor). See _launch_tool.
        tools_mb = tk.Menubutton(tb, text="Tools ▾", bg=self.BG2,
                                 fg="#C0B0D0", relief=tk.FLAT,
                                 font=("Segoe UI", 8), padx=4)
        tools_menu = tk.Menu(tools_mb, tearoff=0)
        # Tool paths are relative to the tools/ folder (this editor's parent),
        # since the companion tools are siblings of this editor folder.
        tools_menu.add_command(
            label="Font  Extract / Repack",
            command=lambda: self._launch_tool(
                "font_editor/font_editor_gui.py", "Font Editor",
                ["bof4_font_editor_gui.exe", "font_editor_gui.exe"]))
        tools_menu.add_command(
            label="Graphics  Extract / Repack",
            command=lambda: self._launch_tool(
                "graphics_encoder/bof4_gui.py", "Graphics Editor",
                ["bof4_graphics_unpacker_repacker_gui.exe", "bof4_gui.exe"]))
        tools_menu.add_separator()
        tools_menu.add_command(
            label="Roll back GOG exe (Update 7 → 6)…",
            command=self._gog_rollback)
        tools_mb.config(menu=tools_menu)
        tools_mb.pack(side=tk.RIGHT, padx=2)

        ttk.Separator(tb, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=10)

        # progress label — fixed at the far right (packed RIGHT *before* the
        # scroll canvas so the expanding canvas can't squeeze it out).
        self.progress_lbl = tk.Label(tb, text="", bg=self.BG2,
                                      fg=self.ACC_TRA, font=("Consolas", 9))
        self.progress_lbl.pack(side=tk.RIGHT, padx=12)

        # ── Draggable action buttons (the ONLY scrolling part) ───────────────
        # These live in their own sub-frame so they can be reordered by drag &
        # drop. Grab a button and drag it left/right to move it; the layout is
        # saved to editor_config ("toolbar_order") on drop and restored on the
        # next launch. A plain click (no drag) still fires the button's action.
        # (spec key, label, command, colour) — key is the stable persistence id.
        self._action_specs = [
            ("save",         "Save",               self._save,                "#4C6A4C"),
            ("export",       "Export",             self._export,              "#4A6080"),
            ("export_all",   "Export All",         self._export_all_modified, "#4A6080"),
            ("open_items",   "Open Items",         self._open_todo_browser,   "#5A5A4A"),
            ("codes_missing","Missing CR Codes",   self._open_missing_codes,  "#5A5A4A"),
            ("codes_added",  "Added CR Codes",     self._open_added_codes,    "#5A5A4A"),
            ("dump_text",    "Dump Text…",    self._dump_text_from_dat,  "#4A6A5A"),
            ("sync_all",     "Sync All",           self._sync_translations,   "#5A4A6A"),
            ("repack_all",   "Repack All",         self._repack_all,          "#6A4A4A"),
            ("out_dir",      "Out Dir…",      self._pick_repack_out_dir, "#6A4A4A"),
            ("gfx_dir",      "Gfx Dir…",      self._pick_repack_import_dir, "#6A4A4A"),
            ("locate_dats",  "Locate DATs…",  self._locate_pristine,     "#4A6A4A"),
            ("restore_dat",  "Restore DAT",        self._restore_pristine,    "#6A5A3A"),
            ("seal_monsters","Monster Seal",       self._toggle_seal_monsters,"#5A4A4A"),
            ("glossary",     "Glossary",           self._open_glossary_editor,"#5A5A4A"),
            ("find_replace", "Find/Replace",       self._open_batch_replace,  "#4A5A5A"),
            ("import_psx",   "Import PSX",          self._open_psx_import,     "#4A6A6A"),
            ("import_missing_psx", "Import missing PSX", self._import_missing_psx, "#4A6A6A"),
            # Buttons removed from the toolbar (functionality kept, methods intact):
            #   Copy Src   -> _copy_source_to_trans
            #   Repack     -> _repack_current  (Repack All kept)
            #   Todo Export-> _export_todo     (superseded by Open Items)
            #   Clean PSX  -> _clean_psx_codes
            # VWF buttons likewise kept as methods (_reload_vwf / _open_vwf_editor).
            ("colors",       "Colors…",       self._open_color_editor,   "#4A4A6A"),
        ]
        # The action buttons live inside a canvas that fills the gap between the
        # fixed left combos and the fixed right group; when they overflow it, the
        # canvas scrolls (and only it — everything else stays put).
        self._tb_canvas = tk.Canvas(tb, bg=self.BG2, highlightthickness=0,
                                    bd=0, height=1)
        self._tb_canvas.pack(side=tk.LEFT, fill=tk.X, expand=True)
        self._tb_hbar = ttk.Scrollbar(tb_outer, orient=tk.HORIZONTAL,
                                      command=self._tb_canvas.xview)
        self._tb_hbar_shown = False
        self._tb_canvas.configure(xscrollcommand=self._on_tb_scroll)
        self.action_bar = tk.Frame(self._tb_canvas, bg=self.BG2)
        self._tb_inner = self.action_bar
        self._tb_win = self._tb_canvas.create_window(
            (0, 0), window=self.action_bar, anchor="nw")
        self.action_bar.bind("<Configure>", self._sync_toolbar_scroll)
        self._tb_canvas.bind("<Configure>", self._sync_toolbar_scroll)
        # Shift+wheel scrolls the buttons horizontally when they overflow.
        for _w in (self._tb_canvas, self.action_bar):
            _w.bind("<Shift-MouseWheel>",
                    lambda e: self._tb_canvas.xview_scroll(
                        int(-e.delta / 120), "units")
                    if self._tb_hbar_shown else None)
        self._action_btns = {}
        self._drag_btn = None
        self._build_action_buttons()

        # ── Search + Filter bar ─────────────────────────────────────────────
        sb = tk.Frame(self, bg=self.BG2, pady=3)
        sb.pack(fill=tk.X)

        tk.Label(sb, text="  Search:", bg=self.BG2, fg=self.FG,
                 font=("Segoe UI", 9)).pack(side=tk.LEFT)
        self.search_var = tk.StringVar()
        self.search_entry = ttk.Combobox(sb, textvariable=self.search_var,
                                          font=("Consolas", 9), width=30)
        self.search_entry.pack(side=tk.LEFT, padx=4)
        self.search_entry.bind("<Return>", lambda e: self._search_next())
        self._search_history = []

        def sbtn(text, cmd):
            tk.Button(sb, text=text, command=cmd, bg="#4C5052", fg=self.FG,
                      relief=tk.FLAT, padx=6, pady=1,
                      font=("Segoe UI", 8)).pack(side=tk.LEFT, padx=2)

        sbtn("Find Next", self._search_next)
        sbtn("Find Prev", self._search_prev)

        # Case-sensitive toggle: when on, "nichts" won't match "NICHTS".
        self._match_case_var = tk.BooleanVar(value=False)
        tk.Checkbutton(sb, text="Match Case", variable=self._match_case_var,
                       bg=self.BG2, fg=self.FG, selectcolor=self.BG,
                       activebackground=self.BG2, activeforeground=self.FG2,
                       font=("Segoe UI", 8),
                       command=self._on_match_case_change).pack(side=tk.LEFT, padx=4)

        self._search_hits  = []
        self._search_pos   = -1

        self.search_result_lbl = tk.Label(sb, text="", bg=self.BG2,
                                           fg="#888888", font=("Consolas", 8))
        self.search_result_lbl.pack(side=tk.LEFT, padx=8)

        # Filter
        ttk.Separator(sb, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=8)
        tk.Label(sb, text="Filter:", bg=self.BG2, fg=self.FG,
                 font=("Segoe UI", 9)).pack(side=tk.LEFT, padx=(0, 4))
        self._filter_var = tk.StringVar(value=FILTER_ALL)
        for fval, ftxt in [(FILTER_ALL, "All"), (FILTER_UNTRANSLATED, "Todo"),
                           (FILTER_TRANSLATED, "Done"), (FILTER_DRAFT, "Draft"),
                           (FILTER_REVIEWED, "Review"), (FILTER_FINAL, "Final"),
                           (FILTER_MISSING, "Missing Codes"),
                           (FILTER_EXTRA, "Extra Codes"),
                           (FILTER_SOURCE_CHANGED, "Src Changed"),
                           (FILTER_NOTED, "Noted ✎")]:
            tk.Radiobutton(sb, text=ftxt, variable=self._filter_var, value=fval,
                           bg=self.BG2, fg=self.FG, selectcolor=self.BG,
                           activebackground=self.BG2, activeforeground=self.FG2,
                           font=("Segoe UI", 8), indicatoron=0, padx=6, pady=1,
                           relief=tk.FLAT, overrelief=tk.RAISED,
                           command=self._on_filter_change
                           ).pack(side=tk.LEFT, padx=1)

        # Overall progress
        self._overall_lbl = tk.Label(sb, text="", bg=self.BG2,
                                      fg="#AAAAAA", font=("Consolas", 8))
        self._overall_lbl.pack(side=tk.RIGHT, padx=8)

        # Keyboard shortcuts
        self.bind("<Control-s>", lambda e: self._save())
        self.bind("<Control-Return>", lambda e: self._next_string())
        self.bind("<Control-f>", lambda e: (self.search_entry.focus_set(),
                                             self.search_entry.select_range(0, tk.END)))
        self.bind("<Control-u>", lambda e: self._next_untrans())
        self.bind("<Control-g>", lambda e: self._open_glossary_editor())

        # ── Header info bar ──────────────────────────────────────────────────
        self.hdr_lbl = tk.Label(self, text="", bg=self.BG2, fg=self.FG,
                                 font=("Consolas", 9, "bold"),
                                 anchor=tk.W, padx=8, pady=4)
        self.hdr_lbl.pack(fill=tk.X)

        # ── Main paned window ────────────────────────────────────────────────
        vpw = tk.PanedWindow(self, orient=tk.VERTICAL, bg=self.BG,
                              sashwidth=6, sashrelief=tk.FLAT, handlesize=0)
        vpw.pack(fill=tk.BOTH, expand=True, padx=0, pady=0)

        # String list
        list_frame = tk.Frame(vpw, bg=self.BG)
        vpw.add(list_frame, minsize=140, height=200)
        self._build_string_list(list_frame)

        # Bottom: source | translation
        hpw = tk.PanedWindow(vpw, orient=tk.HORIZONTAL, bg=self.BG,
                              sashwidth=6, sashrelief=tk.FLAT, handlesize=0)
        vpw.add(hpw, minsize=250)

        src_frame   = tk.Frame(hpw, bg=self.BG)
        trans_frame = tk.Frame(hpw, bg=self.BG)
        hpw.add(src_frame,   minsize=300)
        hpw.add(trans_frame, minsize=300)

        self._build_source_panel(src_frame)
        self._build_trans_panel(trans_frame)

        # ── Status bar ───────────────────────────────────────────────────────
        sbar = tk.Frame(self, bg=self.BG2, pady=3)
        sbar.pack(fill=tk.X)

        self.status_var = tk.StringVar(value="Open a file to begin.")
        tk.Label(sbar, textvariable=self.status_var, bg=self.BG2, fg=self.FG,
                 font=("Consolas", 9), anchor=tk.W).pack(side=tk.LEFT, padx=8)

        # Pristine-reference indicator (left of autosave). Click = re-check.
        self._pristine_var = tk.StringVar(value="")
        self._pristine_lbl = tk.Label(sbar, textvariable=self._pristine_var, bg=self.BG2,
                                      fg="#888888", font=("Consolas", 8), cursor="hand2")
        self._pristine_lbl.pack(side=tk.RIGHT, padx=8)
        self._pristine_lbl.bind("<Button-1>", lambda e: self._update_pristine_status())
        self.after(300, self._update_pristine_status)

        self._autosave_lbl = tk.Label(sbar, text="", bg=self.BG2,
                                       fg="#555555", font=("Consolas", 8))
        self._autosave_lbl.pack(side=tk.LEFT, padx=8)

        nav = tk.Frame(sbar, bg=self.BG2)
        nav.pack(side=tk.RIGHT, padx=6)

        def navbtn(text, cmd):
            tk.Button(nav, text=text, command=cmd, bg="#4C5052", fg=self.FG,
                      relief=tk.FLAT, padx=8, pady=1,
                      font=("Segoe UI", 9)).pack(side=tk.LEFT, padx=2)

        navbtn("Prev",               self._prev_string)
        navbtn("Next",               self._next_string)
        navbtn("Next Untranslated",  self._next_untrans)

    def _build_string_list(self, parent):
        cols = ("offset", "st", "flag", "source", "translation")
        # `extended` allows Ctrl+click and Shift+click to pick multiple
        # rows. The Status button then cycles the status of ALL selected
        # rows in one shot, which is how bulk Draft→Review→Final flips
        # are meant to work.
        self.tree = ttk.Treeview(parent, columns=cols, show="headings",
                                  selectmode="extended")
        self.tree.heading("offset",      text="Offset")
        self.tree.heading("st",          text="St")
        self.tree.heading("flag",        text="Fl")
        self.tree.heading("source",      text="Source")
        self.tree.heading("translation", text="Translation")
        self.tree.column("offset",      width=90,  stretch=False, anchor=tk.CENTER)
        self.tree.column("st",          width=40,  stretch=False, anchor=tk.CENTER)
        self.tree.column("flag",        width=26,  stretch=False, anchor=tk.CENTER)
        self.tree.column("source",      width=500, minwidth=200)
        self.tree.column("translation", width=500, minwidth=200)

        vsb = ttk.Scrollbar(parent, orient=tk.VERTICAL,   command=self.tree.yview)
        hsb = ttk.Scrollbar(parent, orient=tk.HORIZONTAL, command=self.tree.xview)
        self.tree.configure(yscrollcommand=vsb.set, xscrollcommand=hsb.set)

        self.tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        vsb.pack(side=tk.RIGHT,  fill=tk.Y)
        hsb.pack(side=tk.BOTTOM, fill=tk.X)

        # Row color scheme. User overrides via editor_config["tag_colors"]
        # are resolved by self._tag_color() — the "Colors…" toolbar button
        # opens a picker that edits the live tag_configure and persists.
        self._apply_tag_colors()
        self.tree.bind("<<TreeviewSelect>>", self._on_str_select)

    def _build_source_panel(self, parent):
        hdr = tk.Frame(parent, bg=self.BG2)
        hdr.pack(fill=tk.X)
        tk.Label(hdr, text="  SOURCE  (read-only)", bg=self.BG2, fg=self.ACC_SRC,
                 font=("Segoe UI", 9, "bold"), pady=4).pack(side=tk.LEFT)

        nb = ttk.Notebook(parent)
        nb.pack(fill=tk.BOTH, expand=True)

        raw_f = tk.Frame(nb, bg=self.BG3)
        fmt_f = tk.Frame(nb, bg=self.BG3)
        clean_f = tk.Frame(nb, bg=self.BG3)
        nb.add(raw_f, text="  Raw  ")
        nb.add(fmt_f, text="  Formatted  ")
        nb.add(clean_f, text="  Clean  ")

        self.src_fmt = self._make_text(fmt_f, editable=False,
                                        font=("Segoe UI", 11))
        self.src_raw = self._make_text(raw_f, editable=False,
                                        font=("Consolas", 10), fg="#AAAAAA")
        self.src_clean = self._make_text(clean_f, editable=False,
                                          font=("Segoe UI", 11))

        self._setup_clean_tags(self.src_fmt)
        self._add_ctrl_tags(self.src_raw)

    def _build_trans_panel(self, parent):
        hdr = tk.Frame(parent, bg=self.BG2)
        hdr.pack(fill=tk.X)
        tk.Label(hdr, text="  TRANSLATION", bg=self.BG2, fg=self.ACC_TRA,
                 font=("Segoe UI", 9, "bold"), pady=4).pack(side=tk.LEFT)
        self.trans_offset_lbl = tk.Label(hdr, text="", bg=self.BG2,
                                          fg="#777777", font=("Consolas", 9))
        self.trans_offset_lbl.pack(side=tk.RIGHT, padx=8)

        # Status flag button
        self._status_btn = tk.Button(hdr, text="[Status]", command=self._cycle_status,
                                      bg="#4C5052", fg="#AAAAAA", relief=tk.FLAT,
                                      padx=6, pady=1, font=("Consolas", 8))
        self._status_btn.pack(side=tk.RIGHT, padx=4)

        # "Go to Primary" bar
        self._goto_bar = tk.Frame(parent, bg="#3A2A4A", pady=4)
        # Action buttons are packed side=LEFT with before=self._goto_lbl so they
        # sit at the LEFT edge and stay visible no matter how narrow the window is;
        # the explanatory label follows them and WRAPS (never pushes a button off
        # the right edge). See _refresh_goto_bar.
        self._goto_lbl = tk.Label(self._goto_bar, text="", bg="#3A2A4A",
                                   fg="#CCAADD", font=("Consolas", 9),
                                   wraplength=560, justify=tk.LEFT, anchor=tk.W)
        self._goto_lbl.pack(side=tk.LEFT, padx=8)
        self._goto_btn = tk.Button(self._goto_bar, text="Go to Primary",
                                    command=self._on_goto_primary,
                                    bg="#5A4A6A", fg="white", relief=tk.FLAT,
                                    padx=12, pady=2, cursor="hand2",
                                    font=("Segoe UI", 9, "bold"))
        self._goto_btn.pack(side=tk.LEFT, padx=4)
        self._goto_target = None
        # Secondary action on the same bar: unlock a duplicate for independent
        # editing, or re-lock an unlocked one back into the shared group.
        self._dup_btn = tk.Button(self._goto_bar, text="",
                                   command=self._on_toggle_dup_unlock,
                                   bg="#4A6A5A", fg="white", relief=tk.FLAT,
                                   padx=12, pady=2, cursor="hand2",
                                   font=("Segoe UI", 9, "bold"))
        self._dup_btn_target = None   # (base, key, raw) for the toggle action
        # Tertiary action: choose THIS occurrence as the group's master (the one
        # Sync All propagates from), or clear that choice.
        self._master_btn = tk.Button(self._goto_bar, text="",
                                      command=self._on_toggle_master,
                                      bg="#5A4A6A", fg="white", relief=tk.FLAT,
                                      padx=12, pady=2, cursor="hand2",
                                      font=("Segoe UI", 9, "bold"))
        self._master_btn_target = None   # (base, key, raw) for the master toggle
        # Monster-name seal/unseal toggle (shares the goto bar).
        self._seal_btn = tk.Button(self._goto_bar, text="",
                                   command=self._on_toggle_seal,
                                   bg="#4A6A5A", fg="white", relief=tk.FLAT,
                                   padx=12, pady=2, cursor="hand2",
                                   font=("Segoe UI", 9, "bold"))
        self._seal_btn_target = None     # (base, key) for the seal toggle
        # Duplicate-group BROWSER: from any member of a shared group, list ALL its
        # copies and jump to each (the reverse of "Go to primary") so it's easy to
        # survey the group and decide which occurrence should be the master.
        self._dupsbrowse_btn = tk.Button(self._goto_bar, text="",
                                          command=self._on_browse_dups,
                                          bg="#4A5A6A", fg="white", relief=tk.FLAT,
                                          padx=12, pady=2, cursor="hand2",
                                          font=("Segoe UI", 9, "bold"))
        self._dupsbrowse_target = None   # raw group key for the browser

        nb = ttk.Notebook(parent)
        # Packed later (after the always-visible bottom bars) so that shrinking
        # the window steals height from the editor, never from the info/footer.

        edit_f   = tk.Frame(nb, bg="#1A2218")
        fmt_f    = tk.Frame(nb, bg="#1E2A1E")
        nb.add(edit_f,   text="  Raw  ")
        nb.add(fmt_f,    text="  Formatted  ")
        # Preview tab removed — the glyph-width mockup never matched the game and
        # added no value. _build_dialog_mockup / _update_dialog_mockup / render are
        # kept but dormant (guarded by self._mockup_enabled).

        self.trans_edit = self._make_text(edit_f, editable=True,
                                           font=("Segoe UI", 11),
                                           bg="#1A2218")
        self._add_ctrl_tags(self.trans_edit)
        # Misspelled-word marker: red underline (Tk has no wavy underline, so a
        # coloured straight underline stands in for the "red squiggly").
        self.trans_edit.tag_configure("misspell", underline=True,
                                      underlinefg="#FF5555")
        self.trans_edit.bind("<KeyRelease>", self._on_trans_key)
        self.trans_edit.bind("<Button-3>", self._on_trans_right_click)

        self.trans_fmt = self._make_text(fmt_f, editable=True,
                                          font=("Segoe UI", 11),
                                          bg="#1E2A1E")
        self._setup_clean_tags(self.trans_fmt)
        self.trans_fmt.tag_configure("elided", elide=True)
        # Same misspelled-word marker as the Raw editor so red underlines and
        # right-click suggestions work in the Formatted view too.
        self.trans_fmt.tag_configure("misspell", underline=True,
                                     underlinefg="#FF5555")
        self.trans_fmt.bind("<Key>", self._on_fmt_key)
        self.trans_fmt.bind("<KeyRelease>", self._on_fmt_key_release)
        self.trans_fmt.bind("<Button-3>", self._on_trans_right_click)

        nb.bind("<<NotebookTabChanged>>", self._on_trans_tab_change)
        self._trans_nb = nb

        # Preview tab disabled — mockup widgets are not built; all mockup
        # update/render paths early-return on this flag.
        self._mockup_enabled = False

        # ── Validation + glossary warnings ───────────────────────────────────
        # Errors (red) — control-code mismatches that break in-game rendering.
        # Remarks (yellow) — byte-length / glossary notes; informational only.
        self._warn_frame = tk.Frame(parent, bg=self.BG2)
        self._warn_err_lbl = tk.Label(self._warn_frame, text="", bg="#3A2020",
                                       fg="#CC8888", font=("Consolas", 8),
                                       anchor=tk.W, wraplength=600, justify=tk.LEFT)
        self._warn_rem_lbl = tk.Label(self._warn_frame, text="", bg="#3A3520",
                                       fg="#D8C070", font=("Consolas", 8),
                                       anchor=tk.W, wraplength=600, justify=tk.LEFT)

        # ── Escape-token quick-insert bar ───────────────────────────────────
        # One click drops a hard-to-type control token at the Raw editor's
        # cursor (see _insert_escape). Bottom-anchored so it stays visible.
        esc_bar = tk.Frame(parent, bg=self.BG2)
        tk.Label(esc_bar, text=" Insert:", bg=self.BG2, fg="#888888",
                 font=("Segoe UI", 8)).pack(side=tk.LEFT)
        for token in ("\\x7f", "\\x80", "\\x81", "\\x82", "\\x83",
                      "~", "\\n", "[02]"):
            tk.Button(esc_bar, text=token,
                      command=lambda t=token: self._insert_escape(t),
                      bg="#333A44", fg="#B0C4D4", relief=tk.FLAT,
                      font=("Consolas", 9), padx=6, pady=0).pack(
                          side=tk.LEFT, padx=2, pady=2)

        # ── Translation Memory suggestions ──────────────────────────────────
        # When a string is selected, show the most similar SOURCE strings you've
        # already translated (across all files) so you can reuse prior work with
        # one click. Double-click a row (or Enter) to drop its translation into
        # the Raw editor. Fuzzy-matched via difflib; refreshed after each save.
        tm_frame = tk.Frame(parent, bg=self.BG2)
        tm_hdr = tk.Frame(tm_frame, bg=self.BG2)
        tm_hdr.pack(fill=tk.X)
        tk.Label(tm_hdr, text=" Memory:", bg=self.BG2, fg="#88AA88",
                 font=("Segoe UI", 8)).pack(side=tk.LEFT)
        self._tm_hint = tk.Label(tm_hdr, text="(similar strings you've translated — double-click to apply)",
                                 bg=self.BG2, fg="#556655", font=("Segoe UI", 8))
        self._tm_hint.pack(side=tk.LEFT)
        tm_body = tk.Frame(tm_frame, bg=self.BG2)
        tm_body.pack(fill=tk.X)
        self._tm_list = tk.Listbox(tm_body, height=3, bg="#141C14", fg="#B8D0B8",
                                   font=("Consolas", 8), relief=tk.FLAT,
                                   selectbackground="#2E4A2E",
                                   activestyle="none", highlightthickness=0)
        tm_sb = ttk.Scrollbar(tm_body, orient=tk.VERTICAL,
                              command=self._tm_list.yview)
        self._tm_list.configure(yscrollcommand=tm_sb.set)
        self._tm_list.pack(side=tk.LEFT, fill=tk.X, expand=True)
        tm_sb.pack(side=tk.RIGHT, fill=tk.Y)
        self._tm_list.bind("<Double-Button-1>", self._apply_tm_suggestion)
        self._tm_list.bind("<Return>", self._apply_tm_suggestion)
        self._tm_suggestions = []   # parallel to listbox rows: translation text

        # ── Translator note (per string) ────────────────────────────────────
        # A free-text note attached to the current string, stored in a
        # `<file>_notes.json` sidecar (never touches the translation JSON).
        # Surfaced by the "Noted" filter. Commits on focus-out / Enter.
        notes_bar = tk.Frame(parent, bg=self.BG2)
        tk.Label(notes_bar, text=" Note:", bg=self.BG2, fg="#C0A860",
                 font=("Segoe UI", 8)).pack(side=tk.LEFT)
        self._note_var = tk.StringVar()
        self._note_entry = tk.Entry(notes_bar, textvariable=self._note_var,
                                    bg="#22201A", fg="#D8C88A",
                                    insertbackground="#D8C88A", relief=tk.FLAT,
                                    font=("Segoe UI", 9))
        self._note_entry.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(4, 6))
        self._note_entry.bind("<FocusOut>", self._commit_note)
        self._note_entry.bind("<Return>",   self._commit_note)

        # ── Footer ──────────────────────────────────────────────────────────
        footer = tk.Frame(parent, bg=self.BG2)
        tk.Label(footer,
                 text="Raw: type with \\n  |  Fmt: Enter=line break  |  Ctrl+S=save  Ctrl+U=next todo",
                 bg=self.BG2, fg="#555555", font=("Segoe UI", 8),
                 anchor=tk.W, padx=6, pady=2).pack(side=tk.LEFT)
        self.byte_lbl = tk.Label(footer, text="", bg=self.BG2,
                                  fg="#666666", font=("Consolas", 8), padx=8)
        self.byte_lbl.pack(side=tk.RIGHT)

        # Pack the persistent bottom bars FIRST (footer at the very bottom, then
        # the warning frame, then the insert bar) so they reserve their height
        # before the editor notebook claims the rest. This keeps the info/warning
        # field visible no matter how far the window is shrunk vertically.
        footer.pack(side=tk.BOTTOM, fill=tk.X)
        self._warn_frame.pack(side=tk.BOTTOM, fill=tk.X)   # empty => 0 px tall
        esc_bar.pack(side=tk.BOTTOM, fill=tk.X)
        notes_bar.pack(side=tk.BOTTOM, fill=tk.X)
        tm_frame.pack(side=tk.BOTTOM, fill=tk.X)
        nb.pack(side=tk.TOP, fill=tk.BOTH, expand=True)

    def _build_dialog_mockup(self, parent):
        """Build the dialog box mockup preview.

        Pixel-perfect render path: composite the textbox + portrait + glyphs
        + advance symbol at native game resolution (480x104), then scale up
        with NEAREST so 1 game pixel = N screen pixels — no antialiasing.
        Glyph PNGs come from localization_editor/font/, advances come from
        vwf_config.txt.
        """
        self._mockup_outer = tk.Frame(parent, bg="#0A0A1A", padx=10, pady=10)
        self._mockup_outer.pack(fill=tk.BOTH, expand=True)

        # Info + portrait toggle row
        top_f = tk.Frame(self._mockup_outer, bg="#0A0A1A")
        top_f.pack(fill=tk.X, pady=(0, 4))
        tk.Label(top_f, text="Dialog Box Preview", bg="#0A0A1A", fg="#666688",
                 font=("Segoe UI", 8)).pack(side=tk.LEFT)
        self._portrait_toggle_btn = tk.Button(
            top_f, text="Portrait: OFF", bg="#2A2A4A", fg="#8888AA",
            relief=tk.FLAT, font=("Consolas", 8), padx=6,
            command=self._toggle_portrait)
        self._portrait_toggle_btn.pack(side=tk.RIGHT)
        self._portrait_flip_btn = tk.Button(
            top_f, text="Flip: OFF", bg="#2A2A4A", fg="#8888AA",
            relief=tk.FLAT, font=("Consolas", 8), padx=6,
            command=self._toggle_portrait_flip)
        self._portrait_flip_btn.pack(side=tk.RIGHT, padx=(0, 4))

        # State
        self._portrait_visible = False
        self._portrait_flipped = False
        self._mockup_pages = []
        self._mockup_current_page = 0
        # User-tweakable render parameters (game pixels). Loaded from
        # editor_config["preview"] if present, otherwise defaults. See
        # the tweak panel below.
        prev_cfg = (self._read_editor_config() or {}).get("preview", {})
        self._pv_text_left   = int(prev_cfg.get("text_left",
                                                MOCKUP_TEXT_INSET))
        self._pv_text_right  = int(prev_cfg.get("text_right",
                                                MOCKUP_TEXT_INSET))
        self._pv_text_top    = int(prev_cfg.get("text_top",
                                                MOCKUP_TEXT_TOP))
        self._pv_line_height = int(prev_cfg.get("line_height",
                                                MOCKUP_LINE_HEIGHT))
        self._pv_adv_fudge   = int(prev_cfg.get("advance_fudge", 0))
        # Kept for code paths still referencing it (overflow checks etc.).
        self._line_max_var = tk.IntVar(value=DEFAULT_LINE_MAX_PX)

        # Load textbox + advance symbol + portrait (PIL Image objects, kept
        # for compositing — we re-render on every update).
        self._textbox_img = None
        self._advance_img = None
        self._portrait_img = None
        if _HAS_PIL:
            if os.path.exists(TEXTBOX_PATH):
                try:
                    self._textbox_img = Image.open(TEXTBOX_PATH).convert("RGBA")
                except Exception:
                    pass
            if os.path.exists(ADVANCE_PATH):
                try:
                    self._advance_img = Image.open(ADVANCE_PATH).convert("RGBA")
                except Exception:
                    pass
            if os.path.exists(PORTRAIT_PATH):
                try:
                    pimg = Image.open(PORTRAIT_PATH).convert("RGBA")
                    # Scale portrait to fit textbox height while keeping
                    # native aspect (NEAREST so it stays pixel-art crisp).
                    target_h = MOCKUP_NATIVE_H
                    p_scale = target_h / pimg.height
                    p_w = max(1, int(pimg.width * p_scale))
                    self._portrait_img = pimg.resize(
                        (p_w, target_h), Image.NEAREST)
                except Exception:
                    pass

        # Native canvas size = textbox size; the displayed canvas is scaled.
        if self._textbox_img:
            self._mockup_native_w = self._textbox_img.width
            self._mockup_native_h = self._textbox_img.height
        else:
            self._mockup_native_w = MOCKUP_NATIVE_W
            self._mockup_native_h = MOCKUP_NATIVE_H

        disp_w = self._mockup_native_w * MOCKUP_DISPLAY_SCALE
        disp_h = self._mockup_native_h * MOCKUP_DISPLAY_SCALE

        self._mockup_canvas = tk.Canvas(
            self._mockup_outer, width=disp_w, height=disp_h,
            bg="#0A0A1A", highlightthickness=0)
        self._mockup_canvas.pack()
        # The canvas always shows ONE composited PhotoImage covering the
        # entire textbox area; we recompute it on every render.
        self._mockup_photo = None
        self._mockup_image_id = self._mockup_canvas.create_image(
            0, 0, anchor=tk.NW, image=None, tags="composite")

        # Page navigation
        nav_f = tk.Frame(self._mockup_outer, bg="#0A0A1A")
        nav_f.pack(fill=tk.X, pady=(4, 0))
        self._mockup_page_lbl = tk.Label(
            nav_f, text="Page 1/1", bg="#0A0A1A", fg="#666688",
            font=("Consolas", 8))
        self._mockup_page_lbl.pack(side=tk.LEFT)
        self._mockup_prev_btn = tk.Button(
            nav_f, text="< Prev", bg="#2A2A4A", fg="#8888AA",
            relief=tk.FLAT, font=("Consolas", 8),
            command=lambda: self._mockup_go_page(-1))
        self._mockup_prev_btn.pack(side=tk.LEFT, padx=4)
        self._mockup_next_btn = tk.Button(
            nav_f, text="Next >", bg="#2A2A4A", fg="#8888AA",
            relief=tk.FLAT, font=("Consolas", 8),
            command=lambda: self._mockup_go_page(1))
        self._mockup_next_btn.pack(side=tk.LEFT, padx=4)

        # Warning area
        self._mockup_warn = tk.Label(
            self._mockup_outer, text="", bg="#0A0A1A",
            fg="#CC6666", font=("Consolas", 8),
            anchor=tk.W, wraplength=disp_w, justify=tk.LEFT)
        self._mockup_warn.pack(fill=tk.X, pady=(2, 0))

        # Tweak panel — lets the user dial in where text starts, where it
        # cuts off, vertical offset, line height, and a per-glyph advance
        # fudge that's added to every `cursor += advance` step. All values
        # in game pixels. Saved to editor_config["preview"] on change.
        tweak = tk.LabelFrame(self._mockup_outer, text=" Preview tweaks ",
                              bg="#0A0A1A", fg="#8888AA",
                              font=("Segoe UI", 8),
                              padx=6, pady=4, bd=1, relief=tk.GROOVE)
        tweak.pack(fill=tk.X, pady=(6, 0))

        self._pv_vars = {
            "text_left":     tk.IntVar(value=self._pv_text_left),
            "text_right":    tk.IntVar(value=self._pv_text_right),
            "text_top":      tk.IntVar(value=self._pv_text_top),
            "line_height":   tk.IntVar(value=self._pv_line_height),
            "advance_fudge": tk.IntVar(value=self._pv_adv_fudge),
        }
        labels = [
            ("text_left",     "Text left",  "px from textbox left edge "
                                            "(or after portrait when on)"),
            ("text_right",    "Text right", "px from textbox right edge "
                                            "(cut-off marker)"),
            ("text_top",      "Text top",   "px from top of textbox"),
            ("line_height",   "Line height","px between lines"),
            ("advance_fudge", "Adv fudge",  "extra px added after every "
                                            "glyph's advance (tight spacing "
                                            "= negative, wider = positive)"),
        ]
        for i, (key, label, tip) in enumerate(labels):
            tk.Label(tweak, text=label + ":", bg="#0A0A1A", fg="#AAAAAA",
                     font=("Consolas", 8)
                     ).grid(row=i, column=0, sticky=tk.W, padx=(0, 4))
            sp = tk.Spinbox(tweak, from_=-32, to=200, width=5,
                            textvariable=self._pv_vars[key],
                            bg="#2A2A4A", fg="#CCCCCC",
                            buttonbackground="#2A2A4A",
                            font=("Consolas", 8), relief=tk.FLAT,
                            command=self._on_tweak_change)
            sp.grid(row=i, column=1, sticky=tk.W, padx=2)
            sp.bind("<KeyRelease>", lambda _e: self._on_tweak_change())
            tk.Label(tweak, text=tip, bg="#0A0A1A", fg="#555577",
                     font=("Segoe UI", 8)
                     ).grid(row=i, column=2, sticky=tk.W, padx=(6, 0))
        tk.Button(tweak, text="Reset",
                  command=self._reset_tweaks,
                  bg="#4A4A5A", fg="#CCCCCC",
                  relief=tk.FLAT, font=("Segoe UI", 8)
                  ).grid(row=0, column=3, rowspan=2, padx=(12, 0),
                         sticky=tk.NS)

    def _on_tweak_change(self):
        """Called when any preview-tweak spinbox changes. Reads the
        IntVars back into instance attributes, saves to editor_config,
        and re-renders."""
        try:
            self._pv_text_left   = int(self._pv_vars["text_left"].get())
            self._pv_text_right  = int(self._pv_vars["text_right"].get())
            self._pv_text_top    = int(self._pv_vars["text_top"].get())
            self._pv_line_height = max(
                1, int(self._pv_vars["line_height"].get()))
            self._pv_adv_fudge   = int(self._pv_vars["advance_fudge"].get())
        except Exception:
            return
        cfg = self._read_editor_config() or {}
        cfg["preview"] = {
            "text_left":     self._pv_text_left,
            "text_right":    self._pv_text_right,
            "text_top":      self._pv_text_top,
            "line_height":   self._pv_line_height,
            "advance_fudge": self._pv_adv_fudge,
        }
        self._write_editor_config(cfg)
        self._render_mockup_page()

    def _reset_tweaks(self):
        """Restore the built-in defaults for the preview tweak panel."""
        self._pv_vars["text_left"].set(MOCKUP_TEXT_INSET)
        self._pv_vars["text_right"].set(MOCKUP_TEXT_INSET)
        self._pv_vars["text_top"].set(MOCKUP_TEXT_TOP)
        self._pv_vars["line_height"].set(MOCKUP_LINE_HEIGHT)
        self._pv_vars["advance_fudge"].set(0)
        self._on_tweak_change()

    def _toggle_portrait(self):
        """Toggle portrait overlay and shift text accordingly."""
        self._portrait_visible = not self._portrait_visible
        if self._portrait_visible:
            self._portrait_toggle_btn.config(text="Portrait: ON", fg="#88CC88")
        else:
            self._portrait_toggle_btn.config(text="Portrait: OFF", fg="#8888AA")
        self._render_mockup_page()

    def _toggle_portrait_flip(self):
        """Mirror the portrait and anchor it to the right side instead of
        the left. Text returns to the left margin when flipped."""
        self._portrait_flipped = not self._portrait_flipped
        if self._portrait_flipped:
            self._portrait_flip_btn.config(text="Flip: ON", fg="#88CC88")
        else:
            self._portrait_flip_btn.config(text="Flip: OFF", fg="#8888AA")
        self._render_mockup_page()

    def _make_text(self, parent, editable, font, fg=None, bg=None):
        w = tk.Text(parent,
                    bg=bg or self.BG3,
                    fg=fg or self.FG2,
                    wrap=tk.WORD,
                    font=font,
                    state=tk.NORMAL if editable else tk.DISABLED,
                    relief=tk.FLAT,
                    padx=10, pady=8,
                    insertbackground="white",
                    selectbackground=self.SEL,
                    undo=editable,
                    spacing1=2, spacing3=2)
        w.pack(fill=tk.BOTH, expand=True)
        return w

    def _add_ctrl_tags(self, widget):
        widget.tag_configure("ctrl",     foreground=self.CTRL_FG,
                             font=("Consolas", 9))
        widget.tag_configure("at_break", foreground=self.AT_FG,
                             font=("Consolas", 9, "bold"))

    def _setup_clean_tags(self, widget):
        for code, color in TEXT_COLORS.items():
            widget.tag_configure(f"tc_{code}", foreground=color)
        widget.tag_configure("sep",         foreground="#555555",
                             font=("Consolas", 8))
        widget.tag_configure("pagebreak",   foreground="#888844",
                             font=("Consolas", 8))
        widget.tag_configure("pause",       foreground="#888888",
                             font=("Segoe UI", 10, "italic"))
        widget.tag_configure("char_name",   foreground="#E0A040",
                             font=("Segoe UI", 11, "bold"))
        widget.tag_configure("placeholder", foreground="#8888CC",
                             font=("Consolas", 9, "italic"))
        widget.tag_configure("speaker",     foreground="#CC88DD",
                             font=("Segoe UI", 9, "bold"))
        # Character emitted by a glyph-emit opcode ([09:22:XX]/[18:22:XX]) —
        # shown as the actual letter so the line reads correctly, tinted so it
        # is clearly an opcode-drawn glyph and not literal text.
        widget.tag_configure("glyphemit",   foreground="#66CC99",
                             font=("Segoe UI", 11, "bold"))

    def _apply_treeview_style(self):
        s = ttk.Style(self)
        s.theme_use("clam")
        s.configure("Treeview",
                    background=self.BG2, foreground=self.FG,
                    fieldbackground=self.BG2, font=("Consolas", 9),
                    rowheight=22)
        s.configure("Treeview.Heading",
                    background="#4C5052", foreground="#CCCCCC",
                    font=("Segoe UI", 9, "bold"))
        s.map("Treeview", background=[("selected", self.SEL)])
        s.configure("TCombobox", fieldbackground=self.BG2, background=self.BG2,
                    foreground=self.FG, selectbackground=self.SEL)
        s.configure("TNotebook", background=self.BG2)
        s.configure("TNotebook.Tab", background=self.BG2, foreground=self.FG,
                    padding=[8, 3])
        s.map("TNotebook.Tab", background=[("selected", self.BG3)],
              foreground=[("selected", self.ACC_SRC)])

    @staticmethod
    def _dup_raw(s):
        """Grouping / duplicate key for a string. Multi-item slots group on
        their FULL tail (`full_raw`), NOT the leading-item text shown in the
        UI. Two slots with the same leading item but different tails — e.g.
        the four "Lvl 1 [07:06] Magic" element variants, identical in English
        because the element is a runtime placeholder but translated as
        Feuer/Wind/Wasser/Erde — therefore stay INDEPENDENT and are never
        locked or auto-overwritten as duplicates of one another."""
        return s.get("full_raw") or s["raw"]

    # ── Shared string index (cross-file deduplication) ───────────────────────
    def _build_shared_index(self):
        idx = {}
        lead = {}   # (base, leading_item) -> [(base, key), ...]  multi-item only
        for fname in sorted(os.listdir(DUMP_DIR)):
            if not fname.endswith(".txt"):
                continue
            base = fname[:-4]
            path = os.path.join(DUMP_DIR, fname)
            _, entries = parse_dump(path)
            for e in entries:
                for s in e["strings"]:
                    raw = self._dup_raw(s)
                    key = self._key(e["num"], s["offset"])
                    # A sealed monster name is kept as English on purpose — it must
                    # not join any duplicate group (neither source another string
                    # nor be overwritten by one), so leave it out of the index.
                    if self._is_monster_sealed(base, key) or self._is_uncensor_sealed(base, key):
                        continue
                    idx.setdefault(raw, []).append((base, key))
                    # Manual-duplicate targets inherit from their own master
                    # (a different offset), so they must NOT ride the
                    # leading-sibling autofill — otherwise the fall-through
                    # line they happen to share (e.g. "Powers up after 1000
                    # kills.") would clobber the inherited value. Keep them out
                    # of the lead index entirely (both directions).
                    if s.get("multi_item") and (base, key) not in MANUAL_DUP_MAP:
                        lead.setdefault((base, s["raw"]), []).append((base, key))
        self._shared_index = {
            raw: locs for raw, locs in idx.items()
            if len(locs) > 1
        }
        self._primary_loc = {
            raw: locs[0] for raw, locs in self._shared_index.items()
        }
        # User-chosen master overrides the default first-occurrence primary, so
        # display/auto-fill/re-lock all source from the chosen location. Ignore a
        # stale override whose target is no longer in the group.
        for raw, master in self._dup_masters.items():
            locs = self._shared_index.get(raw)
            if locs and master in locs:
                self._primary_loc[raw] = master
        # Per-file index of multi-item slots that share an identical LEADING
        # item (the visible line). Used only to auto-FILL empty siblings —
        # never to lock or overwrite — so translating one genuinely-repeated
        # line (e.g. "Light healing of [07:00] target(s).") seeds the rest.
        self._lead_index = {k: v for k, v in lead.items() if len(v) > 1}

    def _sync_translations(self):
        self._commit_current()
        self._save()
        if not self._shared_index:
            self.status_var.set("Building shared index...")
            self.update()
            self._build_shared_index()

        all_full = {}     # base -> {key: {"source","translation"}}
        all_trans = {}    # base -> {key: translation}  (view for match logic)
        if os.path.isdir(TRANS_DIR):
            for fname in os.listdir(TRANS_DIR):
                if fname.endswith(".json") and not _is_aux_json(fname):
                    base = fname[:-5]
                    tp = os.path.join(TRANS_DIR, fname)
                    full = load_translations_full(tp)
                    all_full[base]  = full
                    all_trans[base] = {k: r["translation"] for k, r in full.items()}

        synced = 0
        modified = set()
        for raw, all_locations in self._shared_index.items():
            # Drop user-unlocked locations from the group entirely: they are
            # independent, so they neither contribute a source value nor get
            # overwritten. If unlocking leaves <2 members there is nothing to
            # sync for this raw.
            locations = [loc for loc in all_locations
                         if (loc[0], loc[1]) not in self._unlocked_dups]
            if len(locations) < 2:
                continue
            # User-chosen master: propagate deterministically FROM that location
            # to every other (non-unlocked) member — overrides the edit
            # heuristic below. If the master is present but still empty, skip the
            # group so we never blank the others with an empty master.
            master = self._dup_masters.get(raw)
            if master and master in locations:
                mbase, mkey = master
                mt = all_trans.get(mbase, {}).get(mkey, "")
                if not mt.strip():
                    continue
                for base, key in locations:
                    t = all_trans.setdefault(base, {})
                    if t.get(key, "") != mt:
                        t[key] = mt
                        modified.add(base)
                        synced += 1
                continue
            # Bidirectional: use whichever location has a translation as the
            # source. Prefer a translation that DIFFERS from the source raw
            # (= a real edit) over one that equals it (= an auto-fill).
            # Without this preference, ctrl-only entries — where every
            # location's auto-fill equals the raw — would have the
            # first-alphabetical file's auto-fill picked as `source_trans`
            # and propagated over real edits made in later-alphabetical
            # files, silently reverting user work. Among multiple edits,
            # the first non-equal-to-raw wins; if none differ from raw,
            # fall back to the first non-empty translation (preserves
            # behavior for normal text entries where edits naturally
            # differ from the English source anyway).
            edit_trans = ""
            any_trans  = ""
            for base, key in locations:
                t = all_trans.get(base, {}).get(key, "")
                if not t.strip():
                    continue
                if not any_trans:
                    any_trans = t
                if t != raw and not edit_trans:
                    edit_trans = t
                    break
            source_trans = edit_trans or any_trans
            if not source_trans:
                continue
            for base, key in locations:
                t = all_trans.setdefault(base, {})
                if t.get(key, "") != source_trans:
                    t[key] = source_trans
                    modified.add(base)
                    synced += 1

        for base in modified:
            path = os.path.join(TRANS_DIR, base + ".json")
            # Re-serialize in content-addressed form, preserving each key's
            # existing source (None for keys this file never had a source for;
            # they re-fill from the live row when that file is next opened+saved).
            full = all_full.get(base, {})
            out = {k: {"source": full.get(k, {}).get("source"), "translation": t}
                   for k, t in all_trans[base].items()}
            with open(path, "w", encoding="utf-8") as f:
                json.dump(out, f, ensure_ascii=False, indent=2, sort_keys=True)

        self._load_translations()
        self._tm_dirty = True        # synced translations -> refresh TM corpus
        self._populate_tree()
        self._calculate_overall_progress()
        self._update_overall_progress_display()
        self._refresh_current_widget()
        self.status_var.set(f"Synced {synced} translations across {len(modified)} files.")

    def _get_shared_count(self, raw):
        locs = self._shared_index.get(raw)
        return len(locs) if locs else 0

    # ── Duplicate unlock (edit a shared duplicate independently) ─────────────
    def _load_dup_unlocks(self):
        """Load the set of duplicate locations the user has unlocked for
        independent editing. Stored as a flat list of "base|key" strings."""
        self._unlocked_dups = set()
        if not os.path.exists(DUP_UNLOCK_PATH):
            return
        try:
            with open(DUP_UNLOCK_PATH, encoding="utf-8") as f:
                data = json.load(f)
            for item in data:
                base, sep, key = str(item).partition("|")
                if sep and key:
                    self._unlocked_dups.add((base, key))
        except Exception:
            self._unlocked_dups = set()

    def _save_dup_unlocks(self):
        try:
            os.makedirs(TRANS_DIR, exist_ok=True)
            data = sorted(f"{b}|{k}" for (b, k) in self._unlocked_dups)
            with open(DUP_UNLOCK_PATH, "w", encoding="utf-8") as f:
                json.dump(data, f, ensure_ascii=False, indent=2)
        except Exception:
            pass

    def _is_dup_unlocked(self, base, key):
        return (base, key) in self._unlocked_dups

    # ── Duplicate master (choose which occurrence drives the group) ───────────
    def _load_dup_masters(self):
        """Load the user-chosen master per duplicate group. Stored as
        {raw: "base|key"}."""
        self._dup_masters = {}
        if not os.path.exists(DUP_MASTER_PATH):
            return
        try:
            with open(DUP_MASTER_PATH, encoding="utf-8") as f:
                data = json.load(f)
            for raw, loc in data.items():
                base, sep, key = str(loc).partition("|")
                if sep and key:
                    self._dup_masters[raw] = (base, key)
        except Exception:
            self._dup_masters = {}

    def _save_dup_masters(self):
        try:
            os.makedirs(TRANS_DIR, exist_ok=True)
            data = {raw: f"{b}|{k}" for raw, (b, k) in self._dup_masters.items()}
            with open(DUP_MASTER_PATH, "w", encoding="utf-8") as f:
                json.dump(data, f, ensure_ascii=False, indent=2, sort_keys=True)
        except Exception:
            pass

    def _set_dup_master(self, raw, base, key):
        """Make (base,key) the master of its duplicate group, or clear the
        override when it's already the master. Rebuilds the shared index so the
        new master takes effect everywhere (display, auto-fill, Sync All)."""
        loc = (base, key)
        if self._dup_masters.get(raw) == loc:
            self._dup_masters.pop(raw, None)          # toggle off -> back to default
            msg = "Master cleared: back to first-occurrence default."
        else:
            self._dup_masters[raw] = loc
            # A master is the shared source, not an independent location — if this
            # location was previously unlocked, rejoin it to the group.
            if loc in self._unlocked_dups:
                self._unlocked_dups.discard(loc)
                self._save_dup_unlocks()
            msg = "Master set: Sync All now sources every duplicate from here."
        self._save_dup_masters()
        self._build_shared_index()                    # repoint _primary_loc
        self._populate_tree()
        self._refresh_current_widget()
        self.status_var.set(msg)

    def _toggle_dup_unlock(self, base, key, raw):
        """Flip a duplicate location between shared (locked) and independent
        (unlocked). When unlocking, seed the location's own translation from
        the shared primary so the user starts from the current text instead
        of an empty box. When re-locking, pull the primary's value back so
        the location rejoins the shared group cleanly."""
        loc = (base, key)
        # Manual duplicates carry an explicit master; fall back to the
        # text-keyed primary for ordinary shared strings.
        manual_master = MANUAL_DUP_MAP.get(loc)
        if loc in self._unlocked_dups:
            # Re-lock: rejoin the shared group; restore the primary's value.
            self._unlocked_dups.discard(loc)
            prim = manual_master or self._primary_loc.get(raw)
            if prim:
                pbase, pkey = prim
                prim_trans = self._read_trans_value(pbase, pkey)
                if prim_trans is not None:
                    self._write_trans_value(base, key, prim_trans)
            self.status_var.set("Re-locked: rejoined shared translation.")
        else:
            # Unlock: seed from primary (or current) so editing starts populated.
            self._unlocked_dups.add(loc)
            if not self._read_trans_value(base, key):
                prim = manual_master or self._primary_loc.get(raw)
                if prim:
                    pbase, pkey = prim
                    seed = self._read_trans_value(pbase, pkey)
                    if seed:
                        self._write_trans_value(base, key, seed)
            self.status_var.set(
                "Unlocked: this duplicate now has its own translation.")
        self._save_dup_unlocks()

    # ── Monster (enemy) name seal ────────────────────────────────────────────
    def _build_monster_index(self):
        """Scan the AREAB*.DAT dumps for the enemy-name tables (entries the
        extractor tagged with a per-name byte cap) and record every (base, key)
        so monster names can be sealed globally. Cheap: only the ~131 AREAB
        files, parsed once at startup."""
        self._monster_keys = set()
        if not os.path.isdir(DUMP_DIR):
            return
        for fname in os.listdir(DUMP_DIR):
            if not fname.endswith(".txt") or not fname.upper().startswith("AREAB"):
                continue
            base = fname[:-4]
            try:
                _, entries = parse_dump(os.path.join(DUMP_DIR, fname))
            except Exception:
                continue
            for e in entries:
                if "max_byte_len" not in e:      # only the enemy-name table entries
                    continue
                for s in e["strings"]:
                    self._monster_keys.add((base, self._key(e["num"], s["offset"])))

    def _load_monster_unseals(self):
        self._unsealed_monsters = set()
        if not os.path.exists(MONSTER_UNSEAL_PATH):
            return
        try:
            with open(MONSTER_UNSEAL_PATH, encoding="utf-8") as f:
                data = json.load(f)
            for item in data:
                base, sep, key = str(item).partition("|")
                if sep and key:
                    self._unsealed_monsters.add((base, key))
        except Exception:
            self._unsealed_monsters = set()

    def _save_monster_unseals(self):
        try:
            os.makedirs(TRANS_DIR, exist_ok=True)
            data = sorted(f"{b}|{k}" for (b, k) in self._unsealed_monsters)
            with open(MONSTER_UNSEAL_PATH, "w", encoding="utf-8") as f:
                json.dump(data, f, ensure_ascii=False, indent=2)
        except Exception:
            pass

    def _is_monster_name(self, base, key):
        return (base, key) in self._monster_keys

    def _is_monster_sealed(self, base, key):
        """A monster name is sealed (locked as English) when the global seal is
        on and the user hasn't explicitly unsealed this one."""
        return (self._seal_monsters
                and (base, key) in self._monster_keys
                and (base, key) not in self._unsealed_monsters)

    def _toggle_monster_seal(self, base, key):
        """Per-name: unseal a monster name for translation, or re-seal it."""
        loc = (base, key)
        if loc in self._unsealed_monsters:
            self._unsealed_monsters.discard(loc)
            self.status_var.set("Sealed: monster name kept as English.")
        else:
            self._unsealed_monsters.add(loc)
            self.status_var.set("Unsealed: this monster name is now translatable.")
        self._save_monster_unseals()
        self._build_shared_index()          # sealed names leave/rejoin the dup system
        self._populate_tree()

    # ── Uncensor-only slots (AREAM027): locked as English until unlocked ─────────
    def _load_uncensor_unlocks(self):
        self._unlocked_uncensor = set()
        if not os.path.exists(UNCENSOR_UNLOCK_PATH):
            return
        try:
            with open(UNCENSOR_UNLOCK_PATH, encoding="utf-8") as f:
                data = json.load(f)
            for item in data:
                base, sep, key = str(item).partition("|")
                if sep and key:
                    self._unlocked_uncensor.add((base, key))
        except Exception:
            self._unlocked_uncensor = set()

    def _save_uncensor_unlocks(self):
        try:
            os.makedirs(TRANS_DIR, exist_ok=True)
            data = sorted(f"{b}|{k}" for (b, k) in self._unlocked_uncensor)
            with open(UNCENSOR_UNLOCK_PATH, "w", encoding="utf-8") as f:
                json.dump(data, f, ensure_ascii=False, indent=2)
        except Exception:
            pass

    def _is_uncensor_entry(self, base, key):
        return (base, key) in UNCENSOR_ENTRIES

    def _is_uncensor_sealed(self, base, key):
        """An uncensor-only slot is sealed (locked as English) until the user
        explicitly unlocks it — it only renders when the scene is played
        uncensored (d3d9 flag censor_fix_aream027)."""
        return ((base, key) in UNCENSOR_ENTRIES
                and (base, key) not in self._unlocked_uncensor)

    def _toggle_uncensor_unlock(self, base, key):
        """Per-slot: unlock an uncensor line for translation, or re-lock it."""
        loc = (base, key)
        if loc in self._unlocked_uncensor:
            self._unlocked_uncensor.discard(loc)
            self.status_var.set("Locked: uncensor-only line kept as English.")
        else:
            self._unlocked_uncensor.add(loc)
            self.status_var.set("Unlocked: this uncensor-only line is now translatable.")
        self._save_uncensor_unlocks()
        self._build_shared_index()
        self._populate_tree()
        self._refresh_current_widget()

    def _toggle_seal_monsters(self):
        """Global toggle: seal ALL monster names (default) or allow translating
        them. Per-name unseals are remembered either way."""
        self._commit_current()
        self._save()
        self._seal_monsters = not self._seal_monsters
        self._config["seal_monster_names"] = self._seal_monsters
        self._save_config()
        self._build_shared_index()
        self._populate_tree()
        self._refresh_current_widget()
        self.status_var.set(
            "Monster names SEALED (English by default; unseal individually)."
            if self._seal_monsters else
            "Monster names UNSEALED globally (all translatable).")

    def _read_trans_value(self, base, key):
        """Read a single translation value from disk (or the in-memory map
        if it's the current file). Returns None if the file/key is absent."""
        cur_base = (self.current_file[:-4]
                    if self.current_file and self.current_file.endswith(".txt")
                    else self.current_file)
        if base == cur_base:
            return self.translations.get(key)
        path = os.path.join(TRANS_DIR, base + ".json")
        if not os.path.exists(path):
            return None
        try:
            return load_translations_json(path).get(key)
        except Exception:
            return None

    def _write_trans_value(self, base, key, value):
        """Write a single translation value to disk (and the in-memory map
        if it's the current file)."""
        cur_base = (self.current_file[:-4]
                    if self.current_file and self.current_file.endswith(".txt")
                    else self.current_file)
        if base == cur_base:
            if value:
                self.translations[key] = value
            elif key in self.translations:
                del self.translations[key]
            self._dirty = True
            return
        path = os.path.join(TRANS_DIR, base + ".json")
        data = {}
        if os.path.exists(path):
            try:
                data = load_translations_json(path)
            except Exception:
                data = {}
        if value:
            data[key] = value
        elif key in data:
            del data[key]
        try:
            write_translations_json(path, data)
        except Exception:
            pass

    def _is_duplicate(self, raw, current_base, current_key):
        # A sealed monster name stands alone (kept English) — never a duplicate.
        if self._is_monster_sealed(current_base, current_key):
            return False, None, None
        # Manual duplicate override (cross-offset; source text may differ).
        # Locked to its master unless the user unlocked this exact location.
        man = MANUAL_DUP_MAP.get((current_base, current_key))
        if man is not None:
            if (current_base, current_key) in self._unlocked_dups:
                return False, None, None
            return True, man[0], man[1]
        if raw not in self._primary_loc:
            return False, None, None
        prim_base, prim_key = self._primary_loc[raw]
        if prim_base == current_base and prim_key == current_key:
            return False, None, None
        # User-unlocked duplicates are treated as independent (editable) —
        # they keep their own translation and are skipped by all sync paths.
        if (current_base, current_key) in self._unlocked_dups:
            return False, None, None
        return True, prim_base, prim_key

    # ── File dropdown type-ahead + arrow-key navigation ──────────────────────
    def _on_file_arrow(self, event):
        # The readonly combobox has already advanced its value via its own
        # class binding (fires on KeyPress, before this release fires). We
        # just need to load the newly selected file. Guarded to avoid
        # reloading the same file twice in a row.
        fname = self.file_var.get()
        if fname and fname != self._last_loaded_file:
            self._last_loaded_file = fname
            self._on_file_select()

    def _on_file_key(self, event):
        # Type-ahead: letter keys jump to the first file starting with the
        # accumulated prefix. Arrow keys are handled in _on_file_arrow.
        if event.keysym in ("Up", "Down"):
            return  # let native cycling happen; _on_file_arrow handles load
        ch = event.char.lower()
        if not ch or not ch.isalpha():
            return
        if self._file_key_after:
            self.after_cancel(self._file_key_after)
        self._file_key_buf += ch
        self._file_key_after = self.after(600, self._reset_file_key)
        prefix = self._file_key_buf.upper()
        files = list(self.file_cb["values"])
        for i, f in enumerate(files):
            if f.upper().startswith(prefix):
                self.file_cb.current(i)
                self._on_file_select()
                break

    def _reset_file_key(self):
        self._file_key_buf = ""
        self._file_key_after = None

    # ── Filter ────────────────────────────────────────────────────────────────
    def _on_filter_change(self):
        self._filter_mode = self._filter_var.get()
        self._populate_tree()

    # ── Directory pickers ────────────────────────────────────────────────────
    def _apply_data_dirs(self):
        """Honor persisted source (text_dump) / translations folders from config
        so the editor works when run from a copied/standalone location where the
        default GAME_ROOT/text_dump and GAME_ROOT/translations don't exist.
        Only applies a stored path if it still points at a real folder."""
        global DUMP_DIR, TRANS_DIR
        cd = self._config.get("dump_dir")
        if cd and os.path.isdir(cd):
            DUMP_DIR = cd
        ct = self._config.get("trans_dir")
        if ct and os.path.isdir(ct):
            TRANS_DIR = ct
        try:
            os.makedirs(DUMP_DIR, exist_ok=True)
            os.makedirs(TRANS_DIR, exist_ok=True)
        except Exception:
            pass

    def _pick_source_dir(self):
        from tkinter import filedialog
        global DUMP_DIR
        d = filedialog.askdirectory(
            title="Select Source Text Directory (text_dump/)",
            initialdir=DUMP_DIR if os.path.isdir(DUMP_DIR) else GAME_ROOT)
        if d:
            DUMP_DIR = d
            self._config["dump_dir"] = d     # remember across launches
            self._save_config()
            self.title(f"BoF4 Localization Editor \u2014 src: {os.path.basename(d)}")
            self._load_file_list()

    def _pick_trans_dir(self):
        from tkinter import filedialog
        global TRANS_DIR
        d = filedialog.askdirectory(
            title="Select Translations Directory (translations/)",
            initialdir=TRANS_DIR if os.path.isdir(TRANS_DIR) else GAME_ROOT)
        if d:
            TRANS_DIR = d
            self._config["trans_dir"] = d    # remember across launches
            self._save_config()
            self.title(f"BoF4 Localization Editor \u2014 trans: {os.path.basename(d)}")
            self._load_file_list()

    # ── PSX German import ──────────────────────────────────────────────────
    def _open_psx_import(self):
        """Modal dialog: pick PSX disc folders + output dir, run extractor.

        Wraps `extract_psx_german.run_extraction()`. Paths are remembered
        in editor_config.json so the next invocation pre-fills them.
        """
        from tkinter import filedialog, messagebox
        import threading

        # Defaults: prefer values saved last time, fall back to project
        # layout, fall back to the editor's own TRANS_DIR.
        cfg = self._read_editor_config()
        psx_block = cfg.get('psx_import', {}) if isinstance(cfg, dict) else {}
        default_de = psx_block.get('psx_de_bin') or os.path.join(
            GAME_ROOT, "bof4_psx", "breath of fire 4", "BIN")
        default_us = psx_block.get('psx_us_bin') or os.path.join(
            GAME_ROOT, "bof4_psx", "bof4_us", "BIN")
        default_pc = psx_block.get('pc_dat_dir') or os.path.join(
            GAME_ROOT, "DAT_backup")
        default_out = psx_block.get('trans_dir') or TRANS_DIR

        dlg = tk.Toplevel(self)
        dlg.title("Import German text from PSX")
        dlg.configure(bg=self.BG)
        dlg.transient(self)
        dlg.grab_set()
        dlg.geometry("780x520")

        path_vars = {}
        rows = [
            ('psx_de_bin',
             "PSX German disc 'BIN' folder",
             default_de,
             "(folder containing WORLD/, SYSTEM/, BATTLE/, SCENARIO/ "
             "from the German Twisted-Phoenix patched disc)"),
            ('psx_us_bin',
             "PSX US disc 'BIN' folder",
             default_us,
             "(same layout, US release — used as a matching anchor)"),
            ('pc_dat_dir',
             "PC DAT folder",
             default_pc,
             "(GoG/Steam DAT/ or DAT_backup/ — the English source)"),
            ('trans_dir',
             "Output translations folder",
             default_out,
             "(JSON files will be written here, one per matched DAT)"),
        ]

        form = tk.Frame(dlg, bg=self.BG, padx=12, pady=10)
        form.pack(fill=tk.X)
        for key, label, default, hint in rows:
            row = tk.Frame(form, bg=self.BG)
            row.pack(fill=tk.X, pady=4)
            tk.Label(row, text=label + ":", bg=self.BG, fg=self.FG,
                     font=("Segoe UI", 9, "bold"),
                     width=32, anchor='w').pack(side=tk.LEFT)
            var = tk.StringVar(value=default)
            path_vars[key] = var
            tk.Entry(row, textvariable=var, bg=self.BG2, fg=self.FG,
                     font=("Consolas", 9), width=58, relief=tk.FLAT
                     ).pack(side=tk.LEFT, fill=tk.X, expand=True, padx=4)

            def make_picker(v=var, t=label):
                def pick():
                    d = filedialog.askdirectory(
                        title=f"Select {t}",
                        initialdir=v.get() if os.path.isdir(v.get())
                                   else GAME_ROOT)
                    if d:
                        v.set(d)
                return pick
            tk.Button(row, text="Browse", command=make_picker(),
                      bg="#4A5A5A", fg="white", relief=tk.FLAT,
                      padx=8, font=("Segoe UI", 9)).pack(side=tk.LEFT)
            tk.Label(form, text="    " + hint, bg=self.BG,
                     fg=self.ACC_TRA, font=("Segoe UI", 8),
                     anchor='w').pack(fill=tk.X)

        # ── Output area ───────────────────────────────────────────
        log_frame = tk.Frame(dlg, bg=self.BG, padx=12, pady=4)
        log_frame.pack(fill=tk.BOTH, expand=True)
        tk.Label(log_frame, text="Progress:", bg=self.BG, fg=self.FG,
                 font=("Segoe UI", 9, "bold")).pack(anchor='w')
        log_text = tk.Text(log_frame, bg="#1B1B1B", fg="#D0D0D0",
                            font=("Consolas", 9), wrap='word',
                            height=12, relief=tk.FLAT)
        log_text.pack(fill=tk.BOTH, expand=True, pady=2)
        log_text.insert('end',
            "Ready. Click Extract to scan PSX EMI files and write "
            "translations into the output folder.\n")
        log_text.configure(state='disabled')

        # ── Options row ───────────────────────────────────────────
        # Default: don't overwrite — the matcher's "if not entry: keep"
        # logic protects user-edited German strings. When the user is
        # specifically refreshing legacy entries (e.g. ones imported
        # before the PSX->PC remap landed), tick this and re-run.
        opts_row = tk.Frame(dlg, bg=self.BG, padx=12, pady=2)
        opts_row.pack(fill=tk.X)
        overwrite_var = tk.BooleanVar(value=False)
        tk.Checkbutton(
            opts_row,
            text="Overwrite existing translations (refresh stale imports)",
            variable=overwrite_var,
            bg=self.BG, fg=self.FG2, selectcolor=self.BG3,
            activebackground=self.BG, activeforeground=self.FG2,
            font=("Segoe UI", 9)).pack(side=tk.LEFT, anchor='w')

        # ── Bottom button bar ─────────────────────────────────────
        bar = tk.Frame(dlg, bg=self.BG, padx=12, pady=8)
        bar.pack(fill=tk.X)
        status_var = tk.StringVar(value="")
        tk.Label(bar, textvariable=status_var, bg=self.BG,
                 fg=self.ACC_SRC, font=("Segoe UI", 9)).pack(side=tk.LEFT)
        run_btn  = tk.Button(bar, text="Close", bg="#4C5052", fg="white",
                              relief=tk.FLAT, padx=14,
                              command=dlg.destroy)
        run_btn.pack(side=tk.RIGHT, padx=4)
        extract_btn = tk.Button(bar, text="Extract", bg="#4C6A4C",
                                 fg="white", relief=tk.FLAT, padx=14,
                                 font=("Segoe UI", 9, "bold"))
        extract_btn.pack(side=tk.RIGHT, padx=4)

        def append_log(msg):
            log_text.configure(state='normal')
            log_text.insert('end', msg + "\n")
            log_text.see('end')
            log_text.configure(state='disabled')
            log_text.update_idletasks()

        def do_extract():
            paths = {k: v.get().strip() for k, v in path_vars.items()}
            for k, label, _, _ in rows:
                if not paths[k]:
                    messagebox.showerror(
                        "Missing path", f"Please choose: {label}",
                        parent=dlg)
                    return
            for k in ('psx_de_bin', 'psx_us_bin', 'pc_dat_dir'):
                if not os.path.isdir(paths[k]):
                    messagebox.showerror(
                        "Not a folder",
                        f"Path doesn't exist or isn't a directory:\n"
                        f"{paths[k]}", parent=dlg)
                    return
            os.makedirs(paths['trans_dir'], exist_ok=True)

            # Persist for next time
            cfg2 = self._read_editor_config() or {}
            if not isinstance(cfg2, dict): cfg2 = {}
            cfg2['psx_import'] = paths
            self._write_editor_config(cfg2)

            extract_btn.configure(state='disabled', text='Working...')
            status_var.set("Extracting...")
            log_text.configure(state='normal')
            log_text.delete('1.0', 'end')
            log_text.configure(state='disabled')

            overwrite = bool(overwrite_var.get())

            def worker():
                try:
                    import importlib
                    if 'extract_psx_german' in sys.modules:
                        importlib.reload(sys.modules['extract_psx_german'])
                    epg = importlib.import_module('extract_psx_german')
                    summary = epg.run_extraction(
                        psx_de_bin=paths['psx_de_bin'],
                        psx_us_bin=paths['psx_us_bin'],
                        pc_dat_dir=paths['pc_dat_dir'],
                        trans_dir=paths['trans_dir'],
                        overwrite=overwrite,
                        log=lambda m: self.after(
                            0, lambda mm=m: append_log(mm)))
                except Exception as e:
                    self.after(0, lambda: append_log(f"ERROR: {e}"))
                    self.after(0, lambda: status_var.set("Failed."))
                    summary = None

                def finish():
                    extract_btn.configure(state='normal', text='Extract')
                    if summary:
                        status_var.set(
                            f"Done — {summary.get('total_added',0)} "
                            f"translations added across "
                            f"{summary.get('files_matched',0)} files.")
                        # If user wrote to the active TRANS_DIR, refresh
                        if os.path.normcase(paths['trans_dir']) == \
                           os.path.normcase(TRANS_DIR):
                            try:
                                self._load_translations()
                                self.status_var.set(
                                    "Translations reloaded after PSX "
                                    "import.")
                            except Exception:
                                pass
                self.after(0, finish)

            t = threading.Thread(target=worker, daemon=True)
            t.start()

        extract_btn.configure(command=do_extract)

    def _import_missing_psx(self):
        """One-click PSX import in 'safe-merge' mode: only fills in entries
        that have no German yet, NEVER overwrites edited translations.
        Reuses the saved paths from the full Import PSX dialog. If those
        aren't set up yet, falls back to opening the full dialog so the
        user can configure them first."""
        from tkinter import messagebox
        import threading

        cfg = self._read_editor_config() or {}
        psx_block = cfg.get('psx_import', {}) if isinstance(cfg, dict) else {}
        paths = {
            'psx_de_bin':  psx_block.get('psx_de_bin', ''),
            'psx_us_bin':  psx_block.get('psx_us_bin', ''),
            'pc_dat_dir':  psx_block.get('pc_dat_dir', ''),
            'trans_dir':   psx_block.get('trans_dir', TRANS_DIR),
        }
        # If any required dir is missing/invalid, route to the full dialog
        # so the user configures + persists the paths once, then they can
        # use this button thereafter.
        for k in ('psx_de_bin', 'psx_us_bin', 'pc_dat_dir'):
            if not paths[k] or not os.path.isdir(paths[k]):
                messagebox.showinfo(
                    "Set up PSX paths first",
                    "Paths for the PSX disc folders aren't configured yet.\n"
                    "Opening the full Import PSX dialog so you can set them\n"
                    "and persist them. After that, 'Import missing PSX' will\n"
                    "run silently with those saved paths.",
                    parent=self)
                return self._open_psx_import()
        if not os.path.isdir(paths['trans_dir']):
            os.makedirs(paths['trans_dir'], exist_ok=True)

        # Confirm — this writes to translations on disk; user should know
        if not messagebox.askyesno(
                "Import missing PSX entries",
                "Run PSX→PC matcher in SAFE-MERGE mode?\n\n"
                "• Existing non-empty translations: PRESERVED\n"
                "• Empty slots / never-translated entries: filled from PSX\n"
                "• Output dir: " + paths['trans_dir'] + "\n\n"
                "(Same as the Import PSX dialog with 'Overwrite' unchecked,\n"
                " just one-click and silent.)",
                parent=self):
            return

        self.status_var.set("PSX safe-merge import running...")

        def worker():
            summary = None
            err = None
            try:
                import importlib
                if 'extract_psx_german' in sys.modules:
                    importlib.reload(sys.modules['extract_psx_german'])
                epg = importlib.import_module('extract_psx_german')
                log_lines = []
                summary = epg.run_extraction(
                    psx_de_bin=paths['psx_de_bin'],
                    psx_us_bin=paths['psx_us_bin'],
                    pc_dat_dir=paths['pc_dat_dir'],
                    trans_dir=paths['trans_dir'],
                    overwrite=False,
                    log=lambda m: log_lines.append(m))
                # Tail of log for the popup
                summary['_tail'] = "\n".join(log_lines[-15:])
            except Exception as e:
                err = f"{type(e).__name__}: {e}"

            def finish():
                if err:
                    messagebox.showerror(
                        "Import missing PSX failed",
                        "The PSX safe-merge import failed:\n\n" + err,
                        parent=self)
                    self.status_var.set("PSX safe-merge import FAILED.")
                    return
                added = summary.get('total_added', 0)
                files = summary.get('files_matched', 0)
                tail  = summary.get('_tail', '')
                messagebox.showinfo(
                    "Import missing PSX — done",
                    f"Safe-merge complete.\n\n"
                    f"  {added} translation(s) added\n"
                    f"  {files} file(s) touched\n\n"
                    f"(Existing entries were NOT overwritten.)\n\n"
                    f"Last log lines:\n{tail}",
                    parent=self)
                self.status_var.set(
                    f"PSX safe-merge: +{added} translations in "
                    f"{files} files (existing preserved).")
                if os.path.normcase(paths['trans_dir']) == \
                   os.path.normcase(TRANS_DIR):
                    try:
                        self._load_translations()
                    except Exception:
                        pass
            self.after(0, finish)

        threading.Thread(target=worker, daemon=True).start()

    # Tiny config-file helpers used by the PSX import dialog.
    def _read_editor_config(self):
        try:
            with open(CONFIG_PATH, encoding='utf-8') as f:
                return json.load(f)
        except Exception:
            return {}

    def _write_editor_config(self, cfg):
        try:
            os.makedirs(os.path.dirname(CONFIG_PATH), exist_ok=True)
            with open(CONFIG_PATH, 'w', encoding='utf-8') as f:
                json.dump(cfg, f, indent=2, ensure_ascii=False)
        except Exception:
            pass

    # ── Draggable toolbar buttons ────────────────────────────────────────────
    def _build_action_buttons(self):
        """Create the toolbar action buttons (once) and (re)pack them in the
        order saved in editor_config["toolbar_order"]. Unknown/new spec keys are
        appended at the end so adding a button later never loses it."""
        if not self._action_btns:
            for key, text, cmd, bg in self._action_specs:
                b = tk.Button(self.action_bar, text=text, bg=bg, fg="white",
                              relief=tk.FLAT, padx=10, pady=2,
                              font=("Segoe UI", 9))
                b._spec_key = key
                b._command  = cmd
                # No command= : we invoke it ourselves on a click that wasn't a
                # drag, so dragging to reorder never also triggers the action.
                b.bind("<ButtonPress-1>",   self._drag_start)
                b.bind("<B1-Motion>",       self._drag_motion)
                b.bind("<ButtonRelease-1>", self._drag_end)
                self._action_btns[key] = b

        known = [s[0] for s in self._action_specs]
        order = [k for k in (self._config.get("toolbar_order") or [])
                 if k in self._action_btns]
        for k in known:                     # append any spec not yet in the saved order
            if k not in order:
                order.append(k)
        self._action_order = order
        for b in self._action_btns.values():
            b.pack_forget()
        for k in order:
            self._action_btns[k].pack(side=tk.LEFT, padx=3)

    def _drag_start(self, e):
        self._drag_btn   = e.widget
        self._drag_x0    = e.x_root
        self._drag_moved = False

    def _drag_motion(self, e):
        btn = getattr(self, "_drag_btn", None)
        if btn is None:
            return
        if not self._drag_moved and abs(e.x_root - self._drag_x0) <= 6:
            return                          # below threshold — still a potential click
        if not self._drag_moved:
            self._drag_moved = True
            btn.config(relief=tk.RIDGE)     # visual "picked up" cue
        # Live insertion preview: place the dragged button where the cursor is.
        key       = btn._spec_key
        remaining = [k for k in self._action_order if k != key]
        ins = 0
        for k in remaining:
            sib = self._action_btns[k]
            mid = sib.winfo_rootx() + sib.winfo_width() / 2
            if e.x_root > mid:
                ins += 1
            else:
                break
        new_order = remaining[:ins] + [key] + remaining[ins:]
        if new_order != self._action_order:
            self._action_order = new_order
            for k in self._action_btns.values():
                k.pack_forget()
            for k in new_order:
                self._action_btns[k].pack(side=tk.LEFT, padx=3)

    def _drag_end(self, e):
        btn = getattr(self, "_drag_btn", None)
        self._drag_btn = None
        if btn is None:
            return
        btn.config(relief=tk.FLAT)
        if self._drag_moved:
            self._config["toolbar_order"] = list(self._action_order)
            self._save_config()
            self.status_var.set("Toolbar layout saved.")
        elif getattr(btn, "_command", None):
            btn._command()                  # a plain click — run the action

    # ── File loading ───────────────────────────────────────────────────────────
    def _load_file_list(self):
        files = sorted(f for f in os.listdir(DUMP_DIR) if f.endswith(".txt"))
        self.file_cb["values"] = files
        if files:
            self.file_cb.set(files[0])
            self._on_file_select()

    def _on_file_select(self, event=None):
        self._commit_current()
        fname = self.file_var.get()
        if not fname:
            return
        self.current_file = fname
        self._last_loaded_file = fname
        path = os.path.join(DUMP_DIR, fname)

        try:
            mtime = os.path.getmtime(path)
        except OSError:
            mtime = 0
        if fname in self._dump_mtimes and self._dump_mtimes[fname] != mtime:
            self.status_var.set(f"{fname} changed on disk — rebuilding index...")
            self.update()
            self._build_shared_index()
        self._dump_mtimes[fname] = mtime

        self.file_header, self.entries = parse_dump(path)
        self._load_translations()
        self._load_string_status()
        self._load_notes()
        self._unlocked_ctrl_only.clear()  # per-file scope; fresh on each load
        n_auto = self._auto_fill_ctrl_only()
        if n_auto:
            self.status_var.set(
                f"Auto-filled {n_auto} control-code-only string(s) — "
                f"Ctrl+S to persist")
        self.current_entry_idx = 0
        self.current_str_idx   = 0
        # Invalidate the "widgets are synced for X" marker — the widgets
        # still show the previous file's string until _display_string runs
        # below, so a commit before then would be writing stale content.
        self._displayed_key = (None, None, None)

        labels = [f"Entry {e['num']} @ {e['at']}  ({e['unique']} unique / {e['total']} total)"
                  for e in self.entries]
        self.entry_cb["values"] = labels
        if labels:
            # Auto-select the entry with the most translations (instead of
            # always defaulting to entry 0, which might be empty while the
            # interesting text is in a later entry like entry 5).
            best_idx = 0
            best_count = 0
            for ei, e in enumerate(self.entries):
                count = sum(1 for s in e["strings"]
                            if self.translations.get(
                                self._key(e["num"], s["offset"]), "").strip())
                if count > best_count:
                    best_count = count
                    best_idx = ei
            self.entry_cb.current(best_idx)
            self.current_entry_idx = best_idx
        self._load_entry()

    def _on_entry_select(self, event=None):
        self._commit_current()
        self.current_entry_idx = self.entry_cb.current()
        self.current_str_idx   = 0
        # Same invalidation as file switch: widgets still show the old
        # entry's string until _display_string runs.
        self._displayed_key = (None, None, None)
        self._load_entry()

    def _load_entry(self):
        if not self.entries:
            return
        e = self.entries[self.current_entry_idx]
        self.hdr_lbl.config(
            text=f"  # {self.file_header}   |   "
                 f"## Entry {e['num']} @ {e['at']} (size {e['size']})   |   "
                 f"{e['unique']} unique strings, {e['total']} offset-table entries"
        )
        self._populate_tree()
        children = self.tree.get_children()
        if children:
            self.tree.selection_set(children[0])
            self.tree.see(children[0])
            self._on_str_select()

    # ── String list ────────────────────────────────────────────────────────────
    def _populate_tree(self):
        # Remember the current selection/focus/scroll so a rebuild (Save,
        # Export, Sync All, filter change, …) doesn't drop the user off the row
        # they were working on. Deleting the rows clears the selection, so we
        # re-apply it at the end for any iid that still exists. Callers that
        # deliberately want row 0 (e.g. _load_entry on entry change) select it
        # explicitly AFTER this returns, which overrides the restore.
        prev_sel   = self.tree.selection()
        prev_focus = self.tree.focus()
        try:
            prev_yview = self.tree.yview()[0]
        except Exception:
            prev_yview = None

        for iid in self.tree.get_children():
            self.tree.delete(iid)
        if not self.entries:
            return
        e = self.entries[self.current_entry_idx]
        current_base = self.current_file[:-4] if self.current_file.endswith(".txt") else self.current_file
        done = 0
        total_translatable = 0
        for s in e["strings"]:
            key   = self._key(e["num"], s["offset"])
            trans = self.translations.get(key, "")
            is_suffix = bool(s.get("suffix_of"))
            is_dup, dup_base, dup_key = self._is_duplicate(
                self._dup_raw(s), current_base, key)
            # A shared, non-primary location the user unlocked for its own
            # translation. is_dup is already False for it; we flag it in the
            # list so independent duplicates are visible at a glance.
            prim = self._primary_loc.get(self._dup_raw(s))
            is_indep = (prim is not None
                        and (prim[0], prim[1]) != (current_base, key)
                        and (current_base, key) in self._unlocked_dups)
            is_sealed = (self._is_monster_sealed(current_base, key)
                         or self._is_uncensor_sealed(current_base, key))
            is_done = bool(trans.strip())
            status_flag = self._string_status.get(key, STATUS_NONE)

            # Sealed monster names are intentionally English — not "to translate".
            if not is_suffix and not is_dup and not is_sealed:
                total_translatable += 1
                if is_done:
                    done += 1

            # Apply filter
            if self._filter_mode == FILTER_UNTRANSLATED:
                if is_done or is_suffix or is_dup:
                    continue
            elif self._filter_mode == FILTER_TRANSLATED:
                if not is_done or is_suffix or is_dup:
                    continue
            elif self._filter_mode in (FILTER_DRAFT, FILTER_REVIEWED, FILTER_FINAL):
                if status_flag != self._filter_mode:
                    continue
            elif self._filter_mode == FILTER_MISSING:
                if is_suffix or is_dup or not self._has_missing_codes(s, trans):
                    continue
            elif self._filter_mode == FILTER_EXTRA:
                if is_suffix or is_dup or not self._has_extra_codes(s, trans):
                    continue
            elif self._filter_mode == FILTER_SOURCE_CHANGED:
                if key not in getattr(self, "_source_changed_keys", ()):
                    continue
            elif self._filter_mode == FILTER_NOTED:
                if not self._notes.get(key, "").strip():
                    continue

            src_prev   = raw_to_display(s["raw"]).replace("\n", " | ")[:100]
            trans_prev = trans.replace("\n", " | ")[:100]
            shared_n = self._get_shared_count(self._dup_raw(s))

            if is_suffix:
                tag = "suffix"
                status = "SUF"
            elif is_dup:
                tag = "suffix"
                if (current_base, key) in MANUAL_DUP_MAP:
                    # Manual (cross-offset) dup: point at the master offset.
                    m_off = dup_key.split(":", 1)[-1] if dup_key else ""
                    status = f"DUP->{m_off}"
                else:
                    status = f"DUP({shared_n})"
            elif is_indep:
                tag = "done" if is_done else "todo"
                status = ("IND" if is_done else "IND-TODO") + \
                         (f"({shared_n})" if shared_n > 1 else "")
            elif is_done:
                tag = "done"
                status = "OK" + (f"({shared_n})" if shared_n > 1 else "")
            else:
                tag = "todo"
                status = "TODO" + (f"({shared_n})" if shared_n > 1 else "")

            # Override tag with status flag if translated
            if is_done and not is_suffix and not is_dup and status_flag:
                tag = status_flag

            flag_icon, _ = STATUS_ICONS.get(status_flag, ("", "#666666"))
            # Note marker: a pencil next to the status flag when this row has
            # a translator note (visible at a glance; "Noted" filter isolates).
            if self._notes.get(key, "").strip():
                flag_icon = (flag_icon + "✎").strip()

            self.tree.insert("", tk.END, iid=s["offset"],
                             values=(s["offset"], status, flag_icon,
                                     src_prev, trans_prev),
                             tags=(tag,))

        pct = int(100 * done / total_translatable) if total_translatable else 0
        self.progress_lbl.config(text=f"{done}/{total_translatable} translated  ({pct}%)")

        # Re-apply the pre-rebuild selection so operations that merely refresh
        # the list (Save/Export/Sync/filter) keep the user's highlighted row.
        keep = [iid for iid in prev_sel if self.tree.exists(iid)]
        if keep:
            self.tree.selection_set(keep)
            focus_iid = prev_focus if self.tree.exists(prev_focus) else keep[0]
            self.tree.focus(focus_iid)
            if prev_yview is not None:
                try:
                    self.tree.yview_moveto(prev_yview)
                except Exception:
                    self.tree.see(keep[0])
            else:
                self.tree.see(keep[0])

    def _on_str_select(self, event=None):
        sel = self.tree.selection()
        if not sel:
            return
        self._commit_current()
        offset = sel[0]
        e = self.entries[self.current_entry_idx]
        for i, s in enumerate(e["strings"]):
            if s["offset"] == offset:
                self.current_str_idx = i
                self._display_string(s)
                break
        self._update_status()

    # ── Clean formatted rendering ────────────────────────────────────────────
    @staticmethod
    def _portrait_speaker(code_inner):
        parts = code_inner.split(':')
        if len(parts) < 2:
            return None
        try:
            first = int(parts[1], 16)
        except ValueError:
            return None
        high = (first >> 4) & 0xF
        low = first & 0xF
        if high in (0x5, 0xD):
            name = PARTY_NAMES.get(low)
            return name or f"Party#{low}"
        elif high in (0x0, 0x8):
            return f"NPC#{low}"
        return None

    _HIDDEN_CODES = frozenset([
        '0C', '14', '10', '16', '1C', '0D', '08',
        '11', '12', '13', '15', '19', '1E', '1F',
    ])

    def _render_clean(self, widget, text):
        widget.config(state=tk.NORMAL)
        widget.delete("1.0", tk.END)
        color_tag = None
        pos = 0
        at_start = True
        for m in CTRL_RE.finditer(text):
            seg = text[pos:m.start()]
            if seg:
                self._insert_clean_text(widget, seg, color_tag, at_start)
                at_start = False
            inner = m.group()[1:-1].upper()
            first_byte = inner.split(':')[0]
            if inner.startswith('05:') and len(inner) >= 5:
                color_code = inner[3:5]
                if color_code in TEXT_COLORS:
                    color_tag = f"tc_{color_code}"
            elif inner == '06':
                color_tag = None
            elif inner == '02':
                widget.insert(tk.END, " >\n", "sep")
            elif inner == '0B':
                widget.insert(tk.END, "...", "pause")
            elif inner.startswith('04:') and len(inner) >= 5:
                idx = int(inner[3:5], 16)
                if idx in PARTY_NAMES:
                    widget.insert(tk.END, PARTY_NAMES[idx], "char_name")
                elif _byte_to_unicode(idx) is not None:
                    # [04:XX] glyph-emit (XX>=0x20): draw the actual letter,
                    # e.g. [04:66]->'f', [04:6D]->'m', [04:22]->'"'. Same class
                    # as [09:22:XX] but a 2-byte opcode _glyph_emit_char misses.
                    widget.insert(tk.END, _byte_to_unicode(idx), "glyphemit")
                else:
                    widget.insert(tk.END, f"Char#{idx}", "char_name")
                at_start = False
            elif inner == '03':
                widget.insert(tk.END, "Hero", "char_name")
                at_start = False
            elif inner.startswith('17:'):
                speaker = self._portrait_speaker(inner)
                if speaker:
                    widget.insert(tk.END, f"[{speaker}] ", "speaker")
                    at_start = False
            elif _glyph_emit_char(inner) is not None:
                widget.insert(tk.END, _glyph_emit_char(inner), "glyphemit")
                at_start = False
            elif inner.startswith('09:'):
                widget.insert(tk.END, "{item}", "placeholder")
                at_start = False
            elif inner.startswith('07:'):
                widget.insert(tk.END, "{var}", "placeholder")
                at_start = False
            elif inner.startswith('18:'):
                widget.insert(tk.END, "{ref}", "placeholder")
            elif inner.startswith('0E:'):
                pass
            elif first_byte in self._HIDDEN_CODES:
                pass
            else:
                widget.insert(tk.END, m.group(), "ctrl")
            pos = m.end()
        remaining = text[pos:]
        if remaining:
            self._insert_clean_text(widget, remaining, color_tag, at_start)
        widget.config(state=tk.DISABLED)

    def _insert_clean_text(self, widget, seg, color_tag, at_start):
        tags = (color_tag,) if color_tag else ()
        parts = seg.split("@")
        for i, part in enumerate(parts):
            if i > 0:
                if not at_start or widget.get("1.0", tk.END).strip():
                    widget.insert(tk.END, "\n")
            if part:
                widget.insert(tk.END, part, tags)

    # ── Clean view (plain text without any codes) ───────────────────────────
    def _update_clean_view(self, s):
        """Show the source string as pure plain text — no control codes,
        no escape sequences, no newlines. Ready for copy-paste into a translator."""
        self.src_clean.config(state=tk.NORMAL)
        self.src_clean.delete("1.0", tk.END)

        display = raw_to_display(s["raw"])
        clean = _strip_ctrl_codes(display)
        # Collapse newlines and @ into spaces for one continuous sentence
        clean = clean.replace('\n', ' ').replace('@', ' ')
        # Collapse multiple spaces
        clean = re.sub(r' {2,}', ' ', clean).strip()

        self.src_clean.insert("1.0", clean)
        self.src_clean.config(state=tk.DISABLED)

    def _display_string(self, s):
        raw     = s["raw"]
        display = raw_to_display(raw)

        # Formatted source
        self._render_clean(self.src_fmt, display)

        # Raw source
        self._write_text(self.src_raw, raw, editable=False, colorize=False)

        # Clean view (plain text for translation)
        self._update_clean_view(s)

        # Source byte size
        self._src_byte_count = self._count_bytes(display)

        is_suffix = bool(s.get("suffix_of"))

        # Check duplicate
        e   = self.entries[self.current_entry_idx]
        key = self._key(e["num"], s["offset"])
        current_base = self.current_file[:-4] if self.current_file.endswith(".txt") else self.current_file
        is_dup, dup_base, dup_key = self._is_duplicate(self._dup_raw(s), current_base, key)
        # Ctrl-only lock only applies when the translation is still equal to
        # the auto-filled source. Once the user has edited it to differ from
        # the source (even one byte), treat it as a real edit and keep it
        # editable across reloads — otherwise the user would have to re-click
        # "Unlock to edit" every time the file is reopened, and worse, the
        # widget population in the locked path is empty so the existing edit
        # would not be visible until they unlocked again.
        is_ctrl_only = (_is_ctrl_only_raw(s["raw"])
                        and key not in self._unlocked_ctrl_only
                        and self.translations.get(key, "") == s["raw"])
        # Monster (enemy) names: sealed = locked as English until unsealed.
        is_monster = self._is_monster_name(current_base, key)
        is_sealed  = self._is_monster_sealed(current_base, key)
        is_uncensor_sealed = self._is_uncensor_sealed(current_base, key)
        is_locked = (is_suffix or is_dup or is_ctrl_only or is_sealed
                     or is_uncensor_sealed)

        # A shared, non-primary location the user has unlocked: it is editable
        # (is_dup is already False for it), but we still want to flag it and
        # offer a re-lock action.
        prim = self._primary_loc.get(self._dup_raw(s))
        is_unlocked_dup = (prim is not None
                           and (prim[0], prim[1]) != (current_base, key)
                           and (current_base, key) in self._unlocked_dups)

        # Go to Primary bar (also reused for the ctrl-only lock banner and the
        # duplicate unlock / re-lock action). The secondary _dup_btn is hidden
        # by default and shown only for the two duplicate cases.
        self._dup_btn.pack_forget()
        self._dup_btn_target = None
        self._master_btn.pack_forget()
        self._master_btn_target = None
        self._seal_btn.pack_forget()
        self._seal_btn_target = None
        self._dupsbrowse_btn.pack_forget()
        self._dupsbrowse_target = None
        # Master-choice applies to ordinary text-keyed groups (>=2 members), not
        # manual cross-offset dups. raw_group = the group key for this row.
        raw_group = self._dup_raw(s)
        group_locs = self._shared_index.get(raw_group)
        is_shared_group = (group_locs is not None and len(group_locs) >= 2
                           and (current_base, key) not in MANUAL_DUP_MAP)
        is_override_master = (is_shared_group
                              and self._dup_masters.get(raw_group) == (current_base, key))
        if is_sealed:
            self._goto_bar.pack(fill=tk.X, before=self._trans_nb)
            self._goto_lbl.config(
                text="MONSTER NAME -- sealed (kept as English). "
                     "Unseal to translate it.")
            self._goto_target = None
            self._goto_btn.pack_forget()
            self._seal_btn_target = (current_base, key)
            self._seal_btn.config(text="  Unseal (translate)  ", bg="#4A6A5A")
            self._seal_btn.pack(side=tk.LEFT, padx=4, before=self._goto_lbl)
        elif is_uncensor_sealed:
            self._goto_bar.pack(fill=tk.X, before=self._trans_nb)
            self._goto_lbl.config(
                text=f"UNCENSOR-ONLY ({current_base} scene) -- locked as English. This line "
                     "renders ONLY when the scene is played uncensored. Unlock to translate.")
            self._goto_target = None
            self._goto_btn.pack_forget()
            self._seal_btn_target = (current_base, key)
            self._seal_btn.config(text="  Unlock (translate)  ", bg="#4A6A5A")
            self._seal_btn.pack(side=tk.LEFT, padx=4, before=self._goto_lbl)
        elif is_suffix:
            self._goto_bar.pack(fill=tk.X, before=self._trans_nb)
            self._goto_lbl.config(text=f"SUFFIX of {s['suffix_of']} -- translate the root string")
            self._goto_target = None
            self._goto_btn.pack_forget()
        elif is_dup:
            self._goto_bar.pack(fill=tk.X, before=self._trans_nb)
            is_manual = (current_base, key) in MANUAL_DUP_MAP
            if is_manual:
                # Same-file, cross-offset inherited duplicate.
                self._goto_lbl.config(
                    text=f"DUPLICATE of {dup_key} -- translate that one "
                         f"(this slot shares its text)")
                self._goto_btn.config(text=f"  Go to {dup_key}  ")
            else:
                self._goto_lbl.config(
                    text=f"DUPLICATE -- shared with primary at {dup_base}.txt {dup_key}")
                self._goto_btn.config(text=f"  Go to {dup_base}.txt  ")
            self._goto_target = (dup_base, dup_key)
            self._goto_btn.pack(side=tk.LEFT, padx=4)
            self._dup_btn_target = (current_base, key, s["raw"])
            self._dup_btn.config(text="  Unlock (edit on its own)  ",
                                 bg="#4A6A5A")
            self._dup_btn.pack(side=tk.LEFT, padx=4)
            # Let the user promote THIS occurrence to the group's master.
            if is_shared_group and not is_manual:
                self._master_btn_target = (current_base, key, raw_group)
                self._master_btn.config(text="  Set as Master  ")
                self._master_btn.pack(side=tk.LEFT, padx=4)
        elif is_override_master:
            # This row is the user-chosen master of its duplicate group.
            self._goto_bar.pack(fill=tk.X, before=self._trans_nb)
            n = len(group_locs)
            self._goto_lbl.config(
                text=f"MASTER of duplicate group ({n} copies) -- "
                     f"Sync All sources them all from here")
            self._goto_target = None
            self._goto_btn.pack_forget()
            self._master_btn_target = (current_base, key, raw_group)
            self._master_btn.config(text="  Clear Master (use default)  ")
            self._master_btn.pack(side=tk.LEFT, padx=4)
        elif is_unlocked_dup:
            self._goto_bar.pack(fill=tk.X, before=self._trans_nb)
            self._goto_lbl.config(
                text=f"INDEPENDENT -- unlocked duplicate (primary at "
                     f"{prim[0]}.txt {prim[1]})")
            self._goto_target = (prim[0], prim[1])
            self._goto_btn.pack(side=tk.LEFT, padx=4)
            self._goto_btn.config(text=f"  Go to {prim[0]}.txt  ")
            self._dup_btn_target = (current_base, key, s["raw"])
            self._dup_btn.config(text="  Re-lock (share primary)  ",
                                 bg="#6A5A4A")
            self._dup_btn.pack(side=tk.LEFT, padx=4)
        elif is_ctrl_only:
            self._goto_bar.pack(fill=tk.X, before=self._trans_nb)
            self._goto_lbl.config(text="CONTROL CODES ONLY -- auto-filled from source")
            self._goto_target = ("__unlock_ctrl_only__", key)
            self._goto_btn.pack(side=tk.LEFT, padx=4)
            self._goto_btn.config(text="  Unlock to edit  ")
        else:
            self._goto_bar.pack_forget()
            self._goto_target = None

        # An UNSEALED monster name is a normal editable string, but still offer a
        # one-click way to re-seal it (keep English). Show the bar just for this
        # button if nothing else claimed it.
        if is_monster and not is_sealed:
            if not self._goto_bar.winfo_ismapped():
                self._goto_bar.pack(fill=tk.X, before=self._trans_nb)
                self._goto_lbl.config(text="MONSTER NAME -- unsealed (translatable)")
                self._goto_btn.pack_forget()
                self._goto_target = None
            self._seal_btn_target = (current_base, key)
            self._seal_btn.config(text="  Seal (keep English)  ", bg="#6A5A4A")
            self._seal_btn.pack(side=tk.LEFT, padx=4, before=self._goto_lbl)

        # An UNLOCKED uncensor-only slot: editable, but flag it (uncensor-scene
        # only) and offer a one-click re-lock.
        if self._is_uncensor_entry(current_base, key) and not is_uncensor_sealed:
            if not self._goto_bar.winfo_ismapped():
                self._goto_bar.pack(fill=tk.X, before=self._trans_nb)
                self._goto_lbl.config(
                    text=f"UNCENSOR-ONLY ({current_base} scene) -- unlocked (translatable); "
                         "renders only when played uncensored")
                self._goto_btn.pack_forget()
                self._goto_target = None
            self._seal_btn_target = (current_base, key)
            self._seal_btn.config(text="  Lock (uncensor-only)  ", bg="#6A5A4A")
            self._seal_btn.pack(side=tk.LEFT, padx=4, before=self._goto_lbl)

        # Any member of a shared duplicate group: offer a browser to walk ALL its
        # copies. Complements the per-duplicate "Go to primary" — from the master
        # (or any copy) you can survey the whole group and jump around / re-pick
        # the master. Shown on top of whatever else claimed the bar; if the row is
        # a plain primary (no other banner), show the bar just for this.
        if is_shared_group:
            if not self._goto_bar.winfo_ismapped():
                self._goto_bar.pack(fill=tk.X, before=self._trans_nb)
                n = len(group_locs)
                who = "MASTER" if is_override_master else "PRIMARY"
                self._goto_lbl.config(
                    text=f"{who} of duplicate group ({n} copies) -- "
                         f"Sync All sources them all from here")
                self._goto_btn.pack_forget()
                self._goto_target = None
            self._dupsbrowse_target = raw_group
            self._dupsbrowse_cur = (current_base, key)
            self._dupsbrowse_btn.config(text=f"  Duplicates ({len(group_locs)})...  ")
            self._dupsbrowse_btn.pack(side=tk.LEFT, padx=4)

        # Update status button
        status_flag = self._string_status.get(key, STATUS_NONE)
        self._update_status_btn(status_flag)

        # Translation
        trans = self.translations.get(key, "")
        # A manual-duplicate slot inherits the master's translation: preview it
        # (read-only) so the translator sees what this slot will render as.
        locked_preview = ""
        if is_dup and (current_base, key) in MANUAL_DUP_MAP:
            locked_preview = self._read_trans_value(dup_base, dup_key) or ""
        elif is_sealed or is_uncensor_sealed:
            # Show the English source (read-only) so the translator sees what the
            # sealed / uncensor-locked slot will render as in-game.
            locked_preview = s["raw"]

        self.trans_edit.config(state=tk.NORMAL)
        self.trans_edit.delete("1.0", tk.END)
        self.trans_fmt.config(state=tk.NORMAL)
        self.trans_fmt.delete("1.0", tk.END)

        if is_locked:
            if locked_preview:
                self._insert_colored(self.trans_edit, locked_preview)
                self._render_fmt_editable(self.trans_fmt,
                                          raw_to_display(locked_preview))
            self.trans_edit.config(state=tk.DISABLED)
            self.trans_fmt.config(state=tk.DISABLED)
        elif trans:
            self._insert_colored(self.trans_edit, trans)
            self._render_fmt_editable(self.trans_fmt, raw_to_display(trans))

        # Always park the Raw widget's caret at end-of-text so a click
        # into the translation drops the user at the natural typing
        # position. We deliberately do NOT call focus_set() here:
        # stealing focus on every selection blocks Up/Down arrow-key
        # navigation in the string list (the tree loses focus, so
        # arrow keys would only move the text cursor). The user
        # explicitly clicks into the translation widget when they
        # want to type — that's when focus moves over.
        if not is_locked:
            self.trans_edit.mark_set(tk.INSERT, tk.END)
        self.trans_offset_lbl.config(text=s["offset"])
        self._refresh_byte_counter()

        # Run validation
        self._validate_current(s, trans, is_locked)

        # Update dialog mockup
        self._update_dialog_mockup(s, trans)

        # Re-underline misspellings for the freshly loaded translation.
        self._schedule_spellcheck(delay=1)

        # Translator note for this row. Commit any pending edit for the
        # PREVIOUS row first (its box may not have fired FocusOut yet), then
        # switch the box to this row and remember which row it now represents.
        if hasattr(self, "_note_var"):
            self._commit_note(refresh=False)
            self._note_key = key
            self._note_var.set(self._notes.get(key, ""))

        # Translation-memory suggestions for this row's source text.
        self._refresh_tm_panel(self._cur_source.get(key) or display)

        # Mark widgets as synced for this specific string so _commit_current
        # will now accept writes from trans_edit/trans_fmt as legitimate.
        self._displayed_key = (self.current_file, e["num"], s["offset"])

    def _write_text(self, widget, text, editable, colorize, labels=False):
        widget.config(state=tk.NORMAL)
        widget.delete("1.0", tk.END)
        if colorize:
            self._insert_colored(widget, text, labels=labels)
        else:
            widget.insert("1.0", text)
        if not editable:
            widget.config(state=tk.DISABLED)

    def _insert_colored(self, widget, text, labels=False):
        pos = 0
        for m in CTRL_RE.finditer(text):
            segment = text[pos:m.start()]
            if segment:
                self._insert_plain(widget, segment)
            display = ctrl_label(m.group()) if labels else m.group()
            widget.insert(tk.END, display, "ctrl")
            pos = m.end()
        self._insert_plain(widget, text[pos:])

    def _insert_plain(self, widget, text):
        parts = text.split("@")
        for i, part in enumerate(parts):
            if part:
                widget.insert(tk.END, part)
            if i < len(parts) - 1:
                widget.insert(tk.END, "@", "at_break")

    # ── Validation ─────────────────────────────────────────────────────────────
    def _diff_ctrl_codes(self, src_display, trans_display):
        """Return (missing, extra) control-code lists.

        [02] page-breaks are filtered from `missing` because the German
        translations (post PSX-import remap) drop them but the engine
        still page-breaks correctly — flagging them is just noise.

        [09:22:XX] is normalized so a deliberate byte swap (e.g. 0x55→
        0x90 to emit Ä instead of U) doesn't surface as a missing+extra
        pair. The opcode shape matters for portrait state; the XX byte
        is content the translator is expected to adjust. The separate
        glyph-emit remark below catches cases where the byte still
        doesn't match the following letter.
        """
        src_norm   = _GLYPH_EMIT_RE.sub("[09:22:*]", src_display)
        trans_norm = _GLYPH_EMIT_RE.sub("[09:22:*]", trans_display)
        # [04:XX] glyph-emit letters are content, not structure — drop them so a
        # translated letter (e.g. [04:66]->[04:6D]) isn't a missing/extra pair.
        src_norm   = _norm_glyph04(src_norm)
        trans_norm = _norm_glyph04(trans_norm)
        src_codes  = _extract_ctrl_codes(src_norm)
        trans_codes = _extract_ctrl_codes(trans_norm)
        missing, extra = [], []
        if src_codes != trans_codes:
            missing = list(src_codes)
            for c in trans_codes:
                if c in missing:
                    missing.remove(c)
            extra = list(trans_codes)
            for c in src_codes:
                if c in extra:
                    extra.remove(c)
            # [02] page-breaks are noise on BOTH sides: the engine page-breaks
            # on its own, so neither a dropped nor an added [02] is a real fault.
            missing = [c for c in missing if c.upper() != '[02]']
            extra   = [c for c in extra   if c.upper() != '[02]']

        # Button/special glyphs (\x80 escapes + literal '~') — same missing/extra
        # logic, merged into the same lists so the red warning and the "Missing
        # Codes" filter both flag a dropped button prompt.
        src_esc   = _extract_special_glyphs(src_display)
        trans_esc = _extract_special_glyphs(trans_display)
        if src_esc != trans_esc:
            esc_missing = list(src_esc)
            for c in trans_esc:
                if c in esc_missing:
                    esc_missing.remove(c)
            esc_extra = list(trans_esc)
            for c in src_esc:
                if c in esc_extra:
                    esc_extra.remove(c)
            missing += esc_missing
            extra += esc_extra

        # Order the reports by where each token first appears in the text (not
        # alphabetically), so the warning reads left-to-right as the translator
        # sees the source — e.g. "~ \x7f", not the sorted "\x7f ~".
        def _pos(token, text):
            idx = text.lower().find(token.lower())
            return idx if idx >= 0 else len(text)
        missing.sort(key=lambda t: _pos(t, src_display))
        extra.sort(key=lambda t: _pos(t, trans_display))

        return missing, extra

    def _has_missing_codes(self, s, trans):
        """True if the translation is non-empty AND drops control codes
        that appear in the source. Used by the Missing Codes filter."""
        if not trans or not trans.strip():
            return False
        missing, _ = self._diff_ctrl_codes(
            raw_to_display(s["raw"]), raw_to_display(trans))
        return bool(missing)

    def _has_extra_codes(self, s, trans):
        """True if the translation is non-empty AND adds control codes / button
        glyphs that are NOT in the source. Used by the Extra Codes filter —
        mirrors _has_missing_codes but on the `extra` half of the diff."""
        if not trans or not trans.strip():
            return False
        _, extra = self._diff_ctrl_codes(
            raw_to_display(s["raw"]), raw_to_display(trans))
        return bool(extra)

    def _validate_current(self, s, trans, is_locked):
        """Check for control code mismatches, byte limits, and glossary consistency.

        Errors (red) — control-code mismatches; these break in-game rendering.
        Remarks (yellow) — byte-length and glossary notes; informational only.
        """
        errors = []
        remarks = []

        if is_locked or not trans.strip():
            # Frame stays put (bottom-anchored); just clear its rows.
            self._warn_err_lbl.pack_forget()
            self._warn_rem_lbl.pack_forget()
            return

        src_raw = s["raw"]
        src_display = raw_to_display(src_raw)
        trans_display = raw_to_display(trans)

        # 1. Control code mismatch (ERROR — red)
        missing, extra = self._diff_ctrl_codes(src_display, trans_display)
        if missing:
            errors.append(f"Missing codes: {' '.join(missing)}")
        if extra:
            errors.append(f"Extra codes: {' '.join(extra)}")

        # 2. Byte limit check (REMARK — yellow)
        trans_bytes = self._count_bytes(trans_display)
        src_bytes   = self._count_bytes(src_display)
        if trans_bytes > src_bytes + 30:
            remarks.append(f"Translation is {trans_bytes - src_bytes} bytes longer than source")

        # 2b. Per-entry hard byte cap (enemy/boss name tables = 8 bytes).
        # repack_text.py rejects overflowing slots, so we surface the
        # overflow in yellow here so the translator can reword inline.
        if self.entries and 0 <= self.current_entry_idx < len(self.entries):
            e_cap = self.entries[self.current_entry_idx].get("max_byte_len")
            if e_cap is not None and trans_bytes > e_cap:
                over = trans_bytes - e_cap
                remarks.append(
                    f"Name too long by {over} byte(s) — engine cap is {e_cap} "
                    f"(repack will skip this slot)")

        # 3. [09:22:XX] glyph-emit byte mismatch (REMARK — yellow).
        # The opcode emits ONE glyph (XX = atlas index) AND commits dialog
        # portrait state — deleting it breaks the portrait on later
        # frames. The English script uses XX in 0x41–0x5A (ASCII capital
        # letter). When the translated line starts with a German umlaut,
        # XX must change to the umlaut's atlas byte (0x90=Ä, 0x91=Ü,
        # 0x92=Ö, 0x93=ä, 0x94=ü, 0x95=ö, 0x96=ß in stock vwf_config).
        # See memory/reference_control_codes.md for the full story.
        for m in _GLYPH_EMIT_RE.finditer(trans_display):
            xx = int(m.group(1), 16)
            if not (0x41 <= xx <= 0x5A):
                continue  # already a non-ASCII byte — assume user adjusted
            after = trans_display[m.end():m.end() + 6]
            after_stripped = CTRL_RE.sub("", after)
            if not after_stripped:
                continue
            nxt = after_stripped[0]
            if ord(nxt) <= 0x7E or not nxt.isalpha():
                continue  # ASCII follow-up — original opcode byte is fine
            cap = _UMLAUT_TO_CAPITAL.get(nxt, nxt)
            expected = _UNICODE_TO_GAME_BYTE.get(cap)
            if expected is None or expected == xx:
                continue
            remarks.append(
                f"[09:22:{xx:02X}] emits '{chr(xx)}' but next char is "
                f"'{nxt}' — try [09:22:{expected:02X}] for '{cap}' "
                f"(don't delete the opcode — it controls portrait state)")

        # 4. Glossary consistency check (REMARK — yellow)
        if self._glossary:
            src_plain  = _strip_ctrl_codes(src_display).lower()
            trans_plain = _strip_ctrl_codes(trans_display).lower()
            for en_term, de_term in self._glossary.items():
                if en_term.lower() in src_plain:
                    if de_term.lower() not in trans_plain:
                        remarks.append(f"Glossary: '{en_term}' -> '{de_term}' not found in translation")

        if errors or remarks:
            self._warn_err_lbl.config(text="  |  ".join(errors))
            self._warn_rem_lbl.config(text="  |  ".join(remarks))
            self._warn_err_lbl.pack_forget()
            self._warn_rem_lbl.pack_forget()
            if errors:
                self._warn_err_lbl.pack(fill=tk.X, padx=6, pady=(2, 0))
            if remarks:
                self._warn_rem_lbl.pack(fill=tk.X, padx=6,
                                        pady=(0, 2) if errors else (2, 2))
        else:
            self._warn_err_lbl.pack_forget()
            self._warn_rem_lbl.pack_forget()

    # ── Dialog mockup ──────────────────────────────────────────────────────────
    def _update_dialog_mockup(self, s, trans):
        """Update the dialog box mockup with translation text (or source if no translation).
        Parses color codes to render colored text segments on the canvas.

        Page breaks: [02], @, and [13:XX] all start a new dialog box (page).
        [13:XX] is the engine's wait-then-clear-box code that suffix offsets
        anchor onto, so splitting on it makes the preview match what the
        player actually sees page by page.
        Line breaks: \\n splits lines within a single dialog box.
        """
        if not getattr(self, "_mockup_enabled", False):
            return          # Preview tab removed — dormant
        raw = trans if trans.strip() else s["raw"]
        display = raw_to_display(raw)

        # Split into pages at [02], @, or [13:XX] — all are dialog box separators
        page_texts = re.split(r'\[02\]|@|\[13:[0-9A-Fa-f]{2}\]', display)
        pages = []  # list of pages, each page = list of lines, each line = list of (text, color)

        for page_text in page_texts:
            # Parse this page into colored segments
            segments = []  # list of (text, color_hex_or_None)
            current_color = None
            pos = 0
            for m in CTRL_RE.finditer(page_text):
                seg = page_text[pos:m.start()]
                if seg:
                    segments.append((seg, current_color))
                inner = m.group()[1:-1].upper()

                if inner.startswith('05:') and len(inner) >= 5:
                    color_code = inner[3:5]
                    current_color = TEXT_COLORS.get(color_code)
                elif inner == '06':
                    current_color = None
                elif inner.startswith('04:') and len(inner) >= 5:
                    idx = int(inner[3:5], 16)
                    if idx in PARTY_NAMES:
                        segments.append((PARTY_NAMES[idx], "#E0A040"))
                    elif _byte_to_unicode(idx) is not None:
                        # [04:XX] glyph-emit -> real letter, in normal text
                        # colour so the word reads correctly ("First"/"müsst").
                        segments.append((_byte_to_unicode(idx), current_color))
                    else:
                        segments.append((f"Char#{idx}", "#E0A040"))
                elif inner == '03':
                    segments.append(("Hero", "#E0A040"))
                elif inner.startswith('09:'):
                    segments.append(("{item}", "#8888CC"))
                elif inner.startswith('07:'):
                    segments.append(("{var}", "#8888CC"))
                # Skip all other codes (hidden)
                pos = m.end()

            remaining = page_text[pos:]
            if remaining:
                segments.append((remaining, current_color))

            # Split into lines at \n only (@ is already a page break)
            lines = [[]]
            for seg_text, seg_color in segments:
                parts = seg_text.split('\n')
                for pi, part in enumerate(parts):
                    if pi > 0:
                        lines.append([])
                    if part:
                        lines[-1].append((part, seg_color))

            # Remove empty leading/trailing lines
            while lines and not lines[0]:
                lines.pop(0)
            while lines and not lines[-1]:
                lines.pop()

            if lines:
                pages.append(lines)

        if not pages:
            pages = [[[("", None)]]]

        self._mockup_pages = pages
        self._mockup_current_page = 0
        self._render_mockup_page()

    def _render_mockup_page(self):
        """Composite the current page at native game resolution then scale up
        with NEAREST. Layers (bottom to top): textbox bg, portrait, glyphs,
        advance symbol (when there's a next page)."""
        if not getattr(self, "_mockup_enabled", False):
            return          # Preview tab removed — dormant
        if not _HAS_PIL:
            return

        page_idx = self._mockup_current_page
        if page_idx >= len(self._mockup_pages):
            page_idx = 0
        page = self._mockup_pages[page_idx] if self._mockup_pages else []

        nw = self._mockup_native_w
        nh = self._mockup_native_h

        # 1) Base canvas with the textbox background.
        canvas_img = Image.new("RGBA", (nw, nh), (0, 0, 0, 0))
        if self._textbox_img:
            canvas_img.paste(self._textbox_img, (0, 0), self._textbox_img)

        warnings = []

        # 2) Portrait. Anchored 8 px from left (or 8 px from right when
        # flipped). Text origin shifts so it always starts 8 px after the
        # portrait's far edge (or just 8 px from the textbox edge with no
        # portrait).
        # Pull user tweaks (default to constants when not set yet).
        left_inset   = getattr(self, "_pv_text_left",   MOCKUP_TEXT_INSET)
        right_inset  = getattr(self, "_pv_text_right",  MOCKUP_TEXT_INSET)
        top_inset    = getattr(self, "_pv_text_top",    MOCKUP_TEXT_TOP)
        line_h       = getattr(self, "_pv_line_height", MOCKUP_LINE_HEIGHT)
        adv_fudge    = getattr(self, "_pv_adv_fudge",   0)

        port_w = 0
        if self._portrait_visible and self._portrait_img:
            pimg = self._portrait_img
            port_w = pimg.width
            # Portrait may be taller than the textbox; align bottom edges
            # so the head sticks above the textbox like a typical RPG bust.
            py = nh - pimg.height
            if self._portrait_flipped:
                px = nw - left_inset - port_w
                pimg = pimg.transpose(Image.FLIP_LEFT_RIGHT)
            else:
                px = left_inset
            canvas_img.alpha_composite(pimg, (px, py))

        # Where text starts (game pixels):
        if self._portrait_visible and not self._portrait_flipped:
            text_x0 = left_inset + port_w + left_inset
        else:
            text_x0 = left_inset
        # Right edge of text region:
        if self._portrait_visible and self._portrait_flipped:
            text_x_end = nw - right_inset - port_w - right_inset
        else:
            text_x_end = nw - right_inset
        avail_w = max(1, text_x_end - text_x0)

        # 3) Glyphs (one game pixel per source pixel).
        line_idx = 0
        for line_segments in page:
            if line_idx >= DIALOG_MAX_LINES:
                warnings.append(
                    f"Page {page_idx+1}: >{DIALOG_MAX_LINES} lines")
                break
            y = top_inset + line_idx * line_h
            cx = text_x0
            line_w = 0
            for seg_text, _seg_color in line_segments:
                for ch in seg_text:
                    if ch == ' ':
                        adv = self._gfont_advance.get(' ', DEFAULT_SPACE_PX)
                        bearing = 0
                    else:
                        adv = self._gfont_advance.get(ch, 12)
                        bearing = self._gfont_bearing.get(ch, 0)
                    glyph = self._gfont_pil.get(ch)
                    if glyph and ch != ' ':
                        left = self._gfont_left.get(ch, 0)
                        # Engine semantics (confirmed at 0x00527E60 in
                        # BOF4.exe disassembly):
                        #   draw glyph at cursor + bearing
                        #   cursor += advance   (ONLY — bearing is NOT
                        #                        added to the cursor)
                        # Paste PNG so its visible left (col `left`)
                        # lands exactly at (cx + bearing).
                        gx = cx + bearing - left
                        canvas_img.alpha_composite(glyph, (gx, y))
                    step = adv + adv_fudge
                    cx += step
                    line_w += step

            if line_w > avail_w:
                warnings.append(
                    f"Line {line_idx+1}: {line_w}px / {avail_w}px max")
                # Red marker at the right edge of the available area.
                from PIL import ImageDraw
                draw = ImageDraw.Draw(canvas_img)
                draw.line(
                    [(text_x_end, y), (text_x_end, y + line_h)],
                    fill=(255, 64, 64, 255), width=1)
            line_idx += 1

        # 4) Advance symbol — drawn at the horizontal center near the
        # bottom whenever there's a next page (i.e. a [02] / [13:XX] split
        # comes later in the same translation).
        total_pages = len(self._mockup_pages)
        has_next = page_idx < total_pages - 1
        if has_next and self._advance_img:
            ax = (nw - self._advance_img.width) // 2
            ay = nh - self._advance_img.height - 4
            canvas_img.alpha_composite(self._advance_img, (ax, ay))

        # Scale up with NEAREST so each game pixel becomes N screen pixels.
        if MOCKUP_DISPLAY_SCALE != 1:
            disp_img = canvas_img.resize(
                (nw * MOCKUP_DISPLAY_SCALE, nh * MOCKUP_DISPLAY_SCALE),
                Image.NEAREST)
        else:
            disp_img = canvas_img

        self._mockup_photo = ImageTk.PhotoImage(disp_img)
        self._mockup_canvas.itemconfig(
            self._mockup_image_id, image=self._mockup_photo)

        # Page nav state
        self._mockup_page_lbl.config(text=f"Page {page_idx + 1}/{total_pages}")
        self._mockup_prev_btn.config(
            state=tk.NORMAL if page_idx > 0 else tk.DISABLED)
        self._mockup_next_btn.config(
            state=tk.NORMAL if has_next else tk.DISABLED)
        if warnings:
            self._mockup_warn.config(text="! " + "  |  ".join(warnings))
        else:
            self._mockup_warn.config(text="")

    def _mockup_go_page(self, delta):
        new_page = self._mockup_current_page + delta
        if 0 <= new_page < len(self._mockup_pages):
            self._mockup_current_page = new_page
            self._render_mockup_page()

    # ── Translation management ─────────────────────────────────────────────────
    @staticmethod
    def _key(entry_num, offset):
        return f"{entry_num}:{offset}"

    def _commit_current(self):
        if not self.entries:
            return
        e = self.entries[self.current_entry_idx]
        if self.current_str_idx >= len(e["strings"]):
            return
        s = e["strings"][self.current_str_idx]
        if s.get("suffix_of"):
            return
        key = self._key(e["num"], s["offset"])
        current_base = self.current_file[:-4] if self.current_file.endswith(".txt") else self.current_file
        is_dup, _, _ = self._is_duplicate(self._dup_raw(s), current_base, key)
        if is_dup:
            return
        # Gate: if the translation widgets haven't been populated for this
        # specific string yet, their content is stale (from a previously
        # displayed string) or empty (first load). Committing would wipe or
        # corrupt the saved translation for the current string. Skip until
        # _display_string has had a chance to sync the widgets.
        expected_key = (self.current_file, e["num"], s["offset"])
        if self._displayed_key != expected_key:
            return
        try:
            active = self._trans_nb.index(self._trans_nb.select())
        except Exception:
            active = 0
        if active == 1:
            text = self._read_fmt_as_raw()
        else:
            text = self.trans_edit.get("1.0", tk.END).rstrip("\n")
        old_trans = self.translations.get(key, "")
        if text:
            self.translations[key] = text
        elif key in self.translations:
            del self.translations[key]
        if text != old_trans:
            self._dirty = True
            # Track recently edited
            self._track_recently_edited(s, key)
            # Auto-propagate to duplicate locations across other files so
            # the user doesn't have to click Sync All for every edit.
            self._auto_propagate_duplicates(self._dup_raw(s), current_base, key, text)
            # Multi-item slot: keep sibling slots that share this exact leading
            # line (e.g. "Light healing of [07:00] target(s)." repeated across
            # the magic block) in sync automatically — propagate to siblings
            # that are empty OR were in sync with the pre-edit value. Siblings
            # the user made deliberately different (e.g. Feuer/Wind/Wasser/Erde
            # element variants of an identical English line) never match and
            # are left untouched.
            if s.get("multi_item"):
                self._autofill_lead_siblings(current_base, key, s["raw"],
                                             text, old_trans)

    def _auto_propagate_duplicates(self, raw, src_base, src_key, text):
        """When the translation of a shared string changes, write the new
        value into every other file that holds the same raw string. Writes
        are persisted directly to disk so the counter reflects reality
        without needing Sync All."""
        # Manual duplicates: if the edited location is a master, push its value
        # into every locked target (source text differs, so these are NOT in
        # the text-keyed shared_index and must be handled explicitly).
        for tbase, tkey in MANUAL_DUP_REVERSE.get((src_base, src_key), []):
            if (tbase, tkey) in self._unlocked_dups:
                continue
            self._write_trans_value(tbase, tkey, text)

        locations = self._shared_index.get(raw)
        if not locations:
            return
        # If the edited location is itself an unlocked duplicate, it is
        # independent — do NOT push its value onto the shared group.
        if (src_base, src_key) in self._unlocked_dups:
            return
        touched = 0
        for base, key in locations:
            if base == src_base and key == src_key:
                continue
            # Don't overwrite locations the user unlocked for their own text.
            if (base, key) in self._unlocked_dups:
                continue
            path = os.path.join(TRANS_DIR, base + ".json")
            data = {}
            if os.path.exists(path):
                try:
                    data = load_translations_json(path)
                except Exception:
                    data = {}
            cur = data.get(key, "")
            if cur == text:
                continue
            if text:
                data[key] = text
            elif key in data:
                del data[key]
            try:
                write_translations_json(path, data)
                touched += 1
            except Exception:
                pass
        if touched:
            self.status_var.set(
                f"Auto-synced to {touched} duplicate location"
                f"{'s' if touched != 1 else ''}.")

    def _autofill_lead_siblings(self, base, src_key, lead_raw, text, old_text=""):
        """Keep multi-item sibling slots (same file, identical leading line) in
        sync after an edit. A sibling is updated only if it is EMPTY or its
        current value equals `old_text` (i.e. it was in sync with this slot
        before the edit). A sibling holding any OTHER value — a deliberately
        different translation such as an element variant — is left untouched.
        Empty edits (deletions) are not propagated. Siblings live in the same
        file, so writes go through the in-memory map (persisted on next save)."""
        if not text.strip():
            return
        sibs = self._lead_index.get((base, lead_raw))
        if not sibs:
            return
        old = (old_text or "").strip()
        filled = 0
        for b, key in sibs:
            if key == src_key:
                continue
            existing = (self._read_trans_value(b, key) or "")
            in_sync = (not existing.strip()) or (existing == old_text)
            if not in_sync:
                continue                      # deliberately-different: keep it
            if existing == text:
                continue                      # already matches
            self._write_trans_value(b, key, text)
            filled += 1
        if filled:
            self.status_var.set(
                f"Synced this line to {filled} matching slot(s).")

    def _load_translations(self):
        path = os.path.join(TRANS_DIR, self.current_file.replace(".txt", ".json"))
        if os.path.exists(path):
            self._trans_full = load_translations_full(path)
        else:
            self._trans_full = {}
        self._heal_translations()
        self._dirty = False

    def _heal_translations(self):
        """Build `self.translations` (key -> target text) for the CURRENTLY
        loaded DAT rows by pairing on SOURCE TEXT, so translations re-attach
        themselves across DAT layout changes (offset drift) for any language.

        Strategy, per entry:
          * migrated rows (stored `source` present) are matched to current rows
            by source text, occurrence-indexed in offset order (handles dups);
          * un-migrated rows (source is None) fall back to the legacy offset key.
        Orphaned stored translations (source no longer present in this DAT) are
        NOT dropped — they stay in `self._trans_full` and are preserved on save.
        `self._source_changed_keys` collects current rows that look renamed
        (no source match, but the entry has leftover stored translations) for the
        review filter.
        """
        stored = self._trans_full
        self.translations = {}
        self._cur_source = {}          # current key -> underlying source text
        self._source_changed_keys = set()
        self._realigned = 0

        # Pool of stored translations keyed by (entry, source_text), in stored
        # offset order, for text matching. Only entries carrying a `source`.
        # Items are [key, tr] and set to None once consumed.
        from collections import defaultdict
        pool = defaultdict(list)
        legacy = {}                    # key -> translation (source is None)
        for key, rec in stored.items():
            src = rec.get("source")
            tr  = rec.get("translation", "")
            if src is None:
                legacy[key] = tr
            else:
                ent = key.split(":", 1)[0]
                pool[(ent, src)].append([key, tr])

        # Snapshot the current rows grouped by their (entry, source). We assign
        # in TWO passes so identical-source rows (suffixes, duplicates, repeated
        # lines) don't reshuffle their stored translations on every load:
        #   Pass 1 — pin each stored translation to the current row with the
        #            SAME offset key (the no-drift case: every value returns to
        #            its own offset, so nothing "keeps overriding itself").
        #   Pass 2 — positionally distribute whatever stored translations remain
        #            to the still-unmatched current rows, in offset order (the
        #            offset-drift path that keeps translations attached across
        #            DAT layout changes).
        current = []                   # (key, entry_num, source) in offset order
        for e in getattr(self, "entries", []) or []:
            num = e["num"]
            for s in e["strings"]:
                key = self._key(num, s["offset"])
                underlying = s.get("full_raw") or s["raw"]
                self._cur_source[key] = underlying
                current.append((key, num, underlying))

        # Pass 1: exact offset-key matches within the same (entry, source) group.
        unmatched = []
        for key, num, src in current:
            lst = pool.get((num, src))
            hit = None
            if lst:
                for i, item in enumerate(lst):
                    if item is not None and item[0] == key:
                        hit = i
                        break
            if hit is not None:
                _old_key, tr = lst[hit]
                lst[hit] = None
                if tr:
                    self.translations[key] = tr
            else:
                unmatched.append((key, num, src))

        # Pass 2: positional distribution of leftovers to drifted rows.
        for key, num, src in unmatched:
            lst = pool.get((num, src))
            chosen = None
            if lst:
                for i, item in enumerate(lst):
                    if item is not None:
                        chosen = i
                        break
            if chosen is not None:
                _old_key, tr = lst[chosen]
                lst[chosen] = None
                if tr:
                    self.translations[key] = tr
                    self._realigned += 1
                continue
            # Legacy fallback: un-migrated file, pair by offset key.
            if key in legacy and legacy[key]:
                self.translations[key] = legacy[key]

        # Anything still sitting in the pool = stored translation whose source
        # no longer matches a current row -> the source was changed/removed.
        # Flag current rows in those entries that ended up untranslated.
        leftover_entries = {ent for (ent, _src), lst in pool.items()
                            if any(it is not None and it[1] for it in lst)}
        if leftover_entries:
            for e in getattr(self, "entries", []) or []:
                if e["num"] not in leftover_entries:
                    continue
                for s in e["strings"]:
                    key = self._key(e["num"], s["offset"])
                    if key not in self.translations:
                        self._source_changed_keys.add(key)

        # Surface a one-line summary when the DAT drifted from the stored data.
        n_ch = len(self._source_changed_keys)
        if (self._realigned or n_ch) and hasattr(self, "status_var"):
            try:
                self.status_var.set(
                    f"Re-attached {self._realigned} translation(s) by source text"
                    + (f"  ·  {n_ch} source(s) changed — see 'Src Changed' filter"
                       if n_ch else ""))
            except Exception:
                pass

    @staticmethod
    def _pair_by_source(entries, trans_full):
        """Return {key -> translation} for the given DAT rows by pairing on
        SOURCE text — the same offset-drift-resilient logic as
        `_heal_translations`, but pure/stateless so the global open-items scan
        can heal ANY file without disturbing the currently-loaded one.

        Migrated rows (stored `source` present) match by source text via the
        same two-pass rule as `_heal_translations` (exact same-offset first,
        then positional distribution of leftovers); un-migrated rows (source
        None) fall back to the legacy offset key.
        """
        from collections import defaultdict
        pool = defaultdict(list)       # (entry, source) -> [[key, tr], ...]
        legacy = {}
        for key, rec in trans_full.items():
            src = rec.get("source")
            tr  = rec.get("translation", "")
            if src is None:
                legacy[key] = tr
            else:
                ent = key.split(":", 1)[0]
                pool[(ent, src)].append([key, tr])

        current = []
        for e in entries:
            num = e["num"]
            for s in e["strings"]:
                key = f"{num}:{s['offset']}"
                current.append((key, num, s.get("full_raw") or s["raw"]))

        out = {}
        unmatched = []
        for key, num, src in current:           # pass 1: exact offset match
            lst = pool.get((num, src))
            hit = None
            if lst:
                for i, item in enumerate(lst):
                    if item is not None and item[0] == key:
                        hit = i
                        break
            if hit is not None:
                tr = lst[hit][1]
                lst[hit] = None
                if tr:
                    out[key] = tr
            else:
                unmatched.append((key, num, src))
        for key, num, src in unmatched:         # pass 2: positional leftovers
            lst = pool.get((num, src))
            chosen = None
            if lst:
                for i, item in enumerate(lst):
                    if item is not None:
                        chosen = i
                        break
            if chosen is not None:
                tr = lst[chosen][1]
                lst[chosen] = None
                if tr:
                    out[key] = tr
                continue
            if key in legacy and legacy[key]:
                out[key] = legacy[key]
        return out

    def _auto_fill_ctrl_only(self):
        """Auto-copy source -> translation for any string that consists only
        of control codes (e.g. `[14:42]`, `[0C:06][17:41:00]`).

        These have no real text to translate; the engine still needs the
        codes intact, so the only correct translation IS the source.
        Strings filled this way are flagged STATUS_FINAL so they drop out
        of the Todo filter and the user doesn't waste time on them.
        Also auto-locks them — the per-string unlock button (in the
        ctrl-only banner) lets the user edit if they really need to.
        """
        if not self.entries:
            return 0
        filled = 0
        for e in self.entries:
            for s in e["strings"]:
                if s.get("suffix_of"):
                    continue
                if not _is_ctrl_only_raw(s["raw"]):
                    continue
                key = self._key(e["num"], s["offset"])
                if self.translations.get(key, "").strip():
                    continue
                self.translations[key] = s["raw"]
                self._string_status[key] = STATUS_FINAL
                filled += 1
        if filled:
            self._dirty = True
        return filled

    def _refresh_current_widget(self):
        """Re-render the currently-displayed string from `self.translations`.

        Required after batch operations (Clean PSX, Sync All, Export All,
        PSX import) that mutate translations on disk and reload them via
        `_load_translations()`. Without this, the editing widget still
        shows the pre-operation text — and a subsequent Save would commit
        that stale content back over the freshly cleaned/synced data.
        """
        if not self.entries:
            return
        try:
            e = self.entries[self.current_entry_idx]
            if self.current_str_idx < len(e["strings"]):
                self._display_string(e["strings"][self.current_str_idx])
        except Exception:
            pass

    def _apply_manual_dups(self):
        """Materialize each manual-duplicate master's translation into its
        locked targets so export/repack (which read the JSON per key) pick up
        the inherited value. Unlocked targets keep their own text. Cheap: the
        map is tiny and same-file, so all reads/writes hit self.translations."""
        for (tbase, tkey), (mbase, mkey) in MANUAL_DUP_MAP.items():
            if (tbase, tkey) in self._unlocked_dups:
                continue
            mval = self._read_trans_value(mbase, mkey) or ""
            cur  = self._read_trans_value(tbase, tkey) or ""
            if cur != mval:
                self._write_trans_value(tbase, tkey, mval)

    def _save(self, event=None):
        if not self.current_file:
            return
        self._commit_current()
        self._apply_manual_dups()      # mirror manual-dup masters into targets
        # Save translations
        path = os.path.join(TRANS_DIR, self.current_file.replace(".txt", ".json"))
        with open(path, "w", encoding="utf-8") as f:
            json.dump(self._build_save_dict(), f, ensure_ascii=False, indent=2,
                      sort_keys=True)
        # Save string status
        self._save_string_status()
        self._commit_note(refresh=False)   # flush any pending note edit
        self._save_notes()
        self._tm_dirty = True        # new translations -> refresh memory corpus
        self._dirty = False
        self.status_var.set(f"Saved  ->  {os.path.basename(path)}")
        self._populate_tree()
        self._calculate_overall_progress()
        self._update_overall_progress_display()
        if self.entries and self.current_str_idx < len(self.entries[self.current_entry_idx]["strings"]):
            s = self.entries[self.current_entry_idx]["strings"][self.current_str_idx]
            self._display_string(s)

    def _build_save_dict(self):
        """Serialize translations in the content-addressed dict form
        `{key: {"source": <base text>, "translation": <target>}}`.

        - Current rows with a non-empty translation are written with their live
          source text (self._cur_source), so the file re-describes itself and
          stays resilient to future DAT changes.
        - Orphaned stored entries (loaded but not matched to a current row) are
          preserved verbatim as a lossless backup — never dropped on save.
        """
        cur_src = getattr(self, "_cur_source", {})
        full    = getattr(self, "_trans_full", {})
        out = {}
        # Preserve orphans (stored keys that aren't current rows) verbatim.
        cur_keys = set(cur_src)
        for k, rec in full.items():
            if k not in cur_keys:
                out[k] = rec
        # Write current translated rows with live source.
        for k, tr in self.translations.items():
            if tr and tr.strip():
                out[k] = {"source": cur_src.get(k), "translation": tr}
        return out

    # Suffix lines in text_dump end with `  # suffix:[0xNNNN]` after the
    # closing quote; STR_RE's $-anchor refuses to match them as-is. We
    # strip the trailing comment before matching so suffix-slot
    # translations from JSON propagate into the exported .txt and
    # repack_text picks them up (necessary for the Option B per-slot
    # multi-item layout). Matched PRECISELY (only `# suffix:[0x..]` at end of
    # line) so content containing " # " — e.g. "with # of faeries" — is not
    # truncated, which previously made STR_RE fail and skipped the line.
    _SUFFIX_COMMENT_RE = re.compile(r"\s+#\s+suffix:\[0x[0-9A-Fa-f]+\]\s*$")

    def _export(self):
        if not self.current_file:
            return
        self._save()
        src_path = os.path.join(DUMP_DIR, self.current_file)
        out_path = os.path.join(TRANS_DIR, self.current_file)
        cur_num = None
        out_lines = []
        with open(src_path, encoding="utf-8") as f:
            for line in f:
                stripped = line.rstrip("\n")
                m_e = re.match(r"## Entry (\d+) @ ", stripped)
                if m_e:
                    cur_num = m_e.group(1)
                stripped_nc = self._SUFFIX_COMMENT_RE.sub("", stripped)
                m_s = STR_RE.match(stripped_nc)
                if m_s and cur_num:
                    offset = m_s.group(1)
                    key    = self._key(cur_num, offset)
                    if key in self.translations and self.translations[key].strip():
                        raw_trans = display_to_export(self.translations[key])
                        out_lines.append(f"{offset} '{raw_trans}'\n")
                        continue
                out_lines.append(line)
        with open(out_path, "w", encoding="utf-8") as f:
            f.writelines(out_lines)
        messagebox.showinfo("Exported",
                            f"Modified .txt saved to:\n{out_path}\n\n"
                            f"Ready for repacking into DAT files.")

    def _export_all_modified(self):
        self._commit_current()
        self._save()
        self._apply_manual_dups()   # materialize masters into targets on disk
        exported = []
        for fname in sorted(os.listdir(TRANS_DIR)):
            if not fname.endswith(".json"):
                continue
            if _is_aux_json(fname):
                continue
            json_path = os.path.join(TRANS_DIR, fname)
            trans = load_translations_json(json_path)
            if not any(v.strip() for v in trans.values()):
                continue
            txt_name = fname.replace(".json", ".txt")
            src_path = os.path.join(DUMP_DIR, txt_name)
            if not os.path.exists(src_path):
                continue
            cur_num = None
            out_lines = []
            with open(src_path, encoding="utf-8") as f:
                for line in f:
                    stripped = line.rstrip("\n")
                    m_e = re.match(r"## Entry (\d+) @ ", stripped)
                    if m_e:
                        cur_num = m_e.group(1)
                    stripped_nc = self._SUFFIX_COMMENT_RE.sub("", stripped)
                    m_s = STR_RE.match(stripped_nc)
                    if m_s and cur_num:
                        offset = m_s.group(1)
                        key = self._key(cur_num, offset)
                        if key in trans and trans[key].strip():
                            raw_trans = display_to_export(trans[key])
                            out_lines.append(f"{offset} '{raw_trans}'\n")
                            continue
                    out_lines.append(line)
            out_path = os.path.join(TRANS_DIR, txt_name)
            with open(out_path, "w", encoding="utf-8") as f:
                f.writelines(out_lines)
            exported.append(txt_name)

        if exported:
            messagebox.showinfo("Export All",
                                f"Exported {len(exported)} files to translations/:\n\n"
                                + "\n".join(exported[:20])
                                + ("\n..." if len(exported) > 20 else ""))
        else:
            messagebox.showinfo("Export All", "No files with translations to export.")

    # ── Todo / audit export ───────────────────────────────────────────────
    # Regex catching PSX-only opcodes that *should* have been remapped
    # during import. If the German still carries one of these, the entry
    # was imported by a pre-Apr-2026 matcher (or had its leading remap
    # stripped by the old Clean PSX step 4) — flagged in the
    # `psx_codes_remaining` bucket so the user can re-import or run
    # Clean PSX over it.
    _PSX_RESIDUAL_OPCODE_RE = re.compile(r'\[(17|19):[0-9A-Fa-f:]+\]')
    # PSX [16:XX] is intentionally NOT flagged here even though it's
    # remappable, because legitimate PC sources also use [16:XX] (timed
    # text) so post-remap text legitimately contains it. The 17 and 19
    # opcodes are PSX-only and never appear in PC-format strings.

    def _export_todo(self):
        """Walk every text_dump file and its translation JSON, collect
        what's still missing, and dump one consolidated `todo.json` for
        offline inspection.

        Two buckets, both grouped by source filename:

          missing
              PC entries with no German at all. Excludes suffix entries
              (parent's translation covers them) and non-primary
              duplicates (the primary location holds the canonical
              translation that auto-syncs across files).

          psx_codes_remaining
              German entries that still carry PSX-only opcodes
              ([17:...] or [19:...]). Strong signal the entry was
              imported before the matcher's PSX→PC remap landed; either
              re-import with overwrite enabled, or run Clean PSX.

        Output: <TRANS_DIR>/todo.json. Asks the user where to put it
        so they can stash it elsewhere if needed.
        """
        from tkinter import filedialog
        import time

        # Build the shared index up front so duplicates are filtered
        # consistently with how the main string list filters them.
        if not self._shared_index:
            self.status_var.set("Building shared string index for todo export...")
            self.update()
            self._build_shared_index()

        missing = {}            # filename -> [{key, source, stripped}, ...]
        psx_remaining = {}      # filename -> [{key, source, translation,
                                #               psx_codes}, ...]
        per_file_summary = []   # (filename, missing_count, psx_remaining_count, total)

        total_translatable = 0
        total_missing = 0
        total_psx_residual = 0

        for fname in sorted(os.listdir(DUMP_DIR)):
            if not fname.endswith(".txt"):
                continue
            base = fname[:-4]
            dump_path = os.path.join(DUMP_DIR, fname)
            json_path = os.path.join(TRANS_DIR, base + ".json")

            trans = {}
            if os.path.exists(json_path):
                try:
                    trans = load_translations_json(json_path)
                except Exception:
                    trans = {}

            try:
                _, entries = parse_dump(dump_path)
            except Exception:
                continue

            file_missing = []
            file_psx_residual = []
            file_translatable = 0

            for e in entries:
                for s in e["strings"]:
                    if s.get("suffix_of"):
                        continue   # parent string carries the translation
                    key = self._key(e["num"], s["offset"])
                    is_dup, _, _ = self._is_duplicate(self._dup_raw(s), base, key)
                    if is_dup:
                        continue   # primary location is the source of truth
                    file_translatable += 1

                    raw = s["raw"]
                    src_display = raw_to_display(raw)
                    stripped = _strip_ctrl_codes(src_display).strip()
                    de = trans.get(key, "").strip()

                    # Don't count entries that have no displayable
                    # text content as "translatable". Sources like
                    # `[14:42]` or `[0C:01][14:41]` are pure formatting
                    # placeholders — the engine emits no glyphs from
                    # them, so there's nothing to translate. Counting
                    # them would inflate the missing count with rows
                    # the user can never satisfy. We require at least
                    # one alphabetic character in the stripped key.
                    if not any(c.isalpha() for c in stripped):
                        file_translatable -= 1   # undo the +=1 above
                        continue

                    if not de:
                        file_missing.append({
                            "key":      key,
                            "offset":   s["offset"],
                            "source":   src_display,
                            "stripped": stripped,
                        })
                    else:
                        residual = self._PSX_RESIDUAL_OPCODE_RE.findall(de)
                        if residual:
                            file_psx_residual.append({
                                "key":         key,
                                "offset":      s["offset"],
                                "source":      src_display,
                                "translation": de,
                                "psx_codes":   sorted(set(
                                    f"[{op}:...]" for op in residual)),
                            })

            if file_missing:
                missing[fname] = file_missing
            if file_psx_residual:
                psx_remaining[fname] = file_psx_residual
            total_translatable += file_translatable
            total_missing      += len(file_missing)
            total_psx_residual += len(file_psx_residual)
            per_file_summary.append(
                (fname, len(file_missing), len(file_psx_residual),
                 file_translatable))

        # Sort the per-file summary so the worst offenders show up at
        # the top of the JSON — the user is going to scan it visually.
        per_file_summary.sort(
            key=lambda t: (t[1] + t[2]), reverse=True)

        out = {
            "generated":           time.strftime("%Y-%m-%d %H:%M:%S"),
            "dump_dir":            DUMP_DIR,
            "trans_dir":           TRANS_DIR,
            "summary": {
                "files_scanned":         len(per_file_summary),
                "total_translatable":    total_translatable,
                "missing":               total_missing,
                "psx_codes_remaining":   total_psx_residual,
                "per_file": [
                    {"file": f, "missing": m, "psx_residual": r,
                     "translatable": t}
                    for f, m, r, t in per_file_summary
                    if m or r
                ],
            },
            "missing":             missing,
            "psx_codes_remaining": psx_remaining,
        }

        default_path = os.path.join(TRANS_DIR, "todo.json")
        save_path = filedialog.asksaveasfilename(
            title="Save todo export",
            initialfile="todo.json",
            initialdir=TRANS_DIR,
            defaultextension=".json",
            filetypes=[("JSON", "*.json"), ("All files", "*.*")])
        if not save_path:
            self.status_var.set("Todo export cancelled.")
            return

        try:
            with open(save_path, "w", encoding="utf-8") as f:
                json.dump(out, f, ensure_ascii=False, indent=2,
                          sort_keys=False)
        except Exception as ex:
            messagebox.showerror("Todo export",
                                 f"Could not write {save_path}:\n{ex}")
            return

        self.status_var.set(
            f"Todo export -> {os.path.basename(save_path)} "
            f"({total_missing} missing, {total_psx_residual} PSX residual)")
        messagebox.showinfo(
            "Todo export",
            f"Wrote {os.path.basename(save_path)}.\n\n"
            f"Total translatable PC entries: {total_translatable}\n"
            f"Missing translations:           {total_missing}\n"
            f"Stale PSX-coded entries:        {total_psx_residual}\n\n"
            f"The 'psx_codes_remaining' bucket lists German entries\n"
            f"that still carry [17:...] or [19:...] opcodes — those\n"
            f"were imported before the PSX->PC opcode remap landed.\n"
            f"Re-import with 'Overwrite existing translations' ticked,\n"
            f"or run Clean PSX, to refresh them.")

    # ── Global open/todo browser (all DATs at once) ──────────────────────────
    def _scan_open_items(self, include_dups=False):
        """Walk EVERY dump file + its source-healed translations and collect
        all still-untranslated, translatable rows across the whole project.

        Returns (rows, n_files_with_open). Each row is a dict:
            {file, base, key, entry, offset, source, is_dup, dup_base, dup_key,
             shared_n}

        Non-primary duplicates are excluded unless `include_dups`. A row is
        "open" iff its canonical translation is empty — for a duplicate that is
        the PRIMARY location's translation (they share one), so duplicates only
        appear while the shared string is genuinely untranslated anywhere.
        Pure-formatting placeholders (no alphabetic content) are skipped, the
        same rule the per-file Todo filter and Todo Export use.
        """
        if not self._shared_index:
            self._build_shared_index()

        # Pass 1: parse + source-heal every file once.
        per_file = {}   # base -> (entries, healed_map)
        for fname in sorted(os.listdir(DUMP_DIR)):
            if not fname.endswith(".txt"):
                continue
            base = fname[:-4]
            dump_path = os.path.join(DUMP_DIR, fname)
            try:
                _, entries = parse_dump(dump_path)
            except Exception:
                continue
            json_path = os.path.join(TRANS_DIR, base + ".json")
            trans_full = (load_translations_full(json_path)
                          if os.path.exists(json_path) else {})
            per_file[base] = (entries, self._pair_by_source(entries, trans_full))

        # Pass 2: collect open rows.
        rows = []
        files_with_open = set()
        for base, (entries, healed) in per_file.items():
            fname = base + ".txt"
            for e in entries:
                for s in e["strings"]:
                    if s.get("suffix_of"):
                        continue
                    key = self._key(e["num"], s["offset"])
                    is_dup, dup_base, dup_key = self._is_duplicate(
                        self._dup_raw(s), base, key)

                    # Translated? For a duplicate the canonical text lives at
                    # the primary location; look it up there.
                    if is_dup:
                        prim_healed = per_file.get(dup_base, (None, {}))[1]
                        translated = bool(prim_healed.get(dup_key, "").strip())
                    else:
                        translated = bool(healed.get(key, "").strip())
                    if translated:
                        continue

                    src_display = raw_to_display(s["raw"])
                    stripped = _strip_ctrl_codes(src_display).strip()
                    if not any(c.isalpha() for c in stripped):
                        continue   # pure formatting placeholder — nothing to do

                    if is_dup and not include_dups:
                        continue

                    rows.append({
                        "file":      fname,
                        "base":      base,
                        "key":       key,
                        "entry":     e["num"],
                        "offset":    s["offset"],
                        "source":    src_display.replace("\n", " | ")[:90],
                        "is_dup":    is_dup,
                        "dup_base":  dup_base,
                        "dup_key":   dup_key,
                        "shared_n":  self._get_shared_count(self._dup_raw(s)),
                    })
                    files_with_open.add(fname)
        return rows, len(files_with_open)

    def _open_todo_browser(self):
        """A single window listing every open/untranslated string across ALL
        DATs (not just the loaded one). Double-click a row to jump straight to
        it in the editor; toggle 'Include duplicates' to also surface shared
        locations (tagged DUP -> primary) so they can be unlocked for an
        independent translation."""
        # Re-focus an already-open browser instead of stacking windows.
        if getattr(self, "_todo_win", None) is not None:
            try:
                self._todo_win.deiconify()
                self._todo_win.lift()
                self._todo_win.focus_force()
                return
            except tk.TclError:
                self._todo_win = None

        win = tk.Toplevel(self)
        self._todo_win = win
        win.title("Open Items — all DATs")
        win.configure(bg=self.BG)
        win.geometry("980x620")

        def _on_close():
            self._todo_win = None
            win.destroy()
        win.protocol("WM_DELETE_WINDOW", _on_close)

        bar = tk.Frame(win, bg=self.BG2)
        bar.pack(fill=tk.X)
        count_lbl = tk.Label(bar, text="Scanning…", bg=self.BG2, fg=self.ACC_TRA,
                             font=("Consolas", 10))
        count_lbl.pack(side=tk.LEFT, padx=10, pady=5)

        inc_dups_var = tk.BooleanVar(value=False)
        tk.Checkbutton(bar, text="Include duplicates", variable=inc_dups_var,
                       bg=self.BG2, fg=self.FG, selectcolor=self.BG,
                       activebackground=self.BG2, activeforeground=self.FG2,
                       font=("Segoe UI", 9),
                       command=lambda: _refresh()).pack(side=tk.LEFT, padx=8)
        tk.Button(bar, text="Refresh", command=lambda: _refresh(),
                  bg="#4C5052", fg=self.FG, relief=tk.FLAT, padx=8,
                  font=("Segoe UI", 8)).pack(side=tk.LEFT, padx=4)
        tk.Label(bar, text="double-click a row to jump to it",
                 bg=self.BG2, fg="#888888",
                 font=("Segoe UI", 8)).pack(side=tk.RIGHT, padx=10)

        cols = ("file", "entry", "offset", "status", "source")
        tree = ttk.Treeview(win, columns=cols, show="headings")
        for c, txt, w in [("file", "File", 150), ("entry", "Entry", 55),
                          ("offset", "Offset", 75), ("status", "Status", 130),
                          ("source", "Source (English)", 540)]:
            tree.heading(c, text=txt)
            tree.column(c, width=w, anchor=tk.W,
                        stretch=(c == "source"))
        tree.tag_configure("todo", background=self.TODO_BG, foreground="#CCCCAA")
        tree.tag_configure("dup", background="#2B2B33", foreground="#8A8AA0")
        vs = ttk.Scrollbar(win, orient=tk.VERTICAL, command=tree.yview)
        tree.configure(yscrollcommand=vs.set)
        vs.pack(side=tk.RIGHT, fill=tk.Y)
        tree.pack(fill=tk.BOTH, expand=True)

        row_by_iid = {}

        def _refresh():
            for iid in tree.get_children():
                tree.delete(iid)
            row_by_iid.clear()
            count_lbl.config(text="Scanning…")
            win.update_idletasks()
            rows, n_files = self._scan_open_items(
                include_dups=inc_dups_var.get())
            n_dup = 0
            for i, r in enumerate(rows):
                iid = str(i)
                if r["is_dup"]:
                    n_dup += 1
                    status = f"DUP -> {r['dup_base']} {r['dup_key']}"
                    tag = "dup"
                else:
                    sh = r["shared_n"]
                    status = "TODO" + (f" (shared x{sh})" if sh > 1 else "")
                    tag = "todo"
                tree.insert("", tk.END, iid=iid,
                            values=(r["file"], r["entry"], r["offset"],
                                    status, r["source"]),
                            tags=(tag,))
                row_by_iid[iid] = r
            uniq = len(rows) - n_dup
            msg = f"{uniq} open across {n_files} file(s)"
            if inc_dups_var.get():
                msg += f"  ·  +{n_dup} duplicate location(s)"
            count_lbl.config(text=msg)

        def _on_double(event=None):
            sel = tree.selection()
            if not sel:
                return
            r = row_by_iid.get(sel[0])
            if not r:
                return
            # Jump to the row's OWN location (for a duplicate that's the dup
            # slot itself, where the Unlock button is available).
            self._goto_primary(r["base"], r["key"])
            self.lift()
            self.focus_force()
        tree.bind("<Double-1>", _on_double)
        tree.bind("<Return>", _on_double)

        win.after(50, _refresh)

    # Break codes: a plain newline (\n) and the [02] page-break. Excluded from
    # the code-check by default (translators re-wrap freely and the engine
    # page-breaks on its own), shown only when "Show breaks" is toggled on.
    @staticmethod
    def _break_diff(src_disp, trans_disp):
        """(missing, extra) multiset diff of BREAK tokens only — one '\\n' per
        newline and one '[02]' per page-break. Used to optionally fold breaks
        back into the code-check when the user asks to see them."""
        def toks(t):
            return (["\\n"] * t.count("\n")
                    + ["[02]"] * len(re.findall(r"\[02\]", t, re.I)))
        src, tr = toks(src_disp), toks(trans_disp)
        miss = list(src)
        for c in tr:
            if c in miss:
                miss.remove(c)
        ext = list(tr)
        for c in src:
            if c in ext:
                ext.remove(c)
        return miss, ext

    def _scan_code_issues(self, include_breaks=False):
        """Walk every dump + its source-healed translations and collect all
        TRANSLATED rows whose control codes don't match the source: codes in
        the source but dropped from the translation ("MISSING"), or codes added
        to the translation that aren't in the source ("EXTRA"). Only primary
        (non-duplicate) rows are examined, since a duplicate shares the primary's
        source AND translation.

        Plain newlines (\\n) and [02] page-breaks are ignored unless
        `include_breaks` — then they're folded back in as \\n / [02] tokens so a
        translator can audit line/page breaks too.

        Returns (rows, n_files_with_issues). Each row is a dict:
            {file, base, key, entry, offset, kind, codes, source}
        with kind in {"MISSING","EXTRA"} and codes the space-joined tokens.
        """
        if not self._shared_index:
            self._build_shared_index()

        # Pass 1: parse + source-heal every file once (same as _scan_open_items).
        per_file = {}
        for fname in sorted(os.listdir(DUMP_DIR)):
            if not fname.endswith(".txt"):
                continue
            base = fname[:-4]
            try:
                _, entries = parse_dump(os.path.join(DUMP_DIR, fname))
            except Exception:
                continue
            json_path = os.path.join(TRANS_DIR, base + ".json")
            trans_full = (load_translations_full(json_path)
                          if os.path.exists(json_path) else {})
            per_file[base] = (entries, self._pair_by_source(entries, trans_full))

        # Pass 2: diff codes on every translated primary row.
        rows = []
        files_with_issue = set()
        for base, (entries, healed) in per_file.items():
            fname = base + ".txt"
            for e in entries:
                for s in e["strings"]:
                    if s.get("suffix_of"):
                        continue
                    key = self._key(e["num"], s["offset"])
                    is_dup, _, _ = self._is_duplicate(
                        self._dup_raw(s), base, key)
                    if is_dup:
                        continue          # checked at the primary location
                    trans = healed.get(key, "")
                    if not trans or not trans.strip():
                        continue          # untranslated -> Open Items' job
                    src_full   = raw_to_display(s["raw"])
                    trans_full = raw_to_display(trans)
                    missing, extra = self._diff_ctrl_codes(src_full, trans_full)
                    if include_breaks:
                        bm, be = self._break_diff(src_full, trans_full)
                        missing = missing + bm
                        extra   = extra + be
                    if not missing and not extra:
                        continue
                    src_disp = src_full.replace("\n", " | ")[:90]
                    for kind, codes in (("MISSING", missing), ("EXTRA", extra)):
                        if codes:
                            rows.append({
                                "file": fname, "base": base, "key": key,
                                "entry": e["num"], "offset": s["offset"],
                                "kind": kind, "codes": " ".join(codes),
                                "source": src_disp,
                            })
                    files_with_issue.add(fname)
        return rows, len(files_with_issue)

    def _open_missing_codes(self):
        """Toolbar 'Missing CR Codes' — code-check focused on dropped codes."""
        self._open_codes_browser(focus="missing")

    def _open_added_codes(self):
        """Toolbar 'Added CR Codes' — code-check focused on added codes."""
        self._open_codes_browser(focus="extra")

    def _open_codes_browser(self, focus="missing"):
        """A single window listing every translated string across ALL DATs whose
        control codes don't match the source — MISSING (codes dropped from the
        translation) and EXTRA (codes added that aren't in the source). `focus`
        ('missing'|'extra') sets which kind is shown first; both checkboxes stay
        available. 'Show breaks' folds \\n / [02] back in (off by default).
        Double-click a row to jump straight to it. Mirrors the Open Items
        browser. The two toolbar buttons share this one window."""
        want_missing = (focus != "extra")
        want_extra   = (focus == "extra")

        # One shared window: if already open, just re-aim it at the clicked kind.
        if getattr(self, "_codes_win", None) is not None:
            try:
                self._codes_win.deiconify()
                self._codes_win.lift()
                self._codes_win.focus_force()
                self._codes_show_missing.set(want_missing)
                self._codes_show_extra.set(want_extra)
                self._codes_repaint()
                return
            except tk.TclError:
                self._codes_win = None

        win = tk.Toplevel(self)
        self._codes_win = win
        win.title("Code Check — all DATs")
        win.configure(bg=self.BG)
        win.geometry("980x620")

        def _on_close():
            self._codes_win = None
            win.destroy()
        win.protocol("WM_DELETE_WINDOW", _on_close)

        bar = tk.Frame(win, bg=self.BG2)
        bar.pack(fill=tk.X)
        count_lbl = tk.Label(bar, text="Scanning…", bg=self.BG2, fg=self.ACC_TRA,
                             font=("Consolas", 10))
        count_lbl.pack(side=tk.LEFT, padx=10, pady=5)

        show_missing = tk.BooleanVar(value=want_missing)
        show_extra   = tk.BooleanVar(value=want_extra)
        show_breaks  = tk.BooleanVar(value=False)
        # Expose the display filters so a second button-press can re-aim the
        # already-open window (see the re-focus branch above).
        self._codes_show_missing = show_missing
        self._codes_show_extra   = show_extra
        tk.Checkbutton(bar, text="Missing codes", variable=show_missing,
                       bg=self.BG2, fg=self.FG, selectcolor=self.BG,
                       activebackground=self.BG2, activeforeground=self.FG2,
                       font=("Segoe UI", 9),
                       command=lambda: _repaint()).pack(side=tk.LEFT, padx=8)
        tk.Checkbutton(bar, text="Added codes", variable=show_extra,
                       bg=self.BG2, fg=self.FG, selectcolor=self.BG,
                       activebackground=self.BG2, activeforeground=self.FG2,
                       font=("Segoe UI", 9),
                       command=lambda: _repaint()).pack(side=tk.LEFT, padx=8)
        tk.Checkbutton(bar, text="Show breaks (\\n, [02])", variable=show_breaks,
                       bg=self.BG2, fg=self.FG, selectcolor=self.BG,
                       activebackground=self.BG2, activeforeground=self.FG2,
                       font=("Segoe UI", 9),
                       command=lambda: _refresh()).pack(side=tk.LEFT, padx=8)
        tk.Button(bar, text="Refresh", command=lambda: _refresh(),
                  bg="#4C5052", fg=self.FG, relief=tk.FLAT, padx=8,
                  font=("Segoe UI", 8)).pack(side=tk.LEFT, padx=4)
        tk.Label(bar, text="double-click a row to jump to it",
                 bg=self.BG2, fg="#888888",
                 font=("Segoe UI", 8)).pack(side=tk.RIGHT, padx=10)

        cols = ("file", "entry", "offset", "kind", "codes", "source")
        tree = ttk.Treeview(win, columns=cols, show="headings")
        for c, txt, w in [("file", "File", 140), ("entry", "Entry", 50),
                          ("offset", "Offset", 70), ("kind", "Kind", 80),
                          ("codes", "Codes", 160),
                          ("source", "Source (English)", 440)]:
            tree.heading(c, text=txt)
            tree.column(c, width=w, anchor=tk.W, stretch=(c == "source"))
        tree.tag_configure("missing", background="#3A2B2B", foreground="#E0A0A0")
        tree.tag_configure("extra",   background="#2B2B3A", foreground="#A0A0E0")
        vs = ttk.Scrollbar(win, orient=tk.VERTICAL, command=tree.yview)
        tree.configure(yscrollcommand=vs.set)
        vs.pack(side=tk.RIGHT, fill=tk.Y)
        tree.pack(fill=tk.BOTH, expand=True)

        row_by_iid = {}
        all_rows = []   # cached scan; _repaint filters without rescanning

        def _repaint():
            for iid in tree.get_children():
                tree.delete(iid)
            row_by_iid.clear()
            sm, se = show_missing.get(), show_extra.get()
            n_m = n_e = 0
            files = set()
            for i, r in enumerate(all_rows):
                if r["kind"] == "MISSING":
                    if not sm:
                        continue
                    n_m += 1
                    tag = "missing"
                else:
                    if not se:
                        continue
                    n_e += 1
                    tag = "extra"
                iid = str(i)
                tree.insert("", tk.END, iid=iid,
                            values=(r["file"], r["entry"], r["offset"],
                                    r["kind"], r["codes"], r["source"]),
                            tags=(tag,))
                row_by_iid[iid] = r
                files.add(r["file"])
            count_lbl.config(
                text=f"{n_m} missing · {n_e} added  across {len(files)} file(s)"
                     + ("  (breaks shown)" if show_breaks.get() else ""))
        self._codes_repaint = _repaint

        def _refresh():
            nonlocal all_rows
            count_lbl.config(text="Scanning…")
            win.update_idletasks()
            all_rows, _ = self._scan_code_issues(
                include_breaks=show_breaks.get())
            _repaint()

        def _on_double(event=None):
            sel = tree.selection()
            if not sel:
                return
            r = row_by_iid.get(sel[0])
            if not r:
                return
            self._goto_primary(r["base"], r["key"])
            self.lift()
            self.focus_force()
        tree.bind("<Double-1>", _on_double)
        tree.bind("<Return>", _on_double)

        win.after(50, _refresh)

    # ── Navigation ─────────────────────────────────────────────────────────────
    def _go_to(self, idx):
        e = self.entries[self.current_entry_idx]
        strings = e["strings"]
        if not strings:
            return
        idx = max(0, min(idx, len(strings) - 1))
        self.current_str_idx = idx
        offset = strings[idx]["offset"]
        # If offset is not in tree (filtered out), select nearest visible
        if not self.tree.exists(offset):
            children = self.tree.get_children()
            if children:
                self.tree.selection_set(children[0])
                self.tree.see(children[0])
                self._on_str_select()
            return
        self.tree.selection_set(offset)
        self.tree.see(offset)
        self._on_str_select()

    def _prev_string(self):
        self._commit_current()
        self._go_to(self.current_str_idx - 1)

    def _next_string(self):
        self._commit_current()
        self._go_to(self.current_str_idx + 1)

    def _next_untrans(self):
        """Jump to next untranslated string, crossing entry and file boundaries."""
        self._commit_current()
        e       = self.entries[self.current_entry_idx]
        strings = e["strings"]
        current_base = self.current_file[:-4] if self.current_file.endswith(".txt") else self.current_file

        # Search in current entry first
        for i in range(self.current_str_idx + 1, len(strings)):
            s   = strings[i]
            if s.get("suffix_of"):
                continue
            key = self._key(e["num"], s["offset"])
            is_dup, _, _ = self._is_duplicate(self._dup_raw(s), current_base, key)
            if is_dup:
                continue
            if not self.translations.get(key, "").strip():
                self._go_to(i)
                return

        # Search remaining entries in current file
        for ei in range(self.current_entry_idx + 1, len(self.entries)):
            e2 = self.entries[ei]
            for si, s in enumerate(e2["strings"]):
                if s.get("suffix_of"):
                    continue
                key = self._key(e2["num"], s["offset"])
                is_dup, _, _ = self._is_duplicate(self._dup_raw(s), current_base, key)
                if is_dup:
                    continue
                if not self.translations.get(key, "").strip():
                    self.current_entry_idx = ei
                    self.entry_cb.current(ei)
                    self._load_entry()
                    self._go_to(si)
                    return

        # Search remaining files
        files = list(self.file_cb["values"])
        current_idx = files.index(self.current_file) if self.current_file in files else -1
        for fi in range(current_idx + 1, len(files)):
            fname = files[fi]
            base = fname[:-4]
            # Load translations for this file
            trans_path = os.path.join(TRANS_DIR, base + ".json")
            trans = {}
            if os.path.exists(trans_path):
                with open(trans_path, encoding="utf-8") as f:
                    trans = load_translations_json(trans_path)
            # Parse the file
            _, entries = parse_dump(os.path.join(DUMP_DIR, fname))
            for ei, e2 in enumerate(entries):
                for si, s in enumerate(e2["strings"]):
                    if s.get("suffix_of"):
                        continue
                    key = self._key(e2["num"], s["offset"])
                    is_dup, _, _ = self._is_duplicate(self._dup_raw(s), base, key)
                    if is_dup:
                        continue
                    if not trans.get(key, "").strip():
                        # Jump to this file/entry/string
                        self.file_cb.set(fname)
                        self._on_file_select()
                        if ei != self.current_entry_idx:
                            self.current_entry_idx = ei
                            self.entry_cb.current(ei)
                            self._load_entry()
                        self._go_to(si)
                        self.status_var.set(f"Jumped to {fname}")
                        return

        self.status_var.set("All strings translated across all files!")

    # ── Jump to primary ─────────────────────────────────────────────────────────
    def _on_goto_primary(self):
        if not self._goto_target:
            return
        # Sentinel: ctrl-only banner reuses this bar; the button means
        # "unlock for editing" instead of "navigate to primary".
        if self._goto_target[0] == "__unlock_ctrl_only__":
            self._unlocked_ctrl_only.add(self._goto_target[1])
            # Force-switch to the Raw tab and focus its editor. The
            # Formatted tab renders every ctrl code with elide=True, so
            # for a ctrl-only string the user would see an empty widget
            # and any typed bytes would be silently concatenated with
            # the hidden auto-filled codes when committed. Raw makes the
            # codes visible so editing them is what the user expects.
            try:
                self._trans_nb.select(0)
                self._last_trans_tab = 0
            except Exception:
                pass
            if self.entries and self.current_str_idx < len(
                    self.entries[self.current_entry_idx]["strings"]):
                self._display_string(
                    self.entries[self.current_entry_idx]["strings"][self.current_str_idx])
            try:
                self.trans_edit.focus_set()
                self.trans_edit.mark_set(tk.INSERT, tk.END)
            except Exception:
                pass
            return
        self._goto_primary(*self._goto_target)

    def _on_toggle_dup_unlock(self):
        """Unlock the current duplicate for independent editing, or re-lock an
        already-unlocked one back into the shared group."""
        if not self._dup_btn_target:
            return
        base, key, raw = self._dup_btn_target
        # Commit whatever is in the widgets first so a pending edit isn't lost
        # when we redraw (only meaningful when this location is already
        # editable, i.e. an unlocked duplicate being re-locked).
        self._commit_current()
        self._toggle_dup_unlock(base, key, raw)
        # Redraw with the new lock state BEFORE persisting, so the now-(un)locked
        # widgets reflect the seeded/restored value. Doing this first prevents
        # the _save() -> _commit_current pass from reading stale widget content
        # (an empty just-unlocked box) and wiping the seeded translation.
        if self.entries and self.current_str_idx < len(
                self.entries[self.current_entry_idx]["strings"]):
            self._display_string(
                self.entries[self.current_entry_idx]["strings"][self.current_str_idx])
        self._save()
        # If we just unlocked, drop focus into the editor ready to type.
        if (base, key) in self._unlocked_dups:
            try:
                self.trans_edit.focus_set()
                self.trans_edit.mark_set(tk.INSERT, tk.END)
            except Exception:
                pass

    def _on_toggle_master(self):
        """Set the current occurrence as its duplicate group's master (the one
        Sync All propagates from), or clear that choice back to the default
        first-occurrence primary."""
        if not self._master_btn_target:
            return
        base, key, raw = self._master_btn_target
        # Persist any pending edit first (the chosen master's value is what the
        # group will source from, so don't lose an in-progress edit on redraw).
        self._commit_current()
        self._save()
        self._set_dup_master(raw, base, key)
        # Re-show the current string with its new master state.
        if self.entries and self.current_str_idx < len(
                self.entries[self.current_entry_idx]["strings"]):
            self._display_string(
                self.entries[self.current_entry_idx]["strings"][self.current_str_idx])

    def _on_browse_dups(self):
        """Pop up a list of EVERY copy in the current duplicate group so you can
        jump to any of them and see which occurrence carries which translation —
        the reverse of "Go to primary". Any copy can be promoted to master here,
        which is the whole point: survey the group, then decide X -> Y."""
        raw = getattr(self, "_dupsbrowse_target", None)
        if not raw:
            return
        locs = list(self._shared_index.get(raw, []))
        if len(locs) < 2:
            return
        cur = getattr(self, "_dupsbrowse_cur", None)

        # Load each involved file's translations once, for the per-copy preview.
        trans_by_base = {}
        for b, _k in locs:
            if b not in trans_by_base:
                tp = os.path.join(TRANS_DIR, b + ".json")
                try:
                    trans_by_base[b] = (load_translations_json(tp)
                                        if os.path.exists(tp) else {})
                except Exception:
                    trans_by_base[b] = {}
        # Overlay the current file's in-memory (possibly unsaved) edits so the row
        # you're on previews what you actually see in the editor.
        try:
            cur_base = self.file_var.get()
            if cur_base.endswith(".txt"):
                cur_base = cur_base[:-4]
            if cur_base in trans_by_base and isinstance(self.translations, dict):
                trans_by_base[cur_base] = {**trans_by_base[cur_base],
                                           **self.translations}
        except Exception:
            pass

        win = tk.Toplevel(self)
        win.title("Duplicate group")
        win.configure(bg="#2A2436")
        win.geometry("760x420")
        win.transient(self)

        src = raw.replace("\n", " / ")
        if len(src) > 90:
            src = src[:90] + "..."
        tk.Label(win, text=f"{len(locs)} copies share this text:", bg="#2A2436",
                 fg="#CCAADD", font=("Segoe UI", 9, "bold")).pack(
                 anchor="w", padx=10, pady=(8, 0))
        tk.Label(win, text=src, bg="#2A2436", fg="#EEEEEE", wraplength=730,
                 justify="left", font=("Consolas", 9)).pack(
                 anchor="w", padx=10, pady=(0, 6))
        tk.Label(win,
                 text="★ = master (Sync All sources from it)   "
                      "▶ = the copy you're editing now.  "
                      "Double-click a row to jump to it.",
                 bg="#2A2436", fg="#9A8AAA", font=("Segoe UI", 8)).pack(
                 anchor="w", padx=10)

        frame = tk.Frame(win, bg="#2A2436")
        frame.pack(fill=tk.BOTH, expand=True, padx=10, pady=6)
        sb = tk.Scrollbar(frame)
        sb.pack(side=tk.RIGHT, fill=tk.Y)
        lb = tk.Listbox(frame, yscrollcommand=sb.set, activestyle="dotbox",
                        bg="#1E1A28", fg="#EEEEEE",
                        selectbackground="#5A4A6A", font=("Consolas", 9))
        lb.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        sb.config(command=lb.yview)

        def _rows():
            master = self._primary_loc.get(raw)
            lb.delete(0, tk.END)
            for b, k in locs:
                mark = ""
                if (b, k) == master:
                    mark += "★"
                if cur and (b, k) == cur:
                    mark += "▶"
                mark = (mark or "  ").ljust(3)
                tr = trans_by_base.get(b, {}).get(k, "")
                tr = tr.replace("\n", " ") if tr else "(untranslated)"
                if len(tr) > 46:
                    tr = tr[:46] + "..."
                lb.insert(tk.END, f"{mark} {b}.txt  {k:<14}  {tr}")
        _rows()

        def _selected_loc():
            sel = lb.curselection()
            return locs[sel[0]] if sel else None

        def _go(_evt=None):
            loc = _selected_loc()
            if not loc:
                return
            win.destroy()
            self._goto_primary(loc[0], loc[1])

        def _make_master():
            loc = _selected_loc()
            if not loc:
                return
            self._commit_current()
            self._save()
            self._set_dup_master(raw, loc[0], loc[1])
            _rows()   # refresh the star in place
            # Reflect the new master on the main editor too.
            if self.entries and self.current_str_idx < len(
                    self.entries[self.current_entry_idx]["strings"]):
                self._display_string(
                    self.entries[self.current_entry_idx]["strings"][self.current_str_idx])

        lb.bind("<Double-Button-1>", _go)

        bar = tk.Frame(win, bg="#2A2436")
        bar.pack(fill=tk.X, padx=10, pady=(0, 10))
        tk.Button(bar, text="Go to selected", command=_go, bg="#5A4A6A",
                  fg="white", relief=tk.FLAT, padx=10, pady=2, cursor="hand2",
                  font=("Segoe UI", 9, "bold")).pack(side=tk.LEFT)
        tk.Button(bar, text="Set selected as Master", command=_make_master,
                  bg="#4A6A5A", fg="white", relief=tk.FLAT, padx=10, pady=2,
                  cursor="hand2", font=("Segoe UI", 9, "bold")).pack(
                  side=tk.LEFT, padx=6)
        tk.Button(bar, text="Close", command=win.destroy, bg="#4C5052",
                  fg="white", relief=tk.FLAT, padx=10, pady=2, cursor="hand2",
                  font=("Segoe UI", 9)).pack(side=tk.RIGHT)

    def _on_toggle_seal(self):
        """Seal/unseal the current monster name, OR lock/unlock an uncensor-only
        slot — the shared seal button serves both (dispatched by target type)."""
        if not self._seal_btn_target:
            return
        base, key = self._seal_btn_target
        self._commit_current()
        self._save()
        if self._is_uncensor_entry(base, key):
            self._toggle_uncensor_unlock(base, key)
        else:
            self._toggle_monster_seal(base, key)
        if self.entries and self.current_str_idx < len(
                self.entries[self.current_entry_idx]["strings"]):
            self._display_string(
                self.entries[self.current_entry_idx]["strings"][self.current_str_idx])
        # If just unsealed/unlocked, drop focus into the editor ready to type.
        if ((base, key) in self._unsealed_monsters
                or (base, key) in self._unlocked_uncensor):
            try:
                self.trans_edit.focus_set()
                self.trans_edit.mark_set(tk.INSERT, tk.END)
            except Exception:
                pass

    def _goto_primary(self, base, key):
        target_file = base + ".txt"
        parts = key.split(":", 1)
        if len(parts) != 2:
            return
        target_entry_num = parts[0]
        target_offset = parts[1]

        if self.file_var.get() != target_file:
            files = list(self.file_cb["values"])
            if target_file not in files:
                self.status_var.set(f"File {target_file} not found!")
                return
            self.file_cb.set(target_file)
            self._on_file_select()

        for ei, e in enumerate(self.entries):
            if e["num"] == target_entry_num:
                if ei != self.current_entry_idx:
                    self.current_entry_idx = ei
                    self.entry_cb.current(ei)
                    self._load_entry()
                for si, s in enumerate(e["strings"]):
                    if s["offset"] == target_offset:
                        self._go_to(si)
                        return
                break
        self.status_var.set(f"String {key} not found in {target_file}")

    # ── Search ─────────────────────────────────────────────────────────────────
    def _on_match_case_change(self):
        # Toggling case-sensitivity changes results for the same query text —
        # invalidate the cache and re-run the current search if there is one.
        self._last_query = None
        self._search_hits = []
        self._search_pos = -1
        if self.search_var.get().strip():
            self._search_next()

    def _build_search_hits(self, query):
        if not query:
            return []
        match_case = self._match_case_var.get()
        # Case-fold both the query and the compared text unless Match Case is on.
        cf = (lambda t: t) if match_case else (lambda t: t.lower())
        q = cf(query)
        nq = _search_norm(query, lower=not match_case)   # code/whitespace-insensitive query
        hits = []
        self.status_var.set("Searching all files...")
        self.update()
        for fname in sorted(os.listdir(DUMP_DIR)):
            if not fname.endswith(".txt"):
                continue
            base = fname[:-4]
            path = os.path.join(DUMP_DIR, fname)
            _, entries = parse_dump(path)
            trans_path = os.path.join(TRANS_DIR, base + ".json")
            trans = {}
            if os.path.exists(trans_path):
                with open(trans_path, encoding="utf-8") as f:
                    trans = load_translations_json(trans_path)
            for ei, e in enumerate(entries):
                for si, s in enumerate(e["strings"]):
                    raw = cf(s["raw"])
                    key = self._key(e["num"], s["offset"])
                    tr = cf(trans.get(key, ""))
                    # Fast path: literal substring (lets you search for a raw
                    # control code like "[05:04]"). Fallback: control-code- and
                    # whitespace-insensitive match so plain text finds strings
                    # that carry inline codes / line breaks.
                    if (q in raw or q in tr or
                            (nq and (nq in _search_norm(s["raw"], lower=not match_case) or
                                     nq in _search_norm(trans.get(key, ""), lower=not match_case)))):
                        hits.append((fname, ei, si))
        return hits

    def _search_next(self):
        query = self.search_var.get().strip()
        if not query:
            return
        if not self._search_hits or getattr(self, '_last_query', '') != query:
            self._search_hits = self._build_search_hits(query)
            self._search_pos = -1
            self._last_query = query
            if query not in self._search_history:
                self._search_history.insert(0, query)
                self._search_history = self._search_history[:20]
                self.search_entry["values"] = self._search_history
        if not self._search_hits:
            self.search_result_lbl.config(text="No results")
            return
        self._search_pos = (self._search_pos + 1) % len(self._search_hits)
        self._go_to_search_hit()

    def _search_prev(self):
        query = self.search_var.get().strip()
        if not query:
            return
        if not self._search_hits or getattr(self, '_last_query', '') != query:
            self._search_hits = self._build_search_hits(query)
            self._search_pos = 0
            self._last_query = query
        if not self._search_hits:
            self.search_result_lbl.config(text="No results")
            return
        self._search_pos = (self._search_pos - 1) % len(self._search_hits)
        self._go_to_search_hit()

    def _go_to_search_hit(self):
        fname, ei, si = self._search_hits[self._search_pos]
        total = len(self._search_hits)
        self.search_result_lbl.config(
            text=f"{self._search_pos + 1}/{total} ({fname})")
        if self.file_var.get() != fname:
            self._commit_current()
            self.file_cb.set(fname)
            self._on_file_select()
        if ei != self.current_entry_idx:
            self._commit_current()
            self.current_entry_idx = ei
            self.entry_cb.current(ei)
            self._load_entry()
        self._go_to(si)

    # ── Formatted tab handling ─────────────────────────────────────────────────
    def _read_fmt_as_raw(self):
        display = self.trans_fmt.get("1.0", tk.END).rstrip("\n")
        display = re.sub(r'@\n', '@', display)
        # [02] is now visible+editable. Strip the decorative " >\n" that
        # follows it. We MUST consume the trailing \n too, otherwise each
        # Raw -> Formatted -> Raw round-trip leaks one literal \n into the
        # raw text, and the leak compounds on every tab switch.
        # An intentional user `\n` after `[02]` survives because the
        # formatter renders [02]\nText as `[02] >\n\nText`, so stripping
        # one decorative `\n` leaves the user's behind.
        display = re.sub(r'(\[02\]) >\n', r'\1', display)
        display = re.sub(r'(\[02\]) >$', r'\1', display)
        # Clean up orphan decorations left behind by partial deletions:
        # e.g. user removed the [02] but the " >\n" marker remains.
        display = re.sub(r'(?<!\[02\]) >\n', '', display)
        display = re.sub(r'(?<!\[02\]) >$', '', display)
        display = re.sub(r'(\[0[Bb]\])\.\.\.', r'\1', display)
        return display_to_raw(display)

    def _get_current_trans_text(self):
        """Read the current translation text from whichever tab has content.
        Always returns raw format (with \\n escapes)."""
        # Try Formatted tab first (may have edits not yet synced to Raw)
        try:
            fmt_content = self.trans_fmt.get("1.0", tk.END).rstrip("\n")
            if fmt_content.strip():
                return self._read_fmt_as_raw()
        except Exception:
            pass
        # Then Raw tab
        raw = self.trans_edit.get("1.0", tk.END).rstrip("\n")
        if raw.strip():
            return raw
        # Fall back to saved translation
        if self.entries and self.current_str_idx < len(self.entries[self.current_entry_idx]["strings"]):
            e = self.entries[self.current_entry_idx]
            s = e["strings"][self.current_str_idx]
            key = self._key(e["num"], s["offset"])
            return self.translations.get(key, "")
        return ""

    def _on_trans_tab_change(self, event=None):
        try:
            new_tab = self._trans_nb.index(self._trans_nb.select())
        except Exception:
            return

        prev_tab = self._last_trans_tab

        if new_tab == 1:
            # Switching TO Formatted: populate from Raw
            raw = self.trans_edit.get("1.0", tk.END).rstrip("\n")
            if raw.strip():
                display = raw_to_display(raw)
                self._render_fmt_editable(self.trans_fmt, display)
        elif new_tab == 2:
            # Switching TO Preview: always sync from current content
            if self.entries and self.current_str_idx < len(self.entries[self.current_entry_idx]["strings"]):
                s = self.entries[self.current_entry_idx]["strings"][self.current_str_idx]
                # If coming from Formatted, sync edits back to Raw first
                if prev_tab == 1:
                    try:
                        raw = self._read_fmt_as_raw()
                        if raw.strip():
                            self.trans_edit.delete("1.0", tk.END)
                            self._insert_colored(self.trans_edit, raw)
                    except Exception:
                        pass
                trans = self._get_current_trans_text()
                self._update_dialog_mockup(s, trans)
        elif new_tab == 0:
            # Switching TO Raw: populate from Formatted if coming from there
            if prev_tab == 1:
                raw = self._read_fmt_as_raw()
                if raw.strip():
                    self.trans_edit.delete("1.0", tk.END)
                    self._insert_colored(self.trans_edit, raw)

        self._last_trans_tab = new_tab
        # The re-render above cleared any misspell tags on the target widget.
        self._schedule_spellcheck(delay=1)

    def _render_fmt_editable(self, widget, text):
        widget.config(state=tk.NORMAL)
        widget.delete("1.0", tk.END)
        color_tag = None
        pos = 0
        at_start = True
        for m in CTRL_RE.finditer(text):
            seg = text[pos:m.start()]
            if seg:
                self._insert_fmt_text(widget, seg, color_tag, at_start)
                at_start = False
            code = m.group()
            inner = code[1:-1].upper()
            first_byte = inner.split(':')[0]

            if inner.startswith('05:') and len(inner) >= 5:
                color_code = inner[3:5]
                if color_code in TEXT_COLORS:
                    color_tag = f"tc_{color_code}"
                widget.insert(tk.END, code, "elided")
            elif inner == '06':
                color_tag = None
                widget.insert(tk.END, code, "elided")
            elif inner == '02':
                # Keep [02] VISIBLE and editable in the Formatted view
                # so it can be selected/backspaced like any other text.
                # Decorative " >" + newline stays for readability.
                widget.insert(tk.END, code, "pagebreak")
                widget.insert(tk.END, " >", "sep")
                widget.insert(tk.END, "\n")
                at_start = True
            elif inner == '0B':
                # [0B] is a pause/wait control with no visible glyph;
                # keep it elided so the Formatted view doesn't render
                # decorative dots that confuse byte counting.
                widget.insert(tk.END, code, "elided")
            elif inner.startswith('04:') and len(inner) >= 5:
                idx = int(inner[3:5], 16)
                widget.insert(tk.END, code, "elided")
                if idx not in PARTY_NAMES and _byte_to_unicode(idx) is not None:
                    # [04:XX] glyph-emit: green glyph chip, like [09:22:XX].
                    lbl = tk.Label(widget, text=_byte_to_unicode(idx),
                                   bg="#233329", fg="#66CC99",
                                   font=("Segoe UI", 10, "bold"), padx=1, pady=0)
                    widget.window_create(tk.END, window=lbl)
                    at_start = False
                else:
                    name = PARTY_NAMES.get(idx, f"Char#{idx}")
                    lbl = tk.Label(widget, text=name, bg="#2A3A2A",
                                   fg="#E0A040", font=("Segoe UI", 10, "bold"),
                                   padx=2, pady=0)
                    widget.window_create(tk.END, window=lbl)
            elif inner == '03':
                widget.insert(tk.END, code, "elided")
                lbl = tk.Label(widget, text="Hero", bg="#2A3A2A",
                               fg="#E0A040", font=("Segoe UI", 10, "bold"),
                               padx=2, pady=0)
                widget.window_create(tk.END, window=lbl)
            elif _glyph_emit_char(inner) is not None:
                # Glyph-emit opcode: hide the raw code but show the letter it
                # draws, so the line reads correctly and the byte survives edits.
                widget.insert(tk.END, code, "elided")
                lbl = tk.Label(widget, text=_glyph_emit_char(inner),
                               bg="#233329", fg="#66CC99",
                               font=("Segoe UI", 10, "bold"), padx=1, pady=0)
                widget.window_create(tk.END, window=lbl)
                at_start = False
            elif inner.startswith('09:'):
                widget.insert(tk.END, code, "elided")
                lbl = tk.Label(widget, text="{item}", bg="#2A3A2A",
                               fg="#8888CC", font=("Consolas", 9),
                               padx=2, pady=0)
                widget.window_create(tk.END, window=lbl)
            elif inner.startswith('07:'):
                widget.insert(tk.END, code, "elided")
                lbl = tk.Label(widget, text="{var}", bg="#2A3A2A",
                               fg="#8888CC", font=("Consolas", 9),
                               padx=2, pady=0)
                widget.window_create(tk.END, window=lbl)
            elif first_byte in self._HIDDEN_CODES:
                widget.insert(tk.END, code, "elided")
            else:
                widget.insert(tk.END, code, "elided")
            pos = m.end()
        remaining = text[pos:]
        if remaining:
            self._insert_fmt_text(widget, remaining, color_tag, at_start)

    def _insert_fmt_text(self, widget, seg, color_tag, at_start):
        tags = (color_tag,) if color_tag else ()
        parts = seg.split("@")
        for i, part in enumerate(parts):
            if i > 0:
                widget.insert(tk.END, "@", "elided")
                if not at_start or widget.get("1.0", tk.END).strip():
                    widget.insert(tk.END, "\n")
            if part:
                widget.insert(tk.END, part, tags)

    def _on_fmt_key(self, event):
        # Allow navigation keys
        if event.keysym in ('Left', 'Right', 'Up', 'Down', 'Home', 'End',
                             'Prior', 'Next', 'Shift_L', 'Shift_R',
                             'Control_L', 'Control_R', 'Alt_L', 'Alt_R',
                             'Tab', 'Escape', 'Menu', 'Win_L', 'Win_R',
                             'Caps_Lock', 'Num_Lock', 'Scroll_Lock',
                             'F1', 'F2', 'F3', 'F4', 'F5', 'F6',
                             'F7', 'F8', 'F9', 'F10', 'F11', 'F12'):
            return
        # Allow Ctrl shortcuts (Ctrl+S, Ctrl+C, Ctrl+V, Ctrl+Z, etc.)
        if event.state & 0x4:
            return
        # Enter = line break
        if event.keysym == 'Return':
            self.trans_fmt.insert(tk.INSERT, "\n")
            self._refresh_byte_counter()
            return "break"
        # Allow backspace, delete
        if event.keysym in ('BackSpace', 'Delete'):
            return
        # Allow printable characters (space, letters, digits, punctuation)
        if event.char and ord(event.char) >= 0x20:
            return
        # Block everything else (Alt combos, etc.)
        return "break"

    def _on_fmt_key_release(self, event=None):
        self._refresh_byte_counter()
        self._revalidate_live()
        self._schedule_spellcheck()

    def _count_bytes(self, text: str) -> int:
        n, pos = 0, 0
        while pos < len(text):
            m = CTRL_RE.match(text, pos)
            if m:
                n += m.group().count(':') + 1
                pos = m.end()
            else:
                n += 1
                pos += 1
        return n

    def _refresh_byte_counter(self):
        if not hasattr(self, 'byte_lbl'):
            return
        try:
            active = self._trans_nb.index(self._trans_nb.select())
        except Exception:
            active = 0
        if active == 1:
            content = self._read_fmt_as_raw()
        else:
            content = self.trans_edit.get("1.0", tk.END).rstrip("\n")
        n   = self._count_bytes(content)
        src = getattr(self, '_src_byte_count', 0)
        if src:
            delta = n - src
            sign  = '+' if delta > 0 else ''
            color = "#CC7777" if delta > 15 else "#66AA66" if delta <= 0 else "#AAAAAA"
            self.byte_lbl.config(
                text=f"~{n} B  (src {src} B  d{sign}{delta})", fg=color)
        else:
            self.byte_lbl.config(text=f"~{n} B", fg="#666666")

    def _on_trans_key(self, event=None):
        w = self.trans_edit
        w.tag_remove("ctrl",     "1.0", tk.END)
        w.tag_remove("at_break", "1.0", tk.END)
        content = w.get("1.0", tk.END)
        for m in CTRL_RE.finditer(content):
            w.tag_add("ctrl", f"1.0+{m.start()}c", f"1.0+{m.end()}c")
        for m in re.finditer(r"@", content):
            w.tag_add("at_break", f"1.0+{m.start()}c", f"1.0+{m.end()}c")
        self._refresh_byte_counter()
        self._revalidate_live()
        self._schedule_spellcheck()

    def _revalidate_live(self):
        """Re-run _validate_current with the live editor content so warnings
        (e.g. enemy-name 8-byte cap) update as the translator types instead
        of only on string navigation."""
        if not self.entries:
            return
        if not (0 <= self.current_entry_idx < len(self.entries)):
            return
        e = self.entries[self.current_entry_idx]
        if not (0 <= self.current_str_idx < len(e["strings"])):
            return
        s = e["strings"][self.current_str_idx]
        try:
            active = self._trans_nb.index(self._trans_nb.select())
        except Exception:
            active = 0
        if active == 1:
            trans = self._read_fmt_as_raw()
        else:
            trans = self.trans_edit.get("1.0", tk.END).rstrip("\n")
        is_locked = bool(s.get("suffix_of"))
        try:
            self._validate_current(s, trans, is_locked)
        except Exception:
            # Validation must never crash the keypress handler.
            pass

    # ── Escape-token quick insert ───────────────────────────────────────────
    def _insert_escape(self, token):
        """Insert a control/escape token (\\x7f, \\n, [02], ~, …) at the Raw
        translation editor's cursor. Switches to the Raw tab and focuses it so
        the token always lands somewhere predictable."""
        try:
            self._trans_nb.select(0)   # Raw tab
        except Exception:
            pass
        w = self.trans_edit
        if str(w.cget("state")) == "disabled":
            self.status_var.set("String is locked — can't insert here.")
            return
        w.focus_set()
        w.insert(tk.INSERT, token)
        self._on_trans_key(None)       # re-tag codes, byte count, revalidate
        self._schedule_spellcheck()

    # ── Spell-check ─────────────────────────────────────────────────────────
    def _init_spell(self):
        """(Re)build the SpellManager from config. Safe if the module or a
        dictionary is missing — spell-check simply stays inactive."""
        if _spell is None:
            self._spell = None
            return
        try:
            self._spell = _spell.SpellManager(
                DICT_DIR,
                enabled=self._config.get("spell_enabled", []),
                personal=self._config.get("spell_personal", []))
        except Exception:
            self._spell = None

    def _spell_active(self):
        return bool(self._spell and self._spell.has_active())

    def _schedule_spellcheck(self, delay=400):
        """Debounced re-underline of BOTH translation editors (Raw + Formatted)."""
        if not hasattr(self, "trans_edit"):
            return
        if not self._spell_active():
            self.trans_edit.tag_remove("misspell", "1.0", tk.END)
            if hasattr(self, "trans_fmt"):
                self.trans_fmt.tag_remove("misspell", "1.0", tk.END)
            return
        if self._spell_after:
            try:
                self.after_cancel(self._spell_after)
            except Exception:
                pass
        self._spell_after = self.after(delay, self._spell_underline_all)

    def _spell_underline_all(self):
        self._spell_after = None
        self._spell_underline()       # Raw editor (plain-offset mapping)
        self._spell_underline_fmt()   # Formatted editor (window/elide-aware)

    def _spell_underline(self):
        w = getattr(self, "trans_edit", None)
        if w is None:
            return
        w.tag_remove("misspell", "1.0", tk.END)
        if not self._spell_active():
            return
        text = w.get("1.0", "end-1c")
        masked = self._mask_noncheck(text)
        for m in _spell.SpellManager.WORD_RE.finditer(masked):
            word = text[m.start():m.end()]
            if len(word) < 2:
                continue
            if not self._spell.check(word):
                w.tag_add("misspell", f"1.0+{m.start()}c", f"1.0+{m.end()}c")

    def _spell_underline_fmt(self):
        """Underline misspellings in the Formatted editor. It embeds control
        codes as *elided* text and some glyphs as embedded windows, so plain
        `1.0+Nc` offsets don't line up — we walk real text runs via `dump` and
        map each word to its true widget index within its run."""
        w = getattr(self, "trans_fmt", None)
        if w is None:
            return
        w.tag_remove("misspell", "1.0", tk.END)
        if not self._spell_active():
            return
        try:
            segments = w.dump("1.0", "end-1c", text=True)
        except Exception:
            return
        for kind, val, index in segments:
            if kind != "text" or not val:
                continue
            masked = self._mask_noncheck(val)
            for m in _spell.SpellManager.WORD_RE.finditer(masked):
                word = val[m.start():m.end()]
                if len(word) < 2:
                    continue
                if not self._spell.check(word):
                    w.tag_add("misspell",
                              w.index(f"{index}+{m.start()}c"),
                              w.index(f"{index}+{m.end()}c"))

    @staticmethod
    def _mask_noncheck(text):
        """Length-preserving copy of `text` with control codes / \\xNN escapes /
        \\n / ~ blanked to spaces, so the spell tokenizer never treats them as
        words."""
        out = list(text)
        for m in re.finditer(r'\[[0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2})*\]'
                             r'|\\x[0-9A-Fa-f]{2}|\\n|~', text):
            for i in range(m.start(), m.end()):
                out[i] = ' '
        return ''.join(out)

    def _on_trans_right_click(self, event):
        """Right-click a red-underlined word (in either editor) for suggestions."""
        w = event.widget
        if w not in (getattr(self, "trans_edit", None),
                     getattr(self, "trans_fmt", None)):
            return
        if str(w.cget("state")) == "disabled" or not self._spell_active():
            return
        idx = w.index(f"@{event.x},{event.y}")
        ranges = w.tag_ranges("misspell")
        hit = None
        for i in range(0, len(ranges), 2):
            s, e = ranges[i], ranges[i + 1]
            if w.compare(s, "<=", idx) and w.compare(idx, "<", e):
                hit = (s, e)
                break
        if hit is None:
            return
        word = w.get(hit[0], hit[1])
        menu = tk.Menu(self, tearoff=0, bg=self.BG2, fg=self.FG,
                       activebackground=self.SEL, activeforeground="white")
        sugg = self._spell.suggest(word)
        if sugg:
            for s in sugg:
                menu.add_command(
                    label=s,
                    command=lambda s=s, a=hit[0], b=hit[1], wid=w:
                        self._spell_replace(wid, a, b, s))
        else:
            menu.add_command(label="(no suggestions)", state=tk.DISABLED)
        menu.add_separator()
        menu.add_command(label=f'Add “{word}” to dictionary',
                         command=lambda word=word: self._spell_add(word))
        menu.add_command(label=f'Ignore “{word}” (this session)',
                         command=lambda word=word: self._spell_ignore(word))
        try:
            menu.tk_popup(event.x_root, event.y_root)
        finally:
            menu.grab_release()
        return "break"

    def _spell_replace(self, w, start, end, repl):
        if str(w.cget("state")) == "disabled":
            return
        w.delete(start, end)
        w.insert(start, repl)
        # Commit + revalidate through the path that matches the edited widget.
        if w is getattr(self, "trans_fmt", None):
            self._on_fmt_key_release(None)
        else:
            self._on_trans_key(None)
        self._schedule_spellcheck(delay=1)

    def _spell_add(self, word):
        """Add a word to the persistent personal dictionary."""
        if not self._spell:
            return
        self._spell.add_personal(word)
        self._config["spell_personal"] = self._spell.personal_list()
        self._save_config()
        self._schedule_spellcheck(delay=1)

    def _spell_ignore(self, word):
        """Accept a word for this session only (not saved to config)."""
        if self._spell:
            self._spell.add_personal(word)
            self._schedule_spellcheck(delay=1)

    def _open_dictionaries(self):
        """Manage spell-check dictionaries: enable/disable installed ones (any
        number at once), install more from the LibreOffice GitHub repo, remove."""
        if _spell is None:
            messagebox.showerror(
                "Dictionaries",
                "Spell-check module loc_spellcheck.py is missing.")
            return
        if self._spell is None:
            self._init_spell()

        win = tk.Toplevel(self)
        win.title("Dictionaries — spell-check")
        win.geometry("560x540")
        win.configure(bg=self.BG)
        win.transient(self)

        tk.Label(win, text="Spell-check dictionaries", bg=self.BG, fg=self.FG2,
                 font=("Segoe UI", 12, "bold")).pack(pady=(10, 2))
        engine = ("spylls (full Hunspell)"
                  if _spell.SpellManager.spylls_available()
                  else "basic word-list — run 'pip install spylls' for better accuracy")
        tk.Label(win, text=f"Engine: {engine}", bg=self.BG, fg="#888888",
                 font=("Segoe UI", 8)).pack()

        inst_lf = tk.LabelFrame(
            win, text=" Installed  (tick to use — several may be combined) ",
            bg=self.BG, fg=self.FG, font=("Segoe UI", 9))
        inst_lf.pack(fill=tk.BOTH, expand=True, padx=12, pady=8)
        inst_holder = tk.Frame(inst_lf, bg=self.BG)
        inst_holder.pack(fill=tk.BOTH, expand=True, padx=6, pady=6)

        state = {"vars": {}}

        def refresh_installed():
            for c in inst_holder.winfo_children():
                c.destroy()
            state["vars"] = {}
            installed = self._spell.installed() if self._spell else []
            if not installed:
                tk.Label(inst_holder,
                         text="No dictionaries installed yet — use “Install from "
                              "GitHub” below.",
                         bg=self.BG, fg="#888888", font=("Segoe UI", 9),
                         wraplength=480, justify=tk.LEFT).pack(anchor=tk.W)
                return
            enabled = set(self._spell.enabled)
            for base in installed:
                v = tk.BooleanVar(value=base in enabled)
                state["vars"][base] = v
                row = tk.Frame(inst_holder, bg=self.BG)
                row.pack(fill=tk.X, anchor=tk.W)
                tk.Checkbutton(row, text=base, variable=v, bg=self.BG, fg=self.FG,
                               selectcolor=self.BG3, activebackground=self.BG,
                               activeforeground=self.FG,
                               font=("Segoe UI", 10)).pack(side=tk.LEFT)
                tk.Button(row, text="Remove", bg="#5A3A3A", fg="white",
                          relief=tk.FLAT, font=("Segoe UI", 8), padx=6,
                          command=lambda b=base: remove_dict(b)).pack(
                              side=tk.RIGHT, padx=4)

        def apply_enabled():
            bases = [b for b, v in state["vars"].items() if v.get()]
            self._spell.set_enabled(bases)
            self._config["spell_enabled"] = bases
            self._save_config()
            self._schedule_spellcheck(delay=1)
            self.status_var.set(
                f"Spell-check: {len(bases)} dictionary(ies) active."
                if bases else "Spell-check: off.")

        def remove_dict(base):
            if not messagebox.askyesno("Remove",
                                       f"Delete dictionary “{base}”?", parent=win):
                return
            for ext in (".dic", ".aff"):
                try:
                    os.remove(os.path.join(DICT_DIR, base + ext))
                except Exception:
                    pass
            if base in self._spell.enabled:
                self._spell.enabled.remove(base)
            self._spell.set_enabled(self._spell.enabled)
            self._config["spell_enabled"] = self._spell.enabled
            self._save_config()
            refresh_installed()

        dl_lf = tk.LabelFrame(
            win, text=" Install from LibreOffice/dictionaries (GitHub) ",
            bg=self.BG, fg=self.FG, font=("Segoe UI", 9))
        dl_lf.pack(fill=tk.X, padx=12, pady=(0, 8))
        row1 = tk.Frame(dl_lf, bg=self.BG)
        row1.pack(fill=tk.X, padx=6, pady=6)
        tk.Label(row1, text="Language:", bg=self.BG, fg=self.FG,
                 font=("Segoe UI", 9)).pack(side=tk.LEFT)
        lang_var = tk.StringVar()
        lang_cb = ttk.Combobox(row1, textvariable=lang_var, width=10,
                               state="readonly", font=("Consolas", 9))
        lang_cb.pack(side=tk.LEFT, padx=6)
        dict_var = tk.StringVar()
        dict_cb = ttk.Combobox(row1, textvariable=dict_var, width=20,
                               state="readonly", font=("Consolas", 9))
        dict_cb.pack(side=tk.LEFT, padx=6)
        online = {"dicts": []}
        dl_status = tk.Label(dl_lf, text="", bg=self.BG, fg="#888888",
                             font=("Segoe UI", 8), anchor=tk.W)
        dl_status.pack(fill=tk.X, padx=8, pady=(0, 4))

        def load_langs():
            dl_status.config(text="Fetching language list…")
            win.update_idletasks()
            try:
                langs = [l for l in _spell.list_online_languages()
                         if not l.startswith(".")]
            except Exception as ex:
                dl_status.config(text=f"Fetch failed: {ex}")
                return
            lang_cb["values"] = langs
            dl_status.config(text=f"{len(langs)} languages available — pick one.")

        def on_lang(*_):
            folder = lang_var.get()
            if not folder:
                return
            dl_status.config(text=f"Fetching {folder}…")
            win.update_idletasks()
            try:
                online["dicts"] = _spell.list_online_dictionaries(folder)
            except Exception as ex:
                dl_status.config(text=f"Fetch failed: {ex}")
                return
            bases = [d["base"] for d in online["dicts"]]
            dict_cb["values"] = bases
            if bases:
                dict_cb.current(0)
            dl_status.config(text=f"{len(bases)} dictionary(ies) in {folder}.")

        def do_install():
            base = dict_var.get()
            entry = next((d for d in online["dicts"] if d["base"] == base), None)
            if not entry:
                dl_status.config(text="Pick a dictionary first.")
                return
            dl_status.config(text=f"Downloading {base}…")
            win.update_idletasks()
            try:
                _spell.install_dictionary(DICT_DIR, entry)
            except Exception as ex:
                dl_status.config(text=f"Download failed: {ex}")
                return
            dl_status.config(text=f"Installed {base}. Tick it above to use it.")
            refresh_installed()

        lang_var.trace_add("write", on_lang)
        tk.Button(row1, text="Install", bg="#3A5A3A", fg="white", relief=tk.FLAT,
                  font=("Segoe UI", 9), padx=8,
                  command=do_install).pack(side=tk.LEFT, padx=6)
        tk.Button(row1, text="↻", bg="#3A4A5A", fg="white", relief=tk.FLAT,
                  font=("Segoe UI", 9), padx=6, command=load_langs).pack(
                      side=tk.RIGHT, padx=4)

        btnf = tk.Frame(win, bg=self.BG)
        btnf.pack(fill=tk.X, padx=12, pady=10)
        tk.Button(btnf, text="Apply & Close", bg="#3A5A3A", fg="white",
                  relief=tk.FLAT, font=("Segoe UI", 10), padx=12, pady=3,
                  command=lambda: (apply_enabled(), win.destroy())).pack(side=tk.RIGHT)
        tk.Button(btnf, text="Apply", bg="#3A4A5A", fg="white", relief=tk.FLAT,
                  font=("Segoe UI", 10), padx=12, pady=3,
                  command=apply_enabled).pack(side=tk.RIGHT, padx=6)

        refresh_installed()
        self.after(50, load_langs)

    def _copy_source_to_trans(self):
        e = self.entries[self.current_entry_idx] if self.entries else None
        if not e or self.current_str_idx >= len(e["strings"]):
            return
        s    = e["strings"][self.current_str_idx]
        disp = raw_to_display(s["raw"])
        self.trans_edit.config(state=tk.NORMAL)
        self.trans_edit.delete("1.0", tk.END)
        self._insert_colored(self.trans_edit, disp)

    def _update_status(self):
        if not self.entries:
            return
        e     = self.entries[self.current_entry_idx]
        total = len(e["strings"])
        done  = sum(1 for s in e["strings"]
                    if self.translations.get(self._key(e["num"], s["offset"]), "").strip())
        self.status_var.set(
            f"{self.current_file}  |  String {self.current_str_idx + 1} / {total}  |  "
            f"{done} / {total} translated"
        )

    # ── String status flags ────────────────────────────────────────────────────
    def _load_string_status(self):
        path = os.path.join(TRANS_DIR,
                            self.current_file.replace(".txt", "_status.json"))
        if os.path.exists(path):
            with open(path, encoding="utf-8") as f:
                self._string_status = json.load(f)
        else:
            self._string_status = {}

    def _save_string_status(self):
        if not self._string_status:
            return
        path = os.path.join(TRANS_DIR,
                            self.current_file.replace(".txt", "_status.json"))
        with open(path, "w", encoding="utf-8") as f:
            json.dump(self._string_status, f, ensure_ascii=False, indent=2,
                      sort_keys=True)

    # ── Translator notes (per-string, `<file>_notes.json` sidecar) ──────────────
    def _load_notes(self):
        path = os.path.join(TRANS_DIR,
                            self.current_file.replace(".txt", "_notes.json"))
        if os.path.exists(path):
            try:
                with open(path, encoding="utf-8") as f:
                    self._notes = json.load(f)
            except Exception:
                self._notes = {}
        else:
            self._notes = {}

    def _save_notes(self):
        if not self._notes:
            return
        path = os.path.join(TRANS_DIR,
                            self.current_file.replace(".txt", "_notes.json"))
        with open(path, "w", encoding="utf-8") as f:
            json.dump(self._notes, f, ensure_ascii=False, indent=2,
                      sort_keys=True)

    def _current_key(self):
        """Offset key of the row currently shown, or None."""
        if not self.entries:
            return None
        e = self.entries[self.current_entry_idx]
        if self.current_str_idx >= len(e["strings"]):
            return None
        s = e["strings"][self.current_str_idx]
        return self._key(e["num"], s["offset"])

    def _commit_note(self, event=None, refresh=True):
        """Persist the note box content into self._notes for the row the box was
        populated for (tracked in self._note_key), so a late FocusOut after the
        selection already moved still writes to the correct row. `refresh` is
        False for internal flushes (display switch / save) that repopulate the
        tree themselves."""
        key = getattr(self, "_note_key", None)
        if key is None:
            return
        txt = self._note_var.get().strip()
        old = self._notes.get(key, "")
        if txt == old:
            return
        if txt:
            self._notes[key] = txt
        elif key in self._notes:
            del self._notes[key]
        self._save_notes()
        if refresh:
            self._populate_tree()   # note marker (✎) appears/disappears now

    # ── Translation Memory (fuzzy reuse of prior translations) ──────────────────
    def _build_tm_index(self):
        """Scan every translations JSON and collect (norm_source, source,
        translation) triples for non-empty, source-bearing rows. Deduped by
        normalized source (first non-empty translation wins). Also builds an
        inverted word index (word -> [entry idx]) so suggestions only run the
        O(n*m) difflib ratio on candidates that share words with the query —
        that keeps per-lookup time under a millisecond even for a 7k+ corpus.
        Gated by self._tm_dirty so it only rebuilds after edits/saves."""
        from collections import defaultdict
        entries = []
        widx = defaultdict(list)
        seen = set()
        if os.path.isdir(TRANS_DIR):
            for fname in sorted(os.listdir(TRANS_DIR)):
                if not fname.endswith(".json") or _is_aux_json(fname):
                    continue
                try:
                    full = load_translations_full(os.path.join(TRANS_DIR, fname))
                except Exception:
                    continue
                for rec in full.values():
                    src = rec.get("source")
                    tr  = rec.get("translation", "")
                    if not src or not tr or not tr.strip():
                        continue
                    ns = _search_norm(src)
                    if not ns or ns in seen:
                        continue
                    seen.add(ns)
                    idx = len(entries)
                    entries.append((ns, src, tr))
                    for w in set(ns.split()):
                        widx[w].append(idx)
        self._tm_entries = entries
        self._tm_word_index = widx
        self._tm_dirty = False

    def _tm_suggest(self, source_text, limit=6, threshold=0.55):
        """Return up to `limit` (ratio, source, translation) tuples whose source
        is most similar to `source_text`, best first. Exact matches (ratio 1.0)
        included — reusing an identical prior translation is the best case.
        Only entries sharing at least half the query's words are scored (via the
        inverted index), so this stays fast on the full-project corpus."""
        if self._tm_dirty:
            self._build_tm_index()
        nq = _search_norm(source_text)
        qwords = set(nq.split())
        if not qwords or not self._tm_entries:
            return []
        # Gather candidates sharing enough words with the query.
        need = max(1, (len(qwords) + 1) // 2)
        from collections import defaultdict
        shared = defaultdict(int)
        for w in qwords:
            for idx in self._tm_word_index.get(w, ()):
                shared[idx] += 1
        cands = [idx for idx, c in shared.items() if c >= need]
        sm = difflib.SequenceMatcher()
        sm.set_seq2(nq)
        out = []
        for idx in cands:
            ns, src, tr = self._tm_entries[idx]
            if ns == nq:
                out.append((1.0, src, tr))
                continue
            sm.set_seq1(ns)
            r = sm.ratio()
            if r >= threshold:
                out.append((r, src, tr))
        out.sort(key=lambda t: t[0], reverse=True)
        return out[:limit]

    def _refresh_tm_panel(self, source_text):
        """Fill the Memory listbox with suggestions for `source_text`."""
        if not hasattr(self, "_tm_list"):
            return
        self._tm_list.delete(0, tk.END)
        self._tm_suggestions = []
        sugg = self._tm_suggest(source_text) if source_text else []
        for ratio, src, tr in sugg:
            pct = int(round(ratio * 100))
            preview = tr.replace("\\n", " / ").replace("\n", " / ")
            if len(preview) > 80:
                preview = preview[:79] + "…"
            self._tm_list.insert(tk.END, f"{pct:3d}%  {preview}")
            self._tm_suggestions.append(tr)
        if not sugg:
            self._tm_hint.config(text="(no similar strings translated yet)")
        else:
            self._tm_hint.config(
                text="(similar strings you've translated — double-click to apply)")

    def _apply_tm_suggestion(self, event=None):
        """Drop the selected memory suggestion into the Raw translation editor."""
        sel = self._tm_list.curselection()
        if not sel:
            return
        idx = sel[0]
        if idx >= len(self._tm_suggestions):
            return
        tr = self._tm_suggestions[idx]
        # Don't apply onto a locked row (suffix / duplicate / ctrl-only) — the
        # editor would show the text but never persist it.
        if str(self.trans_edit["state"]) == tk.DISABLED:
            self.status_var.set("This row is locked — can't apply a suggestion here.")
            return
        self._trans_nb.select(0)                    # Raw tab
        self.trans_edit.config(state=tk.NORMAL)
        self.trans_edit.delete("1.0", tk.END)
        self._insert_colored(self.trans_edit, tr)
        self.trans_edit.focus_set()
        self.trans_edit.mark_set(tk.INSERT, tk.END)
        self._commit_current()
        self._populate_tree()
        self.status_var.set("Applied translation-memory suggestion.")

    def _cycle_status(self):
        """Cycle status flag: none -> draft -> reviewed -> final -> none.

        If multiple rows are selected in the tree, the CURRENT row's
        status is advanced once and the new value is written to EVERY
        selected row. That makes bulk flips (select 20 rows, hit the
        Status button) produce a uniform result instead of each row
        advancing from its own individual starting point.
        """
        if not self.entries:
            return
        e = self.entries[self.current_entry_idx]
        if self.current_str_idx >= len(e["strings"]):
            return

        # Collect target offsets — all selected rows, or just the
        # current one if the selection is empty (happens right after
        # a file load before the user clicks anything).
        selected_offsets = list(self.tree.selection())
        if not selected_offsets:
            cur = e["strings"][self.current_str_idx]
            selected_offsets = [cur["offset"]]

        # Compute the NEW status once, based on the current (focused) row.
        cur_s   = e["strings"][self.current_str_idx]
        cur_key = self._key(e["num"], cur_s["offset"])
        current = self._string_status.get(cur_key, STATUS_NONE)
        idx = STATUS_CYCLE.index(current) if current in STATUS_CYCLE else 0
        new_status = STATUS_CYCLE[(idx + 1) % len(STATUS_CYCLE)]

        # Build a quick lookup: offset string (tree iid) -> string dict.
        by_offset = {s["offset"]: s for s in e["strings"]}

        for off in selected_offsets:
            s = by_offset.get(off)
            if not s:
                continue
            key = self._key(e["num"], s["offset"])
            if new_status:
                self._string_status[key] = new_status
            elif key in self._string_status:
                del self._string_status[key]

        self._update_status_btn(new_status)
        self._dirty = True
        self._populate_tree()

        # Live color feedback: after populate, the new tag is set on the
        # affected rows, but ttk's selection-highlight covers the tag bg
        # so the user can't SEE the color change on the currently-selected
        # row(s). Briefly leave rows unselected so the new tag color shows,
        # then re-select after a short delay so the user can keep cycling.
        try:
            self.tree.selection_remove(*self.tree.selection())
        except Exception:
            pass
        def _restore():
            try:
                if selected_offsets:
                    self.tree.selection_set(selected_offsets)
            except Exception:
                pass
        self.after(450, _restore)

    def _update_status_btn(self, status):
        icon, color = STATUS_ICONS.get(status, ("", "#666666"))
        if status:
            self._status_btn.config(text=f"[{status.upper()}]", fg=color)
        else:
            self._status_btn.config(text="[Status]", fg="#666666")

    # ── Tag colors (editable via the "Colors…" toolbar dialog) ────────────────
    def _tag_color(self, tag, which):
        """Return the configured colour for (tag, which) where which is
        'bg' or 'fg'. Falls back to DEFAULT_TAG_COLORS."""
        cfg_all = getattr(self, "_tag_colors", None)
        if cfg_all is None:
            cfg_all = self.DEFAULT_TAG_COLORS
        entry = cfg_all.get(tag) or self.DEFAULT_TAG_COLORS.get(tag, {})
        return entry.get(which) or self.DEFAULT_TAG_COLORS[tag][which]

    def _apply_tag_colors(self):
        """Push the currently-configured tag colors into the Treeview."""
        if not hasattr(self, "tree") or self.tree is None:
            return
        for tag in self.DEFAULT_TAG_COLORS:
            self.tree.tag_configure(
                tag,
                background=self._tag_color(tag, "bg"),
                foreground=self._tag_color(tag, "fg"),
            )

    def _save_tag_colors(self):
        cfg = self._read_editor_config() or {}
        cfg["tag_colors"] = self._tag_colors
        self._write_editor_config(cfg)

    def _load_tag_colors(self):
        """Merge saved tag colors over defaults. Called at startup."""
        cfg = self._read_editor_config() or {}
        saved = cfg.get("tag_colors", {})
        self._tag_colors = {
            tag: {
                "bg": saved.get(tag, {}).get("bg", defaults["bg"]),
                "fg": saved.get(tag, {}).get("fg", defaults["fg"]),
            }
            for tag, defaults in self.DEFAULT_TAG_COLORS.items()
        }

    def _open_color_editor(self):
        """Dialog for editing background / foreground of each row tag.
        Per tag: two swatches (bg, fg) + hex entries + native picker
        button. Live preview row shows the result. Reset restores the
        built-in defaults. Apply saves and refreshes the tree."""
        from tkinter import colorchooser

        if not hasattr(self, "_tag_colors"):
            self._load_tag_colors()

        # Work on a copy so Cancel is free.
        work = {tag: dict(cols) for tag, cols in self._tag_colors.items()}

        win = tk.Toplevel(self)
        win.title("Tag Colors")
        win.configure(bg=self.BG)
        win.transient(self)
        win.resizable(False, False)

        frame = tk.Frame(win, bg=self.BG, padx=12, pady=12)
        frame.pack(fill=tk.BOTH, expand=True)

        tk.Label(frame, text="Row Tag Colors",
                 bg=self.BG, fg=self.ACC_TRA,
                 font=("Segoe UI", 10, "bold")
                 ).grid(row=0, column=0, columnspan=6,
                        sticky=tk.W, pady=(0, 8))
        hdr = ("#555555", "Segoe UI", 8)
        for col, label in enumerate(
                ["Tag", "BG hex", "BG", "FG hex", "FG", "Preview"]):
            tk.Label(frame, text=label, bg=self.BG, fg=hdr[0],
                     font=(hdr[1], hdr[2])
                     ).grid(row=1, column=col, sticky=tk.W, padx=4)

        swatches = {}    # tag -> (bg_swatch_label, fg_swatch_label, preview_label)
        bg_vars = {}
        fg_vars = {}

        def normalize_hex(s):
            s = (s or "").strip()
            if not s:
                return None
            if not s.startswith("#"):
                s = "#" + s
            if len(s) == 4:  # #abc → #aabbcc
                s = "#" + "".join(ch * 2 for ch in s[1:])
            if len(s) != 7:
                return None
            try:
                int(s[1:], 16)
            except ValueError:
                return None
            return s.upper()

        def refresh_row(tag):
            bg = normalize_hex(bg_vars[tag].get()) or work[tag]["bg"]
            fg = normalize_hex(fg_vars[tag].get()) or work[tag]["fg"]
            work[tag]["bg"] = bg
            work[tag]["fg"] = fg
            bg_sw, fg_sw, prev = swatches[tag]
            bg_sw.config(bg=bg)
            fg_sw.config(bg=fg)
            prev.config(bg=bg, fg=fg)

        def make_picker(tag, which):
            def pick():
                start = work[tag][which]
                result = colorchooser.askcolor(
                    color=start, parent=win,
                    title=f"{tag} — {which}")
                if result and result[1]:
                    hexv = result[1].upper()
                    (bg_vars if which == "bg" else fg_vars)[tag].set(hexv)
                    refresh_row(tag)
            return pick

        SAMPLE = "Die abendliche Stille..."

        for i, tag in enumerate(self.DEFAULT_TAG_COLORS):
            row = i + 2
            tk.Label(frame, text=tag, bg=self.BG, fg=self.FG,
                     font=("Consolas", 9)
                     ).grid(row=row, column=0, sticky=tk.W, padx=4, pady=2)

            bg_vars[tag] = tk.StringVar(value=work[tag]["bg"])
            fg_vars[tag] = tk.StringVar(value=work[tag]["fg"])

            bg_entry = tk.Entry(frame, textvariable=bg_vars[tag], width=9,
                                bg=self.BG2, fg=self.FG, font=("Consolas", 9),
                                insertbackground=self.FG)
            bg_entry.grid(row=row, column=1, padx=2)
            bg_entry.bind("<KeyRelease>",
                          lambda _e, t=tag: refresh_row(t))

            bg_sw = tk.Label(frame, text="   ", bg=work[tag]["bg"],
                             width=4, cursor="hand2",
                             relief=tk.RAISED, bd=1)
            bg_sw.grid(row=row, column=2, padx=2)
            bg_sw.bind("<Button-1>",
                       lambda _e, t=tag: make_picker(t, "bg")())

            fg_entry = tk.Entry(frame, textvariable=fg_vars[tag], width=9,
                                bg=self.BG2, fg=self.FG, font=("Consolas", 9),
                                insertbackground=self.FG)
            fg_entry.grid(row=row, column=3, padx=2)
            fg_entry.bind("<KeyRelease>",
                          lambda _e, t=tag: refresh_row(t))

            fg_sw = tk.Label(frame, text="   ", bg=work[tag]["fg"],
                             width=4, cursor="hand2",
                             relief=tk.RAISED, bd=1)
            fg_sw.grid(row=row, column=4, padx=2)
            fg_sw.bind("<Button-1>",
                       lambda _e, t=tag: make_picker(t, "fg")())

            prev = tk.Label(frame, text=f" {SAMPLE} ",
                            bg=work[tag]["bg"], fg=work[tag]["fg"],
                            font=("Consolas", 9))
            prev.grid(row=row, column=5, padx=(8, 0), sticky=tk.W)

            swatches[tag] = (bg_sw, fg_sw, prev)

        btn_row = len(self.DEFAULT_TAG_COLORS) + 2
        btn_f = tk.Frame(frame, bg=self.BG)
        btn_f.grid(row=btn_row, column=0, columnspan=6,
                   sticky=tk.E, pady=(12, 0))

        def do_reset():
            for tag, defaults in self.DEFAULT_TAG_COLORS.items():
                work[tag] = dict(defaults)
                bg_vars[tag].set(defaults["bg"])
                fg_vars[tag].set(defaults["fg"])
                refresh_row(tag)

        def do_apply():
            # Final hex normalization from the entry text (in case the
            # user typed something and never blurred).
            for tag in list(work):
                refresh_row(tag)
            self._tag_colors = {t: dict(c) for t, c in work.items()}
            self._apply_tag_colors()
            self._save_tag_colors()
            self._populate_tree()

        def do_ok():
            do_apply()
            win.destroy()

        tk.Button(btn_f, text="Reset Defaults",
                  command=do_reset, bg="#5A4A4A", fg=self.FG,
                  relief=tk.FLAT, padx=10, font=("Segoe UI", 9)
                  ).pack(side=tk.LEFT, padx=4)
        tk.Button(btn_f, text="Apply",
                  command=do_apply, bg="#4A5A6A", fg=self.FG,
                  relief=tk.FLAT, padx=10, font=("Segoe UI", 9)
                  ).pack(side=tk.LEFT, padx=4)
        tk.Button(btn_f, text="OK",
                  command=do_ok, bg="#4A6A4A", fg=self.FG,
                  relief=tk.FLAT, padx=14, font=("Segoe UI", 9)
                  ).pack(side=tk.LEFT, padx=4)
        tk.Button(btn_f, text="Cancel",
                  command=win.destroy, bg="#4A4A4A", fg=self.FG,
                  relief=tk.FLAT, padx=10, font=("Segoe UI", 9)
                  ).pack(side=tk.LEFT, padx=4)

    # ── Glossary ───────────────────────────────────────────────────────────────
    def _load_glossary(self):
        if os.path.exists(GLOSSARY_PATH):
            with open(GLOSSARY_PATH, encoding="utf-8") as f:
                data = json.load(f)
                self._glossary = data.get("terms", data)
        else:
            self._glossary = {}

    def _save_glossary(self):
        with open(GLOSSARY_PATH, "w", encoding="utf-8") as f:
            json.dump({"terms": self._glossary}, f, ensure_ascii=False, indent=2,
                      sort_keys=True)

    def _open_glossary_editor(self):
        """Open the glossary editor dialog."""
        win = tk.Toplevel(self)
        win.title("Glossary / Terminology")
        win.geometry("600x500")
        win.configure(bg=self.BG)
        win.transient(self)

        # Title
        tk.Label(win, text="Glossary: English -> German",
                 bg=self.BG, fg=self.FG2,
                 font=("Segoe UI", 11, "bold")).pack(pady=8)

        # List frame
        list_f = tk.Frame(win, bg=self.BG)
        list_f.pack(fill=tk.BOTH, expand=True, padx=10)

        cols = ("english", "german")
        tree = ttk.Treeview(list_f, columns=cols, show="headings",
                             selectmode="browse")
        tree.heading("english", text="English Term")
        tree.heading("german",  text="German Term")
        tree.column("english", width=250)
        tree.column("german",  width=250)

        vsb = ttk.Scrollbar(list_f, orient=tk.VERTICAL, command=tree.yview)
        tree.configure(yscrollcommand=vsb.set)
        tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        vsb.pack(side=tk.RIGHT, fill=tk.Y)

        def refresh():
            for iid in tree.get_children():
                tree.delete(iid)
            for en, de in sorted(self._glossary.items(), key=lambda x: x[0].lower()):
                tree.insert("", tk.END, values=(en, de))

        refresh()

        # Input fields
        input_f = tk.Frame(win, bg=self.BG)
        input_f.pack(fill=tk.X, padx=10, pady=8)

        tk.Label(input_f, text="EN:", bg=self.BG, fg=self.FG,
                 font=("Segoe UI", 9)).pack(side=tk.LEFT)
        en_var = tk.StringVar()
        en_entry = tk.Entry(input_f, textvariable=en_var, width=20,
                            bg=self.BG3, fg=self.FG2, font=("Consolas", 10),
                            insertbackground="white")
        en_entry.pack(side=tk.LEFT, padx=4)

        tk.Label(input_f, text="DE:", bg=self.BG, fg=self.FG,
                 font=("Segoe UI", 9)).pack(side=tk.LEFT, padx=(8, 0))
        de_var = tk.StringVar()
        de_entry = tk.Entry(input_f, textvariable=de_var, width=20,
                            bg=self.BG3, fg=self.FG2, font=("Consolas", 10),
                            insertbackground="white")
        de_entry.pack(side=tk.LEFT, padx=4)

        def on_select(event=None):
            sel = tree.selection()
            if sel:
                vals = tree.item(sel[0])["values"]
                en_var.set(vals[0])
                de_var.set(vals[1])
        tree.bind("<<TreeviewSelect>>", on_select)

        def add_term():
            en = en_var.get().strip()
            de = de_var.get().strip()
            if en and de:
                self._glossary[en] = de
                self._save_glossary()
                refresh()
                en_var.set("")
                de_var.set("")

        def delete_term():
            en = en_var.get().strip()
            if en in self._glossary:
                del self._glossary[en]
                self._save_glossary()
                refresh()
                en_var.set("")
                de_var.set("")

        btn_f = tk.Frame(win, bg=self.BG)
        btn_f.pack(fill=tk.X, padx=10, pady=(0, 10))
        tk.Button(btn_f, text="Add / Update", command=add_term,
                  bg="#4C6A4C", fg="white", relief=tk.FLAT,
                  font=("Segoe UI", 9)).pack(side=tk.LEFT, padx=4)
        tk.Button(btn_f, text="Delete", command=delete_term,
                  bg="#6A4C4C", fg="white", relief=tk.FLAT,
                  font=("Segoe UI", 9)).pack(side=tk.LEFT, padx=4)

        info = tk.Label(win, text=f"{len(self._glossary)} terms  |  "
                        "Glossary warnings appear when translating strings that contain listed terms",
                        bg=self.BG, fg="#666666", font=("Segoe UI", 8))
        info.pack(pady=(0, 8))

    # ── Batch find & replace ──────────────────────────────────────────────────
    def _open_batch_replace(self):
        """Open batch find & replace dialog across all translation files."""
        win = tk.Toplevel(self)
        win.title("Batch Find & Replace")
        win.geometry("550x350")
        win.configure(bg=self.BG)
        win.transient(self)

        tk.Label(win, text="Find & Replace across ALL translations",
                 bg=self.BG, fg=self.FG2,
                 font=("Segoe UI", 11, "bold")).pack(pady=8)

        form = tk.Frame(win, bg=self.BG)
        form.pack(fill=tk.X, padx=20, pady=4)

        tk.Label(form, text="Find:", bg=self.BG, fg=self.FG,
                 font=("Segoe UI", 10)).grid(row=0, column=0, sticky=tk.W, pady=4)
        find_var = tk.StringVar()
        find_entry = tk.Entry(form, textvariable=find_var, width=40,
                              bg=self.BG3, fg=self.FG2, font=("Consolas", 10),
                              insertbackground="white")
        find_entry.grid(row=0, column=1, padx=8, pady=4)

        tk.Label(form, text="Replace:", bg=self.BG, fg=self.FG,
                 font=("Segoe UI", 10)).grid(row=1, column=0, sticky=tk.W, pady=4)
        repl_var = tk.StringVar()
        repl_entry = tk.Entry(form, textvariable=repl_var, width=40,
                              bg=self.BG3, fg=self.FG2, font=("Consolas", 10),
                              insertbackground="white")
        repl_entry.grid(row=1, column=1, padx=8, pady=4)

        case_var = tk.BooleanVar(value=False)
        tk.Checkbutton(form, text="Case sensitive", variable=case_var,
                       bg=self.BG, fg=self.FG, selectcolor=self.BG3,
                       font=("Segoe UI", 9)).grid(row=2, column=1, sticky=tk.W,
                                                    padx=8, pady=4)

        result_lbl = tk.Label(win, text="", bg=self.BG, fg=self.ACC_TRA,
                               font=("Consolas", 9))
        result_lbl.pack(pady=8)

        preview_text = tk.Text(win, bg=self.BG3, fg=self.FG, height=8,
                                font=("Consolas", 9), wrap=tk.WORD,
                                state=tk.DISABLED)
        preview_text.pack(fill=tk.BOTH, expand=True, padx=20, pady=(0, 8))

        def _count_occurrences(val, find, case):
            hay = val if case else val.lower()
            needle = find if case else find.lower()
            return hay.count(needle)

        def _apply_replace(val, find, repl, case):
            if case:
                return val.replace(find, repl)
            # Case-insensitive, left-to-right, non-overlapping.
            out = []
            low = val.lower()
            fl = find.lower()
            i = 0
            while True:
                j = low.find(fl, i)
                if j == -1:
                    out.append(val[i:])
                    break
                out.append(val[i:j])
                out.append(repl)
                i = j + len(find)
            return "".join(out)

        def _context(val, find, repl, case):
            """A short before->after snippet around the first match."""
            low = val if case else val.lower()
            fl = find if case else find.lower()
            j = low.find(fl)
            if j == -1:
                return ""
            a = max(0, j - 15)
            b = min(len(val), j + len(find) + 15)
            before = ("…" if a > 0 else "") + val[a:b] + ("…" if b < len(val) else "")
            after = _apply_replace(before, find, repl, case)
            snip = lambda s: s.replace("\n", "⏎")
            return f"{snip(before)}  ->  {snip(after)}"

        def do_preview():
            find = find_var.get()
            if not find:
                result_lbl.config(text="Enter text to find.")
                return
            repl = repl_var.get()
            case = case_var.get()
            count = 0
            files_hit = 0
            previews = []
            for fname in sorted(os.listdir(TRANS_DIR)):
                if not fname.endswith(".json") or _is_aux_json(fname):
                    continue
                path = os.path.join(TRANS_DIR, fname)
                try:
                    trans = load_translations_json(path)
                except Exception:
                    continue
                file_matches = 0
                for key, val in trans.items():
                    n = _count_occurrences(val, find, case)
                    if n:
                        count += n
                        file_matches += n
                        if len(previews) < 40:
                            previews.append(
                                f"{fname[:-5]} {key}:  {_context(val, find, repl, case)}")
                if file_matches:
                    files_hit += 1
            result_lbl.config(
                text=f"{count} match(es) in {files_hit} file(s)"
                     if count else "No matches found.")
            preview_text.config(state=tk.NORMAL)
            preview_text.delete("1.0", tk.END)
            body = "\n".join(previews)
            if count > len(previews):
                body += f"\n… (+{count - len(previews)} more)"
            preview_text.insert("1.0", body)
            preview_text.config(state=tk.DISABLED)

        def do_replace():
            find = find_var.get()
            repl = repl_var.get()
            if not find:
                return
            case = case_var.get()
            if not messagebox.askyesno("Confirm",
                    f"Replace all occurrences of '{find}' with '{repl}'?",
                    parent=win):
                return
            count = 0
            modified = 0
            for fname in sorted(os.listdir(TRANS_DIR)):
                if not fname.endswith(".json") or _is_aux_json(fname):
                    continue
                path = os.path.join(TRANS_DIR, fname)
                try:
                    trans = load_translations_json(path)
                except Exception:
                    continue
                changed = False
                for key in list(trans.keys()):
                    val = trans[key]
                    n = _count_occurrences(val, find, case)
                    if n:
                        trans[key] = _apply_replace(val, find, repl, case)
                        count += n
                        changed = True
                if changed:
                    write_translations_json(path, trans)
                    modified += 1

            result_lbl.config(text=f"Replaced {count} occurrence(s) in {modified} file(s)")
            # Reload current file so the tree reflects the on-disk changes.
            self._load_translations()
            self._populate_tree()

        btn_f = tk.Frame(win, bg=self.BG)
        btn_f.pack(fill=tk.X, padx=20, pady=(0, 10))
        tk.Button(btn_f, text="Preview", command=do_preview,
                  bg="#4A6080", fg="white", relief=tk.FLAT,
                  font=("Segoe UI", 9)).pack(side=tk.LEFT, padx=4)
        tk.Button(btn_f, text="Replace All", command=do_replace,
                  bg="#804A4A", fg="white", relief=tk.FLAT,
                  font=("Segoe UI", 9)).pack(side=tk.LEFT, padx=4)

    # ── Auto-save ──────────────────────────────────────────────────────────────
    def _schedule_autosave(self):
        self._autosave_id = self.after(AUTOSAVE_INTERVAL_MS, self._do_autosave)

    def _do_autosave(self):
        if self._dirty and self.current_file:
            self._commit_current()
            path = os.path.join(TRANS_DIR, self.current_file.replace(".txt", ".json"))
            with open(path, "w", encoding="utf-8") as f:
                json.dump(self._build_save_dict(), f, ensure_ascii=False, indent=2,
                          sort_keys=True)
            self._save_string_status()
            self._save_notes()
            self._tm_dirty = True
            self._dirty = False
            ts = time.strftime("%H:%M:%S")
            self._autosave_lbl.config(text=f"Auto-saved {ts}")
        self._schedule_autosave()

    # ── Config (game directory, etc.) ───────────────────────────────────────────
    def _load_config(self):
        if os.path.exists(CONFIG_PATH):
            try:
                with open(CONFIG_PATH, encoding="utf-8") as f:
                    self._config = json.load(f)
            except Exception:
                self._config = {}
        else:
            self._config = {}

    def _save_config(self):
        # Ensure the configs/ folder exists — on a fresh/standalone run it
        # won't, and writing editor_config.json would FileNotFoundError.
        os.makedirs(os.path.dirname(CONFIG_PATH), exist_ok=True)
        with open(CONFIG_PATH, "w", encoding="utf-8") as f:
            json.dump(self._config, f, ensure_ascii=False, indent=2)

    def _get_game_dir(self):
        """Return the game directory (where DAT/ lives). Prompt if not set."""
        gd = self._config.get("game_dir", "")
        if gd and os.path.isdir(os.path.join(gd, "DAT")):
            return gd
        # Try auto-detect from GAME_ROOT (walked up from the editor's folder)
        if os.path.isdir(os.path.join(GAME_ROOT, "DAT")):
            self._config["game_dir"] = GAME_ROOT
            self._save_config()
            return GAME_ROOT
        # Prompt user
        return self._pick_game_dir()

    def _pick_game_dir(self):
        """Let the user pick the game directory."""
        d = filedialog.askdirectory(title="Select Breath of Fire IV game folder (contains DAT/)")
        if not d:
            return None
        if not os.path.isdir(os.path.join(d, "DAT")):
            messagebox.showerror("Invalid directory",
                                 f"No DAT/ folder found in:\n{d}\n\n"
                                 "Please select the game root folder.")
            return None
        self._config["game_dir"] = d
        self._save_config()
        return d

    def _get_repack_out_dir(self, game_dir):
        """Folder the repacked German DATs are written to. The live DAT/ is NEVER
        overwritten and the source is always DAT_backup/. Stored in config; prompts
        the first time (default suggestion: <game>/DAT_DE)."""
        od = self._config.get("repack_out_dir", "")
        if od:
            return od
        return self._pick_repack_out_dir(game_dir)

    def _pick_repack_out_dir(self, game_dir=None):
        """Choose / change the repack output folder (toolbar 'Out Dir…')."""
        game_dir = game_dir or self._get_game_dir()
        start = (self._config.get("repack_out_dir")
                 or (os.path.join(game_dir, "DAT_DE") if game_dir else GAME_ROOT))
        d = filedialog.askdirectory(
            title="Output folder for repacked German DATs (live DAT/ stays untouched)",
            initialdir=start if os.path.isdir(start) else (game_dir or GAME_ROOT))
        if not d:
            return self._config.get("repack_out_dir") or None
        # Guard: warn before allowing the live DAT/ folder as the target.
        if game_dir and os.path.normcase(os.path.abspath(d)) == \
                os.path.normcase(os.path.join(game_dir, "DAT")):
            if not messagebox.askyesno(
                    "Output = live DAT/",
                    "That is the live DAT/ folder the game loads — writing here "
                    "overwrites your original game DATs.\n\nUse it anyway?"):
                return self._config.get("repack_out_dir") or None
        self._config["repack_out_dir"] = d
        self._save_config()
        self.status_var.set(f"Repack output folder: {d}")
        return d

    def _get_repack_import_dir(self):
        """German-graphics folder whose NON-TEXT (texture) entries get merged into
        the repack output so a text repack doesn't revert labels to US. If the user
        hasn't picked one, DEFAULT to <game>/graphics_to_edit when it exists — that
        way a plain text repack never silently wipes graphics edited there. Returns
        '' only if neither a configured nor a default folder is available."""
        cfg = self._config.get("repack_import_dir", "")
        if cfg:
            return cfg
        for base in (self._get_game_dir() or GAME_ROOT, GAME_ROOT):
            if not base:
                continue
            cand = os.path.join(base, "graphics_to_edit")
            if os.path.isdir(cand):
                return cand
        return ""

    def _pick_repack_import_dir(self):
        """Choose / change the German-graphics import folder (toolbar 'Gfx Dir…')."""
        start = (self._config.get("repack_import_dir")
                 or self._get_game_dir() or GAME_ROOT)
        d = filedialog.askdirectory(
            title="German-graphics folder — its texture entries are merged into the "
                  "repack output (cancel = leave unchanged)",
            initialdir=start if os.path.isdir(start) else GAME_ROOT)
        if not d:
            return self._config.get("repack_import_dir") or ""
        self._config["repack_import_dir"] = d
        self._save_config()
        self.status_var.set(f"Graphics import folder: {d}")
        return d

    # ── Pristine-DAT guard (safe-repack reference) ───────────────────────────────
    def _pristine_dir(self):
        """The pristine-US reference folder repack_text reads text from. Non-prompting
        (used by the startup status check) — falls back to config/GAME_ROOT."""
        gd = self._config.get("game_dir") or GAME_ROOT
        return os.path.join(gd, "DAT_backup")   # == DAT_Backup (Windows case-insensitive)

    def _update_pristine_status(self):
        """Light status-bar indicator: backup present + INIT.DAT size matches the
        manifest. Full per-DAT verification runs at repack time."""
        if _pg is None:
            self._pristine_var.set("guard: n/a"); self._pristine_lbl.config(fg="#888888")
            return
        man = _pg.load_manifest()
        pdir = self._pristine_dir()
        init = os.path.join(pdir, "INIT.DAT")
        if man is None:
            self._pristine_var.set("⚠ no manifest"); self._pristine_lbl.config(fg="#CC8888")
        elif not os.path.isdir(pdir) or not os.path.exists(init):
            self._pristine_var.set("⚠ no pristine backup"); self._pristine_lbl.config(fg="#CC8888")
        elif man.get("init_size") and os.path.getsize(init) != man["init_size"]:
            self._pristine_var.set("⚠ backup ≠ pristine"); self._pristine_lbl.config(fg="#D8A050")
        else:
            self._pristine_var.set("✓ pristine ref"); self._pristine_lbl.config(fg="#88BB88")

    def _pre_repack_pristine_gate(self):
        """Full per-DAT check of the pristine backup before repacking. Returns True
        to proceed, False to abort. On mismatch shows the fallback popup so a newer
        clean game version can still be used, while a German-contaminated backup is
        caught (with the exact DAT names + a German-marker advisory)."""
        if _pg is None:
            return True   # guard unavailable — don't block
        man = _pg.load_manifest()
        if man is None:
            return messagebox.askyesno(
                "No pristine manifest",
                "pristine_text_manifest.json is missing, so the text source can't be "
                "verified. Repack anyway?")
        pdir = self._pristine_dir()
        if not os.path.isdir(pdir):
            messagebox.showerror(
                "No pristine backup",
                f"No pristine reference folder found at:\n{pdir}\n\n"
                "Use 'Locate DATs…' to point at a clean US DAT folder first.")
            return False
        r = _pg.check_dir(pdir, man)
        if not r["modified"]:
            return True   # all known DATs pristine (graphics-only is fine)

        names = ", ".join(r["modified"][:12]) + (" …" if len(r["modified"]) > 12 else "")
        ger = (", ".join(r["german"][:8]) if r["german"] else "")
        advisory = (f"German characters detected in: {ger}\n→ looks ALREADY-TRANSLATED "
                    f"(NOT a clean version). Repacking will scramble text."
                    if r["german"] else
                    "No German characters detected → this may just be a newer/different "
                    "clean game version, which is safe.")
        msg = (f"{len(r['modified'])} DAT(s) in your pristine backup have MODIFIED text:\n"
               f"  {names}\n\n{advisory}\n\n"
               f"• Locate clean DATs…  pick a clean US folder to use instead.\n"
               f"• Use anyway          treat the backup as clean (re-learns the manifest).\n"
               f"• Cancel              stop and fix it.")
        dlg = self._three_choice("Pristine check failed", msg,
                                  ["Locate clean DATs…", "Use anyway", "Cancel"])
        if dlg == "Locate clean DATs…":
            return bool(self._locate_pristine()) and self._pre_repack_pristine_gate()
        if dlg == "Use anyway":
            try:
                _pg.save_manifest(_pg.build_manifest(pdir))
                self._update_pristine_status()
                self.status_var.set("Re-learned pristine manifest from current backup.")
                return True
            except Exception as ex:
                messagebox.showerror("Manifest", f"Could not re-learn manifest:\n{ex}")
                return False
        return False

    def _three_choice(self, title, msg, labels):
        """Modal with up to three buttons; returns the chosen label or None."""
        dlg = tk.Toplevel(self); dlg.title(title); dlg.transient(self); dlg.grab_set()
        tk.Label(dlg, text=msg, justify=tk.LEFT, anchor=tk.W,
                 font=("Consolas", 9), padx=14, pady=12, wraplength=520).pack(fill=tk.BOTH)
        out = {"v": None}
        bf = tk.Frame(dlg); bf.pack(fill=tk.X, padx=12, pady=(0, 12))
        def pick(l): out["v"] = l; dlg.destroy()
        for l in labels:
            tk.Button(bf, text=l, command=lambda l=l: pick(l), padx=10, pady=3).pack(
                side=tk.RIGHT, padx=4)
        dlg.protocol("WM_DELETE_WINDOW", lambda: pick(None))
        self.wait_window(dlg)
        return out["v"]

    def _locate_pristine(self):
        """Pick a clean US DAT folder and copy it into the pristine backup. Returns
        the backup path on success, else None."""
        d = filedialog.askdirectory(
            title="Select a CLEAN, unmodified US DAT folder (e.g. a fresh GOG install)")
        if not d:
            return None
        man = _pg.load_manifest() if _pg else None
        # Verify the chosen folder looks pristine before adopting it.
        if _pg and man:
            r = _pg.check_dir(d, man)
            if r["modified"]:
                ger = (", ".join(r["german"][:8]) if r["german"] else "")
                if not messagebox.askyesno(
                        "Folder not pristine",
                        f"{len(r['modified'])} DAT(s) here have modified text"
                        + (f" (German chars in: {ger})" if ger else "") +
                        ".\n\nUse it as the pristine reference anyway?"):
                    return None
        pdir = self._pristine_dir()
        if os.path.normcase(os.path.abspath(d)) == os.path.normcase(os.path.abspath(pdir)):
            self._update_pristine_status()
            return pdir
        try:
            self.status_var.set("Copying clean DATs into pristine backup…"); self.update()
            os.makedirs(pdir, exist_ok=True)
            n = 0
            for fn in os.listdir(d):
                if fn.lower().endswith(".dat"):
                    shutil.copy2(os.path.join(d, fn), os.path.join(pdir, fn))
                    n += 1
            self.status_var.set(f"Pristine backup set from {d} ({n} DATs).")
        except Exception as ex:
            messagebox.showerror("Locate DATs", f"Copy failed:\n{ex}")
            return None
        self._update_pristine_status()
        return pdir

    def _restore_pristine(self):
        """Copy the pristine backup over the live DAT/ (one-click revert)."""
        gd = self._get_game_dir()
        if not gd:
            return
        pdir = self._pristine_dir()
        live = os.path.join(gd, "DAT")
        if not os.path.isdir(pdir):
            messagebox.showerror("Restore", f"No pristine backup at:\n{pdir}")
            return
        if not messagebox.askyesno(
                "Restore pristine DAT/",
                f"Overwrite the live DAT/ with the pristine backup?\n\n"
                f"{pdir}\n  ->  {live}\n\n"
                "This reverts ALL text AND graphics in DAT/ to original US. "
                "(Your graphics_to_edit folder is untouched.)"):
            return
        try:
            self.status_var.set("Restoring pristine DAT/…"); self.update()
            n = 0
            for fn in os.listdir(pdir):
                if fn.lower().endswith(".dat"):
                    shutil.copy2(os.path.join(pdir, fn), os.path.join(live, fn))
                    n += 1
            self.status_var.set(f"Restored {n} DAT(s) to pristine US.")
            self._update_pristine_status()
        except Exception as ex:
            messagebox.showerror("Restore", f"Restore failed:\n{ex}")

    # ── Repacking ──────────────────────────────────────────────────────────────
    def _open_vwf_editor(self):
        """Dialog to view + tweak the VWF config (bearing/advance per
        glyph) used by the editor preview. Changes apply live to the
        preview's in-memory tables and can be saved to an editor-only
        copy at localization_editor/configs/vwf_config_editor.txt.

        The project-root vwf_config.txt is NEVER touched here — the DLL
        and repacker keep reading it untouched. The editor uses the
        editor copy when it exists, else falls back to the project
        file.
        """
        # Parse the currently active config file, preserving comment
        # lines / blank lines / non-VWF lines verbatim so we round-trip
        # unchanged when saving.
        active_path = getattr(self, "_active_vwf_path", VWF_CONFIG_PATH)
        if not os.path.exists(active_path):
            messagebox.showerror(
                "VWF editor",
                f"VWF config file not found:\n{active_path}")
            return

        line_re = re.compile(
            r"^(\s*0x([0-9A-Fa-f]+)\s*=\s*)(\S+)(\s*:\s*)"
            r"(-?\d+)(\s*:\s*)(-?\d+)(.*)$")

        # Each parsed VWF line becomes a dict:
        #   {kind='vwf', byte=int, name=str,
        #    bearing=int, advance=int,
        #    prefix=..., sep1=..., sep2=..., suffix=...}
        # Non-VWF lines are stored as {kind='raw', text=...}.
        parsed = []
        with open(active_path, encoding="utf-8") as f:
            for raw in f:
                line = raw.rstrip("\n")
                m = line_re.match(line.split("#", 1)[0] + (
                    ("#" + line.split("#", 1)[1]) if "#" in line else ""))
                # Simpler: match only the numeric portion; preserve any
                # trailing comment as part of suffix.
                m2 = re.match(
                    r"^(\s*0x([0-9A-Fa-f]+)\s*=\s*)(\S+)(\s*:\s*)"
                    r"(-?\d+)(\s*:\s*)(-?\d+)(.*)$", line)
                if not m2:
                    parsed.append({"kind": "raw", "text": line})
                    continue
                parsed.append({
                    "kind":    "vwf",
                    "prefix":  m2.group(1),
                    "byte":    int(m2.group(2), 16),
                    "name":    m2.group(3),
                    "sep1":    m2.group(4),
                    "bearing": int(m2.group(5)),
                    "sep2":    m2.group(6),
                    "advance": int(m2.group(7)),
                    "suffix":  m2.group(8),
                })

        win = tk.Toplevel(self)
        win.title("VWF Editor")
        win.configure(bg=self.BG)
        win.transient(self)
        win.geometry("760x640")

        # Header bar.
        hdr = tk.Frame(win, bg=self.BG, padx=10, pady=8)
        hdr.pack(fill=tk.X)
        editor_active = (active_path == VWF_EDITOR_PATH)
        status_txt = (
            "Active: vwf_config_editor.txt (editor copy)"
            if editor_active else
            "Active: vwf_config.txt (project default)")
        status_lbl = tk.Label(hdr, text=status_txt, bg=self.BG,
                              fg="#88CC88" if editor_active else "#AAAABB",
                              font=("Consolas", 9))
        status_lbl.pack(side=tk.LEFT)

        # Scrollable grid of rows.
        list_outer = tk.Frame(win, bg=self.BG2)
        list_outer.pack(fill=tk.BOTH, expand=True, padx=10, pady=(0, 6))
        canvas = tk.Canvas(list_outer, bg=self.BG2, highlightthickness=0)
        vsb = tk.Scrollbar(list_outer, orient=tk.VERTICAL,
                           command=canvas.yview)
        canvas.configure(yscrollcommand=vsb.set)
        vsb.pack(side=tk.RIGHT, fill=tk.Y)
        canvas.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        inner = tk.Frame(canvas, bg=self.BG2)
        inner_id = canvas.create_window((0, 0), window=inner, anchor=tk.NW)
        def _resize_inner(_e=None):
            canvas.configure(scrollregion=canvas.bbox("all"))
            canvas.itemconfigure(inner_id, width=canvas.winfo_width())
        inner.bind("<Configure>", _resize_inner)
        canvas.bind("<Configure>", _resize_inner)
        # Mouse-wheel scroll on Windows.
        def _on_wheel(e):
            canvas.yview_scroll(int(-1 * (e.delta / 120)), "units")
        canvas.bind_all("<MouseWheel>", _on_wheel)

        # Column headers.
        for col, txt in enumerate(
                ["Byte", "Name", "Bearing", "Advance", "Glyph"]):
            tk.Label(inner, text=txt, bg=self.BG2, fg="#555577",
                     font=("Segoe UI", 8, "bold")
                     ).grid(row=0, column=col, sticky=tk.W, padx=4, pady=2)

        # Index parsed entries for the live-apply loop + save.
        vwf_rows = [p for p in parsed if p["kind"] == "vwf"]
        vars_by_byte = {}  # byte -> (bearing_var, advance_var)
        thumb_refs = []    # keep ImageTk refs alive

        def _apply_live(byte):
            """Apply a single row's bearing/advance to the in-memory
            _gfont_bearing / _gfont_advance dicts, then re-render the
            mockup so the user sees the change live."""
            b, a = vars_by_byte[byte]
            try:
                bearing = int(b.get())
                advance = int(a.get())
            except Exception:
                return
            uni = _byte_to_unicode(byte)
            if uni:
                self._gfont_bearing[uni] = bearing
                self._gfont_advance[uni] = advance
            # Also push into the parsed entry so save reflects it.
            for p in vwf_rows:
                if p["byte"] == byte:
                    p["bearing"] = bearing
                    p["advance"] = advance
                    break
            try:
                self._render_mockup_page()
            except Exception:
                pass

        for i, p in enumerate(vwf_rows):
            r = i + 1
            tk.Label(inner, text=f"0x{p['byte']:02X}",
                     bg=self.BG2, fg="#CCCCCC",
                     font=("Consolas", 9)
                     ).grid(row=r, column=0, sticky=tk.W, padx=4)
            tk.Label(inner, text=p["name"],
                     bg=self.BG2, fg=self.FG,
                     font=("Consolas", 9)
                     ).grid(row=r, column=1, sticky=tk.W, padx=4)

            b_var = tk.IntVar(value=p["bearing"])
            a_var = tk.IntVar(value=p["advance"])
            vars_by_byte[p["byte"]] = (b_var, a_var)

            byte_copy = p["byte"]

            def _make_cb(by=byte_copy):
                return lambda *_: _apply_live(by)

            b_sp = tk.Spinbox(inner, from_=-32, to=64, width=5,
                              textvariable=b_var, bg=self.BG3, fg=self.FG,
                              buttonbackground=self.BG3,
                              insertbackground=self.FG,
                              font=("Consolas", 9), relief=tk.FLAT)
            b_sp.grid(row=r, column=2, sticky=tk.W, padx=2)
            b_sp.configure(command=_make_cb())
            b_sp.bind("<KeyRelease>", lambda _e, by=byte_copy:
                      _apply_live(by))
            a_sp = tk.Spinbox(inner, from_=0, to=64, width=5,
                              textvariable=a_var, bg=self.BG3, fg=self.FG,
                              buttonbackground=self.BG3,
                              insertbackground=self.FG,
                              font=("Consolas", 9), relief=tk.FLAT)
            a_sp.grid(row=r, column=3, sticky=tk.W, padx=2)
            a_sp.configure(command=_make_cb())
            a_sp.bind("<KeyRelease>", lambda _e, by=byte_copy:
                      _apply_live(by))

            # Tiny glyph thumbnail (native 24×24, no upscale).
            uni = _byte_to_unicode(byte_copy)
            glyph = self._gfont_pil.get(uni) if uni else None
            if glyph:
                try:
                    photo = ImageTk.PhotoImage(glyph)
                    thumb_refs.append(photo)
                    tk.Label(inner, image=photo, bg=self.BG2, bd=0
                             ).grid(row=r, column=4, padx=4)
                except Exception:
                    pass

        # Footer: action buttons.
        btn_f = tk.Frame(win, bg=self.BG, padx=10, pady=8)
        btn_f.pack(fill=tk.X)

        def _do_save():
            """Write the (possibly edited) config to the editor-only
            copy. The project-root vwf_config.txt is not touched."""
            os.makedirs(EDITOR_CONFIG_DIR, exist_ok=True)
            with open(VWF_EDITOR_PATH, "w", encoding="utf-8") as f:
                for p in parsed:
                    if p["kind"] == "raw":
                        f.write(p["text"] + "\n")
                    else:
                        f.write(f"{p['prefix']}{p['name']}{p['sep1']}"
                                f"{p['bearing']}{p['sep2']}"
                                f"{p['advance']}{p['suffix']}\n")
            self._active_vwf_path = VWF_EDITOR_PATH
            status_lbl.config(
                text="Active: vwf_config_editor.txt (editor copy)",
                fg="#88CC88")
            self._reload_vwf()
            self.status_var.set(f"VWF editor copy saved: {VWF_EDITOR_PATH}")

        def _do_reset():
            """Delete the editor copy so the editor reverts to the
            project-root vwf_config.txt."""
            if os.path.exists(VWF_EDITOR_PATH):
                try:
                    os.remove(VWF_EDITOR_PATH)
                except OSError as e:
                    messagebox.showerror("VWF editor",
                                         f"Could not remove editor copy:\n{e}")
                    return
            self._active_vwf_path = VWF_CONFIG_PATH
            status_lbl.config(
                text="Active: vwf_config.txt (project default)",
                fg="#AAAABB")
            self._reload_vwf()
            win.destroy()
            # Reopen with freshly-parsed project file so the dialog
            # reflects the reverted state.
            self._open_vwf_editor()

        def _do_load():
            from tkinter import filedialog
            path = filedialog.askopenfilename(
                parent=win,
                title="Load VWF config",
                filetypes=[("VWF config", "*.txt"), ("All files", "*.*")],
                initialdir=os.path.dirname(VWF_CONFIG_PATH))
            if not path:
                return
            # Copy into the editor slot so the editor starts using it.
            os.makedirs(EDITOR_CONFIG_DIR, exist_ok=True)
            try:
                with open(path, encoding="utf-8") as src, \
                     open(VWF_EDITOR_PATH, "w", encoding="utf-8") as dst:
                    dst.write(src.read())
            except Exception as e:
                messagebox.showerror("VWF editor",
                                     f"Could not load VWF:\n{e}")
                return
            self._reload_vwf()
            win.destroy()
            self._open_vwf_editor()

        tk.Button(btn_f, text="Save to editor copy",
                  command=_do_save, bg="#4A6A4A", fg=self.FG,
                  relief=tk.FLAT, padx=12, font=("Segoe UI", 9)
                  ).pack(side=tk.LEFT, padx=4)
        tk.Button(btn_f, text="Reset to project vwf_config.txt",
                  command=_do_reset, bg="#6A4A4A", fg=self.FG,
                  relief=tk.FLAT, padx=12, font=("Segoe UI", 9)
                  ).pack(side=tk.LEFT, padx=4)
        tk.Button(btn_f, text="Load from file…",
                  command=_do_load, bg="#4A5A6A", fg=self.FG,
                  relief=tk.FLAT, padx=12, font=("Segoe UI", 9)
                  ).pack(side=tk.LEFT, padx=4)
        tk.Button(btn_f, text="Close",
                  command=win.destroy, bg="#4A4A4A", fg=self.FG,
                  relief=tk.FLAT, padx=12, font=("Segoe UI", 9)
                  ).pack(side=tk.RIGHT, padx=4)

        # Unbind mouse-wheel when the dialog closes so it doesn't stay
        # captured on the main window.
        def _on_close():
            try:
                canvas.unbind_all("<MouseWheel>")
            except Exception:
                pass
            win.destroy()
        win.protocol("WM_DELETE_WINDOW", _on_close)

    def _reload_vwf(self):
        """Re-read vwf_config.txt + glyph PNGs into the editor without
        restarting. Useful after editing widths or re-exporting glyphs
        from the font editor GUI."""
        self._load_game_font()
        # Force a redraw of anything that caches width — the mockup is
        # the main visible consumer. Try to refresh it gracefully.
        for attr in ("_refresh_mockup", "_render_mockup",
                     "_update_mockup", "_refresh_byte_counter"):
            fn = getattr(self, attr, None)
            if callable(fn):
                try:
                    fn()
                except Exception:
                    pass
        n_adv = len(getattr(self, "_gfont_advance", {}))
        n_png = len(getattr(self, "_gfont_pil", {}))
        self.status_var.set(
            f"VWF reloaded: {n_adv} advances, {n_png} glyph PNGs.")

    def _repack_current(self):
        """Export current file and repack it into the DAT."""
        if not self.current_file:
            return
        game_dir = self._get_game_dir()
        if not game_dir:
            return
        self._save()
        self._export_file_for_repack(self.current_file)
        base = self.current_file.replace(".txt", "")
        self._run_repack([base], game_dir, sync_shared=True)

    # ── Clean PSX control codes ──────────────────────────────────────────────
    # PSX -> PC opcode rewrites. Argument bytes pass through unchanged
    # — only the opcode byte differs. Confirmed pairs:
    #   PSX 0x17 [XX:YY] -> PC 0x14 [XX:YY] (portrait, 2-arg)
    #   PSX 0x16 [XX]    -> PC 0x13 [XX]    (timed text, 1-arg)
    #   PSX 0x19 [XX]    -> PC 0x16 [XX]    (timed text variant)
    #   PSX 0x14 [XX]    -> PC 0x12 [XX]    (option/box action)
    #   PSX 0x1C [XX]    -> PC 0x19 [XX]    (1-arg; discovered via
    #                       _opcode_discovery.py, 94% PSX support;
    #                       common in shop-dialogue prefix)
    # Order matters at the byte level (17->14 must run before 14->12)
    # so we use a single alternation regex with a substitution map
    # to avoid the chain hazard of separate sequential subs.
    _PSX_REMAP_MAP = {"17": "14", "16": "13", "19": "16", "14": "12",
                      "1C": "19"}
    _PSX_REMAP_RE  = re.compile(r"\[(17|16|19|14|1C):([0-9A-Fa-f:]+)\]")
    _PSX_MENU_WAIT_RE = re.compile(r'\[16:02\](?=\[16:[0-9A-Fa-f]{2}\])')
    _PSX_TRIO_NN00_RE = re.compile(r'\[05:0A\]\[15:00\]\[06\]')
    _COLOR_TOKEN_RE   = re.compile(r'(\[05:[0-9A-Fa-f]{2}\]|\[06\])')

    @classmethod
    def _apply_psx_remap(cls, text):
        return cls._PSX_REMAP_RE.sub(
            lambda m: f"[{cls._PSX_REMAP_MAP[m.group(1).lower()]}:{m.group(2)}]",
            text)

    @classmethod
    def _strip_orphan_color_end(cls, text):
        """Drop `[06]` color-end opcodes that are PSX-only speaker-
        label markers (preceded by literal text, no matching
        `[05:XX]` open). Preserves `[05:XX]…[06]` color blocks AND
        PC's standalone form `[14:XX][06]` (portrait-then-color-end
        chain — common in dialogue PC source)."""
        parts = cls._COLOR_TOKEN_RE.split(text)
        open_count = 0
        last_char = ''
        for i, p in enumerate(parts):
            if p == '[06]':
                if open_count > 0:
                    open_count -= 1
                elif last_char != ']':
                    parts[i] = ''
                    continue
            elif p.startswith('[05:'):
                open_count += 1
            if parts[i]:
                last_char = parts[i][-1]
        return ''.join(parts)

    def _clean_psx_codes(self):
        """Convert PSX-specific control codes and characters to PC format.

        Operations applied to every JSON entry:
          1. PSX -> PC opcode remap (17->14, 16->13, 19->16, 14->12)
             via a single-pass alternation regex.
          2. [90] -> ... (PSX ellipsis glyph). The German PSX often
             brackets [90] with a literal '.', so we consume one
             adjacent '.' on each side to land at exactly 3 dots.

        The previous "strip leading PSX-only control codes that don't
        match the PC source" step was REMOVED. It was destroying the
        just-converted leading codes (e.g. PSX [17:02:02] -> PC
        [14:02:02], whereas PC source has [14:02][02] — byte-identical
        when re-encoded but the regex string-compare flagged them as
        different and stripped the converted code, losing the
        portrait info entirely).
        """
        self._commit_current()
        self._save()

        total_cleaned = 0
        files_touched = 0
        for fname in sorted(os.listdir(DUMP_DIR)):
            if not fname.endswith(".txt"):
                continue
            base = fname[:-4]
            json_path = os.path.join(TRANS_DIR, base + ".json")
            if not os.path.exists(json_path):
                continue

            trans = load_translations_json(json_path)

            changed = False
            for key, german in list(trans.items()):
                if not german or not german.strip():
                    continue
                original = german

                # 1. PSX -> PC opcode remap (single pass).
                german = self._apply_psx_remap(german)

                # 2. PSX ellipsis [90] -> ...
                # Also consume one optional leading space because the
                # German PSX often pads `[90]` with a space (e.g.
                # "Nein? [90].") that PC's literal `...` doesn't have.
                german = re.sub(r' ?\.?\[90\]\.?', '...', german)

                # 3. German em-dash typography: `--` (American/PSX
                # style) -> ` - ` (proper German). Consume one
                # optional adjacent space on each side so we don't
                # double up.
                german = re.sub(r' ?-- ?', ' - ', german)

                # 4. Strip PSX `[16:02]` wait-opcode when followed by
                # another [16:XX] (menu/portrait lead-in PC's engine
                # doesn't need; audit Pattern #3, 119 entries).
                german = self._PSX_MENU_WAIT_RE.sub('', german)

                # 5. Collapse PSX `[05:0A][15:00][06]` button-icon trio
                # (NN=00 form) to PC `~` (0x7E). Runs BEFORE the orphan
                # color-end strip so the trio's matching `[06]` is
                # consumed as a unit; audit Pattern #14, 18 entries.
                german = self._PSX_TRIO_NN00_RE.sub('\x7e', german)

                # 6. Drop orphan `[06]` color-end opcodes (no preceding
                # unclosed [05:XX] color-start). PSX wraps speaker-name
                # labels in a stray `[06]` PC source omits entirely;
                # audit Pattern #5, 63 entries.
                german = self._strip_orphan_color_end(german)

                if german != original:
                    trans[key] = german
                    changed = True
                    total_cleaned += 1

            if changed:
                files_touched += 1
                write_translations_json(json_path, trans)

        self._load_translations()
        self._populate_tree()
        # Re-render the open string so the widget shows the cleaned text.
        # Without this, a subsequent Save would commit the pre-clean
        # widget content back over the freshly cleaned JSON.
        self._refresh_current_widget()
        messagebox.showinfo("Clean PSX Codes",
                            f"Cleaned {total_cleaned} translations "
                            f"across {files_touched} files.\n\n"
                            f"Conversions applied:\n"
                            f"  [17:XX:XX] \u2192 [14:XX:XX] (portrait)\n"
                            f"  [16:XX] \u2192 [13:XX] (timed text)\n"
                            f"  [19:XX] \u2192 [16:XX] (timed text variant)\n"
                            f"  [14:XX] \u2192 [12:XX]\n"
                            f"  [1C:XX] \u2192 [19:XX]\n"
                            f"  [90] \u2192 ... (ellipsis)\n"
                            f"  -- \u2192  -  (German em-dash)\n"
                            f"  [16:02][16:XX] \u2192 [16:XX] "
                            f"(strip menu wait)\n"
                            f"  [05:0A][15:00][06] \u2192 ~ "
                            f"(action button)\n"
                            f"  orphan [06] \u2192 (dropped)")

    def _repack_all(self):
        """Export all translated files and repack them."""
        game_dir = self._get_game_dir()
        if not game_dir:
            return
        self._commit_current()
        self._save()
        self._apply_manual_dups()   # materialize masters into targets on disk

        # Find all files with translations
        bases = []
        for fname in sorted(os.listdir(TRANS_DIR)):
            if not fname.endswith(".json") or _is_aux_json(fname):
                continue
            json_path = os.path.join(TRANS_DIR, fname)
            try:
                trans = load_translations_json(json_path)
                if any(v.strip() for v in trans.values()):
                    txt_name = fname.replace(".json", ".txt")
                    self._export_file_for_repack(txt_name)
                    bases.append(fname.replace(".json", ""))
            except Exception:
                continue

        if not bases:
            messagebox.showinfo("Repack All", "No files with translations to repack.")
            return

        self._run_repack(bases, game_dir, sync_shared=True)

    def _export_file_for_repack(self, txt_name):
        """Export a single file's translations to .txt format for the repacker."""
        base = txt_name.replace(".txt", "")
        json_path = os.path.join(TRANS_DIR, base + ".json")
        if not os.path.exists(json_path):
            return
        trans = load_translations_json(json_path)
        if not any(v.strip() for v in trans.values()):
            return

        src_path = os.path.join(DUMP_DIR, txt_name)
        if not os.path.exists(src_path):
            return

        cur_num = None
        out_lines = []
        with open(src_path, encoding="utf-8") as f:
            for line in f:
                stripped = line.rstrip("\n")
                m_e = re.match(r"## Entry (\d+) @ ", stripped)
                if m_e:
                    cur_num = m_e.group(1)
                stripped_nc = self._SUFFIX_COMMENT_RE.sub("", stripped)
                m_s = STR_RE.match(stripped_nc)
                if m_s and cur_num:
                    offset = m_s.group(1)
                    key = self._key(cur_num, offset)
                    # Sealed monster names export as ENGLISH regardless of any
                    # (possibly stale) German in the JSON — the seal safeguard is
                    # enforced at the output, not just hidden in the UI.
                    if self._is_monster_sealed(base, key) or self._is_uncensor_sealed(base, key):
                        out_lines.append(line)
                        continue
                    if key in trans and trans[key].strip():
                        raw_trans = display_to_export(trans[key])
                        out_lines.append(f"{offset} '{raw_trans}'\n")
                        continue
                out_lines.append(line)

        out_path = os.path.join(TRANS_DIR, txt_name)
        with open(out_path, "w", encoding="utf-8") as f:
            f.writelines(out_lines)

    def _rollback_patch_dir(self):
        """Folder holding the bundled GOG rollback patches (source-run or frozen).
        Returns the first candidate dir that actually contains a known patch."""
        cands = [SCRIPT_DIR]
        mp = getattr(sys, "_MEIPASS", None)
        if mp:
            cands.append(mp)
        cands.append(os.path.dirname(os.path.abspath(_gogroll.__file__)))
        for d in cands:
            if any(os.path.isfile(os.path.join(d, e["patch"]))
                   for e in _gogroll.PATCHES):
                return d
        return None

    def _gog_rollback(self):
        """Convert the user's GOG BOF4.exe back to the mod-compatible update 6,
        in place, using whichever bundled binary-delta patch matches their build.
        Heavily gated: confirms with the user, refuses anything without a matching
        patch, backs up the original, and verifies the result before keeping it."""
        if _gogroll is None:
            messagebox.showerror("Rollback", "gog_rollback module not available.")
            return
        pdir = self._rollback_patch_dir()
        if not pdir:
            messagebox.showerror(
                "Rollback",
                "Rollback patch(es) not found next to the editor. They ship with "
                "the tool — reinstall if they're missing.")
            return
        # Locate the game exe: prefer the configured game dir, else ask.
        gd = self._config.get("game_dir") or GAME_ROOT
        exe = os.path.join(gd, "BOF4.exe")
        if not os.path.isfile(exe):
            exe = filedialog.askopenfilename(
                title="Select your GOG BOF4.exe",
                initialdir=gd if os.path.isdir(gd) else None,
                filetypes=[("BOF4.exe", "BOF4.exe"), ("All exe", "*.exe")])
            if not exe:
                return
        # Classify before touching anything.
        try:
            kind, entry = _gogroll.classify(open(exe, "rb").read())
        except Exception as e:
            messagebox.showerror("Rollback", f"Could not read exe:\n{e}")
            return
        if kind == "target":
            messagebox.showinfo(
                "Rollback",
                f"This BOF4.exe is already {_gogroll.TARGET_NAME} (the "
                "mod-compatible build). Nothing to do.")
            return
        if kind != "rollbackable":
            messagebox.showwarning(
                "Rollback",
                "This BOF4.exe isn't a build I have a rollback patch for.\n\n"
                "It may be a different/newer version, or a non-GOG (e.g. Steam) "
                "exe, or already modified. I won't touch it. If GOG shipped a "
                "newer update, a matching patch needs to be added to the tool.")
            return
        if not messagebox.askyesno(
                f"Roll back to {_gogroll.TARGET_NAME}?",
                f"Your BOF4.exe is {entry['name']}, which the mod can't fully "
                "support (the uncensored scenes can't be restored on it).\n\n"
                f"This will convert it back to {_gogroll.TARGET_NAME} (the "
                "mod-compatible build), in place. Your original is backed up to "
                "BOF4.exe.rollback_backup first, and the result is verified "
                f"before it's kept.\n\nExe:\n{exe}\n\nProceed?"):
            return
        msgs = []
        ok = _gogroll.rollback(exe, pdir, log=msgs.append)
        report = "\n".join(msgs)
        if ok:
            self.status_var.set("GOG exe rolled back to update 6.")
            messagebox.showinfo("Rollback complete", report)
        else:
            messagebox.showerror("Rollback not applied", report)

    def _launch_tool(self, rel_path, friendly, exe_names=None):
        """Launch a companion GUI tool as an independent process.

        rel_path is POSIX-style, relative to the tools/ folder (this editor's
        PARENT) — the companion tools are siblings of this editor folder. The
        tool runs detached (Popen, no wait) so it doesn't block the editor,
        with cwd=GAME_ROOT so its game-relative defaults (DAT/, INIT.DAT, ...)
        resolve. Prefers pythonw.exe to avoid a stray console.
        """
        tools_dir = os.path.dirname(SCRIPT_DIR)   # .../localization_editor/tools
        script = os.path.join(tools_dir, *rel_path.split("/"))
        stem = os.path.splitext(os.path.basename(rel_path))[0]   # e.g. 'bof4_gui'
        # Prefer a packaged sibling .exe (ships to users who have no Python).
        # Two supported layouts, tried in order:
        #  1) flat sibling next to THIS exe — the shared-runtime bundle, where
        #     all tools live together in one BoF4_Translation_Tools/ folder;
        #  2) per-folder sibling next to the source .py — the dev / old onefile
        #     layout (…/graphics_encoder/bof4_gui.exe).
        # Explicit exe names (the renamed release binaries) are checked first as
        # flat siblings, then the stem-derived name, then the per-folder path —
        # so this works for the renamed bundle, the old bundle, AND dev/source.
        exe_candidates = [os.path.join(SCRIPT_DIR, n) for n in (exe_names or [])]
        exe_candidates += [
            os.path.join(SCRIPT_DIR, stem + ".exe"),
            os.path.splitext(script)[0] + ".exe",
        ]
        for exe_sibling in exe_candidates:
            if os.path.exists(exe_sibling):
                try:
                    subprocess.Popen([exe_sibling], cwd=GAME_ROOT)
                    self.status_var.set(f"Launched {friendly}.")
                except Exception as e:
                    messagebox.showerror(
                        "Launch failed", f"Could not launch {friendly}:\n{e}")
                return
        if not os.path.exists(script):
            messagebox.showerror(
                "Tool not found",
                f"{friendly} was not found next to the editor:\n"
                f"{exe_candidates[0]}\n(nor as a script at {script})")
            return
        # Source-run path: prefer pythonw.exe (no console window) for GUI tools.
        # If this editor is itself a frozen .exe, sys.executable is the editor —
        # not a Python — so there is no interpreter to run a loose .py.
        if getattr(sys, "frozen", False):
            messagebox.showerror(
                "Tool not found",
                f"{friendly} needs its packaged .exe next to the editor:\n"
                f"{exe_candidates[0]}")
            return
        exe = sys.executable
        pyw = os.path.join(os.path.dirname(exe), "pythonw.exe")
        if os.path.exists(pyw):
            exe = pyw
        try:
            subprocess.Popen([exe, script], cwd=GAME_ROOT)
            self.status_var.set(f"Launched {friendly}.")
        except Exception as e:
            messagebox.showerror(
                "Launch failed", f"Could not launch {friendly}:\n{e}")

    def _dump_text_from_dat(self):
        """Extract English source text from a chosen DAT folder into the current
        source (text_dump) folder, via extract_text.py. This is how a fresh /
        standalone editor gets its file list populated: pick the game's DAT
        folder (ideally DAT_backup/ = untouched English), and .txt dumps land in
        DUMP_DIR (whatever 'Source…' points at)."""
        extract_script = os.path.join(SCRIPT_DIR, "extract_text.py")
        # When frozen, extract_text is bundled inside this exe (run via
        # --run-helper), so the .py won't exist on disk — skip the file check.
        if not getattr(sys, "frozen", False) and not os.path.exists(extract_script):
            messagebox.showerror("Dump Text",
                                 f"extract_text.py not found at:\n{extract_script}")
            return

        # Suggest DAT_backup (English originals) first, then DAT/, under the
        # configured game dir or GAME_ROOT.
        gd = self._get_game_dir() or GAME_ROOT
        suggest = ""
        for cand in (os.path.join(gd, "DAT_backup"), os.path.join(gd, "DAT"),
                     os.path.join(GAME_ROOT, "DAT_backup"),
                     os.path.join(GAME_ROOT, "DAT")):
            if os.path.isdir(cand):
                suggest = cand
                break

        dat_dir = filedialog.askdirectory(
            title="Select DAT folder to dump text FROM "
                  "(DAT_backup = untouched English source is best)",
            initialdir=suggest if suggest else GAME_ROOT)
        if not dat_dir:
            self.status_var.set("Dump Text cancelled.")
            return

        import glob as _glob
        n_dats = len(_glob.glob(os.path.join(dat_dir, "*.DAT")))
        if n_dats == 0:
            messagebox.showerror(
                "Dump Text",
                f"No .DAT files found in:\n{dat_dir}\n\n"
                "Pick the game's DAT/ or DAT_backup/ folder.")
            return

        # Japanese DATs are not supported yet — pairing against them is poor. If
        # the chosen folder looks Japanese (…/japanese/…), block with a notice.
        # (JP decoding via jp_tables/ + --jp is temporarily disabled.)
        guess_jp = "japanese" in dat_dir.replace("\\", "/").lower()
        if guess_jp:
            messagebox.showinfo(
                "Dump Text — Japanese not supported yet",
                "Japanese DATs are not supported yet.\n\n"
                "The Japanese script can be decoded, but translation-pairing "
                "against it isn't ready, so dumping Japanese text is disabled "
                "for now. Please pick an English DAT folder "
                "(DAT_backup / DAT).")
            self.status_var.set("Dump Text cancelled — Japanese DAT not supported yet.")
            return
        is_jp = False   # English-only until JP support is re-enabled

        # Confirm the destination (the editor's current source folder).
        existing = [f for f in os.listdir(DUMP_DIR) if f.endswith(".txt")] \
            if os.path.isdir(DUMP_DIR) else []
        msg = (f"Dump {'JAPANESE' if is_jp else 'English'} text from {n_dats} "
               f"DAT(s) in:\n{dat_dir}\n\n"
               f"…into the source folder:\n{DUMP_DIR}\n\n")
        if existing:
            msg += (f"This OVERWRITES the {len(existing)} existing .txt file(s) "
                    "there (your translations/ are NOT touched).\n\n")
        msg += "Proceed?"
        if not messagebox.askyesno("Dump Text", msg):
            self.status_var.set("Dump Text cancelled.")
            return

        os.makedirs(DUMP_DIR, exist_ok=True)
        if getattr(sys, "frozen", False):
            cmd = [sys.executable, "--run-helper", "extract_text",
                   "--dat-dir", dat_dir, "--out-dir", DUMP_DIR]
        else:
            cmd = [sys.executable, extract_script,
                   "--dat-dir", dat_dir, "--out-dir", DUMP_DIR]
        if is_jp:
            cmd.append("--jp")
        self.status_var.set(
            f"Dumping {'JP' if is_jp else 'EN'} text from {n_dats} DAT(s) "
            f"→ {DUMP_DIR} …")
        self.update()
        try:
            result = subprocess.run(cmd, capture_output=True, text=True,
                                    cwd=SCRIPT_DIR, timeout=600)
            output = (result.stdout or "") + (result.stderr or "")
            ok = (result.returncode == 0) and "Traceback" not in output
            if ok:
                self._load_file_list()
                n_txt = len([f for f in os.listdir(DUMP_DIR)
                             if f.endswith(".txt")]) if os.path.isdir(DUMP_DIR) else 0
                self.status_var.set(
                    f"Dumped text → {n_txt} .txt file(s) in source folder.")
                messagebox.showinfo(
                    "Dump Text — complete",
                    f"Extracted text into:\n{DUMP_DIR}\n\n"
                    f"{n_txt} .txt file(s) now available.\n\n"
                    + (output[-1500:] if output.strip() else ""))
            else:
                self.status_var.set("Dump Text FAILED — see dialog.")
                messagebox.showerror(
                    "Dump Text — failed",
                    "extract_text.py reported an error:\n\n" + output[-3000:])
        except subprocess.TimeoutExpired:
            self.status_var.set("Dump Text timed out.")
            messagebox.showerror("Dump Text",
                                 "extract_text.py timed out (>600s).")
        except Exception as ex:
            self.status_var.set("Dump Text error.")
            messagebox.showerror("Dump Text",
                                 f"Exception running extract_text.py:\n\n{ex}")

    def _run_repack(self, bases, game_dir, sync_shared=True):
        """Run repack_text.py as a subprocess for the given base names."""
        repack_script = os.path.join(SCRIPT_DIR, "repack_text.py")
        # When frozen, repack_text is bundled inside this exe (run via
        # --run-helper), so the .py won't exist on disk — skip the file check.
        if not getattr(sys, "frozen", False) and not os.path.exists(repack_script):
            messagebox.showerror("Repack Error",
                                 f"repack_text.py not found at:\n{repack_script}")
            return

        # Gate: the pristine text source must be verified before we rebuild text
        # from it (prevents the German-source scramble). Shows the fallback popup.
        if not self._pre_repack_pristine_gate():
            self.status_var.set("Repack cancelled — pristine check.")
            return

        # Build list of .txt files to repack
        txt_files = []
        for base in bases:
            txt_path = os.path.join(TRANS_DIR, base + ".txt")
            if os.path.exists(txt_path):
                txt_files.append(txt_path)

        if not txt_files:
            messagebox.showinfo("Repack", "No exported .txt files found to repack.")
            return

        out_dir = self._get_repack_out_dir(game_dir)
        if not out_dir:
            self.status_var.set("Repack cancelled — no output folder chosen.")
            return

        import_dir = self._get_repack_import_dir()
        gfx_note = (f"  (graphics from {os.path.basename(import_dir)})"
                    if import_dir else "  (no graphics overlay)")
        self.status_var.set(
            f"Repacking {len(txt_files)} file(s) -> {out_dir}{gfx_note} ...")
        self.update()

        if getattr(sys, "frozen", False):
            cmd = [sys.executable, "--run-helper", "repack_text"] + txt_files
        else:
            cmd = [sys.executable, repack_script] + txt_files
        if sync_shared:
            cmd.append("--sync-shared")
        # ' ' always repacks as plain ASCII 0x20: the engine renderer patch now
        # draws 0x20 at its proper VWF width, so no glyph substitution is needed.
        cmd.append("--legacy-space")
        cmd += ["--out-dir", out_dir]
        if import_dir:
            cmd += ["--import-dir", import_dir]

        try:
            result = subprocess.run(cmd, capture_output=True, text=True,
                                     cwd=game_dir, timeout=120)
            output = result.stdout + result.stderr

            # A zero return code is the primary success signal, but also
            # scan the output for error markers that some failure modes
            # print without setting a non-zero exit code.
            error_markers = ('Traceback', 'ValueError', 'AttributeError',
                             'ERROR:', '  error:', 'Exception', 'SKIP')
            has_error_text = any(m in output for m in error_markers)
            success = (result.returncode == 0) and not has_error_text

            if success:
                self.status_var.set(f"Repacked {len(txt_files)} file(s) successfully.")
                self._show_repack_output(
                    "Repack Complete", output, success=True,
                    repacked_count=len(txt_files))
                # Reload current file but preserve position so the user stays
                # on the string they were editing, instead of jumping back to
                # the top of the (most-translated) entry.
                #
                # We derive the saved row from current_entry_idx/current_str_idx
                # rather than from tree.selection(): _repack_all()/_repack_current()
                # call _save() before us, and _save() rebuilds the tree via
                # _populate_tree(), which WIPES the tree selection. The index
                # pair survives that, so it's the reliable anchor.
                saved_entry = self.current_entry_idx
                saved_str   = self.current_str_idx
                saved_offset = None
                if self.entries and 0 <= saved_entry < len(self.entries):
                    strs = self.entries[saved_entry]["strings"]
                    if 0 <= saved_str < len(strs):
                        saved_offset = strs[saved_str]["offset"]
                self._on_file_select()
                if self.entries and 0 <= saved_entry < len(self.entries):
                    self.current_entry_idx = saved_entry
                    try:
                        self.entry_cb.current(saved_entry)
                    except Exception:
                        pass
                    if saved_str < len(self.entries[saved_entry]["strings"]):
                        self.current_str_idx = saved_str
                    self._load_entry()
                    # _load_entry() auto-selects the first row; override it to
                    # restore the exact row the user had (if it's still visible
                    # under the active filter).
                    if saved_offset is not None and self.tree.exists(saved_offset):
                        self.tree.selection_set(saved_offset)
                        self.tree.see(saved_offset)
                        self._on_str_select()
            else:
                if result.returncode != 0:
                    self.status_var.set(
                        f"Repack failed (exit code {result.returncode})")
                else:
                    self.status_var.set("Repack finished with errors in output.")
                self._show_repack_output(
                    "Repack Error", output, success=False,
                    repacked_count=len(txt_files))
        except subprocess.TimeoutExpired:
            self.status_var.set("Repack timed out!")
            self._show_repack_output(
                "Repack Error",
                "Repack process timed out after 120 seconds.\n\n"
                "The repacker was still running when the timer expired. "
                "The DAT files may be in an inconsistent state — restore "
                "from DAT_backup/ if needed.",
                success=False, repacked_count=0)
        except Exception as ex:
            self.status_var.set("Repack failed!")
            self._show_repack_output(
                "Repack Error",
                f"Exception running repack_text.py:\n\n{ex}",
                success=False, repacked_count=0)

    def _show_repack_output(self, title, output, success, repacked_count=0):
        """Show repack output in a scrollable dialog.

        The button at the bottom is relabelled and recoloured based on
        whether the repack succeeded (green, 'Success & Close') or
        produced errors (red, 'Close due to Errors')."""
        win = tk.Toplevel(self)
        win.title(title)
        win.geometry("760x480")
        win.configure(bg=self.BG)
        win.transient(self)

        # Status banner at the top — large, colour-coded
        if success:
            banner_bg = "#2D5A2D"  # green
            banner_fg = "#CCEECC"
            banner_text = (f"  Repack complete  ({repacked_count} "
                           f"file(s) written successfully)")
        else:
            banner_bg = "#5A2D2D"  # red
            banner_fg = "#FFCCCC"
            banner_text = "  Repack finished with errors"
        tk.Label(win, text=banner_text, bg=banner_bg, fg=banner_fg,
                 font=("Segoe UI", 11, "bold"), anchor=tk.W,
                 padx=10, pady=6).pack(fill=tk.X)

        # Output text box with a scrollbar
        body = tk.Frame(win, bg=self.BG)
        body.pack(fill=tk.BOTH, expand=True, padx=10, pady=10)

        text = tk.Text(body, bg=self.BG3, fg=self.FG2, font=("Consolas", 9),
                       wrap=tk.WORD, state=tk.NORMAL)
        scroll = ttk.Scrollbar(body, orient=tk.VERTICAL, command=text.yview)
        text.configure(yscrollcommand=scroll.set)
        text.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        scroll.pack(side=tk.RIGHT, fill=tk.Y)

        # Highlight error lines in red so they're easy to spot
        text.tag_configure("err", foreground="#FF6666",
                           font=("Consolas", 9, "bold"))

        body_text = output if output.strip() else "(no output)"
        text.insert("1.0", body_text)

        if not success:
            # Scan the inserted content and mark error-ish lines.
            error_markers = ('Traceback', 'ValueError', 'AttributeError',
                             'Error', 'error:', 'Exception', 'SKIP', 'FAIL')
            for line_no, line in enumerate(body_text.splitlines(), start=1):
                if any(m in line for m in error_markers):
                    text.tag_add("err", f"{line_no}.0", f"{line_no}.end")
            # Auto-scroll to the first highlighted line
            idx = text.search("err", "1.0", stopindex=tk.END)
            first_err = text.tag_nextrange("err", "1.0")
            if first_err:
                text.see(first_err[0])

        text.config(state=tk.DISABLED)

        # Bottom button — labelled and coloured based on outcome
        if success:
            btn_text = "Success & Close"
            btn_bg   = "#4C6A4C"   # green
            btn_hover = "#5C7A5C"
        else:
            btn_text = "Close due to Errors"
            btn_bg   = "#6A4C4C"   # red
            btn_hover = "#7A5C5C"
        btn = tk.Button(win, text=btn_text, command=win.destroy,
                        bg=btn_bg, fg="white", relief=tk.FLAT,
                        padx=18, pady=6,
                        font=("Segoe UI", 10, "bold"))
        btn.pack(pady=(0, 12))
        btn.bind("<Enter>", lambda e: btn.config(bg=btn_hover))
        btn.bind("<Leave>", lambda e: btn.config(bg=btn_bg))

    # ── Recently edited tracking ──────────────────────────────────────────────
    def _track_recently_edited(self, s, key):
        preview = raw_to_display(s["raw"])[:40].replace("\n", " ")
        entry = (self.current_file, key, preview)
        # Remove if already in list
        self._recently_edited = [e for e in self._recently_edited
                                  if not (e[0] == entry[0] and e[1] == entry[1])]
        self._recently_edited.insert(0, entry)
        self._recently_edited = self._recently_edited[:50]

    # ── Overall progress ──────────────────────────────────────────────────────
    def _calculate_overall_progress(self):
        """Calculate translation progress across all files."""
        total_done = 0
        total_strings = 0
        for fname in sorted(os.listdir(DUMP_DIR)):
            if not fname.endswith(".txt"):
                continue
            base = fname[:-4]
            # Load translations
            trans_path = os.path.join(TRANS_DIR, base + ".json")
            trans = {}
            if os.path.exists(trans_path):
                try:
                    trans = load_translations_json(trans_path)
                except Exception:
                    pass

            # Parse dump file
            path = os.path.join(DUMP_DIR, fname)
            _, entries = parse_dump(path)
            file_done = 0
            file_total = 0
            for e in entries:
                for s in e["strings"]:
                    if s.get("suffix_of"):
                        continue
                    key = self._key(e["num"], s["offset"])
                    # Skip non-primary duplicates
                    is_dup, _, _ = self._is_duplicate(self._dup_raw(s), base, key)
                    if is_dup:
                        continue
                    # Sealed monster names are intentionally kept English.
                    if self._is_monster_sealed(base, key) or self._is_uncensor_sealed(base, key):
                        continue
                    file_total += 1
                    if trans.get(key, "").strip():
                        file_done += 1

            self._overall_progress[fname] = (file_done, file_total)
            total_done += file_done
            total_strings += file_total

        self._overall_progress["__total__"] = (total_done, total_strings)

    def _update_overall_progress_display(self):
        done, total = self._overall_progress.get("__total__", (0, 0))
        if total:
            pct = int(100 * done / total)
            self._overall_lbl.config(
                text=f"Overall: {done}/{total} ({pct}%)")
        else:
            self._overall_lbl.config(text="")

    # ── Utilities ──────────────────────────────────────────────────────────────
    def _on_close(self):
        if self._dirty:
            if messagebox.askyesno("Unsaved changes",
                                   "You have unsaved translations.\nSave before closing?"):
                self._save()
        if self._autosave_id:
            self.after_cancel(self._autosave_id)
        self.destroy()


if __name__ == "__main__":
    app = LocEditor()
    app.mainloop()
