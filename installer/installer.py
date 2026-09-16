r"""
Installer der deutschen Uebersetzung von Breath of Fire IV (PC).

Tut nur eines: spielt die deutsche Uebersetzung ein. Kein Zustand, keine
Deinstallation -- um zu Englisch zurueckzukehren, das Spiel neu laden.

Hinweis fuer den, der baut: kein dem Nutzer gezeigter Text nennt Python oder die
Quelldateien. Verteilt wird nur die ausfuehrbare Datei.
"""

import io
import os
import re
import shutil
import sys
import time

# --- Pfade: funktioniert sowohl im PyInstaller-Bundle als auch aus dem Quellbaum ---
if getattr(sys, 'frozen', False):
    BUNDLE = sys._MEIPASS                                  # entpacktes Bundle
    HERE = os.path.dirname(sys.executable)
else:
    BUNDLE = os.path.dirname(os.path.abspath(__file__))
    HERE = BUNDLE

sys.path.insert(0, os.path.join(BUNDLE, 'runtime'))
from apply_patch import load_patch, apply as applica_patch      # noqa: E402

PAYLOAD = os.path.join(BUNDLE, 'payload')
PATCH_FILE = os.path.join(PAYLOAD, 'patch_de.bof4')
ASSETS = os.path.join(BUNDLE, 'assets')          # DLL + sidecars + DEMO2 + config template
APP_ID = '4249150_BreathofFire4'
GIOCO = 'Breath of Fire IV'


# ----------------------------------------------- Anzeige-Fix (DLL) + Titelbildschirm
def _copy_asset(name, dst):
    """Kopiert eine gebuendelte Datei nach dst (Ordner werden angelegt)."""
    d = os.path.dirname(dst)
    if d:
        os.makedirs(d, exist_ok=True)
    shutil.copy2(os.path.join(ASSETS, name), dst)


def _set_jp_title(cfg_path, jp):
    """Setzt jp_title in der Konfig. Vorhandene Konfig: nur die Zeile aendern
    (Nutzer-Einstellungen bleiben). Fehlt sie: Vorlage einspielen und setzen."""
    val = 'true' if jp else 'false'
    if not os.path.exists(cfg_path):
        _copy_asset('_d3d9_hook_config.txt', cfg_path)
    try:
        lines = open(cfg_path, encoding='utf-8', errors='ignore').read().splitlines()
    except OSError:
        return
    hit = False
    for i, l in enumerate(lines):
        s = l.lstrip()
        if s.startswith('jp_title') and '=' in s and not s.startswith('#'):
            lines[i] = re.sub(r'(jp_title\s*=\s*)\S+', r'\g<1>' + val, l, count=1)
            hit = True
            break
    if not hit:
        lines.append('jp_title                = %s' % val)
    open(cfg_path, 'w', encoding='utf-8').write('\n'.join(lines) + '\n')


def chiedi_titolo():
    """True = japanischer Titelbildschirm, False = US."""
    print('\nWelchen Titlescreen moechtest du verwenden?')
    print('  [1] Amerikanischen (US)   (Standard)')
    print('  [2] Japanischen')
    while True:
        c = input('Auswahl [1/2]: ').strip()
        if c in ('', '1'):
            return False
        if c == '2':
            return True
        print('  Bitte 1 oder 2 eingeben.')


def installa_hook(exe_dir, dat, jp):
    """Kopiert den Anzeige-Fix (d3d9.dll) + die Lokalisierungs-Dateien, den
    passenden Titelbildschirm (DEMO2.DAT) und die Untertitel; setzt jp_title."""
    _copy_asset('d3d9.dll', os.path.join(exe_dir, 'd3d9.dll'))
    for f in ('rect_tuner.txt', 'rect_widths.txt', 'vwf_config.txt'):
        _copy_asset(f, os.path.join(exe_dir, f))
    _copy_asset('ZBOF4.srt', os.path.join(exe_dir, 'MOV', 'ZBOF4.srt'))
    _copy_asset('DEMO2_JP.DAT' if jp else 'DEMO2_US.DAT', os.path.join(dat, 'DEMO2.DAT'))
    # German Dengeki-Store area DATs (text + Manillo portraits). Shipped full-file
    # because the added portrait entries change the sub-file count, so the md5
    # text-patch skips them.
    for f in ('AREAD068.DAT', 'AREAS052.DAT'):
        _copy_asset(f, os.path.join(dat, f))
    _set_jp_title(os.path.join(exe_dir, '_d3d9_hook_config.txt'), jp)


