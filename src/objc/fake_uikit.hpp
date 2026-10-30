// Fake UIKit / QuartzCore / Foundation containers with real state, plus a minimal main run loop.
//
// Unity's iOS glue (UnityAppController, UnityView, DisplayManager) runs as real guest code; it needs the UIKit objects
// it talks to to remember what it told them: views keep frames, subviews and their layer, windows keep their root view
// controller, CAMetalLayer hands out drawables sized from drawableSize, CADisplayLink calls its target, delayed
// performSelector: calls fire, and arrays/dictionaries/map tables actually store objects. Guest instances whose class
// derives from a UIKit class (UnityView : UIView) get the same UIKit state through a side table.
#pragma once

extern "C" void objc_msgSend_guest();
extern "C" void host_vibrate(int ms, int amplitude);   // haptics.cpp

namespace fakeui {

using Send0 = uint64_t (*)(uint64_t, const char*);
using Send1 = uint64_t (*)(uint64_t, const char*, uint64_t);
using Send2 = uint64_t (*)(uint64_t, const char*, uint64_t, uint64_t);
using SendRect = uint64_t (*)(uint64_t, const char*, double, double, double, double);

// user_defaults.hpp
inline uint64_t standard_defaults();
inline bool defaults_message(CallFrame& frame, FakeObjcObj* d, const std::string& sel);
inline void defaults_flush(bool force);
inline uint64_t send(uint64_t o, const char* s) { return reinterpret_cast<Send0>(&objc_msgSend_guest)(o, objcrt::intern(s)); }
inline uint64_t send(uint64_t o, const char* s, uint64_t a) { return reinterpret_cast<Send1>(&objc_msgSend_guest)(o, objcrt::intern(s), a); }
inline uint64_t send(uint64_t o, const char* s, uint64_t a, uint64_t b) {
    return reinterpret_cast<Send2>(&objc_msgSend_guest)(o, objcrt::intern(s), a, b);
}
inline uint64_t send_sel(uint64_t o, const char* sel_ptr, uint64_t a) { return reinterpret_cast<Send1>(&objc_msgSend_guest)(o, sel_ptr, a); }
inline bool guest_implements(uint64_t o, const char* s) { return objcrt::lookup_imp(o, s) != 0; }

constexpr double kScreenW = screen::kWidth, kScreenScale = screen::kScale;

inline double getd(FakeObjcObj* o, const std::string& k, double def) {
    auto it = o->props.find(k + "#d");
    if (it == o->props.end()) return def;
    double d;
    std::memcpy(&d, &it->second, 8);
    return d;
}
inline void setd(FakeObjcObj* o, const std::string& k, double v) {
    uint64_t b;
    std::memcpy(&b, &v, 8);
    o->props[k + "#d"] = b;
}
inline uint64_t getp(FakeObjcObj* o, const std::string& k) {
    auto it = o->props.find(k);
    return it == o->props.end() ? 0 : it->second;
}

// Classes whose instances are kind-8 stateful fakes. Anything else allocated from a foreign class stays inert (kind 5).
inline bool is_stateful_class(const std::string& c) {
    static const std::unordered_set<std::string> k = {
        "UIWindow", "UIView", "UIViewController", "UIImageView", "UIActivityIndicatorView", "UIScreenMode", "UIColor",
        "CALayer", "CAMetalLayer", "CAMetalDrawable", "CADisplayLink", "NSArray", "NSMutableArray", "NSDictionary",
        "NSMutableDictionary", "NSMapTable", "NSNumber", "NSRunLoop", "NSNotificationCenter", "NSUserDefaults",
        "AVAudioSession", "NSAssertionHandler", "NSDate", "NSMutableSet", "NSSet", "NSURL", "NSError", "AVURLAsset", "UITouch", "UIEvent",
        "UIImpactFeedbackGenerator", "UINotificationFeedbackGenerator", "UISelectionFeedbackGenerator",
        "AVAssetResourceLoader"};
    return k.count(c) != 0;
}
inline const char* parent_class(const std::string& c) {
    static const std::unordered_map<std::string, const char*> p = {
        {"UIWindow", "UIView"}, {"UIImageView", "UIView"}, {"UIActivityIndicatorView", "UIView"}, {"UIView", "UIResponder"},
        {"UIViewController", "UIResponder"}, {"UIResponder", "NSObject"}, {"CAMetalLayer", "CALayer"},
        {"CALayer", "NSObject"}, {"NSMutableArray", "NSArray"}, {"NSMutableDictionary", "NSDictionary"},
        {"NSMutableString", "NSString"}, {"NSMutableSet", "NSSet"}};
    auto it = p.find(c);
    return it == p.end() ? (c == "NSObject" ? nullptr : "NSObject") : it->second;
}
inline bool is_container(const std::string& c) {
    return c == "NSArray" || c == "NSMutableArray" || c == "NSDictionary" || c == "NSMutableDictionary" || c == "NSMapTable" ||
           c == "NSSet" || c == "NSMutableSet" || c == "NSUserDefaults";
}
inline bool is_view_class(const std::string& c) {
    for (const char* p = c.c_str(); p; ) {
        std::string n = p;
        if (n == "UIView") return true;
        p = parent_class(n);
    }
    return false;
}

// Side table: UIKit state for guest instances that subclass UIKit classes.
inline std::mutex g_guest_mu;
inline std::unordered_map<uint64_t, FakeObjcObj*> g_guest_state;

// The fake state for o (a kind-8 fake or a UIKit-derived guest instance), with its UIKit class name in s.
inline FakeObjcObj* ui_state(uint64_t o) {
    if (FakeObjcObj* f = fake_lookup(o)) return f->kind == 8 ? f : nullptr;
    bool is_cls = false;
    objcrt::GClass* gc = objcrt::guest_class_for_object(o, &is_cls);
    if (!gc || is_cls) return nullptr;
    std::string base = objcrt::foreign_base(gc);
    if (base.empty() || base == "NSObject") return nullptr;
    std::lock_guard<std::mutex> l(g_guest_mu);
    auto it = g_guest_state.find(o);
    if (it != g_guest_state.end()) return it->second;
    auto* st = new FakeObjcObj{8, base};
    g_guest_state[o] = st;
    return st;
}

// Class-name chain of an object (guest classes first, then the UIKit/Foundation parents).
inline std::vector<std::string> class_chain(uint64_t o) {
    std::vector<std::string> out;
    std::string base;
    bool is_cls = false;
    if (objcrt::GClass* gc = objcrt::guest_class_for_object(o, &is_cls)) {
        if (is_cls) return out;
        for (objcrt::GClass* c = gc; c; c = c->super) out.push_back(c->name);
        base = objcrt::foreign_base(gc);
        if (base.empty()) base = "NSObject";
    } else if (FakeObjcObj* f = fake_lookup(o)) {
        if (f->kind == 2) base = "NSString";
        else if (f->kind == 1 || f->kind == 4) base = "NSBundle";
        else if (f->kind == 5 || f->kind == 6 || f->kind == 8) base = f->s;
        else return out;
    } else {
        return out;
    }
    for (const char* p = base.c_str(); p; p = parent_class(out.back())) out.push_back(p);
    return out;
}
inline std::string class_name_of_class(uint64_t cls) {
    bool is_cls = false;
    if (objcrt::GClass* gc = objcrt::guest_class_for_object(cls, &is_cls)) return is_cls ? gc->name : "";
    if (FakeObjcObj* f = fake_lookup(cls)) {
        if (f->kind == 7) return f->s;
        if (f->kind == 3 && f->s.compare(0, 6, "class:") == 0) return f->s.substr(6);
    }
    return "";
}
inline bool is_kind_of(uint64_t o, uint64_t cls) {
    std::string want = class_name_of_class(cls);
    if (want.empty()) return false;
    for (const std::string& n : class_chain(o))
        if (n == want) return true;
    return false;
}

inline uint64_t alloc_for_class(const std::string& cls) {
    if (cls == "NSString" || cls == "NSMutableString") return fake_make(2, "");
    return fake_make(is_stateful_class(cls) ? 8 : 5, cls);
}

inline uint64_t new_array(const std::vector<uint64_t>& items) {
    uint64_t h = fake_make(8, "NSArray");
    fake_lookup(h)->items = items;
    for (uint64_t i : items) fake_retain(i);
    return h;
}

inline bool keys_equal(uint64_t a, uint64_t b) {
    if (a == b) return true;
    std::string sa, sb;
    return objc_string_value(a, sa) && objc_string_value(b, sb) && sa == sb;
}
inline long find_key(FakeObjcObj* o, uint64_t k) {
    for (size_t i = 0; i < o->items.size(); ++i)
        if (keys_equal(o->items[i], k)) return static_cast<long>(i);
    return -1;
}

// ---- run loop state: delayed performs, display links, views waiting for layout ----
struct DelayedPerform { uint64_t target; const char* sel; uint64_t arg; double due; };
inline std::mutex g_loop_mu;
inline std::vector<DelayedPerform> g_delayed;
inline std::vector<uint64_t> g_display_links;
inline std::vector<uint64_t> g_needs_layout;
inline uint64_t g_key_window = 0, g_app_delegate = 0;
inline bool g_appear_pending = false;
inline std::atomic<uint64_t> g_presents{0}, g_drawables{0};

inline double now_s() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
inline void mark_needs_layout(uint64_t v) {
    std::lock_guard<std::mutex> l(g_loop_mu);
    for (uint64_t x : g_needs_layout) if (x == v) return;
    g_needs_layout.push_back(v);
}

inline uint64_t main_screen() { return send(fake_class_named("UIScreen"), "mainScreen"); }

// The view's layer: [[[self class] layerClass] layer] in UIKit; a CAMetalLayer when +layerClass says so.
inline uint64_t view_layer(uint64_t view, FakeObjcObj* st) {
    if (uint64_t l = getp(st, "layer")) return l;
    std::string lc = "CALayer";
    bool is_cls = false;
    if (objcrt::GClass* gc = objcrt::guest_class_for_object(view, &is_cls)) {
        if (!is_cls && objcrt::lookup_imp(gc->addr, "layerClass")) {
            std::string n = class_name_of_class(send(gc->addr, "layerClass"));
            if (!n.empty()) lc = n;
        }
    }
    uint64_t layer = fake_make(8, lc);
    FakeObjcObj* ls = fake_lookup(layer);
    ls->props["#view"] = view;
    setd(ls, "contentsScale", getd(st, "contentScaleFactor", 1.0));
    st->props["layer"] = layer;
    printf("[UI] layer for view %#llx: %s\n", static_cast<unsigned long long>(view), lc.c_str());
    fflush(stdout);
    return layer;
}

inline void view_set_frame(uint64_t view, FakeObjcObj* st, double x, double y, double w, double h) {
    bool resized = st->rect[2] != w || st->rect[3] != h;
    st->rect[0] = x; st->rect[1] = y; st->rect[2] = w; st->rect[3] = h;
    if (resized) mark_needs_layout(view);
}

inline void add_subview(uint64_t parent, FakeObjcObj* ps, uint64_t child) {
    FakeObjcObj* cs = ui_state(child);
    if (!cs) return;
    if (uint64_t old = getp(cs, "superview")) {
        if (FakeObjcObj* os = ui_state(old)) {
            auto& v = os->items;
            v.erase(std::remove(v.begin(), v.end(), child), v.end());
            ++os->mutations;
        }
    }
    auto& v = ps->items;
    v.erase(std::remove(v.begin(), v.end(), child), v.end());
    v.push_back(child);
    ++ps->mutations;
    cs->props["superview"] = parent;
    mark_needs_layout(child);
}
inline uint64_t window_of(uint64_t view) {
    for (int depth = 0; view && depth < 64; ++depth) {
        FakeObjcObj* s = ui_state(view);
        if (!s) return 0;
        if (s->s == "UIWindow") return view;
        view = getp(s, "superview");
    }
    return 0;
}

// UIViewController.view: loads lazily (guest -loadView if it has one, else a screen-sized UIView), then -viewDidLoad.
inline uint64_t controller_view(uint64_t vc, FakeObjcObj* st) {
    if (uint64_t v = getp(st, "view")) return v;
    if (guest_implements(vc, "loadView")) send(vc, "loadView");
    if (!getp(st, "view")) {
        uint64_t v = fake_make(8, "UIView");
        FakeObjcObj* vs = fake_lookup(v);
        vs->rect[2] = kScreenW; vs->rect[3] = screen::g_height;
        st->props["view"] = v;
        vs->props["#viewController"] = vc;
    }
    if (guest_implements(vc, "viewDidLoad")) send(vc, "viewDidLoad");
    return getp(st, "view");
}

// CAMetalLayer.nextDrawable: three drawables per layer, re-created when drawableSize or pixelFormat change.
inline uint64_t next_drawable(uint64_t layer, FakeObjcObj* ls) {
    static std::mutex mu;
    std::lock_guard<std::mutex> l(mu);
    double w = getd(ls, "drawableSize.w", 0), h = getd(ls, "drawableSize.h", 0);
    if (w <= 0 || h <= 0) {
        uint64_t view = getp(ls, "#view");
        FakeObjcObj* vs = view ? ui_state(view) : nullptr;
        double sc = getd(ls, "contentsScale", 1.0);
        w = (vs ? vs->rect[2] : kScreenW) * sc;
        h = (vs ? vs->rect[3] : screen::g_height) * sc;
    }
    uint64_t pf = ls->props.count("pixelFormat") ? ls->props["pixelFormat"] : 80;   // MTLPixelFormatBGRA8Unorm
    const int count = g_drawable_count.load();
    uint64_t key = (static_cast<uint64_t>(w) << 32) ^ (static_cast<uint64_t>(h) << 8) ^ pf ^ (static_cast<uint64_t>(count) << 56);
    if (getp(ls, "#drawableKey") != key || ls->values.empty()) {
        ls->values.clear();
        for (int i = 0; i < count; ++i) {
            uint64_t d = fake_make(8, "CAMetalDrawable");
            uint64_t t;
            FakeObjcObj* tex = fake_metal_bag(&t);
            tex->props["width"] = static_cast<uint64_t>(w);
            tex->props["height"] = static_cast<uint64_t>(h);
            tex->props["depth"] = 1;
            tex->props["pixelFormat"] = pf;
            tex->props["textureType"] = 2;
            tex->props["sampleCount"] = 1;
            tex->props["mipmapLevelCount"] = 1;
            tex->props["arrayLength"] = 1;
            tex->props["usage"] = 4;          // MTLTextureUsageRenderTarget
            tex->props["storageMode"] = 2;    // MTLStorageModePrivate
            tex->props["framebufferOnly"] = getp(ls, "framebufferOnly");
            FakeObjcObj* ds = fake_lookup(d);
            ds->props["texture"] = t;
            ds->props["layer"] = layer;
            ds->props["drawableID"] = static_cast<uint64_t>(i);
            ls->values.push_back(d);
        }
        ls->props["#drawableKey"] = key;
        printf("[UI] CAMetalLayer %#llx %d drawables %.0fx%.0f pixelFormat %llu\n", static_cast<unsigned long long>(layer), count, w, h,
               static_cast<unsigned long long>(pf));
        fflush(stdout);
    }
    uint64_t idx = getp(ls, "#next") % ls->values.size();
    ls->props["#next"] = idx + 1;
    ++g_drawables;
    return ls->values[idx];
}

// Messages only meaningful on classes (kind-7 receivers).
inline bool class_message(CallFrame& frame, const std::string& cls, const std::string& sel) {
    const double* in = g_objc_in_d;
    if (sel == "alloc" || sel == "allocWithZone:" || sel == "new") {
        if (!is_stateful_class(cls) && cls != "NSString" && cls != "NSMutableString") return false;
        frame.ret_x0 = alloc_for_class(cls);
        return true;
    }
    if (cls == "NSArray" || cls == "NSMutableArray") {
        if (sel == "array" || sel == "arrayWithCapacity:") { frame.ret_x0 = fake_make(8, cls); return true; }
        if (sel == "arrayWithObject:") { frame.ret_x0 = new_array({frame.x[2]}); return true; }
        if (sel == "arrayWithObjects:count:") {
            const uint64_t* p = reinterpret_cast<const uint64_t*>(frame.x[2]);
            frame.ret_x0 = new_array(p ? std::vector<uint64_t>(p, p + frame.x[3]) : std::vector<uint64_t>{});
            return true;
        }
    }
    if (cls == "NSDictionary" || cls == "NSMutableDictionary") {
        if (sel == "dictionary" || sel == "dictionaryWithCapacity:") { frame.ret_x0 = fake_make(8, cls); return true; }
        if (sel == "dictionaryWithObjects:forKeys:count:") {
            uint64_t h = fake_make(8, cls);
            FakeObjcObj* d = fake_lookup(h);
            const uint64_t* vals = reinterpret_cast<const uint64_t*>(frame.x[2]);
            const uint64_t* keys = reinterpret_cast<const uint64_t*>(frame.x[3]);
            for (uint64_t i = 0; vals && keys && i < frame.x[4]; ++i) {
                d->items.push_back(keys[i]); d->values.push_back(vals[i]);
                fake_retain(keys[i]); fake_retain(vals[i]);
            }
            frame.ret_x0 = h;
            return true;
        }
    }
    if (cls == "NSMapTable" && (sel.compare(0, 12, "mapTableWith") == 0 || sel.find("ObjectsMapTable") != std::string::npos)) {
        frame.ret_x0 = fake_make(8, "NSMapTable");
        return true;
    }
    if (cls == "NSNumber" && sel.compare(0, 10, "numberWith") == 0) {
        uint64_t h = fake_make(8, "NSNumber");
        FakeObjcObj* n = fake_lookup(h);
        bool fp = sel == "numberWithFloat:" || sel == "numberWithDouble:";
        double v = fp ? in[0] : static_cast<double>(static_cast<int64_t>(frame.x[2]));
        if (sel == "numberWithFloat:") { float f; std::memcpy(&f, &in[0], 4); v = f; }
        if (sel == "numberWithBool:") v = (frame.x[2] & 0xff) ? 1 : 0;
        if (fp) n->props["#real"] = 1;
        n->props["value"] = fp ? static_cast<uint64_t>(static_cast<int64_t>(v)) : frame.x[2];
        setd(n, "value", v);
        frame.ret_x0 = h;
        return true;
    }
    if (cls == "UIScreen" && sel == "screens") { frame.ret_x0 = new_array({main_screen()}); return true; }
    if (cls == "CADisplayLink" && sel == "displayLinkWithTarget:selector:") {
        uint64_t h = fake_make(8, "CADisplayLink");
        FakeObjcObj* d = fake_lookup(h);
        d->props["target"] = frame.x[2];
        d->props["action"] = frame.x[3];
        printf("[UI] CADisplayLink %#llx -> [%#llx %s]\n", static_cast<unsigned long long>(h),
               static_cast<unsigned long long>(frame.x[2]), reinterpret_cast<const char*>(frame.x[3]));
        fflush(stdout);
        frame.ret_x0 = h;
        return true;
    }
    if (cls == "CAMetalLayer" && sel == "layer") { frame.ret_x0 = fake_make(8, cls); return true; }
    // Singletons
    static const std::pair<const char*, const char*> singles[] = {
        {"NSRunLoop", "currentRunLoop"}, {"NSRunLoop", "mainRunLoop"}, {"NSNotificationCenter", "defaultCenter"},
        {"AVAudioSession", "sharedInstance"}, {"NSAssertionHandler", "currentHandler"}};
    if (cls == "NSUserDefaults" && sel == "standardUserDefaults") { frame.ret_x0 = standard_defaults(); return true; }
    for (const auto& s : singles) {
        if (cls == s.first && sel == s.second) {
            static std::mutex mu;
            static std::unordered_map<std::string, uint64_t> made;
            std::lock_guard<std::mutex> l(mu);
            auto it = made.find(cls);
            if (it == made.end()) it = made.emplace(cls, fake_make(8, cls)).first;
            frame.ret_x0 = it->second;
            return true;
        }
    }
    if (cls == "NSDate" && sel.compare(0, 4, "date") == 0) { frame.ret_x0 = fake_make(8, "NSDate"); return true; }
    if (cls == "NSURL" && (sel == "URLWithString:" || sel == "fileURLWithPath:" || sel == "fileURLWithPath:isDirectory:")) {
        uint64_t h = fake_make(8, "NSURL");
        std::string s;
        objc_string_value(frame.x[2], s);
        if (sel != "URLWithString:" && s.compare(0, 7, "file://") != 0) s = "file://" + s;
        fake_lookup(h)->props["absoluteString"] = fake_make(2, s);
        frame.ret_x0 = h;
        return true;
    }
    if (cls == "AVURLAsset" && sel == "URLAssetWithURL:options:") {
        uint64_t h = fake_make(8, "AVURLAsset");
        fake_lookup(h)->props["URL"] = frame.x[2];
        fake_retain(frame.x[2]);
        frame.ret_x0 = h;
        return true;
    }
    if (cls == "UIColor" && (sel.size() > 5 && sel.compare(sel.size() - 5, 5, "Color") == 0)) { frame.ret_x0 = fake_make(8, "UIColor"); return true; }
    if (cls == "UIColor" && sel.compare(0, 9, "colorWith") == 0) { frame.ret_x0 = fake_make(8, "UIColor"); return true; }
    return false;
}

inline bool container_message(CallFrame& frame, FakeObjcObj* o, const std::string& sel) {
    const std::string& c = o->s;
    const bool dict = c == "NSDictionary" || c == "NSMutableDictionary" || c == "NSMapTable" || c == "NSUserDefaults";
    if (sel == "count") { frame.ret_x0 = o->items.size(); return true; }
    if (sel == "countByEnumeratingWithState:objects:count:") {
        // NSFastEnumerationState { state; itemsPtr; mutationsPtr; extra[5] }: hand out the whole backing array once.
        uint64_t* st = reinterpret_cast<uint64_t*>(frame.x[2]);
        if (!st || st[0]) { frame.ret_x0 = 0; return true; }
        st[0] = 1;
        st[1] = reinterpret_cast<uint64_t>(o->items.data());
        st[2] = reinterpret_cast<uint64_t>(&o->mutations);
        frame.ret_x0 = o->items.size();
        return true;
    }
    if (sel == "init" || sel == "initWithCapacity:" || sel == "copy" || sel == "mutableCopy") { frame.ret_x0 = frame.x[0]; return true; }
    if (sel == "removeAllObjects") { o->items.clear(); o->values.clear(); ++o->mutations; return true; }
    if (dict) {
        if (c == "NSUserDefaults" && defaults_message(frame, o, sel)) return true;
        if (sel == "objectForKey:" || sel == "objectForKeyedSubscript:" || sel == "valueForKey:") {
            long i = find_key(o, frame.x[2]);
            frame.ret_x0 = i < 0 ? 0 : o->values[i];
            return true;
        }
        if (sel == "setObject:forKey:" || sel == "setObject:forKeyedSubscript:" || sel == "setValue:forKey:") {
            long i = find_key(o, frame.x[3]);
            if (!frame.x[2]) {
                if (i >= 0) { o->items.erase(o->items.begin() + i); o->values.erase(o->values.begin() + i); }
            } else if (i < 0) {
                o->items.push_back(frame.x[3]); o->values.push_back(frame.x[2]);
                fake_retain(frame.x[3]); fake_retain(frame.x[2]);
            } else {
                o->values[i] = frame.x[2];
                fake_retain(frame.x[2]);
            }
            ++o->mutations;
            return true;
        }
        if (sel == "removeObjectForKey:") {
            long i = find_key(o, frame.x[2]);
            if (i >= 0) { o->items.erase(o->items.begin() + i); o->values.erase(o->values.begin() + i); ++o->mutations; }
            return true;
        }
        if (sel == "allKeys" || sel == "keyEnumerator") { frame.ret_x0 = new_array(o->items); return true; }
        if (sel == "allValues" || sel == "objectEnumerator") { frame.ret_x0 = new_array(o->values); return true; }
        if (sel == "boolForKey:" || sel == "integerForKey:") {
            long i = find_key(o, frame.x[2]);
            frame.ret_x0 = i < 0 ? 0 : send(o->values[i], sel == "boolForKey:" ? "boolValue" : "integerValue");
            return true;
        }
        return false;
    }
    if (sel == "objectAtIndex:" || sel == "objectAtIndexedSubscript:") {
        frame.ret_x0 = frame.x[2] < o->items.size() ? o->items[frame.x[2]] : 0;
        return true;
    }
    if (sel == "firstObject") { frame.ret_x0 = o->items.empty() ? 0 : o->items.front(); return true; }
    if (sel == "lastObject") { frame.ret_x0 = o->items.empty() ? 0 : o->items.back(); return true; }
    if (sel == "addObject:") { if (frame.x[2]) { o->items.push_back(frame.x[2]); fake_retain(frame.x[2]); ++o->mutations; } return true; }
    if (sel == "removeObject:") {
        o->items.erase(std::remove(o->items.begin(), o->items.end(), frame.x[2]), o->items.end());
        ++o->mutations;
        return true;
    }
    if (sel == "containsObject:") { frame.ret_x0 = std::find(o->items.begin(), o->items.end(), frame.x[2]) != o->items.end(); return true; }
    if (sel == "indexOfObject:") {
        auto it = std::find(o->items.begin(), o->items.end(), frame.x[2]);
        frame.ret_x0 = it == o->items.end() ? 0x7fffffffffffffffull : static_cast<uint64_t>(it - o->items.begin());
        return true;
    }
    return false;
}

inline bool view_message(CallFrame& frame, uint64_t self, FakeObjcObj* st, const std::string& sel, const double* in) {
    if (sel == "initWithFrame:") {
        st->rect[0] = in[0]; st->rect[1] = in[1]; st->rect[2] = in[2]; st->rect[3] = in[3];
        frame.ret_x0 = self;
        return true;
    }
    if (sel == "frame") { objc_ret_rect(st->rect[0], st->rect[1], st->rect[2], st->rect[3]); frame.ret_x0 = 0; return true; }
    if (sel == "bounds") { objc_ret_rect(0, 0, st->rect[2], st->rect[3]); frame.ret_x0 = 0; return true; }
    if (sel == "center") { objc_ret_rect(st->rect[0] + st->rect[2] / 2, st->rect[1] + st->rect[3] / 2, 0, 0); return true; }
    if (sel == "setFrame:") { view_set_frame(self, st, in[0], in[1], in[2], in[3]); return true; }
    if (sel == "setBounds:") { view_set_frame(self, st, st->rect[0], st->rect[1], in[2], in[3]); return true; }
    if (sel == "contentScaleFactor") { objc_ret_double(getd(st, "contentScaleFactor", 1.0)); return true; }
    if (sel == "setContentScaleFactor:") {
        setd(st, "contentScaleFactor", in[0]);
        if (uint64_t l = getp(st, "layer")) setd(fake_lookup(l), "contentsScale", in[0]);
        mark_needs_layout(self);
        return true;
    }
    if (sel == "layer") { frame.ret_x0 = view_layer(self, st); return true; }
    if (sel == "superview") { frame.ret_x0 = getp(st, "superview"); return true; }
    if (sel == "subviews") { frame.ret_x0 = new_array(st->items); return true; }
    if (sel == "addSubview:" || sel == "bringSubviewToFront:" || sel == "insertSubview:atIndex:") {
        if (sel == "bringSubviewToFront:" && getp(ui_state(frame.x[2]) ? ui_state(frame.x[2]) : st, "superview") != self) return true;
        add_subview(self, st, frame.x[2]);
        return true;
    }
    if (sel == "removeFromSuperview") {
        if (uint64_t p = getp(st, "superview"))
            if (FakeObjcObj* ps = ui_state(p)) {
                ps->items.erase(std::remove(ps->items.begin(), ps->items.end(), self), ps->items.end());
                ++ps->mutations;
            }
        st->props["superview"] = 0;
        return true;
    }
    if (sel == "window") { frame.ret_x0 = window_of(getp(st, "superview")) ? window_of(getp(st, "superview")) : (st->s == "UIWindow" ? self : 0); return true; }
    if (sel == "setNeedsLayout" || sel == "setNeedsDisplay") { if (sel == "setNeedsLayout") mark_needs_layout(self); return true; }
    if (sel == "layoutIfNeeded") {
        if (guest_implements(self, "layoutSubviews")) send(self, "layoutSubviews");
        return true;
    }
    if (sel == "layoutSubviews") return true;   // UIView's own implementation (guest overrides are dispatched first)
    if (sel == "safeAreaInsets") {
        objc_ret_rect(screen::g_insets[0], screen::g_insets[1], screen::g_insets[2], screen::g_insets[3]);
        frame.ret_x0 = 0;
        return true;
    }
    if (sel == "layoutMargins") { objc_ret_rect(0, 0, 0, 0); frame.ret_x0 = 0; return true; }
    if (sel == "screen") { frame.ret_x0 = main_screen(); return true; }
    if (sel == "isHidden") { frame.ret_x0 = getp(st, "hidden"); return true; }
    if (sel == "traitCollection" || sel == "nextResponder" || sel == "gestureRecognizers") { frame.ret_x0 = 0; return true; }
    if (sel == "convertPoint:toView:" || sel == "convertPoint:fromView:" || sel == "convertPoint:toCoordinateSpace:" ||
        sel == "convertPoint:fromCoordinateSpace:") { objc_ret_rect(in[0], in[1], 0, 0); return true; }
    if (st->s == "UIWindow") {
        if (sel == "rootViewController") { frame.ret_x0 = getp(st, "rootViewController"); return true; }
        if (sel == "setRootViewController:") {
            st->props["rootViewController"] = frame.x[2];
            if (FakeObjcObj* vcs = frame.x[2] ? ui_state(frame.x[2]) : nullptr) {
                uint64_t v = controller_view(frame.x[2], vcs);
                if (FakeObjcObj* vs = v ? ui_state(v) : nullptr) {
                    view_set_frame(v, vs, 0, 0, st->rect[2], st->rect[3]);
                    add_subview(self, st, v);
                }
            }
            if (g_key_window == self) { std::lock_guard<std::mutex> l(g_loop_mu); g_appear_pending = true; }
            printf("[UI] window %#llx rootViewController = %#llx\n", static_cast<unsigned long long>(self),
                   static_cast<unsigned long long>(frame.x[2]));
            fflush(stdout);
            return true;
        }
        if (sel == "makeKeyAndVisible" || sel == "makeKeyWindow") {
            { std::lock_guard<std::mutex> l(g_loop_mu); g_key_window = self; g_appear_pending = true; }
            st->props["hidden"] = 0;
            printf("[UI] window %#llx is key and visible\n", static_cast<unsigned long long>(self));
            fflush(stdout);
            return true;
        }
        if (sel == "isKeyWindow") { frame.ret_x0 = g_key_window == self; return true; }
    }
    return false;
}

inline bool controller_message(CallFrame& frame, uint64_t self, FakeObjcObj* st, const std::string& sel) {
    if (sel == "view") { frame.ret_x0 = controller_view(self, st); return true; }
    if (sel == "viewIfLoaded") { frame.ret_x0 = getp(st, "view"); return true; }
    if (sel == "isViewLoaded") { frame.ret_x0 = getp(st, "view") != 0; return true; }
    if (sel == "setView:") {
        st->props["view"] = frame.x[2];
        if (FakeObjcObj* vs = frame.x[2] ? ui_state(frame.x[2]) : nullptr) vs->props["#viewController"] = self;
        return true;
    }
    if (sel == "interfaceOrientation") { frame.ret_x0 = 1; return true; }
    if (sel == "presentedViewController" || sel == "presentingViewController" || sel == "navigationController" ||
        sel == "transitionCoordinator") { frame.ret_x0 = 0; return true; }
    // UIViewController's own (empty) lifecycle implementations, reached by guest [super viewDidAppear:] etc.
    if (sel.compare(0, 4, "view") == 0 && (sel.find("Appear") != std::string::npos || sel.find("Layout") != std::string::npos ||
                                           sel == "viewDidLoad")) return true;
    if (sel == "setNeedsStatusBarAppearanceUpdate" || sel == "setNeedsUpdateOfHomeIndicatorAutoHidden" ||
        sel == "setNeedsUpdateOfScreenEdgesDeferringSystemGestures") return true;
    return false;
}

inline bool layer_message(CallFrame& frame, uint64_t self, FakeObjcObj* st, const std::string& sel, const double* in) {
    if (sel == "nextDrawable") { frame.ret_x0 = next_drawable(self, st); return true; }
    if (sel == "drawableSize") { objc_ret_rect(getd(st, "drawableSize.w", 0), getd(st, "drawableSize.h", 0), 0, 0); return true; }
    if (sel == "setDrawableSize:") {
        if (getd(st, "drawableSize.w", 0) != in[0] || getd(st, "drawableSize.h", 0) != in[1]) {
            printf("[UI] CAMetalLayer %#llx drawableSize %.0fx%.0f\n", static_cast<unsigned long long>(self), in[0], in[1]);
            fflush(stdout);
        }
        setd(st, "drawableSize.w", in[0]);
        setd(st, "drawableSize.h", in[1]);
        return true;
    }
    if (sel == "contentsScale") { objc_ret_double(getd(st, "contentsScale", 1.0)); return true; }
    if (sel == "setContentsScale:") { setd(st, "contentsScale", in[0]); return true; }
    if (sel == "bounds" || sel == "frame") {
        uint64_t view = getp(st, "#view");
        FakeObjcObj* vs = view ? ui_state(view) : nullptr;
        objc_ret_rect(0, 0, vs ? vs->rect[2] : st->rect[2], vs ? vs->rect[3] : st->rect[3]);
        return true;
    }
    if (sel == "setBounds:" || sel == "setFrame:") { st->rect[2] = in[2]; st->rect[3] = in[3]; return true; }
    if (sel == "pixelFormat") { frame.ret_x0 = st->props.count("pixelFormat") ? st->props["pixelFormat"] : 80; return true; }
    if (sel == "delegate" || sel == "superlayer" || sel == "sublayers" || sel == "colorspace") { frame.ret_x0 = getp(st, sel); return true; }
    return false;
}

inline bool display_link_message(CallFrame& frame, uint64_t self, FakeObjcObj* st, const std::string& sel) {
    if (sel == "addToRunLoop:forMode:") {
        std::lock_guard<std::mutex> l(g_loop_mu);
        if (std::find(g_display_links.begin(), g_display_links.end(), self) == g_display_links.end()) g_display_links.push_back(self);
        printf("[UI] CADisplayLink %#llx added to run loop\n", static_cast<unsigned long long>(self));
        fflush(stdout);
        return true;
    }
    if (sel == "invalidate" || sel == "removeFromRunLoop:forMode:") {
        std::lock_guard<std::mutex> l(g_loop_mu);
        g_display_links.erase(std::remove(g_display_links.begin(), g_display_links.end(), self), g_display_links.end());
        return true;
    }
    if (sel == "setPaused:") { st->props["paused"] = frame.x[2] & 0xff; return true; }
    if (sel == "isPaused" || sel == "paused") { frame.ret_x0 = getp(st, "paused"); return true; }
    if (sel == "timestamp") { objc_ret_double(getd(st, "timestamp", now_s())); return true; }
    if (sel == "targetTimestamp") { objc_ret_double(getd(st, "targetTimestamp", now_s() + 1 / 60.0)); return true; }
    if (sel == "duration") { objc_ret_double(1 / 60.0); return true; }
    return false;
}

inline bool misc_message(CallFrame& frame, FakeObjcObj* st, const std::string& sel, const double* in) {
    const std::string& c = st->s;
    if (c == "NSNumber") {
        double v = getd(st, "value", 0);
        if (sel == "floatValue") { objc_ret_float(static_cast<float>(v)); return true; }
        if (sel == "doubleValue") { objc_ret_double(v); return true; }
        if (sel == "boolValue") { frame.ret_x0 = v != 0; return true; }
        if (sel == "objCType") { frame.ret_x0 = reinterpret_cast<uint64_t>(getp(st, "#real") ? "d" : "q"); return true; }
        if (sel == "stringValue" || sel == "description") {
            char buf[40];
            if (getp(st, "#real")) snprintf(buf, sizeof buf, "%.17g", v);
            else snprintf(buf, sizeof buf, "%lld", static_cast<long long>(static_cast<int64_t>(getp(st, "value"))));
            frame.ret_x0 = fake_make(2, buf);
            return true;
        }
        if (sel.size() > 5 && sel.compare(sel.size() - 5, 5, "Value") == 0) { frame.ret_x0 = static_cast<uint64_t>(static_cast<int64_t>(v)); return true; }
    }
    if (c == "UIScreenMode") {
        if (sel == "size") { objc_ret_rect(kScreenW * kScreenScale, screen::g_height * kScreenScale, 0, 0); return true; }
        if (sel == "pixelAspectRatio") { objc_ret_double(1.0); return true; }
    }
    if (c == "CAMetalDrawable") {
        if (sel == "texture" || sel == "layer" || sel == "drawableID") { frame.ret_x0 = getp(st, sel); return true; }
        if (sel == "present" || sel == "presentAtTime:" || sel == "presentAfterMinimumDuration:") { ++g_presents; return true; }
        if (sel == "addPresentedHandler:") { st->handlers.push_back(frame.x[2]); return true; }
    }
    if (c == "AVAudioSession") {
        if (sel.size() > 6 && sel.compare(sel.size() - 6, 6, "error:") == 0) { frame.ret_x0 = 1; return true; }
        if (sel == "outputVolume") { objc_ret_float(0.5f); return true; }
        if (sel == "sampleRate" || sel == "preferredSampleRate") { objc_ret_double(48000.0); return true; }
        if (sel == "IOBufferDuration" || sel == "preferredIOBufferDuration") { objc_ret_double(0.01); return true; }
        if (sel == "outputNumberOfChannels" || sel == "maximumOutputNumberOfChannels") { frame.ret_x0 = 2; return true; }
        if (sel == "category" || sel == "mode") { frame.ret_x0 = fake_make(2, sel == "category" ? "AVAudioSessionCategorySoloAmbient" : "AVAudioSessionModeDefault"); return true; }
        if (sel == "currentRoute" || sel == "isOtherAudioPlaying" || sel == "secondaryAudioShouldBeSilencedHint") { frame.ret_x0 = 0; return true; }
    }
    if (c == "NSAssertionHandler" && sel == "handleFailureInMethod:object:file:lineNumber:description:") {
        std::string file, desc;
        objc_string_value(frame.x[4], file);
        objc_string_value(frame.x[6], desc);
        const char* m = reinterpret_cast<const char*>(frame.x[2]);
        fprintf(stderr, "[ASSERT] %s %s:%llu %s\n", m ? m : "?", file.c_str(), static_cast<unsigned long long>(frame.x[5]), desc.c_str());
        printf("[ASSERT] %s %s:%llu %s\n", m ? m : "?", file.c_str(), static_cast<unsigned long long>(frame.x[5]), desc.c_str());
        fflush(stdout);
        return true;
    }
    if (c == "NSRunLoop" || c == "NSNotificationCenter") { frame.ret_x0 = 0; return true; }
    // Haptics: UIKit feedback generators -> device vibration (haptics.cpp)
    if (c == "UIImpactFeedbackGenerator") {
        if (sel == "initWithStyle:") { st->props["style"] = frame.x[2]; frame.ret_x0 = frame.x[0]; return true; }
        if (sel == "impactOccurred" || sel == "impactOccurredWithIntensity:") {
            // UIImpactFeedbackStyle light, medium, heavy, soft, rigid
            static const int ms[5] = {12, 20, 30, 15, 10}, amp[5] = {90, 150, 255, 70, 200};
            uint64_t s = getp(st, "style") < 5 ? getp(st, "style") : 1;
            double k = sel == "impactOccurred" ? 1.0 : std::clamp(in[0], 0.0, 1.0);
            host_vibrate(ms[s], std::max(1, static_cast<int>(amp[s] * k)));
            return true;
        }
        if (sel == "prepare") return true;
    }
    if (c == "UINotificationFeedbackGenerator") {
        if (sel == "notificationOccurred:") {   // success, warning, error
            static const int ms[3] = {40, 60, 90};
            host_vibrate(ms[frame.x[2] < 3 ? frame.x[2] : 0], 200);
            return true;
        }
        if (sel == "prepare") return true;
    }
    if (c == "UISelectionFeedbackGenerator") {
        if (sel == "selectionChanged") { host_vibrate(8, 80); return true; }
        if (sel == "prepare") return true;
    }
    if (c == "UITouch") {
        if (sel == "locationInView:" || sel == "preciseLocationInView:") { objc_ret_rect(getd(st, "x", 0), getd(st, "y", 0), 0, 0); return true; }
        if (sel == "previousLocationInView:" || sel == "precisePreviousLocationInView:") {
            objc_ret_rect(getd(st, "px", 0), getd(st, "py", 0), 0, 0);
            return true;
        }
        if (sel == "majorRadius") { objc_ret_double(20.0); return true; }
        if (sel == "majorRadiusTolerance") { objc_ret_double(5.0); return true; }
        if (sel == "force" || sel == "maximumPossibleForce" || sel == "azimuthAngleInView:") { objc_ret_double(0.0); return true; }
        if (sel == "altitudeAngle") { objc_ret_double(1.5707963267948966); return true; }
        if (sel == "timestamp") { objc_ret_double(getd(st, "timestamp", 0)); return true; }
    }
    if (c == "UIEvent") {
        if (sel == "allTouches" || sel == "touchesForView:" || sel == "touchesForWindow:") { frame.ret_x0 = getp(st, "touches"); return true; }
        if (sel == "type" || sel == "subtype") { frame.ret_x0 = 0; return true; }
        if (sel == "timestamp") { objc_ret_double(getd(st, "timestamp", 0)); return true; }
    }
    if (c == "NSURL") {
        std::string s;
        objc_string_value(getp(st, "absoluteString"), s);
        if (sel == "isFileURL") { frame.ret_x0 = s.compare(0, 7, "file://") == 0; return true; }
        if (sel == "path") { frame.ret_x0 = fake_make(2, s.compare(0, 7, "file://") == 0 ? s.substr(7) : s); return true; }
        if (sel == "scheme") { size_t c2 = s.find(':'); frame.ret_x0 = fake_make(2, c2 == std::string::npos ? "" : s.substr(0, c2)); return true; }
    }
    // AVFoundation without a decoder: assets finish loading as "failed", like a device that cannot play the format.
    if (c == "AVURLAsset") {
        if (sel == "initWithURL:options:") {
            st->props["URL"] = frame.x[2];
            fake_retain(frame.x[2]);
            std::string u;
            FakeObjcObj* url = fake_lookup(frame.x[2]);
            if (url) objc_string_value(getp(url, "absoluteString"), u);
            printf("[AV] AVURLAsset %s\n", u.c_str());
            fflush(stdout);
            frame.ret_x0 = frame.x[0];
            return true;
        }
        if (sel == "resourceLoader") {
            if (!getp(st, "resourceLoader")) st->props["resourceLoader"] = fake_make(8, "AVAssetResourceLoader");
            frame.ret_x0 = getp(st, "resourceLoader");
            return true;
        }
        if (sel == "loadValuesAsynchronouslyForKeys:completionHandler:") {
            std::lock_guard<std::mutex> l(g_loop_mu);
            g_delayed.push_back({frame.x[0], nullptr, reinterpret_cast<uint64_t>(heap_block_copy(reinterpret_cast<void*>(frame.x[3]))), now_s()});
            return true;
        }
        if (sel == "statusOfValueForKey:error:") {
            if (frame.x[3]) {
                uint64_t err = fake_make(8, "NSError");
                FakeObjcObj* e = fake_lookup(err);
                e->props["code"] = static_cast<uint64_t>(-11828);   // AVErrorFileFormatNotRecognized
                e->props["localizedDescription"] = fake_make(2, "Cannot Open");
                e->props["localizedFailureReason"] = fake_make(2, "No video decoder in this runtime");
                *reinterpret_cast<uint64_t*>(frame.x[3]) = err;
            }
            frame.ret_x0 = 3;   // AVKeyValueStatusFailed
            return true;
        }
        if (sel == "tracks" || sel == "tracksWithMediaType:") { frame.ret_x0 = new_array({}); return true; }
        if (sel == "isPlayable") { frame.ret_x0 = 0; return true; }
    }
    (void)in;
    return false;
}

// Binary property list (bplist00) -> fake Foundation objects: dict/array/string/number/bool. Data, dates and UIDs
// become nil; nothing in Info.plist that Unity reads uses them.
class BPlist {
public:
    explicit BPlist(std::vector<uint8_t> d) : d_(std::move(d)) {}
    uint64_t parse() {
        if (d_.size() < 40 || std::memcmp(d_.data(), "bplist00", 8) != 0) return 0;
        const uint8_t* t = d_.data() + d_.size() - 32;
        off_size_ = t[6];
        ref_size_ = t[7];
        num_ = be(t + 8, 8);
        uint64_t top = be(t + 16, 8);
        table_ = be(t + 24, 8);
        if (!off_size_ || !ref_size_ || table_ + num_ * off_size_ > d_.size() - 32) return 0;
        return object(top, 0);
    }

private:
    uint64_t be(const uint8_t* p, size_t n) const {
        uint64_t v = 0;
        for (size_t i = 0; i < n; ++i) v = (v << 8) | p[i];
        return v;
    }
    bool in_range(uint64_t off, uint64_t n) const { return off <= d_.size() && n <= d_.size() - off; }
    uint64_t offset_of(uint64_t ref) const { return ref < num_ ? be(d_.data() + table_ + ref * off_size_, off_size_) : ~0ull; }
    // Object length from the marker's low nibble, or a following int object when it is 0xF. Advances pos.
    bool length(uint8_t low, uint64_t& pos, uint64_t& len) const {
        if (low != 0xF) { len = low; return true; }
        if (!in_range(pos, 1) || (d_[pos] >> 4) != 0x1) return false;
        uint64_t n = 1ull << (d_[pos] & 0xF);
        if (n > 8 || !in_range(pos + 1, n)) return false;
        len = be(&d_[pos + 1], n);
        pos += 1 + n;
        return true;
    }
    uint64_t number(double v, uint64_t iv) const {
        uint64_t h = fake_make(8, "NSNumber");
        FakeObjcObj* n = fake_lookup(h);
        n->props["value"] = iv;
        setd(n, "value", v);
        return h;
    }
    uint64_t object(uint64_t ref, int depth) {
        uint64_t pos = offset_of(ref);
        if (depth > 32 || !in_range(pos, 1)) return 0;
        uint8_t m = d_[pos++], hi = m >> 4, lo = m & 0xF;
        uint64_t len = 0;
        switch (hi) {
            case 0x0:
                return (m == 0x08 || m == 0x09) ? number(m == 0x09, m == 0x09) : 0;
            case 0x1: {
                uint64_t n = 1ull << lo;
                if (n > 8 || !in_range(pos, n)) return 0;
                int64_t v = static_cast<int64_t>(be(&d_[pos], n));
                return number(static_cast<double>(v), static_cast<uint64_t>(v));
            }
            case 0x2: {
                uint64_t n = 1ull << lo, bits = in_range(pos, n) ? be(&d_[pos], n) : 0;
                double v = 0;
                if (n == 8) std::memcpy(&v, &bits, 8);
                else if (n == 4) { uint32_t b32 = static_cast<uint32_t>(bits); float f; std::memcpy(&f, &b32, 4); v = f; }
                return number(v, static_cast<uint64_t>(static_cast<int64_t>(v)));
            }
            case 0x5:
            case 0x6: {
                if (!length(lo, pos, len)) return 0;
                uint64_t bytes = hi == 0x5 ? len : len * 2;
                if (!in_range(pos, bytes)) return 0;
                std::string s;
                for (uint64_t i = 0; i < len; ++i) {
                    uint32_t c = hi == 0x5 ? d_[pos + i] : static_cast<uint32_t>(be(&d_[pos + 2 * i], 2));
                    if (c < 0x80) s.push_back(static_cast<char>(c));
                    else if (c < 0x800) { s.push_back(static_cast<char>(0xC0 | (c >> 6))); s.push_back(static_cast<char>(0x80 | (c & 0x3F))); }
                    else { s.push_back(static_cast<char>(0xE0 | (c >> 12))); s.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F))); s.push_back(static_cast<char>(0x80 | (c & 0x3F))); }
                }
                return fake_make(2, s);
            }
            case 0xA:
            case 0xD: {
                if (!length(lo, pos, len)) return 0;
                uint64_t refs = hi == 0xD ? len * 2 : len;
                if (!in_range(pos, refs * ref_size_)) return 0;
                auto rref = [&](uint64_t i) { return be(&d_[pos + i * ref_size_], ref_size_); };
                uint64_t h = fake_make(8, hi == 0xD ? "NSDictionary" : "NSArray");
                std::vector<uint64_t> keys, vals;
                for (uint64_t i = 0; i < len; ++i) {
                    if (hi == 0xD) { keys.push_back(object(rref(i), depth + 1)); vals.push_back(object(rref(len + i), depth + 1)); }
                    else vals.push_back(object(rref(i), depth + 1));
                }
                FakeObjcObj* o = fake_lookup(h);
                if (hi == 0xD) { o->items = keys; o->values = vals; } else { o->items = vals; }
                for (uint64_t k : keys) fake_retain(k);
                for (uint64_t v : vals) fake_retain(v);
                return h;
            }
            default:
                return 0;
        }
    }
    std::vector<uint8_t> d_;
    uint8_t off_size_ = 0, ref_size_ = 0;
    uint64_t num_ = 0, table_ = 0;
};

