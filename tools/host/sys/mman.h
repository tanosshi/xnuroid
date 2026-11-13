// Minimal <sys/mman.h> for the Windows host build of src/loader/armv7.cpp.
// The loader only ever wants anonymous, writable pages at a preferred low address, so the host implementation in
// tools/host_mmap.cpp maps these onto VirtualAlloc.
#pragma once

#include <cstddef>
#include <cstdint>

#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4

#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS
#define MAP_FIXED_NOREPLACE 0x100000

#define MAP_FAILED (reinterpret_cast<void*>(-1))

extern "C" void* mmap(void* addr, size_t length, int prot, int flags, int fd, long long offset);
extern "C" int munmap(void* addr, size_t length);
extern "C" int mprotect(void* addr, size_t length, int prot);
