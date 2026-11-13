// armv7: load a 32-bit iOS image into the low arena, apply its classic dyld fixups with 4-byte slots, and
// execute its entry point under a small ARM interpreter until it calls an imported symbol.
#pragma once

#include <cstddef>
#include <cstdint>

namespace armv7 {

// Returns 0 when guest code ran and reached an imported symbol, 1 on a stop the interpreter could not handle.
int run_image(uint8_t* data, size_t size, const char* path);

}  // namespace armv7
