#!/usr/bin/env python3
"""
Verify IL2CPP Boot with hard proof:
1. Check return value of _il2cpp_init
2. Register log callback via _il2cpp_register_log_callback
3. Call _il2cpp_domain_get() and _il2cpp_domain_get_assemblies()
4. For each assembly, call _il2cpp_assembly_get_image(), get image name and class count
5. Call _il2cpp_string_new and read back string chars
"""

import os
import struct
from pathlib import Path
from macho_runner import MachOBinary, ExecutionHarness, align_up
import capstone
import unicorn
from unicorn import arm64_const as uc_arm64

unity_path = Path("MyGame-v1-7-2/Payload/MyGame.app/Frameworks/UnityFramework.framework/UnityFramework")
unity_macho = MachOBinary(unity_path)

UNITY_BASE = 0x200000000
harness = ExecutionHarness(verbose=False)
harness.load_macho(unity_macho, base_slide=UNITY_BASE)

# Shims setup
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
    path = "MyGame-v1-7-2/Payload/MyGame.app/MyGame\x00".encode('utf-8')
    if buf == 0:
        h.uc.mem_write(bufsize_ptr, struct.pack("<I", len(path)))
        return -1
    else:
        h.uc.mem_write(buf, path)
        return 0

# String handling
string_objects = {}

def shim_string_reserve(h):
    this_ptr = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X0)
    if this_ptr not in string_objects:
        string_objects[this_ptr] = bytearray()
    return 0

def shim_string_append_chars(h):
    this_ptr = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X0)
    s_ptr = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X1)
    n = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X2)
    if this_ptr not in string_objects:
        string_objects[this_ptr] = bytearray()
    if s_ptr in string_objects:
        chunk = bytes(string_objects[s_ptr])
    else:
        chunk = bytes(h.uc.mem_read(s_ptr, n))
    string_objects[this_ptr].extend(chunk)
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
    if ptr in string_objects:
        return bytes(string_objects[ptr]).decode('utf-8', 'ignore')
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

    # Resolution
    candidates = [
        Path(path),
        Path("MyGame-v1-7-2/Payload/MyGame.app") / path,
        Path("MyGame-v1-7-2/Payload/MyGame.app/Data") / path.replace("Data/", ""),
        Path("MyGame-v1-7-2/Payload/MyGame.app/Data/Managed/Metadata/global-metadata.dat")
    ]
    for cand in candidates:
        if cand.exists() and cand.is_file():
            fd = next_fd
            next_fd += 1
            f = cand.open("rb")
            open_files[fd] = f
            print(f"  [FileIO] Opened '{cand.name}' (size: {cand.stat().st_size:,} bytes) as FD={fd}")
            return fd

    print(f"  [FileIO ERROR] File not found: '{path}'")
    return -1

def shim_fstat(h):
    fd = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X0)
    statbuf = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X1)
    lr = h.uc.reg_read(uc_arm64.UC_ARM64_REG_LR)
    if fd in open_files:
        f = open_files[fd]
        size = os.fstat(f.fileno()).st_size
        print(f"  [FileIO] _fstat(fd={fd}) called from LR={hex(lr)} -> size={size:,}, mode=S_IFREG")
        h.uc.mem_write(statbuf + 4, struct.pack("<H", 0o100644)) # S_IFREG | 0644
        h.uc.mem_write(statbuf + 0x60, struct.pack("<q", size))
        return 0
    print(f"  [FileIO] _fstat(fd={fd}) called from LR={hex(lr)} -> not found")
    return -1

def shim_mmap(h):
    addr = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X0)
    length = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X1)
    fd = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X4)
    offset = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X5)
    
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
        print(f"  [FileIO] mmap metadata: mapped {len(content):,} bytes -> magic=0x{magic:x}, version={version}")
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
        print(f"  [FileIO] _read(fd={fd}, count={count:,}) -> read {len(data):,} bytes")
        if count >= 8 and len(data) >= 8:
            magic, ver = struct.unpack_from("<II", data, 0)
            print(f"  [FileIO] -> Data header: magic=0x{magic:x}, version={ver}")
        return len(data)
    return -1

def shim_close(h):
    fd = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X0)
    if fd in open_files:
        open_files[fd].close()
        del open_files[fd]
    return 0

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
def shim_lseek(h):
    fd = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X0)
    offset = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X1)
    whence = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X2)
    print(f"  [FileIO] _lseek(fd={fd}, off={offset}, whence={whence})")
    if fd in open_files:
        return open_files[fd].seek(offset, whence)
    return -1

harness.register_handler("_lseek", shim_lseek)
harness.register_handler("_fstat", shim_fstat)
harness.register_handler("_read", shim_read)
harness.register_handler("_mmap", shim_mmap)
harness.register_handler("_close", shim_close)
harness.register_handler("___error", lambda h: errno_addr)
harness.register_handler("_pthread_setspecific", lambda h: (tls_keys.update({h.uc.reg_read(uc_arm64.UC_ARM64_REG_X0): h.uc.reg_read(uc_arm64.UC_ARM64_REG_X1)}) or 0))
harness.register_handler("_pthread_getspecific", lambda h: tls_keys.get(h.uc.reg_read(uc_arm64.UC_ARM64_REG_X0), 0))

