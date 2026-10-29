#!/usr/bin/env python3
"""Investigate the initializers with proper malloc, memcpy, strlen, and cxa_guard shims."""

import struct
from pathlib import Path
from macho_runner import MachOBinary, ExecutionHarness, align_up
import capstone
import unicorn
from unicorn import arm64_const as uc_arm64
from game_config import unity_framework_path

macho = MachOBinary(unity_framework_path())
cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)

BASE_SLIDE = 0x200000000

harness = ExecutionHarness(verbose=False)
harness.load_macho(macho, base_slide=BASE_SLIDE)

def dummy_zero(h): return 0

# Register basic C runtime functions
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
    # Zero memory
    h.uc.mem_write(addr, b"\x00" * max(size, 16))
    return addr

def shim_free(h):
    return 0

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
    if val == 0:
        return 1 # needs init
    return 0 # already inited

def shim_dispatch_semaphore_create(h):
    val = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X0)
    sem_addr = h.heap_ptr
    h.heap_ptr = align_up(h.heap_ptr + 32, 16)
    h.uc.mem_write(sem_addr, struct.pack("<QQ", val, val))
    return sem_addr

def shim_dispatch_semaphore_wait(h):
    return 0

def shim_dispatch_semaphore_signal(h):
    return 0

def shim_cxa_guard_release(h):
    guard_addr = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X0)
    h.uc.mem_write(guard_addr, b"\x01")
    return 0

harness.register_handler("___cxa_guard_release", shim_cxa_guard_release)
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

harness.register_handler("_posix_memalign", shim_posix_memalign)
harness.register_handler("_dispatch_semaphore_create", shim_dispatch_semaphore_create)
harness.register_handler("_dispatch_semaphore_wait", shim_dispatch_semaphore_wait)
harness.register_handler("_dispatch_semaphore_signal", shim_dispatch_semaphore_signal)

harness.register_handler("_malloc", shim_malloc)
harness.register_handler("_calloc", shim_malloc)
harness.register_handler("_free", shim_free)
harness.register_handler("_strlen", shim_strlen)
harness.register_handler("_memcpy", shim_memcpy)
harness.register_handler("_memmove", shim_memcpy)
harness.register_handler("___cxa_guard_acquire", shim_cxa_guard_acquire)
harness.register_handler("___cxa_guard_release", shim_cxa_guard_release)

abort_called = [False]
def on_abort(h):
    abort_called[0] = True
    h.uc.emu_stop()
    return 0

harness.register_handler("_abort", on_abort)

current_traps = []
def trap_tracker(sym, x0, x1, x2, x3):
    current_traps.append((sym, hex(x0), hex(x1)))

harness.on_trap = trap_tracker

failed = []
success_count = 0

for i, init_rva in enumerate(macho.mod_init_funcs):
    abort_called[0] = False
    current_traps.clear()
    func_ptr = init_rva + BASE_SLIDE

    sp = harness.STACK_BASE + harness.STACK_SIZE - 0x2000
    harness.uc.reg_write(uc_arm64.UC_ARM64_REG_SP, sp)
    harness.uc.reg_write(uc_arm64.UC_ARM64_REG_LR, 0xDEADBEEF)

    error = None
    try:
        harness.uc.emu_start(func_ptr, 0xDEADBEEF, timeout=0, count=50000)
    except unicorn.UcError as e:
        pc = harness.uc.reg_read(uc_arm64.UC_ARM64_REG_PC)
        error = f"Unicorn Error: {e} at PC={hex(pc)}"
    except Exception as e:
        pc = harness.uc.reg_read(uc_arm64.UC_ARM64_REG_PC)
        error = f"Exception: {e} at PC={hex(pc)}"

    if abort_called[0]:
        pc = harness.uc.reg_read(uc_arm64.UC_ARM64_REG_PC)
        error = f"Called _abort() (halted at PC={hex(pc)})"

    if error:
        failed.append({
            "index": i,
            "rva": hex(init_rva),
            "vmaddr": hex(func_ptr),
            "error": error,
            "traps": list(current_traps)
        })
    else:
        success_count += 1

print(f"\n=======================================================")
print(f"Summary: {success_count} succeeded, {len(failed)} failed out of {len(macho.mod_init_funcs)}")
print(f"=======================================================")
for f in failed:
    print(f"\n[FAIL] Initializer #{f['index']} at RVA {f['rva']} (VM {f['vmaddr']}):")
    print(f"       Error: {f['error']}")
    print(f"       Traps: {f['traps']}")
