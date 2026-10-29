// Metal on OpenGL ES 3.2: the fake Metal property bags (fake_foundation.hpp) get real GL objects behind them.
//
// Descriptors stay property bags; the objects made from them carry a GL handle (props "#gl" -> C++ struct):
//   MTLTexture -> GL texture (created lazily, so drawable textures work too), MTLSamplerState -> GL sampler,
//   MTLRenderPipelineState -> linked program (MSL translated by msl_glsl.cpp) + vertex layout + blend state,
//   MTLDepthStencilState -> captured state, MTLRenderCommandEncoder -> FBO + bound resources; draws execute at once.
// MTLBuffers remain CPU memory; each draw streams the ranges it reads into per-thread GL ring buffers, which also
// matches Metal's "CPU writes are visible to the next command" model without tracking writes.
// Included once by android_loader.cpp after fake_foundation.hpp.
#pragma once

#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>

#include <algorithm>
#include <map>

#include "msl_glsl.hpp"

namespace mtlgl {

// ---- formats ----
struct Fmt {
    GLenum internal = 0, format = 0, type = 0;
    bool compressed = false, bgra = false, depth = false, stencil = false, integer = false;
    int bw = 1, bh = 1, bytes = 4;   // block size and bytes per block (pixel for uncompressed)
};

inline bool format_of(uint64_t pf, Fmt& f) {
    f = Fmt();
    auto u = [&](GLenum i, GLenum fo, GLenum t, int b) { f.internal = i; f.format = fo; f.type = t; f.bytes = b; return true; };
    switch (pf) {
        case 1: return u(GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1);                // A8Unorm (swizzled at creation)
        case 10: case 11: return u(GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1);
        case 12: return u(GL_R8_SNORM, GL_RED, GL_BYTE, 1);
        case 13: f.integer = true; return u(GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, 1);
        case 14: f.integer = true; return u(GL_R8I, GL_RED_INTEGER, GL_BYTE, 1);
        case 23: f.integer = true; return u(GL_R16UI, GL_RED_INTEGER, GL_UNSIGNED_SHORT, 2);
        case 24: f.integer = true; return u(GL_R16I, GL_RED_INTEGER, GL_SHORT, 2);
        case 25: return u(GL_R16F, GL_RED, GL_HALF_FLOAT, 2);
        case 30: case 31: return u(GL_RG8, GL_RG, GL_UNSIGNED_BYTE, 2);
        case 32: return u(GL_RG8_SNORM, GL_RG, GL_BYTE, 2);
        case 33: f.integer = true; return u(GL_RG8UI, GL_RG_INTEGER, GL_UNSIGNED_BYTE, 2);
        case 34: f.integer = true; return u(GL_RG8I, GL_RG_INTEGER, GL_BYTE, 2);
        case 53: f.integer = true; return u(GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, 4);
        case 54: f.integer = true; return u(GL_R32I, GL_RED_INTEGER, GL_INT, 4);
        case 55: return u(GL_R32F, GL_RED, GL_FLOAT, 4);
        case 63: f.integer = true; return u(GL_RG16UI, GL_RG_INTEGER, GL_UNSIGNED_SHORT, 4);
        case 64: f.integer = true; return u(GL_RG16I, GL_RG_INTEGER, GL_SHORT, 4);
        case 65: return u(GL_RG16F, GL_RG, GL_HALF_FLOAT, 4);
        case 70: return u(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4);
        case 71: return u(GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, 4);
        case 72: return u(GL_RGBA8_SNORM, GL_RGBA, GL_BYTE, 4);
        case 73: f.integer = true; return u(GL_RGBA8UI, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, 4);
        case 74: f.integer = true; return u(GL_RGBA8I, GL_RGBA_INTEGER, GL_BYTE, 4);
        case 80: f.bgra = true; return u(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4);
        case 81: f.bgra = true; return u(GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, 4);
        case 90: return u(GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, 4);
        case 91: f.integer = true; return u(GL_RGB10_A2UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT_2_10_10_10_REV, 4);
        case 92: return u(GL_R11F_G11F_B10F, GL_RGB, GL_UNSIGNED_INT_10F_11F_11F_REV, 4);
        case 93: return u(GL_RGB9_E5, GL_RGB, GL_UNSIGNED_INT_5_9_9_9_REV, 4);
        case 103: f.integer = true; return u(GL_RG32UI, GL_RG_INTEGER, GL_UNSIGNED_INT, 8);
        case 104: f.integer = true; return u(GL_RG32I, GL_RG_INTEGER, GL_INT, 8);
        case 105: return u(GL_RG32F, GL_RG, GL_FLOAT, 8);
        case 113: f.integer = true; return u(GL_RGBA16UI, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, 8);
        case 114: f.integer = true; return u(GL_RGBA16I, GL_RGBA_INTEGER, GL_SHORT, 8);
        case 115: return u(GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, 8);
        case 123: f.integer = true; return u(GL_RGBA32UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT, 16);
        case 124: f.integer = true; return u(GL_RGBA32I, GL_RGBA_INTEGER, GL_INT, 16);
        case 125: return u(GL_RGBA32F, GL_RGBA, GL_FLOAT, 16);
        case 250: f.depth = true; return u(GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, 2);
        case 252: f.depth = true; return u(GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, 4);
        case 253: f.stencil = true; return u(GL_STENCIL_INDEX8, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, 1);
        case 255: f.depth = f.stencil = true; return u(GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, 4);
        case 260: f.depth = f.stencil = true; return u(GL_DEPTH32F_STENCIL8, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV, 8);
        default: break;
    }
    // ETC2 / EAC
    struct C { uint64_t pf; GLenum gl; int bytes; };
    static const C etc[] = {{170, GL_COMPRESSED_R11_EAC, 8}, {172, GL_COMPRESSED_SIGNED_R11_EAC, 8}, {174, GL_COMPRESSED_RG11_EAC, 16},
                            {176, GL_COMPRESSED_SIGNED_RG11_EAC, 16}, {178, GL_COMPRESSED_RGBA8_ETC2_EAC, 16},
                            {179, GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC, 16}, {180, GL_COMPRESSED_RGB8_ETC2, 8},
                            {181, GL_COMPRESSED_SRGB8_ETC2, 8}, {182, GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2, 8},
                            {183, GL_COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2, 8}};
    for (const C& c : etc)
        if (c.pf == pf) { f.compressed = true; f.internal = c.gl; f.bw = f.bh = 4; f.bytes = c.bytes; return true; }
    // ASTC: sRGB 186..200 and LDR 204..218 share the block-size order below (no 7x? sizes).
    static const int astc[][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {0, 0}, {8, 5}, {8, 6}, {8, 8}, {10, 5}, {10, 6}, {10, 8},
                                  {10, 10}, {12, 10}, {12, 12}};
    static const GLenum astc_ldr[] = {GL_COMPRESSED_RGBA_ASTC_4x4, GL_COMPRESSED_RGBA_ASTC_5x4, GL_COMPRESSED_RGBA_ASTC_5x5,
                                      GL_COMPRESSED_RGBA_ASTC_6x5, GL_COMPRESSED_RGBA_ASTC_6x6, 0, GL_COMPRESSED_RGBA_ASTC_8x5,
                                      GL_COMPRESSED_RGBA_ASTC_8x6, GL_COMPRESSED_RGBA_ASTC_8x8, GL_COMPRESSED_RGBA_ASTC_10x5,
                                      GL_COMPRESSED_RGBA_ASTC_10x6, GL_COMPRESSED_RGBA_ASTC_10x8, GL_COMPRESSED_RGBA_ASTC_10x10,
                                      GL_COMPRESSED_RGBA_ASTC_12x10, GL_COMPRESSED_RGBA_ASTC_12x12};
    static const GLenum astc_srgb[] = {GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4, GL_COMPRESSED_SRGB8_ALPHA8_ASTC_5x4,
                                       GL_COMPRESSED_SRGB8_ALPHA8_ASTC_5x5, GL_COMPRESSED_SRGB8_ALPHA8_ASTC_6x5,
                                       GL_COMPRESSED_SRGB8_ALPHA8_ASTC_6x6, 0, GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x5,
                                       GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x6, GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x8,
                                       GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x5, GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x6,
                                       GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x8, GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x10,
                                       GL_COMPRESSED_SRGB8_ALPHA8_ASTC_12x10, GL_COMPRESSED_SRGB8_ALPHA8_ASTC_12x12};
    int k = -1;
    bool srgb = false;
    if (pf >= 186 && pf <= 200) { k = static_cast<int>(pf - 186); srgb = true; }
    if (pf >= 204 && pf <= 218) k = static_cast<int>(pf - 204);
    if (k >= 0 && astc[k][0]) {
        f.compressed = true;
        f.internal = srgb ? astc_srgb[k] : astc_ldr[k];
        f.bw = astc[k][0];
        f.bh = astc[k][1];
        f.bytes = 16;
        return true;
    }
    return false;
}

// ---- objects ----
struct Tex {
    GLuint id = 0;
    GLenum target = GL_TEXTURE_2D;
    int w = 1, h = 1, d = 1, levels = 1, layers = 1, samples = 1;
    uint64_t pf = 0;
    Fmt f;
};
struct Sampler { GLuint id = 0; bool compare = false; };
struct StencilFace { GLenum func = GL_ALWAYS, fail = GL_KEEP, zfail = GL_KEEP, pass = GL_KEEP; GLuint read = 0xffffffff, write = 0xffffffff; };
struct DepthStencil {
    GLenum depth_func = GL_ALWAYS;
    bool depth_write = false, stencil = false;
    StencilFace front, back;
};
struct Attr { int loc; uint64_t format, offset, buffer; };
struct Layout { uint64_t stride = 0, step = 1, rate = 1; };
struct Blend {
    bool enabled = false;
    GLenum src_rgb = GL_ONE, dst_rgb = GL_ZERO, src_a = GL_ONE, dst_a = GL_ZERO, op_rgb = GL_FUNC_ADD, op_a = GL_FUNC_ADD;
    uint64_t mask = 15;
};
struct Pipeline {
    GLuint prog = 0;
    msl::Shader vs, fs;
    std::vector<Attr> attrs;
    std::map<uint64_t, Layout> layouts;
    Blend blend[8];
    bool alpha_to_coverage = false;
    std::vector<int> vs_units, fs_units;     // texture unit per sampler use
    std::vector<int> vs_bind, fs_bind;       // UBO binding per block
};
struct BoundBuf {
    FakeObjcObj* buf = nullptr;
    uint64_t offset = 0;
    std::vector<uint8_t> bytes;              // set*Bytes: copied data
    const uint8_t* data(size_t& avail) const {
        if (!bytes.empty()) { avail = bytes.size(); return bytes.data(); }
        if (!buf || !buf->mem || offset > buf->len) { avail = 0; return nullptr; }
        avail = buf->len - offset;
        return static_cast<const uint8_t*>(buf->mem) + offset;
    }
};
struct Encoder {
    Pipeline* pipe = nullptr;
    BoundBuf vbuf[32], fbuf[32];
    uint64_t vtex[32] = {}, ftex[32] = {}, vsamp[16] = {}, fsamp[16] = {};
    DepthStencil* ds = nullptr;
    GLuint ref_front = 0, ref_back = 0;
    uint64_t cull = 0, winding = 0;
    double depth_bias = 0, slope = 0;
    int rt_w = 0, rt_h = 0;
    bool valid = false;
    struct Resolve { GLuint src_tex, dst_tex; int w, h; GLenum attach; } resolve[8];
    int nresolve = 0;
    int draws = 0;
};

inline std::mutex g_gl_mu;
inline uint64_t g_frame_no = 0;
// LOADER_GL_TRACE=<n>: log render passes and draws for the first n presented frames.
inline bool tracing() {
    static const uint64_t n = getenv("LOADER_GL_TRACE") ? strtoull(getenv("LOADER_GL_TRACE"), nullptr, 10) : 0;
    return g_frame_no < n;
}

template <class T> T* gl_obj(FakeObjcObj* o) { return o ? reinterpret_cast<T*>(o->props.count("#gl") ? o->props["#gl"] : 0) : nullptr; }
inline uint64_t prop(FakeObjcObj* o, const char* k, uint64_t def) {
    if (!o) return def;
    auto it = o->props.find(k);
    return it == o->props.end() ? def : it->second;
}
inline double propd(FakeObjcObj* o, const std::string& k, double def) {
    if (!o) return def;
    auto it = o->props.find(k + "#d");
    if (it == o->props.end()) return def;
    double d;
    std::memcpy(&d, &it->second, 8);
    return d;
}
inline FakeObjcObj* child(FakeObjcObj* o, const char* prop_name, int index) {
    if (!o) return nullptr;
    FakeObjcObj* arr = fake_lookup(prop(o, prop_name, 0));
    if (!arr) return nullptr;
    return fake_lookup(prop(arr, ("#obj" + std::to_string(index)).c_str(), 0));
}
inline bool gles_enabled() {
    static const bool on = [] {
        const char* e = getenv("LOADER_GLES");
        return getenv("LOADER_ENGINE") && !(e && e[0] == '0');
    }();
    return on;
}

// ---- per-thread GL state ----
struct Ring {
    GLuint buf = 0;
    GLenum target = 0;
    size_t size = 0, head = 0;
    size_t push(const void* data, size_t len, size_t align) {
        if (!buf) {
            glGenBuffers(1, &buf);
            glBindBuffer(target, buf);
            glBufferData(target, static_cast<GLsizeiptr>(size), nullptr, GL_STREAM_DRAW);
        }
        glBindBuffer(target, buf);
        head = (head + align - 1) / align * align;
        if (head + len > size) {
            glBufferData(target, static_cast<GLsizeiptr>(size), nullptr, GL_STREAM_DRAW);   // orphan
            head = 0;
        }
        size_t at = head;
        if (len) glBufferSubData(target, static_cast<GLintptr>(at), static_cast<GLsizeiptr>(len), data);
        head += len;
        return at;
    }
};
struct ThreadGL {
    bool init = false;
    Ring vb{0, GL_ARRAY_BUFFER, 16u << 20}, ib{0, GL_ELEMENT_ARRAY_BUFFER, 4u << 20}, ub{0, GL_UNIFORM_BUFFER, 8u << 20};
    GLuint vao = 0, fbo = 0, read_fbo = 0, dummy2d = 0, dummy_sampler = 0;
    GLint ubo_align = 256, max_draw_buffers = 4;
    uint32_t enabled_attribs = 0;
};
inline ThreadGL& tgl() {
    static thread_local ThreadGL t;
    if (!t.init && gles::ensure_context()) {
        t.init = true;
        glGenVertexArrays(1, &t.vao);
        glGenFramebuffers(1, &t.fbo);
        glGenFramebuffers(1, &t.read_fbo);
        glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &t.ubo_align);
        glGetIntegerv(GL_MAX_DRAW_BUFFERS, &t.max_draw_buffers);
        if (t.max_draw_buffers > 8) t.max_draw_buffers = 8;
        glGenTextures(1, &t.dummy2d);
        glBindTexture(GL_TEXTURE_2D, t.dummy2d);
        const uint8_t black[4] = {0, 0, 0, 255};
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 1, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, black);
        glGenSamplers(1, &t.dummy_sampler);
        glSamplerParameteri(t.dummy_sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glSamplerParameteri(t.dummy_sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        if (gles::has_extension("GL_EXT_clip_control")) {
            auto clip = reinterpret_cast<void (*)(GLenum, GLenum)>(eglGetProcAddress("glClipControlEXT"));
            if (clip) clip(GL_LOWER_LEFT_EXT, GL_ZERO_TO_ONE_EXT);
        }
        glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
    }
    return t;
}
inline bool clip_control() {
    static const bool c = gles::ensure_context() && gles::has_extension("GL_EXT_clip_control");
    return c;
}

// ---- textures ----
inline GLenum target_of(uint64_t type, uint64_t samples) {
    switch (type) {
        case 3: return GL_TEXTURE_2D_ARRAY;
        case 4: return GL_TEXTURE_2D_MULTISAMPLE;
        case 5: return GL_TEXTURE_CUBE_MAP;
        case 6: return GL_TEXTURE_CUBE_MAP_ARRAY;
        case 7: return GL_TEXTURE_3D;
        case 8: return GL_TEXTURE_2D_MULTISAMPLE_ARRAY;
        default: return samples > 1 ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;
    }
}

// The GL texture behind a Metal texture bag (created on first use from its descriptor properties).
inline Tex* texture(FakeObjcObj* o) {
    if (!o) return nullptr;
    if (Tex* t = gl_obj<Tex>(o)) return t;
    if (uint64_t src = prop(o, "#viewOf", 0)) return texture(fake_lookup(src));
    ThreadGL& g = tgl();
    if (!g.init) return nullptr;
    static std::mutex create_mu;   // two threads may realize the same texture
    std::lock_guard<std::mutex> l(create_mu);
    if (Tex* t = gl_obj<Tex>(o)) return t;
    auto* t = new Tex();
    t->pf = prop(o, "pixelFormat", 70);
    t->w = static_cast<int>(std::max<uint64_t>(1, prop(o, "width", 1)));
    t->h = static_cast<int>(std::max<uint64_t>(1, prop(o, "height", 1)));
    t->d = static_cast<int>(std::max<uint64_t>(1, prop(o, "depth", 1)));
    t->levels = static_cast<int>(std::max<uint64_t>(1, prop(o, "mipmapLevelCount", 1)));
    t->layers = static_cast<int>(std::max<uint64_t>(1, prop(o, "arrayLength", 1)));
    t->samples = static_cast<int>(std::max<uint64_t>(1, prop(o, "sampleCount", 1)));
    t->target = target_of(prop(o, "textureType", 2), t->samples);
    if (!format_of(t->pf, t->f)) {
        fprintf(stderr, "[MTLGL] unsupported pixelFormat %llu (%dx%d); using RGBA8\n", static_cast<unsigned long long>(t->pf), t->w, t->h);
        format_of(70, t->f);
    }
    glGenTextures(1, &t->id);
    glBindTexture(t->target, t->id);
    switch (t->target) {
        case GL_TEXTURE_2D: case GL_TEXTURE_CUBE_MAP: glTexStorage2D(t->target, t->levels, t->f.internal, t->w, t->h); break;
        case GL_TEXTURE_2D_ARRAY: glTexStorage3D(t->target, t->levels, t->f.internal, t->w, t->h, t->layers); break;
        case GL_TEXTURE_CUBE_MAP_ARRAY: glTexStorage3D(t->target, t->levels, t->f.internal, t->w, t->h, t->layers * 6); break;
        case GL_TEXTURE_3D: glTexStorage3D(t->target, t->levels, t->f.internal, t->w, t->h, t->d); break;
        case GL_TEXTURE_2D_MULTISAMPLE: glTexStorage2DMultisample(t->target, t->samples, t->f.internal, t->w, t->h, GL_TRUE); break;
        default: glTexStorage3DMultisample(t->target, t->samples, t->f.internal, t->w, t->h, t->layers, GL_TRUE); break;
    }
    if (t->pf == 1) {   // A8: alpha from the red channel
        glTexParameteri(t->target, GL_TEXTURE_SWIZZLE_R, GL_ZERO);
        glTexParameteri(t->target, GL_TEXTURE_SWIZZLE_G, GL_ZERO);
        glTexParameteri(t->target, GL_TEXTURE_SWIZZLE_B, GL_ZERO);
        glTexParameteri(t->target, GL_TEXTURE_SWIZZLE_A, GL_RED);
    }
    if (t->target != GL_TEXTURE_2D_MULTISAMPLE && t->target != GL_TEXTURE_2D_MULTISAMPLE_ARRAY)
        glTexParameteri(t->target, GL_TEXTURE_MAX_LEVEL, t->levels - 1);
    GLenum err = glGetError();
    if (err) fprintf(stderr, "[MTLGL] texture pf %llu %dx%dx%d levels %d target 0x%x: GL error 0x%x\n",
                     static_cast<unsigned long long>(t->pf), t->w, t->h, t->d, t->levels, t->target, err);
    o->props["#gl"] = reinterpret_cast<uint64_t>(t);
    gles::publish();
    return t;
}

// Uploads one region. bytes rows are bpr apart, images bpi apart (array slice / 3D depth handled by the caller).
inline void upload(Tex* t, int level, int slice, int x, int y, int z, int w, int h, int dz, const uint8_t* src, size_t bpr, size_t bpi) {
    if (!t || !src || w <= 0 || h <= 0) return;
    glBindTexture(t->target, t->id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (t->f.compressed) {
        int bx = (w + t->f.bw - 1) / t->f.bw, by = (h + t->f.bh - 1) / t->f.bh;
        size_t tight = static_cast<size_t>(bx) * t->f.bytes;
        std::vector<uint8_t> packed;
        const uint8_t* p = src;
        if (bpr && bpr != tight) {
            packed.resize(tight * by);
            for (int r = 0; r < by; ++r) std::memcpy(&packed[r * tight], src + r * bpr, tight);
            p = packed.data();
        }
        GLsizei size = static_cast<GLsizei>(tight * by);
        if (t->target == GL_TEXTURE_CUBE_MAP)
            glCompressedTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + slice, level, x, y, w, h, t->f.internal, size, p);
        else if (t->target == GL_TEXTURE_2D)
            glCompressedTexSubImage2D(GL_TEXTURE_2D, level, x, y, w, h, t->f.internal, size, p);
        else
            glCompressedTexSubImage3D(t->target, level, x, y, t->target == GL_TEXTURE_3D ? z : slice, w, h, 1, t->f.internal, size, p);
        return;
    }
    std::vector<uint8_t> swz;
    const uint8_t* p = src;
    size_t row = static_cast<size_t>(w) * t->f.bytes;
    if (t->f.bgra) {
        swz.resize(row * h * dz);
        for (int zz = 0; zz < dz; ++zz)
            for (int r = 0; r < h; ++r) {
                const uint8_t* s = src + zz * bpi + r * bpr;
                uint8_t* d = &swz[(zz * h + r) * row];
                for (int c = 0; c < w; ++c) { d[4 * c] = s[4 * c + 2]; d[4 * c + 1] = s[4 * c + 1]; d[4 * c + 2] = s[4 * c]; d[4 * c + 3] = s[4 * c + 3]; }
            }
        p = swz.data();
        bpr = row;
        bpi = row * h;
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, bpr ? static_cast<GLint>(bpr / t->f.bytes) : 0);
    glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, bpr && bpi ? static_cast<GLint>(bpi / bpr) : 0);
    if (t->target == GL_TEXTURE_CUBE_MAP)
        glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + slice, level, x, y, w, h, t->f.format, t->f.type, p);
    else if (t->target == GL_TEXTURE_2D)
        glTexSubImage2D(GL_TEXTURE_2D, level, x, y, w, h, t->f.format, t->f.type, p);
    else if (t->target == GL_TEXTURE_3D)
        glTexSubImage3D(t->target, level, x, y, z, w, h, dz, t->f.format, t->f.type, p);
    else
        glTexSubImage3D(t->target, level, x, y, slice, w, h, 1, t->f.format, t->f.type, p);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
}

