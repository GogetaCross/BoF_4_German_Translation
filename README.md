# Breath of Fire IV — German Localization Toolchain

Source code for the German (Deutsch) fan-localization of **Breath of Fire IV**
(PC, GOG / Steam). This repository is **code only** — it lets you build the
runtime display hook, run the translation tools, and compile the installer.

> ⚠️ **No game data is included.** Breath of Fire IV is © Capcom. These tools
> operate on files from your **own legally-owned copy** of the game. The
> translation itself (the DAT patches) is distributed as a separate patch that
> requires the original game; nothing here contains Capcom assets, text, or
> graphics.

## What's inside

| Folder | Contents |
|---|---|
| `hook/` | The `d3d9.dll` proxy hook (C++/CMake): VWF font, menu-box autofit, world-map label centering, uncensor restores, Dengeki-Store unlock, intro subtitles, JP/US title card, in-game ImGui config overlay, fast-forward, and more. Vendors Dear ImGui (MIT) + MinHook (BSD-2). |
| `tools/` | The localization toolchain (Python): `localization_editor.py` (PO-Edit-style GUI), `extract_text.py`, `repack_text.py`, plus helpers. |
| `installer/` | The end-user installer build: Inno Setup script (`inno/BoF4_DE.iss`) + `apply_inno.py`, and the md5 sub-file patch builder (`build_patch.py`). |
| `config/` | Mechanical display config (byte→width font metrics, UI box widths). No prose. |

## Build

See [`docs/BUILD.md`](docs/BUILD.md). In short:

- **DLL:** `cmake -S hook -B hook/build -A Win32` → `cmake --build hook/build --config Release` → `hook/build/bin/Release/d3d9.dll` (must be 32-bit to match `BOF4.exe`).
- **Tools:** plain Python 3 — `py tools/localization_editor.py`.
- **Installer:** assemble the payload from your own game, then compile with Inno Setup 7 (`ISCC.exe installer/inno/BoF4_DE.iss`).

## Credits

Built on the work of the fan community — with thanks to **Twisted Phoenix
Translations** (original PSX translation), and **Navarchos, Ratty & FlamePurge**
(Dengeki-Store restoration and the uncensor groundwork). Breath of Fire IV © Capcom.

## License

Original code: **MIT** (see [`LICENSE`](LICENSE)). Vendored components keep their
own licenses (Dear ImGui — MIT; MinHook — BSD-2).
