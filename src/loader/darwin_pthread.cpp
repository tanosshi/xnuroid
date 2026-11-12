// Darwin pthread semantics on bionic: attribute constants, statically initialized locks, Mach semaphores.
//
// Darwin's PTHREAD_MUTEX_INITIALIZER / PTHREAD_COND_INITIALIZER / PTHREAD_RWLOCK_INITIALIZER put a signature in the
// first word and zeros after it. bionic reads that word as lock state: a "locked" mutex whose futex wait returns at
// once, so the waiter spins on a core forever. Every lock entry point therefore turns a Darwin static initializer into
// a real bionic object first (the Darwin storage is larger than bionic's, so it fits in place).
#include <pthread.h>
#include <semaphore.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr uint64_t kMutexSig = 0x32AAABA7, kMutexErrorcheckSig = 0x32AAABA1, kMutexRecursiveSig = 0x32AAABA2,
                   kMutexFirstfitSig = 0x32AAABA3, kCondSig = 0x3CB0B1BB, kRwlockSig = 0x2DA8B3B4;

std::mutex g_init_mu;

uint64_t first_word(const void* p) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}

void fix_mutex(pthread_mutex_t* m) {
    uint64_t sig = first_word(m);
    if (sig != kMutexSig && sig != kMutexErrorcheckSig && sig != kMutexRecursiveSig && sig != kMutexFirstfitSig) return;
    std::lock_guard<std::mutex> l(g_init_mu);
    sig = first_word(m);
    if (sig != kMutexSig && sig != kMutexErrorcheckSig && sig != kMutexRecursiveSig && sig != kMutexFirstfitSig) return;
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, sig == kMutexRecursiveSig ? PTHREAD_MUTEX_RECURSIVE
                                  : sig == kMutexErrorcheckSig ? PTHREAD_MUTEX_ERRORCHECK : PTHREAD_MUTEX_NORMAL);
    pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
}
void fix_cond(pthread_cond_t* c) {
    if (first_word(c) != kCondSig) return;
    std::lock_guard<std::mutex> l(g_init_mu);
    if (first_word(c) == kCondSig) pthread_cond_init(c, nullptr);
}
void fix_rwlock(pthread_rwlock_t* r) {
    if (first_word(r) != kRwlockSig) return;
    std::lock_guard<std::mutex> l(g_init_mu);
    if (first_word(r) == kRwlockSig) pthread_rwlock_init(r, nullptr);
}

// ---- locks ----
int mutex_lock(pthread_mutex_t* m) { fix_mutex(m); return pthread_mutex_lock(m); }
int mutex_trylock(pthread_mutex_t* m) { fix_mutex(m); return pthread_mutex_trylock(m); }
int mutex_unlock(pthread_mutex_t* m) { fix_mutex(m); return pthread_mutex_unlock(m); }
int mutex_destroy(pthread_mutex_t* m) { fix_mutex(m); return pthread_mutex_destroy(m); }
int cond_wait(pthread_cond_t* c, pthread_mutex_t* m) { fix_cond(c); fix_mutex(m); return pthread_cond_wait(c, m); }
int cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m, const timespec* t) {
    fix_cond(c);
    fix_mutex(m);
    int rc = pthread_cond_timedwait(c, m, t);
    return rc == ETIMEDOUT ? 60 : rc;   // Darwin ETIMEDOUT
}
int cond_timedwait_relative(pthread_cond_t* c, pthread_mutex_t* m, const timespec* rel) {
    timespec abs;
    clock_gettime(CLOCK_REALTIME, &abs);
    if (rel) {
        abs.tv_sec += rel->tv_sec;
        abs.tv_nsec += rel->tv_nsec;
        if (abs.tv_nsec >= 1000000000L) { abs.tv_sec += 1; abs.tv_nsec -= 1000000000L; }
    }
    return cond_timedwait(c, m, &abs);
}
int cond_signal(pthread_cond_t* c) { fix_cond(c); return pthread_cond_signal(c); }
int cond_broadcast(pthread_cond_t* c) { fix_cond(c); return pthread_cond_broadcast(c); }
int cond_destroy(pthread_cond_t* c) { fix_cond(c); return pthread_cond_destroy(c); }
int rwlock_rdlock(pthread_rwlock_t* r) { fix_rwlock(r); return pthread_rwlock_rdlock(r); }
int rwlock_wrlock(pthread_rwlock_t* r) { fix_rwlock(r); return pthread_rwlock_wrlock(r); }
int rwlock_tryrdlock(pthread_rwlock_t* r) { fix_rwlock(r); return pthread_rwlock_tryrdlock(r); }
int rwlock_trywrlock(pthread_rwlock_t* r) { fix_rwlock(r); return pthread_rwlock_trywrlock(r); }
int rwlock_unlock(pthread_rwlock_t* r) { fix_rwlock(r); return pthread_rwlock_unlock(r); }
int rwlock_destroy(pthread_rwlock_t* r) { fix_rwlock(r); return pthread_rwlock_destroy(r); }

