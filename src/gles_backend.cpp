#include "gles_backend.hpp"
#include "msl_glsl.hpp"

#include <dirent.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <zlib.h>

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

namespace gles {
namespace {

std::mutex g_mu;
EGLDisplay g_display = EGL_NO_DISPLAY;
EGLConfig g_config = nullptr;
EGLContext g_root = EGL_NO_CONTEXT;
bool g_surfaceless = false;
bool g_failed = false;
std::string g_extensions;

thread_local EGLContext t_context = EGL_NO_CONTEXT;
thread_local EGLSurface t_surface = EGL_NO_SURFACE;

bool init_display_locked() {
    if (g_root != EGL_NO_CONTEXT) return true;
    if (g_failed) return false;
    g_failed = true;
    g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_display == EGL_NO_DISPLAY || !eglInitialize(g_display, nullptr, nullptr)) {
        fprintf(stderr, "[GLES] eglInitialize failed: 0x%x\n", eglGetError());
        return false;
    }
    const char* egl_ext = eglQueryString(g_display, EGL_EXTENSIONS);
    g_surfaceless = egl_ext && std::strstr(egl_ext, "EGL_KHR_surfaceless_context");
    const EGLint attrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLint n = 0;
    if (!eglChooseConfig(g_display, attrs, &g_config, 1, &n) || n == 0) {
        fprintf(stderr, "[GLES] no ES3 pbuffer config\n");
        return false;
    }
    const EGLint ctx_attrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE};
    g_root = eglCreateContext(g_display, g_config, EGL_NO_CONTEXT, ctx_attrs);
    if (g_root == EGL_NO_CONTEXT) {
        fprintf(stderr, "[GLES] eglCreateContext (ES 3.2) failed: 0x%x\n", eglGetError());
        return false;
    }
    g_failed = false;
    return true;
}

}  // namespace

bool ensure_context() {
    if (t_context != EGL_NO_CONTEXT) return true;
    EGLContext ctx;
    {
        std::lock_guard<std::mutex> l(g_mu);
        if (!init_display_locked()) return false;
        const EGLint ctx_attrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE};
        ctx = eglCreateContext(g_display, g_config, g_root, ctx_attrs);
    }
    if (ctx == EGL_NO_CONTEXT) {
        fprintf(stderr, "[GLES] shared context creation failed: 0x%x\n", eglGetError());
        return false;
    }
    EGLSurface surf = EGL_NO_SURFACE;
    if (!g_surfaceless) {
        const EGLint pb[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        surf = eglCreatePbufferSurface(g_display, g_config, pb);
    }
    if (!eglMakeCurrent(g_display, surf, surf, ctx)) {
        fprintf(stderr, "[GLES] eglMakeCurrent failed: 0x%x\n", eglGetError());
        eglDestroyContext(g_display, ctx);
        return false;
    }
    t_context = ctx;
    t_surface = surf;
    return true;
}

void publish() {
    if (t_context != EGL_NO_CONTEXT) glFinish();
}

bool has_extension(const char* name) {
    if (g_extensions.empty() && ensure_context()) {
        GLint n = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &n);
        std::string all;
        for (GLint i = 0; i < n; ++i) {
            all += reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
            all += ' ';
        }
        std::lock_guard<std::mutex> l(g_mu);
        g_extensions = all;
    }
    std::string key = std::string(name) + ' ';
    return g_extensions.find(key) != std::string::npos;
}

void probe() {
    if (!ensure_context()) return;
    printf("[GLES] %s | %s | %s | surfaceless=%d\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION),
           g_surfaceless ? 1 : 0);
    GLint v[6] = {};
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &v[0]);
    glGetIntegerv(GL_MAX_VERTEX_UNIFORM_BLOCKS, &v[1]);
    glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_BLOCKS, &v[2]);
    glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &v[3]);
    glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &v[4]);
    glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, &v[5]);
    printf("[GLES] maxTex %d, UBOs vs %d fs %d, UBO size %d, tex units %d, vs SSBOs %d\n", v[0], v[1], v[2], v[3], v[4], v[5]);
    for (const char* e : {"GL_KHR_texture_compression_astc_ldr", "GL_EXT_texture_format_BGRA8888", "GL_EXT_color_buffer_float",
                          "GL_EXT_color_buffer_half_float", "GL_EXT_texture_compression_s3tc", "GL_IMG_texture_compression_pvrtc",
                          "GL_EXT_shader_framebuffer_fetch", "GL_EXT_clip_control", "GL_EXT_depth_clamp", "GL_OES_texture_stencil8"})
        printf("[GLES]   %-40s %s\n", e, has_extension(e) ? "yes" : "no");
    fflush(stdout);
}

