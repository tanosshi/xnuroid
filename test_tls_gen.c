#include <stdio.h>
#include <stdint.h>

__thread uint64_t g_apple_tsd[512];

uint64_t* get_apple_tsd() {
    return &g_apple_tsd[0];
}

int main() {
    printf("get_apple_tsd: %p\n", (void*)get_apple_tsd());
    return 0;
}
