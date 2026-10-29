// NativeActivity host: runs the Mach-O loader inside an Android app process that owns a real window.
//
// Layout on the device (adb can write the app's external files dir):
//   <internalDataPath>/game/        UnityFramework, Info.plist, Data/ ...   (LOADER_GAME_DIR; the app cannot read
//                                   files adb creates in its external dir, so build_apk.sh copies via run-as)
//   <externalDataPath>/loader.env   optional KEY=VALUE lines, applied over the defaults below
//   <externalDataPath>/out.txt, err.txt, exit.txt   loader stdout / stderr / exit code
#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" int loader_main(int argc, char** argv);

namespace {

constexpr const char* kTag = "xnuroid";
std::atomic<ANativeWindow*> g_window{nullptr};
std::atomic<bool> g_loader_started{false};
std::string g_files;

void log(const char* fmt, const char* arg = "") { __android_log_print(ANDROID_LOG_INFO, kTag, fmt, arg); }

std::string library_dir() {
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void*>(&library_dir), &info) || !info.dli_fname) return "";
    std::string p = info.dli_fname;
    size_t slash = p.rfind('/');
    return slash == std::string::npos ? "" : p.substr(0, slash);
}

void apply_env_file(const std::string& path) {
    FILE* fp = fopen(path.c_str(), "r");
    if (!fp) return;
    char line[1024];
    while (fgets(line, sizeof line, fp)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
        size_t eq = s.find('=');
        if (s.empty() || s[0] == '#' || eq == std::string::npos) continue;
        setenv(s.substr(0, eq).c_str(), s.substr(eq + 1).c_str(), 1);
        log("env %s", s.c_str());
    }
    fclose(fp);
}

// One cleared frame so the window shows the host is alive before the engine has drawn anything. EGL is torn down
// again so the renderer can create its own context on Unity's render thread.
void show_boot_frame(ANativeWindow* window) {
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, nullptr, nullptr)) return;
    const EGLint cfg_attr[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                               EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    EGLConfig cfg;
    EGLint n = 0;
    if (eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) && n > 0) {
        const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
        EGLSurface surf = eglCreateWindowSurface(dpy, cfg, window, nullptr);
        if (ctx != EGL_NO_CONTEXT && surf != EGL_NO_SURFACE && eglMakeCurrent(dpy, surf, surf, ctx)) {
            glClearColor(0.06f, 0.08f, 0.12f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            eglSwapBuffers(dpy, surf);
            eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }
        if (surf != EGL_NO_SURFACE) eglDestroySurface(dpy, surf);
        if (ctx != EGL_NO_CONTEXT) eglDestroyContext(dpy, ctx);
    }
    eglTerminate(dpy);
}

void* loader_thread(void*) {
    static char arg0[] = "xnuroid";
    char* argv[] = {arg0, nullptr};
    log("loader_main starting");
    int rc = loader_main(1, argv);
    fflush(stdout);
    fflush(stderr);
    if (FILE* fp = fopen((g_files + "/exit.txt").c_str(), "w")) {
        fprintf(fp, "EXIT=%d\n", rc);
        fclose(fp);
    }
    __android_log_print(ANDROID_LOG_INFO, kTag, "loader_main returned %d", rc);
    return nullptr;
}

void start_loader() {
    if (g_loader_started.exchange(true)) return;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64u << 20);   // Unity's iOS main thread runs on the 8 MB+ main stack
    pthread_t t;
    if (pthread_create(&t, &attr, loader_thread, nullptr) != 0) log("pthread_create for the loader failed");
    pthread_attr_destroy(&attr);
}

void on_app_cmd(android_app* app, int32_t cmd) {
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            if (app->window) {
                g_window = app->window;
                show_boot_frame(app->window);
                start_loader();
            }
            break;
        case APP_CMD_TERM_WINDOW:
            g_window = nullptr;
            break;
        default:
            break;
    }
}

void configure(android_app* app) {
    g_files = app->activity->externalDataPath ? app->activity->externalDataPath : "";
    std::string internal = app->activity->internalDataPath ? app->activity->internalDataPath : g_files;
    std::string tmp = internal + "/tmp/";
    mkdir(tmp.c_str(), 0700);
    setenv("LOADER_GAME_DIR", (internal + "/game").c_str(), 0);
    setenv("LOADER_TMP_DIR", tmp.c_str(), 0);
    setenv("LOADER_LIBCXX", (library_dir() + "/libsysc++.so").c_str(), 0);
    setenv("LOADER_ENGINE", "8", 0);
    setenv("LOADER_FDE", "all", 0);
    setenv("LOADER_FRAMES", "100000000", 0);
    apply_env_file(g_files + "/loader.env");
    freopen((g_files + "/out.txt").c_str(), "w", stdout);
    freopen((g_files + "/err.txt").c_str(), "w", stderr);
    log("files dir %s", g_files.c_str());
}

}  // namespace

// The window the renderer presents to (null while the activity has no surface).
extern "C" ANativeWindow* host_window() { return g_window.load(); }

void android_main(android_app* app) {
    app->onAppCmd = on_app_cmd;
    configure(app);
    while (!app->destroyRequested) {
        android_poll_source* source = nullptr;
        int events = 0;
        if (ALooper_pollOnce(-1, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0 && source)
            source->process(app, source);
    }
    // The engine cannot be unwound from another thread; end the process with the activity.
    fflush(stdout);
    fflush(stderr);
    _exit(0);
}
