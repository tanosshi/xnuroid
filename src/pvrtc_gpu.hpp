// PVRTC1 4 bpp decoding on the GPU: the compressed blocks go into a shader storage buffer and a GLES 3.1 compute
// shader writes the RGBA8 level with imageStore. Same rules as the CPU decoder (pvrtc.cpp): bilinear A/B colours
// from the four surrounding block centres, the pixel's own block's 2-bit modulation, punch-through alpha mode.
// Included once by mtl_gles.hpp.
#pragma once

#include <mutex>

namespace mtlgl {

inline const char* const kPvrtcCompute = R"(#version 310 es
layout(local_size_x = 8, local_size_y = 8) in;
layout(std430, binding = 0) readonly buffer Blocks { uvec2 blocks[]; };
layout(rgba8, binding = 0) writeonly uniform highp image2D dst;
uniform ivec2 u_blocks;   // blocks per row / column (powers of two)
uniform ivec2 u_size;     // level size in pixels
uniform int u_minbits;    // log2(min(blocks x, blocks y))

uint morton(uint x, uint y) {
    uint m = 0u;
    for (int i = 0; i < u_minbits; ++i) {
        m |= ((y >> uint(i)) & 1u) << uint(2 * i);
        m |= ((x >> uint(i)) & 1u) << uint(2 * i + 1);
    }
    uint rest = (u_blocks.x > u_blocks.y ? x : y) >> uint(u_minbits);
    return m | (rest << uint(2 * u_minbits));
}
uvec2 block(ivec2 b) {
    ivec2 w = b & (u_blocks - 1);
    return blocks[morton(uint(w.x), uint(w.y))];
}
ivec4 color_a(uint c) {
    if ((c & 0x8000u) != 0u) {
        int r = int((c >> 10) & 31u), g = int((c >> 5) & 31u), b = int(((c >> 1) & 15u) << 1 | ((c >> 4) & 1u));
        return ivec4(r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2, 255);
    }
    int a = int(((c >> 12) & 7u) << 1), r = int((c >> 8) & 15u), g = int((c >> 4) & 15u), b = int(((c >> 1) & 7u) << 1 | ((c >> 3) & 1u));
    return ivec4(r * 17, g * 17, b * 17, a * 17);
}
ivec4 color_b(uint c) {
    c >>= 16;
    if ((c & 0x8000u) != 0u) {
        int r = int((c >> 10) & 31u), g = int((c >> 5) & 31u), b = int(c & 31u);
        return ivec4(r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2, 255);
    }
    int a = int(((c >> 12) & 7u) << 1), r = int((c >> 8) & 15u), g = int((c >> 4) & 15u), b = int(c & 15u);
    return ivec4(r * 17, g * 17, b * 17, a * 17);
}
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= u_size.x || p.y >= u_size.y) return;
    ivec2 g = p - 2 + u_blocks * 4;
    ivec2 t = g >> 2, f = g & 3;
    uvec2 q0 = block(t), q1 = block(t + ivec2(1, 0)), q2 = block(t + ivec2(0, 1)), q3 = block(t + ivec2(1, 1));
    int w00 = (4 - f.x) * (4 - f.y), w01 = f.x * (4 - f.y), w10 = (4 - f.x) * f.y, w11 = f.x * f.y;
    ivec4 ca = (color_a(q0.y) * w00 + color_a(q1.y) * w01 + color_a(q2.y) * w10 + color_a(q3.y) * w11) >> 4;
    ivec4 cb = (color_b(q0.y) * w00 + color_b(q1.y) * w01 + color_b(q2.y) * w10 + color_b(q3.y) * w11) >> 4;
    uvec2 own = block(p >> 2);
    uint v = (own.x >> uint(2 * ((p.y & 3) * 4 + (p.x & 3)))) & 3u;
    bool punch = (own.y & 1u) != 0u;
    int weight = punch ? (v == 0u ? 0 : v == 3u ? 8 : 4) : (v == 0u ? 0 : v == 1u ? 3 : v == 2u ? 5 : 8);
    ivec4 c = clamp((ca * (8 - weight) + cb * weight) >> 3, 0, 255);
    if (punch && v == 2u) c.a = 0;
    imageStore(dst, p, vec4(c) / 255.0);
}
)";

// Decodes a 4 bpp level into tex (RGBA8 storage). False when compute is unavailable; the caller then uses the CPU.
inline bool pvrtc_gpu_decode(GLuint tex, GLenum target, int level, int slice, const uint8_t* src, int w, int h) {
    static std::mutex mu;
    static GLuint prog = 0;
    static bool failed = false;
    static GLint loc_blocks = -1, loc_size = -1, loc_minbits = -1;
    {
        std::lock_guard<std::mutex> l(mu);
        if (failed) return false;
        if (!prog) {
            GLuint s = glCreateShader(GL_COMPUTE_SHADER);
            glShaderSource(s, 1, &kPvrtcCompute, nullptr);
            glCompileShader(s);
            GLint ok = 0;
            glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
            if (ok) {
                prog = glCreateProgram();
                glAttachShader(prog, s);
                glLinkProgram(prog);
                glGetProgramiv(prog, GL_LINK_STATUS, &ok);
            }
            if (!ok) {
                char log[1024] = {};
                glGetShaderInfoLog(s, sizeof log, nullptr, log);
                fprintf(stderr, "[MTLGL] PVRTC compute shader unavailable: %s\n", log);
                failed = true;
                return false;
            }
            glDeleteShader(s);
            loc_blocks = glGetUniformLocation(prog, "u_blocks");
            loc_size = glGetUniformLocation(prog, "u_size");
            loc_minbits = glGetUniformLocation(prog, "u_minbits");
        }
    }
    const int bx = std::max(w, 8) / 4, by = std::max(h, 8) / 4;
    if ((bx & (bx - 1)) || (by & (by - 1))) return false;   // PVRTC levels are powers of two
    int minbits = 0;
    while ((1 << (minbits + 1)) <= std::min(bx, by)) ++minbits;
    thread_local GLuint ssbo = 0;
    thread_local size_t ssbo_size = 0;
    const size_t bytes = static_cast<size_t>(bx) * by * 8;
    if (!ssbo) glGenBuffers(1, &ssbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
    if (bytes > ssbo_size) {
        glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(bytes), src, GL_STREAM_DRAW);
        ssbo_size = bytes;
    } else {
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(bytes), src);
    }
    glUseProgram(prog);
    glUniform2i(loc_blocks, bx, by);
    glUniform2i(loc_size, w, h);
    glUniform1i(loc_minbits, minbits);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo);
    const bool layered = target != GL_TEXTURE_2D;
    glBindImageTexture(0, tex, level, GL_FALSE, layered ? slice : 0, GL_WRITE_ONLY, GL_RGBA8);
    glDispatchCompute(static_cast<GLuint>((w + 7) / 8), static_cast<GLuint>((h + 7) / 8), 1);
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glUseProgram(0);
    return glGetError() == GL_NO_ERROR;
}

}  // namespace mtlgl
