#include "shims.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace loader {

namespace {

void shim_cxa_atexit(CallFrame& frame, void*) {
    // x0 = func, x1 = arg, x2 = dso_handle
    frame.ret_x0 = 0;
}

void shim_cxa_guard_acquire(CallFrame& frame, void*) {
    // x0 = guard pointer
    // In our simplified host environment: returns 1 if not yet initialized, 0 if done.
    frame.ret_x0 = 1;
}

void shim_cxa_guard_release(CallFrame& frame, void*) {
    frame.ret_x0 = 0;
}

void shim_malloc(CallFrame& frame, void*) {
    size_t size = static_cast<size_t>(frame.x[0]);
    void* ptr = std::calloc(1, size ? size : 16);
    frame.ret_x0 = reinterpret_cast<uint64_t>(ptr);
}

void shim_free(CallFrame& frame, void*) {
    void* ptr = reinterpret_cast<void*>(frame.x[0]);
    if (ptr) std::free(ptr);
    frame.ret_x0 = 0;
}

void shim_posix_memalign(CallFrame& frame, void*) {
    // x0 = memptr, x1 = alignment, x2 = size
    void** memptr = reinterpret_cast<void**>(frame.x[0]);
    size_t align = static_cast<size_t>(frame.x[1]);
    size_t size = static_cast<size_t>(frame.x[2]);
    if (align < sizeof(void*)) align = sizeof(void*);
    void* ptr = nullptr;
#if defined(_WIN32)
    ptr = _aligned_malloc(size ? size : 16, align);
#else
    posix_memalign(&ptr, align, size ? size : 16);
#endif
    if (memptr) *memptr = ptr;
    frame.ret_x0 = 0;
}

void shim_strlen(CallFrame& frame, void*) {
    const char* str = reinterpret_cast<const char*>(frame.x[0]);
    frame.ret_x0 = str ? std::strlen(str) : 0;
}

void shim_memcpy(CallFrame& frame, void*) {
    void* dst = reinterpret_cast<void*>(frame.x[0]);
    const void* src = reinterpret_cast<const void*>(frame.x[1]);
    size_t n = static_cast<size_t>(frame.x[2]);
    if (dst && src && n) std::memcpy(dst, src, n);
    frame.ret_x0 = frame.x[0];
}

void shim_dispatch_semaphore_create(CallFrame& frame, void*) {
    // x0 = value
    // Return a dummy non-null handle
    static uint64_t sem_id = 0x1000;
    frame.ret_x0 = ++sem_id;
}

void shim_dispatch_semaphore_wait(CallFrame& frame, void*) {
    frame.ret_x0 = 0;
}

void shim_dispatch_semaphore_signal(CallFrame& frame, void*) {
    frame.ret_x0 = 0;
}

void shim_pthread_mutex_init(CallFrame& frame, void*) { frame.ret_x0 = 0; }
void shim_pthread_mutex_lock(CallFrame& frame, void*) { frame.ret_x0 = 0; }
void shim_pthread_mutex_unlock(CallFrame& frame, void*) { frame.ret_x0 = 0; }
void shim_pthread_key_create(CallFrame& frame, void*) { frame.ret_x0 = 0; }
void shim_pthread_cond_init(CallFrame& frame, void*) { frame.ret_x0 = 0; }
void shim_mach_timebase_info(CallFrame& frame, void*) {
    struct mach_timebase_info { uint32_t numer; uint32_t denom; };
    auto* info = reinterpret_cast<mach_timebase_info*>(frame.x[0]);
    if (info) { info->numer = 1; info->denom = 1; }
    frame.ret_x0 = 0;
}

// Minimal Objective-C shims
void shim_objc_autoreleasePoolPush(CallFrame& frame, void*) { frame.ret_x0 = 0x1000; }
void shim_objc_autoreleasePoolPop(CallFrame& frame, void*) { frame.ret_x0 = 0; }
void shim_objc_retain(CallFrame& frame, void*) { frame.ret_x0 = frame.x[0]; }
void shim_objc_release(CallFrame& frame, void*) { frame.ret_x0 = 0; }

void shim_UIApplicationMain(CallFrame& frame, void*) {
    std::cout << "[Shim] _UIApplicationMain called! argc=" << frame.x[0] << std::endl;
    frame.ret_x0 = 0;
}

} // namespace

ShimRegistry& ShimRegistry::instance() {
    static ShimRegistry reg;
    return reg;
}

void ShimRegistry::register_shim(const std::string& symbol, ShimHandler handler) {
    handlers_[symbol] = handler;
}

ShimHandler ShimRegistry::find_shim(const std::string& symbol) const {
    auto it = handlers_.find(symbol);
    return it != handlers_.end() ? it->second : nullptr;
}

bool ShimRegistry::dispatch(const std::string& symbol, CallFrame& frame, void* context) {
    auto handler = find_shim(symbol);
    if (handler) {
        handler(frame, context);
        return true;
    }
    return false;
}

void ShimRegistry::init_standard_shims() {
    register_shim("___cxa_atexit", shim_cxa_atexit);
    register_shim("___cxa_guard_acquire", shim_cxa_guard_acquire);
    register_shim("___cxa_guard_release", shim_cxa_guard_release);
    register_shim("_malloc", shim_malloc);
    register_shim("_free", shim_free);
    register_shim("_posix_memalign", shim_posix_memalign);
    register_shim("_strlen", shim_strlen);
    register_shim("_memcpy", shim_memcpy);
    register_shim("_dispatch_semaphore_create", shim_dispatch_semaphore_create);
    register_shim("_dispatch_semaphore_wait", shim_dispatch_semaphore_wait);
    register_shim("_dispatch_semaphore_signal", shim_dispatch_semaphore_signal);
    register_shim("_pthread_mutex_init", shim_pthread_mutex_init);
    register_shim("_pthread_mutex_lock", shim_pthread_mutex_lock);
    register_shim("_pthread_mutex_unlock", shim_pthread_mutex_unlock);
    register_shim("_pthread_key_create", shim_pthread_key_create);
    register_shim("_pthread_cond_init", shim_pthread_cond_init);
    register_shim("_mach_timebase_info", shim_mach_timebase_info);
    register_shim("_objc_autoreleasePoolPush", shim_objc_autoreleasePoolPush);
    register_shim("_objc_autoreleasePoolPop", shim_objc_autoreleasePoolPop);
    register_shim("_objc_retain", shim_objc_retain);
    register_shim("_objc_retainAutoreleasedReturnValue", shim_objc_retain);
    register_shim("_objc_autoreleaseReturnValue", shim_objc_retain);
    register_shim("_objc_release", shim_objc_release);
    register_shim("_UIApplicationMain", shim_UIApplicationMain);
}

} // namespace loader