// Attaches level/slice of t to the bound read or draw framebuffer.
inline void attach(GLenum fb_target, GLenum point, Tex* t, int level, int slice) {
    if (t->target == GL_TEXTURE_2D || t->target == GL_TEXTURE_2D_MULTISAMPLE)
        glFramebufferTexture2D(fb_target, point, t->target, t->id, level);
    else if (t->target == GL_TEXTURE_CUBE_MAP)
        glFramebufferTexture2D(fb_target, point, GL_TEXTURE_CUBE_MAP_POSITIVE_X + slice, t->id, level);
    else
        glFramebufferTextureLayer(fb_target, point, t->id, level, slice);
}

// Reads level 0 of t as RGBA8 rows top to bottom (Metal row order).
inline bool read_rgba(Tex* t, std::vector<uint8_t>& out) {
    ThreadGL& g = tgl();
    if (!g.init || !t || t->f.compressed || t->f.depth) return false;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g.read_fbo);
    attach(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, t, 0, 0);
    out.resize(static_cast<size_t>(t->w) * t->h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, t->w, t->h, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    return glGetError() == GL_NO_ERROR;
}

// ---- samplers / depth-stencil ----
inline GLenum compare_fn(uint64_t f) {
    static const GLenum m[] = {GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
    return f < 8 ? m[f] : GL_ALWAYS;
}
inline GLenum stencil_op(uint64_t o) {
    static const GLenum m[] = {GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, GL_INCR_WRAP, GL_DECR_WRAP};
    return o < 8 ? m[o] : GL_KEEP;
}
inline GLenum address(uint64_t a) {
    switch (a) {
        case 1: return GL_MIRROR_CLAMP_TO_EDGE_EXT;
        case 2: return GL_REPEAT;
        case 3: return GL_MIRRORED_REPEAT;
        case 4: case 5: return GL_CLAMP_TO_BORDER_EXT;
        default: return GL_CLAMP_TO_EDGE;
    }
}
inline Sampler* make_sampler(FakeObjcObj* d) {
    if (!tgl().init) return nullptr;
    auto* s = new Sampler();
    glGenSamplers(1, &s->id);
    uint64_t minf = prop(d, "minFilter", 0), magf = prop(d, "magFilter", 0), mip = prop(d, "mipFilter", 0);
    GLenum min = mip == 0 ? (minf ? GL_LINEAR : GL_NEAREST)
               : mip == 1 ? (minf ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST)
                          : (minf ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR);
    glSamplerParameteri(s->id, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(min));
    glSamplerParameteri(s->id, GL_TEXTURE_MAG_FILTER, magf ? GL_LINEAR : GL_NEAREST);
    glSamplerParameteri(s->id, GL_TEXTURE_WRAP_S, static_cast<GLint>(address(prop(d, "sAddressMode", 0))));
    glSamplerParameteri(s->id, GL_TEXTURE_WRAP_T, static_cast<GLint>(address(prop(d, "tAddressMode", 0))));
    glSamplerParameteri(s->id, GL_TEXTURE_WRAP_R, static_cast<GLint>(address(prop(d, "rAddressMode", 0))));
    uint64_t aniso = prop(d, "maxAnisotropy", 1);
    if (aniso > 1 && gles::has_extension("GL_EXT_texture_filter_anisotropic"))
        glSamplerParameterf(s->id, GL_TEXTURE_MAX_ANISOTROPY_EXT, static_cast<float>(aniso));
    glSamplerParameterf(s->id, GL_TEXTURE_MIN_LOD, static_cast<float>(propd(d, "lodMinClamp", 0.0)));
    glSamplerParameterf(s->id, GL_TEXTURE_MAX_LOD, static_cast<float>(propd(d, "lodMaxClamp", 1000.0)));
    if (d && d->props.count("compareFunction")) {
        s->compare = true;
        glSamplerParameteri(s->id, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glSamplerParameteri(s->id, GL_TEXTURE_COMPARE_FUNC, static_cast<GLint>(compare_fn(prop(d, "compareFunction", 7))));
    }
    gles::publish();
    return s;
}
inline StencilFace stencil_face(FakeObjcObj* d) {
    StencilFace f;
    if (!d) return f;
    f.func = compare_fn(prop(d, "stencilCompareFunction", 7));
    f.fail = stencil_op(prop(d, "stencilFailureOperation", 0));
    f.zfail = stencil_op(prop(d, "depthFailureOperation", 0));
    f.pass = stencil_op(prop(d, "depthStencilPassOperation", 0));
    f.read = static_cast<GLuint>(prop(d, "readMask", 0xffffffff));
    f.write = static_cast<GLuint>(prop(d, "writeMask", 0xffffffff));
    return f;
}
inline DepthStencil* make_depth_stencil(FakeObjcObj* d) {
    auto* s = new DepthStencil();
    s->depth_func = compare_fn(prop(d, "depthCompareFunction", 7));
    s->depth_write = prop(d, "depthWriteEnabled", 0) & 0xff;
    FakeObjcObj* fr = fake_lookup(prop(d, "frontFaceStencil", 0));
    FakeObjcObj* bk = fake_lookup(prop(d, "backFaceStencil", 0));
    s->stencil = fr || bk;
    s->front = stencil_face(fr);
    s->back = stencil_face(bk ? bk : fr);
    return s;
}

// ---- pipelines ----
inline GLenum blend_factor(uint64_t f) {
    static const GLenum m[] = {GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                               GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_SRC_ALPHA_SATURATE,
                               GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_COLOR, GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA};
    return f < 15 ? m[f] : GL_ONE;
}
inline GLenum blend_op(uint64_t o) {
    static const GLenum m[] = {GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, GL_MIN, GL_MAX};
    return o < 5 ? m[o] : GL_FUNC_ADD;
}

inline std::vector<std::pair<std::string, std::string>> macros_of(FakeObjcObj* options) {
    std::vector<std::pair<std::string, std::string>> out;
    FakeObjcObj* dict = fake_lookup(prop(options, "preprocessorMacros", 0));
    if (!dict) return out;
    for (size_t i = 0; i < dict->items.size() && i < dict->values.size(); ++i) {
        std::string k, v;
        if (!objc_string_value(dict->items[i], k)) continue;
        if (!objc_string_value(dict->values[i], v)) {
            FakeObjcObj* n = fake_lookup(dict->values[i]);
            v = n ? std::to_string(static_cast<long long>(prop(n, "value", 0))) : "1";
        }
        out.push_back({k, v});
    }
    return out;
}

inline GLuint compile(GLenum type, const std::string& src, std::string& log) {
    GLuint s = glCreateShader(type);
    const char* p = src.c_str();
    glShaderSource(s, 1, &p, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[2048] = {};
        glGetShaderInfoLog(s, sizeof buf, nullptr, buf);
        log = buf;
        glDeleteShader(s);
        return 0;
    }
    return s;
}

inline void dump_failed_shader(const char* what, const std::string& msl_src, const std::string& glsl, const std::string& log) {
    static std::atomic<int> n{0};
    int id = n++;
    const char* dir = getenv("LOADER_FRAME_DIR");
    std::string base = std::string(dir ? dir : "/data/local/tmp") + "/failed_" + std::to_string(id);
    if (FILE* fp = fopen((base + ".metal").c_str(), "wb")) { fwrite(msl_src.data(), 1, msl_src.size(), fp); fclose(fp); }
    if (FILE* fp = fopen((base + ".glsl").c_str(), "wb")) { fwrite(glsl.data(), 1, glsl.size(), fp); fclose(fp); }
    fprintf(stderr, "[MTLGL] %s failed (%s.*): %s\n", what, base.c_str(), log.c_str());
}

inline Pipeline* make_pipeline(FakeObjcObj* desc) {
    if (!tgl().init) return nullptr;
    auto* p = new Pipeline();
    FakeObjcObj* vf = fake_lookup(prop(desc, "vertexFunction", 0));
    FakeObjcObj* ff = fake_lookup(prop(desc, "fragmentFunction", 0));
    auto source_of = [](FakeObjcObj* fn, std::string& src, FakeObjcObj*& opts) {
        FakeObjcObj* lib = fn ? fake_lookup(prop(fn, "#library", 0)) : nullptr;
        if (!lib) return false;
        src = lib->s;
        opts = fake_lookup(prop(lib, "#options", 0));
        return true;
    };
    std::string vsrc, fsrc;
    FakeObjcObj *vo = nullptr, *fo = nullptr;
    if (!source_of(vf, vsrc, vo)) { fprintf(stderr, "[MTLGL] pipeline without vertex function source\n"); return p; }
    p->vs = msl::translate(vsrc, macros_of(vo), clip_control());
    bool have_fs = source_of(ff, fsrc, fo);
    if (have_fs) p->fs = msl::translate(fsrc, macros_of(fo), clip_control());
    else p->fs.ok = true, p->fs.glsl = "#version 320 es\nvoid main() {}\n";
    if (!p->vs.ok) { dump_failed_shader("vertex translation", vsrc, "", p->vs.error); return p; }
    if (!p->fs.ok) { dump_failed_shader("fragment translation", fsrc, "", p->fs.error); return p; }
    if (getenv("LOADER_GL_SOLID") && p->fs.glsl.find("o_SV_Target0") != std::string::npos) {   // geometry debug: solid magenta
        size_t end = p->fs.glsl.rfind('}');
        p->fs.glsl.insert(end, "o_SV_Target0 = vec4(1.0, 0.0, 1.0, 1.0);\n");
        static const std::string solid = "{o_SV_Target0 = vec4(1.0, 0.0, 1.0, 1.0); return;}";
        for (size_t r = p->fs.glsl.find("return;"); r != std::string::npos; r = p->fs.glsl.find("return;", r + solid.size())) {
            p->fs.glsl.replace(r, 7, solid);
        }
    }
    std::string log;
    GLuint vs = compile(GL_VERTEX_SHADER, p->vs.glsl, log);
    if (!vs) { dump_failed_shader("vertex compile", vsrc, p->vs.glsl, log); return p; }
    GLuint fs = compile(GL_FRAGMENT_SHADER, p->fs.glsl, log);
    if (!fs) { dump_failed_shader("fragment compile", fsrc, p->fs.glsl, log); glDeleteShader(vs); return p; }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char buf[2048] = {};
        glGetProgramInfoLog(prog, sizeof buf, nullptr, buf);
        dump_failed_shader("link", vsrc + "\n// ---- fragment ----\n" + fsrc, p->vs.glsl + "\n// ---- fragment ----\n" + p->fs.glsl, buf);
        glDeleteProgram(prog);
        return p;
    }
    glUseProgram(prog);
    int unit = 0, binding = 0;
    for (const msl::SamplerUse& s : p->vs.samplers) { glUniform1i(glGetUniformLocation(prog, s.uniform.c_str()), unit); p->vs_units.push_back(unit++); }
    for (const msl::SamplerUse& s : p->fs.samplers) { glUniform1i(glGetUniformLocation(prog, s.uniform.c_str()), unit); p->fs_units.push_back(unit++); }
    for (const msl::BlockUse& b : p->vs.blocks) { glUniformBlockBinding(prog, glGetUniformBlockIndex(prog, b.block.c_str()), binding); p->vs_bind.push_back(binding++); }
    for (const msl::BlockUse& b : p->fs.blocks) { glUniformBlockBinding(prog, glGetUniformBlockIndex(prog, b.block.c_str()), binding); p->fs_bind.push_back(binding++); }
    glUseProgram(0);
    p->prog = prog;
    // vertex layout
    FakeObjcObj* vd = fake_lookup(prop(desc, "vertexDescriptor", 0));
    if (getenv("LOADER_GL_TRACE")) {
        std::string keys;
        for (auto& kv : desc->props) keys += kv.first + " ";
        printf("[GLTRACE] pipeline desc keys: %s| vd %p\n", keys.c_str(), static_cast<void*>(vd));
        if (vd) {
            keys.clear();
            for (auto& kv : vd->props) keys += kv.first + " ";
            FakeObjcObj* at = fake_lookup(prop(vd, "attributes", 0));
            std::string ak;
            if (at) for (auto& kv : at->props) ak += kv.first + " ";
            printf("[GLTRACE]   vd keys: %s| attributes keys: %s\n", keys.c_str(), ak.c_str());
        }
    }
    for (const msl::VertexInput& in : p->vs.inputs) {
        FakeObjcObj* a = child(vd, "attributes", in.location);
        if (!a || !prop(a, "format", 0)) continue;
        p->attrs.push_back({in.location, prop(a, "format", 0), prop(a, "offset", 0), prop(a, "bufferIndex", 0)});
        uint64_t bi = prop(a, "bufferIndex", 0);
        if (!p->layouts.count(bi)) {
            FakeObjcObj* l = child(vd, "layouts", static_cast<int>(bi));
            Layout lay;
            lay.stride = prop(l, "stride", 0);
            lay.step = prop(l, "stepFunction", 1);
            lay.rate = std::max<uint64_t>(1, prop(l, "stepRate", 1));
            p->layouts[bi] = lay;
        }
    }
    for (int i = 0; i < 8; ++i) {
        FakeObjcObj* c = child(desc, "colorAttachments", i);
        if (!c) continue;
        Blend& b = p->blend[i];
        b.enabled = prop(c, "blendingEnabled", 0) & 0xff;
        b.src_rgb = blend_factor(prop(c, "sourceRGBBlendFactor", 1));
        b.dst_rgb = blend_factor(prop(c, "destinationRGBBlendFactor", 0));
        b.src_a = blend_factor(prop(c, "sourceAlphaBlendFactor", 1));
        b.dst_a = blend_factor(prop(c, "destinationAlphaBlendFactor", 0));
        b.op_rgb = blend_op(prop(c, "rgbBlendOperation", 0));
        b.op_a = blend_op(prop(c, "alphaBlendOperation", 0));
        b.mask = prop(c, "writeMask", 15);
    }
    p->alpha_to_coverage = prop(desc, "alphaToCoverageEnabled", 0) & 0xff;
    gles::publish();
    return p;
}

// ---- render passes and draws ----
struct VFmt { GLint comps; GLenum type; bool norm, integer; int size; };
inline bool vertex_format(uint64_t f, VFmt& v) {
    struct E { uint64_t f; VFmt v; };
    static const E m[] = {
        {1, {2, GL_UNSIGNED_BYTE, false, true, 2}}, {2, {3, GL_UNSIGNED_BYTE, false, true, 3}}, {3, {4, GL_UNSIGNED_BYTE, false, true, 4}},
        {4, {2, GL_BYTE, false, true, 2}}, {5, {3, GL_BYTE, false, true, 3}}, {6, {4, GL_BYTE, false, true, 4}},
        {7, {2, GL_UNSIGNED_BYTE, true, false, 2}}, {8, {3, GL_UNSIGNED_BYTE, true, false, 3}}, {9, {4, GL_UNSIGNED_BYTE, true, false, 4}},
        {10, {2, GL_BYTE, true, false, 2}}, {11, {3, GL_BYTE, true, false, 3}}, {12, {4, GL_BYTE, true, false, 4}},
        {13, {2, GL_UNSIGNED_SHORT, false, true, 4}}, {14, {3, GL_UNSIGNED_SHORT, false, true, 6}}, {15, {4, GL_UNSIGNED_SHORT, false, true, 8}},
        {16, {2, GL_SHORT, false, true, 4}}, {17, {3, GL_SHORT, false, true, 6}}, {18, {4, GL_SHORT, false, true, 8}},
        {19, {2, GL_UNSIGNED_SHORT, true, false, 4}}, {20, {3, GL_UNSIGNED_SHORT, true, false, 6}}, {21, {4, GL_UNSIGNED_SHORT, true, false, 8}},
        {22, {2, GL_SHORT, true, false, 4}}, {23, {3, GL_SHORT, true, false, 6}}, {24, {4, GL_SHORT, true, false, 8}},
        {25, {2, GL_HALF_FLOAT, false, false, 4}}, {26, {3, GL_HALF_FLOAT, false, false, 6}}, {27, {4, GL_HALF_FLOAT, false, false, 8}},
        {28, {1, GL_FLOAT, false, false, 4}}, {29, {2, GL_FLOAT, false, false, 8}}, {30, {3, GL_FLOAT, false, false, 12}},
        {31, {4, GL_FLOAT, false, false, 16}}, {32, {1, GL_INT, false, true, 4}}, {33, {2, GL_INT, false, true, 8}},
        {34, {3, GL_INT, false, true, 12}}, {35, {4, GL_INT, false, true, 16}}, {36, {1, GL_UNSIGNED_INT, false, true, 4}},
        {37, {2, GL_UNSIGNED_INT, false, true, 8}}, {38, {3, GL_UNSIGNED_INT, false, true, 12}}, {39, {4, GL_UNSIGNED_INT, false, true, 16}},
        {40, {4, GL_INT_2_10_10_10_REV, true, false, 4}}, {41, {4, GL_UNSIGNED_INT_2_10_10_10_REV, true, false, 4}},
        {45, {1, GL_UNSIGNED_BYTE, false, true, 1}}, {46, {1, GL_BYTE, false, true, 1}}, {47, {1, GL_UNSIGNED_BYTE, true, false, 1}},
        {48, {1, GL_BYTE, true, false, 1}}, {49, {1, GL_UNSIGNED_SHORT, false, true, 2}}, {50, {1, GL_SHORT, false, true, 2}},
        {51, {1, GL_UNSIGNED_SHORT, true, false, 2}}, {52, {1, GL_SHORT, true, false, 2}}, {53, {1, GL_HALF_FLOAT, false, false, 2}}};
    for (const E& e : m) if (e.f == f) { v = e.v; return true; }
    return false;
}

inline void begin_pass(Encoder* e, FakeObjcObj* pass) {
    ThreadGL& g = tgl();
    if (!g.init) return;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g.fbo);
    GLenum bufs[8];
    int nbufs = 0, w = 0, h = 0;
    struct Clear { int i; double c[4]; bool integer; };
    std::vector<Clear> clears;
    for (int i = 0; i < g.max_draw_buffers; ++i) {
        FakeObjcObj* c = child(pass, "colorAttachments", i);
        Tex* t = c ? texture(fake_lookup(prop(c, "texture", 0))) : nullptr;
        if (!t) {
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, 0, 0);
            bufs[i] = GL_NONE;
            continue;
        }
        int level = static_cast<int>(prop(c, "level", 0));
        int slice = static_cast<int>(t->target == GL_TEXTURE_3D ? prop(c, "depthPlane", 0) : prop(c, "slice", 0));
        attach(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, t, level, slice);
        bufs[i] = GL_COLOR_ATTACHMENT0 + i;
        nbufs = i + 1;
        w = std::max(1, t->w >> level);
        h = std::max(1, t->h >> level);
        if (prop(c, "loadAction", 0) == 2) {
            Clear cl{i, {propd(c, "#clear0", 0), propd(c, "#clear1", 0), propd(c, "#clear2", 0), propd(c, "#clear3", 1)}, t->f.integer};
            clears.push_back(cl);
        }
        uint64_t store = prop(c, "storeAction", 1);
        Tex* rt = (store == 2 || store == 3) ? texture(fake_lookup(prop(c, "resolveTexture", 0))) : nullptr;
        if (rt && e->nresolve < 8) e->resolve[e->nresolve++] = {t->id, rt->id, w, h, static_cast<GLenum>(GL_COLOR_ATTACHMENT0 + i)};
    }
    if (!nbufs) bufs[0] = GL_NONE;
    glDrawBuffers(nbufs ? nbufs : 1, bufs);
    FakeObjcObj* da = fake_lookup(prop(pass, "depthAttachment", 0));
    FakeObjcObj* sa = fake_lookup(prop(pass, "stencilAttachment", 0));
    Tex* dt = da ? texture(fake_lookup(prop(da, "texture", 0))) : nullptr;
    Tex* st = sa ? texture(fake_lookup(prop(sa, "texture", 0))) : nullptr;
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
    if (dt && st && dt == st && dt->f.stencil) {
        attach(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, dt, 0, static_cast<int>(prop(da, "slice", 0)));
    } else {
        if (dt) attach(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, dt, 0, static_cast<int>(prop(da, "slice", 0)));
        if (st) attach(GL_DRAW_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, st, 0, static_cast<int>(prop(sa, "slice", 0)));
    }
    if (!w && dt) { w = dt->w; h = dt->h; }
    if (!w && st) { w = st->w; h = st->h; }
    if (!w) { w = static_cast<int>(prop(pass, "renderTargetWidth", 1)); h = static_cast<int>(prop(pass, "renderTargetHeight", 1)); }
    e->rt_w = w;
    e->rt_h = h;
    GLenum status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    e->valid = status == GL_FRAMEBUFFER_COMPLETE;
    if (!e->valid) {
        static int logged = 0;
        if (logged++ < 10) fprintf(stderr, "[MTLGL] render pass framebuffer incomplete: 0x%x (%d color, depth %d)\n", status, nbufs, dt != nullptr);
        return;
    }
    glDisable(GL_SCISSOR_TEST);
    for (const Clear& c : clears) {
        glColorMaski(c.i, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        if (c.integer) {
            GLuint u[4] = {static_cast<GLuint>(c.c[0]), static_cast<GLuint>(c.c[1]), static_cast<GLuint>(c.c[2]), static_cast<GLuint>(c.c[3])};
            glClearBufferuiv(GL_COLOR, c.i, u);
        } else {
            GLfloat f[4] = {static_cast<GLfloat>(c.c[0]), static_cast<GLfloat>(c.c[1]), static_cast<GLfloat>(c.c[2]), static_cast<GLfloat>(c.c[3])};
            glClearBufferfv(GL_COLOR, c.i, f);
        }
    }
    bool clear_d = dt && prop(da, "loadAction", 0) == 2, clear_s = st && prop(sa, "loadAction", 0) == 2;
    if (clear_d) glDepthMask(GL_TRUE);
    if (clear_s) glStencilMask(0xff);
    float cd = static_cast<float>(propd(da, "clearDepth", 1.0));
    GLint cs = static_cast<GLint>(prop(sa, "clearStencil", 0));
    if (clear_d && clear_s && dt == st) glClearBufferfi(GL_DEPTH_STENCIL, 0, cd, cs);
    else {
        if (clear_d) glClearBufferfv(GL_DEPTH, 0, &cd);
        if (clear_s) glClearBufferiv(GL_STENCIL, 0, &cs);
    }
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, w, h);
    glViewport(0, 0, w, h);
    glDepthRangef(0, 1);
    if (tracing()) {
        FakeObjcObj* c0 = child(pass, "colorAttachments", 0);
        Tex* t0 = c0 ? texture(fake_lookup(prop(c0, "texture", 0))) : nullptr;
        printf("[GLTRACE] pass %dx%d colors %d (c0 tex %u pf %llu load %llu clear %.2f,%.2f,%.2f,%.2f) depth %d err 0x%x\n", w, h, nbufs,
               t0 ? t0->id : 0, t0 ? static_cast<unsigned long long>(t0->pf) : 0ull, static_cast<unsigned long long>(prop(c0, "loadAction", 0)),
               propd(c0, "#clear0", 0), propd(c0, "#clear1", 0), propd(c0, "#clear2", 0), propd(c0, "#clear3", 1), dt != nullptr, glGetError());
    }
}

