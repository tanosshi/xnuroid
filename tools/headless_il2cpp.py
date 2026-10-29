#!/usr/bin/env python3
"""
Headless Milestone with libc++ basic_string shims to open global-metadata.dat
"""

import os
import struct
from pathlib import Path
from macho_runner import MachOBinary, ExecutionHarness, align_up
import capstone
import unicorn
from unicorn import arm64_const as uc_arm64
from game_config import app_bundle_dir, app_executable_path, unity_framework_path

unity_macho = MachOBinary(unity_framework_path())

UNITY_BASE = 0x200000000

harness = ExecutionHarness(verbose=False)
harness.load_macho(unity_macho, base_slide=UNITY_BASE)

def dummy_zero(h): return 0

harness.register_handler("___cxa_atexit", dummy_zero)
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
harness.register_handler("_newlocale", dummy_zero)
harness.register_handler("_setlocale", dummy_zero)
harness.register_handler("_pthread_cond_init", dummy_zero)
harness.register_handler("_malloc_size", lambda h: 64)
harness.register_handler("__dyld_image_count", lambda h: 1)

def shim_malloc(h):
    size = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X0)
    addr = h.heap_ptr
    h.heap_ptr = align_up(h.heap_ptr + max(size, 16), 16)
    h.uc.mem_write(addr, b"\x00" * max(size, 16))
    return addr

def shim_free(h): return 0

def shim_realloc(h):
    ptr = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X0)
    size = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X1)
    new_addr = h.heap_ptr
    alloc_size = max(size, 16)
    h.heap_ptr = align_up(h.heap_ptr + alloc_size, 16)
    h.uc.mem_write(new_addr, b"\x00" * alloc_size)
    if ptr != 0:
        try:
            old_data = h.uc.mem_read(ptr, min(size, 64))
            h.uc.mem_write(new_addr, bytes(old_data))
        except Exception:
            pass
    return new_addr

def shim_strlen(h):
    addr = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X0)
    return len(h.read_string(addr))

def shim_memcpy(h):
    dst = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X0)
    src = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X1)
    n = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X2)
    if n > 0:
        h.uc.mem_write(dst, bytes(h.uc.mem_read(src, n)))
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

def shim_ns_get_executable_path(h):
    buf = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X0)
    bufsize_ptr = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X1)
    # Return the path to the guest executable
    path = f"{app_executable_path()}\x00".encode('utf-8')
    if buf == 0:
        h.uc.mem_write(bufsize_ptr, struct.pack("<I", len(path)))
        return -1
    else:
        h.uc.mem_write(buf, path)
        return 0

# libc++ std::string implementation in Python
string_objects = {} # this_ptr -> bytearray

def shim_string_reserve(h):
    this_ptr = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X0)
    cap = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X1)
    if this_ptr not in string_objects:
        string_objects[this_ptr] = bytearray()
    return 0

def shim_string_append_chars(h):
    this_ptr = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X0)
    s_ptr = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X1)
    n = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X2)
    if this_ptr not in string_objects:
        string_objects[this_ptr] = bytearray()
    
    # Check if s_ptr is another string_object
    if s_ptr in string_objects:
        chunk = bytes(string_objects[s_ptr])
    else:
        chunk = bytes(h.uc.mem_read(s_ptr, n))
    
    string_objects[this_ptr].extend(chunk)
    # Write null-terminated string to this_ptr + 1 (SSO short string) or long string layout
    s_bytes = bytes(string_objects[this_ptr]) + b"\x00"
    if len(s_bytes) <= 23:
        # short string: byte 0 = len << 1
        h.uc.mem_write(this_ptr, struct.pack("<B", len(string_objects[this_ptr]) << 1))
        h.uc.mem_write(this_ptr + 1, s_bytes)
    else:
        # long string: ptr at this_ptr, size at this_ptr + 8, cap at this_ptr + 16 | 1
        buf_addr = h.heap_ptr
        h.heap_ptr = align_up(h.heap_ptr + len(s_bytes), 16)
        h.uc.mem_write(buf_addr, s_bytes)
        h.uc.mem_write(this_ptr, struct.pack("<QQQ", buf_addr, len(string_objects[this_ptr]), len(s_bytes) | 1))
    return this_ptr

