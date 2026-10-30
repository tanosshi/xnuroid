// App mode OpenGL ES: EAGLContext on EGL, the guest's GL calls go straight to Android's GLES 1.x / 2.0 libraries.
//
// An iOS app renders into a framebuffer object whose color renderbuffer got its storage from the CAEAGLLayer
// (-renderbufferStorage:fromDrawable:) and shows it with -presentRenderbuffer:. Here that "drawable" framebuffer is
// mapped onto the EGL window surface (framebuffer 0): binding it binds 0, its renderbuffer reports the size of the
// guest screen inside the surface, viewport/scissor are offset into that area (the guest screen keeps its aspect
// ratio and is letterboxed if needed), and presenting swaps buffers. Everything else is passed through untouched.
// PVRTC textures, which Android GPUs other than PowerVR cannot sample, are decoded to RGBA8 on upload.
#include "app.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include <set>

#include "pvrtc.hpp"

extern "C" ANativeWindow* host_window() __attribute__((weak));

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned int GLbitfield;
typedef float GLfloat;
typedef unsigned char GLboolean;

namespace app {

void screen_content_rect(int& x, int& y, int& w, int& h, int& sw, int& sh);   // app_uikit.cpp

namespace {

constexpr GLenum GL_FRAMEBUFFER = 0x8D40, GL_RENDERBUFFER = 0x8D41, GL_COLOR_ATTACHMENT0 = 0x8CE0, GL_FRAMEBUFFER_COMPLETE = 0x8CD5;
constexpr GLenum GL_RENDERBUFFER_WIDTH = 0x8D42, GL_RENDERBUFFER_HEIGHT = 0x8D43, GL_RENDERBUFFER_INTERNAL_FORMAT = 0x8D44;
constexpr GLenum GL_FRAMEBUFFER_BINDING = 0x8CA6, GL_RENDERBUFFER_BINDING = 0x8CA7, GL_VIEWPORT = 0x0BA2, GL_SCISSOR_BOX = 0x0C10, GL_SCISSOR_TEST = 0x0C11;
constexpr GLenum GL_RGBA = 0x1908, GL_UNSIGNED_BYTE = 0x1401, GL_BGRA_EXT = 0x80E1, GL_RGBA8_OES = 0x8058;
constexpr GLenum GL_COLOR_BUFFER_BIT = 0x4000;

void* g_gles1 = nullptr;
void* g_gles2 = nullptr;
EGLDisplay g_dpy = EGL_NO_DISPLAY;
EGLConfig g_cfg = nullptr;
EGLSurface g_win = EGL_NO_SURFACE;
bool g_win_is_pbuffer = false;
int g_surf_w = 0, g_surf_h = 0;
std::mutex g_egl_mu;

struct CtxState {
    EGLContext egl = EGL_NO_CONTEXT;
    int api = 1;
    std::set<GLuint> drawable_rbs, drawable_fbos;
    GLuint bound_fbo = 0, bound_rb = 0;
    bool scissor = false;
    GLint scissor_box[4] = {0, 0, 0, 0};
    GLint viewport[4] = {0, 0, 0, 0};
};
thread_local CtxState* t_ctx = nullptr;
thread_local id t_current = nullptr;
thread_local EGLSurface t_pbuffer = EGL_NO_SURFACE;

template <class F> F gl_fn(const char* name) {
    void* p = nullptr;
    if (g_gles1) p = dlsym(g_gles1, name);
    if (!p && g_gles2) p = dlsym(g_gles2, name);
    if (!p) p = reinterpret_cast<void*>(eglGetProcAddress(name));
    return reinterpret_cast<F>(p);
}
#define GLF(ret, name, args)                                       \
    static ret(*real_##name) args = nullptr;                        \
    static void load_##name() { if (!real_##name) real_##name = gl_fn<ret(*) args>(#name); }

GLF(void, glBindFramebufferOES, (GLenum, GLuint))
GLF(void, glBindFramebuffer, (GLenum, GLuint))
GLF(void, glBindRenderbufferOES, (GLenum, GLuint))
GLF(void, glBindRenderbuffer, (GLenum, GLuint))
GLF(void, glFramebufferRenderbufferOES, (GLenum, GLenum, GLenum, GLuint))
GLF(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint))
GLF(void, glGetRenderbufferParameterivOES, (GLenum, GLenum, GLint*))
GLF(void, glGetRenderbufferParameteriv, (GLenum, GLenum, GLint*))
GLF(GLenum, glCheckFramebufferStatusOES, (GLenum))
GLF(GLenum, glCheckFramebufferStatus, (GLenum))
GLF(void, glDeleteFramebuffersOES, (GLsizei, const GLuint*))
GLF(void, glDeleteFramebuffers, (GLsizei, const GLuint*))
GLF(void, glDeleteRenderbuffersOES, (GLsizei, const GLuint*))
GLF(void, glDeleteRenderbuffers, (GLsizei, const GLuint*))
GLF(void, glRenderbufferStorageOES, (GLenum, GLenum, GLsizei, GLsizei))
GLF(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei))
GLF(void, glViewport, (GLint, GLint, GLsizei, GLsizei))
GLF(void, glScissor, (GLint, GLint, GLsizei, GLsizei))
GLF(void, glEnable, (GLenum))
GLF(void, glDisable, (GLenum))
GLF(void, glClear, (GLbitfield))
GLF(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))
GLF(void, glGetIntegerv, (GLenum, GLint*))
GLF(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*))
GLF(void, glTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*))
GLF(void, glCompressedTexImage2D, (GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void*))
GLF(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*))
GLF(GLboolean, glIsEnabled, (GLenum))
GLF(void, glFinish, ())
GLF(void, glDrawArrays, (GLenum, GLint, GLsizei))
GLF(void, glDrawElements, (GLenum, GLsizei, GLenum, const void*))
GLF(GLenum, glGetError, ())
static std::atomic<uint64_t> g_draw_calls{0}, g_gl_errors{0}, g_tex_uploads{0};
static void w_glDrawArrays(GLenum m, GLint f, GLsizei c) { g_draw_calls.fetch_add(1); real_glDrawArrays(m, f, c); }
static void w_glDrawElements(GLenum m, GLsizei c, GLenum t, const void* i) { g_draw_calls.fetch_add(1); real_glDrawElements(m, c, t, i); }

