// NativeActivity host: runs the Mach-O loader inside an Android app process that owns a real window.
//
// Layout on the device (adb can write the app's external files dir):
//   <internalDataPath>/game/        UnityFramework, Info.plist, Data/ ...   (LOADER_GAME_DIR; extracted from the APK's
//                                   assets/game/ on first launch and whenever assets/game.stamp changes)
//   <externalDataPath>/loader.env   optional KEY=VALUE lines, applied over the defaults below
//   <externalDataPath>/out.txt, err.txt, exit.txt   loader stdout / stderr / exit code
#include <android/asset_manager.h>
#include <android/bitmap.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <ftw.h>
#include <jni.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" int loader_main(int argc, char** argv);
extern "C" void host_touch(int id, int phase, float nx, float ny);          // fake_uikit.hpp (engine mode)
extern "C" void app_host_touch(int id, int phase, float nx, float ny) __attribute__((weak));   // app_uikit.cpp (app mode)
extern "C" void app_host_lifecycle(int active) __attribute__((weak));       // app_uikit.cpp
extern "C" void host_set_vibrate_hook(void (*hook)(int ms, int amplitude));  // haptics.cpp
extern "C" void host_set_screen_size(int width_px, int height_px);           // screen_metrics.hpp
extern "C" void host_set_safe_area(int top_px, int left_px, int bottom_px, int right_px, int width_px);

namespace {

constexpr const char* kTag = "xnuroid";
std::atomic<ANativeWindow*> g_window{nullptr};
std::atomic<unsigned> g_window_gen{0};   // bumped on every window change (a new window may reuse the old address)
std::atomic<unsigned> g_resume_count{0};   // APP_CMD_RESUME count: tells a wait that the user came back to the app
std::atomic<bool> g_loader_started{false};
std::string g_files;
AAssetManager* g_assets = nullptr;

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

std::string read_asset(const char* name) {
    AAsset* a = g_assets ? AAssetManager_open(g_assets, name, AASSET_MODE_BUFFER) : nullptr;
    if (!a) return "";
    std::string s(static_cast<const char*>(AAsset_getBuffer(a)), static_cast<size_t>(AAsset_getLength(a)));
    AAsset_close(a);
    return s;
}

std::string read_file(const std::string& path) {
    std::string s;
    if (FILE* fp = fopen(path.c_str(), "rb")) {
        char buf[256];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, fp)) > 0) s.append(buf, n);
        fclose(fp);
    }
    return s;
}

void make_dirs(const std::string& path) {
    for (size_t i = path.find('/', 1); i != std::string::npos; i = path.find('/', i + 1)) mkdir(path.substr(0, i).c_str(), 0700);
    mkdir(path.c_str(), 0700);
}

void remove_tree(const std::string& path) {
    nftw(path.c_str(), [](const char* p, const struct stat*, int, FTW*) { return remove(p); }, 16, FTW_DEPTH | FTW_PHYS);
}