// Darwin's pthread_once_t is {signature, 8 opaque bytes} and PTHREAD_ONCE_INIT zeroes the opaque part; bionic reads
// the signature as "initialization underway" and waits forever. Keep bionic's once state in the opaque bytes.
int once(void* o, void (*fn)()) { return pthread_once(reinterpret_cast<pthread_once_t*>(static_cast<char*>(o) + 8), fn); }

// ---- libc++ std::mutex / std::condition_variable ----
// Their constexpr constructors leave the same Darwin signatures in place, and the system libc++ would hand the object
// straight to bionic, so the out-of-line members go through the fixups above. std::mutex is a bare pthread_mutex_t,
// std::condition_variable a bare pthread_cond_t, and unique_lock<mutex> starts with its mutex pointer.
void cxx_mutex_lock(pthread_mutex_t* m) { mutex_lock(m); }
bool cxx_mutex_try_lock(pthread_mutex_t* m) { return mutex_trylock(m) == 0; }
void cxx_mutex_unlock(pthread_mutex_t* m) { mutex_unlock(m); }
void cxx_mutex_destroy(pthread_mutex_t* m) { mutex_destroy(m); }
void cxx_cv_notify_one(pthread_cond_t* c) { cond_signal(c); }
void cxx_cv_notify_all(pthread_cond_t* c) { cond_broadcast(c); }
void cxx_cv_wait(pthread_cond_t* c, pthread_mutex_t** lock) { cond_wait(c, *lock); }
// time_point<system_clock, nanoseconds> is passed by value as its tick count
void cxx_cv_timed_wait(pthread_cond_t* c, pthread_mutex_t** lock, int64_t abs_ns) {
    timespec ts{};
    if (abs_ns > 0) { ts.tv_sec = static_cast<time_t>(abs_ns / 1000000000); ts.tv_nsec = static_cast<long>(abs_ns % 1000000000); }
    cond_timedwait(c, *lock, &ts);
}
void cxx_cv_destroy(pthread_cond_t* c) { cond_destroy(c); }

// Mach semaphores (Unity's PlatformSemaphore on Apple: job system and render-thread command queue). semaphore_t is a
// 32-bit port name; it indexes a table of POSIX semaphores. Destroyed slots are kept so a late signal stays harmless.
static std::mutex g_mach_sem_mu;
static std::vector<sem_t*> g_mach_sems;
static sem_t* mach_sem(uint32_t name) {
    std::lock_guard<std::mutex> l(g_mach_sem_mu);
    uint32_t i = name - 0x5000;
    return name >= 0x5000 && i < g_mach_sems.size() ? g_mach_sems[i] : nullptr;
}
int native_semaphore_create(uint32_t, uint32_t* sem, int, int value) {
    auto* s = new sem_t;
    sem_init(s, 0, static_cast<unsigned>(value < 0 ? 0 : value));
    std::lock_guard<std::mutex> l(g_mach_sem_mu);
    g_mach_sems.push_back(s);
    if (sem) *sem = 0x5000 + static_cast<uint32_t>(g_mach_sems.size() - 1);
    return 0;
}
int native_semaphore_destroy(uint32_t, uint32_t sem) { return mach_sem(sem) ? 0 : 15; }   // KERN_INVALID_NAME
int native_semaphore_signal(uint32_t sem) {
    sem_t* s = mach_sem(sem);
    if (!s) return 15;
    sem_post(s);
    return 0;
}
int native_semaphore_wait(uint32_t sem) {
    sem_t* s = mach_sem(sem);
    if (!s) return 15;
    while (sem_wait(s) != 0 && errno == EINTR) {}
    return 0;
}