def shim_string_append_char(h):
    this_ptr = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X0)
    n = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X1)
    c = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X2) & 0xFF
    if this_ptr not in string_objects:
        string_objects[this_ptr] = bytearray()
    string_objects[this_ptr].extend(bytes([c]) * n)
    s_bytes = bytes(string_objects[this_ptr]) + b"\x00"
    if len(s_bytes) <= 23:
        h.uc.mem_write(this_ptr, struct.pack("<B", len(string_objects[this_ptr]) << 1))
        h.uc.mem_write(this_ptr + 1, s_bytes)
    else:
        buf_addr = h.heap_ptr
        h.heap_ptr = align_up(h.heap_ptr + len(s_bytes), 16)
        h.uc.mem_write(buf_addr, s_bytes)
        h.uc.mem_write(this_ptr, struct.pack("<QQQ", buf_addr, len(string_objects[this_ptr]), len(s_bytes) | 1))
    return this_ptr

harness.register_handler("__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE7reserveEm", shim_string_reserve)
harness.register_handler("__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6appendEPKcm", shim_string_append_chars)
harness.register_handler("__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6appendEmc", shim_string_append_char)

# File I/O
open_files = {}
next_fd = 100

def get_string_content(h, ptr):
    # Check if ptr is in string_objects
    if ptr in string_objects:
        return bytes(string_objects[ptr]).decode('utf-8', 'ignore')
    # Check libc++ layout
    raw = h.uc.mem_read(ptr, 24)
    if (raw[0] & 1) == 0:
        size = raw[0] >> 1
        return raw[1:1+size].decode('utf-8', 'ignore')
    else:
        str_ptr, size, cap = struct.unpack('<QQQ', raw)
        return h.read_string(str_ptr, size)

def shim_open(h):
    global next_fd
    path_ptr = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X0)
    flags = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X1)
    path = get_string_content(h, path_ptr)
    if not path:
        path = h.read_string(path_ptr)

    print(f"\n>>> [FileIO TRAP] _open('{path}', flags={hex(flags)}) <<<")
    
    # Candidate path resolution
    candidates = [
        Path(path),
        app_bundle_dir() / path,
        app_bundle_dir() / "Data" / path.replace("Data/", ""),
        app_bundle_dir() / "Data" / "Managed" / "Metadata" / "global-metadata.dat",
    ]

    for cand in candidates:
        if cand.exists() and cand.is_file():
            fd = next_fd
            next_fd += 1
            f = cand.open("rb")
            open_files[fd] = f
            size = cand.stat().st_size
            print(f"[*] [FileIO SUCCESS] Opened '{cand}' (FD={fd}, size={size:,} bytes)!")
            return fd

    print(f"  [FileIO ERROR] File not found: '{path}'")
    return -1

def shim_fstat(h):
    fd = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X0)
    statbuf = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X1)
    if fd in open_files:
        f = open_files[fd]
        size = os.fstat(f.fileno()).st_size
        print(f"  [FileIO] _fstat(fd={fd}) -> size={size:,} bytes")
        # Darwin struct stat: st_size is at offset 96 (0x60)
        h.uc.mem_write(statbuf + 0x60, struct.pack("<q", size))
        return 0
    return -1

def shim_mmap(h):
    addr = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X0)
    length = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X1)
    prot = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X2)
    flags = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X3)
    fd = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X4)
    offset = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X5)
    print(f"  [FileIO] _mmap(len={length:,}, fd={fd}, off={hex(offset)})")
    
    alloc_addr = align_up(h.heap_ptr, 4096)
    aligned_len = align_up(length, 4096)
    h.heap_ptr = alloc_addr + aligned_len

    if fd in open_files:
        f = open_files[fd]
        f.seek(offset)
        content = f.read(length)
        if len(content) < aligned_len:
            content += b"\x00" * (aligned_len - len(content))
        h.uc.mem_write(alloc_addr, content)
        magic = struct.unpack_from("<I", content, 0)[0]
        version = struct.unpack_from("<I", content, 4)[0]
        print(f"[*] [METADATA LOADED] Mapped {len(content):,} bytes at {hex(alloc_addr)}! Magic={hex(magic)}, Version={version}")
        return alloc_addr
    else:
        h.uc.mem_write(alloc_addr, b"\x00" * aligned_len)
        return alloc_addr

