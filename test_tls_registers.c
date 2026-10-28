#include <stdio.h>
#include <stdint.h>
#include <signal.h>
#include <setjmp.h>

static sigjmp_buf jmpbuf;

static void sigill_handler(int sig) {
    (void)sig;
    siglongjmp(jmpbuf, 1);
}

int main(void) {
    printf("[TLS Test] Testing TPIDR_EL0 and TPIDRRO_EL0 behavior on this device...\n");

    // Test 1: TPIDR_EL0 (Bionic TLS)
    uint64_t tpidr = 0;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tpidr));
    printf("[TLS Test] TPIDR_EL0 read: 0x%llx\n", (unsigned long long)tpidr);

    // Test 2: TPIDRRO_EL0 (Apple Darwin TLS)
    signal(SIGILL, sigill_handler);
    if (sigsetjmp(jmpbuf, 1) == 0) {
        uint64_t tpidrro = 0;
        __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tpidrro));
        printf("[TLS Test] TPIDRRO_EL0 read: 0x%llx (SUCCESS - read is permitted!)\n", (unsigned long long)tpidrro);
    } else {
        printf("[TLS Test] TPIDRRO_EL0 read triggered SIGILL (trap needed or emulate/kernel restricted)\n");
    }

    return 0;
}