# --------------------------------------------------------- Spiel automatisch finden
def _steam_libraries():
    """Liest libraryfolders.vdf: Steam selbst sagt, wo seine Bibliotheken liegen."""
    libs = []
    for base in (os.environ.get('ProgramFiles(x86)'), os.environ.get('ProgramFiles'),
                 r'C:\Program Files (x86)'):
        if not base:
            continue
        vdf = os.path.join(base, 'Steam', 'steamapps', 'libraryfolders.vdf')
        if os.path.exists(vdf):
            try:
                txt = open(vdf, encoding='utf-8', errors='ignore').read()
            except OSError:
                continue
            for m in re.finditer(r'"path"\s+"([^"]+)"', txt):
                libs.append(m.group(1).replace('\\\\', '\\'))
            break
    return libs


def _candidati():
    """Alle plausiblen Spielordner (Steam + GOG-Heuristik ueber alle Laufwerke)."""
    visti = []

    def agg(p):
        if p and os.path.isdir(p) and p not in visti:
            visti.append(p)

    for lib in _steam_libraries():
        agg(os.path.join(lib, 'steamapps', 'common', APP_ID))
    for d in 'CDEFGHIJKL':
        agg(r'%s:\Steam\steamapps\common\%s' % (d, APP_ID))
        agg(r'%s:\SteamLibrary\steamapps\common\%s' % (d, APP_ID))
        agg(r'%s:\GOG Games\%s' % (d, GIOCO))
        agg(r'%s:\GOG\%s' % (d, GIOCO))
        agg(r'%s:\Games\%s' % (d, GIOCO))
        agg(r'%s:\%s' % (d, GIOCO))
    return visti


def _dat_dir(game_root):
    """Findet den DAT-Ordner: Steam nutzt english\\DAT, GOG oft nur DAT."""
    for sub in (os.path.join('english', 'DAT'), 'DAT'):
        d = os.path.join(game_root, sub)
        if os.path.isdir(d) and any(f.upper().endswith('.DAT') for f in os.listdir(d)):
            return d
    return None


def trova_gioco():
    for g in _candidati():
        d = _dat_dir(g)
        if d:
            return g, d
    return None, None


def chiedi_percorso():
    print('\nSpiel nicht automatisch gefunden.')
    print('  Bei Steam: Rechtsklick aufs Spiel > Verwalten > Lokale Dateien,')
    print('  dann den Pfad aus der Adresszeile kopieren.\n')
    while True:
        p = input('Spielordner (leer = abbrechen): ').strip().strip('"')
        if not p:
            return None, None
        d = _dat_dir(p)
        if d:
            return p, d
        print('  Dort finde ich keinen "DAT"-Ordner. Nochmal.')