// XML property list -> the same fake objects (dict, array, string, integer, real, true/false; data/date -> nil).
class XmlPlist {
public:
    explicit XmlPlist(std::string s) : s_(std::move(s)) {}
    uint64_t parse() {
        size_t p = s_.find("<plist");
        if (p == std::string::npos) return 0;
        pos_ = s_.find('>', p);
        if (pos_ == std::string::npos) return 0;
        ++pos_;
        std::string tag;
        return next_tag(tag) ? value(tag, 0) : 0;
    }

private:
    // Moves past the next element start tag; tag gets its name ("dict", "/dict", "true/" ...). Skips comments.
    bool next_tag(std::string& tag) {
        for (;;) {
            size_t lt = s_.find('<', pos_);
            if (lt == std::string::npos) return false;
            if (s_.compare(lt, 4, "<!--") == 0) {
                size_t e = s_.find("-->", lt);
                if (e == std::string::npos) return false;
                pos_ = e + 3;
                continue;
            }
            size_t gt = s_.find('>', lt);
            if (gt == std::string::npos) return false;
            tag = s_.substr(lt + 1, gt - lt - 1);
            size_t sp = tag.find_first_of(" \t\r\n");
            if (sp != std::string::npos) tag = tag.substr(0, sp) + (tag.back() == '/' ? "/" : "");
            pos_ = gt + 1;
            return true;
        }
    }
    std::string text_until(const std::string& close) {
        size_t e = s_.find("</" + close, pos_);
        if (e == std::string::npos) e = s_.size();
        std::string raw = s_.substr(pos_, e - pos_), out;
        pos_ = s_.find('>', e);
        pos_ = pos_ == std::string::npos ? s_.size() : pos_ + 1;
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] != '&') { out.push_back(raw[i]); continue; }
            size_t semi = raw.find(';', i);
            std::string ent = semi == std::string::npos ? "" : raw.substr(i + 1, semi - i - 1);
            if (ent == "amp") out.push_back('&');
            else if (ent == "lt") out.push_back('<');
            else if (ent == "gt") out.push_back('>');
            else if (ent == "quot") out.push_back('"');
            else if (ent == "apos") out.push_back('\'');
            else { out.push_back('&'); continue; }
            i = semi;
        }
        return out;
    }
    uint64_t number(double v, uint64_t iv) {
        uint64_t h = fake_make(8, "NSNumber");
        FakeObjcObj* n = fake_lookup(h);
        n->props["value"] = iv;
        setd(n, "value", v);
        return h;
    }
    uint64_t value(const std::string& tag, int depth) {
        if (depth > 32) return 0;
        if (tag == "true/" || tag == "false/") return number(tag == "true/", tag == "true/");
        if (tag == "string") return fake_make(2, text_until("string"));
        if (tag == "string/") return fake_make(2, "");
        if (tag == "integer") { int64_t v = strtoll(text_until("integer").c_str(), nullptr, 10); return number(static_cast<double>(v), static_cast<uint64_t>(v)); }
        if (tag == "real") {
            double v = strtod(text_until("real").c_str(), nullptr);
            uint64_t h = number(v, static_cast<uint64_t>(static_cast<int64_t>(v)));
            fake_lookup(h)->props["#real"] = 1;
            return h;
        }
        if (tag == "data" || tag == "date") { text_until(tag); return 0; }
        if (tag == "array/" || tag == "dict/") return fake_make(8, tag == "dict/" ? "NSDictionary" : "NSArray");
        if (tag != "array" && tag != "dict") return 0;
        const bool dict = tag == "dict";
        uint64_t h = fake_make(8, dict ? "NSDictionary" : "NSArray");
        std::vector<uint64_t> keys, vals;
        std::string t;
        while (next_tag(t)) {
            if (t == "/" + tag) break;
            if (dict && t == "key") {
                keys.push_back(fake_make(2, text_until("key")));
                if (!next_tag(t)) break;
                vals.push_back(value(t, depth + 1));
            } else if (!dict) {
                vals.push_back(value(t, depth + 1));
            }
        }
        FakeObjcObj* o = fake_lookup(h);
        if (dict) {
            for (size_t i = 0; i < keys.size() && i < vals.size(); ++i)
                if (vals[i]) { o->items.push_back(keys[i]); o->values.push_back(vals[i]); fake_retain(keys[i]); fake_retain(vals[i]); }
        } else {
            for (uint64_t v : vals) if (v) { o->items.push_back(v); fake_retain(v); }
        }
        return h;
    }
    std::string s_;
    size_t pos_ = 0;
};

