#!/usr/bin/env python3
"""Lists a PEF program's imported symbols, sorted, one per line, to diff a
new classic game's OS calls against a supported one's:

  imports.py "/Applications/<game folder>/<program>" > new.txt

Mach-O games need LOONY_TRACE=imports instead."""
import struct
import sys

data = open(sys.argv[1], "rb").read()
if data[:8] != b"Joy!peff":
    sys.exit(f"{sys.argv[1]} isn't a PEF program")
loader = None
for i in range(struct.unpack(">H", data[32:34])[0]):
    h = data[40 + 28 * i : 68 + 28 * i]
    size, offset, kind = struct.unpack(">I", h[16:20])[0], struct.unpack(">I", h[20:24])[0], h[24]
    if kind == 4:
        loader = data[offset : offset + size]
if loader is None:
    sys.exit("no loader section")
nlibs, nsyms = struct.unpack(">II", loader[24:32])
strings = struct.unpack(">I", loader[40:44])[0]
names = []
for j in range(nsyms):
    at = strings + (struct.unpack(">I", loader[56 + 24 * nlibs + 4 * j : 60 + 24 * nlibs + 4 * j])[0] & 0xFFFFFF)
    names.append(loader[at : loader.index(b"\0", at)].decode("mac_roman"))
print("\n".join(sorted(names)))
