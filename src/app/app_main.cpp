// App mode entry points called by the loader (android_loader.cpp):
//   app::prepare()         register every native class and build the C-symbol map  (before dyld binds)
//   app::resolve_symbol()  a bound import -> a native function / data / class, or nullptr to fall through
//   app::boot(info)        register the guest's own classes and categories, then run +load
//   app::run_main(entry)   call the guest main(), which calls UIApplicationMain and never returns
#include "app.hpp"

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
        init_misc();
        build_symbols();
        log("app mode ready: native classes and %zu C symbols registered", g_symbols->size());
    });
}

void* resolve_symbol(const std::string& name) {
    prepare();
    auto it = g_symbols->find(name);
    if (it != g_symbols->end()) return it->second;
    // Objective-C class / metaclass descriptors -> our native class objects
    if (name.rfind("_OBJC_CLASS_$_", 0) == 0) {
        Class c = class_named(name.c_str() + 14);
        return c ? reinterpret_cast<void*>(c) : nullptr;
    }
    if (name.rfind("_OBJC_METACLASS_$_", 0) == 0) {
        Class c = class_named(name.c_str() + 18);
        return c ? reinterpret_cast<void*>(metaclass_of(c)) : nullptr;
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

void boot(const ImageInfo& info) {
    prepare();
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