inline uint64_t info_dictionary() {
    static std::once_flag once;
    static uint64_t dict = 0;
    std::call_once(once, [] {
        std::string path = std::string(game_dir()) + "/Info.plist";
        std::vector<uint8_t> data;
        if (FILE* fp = fopen(path.c_str(), "rb")) {
            uint8_t buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, fp)) > 0) data.insert(data.end(), buf, buf + n);
            fclose(fp);
        }
        if (data.size() >= 8 && std::memcmp(data.data(), "bplist00", 8) == 0) dict = BPlist(std::move(data)).parse();
        else dict = XmlPlist(std::string(data.begin(), data.end())).parse();
        FakeObjcObj* d = dict ? fake_lookup(dict) : nullptr;
        printf("[UI] Info.plist %s: %zu keys\n", path.c_str(), d ? d->items.size() : static_cast<size_t>(0));
        fflush(stdout);
        if (!d) dict = fake_make(8, "NSDictionary");
    });
    return dict;
}

inline bool string_message(CallFrame& frame, FakeObjcObj* s, const std::string& sel) {
    std::string a;
    // initializers fill the string allocated by +[NSString alloc]
    if (sel == "initWithBytes:length:encoding:" || sel == "initWithBytesNoCopy:length:encoding:freeWhenDone:") {
        const char* b = reinterpret_cast<const char*>(frame.x[2]);
        s->s.assign(b ? b : "", b ? frame.x[3] : 0);
        frame.ret_x0 = frame.x[0];
        return true;
    }
    if (sel == "initWithUTF8String:" || sel == "initWithCString:encoding:") {
        const char* c = reinterpret_cast<const char*>(frame.x[2]);
        s->s = c ? c : "";
        frame.ret_x0 = c ? frame.x[0] : 0;
        return true;
    }
    if (sel == "initWithString:" || sel == "initWithFormat:") {
        objc_string_value(frame.x[2], s->s);
        frame.ret_x0 = frame.x[0];
        return true;
    }
    if (sel == "init") { frame.ret_x0 = frame.x[0]; return true; }
    if (sel == "appendString:") { if (objc_string_value(frame.x[2], a)) s->s += a; return true; }
    const std::string& v = s->s;
    if (sel == "stringByDeletingPathExtension") {
        size_t slash = v.rfind('/'), dot = v.rfind('.');
        frame.ret_x0 = fake_make(2, dot != std::string::npos && (slash == std::string::npos || dot > slash) ? v.substr(0, dot) : v);
        return true;
    }
    if (sel == "pathExtension") {
        size_t slash = v.rfind('/'), dot = v.rfind('.');
        frame.ret_x0 = fake_make(2, dot != std::string::npos && (slash == std::string::npos || dot > slash) ? v.substr(dot + 1) : "");
        return true;
    }
    if (sel == "lastPathComponent") { size_t sl = v.rfind('/'); frame.ret_x0 = fake_make(2, sl == std::string::npos ? v : v.substr(sl + 1)); return true; }
    if (sel == "stringByDeletingLastPathComponent") { size_t sl = v.rfind('/'); frame.ret_x0 = fake_make(2, sl == std::string::npos ? "" : v.substr(0, sl)); return true; }
    if (sel == "stringByAppendingString:") { objc_string_value(frame.x[2], a); frame.ret_x0 = fake_make(2, v + a); return true; }
    if (sel == "stringByReplacingOccurrencesOfString:withString:") {   // PlayerPrefs keys go through this
        std::string with, out;
        objc_string_value(frame.x[3], with);
        if (!objc_string_value(frame.x[2], a) || a.empty()) { frame.ret_x0 = fake_make(2, v); return true; }
        for (size_t p = 0;;) {
            size_t hit = v.find(a, p);
            out.append(v, p, hit == std::string::npos ? std::string::npos : hit - p);
            if (hit == std::string::npos) break;
            out += with;
            p = hit + a.size();
        }
        frame.ret_x0 = fake_make(2, out);
        return true;
    }
    if (sel == "hasPrefix:") { frame.ret_x0 = objc_string_value(frame.x[2], a) && v.compare(0, a.size(), a) == 0; return true; }
    if (sel == "hasSuffix:") {
        frame.ret_x0 = objc_string_value(frame.x[2], a) && v.size() >= a.size() && v.compare(v.size() - a.size(), a.size(), a) == 0;
        return true;
    }
    if (sel == "isEqual:") { frame.ret_x0 = objc_string_value(frame.x[2], a) && a == v; return true; }
    if (sel == "hash") { frame.ret_x0 = std::hash<std::string>()(v); return true; }
    if (sel == "intValue" || sel == "integerValue" || sel == "longLongValue") { frame.ret_x0 = static_cast<uint64_t>(strtoll(v.c_str(), nullptr, 10)); return true; }
    if (sel == "boolValue") { frame.ret_x0 = !v.empty() && (v[0] == 'Y' || v[0] == 'y' || v[0] == 'T' || v[0] == 't' || (v[0] >= '1' && v[0] <= '9')); return true; }
    if (sel == "floatValue") { objc_ret_float(strtof(v.c_str(), nullptr)); return true; }
    if (sel == "doubleValue") { objc_ret_double(strtod(v.c_str(), nullptr)); return true; }
    if (sel == "characterAtIndex:") { frame.ret_x0 = frame.x[2] < v.size() ? static_cast<uint8_t>(v[frame.x[2]]) : 0; return true; }
    if (sel == "class") { frame.ret_x0 = fake_class_named("NSString"); return true; }
    if (sel == "isKindOfClass:") { frame.ret_x0 = is_kind_of(frame.x[0], frame.x[2]); return true; }
    return false;
}