inline void end_pass(Encoder* e) {
    ThreadGL& g = tgl();
    if (!g.init) return;
    for (int i = 0; i < e->nresolve; ++i) {
        const auto& r = e->resolve[i];
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g.read_fbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE, r.src_tex, 0);
        GLuint tmp;
        glGenFramebuffers(1, &tmp);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, tmp);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r.dst_tex, 0);
        glDisable(GL_SCISSOR_TEST);
        glBlitFramebuffer(0, 0, r.w, r.h, 0, 0, r.w, r.h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glDeleteFramebuffers(1, &tmp);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

inline void apply_state(Encoder* e) {
    Pipeline* p = e->pipe;
    for (int i = 0; i < tgl().max_draw_buffers; ++i) {
        const Blend& b = p->blend[i];
        if (b.enabled) {
            glEnablei(GL_BLEND, i);
            glBlendFuncSeparatei(i, b.src_rgb, b.dst_rgb, b.src_a, b.dst_a);
            glBlendEquationSeparatei(i, b.op_rgb, b.op_a);
        } else {
            glDisablei(GL_BLEND, i);
        }
        glColorMaski(i, (b.mask & 8) != 0, (b.mask & 4) != 0, (b.mask & 2) != 0, (b.mask & 1) != 0);
    }
    if (p->alpha_to_coverage) glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE); else glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    DepthStencil def;
    const DepthStencil* ds = e->ds ? e->ds : &def;
    if (ds->depth_func != GL_ALWAYS || ds->depth_write) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(ds->depth_func);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(ds->depth_write ? GL_TRUE : GL_FALSE);
    if (ds->stencil) {
        glEnable(GL_STENCIL_TEST);
        glStencilFuncSeparate(GL_FRONT, ds->front.func, static_cast<GLint>(e->ref_front), ds->front.read);
        glStencilFuncSeparate(GL_BACK, ds->back.func, static_cast<GLint>(e->ref_back), ds->back.read);
        glStencilOpSeparate(GL_FRONT, ds->front.fail, ds->front.zfail, ds->front.pass);
        glStencilOpSeparate(GL_BACK, ds->back.fail, ds->back.zfail, ds->back.pass);
        glStencilMaskSeparate(GL_FRONT, ds->front.write);
        glStencilMaskSeparate(GL_BACK, ds->back.write);
    } else {
        glDisable(GL_STENCIL_TEST);
    }
    // Rows are stored top-first like Metal, which mirrors GL's window y: Metal clockwise is GL counter-clockwise.
    glFrontFace(e->winding == 0 ? GL_CCW : GL_CW);
    if (e->cull == 0) glDisable(GL_CULL_FACE);
    else { glEnable(GL_CULL_FACE); glCullFace(e->cull == 1 ? GL_FRONT : GL_BACK); }
    if (e->depth_bias != 0 || e->slope != 0) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(static_cast<float>(e->slope), static_cast<float>(e->depth_bias));
    } else {
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
}

