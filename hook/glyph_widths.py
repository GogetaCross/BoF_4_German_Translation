#!/usr/bin/env python3
"""
Scan tex_dump/*.png (the d3d9 hook's glyph captures) and group them by
their visual pixel width. Intended to find ASCII/symbol slots that share
the same advance-width as the umlauts you want to inject.

For each 32x32 glyph PNG it reports:
  - CRC (from the filename)
  - left edge (first opaque column)
  - right edge (last opaque column)
  - effective width (right - left + 1)
  - opaque pixel count
  - a small ASCII bar showing the glyph's horizontal coverage

Groups with the same "effective width" are the ones that share the same
advance in the game's VWF table (empirically at least).

Usage:
    py d3d9_hook\glyph_widths.py                       # show all widths
    py d3d9_hook\glyph_widths.py 22                    # show only width=22
    py d3d9_hook\glyph_widths.py 18 24                 # show widths in range

Requires Pillow (PIL). Install with: py -m pip install Pillow
"""
import os
import re
import sys
from collections import defaultdict

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow required. Install with: py -m pip install Pillow")

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
GAME_DIR   = os.path.normpath(os.path.join(SCRIPT_DIR, ".."))
DUMP_DIR   = os.path.join(GAME_DIR, "tex_dump")

# Match filenames like: 0042_32x32_A8R8G8B8_a1b2c3d4.png
FNAME_RE = re.compile(
    r"^(\d{4})_(\d+)x(\d+)_[A-Za-z0-9]+_([0-9A-Fa-f]{8})\.png$"
)

ALPHA_THRESHOLD = 16  # pixels with alpha > this count as "opaque"


def analyze_glyph(path):
    """Return (left, right, width, opaque_count, horizontal_mask) or None."""
    img = Image.open(path).convert("RGBA")
    w, h = img.size
    if w != 32 or h != 32:
        return None
    pixels = img.load()

    # Per-column: is there any opaque pixel?
    col_has = [False] * w
    opaque_count = 0
    for y in range(h):
        for x in range(w):
            if pixels[x, y][3] > ALPHA_THRESHOLD:
                col_has[x] = True
                opaque_count += 1
    if opaque_count == 0:
        return None

    left  = next(i for i in range(w) if col_has[i])
    right = next(i for i in range(w - 1, -1, -1) if col_has[i])
    # The advance-width we care about is usually "right + 1" from the
    # glyph's left edge, plus maybe 1 pixel of bearing on the left.
    # We report both "extent" (right-left+1) and "right-edge-from-zero"
    # so you can compare either way.
    extent = right - left + 1
    return left, right, extent, opaque_count, col_has


def main():
    args = sys.argv[1:]
    filter_min = int(args[0]) if len(args) >= 1 else None
    filter_max = int(args[1]) if len(args) >= 2 else filter_min

    if not os.path.isdir(DUMP_DIR):
        sys.exit(f"no tex_dump folder at: {DUMP_DIR}")

    # Dedupe by CRC — same glyph may be dumped many times across runs.
    by_crc = {}
    for fname in sorted(os.listdir(DUMP_DIR)):
        m = FNAME_RE.match(fname)
        if not m:
            continue
        seq, sw, sh, crc = m.groups()
        if int(sw) != 32 or int(sh) != 32:
            continue
        crc = crc.upper()
        if crc in by_crc:
            continue  # keep only the first encounter per CRC
        result = analyze_glyph(os.path.join(DUMP_DIR, fname))
        if result:
            by_crc[crc] = (fname, result)

    if not by_crc:
        print("No 32x32 glyph PNGs found in tex_dump/.")
        return

    # Group by extent (how wide the glyph is, left edge to right edge)
    groups = defaultdict(list)
    # Also group by right-edge (how far the glyph reaches from x=0)
    right_groups = defaultdict(list)
    for crc, (fname, (left, right, extent, n_op, col_has)) in by_crc.items():
        groups[extent].append((crc, left, right, n_op, col_has, fname))
        right_groups[right + 1].append(crc)

    # Print summary histogram
    print("=" * 72)
    print(f"  Width histogram (glyph extent = right - left + 1)")
    print(f"  {len(by_crc)} unique 32x32 glyphs from tex_dump/")
    print("=" * 72)
    for extent in sorted(groups):
        n = len(groups[extent])
        bar = "#" * min(n, 50)
        print(f"  extent={extent:>3} px   {n:>3} glyphs  {bar}")

    print()
    print("=" * 72)
    print("  Per-glyph details")
    print("=" * 72)

    for extent in sorted(groups):
        if filter_min is not None and not (filter_min <= extent <= filter_max):
            continue
        n = len(groups[extent])
        print(f"\n--- extent = {extent} px  ({n} glyphs) ---")
        for crc, left, right, n_op, col_has, fname in sorted(groups[extent]):
            # Render a tiny visual bar: X marks opaque columns, . empty
            bar = "".join("X" if c else "." for c in col_has)
            print(f"  {crc}  left={left:>2} right={right:>2} "
                  f"ext={extent:>2} opaque={n_op:>3}  "
                  f"[{bar}]  {fname}")

    # Print right-edge alternative (sometimes more useful for fixed-leftmost
    # glyphs like how the game probably positions each 32x32 tile)
    print()
    print("=" * 72)
    print("  Right-edge groups (how far right the glyph extends)")
    print("=" * 72)
    for r in sorted(right_groups):
        n = len(right_groups[r])
        print(f"  right_edge={r:>3}   {n:>3} glyphs  "
              f"CRCs: {' '.join(right_groups[r][:10])}"
              f"{'...' if n > 10 else ''}")


if __name__ == "__main__":
    main()
