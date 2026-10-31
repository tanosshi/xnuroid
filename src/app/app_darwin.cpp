// Darwin libSystem, dyld and libc++ pieces that newer iOS apps reach: thread-local variables, the stack probe, the
// ctype rune table, Mach VM remapping (memory mirrors), POSIX shared memory, availability checks, a few libc++ entry
// points whose Apple mangling differs from bionic's, and framework string constants answered with plain NSStrings.
#include "app.hpp"

#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

extern "C" void app_tlv_get_addr();     // app_msgsend.s
extern "C" void app_chkstk_darwin();    // app_msgsend.s
extern "C" int __cxa_thread_atexit_impl(void (*fn)(void*), void* obj, void* dso);
extern "C" void* __dso_handle;

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

namespace {

// ---- thread-local variables (__thread_vars) ----
// Each descriptor is {thunk, key, offset}; dyld binds the thunk to _tlv_bootstrap. The thread's block is a copy of
// the image's thread-local template (__thread_data, then zeros for __thread_bss) and offset indexes into it.
struct TLVDescriptor { void* thunk; unsigned long key; unsigned long offset; };
uintptr_t g_tlv_start = 0, g_tlv_size = 0, g_tlv_data = 0, g_tlv_data_size = 0;
pthread_key_t g_tlv_key;

int tlv_atexit(void (*fn)(void*), void* obj) { return __cxa_thread_atexit_impl(fn, obj, &__dso_handle); }

// ---- _DefaultRuneLocale ----
// Darwin's <ctype.h> inlines isalpha()/isspace()/... for ASCII as a lookup in this table, so it must be real.
struct DarwinRuneRange { int nranges; void* ranges; };
struct DarwinRuneLocale {
    char magic[8];
    char encoding[32];
    void* sgetrune;
    void* sputrune;
    int32_t invalid_rune;
    uint32_t runetype[256];
    int32_t maplower[256];
    int32_t mapupper[256];
    DarwinRuneRange runetype_ext, maplower_ext, mapupper_ext;
    void* variable;
    int variable_len;
    int ncharclasses;
    void* charclasses;
};
static_assert(offsetof(DarwinRuneLocale, runetype) == 60 && sizeof(DarwinRuneLocale) == 3208, "Darwin _RuneLocale layout");

DarwinRuneLocale make_rune_locale() {
    enum : uint32_t { A = 0x100, C = 0x200, D = 0x400, G = 0x800, L = 0x1000, P = 0x2000, S = 0x4000, U = 0x8000,
                      X = 0x10000, B = 0x20000, R = 0x40000, SW1 = 0x40000000 };
    DarwinRuneLocale r{};
    std::memcpy(r.magic, "RuneMagA", 8);
    std::strcpy(r.encoding, "NONE");
    r.invalid_rune = 0xFFFD;
    for (int c = 0; c < 256; ++c) {
        r.maplower[c] = c;
        r.mapupper[c] = c;
        if (c >= 128) continue;
        uint32_t t = 0;
        if (isalpha(c)) t |= A;
        if (iscntrl(c)) t |= C;
        if (isdigit(c)) t |= D | static_cast<uint32_t>(c - '0');
        if (isgraph(c)) t |= G;
        if (islower(c)) t |= L;
        if (ispunct(c)) t |= P;
        if (isspace(c)) t |= S;
        if (isupper(c)) t |= U;
        if (isxdigit(c)) t |= X | (isdigit(c) ? 0 : static_cast<uint32_t>(10 + (tolower(c) - 'a')));
        if (c == ' ' || c == '\t') t |= B;
        if (isprint(c)) t |= R | SW1;
        r.runetype[c] = t;
        r.maplower[c] = tolower(c);
        r.mapupper[c] = toupper(c);
    }
    return r;
}
DarwinRuneLocale g_rune_locale = make_rune_locale();

// ---- Mach VM ----
constexpr int kVmFlagsAnywhere = 1, kVmFlagsOverwrite = 0x4000;
constexpr int kKernSuccess = 0, kKernInvalidAddress = 1, kKernNoSpace = 3, kKernFailure = 5;

// Reserve [want, want+size) only if nothing is mapped there (Mach's fixed placement fails instead of replacing).
bool reserve_exact(void* want, size_t size) {
    void* r = mmap(want, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (r == want) return true;
    if (r != MAP_FAILED) munmap(r, size);   // old kernel: the flag was only a hint
    return false;
}

// Shared anonymous memory, so vm_remap can alias it later (mremap duplicates only shared mappings).
int vm_allocate_shared(uint32_t, uintptr_t* address, size_t size, int flags) {
    void* want = (flags & kVmFlagsAnywhere) ? nullptr : reinterpret_cast<void*>(*address);
    if (want && !reserve_exact(want, size)) return kKernNoSpace;
    void* p = mmap(want, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS | (want ? MAP_FIXED : 0), -1, 0);
    if (p == MAP_FAILED) return kKernNoSpace;
    *address = reinterpret_cast<uintptr_t>(p);
    return kKernSuccess;
}

// vm_remap(copy = false): a second mapping of the same pages (PPSSPP's memory mirrors).
int vm_remap(uint32_t, uintptr_t* target, size_t size, uintptr_t, int flags, uint32_t, uintptr_t source, int copy,
             int* cur_prot, int* max_prot, unsigned) {
    if (copy) return kKernFailure;
    void* p;
    if ((flags & kVmFlagsAnywhere) || !*target) {
        p = mremap(reinterpret_cast<void*>(source), 0, size, MREMAP_MAYMOVE);
    } else {
        void* want = reinterpret_cast<void*>(*target);
        if (!(flags & kVmFlagsOverwrite) && !reserve_exact(want, size)) return kKernNoSpace;
        p = mremap(reinterpret_cast<void*>(source), 0, size, MREMAP_MAYMOVE | MREMAP_FIXED, want);
        if (p == MAP_FAILED && !(flags & kVmFlagsOverwrite)) munmap(want, size);
    }
    if (p == MAP_FAILED) {
        app::log("vm_remap %#lx -> %#lx (%zu bytes) failed: %s", static_cast<unsigned long>(source),
                 static_cast<unsigned long>(*target), size, strerror(errno));
        return kKernInvalidAddress;
    }
    *target = reinterpret_cast<uintptr_t>(p);
    if (cur_prot) *cur_prot = PROT_READ | PROT_WRITE;
    if (max_prot) *max_prot = PROT_READ | PROT_WRITE | PROT_EXEC;
    return kKernSuccess;
}

int host_page_size(uint32_t, uintptr_t* out) { if (out) *out = static_cast<uintptr_t>(getpagesize()); return kKernSuccess; }
uint32_t mach_host_self() { return 0x103; }
int host_statistics(uint32_t, int, void*, uint32_t*) { return kKernFailure; }
uint64_t mach_continuous_time() {   // same units as the loader's mach_absolute_time (ns, timebase 1/1)
    timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}
size_t os_proc_available_memory() { return static_cast<size_t>(sysconf(_SC_AVPHYS_PAGES)) * static_cast<size_t>(getpagesize()); }
int pthread_threadid_np(pthread_t t, uint64_t* id) {
    if (!id) return EINVAL;
    *id = static_cast<uint64_t>(t ? pthread_gettid_np(t) : gettid());
    return 0;
}
// OSAtomic (deprecated libkern atomics, still common in iOS-7-era code)
int32_t os_atomic_add32(int32_t amount, volatile int32_t* value) { return __atomic_add_fetch(value, amount, __ATOMIC_SEQ_CST); }
int32_t os_atomic_increment32(volatile int32_t* value) { return __atomic_add_fetch(value, 1, __ATOMIC_SEQ_CST); }
int32_t os_atomic_decrement32(volatile int32_t* value) { return __atomic_sub_fetch(value, 1, __ATOMIC_SEQ_CST); }
int64_t os_atomic_add64(int64_t amount, volatile int64_t* value) { return __atomic_add_fetch(value, amount, __ATOMIC_SEQ_CST); }
bool os_atomic_cas32(int32_t expected, int32_t desired, volatile int32_t* value) {
    return __atomic_compare_exchange_n(value, &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
bool os_atomic_cas64(int64_t expected, int64_t desired, volatile int64_t* value) {
    return __atomic_compare_exchange_n(value, &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
bool os_atomic_cas_ptr(void* expected, void* desired, void* volatile* value) {
    return __atomic_compare_exchange_n(value, &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
void os_memory_barrier() { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

// mach_wait_until: deadline in mach_absolute_time units (ns here, timebase 1/1)
int mach_wait_until(uint64_t deadline) {
    timespec ts{static_cast<time_t>(deadline / 1000000000ULL), static_cast<long>(deadline % 1000000000ULL)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {}
    return kKernSuccess;
}
void srandomdev() { srandom(static_cast<unsigned>(time(nullptr) ^ (getpid() << 16) ^ mach_continuous_time())); }
int vm_protect(uint32_t, uintptr_t address, size_t size, int, int prot) {
    return mprotect(reinterpret_cast<void*>(address & ~static_cast<uintptr_t>(getpagesize() - 1)), size + (address & (getpagesize() - 1)), prot & 7) == 0 ? kKernSuccess : 2;   // KERN_PROTECTION_FAILURE
}
int thread_policy_set(uint32_t, int, void*, uint32_t) { return kKernSuccess; }
int thread_info(uint32_t, int, void*, uint32_t*) { return kKernFailure; }
int32_t ns_version_of_link_time_library(const char*) { return -1; }   // "not linked": apps then skip version-specific workarounds

// ---- signals: Darwin numbers some signals differently, and its struct sigaction is {handler, u32 mask, int flags}
// with its own flag bits; bionic's is 32 bytes. Crash signals stay with the loader's handler (it reports where the app
// crashed); an app's own crash reporter would only swallow that. Other handlers get Darwin signal numbers and info.
int linux_signal(int d) {
    switch (d) {
        case 10: return SIGBUS;   case 12: return SIGSYS;   case 16: return SIGURG;   case 17: return SIGSTOP;
        case 18: return SIGTSTP;  case 19: return SIGCONT;  case 20: return SIGCHLD;  case 23: return SIGIO;
        case 30: return SIGUSR1;  case 31: return SIGUSR2;  case 7: case 29: return 0;   // SIGEMT, SIGINFO: none
        default: return d >= 1 && d <= 28 ? d : 0;
    }
}
int darwin_signal_of(int l) {
    for (int d = 1; d <= 31; ++d) if (linux_signal(d) == l) return d;
    return l;
}
bool loader_owned(int l) { return l == SIGSEGV || l == SIGBUS || l == SIGILL || l == SIGFPE || l == SIGABRT || l == SIGTRAP || l == SIGUSR2; }

struct DarwinSigaction { void* handler; uint32_t mask; int flags; };
constexpr int kDSaOnstack = 0x1, kDSaRestart = 0x2, kDSaResethand = 0x4, kDSaNocldstop = 0x8, kDSaNodefer = 0x10,
              kDSaNocldwait = 0x20, kDSaSiginfo = 0x40;
void* g_guest_handlers[65];
int g_guest_flags[65];

struct DarwinSiginfo { int signo, err, code, pid; uint32_t uid; int status; void* addr; uint64_t value; long band; unsigned long pad[7]; };
void guest_signal_trampoline(int sig, siginfo_t* info, void*) {
    void* h = g_guest_handlers[sig];
    int dsig = darwin_signal_of(sig);
    if (!h) return;
    if (g_guest_flags[sig] & kDSaSiginfo) {
        DarwinSiginfo di{};
        di.signo = dsig;
        if (info) { di.err = info->si_errno; di.code = info->si_code; di.pid = info->si_pid; di.uid = info->si_uid; di.addr = info->si_addr; }
        alignas(16) static thread_local uint8_t ucontext[1024];   // Darwin ucontext layout is not provided
        reinterpret_cast<void (*)(int, DarwinSiginfo*, void*)>(h)(dsig, &di, ucontext);
    } else {
        reinterpret_cast<void (*)(int)>(h)(dsig);
    }
}

int darwin_sigaction(int dsig, const DarwinSigaction* act, DarwinSigaction* old) {
    int sig = linux_signal(dsig);
    if (!sig) { errno = EINVAL; return -1; }
    if (old) {
        *old = {};
        if (g_guest_handlers[sig]) { old->handler = g_guest_handlers[sig]; old->flags = g_guest_flags[sig]; }
    }
    if (!act) return 0;
    if (loader_owned(sig)) {   // remembered for the old-action report, never installed
        g_guest_handlers[sig] = act->handler == SIG_DFL || act->handler == SIG_IGN ? nullptr : act->handler;
        g_guest_flags[sig] = act->flags;
        app::log("signal %d: the app's handler is not installed; the loader's crash report stays", dsig);
        return 0;
    }
    struct sigaction sa{};
    int f = act->flags;
    sa.sa_flags = (f & kDSaOnstack ? SA_ONSTACK : 0) | (f & kDSaRestart ? SA_RESTART : 0) | (f & kDSaResethand ? SA_RESETHAND : 0) |
                  (f & kDSaNocldstop ? SA_NOCLDSTOP : 0) | (f & kDSaNodefer ? SA_NODEFER : 0) | (f & kDSaNocldwait ? SA_NOCLDWAIT : 0);
    sigemptyset(&sa.sa_mask);
    for (int d = 1; d <= 31; ++d) if ((act->mask >> (d - 1)) & 1) if (int l = linux_signal(d)) sigaddset(&sa.sa_mask, l);
    if (act->handler == SIG_DFL || act->handler == SIG_IGN) {
        sa.sa_handler = reinterpret_cast<void (*)(int)>(act->handler);
        g_guest_handlers[sig] = nullptr;
    } else {
        g_guest_handlers[sig] = act->handler;
        g_guest_flags[sig] = f;
        sa.sa_flags |= SA_SIGINFO;
        sa.sa_sigaction = guest_signal_trampoline;
    }
    return sigaction(sig, &sa, nullptr);
}
void* darwin_signal(int dsig, void* handler) {
    DarwinSigaction act{handler, 0, kDSaRestart}, old{};
    return darwin_sigaction(dsig, &act, &old) == 0 ? old.handler : reinterpret_cast<void*>(SIG_ERR);
}

int sys_cache_control(int function, void* start, size_t len) {
    if (function == 1) __builtin___clear_cache(static_cast<char*>(start), static_cast<char*>(start) + len);   // prepare for execution
    return 0;
}

// ---- POSIX shared memory: a file under the sandbox tmp dir (Darwin open flags differ from Linux) ----
std::string shm_path(const char* name) {
    std::string n = name ? name : "";
    for (char& ch : n) if (ch == '/') ch = '_';
    return app::sandbox_dir("tmp") + "/shm" + n;
}
}  // namespace

extern "C" int app_shm_open_impl(const char* name, int oflag, const uint64_t* va) {
    int f = oflag & 3;
    if (oflag & 0x200) f |= O_CREAT;
    if (oflag & 0x400) f |= O_TRUNC;
    if (oflag & 0x800) f |= O_EXCL;
    mode_t mode = (oflag & 0x200) && va ? static_cast<mode_t>(va[0]) : 0600;
    return open(shm_path(name).c_str(), f | O_CLOEXEC, mode);
}
APP_VARIADIC(app_th_shm_open, app_shm_open_impl, 2)

namespace {
int shm_unlink(const char* name) { return unlink(shm_path(name).c_str()); }

// ---- select: Darwin's struct timeval has a 32-bit tv_usec (the upper half is padding, not part of the value) ----
int darwin_select(int n, fd_set* r, fd_set* w, fd_set* e, void* tv) {
    timeval t{}, *pt = nullptr;
    if (tv) {
        std::memcpy(&t.tv_sec, tv, 8);
        int32_t us;
        std::memcpy(&us, static_cast<char*>(tv) + 8, 4);
        t.tv_usec = us;
        pt = &t;
    }
    int rc = select(n, r, w, e, pt);
    if (tv) {
        std::memcpy(tv, &t.tv_sec, 8);
        int32_t us = static_cast<int32_t>(t.tv_usec);
        std::memcpy(static_cast<char*>(tv) + 8, &us, 4);
    }
    return rc;
}
int darwin_check_fd_set_overflow(int fd, const void*, int) { return fd >= 0 && fd < FD_SETSIZE; }

// ---- statvfs: Darwin's struct is 64 bytes with 32-bit block/file counts (bionic writes 112, past a guest buffer) ----
struct DarwinStatvfs {
    unsigned long f_bsize, f_frsize;
    uint32_t f_blocks, f_bfree, f_bavail, f_files, f_ffree, f_favail;
    unsigned long f_fsid, f_flag, f_namemax;
};
static_assert(sizeof(DarwinStatvfs) == 64, "Darwin statvfs layout");
void to_darwin(const struct statvfs& s, DarwinStatvfs* d) {
    // counts that do not fit in 32 bits are scaled with a larger fragment size, as Darwin does
    unsigned long frsize = s.f_frsize ? s.f_frsize : s.f_bsize;
    uint64_t blocks = s.f_blocks, bfree = s.f_bfree, bavail = s.f_bavail;
    while (blocks > UINT32_MAX) { blocks >>= 1; bfree >>= 1; bavail >>= 1; frsize <<= 1; }
    *d = {};
    d->f_bsize = s.f_bsize;
    d->f_frsize = frsize;
    d->f_blocks = static_cast<uint32_t>(blocks);
    d->f_bfree = static_cast<uint32_t>(bfree);
    d->f_bavail = static_cast<uint32_t>(bavail);
    d->f_files = static_cast<uint32_t>(std::min<uint64_t>(s.f_files, UINT32_MAX));
    d->f_ffree = static_cast<uint32_t>(std::min<uint64_t>(s.f_ffree, UINT32_MAX));
    d->f_favail = static_cast<uint32_t>(std::min<uint64_t>(s.f_favail, UINT32_MAX));
    d->f_fsid = s.f_fsid;
    d->f_flag = s.f_flag;
    d->f_namemax = s.f_namemax;
}
int darwin_statvfs(const char* path, DarwinStatvfs* out) {
    struct statvfs s;
    int rc = statvfs(app::remap_path(path ? path : "").c_str(), &s);
    if (rc == 0 && out) to_darwin(s, out);
    return rc;
}
int darwin_fstatvfs(int fd, DarwinStatvfs* out) {
    struct statvfs s;
    int rc = fstatvfs(fd, &s);
    if (rc == 0 && out) to_darwin(s, out);
    return rc;
}

// ---- clock_gettime: Darwin's clock ids (MONOTONIC is 6, which Linux reads as REALTIME_COARSE) ----
clockid_t linux_clock(int darwin_id) {
    switch (darwin_id) {
        case 0: return CLOCK_REALTIME;
        case 4: case 5: return CLOCK_MONOTONIC_RAW;   // MONOTONIC_RAW(_APPROX)
        case 6: return CLOCK_BOOTTIME;                 // MONOTONIC (counts across sleep on Darwin)
        case 8: case 9: return CLOCK_MONOTONIC;        // UPTIME_RAW(_APPROX): stops in sleep
        case 12: return CLOCK_PROCESS_CPUTIME_ID;
        case 16: return CLOCK_THREAD_CPUTIME_ID;
        default: return CLOCK_MONOTONIC;
    }
}
int darwin_clock_gettime(int id, timespec* ts) { return clock_gettime(linux_clock(id), ts); }
int darwin_clock_getres(int id, timespec* ts) { return clock_getres(linux_clock(id), ts); }
uint64_t darwin_clock_gettime_nsec_np(int id) {
    timespec ts{};
    clock_gettime(linux_clock(id), &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

// @available(iOS N, *): entries are {platform, version as 0xMMMMmmpp}; platform 2 is iOS.
struct BuildVersion { uint32_t platform, version; };
bool availability_version_check(uint32_t count, const BuildVersion* v) {
    for (uint32_t i = 0; i < count; ++i)
        if (v[i].platform == 2 && v[i].version > app::ios_version_packed()) return false;
    return true;
}

void* dispatch_queue_attr_make_with_qos_class(void* attr, int, int) { return attr; }
id cf_dictionary_create_mutable_copy(void*, long, id dict) {
    return dict ? app::send(dict, "mutableCopy") : app::send(app::alloc(app::class_named("NSMutableDictionary")), "init");
}

// ---- libc++ entry points that use Darwin's 128-byte __mbstate_t (bionic mangles mbstate_t, 8 bytes) ----
struct DarwinFpos { uint8_t state[128]; int64_t off; };
DarwinFpos streambuf_seekpos(void*, const DarwinFpos*, unsigned) {   // the base class answer: "cannot seek"
    DarwinFpos r{};
    r.off = -1;
    return r;
}
void* libcxx_sym(const char* name) {
    static void* h = [] {
        const char* p = getenv("LOADER_LIBCXX");
        void* lib = p ? dlopen(p, RTLD_NOW | RTLD_NOLOAD) : nullptr;
        return lib ? lib : RTLD_DEFAULT;
    }();
    void* s = dlsym(h, name);
    return s ? s : dlsym(RTLD_DEFAULT, name);
}
void* istream_seekg_pos(void* self, const DarwinFpos* pos) {   // seekg(pos) -> seekg(off, ios_base::beg)
    using Fn = void* (*)(void*, long long, int);
    static Fn fn = reinterpret_cast<Fn>(libcxx_sym("_ZNSt3__113basic_istreamIcNS_11char_traitsIcEEE5seekgExNS_8ios_base7seekdirE"));
    return fn ? fn(self, pos ? pos->off : 0, 0) : self;
}

id* str_const(const char* v) { id* slot = new id; *slot = app::str_new(v); return slot; }

// link_ntoa: a Darwin sockaddr_dl {len, family, index(2), type, nlen, alen, slen, data[]} as "name:a.b.c.d.e.f"
char* darwin_link_ntoa(const uint8_t* sdl) {
    static thread_local char buf[64];
    buf[0] = 0;
    if (!sdl) return buf;
    size_t nlen = sdl[5], alen = sdl[6], o = 0;
    const uint8_t* data = sdl + 8;
    for (size_t i = 0; i < nlen && o < 30; ++i) buf[o++] = static_cast<char>(data[i]);
    if (alen) buf[o++] = ':';
    for (size_t i = 0; i < alen && o + 4 < sizeof buf; ++i)
        o += static_cast<size_t>(snprintf(buf + o, sizeof buf - o, i ? ".%x" : "%x", data[nlen + i]));
    buf[o] = 0;
    return buf;
}
}  // namespace

extern "C" void* app_tlv_resolve(TLVDescriptor* d) {
    static __thread uint8_t* block = nullptr;
    if (__builtin_expect(!block, 0)) {
        block = static_cast<uint8_t*>(calloc(1, g_tlv_size ? g_tlv_size : 1));
        if (g_tlv_data_size) std::memcpy(block + (g_tlv_data - g_tlv_start), reinterpret_cast<void*>(g_tlv_data), g_tlv_data_size);
        pthread_setspecific(g_tlv_key, block);   // frees it when the thread exits, after its thread_local destructors
    }
    return block + d->offset;
}

namespace app {

uint32_t ios_version_packed() {
    static const uint32_t v = [] {
        unsigned major = 0, minor = 0, patch = 0;
        sscanf(ios_version().c_str(), "%u.%u.%u", &major, &minor, &patch);
        return major << 16 | minor << 8 | patch;
    }();
    return v;
}

// The iOS version reported to the app: LOADER_APP_IOS_VERSION, else 8.4 or the bundle's MinimumOSVersion if newer
// (so an app that requires iOS 15 sees an OS it supports).
const std::string& ios_version() {
    static const std::string v = [] {
        if (const char* e = getenv("LOADER_APP_IOS_VERSION")) return std::string(e);
        std::string min = utf8(dict_get(info_dictionary(), "MinimumOSVersion"));
        unsigned a = 0, b = 0;
        return sscanf(min.c_str(), "%u.%u", &a, &b) >= 1 && (a > 8 || (a == 8 && b > 4)) ? min : std::string("8.4");
    }();
    return v;
}

void tlv_setup(const ImageInfo& im) {
    g_tlv_start = im.tlv_start;
    g_tlv_size = im.tlv_size;
    g_tlv_data = im.tlv_data;
    g_tlv_data_size = im.tlv_data_size;
    pthread_key_create(&g_tlv_key, free);
    if (g_tlv_size) log("thread-local template: %zu bytes (%zu initialized)", static_cast<size_t>(g_tlv_size), static_cast<size_t>(g_tlv_data_size));
}

void register_darwin_symbols(std::unordered_map<std::string, void*>& m) {
    auto r = [&](const char* n, auto f) { m[n] = reinterpret_cast<void*>(f); };
    // dyld / compiler runtime
    r("__tlv_bootstrap", &app_tlv_get_addr);
    r("__tlv_atexit", &tlv_atexit);
    r("____chkstk_darwin", &app_chkstk_darwin);
    r("__availability_version_check", &availability_version_check);
    m["__DefaultRuneLocale"] = &g_rune_locale;
    // libSystem / Mach
    r("_vm_allocate", &vm_allocate_shared);
    r("_vm_remap", &vm_remap);
    r("_host_page_size", &host_page_size);
    r("_mach_host_self", &mach_host_self);
    r("_host_statistics", &host_statistics);
    r("_mach_continuous_time", &mach_continuous_time);
    r("_os_proc_available_memory", &os_proc_available_memory);
    for (const char* n : {"_OSAtomicAdd32", "_OSAtomicAdd32Barrier"}) r(n, &os_atomic_add32);
    for (const char* n : {"_OSAtomicIncrement32", "_OSAtomicIncrement32Barrier"}) r(n, &os_atomic_increment32);
    for (const char* n : {"_OSAtomicDecrement32", "_OSAtomicDecrement32Barrier"}) r(n, &os_atomic_decrement32);
    for (const char* n : {"_OSAtomicAdd64", "_OSAtomicAdd64Barrier"}) r(n, &os_atomic_add64);
    for (const char* n : {"_OSAtomicCompareAndSwap32", "_OSAtomicCompareAndSwap32Barrier", "_OSAtomicCompareAndSwapInt",
                          "_OSAtomicCompareAndSwapIntBarrier"}) r(n, &os_atomic_cas32);
    for (const char* n : {"_OSAtomicCompareAndSwap64", "_OSAtomicCompareAndSwap64Barrier", "_OSAtomicCompareAndSwapLong",
                          "_OSAtomicCompareAndSwapLongBarrier"}) r(n, &os_atomic_cas64);
    for (const char* n : {"_OSAtomicCompareAndSwapPtr", "_OSAtomicCompareAndSwapPtrBarrier"}) r(n, &os_atomic_cas_ptr);
    r("_OSMemoryBarrier", &os_memory_barrier);
    r("_mach_wait_until", &mach_wait_until);
    r("_srandomdev", &srandomdev);
    r("_vm_protect", &vm_protect);
    r("_thread_policy_set", &thread_policy_set);
    r("_thread_info", &thread_info);
    r("_NSVersionOfLinkTimeLibrary", &ns_version_of_link_time_library);
    r("_sigaction", &darwin_sigaction);
    r("_signal", &darwin_signal);
    r("_pthread_threadid_np", &pthread_threadid_np);
    r("_sys_cache_control", &sys_cache_control);
    r("_shm_open", &app_th_shm_open);
    r("_shm_unlink", &shm_unlink);
    r("_select", &darwin_select);
    r("_select$DARWIN_EXTSN", &darwin_select);
    r("___darwin_check_fd_set_overflow", &darwin_check_fd_set_overflow);
    r("_statvfs", &darwin_statvfs);
    r("_fstatvfs", &darwin_fstatvfs);
    r("_clock_gettime", &darwin_clock_gettime);
    r("_clock_getres", &darwin_clock_getres);
    r("_clock_gettime_nsec_np", &darwin_clock_gettime_nsec_np);
    r("_dispatch_queue_attr_make_with_qos_class", &dispatch_queue_attr_make_with_qos_class);
    r("_CFDictionaryCreateMutableCopy", &cf_dictionary_create_mutable_copy);
    // libc++ with Darwin's __mbstate_t
    r("__ZNSt3__115basic_streambufIcNS_11char_traitsIcEEE7seekposENS_4fposI11__mbstate_tEEj", &streambuf_seekpos);
    r("__ZNSt3__113basic_istreamIcNS_11char_traitsIcEEE5seekgENS_4fposI11__mbstate_tEE", &istream_seekg_pos);
    if (void* sym = libcxx_sym("_ZNSt3__17codecvtIcc9mbstate_tE2idE")) m["__ZNSt3__17codecvtIcc11__mbstate_tE2idE"] = sym;
    // framework constants: their values are the names (as on iOS for most of them)
    for (const char* n : {"kCGColorSpaceAdobeRGB1998", "kCGColorSpaceDCIP3", "kCGColorSpaceDisplayP3", "kCGColorSpaceExtendedLinearDisplayP3",
                          "kCGColorSpaceExtendedLinearITUR_2020", "kCGColorSpaceExtendedLinearSRGB", "kCGColorSpaceExtendedSRGB",
                          "kCGColorSpaceITUR_2100_HLG", "kCGColorSpaceITUR_2100_PQ", "kCGColorSpaceITUR_709", "kCGColorSpaceSRGB",
                          "kCGColorSpaceGenericRGB", "kCGColorSpaceLinearSRGB",
                          "kCAGravityCenter", "kCAGravityTop", "kCAGravityBottom", "kCAGravityLeft", "kCAGravityRight", "kCAGravityTopLeft",
                          "kCAGravityTopRight", "kCAGravityBottomLeft", "kCAGravityBottomRight", "kCAGravityResize",
                          "kCAGravityResizeAspect", "kCAGravityResizeAspectFill",
                          "AVAudioSessionCategoryAmbient", "AVAudioSessionCategorySoloAmbient", "AVAudioSessionCategoryPlayback",
                          "AVAudioSessionCategoryRecord", "AVAudioSessionCategoryPlayAndRecord", "AVAudioSessionCategoryAudioProcessing",
                          "AVAudioSessionModeDefault", "AVAudioSessionInterruptionNotification", "AVAudioSessionInterruptionTypeKey",
                          "AVAudioSessionRouteChangeNotification", "AVAudioSessionMediaServicesWereResetNotification",
                          "AVCaptureSessionPresetMedium", "AVLayerVideoGravityResizeAspectFill", "AVMediaTypeVideo", "AVMediaTypeAudio",
                          "kCTFontAttributeName", "kCTForegroundColorAttributeName", "kCTForegroundColorFromContextAttributeName",
                          "kCVPixelBufferPixelFormatTypeKey", "kUTTypeFolder", "kUTTypeItem", "kUTTypeData", "kUTTypeContent",
                          "MTLCommandBufferEncoderInfoErrorKey", "MTLCommonCounterSetTimestamp", "MTLCommonCounterTimestamp",
                          "kIOSurfaceBytesPerElement", "kIOSurfaceBytesPerRow", "kIOSurfaceElementHeight", "kIOSurfaceElementWidth",
                          "kIOSurfaceHeight", "kIOSurfaceWidth", "kIOSurfaceIsGlobal", "kIOSurfacePixelFormat", "kIOSurfacePlaneInfo",
                          "kIOSurfacePlaneBytesPerElement", "kIOSurfacePlaneElementHeight", "kIOSurfacePlaneElementWidth",
                          "kIOSurfacePlaneHeight", "kIOSurfacePlaneWidth",
                          "GCControllerDidConnectNotification", "GCControllerDidDisconnectNotification"})
        if (!m.count(std::string("_") + n)) m[std::string("_") + n] = str_const(n);
    // uniform type identifiers have real values apps compare against
    m["_kUTTypeFolder"] = str_const("public.folder");
    m["_kUTTypeItem"] = str_const("public.item");
    m["_kUTTypeData"] = str_const("public.data");
    m["_kUTTypeContent"] = str_const("public.content");
    // Security item classes and attributes have short values of their own
    m["_kSecClassKey"] = str_const("keys");
    m["_kSecAttrApplicationTag"] = str_const("atag");
    m["_kSecAttrKeyType"] = str_const("type");
    m["_kSecAttrKeyTypeRSA"] = str_const("42");
    m["_kSecReturnPersistentRef"] = str_const("r_PersistentRef");
    m["_kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly"] = str_const("cku");
    // data that is not an NSString
    struct CMTime { int64_t value; int32_t timescale; uint32_t flags; int64_t epoch; };
    static const CMTime cm_zero{0, 1, 1, 0};
    m["_kCMTimeZero"] = const_cast<CMTime*>(&cm_zero);
    static FILE* stdinp = stdin;
    m["___stdinp"] = &stdinp;
    static const uint8_t ndr[8] = {0, 0, 0, 0, 1, 0, 0, 0};   // NDR_record: little-endian int, ASCII, IEEE float
    m["_NDR_record"] = const_cast<uint8_t*>(ndr);
    static void* system_allocator = nullptr;                  // NULL allocator means the default one
    m["_kCFAllocatorSystemDefault"] = &system_allocator;
    register_crypto_symbols(m);
    m["_link_ntoa"] = reinterpret_cast<void*>(&darwin_link_ntoa);
}

// Framework data a game imports that nothing above provides. Nearly all of it is NSString constants (notification
// names, dictionary keys, exception names), whose value is their own name, so one is made on demand; a fault page
// would only defer the crash to the first use.
void* data_constant(const std::string& name) {
    if (name.size() < 4 || name[0] != '_' || name.rfind("_OBJC_", 0) == 0 || name.rfind("__", 0) == 0) return nullptr;
    for (size_t i = 1; i < name.size(); ++i)
        if (!std::isalnum(static_cast<unsigned char>(name[i])) && name[i] != '_') return nullptr;
    if (name.find("CallBacks") != std::string::npos) {   // CFArray/CFDictionary callback structs: zeroed
        static uint64_t callbacks[8][8];
        static int used;
        return used < 8 ? callbacks[used++] : nullptr;
    }
    log("framework constant %s made a string", name.c_str());
    return str_const(name.c_str() + 1);
}

}  // namespace app
