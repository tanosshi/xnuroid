"""Decode __unwind_info from a thin arm64 Mach-O; reference for the loader's FDE converter.

Usage:
  python compact_unwind.py <macho>                       summary + CFI-shape histogram
  python compact_unwind.py <macho> --dump                one line per function
  python compact_unwind.py <macho> --compare <txt>       diff against `llvm-objdump --unwind-info <macho> > txt`
  python compact_unwind.py <macho> --func 0x1234 ...     show decoded CFI for functions (image offsets)

Offsets are image-relative (vmaddr - __TEXT vmaddr), as in llvm-objdump.
"""
import re
import struct
import sys

FRAMELESS, DWARF, FRAME = 0x02000000, 0x03000000, 0x04000000
PAIRS = [(0x1, "x19", 19, "x20", 20), (0x2, "x21", 21, "x22", 22), (0x4, "x23", 23, "x24", 24),
         (0x8, "x25", 25, "x26", 26), (0x10, "x27", 27, "x28", 28),
         (0x100, "d8", 72, "d9", 73), (0x200, "d10", 74, "d11", 75),
         (0x400, "d12", 76, "d13", 77), (0x800, "d14", 78, "d15", 79)]


def find_section(data, segname, sectname):
    ncmds = struct.unpack_from("<I", data, 16)[0]
    pos, text_vm = 32, None
    found = None
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", data, pos)
        if cmd == 0x19:
            seg = data[pos + 8:pos + 24].rstrip(b"\0").decode()
            vmaddr = struct.unpack_from("<Q", data, pos + 24)[0]
            if seg == "__TEXT":
                text_vm = vmaddr
            nsects = struct.unpack_from("<I", data, pos + 64)[0]
            for s in range(nsects):
                o = pos + 72 + 80 * s
                sn = data[o:o + 16].rstrip(b"\0").decode()
                sg = data[o + 16:o + 32].rstrip(b"\0").decode()
                addr, sz, off = struct.unpack_from("<QQI", data, o + 32)
                if (sg, sn) == (segname, sectname):
                    found = (addr, sz, off)
        pos += size
    return text_vm, found


