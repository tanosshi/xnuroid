"""Static call-graph peek at Objective-C methods of a thin arm64 Mach-O (classic dyld info, not chained fixups).

Usage: python macho_objc.py <macho> [class_regex] [depth]
  class_regex  default: ^Unity(?!PLCrash)   (matched against ObjC class names)
  depth        default: 2  (0 = methods only; 1 = also summarise direct callees inside the image)

For every method of every matching class (and its metaclass) prints the image offset of the IMP, then
what the code references up to the first `ret`: bl targets (imports named via the lazy-bind stream),
C strings / CFStrings loaded via adrp+add, and selectors loaded via adrp+ldr.
It is a heuristic linear scan (no control-flow graph) meant to identify stripped functions by their strings.
"""
import re
import struct
import sys
from collections import OrderedDict


class Image:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        d = self.d
        ncmds = struct.unpack_from("<I", d, 16)[0]
        pos = 32
        self.segs, self.secs = [], {}
        self.lazy = (0, 0)
        self.bind = (0, 0)
        for _ in range(ncmds):
            cmd, size = struct.unpack_from("<II", d, pos)
            if cmd == 0x19:
                segname = d[pos + 8:pos + 24].rstrip(b"\0").decode()
                vm, vmsz, fo, fsz = struct.unpack_from("<QQQQ", d, pos + 24)
                self.segs.append((vm, vmsz, fo, fsz, segname))
                n = struct.unpack_from("<I", d, pos + 64)[0]
                for s in range(n):
                    o = pos + 72 + 80 * s
                    nm = d[o:o + 16].rstrip(b"\0").decode()
                    addr, sz, off = struct.unpack_from("<QQI", d, o + 32)
                    self.secs[nm] = (addr, sz, off)
                    if nm == "__text":
                        self.text_vm = addr
            elif cmd in (0x22, 0x80000022):
                self.bind = struct.unpack_from("<II", d, pos + 16)
                self.lazy = struct.unpack_from("<II", d, pos + 32)
            pos += size
        self.base = min(s[0] for s in self.segs if s[4] == "__TEXT")
        self._lazy_syms = None

    def off(self, va):
        for vm, vmsz, fo, fsz, _ in self.segs:
            if vm <= va < vm + vmsz and va - vm < fsz:
                return fo + va - vm
        return None

    def u64(self, va):
        o = self.off(va)
        return struct.unpack_from("<Q", self.d, o)[0] if o is not None else None

    def u32(self, va):
        o = self.off(va)
        return struct.unpack_from("<I", self.d, o)[0] if o is not None else None

    def cstr(self, va, maxlen=120):
        o = self.off(va)
        if o is None:
            return None
        e = self.d.find(b"\0", o, o + maxlen)
        if e < 0:
            e = o + maxlen
        return self.d[o:e].decode("utf-8", "replace")

    @staticmethod
    def ptr(v):
        return None if v is None else v & 0x0000007FFFFFFFFF

    def in_sec(self, name, va):
        s = self.secs.get(name)
        return bool(s) and s[0] <= va < s[0] + s[1]

    def lazy_symbols(self):
        """lazy pointer slot va -> symbol name, from the lazy bind stream."""
        if self._lazy_syms is not None:
            return self._lazy_syms
        out = {}
        off, size = self.lazy
        d, p, end = self.d, self.lazy[0], self.lazy[0] + self.lazy[1]
        seg_vm, sym, a = 0, "", 0

        def uleb(p):
            r = s = 0
            while True:
                b = d[p]; p += 1
                r |= (b & 0x7F) << s
                if not b & 0x80:
                    return r, p
                s += 7
        while size and p < end:
            b = d[p]; p += 1
            op, imm = b & 0xF0, b & 0x0F
            if op == 0x70:
                seg_vm = self.segs[imm][0]
                v, p = uleb(p); a = seg_vm + v
            elif op == 0x40:
                e = d.index(b"\0", p); sym = d[p:e].decode(); p = e + 1
            elif op == 0x90:
                out[a] = sym; a += 8
            elif op == 0x20:
                _, p = uleb(p)
            elif op == 0x60:
                while d[p] & 0x80: p += 1
                p += 1
            elif op in (0x10, 0x30, 0x50, 0x00):
                pass
            elif op == 0x80:
                v, p = uleb(p); a += v
        self._lazy_syms = out
        return out

    def stub_name(self, va):
        """Name of the import behind a __stubs entry at va (adrp x16; ldr x16,[x16,#o]; br x16)."""
        if not self.in_sec("__stubs", va):
            return None
        i0, i1 = self.u32(va), self.u32(va + 4)
        if i0 is None or (i0 & 0x9F00001F) != 0x90000010:
            return None
        immlo, immhi = (i0 >> 29) & 3, (i0 >> 5) & 0x7FFFF
        imm = (((immhi << 2) | immlo) ^ 0x100000) - 0x100000
        page = (va & ~0xFFF) + (imm << 12)
        off12 = ((i1 >> 10) & 0xFFF) << 3
        return self.lazy_symbols().get(page + off12)


