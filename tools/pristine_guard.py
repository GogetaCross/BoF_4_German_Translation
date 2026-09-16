"""
Shared pristine-DAT guard for the BoF4 localization pipeline.

The text repacker rebuilds text from a PRISTINE-US backup. This module verifies a
folder really is that pristine reference by comparing each DAT's TEXT fingerprint
(SHA1 of f2==0 entry content — graphics-invariant, text-sensitive) against the
shipped manifest (pristine_text_manifest.json, built by build_pristine_manifest.py).

Used by:
  * the localization editor — on load (status) and before Repack (gate + popup),
  * repack_text.py — to refuse a contaminated backup before it scrambles text.

dat_text_hash() MUST match build_pristine_manifest.py exactly (it imports from here).
"""
import os, sys, json, struct, hashlib

# When frozen into the editor .exe (PyInstaller), __file__ points inside the temp
# _MEIPASS extraction dir. Anchor the WRITABLE manifest next to the real .exe, and
# keep a read-only BUNDLED copy (shipped inside the exe) as the fallback seed.
if getattr(sys, "frozen", False):
    HERE = os.path.dirname(sys.executable)
    _BUNDLED_DIR = getattr(sys, "_MEIPASS", HERE)
else:
    HERE = os.path.dirname(os.path.abspath(__file__))
    _BUNDLED_DIR = HERE
MANIFEST_PATH = os.path.join(HERE, "pristine_text_manifest.json")
_BUNDLED_MANIFEST = os.path.join(_BUNDLED_DIR, "pristine_text_manifest.json")

# Game-text byte ranges that German adds: ä ö ü Ä Ö Ü ß map to high bytes the US
# text never uses. Presence in a TEXT entry strongly implies "already translated".
# Used only as advisory in the future-version popup (not for the hash check).
GERMAN_MARKER_BYTES = frozenset({0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96})


def dat_text_hash(path):
    """(sha1_hex, n_text_entries) over the content of every f2==0 (text) entry, or
    (None, 0) if not a parseable DAT. Graphics-invariant: f2!=0 entries (textures/
    audio) and file offsets are excluded, so graphics-only edits hash identical."""
    try:
        buf = open(path, "rb").read()
    except OSError:
        return None, 0
    if len(buf) < 16:
        return None, 0
    toc_size = struct.unpack_from("<I", buf, 0)[0]
    n = toc_size // 16
    if n <= 0 or n > 100000:
        return None, 0
    h = hashlib.sha1()
    count = 0
    for i in range(n):
        base = i * 16
        if base + 16 > len(buf):
            break
        off, size, f2, _f3 = struct.unpack_from("<IIII", buf, base)
        if f2 != 0:
            continue
        h.update(struct.pack("<II", i, size))
        if size and off + size <= len(buf):
            h.update(buf[off:off + size])
            count += 1
    return h.hexdigest(), count


def _accepted_aliases(name):
    """Extra-accepted text_sha1 hashes for a DAT beyond the manifest's pristine
    one (e.g. the uncensor-materialised variant). Sourced from uncensor_patch so
    the allowance lives with the patch and survives manifest rebuilds. Empty if
    that module is unavailable (older bundle)."""
    try:
        import uncensor_patch
        return uncensor_patch.accepted_text_sha1(name)
    except Exception:
        return set()


def text_hash_ok(name, text_sha1, man_entry):
    """True if a DAT's TEXT fingerprint is a valid pristine repack source: it
    equals the manifest's text_sha1, OR an explicitly-accepted alias for that DAT
    (the optional per-DAT gate — see uncensor_patch.ACCEPTED_TEXT_SHA1)."""
    if text_sha1 == man_entry.get("text_sha1"):
        return True
    return text_sha1 in _accepted_aliases(name)