def parse_unwind_info(ui):
    """-> (personality_got_offsets, entries[(func_off, enc)], func_end_of_last, lsda[(func_off, lsda_off)])"""
    ver, ce_off, ce_cnt, p_off, p_cnt, i_off, i_cnt, = struct.unpack_from("<7I", ui, 0)
    assert ver == 1, ver
    common = struct.unpack_from(f"<{ce_cnt}I", ui, ce_off)
    pers = list(struct.unpack_from(f"<{p_cnt}I", ui, p_off))
    idx = [struct.unpack_from("<III", ui, i_off + 12 * i) for i in range(i_cnt)]
    entries, lsda = [], []
    for k, (fo, sl, lo) in enumerate(idx[:-1]):
        kind = struct.unpack_from("<I", ui, sl)[0]
        if kind == 2:  # regular
            eoff, ecnt = struct.unpack_from("<HH", ui, sl + 4)
            for j in range(ecnt):
                f, e = struct.unpack_from("<II", ui, sl + eoff + 8 * j)
                entries.append((f, e))
        elif kind == 3:  # compressed
            eoff, ecnt, coff, ccnt = struct.unpack_from("<HHHH", ui, sl + 4)
            local = struct.unpack_from(f"<{ccnt}I", ui, sl + coff)
            for j in range(ecnt):
                v = struct.unpack_from("<I", ui, sl + eoff + 4 * j)[0]
                n = v >> 24
                entries.append((fo + (v & 0xFFFFFF), common[n] if n < ce_cnt else local[n - ce_cnt]))
        else:
            raise ValueError(f"unknown second-level kind {kind}")
    lo0, lo1 = idx[0][2], idx[-1][2]
    for j in range((lo1 - lo0) // 8):
        lsda.append(struct.unpack_from("<II", ui, lo0 + 8 * j))
    return pers, entries, idx[-1][0], lsda


def cfi_for(enc):
    """Return a tuple describing the single CFI row valid at call sites, or None if not synthesizable."""
    mode = enc & 0x0F000000
    if enc == 0:
        return None
    if mode == FRAMELESS:
        return ("frameless", ((enc & 0x00FFF000) >> 12) * 16)
    if mode == FRAME:
        saves = []  # (dwarf reg, cfa offset)
        off = -24
        for bit, n1, r1, n2, r2 in PAIRS:
            if enc & bit:
                saves += [(n1, r1, off), (n2, r2, off - 8)]
                off -= 16
        return ("frame", saves)
    if mode == DWARF:
        return ("dwarf", enc & 0x00FFFFFF)
    return ("unsupported", enc)


def functions(entries, end):
    """Yield (start, end, enc, personality_index, has_lsda)."""
    for i, (f, e) in enumerate(entries):
        nxt = entries[i + 1][0] if i + 1 < len(entries) else end
        yield f, nxt, e, (e >> 28) & 3, bool(e & 0x40000000)


def describe(enc):
    c = cfi_for(enc)
    if c is None:
        return "no-unwind"
    if c[0] == "frameless":
        return f"CFA=sp+{c[1]}; lr stays in x30"
    if c[0] == "frame":
        s = " ".join(f"{n}@CFA{o}" for n, _, o in c[1])
        return f"CFA=x29+16; x29@CFA-16 x30@CFA-8" + (" " + s if s else "")
    return f"{c[0]} {c[1]:#x}"


def main():
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(1)
    data = open(sys.argv[1], "rb").read()
    text_vm, sec = find_section(data, "__TEXT", "__unwind_info")
    addr, sz, off = sec
    pers, entries, end, lsda = parse_unwind_info(data[off:off + sz])
    funcs = list(functions(entries, end))
    args = sys.argv[2:]
    if "--dump" in args:
        for s, e, enc, p, l in funcs:
            print(f"{s:#010x}-{e:#010x} enc={enc:#010x} pers={p} lsda={int(l)}  {describe(enc)}")
    if "--func" in args:
        want = {int(a, 0) for a in args[args.index("--func") + 1:]}
        for s, e, enc, p, l in funcs:
            if s in want:
                print(f"{s:#010x}-{e:#010x} enc={enc:#010x} pers={p} lsda={int(l)}  {describe(enc)}")
    if "--compare" in args:
        txt = open(args[args.index("--compare") + 1], encoding="utf-8", errors="replace").read()
        ref = [(int(a, 16), int(b, 16)) for a, b in
               re.findall(r"function offset=(0x[0-9a-fA-F]+), encoding(?:\[\d+\])?=(0x[0-9a-fA-F]+)", txt)]
        ref_lsda = [(int(a, 16), int(b, 16)) for a, b in
                    re.findall(r"function offset=(0x[0-9a-fA-F]+), LSDA offset=(0x[0-9a-fA-F]+)", txt)]
        ref_pers = [int(a, 16) for a in re.findall(r"personality\[\d+\]: .*?(0x[0-9a-fA-F]+)", txt, re.I)]
        if not ref:
            print("COMPARE FAILED: no entries recognised in the llvm-objdump text (format differs?)"); sys.exit(3)
        bad = 0
        if ref != entries:
            bad += 1
            print(f"MISMATCH entries: mine={len(entries)} objdump={len(ref)}")
            for i, (a, b) in enumerate(zip(entries, ref)):
                if a != b:
                    print(f"  first diff at #{i}: mine=({a[0]:#x},{a[1]:#x}) objdump=({b[0]:#x},{b[1]:#x})"); break
        if ref_lsda != lsda:
            bad += 1
            print(f"MISMATCH lsda: mine={len(lsda)} objdump={len(ref_lsda)}")
        if ref_pers and ref_pers != pers:
            print(f"NOTE personality text differs: mine={[hex(p) for p in pers]} objdump={[hex(p) for p in ref_pers]} (check by eye)")
        print("COMPARE OK" if not bad else "COMPARE FAILED",
              f"({len(entries)} entries, {len(lsda)} lsda, {len(pers)} personalities)")
        sys.exit(1 if bad else 0)
    if not any(a in args for a in ("--dump", "--func", "--compare")):
        from collections import Counter
        modes = Counter(("none" if e == 0 else {FRAMELESS: "frameless", DWARF: "dwarf", FRAME: "frame"}.get(e & 0x0F000000, "other"))
                        for _, _, e, _, _ in funcs)
        shapes = Counter(describe(e) for _, _, e, _, _ in funcs)
        print(f"__unwind_info: {len(funcs)} functions, {len(lsda)} LSDA, personality GOT offsets {[hex(p) for p in pers]}")
        print("  modes:", dict(modes))
        print("  with personality:", sum(1 for f in funcs if f[3]), " with lsda flag:", sum(1 for f in funcs if f[4]))
        print("  distinct CFI shapes:", len(shapes))
        for k, v in shapes.most_common(8):
            print(f"    {v:6d}  {k}")


if __name__ == "__main__":
    main()
