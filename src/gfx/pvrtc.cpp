#include "pvrtc.hpp"

#include <algorithm>
#include <cstring>
#include <thread>

namespace pvrtc {
namespace {

struct Rgba { int r, g, b, a; };

// 8-bit channels from the 15-bit colour fields (opaque RGB554/555 or translucent ARGB3443/3444).
Rgba color_a(uint32_t c) {
    if (c & 0x8000) {
        int r = (c >> 10) & 31, g = (c >> 5) & 31, b = ((c >> 1) & 15) << 1 | ((c >> 4) & 1);
        return {r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2, 255};
    }
    int a = ((c >> 12) & 7) << 1, r = (c >> 8) & 15, g = (c >> 4) & 15, b = ((c >> 1) & 7) << 1 | ((c >> 3) & 1);
    return {r * 17, g * 17, b * 17, a * 17};
}
Rgba color_b(uint32_t c) {
    c >>= 16;
    if (c & 0x8000) {
        int r = (c >> 10) & 31, g = (c >> 5) & 31, b = c & 31;
        return {r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2, 255};
    }
    int a = ((c >> 12) & 7) << 1, r = (c >> 8) & 15, g = (c >> 4) & 15, b = c & 15;
    return {r * 17, g * 17, b * 17, a * 17};
}

// Block index of block (x, y) in a bx * by grid: Morton interleave over the smaller dimension, the rest appended.
uint32_t morton(uint32_t x, uint32_t y, uint32_t bx, uint32_t by) {
    uint32_t mind = std::min(bx, by), out = 0, shift = 0;
    for (uint32_t bit = 1; bit < mind; bit <<= 1, ++shift) {
        if (y & bit) out |= 1u << (2 * shift);
        if (x & bit) out |= 1u << (2 * shift + 1);
    }
    uint32_t rest = (bx > by ? x : y) >> shift;
    return out | rest << (2 * shift);
}

struct Blocks {
    int bx, by;
    std::vector<Rgba> a, b;          // raster order
    std::vector<uint32_t> mod, col;
    size_t at(int x, int y) const {   // wrapping block coordinates
        int ux = ((x % bx) + bx) % bx, uy = ((y % by) + by) % by;
        return static_cast<size_t>(uy) * bx + ux;
    }
};

inline uint8_t mix(int ca, int cb, int weight) {
    int v = (ca * (8 - weight) + cb * weight) >> 3;
    return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v);
}

// 4 bpp, cell by cell: a cell is the 4x4 pixel square between four block centres, so its four colour sources are
// fixed and the bilinear weights depend only on the position inside the cell.
void decode_cells_4bpp(const Blocks& k, int w, int h, int cy0, int cy1, uint8_t* out) {
    static const int w4[4] = {0, 3, 5, 8}, w4p[4] = {0, 4, 4, 8};
    const int W = k.bx * 4, H = k.by * 4;
    for (int cy = cy0; cy < cy1; ++cy) {
        for (int cx = 0; cx < k.bx; ++cx) {
            const size_t q[4] = {k.at(cx, cy), k.at(cx + 1, cy), k.at(cx, cy + 1), k.at(cx + 1, cy + 1)};
            for (int fy = 0; fy < 4; ++fy) {
                const int y = (cy * 4 + 2 + fy) % H;
                if (y >= h) continue;
                const int own_y = fy < 2 ? 0 : 2, py = y & 3;
                for (int fx = 0; fx < 4; ++fx) {
                    const int x = (cx * 4 + 2 + fx) % W;
                    if (x >= w) continue;
                    const int w00 = (4 - fx) * (4 - fy), w01 = fx * (4 - fy), w10 = (4 - fx) * fy, w11 = fx * fy;
                    const size_t own = q[own_y + (fx < 2 ? 0 : 1)];
                    const uint32_t v = (k.mod[own] >> (2 * (py * 4 + (x & 3)))) & 3;
                    const bool punch_mode = k.col[own] & 1;
                    const int weight = punch_mode ? w4p[v] : w4[v];
                    const Rgba &a0 = k.a[q[0]], &a1 = k.a[q[1]], &a2 = k.a[q[2]], &a3 = k.a[q[3]];
                    const Rgba &b0 = k.b[q[0]], &b1 = k.b[q[1]], &b2 = k.b[q[2]], &b3 = k.b[q[3]];
                    uint8_t* o = out + (static_cast<size_t>(y) * w + x) * 4;
                    o[0] = mix((a0.r * w00 + a1.r * w01 + a2.r * w10 + a3.r * w11) >> 4, (b0.r * w00 + b1.r * w01 + b2.r * w10 + b3.r * w11) >> 4, weight);
                    o[1] = mix((a0.g * w00 + a1.g * w01 + a2.g * w10 + a3.g * w11) >> 4, (b0.g * w00 + b1.g * w01 + b2.g * w10 + b3.g * w11) >> 4, weight);
                    o[2] = mix((a0.b * w00 + a1.b * w01 + a2.b * w10 + a3.b * w11) >> 4, (b0.b * w00 + b1.b * w01 + b2.b * w10 + b3.b * w11) >> 4, weight);
                    o[3] = punch_mode && v == 2 ? 0
                                                : mix((a0.a * w00 + a1.a * w01 + a2.a * w10 + a3.a * w11) >> 4,
                                                      (b0.a * w00 + b1.a * w01 + b2.a * w10 + b3.a * w11) >> 4, weight);
                }
            }
        }
    }
}

// 2 bpp (8x4 blocks), pixel by pixel.
void decode_rows_2bpp(const Blocks& k, int w, int y0, int y1, uint8_t* out) {
    static const int w4[4] = {0, 3, 5, 8};
    const int bw = 8, bh = 4;
    for (int y = y0; y < y1; ++y) {
        const int gy = y - bh / 2 + k.by * bh, ty = gy / bh, fy = gy % bh, py = y % bh;
        for (int x = 0; x < w; ++x) {
            const int gx = x - bw / 2 + k.bx * bw, tx = gx / bw, fx = gx % bw;
            const size_t q0 = k.at(tx, ty), q1 = k.at(tx + 1, ty), q2 = k.at(tx, ty + 1), q3 = k.at(tx + 1, ty + 1);
            const int w00 = (bw - fx) * (bh - fy), w01 = fx * (bh - fy), w10 = (bw - fx) * fy, w11 = fx * fy, area = bw * bh;
            const size_t own = k.at(x / bw, y / bh);
            const int px = x % bw;
            int weight;
            if (!(k.col[own] & 1)) {
                weight = ((k.mod[own] >> (py * 8 + px)) & 1) ? 8 : 0;
            } else {
                // interpolated: 2-bit values on a checkerboard, the other pixels average their neighbours
                auto stored = [&](int sx, int sy) {
                    int ax = x - px + sx, ay = y - py + sy;
                    size_t blk = k.at(ax >= 0 ? ax / bw : (ax - bw + 1) / bw, ay >= 0 ? ay / bh : (ay - bh + 1) / bh);
                    int lx = ((ax % bw) + bw) % bw, ly = ((ay % bh) + bh) % bh;
                    return w4[(k.mod[blk] >> (2 * ((ly * 8 + lx) >> 1))) & 3];
                };
                if (((px ^ py) & 1) == 0) weight = stored(px, py);
                else weight = (stored(px - 1, py) + stored(px + 1, py) + stored(px, py - 1) + stored(px, py + 1) + 2) / 4;
            }
            uint8_t* o = out + (static_cast<size_t>(y) * w + x) * 4;
            const Rgba &a0 = k.a[q0], &a1 = k.a[q1], &a2 = k.a[q2], &a3 = k.a[q3];
            const Rgba &b0 = k.b[q0], &b1 = k.b[q1], &b2 = k.b[q2], &b3 = k.b[q3];
            o[0] = mix((a0.r * w00 + a1.r * w01 + a2.r * w10 + a3.r * w11) / area, (b0.r * w00 + b1.r * w01 + b2.r * w10 + b3.r * w11) / area, weight);
            o[1] = mix((a0.g * w00 + a1.g * w01 + a2.g * w10 + a3.g * w11) / area, (b0.g * w00 + b1.g * w01 + b2.g * w10 + b3.g * w11) / area, weight);
            o[2] = mix((a0.b * w00 + a1.b * w01 + a2.b * w10 + a3.b * w11) / area, (b0.b * w00 + b1.b * w01 + b2.b * w10 + b3.b * w11) / area, weight);
            o[3] = mix((a0.a * w00 + a1.a * w01 + a2.a * w10 + a3.a * w11) / area, (b0.a * w00 + b1.a * w01 + b2.a * w10 + b3.a * w11) / area, weight);
        }
    }
}

}  // namespace