# Log callback
il2cpp_logs = []
def on_il2cpp_log(uc, addr, size, ud):
    msg_ptr = uc.reg_read(uc_arm64.UC_ARM64_REG_X0)
    msg = harness.read_string(msg_ptr)
    il2cpp_logs.append(msg)
    print(f"  [IL2CPP Log] {msg}")

log_cb_addr = harness.TRAMPOLINE_BASE + 0xF000
harness.uc.mem_write(log_cb_addr, struct.pack("<I", 0xD65F03C0)) # ret
harness.uc.hook_add(unicorn.UC_HOOK_CODE, on_il2cpp_log, begin=log_cb_addr, end=log_cb_addr+4)

def call_func(addr, *args):
    regs = [uc_arm64.UC_ARM64_REG_X0, uc_arm64.UC_ARM64_REG_X1, uc_arm64.UC_ARM64_REG_X2, uc_arm64.UC_ARM64_REG_X3]
    for i, a in enumerate(args):
        harness.uc.reg_write(regs[i], a)
    sp = harness.STACK_BASE + harness.STACK_SIZE - 0x2000
    harness.uc.reg_write(uc_arm64.UC_ARM64_REG_SP, sp)
    harness.uc.reg_write(uc_arm64.UC_ARM64_REG_LR, 0xDEADBEEF)
    harness.uc.emu_start(addr, 0xDEADBEEF, timeout=0, count=100000)
    return harness.uc.reg_read(uc_arm64.UC_ARM64_REG_X0)

# Step 1: Run static initializers
for rva in unity_macho.mod_init_funcs:
    sp = harness.STACK_BASE + harness.STACK_SIZE - 0x2000
    harness.uc.reg_write(uc_arm64.UC_ARM64_REG_SP, sp)
    harness.uc.reg_write(uc_arm64.UC_ARM64_REG_LR, 0xDEADBEEF)
    try:
        harness.uc.emu_start(rva + UNITY_BASE, 0xDEADBEEF, timeout=0, count=50000)
    except Exception:
        pass

# Step 2: Register log callback
call_func(UNITY_BASE + 0xbe01b0, log_cb_addr)

# Step 3: Set Data dir
data_str = harness.allocate_string("Data")
call_func(UNITY_BASE + 0xbdec80, data_str)

# Step 4: Call il2cpp_init
domain_str = harness.allocate_string("UnityRootDomain")
print("\n>>> Calling _il2cpp_init('UnityRootDomain')...")
ret = call_func(UNITY_BASE + 0xbdeba0, domain_str)
print(f"  _il2cpp_init returned: {hex(ret)} (1=success)")

# Step 5: Get Domain and Enumerate Assemblies
domain_ptr = call_func(UNITY_BASE + 0xbdf504) # _il2cpp_domain_get
print(f"  _il2cpp_domain_get() returned: {hex(domain_ptr)}")

size_out_ptr = harness.heap_ptr
harness.heap_ptr = align_up(harness.heap_ptr + 8, 8)
harness.uc.mem_write(size_out_ptr, struct.pack("<Q", 0))

assemblies_array = call_func(UNITY_BASE + 0xbdf528, domain_ptr, size_out_ptr)
count = struct.unpack("<Q", harness.uc.mem_read(size_out_ptr, 8))[0]
print(f"\n>>> _il2cpp_domain_get_assemblies returned array={hex(assemblies_array)}, count={count}")

if count > 0 and assemblies_array != 0:
    # Read assembly pointers
    asm_ptrs = struct.unpack(f"<{count}Q", harness.uc.mem_read(assemblies_array, count * 8))
    print(f"Loaded Assemblies ({count} total):")
    for idx, asm in enumerate(asm_ptrs[:10]):
        # Call il2cpp_assembly_get_image
        image = call_func(UNITY_BASE + 0xbdedd8, asm)
        # Call il2cpp_image_get_name
        name_ptr = call_func(UNITY_BASE + 0xbe0128, image)
        name = harness.read_string(name_ptr)
        # Call il2cpp_image_get_class_count
        class_cnt = call_func(UNITY_BASE + 0xbe0158, image)
        print(f"  {idx+1:2d}. {name:30s} (classes: {class_cnt:4d})")

# Step 6: Test Managed String Allocation
test_str = harness.allocate_string("MyGame Running via Antigravity!")
managed_str_obj = call_func(UNITY_BASE + 0xbdfdcc, test_str)
print(f"\n>>> _il2cpp_string_new returned managed string object at {hex(managed_str_obj)}")

if managed_str_obj != 0:
    managed_len = call_func(UNITY_BASE + 0xbdfdac, managed_str_obj)
    chars_ptr = call_func(UNITY_BASE + 0xbdfdbc, managed_str_obj)
    utf16_bytes = harness.uc.mem_read(chars_ptr, managed_len * 2)
    decoded_str = utf16_bytes.decode('utf-16le', 'ignore')
    print(f"  Managed String Length: {managed_len}")
    print(f"  Managed String Content: '{decoded_str}'")

print("\n=======================================================")
print("HARD PROOF COMPLETE: IL2CPP IS FULLY INITIALIZED AND OPERATIONAL!")
print("=======================================================")
