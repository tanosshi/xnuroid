from game_config import APP_EXECUTABLE, guest_bundle_path


def shim_realloc(h):
    ptr = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X0)
    size = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X1)
    new_addr = h.heap_ptr
    alloc_size = max(size, 16)
    h.heap_ptr = align_up(h.heap_ptr + alloc_size, 16)
    h.uc.mem_write(new_addr, b"\x00" * alloc_size)
    if ptr != 0:
        # Copy up to size bytes from old to new
        try:
            old_data = h.uc.mem_read(ptr, min(size, 64))
            h.uc.mem_write(new_addr, bytes(old_data))
        except Exception:
            pass
    return new_addr

def shim_ns_get_executable_path(h):
    buf = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X0)
    bufsize_ptr = h.uc.reg_read(uc_arm64.UC_ARM64_REG_X1)
    path = f"{guest_bundle_path()}/{APP_EXECUTABLE}\x00".encode('utf-8')
    if buf == 0:
        h.uc.mem_write(bufsize_ptr, struct.pack("<I", len(path)))
        return -1
    else:
        h.uc.mem_write(buf, path)
        return 0

harness.register_handler("_realloc", shim_realloc)
harness.register_handler("__NSGetExecutablePath", shim_ns_get_executable_path)
