// Windows implementation of the small mmap surface declared in tools/host/sys/mman.h.
#include "host/sys/mman.h"

#include <windows.h>

void* mmap(void* addr, size_t length, int prot, int flags, int fd, long long offset) {
    (void)fd;
    (void)offset;
    DWORD page = PAGE_READWRITE;
    if (prot & PROT_EXEC) page = (prot & PROT_WRITE) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    else if (!(prot & PROT_WRITE)) page = PAGE_READONLY;

    const DWORD type = MEM_RESERVE | MEM_COMMIT;
    void* p = VirtualAlloc(addr, length, type, page);
    if (!p && !(flags & MAP_FIXED)) p = VirtualAlloc(nullptr, length, type, page);
    if (!p) return MAP_FAILED;
    if ((flags & MAP_FIXED) && p != addr) {
        VirtualFree(p, 0, MEM_RELEASE);
        return MAP_FAILED;
    }
    return p;
}

int munmap(void* addr, size_t length) {
    (void)length;
    return VirtualFree(addr, 0, MEM_RELEASE) ? 0 : -1;
}

int mprotect(void* addr, size_t length, int prot) {
    DWORD page = PAGE_READWRITE;
    if (prot & PROT_EXEC) page = (prot & PROT_WRITE) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    else if (!(prot & PROT_WRITE)) page = PAGE_READONLY;
    DWORD old = 0;
    return VirtualProtect(addr, length, page, &old) ? 0 : -1;
}
