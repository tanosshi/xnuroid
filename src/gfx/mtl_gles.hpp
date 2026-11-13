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

#include <unistd.h>

#include <algorithm>
#include <cstdarg>
#include <map>
#include <set>
#include <mutex>
#include <tuple>

#include "msl_glsl.hpp"
#include "pvrtc.hpp"
#include "pvrtc_gpu.hpp"

#include <thread>

namespace mtlgl {

inline std::atomic<uint64_t> g_draws_frame{0};        // draws issued since the last dumped frame
inline std::atomic<uint64_t> g_last_big_target{0};    // target texture of the last full-size draw
inline std::atomic<uint64_t> g_last_big_target_obj{0};  // metal texture object of the last full-size render pass
inline std::atomic<uint64_t> g_presented_drawable{0};  // the CAMetalDrawable most recently presented
// pass/draw census, drained by the frame dump: depth-attached passes are the ones a camera renders into
inline std::atomic<uint64_t> g_passes_depth{0}, g_passes_nodepth{0}, g_draws_depth{0}, g_draws_nodepth{0};

// ---- formats ----
struct Fmt {
    GLenum internal = 0, format = 0, type = 0;
    bool compressed = false, bgra = false, depth = false, stencil = false, integer = false;
    int norm16 = 0;                  // 16-bit normalized source widened to 32-bit float (no GL_EXT_texture_norm16): 1 unorm, 2 snorm
    int pvrtc = 0;                   // PVRTC1 source decoded to RGBA8 on upload: 4 or 2 bpp
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
        case 20: case 22: case 60: case 62: case 110: case 112: {   // R/RG/RGBA16 Unorm/Snorm
            const int comps = pf < 60 ? 1 : pf < 110 ? 2 : 4;
            const bool snorm = pf == 22 || pf == 62 || pf == 112;
            static const GLenum fmt[] = {GL_RED, GL_RG, 0, GL_RGBA};
            if (gles::has_extension("GL_EXT_texture_norm16")) {
                static const GLenum un[] = {0x822A, 0x822C, 0, 0x805B}, sn[] = {0x8F98, 0x8F99, 0, 0x8F9B};
                return u(snorm ? sn[comps - 1] : un[comps - 1], fmt[comps - 1], snorm ? GL_SHORT : GL_UNSIGNED_SHORT, 2 * comps);
            }
            static const GLenum fl[] = {GL_R32F, GL_RG32F, 0, GL_RGBA32F};
            f.norm16 = snorm ? 2 : 1;
            return u(fl[comps - 1], fmt[comps - 1], GL_FLOAT, 4 * comps);   // bytes = GL-side texel size
        }
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
    if (pf >= 160 && pf <= 167) {   // PVRTC RGB/RGBA 2/4 bpp (odd values sRGB)
        f.pvrtc = (pf - 160) % 4 < 2 ? 2 : 4;
        return u((pf & 1) ? GL_SRGB8_ALPHA8 : GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4);
    }
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
    std::atomic<uint32_t> uploads{0}, renders{0};   // LOADER_TEX_PROFILE bookkeeping
    uint64_t born = 0;
    bool placeholder = false;                        // tiny single-colour texture (Unity white/black/grey/bump defaults)
    uint32_t solid = 0;
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
    std::string vs_name, fs_name;   // MTLFunction names, for diagnostics
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
// LOADER_PASS_TRACE: what the guest did inside one pass, accumulated between the encoder's begin and endEncoding.
struct PassTrace {
    int id = 0;                       // 1-based pass number; 0 when the pass is not observed
    int w = 0, h = 0, colors = 0;
    bool depth = false, valid = false;
    int pipe_sets = 0, pipe_extra = 0;
    std::vector<std::pair<std::string, int>> pipes;   // "vs" / "fs" prog N -> times set
    struct Bound {
        int idx = 0;
        uint64_t obj = 0, off = 0;
        int n = 0, texid = 0, tw = 0, th = 0;
        uint64_t pf = 0;
        bool bytes = false;
    };
    std::vector<Bound> vb, fb, vt, ft;
    int vp_sets = 0;
    double vpv[6] = {0, 0, 0, 0, 0, 1};
    int sc_sets = 0;
    uint64_t scv[4] = {0, 0, 0, 0};
    int ds_sets = 0;
    uint64_t ds_obj = 0;
    int ds_func = -1, ds_write = 0, ds_stencil = 0;
    int draws = 0, rejected = 0, draw_extra = 0, reject_extra = 0;
    std::vector<std::pair<std::string, int>> drawlog, rejects;
    Tex* sampled = nullptr;           // first texture the fragment stage sampled on an accepted draw
    std::string last_vs, last_fs;     // pipeline of the last accepted draw
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
    GLuint targets[9] = {};                  // attachment texture ids (feedback check)
    int scissor[4] = {}, viewport[4] = {};   // last rects set (draw-sequence profile)
    bool valid = false;
    struct Resolve { GLuint src_tex, dst_tex; int w, h; GLenum attach; } resolve[8];
    int nresolve = 0;
    int draws = 0;
    PassTrace pass;   // LOADER_PASS_TRACE: what the guest did in this pass
};

inline std::mutex g_gl_mu;
// FakeObjcObj::gltype: what props["#gl"] points to
constexpr uint8_t kGlTexture = 1, kGlSampler = 2, kGlPipeline = 3, kGlDepthStencil = 4, kGlEncoder = 5;
inline std::atomic<int64_t> g_texture_bytes{0};
inline int64_t texture_bytes(const Tex* t) {
    int64_t texels = 0;
    for (int l = 0; l < t->levels; ++l) texels += static_cast<int64_t>(std::max(1, t->w >> l)) * std::max(1, t->h >> l);
    int64_t faces = t->target == GL_TEXTURE_CUBE_MAP ? 6 : t->target == GL_TEXTURE_CUBE_MAP_ARRAY ? 6 * t->layers : t->layers;
    int64_t depth = t->target == GL_TEXTURE_3D ? t->d : 1;
    int64_t bpp = t->f.compressed ? 1 : t->f.bytes;   // compressed formats: about one byte per texel (ASTC 4x4/ETC2 RGBA)
    return texels * faces * depth * bpp * t->samples;
}
inline uint64_t g_frame_no = 0;
inline uint64_t g_trace_triggered = ~0ull;   // frame requested through LOADER_GL_TRACE_TRIGGER
inline bool g_trace_dump_textures = true;    // profiler auto-traces skip the per-draw texture PNGs (seconds per frame)

// LOADER_TEX_PROFILE: texture lifetime, upload and sampling profiler. Logs [TEXPROF] anomalies with frame numbers
// (rate-limited per texture), a per-second summary, and with LOADER_FRAME_DIR traces and dumps the frame of the
// first LOADER_TEX_PROFILE anomalies (default 6).
inline bool tex_profiling() {
    static const bool on = getenv("LOADER_TEX_PROFILE") != nullptr;
    return on;
}
struct TexProfile {
    std::atomic<uint32_t> creates{0}, destroys{0}, uploads{0}, upload_kb{0}, renders{0}, anomalies{0};
    std::atomic<uint32_t> short_uniforms{0}, short_vertices{0}, dropped_draws{0}, bad_passes{0};
    std::atomic<bool> pending{false};
    int auto_traces = 0;
    std::mutex mu;
    std::map<std::pair<GLuint, int>, uint64_t> last_logged;
};
inline TexProfile g_texprof;
enum TexAnomaly { kUninit, kMissing, kFeedback, kChurn, kSkippedBlit, kUnimplemented, kReupload, kPlaceholderSwap, kPlaceholderUse,
                  kShortUniform, kShortVertex, kDroppedDraw, kBadPass, kPipeline };
inline const char* anomaly_name(int k) {
    static const char* n[] = {"sample-uninit", "sample-missing", "feedback", "churn", "skipped-blit", "unimplemented", "reupload",
                              "placeholder-swap", "placeholder-use", "short-uniform", "short-vertex", "dropped-draw", "bad-pass", "pipeline"};
    return n[k];
}
inline void tex_anomaly(int kind, GLuint id, const char* fmt, ...) {
    {
        std::lock_guard<std::mutex> l(g_texprof.mu);
        auto it = g_texprof.last_logged.find({id, kind});
        if (it != g_texprof.last_logged.end() && g_frame_no - it->second < 60) return;
        g_texprof.last_logged[{id, kind}] = g_frame_no;
    }
    if (kind != kReupload && kind != kUnimplemented && kind != kPlaceholderUse && kind != kPipeline) {
        ++g_texprof.anomalies;
        g_texprof.pending = true;
    }
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    printf("[TEXPROF] f%llu tid %d %s tex %u: %s\n", static_cast<unsigned long long>(g_frame_no), gettid(), anomaly_name(kind), id, msg);
}
inline void tex_profile_sample(const Tex* t, uint64_t handle, GLenum want, const GLuint* targets) {
    if (!t) {
        if (handle) tex_anomaly(kMissing, 0, "slot handle 0x%llx has no texture", static_cast<unsigned long long>(handle));
        return;
    }
    if (t->target != want) tex_anomaly(kMissing, t->id, "target 0x%x bound where shader wants 0x%x", t->target, want);
    if (!t->uploads && !t->renders)
        tex_anomaly(kUninit, t->id, "pf %llu %dx%d levels %d born f%llu, sampled with no upload or render",
                    static_cast<unsigned long long>(t->pf), t->w, t->h, t->levels, static_cast<unsigned long long>(t->born));
    for (int i = 0; i < 9; ++i)
        if (targets[i] && targets[i] == t->id) tex_anomaly(kFeedback, t->id, "sampled while attached to the current pass");
}
// A draw (program, sampler, vertex/index count) whose texture alternates between a real texture and a placeholder is
// the signature of an object briefly rendered with Unity's default texture.
inline void tex_profile_slot(const Tex* t, GLuint prog, size_t sampler, const std::string& name, uint64_t count) {
    struct Last { GLuint id; bool placeholder; int w, h; uint64_t pf; };
    // fully transparent placeholders draw nothing under Unity's blending; only visible ones can flash
    const bool visible = t && t->placeholder && (t->solid >> 24) != 0;
    static std::mutex mu;
    static std::map<std::tuple<GLuint, size_t, uint64_t>, Last> last;
    const Last now{t ? t->id : 0, visible, t ? t->w : 0, t ? t->h : 0, t ? t->pf : 0};
    Last before;
    {
        std::lock_guard<std::mutex> l(mu);
        auto [it, fresh] = last.try_emplace({prog, sampler, count}, now);
        before = it->second;
        it->second = now;
        if (fresh) {
            if (now.placeholder)
                tex_anomaly(kPlaceholderUse, now.id, "prog %u %s count %llu draws with placeholder colour %08x", prog, name.c_str(),
                            static_cast<unsigned long long>(count), t->solid);
            return;
        }
    }
    if (before.id != now.id && before.placeholder != now.placeholder)
        tex_anomaly(kPlaceholderSwap, now.id, "prog %u %s count %llu: tex %u pf %llu %dx%d%s -> tex %u pf %llu %dx%d%s", prog, name.c_str(),
                    static_cast<unsigned long long>(count), before.id, static_cast<unsigned long long>(before.pf), before.w, before.h,
                    before.placeholder ? " (placeholder)" : "", now.id, static_cast<unsigned long long>(now.pf), now.w, now.h,
                    now.placeholder ? " (placeholder)" : "");
}
// LOADER_GL_TRACE=<n>: log render passes and draws for the first n presented frames.
inline bool tracing() {
    static const uint64_t n = getenv("LOADER_GL_TRACE") ? strtoull(getenv("LOADER_GL_TRACE"), nullptr, 10) : 0;
    static const uint64_t at = getenv("LOADER_GL_TRACE_AT") ? strtoull(getenv("LOADER_GL_TRACE_AT"), nullptr, 10) : ~0ull;   // one frame
    return g_frame_no < n || g_frame_no == at || g_frame_no == g_trace_triggered;
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
    // Two stores take turns. A full one hands over to the other, so the ranges already pushed for the draw being
    // assembled stay intact; orphaning the store in place would drop them. buf is the store of the last push.
    GLuint stores[2] = {0, 0};
    size_t caps[2] = {0, 0};
    int cur = 0;
    size_t push(const void* data, size_t len, size_t align) {
        head = (head + align - 1) / align * align;
        if (!buf || head + len > caps[cur]) {
            if (buf) cur ^= 1;
            if (!stores[cur]) glGenBuffers(1, &stores[cur]);
            buf = stores[cur];
            caps[cur] = std::max(size, len);   // a single push larger than the ring gets a store of its own size
            glBindBuffer(target, buf);
            glBufferData(target, static_cast<GLsizeiptr>(caps[cur]), nullptr, GL_STREAM_DRAW);   // orphan
            head = 0;
        } else {
            glBindBuffer(target, buf);
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
    if (t->w <= 1 && t->h <= 1) {
        static std::atomic<int> tiny{0};
        if (tiny++ < 2) {
            fprintf(stderr, "[MTLGL] texture realized 1x1: pixelFormat %llu, %zu properties, class %s\n",
                    static_cast<unsigned long long>(t->pf), o->props.size(), o->s.c_str());
            for (const auto& kv : o->props) {
                if (kv.first.empty() || kv.first[0] == '#') continue;
                fprintf(stderr, "[MTLGL]     prop %s = %llu\n", kv.first.c_str(), static_cast<unsigned long long>(kv.second));
            }
        }
    }
    t->levels = static_cast<int>(std::max<uint64_t>(1, prop(o, "mipmapLevelCount", 1)));
    int full_chain = 1;   // glTexStorage rejects more levels than the size has
    for (int m = std::max(std::max(t->w, t->h), prop(o, "textureType", 2) == 7 ? t->d : 1); m > 1; m >>= 1) ++full_chain;
    t->levels = std::min(t->levels, full_chain);
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
    o->gltype = kGlTexture;
    g_texture_bytes += texture_bytes(t);
    t->born = g_frame_no;
    if (tex_profiling()) {
        ++g_texprof.creates;
        printf("[TEXPROF] f%llu tid %d create tex %u pf %llu %dx%dx%d levels %d target 0x%x usage %llu\n", static_cast<unsigned long long>(g_frame_no),
               gettid(), t->id, static_cast<unsigned long long>(t->pf), t->w, t->h, t->layers, t->levels, t->target,
               static_cast<unsigned long long>(prop(o, "usage", 0)));
    }
    gles::publish();
    return t;
}

// Uploads one region. bytes rows are bpr apart, images bpi apart (array slice / 3D depth handled by the caller).
inline void upload(Tex* t, int level, int slice, int x, int y, int z, int w, int h, int dz, const uint8_t* src, size_t bpr, size_t bpi) {
    if (!t || !src || w <= 0 || h <= 0) return;
    glBindTexture(t->target, t->id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    // PVRTC: whole levels. 4 bpp decodes on the GPU; 2 bpp, sRGB or no compute support decode on the CPU.
    if (t->f.pvrtc == 4 && t->f.internal == GL_RGBA8 && !x && !y) {
        while (glGetError() != GL_NO_ERROR) {}
        glBindTexture(t->target, 0);
        if (pvrtc_gpu_decode(t->id, t->target, level, slice, src, w, h)) return;
    }
    if (t->f.pvrtc) {
        const int bw = t->f.pvrtc == 2 ? 8 : 4;
        size_t blocks = static_cast<size_t>(std::max(w, bw * 2) / bw) * static_cast<size_t>(std::max(h, 8) / 4);
        std::vector<uint8_t> rgba;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        bool decoded = !x && !y && pvrtc::decode(src, blocks * 8, w, h, t->f.pvrtc == 2, rgba);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        static const bool log_times = getenv("LOADER_LOG_PVRTC") != nullptr;
        if (log_times)
            fprintf(stderr, "[MTLGL] PVRTC %dx%d level %d: %.1f ms\n", w, h, level, (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
        if (!decoded) {
            fprintf(stderr, "[MTLGL] PVRTC upload %dx%d at %d,%d not decoded\n", w, h, x, y);
            return;
        }
        Fmt saved = t->f;
        t->f.pvrtc = 0;
        upload(t, level, slice, 0, 0, z, w, h, 1, rgba.data(), static_cast<size_t>(w) * 4, static_cast<size_t>(w) * 4 * h);
        t->f = saved;
        return;
    }
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
    if (t->f.norm16) {   // widen 16-bit normalized texels to float
        const size_t comps = static_cast<size_t>(t->f.bytes / 4), srow = static_cast<size_t>(w) * comps * 2;
        if (bpr && bpr < srow) { fprintf(stderr, "[MTLGL] upload pf %llu: bytesPerRow %zu < %zu, skipped\n", static_cast<unsigned long long>(t->pf), bpr, srow); return; }
        swz.resize(row * h * dz);
        float* out = reinterpret_cast<float*>(swz.data());
        for (int zz = 0; zz < dz; ++zz)
            for (int r = 0; r < h; ++r) {
                const uint8_t* s = src + zz * bpi + r * (bpr ? bpr : srow);
                for (size_t c = 0; c < w * comps; ++c) {
                    uint16_t v;
                    std::memcpy(&v, s + 2 * c, 2);
                    *out++ = t->f.norm16 == 2 ? std::max(-1.0f, static_cast<int16_t>(v) / 32767.0f) : v / 65535.0f;
                }
            }
        p = swz.data();
        bpr = row;
        bpi = row * h;
    } else if (bpr && bpr < row) {
        fprintf(stderr, "[MTLGL] upload pf %llu %dx%d: bytesPerRow %zu < %zu, skipped\n", static_cast<unsigned long long>(t->pf), w, h, bpr, row);
        return;
    }
    if (t->f.bgra && !t->f.norm16) {
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
inline bool read_rgba(Tex* t, std::vector<uint8_t>& out, int level = 0) {
    ThreadGL& g = tgl();
    if (!g.init || !t || t->f.compressed || t->f.depth || level < 0 || level >= t->levels) return false;
    const int w = std::max(1, t->w >> level), h = std::max(1, t->h >> level);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g.read_fbo);
    attach(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, t, level, 0);
    out.resize(static_cast<size_t>(w) * h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
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
    if (uint64_t cmp = prop(d, "compareFunction", 0)) {   // MTLCompareFunctionNever (0): plain sampling
        s->compare = true;
        glSamplerParameteri(s->id, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glSamplerParameteri(s->id, GL_TEXTURE_COMPARE_FUNC, static_cast<GLint>(compare_fn(cmp)));
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
    {
        static std::atomic<int> calls{0};
        const int call = calls++;
        if (call < 12)
            fprintf(stderr, "[MTLGL] make_pipeline #%d vf=%s ff=%s%s\n", call, vf ? vf->s.c_str() : "(none)",
                    ff ? ff->s.c_str() : "(none)", desc ? "" : " (no descriptor)");
    }
    auto source_of = [](FakeObjcObj* fn, std::string& src, FakeObjcObj*& opts) {
        FakeObjcObj* lib = fn ? fake_lookup(prop(fn, "#library", 0)) : nullptr;
        if (!lib) return false;
        src = lib->s;
        opts = fake_lookup(prop(lib, "#options", 0));
        return true;
    };
    std::string vsrc, fsrc;
    FakeObjcObj *vo = nullptr, *fo = nullptr;
    if (!source_of(vf, vsrc, vo)) {
        uint64_t fnh = prop(desc, "vertexFunction", 0), libh = vf ? prop(vf, "#library", 0) : 0;
        FakeObjcObj* lib = fake_lookup(libh);
        fprintf(stderr, "[MTLGL] pipeline without vertex function source (fn %#llx props %zu, lib %#llx source %zu)\n",
                static_cast<unsigned long long>(fnh), vf ? vf->props.size() : 0, static_cast<unsigned long long>(libh),
                lib ? lib->s.size() : 0);
        return p;
    }
    p->vs_name = vf->s;
    p->fs_name = ff ? ff->s : std::string();
    p->vs = msl::translate(vsrc, macros_of(vo), clip_control(), vf->s);
    bool have_fs = source_of(ff, fsrc, fo);
    if (have_fs) p->fs = msl::translate(fsrc, macros_of(fo), clip_control(), ff->s);
    else p->fs.ok = true, p->fs.glsl = "#version 320 es\nvoid main() {}\n";
    if (!p->vs.ok) { dump_failed_shader("vertex translation", vsrc, "", p->vs.error); return p; }
    if (!p->fs.ok) { dump_failed_shader("fragment translation", fsrc, "", p->fs.error); return p; }
    if (getenv("LOADER_GL_SOLID")) {   // geometry debug: every fragment writes magenta, so coverage is visible
        std::string& g = p->fs.glsl;
        std::string outname;
        for (size_t i = g.find("out "); i != std::string::npos; i = g.find("out ", i + 4)) {
            const size_t semi = g.find(';', i);
            if (semi == std::string::npos) break;
            const std::string decl = g.substr(i, semi - i);
            if (decl.find("vec4") == std::string::npos) continue;
            const size_t sp = decl.find_last_of(" \t\n", decl.find("vec4") + 4);
            if (sp == std::string::npos) continue;
            outname = decl.substr(sp + 1);
            while (!outname.empty() && (outname.back() == ' ' || outname.back() == '\t' || outname.back() == '\n')) outname.pop_back();
            if (!outname.empty()) break;
        }
        if (!outname.empty()) {
            const std::string force = outname + " = vec4(1.0, 0.0, 1.0, 1.0);\n";
            g.insert(g.rfind('}'), force);
            const std::string solid = "{" + force + "return;}";
            for (size_t r = g.find("return;"); r != std::string::npos && r < g.rfind('}'); r = g.find("return;", r + solid.size()))
                g.replace(r, 7, solid);
            static std::atomic<int> n{0};
            if (n++ < 3) fprintf(stderr, "[MTLGL] SOLID: forcing %s magenta\n", outname.c_str());
        } else {
            static std::atomic<int> n{0};
            if (n++ < 2) fprintf(stderr, "[MTLGL] SOLID: no vec4 output in fragment shader (%zu bytes)\n", g.size());
        }
    }
    if (getenv("LOADER_GL_SOLID_VS")) {   // geometry debug: ignore the vertex transform, cover the whole target
        std::string& g = p->vs.glsl;
        const size_t end = g.rfind('}');
        if (end != std::string::npos)
            g.insert(end, "gl_Position = vec4(vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1)) * 2.0 - 1.0, 0.0, 1.0);\n");
    }
    std::string log;
    GLuint vs = compile(GL_VERTEX_SHADER, p->vs.glsl, log);
    if (!vs) { dump_failed_shader("vertex compile", vsrc, p->vs.glsl, log); return p; }
    GLuint fs = compile(GL_FRAGMENT_SHADER, p->fs.glsl, log);
    if (!fs) {
        fprintf(stderr, "[MTLGL] fragment compile failed (%s): %.300s\n", p->fs_name.c_str(), log.c_str());
        dump_failed_shader("fragment compile", fsrc, p->fs.glsl, log);
        glDeleteShader(vs);
        return p;
    }
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
    {
        static std::atomic<int> made{0};
        if (made++ < 12) fprintf(stderr, "[MTLGL] pipeline %s / %s -> prog %u\n", p->vs_name.c_str(), p->fs_name.c_str(), prog);
    }
    {
        static std::mutex mu;
        static std::set<std::string> seen;
        std::lock_guard<std::mutex> l(mu);
        const std::string key = p->vs_name + " / " + p->fs_name;
        if (seen.insert(key).second)
            fprintf(stderr, "[MTLGL] pipeline new #%zu: %s -> prog %u\n", seen.size(), key.c_str(), prog);
    }
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

// Draw-sequence profile: every pass and draw of a frame as (program, texture, scissor, viewport). A frame whose sequence
// differs from the previous one logs the differing span, which exposes draws that are missing, extra or reordered.
struct SeqEntry {
    GLuint prog, tex;   // prog 0: pass start
    int a[4], b[4];     // draw: scissor, viewport; pass: load action, w, h
    bool operator==(const SeqEntry& o) const { return prog == o.prog && tex == o.tex && !std::memcmp(a, o.a, sizeof a) && !std::memcmp(b, o.b, sizeof b); }
};
inline std::vector<SeqEntry> g_seq, g_seq_prev;
inline std::mutex g_seq_mu;
inline std::set<pid_t> g_draw_tids;   // threads that issued draws since the last summary
// Late-write check: bytes a draw read at encode time, re-hashed at commit. Metal lets the CPU fill buffers until the
// command buffer is committed; this backend reads them at encode time, so a change in between means stale data was drawn.
struct ReadRange { const uint8_t* p; size_t n; uint64_t hash; GLuint prog; char kind; };
inline std::vector<ReadRange> g_reads;
inline uint64_t hash_bytes(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
inline void tex_profile_read(const uint8_t* p, size_t n, GLuint prog, char kind) {
    if (!p || !n) return;
    std::lock_guard<std::mutex> l(g_seq_mu);
    g_reads.push_back({p, n, hash_bytes(p, n), prog, kind});
}
inline void tex_profile_pass(GLuint target, int load, int w, int h) {
    std::lock_guard<std::mutex> l(g_seq_mu);
    (void)target;   // drawables rotate every frame; comparing their ids would flag every frame
    g_seq.push_back({0, 0, {load, w, h, 0}, {}});
}
inline void tex_profile_draw(const Encoder* e) {
    SeqEntry s{e->pipe->prog, 0, {}, {}};
    for (const msl::SamplerUse& su : e->pipe->fs.samplers)
        if (su.texture >= 0 && su.texture < 32)
            if (FakeObjcObj* o = fake_lookup(e->ftex[su.texture]))
                if (const Tex* t = gl_obj<Tex>(o)) { s.tex = t->id; break; }
    std::memcpy(s.a, e->scissor, sizeof s.a);
    std::memcpy(s.b, e->viewport, sizeof s.b);
    std::lock_guard<std::mutex> l(g_seq_mu);
    g_seq.push_back(s);
    g_draw_tids.insert(gettid());
}
inline std::string seq_entry_text(const SeqEntry& s) {
    char b[160];
    if (!s.prog) snprintf(b, sizeof b, "PASS load %d %dx%d", s.a[0], s.a[1], s.a[2]);
    else snprintf(b, sizeof b, "prog %u tex %u sc %d,%d %dx%d vp %d,%d %dx%d", s.prog, s.tex, s.a[0], s.a[1], s.a[2], s.a[3], s.b[0], s.b[1], s.b[2], s.b[3]);
    return b;
}
// Called at present; logs how this frame's sequence differs from the last one.
// Called at every command buffer commit, before completion handlers can let the app reuse the memory.
inline void tex_profile_check_reads(uint64_t n) {
    std::lock_guard<std::mutex> l(g_seq_mu);
    {
        size_t changed = 0, bytes = 0;
        std::map<std::pair<GLuint, char>, int> by;
        for (const ReadRange& r : g_reads)
            if (hash_bytes(r.p, r.n) != r.hash) { ++changed; bytes += r.n; ++by[{r.prog, r.kind}]; }
        if (changed) {
            std::string who;
            for (auto& kv : by) who += " prog" + std::to_string(kv.first.first) + "/" + kv.first.second + "x" + std::to_string(kv.second);
            printf("[LATE] f%llu: %zu of %zu draw reads changed before commit (%zu bytes):%s\n", static_cast<unsigned long long>(n), changed,
                   g_reads.size(), bytes, who.c_str());
        }
        g_reads.clear();
    }
}
inline void tex_profile_frame(uint64_t n) {
    std::lock_guard<std::mutex> l(g_seq_mu);
    const std::vector<SeqEntry>& a = g_seq_prev;
    const std::vector<SeqEntry>& b = g_seq;
    size_t head = 0;
    while (head < a.size() && head < b.size() && a[head] == b[head]) ++head;
    size_t tail = 0;
    while (tail < a.size() - head && tail < b.size() - head && a[a.size() - 1 - tail] == b[b.size() - 1 - tail]) ++tail;
    if (!a.empty() && (head != a.size() || a.size() != b.size())) {
        const size_t da = a.size() - head - tail, db = b.size() - head - tail;
        printf("[SEQ] f%llu: %zu -> %zu entries, differs at %zu (-%zu +%zu)\n", static_cast<unsigned long long>(n), a.size(), b.size(), head, da, db);
        for (size_t i = 0; i < da && i < 6; ++i) printf("[SEQ]   - %s\n", seq_entry_text(a[head + i]).c_str());
        for (size_t i = 0; i < db && i < 6; ++i) printf("[SEQ]   + %s\n", seq_entry_text(b[head + i]).c_str());
    }
    g_seq_prev.swap(g_seq);
    g_seq.clear();
}

inline bool debug_clear_size_ok(int w, int h) {   // one-shot fuller-frame dump right after the clears
    static std::atomic<int> n{0};
    return w > 100 && h > 100 && n++ < 2;
}

// ---- LOADER_PASS_TRACE: what the guest does inside a pass -------------------------------------------------------
// Recorded between renderCommandEncoderWithDescriptor: and endEncoding, printed once per pass (max 12 per run) as one
// block: state bound, every draw entry point, draws dropped before rasterising with the code's reason, then the pass
// size and its final draw count. LOADER_PASS_TEX_DUMP=<frame> reuses the same observation for its texture dump.
inline bool pass_tracing() {
    static const bool on = [] {
        const char* v = getenv("LOADER_PASS_TRACE");
        return v && v[0] && !(v[0] == '0' && v[1] == '\0');
    }();
    return on;
}
inline bool pass_tex_dump_wanted() {
    static const bool on = getenv("LOADER_PASS_TEX_DUMP") != nullptr;
    return on;
}
inline bool pass_observe() {
    static const bool on = pass_tracing() || pass_tex_dump_wanted();
    return on;
}
// The GL texture behind a bound texture handle (follows texture views), without realizing a new one.
inline Tex* trace_tex(uint64_t handle) {
    FakeObjcObj* o = handle ? fake_lookup(handle) : nullptr;
    for (int i = 0; o && i < 4; ++i) {
        if (Tex* t = gl_obj<Tex>(o)) return t;
        const uint64_t src = prop(o, "#viewOf", 0);
        o = src ? fake_lookup(src) : nullptr;
    }
    return nullptr;
}
inline void pass_note_pipe(Encoder* e, uint64_t handle) {
    ++e->pass.pipe_sets;
    Pipeline* p = gl_obj<Pipeline>(fake_lookup(handle));
    const std::string key = p ? "\"" + p->vs_name + "\" / \"" + p->fs_name + "\" prog " + std::to_string(p->prog) : std::string("(null pipeline)");
    for (auto& kv : e->pass.pipes)
        if (kv.first == key) { ++kv.second; return; }
    if (e->pass.pipes.size() < 6) e->pass.pipes.push_back({key, 1});
    else ++e->pass.pipe_extra;
}
inline void pass_note_ds(Encoder* e, uint64_t handle) {
    ++e->pass.ds_sets;
    e->pass.ds_obj = handle;
    DepthStencil* ds = gl_obj<DepthStencil>(fake_lookup(handle));
    e->pass.ds_func = ds ? static_cast<int>(ds->depth_func) : -1;
    e->pass.ds_write = ds && ds->depth_write;
    e->pass.ds_stencil = ds && ds->stencil;
}
inline void pass_note_viewport(Encoder* e, const double* v) {
    ++e->pass.vp_sets;
    for (int i = 0; i < 6; ++i) e->pass.vpv[i] = v[i];
}
inline void pass_note_scissor(Encoder* e, const uint64_t* r) {
    ++e->pass.sc_sets;
    for (int i = 0; i < 4; ++i) e->pass.scv[i] = r[i];
}
inline void pass_note_bind(PassTrace& pt, bool vtx, int idx, uint64_t obj, uint64_t off) {
    std::vector<PassTrace::Bound>& v = vtx ? pt.vb : pt.fb;
    for (auto& b : v)
        if (!b.bytes && b.idx == idx && b.obj == obj && b.off == off) { ++b.n; return; }
    if (v.size() < 8) v.push_back(PassTrace::Bound{idx, obj, off, 1, 0, 0, 0, 0, false});
}
inline void pass_note_bytes(PassTrace& pt, bool vtx, int idx, uint64_t len) {
    std::vector<PassTrace::Bound>& v = vtx ? pt.vb : pt.fb;
    for (auto& b : v)
        if (b.bytes && b.idx == idx && b.obj == len) { ++b.n; return; }
    if (v.size() < 8) v.push_back(PassTrace::Bound{idx, len, 0, 1, 0, 0, 0, 0, true});
}
inline void pass_note_tex(PassTrace& pt, bool vtx, int idx, uint64_t handle) {
    std::vector<PassTrace::Bound>& v = vtx ? pt.vt : pt.ft;
    for (auto& b : v)
        if (b.idx == idx && b.obj == handle) { ++b.n; return; }
    if (v.size() < 8) {
        Tex* t = trace_tex(handle);
        v.push_back(PassTrace::Bound{idx, handle, 0, 1, t ? static_cast<int>(t->id) : 0, t ? t->w : 0, t ? t->h : 0, t ? t->pf : 0, false});
    }
}
inline void pass_note_draw(Encoder* e, const std::string& text) {
    for (auto& kv : e->pass.drawlog)
        if (kv.first == text) { ++kv.second; return; }
    if (e->pass.drawlog.size() < 8) e->pass.drawlog.push_back({text, 1});
    else ++e->pass.draw_extra;
}
inline void pass_note_reject(Encoder* e, const std::string& why) {
    if (!e) return;
    ++e->pass.rejected;
    for (auto& kv : e->pass.rejects)
        if (kv.first == why) { ++kv.second; return; }
    if (e->pass.rejects.size() < 8) e->pass.rejects.push_back({why, 1});
    else ++e->pass.reject_extra;
}
// The reason the draw path gave for not rasterising; does not call ready() again (its skip counter advances per call).
inline const char* draw_reject_reason(Encoder* e, uint64_t count) {
    if (!count) return "count 0";
    if (!e) return "no encoder";
    if (!e->valid) return "framebuffer incomplete";
    if (!e->pipe) return "no pipeline bound";
    if (!e->pipe->prog) return "pipeline has no GL program";
    return "dropped by LOADER_GL_SKIP_DRAW";
}
// First texture the fragment stage samples: what the draw actually reads.
inline Tex* pass_sample_tex(Encoder* e) {
    if (Pipeline* p = e->pipe)
        for (const msl::SamplerUse& su : p->fs.samplers)
            if (su.texture >= 0 && su.texture < 32)
                if (Tex* t = trace_tex(e->ftex[su.texture])) return t;
    return trace_tex(e->ftex[0]);
}
inline void pass_note_accepted(Encoder* e) {
    ++e->pass.draws;
    if (!e->pass.sampled) e->pass.sampled = pass_sample_tex(e);
    if (e->pipe) {
        e->pass.last_vs = e->pipe->vs_name;
        e->pass.last_fs = e->pipe->fs_name;
    }
}
inline std::string pass_draw_text(const char* entry, uint64_t prim, uint64_t start, uint64_t count, uint64_t inst) {
    return std::string(entry) + " count=" + std::to_string(count) + " start=" + std::to_string(start) + " prim=" + std::to_string(prim) +
           " inst=" + std::to_string(inst);
}
inline std::string pass_draw_indexed_text(uint64_t prim, uint64_t count, uint64_t type, uint64_t ibuf, uint64_t off, uint64_t inst) {
    char h[32];
    snprintf(h, sizeof h, "0x%llx", static_cast<unsigned long long>(ibuf));
    return "drawIndexedPrimitives count=" + std::to_string(count) + " type=" + std::to_string(type) + " prim=" + std::to_string(prim) + " ibuf=" + h +
           "+" + std::to_string(off) + " inst=" + std::to_string(inst);
}
inline void pass_trace_end(Encoder* e) {
    if (!e || !e->pass.id || !pass_tracing()) return;
    static std::mutex mu;
    static int printed = 0;
    std::string out;
    {
        std::lock_guard<std::mutex> l(mu);
        if (printed >= 12) {
            if (printed == 12) {
                fprintf(stderr, "[PASSTRACE] 12 passes traced; no more (LOADER_PASS_TRACE is bounded)\n");
                printed = 13;
            }
            return;
        }
        ++printed;
        const PassTrace& p = e->pass;
        auto add = [&out](const char* fmt, ...) {
            char line[1024];
            va_list ap;
            va_start(ap, fmt);
            vsnprintf(line, sizeof line, fmt, ap);
            va_end(ap);
            out += line;
        };
        add("[PASSTRACE] pass %d %dx%d color=%d depth=%d valid=%d pipelineSets=%d\n", p.id, p.w, p.h, p.colors, p.depth ? 1 : 0,
            p.valid ? 1 : 0, p.pipe_sets);
        if (!p.pipes.empty()) {
            std::string s;
            for (const auto& kv : p.pipes) {
                if (!s.empty()) s += ", ";
                s += kv.first;
                if (kv.second > 1) s += " x" + std::to_string(kv.second);
            }
            if (p.pipe_extra) s += " +" + std::to_string(p.pipe_extra) + " more";
            add("[PASSTRACE]   pipeline %s\n", s.c_str());
        }
        auto binds = [](const std::vector<PassTrace::Bound>& v) {
            std::string s;
            for (const PassTrace::Bound& b : v) {
                char one[128];
                if (b.bytes) snprintf(one, sizeof one, " [%d]=bytes(%llu)", b.idx, static_cast<unsigned long long>(b.obj));
                else snprintf(one, sizeof one, " [%d]=0x%llx+%llu", b.idx, static_cast<unsigned long long>(b.obj),
                              static_cast<unsigned long long>(b.off));
                s += one;
                if (b.n > 1) s += " x" + std::to_string(b.n);
            }
            return s.empty() ? std::string("none") : s;
        };
        auto texts = [](const std::vector<PassTrace::Bound>& v) {
            std::string s;
            for (const PassTrace::Bound& b : v) {
                char one[160];
                if (b.texid)
                    snprintf(one, sizeof one, " [%d]=0x%llx(gl tex %d %dx%d pf %llu)", b.idx, static_cast<unsigned long long>(b.obj), b.texid, b.tw,
                             b.th, static_cast<unsigned long long>(b.pf));
                else snprintf(one, sizeof one, " [%d]=0x%llx(no GL texture)", b.idx, static_cast<unsigned long long>(b.obj));
                s += one;
                if (b.n > 1) s += " x" + std::to_string(b.n);
            }
            return s.empty() ? std::string("none") : s;
        };
        add("[PASSTRACE]   vbuf %s\n[PASSTRACE]   fbuf %s\n", binds(p.vb).c_str(), binds(p.fb).c_str());
        add("[PASSTRACE]   vtex %s\n[PASSTRACE]   ftex %s\n", texts(p.vt).c_str(), texts(p.ft).c_str());
        if (p.vp_sets)
            add("[PASSTRACE]   viewport %g,%g %gx%g znear %g zfar %g x%d\n", p.vpv[0], p.vpv[1], p.vpv[2], p.vpv[3], p.vpv[4], p.vpv[5],
                p.vp_sets);
        else add("[PASSTRACE]   viewport (never set; pass rect used)\n");
        if (p.sc_sets)
            add("[PASSTRACE]   scissor %llu,%llu %llux%llu x%d\n", static_cast<unsigned long long>(p.scv[0]),
                static_cast<unsigned long long>(p.scv[1]), static_cast<unsigned long long>(p.scv[2]),
                static_cast<unsigned long long>(p.scv[3]), p.sc_sets);
        else add("[PASSTRACE]   scissor (never set; pass rect used)\n");
        if (p.ds_sets)
            add("[PASSTRACE]   depthstencil 0x%llx func 0x%x write %d stencil %d x%d\n", static_cast<unsigned long long>(p.ds_obj), p.ds_func,
                p.ds_write, p.ds_stencil, p.ds_sets);
        else add("[PASSTRACE]   depthstencil (never set; depth test off, no write)\n");
        std::string d;
        for (const auto& kv : p.drawlog) {
            if (!d.empty()) d += ", ";
            d += kv.first;
            if (kv.second > 1) d += " x" + std::to_string(kv.second);
        }
        if (p.draw_extra) d += " +" + std::to_string(p.draw_extra) + " more";
        add("[PASSTRACE]   draws %d: %s\n", p.draws, d.empty() ? "(no draw call)" : d.c_str());
        if (p.rejected) {
            std::string r;
            for (const auto& kv : p.rejects) {
                if (!r.empty()) r += ", ";
                r += "\"" + kv.first + "\"";
                if (kv.second > 1) r += " x" + std::to_string(kv.second);
            }
            if (p.reject_extra) r += " +" + std::to_string(p.reject_extra) + " more";
            add("[PASSTRACE]   rejected %d: %s\n", p.rejected, r.c_str());
        }
        add("[PASSTRACE] end pass %d: %dx%d draws=%d rejected=%d\n", p.id, p.w, p.h, p.draws, p.rejected);
    }
    fputs(out.c_str(), stderr);
    fflush(stderr);
}
// LOADER_PASS_TEX_DUMP=<frame>: at that frame, dump the texture the 1080x2340 no-depth pass (the guest's own
// vertex/fragment pipeline over the screen target) samples, and print its colour histogram.
inline void pass_tex_dump(Encoder* e) {
    static const uint64_t want = getenv("LOADER_PASS_TEX_DUMP") ? strtoull(getenv("LOADER_PASS_TEX_DUMP"), nullptr, 10) : 0;
    static bool done = false;
    if (!e || !want || done || !e->pass.id || e->rt_w != 1080 || e->rt_h != 2340 || e->pass.depth) return;
    const uint64_t frame = g_frame_no + 1;   // passes run before the commit that numbers the frame
    if (frame != want) return;
    Tex* t = e->pass.sampled;
    if (!t && !e->pass.ft.empty()) t = trace_tex(e->pass.ft[0].obj);
    done = true;
    std::vector<uint8_t> px;
    if (!t || !read_rgba(t, px) || px.empty()) {
        fprintf(stderr, "[PASSTEX] frame %llu pass %d %dx%d: sampled texture %s not readable\n", static_cast<unsigned long long>(frame), e->pass.id,
                e->rt_w, e->rt_h, t ? "(glReadPixels failed)" : "(none bound)");
        return;
    }
    std::map<uint32_t, uint32_t> hist;
    uint64_t other = 0;
    const uint64_t np = px.size() / 4;
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
        const uint32_t k = static_cast<uint32_t>(px[i]) << 24 | static_cast<uint32_t>(px[i + 1]) << 16 | static_cast<uint32_t>(px[i + 2]) << 8 | px[i + 3];
        auto it = hist.find(k);
        if (it != hist.end()) ++it->second;
        else if (hist.size() < 4096) hist.emplace(k, 1);
        else ++other;
    }
    std::vector<std::pair<uint32_t, uint32_t>> top(hist.begin(), hist.end());
    std::sort(top.begin(), top.end(), [](const std::pair<uint32_t, uint32_t>& a, const std::pair<uint32_t, uint32_t>& b) { return a.second > b.second; });
    std::string s;
    for (size_t i = 0; i < top.size() && i < 8; ++i) {
        char one[64];
        snprintf(one, sizeof one, "%08x %.2f%%%s", top[i].first, 100.0 * top[i].second / static_cast<double>(np),
                 i + 1 < top.size() && i < 7 ? ", " : "");
        s += one;
    }
    fprintf(stderr,
            "[PASSTEX] frame %llu pass %d %dx%d vs \"%s\" fs \"%s\": samples tex %u %dx%d pf %llu, %llu px, %s%zu distinct colours, top: %s\n",
            static_cast<unsigned long long>(frame), e->pass.id, e->rt_w, e->rt_h, e->pass.last_vs.c_str(), e->pass.last_fs.c_str(), t->id, t->w,
            t->h, static_cast<unsigned long long>(t->pf), static_cast<unsigned long long>(np), other ? "4096+ (capped) " : "", hist.size(), s.c_str());
    if (const char* dir = getenv("LOADER_FRAME_DIR")) {
        char path[512];
        snprintf(path, sizeof path, "%s/pass_tex_f%llu_tex%u_%dx%d.png", dir, static_cast<unsigned long long>(frame), t->id, t->w, t->h);
        const bool ok = gles::write_png(path, t->w, t->h, px.data());
        fprintf(stderr, "[PASSTEX]   dumped %s (%s)\n", path, ok ? "ok" : "WRITE FAILED");
    }
}
inline void begin_pass(Encoder* e, FakeObjcObj* pass) {
    ThreadGL& g = tgl();
    if (!g.init) return;
    {
        static std::atomic<uint64_t> prev_draws{0};
        static std::atomic<int> pseq{0};
        if (pseq.load() < 24) {
            if (pseq.load() > 0)
                fprintf(stderr, "[MTLGL] pass %d issued %llu draws\n", pseq.load() - 1,
                        static_cast<unsigned long long>(g_draws_frame.load() - prev_draws.load()));
            prev_draws = g_draws_frame.load();
            ++pseq;
        }
    }
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
        e->targets[i] = t->id;
        ++t->renders;
        int level = static_cast<int>(prop(c, "level", 0));
        int slice = static_cast<int>(t->target == GL_TEXTURE_3D ? prop(c, "depthPlane", 0) : prop(c, "slice", 0));
        attach(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, t, level, slice);
        static std::atomic<int> at_logged{0};
        if (at_logged++ < 8)
            fprintf(stderr, "[MTLGL] attach color%d: tex %u target 0x%x internal 0x%x %dx%d level %d samples %d err 0x%x\n", i, t->id,
                    t->target, t->f.internal, t->w, t->h, level, t->samples, glGetError());
        if (t->w <= 4 && t->h <= 4) {
            static std::atomic<int> tiny_passes{0};
            if (tiny_passes++ < 8)
                fprintf(stderr, "[MTLGL] tiny pass target: color tex %u %dx%d pf %llu\n", t->id, t->w, t->h,
                        static_cast<unsigned long long>(t->pf));
        }
        bufs[i] = GL_COLOR_ATTACHMENT0 + i;
        nbufs = i + 1;
        w = std::max(1, t->w >> level);
        h = std::max(1, t->h >> level);
        if (w > 100 && h > 100 && prop(c, "texture", 0)) g_last_big_target_obj = prop(c, "texture", 0);
        if (prop(c, "loadAction", 0) == 2) {
            Clear cl{i, {propd(c, "#clear0", 0), propd(c, "#clear1", 0), propd(c, "#clear2", 0), propd(c, "#clear3", 1)}, t->f.integer};
            clears.push_back(cl);
        }
        uint64_t store = prop(c, "storeAction", 1);
        Tex* rt = (store == 2 || store == 3) ? texture(fake_lookup(prop(c, "resolveTexture", 0))) : nullptr;
        if (rt && e->nresolve < 8) e->resolve[e->nresolve++] = {t->id, rt->id, w, h, static_cast<GLenum>(GL_COLOR_ATTACHMENT0 + i)};
    }
    if (!nbufs) bufs[0] = GL_NONE;
    if (!nbufs) {
        static thread_local bool warned = false;
        if (!warned) {   // where the attachment chain breaks: descriptor array, its first child, that child's texture
            warned = true;
            uint64_t arr = prop(pass, "colorAttachments", 0);
            FakeObjcObj* c0 = fake_lookup(prop(fake_lookup(arr), "#obj0", 0));
            fprintf(stderr, "[MTLGL] pass without color target: colorAttachments=%#llx child0=%p texture=%#llx\n",
                    static_cast<unsigned long long>(arr), static_cast<void*>(c0),
                    static_cast<unsigned long long>(c0 ? prop(c0, "texture", 0) : 0));
        }
    }
    glDrawBuffers(nbufs ? nbufs : 1, bufs);
    FakeObjcObj* da = fake_lookup(prop(pass, "depthAttachment", 0));
    FakeObjcObj* sa = fake_lookup(prop(pass, "stencilAttachment", 0));
    Tex* dt = da ? texture(fake_lookup(prop(da, "texture", 0))) : nullptr;
    Tex* st = sa ? texture(fake_lookup(prop(sa, "texture", 0))) : nullptr;
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
    if (dt) { e->targets[8] = dt->id; ++dt->renders; }
    (dt ? g_passes_depth : g_passes_nodepth) += 1;
    if (st) ++st->renders;
    if (tex_profiling()) ++g_texprof.renders;
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
    if (pass_observe()) {
        static std::atomic<int> next{1};
        e->pass.id = next++;
        e->pass.w = w;
        e->pass.h = h;
        e->pass.colors = nbufs;
        e->pass.depth = dt != nullptr;
        e->pass.valid = e->valid;
    }
    static std::atomic<int> pass_logged{0};
    if (pass_logged++ < 24) {
        GLint bound = 0, obj = 0, objtype = 0, objlevel = 0, err = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &bound);
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &obj);
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &objtype);
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL, &objlevel);
        err = glGetError();
        GLint immutable = 0, is_tex = 0;
        if (nbufs) {
            glIsTexture(obj);
            is_tex = glIsTexture(obj);
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(obj));
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_IMMUTABLE_FORMAT, &immutable);
        }
        GLenum alt = 0;
        if (status != GL_FRAMEBUFFER_COMPLETE) {   // same framebuffer with a texture known to be good
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.dummy2d, 0);
            alt = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, static_cast<GLuint>(obj), 0);
        }
        const uint64_t ca_la = prop(child(pass, "colorAttachments", 0), "loadAction", 0);
        const uint64_t ca_st = prop(child(pass, "colorAttachments", 0), "storeAction", 0);
        fprintf(stderr,
                "[MTLGL] pass %d: colors=%d depth=%d %dx%d status=0x%x fbo=%d att0=%d type=0x%x lvl=%d err=0x%x istex=%d immutable=%d "
                "dummy_status=0x%x colorLA=%llu colorST=%llu clear=%g,%g,%g,%g depthLA=%llu\n",
                pass_logged.load(), nbufs, dt != nullptr, w, h, status, bound, obj, objtype, objlevel, err, is_tex, immutable, alt,
                static_cast<unsigned long long>(ca_la), static_cast<unsigned long long>(ca_st),
                propd(child(pass, "colorAttachments", 0), "#clear0", 0), propd(child(pass, "colorAttachments", 0), "#clear1", 0),
                propd(child(pass, "colorAttachments", 0), "#clear2", 0), propd(child(pass, "colorAttachments", 0), "#clear3", 1),
                static_cast<unsigned long long>(prop(da, "loadAction", 2)));
    }
    if (!e->valid) {
        if (tex_profiling()) {
            ++g_texprof.bad_passes;
            tex_anomaly(kBadPass, 0, "framebuffer incomplete 0x%x (%d color, depth %d)", status, nbufs, dt != nullptr);
        }
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
            static const bool debug_clear = getenv("LOADER_DEBUG_CLEAR") != nullptr;   // coverage test: paint the target magenta
            if (debug_clear && w > 100) {
                f[0] = 1.0f;
                f[1] = 0.0f;
                f[2] = 1.0f;
                f[3] = 1.0f;
            }
            glClearBufferfv(GL_COLOR, c.i, f);
        }
    }
    // Metal's default load action is DontCare for colour but Clear for depth and stencil attachments.
    bool clear_d = dt && prop(da, "loadAction", 2) == 2, clear_s = st && prop(sa, "loadAction", 2) == 2;
    if (clear_d) glDepthMask(GL_TRUE);
    if (clear_s) glStencilMask(0xff);
    float cd = static_cast<float>(propd(da, "clearDepth", 1.0));
    GLint cs = static_cast<GLint>(prop(sa, "clearStencil", 0));
    if (clear_d && clear_s && dt == st) glClearBufferfi(GL_DEPTH_STENCIL, 0, cd, cs);
    else {
        if (clear_d) glClearBufferfv(GL_DEPTH, 0, &cd);
        if (clear_s) glClearBufferiv(GL_STENCIL, 0, &cs);
    }
    static const bool dbg_clear_dump = getenv("LOADER_DEBUG_CLEAR_DUMP") != nullptr;
    if (dbg_clear_dump && debug_clear_size_ok(w, h)) {
        std::vector<uint8_t> px(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g.fbo);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
        const bool ok = gles::write_png("/data/local/tmp/jc/frames/dbg_after_clear.png", w, h, px.data());
        fprintf(stderr, "[MTLGL] debug dump after clear %dx%d -> %s\n", w, h, ok ? "written" : "WRITE FAILED");
    }
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, w, h);
    glViewport(0, 0, w, h);
    e->scissor[2] = e->viewport[2] = w;
    e->scissor[3] = e->viewport[3] = h;
    if (tex_profiling()) tex_profile_pass(e->targets[0], static_cast<int>(prop(child(pass, "colorAttachments", 0), "loadAction", 0)), w, h);
    glDepthRangef(0, 1);
    if (tracing()) {
        FakeObjcObj* c0 = child(pass, "colorAttachments", 0);
        Tex* t0 = c0 ? texture(fake_lookup(prop(c0, "texture", 0))) : nullptr;
        printf("[GLTRACE] pass %dx%d colors %d (c0 tex %u pf %llu load %llu clear %.2f,%.2f,%.2f,%.2f) depth %d pf %llu load %lld stencil %d pf %llu "
               "load %lld (-1 = unset) err 0x%x\n", w, h, nbufs,
               t0 ? t0->id : 0, t0 ? static_cast<unsigned long long>(t0->pf) : 0ull, static_cast<unsigned long long>(prop(c0, "loadAction", 0)),
               propd(c0, "#clear0", 0), propd(c0, "#clear1", 0), propd(c0, "#clear2", 0), propd(c0, "#clear3", 1), dt != nullptr,
               dt ? static_cast<unsigned long long>(dt->pf) : 0ull, static_cast<long long>(prop(da, "loadAction", ~0ull)), st != nullptr,
               st ? static_cast<unsigned long long>(st->pf) : 0ull, static_cast<long long>(prop(sa, "loadAction", ~0ull)), glGetError());
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
    pass_trace_end(e);
    pass_tex_dump(e);
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
    static const bool no_depth = getenv("LOADER_GL_NO_DEPTH") != nullptr, no_stencil = getenv("LOADER_GL_NO_STENCIL") != nullptr;   // debug
    if (!no_depth && (ds->depth_func != GL_ALWAYS || ds->depth_write)) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(ds->depth_func);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(ds->depth_write ? GL_TRUE : GL_FALSE);
    if (ds->stencil && !no_stencil) {
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

inline void bind_resources(Encoder* e, ThreadGL& g, uint64_t count) {
    Pipeline* p = e->pipe;
    auto blocks = [&](const std::vector<msl::BlockUse>& bl, const std::vector<int>& bind, BoundBuf* bufs) {
        for (size_t i = 0; i < bl.size(); ++i) {
            size_t avail = 0;
            const uint8_t* src = bl[i].buffer >= 0 && bl[i].buffer < 32 ? bufs[bl[i].buffer].data(avail) : nullptr;
            static std::vector<uint8_t> zero;
            size_t n = static_cast<size_t>(bl[i].size);
            if (!src || avail < n) {
                if (tex_profiling()) {
                    ++g_texprof.short_uniforms;
                    tex_anomaly(kShortUniform, p->prog, "prog %u %s %s from buffer %d: shader reads %zu bytes, %zu bound (rest zero-filled)", p->prog,
                                bufs == e->vbuf ? "vs" : "fs", bl[i].block.c_str(), bl[i].buffer, n, src ? avail : 0);
                }
                if (zero.size() < n) zero.resize(n);
                std::fill(zero.begin(), zero.begin() + static_cast<long>(n), 0);
                if (src) std::memcpy(zero.data(), src, avail);
                src = zero.data();
            }
            if (tex_profiling() && src != zero.data() && bufs[bl[i].buffer].bytes.empty()) tex_profile_read(src, n, p->prog, 'u');
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
            if (tex_profiling()) {
                tex_profile_sample(t, su[i].texture >= 0 && su[i].texture < 32 ? tex[su[i].texture] : 0, target, e->targets);
                tex_profile_slot(t, p->prog, (tex == e->vtex ? 100 : 0) + i, su[i].uniform, count);
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
    std::map<uint64_t, std::pair<GLuint, size_t>> uploaded;   // buffer index -> ring store and offset
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
            if (tex_profiling() && n < need) {
                ++g_texprof.short_vertices;
                tex_anomaly(kShortVertex, p->prog, "prog %u vertex buffer %llu: draw reads %llu bytes, %zu bound", p->prog,
                            static_cast<unsigned long long>(a.buffer), static_cast<unsigned long long>(need), n);
            }
            if (tex_profiling() && e->vbuf[a.buffer].bytes.empty()) tex_profile_read(src, n, p->prog, 'v');
            const size_t at = g.vb.push(src, n, 16);
            uploaded[a.buffer] = {g.vb.buf, at};
        }
        glBindBuffer(GL_ARRAY_BUFFER, uploaded[a.buffer].first);
        const void* ptr = reinterpret_cast<const void*>(uploaded[a.buffer].second + a.offset);
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
    if (!ok && tex_profiling()) {
        ++g_texprof.dropped_draws;
        tex_anomaly(kDroppedDraw, e && e->pipe ? e->pipe->prog : 0, "draw dropped: encoder %d valid %d pipeline %d program %u", e != nullptr,
                    e && e->valid, e && e->pipe, e && e->pipe ? e->pipe->prog : 0);
    }
    if (ok && tex_profiling()) {
        static std::mutex mu;
        static std::map<GLuint, bool> logged;
        std::unique_lock<std::mutex> l(mu);
        if (logged.emplace(e->pipe->prog, true).second) {
            l.unlock();
            std::string attrs;
            for (const Attr& a : e->pipe->attrs)
                attrs += " loc" + std::to_string(a.loc) + ":fmt" + std::to_string(a.format) + "@buf" + std::to_string(a.buffer) + "+" + std::to_string(a.offset);
            const Blend& b = e->pipe->blend[0];
            tex_anomaly(kPipeline, e->pipe->prog, "attrs%s | blend %d 0x%x/0x%x a 0x%x/0x%x mask %llu | fs samplers %zu blocks %zu", attrs.c_str(), b.enabled,
                        b.src_rgb, b.dst_rgb, b.src_a, b.dst_a, static_cast<unsigned long long>(b.mask), e->pipe->fs.samplers.size(), e->pipe->fs.blocks.size());
        }
    }
    if (!ok && tracing()) printf("[GLTRACE] draw skipped: encoder %d valid %d pipe %d prog %u\n", e != nullptr, e && e->valid,
                                 e && e->pipe, e && e->pipe ? e->pipe->prog : 0);
    return ok;
}

// LOADER_GL_TRACE_PROBE=<x>,<y> (or "x,y" in the trigger file): print the render target pixel at x,y (Metal
// coordinates) after each traced draw.
inline int g_probe_x = -1, g_probe_y = -1;
inline void trace_probe() {
    static const bool env = [] {
        const char* s = getenv("LOADER_GL_TRACE_PROBE");
        return s && sscanf(s, "%d,%d", &g_probe_x, &g_probe_y) == 2;
    }();
    (void)env;
    const int px = g_probe_x, py = g_probe_y;
    if (px < 0 || py < 0) return;
    GLint draw_fb = 0, read_fb = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(draw_fb));
    uint8_t c[4] = {};
    glReadPixels(px, py, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(read_fb));
    printf("[GLTRACE]   probe %d,%d = %u %u %u %u err 0x%x\n", px, py, c[0], c[1], c[2], c[3], glGetError());
}

inline void draw(Encoder* e, uint64_t prim, uint64_t start, uint64_t count, uint64_t instances) {
    ThreadGL& g = tgl();
    if (!ready(e) || !count) {
        if (pass_observe()) pass_note_reject(e, draw_reject_reason(e, count));
        return;
    }
    if (pass_observe()) pass_note_accepted(e);
    glUseProgram(e->pipe->prog);
    apply_state(e);
    bind_resources(e, g, count);
    bind_vertices(e, g, start + count - 1, instances ? instances : 1);
    glDrawArraysInstanced(primitive(prim), static_cast<GLint>(start), static_cast<GLsizei>(count), static_cast<GLsizei>(instances ? instances : 1));
    if (tex_profiling()) tex_profile_draw(e);
    if (tracing()) printf("[GLTRACE] drawArrays prim %llu start %llu count %llu prog %u err 0x%x\n", static_cast<unsigned long long>(prim),
                          static_cast<unsigned long long>(start), static_cast<unsigned long long>(count), e->pipe->prog, glGetError());
    if (tracing()) trace_probe();
}

inline void draw_indexed(Encoder* e, uint64_t prim, uint64_t count, uint64_t type, FakeObjcObj* ibuf, uint64_t ioff, uint64_t instances) {
    ThreadGL& g = tgl();
    if (!ready(e) || !count || !ibuf || !ibuf->mem) {
        if (pass_observe())
            pass_note_reject(e, !count ? "index count 0" : !ibuf ? "index buffer nil" : !ibuf->mem ? "index buffer has no storage" : draw_reject_reason(e, count));
        static std::atomic<int> skipped{0};
        if (skipped++ < 6)
            fprintf(stderr, "[MTLGL] draw skipped: ready=%d count=%llu ibuf=%p mem=%p valid=%d pipe=%p prog=%u vs=%s\n",
                    ready(e) ? 1 : 0, static_cast<unsigned long long>(count), static_cast<void*>(ibuf), ibuf ? ibuf->mem : nullptr,
                    e->valid ? 1 : 0, static_cast<void*>(e->pipe), e->pipe ? e->pipe->prog : 0u,
                    e->pipe ? e->pipe->vs_name.c_str() : "(none)");
        return;
    }
    size_t isz = type == 1 ? 4 : 2;
    if (ioff + count * isz > ibuf->len) {
        if (pass_observe()) {
            char why[160];
            snprintf(why, sizeof why, "index range %llu+%llu*%zu past buffer end (%llu bytes)", static_cast<unsigned long long>(ioff),
                     static_cast<unsigned long long>(count), isz, static_cast<unsigned long long>(ibuf->len));
            pass_note_reject(e, why);
        }
        return;
    }
    const uint8_t* idx = static_cast<const uint8_t*>(ibuf->mem) + ioff;
    uint64_t maxv = 0;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t v = isz == 4 ? reinterpret_cast<const uint32_t*>(idx)[i] : reinterpret_cast<const uint16_t*>(idx)[i];
        if (v != (isz == 4 ? 0xffffffffull : 0xffffull) && v > maxv) maxv = v;
    }
    glUseProgram(e->pipe->prog);
    apply_state(e);
    bind_resources(e, g, count);
    if (pass_observe()) pass_note_accepted(e);
    bind_vertices(e, g, maxv, instances ? instances : 1);
    if (tex_profiling()) tex_profile_read(idx, count * isz, e->pipe->prog, 'i');
    size_t off = g.ib.push(idx, count * isz, 4);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ib.buf);
    glDrawElementsInstanced(primitive(prim), static_cast<GLsizei>(count), isz == 4 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT,
                            reinterpret_cast<const void*>(off), static_cast<GLsizei>(instances ? instances : 1));
    ++g_draws_frame;
    (e->targets[8] ? g_draws_depth : g_draws_nodepth) += 1;
    if (e->rt_w > 100 && e->rt_h > 100) g_last_big_target = e->targets[0];
    static const bool draw_dump = getenv("LOADER_DRAW_DUMP") != nullptr;   // coverage proof: read the target back right here
    static std::atomic<int> draw_dump_n{0};
    if (draw_dump && e->rt_w > 100 && e->rt_h > 100 && draw_dump_n++ < 3) {
        std::vector<uint8_t> px(static_cast<size_t>(e->rt_w) * static_cast<size_t>(e->rt_h) * 4, 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g.fbo);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, e->rt_w, e->rt_h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
        gles::write_png("/data/local/tmp/jc/frames/dbg_after_draw.png", e->rt_w, e->rt_h, px.data());
        fprintf(stderr, "[MTLGL] debug dump after draw %dx%d target tex %u err 0x%x\n", e->rt_w, e->rt_h, e->targets[0],
                glGetError());
    }
    static std::atomic<int> draws_logged{0};
    if (draws_logged++ < 4)
        fprintf(stderr, "[MTLGL] draw: prog %u count %llu maxv %llu attrs %zu rt %dx%d tex %u err 0x%x thread %zu\n", e->pipe->prog,
                static_cast<unsigned long long>(count), static_cast<unsigned long long>(maxv), e->pipe->attrs.size(), e->rt_w, e->rt_h,
                e->targets[0], glGetError(), std::hash<std::thread::id>{}(std::this_thread::get_id()));
    if (tex_profiling()) tex_profile_draw(e);
    if (tracing()) {
        printf("[GLTRACE] drawElements %s / %s prim %llu count %llu maxv %llu prog %u attrs %zu vsblocks %zu fs samplers %zu ftex0 %u err 0x%x\n",
               e->pipe->vs_name.c_str(), e->pipe->fs_name.c_str(),
               static_cast<unsigned long long>(prim), static_cast<unsigned long long>(count), static_cast<unsigned long long>(maxv),
               e->pipe->prog, e->pipe->attrs.size(), e->pipe->vs.blocks.size(), e->pipe->fs.samplers.size(),
               texture(fake_lookup(e->ftex[0])) ? texture(fake_lookup(e->ftex[0]))->id : 0, glGetError());
        const Blend& b0 = e->pipe->blend[0];
        for (const msl::SamplerUse& su : e->pipe->fs.samplers) {
            Tex* t = su.texture >= 0 && su.texture < 32 ? texture(fake_lookup(e->ftex[su.texture])) : nullptr;
            printf("[GLTRACE]   fs %s <- tex[%d] %s pf %llu %dx%d levels %d, sampler %d\n", su.uniform.c_str(), su.texture,
                   t ? "" : "(NONE)", t ? static_cast<unsigned long long>(t->pf) : 0ull, t ? t->w : 0, t ? t->h : 0, t ? t->levels : 0, su.sampler);
        }
        if (const DepthStencil* ds = e->ds)
            printf("[GLTRACE]   depth func 0x%x write %d | stencil %d front func 0x%x pass-op 0x%x ref %u read 0x%x write 0x%x | back func 0x%x "
                   "pass-op 0x%x ref %u | cull %llu winding %llu\n", ds->depth_func, ds->depth_write, ds->stencil, ds->front.func, ds->front.pass, e->ref_front,
                   ds->front.read, ds->front.write, ds->back.func, ds->back.pass, e->ref_back, static_cast<unsigned long long>(e->cull),
                   static_cast<unsigned long long>(e->winding));
        printf("[GLTRACE]   blend %d src 0x%x dst 0x%x mask %llu | vbuf0 %p+%llu fbuf0 %p\n", b0.enabled, b0.src_rgb, b0.dst_rgb,
               static_cast<unsigned long long>(b0.mask), static_cast<void*>(e->vbuf[0].buf), static_cast<unsigned long long>(e->vbuf[0].offset),
               static_cast<void*>(e->fbuf[0].buf));
        printf("[GLTRACE]   targets %u %u depth %u | drawable obj %#llx\n", e->targets[0], e->targets[1], e->targets[8],
               static_cast<unsigned long long>(g_presented_drawable.load()));
        {
            GLint sb[4] = {0, 0, 0, 0}, vp[4] = {0, 0, 0, 0}, sc = 0, dtest = 0, fbo = 0, prog_cur = 0;
            glGetIntegerv(GL_SCISSOR_BOX, sb);
            glGetIntegerv(GL_VIEWPORT, vp);
            glGetIntegerv(GL_SCISSOR_TEST, &sc);
            glGetIntegerv(GL_DEPTH_TEST, &dtest);
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
            glGetIntegerv(GL_CURRENT_PROGRAM, &prog_cur);
            printf("[GLTRACE]   gl: fbo %d (want %u) prog %d (want %u) scissor[%d %d %d %d] en %d viewport[%d %d %d %d] depthtest %d\n",
                   fbo, g.fbo, prog_cur, e->pipe->prog, sb[0], sb[1], sb[2], sb[3], sc, vp[0], vp[1], vp[2], vp[3], dtest);
        }
        for (const Attr& a : e->pipe->attrs) {
            if (a.buffer >= 32) continue;
            size_t avail = 0;
            const uint8_t* src = e->vbuf[a.buffer].data(avail);
            if (!src || avail < a.offset + 48) continue;
            const float* fv = reinterpret_cast<const float*>(src + a.offset);
            size_t stride = static_cast<size_t>(e->pipe->layouts[a.buffer].stride / 4);
            if (!stride) stride = 4;
            printf("[GLTRACE]   attr loc %d fmt %llu off %llu buf %llu stride %llu: %g %g %g | %g %g %g\n", a.loc,
                   static_cast<unsigned long long>(a.format), static_cast<unsigned long long>(a.offset),
                   static_cast<unsigned long long>(a.buffer), static_cast<unsigned long long>(e->pipe->layouts[a.buffer].stride), fv[0],
                   fv[1], fv[2], fv[stride], fv[stride + 1], fv[stride + 2]);
        }
        for (size_t bi = 0; bi < e->pipe->vs.blocks.size() && bi < 3; ++bi) {
            const auto& bl = e->pipe->vs.blocks[bi];
            size_t avail = 0;
            const uint8_t* src = bl.buffer >= 0 && bl.buffer < 32 ? e->vbuf[bl.buffer].data(avail) : nullptr;
            const float* fv = reinterpret_cast<const float*>(src);
            printf("[GLTRACE]   vs block %s <- buf %d avail %zu first %g %g %g %g\n", bl.block.c_str(), bl.buffer, avail,
                   src ? fv[0] : 0.0f, src ? fv[1] : 0.0f, src ? fv[2] : 0.0f, src ? fv[3] : 0.0f);
        }
        trace_probe();
        static std::map<GLuint, bool> dumped;
        const char* dir = getenv("LOADER_FRAME_DIR");
        if (dir && dumped.emplace(e->pipe->prog, true).second) {
            std::string path = std::string(dir) + "/prog_" + std::to_string(e->pipe->prog) + ".glsl";
            if (FILE* fp = fopen(path.c_str(), "wb")) {
                fprintf(fp, "%s\n// ---- fragment ----\n%s", e->pipe->vs.glsl.c_str(), e->pipe->fs.glsl.c_str());
                fclose(fp);
            }
        }
        static std::map<GLuint, bool> dumped_tex;
        Tex* first = texture(fake_lookup(e->ftex[0]));
        const char* tex_dir = g_trace_dump_textures && first && dumped_tex.emplace(first->id, true).second ? getenv("LOADER_FRAME_DIR") : nullptr;
        if (const char* dir = tex_dir) {   // the first texture of each traced draw, once per texture
            Tex* t = first;
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

// Debug band detector: two RGBA columns per frame (left then right, `rows` each). Logs runs of rows whose colour jumps in
// one frame and jumps back in the next, in both columns at once: a strip of the image that was wrong for a single frame.
struct BandDetector {
    const char* name;
    std::vector<int> prev, prev2;
    void feed(const std::vector<uint8_t>& cols, int rows, uint64_t n) {
        if (rows <= 0 || cols.size() < static_cast<size_t>(rows) * 8) { prev.clear(); prev2.clear(); return; }
        std::vector<int> cur(static_cast<size_t>(rows) * 2);
        for (size_t i = 0; i < cur.size(); ++i) cur[i] = cols[i * 4] + cols[i * 4 + 1] + cols[i * 4 + 2];
        if (prev2.size() == cur.size() && prev.size() == cur.size()) {
            auto odd = [&](int r) {
                for (int c = 0; c < 2; ++c) {
                    const size_t i = static_cast<size_t>(c) * rows + r;
                    if (std::abs(prev[i] - prev2[i]) < 60 || std::abs(prev[i] - cur[i]) < 60) return false;
                }
                return true;
            };
            for (int r = 0; r < rows;) {
                if (!odd(r)) { ++r; continue; }
                int e = r;
                while (e < rows && odd(e)) ++e;
                if (e - r >= 6)
                    printf("[BAND] %s f%llu rows %d-%d of %d (left %d->%d, right %d->%d)\n", name, static_cast<unsigned long long>(n - 1), r, e - 1, rows,
                           prev2[r], prev[r], prev2[rows + r], prev[rows + r]);
                r = e;
            }
        }
        prev2.swap(prev);
        prev.swap(cur);
    }
};
inline bool read_columns(Tex* t, int x, std::vector<uint8_t>& out) {
    ThreadGL& g = tgl();
    if (!g.init || !t || t->f.compressed || x >= t->w) return false;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g.read_fbo);
    attach(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, t, 0, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    out.resize(static_cast<size_t>(t->h) * 8);
    glReadPixels(x, 0, 1, t->h, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
    glReadPixels(t->w - 1 - x, 0, 1, t->h, GL_RGBA, GL_UNSIGNED_BYTE, out.data() + static_cast<size_t>(t->h) * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    return true;
}

// Command buffer commit: flush, then save presented drawables (LOADER_FRAME_DIR, every LOADER_FRAME_EVERY).
// [CAMetalDrawable present] is the pre-iOS-13 presenting call: it arrives *after* the command buffer is committed,
// so there is no #present property to hang the frame on. The drawable is queued here and saved on the next commit,
// by which time its GL work has been flushed.
inline void dump_presented(uint64_t drawable) {
    static const char* dir = getenv("LOADER_FRAME_DIR");
    static const uint64_t every = getenv("LOADER_FRAME_EVERY") ? strtoull(getenv("LOADER_FRAME_EVERY"), nullptr, 10) : 60;
    static uint64_t seen = 0;
    const uint64_t n = ++seen;
    g_frame_no = n;   // so LOADER_GL_TRACE_AT / LOADER_TEX_PROFILE see the same frame numbers on this presenting path
    if (!dir || !every || n % every) return;
    Tex* t = texture(fake_lookup(prop(fake_lookup(drawable), "texture", 0)));
    // The presenter is not always the buffer the last pass rendered into (Unity presents the previous frame's
    // drawable), so fall back to whatever full-size texture the render passes actually wrote.
    if (Tex* lt = texture(fake_lookup(g_last_big_target_obj.load())))
        if (lt != t && lt->w > 100 && lt->h > 100) t = lt;
    std::vector<uint8_t> px;
    if (!read_rgba(t, px)) {
        static std::atomic<int> missed{0};
        if (missed++ < 4)
            fprintf(stderr, "[MTLGL] dump frame %llu: no pixels (tex %p id %u, %dx%d)\n", static_cast<unsigned long long>(n),
                    static_cast<void*>(t), t ? t->id : 0u, t ? t->w : 0, t ? t->h : 0);
        return;
    }
    fprintf(stderr, "[MTLGL] dump: drawable tex id %u %dx%d rendered %llu times, %llu draws since last dump, thread %zu\n", t->id, t->w,
            t->h, static_cast<unsigned long long>(t->renders), static_cast<unsigned long long>(g_draws_frame.exchange(0)),
            std::hash<std::thread::id>{}(std::this_thread::get_id()));
    fprintf(stderr, "[MTLGL] dump: last full-size draw target was tex %llu, %llu guest msgs since last dump\n",
            static_cast<unsigned long long>(g_last_big_target.load()),
            static_cast<unsigned long long>(g_guest_msgs.exchange(0)));
    if (getenv("LOADER_SELECTOR_COUNTS")) {
        std::vector<std::pair<uint64_t, std::string>> top;
        {
            std::lock_guard<std::mutex> l(g_sel_mu);
            for (const auto& kv : g_sel_counts) top.emplace_back(kv.second, kv.first);
            g_sel_counts.clear();
        }
        std::sort(top.rbegin(), top.rend());
        for (size_t i = 0; i < top.size() && i < 14; ++i)
            fprintf(stderr, "[SEL] %8llu  %s\n", static_cast<unsigned long long>(top[i].first), top[i].second.c_str());
    }
    fprintf(stderr, "[MTLGL] census: passes depth %llu nodepth %llu | draws depth %llu nodepth %llu\n",
            static_cast<unsigned long long>(g_passes_depth.exchange(0)),
            static_cast<unsigned long long>(g_passes_nodepth.exchange(0)),
            static_cast<unsigned long long>(g_draws_depth.exchange(0)),
            static_cast<unsigned long long>(g_draws_nodepth.exchange(0)));
    char path[512];
    snprintf(path, sizeof path, "%s/frame_%05llu.png", dir, static_cast<unsigned long long>(n));
    for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;   // the layer is opaque
    bool ok = gles::write_png(path, t->w, t->h, px.data());
    printf("[MTLGL] frame %llu -> %s (%dx%d)%s\n", static_cast<unsigned long long>(n), path, t->w, t->h, ok ? "" : " WRITE FAILED");
    fflush(stdout);
}

inline void commit(FakeObjcObj* cb) {
    if (tgl().init) {
        if (uint64_t d = g_presented_drawable.exchange(0)) dump_presented(d);
    }
    if (!tgl().init) {
        // No GL context on this thread: nothing can be drawn or presented, so say so once per thread instead of
        // silently dropping every frame.
        static thread_local bool warned = false;
        if (!warned) {
            warned = true;
            fprintf(stderr, "[MTLGL] commit without a GL context on tid %d (gles_enabled=%d)\n", gettid(), gles_enabled() ? 1 : 0);
        }
        return;
    }
    if (tex_profiling()) tex_profile_check_reads(g_frame_no);
    uint64_t drawable = prop(cb, "#present", 0);
    if (!drawable) {
        // Legacy `[CAMetalDrawable present]` path: no presentDrawable: was issued, so the frame counter and the [MEM]
        // block further down are never reached, and a title on this path would report no memory telemetry at all.
        glFlush();
        static const bool mem_stats_legacy = getenv("LOADER_MEM_STATS") != nullptr;
        static uint64_t legacy_frames = 0;
        if (mem_stats_legacy && ++legacy_frames % 600 == 0) {
            size_t live;
            {
                std::lock_guard<std::mutex> l(g_fake_mu);
                live = g_fake_objc.size();
            }
            printf("[MEM] frame %llu (legacy present): %zu live fakes, %.1f MB GL textures\n",
                   static_cast<unsigned long long>(legacy_frames), live, g_texture_bytes.load() / 1048576.0);
            fflush(stdout);
        }
        return;
    }
    cb->props.erase("#present");
    uint64_t n = ++g_frame_no;
    ++g_present_count;
    fake_reap_graves();
    if (tex_profiling()) {
        tex_profile_frame(n);
        static const int max_traces = atoi(getenv("LOADER_TEX_PROFILE")) > 0 ? atoi(getenv("LOADER_TEX_PROFILE")) : 6;
        if (g_texprof.pending.exchange(false) && g_texprof.auto_traces < max_traces && getenv("LOADER_FRAME_DIR")) {
            ++g_texprof.auto_traces;
            g_trace_triggered = n;
            g_trace_dump_textures = false;
            g_probe_x = g_probe_y = -1;
            printf("[TEXPROF] anomaly in f%llu: tracing and dumping f%llu\n", static_cast<unsigned long long>(n - 1), static_cast<unsigned long long>(n));
        }
        static struct timespec last{};
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec != last.tv_sec) {
            {
                std::lock_guard<std::mutex> l(g_seq_mu);
                std::string tids;
                for (pid_t t : g_draw_tids) tids += " " + std::to_string(t);
                printf("[TEXPROF] draw threads:%s, present thread %d\n", tids.c_str(), gettid());
                g_draw_tids.clear();
            }
            printf("[TEXPROF] f%llu t=%lld: creates %u destroys %u uploads %u (%u KB) passes %u anomalies %u | short uniforms %u vertices %u, "
                   "dropped draws %u, bad passes %u, %.1f MB live\n",
                   static_cast<unsigned long long>(n), static_cast<long long>(now.tv_sec), g_texprof.creates.exchange(0), g_texprof.destroys.exchange(0),
                   g_texprof.uploads.exchange(0), g_texprof.upload_kb.exchange(0), g_texprof.renders.exchange(0), g_texprof.anomalies.exchange(0),
                   g_texprof.short_uniforms.exchange(0), g_texprof.short_vertices.exchange(0), g_texprof.dropped_draws.exchange(0), g_texprof.bad_passes.exchange(0),
                   g_texture_bytes.load() / 1048576.0);
            fflush(stdout);
            last = now;
        }
    }
    static const bool mem_stats = getenv("LOADER_MEM_STATS") != nullptr;
    if (mem_stats && n % 600 == 0) {
        size_t live;
        {
            std::lock_guard<std::mutex> l(g_fake_mu);
            live = g_fake_objc.size();
        }
        printf("[MEM] frame %llu: %zu live fakes, %.1f MB GL textures\n", static_cast<unsigned long long>(n), live, g_texture_bytes.load() / 1048576.0);
        std::map<std::string, size_t> by;
        std::vector<std::pair<int64_t, std::string>> big;
        {
            std::lock_guard<std::mutex> l(g_fake_mu);
            for (auto& kv : g_fake_objc) {
                FakeObjcObj* o = kv.second;
                std::string k = std::to_string(o->kind) + ":" + (o->s.size() > 24 ? o->s.substr(0, 24) : o->s);
                if (fake_life_debug()) {
                    std::lock_guard<std::mutex> g(g_grave_mu);
                    auto b = g_fake_born.find(reinterpret_cast<uint64_t>(kv.first));
                    if (b != g_fake_born.end()) k += " <" + b->second;
                }
                ++by[k];
                if (o->gltype == kGlTexture)
                    if (Tex* t = reinterpret_cast<Tex*>(prop(o, "#gl", 0)))
                        big.push_back({texture_bytes(t), std::to_string(t->w) + "x" + std::to_string(t->h) + "x" + std::to_string(t->layers) +
                                                         " L" + std::to_string(t->levels) + " fmt" + std::to_string(prop(o, "pixelFormat", 0))});
            }
        }
        std::vector<std::pair<size_t, std::string>> top;
        for (auto& kv : by) top.push_back({kv.second, kv.first});
        std::sort(top.rbegin(), top.rend());
        for (size_t i = 0; i < top.size() && i < 8; ++i) printf("[MEM]   %6zu %s\n", top[i].first, top[i].second.c_str());
        std::sort(big.rbegin(), big.rend());
        for (size_t i = 0; i < big.size() && i < 6; ++i) printf("[MEM]   tex %.1f MB %s\n", big[i].first / 1048576.0, big[i].second.c_str());
        fflush(stdout);
    }
    const bool traced = n - 1 == g_trace_triggered;
    static const char* trigger = getenv("LOADER_GL_TRACE_TRIGGER");
    if (trigger && n % 5 == 0 && access(trigger, F_OK) == 0) {
        if (FILE* f = fopen(trigger, "r")) {
            int x, y;
            if (fscanf(f, "%d,%d", &x, &y) == 2) { g_probe_x = x; g_probe_y = y; }
            fclose(f);
        }
        unlink(trigger);
        g_trace_triggered = n;
        g_trace_dump_textures = true;
        printf("[GLTRACE] triggered: frame %llu\n", static_cast<unsigned long long>(n));
    }
    static const char* dir = getenv("LOADER_FRAME_DIR");
    static const uint64_t every = getenv("LOADER_FRAME_EVERY") ? strtoull(getenv("LOADER_FRAME_EVERY"), nullptr, 10) : 60;
    if (dir && (traced || (every && n % every == 0))) {
        Tex* t = texture(fake_lookup(prop(fake_lookup(drawable), "texture", 0)));
        std::vector<uint8_t> px;
        if (!read_rgba(t, px)) {
            static std::atomic<int> missed{0};
            if (missed++ < 4)
                fprintf(stderr, "[MTLGL] dump frame %llu: no pixels (tex %p id %u, %dx%d)\n",
                        static_cast<unsigned long long>(n), static_cast<void*>(t), t ? t->id : 0u, t ? t->w : 0, t ? t->h : 0);
        }
        if (read_rgba(t, px)) {
            char path[512];
            snprintf(path, sizeof path, "%s/frame_%05llu.png", dir, static_cast<unsigned long long>(n));
            for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;   // the layer is opaque
            bool ok = gles::write_png(path, t->w, t->h, px.data());
            printf("[MTLGL] frame %llu -> %s (%dx%d)%s\n", static_cast<unsigned long long>(n), path, t->w, t->h, ok ? "" : " WRITE FAILED");
            fflush(stdout);
        }
    }
    // LOADER_PRESENT_CTL=<file> (debug): re-read every 30 frames. 0 default, 1 glFinish before presenting, 2 also after the
    // swap, 3..8 sets the drawable pool size, 9 runs the band detectors on the drawable and the window.
    static const char* present_ctl = getenv("LOADER_PRESENT_CTL");
    static int present_mode = 0;
    if (present_ctl && n % 30 == 0) {
        if (FILE* f = fopen(present_ctl, "r")) {
            int v = 0;
            if (fscanf(f, "%d", &v) == 1 && v != present_mode) {
                present_mode = v;
                g_drawable_count = v >= 3 && v <= 8 ? v : 3;
                printf("[PRESENT] f%llu mode %d\n", static_cast<unsigned long long>(n), v);
                fflush(stdout);
            }
            fclose(f);
        }
    }
    if (present_mode == 1 || present_mode == 2) glFinish();
    static BandDetector drawable_bands{"drawable", {}, {}}, window_bands{"window", {}, {}};
    Tex* shown = texture(fake_lookup(prop(fake_lookup(drawable), "texture", 0)));
    gles::set_window_probe(present_mode == 9 && shown ? 20 * 1440 / shown->w : -1);
    if (present_mode == 9 && shown) {
        static std::vector<uint8_t> cols;
        if (read_columns(shown, 20, cols)) drawable_bands.feed(cols, shown->h, n);
    }
    if (shown) gles::present_texture(shown->id, shown->w, shown->h);
    if (present_mode == 9) {
        const std::vector<uint8_t>& wc = gles::window_columns();
        window_bands.feed(wc, static_cast<int>(wc.size() / 8), n);
        fflush(stdout);
    }
    if (present_mode == 2) glFinish();
    glFlush();
}

}  // namespace mtlgl

// Frees the GL object of a dead Metal object (called from fake_reap_graves on the presenting thread).
static void mtl_gles_destroy(FakeObjcObj* o) {
    using namespace mtlgl;
    auto it = o->props.find("#gl");
    if (it == o->props.end() || !it->second || !o->gltype) return;
    void* p = reinterpret_cast<void*>(it->second);
    const bool gl = tgl().init;
    switch (o->gltype) {
        case kGlTexture: {
            auto* t = static_cast<Tex*>(p);
            g_texture_bytes -= texture_bytes(t);
            if (tex_profiling()) {
                ++g_texprof.destroys;
                const uint64_t age = g_frame_no - t->born;
                if (age < 120)
                    tex_anomaly(kChurn, t->id, "pf %llu %dx%d destroyed after %llu frames (%u uploads, %u renders)", static_cast<unsigned long long>(t->pf),
                                t->w, t->h, static_cast<unsigned long long>(age), t->uploads.load(), t->renders.load());
            }
            if (gl) glDeleteTextures(1, &t->id);
            delete t;
            break;
        }
        case kGlSampler: {
            auto* s = static_cast<Sampler*>(p);
            if (gl) glDeleteSamplers(1, &s->id);
            delete s;
            break;
        }
        case kGlPipeline: {
            auto* pl = static_cast<Pipeline*>(p);
            if (gl && pl->prog) glDeleteProgram(pl->prog);
            delete pl;
            break;
        }
        case kGlDepthStencil: delete static_cast<DepthStencil*>(p); break;
        case kGlEncoder: delete static_cast<Encoder*>(p); break;
        default: break;
    }
    o->props.erase(it);
}

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
        fake_retain_owned_props(t);
        frame.ret_x0 = h;
        return true;
    }
    if (sel == "newTextureViewWithPixelFormat:" || sel.compare(0, 31, "newTextureViewWithPixelFormat:t") == 0) {
        uint64_t h;
        FakeObjcObj* t = fake_metal_bag(&h);
        t->props = self->props;
        t->props.erase("#gl");
        fake_retain_owned_props(t);
        t->props["pixelFormat"] = frame.x[2];
        fake_set_prop(t, "#viewOf", frame.x[0]);
        frame.ret_x0 = h;
        return true;
    }
    if (sel == "newSamplerStateWithDescriptor:") {
        uint64_t h;
        FakeObjcObj* s = fake_metal_bag(&h);
        std::lock_guard<std::mutex> l(g_gl_mu);
        s->props["#gl"] = reinterpret_cast<uint64_t>(make_sampler(fake_lookup(frame.x[2])));
        s->gltype = kGlSampler;
        frame.ret_x0 = h;
        return true;
    }
    if (sel == "newDepthStencilStateWithDescriptor:") {
        uint64_t h;
        FakeObjcObj* s = fake_metal_bag(&h);
        s->props["#gl"] = reinterpret_cast<uint64_t>(make_depth_stencil(fake_lookup(frame.x[2])));
        s->gltype = kGlDepthStencil;
        frame.ret_x0 = h;
        return true;
    }
    if (sel == "newLibraryWithSource:options:error:") {
        uint64_t h;
        FakeObjcObj* lib = fake_metal_bag(&h);
        objc_string_value(frame.x[2], lib->s);
        fake_set_prop(lib, "#options", frame.x[3]);
        if (frame.x[4]) *reinterpret_cast<uint64_t*>(frame.x[4]) = 0;
        frame.ret_x0 = h;
        return true;
    }
    if (sel == "newFunctionWithName:" || sel == "newFunctionWithName:constantValues:error:") {
        uint64_t h;
        FakeObjcObj* fn = fake_metal_bag(&h);
        objc_string_value(frame.x[2], fn->s);
        fake_set_prop(fn, "#library", frame.x[0]);
        fake_set_prop(fn, "name", frame.x[2]);
        frame.ret_x0 = h;
        return true;
    }
    if (sel.compare(0, 37, "newRenderPipelineStateWithDescriptor:") == 0) {
        uint64_t h;
        FakeObjcObj* ps = fake_metal_bag(&h);
        {
            std::lock_guard<std::mutex> l(g_gl_mu);
            ps->props["#gl"] = reinterpret_cast<uint64_t>(make_pipeline(fake_lookup(frame.x[2])));
            ps->gltype = kGlPipeline;
        }
        size_t colons = colon_count(sel);
        if (sel.size() > 18 && sel.compare(sel.size() - 18, 18, "completionHandler:") == 0) {
            // ...:completionHandler: ^(state, error) / ...:options:completionHandler: ^(state, reflection, error)
            const uint64_t block = frame.x[1 + colons];
            if (void* inv = block ? *reinterpret_cast<void**>(block + 16) : nullptr)
                reinterpret_cast<void (*)(uint64_t, uint64_t, uint64_t, uint64_t)>(inv)(block, h, 0, 0);
            frame.ret_x0 = 0;
            return true;
        }
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
            if (t && tex_profiling()) {
                ++g_texprof.uploads;
                g_texprof.upload_kb += static_cast<uint32_t>(r[3] * r[4] * t->f.bytes / std::max(1, t->f.bw * t->f.bh) >> 10);
                if (t->uploads++ && g_frame_no != t->born)
                    tex_anomaly(kReupload, t->id, "pf %llu %dx%d region %llu,%llu %llux%llu level %d", static_cast<unsigned long long>(t->pf), t->w, t->h,
                                static_cast<unsigned long long>(r[0]), static_cast<unsigned long long>(r[1]), static_cast<unsigned long long>(r[3]),
                                static_cast<unsigned long long>(r[4]), level);
                if (!level && !t->f.compressed && !t->f.pvrtc && t->f.bytes == 4 && t->w * t->h <= 16 && bytes) {
                    const size_t row = bpr ? bpr : static_cast<size_t>(r[3]) * 4;
                    uint32_t first;
                    std::memcpy(&first, bytes, 4);
                    bool same = true;
                    for (uint64_t y = 0; same && y < r[4]; ++y)
                        for (uint64_t x = 0; same && x < r[3]; ++x) same = std::memcmp(bytes + y * row + x * 4, &first, 4) == 0;
                    t->placeholder = same;
                    t->solid = first;
                    if (same) printf("[TEXPROF] f%llu placeholder tex %u pf %llu %dx%d colour %08x\n", static_cast<unsigned long long>(g_frame_no), t->id,
                                     static_cast<unsigned long long>(t->pf), t->w, t->h, first);
                }
            } else if (t) {
                ++t->uploads;
            }
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
        const int level = static_cast<int>(frame.x[5]);
        // 8-bit formats only: the guest's rows hold bytes-per-pixel of the texture's own format, not always 4
        const size_t bpp = t && t->f.type == GL_UNSIGNED_BYTE && !t->f.integer ? static_cast<size_t>(t->f.bytes) : 0;
        if (t && r && dst && (bpp == 1 || bpp == 2 || bpp == 4) && read_rgba(t, px, level)) {
            const uint64_t lw = static_cast<uint64_t>(std::max(1, t->w >> level)), lh = static_cast<uint64_t>(std::max(1, t->h >> level));
            for (uint64_t y = 0; y < r[4] && r[1] + y < lh; ++y)
                for (uint64_t x = 0; x < r[3] && r[0] + x < lw; ++x) {
                    const uint8_t* s = &px[((r[1] + y) * lw + r[0] + x) * 4];
                    uint8_t* d = dst + y * frame.x[3] + x * bpp;
                    if (t->f.bgra) { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3]; } else std::memcpy(d, s, bpp);
                }
        }
        return true;
    }
    // command buffers / encoders
    if (sel == "renderCommandEncoderWithDescriptor:") {
        static std::atomic<int> enc_logged{0};
        if (enc_logged++ < 3) {
            FakeObjcObj* p = fake_lookup(frame.x[2]);
            fprintf(stderr, "[MTLGL] encoder pass %#llx%s\n", static_cast<unsigned long long>(frame.x[2]),
                    p && p->props.count("colorAttachments") ? " (has attachments)" : " (no attachments stored)");
        }
        uint64_t h;
        FakeObjcObj* enc = fake_metal_bag(&h);
        auto* e = new Encoder();
        enc->props["#gl"] = reinterpret_cast<uint64_t>(e);
        enc->gltype = kGlEncoder;
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
        if (sel == "setRenderPipelineState:") {
            e->pipe = gl_obj<Pipeline>(fake_lookup(frame.x[2]));
            if (pass_observe()) pass_note_pipe(e, frame.x[2]);
            return true;
        }
        if (sel == "setDepthStencilState:") {
            e->ds = gl_obj<DepthStencil>(fake_lookup(frame.x[2]));
            if (pass_observe()) pass_note_ds(e, frame.x[2]);
            return true;
        }
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
            static std::atomic<int> vp_logged{0};
            if (v && vp_logged++ < 8)
                fprintf(stderr, "[MTLGL] setViewport: %g %g %g %g %g %g\n", v[0], v[1], v[2], v[3], v[4], v[5]);
            if (pass_observe() && v) pass_note_viewport(e, v);
            if (v && e->valid) {
                for (int i = 0; i < 4; ++i) e->viewport[i] = static_cast<int>(v[i]);
                glViewport(static_cast<GLint>(v[0]), static_cast<GLint>(v[1]), static_cast<GLsizei>(v[2]), static_cast<GLsizei>(v[3]));
                glDepthRangef(static_cast<float>(v[4]), static_cast<float>(v[5]));
            }
            return true;
        }
        if (sel == "setScissorRect:") {
            const uint64_t* r = reinterpret_cast<const uint64_t*>(frame.x[2]);   // MTLScissorRect by reference
            if (pass_observe() && r) pass_note_scissor(e, r);
            if (r && e->valid) for (int i = 0; i < 4; ++i) e->scissor[i] = static_cast<int>(r[i]);
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
                if (pass_observe()) pass_note_bind(e->pass, vtx, static_cast<int>(frame.x[4]), frame.x[2], frame.x[3]);
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
                if (pass_observe()) pass_note_bytes(e->pass, vtx, static_cast<int>(frame.x[4]), frame.x[3]);
                return true;
            }
            if (rest == "Texture:atIndex:" && frame.x[3] < 32) {
                (vtx ? e->vtex : e->ftex)[frame.x[3]] = frame.x[2];
                if (pass_observe()) pass_note_tex(e->pass, vtx, static_cast<int>(frame.x[3]), frame.x[2]);
                return true;
            }
            // array forms: (ids, [offsets,] NSRange{location, length}) with the range in two registers
            if (rest == "Textures:withRange:" || rest == "SamplerStates:withRange:") {
                const uint64_t* ids = reinterpret_cast<const uint64_t*>(frame.x[2]);
                bool tex = rest[0] == 'T';
                uint64_t* slots = tex ? (vtx ? e->vtex : e->ftex) : (vtx ? e->vsamp : e->fsamp);
                uint64_t limit = tex ? 32 : 16;
                for (uint64_t i = 0; ids && i < frame.x[4] && frame.x[3] + i < limit; ++i) {
                    slots[frame.x[3] + i] = ids[i];
                    if (tex && pass_observe()) pass_note_tex(e->pass, vtx, static_cast<int>(frame.x[3] + i), ids[i]);
                }
                return true;
            }
            if (rest == "Buffers:offsets:withRange:") {
                const uint64_t* ids = reinterpret_cast<const uint64_t*>(frame.x[2]);
                const uint64_t* offs = reinterpret_cast<const uint64_t*>(frame.x[3]);
                for (uint64_t i = 0; ids && i < frame.x[5] && frame.x[4] + i < 32; ++i) {
                    BoundBuf& b = bufs[frame.x[4] + i];
                    b.buf = fake_lookup(ids[i]);
                    b.offset = offs ? offs[i] : 0;
                    if (pass_observe()) pass_note_bind(e->pass, vtx, static_cast<int>(frame.x[4] + i), ids[i], b.offset);
                    b.bytes.clear();
                }
                return true;
            }
            if (rest == "SamplerState:atIndex:" && frame.x[3] < 16) { (vtx ? e->vsamp : e->fsamp)[frame.x[3]] = frame.x[2]; return true; }
            if (rest == "SamplerState:lodMinClamp:lodMaxClamp:atIndex:" && frame.x[3] < 16) { (vtx ? e->vsamp : e->fsamp)[frame.x[3]] = frame.x[2]; return true; }
        }
        if (sel == "drawPrimitives:vertexStart:vertexCount:") {
            if (pass_observe()) pass_note_draw(e, pass_draw_text("drawPrimitives", frame.x[2], frame.x[3], frame.x[4], 1));
            draw(e, frame.x[2], frame.x[3], frame.x[4], 1);
            return false;
        }
        if (sel == "drawPrimitives:vertexStart:vertexCount:instanceCount:") {
            if (pass_observe()) pass_note_draw(e, pass_draw_text("drawPrimitives", frame.x[2], frame.x[3], frame.x[4], frame.x[5]));
            draw(e, frame.x[2], frame.x[3], frame.x[4], frame.x[5]);
            return false;
        }
        if (sel == "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:") {
            if (pass_observe())
                pass_note_draw(e, pass_draw_indexed_text(frame.x[2], frame.x[3], frame.x[4], frame.x[5], frame.x[6], 1));
            draw_indexed(e, frame.x[2], frame.x[3], frame.x[4], fake_lookup(frame.x[5]), frame.x[6], 1);
            return false;
        }
        if (sel == "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:") {
            if (pass_observe())
                pass_note_draw(e, pass_draw_indexed_text(frame.x[2], frame.x[3], frame.x[4], frame.x[5], frame.x[6], frame.x[7]));
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
        if (tex_profiling()) {
            const Tex* t = texture(fake_lookup(frame.x[7]));
            const uint64_t* size = reinterpret_cast<const uint64_t*>(frame.x[6]);
            tex_anomaly(kSkippedBlit, t ? t->id : 0, "copyFromBuffer->texture %llux%llu into pf %llu %dx%d dropped",
                        static_cast<unsigned long long>(size ? size[0] : 0), static_cast<unsigned long long>(size ? size[1] : 0),
                        static_cast<unsigned long long>(t ? t->pf : 0), t ? t->w : 0, t ? t->h : 0);
        }
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
    if (tex_profiling() && (sel.compare(0, 8, "copyFrom") == 0 || sel.compare(0, 8, "generate") == 0 || sel.compare(0, 4, "fill") == 0)) {
        static std::mutex mu;
        static std::map<std::string, uint64_t> seen;
        std::lock_guard<std::mutex> l(mu);
        if (seen[sel]++ == 0) tex_anomaly(kUnimplemented, 0, "Metal selector %s falls through to the property bag", sel.c_str());
    }
    return false;
}
