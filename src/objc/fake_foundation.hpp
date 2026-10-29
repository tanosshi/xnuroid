// Fake Foundation / UIKit / QuartzCore / Metal objects answered by the engine-mode objc_msgSend fallback.
// Included once by android_loader.cpp after CallFrame, the image globals and objc_runtime.hpp are declared.
#pragma once

// Minimal Foundation for engine mode: one fake NSBundle and heap-allocated fake NSStrings.
// Pointers are tracked in a set (receivers can be arbitrary), so lookups never dereference unknown memory.
static thread_local double g_objc_ret_d[4];
static thread_local double g_objc_in_d[4];
extern "C" double* objc_in_d_ptr() { return g_objc_in_d; }
extern "C" double* objc_ret_d_ptr() { return g_objc_ret_d; }
static std::atomic<uint64_t> g_draws_this_frame{0};   // LOADER_DRAW_STATS
static thread_local uint64_t g_objc_in_x8;   // indirect-result pointer of the message being answered
extern "C" void objc_set_in_x8(uint64_t v) { g_objc_in_x8 = v; }
static void objc_ret_double(double v) { g_objc_ret_d[0] = v; }
static void objc_ret_float(float v) { g_objc_ret_d[0] = 0; std::memcpy(&g_objc_ret_d[0], &v, sizeof v); }
static void objc_ret_rect(double x, double y, double w, double h) { g_objc_ret_d[0] = x; g_objc_ret_d[1] = y; g_objc_ret_d[2] = w; g_objc_ret_d[3] = h; }

// kind 1 = bundle, 2 = string, 3 = Metal property bag, 4 = framework bundle, 5 = generic object, 6 = singleton,
// 7 = foreign class object, 8 = UIKit/QuartzCore/Foundation object with real state (see fake_uikit_message).
struct FakeObjcObj {
    int kind; std::string s;
    std::unordered_map<std::string, uint64_t> props;   // property bag: set<Name>: / <name>
    void* mem = nullptr; size_t len = 0;               // MTLBuffer storage
    std::vector<uint64_t> handlers;                    // command buffer completed/scheduled handler blocks
    std::vector<uint64_t> items;                       // NSArray elements, NSDictionary/NSMapTable keys, subviews
    std::vector<uint64_t> values;                      // NSDictionary/NSMapTable values (parallel to items)
    unsigned long mutations = 0;                       // fast-enumeration mutation counter
    double rect[4] = {0, 0, 0, 0};                     // UIView frame (origin, size) / CGSize in rect[2..3]
    // Lifetime (Metal objects, kind 3): Cocoa reference counting; at zero the object is destroyed a few frames later.
    std::atomic<int> refs{1};
    bool parent_owned = false;   // created as a property of another object, which holds its reference
    bool borrowed_mem = false;   // mem belongs to the guest (newBufferWithBytesNoCopy:)
    uint8_t gltype = 0;          // what props["#gl"] points to (mtl_gles.hpp)
    bool counted = false;        // a non-Metal object under retain/release (strings handed out +0)
};
// Render and job threads send messages too, so the registry is locked; objects themselves are per-thread in practice.
static std::mutex g_fake_mu;
static std::unordered_map<void*, FakeObjcObj*> g_fake_objc;
static FakeObjcObj* fake_lookup(uint64_t p) {
    std::lock_guard<std::mutex> l(g_fake_mu);
    auto it = g_fake_objc.find(reinterpret_cast<void*>(p));
    return it == g_fake_objc.end() ? nullptr : it->second;
}
// LOADER_FAKE_HANDLE_SIZE: bytes behind each fake object handle (debug aid for guest writes into fakes)
static size_t fake_handle_size() {
    static const size_t n = getenv("LOADER_FAKE_HANDLE_SIZE") ? strtoull(getenv("LOADER_FAKE_HANDLE_SIZE"), nullptr, 10) : 32;
    return n;
}
// Objects created while one message is answered (shim_objc_msgSend decides which of them it returns +0).
static thread_local std::vector<uint64_t> t_created;
static thread_local int t_msg_depth = 0;
static uint64_t fake_make(int kind, const std::string& s) {
    auto* o = new FakeObjcObj{kind, s};
    void* handle = calloc(1, fake_handle_size());   // a real, distinct, non-null address to hand to the guest
    {
        std::lock_guard<std::mutex> l(g_fake_mu);
        g_fake_objc[handle] = o;
    }
    if (t_msg_depth) t_created.push_back(reinterpret_cast<uint64_t>(handle));
    return reinterpret_cast<uint64_t>(handle);
}
// One kind-7 class object per foreign class name, shared by NSClassFromString, objc_getClass and the bound
// _OBJC_CLASS_$_ imports, so class identity comparisons (isKindOfClass:, ==) work.
static uint64_t fake_class_named(const std::string& name) {
    static std::mutex mu;
    static std::unordered_map<std::string, uint64_t> classes;
    std::lock_guard<std::mutex> l(mu);
    auto it = classes.find(name);
    if (it == classes.end()) it = classes.emplace(name, fake_make(7, name)).first;
    return it->second;
}
// Reads an NSString argument: one of our fake strings, or a compile-time @"..." literal (__NSConstantString:
// isa, flags, char* data, length) living in the loaded guest image.
static bool objc_string_value(uint64_t p, std::string& out) {
    if (!p) return false;
    if (FakeObjcObj* f = fake_lookup(p)) {
        if (f->kind != 2) return false;
        out = f->s;
        return true;
    }
    uint64_t base = reinterpret_cast<uint64_t>(g_loaded_image_base);
    uint64_t end = base + g_loaded_image_size;
    if (p < base || p + 32 > end) return false;
    uint64_t data = *reinterpret_cast<uint64_t*>(p + 16);
    uint64_t len = *reinterpret_cast<uint64_t*>(p + 24);
    if (data < base || data >= end || len > 4096) return false;
    out.assign(reinterpret_cast<const char*>(data), static_cast<size_t>(len));
    return true;
}
// kind 3 = generic fake Metal object (device, queue, buffer, texture, library, ...): capability queries say yes,
// factory selectors (new*/make*) return another fake, everything else is nil/0.
static uint64_t fake_metal_object() { return fake_make(3, "mtl"); }
static FakeObjcObj* fake_metal_bag(uint64_t* handle_out = nullptr) {
    uint64_t h = fake_make(3, "mtl");
    if (handle_out) *handle_out = h;
    return fake_lookup(h);
}