inline bool bundle_message(CallFrame& frame, const std::string& sel) {
    if (sel == "infoDictionary") { frame.ret_x0 = info_dictionary(); return true; }
    if (sel == "objectForInfoDictionaryKey:") { frame.ret_x0 = send(info_dictionary(), "objectForKey:", frame.x[2]); return true; }
    if (sel == "pathForResource:ofType:") {
        // A file in the bundle root; a compiled storyboard (name.storyboardc) answers for its "nib".
        std::string name, type;
        objc_string_value(frame.x[2], name);
        objc_string_value(frame.x[3], type);
        std::string base = std::string(game_dir()) + "/" + name;
        std::string p = type.empty() ? base : base + "." + type;
        if (access(p.c_str(), F_OK) != 0 && type == "nib") p = base + ".storyboardc";
        frame.ret_x0 = access(p.c_str(), F_OK) == 0 ? fake_make(2, p) : 0;
        return true;
    }
    if (sel == "loadNibNamed:owner:options:") {
        // No nib decoding: the launch screen's top-level object is a screen-sized, empty view.
        uint64_t v = fake_make(8, "UIView");
        FakeObjcObj* vs = fake_lookup(v);
        vs->rect[2] = kScreenW;
        vs->rect[3] = screen::g_height;
        frame.ret_x0 = new_array({v});
        return true;
    }
    if (sel == "URLForResource:withExtension:") { frame.ret_x0 = 0; return true; }
    return false;
}

