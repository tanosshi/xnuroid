"""Find code that reads/writes given global addresses (adrp+add/ldr/str patterns) in a thin arm64 Mach-O.

Usage: python macho_xref.py <macho> <targets.txt | 0xaddr ...>
Targets are image offsets (vmaddr as shown by llvm-objdump). Each hit prints the instruction address, kind
(ldr/str/add/...), and the enclosing function from __unwind_info, so the setter of a null global can be found
without disassembling by hand. Also prints ADRP-only hits for page-level matches with a matching low offset.
"""
import struct
import sys
import bisect

from compact_unwind import find_section, parse_unwind_info, functions


def main():
    data = open(sys.argv[1], "rb").read()
    targets, calls = set(), set()

    ptrs = set()
    strpats = []

    def add(tok):
        if tok.startswith("str:"):
            strpats.append(tok[4:].encode())
            return
        if tok.startswith("ptr:"):
            ptrs.add(int(tok[4:], 16))
        elif tok.startswith("call:"):
            calls.add(int(tok[5:], 16))
        else:
            targets.add(int(tok, 16))
    for a in sys.argv[2:]:
        if a.startswith(("0x", "call:", "ptr:", "str:")):
            add(a)
        else:
            for line in open(a):
                line = line.split("#")[0].strip()
                if line:
                    add(line.split()[0])
    call_hits = {c: [] for c in calls}
    text_vm, sec = find_section(data, "__TEXT", "__unwind_info")
    addr, sz, off = sec
    pers, entries, end, lsda = parse_unwind_info(data[off:off + sz])
    starts = [f[0] for f in functions(entries, end)]
    # code sections
    ncmds = struct.unpack_from("<I", data, 16)[0]
    pos, code = 32, []
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", data, pos)
        if cmd == 0x19:
            n = struct.unpack_from("<I", data, pos + 64)[0]
            for s in range(n):
                o = pos + 72 + 80 * s
                a, ssz, soff = struct.unpack_from("<QQI", data, o + 32)
                flags = struct.unpack_from("<I", data, o + 64)[0]
                nm = data[o:o + 16].rstrip(b"\0").decode()
                if (flags & 0x80000000) and nm in ("__text", "il2cpp"):
                    code.append((a, ssz, soff, nm))
        pos += size
    if strpats:
        # find literal strings in __cstring (and neighbours); a reference may point at a merged suffix, so
        # register the match address and the start of the enclosing C string.
        ncm = struct.unpack_from("<I", data, 16)[0]
        p3 = 32
        while ncm:
            cmd, size = struct.unpack_from("<II", data, p3)
            if cmd == 0x19:
                n = struct.unpack_from("<I", data, p3 + 64)[0]
                for s_i in range(n):
                    o = p3 + 72 + 80 * s_i
                    nm = data[o:o + 16].rstrip(b"\0").decode()
                    if nm in ("__cstring", "__objc_methname", "__ustring"):
                        a_, sz_, off_ = struct.unpack_from("<QQI", data, o + 32)
                        blob = data[off_:off_ + sz_]
                        for pat in strpats:
                            i = blob.find(pat)
                            while i >= 0:
                                start = blob.rfind(b"\0", 0, i) + 1
                                print(f"string {pat!r} at image+{a_ + i:#x} (c-string starts image+{a_ + start:#x}): {blob[start:blob.find(b'\0', i)][:100]!r}")
                                targets.add(a_ + i)
                                targets.add(a_ + start)
                                i = blob.find(pat, i + 1)
            p3 += size
            ncm -= 1
    hits = {}
    pages = {}
    for t in targets:
        pages.setdefault(t & ~0xFFF, []).append(t)
    hits = {t: [] for t in targets}
    for a, ssz, soff, nm in code:
        words = struct.unpack_from(f"<{ssz // 4}I", data, soff)
        live = {}  # reg -> (page, idx)
        for i, w in enumerate(words):
            va = a + 4 * i
            if (w & 0x9F000000) == 0x90000000:
                imm = (((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3)
                if imm & 0x100000:
                    imm -= 0x200000
                page = (va & ~0xFFF) + (imm << 12)
                if page in pages:
                    live[w & 31] = (page, i)
                else:
                    live.pop(w & 31, None)
                continue
            if calls and (w & 0x7C000000) == 0x14000000:  # b / bl
                imm = w & 0x3FFFFFF
                if imm & 0x2000000:
                    imm -= 0x4000000
                if va + imm * 4 in call_hits:
                    call_hits[va + imm * 4].append((va, "bl" if w & 0x80000000 else "b"))
                continue
            # PC-relative forms: ldr xN, literal  /  adr xN, label  (linker-optimised adrp pairs)
            if (w & 0xBB000000) == 0x18000000:
                imm = (w >> 5) & 0x7FFFF
                if imm & 0x40000:
                    imm -= 0x80000
                if va + imm * 4 in hits:
                    hits[va + imm * 4].append((va, "ldr-lit"))
                continue
            if (w & 0x9F000000) == 0x10000000:
                imm = (((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3)
                if imm & 0x100000:
                    imm -= 0x200000
                if va + imm in hits:
                    hits[va + imm].append((va, "adr"))
                continue
            if not live:
                continue
            rn = (w >> 5) & 31
            if rn not in live or i - live[rn][1] > 40:
                continue
            page = live[rn][0]
            kind = tgt = None
            if (w & 0xFF800000) == 0x91000000:  # add
                kind, tgt = "add", page + ((w >> 10) & 0xFFF)
            elif (w & 0x3B000000) == 0x39000000:  # ldr/str unsigned-offset (all sizes)
                size = (w >> 30) & 3
                opc = (w >> 22) & 3
                kind = "ldr" if opc & 1 else "str"
                tgt = page + (((w >> 10) & 0xFFF) << size)
            if kind is None:
                # ldp/stp (signed offset): xx101 0 010 L imm7 Rt2 Rn Rt ; opc in bits 31:30
                if (w & 0x3FC00000) in (0x29400000, 0x29000000):
                    scale = 8 if (w >> 31) & 1 else 4
                    imm7 = (w >> 15) & 0x7F
                    if imm7 & 0x40:
                        imm7 -= 0x80
                    kind = "ldp" if (w >> 22) & 1 else "stp"
                    for extra in (0, scale):
                        t2 = page + imm7 * scale + extra
                        if t2 in hits:
                            hits[t2].append((va, kind))
                continue
            if tgt in hits:
                hits[tgt].append((va, kind))
    if ptrs:  # 8-byte pointers to a function/data address stored anywhere in the file (vtables, init tables)
        ncm = struct.unpack_from("<I", data, 16)[0]
        p2 = 32
        for _ in range(ncm):
            cmd, size = struct.unpack_from("<II", data, p2)
            if cmd == 0x19:
                segname = data[p2 + 8:p2 + 24].rstrip(b"\0").decode()
                vm, vmsz, fo, fsz = struct.unpack_from("<QQQQ", data, p2 + 24)
                if segname != "__TEXT" or True:
                    for k in range(0, fsz - 7, 8):
                        v = struct.unpack_from("<Q", data, fo + k)[0]
                        if v in ptrs:
                            print(f"ptr to image+{v:#x} stored at image+{vm + k:#x} ({segname})")
            p2 += size
    for c in sorted(calls):
        print(f"\n=== calls to image+{c:#x}: {len(call_hits[c])}")
        for va, kind in call_hits[c][:60]:
            k = bisect.bisect_right(starts, va) - 1
            print(f"  {kind:2s} at image+{va:#x}   in function image+{starts[k] if k >= 0 else 0:#x}")
    for t in sorted(targets):
        print(f"\n=== image+{t:#x}: {len(hits[t])} references")
        for va, kind in hits[t][:60]:
            k = bisect.bisect_right(starts, va) - 1
            fn = starts[k] if k >= 0 else 0
            print(f"  {kind:3s} at image+{va:#x}   in function image+{fn:#x}")


if __name__ == "__main__":
    main()
