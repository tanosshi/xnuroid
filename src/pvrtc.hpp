// PVRTC1 decoder (4 bpp and 2 bpp) to RGBA8, for GPUs without IMG_texture_compression_pvrtc.
//
// A texture is a grid of 64-bit blocks (4x4 pixels at 4 bpp, 8x4 at 2 bpp) in Morton order. Each block holds two
// low-resolution colours A and B and per-pixel modulation; a pixel's A/B are the bilinear blend of the four block
// colours around it (block colours sit at block centres), then mixed by its modulation weight.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pvrtc {

// Decodes a w x h level (power-of-two sizes; levels smaller than the minimum block grid are padded as stored).
// Rows are spread over the CPU cores. Returns false if src is too short.
bool decode(const uint8_t* src, size_t src_len, int w, int h, bool two_bpp, std::vector<uint8_t>& out);

}  // namespace pvrtc
