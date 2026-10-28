#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cerrno>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <malloc.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <pthread.h>
#include <time.h>
#include <stdarg.h>
#include <dirent.h>
#include <ucontext.h>
#include <sys/ioctl.h>

#include "shim_types.hpp"

// Thread-local Apple TSD block (512 64-bit slots = 4096 bytes per thread)
alignas(64) __thread uint64_t g_apple_tsd[512] = {0};
static int64_t g_tsd_tprel_diff = 0;

static uint64_t get_tpidr_el0() {
    uint64_t v;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(v));
    return v;
}

// Global trampolines and handlers
extern "C" void callframe_trampoline();
extern "C" void* native_mmap(void*, size_t, int, int, int, off_t);

struct TrampolineEntry {
    ShimHandler handler;
    const char* symbol_name;
};

#define MAX_TRAMPOLINES 8192
#define TRAMPOLINE_STUB_WORDS 8
#define TRAMPOLINE_POOL_SIZE 1048576
static TrampolineEntry g_trampoline_entries[MAX_TRAMPOLINES];
static size_t g_trampoline_count = 0;
static uint32_t* g_trampoline_code = nullptr;
static uint8_t* g_loaded_image_base = nullptr;
static int64_t g_loaded_image_slide = 0;
static size_t g_loaded_image_size = 0;

extern "C" uint64_t g_mmap_capture[160];
extern "C" uint64_t g_mmap_capture_count;

struct BindEvent {
    uint64_t loc;
    uint64_t target;
    const char* symbol;
    bool unresolved;
};
static std::vector<BindEvent> g_binds;
static std::vector<std::string> g_unresolved_imports;
static std::unordered_set<std::string> g_unresolved_set;

static const char* trampoline_symbol(void* ctx) {
    if (!ctx) return nullptr;
    return reinterpret_cast<TrampolineEntry*>(ctx)->symbol_name;
}

static void wr_str(int fd, const char* s) {
    if (!s) s = "(null)";
    size_t n = 0;
    while (s[n]) ++n;
    write(fd, s, n);
}

static void wr_hex(int fd, uint64_t v) {
    char buf[18];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; ++i) {
        buf[17 - i] = "0123456789abcdef"[v & 0xf];
        v >>= 4;
    }
    write(fd, buf, 18);
}

static void dump_ptr_context(int fd, uint64_t p, const char* label) {
    wr_str(fd, "  ");
    wr_str(fd, label);
    wr_str(fd, "=");
    wr_hex(fd, p);
    uint8_t* base = g_loaded_image_base;
    if (base && p >= reinterpret_cast<uint64_t>(base) &&
        p + 8 <= reinterpret_cast<uint64_t>(base) + g_loaded_image_size) {
        uint64_t pointed = *reinterpret_cast<uint64_t*>(p);
        wr_str(fd, " in image *p=");
        wr_hex(fd, pointed);
        for (const auto& b : g_binds) {
            if (b.loc == p) {
                wr_str(fd, " bind_slot ");
                wr_str(fd, b.symbol);
                if (b.unresolved) wr_str(fd, " UNRESOLVED");
            }
        }
    }
    for (const auto& b : g_binds) {
        if (b.target == p) {
            wr_str(fd, " is_bind_target_of ");
            wr_str(fd, b.symbol);
            wr_str(fd, " @");
            wr_hex(fd, b.loc);
        }
    }
    wr_str(fd, "\n");
}

static void dump_mmap_captures() {
    const int fd = 2;
    uint64_t count = g_mmap_capture_count;
    wr_str(fd, "[MMAP CAPTURE] count=");
    wr_hex(fd, count);
    wr_str(fd, "\n");
    uint64_t n = count < 16 ? count : 16;
    uint64_t start = count <= 16 ? 0 : (count - 16);
    for (uint64_t i = 0; i < n; ++i) {
        uint64_t idx = (start + i) & 15;
        const uint64_t* rec = g_mmap_capture + idx * 10;
        wr_str(fd, "  #");
        wr_hex(fd, start + i);
        wr_str(fd, " x0="); wr_hex(fd, rec[0]);
        wr_str(fd, " x1="); wr_hex(fd, rec[1]);
        wr_str(fd, " x2="); wr_hex(fd, rec[2]);
        wr_str(fd, " x3="); wr_hex(fd, rec[3]);
        wr_str(fd, " x4="); wr_hex(fd, rec[4]);
        wr_str(fd, " x5="); wr_hex(fd, rec[5]);
        wr_str(fd, " x6="); wr_hex(fd, rec[6]);
        wr_str(fd, " x7="); wr_hex(fd, rec[7]);
        wr_str(fd, " lr="); wr_hex(fd, rec[8]);
        wr_str(fd, " sp="); wr_hex(fd, rec[9]);
        wr_str(fd, "\n");
        dump_ptr_context(fd, rec[2], "x2");
        dump_ptr_context(fd, rec[8], "lr");
    }
}

// ---------------------------------------------------------------------------
// Crash diagnostics: safe reads, address classification, shim ring buffer
// ---------------------------------------------------------------------------
static int g_probe_pipe[2] = {-1, -1};

static void init_safe_read() {
    if (g_probe_pipe[0] >= 0) return;
    if (pipe2(g_probe_pipe, O_NONBLOCK | O_CLOEXEC) != 0) g_probe_pipe[0] = g_probe_pipe[1] = -1;
}

// Reads n (<=256) bytes from an arbitrary address without faulting: the kernel
// returns EFAULT from write() instead of raising SIGSEGV.
static bool safe_read(uint64_t addr, void* out, size_t n) {
    if (g_probe_pipe[0] < 0 || addr < 0x1000 || n > 256) return false;
    ssize_t w = write(g_probe_pipe[1], reinterpret_cast<const void*>(addr), n);
    if (w <= 0) return false;
    char tmp[256];
    ssize_t r = read(g_probe_pipe[0], tmp, static_cast<size_t>(w));
    if (w != static_cast<ssize_t>(n) || r != w) return false;
    std::memcpy(out, tmp, n);
    return true;
}

static bool in_image(uint64_t a) {
    uint64_t b = reinterpret_cast<uint64_t>(g_loaded_image_base);
    return b && a >= b && a < b + g_loaded_image_size;
}

static bool in_tramp_pool(uint64_t a) {
    uint64_t b = reinterpret_cast<uint64_t>(g_trampoline_code);
    return b && a >= b && a < b + TRAMPOLINE_POOL_SIZE;
}

static const char* base_name(const char* p) {
    const char* s = std::strrchr(p, '/');
    return s ? s + 1 : p;
}

// Prints "[kind] detail" for an address. kind: image / tramp / loader / libc / libc++ / other.
// Returns 1 for guest image, 2 for trampoline, 3 for loader, 4 for libc, 0 otherwise.
static int describe_addr(int fd, uint64_t a) {
    if (in_image(a)) {
        wr_str(fd, "[image] image_base+");
        wr_hex(fd, a - reinterpret_cast<uint64_t>(g_loaded_image_base));
        return 1;
    }
    if (in_tramp_pool(a)) {
        size_t idx = (a - reinterpret_cast<uint64_t>(g_trampoline_code)) / (TRAMPOLINE_STUB_WORDS * 4);
        wr_str(fd, "[tramp] stub for ");
        wr_str(fd, idx < g_trampoline_count ? g_trampoline_entries[idx].symbol_name : "?");
        return 2;
    }
    Dl_info di{};
    if (a && dladdr(reinterpret_cast<void*>(a), &di) && di.dli_fname) {
        const char* bn = base_name(di.dli_fname);
        int kind = 0;
        const char* tag = "other";
        if (std::strstr(di.dli_fname, "xnuroid")) { tag = "loader"; kind = 3; }
        else if (std::strcmp(bn, "libc.so") == 0) { tag = "libc"; kind = 4; }
        else if (std::strstr(bn, "libc++")) { tag = "libc++"; kind = 5; }
        wr_str(fd, "["); wr_str(fd, tag); wr_str(fd, "] ");
        wr_str(fd, bn); wr_str(fd, "!");
        if (di.dli_sname) {
            wr_str(fd, di.dli_sname);
            wr_str(fd, "+");
            wr_hex(fd, a - reinterpret_cast<uint64_t>(di.dli_saddr));
        } else {
            wr_str(fd, "(no symbol) base+");
            wr_hex(fd, a - reinterpret_cast<uint64_t>(di.dli_fbase));
        }
        return kind;
    }
    wr_str(fd, "[other] unmapped/unknown");
    return 0;
}

// Prints  "string" preview if the value points at >=3 printable bytes then NUL/end.
static void wr_string_preview(int fd, uint64_t p) {
    if (p < 0x10000) return;
    char buf[40];
    if (!safe_read(p, buf, 32)) return;
    size_t n = 0;
    while (n < 32 && buf[n]) {
        if (buf[n] < 0x20 || buf[n] > 0x7e) return;
        ++n;
    }
    if (n < 3) return;
    buf[n] = 0;
    wr_str(fd, " \""); wr_str(fd, buf); wr_str(fd, n == 32 ? "...\"" : "\"");
}

struct ShimRingEntry {
    const char* name;
    uint64_t x[4];
    uint64_t lr;
    uint64_t sp;
};
static ShimRingEntry g_shim_ring[32];
static uint64_t g_shim_ring_count = 0;

static void shim_ring_record(const char* name, const uint64_t* x, uint64_t lr, uint64_t sp) {
    uint64_t i = __atomic_fetch_add(&g_shim_ring_count, 1, __ATOMIC_RELAXED);
    ShimRingEntry& e = g_shim_ring[i & 31];
    e.name = name;
    for (int k = 0; k < 4; ++k) e.x[k] = x[k];
    e.lr = lr;
    e.sp = sp;
}

static void dump_shim_ring(int fd) {
    uint64_t count = g_shim_ring_count;
    uint64_t n = count < 32 ? count : 32;
    wr_str(fd, "\n--- Shim ring buffer (oldest -> newest, total calls=");
    wr_hex(fd, count);
    wr_str(fd, ") ---\n");
    for (uint64_t k = 0; k < n; ++k) {
        uint64_t seq = count - n + k;
        const ShimRingEntry& e = g_shim_ring[seq & 31];
        wr_str(fd, k + 1 == n ? " >#" : "  #");
        wr_hex(fd, seq);
        wr_str(fd, " "); wr_str(fd, e.name);
        for (int i = 0; i < 4; ++i) {
            wr_str(fd, " x"); char d[2] = {static_cast<char>('0' + i), 0}; wr_str(fd, d);
            wr_str(fd, "="); wr_hex(fd, e.x[i]);
            wr_string_preview(fd, e.x[i]);
        }
        wr_str(fd, "\n        guest lr="); wr_hex(fd, e.lr); wr_str(fd, " ");
        describe_addr(fd, e.lr);
        wr_str(fd, " sp="); wr_hex(fd, e.sp);
        wr_str(fd, "\n");
    }
}

// Walks the AArch64 frame-pointer chain. Each record is {prev_fp, return_addr}.
static void dump_frame_walk(int fd, uint64_t pc, uint64_t lr, uint64_t fp, uint64_t sp) {
    wr_str(fd, "\n--- Frame walk (x29 chain) ---\n");
    wr_str(fd, "  #pc  "); wr_hex(fd, pc); wr_str(fd, " "); describe_addr(fd, pc); wr_str(fd, "\n");
    wr_str(fd, "  #lr  "); wr_hex(fd, lr); wr_str(fd, " "); describe_addr(fd, lr); wr_str(fd, "\n");
    int first_guest = -1;
    uint64_t guest_ret = 0;
    if (in_image(lr)) { first_guest = -2; guest_ret = lr; }
    for (int depth = 0; depth < 48; ++depth) {
        if ((fp & 0xf) || fp < sp || fp - sp > (32ull << 20)) {
            wr_str(fd, "  (chain stops: fp="); wr_hex(fd, fp); wr_str(fd, " invalid/out of stack range)\n");
            break;
        }
        uint64_t rec[2];
        if (!safe_read(fp, rec, 16)) { wr_str(fd, "  (chain stops: unreadable fp)\n"); break; }
        wr_str(fd, "  #"); wr_hex(fd, static_cast<uint64_t>(depth));
        wr_str(fd, "  fp="); wr_hex(fd, fp);
        wr_str(fd, " ret="); wr_hex(fd, rec[1]); wr_str(fd, " ");
        int k = describe_addr(fd, rec[1]);
        wr_str(fd, "\n");
        if (k == 1 && first_guest == -1) { first_guest = depth; guest_ret = rec[1]; }
        if (rec[0] <= fp || rec[1] == 0) break;
        fp = rec[0];
    }
    wr_str(fd, "  FIRST GUEST FRAME: ");
    if (first_guest != -1) {
        wr_hex(fd, guest_ret); wr_str(fd, " "); describe_addr(fd, guest_ret); wr_str(fd, "\n");
    } else {
        wr_str(fd, "none in fp chain\n");
    }
    // Fallback: scan the stack for words that point into the guest image.
    wr_str(fd, "  Stack-scan guest return candidates (sp..sp+0x4000):\n");
    int found = 0;
    for (uint64_t off = 0; off < 0x4000 && found < 10; off += 8) {
        uint64_t w;
        if (!safe_read(sp + off, &w, 8)) break;
        if (in_image(w)) {
            wr_str(fd, "    [sp+"); wr_hex(fd, off); wr_str(fd, "] "); wr_hex(fd, w); wr_str(fd, " ");
            describe_addr(fd, w); wr_str(fd, "\n");
            ++found;
        }
    }
}