def read_classes(img):
    sec = img.secs.get("__objc_classlist")
    out = []
    if not sec:
        return out
    addr, sz, _ = sec
    for i in range(sz // 8):
        c = Image.ptr(img.u64(addr + 8 * i))
        if c:
            out.append(c)
    return out


def methods_of(img, cls_va):
    data = Image.ptr(img.u64(cls_va + 32))
    if not data:
        return None, []
    ro = data & ~7
    name = img.cstr(Image.ptr(img.u64(ro + 24)) or 0)
    ml = Image.ptr(img.u64(ro + 32))
    res = []
    if ml:
        ent = img.u32(ml)
        cnt = img.u32(ml + 4)
        esz, rel = ent & 0xFFFF, bool(ent & 0x80000000)
        for i in range(cnt):
            e = ml + 8 + i * (12 if rel else 24)
            if rel:
                def r32(x):
                    v = img.u32(x)
                    return v - (1 << 32) if v & 0x80000000 else v
                sel_ref = e + r32(e)
                sel = img.cstr(Image.ptr(img.u64(sel_ref)) or 0)
                imp = (e + 8) + r32(e + 8)
            else:
                sel = img.cstr(Image.ptr(img.u64(e)) or 0)
                imp = Image.ptr(img.u64(e + 16))
            res.append((sel, imp))
    return name, res


def scan(img, start, maxins=1500):
    """Linear scan from `start`; stops at a ret / tail call once no forward branch targets remain."""
    bls, strs, sels = OrderedDict(), OrderedDict(), OrderedDict()
    adrp = {}
    va = start
    maxt = start

    def add_str(t):
        if img.in_sec("__cstring", t):
            strs[img.cstr(t)] = 1
        elif img.in_sec("__cfstring", t):
            p = Image.ptr(img.u64(t + 16))
            if p: strs["@\"" + (img.cstr(p) or "?") + "\""] = 1

    def sx(v, bits):
        return v - (1 << bits) if v & (1 << (bits - 1)) else v

    for _ in range(maxins):
        w = img.u32(va)
        if w is None:
            break
        if (w & 0x9F000000) == 0x90000000:  # adrp
            imm = sx((((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3), 21)
            adrp[w & 31] = (va & ~0xFFF) + (imm << 12)
        elif (w & 0x9F000000) == 0x10000000:  # adr
            imm = sx((((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3), 21)
            adrp[w & 31] = va + imm
            add_str(va + imm)
        elif (w & 0xFF800000) == 0x91000000:  # add xd, xn, #imm
            rn, rd = (w >> 5) & 31, w & 31
            if rn in adrp:
                t = adrp[rn] + ((w >> 10) & 0xFFF)
                adrp[rd] = t
                add_str(t)
        elif (w & 0xFFC00000) == 0xF9400000:  # ldr xt, [xn, #imm]
            rn, rt = (w >> 5) & 31, w & 31
            if rn in adrp:
                t = adrp[rn] + (((w >> 10) & 0xFFF) << 3)
                if img.in_sec("__objc_selrefs", t):
                    sels[img.cstr(Image.ptr(img.u64(t)) or 0)] = 1
            adrp.pop(rt, None)
        elif (w & 0xFC000000) == 0x94000000:  # bl
            t = va + sx(w & 0x3FFFFFF, 26) * 4
            bls[t] = bls.get(t, 0) + 1
        elif (w & 0xFC000000) == 0x14000000:  # b: forward jump inside the function, or a tail call
            t = va + sx(w & 0x3FFFFFF, 26) * 4
            if va < t < va + 0x8000 and t <= maxt + 0x8000 and not img.in_sec("__stubs", t) and t < maxt + 0x8000:
                maxt = max(maxt, t)
            else:
                bls[t] = bls.get(t, 0) + 1
                if va >= maxt:
                    break
        elif (w & 0xFF000010) == 0x54000000:  # b.cond
            t = va + sx((w >> 5) & 0x7FFFF, 19) * 4
            maxt = max(maxt, t)
        elif (w & 0x7E000000) == 0x34000000:  # cbz/cbnz
            t = va + sx((w >> 5) & 0x7FFFF, 19) * 4
            maxt = max(maxt, t)
        elif (w & 0x7E000000) == 0x36000000:  # tbz/tbnz
            t = va + sx((w >> 5) & 0x3FFF, 14) * 4
            maxt = max(maxt, t)
        elif w == 0xD65F03C0 and va >= maxt:  # ret with nothing left to cover
            break
        va += 4
    return bls, strs, sels


def describe(img, imp, depth, seen, indent="    "):
    bls, strs, sels = scan(img, imp)
    lines = []
    for s in strs: lines.append(f"{indent}str  {s!r}")
    for s in sels: lines.append(f"{indent}sel  {s}")
    callees = []
    for t, n in bls.items():
        nm = img.stub_name(t)
        if nm:
            if nm not in ("_objc_msgSend", "_objc_retain", "_objc_release", "_objc_autorelease", "_objc_retainAutoreleasedReturnValue",
                          "_objc_storeStrong", "_objc_retainAutorelease", "_objc_autoreleaseReturnValue"):
                lines.append(f"{indent}call {nm}")
        else:
            lines.append(f"{indent}call image+{t - img.base:#x}" + (f" x{n}" if n > 1 else ""))
            callees.append(t)
    if depth > 0:
        for t in callees:
            if t in seen:
                continue
            seen.add(t)
            lines.append(f"{indent}  -> callee image+{t - img.base:#x}")
            lines += describe(img, t, depth - 1, seen, indent + "      ")
    return lines


def funcs_mode():
    """macho_objc.py <macho> --funcs <file-or-0xaddr,...> [depth]: summarise arbitrary functions (image offsets)."""
    img = Image(sys.argv[1])
    spec = sys.argv[3]
    depth = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    addrs = []
    try:
        for line in open(spec):
            t = line.split("#")[0].strip()
            if t:
                addrs.append(int(t.split()[0], 16))
    except OSError:
        addrs = [int(x, 16) for x in spec.split(",")]
    starts = []
    try:
        from compact_unwind import parse_unwind_info, functions
        d = img.d
        ui = img.secs["__unwind_info"]
        pers, entries, end, lsda = parse_unwind_info(d[ui[2]:ui[2] + ui[1]])
        starts = [f[0] for f in functions(entries, end)]
    except Exception as e:  # snapping is a convenience only
        print(f"[no function snapping: {e}]")
    import bisect
    for a in addrs:
        fn = a
        if starts:
            k = bisect.bisect_right(starts, a) - 1
            if k >= 0:
                fn = starts[k]
        note = "" if fn == a else f"  (address image+{a:#x} is inside this function)"
        print(f"\n=== function image+{fn:#x}{note}")
        for ln in describe(img, img.base + fn, depth, set()):
            print(ln)


def main():
    if len(sys.argv) > 2 and sys.argv[2] == "--funcs":
        return funcs_mode()
    img = Image(sys.argv[1])
    rx = re.compile(sys.argv[2] if len(sys.argv) > 2 else r"^Unity(?!PLCrash)")
    depth = int(sys.argv[3]) if len(sys.argv) > 3 else 2
    classes = read_classes(img)
    print(f"__objc_classlist: {len(classes)} classes; imagebase {img.base:#x}")
    for c in classes:
        try:
            name, meths = methods_of(img, c)
            if not name or not rx.search(name):
                continue
            meta = Image.ptr(img.u64(c))
            mname, mmeths = methods_of(img, meta) if meta else (None, [])
        except Exception as e:  # keep going on odd class layouts
            print(f"[skip class {c:#x}: {e}]")
            continue
        print(f"\n=== class {name}: {len(meths)} instance methods, {len(mmeths)} class methods")
        for kind, lst in (("-", meths), ("+", mmeths)):
            for sel, imp in lst:
                if not imp:
                    continue
                print(f"  {kind}[{name} {sel}] image+{imp - img.base:#x}")
                try:
                    for ln in describe(img, imp, depth, set()):
                        print(ln)
                except Exception as e:
                    print(f"    [scan error: {e}]")


if __name__ == "__main__":
    main()