inline void bind_resources(Encoder* e, ThreadGL& g) {
    Pipeline* p = e->pipe;
    auto blocks = [&](const std::vector<msl::BlockUse>& bl, const std::vector<int>& bind, BoundBuf* bufs) {
        for (size_t i = 0; i < bl.size(); ++i) {
            size_t avail = 0;
            const uint8_t* src = bl[i].buffer >= 0 && bl[i].buffer < 32 ? bufs[bl[i].buffer].data(avail) : nullptr;
            static std::vector<uint8_t> zero;
            size_t n = static_cast<size_t>(bl[i].size);
            if (!src || avail < n) {
                if (zero.size() < n) zero.resize(n);
                std::fill(zero.begin(), zero.begin() + static_cast<long>(n), 0);
                if (src) std::memcpy(zero.data(), src, avail);
                src = zero.data();
            }
            size_t off = g.ub.push(src, n, static_cast<size_t>(g.ubo_align));
            glBindBufferRange(GL_UNIFORM_BUFFER, static_cast<GLuint>(bind[i]), g.ub.buf, static_cast<GLintptr>(off), static_cast<GLsizeiptr>(n));
        }
    };
    blocks(p->vs.blocks, p->vs_bind, e->vbuf);
    blocks(p->fs.blocks, p->fs_bind, e->fbuf);
    auto samplers = [&](const std::vector<msl::SamplerUse>& su, const std::vector<int>& units, uint64_t* tex, uint64_t* samp) {
        for (size_t i = 0; i < su.size(); ++i) {
            glActiveTexture(GL_TEXTURE0 + units[i]);
            Tex* t = su[i].texture >= 0 && su[i].texture < 32 ? texture(fake_lookup(tex[su[i].texture])) : nullptr;
            GLenum target = GL_TEXTURE_2D;
            switch (su[i].kind) {
                case msl::TexKind::Tex2DArray: case msl::TexKind::Depth2DArray: target = GL_TEXTURE_2D_ARRAY; break;
                case msl::TexKind::Tex3D: target = GL_TEXTURE_3D; break;
                case msl::TexKind::TexCube: case msl::TexKind::DepthCube: target = GL_TEXTURE_CUBE_MAP; break;
                case msl::TexKind::TexCubeArray: target = GL_TEXTURE_CUBE_MAP_ARRAY; break;
                default: break;
            }
            if (t && t->target == target) glBindTexture(target, t->id);
            else if (target == GL_TEXTURE_2D) glBindTexture(GL_TEXTURE_2D, g.dummy2d);
            else glBindTexture(target, 0);
            Sampler* s = su[i].sampler >= 0 && su[i].sampler < 16 ? gl_obj<Sampler>(fake_lookup(samp[su[i].sampler])) : nullptr;
            glBindSampler(static_cast<GLuint>(units[i]), s ? s->id : g.dummy_sampler);
        }
    };
    samplers(p->vs.samplers, p->vs_units, e->vtex, e->vsamp);
    samplers(p->fs.samplers, p->fs_units, e->ftex, e->fsamp);
}

