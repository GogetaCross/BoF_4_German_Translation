#!/usr/bin/env python3
"""
Analyze d3d9_hook.log draw entries to infer VWF advance widths for every
glyph CRC the game drew.

Run this after a session where the game rendered the text you want to
measure. It parses lines of the form:

    DRAW crc=XXXXXXXX 32x32 x=123.45 y=456.78

and computes, for each CRC, the statistical distribution of x-differences
to the NEXT draw on the same y-line. That next-draw x-delta is the advance
width the game used to place the following character.

Usage:
    py d3d9_hook/analyze_widths.py                   # analyze d3d9_hook.log
    py d3d9_hook/analyze_widths.py somewhere/other.log
"""
import re
import sys
import os
import collections

DRAW_RE = re.compile(
    r"^DRAW crc=([0-9A-Fa-f]{8}) (\d+)x(\d+) x=([-+]?[\d.]+) y=([-+]?[\d.]+)"
)


def main():
    log_path = sys.argv[1] if len(sys.argv) > 1 else "d3d9_hook.log"
    if not os.path.exists(log_path):
        # Try the game-folder relative path from this script's location
        here = os.path.dirname(os.path.abspath(__file__))
        alt = os.path.normpath(os.path.join(here, "..", "d3d9_hook.log"))
        if os.path.exists(alt):
            log_path = alt
    if not os.path.exists(log_path):
        sys.exit(f"log not found: {log_path}")

    draws = []  # list of (crc, w, h, x, y) in log order
    with open(log_path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            m = DRAW_RE.match(line)
            if m:
                crc, w, h, x, y = m.groups()
                draws.append((crc.upper(), int(w), int(h), float(x), float(y)))

    print(f"Parsed {len(draws)} DRAW lines from {log_path}")
    if not draws:
        return

    # Per-CRC advance widths: for each consecutive pair on the same y-row
    # (where successor_x > current_x), record (successor_x - current_x)
    # as the advance-after-this-glyph.
    advances = collections.defaultdict(list)
    Y_TOL = 2.0  # same-line tolerance in pixels

    for i, (crc, w, h, x, y) in enumerate(draws):
        # Look ahead for the next draw on the same y-line
        for j in range(i + 1, min(i + 8, len(draws))):
            ncrc, nw, nh, nx, ny = draws[j]
            if abs(ny - y) > Y_TOL:
                continue
            dx = nx - x
            if 0 < dx < 64:  # sensible advance range
                advances[crc].append(round(dx, 2))
            break

    # Sort by most frequently drawn glyphs first
    freq = collections.Counter(d[0] for d in draws)
    print(f"\nUnique glyph CRCs drawn: {len(freq)}")

    print(f"\n{'CRC':<10} {'count':>6} {'size':<8} "
          f"{'advance (mode)':>15} {'min':>5} {'max':>5}")
    print("-" * 60)

    # Build a crc -> size map for display
    size_of = {}
    for crc, w, h, _, _ in draws:
        size_of.setdefault(crc, f"{w}x{h}")

    rows = []
    for crc, n in freq.most_common():
        advs = advances.get(crc, [])
        if not advs:
            rows.append((crc, n, size_of.get(crc, "?"), None, None, None, 0))
            continue
        counter = collections.Counter(advs)
        mode_val, _ = counter.most_common(1)[0]
        rows.append((crc, n, size_of.get(crc, "?"),
                     mode_val, min(advs), max(advs), len(advs)))

    # Print in two sections: well-measured first, then unmeasured
    measured = [r for r in rows if r[3] is not None]
    unmeasured = [r for r in rows if r[3] is None]

    for crc, n, sz, mode, mn, mx, nm in measured:
        print(f"{crc:<10} {n:>6} {sz:<8} {mode:>15.2f} {mn:>5.1f} {mx:>5.1f}"
              f"  ({nm} samples)")

    if unmeasured:
        print(f"\n{len(unmeasured)} CRCs have no successor-on-same-line samples "
              f"(always last on their row, or only drawn once)")
        for crc, n, sz, *_ in unmeasured[:20]:
            print(f"  {crc}  count={n}  size={sz}")
        if len(unmeasured) > 20:
            print(f"  ... and {len(unmeasured) - 20} more")


if __name__ == "__main__":
    main()