// ---- lifetime of Metal objects ----
// Retain/release/autorelease follow Cocoa rules for kind-3 objects and for strings returned +0; other fakes are immortal. A property holds a
// reference to its value when the key is public or one of the internal owning keys below.
static void mtl_gles_destroy(FakeObjcObj* o);   // mtl_gles.hpp: frees the GL object behind props["#gl"]
static bool owning_key(const std::string& k) {
    if (k.empty() || (k.size() > 2 && k.compare(k.size() - 2, 2, "#d") == 0)) return false;
    if (k[0] != '#') return true;
    return k.compare(0, 4, "#obj") == 0 || k == "#viewOf" || k == "#library" || k == "#options";
}
static FakeObjcObj* fake_counted(uint64_t h) {
    FakeObjcObj* o = h ? fake_lookup(h) : nullptr;
    return o && (o->kind == 3 || o->counted) ? o : nullptr;
}
// Every fake keeps a count, so references taken before an object becomes counted (native stores while the message
// that creates it is still running) are not lost; only counted objects and Metal objects die at zero.
static void fake_retain(uint64_t h) {
    if (FakeObjcObj* o = h ? fake_lookup(h) : nullptr) o->refs.fetch_add(1);
}
struct Grave { FakeObjcObj* obj; void* handle; uint64_t frame; };
// LOADER_FAKE_LIFE: remember dead Metal objects so later messages to them are reported (shim_objc_msgSend).
static bool fake_life_debug() { static const bool on = getenv("LOADER_FAKE_LIFE") != nullptr; return on; }
static std::unordered_map<uint64_t, std::string> g_fake_born, g_fake_dead;
static std::mutex g_grave_mu;
static std::vector<Grave> g_graves;
static std::atomic<uint64_t> g_present_count{0};   // advanced per presented frame (mtl_gles.hpp)
static void fake_release(uint64_t h) {
    FakeObjcObj* o = h ? fake_lookup(h) : nullptr;
    if (!o || o->refs.fetch_sub(1) != 1 || (o->kind != 3 && !o->counted)) return;
    // dead: unreachable through the registry at once, memory and GL objects freed a few frames later, since
    // encoders still running on other threads may hold raw pointers
    {
        std::lock_guard<std::mutex> l(g_fake_mu);
        g_fake_objc.erase(reinterpret_cast<void*>(h));
    }
    std::vector<uint64_t> children;
    for (auto& kv : o->props)
        if (owning_key(kv.first)) children.push_back(kv.second);
    if (o->kind != 3) {
        children.insert(children.end(), o->items.begin(), o->items.end());
        children.insert(children.end(), o->values.begin(), o->values.end());
    }
    {
        std::lock_guard<std::mutex> l(g_grave_mu);
        g_graves.push_back({o, reinterpret_cast<void*>(h), g_present_count.load()});
        if (fake_life_debug()) g_fake_dead[h] = (g_fake_born.count(h) ? g_fake_born[h] : std::string("?")) + " \"" + o->s.substr(0, 60) + "\"";
    }
    for (uint64_t c : children) fake_release(c);
}
// Called once per presented frame by the renderer (a thread with a GL context).
static void fake_reap_graves() {
    std::vector<Grave> due;
    {
        std::lock_guard<std::mutex> l(g_grave_mu);
        uint64_t now = g_present_count.load();
        for (size_t i = 0; i < g_graves.size();) {
            if (now >= g_graves[i].frame + 3) { due.push_back(g_graves[i]); g_graves[i] = g_graves.back(); g_graves.pop_back(); }
            else ++i;
        }
    }
    for (Grave& g : due) {
        mtl_gles_destroy(g.obj);
        if (g.obj->mem && !g.obj->borrowed_mem) free(g.obj->mem);
        delete g.obj;
        if (!fake_life_debug()) free(g.handle);   // kept while debugging so a dead address is never reused
    }
}
// Values copied wholesale from another bag (descriptor -> object) get their own references.
static void fake_retain_owned_props(FakeObjcObj* o) {
    for (auto& kv : o->props)
        if (owning_key(kv.first)) fake_retain(kv.second);
}
// Stores an object-valued property, holding a reference to the new value and dropping the old one.
static void fake_set_prop(FakeObjcObj* o, const std::string& key, uint64_t value) {
    uint64_t old = 0;
    auto it = o->props.find(key);
    if (it != o->props.end()) old = it->second;
    if (old == value) return;
    bool own = owning_key(key);
    if (own) fake_retain(value);
    o->props[key] = value;
    if (own && old) fake_release(old);
}