// Streams the vertex data a draw reads and points the attributes at it.
inline void bind_vertices(Encoder* e, ThreadGL& g, uint64_t max_vertex, uint64_t instances) {
    Pipeline* p = e->pipe;
    glBindVertexArray(g.vao);
    uint32_t want = 0;
    std::map<uint64_t, size_t> uploaded;   // buffer index -> ring offset
    for (const Attr& a : p->attrs) {
        VFmt vf;
        if (!vertex_format(a.format, vf) || a.buffer >= 32) continue;
        const Layout& l = p->layouts[a.buffer];
        if (!uploaded.count(a.buffer)) {
            uint64_t elems = l.step == 2 ? (instances + l.rate - 1) / l.rate : l.step == 0 ? 1 : max_vertex + 1;
            uint64_t need = 0;
            for (const Attr& b : p->attrs) {
                VFmt bf;
                if (b.buffer == a.buffer && vertex_format(b.format, bf)) need = std::max<uint64_t>(need, b.offset + bf.size);
            }
            need += (elems ? elems - 1 : 0) * l.stride;
            size_t avail = 0;
            const uint8_t* src = e->vbuf[a.buffer].data(avail);
            size_t n = src ? std::min<size_t>(need, avail) : 0;
            uploaded[a.buffer] = g.vb.push(src, n, 16);
        }
        glBindBuffer(GL_ARRAY_BUFFER, g.vb.buf);
        const void* ptr = reinterpret_cast<const void*>(uploaded[a.buffer] + a.offset);
        GLsizei stride = static_cast<GLsizei>(l.step == 0 ? 0 : l.stride);
        if (vf.integer) glVertexAttribIPointer(static_cast<GLuint>(a.loc), vf.comps, vf.type, stride, ptr);
        else glVertexAttribPointer(static_cast<GLuint>(a.loc), vf.comps, vf.type, vf.norm ? GL_TRUE : GL_FALSE, stride, ptr);
        glVertexAttribDivisor(static_cast<GLuint>(a.loc), l.step == 2 ? static_cast<GLuint>(l.rate) : l.step == 0 ? 0x7fffffff : 0);
        want |= 1u << a.loc;
    }
    for (int i = 0; i < 16; ++i) {
        bool on = want & (1u << i);
        if (on != ((g.enabled_attribs >> i) & 1)) { if (on) glEnableVertexAttribArray(i); else glDisableVertexAttribArray(i); }
    }
    g.enabled_attribs = want;
}