def dat_has_german_markers(path):
    """True if any TEXT entry contains German umlaut/ß bytes — advisory heuristic
    to tell 'already-translated' from 'clean newer version' when the hash mismatches."""
    try:
        buf = open(path, "rb").read()
    except OSError:
        return False
    if len(buf) < 16:
        return False
    n = struct.unpack_from("<I", buf, 0)[0] // 16
    if n <= 0 or n > 100000:
        return False
    for i in range(n):
        off, size, f2, _f3 = struct.unpack_from("<IIII", buf, i * 16)
        if f2 != 0 or not size or off + size > len(buf):
            continue
        if any(b in GERMAN_MARKER_BYTES for b in buf[off:off + size]):
            return True
    return False


def load_manifest(path=None):
    """Parsed manifest dict, or None if missing/corrupt.

    Tries the writable copy next to the .exe first, then the bundled seed shipped
    inside the exe — so a frozen build still finds its manifest even before any
    'Use anyway' learned copy has been written next to the exe."""
    candidates = [path] if path else [MANIFEST_PATH, _BUNDLED_MANIFEST]
    for p in candidates:
        if not p:
            continue
        try:
            with open(p, encoding="utf-8") as f:
                d = json.load(f)
            if isinstance(d, dict) and "dats" in d:
                return d
        except (OSError, ValueError):
            continue
    return None


def check_dir(dat_dir, manifest=None):
    """Compare a DAT folder's text fingerprints against the manifest.

    Returns dict:
      ok        : [names]  text matches pristine
      modified  : [names]  text DIFFERS (likely already-translated) — the blockers
      german    : [names]  subset of modified that has German umlaut bytes (strong tell)
      unknown   : [names]  present in folder but not in the manifest (skip, don't block)
      missing   : [names]  in the manifest but absent from the folder
      no_manifest: bool
    """
    if manifest is None:
        manifest = load_manifest()
    res = {"ok": [], "modified": [], "german": [], "unknown": [],
           "missing": [], "no_manifest": manifest is None}
    if manifest is None:
        return res
    man = manifest.get("dats", {})
    present = {}
    if os.path.isdir(dat_dir):
        for fn in os.listdir(dat_dir):
            if fn.lower().endswith(".dat"):
                present[fn] = os.path.join(dat_dir, fn)
    # Case-insensitive name match against the manifest keys.
    man_by_lower = {k.lower(): k for k in man}
    for fn, p in sorted(present.items()):
        key = man_by_lower.get(fn.lower())
        if key is None:
            res["unknown"].append(fn)
            continue
        hh, _ = dat_text_hash(p)
        if text_hash_ok(fn, hh, man[key]):
            res["ok"].append(fn)
        else:
            res["modified"].append(fn)
            if dat_has_german_markers(p):
                res["german"].append(fn)
    present_lower = {fn.lower() for fn in present}
    for key in man:
        if key.lower() not in present_lower:
            res["missing"].append(key)
    return res


def build_manifest(dat_dir):
    """Build a manifest dict from a DAT folder assumed pristine (for 'Use anyway' /
    learning a new game version). Mirrors build_pristine_manifest.py."""
    import glob
    dats = {}
    for p in sorted(set(glob.glob(os.path.join(dat_dir, "*.DAT")) +
                        glob.glob(os.path.join(dat_dir, "*.dat"))),
                    key=lambda x: os.path.basename(x).lower()):
        hh, cnt = dat_text_hash(p)
        if hh is None:
            continue
        dats[os.path.basename(p)] = {"text_sha1": hh, "text_entries": cnt}
    init = os.path.join(dat_dir, "INIT.DAT")
    return {"version": 1, "source": os.path.basename(dat_dir.rstrip("/\\")),
            "note": "Per-DAT SHA1 of TEXT entries (f2==0) content only — graphics-invariant.",
            "init_size": os.path.getsize(init) if os.path.exists(init) else None,
            "dat_count": len(dats), "dats": dats}


def save_manifest(manifest, path=MANIFEST_PATH):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1)


def is_pristine(dat_dir, manifest=None):
    """True if the folder has NO text-modified DATs known to the manifest (graphics-
    only edits are fine). Unknown/missing DATs don't fail it."""
    r = check_dir(dat_dir, manifest)
    return (not r["no_manifest"]) and not r["modified"]
