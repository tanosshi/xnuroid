from pathlib import Path
import capstone
from macho_runner import MachOBinary
from game_config import unity_framework_path

b = MachOBinary(unity_framework_path())

seg_text = [seg for seg in b.segments if seg.name == "__TEXT"][0]
text_bytes = b.data[seg_text.fileoff : seg_text.fileoff + seg_text.filesize]

md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)

mrs_sites = []
for off in range(0, len(text_bytes), 4):
    u32 = int.from_bytes(text_bytes[off:off+4], "little")
    # MRS Xt, TPIDRRO_EL0 is 0xd53bd060 | t
    if (u32 & 0xFFFFFFE0) == 0xd53bd060:
        reg_num = u32 & 0x1F
        mrs_sites.append((off, reg_num, u32))

print(f"Found {len(mrs_sites)} mrs *, tpidrro_el0 sites")

patterns = {}
site_details = []

for idx, (off, reg, u32) in enumerate(mrs_sites):
    vm_addr = seg_text.vmaddr + off
    code_slice = text_bytes[off : off + 32]
    disasm = list(md.disasm(code_slice, vm_addr))
    instrs = [f"{ins.mnemonic} {ins.op_str}" for ins in disasm[:6]]
    
    reg_name = f"x{reg}"
    pattern_desc = "other"
    accessed_offset = None
    
    # Check next instructions for access through reg_name
    for next_ins in disasm[1:4]:
        if f"[{reg_name}" in next_ins.op_str:
            if "#" in next_ins.op_str:
                accessed_offset = next_ins.op_str.split("#")[-1].rstrip("]").strip()
                pattern_desc = f"{next_ins.mnemonic} [{reg_name}, #{accessed_offset}]"
            else:
                pattern_desc = f"{next_ins.mnemonic} [{reg_name}]"
            break
        elif next_ins.mnemonic.startswith("add") and reg_name in next_ins.op_str:
            pattern_desc = f"{next_ins.mnemonic} {next_ins.op_str}"
            break
    
    patterns[pattern_desc] = patterns.get(pattern_desc, 0) + 1
    site_details.append((vm_addr, instrs, pattern_desc, accessed_offset))

print("\n=== PATTERNS OBSERVED ===")
for p, count in sorted(patterns.items(), key=lambda x: -x[1]):
    print(f"  {p:45s}: {count:2d} sites")

print("\n=== ALL 62 SITES AND OFFSETS ===")
for i, (vm_addr, instrs, pat, off) in enumerate(site_details, 1):
    ins0 = instrs[0] if len(instrs) > 0 else ""
    ins1 = instrs[1] if len(instrs) > 1 else ""
    ins2 = instrs[2] if len(instrs) > 2 else ""
    print(f"{i:2d}. 0x{vm_addr:08x}: {pat:35s} | {ins0} ; {ins1} ; {ins2}")