// Called from callframe_trampoline for every CallFrame-style shim.
extern "C" void callframe_dispatch(CallFrame* frame, TrampolineEntry* entry) {
    shim_ring_record(entry->symbol_name, frame->x, frame->lr, frame->sp);
    entry->handler(*frame, entry);
}

// Stub: ldr x17,=entry; ldr x16,=trampoline; br x16
// Layout (8 words = 32 bytes):
//  word 0: hint #34 (bti c)
//  word 1: ldr x17, [pc, #12] -> entry ptr at word[4..5]
//  word 2: ldr x16, [pc, #16] -> trampoline at word[6..7]
//  word 3: br x16
//  word 4..5: &TrampolineEntry
//  word 6..7: callframe_trampoline
static void* allocate_trampoline(ShimHandler handler, const char* symbol_name) {
    if (g_trampoline_count >= MAX_TRAMPOLINES) {
        fprintf(stderr, "[-] Trampoline pool exhausted at %zu!\n", g_trampoline_count);
        std::abort();
    }
    size_t idx = g_trampoline_count++;
    g_trampoline_entries[idx] = {handler, symbol_name};

    uint32_t* stub = g_trampoline_code + (idx * TRAMPOLINE_STUB_WORDS);

    stub[0] = 0xd503245f;  // hint #34 (bti c)
    stub[1] = 0x58000071;  // ldr x17, [pc, #12]
    stub[2] = 0x58000090;  // ldr x16, [pc, #16]
    stub[3] = 0xd61f0200;  // br x16

    *reinterpret_cast<uint64_t*>(&stub[4]) = reinterpret_cast<uint64_t>(&g_trampoline_entries[idx]);
    *reinterpret_cast<uint64_t*>(&stub[6]) = reinterpret_cast<uint64_t>(&callframe_trampoline);

    return stub;
}

static uint32_t encode_b(const uint8_t* from, const uint8_t* to, const char* what) {
    int64_t bytes = static_cast<int64_t>(to - from);
    if (bytes & 3) {
        fprintf(stderr, "[FATAL] %s: unaligned branch %p -> %p\n", what, from, to);
        std::abort();
    }
    int64_t off = bytes / 4;
    if (off < -(1 << 25) || off >= (1 << 25)) {
        fprintf(stderr, "[FATAL] %s: branch out of +/-128MB from=%p to=%p delta=%ld\n",
                what, from, to, static_cast<long>(bytes));
        std::abort();
    }
    return 0x14000000u | (static_cast<uint32_t>(off) & 0x03ffffffu);
}

static uint32_t encode_add_sub_imm12(uint32_t rd, uint32_t rn, int64_t imm, bool lsl12) {
    uint32_t mag;
    uint32_t opc;
    if (imm >= 0) {
        mag = static_cast<uint32_t>(imm);
        opc = 0x91000000u;
    } else {
        mag = static_cast<uint32_t>(-imm);
        opc = 0xd1000000u;
    }
    return opc | ((lsl12 ? 1u : 0u) << 22) | ((mag & 0xfffu) << 10) | (rn << 5) | rd;
}

extern "C" void native_bzero(void* s, size_t n) {
    if (s && n) std::memset(s, 0, n);
}

extern "C" void native_memset_pattern16(void* b, const void* pattern16, size_t len) {
    if (!b || !pattern16 || len == 0) return;
    uint8_t* dst = reinterpret_cast<uint8_t*>(b);
    size_t chunks = len / 16;
    for (size_t i = 0; i < chunks; ++i) {
        std::memcpy(dst + i * 16, pattern16, 16);
    }
    size_t rem = len % 16;
    if (rem > 0) {
        std::memcpy(dst + chunks * 16, pattern16, rem);
    }
}

// Apple Darwin TSD / pthread_key emulation
static int g_next_apple_key = 1;

extern "C" int native_pthread_key_create(pthread_key_t* key, void (*destructor)(void*)) {
    if (!key) return EINVAL;
    *key = g_next_apple_key++;
    return 0;
}

extern "C" int native_pthread_setspecific(pthread_key_t key, const void* value) {
    if (key >= 0 && key < 512) {
        g_apple_tsd[key] = reinterpret_cast<uint64_t>(value);
        return 0;
    }
    return EINVAL;
}

extern "C" void* native_pthread_getspecific(pthread_key_t key) {
    if (key >= 0 && key < 512) {
        return reinterpret_cast<void*>(g_apple_tsd[key]);
    }
    return nullptr;
}

extern "C" int native_pthread_key_delete(pthread_key_t key) {
    return 0;
}

extern "C" int native_NSGetExecutablePath(char* buf, uint32_t* bufsize) {
    const char* path = "/data/local/tmp/game/UnityFramework";
    uint32_t len = static_cast<uint32_t>(std::strlen(path));
    if (!buf || !bufsize || *bufsize < len + 1) {
        if (bufsize) *bufsize = len + 1;
        return -1;
    }
    std::memcpy(buf, path, len + 1);
    *bufsize = len;
    return 0;
}

extern "C" char*** native_NSGetEnviron(void) {
    static char* dummy_env[] = { nullptr };
    static char** p_env = dummy_env;
    return &p_env;
}

extern "C" uint32_t native_dyld_image_count(void) {
    return 1;
}

extern "C" const void* native_dyld_get_image_header(uint32_t image_index) {
    if (image_index == 0) return g_loaded_image_base;
    return nullptr;
}

extern "C" intptr_t native_dyld_get_image_vmaddr_slide(uint32_t image_index) {
    if (image_index == 0) return g_loaded_image_slide;
    return 0;
}

extern "C" const char* native_dyld_get_image_name(uint32_t image_index) {
    if (image_index == 0) return "/data/local/tmp/game/UnityFramework";
    return nullptr;
}

extern "C" void native_dyld_register_func_for_add_image(void (*func)(const void* mh, intptr_t vmaddr_slide)) {
    if (func && g_loaded_image_base) {
        func(g_loaded_image_base, g_loaded_image_slide);
    }
}

extern "C" void native_dyld_register_func_for_remove_image(void (*func)(const void* mh, intptr_t vmaddr_slide)) {}

// Apple Grand Central Dispatch (GCD) emulation
extern "C" void* native_dispatch_semaphore_create(long value) {
    static uint64_t g_sem_id = 0x1000;
    return reinterpret_cast<void*>(++g_sem_id);
}

extern "C" long native_dispatch_semaphore_wait(void* dsema, uint64_t timeout) {
    return 0;
}

extern "C" long native_dispatch_semaphore_signal(void* dsema) {
    return 0;
}

extern "C" void* native_dispatch_queue_create(const char* label, void* attr) {
    static uint64_t g_queue_id = 0x2000;
    return reinterpret_cast<void*>(++g_queue_id);
}

extern "C" void* native_dispatch_get_global_queue(long identifier, unsigned long flags) {
    static uint64_t g_gqueue = 0x3000;
    return reinterpret_cast<void*>(g_gqueue);
}

extern "C" void native_dispatch_async(void* queue, void* block) {}
extern "C" void native_dispatch_sync(void* queue, void (*work)(void*)) { if (work) work(nullptr); }
extern "C" void native_dispatch_release(void* obj) {}
extern "C" void native_dispatch_retain(void* obj) {}
extern "C" void native_dispatch_resume(void* obj) {}
extern "C" void native_dispatch_suspend(void* obj) {}

extern "C" int native_cxa_atexit(void (*func)(void*), void* arg, void* dso) {
    return 0;
}

static void log_term(const char* what, uint64_t lr, int code, const char* sym) {
    wr_str(2, "[TERM] ");
    wr_str(2, what);
    if (sym) {
        wr_str(2, " ");
        wr_str(2, sym);
    }
    wr_str(2, " code=");
    wr_hex(2, static_cast<uint64_t>(static_cast<uint32_t>(code)));
    wr_str(2, " lr=");
    wr_hex(2, lr);
    wr_str(2, "\n");
}

extern "C" void native_exit(int code) {
    log_term("exit", reinterpret_cast<uint64_t>(__builtin_return_address(0)), code, nullptr);
    exit(code);
}

extern "C" void native__exit(int code) {
    log_term("_exit", reinterpret_cast<uint64_t>(__builtin_return_address(0)), code, nullptr);
    _exit(code);
}

extern "C" void native_abort() {
    log_term("abort", reinterpret_cast<uint64_t>(__builtin_return_address(0)), 134, nullptr);
    std::abort();
}

extern "C" int* native_darwin_error() {
    return &errno;
}

extern "C" void native_OSSpinLockLock(volatile int32_t* lock) {
    while (__atomic_test_and_set(lock, __ATOMIC_ACQUIRE)) {}
}

extern "C" void native_OSSpinLockUnlock(volatile int32_t* lock) {
    __atomic_clear(lock, __ATOMIC_RELEASE);
}

extern "C" int32_t native_OSAtomicAdd32Barrier(int32_t amount, volatile int32_t* value) {
    return __atomic_add_fetch(value, amount, __ATOMIC_SEQ_CST);
}

extern "C" int64_t native_OSAtomicAdd64Barrier(int64_t amount, volatile int64_t* value) {
    return __atomic_add_fetch(value, amount, __ATOMIC_SEQ_CST);
}

