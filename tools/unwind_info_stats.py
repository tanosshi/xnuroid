"""Summarise unwind data in a thin arm64 Mach-O: __eh_frame, __unwind_info encodings, LSDAs.

Usage: python unwind_info_stats.py <UnityFramework>
"""
import collections
import struct
import sys


def sections(data):
    ncmds = struct.unpack_from("<I", data, 16)[0]
    pos, out = 32, {}
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", data, pos)
        if cmd == 0x19:
            nsects = struct.unpack_from("<I", data, pos + 64)[0]
            sp = pos + 72
            for _ in range(nsects):
                name = data[sp:sp + 16].rstrip(b"\0").decode()
                seg = data[sp + 16:sp + 32].rstrip(b"\0").decode()
                addr, sz, off = struct.unpack_from("<QQI", data, sp + 32)
                out[name] = (seg, addr, sz, off)
                sp += 80
        pos += size
    return out


def main():
    data = open(sys.argv[1], "rb").read()
    secs = sections(data)
    for n in ("__text", "__eh_frame", "__unwind_info", "__gcc_except_tab"):
        seg, addr, sz, off = secs[n]
        print(f"{seg},{n}: vmaddr=0x{addr:x} size=0x{sz:x} fileoff=0x{off:x}")

    seg, addr, sz, off = secs["__eh_frame"]
    eh = data[off:off + sz]
    print(f"\n__eh_frame records ({sz} bytes):")
    p = 0
    while p + 4 <= len(eh):
        length = struct.unpack_from("<I", eh, p)[0]
        if length == 0:
            print(f"  +0x{p:x}: terminator")
            break
        cie_id = struct.unpack_from("<I", eh, p + 4)[0]
        kind = "CIE" if cie_id == 0 else "FDE"
        extra = ""
        if kind == "FDE":
            rel, rng = struct.unpack_from("<iI", eh, p + 8)
            extra = f" pc_begin(rel)=0x{rel & 0xffffffff:x} range=0x{rng:x}"
        print(f"  +0x{p:x}: {kind} length=0x{length:x}{extra}")
        p += 4 + length

    seg, uaddr, usz, uoff = secs["__unwind_info"]
    ui = data[uoff:uoff + usz]
    (ver, ce_off, ce_cnt, pers_off, pers_cnt, idx_off, idx_cnt) = struct.unpack_from("<7I", ui, 0)
    print(f"\n__unwind_info: version={ver} common_encodings={ce_cnt} personalities={pers_cnt} first_level_entries={idx_cnt}")
    common = list(struct.unpack_from(f"<{ce_cnt}I", ui, ce_off))
    pers = list(struct.unpack_from(f"<{pers_cnt}i", ui, pers_off))
    print("  personality GOT slot offsets (relative to image base):", [hex(x) for x in pers])

    modes = collections.Counter()
    with_lsda = with_pers = funcs = 0
    dwarf_fde_offsets = set()
    names = {0: "NONE", 2: "FRAMELESS", 3: "DWARF", 4: "FRAME"}
    for i in range(idx_cnt):
        fo, sl, lsda = struct.unpack_from("<III", ui, idx_off + 12 * i)
        if sl == 0:
            continue
        kind = struct.unpack_from("<I", ui, sl)[0]
        if kind == 2:  # regular
            eoff, ecnt = struct.unpack_from("<HH", ui, sl + 4)
            entries = [struct.unpack_from("<II", ui, sl + eoff + 8 * j) for j in range(ecnt)]
            encs = [e for _, e in entries]
        elif kind == 3:  # compressed
            eoff, ecnt, enc_off, enc_cnt = struct.unpack_from("<HHHH", ui, sl + 4)
            local = list(struct.unpack_from(f"<{enc_cnt}I", ui, sl + enc_off))
            encs = []
            for j in range(ecnt):
                v = struct.unpack_from("<I", ui, sl + eoff + 4 * j)[0]
                idx = v >> 24
                encs.append(common[idx] if idx < ce_cnt else local[idx - ce_cnt])
        else:
            continue
        for e in encs:
            funcs += 1
            m = (e >> 24) & 0xF
            modes[names.get(m, f"MODE{m}")] += 1
            if e & 0x30000000:
                with_pers += 1
            if e & 0x40000000:
                with_lsda += 1
            if m == 3:
                dwarf_fde_offsets.add(e & 0x00FFFFFF)
    print(f"  functions with entries: {funcs}")
    for k, v in modes.most_common():
        print(f"  mode {k:9s}: {v}")
    print(f"  with personality: {with_pers}   with LSDA: {with_lsda}")
    print(f"  distinct DWARF FDE offsets referenced from __eh_frame: {len(dwarf_fde_offsets)}")


if __name__ == "__main__":
    main()
