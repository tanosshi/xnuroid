#include <stdio.h>
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>

static jmp_buf g_env;
static void sig_handler(int sig) {
    longjmp(g_env, 1);
}

int main() {
    signal(SIGSEGV, sig_handler);
    signal(SIGBUS, sig_handler);

    uint64_t tpidr;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tpidr));
    printf("tpidr_el0: 0x%lx\n", tpidr);

    pthread_key_t key;
    int rc = pthread_key_create(&key, NULL);
    printf("pthread_key_create returned %d, key = %d\n", rc, (int)key);
    
    void* test_val = (void*)0x1122334455667788ULL;
    pthread_setspecific(key, test_val);
    void* got = pthread_getspecific(key);
    printf("pthread_getspecific returned %p\n", got);

    uint64_t* base = (uint64_t*)(tpidr & ~7ULL);
    for (int i = 0; i < 256; ++i) {
        if (setjmp(g_env) == 0) {
            if (base[i] == (uint64_t)test_val) {
                printf("Found test_val at base[+%d] (offset +%ld bytes / +0x%lx)!\n", 
                       i, (long)(i * sizeof(uint64_t)), (unsigned long)(i * sizeof(uint64_t)));
            }
        }
    }
    for (int i = 1; i < 256; ++i) {
        if (setjmp(g_env) == 0) {
            if (base[-i] == (uint64_t)test_val) {
                printf("Found test_val at base[-%d] (offset -%ld bytes / -0x%lx)!\n", 
                       i, (long)(i * sizeof(uint64_t)), (unsigned long)(i * sizeof(uint64_t)));
            }
        }
    }
    printf("Probe complete.\n");
    return 0;
}
