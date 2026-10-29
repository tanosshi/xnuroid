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
};
// Render and job threads send messages too, so the registry is locked; objects themselves are per-thread in practice.
static std::mutex g_fake_mu;
static std::unordered_map<void*, FakeObjcObj*> g_fake_objc;
static FakeObjcObj* fake_lookup(uint64_t p) {
    std::lock_guard<std::mutex> l(g_fake_mu);
    auto it = g_fake_objc.find(reinterpret_cast<void*>(p));
    return it == g_fake_objc.end() ? nullptr : it->second;
}
static uint64_t fake_make(int kind, const std::string& s) {
    auto* o = new FakeObjcObj{kind, s};
    void* handle = calloc(1, 32);   // a real, distinct, non-null address to hand to the guest
    std::lock_guard<std::mutex> l(g_fake_mu);
    g_fake_objc[handle] = o;
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
        if (sel == "retain" || sel == "autorelease" || sel == "self" || sel == "copy" || sel == "alloc" ||
            sel == "allocWithZone:" || starts("init") || sel == "copyWithZone:") { frame.ret_x0 = frame.x[0]; return true; }
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
            frame.ret_x0 = h; return true;
        }
        // textures made from a descriptor / another texture inherit its values
        if (starts("newTextureWithDescriptor:") || starts("newTextureViewWithPixelFormat:")) {
            uint64_t h; FakeObjcObj* t = fake_metal_bag(&h);
            FakeObjcObj* src = starts("newTextureViewWithPixelFormat:") ? self : fake_lookup(frame.x[2]);
            if (src) t->props = src->props;
            if (starts("newTextureViewWithPixelFormat:")) t->props["pixelFormat"] = frame.x[2];
            frame.ret_x0 = h; return true;
        }
        // command buffers
        if (sel == "addCompletedHandler:" || sel == "addScheduledHandler:") { self->handlers.push_back(frame.x[2]); return true; }
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
            if (it == self->props.end()) { uint64_t h; fake_metal_bag(&h); it = self->props.emplace(key, h).first; }
            frame.ret_x0 = it->second; return true;
        }
        if (sel == "setObject:atIndexedSubscript:") { self->props["#obj" + std::to_string(frame.x[3])] = frame.x[2]; return true; }
        // property bag: setFoo: / foo
        if (starts("set") && colons == 1) {
            std::string key = lower_first(sel.substr(3, sel.size() - 4));
            self->props[key] = frame.x[2];
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
                                                   "Buffers", "Layout", "Options"};
            for (const char* suf : objprops) {
                size_t n = std::strlen(suf);
                if (sel.size() > n && sel.compare(sel.size() - n, n, suf) == 0) {
                    uint64_t h; fake_metal_bag(&h);
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
    static uint64_t dev = fake_make(3, "device");
    return reinterpret_cast<void*>(dev);
}
