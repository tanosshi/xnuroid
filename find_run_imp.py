#!/usr/bin/env python3
"""Find the method implementation (IMP) of runUIApplicationMainWithArgc:argv: in UnityFramework."""

import struct
from pathlib import Path
from macho_runner import MachOBinary

unity_path = Path("MyGame-v1-7-2/Payload/MyGame.app/Frameworks/UnityFramework.framework/UnityFramework")
macho = MachOBinary(unity_path)

# Let's search for the method name in __TEXT.__objc_methname
target_meth = b"runUIApplicationMainWithArgc:argv:"
idx = macho.data.find(target_meth)
print(f"Method name found at file offset: {hex(idx)}")

# In Mach-O, method lists in 64-bit:
# Objective-C method list entries (or relative method lists)
# Let's search for references to this offset in __DATA or __DATA_CONST
# In UnityFramework (iOS 14+), methods are often relative method lists:
# struct method_t { int32_t name_offset; int32_t types_offset; int32_t imp_offset; };
# Or 64-bit pointers: { uint64_t name; uint64_t types; uint64_t imp; }
print("Searching for method list referencing this selector...")
for offset in range(0, len(macho.data) - 8, 4):
    # Check 64-bit pointer
    val64 = struct.unpack_from("<Q", macho.data, offset)[0]
    if val64 == idx: # vmaddr == fileoff for __TEXT in UnityFramework!
        print(f"64-bit pointer to methname at fileoff {hex(offset)}")
        # Next two pointers might be types and imp
        types_ptr, imp_ptr = struct.unpack_from("<QQ", macho.data, offset + 8)
        print(f"   types: {hex(types_ptr)}, imp: {hex(imp_ptr)}")
    
    # Check relative method list (offset of relative pointer is offset + rel_val = target)
    rel_val = struct.unpack_from("<i", macho.data, offset)[0]
    if offset + rel_val == idx:
        print(f"Relative pointer to methname at fileoff {hex(offset)}")
        types_rel, imp_rel = struct.unpack_from("<ii", macho.data, offset + 4)
        imp_addr = offset + 8 + imp_rel
        print(f"   types: {hex(offset + 4 + types_rel)}, imp: {hex(imp_addr)}")
