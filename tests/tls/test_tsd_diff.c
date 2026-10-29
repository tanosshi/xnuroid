#include <stdio.h>
#include <stdint.h>
#include <pthread.h>

__thread uint64_t g_apple_tsd[512];

static uint64_t get_tpidr() {
    uint64_t v;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(v));
    return v;
}

void* thread_func(void* arg) {
    long id = (long)arg;
    uint64_t tpidr = get_tpidr();
    uint64_t tsd_addr = (uint64_t)&g_apple_tsd[0];
    int64_t diff = (int64_t)tsd_addr - (int64_t)tpidr;
    printf("Thread %ld: tpidr=0x%lx, tsd=0x%lx, diff=%ld (0x%lx)\n", id, tpidr, tsd_addr, diff, diff);
    g_apple_tsd[10] = 0x12340000 + id;
    if (g_apple_tsd[10] != 0x12340000 + id) {
        printf("Thread %ld: error writing TSD!\n", id);
    }
    return NULL;
}

int main() {
    thread_func((void*)0);
    pthread_t t1, t2;
    pthread_create(&t1, NULL, thread_func, (void*)1);
    pthread_create(&t2, NULL, thread_func, (void*)2);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    return 0;
}
