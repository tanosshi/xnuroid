"""List dyld bind records that carry a non-zero addend (BIND_OPCODE_SET_ADDEND_SLEB).

Usage: python bind_addends.py <UnityFramework> [vmaddr ...]
The loader's bind-stream interpreter ignores SLEB addends; this shows what that drops.
"""
import struct
import sys


def uleb(b, p):
    r = s = 0
    while True:
        x = b[p]; p += 1
        r |= (x & 0x7F) << s
        if not x & 0x80:
            return r, p
        s += 7


def sleb(b, p):
    r = s = 0
    while True:
        x = b[p]; p += 1
        r |= (x & 0x7F) << s
        s += 7
        if not x & 0x80:
            if x & 0x40:
                r -= 1 << s
            return r, p


def main():
    data = open(sys.argv[1], "rb").read()
    watch = {int(a, 0) for a in sys.argv[2:] if not a.startswith("sym:")}
    watch_syms = {a[4:] for a in sys.argv[2:] if a.startswith("sym:")}
    sym_hits = {}
    ncmds = struct.unpack_from("<I", data, 16)[0]
    pos, segs, bind_off, bind_size, lazy_off, lazy_size = 32, [], 0, 0, 0, 0
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", data, pos)
        if cmd == 0x19:
            vm = struct.unpack_from("<Q", data, pos + 24)[0]
            segs.append(vm)
        elif cmd in (0x22, 0x80000022):
            bind_off, bind_size = struct.unpack_from("<II", data, pos + 16)
            lazy_off, lazy_size = struct.unpack_from("<II", data, pos + 32)
        pos += size

    total = with_addend = 0
    by_symbol = {}
    for name, off, size in (("regular", bind_off, bind_size), ("lazy", lazy_off, lazy_size)):
        b = data[off:off + size]
        p = 0
        seg = soff = 0
        sym, addend = "", 0
        while p < len(b):
            byte = b[p]; p += 1
            op, imm = byte & 0xF0, byte & 0x0F

            def hit(loc):
                nonlocal total, with_addend
                total += 1
                if addend:
                    with_addend += 1
                    by_symbol.setdefault(sym, set()).add(addend)
                if loc in watch:
                    print(f"  {name}: vmaddr 0x{loc:x} <- {sym} + {addend}")
                if sym in watch_syms:
                    sym_hits.setdefault(sym, []).append((loc, addend))

            if op == 0x00:
                if name == "regular":
                    break
                seg = soff = 0; addend = 0
            elif op == 0x20:
                _, p = uleb(b, p)
            elif op == 0x40:
                e = b.index(0, p); sym = b[p:e].decode(); p = e + 1
            elif op == 0x60:
                addend, p = sleb(b, p)
            elif op == 0x70:
                seg = imm; soff, p = uleb(b, p)
            elif op == 0x80:
                v, p = uleb(b, p); soff += v
            elif op == 0x90:
                hit(segs[seg] + soff); soff += 8
            elif op == 0xA0:
                hit(segs[seg] + soff); v, p = uleb(b, p); soff += 8 + v
            elif op == 0xB0:
                hit(segs[seg] + soff); soff += 8 + imm * 8
            elif op == 0xC0:
                cnt, p = uleb(b, p); skip, p = uleb(b, p)
                for _ in range(cnt):
                    hit(segs[seg] + soff); soff += 8 + skip
    for s, hits in sym_hits.items():
        print(f"  {s}: {len(hits)} binds, first 6: " + ", ".join(f"0x{l:x}(+{a})" for l, a in hits[:6]))
    print(f"binds: {total}, with non-zero addend: {with_addend}")
    for s, a in sorted(by_symbol.items()):
        print(f"  {s}: addends {sorted(a)}")


if __name__ == "__main__":
    main()