void load_all() {
    load_glBindFramebufferOES(); load_glBindFramebuffer(); load_glBindRenderbufferOES(); load_glBindRenderbuffer();
    load_glFramebufferRenderbufferOES(); load_glFramebufferRenderbuffer(); load_glGetRenderbufferParameterivOES(); load_glGetRenderbufferParameteriv();
    load_glCheckFramebufferStatusOES(); load_glCheckFramebufferStatus(); load_glDeleteFramebuffersOES(); load_glDeleteFramebuffers();
    load_glDeleteRenderbuffersOES(); load_glDeleteRenderbuffers(); load_glRenderbufferStorageOES(); load_glRenderbufferStorage();
    load_glViewport(); load_glScissor(); load_glEnable(); load_glDisable(); load_glClear(); load_glClearColor(); load_glGetIntegerv();
    load_glTexImage2D(); load_glTexSubImage2D(); load_glCompressedTexImage2D(); load_glReadPixels(); load_glIsEnabled(); load_glFinish();
    load_glDrawArrays(); load_glDrawElements(); load_glGetError();
}

bool content_rect(int& x, int& y, int& w, int& h) {
    int sw, sh;
    screen_content_rect(x, y, w, h, sw, sh);
    // the surface may differ from what UIKit assumed (CLI pbuffer, late window changes): rescale proportionally
    if (g_surf_w && g_surf_h && (sw != g_surf_w || sh != g_surf_h)) {
        double kx = static_cast<double>(g_surf_w) / sw, ky = static_cast<double>(g_surf_h) / sh;
        x = static_cast<int>(x * kx); w = static_cast<int>(w * kx);
        y = static_cast<int>(y * ky); h = static_cast<int>(h * ky);
    }
    // GL's origin is bottom-left: the guest's bottom edge maps to the surface's bottom edge plus the letterbox margin
    if (g_surf_h) y = g_surf_h - (y + h);
    return x != 0 || y != 0 || w != g_surf_w || h != g_surf_h;
}

bool egl_init() {
    std::lock_guard<std::mutex> l(g_egl_mu);
    if (g_dpy != EGL_NO_DISPLAY) return true;
    g_gles1 = dlopen("libGLESv1_CM.so", RTLD_NOW | RTLD_GLOBAL);
    g_gles2 = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    if (!g_gles1) log("dlopen libGLESv1_CM.so failed: %s", dlerror());
    g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_dpy == EGL_NO_DISPLAY || !eglInitialize(g_dpy, nullptr, nullptr)) { log("eglInitialize failed"); g_dpy = EGL_NO_DISPLAY; return false; }
    const EGLint attrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES_BIT | EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE};
    EGLint n = 0;
    if (!eglChooseConfig(g_dpy, attrs, &g_cfg, 1, &n) || n < 1) {
        const EGLint fallback[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
                                   EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 16, EGL_NONE};
        if (!eglChooseConfig(g_dpy, fallback, &g_cfg, 1, &n) || n < 1) { log("eglChooseConfig failed"); return false; }
    }
    load_all();
    log("EGL ready (GLES1 %s)", g_gles1 ? "loaded" : "missing");
    return true;
}

ANativeWindow* g_win_native = nullptr;   // the ANativeWindow g_win was created from (for background/foreground)

