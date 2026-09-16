# Building

## 1. The display hook (`d3d9.dll`)

**Requirements:** Visual Studio (MSVC, "Desktop development with C++") and CMake ≥ 3.15.

`BOF4.exe` is 32-bit, so the DLL **must** be built as **Win32 (x86)**:

```sh
cmake -S hook -B hook/build -A Win32
cmake --build hook/build --config Release
```

Output: `hook/build/bin/Release/d3d9.dll`. Copy it next to `BOF4.exe`
(GOG: the game root; Steam: `…/4249150_BreathofFire4/english/`).

On first launch the hook writes a documented `_d3d9_hook_config.txt` next to the
game exe. The in-game settings overlay opens with the top-left `^ / ~` key.

Vendored dependencies are already in-tree (`hook/src/imgui`, `hook/src/minhook`) —
no extra fetch needed.

## 2. The localization tools (Python)

Plain Python 3 (3.11+), no build step:

```sh
py tools/localization_editor.py     # PO-Edit-style translation GUI
py tools/extract_text.py --help     # dump text from DAT containers
py tools/repack_text.py  --help     # inject translations back into DATs
```

The tools read/write the DAT containers of your own game copy. `config/vwf_config.txt`
is the single source of truth for the umlaut byte↔glyph mapping used by `repack_text.py`.

## 3. The end-user installer (Inno Setup)

The installer ships **only diffs** — never whole game DATs. Building it needs a
**payload assembled from your own game** (not included here, for copyright reasons):

1. Build the md5 text patch: `py installer/build_patch.py` → `payload/patch_de.bof4`
   (diffs your English `DAT_backup/` vs. your translated `DAT/`).
2. Generate the `bsdiff4` diffs for the store DATs + DEMO2 title variants into
   `installer/inno/derived/` (from your pristine originals).
3. Drop the official **Python embeddable** (python.org) into `installer/inno/pyembed/`
   and copy the `bsdiff4` package beside it.
4. Place `d3d9.dll` + the sidecar `.txt` + subtitle into `installer/inno/files/`.
5. Compile: `"C:\Program Files\Inno Setup 7\ISCC.exe" installer/inno/BoF4_DE.iss`
   → `installer/inno/dist/BoF4_DE_Installieren.exe`.

Inno Setup was chosen over PyInstaller because its bootloader is trusted by
Windows Defender (PyInstaller one-file exes trip the "Wacatac" ML false positive);
it runs the official embedded Python on `apply_inno.py`, so there is no
self-extracting executable to flag.