// Autorelease pools, per thread. A pool token is the stack height at push (+1 so it is never 0).
static thread_local std::vector<uint64_t> t_autorelease;
static void fake_autorelease(uint64_t h) {
    if (fake_counted(h)) t_autorelease.push_back(h);
}
static uint64_t fake_pool_push() { return t_autorelease.size() + 1; }
static void fake_pool_pop(uint64_t token) {
    size_t keep = token ? static_cast<size_t>(token - 1) : 0;
    while (t_autorelease.size() > keep) {
        uint64_t h = t_autorelease.back();
        t_autorelease.pop_back();
        fake_release(h);
    }
}
// Cocoa method families that return +1 (owned) results; everything else returns +0 (autoreleased).
static bool returns_retained(const std::string& sel) {
    for (const char* fam : {"alloc", "new", "copy", "mutableCopy", "init"}) {
        size_t n = std::strlen(fam);
        if (sel.compare(0, n, fam) == 0 && (sel.size() == n || !std::islower(static_cast<unsigned char>(sel[n])))) return true;
    }
    return false;
}
// A Metal object or string made while answering a +0 selector is owned by the current autorelease pool, unless a
// parent holds it. Strings returned +1 stay immortal: the native stores below did not always retain them.
// +0 class factories of NSArray/NSDictionary/NSSet/NSNumber; their results are never cached by this loader.
static bool collection_factory(const std::string& sel) {
    if (sel == "set") return true;
    for (const char* p : {"array", "dictionary", "setWith", "numberWith"}) {
        size_t n = std::strlen(p);
        if (sel.compare(0, n, p) == 0 && (sel.size() == n || sel[n] == ':' || std::isupper(static_cast<unsigned char>(sel[n])))) return true;
    }
    return false;
}
static void fake_autorelease_fresh(uint64_t ret, size_t mark, const std::string& sel) {
    if (!ret || t_created.size() == mark) return;
    if (std::find(t_created.begin() + mark, t_created.end(), ret) == t_created.end()) return;
    FakeObjcObj* o = fake_lookup(ret);
    if (!o) return;
    if (fake_life_debug()) { std::lock_guard<std::mutex> l(g_grave_mu); g_fake_born[ret] = sel; }
    if ((o->kind == 2 || (o->kind == 8 && collection_factory(sel))) && !o->parent_owned && !returns_retained(sel)) o->counted = true;
    if (!o->counted && o->kind != 3) return;
    if (!o->parent_owned && !returns_retained(sel)) fake_autorelease(ret);
}
static std::string lower_first(const std::string& s) {
    std::string r = s;
    if (!r.empty() && r[0] >= 'A' && r[0] <= 'Z') r[0] = static_cast<char>(r[0] + 32);
    return r;
}
static size_t colon_count(const std::string& s) { size_t n = 0; for (char c : s) n += c == ':'; return n; }
static void run_block(uint64_t block, uint64_t arg) {
    if (!block) return;
    void* inv = *reinterpret_cast<void**>(block + 16);   // Block_layout.invoke
    if (inv) reinterpret_cast<void (*)(uint64_t, uint64_t)>(inv)(block, arg);
}
// Metal objects are property bags: set<Name>: stores, <name> reads back, new*/make* create more bags, buffers own real
// memory, descriptors carry their values into the textures made from them, command buffers fire their handlers.
static bool mtl_gles_message(CallFrame& frame, const std::string& sel, FakeObjcObj* self);   // mtl_gles.hpp