// the surface the guest's drawable maps to: the Android window, or an offscreen pbuffer in the command-line host
void ensure_window_surface() {
    if (g_win != EGL_NO_SURFACE) return;
    ANativeWindow* win = host_window ? host_window() : nullptr;
    g_win_native = win;
    if (win) {
        EGLint fmt = 0;
        eglGetConfigAttrib(g_dpy, g_cfg, EGL_NATIVE_VISUAL_ID, &fmt);
        ANativeWindow_setBuffersGeometry(win, 0, 0, fmt);
        g_win = eglCreateWindowSurface(g_dpy, g_cfg, win, nullptr);
        g_win_is_pbuffer = false;
    }
    if (g_win == EGL_NO_SURFACE) {
        int x, y, w, h, sw, sh;
        screen_content_rect(x, y, w, h, sw, sh);
        const EGLint pb[] = {EGL_WIDTH, sw, EGL_HEIGHT, sh, EGL_NONE};
        g_win = eglCreatePbufferSurface(g_dpy, g_cfg, pb);
        g_win_is_pbuffer = true;
    }
    eglQuerySurface(g_dpy, g_win, EGL_WIDTH, &g_surf_w);
    eglQuerySurface(g_dpy, g_win, EGL_HEIGHT, &g_surf_h);
    log("drawable surface %dx%d (%s)", g_surf_w, g_surf_h, g_win_is_pbuffer ? "pbuffer" : "window");
}

// Reconcile the EGL window surface with the host's current ANativeWindow. On background the host drops the window
// (host_window() -> null); on return it hands back a new one. Rebuild the surface and rebind the current context so
// rendering resumes. Returns false while there is no window (caller should skip the frame). Main thread only.
bool refresh_window_surface() {
    if (!on_main_thread()) return g_win != EGL_NO_SURFACE;
    ANativeWindow* cur = host_window ? host_window() : nullptr;
    if (g_win != EGL_NO_SURFACE && (cur == g_win_native || g_win_is_pbuffer)) return true;   // unchanged
    // window changed (or first appeared): drop the old surface
    EGLContext ctx = eglGetCurrentContext();
    if (g_win != EGL_NO_SURFACE) {
        eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (!g_win_is_pbuffer) eglDestroySurface(g_dpy, g_win);
        g_win = EGL_NO_SURFACE;
    }
    g_win_native = cur;
    if (!cur) return false;   // still backgrounded
    EGLint fmt = 0;
    eglGetConfigAttrib(g_dpy, g_cfg, EGL_NATIVE_VISUAL_ID, &fmt);
    ANativeWindow_setBuffersGeometry(cur, 0, 0, fmt);
    g_win = eglCreateWindowSurface(g_dpy, g_cfg, cur, nullptr);
    g_win_is_pbuffer = false;
    if (g_win == EGL_NO_SURFACE) { log("resume: eglCreateWindowSurface failed 0x%x", eglGetError()); return false; }
    eglQuerySurface(g_dpy, g_win, EGL_WIDTH, &g_surf_w);
    eglQuerySurface(g_dpy, g_win, EGL_HEIGHT, &g_surf_h);
    if (ctx != EGL_NO_CONTEXT && !eglMakeCurrent(g_dpy, g_win, g_win, ctx)) log("resume: eglMakeCurrent failed 0x%x", eglGetError());
    log("resume: new drawable surface %dx%d", g_surf_w, g_surf_h);
    return true;
}

EGLSurface thread_surface() {
    if (on_main_thread()) { ensure_window_surface(); return g_win; }
    if (t_pbuffer == EGL_NO_SURFACE) {
        const EGLint pb[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        t_pbuffer = eglCreatePbufferSurface(g_dpy, g_cfg, pb);
    }
    return t_pbuffer;
}

Class C_EAGLContext, C_EAGLSharegroup;
struct EaglExt : Ext {
    CtxState st;
    id sharegroup = nullptr;
    ~EaglExt() override {
        if (st.egl != EGL_NO_CONTEXT) {
            if (eglGetCurrentContext() == st.egl) { eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); t_ctx = nullptr; }
            eglDestroyContext(g_dpy, st.egl);
        }
        release(sharegroup);
    }
};
struct ShareExt : Ext { EGLContext root = EGL_NO_CONTEXT; };

id make_context(id self, int api, id sharegroup) {
    if (!egl_init()) { release(self); return nullptr; }
    auto* e = ext_get<EaglExt>(self);
    e->st.api = api;
    if (!sharegroup) sharegroup = autorelease(alloc(C_EAGLSharegroup));
    e->sharegroup = retain(sharegroup);
    auto* sg = ext_get<ShareExt>(sharegroup);
    const EGLint attrs[] = {EGL_CONTEXT_CLIENT_VERSION, api >= 3 ? 3 : api, EGL_NONE};
    e->st.egl = eglCreateContext(g_dpy, g_cfg, sg->root, attrs);
    if (e->st.egl == EGL_NO_CONTEXT) { log("eglCreateContext(ES%d) failed: 0x%x", api, eglGetError()); release(self); return nullptr; }
    if (sg->root == EGL_NO_CONTEXT) sg->root = e->st.egl;
    log("EAGLContext %p: OpenGL ES %d", static_cast<void*>(self), api);
    return self;
}

