# BoF4 D3D9 Texture Hook — Dump & Inject

A custom `d3d9.dll` proxy that sits between the GoG BoF4 DirectDraw wrapper
and the real system `d3d9.dll`. It:

1. **Dumps** every texture upload to a PNG in `tex_dump/` (with alpha)
2. **Injects** replacement textures from `tex_inject/<crc>.png` — the
   game sees its own upload, but what actually reaches the GPU is your
   modified pixels

Purpose: find the dialog font atlas that BOF4.exe uploads to the GPU,
then modify it (add umlauts, etc.) without touching any DAT files.

All other font-location strategies (decompressing INIT.DAT entry 0 and
guessing formats) failed, but the game MUST upload the font as a D3D
texture at runtime — so we capture it at that point.

## Requirements

- CMake 3.15+ (you have 4.2.1 — fine)
- Visual Studio 2019 or 2022 with the **x86 (Win32)** C++ toolset
  - This is the C++ desktop development workload
- `d3d9.h` headers from the Windows SDK (installed with VS)

**Important**: BOF4.exe is 32-bit. The DLL *must* be built 32-bit.

## Build

Open a Developer Command Prompt for VS or set PATH so `cmake` and
`cl.exe` are available. Then:

```bat
cd "D:\SteamLibrary\steamapps\common\Breath of Fire IV\d3d9_hook"

rem Configure for 32-bit (Win32)
cmake -S . -B build -A Win32

rem Build Release
cmake --build build --config Release
```

The DLL lands at `build\bin\Release\d3d9.dll`.

## Install

Copy `build\bin\Release\d3d9.dll` into the game folder (next to `BOF4.exe`):

```bat
copy /Y build\bin\Release\d3d9.dll "D:\SteamLibrary\steamapps\common\Breath of Fire IV\"
```

## Run

1. Launch the game normally.
2. Play until dialog text appears on screen.
3. Quit the game.
4. Look in `D:\SteamLibrary\steamapps\common\Breath of Fire IV\tex_dump\`
   for the dumped BMPs, and `d3d9_hook.log` for the hook trace.

## What to look for

The font atlas will be one of the dumped PNGs. It should look like:
- Small to medium size (256x256, 512x256, etc.)
- A grid of glyphs (letters, numbers, punctuation) on a transparent/black
  background
- Likely `A8R8G8B8` or `A8` format

Thumbnail-view the `tex_dump/` folder in File Explorer — the font atlas
will be visually distinctive.

Dumped filenames look like: `0042_256x128_A8R8G8B8_a1b2c3d4.png`
- `0042` — sequential upload counter
- `256x128` — texture dimensions
- `A8R8G8B8` — source D3D format
- `a1b2c3d4` — CRC32 hash of the RGBA8 pixels (this is what injection
  matches on)

## Injecting modified textures

1. Find the font atlas dump in `tex_dump/`, e.g. `0042_256x128_A8R8G8B8_a1b2c3d4.png`
2. Edit it in any image editor (Photoshop, GIMP, Aseprite, paint.net) —
   draw your umlaut glyphs, modify text, whatever
3. Save it as `tex_inject\a1b2c3d4.png` — the filename is just the
   `CRC32` part of the dump, nothing else. Width/height/format must
   match the original.
4. Run the game. On each upload the hook compares the CRC to files in
   `tex_inject/`. If it matches, your pixels replace the original's
   in-place before the GPU sees them.
5. Confirm in `d3d9_hook.log` — it will say `INJECTED tex_inject\XXX.png`
   whenever an override fires.

**Important**: The CRC must match the original upload's RGBA8 pixels
(not the file bytes of your edit). The CRC shown in the dump filename
is already the right one — just use that hex prefix as the inject
filename.

**Size mismatch**: if your edited PNG has different dimensions than the
original texture, the hook skips it with a log message. Resize it to
match.

**Supported D3D formats for injection**: A8R8G8B8, X8R8G8B8, R8G8B8,
R5G6B5, X1R5G5B5, A1R5G5B5, A4R4G4B4, X4R4G4B4, A8, L8, A8L8. DXT
compressed textures are dumped but not injected (yet).

## Uninstall

Delete `d3d9.dll` from the game folder. The GoG wrapper will use the
real system `d3d9.dll` directly.

## How it works

1. The game folder is first in the DLL search path. When the GoG
   `ddraw.dll` wrapper calls `LoadLibrary("d3d9.dll")` (or has d3d9 in
   its import table), Windows loads **our** `d3d9.dll` instead of
   System32's.
2. On `DLL_PROCESS_ATTACH` we `LoadLibrary` the real `d3d9.dll` from
   `C:\Windows\System32\` and grab `Direct3DCreate9`.
3. Our exported `Direct3DCreate9` forwards to the real one, then
   patches the returned `IDirect3D9` vtable at slot 16 to redirect
   `CreateDevice` to our hook.
4. In our `CreateDevice` hook we patch slot 23 of the returned
   `IDirect3DDevice9` vtable to redirect `CreateTexture`.
5. In our `CreateTexture` hook we patch slots 19 and 20 of the returned
   `IDirect3DTexture9` vtable to redirect `LockRect` and `UnlockRect`.
6. When `LockRect` is called on a new texture we record the locked
   pointer. When `UnlockRect` is called we read the pixel data (before
   calling the real unlock, which might invalidate the pointer),
   convert it to 32-bit BGRA, and write it to `tex_dump/*.bmp`.

Each vtable is shared across all COM instances of the same class, so
we only patch each one once.

## Troubleshooting

**Game won't start / crashes immediately**
- Check `d3d9_hook.log` — if it says "could not resolve real
  Direct3DCreate9", your System32 d3d9.dll is missing or our
  `LoadLibrary` path is wrong.
- Verify the DLL is 32-bit: `dumpbin /headers d3d9.dll | findstr machine`
  should say `14C machine (x86)`.
- If you built 64-bit by accident, CMake should have errored out — but
  double-check with `-A Win32`.

**No BMPs appear in tex_dump/**
- Check `d3d9_hook.log` to see if `Direct3DCreate9` was called. If yes,
  check that `CreateDevice`, `CreateTexture`, and `LockRect`/`UnlockRect`
  hooks all fired.
- If `CreateTexture` never fires, the game might be using
  `CreateOffscreenPlainSurface` or `CreateRenderTarget` directly — add
  those hooks in `hooks.cpp`.

**d3d9_hook.log doesn't get created**
- Your game folder might not be the current working directory. Try
  adding `GetModuleFileName` + path-relative logging in `dllmain.cpp`.
