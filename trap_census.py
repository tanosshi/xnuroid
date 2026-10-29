#!/usr/bin/env python3
"""
Trap Census: Run real UnityFramework code under Unicorn from the guest launcher.
Logs every single trap hit during the execution of real engine startup code.
"""

import struct
from pathlib import Path
from macho_runner import MachOBinary, ExecutionHarness, align_up
import capstone
import unicorn
from unicorn import arm64_const as uc_arm64
from game_config import app_executable_path, unity_framework_path

launcher_macho = MachOBinary(app_executable_path())
unity_macho = MachOBinary(unity_framework_path())

cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)

# Base slide for UnityFramework
UNITY_BASE = 0x200000000

harness = ExecutionHarness(verbose=False)

# 1. Load the guest launcher at its preferred base 0x100000000
launcher_entry = harness.load_macho(launcher_macho, base_slide=0)

# 2. Load UnityFramework at UNITY_BASE
unity_entry = harness.load_macho(unity_macho, base_slide=UNITY_BASE)

# Register C/POSIX runtime shims
def dummy_zero(h): return 0
harness.register_handler("___cxa_atexit", dummy_zero)
harness.register_handler("_pthread_key_create", dummy_zero)
harness.register_handler("_pthread_mutex_init", dummy_zero)
harness.register_handler("_pthread_mutex_lock", dummy_zero)
harness.register_handler("_pthread_mutex_unlock", dummy_zero)
harness.register_handler("_pthread_mutexattr_init", dummy_zero)
harness.register_handler("_pthread_mutexattr_settype", dummy_zero)
harness.register_handler("_pthread_mutexattr_destroy", dummy_zero)
harness.register_handler("_pthread_self", dummy_zero)
harness.register_handler("_mach_timebase_info", dummy_zero)
harness.register_handler("_gettimeofday", dummy_zero)
harness.register_handler("_bzero", dummy_zero)
harness.register_handler("_memset", dummy_zero)

def shim_malloc(h):
    size = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X0)
    addr = h.heap_ptr
    h.heap_ptr = align_up(h.heap_ptr + max(size, 16), 16)
    h.uc.mem_write(addr, b"\x00" * max(size, 16))
    return addr

def shim_free(h): return 0

def shim_strlen(h):
    addr = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X0)
    s = h.read_string(addr)
    return len(s)

def shim_memcpy(h):
    dst = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X0)
    src = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X1)
    n = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X2)
    if n > 0:
        data = h.uc.mem_read(src, n)
        h.uc.mem_write(dst, bytes(data))
    return dst

def shim_cxa_guard_acquire(h):
    guard_addr = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X0)
    val = struct.unpack("<B", h.uc.mem_read(guard_addr, 1))[0]
    return 1 if val == 0 else 0

def shim_cxa_guard_release(h):
    guard_addr = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X0)
    h.uc.mem_write(guard_addr, b"\x01")
    return 0

def shim_posix_memalign(h):
    memptr = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X0)
    alignment = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X1)
    size = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X2)
    align = max(alignment, 16)
    addr = align_up(h.heap_ptr, align)
    h.heap_ptr = align_up(addr + max(size, 16), 16)
    h.uc.mem_write(addr, b"\x00" * max(size, 16))
    h.uc.mem_write(memptr, struct.pack("<Q", addr))
    return 0

def shim_dispatch_semaphore_create(h):
    val = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X0)
    sem_addr = h.heap_ptr
    h.heap_ptr = align_up(h.heap_ptr + 32, 16)
    h.uc.mem_write(sem_addr, struct.pack("<QQ", val, val))
    return sem_addr

harness.register_handler("_malloc", shim_malloc)
harness.register_handler("_calloc", shim_malloc)
harness.register_handler("_free", shim_free)
harness.register_handler("_strlen", shim_strlen)
harness.register_handler("_memcpy", shim_memcpy)
harness.register_handler("_memmove", shim_memcpy)
harness.register_handler("___cxa_guard_acquire", shim_cxa_guard_acquire)
harness.register_handler("___cxa_guard_release", shim_cxa_guard_release)
harness.register_handler("_posix_memalign", shim_posix_memalign)
harness.register_handler("_dispatch_semaphore_create", shim_dispatch_semaphore_create)

# Trap census logging
census_log = []
trap_counts = {}

