// ff_overlay.h — tiny top-right "fast-forward speed" indicator (e.g. "> 4x").
// Self-contained D3D9 overlay (GDI text -> A8R8G8B8 texture -> DrawPrimitiveUP),
// independent of the subtitle overlay so the two never fight over one texture.
//
// The fast-forward hotkey thread (bof4_hooks.cpp) calls ff_overlay_set() on every
// speed change. The Present hooks (hooks.cpp) call ff_overlay_on_present() each
// frame; it draws while the speed is >1x, plus a brief flash at 1x so the user
// sees it drop back to normal. hook_Reset() must call ff_overlay_on_lost_device()
// to drop the D3DPOOL_DEFAULT texture before a device Reset.
#pragma once

struct IDirect3DDevice9;
struct IDirect3DSwapChain9;

// Record the current speed multiplier (1.0 = normal). >1 keeps the indicator
// visible; any change also triggers a short on-screen flash.
void ff_overlay_set(float scale);

// Draw the indicator (once per frame) if currently visible. Cheap no-op when
// the speed is 1x and no flash is active.
void ff_overlay_on_present(IDirect3DDevice9* dev, IDirect3DSwapChain9* sc);

// Release the D3DPOOL_DEFAULT texture before a device Reset.
void ff_overlay_on_lost_device();
