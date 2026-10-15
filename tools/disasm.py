#!/usr/bin/env python3
"""Disassembles a game's PowerPC code, by function name, for reverse
engineering (a development tool; the app doesn't use it).

The Classic games' PEF code keeps a traceback table after each function,
with its C++ name and its length, so functions can be found by name and
calls can be labelled. Addresses are code offsets, as the shim prints them
(code+0x11e90).

  tools/disasm.py <program> --list [text]      the functions, or those whose name has text
  tools/disasm.py <program> <name>|<offset>... disassemble functions

A name matches exactly, or as the start of a mangled name (Event_StartGame
finds Event_StartGame__Fv); an offset (0x15584) picks the function holding
it. Needs Capstone: pip install -r tools/requirements-dev.txt
"""
import re
import struct
import sys

try:
    import capstone
except ImportError:
    sys.exit("disasm.py needs Capstone: pip install -r tools/requirements-dev.txt")


def code_section(path):
    """The PEF container's first code section, as bytes."""
    data = open(path, "rb").read()
    if data[:8] != b"Joy!peff":
        sys.exit(f"{path} isn't a PEF program")
    nsections = struct.unpack(">H", data[32:34])[0]
    for i in range(nsections):
        h = data[40 + 28 * i : 68 + 28 * i]
        _, _, _, _, raw_size, offset, kind = struct.unpack(">iIIIIIB", h[:25])
        if kind == 0:
            return data[offset : offset + raw_size]
    sys.exit(f"{path} has no code section")


def functions(code):
    """(start, end, name) for each function with a named traceback table,
    in order. A traceback table starts with a zero word after the code, and
    its optional fields come in a fixed order (the AIX layout)."""
    found = []
    for pos in range(0, len(code) - 12, 4):
        if code[pos : pos + 4] != b"\0\0\0\0":
            continue
        t = pos + 4
        version, _, f1, f2, _, _, fixedparms, f5 = code[t : t + 8]
        if version != 0 or not f2 & 0x40 or not f1 & 0x20:  # a name and tb_offset
            continue
        t += 8
        if fixedparms or f5 >> 1:
            t += 4  # parminfo
        tb_offset = struct.unpack(">I", code[t : t + 4])[0]
        t += 4
        if f2 & 0x80:
            t += 4  # hand_mask
        if f1 & 0x08:  # ctl_info
            t += 4 + 4 * struct.unpack(">I", code[t : t + 4])[0]
        if t + 2 > len(code):
            continue
        n = struct.unpack(">H", code[t : t + 2])[0]
        name = code[t + 2 : t + 2 + n]
        if not 0 < n < 256 or not re.fullmatch(rb"[\x21-\x7e]+", name) or tb_offset > pos:
            continue
        found.append((pos - tb_offset, pos, name.decode().lstrip(".")))
    return found


def find(funcs, what):
    hex_offset = what[5:] if what.startswith("code+") else what
    if hex_offset.lower().startswith("0x"):
        offset = int(hex_offset, 16)
        return [f for f in funcs if f[0] <= offset < f[1]]
    exact = [f for f in funcs if f[2] == what]
    return exact or [f for f in funcs if f[2].startswith(what + "__")]


def disassemble(code, funcs, func):
    start, end, name = func
    names = {f[0]: f[2] for f in funcs}
    md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
    print(f"{name}  code+0x{start:05x}..0x{end:05x}")
    for insn in md.disasm(code[start:end], start):
        note = ""
        if insn.mnemonic in ("bl", "b") and insn.op_str.startswith("0x"):
            target = int(insn.op_str, 16)
            if target in names:
                note = f"  ; {names[target]}"
        word = struct.unpack(">I", insn.bytes)[0]
        print(f"  {insn.address:05x}: {word:08x}  {insn.mnemonic:8} {insn.op_str}{note}")
    print()


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    code = code_section(argv[1])
    funcs = functions(code)
    if argv[2] == "--list":
        text = argv[3] if len(argv) > 3 else ""
        for start, end, name in funcs:
            if text.lower() in name.lower():
                print(f"code+0x{start:05x}  {end - start:6}  {name}")
        return
    for what in argv[2:]:
        matches = find(funcs, what)
        if not matches:
            sys.exit(f"no function {what}")
        for f in matches:
            disassemble(code, funcs, f)


if __name__ == "__main__":
    main(sys.argv)
