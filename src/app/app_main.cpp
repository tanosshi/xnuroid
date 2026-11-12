// App mode entry points called by the loader (android_loader.cpp):
//   app::prepare()         register every native class and build the C-symbol map  (before dyld binds)
//   app::resolve_symbol()  a bound import -> a native function / data / class, or nullptr to fall through
//   app::boot(info)        register the guest's own classes and categories, then run +load
//   app::run_main(entry)   call the guest main(), which calls UIApplicationMain and never returns
#include "app.hpp"

#include <fcntl.h>
#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mutex>

namespace app {

void run_loads();                                 // app_runtime.cpp
void boot_guest(const ImageInfo& info);           // app_runtime.cpp
bool override_method(const char* cls, const char* sel, bool class_method, IMP imp);
void register_guest_protocols(uintptr_t list, uintptr_t size);
void init_nib();                                  // app_nib.cpp
void apply_game_patches();                        // app_patches.cpp
void* gl_resolve(const char* name);               // app_gl.cpp

namespace {

std::once_flag g_prepared;
std::unordered_map<std::string, void*>* g_symbols;

// neuter crash reporters / analytics the title bundles: they fork Mach exception servers, spin network threads and
// read /proc in ways that only waste time in a single-player port. The classes still exist (so +[Class method]
// calls succeed); these high-traffic entry points just return quietly.
// A method that ignores its arguments and returns nil/zero, to make an analytics/ad SDK entry point inert.
static id sdk_nop(id, SEL, ...) { return nullptr; }

void disable_sdk_noise() {
    if (getenv("LOADER_APP_KEEP_SDKS")) return;
    // Bundled crash reporters, analytics and ad networks spin background threads, Mach exception servers and
    // network/file I/O that a single-player Android port neither needs nor can satisfy. Stub their launch-time
    // entry points so the classes still exist (so the game's calls succeed) but do nothing.
    auto nop = reinterpret_cast<IMP>(&sdk_nop);
    struct { const char* cls; const char* sel; bool cls_method; } k[] = {
        {"Flurry", "startSession:", true}, {"Flurry", "startSession:withOptions:", true}, {"Flurry", "startSession:withCrashReporting:", true},
        {"Flurry", "logEvent:", true}, {"Flurry", "logEvent:withParameters:", true}, {"Flurry", "logEvent:timed:", true},
        {"Flurry", "logEvent:withParameters:timed:", true}, {"Flurry", "endTimedEvent:withParameters:", true},
        {"Flurry", "logError:message:exception:", true}, {"Flurry", "logError:message:error:", true},
        {"Flurry", "setCrashReportingEnabled:", true}, {"Flurry", "logAllPageViews:", true}, {"Flurry", "setSessionReportsOnCloseEnabled:", true},
        {"FlurryImpl", "startSession:", true}, {"FlurryImpl", "startSession:", false},
        {"Crashlytics", "startWithAPIKey:", true}, {"Crashlytics", "startWithAPIKey:afterDelay:", true},
        {"Crashlytics", "sharedInstance", true}, {"Crashlytics", "setDelegate:", true}, {"Crashlytics", "setCrashReportingEnabled:", true},
        {"Fabric", "with:", true},
        {"Chartboost", "startWithAppId:appSignature:delegate:", true}, {"Chartboost", "cacheInterstitial:", true},
        {"Chartboost", "showInterstitial:", true}, {"Chartboost", "cacheMoreApps:", true}, {"Chartboost", "showMoreApps:", true},
        {"Chartboost", "cacheRewardedVideo:", true}, {"Chartboost", "showRewardedVideo:", true},
    };
    int n = 0;
    for (auto& e : k) if (override_method(e.cls, e.sel, e.cls_method, nop)) ++n;
    log("neutered %d analytics/ad SDK entry points (LOADER_APP_KEEP_SDKS to keep)", n);
}

void build_symbols() {
    g_symbols = new std::unordered_map<std::string, void*>();
    register_runtime_symbols(*g_symbols);
    register_foundation_symbols(*g_symbols);
    register_foundation2_symbols(*g_symbols);
    register_uikit_symbols(*g_symbols);
    register_gl_symbols(*g_symbols);
    register_audio_symbols(*g_symbols);
    register_cg_symbols(*g_symbols);
    register_misc_symbols(*g_symbols);
    register_darwin_symbols(*g_symbols);
}

}  // namespace

// register_foundation_symbols is split across the two Foundation files; the second has nothing extra, so the
// Foundation C symbols (NSString constants, etc.) live here alongside the global error-domain names.
static id* nsstr_const(const char* v) { id* slot = new id; *slot = str_new(v); return slot; }
extern "C" void app_th_NSLog();                   // app_foundation.cpp (variadic thunk)
extern "C" void app_NSLogv(id, void*);
void register_foundation_symbols(std::unordered_map<std::string, void*>& m) {
    m["_NSLog"] = reinterpret_cast<void*>(&app_th_NSLog);
    m["_NSLogv"] = reinterpret_cast<void*>(&app_NSLogv);
    for (const char* n : {"NSLocalizedDescriptionKey", "NSLocalizedFailureReasonErrorKey", "NSLocalizedRecoverySuggestionErrorKey",
                          "NSUnderlyingErrorKey", "NSFilePathErrorKey", "NSURLErrorKey", "NSURLErrorFailingURLErrorKey", "NSURLErrorFailingURLStringErrorKey",
                          "NSErrorFailingURLStringKey", "NSErrorFailingURLKey", "NSStringEncodingErrorKey",
                          "NSCocoaErrorDomain", "NSPOSIXErrorDomain", "NSOSStatusErrorDomain", "NSMachErrorDomain", "NSURLErrorDomain",
                          "NSStreamSocketSecurityLevelKey", "NSStreamDataWrittenToMemoryStreamKey", "NSStreamFileCurrentOffsetKey",
                          "NSFileSize", "NSFileType", "NSFileModificationDate", "NSFileCreationDate", "NSFilePosixPermissions", "NSFileSystemSize",
                          "NSFileSystemFreeSize", "NSFileTypeDirectory", "NSFileTypeRegular", "NSFileProtectionKey", "NSFileProtectionNone",
                          "NSFileProtectionComplete", "NSDefaultRunLoopMode", "NSRunLoopCommonModes",
                          "NSMetadataQueryDidFinishGatheringNotification", "NSMetadataQueryDidUpdateNotification", "NSMetadataItemFSNameKey",
                          "NSMetadataItemURLKey", "NSMetadataQueryUbiquitousDataScope", "NSMetadataQueryUbiquitousDocumentsScope",
                          "NSURLAuthenticationMethodServerTrust", "NSURLIsDirectoryKey", "NSURLNameKey", "NSURLFileSizeKey", "NSURLContentModificationDateKey",
                          "NSKeyValueChangeNewKey", "NSKeyValueChangeOldKey", "NSKeyValueChangeKindKey",
                          "NSLocaleCountryCode", "NSLocaleCurrencyCode", "NSLocaleLanguageCode", "NSLocaleIdentifier", "NSGregorianCalendar",
                          "NSWeekCalendarUnit", "NSCurrentLocaleDidChangeNotification"})
        m[std::string("_") + n] = nsstr_const(n);
    // KVO change dictionary keys have short values of their own, which observers look up
    m["_NSKeyValueChangeKindKey"] = nsstr_const("kind");
    m["_NSKeyValueChangeNewKey"] = nsstr_const("new");
    m["_NSKeyValueChangeOldKey"] = nsstr_const("old");
    m["_NSKeyValueChangeIndexesKey"] = nsstr_const("indexes");
    m["_NSKeyValueChangeNotificationIsPriorKey"] = nsstr_const("notificationIsPrior");
    // NSFoundationVersionNumber: iOS 8.x (so version checks behave like the shipping OS)
    static double fvn = 1144.17;
    m["_NSFoundationVersionNumber"] = &fvn;
}

void prepare() {
    std::call_once(g_prepared, [] {
        init_runtime_classes();
        init_foundation();
        init_foundation2();
        init_cg();          // UIImage lives here, before UIKit references it
        init_uikit();
        init_nib();
        init_gl();
        init_audio();
        init_av();
        init_misc();
        build_symbols();
        log("app mode ready: native classes and %zu C symbols registered", g_symbols->size());
    });
}

// A framework class we do not implement: on iOS a weak-linked class missing at runtime is simply absent, and code
// that uses it anyway gets nil from every message. A stub class (with the closest UIKit base by name) keeps that
// behavior while letting alloc/init and superclass wiring work.
static Class stub_class(const char* name) {
    if (Class c = class_named(name)) return c;
    std::string n = name;
    auto ends = [&](const char* s) { size_t k = strlen(s); return n.size() >= k && n.compare(n.size() - k, k, s) == 0; };
    const char* super = ends("GestureRecognizer") ? "UIGestureRecognizer" : ends("ViewController") ? "UIViewController"
                        : ends("Layer") ? "CALayer" : ends("View") ? "UIView" : "NSObject";
    if (!class_named(super)) super = "NSObject";
    log("stub class %s : %s", name, super);
    return def_class(strdup(name), super);
}

void* resolve_symbol(const std::string& name) {
    prepare();
    auto it = g_symbols->find(name);
    if (it != g_symbols->end()) return it->second;
    // Objective-C class / metaclass descriptors -> our native class objects
    if (name.rfind("_OBJC_CLASS_$_", 0) == 0) {
        return reinterpret_cast<void*>(stub_class(name.c_str() + 14));
    }
    if (name.rfind("_OBJC_METACLASS_$_", 0) == 0) {
        return reinterpret_cast<void*>(metaclass_of(stub_class(name.c_str() + 18)));
    }
    // Compile-time @"..." string literals (__NSCFConstantString instances in the image) bind their isa to this
    // symbol; point it at the real constant-string class so messages to literals dispatch.
    if (name == "___CFConstantStringClassReference") return reinterpret_cast<void*>(class_named("__NSCFConstantString"));
    if (name == "_OBJC_EHTYPE_$_NSException" || name == "_OBJC_EHTYPE_id") { static uint64_t t[8] = {0}; return t; }
    // dynamically looked-up GL entry points (the game resolves some through dlsym/NSAddressOfSymbol)
    if (name.rfind("_gl", 0) == 0 || name.rfind("_egl", 0) == 0) {
        if (void* p = gl_resolve(name.c_str() + 1)) return p;
    }
    return nullptr;
}

// With all-files access the host passes the shared storage root (LOADER_SHARED_STORAGE); the app's Documents then
// live in <root>/xnuroid/<display name>, where the user can reach them (like an iOS app's folder in Files). The
// first time, whatever the app already had in its private Documents is copied there.
static std::string g_copy_from, g_copy_to;
static int copy_entry(const char* path, const struct stat*, int type, struct FTW*) {
    std::string out = g_copy_to + (path + g_copy_from.size());
    if (type == FTW_D) { mkdir(out.c_str(), 0775); return 0; }
    if (type != FTW_F) return 0;
    int in = open(path, O_RDONLY | O_CLOEXEC), o = open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0664);
    char buf[1 << 16];
    ssize_t n;
    while (in >= 0 && o >= 0 && (n = read(in, buf, sizeof buf)) > 0) if (write(o, buf, static_cast<size_t>(n)) != n) break;
    if (in >= 0) close(in);
    if (o >= 0) close(o);
    return 0;
}
static void choose_documents_dir() {
    const char* root = getenv("LOADER_SHARED_STORAGE");
    if (!root || !*root) return;
    id info = info_dictionary();
    std::string name = utf8(dict_get(info, "CFBundleDisplayName"));
    if (name.empty()) name = utf8(dict_get(info, "CFBundleName"));
    if (name.empty()) name = "App";
    for (char& c : name) if (c == '/' || c == ':') c = '_';
    std::string base = std::string(root) + "/xnuroid", dir = base + "/" + name;
    struct stat st;
    bool fresh = stat(dir.c_str(), &st) != 0;
    mkdir(base.c_str(), 0775);
    mkdir(dir.c_str(), 0775);
    if (access(dir.c_str(), W_OK) != 0) { log("documents: %s is not writable, keeping them private", dir.c_str()); return; }
    if (fresh) {
        g_copy_from = sandbox_dir("Documents");
        g_copy_to = dir;
        nftw(g_copy_from.c_str(), copy_entry, 16, FTW_PHYS);
    }
    set_documents_dir(dir);
    log("documents: %s%s", dir.c_str(), fresh ? " (private documents copied)" : "");
}

void boot(const ImageInfo& info) {
    prepare();
    choose_documents_dir();
    tlv_setup(info);
    if (info.protolist) register_guest_protocols(info.protolist, info.protolist_size);
    // boot_guest (app_runtime.cpp) reads class/category/selector sections and wires superclasses to the native
    // classes already bound into the image by dyld; the guest classes must exist before we can neuter any.
    boot_guest(info);
    disable_sdk_noise();
    apply_game_patches();
}

[[noreturn]] void run_main(void* entry, int argc, char** argv) {
    log("calling guest main() at %p", entry);
    run_loads();
    auto main_fn = reinterpret_cast<int (*)(int, char**)>(entry);
    int rc = main_fn(argc, argv);
    log("guest main() returned %d (unexpected; UIApplicationMain should not return)", rc);
    run_loop_forever();
}

}  // namespace app
