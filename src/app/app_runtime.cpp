// App-mode Objective-C runtime: classes (guest + native), selectors, dispatch, reference counting, autorelease
// pools, weak references, associated objects, blocks and the objc_* / class_* C API the guest imports.
#include "app.hpp"

#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdlib>
#include <set>
#include <shared_mutex>
#include <unordered_set>

extern "C" void app_objc_msgSend();
extern "C" void app_objc_msgSendSuper2();
extern "C" void app_objc_msgSendSuper();
extern "C" void app_nil_imp();
extern "C" void app_forward_thunk();

namespace app {

// ---------------------------------------------------------------------------------------------------------------
// logging
// ---------------------------------------------------------------------------------------------------------------
void log(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[APP] %s\n", buf);
}

bool trace_enabled(const char* what) {
    static const char* t = getenv("LOADER_APP_TRACE");
    if (!t) return false;
    return std::strstr(t, what) != nullptr || std::strcmp(t, "all") == 0;
}

// ---------------------------------------------------------------------------------------------------------------
// image range
// ---------------------------------------------------------------------------------------------------------------
static uintptr_t g_img_lo = 0, g_img_hi = 0;
void set_image_range(uintptr_t lo, uintptr_t hi) { g_img_lo = lo; g_img_hi = hi; }
bool in_image(const void* p) { auto a = reinterpret_cast<uintptr_t>(p); return a >= g_img_lo && a < g_img_hi; }
uintptr_t image_base() { return g_img_lo; }

// ---------------------------------------------------------------------------------------------------------------
// selectors
// ---------------------------------------------------------------------------------------------------------------
static std::mutex g_sel_mu;
static std::unordered_map<std::string, const char*>* g_sels;
SEL sel(const char* name) {
    if (!name) return nullptr;
    std::lock_guard<std::mutex> l(g_sel_mu);
    if (!g_sels) g_sels = new std::unordered_map<std::string, const char*>();
    auto it = g_sels->find(name);
    if (it != g_sels->end()) return it->second;
    const char* c = in_image(name) ? name : strdup(name);
    g_sels->emplace(c, c);
    return c;
}

// ---------------------------------------------------------------------------------------------------------------
// classes
// ---------------------------------------------------------------------------------------------------------------
struct class_t { uintptr_t isa, superclass, cache, vtable, data; };
struct class_ro_t {
    uint32_t flags, instanceStart, instanceSize, reserved;
    uintptr_t ivarLayout;
    const char* name;
    uintptr_t baseMethods, baseProtocols, ivars, weakIvarLayout, baseProperties;
};
struct MethodEntry { SEL name; const char* types; IMP imp; };

constexpr uint64_t kClassMagic = 0x5243'4c41'5353'2121ull;   // "RCLASS!!"

struct Cache {
    uint32_t mask, count, epoch;
    struct Slot { std::atomic<SEL> sel; std::atomic<IMP> imp; };
    Slot slots[1];
};

struct RClass {
    uint64_t magic = kClassMagic;
    std::string name;
    Class cls = 0;
    RClass* super = nullptr;
    RClass* meta = nullptr;          // class -> its metaclass
    RClass* instance = nullptr;      // metaclass -> its class
    bool is_meta = false, native = false, zombie = false;
    uint32_t inst_size = 8;
    std::unordered_map<SEL, MethodEntry*> methods;
    std::vector<uintptr_t> protocols;             // guest protocol_t addresses
    std::unordered_set<std::string> proto_names;  // native conformances
    std::atomic<Cache*> cache{nullptr};
    std::atomic<int> init_state{0};               // 0 none, 1 running, 2 done
    pthread_t init_thread{};
    uintptr_t ro = 0;                             // guest class_ro_t
};

static std::recursive_mutex g_rt;
static std::unordered_map<uintptr_t, RClass*>* g_by_addr;
static std::unordered_map<std::string, RClass*>* g_by_name;
static std::atomic<uint32_t> g_epoch{1};
static RClass* g_nsobject = nullptr;
static RClass* g_zombie = nullptr;

static std::unordered_map<uintptr_t, RClass*>& by_addr() { if (!g_by_addr) g_by_addr = new std::unordered_map<uintptr_t, RClass*>(); return *g_by_addr; }
static std::unordered_map<std::string, RClass*>& by_name() { if (!g_by_name) g_by_name = new std::unordered_map<std::string, RClass*>(); return *g_by_name; }

static RClass* rc_of_class(Class c) {
    // Reject null, misaligned and low (tagged-pointer / small-integer) values before dereferencing: a class
    // pointer is always 8-byte aligned and well above the first page.
    if (c < 0x1000 || (c & 7)) return nullptr;
    auto* ct = reinterpret_cast<class_t*>(c);
    auto* rc = reinterpret_cast<RClass*>(ct->cache);
    if (rc && reinterpret_cast<uintptr_t>(rc) >= 0x1000 && (reinterpret_cast<uintptr_t>(rc) & 7) == 0 && rc->magic == kClassMagic && rc->cls == c) return rc;
    std::lock_guard<std::recursive_mutex> l(g_rt);
    auto it = by_addr().find(c);
    return it == by_addr().end() ? nullptr : it->second;
}

Class class_named(const char* name) {
    std::lock_guard<std::recursive_mutex> l(g_rt);
    auto it = by_name().find(name);
    return it == by_name().end() ? 0 : it->second->cls;
}
Class class_of(id o) { return o ? o->isa : 0; }
Class superclass_of(Class c) { RClass* r = rc_of_class(c); return r && r->super ? r->super->cls : 0; }
const char* class_name(Class c) { RClass* r = rc_of_class(c); return r ? r->name.c_str() : "?"; }
bool is_meta(Class c) { RClass* r = rc_of_class(c); return r && r->is_meta; }
bool is_class_object(id o) {
    if (!o) return false;
    RClass* r = rc_of_class(reinterpret_cast<Class>(o));
    return r != nullptr;
}
Class metaclass_of(Class c) { RClass* r = rc_of_class(c); return r && r->meta ? r->meta->cls : 0; }
bool is_subclass(Class c, Class of) {
    for (RClass* r = rc_of_class(c); r; r = r->super)
        if (r->cls == of) return true;
    return false;
}
bool is_kind(id o, Class of) { return o && is_subclass(o->isa, of); }
bool is_kind(id o, const char* n) { Class c = class_named(n); return c && is_kind(o, c); }
uint32_t instance_size(Class c) { RClass* r = rc_of_class(c); return r ? r->inst_size : 8; }
IMP nil_imp() { return reinterpret_cast<IMP>(&app_nil_imp); }

static class_t* new_class_struct(const char* name, uint32_t size) {
    // classes get a header too (immortal), so has_header()/retain treat them uniformly
    auto* mem = static_cast<uint8_t*>(calloc(1, 16 + sizeof(class_t) + sizeof(class_ro_t)));
    *reinterpret_cast<uint32_t*>(mem) = 0x0b1ec7ed;
    reinterpret_cast<std::atomic<int32_t>*>(mem + 4)->store(1 << 30);
    auto* ct = reinterpret_cast<class_t*>(mem + 16);
    auto* ro = reinterpret_cast<class_ro_t*>(mem + 16 + sizeof(class_t));
    ro->name = strdup(name);
    ro->instanceStart = 8;
    ro->instanceSize = size;
    ct->data = reinterpret_cast<uintptr_t>(ro);
    return ct;
}

static void invalidate_caches() { g_epoch.fetch_add(1); }

static void add_method_locked(RClass* r, SEL s, IMP imp, const char* types) {
    auto it = r->methods.find(s);
    if (it != r->methods.end()) {
        it->second->imp = imp;
        if (types) it->second->types = types;
    } else {
        r->methods[s] = new MethodEntry{s, types ? types : "", imp};
    }
}

Class def_class(const char* name, const char* super_name, uint32_t size) {
    std::lock_guard<std::recursive_mutex> l(g_rt);
    auto existing = by_name().find(name);
    if (existing != by_name().end()) return existing->second->cls;
    RClass* sup = nullptr;
    if (super_name) {
        auto it = by_name().find(super_name);
        if (it == by_name().end()) { log("def_class %s: unknown superclass %s", name, super_name); abort(); }
        sup = it->second;
    }
    class_t* c = new_class_struct(name, size);
    class_t* m = new_class_struct(name, 40);
    auto* rc = new RClass();
    auto* rm = new RClass();
    rc->name = rm->name = name;
    rc->cls = reinterpret_cast<Class>(c);
    rm->cls = reinterpret_cast<Class>(m);
    rc->native = rm->native = true;
    rm->is_meta = true;
    rc->meta = rm;
    rm->instance = rc;
    rc->inst_size = sup && sup->inst_size > size ? sup->inst_size : size;
    rc->super = sup;
    rc->init_state = rm->init_state = 2;
    if (sup) {
        rm->super = sup->meta;
    } else {
        rm->super = rc;   // root metaclass -> root class
    }
    c->isa = rm->cls;
    c->superclass = sup ? sup->cls : 0;
    c->cache = reinterpret_cast<uintptr_t>(rc);
    m->isa = sup ? reinterpret_cast<class_t*>(sup->meta->cls)->isa : rm->cls;   // every metaclass's isa = root metaclass
    if (!sup) m->isa = rm->cls;
    m->superclass = rm->super->cls;
    m->cache = reinterpret_cast<uintptr_t>(rm);
    by_addr()[rc->cls] = rc;
    by_addr()[rm->cls] = rm;
    by_name()[name] = rc;
    if (!g_nsobject && std::strcmp(name, "NSObject") == 0) g_nsobject = rc;
    return rc->cls;
}

void def(Class c, const char* s, IMP imp) {
    std::lock_guard<std::recursive_mutex> l(g_rt);
    RClass* r = rc_of_class(c);
    if (!r) return;
    add_method_locked(r, sel(s), imp, nullptr);
    invalidate_caches();
}
void defc(Class c, const char* s, IMP imp) {
    std::lock_guard<std::recursive_mutex> l(g_rt);
    RClass* r = rc_of_class(c);
    if (!r || !r->meta) return;
    add_method_locked(r->meta, sel(s), imp, nullptr);
    invalidate_caches();
}

// ---------------------------------------------------------------------------------------------------------------
// objects
// ---------------------------------------------------------------------------------------------------------------
struct Header { uint32_t magic; std::atomic<int32_t> rc; Ext* ext; };
static_assert(sizeof(Header) == 16, "header");
constexpr uint32_t kObjMagic = 0xa110c8ed;
constexpr uint32_t kClsMagic = 0x0b1ec7ed;
constexpr int32_t kImmortal = 1 << 30;
constexpr int32_t kDeallocating = 1 << 29;

static inline Header* hdr(id o) { return reinterpret_cast<Header*>(o) - 1; }
bool has_header(id o) {
    if (!o || in_image(o)) return false;
    if (reinterpret_cast<uintptr_t>(o) & 7) return false;
    uint32_t m = hdr(o)->magic;
    return m == kObjMagic || m == kClsMagic;
}
Ext*& ext_slot(id o) { return hdr(o)->ext; }

static bool g_no_free = false;
static bool g_zombies = false;

id alloc(Class c) {
    RClass* r = rc_of_class(c);
    size_t size = r ? r->inst_size : 8;
    if (size < 8) size = 8;
    auto* mem = static_cast<uint8_t*>(calloc(1, sizeof(Header) + size));
    auto* h = reinterpret_cast<Header*>(mem);
    h->magic = kObjMagic;
    h->rc.store(1, std::memory_order_relaxed);
    id o = reinterpret_cast<id>(mem + sizeof(Header));
    o->isa = c;
    return o;
}

id retain(id o) {
    if (!has_header(o)) return o;
    Header* h = hdr(o);
    if (h->rc.load(std::memory_order_relaxed) >= kDeallocating) return o;
    h->rc.fetch_add(1, std::memory_order_relaxed);
    return o;
}

void release(id o) {
    if (!has_header(o)) return;
    Header* h = hdr(o);
    int32_t v = h->rc.load(std::memory_order_relaxed);
    if (v >= kDeallocating) return;
    if (h->rc.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        h->rc.store(kDeallocating);
        send<void>(o, "dealloc");
    }
}

int32_t retain_count(id o) {
    if (!has_header(o)) return INT32_MAX;
    int32_t v = hdr(o)->rc.load();
    return v >= kDeallocating ? INT32_MAX : v;
}

// associated objects and weak references, cleared at dealloc
static std::mutex g_side_mu;
struct Assoc { uintptr_t policy; id value; };
static std::unordered_map<id, std::unordered_map<const void*, Assoc>>* g_assoc;
static std::unordered_map<id, std::set<id*>>* g_weak;
static std::unordered_map<id, std::recursive_mutex*>* g_sync;

static void clear_side_tables(id o) {
    std::vector<id> release_later;
    {
        std::lock_guard<std::mutex> l(g_side_mu);
        if (g_assoc) {
            auto it = g_assoc->find(o);
            if (it != g_assoc->end()) {
                for (auto& kv : it->second)
                    if (kv.second.policy & 3) release_later.push_back(kv.second.value);
                g_assoc->erase(it);
            }
        }
        if (g_weak) {
            auto it = g_weak->find(o);
            if (it != g_weak->end()) {
                for (id* loc : it->second)
                    if (*loc == o) *loc = nullptr;
                g_weak->erase(it);
            }
        }
    }
    for (id v : release_later) release(v);
}

static std::vector<void (*)(id)> g_dealloc_hooks;
void add_dealloc_hook(void (*hook)(id)) { g_dealloc_hooks.push_back(hook); }

void dealloc_object(id o) {
    if (!has_header(o)) return;
    Header* h = hdr(o);
    if (h->magic == kClsMagic) return;
    for (auto hook : g_dealloc_hooks) hook(o);
    clear_side_tables(o);
    Ext* e = h->ext;
    h->ext = nullptr;
    delete e;
    if (g_zombies && g_zombie) {
        h->ext = reinterpret_cast<Ext*>(o->isa);   // remembers the class for the zombie report
        o->isa = g_zombie->cls;
        return;
    }
    if (g_no_free) return;
    h->magic = 0xdeadbeef;
    free(h);
}

// ---------------------------------------------------------------------------------------------------------------
// autorelease pools (per thread)
// ---------------------------------------------------------------------------------------------------------------
static thread_local std::vector<id>* t_pool;
static std::vector<id>& pool() { if (!t_pool) t_pool = new std::vector<id>(); return *t_pool; }
id autorelease(id o) {
    if (!o) return o;
    if (!has_header(o)) return o;
    pool().push_back(o);
    return o;
}
void* pool_push() { return reinterpret_cast<void*>(pool().size() + 1); }
void pool_pop(void* token) {
    size_t mark = reinterpret_cast<size_t>(token);
    if (mark) --mark;
    auto& p = pool();
    while (p.size() > mark) {
        id o = p.back();
        p.pop_back();
        release(o);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// method lookup
// ---------------------------------------------------------------------------------------------------------------
static Cache* new_cache(uint32_t cap) {
    auto* c = static_cast<Cache*>(calloc(1, sizeof(Cache) + sizeof(Cache::Slot) * cap));
    c->mask = cap - 1;
    c->epoch = g_epoch.load();
    return c;
}

static inline IMP cache_find(RClass* r, SEL s) {
    Cache* c = r->cache.load(std::memory_order_acquire);
    if (!c || c->epoch != g_epoch.load(std::memory_order_relaxed)) return nullptr;
    uint32_t i = (reinterpret_cast<uintptr_t>(s) >> 3) & c->mask;
    for (uint32_t n = 0; n <= c->mask; ++n, i = (i + 1) & c->mask) {
        SEL k = c->slots[i].sel.load(std::memory_order_acquire);
        if (k == s) return c->slots[i].imp.load(std::memory_order_relaxed);
        if (!k) return nullptr;
    }
    return nullptr;
}

static void cache_insert_locked(RClass* r, SEL s, IMP imp) {
    Cache* c = r->cache.load();
    if (!c || c->epoch != g_epoch.load()) {
        c = new_cache(64);
        r->cache.store(c, std::memory_order_release);   // the old table is leaked: readers may still be in it
    }
    if ((c->count + 1) * 2 > c->mask + 1) {
        Cache* bigger = new_cache((c->mask + 1) * 2);
        for (uint32_t i = 0; i <= c->mask; ++i) {
            SEL k = c->slots[i].sel.load();
            if (!k) continue;
            uint32_t j = (reinterpret_cast<uintptr_t>(k) >> 3) & bigger->mask;
            while (bigger->slots[j].sel.load()) j = (j + 1) & bigger->mask;
            bigger->slots[j].imp.store(c->slots[i].imp.load());
            bigger->slots[j].sel.store(k);
            bigger->count++;
        }
        c = bigger;
        r->cache.store(c, std::memory_order_release);
    }
    uint32_t i = (reinterpret_cast<uintptr_t>(s) >> 3) & c->mask;
    while (true) {
        SEL k = c->slots[i].sel.load();
        if (k == s) { c->slots[i].imp.store(imp); return; }
        if (!k) break;
        i = (i + 1) & c->mask;
    }
    c->slots[i].imp.store(imp, std::memory_order_relaxed);
    c->slots[i].sel.store(s, std::memory_order_release);
    c->count++;
}

static MethodEntry* find_method_locked(RClass* r, SEL s) {
    for (; r; r = r->super) {
        auto it = r->methods.find(s);
        if (it != r->methods.end()) return it->second;
    }
    return nullptr;
}

// +initialize: superclasses first, once per class, before the class (or an instance) receives its first message
static void ensure_initialized(RClass* cls) {
    if (!cls || cls->init_state.load(std::memory_order_acquire) == 2) return;
    if (cls->super) ensure_initialized(cls->super);
    {
        std::lock_guard<std::recursive_mutex> l(g_rt);
        int st = cls->init_state.load();
        if (st == 2) return;
        if (st == 1) {
            if (pthread_equal(cls->init_thread, pthread_self())) return;   // re-entrant message during +initialize
        } else {
            cls->init_state = 1;
            cls->init_thread = pthread_self();
            goto run;
        }
    }
    while (cls->init_state.load() != 2) usleep(100);
    return;
run:
    if (trace_enabled("init")) log("+[%s initialize]", cls->name.c_str());
    send<void>(reinterpret_cast<id>(cls->cls), "initialize");
    cls->init_state.store(2, std::memory_order_release);
}

struct MissKey { RClass* r; SEL s; bool operator==(const MissKey& o) const { return r == o.r && s == o.s; } };
struct MissHash { size_t operator()(const MissKey& k) const { return std::hash<void*>()(k.r) ^ std::hash<const void*>()(k.s); } };
static std::unordered_set<MissKey, MissHash>* g_misses;

static thread_local id t_forward_target;

static IMP slow_lookup(RClass* r, SEL s, id obj, bool forward) {
    IMP imp = nullptr;
    {
        std::lock_guard<std::recursive_mutex> l(g_rt);
        if (MethodEntry* m = find_method_locked(r, s)) {
            imp = m->imp;
            cache_insert_locked(r, s, imp);
            return imp;
        }
    }
    if (!forward) return nullptr;
    // forwardingTargetForSelector: overridden by a guest class
    static SEL s_fwd = sel("forwardingTargetForSelector:");
    if (s != s_fwd && !r->zombie) {
        MethodEntry* m;
        {
            std::lock_guard<std::recursive_mutex> l(g_rt);
            m = find_method_locked(r, s_fwd);
        }
        if (m && !rc_of_class(r->cls)->native && m->imp) {
            RClass* owner = nullptr;
            {
                std::lock_guard<std::recursive_mutex> l(g_rt);
                for (RClass* c = r; c; c = c->super)
                    if (c->methods.count(s_fwd)) { owner = c; break; }
            }
            if (owner && !owner->native) {
                id target = reinterpret_cast<id (*)(id, SEL, SEL)>(m->imp)(obj, s_fwd, s);
                if (target && target != obj) {
                    t_forward_target = target;
                    return reinterpret_cast<IMP>(&app_forward_thunk);
                }
            }
        }
    }
    {
        std::lock_guard<std::recursive_mutex> l(g_rt);
        if (!g_misses) g_misses = new std::unordered_set<MissKey, MissHash>();
        if (g_misses->insert({r, s}).second) {
            if (r->zombie) {
                log("*** message -%s sent to deallocated instance %p", s, static_cast<void*>(obj));
            } else {
                log("MISSING %c[%s %s]", r->is_meta ? '+' : '-', r->name.c_str(), s);
            }
        }
        cache_insert_locked(r, s, nil_imp());
    }
    return nil_imp();
}

static inline RClass* receiver_class(id obj) {
    RClass* r = rc_of_class(obj->isa);
    if (!r) {
        static std::atomic<int> bad{0};
        if (bad.fetch_add(1) < 20) log("bad isa %p for object %p", reinterpret_cast<void*>(obj->isa), static_cast<void*>(obj));
    }
    return r;
}

IMP lookup(id obj, SEL s) {
    if (!obj) return nil_imp();
    RClass* r = receiver_class(obj);
    if (!r) return nil_imp();
    RClass* init_cls = r->is_meta ? r->instance : r;
    if (init_cls && init_cls->init_state.load(std::memory_order_acquire) != 2) ensure_initialized(init_cls);
    if (IMP imp = cache_find(r, s)) return imp;
    return slow_lookup(r, s, obj, true);
}

IMP lookup_noforward(id obj, SEL s) {
    if (!obj) return nullptr;
    RClass* r = rc_of_class(obj->isa);
    if (!r) return nullptr;
    IMP imp = cache_find(r, s);
    if (imp == nil_imp()) return nullptr;
    if (imp) return imp;
    return slow_lookup(r, s, obj, false);
}

bool responds(id obj, SEL s) { return lookup_noforward(obj, s) != nullptr; }

IMP lookup_super(Class from, SEL s) {
    RClass* r = rc_of_class(from);
    if (!r || !r->super) return nullptr;
    std::lock_guard<std::recursive_mutex> l(g_rt);
    MethodEntry* m = find_method_locked(r->super, s);
    return m ? m->imp : nullptr;
}

}  // namespace app

using namespace app;

extern "C" IMP app_lookup_c(id obj, SEL s) {
    if (trace_enabled("msg")) {
        RClass* r = rc_of_class(obj->isa);
        log("msg %c[%s %s]", r && r->is_meta ? '+' : '-', r ? r->name.c_str() : "?", s);
    }
    return lookup(obj, s);
}

// objc_super { receiver, current_class }: search starts at current_class's superclass
extern "C" IMP app_lookup_super2_c(uintptr_t* sup, SEL s) {
    RClass* r = rc_of_class(static_cast<Class>(sup[1]));
    if (!r) return nil_imp();
    RClass* start = r->super;
    if (!start) return nil_imp();
    if (IMP imp = cache_find(start, s)) return imp;
    return slow_lookup(start, s, reinterpret_cast<id>(sup[0]), true);
}

extern "C" IMP app_lookup_super_c(uintptr_t* sup, SEL s) {
    RClass* r = rc_of_class(static_cast<Class>(sup[1]));
    if (!r) return nil_imp();
    if (IMP imp = cache_find(r, s)) return imp;
    return slow_lookup(r, s, reinterpret_cast<id>(sup[0]), true);
}

extern "C" id app_take_forward_target() { id t = t_forward_target; t_forward_target = nullptr; return t; }

// ---------------------------------------------------------------------------------------------------------------
// guest image registration
// ---------------------------------------------------------------------------------------------------------------
namespace app {

static constexpr uintptr_t kPtrMask = 0x00007ffffffffff8ull;
static inline uintptr_t rd(uintptr_t a) { return *reinterpret_cast<uintptr_t*>(a); }
static inline uint32_t rd32(uintptr_t a) { return *reinterpret_cast<uint32_t*>(a); }
static inline int32_t rdi32(uintptr_t a) { return *reinterpret_cast<int32_t*>(a); }

static void read_method_list_locked(uintptr_t list, RClass* into) {
    if (!list) return;
    uint32_t ent = rd32(list), cnt = rd32(list + 4);
    bool rel = ent & 0x80000000u;
    uint32_t esz = rel ? 12 : (ent & 0xfffc);
    if (!rel && esz < 24) esz = 24;
    for (uint32_t i = 0; i < cnt; ++i) {
        uintptr_t e = list + 8 + i * esz;
        const char* name;
        const char* types;
        IMP imp;
        if (rel) {
            name = reinterpret_cast<const char*>(rd(e + rdi32(e)));
            types = reinterpret_cast<const char*>(e + 4 + rdi32(e + 4));
            imp = reinterpret_cast<IMP>(e + 8 + rdi32(e + 8));
        } else {
            SEL canon = sel(reinterpret_cast<const char*>(rd(e)));
            *reinterpret_cast<uintptr_t*>(e) = reinterpret_cast<uintptr_t>(canon);   // uniqued, as libobjc does
            name = canon;
            types = reinterpret_cast<const char*>(rd(e + 8));
            imp = reinterpret_cast<IMP>(rd(e + 16));
        }
        if (!name || !imp) continue;
        add_method_locked(into, sel(name), imp, types);
    }
}

static void read_protocols(uintptr_t list, RClass* into) {
    if (!list) return;
    uint64_t n = rd(list);
    for (uint64_t i = 0; i < n; ++i) into->protocols.push_back(rd(list + 8 + 8 * i));
}

static RClass* register_guest_class_locked(uintptr_t cls) {
    auto it = by_addr().find(cls);
    if (it != by_addr().end()) return it->second;
    uintptr_t meta = rd(cls) & ~7ull;
    auto mk = [&](uintptr_t c, bool is_meta) {
        auto* r = new RClass();
        r->cls = c;
        r->is_meta = is_meta;
        uintptr_t ro = rd(c + 32) & kPtrMask;
        r->ro = ro;
        r->inst_size = rd32(ro + 8);
        if (r->inst_size < 8) r->inst_size = 8;
        const char* nm = reinterpret_cast<const char*>(rd(ro + 24));
        r->name = nm ? nm : "?";
        read_method_list_locked(rd(ro + 32), r);
        read_protocols(rd(ro + 40), r);
        by_addr()[c] = r;
        reinterpret_cast<class_t*>(c)->cache = reinterpret_cast<uintptr_t>(r);
        return r;
    };
    RClass* rc = mk(cls, false);
    RClass* rm = mk(meta, true);
    rc->meta = rm;
    rm->instance = rc;
    by_name()[rc->name] = rc;
    return rc;
}

static void link_super_locked(RClass* r) {
    uintptr_t sup = rd(r->cls + 8) & ~7ull;
    if (!sup) {
        // a guest root class: its metaclass's superclass is the class itself
        r->super = r->is_meta ? r->instance : nullptr;
        return;
    }
    auto it = by_addr().find(sup);
    if (it != by_addr().end()) {
        r->super = it->second;
    } else {
        log("class %s%s: unknown superclass %p, using NSObject", r->is_meta ? "meta " : "", r->name.c_str(), reinterpret_cast<void*>(sup));
        r->super = r->is_meta ? g_nsobject->meta : g_nsobject;
    }
}

static std::vector<std::pair<RClass*, IMP>> g_load_list;

void boot_guest(const ImageInfo& im) {
    g_no_free = getenv("LOADER_APP_NO_FREE") != nullptr;
    g_zombies = getenv("LOADER_APP_ZOMBIES") != nullptr;
    std::lock_guard<std::recursive_mutex> l(g_rt);
    set_image_range(im.base, im.base + im.size);
    // uniqued selector references
    for (uintptr_t p = im.selrefs; p < im.selrefs + im.selrefs_size; p += 8) {
        auto* slot = reinterpret_cast<uintptr_t*>(p);
        *slot = reinterpret_cast<uintptr_t>(sel(reinterpret_cast<const char*>(*slot)));
    }
    std::vector<RClass*> guest;
    for (uintptr_t p = im.classlist; p < im.classlist + im.classlist_size; p += 8) {
        uintptr_t c = rd(p) & ~7ull;
        if (!c) continue;
        RClass* r = register_guest_class_locked(c);
        guest.push_back(r);
        guest.push_back(r->meta);
    }
    for (RClass* r : guest) link_super_locked(r);
    size_t ncat = 0;
    auto attach = [&](uintptr_t list, uintptr_t size, bool nonlazy) {
        for (uintptr_t p = list; p < list + size; p += 8) {
            uintptr_t cat = rd(p) & ~7ull;
            if (!cat) continue;
            uintptr_t cls = rd(cat + 8) & ~7ull;
            RClass* r = rc_of_class(cls);
            if (!r) {
                log("category %s on unknown class %p", reinterpret_cast<const char*>(rd(cat)), reinterpret_cast<void*>(cls));
                continue;
            }
            read_method_list_locked(rd(cat + 16), r);
            if (r->meta) read_method_list_locked(rd(cat + 24), r->meta);
            read_protocols(rd(cat + 32), r);
            ++ncat;
            (void)nonlazy;
        }
    };
    attach(im.catlist, im.catlist_size, false);
    attach(im.nlcatlist, im.nlcatlist_size, true);
    invalidate_caches();
    log("runtime: %zu guest classes, %zu categories, %zu selectors", guest.size() / 2, ncat, g_sels ? g_sels->size() : 0);
    // +load: non-lazy classes, superclasses first (skip compatibility shims that patch the system runtime)
    static const char* kSkipLoad[] = {"__ARCLite__", "CBDynamicallyLoadedDependencies"};
    for (uintptr_t p = im.nlclslist; p < im.nlclslist + im.nlclslist_size; p += 8) {
        uintptr_t c = rd(p) & ~7ull;
        RClass* r = rc_of_class(c);
        if (!r) continue;
        bool skip = false;
        for (const char* s : kSkipLoad) skip |= r->name == s;
        auto it = r->meta->methods.find(sel("load"));
        if (it == r->meta->methods.end()) continue;
        if (skip) { log("+[%s load] skipped", r->name.c_str()); continue; }
        g_load_list.push_back({r, it->second->imp});
    }
}

void run_loads() {
    for (auto& [r, imp] : g_load_list) {
        log("+[%s load]", r->name.c_str());
        reinterpret_cast<void (*)(id, SEL)>(imp)(reinterpret_cast<id>(r->cls), sel("load"));
    }
    g_load_list.clear();
}

// the current implementation of a method (searching superclasses), or null
IMP method_imp(const char* cls_name, const char* sel_name, bool class_method) {
    std::lock_guard<std::recursive_mutex> l(g_rt);
    auto it = by_name().find(cls_name);
    if (it == by_name().end()) return nullptr;
    MethodEntry* m = find_method_locked(class_method ? it->second->meta : it->second, sel(sel_name));
    return m ? m->imp : nullptr;
}

// replaces a guest method with a native one (used to keep analytics/ad SDKs inert)
bool override_method(const char* cls_name, const char* sel_name, bool class_method, IMP imp) {
    std::lock_guard<std::recursive_mutex> l(g_rt);
    auto it = by_name().find(cls_name);
    if (it == by_name().end()) return false;
    RClass* r = class_method ? it->second->meta : it->second;
    add_method_locked(r, sel(sel_name), imp, nullptr);
    invalidate_caches();
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// protocols
// ---------------------------------------------------------------------------------------------------------------
static const char* protocol_name(uintptr_t p) { return p ? reinterpret_cast<const char*>(rd(p + 8)) : nullptr; }
static bool protocol_conforms(uintptr_t p, const char* name, int depth = 0) {
    if (!p || depth > 16) return false;
    const char* n = protocol_name(p);
    if (n && std::strcmp(n, name) == 0) return true;
    uintptr_t list = rd(p + 16);
    if (!list) return false;
    uint64_t cnt = rd(list);
    for (uint64_t i = 0; i < cnt; ++i)
        if (protocol_conforms(rd(list + 8 + 8 * i), name, depth + 1)) return true;
    return false;
}
bool class_conforms(Class c, const char* proto) {
    std::lock_guard<std::recursive_mutex> l(g_rt);
    for (RClass* r = rc_of_class(c); r; r = r->super) {
        if (r->proto_names.count(proto)) return true;
        for (uintptr_t p : r->protocols)
            if (protocol_conforms(p, proto)) return true;
    }
    return false;
}
void declare_protocol(Class c, const char* proto) {
    std::lock_guard<std::recursive_mutex> l(g_rt);
    if (RClass* r = rc_of_class(c)) r->proto_names.insert(proto);
}

// Protocol objects: the guest's protocol_t, or a native stand-in {isa, name}
static std::unordered_map<std::string, uintptr_t>* g_protocols;
uintptr_t protocol_named(const char* name) {
    std::lock_guard<std::recursive_mutex> l(g_rt);
    if (!g_protocols) g_protocols = new std::unordered_map<std::string, uintptr_t>();
    auto it = g_protocols->find(name);
    if (it != g_protocols->end()) return it->second;
    auto* p = static_cast<uintptr_t*>(calloc(1, 16 + 80));
    reinterpret_cast<uint32_t*>(p)[0] = kClsMagic;
    reinterpret_cast<std::atomic<int32_t>*>(p)[1].store(kImmortal);
    uintptr_t* obj = p + 2;
    Class pc = class_named("Protocol");
    obj[0] = pc;
    obj[1] = reinterpret_cast<uintptr_t>(strdup(name));
    (*g_protocols)[name] = reinterpret_cast<uintptr_t>(obj);
    return reinterpret_cast<uintptr_t>(obj);
}
void register_guest_protocols(uintptr_t list, uintptr_t size) {
    std::lock_guard<std::recursive_mutex> l(g_rt);
    if (!g_protocols) g_protocols = new std::unordered_map<std::string, uintptr_t>();
    for (uintptr_t p = list; p < list + size; p += 8) {
        uintptr_t proto = rd(p) & ~7ull;
        if (const char* n = protocol_name(proto)) g_protocols->emplace(n, proto);
    }
}

}  // namespace app

// ---------------------------------------------------------------------------------------------------------------
// blocks
// ---------------------------------------------------------------------------------------------------------------
namespace {

struct BlockDescriptor { unsigned long reserved, size; void (*copy)(void*, const void*); void (*dispose)(const void*); };
struct BlockLayout { uintptr_t isa; std::atomic<int32_t> flags; int32_t reserved; void* invoke; BlockDescriptor* desc; };
struct Byref {
    uintptr_t isa;
    Byref* forwarding;
    std::atomic<int32_t> flags;
    uint32_t size;
    void (*keep)(Byref*, Byref*);
    void (*destroy)(Byref*);
};

enum : int32_t {
    BLOCK_REFCOUNT_MASK = 0xfffe,
    BLOCK_NEEDS_FREE = 1 << 24,
    BLOCK_HAS_COPY_DISPOSE = 1 << 25,
    BLOCK_IS_GC = 1 << 27,
    BLOCK_IS_GLOBAL = 1 << 28,
    BLOCK_BYREF_NEEDS_FREE = 1 << 24,
    BLOCK_BYREF_HAS_COPY_DISPOSE = 1 << 25,
};
enum { FIELD_IS_OBJECT = 3, FIELD_IS_BLOCK = 7, FIELD_IS_BYREF = 8, FIELD_IS_WEAK = 16, BYREF_CALLER = 128 };

Class g_malloc_block_cls = 0;

}  // namespace

extern "C" void* app_Block_copy(const void* arg) {
    if (!arg) return nullptr;
    auto* b = const_cast<BlockLayout*>(static_cast<const BlockLayout*>(arg));
    int32_t f = b->flags.load();
    if (f & BLOCK_IS_GLOBAL) return b;
    if (f & BLOCK_NEEDS_FREE) {
        b->flags.fetch_add(2);
        return b;
    }
    size_t size = b->desc->size;
    // heap blocks get an object header so retain/release/autorelease treat them as objects
    auto* mem = static_cast<uint8_t*>(malloc(16 + size));
    auto* h = reinterpret_cast<uint32_t*>(mem);
    h[0] = 0;   // no refcount header: blocks count in their own flags
    auto* nb = reinterpret_cast<BlockLayout*>(mem + 16);
    std::memcpy(static_cast<void*>(nb), b, size);
    nb->isa = g_malloc_block_cls;
    nb->flags.store((f & ~(BLOCK_REFCOUNT_MASK)) | BLOCK_NEEDS_FREE | 2);
    if (f & BLOCK_HAS_COPY_DISPOSE) b->desc->copy(nb, b);
    return nb;
}

extern "C" void app_Block_release(const void* arg) {
    if (!arg) return;
    auto* b = const_cast<BlockLayout*>(static_cast<const BlockLayout*>(arg));
    int32_t f = b->flags.load();
    if (!(f & BLOCK_NEEDS_FREE)) return;
    if ((b->flags.fetch_sub(2) & BLOCK_REFCOUNT_MASK) == 2) {
        if (f & BLOCK_HAS_COPY_DISPOSE) b->desc->dispose(b);
        free(reinterpret_cast<uint8_t*>(b) - 16);
    }
}

static Byref* byref_copy(Byref* src) {
    if (src->forwarding->flags.load() & BLOCK_REFCOUNT_MASK) {
        src->forwarding->flags.fetch_add(2);
        return src->forwarding;
    }
    auto* copy = static_cast<Byref*>(malloc(src->size));
    std::memcpy(static_cast<void*>(copy), src, src->size);
    copy->isa = 0;
    copy->flags.store(src->flags.load() | BLOCK_BYREF_NEEDS_FREE | 4);
    copy->forwarding = copy;
    src->forwarding = copy;
    if (src->flags.load() & BLOCK_BYREF_HAS_COPY_DISPOSE) {
        copy->keep = src->keep;
        copy->destroy = src->destroy;
        src->keep(copy, src);
    }
    return copy;
}

static void byref_release(Byref* b) {
    b = b->forwarding;
    if (!(b->flags.load() & BLOCK_BYREF_NEEDS_FREE)) return;
    if ((b->flags.fetch_sub(2) & BLOCK_REFCOUNT_MASK) == 2) {
        if (b->flags.load() & BLOCK_BYREF_HAS_COPY_DISPOSE) b->destroy(b);
        free(b);
    }
}

extern "C" void app_Block_object_assign(void* dst, const void* obj, int flags) {
    auto** d = static_cast<const void**>(dst);
    if (flags & BYREF_CALLER) { *d = obj; return; }
    switch (flags & 0x1f) {
        case FIELD_IS_OBJECT: app::retain(const_cast<id>(static_cast<const AppObj*>(obj))); *d = obj; break;
        case FIELD_IS_BLOCK: *d = app_Block_copy(obj); break;
        case FIELD_IS_BYREF:
        case FIELD_IS_BYREF | FIELD_IS_WEAK: *d = byref_copy(const_cast<Byref*>(static_cast<const Byref*>(obj))); break;
        default: *d = obj; break;
    }
}

extern "C" void app_Block_object_dispose(const void* obj, int flags) {
    if (flags & BYREF_CALLER) return;
    switch (flags & 0x1f) {
        case FIELD_IS_OBJECT: app::release(const_cast<id>(static_cast<const AppObj*>(obj))); break;
        case FIELD_IS_BLOCK: app_Block_release(obj); break;
        case FIELD_IS_BYREF:
        case FIELD_IS_BYREF | FIELD_IS_WEAK: byref_release(const_cast<Byref*>(static_cast<const Byref*>(obj))); break;
        default: break;
    }
}

// block objects receive retain/release/copy/autorelease like any object
namespace app {
id block_copy(id b) { return static_cast<id>(app_Block_copy(b)); }
void block_release(id b) { app_Block_release(b); }
void* block_invoke_ptr(id b) { return b ? reinterpret_cast<BlockLayout*>(b)->invoke : nullptr; }
}

// ---------------------------------------------------------------------------------------------------------------
// objc_* C API
// ---------------------------------------------------------------------------------------------------------------
extern "C" {

static id rt_retain(id o) { return app::retain(o); }
static void rt_release(id o) { app::release(o); }
static id rt_autorelease(id o) { return app::autorelease(o); }
static id rt_retain_autorelease(id o) { return app::autorelease(app::retain(o)); }
static void* rt_pool_push() { return app::pool_push(); }
static void rt_pool_pop(void* t) { app::pool_pop(t); }
static void rt_store_strong(id* loc, id v) {
    id old = *loc;
    if (old == v) return;
    *loc = app::retain(v);
    app::release(old);
}
static id rt_retain_block(id b) { return static_cast<id>(app_Block_copy(b)); }

static void weak_register(id* loc, id v) {
    if (!v || !app::has_header(v)) return;
    std::lock_guard<std::mutex> l(g_side_mu);
    if (!g_weak) g_weak = new std::unordered_map<id, std::set<id*>>();
    (*g_weak)[v].insert(loc);
}
static void weak_unregister(id* loc, id v) {
    if (!v) return;
    std::lock_guard<std::mutex> l(g_side_mu);
    if (!g_weak) return;
    auto it = g_weak->find(v);
    if (it != g_weak->end()) it->second.erase(loc);
}
static id rt_store_weak(id* loc, id v) {
    weak_unregister(loc, *loc);
    *loc = v;
    weak_register(loc, v);
    return v;
}
static id rt_init_weak(id* loc, id v) { *loc = nullptr; return rt_store_weak(loc, v); }
static void rt_destroy_weak(id* loc) { weak_unregister(loc, *loc); *loc = nullptr; }
static id rt_load_weak_retained(id* loc) { std::lock_guard<std::mutex> l(g_side_mu); return app::retain(*loc); }
static id rt_load_weak(id* loc) { return app::autorelease(rt_load_weak_retained(loc)); }
static void rt_copy_weak(id* to, id* from) { rt_init_weak(to, *from); }
static void rt_move_weak(id* to, id* from) { rt_init_weak(to, *from); rt_destroy_weak(from); }

static id rt_get_property(id self, SEL, ptrdiff_t off, bool atomic) {
    id v = *reinterpret_cast<id*>(reinterpret_cast<uint8_t*>(self) + off);
    if (!atomic) return v;
    return app::autorelease(app::retain(v));
}
static void rt_set_property(id self, SEL, ptrdiff_t off, id v, bool, signed char copy) {
    id* slot = reinterpret_cast<id*>(reinterpret_cast<uint8_t*>(self) + off);
    id nv = copy ? (copy == 2 ? app::send(v, "mutableCopy") : app::send(v, "copy")) : app::retain(v);
    id old = *slot;
    *slot = nv;
    app::release(old);
}
static void rt_set_property_atomic_copy(id self, SEL s, id v, ptrdiff_t off) { rt_set_property(self, s, off, v, true, 1); }
static void rt_set_property_nonatomic_copy(id self, SEL s, id v, ptrdiff_t off) { rt_set_property(self, s, off, v, false, 1); }
static void rt_copy_struct(void* dst, const void* src, ptrdiff_t n, bool, bool) { std::memmove(dst, src, n); }

static std::recursive_mutex* sync_mutex(id o) {
    std::lock_guard<std::mutex> l(g_side_mu);
    if (!g_sync) g_sync = new std::unordered_map<id, std::recursive_mutex*>();
    auto& m = (*g_sync)[o];
    if (!m) m = new std::recursive_mutex();
    return m;
}
static int rt_sync_enter(id o) { if (o) sync_mutex(o)->lock(); return 0; }
static int rt_sync_exit(id o) { if (o) sync_mutex(o)->unlock(); return 0; }

static void rt_set_assoc(id o, const void* key, id v, uintptr_t policy) {
    if (!o) return;
    id stored = v;
    if ((policy & 3) == 3) stored = app::send(v, "copy");
    else if (policy & 1) stored = app::retain(v);
    id old = nullptr;
    uintptr_t old_policy = 0;
    {
        std::lock_guard<std::mutex> l(g_side_mu);
        if (!g_assoc) g_assoc = new std::unordered_map<id, std::unordered_map<const void*, Assoc>>();
        auto& m = (*g_assoc)[o];
        auto it = m.find(key);
        if (it != m.end()) { old = it->second.value; old_policy = it->second.policy; }
        if (v) m[key] = {policy, stored};
        else m.erase(key);
    }
    if (old_policy & 3) app::release(old);
}
static id rt_get_assoc(id o, const void* key) {
    std::lock_guard<std::mutex> l(g_side_mu);
    if (!g_assoc) return nullptr;
    auto it = g_assoc->find(o);
    if (it == g_assoc->end()) return nullptr;
    auto k = it->second.find(key);
    return k == it->second.end() ? nullptr : k->second.value;
}
static void rt_remove_assocs(id o) {
    std::vector<id> rel;
    {
        std::lock_guard<std::mutex> l(g_side_mu);
        if (!g_assoc) return;
        auto it = g_assoc->find(o);
        if (it == g_assoc->end()) return;
        for (auto& kv : it->second)
            if (kv.second.policy & 3) rel.push_back(kv.second.value);
        g_assoc->erase(it);
    }
    for (id v : rel) app::release(v);
}

static Class rt_get_class(const char* name) { return name ? app::class_named(name) : 0; }
static Class rt_get_meta_class(const char* name) { return app::metaclass_of(app::class_named(name)); }
static Class rt_object_get_class(id o) { return o ? o->isa : 0; }
static Class rt_object_set_class(id o, Class c) { if (!o) return 0; Class old = o->isa; o->isa = c; return old; }
static const char* rt_class_get_name(Class c) { return c ? app::class_name(c) : "nil"; }
static const char* rt_object_get_class_name(id o) { return o ? app::class_name(o->isa) : "nil"; }
static Class rt_class_get_superclass(Class c) { return app::superclass_of(c); }
static bool rt_class_is_meta(Class c) { return app::is_meta(c); }
static size_t rt_class_get_instance_size(Class c) { return app::instance_size(c); }
static const char* rt_sel_get_name(SEL s) { return s; }
static SEL rt_sel_register(const char* n) { return app::sel(n); }
static bool rt_sel_is_equal(SEL a, SEL b) { return a == b; }

static MethodEntry* method_for(Class c, SEL s) {
    std::lock_guard<std::recursive_mutex> l(g_rt);
    RClass* r = rc_of_class(c);
    return r ? find_method_locked(r, s) : nullptr;
}
static void* rt_class_get_instance_method(Class c, SEL s) { return method_for(c, s); }
static void* rt_class_get_class_method(Class c, SEL s) { return method_for(app::metaclass_of(c), s); }
static IMP rt_class_get_method_implementation(Class c, SEL s) {
    MethodEntry* m = method_for(c, s);
    return m ? m->imp : app::nil_imp();
}
static bool rt_class_responds(Class c, SEL s) { return method_for(c, s) != nullptr; }
static bool rt_class_add_method(Class c, SEL s, IMP imp, const char* types) {
    std::lock_guard<std::recursive_mutex> l(g_rt);
    RClass* r = rc_of_class(c);
    if (!r || r->methods.count(s)) return false;
    add_method_locked(r, s, imp, types);
    invalidate_caches();
    return true;
}
static IMP rt_class_replace_method(Class c, SEL s, IMP imp, const char* types) {
    std::lock_guard<std::recursive_mutex> l(g_rt);
    RClass* r = rc_of_class(c);
    if (!r) return nullptr;
    auto it = r->methods.find(s);
    IMP old = it != r->methods.end() ? it->second->imp : nullptr;
    add_method_locked(r, s, imp, types);
    invalidate_caches();
    return old;
}
static bool rt_class_add_protocol(Class c, uintptr_t proto) {
    std::lock_guard<std::recursive_mutex> l(g_rt);
    RClass* r = rc_of_class(c);
    if (!r) return false;
    r->protocols.push_back(proto);
    return true;
}
static bool rt_class_conforms(Class c, uintptr_t proto) {
    const char* n = proto ? reinterpret_cast<const char*>(*reinterpret_cast<uintptr_t*>(proto + 8)) : nullptr;
    return n && app::class_conforms(c, n);
}
static bool rt_class_add_property(Class, const char*, const void*, unsigned) { return true; }
static void* rt_property_copy_attribute_list(void*, unsigned* out) { if (out) *out = 0; return nullptr; }
static IMP rt_method_get_implementation(MethodEntry* m) { return m ? m->imp : nullptr; }
static SEL rt_method_get_name(MethodEntry* m) { return m ? m->name : nullptr; }
static const char* rt_method_get_type_encoding(MethodEntry* m) { return m ? m->types : nullptr; }
static IMP rt_method_set_implementation(MethodEntry* m, IMP imp) {
    if (!m) return nullptr;
    std::lock_guard<std::recursive_mutex> l(g_rt);
    IMP old = m->imp;
    m->imp = imp;
    invalidate_caches();
    return old;
}
static void rt_method_exchange(MethodEntry* a, MethodEntry* b) {
    if (!a || !b) return;
    std::lock_guard<std::recursive_mutex> l(g_rt);
    std::swap(a->imp, b->imp);
    invalidate_caches();
}

// ivars: guest class_ro_t ivar lists {offset*, name, type, alignment, size}
static void* rt_class_get_instance_variable(Class c, const char* name) {
    for (RClass* r = rc_of_class(c); r; r = r->super) {
        if (!r->ro) continue;
        uintptr_t list = rd(r->ro + 48);
        if (!list) continue;
        uint32_t esz = rd32(list) & 0xffff, n = rd32(list + 4);
        if (esz < 32) esz = 32;
        for (uint32_t i = 0; i < n; ++i) {
            uintptr_t e = list + 8 + i * esz;
            const char* nm = reinterpret_cast<const char*>(rd(e + 8));
            if (nm && std::strcmp(nm, name) == 0) return reinterpret_cast<void*>(e);
        }
    }
    return nullptr;
}
void* app_class_get_instance_variable(Class c, const char* name) { return rt_class_get_instance_variable(c, name); }
static ptrdiff_t rt_ivar_get_offset(void* ivar) {
    if (!ivar) return 0;
    uintptr_t offp = rd(reinterpret_cast<uintptr_t>(ivar));
    return offp ? *reinterpret_cast<int32_t*>(offp) : 0;
}
static const char* rt_ivar_get_type(void* ivar) { return ivar ? reinterpret_cast<const char*>(rd(reinterpret_cast<uintptr_t>(ivar) + 16)) : ""; }
static id rt_object_get_ivar(id o, void* ivar) {
    if (!o || !ivar) return nullptr;
    return *reinterpret_cast<id*>(reinterpret_cast<uint8_t*>(o) + rt_ivar_get_offset(ivar));
}
static void rt_object_set_ivar(id o, void* ivar, id v) {
    if (!o || !ivar) return;
    *reinterpret_cast<id*>(reinterpret_cast<uint8_t*>(o) + rt_ivar_get_offset(ivar)) = v;
}
static id rt_construct_instance(Class c, void* bytes) {
    if (!bytes) return nullptr;
    reinterpret_cast<id>(bytes)->isa = c;
    return static_cast<id>(bytes);
}
static id rt_class_create_instance(Class c, size_t extra) {
    (void)extra;
    return app::alloc(c);
}

// dynamic classes: objc_allocateClassPair/objc_initializeClassPair + objc_registerClassPair
static Class rt_allocate_class_pair(Class super, const char* name, size_t) {
    if (app::class_named(name)) return 0;
    return app::def_class(name, super ? app::class_name(super) : nullptr);
}
static Class rt_initialize_class_pair(Class super, const char* name, Class, Class) { return rt_allocate_class_pair(super, name, 0); }
static void rt_register_class_pair(Class) {}
static void rt_dispose_class_pair(Class) {}

static uintptr_t rt_get_protocol(const char* name) { return app::protocol_named(name); }
static const char* rt_protocol_get_name(uintptr_t p) { return p ? reinterpret_cast<const char*>(rd(p + 8)) : nullptr; }

static int rt_enumeration_mutation(id o) {
    app::log("collection %p mutated while being enumerated", static_cast<void*>(o));
    return 0;
}

// exceptions: there is no unwinder for Objective-C exceptions here; report and stop
// No unwinder: objc_exception_throw cannot reach an @catch, so log and return rather than aborting the app.
static std::atomic<int> g_throw_count{0};
static void rt_exception_throw(id ex) {
    std::string name = app::utf8(app::send(ex, "name"));
    std::string reason = app::utf8(app::send(ex, "reason"));
    app::log("*** uncaught exception (ignored) '%s': '%s'", name.c_str(), reason.c_str());
    if (g_throw_count.fetch_add(1) > 200) { app::log("too many exceptions; aborting"); fflush(stderr); abort(); }
}
static void rt_exception_rethrow() { app::log("objc_exception_rethrow (ignored)"); if (g_throw_count.fetch_add(1) > 200) abort(); }
static void* rt_begin_catch(void* e) { return e; }
static void rt_end_catch() {}
static void rt_terminate() { app::log("objc_terminate"); abort(); }

static id rt_alloc(Class c) { return app::send_cls(c, "alloc"); }
static id rt_alloc_init(Class c) { return app::send(app::send_cls(c, "alloc"), "init"); }
static id rt_opt_new(Class c) { return app::send_cls(c, "new"); }
static Class rt_opt_class(id o) { return app::send<Class>(o, "class"); }
static bool rt_opt_is_kind(id o, Class c) { return app::send<bool>(o, "isKindOfClass:", c); }
static bool rt_opt_responds(id o, SEL s) { return app::send<bool>(o, "respondsToSelector:", s); }
static id rt_retained_return(id o) { return o; }

}  // extern "C"

namespace app {

void register_runtime_symbols(std::unordered_map<std::string, void*>& m) {
    auto r = [&](const char* n, auto f) { m[n] = reinterpret_cast<void*>(f); };
    r("_objc_msgSend", &app_objc_msgSend);
    r("_objc_msgSendSuper2", &app_objc_msgSendSuper2);
    r("_objc_msgSendSuper", &app_objc_msgSendSuper);
    r("_objc_retain", &rt_retain);
    r("_objc_release", &rt_release);
    r("_objc_autorelease", &rt_autorelease);
    r("_objc_autoreleaseReturnValue", &rt_autorelease);
    r("_objc_retainAutorelease", &rt_retain_autorelease);
    r("_objc_retainAutoreleaseReturnValue", &rt_retain_autorelease);
    r("_objc_retainAutoreleasedReturnValue", &rt_retain);
    r("_objc_unsafeClaimAutoreleasedReturnValue", &rt_retained_return);
    r("_objc_autoreleasePoolPush", &rt_pool_push);
    r("_objc_autoreleasePoolPop", &rt_pool_pop);
    r("_objc_storeStrong", &rt_store_strong);
    r("_objc_retainBlock", &rt_retain_block);
    r("_objc_initWeak", &rt_init_weak);
    r("_objc_storeWeak", &rt_store_weak);
    r("_objc_destroyWeak", &rt_destroy_weak);
    r("_objc_loadWeak", &rt_load_weak);
    r("_objc_loadWeakRetained", &rt_load_weak_retained);
    r("_objc_copyWeak", &rt_copy_weak);
    r("_objc_moveWeak", &rt_move_weak);
    r("_objc_getProperty", &rt_get_property);
    r("_objc_setProperty", &rt_set_property);
    r("_objc_setProperty_atomic_copy", &rt_set_property_atomic_copy);
    r("_objc_setProperty_nonatomic_copy", &rt_set_property_nonatomic_copy);
    r("_objc_copyStruct", &rt_copy_struct);
    r("_objc_sync_enter", &rt_sync_enter);
    r("_objc_sync_exit", &rt_sync_exit);
    r("_objc_setAssociatedObject", &rt_set_assoc);
    r("_objc_getAssociatedObject", &rt_get_assoc);
    r("_objc_removeAssociatedObjects", &rt_remove_assocs);
    r("_objc_getClass", &rt_get_class);
    r("_objc_lookUpClass", &rt_get_class);
    r("_objc_getRequiredClass", &rt_get_class);
    r("_objc_getMetaClass", &rt_get_meta_class);
    r("_NSClassFromString_rt", &rt_get_class);
    r("_object_getClass", &rt_object_get_class);
    r("_object_setClass", &rt_object_set_class);
    r("_object_getClassName", &rt_object_get_class_name);
    r("_class_getName", &rt_class_get_name);
    r("_class_getSuperclass", &rt_class_get_superclass);
    r("_class_isMetaClass", &rt_class_is_meta);
    r("_class_getInstanceSize", &rt_class_get_instance_size);
    r("_class_getInstanceMethod", &rt_class_get_instance_method);
    r("_class_getClassMethod", &rt_class_get_class_method);
    r("_class_getMethodImplementation", &rt_class_get_method_implementation);
    r("_class_respondsToSelector", &rt_class_responds);
    r("_class_addMethod", &rt_class_add_method);
    r("_class_replaceMethod", &rt_class_replace_method);
    r("_class_addProtocol", &rt_class_add_protocol);
    r("_class_conformsToProtocol", &rt_class_conforms);
    r("_class_addProperty", &rt_class_add_property);
    r("_class_getInstanceVariable", &rt_class_get_instance_variable);
    r("_class_createInstance", &rt_class_create_instance);
    r("_property_copyAttributeList", &rt_property_copy_attribute_list);
    r("_method_getImplementation", &rt_method_get_implementation);
    r("_method_getName", &rt_method_get_name);
    r("_method_getTypeEncoding", &rt_method_get_type_encoding);
    r("_method_setImplementation", &rt_method_set_implementation);
    r("_method_exchangeImplementations", &rt_method_exchange);
    r("_ivar_getOffset", &rt_ivar_get_offset);
    r("_ivar_getTypeEncoding", &rt_ivar_get_type);
    r("_object_getIvar", &rt_object_get_ivar);
    r("_object_setIvar", &rt_object_set_ivar);
    r("_objc_constructInstance", &rt_construct_instance);
    r("_objc_allocateClassPair", &rt_allocate_class_pair);
    r("_objc_initializeClassPair", &rt_initialize_class_pair);
    r("_objc_registerClassPair", &rt_register_class_pair);
    r("_objc_disposeClassPair", &rt_dispose_class_pair);
    r("_objc_getProtocol", &rt_get_protocol);
    r("_protocol_getName", &rt_protocol_get_name);
    r("_sel_getName", &rt_sel_get_name);
    r("_sel_getUid", &rt_sel_register);
    r("_sel_registerName", &rt_sel_register);
    r("_sel_isEqual", &rt_sel_is_equal);
    r("_objc_enumerationMutation", &rt_enumeration_mutation);
    r("_objc_exception_throw", &rt_exception_throw);
    r("_objc_exception_rethrow", &rt_exception_rethrow);
    r("_objc_begin_catch", &rt_begin_catch);
    r("_objc_end_catch", &rt_end_catch);
    r("_objc_terminate", &rt_terminate);
    r("_objc_alloc", &rt_alloc);
    r("_objc_allocWithZone", &rt_alloc);
    r("_objc_alloc_init", &rt_alloc_init);
    r("_objc_opt_new", &rt_opt_new);
    r("_objc_opt_class", &rt_opt_class);
    r("_objc_opt_isKindOfClass", &rt_opt_is_kind);
    r("_objc_opt_respondsToSelector", &rt_opt_responds);
    r("__Block_copy", &app_Block_copy);
    r("__Block_release", &app_Block_release);
    r("__Block_object_assign", &app_Block_object_assign);
    r("__Block_object_dispose", &app_Block_object_dispose);
    static uintptr_t empty_cache[4] = {0}, empty_vtable[4] = {0};
    m["__objc_empty_cache"] = empty_cache;
    m["__objc_empty_vtable"] = empty_vtable;
}

// ---------------------------------------------------------------------------------------------------------------
// NSObject, Protocol, blocks, zombies
// ---------------------------------------------------------------------------------------------------------------
std::string default_description(id o) {
    char buf[128];
    snprintf(buf, sizeof buf, "<%s: %p>", class_name(o->isa), static_cast<void*>(o));
    return buf;
}

void init_runtime_classes() {
    Class c = def_class("NSObject", nullptr);
    // class side (also reachable through the root metaclass -> NSObject chain for every class object)
    defc(c, "alloc", +[](id self, SEL) -> id { return alloc(reinterpret_cast<Class>(self)); });
    defc(c, "allocWithZone:", +[](id self, SEL, void*) -> id { return alloc(reinterpret_cast<Class>(self)); });
    defc(c, "new", +[](id self, SEL) -> id { return send(send(self, "alloc"), "init"); });
    defc(c, "class", +[](id self, SEL) -> id { return self; });
    defc(c, "superclass", +[](id self, SEL) -> Class { return superclass_of(reinterpret_cast<Class>(self)); });
    defc(c, "initialize", +[](id, SEL) {});
    defc(c, "load", +[](id, SEL) {});
    defc(c, "description", +[](id self, SEL) -> id { return str(class_name(reinterpret_cast<Class>(self))); });
    defc(c, "isSubclassOfClass:", +[](id self, SEL, Class o) -> bool { return is_subclass(reinterpret_cast<Class>(self), o); });
    defc(c, "instancesRespondToSelector:", +[](id self, SEL, SEL s) -> bool { return rt_class_responds(reinterpret_cast<Class>(self), s); });
    defc(c, "instanceMethodForSelector:", +[](id self, SEL, SEL s) -> IMP { return rt_class_get_method_implementation(reinterpret_cast<Class>(self), s); });
    defc(c, "conformsToProtocol:", +[](id self, SEL, uintptr_t p) -> bool { return rt_class_conforms(reinterpret_cast<Class>(self), p); });
    defc(c, "resolveInstanceMethod:", +[](id, SEL, SEL) -> bool { return false; });
    defc(c, "resolveClassMethod:", +[](id, SEL, SEL) -> bool { return false; });

    def(c, "init", +[](id self, SEL) -> id { return self; });
    def(c, "dealloc", +[](id self, SEL) { dealloc_object(self); });
    def(c, "finalize", +[](id, SEL) {});
    def(c, "retain", +[](id self, SEL) -> id { return retain(self); });
    def(c, "release", +[](id self, SEL) { release(self); });
    def(c, "autorelease", +[](id self, SEL) -> id { return autorelease(self); });
    def(c, "retainCount", +[](id self, SEL) -> NSUInteger { return static_cast<NSUInteger>(retain_count(self)); });
    def(c, "copy", +[](id self, SEL) -> id { return send(self, "copyWithZone:", static_cast<void*>(nullptr)); });
    def(c, "mutableCopy", +[](id self, SEL) -> id { return send(self, "mutableCopyWithZone:", static_cast<void*>(nullptr)); });
    def(c, "zone", +[](id, SEL) -> void* { return nullptr; });
    def(c, "self", +[](id self, SEL) -> id { return self; });
    def(c, "class", +[](id self, SEL) -> Class { return is_class_object(self) ? reinterpret_cast<Class>(self) : self->isa; });
    def(c, "superclass", +[](id self, SEL) -> Class { return superclass_of(self->isa); });
    def(c, "isProxy", +[](id, SEL) -> bool { return false; });
    def(c, "hash", +[](id self, SEL) -> NSUInteger { return reinterpret_cast<NSUInteger>(self) >> 4; });
    def(c, "isEqual:", +[](id self, SEL, id o) -> bool { return self == o; });
    def(c, "description", +[](id self, SEL) -> id { return str(default_description(self)); });
    def(c, "debugDescription", +[](id self, SEL) -> id { return send(self, "description"); });
    def(c, "isKindOfClass:", +[](id self, SEL, Class k) -> bool {
        Class mine = is_class_object(self) ? class_of(self) : self->isa;
        return is_subclass(mine, k);
    });
    def(c, "isMemberOfClass:", +[](id self, SEL, Class k) -> bool { return class_of(self) == k; });
    def(c, "respondsToSelector:", +[](id self, SEL, SEL s) -> bool { return s && responds(self, s); });
    def(c, "conformsToProtocol:", +[](id self, SEL, uintptr_t p) -> bool { return rt_class_conforms(class_of(self), p); });
    def(c, "methodForSelector:", +[](id self, SEL, SEL s) -> IMP { return lookup(self, s); });
    def(c, "performSelector:", +[](id self, SEL, SEL s) -> id { return send(self, s); });
    def(c, "performSelector:withObject:", +[](id self, SEL, SEL s, id a) -> id { return send(self, s, a); });
    def(c, "performSelector:withObject:withObject:", +[](id self, SEL, SEL s, id a, id b) -> id { return send(self, s, a, b); });
    def(c, "forwardingTargetForSelector:", +[](id, SEL, SEL) -> id { return nullptr; });
    def(c, "doesNotRecognizeSelector:", +[](id self, SEL, SEL s) { log("doesNotRecognizeSelector: -[%s %s]", class_name(class_of(self)), s); });
    def(c, "copyWithZone:", +[](id self, SEL, void*) -> id { log("-[%s copyWithZone:] not implemented; retaining", class_name(class_of(self))); return retain(self); });
    def(c, "mutableCopyWithZone:", +[](id self, SEL, void*) -> id { return retain(self); });
    // KVO is not emulated
    def(c, "addObserver:forKeyPath:options:context:", +[](id, SEL, id, id, NSUInteger, void*) {});
    def(c, "removeObserver:forKeyPath:", +[](id, SEL, id, id) {});
    def(c, "removeObserver:forKeyPath:context:", +[](id, SEL, id, id, void*) {});
    def(c, "willChangeValueForKey:", +[](id, SEL, id) {});
    def(c, "didChangeValueForKey:", +[](id, SEL, id) {});

    Class proto = def_class("Protocol", "NSObject");
    def(proto, "name", +[](id self, SEL) -> id { return str(reinterpret_cast<const char*>(reinterpret_cast<uintptr_t*>(self)[1])); });

    // blocks
    Class nsblock = def_class("NSBlock", "NSObject");
    def(nsblock, "copy", +[](id self, SEL) -> id { return block_copy(self); });
    def(nsblock, "copyWithZone:", +[](id self, SEL, void*) -> id { return block_copy(self); });
    def(nsblock, "retain", +[](id self, SEL) -> id { return block_copy(self); });
    def(nsblock, "release", +[](id self, SEL) { block_release(self); });
    def(nsblock, "autorelease", +[](id self, SEL) -> id {
        // blocks count in their flags; balance with a deferred release on the main loop pool
        return self;
    });
    def(nsblock, "retainCount", +[](id self, SEL) -> NSUInteger { return 1; });
    def(nsblock, "invoke", +[](id self, SEL) { reinterpret_cast<void (*)(id)>(block_invoke_ptr(self))(self); });
    def_class("__NSStackBlock__", "NSBlock");
    def_class("__NSGlobalBlock__", "NSBlock");
    g_malloc_block_cls = def_class("__NSMallocBlock__", "NSBlock");

    Class z = def_class("_NSZombie_", nullptr);
    {
        std::lock_guard<std::recursive_mutex> l(g_rt);
        g_zombie = rc_of_class(z);
        g_zombie->zombie = true;
        g_zombie->meta->zombie = true;
    }
}

}  // namespace app