// build_apk.sh packages the game bundle as assets/game/<path>, lists the paths in assets/game.files and versions
// them with assets/game.stamp. The guest opens its bundle with ordinary file calls, so the bundle is copied out to
// LOADER_GAME_DIR on first launch and again whenever the stamp changes. The stamp is written last, so an
// interrupted copy starts over. An APK without a bundled game uses whatever LOADER_GAME_DIR already holds.
bool extract_bundled_game() {
    const std::string stamp = read_asset("game.stamp");
    if (stamp.empty()) return true;
    const std::string dir = getenv("LOADER_GAME_DIR"), stamp_path = dir + ".stamp";
    if (read_file(stamp_path) == stamp) return true;
    __android_log_print(ANDROID_LOG_INFO, kTag, "extracting bundled game to %s", dir.c_str());
    timespec t0{}, t1{};
    clock_gettime(CLOCK_MONOTONIC, &t0);
    unlink(stamp_path.c_str());
    remove_tree(dir);
    make_dirs(dir);
    const std::string list = read_asset("game.files");
    std::string buf(1 << 20, '\0');
    size_t files = 0, bytes = 0;
    for (size_t pos = 0, end; pos < list.size(); pos = end + 1) {
        end = list.find('\n', pos);
        if (end == std::string::npos) end = list.size();
        const std::string rel = list.substr(pos, end - pos);
        if (rel.empty()) continue;
        AAsset* a = AAssetManager_open(g_assets, ("game/" + rel).c_str(), AASSET_MODE_STREAMING);
        const std::string out = dir + "/" + rel;
        make_dirs(out.substr(0, out.rfind('/')));
        FILE* fp = a ? fopen(out.c_str(), "wb") : nullptr;
        bool ok = fp != nullptr;
        for (int n; ok && (n = AAsset_read(a, buf.data(), buf.size())) != 0;) {
            ok = n > 0 && fwrite(buf.data(), 1, static_cast<size_t>(n), fp) == static_cast<size_t>(n);
            bytes += n > 0 ? static_cast<size_t>(n) : 0;
        }
        if (fp && fclose(fp) != 0) ok = false;
        if (a) AAsset_close(a);
        if (!ok) {
            __android_log_print(ANDROID_LOG_ERROR, kTag, "extracting game/%s failed", rel.c_str());
            fprintf(stderr, "[FATAL] extracting bundled game/%s failed (storage full?)\n", rel.c_str());
            return false;
        }
        ++files;
    }
    if (FILE* fp = fopen(stamp_path.c_str(), "wb")) {
        fwrite(stamp.data(), 1, stamp.size(), fp);
        fclose(fp);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    __android_log_print(ANDROID_LOG_INFO, kTag, "extracted %zu files (%zu MB) in %.1f s", files, bytes >> 20,
                        (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9);
    return true;
}

extern "C" const char* host_shared_storage(int request, int wait_s);

// Every app's documents go to shared storage when the app has "All files access" (the guest's Documents becomes
// <shared>/xnuroid/<app name>, and any path on the phone can be opened). It is asked for once, on first launch;
// denied, the documents stay in app-private storage until it is granted in Settings. LOADER_SHARED_DOCUMENTS=0 opts out.
void choose_storage() {
    const char* opt = getenv("LOADER_SHARED_DOCUMENTS");
    if (opt && std::strcmp(opt, "0") == 0) return;
    mkdir(getenv("LOADER_SANDBOX_DIR"), 0700);
    const std::string marker = std::string(getenv("LOADER_SANDBOX_DIR")) + "/.storage_asked";
    struct stat st;
    bool ask = stat(marker.c_str(), &st) != 0;
    const char* root = host_shared_storage(ask, 120);
    if (ask) if (FILE* fp = fopen(marker.c_str(), "w")) fclose(fp);
    if (root) setenv("LOADER_SHARED_STORAGE", root, 1);
}

void* loader_thread(void*) {
    static char arg0[] = "xnuroid";
    char* argv[] = {arg0, nullptr};
    int rc = 1;
    choose_storage();
    if (extract_bundled_game()) {
        log("loader_main starting");
        rc = loader_main(1, argv);
    }
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
    if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_KEY) {
        // Gamepad buttons are swallowed: left unhandled, the system turns B into BACK, which closes the game.
        const int32_t key = AKeyEvent_getKeyCode(ev);
        return (key >= AKEYCODE_BUTTON_A && key <= AKEYCODE_BUTTON_MODE) || (key >= AKEYCODE_BUTTON_1 && key <= AKEYCODE_BUTTON_16);
    }
    if (AInputEvent_getType(ev) != AINPUT_EVENT_TYPE_MOTION || !app->window) return 0;
    // Touchscreen, mouse, stylus. A joystick also sends ACTION_MOVE, with axis values where a pointer has pixels.
    if (!(AInputEvent_getSource(ev) & AINPUT_SOURCE_CLASS_POINTER)) return 0;
    const float w = static_cast<float>(ANativeWindow_getWidth(app->window)), h = static_cast<float>(ANativeWindow_getHeight(app->window));
    if (w <= 0 || h <= 0) return 0;
    const int32_t action = AMotionEvent_getAction(ev), kind = action & AMOTION_EVENT_ACTION_MASK;
    const size_t index = static_cast<size_t>((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
    auto send = [&](size_t i, int phase) {
        int id = AMotionEvent_getPointerId(ev, i);
        float nx = AMotionEvent_getX(ev, i) / w, ny = AMotionEvent_getY(ev, i) / h;
        host_touch(id, phase, nx, ny);
        if (app_host_touch) app_host_touch(id, phase, nx, ny);
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

JavaVM* g_vm = nullptr;
// JNIEnv of the calling thread. A thread that was not attached yet stays attached until it ends: the key
// destructor detaches it, because ART aborts when an attached thread exits.
JNIEnv* jni_env() {
    JNIEnv* env = nullptr;
    if (!g_vm) return nullptr;
    if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
    static pthread_key_t key;
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, [] { pthread_key_create(&key, [](void*) { g_vm->DetachCurrentThread(); }); });
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
    pthread_setspecific(key, &key);
    return env;
}

// Vibrator.vibrate(VibrationEffect.createOneShot(ms, amplitude)) through JNI.
jobject g_vibrator = nullptr;
jmethodID g_vibrate = nullptr, g_one_shot = nullptr;
jclass g_effect_class = nullptr;
void jni_vibrate(int ms, int amplitude) {
    JNIEnv* env = g_vibrator ? jni_env() : nullptr;
    if (!env) return;
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

// Text through android.graphics (Paint / Canvas / Bitmap): the guest's CoreText measures and draws with it, so app
// text gets the system fonts in any script. A calling thread attaches for the call and detaches again if it was not
// attached before (guest threads come and go, and an attached thread must not exit).
struct TextJni {
    jclass paint, canvas, bitmap;
    jobject argb8888;
    jmethodID paint_ctor, set_text_size, set_color, measure_text, get_font_metrics, canvas_ctor, draw_text, create_bitmap, recycle;
    jfieldID fm_ascent, fm_descent, fm_leading;
};
TextJni g_text{};
bool g_text_ready = false;

void setup_text(android_app* app) {
    JNIEnv* env = nullptr;
    if (!g_vm || g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
    auto global = [env](jclass c) { return c ? static_cast<jclass>(env->NewGlobalRef(c)) : nullptr; };
    g_text.paint = global(env->FindClass("android/graphics/Paint"));
    g_text.canvas = global(env->FindClass("android/graphics/Canvas"));
    g_text.bitmap = global(env->FindClass("android/graphics/Bitmap"));
    jclass config = env->FindClass("android/graphics/Bitmap$Config");
    jclass metrics = env->FindClass("android/graphics/Paint$FontMetrics");
    if (env->ExceptionCheck() || !g_text.paint || !g_text.canvas || !g_text.bitmap || !config || !metrics) {
        env->ExceptionClear();
        log("text: android.graphics unavailable");
        return;
    }
    g_text.argb8888 = env->NewGlobalRef(env->GetStaticObjectField(config, env->GetStaticFieldID(config, "ARGB_8888", "Landroid/graphics/Bitmap$Config;")));
    g_text.paint_ctor = env->GetMethodID(g_text.paint, "<init>", "(I)V");
    g_text.set_text_size = env->GetMethodID(g_text.paint, "setTextSize", "(F)V");
    g_text.set_color = env->GetMethodID(g_text.paint, "setColor", "(I)V");
    g_text.measure_text = env->GetMethodID(g_text.paint, "measureText", "(Ljava/lang/String;)F");
    g_text.get_font_metrics = env->GetMethodID(g_text.paint, "getFontMetrics", "()Landroid/graphics/Paint$FontMetrics;");
    g_text.canvas_ctor = env->GetMethodID(g_text.canvas, "<init>", "(Landroid/graphics/Bitmap;)V");
    g_text.draw_text = env->GetMethodID(g_text.canvas, "drawText", "(Ljava/lang/String;FFLandroid/graphics/Paint;)V");
    g_text.create_bitmap = env->GetStaticMethodID(g_text.bitmap, "createBitmap", "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;");
    g_text.recycle = env->GetMethodID(g_text.bitmap, "recycle", "()V");
    g_text.fm_ascent = env->GetFieldID(metrics, "ascent", "F");
    g_text.fm_descent = env->GetFieldID(metrics, "descent", "F");
    g_text.fm_leading = env->GetFieldID(metrics, "leading", "F");
    if (env->ExceptionCheck() || !g_text.argb8888) { env->ExceptionClear(); log("text: android.graphics methods missing"); return; }
    g_text_ready = true;
    (void)app;
}

struct JniCall {
    JNIEnv* env = jni_env();
};

// "All files access" (MANAGE_EXTERNAL_STORAGE) for apps whose documents should live on shared storage.
struct StorageJni {
    jobject activity;
    jclass environment, intent, uri;
    jmethodID is_manager, storage_dir, file_path, intent_ctor, uri_parse, start_activity, package_name;
};
StorageJni g_storage{};
bool g_storage_ready = false;

void setup_storage(android_app* app) {
    JNIEnv* env = nullptr;
    if (!g_vm || g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
    auto global = [env](jclass c) { return c ? static_cast<jclass>(env->NewGlobalRef(c)) : nullptr; };
    g_storage.environment = global(env->FindClass("android/os/Environment"));
    g_storage.intent = global(env->FindClass("android/content/Intent"));
    g_storage.uri = global(env->FindClass("android/net/Uri"));
    jclass file = env->FindClass("java/io/File");
    jclass activity = env->GetObjectClass(app->activity->clazz);
    if (env->ExceptionCheck() || !g_storage.environment || !g_storage.intent || !g_storage.uri || !file) { env->ExceptionClear(); return; }
    g_storage.activity = env->NewGlobalRef(app->activity->clazz);
    g_storage.is_manager = env->GetStaticMethodID(g_storage.environment, "isExternalStorageManager", "()Z");
    g_storage.storage_dir = env->GetStaticMethodID(g_storage.environment, "getExternalStorageDirectory", "()Ljava/io/File;");
    g_storage.file_path = env->GetMethodID(file, "getAbsolutePath", "()Ljava/lang/String;");
    g_storage.intent_ctor = env->GetMethodID(g_storage.intent, "<init>", "(Ljava/lang/String;Landroid/net/Uri;)V");
    g_storage.uri_parse = env->GetStaticMethodID(g_storage.uri, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
    g_storage.start_activity = env->GetMethodID(activity, "startActivity", "(Landroid/content/Intent;)V");
    g_storage.package_name = env->GetMethodID(activity, "getPackageName", "()Ljava/lang/String;");
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
    g_storage_ready = true;
}

// The system document picker, through HostActivity.pickDocuments (android/java); the result comes back through
// onDocumentsPicked on the UI thread and goes to the one pending callback.
using PickCallback = void (*)(void* ctx, const char* const* paths, int count);
std::mutex g_pick_mu;
PickCallback g_pick_cb = nullptr;
void* g_pick_ctx = nullptr;
jmethodID g_pick_documents = nullptr;

void JNICALL on_documents_picked(JNIEnv* env, jclass, jobjectArray paths) {
    PickCallback cb;
    void* ctx;
    {
        std::lock_guard<std::mutex> l(g_pick_mu);
        cb = g_pick_cb;
        ctx = g_pick_ctx;
        g_pick_cb = nullptr;
    }
    if (!cb) return;
    std::vector<std::string> v;
    for (jsize i = 0, n = paths ? env->GetArrayLength(paths) : 0; i < n; ++i) {
        auto s = static_cast<jstring>(env->GetObjectArrayElement(paths, i));
        if (const char* c = s ? env->GetStringUTFChars(s, nullptr) : nullptr) { v.emplace_back(c); env->ReleaseStringUTFChars(s, c); }
        if (s) env->DeleteLocalRef(s);
    }
    std::vector<const char*> p;
    for (auto& s : v) p.push_back(s.c_str());
    __android_log_print(ANDROID_LOG_INFO, kTag, "document picker: %zu picked", p.size());
    cb(ctx, p.data(), static_cast<int>(p.size()));
}

void setup_picker() {
    JNIEnv* env = nullptr;
    if (!g_storage_ready || g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
    jclass activity = env->GetObjectClass(g_storage.activity);
    g_pick_documents = env->GetMethodID(activity, "pickDocuments", "(ZZ)V");
    static const JNINativeMethod natives[] = {{"onDocumentsPicked", "([Ljava/lang/String;)V", reinterpret_cast<void*>(&on_documents_picked)}};
    if (env->ExceptionCheck() || !g_pick_documents || env->RegisterNatives(activity, natives, 1) != JNI_OK) {
        env->ExceptionClear();
        g_pick_documents = nullptr;
        log("document picker unavailable (activity is not HostActivity)");
    }
}

// Media playback for AVPlayer / AVAudioPlayer: HostActivity's MediaPlayer (see HostActivity.media*). Events come back
// on the UI thread and go to the app layer's callback.
struct MediaJni {
    jmethodID open = nullptr, play, pause, close, seek, volume, loop, position, duration;
    jclass activity_class = nullptr;
};
MediaJni g_media;
std::atomic<void (*)(int, int)> g_media_event{nullptr};

void JNICALL on_media_event(JNIEnv*, jclass, jint id, jint event) {
    if (auto cb = g_media_event.load()) cb(id, event);
}

void setup_media() {
    JNIEnv* env = nullptr;
    if (!g_storage_ready || g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
    jclass activity = env->GetObjectClass(g_storage.activity);
    MediaJni m;
    m.open = env->GetMethodID(activity, "mediaOpen", "(ILjava/lang/String;Z)V");
    m.play = env->GetMethodID(activity, "mediaPlay", "(IIIII)V");
    m.pause = env->GetMethodID(activity, "mediaPause", "(I)V");
    m.close = env->GetMethodID(activity, "mediaClose", "(I)V");
    m.seek = env->GetMethodID(activity, "mediaSeek", "(II)V");
    m.volume = env->GetMethodID(activity, "mediaSetVolume", "(IF)V");
    m.loop = env->GetMethodID(activity, "mediaSetLoop", "(IZ)V");
    m.position = env->GetMethodID(activity, "mediaPosition", "(I)I");
    m.duration = env->GetStaticMethodID(activity, "mediaDuration", "(Ljava/lang/String;)I");
    static const JNINativeMethod natives[] = {{"onMediaEvent", "(II)V", reinterpret_cast<void*>(&on_media_event)}};
    if (env->ExceptionCheck() || !m.duration || env->RegisterNatives(activity, natives, 1) != JNI_OK) {
        env->ExceptionClear();
        log("media playback unavailable (activity is not HostActivity)");
        return;
    }
    m.activity_class = static_cast<jclass>(env->NewGlobalRef(activity));
    g_media = m;
}

jobject make_paint(JNIEnv* env, float size, jint argb) {
    jobject p = env->NewObject(g_text.paint, g_text.paint_ctor, 1 /* ANTI_ALIAS_FLAG */);
    env->CallVoidMethod(p, g_text.set_text_size, size);
    env->CallVoidMethod(p, g_text.set_color, argb);
    return p;
}

// Display.getRotation() (Surface.ROTATION_*), polled from the glue thread: a 180-degree flip between the two
// landscape orientations changes no configuration, so there is no event to wait for.
std::atomic<int> g_rotation{-1};
jobject g_display = nullptr;
jmethodID g_get_rotation = nullptr;
void refresh_rotation(android_app* app) {
    JNIEnv* env = nullptr;
    if (!g_vm || g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
    if (!g_display) {
        if (env->PushLocalFrame(8) != JNI_OK) return;
        jobject wm = env->CallObjectMethod(app->activity->clazz,
            env->GetMethodID(env->GetObjectClass(app->activity->clazz), "getWindowManager", "()Landroid/view/WindowManager;"));
        jobject display = wm ? env->CallObjectMethod(wm, env->GetMethodID(env->FindClass("android/view/WindowManager"),
            "getDefaultDisplay", "()Landroid/view/Display;")) : nullptr;
        if (!env->ExceptionCheck() && display) {
            g_display = env->NewGlobalRef(display);
            g_get_rotation = env->GetMethodID(env->FindClass("android/view/Display"), "getRotation", "()I");
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->PopLocalFrame(nullptr);
        if (!g_display || !g_get_rotation) return;
    }
    jint r = env->CallIntMethod(g_display, g_get_rotation);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
    if (g_rotation.exchange(r) != r) __android_log_print(ANDROID_LOG_INFO, kTag, "display rotation %d", r * 90);
}

// UI thread: hides the system bars (a swipe shows them transiently) and reports their full extent, the display cutout
// and the mandatory gesture areas (the home-indicator analogue) to the guest as safe-area insets. Root insets exist
// once the window is attached, i.e. by the first focus.
void apply_system_ui(ANativeActivity* activity) {
    JNIEnv* env = activity->env;
    if (env->PushLocalFrame(16) != JNI_OK) return;
    auto failed = [env] {
        if (!env->ExceptionCheck()) return false;
        env->ExceptionClear();
        return true;
    };
    [&] {
        jclass window_class = env->FindClass("android/view/Window");
        jclass type_class = env->FindClass("android/view/WindowInsets$Type");
        jclass controller_class = env->FindClass("android/view/WindowInsetsController");
        jclass view_class = env->FindClass("android/view/View");
        jclass insets_class = env->FindClass("android/view/WindowInsets");
        jclass rect_class = env->FindClass("android/graphics/Insets");
        if (failed()) return;
        jobject window = env->CallObjectMethod(activity->clazz,
            env->GetMethodID(env->GetObjectClass(activity->clazz), "getWindow", "()Landroid/view/Window;"));
        jint bars = env->CallStaticIntMethod(type_class, env->GetStaticMethodID(type_class, "systemBars", "()I"));
        jint cutout = env->CallStaticIntMethod(type_class, env->GetStaticMethodID(type_class, "displayCutout", "()I"));
        jint gestures = env->CallStaticIntMethod(type_class, env->GetStaticMethodID(type_class, "mandatorySystemGestures", "()I"));
        if (failed() || !window) return;
        jobject controller = env->CallObjectMethod(window,
            env->GetMethodID(window_class, "getInsetsController", "()Landroid/view/WindowInsetsController;"));
        if (failed()) return;
        if (controller) {
            constexpr jint kShowTransientBarsBySwipe = 2;
            env->CallVoidMethod(controller, env->GetMethodID(controller_class, "setSystemBarsBehavior", "(I)V"), kShowTransientBarsBySwipe);
            env->CallVoidMethod(controller, env->GetMethodID(controller_class, "hide", "(I)V"), bars);
            if (failed()) return;
        }
        jobject decor = env->CallObjectMethod(window, env->GetMethodID(window_class, "getDecorView", "()Landroid/view/View;"));
        if (failed() || !decor) return;
        jobject root = env->CallObjectMethod(decor, env->GetMethodID(view_class, "getRootWindowInsets", "()Landroid/view/WindowInsets;"));
        if (failed() || !root) return;
        jobject unsafe = env->CallObjectMethod(root,
            env->GetMethodID(insets_class, "getInsetsIgnoringVisibility", "(I)Landroid/graphics/Insets;"), bars | cutout);
        jobject gesture = env->CallObjectMethod(root, env->GetMethodID(insets_class, "getInsets", "(I)Landroid/graphics/Insets;"), gestures);
        if (failed() || !unsafe || !gesture) return;
        jobject inset = env->CallStaticObjectMethod(rect_class,
            env->GetStaticMethodID(rect_class, "max", "(Landroid/graphics/Insets;Landroid/graphics/Insets;)Landroid/graphics/Insets;"), unsafe, gesture);
        jint width = env->CallIntMethod(decor, env->GetMethodID(view_class, "getWidth", "()I"));
        if (failed() || !inset) return;
        auto side = [&](const char* name) { return env->GetIntField(inset, env->GetFieldID(rect_class, name, "I")); };
        const jint top = side("top"), left = side("left"), bottom = side("bottom"), right = side("right");
        if (failed()) return;
        host_set_safe_area(top, left, bottom, right, width);
        __android_log_print(ANDROID_LOG_INFO, kTag, "safe area px top=%d left=%d bottom=%d right=%d of width %d",
                            top, left, bottom, right, width);
    }();
    env->PopLocalFrame(nullptr);
}

void (*g_glue_focus_changed)(ANativeActivity*, int) = nullptr;
void on_focus_changed(ANativeActivity* activity, int focused) {
    if (focused) apply_system_ui(activity);
    g_glue_focus_changed(activity, focused);
}

void on_app_cmd(android_app* app, int32_t cmd) {
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            if (app->window) {
                g_window = app->window;
                ++g_window_gen;
                if (!g_loader_started) {   // the guest screen is fixed once the engine starts; the renderer owns the window
                    host_set_screen_size(ANativeWindow_getWidth(app->window), ANativeWindow_getHeight(app->window));
                    show_boot_frame(app->window);
                }
                start_loader();
            }
            break;
        case APP_CMD_TERM_WINDOW:
            // The renderer watches g_window and tears its EGL surface down on the next frame; clearing it here
            // (before the framework destroys the ANativeWindow) keeps the render thread off the dead window.
            g_window = nullptr;
            ++g_window_gen;
            break;
        case APP_CMD_RESUME:
            ++g_resume_count;
            [[fallthrough]];
        case APP_CMD_GAINED_FOCUS:
            if (app_host_lifecycle) app_host_lifecycle(1);
            break;
        case APP_CMD_LOST_FOCUS:
        case APP_CMD_PAUSE:
            if (app_host_lifecycle) app_host_lifecycle(0);
            break;
        default:
            break;
    }
}

void configure(android_app* app) {
    g_files = app->activity->externalDataPath ? app->activity->externalDataPath : "";
    g_assets = app->activity->assetManager;
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
#ifdef XNUROID_KEEP_SPLASH
    // Built with KEEP_SPLASH=1: leave a splash scene that waits on a VideoPlayer alone, so the game plays its own
    // launch video. loader.env can override this with LOADER_NO_VIDEO_SKIP=0.
    setenv("LOADER_NO_VIDEO_SKIP", "1", 0);
#endif
    apply_env_file(g_files + "/loader.env");
    freopen((g_files + "/out.txt").c_str(), "w", stdout);
    setvbuf(stdout, nullptr, _IOLBF, 0);   // line by line, so the log is current (and complete if the app dies)
    freopen((g_files + "/err.txt").c_str(), "w", stderr);
    log("files dir %s", g_files.c_str());
}

}  // namespace

// The window the renderer presents to (null while the activity has no surface).
extern "C" ANativeWindow* host_window() { return g_window.load(); }
// Changes whenever the window does, including when a recreated window lands at the old address.
extern "C" unsigned host_window_generation() { return g_window_gen.load(); }
// Surface.ROTATION_* of the display (0..3), or -1 before it is known.
extern "C" int host_display_rotation() { return g_rotation.load(); }

// The shared storage root (/storage/emulated/0) when the app holds "All files access", else null. With request set,
// a missing grant opens the system screen for it and waits up to wait_s seconds for the user to allow it.
extern "C" const char* host_shared_storage(int request, int wait_s) {
    static std::string root;
    JniCall j;
    JNIEnv* env = j.env;
    if (!env || !g_storage_ready || env->PushLocalFrame(8) != JNI_OK) return nullptr;
    auto granted = [env] {
        bool g = env->CallStaticBooleanMethod(g_storage.environment, g_storage.is_manager);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        return g;
    };
    bool ok = granted();
    if (!ok && request) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "asking for all-files access");
        jstring pkg = static_cast<jstring>(env->CallObjectMethod(g_storage.activity, g_storage.package_name));
        const char* p = pkg ? env->GetStringUTFChars(pkg, nullptr) : nullptr;
        std::string uri = std::string("package:") + (p ? p : "");
        if (p) env->ReleaseStringUTFChars(pkg, p);
        jobject u = env->CallStaticObjectMethod(g_storage.uri, g_storage.uri_parse, env->NewStringUTF(uri.c_str()));
        jobject intent = env->NewObject(g_storage.intent, g_storage.intent_ctor, env->NewStringUTF("android.settings.MANAGE_APP_ALL_FILES_ACCESS_PERMISSION"), u);
        unsigned resumes = g_resume_count.load();
        if (!env->ExceptionCheck() && intent) env->CallVoidMethod(g_storage.activity, g_storage.start_activity, intent);
        if (env->ExceptionCheck()) env->ExceptionClear();
        // until it is allowed, or the user comes back without allowing it
        for (int i = 0; i < wait_s * 4 && !(ok = granted()); ++i) {
            if (i > 4 && g_resume_count.load() != resumes) { ok = granted(); break; }
            usleep(250000);
        }
        __android_log_print(ANDROID_LOG_INFO, kTag, "all-files access %s", ok ? "granted" : "not granted");
    }
    if (ok && root.empty()) {
        jobject dir = env->CallStaticObjectMethod(g_storage.environment, g_storage.storage_dir);
        jstring path = dir ? static_cast<jstring>(env->CallObjectMethod(dir, g_storage.file_path)) : nullptr;
        if (const char* s = path ? env->GetStringUTFChars(path, nullptr) : nullptr) { root = s; env->ReleaseStringUTFChars(path, s); }
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (root.empty()) root = "/storage/emulated/0";
    }
    env->PopLocalFrame(nullptr);
    return ok ? root.c_str() : nullptr;
}

// Opens the system picker for files (or a folder); cb gets the chosen paths (none if cancelled) on the UI thread.
// Returns 0 if no picker could be shown (cb is then not called).
extern "C" int host_pick_documents(int folder, int multiple, PickCallback cb, void* ctx) {
    JniCall j;
    JNIEnv* env = j.env;
    if (!env || !g_pick_documents) return 0;
    {
        std::lock_guard<std::mutex> l(g_pick_mu);
        if (g_pick_cb) return 0;   // one picker at a time
        g_pick_cb = cb;
        g_pick_ctx = ctx;
    }
    env->CallVoidMethod(g_storage.activity, g_pick_documents, static_cast<jboolean>(folder != 0), static_cast<jboolean>(multiple != 0));
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        std::lock_guard<std::mutex> l(g_pick_mu);
        g_pick_cb = nullptr;
        return 0;
    }
    return 1;
}

// ---- media playback (AVPlayer / AVAudioPlayer), see HostActivity.media*
extern "C" void host_media_set_callback(void (*cb)(int id, int event)) { g_media_event = cb; }

extern "C" int host_media_open(int id, const char* path, int video) {
    JniCall j;
    if (!j.env || !g_media.open || !path) return 0;
    jstring s = j.env->NewStringUTF(path);
    j.env->CallVoidMethod(g_storage.activity, g_media.open, id, s, static_cast<jboolean>(video != 0));
    j.env->DeleteLocalRef(s);
    if (j.env->ExceptionCheck()) { j.env->ExceptionClear(); return 0; }
    return 1;
}

// play / pause / close / seek / volume / loop: queued to the UI thread, no result
static void media_call(jmethodID m, int id, int a = 0, int b = 0, int c = 0, int d = 0) {
    JniCall j;
    if (!j.env || !m) return;
    if (m == g_media.play) j.env->CallVoidMethod(g_storage.activity, m, id, a, b, c, d);
    else if (m == g_media.seek) j.env->CallVoidMethod(g_storage.activity, m, id, a);
    else if (m == g_media.loop) j.env->CallVoidMethod(g_storage.activity, m, id, static_cast<jboolean>(a != 0));
    else j.env->CallVoidMethod(g_storage.activity, m, id);
    if (j.env->ExceptionCheck()) j.env->ExceptionClear();
}
extern "C" void host_media_play(int id, int left, int top, int width, int height) { media_call(g_media.play, id, left, top, width, height); }
extern "C" void host_media_pause(int id) { media_call(g_media.pause, id); }
extern "C" void host_media_close(int id) { media_call(g_media.close, id); }
extern "C" void host_media_seek(int id, int ms) { media_call(g_media.seek, id, ms); }
extern "C" void host_media_loop(int id, int loop) { media_call(g_media.loop, id, loop); }
extern "C" void host_media_volume(int id, float volume) {
    JniCall j;
    if (!j.env || !g_media.volume) return;
    j.env->CallVoidMethod(g_storage.activity, g_media.volume, id, volume);
    if (j.env->ExceptionCheck()) j.env->ExceptionClear();
}
extern "C" int host_media_position(int id) {
    JniCall j;
    if (!j.env || !g_media.position) return 0;
    jint ms = j.env->CallIntMethod(g_storage.activity, g_media.position, id);
    if (j.env->ExceptionCheck()) { j.env->ExceptionClear(); return 0; }
    return ms;
}
// length of a media file in milliseconds (0 if unknown)
extern "C" int host_media_duration(const char* path) {
    JniCall j;
    if (!j.env || !g_media.duration || !path) return 0;
    jstring s = j.env->NewStringUTF(path);
    jint ms = j.env->CallStaticIntMethod(g_media.activity_class, g_media.duration, s);
    j.env->DeleteLocalRef(s);
    if (j.env->ExceptionCheck()) { j.env->ExceptionClear(); return 0; }
    return ms;
}

// Text metrics for a UTF-16 string at size px: advance width, ascent and descent (both positive) and leading.
extern "C" int host_text_measure(const uint16_t* text, int len, float size, float* width, float* ascent, float* descent, float* leading) {
    JniCall j;
    JNIEnv* env = j.env;
    if (!env || env->PushLocalFrame(8) != JNI_OK) return 0;
    jstring s = env->NewString(reinterpret_cast<const jchar*>(text), len);
    jobject paint = make_paint(env, size, -1);
    float w = env->CallFloatMethod(paint, g_text.measure_text, s);
    jobject fm = env->CallObjectMethod(paint, g_text.get_font_metrics);
    bool ok = !env->ExceptionCheck() && fm;
    if (ok) {
        *width = w;
        *ascent = -env->GetFloatField(fm, g_text.fm_ascent);
        *descent = env->GetFloatField(fm, g_text.fm_descent);
        *leading = env->GetFloatField(fm, g_text.fm_leading);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->PopLocalFrame(nullptr);
    return ok;
}

// Draws a UTF-16 string over dst (w x h, RGBA8888 premultiplied, top row first) with its baseline starting at (x, y)
// from the top-left, in rgba (0xRRGGBBAA).
extern "C" int host_text_draw(const uint16_t* text, int len, float size, uint32_t rgba, uint8_t* dst, int w, int h, float x, float y) {
    JniCall j;
    JNIEnv* env = j.env;
    if (!env || w <= 0 || h <= 0 || env->PushLocalFrame(8) != JNI_OK) return 0;
    jint argb = static_cast<jint>((rgba & 0xff) << 24 | rgba >> 8);
    jstring s = env->NewString(reinterpret_cast<const jchar*>(text), len);
    jobject paint = make_paint(env, size, argb);
    jobject bmp = env->CallStaticObjectMethod(g_text.bitmap, g_text.create_bitmap, w, h, g_text.argb8888);
    bool ok = false;
    if (!env->ExceptionCheck() && bmp) {
        jobject canvas = env->NewObject(g_text.canvas, g_text.canvas_ctor, bmp);
        if (!env->ExceptionCheck() && canvas) env->CallVoidMethod(canvas, g_text.draw_text, s, x, y, paint);
        AndroidBitmapInfo info{};
        void* px = nullptr;
        if (!env->ExceptionCheck() && AndroidBitmap_getInfo(env, bmp, &info) == 0 && AndroidBitmap_lockPixels(env, bmp, &px) == 0) {
            for (int row = 0; row < h; ++row) {
                const uint8_t* src = static_cast<const uint8_t*>(px) + static_cast<size_t>(row) * info.stride;
                uint8_t* d = dst + static_cast<size_t>(row) * w * 4;
                for (int col = 0; col < w; ++col, src += 4, d += 4) {
                    unsigned a = src[3];
                    if (!a) continue;
                    for (int c = 0; c < 4; ++c) d[c] = static_cast<uint8_t>(src[c] + d[c] * (255 - a) / 255);   // premultiplied over
                }
            }
            AndroidBitmap_unlockPixels(env, bmp);
            ok = true;
        }
        env->CallVoidMethod(bmp, g_text.recycle);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->PopLocalFrame(nullptr);
    return ok;
}

// Activity entry point (android.app.func_name): the glue's ANativeActivity_onCreate plus UI-thread system-bar handling.
extern "C" JNIEXPORT void xnuroid_on_create(ANativeActivity* activity, void* saved_state, size_t saved_state_size) {
    ANativeActivity_onCreate(activity, saved_state, saved_state_size);
    g_glue_focus_changed = activity->callbacks->onWindowFocusChanged;
    activity->callbacks->onWindowFocusChanged = on_focus_changed;
    apply_system_ui(activity);
}

void android_main(android_app* app) {
    app->onAppCmd = on_app_cmd;
    app->onInputEvent = on_input;
    setup_vibrator(app);
    setup_text(app);
    setup_storage(app);
    setup_picker();
    setup_media();
    configure(app);
    refresh_rotation(app);
    timespec last{};
    clock_gettime(CLOCK_MONOTONIC, &last);
    while (!app->destroyRequested) {
        android_poll_source* source = nullptr;
        int events = 0;
        if (ALooper_pollOnce(500, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0 && source)
            source->process(app, source);
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((now.tv_sec - last.tv_sec) * 1000 + (now.tv_nsec - last.tv_nsec) / 1000000 >= 500) {
            last = now;
            refresh_rotation(app);
        }
    }
    // The engine cannot be unwound from another thread; end the process with the activity.
    fflush(stdout);
    fflush(stderr);
    _exit(0);
}
