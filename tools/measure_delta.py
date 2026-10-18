#!/usr/bin/env python3
"""Sizes a game-start snapshot stored whole and as a delta against the
baseline (#46, step 1), from the two files LOONY_MEASURE_DELTA writes
(src/measure.h): each a list of resident guest pages, an 8-byte
little-endian offset and the page's bytes. zstd must be on the PATH.

    tools/measure_delta.py <folder> [<other baseline.pages>]

Prints the resident set at the game's start, and the pages that differ from
the baseline (resident at the start and not at the baseline, or different),
counted in host pages and in 4 KB pieces, each raw and compressed at zstd's
levels 3 and 19; the delta also XORed with the baseline, where the baseline
has the page. With a second baseline, says whether the two are the same.
"""
import os
import subprocess
import sys

PAGE = 16384
PIECE = 4096
REGIONS = (('low memory', 0, 0x00100000), ('image', 0x00100000, 0x01000000),  # src/guest_mem.h
           ('heap', 0x01000000, 0x06000000), ('stack', 0x06000000, 0x07000000))


def load(path):
    data = open(path, 'rb').read()
    pages, i = {}, 0
    while i < len(data):
        off = int.from_bytes(data[i:i + 8], 'little')
        pages[off] = data[i + 8:i + 8 + PAGE]
        i += 8 + PAGE
    return pages


def zstd(blob, level):
    return len(subprocess.run(['zstd', '-q', '-c', f'-{level}', '-T1'], input=blob,
                              capture_output=True, check=True).stdout)


def pieces(pages):
    return {off + k: page[k:k + PIECE] for off, page in pages.items()
            for k in range(0, PAGE, PIECE)}


def report(name, items, base=None):
    """items: offset -> bytes. With base, also XORed against it."""
    offs = sorted(items)
    raw = b''.join(o.to_bytes(8, 'little') + items[o] for o in offs)
    line = f'{name}: {len(offs)} x {len(items[offs[0]]) if offs else 0} B = ' \
           f'{len(raw) - 8 * len(offs):,} B; zstd -3 {zstd(raw, 3):,} B, -19 {zstd(raw, 19):,} B'
    if base is not None:
        def xor(o):
            b = base.get(o)
            n = len(items[o])
            return items[o] if b is None else \
                (int.from_bytes(items[o], 'big') ^ int.from_bytes(b, 'big')).to_bytes(n, 'big')
        x = b''.join(o.to_bytes(8, 'little') + xor(o) for o in offs)
        line += f'; XORed with the baseline, zstd -3 {zstd(x, 3):,} B, -19 {zstd(x, 19):,} B'
    print(line)


def main():
    folder = sys.argv[1]
    base = load(os.path.join(folder, 'baseline.pages'))
    start = load(os.path.join(folder, 'start.pages'))
    if len(sys.argv) > 2:
        other = load(sys.argv[2])
        same = [o for o in base if other.get(o) == base[o]]
        print(f'baselines: {len(base)} and {len(other)} pages, {len(same)} the same, '
              f'{len(set(base) ^ set(other))} resident in only one')
    gone = [o for o in base if o not in start]
    print(f'baseline: {len(base)} pages resident; {len(gone)} of them not resident at the start')
    report('baseline, whole', base)
    report('start, whole', start)
    report('start, differing host pages',
           {o: p for o, p in start.items() if base.get(o, bytes(PAGE)) != p}, base)
    differ = [o for o, p in start.items() if base.get(o, bytes(PAGE)) != p]
    print('differing host pages by region: ' + ', '.join(
        f'{name} {sum(lo <= o < hi for o in differ)} of {sum(lo <= o < hi for o in start)}'
        for name, lo, hi in REGIONS))
    bp, sp = pieces(base), pieces(start)
    report('start, differing 4 KB pieces',
           {o: p for o, p in sp.items() if bp.get(o, bytes(PIECE)) != p}, bp)


main()