inline GLenum primitive(uint64_t t) {
    static const GLenum m[] = {GL_POINTS, GL_LINES, GL_LINE_STRIP, GL_TRIANGLES, GL_TRIANGLE_STRIP};
    return t < 5 ? m[t] : GL_TRIANGLES;
}

inline bool ready(Encoder* e) {
    bool ok = e && e->valid && e->pipe && e->pipe->prog;
    static const int skip = getenv("LOADER_GL_SKIP_DRAW") ? atoi(getenv("LOADER_GL_SKIP_DRAW")) : 0;   // debug: drop the nth draw of each pass
    if (ok && skip && ++e->draws == skip) return false;
    if (!ok && tracing()) printf("[GLTRACE] draw skipped: encoder %d valid %d pipe %d prog %u\n", e != nullptr, e && e->valid,
                                 e && e->pipe, e && e->pipe ? e->pipe->prog : 0);
    return ok;
}

inline void draw(Encoder* e, uint64_t prim, uint64_t start, uint64_t count, uint64_t instances) {
    ThreadGL& g = tgl();
    if (!ready(e) || !count) return;
    glUseProgram(e->pipe->prog);
    apply_state(e);
    bind_resources(e, g);
    bind_vertices(e, g, start + count - 1, instances ? instances : 1);
    glDrawArraysInstanced(primitive(prim), static_cast<GLint>(start), static_cast<GLsizei>(count), static_cast<GLsizei>(instances ? instances : 1));
    if (tracing()) printf("[GLTRACE] drawArrays prim %llu start %llu count %llu prog %u err 0x%x\n", static_cast<unsigned long long>(prim),
                          static_cast<unsigned long long>(start), static_cast<unsigned long long>(count), e->pipe->prog, glGetError());
}