static bool fake_metal_message(CallFrame& frame, const std::string& sel) {
    FakeObjcObj* self = fake_lookup(frame.x[0]);
    auto starts = [&](const char* p) { return sel.compare(0, std::strlen(p), p) == 0; };
    const size_t colons = colon_count(sel);
    const bool is_class = self && (self->kind == 7 || self->s.compare(0, 6, "class:") == 0);

    // LOADER_DUMP_MSL=<dir>: write every runtime-compiled Metal Shading Language source to <dir>/<n>.metal
    if (sel == "newLibraryWithSource:options:error:") {
        static const char* dir = getenv("LOADER_DUMP_MSL");
        static std::atomic<int> n{0};
        std::string src;
        if (dir && objc_string_value(frame.x[2], src)) {
            std::string path = std::string(dir) + "/" + std::to_string(n++) + ".metal";
            if (FILE* fp = fopen(path.c_str(), "wb")) { fwrite(src.data(), 1, src.size(), fp); fclose(fp); }
        }
    }
    // LOADER_DRAW_STATS=1: draws per presented frame, every 60 presents
    if (sel.compare(0, 4, "draw") == 0 && sel.find("Primitives") != std::string::npos) {
        static const bool on = getenv("LOADER_DRAW_STATS") != nullptr;
        if (on) ++g_draws_this_frame;
    }

    if (mtl_gles_message(frame, sel, self)) return true;

    // class-style constructors for descriptors
    if (sel == "texture2DDescriptorWithPixelFormat:width:height:mipmapped:" || sel == "textureCubeDescriptorWithPixelFormat:size:mipmapped:" ||
        sel == "textureBufferDescriptorWithPixelFormat:width:usage:") {
        uint64_t h; FakeObjcObj* d = fake_metal_bag(&h);
        d->props["pixelFormat"] = frame.x[2];
        d->props["width"] = frame.x[3];
        if (sel[7] == '2') { d->props["height"] = frame.x[4]; d->props["textureType"] = 2; d->props["mipmapLevelCount"] = frame.x[5] ? 1 + 10 : 1; }
        else if (sel[7] == 'C') { d->props["height"] = frame.x[3]; d->props["textureType"] = 5; d->props["mipmapLevelCount"] = frame.x[4] ? 1 + 10 : 1; }
        d->props["depth"] = 1; d->props["arrayLength"] = 1; d->props["sampleCount"] = 1;
        frame.ret_x0 = h; return true;
    }
    if (self && !is_class && self->kind == 3) {
        if (sel == "name") { frame.ret_x0 = fake_make(2, self->s == "device" ? "FakeGPU" : "fake"); return true; }
        if (sel == "description") { frame.ret_x0 = fake_make(2, "FakeMetalObject"); return true; }
        if (sel == "retain") { fake_retain(frame.x[0]); frame.ret_x0 = frame.x[0]; return true; }
        if (sel == "release") { fake_release(frame.x[0]); frame.ret_x0 = 0; return true; }
        if (sel == "autorelease") { fake_autorelease(frame.x[0]); frame.ret_x0 = frame.x[0]; return true; }
        if (sel == "copy" || sel == "copyWithZone:" || sel == "mutableCopy") { fake_retain(frame.x[0]); frame.ret_x0 = frame.x[0]; return true; }   // shared, +1
        if (sel == "self" || sel == "alloc" || sel == "allocWithZone:" || starts("init")) { frame.ret_x0 = frame.x[0]; return true; }
        if (sel == "contents") { frame.ret_x0 = reinterpret_cast<uint64_t>(self->mem); return true; }
        if (sel == "length") { frame.ret_x0 = self->len; return true; }
        // buffers
        if (sel == "newBufferWithLength:options:") {
            uint64_t h; FakeObjcObj* b = fake_metal_bag(&h);
            b->len = frame.x[2]; b->mem = calloc(1, b->len ? b->len : 1); b->props["storageMode"] = (frame.x[3] >> 4) & 0xF;
            frame.ret_x0 = h; return true;
        }
        if (sel == "newBufferWithBytes:length:options:") {
            uint64_t h; FakeObjcObj* b = fake_metal_bag(&h);
            b->len = frame.x[3]; b->mem = calloc(1, b->len ? b->len : 1);
            if (frame.x[2] && b->len) std::memcpy(b->mem, reinterpret_cast<const void*>(frame.x[2]), b->len);
            frame.ret_x0 = h; return true;
        }
        if (sel == "newBufferWithBytesNoCopy:length:options:deallocator:") {
            uint64_t h; FakeObjcObj* b = fake_metal_bag(&h);
            b->len = frame.x[3]; b->mem = reinterpret_cast<void*>(frame.x[2]);
            b->borrowed_mem = true;
            frame.ret_x0 = h; return true;
        }
        // textures made from a descriptor / another texture inherit its values
        if (starts("newTextureWithDescriptor:") || starts("newTextureViewWithPixelFormat:")) {
            uint64_t h; FakeObjcObj* t = fake_metal_bag(&h);
            FakeObjcObj* src = starts("newTextureViewWithPixelFormat:") ? self : fake_lookup(frame.x[2]);
            if (src) { t->props = src->props; fake_retain_owned_props(t); }
            if (starts("newTextureViewWithPixelFormat:")) t->props["pixelFormat"] = frame.x[2];
            frame.ret_x0 = h; return true;
        }
        // command buffers
        if (sel == "addCompletedHandler:" || sel == "addScheduledHandler:") { self->handlers.push_back(reinterpret_cast<uint64_t>(heap_block_copy(reinterpret_cast<void*>(frame.x[2])))); return true; }
        if (sel == "commit" || sel == "waitUntilCompleted" || sel == "waitUntilScheduled") {
            if (sel == "commit") {
                std::vector<uint64_t> hs;
                hs.swap(self->handlers);
                for (uint64_t blk : hs) run_block(blk, frame.x[0]);
            }
            return true;
        }
        if (sel == "status") { frame.ret_x0 = 4; return true; }                        // MTLCommandBufferStatusCompleted
        if (sel == "error") { frame.ret_x0 = 0; return true; }
        if (sel == "objectAtIndexedSubscript:") {
            std::string key = "#obj" + std::to_string(frame.x[2]);
            auto it = self->props.find(key);
            if (it == self->props.end()) {
                uint64_t h;
                fake_metal_bag(&h)->parent_owned = true;
                it = self->props.emplace(key, h).first;
            }
            frame.ret_x0 = it->second; return true;
        }
        if (sel == "setObject:atIndexedSubscript:") { fake_set_prop(self, "#obj" + std::to_string(frame.x[3]), frame.x[2]); return true; }
        // property bag: setFoo: / foo
        if (starts("set") && colons == 1) {
            std::string key = lower_first(sel.substr(3, sel.size() - 4));
            fake_set_prop(self, key, frame.x[2]);
            uint64_t bits; std::memcpy(&bits, &g_objc_in_d[0], 8);
            self->props[key + "#d"] = bits;
            return true;
        }
        if (colons == 0) {
            auto it = self->props.find(sel);
            if (it != self->props.end()) {
                frame.ret_x0 = it->second;
                auto dt = self->props.find(sel + "#d");
                if (dt != self->props.end()) { double d; std::memcpy(&d, &dt->second, 8); objc_ret_double(d); }
                return true;
            }
            // object-valued properties are created on first read
            static const char* const objprops[] = {"Descriptor", "Attachments", "Attachment", "Layouts", "Attributes", "Descriptors",
                                                   "Buffers", "Layout", "Options", "attributes", "layouts"};
            for (const char* suf : objprops) {
                size_t n = std::strlen(suf);
                if (sel.size() >= n && sel.compare(sel.size() - n, n, suf) == 0) {
                    uint64_t h;
                    fake_metal_bag(&h)->parent_owned = true;
                    self->props[sel] = h; frame.ret_x0 = h; return true;
                }
            }
        }
    }
    if (starts("supports") || sel == "isDepth24Stencil8PixelFormatSupported" || sel == "areProgrammableSamplePositionsSupported") {
        frame.ret_x0 = 1; return true;
    }
    // descriptor classes: +new, +alloc, +xxxDescriptor, +renderPassDescriptor ...
    if (is_class || (self == nullptr)) {
        if (sel == "new" || sel == "alloc" || sel == "allocWithZone:" || sel.find("Descriptor") != std::string::npos ||
            sel == "defaultCompileOptions" || sel == "renderPassDescriptor") {
            uint64_t h; fake_metal_bag(&h); frame.ret_x0 = h; return true;
        }
    }
    if (starts("new") || starts("make") || starts("heap") || sel == "device" || sel == "commandBuffer" ||
        sel == "commandBufferWithUnretainedReferences" || sel == "blitCommandEncoder" ||
        starts("renderCommandEncoderWith") || sel == "computeCommandEncoder" || sel == "parallelRenderCommandEncoderWithDescriptor:" ||
        sel == "commandQueue" || sel == "nextDrawable" || sel == "texture" || sel == "drawable") {
        uint64_t h; fake_metal_bag(&h);
        if (self) { FakeObjcObj* n = fake_lookup(h); n->props["#parent"] = frame.x[0]; }
        frame.ret_x0 = h; return true;
    }
    if (sel == "maxBufferLength" || sel == "recommendedMaxWorkingSetSize" || sel == "maxTextureSize") { frame.ret_x0 = 0x40000000ull; return true; }
    if (sel == "registryID") { frame.ret_x0 = 1; return true; }
    return false;
}

