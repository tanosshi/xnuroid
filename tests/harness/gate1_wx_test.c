#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void) {
    long page_size = sysconf(_SC_PAGESIZE);
    printf("[Gate 1 Test] Detected host page size: %ld bytes\n", page_size);

    // Step 1: mmap RW
    printf("[Gate 1 Test] Step 1: Allocating RW memory via mmap...\n");
    size_t alloc_size = (size_t)page_size;
    void* mem = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) {
        perror("mmap failed");
        return 1;
    }
    printf("[Gate 1 Test]   Allocated at %p\n", mem);

    // Step 2: Write ARM64 code
    // add x0, x0, #42   -> 0x9100a800
    // ret               -> 0xd65f03c0
    printf("[Gate 1 Test] Step 2: Writing ARM64 instructions to RW memory...\n");
    uint32_t* code = (uint32_t*)mem;
    code[0] = 0x9100A800; // add x0, x0, #42
    code[1] = 0xD65F03C0; // ret

    // Step 3: mprotect to RX
    printf("[Gate 1 Test] Step 3: Changing memory protection to PROT_READ | PROT_EXEC...\n");
    if (mprotect(mem, alloc_size, PROT_READ | PROT_EXEC) != 0) {
        perror("mprotect to RX failed");
        return 2;
    }

    // Step 4: Flush instruction cache
    printf("[Gate 1 Test] Step 4: Flushing instruction cache...\n");
    __builtin___clear_cache((char*)mem, (char*)mem + alloc_size);

    // Step 5: Execute
    printf("[Gate 1 Test] Step 5: Calling code at %p...\n", mem);
    typedef uint64_t (*test_fn)(uint64_t);
    test_fn fn = (test_fn)mem;
    uint64_t result = fn(100);

    printf("[Gate 1 Test] Result = %llu (expected 142)\n", (unsigned long long)result);
    if (result == 142) {
        printf("[Gate 1 Test] SUCCESS: Android W^X transition (RW -> mprotect RX -> execute) is PERMITTED!\n");
        munmap(mem, alloc_size);
        return 0;
    } else {
        printf("[Gate 1 Test] FAILED: unexpected result\n");
        munmap(mem, alloc_size);
        return 3;
    }
}
