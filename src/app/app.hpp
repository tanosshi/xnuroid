// App mode: running a plain Objective-C iOS app (no Unity), e.g. an iOS 5/6-era OpenGL ES 1.1 game.
//
// The guest's own classes are registered from the image and dispatched by a real (if small) Objective-C runtime.
// Foundation, UIKit, QuartzCore, OpenGLES, OpenAL, CoreGraphics and friends are native classes and functions
// implemented here. Every object our runtime allocates carries a 16-byte header in front of its isa:
//   [magic u32][refcount i32][Ext* ext][isa][guest ivars...]
// Native classes keep their state in the Ext (so a guest subclass of UIView keeps its ivars at their compiled
// offsets and never collides with ours). Objects inside the image (constant strings, global blocks) and class
// objects are immortal.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct AppObj { uintptr_t isa; };
typedef AppObj* id;
typedef const char* SEL;
typedef void* IMP;
typedef uintptr_t Class;
typedef bool BOOL;
typedef long NSInteger;
typedef unsigned long NSUInteger;

struct CGPoint { double x, y; };
struct CGSize { double width, height; };
struct CGRect { CGPoint origin; CGSize size; };
struct NSRange { NSUInteger location, length; };
struct CGAffineTransform { double a, b, c, d, tx, ty; };
constexpr NSUInteger NSNotFound = 0x7fffffffffffffffUL;

namespace app {

// ---- logging ----
void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
bool trace_enabled(const char* what);   // LOADER_APP_TRACE=objc,gl,al,...

// ---- runtime ----
struct Ext { virtual ~Ext() {} };
struct RClass;

SEL sel(const char* name);                       // canonical selector pointer
Class class_named(const char* name);             // 0 if unknown
Class class_of(id obj);                          // isa (instances) / metaclass (class objects)
Class superclass_of(Class c);
const char* class_name(Class c);
bool is_class_object(id obj);
bool is_meta(Class c);
bool is_subclass(Class c, Class of);
bool is_kind(id obj, Class of);
bool is_kind(id obj, const char* class_name);
IMP lookup(id obj, SEL s);                       // never null; misses return a nil-returning stub
IMP lookup_noforward(id obj, SEL s);             // null when the receiver does not implement s
bool responds(id obj, SEL s);
IMP nil_imp();
uint32_t instance_size(Class c);

id alloc(Class c);                               // +1, zeroed, isa set
id retain(id o);
void release(id o);
id autorelease(id o);
int32_t retain_count(id o);
void dealloc_object(id o);                       // NSObject -dealloc: drops Ext, associations, memory
bool has_header(id o);
Ext*& ext_slot(id o);                            // only for objects with a header
template <class T> T* ext_as(id o) { return o && has_header(o) ? dynamic_cast<T*>(ext_slot(o)) : nullptr; }
template <class T> T* ext_get(id o) {             // the Ext of type T, created on first use
    if (!o || !has_header(o)) return nullptr;
    Ext*& e = ext_slot(o);
    if (T* t = dynamic_cast<T*>(e)) return t;
    if (!e) { T* t = new T(); e = t; return t; }
    return nullptr;
}
void* pool_push();
void pool_pop(void* token);

// native classes
Class def_class(const char* name, const char* super_name, uint32_t instance_size = 8);
void def(Class c, const char* sel_name, IMP imp);          // instance method
void defc(Class c, const char* sel_name, IMP imp);         // class method
template <class F> inline void def(Class c, const char* s, F f) { def(c, s, reinterpret_cast<IMP>(f)); }
template <class F> inline void defc(Class c, const char* s, F f) { defc(c, s, reinterpret_cast<IMP>(f)); }
Class metaclass_of(Class c);

// message sends from native code (receiver nil -> zero value). SEL is const char*, so the selector may be a
// string literal or an already-interned selector; sel() canonicalizes either.
template <class R = id, class... A>
inline R send(id obj, const char* name, A... a) {
    if (!obj) return R();
    SEL s = sel(name);
    return reinterpret_cast<R (*)(id, SEL, A...)>(lookup(obj, s))(obj, s, a...);
}
template <class R = id, class... A>
inline R send_cls(Class c, const char* name, A... a) { return send<R, A...>(reinterpret_cast<id>(c), name, a...); }
inline id cls_obj(Class c) { return reinterpret_cast<id>(c); }
// calls the superclass implementation (start the search above `from`)
IMP lookup_super(Class from, SEL s);
template <class R = id, class... A>
inline R send_super(id obj, Class from, const char* name, A... a) {
    SEL s = sel(name);
    IMP imp = lookup_super(from, s);
    if (!obj || !imp) return R();
    return reinterpret_cast<R (*)(id, SEL, A...)>(imp)(obj, s, a...);
}
// explicit no-arg variant to send a SEL already in hand without extra template plumbing
template <class R = id>
inline R send_sel(id obj, SEL s) {
    if (!obj) return R();
    return reinterpret_cast<R (*)(id, SEL)>(lookup(obj, s))(obj, s);
}

// guest image
bool in_image(const void* p);
void set_image_range(uintptr_t lo, uintptr_t hi);
uintptr_t image_base();

// ---- strings (NSString / CFString) ----
bool is_string(id o);
std::string utf8(id s);                          // "" for nil / non-strings
const std::u16string& u16(id s);                 // empty for nil
id str(const std::string& utf8);                 // autoreleased NSString
id str_new(const std::string& utf8);             // +1
id str16(const std::u16string& s);               // autoreleased
id str16_new(const std::u16string& s, bool mutable_ = false);
const char* cstr(id s);                          // UTF-8 kept alive with the string
std::u16string to16(const std::string& s);
std::string to8(const std::u16string& s);
std::string describe(id o);                      // -description as UTF-8
// printf-style formatting with %@; args are consecutive 8-byte Darwin variadic slots
std::string format(const std::string& fmt, const uint64_t* va);

// ---- numbers / values ----
bool is_number(id o);
id num_i(int64_t v);
id num_u(uint64_t v);
id num_d(double v);
id num_f(float v);
id num_b(bool v);
int64_t num_ival(id o);
double num_dval(id o);
id value_bytes(const void* p, size_t n, const char* objctype);

// ---- collections ----
bool is_array(id o);
bool is_dict(id o);
id arr_new(const std::vector<id>& items, bool mutable_ = false);   // retains items, +1
id arr(const std::vector<id>& items);                              // autoreleased
const std::vector<id>& arr_items(id a);
void arr_add(id a, id o);
id dict_new(bool mutable_ = true);                                 // +1
id dict_get(id d, id key);
id dict_get(id d, const char* key);
void dict_set(id d, id key, id value);
void dict_set(id d, const char* key, id value);
std::vector<std::pair<id, id>> dict_entries(id d);
bool obj_equal(id a, id b);
size_t obj_hash(id a);

// ---- data ----
bool is_data(id o);
id data_new(const void* p, size_t n);            // +1 copy
id data_new_nocopy(void* p, size_t n, bool free_when_done);
const uint8_t* data_bytes(id d);
size_t data_len(id d);

// ---- property lists ----
id plist_parse(const uint8_t* p, size_t n, bool mutable_containers);   // +1 or nil
bool plist_write_xml(id root, std::string& out);
bool plist_write_binary(id root, std::string& out);
id json_parse(const uint8_t* p, size_t n);                             // +1 or nil
bool json_write(id root, std::string& out, bool pretty);

// ---- environment ----
std::string bundle_dir();                        // the .app directory
std::string sandbox_dir(const char* sub);        // writable container subdirectory, created on demand
std::string remap_path(const std::string& p);    // guest absolute paths -> device paths
id info_dictionary();                            // parsed Info.plist (immortal)
bool read_file(const std::string& path, std::string& out);

// ---- run loop (app_foundation2.cpp) ----
void post_main(std::function<void()> fn, double delay_s = 0);          // thread-safe
bool on_main_thread();
double now_s();                                  // monotonic seconds
uint64_t timer_add(double delay_s, double interval_s, std::function<void()> fn);   // interval 0 = one-shot
void timer_remove(uint64_t timer);
void timer_set_next(uint64_t timer, double delay_s);
void run_loop_once(double max_wait_s);           // main thread: run due work, waiting up to max_wait_s
[[noreturn]] void run_loop_forever();
void mark_main_thread();
void add_dealloc_hook(void (*hook)(id));
void set_frame_hook(void (*hook)());             // called once per main-loop pass (touch delivery)
// spawns a thread that can run guest code (Apple TSD bound, autorelease pool)
void spawn_thread(std::function<void()> fn, const char* name = nullptr);

// ---- registration hooks for each module ----
void init_runtime_classes();     // app_runtime.cpp: NSObject, blocks
void init_foundation();          // app_foundation.cpp
void init_foundation2();         // app_foundation2.cpp
void init_uikit();               // app_uikit.cpp
void init_gl();                  // app_gl.cpp
void init_audio();               // app_audio.cpp
void init_cg();                  // app_cg.cpp
void init_misc();                // app_misc.cpp
void register_c_symbols(std::unordered_map<std::string, void*>& m);   // every module adds its C functions/data
void register_runtime_symbols(std::unordered_map<std::string, void*>& m);
void register_foundation_symbols(std::unordered_map<std::string, void*>& m);
void register_foundation2_symbols(std::unordered_map<std::string, void*>& m);
void register_uikit_symbols(std::unordered_map<std::string, void*>& m);
void register_gl_symbols(std::unordered_map<std::string, void*>& m);
void register_audio_symbols(std::unordered_map<std::string, void*>& m);
void register_cg_symbols(std::unordered_map<std::string, void*>& m);
void register_misc_symbols(std::unordered_map<std::string, void*>& m);

}  // namespace app

