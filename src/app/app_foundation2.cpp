// App mode Foundation, part 2: the main run loop, NSTimer, NSRunLoop, performSelector variants, NSNotificationCenter,
// NSUserDefaults, NSBundle, NSFileManager, NSURL, NSError, NSException, NSAutoreleasePool, NSThread, locks, dates,
// locale/time zone/calendar, NSScanner, NSXMLParser, serialization, keyed archiving, operation queues, the (offline)
// URL loading system and the smaller value classes.
#include "app.hpp"

#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <map>
#include <regex>
#include <set>

extern "C" void loader_bind_thread();   // android_loader.cpp: Apple TSD for threads created outside the guest

namespace app {

id block_copy(id b);
void block_release(id b);
void* block_invoke_ptr(id b);
void declare_protocol(Class c, const char* proto);
bool class_conforms(Class c, const char* proto);

static inline void call_block0(id b) { if (b) reinterpret_cast<void (*)(id)>(block_invoke_ptr(b))(b); }

// ---------------------------------------------------------------------------------------------------------------
// run loop
// ---------------------------------------------------------------------------------------------------------------
namespace {
struct TimerRec { double fire, interval; std::function<void()> fn; };
std::mutex g_loop_mu;
std::condition_variable g_loop_cv;
std::deque<std::function<void()>> g_queue;
std::map<uint64_t, TimerRec> g_timers;
std::multimap<double, uint64_t> g_schedule;
uint64_t g_next_timer = 1;
pthread_t g_main_thread;
bool g_main_set = false;
void (*g_frame_hook)() = nullptr;
}  // namespace

double now_s() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
void mark_main_thread() { g_main_thread = pthread_self(); g_main_set = true; }
bool on_main_thread() { return !g_main_set || pthread_equal(g_main_thread, pthread_self()); }
void set_frame_hook(void (*hook)()) { g_frame_hook = hook; }

void post_main(std::function<void()> fn, double delay) {
    if (delay > 0) { timer_add(delay, 0, std::move(fn)); return; }
    {
        std::lock_guard<std::mutex> l(g_loop_mu);
        g_queue.push_back(std::move(fn));
    }
    g_loop_cv.notify_one();
}

uint64_t timer_add(double delay, double interval, std::function<void()> fn) {
    uint64_t tid;
    {
        std::lock_guard<std::mutex> l(g_loop_mu);
        tid = g_next_timer++;
        double fire = now_s() + std::max(0.0, delay);
        g_timers[tid] = TimerRec{fire, interval, std::move(fn)};
        g_schedule.emplace(fire, tid);
    }
    g_loop_cv.notify_one();
    return tid;
}

void timer_remove(uint64_t tid) {
    std::lock_guard<std::mutex> l(g_loop_mu);
    g_timers.erase(tid);   // its schedule entry is skipped when it comes due
}

void timer_set_next(uint64_t tid, double delay) {
    {
        std::lock_guard<std::mutex> l(g_loop_mu);
        auto it = g_timers.find(tid);
        if (it == g_timers.end()) return;
        it->second.fire = now_s() + std::max(0.0, delay);
        g_schedule.emplace(it->second.fire, tid);
    }
    g_loop_cv.notify_one();
}

void run_loop_once(double max_wait) {
    std::deque<std::function<void()>> work;
    {
        std::unique_lock<std::mutex> l(g_loop_mu);
        double deadline = now_s() + max_wait;
        while (!g_schedule.empty() && !g_timers.count(g_schedule.begin()->second)) g_schedule.erase(g_schedule.begin());
        if (!g_schedule.empty()) deadline = std::min(deadline, g_schedule.begin()->first);
        double wait = deadline - now_s();
        if (g_queue.empty() && wait > 0)
            g_loop_cv.wait_for(l, std::chrono::microseconds(static_cast<int64_t>(wait * 1e6)), [] { return !g_queue.empty(); });
        work.swap(g_queue);
    }
    for (auto& fn : work) {
        void* p = pool_push();
        fn();
        pool_pop(p);
    }
    double now = now_s();
    for (int guard = 0; guard < 256; ++guard) {
        std::function<void()> fn;
        {
            std::lock_guard<std::mutex> l(g_loop_mu);
            if (g_schedule.empty() || g_schedule.begin()->first > now) break;
            auto [fire, tid] = *g_schedule.begin();
            g_schedule.erase(g_schedule.begin());
            auto it = g_timers.find(tid);
            if (it == g_timers.end() || it->second.fire != fire) continue;   // removed or rescheduled
            fn = it->second.fn;
            if (it->second.interval > 0) {
                it->second.fire += it->second.interval;
                if (it->second.fire <= now) it->second.fire = now + it->second.interval;
                g_schedule.emplace(it->second.fire, tid);
            } else {
                g_timers.erase(it);
            }
        }
        void* p = pool_push();
        fn();
        pool_pop(p);
    }
    if (g_frame_hook) {
        void* p = pool_push();
        g_frame_hook();
        pool_pop(p);
    }
}

void run_loop_forever() {
    for (;;) run_loop_once(1.0);
}

struct ThreadStart { std::function<void()> fn; std::string name; };
static void* thread_entry(void* arg) {
    auto* ts = static_cast<ThreadStart*>(arg);
    loader_bind_thread();
    if (!ts->name.empty()) pthread_setname_np(pthread_self(), ts->name.substr(0, 15).c_str());
    void* p = pool_push();
    ts->fn();
    pool_pop(p);
    delete ts;
    return nullptr;
}
void spawn_thread(std::function<void()> fn, const char* name) {
    auto* ts = new ThreadStart{std::move(fn), name ? name : ""};
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 8u << 20);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    pthread_create(&t, &a, thread_entry, ts);
    pthread_attr_destroy(&a);
}

// ---------------------------------------------------------------------------------------------------------------
// files & paths
// ---------------------------------------------------------------------------------------------------------------
bool read_file(const std::string& path, std::string& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? static_cast<size_t>(n) : 0);
    size_t got = n > 0 ? fread(&out[0], 1, static_cast<size_t>(n), f) : 0;
    out.resize(got);
    fclose(f);
    return true;
}

static std::string g_bundle_dir;
std::string bundle_dir() {
    if (g_bundle_dir.empty()) {
        const char* d = getenv("LOADER_GAME_DIR");
        g_bundle_dir = d ? d : "/data/local/tmp/game";
    }
    return g_bundle_dir;
}
std::string sandbox_dir(const char* sub) {
    static const std::string root = getenv("LOADER_SANDBOX_DIR") ? getenv("LOADER_SANDBOX_DIR") : "/data/local/tmp/sandbox";
    std::string p = root;
    mkdir(p.c_str(), 0755);
    for (const char* s = sub; s && *s;) {
        const char* slash = strchr(s, '/');
        p += "/" + std::string(s, slash ? static_cast<size_t>(slash - s) : strlen(s));
        mkdir(p.c_str(), 0755);
        s = slash ? slash + 1 : nullptr;
    }
    return p;
}
std::string remap_path(const std::string& p) {
    // Guests occasionally build paths from a hard-coded iOS container layout
    static const char* kPrefixes[] = {"/var/mobile/Applications/", "/private/var/mobile/"};
    for (const char* pre : kPrefixes) {
        if (p.compare(0, strlen(pre), pre) == 0) {
            size_t d = p.find("/Documents");
            if (d != std::string::npos) return sandbox_dir("Documents") + p.substr(d + 10);
            size_t lib = p.find("/Library");
            if (lib != std::string::npos) return sandbox_dir("Library") + p.substr(lib + 8);
        }
    }
    return p;
}

static id g_info;
id info_dictionary() {
    if (g_info) return g_info;
    std::string data;
    if (read_file(bundle_dir() + "/Info.plist", data))
        g_info = plist_parse(reinterpret_cast<const uint8_t*>(data.data()), data.size(), false);
    if (!g_info) g_info = dict_new(false);
    return g_info;
}

std::string bundle_executable(const std::string& bundle) {
    prepare();   // plist_parse builds NSString/NSDictionary objects, so the native classes must exist first
    std::string data;
    if (!read_file(bundle + "/Info.plist", data)) return "";
    // a plist can be parsed before the runtime exists only through the native classes, which are set up by then
    id d = plist_parse(reinterpret_cast<const uint8_t*>(data.data()), data.size(), false);
    std::string exe = utf8(dict_get(d, "CFBundleExecutable"));
    release(d);
    return exe;
}

// ---------------------------------------------------------------------------------------------------------------
// NSDate
// ---------------------------------------------------------------------------------------------------------------
constexpr double kUnixToRef = 978307200.0;
static Class C_NSDate;
struct DateExt : Ext { double t = 0; };
static double wall_ref_now() {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9 - kUnixToRef;
}
static id date_new(double ref) { id d = alloc(C_NSDate); ext_get<DateExt>(d)->t = ref; return d; }
static id date(double ref) { return autorelease(date_new(ref)); }
static double date_t(id d) { DateExt* e = ext_as<DateExt>(d); return e ? e->t : 0; }
double date_ref(id d) { return date_t(d); }
id make_date(double ref) { return date(ref); }

static void init_date() {
    C_NSDate = def_class("NSDate", "NSObject");
    Class D = C_NSDate;
    def_class("NSMutableDate", "NSDate");
    defc(D, "date", +[](id, SEL) -> id { return date(wall_ref_now()); });
    defc(D, "dateWithTimeIntervalSinceNow:", +[](id, SEL, double s) -> id { return date(wall_ref_now() + s); });
    defc(D, "dateWithTimeIntervalSince1970:", +[](id, SEL, double s) -> id { return date(s - kUnixToRef); });
    defc(D, "dateWithTimeIntervalSinceReferenceDate:", +[](id, SEL, double s) -> id { return date(s); });
    defc(D, "dateWithTimeInterval:sinceDate:", +[](id, SEL, double s, id d) -> id { return date(date_t(d) + s); });
    defc(D, "distantFuture", +[](id, SEL) -> id { return date(63113904000.0); });
    defc(D, "distantPast", +[](id, SEL) -> id { return date(-63114076800.0); });
    defc(D, "timeIntervalSinceReferenceDate", +[](id, SEL) -> double { return wall_ref_now(); });
    def(D, "init", +[](id self, SEL) -> id { ext_get<DateExt>(self)->t = wall_ref_now(); return self; });
    def(D, "initWithTimeIntervalSinceNow:", +[](id self, SEL, double s) -> id { ext_get<DateExt>(self)->t = wall_ref_now() + s; return self; });
    def(D, "initWithTimeIntervalSince1970:", +[](id self, SEL, double s) -> id { ext_get<DateExt>(self)->t = s - kUnixToRef; return self; });
    def(D, "initWithTimeIntervalSinceReferenceDate:", +[](id self, SEL, double s) -> id { ext_get<DateExt>(self)->t = s; return self; });
    def(D, "initWithTimeInterval:sinceDate:", +[](id self, SEL, double s, id d) -> id { ext_get<DateExt>(self)->t = date_t(d) + s; return self; });
    def(D, "timeIntervalSinceReferenceDate", +[](id self, SEL) -> double { return date_t(self); });
    def(D, "timeIntervalSince1970", +[](id self, SEL) -> double { return date_t(self) + kUnixToRef; });
    def(D, "timeIntervalSinceNow", +[](id self, SEL) -> double { return date_t(self) - wall_ref_now(); });
    def(D, "timeIntervalSinceDate:", +[](id self, SEL, id o) -> double { return date_t(self) - date_t(o); });
    def(D, "dateByAddingTimeInterval:", +[](id self, SEL, double s) -> id { return date(date_t(self) + s); });
    def(D, "addTimeInterval:", +[](id self, SEL, double s) -> id { return date(date_t(self) + s); });
    def(D, "compare:", +[](id self, SEL, id o) -> long { double a = date_t(self), b = date_t(o); return a < b ? -1 : a > b ? 1 : 0; });
    def(D, "earlierDate:", +[](id self, SEL, id o) -> id { return date_t(self) <= date_t(o) ? self : o; });
    def(D, "laterDate:", +[](id self, SEL, id o) -> id { return date_t(self) >= date_t(o) ? self : o; });
    def(D, "isEqualToDate:", +[](id self, SEL, id o) -> bool { return o && date_t(self) == date_t(o); });
    def(D, "isEqual:", +[](id self, SEL, id o) -> bool { return o && is_kind(o, C_NSDate) && date_t(self) == date_t(o); });
    def(D, "hash", +[](id self, SEL) -> NSUInteger { return static_cast<NSUInteger>(date_t(self)); });
    def(D, "copyWithZone:", +[](id self, SEL, void*) -> id { return retain(self); });
    def(D, "description", +[](id self, SEL) -> id {
        time_t t = static_cast<time_t>(date_t(self) + kUnixToRef);
        struct tm g;
        gmtime_r(&t, &g);
        char b[64];
        strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S +0000", &g);
        return str(b);
    });
}

// ---------------------------------------------------------------------------------------------------------------
// NSTimer / NSRunLoop / delayed performs
// ---------------------------------------------------------------------------------------------------------------
static Class C_NSTimer, C_NSRunLoop;
struct TimerExt : Ext {
    uint64_t tid = 0;
    id target = nullptr, user_info = nullptr, block = nullptr;
    SEL sel = nullptr;
    double interval = 0;
    bool repeats = false, valid = true, scheduled = false;
    double fire_ref = 0;
    ~TimerExt() override { release(target); release(user_info); if (block) block_release(block); }
};

static void timer_fire(id t);
static void timer_schedule(id t) {
    auto* e = ext_get<TimerExt>(t);
    if (e->scheduled || !e->valid) return;
    e->scheduled = true;
    retain(t);   // the run loop's reference
    double delay = e->fire_ref ? e->fire_ref - wall_ref_now() : e->interval;
    e->tid = timer_add(delay, e->repeats ? std::max(e->interval, 0.0001) : 0, [t] { timer_fire(t); });
}
static void timer_invalidate(id t) {
    auto* e = ext_get<TimerExt>(t);
    if (!e->valid) return;
    e->valid = false;
    id target = e->target, ui = e->user_info;
    e->target = e->user_info = nullptr;
    if (e->scheduled) { timer_remove(e->tid); e->scheduled = false; release(target); release(ui); release(t); }
    else { release(target); release(ui); }
}
static void timer_fire(id t) {
    auto* e = ext_get<TimerExt>(t);
    if (!e->valid) return;
    retain(t);
    if (e->block) reinterpret_cast<void (*)(id, id)>(block_invoke_ptr(e->block))(e->block, t);
    else if (e->target) send<void>(e->target, e->sel, t);
    if (!e->repeats) timer_invalidate(t);
    release(t);
}
static id timer_make(double interval, id target, SEL s, id ui, bool repeats) {
    id t = alloc(C_NSTimer);
    auto* e = ext_get<TimerExt>(t);
    e->interval = interval < 0 ? 0 : interval;
    e->target = retain(target);
    e->sel = s;
    e->user_info = retain(ui);
    e->repeats = repeats;
    return autorelease(t);
}