static bool fake_uikit_message(CallFrame& frame, const std::string& sel);   // fake_uikit.hpp

static bool fake_foundation(CallFrame& frame, const std::string& sel) {
    if (sel == "retain" || sel == "release" || sel == "autorelease" || sel == "copy" || sel == "copyWithZone:" || sel == "mutableCopy") {
        FakeObjcObj* c = fake_counted(frame.x[0]);
        if (c && c->kind != 3) {
            if (sel == "release") { fake_release(frame.x[0]); frame.ret_x0 = 0; return true; }
            if (sel == "autorelease") fake_autorelease(frame.x[0]);
            else fake_retain(frame.x[0]);
            frame.ret_x0 = frame.x[0];
            return true;
        }
    }
    if (fake_uikit_message(frame, sel)) return true;
    FakeObjcObj* self = fake_lookup(frame.x[0]);
    // --- foreign class objects (kind 7: _OBJC_CLASS_$_UIDevice, ...) ---
    if (self && self->kind == 7) {
        const std::string& cls = self->s;
        if (cls.compare(0, 3, "MTL") == 0 && fake_metal_message(frame, sel)) return true;
        if (sel == "stringWithFormat:") {   // variadic: keep the (unformatted) format string, enough for labels/logging
            std::string f;
            frame.ret_x0 = objc_string_value(frame.x[2], f) ? fake_make(2, f) : fake_make(2, "");
            return true;
        }
        if (sel == "alloc" || sel == "allocWithZone:" || sel == "new") { frame.ret_x0 = fake_make(5, cls); return true; }
        if (sel == "class" || sel == "self" || sel == "retain") { frame.ret_x0 = frame.x[0]; return true; }
        if (sel == "instancesRespondToSelector:" || sel == "respondsToSelector:") {
            std::string q;
            const char* qs = reinterpret_cast<const char*>(frame.x[2]);
            if (qs) q = qs;
            // Only claim the selectors this loader actually emulates.
            static const char* const yes[] = {"systemVersion", "currentDevice", "mainScreen", "sharedApplication", "processInfo",
                                              "nativeScale", "maximumFramesPerSecond", "nativeBounds", nullptr};
            bool ok = false;
            for (const char* const* y = yes; *y; ++y) if (q == *y) ok = true;
            frame.ret_x0 = ok;
            return true;
        }
        if (sel == "isSubclassOfClass:" || sel == "isKindOfClass:") { frame.ret_x0 = 0; return true; }
        if (sel == "name" || sel == "description") { frame.ret_x0 = fake_make(2, cls); return true; }
        // fall through: class-level singletons such as +currentDevice are handled below
    }
    // --- UIKit / Foundation singletons (kind 6, name in s) ---
    if (sel == "currentDevice" || sel == "mainScreen" || sel == "sharedApplication" || sel == "processInfo" ||
        sel == "currentLocale" || sel == "preferredLanguages") {
        static std::unordered_map<std::string, uint64_t> singles;
        const char* nm = sel == "currentDevice" ? "UIDevice" : sel == "mainScreen" ? "UIScreen" :
                         sel == "sharedApplication" ? "UIApplication" : sel == "processInfo" ? "NSProcessInfo" : "NSLocale";
        auto it = singles.find(nm);
        if (it == singles.end()) it = singles.emplace(nm, fake_make(6, nm)).first;
        frame.ret_x0 = it->second;
        return true;
    }
    if (self && self->kind == 6) {
        const std::string& cls = self->s;
        if (cls == "UIDevice") {
            if (sel == "systemVersion") { frame.ret_x0 = fake_make(2, "16.0"); return true; }
            if (sel == "systemName") { frame.ret_x0 = fake_make(2, "iOS"); return true; }
            if (sel == "model" || sel == "localizedModel") { frame.ret_x0 = fake_make(2, "iPhone"); return true; }
            if (sel == "name") { frame.ret_x0 = fake_make(2, "iPhone"); return true; }
            if (sel == "identifierForVendor") { frame.ret_x0 = fake_make(5, "uuid"); return true; }
            if (sel == "userInterfaceIdiom") { frame.ret_x0 = 0; return true; }       // UIUserInterfaceIdiomPhone
            if (sel == "orientation") { frame.ret_x0 = 1; return true; }              // portrait
            if (sel == "isMultitaskingSupported") { frame.ret_x0 = 1; return true; }
            if (sel == "batteryLevel") { objc_ret_float(1.0f); return true; }
            if (sel == "batteryState") { frame.ret_x0 = 2; return true; }
        } else if (cls == "UIScreen") {
            if (sel == "bounds" || sel == "applicationFrame") { objc_ret_rect(0, 0, 360, 780); return true; }
            if (sel == "nativeBounds") { objc_ret_rect(0, 0, 1080, 2340); return true; }
            if (sel == "scale" || sel == "nativeScale") { objc_ret_double(3.0); return true; }
            if (sel == "maximumFramesPerSecond") { frame.ret_x0 = 60; return true; }
            if (sel == "brightness") { objc_ret_double(0.5); return true; }
        } else if (cls == "UIApplication") {
            if (sel == "applicationState") { frame.ret_x0 = 0; return true; }           // active
            if (sel == "statusBarOrientation") { frame.ret_x0 = 1; return true; }
            if (sel == "isIdleTimerDisabled") { frame.ret_x0 = 0; return true; }
        } else if (cls == "NSProcessInfo") {
            if (sel == "physicalMemory") { frame.ret_x0 = 6ull << 30; return true; }
            if (sel == "processorCount" || sel == "activeProcessorCount") { frame.ret_x0 = 8; return true; }
            if (sel == "isLowPowerModeEnabled") { frame.ret_x0 = 0; return true; }
            if (sel == "systemUptime") { objc_ret_double(1000.0); return true; }
            if (sel == "processName") { frame.ret_x0 = fake_make(2, "UnityFramework"); return true; }
        }
        bool keep = sel == "retain" || sel == "autorelease" || sel == "self";
        frame.ret_x0 = keep ? frame.x[0] : 0;
        return true;
    }
    if (self && self->kind == 5) {      // generic foreign object (NSMutableArray, UIView, ...): inert, answers nil/0
        bool keep = sel.compare(0, 4, "init") == 0 || sel == "retain" || sel == "autorelease" || sel == "self" || sel == "copy" ||
                    sel == "mutableCopy" || sel == "alloc";
        frame.ret_x0 = keep ? frame.x[0] : 0;
        return true;
    }
    if (self && self->kind == 3) {
        if (fake_metal_message(frame, sel)) return true;
        frame.ret_x0 = 0;
        return true;
    }
    if (sel == "bundleWithPath:" || sel == "bundleForClass:" || sel == "bundleWithIdentifier:") {
        std::string path;
        if (objc_string_value(frame.x[2], path) && path.find("/System/Library/") != std::string::npos) {
            fprintf(stderr, "[OBJC] framework bundle %s -> fake, load succeeds\n", path.c_str());
            frame.ret_x0 = fake_make(4, path);   // kind 4 = Apple framework bundle: load succeeds
            return true;
        }
    }
    if (self && self->kind == 4) {
        if (sel == "load") { frame.ret_x0 = 1; return true; }
        if (sel == "isLoaded") { frame.ret_x0 = 1; return true; }
        if (sel == "unload") { frame.ret_x0 = 0; return true; }
        frame.ret_x0 = 0;
        return true;
    }
    if (sel == "mainBundle") {
        static uint64_t bundle = fake_make(1, game_dir());
        frame.ret_x0 = bundle;
        return true;
    }
    if (sel == "stringWithUTF8String:" || sel == "stringWithCString:encoding:") {
        const char* c = reinterpret_cast<const char*>(frame.x[2]);
        frame.ret_x0 = c ? fake_make(2, c) : 0;
        return true;
    }
    if (self && self->kind == 1) {
        if (sel == "bundlePath" || sel == "resourcePath" || sel == "executablePath" || sel == "privateFrameworksPath") {
            frame.ret_x0 = fake_make(2, self->s);
            return true;
        }
        if (sel == "bundleIdentifier") { frame.ret_x0 = fake_make(2, "com.fake.game"); return true; }
    }
    if (self && self->kind == 2) {
        if (sel == "UTF8String" || sel == "cString" || sel == "fileSystemRepresentation" || sel == "lossyCString") {
            frame.ret_x0 = reinterpret_cast<uint64_t>(self->s.c_str());
            return true;
        }
        if (sel == "length") { frame.ret_x0 = self->s.size(); return true; }
        if (sel == "stringByAppendingPathComponent:") {
            std::string a;
            frame.ret_x0 = objc_string_value(frame.x[2], a) ? fake_make(2, self->s + "/" + a) : 0;
            return true;
        }
        if (sel == "isEqualToString:") {
            std::string a;
            frame.ret_x0 = objc_string_value(frame.x[2], a) && a == self->s;
            return true;
        }
        if (sel == "description" || sel == "copy" || sel == "retain" || sel == "autorelease") { frame.ret_x0 = frame.x[0]; return true; }
    }
    return false;
}