bool set_current(id ctx) {
    if (!egl_init()) return false;
    if (ctx == t_current) return true;
    if (!ctx) {
        eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        release(t_current);
        t_current = nullptr;
        t_ctx = nullptr;
        return true;
    }
    auto* e = ext_get<EaglExt>(ctx);
    EGLSurface s = thread_surface();
    if (!eglMakeCurrent(g_dpy, s, s, e->st.egl)) { log("eglMakeCurrent failed: 0x%x", eglGetError()); return false; }
    if (on_main_thread()) eglSwapInterval(g_dpy, 1);
    retain(ctx);
    release(t_current);
    t_current = ctx;
    t_ctx = &e->st;
    return true;
}

// ---- frame statistics / dumps ----
uint64_t g_frames = 0;
double g_fps_t0 = 0;
void dump_frame() {
    static const char* dir = getenv("LOADER_APP_FRAME_DIR");
    static int every = getenv("LOADER_APP_FRAME_EVERY") ? atoi(getenv("LOADER_APP_FRAME_EVERY")) : 60;
    if (!dir || every <= 0 || g_frames % every != 0) return;
    std::vector<uint8_t> px(static_cast<size_t>(g_surf_w) * g_surf_h * 4);
    real_glReadPixels(0, 0, g_surf_w, g_surf_h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    char path[512];
    snprintf(path, sizeof path, "%s/frame_%06llu.ppm", dir, static_cast<unsigned long long>(g_frames));
    mkdir(dir, 0755);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", g_surf_w, g_surf_h);
    for (int y = g_surf_h - 1; y >= 0; --y)
        for (int x = 0; x < g_surf_w; ++x) fwrite(&px[(static_cast<size_t>(y) * g_surf_w + x) * 4], 1, 3, f);
    fclose(f);
    log("frame %llu -> %s", static_cast<unsigned long long>(g_frames), path);
}

bool present() {
    if (!t_ctx) return false;
    if (!refresh_window_surface()) return false;   // backgrounded: skip the frame until the window returns
    if (g_win == EGL_NO_SURFACE) return false;
    ++g_frames;
    dump_frame();
    static const uint64_t max_frames = getenv("LOADER_APP_MAX_FRAMES") ? strtoull(getenv("LOADER_APP_MAX_FRAMES"), nullptr, 10) : 0;
    if (max_frames && g_frames >= max_frames) {
        app::log("reached LOADER_APP_MAX_FRAMES=%llu, exiting", static_cast<unsigned long long>(max_frames));
        eglSwapBuffers(g_dpy, g_win);
        fflush(stdout);
        fflush(stderr);
        _exit(0);
    }
    if (g_frames <= 5 || g_frames % 120 == 0) {
        GLenum e = real_glGetError ? real_glGetError() : 0;
        log("frame %llu: %llu draws, %llu tex uploads, glGetError=0x%x", static_cast<unsigned long long>(g_frames),
            static_cast<unsigned long long>(g_draw_calls.load()), static_cast<unsigned long long>(g_tex_uploads.load()), e);
    }
    bool ok = eglSwapBuffers(g_dpy, g_win);
    double t = now_s();
    if (!g_fps_t0) g_fps_t0 = t;
    static uint64_t last_frames = 0;
    if (t - g_fps_t0 >= 10) {
        log("%.1f fps (%llu frames)", (g_frames - last_frames) / (t - g_fps_t0), static_cast<unsigned long long>(g_frames));
        g_fps_t0 = t;
        last_frames = g_frames;
    }
    return ok;
}

bool drawable_bound() { return t_ctx && t_ctx->drawable_fbos.count(t_ctx->bound_fbo); }

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// GL wrappers
// ---------------------------------------------------------------------------------------------------------------
extern "C" {

static void w_bind_fbo_impl(GLenum target, GLuint fb, bool oes) {
    auto real = oes ? real_glBindFramebufferOES : real_glBindFramebuffer;
    if (!real) real = oes ? real_glBindFramebuffer : real_glBindFramebufferOES;
    if (t_ctx) t_ctx->bound_fbo = fb;
    GLuint actual = t_ctx && t_ctx->drawable_fbos.count(fb) ? 0 : fb;
    if (real) real(target, actual);
    // viewport/scissor are context state: re-apply the guest's values for the framebuffer now bound
    int cx, cy, cw, ch;
    if (t_ctx && content_rect(cx, cy, cw, ch)) {
        bool win = actual == 0 && fb != 0;
        const GLint* v = t_ctx->viewport;
        const GLint* s = t_ctx->scissor_box;
        if (v[2]) real_glViewport(v[0] + (win ? cx : 0), v[1] + (win ? cy : 0), v[2], v[3]);
        if (s[2]) real_glScissor(s[0] + (win ? cx : 0), s[1] + (win ? cy : 0), s[2], s[3]);
    }
}
static void w_glBindFramebufferOES(GLenum t, GLuint fb) { w_bind_fbo_impl(t, fb, true); }
static void w_glBindFramebuffer(GLenum t, GLuint fb) { w_bind_fbo_impl(t, fb, false); }
static void w_glBindRenderbufferOES(GLenum t, GLuint rb) { if (t_ctx) t_ctx->bound_rb = rb; if (real_glBindRenderbufferOES) real_glBindRenderbufferOES(t, rb); else if (real_glBindRenderbuffer) real_glBindRenderbuffer(t, rb); }
static void w_glBindRenderbuffer(GLenum t, GLuint rb) { if (t_ctx) t_ctx->bound_rb = rb; if (real_glBindRenderbuffer) real_glBindRenderbuffer(t, rb); else if (real_glBindRenderbufferOES) real_glBindRenderbufferOES(t, rb); }

static void w_fb_rb_impl(GLenum target, GLenum attach, GLenum rbt, GLuint rb, bool oes) {
    auto real = oes ? real_glFramebufferRenderbufferOES : real_glFramebufferRenderbuffer;
    if (!real) real = oes ? real_glFramebufferRenderbuffer : real_glFramebufferRenderbufferOES;
    if (t_ctx && attach == GL_COLOR_ATTACHMENT0 && t_ctx->drawable_rbs.count(rb)) {
        GLuint fb = t_ctx->bound_fbo;
        t_ctx->drawable_fbos.insert(fb);
        auto bind = real_glBindFramebufferOES ? real_glBindFramebufferOES : real_glBindFramebuffer;
        if (bind) bind(GL_FRAMEBUFFER, 0);
        if (trace_enabled("gl")) log("framebuffer %u is the drawable (window surface)", fb);
        return;
    }
    if (drawable_bound()) return;   // depth/stencil for the window come from the EGL config
    if (real) real(target, attach, rbt, rb);
}
static void w_glFramebufferRenderbufferOES(GLenum t, GLenum a, GLenum rt, GLuint rb) { w_fb_rb_impl(t, a, rt, rb, true); }
static void w_glFramebufferRenderbuffer(GLenum t, GLenum a, GLenum rt, GLuint rb) { w_fb_rb_impl(t, a, rt, rb, false); }

static void w_get_rb_param(GLenum target, GLenum pname, GLint* out, bool oes) {
    if (t_ctx && t_ctx->drawable_rbs.count(t_ctx->bound_rb)) {
        int x, y, w, h;
        content_rect(x, y, w, h);
        if (pname == GL_RENDERBUFFER_WIDTH) { *out = w; return; }
        if (pname == GL_RENDERBUFFER_HEIGHT) { *out = h; return; }
        if (pname == GL_RENDERBUFFER_INTERNAL_FORMAT) { *out = GL_RGBA8_OES; return; }
    }
    auto real = oes ? real_glGetRenderbufferParameterivOES : real_glGetRenderbufferParameteriv;
    if (!real) real = oes ? real_glGetRenderbufferParameteriv : real_glGetRenderbufferParameterivOES;
    if (real) real(target, pname, out);
}
static void w_glGetRenderbufferParameterivOES(GLenum t, GLenum p, GLint* o) { w_get_rb_param(t, p, o, true); }
static void w_glGetRenderbufferParameteriv(GLenum t, GLenum p, GLint* o) { w_get_rb_param(t, p, o, false); }
static GLenum w_glCheckFramebufferStatusOES(GLenum t) {
    if (drawable_bound()) return GL_FRAMEBUFFER_COMPLETE;
    return real_glCheckFramebufferStatusOES ? real_glCheckFramebufferStatusOES(t) : real_glCheckFramebufferStatus ? real_glCheckFramebufferStatus(t) : GL_FRAMEBUFFER_COMPLETE;
}
static GLenum w_glCheckFramebufferStatus(GLenum t) {
    if (drawable_bound()) return GL_FRAMEBUFFER_COMPLETE;
    return real_glCheckFramebufferStatus ? real_glCheckFramebufferStatus(t) : real_glCheckFramebufferStatusOES ? real_glCheckFramebufferStatusOES(t) : GL_FRAMEBUFFER_COMPLETE;
}
static void w_del_fbos(GLsizei n, const GLuint* ids, bool oes) {
    if (t_ctx) for (GLsizei i = 0; i < n; ++i) { t_ctx->drawable_fbos.erase(ids[i]); if (t_ctx->bound_fbo == ids[i]) t_ctx->bound_fbo = 0; }
    auto real = oes ? real_glDeleteFramebuffersOES : real_glDeleteFramebuffers;
    if (!real) real = oes ? real_glDeleteFramebuffers : real_glDeleteFramebuffersOES;
    if (real) real(n, ids);
}
static void w_glDeleteFramebuffersOES(GLsizei n, const GLuint* ids) { w_del_fbos(n, ids, true); }
static void w_glDeleteFramebuffers(GLsizei n, const GLuint* ids) { w_del_fbos(n, ids, false); }
static void w_del_rbs(GLsizei n, const GLuint* ids, bool oes) {
    if (t_ctx) for (GLsizei i = 0; i < n; ++i) t_ctx->drawable_rbs.erase(ids[i]);
    auto real = oes ? real_glDeleteRenderbuffersOES : real_glDeleteRenderbuffers;
    if (!real) real = oes ? real_glDeleteRenderbuffers : real_glDeleteRenderbuffersOES;
    if (real) real(n, ids);
}
static void w_glDeleteRenderbuffersOES(GLsizei n, const GLuint* ids) { w_del_rbs(n, ids, true); }
static void w_glDeleteRenderbuffers(GLsizei n, const GLuint* ids) { w_del_rbs(n, ids, false); }
static void w_glRenderbufferStorageOES(GLenum t, GLenum f, GLsizei w, GLsizei h) {
    if (t_ctx && t_ctx->drawable_rbs.count(t_ctx->bound_rb)) return;
    if (real_glRenderbufferStorageOES) real_glRenderbufferStorageOES(t, f, w, h); else if (real_glRenderbufferStorage) real_glRenderbufferStorage(t, f, w, h);
}
static void w_glRenderbufferStorage(GLenum t, GLenum f, GLsizei w, GLsizei h) {
    if (t_ctx && t_ctx->drawable_rbs.count(t_ctx->bound_rb)) return;
    if (real_glRenderbufferStorage) real_glRenderbufferStorage(t, f, w, h); else if (real_glRenderbufferStorageOES) real_glRenderbufferStorageOES(t, f, w, h);
}

static void w_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    if (t_ctx) { t_ctx->viewport[0] = x; t_ctx->viewport[1] = y; t_ctx->viewport[2] = w; t_ctx->viewport[3] = h; }
    if (drawable_bound()) {
        int cx, cy, cw, ch;
        content_rect(cx, cy, cw, ch);
        real_glViewport(x + cx, y + cy, w, h);
        return;
    }
    real_glViewport(x, y, w, h);
}
static void w_glScissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    if (t_ctx) { t_ctx->scissor_box[0] = x; t_ctx->scissor_box[1] = y; t_ctx->scissor_box[2] = w; t_ctx->scissor_box[3] = h; }
    if (drawable_bound()) {
        int cx, cy, cw, ch;
        content_rect(cx, cy, cw, ch);
        real_glScissor(x + cx, y + cy, w, h);
        return;
    }
    real_glScissor(x, y, w, h);
}
static void w_glEnable(GLenum cap) { if (cap == GL_SCISSOR_TEST && t_ctx) t_ctx->scissor = true; real_glEnable(cap); }
static void w_glDisable(GLenum cap) { if (cap == GL_SCISSOR_TEST && t_ctx) t_ctx->scissor = false; real_glDisable(cap); }
static void w_glClear(GLbitfield mask) {
    int cx, cy, cw, ch;
    if (drawable_bound() && !t_ctx->scissor && content_rect(cx, cy, cw, ch)) {
        // keep the letterbox bars black: clear them once, then restrict the guest's clear to its screen
        real_glDisable(GL_SCISSOR_TEST);
        real_glClear(mask);
        real_glEnable(GL_SCISSOR_TEST);
        real_glScissor(cx, cy, cw, ch);
        real_glClear(mask);
        real_glDisable(GL_SCISSOR_TEST);
        return;
    }
    real_glClear(mask);
}
static void w_glGetIntegerv(GLenum pname, GLint* out) {
    if (t_ctx) {
        if (pname == GL_FRAMEBUFFER_BINDING) { *out = static_cast<GLint>(t_ctx->bound_fbo); return; }
        if (pname == GL_RENDERBUFFER_BINDING) { *out = static_cast<GLint>(t_ctx->bound_rb); return; }
        if (pname == GL_VIEWPORT && drawable_bound()) { std::memcpy(out, t_ctx->viewport, sizeof t_ctx->viewport); return; }
        if (pname == GL_SCISSOR_BOX && drawable_bound()) { std::memcpy(out, t_ctx->scissor_box, sizeof t_ctx->scissor_box); return; }
    }
    real_glGetIntegerv(pname, out);
}
static void w_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, void* p) {
    if (drawable_bound()) { int cx, cy, cw, ch; content_rect(cx, cy, cw, ch); x += cx; y += cy; }
    real_glReadPixels(x, y, w, h, f, t, p);
}

