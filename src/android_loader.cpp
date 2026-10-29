#include <atomic>
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
#include <link.h>
#include <pthread.h>
#include <semaphore.h>
#include <mutex>
#include <time.h>
#include <stdarg.h>
#include <dirent.h>
#include <ucontext.h>
#include <sys/ioctl.h>

#include "shim_types.hpp"
#include "guest_fde.hpp"
#include "objc_runtime.hpp"
#include "gles_backend.hpp"

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
    int64_t addend;
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

// Return addresses saved by PAC-enabled system libraries are signed (extra bits above bit 38 on
// this 39-bit-VA device). Code addresses never carry a tag byte, so strip bits >= 39 when the top
// byte is zero.
static uint64_t strip_pac(uint64_t a) {
    if ((a >> 39) != 0 && (a >> 56) == 0) return a & 0x7fffffffffULL;
    return a;
}

// Prints "[kind] detail" for an address. kind: image / tramp / loader / libc / libc++ / other.
// Returns 1 for guest image, 2 for trampoline, 3 for loader, 4 for libc, 0 otherwise.
static int describe_addr(int fd, uint64_t a, bool is_return_addr = false) {
    {
        uint64_t s = strip_pac(a);
        if (s != a) {
            wr_str(fd, "(pac-signed, stripped to "); wr_hex(fd, s); wr_str(fd, ") ");
            a = s;
        }
    }
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
    // A return address can sit exactly on the next function's first byte; look up the call site.
    if (a && dladdr(reinterpret_cast<void*>(is_return_addr ? a - 1 : a), &di) && di.dli_fname) {
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
    wr_str(fd, "  #lr  "); wr_hex(fd, lr); wr_str(fd, " "); describe_addr(fd, lr, true); wr_str(fd, "\n");
    int first_guest = -1;
    uint64_t guest_ret = 0;
    if (in_image(strip_pac(lr))) { first_guest = -2; guest_ret = strip_pac(lr); }
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
        int k = describe_addr(fd, rec[1], true);
        wr_str(fd, "\n");
        if (k == 1 && first_guest == -1) { first_guest = depth; guest_ret = strip_pac(rec[1]); }
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

// Darwin's pthread_key_t is a 64-bit unsigned long; bionic's is a 32-bit int. The guest's key variable is 8 bytes, so
// write all of it (a 4-byte store leaves garbage in the upper half and key*8 becomes a wild TSD index).
extern "C" int native_pthread_key_create(uint64_t* key, void (*destructor)(void*)) {
    if (!key) return EINVAL;
    *key = static_cast<uint64_t>(g_next_apple_key++);
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

// Root of the app bundle on the device (UnityFramework, Info.plist, Data/). LOADER_GAME_DIR overrides the default.
static const char* game_dir() {
    static const std::string d = getenv("LOADER_GAME_DIR") ? getenv("LOADER_GAME_DIR") : "/data/local/tmp/game";
    return d.c_str();
}
static const std::string& game_binary_path() {
    static const std::string p = std::string(game_dir()) + "/UnityFramework";
    return p;
}

extern "C" int native_NSGetExecutablePath(char* buf, uint32_t* bufsize) {
    const char* path = game_binary_path().c_str();
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
    if (image_index == 0) return game_binary_path().c_str();
    return nullptr;
}

extern "C" void native_dyld_register_func_for_add_image(void (*func)(const void* mh, intptr_t vmaddr_slide)) {
    if (func && g_loaded_image_base) {
        func(g_loaded_image_base, g_loaded_image_slide);
    }
}

extern "C" void native_dyld_register_func_for_remove_image(void (*func)(const void* mh, intptr_t vmaddr_slide)) {}

// Apple Grand Central Dispatch (GCD) emulation
// dispatch_semaphore_t is an opaque heap object here: a POSIX semaphore. Timeouts are dispatch_time() values, which
// native_dispatch_time encodes as absolute CLOCK_MONOTONIC nanoseconds (DISPATCH_TIME_FOREVER = ~0).
extern "C" void* native_dispatch_semaphore_create(long value) {
    if (value < 0) return nullptr;
    auto* s = new sem_t;
    sem_init(s, 0, static_cast<unsigned>(value));
    return s;
}

extern "C" long native_dispatch_semaphore_wait(void* dsema, uint64_t timeout) {
    auto* s = static_cast<sem_t*>(dsema);
    if (!s) return 0;
    if (timeout == ~0ull) {
        while (sem_wait(s) != 0 && errno == EINTR) {}
        return 0;
    }
    timespec deadline{static_cast<time_t>(timeout / 1000000000ull), static_cast<long>(timeout % 1000000000ull)};
    for (;;) {
        if (sem_clockwait(s, CLOCK_MONOTONIC, &deadline) == 0) return 0;
        if (errno != EINTR) return 1;   // timed out
    }
}

extern "C" long native_dispatch_semaphore_signal(void* dsema) {
    auto* s = static_cast<sem_t*>(dsema);
    if (!s) return 0;
    int waiting = 0;
    sem_getvalue(s, &waiting);
    sem_post(s);
    return waiting < 0 ? 1 : 0;   // non-zero when a waiter was woken
}

extern "C" void* native_dispatch_queue_create(const char* label, void* attr) {
    static uint64_t g_queue_id = 0x2000;
    return reinterpret_cast<void*>(++g_queue_id);
}

extern "C" void* native_dispatch_get_global_queue(long identifier, unsigned long flags) {
    static uint64_t g_gqueue = 0x3000;
    return reinterpret_cast<void*>(g_gqueue);
}

// Block_layout: isa, int flags, int reserved, invoke, descriptor{reserved, size, copy_helper, dispose_helper}
static void* heap_block_copy(void* block) {
    if (!block) return nullptr;
    int32_t flags = *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(block) + 8);
    if (flags & ((1 << 28) | (1 << 24))) return block;   // BLOCK_IS_GLOBAL, or already on the heap (BLOCK_NEEDS_FREE)
    uint64_t* desc = *reinterpret_cast<uint64_t**>(static_cast<uint8_t*>(block) + 24);
    size_t size = desc ? static_cast<size_t>(desc[1]) : 32;
    void* copy = malloc(size < 32 ? 32 : size);
    std::memcpy(copy, block, size);
    *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(copy) + 8) |= 1 << 24;
    if (desc && (flags & (1 << 25)) && desc[2])   // BLOCK_HAS_COPY_DISPOSE: retain captured objects/__block vars
        reinterpret_cast<void (*)(void*, void*)>(desc[2])(copy, block);
    return copy;
}
extern "C" void native_dispatch_run_on_main(void* block);   // fake_uikit.hpp run loop
// Every queue is served by the main run loop: work runs in submission order on the engine's main thread.
extern "C" void native_dispatch_async(void* queue, void* block) { native_dispatch_run_on_main(heap_block_copy(block)); }
extern "C" void native_dispatch_sync(void* queue, void* block) {
    if (!block) return;
    auto invoke = *reinterpret_cast<void (**)(void*)>(static_cast<uint8_t*>(block) + 16);
    if (invoke) invoke(block);
}
extern "C" void native_dispatch_release(void* obj) {}

// Engine-mode pthread_create wrapper: logs the result and the lifetime of every guest thread.
struct MachThread;
static MachThread* mach_register_current();
static void mach_unregister_current();
struct GuestThreadStart { void* (*fn)(void*); void* arg; int id; };
static std::atomic<int> g_thread_seq{0};
static void* guest_thread_tramp(void* p) {
    GuestThreadStart st = *static_cast<GuestThreadStart*>(p);
    free(p);
    static const bool log = getenv("LOADER_LOG_THREADS") != nullptr;
    mach_register_current();
    if (log) fprintf(stderr, "[THREADS] #%d started: fn=%p arg=%p\n", st.id, reinterpret_cast<void*>(st.fn), st.arg);
    void* r = st.fn(st.arg);
    if (log) fprintf(stderr, "[THREADS] #%d returned %p\n", st.id, r);
    mach_unregister_current();
    return r;
}

// ---------------------------------------------------------------------------
// Mach thread emulation (engine mode). IL2CPP's Boehm GC stops the world on Apple platforms with task_threads /
// thread_suspend / thread_get_state / thread_resume. Here a thread's "port" is its tid; suspend is a realtime signal
// whose handler records the interrupted registers and parks the thread until resume. Not SIGUSR1: app processes
// inherit it blocked from zygote (ART's signal catcher owns it), so a suspend would never be delivered.
// ---------------------------------------------------------------------------
#include <semaphore.h>
#include <sys/syscall.h>
struct MachThread {
    uint32_t port = 0;
    pid_t tid = 0;
    pthread_t pt{};
    sem_t resume;
    volatile int suspended = 0;
    int suspend_count = 0;
    uint64_t regs[31] = {};
    uint64_t sp = 0, pc = 0;
    uint32_t cpsr = 0;
};
static std::mutex g_mt_mu;
static std::vector<MachThread*> g_mt;
static thread_local MachThread* t_mach_self = nullptr;
static bool g_mt_signal_installed = false;
static int mach_suspend_signal() { return SIGRTMIN + 3; }

static void mach_suspend_handler(int, siginfo_t*, void* ctx) {
    MachThread* me = t_mach_self;
    if (!me) return;
    ucontext_t* uc = reinterpret_cast<ucontext_t*>(ctx);
    for (int i = 0; i < 31; ++i) me->regs[i] = uc->uc_mcontext.regs[i];
    me->sp = uc->uc_mcontext.sp;
    me->pc = uc->uc_mcontext.pc;
    me->cpsr = static_cast<uint32_t>(uc->uc_mcontext.pstate);
    me->suspended = 1;
    while (sem_wait(&me->resume) != 0 && errno == EINTR) {}
    me->suspended = 0;
}
static MachThread* mach_register_current() {
    if (t_mach_self) return t_mach_self;
    auto* m = new MachThread();
    m->tid = static_cast<pid_t>(syscall(SYS_gettid));
    m->port = static_cast<uint32_t>(m->tid);
    m->pt = pthread_self();
    sem_init(&m->resume, 0, 0);
    t_mach_self = m;
    sigset_t unblock;
    sigemptyset(&unblock);
    sigaddset(&unblock, mach_suspend_signal());
    pthread_sigmask(SIG_UNBLOCK, &unblock, nullptr);
    std::lock_guard<std::mutex> l(g_mt_mu);
    if (!g_mt_signal_installed) {
        struct sigaction sa{};
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sa.sa_sigaction = mach_suspend_handler;
        sigemptyset(&sa.sa_mask);
        sigaction(mach_suspend_signal(), &sa, nullptr);
        g_mt_signal_installed = true;
    }
    g_mt.push_back(m);
    return m;
}
static void mach_unregister_current() {
    if (!t_mach_self) return;
    std::lock_guard<std::mutex> l(g_mt_mu);
    for (size_t i = 0; i < g_mt.size(); ++i) if (g_mt[i] == t_mach_self) { g_mt.erase(g_mt.begin() + i); break; }
    t_mach_self = nullptr;   // MachThread leaks on purpose: a late signal must never touch freed memory
}
static MachThread* mach_find(uint32_t port) {
    std::lock_guard<std::mutex> l(g_mt_mu);
    for (MachThread* m : g_mt) if (m->port == port) return m;
    return nullptr;
}
extern "C" {
uint32_t native_mach_thread_self_real() { return mach_register_current()->port; }
uint32_t native_pthread_mach_thread_np_real(pthread_t t) {
    if (pthread_equal(t, pthread_self())) return mach_register_current()->port;
    std::lock_guard<std::mutex> l(g_mt_mu);
    for (MachThread* m : g_mt) if (pthread_equal(m->pt, t)) return m->port;
    return 0;
}
int native_task_threads(uint32_t, uint32_t** list, uint32_t* count) {
    mach_register_current();
    std::lock_guard<std::mutex> l(g_mt_mu);
    auto* arr = static_cast<uint32_t*>(malloc(sizeof(uint32_t) * (g_mt.size() ? g_mt.size() : 1)));
    for (size_t i = 0; i < g_mt.size(); ++i) arr[i] = g_mt[i]->port;
    *list = arr;
    *count = static_cast<uint32_t>(g_mt.size());
    return 0;
}
int native_thread_suspend(uint32_t port) {
    MachThread* m = mach_find(port);
    if (!m || m == t_mach_self) {
        std::lock_guard<std::mutex> l(g_mt_mu);
        fprintf(stderr, "[MACH] thread_suspend(port=%u) -> %s; registered:", port, !m ? "UNKNOWN PORT" : "self");
        for (MachThread* t : g_mt) fprintf(stderr, " %u", t->port);
        fprintf(stderr, " (caller tid %ld)\n", static_cast<long>(syscall(SYS_gettid)));
        return 4;   // KERN_INVALID_ARGUMENT
    }
    bool signal_it;
    {
        std::lock_guard<std::mutex> l(g_mt_mu);
        signal_it = (m->suspend_count++ == 0);
    }
    if (!signal_it) return 0;
    syscall(SYS_tgkill, getpid(), m->tid, mach_suspend_signal());
    for (int i = 0; i < 40000 && !m->suspended; ++i) usleep(50);   // up to 2 s
    if (!m->suspended) fprintf(stderr, "[MACH] thread_suspend(port=%u): thread did not acknowledge the suspend signal within 2 s\n", port);
    return m->suspended ? 0 : 5;
}
int native_thread_resume(uint32_t port) {
    MachThread* m = mach_find(port);
    if (!m) return 4;
    bool post = false;
    {
        std::lock_guard<std::mutex> l(g_mt_mu);
        if (m->suspend_count > 0 && --m->suspend_count == 0) post = true;
    }
    if (post) sem_post(&m->resume);
    return 0;
}
// flavor 6 = ARM_THREAD_STATE64: x[29], fp, lr, sp, pc (8 bytes each), cpsr, pad (4 each) = 68 words
int native_thread_get_state(uint32_t port, int flavor, uint32_t* state, uint32_t* count) {
    MachThread* m = mach_find(port);
    if (!m || flavor != 6 || !state || !count || *count < 68) return 5;
    if (!m->suspended && m != t_mach_self) return 5;
    uint64_t* w = reinterpret_cast<uint64_t*>(state);
    if (m == t_mach_self) {   // querying the caller itself: approximate with current sp
        std::memset(state, 0, 68 * 4);
        w[31] = reinterpret_cast<uint64_t>(__builtin_frame_address(0));
    } else {
        for (int i = 0; i < 29; ++i) w[i] = m->regs[i];
        w[29] = m->regs[29];   // fp
        w[30] = m->regs[30];   // lr
        w[31] = m->sp;
        w[32] = m->pc;
        state[66] = m->cpsr;
        state[67] = 0;
    }
    *count = 68;
    return 0;
}
}

extern "C" int native_pthread_create_logged(pthread_t* t, const pthread_attr_t* attr, void* (*fn)(void*), void* arg) {
    auto* st = static_cast<GuestThreadStart*>(malloc(sizeof(GuestThreadStart)));
    st->fn = fn; st->arg = arg; st->id = ++g_thread_seq;
    // iOS lets threads ask for SCHED_RR; an unprivileged Android process gets EPERM and the thread is never created
    // (Unity's job workers!). Keep the guest's stack size etc. but drop explicit scheduling.
    pthread_attr_t a2;
    if (attr) std::memcpy(&a2, attr, sizeof a2); else pthread_attr_init(&a2);
    sched_param sp{};
    pthread_attr_setinheritsched(&a2, PTHREAD_INHERIT_SCHED);
    pthread_attr_setschedpolicy(&a2, SCHED_OTHER);
    pthread_attr_setschedparam(&a2, &sp);
    int rc = pthread_create(t, &a2, guest_thread_tramp, st);
    static const bool log = getenv("LOADER_LOG_THREADS") != nullptr;
    if (log || rc) fprintf(stderr, "[THREADS] pthread_create #%d fn=%p -> rc=%d%s\n", st->id, reinterpret_cast<void*>(fn), rc, rc ? " (FAILED)" : "");
    if (rc) free(st);
    return rc;
}

// Guest dlopen/dlsym: Apple frameworks resolve to a fake handle; MTLCreateSystemDefaultDevice returns a fake device.
static char g_fake_framework_handle[16];
extern "C" void* native_mtl_create_system_default_device();
extern "C" void* native_guest_dlopen(const char* path, int flags) {
    if (path && (std::strstr(path, "/System/Library/") || std::strstr(path, ".framework"))) {
        fprintf(stderr, "[DLOPEN] fake framework handle for %s\n", path);
        return g_fake_framework_handle;
    }
    return dlopen(path, RTLD_NOW);
}
extern "C" void* native_guest_dlsym(void* handle, const char* name) {
    if (handle == static_cast<void*>(g_fake_framework_handle)) {
        fprintf(stderr, "[DLSYM] fake framework: %s\n", name ? name : "(null)");
        if (name && std::strcmp(name, "MTLCreateSystemDefaultDevice") == 0)
            return reinterpret_cast<void*>(&native_mtl_create_system_default_device);
        return nullptr;
    }
    if (name && std::strcmp(name, "MTLCreateSystemDefaultDevice") == 0)
        return reinterpret_cast<void*>(&native_mtl_create_system_default_device);
    // Darwin RTLD_NEXT=-1, RTLD_DEFAULT=-2, RTLD_SELF=-3, RTLD_MAIN_ONLY=-5
    intptr_t h = reinterpret_cast<intptr_t>(handle);
    if (h == -2 || h == -3 || h == -5) handle = RTLD_DEFAULT;
    else if (h == -1) handle = RTLD_NEXT;
    return dlsym(handle, name);
}

// ---------------------------------------------------------------------------
// Batch of small Darwin libc/libSystem replacements (natives: plain C ABI, bound directly).
// ---------------------------------------------------------------------------
#include <cmath>
#include <cctype>
extern "C" int native_tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
extern "C" int native_toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
// Darwin <runetype.h> class bits; ASCII only (no locale tables).
extern "C" int native_maskrune(uint32_t c, unsigned long mask) {
    if (c >= 128) return 0;
    unsigned long r = 0;
    int ch = static_cast<int>(c);
    if (isalpha(ch)) r |= 0x100;       // _CTYPE_A
    if (iscntrl(ch)) r |= 0x200;       // _CTYPE_C
    if (isdigit(ch)) r |= 0x400;       // _CTYPE_D
    if (isgraph(ch)) r |= 0x800;       // _CTYPE_G
    if (islower(ch)) r |= 0x1000;      // _CTYPE_L
    if (isprint(ch)) r |= 0x40000;     // _CTYPE_R
    if (ispunct(ch)) r |= 0x2000;      // _CTYPE_P
    if (isspace(ch)) r |= 0x4000;      // _CTYPE_S
    if (isupper(ch)) r |= 0x8000;      // _CTYPE_U
    if (isxdigit(ch)) r |= 0x10000;    // _CTYPE_X
    if (ch == ' ' || ch == '\t') r |= 0x20000;  // _CTYPE_B (blank)
    return (r & mask) != 0;
}
struct ScD { double s, c; };
struct ScF { float s, c; };
extern "C" ScD native_sincos_stret(double x) { return {sin(x), cos(x)}; }
extern "C" ScF native_sincosf_stret(float x) { return {sinf(x), cosf(x)}; }
extern "C" float native_exp10f(float x) { return powf(10.0f, x); }
extern "C" int native_atexit(void (*fn)(void)) { (void)fn; return 0; }
extern "C" uint64_t native_clock_gettime_nsec_np(int darwin_clock) {
    clockid_t id = CLOCK_MONOTONIC;
    switch (darwin_clock) {  // Darwin clockid_t -> Linux
        case 0: id = CLOCK_REALTIME; break;
        case 4: case 8: id = CLOCK_MONOTONIC_RAW; break;  // MONOTONIC_RAW, UPTIME_RAW
        case 6: id = CLOCK_MONOTONIC; break;
        case 12: id = CLOCK_PROCESS_CPUTIME_ID; break;
        case 16: id = CLOCK_THREAD_CPUTIME_ID; break;
        default: break;
    }
    timespec ts;
    if (clock_gettime(id, &ts) != 0) return 0;
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}
// dispatch_once: predicate is a Darwin intptr; 0 = not run, ~0 = done. A Block's invoke pointer sits at +16.
static pthread_mutex_t g_once_mutex = PTHREAD_MUTEX_INITIALIZER;
extern "C" void native_dispatch_once_f(intptr_t* pred, void* ctx, void (*fn)(void*)) {
    if (__atomic_load_n(pred, __ATOMIC_ACQUIRE) == -1) return;
    pthread_mutex_lock(&g_once_mutex);
    bool run = (*pred != -1);
    if (run) *pred = -1;  // mark first so recursive requests on this thread do not deadlock/loop
    pthread_mutex_unlock(&g_once_mutex);
    if (run) fn(ctx);
}
extern "C" void native_dispatch_once(intptr_t* pred, void* block) {
    if (__atomic_load_n(pred, __ATOMIC_ACQUIRE) == -1) return;
    pthread_mutex_lock(&g_once_mutex);
    bool run = (*pred != -1);
    if (run) *pred = -1;
    pthread_mutex_unlock(&g_once_mutex);
    if (run) reinterpret_cast<void (*)(void*)>(*reinterpret_cast<void**>(static_cast<char*>(block) + 16))(block);
}
extern "C" void native_assert_rtn(const char* func, const char* file, int line, const char* expr) {
    fprintf(stderr, "[ASSERT] %s:%d: %s: Assertion `%s' failed.\n", file ? file : "?", line, func ? func : "?", expr ? expr : "?");
    fflush(stderr);
    abort();
}
static void* heap_block_copy(void* block);
// Captured-variable copy for block copy helpers. Flags: 3 object, 7 block, 8 __block byref, 128 byref caller.
// Byref layout: isa, forwarding, int flags, int size, [keep, dispose when flags has 1<<25], variables...
extern "C" void native_block_object_assign(void* dst, const void* src, int flags) {
    void** out = static_cast<void**>(dst);
    if ((flags & 128) || !src) { *out = const_cast<void*>(src); return; }
    if ((flags & 0x1f) == 7) { *out = heap_block_copy(const_cast<void*>(src)); return; }
    if ((flags & 0x1f) == 8) {
        auto* byref = static_cast<uint8_t*>(const_cast<void*>(src));
        uint8_t* fwd = *reinterpret_cast<uint8_t**>(byref + 8);
        int32_t bflags = *reinterpret_cast<int32_t*>(fwd + 16);
        if (bflags & (1 << 24)) { *out = fwd; return; }   // already moved to the heap
        int32_t size = *reinterpret_cast<int32_t*>(byref + 20);
        auto* copy = static_cast<uint8_t*>(malloc(size < 32 ? 32 : size));
        std::memcpy(copy, byref, size);
        *reinterpret_cast<uint8_t**>(copy + 8) = copy;
        *reinterpret_cast<uint8_t**>(byref + 8) = copy;
        *reinterpret_cast<int32_t*>(copy + 16) |= 1 << 24;
        if (bflags & (1 << 25)) {
            auto keep = *reinterpret_cast<void (**)(void*, void*)>(byref + 24);
            if (keep) keep(copy, byref);
        }
        *out = copy;
        return;
    }
    *out = const_cast<void*>(src);
}
extern "C" void native_block_object_dispose(const void* obj, int flags) { (void)obj; (void)flags; }
// Mach / exception-port / crash-reporter calls: report KERN_FAILURE (5) so the caller takes its error path.
extern "C" int native_kern_failure() { return 5; }
extern "C" void* native_null() { return nullptr; }
extern "C" void* native_dispatch_fake_handle() { return calloc(1, 64); }
// dispatch_time: absolute CLOCK_MONOTONIC ns. DISPATCH_TIME_NOW (0) means "now"; DISPATCH_TIME_FOREVER stays ~0.
extern "C" uint64_t native_dispatch_time(uint64_t when, int64_t delta) {
    if (when == ~0ull) return ~0ull;
    if (when == 0) {
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        when = static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
    }
    return delta < 0 && static_cast<uint64_t>(-delta) > when ? 0 : when + static_cast<uint64_t>(delta);
}
extern "C" int native_cond_timedwait_relative_np(pthread_cond_t* cond, pthread_mutex_t* mutex, const timespec* rel) {
    timespec abs;
    clock_gettime(CLOCK_REALTIME, &abs);
    if (rel) {
        abs.tv_sec += rel->tv_sec;
        abs.tv_nsec += rel->tv_nsec;
        if (abs.tv_nsec >= 1000000000L) { abs.tv_sec += 1; abs.tv_nsec -= 1000000000L; }
    }
    int rc = pthread_cond_timedwait(cond, mutex, &abs);
    return rc == ETIMEDOUT ? 60 : rc;   // Darwin ETIMEDOUT
}
// ObjC exception personality: C++ exceptions never need to be caught by ObjC frames; keep unwinding.
extern "C" int native_objc_personality(int, int, uint64_t, void*, void*) { return 8; /* _URC_CONTINUE_UNWIND */ }
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

// Memory allocator shims. Darwin's malloc_size() answers 0 for memory malloc did not hand out, and Unity relies on
// that (its malloc allocator's Contains() is `malloc_size(p) != 0`, probed with other allocators' pointers). Scudo's
// malloc_usable_size() aborts on such pointers, so the blocks the guest gets from its malloc family are tracked.
namespace guest_heap {
constexpr size_t kShards = 64;
struct Shard { std::mutex mu; std::unordered_set<uintptr_t> live; };
inline Shard g_shards[kShards];
inline Shard& shard(const void* p) { return g_shards[(reinterpret_cast<uintptr_t>(p) >> 4) % kShards]; }
inline void add(void* p) { if (p) { Shard& s = shard(p); std::lock_guard<std::mutex> l(s.mu); s.live.insert(reinterpret_cast<uintptr_t>(p)); } }
inline void remove(void* p) { if (p) { Shard& s = shard(p); std::lock_guard<std::mutex> l(s.mu); s.live.erase(reinterpret_cast<uintptr_t>(p)); } }
inline bool owns(const void* p) { Shard& s = shard(p); std::lock_guard<std::mutex> l(s.mu); return s.live.count(reinterpret_cast<uintptr_t>(p)) != 0; }
}  // namespace guest_heap
extern "C" void* native_guest_malloc(size_t n) { void* p = malloc(n); guest_heap::add(p); return p; }
extern "C" void* native_guest_calloc(size_t n, size_t m) { void* p = calloc(n, m); guest_heap::add(p); return p; }
extern "C" void* native_guest_realloc(void* old, size_t n) {
    if (old) guest_heap::remove(old);
    void* p = realloc(old, n);
    guest_heap::add(p ? p : (n ? old : nullptr));   // failed realloc keeps the old block
    return p;
}
extern "C" void native_guest_free(void* p) { guest_heap::remove(p); free(p); }
extern "C" int native_guest_posix_memalign(void** out, size_t align, size_t n) {
    int rc = posix_memalign(out, align, n);
    if (rc == 0) guest_heap::add(*out);
    return rc;
}
extern "C" size_t native_malloc_size(const void* ptr) {
    if (!ptr || !guest_heap::owns(ptr)) return 0;
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

// Mach semaphores (Unity's PlatformSemaphore on Apple: job system and render-thread command queue). semaphore_t is a
// 32-bit port name; it indexes a table of POSIX semaphores. Destroyed slots are kept so a late signal stays harmless.
static std::mutex g_mach_sem_mu;
static std::vector<sem_t*> g_mach_sems;
static sem_t* mach_sem(uint32_t name) {
    std::lock_guard<std::mutex> l(g_mach_sem_mu);
    uint32_t i = name - 0x5000;
    return name >= 0x5000 && i < g_mach_sems.size() ? g_mach_sems[i] : nullptr;
}
extern "C" int native_semaphore_create(uint32_t, uint32_t* sem, int, int value) {
    auto* s = new sem_t;
    sem_init(s, 0, static_cast<unsigned>(value < 0 ? 0 : value));
    std::lock_guard<std::mutex> l(g_mach_sem_mu);
    g_mach_sems.push_back(s);
    if (sem) *sem = 0x5000 + static_cast<uint32_t>(g_mach_sems.size() - 1);
    return 0;
}
extern "C" int native_semaphore_destroy(uint32_t, uint32_t sem) { return mach_sem(sem) ? 0 : 15; }   // KERN_INVALID_NAME
extern "C" int native_semaphore_signal(uint32_t sem) {
    sem_t* s = mach_sem(sem);
    if (!s) return 15;
    sem_post(s);
    return 0;
}
extern "C" int native_semaphore_wait(uint32_t sem) {
    sem_t* s = mach_sem(sem);
    if (!s) return 15;
    while (sem_wait(s) != 0 && errno == EINTR) {}
    return 0;
}

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
// Darwin setjmp family (assembly in trampolines.s; 192-byte guest jmp_buf)
// ---------------------------------------------------------------------------
extern "C" int darwin_setjmp(void* buf) __attribute__((returns_twice));
extern "C" void darwin_longjmp(void* buf, int val) __attribute__((noreturn));
extern "C" int darwin_jmp_fp_selftest(void* buf);

struct alignas(16) JmpTest {
    int buf[48];                 // Darwin-sized buffer
    uint64_t guard[8];           // detects any write past 192 bytes
};

static __attribute__((noinline)) void jmp_nested(int depth, JmpTest* t, volatile int* trail) {
    trail[depth] = 1;
    if (depth == 0) darwin_longjmp(t->buf, 7);
    jmp_nested(depth - 1, t, trail);
    trail[depth] = 2;  // must never run
}

static bool selftest_setjmp() {
    static JmpTest t;
    for (auto& g : t.guard) g = 0xA5A5A5A5A5A5A5A5ull;
    volatile int trail[8] = {0};
    volatile int stage = 0;
    bool ok = true;
    int rc = darwin_setjmp(t.buf);
    if (rc == 0) {
        stage = 1;
        jmp_nested(5, &t, trail);
        stage = 99;  // not reached
    } else {
        ok = ok && rc == 7 && stage == 1;
        for (int i = 0; i <= 5; ++i) ok = ok && trail[i] == 1;  // all frames entered, none returned
    }
    for (auto g : t.guard) ok = ok && g == 0xA5A5A5A5A5A5A5A5ull;
    // longjmp(buf, 0) must return 1
    volatile int second = 0;
    int rc2 = darwin_setjmp(t.buf);
    if (rc2 == 0) { second = 1; darwin_longjmp(t.buf, 0); }
    ok = ok && rc2 == 1 && second == 1;
    wr_str(2, "[SELFTEST] Darwin setjmp/longjmp (nested depth 5, val 7 and val 0->1, 192-byte guard) ");
    wr_str(2, ok ? "PASS\n" : "FAIL\n");
    // d8-d15: set known values, setjmp, zero them, longjmp, verify restored (in assembly).
    static JmpTest fp;
    for (auto& g : fp.guard) g = 0xA5A5A5A5A5A5A5A5ull;
    bool fp_ok = darwin_jmp_fp_selftest(fp.buf) == 1;
    for (auto g : fp.guard) fp_ok = fp_ok && g == 0xA5A5A5A5A5A5A5A5ull;
    wr_str(2, "[SELFTEST] Darwin setjmp/longjmp restores d8-d15 (clobbered to 0 between) ");
    wr_str(2, fp_ok ? "PASS\n" : "FAIL\n");
    return ok && fp_ok;
}

// ---------------------------------------------------------------------------
// open / openat / fcntl / ioctl (Darwin constants + variadic args)
// ---------------------------------------------------------------------------
static std::string remap_game_path(const char* path) {
    std::string p = path ? path : "";
    if (p.find("Data/") != std::string::npos || p.find("Managed/") != std::string::npos) {
        size_t idx = p.find("Data/");
        if (idx != std::string::npos) {
            p = std::string(game_dir()) + "/" + p.substr(idx);
        } else {
            p = std::string(game_dir()) + "/Data/" + p;
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
// Darwin PTHREAD_CREATE_JOINABLE 1 / DETACHED 2 (bionic 0 / 1).
extern "C" int native_pthread_attr_setdetachstate(pthread_attr_t* a, int s) {
    return pthread_attr_setdetachstate(a, s == 2 ? PTHREAD_CREATE_DETACHED : PTHREAD_CREATE_JOINABLE);
}
// Darwin SCHED_OTHER 1 / FIFO 4 / RR 2. Real-time policies are refused for unprivileged Android processes and
// native_pthread_create_logged drops them anyway, so the request is accepted and recorded as SCHED_OTHER.
extern "C" int native_pthread_attr_setschedpolicy(pthread_attr_t* a, int p) {
    if (p != 1 && p != 2 && p != 4) return EINVAL;
    pthread_attr_setschedpolicy(a, SCHED_OTHER);
    return 0;
}
extern "C" int native_pthread_attr_setschedparam(pthread_attr_t*, const void*) { return 0; }

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

// Darwin sysctl answers for the emulated device (iPhone 12 Pro Max on iOS 16.0, matching the fake UIDevice).
// kind: 's' = NUL-terminated string, 'i' = int32, 'q' = int64, 'z' = zero-filled struct of `size` bytes,
// 'b' = struct timeval of the host boot time.
struct DarwinSysctl { const char* name; int mib0, mib1; char kind; const char* s; int64_t v; size_t size; };
static const DarwinSysctl kDarwinSysctls[] = {
    {"hw.machine", 6, 1, 's', "iPhone13,4", 0, 0},
    {"hw.model", 6, 2, 's', "D54pAP", 0, 0},
    {"hw.ncpu", 6, 3, 'i', nullptr, 8, 0},
    {"hw.byteorder", 6, 4, 'i', nullptr, 1234, 0},
    {"hw.physmem", 6, 5, 'i', nullptr, 0x7fffffff, 0},
    {"hw.pagesize", 6, 7, 'q', nullptr, 4096, 0},   // host page size, same as _vm_page_size
    {"hw.cpufrequency", 6, 15, 'q', nullptr, 3000000000LL, 0},
    {"hw.cachelinesize", 6, 16, 'q', nullptr, 64, 0},
    {"hw.l1icachesize", 6, 17, 'q', nullptr, 65536, 0},
    {"hw.l1dcachesize", 6, 18, 'q', nullptr, 65536, 0},
    {"hw.l2cachesize", 6, 20, 'q', nullptr, 4194304, 0},
    {"hw.memsize", 6, 24, 'q', nullptr, 6LL * 1024 * 1024 * 1024, 0},
    {"hw.availcpu", 6, 25, 'i', nullptr, 8, 0},
    {"hw.physicalcpu", -1, -1, 'i', nullptr, 8, 0},
    {"hw.physicalcpu_max", -1, -1, 'i', nullptr, 8, 0},
    {"hw.logicalcpu", -1, -1, 'i', nullptr, 8, 0},
    {"hw.logicalcpu_max", -1, -1, 'i', nullptr, 8, 0},
    {"hw.activecpu", -1, -1, 'i', nullptr, 8, 0},
    {"hw.cputype", -1, -1, 'i', nullptr, 0x0100000c, 0},
    {"hw.cpusubtype", -1, -1, 'i', nullptr, 2, 0},
    {"hw.optional.neon", -1, -1, 'i', nullptr, 1, 0},
    {"hw.optional.floatingpoint", -1, -1, 'i', nullptr, 1, 0},
    {"hw.optional.armv8_crc32", -1, -1, 'i', nullptr, 1, 0},
    {"kern.ostype", 1, 1, 's', "Darwin", 0, 0},
    {"kern.osrelease", 1, 2, 's', "22.0.0", 0, 0},
    {"kern.version", 1, 4, 's', "Darwin Kernel Version 22.0.0", 0, 0},
    {"kern.hostname", 1, 10, 's', "iPhone", 0, 0},
    {"kern.osversion", 1, 65, 's', "20A362", 0, 0},
    {"kern.proc", 1, 14, 'z', nullptr, 0, 648},   // struct kinfo_proc; all-zero = not traced
    {"kern.boottime", 1, 21, 'b', nullptr, 0, 0},
};

// Size query (oldp == NULL) reports the needed length; a short buffer fails with ENOMEM like Darwin.
static int darwin_sysctl_answer(const DarwinSysctl& e, void* oldp, size_t* oldlenp) {
    if (!oldlenp) return oldp ? (errno = EINVAL, -1) : 0;
    char tmp[16];
    const void* src = tmp;
    size_t len;
    switch (e.kind) {
        case 's': src = e.s; len = std::strlen(e.s) + 1; break;
        case 'i': { int32_t v = static_cast<int32_t>(e.v); std::memcpy(tmp, &v, 4); len = 4; break; }
        case 'q': std::memcpy(tmp, &e.v, 8); len = 8; break;
        case 'b': {   // Darwin arm64 timeval: int64 tv_sec, int32 tv_usec, 4 bytes padding
            struct timespec rt, bt;
            clock_gettime(CLOCK_REALTIME, &rt);
            clock_gettime(CLOCK_BOOTTIME, &bt);
            int64_t ns = (rt.tv_sec - bt.tv_sec) * 1000000000LL + (rt.tv_nsec - bt.tv_nsec);
            int64_t sec = ns / 1000000000LL;
            int32_t usec = static_cast<int32_t>((ns % 1000000000LL) / 1000);
            std::memset(tmp, 0, 16);
            std::memcpy(tmp, &sec, 8);
            std::memcpy(tmp + 8, &usec, 4);
            len = 16;
            break;
        }
        default: src = nullptr; len = e.size; break;
    }
    if (!oldp) { *oldlenp = len; return 0; }
    if (*oldlenp < len) {
        // Darwin accepts a 4-byte buffer for 64-bit integer nodes and truncates
        if (e.kind == 'q' && *oldlenp == 4) { int32_t v = static_cast<int32_t>(e.v); std::memcpy(oldp, &v, 4); return 0; }
        *oldlenp = len;
        errno = ENOMEM;
        return -1;
    }
    if (src) std::memcpy(oldp, src, len); else std::memset(oldp, 0, len);
    *oldlenp = len;
    return 0;
}

static void shim_sysctl(CallFrame& frame, void*) {
    int* name = reinterpret_cast<int*>(frame.x[0]);
    unsigned int namelen = static_cast<unsigned int>(frame.x[1]);
    void* oldp = reinterpret_cast<void*>(frame.x[2]);
    size_t* oldlenp = reinterpret_cast<size_t*>(frame.x[3]);
    if (name && namelen >= 2) {
        for (const DarwinSysctl& e : kDarwinSysctls) {
            if (e.mib0 == name[0] && e.mib1 == name[1]) {
                frame.ret_x0 = static_cast<uint64_t>(static_cast<int64_t>(darwin_sysctl_answer(e, oldp, oldlenp)));
                return;
            }
        }
    }
    fprintf(stderr, "[SHIM] sysctl({%d,%d}, len %u) -> ENOENT\n", name && namelen > 0 ? name[0] : -1,
            name && namelen > 1 ? name[1] : -1, namelen);
    errno = ENOENT;
    frame.ret_x0 = static_cast<uint64_t>(-1);
}

static void shim_sysctlbyname(CallFrame& frame, void*) {
    const char* name = reinterpret_cast<const char*>(frame.x[0]);
    void* oldp = reinterpret_cast<void*>(frame.x[1]);
    size_t* oldlenp = reinterpret_cast<size_t*>(frame.x[2]);
    if (name) {
        for (const DarwinSysctl& e : kDarwinSysctls) {
            if (std::strcmp(e.name, name) == 0) {
                int rc = darwin_sysctl_answer(e, oldp, oldlenp);
                printf("[SHIM] sysctlbyname(\"%s\", %s) -> %d len=%zu\n", name, oldp ? "read" : "size", rc,
                       oldlenp ? *oldlenp : 0);
                frame.ret_x0 = static_cast<uint64_t>(static_cast<int64_t>(rc));
                return;
            }
        }
    }
    fprintf(stderr, "[SHIM] sysctlbyname(\"%s\") -> ENOENT\n", name ? name : "null");
    errno = ENOENT;
    frame.ret_x0 = static_cast<uint64_t>(-1);
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

#include "fake_foundation.hpp"
#include "mtl_gles.hpp"
#include "fake_uikit.hpp"

static void shim_objc_msgSend(CallFrame& frame, void*) {
    void* receiver = reinterpret_cast<void*>(frame.x[0]);
    const char* sel = reinterpret_cast<const char*>(frame.x[1]);
    static std::unordered_set<std::string> seen_selectors;
    static int call_seq = 0;
    static std::mutex log_mu;
    std::string sel_str = sel ? sel : "null";
    bool first_use;
    {
        std::lock_guard<std::mutex> l(log_mu);
        first_use = seen_selectors.insert(sel_str).second;
    }
    if (first_use) {
        FakeObjcObj* fo = fake_lookup(frame.x[0]);
        printf("[OBJC_DYNAMIC #%d] [%p %s]%s%s\n", ++call_seq, receiver, sel_str.c_str(), fo ? "  fake:" : "", fo ? (std::to_string(fo->kind) + ":" + fo->s).c_str() : "");
    }
    // Engine mode (LOADER_ENGINE / LOADER_OBJC_NIL): behave like an empty Foundation. Messages answer nil/0 (so fast
    // enumeration ends, lookups miss, bundles are absent) except the retain/alloc family, which hands back the receiver.
    static const bool nil_mode = getenv("LOADER_ENGINE") != nullptr || getenv("LOADER_OBJC_NIL") != nullptr;
    if (nil_mode) {
        if (fake_foundation(frame, sel_str)) {
            static std::unordered_set<std::string> ret_seen;
            bool first_ret;
            {
                std::lock_guard<std::mutex> l(log_mu);
                first_ret = ret_seen.insert(sel_str).second;
            }
            if (first_ret) printf("[OBJC_RET] %s -> %#llx\n", sel_str.c_str(), static_cast<unsigned long long>(frame.ret_x0));
            return;
        }
        static const char* const keep_self[] = {"alloc", "allocWithZone:", "init", "new", "retain", "autorelease", "self",
                                                "copy", "mutableCopy", "copyWithZone:", "mutableCopyWithZone:",
                                                "retainAutorelease", nullptr};
        bool keep = false;
        for (const char* const* k = keep_self; *k; ++k)
            if (sel_str == *k) { keep = true; break; }
        if (!keep && sel_str.compare(0, 8, "initWith") == 0) keep = true;
        frame.ret_x0 = keep ? frame.x[0] : 0;
        return;
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

// ---------------------------------------------------------------------------
// Import classification.
// Names bound through the LAZY stream are call targets (stubs in __la_symbol_ptr),
// i.e. functions. Names that only appear in the regular stream are treated as DATA
// (constants, globals, class refs) unless listed as known functions below.
// ---------------------------------------------------------------------------
static std::unordered_set<std::string> g_lazy_names;
static const char* const kKnownFunctionsInRegularStream[] = {
    "___objc_personality_v0", "___gxx_personality_v0", "___chkstk_darwin", nullptr};

static bool is_function_import(const std::string& name) {
    if (g_lazy_names.count(name)) return true;
    for (const char* const* k = kKnownFunctionsInRegularStream; *k; ++k)
        if (name == *k) return true;
    return false;
}

// Poison region: unresolved DATA imports resolve to distinct slots in a PROT_NONE
// mapping, so any read/write through them faults and the SEGV handler can name the symbol.
// One page per symbol so a touch of one never unprotects another.
#define POISON_SLOT_SIZE 4096
#define POISON_SLOTS 512
// Default: log the first touch of each poisoned symbol, unprotect its page (reads return zeros) and
// resume. LOADER_POISON_STRICT=1 restores fault-and-dump on the first touch.
static bool g_poison_continue = true;
static size_t g_poison_touch_count = 0;
static uint8_t* g_poison_base = nullptr;
static std::vector<std::string> g_poison_names;
static std::vector<std::string> g_unresolved_fn_names;

static void* poison_slot_for(const std::string& name) {
    if (!g_poison_base) {
        void* p = mmap(nullptr, POISON_SLOT_SIZE * POISON_SLOTS, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) return nullptr;
        g_poison_base = static_cast<uint8_t*>(p);
    }
    if (g_poison_names.size() >= POISON_SLOTS) return nullptr;
    size_t idx = g_poison_names.size();
    g_poison_names.push_back(name);
    return g_poison_base + idx * POISON_SLOT_SIZE;
}

// ---------------------------------------------------------------------------
// Debug mode: LOADER_TRACE_DIRECT=1 (log first 8 calls per symbol), =all (every call),
// =list (just print the startup list). Imports resolved straight to bionic via the
// lazy stream are routed through direct_trace_forward, which logs and tail-jumps.
// ---------------------------------------------------------------------------
struct DirectEntry {
    void* target;
    const char* name;
    uint64_t calls;
};
static DirectEntry g_direct_entries[MAX_TRAMPOLINES];
static int g_trace_mode = 0;  // 0 off, 1 capped, 2 all, 3 list only

// C++ exception machinery imports that get logging stubs when LOADER_EXCEPTION_TEST or
// LOADER_TRACE_UNWIND is set (independent of LOADER_TRACE_DIRECT).
static bool g_trace_unwind = false;
static bool is_unwind_hook_name(const std::string& n) {
    static const char* const names[] = {
        "___cxa_throw", "___cxa_rethrow", "___cxa_begin_catch", "___cxa_end_catch",
        "___cxa_allocate_exception", "___cxa_free_exception", "___cxa_call_unexpected",
        "__ZSt9terminatev", "__Unwind_Resume", nullptr};
    for (const char* const* p = names; *p; ++p)
        if (n == *p) return true;
    return false;
}
static const uint64_t kTraceCap = 8;
extern "C" void direct_trace_forward();

extern "C" void direct_trace_log(DirectEntry* e, uint64_t lr, const uint64_t* x) {
    uint64_t n = __atomic_add_fetch(&e->calls, 1, __ATOMIC_RELAXED);
    shim_ring_record(e->name, x, lr, 0);
    if (g_trace_mode == 2 || n <= kTraceCap) {
        wr_str(2, "[DIRECT] "); wr_str(2, e->name);
        wr_str(2, " #"); wr_hex(2, n);
        wr_str(2, " lr="); wr_hex(2, lr);
        if (in_image(lr)) { wr_str(2, " image+"); wr_hex(2, lr - reinterpret_cast<uint64_t>(g_loaded_image_base)); }
        wr_str(2, " x0="); wr_hex(2, x[0]);
        wr_str(2, " x1="); wr_hex(2, x[1]);
        wr_str(2, " x2="); wr_hex(2, x[2]);
        wr_str(2, "\n");
    }
}

static void* make_direct_trace_stub(void* target, const char* name) {
    if (g_trampoline_count >= MAX_TRAMPOLINES) return target;
    size_t idx = g_trampoline_count++;
    g_trampoline_entries[idx] = {nullptr, name};
    g_direct_entries[idx] = {target, name, 0};
    uint32_t* stub = g_trampoline_code + (idx * TRAMPOLINE_STUB_WORDS);
    stub[0] = 0xd503245f;  // bti c
    stub[1] = 0x58000071;  // ldr x17, [pc, #12]
    stub[2] = 0x58000090;  // ldr x16, [pc, #16]
    stub[3] = 0xd61f0200;  // br x16
    *reinterpret_cast<uint64_t*>(&stub[4]) = reinterpret_cast<uint64_t>(&g_direct_entries[idx]);
    *reinterpret_cast<uint64_t*>(&stub[6]) = reinterpret_cast<uint64_t>(&direct_trace_forward);
    return stub;
}

struct DirectBound {
    std::string name;
    bool libcxx;
    bool lazy;
    std::string module;
};
static std::vector<DirectBound> g_direct_bound;

// Known Darwin/bionic differences for names that currently bind straight to bionic.
// Informational only: nothing here is fixed yet.
struct DiffNote {
    const char* name;  // exact (leading '_' stripped) or prefix when it ends with '*'
    const char* cat;
    const char* note;
};
static const DiffNote kDiffNotes[] = {
    // errno / error-number constants
    {"strerror", "ERRNO", "message table indexed by errno; Darwin and Linux numbers differ (EAGAIN 35/11, ETIMEDOUT 60/110, ...)"},
    {"strerror_r", "ERRNO", "errno numbering differs"},
    {"perror", "ERRNO", "errno numbering differs"},
    {"pthread_cond_timedwait", "ERRNO", "returns ETIMEDOUT: Darwin 60, bionic 110"},
    {"pthread_mutex_timedlock", "ERRNO", "returns ETIMEDOUT: Darwin 60, bionic 110"},
    {"sem_timedwait", "ERRNO", "errno ETIMEDOUT 60 vs 110"},
    {"getaddrinfo", "ERRNO", "EAI_* values differ (Darwin EAI_NONAME 8, Linux -2); addrinfo field order differs"},
    {"gai_strerror", "ERRNO", "EAI_* values differ"},
    {"connect", "ERRNO", "errno EINPROGRESS 36/115, ECONNREFUSED 61/111, ..."},
    {"accept", "ERRNO", "errno numbering differs"},
    {"read", "ERRNO", "errno EAGAIN 35 vs 11 if guest compares"},
    {"write", "ERRNO", "errno EAGAIN 35 vs 11 if guest compares"},
    // signals
    {"sigaction", "SIGNAL", "struct sigaction layout, SIG* numbers (SIGBUS 10/7, SIGUSR1 30/10, SIGCHLD 20/17), siginfo_t, ucontext differ"},
    {"signal", "SIGNAL", "SIG* numbers differ"},
    {"sigprocmask", "SIGNAL", "sigset_t 4 bytes on Darwin, 8 on bionic LP64; SIG_* how values differ"},
    {"pthread_sigmask", "SIGNAL", "sigset_t size and SIG_* how values differ"},
    {"sigaltstack", "SIGNAL", "stack_t field order differs"},
    {"sigemptyset", "SIGNAL", "sigset_t size differs"},
    {"sigfillset", "SIGNAL", "sigset_t size differs"},
    {"sigaddset", "SIGNAL", "sigset_t size/signal numbers differ"},
    {"sigdelset", "SIGNAL", "sigset_t size/signal numbers differ"},
    {"sigismember", "SIGNAL", "sigset_t size/signal numbers differ"},
    {"kill", "SIGNAL", "signal numbers differ"},
    {"raise", "SIGNAL", "signal numbers differ"},
    {"pthread_kill", "SIGNAL", "signal numbers differ"},
    // sockets
    {"socket", "SOCKET", "AF_INET6 30/10, SOCK_* same, protocol constants differ"},
    {"bind", "SOCKET", "sockaddr has sa_len on Darwin; sa_family width differs"},
    {"listen", "SOCKET", "sockaddr/constant differences"},
    {"send", "SOCKET", "MSG_* flag values differ"},
    {"sendto", "SOCKET", "MSG_* and sockaddr layout differ"},
    {"sendmsg", "SOCKET", "msghdr/cmsghdr layout and MSG_* differ"},
    {"recv", "SOCKET", "MSG_* flag values differ"},
    {"recvfrom", "SOCKET", "MSG_* and sockaddr layout differ"},
    {"recvmsg", "SOCKET", "msghdr/cmsghdr layout and MSG_* differ"},
    {"setsockopt", "SOCKET", "SOL_SOCKET 0xffff/1, SO_* values differ (SO_REUSEADDR 4/2, SO_NOSIGPIPE Darwin only)"},
    {"getsockopt", "SOCKET", "SOL_SOCKET and SO_* values differ"},
    {"getsockname", "SOCKET", "sockaddr layout differs"},
    {"getpeername", "SOCKET", "sockaddr layout differs"},
    {"shutdown", "SOCKET", "SHUT_* same; errno differs"},
    {"socketpair", "SOCKET", "AF_* values differ"},
    {"inet_ntop", "SOCKET", "AF_INET6 30 vs 10"},
    {"inet_pton", "SOCKET", "AF_INET6 30 vs 10"},
    {"getnameinfo", "SOCKET", "sockaddr layout and NI_* flags differ"},
    // time
    {"gettimeofday", "TIME", "Darwin timeval.tv_usec is int32, bionic long (16 vs 16 bytes but different padding)"},
    {"clock_gettime", "TIME", "clockid values differ (Darwin MONOTONIC=6, PROCESS_CPUTIME=12, UPTIME_RAW=8; Linux 1/2/-)"},
    {"clock_getres", "TIME", "clockid values differ"},
    {"clock_settime", "TIME", "clockid values differ"},
    {"select", "TIME", "timeval.tv_usec width; fd_set same"},
    {"utimes", "TIME", "timeval.tv_usec width"},
    {"futimes", "TIME", "timeval.tv_usec width"},
    {"setitimer", "TIME", "itimerval/timeval layout; ITIMER_* same"},
    {"getitimer", "TIME", "itimerval/timeval layout"},
    {"getrusage", "TIME", "rusage timeval fields and ru_maxrss units (bytes on Darwin, KB on Linux)"},
    // dirent
    {"readdir_r", "DIRENT", "Darwin dirent d_name at +21 (bionic +19), d_namlen field"},
    {"scandir", "DIRENT", "passes dirent** to guest callbacks; layout differs"},
    {"alphasort", "DIRENT", "dirent layout differs"},
    {"seekdir", "DIRENT", "telldir cookie semantics differ"},
    {"telldir", "DIRENT", "cookie semantics differ"},
    {"getdirentries", "DIRENT", "Darwin-only record format"},
    {"fts_open", "DIRENT", "FTSENT layout differs"},
    {"fts_read", "DIRENT", "FTSENT layout differs"},
    // ifaddrs / network interfaces
    {"getifaddrs", "IFADDRS", "ifaddrs/sockaddr layout (sa_len, AF_LINK vs AF_PACKET) and IFF_* flags differ"},
    {"freeifaddrs", "IFADDRS", "paired with getifaddrs layout"},
    {"if_nametoindex", "IFADDRS", "ok but pair with getifaddrs"},
    {"if_indextoname", "IFADDRS", "ok but pair with getifaddrs"},
    // misc constants / structs
    {"sysconf", "CONSTANTS", "_SC_* values differ (Darwin _SC_PAGESIZE 29, _SC_NPROCESSORS_ONLN 58; bionic 39 / 97)"},
    {"getrlimit", "CONSTANTS", "RLIMIT_* numbers differ (NOFILE 8/7, NPROC 7/6, MEMLOCK 6/8, AS 5/9)"},
    {"setrlimit", "CONSTANTS", "RLIMIT_* numbers differ"},
    {"madvise", "CONSTANTS", "MADV_FREE Darwin 5, Linux 8; MADV_ZERO_WIRED_PAGES Darwin only"},
    {"msync", "CONSTANTS", "MS_SYNC Darwin 0x10 vs Linux 4"},
    {"setlocale", "CONSTANTS", "LC_* numbering differs (Darwin LC_ALL 0; bionic LC_ALL 6)"},
    {"newlocale", "CONSTANTS", "LC_*_MASK values differ"},
    {"uselocale", "CONSTANTS", "xlocale types differ"},
    {"dlopen", "CONSTANTS", "RTLD_GLOBAL 8/0x100, RTLD_LOCAL 4/0; Darwin RTLD_DEFAULT is (void*)-2"},
    {"dlsym", "CONSTANTS", "Darwin RTLD_DEFAULT (void*)-2, RTLD_NEXT -1, RTLD_SELF -3 are not bionic handles"},
    {"pthread_attr_setdetachstate", "CONSTANTS", "PTHREAD_CREATE_JOINABLE Darwin 1 / bionic 0; DETACHED 2 / 1"},
    {"pthread_attr_getdetachstate", "CONSTANTS", "PTHREAD_CREATE_* values differ"},
    {"pthread_attr_setschedpolicy", "CONSTANTS", "SCHED_OTHER Darwin 1 / bionic 0; SCHED_FIFO 4 / 1"},
    {"pthread_attr_setschedparam", "CONSTANTS", "sched_param layout differs (Darwin has opaque padding)"},
    {"pthread_setschedparam", "CONSTANTS", "SCHED_* and sched_param differ"},
    {"pthread_getschedparam", "CONSTANTS", "SCHED_* and sched_param differ"},
    {"pthread_once", "STRUCT", "Darwin pthread_once_t {sig, opaque} with static init 0x30B1BCBA; bionic is a 4-byte int"},
    {"pthread_mutex_*", "STRUCT", "Darwin pthread_mutex_t layout and static-initializer signature word differ"},
    {"pthread_cond_*", "STRUCT", "Darwin pthread_cond_t layout and static-initializer signature differ"},
    {"pthread_rwlock_*", "STRUCT", "Darwin pthread_rwlock_t layout and static-initializer signature differ"},
    {"setjmp", "STRUCT", "jmp_buf: Darwin int[48]=192 bytes, bionic arm64 long[32]=256 bytes (overflows guest buffer)"},
    {"_setjmp", "STRUCT", "jmp_buf size differs (see setjmp)"},
    {"sigsetjmp", "STRUCT", "sigjmp_buf size differs"},
    {"longjmp", "STRUCT", "jmp_buf layout differs"},
    {"_longjmp", "STRUCT", "jmp_buf layout differs"},
    {"siglongjmp", "STRUCT", "sigjmp_buf layout differs"},
    {"uname", "STRUCT", "utsname field width 256 (Darwin) vs 65 (bionic)"},
    {"statfs", "STRUCT", "struct statfs layout differs"},
    {"fstatfs", "STRUCT", "struct statfs layout differs"},
    {"statvfs", "STRUCT", "struct statvfs layout differs (deferred)"},
    {"fstatvfs", "STRUCT", "struct statvfs layout differs"},
    {"getmntinfo", "STRUCT", "Darwin-only / struct statfs"},
    {"__swbuf", "STRUCT", "Darwin stdio internals (FILE fields accessed by guest macros)"},
    {"__srget", "STRUCT", "Darwin stdio internals"},
    {"fstatat", "STRUCT", "struct stat layout and AT_FDCWD (-2 vs -100)"},
    {"fchmodat", "CONSTANTS", "AT_FDCWD -2 vs -100"},
    {"unlinkat", "CONSTANTS", "AT_FDCWD -2 vs -100"},
    {"mkdirat", "CONSTANTS", "AT_FDCWD -2 vs -100"},
    {"posix_spawn", "STRUCT", "Darwin posix_spawn attr/file_actions layout"},
    {nullptr, nullptr, nullptr}};

static const DiffNote* find_diff_note(const std::string& sym) {
    const char* s = sym.c_str();
    if (s[0] == '_') ++s;
    for (const DiffNote* d = kDiffNotes; d->name; ++d) {
        size_t n = std::strlen(d->name);
        if (n && d->name[n - 1] == '*') {
            if (std::strncmp(s, d->name, n - 1) == 0) return d;
        } else if (std::strcmp(s, d->name) == 0 || std::strcmp(sym.c_str(), d->name) == 0) {
            return d;
        }
    }
    return nullptr;
}

static void dump_direct_bound_list() {
    size_t libc_n = 0, libcxx_n = 0, flagged = 0;
    for (const auto& b : g_direct_bound) (b.libcxx ? libcxx_n : libc_n)++;
    fprintf(stderr, "\n[DIRECT] %zu imports bound straight to bionic libc (+%zu to libc++); trace mode=%d\n",
            libc_n, libcxx_n, g_trace_mode);
    for (const auto& b : g_direct_bound) {
        if (b.libcxx) continue;
        const DiffNote* d = find_diff_note(b.name);
        if (d) ++flagged;
        fprintf(stderr, "[DIRECT-BOUND] %-40s %-8s %-10s%s%s%s%s\n", b.name.c_str(), b.module.c_str(), b.lazy ? "lazy" : "regular",
                d ? "  DIFFERS[" : "", d ? d->cat : "", d ? "] " : "", d ? d->note : "");
    }
    fprintf(stderr, "[DIRECT] %zu libc names flagged with known Darwin/bionic differences\n", flagged);
    for (const auto& b : g_direct_bound) {
        if (b.libcxx) fprintf(stderr, "[DIRECT-BOUND-LIBCXX] %s\n", b.name.c_str());
    }
}

// ---------------------------------------------------------------------------
// Apple framework function stubs (CoreFoundation / CoreGraphics / AudioToolbox / ...), engine mode.
// CF types are toll-free bridged with the fake NS objects above, so they share fake_lookup()/objc_string_value().
// ---------------------------------------------------------------------------
#include <atomic>
#include <thread>

extern "C" {
void* native_cf_retain(void* o) { return o; }
void native_cf_release(void*) {}
int64_t native_cfstring_get_length(void* s) { std::string v; return objc_string_value(reinterpret_cast<uint64_t>(s), v) ? static_cast<int64_t>(v.size()) : 0; }
int native_cfstring_get_cstring(void* s, char* buf, int64_t size, uint32_t) {
    std::string v;
    if (!buf || size <= 0 || !objc_string_value(reinterpret_cast<uint64_t>(s), v)) return 0;
    if (static_cast<int64_t>(v.size()) + 1 > size) return 0;
    std::memcpy(buf, v.c_str(), v.size() + 1);
    return 1;
}
int64_t native_cfstring_max_size(int64_t len, uint32_t) { return len * 4 + 1; }
void* native_cf_null() { return nullptr; }
int native_cf_false() { return 0; }
int64_t native_cf_zero() { return 0; }
void* native_cfarray_create_mutable(void*, int64_t, void*) { return reinterpret_cast<void*>(fake_make(3, "cfarray")); }
void* native_cfstring_create_with_characters(void*, const uint16_t* chars, int64_t n) {
    std::string s;
    for (int64_t i = 0; chars && i < n; ++i) s.push_back(chars[i] < 128 ? static_cast<char>(chars[i]) : '?');
    return reinterpret_cast<void*>(fake_make(2, s));
}
// NS functions
void* native_nsclass_from_string(void* name) {
    std::string n;
    if (!objc_string_value(reinterpret_cast<uint64_t>(name), n) || n.empty()) return nullptr;
    if (objcrt::GClass* gc = objcrt::find_class_by_name(n.c_str())) return reinterpret_cast<void*>(gc->addr);
    return reinterpret_cast<void*>(fake_class_named(n));
}
void* native_ns_temp_dir() {
    const char* t = getenv("LOADER_TMP_DIR");
    return reinterpret_cast<void*>(fake_make(2, t ? t : "/data/local/tmp/"));
}
// The app's writable container (Documents, Library, Library/Caches, ...). LOADER_SANDBOX_DIR overrides the default.
static std::string sandbox_path(const char* sub) {
    static const std::string root = getenv("LOADER_SANDBOX_DIR") ? getenv("LOADER_SANDBOX_DIR") : "/data/local/tmp/sandbox";
    std::string p = root;
    mkdir(p.c_str(), 0755);
    for (const char* s = sub; s && *s;) {   // create each component of sub
        const char* slash = strchr(s, '/');
        p += "/" + std::string(s, slash ? static_cast<size_t>(slash - s) : strlen(s));
        mkdir(p.c_str(), 0755);
        s = slash ? slash + 1 : nullptr;
    }
    return p;
}
// NSSearchPathForDirectoriesInDomains(directory, domainMask, expandTilde) for the user domain.
void* native_ns_search_paths(uint64_t directory, uint64_t, int) {
    const char* sub = nullptr;
    switch (directory) {
        case 9: sub = "Documents"; break;                           // NSDocumentDirectory
        case 5: sub = "Library"; break;                             // NSLibraryDirectory
        case 13: sub = "Library/Caches"; break;                     // NSCachesDirectory
        case 14: sub = "Library/Application Support"; break;        // NSApplicationSupportDirectory
        case 15: sub = "Downloads"; break;                          // NSDownloadsDirectory
        default: break;
    }
    if (!sub) return reinterpret_cast<void*>(fakeui::new_array({}));
    return reinterpret_cast<void*>(fakeui::new_array({fake_make(2, sandbox_path(sub))}));
}
void* native_ns_uncaught_handler_get() { return nullptr; }
void native_ns_uncaught_handler_set(void*) {}
void native_chkstk_darwin() {}
// CFLocale: libc++'s locale initialisation asks CoreFoundation for the current locale; answer en_US.
void* native_cflocale_copy_current() { static uint64_t loc = fake_make(5, "CFLocale"); return reinterpret_cast<void*>(loc); }
void* native_cflocale_get_identifier(void*) { return reinterpret_cast<void*>(fake_make(2, "en_US")); }
void* native_cflocale_get_value(void*, const void* key) {
    if (key == g_kCFLocaleLanguageCode) return reinterpret_cast<void*>(fake_make(2, "en"));
    if (key == g_kCFLocaleCountryCode) return reinterpret_cast<void*>(fake_make(2, "US"));
    return reinterpret_cast<void*>(fake_make(2, ""));
}
// CFStringGetBytes(str, CFRange{loc,len}, enc, lossByte, isExternal, buf, maxBuf, CFIndex* used)
int64_t native_cfstring_get_bytes(void* s, int64_t loc, int64_t len, uint32_t, uint8_t, uint8_t, uint8_t* buf, int64_t max_buf, int64_t* used) {
    std::string v;
    if (!objc_string_value(reinterpret_cast<uint64_t>(s), v)) { if (used) *used = 0; return 0; }
    if (loc < 0 || loc > static_cast<int64_t>(v.size())) loc = 0;
    if (len < 0 || loc + len > static_cast<int64_t>(v.size())) len = static_cast<int64_t>(v.size()) - loc;
    int64_t n = buf ? std::min<int64_t>(len, max_buf) : len;
    if (buf && n > 0) std::memcpy(buf, v.data() + loc, static_cast<size_t>(n));
    if (used) *used = n;
    return n;
}
// CoreGraphics math (CGAffineTransform = 6 doubles, returned via x8; CGRect = HFA of 4 doubles)
struct CGAT { double a, b, c, d, tx, ty; };
struct CGR { double x, y, w, h; };
CGAT native_cgat_concat(CGAT t1, CGAT t2) {
    return {t1.a * t2.a + t1.b * t2.c, t1.a * t2.b + t1.b * t2.d, t1.c * t2.a + t1.d * t2.c, t1.c * t2.b + t1.d * t2.d,
            t1.tx * t2.a + t1.ty * t2.c + t2.tx, t1.tx * t2.b + t1.ty * t2.d + t2.ty};
}
CGAT native_cgat_invert(CGAT t) {
    double det = t.a * t.d - t.b * t.c;
    if (det == 0) return t;
    return {t.d / det, -t.b / det, -t.c / det, t.a / det, (t.c * t.ty - t.d * t.tx) / det, (t.b * t.tx - t.a * t.ty) / det};
}
CGAT native_cgat_make_rotation(double r) { return {cos(r), sin(r), -sin(r), cos(r), 0, 0}; }
CGR native_cgrect_union(CGR a, CGR b) {
    double x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
    double x1 = std::max(a.x + a.w, b.x + b.w), y1 = std::max(a.y + a.h, b.y + b.h);
    return {x0, y0, x1 - x0, y1 - y0};
}
void* native_fake_handle() { return reinterpret_cast<void*>(fake_make(3, "handle")); }
int native_ret_error() { return -1; }
struct CMTime { int64_t value; int32_t timescale; uint32_t flags; int64_t epoch; };
double native_cmtime_seconds(CMTime t) { return t.timescale ? static_cast<double>(t.value) / t.timescale : 0.0; }
CMTime native_cmtime_make(int64_t v, int32_t ts) { return {v, ts, 1, 0}; }
CMTime native_cmtime_make_seconds(double s, int32_t ts) { return {static_cast<int64_t>(s * ts), ts, 1, 0}; }
int32_t native_cmtime_compare(CMTime a, CMTime b) {
    double x = native_cmtime_seconds(a), y = native_cmtime_seconds(b);
    return x < y ? -1 : (x > y ? 1 : 0);
}

// AudioUnit output: audio_unit.cpp
void* native_au_find_next(void* prev, const void*);
int32_t native_au_instance_new(void*, void** out);
int32_t native_au_instance_dispose(void* u);
int32_t native_au_set_property(void* u, uint32_t prop, uint32_t, uint32_t, const void* data, uint32_t size);
int32_t native_au_get_property(void* u, uint32_t prop, uint32_t, uint32_t, void* data, uint32_t* size);
int32_t native_au_zero(void*);
int32_t native_au_start(void* u);
int32_t native_au_stop(void* u);
} // extern "C"


// ---------------------------------------------------------------------------
// Objective-C runtime glue: called from objc_msgSend_guest / objc_msgSendSuper2_guest (trampolines.s).
// ---------------------------------------------------------------------------
extern "C" void objc_msgSend_guest();
extern "C" void objc_msgSendSuper2_guest();
extern "C" uint64_t objc_lookup_imp_c(uint64_t recv, const char* sel) { return objcrt::lookup_imp(recv, sel); }
extern "C" uint64_t objc_lookup_super_imp_c(uint64_t sup, const char* sel) { return objcrt::lookup_super_imp(sup, sel); }

static uint64_t objc_send_noargs(uint64_t obj, const char* sel) {
    uint64_t imp = objcrt::lookup_imp(obj, sel);
    if (!imp) return obj;
    return reinterpret_cast<uint64_t (*)(uint64_t, const char*)>(imp)(obj, objcrt::intern(sel));
}

extern "C" uint64_t objc_fallback_c(uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3, uint64_t x4, uint64_t x5, uint64_t x6, uint64_t x7) {
    const char* sel = reinterpret_cast<const char*>(x1);
    std::string s = sel ? sel : "";
    bool is_cls = false;
    if (objcrt::GClass* gc = objcrt::guest_class_for_object(x0, &is_cls)) {
        if (is_cls) {   // class receiver
            if (s == "alloc" || s == "allocWithZone:") return objcrt::alloc_instance(gc);
            if (s == "new") return objc_send_noargs(objcrt::alloc_instance(gc), "init");
            if (s == "class" || s == "self" || s == "retain" || s == "autorelease") return x0;
            if (s == "initialize" || s == "load" || s == "release") return 0;
            if (s == "respondsToSelector:") return objcrt::lookup_imp(x0, reinterpret_cast<const char*>(x2)) != 0;
        } else {        // instance receiver
            if (s == "class") return gc->addr;
            if (s == "isEqual:") return x0 == x2;
            if (s == "hash") return x0;
            if (s == "isKindOfClass:" || s == "isMemberOfClass:") return fakeui::is_kind_of(x0, x2);
            if (s == "respondsToSelector:") return objcrt::lookup_imp(x0, reinterpret_cast<const char*>(x2)) != 0;
            if (s == "performSelector:") return fakeui::send_sel(x0, reinterpret_cast<const char*>(x2), 0);
            if (s == "performSelector:withObject:") return fakeui::send_sel(x0, reinterpret_cast<const char*>(x2), x3);
            // Subclasses of UIKit classes keep UIKit state (frame, layer, subviews...) in fake_uikit.hpp.
            if (!fakeui::ui_state(x0)) {
                if (s.compare(0, 4, "init") == 0 || s == "retain" || s == "autorelease" || s == "self" || s == "copy" ||
                    s == "mutableCopy" || s == "copyWithZone:" || s == "retainAutorelease") return x0;
                if (s == "release" || s == "dealloc") return 0;
            }
        }
    }
    g_objc_ret_d[0] = g_objc_ret_d[1] = g_objc_ret_d[2] = g_objc_ret_d[3] = 0;
    CallFrame f{};
    f.x[0] = x0; f.x[1] = x1; f.x[2] = x2; f.x[3] = x3; f.x[4] = x4; f.x[5] = x5; f.x[6] = x6; f.x[7] = x7;
    shim_objc_msgSend(f, nullptr);
    return f.ret_x0;
}

extern "C" {
uint64_t native_objc_alloc(uint64_t cls) {
    bool is_cls = false;
    if (objcrt::GClass* gc = objcrt::guest_class_for_object(cls, &is_cls))
        if (is_cls) return objcrt::alloc_instance(gc);
    std::string name = fakeui::class_name_of_class(cls);
    return name.empty() ? fake_make(5, "obj") : fakeui::alloc_for_class(name);
}
uint64_t native_objc_alloc_init(uint64_t cls) { return objc_send_noargs(native_objc_alloc(cls), "init"); }
uint64_t native_objc_identity(uint64_t x) { return x; }
void native_objc_noop() {}
void native_objc_store_strong(uint64_t* addr, uint64_t v) { if (addr) *addr = v; }
uint64_t native_objc_load_weak(uint64_t* addr) { return addr ? *addr : 0; }
uint64_t native_objc_store_weak(uint64_t* addr, uint64_t v) { if (addr) *addr = v; return v; }
void native_objc_copy_weak(uint64_t* to, uint64_t* from) { if (to) *to = from ? *from : 0; }
uint64_t native_objc_get_property(uint64_t self, void*, int64_t off, bool) { return self ? *reinterpret_cast<uint64_t*>(self + off) : 0; }
void native_objc_set_property(uint64_t self, void*, uint64_t v, int64_t off) { if (self) *reinterpret_cast<uint64_t*>(self + off) = v; }
uint64_t native_objc_opt_class(uint64_t o) {
    bool is_cls = false;
    if (objcrt::GClass* gc = objcrt::guest_class_for_object(o, &is_cls)) return gc->addr;
    FakeObjcObj* f = fake_lookup(o);
    if (!f) return 0;
    if (f->kind == 7 || (f->kind == 3 && f->s.compare(0, 6, "class:") == 0)) return o;   // [Cls class] == Cls
    if (f->kind == 2) return fake_class_named("NSString");
    if (f->kind == 5 || f->kind == 6 || f->kind == 8) return fake_class_named(f->s);
    return 0;
}
uint64_t native_objc_opt_is_kind_of(uint64_t o, uint64_t cls) { return fakeui::is_kind_of(o, cls); }
uint64_t native_class_replace_method(uint64_t cls, const char* sel, uint64_t imp, const char*) {
    uint64_t old = objcrt::replace_method(cls, sel, imp);
    fprintf(stderr, "[OBJC-RT] class_replaceMethod(%#llx, %s, image+%#llx)\n", static_cast<unsigned long long>(cls), sel ? sel : "?",
            static_cast<unsigned long long>(imp - reinterpret_cast<uint64_t>(g_loaded_image_base)));
    return old;
}
// @synchronized: one recursive mutex per object
static std::recursive_mutex* objc_sync_mutex(uint64_t o) {
    static std::mutex mu;
    static std::unordered_map<uint64_t, std::recursive_mutex*> m;
    std::lock_guard<std::mutex> l(mu);
    auto& slot = m[o];
    if (!slot) slot = new std::recursive_mutex();
    return slot;
}
int native_objc_sync_enter(uint64_t o) { if (o) objc_sync_mutex(o)->lock(); return 0; }
int native_objc_sync_exit(uint64_t o) { if (o) objc_sync_mutex(o)->unlock(); return 0; }
uint64_t native_objc_opt_responds(uint64_t o, const char* sel) { return sel && objcrt::lookup_imp(o, sel) != 0; }
uint64_t native_objc_get_class(const char* name) {
    if (!name) return 0;
    if (objcrt::GClass* gc = objcrt::find_class_by_name(name)) return gc->addr;
    return reinterpret_cast<uint64_t>(native_nsclass_from_string(reinterpret_cast<void*>(fake_make(2, name))));
}
uint64_t native_object_get_class(uint64_t o) {
    if (uint64_t isa = objcrt::isa_of(o)) return isa;
    return native_objc_opt_class(o);
}
const char* native_sel_register(const char* n) { return n ? objcrt::intern(n) : nullptr; }
void native_objc_copy_struct(void* dst, const void* src, size_t n, bool, bool) { if (dst && src) std::memcpy(dst, src, n); }
}

static void register_objc_runtime_natives() {
    auto reg = [](const char* n, void* f) { g_custom_shims[n] = f; };
    reg("_objc_msgSend", reinterpret_cast<void*>(&objc_msgSend_guest));
    reg("_objc_msgSendSuper2", reinterpret_cast<void*>(&objc_msgSendSuper2_guest));
    for (const char* n : {"_objc_alloc", "_objc_allocWithZone"}) reg(n, reinterpret_cast<void*>(&native_objc_alloc));
    for (const char* n : {"_objc_alloc_init", "_objc_opt_new"}) reg(n, reinterpret_cast<void*>(&native_objc_alloc_init));
    for (const char* n : {"_objc_retain", "_objc_retainAutoreleasedReturnValue", "_objc_retainAutorelease",
                          "_objc_retainAutoreleaseReturnValue", "_objc_autorelease", "_objc_autoreleaseReturnValue",
                          "_objc_unsafeClaimAutoreleasedReturnValue"})
        reg(n, reinterpret_cast<void*>(&native_objc_identity));
    for (const char* n : {"_objc_release", "_objc_destroyWeak", "__Block_release", "_objc_enumerationMutation"})
        reg(n, reinterpret_cast<void*>(&native_objc_noop));
    reg("_objc_storeStrong", reinterpret_cast<void*>(&native_objc_store_strong));
    for (const char* n : {"_objc_retainBlock", "__Block_copy"}) reg(n, reinterpret_cast<void*>(&heap_block_copy));
    for (const char* n : {"_objc_loadWeakRetained", "_objc_loadWeak"}) reg(n, reinterpret_cast<void*>(&native_objc_load_weak));
    for (const char* n : {"_objc_storeWeak", "_objc_initWeak"}) reg(n, reinterpret_cast<void*>(&native_objc_store_weak));
    for (const char* n : {"_objc_copyWeak", "_objc_moveWeak"}) reg(n, reinterpret_cast<void*>(&native_objc_copy_weak));
    reg("_objc_getProperty", reinterpret_cast<void*>(&native_objc_get_property));
    for (const char* n : {"_objc_setProperty_atomic", "_objc_setProperty_nonatomic", "_objc_setProperty_atomic_copy",
                          "_objc_setProperty_nonatomic_copy"})
        reg(n, reinterpret_cast<void*>(&native_objc_set_property));
    reg("_objc_opt_class", reinterpret_cast<void*>(&native_objc_opt_class));
    reg("_objc_opt_isKindOfClass", reinterpret_cast<void*>(&native_objc_opt_is_kind_of));
    reg("_objc_opt_respondsToSelector", reinterpret_cast<void*>(&native_objc_opt_responds));
    for (const char* n : {"_objc_getClass", "_objc_lookUpClass", "_objc_getRequiredClass"})
        reg(n, reinterpret_cast<void*>(&native_objc_get_class));
    reg("_object_getClass", reinterpret_cast<void*>(&native_object_get_class));
    reg("_class_replaceMethod", reinterpret_cast<void*>(&native_class_replace_method));
    reg("_objc_sync_enter", reinterpret_cast<void*>(&native_objc_sync_enter));
    reg("_objc_sync_exit", reinterpret_cast<void*>(&native_objc_sync_exit));
    reg("_sel_registerName", reinterpret_cast<void*>(&native_sel_register));
    reg("_objc_copyStruct", reinterpret_cast<void*>(&native_objc_copy_struct));
}

static void register_darwin_framework_stubs() {
    auto reg = [](const char* n, void* f) { g_custom_shims[n] = f; };
    reg("_CFRetain", reinterpret_cast<void*>(&native_cf_retain));
    reg("_CFRelease", reinterpret_cast<void*>(&native_cf_release));
    reg("_CFStringGetLength", reinterpret_cast<void*>(&native_cfstring_get_length));
    reg("_CFStringGetCString", reinterpret_cast<void*>(&native_cfstring_get_cstring));
    reg("_CFStringGetMaximumSizeForEncoding", reinterpret_cast<void*>(&native_cfstring_max_size));
    reg("_CFStringCreateWithCharacters", reinterpret_cast<void*>(&native_cfstring_create_with_characters));
    reg("_CFArrayCreateMutable", reinterpret_cast<void*>(&native_cfarray_create_mutable));
    for (const char* n : {"_CFArrayAppendValue", "_CFWriteStreamOpen", "_CFStreamCreateBoundPair", "_CGColorRelease",
                          "_CGColorSpaceRelease", "_CVBufferRelease"})
        reg(n, reinterpret_cast<void*>(&native_cf_release));
    for (const char* n : {"_CFDataCreateWithBytesNoCopy", "_CFDataGetBytePtr", "_CFDictionaryGetValue",
                          "_CFURLGetFileSystemRepresentation", "_CFUUIDCreate",
                          "_CFUUIDCreateFromUUIDBytes", "_CFUUIDCreateString", "_CFNumberGetValue",
                          "_CTFontCopyFontDescriptor", "_CTFontCreateUIFontForLanguage", "_CTFontDescriptorCopyAttribute",
                          "_CTFontDescriptorCreateMatchingFontDescriptor", "_CTFontDescriptorCreateWithAttributes",
                          "_CTFontDescriptorCreateWithNameAndSize", "_CMFormatDescriptionGetExtension",
                          "_CMAudioFormatDescriptionGetRichestDecodableFormat", "_CMSampleBufferGetImageBuffer",
                          "_SCNetworkReachabilityCreateWithAddress", "_SecCertificateCopyData", "_SecCertificateCreateWithData",
                          "_SecPolicyCreateBasicX509", "_SecTrustGetCertificateAtIndex", "_UIImagePNGRepresentation",
                          "_MTAudioProcessingTapGetStorage", "_CVMetalTextureGetTexture"})
        reg(n, reinterpret_cast<void*>(&native_cf_null));
    for (const char* n : {"_CFDataGetLength", "_CMFormatDescriptionGetMediaSubType", "_SecTrustGetCertificateCount",
                          "_CVPixelBufferGetHeight", "_CVPixelBufferGetWidth", "_CVPixelBufferGetPixelFormatType"})
        reg(n, reinterpret_cast<void*>(&native_cf_zero));
    for (const char* n : {"_SCNetworkReachabilityGetFlags", "_SecTrustCreateWithCertificates", "_SecTrustEvaluate",
                          "_CVPixelBufferCreate", "_CVMetalTextureCacheCreate", "_CVMetalTextureCacheCreateTextureFromImage",
                          "_MTAudioProcessingTapCreate", "_MTAudioProcessingTapGetSourceAudio", "_UIApplicationMain",
                          "_CMVideoFormatDescriptionGetPresentationDimensions", "_CMSampleBufferGetPresentationTimeStamp",
                          "_CMTimeRangeMake", "_CMTimeMultiply", "_CMTimeMultiplyByFloat64", "_CVMetalTextureCacheFlush"})
        reg(n, reinterpret_cast<void*>(&native_ret_error));
    reg("_CGColorCreate", reinterpret_cast<void*>(&native_fake_handle));
    reg("_CGColorSpaceCreateDeviceRGB", reinterpret_cast<void*>(&native_fake_handle));
    reg("_CGAffineTransformConcat", reinterpret_cast<void*>(&native_cgat_concat));
    reg("_CGAffineTransformInvert", reinterpret_cast<void*>(&native_cgat_invert));
    reg("_CGAffineTransformMakeRotation", reinterpret_cast<void*>(&native_cgat_make_rotation));
    reg("_CGRectUnion", reinterpret_cast<void*>(&native_cgrect_union));
    reg("_CMTimeGetSeconds", reinterpret_cast<void*>(&native_cmtime_seconds));
    reg("_CMTimeMake", reinterpret_cast<void*>(&native_cmtime_make));
    reg("_CMTimeMakeWithSeconds", reinterpret_cast<void*>(&native_cmtime_make_seconds));
    reg("_CMTimeCompare", reinterpret_cast<void*>(&native_cmtime_compare));
    reg("_CFLocaleCopyCurrent", reinterpret_cast<void*>(&native_cflocale_copy_current));
    reg("_CFLocaleGetIdentifier", reinterpret_cast<void*>(&native_cflocale_get_identifier));
    reg("_CFLocaleGetValue", reinterpret_cast<void*>(&native_cflocale_get_value));
    reg("_CFStringGetBytes", reinterpret_cast<void*>(&native_cfstring_get_bytes));
    reg("_NSClassFromString", reinterpret_cast<void*>(&native_nsclass_from_string));
    reg("_NSTemporaryDirectory", reinterpret_cast<void*>(&native_ns_temp_dir));
    reg("_NSSearchPathForDirectoriesInDomains", reinterpret_cast<void*>(&native_ns_search_paths));
    reg("_NSGetUncaughtExceptionHandler", reinterpret_cast<void*>(&native_ns_uncaught_handler_get));
    reg("_NSSetUncaughtExceptionHandler", reinterpret_cast<void*>(&native_ns_uncaught_handler_set));
    reg("___chkstk_darwin", reinterpret_cast<void*>(&native_chkstk_darwin));
    // AudioUnit output
    reg("_AudioComponentFindNext", reinterpret_cast<void*>(&native_au_find_next));
    reg("_AudioComponentInstanceNew", reinterpret_cast<void*>(&native_au_instance_new));
    reg("_AudioComponentInstanceDispose", reinterpret_cast<void*>(&native_au_instance_dispose));
    reg("_AudioUnitSetProperty", reinterpret_cast<void*>(&native_au_set_property));
    reg("_AudioUnitGetProperty", reinterpret_cast<void*>(&native_au_get_property));
    reg("_AudioUnitInitialize", reinterpret_cast<void*>(&native_au_zero));
    reg("_AudioUnitUninitialize", reinterpret_cast<void*>(&native_au_zero));
    reg("_AudioOutputUnitStart", reinterpret_cast<void*>(&native_au_start));
    reg("_AudioOutputUnitStop", reinterpret_cast<void*>(&native_au_stop));
    // AudioQueue / AudioFile: report failure so the caller falls back or skips
    for (const char* n : {"_AudioQueueAllocateBuffer", "_AudioQueueAllocateBufferWithPacketDescriptions", "_AudioQueueDispose",
                          "_AudioQueueEnqueueBuffer", "_AudioQueueFlush", "_AudioQueueNewOutput", "_AudioQueueOfflineRender",
                          "_AudioQueueSetOfflineRenderFormat", "_AudioQueueSetProperty", "_AudioQueueStart", "_AudioQueueStop",
                          "_AudioUnitRender", "_AudioFileClose", "_AudioFileGetProperty", "_AudioFileGetPropertyInfo",
                          "_AudioFileOpenWithCallbacks", "_AudioFileReadPacketData"})
        reg(n, reinterpret_cast<void*>(&native_ret_error));
}

static void init_custom_shims() {
    g_custom_shims["_open"] = allocate_trampoline(shim_open, "_open");
    // setjmp family: asm, tail-bound, guest-sized (192-byte) jmp_buf
    g_custom_shims["_setjmp"] = reinterpret_cast<void*>(&darwin_setjmp);
    g_custom_shims["__setjmp"] = reinterpret_cast<void*>(&darwin_setjmp);
    g_custom_shims["_sigsetjmp"] = reinterpret_cast<void*>(&darwin_setjmp);
    g_custom_shims["_longjmp"] = reinterpret_cast<void*>(&darwin_longjmp);
    g_custom_shims["__longjmp"] = reinterpret_cast<void*>(&darwin_longjmp);
    g_custom_shims["_siglongjmp"] = reinterpret_cast<void*>(&darwin_longjmp);
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
    g_custom_shims["_pthread_attr_setdetachstate"] = reinterpret_cast<void*>(&native_pthread_attr_setdetachstate);
    g_custom_shims["_pthread_attr_setschedpolicy"] = reinterpret_cast<void*>(&native_pthread_attr_setschedpolicy);
    g_custom_shims["_pthread_attr_setschedparam"] = reinterpret_cast<void*>(&native_pthread_attr_setschedparam);

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
    {
        auto reg = [](const char* n, void* f) { g_custom_shims[n] = f; };
        if (getenv("LOADER_ENGINE")) {   // engine mode only: keep the default run on plain bionic dlopen/dlsym
            reg("_mach_thread_self", reinterpret_cast<void*>(&native_mach_thread_self_real));
            reg("_pthread_mach_thread_np", reinterpret_cast<void*>(&native_pthread_mach_thread_np_real));
            reg("_task_threads", reinterpret_cast<void*>(&native_task_threads));
            reg("_thread_suspend", reinterpret_cast<void*>(&native_thread_suspend));
            reg("_thread_resume", reinterpret_cast<void*>(&native_thread_resume));
            reg("_thread_get_state", reinterpret_cast<void*>(&native_thread_get_state));
            reg("_dlopen", reinterpret_cast<void*>(&native_guest_dlopen));
            reg("_dlsym", reinterpret_cast<void*>(&native_guest_dlsym));
            reg("_pthread_create", reinterpret_cast<void*>(&native_pthread_create_logged));
            register_darwin_framework_stubs();
            if (!getenv("LOADER_NO_OBJC_RT")) register_objc_runtime_natives();
        }
        reg("___tolower", reinterpret_cast<void*>(&native_tolower));
        reg("___toupper", reinterpret_cast<void*>(&native_toupper));
        reg("___maskrune", reinterpret_cast<void*>(&native_maskrune));
        reg("___sincos_stret", reinterpret_cast<void*>(&native_sincos_stret));
        reg("___sincosf_stret", reinterpret_cast<void*>(&native_sincosf_stret));
        reg("___exp10f", reinterpret_cast<void*>(&native_exp10f));
        reg("_atexit", reinterpret_cast<void*>(&native_atexit));
        reg("_clock_gettime_nsec_np", reinterpret_cast<void*>(&native_clock_gettime_nsec_np));
        reg("_dispatch_once", reinterpret_cast<void*>(&native_dispatch_once));
        reg("_dispatch_once_f", reinterpret_cast<void*>(&native_dispatch_once_f));
        reg("___assert_rtn", reinterpret_cast<void*>(&native_assert_rtn));
        reg("__Block_object_assign", reinterpret_cast<void*>(&native_block_object_assign));
        reg("__Block_object_dispose", reinterpret_cast<void*>(&native_block_object_dispose));
        reg("___objc_personality_v0", reinterpret_cast<void*>(&native_objc_personality));
        reg("_dispatch_time", reinterpret_cast<void*>(&native_dispatch_time));
        reg("_dispatch_source_create", reinterpret_cast<void*>(&native_dispatch_fake_handle));
        for (const char* n : {"_dispatch_source_cancel", "_dispatch_source_set_event_handler", "_dispatch_source_set_timer",
                              "_imp_implementationWithBlock"})
            reg(n, reinterpret_cast<void*>(&native_null));
        for (const char* n : {"_mach_msg", "_mach_make_memory_entry_64", "_mach_port_move_member", "_mach_port_request_notification",
                              "_task_get_exception_ports", "_task_swap_exception_ports", "_thread_get_exception_ports",
                              "_thread_swap_exception_ports", "_task_threads", "_task_info", "_thread_get_state",
                              "_thread_resume", "_thread_suspend", "_vm_map", "_vm_read_overwrite", "_exception_raise",
                              "_exception_raise_state", "_exception_raise_state_identity"})
            reg(n, reinterpret_cast<void*>(&native_kern_failure));
        reg("_pthread_cond_timedwait_relative_np", reinterpret_cast<void*>(&native_cond_timedwait_relative_np));
    }
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
    g_custom_shims["_malloc"] = reinterpret_cast<void*>(&native_guest_malloc);
    g_custom_shims["_calloc"] = reinterpret_cast<void*>(&native_guest_calloc);
    g_custom_shims["_realloc"] = reinterpret_cast<void*>(&native_guest_realloc);
    g_custom_shims["_free"] = reinterpret_cast<void*>(&native_guest_free);
    g_custom_shims["_posix_memalign"] = reinterpret_cast<void*>(&native_guest_posix_memalign);
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

    // Dynamic ObjC message dispatch; engine mode already bound the guest-runtime dispatchers, which must win
    g_custom_shims.emplace("_objc_msgSend", allocate_trampoline(shim_objc_msgSend, "_objc_msgSend"));
    g_custom_shims.emplace("_objc_msgSendSuper2", allocate_trampoline(shim_objc_msgSend, "_objc_msgSendSuper2"));

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

    // Engine mode: Mach thread emulation must override the earlier KERN_FAILURE stubs and constant-port versions.
    if (getenv("LOADER_ENGINE")) {
        g_custom_shims["_mach_thread_self"] = reinterpret_cast<void*>(&native_mach_thread_self_real);
        g_custom_shims["_pthread_mach_thread_np"] = reinterpret_cast<void*>(&native_pthread_mach_thread_np_real);
        g_custom_shims["_task_threads"] = reinterpret_cast<void*>(&native_task_threads);
        g_custom_shims["_thread_suspend"] = reinterpret_cast<void*>(&native_thread_suspend);
        g_custom_shims["_thread_resume"] = reinterpret_cast<void*>(&native_thread_resume);
        g_custom_shims["_thread_get_state"] = reinterpret_cast<void*>(&native_thread_get_state);
    }

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
        void* cls;
        if (getenv("LOADER_ENGINE")) {   // engine mode: a named fake class object that messages can be answered for
            cls = name.rfind("_OBJC_CLASS_$_", 0) == 0 ? reinterpret_cast<void*>(fake_class_named(name.substr(14)))
                                                         : reinterpret_cast<void*>(fake_make(7, name.substr(18)));
        } else {
            cls = calloc(1, 128);
        }
        s_dummy_classes[name] = cls;
        g_resolved_cache[name] = cls;
        return cls;
    }

    // Check dlsym for standard C/POSIX/libc++ symbols
    const char* stripped = name.c_str();
    if (stripped[0] == '_') stripped++; // Strip leading underscore

    // Records a symbol bound straight to bionic; in trace mode, lazy (call) targets
    // get a logging forwarder so every call shows up with its guest lr.
    auto note_direct = [&](void* target, bool) -> void* {
        bool lazy = g_lazy_names.count(name) != 0;
        // dlsym on the libc++ handle also finds libc symbols (dependency), so classify by owner module.
        Dl_info di{};
        bool from_libcxx = dladdr(target, &di) && di.dli_fname && std::strstr(di.dli_fname, "libc++") != nullptr;
        g_direct_bound.push_back({name, from_libcxx, lazy, di.dli_fname ? base_name(di.dli_fname) : "?"});
        if (((g_trace_mode == 1 || g_trace_mode == 2) && !from_libcxx && lazy) ||
            (g_trace_unwind && is_unwind_hook_name(name))) {
            target = make_direct_trace_stub(target, strdup(name.c_str()));
        }
        g_resolved_cache[name] = target;
        return target;
    };

    // Check system libc++.so first for C++ symbols
    if (g_libcxx_handle) {
        void* csym = dlsym(g_libcxx_handle, name.c_str());
        if (csym) return note_direct(csym, true);
        if (stripped != name.c_str()) {
            csym = dlsym(g_libcxx_handle, stripped);
            if (csym) return note_direct(csym, true);
        }
    }

    void* sym = dlsym(RTLD_DEFAULT, stripped);
    if (sym) return note_direct(sym, false);

    // Try without stripping if name started with __Z
    if (name.rfind("__Z", 0) == 0) {
        sym = dlsym(RTLD_DEFAULT, name.c_str());
        if (sym) return note_direct(sym, false);
    }

    // Objective-C runtime functions
    if (name.rfind("_objc_", 0) == 0) {
        void* stub = allocate_trampoline(shim_objc_stub, strdup(name.c_str()));
        g_resolved_cache[name] = stub;
        return stub;
    }

    bool is_fn = is_function_import(name);
    if (g_unresolved_set.insert(name).second) {
        fprintf(stderr, "[UNRESOLVED IMPORT] %s %s\n", is_fn ? "FUNCTION" : "DATA    ", name.c_str());
        g_unresolved_imports.push_back(name);
        if (is_fn) g_unresolved_fn_names.push_back(name);
    }
    if (!is_fn && getenv("LOADER_ENGINE") &&
        (name.rfind("_OBJC_CLASS_$_", 0) == 0 || name.rfind("_OBJC_METACLASS_$_", 0) == 0)) {
        // Engine mode: Objective-C class references to frameworks we do not have become fake class objects
        // (kind 7) instead of poison pages, so +class messages are answered rather than faulting.
        void* obj = name.rfind("_OBJC_CLASS_$_", 0) == 0 ? reinterpret_cast<void*>(fake_class_named(name.substr(14)))
                                                          : reinterpret_cast<void*>(fake_make(7, name.substr(18)));
        g_resolved_cache[name] = obj;
        return obj;
    }
    if (!is_fn) {
        // DATA import: PROT_NONE poison slot, so the first touch names the symbol.
        if (void* slot = poison_slot_for(name)) {
            g_resolved_cache[name] = slot;
            return slot;
        }
    }
    void* stub = allocate_trampoline(shim_unresolved, strdup(name.c_str()));
    g_resolved_cache[name] = stub;
    return stub;
}

static void il2cpp_log_callback(const char* log) {
    printf("[IL2CPP LOG] %s\n", log ? log : "");
}

#include <ucontext.h>

// ---- watchdog: LOADER_WATCHDOG=<seconds> dumps every thread's stack (SIGUSR2 to each tid), then exits 124 ----
#include <sys/syscall.h>
#include <thread>
static volatile int g_wd_done = 0;
static void thread_dump_handler(int, siginfo_t*, void* ctx) {
    ucontext_t* uc = reinterpret_cast<ucontext_t*>(ctx);
    uint64_t pc = uc->uc_mcontext.pc, sp = uc->uc_mcontext.sp, fp = uc->uc_mcontext.regs[29], lr = uc->uc_mcontext.regs[30];
    char comm[32] = {0};
    int cf = open("/proc/thread-self/comm", O_RDONLY);
    if (cf >= 0) { ssize_t n = read(cf, comm, sizeof comm - 1); if (n > 0 && comm[n - 1] == '\n') comm[n - 1] = 0; close(cf); }
    wr_str(2, "\n[THREAD] tid="); wr_hex(2, static_cast<uint64_t>(syscall(SYS_gettid)));
    wr_str(2, " comm="); wr_str(2, comm);
    wr_str(2, " syscall(x8)="); wr_hex(2, uc->uc_mcontext.regs[8]);
    wr_str(2, " x0="); wr_hex(2, uc->uc_mcontext.regs[0]);
    wr_str(2, " x1="); wr_hex(2, uc->uc_mcontext.regs[1]);
    wr_str(2, " x2="); wr_hex(2, uc->uc_mcontext.regs[2]);
    wr_str(2, "\n[THREAD]   pc -> "); describe_addr(2, pc);
    wr_str(2, "\n[THREAD]   lr -> "); describe_addr(2, lr);
    wr_str(2, "\n");
    uint64_t prev = sp;
    for (int i = 0; i < 28 && fp > prev && fp - sp < (8u << 20) && (fp & 7) == 0; ++i) {
        uint64_t ret = reinterpret_cast<uint64_t*>(fp)[1];
        wr_str(2, "[THREAD]   #"); wr_hex(2, i); wr_str(2, " "); describe_addr(2, ret & 0x0000007fffffffffull); wr_str(2, "\n");
        prev = fp;
        fp = reinterpret_cast<uint64_t*>(fp)[0];
    }
    g_wd_done = 1;
}
static void start_watchdog(unsigned seconds) {
    struct sigaction sa{};
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = thread_dump_handler;
    sigaction(SIGUSR2, &sa, nullptr);
    int count = getenv("LOADER_WATCHDOG_COUNT") ? atoi(getenv("LOADER_WATCHDOG_COUNT")) : 1;
    std::thread([seconds, count] {
        for (int round = 1; round <= count; ++round) {
            sleep(seconds);
            timespec cpu{}; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu);
            fprintf(stderr, "\n[WATCHDOG] sample %d/%d at +%u s (process cpu %.1f s): dumping all threads\n", round, count, seconds * round,
                    cpu.tv_sec + cpu.tv_nsec / 1e9);
            fflush(stderr);
            pid_t pid = getpid();
            if (DIR* d = opendir("/proc/self/task")) {
                std::vector<int> tids;
                while (dirent* e = readdir(d)) if (e->d_name[0] != '.') tids.push_back(atoi(e->d_name));
                closedir(d);
                for (int tid : tids) {
                    g_wd_done = 0;
                    syscall(SYS_tgkill, pid, tid, SIGUSR2);
                    for (int i = 0; i < 50 && !g_wd_done; ++i) usleep(10000);
                }
            }
        }
        fprintf(stderr, "[WATCHDOG] done\n");
        fflush(stderr);
        _exit(124);
    }).detach();
}

static void segv_handler(int sig, siginfo_t* info, void* ctx) {
    ucontext_t* uc = reinterpret_cast<ucontext_t*>(ctx);
    uint64_t pc = uc->uc_mcontext.pc;
    uint64_t sp = uc->uc_mcontext.sp;
    uint64_t lr = uc->uc_mcontext.regs[30];
    uint64_t fp = uc->uc_mcontext.regs[29];
    {
        uint64_t fa = reinterpret_cast<uint64_t>(info->si_addr);
        uint64_t pb = reinterpret_cast<uint64_t>(g_poison_base);
        if (pb && fa >= pb && fa < pb + static_cast<uint64_t>(POISON_SLOT_SIZE) * POISON_SLOTS) {
            size_t slot = (fa - pb) / POISON_SLOT_SIZE;
            wr_str(2, "[POISON] guest touched unresolved DATA import '");
            wr_str(2, slot < g_poison_names.size() ? g_poison_names[slot].c_str() : "?");
            wr_str(2, "' at slot+"); wr_hex(2, (fa - pb) % POISON_SLOT_SIZE);
            wr_str(2, pc == fa ? " (EXECUTED as code: probably a function, misclassified as data)\n"
                               : " (data read/write)\n");
            wr_str(2, "[POISON]   pc -> "); describe_addr(2, pc); wr_str(2, "\n");
            wr_str(2, "[POISON]   lr -> "); describe_addr(2, lr); wr_str(2, "\n");
            if (g_poison_continue && pc != fa) {
                ++g_poison_touch_count;
                mprotect(g_poison_base + slot * POISON_SLOT_SIZE, POISON_SLOT_SIZE, PROT_READ | PROT_WRITE);
                wr_str(2, "[POISON]   continue mode: page now readable (zeros); resuming\n");
                return;
            }
        }
    }
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

// Asks the system unwinder (bionic libc.so, LLVM libunwind) directly whether it can find an FDE
// for a few addresses. Positive controls (loader, libc, libc++) must return non-null; a null for
// guest addresses means any _Unwind_RaiseException passing through guest frames ends there.
struct EhBases { void* tbase; void* dbase; void* func; };
struct PhdrScan {
    uint64_t pc;
    int objects;
    int with_eh_frame_hdr;
    const char* hit;
};

static int phdr_scan_cb(struct dl_phdr_info* info, size_t, void* data) {
    auto* s = static_cast<PhdrScan*>(data);
    ++s->objects;
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr)& ph = info->dlpi_phdr[i];
        if (ph.p_type == PT_GNU_EH_FRAME) ++s->with_eh_frame_hdr;
        if (ph.p_type == PT_LOAD) {
            uint64_t lo = info->dlpi_addr + ph.p_vaddr;
            if (s->pc >= lo && s->pc < lo + ph.p_memsz) s->hit = info->dlpi_name ? info->dlpi_name : "(unnamed)";
        }
    }
    return 0;
}

static void unwinder_probe(uint64_t guest_pc_a, uint64_t guest_pc_b) {
    typedef const void* (*find_fde_fn)(void*, EhBases*);
    auto find_fde = reinterpret_cast<find_fde_fn>(dlsym(RTLD_DEFAULT, "_Unwind_Find_FDE"));
    fprintf(stderr, "\n[UNWIND-PROBE] _Unwind_Find_FDE=%p __register_frame=%p\n", reinterpret_cast<void*>(find_fde),
            dlsym(RTLD_DEFAULT, "__register_frame"));
    PhdrScan scan{guest_pc_a, 0, 0, nullptr};
    dl_iterate_phdr(phdr_scan_cb, &scan);
    fprintf(stderr, "[UNWIND-PROBE] dl_iterate_phdr: %d objects (%d with PT_GNU_EH_FRAME); guest pc %#llx covered by: %s\n",
            scan.objects, scan.with_eh_frame_hdr, static_cast<unsigned long long>(guest_pc_a),
            scan.hit ? scan.hit : "NO OBJECT (guest image is invisible to the unwinder)");
    if (!find_fde) return;
    struct Probe { const char* what; uint64_t pc; };
    const Probe probes[] = {
        {"guest  (control: should be NULL)", guest_pc_a},
        {"guest  (control: should be NULL)", guest_pc_b},
        {"loader fn      (positive control)", reinterpret_cast<uint64_t>(&init_safe_read)},
        {"libc abort()   (positive control)", reinterpret_cast<uint64_t>(dlsym(RTLD_DEFAULT, "abort"))},
        {"libc++ __cxa_throw (positive control)", reinterpret_cast<uint64_t>(dlsym(RTLD_DEFAULT, "__cxa_throw"))},
    };
    for (const auto& p : probes) {
        EhBases b{};
        const void* fde = find_fde(reinterpret_cast<void*>(p.pc + 4), &b);
        fprintf(stderr, "[UNWIND-PROBE] _Unwind_Find_FDE(%#llx) %-38s -> fde=%p func=%p\n",
                static_cast<unsigned long long>(p.pc + 4), p.what, fde, b.func);
    }
}

extern "C" int loader_main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    if (const char* dir = getenv("LOADER_MSL_TEST")) return gles::msl_selftest(dir) == 0 ? 0 : 1;

    init_safe_read();
    if (const char* tm = getenv("LOADER_TRACE_DIRECT")) {
        g_trace_mode = std::strcmp(tm, "all") == 0 ? 2 : std::strcmp(tm, "list") == 0 ? 3 : 1;
    }
    g_poison_continue = getenv("LOADER_POISON_STRICT") == nullptr;
    g_trace_unwind = getenv("LOADER_EXCEPTION_TEST") != nullptr || getenv("LOADER_TRACE_UNWIND") != nullptr;
    struct sigaction sa{};
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = segv_handler;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);  // std::terminate / abort() -> full dump
    sigaction(SIGILL, &sa, nullptr);
    if (const char* wd = getenv("LOADER_WATCHDOG")) start_watchdog(static_cast<unsigned>(atoi(wd)));

    printf("=== Native ARM64 Mach-O Android Loader ===\n");

    // 1. Calculate TSD offset
    uint64_t tpidr = get_tpidr_el0();
    uint64_t tsd_addr = reinterpret_cast<uint64_t>(&g_apple_tsd[0]);
    g_tsd_tprel_diff = static_cast<int64_t>(tsd_addr - tpidr);
    printf("[*] TPIDR_EL0: 0x%lx, Apple TSD: 0x%lx, diff: %ld (0x%lx)\n",
           tpidr, tsd_addr, (long)g_tsd_tprel_diff, (unsigned long)g_tsd_tprel_diff);

    // Load system libc++.so (which uses std::__1 ABI on Android)
    // An app's linker namespace cannot open /system/lib64/libc++.so; the APK ships a copy (LOADER_LIBCXX).
    if (const char* lc = getenv("LOADER_LIBCXX")) g_libcxx_handle = dlopen(lc, RTLD_NOW | RTLD_GLOBAL);
    if (!g_libcxx_handle) g_libcxx_handle = dlopen("/system/lib64/libc++.so", RTLD_NOW | RTLD_GLOBAL);
    if (!g_libcxx_handle) {
        printf("[-] dlopen /system/lib64/libc++.so failed: %s\n", dlerror());
    } else {
        printf("[+] Loaded /system/lib64/libc++.so (std::__1 ABI)\n");
    }

    // 3. Open UnityFramework binary
    const char* bin_path = game_binary_path().c_str();
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
    uint64_t unwind_info_vm = 0, unwind_info_size = 0, eh_frame_vm = 0, eh_frame_size = 0;
    uint64_t objc_classlist_vm = 0, objc_classlist_size = 0, objc_catlist_vm = 0, objc_catlist_size = 0, objc_methname_vm = 0, objc_methname_size = 0;

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
            uint32_t nsects = *reinterpret_cast<uint32_t*>(file_data + cur + 64);
            for (uint32_t k = 0; k < nsects; ++k) {
                const uint8_t* sp = file_data + cur + 72 + 80 * k;
                uint64_t saddr = *reinterpret_cast<const uint64_t*>(sp + 32);
                uint64_t ssize = *reinterpret_cast<const uint64_t*>(sp + 40);
                if (std::strncmp(reinterpret_cast<const char*>(sp), "__unwind_info", 16) == 0) { unwind_info_vm = saddr; unwind_info_size = ssize; }
                if (std::strncmp(reinterpret_cast<const char*>(sp), "__eh_frame", 16) == 0) { eh_frame_vm = saddr; eh_frame_size = ssize; }
                if (std::strncmp(reinterpret_cast<const char*>(sp), "__objc_classlist", 16) == 0) { objc_classlist_vm = saddr; objc_classlist_size = ssize; }
                if (std::strncmp(reinterpret_cast<const char*>(sp), "__objc_catlist", 16) == 0) { objc_catlist_vm = saddr; objc_catlist_size = ssize; }
                if (std::strncmp(reinterpret_cast<const char*>(sp), "__objc_methname", 16) == 0) { objc_methname_vm = saddr; objc_methname_size = ssize; }
            }
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
    auto apply_bind_stream = [&](uint32_t off, uint32_t size, const char* sname, bool collect_only = false) {
        if (size == 0) return;
        const uint8_t* ptr = file_data + off;
        const uint8_t* end = ptr + size;
        uint32_t s_idx = 0;
        uint64_t s_offset = 0;
        std::string symbol = "";
        size_t b_count = 0;
        int64_t addend = 0; // BIND_OPCODE_SET_ADDEND_SLEB; sticks until the next SET_ADDEND_SLEB, as in dyld

        auto do_bind = [&](uint8_t* loc) {
            if (collect_only) {
                g_lazy_names.insert(symbol);
                return;
            }
            void* target = resolve_symbol(symbol);
            *reinterpret_cast<uint64_t*>(loc) = reinterpret_cast<uint64_t>(target) + static_cast<uint64_t>(addend);
            g_binds.push_back({reinterpret_cast<uint64_t>(loc),
                               reinterpret_cast<uint64_t>(target),
                               strdup(symbol.c_str()),
                               g_unresolved_set.count(symbol) != 0,
                               addend});
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
            else if (op == 0x60) { // SET_ADDEND_SLEB
                int64_t v = 0;
                unsigned shift = 0;
                uint8_t b;
                do {
                    b = *ptr++;
                    v |= static_cast<int64_t>(b & 0x7f) << shift;
                    shift += 7;
                } while (b & 0x80);
                if (shift < 64 && (b & 0x40)) v |= -(static_cast<int64_t>(1) << shift);
                addend = v;
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
        if (!collect_only) printf("[+] Applied %zu bindings in %s stream\n", b_count, sname);
    };

    // Pre-pass: learn which names are call targets (lazy stream) before resolving anything.
    apply_bind_stream(lazy_off, lazy_size, "lazy", true);
    printf("[*] %zu unique symbols in lazy (function) stream\n", g_lazy_names.size());
    apply_bind_stream(bind_off, bind_size, "regular");
    apply_bind_stream(lazy_off, lazy_size, "lazy");

    // Addend self-check: every bind with an addend must satisfy slot == resolved symbol + addend.
    {
        size_t n_addend = 0, n_ok = 0, n_nonzero_bad = 0, n_name_hi = 0, n_name_noncanon = 0;
        const char* hi_example = nullptr;
        uint64_t hi_example_val = 0;
        for (const auto& b : g_binds) {
            if (b.addend == 0) continue;
            n_addend++;
            uint64_t slot = *reinterpret_cast<uint64_t*>(b.loc);
            if (slot == b.target + static_cast<uint64_t>(b.addend)) n_ok++; else n_nonzero_bad++;
            // typeinfo object: word[0] = vptr (this slot), word[1] = name pointer.
            // Apple arm64 libc++ uses bit 63 of the name pointer as a non-unique-RTTI flag.
            uint64_t name = reinterpret_cast<uint64_t*>(b.loc)[1];
            if (name >> 63) {
                n_name_hi++;
                if (!hi_example) { hi_example = b.symbol; hi_example_val = name; }
                // Apple's non-unique-RTTI flag; Android's libc++ would deref the raw word. Clear it.
                reinterpret_cast<uint64_t*>(b.loc)[1] = name & ~(1ull << 63);
            } else if ((name >> 48) != 0) {
                n_name_noncanon++;
            }
        }
        printf("[*] Bind addends: %zu addended binds, %zu slot==sym+addend OK, %zu mismatched\n",
               n_addend, n_ok, n_nonzero_bad);
        printf("[*] Addended-bind name pointers (word[1]): %zu with bit 63 set, %zu other non-canonical\n",
               n_name_hi, n_name_noncanon);
        if (hi_example)
            printf("[*]   bit-63 cleared on %zu name words; example: vptr symbol %s, name word was 0x%llx\n", n_name_hi,
                   hi_example, static_cast<unsigned long long>(hi_example_val));
    }
    printf("[*] Unresolved imports: %zu unique (%zu function -> shim_unresolved, %zu data -> poison)\n",
           g_unresolved_imports.size(), g_unresolved_fn_names.size(), g_poison_names.size());
    for (const auto& n : g_unresolved_imports) {
        fprintf(stderr, "  UNRESOLVED %s %s\n", is_function_import(n) ? "fn  " : "data", n.c_str());
    }
    if (g_trace_mode) dump_direct_bound_list();

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
    selftest_setjmp();

    // Compact unwind -> DWARF FDEs for the system unwinder (opt-in).
    //   LOADER_FDE=all              register every guest function
    //   LOADER_FDE_ONLY=<file>      register only functions containing the image offsets listed in <file>
    //   LOADER_FDE_DUMP=1           log every registered function
    {
        const char* fde_all = getenv("LOADER_FDE");
        const char* fde_only = getenv("LOADER_FDE_ONLY");
        if ((fde_all && *fde_all) || (fde_only && *fde_only)) {
            guest_fde::Options fo;
            fo.dump = getenv("LOADER_FDE_DUMP") != nullptr;
            if (fde_only && *fde_only) {
                fo.only = true;
                FILE* lf = fopen(fde_only, "r");
                if (!lf) { fprintf(stderr, "[FATAL] LOADER_FDE_ONLY: cannot open %s\n", fde_only); return 1; }
                char line[256];
                while (fgets(line, sizeof line, lf)) {
                    const char* x = strstr(line, "0x");
                    if (x) fo.only_offsets.push_back(strtoull(x, nullptr, 16));
                }
                fclose(lf);
                fprintf(stderr, "[FDE] subset mode: %zu addresses from %s\n", fo.only_offsets.size(), fde_only);
            }
            if (!unwind_info_vm) { fprintf(stderr, "[FATAL] no __unwind_info section\n"); return 1; }
            const uint8_t* text_base = image_base + (text_seg->vmaddr - min_vm);
            guest_fde::Result fr;
            bool ok = guest_fde::register_all(
                text_base, image_base + (unwind_info_vm - min_vm), unwind_info_size,
                eh_frame_vm ? image_base + (eh_frame_vm - min_vm) : nullptr, eh_frame_size, fo, fr);
            if (!ok) { fprintf(stderr, "[FATAL] FDE registration failed\n"); return 1; }
        }
    }

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

    // Engine stage (opt-in): let Unity's own startup code run instead of our manual il2cpp boot.
    //   LOADER_ENGINE=1  UnityInitApplicationNoGraphics("/data/local/tmp/game")           (image+0x7cc3d4)
    //   LOADER_ENGINE=2  same, preceded by the startup helper didFinishLaunching calls first (image+0x7cde30)
    // Offsets come from macho_objc.py's static scan of -[UnityAppController application:didFinishLaunchingWithOptions:];
    // the argument list of each function is a guess until the disassembly dumps are read.
    if (const char* eng = getenv("LOADER_ENGINE")) {
        typedef void (*engine_init_fn)(const char*);
        typedef void (*engine_pre_fn)();
        mach_register_current();   // the main thread must be in the Mach thread table before the GC asks for it
        // Register the guest's own Objective-C classes/categories so messages reach their real method implementations.
        if (!getenv("LOADER_NO_OBJC_RT") && objc_classlist_vm) {
            auto rt = [&](uint64_t vm) { return reinterpret_cast<uint64_t>(image_base) + (vm - min_vm); };
            objcrt::init(reinterpret_cast<uint64_t>(image_base), reinterpret_cast<uint64_t>(image_base) + total_vmsize,
                         rt(objc_classlist_vm), objc_classlist_size, objc_catlist_vm ? rt(objc_catlist_vm) : 0, objc_catlist_size,
                         objc_methname_vm ? rt(objc_methname_vm) : 0, objc_methname_size,
                         [&](uint64_t slot) -> const char* {
                             for (const auto& b : g_binds)
                                 if (b.loc == slot) return b.symbol;
                             return nullptr;
                         });
        }
        // LOADER_PEEK=0x2bf5700,0x2b59340: print the 8-byte value at image offsets, the segment it lives in,
        // and the import it was bound to (if any).
        if (const char* peek = getenv("LOADER_PEEK")) {
            std::string list = peek;
            size_t pos = 0;
            while (pos < list.size()) {
                size_t comma = list.find(',', pos);
                std::string tok = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                pos = comma == std::string::npos ? list.size() : comma + 1;
                uint64_t off = strtoull(tok.c_str(), nullptr, 16);
                const char* segn = "?";
                for (auto& sg : segments)
                    if (sg.vmaddr <= off + min_vm && off + min_vm < sg.vmaddr + sg.vmsize) segn = sg.name.c_str();
                uint64_t val = *reinterpret_cast<uint64_t*>(image_base + off);
                const char* bound = "";
                for (const auto& b : g_binds)
                    if (b.loc == reinterpret_cast<uint64_t>(image_base + off)) { bound = b.symbol; break; }
                printf("[PEEK] image+%#llx (%s) = %#llx%s%s\n", static_cast<unsigned long long>(off), segn,
                       static_cast<unsigned long long>(val), *bound ? "  bound import: " : "", bound);
            }
        }
        printf("\n=== ENGINE STAGE (LOADER_ENGINE=%s) ===\n", eng);
        fflush(stdout);
        if (std::strcmp(eng, "2") == 0) {
            printf("[E1] startup helper image+0x7cde30()...\n"); fflush(stdout);
            reinterpret_cast<engine_pre_fn>(get_fn(0x7cde30))();
            printf("[E1] returned\n"); fflush(stdout);
        }
        if (atoi(eng) >= 5) {
            // Replay -[UnityFramework frameworkWarmup:argv:] (image+0x16850) minus the UIKit swizzling at image+0x12b90:
            // UnityInitRuntime(argc, argv) at image+0x7cc27c, then image+0x1b304 (mono/feature registration).
            // LOADER_ENGINE_ARGS="-batchmode -nographics" appends engine command-line switches (parsed at image+0x51e1d4).
            static char arg0[] = "UnityFramework";
            static std::vector<std::string> extra;
            static std::vector<char*> fake_argv_v;
            fake_argv_v.push_back(arg0);
            if (const char* ea = getenv("LOADER_ENGINE_ARGS")) {
                std::string all = ea;
                size_t i = 0;
                while (i < all.size()) {
                    size_t j = all.find_first_of(" ,", i);
                    if (j == std::string::npos) j = all.size();
                    if (j > i) extra.push_back(all.substr(i, j - i));
                    i = j + 1;
                }
                for (auto& e : extra) fake_argv_v.push_back(e.data());
            }
            fake_argv_v.push_back(nullptr);
            int fake_argc = static_cast<int>(fake_argv_v.size()) - 1;
            typedef void (*runtime_init_fn)(int, char**);
            printf("[E1a] UnityInitRuntime(%d, argv) image+0x7cc27c...\n", fake_argc); fflush(stdout);
            reinterpret_cast<runtime_init_fn>(get_fn(0x7cc27c))(fake_argc, fake_argv_v.data());
            printf("[E1a] returned\n"); fflush(stdout);
            if (!getenv("LOADER_ENGINE_SKIP_1B304")) {
                printf("[E1b] image+0x1b304()...\n"); fflush(stdout);
                reinterpret_cast<engine_pre_fn>(get_fn(0x1b304))();
                printf("[E1b] returned\n"); fflush(stdout);
            }
        }
        if (std::strcmp(eng, "3") == 0 || std::strcmp(eng, "4") == 0) {
            // image+0x31eae8 allocates the 0x58-byte registry that image+0x2b59340 should point to (xref.txt);
            // its only static caller is on the scene-loading path, so create it up front.
            printf("[E1] registry ctor image+0x31eae8() (global before: %p)...\n",
                   *reinterpret_cast<void**>(get_fn(0x2b59340))); fflush(stdout);
            reinterpret_cast<engine_pre_fn>(get_fn(0x31eae8))();
            printf("[E1] registry global now %p\n", *reinterpret_cast<void**>(get_fn(0x2b59340))); fflush(stdout);
        }
        // image+0x1516c caches the app bundle path in image+0x2b428c8 and returns it as-is when set, so seed it
        // and the engine never needs [[NSBundle mainBundle] bundlePath].
        {
            char** bundle_cache = reinterpret_cast<char**>(get_fn(0x2b428c8));
            printf("[E0] bundle path cache (image+0x2b428c8) was %p; seeding \"%s\"\n", static_cast<void*>(*bundle_cache), game_dir());
            *bundle_cache = strdup(game_dir());
        }
        if (atoi(eng) >= 8) {
            // The app launch itself: -[UnityAppController application:didFinishLaunchingWithOptions:] runs engine init,
            // view/display setup and startUnity: (queued by applicationDidBecomeActive:), then the display link ticks.
            const char* fr = getenv("LOADER_FRAMES");
            fakeui_run_application(fr ? atoi(fr) : 300);
            // engine threads are still running; static destructors must not run under them
            fflush(stdout);
            fflush(stderr);
            _exit(0);
        }
        printf("[E2] UnityInitApplicationNoGraphics(\"%s\") image+0x7cc3d4...\n", game_dir()); fflush(stdout);
        reinterpret_cast<engine_init_fn>(get_fn(0x7cc3d4))(game_dir());
        printf("[E2] returned: engine init without graphics completed\n"); fflush(stdout);
        if (atoi(eng) >= 6) {
            // startUnity's next step: graphics init at image+0x7cc5c4. It asks image+0x170dc for the rendering API and
            // treats -1 as "none", passing a no-graphics flag to PlayerInitEngineGraphics (image+0x4ae350).
            // -[UnityAppController selectRenderingAPI] (image+0x1720c) probes Metal via dlopen/dlsym and records the choice;
            // -[UnityAppController renderingAPI] (image+0x17360) reads it back. self only needs to be valid memory.
            {
                typedef void (*sel_api_fn)(void*, void*);
                typedef int (*get_api_fn)(void*, void*);
                void* fake_self = calloc(1, 1024);
                printf("[E2b] selectRenderingAPI image+0x1720c...\n"); fflush(stdout);
                reinterpret_cast<sel_api_fn>(get_fn(0x1720c))(fake_self, nullptr);
                printf("[E2b] renderingAPI -> %d\n", reinterpret_cast<get_api_fn>(get_fn(0x17360))(fake_self, nullptr)); fflush(stdout);
            }
            printf("[E3] UnityInitApplicationGraphics image+0x7cc5c4...\n"); fflush(stdout);
            reinterpret_cast<engine_pre_fn>(get_fn(0x7cc5c4))();
            printf("[E3] returned\n"); fflush(stdout);
        }
        if (atoi(eng) >= 7) {
            // image+0x7cc6cc reaches PlayerLoadFirstScene (image+0x4ae81c: 'level0', 'sharedassets0.assets').
            printf("[E4] load application / first scene image+0x7cc6cc...\n"); fflush(stdout);
            reinterpret_cast<engine_pre_fn>(get_fn(0x7cc6cc))();
            printf("[E4] returned\n"); fflush(stdout);
        }
        return 0;
    }

    printf("\n=== EXECUTING HEADLESS IL2CPP BOOT ON S24+ ===\n");

    printf("[1/5] Registering log callback...\n");
    il2cpp_register_log_callback(il2cpp_log_callback);

    std::string data_dir = std::string(game_dir()) + "/Data";
    printf("[2/5] Setting data dir: %s...\n", data_dir.c_str());
    il2cpp_set_data_dir(data_dir.c_str());

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

    // [6/5] Invoke one simple managed method through il2cpp_runtime_invoke.
    typedef const char* (*image_get_name_fn)(void*);
    typedef void* (*class_from_name_fn)(void*, const char*, const char*);
    typedef void* (*class_get_methods_fn)(void*, void**);
    typedef const char* (*method_get_name_fn)(void*);
    typedef uint32_t (*method_get_param_count_fn)(void*);
    typedef void* (*method_get_param_fn)(void*, uint32_t);
    typedef void* (*method_get_return_type_fn)(void*);
    typedef uint32_t (*method_get_flags_fn)(void*, uint32_t*);
    typedef char* (*type_get_name_fn)(void*);
    typedef void* (*runtime_invoke_fn)(void*, void*, void**, void**);
    typedef void* (*object_unbox_fn)(void*);
    typedef void* (*object_get_class_fn)(void*);
    typedef void (*format_exception_fn)(void*, char*, int);
    typedef int (*string_length_fn)(void*);
    typedef uint16_t* (*string_chars_fn)(void*);
    auto image_get_name = reinterpret_cast<image_get_name_fn>(get_fn(0xbe0128));
    auto class_from_name = reinterpret_cast<class_from_name_fn>(get_fn(0xbdee6c));
    auto class_get_methods = reinterpret_cast<class_get_methods_fn>(get_fn(0xbdeefc));
    auto method_get_name = reinterpret_cast<method_get_name_fn>(get_fn(0xbdfa9c));
    auto method_get_param_count = reinterpret_cast<method_get_param_count_fn>(get_fn(0xbdfadc));
    auto method_get_param = reinterpret_cast<method_get_param_fn>(get_fn(0xbdfaec));
    auto method_get_return_type = reinterpret_cast<method_get_return_type_fn>(get_fn(0xbdfa6c));
    auto method_get_flags = reinterpret_cast<method_get_flags_fn>(get_fn(0xbdfb2c));
    auto type_get_name = reinterpret_cast<type_get_name_fn>(get_fn(0xbdff64));
    auto runtime_invoke = reinterpret_cast<runtime_invoke_fn>(get_fn(0xbdfd48));
    auto object_unbox = reinterpret_cast<object_unbox_fn>(get_fn(0xbdfc94));
    auto object_get_class = reinterpret_cast<object_get_class_fn>(get_fn(0xbdfc40));
    auto format_exception = reinterpret_cast<format_exception_fn>(get_fn(0xbdf58c));
    auto string_length = reinterpret_cast<string_length_fn>(get_fn(0xbdfdac));
    auto string_chars = reinterpret_cast<string_chars_fn>(get_fn(0xbdfdbc));

    printf("\n[6/5] Invoking a managed static method via il2cpp_runtime_invoke...\n");
    void* mscorlib = nullptr;
    for (size_t i = 0; i < asm_count; ++i) {
        void* img = il2cpp_assembly_get_image(assemblies[i]);
        const char* iname = image_get_name(img);
        if (iname && std::strcmp(iname, "mscorlib.dll") == 0) { mscorlib = img; break; }
    }
    printf("[*] mscorlib.dll image: %p\n", mscorlib);

    struct Candidate {
        const char* ns; const char* cls; const char* method; int nparams;
        const char* p0; const char* p1;
    };
    const Candidate candidates[] = {
        {"System", "Math", "Max", 2, "System.Int32", "System.Int32"},
        {"System", "Math", "Abs", 1, "System.Int32", nullptr},
        {"System", "String", "Concat", 2, "System.String", "System.String"},
        {"System", "Int32", "Parse", 1, "System.String", nullptr},
    };

    void* chosen = nullptr;
    const Candidate* chosen_c = nullptr;
    for (const auto& c : candidates) {
        if (!mscorlib) break;
        void* klass = class_from_name(mscorlib, c.ns, c.cls);
        printf("[*] class %s.%s -> %p\n", c.ns, c.cls, klass);
        if (!klass) continue;
        void* iter = nullptr;
        size_t listed = 0;
        while (void* m = class_get_methods(klass, &iter)) {
            const char* mn = method_get_name(m);
            if (!mn || std::strcmp(mn, c.method) != 0) continue;
            uint32_t pc = method_get_param_count(m);
            uint32_t fl = method_get_flags(m, nullptr);
            bool is_static = (fl & 0x10) != 0;
            std::string sig;
            bool match = pc == static_cast<uint32_t>(c.nparams) && is_static;
            for (uint32_t k = 0; k < pc; ++k) {
                char* tn = type_get_name(method_get_param(m, k));
                sig += (k ? ", " : ""); sig += tn ? tn : "?";
                const char* want = k == 0 ? c.p0 : c.p1;
                if (match && (!want || !tn || std::strcmp(tn, want) != 0)) match = false;
            }
            ++listed;
            printf("    %s.%s(%s) static=%d%s\n", c.cls, mn, sig.c_str(), is_static, match ? "  <== match" : "");
            if (match && !chosen) { chosen = m; chosen_c = &c; }
        }
        if (listed == 0) printf("    (no methods named %s found; likely stripped)\n", c.method);
        if (chosen) break;
    }

    if (!chosen) {
        printf("[-] [6/5] no suitable candidate method found in mscorlib (see listing above)\n");
    } else {
        printf("[*] Invoking %s.%s.%s (method=%p)\n", chosen_c->ns, chosen_c->cls, chosen_c->method, chosen);
        int a = 7, b = 42;
        void* str_a = nullptr;
        void* str_b = nullptr;
        void* args[2] = {nullptr, nullptr};
        if (std::strcmp(chosen_c->p0, "System.Int32") == 0) {
            args[0] = &a;
            args[1] = chosen_c->nparams > 1 ? static_cast<void*>(&b) : nullptr;
            printf("[*] args: int %d%s\n", a, chosen_c->nparams > 1 ? ", int 42" : "");
        } else {
            str_a = il2cpp_string_new(chosen_c->nparams > 1 ? "Hello, " : "1234");
            str_b = chosen_c->nparams > 1 ? il2cpp_string_new("il2cpp") : nullptr;
            args[0] = str_a;
            args[1] = str_b;
            printf("[*] args: string objects %p %p\n", str_a, str_b);
        }
        void* exc = nullptr;
        void* ret = runtime_invoke(chosen, nullptr, args, &exc);
        printf("[*] il2cpp_runtime_invoke returned object=%p exception=%p\n", ret, exc);
        if (exc) {
            char msg[2048] = {0};
            format_exception(exc, msg, sizeof(msg));
            void* ek = object_get_class(exc);
            printf("[-] managed exception (%s): %s\n", ek ? il2cpp_class_get_name(ek) : "?", msg);
        } else if (ret) {
            char* rtn = type_get_name(method_get_return_type(chosen));
            printf("[*] return type: %s\n", rtn ? rtn : "?");
            if (rtn && std::strcmp(rtn, "System.String") == 0) {
                int len = string_length(ret);
                const uint16_t* ch = string_chars(ret);
                std::string out;
                for (int i = 0; i < len; ++i) out.push_back(static_cast<char>(ch[i] < 128 ? ch[i] : '?'));
                printf("[+] [6/5] returned string (%d chars): \"%s\"\n", len, out.c_str());
            } else {
                printf("[+] [6/5] returned int: %d\n", *static_cast<int*>(object_unbox(ret)));
            }
        } else {
            printf("[-] [6/5] returned null with no exception\n");
        }
    }

    // [7/5] Managed exception round-trip: Int32.Parse("abc") must throw FormatException,
    // which il2cpp_runtime_invoke should catch and return through the exception out-param.
    const char* exc_test = getenv("LOADER_EXCEPTION_TEST");
    if (exc_test && std::strcmp(exc_test, "1") == 0 && mscorlib) {
        // Culture-free throw tests: exceptions raised by managed IL with no CultureInfo/CF locale use.
        //   A) Math.Abs(Int32.MinValue)      -> OverflowException   (IL `throw new OverflowException(msg)`)
        //   B) Array.Copy(null, null, 0)     -> ArgumentNullException (IL `throw new ArgumentNullException(name)`)
        // Only the exception's class name is read; the message path is never touched.
        printf("\n[7/5] Managed throw tests with no culture lookup (unwinder test)\n");
        auto find_method = [&](void* klass, const char* name, std::initializer_list<const char*> ptypes) -> void* {
            void* it = nullptr;
            while (void* m = class_get_methods(klass, &it)) {
                const char* mn = method_get_name(m);
                if (!mn || std::strcmp(mn, name) != 0 || method_get_param_count(m) != ptypes.size()) continue;
                uint32_t k = 0;
                bool ok = true;
                for (const char* want : ptypes) {
                    char* tn = type_get_name(method_get_param(m, k++));
                    if (!tn || std::strcmp(tn, want) != 0) { ok = false; break; }
                }
                if (ok && (method_get_flags(m, nullptr) & 0x10)) return m;
            }
            return nullptr;
        };
        auto class_get_namespace = reinterpret_cast<const char* (*)(void*)>(get_fn(0xbdef2c));
        auto run_throw_test = [&](const char* label, void* method, void** margs, const char* expect) {
            if (!method) { printf("[-] %s: method not present (stripped)\n", label); return; }
            void* exc = nullptr;
            printf("[*] %s: invoking method=%p, expecting %s. If the process dies after this line, unwinding failed.\n",
                   label, method, expect);
            void* ret = runtime_invoke(method, nullptr, margs, &exc);
            printf("[*] %s: runtime_invoke returned object=%p exception=%p\n", label, ret, exc);
            if (exc) {
                void* ek = object_get_class(exc);
                printf("[+] %s: caught managed exception of class %s.%s\n", label,
                       ek ? class_get_namespace(ek) : "?", ek ? il2cpp_class_get_name(ek) : "?");
            } else {
                printf("[-] %s: NO exception returned (ret=%p)\n", label, ret);
            }
        };

        void* math_cls = class_from_name(mscorlib, "System", "Math");
        void* arr_cls = class_from_name(mscorlib, "System", "Array");
        void* abs_m = math_cls ? find_method(math_cls, "Abs", {"System.Int32"}) : nullptr;
        void* copy_m = arr_cls ? find_method(arr_cls, "Copy", {"System.Array", "System.Array", "System.Int32"}) : nullptr;
        printf("[*] System.Math.Abs(Int32)=%p  System.Array.Copy(Array,Array,Int32)=%p\n", abs_m, copy_m);

        unwinder_probe(reinterpret_cast<uint64_t>(image_base) + 0xc02988, reinterpret_cast<uint64_t>(image_base) + 0xbdb304);

        int int_min = INT32_MIN;
        void* abs_args[1] = {&int_min};
        run_throw_test("7A Math.Abs(int.MinValue)", abs_m, abs_args, "System.OverflowException");
        int zero = 0;
        void* copy_args[3] = {nullptr, nullptr, &zero};
        run_throw_test("7B Array.Copy(null,null,0)", copy_m, copy_args, "System.ArgumentNullException");

        // LOADER_EXCEPTION_BENCH=1: 100 throw+catch round trips through runtime_invoke (7A), timed.
        if (getenv("LOADER_EXCEPTION_BENCH") && abs_m) {
            const int n = 100;
            int got = 0;
            double t0 = guest_fde::now_ms();
            for (int i = 0; i < n; ++i) {
                void* e = nullptr;
                runtime_invoke(abs_m, nullptr, abs_args, &e);
                if (e) ++got;
            }
            double ms = guest_fde::now_ms() - t0;
            printf("[BENCH] %d/%d throws returned an exception object; total %.2f ms, %.3f ms per throw+catch\n", got, n, ms, ms / n);
        }
    }
    if (exc_test && std::strcmp(exc_test, "parse") == 0 && mscorlib) {
        printf("\n[7/5] Invoking System.Int32.Parse(\"abc\") (expected: managed FormatException; needs CF locale)...\n");
        void* int32_cls = class_from_name(mscorlib, "System", "Int32");
        void* parse = nullptr;
        void* it2 = nullptr;
        while (void* m = class_get_methods(int32_cls, &it2)) {
            const char* mn = method_get_name(m);
            if (!mn || std::strcmp(mn, "Parse") != 0 || method_get_param_count(m) != 1) continue;
            char* tn = type_get_name(method_get_param(m, 0));
            printf("    Int32.Parse(%s)\n", tn ? tn : "?");
            if (tn && std::strcmp(tn, "System.String") == 0) parse = m;
        }
        if (!parse) {
            printf("[-] [7/5] Int32.Parse(string) not present (stripped)\n");
        } else {
            void* bad = il2cpp_string_new("abc");
            void* pargs[1] = {bad};
            void* pexc = nullptr;
            printf("[*] calling il2cpp_runtime_invoke(Parse, null, [\"abc\"], &exc); if the process dies now, exception unwinding failed\n");
            void* pret = runtime_invoke(parse, nullptr, pargs, &pexc);
            printf("[*] runtime_invoke returned object=%p exception=%p\n", pret, pexc);
            if (pexc) {
                char msg[2048] = {0};
                format_exception(pexc, msg, sizeof(msg));
                void* ek = object_get_class(pexc);
                printf("[+] [7/5] caught managed exception (%s): %s\n", ek ? il2cpp_class_get_name(ek) : "?", msg);
            } else {
                printf("[-] [7/5] no exception returned (unexpected); ret=%p\n", pret);
            }
        }
    }

    return 0;
}

#ifndef LOADER_LIBRARY
int main(int argc, char** argv) { return loader_main(argc, argv); }
#endif
