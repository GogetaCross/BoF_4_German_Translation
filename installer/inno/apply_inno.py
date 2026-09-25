r"""
apply_inno.py -- non-interactive applier, driven by the Inno Setup front-end.

    python apply_inno.py "<game_root>" us|jp

Runs under the bundled official Python embeddable (no PyInstaller), so it does
not trip the Wacatac AV heuristic. Ships ONLY diffs + our own files (no whole
game DATs):
  * text/graphics  -> compact md5 sub-file patch (payload/patch_de.bof4)
  * store DATs + the chosen DEMO2 title -> bsdiff4 diffs from the pristine
    originals (derived/*.bsdiff), applied from DAT_backup so re-runs and
    US<->JP switches always start from clean bytes
  * d3d9.dll + sidecars + subtitle + config -> copied from files/
"""
import sys, os, shutil, re, hashlib, platform, traceback, datetime

BASE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(BASE, "runtime"))   # bof4lib, apply_patch
sys.path.insert(0, os.path.join(BASE, "pyembed"))   # bundled bsdiff4
import bsdiff4                                        # noqa: E402
from apply_patch import load_patch, apply as applica_patch   # noqa: E402


LOG_NAME = "BoF4_DE_Installation.log"


class _Tee:
    """Mirror print() into the log file; the Inno front-end hides the console."""
    def __init__(self, *streams):
        self.streams = [st for st in streams if st is not None]

    def write(self, s):
        for st in self.streams:
            try:
                st.write(s); st.flush()
            except Exception:                                   # noqa: BLE001
                pass

    def flush(self):
        pass


def _open_log(root):
    """Log next to the game; fall back to %TEMP% if the game folder is not writable."""
    for d in (root, os.environ.get("TEMP") or os.getcwd()):
        try:
            return open(os.path.join(d, LOG_NAME), "w", encoding="utf-8")
        except OSError:
            continue
    return None


def _md5(path):
    try:
        h = hashlib.md5()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        return "%s (%d B)" % (h.hexdigest(), os.path.getsize(path))
    except OSError as e:
        return "nicht lesbar: %s" % e


def _log_env(root, dat):
    print("Zeit       : %s" % datetime.datetime.now().isoformat(timespec="seconds"))
    print("Argumente  : %s" % sys.argv[1:])
    print("Python     : %s / Windows %s" % (platform.python_version(), platform.version()))
    print("Spielordner: %s" % root)
    print("DAT-Ordner : %s" % dat)
    try:
        print("Frei       : %d MB" % (shutil.disk_usage(root).free // (1 << 20)))
    except OSError as e:
        print("Frei       : ? (%s)" % e)
    exe = os.path.join(os.path.dirname(dat) if dat else root, "BOF4.exe")
    print("BOF4.exe   : %s" % (_md5(exe) if os.path.exists(exe) else "nicht gefunden"))
    print("DAT_backup : %s" % ("vorhanden" if dat and os.path.isdir(
        os.path.join(os.path.dirname(dat), "DAT_backup")) else "neu"))


def _dat_dir(root):
    for sub in (os.path.join("english", "DAT"), "DAT"):
        d = os.path.join(root, sub)
        if os.path.isdir(d) and any(f.upper().endswith(".DAT") for f in os.listdir(d)):
            return d
    return None


def _set_jp_title(cfg_path, jp):
    val = "true" if jp else "false"
    if not os.path.exists(cfg_path):
        shutil.copy2(os.path.join(BASE, "files", "_d3d9_hook_config.txt"), cfg_path)
    lines = open(cfg_path, encoding="utf-8", errors="ignore").read().splitlines()
    hit = False
    for i, l in enumerate(lines):
        if l.lstrip().startswith("jp_title") and "=" in l and not l.lstrip().startswith("#"):
            lines[i] = re.sub(r"(jp_title\s*=\s*)\S+", r"\g<1>" + val, l, count=1)
            hit = True
            break
    if not hit:
        lines.append("jp_title                = %s" % val)
    open(cfg_path, "w", encoding="utf-8").write("\n".join(lines) + "\n")


def main():
    if len(sys.argv) < 2:
        print("Usage: apply_inno.py <game_root> [us|jp]"); return 2
    root = sys.argv[1]
    jp = (len(sys.argv) > 2 and sys.argv[2].lower() == "jp")

    dat = _dat_dir(root)
    _log_env(root, dat)
    if not dat:
        print("FEHLER: Kein DAT-Ordner im Spielverzeichnis gefunden."); return 1
    exe_dir = os.path.dirname(dat)

    # 1. Full DAT backup (once) -> the pristine source for the bsdiff bases.
    bak = os.path.join(exe_dir, "DAT_backup")
    if not os.path.isdir(bak):
        print('Sichere den DAT-Ordner ("DAT_backup") ...')
        shutil.copytree(dat, bak)

    # 2. bsdiff the derived files from the PRISTINE backup -> live DAT/.
    derived = os.path.join(BASE, "derived")
    jobs = [("AREAD068.DAT", "AREAD068.bsdiff"),
            ("AREAS052.DAT", "AREAS052.bsdiff"),
            ("DEMO2.DAT", "DEMO2_JP.bsdiff" if jp else "DEMO2_US.bsdiff")]
    for name, patch in jobs:
        src = os.path.join(bak, name)
        if not os.path.exists(src):
            print("  !! %s fehlt im Backup - uebersprungen." % name); continue
        print("  bsdiff %s <- %s" % (name, _md5(src)))
        try:
            bsdiff4.file_patch(src, os.path.join(dat, name), os.path.join(derived, patch))
        except Exception as e:                                   # noqa: BLE001
            print("  !! %s: %s (Spieldateien nicht im Originalzustand?)" % (name, e))
            return 1

    # 3. Compact md5 text/graphics patch (skips the now-German DEMO2 + absent store).
    print("Spiele Uebersetzung ein ...")
    tab = load_patch(os.path.join(BASE, "payload", "patch_de.bof4"))
    _scritti, _sub, _gia, errori = applica_patch(dat, tab, root, os.path.join(root, "_backup_de"))
    if errori:
        print("PROBLEME (%d):" % len(errori))
        for e in errori:
            print("  !! " + e)
        return 1

    # 4. Our own files (hook + sidecars + subtitle).
    files = os.path.join(BASE, "files")
    for f in ("d3d9.dll", "rect_tuner.txt", "rect_widths.txt", "vwf_config.txt"):
        shutil.copy2(os.path.join(files, f), os.path.join(exe_dir, f))
    os.makedirs(os.path.join(exe_dir, "MOV"), exist_ok=True)
    shutil.copy2(os.path.join(files, "MOV", "ZBOF4.srt"), os.path.join(exe_dir, "MOV", "ZBOF4.srt"))

    # 5. Title-screen config (preserve an existing config; only touch jp_title).
    _set_jp_title(os.path.join(exe_dir, "_d3d9_hook_config.txt"), jp)

    print("FERTIG")
    return 0


if __name__ == "__main__":
    log = _open_log(sys.argv[1] if len(sys.argv) > 1 else os.getcwd())
    sys.stdout = sys.stderr = _Tee(sys.__stdout__, log)
    try:
        rc = main()
    except Exception:                                            # noqa: BLE001
        print("FEHLER (unerwartet):")
        print(traceback.format_exc()); rc = 1
    print("Rueckgabe  : %d" % rc)
    if log:
        log.close()
    sys.exit(rc)