// Naked thunk for a native method/function with Darwin variadic arguments: on entry sp points at the first
// variadic slot, which is passed to IMPL as an extra argument in register x<REG>.
#define APP_VARIADIC(NAME, IMPL, REG)                                                      \
    extern "C" __attribute__((naked)) void NAME() {                                        \
        asm volatile("mov x" #REG ", sp\n\tb " #IMPL);                                     \
    }

// Loader-facing entry points (android_loader.cpp)
namespace app {
struct ImageInfo {
    uintptr_t base = 0, size = 0;
    int64_t slide = 0;
    uintptr_t classlist = 0, classlist_size = 0;
    uintptr_t catlist = 0, catlist_size = 0;
    uintptr_t nlclslist = 0, nlclslist_size = 0;
    uintptr_t nlcatlist = 0, nlcatlist_size = 0;
    uintptr_t selrefs = 0, selrefs_size = 0;
    uintptr_t methname = 0, methname_size = 0;
    uintptr_t protolist = 0, protolist_size = 0;
};
void prepare();                                   // register native classes + C symbols (idempotent)
void* resolve_symbol(const std::string& name);    // null if app mode does not provide it
void boot(const ImageInfo& info);                 // after binds: register classes, attach categories, +load
[[noreturn]] void run_main(void* entry, int argc, char** argv);
std::string bundle_executable(const std::string& bundle);   // CFBundleExecutable from Info.plist, "" if none
}  // namespace app
