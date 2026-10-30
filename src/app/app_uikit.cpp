// App mode UIKit / QuartzCore: UIApplication and UIApplicationMain, UIScreen, UIDevice, the view hierarchy (UIView,
// UIWindow, UIViewController), touches, CALayer/CAEAGLLayer, CADisplayLink, and inert stand-ins for the controls an
// OpenGL game rarely shows (alerts are answered with their cancel button so the game is never left waiting).
#include "app.hpp"

#include <android/native_window.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>

extern "C" ANativeWindow* host_window() __attribute__((weak));

namespace app {

id block_copy(id b);
void block_release(id b);
void* block_invoke_ptr(id b);
void declare_protocol(Class c, const char* proto);
void post_note(const char* name, id object, id info);
void defaults_flush();
id nib_instantiate_storyboard_initial(const std::string& storyboard);   // app_nib.cpp
bool nib_load_view(id vc);                                              // app_nib.cpp
id nib_load_main(const std::string& name, id owner);                    // app_nib.cpp

// ---------------------------------------------------------------------------------------------------------------
// screen geometry
// ---------------------------------------------------------------------------------------------------------------
struct Screen {
    double w_pt = 568, h_pt = 320;    // UIScreen bounds (interface orientation)
    double scale = 2;
    int px_w = 1920, px_h = 1080;      // the Android surface
    double content_x = 0, content_y = 0, content_w = 1920, content_h = 1080;   // where the guest screen lands in the surface
    bool landscape = true;
    bool pad = false;
    bool ready = false;
};
static Screen g_screen;

static bool info_landscape() {
    id orients = dict_get(info_dictionary(), "UISupportedInterfaceOrientations");
    const auto& v = arr_items(orients);
    if (v.empty()) {
        id o = dict_get(info_dictionary(), "UIInterfaceOrientation");
        return o && utf8(o).find("Landscape") != std::string::npos;
    }
    bool land = false, port = false;
    for (id o : v) (utf8(o).find("Landscape") != std::string::npos ? land : port) = true;
    return land && !port ? true : (port ? false : land);
}

const Screen& screen() {
    if (g_screen.ready) return g_screen;
    Screen s;
    s.landscape = info_landscape();
    if (const char* f = getenv("LOADER_APP_ORIENTATION")) s.landscape = std::strcmp(f, "portrait") != 0;
    ANativeWindow* win = host_window ? host_window() : nullptr;
    if (win) { s.px_w = ANativeWindow_getWidth(win); s.px_h = ANativeWindow_getHeight(win); }
    else if (!s.landscape) std::swap(s.px_w, s.px_h);
    if (s.landscape != (s.px_w >= s.px_h)) std::swap(s.px_w, s.px_h);   // the surface is rotated to match below
    s.pad = getenv("LOADER_APP_IDIOM") && std::strcmp(getenv("LOADER_APP_IDIOM"), "pad") == 0;
    double long_px = std::max(s.px_w, s.px_h), short_px = std::min(s.px_w, s.px_h);
    double short_pt, long_pt;
    if (s.pad) { short_pt = 768; long_pt = 1024; s.scale = 2; }
    else {
        short_pt = 320;
        long_pt = std::round(320.0 * long_px / short_px);
        long_pt = std::min(568.0, std::max(480.0, long_pt));
        s.scale = 2;
    }
    if (const char* sw = getenv("LOADER_APP_SCREEN")) {   // "568x320"
        double a = 0, b = 0;
        if (sscanf(sw, "%lfx%lf", &a, &b) == 2 && a > 0 && b > 0) { long_pt = std::max(a, b); short_pt = std::min(a, b); }
    }
    s.w_pt = s.landscape ? long_pt : short_pt;
    s.h_pt = s.landscape ? short_pt : long_pt;
    // fit the guest screen into the surface, keeping its aspect ratio
    double k = std::min(s.px_w / s.w_pt, s.px_h / s.h_pt);
    s.content_w = std::round(s.w_pt * k);
    s.content_h = std::round(s.h_pt * k);
    s.content_x = std::floor((s.px_w - s.content_w) / 2);
    s.content_y = std::floor((s.px_h - s.content_h) / 2);
    s.ready = true;
    g_screen = s;
    log("screen %gx%g pt @%gx, surface %dx%d px, content %gx%g at (%g,%g)", s.w_pt, s.h_pt, s.scale, s.px_w, s.px_h, s.content_w, s.content_h,
        s.content_x, s.content_y);
    return g_screen;
}
void screen_content_rect(int& x, int& y, int& w, int& h, int& sw, int& sh) {
    const Screen& s = screen();
    x = static_cast<int>(s.content_x); y = static_cast<int>(s.content_y);
    w = static_cast<int>(s.content_w); h = static_cast<int>(s.content_h);
    sw = s.px_w; sh = s.px_h;
}

// ---------------------------------------------------------------------------------------------------------------
// view storage
// ---------------------------------------------------------------------------------------------------------------
static Class C_UIResponder, C_UIView, C_UIWindow, C_UIViewController, C_UIApplication, C_CALayer, C_CAEAGLLayer, C_UITouch, C_UIEvent,
    C_UIColor, C_UIFont, C_UIScreen, C_UIImage, C_UIGestureRecognizer, C_UITapGR, C_UIControl;

struct LayerExt : Ext {
    CGRect bounds{{0, 0}, {0, 0}};
    CGPoint position{0, 0}, anchor{0.5, 0.5};
    double opacity = 1, scale = 1, z = 0, corner = 0;
    bool hidden = false, opaque = false, masks = false;
    id delegate = nullptr;            // weak (the view)
    id drawable_props = nullptr, contents = nullptr, background = nullptr;
    std::vector<id> sublayers;
    id superlayer = nullptr;          // weak
    ~LayerExt() override { release(drawable_props); release(contents); release(background); for (id l : sublayers) release(l); }
};

struct ViewExt : Ext {
    CGRect bounds{{0, 0}, {0, 0}};
    CGPoint center{0, 0};
    CGAffineTransform transform{1, 0, 0, 1, 0, 0};
    double alpha = 1, content_scale = 1;
    bool hidden = false, interaction = true, multitouch = false, opaque = true, clips = false, exclusive = false;
    long tag = 0, content_mode = 0;
    NSUInteger autoresize = 0;
    id layer = nullptr, background = nullptr;
    id superview = nullptr;           // weak
    id controller = nullptr;          // weak: the view controller whose root view this is
    std::vector<id> subviews;
    std::vector<id> recognizers;
    std::unordered_map<std::string, id> props;   // controls: text, font, image, title, ...
    ~ViewExt() override {
        for (id v : subviews) { if (ViewExt* e = ext_as<ViewExt>(v)) e->superview = nullptr; release(v); }
        for (id g : recognizers) release(g);
        release(layer);
        release(background);
        for (auto& kv : props) release(kv.second);
    }
};
struct WindowExt : ViewExt { id root = nullptr; bool key = false; double level = 0; ~WindowExt() override { release(root); } };
struct VCExt : Ext {
    id view = nullptr, presented = nullptr, title = nullptr, nib_name = nullptr, storyboard = nullptr;
    id presenting = nullptr, parent = nullptr;   // weak
    std::vector<id> children;
    std::unordered_map<std::string, id> props;
    bool loading = false;
    ~VCExt() override {
        if (view) if (ViewExt* e = ext_as<ViewExt>(view)) e->controller = nullptr;
        release(view); release(presented); release(title); release(nib_name); release(storyboard);
        for (id c : children) release(c);
        for (auto& kv : props) release(kv.second);
    }
};

static ViewExt* vx(id v) {
    if (!v || !has_header(v)) return nullptr;
    Ext*& e = ext_slot(v);
    if (ViewExt* x = dynamic_cast<ViewExt*>(e)) return x;
    if (e) return nullptr;
    ViewExt* x = is_kind(v, C_UIWindow) ? new WindowExt() : new ViewExt();
    e = x;
    return x;
}
static WindowExt* wx(id v) { return dynamic_cast<WindowExt*>(vx(v)); }
static VCExt* cx(id vc) { return ext_get<VCExt>(vc); }
static LayerExt* lx(id l) { return ext_get<LayerExt>(l); }

static CGRect view_frame(ViewExt* e) {
    return {{e->center.x - e->bounds.size.width / 2, e->center.y - e->bounds.size.height / 2}, e->bounds.size};
}
static void view_set_frame(id v, CGRect r) {
    ViewExt* e = vx(v);
    e->bounds.size = r.size;
    e->center = {r.origin.x + r.size.width / 2, r.origin.y + r.size.height / 2};
    if (e->layer) { LayerExt* l = lx(e->layer); l->bounds = e->bounds; l->position = e->center; }
}

static id g_app, g_key_window;
static std::vector<id> g_windows;

// ---------------------------------------------------------------------------------------------------------------
// touches
// ---------------------------------------------------------------------------------------------------------------
struct TouchExt : Ext {
    CGPoint loc{0, 0}, prev{0, 0};   // window coordinates
    long phase = 0;
    long taps = 1;
    double timestamp = 0, began = 0;
    id view = nullptr, window = nullptr;
    ~TouchExt() override { release(view); release(window); }
};
struct EventExt : Ext { std::vector<id> touches; double timestamp = 0; ~EventExt() override { for (id t : touches) release(t); } };

static CGPoint convert_to_view(id view, CGPoint p_window);
static CGPoint convert_from_view(id view, CGPoint p);

struct HostTouch { int id; int phase; float x, y; };
static std::mutex g_touch_mu;
static std::vector<HostTouch> g_host_touches;

extern "C" void app_host_touch(int pid, int phase, float nx, float ny) {
    std::lock_guard<std::mutex> l(g_touch_mu);
    if (phase == 1 && !g_host_touches.empty() && g_host_touches.back().id == pid && g_host_touches.back().phase == 1) {
        g_host_touches.back().x = nx;
        g_host_touches.back().y = ny;
        return;
    }
    g_host_touches.push_back({pid, phase, nx, ny});
}

static id hit_test(id view, CGPoint p_window);

static void deliver(const std::vector<id>& touches, const char* sel_name, id event) {
    // group by view, one message per view (UIKit sends each view only the touches that belong to it)
    std::map<id, std::vector<id>> by_view;
    for (id t : touches) by_view[ext_get<TouchExt>(t)->view].push_back(t);
    for (auto& [view, ts] : by_view) {
        if (!view) continue;
        id set = send_cls(class_named("NSSet"), "setWithArray:", arr(ts));
        if (trace_enabled("touch")) {
            CGPoint p = ext_get<TouchExt>(ts[0])->loc;
            log("%s -> %s (%zu) at %.1f,%.1f", sel_name, class_name(class_of(view)), ts.size(), p.x, p.y);
        }
        send<void>(view, sel_name, set, event);
    }
}

static void recognizers_on_end(id touch);

static void deliver_host_touches() {
    std::vector<HostTouch> batch;
    {
        std::lock_guard<std::mutex> l(g_touch_mu);
        batch.swap(g_host_touches);
    }
    if (batch.empty() || !g_key_window) return;
    static std::map<int, id> live;
    static id event = nullptr;
    if (!event) event = alloc(C_UIEvent);
    const Screen& s = screen();
    double t = now_s();
    for (const HostTouch& h : batch) {
        // surface -> guest points (the guest screen may be letterboxed inside the surface)
        double px = h.x * s.px_w, py = h.y * s.px_h;
        CGPoint p{(px - s.content_x) * s.w_pt / s.content_w, (py - s.content_y) * s.h_pt / s.content_h};
        id& touch = live[h.id];
        if (h.phase == 0 || !touch) {
            if (touch) release(touch);
            touch = alloc(C_UITouch);
            auto* te = ext_get<TouchExt>(touch);
            te->loc = te->prev = p;
            te->began = t;
            te->window = retain(g_key_window);
            id v = hit_test(g_key_window, p);
            te->view = retain(v ? v : g_key_window);
            if (h.phase != 0) { te->phase = 0; }
        }
        auto* te = ext_get<TouchExt>(touch);
        te->prev = te->loc;
        te->loc = p;
        te->timestamp = t;
        te->phase = h.phase == 0 ? 0 : h.phase == 1 ? 1 : h.phase == 3 ? 3 : 4;
        auto* ee = ext_get<EventExt>(event);
        for (id x : ee->touches) release(x);
        ee->touches.clear();
        for (auto& kv : live) if (kv.second) ee->touches.push_back(retain(kv.second));
        ee->timestamp = t;
        const char* sel_name = te->phase == 0 ? "touchesBegan:withEvent:" : te->phase == 1 ? "touchesMoved:withEvent:" : te->phase == 3 ? "touchesEnded:withEvent:" : "touchesCancelled:withEvent:";
        void* pool = pool_push();
        deliver({touch}, sel_name, event);
        if (te->phase == 3) recognizers_on_end(touch);
        pool_pop(pool);
        if (te->phase >= 3) {
            release(touch);
            live.erase(h.id);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// geometry helpers
// ---------------------------------------------------------------------------------------------------------------
static CGPoint origin_in_window(id view) {
    double x = 0, y = 0;
    for (id v = view; v; v = vx(v)->superview) {
        ViewExt* e = vx(v);
        if (!e) break;
        CGRect f = view_frame(e);
        if (is_kind(v, C_UIWindow) && !e->superview) break;
        x += f.origin.x - e->bounds.origin.x;
        y += f.origin.y - e->bounds.origin.y;
    }
    return {x, y};
}
static CGPoint convert_to_view(id view, CGPoint p) { CGPoint o = origin_in_window(view); return {p.x - o.x, p.y - o.y}; }
static CGPoint convert_from_view(id view, CGPoint p) { CGPoint o = origin_in_window(view); return {p.x + o.x, p.y + o.y}; }

static id hit_test(id view, CGPoint pw) {
    ViewExt* e = vx(view);
    if (!e || e->hidden || !e->interaction || e->alpha < 0.01) return nullptr;
    CGPoint local = convert_to_view(view, pw);
    bool inside = local.x >= e->bounds.origin.x && local.y >= e->bounds.origin.y && local.x < e->bounds.origin.x + e->bounds.size.width &&
                  local.y < e->bounds.origin.y + e->bounds.size.height;
    if (!inside && !is_kind(view, C_UIWindow)) return nullptr;
    for (auto it = e->subviews.rbegin(); it != e->subviews.rend(); ++it)
        if (id h = hit_test(*it, pw)) return h;
    return view;
}

static void view_will_appear(id vc, bool appear) {
    if (!vc) return;
    send<void>(vc, appear ? "viewWillAppear:" : "viewWillDisappear:", 0);
    send<void>(vc, appear ? "viewDidAppear:" : "viewDidDisappear:", 0);
}

static void add_subview(id parent, id child, long index = -1) {
    if (!child || child == parent) return;
    ViewExt* pe = vx(parent);
    ViewExt* ce = vx(child);
    if (!pe || !ce) return;
    retain(child);
    if (ce->superview) {
        ViewExt* old = vx(ce->superview);
        auto it = std::find(old->subviews.begin(), old->subviews.end(), child);
        if (it != old->subviews.end()) { old->subviews.erase(it); release(child); }
    }
    if (responds(child, sel("willMoveToSuperview:"))) send<void>(child, "willMoveToSuperview:", parent);
    if (index < 0 || static_cast<size_t>(index) > pe->subviews.size()) pe->subviews.push_back(child);
    else pe->subviews.insert(pe->subviews.begin() + index, child);
    ce->superview = parent;
    if (ce->layer && pe->layer) { LayerExt* pl = lx(pe->layer); LayerExt* cl = lx(ce->layer); if (!cl->superlayer) { pl->sublayers.push_back(retain(ce->layer)); cl->superlayer = pe->layer; } }
    send<void>(child, "didMoveToSuperview");
    send<void>(child, "didMoveToWindow");
    send<void>(parent, "didAddSubview:", child);
}
static void remove_from_superview(id child) {
    ViewExt* ce = vx(child);
    if (!ce || !ce->superview) return;
    id parent = ce->superview;
    ViewExt* pe = vx(parent);
    send<void>(parent, "willRemoveSubview:", child);
    if (ce->layer && pe->layer) {
        LayerExt* pl = lx(pe->layer);
        auto it = std::find(pl->sublayers.begin(), pl->sublayers.end(), ce->layer);
        if (it != pl->sublayers.end()) { pl->sublayers.erase(it); lx(ce->layer)->superlayer = nullptr; release(ce->layer); }
    }
    ce->superview = nullptr;
    auto it = std::find(pe->subviews.begin(), pe->subviews.end(), child);
    if (it != pe->subviews.end()) pe->subviews.erase(it);
    send<void>(child, "didMoveToSuperview");
    send<void>(child, "didMoveToWindow");
    release(child);
}
static id view_window(id v) {
    for (; v; v = vx(v) ? vx(v)->superview : nullptr)
        if (is_kind(v, C_UIWindow)) return v;
    return nullptr;
}

static id view_layer(id v) {
    ViewExt* e = vx(v);
    if (!e) return nullptr;
    if (!e->layer) {
        Class lc = send<Class>(cls_obj(class_of(v)), "layerClass");
        if (!lc) lc = C_CALayer;
        e->layer = send(send_cls(lc, "alloc"), "init");
        LayerExt* l = lx(e->layer);
        l->delegate = v;
        l->bounds = e->bounds;
        l->position = e->center;
        l->scale = e->content_scale;
    }
    return e->layer;
}

// ---------------------------------------------------------------------------------------------------------------
// UIColor / UIFont
// ---------------------------------------------------------------------------------------------------------------
struct ColorExt : Ext { double r = 0, g = 0, b = 0, a = 1; id pattern = nullptr; ~ColorExt() override { release(pattern); } };
static id color(double r, double g, double b, double a) {
    id c = alloc(C_UIColor);
    auto* e = ext_get<ColorExt>(c);
    e->r = r; e->g = g; e->b = b; e->a = a;
    return autorelease(c);
}
bool color_components(id c, double rgba[4]) {
    ColorExt* e = ext_as<ColorExt>(c);
    if (!e) return false;
    rgba[0] = e->r; rgba[1] = e->g; rgba[2] = e->b; rgba[3] = e->a;
    return true;
}
id make_color(double r, double g, double b, double a) { return color(r, g, b, a); }
struct FontExt : Ext { std::string name = "Helvetica"; double size = 12; bool bold = false; };
static id font(const std::string& name, double size, bool bold) {
    id f = alloc(C_UIFont);
    auto* e = ext_get<FontExt>(f);
    e->name = name; e->size = size; e->bold = bold;
    return autorelease(f);
}
double font_size(id f) { FontExt* e = ext_as<FontExt>(f); return e ? e->size : 12; }

CGSize text_size(id s, id f, double max_w) {
    double fs = font_size(f);
    double line_h = std::ceil(fs * 1.2);
    double w = std::ceil(u16(s).size() * fs * 0.55);
    if (max_w > 0 && w > max_w) {
        double lines = std::ceil(w / max_w);
        return {max_w, lines * line_h};
    }
    return {w, line_h};
}

// ---------------------------------------------------------------------------------------------------------------
// CADisplayLink
// ---------------------------------------------------------------------------------------------------------------
static Class C_CADisplayLink;
struct LinkExt : Ext {
    id target = nullptr;
    SEL sel = nullptr;
    long interval = 1;
    long preferred_fps = 0;
    bool paused = false, scheduled = false, valid = true;
    uint64_t tid = 0;
    double timestamp = 0, last = 0;
    ~LinkExt() override { release(target); }
};
static double link_period(LinkExt* e) {
    if (e->preferred_fps > 0) return 1.0 / static_cast<double>(e->preferred_fps);
    return std::max<long>(1, e->interval) / 60.0;
}
static void link_fire(id link) {
    auto* e = ext_get<LinkExt>(link);
    if (!e->valid || e->paused) return;
    double t = now_s();
    e->last = e->timestamp ? e->timestamp : t;
    e->timestamp = t;
    send<void>(e->target, e->sel, link);
}
static void link_schedule(id link) {
    auto* e = ext_get<LinkExt>(link);
    if (e->scheduled || !e->valid) return;
    e->scheduled = true;
    retain(link);
    e->tid = timer_add(link_period(e), link_period(e), [link] { link_fire(link); });
}
static void link_unschedule(id link) {
    auto* e = ext_get<LinkExt>(link);
    if (!e->scheduled) return;
    e->scheduled = false;
    timer_remove(e->tid);
    release(link);
}

// ---------------------------------------------------------------------------------------------------------------
// alerts: answered automatically with the cancel button
// ---------------------------------------------------------------------------------------------------------------
struct AlertExt : ViewExt { id title = nullptr, message = nullptr, delegate = nullptr; std::vector<id> buttons; long cancel = -1; ~AlertExt() override { release(title); release(message); for (id b : buttons) release(b); } };
static AlertExt* ax(id a) {
    Ext*& e = ext_slot(a);
    if (auto* x = dynamic_cast<AlertExt*>(e)) return x;
    delete e;
    auto* x = new AlertExt();
    e = x;
    return x;
}
extern "C" id app_impl_alert_init(id self, SEL, id title, id message, id delegate, id cancel, id other, const uint64_t* va) {
    auto* e = ax(self);
    e->title = retain(title);
    e->message = retain(message);
    e->delegate = delegate;
    if (cancel) { e->cancel = static_cast<long>(e->buttons.size()); e->buttons.push_back(retain(cancel)); }
    if (other) {
        e->buttons.push_back(retain(other));
        for (size_t i = 0; va && va[i]; ++i) e->buttons.push_back(retain(reinterpret_cast<id>(va[i])));
    }
    return self;
}
APP_VARIADIC(app_th_alert_init, app_impl_alert_init, 7)
extern "C" id app_impl_sheet_init(id self, SEL, id title, id delegate, id cancel, id destructive, id other, const uint64_t* va) {
    auto* e = ax(self);
    e->title = retain(title);
    e->delegate = delegate;
    if (destructive) e->buttons.push_back(retain(destructive));
    if (other) {
        e->buttons.push_back(retain(other));
        for (size_t i = 0; va && va[i]; ++i) e->buttons.push_back(retain(reinterpret_cast<id>(va[i])));
    }
    if (cancel) { e->cancel = static_cast<long>(e->buttons.size()); e->buttons.push_back(retain(cancel)); }
    return self;
}
APP_VARIADIC(app_th_sheet_init, app_impl_sheet_init, 7)

static void alert_show(id self, bool sheet) {
    auto* e = ax(self);
    std::string buttons;
    for (id b : e->buttons) buttons += "[" + utf8(b) + "] ";
    log("%s: \"%s\" %s %s", sheet ? "UIActionSheet" : "UIAlertView", utf8(e->title).c_str(), utf8(e->message).c_str(), buttons.c_str());
    if (getenv("LOADER_APP_ALERTS") && std::strcmp(getenv("LOADER_APP_ALERTS"), "keep") == 0) return;
    long idx = e->cancel >= 0 ? e->cancel : (e->buttons.empty() ? -1 : 0);
    retain(self);
    post_main([self, idx, sheet] {
        auto* x = ax(self);
        id d = x->delegate;
        const char* clicked = sheet ? "actionSheet:clickedButtonAtIndex:" : "alertView:clickedButtonAtIndex:";
        const char* will = sheet ? "actionSheet:willDismissWithButtonIndex:" : "alertView:willDismissWithButtonIndex:";
        const char* did = sheet ? "actionSheet:didDismissWithButtonIndex:" : "alertView:didDismissWithButtonIndex:";
        if (responds(d, sel(clicked))) send<void>(d, clicked, self, idx);
        if (responds(d, sel(will))) send<void>(d, will, self, idx);
        if (responds(d, sel(did))) send<void>(d, did, self, idx);
        release(self);
    }, 0.25);
}

// ---------------------------------------------------------------------------------------------------------------
// gesture recognizers (taps)
// ---------------------------------------------------------------------------------------------------------------
struct GRExt : Ext {
    struct T { id target; SEL action; };
    std::vector<T> targets;
    id view = nullptr;   // weak
    id delegate = nullptr;
    long taps = 1, touches = 1, state = 0;
    bool enabled = true, cancels = true;
};
static void recognizers_on_end(id touch) {
    auto* te = ext_get<TouchExt>(touch);
    double dx = te->loc.x - te->prev.x, dy = te->loc.y - te->prev.y;
    (void)dx; (void)dy;
    if (now_s() - te->began > 0.6) return;
    for (id v = te->view; v; v = vx(v) ? vx(v)->superview : nullptr) {
        ViewExt* e = vx(v);
        if (!e) break;
        for (id g : std::vector<id>(e->recognizers)) {
            auto* ge = ext_get<GRExt>(g);
            if (!ge->enabled || !is_kind(g, C_UITapGR)) continue;
            if (responds(ge->delegate, sel("gestureRecognizerShouldBegin:")) && !send<bool>(ge->delegate, "gestureRecognizerShouldBegin:", g)) continue;
            if (responds(ge->delegate, sel("gestureRecognizer:shouldReceiveTouch:")) && !send<bool>(ge->delegate, "gestureRecognizer:shouldReceiveTouch:", g, touch)) continue;
            ge->state = 3;
            for (auto& t : ge->targets) send<void>(t.target, t.action, g);
            ge->state = 0;
            return;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// UIApplicationMain
// ---------------------------------------------------------------------------------------------------------------
static void window_make_key(id w) {
    if (g_key_window == w) return;
    if (g_key_window) if (WindowExt* old = wx(g_key_window)) old->key = false;
    g_key_window = w;
    if (WindowExt* e = wx(w)) e->key = true;
    if (std::find(g_windows.begin(), g_windows.end(), w) == g_windows.end()) g_windows.push_back(retain(w));
}

static void set_root(id w, id vc) {
    WindowExt* e = wx(w);
    if (!e || e->root == vc) return;
    id old = e->root;
    if (old) {
        id ov = cx(old)->view;
        if (ov) remove_from_superview(ov);
    }
    e->root = retain(vc);
    if (vc) {
        id v = send(vc, "view");
        if (v) {
            const Screen& s = screen();
            view_set_frame(v, {{0, 0}, {s.w_pt, s.h_pt}});
            add_subview(w, v);
        }
        if (e->key) view_will_appear(vc, true);
    }
    release(old);
}

static void frame_hook() { deliver_host_touches(); }

extern "C" int app_UIApplicationMain(int argc, char** argv, id principal, id delegate_name) {
    (void)argc; (void)argv;
    mark_main_thread();
    set_frame_hook(frame_hook);
    screen();
    Class pc = principal ? class_named(utf8(principal).c_str()) : 0;
    if (!pc) { id p = dict_get(info_dictionary(), "NSPrincipalClass"); if (p) pc = class_named(utf8(p).c_str()); }
    if (!pc) pc = C_UIApplication;
    g_app = send(send_cls(pc, "alloc"), "init");
    log("UIApplicationMain: application %s, delegate %s", class_name(pc), delegate_name ? utf8(delegate_name).c_str() : "(from nib)");
    if (delegate_name) {
        Class dc = class_named(utf8(delegate_name).c_str());
        if (!dc) log("delegate class %s not found", utf8(delegate_name).c_str());
        else {
            id d = send(send_cls(dc, "alloc"), "init");
            send<void>(g_app, "setDelegate:", d);
        }
    }
    id delegate = send(g_app, "delegate");
    id window = nullptr;
    std::string sb = utf8(dict_get(info_dictionary(), "UIMainStoryboardFile"));
    std::string nib = utf8(dict_get(info_dictionary(), "NSMainNibFile"));
    const char* use_sb = getenv("LOADER_APP_STORYBOARD");
    if (!sb.empty() && !(use_sb && std::strcmp(use_sb, "0") == 0)) {
        id vc = nib_instantiate_storyboard_initial(sb);
        const Screen& s = screen();
        window = send(send_cls(C_UIWindow, "alloc"), "initWithFrame:", CGRect{{0, 0}, {s.w_pt, s.h_pt}});
        if (vc) send<void>(window, "setRootViewController:", vc);
        if (responds(delegate, sel("setWindow:"))) send<void>(delegate, "setWindow:", window);
        else send<void>(delegate, "setValue:forKey:", window, str("window"));
        release(window);
    } else if (!sb.empty()) {
        const Screen& s = screen();
        window = send(send_cls(C_UIWindow, "alloc"), "initWithFrame:", CGRect{{0, 0}, {s.w_pt, s.h_pt}});
        if (responds(delegate, sel("setWindow:"))) send<void>(delegate, "setWindow:", window);
        release(window);
    } else if (!nib.empty()) {
        nib_load_main(nib, g_app);
        delegate = send(g_app, "delegate");
    }
    if (!delegate) log("no application delegate");
    void* pool = pool_push();
    if (responds(delegate, sel("application:willFinishLaunchingWithOptions:")))
        send<bool>(delegate, "application:willFinishLaunchingWithOptions:", g_app, static_cast<id>(nullptr));
    if (responds(delegate, sel("application:didFinishLaunchingWithOptions:")))
        send<bool>(delegate, "application:didFinishLaunchingWithOptions:", g_app, static_cast<id>(nullptr));
    else if (responds(delegate, sel("applicationDidFinishLaunching:")))
        send<void>(delegate, "applicationDidFinishLaunching:", g_app);
    post_note("UIApplicationDidFinishLaunchingNotification", g_app, nullptr);
    id w = responds(delegate, sel("window")) ? send(delegate, "window") : nullptr;
    if (w && !g_key_window) send<void>(w, "makeKeyAndVisible");
    if (responds(delegate, sel("applicationDidBecomeActive:"))) send<void>(delegate, "applicationDidBecomeActive:", g_app);
    post_note("UIApplicationDidBecomeActiveNotification", g_app, nullptr);
    pool_pop(pool);
    log("entering main run loop");
    run_loop_forever();
}

// lifecycle events from the Android host
extern "C" void app_host_lifecycle(int active) {
    post_main([active] {
        id d = send(g_app, "delegate");
        if (!active) {
            if (responds(d, sel("applicationWillResignActive:"))) send<void>(d, "applicationWillResignActive:", g_app);
            post_note("UIApplicationWillResignActiveNotification", g_app, nullptr);
            if (responds(d, sel("applicationDidEnterBackground:"))) send<void>(d, "applicationDidEnterBackground:", g_app);
            post_note("UIApplicationDidEnterBackgroundNotification", g_app, nullptr);
            defaults_flush();
        } else {
            if (responds(d, sel("applicationWillEnterForeground:"))) send<void>(d, "applicationWillEnterForeground:", g_app);
            post_note("UIApplicationWillEnterForegroundNotification", g_app, nullptr);
            if (responds(d, sel("applicationDidBecomeActive:"))) send<void>(d, "applicationDidBecomeActive:", g_app);
            post_note("UIApplicationDidBecomeActiveNotification", g_app, nullptr);
        }
    });
}

// ---------------------------------------------------------------------------------------------------------------
// class definitions
// ---------------------------------------------------------------------------------------------------------------
static CGRect parse_rect_string(const std::string& s) {
    CGRect r{{0, 0}, {0, 0}};
    sscanf(s.c_str(), "{{%lf, %lf}, {%lf, %lf}}", &r.origin.x, &r.origin.y, &r.size.width, &r.size.height);
    return r;
}
static CGPoint parse_point_string(const std::string& s) {
    CGPoint p{0, 0};
    sscanf(s.c_str(), "{%lf, %lf}", &p.x, &p.y);
    return p;
}
CGRect coder_rect(id coder, const char* key) {
    id v = send(coder, "decodeObjectForKey:", str(key));
    if (is_string(v)) return parse_rect_string(utf8(v));
    return send<CGRect>(coder, "decodeCGRectForKey:", str(key));
}

static void define_view_class() {
    C_UIResponder = def_class("UIResponder", "NSObject");
    Class R = C_UIResponder;
    for (const char* s : {"touchesBegan:withEvent:", "touchesMoved:withEvent:", "touchesEnded:withEvent:", "touchesCancelled:withEvent:"}) {
        def(R, s, +[](id self, SEL cmd, id touches, id event) {
            id next = send(self, "nextResponder");
            if (next && next != self) send<void>(next, cmd, touches, event);
        });
    }
    def(R, "nextResponder", +[](id, SEL) -> id { return nullptr; });
    def(R, "becomeFirstResponder", +[](id, SEL) -> bool { return true; });
    def(R, "resignFirstResponder", +[](id, SEL) -> bool { return true; });
    def(R, "canBecomeFirstResponder", +[](id, SEL) -> bool { return false; });
    def(R, "canResignFirstResponder", +[](id, SEL) -> bool { return true; });
    def(R, "isFirstResponder", +[](id, SEL) -> bool { return false; });
    def(R, "motionBegan:withEvent:", +[](id, SEL, long, id) {});
    def(R, "motionEnded:withEvent:", +[](id, SEL, long, id) {});
    def(R, "motionCancelled:withEvent:", +[](id, SEL, long, id) {});
    def(R, "remoteControlReceivedWithEvent:", +[](id, SEL, id) {});
    def(R, "inputView", +[](id, SEL) -> id { return nullptr; });
    def(R, "undoManager", +[](id, SEL) -> id { return nullptr; });

    C_UIView = def_class("UIView", "UIResponder");
    Class V = C_UIView;
    defc(V, "layerClass", +[](id, SEL) -> Class { return C_CALayer; });
    defc(V, "areAnimationsEnabled", +[](id, SEL) -> bool { return true; });
    defc(V, "setAnimationsEnabled:", +[](id, SEL, bool) {});
    defc(V, "beginAnimations:context:", +[](id, SEL, id, void*) {});
    defc(V, "commitAnimations", +[](id, SEL) {});
    for (const char* s : {"setAnimationDuration:", "setAnimationDelay:", "setAnimationRepeatCount:"}) defc(V, s, +[](id, SEL, double) {});
    for (const char* s : {"setAnimationCurve:", "setAnimationTransition:forView:cache:", "setAnimationBeginsFromCurrentState:", "setAnimationRepeatAutoreverses:"})
        defc(V, s, +[](id, SEL, long) {});
    defc(V, "setAnimationDelegate:", +[](id, SEL, id) {});
    defc(V, "setAnimationDidStopSelector:", +[](id, SEL, SEL) {});
    defc(V, "setAnimationWillStartSelector:", +[](id, SEL, SEL) {});
    defc(V, "animateWithDuration:animations:", +[](id, SEL, double, id anim) { if (anim) reinterpret_cast<void (*)(id)>(block_invoke_ptr(anim))(anim); });
    auto anim_completion = +[](id, SEL, double d, id anim, id completion) {
        if (anim) reinterpret_cast<void (*)(id)>(block_invoke_ptr(anim))(anim);
        if (completion) {
            id c = block_copy(completion);
            post_main([c] { reinterpret_cast<void (*)(id, bool)>(block_invoke_ptr(c))(c, true); block_release(c); }, std::min(d, 1.0));
        }
    };
    defc(V, "animateWithDuration:animations:completion:", anim_completion);
    defc(V, "animateWithDuration:delay:options:animations:completion:", +[](id self, SEL, double d, double, NSUInteger, id anim, id completion) {
        reinterpret_cast<void (*)(id, SEL, double, id, id)>(lookup(self, sel("animateWithDuration:animations:completion:")))(self, nullptr, d, anim, completion);
    });
    defc(V, "animateWithDuration:delay:usingSpringWithDamping:initialSpringVelocity:options:animations:completion:",
         +[](id self, SEL, double d, double, double, double, NSUInteger, id anim, id completion) {
             reinterpret_cast<void (*)(id, SEL, double, id, id)>(lookup(self, sel("animateWithDuration:animations:completion:")))(self, nullptr, d, anim, completion);
         });
    defc(V, "transitionWithView:duration:options:animations:completion:", +[](id self, SEL, id, double d, NSUInteger, id anim, id completion) {
        reinterpret_cast<void (*)(id, SEL, double, id, id)>(lookup(self, sel("animateWithDuration:animations:completion:")))(self, nullptr, d, anim, completion);
    });
    defc(V, "performWithoutAnimation:", +[](id, SEL, id b) { if (b) reinterpret_cast<void (*)(id)>(block_invoke_ptr(b))(b); });

    def(V, "init", +[](id self, SEL) -> id { return send(self, "initWithFrame:", CGRect{{0, 0}, {0, 0}}); });
    def(V, "initWithFrame:", +[](id self, SEL, CGRect r) -> id { vx(self); view_set_frame(self, r); return self; });
    def(V, "initWithCoder:", +[](id self, SEL, id coder) -> id {
        ViewExt* e = vx(self);
        CGRect b = coder_rect(coder, "UIBounds");
        id c = send(coder, "decodeObjectForKey:", str("UICenter"));
        e->bounds = b;
        e->center = is_string(c) ? parse_point_string(utf8(c)) : CGPoint{b.size.width / 2, b.size.height / 2};
        e->multitouch = send<bool>(coder, "decodeBoolForKey:", str("UIMultipleTouchEnabled"));
        e->hidden = send<bool>(coder, "decodeBoolForKey:", str("UIHidden"));
        e->interaction = !send<bool>(coder, "decodeBoolForKey:", str("UIUserInteractionDisabled"));
        e->tag = send<long>(coder, "decodeIntegerForKey:", str("UITag"));
        if (send<bool>(coder, "containsValueForKey:", str("UIAlpha"))) e->alpha = send<double>(coder, "decodeDoubleForKey:", str("UIAlpha"));
        e->clips = send<bool>(coder, "decodeBoolForKey:", str("UIClipsToBounds"));
        e->autoresize = static_cast<NSUInteger>(send<long>(coder, "decodeIntegerForKey:", str("UIAutoresizingMask")));
        id bg = send(coder, "decodeObjectForKey:", str("UIBackgroundColor"));
        if (bg) e->background = retain(bg);
        id subs = send(coder, "decodeObjectForKey:", str("UISubviews"));
        for (id sv : arr_items(subs)) add_subview(self, sv);
        return self;
    });
    def(V, "frame", +[](id self, SEL) -> CGRect { ViewExt* e = vx(self); return e ? view_frame(e) : CGRect{}; });
    def(V, "setFrame:", +[](id self, SEL, CGRect r) {
        ViewExt* e = vx(self);
        CGSize old = e->bounds.size;
        view_set_frame(self, r);
        if (old.width != r.size.width || old.height != r.size.height) send<void>(self, "layoutSubviews");
    });
    def(V, "bounds", +[](id self, SEL) -> CGRect { ViewExt* e = vx(self); return e ? e->bounds : CGRect{}; });
    def(V, "setBounds:", +[](id self, SEL, CGRect r) { ViewExt* e = vx(self); e->bounds = r; if (e->layer) lx(e->layer)->bounds = r; });
    def(V, "center", +[](id self, SEL) -> CGPoint { return vx(self)->center; });
    def(V, "setCenter:", +[](id self, SEL, CGPoint p) { ViewExt* e = vx(self); e->center = p; if (e->layer) lx(e->layer)->position = p; });
    def(V, "transform", +[](id self, SEL) -> CGAffineTransform { return vx(self)->transform; });
    def(V, "setTransform:", +[](id self, SEL, CGAffineTransform t) { vx(self)->transform = t; });
    def(V, "alpha", +[](id self, SEL) -> double { return vx(self)->alpha; });
    def(V, "setAlpha:", +[](id self, SEL, double a) { vx(self)->alpha = a; });
    def(V, "isHidden", +[](id self, SEL) -> bool { return vx(self)->hidden; });
    def(V, "hidden", +[](id self, SEL) -> bool { return vx(self)->hidden; });
    def(V, "setHidden:", +[](id self, SEL, bool h) { vx(self)->hidden = h; });
    def(V, "isUserInteractionEnabled", +[](id self, SEL) -> bool { return vx(self)->interaction; });
    def(V, "userInteractionEnabled", +[](id self, SEL) -> bool { return vx(self)->interaction; });
    def(V, "setUserInteractionEnabled:", +[](id self, SEL, bool b) { vx(self)->interaction = b; });
    def(V, "isMultipleTouchEnabled", +[](id self, SEL) -> bool { return vx(self)->multitouch; });
    def(V, "setMultipleTouchEnabled:", +[](id self, SEL, bool b) { vx(self)->multitouch = b; });
    def(V, "isExclusiveTouch", +[](id self, SEL) -> bool { return vx(self)->exclusive; });
    def(V, "setExclusiveTouch:", +[](id self, SEL, bool b) { vx(self)->exclusive = b; });
    def(V, "isOpaque", +[](id self, SEL) -> bool { return vx(self)->opaque; });
    def(V, "setOpaque:", +[](id self, SEL, bool b) { vx(self)->opaque = b; });
    def(V, "clipsToBounds", +[](id self, SEL) -> bool { return vx(self)->clips; });
    def(V, "setClipsToBounds:", +[](id self, SEL, bool b) { vx(self)->clips = b; });
    def(V, "tag", +[](id self, SEL) -> long { return vx(self)->tag; });
    def(V, "setTag:", +[](id self, SEL, long t) { vx(self)->tag = t; });
    def(V, "contentMode", +[](id self, SEL) -> long { return vx(self)->content_mode; });
    def(V, "setContentMode:", +[](id self, SEL, long m) { vx(self)->content_mode = m; });
    def(V, "autoresizingMask", +[](id self, SEL) -> NSUInteger { return vx(self)->autoresize; });
    def(V, "setAutoresizingMask:", +[](id self, SEL, NSUInteger m) { vx(self)->autoresize = m; });
    def(V, "setAutoresizesSubviews:", +[](id, SEL, bool) {});
    def(V, "autoresizesSubviews", +[](id, SEL) -> bool { return true; });
    def(V, "contentScaleFactor", +[](id self, SEL) -> double { return vx(self)->content_scale; });
    def(V, "setContentScaleFactor:", +[](id self, SEL, double s) { ViewExt* e = vx(self); e->content_scale = s; if (e->layer) lx(e->layer)->scale = s; });
    def(V, "backgroundColor", +[](id self, SEL) -> id { return vx(self)->background; });
    def(V, "setBackgroundColor:", +[](id self, SEL, id c) { ViewExt* e = vx(self); id o = e->background; e->background = retain(c); release(o); });
    def(V, "setClearsContextBeforeDrawing:", +[](id, SEL, bool) {});
    def(V, "setTintColor:", +[](id, SEL, id) {});
    def(V, "tintColor", +[](id, SEL) -> id { return color(0, 0.48, 1, 1); });
    def(V, "layer", +[](id self, SEL) -> id { return view_layer(self); });
    def(V, "superview", +[](id self, SEL) -> id { return vx(self)->superview; });
    def(V, "subviews", +[](id self, SEL) -> id { return arr(vx(self)->subviews); });
    def(V, "window", +[](id self, SEL) -> id { return view_window(self); });
    def(V, "addSubview:", +[](id self, SEL, id v) { add_subview(self, v); });
    def(V, "insertSubview:atIndex:", +[](id self, SEL, id v, long i) { add_subview(self, v, i); });
    def(V, "insertSubview:aboveSubview:", +[](id self, SEL, id v, id other) {
        auto& s = vx(self)->subviews; auto it = std::find(s.begin(), s.end(), other);
        add_subview(self, v, it == s.end() ? -1 : static_cast<long>(it - s.begin()) + 1);
    });
    def(V, "insertSubview:belowSubview:", +[](id self, SEL, id v, id other) {
        auto& s = vx(self)->subviews; auto it = std::find(s.begin(), s.end(), other);
        add_subview(self, v, it == s.end() ? 0 : static_cast<long>(it - s.begin()));
    });
    def(V, "removeFromSuperview", +[](id self, SEL) { remove_from_superview(self); });
    def(V, "bringSubviewToFront:", +[](id self, SEL, id v) {
        auto& s = vx(self)->subviews; auto it = std::find(s.begin(), s.end(), v);
        if (it != s.end()) { s.erase(it); s.push_back(v); }
    });
    def(V, "sendSubviewToBack:", +[](id self, SEL, id v) {
        auto& s = vx(self)->subviews; auto it = std::find(s.begin(), s.end(), v);
        if (it != s.end()) { s.erase(it); s.insert(s.begin(), v); }
    });
    def(V, "exchangeSubviewAtIndex:withSubviewAtIndex:", +[](id self, SEL, long a, long b) {
        auto& s = vx(self)->subviews;
        if (a >= 0 && b >= 0 && static_cast<size_t>(a) < s.size() && static_cast<size_t>(b) < s.size()) std::swap(s[a], s[b]);
    });
    def(V, "isDescendantOfView:", +[](id self, SEL, id v) -> bool { for (id x = self; x; x = vx(x)->superview) if (x == v) return true; return false; });
    def(V, "viewWithTag:", +[](id self, SEL, long tag) -> id {
        std::vector<id> q = {self};
        while (!q.empty()) { id v = q.front(); q.erase(q.begin()); if (vx(v)->tag == tag) return v; for (id c : vx(v)->subviews) q.push_back(c); }
        return nullptr;
    });
    def(V, "nextResponder", +[](id self, SEL) -> id { ViewExt* e = vx(self); return e->controller ? e->controller : e->superview; });
    def(V, "hitTest:withEvent:", +[](id self, SEL, CGPoint p, id) -> id { return hit_test(self, convert_from_view(self, p)); });
    def(V, "pointInside:withEvent:", +[](id self, SEL, CGPoint p, id) -> bool {
        CGRect b = vx(self)->bounds;
        return p.x >= b.origin.x && p.y >= b.origin.y && p.x < b.origin.x + b.size.width && p.y < b.origin.y + b.size.height;
    });
    def(V, "convertPoint:toView:", +[](id self, SEL, CGPoint p, id v) -> CGPoint { CGPoint w = convert_from_view(self, p); return v ? convert_to_view(v, w) : w; });
    def(V, "convertPoint:fromView:", +[](id self, SEL, CGPoint p, id v) -> CGPoint { CGPoint w = v ? convert_from_view(v, p) : p; return convert_to_view(self, w); });
    def(V, "convertRect:toView:", +[](id self, SEL, CGRect r, id v) -> CGRect {
        CGPoint w = convert_from_view(self, r.origin); CGPoint o = v ? convert_to_view(v, w) : w; return {o, r.size};
    });
    def(V, "convertRect:fromView:", +[](id self, SEL, CGRect r, id v) -> CGRect {
        CGPoint w = v ? convert_from_view(v, r.origin) : r.origin; return {convert_to_view(self, w), r.size};
    });
    for (const char* s : {"setNeedsDisplay", "setNeedsLayout", "layoutIfNeeded", "layoutSubviews", "didMoveToSuperview", "didMoveToWindow", "sizeToFit",
                          "setNeedsUpdateConstraints", "updateConstraintsIfNeeded", "removeConstraints:", "setTranslatesAutoresizingMaskIntoConstraints:"})
        def(V, s, +[](id, SEL) {});
    def(V, "setNeedsDisplayInRect:", +[](id, SEL, CGRect) {});
    def(V, "drawRect:", +[](id, SEL, CGRect) {});
    def(V, "willMoveToSuperview:", +[](id, SEL, id) {});
    def(V, "willMoveToWindow:", +[](id, SEL, id) {});
    def(V, "didAddSubview:", +[](id, SEL, id) {});
    def(V, "willRemoveSubview:", +[](id, SEL, id) {});
    def(V, "sizeThatFits:", +[](id self, SEL, CGSize) -> CGSize { return vx(self)->bounds.size; });
    def(V, "intrinsicContentSize", +[](id, SEL) -> CGSize { return {-1, -1}; });
    def(V, "endEditing:", +[](id, SEL, bool) -> bool { return true; });
    def(V, "addGestureRecognizer:", +[](id self, SEL, id g) { vx(self)->recognizers.push_back(retain(g)); ext_get<GRExt>(g)->view = self; });
    def(V, "removeGestureRecognizer:", +[](id self, SEL, id g) {
        auto& r = vx(self)->recognizers; auto it = std::find(r.begin(), r.end(), g);
        if (it != r.end()) { r.erase(it); release(g); }
    });
    def(V, "gestureRecognizers", +[](id self, SEL) -> id { return arr(vx(self)->recognizers); });
    def(V, "safeAreaInsets", +[](id, SEL) -> CGRect { return {{0, 0}, {0, 0}}; });
    def(V, "layoutMargins", +[](id, SEL) -> CGRect { return {{8, 8}, {8, 8}}; });
    def(V, "addConstraint:", +[](id, SEL, id) {});
    def(V, "addConstraints:", +[](id, SEL, id) {});
    def(V, "setAccessibilityLabel:", +[](id, SEL, id) {});
    def(V, "setIsAccessibilityElement:", +[](id, SEL, bool) {});
    def(V, "snapshotViewAfterScreenUpdates:", +[](id, SEL, bool) -> id { return nullptr; });
    def(V, "drawViewHierarchyInRect:afterScreenUpdates:", +[](id, SEL, CGRect, bool) -> bool { return false; });
    // properties of controls built on UIView (labels, buttons, image views, text fields ...)
    auto prop_get = +[](id self, SEL cmd) -> id { auto& p = vx(self)->props; auto it = p.find(cmd); return it == p.end() ? nullptr : it->second; };
    auto prop_set = +[](id self, SEL cmd, id v) {
        std::string s = cmd;   // setFoo: -> foo
        std::string key = std::string(1, static_cast<char>(tolower(s[3]))) + s.substr(4, s.size() - 5);
        auto& p = vx(self)->props;
        id c = is_string(v) ? send(v, "copy") : retain(v);
        id old = p.count(key) ? p[key] : nullptr;
        p[key] = c;
        release(old);
    };
    (void)prop_get;
    (void)prop_set;
}

static id prop(id view, const char* key) { auto& p = vx(view)->props; auto it = p.find(key); return it == p.end() ? nullptr : it->second; }
static void set_prop(id view, const char* key, id v) {
    auto& p = vx(view)->props;
    id c = is_string(v) ? send(v, "copy") : retain(v);
    id old = p.count(key) ? p[key] : nullptr;
    p[key] = c;
    release(old);
}

#define PROP_OBJ(CLS, GET, SET)                                                                     \
    def(CLS, GET, +[](id self, SEL) -> id { return prop(self, GET); });                             \
    def(CLS, SET, +[](id self, SEL, id v) { set_prop(self, GET, v); });
#define PROP_NOP_LONG(CLS, SET) def(CLS, SET, +[](id, SEL, long) {});

static void define_controls() {
    Class CTL = def_class("UIControl", "UIView");
    C_UIControl = CTL;
    struct Action { id target; SEL action; NSUInteger mask; };
    def(CTL, "addTarget:action:forControlEvents:", +[](id self, SEL, id target, SEL action, NSUInteger mask) {
        (void)target; (void)action; (void)mask;
        set_prop(self, "#hasTargets", num_b(true));
    });
    def(CTL, "removeTarget:action:forControlEvents:", +[](id, SEL, id, SEL, NSUInteger) {});
    def(CTL, "allTargets", +[](id, SEL) -> id { return send_cls(class_named("NSSet"), "set"); });
    def(CTL, "isEnabled", +[](id, SEL) -> bool { return true; });
    def(CTL, "setEnabled:", +[](id, SEL, bool) {});
    def(CTL, "isSelected", +[](id, SEL) -> bool { return false; });
    def(CTL, "setSelected:", +[](id, SEL, bool) {});
    def(CTL, "isHighlighted", +[](id, SEL) -> bool { return false; });
    def(CTL, "setHighlighted:", +[](id, SEL, bool) {});
    def(CTL, "sendActionsForControlEvents:", +[](id, SEL, NSUInteger) {});
    PROP_NOP_LONG(CTL, "setContentHorizontalAlignment:")
    PROP_NOP_LONG(CTL, "setContentVerticalAlignment:")

    Class L = def_class("UILabel", "UIView");
    PROP_OBJ(L, "text", "setText:")
    PROP_OBJ(L, "font", "setFont:")
    PROP_OBJ(L, "textColor", "setTextColor:")
    PROP_OBJ(L, "shadowColor", "setShadowColor:")
    PROP_OBJ(L, "highlightedTextColor", "setHighlightedTextColor:")
    PROP_OBJ(L, "attributedText", "setAttributedText:")
    PROP_NOP_LONG(L, "setTextAlignment:")
    PROP_NOP_LONG(L, "setLineBreakMode:")
    PROP_NOP_LONG(L, "setNumberOfLines:")
    PROP_NOP_LONG(L, "setAdjustsFontSizeToFitWidth:")
    PROP_NOP_LONG(L, "setBaselineAdjustment:")
    def(L, "setMinimumFontSize:", +[](id, SEL, double) {});
    def(L, "setMinimumScaleFactor:", +[](id, SEL, double) {});
    def(L, "setShadowOffset:", +[](id, SEL, CGSize) {});
    def(L, "shadowOffset", +[](id, SEL) -> CGSize { return {0, -1}; });
    def(L, "textAlignment", +[](id, SEL) -> long { return 0; });
    def(L, "lineBreakMode", +[](id, SEL) -> long { return 4; });
    def(L, "numberOfLines", +[](id, SEL) -> long { return 1; });
    def(L, "setHighlighted:", +[](id, SEL, bool) {});
    def(L, "isHighlighted", +[](id, SEL) -> bool { return false; });
    def(L, "setEnabled:", +[](id, SEL, bool) {});
    def(L, "sizeThatFits:", +[](id self, SEL, CGSize max) -> CGSize { return text_size(prop(self, "text"), prop(self, "font"), max.width); });
    def(L, "drawTextInRect:", +[](id, SEL, CGRect) {});
    def(L, "textRectForBounds:limitedToNumberOfLines:", +[](id self, SEL, CGRect b, long) -> CGRect { return {b.origin, text_size(prop(self, "text"), prop(self, "font"), b.size.width)}; });

    Class IV = def_class("UIImageView", "UIView");
    def(IV, "initWithImage:", +[](id self, SEL, id img) -> id {
        CGSize s = img ? send<CGSize>(img, "size") : CGSize{0, 0};
        send(self, "initWithFrame:", CGRect{{0, 0}, s});
        set_prop(self, "image", img);
        return self;
    });
    PROP_OBJ(IV, "image", "setImage:")
    PROP_OBJ(IV, "highlightedImage", "setHighlightedImage:")
    PROP_OBJ(IV, "animationImages", "setAnimationImages:")
    def(IV, "setAnimationDuration:", +[](id, SEL, double) {});
    PROP_NOP_LONG(IV, "setAnimationRepeatCount:")
    def(IV, "startAnimating", +[](id, SEL) {});
    def(IV, "stopAnimating", +[](id, SEL) {});
    def(IV, "isAnimating", +[](id, SEL) -> bool { return false; });

    Class B = def_class("UIButton", "UIControl");
    defc(B, "buttonWithType:", +[](id self, SEL, long) -> id { return autorelease(send(send(self, "alloc"), "initWithFrame:", CGRect{{0, 0}, {0, 0}})); });
    def(B, "setTitle:forState:", +[](id self, SEL, id t, NSUInteger) { set_prop(self, "title", t); });
    def(B, "titleForState:", +[](id self, SEL, NSUInteger) -> id { return prop(self, "title"); });
    def(B, "currentTitle", +[](id self, SEL) -> id { return prop(self, "title"); });
    def(B, "setImage:forState:", +[](id self, SEL, id i, NSUInteger) { set_prop(self, "image", i); });
    def(B, "imageForState:", +[](id self, SEL, NSUInteger) -> id { return prop(self, "image"); });
    def(B, "setBackgroundImage:forState:", +[](id self, SEL, id i, NSUInteger) { set_prop(self, "backgroundImage", i); });
    def(B, "backgroundImageForState:", +[](id self, SEL, NSUInteger) -> id { return prop(self, "backgroundImage"); });
    def(B, "setTitleColor:forState:", +[](id, SEL, id, NSUInteger) {});
    def(B, "setTitleShadowColor:forState:", +[](id, SEL, id, NSUInteger) {});
    def(B, "setAttributedTitle:forState:", +[](id, SEL, id, NSUInteger) {});
    def(B, "titleLabel", +[](id self, SEL) -> id {
        id l = prop(self, "#titleLabel");
        if (!l) { l = autorelease(send(send_cls(class_named("UILabel"), "alloc"), "initWithFrame:", CGRect{{0, 0}, {0, 0}})); set_prop(self, "#titleLabel", l); }
        return l;
    });
    def(B, "imageView", +[](id self, SEL) -> id {
        id l = prop(self, "#imageView");
        if (!l) { l = autorelease(send(send_cls(class_named("UIImageView"), "alloc"), "initWithFrame:", CGRect{{0, 0}, {0, 0}})); set_prop(self, "#imageView", l); }
        return l;
    });
    def(B, "setShowsTouchWhenHighlighted:", +[](id, SEL, bool) {});
    def(B, "setAdjustsImageWhenHighlighted:", +[](id, SEL, bool) {});
    def(B, "setContentEdgeInsets:", +[](id, SEL, CGRect) {});
    def(B, "setTitleEdgeInsets:", +[](id, SEL, CGRect) {});
    def(B, "setImageEdgeInsets:", +[](id, SEL, CGRect) {});

    Class TF = def_class("UITextField", "UIControl");
    PROP_OBJ(TF, "text", "setText:")
    PROP_OBJ(TF, "placeholder", "setPlaceholder:")
    PROP_OBJ(TF, "font", "setFont:")
    PROP_OBJ(TF, "textColor", "setTextColor:")
    PROP_OBJ(TF, "delegate", "setDelegate:")
    PROP_OBJ(TF, "inputAccessoryView", "setInputAccessoryView:")
    for (const char* s : {"setBorderStyle:", "setKeyboardType:", "setReturnKeyType:", "setAutocorrectionType:", "setAutocapitalizationType:",
                          "setClearButtonMode:", "setTextAlignment:", "setEnablesReturnKeyAutomatically:", "setClearsOnBeginEditing:", "setSecureTextEntry:",
                          "setKeyboardAppearance:", "setSpellCheckingType:", "setAdjustsFontSizeToFitWidth:"})
        def(TF, s, +[](id, SEL, long) {});
    def(TF, "becomeFirstResponder", +[](id self, SEL) -> bool { log("UITextField becomeFirstResponder (no on-screen keyboard)"); (void)self; return true; });
    def(TF, "resignFirstResponder", +[](id, SEL) -> bool { return true; });
    def(TF, "isFirstResponder", +[](id, SEL) -> bool { return false; });
    def(TF, "isEditing", +[](id, SEL) -> bool { return false; });

    Class TV = def_class("UITextView", "UIView");
    PROP_OBJ(TV, "text", "setText:")
    PROP_OBJ(TV, "font", "setFont:")
    PROP_OBJ(TV, "delegate", "setDelegate:")
    for (const char* s : {"setEditable:", "setScrollEnabled:", "setTextAlignment:", "setDataDetectorTypes:", "setSelectable:"}) def(TV, s, +[](id, SEL, long) {});

    Class SV = def_class("UIScrollView", "UIView");
    PROP_OBJ(SV, "delegate", "setDelegate:")
    for (const char* s : {"setScrollEnabled:", "setPagingEnabled:", "setShowsHorizontalScrollIndicator:", "setShowsVerticalScrollIndicator:", "setBounces:",
                          "setScrollsToTop:", "setDelaysContentTouches:", "setAlwaysBounceVertical:", "setDecelerationRate:", "setIndicatorStyle:"})
        def(SV, s, +[](id, SEL, long) {});
    def(SV, "setContentSize:", +[](id, SEL, CGSize) {});
    def(SV, "contentSize", +[](id, SEL) -> CGSize { return {0, 0}; });
    def(SV, "setContentOffset:", +[](id, SEL, CGPoint) {});
    def(SV, "setContentOffset:animated:", +[](id, SEL, CGPoint, bool) {});
    def(SV, "contentOffset", +[](id, SEL) -> CGPoint { return {0, 0}; });
    def(SV, "setContentInset:", +[](id, SEL, CGRect) {});
    def(SV, "scrollRectToVisible:animated:", +[](id, SEL, CGRect, bool) {});
    def(SV, "flashScrollIndicators", +[](id, SEL) {});

    Class TBV = def_class("UITableView", "UIScrollView");
    def(TBV, "initWithFrame:style:", +[](id self, SEL, CGRect r, long) -> id { return send(self, "initWithFrame:", r); });
    PROP_OBJ(TBV, "dataSource", "setDataSource:")
    for (const char* s : {"setRowHeight:", "setSeparatorStyle:", "setSeparatorColor:", "setAllowsSelection:", "setSectionHeaderHeight:", "setSectionFooterHeight:",
                          "setEstimatedRowHeight:", "setBackgroundView:", "setTableHeaderView:", "setTableFooterView:", "setEditing:"})
        def(TBV, s, +[](id, SEL, long) {});
    for (const char* s : {"reloadData", "beginUpdates", "endUpdates"}) def(TBV, s, +[](id, SEL) {});
    def(TBV, "dequeueReusableCellWithIdentifier:", +[](id, SEL, id) -> id { return nullptr; });
    def(TBV, "visibleCells", +[](id, SEL) -> id { return arr({}); });
    def(TBV, "indexPathForSelectedRow", +[](id, SEL) -> id { return nullptr; });
    def(TBV, "deselectRowAtIndexPath:animated:", +[](id, SEL, id, bool) {});
    def(TBV, "insertRowsAtIndexPaths:withRowAnimation:", +[](id, SEL, id, long) {});
    def(TBV, "deleteRowsAtIndexPaths:withRowAnimation:", +[](id, SEL, id, long) {});
    def(TBV, "reloadRowsAtIndexPaths:withRowAnimation:", +[](id, SEL, id, long) {});
    def(TBV, "cellForRowAtIndexPath:", +[](id, SEL, id) -> id { return nullptr; });
    def(TBV, "scrollToRowAtIndexPath:atScrollPosition:animated:", +[](id, SEL, id, long, bool) {});
    Class CELL = def_class("UITableViewCell", "UIView");
    def(CELL, "initWithStyle:reuseIdentifier:", +[](id self, SEL, long, id) -> id { return send(self, "initWithFrame:", CGRect{{0, 0}, {320, 44}}); });
    def(CELL, "initWithFrame:reuseIdentifier:", +[](id self, SEL, CGRect r, id) -> id { return send(self, "initWithFrame:", r); });
    def(CELL, "contentView", +[](id self, SEL) -> id {
        id v = prop(self, "#content");
        if (!v) { v = autorelease(send(send_cls(C_UIView, "alloc"), "initWithFrame:", send<CGRect>(self, "bounds"))); set_prop(self, "#content", v); add_subview(self, v); }
        return v;
    });
    def(CELL, "textLabel", +[](id self, SEL) -> id {
        id l = prop(self, "#text");
        if (!l) { l = autorelease(send(send_cls(class_named("UILabel"), "alloc"), "initWithFrame:", CGRect{{0, 0}, {0, 0}})); set_prop(self, "#text", l); }
        return l;
    });
    def(CELL, "detailTextLabel", +[](id self, SEL) -> id {
        id l = prop(self, "#detail");
        if (!l) { l = autorelease(send(send_cls(class_named("UILabel"), "alloc"), "initWithFrame:", CGRect{{0, 0}, {0, 0}})); set_prop(self, "#detail", l); }
        return l;
    });
    def(CELL, "imageView", +[](id self, SEL) -> id {
        id l = prop(self, "#image");
        if (!l) { l = autorelease(send(send_cls(class_named("UIImageView"), "alloc"), "initWithFrame:", CGRect{{0, 0}, {0, 0}})); set_prop(self, "#image", l); }
        return l;
    });
    for (const char* s : {"setSelectionStyle:", "setAccessoryType:", "setAccessoryView:", "setSelected:", "setBackgroundView:", "setSelectedBackgroundView:", "setIndentationLevel:"})
        def(CELL, s, +[](id, SEL, long) {});
    def(CELL, "prepareForReuse", +[](id, SEL) {});
    def(CELL, "reuseIdentifier", +[](id, SEL) -> id { return nullptr; });

    Class WV = def_class("UIWebView", "UIView");
    PROP_OBJ(WV, "delegate", "setDelegate:")
    def(WV, "loadRequest:", +[](id self, SEL, id req) { log("UIWebView loadRequest: %s (not displayed)", utf8(send(send(req, "URL"), "absoluteString")).c_str()); (void)self; });
    def(WV, "loadHTMLString:baseURL:", +[](id, SEL, id, id) {});
    def(WV, "loadData:MIMEType:textEncodingName:baseURL:", +[](id, SEL, id, id, id, id) {});
    def(WV, "stringByEvaluatingJavaScriptFromString:", +[](id, SEL, id) -> id { return str(""); });
    def(WV, "stopLoading", +[](id, SEL) {});
    def(WV, "reload", +[](id, SEL) {});
    def(WV, "goBack", +[](id, SEL) {});
    def(WV, "isLoading", +[](id, SEL) -> bool { return false; });
    def(WV, "canGoBack", +[](id, SEL) -> bool { return false; });
    def(WV, "scrollView", +[](id self, SEL) -> id {
        id l = prop(self, "#scroll");
        if (!l) { l = autorelease(send(send_cls(class_named("UIScrollView"), "alloc"), "initWithFrame:", CGRect{{0, 0}, {0, 0}})); set_prop(self, "#scroll", l); }
        return l;
    });
    for (const char* s : {"setScalesPageToFit:", "setDataDetectorTypes:", "setAllowsInlineMediaPlayback:", "setMediaPlaybackRequiresUserAction:", "setSuppressesIncrementalRendering:"})
        def(WV, s, +[](id, SEL, long) {});

    Class AI = def_class("UIActivityIndicatorView", "UIView");
    def(AI, "initWithActivityIndicatorStyle:", +[](id self, SEL, long) -> id { return send(self, "initWithFrame:", CGRect{{0, 0}, {20, 20}}); });
    for (const char* s : {"startAnimating", "stopAnimating"}) def(AI, s, +[](id, SEL) {});
    def(AI, "isAnimating", +[](id, SEL) -> bool { return false; });
    for (const char* s : {"setHidesWhenStopped:", "setActivityIndicatorViewStyle:", "setColor:"}) def(AI, s, +[](id, SEL, long) {});

    Class TB = def_class("UIToolbar", "UIView");
    for (const char* s : {"setItems:", "setItems:animated:", "setBarStyle:", "setTintColor:", "setTranslucent:"}) def(TB, s, +[](id, SEL, id) {});
    Class BBI = def_class("UIBarButtonItem", "NSObject");
    def(BBI, "initWithTitle:style:target:action:", +[](id self, SEL, id, long, id, SEL) -> id { return self; });
    def(BBI, "initWithBarButtonSystemItem:target:action:", +[](id self, SEL, long, id, SEL) -> id { return self; });
    def(BBI, "initWithCustomView:", +[](id self, SEL, id) -> id { return self; });
    def(BBI, "setEnabled:", +[](id, SEL, bool) {});
    def_class("UINavigationBar", "UIView");
    Class NI = def_class("UINavigationItem", "NSObject");
    for (const char* s : {"setTitle:", "setRightBarButtonItem:", "setLeftBarButtonItem:", "setTitleView:", "setHidesBackButton:", "setBackBarButtonItem:"})
        def(NI, s, +[](id, SEL, id) {});
    def_class("UIPageControl", "UIControl");
    def_class("UISwitch", "UIControl");
    def_class("UISlider", "UIControl");
    def_class("UISegmentedControl", "UIControl");
    def_class("UIProgressView", "UIView");
    def_class("UIPickerView", "UIView");
    def_class("UIDatePicker", "UIControl");
    def_class("UISearchBar", "UIView");
    def_class("UICollectionView", "UIScrollView");
    def_class("UIVisualEffectView", "UIView");

    Class AV = def_class("UIAlertView", "UIView");
    def(AV, "initWithTitle:message:delegate:cancelButtonTitle:otherButtonTitles:", &app_th_alert_init);
    def(AV, "init", +[](id self, SEL) -> id { ax(self); return send(self, "initWithFrame:", CGRect{{0, 0}, {0, 0}}); });
    def(AV, "show", +[](id self, SEL) { alert_show(self, false); });
    def(AV, "addButtonWithTitle:", +[](id self, SEL, id t) -> long { auto* e = ax(self); e->buttons.push_back(retain(t)); return static_cast<long>(e->buttons.size()) - 1; });
    def(AV, "buttonTitleAtIndex:", +[](id self, SEL, long i) -> id { auto* e = ax(self); return i >= 0 && static_cast<size_t>(i) < e->buttons.size() ? e->buttons[i] : nullptr; });
    def(AV, "numberOfButtons", +[](id self, SEL) -> long { return static_cast<long>(ax(self)->buttons.size()); });
    def(AV, "cancelButtonIndex", +[](id self, SEL) -> long { return ax(self)->cancel; });
    def(AV, "setCancelButtonIndex:", +[](id self, SEL, long i) { ax(self)->cancel = i; });
    def(AV, "firstOtherButtonIndex", +[](id self, SEL) -> long { return ax(self)->cancel == 0 ? 1 : 0; });
    def(AV, "title", +[](id self, SEL) -> id { return ax(self)->title; });
    def(AV, "setTitle:", +[](id self, SEL, id t) { auto* e = ax(self); release(e->title); e->title = retain(t); });
    def(AV, "message", +[](id self, SEL) -> id { return ax(self)->message; });
    def(AV, "setMessage:", +[](id self, SEL, id t) { auto* e = ax(self); release(e->message); e->message = retain(t); });
    def(AV, "delegate", +[](id self, SEL) -> id { return ax(self)->delegate; });
    def(AV, "setDelegate:", +[](id self, SEL, id d) { ax(self)->delegate = d; });
    def(AV, "dismissWithClickedButtonIndex:animated:", +[](id self, SEL, long i, bool) {
        id d = ax(self)->delegate;
        if (responds(d, sel("alertView:didDismissWithButtonIndex:"))) send<void>(d, "alertView:didDismissWithButtonIndex:", self, i);
    });
    def(AV, "setAlertViewStyle:", +[](id, SEL, long) {});
    def(AV, "alertViewStyle", +[](id, SEL) -> long { return 0; });
    def(AV, "textFieldAtIndex:", +[](id self, SEL, long) -> id {
        id t = prop(self, "#field");
        if (!t) { t = autorelease(send(send_cls(class_named("UITextField"), "alloc"), "initWithFrame:", CGRect{{0, 0}, {0, 0}})); set_prop(self, "#field", t); }
        return t;
    });
    def(AV, "isVisible", +[](id, SEL) -> bool { return false; });
    Class AS = def_class("UIActionSheet", "UIView");
    def(AS, "initWithTitle:delegate:cancelButtonTitle:destructiveButtonTitle:otherButtonTitles:", &app_th_sheet_init);
    def(AS, "showInView:", +[](id self, SEL, id) { alert_show(self, true); });
    def(AS, "showFromRect:inView:animated:", +[](id self, SEL, CGRect, id, bool) { alert_show(self, true); });
    def(AS, "addButtonWithTitle:", +[](id self, SEL, id t) -> long { auto* e = ax(self); e->buttons.push_back(retain(t)); return static_cast<long>(e->buttons.size()) - 1; });
    def(AS, "setCancelButtonIndex:", +[](id self, SEL, long i) { ax(self)->cancel = i; });
    def(AS, "cancelButtonIndex", +[](id self, SEL) -> long { return ax(self)->cancel; });
    def(AS, "buttonTitleAtIndex:", +[](id self, SEL, long i) -> id { auto* e = ax(self); return i >= 0 && static_cast<size_t>(i) < e->buttons.size() ? e->buttons[i] : nullptr; });
    def(AS, "setActionSheetStyle:", +[](id, SEL, long) {});
    Class AC = def_class("UIAlertController", "NSObject");
    defc(AC, "alertControllerWithTitle:message:preferredStyle:", +[](id, SEL, id t, id m, long) -> id {
        log("UIAlertController: \"%s\" %s", utf8(t).c_str(), utf8(m).c_str());
        return autorelease(alloc(class_named("UIAlertController")));
    });
    def(AC, "addAction:", +[](id, SEL, id) {});
    def(AC, "addTextFieldWithConfigurationHandler:", +[](id, SEL, id) {});
    Class AA = def_class("UIAlertAction", "NSObject");
    defc(AA, "actionWithTitle:style:handler:", +[](id, SEL, id, long, id) -> id { return autorelease(alloc(class_named("UIAlertAction"))); });

    C_UIGestureRecognizer = def_class("UIGestureRecognizer", "NSObject");
    Class G = C_UIGestureRecognizer;
    def(G, "initWithTarget:action:", +[](id self, SEL, id t, SEL a) -> id { if (t) ext_get<GRExt>(self)->targets.push_back({t, a}); return self; });
    def(G, "addTarget:action:", +[](id self, SEL, id t, SEL a) { ext_get<GRExt>(self)->targets.push_back({t, a}); });
    def(G, "removeTarget:action:", +[](id self, SEL, id t, SEL) {
        auto& v = ext_get<GRExt>(self)->targets;
        v.erase(std::remove_if(v.begin(), v.end(), [t](const GRExt::T& x) { return !t || x.target == t; }), v.end());
    });
    def(G, "view", +[](id self, SEL) -> id { return ext_get<GRExt>(self)->view; });
    def(G, "state", +[](id self, SEL) -> long { return ext_get<GRExt>(self)->state; });
    def(G, "delegate", +[](id self, SEL) -> id { return ext_get<GRExt>(self)->delegate; });
    def(G, "setDelegate:", +[](id self, SEL, id d) { ext_get<GRExt>(self)->delegate = d; });
    def(G, "isEnabled", +[](id self, SEL) -> bool { return ext_get<GRExt>(self)->enabled; });
    def(G, "setEnabled:", +[](id self, SEL, bool b) { ext_get<GRExt>(self)->enabled = b; });
    def(G, "setCancelsTouchesInView:", +[](id self, SEL, bool b) { ext_get<GRExt>(self)->cancels = b; });
    def(G, "setDelaysTouchesBegan:", +[](id, SEL, bool) {});
    def(G, "setDelaysTouchesEnded:", +[](id, SEL, bool) {});
    def(G, "requireGestureRecognizerToFail:", +[](id, SEL, id) {});
    def(G, "locationInView:", +[](id, SEL, id) -> CGPoint { return {0, 0}; });
    def(G, "numberOfTouches", +[](id, SEL) -> NSUInteger { return 1; });
    C_UITapGR = def_class("UITapGestureRecognizer", "UIGestureRecognizer");
    def(C_UITapGR, "setNumberOfTapsRequired:", +[](id self, SEL, NSUInteger n) { ext_get<GRExt>(self)->taps = static_cast<long>(n); });
    def(C_UITapGR, "setNumberOfTouchesRequired:", +[](id self, SEL, NSUInteger n) { ext_get<GRExt>(self)->touches = static_cast<long>(n); });
    for (const char* n : {"UIPanGestureRecognizer", "UISwipeGestureRecognizer", "UIPinchGestureRecognizer", "UILongPressGestureRecognizer", "UIRotationGestureRecognizer"}) {
        Class c = def_class(n, "UIGestureRecognizer");
        for (const char* s : {"setDirection:", "setMinimumPressDuration:", "setMaximumNumberOfTouches:", "setMinimumNumberOfTouches:"}) def(c, s, +[](id, SEL, long) {});
    }

    Class BP = def_class("UIBezierPath", "NSObject");
    defc(BP, "bezierPath", +[](id self, SEL) -> id { return autorelease(alloc(reinterpret_cast<Class>(self))); });
    defc(BP, "bezierPathWithRect:", +[](id self, SEL, CGRect) -> id { return autorelease(alloc(reinterpret_cast<Class>(self))); });
    defc(BP, "bezierPathWithOvalInRect:", +[](id self, SEL, CGRect) -> id { return autorelease(alloc(reinterpret_cast<Class>(self))); });
    defc(BP, "bezierPathWithRoundedRect:cornerRadius:", +[](id self, SEL, CGRect, double) -> id { return autorelease(alloc(reinterpret_cast<Class>(self))); });
    for (const char* s : {"moveToPoint:", "addLineToPoint:"}) def(BP, s, +[](id, SEL, CGPoint) {});
    for (const char* s : {"closePath", "fill", "stroke", "addClip", "removeAllPoints"}) def(BP, s, +[](id, SEL) {});
    def(BP, "setLineWidth:", +[](id, SEL, double) {});
    def(BP, "CGPath", +[](id, SEL) -> void* { return nullptr; });

    Class PS = def_class("NSParagraphStyle", "NSObject");
    def_class("NSMutableParagraphStyle", "NSParagraphStyle");
    defc(PS, "defaultParagraphStyle", +[](id self, SEL) -> id { return autorelease(alloc(reinterpret_cast<Class>(self))); });
    for (const char* s : {"setAlignment:", "setLineBreakMode:", "setLineSpacing:", "setParagraphSpacing:"}) def(PS, s, +[](id, SEL, long) {});
    def(PS, "copyWithZone:", +[](id self, SEL, void*) -> id { return retain(self); });
    def(PS, "mutableCopyWithZone:", +[](id self, SEL, void*) -> id { return retain(self); });
    Class SH = def_class("NSShadow", "NSObject");
    for (const char* s : {"setShadowColor:", "setShadowBlurRadius:"}) def(SH, s, +[](id, SEL, id) {});
    def(SH, "setShadowOffset:", +[](id, SEL, CGSize) {});
    Class AT = def_class("NSAttributedString", "NSObject");
    def_class("NSMutableAttributedString", "NSAttributedString");
    struct AttrExt : Ext { id s = nullptr; ~AttrExt() override { release(s); } };
    def(AT, "initWithString:", +[](id self, SEL, id s) -> id { ext_get<AttrExt>(self)->s = send(s, "copy"); return self; });
    def(AT, "initWithString:attributes:", +[](id self, SEL, id s, id) -> id { ext_get<AttrExt>(self)->s = send(s, "copy"); return self; });
    def(AT, "string", +[](id self, SEL) -> id { return ext_get<AttrExt>(self)->s; });
    def(AT, "length", +[](id self, SEL) -> NSUInteger { return u16(ext_get<AttrExt>(self)->s).size(); });
    def(AT, "addAttribute:value:range:", +[](id, SEL, id, id, NSRange) {});
    def(AT, "addAttributes:range:", +[](id, SEL, id, NSRange) {});
    def(AT, "setAttributes:range:", +[](id, SEL, id, NSRange) {});
    def(AT, "boundingRectWithSize:options:context:", +[](id self, SEL, CGSize max, NSUInteger, id) -> CGRect { return {{0, 0}, text_size(ext_get<AttrExt>(self)->s, nullptr, max.width)}; });
    def(AT, "size", +[](id self, SEL) -> CGSize { return text_size(ext_get<AttrExt>(self)->s, nullptr, 0); });
}

static void define_window_vc_app() {
    C_UIWindow = def_class("UIWindow", "UIView");
    Class W = C_UIWindow;
    def(W, "makeKeyAndVisible", +[](id self, SEL) {
        window_make_key(self);
        vx(self)->hidden = false;
        if (id r = wx(self)->root) view_will_appear(r, true);
    });
    def(W, "makeKeyWindow", +[](id self, SEL) { window_make_key(self); });
    def(W, "isKeyWindow", +[](id self, SEL) -> bool { return wx(self)->key; });
    def(W, "becomeKeyWindow", +[](id, SEL) {});
    def(W, "resignKeyWindow", +[](id, SEL) {});
    def(W, "rootViewController", +[](id self, SEL) -> id { return wx(self)->root; });
    def(W, "setRootViewController:", +[](id self, SEL, id vc) { set_root(self, vc); });
    def(W, "screen", +[](id, SEL) -> id { return send_cls(C_UIScreen, "mainScreen"); });
    def(W, "setScreen:", +[](id, SEL, id) {});
    def(W, "windowLevel", +[](id self, SEL) -> double { return wx(self)->level; });
    def(W, "setWindowLevel:", +[](id self, SEL, double l) { wx(self)->level = l; });
    def(W, "sendEvent:", +[](id, SEL, id) {});
    def(W, "nextResponder", +[](id, SEL) -> id { return g_app; });
    def(W, "convertPoint:toWindow:", +[](id, SEL, CGPoint p, id) -> CGPoint { return p; });
    def(W, "convertPoint:fromWindow:", +[](id, SEL, CGPoint p, id) -> CGPoint { return p; });

    C_UIViewController = def_class("UIViewController", "UIResponder");
    Class VC = C_UIViewController;
    def(VC, "init", +[](id self, SEL) -> id { return send(self, "initWithNibName:bundle:", static_cast<id>(nullptr), static_cast<id>(nullptr)); });
    def(VC, "initWithNibName:bundle:", +[](id self, SEL, id nib, id) -> id { cx(self)->nib_name = retain(nib); return self; });
    def(VC, "initWithCoder:", +[](id self, SEL, id coder) -> id {
        auto* e = cx(self);
        id n = send(coder, "decodeObjectForKey:", str("UINibName"));
        e->nib_name = retain(n);
        e->storyboard = retain(send(coder, "decodeObjectForKey:", str("UIStoryboard")));
        return self;
    });
    def(VC, "view", +[](id self, SEL) -> id {
        auto* e = cx(self);
        if (!e->view && !e->loading) {
            e->loading = true;
            send<void>(self, "loadView");
            if (!e->view) {
                const Screen& s = screen();
                id v = send(send_cls(C_UIView, "alloc"), "initWithFrame:", CGRect{{0, 0}, {s.w_pt, s.h_pt}});
                send<void>(self, "setView:", v);
                release(v);
            }
            e->loading = false;
            send<void>(self, "viewDidLoad");
        }
        return e->view;
    });
    def(VC, "viewIfLoaded", +[](id self, SEL) -> id { return cx(self)->view; });
    def(VC, "isViewLoaded", +[](id self, SEL) -> bool { return cx(self)->view != nullptr; });
    def(VC, "setView:", +[](id self, SEL, id v) {
        auto* e = cx(self);
        if (e->view == v) return;
        if (e->view) if (ViewExt* ve = vx(e->view)) if (ve->controller == self) ve->controller = nullptr;
        id old = e->view;
        e->view = retain(v);
        if (v) vx(v)->controller = self;
        release(old);
    });
    def(VC, "loadView", +[](id self, SEL) {
        auto* e = cx(self);
        if (e->nib_name && nib_load_view(self)) return;
        const Screen& s = screen();
        id v = send(send_cls(C_UIView, "alloc"), "initWithFrame:", CGRect{{0, 0}, {s.w_pt, s.h_pt}});
        send<void>(self, "setView:", v);
        release(v);
    });
    for (const char* s : {"viewDidLoad", "viewDidUnload", "didReceiveMemoryWarning", "viewWillLayoutSubviews", "viewDidLayoutSubviews", "updateViewConstraints",
                          "setNeedsStatusBarAppearanceUpdate", "setNeedsUpdateOfHomeIndicatorAutoHidden", "awakeFromNib", "removeFromParentViewController"})
        def(VC, s, +[](id, SEL) {});
    for (const char* s : {"viewWillAppear:", "viewDidAppear:", "viewWillDisappear:", "viewDidDisappear:"}) def(VC, s, +[](id, SEL, bool) {});
    def(VC, "nibName", +[](id self, SEL) -> id { return cx(self)->nib_name; });
    def(VC, "nibBundle", +[](id, SEL) -> id { return send_cls(class_named("NSBundle"), "mainBundle"); });
    def(VC, "storyboard", +[](id self, SEL) -> id { return cx(self)->storyboard; });
    def(VC, "title", +[](id self, SEL) -> id { return cx(self)->title; });
    def(VC, "setTitle:", +[](id self, SEL, id t) { auto* e = cx(self); id o = e->title; e->title = send(t, "copy"); release(o); });
    def(VC, "nextResponder", +[](id self, SEL) -> id { id v = cx(self)->view; return v ? vx(v)->superview : nullptr; });
    def(VC, "interfaceOrientation", +[](id, SEL) -> long { return screen().landscape ? 4 : 1; });
    def(VC, "shouldAutorotate", +[](id, SEL) -> bool { return true; });
    def(VC, "shouldAutorotateToInterfaceOrientation:", +[](id, SEL, long o) -> bool { return screen().landscape ? (o == 3 || o == 4) : (o == 1); });
    def(VC, "supportedInterfaceOrientations", +[](id, SEL) -> NSUInteger { return screen().landscape ? 24 : 2; });
    def(VC, "preferredInterfaceOrientationForPresentation", +[](id, SEL) -> long { return screen().landscape ? 4 : 1; });
    def(VC, "prefersStatusBarHidden", +[](id, SEL) -> bool { return true; });
    def(VC, "preferredStatusBarStyle", +[](id, SEL) -> long { return 0; });
    def(VC, "setWantsFullScreenLayout:", +[](id, SEL, bool) {});
    def(VC, "wantsFullScreenLayout", +[](id, SEL) -> bool { return true; });
    for (const char* s : {"setModalTransitionStyle:", "setModalPresentationStyle:", "setEdgesForExtendedLayout:", "setAutomaticallyAdjustsScrollViewInsets:",
                          "setExtendedLayoutIncludesOpaqueBars:", "setDefinesPresentationContext:", "setProvidesPresentationContextTransitionStyle:",
                          "setHidesBottomBarWhenPushed:", "setPreferredContentSize:"})
        def(VC, s, +[](id, SEL, long) {});
    def(VC, "modalTransitionStyle", +[](id, SEL) -> long { return 0; });
    def(VC, "modalPresentationStyle", +[](id, SEL) -> long { return 0; });
    auto present = +[](id self, SEL, id vc, bool, id completion) {
        if (!vc) return;
        auto* e = cx(self);
        id old = e->presented;
        e->presented = retain(vc);
        cx(vc)->presenting = self;
        release(old);
        id v = send(vc, "view");
        id w = g_key_window;
        if (w && v) {
            const Screen& s = screen();
            view_set_frame(v, {{0, 0}, {s.w_pt, s.h_pt}});
            add_subview(w, v);
        }
        view_will_appear(vc, true);
        if (completion) reinterpret_cast<void (*)(id)>(block_invoke_ptr(completion))(completion);
    };
    def(VC, "presentViewController:animated:completion:", present);
    def(VC, "presentModalViewController:animated:", +[](id self, SEL, id vc, bool a) {
        reinterpret_cast<void (*)(id, SEL, id, bool, id)>(lookup(self, sel("presentViewController:animated:completion:")))(self, nullptr, vc, a, nullptr);
    });
    auto dismiss = +[](id self, SEL, bool, id completion) {
        auto* e = cx(self);
        id target = e->presented ? self : e->presenting;
        if (!target) { if (completion) reinterpret_cast<void (*)(id)>(block_invoke_ptr(completion))(completion); return; }
        auto* te = cx(target);
        id vc = te->presented;
        te->presented = nullptr;
        if (vc) {
            view_will_appear(vc, false);
            if (id v = cx(vc)->view) remove_from_superview(v);
            cx(vc)->presenting = nullptr;
            release(vc);
        }
        if (completion) reinterpret_cast<void (*)(id)>(block_invoke_ptr(completion))(completion);
    };
    def(VC, "dismissViewControllerAnimated:completion:", dismiss);
    def(VC, "dismissModalViewControllerAnimated:", +[](id self, SEL, bool a) {
        reinterpret_cast<void (*)(id, SEL, bool, id)>(lookup(self, sel("dismissViewControllerAnimated:completion:")))(self, nullptr, a, nullptr);
    });
    def(VC, "presentedViewController", +[](id self, SEL) -> id { return cx(self)->presented; });
    def(VC, "modalViewController", +[](id self, SEL) -> id { return cx(self)->presented; });
    def(VC, "presentingViewController", +[](id self, SEL) -> id { return cx(self)->presenting; });
    def(VC, "parentViewController", +[](id self, SEL) -> id { return cx(self)->parent ? cx(self)->parent : cx(self)->presenting; });
    def(VC, "navigationController", +[](id, SEL) -> id { return nullptr; });
    def(VC, "tabBarController", +[](id, SEL) -> id { return nullptr; });
    def(VC, "navigationItem", +[](id self, SEL) -> id {
        auto& p = cx(self)->props;
        if (!p.count("navItem")) p["navItem"] = alloc(class_named("UINavigationItem"));
        return p["navItem"];
    });
    def(VC, "addChildViewController:", +[](id self, SEL, id c) { cx(self)->children.push_back(retain(c)); cx(c)->parent = self; });
    def(VC, "childViewControllers", +[](id self, SEL) -> id { return arr(cx(self)->children); });
    def(VC, "didMoveToParentViewController:", +[](id, SEL, id) {});
    def(VC, "willMoveToParentViewController:", +[](id, SEL, id) {});
    def(VC, "isBeingPresented", +[](id, SEL) -> bool { return false; });
    def(VC, "isBeingDismissed", +[](id, SEL) -> bool { return false; });
    def(VC, "isMovingToParentViewController", +[](id, SEL) -> bool { return false; });
    def(VC, "transitionCoordinator", +[](id, SEL) -> id { return nullptr; });
    def(VC, "traitCollection", +[](id, SEL) -> id { return nullptr; });
    def(VC, "performSegueWithIdentifier:sender:", +[](id, SEL, id i, id) { log("performSegueWithIdentifier:%s not supported", utf8(i).c_str()); });

    Class NC = def_class("UINavigationController", "UIViewController");
    def(NC, "initWithRootViewController:", +[](id self, SEL, id root) -> id {
        send(self, "initWithNibName:bundle:", static_cast<id>(nullptr), static_cast<id>(nullptr));
        if (root) { cx(self)->children.push_back(retain(root)); cx(root)->parent = self; }
        return self;
    });
    def(NC, "loadView", +[](id self, SEL) {
        const Screen& s = screen();
        id v = send(send_cls(C_UIView, "alloc"), "initWithFrame:", CGRect{{0, 0}, {s.w_pt, s.h_pt}});
        send<void>(self, "setView:", v);
        release(v);
        auto& ch = cx(self)->children;
        if (!ch.empty()) { id cv = send(ch.back(), "view"); view_set_frame(cv, {{0, 0}, {s.w_pt, s.h_pt}}); add_subview(v, cv); }
    });
    def(NC, "pushViewController:animated:", +[](id self, SEL, id vc, bool) {
        cx(self)->children.push_back(retain(vc));
        cx(vc)->parent = self;
        if (id v = cx(self)->view) { const Screen& s = screen(); id cv = send(vc, "view"); view_set_frame(cv, {{0, 0}, {s.w_pt, s.h_pt}}); add_subview(v, cv); }
        view_will_appear(vc, true);
    });
    def(NC, "popViewControllerAnimated:", +[](id self, SEL, bool) -> id {
        auto& ch = cx(self)->children;
        if (ch.size() <= 1) return nullptr;
        id vc = ch.back();
        ch.pop_back();
        if (id v = cx(vc)->view) remove_from_superview(v);
        return autorelease(vc);
    });
    def(NC, "popToRootViewControllerAnimated:", +[](id self, SEL, bool) -> id {
        auto& ch = cx(self)->children;
        while (ch.size() > 1) { id vc = ch.back(); ch.pop_back(); if (id v = cx(vc)->view) remove_from_superview(v); release(vc); }
        return arr({});
    });
    def(NC, "viewControllers", +[](id self, SEL) -> id { return arr(cx(self)->children); });
    def(NC, "topViewController", +[](id self, SEL) -> id { auto& ch = cx(self)->children; return ch.empty() ? nullptr : ch.back(); });
    def(NC, "visibleViewController", +[](id self, SEL) -> id { auto& ch = cx(self)->children; return ch.empty() ? nullptr : ch.back(); });
    def(NC, "setNavigationBarHidden:", +[](id, SEL, bool) {});
    def(NC, "setNavigationBarHidden:animated:", +[](id, SEL, bool, bool) {});
    def(NC, "setToolbarHidden:", +[](id, SEL, bool) {});
    def(NC, "navigationBar", +[](id, SEL) -> id { return autorelease(alloc(class_named("UINavigationBar"))); });
    def(NC, "setDelegate:", +[](id, SEL, id) {});
    Class TBC = def_class("UITabBarController", "UIViewController");
    def(TBC, "setViewControllers:", +[](id self, SEL, id vcs) { for (id v : arr_items(vcs)) cx(self)->children.push_back(retain(v)); });
    def(TBC, "setViewControllers:animated:", +[](id self, SEL, id vcs, bool) { for (id v : arr_items(vcs)) cx(self)->children.push_back(retain(v)); });
    def(TBC, "viewControllers", +[](id self, SEL) -> id { return arr(cx(self)->children); });
    def(TBC, "setSelectedIndex:", +[](id, SEL, NSUInteger) {});
    def(TBC, "selectedIndex", +[](id, SEL) -> NSUInteger { return 0; });
    def(TBC, "setDelegate:", +[](id, SEL, id) {});
    def_class("UIActivityViewController", "UIViewController");
    def_class("UIImagePickerController", "UINavigationController");

    C_UIApplication = def_class("UIApplication", "UIResponder");
    Class A = C_UIApplication;
    struct AppExt : Ext { id delegate = nullptr; bool idle = false, status_hidden = true, net = false; long badge = 0, orientation = 4; };
    defc(A, "sharedApplication", +[](id, SEL) -> id { return g_app; });
    def(A, "init", +[](id self, SEL) -> id { if (!g_app) g_app = self; ext_get<AppExt>(self)->orientation = screen().landscape ? 4 : 1; return self; });
    def(A, "delegate", +[](id self, SEL) -> id { return ext_get<AppExt>(self)->delegate; });
    def(A, "setDelegate:", +[](id self, SEL, id d) { ext_get<AppExt>(self)->delegate = d; });
    def(A, "keyWindow", +[](id, SEL) -> id { return g_key_window; });
    def(A, "windows", +[](id, SEL) -> id { return arr(g_windows); });
    def(A, "nextResponder", +[](id, SEL) -> id { return nullptr; });
    def(A, "statusBarOrientation", +[](id self, SEL) -> long { return ext_get<AppExt>(self)->orientation; });
    def(A, "setStatusBarOrientation:", +[](id self, SEL, long o) { ext_get<AppExt>(self)->orientation = o; });
    def(A, "setStatusBarOrientation:animated:", +[](id self, SEL, long o, bool) { ext_get<AppExt>(self)->orientation = o; });
    def(A, "isStatusBarHidden", +[](id self, SEL) -> bool { return ext_get<AppExt>(self)->status_hidden; });
    def(A, "setStatusBarHidden:", +[](id self, SEL, bool h) { ext_get<AppExt>(self)->status_hidden = h; });
    def(A, "setStatusBarHidden:animated:", +[](id self, SEL, bool h, bool) { ext_get<AppExt>(self)->status_hidden = h; });
    def(A, "setStatusBarHidden:withAnimation:", +[](id self, SEL, bool h, long) { ext_get<AppExt>(self)->status_hidden = h; });
    def(A, "statusBarFrame", +[](id, SEL) -> CGRect { return {{0, 0}, {0, 0}}; });
    def(A, "setStatusBarStyle:", +[](id, SEL, long) {});
    def(A, "setStatusBarStyle:animated:", +[](id, SEL, long, bool) {});
    def(A, "statusBarStyle", +[](id, SEL) -> long { return 0; });
    def(A, "isIdleTimerDisabled", +[](id self, SEL) -> bool { return ext_get<AppExt>(self)->idle; });
    def(A, "setIdleTimerDisabled:", +[](id self, SEL, bool b) { ext_get<AppExt>(self)->idle = b; });
    def(A, "isNetworkActivityIndicatorVisible", +[](id self, SEL) -> bool { return ext_get<AppExt>(self)->net; });
    def(A, "setNetworkActivityIndicatorVisible:", +[](id self, SEL, bool b) { ext_get<AppExt>(self)->net = b; });
    def(A, "applicationIconBadgeNumber", +[](id self, SEL) -> long { return ext_get<AppExt>(self)->badge; });
    def(A, "setApplicationIconBadgeNumber:", +[](id self, SEL, long n) { ext_get<AppExt>(self)->badge = n; });
    def(A, "applicationState", +[](id, SEL) -> long { return 0; });
    def(A, "backgroundTimeRemaining", +[](id, SEL) -> double { return 600; });
    def(A, "openURL:", +[](id, SEL, id u) -> bool { log("openURL: %s (ignored)", utf8(send(u, "absoluteString")).c_str()); return false; });
    def(A, "openURL:options:completionHandler:", +[](id, SEL, id u, id, id h) {
        log("openURL: %s (ignored)", utf8(send(u, "absoluteString")).c_str());
        if (h) reinterpret_cast<void (*)(id, bool)>(block_invoke_ptr(h))(h, false);
    });
    def(A, "canOpenURL:", +[](id, SEL, id) -> bool { return false; });
    def(A, "beginBackgroundTaskWithExpirationHandler:", +[](id, SEL, id) -> NSUInteger { static NSUInteger n = 0; return ++n; });
    def(A, "beginBackgroundTaskWithName:expirationHandler:", +[](id, SEL, id, id) -> NSUInteger { static NSUInteger n = 100; return ++n; });
    def(A, "endBackgroundTask:", +[](id, SEL, NSUInteger) {});
    for (const char* s : {"beginIgnoringInteractionEvents", "endIgnoringInteractionEvents", "registerForRemoteNotifications", "unregisterForRemoteNotifications",
                          "cancelAllLocalNotifications", "beginReceivingRemoteControlEvents", "endReceivingRemoteControlEvents"})
        def(A, s, +[](id, SEL) {});
    def(A, "isIgnoringInteractionEvents", +[](id, SEL) -> bool { return false; });
    def(A, "registerForRemoteNotificationTypes:", +[](id, SEL, NSUInteger) {});
    def(A, "registerUserNotificationSettings:", +[](id, SEL, id) {});
    def(A, "currentUserNotificationSettings", +[](id, SEL) -> id { return nullptr; });
    def(A, "enabledRemoteNotificationTypes", +[](id, SEL) -> NSUInteger { return 0; });
    def(A, "isRegisteredForRemoteNotifications", +[](id, SEL) -> bool { return false; });
    def(A, "scheduleLocalNotification:", +[](id, SEL, id) {});
    def(A, "presentLocalNotificationNow:", +[](id, SEL, id) {});
    def(A, "cancelLocalNotification:", +[](id, SEL, id) {});
    def(A, "scheduledLocalNotifications", +[](id, SEL) -> id { return arr({}); });
    def(A, "setMinimumBackgroundFetchInterval:", +[](id, SEL, double) {});
    def(A, "setApplicationSupportsShakeToEdit:", +[](id, SEL, bool) {});
    def(A, "applicationSupportsShakeToEdit", +[](id, SEL) -> bool { return false; });
    def(A, "sendAction:to:from:forEvent:", +[](id, SEL, SEL a, id to, id from, id) -> bool { if (to) send<void>(to, a, from); return to != nullptr; });
    def(A, "supportedInterfaceOrientationsForWindow:", +[](id, SEL, id) -> NSUInteger { return screen().landscape ? 24 : 2; });
    def(A, "userInterfaceLayoutDirection", +[](id, SEL) -> long { return 0; });
    def(A, "preferredContentSizeCategory", +[](id, SEL) -> id { return str("UICTContentSizeCategoryL"); });
    def(A, "setNewsstandIconImage:", +[](id, SEL, id) {});
    def(A, "connectedScenes", +[](id, SEL) -> id { return send_cls(class_named("NSSet"), "set"); });
    Class LN = def_class("UILocalNotification", "NSObject");
    for (const char* s : {"setFireDate:", "setAlertBody:", "setAlertAction:", "setSoundName:", "setUserInfo:", "setTimeZone:", "setAlertLaunchImage:"}) def(LN, s, +[](id, SEL, id) {});
    for (const char* s : {"setApplicationIconBadgeNumber:", "setRepeatInterval:", "setHasAction:"}) def(LN, s, +[](id, SEL, long) {});
    Class UNS = def_class("UIUserNotificationSettings", "NSObject");
    defc(UNS, "settingsForTypes:categories:", +[](id, SEL, NSUInteger, id) -> id { return autorelease(alloc(class_named("UIUserNotificationSettings"))); });
    def(UNS, "types", +[](id, SEL) -> NSUInteger { return 0; });
}

struct ImageExt;   // app_cg.cpp owns UIImage storage

static void define_screen_device() {
    C_UIScreen = def_class("UIScreen", "NSObject");
    Class S = C_UIScreen;
    static id main = alloc(S);
    defc(S, "mainScreen", +[](id, SEL) -> id { return main; });
    defc(S, "screens", +[](id, SEL) -> id { return arr({main}); });
    def(S, "bounds", +[](id, SEL) -> CGRect { const Screen& s = screen(); return {{0, 0}, {s.w_pt, s.h_pt}}; });
    def(S, "applicationFrame", +[](id, SEL) -> CGRect { const Screen& s = screen(); return {{0, 0}, {s.w_pt, s.h_pt}}; });
    def(S, "nativeBounds", +[](id, SEL) -> CGRect { const Screen& s = screen(); return {{0, 0}, {std::min(s.w_pt, s.h_pt) * s.scale, std::max(s.w_pt, s.h_pt) * s.scale}}; });
    def(S, "scale", +[](id, SEL) -> double { return screen().scale; });
    def(S, "nativeScale", +[](id, SEL) -> double { return screen().scale; });
    def(S, "brightness", +[](id, SEL) -> double { return 0.8; });
    def(S, "setBrightness:", +[](id, SEL, double) {});
    def(S, "maximumFramesPerSecond", +[](id, SEL) -> long { return 60; });
    def(S, "coordinateSpace", +[](id self, SEL) -> id { return self; });
    def(S, "fixedCoordinateSpace", +[](id self, SEL) -> id { return self; });
    def(S, "setOverscanCompensation:", +[](id, SEL, long) {});
    def(S, "mirroredScreen", +[](id, SEL) -> id { return nullptr; });
    Class M = def_class("UIScreenMode", "NSObject");
    static id mode = alloc(M);
    def(M, "size", +[](id, SEL) -> CGSize { const Screen& s = screen(); return {s.w_pt * s.scale, s.h_pt * s.scale}; });
    def(M, "pixelAspectRatio", +[](id, SEL) -> double { return 1; });
    def(S, "currentMode", +[](id, SEL) -> id { return mode; });
    def(S, "preferredMode", +[](id, SEL) -> id { return mode; });
    def(S, "availableModes", +[](id, SEL) -> id { return arr({mode}); });
    def(S, "setCurrentMode:", +[](id, SEL, id) {});
    def(S, "displayLinkWithTarget:selector:", +[](id, SEL, id t, SEL s) -> id { return send_cls(C_CADisplayLink, "displayLinkWithTarget:selector:", t, s); });

    Class D = def_class("UIDevice", "NSObject");
    static id dev = alloc(D);
    defc(D, "currentDevice", +[](id, SEL) -> id { return dev; });
    def(D, "systemVersion", +[](id, SEL) -> id { const char* v = getenv("LOADER_APP_IOS_VERSION"); return str(v ? v : "8.4"); });
    def(D, "systemName", +[](id, SEL) -> id { return str("iPhone OS"); });
    def(D, "model", +[](id, SEL) -> id { return str(screen().pad ? "iPad" : "iPhone"); });
    def(D, "localizedModel", +[](id, SEL) -> id { return str(screen().pad ? "iPad" : "iPhone"); });
    def(D, "name", +[](id, SEL) -> id { return str(screen().pad ? "iPad" : "iPhone"); });
    def(D, "userInterfaceIdiom", +[](id, SEL) -> long { return screen().pad ? 1 : 0; });
    def(D, "orientation", +[](id, SEL) -> long { return screen().landscape ? 3 : 1; });
    def(D, "beginGeneratingDeviceOrientationNotifications", +[](id, SEL) {});
    def(D, "endGeneratingDeviceOrientationNotifications", +[](id, SEL) {});
    def(D, "isGeneratingDeviceOrientationNotifications", +[](id, SEL) -> bool { return true; });
    def(D, "batteryLevel", +[](id, SEL) -> float { return 1.0f; });
    def(D, "batteryState", +[](id, SEL) -> long { return 3; });
    def(D, "isBatteryMonitoringEnabled", +[](id, SEL) -> bool { return false; });
    def(D, "setBatteryMonitoringEnabled:", +[](id, SEL, bool) {});
    def(D, "isProximityMonitoringEnabled", +[](id, SEL) -> bool { return false; });
    def(D, "setProximityMonitoringEnabled:", +[](id, SEL, bool) {});
    def(D, "isMultitaskingSupported", +[](id, SEL) -> bool { return true; });
    def(D, "playInputClick", +[](id, SEL) {});
    def(D, "identifierForVendor", +[](id, SEL) -> id {
        static id uuid = nullptr;
        if (!uuid) {
            id saved = send(send_cls(class_named("NSUserDefaults"), "standardUserDefaults"), "stringForKey:", str("xnuroid.identifierForVendor"));
            if (saved) uuid = retain(send(send_cls(class_named("NSUUID"), "alloc"), "initWithUUIDString:", saved));
            if (!uuid) {
                uuid = retain(send_cls(class_named("NSUUID"), "UUID"));
                send<void>(send_cls(class_named("NSUserDefaults"), "standardUserDefaults"), "setObject:forKey:", send(uuid, "UUIDString"), str("xnuroid.identifierForVendor"));
            }
        }
        return uuid;
    });
    def(D, "uniqueIdentifier", +[](id self, SEL) -> id { return send(send(self, "identifierForVendor"), "UUIDString"); });
}

static void define_touch_event() {
    C_UITouch = def_class("UITouch", "NSObject");
    Class T = C_UITouch;
    def(T, "locationInView:", +[](id self, SEL, id v) -> CGPoint { auto* e = ext_get<TouchExt>(self); return v ? convert_to_view(v, e->loc) : e->loc; });
    def(T, "previousLocationInView:", +[](id self, SEL, id v) -> CGPoint { auto* e = ext_get<TouchExt>(self); return v ? convert_to_view(v, e->prev) : e->prev; });
    def(T, "preciseLocationInView:", +[](id self, SEL, id v) -> CGPoint { auto* e = ext_get<TouchExt>(self); return v ? convert_to_view(v, e->loc) : e->loc; });
    def(T, "precisePreviousLocationInView:", +[](id self, SEL, id v) -> CGPoint { auto* e = ext_get<TouchExt>(self); return v ? convert_to_view(v, e->prev) : e->prev; });
    def(T, "phase", +[](id self, SEL) -> long { return ext_get<TouchExt>(self)->phase; });
    def(T, "tapCount", +[](id self, SEL) -> NSUInteger { return static_cast<NSUInteger>(ext_get<TouchExt>(self)->taps); });
    def(T, "timestamp", +[](id self, SEL) -> double { return ext_get<TouchExt>(self)->timestamp; });
    def(T, "view", +[](id self, SEL) -> id { return ext_get<TouchExt>(self)->view; });
    def(T, "window", +[](id self, SEL) -> id { return ext_get<TouchExt>(self)->window; });
    def(T, "majorRadius", +[](id, SEL) -> double { return 20; });
    def(T, "majorRadiusTolerance", +[](id, SEL) -> double { return 5; });
    def(T, "force", +[](id, SEL) -> double { return 0; });
    def(T, "maximumPossibleForce", +[](id, SEL) -> double { return 0; });
    def(T, "type", +[](id, SEL) -> long { return 0; });
    def(T, "gestureRecognizers", +[](id, SEL) -> id { return arr({}); });
    def(T, "copyWithZone:", +[](id self, SEL, void*) -> id { return retain(self); });

    C_UIEvent = def_class("UIEvent", "NSObject");
    Class E = C_UIEvent;
    def(E, "allTouches", +[](id self, SEL) -> id { return send_cls(class_named("NSSet"), "setWithArray:", arr(ext_get<EventExt>(self)->touches)); });
    def(E, "touchesForView:", +[](id self, SEL, id v) -> id {
        std::vector<id> r; for (id t : ext_get<EventExt>(self)->touches) if (ext_get<TouchExt>(t)->view == v) r.push_back(t);
        return send_cls(class_named("NSSet"), "setWithArray:", arr(r));
    });
    def(E, "touchesForWindow:", +[](id self, SEL, id) -> id { return send_cls(class_named("NSSet"), "setWithArray:", arr(ext_get<EventExt>(self)->touches)); });
    def(E, "touchesForGestureRecognizer:", +[](id self, SEL, id) -> id { return send_cls(class_named("NSSet"), "setWithArray:", arr(ext_get<EventExt>(self)->touches)); });
    def(E, "timestamp", +[](id self, SEL) -> double { return ext_get<EventExt>(self)->timestamp; });
    def(E, "type", +[](id, SEL) -> long { return 0; });
    def(E, "subtype", +[](id, SEL) -> long { return 0; });
}

static void define_layers() {
    C_CALayer = def_class("CALayer", "NSObject");
    Class L = C_CALayer;
    defc(L, "layer", +[](id self, SEL) -> id { return autorelease(send(send(self, "alloc"), "init")); });
    def(L, "init", +[](id self, SEL) -> id { lx(self); return self; });
    def(L, "bounds", +[](id self, SEL) -> CGRect { return lx(self)->bounds; });
    def(L, "setBounds:", +[](id self, SEL, CGRect r) { lx(self)->bounds = r; });
    def(L, "frame", +[](id self, SEL) -> CGRect {
        auto* e = lx(self);
        return {{e->position.x - e->bounds.size.width * e->anchor.x, e->position.y - e->bounds.size.height * e->anchor.y}, e->bounds.size};
    });
    def(L, "setFrame:", +[](id self, SEL, CGRect r) {
        auto* e = lx(self);
        e->bounds.size = r.size;
        e->position = {r.origin.x + r.size.width * e->anchor.x, r.origin.y + r.size.height * e->anchor.y};
    });
    def(L, "position", +[](id self, SEL) -> CGPoint { return lx(self)->position; });
    def(L, "setPosition:", +[](id self, SEL, CGPoint p) { lx(self)->position = p; });
    def(L, "anchorPoint", +[](id self, SEL) -> CGPoint { return lx(self)->anchor; });
    def(L, "setAnchorPoint:", +[](id self, SEL, CGPoint p) { lx(self)->anchor = p; });
    def(L, "zPosition", +[](id self, SEL) -> double { return lx(self)->z; });
    def(L, "setZPosition:", +[](id self, SEL, double z) { lx(self)->z = z; });
    def(L, "opacity", +[](id self, SEL) -> float { return static_cast<float>(lx(self)->opacity); });
    def(L, "setOpacity:", +[](id self, SEL, float o) { lx(self)->opacity = o; });
    def(L, "isHidden", +[](id self, SEL) -> bool { return lx(self)->hidden; });
    def(L, "setHidden:", +[](id self, SEL, bool h) { lx(self)->hidden = h; });
    def(L, "isOpaque", +[](id self, SEL) -> bool { return lx(self)->opaque; });
    def(L, "setOpaque:", +[](id self, SEL, bool o) { lx(self)->opaque = o; });
    def(L, "contentsScale", +[](id self, SEL) -> double { return lx(self)->scale; });
    def(L, "setContentsScale:", +[](id self, SEL, double s) { lx(self)->scale = s; });
    def(L, "cornerRadius", +[](id self, SEL) -> double { return lx(self)->corner; });
    def(L, "setCornerRadius:", +[](id self, SEL, double r) { lx(self)->corner = r; });
    def(L, "masksToBounds", +[](id self, SEL) -> bool { return lx(self)->masks; });
    def(L, "setMasksToBounds:", +[](id self, SEL, bool m) { lx(self)->masks = m; });
    def(L, "contents", +[](id self, SEL) -> id { return lx(self)->contents; });
    def(L, "setContents:", +[](id self, SEL, id c) { auto* e = lx(self); id o = e->contents; e->contents = retain(c); release(o); });
    def(L, "delegate", +[](id self, SEL) -> id { return lx(self)->delegate; });
    def(L, "setDelegate:", +[](id self, SEL, id d) { lx(self)->delegate = d; });
    def(L, "sublayers", +[](id self, SEL) -> id { return arr(lx(self)->sublayers); });
    def(L, "superlayer", +[](id self, SEL) -> id { return lx(self)->superlayer; });
    def(L, "addSublayer:", +[](id self, SEL, id l) { if (!l) return; lx(self)->sublayers.push_back(retain(l)); lx(l)->superlayer = self; });
    def(L, "insertSublayer:atIndex:", +[](id self, SEL, id l, unsigned i) {
        auto& s = lx(self)->sublayers; s.insert(s.begin() + std::min<size_t>(i, s.size()), retain(l)); lx(l)->superlayer = self;
    });
    def(L, "removeFromSuperlayer", +[](id self, SEL) {
        id sup = lx(self)->superlayer;
        if (!sup) return;
        auto& s = lx(sup)->sublayers;
        auto it = std::find(s.begin(), s.end(), self);
        lx(self)->superlayer = nullptr;
        if (it != s.end()) { s.erase(it); release(self); }
    });
    for (const char* s : {"setNeedsDisplay", "display", "setNeedsLayout", "layoutIfNeeded", "layoutSublayers", "removeAllAnimations", "displayIfNeeded"})
        def(L, s, +[](id, SEL) {});
    def(L, "addAnimation:forKey:", +[](id, SEL, id, id) {});
    def(L, "removeAnimationForKey:", +[](id, SEL, id) {});
    def(L, "animationForKey:", +[](id, SEL, id) -> id { return nullptr; });
    def(L, "animationKeys", +[](id, SEL) -> id { return nullptr; });
    def(L, "presentationLayer", +[](id self, SEL) -> id { return self; });
    def(L, "modelLayer", +[](id self, SEL) -> id { return self; });
    def(L, "renderInContext:", +[](id, SEL, void*) {});
    for (const char* s : {"setBorderWidth:", "setShadowOpacity:", "setShadowRadius:", "setRasterizationScale:"}) def(L, s, +[](id, SEL, double) {});
    for (const char* s : {"setBorderColor:", "setShadowColor:", "setBackgroundColor:", "setShadowPath:", "setMask:", "setName:", "setContentsGravity:",
                          "setMinificationFilter:", "setMagnificationFilter:", "setActions:", "setStyle:"})
        def(L, s, +[](id, SEL, void*) {});
    def(L, "setShadowOffset:", +[](id, SEL, CGSize) {});
    def(L, "setShouldRasterize:", +[](id, SEL, bool) {});
    def(L, "setGeometryFlipped:", +[](id, SEL, bool) {});
    def(L, "setNeedsDisplayOnBoundsChange:", +[](id, SEL, bool) {});
    def(L, "setTransform:", +[](id, SEL, const void*) {});
    def(L, "setAffineTransform:", +[](id, SEL, CGAffineTransform) {});
    def(L, "affineTransform", +[](id, SEL) -> CGAffineTransform { return {1, 0, 0, 1, 0, 0}; });
    def(L, "convertPoint:toLayer:", +[](id, SEL, CGPoint p, id) -> CGPoint { return p; });
    def(L, "convertPoint:fromLayer:", +[](id, SEL, CGPoint p, id) -> CGPoint { return p; });

    C_CAEAGLLayer = def_class("CAEAGLLayer", "CALayer");
    def(C_CAEAGLLayer, "drawableProperties", +[](id self, SEL) -> id { return lx(self)->drawable_props; });
    def(C_CAEAGLLayer, "setDrawableProperties:", +[](id self, SEL, id d) { auto* e = lx(self); id o = e->drawable_props; e->drawable_props = send(d, "copy"); release(o); });
    def(C_CAEAGLLayer, "setPresentsWithTransaction:", +[](id, SEL, bool) {});
    def_class("CAMetalLayer", "CALayer");
    def_class("CAGradientLayer", "CALayer");
    def_class("CAShapeLayer", "CALayer");
    def_class("CATextLayer", "CALayer");
    def_class("CAScrollLayer", "CALayer");
    def_class("CAEmitterLayer", "CALayer");
    def_class("CAReplicatorLayer", "CALayer");

    C_CADisplayLink = def_class("CADisplayLink", "NSObject");
    Class DL = C_CADisplayLink;
    defc(DL, "displayLinkWithTarget:selector:", +[](id, SEL, id t, SEL s) -> id {
        id l = alloc(C_CADisplayLink);
        auto* e = ext_get<LinkExt>(l);
        e->target = retain(t);
        e->sel = s;
        return autorelease(l);
    });
    def(DL, "addToRunLoop:forMode:", +[](id self, SEL, id, id) { link_schedule(self); });
    def(DL, "removeFromRunLoop:forMode:", +[](id self, SEL, id, id) { link_unschedule(self); });
    def(DL, "invalidate", +[](id self, SEL) {
        auto* e = ext_get<LinkExt>(self);
        if (!e->valid) return;
        e->valid = false;
        link_unschedule(self);
        id t = e->target;
        e->target = nullptr;
        release(t);
    });
    def(DL, "isPaused", +[](id self, SEL) -> bool { return ext_get<LinkExt>(self)->paused; });
    def(DL, "paused", +[](id self, SEL) -> bool { return ext_get<LinkExt>(self)->paused; });
    def(DL, "setPaused:", +[](id self, SEL, bool p) { ext_get<LinkExt>(self)->paused = p; });
    def(DL, "frameInterval", +[](id self, SEL) -> long { return ext_get<LinkExt>(self)->interval; });
    def(DL, "setFrameInterval:", +[](id self, SEL, long n) {
        auto* e = ext_get<LinkExt>(self);
        e->interval = std::max<long>(1, n);
        if (e->scheduled) { link_unschedule(self); link_schedule(self); }
    });
    def(DL, "preferredFramesPerSecond", +[](id self, SEL) -> long { return ext_get<LinkExt>(self)->preferred_fps; });
    def(DL, "setPreferredFramesPerSecond:", +[](id self, SEL, long n) {
        auto* e = ext_get<LinkExt>(self);
        e->preferred_fps = n;
        if (e->scheduled) { link_unschedule(self); link_schedule(self); }
    });
    def(DL, "timestamp", +[](id self, SEL) -> double { return ext_get<LinkExt>(self)->timestamp; });
    def(DL, "targetTimestamp", +[](id self, SEL) -> double { auto* e = ext_get<LinkExt>(self); return e->timestamp + link_period(e); });
    def(DL, "duration", +[](id self, SEL) -> double { return link_period(ext_get<LinkExt>(self)); });

    Class TX = def_class("CATransaction", "NSObject");
    for (const char* s : {"begin", "commit", "flush"}) defc(TX, s, +[](id, SEL) {});
    defc(TX, "setDisableActions:", +[](id, SEL, bool) {});
    defc(TX, "setAnimationDuration:", +[](id, SEL, double) {});
    defc(TX, "setCompletionBlock:", +[](id, SEL, id b) {
        if (!b) return;
        id c = block_copy(b);
        post_main([c] { reinterpret_cast<void (*)(id)>(block_invoke_ptr(c))(c); block_release(c); });
    });
    defc(TX, "setValue:forKey:", +[](id, SEL, id, id) {});
    Class AN = def_class("CAAnimation", "NSObject");
    defc(AN, "animation", +[](id self, SEL) -> id { return autorelease(alloc(reinterpret_cast<Class>(self))); });
    for (const char* s : {"setDuration:", "setBeginTime:", "setRepeatCount:", "setSpeed:", "setTimeOffset:"}) def(AN, s, +[](id, SEL, double) {});
    for (const char* s : {"setDelegate:", "setTimingFunction:", "setFillMode:", "setFromValue:", "setToValue:", "setByValue:", "setValues:", "setKeyTimes:",
                          "setKeyPath:", "setType:", "setSubtype:", "setTimingFunctions:", "setAnimations:", "setCalculationMode:"})
        def(AN, s, +[](id, SEL, id) {});
    def(AN, "setRemovedOnCompletion:", +[](id, SEL, bool) {});
    def(AN, "setAutoreverses:", +[](id, SEL, bool) {});
    def(AN, "setCumulative:", +[](id, SEL, bool) {});
    def(AN, "setAdditive:", +[](id, SEL, bool) {});
    for (const char* n : {"CAPropertyAnimation", "CABasicAnimation", "CAKeyframeAnimation", "CATransition", "CAAnimationGroup", "CASpringAnimation"})
        def_class(n, std::strcmp(n, "CAPropertyAnimation") == 0 || std::strcmp(n, "CATransition") == 0 || std::strcmp(n, "CAAnimationGroup") == 0 ? "CAAnimation"
                     : std::strcmp(n, "CASpringAnimation") == 0 ? "CABasicAnimation" : "CAPropertyAnimation");
    defc(class_named("CAPropertyAnimation"), "animationWithKeyPath:", +[](id self, SEL, id) -> id { return autorelease(alloc(reinterpret_cast<Class>(self))); });
    Class TF = def_class("CAMediaTimingFunction", "NSObject");
    defc(TF, "functionWithName:", +[](id self, SEL, id) -> id { return autorelease(alloc(reinterpret_cast<Class>(self))); });
    defc(TF, "functionWithControlPoints::::", +[](id self, SEL, float, float, float, float) -> id { return autorelease(alloc(reinterpret_cast<Class>(self))); });
}

static void define_colors_fonts() {
    C_UIColor = def_class("UIColor", "NSObject");
    Class C = C_UIColor;
    defc(C, "colorWithRed:green:blue:alpha:", +[](id, SEL, double r, double g, double b, double a) -> id { return color(r, g, b, a); });
    defc(C, "colorWithWhite:alpha:", +[](id, SEL, double w, double a) -> id { return color(w, w, w, a); });
    defc(C, "colorWithHue:saturation:brightness:alpha:", +[](id, SEL, double h, double s, double v, double a) -> id {
        double r = v, g = v, b = v;
        if (s > 0) {
            double hh = std::fmod(h * 6, 6.0);
            int i = static_cast<int>(hh);
            double f = hh - i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
            switch (i) { case 0: r = v; g = t; b = p; break; case 1: r = q; g = v; b = p; break; case 2: r = p; g = v; b = t; break;
                         case 3: r = p; g = q; b = v; break; case 4: r = t; g = p; b = v; break; default: r = v; g = p; b = q; }
        }
        return color(r, g, b, a);
    });
    defc(C, "colorWithCGColor:", +[](id, SEL, id c) -> id { return c; });
    defc(C, "colorWithPatternImage:", +[](id, SEL, id img) -> id { id c = color(0.5, 0.5, 0.5, 1); ext_get<ColorExt>(c)->pattern = retain(img); return c; });
#define NAMED_COLOR(name, r, g, b, a) defc(C, name, +[](id, SEL) -> id { return color(r, g, b, a); });
    NAMED_COLOR("blackColor", 0, 0, 0, 1)
    NAMED_COLOR("whiteColor", 1, 1, 1, 1)
    NAMED_COLOR("clearColor", 0, 0, 0, 0)
    NAMED_COLOR("redColor", 1, 0, 0, 1)
    NAMED_COLOR("greenColor", 0, 1, 0, 1)
    NAMED_COLOR("blueColor", 0, 0, 1, 1)
    NAMED_COLOR("grayColor", 0.5, 0.5, 0.5, 1)
    NAMED_COLOR("lightGrayColor", 2.0 / 3, 2.0 / 3, 2.0 / 3, 1)
    NAMED_COLOR("darkGrayColor", 1.0 / 3, 1.0 / 3, 1.0 / 3, 1)
    NAMED_COLOR("yellowColor", 1, 1, 0, 1)
    NAMED_COLOR("orangeColor", 1, 0.5, 0, 1)
    NAMED_COLOR("purpleColor", 0.5, 0, 0.5, 1)
    NAMED_COLOR("brownColor", 0.6, 0.4, 0.2, 1)
    NAMED_COLOR("cyanColor", 0, 1, 1, 1)
    NAMED_COLOR("magentaColor", 1, 0, 1, 1)
    NAMED_COLOR("groupTableViewBackgroundColor", 0.94, 0.94, 0.96, 1)
    NAMED_COLOR("scrollViewTexturedBackgroundColor", 0.4, 0.4, 0.4, 1)
#undef NAMED_COLOR
    def(C, "initWithRed:green:blue:alpha:", +[](id self, SEL, double r, double g, double b, double a) -> id {
        auto* e = ext_get<ColorExt>(self); e->r = r; e->g = g; e->b = b; e->a = a; return self;
    });
    def(C, "initWithWhite:alpha:", +[](id self, SEL, double w, double a) -> id { auto* e = ext_get<ColorExt>(self); e->r = e->g = e->b = w; e->a = a; return self; });
    def(C, "CGColor", +[](id self, SEL) -> id { return self; });
    def(C, "colorWithAlphaComponent:", +[](id self, SEL, double a) -> id { auto* e = ext_get<ColorExt>(self); return color(e->r, e->g, e->b, a); });
    def(C, "getRed:green:blue:alpha:", +[](id self, SEL, double* r, double* g, double* b, double* a) -> bool {
        auto* e = ext_get<ColorExt>(self); if (r) *r = e->r; if (g) *g = e->g; if (b) *b = e->b; if (a) *a = e->a; return true;
    });
    def(C, "getWhite:alpha:", +[](id self, SEL, double* w, double* a) -> bool { auto* e = ext_get<ColorExt>(self); if (w) *w = (e->r + e->g + e->b) / 3; if (a) *a = e->a; return true; });
    def(C, "isEqual:", +[](id self, SEL, id o) -> bool {
        ColorExt* a = ext_as<ColorExt>(self); ColorExt* b = ext_as<ColorExt>(o);
        return self == o || (a && b && a->r == b->r && a->g == b->g && a->b == b->b && a->a == b->a);
    });
    def(C, "copyWithZone:", +[](id self, SEL, void*) -> id { return retain(self); });
    for (const char* s : {"set", "setFill", "setStroke"}) def(C, s, +[](id, SEL) {});

    C_UIFont = def_class("UIFont", "NSObject");
    Class F = C_UIFont;
    defc(F, "systemFontOfSize:", +[](id, SEL, double s) -> id { return font("Helvetica", s, false); });
    defc(F, "boldSystemFontOfSize:", +[](id, SEL, double s) -> id { return font("Helvetica-Bold", s, true); });
    defc(F, "italicSystemFontOfSize:", +[](id, SEL, double s) -> id { return font("Helvetica-Oblique", s, false); });
    defc(F, "systemFontOfSize:weight:", +[](id, SEL, double s, double w) -> id { return font("Helvetica", s, w > 0.2); });
    defc(F, "fontWithName:size:", +[](id, SEL, id n, double s) -> id { return font(utf8(n), s, utf8(n).find("Bold") != std::string::npos); });
    defc(F, "preferredFontForTextStyle:", +[](id, SEL, id) -> id { return font("Helvetica", 17, false); });
    defc(F, "systemFontSize", +[](id, SEL) -> double { return 14; });
    defc(F, "labelFontSize", +[](id, SEL) -> double { return 17; });
    defc(F, "buttonFontSize", +[](id, SEL) -> double { return 18; });
    defc(F, "smallSystemFontSize", +[](id, SEL) -> double { return 12; });
    defc(F, "familyNames", +[](id, SEL) -> id { return arr({str("Helvetica")}); });
    defc(F, "fontNamesForFamilyName:", +[](id, SEL, id) -> id { return arr({str("Helvetica"), str("Helvetica-Bold")}); });
    def(F, "pointSize", +[](id self, SEL) -> double { return ext_get<FontExt>(self)->size; });
    def(F, "fontName", +[](id self, SEL) -> id { return str(ext_get<FontExt>(self)->name); });
    def(F, "familyName", +[](id, SEL) -> id { return str("Helvetica"); });
    def(F, "lineHeight", +[](id self, SEL) -> double { return std::ceil(ext_get<FontExt>(self)->size * 1.2); });
    def(F, "ascender", +[](id self, SEL) -> double { return ext_get<FontExt>(self)->size * 0.95; });
    def(F, "descender", +[](id self, SEL) -> double { return -ext_get<FontExt>(self)->size * 0.25; });
    def(F, "capHeight", +[](id self, SEL) -> double { return ext_get<FontExt>(self)->size * 0.7; });
    def(F, "xHeight", +[](id self, SEL) -> double { return ext_get<FontExt>(self)->size * 0.5; });
    def(F, "leading", +[](id, SEL) -> double { return 0; });
    def(F, "fontWithSize:", +[](id self, SEL, double s) -> id { auto* e = ext_get<FontExt>(self); return font(e->name, s, e->bold); });
    def(F, "copyWithZone:", +[](id self, SEL, void*) -> id { return retain(self); });

    // NSString (UIStringDrawing): sizes are estimates; drawing is not rendered
    Class S = class_named("NSString");
    def(S, "sizeWithFont:", +[](id self, SEL, id f) -> CGSize { return text_size(self, f, 0); });
    def(S, "sizeWithFont:forWidth:lineBreakMode:", +[](id self, SEL, id f, double w, long) -> CGSize { CGSize s = text_size(self, f, 0); s.width = std::min(s.width, w); return s; });
    def(S, "sizeWithFont:constrainedToSize:", +[](id self, SEL, id f, CGSize max) -> CGSize { return text_size(self, f, max.width); });
    def(S, "sizeWithFont:constrainedToSize:lineBreakMode:", +[](id self, SEL, id f, CGSize max, long) -> CGSize { return text_size(self, f, max.width); });
    def(S, "sizeWithFont:minFontSize:actualFontSize:forWidth:lineBreakMode:", +[](id self, SEL, id f, double, double* actual, double w, long) -> CGSize {
        if (actual) *actual = font_size(f);
        CGSize s = text_size(self, f, 0); s.width = std::min(s.width, w); return s;
    });
    def(S, "sizeWithAttributes:", +[](id self, SEL, id attrs) -> CGSize { return text_size(self, dict_get(attrs, "NSFont"), 0); });
    def(S, "boundingRectWithSize:options:attributes:context:", +[](id self, SEL, CGSize max, NSUInteger, id attrs, id) -> CGRect {
        return {{0, 0}, text_size(self, dict_get(attrs, "NSFont"), max.width)};
    });
    def(S, "drawAtPoint:withFont:", +[](id self, SEL, CGPoint, id f) -> CGSize { return text_size(self, f, 0); });
    def(S, "drawAtPoint:forWidth:withFont:lineBreakMode:", +[](id self, SEL, CGPoint, double w, id f, long) -> CGSize { CGSize s = text_size(self, f, 0); s.width = std::min(s.width, w); return s; });
    def(S, "drawAtPoint:forWidth:withFont:fontSize:lineBreakMode:baselineAdjustment:", +[](id self, SEL, CGPoint, double w, id f, double, long, long) -> CGSize {
        CGSize s = text_size(self, f, 0); s.width = std::min(s.width, w); return s;
    });
    def(S, "drawInRect:withFont:", +[](id self, SEL, CGRect r, id f) -> CGSize { return text_size(self, f, r.size.width); });
    def(S, "drawInRect:withFont:lineBreakMode:", +[](id self, SEL, CGRect r, id f, long) -> CGSize { return text_size(self, f, r.size.width); });
    def(S, "drawInRect:withFont:lineBreakMode:alignment:", +[](id self, SEL, CGRect r, id f, long, long) -> CGSize { return text_size(self, f, r.size.width); });
    def(S, "drawAtPoint:withAttributes:", +[](id, SEL, CGPoint, id) {});
    def(S, "drawInRect:withAttributes:", +[](id, SEL, CGRect, id) {});
}

void init_uikit() {
    define_view_class();
    define_controls();
    define_window_vc_app();
    define_screen_device();
    define_touch_event();
    define_layers();
    define_colors_fonts();
    Class SB = def_class("UIStoryboard", "NSObject");
    struct SbExt : Ext { std::string name; };
    defc(SB, "storyboardWithName:bundle:", +[](id, SEL, id n, id) -> id { id s = alloc(class_named("UIStoryboard")); ext_get<SbExt>(s)->name = utf8(n); return autorelease(s); });
    def(SB, "instantiateInitialViewController", +[](id self, SEL) -> id { return nib_instantiate_storyboard_initial(ext_get<SbExt>(self)->name); });
    def(SB, "instantiateViewControllerWithIdentifier:", +[](id, SEL, id i) -> id { log("instantiateViewControllerWithIdentifier:%s not supported", utf8(i).c_str()); return nullptr; });
    Class DOC = def_class("UIDocument", "NSObject");
    struct DocExt : Ext { id url = nullptr; long state = 0; ~DocExt() override { release(url); } };
    def(DOC, "initWithFileURL:", +[](id self, SEL, id u) -> id { ext_get<DocExt>(self)->url = retain(u); return self; });
    def(DOC, "fileURL", +[](id self, SEL) -> id { return ext_get<DocExt>(self)->url; });
    def(DOC, "documentState", +[](id self, SEL) -> long { return ext_get<DocExt>(self)->state; });
    def(DOC, "openWithCompletionHandler:", +[](id self, SEL, id h) {
        id url = ext_get<DocExt>(self)->url;
        id data = send_cls(class_named("NSData"), "dataWithContentsOfURL:", url);
        bool ok = data && send<bool>(self, "loadFromContents:ofType:error:", data, static_cast<id>(nullptr), static_cast<id*>(nullptr));
        if (h) { id c = block_copy(h); post_main([c, ok] { reinterpret_cast<void (*)(id, bool)>(block_invoke_ptr(c))(c, ok); block_release(c); }); }
    });
    def(DOC, "saveToURL:forSaveOperation:completionHandler:", +[](id self, SEL, id u, long, id h) {
        id contents = send(self, "contentsForType:error:", static_cast<id>(nullptr), static_cast<id*>(nullptr));
        bool ok = contents && send<bool>(contents, "writeToURL:atomically:", u, 1);
        if (h) { id c = block_copy(h); post_main([c, ok] { reinterpret_cast<void (*)(id, bool)>(block_invoke_ptr(c))(c, ok); block_release(c); }); }
    });
    def(DOC, "closeWithCompletionHandler:", +[](id, SEL, id h) {
        if (h) { id c = block_copy(h); post_main([c] { reinterpret_cast<void (*)(id, bool)>(block_invoke_ptr(c))(c, true); block_release(c); }); }
    });
    def(DOC, "updateChangeCount:", +[](id self, SEL, long) {
        retain(self);
        post_main([self] {
            id url = ext_get<DocExt>(self)->url;
            id contents = send(self, "contentsForType:error:", static_cast<id>(nullptr), static_cast<id*>(nullptr));
            if (contents) send<bool>(contents, "writeToURL:atomically:", url, 1);
            release(self);
        }, 1.0);
    });
    def(DOC, "hasUnsavedChanges", +[](id, SEL) -> bool { return false; });
    def(DOC, "loadFromContents:ofType:error:", +[](id, SEL, id, id, id*) -> bool { return true; });
    def(DOC, "contentsForType:error:", +[](id, SEL, id, id*) -> id { return nullptr; });
    def(DOC, "handleError:userInteractionPermitted:", +[](id, SEL, id, bool) {});
    def(DOC, "fileType", +[](id, SEL) -> id { return nullptr; });
    def(DOC, "localizedName", +[](id self, SEL) -> id { return send(ext_get<DocExt>(self)->url, "lastPathComponent"); });
    Class PB = def_class("UIPasteboard", "NSObject");
    defc(PB, "generalPasteboard", +[](id self, SEL) -> id { static id pb = alloc(reinterpret_cast<Class>(self)); return pb; });
    defc(PB, "pasteboardWithName:create:", +[](id self, SEL, id, bool) -> id { static id pb = alloc(reinterpret_cast<Class>(self)); return pb; });
    def(PB, "string", +[](id, SEL) -> id { return nullptr; });
    def(PB, "setString:", +[](id, SEL, id) {});
    def(PB, "setPersistent:", +[](id, SEL, bool) {});
    def(PB, "dataForPasteboardType:", +[](id, SEL, id) -> id { return nullptr; });
    def(PB, "setData:forPasteboardType:", +[](id, SEL, id, id) {});
    def(PB, "items", +[](id, SEL) -> id { return arr({}); });
}

// ---------------------------------------------------------------------------------------------------------------
// C symbols
// ---------------------------------------------------------------------------------------------------------------
extern "C" double app_CACurrentMediaTime() { return now_s(); }

static id* new_const_string(const char* s) {
    id* slot = new id;
    *slot = str_new(s);
    return slot;
}

void register_uikit_symbols(std::unordered_map<std::string, void*>& m) {
    m["_UIApplicationMain"] = reinterpret_cast<void*>(&app_UIApplicationMain);
    m["_CACurrentMediaTime"] = reinterpret_cast<void*>(&app_CACurrentMediaTime);
    // notification names and other NSString constants exported by UIKit/QuartzCore
    for (const char* n : {"UIApplicationDidBecomeActiveNotification", "UIApplicationDidChangeStatusBarOrientationNotification",
                          "UIApplicationDidEnterBackgroundNotification", "UIApplicationDidReceiveMemoryWarningNotification",
                          "UIApplicationWillEnterForegroundNotification", "UIApplicationWillResignActiveNotification", "UIApplicationWillTerminateNotification",
                          "UIApplicationDidFinishLaunchingNotification", "UIDeviceOrientationDidChangeNotification", "UIDocumentStateChangedNotification",
                          "UITextFieldTextDidChangeNotification", "UIKeyboardWillShowNotification", "UIKeyboardWillHideNotification",
                          "UIKeyboardDidShowNotification", "UIKeyboardDidHideNotification", "UIKeyboardFrameEndUserInfoKey", "UIKeyboardFrameBeginUserInfoKey",
                          "UIApplicationLaunchOptionsURLKey", "UIApplicationLaunchOptionsRemoteNotificationKey", "UIApplicationLaunchOptionsLocalNotificationKey",
                          "UIApplicationLaunchOptionsSourceApplicationKey", "UIScreenDidConnectNotification", "UIScreenDidDisconnectNotification",
                          "UIWindowDidBecomeKeyNotification", "UIApplicationSignificantTimeChangeNotification", "UIContentSizeCategoryDidChangeNotification",
                          "UIApplicationOpenSettingsURLString", "UITrackingRunLoopMode", "UIApplicationStatusBarOrientationUserInfoKey"})
        m[std::string("_") + n] = new_const_string(n);
    m["_NSFontAttributeName"] = new_const_string("NSFont");
    m["_NSForegroundColorAttributeName"] = new_const_string("NSColor");
    m["_NSParagraphStyleAttributeName"] = new_const_string("NSParagraphStyle");
    m["_NSShadowAttributeName"] = new_const_string("NSShadow");
    m["_NSBackgroundColorAttributeName"] = new_const_string("NSBackgroundColor");
    m["_NSKernAttributeName"] = new_const_string("NSKern");
    m["_NSStrokeColorAttributeName"] = new_const_string("NSStrokeColor");
    m["_NSStrokeWidthAttributeName"] = new_const_string("NSStrokeWidth");
    for (const char* n : {"kCAFillModeForwards", "kCAFillModeBoth", "kCAFillModeRemoved", "kCAFillModeBackwards", "kCAMediaTimingFunctionLinear",
                          "kCAMediaTimingFunctionEaseIn", "kCAMediaTimingFunctionEaseOut", "kCAMediaTimingFunctionEaseInEaseOut", "kCAMediaTimingFunctionDefault",
                          "kCATransitionFade", "kCATransitionPush", "kCATransitionMoveIn", "kCATransitionReveal", "kCATransitionFromLeft", "kCATransitionFromRight",
                          "kCATransitionFromTop", "kCATransitionFromBottom", "kCAAnimationLinear", "kCAAnimationDiscrete", "kCAGravityResizeAspect",
                          "kCAGravityResizeAspectFill", "kCAGravityResize", "kCAGravityCenter", "kCAFilterLinear", "kCAFilterNearest"})
        m[std::string("_") + n] = new_const_string(n);
    static uint64_t bg_task_invalid = 0;
    m["_UIBackgroundTaskInvalid"] = &bg_task_invalid;
    static double long_press = 0.5;
    m["_UIWindowLevelNormal"] = new double(0);
    m["_UIWindowLevelAlert"] = new double(2000);
    m["_UIWindowLevelStatusBar"] = new double(1000);
    (void)long_press;
    static CGRect zero_rect{{0, 0}, {0, 0}};
    m["_UIEdgeInsetsZero"] = &zero_rect;
}

}  // namespace app
