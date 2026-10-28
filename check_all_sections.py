import struct
import capstone

cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)

for bin_path in [
    'MyGame-v1-7-2/Payload/MyGame.app/MyGame',
    'MyGame-v1-7-2/Payload/MyGame.app/Frameworks/UnityFramework.framework/UnityFramework'
]:
    data = open(bin_path, 'rb').read()
    print(f"\n=== Executable Sections in {bin_path.split('/')[-1]} ===")
    
    offset = 32
    magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved = struct.unpack_from('<IIIIIIII', data, 0)
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from('<II', data, offset)
        if cmd == 0x19: # LC_SEGMENT_64
            soff = offset + 72
            nsects = struct.unpack_from('<I', data, offset + 64)[0]
            for _ in range(nsects):
                sname = data[soff:soff+16].split(b'\x00', 1)[0].decode('ascii', 'ignore')
                saddr, ssize, sfileoff, salign, sreloff, snrel, sflags, _res = struct.unpack_from('<QQIIIIII', data, soff+32)
                # Check executable flag: S_ATTR_SOME_INSTRUCTIONS (0x00000400) or S_ATTR_PURE_INSTRUCTIONS (0x80000000)
                if (sflags & 0x80000400) != 0:
                    sec_bytes = data[sfileoff : sfileoff + ssize]
                    svc_80_hits = []
                    x18_hits = []
                    for i in range(0, len(sec_bytes) - 4, 4):
                        word = int.from_bytes(sec_bytes[i:i+4], 'little')
                        if word == 0xd4001001:
                            svc_80_hits.append(hex(saddr + i))
                        if ((word & 0x1f) == 18) or (((word >> 5) & 0x1f) == 18) or (((word >> 16) & 0x1f) == 18):
                            insns = list(cs.disasm(sec_bytes[i:i+4], saddr + i))
                            if insns:
                                op = insns[0].op_str
                                import re
                                if re.search(r'\b[xw]18\b', op):
                                    x18_hits.append(f"{hex(saddr + i)}: {insns[0].mnemonic} {op}")
                    print(f"  Section {sname:16s} ({ssize:8d} bytes at {hex(saddr)}): svc #0x80 hits: {len(svc_80_hits)}, x18 uses: {len(x18_hits)}")
                soff += 80
        offset += cmdsize
