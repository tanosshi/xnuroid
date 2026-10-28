// Minimal Objective-C runtime for the guest image.
//
// UnityFramework contains Unity's own iOS glue (UnityAppController, DisplayManager, UnityView, ...) as Objective-C
// classes. There is no ObjC runtime in an Android process, so this registers those classes from the image's
// __objc_classlist / __objc_catlist and dispatches messages to the guest's own method implementations.
// Classes that live outside the image (UIKit, Foundation, Metal, ...) are "foreign": messages to them go to the
// fake-Foundation fallback in android_loader.cpp.
//
// Layout facts used (arm64, non-fragile ABI):
//   class_t   { isa, superclass, cache, vtable, data|flags }          data -> class_ro_t
//   class_ro_t{ u32 flags, instStart, instSize, reserved; ivarLayout; name; baseMethods; ... }
//   method_list_t { u32 entsize_and_flags, count; entries }  flag 0x80000000 = relative (12-byte) entries
//   category_t{ name, cls, instanceMethods, classMethods, protocols, ... }
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace objcrt {

struct GClass {
    uint64_t addr = 0;
    std::string name;
    GClass* super = nullptr;       // guest superclass (null if root or foreign)
    std::string foreign_super;     // import name when the superclass is outside the image
    GClass* meta = nullptr;        // metaclass (class methods) for a class; null for a metaclass
    bool is_meta = false;
    uint32_t instance_size = 16;
    std::unordered_map<const char*, uint64_t> methods;   // interned SEL -> IMP
};

inline std::mutex g_mu;
inline std::unordered_map<uint64_t, GClass*> g_by_addr;        // class_t and metaclass addresses
inline std::unordered_map<std::string, GClass*> g_by_name;
inline std::unordered_set<uint64_t> g_instances;
inline std::unordered_map<std::string, const char*> g_sel_intern;
inline std::unordered_map<const char*, const char*> g_sel_ptr_cache;
inline uint64_t g_image_lo = 0, g_image_hi = 0;
inline int g_miss_logged = 0;

inline bool in_image(uint64_t p) { return p >= g_image_lo && p < g_image_hi; }
inline uint64_t rd64(uint64_t a) { return *reinterpret_cast<const uint64_t*>(a); }
inline uint32_t rd32(uint64_t a) { return *reinterpret_cast<const uint32_t*>(a); }
inline int32_t rdi32(uint64_t a) { return *reinterpret_cast<const int32_t*>(a); }

inline const char* intern_locked(const char* sel) {
    auto pc = g_sel_ptr_cache.find(sel);
    if (pc != g_sel_ptr_cache.end()) return pc->second;
    std::string s(sel);
    auto it = g_sel_intern.find(s);
    const char* canon;
    if (it == g_sel_intern.end()) {
        canon = strdup(sel);
        g_sel_intern.emplace(std::move(s), canon);
    } else {
        canon = it->second;
    }
    g_sel_ptr_cache.emplace(sel, canon);
    return canon;
}
inline const char* intern(const char* sel) {
    std::lock_guard<std::mutex> l(g_mu);
    return intern_locked(sel);
}

inline void read_method_list(uint64_t list, GClass* into) {
    if (!list || !in_image(list)) return;
    uint32_t ent = rd32(list), cnt = rd32(list + 4);
    bool rel = ent & 0x80000000u;
    for (uint32_t i = 0; i < cnt; ++i) {
        uint64_t e = list + 8 + i * (rel ? 12 : 24);
        const char* sel;
        uint64_t imp;
        if (rel) {
            uint64_t sel_ref = e + rdi32(e);
            sel = reinterpret_cast<const char*>(rd64(sel_ref));
            imp = (e + 8) + rdi32(e + 8);
        } else {
            sel = reinterpret_cast<const char*>(rd64(e));
            imp = rd64(e + 16);
        }
        if (!sel || !imp) continue;
        into->methods[intern_locked(sel)] = imp;
    }
}

// bound_symbol(slot_address) -> the import name a data slot was bound to, or nullptr.
using BoundSymbolFn = std::function<const char*(uint64_t)>;

inline GClass* make_class(uint64_t cls, bool meta) {
    auto* c = new GClass();
    c->addr = cls;
    c->is_meta = meta;
    uint64_t ro = rd64(cls + 32) & 0x00007ffffffffff8ull;
    c->instance_size = rd32(ro + 8);
    if (c->instance_size < 16) c->instance_size = 16;
    const char* nm = reinterpret_cast<const char*>(rd64(ro + 24));
    c->name = nm ? nm : "?";
    read_method_list(rd64(ro + 32), c);
    return c;
}

// seg: the image's [lo,hi) runtime range. Section addresses are runtime addresses.
inline void init(uint64_t image_lo, uint64_t image_hi, uint64_t classlist, uint64_t classlist_size, uint64_t catlist,
                 uint64_t catlist_size, uint64_t methname, uint64_t methname_size, const BoundSymbolFn& bound_symbol) {
    std::lock_guard<std::mutex> l(g_mu);
    g_image_lo = image_lo;
    g_image_hi = image_hi;
    // Canonical selector pointers = the image's own strings, so @selector(x) and sel_registerName("x") agree.
    for (uint64_t p = methname; p < methname + methname_size;) {
        const char* s = reinterpret_cast<const char*>(p);
        size_t n = std::strlen(s);
        if (n) g_sel_intern.emplace(std::string(s, n), s);
        p += n + 1;
    }
    std::vector<GClass*> all;
    for (uint64_t i = 0; i < classlist_size / 8; ++i) {
        uint64_t cls = rd64(classlist + 8 * i) & 0x00007fffffffffffull;
        if (!cls) continue;
        uint64_t meta = rd64(cls) & 0x00007fffffffffffull;
        GClass* c = make_class(cls, false);
        GClass* m = meta && in_image(meta) ? make_class(meta, true) : nullptr;
        c->meta = m;
        g_by_addr[cls] = c;
        if (m) g_by_addr[meta] = m;
        g_by_name[c->name] = c;
        all.push_back(c);
        if (m) all.push_back(m);
    }
    for (GClass* c : all) {
        uint64_t sup = rd64(c->addr + 8) & 0x00007fffffffffffull;
        auto it = g_by_addr.find(sup);
        if (it != g_by_addr.end()) {
            c->super = it->second;
        } else {
            const char* sym = bound_symbol ? bound_symbol(c->addr + 8) : nullptr;
            c->foreign_super = sym ? sym : "(root)";
        }
    }
    size_t cats = 0, cat_methods = 0;
    for (uint64_t i = 0; i < catlist_size / 8; ++i) {
        uint64_t cat = rd64(catlist + 8 * i) & 0x00007fffffffffffull;
        if (!cat) continue;
        uint64_t cls = rd64(cat + 8) & 0x00007fffffffffffull;
        auto it = g_by_addr.find(cls);
        if (it == g_by_addr.end()) continue;   // category on a foreign class (UIKit/Foundation)
        GClass* c = it->second;
        size_t before = c->methods.size() + (c->meta ? c->meta->methods.size() : 0);
        read_method_list(rd64(cat + 16), c);
        if (c->meta) read_method_list(rd64(cat + 24), c->meta);
        cat_methods += c->methods.size() + (c->meta ? c->meta->methods.size() : 0) - before;
        ++cats;
    }
    fprintf(stderr, "[OBJC-RT] %zu classes (incl. metaclasses), %zu categories on guest classes (%zu new methods), %zu selectors\n",
            all.size(), cats, cat_methods, g_sel_intern.size());
}

inline GClass* class_of_locked(uint64_t recv, bool* is_class_object = nullptr) {
    auto it = g_by_addr.find(recv);
    if (it != g_by_addr.end() && !it->second->is_meta) {
        if (is_class_object) *is_class_object = true;
        return it->second->meta;           // class method dispatch goes through the metaclass
    }
    if (g_instances.count(recv)) {
        auto ci = g_by_addr.find(rd64(recv) & 0x00007fffffffffffull);
        if (ci != g_by_addr.end()) return ci->second;
    }
    return nullptr;
}

inline uint64_t find_imp_locked(GClass* c, const char* canon_sel) {
    for (; c; c = c->super) {
        auto it = c->methods.find(canon_sel);
        if (it != c->methods.end()) return it->second;
    }
    return 0;
}

// Returns the IMP for (receiver, sel) when the receiver is a guest class/instance and a method exists.
inline uint64_t lookup_imp(uint64_t recv, const char* sel) {
    std::lock_guard<std::mutex> l(g_mu);
    GClass* c = class_of_locked(recv);
    if (!c) return 0;
    const char* canon = intern_locked(sel);
    uint64_t imp = find_imp_locked(c, canon);
    if (!imp && g_miss_logged < 40) {
        ++g_miss_logged;
        fprintf(stderr, "[OBJC-RT] no guest method %c[%s %s] (super: %s) -> fallback\n", c->is_meta ? '+' : '-',
                c->is_meta && c->addr ? c->name.c_str() : c->name.c_str(), sel,
                c->super ? c->super->name.c_str() : c->foreign_super.c_str());
    }
    return imp;
}

// objc_msgSendSuper2: super->super_class is the *current* class; the search starts at its superclass.
inline uint64_t lookup_super_imp(uint64_t super_struct, const char* sel) {
    std::lock_guard<std::mutex> l(g_mu);
    uint64_t cur = rd64(super_struct + 8) & 0x00007fffffffffffull;
    auto it = g_by_addr.find(cur);
    if (it == g_by_addr.end()) return 0;
    return find_imp_locked(it->second->super, intern_locked(sel));
}

inline uint64_t alloc_instance(GClass* c) {
    void* mem = calloc(1, c->instance_size < 32 ? 32 : c->instance_size);
    *reinterpret_cast<uint64_t*>(mem) = c->addr;    // plain (non-"nonpointer") isa
    uint64_t p = reinterpret_cast<uint64_t>(mem);
    std::lock_guard<std::mutex> l(g_mu);
    g_instances.insert(p);
    return p;
}

inline GClass* guest_class_for_object(uint64_t obj, bool* is_class_object = nullptr) {
    std::lock_guard<std::mutex> l(g_mu);
    auto it = g_by_addr.find(obj);
    if (it != g_by_addr.end() && !it->second->is_meta) {
        if (is_class_object) *is_class_object = true;
        return it->second;
    }
    if (g_instances.count(obj)) {
        auto ci = g_by_addr.find(rd64(obj) & 0x00007fffffffffffull);
        if (ci != g_by_addr.end()) return ci->second;
    }
    return nullptr;
}

inline GClass* find_class_by_name(const char* name) {
    std::lock_guard<std::mutex> l(g_mu);
    auto it = g_by_name.find(name);
    return it == g_by_name.end() ? nullptr : it->second;
}

inline bool is_kind_of(GClass* c, GClass* target) {
    for (; c; c = c->super)
        if (c == target) return true;
    return false;
}

}  // namespace objcrt
