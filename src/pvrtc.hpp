// PVRTC1 decoder (4 bpp and 2 bpp) to RGBA8, for GPUs without IMG_texture_compression_pvrtc.
//
// A texture is a grid of 64-bit blocks (4x4 pixels at 4 bpp, 8x4 at 2 bpp) in Morton order. Each block holds two
// low-resolution colours A and B and per-pixel modulation; a pixel's A/B are the bilinear blend of the four block
// colours around it (block colours sit at block centres), then mixed by its modulation weight.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace pvrtc {

struct Rgba { int r, g, b, a; };

// 8-bit channels from the 15-bit colour fields (opaque RGB554/555 or translucent ARGB3443/3444).
inline Rgba color_a(uint32_t c) {
    if (c & 0x8000) {
        int r = (c >> 10) & 31, g = (c >> 5) & 31, b = ((c >> 1) & 15) << 1 | ((c >> 4) & 1);
        return {r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2, 255};
    }
    int a = ((c >> 12) & 7) << 1, r = (c >> 8) & 15, g = (c >> 4) & 15, b = ((c >> 1) & 7) << 1 | ((c >> 3) & 1);
    return {r * 17, g * 17, b * 17, a * 17};
}
inline Rgba color_b(uint32_t c) {
    c >>= 16;
    if (c & 0x8000) {
        int r = (c >> 10) & 31, g = (c >> 5) & 31, b = c & 31;
        return {r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2, 255};
    }
    int a = ((c >> 12) & 7) << 1, r = (c >> 8) & 15, g = (c >> 4) & 15, b = c & 15;
    return {r * 17, g * 17, b * 17, a * 17};
}

// Block index of block (x, y) in a bx * by grid: Morton interleave over the smaller dimension, the rest appended.
inline uint32_t morton(uint32_t x, uint32_t y, uint32_t bx, uint32_t by) {
    uint32_t mind = std::min(bx, by), out = 0, shift = 0;
    for (uint32_t bit = 1; bit < mind; bit <<= 1, ++shift) {
        if (y & bit) out |= 1u << (2 * shift);
        if (x & bit) out |= 1u << (2 * shift + 1);
    }
    uint32_t rest = (bx > by ? x : y) >> shift;
    return out | rest << (2 * shift);
}

// Decodes a w x h level (power-of-two sizes; smaller than the minimum block area is padded as stored).
inline bool decode(const uint8_t* src, size_t src_len, int w, int h, bool two_bpp, std::vector<uint8_t>& out) {
    const int bw = two_bpp ? 8 : 4, bh = 4;
    const uint32_t bx = static_cast<uint32_t>(std::max(w, bw * 2) / bw), by = static_cast<uint32_t>(std::max(h, bh * 2) / bh);
    if (src_len < static_cast<size_t>(bx) * by * 8) return false;
    auto word = [&](uint32_t x, uint32_t y, uint32_t& mod, uint32_t& col) {
        const uint8_t* p = src + static_cast<size_t>(morton(x % bx, y % by, bx, by)) * 8;
        std::memcpy(&mod, p, 4);
        std::memcpy(&col, p + 4, 4);
    };
    out.assign(static_cast<size_t>(w) * h * 4, 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            // the four blocks whose centres surround the pixel
            int gx = x - bw / 2 + static_cast<int>(bx) * bw, gy = y - bh / 2 + static_cast<int>(by) * bh;
            uint32_t x0 = static_cast<uint32_t>(gx / bw), y0 = static_cast<uint32_t>(gy / bh);
            int fx = gx % bw, fy = gy % bh;
            Rgba ca[4], cb[4];
            for (int k = 0; k < 4; ++k) {
                uint32_t m, c;
                word(x0 + (k & 1), y0 + (k >> 1), m, c);
                ca[k] = color_a(c);
                cb[k] = color_b(c);
            }
            auto bilerp = [&](const Rgba* q, int Rgba::*ch) {
                int top = (q[0].*ch) * (bw - fx) + (q[1].*ch) * fx, bot = (q[2].*ch) * (bw - fx) + (q[3].*ch) * fx;
                return (top * (bh - fy) + bot * fy) / (bw * bh);
            };
            // modulation of the pixel's own block
            uint32_t mod, col;
            word(static_cast<uint32_t>(x / bw), static_cast<uint32_t>(y / bh), mod, col);
            int px = x % bw, py = y % bh, weight = 0;
            bool punch = false;
            if (!two_bpp) {
                uint32_t v = (mod >> (2 * (py * 4 + px))) & 3;
                if (col & 1) { static const int wm[4] = {0, 4, 4, 8}; weight = wm[v]; punch = v == 2; }
                else { static const int ws[4] = {0, 3, 5, 8}; weight = ws[v]; }
            } else if (!(col & 1)) {
                weight = ((mod >> (py * 8 + px)) & 1) ? 8 : 0;
            } else {
                // 2 bpp interpolated modes: stored pixels hold 2-bit values on a checkerboard; others average neighbours
                static const int ws[4] = {0, 3, 5, 8};
                auto stored = [&](int sx, int sy) {
                    uint32_t m2, c2;
                    int ax = x - px + sx, ay = y - py + sy;
                    word(static_cast<uint32_t>((ax + static_cast<int>(bx) * bw) / bw), static_cast<uint32_t>((ay + static_cast<int>(by) * bh) / bh), m2, c2);
                    int lx = ((ax % bw) + bw) % bw, ly = ((ay % bh) + bh) % bh;
                    uint32_t idx = static_cast<uint32_t>((ly * 8 + lx) >> 1);
                    return ws[(m2 >> (2 * idx)) & 3];
                };
                if (((px ^ py) & 1) == 0) weight = stored(px, py);
                else weight = (stored(px - 1, py) + stored(px + 1, py) + stored(px, py - 1) + stored(px, py + 1) + 2) / 4;
            }
            uint8_t* o = &out[(static_cast<size_t>(y) * w + x) * 4];
            int Rgba::*chans[4] = {&Rgba::r, &Rgba::g, &Rgba::b, &Rgba::a};
            for (int c = 0; c < 4; ++c) {
                int a = bilerp(ca, chans[c]), b = bilerp(cb, chans[c]);
                o[c] = static_cast<uint8_t>(std::clamp((a * (8 - weight) + b * weight) / 8, 0, 255));
            }
            if (punch) o[3] = 0;
        }
    }
    return true;
}

}  // namespace pvrtc
