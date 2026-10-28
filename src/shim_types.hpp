#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Uniform calling frame matching Apple ARM64 ABI
struct CallFrame {
    uint64_t x[8];     // X0 - X7
    uint64_t d[8];     // D0 - D7 (FP/SIMD registers)
    uint64_t sp;       // Stack Pointer at call site
    uint64_t lr;       // Link Register (return address)
    uint64_t ret_x0;   // Return value in X0
    uint64_t ret_x1;   // Return value in X1

    // Memory accessor for reading arguments passed on stack (e.g. variadics)
    // On Windows/Unicorn: points to a memory reader function.
    // On Android: points directly to memory at `sp`.
    const uint8_t* stack_mem{};

    template <typename T>
    T read_stack(size_t byte_offset) const {
        T val{};
        if (stack_mem) {
            std::memcpy(&val, stack_mem + byte_offset, sizeof(T));
        }
        return val;
    }
};

typedef void (*ShimHandler)(CallFrame& frame, void* context);