static void swizzle_bgra(std::vector<uint8_t>& v, const void* src, size_t n) {
    v.assign(static_cast<const uint8_t*>(src), static_cast<const uint8_t*>(src) + n);
    for (size_t i = 0; i + 3 < n; i += 4) std::swap(v[i], v[i + 2]);
}
static void w_glTexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum fmt, GLenum type, const void* px) {
    g_tex_uploads.fetch_add(1);
    if (fmt == GL_BGRA_EXT && type == GL_UNSIGNED_BYTE) {
        std::vector<uint8_t> v;
        if (px) swizzle_bgra(v, px, static_cast<size_t>(w) * h * 4);
        real_glTexImage2D(target, level, GL_RGBA, w, h, border, GL_RGBA, type, px ? v.data() : nullptr);
        return;
    }
    if (ifmt == static_cast<GLint>(GL_BGRA_EXT)) ifmt = GL_RGBA;
    real_glTexImage2D(target, level, ifmt, w, h, border, fmt, type, px);
}
static void w_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, const void* px) {
    if (fmt == GL_BGRA_EXT && type == GL_UNSIGNED_BYTE && px) {
        std::vector<uint8_t> v;
        swizzle_bgra(v, px, static_cast<size_t>(w) * h * 4);
        real_glTexSubImage2D(target, level, x, y, w, h, GL_RGBA, type, v.data());
        return;
    }
    real_glTexSubImage2D(target, level, x, y, w, h, fmt, type, px);
}
static void w_glCompressedTexImage2D(GLenum target, GLint level, GLenum ifmt, GLsizei w, GLsizei h, GLint border, GLsizei size, const void* data) {
    g_tex_uploads.fetch_add(1);
    if (ifmt >= 0x8C00 && ifmt <= 0x8C03) {   // PVRTC1 (IMG_texture_compression_pvrtc)
        bool two = ifmt == 0x8C01 || ifmt == 0x8C03;
        std::vector<uint8_t> rgba;
        if (pvrtc::decode(static_cast<const uint8_t*>(data), static_cast<size_t>(size), w, h, two, rgba)) {
            real_glTexImage2D(target, level, GL_RGBA, w, h, border, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            return;
        }
        log("PVRTC decode failed (%dx%d, %d bytes)", w, h, size);
    }
    real_glCompressedTexImage2D(target, level, ifmt, w, h, border, size, data);
}
static void w_noop() {}
static void w_glDiscardFramebufferEXT(GLenum, GLsizei, const GLenum*) {}

}  // extern "C"