def shim_read(h):
    fd = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X0)
    buf = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X1)
    count = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X2)
    if fd in open_files:
        data = open_files[fd].read(count)
        h.uc.mem_write(buf, data)
        return len(data)
    return -1

def shim_close(h):
    fd = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X0)
    if fd in open_files:
        open_files[fd].close()
        del open_files[fd]
    return 0

# Set up errno buffer
errno_addr = harness.heap_ptr
harness.heap_ptr = align_up(harness.heap_ptr + 16, 16)
harness.uc.mem_write(errno_addr, struct.pack("<I", 0))

tls_keys = {}
harness.register_handler("_malloc", shim_malloc)
harness.register_handler("_calloc", shim_malloc)
harness.register_handler("_free", shim_free)
harness.register_handler("_realloc", shim_realloc)
harness.register_handler("_posix_memalign", shim_posix_memalign)
harness.register_handler("_strlen", shim_strlen)
harness.register_handler("_memcpy", shim_memcpy)
harness.register_handler("_memmove", shim_memcpy)
harness.register_handler("___cxa_guard_acquire", shim_cxa_guard_acquire)
harness.register_handler("___cxa_guard_release", shim_cxa_guard_release)
harness.register_handler("_dispatch_semaphore_create", shim_dispatch_semaphore_create)
harness.register_handler("__NSGetExecutablePath", shim_ns_get_executable_path)
harness.register_handler("_open", shim_open)
harness.register_handler("_fstat", shim_fstat)
harness.register_handler("_read", shim_read)
harness.register_handler("_mmap", shim_mmap)
harness.register_handler("_close", shim_close)
harness.register_handler("___error", lambda h: errno_addr)
harness.register_handler("_pthread_setspecific", lambda h: (tls_keys.update({h.uc.reg_read(uc_arm64.UC_ARM64_REG_X0): h.uc.reg_read(uc_arm64.UC_ARM64_REG_X1)}) or 0))
harness.register_handler("_pthread_getspecific", lambda h: tls_keys.get(h.uc.reg_read(uc_arm64.UC_ARM64_REG_X0), 0))

print("[Headless] Running UnityFramework static constructors...")
for rva in unity_macho.mod_init_funcs:
    func_ptr = rva + UNITY_BASE
    sp = harness.STACK_BASE + harness.STACK_SIZE - 0x2000
    harness.uc.reg_write(uc_arm64.UC_ARM64_REG_SP, sp)
    harness.uc.reg_write(uc_arm64.UC_ARM64_REG_LR, 0xDEADBEEF)
    try:
        harness.uc.emu_start(func_ptr, 0xDEADBEEF, timeout=0, count=50000)
    except Exception:
        pass

print("[Headless] Calling _il2cpp_set_data_dir('Data')...")
IL2CPP_SET_DATA_DIR = UNITY_BASE + 0xbdec80
data_dir_str = harness.allocate_string("Data")
harness.uc.reg_write(uc_arm64.UC_ARM64_REG_X0, data_dir_str)
harness.uc.reg_write(uc_arm64.UC_ARM64_REG_LR, 0xDEADBEEF)
harness.uc.emu_start(IL2CPP_SET_DATA_DIR, 0xDEADBEEF, timeout=0, count=50000)

print("\n[Headless] Calling _il2cpp_init('UnityRootDomain')...")
IL2CPP_INIT = UNITY_BASE + 0xbdeba0
domain_str = harness.allocate_string("UnityRootDomain")
harness.uc.reg_write(uc_arm64.UC_ARM64_REG_X0, domain_str)
harness.uc.reg_write(uc_arm64.UC_ARM64_REG_LR, 0xDEADBEEF)

harness.on_trap = lambda sym, x0, x1, x2, x3: None # keep clean

try:
    harness.uc.emu_start(IL2CPP_INIT, 0xDEADBEEF, timeout=0, count=200000)
    print("\n>>> _il2cpp_init finished execution successfully! <<<")
except Exception as e:
    pc = harness.uc.reg_read(uc_arm64.UC_ARM64_REG_PC)
    print(f"\nHalted at PC={hex(pc)}: {e}")
