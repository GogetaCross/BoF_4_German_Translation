"""Spell-checking + LibreOffice dictionary management for the localization
editor. No Tkinter here — pure logic so it can be unit-tested headless.

Dictionaries are Hunspell .aff/.dic pairs from the LibreOffice/dictionaries
GitHub repo, stored flat in `localization_editor/dictionaries/<base>.{aff,dic}`.
Spell-checking uses spylls (pure-Python Hunspell) when importable — full affix
+ suggestion support — and degrades to a plain .dic word-set otherwise.

The engine is target-language-neutral: enable any installed dictionary (or
several at once) and a word is accepted if ANY enabled dictionary accepts it.
"""
import os
import re
import json
import difflib
import urllib.request

# ── LibreOffice dictionaries repo ───────────────────────────────────────────
_REPO = "LibreOffice/dictionaries"
_API  = f"https://api.github.com/repos/{_REPO}/contents/"
_UA   = {"User-Agent": "bof4-localization-editor"}


def _http_json(url, timeout=30):
    req = urllib.request.Request(url, headers=_UA)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.load(r)


def _http_bytes(url, timeout=90):
    req = urllib.request.Request(url, headers=_UA)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def list_online_languages():
    """Top-level language folders in the repo (e.g. 'de', 'en', 'fr_FR')."""
    data = _http_json(_API)
    return sorted(d["name"] for d in data if d.get("type") == "dir")


def list_online_dictionaries(folder):
    """Return [{base, dic_url, aff_url}] for every .dic that has a matching
    .aff inside `folder` (a language folder name from list_online_languages)."""
    data = _http_json(_API + folder)
    files = {d["name"]: d for d in data if d.get("type") == "file"}
    out = []
    for name, meta in sorted(files.items()):
        if name.endswith(".dic"):
            base = name[:-4]
            aff = base + ".aff"
            if aff in files:
                out.append({
                    "base": base,
                    "dic_url": files[name]["download_url"],
                    "aff_url": files[aff]["download_url"],
                })
    return out


def install_dictionary(dest_dir, entry):
    """Download the .dic + .aff of `entry` (from list_online_dictionaries) into
    dest_dir. Returns the base name. Writes to a temp name then renames so a
    failed download never leaves a half-written dictionary behind."""
    os.makedirs(dest_dir, exist_ok=True)
    dic = _http_bytes(entry["dic_url"])
    aff = _http_bytes(entry["aff_url"])
    base = entry["base"]
    for data, ext in ((dic, ".dic"), (aff, ".aff")):
        final = os.path.join(dest_dir, base + ext)
        tmp = final + ".part"
        with open(tmp, "wb") as f:
            f.write(data)
        os.replace(tmp, final)
    return base


# ── Spell engine ────────────────────────────────────────────────────────────
class SpellManager:
    # A "word" for spell purposes: runs of letters (any script, incl. umlauts),
    # optionally joined by an apostrophe or hyphen. Digits/underscores excluded.
    WORD_RE = re.compile(r"[^\W\d_]+(?:['’\-][^\W\d_]+)*", re.UNICODE)

    def __init__(self, dict_dir, enabled=None, personal=None):
        self.dict_dir = dict_dir
        self.enabled = list(enabled or [])
        self.personal = set(w.lower() for w in (personal or []))
        self._dicts = {}     # base -> spylls Dictionary  OR  set[str] (fallback)
        self._cache = {}     # word.lower() -> bool
        self._spylls_ok = False
        self._load_enabled()

    # -- dictionary discovery -------------------------------------------------
    def installed(self):
        """Base names of installed dictionaries (both .dic and .aff present)."""
        out = []
        if os.path.isdir(self.dict_dir):
            for f in os.listdir(self.dict_dir):
                if f.endswith(".dic"):
                    base = f[:-4]
                    if os.path.exists(os.path.join(self.dict_dir, base + ".aff")):
                        out.append(base)
        return sorted(out)

    @staticmethod
    def spylls_available():
        try:
            import spylls.hunspell  # noqa: F401
            return True
        except Exception:
            return False

    # -- loading --------------------------------------------------------------
    def _load_wordset(self, dic_path):
        """Fallback engine: a lowercase set of the .dic stems (affixes ignored)."""
        words = set()
        try:
            with open(dic_path, encoding="utf-8", errors="replace") as f:
                next(f, None)  # first line is the entry count
                for line in f:
                    stem = line.strip().split("/", 1)[0].strip()
                    if stem:
                        words.add(stem.lower())
        except Exception:
            pass
        return words

    def _load_enabled(self):
        self._dicts = {}
        self._cache = {}
        try:
            from spylls.hunspell import Dictionary
            self._spylls_ok = True
        except Exception:
            Dictionary = None
            self._spylls_ok = False
        for base in self.enabled:
            path = os.path.join(self.dict_dir, base)
            dic, aff = path + ".dic", path + ".aff"
            if not os.path.exists(dic):
                continue
            if Dictionary is not None and os.path.exists(aff):
                try:
                    self._dicts[base] = Dictionary.from_files(path)
                    continue
                except Exception:
                    pass
            self._dicts[base] = self._load_wordset(dic)

    def set_enabled(self, bases):
        self.enabled = list(bases)
        self._load_enabled()

    def has_active(self):
        return bool(self._dicts)

    # -- checking -------------------------------------------------------------
    def check(self, word):
        """True if `word` is spelled correctly per any enabled dictionary (or is
        in the personal list). Empty/no active dictionaries -> treated correct."""
        if not self._dicts:
            return True
        w = word.lower()
        if w in self.personal:
            return True
        if w in self._cache:
            return self._cache[w]
        ok = False
        for d in self._dicts.values():
            try:
                if isinstance(d, set):
                    if w in d:
                        ok = True
                        break
                elif d.lookup(word):
                    ok = True
                    break
            except Exception:
                pass
        self._cache[w] = ok
        return ok

    def suggest(self, word, limit=7):
        """Correction candidates for `word`, merged across enabled dictionaries."""
        out = []
        for d in self._dicts.values():
            try:
                if isinstance(d, set):
                    cands = difflib.get_close_matches(word.lower(), d, n=limit)
                else:
                    cands = list(d.suggest(word))
            except Exception:
                cands = []
            for s in cands:
                if s not in out:
                    out.append(s)
                if len(out) >= limit:
                    return out
        return out[:limit]

    def add_personal(self, word):
        self.personal.add(word.lower())
        self._cache.pop(word.lower(), None)

    def personal_list(self):
        return sorted(self.personal)