void* gl_resolve(const char* name) {
    static const std::unordered_map<std::string, void*> wrappers = {
        {"glBindFramebufferOES", reinterpret_cast<void*>(&w_glBindFramebufferOES)},
        {"glBindFramebuffer", reinterpret_cast<void*>(&w_glBindFramebuffer)},
        {"glBindRenderbufferOES", reinterpret_cast<void*>(&w_glBindRenderbufferOES)},
        {"glBindRenderbuffer", reinterpret_cast<void*>(&w_glBindRenderbuffer)},
        {"glFramebufferRenderbufferOES", reinterpret_cast<void*>(&w_glFramebufferRenderbufferOES)},
        {"glFramebufferRenderbuffer", reinterpret_cast<void*>(&w_glFramebufferRenderbuffer)},
        {"glGetRenderbufferParameterivOES", reinterpret_cast<void*>(&w_glGetRenderbufferParameterivOES)},
        {"glGetRenderbufferParameteriv", reinterpret_cast<void*>(&w_glGetRenderbufferParameteriv)},
        {"glCheckFramebufferStatusOES", reinterpret_cast<void*>(&w_glCheckFramebufferStatusOES)},
        {"glCheckFramebufferStatus", reinterpret_cast<void*>(&w_glCheckFramebufferStatus)},
        {"glDeleteFramebuffersOES", reinterpret_cast<void*>(&w_glDeleteFramebuffersOES)},
        {"glDeleteFramebuffers", reinterpret_cast<void*>(&w_glDeleteFramebuffers)},
        {"glDeleteRenderbuffersOES", reinterpret_cast<void*>(&w_glDeleteRenderbuffersOES)},
        {"glDeleteRenderbuffers", reinterpret_cast<void*>(&w_glDeleteRenderbuffers)},
        {"glRenderbufferStorageOES", reinterpret_cast<void*>(&w_glRenderbufferStorageOES)},
        {"glRenderbufferStorage", reinterpret_cast<void*>(&w_glRenderbufferStorage)},
        {"glViewport", reinterpret_cast<void*>(&w_glViewport)},
        {"glScissor", reinterpret_cast<void*>(&w_glScissor)},
        {"glEnable", reinterpret_cast<void*>(&w_glEnable)},
        {"glDisable", reinterpret_cast<void*>(&w_glDisable)},
        {"glClear", reinterpret_cast<void*>(&w_glClear)},
        {"glGetIntegerv", reinterpret_cast<void*>(&w_glGetIntegerv)},
        {"glReadPixels", reinterpret_cast<void*>(&w_glReadPixels)},
        {"glDrawArrays", reinterpret_cast<void*>(&w_glDrawArrays)},
        {"glDrawElements", reinterpret_cast<void*>(&w_glDrawElements)},
        {"glTexImage2D", reinterpret_cast<void*>(&w_glTexImage2D)},
        {"glTexSubImage2D", reinterpret_cast<void*>(&w_glTexSubImage2D)},
        {"glCompressedTexImage2D", reinterpret_cast<void*>(&w_glCompressedTexImage2D)},
        {"glDiscardFramebufferEXT", reinterpret_cast<void*>(&w_glDiscardFramebufferEXT)},
        {"glInsertEventMarkerEXT", reinterpret_cast<void*>(&w_noop)},
        {"glPushGroupMarkerEXT", reinterpret_cast<void*>(&w_noop)},
        {"glPopGroupMarkerEXT", reinterpret_cast<void*>(&w_noop)},
        {"glLabelObjectEXT", reinterpret_cast<void*>(&w_noop)},
        {"glResolveMultisampleFramebufferAPPLE", reinterpret_cast<void*>(&w_noop)},
    };
    egl_init();
    auto it = wrappers.find(name);
    if (it != wrappers.end()) return it->second;
    void* p = nullptr;
    if (g_gles1) p = dlsym(g_gles1, name);
    if (!p && g_gles2) p = dlsym(g_gles2, name);
    if (!p) p = reinterpret_cast<void*>(eglGetProcAddress(name));
    return p;
}

