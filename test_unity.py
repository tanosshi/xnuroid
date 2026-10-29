#!/usr/bin/env python3
"""Test loading UnityFramework and executing initializers."""

import sys
from pathlib import Path
from macho_runner import MachOBinary, ExecutionHarness
import unicorn
from unicorn import arm64_const as uc_arm64
from game_config import unity_framework_path

unity_path = unity_framework_path()
if not unity_path.exists():
    print(f"{unity_path} not found")
    sys.exit(1)

print("[Test] Parsing UnityFramework Mach-O...")
macho = MachOBinary(unity_path)
print(f"  Segments: {len(macho.segments)}")
print(f"  Rebases: {len(macho.rebases)}")
print(f"  Bindings: {len(macho.bindings)}")
print(f"  __mod_init_func initializers: {len(macho.mod_init_funcs)}")

harness = ExecutionHarness(verbose=False)
# UnityFramework preferred base is 0x0, let's load it at base slide 0x200000000 (2GB) so it doesn't conflict with anything
base = 0x200000000
harness.load_macho(macho, base_slide=base)

# Register common C/POSIX stubs
def dummy_ok(h):
    return 0

harness.register_handler("___cxa_atexit", dummy_ok)
harness.register_handler("_pthread_key_create", dummy_ok)
harness.register_handler("_pthread_mutex_init", dummy_ok)
harness.register_handler("_pthread_mutex_lock", dummy_ok)
harness.register_handler("_pthread_mutex_unlock", dummy_ok)
harness.register_handler("_mach_timebase_info", dummy_ok)
harness.register_handler("_gettimeofday", dummy_ok)
harness.register_handler("_bzero", dummy_ok)
harness.register_handler("_memset", dummy_ok)

print(f"\n[Test] Executing first 5 static initializers from __mod_init_func...")
for i in range(min(5, len(macho.mod_init_funcs))):
    func_ptr = macho.mod_init_funcs[i] + base
    print(f"\n--- Initializer #{i}: {hex(func_ptr)} ---")
    harness.uc.reg_write(uc_arm64.UC_ARM64_REG_LR, 0xDEADBEEF)
    try:
        harness.uc.emu_start(func_ptr, 0xDEADBEEF, timeout=0, count=5000)
        print(f"Initializer #{i} completed successfully!")
    except unicorn.UcError as e:
        pc = harness.uc.reg_read(uc_arm64.UC_ARM64_REG_PC)
        print(f"Initializer #{i} halted: {e} at PC={hex(pc)}")

print("\n[Test] UnityFramework loader verification completed!")