struct Perform { uint64_t tid; id target; SEL sel; id arg; };
static std::mutex g_perform_mu;
static std::vector<Perform>* g_performs;
static void perform_after(id target, SEL s, id arg, double delay) {
    retain(target);
    retain(arg);
    std::lock_guard<std::mutex> l(g_perform_mu);
    if (!g_performs) g_performs = new std::vector<Perform>();
    auto* holder = new uint64_t(0);
    uint64_t tid = timer_add(delay, 0, [target, s, arg, holder] {
        {
            std::lock_guard<std::mutex> l2(g_perform_mu);
            auto& v = *g_performs;
            v.erase(std::remove_if(v.begin(), v.end(), [&](const Perform& p) { return p.tid == *holder; }), v.end());
        }
        delete holder;
        send<void>(target, s, arg);
        release(target);
        release(arg);
    });
    *holder = tid;
    g_performs->push_back({tid, target, s, arg});
}
static void cancel_performs(id target, SEL s, id arg, bool match_all) {
    std::vector<Perform> dead;
    {
        std::lock_guard<std::mutex> l(g_perform_mu);
        if (!g_performs) return;
        auto& v = *g_performs;
        for (auto it = v.begin(); it != v.end();) {
            if (it->target == target && (match_all || (it->sel == s && obj_equal(it->arg, arg)))) {
                dead.push_back(*it);
                it = v.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& p : dead) { timer_remove(p.tid); release(p.target); release(p.arg); }
}

static void run_on_main_sync(std::function<void()> fn) {
    if (on_main_thread()) { fn(); return; }
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    post_main([&] { fn(); std::lock_guard<std::mutex> l(m); done = true; cv.notify_one(); });
    std::unique_lock<std::mutex> l(m);
    cv.wait(l, [&] { return done; });
}

static id g_main_loop_obj;
static void init_timers() {
    C_NSTimer = def_class("NSTimer", "NSObject");
    Class T = C_NSTimer;
    defc(T, "scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:", +[](id, SEL, double iv, id target, SEL s, id ui, bool rep) -> id {
        id t = timer_make(iv, target, s, ui, rep);
        timer_schedule(t);
        return t;
    });
    defc(T, "timerWithTimeInterval:target:selector:userInfo:repeats:", +[](id, SEL, double iv, id target, SEL s, id ui, bool rep) -> id {
        return timer_make(iv, target, s, ui, rep);
    });
    defc(T, "scheduledTimerWithTimeInterval:repeats:block:", +[](id, SEL, double iv, bool rep, id block) -> id {
        id t = timer_make(iv, nullptr, nullptr, nullptr, rep);
        ext_get<TimerExt>(t)->block = block_copy(block);
        timer_schedule(t);
        return t;
    });
    defc(T, "scheduledTimerWithTimeInterval:invocation:repeats:", +[](id, SEL, double iv, id inv, bool rep) -> id {
        id t = timer_make(iv, inv, sel("invoke"), nullptr, rep);
        timer_schedule(t);
        return t;
    });
    def(T, "initWithFireDate:interval:target:selector:userInfo:repeats:", +[](id self, SEL, id fd, double iv, id target, SEL s, id ui, bool rep) -> id {
        auto* e = ext_get<TimerExt>(self);
        e->interval = iv; e->target = retain(target); e->sel = s; e->user_info = retain(ui); e->repeats = rep; e->fire_ref = date_t(fd);
        return self;
    });
    def(T, "invalidate", +[](id self, SEL) { timer_invalidate(self); });
    def(T, "isValid", +[](id self, SEL) -> bool { return ext_get<TimerExt>(self)->valid; });
    def(T, "fire", +[](id self, SEL) {
        auto* e = ext_get<TimerExt>(self);
        if (!e->valid) return;
        if (e->target) send<void>(e->target, e->sel, self);
        if (!e->repeats) timer_invalidate(self);
    });
    def(T, "userInfo", +[](id self, SEL) -> id { return ext_get<TimerExt>(self)->user_info; });
    def(T, "timeInterval", +[](id self, SEL) -> double { return ext_get<TimerExt>(self)->interval; });
    def(T, "fireDate", +[](id self, SEL) -> id { return date(wall_ref_now() + ext_get<TimerExt>(self)->interval); });
    def(T, "setFireDate:", +[](id self, SEL, id d) {
        auto* e = ext_get<TimerExt>(self);
        double delay = date_t(d) - wall_ref_now();
        if (e->scheduled) timer_set_next(e->tid, delay);
        else e->fire_ref = date_t(d);
    });
    def(T, "setTolerance:", +[](id, SEL, double) {});
    def(T, "tolerance", +[](id, SEL) -> double { return 0; });

    C_NSRunLoop = def_class("NSRunLoop", "NSObject");
    Class R = C_NSRunLoop;
    g_main_loop_obj = alloc(R);
    defc(R, "currentRunLoop", +[](id, SEL) -> id { return g_main_loop_obj; });
    defc(R, "mainRunLoop", +[](id, SEL) -> id { return g_main_loop_obj; });
    def(R, "addTimer:forMode:", +[](id, SEL, id t, id) { timer_schedule(t); });
    def(R, "addPort:forMode:", +[](id, SEL, id, id) {});
    def(R, "removePort:forMode:", +[](id, SEL, id, id) {});
    def(R, "getCFRunLoop", +[](id self, SEL) -> id { return self; });
    def(R, "currentMode", +[](id, SEL) -> id { return str("kCFRunLoopDefaultMode"); });
    def(R, "run", +[](id, SEL) {
        if (on_main_thread()) run_loop_forever();
        for (;;) sleep(3600);
    });
    def(R, "runMode:beforeDate:", +[](id, SEL, id, id d) -> bool {
        if (!on_main_thread()) { usleep(10000); return true; }
        double wait = d ? std::min(1.0, std::max(0.0, date_t(d) - wall_ref_now())) : 0;
        run_loop_once(wait);
        return true;
    });
    def(R, "runUntilDate:", +[](id, SEL, id d) {
        double end = date_t(d);
        if (!on_main_thread()) { while (wall_ref_now() < end) usleep(10000); return; }
        while (wall_ref_now() < end) run_loop_once(std::min(0.1, end - wall_ref_now()));
    });
    def(R, "performSelector:target:argument:order:modes:", +[](id, SEL, SEL s, id target, id arg, NSUInteger, id) { perform_after(target, s, arg, 0); });

    Class O = class_named("NSObject");
    def(O, "performSelector:withObject:afterDelay:", +[](id self, SEL, SEL s, id arg, double d) { perform_after(self, s, arg, d); });
    def(O, "performSelector:withObject:afterDelay:inModes:", +[](id self, SEL, SEL s, id arg, double d, id) { perform_after(self, s, arg, d); });
    def(O, "performSelectorOnMainThread:withObject:waitUntilDone:", +[](id self, SEL, SEL s, id arg, bool wait) {
        if (wait) { run_on_main_sync([=] { send<void>(self, s, arg); }); return; }
        perform_after(self, s, arg, 0);
    });
    def(O, "performSelectorOnMainThread:withObject:waitUntilDone:modes:", +[](id self, SEL, SEL s, id arg, bool wait, id) {
        if (wait) { run_on_main_sync([=] { send<void>(self, s, arg); }); return; }
        perform_after(self, s, arg, 0);
    });
    def(O, "performSelector:onThread:withObject:waitUntilDone:", +[](id self, SEL, SEL s, id, id arg, bool wait) {
        if (wait) { run_on_main_sync([=] { send<void>(self, s, arg); }); return; }
        perform_after(self, s, arg, 0);
    });
    def(O, "performSelector:onThread:withObject:waitUntilDone:modes:", +[](id self, SEL, SEL s, id, id arg, bool wait, id) {
        if (wait) { run_on_main_sync([=] { send<void>(self, s, arg); }); return; }
        perform_after(self, s, arg, 0);
    });
    def(O, "performSelectorInBackground:withObject:", +[](id self, SEL, SEL s, id arg) {
        retain(self); retain(arg);
        spawn_thread([=] { send<void>(self, s, arg); release(self); release(arg); }, "perform");
    });
    defc(O, "cancelPreviousPerformRequestsWithTarget:", +[](id, SEL, id target) { cancel_performs(target, nullptr, nullptr, true); });
    defc(O, "cancelPreviousPerformRequestsWithTarget:selector:object:", +[](id, SEL, id target, SEL s, id arg) { cancel_performs(target, s, arg, false); });
}

// ---------------------------------------------------------------------------------------------------------------
// KVC on NSObject (accessors and ivars)
// ---------------------------------------------------------------------------------------------------------------
extern "C" void* app_class_get_instance_variable(Class c, const char* name);
static std::string cap_first(const std::string& k) { std::string s = k; if (!s.empty()) s[0] = static_cast<char>(toupper(s[0])); return s; }
static id box_scalar(const void* p, char t) {
    switch (t) {
        case 'c': return num_i(*static_cast<const signed char*>(p));
        case 'B': return num_b(*static_cast<const bool*>(p));
        case 'C': return num_i(*static_cast<const unsigned char*>(p));
        case 's': return num_i(*static_cast<const short*>(p));
        case 'S': return num_i(*static_cast<const unsigned short*>(p));
        case 'i': return num_i(*static_cast<const int*>(p));
        case 'I': return num_i(*static_cast<const unsigned*>(p));
        case 'l': case 'q': return num_i(*static_cast<const long long*>(p));
        case 'L': case 'Q': return num_u(*static_cast<const unsigned long long*>(p));
        case 'f': return num_f(*static_cast<const float*>(p));
        case 'd': return num_d(*static_cast<const double*>(p));
        default: return nullptr;
    }
}
static const char* method_return_type(id o, SEL s) {
    (void)o; (void)s;
    return nullptr;
}
static id kvc_get(id self, const std::string& key) {
    if (key.empty()) return nullptr;
    SEL g = sel(key.c_str());
    if (responds(self, g)) {
        // without type information assume an object getter
        return send(self, g);
    }
    SEL is = sel(("is" + cap_first(key)).c_str());
    if (responds(self, is)) return num_b(send<bool>(self, is));
    for (std::string n : {"_" + key, key}) {
        void* iv = app_class_get_instance_variable(class_of(self), n.c_str());
        if (!iv) continue;
        uintptr_t offp = *reinterpret_cast<uintptr_t*>(iv);
        const char* type = *reinterpret_cast<const char**>(static_cast<uint8_t*>(iv) + 16);
        int32_t off = offp ? *reinterpret_cast<int32_t*>(offp) : 0;
        const void* p = reinterpret_cast<uint8_t*>(self) + off;
        if (type && type[0] == '@') return *static_cast<id const*>(p);
        return type ? box_scalar(p, type[0]) : nullptr;
    }
    (void)method_return_type;
    return send(self, "valueForUndefinedKey:", str(key));
}
static void kvc_set(id self, const std::string& key, id v) {
    SEL s = sel(("set" + cap_first(key) + ":").c_str());
    if (responds(self, s)) { send<void>(self, s, v); return; }
    for (std::string n : {"_" + key, key}) {
        void* iv = app_class_get_instance_variable(class_of(self), n.c_str());
        if (!iv) continue;
        uintptr_t offp = *reinterpret_cast<uintptr_t*>(iv);
        const char* type = *reinterpret_cast<const char**>(static_cast<uint8_t*>(iv) + 16);
        int32_t off = offp ? *reinterpret_cast<int32_t*>(offp) : 0;
        void* p = reinterpret_cast<uint8_t*>(self) + off;
        switch (type ? type[0] : '@') {
            case '@': { id old = *static_cast<id*>(p); *static_cast<id*>(p) = retain(v); release(old); break; }
            case 'c': case 'B': case 'C': *static_cast<int8_t*>(p) = static_cast<int8_t>(num_ival(v)); break;
            case 's': case 'S': *static_cast<int16_t*>(p) = static_cast<int16_t>(num_ival(v)); break;
            case 'i': case 'I': *static_cast<int32_t*>(p) = static_cast<int32_t>(num_ival(v)); break;
            case 'l': case 'q': case 'L': case 'Q': *static_cast<int64_t*>(p) = num_ival(v); break;
            case 'f': *static_cast<float*>(p) = static_cast<float>(num_dval(v)); break;
            case 'd': *static_cast<double*>(p) = num_dval(v); break;
            default: break;
        }
        return;
    }
    send<void>(self, "setValue:forUndefinedKey:", v, str(key));
}
static void init_kvc() {
    Class O = class_named("NSObject");
    def(O, "valueForKey:", +[](id self, SEL, id k) -> id { return kvc_get(self, utf8(k)); });
    def(O, "setValue:forKey:", +[](id self, SEL, id v, id k) { kvc_set(self, utf8(k), v); });
    def(O, "valueForKeyPath:", +[](id self, SEL, id kp) -> id {
        std::string p = utf8(kp);
        id cur = self;
        size_t i = 0;
        while (cur) {
            size_t k = p.find('.', i);
            std::string part = p.substr(i, k == std::string::npos ? std::string::npos : k - i);
            cur = send(cur, "valueForKey:", str(part));
            if (k == std::string::npos) break;
            i = k + 1;
        }
        return cur;
    });
    def(O, "setValue:forKeyPath:", +[](id self, SEL, id v, id kp) {
        std::string p = utf8(kp);
        size_t k = p.rfind('.');
        id target = k == std::string::npos ? self : send(self, "valueForKeyPath:", str(p.substr(0, k)));
        send<void>(target, "setValue:forKey:", v, str(k == std::string::npos ? p : p.substr(k + 1)));
    });
    def(O, "setValuesForKeysWithDictionary:", +[](id self, SEL, id d) { for (auto& kv : dict_entries(d)) send<void>(self, "setValue:forKey:", kv.second, kv.first); });
    def(O, "dictionaryWithValuesForKeys:", +[](id self, SEL, id keys) -> id {
        id d = autorelease(dict_new());
        for (id k : arr_items(keys)) { id v = send(self, "valueForKey:", k); dict_set(d, k, v ? v : send_cls(class_named("NSNull"), "null")); }
        return d;
    });
    def(O, "valueForUndefinedKey:", +[](id self, SEL, id k) -> id { log("valueForUndefinedKey: %s on %s", utf8(k).c_str(), class_name(class_of(self))); return nullptr; });
    def(O, "setValue:forUndefinedKey:", +[](id self, SEL, id, id k) { log("setValue:forUndefinedKey: %s on %s", utf8(k).c_str(), class_name(class_of(self))); });
    def(O, "setNilValueForKey:", +[](id, SEL, id) {});
}

// ---------------------------------------------------------------------------------------------------------------
// NSNotificationCenter
// ---------------------------------------------------------------------------------------------------------------
static Class C_NSNotification, C_NSNotificationCenter;
struct NoteExt : Ext { id name = nullptr, object = nullptr, info = nullptr; ~NoteExt() override { release(name); release(object); release(info); } };
struct Observer { id observer; SEL sel; id name; id object; id block; uint64_t serial; };
static std::mutex g_nc_mu;
static std::vector<Observer>* g_observers;
static std::set<id>* g_observer_set;
static uint64_t g_obs_serial = 0;
static id g_center;

static id note_new(id name, id object, id info) {
    id n = alloc(C_NSNotification);
    auto* e = ext_get<NoteExt>(n);
    e->name = retain(name);
    e->object = retain(object);
    e->info = retain(info);
    return autorelease(n);
}
static void nc_remove(id observer, id name, id object, bool all) {
    std::vector<id> blocks;
    {
        std::lock_guard<std::mutex> l(g_nc_mu);
        if (!g_observers) return;
        auto& v = *g_observers;
        for (auto it = v.begin(); it != v.end();) {
            if (it->observer == observer && (all || ((!name || obj_equal(it->name, name)) && (!object || it->object == object)))) {
                release(it->name);
                if (it->block) blocks.push_back(it->block);
                it = v.erase(it);
            } else {
                ++it;
            }
        }
        bool still = false;
        for (auto& o : v) if (o.observer == observer) { still = true; break; }
        if (!still && g_observer_set) g_observer_set->erase(observer);
    }
    for (id b : blocks) block_release(b);
}
static void nc_dealloc_hook(id o) {
    {
        std::lock_guard<std::mutex> l(g_nc_mu);
        if (!g_observer_set || !g_observer_set->count(o)) return;
    }
    nc_remove(o, nullptr, nullptr, true);
}
void post_notification(id note) {
    auto* e = ext_get<NoteExt>(note);
    std::vector<Observer> hits;
    {
        std::lock_guard<std::mutex> l(g_nc_mu);
        if (!g_observers) return;
        for (auto& o : *g_observers)
            if ((!o.name || obj_equal(o.name, e->name)) && (!o.object || o.object == e->object)) hits.push_back(o);
    }
    if (trace_enabled("note")) log("notification %s -> %zu observers", utf8(e->name).c_str(), hits.size());
    for (auto& o : hits) {
        {   // skip observers removed by an earlier one in this pass
            std::lock_guard<std::mutex> l(g_nc_mu);
            bool alive = false;
            for (auto& x : *g_observers) if (x.serial == o.serial) { alive = true; break; }
            if (!alive) continue;
        }
        if (o.block) reinterpret_cast<void (*)(id, id)>(block_invoke_ptr(o.block))(o.block, note);
        else send<void>(o.observer, o.sel, note);
    }
}
void post_note(const char* name, id object, id info) { post_notification(note_new(str(name), object, info)); }

static void init_notifications() {
    C_NSNotification = def_class("NSNotification", "NSObject");
    Class N = C_NSNotification;
    defc(N, "notificationWithName:object:", +[](id, SEL, id n, id o) -> id { return note_new(n, o, nullptr); });
    defc(N, "notificationWithName:object:userInfo:", +[](id, SEL, id n, id o, id i) -> id { return note_new(n, o, i); });
    def(N, "initWithName:object:userInfo:", +[](id self, SEL, id n, id o, id i) -> id {
        auto* e = ext_get<NoteExt>(self); e->name = retain(n); e->object = retain(o); e->info = retain(i); return self;
    });
    def(N, "name", +[](id self, SEL) -> id { return ext_get<NoteExt>(self)->name; });
    def(N, "object", +[](id self, SEL) -> id { return ext_get<NoteExt>(self)->object; });
    def(N, "userInfo", +[](id self, SEL) -> id { return ext_get<NoteExt>(self)->info; });

    C_NSNotificationCenter = def_class("NSNotificationCenter", "NSObject");
    Class C = C_NSNotificationCenter;
    g_center = alloc(C);
    add_dealloc_hook(nc_dealloc_hook);
    defc(C, "defaultCenter", +[](id, SEL) -> id { return g_center; });
    def(C, "addObserver:selector:name:object:", +[](id, SEL, id obs, SEL s, id name, id object) {
        if (!obs) return;
        std::lock_guard<std::mutex> l(g_nc_mu);
        if (!g_observers) { g_observers = new std::vector<Observer>(); g_observer_set = new std::set<id>(); }
        g_observers->push_back({obs, s, name ? (is_string(name) ? str16_new(u16(name)) : retain(name)) : nullptr, object, nullptr, ++g_obs_serial});
        g_observer_set->insert(obs);
    });
    def(C, "addObserverForName:object:queue:usingBlock:", +[](id, SEL, id name, id object, id, id block) -> id {
        id token = autorelease(alloc(class_named("NSObject")));
        retain(token);
        std::lock_guard<std::mutex> l(g_nc_mu);
        if (!g_observers) { g_observers = new std::vector<Observer>(); g_observer_set = new std::set<id>(); }
        g_observers->push_back({token, nullptr, name ? str16_new(u16(name)) : nullptr, object, block_copy(block), ++g_obs_serial});
        g_observer_set->insert(token);
        return token;
    });
    def(C, "removeObserver:", +[](id, SEL, id obs) { nc_remove(obs, nullptr, nullptr, true); });
    def(C, "removeObserver:name:object:", +[](id, SEL, id obs, id name, id object) { nc_remove(obs, name, object, !name && !object); });
    def(C, "postNotification:", +[](id, SEL, id n) { post_notification(n); });
    def(C, "postNotificationName:object:", +[](id, SEL, id name, id object) { post_notification(note_new(name, object, nullptr)); });
    def(C, "postNotificationName:object:userInfo:", +[](id, SEL, id name, id object, id info) { post_notification(note_new(name, object, info)); });
}

// ---------------------------------------------------------------------------------------------------------------
// NSUserDefaults
// ---------------------------------------------------------------------------------------------------------------
static Class C_NSUserDefaults;
static id g_defaults_obj, g_defaults, g_registered;
static bool g_defaults_dirty = false, g_defaults_save_queued = false;
static std::recursive_mutex g_defaults_mu;
static std::string defaults_path() {
    std::string bid = utf8(dict_get(info_dictionary(), "CFBundleIdentifier"));
    if (bid.empty()) bid = "app";
    return sandbox_dir("Library/Preferences") + "/" + bid + ".plist";
}
static void defaults_load() {
    if (g_defaults) return;
    std::string data;
    if (read_file(defaults_path(), data)) g_defaults = plist_parse(reinterpret_cast<const uint8_t*>(data.data()), data.size(), true);
    if (!is_dict(g_defaults)) { release(g_defaults); g_defaults = dict_new(true); }
    g_registered = dict_new(true);
}
static bool defaults_save() {
    std::lock_guard<std::recursive_mutex> l(g_defaults_mu);
    g_defaults_save_queued = false;
    if (!g_defaults_dirty) return true;
    std::string out;
    if (!plist_write_xml(g_defaults, out)) return false;
    std::string p = defaults_path(), tmp = p + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    rename(tmp.c_str(), p.c_str());
    g_defaults_dirty = false;
    return true;
}
static void defaults_touch() {
    g_defaults_dirty = true;
    if (!g_defaults_save_queued) { g_defaults_save_queued = true; post_main([] { defaults_save(); }, 2.0); }
}
static id defaults_get(id key) {
    std::lock_guard<std::recursive_mutex> l(g_defaults_mu);
    defaults_load();
    id v = dict_get(g_defaults, key);
    return v ? v : dict_get(g_registered, key);
}
static void defaults_set(id key, id v) {
    std::lock_guard<std::recursive_mutex> l(g_defaults_mu);
    defaults_load();
    if (v) { id c = send(v, "copy"); dict_set(g_defaults, key, c); release(c); }
    else dict_set(g_defaults, key, nullptr);
    defaults_touch();
}
void defaults_flush() { defaults_save(); }

static void init_defaults() {
    C_NSUserDefaults = def_class("NSUserDefaults", "NSObject");
    Class U = C_NSUserDefaults;
    g_defaults_obj = alloc(U);
    defc(U, "standardUserDefaults", +[](id, SEL) -> id { return g_defaults_obj; });
    defc(U, "resetStandardUserDefaults", +[](id, SEL) {});
    def(U, "initWithSuiteName:", +[](id self, SEL, id) -> id { release(self); return g_defaults_obj; });
    def(U, "objectForKey:", +[](id, SEL, id k) -> id { return defaults_get(k); });
    def(U, "valueForKey:", +[](id, SEL, id k) -> id { return defaults_get(k); });
    def(U, "stringForKey:", +[](id, SEL, id k) -> id { id v = defaults_get(k); return is_string(v) ? v : is_number(v) ? send(v, "stringValue") : nullptr; });
    def(U, "arrayForKey:", +[](id, SEL, id k) -> id { id v = defaults_get(k); return is_array(v) ? v : nullptr; });
    def(U, "dictionaryForKey:", +[](id, SEL, id k) -> id { id v = defaults_get(k); return is_dict(v) ? v : nullptr; });
    def(U, "dataForKey:", +[](id, SEL, id k) -> id { id v = defaults_get(k); return is_data(v) ? v : nullptr; });
    def(U, "stringArrayForKey:", +[](id, SEL, id k) -> id { id v = defaults_get(k); return is_array(v) ? v : nullptr; });
    def(U, "boolForKey:", +[](id, SEL, id k) -> bool { id v = defaults_get(k); return is_string(v) ? send<bool>(v, "boolValue") : num_ival(v) != 0; });
    def(U, "integerForKey:", +[](id, SEL, id k) -> long { return num_ival(defaults_get(k)); });
    def(U, "floatForKey:", +[](id, SEL, id k) -> float { return static_cast<float>(num_dval(defaults_get(k))); });
    def(U, "doubleForKey:", +[](id, SEL, id k) -> double { return num_dval(defaults_get(k)); });
    def(U, "URLForKey:", +[](id, SEL, id k) -> id { id v = defaults_get(k); return is_string(v) ? send_cls(class_named("NSURL"), "URLWithString:", v) : nullptr; });
    def(U, "setObject:forKey:", +[](id, SEL, id v, id k) { defaults_set(k, v); });
    def(U, "setValue:forKey:", +[](id, SEL, id v, id k) { defaults_set(k, v); });
    def(U, "setBool:forKey:", +[](id, SEL, bool v, id k) { defaults_set(k, num_b(v)); });
    def(U, "setInteger:forKey:", +[](id, SEL, long v, id k) { defaults_set(k, num_i(v)); });
    def(U, "setFloat:forKey:", +[](id, SEL, float v, id k) { defaults_set(k, num_f(v)); });
    def(U, "setDouble:forKey:", +[](id, SEL, double v, id k) { defaults_set(k, num_d(v)); });
    def(U, "setURL:forKey:", +[](id, SEL, id v, id k) { defaults_set(k, send(v, "absoluteString")); });
    def(U, "removeObjectForKey:", +[](id, SEL, id k) { defaults_set(k, nullptr); });
    def(U, "registerDefaults:", +[](id, SEL, id d) {
        std::lock_guard<std::recursive_mutex> l(g_defaults_mu);
        defaults_load();
        for (auto& kv : dict_entries(d)) dict_set(g_registered, kv.first, kv.second);
    });
    def(U, "synchronize", +[](id, SEL) -> bool { return defaults_save(); });
    def(U, "dictionaryRepresentation", +[](id, SEL) -> id {
        std::lock_guard<std::recursive_mutex> l(g_defaults_mu);
        defaults_load();
        id d = autorelease(dict_new());
        for (auto& kv : dict_entries(g_registered)) dict_set(d, kv.first, kv.second);
        for (auto& kv : dict_entries(g_defaults)) dict_set(d, kv.first, kv.second);
        return d;
    });
    def(U, "persistentDomainForName:", +[](id, SEL, id) -> id { std::lock_guard<std::recursive_mutex> l(g_defaults_mu); defaults_load(); return g_defaults; });
    def(U, "removePersistentDomainForName:", +[](id, SEL, id) {
        std::lock_guard<std::recursive_mutex> l(g_defaults_mu); defaults_load(); send<void>(g_defaults, "removeAllObjects"); defaults_touch();
    });
}

// ---------------------------------------------------------------------------------------------------------------
// NSBundle
// ---------------------------------------------------------------------------------------------------------------
static Class C_NSBundle;
static id g_main_bundle;
static bool file_exists(const std::string& p, bool* is_dir = nullptr) {
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return false;
    if (is_dir) *is_dir = S_ISDIR(st.st_mode);
    return true;
}
static std::vector<std::string> localization_dirs() { return {"", "en.lproj", "English.lproj", "Base.lproj"}; }
static std::string bundle_find(const std::string& name, const std::string& type, const std::string& dir) {
    std::string file = name;
    if (!type.empty()) file += "." + type;
    std::string base = bundle_dir();
    if (!dir.empty()) base += "/" + dir;
    for (const auto& l : localization_dirs()) {
        std::string p = l.empty() ? base + "/" + file : base + "/" + l + "/" + file;
        if (file_exists(p)) return p;
    }
    return "";
}
static std::unordered_map<std::string, id>* g_strings_tables;
static id strings_table(const std::string& table) {
    if (!g_strings_tables) g_strings_tables = new std::unordered_map<std::string, id>();
    auto it = g_strings_tables->find(table);
    if (it != g_strings_tables->end()) return it->second;
    id d = nullptr;
    std::string p = bundle_find(table, "strings", "");
    std::string data;
    if (!p.empty() && read_file(p, data)) {
        d = plist_parse(reinterpret_cast<const uint8_t*>(data.data()), data.size(), false);
        if (!is_dict(d)) { release(d); d = nullptr; }
    }
    (*g_strings_tables)[table] = d;
    return d;
}

static void init_bundle() {
    C_NSBundle = def_class("NSBundle", "NSObject");
    Class B = C_NSBundle;
    g_main_bundle = alloc(B);
    defc(B, "mainBundle", +[](id, SEL) -> id { return g_main_bundle; });
    defc(B, "bundleForClass:", +[](id, SEL, Class) -> id { return g_main_bundle; });
    defc(B, "bundleWithIdentifier:", +[](id, SEL, id) -> id { return nullptr; });
    defc(B, "bundleWithPath:", +[](id, SEL, id p) -> id { return utf8(p) == bundle_dir() ? g_main_bundle : nullptr; });
    defc(B, "allBundles", +[](id, SEL) -> id { return arr({g_main_bundle}); });
    defc(B, "allFrameworks", +[](id, SEL) -> id { return arr({}); });
    def(B, "bundlePath", +[](id, SEL) -> id { return str(bundle_dir()); });
    def(B, "resourcePath", +[](id, SEL) -> id { return str(bundle_dir()); });
    def(B, "bundleURL", +[](id, SEL) -> id { return send_cls(class_named("NSURL"), "fileURLWithPath:", str(bundle_dir())); });
    def(B, "resourceURL", +[](id, SEL) -> id { return send_cls(class_named("NSURL"), "fileURLWithPath:", str(bundle_dir())); });
    def(B, "executablePath", +[](id, SEL) -> id { return str(bundle_dir() + "/" + utf8(dict_get(info_dictionary(), "CFBundleExecutable"))); });
    def(B, "privateFrameworksPath", +[](id, SEL) -> id { return str(bundle_dir() + "/Frameworks"); });
    def(B, "builtInPlugInsPath", +[](id, SEL) -> id { return str(bundle_dir() + "/PlugIns"); });
    def(B, "appStoreReceiptURL", +[](id, SEL) -> id { return send_cls(class_named("NSURL"), "fileURLWithPath:", str(bundle_dir() + "/StoreKit/receipt")); });
    def(B, "bundleIdentifier", +[](id, SEL) -> id { return dict_get(info_dictionary(), "CFBundleIdentifier"); });
    def(B, "infoDictionary", +[](id, SEL) -> id { return info_dictionary(); });
    def(B, "localizedInfoDictionary", +[](id, SEL) -> id { return info_dictionary(); });
    def(B, "objectForInfoDictionaryKey:", +[](id, SEL, id k) -> id { return dict_get(info_dictionary(), k); });
    def(B, "isLoaded", +[](id, SEL) -> bool { return true; });
    def(B, "load", +[](id, SEL) -> bool { return true; });
    def(B, "principalClass", +[](id, SEL) -> Class { return 0; });
    def(B, "preferredLocalizations", +[](id, SEL) -> id { return arr({str("en")}); });
    def(B, "localizations", +[](id, SEL) -> id { return arr({str("en")}); });
    def(B, "developmentLocalization", +[](id, SEL) -> id { return str("en"); });
    defc(B, "preferredLocalizationsFromArray:", +[](id, SEL, id a) -> id {
        for (id x : arr_items(a)) if (utf8(x) == "en" || utf8(x) == "English" || utf8(x) == "Base") return arr({x});
        const auto& items = arr_items(a);
        return items.empty() ? arr({}) : arr({items[0]});
    });
    def(B, "pathForResource:ofType:", +[](id, SEL, id name, id type) -> id {
        std::string p = bundle_find(utf8(name), utf8(type), "");
        if (p.empty() && trace_enabled("bundle")) log("pathForResource %s.%s not found", utf8(name).c_str(), utf8(type).c_str());
        return p.empty() ? nullptr : str(p);
    });
    def(B, "pathForResource:ofType:inDirectory:", +[](id, SEL, id name, id type, id dir) -> id {
        std::string p = bundle_find(utf8(name), utf8(type), utf8(dir));
        return p.empty() ? nullptr : str(p);
    });
    def(B, "pathForResource:ofType:inDirectory:forLocalization:", +[](id, SEL, id name, id type, id dir, id) -> id {
        std::string p = bundle_find(utf8(name), utf8(type), utf8(dir));
        return p.empty() ? nullptr : str(p);
    });
    def(B, "URLForResource:withExtension:", +[](id, SEL, id name, id type) -> id {
        std::string p = bundle_find(utf8(name), utf8(type), "");
        return p.empty() ? nullptr : send_cls(class_named("NSURL"), "fileURLWithPath:", str(p));
    });
    def(B, "URLForResource:withExtension:subdirectory:", +[](id, SEL, id name, id type, id dir) -> id {
        std::string p = bundle_find(utf8(name), utf8(type), utf8(dir));
        return p.empty() ? nullptr : send_cls(class_named("NSURL"), "fileURLWithPath:", str(p));
    });
    def(B, "pathsForResourcesOfType:inDirectory:", +[](id, SEL, id type, id dir) -> id {
        std::string base = bundle_dir();
        if (!utf8(dir).empty()) base += "/" + utf8(dir);
        std::string ext = utf8(type);
        std::vector<id> out;
        if (DIR* d = opendir(base.c_str())) {
            while (dirent* e = readdir(d)) {
                std::string n = e->d_name;
                if (n == "." || n == "..") continue;
                if (ext.empty() || (n.size() > ext.size() + 1 && n.compare(n.size() - ext.size() - 1, std::string::npos, "." + ext) == 0))
                    out.push_back(str(base + "/" + n));
            }
            closedir(d);
        }
        return arr(out);
    });
    def(B, "localizedStringForKey:value:table:", +[](id, SEL, id key, id value, id table) -> id {
        std::string t = utf8(table);
        if (t.empty()) t = "Localizable";
        id d = strings_table(t);
        id v = d ? dict_get(d, key) : nullptr;
        if (v) return v;
        return value && !u16(value).empty() ? value : key;
    });
    def(B, "loadNibNamed:owner:options:", +[](id, SEL, id n, id, id) -> id { log("loadNibNamed:%s not supported", utf8(n).c_str()); return arr({}); });
}

// ---------------------------------------------------------------------------------------------------------------
// NSError / NSException
// ---------------------------------------------------------------------------------------------------------------
static Class C_NSError, C_NSException;
struct ErrExt : Ext { id domain = nullptr, info = nullptr; long code = 0; ~ErrExt() override { release(domain); release(info); } };
id make_error(const char* domain, long code, const char* desc) {
    id e = alloc(C_NSError);
    auto* x = ext_get<ErrExt>(e);
    x->domain = str_new(domain);
    x->code = code;
    if (desc) { x->info = dict_new(false); dict_set(x->info, "NSLocalizedDescription", str(desc)); }
    return autorelease(e);
}
struct ExcExt : Ext { id name = nullptr, reason = nullptr, info = nullptr; ~ExcExt() override { release(name); release(reason); release(info); } };
// There is no Objective-C exception unwinder here, so a raise cannot transfer to a @catch. Rather than abort
// (which kills the whole app, e.g. when a bundled analytics SDK rejects its own config), log and return: the
// raising code falls through, which for an error path is usually harmless. A runaway raise loop still aborts.
static std::atomic<int> g_raise_count{0};
static void note_raise() {
    if (g_raise_count.fetch_add(1) > 200) { log("too many NSException raises; aborting"); fflush(stderr); abort(); }
}
extern "C" void app_impl_raise_format(id, SEL, id name, id fmt, const uint64_t* va) {
    log("*** NSException raise (ignored): %s: %s", utf8(name).c_str(), format(utf8(fmt), va).c_str());
    note_raise();
}
APP_VARIADIC(app_th_raise_format, app_impl_raise_format, 4)
extern "C" void app_impl_assert_failure(id, SEL, SEL method, id obj, id file, long line, id fmt, const uint64_t* va) {
    log("*** Assertion failure in -[%s %s], %s:%ld: %s", obj ? class_name(class_of(obj)) : "?", method ? method : "?",
        utf8(file).c_str(), line, format(utf8(fmt), va).c_str());
}
APP_VARIADIC(app_th_assert_failure, app_impl_assert_failure, 7)
extern "C" void app_impl_assert_failure_fn(id, SEL, id fn, id file, long line, id fmt, const uint64_t* va) {
    log("*** Assertion failure in %s, %s:%ld: %s", utf8(fn).c_str(), utf8(file).c_str(), line, format(utf8(fmt), va).c_str());
}
APP_VARIADIC(app_th_assert_failure_fn, app_impl_assert_failure_fn, 6)

static void init_errors() {
    C_NSError = def_class("NSError", "NSObject");
    Class E = C_NSError;
    defc(E, "errorWithDomain:code:userInfo:", +[](id, SEL, id d, long c, id i) -> id {
        id e = alloc(C_NSError); auto* x = ext_get<ErrExt>(e); x->domain = retain(d); x->code = c; x->info = retain(i); return autorelease(e);
    });
    def(E, "initWithDomain:code:userInfo:", +[](id self, SEL, id d, long c, id i) -> id {
        auto* x = ext_get<ErrExt>(self); x->domain = retain(d); x->code = c; x->info = retain(i); return self;
    });
    def(E, "domain", +[](id self, SEL) -> id { return ext_get<ErrExt>(self)->domain; });
    def(E, "code", +[](id self, SEL) -> long { return ext_get<ErrExt>(self)->code; });
    def(E, "userInfo", +[](id self, SEL) -> id { return ext_get<ErrExt>(self)->info; });
    def(E, "localizedDescription", +[](id self, SEL) -> id {
        auto* x = ext_get<ErrExt>(self);
        id d = dict_get(x->info, "NSLocalizedDescription");
        if (d) return d;
        char b[256];
        snprintf(b, sizeof b, "The operation couldn't be completed. (%s error %ld.)", utf8(x->domain).c_str(), x->code);
        return str(b);
    });
    def(E, "localizedFailureReason", +[](id self, SEL) -> id { return dict_get(ext_get<ErrExt>(self)->info, "NSLocalizedFailureReason"); });
    def(E, "localizedRecoverySuggestion", +[](id, SEL) -> id { return nullptr; });
    def(E, "description", +[](id self, SEL) -> id {
        auto* x = ext_get<ErrExt>(self);
        return str("Error Domain=" + utf8(x->domain) + " Code=" + std::to_string(x->code));
    });

    C_NSException = def_class("NSException", "NSObject");
    Class X = C_NSException;
    defc(X, "exceptionWithName:reason:userInfo:", +[](id, SEL, id n, id r, id i) -> id {
        id e = alloc(C_NSException); auto* x = ext_get<ExcExt>(e); x->name = retain(n); x->reason = retain(r); x->info = retain(i); return autorelease(e);
    });
    defc(X, "raise:format:", &app_th_raise_format);
    def(X, "initWithName:reason:userInfo:", +[](id self, SEL, id n, id r, id i) -> id {
        auto* x = ext_get<ExcExt>(self); x->name = retain(n); x->reason = retain(r); x->info = retain(i); return self;
    });
    def(X, "name", +[](id self, SEL) -> id { return ext_get<ExcExt>(self)->name; });
    def(X, "reason", +[](id self, SEL) -> id { return ext_get<ExcExt>(self)->reason; });
    def(X, "userInfo", +[](id self, SEL) -> id { return ext_get<ExcExt>(self)->info; });
    def(X, "callStackReturnAddresses", +[](id, SEL) -> id { return arr({}); });
    def(X, "callStackSymbols", +[](id, SEL) -> id { return arr({}); });
    def(X, "raise", +[](id self, SEL) {
        auto* x = ext_get<ExcExt>(self);
        log("*** NSException raise (ignored): %s: %s", utf8(x->name).c_str(), utf8(x->reason).c_str());
        note_raise();
    });

    Class A = def_class("NSAssertionHandler", "NSObject");
    static id handler = alloc(A);
    defc(A, "currentHandler", +[](id, SEL) -> id { return handler; });
    def(A, "handleFailureInMethod:object:file:lineNumber:description:", &app_th_assert_failure);
    def(A, "handleFailureInFunction:file:lineNumber:description:", &app_th_assert_failure_fn);
}

// ---------------------------------------------------------------------------------------------------------------
// NSAutoreleasePool / NSThread / locks
// ---------------------------------------------------------------------------------------------------------------
struct PoolExt : Ext { void* token = nullptr; };
struct ThreadExt : Ext {
    id target = nullptr, arg = nullptr, name = nullptr, dict = nullptr, block = nullptr;
    SEL sel = nullptr;
    bool cancelled = false, executing = false, finished = false, is_main = false;
    ~ThreadExt() override { release(target); release(arg); release(name); release(dict); if (block) block_release(block); }
};
static Class C_NSThread;
static id g_main_nsthread;
static thread_local id t_current_thread;

static id current_thread() {
    if (on_main_thread()) return g_main_nsthread;
    if (!t_current_thread) t_current_thread = alloc(C_NSThread);
    return t_current_thread;
}

struct LockExt : Ext {
    std::recursive_timed_mutex m;
    std::condition_variable_any cv;
    long condition = 0;
    id name = nullptr;
    ~LockExt() override { release(name); }
};

static void init_threads() {
    Class P = def_class("NSAutoreleasePool", "NSObject");
    defc(P, "new", +[](id self, SEL) -> id { id p = alloc(reinterpret_cast<Class>(self)); ext_get<PoolExt>(p)->token = pool_push(); return p; });
    def(P, "init", +[](id self, SEL) -> id { ext_get<PoolExt>(self)->token = pool_push(); return self; });
    auto drain = +[](id self, SEL) {
        void* t = ext_get<PoolExt>(self)->token;
        pool_pop(t);
        dealloc_object(self);
    };
    def(P, "drain", drain);
    def(P, "release", drain);
    def(P, "retain", +[](id self, SEL) -> id { return self; });
    def(P, "autorelease", +[](id self, SEL) -> id { return self; });
    defc(P, "addObject:", +[](id, SEL, id o) { autorelease(o); });
    def(P, "addObject:", +[](id, SEL, id o) { autorelease(o); });

    C_NSThread = def_class("NSThread", "NSObject");
    Class T = C_NSThread;
    g_main_nsthread = alloc(T);
    ext_get<ThreadExt>(g_main_nsthread)->is_main = true;
    defc(T, "currentThread", +[](id, SEL) -> id { return current_thread(); });
    defc(T, "mainThread", +[](id, SEL) -> id { return g_main_nsthread; });
    defc(T, "isMainThread", +[](id, SEL) -> bool { return on_main_thread(); });
    defc(T, "isMultiThreaded", +[](id, SEL) -> bool { return true; });
    defc(T, "sleepForTimeInterval:", +[](id, SEL, double s) { if (s > 0) usleep(static_cast<useconds_t>(s * 1e6)); });
    defc(T, "sleepUntilDate:", +[](id, SEL, id d) { double s = date_t(d) - wall_ref_now(); if (s > 0) usleep(static_cast<useconds_t>(s * 1e6)); });
    defc(T, "setThreadPriority:", +[](id, SEL, double) -> bool { return true; });
    defc(T, "threadPriority", +[](id, SEL) -> double { return 0.5; });
    defc(T, "callStackReturnAddresses", +[](id, SEL) -> id { return arr({}); });
    defc(T, "callStackSymbols", +[](id, SEL) -> id { return arr({}); });
    defc(T, "exit", +[](id, SEL) { pthread_exit(nullptr); });
    defc(T, "detachNewThreadSelector:toTarget:withObject:", +[](id, SEL, SEL s, id target, id arg) {
        retain(target); retain(arg);
        spawn_thread([=] { send<void>(target, s, arg); release(target); release(arg); }, "NSThread");
    });
    defc(T, "detachNewThreadWithBlock:", +[](id, SEL, id block) {
        id b = block_copy(block);
        spawn_thread([b] { call_block0(b); block_release(b); }, "NSThread");
    });
    def(T, "initWithTarget:selector:object:", +[](id self, SEL, id target, SEL s, id arg) -> id {
        auto* e = ext_get<ThreadExt>(self); e->target = retain(target); e->sel = s; e->arg = retain(arg); return self;
    });
    def(T, "initWithBlock:", +[](id self, SEL, id block) -> id { ext_get<ThreadExt>(self)->block = block_copy(block); return self; });
    def(T, "start", +[](id self, SEL) {
        retain(self);
        auto* e = ext_get<ThreadExt>(self);
        e->executing = true;
        spawn_thread([self] {
            t_current_thread = self;
            send<void>(self, "main");
            auto* x = ext_get<ThreadExt>(self);
            x->executing = false;
            x->finished = true;
            release(self);
        }, "NSThread");
    });
    def(T, "main", +[](id self, SEL) {
        auto* e = ext_get<ThreadExt>(self);
        if (e->block) call_block0(e->block);
        else if (e->target) send<void>(e->target, e->sel, e->arg);
    });
    def(T, "cancel", +[](id self, SEL) { ext_get<ThreadExt>(self)->cancelled = true; });
    def(T, "isCancelled", +[](id self, SEL) -> bool { return ext_get<ThreadExt>(self)->cancelled; });
    def(T, "isExecuting", +[](id self, SEL) -> bool { return ext_get<ThreadExt>(self)->executing; });
    def(T, "isFinished", +[](id self, SEL) -> bool { return ext_get<ThreadExt>(self)->finished; });
    def(T, "isMainThread", +[](id self, SEL) -> bool { return ext_get<ThreadExt>(self)->is_main; });
    def(T, "name", +[](id self, SEL) -> id { return ext_get<ThreadExt>(self)->name; });
    def(T, "setName:", +[](id self, SEL, id n) { auto* e = ext_get<ThreadExt>(self); id o = e->name; e->name = send(n, "copy"); release(o); });
    def(T, "threadDictionary", +[](id self, SEL) -> id { auto* e = ext_get<ThreadExt>(self); if (!e->dict) e->dict = dict_new(true); return e->dict; });
    def(T, "setStackSize:", +[](id, SEL, NSUInteger) {});
    def(T, "stackSize", +[](id, SEL) -> NSUInteger { return 8u << 20; });
    def(T, "setThreadPriority:", +[](id, SEL, double) {});
    def(T, "setQualityOfService:", +[](id, SEL, long) {});

    for (const char* n : {"NSLock", "NSRecursiveLock", "NSCondition", "NSConditionLock"}) {
        Class L = def_class(n, "NSObject");
        declare_protocol(L, "NSLocking");
        def(L, "lock", +[](id self, SEL) { ext_get<LockExt>(self)->m.lock(); });
        def(L, "unlock", +[](id self, SEL) { ext_get<LockExt>(self)->m.unlock(); });
        def(L, "tryLock", +[](id self, SEL) -> bool { return ext_get<LockExt>(self)->m.try_lock(); });
        def(L, "lockBeforeDate:", +[](id self, SEL, id d) -> bool {
            double s = std::max(0.0, date_t(d) - wall_ref_now());
            return ext_get<LockExt>(self)->m.try_lock_for(std::chrono::microseconds(static_cast<int64_t>(std::min(s, 1e6) * 1e6)));
        });
        def(L, "wait", +[](id self, SEL) { auto* e = ext_get<LockExt>(self); e->cv.wait(e->m); });
        def(L, "waitUntilDate:", +[](id self, SEL, id d) -> bool {
            auto* e = ext_get<LockExt>(self);
            double s = std::max(0.0, date_t(d) - wall_ref_now());
            return e->cv.wait_for(e->m, std::chrono::microseconds(static_cast<int64_t>(std::min(s, 1e6) * 1e6))) == std::cv_status::no_timeout;
        });
        def(L, "signal", +[](id self, SEL) { ext_get<LockExt>(self)->cv.notify_one(); });
        def(L, "broadcast", +[](id self, SEL) { ext_get<LockExt>(self)->cv.notify_all(); });
        def(L, "name", +[](id self, SEL) -> id { return ext_get<LockExt>(self)->name; });
        def(L, "setName:", +[](id self, SEL, id n) { auto* e = ext_get<LockExt>(self); id o = e->name; e->name = retain(n); release(o); });
        def(L, "initWithCondition:", +[](id self, SEL, long c) -> id { ext_get<LockExt>(self)->condition = c; return self; });
        def(L, "condition", +[](id self, SEL) -> long { return ext_get<LockExt>(self)->condition; });
        def(L, "lockWhenCondition:", +[](id self, SEL, long c) {
            auto* e = ext_get<LockExt>(self);
            e->m.lock();
            while (e->condition != c) e->cv.wait(e->m);
        });
        def(L, "unlockWithCondition:", +[](id self, SEL, long c) {
            auto* e = ext_get<LockExt>(self);
            e->condition = c;
            e->m.unlock();
            e->cv.notify_all();
        });
    }
}

// ---------------------------------------------------------------------------------------------------------------
// NSFileManager / NSURL / NSFileHandle
// ---------------------------------------------------------------------------------------------------------------
static Class C_NSURL, C_NSFileManager;
struct UrlExt : Ext { std::string s; bool file = false; };
static UrlExt* ux(id o) { return ext_get<UrlExt>(o); }
static id url_make(const std::string& s, bool file) {
    id u = alloc(C_NSURL);
    auto* e = ux(u);
    e->file = file;
    e->s = s;
    return autorelease(u);
}
static std::string url_path(UrlExt* e) {
    if (e->file) return e->s;
    std::string s = e->s;
    size_t sch = s.find("://");
    if (sch == std::string::npos) {
        if (s.compare(0, 5, "file:") == 0) return s.substr(5);
        return s;
    }
    std::string rest = s.substr(sch + 3);
    if (s.compare(0, sch, "file") == 0) {
        size_t q = rest.find_first_of("?#");
        return q == std::string::npos ? rest : rest.substr(0, q);
    }
    size_t slash = rest.find('/');
    if (slash == std::string::npos) return "";
    std::string p = rest.substr(slash);
    size_t q = p.find_first_of("?#");
    return q == std::string::npos ? p : p.substr(0, q);
}
static std::string url_abs(UrlExt* e) { return e->file ? "file://" + e->s : e->s; }

static id file_attrs(const std::string& p) {
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return nullptr;
    id d = autorelease(dict_new());
    dict_set(d, "NSFileSize", num_u(static_cast<uint64_t>(st.st_size)));
    dict_set(d, "NSFileModificationDate", date(static_cast<double>(st.st_mtime) - kUnixToRef));
    dict_set(d, "NSFileCreationDate", date(static_cast<double>(st.st_ctime) - kUnixToRef));
    dict_set(d, "NSFileType", str(S_ISDIR(st.st_mode) ? "NSFileTypeDirectory" : "NSFileTypeRegular"));
    dict_set(d, "NSFilePosixPermissions", num_i(st.st_mode & 0777));
    return d;
}
static bool mkdirs(const std::string& p) {
    std::string cur;
    for (size_t i = 0; i <= p.size(); ++i) {
        if (i == p.size() || p[i] == '/') {
            if (!cur.empty()) mkdir(cur.c_str(), 0755);
        }
        if (i < p.size()) cur.push_back(p[i]);
    }
    return file_exists(p);
}
static bool rm_rf(const std::string& p) {
    bool dir = false;
    if (!file_exists(p, &dir)) return false;
    if (dir) {
        if (DIR* d = opendir(p.c_str())) {
            while (dirent* e = readdir(d)) {
                std::string n = e->d_name;
                if (n != "." && n != "..") rm_rf(p + "/" + n);
            }
            closedir(d);
        }
        return rmdir(p.c_str()) == 0;
    }
    return unlink(p.c_str()) == 0;
}
static bool copy_file(const std::string& a, const std::string& b) {
    std::string data;
    if (!read_file(a, data)) return false;
    FILE* f = fopen(b.c_str(), "wb");
    if (!f) return false;
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    return true;
}
static std::vector<std::string> list_dir(const std::string& p) {
    std::vector<std::string> out;
    if (DIR* d = opendir(p.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n != "." && n != "..") out.push_back(n);
        }
        closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}
static void list_recursive(const std::string& base, const std::string& rel, std::vector<id>& out) {
    for (auto& n : list_dir(base + (rel.empty() ? "" : "/" + rel))) {
        std::string r = rel.empty() ? n : rel + "/" + n;
        out.push_back(str(r));
        bool dir = false;
        file_exists(base + "/" + r, &dir);
        if (dir) list_recursive(base, r, out);
    }
}
static id dir_url(long dir) {
    const char* sub = nullptr;
    switch (dir) {
        case 9: sub = "Documents"; break;
        case 5: sub = "Library"; break;
        case 13: sub = "Library/Caches"; break;
        case 14: sub = "Library/Application Support"; break;
        case 15: sub = "Downloads"; break;
        default: return nullptr;
    }
    return url_make(sandbox_dir(sub), true);
}

static void init_files() {
    C_NSURL = def_class("NSURL", "NSObject");
    Class U = C_NSURL;
    declare_protocol(U, "NSCopying");
    defc(U, "fileURLWithPath:", +[](id, SEL, id p) -> id { return p ? url_make(utf8(p), true) : nullptr; });
    defc(U, "fileURLWithPath:isDirectory:", +[](id, SEL, id p, bool) -> id { return p ? url_make(utf8(p), true) : nullptr; });
    defc(U, "URLWithString:", +[](id, SEL, id s) -> id {
        if (!s) return nullptr;
        std::string v = utf8(s);
        if (v.compare(0, 7, "file://") == 0) return url_make(v.substr(7), true);
        return url_make(v, false);
    });
    defc(U, "URLWithString:relativeToURL:", +[](id, SEL, id s, id base) -> id {
        std::string v = utf8(s);
        if (v.find("://") != std::string::npos || !base) return url_make(v, false);
        std::string b = url_abs(ux(base));
        if (!v.empty() && v[0] == '/') {
            size_t sch = b.find("://");
            size_t slash = sch == std::string::npos ? std::string::npos : b.find('/', sch + 3);
            return url_make((slash == std::string::npos ? b : b.substr(0, slash)) + v, false);
        }
        size_t last = b.rfind('/');
        return url_make((last == std::string::npos ? b : b.substr(0, last + 1)) + v, false);
    });
    defc(U, "fileURLWithPathComponents:", +[](id, SEL, id comps) -> id {
        std::string p;
        for (id c : arr_items(comps)) { std::string s = utf8(c); p = p.empty() ? s : (p == "/" ? "/" + s : p + "/" + s); }
        return url_make(p, true);
    });
    def(U, "initFileURLWithPath:", +[](id self, SEL, id p) -> id { auto* e = ux(self); e->s = utf8(p); e->file = true; return self; });
    def(U, "initFileURLWithPath:isDirectory:", +[](id self, SEL, id p, bool) -> id { auto* e = ux(self); e->s = utf8(p); e->file = true; return self; });
    def(U, "initWithString:", +[](id self, SEL, id s) -> id { auto* e = ux(self); e->s = utf8(s); e->file = false; return self; });
    def(U, "path", +[](id self, SEL) -> id { return str(url_path(ux(self))); });
    def(U, "relativePath", +[](id self, SEL) -> id { return str(url_path(ux(self))); });
    def(U, "absoluteString", +[](id self, SEL) -> id { return str(url_abs(ux(self))); });
    def(U, "relativeString", +[](id self, SEL) -> id { return str(url_abs(ux(self))); });
    def(U, "description", +[](id self, SEL) -> id { return str(url_abs(ux(self))); });
    def(U, "absoluteURL", +[](id self, SEL) -> id { return self; });
    def(U, "standardizedURL", +[](id self, SEL) -> id { return self; });
    def(U, "URLByStandardizingPath", +[](id self, SEL) -> id { return self; });
    def(U, "URLByResolvingSymlinksInPath", +[](id self, SEL) -> id { return self; });
    def(U, "filePathURL", +[](id self, SEL) -> id { return self; });
    def(U, "isFileURL", +[](id self, SEL) -> bool { auto* e = ux(self); return e->file || e->s.compare(0, 5, "file:") == 0; });
    def(U, "fileSystemRepresentation", +[](id self, SEL) -> const char* { return cstr(str(url_path(ux(self)))); });
    def(U, "scheme", +[](id self, SEL) -> id {
        auto* e = ux(self); if (e->file) return str("file");
        size_t k = e->s.find(':'); return k == std::string::npos ? nullptr : str(e->s.substr(0, k));
    });
    def(U, "host", +[](id self, SEL) -> id {
        auto* e = ux(self); if (e->file) return nullptr;
        size_t k = e->s.find("://"); if (k == std::string::npos) return nullptr;
        std::string rest = e->s.substr(k + 3);
        size_t end = rest.find_first_of("/:?#");
        return str(rest.substr(0, end));
    });
    def(U, "port", +[](id self, SEL) -> id {
        auto* e = ux(self); size_t k = e->s.find("://"); if (k == std::string::npos) return nullptr;
        std::string rest = e->s.substr(k + 3);
        size_t c = rest.find(':'), sl = rest.find('/');
        if (c == std::string::npos || (sl != std::string::npos && c > sl)) return nullptr;
        return num_i(atoi(rest.c_str() + c + 1));
    });
    def(U, "query", +[](id self, SEL) -> id {
        auto* e = ux(self); size_t q = e->s.find('?'); if (q == std::string::npos) return nullptr;
        size_t h = e->s.find('#', q);
        return str(e->s.substr(q + 1, h == std::string::npos ? std::string::npos : h - q - 1));
    });
    def(U, "fragment", +[](id self, SEL) -> id { auto* e = ux(self); size_t h = e->s.find('#'); return h == std::string::npos ? nullptr : str(e->s.substr(h + 1)); });
    def(U, "resourceSpecifier", +[](id self, SEL) -> id { auto* e = ux(self); size_t k = e->s.find(':'); return str(k == std::string::npos ? e->s : e->s.substr(k + 1)); });
    def(U, "lastPathComponent", +[](id self, SEL) -> id { return send(str(url_path(ux(self))), "lastPathComponent"); });
    def(U, "pathExtension", +[](id self, SEL) -> id { return send(str(url_path(ux(self))), "pathExtension"); });
    def(U, "pathComponents", +[](id self, SEL) -> id { return send(str(url_path(ux(self))), "pathComponents"); });
    def(U, "URLByAppendingPathComponent:", +[](id self, SEL, id c) -> id {
        auto* e = ux(self); std::string s = e->s;
        while (!s.empty() && s.back() == '/') s.pop_back();
        return url_make(s + "/" + utf8(c), e->file);
    });
    def(U, "URLByAppendingPathComponent:isDirectory:", +[](id self, SEL, id c, bool) -> id {
        auto* e = ux(self); std::string s = e->s;
        while (!s.empty() && s.back() == '/') s.pop_back();
        return url_make(s + "/" + utf8(c), e->file);
    });
    def(U, "URLByAppendingPathExtension:", +[](id self, SEL, id x) -> id { auto* e = ux(self); return url_make(e->s + "." + utf8(x), e->file); });
    def(U, "URLByDeletingLastPathComponent", +[](id self, SEL) -> id {
        auto* e = ux(self); std::string s = e->s;
        while (s.size() > 1 && s.back() == '/') s.pop_back();
        size_t k = s.rfind('/');
        return url_make(k == std::string::npos ? s : s.substr(0, k + 1), e->file);
    });
    def(U, "URLByDeletingPathExtension", +[](id self, SEL) -> id { auto* e = ux(self); return url_make(utf8(send(str(e->s), "stringByDeletingPathExtension")), e->file); });
    def(U, "checkResourceIsReachableAndReturnError:", +[](id self, SEL, id* err) -> bool { if (err) *err = nullptr; return file_exists(url_path(ux(self))); });
    def(U, "setResourceValue:forKey:error:", +[](id, SEL, id, id, id* err) -> bool { if (err) *err = nullptr; return true; });
    def(U, "getResourceValue:forKey:error:", +[](id self, SEL, id* out, id key, id* err) -> bool {
        if (err) *err = nullptr;
        std::string k = utf8(key);
        bool dir = false;
        bool ex = file_exists(url_path(ux(self)), &dir);
        if (out) *out = k == "NSURLIsDirectoryKey" ? num_b(ex && dir) : nullptr;
        return true;
    });
    def(U, "isEqual:", +[](id self, SEL, id o) -> bool { return self == o || (o && is_kind(o, C_NSURL) && url_abs(ux(self)) == url_abs(ux(o))); });
    def(U, "hash", +[](id self, SEL) -> NSUInteger { return std::hash<std::string>()(url_abs(ux(self))); });
    def(U, "copyWithZone:", +[](id self, SEL, void*) -> id { return retain(self); });

    C_NSFileManager = def_class("NSFileManager", "NSObject");
    Class F = C_NSFileManager;
    static id fm = alloc(F);
    defc(F, "defaultManager", +[](id, SEL) -> id { return fm; });
    def(F, "fileExistsAtPath:", +[](id, SEL, id p) -> bool { return file_exists(remap_path(utf8(p))); });
    def(F, "fileExistsAtPath:isDirectory:", +[](id, SEL, id p, bool* dir) -> bool {
        bool d = false; bool e = file_exists(remap_path(utf8(p)), &d); if (dir) *dir = d; return e;
    });
    def(F, "isReadableFileAtPath:", +[](id, SEL, id p) -> bool { return access(remap_path(utf8(p)).c_str(), R_OK) == 0; });
    def(F, "isWritableFileAtPath:", +[](id, SEL, id p) -> bool { return access(remap_path(utf8(p)).c_str(), W_OK) == 0; });
    def(F, "isDeletableFileAtPath:", +[](id, SEL, id p) -> bool { return file_exists(remap_path(utf8(p))); });
    def(F, "isExecutableFileAtPath:", +[](id, SEL, id p) -> bool { return access(remap_path(utf8(p)).c_str(), X_OK) == 0; });
    def(F, "createDirectoryAtPath:withIntermediateDirectories:attributes:error:", +[](id, SEL, id p, bool inter, id, id* err) -> bool {
        if (err) *err = nullptr;
        std::string path = remap_path(utf8(p));
        if (inter) return mkdirs(path);
        return mkdir(path.c_str(), 0755) == 0 || file_exists(path);
    });
    def(F, "createDirectoryAtPath:attributes:", +[](id, SEL, id p, id) -> bool { return mkdirs(remap_path(utf8(p))); });
    def(F, "createDirectoryAtURL:withIntermediateDirectories:attributes:error:", +[](id, SEL, id u, bool, id, id* err) -> bool {
        if (err) *err = nullptr; return mkdirs(url_path(ux(u)));
    });
    def(F, "createFileAtPath:contents:attributes:", +[](id, SEL, id p, id data, id) -> bool {
        FILE* f = fopen(remap_path(utf8(p)).c_str(), "wb");
        if (!f) return false;
        if (data) fwrite(data_bytes(data), 1, data_len(data), f);
        fclose(f);
        return true;
    });
    def(F, "contentsAtPath:", +[](id, SEL, id p) -> id {
        std::string s; if (!read_file(remap_path(utf8(p)), s)) return nullptr; return autorelease(data_new(s.data(), s.size()));
    });
    def(F, "removeItemAtPath:error:", +[](id, SEL, id p, id* err) -> bool { if (err) *err = nullptr; return rm_rf(remap_path(utf8(p))); });
    def(F, "removeItemAtURL:error:", +[](id, SEL, id u, id* err) -> bool { if (err) *err = nullptr; return rm_rf(url_path(ux(u))); });
    def(F, "copyItemAtPath:toPath:error:", +[](id, SEL, id a, id b, id* err) -> bool { if (err) *err = nullptr; return copy_file(remap_path(utf8(a)), remap_path(utf8(b))); });
    def(F, "copyItemAtURL:toURL:error:", +[](id, SEL, id a, id b, id* err) -> bool { if (err) *err = nullptr; return copy_file(url_path(ux(a)), url_path(ux(b))); });
    def(F, "moveItemAtPath:toPath:error:", +[](id, SEL, id a, id b, id* err) -> bool { if (err) *err = nullptr; return rename(remap_path(utf8(a)).c_str(), remap_path(utf8(b)).c_str()) == 0; });
    def(F, "moveItemAtURL:toURL:error:", +[](id, SEL, id a, id b, id* err) -> bool { if (err) *err = nullptr; return rename(url_path(ux(a)).c_str(), url_path(ux(b)).c_str()) == 0; });
    def(F, "contentsOfDirectoryAtPath:error:", +[](id, SEL, id p, id* err) -> id {
        if (err) *err = nullptr;
        std::vector<id> v; for (auto& n : list_dir(remap_path(utf8(p)))) v.push_back(str(n)); return arr(v);
    });
    def(F, "directoryContentsAtPath:", +[](id, SEL, id p) -> id { std::vector<id> v; for (auto& n : list_dir(remap_path(utf8(p)))) v.push_back(str(n)); return arr(v); });
    def(F, "contentsOfDirectoryAtURL:includingPropertiesForKeys:options:error:", +[](id, SEL, id u, id, NSUInteger, id* err) -> id {
        if (err) *err = nullptr;
        std::string base = url_path(ux(u));
        std::vector<id> v; for (auto& n : list_dir(base)) v.push_back(url_make(base + "/" + n, true)); return arr(v);
    });
    def(F, "subpathsOfDirectoryAtPath:error:", +[](id, SEL, id p, id* err) -> id { if (err) *err = nullptr; std::vector<id> v; list_recursive(remap_path(utf8(p)), "", v); return arr(v); });
    def(F, "subpathsAtPath:", +[](id, SEL, id p) -> id { std::vector<id> v; list_recursive(remap_path(utf8(p)), "", v); return arr(v); });
    def(F, "enumeratorAtPath:", +[](id, SEL, id p) -> id { std::vector<id> v; list_recursive(remap_path(utf8(p)), "", v); return send(arr(v), "objectEnumerator"); });
    def(F, "attributesOfItemAtPath:error:", +[](id, SEL, id p, id* err) -> id { if (err) *err = nullptr; return file_attrs(remap_path(utf8(p))); });
    def(F, "fileAttributesAtPath:traverseLink:", +[](id, SEL, id p, bool) -> id { return file_attrs(remap_path(utf8(p))); });
    def(F, "setAttributes:ofItemAtPath:error:", +[](id, SEL, id, id, id* err) -> bool { if (err) *err = nullptr; return true; });
    def(F, "attributesOfFileSystemForPath:error:", +[](id, SEL, id p, id* err) -> id {
        if (err) *err = nullptr;
        struct statfs s;
        id d = autorelease(dict_new());
        if (statfs(remap_path(utf8(p)).c_str(), &s) == 0) {
            dict_set(d, "NSFileSystemSize", num_u(static_cast<uint64_t>(s.f_blocks) * s.f_bsize));
            dict_set(d, "NSFileSystemFreeSize", num_u(static_cast<uint64_t>(s.f_bavail) * s.f_bsize));
        }
        return d;
    });
    def(F, "URLsForDirectory:inDomains:", +[](id, SEL, long dir, NSUInteger) -> id { id u = dir_url(dir); return u ? arr({u}) : arr({}); });
    def(F, "URLForDirectory:inDomain:appropriateForURL:create:error:", +[](id, SEL, long dir, NSUInteger, id, bool, id* err) -> id { if (err) *err = nullptr; return dir_url(dir); });
    def(F, "URLForUbiquityContainerIdentifier:", +[](id, SEL, id) -> id { return nullptr; });
    def(F, "ubiquityIdentityToken", +[](id, SEL) -> id { return nullptr; });
    def(F, "isUbiquitousItemAtURL:", +[](id, SEL, id) -> bool { return false; });
    def(F, "setUbiquitous:itemAtURL:destinationURL:error:", +[](id, SEL, bool, id, id, id* err) -> bool { if (err) *err = nullptr; return false; });
    def(F, "currentDirectoryPath", +[](id, SEL) -> id { return str(bundle_dir()); });
    def(F, "changeCurrentDirectoryPath:", +[](id, SEL, id) -> bool { return true; });
    def(F, "fileSystemRepresentationWithPath:", +[](id, SEL, id p) -> const char* { return cstr(p); });
    def(F, "stringWithFileSystemRepresentation:length:", +[](id, SEL, const char* p, NSUInteger n) -> id { return str(std::string(p, n)); });
    def(F, "setDelegate:", +[](id, SEL, id) {});

    Class H = def_class("NSFileHandle", "NSObject");
    struct FhExt : Ext { FILE* f = nullptr; ~FhExt() override { if (f) fclose(f); } };
    auto open_fh = [](const std::string& p, const char* mode) -> id {
        FILE* f = fopen(remap_path(p).c_str(), mode);
        if (!f) return nullptr;
        id h = alloc(class_named("NSFileHandle"));
        ext_get<FhExt>(h)->f = f;
        return autorelease(h);
    };
    static decltype(open_fh) s_open = open_fh;
    defc(H, "fileHandleForReadingAtPath:", +[](id, SEL, id p) -> id { return s_open(utf8(p), "rb"); });
    defc(H, "fileHandleForWritingAtPath:", +[](id, SEL, id p) -> id { return file_exists(remap_path(utf8(p))) ? s_open(utf8(p), "r+b") : nullptr; });
    defc(H, "fileHandleForUpdatingAtPath:", +[](id, SEL, id p) -> id { return file_exists(remap_path(utf8(p))) ? s_open(utf8(p), "r+b") : nullptr; });
    defc(H, "fileHandleForReadingFromURL:error:", +[](id, SEL, id u, id* err) -> id { if (err) *err = nullptr; return s_open(url_path(ux(u)), "rb"); });
    defc(H, "fileHandleForWritingToURL:error:", +[](id, SEL, id u, id* err) -> id { if (err) *err = nullptr; return s_open(url_path(ux(u)), "r+b"); });
    def(H, "readDataToEndOfFile", +[](id self, SEL) -> id {
        FILE* f = ext_get<FhExt>(self)->f; std::string s; char buf[65536]; size_t n;
        while (f && (n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
        return autorelease(data_new(s.data(), s.size()));
    });
    def(H, "readDataOfLength:", +[](id self, SEL, NSUInteger len) -> id {
        FILE* f = ext_get<FhExt>(self)->f; std::string s(len, '\0');
        size_t n = f ? fread(&s[0], 1, len, f) : 0; s.resize(n);
        return autorelease(data_new(s.data(), s.size()));
    });
    def(H, "writeData:", +[](id self, SEL, id d) { if (FILE* f = ext_get<FhExt>(self)->f) fwrite(data_bytes(d), 1, data_len(d), f); });
    def(H, "seekToEndOfFile", +[](id self, SEL) -> unsigned long long { FILE* f = ext_get<FhExt>(self)->f; if (!f) return 0; fseek(f, 0, SEEK_END); return static_cast<unsigned long long>(ftell(f)); });
    def(H, "seekToFileOffset:", +[](id self, SEL, unsigned long long o) { if (FILE* f = ext_get<FhExt>(self)->f) fseek(f, static_cast<long>(o), SEEK_SET); });
    def(H, "offsetInFile", +[](id self, SEL) -> unsigned long long { FILE* f = ext_get<FhExt>(self)->f; return f ? static_cast<unsigned long long>(ftell(f)) : 0; });
    def(H, "truncateFileAtOffset:", +[](id self, SEL, unsigned long long o) { if (FILE* f = ext_get<FhExt>(self)->f) { fflush(f); ftruncate(fileno(f), static_cast<off_t>(o)); fseek(f, static_cast<long>(o), SEEK_SET); } });
    def(H, "synchronizeFile", +[](id self, SEL) { if (FILE* f = ext_get<FhExt>(self)->f) fflush(f); });
    def(H, "closeFile", +[](id self, SEL) { auto* e = ext_get<FhExt>(self); if (e->f) { fclose(e->f); e->f = nullptr; } });
}

// ---------------------------------------------------------------------------------------------------------------
// NSProcessInfo / NSLocale / NSTimeZone / NSCalendar / NSDateComponents / NSDateFormatter
// ---------------------------------------------------------------------------------------------------------------
struct OSVersion { long major, minor, patch; };
struct CompExt : Ext { std::map<std::string, long> v; };
struct FmtExt : Ext { std::string fmt = "yyyy-MM-dd HH:mm:ss"; bool utc = false; long date_style = 0, time_style = 0; };
static Class C_NSDateComponents;

static void tm_of(double ref, bool utc, struct tm& out) {
    time_t t = static_cast<time_t>(std::floor(ref + kUnixToRef));
    if (utc) gmtime_r(&t, &out);
    else localtime_r(&t, &out);
}
static std::string format_date(const std::string& f, double ref, bool utc) {
    struct tm t;
    tm_of(ref, utc, t);
    static const char* months[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
    static const char* days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    std::string out;
    for (size_t i = 0; i < f.size();) {
        char c = f[i];
        if (c == '\'') {
            size_t e = f.find('\'', i + 1);
            out += f.substr(i + 1, (e == std::string::npos ? f.size() : e) - i - 1);
            i = e == std::string::npos ? f.size() : e + 1;
            continue;
        }
        if (!isalpha(static_cast<unsigned char>(c))) { out.push_back(c); ++i; continue; }
        size_t n = 1;
        while (i + n < f.size() && f[i + n] == c) ++n;
        char b[64];
        switch (c) {
            case 'y': snprintf(b, sizeof b, n == 2 ? "%02d" : "%04d", n == 2 ? (t.tm_year + 1900) % 100 : t.tm_year + 1900); out += b; break;
            case 'M':
                if (n >= 4) out += months[t.tm_mon];
                else if (n == 3) out += std::string(months[t.tm_mon]).substr(0, 3);
                else { snprintf(b, sizeof b, n == 2 ? "%02d" : "%d", t.tm_mon + 1); out += b; }
                break;
            case 'd': snprintf(b, sizeof b, n == 2 ? "%02d" : "%d", t.tm_mday); out += b; break;
            case 'D': snprintf(b, sizeof b, "%d", t.tm_yday + 1); out += b; break;
            case 'H': snprintf(b, sizeof b, n == 2 ? "%02d" : "%d", t.tm_hour); out += b; break;
            case 'h': snprintf(b, sizeof b, n == 2 ? "%02d" : "%d", t.tm_hour % 12 ? t.tm_hour % 12 : 12); out += b; break;
            case 'm': snprintf(b, sizeof b, n == 2 ? "%02d" : "%d", t.tm_min); out += b; break;
            case 's': snprintf(b, sizeof b, n == 2 ? "%02d" : "%d", t.tm_sec); out += b; break;
            case 'S': { double frac = ref - std::floor(ref); snprintf(b, sizeof b, "%0*d", static_cast<int>(n), static_cast<int>(frac * std::pow(10, n))); out += b; break; }
            case 'a': out += t.tm_hour < 12 ? "AM" : "PM"; break;
            case 'E': out += n >= 4 ? days[t.tm_wday] : std::string(days[t.tm_wday]).substr(0, 3); break;
            case 'Z': case 'z': case 'x': case 'X': {
                long off = utc ? 0 : t.tm_gmtoff;
                snprintf(b, sizeof b, "%c%02ld%s%02ld", off < 0 ? '-' : '+', std::labs(off) / 3600, c == 'x' || c == 'X' || n >= 5 ? ":" : "", (std::labs(off) % 3600) / 60);
                out += b;
                break;
            }
            default: out.append(n, c); break;
        }
        i += n;
    }
    return out;
}
static bool parse_date(const std::string& f, const std::string& s, bool utc, double& ref) {
    struct tm t{};
    t.tm_mday = 1;
    size_t j = 0;
    long tz_off = 0;
    bool has_tz = false;
    for (size_t i = 0; i < f.size();) {
        char c = f[i];
        if (c == '\'') { size_t e = f.find('\'', i + 1); size_t len = (e == std::string::npos ? f.size() : e) - i - 1; j += len; i = e == std::string::npos ? f.size() : e + 1; continue; }
        if (!isalpha(static_cast<unsigned char>(c))) { if (j < s.size() && s[j] == c) ++j; ++i; continue; }
        size_t n = 1;
        while (i + n < f.size() && f[i + n] == c) ++n;
        i += n;
        if (c == 'Z' || c == 'z' || c == 'x' || c == 'X') {
            if (j < s.size() && (s[j] == 'Z')) { ++j; has_tz = true; continue; }
            if (j < s.size() && (s[j] == '+' || s[j] == '-')) {
                int sign = s[j] == '-' ? -1 : 1; ++j;
                int hh = 0, mm = 0;
                for (int k = 0; k < 2 && j < s.size() && isdigit(static_cast<unsigned char>(s[j])); ++k) hh = hh * 10 + (s[j++] - '0');
                if (j < s.size() && s[j] == ':') ++j;
                for (int k = 0; k < 2 && j < s.size() && isdigit(static_cast<unsigned char>(s[j])); ++k) mm = mm * 10 + (s[j++] - '0');
                tz_off = sign * (hh * 3600 + mm * 60);
                has_tz = true;
            }
            continue;
        }
        if (c == 'a') { if (j + 1 < s.size()) { if (toupper(s[j]) == 'P' && t.tm_hour < 12) t.tm_hour += 12; j += 2; } continue; }
        if (c == 'E' || (c == 'M' && n >= 3)) {
            size_t st = j;
            while (j < s.size() && isalpha(static_cast<unsigned char>(s[j]))) ++j;
            if (c == 'M') {
                static const char* m3[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
                std::string w = s.substr(st, 3);
                for (auto& ch : w) ch = static_cast<char>(tolower(ch));
                for (int k = 0; k < 12; ++k) if (w == m3[k]) t.tm_mon = k;
            }
            continue;
        }
        long v = 0;
        size_t st = j;
        size_t maxd = (c == 'y' && n != 2) ? 4 : (n == 1 ? 2 : n);
        while (j < s.size() && j - st < maxd && isdigit(static_cast<unsigned char>(s[j]))) v = v * 10 + (s[j++] - '0');
        if (j == st) return false;
        switch (c) {
            case 'y': t.tm_year = static_cast<int>((n == 2 ? 2000 + v : v) - 1900); break;
            case 'M': t.tm_mon = static_cast<int>(v - 1); break;
            case 'd': t.tm_mday = static_cast<int>(v); break;
            case 'H': case 'h': t.tm_hour = static_cast<int>(v); break;
            case 'm': t.tm_min = static_cast<int>(v); break;
            case 's': t.tm_sec = static_cast<int>(v); break;
            default: break;
        }
    }
    time_t tt;
    if (utc || has_tz) { tt = timegm(&t); tt -= tz_off; }
    else { t.tm_isdst = -1; tt = mktime(&t); }
    ref = static_cast<double>(tt) - kUnixToRef;
    return true;
}

static void init_locale() {
    Class P = def_class("NSProcessInfo", "NSObject");
    static id pi = alloc(P);
    defc(P, "processInfo", +[](id, SEL) -> id { return pi; });
    def(P, "processName", +[](id, SEL) -> id { id e = dict_get(info_dictionary(), "CFBundleExecutable"); return e ? e : str("app"); });
    def(P, "processIdentifier", +[](id, SEL) -> int { return getpid(); });
    def(P, "environment", +[](id, SEL) -> id { return autorelease(dict_new(false)); });
    def(P, "arguments", +[](id, SEL) -> id { return arr({send(pi, "processName")}); });
    def(P, "hostName", +[](id, SEL) -> id { return str("localhost"); });
    def(P, "globallyUniqueString", +[](id, SEL) -> id {
        char b[64]; snprintf(b, sizeof b, "%08X-%04X-%04X-%04X-%012llX", arc4random(), arc4random() & 0xffff, arc4random() & 0xffff, arc4random() & 0xffff,
                             static_cast<unsigned long long>(arc4random()) << 16 | (arc4random() & 0xffff));
        return str(b);
    });
    def(P, "operatingSystemVersionString", +[](id, SEL) -> id { return str("Version 8.4 (Build 12H143)"); });
    def(P, "operatingSystemVersion", +[](id, SEL) -> OSVersion { return {8, 4, 0}; });
    def(P, "isOperatingSystemAtLeastVersion:", +[](id, SEL, OSVersion v) -> bool {
        OSVersion me{8, 4, 0};
        if (me.major != v.major) return me.major > v.major;
        if (me.minor != v.minor) return me.minor > v.minor;
        return me.patch >= v.patch;
    });
    def(P, "systemUptime", +[](id, SEL) -> double { return now_s(); });
    def(P, "physicalMemory", +[](id, SEL) -> unsigned long long { return 2ull << 30; });
    def(P, "processorCount", +[](id, SEL) -> NSUInteger { return static_cast<NSUInteger>(sysconf(_SC_NPROCESSORS_CONF)); });
    def(P, "activeProcessorCount", +[](id, SEL) -> NSUInteger { return static_cast<NSUInteger>(sysconf(_SC_NPROCESSORS_ONLN)); });
    def(P, "thermalState", +[](id, SEL) -> long { return 0; });
    def(P, "isLowPowerModeEnabled", +[](id, SEL) -> bool { return false; });
    def(P, "beginActivityWithOptions:reason:", +[](id, SEL, unsigned long long, id) -> id { return autorelease(alloc(class_named("NSObject"))); });
    def(P, "endActivity:", +[](id, SEL, id) {});
    def(P, "disableSuddenTermination", +[](id, SEL) {});
    def(P, "enableSuddenTermination", +[](id, SEL) {});

    Class L = def_class("NSLocale", "NSObject");
    static id cur = alloc(L);
    defc(L, "currentLocale", +[](id, SEL) -> id { return cur; });
    defc(L, "systemLocale", +[](id, SEL) -> id { return cur; });
    defc(L, "autoupdatingCurrentLocale", +[](id, SEL) -> id { return cur; });
    defc(L, "localeWithLocaleIdentifier:", +[](id, SEL, id) -> id { return cur; });
    defc(L, "preferredLanguages", +[](id, SEL) -> id { return arr({str("en")}); });
    defc(L, "availableLocaleIdentifiers", +[](id, SEL) -> id { return arr({str("en_US")}); });
    defc(L, "ISOCountryCodes", +[](id, SEL) -> id { return arr({str("US")}); });
    defc(L, "ISOLanguageCodes", +[](id, SEL) -> id { return arr({str("en")}); });
    defc(L, "canonicalLocaleIdentifierFromString:", +[](id, SEL, id s) -> id { return s; });
    defc(L, "componentsFromLocaleIdentifier:", +[](id, SEL, id) -> id {
        id d = autorelease(dict_new()); dict_set(d, "kCFLocaleLanguageCodeKey", str("en")); dict_set(d, "kCFLocaleCountryCodeKey", str("US")); return d;
    });
    def(L, "initWithLocaleIdentifier:", +[](id self, SEL, id) -> id { release(self); return retain(cur); });
    def(L, "localeIdentifier", +[](id, SEL) -> id { return str("en_US"); });
    def(L, "countryCode", +[](id, SEL) -> id { return str("US"); });
    def(L, "languageCode", +[](id, SEL) -> id { return str("en"); });
    def(L, "currencyCode", +[](id, SEL) -> id { return str("USD"); });
    def(L, "currencySymbol", +[](id, SEL) -> id { return str("$"); });
    def(L, "objectForKey:", +[](id, SEL, id k) -> id {
        std::string s = utf8(k);
        if (s.find("CountryCode") != std::string::npos) return str("US");
        if (s.find("LanguageCode") != std::string::npos) return str("en");
        if (s.find("CurrencyCode") != std::string::npos) return str("USD");
        if (s.find("CurrencySymbol") != std::string::npos) return str("$");
        if (s.find("Identifier") != std::string::npos) return str("en_US");
        if (s.find("DecimalSeparator") != std::string::npos) return str(".");
        if (s.find("GroupingSeparator") != std::string::npos) return str(",");
        if (s.find("UsesMetricSystem") != std::string::npos) return num_b(false);
        return nullptr;
    });
    def(L, "displayNameForKey:value:", +[](id, SEL, id, id v) -> id { return v; });
    def(L, "copyWithZone:", +[](id self, SEL, void*) -> id { return self; });

    Class Z = def_class("NSTimeZone", "NSObject");
    struct TzExt : Ext { long off = 0; bool local = true; std::string name = "UTC"; };
    auto local_tz = []() -> id {
        static id z = nullptr;
        if (!z) {
            z = alloc(class_named("NSTimeZone"));
            auto* e = ext_get<TzExt>(z);
            time_t t = time(nullptr);
            struct tm lt;
            localtime_r(&t, &lt);
            e->off = lt.tm_gmtoff;
            e->name = lt.tm_zone ? lt.tm_zone : "GMT";
        }
        return z;
    };
    static decltype(local_tz) s_local = local_tz;
    defc(Z, "localTimeZone", +[](id, SEL) -> id { return s_local(); });
    defc(Z, "systemTimeZone", +[](id, SEL) -> id { return s_local(); });
    defc(Z, "defaultTimeZone", +[](id, SEL) -> id { return s_local(); });
    defc(Z, "setDefaultTimeZone:", +[](id, SEL, id) {});
    defc(Z, "resetSystemTimeZone", +[](id, SEL) {});
    auto with_offset = +[](id, SEL, long off) -> id {
        id z = alloc(class_named("NSTimeZone"));
        auto* e = ext_get<TzExt>(z); e->off = off; e->local = false; e->name = off == 0 ? "GMT" : "GMT" + std::to_string(off / 3600);
        return autorelease(z);
    };
    defc(Z, "timeZoneForSecondsFromGMT:", with_offset);
    defc(Z, "timeZoneWithName:", +[](id self, SEL, id n) -> id {
        std::string s = utf8(n);
        if (s == "UTC" || s == "GMT" || s == "Etc/UTC") return reinterpret_cast<id (*)(id, SEL, long)>(lookup(self, sel("timeZoneForSecondsFromGMT:")))(self, nullptr, 0);
        return s_local();
    });
    defc(Z, "timeZoneWithAbbreviation:", +[](id self, SEL, id) -> id { return reinterpret_cast<id (*)(id, SEL, long)>(lookup(self, sel("timeZoneForSecondsFromGMT:")))(self, nullptr, 0); });
    def(Z, "secondsFromGMT", +[](id self, SEL) -> long { return ext_get<TzExt>(self)->off; });
    def(Z, "secondsFromGMTForDate:", +[](id self, SEL, id) -> long { return ext_get<TzExt>(self)->off; });
    def(Z, "name", +[](id self, SEL) -> id { return str(ext_get<TzExt>(self)->name); });
    def(Z, "abbreviation", +[](id self, SEL) -> id { return str(ext_get<TzExt>(self)->name); });
    def(Z, "isDaylightSavingTime", +[](id, SEL) -> bool { return false; });
    def(Z, "isDaylightSavingTimeForDate:", +[](id, SEL, id) -> bool { return false; });
    def(Z, "description", +[](id self, SEL) -> id { return str(ext_get<TzExt>(self)->name); });
    def(Z, "copyWithZone:", +[](id self, SEL, void*) -> id { return retain(self); });

    C_NSDateComponents = def_class("NSDateComponents", "NSObject");
    Class DC = C_NSDateComponents;
    for (const char* f : {"era", "year", "month", "day", "hour", "minute", "second", "nanosecond", "weekday", "weekdayOrdinal", "quarter", "week",
                          "weekOfMonth", "weekOfYear", "yearForWeekOfYear"}) {
        (void)f;
    }
#define DC_FIELD(get, setsel, key)                                                                                     \
    def(DC, get, +[](id self, SEL) -> long { auto& m = ext_get<CompExt>(self)->v; auto it = m.find(key); return it == m.end() ? NSNotFound : it->second; }); \
    def(DC, setsel, +[](id self, SEL, long v) { ext_get<CompExt>(self)->v[key] = v; });
    DC_FIELD("era", "setEra:", "era")
    DC_FIELD("year", "setYear:", "year")
    DC_FIELD("month", "setMonth:", "month")
    DC_FIELD("day", "setDay:", "day")
    DC_FIELD("hour", "setHour:", "hour")
    DC_FIELD("minute", "setMinute:", "minute")
    DC_FIELD("second", "setSecond:", "second")
    DC_FIELD("nanosecond", "setNanosecond:", "nanosecond")
    DC_FIELD("weekday", "setWeekday:", "weekday")
    DC_FIELD("weekdayOrdinal", "setWeekdayOrdinal:", "weekdayOrdinal")
    DC_FIELD("week", "setWeek:", "week")
    DC_FIELD("weekOfYear", "setWeekOfYear:", "weekOfYear")
    DC_FIELD("weekOfMonth", "setWeekOfMonth:", "weekOfMonth")
#undef DC_FIELD

    Class CAL = def_class("NSCalendar", "NSObject");
    static id cal = alloc(CAL);
    defc(CAL, "currentCalendar", +[](id, SEL) -> id { return cal; });
    defc(CAL, "autoupdatingCurrentCalendar", +[](id, SEL) -> id { return cal; });
    defc(CAL, "calendarWithIdentifier:", +[](id, SEL, id) -> id { return cal; });
    def(CAL, "initWithCalendarIdentifier:", +[](id self, SEL, id) -> id { release(self); return retain(cal); });
    def(CAL, "calendarIdentifier", +[](id, SEL) -> id { return str("gregorian"); });
    def(CAL, "setTimeZone:", +[](id, SEL, id) {});
    def(CAL, "setLocale:", +[](id, SEL, id) {});
    def(CAL, "setFirstWeekday:", +[](id, SEL, NSUInteger) {});
    def(CAL, "firstWeekday", +[](id, SEL) -> NSUInteger { return 1; });
    def(CAL, "components:fromDate:", +[](id, SEL, NSUInteger, id d) -> id {
        struct tm t;
        tm_of(date_t(d), false, t);
        id c = autorelease(alloc(C_NSDateComponents));
        auto& m = ext_get<CompExt>(c)->v;
        m["era"] = 1; m["year"] = t.tm_year + 1900; m["month"] = t.tm_mon + 1; m["day"] = t.tm_mday;
        m["hour"] = t.tm_hour; m["minute"] = t.tm_min; m["second"] = t.tm_sec; m["weekday"] = t.tm_wday + 1;
        m["weekOfYear"] = t.tm_yday / 7 + 1; m["week"] = m["weekOfYear"]; m["weekdayOrdinal"] = (t.tm_mday - 1) / 7 + 1;
        return c;
    });
    def(CAL, "component:fromDate:", +[](id, SEL, NSUInteger unit, id d) -> long {
        struct tm t;
        tm_of(date_t(d), false, t);
        switch (unit) {
            case 4: return t.tm_year + 1900;
            case 8: return t.tm_mon + 1;
            case 16: return t.tm_mday;
            case 32: return t.tm_hour;
            case 64: return t.tm_min;
            case 128: return t.tm_sec;
            case 512: return t.tm_wday + 1;
            default: return 0;
        }
    });
    def(CAL, "dateFromComponents:", +[](id, SEL, id c) -> id {
        auto& m = ext_get<CompExt>(c)->v;
        auto g = [&](const char* k, long dflt) { auto it = m.find(k); return it == m.end() || it->second == static_cast<long>(NSNotFound) ? dflt : it->second; };
        struct tm t{};
        t.tm_year = static_cast<int>(g("year", 2001) - 1900); t.tm_mon = static_cast<int>(g("month", 1) - 1); t.tm_mday = static_cast<int>(g("day", 1));
        t.tm_hour = static_cast<int>(g("hour", 0)); t.tm_min = static_cast<int>(g("minute", 0)); t.tm_sec = static_cast<int>(g("second", 0)); t.tm_isdst = -1;
        return date(static_cast<double>(mktime(&t)) - kUnixToRef);
    });
    def(CAL, "components:fromDate:toDate:options:", +[](id, SEL, NSUInteger units, id a, id b, NSUInteger) -> id {
        double d = date_t(b) - date_t(a);
        id c = autorelease(alloc(C_NSDateComponents));
        auto& m = ext_get<CompExt>(c)->v;
        long rem = static_cast<long>(d);
        if (units & 4) { struct tm ta, tb; tm_of(date_t(a), false, ta); tm_of(date_t(b), false, tb); m["year"] = tb.tm_year - ta.tm_year; }
        if (units & 8) { struct tm ta, tb; tm_of(date_t(a), false, ta); tm_of(date_t(b), false, tb); m["month"] = (tb.tm_year - ta.tm_year) * 12 + tb.tm_mon - ta.tm_mon; }
        if (units & 16) { m["day"] = rem / 86400; rem %= 86400; }
        if (units & 32) { m["hour"] = rem / 3600; rem %= 3600; }
        if (units & 64) { m["minute"] = rem / 60; rem %= 60; }
        if (units & 128) m["second"] = rem;
        return c;
    });
    def(CAL, "dateByAddingComponents:toDate:options:", +[](id, SEL, id c, id d, NSUInteger) -> id {
        auto& m = ext_get<CompExt>(c)->v;
        struct tm t;
        tm_of(date_t(d), false, t);
        auto g = [&](const char* k) { auto it = m.find(k); return it == m.end() || it->second == static_cast<long>(NSNotFound) ? 0L : it->second; };
        t.tm_year += static_cast<int>(g("year")); t.tm_mon += static_cast<int>(g("month")); t.tm_mday += static_cast<int>(g("day"));
        t.tm_hour += static_cast<int>(g("hour")); t.tm_min += static_cast<int>(g("minute")); t.tm_sec += static_cast<int>(g("second")); t.tm_isdst = -1;
        return date(static_cast<double>(mktime(&t)) - kUnixToRef);
    });
    def(CAL, "startOfDayForDate:", +[](id, SEL, id d) -> id {
        struct tm t; tm_of(date_t(d), false, t); t.tm_hour = t.tm_min = t.tm_sec = 0; t.tm_isdst = -1;
        return date(static_cast<double>(mktime(&t)) - kUnixToRef);
    });

    Class DF = def_class("NSDateFormatter", "NSObject");
    def_class("NSFormatter", "NSObject");
    def(DF, "setDateFormat:", +[](id self, SEL, id f) { ext_get<FmtExt>(self)->fmt = utf8(f); });
    def(DF, "dateFormat", +[](id self, SEL) -> id { return str(ext_get<FmtExt>(self)->fmt); });
    def(DF, "setTimeZone:", +[](id self, SEL, id z) { ext_get<FmtExt>(self)->utc = z && send<long>(z, "secondsFromGMT") == 0 && utf8(send(z, "name")) != utf8(send(send_cls(class_named("NSTimeZone"), "localTimeZone"), "name")); });
    def(DF, "setLocale:", +[](id, SEL, id) {});
    def(DF, "setCalendar:", +[](id, SEL, id) {});
    def(DF, "setFormatterBehavior:", +[](id, SEL, NSUInteger) {});
    def(DF, "setLenient:", +[](id, SEL, bool) {});
    def(DF, "setDoesRelativeDateFormatting:", +[](id, SEL, bool) {});
    def(DF, "setDateStyle:", +[](id self, SEL, long s) {
        auto* e = ext_get<FmtExt>(self); e->date_style = s;
        static const char* styles[] = {"", "M/d/yy", "MMM d, y", "MMMM d, y", "EEEE, MMMM d, y"};
        std::string time = e->time_style ? (e->time_style == 1 ? " h:mm a" : " h:mm:ss a") : "";
        e->fmt = std::string(styles[std::min<long>(std::max<long>(s, 0), 4)]) + time;
    });
    def(DF, "setTimeStyle:", +[](id self, SEL, long s) {
        auto* e = ext_get<FmtExt>(self); e->time_style = s;
        static const char* styles[] = {"", "M/d/yy", "MMM d, y", "MMMM d, y", "EEEE, MMMM d, y"};
        std::string d = styles[std::min<long>(std::max<long>(e->date_style, 0), 4)];
        std::string t = s ? (s == 1 ? "h:mm a" : "h:mm:ss a") : "";
        e->fmt = d.empty() ? t : (t.empty() ? d : d + " " + t);
    });
    def(DF, "stringFromDate:", +[](id self, SEL, id d) -> id { auto* e = ext_get<FmtExt>(self); return d ? str(format_date(e->fmt, date_t(d), e->utc)) : nullptr; });
    def(DF, "dateFromString:", +[](id self, SEL, id s) -> id {
        auto* e = ext_get<FmtExt>(self);
        double ref;
        if (!s || !parse_date(e->fmt, utf8(s), e->utc, ref)) return nullptr;
        return date(ref);
    });
    defc(DF, "localizedStringFromDate:dateStyle:timeStyle:", +[](id, SEL, id d, long, long) -> id { return str(format_date("MMM d, y h:mm a", date_t(d), false)); });
    defc(DF, "dateFormatFromTemplate:options:locale:", +[](id, SEL, id t, NSUInteger, id) -> id { return t; });

    Class NF = def_class("NSNumberFormatter", "NSFormatter");
    struct NumFmtExt : Ext { long style = 0; long max_frac = 3; };
    def(NF, "setNumberStyle:", +[](id self, SEL, long s) { ext_get<NumFmtExt>(self)->style = s; });
    def(NF, "setFormatterBehavior:", +[](id, SEL, NSUInteger) {});
    def(NF, "setLocale:", +[](id, SEL, id) {});
    def(NF, "setMaximumFractionDigits:", +[](id self, SEL, long n) { ext_get<NumFmtExt>(self)->max_frac = n; });
    def(NF, "setMinimumFractionDigits:", +[](id, SEL, long) {});
    def(NF, "setGroupingSeparator:", +[](id, SEL, id) {});
    def(NF, "setUsesGroupingSeparator:", +[](id, SEL, bool) {});
    def(NF, "setCurrencySymbol:", +[](id, SEL, id) {});
    def(NF, "stringFromNumber:", +[](id self, SEL, id n) -> id {
        auto* e = ext_get<NumFmtExt>(self);
        char b[64];
        double v = num_dval(n);
        if (e->style == 2) snprintf(b, sizeof b, "$%.2f", v);
        else if (e->style == 3) snprintf(b, sizeof b, "%.0f%%", v * 100);
        else if (v == std::floor(v)) snprintf(b, sizeof b, "%.0f", v);
        else snprintf(b, sizeof b, "%.*f", static_cast<int>(e->max_frac), v);
        return str(b);
    });
    def(NF, "numberFromString:", +[](id, SEL, id s) -> id { std::string v = utf8(s); char* end; double d = strtod(v.c_str(), &end); return end == v.c_str() ? nullptr : num_d(d); });
    defc(NF, "localizedStringFromNumber:numberStyle:", +[](id, SEL, id n, long) -> id { return send(n, "stringValue"); });
}

// ---------------------------------------------------------------------------------------------------------------
// NSScanner
// ---------------------------------------------------------------------------------------------------------------
struct ScanExt : Ext { std::u16string s; size_t loc = 0; id skip = nullptr; bool skip_set = false; ~ScanExt() override { release(skip); } };
static void scan_skip(ScanExt* e) {
    if (!e->skip_set) { while (e->loc < e->s.size() && (e->s[e->loc] == ' ' || e->s[e->loc] == '\t' || e->s[e->loc] == '\n' || e->s[e->loc] == '\r')) ++e->loc; return; }
    if (!e->skip) return;
    while (e->loc < e->s.size() && send<bool>(e->skip, "characterIsMember:", static_cast<unsigned>(e->s[e->loc]))) ++e->loc;
}
static bool scan_number(ScanExt* e, bool allow_float, double& out) {
    scan_skip(e);
    std::string tmp;
    size_t i = e->loc;
    if (i < e->s.size() && (e->s[i] == '-' || e->s[i] == '+')) tmp.push_back(static_cast<char>(e->s[i++]));
    bool digits = false;
    while (i < e->s.size() && ((e->s[i] >= '0' && e->s[i] <= '9') || (allow_float && (e->s[i] == '.' || e->s[i] == 'e' || e->s[i] == 'E' ||
                                                                                       ((e->s[i] == '-' || e->s[i] == '+') && !tmp.empty() && (tmp.back() == 'e' || tmp.back() == 'E')))))) {
        if (e->s[i] >= '0' && e->s[i] <= '9') digits = true;
        tmp.push_back(static_cast<char>(e->s[i++]));
    }
    if (!digits) return false;
    out = strtod(tmp.c_str(), nullptr);
    e->loc = i;
    return true;
}
static void init_scanner() {
    Class S = def_class("NSScanner", "NSObject");
    defc(S, "scannerWithString:", +[](id, SEL, id s) -> id { id o = alloc(class_named("NSScanner")); ext_get<ScanExt>(o)->s = u16(s); return autorelease(o); });
    def(S, "initWithString:", +[](id self, SEL, id s) -> id { ext_get<ScanExt>(self)->s = u16(s); return self; });
    def(S, "string", +[](id self, SEL) -> id { return str16(ext_get<ScanExt>(self)->s); });
    def(S, "scanLocation", +[](id self, SEL) -> NSUInteger { return ext_get<ScanExt>(self)->loc; });
    def(S, "setScanLocation:", +[](id self, SEL, NSUInteger l) { ext_get<ScanExt>(self)->loc = l; });
    def(S, "setCharactersToBeSkipped:", +[](id self, SEL, id cs) { auto* e = ext_get<ScanExt>(self); release(e->skip); e->skip = retain(cs); e->skip_set = true; });
    def(S, "setCaseSensitive:", +[](id, SEL, bool) {});
    def(S, "isAtEnd", +[](id self, SEL) -> bool { auto* e = ext_get<ScanExt>(self); scan_skip(e); return e->loc >= e->s.size(); });
    def(S, "scanInt:", +[](id self, SEL, int* out) -> bool { double v; if (!scan_number(ext_get<ScanExt>(self), false, v)) return false; if (out) *out = static_cast<int>(v); return true; });
    def(S, "scanInteger:", +[](id self, SEL, long* out) -> bool { double v; if (!scan_number(ext_get<ScanExt>(self), false, v)) return false; if (out) *out = static_cast<long>(v); return true; });
    def(S, "scanLongLong:", +[](id self, SEL, long long* out) -> bool { double v; if (!scan_number(ext_get<ScanExt>(self), false, v)) return false; if (out) *out = static_cast<long long>(v); return true; });
    def(S, "scanFloat:", +[](id self, SEL, float* out) -> bool { double v; if (!scan_number(ext_get<ScanExt>(self), true, v)) return false; if (out) *out = static_cast<float>(v); return true; });
    def(S, "scanDouble:", +[](id self, SEL, double* out) -> bool { double v; if (!scan_number(ext_get<ScanExt>(self), true, v)) return false; if (out) *out = v; return true; });
    def(S, "scanHexInt:", +[](id self, SEL, unsigned* out) -> bool {
        auto* e = ext_get<ScanExt>(self); scan_skip(e);
        size_t i = e->loc;
        if (i + 1 < e->s.size() && e->s[i] == '0' && (e->s[i + 1] == 'x' || e->s[i + 1] == 'X')) i += 2;
        unsigned v = 0; bool any = false;
        while (i < e->s.size() && e->s[i] < 128 && isxdigit(e->s[i])) { v = v * 16 + static_cast<unsigned>(isdigit(e->s[i]) ? e->s[i] - '0' : (tolower(e->s[i]) - 'a' + 10)); ++i; any = true; }
        if (!any) return false;
        e->loc = i; if (out) *out = v; return true;
    });
    def(S, "scanString:intoString:", +[](id self, SEL, id s, id* out) -> bool {
        auto* e = ext_get<ScanExt>(self); scan_skip(e);
        const auto& n = u16(s);
        if (e->s.compare(e->loc, n.size(), n) != 0) return false;
        e->loc += n.size();
        if (out) *out = s;
        return true;
    });
    def(S, "scanUpToString:intoString:", +[](id self, SEL, id s, id* out) -> bool {
        auto* e = ext_get<ScanExt>(self); scan_skip(e);
        size_t k = e->s.find(u16(s), e->loc);
        if (k == std::u16string::npos) k = e->s.size();
        if (k == e->loc) return false;
        if (out) *out = str16(e->s.substr(e->loc, k - e->loc));
        e->loc = k;
        return true;
    });
    def(S, "scanCharactersFromSet:intoString:", +[](id self, SEL, id cs, id* out) -> bool {
        auto* e = ext_get<ScanExt>(self); scan_skip(e);
        size_t st = e->loc;
        while (e->loc < e->s.size() && send<bool>(cs, "characterIsMember:", static_cast<unsigned>(e->s[e->loc]))) ++e->loc;
        if (e->loc == st) return false;
        if (out) *out = str16(e->s.substr(st, e->loc - st));
        return true;
    });
    def(S, "scanUpToCharactersFromSet:intoString:", +[](id self, SEL, id cs, id* out) -> bool {
        auto* e = ext_get<ScanExt>(self); scan_skip(e);
        size_t st = e->loc;
        while (e->loc < e->s.size() && !send<bool>(cs, "characterIsMember:", static_cast<unsigned>(e->s[e->loc]))) ++e->loc;
        if (e->loc == st) return false;
        if (out) *out = str16(e->s.substr(st, e->loc - st));
        return true;
    });
}

// ---------------------------------------------------------------------------------------------------------------
// NSXMLParser (SAX over UTF-8)
// ---------------------------------------------------------------------------------------------------------------
struct XmlExt : Ext { std::string data; id delegate = nullptr; bool abort = false; long line = 1, col = 0; id error = nullptr; ~XmlExt() override { release(error); } };
static std::string xml_unescape(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { o.push_back(s[i]); continue; }
        size_t e = s.find(';', i);
        if (e == std::string::npos) { o.push_back('&'); continue; }
        std::string ent = s.substr(i + 1, e - i - 1);
        if (ent == "lt") o += "<";
        else if (ent == "gt") o += ">";
        else if (ent == "amp") o += "&";
        else if (ent == "quot") o += "\"";
        else if (ent == "apos") o += "'";
        else if (!ent.empty() && ent[0] == '#') {
            uint32_t cp = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? static_cast<uint32_t>(strtoul(ent.c_str() + 2, nullptr, 16)) : static_cast<uint32_t>(strtoul(ent.c_str() + 1, nullptr, 10));
            std::u16string u;
            if (cp >= 0x10000) { cp -= 0x10000; u.push_back(static_cast<char16_t>(0xd800 + (cp >> 10))); u.push_back(static_cast<char16_t>(0xdc00 + (cp & 0x3ff))); }
            else u.push_back(static_cast<char16_t>(cp));
            o += to8(u);
        } else {
            o += "&" + ent + ";";
        }
        i = e;
    }
    return o;
}
static bool xml_parse(id parser) {
    auto* x = ext_get<XmlExt>(parser);
    id d = x->delegate;
    const std::string& s = x->data;
    SEL s_start = sel("parser:didStartElement:namespaceURI:qualifiedName:attributes:");
    SEL s_end = sel("parser:didEndElement:namespaceURI:qualifiedName:");
    SEL s_chars = sel("parser:foundCharacters:");
    SEL s_cdata = sel("parser:foundCDATA:");
    bool r_start = responds(d, s_start), r_end = responds(d, s_end), r_chars = responds(d, s_chars), r_cdata = responds(d, s_cdata);
    if (responds(d, sel("parserDidStartDocument:"))) send<void>(d, "parserDidStartDocument:", parser);
    size_t i = 0;
    if (s.size() >= 3 && static_cast<uint8_t>(s[0]) == 0xef && static_cast<uint8_t>(s[1]) == 0xbb && static_cast<uint8_t>(s[2]) == 0xbf) i = 3;
    std::vector<std::string> stack;
    auto fail = [&](const char* why) {
        log("NSXMLParser: %s at offset %zu", why, i);
        x->error = retain(send_cls(class_named("NSError"), "errorWithDomain:code:userInfo:", str("NSXMLParserErrorDomain"), 4L, static_cast<id>(nullptr)));
        if (responds(d, sel("parser:parseErrorOccurred:"))) send<void>(d, "parser:parseErrorOccurred:", parser, x->error);
        return false;
    };
    while (i < s.size() && !x->abort) {
        if (s[i] != '<') {
            size_t e = s.find('<', i);
            if (e == std::string::npos) e = s.size();
            std::string text = s.substr(i, e - i);
            for (size_t k = i; k < e; ++k) if (s[k] == '\n') ++x->line;
            if (r_chars && !stack.empty()) {
                void* p = pool_push();
                send<void>(d, s_chars, parser, str(xml_unescape(text)));
                pool_pop(p);
            }
            i = e;
            continue;
        }
        if (s.compare(i, 4, "<!--") == 0) { size_t e = s.find("-->", i + 4); if (e == std::string::npos) return fail("unterminated comment"); i = e + 3; continue; }
        if (s.compare(i, 9, "<![CDATA[") == 0) {
            size_t e = s.find("]]>", i + 9);
            if (e == std::string::npos) return fail("unterminated CDATA");
            std::string text = s.substr(i + 9, e - i - 9);
            void* p = pool_push();
            if (r_cdata) send<void>(d, s_cdata, parser, autorelease(data_new(text.data(), text.size())));
            else if (r_chars) send<void>(d, s_chars, parser, str(text));
            pool_pop(p);
            i = e + 3;
            continue;
        }
        if (s.compare(i, 2, "<?") == 0) { size_t e = s.find("?>", i + 2); if (e == std::string::npos) return fail("unterminated PI"); i = e + 2; continue; }
        if (s.compare(i, 2, "<!") == 0) {
            int depth = 0;
            size_t k = i;
            for (; k < s.size(); ++k) { if (s[k] == '<') ++depth; else if (s[k] == '>' && --depth == 0) break; }
            i = k + 1;
            continue;
        }
        if (s.compare(i, 2, "</") == 0) {
            size_t e = s.find('>', i);
            if (e == std::string::npos) return fail("unterminated end tag");
            std::string name = s.substr(i + 2, e - i - 2);
            while (!name.empty() && isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
            if (!stack.empty()) stack.pop_back();
            if (r_end) {
                void* p = pool_push();
                send<void>(d, s_end, parser, str(name), static_cast<id>(nullptr), str(name));
                pool_pop(p);
            }
            i = e + 1;
            continue;
        }
        // start tag
        size_t k = i + 1;
        while (k < s.size() && !isspace(static_cast<unsigned char>(s[k])) && s[k] != '>' && s[k] != '/') ++k;
        std::string name = s.substr(i + 1, k - i - 1);
        void* p = pool_push();
        id attrs = autorelease(dict_new());
        bool self_close = false;
        while (k < s.size()) {
            while (k < s.size() && isspace(static_cast<unsigned char>(s[k]))) { if (s[k] == '\n') ++x->line; ++k; }
            if (k >= s.size()) break;
            if (s[k] == '>') { ++k; break; }
            if (s[k] == '/') { self_close = true; ++k; continue; }
            size_t an = k;
            while (k < s.size() && s[k] != '=' && !isspace(static_cast<unsigned char>(s[k])) && s[k] != '>') ++k;
            std::string aname = s.substr(an, k - an);
            while (k < s.size() && isspace(static_cast<unsigned char>(s[k]))) ++k;
            std::string aval;
            if (k < s.size() && s[k] == '=') {
                ++k;
                while (k < s.size() && isspace(static_cast<unsigned char>(s[k]))) ++k;
                char q = k < s.size() ? s[k] : '"';
                if (q == '"' || q == '\'') {
                    size_t e = s.find(q, k + 1);
                    if (e == std::string::npos) { pool_pop(p); return fail("unterminated attribute"); }
                    aval = s.substr(k + 1, e - k - 1);
                    k = e + 1;
                } else {
                    size_t st = k;
                    while (k < s.size() && !isspace(static_cast<unsigned char>(s[k])) && s[k] != '>') ++k;
                    aval = s.substr(st, k - st);
                }
            }
            if (!aname.empty()) dict_set(attrs, str(aname), str(xml_unescape(aval)));
        }
        if (r_start) send<void>(d, s_start, parser, str(name), static_cast<id>(nullptr), str(name), attrs);
        if (self_close) {
            if (r_end) send<void>(d, s_end, parser, str(name), static_cast<id>(nullptr), str(name));
        } else {
            stack.push_back(name);
        }
        pool_pop(p);
        i = k;
    }
    if (x->abort) return false;
    if (responds(d, sel("parserDidEndDocument:"))) send<void>(d, "parserDidEndDocument:", parser);
    return true;
}
static void init_xml() {
    Class X = def_class("NSXMLParser", "NSObject");
    def(X, "initWithData:", +[](id self, SEL, id d) -> id { ext_get<XmlExt>(self)->data.assign(reinterpret_cast<const char*>(data_bytes(d)), data_len(d)); return self; });
    def(X, "initWithContentsOfURL:", +[](id self, SEL, id u) -> id {
        std::string s;
        if (!read_file(remap_path(utf8(send(u, "path"))), s)) { release(self); return nullptr; }
        ext_get<XmlExt>(self)->data = s;
        return self;
    });
    def(X, "initWithStream:", +[](id self, SEL, id) -> id { return self; });
    def(X, "setDelegate:", +[](id self, SEL, id d) { ext_get<XmlExt>(self)->delegate = d; });
    def(X, "delegate", +[](id self, SEL) -> id { return ext_get<XmlExt>(self)->delegate; });
    def(X, "parse", +[](id self, SEL) -> bool { retain(self); bool r = xml_parse(self); release(self); return r; });
    def(X, "abortParsing", +[](id self, SEL) { ext_get<XmlExt>(self)->abort = true; });
    def(X, "parserError", +[](id self, SEL) -> id { return ext_get<XmlExt>(self)->error; });
    def(X, "lineNumber", +[](id self, SEL) -> long { return ext_get<XmlExt>(self)->line; });
    def(X, "columnNumber", +[](id self, SEL) -> long { return ext_get<XmlExt>(self)->col; });
    for (const char* s : {"setShouldProcessNamespaces:", "setShouldReportNamespacePrefixes:", "setShouldResolveExternalEntities:"})
        def(X, s, +[](id, SEL, bool) {});
}

// ---------------------------------------------------------------------------------------------------------------
// NSPropertyListSerialization / NSJSONSerialization / NSKeyedArchiver
// ---------------------------------------------------------------------------------------------------------------
static id archive_encode(id o, int depth);
static id archive_decode(id o, int depth);
struct CoderExt : Ext { id dict = nullptr; bool decoding = false; ~CoderExt() override { release(dict); } };
static Class C_Archiver, C_Unarchiver;

static id archive_encode(id o, int depth) {
    if (!o || depth > 64) return nullptr;
    if (is_string(o) || is_number(o) || is_data(o) || is_kind(o, C_NSDate)) return o;
    if (is_array(o)) {
        std::vector<id> v;
        for (id x : arr_items(o)) { id e = archive_encode(x, depth + 1); if (e) v.push_back(e); }
        id d = autorelease(dict_new());
        dict_set(d, "$array", arr(v));
        return d;
    }
    if (is_dict(o)) {
        std::vector<id> kv;
        for (auto& e : dict_entries(o)) { id k = archive_encode(e.first, depth + 1), v = archive_encode(e.second, depth + 1); if (k && v) { kv.push_back(k); kv.push_back(v); } }
        id d = autorelease(dict_new());
        dict_set(d, "$dict", arr(kv));
        return d;
    }
    if (is_kind(o, "NSSet")) {
        std::vector<id> v;
        for (id x : arr_items(send(o, "allObjects"))) { id e = archive_encode(x, depth + 1); if (e) v.push_back(e); }
        id d = autorelease(dict_new());
        dict_set(d, "$set", arr(v));
        return d;
    }
    if (is_kind(o, "NSNull")) { id d = autorelease(dict_new()); dict_set(d, "$null", num_b(true)); return d; }
    if (!responds(o, sel("encodeWithCoder:"))) { log("archive: %s does not implement encodeWithCoder:", class_name(class_of(o))); return nullptr; }
    id coder = autorelease(alloc(C_Archiver));
    auto* ce = ext_get<CoderExt>(coder);
    ce->dict = dict_new(true);
    dict_set(ce->dict, "$class", str(class_name(class_of(o))));
    send<void>(o, "encodeWithCoder:", coder);
    return ce->dict;
}
static id archive_decode(id o, int depth) {
    if (!o || depth > 64) return nullptr;
    if (!is_dict(o)) return o;
    if (id a = dict_get(o, "$array")) { std::vector<id> v; for (id x : arr_items(a)) { id d = archive_decode(x, depth + 1); if (d) v.push_back(d); } return autorelease(arr_new(v, true)); }
    if (id a = dict_get(o, "$dict")) {
        id d = autorelease(dict_new(true));
        const auto& kv = arr_items(a);
        for (size_t i = 0; i + 1 < kv.size(); i += 2) dict_set(d, archive_decode(kv[i], depth + 1), archive_decode(kv[i + 1], depth + 1));
        return d;
    }
    if (id a = dict_get(o, "$set")) { std::vector<id> v; for (id x : arr_items(a)) v.push_back(archive_decode(x, depth + 1)); return send_cls(class_named("NSMutableSet"), "setWithArray:", arr(v)); }
    if (dict_get(o, "$null")) return send_cls(class_named("NSNull"), "null");
    id cname = dict_get(o, "$class");
    if (!cname) return o;
    Class c = class_named(utf8(cname).c_str());
    if (!c) { log("unarchive: unknown class %s", utf8(cname).c_str()); return nullptr; }
    id coder = autorelease(alloc(C_Unarchiver));
    auto* ce = ext_get<CoderExt>(coder);
    ce->dict = retain(o);
    ce->decoding = true;
    id obj = send(send_cls(c, "alloc"), "initWithCoder:", coder);
    return autorelease(obj);
}
static id unarchive_data(const uint8_t* p, size_t n) {
    id root = plist_parse(p, n, true);
    if (!root) return nullptr;
    autorelease(root);
    if (dict_get(root, "$archiver")) { log("unarchive: Apple NSKeyedArchiver data is not supported"); return nullptr; }
    id top = dict_get(root, "root");
    return archive_decode(top, 0);
}
static id archive_data(id obj) {
    id root = autorelease(dict_new());
    id enc = archive_encode(obj, 0);
    if (enc) dict_set(root, "root", enc);
    dict_set(root, "$xnuroid", num_i(1));
    std::string out;
    if (!plist_write_binary(root, out)) return nullptr;
    return autorelease(data_new(out.data(), out.size()));
}

static void init_serialization() {
    Class P = def_class("NSPropertyListSerialization", "NSObject");
    defc(P, "propertyListWithData:options:format:error:", +[](id, SEL, id d, NSUInteger opt, NSUInteger* fmt, id* err) -> id {
        if (err) *err = nullptr;
        const uint8_t* p = data_bytes(d);
        size_t n = data_len(d);
        if (fmt) *fmt = n >= 6 && std::memcmp(p, "bplist", 6) == 0 ? 200 : 100;
        return autorelease(plist_parse(p, n, opt != 0));
    });
    defc(P, "propertyListFromData:mutabilityOption:format:errorDescription:", +[](id, SEL, id d, NSUInteger opt, NSUInteger* fmt, id* err) -> id {
        if (err) *err = nullptr;
        const uint8_t* p = data_bytes(d);
        size_t n = data_len(d);
        if (fmt) *fmt = n >= 6 && std::memcmp(p, "bplist", 6) == 0 ? 200 : 100;
        return autorelease(plist_parse(p, n, opt != 0));
    });
    auto write = +[](id, SEL, id pl, NSUInteger fmt, NSUInteger, id* err) -> id {
        if (err) *err = nullptr;
        std::string out;
        bool ok = fmt == 200 ? plist_write_binary(pl, out) : plist_write_xml(pl, out);
        return ok ? autorelease(data_new(out.data(), out.size())) : nullptr;
    };
    defc(P, "dataWithPropertyList:format:options:error:", write);
    defc(P, "dataFromPropertyList:format:errorDescription:", +[](id, SEL, id pl, NSUInteger fmt, id* err) -> id {
        if (err) *err = nullptr;
        std::string out;
        bool ok = fmt == 200 ? plist_write_binary(pl, out) : plist_write_xml(pl, out);
        return ok ? autorelease(data_new(out.data(), out.size())) : nullptr;
    });
    defc(P, "propertyList:isValidForFormat:", +[](id, SEL, id, NSUInteger) -> bool { return true; });

    Class J = def_class("NSJSONSerialization", "NSObject");
    defc(J, "JSONObjectWithData:options:error:", +[](id, SEL, id d, NSUInteger, id* err) -> id {
        id o = json_parse(data_bytes(d), data_len(d));
        if (err) *err = o ? nullptr : send_cls(class_named("NSError"), "errorWithDomain:code:userInfo:", str("NSCocoaErrorDomain"), 3840L, static_cast<id>(nullptr));
        return autorelease(o);
    });
    defc(J, "dataWithJSONObject:options:error:", +[](id, SEL, id o, NSUInteger opt, id* err) -> id {
        if (err) *err = nullptr;
        std::string out;
        if (!json_write(o, out, opt & 1)) return nullptr;
        return autorelease(data_new(out.data(), out.size()));
    });
    defc(J, "isValidJSONObject:", +[](id, SEL, id o) -> bool { return is_array(o) || is_dict(o); });

    C_Archiver = def_class("NSKeyedArchiver", "NSObject");
    def_class("NSCoder", "NSObject");
    C_Unarchiver = def_class("NSKeyedUnarchiver", "NSObject");
    defc(C_Archiver, "archivedDataWithRootObject:", +[](id, SEL, id o) -> id { return archive_data(o); });
    defc(C_Archiver, "archiveRootObject:toFile:", +[](id, SEL, id o, id path) -> bool {
        id d = archive_data(o);
        return d && send<bool>(d, "writeToFile:atomically:", path, 1);
    });
    defc(C_Unarchiver, "unarchiveObjectWithData:", +[](id, SEL, id d) -> id { return d ? unarchive_data(data_bytes(d), data_len(d)) : nullptr; });
    defc(C_Unarchiver, "unarchiveObjectWithFile:", +[](id, SEL, id path) -> id {
        std::string s;
        if (!read_file(remap_path(utf8(path)), s)) return nullptr;
        return unarchive_data(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    });
    for (Class C : {C_Archiver, C_Unarchiver}) {
        def(C, "encodeObject:forKey:", +[](id self, SEL, id o, id k) { id e = archive_encode(o, 1); if (e) dict_set(ext_get<CoderExt>(self)->dict, k, e); });
        def(C, "encodeConditionalObject:forKey:", +[](id self, SEL, id o, id k) { id e = archive_encode(o, 1); if (e) dict_set(ext_get<CoderExt>(self)->dict, k, e); });
        def(C, "encodeBool:forKey:", +[](id self, SEL, bool v, id k) { dict_set(ext_get<CoderExt>(self)->dict, k, num_b(v)); });
        def(C, "encodeInt:forKey:", +[](id self, SEL, int v, id k) { dict_set(ext_get<CoderExt>(self)->dict, k, num_i(v)); });
        def(C, "encodeInt32:forKey:", +[](id self, SEL, int v, id k) { dict_set(ext_get<CoderExt>(self)->dict, k, num_i(v)); });
        def(C, "encodeInt64:forKey:", +[](id self, SEL, long long v, id k) { dict_set(ext_get<CoderExt>(self)->dict, k, num_i(v)); });
        def(C, "encodeInteger:forKey:", +[](id self, SEL, long v, id k) { dict_set(ext_get<CoderExt>(self)->dict, k, num_i(v)); });
        def(C, "encodeFloat:forKey:", +[](id self, SEL, float v, id k) { dict_set(ext_get<CoderExt>(self)->dict, k, num_f(v)); });
        def(C, "encodeDouble:forKey:", +[](id self, SEL, double v, id k) { dict_set(ext_get<CoderExt>(self)->dict, k, num_d(v)); });
        def(C, "encodeBytes:length:forKey:", +[](id self, SEL, const void* p, NSUInteger n, id k) { dict_set(ext_get<CoderExt>(self)->dict, k, autorelease(data_new(p, n))); });
        def(C, "decodeObjectForKey:", +[](id self, SEL, id k) -> id { return archive_decode(dict_get(ext_get<CoderExt>(self)->dict, k), 1); });
        def(C, "decodeObjectOfClass:forKey:", +[](id self, SEL, Class, id k) -> id { return archive_decode(dict_get(ext_get<CoderExt>(self)->dict, k), 1); });
        def(C, "decodeObjectOfClasses:forKey:", +[](id self, SEL, id, id k) -> id { return archive_decode(dict_get(ext_get<CoderExt>(self)->dict, k), 1); });
        def(C, "decodeBoolForKey:", +[](id self, SEL, id k) -> bool { return num_ival(dict_get(ext_get<CoderExt>(self)->dict, k)) != 0; });
        def(C, "decodeIntForKey:", +[](id self, SEL, id k) -> int { return static_cast<int>(num_ival(dict_get(ext_get<CoderExt>(self)->dict, k))); });
        def(C, "decodeInt32ForKey:", +[](id self, SEL, id k) -> int { return static_cast<int>(num_ival(dict_get(ext_get<CoderExt>(self)->dict, k))); });
        def(C, "decodeInt64ForKey:", +[](id self, SEL, id k) -> long long { return num_ival(dict_get(ext_get<CoderExt>(self)->dict, k)); });
        def(C, "decodeIntegerForKey:", +[](id self, SEL, id k) -> long { return num_ival(dict_get(ext_get<CoderExt>(self)->dict, k)); });
        def(C, "decodeFloatForKey:", +[](id self, SEL, id k) -> float { return static_cast<float>(num_dval(dict_get(ext_get<CoderExt>(self)->dict, k))); });
        def(C, "decodeDoubleForKey:", +[](id self, SEL, id k) -> double { return num_dval(dict_get(ext_get<CoderExt>(self)->dict, k)); });
        def(C, "decodeBytesForKey:returnedLength:", +[](id self, SEL, id k, NSUInteger* n) -> const void* {
            id d = dict_get(ext_get<CoderExt>(self)->dict, k); if (n) *n = data_len(d); return data_bytes(d);
        });
        def(C, "containsValueForKey:", +[](id self, SEL, id k) -> bool { return dict_get(ext_get<CoderExt>(self)->dict, k) != nullptr; });
        def(C, "allowsKeyedCoding", +[](id, SEL) -> bool { return true; });
        def(C, "requiresSecureCoding", +[](id, SEL) -> bool { return false; });
        def(C, "setRequiresSecureCoding:", +[](id, SEL, bool) {});
        def(C, "finishEncoding", +[](id, SEL) {});
        def(C, "finishDecoding", +[](id, SEL) {});
        def(C, "setDelegate:", +[](id, SEL, id) {});
        def(C, "setOutputFormat:", +[](id, SEL, NSUInteger) {});
    }
}

// ---------------------------------------------------------------------------------------------------------------
// NSOperationQueue / NSOperation
// ---------------------------------------------------------------------------------------------------------------
struct OpExt : Ext {
    std::vector<id> blocks;
    id completion = nullptr, target = nullptr, arg = nullptr;
    SEL sel = nullptr;
    std::atomic<bool> cancelled{false}, executing{false}, finished{false};
    ~OpExt() override { for (id b : blocks) block_release(b); if (completion) block_release(completion); release(target); release(arg); }
};
struct QueueExt : Ext {
    bool main = false, suspended = false;
    std::mutex mu;
    std::condition_variable cv;
    std::deque<id> pending;
    int running = 0;
    bool worker = false;
    id name = nullptr;
};
static id g_main_queue;
static void op_run(id op) {
    auto* e = ext_get<OpExt>(op);
    if (!e->cancelled) {
        e->executing = true;
        send<void>(op, "main");
        e->executing = false;
    }
    e->finished = true;
    if (e->completion) call_block0(e->completion);
}
static void queue_add(id q, id op) {
    auto* qe = ext_get<QueueExt>(q);
    retain(op);
    if (qe->main) { post_main([op] { send<void>(op, "start"); release(op); }); return; }
    std::lock_guard<std::mutex> l(qe->mu);
    qe->pending.push_back(op);
    if (!qe->worker) {
        qe->worker = true;
        retain(q);
        spawn_thread([q, qe] {
            for (;;) {
                id next;
                {
                    std::unique_lock<std::mutex> l2(qe->mu);
                    qe->cv.wait_for(l2, std::chrono::seconds(5), [qe] { return !qe->pending.empty() && !qe->suspended; });
                    if (qe->pending.empty() || qe->suspended) {
                        if (qe->pending.empty()) { qe->worker = false; break; }
                        continue;
                    }
                    next = qe->pending.front();
                    qe->pending.pop_front();
                    qe->running++;
                }
                void* p = pool_push();
                send<void>(next, "start");
                pool_pop(p);
                release(next);
                {
                    std::lock_guard<std::mutex> l3(qe->mu);
                    qe->running--;
                }
                qe->cv.notify_all();
            }
            release(q);
        }, "NSOperationQueue");
    }
    qe->cv.notify_all();
}
static void init_operations() {
    Class O = def_class("NSOperation", "NSObject");
    def(O, "start", +[](id self, SEL) { op_run(self); });
    def(O, "main", +[](id, SEL) {});
    def(O, "cancel", +[](id self, SEL) { ext_get<OpExt>(self)->cancelled = true; });
    def(O, "isCancelled", +[](id self, SEL) -> bool { return ext_get<OpExt>(self)->cancelled; });
    def(O, "isExecuting", +[](id self, SEL) -> bool { return ext_get<OpExt>(self)->executing; });
    def(O, "isFinished", +[](id self, SEL) -> bool { return ext_get<OpExt>(self)->finished; });
    def(O, "isConcurrent", +[](id, SEL) -> bool { return false; });
    def(O, "isAsynchronous", +[](id, SEL) -> bool { return false; });
    def(O, "isReady", +[](id, SEL) -> bool { return true; });
    def(O, "addDependency:", +[](id, SEL, id) {});
    def(O, "removeDependency:", +[](id, SEL, id) {});
    def(O, "dependencies", +[](id, SEL) -> id { return arr({}); });
    def(O, "setQueuePriority:", +[](id, SEL, long) {});
    def(O, "setQualityOfService:", +[](id, SEL, long) {});
    def(O, "setThreadPriority:", +[](id, SEL, double) {});
    def(O, "setName:", +[](id, SEL, id) {});
    def(O, "completionBlock", +[](id self, SEL) -> id { return ext_get<OpExt>(self)->completion; });
    def(O, "setCompletionBlock:", +[](id self, SEL, id b) { auto* e = ext_get<OpExt>(self); if (e->completion) block_release(e->completion); e->completion = b ? block_copy(b) : nullptr; });
    def(O, "waitUntilFinished", +[](id self, SEL) { auto* e = ext_get<OpExt>(self); while (!e->finished) usleep(1000); });

    Class B = def_class("NSBlockOperation", "NSOperation");
    defc(B, "blockOperationWithBlock:", +[](id self, SEL, id b) -> id {
        id op = alloc(reinterpret_cast<Class>(self)); ext_get<OpExt>(op)->blocks.push_back(block_copy(b)); return autorelease(op);
    });
    def(B, "addExecutionBlock:", +[](id self, SEL, id b) { ext_get<OpExt>(self)->blocks.push_back(block_copy(b)); });
    def(B, "executionBlocks", +[](id self, SEL) -> id { return arr(ext_get<OpExt>(self)->blocks); });
    def(B, "main", +[](id self, SEL) { for (id b : ext_get<OpExt>(self)->blocks) call_block0(b); });

    Class I = def_class("NSInvocationOperation", "NSOperation");
    def(I, "initWithTarget:selector:object:", +[](id self, SEL, id t, SEL s, id a) -> id {
        auto* e = ext_get<OpExt>(self); e->target = retain(t); e->sel = s; e->arg = retain(a); return self;
    });
    def(I, "main", +[](id self, SEL) { auto* e = ext_get<OpExt>(self); send<void>(e->target, e->sel, e->arg); });

    Class Q = def_class("NSOperationQueue", "NSObject");
    g_main_queue = alloc(Q);
    ext_get<QueueExt>(g_main_queue)->main = true;
    defc(Q, "mainQueue", +[](id, SEL) -> id { return g_main_queue; });
    defc(Q, "currentQueue", +[](id, SEL) -> id { return on_main_thread() ? g_main_queue : nullptr; });
    def(Q, "init", +[](id self, SEL) -> id { ext_get<QueueExt>(self); return self; });
    def(Q, "addOperation:", +[](id self, SEL, id op) { queue_add(self, op); });
    def(Q, "addOperations:waitUntilFinished:", +[](id self, SEL, id ops, bool wait) {
        for (id op : arr_items(ops)) queue_add(self, op);
        if (wait && !ext_get<QueueExt>(self)->main) for (id op : arr_items(ops)) send<void>(op, "waitUntilFinished");
    });
    def(Q, "addOperationWithBlock:", +[](id self, SEL, id b) { queue_add(self, send_cls(class_named("NSBlockOperation"), "blockOperationWithBlock:", b)); });
    def(Q, "setMaxConcurrentOperationCount:", +[](id, SEL, long) {});
    def(Q, "maxConcurrentOperationCount", +[](id, SEL) -> long { return 1; });
    def(Q, "setSuspended:", +[](id self, SEL, bool s) { auto* e = ext_get<QueueExt>(self); e->suspended = s; e->cv.notify_all(); });
    def(Q, "isSuspended", +[](id self, SEL) -> bool { return ext_get<QueueExt>(self)->suspended; });
    def(Q, "operationCount", +[](id self, SEL) -> NSUInteger { auto* e = ext_get<QueueExt>(self); std::lock_guard<std::mutex> l(e->mu); return e->pending.size() + e->running; });
    def(Q, "operations", +[](id self, SEL) -> id { auto* e = ext_get<QueueExt>(self); std::lock_guard<std::mutex> l(e->mu); return arr(std::vector<id>(e->pending.begin(), e->pending.end())); });
    def(Q, "cancelAllOperations", +[](id self, SEL) {
        auto* e = ext_get<QueueExt>(self); std::lock_guard<std::mutex> l(e->mu);
        for (id op : e->pending) send<void>(op, "cancel");
    });
    def(Q, "waitUntilAllOperationsAreFinished", +[](id self, SEL) {
        auto* e = ext_get<QueueExt>(self);
        if (e->main) return;
        std::unique_lock<std::mutex> l(e->mu);
        e->cv.wait(l, [e] { return e->pending.empty() && e->running == 0; });
    });
    def(Q, "setName:", +[](id self, SEL, id n) { auto* e = ext_get<QueueExt>(self); release(e->name); e->name = retain(n); });
    def(Q, "name", +[](id self, SEL) -> id { return ext_get<QueueExt>(self)->name; });
    def(Q, "setQualityOfService:", +[](id, SEL, long) {});
    def(Q, "setUnderlyingQueue:", +[](id, SEL, void*) {});
}

// ---------------------------------------------------------------------------------------------------------------
// URL loading: there is no network; every request fails as "not connected to the internet"
// ---------------------------------------------------------------------------------------------------------------
struct ReqExt : Ext { id url = nullptr, method = nullptr, body = nullptr, headers = nullptr; double timeout = 60; ~ReqExt() override { release(url); release(method); release(body); release(headers); } };
static id offline_error(id url) {
    id info = autorelease(dict_new());
    dict_set(info, "NSLocalizedDescription", str("The Internet connection appears to be offline."));
    if (url) { dict_set(info, "NSErrorFailingURLKey", url); dict_set(info, "NSErrorFailingURLStringKey", send(url, "absoluteString")); }
    return send_cls(class_named("NSError"), "errorWithDomain:code:userInfo:", str("NSURLErrorDomain"), -1009L, info);
}
static void init_url_loading() {
    Class R = def_class("NSURLRequest", "NSObject");
    Class MR = def_class("NSMutableURLRequest", "NSURLRequest");
    (void)MR;
    defc(R, "requestWithURL:", +[](id self, SEL, id u) -> id { id r = alloc(reinterpret_cast<Class>(self)); ext_get<ReqExt>(r)->url = retain(u); return autorelease(r); });
    defc(R, "requestWithURL:cachePolicy:timeoutInterval:", +[](id self, SEL, id u, NSUInteger, double t) -> id {
        id r = alloc(reinterpret_cast<Class>(self)); auto* e = ext_get<ReqExt>(r); e->url = retain(u); e->timeout = t; return autorelease(r);
    });
    def(R, "initWithURL:", +[](id self, SEL, id u) -> id { ext_get<ReqExt>(self)->url = retain(u); return self; });
    def(R, "initWithURL:cachePolicy:timeoutInterval:", +[](id self, SEL, id u, NSUInteger, double t) -> id { auto* e = ext_get<ReqExt>(self); e->url = retain(u); e->timeout = t; return self; });
    def(R, "URL", +[](id self, SEL) -> id { return ext_get<ReqExt>(self)->url; });
    def(R, "setURL:", +[](id self, SEL, id u) { auto* e = ext_get<ReqExt>(self); release(e->url); e->url = retain(u); });
    def(R, "HTTPMethod", +[](id self, SEL) -> id { id m = ext_get<ReqExt>(self)->method; return m ? m : str("GET"); });
    def(R, "setHTTPMethod:", +[](id self, SEL, id m) { auto* e = ext_get<ReqExt>(self); release(e->method); e->method = retain(m); });
    def(R, "HTTPBody", +[](id self, SEL) -> id { return ext_get<ReqExt>(self)->body; });
    def(R, "setHTTPBody:", +[](id self, SEL, id b) { auto* e = ext_get<ReqExt>(self); release(e->body); e->body = retain(b); });
    def(R, "HTTPBodyStream", +[](id, SEL) -> id { return nullptr; });
    def(R, "setHTTPBodyStream:", +[](id, SEL, id) {});
    def(R, "allHTTPHeaderFields", +[](id self, SEL) -> id { return ext_get<ReqExt>(self)->headers; });
    def(R, "setAllHTTPHeaderFields:", +[](id self, SEL, id h) { auto* e = ext_get<ReqExt>(self); release(e->headers); e->headers = send(h, "mutableCopy"); });
    def(R, "valueForHTTPHeaderField:", +[](id self, SEL, id k) -> id { return dict_get(ext_get<ReqExt>(self)->headers, k); });
    def(R, "setValue:forHTTPHeaderField:", +[](id self, SEL, id v, id k) { auto* e = ext_get<ReqExt>(self); if (!e->headers) e->headers = dict_new(); dict_set(e->headers, k, v); });
    def(R, "addValue:forHTTPHeaderField:", +[](id self, SEL, id v, id k) { auto* e = ext_get<ReqExt>(self); if (!e->headers) e->headers = dict_new(); dict_set(e->headers, k, v); });
    def(R, "timeoutInterval", +[](id self, SEL) -> double { return ext_get<ReqExt>(self)->timeout; });
    def(R, "setTimeoutInterval:", +[](id self, SEL, double t) { ext_get<ReqExt>(self)->timeout = t; });
    for (const char* s : {"setCachePolicy:", "setHTTPShouldHandleCookies:", "setNetworkServiceType:", "setHTTPShouldUsePipelining:", "setAllowsCellularAccess:"})
        def(R, s, +[](id, SEL, NSUInteger) {});
    def(R, "copyWithZone:", +[](id self, SEL, void*) -> id { return retain(self); });
    def(R, "mutableCopyWithZone:", +[](id self, SEL, void*) -> id { return retain(self); });

    def_class("NSURLResponse", "NSObject");
    Class HR = def_class("NSHTTPURLResponse", "NSURLResponse");
    def(HR, "statusCode", +[](id, SEL) -> long { return 0; });
    def(HR, "allHeaderFields", +[](id, SEL) -> id { return autorelease(dict_new(false)); });
    defc(HR, "localizedStringForStatusCode:", +[](id, SEL, long) -> id { return str("offline"); });

    Class C = def_class("NSURLConnection", "NSObject");
    struct ConnExt : Ext { id req = nullptr, delegate = nullptr; bool cancelled = false; ~ConnExt() override { release(req); release(delegate); } };
    auto start_conn = [](id conn) {
        retain(conn);
        post_main([conn] {
            auto* e = ext_get<ConnExt>(conn);
            if (!e->cancelled && responds(e->delegate, sel("connection:didFailWithError:")))
                send<void>(e->delegate, "connection:didFailWithError:", conn, offline_error(send(e->req, "URL")));
            release(conn);
        }, 0.05);
    };
    static decltype(start_conn) s_start = start_conn;
    defc(C, "connectionWithRequest:delegate:", +[](id self, SEL, id req, id d) -> id {
        id c = alloc(reinterpret_cast<Class>(self)); auto* e = ext_get<ConnExt>(c); e->req = retain(req); e->delegate = retain(d); s_start(c); return autorelease(c);
    });
    def(C, "initWithRequest:delegate:", +[](id self, SEL, id req, id d) -> id { auto* e = ext_get<ConnExt>(self); e->req = retain(req); e->delegate = retain(d); s_start(self); return self; });
    def(C, "initWithRequest:delegate:startImmediately:", +[](id self, SEL, id req, id d, bool now) -> id {
        auto* e = ext_get<ConnExt>(self); e->req = retain(req); e->delegate = retain(d); if (now) s_start(self); return self;
    });
    def(C, "start", +[](id self, SEL) { s_start(self); });
    def(C, "cancel", +[](id self, SEL) { ext_get<ConnExt>(self)->cancelled = true; });
    def(C, "scheduleInRunLoop:forMode:", +[](id, SEL, id, id) {});
    def(C, "unscheduleFromRunLoop:forMode:", +[](id, SEL, id, id) {});
    def(C, "setDelegateQueue:", +[](id, SEL, id) {});
    defc(C, "sendSynchronousRequest:returningResponse:error:", +[](id, SEL, id req, id* resp, id* err) -> id {
        if (resp) *resp = nullptr;
        if (err) *err = offline_error(send(req, "URL"));
        return nullptr;
    });
    defc(C, "sendAsynchronousRequest:queue:completionHandler:", +[](id, SEL, id req, id, id handler) {
        id h = block_copy(handler);
        id url = retain(send(req, "URL"));
        post_main([h, url] {
            reinterpret_cast<void (*)(id, id, id, id)>(block_invoke_ptr(h))(h, nullptr, nullptr, offline_error(url));
            block_release(h);
            release(url);
        });
    });
    defc(C, "canHandleRequest:", +[](id, SEL, id) -> bool { return false; });

    Class SC = def_class("NSURLSessionConfiguration", "NSObject");
    static id cfg = alloc(SC);
    defc(SC, "defaultSessionConfiguration", +[](id, SEL) -> id { return cfg; });
    defc(SC, "ephemeralSessionConfiguration", +[](id, SEL) -> id { return cfg; });
    defc(SC, "backgroundSessionConfiguration:", +[](id, SEL, id) -> id { return cfg; });
    defc(SC, "backgroundSessionConfigurationWithIdentifier:", +[](id, SEL, id) -> id { return cfg; });
    for (const char* s : {"setHTTPAdditionalHeaders:", "setTimeoutIntervalForRequest:", "setTimeoutIntervalForResource:", "setURLCache:", "setRequestCachePolicy:",
                          "setHTTPMaximumConnectionsPerHost:", "setAllowsCellularAccess:", "setDiscretionary:", "setSessionSendsLaunchEvents:"})
        def(SC, s, +[](id, SEL, id) {});
    Class SS = def_class("NSURLSession", "NSObject");
    Class TK = def_class("NSURLSessionTask", "NSObject");
    def_class("NSURLSessionDataTask", "NSURLSessionTask");
    def_class("NSURLSessionDownloadTask", "NSURLSessionTask");
    def_class("NSURLSessionUploadTask", "NSURLSessionTask");
    struct TaskExt : Ext { id handler = nullptr, url = nullptr; bool done = false; ~TaskExt() override { if (handler) block_release(handler); release(url); } };
    static id shared = alloc(SS);
    defc(SS, "sharedSession", +[](id, SEL) -> id { return shared; });
    defc(SS, "sessionWithConfiguration:", +[](id, SEL, id) -> id { return shared; });
    defc(SS, "sessionWithConfiguration:delegate:delegateQueue:", +[](id, SEL, id, id, id) -> id { return shared; });
    auto make_task = [](id url, id handler) -> id {
        id t = alloc(class_named("NSURLSessionDataTask"));
        auto* e = ext_get<TaskExt>(t);
        e->url = retain(url);
        e->handler = handler ? block_copy(handler) : nullptr;
        return autorelease(t);
    };
    static decltype(make_task) s_task = make_task;
    def(SS, "dataTaskWithRequest:completionHandler:", +[](id, SEL, id req, id h) -> id { return s_task(send(req, "URL"), h); });
    def(SS, "dataTaskWithURL:completionHandler:", +[](id, SEL, id u, id h) -> id { return s_task(u, h); });
    def(SS, "dataTaskWithRequest:", +[](id, SEL, id req) -> id { return s_task(send(req, "URL"), nullptr); });
    def(SS, "dataTaskWithURL:", +[](id, SEL, id u) -> id { return s_task(u, nullptr); });
    def(SS, "downloadTaskWithRequest:completionHandler:", +[](id, SEL, id req, id h) -> id { return s_task(send(req, "URL"), h); });
    def(SS, "uploadTaskWithRequest:fromData:completionHandler:", +[](id, SEL, id req, id, id h) -> id { return s_task(send(req, "URL"), h); });
    def(SS, "invalidateAndCancel", +[](id, SEL) {});
    def(SS, "finishTasksAndInvalidate", +[](id, SEL) {});
    def(TK, "resume", +[](id self, SEL) {
        retain(self);
        post_main([self] {
            auto* e = ext_get<TaskExt>(self);
            if (!e->done && e->handler) reinterpret_cast<void (*)(id, id, id, id)>(block_invoke_ptr(e->handler))(e->handler, nullptr, nullptr, offline_error(e->url));
            e->done = true;
            release(self);
        }, 0.05);
    });
    def(TK, "cancel", +[](id self, SEL) { ext_get<TaskExt>(self)->done = true; });
    def(TK, "suspend", +[](id, SEL) {});
    def(TK, "state", +[](id self, SEL) -> long { return ext_get<TaskExt>(self)->done ? 3 : 0; });
    def(TK, "taskIdentifier", +[](id self, SEL) -> NSUInteger { return reinterpret_cast<NSUInteger>(self) & 0xffff; });
    def(TK, "originalRequest", +[](id, SEL) -> id { return nullptr; });
    def(TK, "currentRequest", +[](id, SEL) -> id { return nullptr; });
    def(TK, "response", +[](id, SEL) -> id { return nullptr; });

    Class CR = def_class("NSURLCredential", "NSObject");
    defc(CR, "credentialForTrust:", +[](id, SEL, void*) -> id { return autorelease(alloc(class_named("NSURLCredential"))); });
    defc(CR, "credentialWithUser:password:persistence:", +[](id, SEL, id, id, NSUInteger) -> id { return autorelease(alloc(class_named("NSURLCredential"))); });
    Class CA = def_class("NSURLCache", "NSObject");
    static id cache = alloc(CA);
    defc(CA, "sharedURLCache", +[](id, SEL) -> id { return cache; });
    defc(CA, "setSharedURLCache:", +[](id, SEL, id) {});
    def(CA, "removeAllCachedResponses", +[](id, SEL) {});
    def(CA, "initWithMemoryCapacity:diskCapacity:diskPath:", +[](id self, SEL, NSUInteger, NSUInteger, id) -> id { return self; });
    def(CA, "cachedResponseForRequest:", +[](id, SEL, id) -> id { return nullptr; });
    Class HS = def_class("NSHTTPCookieStorage", "NSObject");
    static id cookies = alloc(HS);
    defc(HS, "sharedHTTPCookieStorage", +[](id, SEL) -> id { return cookies; });
    def(HS, "cookies", +[](id, SEL) -> id { return arr({}); });
    def(HS, "cookiesForURL:", +[](id, SEL, id) -> id { return arr({}); });
    def(HS, "setCookieAcceptPolicy:", +[](id, SEL, NSUInteger) {});
}

// ---------------------------------------------------------------------------------------------------------------
// small value classes
// ---------------------------------------------------------------------------------------------------------------
struct IndexSetExt : Ext { std::set<NSUInteger> s; };
struct IndexPathExt : Ext { std::vector<NSUInteger> v; };
struct SortExt : Ext { id key = nullptr; bool asc = true; SEL sel = nullptr; id block = nullptr; ~SortExt() override { release(key); if (block) block_release(block); } };
struct UuidExt : Ext { uint8_t b[16]; };
static void init_values() {
    Class IS = def_class("NSIndexSet", "NSObject");
    Class MIS = def_class("NSMutableIndexSet", "NSIndexSet");
    defc(IS, "indexSet", +[](id self, SEL) -> id { id o = alloc(reinterpret_cast<Class>(self)); ext_get<IndexSetExt>(o); return autorelease(o); });
    defc(IS, "indexSetWithIndex:", +[](id self, SEL, NSUInteger i) -> id { id o = alloc(reinterpret_cast<Class>(self)); ext_get<IndexSetExt>(o)->s.insert(i); return autorelease(o); });
    defc(IS, "indexSetWithIndexesInRange:", +[](id self, SEL, NSRange r) -> id {
        id o = alloc(reinterpret_cast<Class>(self)); auto& s = ext_get<IndexSetExt>(o)->s; for (NSUInteger i = 0; i < r.length; ++i) s.insert(r.location + i); return autorelease(o);
    });
    def(IS, "init", +[](id self, SEL) -> id { ext_get<IndexSetExt>(self); return self; });
    def(IS, "count", +[](id self, SEL) -> NSUInteger { return ext_get<IndexSetExt>(self)->s.size(); });
    def(IS, "firstIndex", +[](id self, SEL) -> NSUInteger { auto& s = ext_get<IndexSetExt>(self)->s; return s.empty() ? NSNotFound : *s.begin(); });
    def(IS, "lastIndex", +[](id self, SEL) -> NSUInteger { auto& s = ext_get<IndexSetExt>(self)->s; return s.empty() ? NSNotFound : *s.rbegin(); });
    def(IS, "containsIndex:", +[](id self, SEL, NSUInteger i) -> bool { return ext_get<IndexSetExt>(self)->s.count(i) != 0; });
    def(IS, "indexGreaterThanIndex:", +[](id self, SEL, NSUInteger i) -> NSUInteger { auto& s = ext_get<IndexSetExt>(self)->s; auto it = s.upper_bound(i); return it == s.end() ? NSNotFound : *it; });
    def(IS, "enumerateIndexesUsingBlock:", +[](id self, SEL, id b) {
        bool stop = false; auto s = ext_get<IndexSetExt>(self)->s;
        for (NSUInteger i : s) { reinterpret_cast<void (*)(id, NSUInteger, bool*)>(block_invoke_ptr(b))(b, i, &stop); if (stop) break; }
    });
    def(IS, "copyWithZone:", +[](id self, SEL, void*) -> id { id o = alloc(class_named("NSIndexSet")); ext_get<IndexSetExt>(o)->s = ext_get<IndexSetExt>(self)->s; return o; });
    def(IS, "mutableCopyWithZone:", +[](id self, SEL, void*) -> id { id o = alloc(class_named("NSMutableIndexSet")); ext_get<IndexSetExt>(o)->s = ext_get<IndexSetExt>(self)->s; return o; });
    def(MIS, "addIndex:", +[](id self, SEL, NSUInteger i) { ext_get<IndexSetExt>(self)->s.insert(i); });
    def(MIS, "removeIndex:", +[](id self, SEL, NSUInteger i) { ext_get<IndexSetExt>(self)->s.erase(i); });
    def(MIS, "addIndexesInRange:", +[](id self, SEL, NSRange r) { auto& s = ext_get<IndexSetExt>(self)->s; for (NSUInteger i = 0; i < r.length; ++i) s.insert(r.location + i); });
    def(MIS, "addIndexes:", +[](id self, SEL, id o) { for (NSUInteger i : ext_get<IndexSetExt>(o)->s) ext_get<IndexSetExt>(self)->s.insert(i); });
    def(MIS, "removeAllIndexes", +[](id self, SEL) { ext_get<IndexSetExt>(self)->s.clear(); });
    Class A = class_named("NSArray");
    Class MA = class_named("NSMutableArray");
    def(A, "objectsAtIndexes:", +[](id self, SEL, id is) -> id {
        std::vector<id> v; const auto& items = arr_items(self);
        for (NSUInteger i : ext_get<IndexSetExt>(is)->s) if (i < items.size()) v.push_back(items[i]);
        return arr(v);
    });
    def(A, "indexesOfObjectsPassingTest:", +[](id self, SEL, id b) -> id {
        id is = autorelease(alloc(class_named("NSMutableIndexSet")));
        auto& s = ext_get<IndexSetExt>(is)->s;
        std::vector<id> items = arr_items(self);
        bool stop = false;
        for (size_t i = 0; i < items.size() && !stop; ++i)
            if (reinterpret_cast<bool (*)(id, id, NSUInteger, bool*)>(block_invoke_ptr(b))(b, items[i], i, &stop)) s.insert(i);
        return is;
    });
    def(MA, "removeObjectsAtIndexes:", +[](id self, SEL, id is) {
        auto s = ext_get<IndexSetExt>(is)->s;
        for (auto it = s.rbegin(); it != s.rend(); ++it) send<void>(self, "removeObjectAtIndex:", *it);
    });
    def(MA, "insertObjects:atIndexes:", +[](id self, SEL, id objs, id is) {
        const auto& items = arr_items(objs); size_t k = 0;
        for (NSUInteger i : ext_get<IndexSetExt>(is)->s) if (k < items.size()) send<void>(self, "insertObject:atIndex:", items[k++], i);
    });

    Class IP = def_class("NSIndexPath", "NSObject");
    defc(IP, "indexPathForRow:inSection:", +[](id, SEL, long row, long sec) -> id {
        id o = alloc(class_named("NSIndexPath")); ext_get<IndexPathExt>(o)->v = {static_cast<NSUInteger>(sec), static_cast<NSUInteger>(row)}; return autorelease(o);
    });
    defc(IP, "indexPathForItem:inSection:", +[](id, SEL, long row, long sec) -> id {
        id o = alloc(class_named("NSIndexPath")); ext_get<IndexPathExt>(o)->v = {static_cast<NSUInteger>(sec), static_cast<NSUInteger>(row)}; return autorelease(o);
    });
    defc(IP, "indexPathWithIndex:", +[](id, SEL, NSUInteger i) -> id { id o = alloc(class_named("NSIndexPath")); ext_get<IndexPathExt>(o)->v = {i}; return autorelease(o); });
    def(IP, "row", +[](id self, SEL) -> long { auto& v = ext_get<IndexPathExt>(self)->v; return v.size() > 1 ? static_cast<long>(v[1]) : 0; });
    def(IP, "item", +[](id self, SEL) -> long { auto& v = ext_get<IndexPathExt>(self)->v; return v.size() > 1 ? static_cast<long>(v[1]) : 0; });
    def(IP, "section", +[](id self, SEL) -> long { auto& v = ext_get<IndexPathExt>(self)->v; return v.empty() ? 0 : static_cast<long>(v[0]); });
    def(IP, "length", +[](id self, SEL) -> NSUInteger { return ext_get<IndexPathExt>(self)->v.size(); });
    def(IP, "indexAtPosition:", +[](id self, SEL, NSUInteger p) -> NSUInteger { auto& v = ext_get<IndexPathExt>(self)->v; return p < v.size() ? v[p] : NSNotFound; });
    def(IP, "isEqual:", +[](id self, SEL, id o) -> bool { return o && is_kind(o, "NSIndexPath") && ext_get<IndexPathExt>(self)->v == ext_get<IndexPathExt>(o)->v; });
    def(IP, "copyWithZone:", +[](id self, SEL, void*) -> id { return retain(self); });

    Class SD = def_class("NSSortDescriptor", "NSObject");
    defc(SD, "sortDescriptorWithKey:ascending:", +[](id, SEL, id k, bool a) -> id {
        id o = alloc(class_named("NSSortDescriptor")); auto* e = ext_get<SortExt>(o); e->key = send(k, "copy"); e->asc = a; return autorelease(o);
    });
    defc(SD, "sortDescriptorWithKey:ascending:selector:", +[](id, SEL, id k, bool a, SEL s) -> id {
        id o = alloc(class_named("NSSortDescriptor")); auto* e = ext_get<SortExt>(o); e->key = send(k, "copy"); e->asc = a; e->sel = s; return autorelease(o);
    });
    defc(SD, "sortDescriptorWithKey:ascending:comparator:", +[](id, SEL, id k, bool a, id b) -> id {
        id o = alloc(class_named("NSSortDescriptor")); auto* e = ext_get<SortExt>(o); e->key = send(k, "copy"); e->asc = a; e->block = block_copy(b); return autorelease(o);
    });
    def(SD, "initWithKey:ascending:", +[](id self, SEL, id k, bool a) -> id { auto* e = ext_get<SortExt>(self); e->key = send(k, "copy"); e->asc = a; return self; });
    def(SD, "initWithKey:ascending:selector:", +[](id self, SEL, id k, bool a, SEL s) -> id { auto* e = ext_get<SortExt>(self); e->key = send(k, "copy"); e->asc = a; e->sel = s; return self; });
    def(SD, "key", +[](id self, SEL) -> id { return ext_get<SortExt>(self)->key; });
    def(SD, "ascending", +[](id self, SEL) -> bool { return ext_get<SortExt>(self)->asc; });
    def(SD, "compareObject:toObject:", +[](id self, SEL, id a, id b) -> long {
        auto* e = ext_get<SortExt>(self);
        id va = e->key ? send(a, "valueForKeyPath:", e->key) : a;
        id vb = e->key ? send(b, "valueForKeyPath:", e->key) : b;
        long r;
        if (e->block) r = reinterpret_cast<long (*)(id, id, id)>(block_invoke_ptr(e->block))(e->block, va, vb);
        else r = send<long>(va, e->sel ? e->sel : sel("compare:"), vb);
        return e->asc ? r : -r;
    });
    def(SD, "reversedSortDescriptor", +[](id self, SEL) -> id {
        auto* e = ext_get<SortExt>(self);
        id o = alloc(class_named("NSSortDescriptor")); auto* x = ext_get<SortExt>(o); x->key = retain(e->key); x->asc = !e->asc; x->sel = e->sel; return autorelease(o);
    });

    Class UU = def_class("NSUUID", "NSObject");
    auto fill = [](id o) { auto* e = ext_get<UuidExt>(o); for (auto& b : e->b) b = static_cast<uint8_t>(arc4random()); e->b[6] = (e->b[6] & 0x0f) | 0x40; e->b[8] = (e->b[8] & 0x3f) | 0x80; };
    static decltype(fill) s_fill = fill;
    defc(UU, "UUID", +[](id, SEL) -> id { id o = alloc(class_named("NSUUID")); s_fill(o); return autorelease(o); });
    def(UU, "init", +[](id self, SEL) -> id { s_fill(self); return self; });
    def(UU, "initWithUUIDBytes:", +[](id self, SEL, const uint8_t* b) -> id { std::memcpy(ext_get<UuidExt>(self)->b, b, 16); return self; });
    def(UU, "initWithUUIDString:", +[](id self, SEL, id s) -> id {
        std::string h; for (char c : utf8(s)) if (isxdigit(static_cast<unsigned char>(c))) h.push_back(c);
        if (h.size() != 32) { release(self); return nullptr; }
        for (int i = 0; i < 16; ++i) ext_get<UuidExt>(self)->b[i] = static_cast<uint8_t>(std::stoi(h.substr(i * 2, 2), nullptr, 16));
        return self;
    });
    def(UU, "UUIDString", +[](id self, SEL) -> id {
        auto* e = ext_get<UuidExt>(self); char b[40]; char* p = b;
        for (int i = 0; i < 16; ++i) { p += snprintf(p, 3, "%02X", e->b[i]); if (i == 3 || i == 5 || i == 7 || i == 9) *p++ = '-'; }
        *p = 0;
        return str(b);
    });
    def(UU, "getUUIDBytes:", +[](id self, SEL, uint8_t* out) { std::memcpy(out, ext_get<UuidExt>(self)->b, 16); });
    def(UU, "copyWithZone:", +[](id self, SEL, void*) -> id { return retain(self); });

    Class VT = def_class("NSValueTransformer", "NSObject");
    static std::unordered_map<std::string, id>* transformers = new std::unordered_map<std::string, id>();
    defc(VT, "setValueTransformer:forName:", +[](id, SEL, id t, id n) { (*transformers)[utf8(n)] = retain(t); });
    defc(VT, "valueTransformerForName:", +[](id, SEL, id n) -> id { auto it = transformers->find(utf8(n)); return it == transformers->end() ? nullptr : it->second; });
    defc(VT, "allowsReverseTransformation", +[](id, SEL) -> bool { return false; });
    defc(VT, "transformedValueClass", +[](id, SEL) -> Class { return class_named("NSObject"); });
    def(VT, "transformedValue:", +[](id, SEL, id v) -> id { return v; });
    def(VT, "reverseTransformedValue:", +[](id, SEL, id v) -> id { return v; });

    // NSPredicate: evaluates to YES (filters are pass-through)
    Class PR = def_class("NSPredicate", "NSObject");
    defc(PR, "predicateWithFormat:", +[](id, SEL, id) -> id { return autorelease(alloc(class_named("NSPredicate"))); });
    defc(PR, "predicateWithFormat:argumentArray:", +[](id, SEL, id, id) -> id { return autorelease(alloc(class_named("NSPredicate"))); });
    defc(PR, "predicateWithBlock:", +[](id, SEL, id) -> id { return autorelease(alloc(class_named("NSPredicate"))); });
    defc(PR, "predicateWithValue:", +[](id, SEL, bool) -> id { return autorelease(alloc(class_named("NSPredicate"))); });
    def(PR, "evaluateWithObject:", +[](id, SEL, id) -> bool { return true; });
    def(A, "filteredArrayUsingPredicate:", +[](id self, SEL, id) -> id { return arr(arr_items(self)); });
    def(MA, "filterUsingPredicate:", +[](id, SEL, id) {});

    // NSRegularExpression on std::regex (ECMAScript)
    Class RE = def_class("NSRegularExpression", "NSObject");
    Class TC = def_class("NSTextCheckingResult", "NSObject");
    struct ReExt : Ext { std::regex re; std::string pattern; bool ok = false; };
    struct MatchExt : Ext { std::vector<NSRange> r; };
    defc(RE, "regularExpressionWithPattern:options:error:", +[](id, SEL, id p, NSUInteger opts, id* err) -> id {
        if (err) *err = nullptr;
        id o = alloc(class_named("NSRegularExpression"));
        auto* e = ext_get<ReExt>(o);
        e->pattern = utf8(p);
        try {
            e->re = std::regex(e->pattern, (opts & 1) ? std::regex::ECMAScript | std::regex::icase : std::regex::ECMAScript);
            e->ok = true;
        } catch (...) {
            log("NSRegularExpression: unsupported pattern %s", e->pattern.c_str());
        }
        return autorelease(o);
    });
    def(RE, "initWithPattern:options:error:", +[](id self, SEL, id p, NSUInteger opts, id* err) -> id {
        if (err) *err = nullptr;
        auto* e = ext_get<ReExt>(self);
        e->pattern = utf8(p);
        try { e->re = std::regex(e->pattern, (opts & 1) ? std::regex::ECMAScript | std::regex::icase : std::regex::ECMAScript); e->ok = true; } catch (...) {}
        return self;
    });
    auto matches = [](id self, id s, NSRange r) -> std::vector<id> {
        std::vector<id> out;
        auto* e = ext_get<ReExt>(self);
        if (!e->ok) return out;
        std::u16string u = u16(s);
        if (r.location > u.size()) return out;
        r.length = std::min(r.length, u.size() - r.location);
        // regex runs on UTF-8 for ASCII-compatible patterns; ranges are mapped back for ASCII text
        std::string t = to8(u.substr(r.location, r.length));
        for (auto it = std::sregex_iterator(t.begin(), t.end(), e->re); it != std::sregex_iterator(); ++it) {
            id m = autorelease(alloc(class_named("NSTextCheckingResult")));
            auto* me = ext_get<MatchExt>(m);
            for (size_t g = 0; g < it->size(); ++g)
                me->r.push_back((*it)[g].matched ? NSRange{r.location + static_cast<NSUInteger>(it->position(g)), static_cast<NSUInteger>(it->length(g))} : NSRange{NSNotFound, 0});
            out.push_back(m);
        }
        return out;
    };
    static decltype(matches) s_matches = matches;
    def(RE, "matchesInString:options:range:", +[](id self, SEL, id s, NSUInteger, NSRange r) -> id { return arr(s_matches(self, s, r)); });
    def(RE, "firstMatchInString:options:range:", +[](id self, SEL, id s, NSUInteger, NSRange r) -> id { auto v = s_matches(self, s, r); return v.empty() ? nullptr : v[0]; });
    def(RE, "numberOfMatchesInString:options:range:", +[](id self, SEL, id s, NSUInteger, NSRange r) -> NSUInteger { return s_matches(self, s, r).size(); });
    def(RE, "rangeOfFirstMatchInString:options:range:", +[](id self, SEL, id s, NSUInteger, NSRange r) -> NSRange {
        auto v = s_matches(self, s, r); return v.empty() ? NSRange{NSNotFound, 0} : ext_get<MatchExt>(v[0])->r[0];
    });
    def(RE, "stringByReplacingMatchesInString:options:range:withTemplate:", +[](id self, SEL, id s, NSUInteger, NSRange, id tmpl) -> id {
        auto* e = ext_get<ReExt>(self);
        if (!e->ok) return s;
        std::string t = utf8(tmpl);
        for (size_t i = 0; i + 1 < t.size(); ++i) if (t[i] == '$' && isdigit(static_cast<unsigned char>(t[i + 1]))) {}
        return str(std::regex_replace(utf8(s), e->re, t));
    });
    def(RE, "pattern", +[](id self, SEL) -> id { return str(ext_get<ReExt>(self)->pattern); });
    def(TC, "range", +[](id self, SEL) -> NSRange { auto& r = ext_get<MatchExt>(self)->r; return r.empty() ? NSRange{NSNotFound, 0} : r[0]; });
    def(TC, "rangeAtIndex:", +[](id self, SEL, NSUInteger i) -> NSRange { auto& r = ext_get<MatchExt>(self)->r; return i < r.size() ? r[i] : NSRange{NSNotFound, 0}; });
    def(TC, "numberOfRanges", +[](id self, SEL) -> NSUInteger { return ext_get<MatchExt>(self)->r.size(); });

    // NSMethodSignature / NSInvocation: enough for integer and object arguments
    Class MS = def_class("NSMethodSignature", "NSObject");
    struct SigExt : Ext { std::string types; };
    defc(MS, "signatureWithObjCTypes:", +[](id, SEL, const char* t) -> id { id o = alloc(class_named("NSMethodSignature")); ext_get<SigExt>(o)->types = t ? t : ""; return autorelease(o); });
    def(MS, "numberOfArguments", +[](id self, SEL) -> NSUInteger {
        std::string t = ext_get<SigExt>(self)->types; NSUInteger n = 0; bool first = true;
        for (size_t i = 0; i < t.size();) {
            if (isdigit(static_cast<unsigned char>(t[i]))) { ++i; continue; }
            if (t[i] == '{') { int d = 0; do { if (t[i] == '{') ++d; else if (t[i] == '}') --d; ++i; } while (i < t.size() && d); }
            else ++i;
            if (first) first = false; else ++n;
        }
        return n;
    });
    def(MS, "methodReturnLength", +[](id self, SEL) -> NSUInteger { return ext_get<SigExt>(self)->types.empty() || ext_get<SigExt>(self)->types[0] == 'v' ? 0 : 8; });
    def(MS, "methodReturnType", +[](id self, SEL) -> const char* { return ext_get<SigExt>(self)->types.c_str(); });
    Class O = class_named("NSObject");
    def(O, "methodSignatureForSelector:", +[](id self, SEL, SEL s) -> id {
        if (!responds(self, s)) return nullptr;
        id o = alloc(class_named("NSMethodSignature"));
        ext_get<SigExt>(o)->types = "@@:@@@@";
        return autorelease(o);
    });
    defc(O, "instanceMethodSignatureForSelector:", +[](id, SEL, SEL) -> id {
        id o = alloc(class_named("NSMethodSignature"));
        ext_get<SigExt>(o)->types = "@@:@@@@";
        return autorelease(o);
    });
    Class IV = def_class("NSInvocation", "NSObject");
    struct InvExt : Ext { id sig = nullptr, target = nullptr; SEL sel = nullptr; uint64_t args[8] = {0}; uint64_t ret = 0; bool retained = false; ~InvExt() override { release(sig); if (retained) release(target); } };
    defc(IV, "invocationWithMethodSignature:", +[](id, SEL, id sig) -> id { id o = alloc(class_named("NSInvocation")); ext_get<InvExt>(o)->sig = retain(sig); return autorelease(o); });
    def(IV, "setTarget:", +[](id self, SEL, id t) { ext_get<InvExt>(self)->target = t; });
    def(IV, "target", +[](id self, SEL) -> id { return ext_get<InvExt>(self)->target; });
    def(IV, "setSelector:", +[](id self, SEL, SEL s) { ext_get<InvExt>(self)->sel = s; });
    def(IV, "selector", +[](id self, SEL) -> SEL { return ext_get<InvExt>(self)->sel; });
    def(IV, "methodSignature", +[](id self, SEL) -> id { return ext_get<InvExt>(self)->sig; });
    def(IV, "setArgument:atIndex:", +[](id self, SEL, const void* p, long i) {
        auto* e = ext_get<InvExt>(self);
        if (i == 0) e->target = *static_cast<id const*>(p);
        else if (i == 1) e->sel = *static_cast<SEL const*>(p);
        else if (i - 2 < 8) std::memcpy(&e->args[i - 2], p, 8);
    });
    def(IV, "getArgument:atIndex:", +[](id self, SEL, void* p, long i) {
        auto* e = ext_get<InvExt>(self);
        if (i == 0) *static_cast<id*>(p) = e->target;
        else if (i == 1) *static_cast<SEL*>(p) = e->sel;
        else if (i - 2 < 8) std::memcpy(p, &e->args[i - 2], 8);
    });
    def(IV, "retainArguments", +[](id self, SEL) { auto* e = ext_get<InvExt>(self); if (!e->retained) { retain(e->target); e->retained = true; } });
    def(IV, "invoke", +[](id self, SEL) {
        auto* e = ext_get<InvExt>(self);
        auto* a = e->args;
        e->ret = send<uint64_t>(e->target, e->sel, a[0], a[1], a[2], a[3], a[4], a[5]);
    });
    def(IV, "invokeWithTarget:", +[](id self, SEL, id t) {
        auto* e = ext_get<InvExt>(self);
        auto* a = e->args;
        e->ret = send<uint64_t>(t, e->sel, a[0], a[1], a[2], a[3], a[4], a[5]);
    });
    def(IV, "getReturnValue:", +[](id self, SEL, void* p) { std::memcpy(p, &ext_get<InvExt>(self)->ret, 8); });
    def(IV, "setReturnValue:", +[](id self, SEL, const void* p) { std::memcpy(&ext_get<InvExt>(self)->ret, p, 8); });

    // NSStream family: memory/file streams that complete synchronously
    Class ST = def_class("NSStream", "NSObject");
    Class IN = def_class("NSInputStream", "NSStream");
    Class OUT = def_class("NSOutputStream", "NSStream");
    struct StreamExt : Ext { std::string buf; size_t pos = 0; std::string path; bool append = false, to_file = false, open = false; id delegate = nullptr; };
    def(ST, "open", +[](id self, SEL) { ext_get<StreamExt>(self)->open = true; });
    def(ST, "close", +[](id self, SEL) {
        auto* e = ext_get<StreamExt>(self);
        if (e->to_file && e->open) {
            FILE* f = fopen(remap_path(e->path).c_str(), e->append ? "ab" : "wb");
            if (f) { fwrite(e->buf.data(), 1, e->buf.size(), f); fclose(f); }
        }
        e->open = false;
    });
    def(ST, "setDelegate:", +[](id self, SEL, id d) { ext_get<StreamExt>(self)->delegate = d; });
    def(ST, "delegate", +[](id self, SEL) -> id { return ext_get<StreamExt>(self)->delegate; });
    def(ST, "scheduleInRunLoop:forMode:", +[](id, SEL, id, id) {});
    def(ST, "removeFromRunLoop:forMode:", +[](id, SEL, id, id) {});
    def(ST, "streamStatus", +[](id self, SEL) -> NSUInteger { return ext_get<StreamExt>(self)->open ? 2 : 0; });
    def(ST, "streamError", +[](id, SEL) -> id { return nullptr; });
    def(ST, "propertyForKey:", +[](id self, SEL, id k) -> id {
        auto* e = ext_get<StreamExt>(self);
        std::string key = utf8(k);
        if (key.find("DataWrittenToMemory") != std::string::npos) return autorelease(data_new(e->buf.data(), e->buf.size()));
        if (key.find("FileCurrentOffset") != std::string::npos) return num_u(e->pos);
        return nullptr;
    });
    def(ST, "setProperty:forKey:", +[](id, SEL, id, id) -> bool { return true; });
    defc(IN, "inputStreamWithData:", +[](id self, SEL, id d) -> id {
        id s = alloc(reinterpret_cast<Class>(self)); ext_get<StreamExt>(s)->buf.assign(reinterpret_cast<const char*>(data_bytes(d)), data_len(d)); return autorelease(s);
    });
    defc(IN, "inputStreamWithFileAtPath:", +[](id self, SEL, id p) -> id {
        id s = alloc(reinterpret_cast<Class>(self));
        if (!read_file(remap_path(utf8(p)), ext_get<StreamExt>(s)->buf)) { release(s); return nullptr; }
        return autorelease(s);
    });
    defc(IN, "inputStreamWithURL:", +[](id self, SEL, id u) -> id {
        id s = alloc(reinterpret_cast<Class>(self));
        if (!read_file(remap_path(utf8(send(u, "path"))), ext_get<StreamExt>(s)->buf)) { release(s); return nullptr; }
        return autorelease(s);
    });
    def(IN, "initWithData:", +[](id self, SEL, id d) -> id { ext_get<StreamExt>(self)->buf.assign(reinterpret_cast<const char*>(data_bytes(d)), data_len(d)); return self; });
    def(IN, "initWithFileAtPath:", +[](id self, SEL, id p) -> id { read_file(remap_path(utf8(p)), ext_get<StreamExt>(self)->buf); return self; });
    def(IN, "read:maxLength:", +[](id self, SEL, uint8_t* out, NSUInteger n) -> long {
        auto* e = ext_get<StreamExt>(self);
        size_t k = std::min<size_t>(n, e->buf.size() - std::min(e->pos, e->buf.size()));
        std::memcpy(out, e->buf.data() + e->pos, k);
        e->pos += k;
        return static_cast<long>(k);
    });
    def(IN, "hasBytesAvailable", +[](id self, SEL) -> bool { auto* e = ext_get<StreamExt>(self); return e->pos < e->buf.size(); });
    def(IN, "getBuffer:length:", +[](id, SEL, uint8_t**, NSUInteger*) -> bool { return false; });
    defc(OUT, "outputStreamToMemory", +[](id self, SEL) -> id { return autorelease(alloc(reinterpret_cast<Class>(self))); });
    defc(OUT, "outputStreamToFileAtPath:append:", +[](id self, SEL, id p, bool a) -> id {
        id s = alloc(reinterpret_cast<Class>(self)); auto* e = ext_get<StreamExt>(s); e->path = utf8(p); e->append = a; e->to_file = true; return autorelease(s);
    });
    defc(OUT, "outputStreamWithURL:append:", +[](id self, SEL, id u, bool a) -> id {
        id s = alloc(reinterpret_cast<Class>(self)); auto* e = ext_get<StreamExt>(s); e->path = utf8(send(u, "path")); e->append = a; e->to_file = true; return autorelease(s);
    });
    def(OUT, "initToMemory", +[](id self, SEL) -> id { return self; });
    def(OUT, "initToFileAtPath:append:", +[](id self, SEL, id p, bool a) -> id { auto* e = ext_get<StreamExt>(self); e->path = utf8(p); e->append = a; e->to_file = true; return self; });
    def(OUT, "initWithURL:append:", +[](id self, SEL, id u, bool a) -> id { auto* e = ext_get<StreamExt>(self); e->path = utf8(send(u, "path")); e->append = a; e->to_file = true; return self; });
    def(OUT, "write:maxLength:", +[](id self, SEL, const uint8_t* p, NSUInteger n) -> long { auto* e = ext_get<StreamExt>(self); e->buf.append(reinterpret_cast<const char*>(p), n); e->pos += n; return static_cast<long>(n); });
    def(OUT, "hasSpaceAvailable", +[](id, SEL) -> bool { return true; });

    def_class("NSMachPort", "NSObject");
    Class PO = def_class("NSPort", "NSObject");
    defc(PO, "port", +[](id self, SEL) -> id { return autorelease(alloc(reinterpret_cast<Class>(self))); });
    def(PO, "setDelegate:", +[](id, SEL, id) {});
    def(PO, "scheduleInRunLoop:forMode:", +[](id, SEL, id, id) {});
    def(PO, "invalidate", +[](id, SEL) {});

    Class MQ = def_class("NSMetadataQuery", "NSObject");
    def(MQ, "startQuery", +[](id, SEL) -> bool { return false; });
    def(MQ, "stopQuery", +[](id, SEL) {});
    def(MQ, "resultCount", +[](id, SEL) -> NSUInteger { return 0; });
    def(MQ, "results", +[](id, SEL) -> id { return arr({}); });
    for (const char* s : {"setPredicate:", "setSearchScopes:", "setSortDescriptors:", "disableUpdates", "enableUpdates"}) def(MQ, s, +[](id, SEL, id) {});
    Class FV = def_class("NSFileVersion", "NSObject");
    defc(FV, "currentVersionOfItemAtURL:", +[](id, SEL, id) -> id { return nullptr; });
    defc(FV, "otherVersionsOfItemAtURL:", +[](id, SEL, id) -> id { return arr({}); });
    defc(FV, "unresolvedConflictVersionsOfItemAtURL:", +[](id, SEL, id) -> id { return arr({}); });
    defc(FV, "removeOtherVersionsOfItemAtURL:error:", +[](id, SEL, id, id* err) -> bool { if (err) *err = nullptr; return true; });
}

// ---------------------------------------------------------------------------------------------------------------
// Grand Central Dispatch: app mode owns all of GCD so queued blocks reach the app run loop / worker threads.
// Queues are small heap objects; the main queue and the global concurrent queues are singletons.
// ---------------------------------------------------------------------------------------------------------------
namespace {
struct Queue {
    bool main = false, concurrent = false;
    std::mutex mu;                       // serial queues run one block at a time
    std::deque<std::function<void()>> pending;
    bool worker = false;
    std::string label;
};
Queue g_main_q_obj{true, false};
Queue g_global_q_obj{false, true};
struct DispatchGroup { std::atomic<long> count{0}; std::mutex mu; std::condition_variable cv; };
constexpr uint64_t kDispatchTimeNow = 0, kDispatchTimeForever = ~0ull;

void queue_run_serial(Queue* q) {
    for (;;) {
        std::function<void()> fn;
        {
            std::lock_guard<std::mutex> l(q->mu);
            if (q->pending.empty()) { q->worker = false; return; }
            fn = q->pending.front();
            q->pending.pop_front();
        }
        void* p = pool_push();
        fn();
        pool_pop(p);
    }
}
void queue_async(Queue* q, std::function<void()> fn) {
    if (!q || q->main) { post_main(std::move(fn)); return; }
    if (q->concurrent) { spawn_thread(std::move(fn), "dispatch"); return; }
    std::lock_guard<std::mutex> l(q->mu);
    q->pending.push_back(std::move(fn));
    if (!q->worker) { q->worker = true; spawn_thread([q] { queue_run_serial(q); }, q->label.empty() ? "dispatch" : q->label.c_str()); }
}
}  // namespace

extern "C" {
static void* gcd_main_queue() { return &g_main_q_obj; }
static void* gcd_get_global_queue(long, unsigned long) { return &g_global_q_obj; }
static void* gcd_get_current_queue() { return on_main_thread() ? static_cast<void*>(&g_main_q_obj) : static_cast<void*>(&g_global_q_obj); }
static const char* gcd_queue_get_label(void* q) { Queue* qq = static_cast<Queue*>(q); return qq && !qq->label.empty() ? qq->label.c_str() : ""; }
static void* gcd_queue_create(const char* label, void*) { Queue* q = new Queue(); q->label = label ? label : ""; return q; }
static uint64_t gcd_time(uint64_t base, int64_t delta) { return (base == kDispatchTimeForever ? kDispatchTimeForever : (base ? base : static_cast<uint64_t>(now_s() * 1e9)) + delta); }
static uint64_t gcd_walltime(const void*, int64_t delta) { return static_cast<uint64_t>(now_s() * 1e9) + delta; }
static void gcd_async(void* q, id block) {
    id b = block_copy(block);
    queue_async(static_cast<Queue*>(q), [b] { call_block0(b); block_release(b); });
}
static void gcd_async_f(void* q, void* ctx, void (*fn)(void*)) { queue_async(static_cast<Queue*>(q), [ctx, fn] { fn(ctx); }); }
static void gcd_sync(void* q, id block) {
    Queue* qq = static_cast<Queue*>(q);
    if (!qq || (qq->main && on_main_thread()) || qq->concurrent) { call_block0(block); return; }
    if (qq->main) { run_on_main_sync([block] { call_block0(block); }); return; }
    std::mutex m; std::condition_variable cv; bool done = false;
    queue_async(qq, [&] { call_block0(block); std::lock_guard<std::mutex> l(m); done = true; cv.notify_one(); });
    std::unique_lock<std::mutex> l(m);
    cv.wait(l, [&] { return done; });
}
static void gcd_sync_f(void* q, void* ctx, void (*fn)(void*)) {
    Queue* qq = static_cast<Queue*>(q);
    if (!qq || (qq->main && on_main_thread()) || qq->concurrent) { fn(ctx); return; }
    std::mutex m; std::condition_variable cv; bool done = false;
    queue_async(qq, [&] { fn(ctx); std::lock_guard<std::mutex> l(m); done = true; cv.notify_one(); });
    std::unique_lock<std::mutex> l(m);
    cv.wait(l, [&] { return done; });
}
static void gcd_after(uint64_t when, void* q, id block) {
    double delay = when > static_cast<uint64_t>(now_s() * 1e9) ? (when - now_s() * 1e9) / 1e9 : 0;
    id b = block_copy(block);
    Queue* qq = static_cast<Queue*>(q);
    post_main([b, qq, delay] { queue_async(qq && !qq->main ? qq : nullptr, [b] { call_block0(b); block_release(b); }); (void)delay; }, delay);
}
static void gcd_after_f(uint64_t when, void* q, void* ctx, void (*fn)(void*)) {
    double delay = when > static_cast<uint64_t>(now_s() * 1e9) ? (when - now_s() * 1e9) / 1e9 : 0;
    post_main([q, ctx, fn] { queue_async(static_cast<Queue*>(q), [ctx, fn] { fn(ctx); }); }, delay);
}
static void* gcd_group_create() { return new DispatchGroup(); }
static void gcd_group_enter(void* g) { static_cast<DispatchGroup*>(g)->count.fetch_add(1); }
static void gcd_group_leave(void* g) {
    auto* grp = static_cast<DispatchGroup*>(g);
    if (grp->count.fetch_sub(1) == 1) { std::lock_guard<std::mutex> l(grp->mu); grp->cv.notify_all(); }
}
static void gcd_group_async(void* g, void* q, id block) {
    auto* grp = static_cast<DispatchGroup*>(g);
    grp->count.fetch_add(1);
    id b = block_copy(block);
    queue_async(static_cast<Queue*>(q), [grp, b] { call_block0(b); block_release(b); if (grp->count.fetch_sub(1) == 1) { std::lock_guard<std::mutex> l(grp->mu); grp->cv.notify_all(); } });
}
static long gcd_group_wait(void* g, uint64_t timeout) {
    auto* grp = static_cast<DispatchGroup*>(g);
    std::unique_lock<std::mutex> l(grp->mu);
    if (timeout == kDispatchTimeForever) { grp->cv.wait(l, [grp] { return grp->count.load() == 0; }); return 0; }
    double secs = timeout > static_cast<uint64_t>(now_s() * 1e9) ? (timeout - now_s() * 1e9) / 1e9 : 0;
    return grp->cv.wait_for(l, std::chrono::microseconds(static_cast<int64_t>(secs * 1e6)), [grp] { return grp->count.load() == 0; }) ? 0 : 1;
}
static void gcd_group_notify(void* g, void* q, id block) {
    auto* grp = static_cast<DispatchGroup*>(g);
    id b = block_copy(block);
    Queue* qq = static_cast<Queue*>(q);
    spawn_thread([grp, qq, b] {
        { std::unique_lock<std::mutex> l(grp->mu); grp->cv.wait(l, [grp] { return grp->count.load() == 0; }); }
        queue_async(qq, [b] { call_block0(b); block_release(b); });
    }, "dispatch-notify");
}
static void gcd_once(long* pred, id block) { if (__sync_bool_compare_and_swap(pred, 0, 1)) call_block0(block); }
static void gcd_once_f(long* pred, void* ctx, void (*fn)(void*)) { if (__sync_bool_compare_and_swap(pred, 0, 1)) fn(ctx); }
static void gcd_retain(void*) {}
static void gcd_release(void*) {}
static void gcd_resume(void*) {}
static void gcd_suspend(void*) {}
static void gcd_set_target(void*, void*) {}
static void gcd_apply(size_t n, void* q, id block) {
    auto fn = reinterpret_cast<void (*)(id, size_t)>(block_invoke_ptr(block));
    (void)q;
    for (size_t i = 0; i < n; ++i) fn(block, i);
}
static void* gcd_sema_create(long v) { auto* s = new std::pair<std::mutex, std::condition_variable>(); (void)v; return s; }
}  // extern "C"

void register_gcd_symbols(std::unordered_map<std::string, void*>& m) {
    auto r = [&](const char* n, auto f) { m[n] = reinterpret_cast<void*>(f); };
    r("_dispatch_get_global_queue", &gcd_get_global_queue);
    r("_dispatch_get_current_queue", &gcd_get_current_queue);
    r("_dispatch_queue_get_label", &gcd_queue_get_label);
    r("_dispatch_queue_create", &gcd_queue_create);
    r("_dispatch_time", &gcd_time);
    r("_dispatch_walltime", &gcd_walltime);
    r("_dispatch_async", &gcd_async);
    r("_dispatch_async_f", &gcd_async_f);
    r("_dispatch_sync", &gcd_sync);
    r("_dispatch_sync_f", &gcd_sync_f);
    r("_dispatch_after", &gcd_after);
    r("_dispatch_after_f", &gcd_after_f);
    r("_dispatch_apply", &gcd_apply);
    r("_dispatch_group_create", &gcd_group_create);
    r("_dispatch_group_enter", &gcd_group_enter);
    r("_dispatch_group_leave", &gcd_group_leave);
    r("_dispatch_group_async", &gcd_group_async);
    r("_dispatch_group_wait", &gcd_group_wait);
    r("_dispatch_group_notify", &gcd_group_notify);
    r("_dispatch_once", &gcd_once);
    r("_dispatch_once_f", &gcd_once_f);
    r("_dispatch_retain", &gcd_retain);
    r("_dispatch_release", &gcd_release);
    r("_dispatch_resume", &gcd_resume);
    r("_dispatch_suspend", &gcd_suspend);
    r("_dispatch_set_target_queue", &gcd_set_target);
    static void* main_q = &g_main_q_obj;
    m["__dispatch_main_q"] = &main_q;   // dispatch_get_main_queue() reads &_dispatch_main_q
    static uint64_t concurrent_attr = 2;
    m["__dispatch_queue_attr_concurrent"] = &concurrent_attr;
}

// ---------------------------------------------------------------------------------------------------------------
// NSStringFromClass / selectors / search paths / uncaught handler
// ---------------------------------------------------------------------------------------------------------------
extern "C" {
static id fn_NSStringFromClass(Class c) { return c ? str(class_name(c)) : nullptr; }
static Class fn_NSClassFromString(id s) { return s ? class_named(utf8(s).c_str()) : 0; }
static id fn_NSStringFromSelector(SEL s) { return s ? str(s) : nullptr; }
static SEL fn_NSSelectorFromString(id s) { return s ? sel(utf8(s).c_str()) : nullptr; }
static id fn_NSStringFromProtocol(uintptr_t p) { return p ? str(reinterpret_cast<const char*>(*reinterpret_cast<uintptr_t*>(p + 8))) : nullptr; }
static id fn_NSSearchPathForDirectoriesInDomains(long dir, unsigned long, bool) {
    const char* sub = dir == 9 ? "Documents" : dir == 5 ? "Library" : dir == 13 ? "Library/Caches" : dir == 14 ? "Library/Application Support" : nullptr;
    return sub ? arr({str(sandbox_dir(sub))}) : arr({});
}
static id fn_NSTemporaryDirectory() { const char* t = getenv("LOADER_TMP_DIR"); return str(t ? t : (sandbox_dir("tmp"))); }
static id fn_NSHomeDirectory() { return str(sandbox_dir("")); }
static id fn_NSUserName() { return str("mobile"); }
static id fn_NSFullUserName() { return str("Mobile User"); }
static void* g_uncaught_handler = nullptr;
static void* fn_NSGetUncaughtExceptionHandler() { return g_uncaught_handler; }
static void fn_NSSetUncaughtExceptionHandler(void* h) { g_uncaught_handler = h; }
static double fn_NSTimeIntervalSince1970() { return 0; }
}

void register_foundation2_symbols(std::unordered_map<std::string, void*>& m) {
    auto r = [&](const char* n, auto f) { m[n] = reinterpret_cast<void*>(f); };
    r("_NSStringFromClass", &fn_NSStringFromClass);
    r("_NSClassFromString", &fn_NSClassFromString);
    r("_NSStringFromSelector", &fn_NSStringFromSelector);
    r("_NSSelectorFromString", &fn_NSSelectorFromString);
    r("_NSStringFromProtocol", &fn_NSStringFromProtocol);
    r("_NSSearchPathForDirectoriesInDomains", &fn_NSSearchPathForDirectoriesInDomains);
    r("_NSTemporaryDirectory", &fn_NSTemporaryDirectory);
    r("_NSHomeDirectory", &fn_NSHomeDirectory);
    r("_NSUserName", &fn_NSUserName);
    r("_NSFullUserName", &fn_NSFullUserName);
    r("_NSGetUncaughtExceptionHandler", &fn_NSGetUncaughtExceptionHandler);
    r("_NSSetUncaughtExceptionHandler", &fn_NSSetUncaughtExceptionHandler);
    register_gcd_symbols(m);
}

void init_foundation2() {
    init_date();
    init_timers();
    init_kvc();
    init_notifications();
    init_defaults();
    init_bundle();
    init_errors();
    init_threads();
    init_files();
    init_locale();
    init_scanner();
    init_xml();
    init_serialization();
    init_operations();
    init_url_loading();
    init_values();
}

}  // namespace app