// CGAffineTransform (6 doubles) is returned through x8 and passed by pointer.
inline bool transform_message(CallFrame& frame, FakeObjcObj* st, const std::string& sel) {
    static const double identity[6] = {1, 0, 0, 1, 0, 0};
    if (sel == "transform" || sel == "targetTransform") {
        double t[6];
        std::memcpy(t, identity, sizeof t);
        for (int i = 0; i < 6; ++i) t[i] = getd(st, "transform" + std::to_string(i), identity[i]);
        if (g_objc_in_x8) std::memcpy(reinterpret_cast<void*>(g_objc_in_x8), t, sizeof t);
        frame.ret_x0 = g_objc_in_x8;
        return true;
    }
    if (sel == "setTransform:") {
        const double* t = reinterpret_cast<const double*>(frame.x[2]);
        if (t) for (int i = 0; i < 6; ++i) setd(st, "transform" + std::to_string(i), t[i]);
        return true;
    }
    return false;
}

}  // namespace fakeui

#include "user_defaults.hpp"

static bool fake_uikit_message(CallFrame& frame, const std::string& sel) {
    using namespace fakeui;
    const uint64_t self = frame.x[0];
    double in[4];
    std::memcpy(in, g_objc_in_d, sizeof in);   // nested sends below overwrite the thread-local argument copy

    if (sel == "performSelector:withObject:afterDelay:") {
        std::lock_guard<std::mutex> l(g_loop_mu);
        g_delayed.push_back({self, reinterpret_cast<const char*>(frame.x[2]), frame.x[3], now_s() + in[0]});
        frame.ret_x0 = 0;
        return true;
    }
    if (sel == "presentDrawable:" || sel == "presentDrawable:afterMinimumDuration:" || sel == "presentDrawable:atTime:") {
        if (FakeObjcObj* cb = fake_lookup(self)) cb->props["#present"] = frame.x[2];   // captured at commit (mtl_gles.hpp)
        uint64_t n = ++g_presents;
        static const bool stats = getenv("LOADER_DRAW_STATS") != nullptr;
        if (stats && n % 60 == 0) {
            printf("[GFX] presents %llu: %.1f draws/frame over last 60\n", static_cast<unsigned long long>(n),
                   g_draws_this_frame.exchange(0) / 60.0);
            fflush(stdout);
        }
        frame.ret_x0 = 0;
        return true;
    }

    FakeObjcObj* f = fake_lookup(self);
    if (f && f->kind == 7) return class_message(frame, f->s, sel);
    if (f && f->kind == 6 && f->s == "UIScreen") {
        if (sel == "preferredMode" || sel == "currentMode") {
            static uint64_t mode = fake_make(8, "UIScreenMode");
            frame.ret_x0 = mode;
            return true;
        }
        if (sel == "availableModes") { frame.ret_x0 = new_array({send(self, "currentMode")}); return true; }
        if (sel == "coordinateSpace" || sel == "fixedCoordinateSpace") { frame.ret_x0 = self; return true; }
        if (sel == "setCurrentMode:" || sel == "setOverscanCompensation:" || sel == "setBrightness:") return true;
        if (sel == "mirroredScreen" || sel == "overscanCompensation") { frame.ret_x0 = 0; return true; }
        if (sel == "bounds") { objc_ret_rect(0, 0, kScreenW, screen::g_height); frame.ret_x0 = 0; return true; }
        return false;
    }
    if (f && f->kind == 6 && f->s == "UIApplication") {
        if (sel == "delegate") { frame.ret_x0 = g_app_delegate; return true; }
        if (sel == "keyWindow") { frame.ret_x0 = g_key_window; return true; }
        if (sel == "windows") { frame.ret_x0 = new_array(g_key_window ? std::vector<uint64_t>{g_key_window} : std::vector<uint64_t>{}); return true; }
        if (sel == "isIgnoringInteractionEvents" || sel == "openURL:" || sel == "canOpenURL:") { frame.ret_x0 = 0; return true; }
        return false;
    }
    if (f && f->kind == 1) return bundle_message(frame, sel);
    if (f && f->kind == 2) return string_message(frame, f, sel);

    FakeObjcObj* st = ui_state(self);
    if (!st) return false;
    const std::string& cls = st->s;
    if (sel == "class") { frame.ret_x0 = fake_class_named(cls); return true; }
    if (sel == "isKindOfClass:" || sel == "isMemberOfClass:") { frame.ret_x0 = fakeui::is_kind_of(self, frame.x[2]); return true; }
    if (sel == "isEqual:") { frame.ret_x0 = keys_equal(self, frame.x[2]); return true; }
    if (sel == "hash") { frame.ret_x0 = self; return true; }
    if (sel == "retain" || sel == "autorelease" || sel == "self") { frame.ret_x0 = self; return true; }
    if (sel == "release" || sel == "dealloc") { frame.ret_x0 = 0; return true; }
    if (sel == "performSelector:") { frame.ret_x0 = send_sel(self, reinterpret_cast<const char*>(frame.x[2]), 0); return true; }
    if (sel == "performSelector:withObject:") { frame.ret_x0 = send_sel(self, reinterpret_cast<const char*>(frame.x[2]), frame.x[3]); return true; }

    if (is_container(cls) && container_message(frame, st, sel)) return true;
    if (is_view_class(cls) && (view_message(frame, self, st, sel, in) || transform_message(frame, st, sel))) return true;
    if (cls == "UIViewController" && controller_message(frame, self, st, sel)) return true;
    if ((cls == "CAMetalLayer" || cls == "CALayer") && layer_message(frame, self, st, sel, in)) return true;
    if (cls == "CADisplayLink" && display_link_message(frame, self, st, sel)) return true;
    if (misc_message(frame, st, sel, in)) return true;

    if (sel.compare(0, 4, "init") == 0 || sel == "copy" || sel == "mutableCopy") { frame.ret_x0 = self; return true; }
    if (sel == "description") { frame.ret_x0 = fake_make(2, cls); return true; }
    if (sel == "respondsToSelector:") { frame.ret_x0 = 0; return true; }
    // property bag: setFoo: stores x2 (and d0), foo / isFoo reads it back
    const size_t colons = colon_count(sel);
    if (sel.compare(0, 3, "set") == 0 && sel.size() > 4 && colons == 1) {
        std::string key = lower_first(sel.substr(3, sel.size() - 4));
        st->props[key] = frame.x[2];
        fake_retain(frame.x[2]);
        setd(st, key, in[0]);
        frame.ret_x0 = 0;
        return true;
    }
    if (colons == 0) {
        std::string key = sel;
        if (!st->props.count(key) && sel.compare(0, 2, "is") == 0 && sel.size() > 2) key = lower_first(sel.substr(2));
        frame.ret_x0 = getp(st, key);
        objc_ret_double(getd(st, key, 0));
        return true;
    }
    frame.ret_x0 = 0;
    return true;
}