bool write_png(const char* path, int w, int h, const uint8_t* rgba) {
    std::vector<uint8_t> raw(static_cast<size_t>(h) * (static_cast<size_t>(w) * 4 + 1));
    for (int y = 0; y < h; ++y) {
        uint8_t* row = &raw[static_cast<size_t>(y) * (static_cast<size_t>(w) * 4 + 1)];
        row[0] = 0;   // filter: none
        std::memcpy(row + 1, rgba + static_cast<size_t>(y) * w * 4, static_cast<size_t>(w) * 4);
    }
    uLongf zlen = compressBound(static_cast<uLong>(raw.size()));
    std::vector<uint8_t> z(zlen);
    if (compress2(z.data(), &zlen, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK) return false;
    FILE* fp = fopen(path, "wb");
    if (!fp) return false;
    auto be32 = [](uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; };
    auto chunk = [&](const char* type, const uint8_t* data, uint32_t len) {
        uint8_t hdr[8];
        be32(hdr, len);
        std::memcpy(hdr + 4, type, 4);
        fwrite(hdr, 1, 8, fp);
        if (len) fwrite(data, 1, len, fp);
        uLong crc = crc32(0, reinterpret_cast<const Bytef*>(type), 4);
        if (len) crc = crc32(crc, data, len);
        uint8_t c[4];
        be32(c, static_cast<uint32_t>(crc));
        fwrite(c, 1, 4, fp);
    };
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    fwrite(sig, 1, 8, fp);
    uint8_t ihdr[13];
    be32(ihdr, static_cast<uint32_t>(w));
    be32(ihdr + 4, static_cast<uint32_t>(h));
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;   // 8-bit RGBA
    chunk("IHDR", ihdr, 13);
    chunk("IDAT", z.data(), static_cast<uint32_t>(zlen));
    chunk("IEND", nullptr, 0);
    fclose(fp);
    return true;
}

// Compiles one GLSL stage; returns the info log on failure ("" on success).
std::string compile_log(GLenum type, const std::string& src, GLuint* out_shader) {
    GLuint s = glCreateShader(type);
    const char* p = src.c_str();
    glShaderSource(s, 1, &p, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    std::string log;
    if (!ok) {
        GLint len = 0;
        glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
        log.resize(len > 1 ? static_cast<size_t>(len) : 1);
        glGetShaderInfoLog(s, len, nullptr, &log[0]);
        if (log.empty() || log[0] == 0) log = "compile failed (no log)";
        glDeleteShader(s);
        s = 0;
    }
    if (out_shader) *out_shader = s; else if (s) glDeleteShader(s);
    return log;
}

int msl_selftest(const char* dir) {
    if (!ensure_context()) return -1;
    probe();
    const bool clip = has_extension("GL_EXT_clip_control");
    DIR* d = opendir(dir);
    if (!d) { printf("[MSL] cannot open %s\n", dir); return -1; }
    std::vector<std::string> files;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() > 6 && n.compare(n.size() - 6, 6, ".metal") == 0) files.push_back(n);
    }
    closedir(d);
    std::sort(files.begin(), files.end());
    int fails = 0;
    for (const std::string& f : files) {
        std::string path = std::string(dir) + "/" + f, src;
        if (FILE* fp = fopen(path.c_str(), "rb")) {
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, fp)) > 0) src.append(buf, n);
            fclose(fp);
        }
        msl::Shader sh = msl::translate(src, {}, clip);
        std::string log = sh.ok ? compile_log(sh.vertex ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER, sh.glsl, nullptr) : "translate: " + sh.error;
        if (FILE* fp = fopen((path + ".glsl").c_str(), "wb")) { fwrite(sh.glsl.data(), 1, sh.glsl.size(), fp); fclose(fp); }
        printf("[MSL] %-12s %s %s  blocks %zu samplers %zu inputs %zu\n", f.c_str(), sh.vertex ? "vs" : "fs", log.empty() ? "OK" : "FAIL",
               sh.blocks.size(), sh.samplers.size(), sh.inputs.size());
        if (!log.empty()) { ++fails; printf("%s\n", log.c_str()); }
    }
    printf("[MSL] %zu shaders, %d failed\n", files.size(), fails);
    fflush(stdout);
    return fails;
}

}  // namespace gles