// ---------------------------------------------------------------------------------------------------------------
// EAGLContext
// ---------------------------------------------------------------------------------------------------------------
void init_gl() {
    C_EAGLSharegroup = def_class("EAGLSharegroup", "NSObject");
    C_EAGLContext = def_class("EAGLContext", "NSObject");
    Class C = C_EAGLContext;
    def(C, "initWithAPI:", +[](id self, SEL, long api) -> id { return make_context(self, static_cast<int>(api), nullptr); });
    def(C, "initWithAPI:sharegroup:", +[](id self, SEL, long api, id sg) -> id { return make_context(self, static_cast<int>(api), sg); });
    defc(C, "setCurrentContext:", +[](id, SEL, id ctx) -> bool { return set_current(ctx); });
    defc(C, "currentContext", +[](id, SEL) -> id { return t_current; });
    def(C, "API", +[](id self, SEL) -> long { return ext_get<EaglExt>(self)->st.api; });
    def(C, "sharegroup", +[](id self, SEL) -> id { return ext_get<EaglExt>(self)->sharegroup; });
    def(C, "isMultiThreaded", +[](id, SEL) -> bool { return false; });
    def(C, "setMultiThreaded:", +[](id, SEL, bool) {});
    def(C, "setDebugLabel:", +[](id, SEL, id) {});
    def(C, "renderbufferStorage:fromDrawable:", +[](id self, SEL, GLenum, id layer) -> bool {
        auto* e = ext_get<EaglExt>(self);
        if (t_ctx != &e->st) { log("renderbufferStorage:fromDrawable: on a context that is not current"); }
        ensure_window_surface();
        e->st.drawable_rbs.insert(e->st.bound_rb);
        if (trace_enabled("gl")) log("renderbuffer %u gets the drawable of layer %p", e->st.bound_rb, static_cast<void*>(layer));
        return true;
    });
    def(C, "presentRenderbuffer:", +[](id self, SEL, GLenum) -> bool {
        auto* e = ext_get<EaglExt>(self);
        if (t_ctx != &e->st) return false;
        return present();
    });
    def(C, "presentRenderbuffer:atTime:", +[](id self, SEL, GLenum, double) -> bool { return t_ctx == &ext_get<EaglExt>(self)->st && present(); });
    def(C, "presentRenderbuffer:afterMinimumDuration:", +[](id self, SEL, GLenum, double) -> bool { return t_ctx == &ext_get<EaglExt>(self)->st && present(); });
}

static id* nsconst(const char* s) { id* slot = new id; *slot = str_new(s); return slot; }

void register_gl_symbols(std::unordered_map<std::string, void*>& m) {
    m["_kEAGLColorFormatRGBA8"] = nsconst("EAGLColorFormatRGBA8");
    m["_kEAGLColorFormatRGB565"] = nsconst("EAGLColorFormat565");
    m["_kEAGLColorFormatSRGBA8"] = nsconst("EAGLColorFormatSRGBA8");
    m["_kEAGLDrawablePropertyColorFormat"] = nsconst("EAGLDrawablePropertyColorFormat");
    m["_kEAGLDrawablePropertyRetainedBacking"] = nsconst("EAGLDrawablePropertyRetainedBacking");
}

}  // namespace app