inline void draw_indexed(Encoder* e, uint64_t prim, uint64_t count, uint64_t type, FakeObjcObj* ibuf, uint64_t ioff, uint64_t instances) {
    ThreadGL& g = tgl();
    if (!ready(e) || !count || !ibuf || !ibuf->mem) return;
    size_t isz = type == 1 ? 4 : 2;
    if (ioff + count * isz > ibuf->len) return;
    const uint8_t* idx = static_cast<const uint8_t*>(ibuf->mem) + ioff;
    uint64_t maxv = 0;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t v = isz == 4 ? reinterpret_cast<const uint32_t*>(idx)[i] : reinterpret_cast<const uint16_t*>(idx)[i];
        if (v != (isz == 4 ? 0xffffffffull : 0xffffull) && v > maxv) maxv = v;
    }
    glUseProgram(e->pipe->prog);
    apply_state(e);
    bind_resources(e, g);
    bind_vertices(e, g, maxv, instances ? instances : 1);
    size_t off = g.ib.push(idx, count * isz, 4);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ib.buf);
    glDrawElementsInstanced(primitive(prim), static_cast<GLsizei>(count), isz == 4 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT,
                            reinterpret_cast<const void*>(off), static_cast<GLsizei>(instances ? instances : 1));
    if (tracing()) {
        printf("[GLTRACE] drawElements prim %llu count %llu maxv %llu prog %u attrs %zu vsblocks %zu fs samplers %zu ftex0 %u err 0x%x\n",
               static_cast<unsigned long long>(prim), static_cast<unsigned long long>(count), static_cast<unsigned long long>(maxv),
               e->pipe->prog, e->pipe->attrs.size(), e->pipe->vs.blocks.size(), e->pipe->fs.samplers.size(),
               texture(fake_lookup(e->ftex[0])) ? texture(fake_lookup(e->ftex[0]))->id : 0, glGetError());
        const Blend& b0 = e->pipe->blend[0];
        printf("[GLTRACE]   blend %d src 0x%x dst 0x%x mask %llu | vbuf0 %p+%llu fbuf0 %p\n", b0.enabled, b0.src_rgb, b0.dst_rgb,
               static_cast<unsigned long long>(b0.mask), static_cast<void*>(e->vbuf[0].buf), static_cast<unsigned long long>(e->vbuf[0].offset),
               static_cast<void*>(e->fbuf[0].buf));
        if (const char* dir = getenv("LOADER_FRAME_DIR")) {   // the first texture of each traced draw
            Tex* t = texture(fake_lookup(e->ftex[0]));
            std::vector<uint8_t> px;
            GLint fb = 0;
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
            if (t && read_rgba(t, px)) {
                char path[512];
                snprintf(path, sizeof path, "%s/tex_%u_pf%llu_%dx%d.png", dir, t->id, static_cast<unsigned long long>(t->pf), t->w, t->h);
                gles::write_png(path, t->w, t->h, px.data());
            } else if (t) {
                printf("[GLTRACE]   tex %u pf %llu %dx%d not readable (compressed=%d)\n", t->id, static_cast<unsigned long long>(t->pf), t->w, t->h, t->f.compressed);
            }
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(fb));
        }
    }
}

// Command buffer commit: flush, then save presented drawables as PNG (LOADER_FRAME_DIR, every LOADER_FRAME_EVERY).
inline void commit(FakeObjcObj* cb) {
    if (!tgl().init) return;
    uint64_t drawable = prop(cb, "#present", 0);
    if (!drawable) { glFlush(); return; }
    cb->props.erase("#present");
    uint64_t n = ++g_frame_no;
    static const char* dir = getenv("LOADER_FRAME_DIR");
    static const uint64_t every = getenv("LOADER_FRAME_EVERY") ? strtoull(getenv("LOADER_FRAME_EVERY"), nullptr, 10) : 60;
    if (dir && every && n % every == 0) {
        Tex* t = texture(fake_lookup(prop(fake_lookup(drawable), "texture", 0)));
        std::vector<uint8_t> px;
        if (read_rgba(t, px)) {
            char path[512];
            snprintf(path, sizeof path, "%s/frame_%05llu.png", dir, static_cast<unsigned long long>(n));
            for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;   // the layer is opaque
            bool ok = gles::write_png(path, t->w, t->h, px.data());
            printf("[MTLGL] frame %llu -> %s (%dx%d)%s\n", static_cast<unsigned long long>(n), path, t->w, t->h, ok ? "" : " WRITE FAILED");
            fflush(stdout);
        }
    }
    glFlush();
}

}  // namespace mtlgl