extern "C" void* native_mtl_create_system_default_device() {
    static uint64_t dev = [] {
        uint64_t h = fake_make(3, "device");
        fake_lookup(h)->refs = 1 << 30;
        return h;
    }();
    return reinterpret_cast<void*>(dev);
}

// LOADER_FAKE_LIFE: report a message whose receiver or first arguments are dead fakes.
static void fake_life_check(const CallFrame& frame, const std::string& sel) {
    if (!fake_life_debug()) return;
    std::lock_guard<std::mutex> l(g_grave_mu);
    for (int a = 0; a < 4; ++a) {
        auto it = g_fake_dead.find(frame.x[a == 0 ? 0 : a + 1]);
        if (it != g_fake_dead.end())
            printf("[DEAD] %s arg%d is dead (born %s) lr=%#llx\n", sel.c_str(), a, it->second.c_str(),
                   static_cast<unsigned long long>(frame.lr - reinterpret_cast<uint64_t>(g_loaded_image_base)));
    }
}

// ARC and MRC entry points (objc_retain, objc_storeStrong, objc_setProperty, ...), bound by register_objc_runtime_natives.
extern "C" {
uint64_t native_objc_retain(uint64_t x) { fake_retain(x); return x; }
void native_objc_release(uint64_t x) { fake_release(x); }
uint64_t native_objc_autorelease(uint64_t x) { fake_autorelease(x); return x; }
uint64_t native_objc_retain_autorelease(uint64_t x) { fake_retain(x); fake_autorelease(x); return x; }
uint64_t native_objc_pool_push() { return fake_pool_push(); }
void native_objc_pool_pop(uint64_t token) { fake_pool_pop(token); }
void native_objc_store_strong(uint64_t* addr, uint64_t v) {
    if (!addr || *addr == v) return;
    uint64_t old = *addr;
    fake_retain(v);
    *addr = v;
    fake_release(old);
}
uint64_t native_objc_load_weak_retained(uint64_t* addr) { return native_objc_retain(addr ? *addr : 0); }
uint64_t native_objc_get_property(uint64_t self, void*, int64_t off, bool atomic) {
    uint64_t v = self ? *reinterpret_cast<uint64_t*>(self + off) : 0;
    return atomic ? native_objc_retain_autorelease(v) : v;
}
void native_objc_set_property(uint64_t self, void*, uint64_t v, int64_t off) {
    if (self) native_objc_store_strong(reinterpret_cast<uint64_t*>(self + off), v);
}
}