// dispatch_async target: the block runs on the next run loop pass (main thread).
extern "C" void native_dispatch_run_on_main(void* block) {
    if (!block) return;
    std::lock_guard<std::mutex> l(fakeui::g_loop_mu);
    fakeui::g_delayed.push_back({0, nullptr, reinterpret_cast<uint64_t>(block), 0});
}

// One pass of the main run loop: due delayed performs, the first appearance of the key window's root view controller,
// pending layout, then every display link (CADisplayLink fires on vsync; the caller paces ticks at 60 Hz).
static void fakeui_run_loop_once() {
    using namespace fakeui;
    const double now = now_s();
    std::vector<DelayedPerform> due;
    std::vector<uint64_t> layout, links;
    bool appear = false;
    uint64_t key = 0;
    {
        std::lock_guard<std::mutex> l(g_loop_mu);
        for (size_t i = 0; i < g_delayed.size();) {
            if (g_delayed[i].due <= now) { due.push_back(g_delayed[i]); g_delayed.erase(g_delayed.begin() + i); }
            else ++i;
        }
        appear = g_appear_pending;
        g_appear_pending = false;
        key = g_key_window;
        layout.swap(g_needs_layout);
        links = g_display_links;
    }
    for (const DelayedPerform& d : due) {
        if (!d.sel) {   // queued block (dispatch_async, completion handlers)
            static const bool log = getenv("LOADER_LOG_BLOCKS") != nullptr;
            if (log) {
                uint64_t inv = d.arg ? *reinterpret_cast<uint64_t*>(d.arg + 16) : 0;
                printf("[UI] block invoke image+%#llx\n", static_cast<unsigned long long>(inv - reinterpret_cast<uint64_t>(g_loaded_image_base)));
                fflush(stdout);
            }
            run_block(d.arg, 0);
            continue;
        }
        printf("[UI] delayed perform -[%#llx %s]\n", static_cast<unsigned long long>(d.target), d.sel);
        fflush(stdout);
        send_sel(d.target, d.sel, d.arg);
    }
    uint64_t root = 0;
    if (appear && key) {
        if (FakeObjcObj* ks = ui_state(key)) root = getp(ks, "rootViewController");
        if (root && guest_implements(root, "viewWillAppear:")) send(root, "viewWillAppear:", 0);
    }
    for (uint64_t v : layout) {
        FakeObjcObj* vs = ui_state(v);
        uint64_t vc = vs ? getp(vs, "#viewController") : 0;
        if (vc && guest_implements(vc, "viewWillLayoutSubviews")) send(vc, "viewWillLayoutSubviews");
        if (guest_implements(v, "layoutSubviews")) send(v, "layoutSubviews");
        if (vc && guest_implements(vc, "viewDidLayoutSubviews")) send(vc, "viewDidLayoutSubviews");
    }
    if (root && guest_implements(root, "viewDidAppear:")) send(root, "viewDidAppear:", 0);
    for (uint64_t link : links) {
        FakeObjcObj* ls = fake_lookup(link);
        if (!ls || getp(ls, "paused")) continue;
        setd(ls, "timestamp", now);
        setd(ls, "targetTimestamp", now + 1 / 60.0);
        send_sel(getp(ls, "target"), reinterpret_cast<const char*>(getp(ls, "action")), link);
    }
}