// Metal messages with a real GL meaning. Returns false to fall through to the property bag behaviour.
static bool mtl_gles_message(CallFrame& frame, const std::string& sel, FakeObjcObj* self) {
    using namespace mtlgl;
    if (!gles_enabled() || !self || self->kind != 3) return false;
    const double* in = g_objc_in_d;
    auto encoder = [&]() { return gl_obj<Encoder>(self); };

    // device / heap factories
    if (sel == "newTextureWithDescriptor:" || sel == "newTextureWithDescriptor:offset:") {
        uint64_t h;
        FakeObjcObj* t = fake_metal_bag(&h);
        if (FakeObjcObj* d = fake_lookup(frame.x[2])) t->props = d->props;
        t->props.erase("#gl");
        frame.ret_x0 = h;
        return true;
    }
    if (sel == "newTextureViewWithPixelFormat:" || sel.compare(0, 31, "newTextureViewWithPixelFormat:t") == 0) {
        uint64_t h;
        FakeObjcObj* t = fake_metal_bag(&h);
        t->props = self->props;
        t->props.erase("#gl");
        t->props["pixelFormat"] = frame.x[2];
        t->props["#viewOf"] = frame.x[0];
        frame.ret_x0 = h;
        return true;
    }
    if (sel == "newSamplerStateWithDescriptor:") {
        uint64_t h;
        FakeObjcObj* s = fake_metal_bag(&h);
        std::lock_guard<std::mutex> l(g_gl_mu);
        s->props["#gl"] = reinterpret_cast<uint64_t>(make_sampler(fake_lookup(frame.x[2])));
        frame.ret_x0 = h;
        return true;
    }
    if (sel == "newDepthStencilStateWithDescriptor:") {
        uint64_t h;
        FakeObjcObj* s = fake_metal_bag(&h);
        s->props["#gl"] = reinterpret_cast<uint64_t>(make_depth_stencil(fake_lookup(frame.x[2])));
        frame.ret_x0 = h;
        return true;
    }
    if (sel == "newLibraryWithSource:options:error:") {
        uint64_t h;
        FakeObjcObj* lib = fake_metal_bag(&h);
        objc_string_value(frame.x[2], lib->s);
        lib->props["#options"] = frame.x[3];
        if (frame.x[4]) *reinterpret_cast<uint64_t*>(frame.x[4]) = 0;
        frame.ret_x0 = h;
        return true;
    }
    if (sel == "newFunctionWithName:" || sel == "newFunctionWithName:constantValues:error:") {
        uint64_t h;
        FakeObjcObj* fn = fake_metal_bag(&h);
        objc_string_value(frame.x[2], fn->s);
        fn->props["#library"] = frame.x[0];
        fn->props["name"] = frame.x[2];
        frame.ret_x0 = h;
        return true;
    }
    if (sel.compare(0, 37, "newRenderPipelineStateWithDescriptor:") == 0) {
        uint64_t h;
        FakeObjcObj* ps = fake_metal_bag(&h);
        {
            std::lock_guard<std::mutex> l(g_gl_mu);
            ps->props["#gl"] = reinterpret_cast<uint64_t>(make_pipeline(fake_lookup(frame.x[2])));
        }
        if (sel == "newRenderPipelineStateWithDescriptor:completionHandler:") { run_block(frame.x[3], h); frame.ret_x0 = 0; return true; }
        size_t colons = colon_count(sel);
        if (colons >= 2) {   // ...:error: / ...:options:reflection:error: : clear the NSError out-parameter
            uint64_t err = frame.x[1 + colons];
            if (err) *reinterpret_cast<uint64_t*>(err) = 0;
        }
        frame.ret_x0 = h;
        return true;
    }
    // texture data
    if (sel == "replaceRegion:mipmapLevel:slice:withBytes:bytesPerRow:bytesPerImage:" || sel == "replaceRegion:mipmapLevel:withBytes:bytesPerRow:") {
        const uint64_t* r = reinterpret_cast<const uint64_t*>(frame.x[2]);   // MTLRegion {origin xyz, size whd}, by reference
        bool sliced = sel.find("slice:") != std::string::npos;
        int level = static_cast<int>(frame.x[3]);
        int slice = sliced ? static_cast<int>(frame.x[4]) : 0;
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(sliced ? frame.x[5] : frame.x[4]);
        size_t bpr = sliced ? frame.x[6] : frame.x[5], bpi = sliced ? frame.x[7] : 0;
        if (r) {
            Tex* t = texture(self);
            upload(t, level, slice, static_cast<int>(r[0]), static_cast<int>(r[1]), static_cast<int>(r[2]), static_cast<int>(r[3]),
                   static_cast<int>(r[4]), static_cast<int>(r[5] ? r[5] : 1), bytes, bpr, bpi);
            gles::publish();
        }
        return true;
    }
    if (sel == "getBytes:bytesPerRow:fromRegion:mipmapLevel:") {
        Tex* t = texture(self);
        std::vector<uint8_t> px;
        const uint64_t* r = reinterpret_cast<const uint64_t*>(frame.x[4]);
        uint8_t* dst = reinterpret_cast<uint8_t*>(frame.x[2]);
        if (t && r && dst && read_rgba(t, px)) {
            for (uint64_t y = 0; y < r[4] && r[1] + y < static_cast<uint64_t>(t->h); ++y)
                for (uint64_t x = 0; x < r[3] && r[0] + x < static_cast<uint64_t>(t->w); ++x) {
                    const uint8_t* s = &px[((r[1] + y) * t->w + r[0] + x) * 4];
                    uint8_t* d = dst + y * frame.x[3] + x * 4;
                    if (t->f.bgra) { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3]; } else std::memcpy(d, s, 4);
                }
        }
        return true;
    }
    // command buffers / encoders
    if (sel == "renderCommandEncoderWithDescriptor:") {
        uint64_t h;
        FakeObjcObj* enc = fake_metal_bag(&h);
        auto* e = new Encoder();
        enc->props["#gl"] = reinterpret_cast<uint64_t>(e);
        enc->props["#parent"] = frame.x[0];
        begin_pass(e, fake_lookup(frame.x[2]));
        frame.ret_x0 = h;
        return true;
    }
    if (sel == "presentDrawable:" || sel == "presentDrawable:afterMinimumDuration:" || sel == "presentDrawable:atTime:") {
        self->props["#present"] = frame.x[2];
        return false;   // presents are also counted by fake_uikit
    }
    if (sel == "commit") { commit(self); return false; }   // handlers fire in the bag code
    if (Encoder* e = encoder()) {
        if (sel == "endEncoding") { end_pass(e); return true; }
        if (sel == "setRenderPipelineState:") { e->pipe = gl_obj<Pipeline>(fake_lookup(frame.x[2])); return true; }
        if (sel == "setDepthStencilState:") { e->ds = gl_obj<DepthStencil>(fake_lookup(frame.x[2])); return true; }
        if (sel == "setStencilReferenceValue:") { e->ref_front = e->ref_back = static_cast<GLuint>(frame.x[2]); return true; }
        if (sel == "setStencilFrontReferenceValue:backReferenceValue:") {
            e->ref_front = static_cast<GLuint>(frame.x[2]);
            e->ref_back = static_cast<GLuint>(frame.x[3]);
            return true;
        }
        if (sel == "setCullMode:") { e->cull = frame.x[2]; return true; }
        if (sel == "setFrontFacingWinding:") { e->winding = frame.x[2]; return true; }
        if (sel == "setDepthBias:slopeScale:clamp:") {
            float b, s;
            std::memcpy(&b, &in[0], 4);
            std::memcpy(&s, &in[1], 4);
            e->depth_bias = b;
            e->slope = s;
            return true;
        }
        if (sel == "setViewport:") {
            const double* v = reinterpret_cast<const double*>(frame.x[2]);   // MTLViewport (6 doubles) by reference
            if (v && e->valid) {
                glViewport(static_cast<GLint>(v[0]), static_cast<GLint>(v[1]), static_cast<GLsizei>(v[2]), static_cast<GLsizei>(v[3]));
                glDepthRangef(static_cast<float>(v[4]), static_cast<float>(v[5]));
            }
            return true;
        }
        if (sel == "setScissorRect:") {
            const uint64_t* r = reinterpret_cast<const uint64_t*>(frame.x[2]);   // MTLScissorRect by reference
            if (r && e->valid) glScissor(static_cast<GLint>(r[0]), static_cast<GLint>(r[1]), static_cast<GLsizei>(r[2]), static_cast<GLsizei>(r[3]));
            return true;
        }
        if (sel == "setBlendColorRed:green:blue:alpha:") {
            float c[4];
            for (int i = 0; i < 4; ++i) std::memcpy(&c[i], &in[i], 4);
            glBlendColor(c[0], c[1], c[2], c[3]);
            return true;
        }
        bool vtx = sel.compare(0, 9, "setVertex") == 0, frg = sel.compare(0, 11, "setFragment") == 0;
        if (vtx || frg) {
            BoundBuf* bufs = vtx ? e->vbuf : e->fbuf;
            const std::string rest = sel.substr(vtx ? 9 : 11);
            if (rest == "Buffer:offset:atIndex:" && frame.x[4] < 32) {
                BoundBuf& b = bufs[frame.x[4]];
                b.buf = fake_lookup(frame.x[2]);
                b.offset = frame.x[3];
                b.bytes.clear();
                return true;
            }
            if (rest == "BufferOffset:atIndex:" && frame.x[3] < 32) { bufs[frame.x[3]].offset = frame.x[2]; return true; }
            if (rest == "Bytes:length:atIndex:" && frame.x[4] < 32) {
                BoundBuf& b = bufs[frame.x[4]];
                const uint8_t* p = reinterpret_cast<const uint8_t*>(frame.x[2]);
                b.bytes.assign(p, p + frame.x[3]);
                if (b.bytes.empty()) b.bytes.push_back(0);
                b.buf = nullptr;
                return true;
            }
            if (rest == "Texture:atIndex:" && frame.x[3] < 32) { (vtx ? e->vtex : e->ftex)[frame.x[3]] = frame.x[2]; return true; }
            if (rest == "SamplerState:atIndex:" && frame.x[3] < 16) { (vtx ? e->vsamp : e->fsamp)[frame.x[3]] = frame.x[2]; return true; }
            if (rest == "SamplerState:lodMinClamp:lodMaxClamp:atIndex:" && frame.x[3] < 16) { (vtx ? e->vsamp : e->fsamp)[frame.x[3]] = frame.x[2]; return true; }
        }
        if (sel == "drawPrimitives:vertexStart:vertexCount:") { draw(e, frame.x[2], frame.x[3], frame.x[4], 1); return false; }
        if (sel == "drawPrimitives:vertexStart:vertexCount:instanceCount:") { draw(e, frame.x[2], frame.x[3], frame.x[4], frame.x[5]); return false; }
        if (sel == "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:") {
            draw_indexed(e, frame.x[2], frame.x[3], frame.x[4], fake_lookup(frame.x[5]), frame.x[6], 1);
            return false;
        }
        if (sel == "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:") {
            draw_indexed(e, frame.x[2], frame.x[3], frame.x[4], fake_lookup(frame.x[5]), frame.x[6], frame.x[7]);
            return false;
        }
    }
    // render pass attachment: clearColor is an MTLClearColor (4 doubles in d0-d3)
    if (sel == "setClearColor:") {
        for (int i = 0; i < 4; ++i) {
            uint64_t bits;
            std::memcpy(&bits, &in[i], 8);
            self->props["#clear" + std::to_string(i) + "#d"] = bits;
        }
        return true;
    }
    if (sel == "clearColor") { objc_ret_rect(propd(self, "#clear0", 0), propd(self, "#clear1", 0), propd(self, "#clear2", 0), propd(self, "#clear3", 1)); return true; }
    // blit encoder
    if (sel == "blitCommandEncoder") {
        uint64_t h;
        fake_metal_bag(&h);
        frame.ret_x0 = h;
        return true;
    }
    if (sel == "generateMipmapsForTexture:") {
        if (Tex* t = texture(fake_lookup(frame.x[2]))) { glBindTexture(t->target, t->id); glGenerateMipmap(t->target); }
        return true;
    }
    if (sel == "copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:") {
        // x2 buffer, x3 offset, x4 bpr, x5 bpi, x6 &MTLSize, x7 texture; slice/level/&origin are on the stack (not passed)
        static int warned = 0;
        if (warned++ < 3) fprintf(stderr, "[MTLGL] buffer->texture blit needs stack arguments; not implemented\n");
        return true;
    }
    if (sel == "copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size:") {
        FakeObjcObj* s = fake_lookup(frame.x[2]);
        FakeObjcObj* d = fake_lookup(frame.x[4]);
        if (s && d && s->mem && d->mem && frame.x[3] + frame.x[6] <= s->len && frame.x[5] + frame.x[6] <= d->len)
            std::memmove(static_cast<uint8_t*>(d->mem) + frame.x[5], static_cast<uint8_t*>(s->mem) + frame.x[3], frame.x[6]);
        return true;
    }
    if (sel == "fillBuffer:range:value:") {
        FakeObjcObj* b = fake_lookup(frame.x[2]);
        if (b && b->mem && frame.x[3] + frame.x[4] <= b->len) std::memset(static_cast<uint8_t*>(b->mem) + frame.x[3], static_cast<int>(frame.x[5] & 0xff), frame.x[4]);
        return true;
    }
    if (sel == "endEncoding" || sel == "synchronizeResource:" || sel == "synchronizeTexture:slice:level:" || sel == "optimizeContentsForGPUAccess:" ||
        sel == "updateFence:" || sel == "waitForFence:" || sel.compare(0, 11, "useResource") == 0 || sel.compare(0, 7, "useHeap") == 0 ||
        sel.compare(0, 13, "memoryBarrier") == 0 || sel == "pushDebugGroup:" || sel == "popDebugGroup" || sel == "insertDebugSignpost:")
        return true;
    return false;
}
