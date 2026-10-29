"""List LC_SYMTAB symbols and ObjC class/method names from a thin arm64 Mach-O.

Usage: python macho_symbols.py <macho> [regex]    (default regex: Unity|Player|Graphics|Metal|Engine)
Prints: symtab counts, matching symbols with image offsets, and matching ObjC method-name strings.
"""
import re
import struct
import sys


def main():
    data = open(sys.argv[1], "rb").read()
    rx = re.compile(sys.argv[2] if len(sys.argv) > 2 else r"Unity|Player|Graphics|Metal|Engine")
    ncmds = struct.unpack_from("<I", data, 16)[0]
    pos, symoff, nsyms, stroff, strsize, secs = 32, 0, 0, 0, 0, []
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", data, pos)
        if cmd == 0x2:
            symoff, nsyms, stroff, strsize = struct.unpack_from("<IIII", data, pos + 8)
        if cmd == 0x19:
            n = struct.unpack_from("<I", data, pos + 64)[0]
            for s in range(n):
                o = pos + 72 + 80 * s
                name = data[o:o + 16].rstrip(b"\0").decode()
                seg = data[o + 16:o + 32].rstrip(b"\0").decode()
                addr, sz, off = struct.unpack_from("<QQI", data, o + 32)
                secs.append((seg, name, addr, sz, off))
        pos += size
    print(f"LC_SYMTAB: {nsyms} symbols, strtab {strsize} bytes")
    hits = 0
    for i in range(nsyms):
        n_strx, n_type, n_sect, n_desc, n_value = struct.unpack_from("<IBBHQ", data, symoff + 16 * i)
        if n_type & 0xE0:  # debug stab
            continue
        end = data.index(b"\0", stroff + n_strx)
        name = data[stroff + n_strx:end].decode("utf-8", "replace")
        if rx.search(name):
            hits += 1
            kind = "ext" if n_type & 1 else "loc"
            print(f"  {kind} {name} 0x{n_value:x}" + ("" if n_type & 0xE else " (undefined)"))
    print(f"matching symbols: {hits}")
    for seg, name, addr, sz, off in secs:
        if name in ("__objc_methname", "__objc_classname"):
            blob = data[off:off + sz].split(b"\0")
            m = [b.decode("utf-8", "replace") for b in blob if b and rx.search(b.decode("utf-8", "replace"))]
            print(f"{seg},{name}: {len(blob)} strings, {len(m)} match")
            for x in m[:200]:
                print("  ", x)


main()