// ---- attributes ----
// Darwin mutex types: NORMAL 0, ERRORCHECK 1, RECURSIVE 2 (bionic NORMAL 0, RECURSIVE 1, ERRORCHECK 2).
int mutexattr_settype(pthread_mutexattr_t* a, int t) {
    return pthread_mutexattr_settype(a, t == 1 ? PTHREAD_MUTEX_ERRORCHECK : t == 2 ? PTHREAD_MUTEX_RECURSIVE : PTHREAD_MUTEX_NORMAL);
}
int mutexattr_setprotocol(pthread_mutexattr_t*, int) { return 0; }
// Darwin PTHREAD_CREATE_JOINABLE 1 / DETACHED 2 (bionic 0 / 1).
int attr_setdetachstate(pthread_attr_t* a, int s) {
    return pthread_attr_setdetachstate(a, s == 2 ? PTHREAD_CREATE_DETACHED : PTHREAD_CREATE_JOINABLE);
}
// Darwin SCHED_OTHER 1 / FIFO 4 / RR 2. Real-time policies are refused for unprivileged Android processes (and the
// pthread_create wrapper drops them), so the request is accepted and recorded as SCHED_OTHER.
int attr_setschedpolicy(pthread_attr_t* a, int p) {
    if (p != 1 && p != 2 && p != 4) return EINVAL;
    pthread_attr_setschedpolicy(a, SCHED_OTHER);
    return 0;
}
int attr_setschedparam(pthread_attr_t*, const void*) { return 0; }

}  // namespace

void register_darwin_pthread_shims(std::unordered_map<std::string, void*>& shims) {
    auto reg = [&](const char* name, auto* fn) { shims[name] = reinterpret_cast<void*>(fn); };
    reg("_pthread_mutex_lock", &mutex_lock);
    reg("_pthread_mutex_trylock", &mutex_trylock);
    reg("_pthread_mutex_unlock", &mutex_unlock);
    reg("_pthread_mutex_destroy", &mutex_destroy);
    reg("_pthread_cond_wait", &cond_wait);
    reg("_pthread_cond_timedwait", &cond_timedwait);
    reg("_pthread_cond_timedwait_relative_np", &cond_timedwait_relative);
    reg("_pthread_cond_signal", &cond_signal);
    reg("_pthread_cond_broadcast", &cond_broadcast);
    reg("_pthread_cond_destroy", &cond_destroy);
    reg("_pthread_rwlock_rdlock", &rwlock_rdlock);
    reg("_pthread_rwlock_wrlock", &rwlock_wrlock);
    reg("_pthread_rwlock_tryrdlock", &rwlock_tryrdlock);
    reg("_pthread_rwlock_trywrlock", &rwlock_trywrlock);
    reg("_pthread_rwlock_unlock", &rwlock_unlock);
    reg("_pthread_rwlock_destroy", &rwlock_destroy);
    reg("_pthread_once", &once);
    reg("_pthread_mutexattr_settype", &mutexattr_settype);
    reg("_pthread_mutexattr_setprotocol", &mutexattr_setprotocol);
    reg("_pthread_attr_setdetachstate", &attr_setdetachstate);
    reg("_pthread_attr_setschedpolicy", &attr_setschedpolicy);
    reg("_pthread_attr_setschedparam", &attr_setschedparam);
    reg("_semaphore_create", &native_semaphore_create);
    reg("_semaphore_destroy", &native_semaphore_destroy);
    reg("_semaphore_signal", &native_semaphore_signal);
    reg("_semaphore_wait", &native_semaphore_wait);
    reg("__ZNSt3__15mutex4lockEv", &cxx_mutex_lock);
    reg("__ZNSt3__15mutex8try_lockEv", &cxx_mutex_try_lock);
    reg("__ZNSt3__15mutex6unlockEv", &cxx_mutex_unlock);
    reg("__ZNSt3__15mutexD1Ev", &cxx_mutex_destroy);
    reg("__ZNSt3__15mutexD2Ev", &cxx_mutex_destroy);
    reg("__ZNSt3__118condition_variable10notify_oneEv", &cxx_cv_notify_one);
    reg("__ZNSt3__118condition_variable10notify_allEv", &cxx_cv_notify_all);
    reg("__ZNSt3__118condition_variable4waitERNS_11unique_lockINS_5mutexEEE", &cxx_cv_wait);
    reg("__ZNSt3__118condition_variable15__do_timed_waitERNS_11unique_lockINS_5mutexEEENS_6chrono10time_pointINS5_12system_"
        "clockENS5_8durationIxNS_5ratioILl1ELl1000000000EEEEEEE", &cxx_cv_timed_wait);
    reg("__ZNSt3__118condition_variableD1Ev", &cxx_cv_destroy);
    reg("__ZNSt3__118condition_variableD2Ev", &cxx_cv_destroy);
}