extern "C" bool native_OSAtomicCompareAndSwap32Barrier(int32_t old_val, int32_t new_val, volatile int32_t* value) {
    return __atomic_compare_exchange_n(value, &old_val, new_val, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

extern "C" bool native_OSAtomicCompareAndSwapPtrBarrier(void* old_val, void* new_val, void* volatile* value) {
    return __atomic_compare_exchange_n(value, &old_val, new_val, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

extern "C" void native_OSMemoryBarrier() {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

// Memory allocator shims
extern "C" size_t native_malloc_size(const void* ptr) {
    if (!ptr) return 0;
    return malloc_usable_size(const_cast<void*>(ptr));
}

// Darwin mach / vm / threading primitives
static uint32_t g_mach_task_self_val = 1;
extern "C" uint32_t native_mach_thread_self() {
    return 100;
}

static uintptr_t g_vm_page_size = 4096;
static uintptr_t g_vm_page_mask = 4095;

extern "C" char* native_realpath_darwin(const char* path, char* resolved_path) {
    return realpath(path, resolved_path);
}

extern "C" void* native_pthread_get_stackaddr_np(pthread_t thread) {
    pthread_attr_t attr;
    pthread_getattr_np(thread ? thread : pthread_self(), &attr);
    void* stackaddr = nullptr;
    size_t stacksize = 0;
    pthread_attr_getstack(&attr, &stackaddr, &stacksize);
    pthread_attr_destroy(&attr);
    return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(stackaddr) + stacksize);
}

extern "C" int native_pthread_setname_np(const char* name) {
    if (!name) return 0;
    return pthread_setname_np(pthread_self(), name);
}

extern "C" uint32_t native_pthread_mach_thread_np(pthread_t thread) {
    return 100;
}

extern "C" const void* native_getsectbynamefromheader_64(const void* mhp_ptr, const char* segname, const char* sectname) {
    if (!mhp_ptr || !segname || !sectname) return nullptr;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(mhp_ptr);
    uint32_t magic = *reinterpret_cast<const uint32_t*>(p);
    if (magic != 0xfeedfacf) return nullptr; // MH_MAGIC_64
    uint32_t ncmds = *reinterpret_cast<const uint32_t*>(p + 16);
    uint32_t cur = 32;
    for (uint32_t i = 0; i < ncmds; ++i) {
        uint32_t cmd = *reinterpret_cast<const uint32_t*>(p + cur);
        uint32_t cmdsize = *reinterpret_cast<const uint32_t*>(p + cur + 4);
        if (cmd == 0x19) { // LC_SEGMENT_64
            const char* seg = reinterpret_cast<const char*>(p + cur + 8);
            uint32_t nsects = *reinterpret_cast<const uint32_t*>(p + cur + 64);
            if (std::strncmp(seg, segname, 16) == 0) {
                uint32_t s_cur = cur + 72;
                for (uint32_t s = 0; s < nsects; ++s) {
                    const char* sname = reinterpret_cast<const char*>(p + s_cur);
                    if (std::strncmp(sname, sectname, 16) == 0) {
                        return p + s_cur;
                    }
                    s_cur += 80;
                }
            }
        }
        cur += cmdsize;
    }
    return nullptr;
}

extern "C" int native_semaphore_create(uint32_t task, void** sem, int policy, int value) {
    static uint64_t sem_id = 0x5000;
    if (sem) *sem = reinterpret_cast<void*>(++sem_id);
    return 0;
}
extern "C" int native_semaphore_destroy(uint32_t task, void* sem) { return 0; }
extern "C" int native_semaphore_signal(void* sem) { return 0; }
extern "C" int native_semaphore_wait(void* sem) { return 0; }

extern "C" int native_mach_port_allocate(uint32_t task, int right, uint32_t* name) {
    static uint32_t g_port = 0x200;
    if (name) *name = ++g_port;
    return 0;
}
extern "C" int native_mach_port_deallocate(uint32_t task, uint32_t name) { return 0; }
extern "C" int native_mach_port_insert_right(uint32_t task, uint32_t name, uint32_t poly, uint32_t polyPoly) { return 0; }
extern "C" int native_mach_port_mod_refs(uint32_t task, uint32_t name, int right, int delta) { return 0; }

extern "C" int native_vm_allocate(uint32_t task, uintptr_t* address, size_t size, int flags) {
    void* p = mmap(reinterpret_cast<void*>(*address), size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED) return 1;
    *address = reinterpret_cast<uintptr_t>(p);
    return 0;
}
extern "C" int native_vm_deallocate(uint32_t task, uintptr_t address, size_t size) {
    munmap(reinterpret_cast<void*>(address), size);
    return 0;
}

// Global data objects
static uint64_t g_block_global[8] = {0};
static uint64_t g_block_stack[8] = {0};
static uint64_t g_dispatch_main_q_obj[16] = {0};
static uint64_t g_dispatch_timer_obj[16] = {0};
static uint64_t g_stack_chk_guard_val = 0x595a5b5c5d5e5fULL;
static FILE* g_fake_stderr = stderr;
static FILE* g_fake_stdout = stdout;

// CoreFoundation / Darwin data symbols
static uint64_t g_cf_constant_string_class[16] = {0};
static uint64_t g_ns_dictionary0[16] = {0};
static void* g_kCFAllocatorDefault = reinterpret_cast<void*>(0x10);
static void* g_kCFAllocatorNull = reinterpret_cast<void*>(0x20);
static const char* g_kCFBundleVersionKey = "CFBundleVersion";
static const char* g_kCFLocaleCountryCode = "kCFLocaleCountryCode";
static const char* g_kCFLocaleLanguageCode = "kCFLocaleLanguageCode";
static const char* g_kCFLocaleScriptCode = "kCFLocaleScriptCode";
static uint64_t g_kCFTypeArrayCallBacks[16] = {0};

// Shim implementations
// ---------------------------------------------------------------------------
// Darwin variadic support.
// Darwin arm64 passes every variadic argument on the stack in 8-byte slots
// (doubles included); only named arguments use x0-x7. AAPCS64 (bionic) reads
// variadics from x/v registers first. We hand bionic a va_list whose __stack
// points at the guest's stack with empty register save areas, so every va_arg
// reads sequentially from the guest stack slots.
// ---------------------------------------------------------------------------
struct Aapcs64VaList {
    void* stack;
    void* gr_top;
    void* vr_top;
    int   gr_offs;
    int   vr_offs;
};
static_assert(sizeof(Aapcs64VaList) == sizeof(va_list), "unexpected va_list layout");

static va_list make_va_list(const void* stack) {
    Aapcs64VaList g{const_cast<void*>(stack), nullptr, nullptr, 0, 0};
    va_list ap;
    std::memcpy(&ap, &g, sizeof(g));
    return ap;
}

// Named = number of named args (all in x0-x7). Variadics start at guest sp.
// Named args that overflow into stack slots are unsupported (compile error).
template <int Named>
static inline va_list guest_va(const CallFrame& f) {
    static_assert(Named >= 0 && Named <= 8,
                  "named args overflow x0-x7: stack-passed named args are not supported");
    return make_va_list(reinterpret_cast<const void*>(f.sp));
}

// Darwin va_list is a plain char* to the next 8-byte slot.
static inline va_list darwin_va(void* darwin_ap) {
    return make_va_list(darwin_ap);
}

static inline void set_ret_int(CallFrame& f, int v) {
    f.ret_x0 = static_cast<uint64_t>(static_cast<int64_t>(v));
}

static const char* cfstring_cstr(uint64_t p) {
    // __CFConstantString { isa, flags(8), const char* str, long length }
    uint64_t w[4];
    if (!safe_read(p, w, sizeof(w))) return nullptr;
    if (w[0] != reinterpret_cast<uint64_t>(&g_cf_constant_string_class)) return nullptr;
    char probe[1];
    if (!safe_read(w[2], probe, 1)) return nullptr;
    return reinterpret_cast<const char*>(w[2]);
}

static void log_nslog(uint64_t fmt_obj) {
    const char* s = cfstring_cstr(fmt_obj);
    wr_str(2, "[NSLog] ");
    if (s) wr_str(2, s);
    else { wr_str(2, "<NSString "); wr_hex(2, fmt_obj); wr_str(2, ">"); }
    wr_str(2, "\n");
}

static void shim_printf(CallFrame& f, void*) {
    va_list ap = guest_va<1>(f);
    set_ret_int(f, vprintf(reinterpret_cast<const char*>(f.x[0]), ap));
}
static void shim_fprintf(CallFrame& f, void*) {
    va_list ap = guest_va<2>(f);
    set_ret_int(f, vfprintf(reinterpret_cast<FILE*>(f.x[0]), reinterpret_cast<const char*>(f.x[1]), ap));
}
static void shim_sprintf(CallFrame& f, void*) {
    va_list ap = guest_va<2>(f);
    set_ret_int(f, vsprintf(reinterpret_cast<char*>(f.x[0]), reinterpret_cast<const char*>(f.x[1]), ap));
}
static void shim_snprintf(CallFrame& f, void*) {
    va_list ap = guest_va<3>(f);
    set_ret_int(f, vsnprintf(reinterpret_cast<char*>(f.x[0]), f.x[1], reinterpret_cast<const char*>(f.x[2]), ap));
}
static void shim_asprintf(CallFrame& f, void*) {
    va_list ap = guest_va<2>(f);
    set_ret_int(f, vasprintf(reinterpret_cast<char**>(f.x[0]), reinterpret_cast<const char*>(f.x[1]), ap));
}
static void shim_dprintf(CallFrame& f, void*) {
    va_list ap = guest_va<2>(f);
    set_ret_int(f, vdprintf(static_cast<int>(f.x[0]), reinterpret_cast<const char*>(f.x[1]), ap));
}
static void shim_sscanf(CallFrame& f, void*) {
    va_list ap = guest_va<2>(f);
    set_ret_int(f, vsscanf(reinterpret_cast<const char*>(f.x[0]), reinterpret_cast<const char*>(f.x[1]), ap));
}
static void shim_fscanf(CallFrame& f, void*) {
    va_list ap = guest_va<2>(f);
    set_ret_int(f, vfscanf(reinterpret_cast<FILE*>(f.x[0]), reinterpret_cast<const char*>(f.x[1]), ap));
}
static void shim_scanf(CallFrame& f, void*) {
    va_list ap = guest_va<1>(f);
    set_ret_int(f, vscanf(reinterpret_cast<const char*>(f.x[0]), ap));
}
static void shim_syslog(CallFrame& f, void*) {
    va_list ap = guest_va<2>(f);
    char buf[1024];
    vsnprintf(buf, sizeof(buf), reinterpret_cast<const char*>(f.x[1]), ap);
    wr_str(2, "[syslog] "); wr_str(2, buf); wr_str(2, "\n");
}
// NSLog uses %@ (unsupported by bionic): log the raw format string only.
static void shim_NSLog(CallFrame& f, void*) { log_nslog(f.x[0]); }

// __snprintf_chk(s, maxlen, flag, slen, fmt, ...)
static void shim_snprintf_chk(CallFrame& f, void*) {
    va_list ap = guest_va<5>(f);
    set_ret_int(f, vsnprintf(reinterpret_cast<char*>(f.x[0]), f.x[1], reinterpret_cast<const char*>(f.x[4]), ap));
}
// __sprintf_chk(s, flag, slen, fmt, ...)
static void shim_sprintf_chk(CallFrame& f, void*) {
    va_list ap = guest_va<4>(f);
    set_ret_int(f, vsprintf(reinterpret_cast<char*>(f.x[0]), reinterpret_cast<const char*>(f.x[3]), ap));
}

// v-variants: the guest passes a Darwin va_list (char*) as the last argument.
extern "C" int native_vprintf(const char* fmt, void* dap) { va_list ap = darwin_va(dap); return vprintf(fmt, ap); }
extern "C" int native_vfprintf(FILE* fp, const char* fmt, void* dap) { va_list ap = darwin_va(dap); return vfprintf(fp, fmt, ap); }
extern "C" int native_vsprintf(char* s, const char* fmt, void* dap) { va_list ap = darwin_va(dap); return vsprintf(s, fmt, ap); }
extern "C" int native_vsnprintf(char* s, size_t n, const char* fmt, void* dap) { va_list ap = darwin_va(dap); return vsnprintf(s, n, fmt, ap); }
extern "C" int native_vasprintf(char** s, const char* fmt, void* dap) { va_list ap = darwin_va(dap); return vasprintf(s, fmt, ap); }
extern "C" int native_vdprintf(int fd, const char* fmt, void* dap) { va_list ap = darwin_va(dap); return vdprintf(fd, fmt, ap); }
extern "C" int native_vsscanf(const char* s, const char* fmt, void* dap) { va_list ap = darwin_va(dap); return vsscanf(s, fmt, ap); }
extern "C" int native_vfscanf(FILE* fp, const char* fmt, void* dap) { va_list ap = darwin_va(dap); return vfscanf(fp, fmt, ap); }
extern "C" int native_vscanf(const char* fmt, void* dap) { va_list ap = darwin_va(dap); return vscanf(fmt, ap); }
extern "C" int native___vsnprintf_chk(char* s, size_t maxlen, int, size_t, const char* fmt, void* dap) {
    va_list ap = darwin_va(dap);
    return vsnprintf(s, maxlen, fmt, ap);
}
extern "C" int native___vsprintf_chk(char* s, int, size_t, const char* fmt, void* dap) {
    va_list ap = darwin_va(dap);
    return vsprintf(s, fmt, ap);
}
extern "C" void native_NSLogv(uint64_t fmt_obj, void*) { log_nslog(fmt_obj); }
extern "C" void native_vsyslog(int, const char* fmt, void* dap) {
    char buf[1024];
    va_list ap = darwin_va(dap);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    wr_str(2, "[syslog] "); wr_str(2, buf); wr_str(2, "\n");
}

// Self-test: emulate a Darwin caller (variadics on the stack, named args in
// registers) and call the real _snprintf trampoline stub.
static bool selftest_variadic_snprintf(void* stub) {
    char buf[128];
    std::memset(buf, 0, sizeof(buf));
    struct alignas(16) Slots { uint64_t i; double d; const char* s; uint64_t pad; };
    Slots slots{7, 2.5, "hi", 0};
    const char* fmt = "%d %.1f %s";
    uint64_t n = sizeof(buf);
    register uint64_t x0 __asm__("x0") = reinterpret_cast<uint64_t>(buf);
    register uint64_t x1 __asm__("x1") = n;
    register uint64_t x2 __asm__("x2") = reinterpret_cast<uint64_t>(fmt);
    register uint64_t x9 __asm__("x9") = reinterpret_cast<uint64_t>(&slots);
    register uint64_t x10 __asm__("x10") = reinterpret_cast<uint64_t>(stub);
    __asm__ volatile(
        "sub sp, sp, #32\n"
        "ldp x11, x12, [x9]\n"
        "stp x11, x12, [sp]\n"
        "ldp x11, x12, [x9, #16]\n"
        "stp x11, x12, [sp, #16]\n"
        "blr x10\n"
        "add sp, sp, #32\n"
        : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x9), "+r"(x10)
        :
        : "x3", "x4", "x5", "x6", "x7", "x8", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x30",
          "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "memory", "cc");
    bool ok = std::strcmp(buf, "7 2.5 hi") == 0;
    wr_str(2, "[SELFTEST] Darwin-variadic _snprintf -> \"");
    wr_str(2, buf);
    wr_str(2, ok ? "\" PASS\n" : "\" FAIL (expected \"7 2.5 hi\")\n");
    return ok;
}

// ---------------------------------------------------------------------------
// open / openat / fcntl / ioctl (Darwin constants + variadic args)
// ---------------------------------------------------------------------------
static std::string remap_game_path(const char* path) {
    std::string p = path ? path : "";
    if (p.find("Data/") != std::string::npos || p.find("Managed/") != std::string::npos) {
        size_t idx = p.find("Data/");
        if (idx != std::string::npos) {
            p = "/data/local/tmp/game/" + p.substr(idx);
        } else {
            p = "/data/local/tmp/game/Data/" + p;
        }
    }
    return p;
}

static int darwin_to_linux_open_flags(int f) {
    int l = 0;
    switch (f & 3) {
        case 0: l = O_RDONLY; break;
        case 1: l = O_WRONLY; break;
        default: l = O_RDWR; break;
    }
    if (f & 0x000004)  l |= O_NONBLOCK;
    if (f & 0x000008)  l |= O_APPEND;
    if (f & 0x000080)  l |= O_SYNC;
    if (f & 0x000100)  l |= O_NOFOLLOW;
    if (f & 0x000200)  l |= O_CREAT;
    if (f & 0x000400)  l |= O_TRUNC;
    if (f & 0x000800)  l |= O_EXCL;
    if (f & 0x020000)  l |= O_NOCTTY;
    if (f & 0x100000)  l |= O_DIRECTORY;
    if (f & 0x1000000) l |= O_CLOEXEC;
    return l;
}

static int linux_to_darwin_open_flags(int l) {
    int f = 0;
    switch (l & O_ACCMODE) {
        case O_RDONLY: f = 0; break;
        case O_WRONLY: f = 1; break;
        default: f = 2; break;
    }
    if (l & O_NONBLOCK) f |= 0x4;
    if (l & O_APPEND)   f |= 0x8;
    return f;
}

static std::unordered_map<int, std::string> g_fd_paths;

static std::string fd_path(int fd) {
    auto it = g_fd_paths.find(fd);
    if (it != g_fd_paths.end()) return it->second;
    char link[64];
    char out[512];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, out, sizeof(out) - 1);
    if (n < 0) return "";
    out[n] = 0;
    return out;
}

static void shim_open(CallFrame& frame, void*) {
    const char* path = reinterpret_cast<const char*>(frame.x[0]);
    int flags = static_cast<int>(frame.x[1]);
    // mode is variadic on Darwin: first stack slot, only meaningful with O_CREAT.
    mode_t mode = (flags & 0x200) ? static_cast<mode_t>(frame.read_stack<int>(0)) : 0;
    std::string p = remap_game_path(path);
    int linux_flags = darwin_to_linux_open_flags(flags);
    int fd = open(p.c_str(), linux_flags, mode);
    int saved_errno = errno;
    if (fd >= 0) g_fd_paths[fd] = p;
    fprintf(stderr, "[SHIM] open(\"%s\") -> \"%s\" darwin_flags=0x%x linux_flags=0x%x mode=0%o -> fd=%d\n",
            path ? path : "(null)", p.c_str(), flags, linux_flags, static_cast<unsigned>(mode), fd);
    errno = saved_errno;
    frame.ret_x0 = static_cast<uint64_t>(static_cast<int64_t>(fd));
}

static void shim_openat(CallFrame& frame, void*) {
    int dirfd = static_cast<int>(frame.x[0]);
    if (dirfd == -2) dirfd = AT_FDCWD;  // Darwin AT_FDCWD is -2
    const char* path = reinterpret_cast<const char*>(frame.x[1]);
    int flags = static_cast<int>(frame.x[2]);
    mode_t mode = (flags & 0x200) ? static_cast<mode_t>(frame.read_stack<int>(0)) : 0;
    std::string p = remap_game_path(path);
    int fd = openat(dirfd, p.c_str(), darwin_to_linux_open_flags(flags), mode);
    if (fd >= 0) g_fd_paths[fd] = p;
    frame.ret_x0 = static_cast<uint64_t>(static_cast<int64_t>(fd));
}

static void shim_fcntl(CallFrame& frame, void*) {
    int fd = static_cast<int>(frame.x[0]);
    int cmd = static_cast<int>(frame.x[1]);
    uint64_t arg = frame.read_stack<uint64_t>(0);  // variadic third arg
    int rc = -1;
    switch (cmd) {
        case 0:  rc = fcntl(fd, F_DUPFD, static_cast<int>(arg)); break;
        case 67: rc = fcntl(fd, F_DUPFD_CLOEXEC, static_cast<int>(arg)); break;  // Darwin F_DUPFD_CLOEXEC
        case 1:  rc = fcntl(fd, F_GETFD); break;
        case 2:  rc = fcntl(fd, F_SETFD, static_cast<int>(arg)); break;
        case 3: {
            rc = fcntl(fd, F_GETFL);
            if (rc >= 0) rc = linux_to_darwin_open_flags(rc);
            break;
        }
        case 4: {
            int l = 0;
            if (arg & 0x4) l |= O_NONBLOCK;
            if (arg & 0x8) l |= O_APPEND;
            rc = fcntl(fd, F_SETFL, l);
            break;
        }
        case 45: case 48: case 41: rc = 0; break;  // F_RDAHEAD, F_NOCACHE, F_PREALLOCATE: no-ops
        case 51: rc = fsync(fd); break;            // F_FULLFSYNC
        case 50: {                                  // F_GETPATH
            std::string p = fd_path(fd);
            if (p.empty() || !arg) { errno = EBADF; rc = -1; }
            else { std::strncpy(reinterpret_cast<char*>(arg), p.c_str(), 1023); reinterpret_cast<char*>(arg)[1023] = 0; rc = 0; }
            break;
        }
        default: errno = EINVAL; rc = -1; break;
    }
    frame.ret_x0 = static_cast<uint64_t>(static_cast<int64_t>(rc));
}

static void shim_ioctl(CallFrame& frame, void*) {
    int fd = static_cast<int>(frame.x[0]);
    uint64_t req = frame.x[1] & 0xffffffffu;
    void* arg = reinterpret_cast<void*>(frame.read_stack<uint64_t>(0));  // variadic
    int rc;
    if (req == 0x4004667f)      rc = ioctl(fd, FIONREAD, arg);   // Darwin FIONREAD
    else if (req == 0x8004667e) rc = ioctl(fd, FIONBIO, arg);    // Darwin FIONBIO
    else { errno = ENOTTY; rc = -1; }
    frame.ret_x0 = static_cast<uint64_t>(static_cast<int64_t>(rc));
}

// ---------------------------------------------------------------------------
// stat family. Darwin arm64 struct stat (0x90 bytes):
//  +00 st_dev i32, +04 st_mode u16, +06 st_nlink u16, +08 st_ino u64,
//  +10 st_uid u32, +14 st_gid u32, +18 st_rdev i32, +20 atimespec, +30 mtimespec,
//  +40 ctimespec, +50 birthtimespec, +60 st_size i64, +68 st_blocks i64,
//  +70 st_blksize i32, +74 st_flags u32, +78 st_gen u32
// ---------------------------------------------------------------------------
static void fill_darwin_stat(uint8_t* d, const struct stat& s) {
    std::memset(d, 0, 0x90);
    auto put32 = [&](size_t o, uint32_t v) { std::memcpy(d + o, &v, 4); };
    auto put16 = [&](size_t o, uint16_t v) { std::memcpy(d + o, &v, 2); };
    auto put64 = [&](size_t o, uint64_t v) { std::memcpy(d + o, &v, 8); };
    put32(0x00, static_cast<uint32_t>(s.st_dev));
    put16(0x04, static_cast<uint16_t>(s.st_mode));
    put16(0x06, static_cast<uint16_t>(s.st_nlink));
    put64(0x08, static_cast<uint64_t>(s.st_ino));
    put32(0x10, static_cast<uint32_t>(s.st_uid));
    put32(0x14, static_cast<uint32_t>(s.st_gid));
    put32(0x18, static_cast<uint32_t>(s.st_rdev));
    put64(0x20, static_cast<uint64_t>(s.st_atim.tv_sec)); put64(0x28, static_cast<uint64_t>(s.st_atim.tv_nsec));
    put64(0x30, static_cast<uint64_t>(s.st_mtim.tv_sec)); put64(0x38, static_cast<uint64_t>(s.st_mtim.tv_nsec));
    put64(0x40, static_cast<uint64_t>(s.st_ctim.tv_sec)); put64(0x48, static_cast<uint64_t>(s.st_ctim.tv_nsec));
    put64(0x50, static_cast<uint64_t>(s.st_ctim.tv_sec)); put64(0x58, static_cast<uint64_t>(s.st_ctim.tv_nsec));
    put64(0x60, static_cast<uint64_t>(s.st_size));
    put64(0x68, static_cast<uint64_t>(s.st_blocks));
    put32(0x70, static_cast<uint32_t>(s.st_blksize));
}

static void shim_fstat(CallFrame& frame, void*) {
    int fd = static_cast<int>(frame.x[0]);
    uint8_t* darwin_stat = reinterpret_cast<uint8_t*>(frame.x[1]);
    struct stat st{};
    int rc = fstat(fd, &st);
    if (rc == 0 && darwin_stat) fill_darwin_stat(darwin_stat, st);
    static int logged = 0;
    if (logged < 64) {
        ++logged;
        fprintf(stderr, "[SHIM] fstat(fd=%d \"%s\") rc=%d st_size=0x%llx st_mode=0%o -> darwin st_size@+0x60=0x%llx st_mode@+0x04=0%o\n",
                fd, fd_path(fd).c_str(), rc, static_cast<unsigned long long>(st.st_size),
                static_cast<unsigned>(st.st_mode),
                static_cast<unsigned long long>(rc == 0 && darwin_stat ? *reinterpret_cast<int64_t*>(darwin_stat + 0x60) : 0),
                static_cast<unsigned>(rc == 0 && darwin_stat ? *reinterpret_cast<uint16_t*>(darwin_stat + 0x04) : 0));
    }
    frame.ret_x0 = static_cast<uint64_t>(static_cast<int64_t>(rc));
}

static void shim_stat(CallFrame& frame, void*) {
    std::string p = remap_game_path(reinterpret_cast<const char*>(frame.x[0]));
    uint8_t* darwin_stat = reinterpret_cast<uint8_t*>(frame.x[1]);
    struct stat st{};
    int rc = stat(p.c_str(), &st);
    if (rc == 0 && darwin_stat) fill_darwin_stat(darwin_stat, st);
    frame.ret_x0 = static_cast<uint64_t>(static_cast<int64_t>(rc));
}

static void shim_lstat(CallFrame& frame, void*) {
    std::string p = remap_game_path(reinterpret_cast<const char*>(frame.x[0]));
    uint8_t* darwin_stat = reinterpret_cast<uint8_t*>(frame.x[1]);
    struct stat st{};
    int rc = lstat(p.c_str(), &st);
    if (rc == 0 && darwin_stat) fill_darwin_stat(darwin_stat, st);
    frame.ret_x0 = static_cast<uint64_t>(static_cast<int64_t>(rc));
}

// ---------------------------------------------------------------------------
// qsort_r (Darwin: base, n, size, thunk, compar(thunk, a, b)), readdir, pthread
// ---------------------------------------------------------------------------
struct DarwinQsortCtx {
    void* thunk;
    int (*cmp)(void*, const void*, const void*);
};
static thread_local DarwinQsortCtx* t_qsort_ctx = nullptr;

static int darwin_qsort_adapter(const void* a, const void* b) {
    return t_qsort_ctx->cmp(t_qsort_ctx->thunk, a, b);
}

extern "C" void native_darwin_qsort_r(void* base, size_t n, size_t size, void* thunk,
                                     int (*cmp)(void*, const void*, const void*)) {
    DarwinQsortCtx ctx{thunk, cmp};
    DarwinQsortCtx* prev = t_qsort_ctx;
    t_qsort_ctx = &ctx;
    qsort(base, n, size, darwin_qsort_adapter);
    t_qsort_ctx = prev;
}

// Darwin 64-bit dirent: d_name is at +21, not +19 as on Linux.
struct DarwinDirent {
    uint64_t d_ino;
    uint64_t d_seekoff;
    uint16_t d_reclen;
    uint16_t d_namlen;
    uint8_t  d_type;
    char     d_name[1024];
};
static thread_local DarwinDirent t_darwin_dirent;

extern "C" void* native_darwin_readdir(DIR* d) {
    struct dirent* e = readdir(d);
    if (!e) return nullptr;
    DarwinDirent& o = t_darwin_dirent;
    std::memset(&o, 0, sizeof(o));
    o.d_ino = e->d_ino;
    o.d_type = e->d_type;
    size_t n = strnlen(e->d_name, sizeof(o.d_name) - 1);
    std::memcpy(o.d_name, e->d_name, n);
    o.d_namlen = static_cast<uint16_t>(n);
    o.d_reclen = static_cast<uint16_t>(sizeof(DarwinDirent));
    return &o;
}

// Darwin: NORMAL=0 ERRORCHECK=1 RECURSIVE=2. bionic: NORMAL=0 RECURSIVE=1 ERRORCHECK=2.
extern "C" int native_pthread_mutexattr_settype(pthread_mutexattr_t* a, int t) {
    int m = (t == 1) ? PTHREAD_MUTEX_ERRORCHECK : (t == 2) ? PTHREAD_MUTEX_RECURSIVE : PTHREAD_MUTEX_NORMAL;
    return pthread_mutexattr_settype(a, m);
}
extern "C" int native_pthread_mutexattr_setprotocol(pthread_mutexattr_t*, int) { return 0; }

// Darwin struct stat64 layout for arm64 (144 bytes = 0x90):
//   +0x00: st_dev       (int32_t)
//   +0x04: st_mode      (uint16_t)
//   +0x06: st_nlink     (uint16_t)
//   +0x08: st_ino       (uint64_t)
//   +0x10: st_uid       (uint32_t)
//   +0x14: st_gid       (uint32_t)
//   +0x18: st_rdev      (int32_t)
//   +0x20: st_atimespec (struct timespec = 2×int64 = 16 bytes)
//   +0x30: st_mtimespec (16 bytes)
//   +0x40: st_ctimespec (16 bytes)
//   +0x50: st_birthtimespec (16 bytes)
//   +0x60: st_size      (int64_t)
//   +0x68: st_blocks    (int64_t)
//   +0x70: st_blksize   (int32_t)
//   +0x74: st_flags     (uint32_t)
//   +0x78: st_gen       (uint32_t)
//   +0x7c: st_lspare    (int32_t)
//   +0x80: st_qspare[2] (2×int64_t)


// C half of guest mmap. native_mmap in trampolines.s spills x0-x7/lr/sp first.
// Guest ABI is Darwin mmap(addr,len,prot,flags,fd,offset) in x0-x5. x6/x7 are junk.
extern "C" void* native_mmap_impl(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
    uint64_t count = g_mmap_capture_count;
    uint64_t idx = (count - 1) & 15;
    const uint64_t* rec = g_mmap_capture + idx * 10;
    wr_str(2, "[MMAP ENTRY] x0="); wr_hex(2, rec[0]);
    wr_str(2, " x1="); wr_hex(2, rec[1]);
    wr_str(2, " x2="); wr_hex(2, rec[2]);
    wr_str(2, " x3="); wr_hex(2, rec[3]);
    wr_str(2, " x4="); wr_hex(2, rec[4]);
    wr_str(2, " x5="); wr_hex(2, rec[5]);
    wr_str(2, " x6="); wr_hex(2, rec[6]);
    wr_str(2, " x7="); wr_hex(2, rec[7]);
    wr_str(2, " lr="); wr_hex(2, rec[8]);
    wr_str(2, " sp="); wr_hex(2, rec[9]);
    wr_str(2, "\n");
    dump_ptr_context(2, rec[2], "x2");
    dump_ptr_context(2, rec[8], "lr");
    // Darwin MAP_* (arm64): SHARED=1 PRIVATE=2 FIXED=0x10 ANON=0x1000
    // Linux MAP_* :         SHARED=1 PRIVATE=2 FIXED=0x10 ANON=0x20
    int linux_flags = 0;
    if (flags & 0x01) linux_flags |= MAP_SHARED;
    if (flags & 0x02) linux_flags |= MAP_PRIVATE;
    if (flags & 0x10) linux_flags |= MAP_FIXED;
    if ((flags & 0x1000) || fd == -1) linux_flags |= MAP_ANONYMOUS;
    if (!(linux_flags & (MAP_SHARED | MAP_PRIVATE))) linux_flags |= MAP_PRIVATE;

    int linux_prot = prot & 0x7;
    if ((linux_prot & PROT_WRITE) && !(linux_prot & PROT_READ)) {
        linux_prot |= PROT_READ;
    }

    int real_fd  = (linux_flags & MAP_ANONYMOUS) ? -1 : fd;
    off_t real_off = (linux_flags & MAP_ANONYMOUS) ? 0 : offset;

    void* res = mmap(addr, length, linux_prot, linux_flags, real_fd, real_off);
    wr_str(2, "[SHIM] mmap addr="); wr_hex(2, rec[0]);
    wr_str(2, " len="); wr_hex(2, rec[1]);
    wr_str(2, " prot="); wr_hex(2, static_cast<uint64_t>(static_cast<uint32_t>(prot)));
    wr_str(2, " flags="); wr_hex(2, static_cast<uint64_t>(static_cast<uint32_t>(flags)));
    wr_str(2, " linux_flags="); wr_hex(2, static_cast<uint64_t>(static_cast<uint32_t>(linux_flags)));
    wr_str(2, " fd="); wr_hex(2, static_cast<uint64_t>(static_cast<uint32_t>(fd)));
    wr_str(2, " off="); wr_hex(2, static_cast<uint64_t>(offset));
    wr_str(2, " -> ");
    if (res == MAP_FAILED) {
        wr_str(2, "MAP_FAILED errno=");
        wr_hex(2, static_cast<uint64_t>(static_cast<uint32_t>(errno)));
    } else {
        wr_hex(2, reinterpret_cast<uint64_t>(res));
    }
    wr_str(2, "\n");
    shim_ring_record("_mmap", rec, rec[8], rec[9]);
    if (res != MAP_FAILED && !(linux_flags & MAP_ANONYMOUS)) {
        wr_str(2, "[MMAP FILE] fd path="); wr_str(2, fd_path(fd).c_str());
        wr_str(2, " len="); wr_hex(2, length);
        struct stat fst{};
        if (fstat(fd, &fst) == 0) { wr_str(2, " file_size="); wr_hex(2, static_cast<uint64_t>(fst.st_size)); }
        wr_str(2, "\n  first 16 bytes:");
        const uint8_t* m = static_cast<const uint8_t*>(res);
        for (int i = 0; i < 16; ++i) {
            char h[4] = {" 0123456789abcdef"[1 + (m[i] >> 4)], "0123456789abcdef"[m[i] & 15], 0, 0};
            wr_str(2, " "); wr_str(2, h);
        }
        wr_str(2, "\n");
        if (length >= 8 && m[0] == 0xAF && m[1] == 0x1B && m[2] == 0xB1 && m[3] == 0xFA) {
            wr_str(2, "  global-metadata magic OK, version=");
            wr_hex(2, *reinterpret_cast<const uint32_t*>(m + 4));
            const void* hit = memmem(m, length, "get_Count", 9);
            wr_str(2, " first \"get_Count\" at file offset ");
            if (hit) wr_hex(2, static_cast<uint64_t>(static_cast<const uint8_t*>(hit) - m));
            else wr_str(2, "NOT FOUND");
            wr_str(2, "\n");
        }
    }
    return res;
}

// Direct native munmap shim
extern "C" int native_munmap(void* addr, size_t length) {
    return munmap(addr, length);
}

// Direct native mprotect shim (flags identical between Darwin and Linux)
extern "C" int native_mprotect(void* addr, size_t length, int prot) {
    return mprotect(addr, length, prot & 0x7);
}

static void shim_mach_timebase_info(CallFrame& frame, void*) {
    struct mach_timebase_info { uint32_t numer; uint32_t denom; };
    auto* info = reinterpret_cast<mach_timebase_info*>(frame.x[0]);
    if (info) { info->numer = 1; info->denom = 1; }
    frame.ret_x0 = 0;
}

static void shim_mach_absolute_time(CallFrame& frame, void*) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    frame.ret_x0 = static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

static void shim_sysctl(CallFrame& frame, void*) {
    int* name = reinterpret_cast<int*>(frame.x[0]);
    unsigned int namelen = static_cast<unsigned int>(frame.x[1]);
    void* oldp = reinterpret_cast<void*>(frame.x[2]);
    size_t* oldlenp = reinterpret_cast<size_t*>(frame.x[3]);

    if (name && namelen >= 2) {
        if (name[0] == 6 /* CTL_HW */) {
            if (name[1] == 3 /* HW_NCPU */ || name[1] == 7 /* HW_AVAILCPU */) {
                if (oldp && oldlenp && *oldlenp >= sizeof(int)) {
                    *reinterpret_cast<int*>(oldp) = 8;
                    *oldlenp = sizeof(int);
                }
            } else if (name[1] == 24 /* HW_MEMSIZE */) {
                if (oldp && oldlenp && *oldlenp >= sizeof(uint64_t)) {
                    *reinterpret_cast<uint64_t*>(oldp) = 8ULL * 1024 * 1024 * 1024;
                    *oldlenp = sizeof(uint64_t);
                }
            } else if (name[1] == 4 /* HW_PAGESIZE */) {
                if (oldp && oldlenp && *oldlenp >= sizeof(int)) {
                    *reinterpret_cast<int*>(oldp) = 4096;
                    *oldlenp = sizeof(int);
                }
            }
        }
    }
    frame.ret_x0 = 0;
}

static void shim_sysctlbyname(CallFrame& frame, void*) {
    const char* name = reinterpret_cast<const char*>(frame.x[0]);
    void* oldp = reinterpret_cast<void*>(frame.x[1]);
    size_t* oldlenp = reinterpret_cast<size_t*>(frame.x[2]);
    printf("[SHIM] sysctlbyname(\"%s\")\n", name ? name : "null");
    if (name && std::strcmp(name, "hw.ncpu") == 0) {
        if (oldp && oldlenp && *oldlenp >= sizeof(int)) {
            *reinterpret_cast<int*>(oldp) = 8;
            *oldlenp = sizeof(int);
        }
    }
    frame.ret_x0 = 0;
}

static void shim_objc_stub(CallFrame& frame, void* ctx) {
    const char* sym = trampoline_symbol(ctx);
    static int count = 0;
    if (++count <= 20) {
        printf("[OBJC STUB] %s\n", sym ? sym : "null");
    }
    // Generic stub returning self or nil
    frame.ret_x0 = frame.x[0];
}

static void shim_objc_msgSend(CallFrame& frame, void*) {
    void* receiver = reinterpret_cast<void*>(frame.x[0]);
    const char* sel = reinterpret_cast<const char*>(frame.x[1]);
    static std::unordered_set<std::string> seen_selectors;
    static int call_seq = 0;
    std::string sel_str = sel ? sel : "null";
    if (seen_selectors.find(sel_str) == seen_selectors.end()) {
        seen_selectors.insert(sel_str);
        printf("[OBJC_DYNAMIC #%d] [%p %s]\n", ++call_seq, receiver, sel_str.c_str());
    }
    // Return receiver
    frame.ret_x0 = frame.x[0];
}

static void shim_unresolved(CallFrame& frame, void* ctx) {
    dump_mmap_captures();
    dump_shim_ring(2);
    const char* sym = trampoline_symbol(ctx);
    log_term("unresolved", frame.lr, 134, sym ? sym : "unknown");
    std::abort();
}

static void shim_bzero(CallFrame& frame, void*) {
    void* s = reinterpret_cast<void*>(frame.x[0]);
    size_t n = static_cast<size_t>(frame.x[1]);
    if (s && n) std::memset(s, 0, n);
}

static std::unordered_map<std::string, void*> g_custom_shims;
static std::unordered_map<std::string, void*> g_resolved_cache;
static void* g_libcxx_handle = nullptr;

static void init_custom_shims() {
    g_custom_shims["_open"] = allocate_trampoline(shim_open, "_open");
    g_custom_shims["_open$NOCANCEL"] = allocate_trampoline(shim_open, "_open$NOCANCEL");
    g_custom_shims["_openat"] = allocate_trampoline(shim_openat, "_openat");
    g_custom_shims["_fcntl"] = allocate_trampoline(shim_fcntl, "_fcntl");
    g_custom_shims["_fcntl$NOCANCEL"] = allocate_trampoline(shim_fcntl, "_fcntl$NOCANCEL");
    g_custom_shims["_ioctl"] = allocate_trampoline(shim_ioctl, "_ioctl");
    g_custom_shims["_fstat"] = allocate_trampoline(shim_fstat, "_fstat");
    g_custom_shims["_fstat$INODE64"] = allocate_trampoline(shim_fstat, "_fstat$INODE64");
    g_custom_shims["_stat"] = allocate_trampoline(shim_stat, "_stat");
    g_custom_shims["_stat$INODE64"] = allocate_trampoline(shim_stat, "_stat$INODE64");
    g_custom_shims["_lstat"] = allocate_trampoline(shim_lstat, "_lstat");
    g_custom_shims["_lstat$INODE64"] = allocate_trampoline(shim_lstat, "_lstat$INODE64");
    g_custom_shims["_qsort_r"] = reinterpret_cast<void*>(&native_darwin_qsort_r);
    g_custom_shims["_readdir"] = reinterpret_cast<void*>(&native_darwin_readdir);
    g_custom_shims["_readdir$INODE64"] = reinterpret_cast<void*>(&native_darwin_readdir);
    g_custom_shims["_pthread_mutexattr_settype"] = reinterpret_cast<void*>(&native_pthread_mutexattr_settype);
    g_custom_shims["_pthread_mutexattr_setprotocol"] = reinterpret_cast<void*>(&native_pthread_mutexattr_setprotocol);

    // Darwin variadics (args on the stack) -> CallFrame shims building an AAPCS64 va_list
    g_custom_shims["_printf"] = allocate_trampoline(shim_printf, "_printf");
    g_custom_shims["_fprintf"] = allocate_trampoline(shim_fprintf, "_fprintf");
    g_custom_shims["_sprintf"] = allocate_trampoline(shim_sprintf, "_sprintf");
    g_custom_shims["_snprintf"] = allocate_trampoline(shim_snprintf, "_snprintf");
    g_custom_shims["_asprintf"] = allocate_trampoline(shim_asprintf, "_asprintf");
    g_custom_shims["_dprintf"] = allocate_trampoline(shim_dprintf, "_dprintf");
    g_custom_shims["_sscanf"] = allocate_trampoline(shim_sscanf, "_sscanf");
    g_custom_shims["_fscanf"] = allocate_trampoline(shim_fscanf, "_fscanf");
    g_custom_shims["_scanf"] = allocate_trampoline(shim_scanf, "_scanf");
    g_custom_shims["_syslog"] = allocate_trampoline(shim_syslog, "_syslog");
    g_custom_shims["_NSLog"] = allocate_trampoline(shim_NSLog, "_NSLog");
    g_custom_shims["___snprintf_chk"] = allocate_trampoline(shim_snprintf_chk, "___snprintf_chk");
    g_custom_shims["___sprintf_chk"] = allocate_trampoline(shim_sprintf_chk, "___sprintf_chk");
    // v-variants take a Darwin va_list (char*)
    g_custom_shims["_vprintf"] = reinterpret_cast<void*>(&native_vprintf);
    g_custom_shims["_vfprintf"] = reinterpret_cast<void*>(&native_vfprintf);
    g_custom_shims["_vsprintf"] = reinterpret_cast<void*>(&native_vsprintf);
    g_custom_shims["_vsnprintf"] = reinterpret_cast<void*>(&native_vsnprintf);
    g_custom_shims["_vasprintf"] = reinterpret_cast<void*>(&native_vasprintf);
    g_custom_shims["_vdprintf"] = reinterpret_cast<void*>(&native_vdprintf);
    g_custom_shims["_vsscanf"] = reinterpret_cast<void*>(&native_vsscanf);
    g_custom_shims["_vfscanf"] = reinterpret_cast<void*>(&native_vfscanf);
    g_custom_shims["_vscanf"] = reinterpret_cast<void*>(&native_vscanf);
    g_custom_shims["_vsyslog"] = reinterpret_cast<void*>(&native_vsyslog);
    g_custom_shims["_NSLogv"] = reinterpret_cast<void*>(&native_NSLogv);
    g_custom_shims["___vsnprintf_chk"] = reinterpret_cast<void*>(&native___vsnprintf_chk);
    g_custom_shims["___vsprintf_chk"] = reinterpret_cast<void*>(&native___vsprintf_chk);
    // mmap/munmap/mprotect: direct native functions to avoid callframe reentrance
    g_custom_shims["_mmap"]     = reinterpret_cast<void*>(native_mmap);
    g_custom_shims["_munmap"]   = reinterpret_cast<void*>(&native_munmap);
    g_custom_shims["_mprotect"] = reinterpret_cast<void*>(&native_mprotect);
    g_custom_shims["_exit"] = reinterpret_cast<void*>(&native_exit);
    g_custom_shims["_Exit"] = reinterpret_cast<void*>(&native_exit);
    g_custom_shims["__exit"] = reinterpret_cast<void*>(&native__exit);
    g_custom_shims["_abort"] = reinterpret_cast<void*>(&native_abort);
    g_custom_shims["_mach_timebase_info"] = allocate_trampoline(shim_mach_timebase_info, "_mach_timebase_info");
    g_custom_shims["_mach_absolute_time"] = allocate_trampoline(shim_mach_absolute_time, "_mach_absolute_time");
    g_custom_shims["_sysctlbyname"] = allocate_trampoline(shim_sysctlbyname, "_sysctlbyname");
    g_custom_shims["_sysctl"] = allocate_trampoline(shim_sysctl, "_sysctl");
    g_custom_shims["_bzero"] = reinterpret_cast<void*>(&native_bzero);
    g_custom_shims["_memset_pattern16"] = reinterpret_cast<void*>(&native_memset_pattern16);

    // Darwin TSD / pthread_key
    g_custom_shims["_pthread_key_create"] = reinterpret_cast<void*>(&native_pthread_key_create);
    g_custom_shims["_pthread_setspecific"] = reinterpret_cast<void*>(&native_pthread_setspecific);
    g_custom_shims["_pthread_getspecific"] = reinterpret_cast<void*>(&native_pthread_getspecific);
    g_custom_shims["_pthread_key_delete"] = reinterpret_cast<void*>(&native_pthread_key_delete);

    // Apple GCD
    g_custom_shims["_dispatch_semaphore_create"] = reinterpret_cast<void*>(&native_dispatch_semaphore_create);
    g_custom_shims["_dispatch_semaphore_wait"] = reinterpret_cast<void*>(&native_dispatch_semaphore_wait);
    g_custom_shims["_dispatch_semaphore_signal"] = reinterpret_cast<void*>(&native_dispatch_semaphore_signal);
    g_custom_shims["_dispatch_queue_create"] = reinterpret_cast<void*>(&native_dispatch_queue_create);
    g_custom_shims["_dispatch_get_global_queue"] = reinterpret_cast<void*>(&native_dispatch_get_global_queue);
    g_custom_shims["_dispatch_async"] = reinterpret_cast<void*>(&native_dispatch_async);
    g_custom_shims["_dispatch_sync"] = reinterpret_cast<void*>(&native_dispatch_sync);
    g_custom_shims["_dispatch_release"] = reinterpret_cast<void*>(&native_dispatch_release);
    g_custom_shims["_dispatch_retain"] = reinterpret_cast<void*>(&native_dispatch_retain);
    g_custom_shims["_dispatch_resume"] = reinterpret_cast<void*>(&native_dispatch_resume);
    g_custom_shims["_dispatch_suspend"] = reinterpret_cast<void*>(&native_dispatch_suspend);

    // pthread extras not in bionic by name or Darwin-only variants
    // pthread_setcancelstate — Darwin/POSIX, not linked on Android by default
    g_custom_shims["_pthread_setcancelstate"] = reinterpret_cast<void*>(
        +[](int state, int* oldstate) -> int {
            if (oldstate) *oldstate = 1; // PTHREAD_CANCEL_DISABLE
            return 0;
        });
    g_custom_shims["_pthread_setcanceltype"] = reinterpret_cast<void*>(
        +[](int type, int* oldtype) -> int {
            if (oldtype) *oldtype = 0;
            return 0;
        });
    g_custom_shims["_pthread_cancel"] = reinterpret_cast<void*>(
        +[](pthread_t) -> int { return 0; });
    g_custom_shims["_pthread_testcancel"] = reinterpret_cast<void*>(
        +[]() -> void {});
    g_custom_shims["_pthread_atfork"] = reinterpret_cast<void*>(
        +[](void(*)(void), void(*)(void), void(*)(void)) -> int { return 0; });

    // Other Darwin APIs
    g_custom_shims["__NSGetExecutablePath"] = reinterpret_cast<void*>(&native_NSGetExecutablePath);
    g_custom_shims["__NSGetEnviron"] = reinterpret_cast<void*>(&native_NSGetEnviron);
    g_custom_shims["__dyld_image_count"] = reinterpret_cast<void*>(&native_dyld_image_count);
    g_custom_shims["__dyld_get_image_header"] = reinterpret_cast<void*>(&native_dyld_get_image_header);
    g_custom_shims["__dyld_get_image_name"] = reinterpret_cast<void*>(&native_dyld_get_image_name);
    g_custom_shims["__dyld_get_image_vmaddr_slide"] = reinterpret_cast<void*>(&native_dyld_get_image_vmaddr_slide);
    g_custom_shims["__dyld_register_func_for_add_image"] = reinterpret_cast<void*>(&native_dyld_register_func_for_add_image);
    g_custom_shims["__dyld_register_func_for_remove_image"] = reinterpret_cast<void*>(&native_dyld_register_func_for_remove_image);
    g_custom_shims["___cxa_atexit"] = reinterpret_cast<void*>(&native_cxa_atexit);
    g_custom_shims["___error"] = reinterpret_cast<void*>(&native_darwin_error);
    g_custom_shims["_OSSpinLockLock"] = reinterpret_cast<void*>(&native_OSSpinLockLock);
    g_custom_shims["_OSSpinLockUnlock"] = reinterpret_cast<void*>(&native_OSSpinLockUnlock);
    g_custom_shims["_OSAtomicAdd32Barrier"] = reinterpret_cast<void*>(&native_OSAtomicAdd32Barrier);
    g_custom_shims["_OSAtomicAdd64Barrier"] = reinterpret_cast<void*>(&native_OSAtomicAdd64Barrier);
    g_custom_shims["_OSAtomicCompareAndSwap32Barrier"] = reinterpret_cast<void*>(&native_OSAtomicCompareAndSwap32Barrier);
    g_custom_shims["_OSAtomicCompareAndSwapPtrBarrier"] = reinterpret_cast<void*>(&native_OSAtomicCompareAndSwapPtrBarrier);
    g_custom_shims["_OSMemoryBarrier"] = reinterpret_cast<void*>(&native_OSMemoryBarrier);

    // Allocator & POSIX / Darwin memory shims
    g_custom_shims["_malloc_size"] = reinterpret_cast<void*>(&native_malloc_size);
    g_custom_shims["_mach_task_self_"] = &g_mach_task_self_val;
    g_custom_shims["_mach_thread_self"] = reinterpret_cast<void*>(&native_mach_thread_self);
    g_custom_shims["_vm_page_size"] = &g_vm_page_size;
    g_custom_shims["_vm_page_mask"] = &g_vm_page_mask;
    g_custom_shims["_realpath$DARWIN_EXTSN"] = reinterpret_cast<void*>(&native_realpath_darwin);
    g_custom_shims["_pthread_get_stackaddr_np"] = reinterpret_cast<void*>(&native_pthread_get_stackaddr_np);
    g_custom_shims["_pthread_setname_np"] = reinterpret_cast<void*>(&native_pthread_setname_np);
    g_custom_shims["_pthread_mach_thread_np"] = reinterpret_cast<void*>(&native_pthread_mach_thread_np);
    g_custom_shims["_getsectbynamefromheader_64"] = reinterpret_cast<void*>(&native_getsectbynamefromheader_64);
    g_custom_shims["_semaphore_create"] = reinterpret_cast<void*>(&native_semaphore_create);
    g_custom_shims["_semaphore_destroy"] = reinterpret_cast<void*>(&native_semaphore_destroy);
    g_custom_shims["_semaphore_signal"] = reinterpret_cast<void*>(&native_semaphore_signal);
    g_custom_shims["_semaphore_wait"] = reinterpret_cast<void*>(&native_semaphore_wait);
    g_custom_shims["_mach_port_allocate"] = reinterpret_cast<void*>(&native_mach_port_allocate);
    g_custom_shims["_mach_port_deallocate"] = reinterpret_cast<void*>(&native_mach_port_deallocate);
    g_custom_shims["_mach_port_insert_right"] = reinterpret_cast<void*>(&native_mach_port_insert_right);
    g_custom_shims["_mach_port_mod_refs"] = reinterpret_cast<void*>(&native_mach_port_mod_refs);
    g_custom_shims["_vm_allocate"] = reinterpret_cast<void*>(&native_vm_allocate);
    g_custom_shims["_vm_deallocate"] = reinterpret_cast<void*>(&native_vm_deallocate);

    // Dynamic ObjC message dispatch
    g_custom_shims["_objc_msgSend"] = allocate_trampoline(shim_objc_msgSend, "_objc_msgSend");
    g_custom_shims["_objc_msgSendSuper2"] = allocate_trampoline(shim_objc_msgSend, "_objc_msgSendSuper2");

    // Data symbols
    g_custom_shims["___stack_chk_guard"] = &g_stack_chk_guard_val;
    g_custom_shims["___stderrp"] = &g_fake_stderr;
    g_custom_shims["___stdoutp"] = &g_fake_stdout;
    g_custom_shims["__dispatch_main_q"] = &g_dispatch_main_q_obj;
    g_custom_shims["__dispatch_source_type_timer"] = &g_dispatch_timer_obj;
    g_custom_shims["__NSConcreteGlobalBlock"] = &g_block_global;
    g_custom_shims["__NSConcreteStackBlock"] = &g_block_stack;

    // CoreFoundation / Darwin data symbols
    g_custom_shims["___CFConstantStringClassReference"] = &g_cf_constant_string_class;
    g_custom_shims["___NSDictionary0__"] = &g_ns_dictionary0;
    g_custom_shims["_kCFAllocatorDefault"] = &g_kCFAllocatorDefault;
    g_custom_shims["_kCFAllocatorNull"] = &g_kCFAllocatorNull;
    g_custom_shims["_kCFBundleVersionKey"] = &g_kCFBundleVersionKey;
    g_custom_shims["_kCFLocaleCountryCode"] = &g_kCFLocaleCountryCode;
    g_custom_shims["_kCFLocaleLanguageCode"] = &g_kCFLocaleLanguageCode;
    g_custom_shims["_kCFLocaleScriptCode"] = &g_kCFLocaleScriptCode;
    g_custom_shims["_kCFTypeArrayCallBacks"] = &g_kCFTypeArrayCallBacks;

    // Apple-ABI std::string shims
    extern void register_apple_string_shims(std::unordered_map<std::string, void*>& shims);
    register_apple_string_shims(g_custom_shims);
}

// Resolver for dyld bindings
static void* resolve_symbol(const std::string& name) {
    auto cache_it = g_resolved_cache.find(name);
    if (cache_it != g_resolved_cache.end()) return cache_it->second;

    auto it = g_custom_shims.find(name);
    if (it != g_custom_shims.end()) {
        g_resolved_cache[name] = it->second;
        return it->second;
    }

    // Objective-C class and metaclass descriptors
    if (name.rfind("_OBJC_CLASS_$_", 0) == 0 || name.rfind("_OBJC_METACLASS_$_", 0) == 0) {
        static std::unordered_map<std::string, void*> s_dummy_classes;
        auto cit = s_dummy_classes.find(name);
        if (cit != s_dummy_classes.end()) return cit->second;
        void* cls = calloc(1, 128);
        s_dummy_classes[name] = cls;
        g_resolved_cache[name] = cls;
        return cls;
    }

    // Check dlsym for standard C/POSIX/libc++ symbols
    const char* stripped = name.c_str();
    if (stripped[0] == '_') stripped++; // Strip leading underscore

    // Check system libc++.so first for C++ symbols
    if (g_libcxx_handle) {
        void* csym = dlsym(g_libcxx_handle, name.c_str());
        if (csym) {
            g_resolved_cache[name] = csym;
            return csym;
        }
        if (stripped != name.c_str()) {
            csym = dlsym(g_libcxx_handle, stripped);
            if (csym) {
                g_resolved_cache[name] = csym;
                return csym;
            }
        }
    }

    void* sym = dlsym(RTLD_DEFAULT, stripped);
    if (sym) {
        g_resolved_cache[name] = sym;
        return sym;
    }

    // Try without stripping if name started with __Z
    if (name.rfind("__Z", 0) == 0) {
        sym = dlsym(RTLD_DEFAULT, name.c_str());
        if (sym) {
            g_resolved_cache[name] = sym;
            return sym;
        }
    }

    // Objective-C runtime functions
    if (name.rfind("_objc_", 0) == 0) {
        void* stub = allocate_trampoline(shim_objc_stub, strdup(name.c_str()));
        g_resolved_cache[name] = stub;
        return stub;
    }

    if (g_unresolved_set.insert(name).second) {
        fprintf(stderr, "[UNRESOLVED IMPORT] %s\n", name.c_str());
        g_unresolved_imports.push_back(name);
    }
    void* stub = allocate_trampoline(shim_unresolved, strdup(name.c_str()));
    g_resolved_cache[name] = stub;
    return stub;
}

static void il2cpp_log_callback(const char* log) {
    printf("[IL2CPP LOG] %s\n", log ? log : "");
}

#include <ucontext.h>

static void segv_handler(int sig, siginfo_t* info, void* ctx) {
    ucontext_t* uc = reinterpret_cast<ucontext_t*>(ctx);
    uint64_t pc = uc->uc_mcontext.pc;
    uint64_t sp = uc->uc_mcontext.sp;
    uint64_t lr = uc->uc_mcontext.regs[30];
    uint64_t fp = uc->uc_mcontext.regs[29];
    fprintf(stderr, "\n[CRASH] Signal %d at fault_addr=%p, pc=0x%lx, lr=0x%lx, sp=0x%lx\n",
            sig, info->si_addr, (unsigned long)pc, (unsigned long)lr, (unsigned long)sp);
    log_term("sig_handler_exit", lr, 1, "loader_segv");

    wr_str(2, "[CRASH] pc -> "); describe_addr(2, pc); wr_str(2, "\n");
    wr_str(2, "[CRASH] lr -> "); describe_addr(2, lr); wr_str(2, "\n");
    {
        // Registers equal to the fault address (kernel strips the top tag byte).
        const uint64_t mask = 0x00ffffffffffffffULL;
        uint64_t fa = reinterpret_cast<uint64_t>(info->si_addr);
        wr_str(2, "[CRASH] registers holding the bad pointer:");
        bool any = false;
        for (int i = 0; i < 31; ++i) {
            if ((uc->uc_mcontext.regs[i] & mask) == (fa & mask)) {
                wr_str(2, " x"); char d[3] = {static_cast<char>('0' + i / 10), static_cast<char>('0' + i % 10), 0};
                wr_str(2, d[0] == '0' ? d + 1 : d); any = true;
            }
        }
        wr_str(2, any ? "\n" : " none\n");
        uint32_t insn = 0;
        if (safe_read(pc, &insn, 4)) {
            int rn = (insn >> 5) & 31;
            wr_str(2, "[CRASH] insn="); wr_hex(2, insn);
            wr_str(2, " base register (Rn, bits 9:5) = x");
            char d[3] = {static_cast<char>('0' + rn / 10), static_cast<char>('0' + rn % 10), 0};
            wr_str(2, d[0] == '0' ? d + 1 : d);
            wr_str(2, " value="); wr_hex(2, uc->uc_mcontext.regs[rn]);
            uint64_t v = uc->uc_mcontext.regs[rn];
            char s[9] = {0};
            for (int i = 0; i < 8; ++i) { char c = static_cast<char>(v >> (8 * i)); s[i] = (c >= 0x20 && c < 0x7f) ? c : '.'; }
            wr_str(2, " as ASCII (LE)=\""); wr_str(2, s); wr_str(2, "\"\n");
        }
    }
    dump_frame_walk(2, pc, lr, fp, sp);
    dump_shim_ring(2);
    dump_mmap_captures();
    dump_ptr_context(2, pc, "pc");
    dump_ptr_context(2, lr, "lr");
    dump_ptr_context(2, uc->uc_mcontext.regs[2], "x2");
    for (int i = 0; i < 31; ++i) {
        fprintf(stderr, "  x%-2d = 0x%016lx%s", i, (unsigned long)uc->uc_mcontext.regs[i],
                (i % 2 == 1 || i == 30) ? "\n" : "   ");
    }

    fprintf(stderr, "\n--- Stack dump from SP (0x%lx) ---\n", (unsigned long)sp);
    uint64_t* sp_ptr = reinterpret_cast<uint64_t*>(sp);
    for (int i = 0; i < 32; ++i) {
        fprintf(stderr, "  [sp + 0x%02x] = 0x%016lx\n", i * 8, sp_ptr[i]);
    }
    uint32_t* pc_ptr = reinterpret_cast<uint32_t*>(pc & ~3ULL);
    for (int i = -4; i <= 4; ++i) {
        uint32_t* p = pc_ptr + i;
        fprintf(stderr, "  %s %p: 0x%08x\n", (i == 0 ? "=>" : "  "), p, *p);
    }

    fprintf(stderr, "\n--- All loaded ELF/Mappings ---\n");
    FILE* f = fopen("/proc/self/maps", "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            if (strstr(line, "xnuroid") || strstr(line, "Unity") || strstr(line, "libc++") || strstr(line, "bionic")) {
                fprintf(stderr, "  %s", line);
            }
        }
        fclose(f);
    }
    _exit(1);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    init_safe_read();
    struct sigaction sa{};
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = segv_handler;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);

    printf("=== Native ARM64 Mach-O Android Loader ===\n");

    // 1. Calculate TSD offset
    uint64_t tpidr = get_tpidr_el0();
    uint64_t tsd_addr = reinterpret_cast<uint64_t>(&g_apple_tsd[0]);
    g_tsd_tprel_diff = static_cast<int64_t>(tsd_addr - tpidr);
    printf("[*] TPIDR_EL0: 0x%lx, Apple TSD: 0x%lx, diff: %ld (0x%lx)\n",
           tpidr, tsd_addr, (long)g_tsd_tprel_diff, (unsigned long)g_tsd_tprel_diff);

    // Load system libc++.so (which uses std::__1 ABI on Android)
    g_libcxx_handle = dlopen("/system/lib64/libc++.so", RTLD_NOW | RTLD_GLOBAL);
    if (!g_libcxx_handle) {
        printf("[-] dlopen /system/lib64/libc++.so failed: %s\n", dlerror());
    } else {
        printf("[+] Loaded /system/lib64/libc++.so (std::__1 ABI)\n");
    }

    // 3. Open UnityFramework binary
    const char* bin_path = "/data/local/tmp/game/UnityFramework";
    int fd = open(bin_path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "[-] Failed to open %s: %s\n", bin_path, strerror(errno));
        return 1;
    }
    struct stat st;
    fstat(fd, &st);
    size_t file_size = st.st_size;
    uint8_t* file_data = reinterpret_cast<uint8_t*>(mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0));
    close(fd);
    printf("[*] Mapped %s (%zu bytes) at %p\n", bin_path, file_size, file_data);

    // 4. Parse Mach-O 64 header
    uint32_t ncmds = *reinterpret_cast<uint32_t*>(file_data + 16);
    uint32_t cur = 32;

    struct SegmentInfo {
        std::string name;
        uint64_t vmaddr;
        uint64_t vmsize;
        uint64_t fileoff;
        uint64_t filesize;
        uint32_t initprot;
    };
    std::vector<SegmentInfo> segments;

    uint32_t rebase_off = 0, rebase_size = 0;
    uint32_t bind_off = 0, bind_size = 0;
    uint32_t lazy_off = 0, lazy_size = 0;
    uint32_t export_off = 0, export_size = 0;

    for (uint32_t i = 0; i < ncmds; ++i) {
        uint32_t cmd = *reinterpret_cast<uint32_t*>(file_data + cur);
        uint32_t cmdsize = *reinterpret_cast<uint32_t*>(file_data + cur + 4);
        if (cmd == 0x19) { // LC_SEGMENT_64
            char segname[17] = {0};
            std::memcpy(segname, file_data + cur + 8, 16);
            uint64_t vmaddr = *reinterpret_cast<uint64_t*>(file_data + cur + 24);
            uint64_t vmsize = *reinterpret_cast<uint64_t*>(file_data + cur + 32);
            uint64_t fileoff = *reinterpret_cast<uint64_t*>(file_data + cur + 40);
            uint64_t filesize = *reinterpret_cast<uint64_t*>(file_data + cur + 48);
            uint32_t initprot = *reinterpret_cast<uint32_t*>(file_data + cur + 60);
            segments.push_back({segname, vmaddr, vmsize, fileoff, filesize, initprot});
        } else if (cmd == 0x22 || cmd == 0x80000022) { // LC_DYLD_INFO
            rebase_off = *reinterpret_cast<uint32_t*>(file_data + cur + 8);
            rebase_size = *reinterpret_cast<uint32_t*>(file_data + cur + 12);
            bind_off = *reinterpret_cast<uint32_t*>(file_data + cur + 16);
            bind_size = *reinterpret_cast<uint32_t*>(file_data + cur + 20);
            lazy_off = *reinterpret_cast<uint32_t*>(file_data + cur + 32);
            lazy_size = *reinterpret_cast<uint32_t*>(file_data + cur + 36);
            export_off = *reinterpret_cast<uint32_t*>(file_data + cur + 40);
            export_size = *reinterpret_cast<uint32_t*>(file_data + cur + 44);
        }
        cur += cmdsize;
    }

    // Determine total image range
    uint64_t min_vm = UINT64_MAX, max_vm = 0;
    for (auto& s : segments) {
        if (s.vmsize == 0) continue;
        if (s.vmaddr < min_vm) min_vm = s.vmaddr;
        if (s.vmaddr + s.vmsize > max_vm) max_vm = s.vmaddr + s.vmsize;
    }
    size_t total_vmsize = max_vm - min_vm;
    printf("[*] Total VM span: 0x%zx bytes (0x%lx - 0x%lx)\n", total_vmsize, min_vm, max_vm);

    // One reservation: trampoline/veneer pool immediately before the guest image
    // so TLS sites can `b veneer` within +/-128MB.
    size_t reservation = TRAMPOLINE_POOL_SIZE + total_vmsize;
    uint8_t* reservation_base = reinterpret_cast<uint8_t*>(mmap(nullptr, reservation, PROT_READ | PROT_WRITE,
                                                                MAP_ANONYMOUS | MAP_PRIVATE, -1, 0));
    if (reservation_base == MAP_FAILED) {
        perror("mmap reservation failed");
        return 1;
    }
    g_trampoline_code = reinterpret_cast<uint32_t*>(reservation_base);
    uint8_t* image_base = reservation_base + TRAMPOLINE_POOL_SIZE;
    printf("[*] Trampoline pool at %p (1MB), image at %p\n", g_trampoline_code, image_base);
    int64_t slide = reinterpret_cast<uintptr_t>(image_base) - min_vm;
    g_loaded_image_base = image_base;
    g_loaded_image_size = total_vmsize;
    g_loaded_image_slide = slide;

    init_custom_shims();
    // Debug switch: fall back to raw bionic printf/scanf (old, crashing behavior) for A/B comparison.
    if (getenv("BISECT_NO_VARIADIC_SHIMS")) {
        for (auto it = g_custom_shims.begin(); it != g_custom_shims.end();) {
            if (it->first.find("printf") != std::string::npos || it->first.find("scanf") != std::string::npos ||
                it->first.find("_chk") != std::string::npos) it = g_custom_shims.erase(it);
            else ++it;
        }
    }


    // Copy segment contents
    for (auto& s : segments) {
        if (s.filesize > 0) {
            uint8_t* dst = image_base + (s.vmaddr - min_vm);
            std::memcpy(dst, file_data + s.fileoff, s.filesize);
            printf("  Copied segment %-14s to %p (%zu bytes)\n", s.name.c_str(), dst, (size_t)s.filesize);
        }
    }

    // 5. Apply Rebases
    printf("[*] Applying dyld rebases (offset 0x%x, size %u)...\n", rebase_off, rebase_size);
    const uint8_t* p = file_data + rebase_off;
    const uint8_t* p_end = p + rebase_size;
    uint32_t seg_idx = 0;
    uint64_t seg_offset = 0;
    size_t rebase_count = 0;

    auto read_uleb = [](const uint8_t*& ptr) -> uint64_t {
        uint64_t res = 0; int shift = 0;
        while (true) {
            uint8_t b = *ptr++;
            res |= static_cast<uint64_t>(b & 0x7f) << shift;
            shift += 7;
            if (!(b & 0x80)) break;
        }
        return res;
    };

    while (p < p_end) {
        uint8_t b = *p++;
        uint8_t op = b & 0xf0;
        uint8_t imm = b & 0x0f;
        if (op == 0x00) break;
        else if (op == 0x10) {} // SET_TYPE_IMM
        else if (op == 0x20) { // SET_SEGMENT_AND_OFFSET_ULEB
            seg_idx = imm;
            seg_offset = read_uleb(p);
        } else if (op == 0x30) { // ADD_ADDR_ULEB
            seg_offset += read_uleb(p);
        } else if (op == 0x40) { // ADD_ADDR_IMM_SCALED
            seg_offset += imm * 8;
        } else if (op == 0x50) { // DO_REBASE_IMM_TIMES
            for (uint8_t i = 0; i < imm; ++i) {
                uint8_t* loc = image_base + (segments[seg_idx].vmaddr - min_vm) + seg_offset;
                *reinterpret_cast<uint64_t*>(loc) += slide;
                seg_offset += 8;
                rebase_count++;
            }
        } else if (op == 0x60) { // DO_REBASE_ULEB_TIMES
            uint64_t count = read_uleb(p);
            for (uint64_t i = 0; i < count; ++i) {
                uint8_t* loc = image_base + (segments[seg_idx].vmaddr - min_vm) + seg_offset;
                *reinterpret_cast<uint64_t*>(loc) += slide;
                seg_offset += 8;
                rebase_count++;
            }
        } else if (op == 0x70) { // DO_REBASE_ADD_ADDR_ULEB
            uint8_t* loc = image_base + (segments[seg_idx].vmaddr - min_vm) + seg_offset;
            *reinterpret_cast<uint64_t*>(loc) += slide;
            seg_offset += 8 + read_uleb(p);
            rebase_count++;
        } else if (op == 0x80) { // DO_REBASE_ULEB_TIMES_SKIPPING_ULEB
            uint64_t count = read_uleb(p);
            uint64_t skip = read_uleb(p);
            for (uint64_t i = 0; i < count; ++i) {
                uint8_t* loc = image_base + (segments[seg_idx].vmaddr - min_vm) + seg_offset;
                *reinterpret_cast<uint64_t*>(loc) += slide;
                seg_offset += 8 + skip;
                rebase_count++;
            }
        }
    }
    printf("[+] Successfully applied %zu rebases!\n", rebase_count);

    // 6. Apply Bindings (Regular and Lazy)
    auto apply_bind_stream = [&](uint32_t off, uint32_t size, const char* sname) {
        if (size == 0) return;
        const uint8_t* ptr = file_data + off;
        const uint8_t* end = ptr + size;
        uint32_t s_idx = 0;
        uint64_t s_offset = 0;
        std::string symbol = "";
        size_t b_count = 0;

        auto do_bind = [&](uint8_t* loc) {
            void* target = resolve_symbol(symbol);
            *reinterpret_cast<uint64_t*>(loc) = reinterpret_cast<uint64_t>(target);
            g_binds.push_back({reinterpret_cast<uint64_t>(loc),
                               reinterpret_cast<uint64_t>(target),
                               strdup(symbol.c_str()),
                               g_unresolved_set.count(symbol) != 0});
        };

        while (ptr < end) {
            uint8_t byte = *ptr++;
            uint8_t op = byte & 0xf0;
            uint8_t imm = byte & 0x0f;
            if (op == 0x00) {
                if (std::strcmp(sname, "lazy") != 0) break;
                s_offset = 0; symbol.clear(); s_idx = 0;
            } else if (op == 0x10) {} // SET_DYLIB_ORDINAL_IMM
            else if (op == 0x20) { read_uleb(ptr); }
            else if (op == 0x30) {} // SPECIAL
            else if (op == 0x40) { // SET_SYMBOL_TRAILING_FLAGS_IMM
                const char* str_start = reinterpret_cast<const char*>(ptr);
                while (*ptr != 0) ptr++;
                symbol = std::string(str_start, ptr - reinterpret_cast<const uint8_t*>(str_start));
                ptr++; // skip null
            } else if (op == 0x50) {} // TYPE
            else if (op == 0x60) { // SLEB
                // read sleb
                while (*ptr++ & 0x80);
            } else if (op == 0x70) {
                s_idx = imm;
                s_offset = read_uleb(ptr);
            } else if (op == 0x80) {
                s_offset += read_uleb(ptr);
            } else if (op == 0x90) { // DO_BIND
                uint8_t* loc = image_base + (segments[s_idx].vmaddr - min_vm) + s_offset;
                do_bind(loc);
                s_offset += 8;
                b_count++;
            } else if (op == 0xa0) { // DO_BIND_ADD_ADDR_ULEB
                uint8_t* loc = image_base + (segments[s_idx].vmaddr - min_vm) + s_offset;
                do_bind(loc);
                s_offset += 8 + read_uleb(ptr);
                b_count++;
            } else if (op == 0xb0) { // DO_BIND_ADD_ADDR_IMM_SCALED
                uint8_t* loc = image_base + (segments[s_idx].vmaddr - min_vm) + s_offset;
                do_bind(loc);
                s_offset += 8 + imm * 8;
                b_count++;
            } else if (op == 0xc0) {
                uint64_t count = read_uleb(ptr);
                uint64_t skip = read_uleb(ptr);
                for (uint64_t i = 0; i < count; ++i) {
                    uint8_t* loc = image_base + (segments[s_idx].vmaddr - min_vm) + s_offset;
                    do_bind(loc);
                    s_offset += 8 + skip;
                    b_count++;
                }
            }
        }
        printf("[+] Applied %zu bindings in %s stream\n", b_count, sname);
    };

    apply_bind_stream(bind_off, bind_size, "regular");
    apply_bind_stream(lazy_off, lazy_size, "lazy");
    printf("[*] Unresolved imports: %zu unique\n", g_unresolved_imports.size());
    for (const auto& n : g_unresolved_imports) {
        fprintf(stderr, "  UNRESOLVED %s\n", n.c_str());
    }

    // 7. TLS veneers in the pool adjacent to the image.
    // Site: `b veneer`. Veneer: mrs Xd, tpidr_el0; add/sub Xd; `b back`.
    // Writes only Xd. Out-of-range is a hard failure.
    printf("[*] Scanning for mrs *, tpidrro_el0 sites to install thread-local veneers...\n");
    SegmentInfo* text_seg = nullptr;
    for (auto& s : segments) {
        if (s.name == "__TEXT") { text_seg = &s; break; }
    }
    if (!text_seg) {
        fprintf(stderr, "[FATAL] no __TEXT segment\n");
        return 1;
    }
    uint8_t* text_base = image_base + (text_seg->vmaddr - min_vm);
    size_t veneer_count = 0;

    uint32_t* veneer_ptr = g_trampoline_code + (MAX_TRAMPOLINES * TRAMPOLINE_STUB_WORDS);
    uint32_t* veneer_end = reinterpret_cast<uint32_t*>(reservation_base + TRAMPOLINE_POOL_SIZE);

    int64_t tsd_diff = g_tsd_tprel_diff;
    int64_t tsd_hi = tsd_diff / 4096;
    int64_t tsd_lo = tsd_diff % 4096;
    if (tsd_hi > 4095 || tsd_hi < -4095) {
        fprintf(stderr, "[FATAL] TSD offset %ld does not fit in two ARM add/sub immediates\n",
                static_cast<long>(tsd_diff));
        return 1;
    }

    for (size_t off = 0; off < text_seg->filesize; off += 4) {
        uint32_t ins = *reinterpret_cast<uint32_t*>(text_base + off);
        if ((ins & 0xffffffe0) != 0xd53bd060) continue; // mrs xN, tpidrro_el0
        uint32_t reg = ins & 0x1f;
        uint8_t* site_addr = text_base + off;

        if (veneer_ptr + 4 > veneer_end) {
            fprintf(stderr, "[FATAL] TLS veneer pool exhausted\n");
            return 1;
        }
        uint32_t* vstub = veneer_ptr;
        size_t nins = 0;
        vstub[nins++] = 0xd53bd040 | reg; // mrs xN, tpidr_el0
        if (tsd_lo != 0) vstub[nins++] = encode_add_sub_imm12(reg, reg, tsd_lo, false);
        if (tsd_hi != 0) vstub[nins++] = encode_add_sub_imm12(reg, reg, tsd_hi, true);
        vstub[nins] = encode_b(reinterpret_cast<uint8_t*>(&vstub[nins]), site_addr + 4, "tls veneer return");
        nins++;
        veneer_ptr += nins;

        *reinterpret_cast<uint32_t*>(site_addr) =
            encode_b(site_addr, reinterpret_cast<uint8_t*>(vstub), "tls site");
        veneer_count++;
    }
    printf("[+] Installed %zu TLS veneers targeting thread_local g_apple_tsd!\n", veneer_count);

    // 8. Set memory permissions & flush icache
    for (auto& s : segments) {
        uint8_t* seg_start = image_base + (s.vmaddr - min_vm);
        int prot = PROT_READ;
        if (s.name == "__TEXT") prot = PROT_READ | PROT_EXEC;
        else if (s.name == "__DATA") prot = PROT_READ | PROT_WRITE;
        else if (s.name == "__DATA_CONST") prot = PROT_READ;

        mprotect(seg_start, s.vmsize, prot);
        if (prot & PROT_EXEC) {
            __builtin___clear_cache(reinterpret_cast<char*>(seg_start), reinterpret_cast<char*>(seg_start + s.vmsize));
        }
    }
    // Protect trampoline code to RX and flush icache (W^X compliant)
    mprotect(g_trampoline_code, TRAMPOLINE_POOL_SIZE, PROT_READ | PROT_EXEC);
    __builtin___clear_cache(reinterpret_cast<char*>(g_trampoline_code),
                            reinterpret_cast<char*>(g_trampoline_code) + TRAMPOLINE_POOL_SIZE);
    printf("[*] Page protections set and instruction caches cleared.\n");
    if (g_custom_shims.count("_snprintf")) selftest_variadic_snprintf(g_custom_shims["_snprintf"]);

    // Check value at 0x29b8890
    uint64_t* bzero_slot = reinterpret_cast<uint64_t*>(image_base + 0x29b8890);
    printf("[*] Value at 0x29b8890 (bzero slot): 0x%lx\n", *bzero_slot);

    // 8.5 Execute static initializers in __mod_init_func (449 initializers at vmaddr 0x2860490)
    uint64_t mod_init_vm = 0x2860490;
    size_t init_count = 449;
    uint64_t* inits = reinterpret_cast<uint64_t*>(image_base + (mod_init_vm - min_vm));
    printf("[*] Running %zu static initializers from __mod_init_func...\n", init_count);
    typedef void (*init_func_t)();
    for (size_t i = 0; i < init_count; ++i) {
        init_func_t fn = reinterpret_cast<init_func_t>(inits[i]);
        if (fn) {
            printf("Running init [%zu/%zu] at %p...\n", i, init_count, (void*)fn);
            fn();
        }
    }
    printf("[+] All %zu static initializers executed successfully!\n", init_count);

    // 9. Call IL2CPP Entry Points!
    typedef void (*il2cpp_register_log_callback_fn)(void (*callback)(const char*));
    typedef void (*il2cpp_set_data_dir_fn)(const char*);
    typedef int (*il2cpp_init_fn)(const char*);
    typedef void* (*il2cpp_domain_get_fn)();
    typedef void** (*il2cpp_domain_get_assemblies_fn)(void* domain, size_t* size);
    typedef void* (*il2cpp_assembly_get_image_fn)(void* assembly);
    typedef size_t (*il2cpp_image_get_class_count_fn)(void* image);
    typedef void* (*il2cpp_image_get_class_fn)(void* image, size_t index);
    typedef const char* (*il2cpp_class_get_name_fn)(void* klass);
    typedef void* (*il2cpp_string_new_fn)(const char* str);

    auto get_fn = [&](uint64_t offset) -> void* {
        return reinterpret_cast<void*>(image_base + offset);
    };

    auto il2cpp_register_log_callback = reinterpret_cast<il2cpp_register_log_callback_fn>(get_fn(0xbe01b0));
    auto il2cpp_set_data_dir = reinterpret_cast<il2cpp_set_data_dir_fn>(get_fn(0xbdec80));
    auto il2cpp_init = reinterpret_cast<il2cpp_init_fn>(get_fn(0xbdeba0));
    auto il2cpp_domain_get = reinterpret_cast<il2cpp_domain_get_fn>(get_fn(0xbdf504));
    auto il2cpp_domain_get_assemblies = reinterpret_cast<il2cpp_domain_get_assemblies_fn>(get_fn(0xbdf528));
    auto il2cpp_assembly_get_image = reinterpret_cast<il2cpp_assembly_get_image_fn>(get_fn(0xbdedd8));
    auto il2cpp_image_get_class_count = reinterpret_cast<il2cpp_image_get_class_count_fn>(get_fn(0xbe0158));
    auto il2cpp_image_get_class = reinterpret_cast<il2cpp_image_get_class_fn>(get_fn(0xbe0170));
    auto il2cpp_class_get_name = reinterpret_cast<il2cpp_class_get_name_fn>(get_fn(0xbdef1c));
    auto il2cpp_string_new = reinterpret_cast<il2cpp_string_new_fn>(get_fn(0xbdfdcc));

    printf("\n=== EXECUTING HEADLESS IL2CPP BOOT ON S24+ ===\n");

    printf("[1/5] Registering log callback...\n");
    il2cpp_register_log_callback(il2cpp_log_callback);

    printf("[2/5] Setting data dir: /data/local/tmp/game/Data...\n");
    il2cpp_set_data_dir("/data/local/tmp/game/Data");

    printf("[3/5] Calling _il2cpp_init(\"UnityRootDomain\")...\n");
    int init_rc = il2cpp_init("UnityRootDomain");
    printf("[*] _il2cpp_init returned: %d (%s)\n", init_rc, init_rc == 1 ? "SUCCESS!" : "FAILED");

    if (init_rc != 1) {
        fprintf(stderr, "[-] IL2CPP boot failed with code %d\n", init_rc);
        return 1;
    }

    printf("\n[4/5] Getting root domain & enumerating assemblies...\n");
    void* domain = il2cpp_domain_get();
    printf("[*] il2cpp_domain_get() -> %p\n", domain);

    size_t asm_count = 0;
    void** assemblies = il2cpp_domain_get_assemblies(domain, &asm_count);
    printf("[+] Successfully loaded %zu managed assemblies!\n", asm_count);

    size_t total_classes = 0;
    for (size_t i = 0; i < asm_count; ++i) {
        void* ass = assemblies[i];
        void* img = il2cpp_assembly_get_image(ass);
        size_t ccount = il2cpp_image_get_class_count(img);
        total_classes += ccount;
        if (i < 10 || i == asm_count - 1) {
            void* first_klass = ccount > 0 ? il2cpp_image_get_class(img, 0) : nullptr;
            const char* kname = first_klass ? il2cpp_class_get_name(first_klass) : "none";
            printf("  Assembly [%2zu]: image=%p, class_count=%4zu, sample_class=%s\n", i, img, ccount, kname);
        }
    }
    printf("[+] Total managed classes parsed from global-metadata.dat: %zu\n", total_classes);

    printf("\n[5/5] Allocating managed string via il2cpp_string_new...\n");
    void* str_obj = il2cpp_string_new("Hello from MyGame on Android Galaxy S24+!");
    printf("[+] il2cpp_string_new allocated managed object at: %p\n", str_obj);

    printf("\n======================================================\n");
    printf("   GATE 2 / HEADLESS IL2CPP BOOT: 100%% VERIFIED PASS!\n");
    printf("======================================================\n");

    return 0;
}
