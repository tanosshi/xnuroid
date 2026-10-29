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
#include <jni.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" int loader_main(int argc, char** argv);
extern "C" void host_touch(int id, int phase, float nx, float ny);          // fake_uikit.hpp
extern "C" void host_set_vibrate_hook(void (*hook)(int ms, int amplitude));  // haptics.cpp

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
    // no eglTerminate: the display is process-wide and the engine's renderer uses it too
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

// Touches: positions normalized to the window, phases in UIKit terms (0 began, 1 moved, 3 ended, 4 cancelled).
int32_t on_input(android_app* app, AInputEvent* ev) {
    if (AInputEvent_getType(ev) != AINPUT_EVENT_TYPE_MOTION || !app->window) return 0;
    const float w = static_cast<float>(ANativeWindow_getWidth(app->window)), h = static_cast<float>(ANativeWindow_getHeight(app->window));
    if (w <= 0 || h <= 0) return 0;
    const int32_t action = AMotionEvent_getAction(ev), kind = action & AMOTION_EVENT_ACTION_MASK;
    const size_t index = static_cast<size_t>((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
    auto send = [&](size_t i, int phase) {
        host_touch(AMotionEvent_getPointerId(ev, i), phase, AMotionEvent_getX(ev, i) / w, AMotionEvent_getY(ev, i) / h);
    };
    switch (kind) {
        case AMOTION_EVENT_ACTION_DOWN: case AMOTION_EVENT_ACTION_POINTER_DOWN: send(index, 0); break;
        case AMOTION_EVENT_ACTION_UP: case AMOTION_EVENT_ACTION_POINTER_UP: send(index, 3); break;
        case AMOTION_EVENT_ACTION_MOVE:
            for (size_t i = 0; i < AMotionEvent_getPointerCount(ev); ++i) send(i, 1);
            break;
        case AMOTION_EVENT_ACTION_CANCEL:
            for (size_t i = 0; i < AMotionEvent_getPointerCount(ev); ++i) send(i, 4);
            break;
        default: return 0;
    }
    return 1;
}

// Vibrator.vibrate(VibrationEffect.createOneShot(ms, amplitude)) through JNI; the calling thread stays attached.
JavaVM* g_vm = nullptr;
jobject g_vibrator = nullptr;
jmethodID g_vibrate = nullptr, g_one_shot = nullptr;
jclass g_effect_class = nullptr;
void jni_vibrate(int ms, int amplitude) {
    JNIEnv* env = nullptr;
    if (!g_vm || !g_vibrator || g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
    jobject effect = env->CallStaticObjectMethod(g_effect_class, g_one_shot, static_cast<jlong>(ms), static_cast<jint>(amplitude));
    if (effect) env->CallVoidMethod(g_vibrator, g_vibrate, effect);
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (effect) env->DeleteLocalRef(effect);
}
void setup_vibrator(android_app* app) {
    JNIEnv* env = nullptr;
    g_vm = app->activity->vm;
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
    jclass context = env->FindClass("android/content/Context");
    jmethodID get_service = env->GetMethodID(context, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    jstring name = env->NewStringUTF("vibrator");
    jobject vib = env->CallObjectMethod(app->activity->clazz, get_service, name);
    jclass vib_class = env->FindClass("android/os/Vibrator");
    jclass effect = env->FindClass("android/os/VibrationEffect");
    if (env->ExceptionCheck() || !vib || !vib_class || !effect) { env->ExceptionClear(); log("no vibrator"); return; }
    g_vibrator = env->NewGlobalRef(vib);
    g_effect_class = static_cast<jclass>(env->NewGlobalRef(effect));
    g_vibrate = env->GetMethodID(vib_class, "vibrate", "(Landroid/os/VibrationEffect;)V");
    g_one_shot = env->GetStaticMethodID(effect, "createOneShot", "(JI)Landroid/os/VibrationEffect;");
    if (env->ExceptionCheck() || !g_vibrate || !g_one_shot) { env->ExceptionClear(); g_vibrator = nullptr; return; }
    host_set_vibrate_hook(jni_vibrate);
}

void on_app_cmd(android_app* app, int32_t cmd) {
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            if (app->window) {
                g_window = app->window;
                if (!g_loader_started) show_boot_frame(app->window);   // the renderer owns the window afterwards
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
    setenv("LOADER_SANDBOX_DIR", (internal + "/sandbox").c_str(), 0);
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
    app->onInputEvent = on_input;
    setup_vibrator(app);
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
