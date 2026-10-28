"""Phase 0 check: is the guest arm64 or arm64e, and does it use pointer authentication?

Usage: python scan_pac.py <UnityFramework>
Reports the Mach-O cpusubtype and counts PAC instructions in executable sections.
Exit code 2 if PAC is in use (arm64e subtype or any PAC prologue/epilogue found).
"""
import struct
import sys

PAC_INSNS = {
    0xD503233F: "paciasp",
    0xD503237F: "pacibsp",
    0xD50323BF: "autiasp",
    0xD50323FF: "autibsp",
    0xD65F0BFF: "retaa",
    0xD65F0FFF: "retab",
}
# authenticated branches/calls/loads: braa/brab/blraa/blrab/eretaa, ldraa/ldrab
def is_auth_branch(i):
    return (i & 0xFEFFF800) == 0xD61F0800 or (i & 0xFEFFF800) == 0xD63F0800


def main():
    d = open(sys.argv[1], "rb").read()
    base = 0
    magic = struct.unpack_from(">I", d, 0)[0]
    if magic in (0xCAFEBABE, 0xCAFEBABF):
        n = struct.unpack_from(">I", d, 4)[0]
        for k in range(n):
            ct, cs, off, size, _ = struct.unpack_from(">iiIII", d, 8 + 20 * k)
            print(f"fat slice {k}: cputype=0x{ct & 0xffffffff:x} cpusubtype=0x{cs & 0xffffffff:x}")
            if ct == 0x0100000C:
                base = off
    magic, cputype, cpusub, ftype, ncmds, _, flags, _ = struct.unpack_from("<IiiIIIII", d, base)
    assert magic == 0xFEEDFACF, hex(magic)
    caps, sub = (cpusub >> 24) & 0xFF, cpusub & 0xFFFFFF
    name = {0: "arm64 (all)", 1: "arm64v8", 2: "arm64e"}.get(sub, "unknown")
    print(f"cputype=0x{cputype & 0xffffffff:x} cpusubtype=0x{cpusub & 0xffffffff:x} -> {name}, caps=0x{caps:x}")
    arm64e = sub == 2

    pos = base + 32
    counts, auth_br, scanned = {}, 0, 0
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", d, pos)
        if cmd == 0x19:  # LC_SEGMENT_64
            nsects = struct.unpack_from("<I", d, pos + 64)[0]
            for s in range(nsects):
                o = pos + 72 + 80 * s
                sectname = d[o:o + 16].rstrip(b"\0").decode()
                segname = d[o + 16:o + 32].rstrip(b"\0").decode()
                _, ssize, soff = struct.unpack_from("<QQI", d, o + 32)
                sflags = struct.unpack_from("<I", d, o + 64)[0]
                if not (sflags & 0x80000000 or sflags & 0x400):  # pure/some instructions
                    continue
                words = struct.unpack_from(f"<{ssize // 4}I", d, base + soff)
                scanned += len(words)
                for w in words:
                    if w in PAC_INSNS:
                        counts[PAC_INSNS[w]] = counts.get(PAC_INSNS[w], 0) + 1
                    elif is_auth_branch(w):
                        auth_br += 1
                print(f"scanned {segname},{sectname}: {ssize // 4} words")
        pos += size
    print(f"instruction words scanned: {scanned}")
    for k, v in sorted(counts.items()):
        print(f"  {k}: {v}")
    print(f"  authenticated branches (braa/blraa family): {auth_br}")
    uses_pac = arm64e or bool(counts) or auth_br
    print("PAC IN USE" if uses_pac else "no PAC found (arm64 without pointer authentication)")
    sys.exit(2 if uses_pac else 0)


main()