bool decode(const uint8_t* src, size_t src_len, int w, int h, bool two_bpp, std::vector<uint8_t>& out) {
    const int bw = two_bpp ? 8 : 4, bh = 4;
    Blocks k;
    k.bx = std::max(w, bw * 2) / bw;
    k.by = std::max(h, bh * 2) / bh;
    const size_t n = static_cast<size_t>(k.bx) * k.by;
    if (src_len < n * 8) return false;
    k.a.resize(n); k.b.resize(n); k.mod.resize(n); k.col.resize(n);
    for (int y = 0; y < k.by; ++y)
        for (int x = 0; x < k.bx; ++x) {
            const uint8_t* p = src + static_cast<size_t>(morton(static_cast<uint32_t>(x), static_cast<uint32_t>(y), static_cast<uint32_t>(k.bx), static_cast<uint32_t>(k.by))) * 8;
            size_t i = static_cast<size_t>(y) * k.bx + x;
            std::memcpy(&k.mod[i], p, 4);
            std::memcpy(&k.col[i], p + 4, 4);
            k.a[i] = color_a(k.col[i]);
            k.b[i] = color_b(k.col[i]);
        }
    out.resize(static_cast<size_t>(w) * h * 4);
    // work unit: rows of cells (4 bpp) or pixel rows (2 bpp)
    const int units = two_bpp ? h : k.by;
    const int threads = std::max(1, std::min<int>(static_cast<int>(std::thread::hardware_concurrency()), units / 16));
    auto run = [&](int u0, int u1) {
        if (two_bpp) decode_rows_2bpp(k, w, u0, u1, out.data());
        else decode_cells_4bpp(k, w, h, u0, u1, out.data());
    };
    if (threads == 1) {
        run(0, units);
        return true;
    }
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) pool.emplace_back(run, units * t / threads, units * (t + 1) / threads);
    for (std::thread& th : pool) th.join();
    return true;
}

}  // namespace pvrtc