// LOADER_TOUCH="<frame>:<x>,<y>[,<frames held>];...": taps in screen pixels, delivered to the key window's root view
// the way UIKit does (touchesBegan / touchesEnded with a UIEvent whose allTouches holds the UITouch).
struct ScriptedTap { int frame, hold; double x, y; uint64_t touch = 0; };
static std::vector<ScriptedTap> parse_touch_script() {
    std::vector<ScriptedTap> out;
    const char* s = getenv("LOADER_TOUCH");
    while (s && *s) {
        const char* semi = strchr(s, ';');
        std::string item = semi ? std::string(s, semi) : std::string(s);
        ScriptedTap t{0, 4, 0, 0};
        if (sscanf(item.c_str(), "%d:%lf,%lf,%d", &t.frame, &t.x, &t.y, &t.hold) >= 3) out.push_back(t);
        s = semi ? semi + 1 : nullptr;
    }
    return out;
}
// Delivers one touch phase to the key window's root view, as UIKit does: touchesBegan/Moved/Ended/Cancelled with a
// UIEvent whose allTouches holds the UITouch. x, y are in points. phase: UITouchPhase (0 began, 1 moved, 3 ended,
// 4 cancelled).
static void send_touch(uint64_t touch, int phase, double x, double y) {
    using namespace fakeui;
    FakeObjcObj* ks = g_key_window ? ui_state(g_key_window) : nullptr;
    uint64_t root = ks ? getp(ks, "rootViewController") : 0;
    uint64_t view = root ? send(root, "view") : 0;
    FakeObjcObj* ts = fake_lookup(touch);
    if (!view || !ts) return;
    if (phase == 0) { setd(ts, "px", x); setd(ts, "py", y); }
    else { setd(ts, "px", getd(ts, "x", x)); setd(ts, "py", getd(ts, "y", y)); }
    setd(ts, "x", x);
    setd(ts, "y", y);
    setd(ts, "timestamp", now_s());
    ts->props["phase"] = static_cast<uint64_t>(phase);
    ts->props["tapCount"] = 1;
    ts->props["view"] = view;
    ts->props["window"] = g_key_window;
    uint64_t set = fake_make(8, "NSSet");
    fake_lookup(set)->items = {touch};
    fake_retain(touch);
    uint64_t ev = fake_make(8, "UIEvent");
    FakeObjcObj* es = fake_lookup(ev);
    es->props["touches"] = set;
    setd(es, "timestamp", now_s());
    static const char* const sels[] = {"touchesBegan:withEvent:", "touchesMoved:withEvent:", "touchesMoved:withEvent:",
                                       "touchesEnded:withEvent:", "touchesCancelled:withEvent:"};
    send(view, sels[phase >= 0 && phase <= 4 ? phase : 4], set, ev);
}