def log_trap(sym, x0, x1, x2, x3):
    desc = f"{sym}(x0={hex(x0)}, x1={hex(x1)}, x2={hex(x2)}, x3={hex(x3)})"
    census_log.append(desc)
    trap_counts[sym] = trap_counts.get(sym, 0) + 1
    # print live
    if trap_counts[sym] <= 5: # only print first 5 of each symbol to avoid spam
        print(f"  [TRAP #{len(census_log):03d}] {desc}")

harness.on_trap = log_trap

# 3. First, execute all 449 static initializers in UnityFramework!
print("[Census] Step 1: Running all 449 static constructors of UnityFramework...")
for i, rva in enumerate(unity_macho.mod_init_funcs):
    func_ptr = rva + UNITY_BASE
    sp = harness.STACK_BASE + harness.STACK_SIZE - 0x2000
    harness.uc.reg_write(uc_arm64.UC_ARM64_REG_SP, sp)
    harness.uc.reg_write(uc_arm64.UC_ARM64_REG_LR, 0xDEADBEEF)
    try:
        harness.uc.emu_start(func_ptr, 0xDEADBEEF, timeout=0, count=50000)
    except Exception as e:
        print(f"Init #{i} error: {e}")

print(f"[Census] Static constructors complete. Initial traps logged: {len(census_log)}")

# 4. Allocate a real mock UnityFramework instance on the heap
unity_inst_addr = harness.heap_ptr
harness.heap_ptr = align_up(harness.heap_ptr + 256, 16)
harness.uc.mem_write(unity_inst_addr, b"\x00" * 256)

# Real IMP of runUIApplicationMainWithArgc:argv: in UnityFramework
REAL_RUN_IMP = UNITY_BASE + 0x16894

def handle_run_main(h):
    x0 = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X0)
    x1 = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X1)
    x2 = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X2) # argc
    x3 = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X3) # argv
    lr = h.uc.reg_read(uc_arm64.UC_ARM64_REG_LR)
    print(f"\n>>> [TRANSITION] Entering REAL UnityFramework code: runUIApplicationMainWithArgc:argv: at {hex(REAL_RUN_IMP)}!")
    print(f"    x0(self)={hex(x0)}, x1(sel)={hex(x1)}, x2(argc)={x2}, x3(argv)={hex(x3)}, lr={hex(lr)}")
    # Jump directly to real IMP
    h.uc.reg_write(uc_arm64.UC_ARM64_REG_PC, REAL_RUN_IMP)
    return None # don't touch PC, let it jump!

# Hook UIApplicationMain trap
def shim_ui_application_main(h):
    argc = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X0)
    argv = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X1)
    pclass = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X2)
    dclass = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X3)
    delegate_name = h.read_cfstring(dclass) if dclass else ""
    if not delegate_name:
        delegate_name = h.read_string(dclass)
    print(f"\n[*] [CENSUS MILESTONE] REAL UnityFramework called _UIApplicationMain!")
    print(f"   argc: {argc}, argv: {hex(argv)}, principal: {hex(pclass)}, delegate: '{delegate_name}'")
    # Stop here to inspect census
    h.uc.emu_stop()
    return 0

harness.register_handler("_UIApplicationMain", shim_ui_application_main)

# Update objc_msgSend handler to forward runUIApplicationMainWithArgc:argv: to real IMP
orig_msg_send = harness._handle_objc_msg_send
def census_msg_send(obj, selector, arg1, arg2):
    if selector == "getInstance":
        return unity_inst_addr
    elif selector == "runUIApplicationMainWithArgc:argv:":
        # Set PC to REAL_RUN_IMP
        harness.uc.reg_write(uc_arm64.UC_ARM64_REG_PC, REAL_RUN_IMP)
        # Ensure x0 is the unity instance
        harness.uc.reg_write(uc_arm64.UC_ARM64_REG_X0, unity_inst_addr)
        return None
    return orig_msg_send(obj, selector, arg1, arg2)

harness._handle_objc_msg_send = census_msg_send

print("\n[Census] Step 2: Executing the guest main entry point and dispatching into UnityFramework...")
census_start_idx = len(census_log)
harness.execute_from(launcher_entry, max_instructions=100000)

print(f"\n=======================================================")
print(f"Trap Census Summary: {len(census_log)} total traps encountered")
print(f"=======================================================")
sorted_traps = sorted(trap_counts.items(), key=lambda kv: kv[1], reverse=True)
for sym, cnt in sorted_traps:
    print(f"  {sym:40s}: {cnt:4d} calls")