# ---------------------------------------------------------------------- Ablauf
def main():
    print('=' * 62)
    print('   BREATH OF FIRE IV  -  Deutsche Uebersetzung')
    print('=' * 62)

    if not os.path.exists(PATCH_FILE):
        print('FEHLER: Patch-Daten fehlen (%s).' % PATCH_FILE)
        return 1

    print('\nSuche das Spiel...')
    forced = os.environ.get('BOF4_GAME')          # Vorrang: erzwungener Pfad
    if forced:
        game, dat = forced, _dat_dir(forced)
        if not dat:
            print('BOF4_GAME zeigt auf keinen gueltigen Spielordner: %s' % forced)
            return 1
    else:
        game, dat = trova_gioco()
        if not game:
            game, dat = chiedi_percorso()
    if not game:
        print('Abgebrochen: Spiel nicht gefunden.')
        return 1

    n_dat = sum(1 for f in os.listdir(dat) if f.upper().endswith('.DAT'))
    print('Gefunden:\n  %s\n  (%d Datendateien)' % (game, n_dat))
    print('\nDie deutsche Uebersetzung legt zuerst eine vollstaendige Kopie deines')
    print('DAT-Ordners an ("DAT_backup"). Um spaeter zum Original zurueckzukehren:')
    print('entweder das Spiel bei Steam/GOG ueberpruefen lassen, ODER den Ordner')
    print('"DAT" loeschen und "DAT_backup" wieder in "DAT" umbenennen.')
    if input('\nFortfahren? [j/N] ').strip().lower() not in ('j', 'ja', 's', 'y'):
        print('Abgebrochen.')
        return 0

    jp = chiedi_titolo()          # Titelbildschirm-Wahl vor dem Einspielen abfragen

    print('\n' + '=' * 62)
    print('EINSPIELEN DER UEBERSETZUNG')
    print('(bitte warten, dieses Fenster nicht schliessen...)')
    t0 = time.time()

    # Vollstaendiges Backup des DAT-Ordners (einmalig, VOR jeder Aenderung), damit
    # der Nutzer per Umbenennen zum Original zurueckkehren kann.
    exe_dir = os.path.dirname(dat)                  # Ordner mit BOF4.exe
    dat_backup = os.path.join(exe_dir, 'DAT_backup')
    if not os.path.isdir(dat_backup):
        print('\nErstelle Backup des DAT-Ordners ("DAT_backup") - kann kurz dauern...')
        try:
            shutil.copytree(dat, dat_backup)
        except Exception as e:                      # noqa: BLE001
            print('  !! Backup fehlgeschlagen: %s' % e)
            print('  Abgebrochen - es wurde nichts veraendert.')
            return 1
    else:
        print('\n("DAT_backup" ist bereits vorhanden und wird beibehalten.)')

    backup_root = os.path.join(game, '_backup_de')
    tab = load_patch(PATCH_FILE)
    scritti, sub_tot, gia, errori = applica_patch(dat, tab, game, backup_root)

    print('')
    if scritti:
        print('  %d Dateien aktualisiert, %d Bloecke ersetzt.' % (scritti, sub_tot))
    else:
        print('  Nichts zu tun: Spiel war bereits uebersetzt.')
    if gia:
        print('  (%d Bloecke waren schon auf Deutsch)' % gia)

    if errori:
        print('\nPROBLEME (%d):' % len(errori))
        for e in errori[:15]:
            print('  !! ' + e)
        print('\nZum Zuruecksetzen: Steam > Rechtsklick > Eigenschaften >')
        print('Installierte Dateien > Integritaet der Dateien ueberpruefen.')
        return 1

    # Anzeige-Fix (d3d9.dll) + Lokalisierungs-Dateien + Titelbildschirm + Untertitel.
    print('\nInstalliere Anzeige-Fix, Untertitel und Titelbildschirm...')
    try:
        installa_hook(exe_dir, dat, jp)
        print('  Titelbildschirm: %s' % ('Japanisch' if jp else 'US'))
    except Exception as e:                  # noqa: BLE001
        print('  !! Konnte Anzeige-Dateien nicht kopieren: %s' % e)
        return 1

    print('\n   Uebersetzung installiert in %.0f Sekunden.' % (time.time() - t0))
    print('   Gute Reise nach Ladon.')
    print('\nRueckkehr zum Original: den Ordner "DAT" loeschen und "DAT_backup"')
    print('wieder in "DAT" umbenennen - oder bei Steam/GOG die Spieldateien')
    print('ueberpruefen lassen (holt die englische Fassung zurueck).')
    print('=' * 62)
    return 0


if __name__ == '__main__':
    try:
        rc = main()
    except KeyboardInterrupt:
        print('\nUnterbrochen.')
        rc = 1
    except Exception as e:            # noqa: BLE001
        print('\nFEHLER: %s' % e)
        rc = 1
    try:
        input('\nEnter zum Schliessen.')
    except EOFError:
        pass
    sys.exit(rc)