static void deliver_scripted_touches(std::vector<ScriptedTap>& taps, int f) {
    for (ScriptedTap& t : taps) {
        bool begin = f == t.frame, end = f == t.frame + t.hold;
        if (!begin && !end) continue;
        if (begin) t.touch = fake_make(8, "UITouch");
        printf("[TOUCH] %s (%.0f, %.0f) px at frame %d\n", begin ? "began" : "ended", t.x, t.y, f);
        fflush(stdout);
        send_touch(t.touch, begin ? 0 : 3, t.x / fakeui::kScreenScale, t.y / fakeui::kScreenScale);
    }
}

// Live input from the app host (any thread): pointer id, phase as above, position normalized to the window (0..1).
struct HostTouch { int id, phase; double x, y; };
static std::mutex g_host_touch_mu;
static std::vector<HostTouch> g_host_touches;
extern "C" void host_touch(int id, int phase, float nx, float ny) {
    std::lock_guard<std::mutex> l(g_host_touch_mu);
    // consecutive moves of one pointer within a tick collapse into the latest position
    if (phase == 1 && !g_host_touches.empty() && g_host_touches.back().id == id && g_host_touches.back().phase == 1) {
        g_host_touches.back().x = nx;
        g_host_touches.back().y = ny;
        return;
    }
    g_host_touches.push_back({id, phase, nx, ny});
}
static void deliver_host_touches() {
    std::vector<HostTouch> batch;
    {
        std::lock_guard<std::mutex> l(g_host_touch_mu);
        batch.swap(g_host_touches);
    }
    static std::unordered_map<int, uint64_t> live;   // pointer id -> UITouch
    for (const HostTouch& t : batch) {
        uint64_t& touch = live[t.id];
        if (t.phase == 0 || !touch) touch = fake_make(8, "UITouch");
        send_touch(touch, t.phase, t.x * fakeui::kScreenW, t.y * screen::g_height);
        if (t.phase >= 3) live.erase(t.id);
    }
}

// LOADER_ENGINE=8: what UIApplicationMain does for the app delegate class, then the main run loop for `frames` ticks.
static void fakeui_run_application(int frames) {
    using namespace fakeui;
    objcrt::GClass* acls = objcrt::find_class_by_name("UnityAppController");
    if (!acls) { printf("[APP] UnityAppController class not found\n"); return; }
    uint64_t app = send(fake_class_named("UIApplication"), "sharedApplication");
    uint64_t ctrl = send(send(acls->addr, "alloc"), "init");
    g_app_delegate = ctrl;
    printf("[APP] -[UnityAppController init] -> %#llx\n", static_cast<unsigned long long>(ctrl)); fflush(stdout);
    printf("[APP] application:willFinishLaunchingWithOptions:\n"); fflush(stdout);
    send(ctrl, "application:willFinishLaunchingWithOptions:", app, 0);
    printf("[APP] application:didFinishLaunchingWithOptions:\n"); fflush(stdout);
    send(ctrl, "application:didFinishLaunchingWithOptions:", app, 0);
    printf("[APP] applicationDidBecomeActive:\n"); fflush(stdout);
    send(ctrl, "applicationDidBecomeActive:", app);
    printf("[APP] run loop: %d ticks\n", frames); fflush(stdout);
    double worst = 0, t0 = now_s();
    std::vector<ScriptedTap> taps = parse_touch_script();
    for (int f = 0; f < frames; ++f) {
        double a = now_s();
        deliver_scripted_touches(taps, f);
        deliver_host_touches();
        uint64_t pool = fake_pool_push();
        fakeui_run_loop_once();
        fake_pool_pop(pool);
        fakeui::defaults_flush(false);
        double ms = (now_s() - a) * 1e3;
        if (ms > worst) worst = ms;
        if (f < 3 || (f + 1) % 60 == 0)
            printf("[APP] tick %d %.2f ms, drawables %llu, presents %llu\n", f + 1, ms,
                   static_cast<unsigned long long>(g_drawables.load()), static_cast<unsigned long long>(g_presents.load()));
        fflush(stdout);
        if (ms < 16.6) usleep(static_cast<useconds_t>((16.6 - ms) * 1000));
    }
    printf("[APP] %d ticks in %.1f s, worst tick %.2f ms, drawables %llu, presents %llu\n", frames, now_s() - t0, worst,
           static_cast<unsigned long long>(g_drawables.load()), static_cast<unsigned long long>(g_presents.load()));
    fflush(stdout);
}
