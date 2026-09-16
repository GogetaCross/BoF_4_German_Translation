# PyInstaller spec -- baut BoF4_DE_Installieren.exe (onefile, console).
#   pyinstaller installer.spec
#
# Bundelt: installer.py (Einstieg) + runtime\ (bof4lib, apply_patch) als Daten
# + payload\ (patch_de.bof4, manifesto.sha256). runtime\ wird zur Laufzeit ueber
# sys.path aus _MEIPASS geladen (siehe installer.py), daher als DATEN eingebunden.

import os
HERE = os.path.abspath(SPECPATH)

datas = [
    (os.path.join(HERE, 'runtime', 'bof4lib.py'),    'runtime'),
    (os.path.join(HERE, 'runtime', 'apply_patch.py'), 'runtime'),
    (os.path.join(HERE, 'payload', 'patch_de.bof4'), 'payload'),
    (os.path.join(HERE, 'payload', 'manifesto.sha256'), 'payload'),
]
# assets\: d3d9.dll + Lokalisierungs-Dateien + beide DEMO2 + Konfig-Vorlage.
_assets_dir = os.path.join(HERE, 'assets')
for _f in sorted(os.listdir(_assets_dir)):
    datas.append((os.path.join(_assets_dir, _f), 'assets'))

a = Analysis(
    [os.path.join(HERE, 'installer.py')],
    pathex=[os.path.join(HERE, 'runtime')],
    binaries=[],
    datas=datas,
    hiddenimports=['hashlib', 'zlib', 'struct', 'shutil'],
    hookspath=[],
    runtime_hooks=[],
    excludes=['tkinter', 'unittest', 'email', 'http', 'xml', 'pydoc', 'pdb'],
    noarchive=False,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name='BoF4_DE_Installieren',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    console=True,          # zeigt das Textfenster mit den Eingabeaufforderungen
    disable_windowed_traceback=False,
)
# Onefile: a.binaries + a.datas werden oben direkt an EXE uebergeben und es gibt
# KEINEN COLLECT-Schritt -> PyInstaller baut eine einzige .exe.
