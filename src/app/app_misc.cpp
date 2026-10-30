// App mode: the rest of the frameworks an iOS 5/6 game links but a single-player Android port does not need to be
// online for — CoreFoundation toll-free bridging, the keychain (backed by a sandbox plist), SystemConfiguration
// reachability (always "reachable"), CoreMotion/accelerometer (fed by Android sensors when present), and inert
// GameKit / StoreKit / iAd / analytics classes so the game's own calls succeed without a network.
#include "app.hpp"

#include <android/sensor.h>
#include <android/looper.h>

#include <algorithm>
#include <map>
#include <set>

namespace app {

void post_note(const char* name, id object, id info);

namespace {

// ---------------------------------------------------------------------------------------------------------------
// CoreFoundation (toll-free bridged to the Foundation objects)
// ---------------------------------------------------------------------------------------------------------------
id g_cfbundle;   // the main bundle, reused as the CFBundle handle

}  // namespace

}  // namespace app

using namespace app;

extern "C" {

// memory
static id cf_retain(id o) { return retain(o); }
static void cf_release(id o) { release(o); }
static long cf_retain_count(id o) { return 1; }
static id cf_autorelease(id o) { return autorelease(o); }
static unsigned long cf_hash(id o) { return obj_hash(o); }
static bool cf_equal(id a, id b) { return a == b || obj_equal(a, b); }
static id cf_copy_description(id o) { return o ? retain(send(o, "description")) : str_new("(null)"); }
static long cf_get_type_id(id) { return 1; }

// CFString
static long cf_string_get_length(id s) { return static_cast<long>(u16(s).size()); }
static uint16_t cf_string_get_char(id s, long i) { const auto& u = u16(s); return i >= 0 && static_cast<size_t>(i) < u.size() ? u[i] : 0; }
static const char* cf_string_get_cstr_ptr(id s, uint32_t) { return is_string(s) ? cstr(s) : nullptr; }
static bool cf_string_get_cstr(id s, char* buf, long size, uint32_t) {
    if (!is_string(s)) return false;
    std::string u = utf8(s);
    if (static_cast<long>(u.size()) + 1 > size) return false;
    std::memcpy(buf, u.c_str(), u.size() + 1);
    return true;
}
static long cf_string_get_bytes(id s, NSRange r, uint32_t, uint8_t, bool, uint8_t* buf, long max, long* used) {
    std::string u = utf8(s);
    long n = std::min<long>(max, static_cast<long>(u.size()));
    if (buf) std::memcpy(buf, u.data(), n);
    if (used) *used = n;
    return n;
}
static id cf_string_create_with_cstr(void*, const char* c, uint32_t) { return c ? str_new(c) : nullptr; }
static id cf_string_create_with_bytes(void*, const uint8_t* b, long n, uint32_t enc, bool) {
    if (enc == 0x08000100 || enc == 0x0600) return str_new(std::string(reinterpret_cast<const char*>(b), n));
    return str_new(std::string(reinterpret_cast<const char*>(b), n));
}
static id cf_string_create_copy(void*, id s) { return s ? str16_new(u16(s)) : nullptr; }
static id cf_string_create_mutable(void*, long) { return str16_new(u"", true); }
static void cf_string_append(id s, id a) { if (is_string(s)) send<void>(s, "appendString:", a); }
static long cf_string_compare(id a, id b, uint32_t opts) { return send<long>(a, "compare:options:", b, static_cast<NSUInteger>(opts)); }

// CFDictionary / CFArray (bridged)
static id cf_dict_get_value(id d, id k) { return dict_get(d, k); }
static bool cf_dict_get_value_if(id d, id k, const void** out) { id v = dict_get(d, k); if (out) *out = v; return v != nullptr; }
static long cf_dict_get_count(id d) { return static_cast<long>(send<NSUInteger>(d, "count")); }
static id cf_dict_create_mutable(void*, long, const void*, const void*) { return dict_new(true); }
static id cf_dict_create(void*, const void** keys, const void** vals, long n, const void*, const void*) {
    id d = dict_new(true);
    for (long i = 0; i < n; ++i) dict_set(d, reinterpret_cast<id>(const_cast<void*>(keys[i])), reinterpret_cast<id>(const_cast<void*>(vals[i])));
    return d;
}
static void cf_dict_set_value(id d, id k, id v) { if (is_dict(d)) dict_set(d, k, v); }
static void cf_dict_add_value(id d, id k, id v) { if (is_dict(d) && !dict_get(d, k)) dict_set(d, k, v); }
static void cf_dict_remove_value(id d, id k) { if (is_dict(d)) send<void>(d, "removeObjectForKey:", k); }
static bool cf_dict_contains(id d, id k) { return dict_get(d, k) != nullptr; }
static id cf_array_create_mutable(void*, long, const void*) { return arr_new({}, true); }
static id cf_array_create(void*, const void** vals, long n, const void*) {
    std::vector<id> v;
    for (long i = 0; i < n; ++i) v.push_back(reinterpret_cast<id>(const_cast<void*>(vals[i])));
    return arr_new(v, true);
}
static long cf_array_get_count(id a) { return static_cast<long>(arr_items(a).size()); }
static id cf_array_get_value(id a, long i) { const auto& v = arr_items(a); return i >= 0 && static_cast<size_t>(i) < v.size() ? v[i] : nullptr; }
static void cf_array_append(id a, id v) { arr_add(a, v); }
static bool cf_array_contains(id a, NSRange, id v) { return send<bool>(a, "containsObject:", v); }

// CFData
static const uint8_t* cf_data_get_ptr(id d) { return data_bytes(d); }
static long cf_data_get_length(id d) { return static_cast<long>(data_len(d)); }
static id cf_data_create(void*, const uint8_t* b, long n) { return data_new(b, n); }

// CFNumber
static id cf_number_create(void*, int type, const void* v) {
    switch (type) {
        case 1: case 2: case 3: case 9: return retain(num_i(*static_cast<const int*>(v)));
        case 4: case 10: return retain(num_i(*static_cast<const long long*>(v)));
        case 5: case 12: return retain(num_f(*static_cast<const float*>(v)));
        case 6: case 13: return retain(num_d(*static_cast<const double*>(v)));
        default: return retain(num_i(*static_cast<const long*>(v)));
    }
}
static bool cf_number_get_value(id n, int type, void* out) {
    switch (type) {
        case 1: case 2: case 3: case 9: *static_cast<int*>(out) = static_cast<int>(num_ival(n)); break;
        case 4: case 10: *static_cast<long long*>(out) = num_ival(n); break;
        case 5: case 12: *static_cast<float*>(out) = static_cast<float>(num_dval(n)); break;
        case 6: case 13: *static_cast<double*>(out) = num_dval(n); break;
        default: *static_cast<long*>(out) = static_cast<long>(num_ival(n)); break;
    }
    return true;
}
static bool cf_number_is_float(id n) { const char* t = is_number(n) ? send<const char*>(n, "objCType") : "q"; return t && (t[0] == 'f' || t[0] == 'd'); }

// CFBundle
static id cf_bundle_get_main() { return g_cfbundle; }
static id cf_bundle_get_info_value(id, id key) { return dict_get(info_dictionary(), key); }
static id cf_bundle_copy_bundle_url(id) { return retain(send_cls(class_named("NSURL"), "fileURLWithPath:", str(bundle_dir()))); }
static id cf_bundle_copy_resources_url(id) { return retain(send_cls(class_named("NSURL"), "fileURLWithPath:", str(bundle_dir()))); }
static id cf_bundle_copy_resource_url(id, id name, id type, id) {
    id p = send(send_cls(class_named("NSBundle"), "mainBundle"), "pathForResource:ofType:", name, type);
    return p ? retain(send_cls(class_named("NSURL"), "fileURLWithPath:", p)) : nullptr;
}
static id cf_bundle_get_identifier(id) { return dict_get(info_dictionary(), "CFBundleIdentifier"); }

// CFURL
static id cf_url_create_with_string(void*, id s, id) { return s ? retain(send_cls(class_named("NSURL"), "URLWithString:", s)) : nullptr; }
static id cf_url_create_with_path(void*, id path, int, bool) { return retain(send_cls(class_named("NSURL"), "fileURLWithPath:", path)); }
static id cf_url_create_from_filesystem(void*, const uint8_t* p, long n, bool) { return retain(send_cls(class_named("NSURL"), "fileURLWithPath:", str(std::string(reinterpret_cast<const char*>(p), n)))); }
static id cf_url_copy_path(id u) { return retain(send(u, "path")); }
static bool cf_url_get_fs_rep(id u, bool, uint8_t* buf, long max) {
    std::string p = utf8(send(u, "path"));
    if (static_cast<long>(p.size()) + 1 > max) return false;
    std::memcpy(buf, p.c_str(), p.size() + 1);
    return true;
}
static id cf_url_create_string_by_adding_escapes(void*, id s, id, id, uint32_t) { return retain(send(s, "stringByAddingPercentEscapesUsingEncoding:", 4)); }

// CFRunLoop
static id cf_runloop_get_current() { return send_cls(class_named("NSRunLoop"), "currentRunLoop"); }
static id cf_runloop_get_main() { return send_cls(class_named("NSRunLoop"), "mainRunLoop"); }
static void cf_runloop_run() { send<void>(cf_runloop_get_current(), "run"); }
static int cf_runloop_run_in_mode(id, double seconds, bool) {
    id until = send_cls(class_named("NSDate"), "dateWithTimeIntervalSinceNow:", seconds);
    send<bool>(cf_runloop_get_current(), "runMode:beforeDate:", str("kCFRunLoopDefaultMode"), until);
    return 2;
}
static void cf_runloop_add_timer(id, id, id) {}
static void cf_runloop_stop(id) {}
static void cf_runloop_wakeup(id) {}
static void cf_noop_vp(void*) {}
static void cf_noop_vpvp(void*, const void*) {}

// CFPreferences -> NSUserDefaults
static id cf_prefs_copy(id key, id) { return retain(send(send_cls(class_named("NSUserDefaults"), "standardUserDefaults"), "objectForKey:", key)); }
static void cf_prefs_set(id key, id value, id) { send<void>(send_cls(class_named("NSUserDefaults"), "standardUserDefaults"), "setObject:forKey:", value, key); }
static bool cf_prefs_sync(id) { return send<bool>(send_cls(class_named("NSUserDefaults"), "standardUserDefaults"), "synchronize"); }

// CFLocale / timing
static id cf_locale_copy_current() { return retain(send_cls(class_named("NSLocale"), "currentLocale")); }
static id cf_locale_get_value(id, id) { return nullptr; }
static double cf_absolute_time() { return send<double>(send_cls(class_named("NSDate"), "self"), "timeIntervalSinceReferenceDate"); }
static id cf_uuid_create(void*) { return retain(send_cls(class_named("NSUUID"), "UUID")); }
static id cf_uuid_create_string(void*, id u) { return u ? retain(send(u, "UUIDString")) : nullptr; }
static id cf_timezone_copy_system() { return retain(send_cls(class_named("NSTimeZone"), "systemTimeZone")); }

// CFStream bound to a socket: unused (no network), report failure
static void cf_stream_pair(void*, id, void**, void**) {}
static bool cf_stream_open(void*) { return false; }
static long cf_stream_status(void*) { return 4; }   // errorOccurred
static void cf_stream_set_prop(void*, id, id) {}

}  // extern "C"

// ---------------------------------------------------------------------------------------------------------------
// keychain (Security.framework) backed by a sandbox plist
// ---------------------------------------------------------------------------------------------------------------
namespace {
std::mutex g_keychain_mu;
id g_keychain;   // array of item dictionaries
std::string keychain_path() { return sandbox_dir("Library") + "/keychain.plist"; }
void keychain_load() {
    if (g_keychain) return;
    std::string data;
    if (read_file(keychain_path(), data)) g_keychain = plist_parse(reinterpret_cast<const uint8_t*>(data.data()), data.size(), true);
    if (!is_array(g_keychain)) { release(g_keychain); g_keychain = arr_new({}, true); }
}
void keychain_save() {
    std::string out;
    if (!plist_write_binary(g_keychain, out)) return;
    FILE* f = fopen(keychain_path().c_str(), "wb");
    if (!f) return;
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
}
bool item_matches(id item, id query) {
    for (auto& kv : dict_entries(query)) {
        // match class and attributes (account, service, generic, ...); skip return/match/value options and the
        // accessibility attribute, which do not identify an item
        std::string k = utf8(kv.first);
        if (k.empty() || k.compare(0, 2, "r_") == 0 || k.compare(0, 2, "m_") == 0 || k.compare(0, 2, "v_") == 0) continue;
        if (k == "accc" || k == "pdmn") continue;
        id have = dict_get(item, kv.first);
        if (!have || !obj_equal(have, kv.second)) return false;
    }
    return true;
}
}

extern "C" {
static int sec_item_copy(id query, id* result) {
    std::lock_guard<std::mutex> l(g_keychain_mu);
    keychain_load();
    std::vector<id> hits;
    for (id item : arr_items(g_keychain)) if (item_matches(item, query)) hits.push_back(item);
    if (hits.empty()) { if (result) *result = nullptr; return -25300; }   // errSecItemNotFound
    bool want_data = dict_get(query, "r_Data") != nullptr;
    bool want_attrs = dict_get(query, "r_Attributes") != nullptr;
    id limit = dict_get(query, "m_Limit");
    bool one = !limit || utf8(limit) == "m_LimitOne";
    auto item_result = [&](id item) -> id {
        if (want_attrs) return item;   // the attribute dictionary (it carries v_Data too)
        id d = dict_get(item, "v_Data");
        return d ? d : item;
    };
    if (result) {
        if (one) *result = retain(item_result(hits[0]));
        else { std::vector<id> v; for (id h : hits) v.push_back(item_result(h)); *result = retain(arr(v)); }
    }
    return 0;
}
static int sec_item_add(id attrs, id* result) {
    std::lock_guard<std::mutex> l(g_keychain_mu);
    keychain_load();
    for (id item : arr_items(g_keychain)) if (item_matches(item, attrs)) return -25299;   // errSecDuplicateItem
    id item = send(attrs, "mutableCopy");
    arr_add(g_keychain, item);
    release(item);
    keychain_save();
    if (result) *result = nullptr;
    return 0;
}
static int sec_item_update(id query, id changes) {
    std::lock_guard<std::mutex> l(g_keychain_mu);
    keychain_load();
    bool any = false;
    for (id item : arr_items(g_keychain))
        if (item_matches(item, query)) { for (auto& kv : dict_entries(changes)) dict_set(item, kv.first, kv.second); any = true; }
    if (!any) return -25300;
    keychain_save();
    return 0;
}
static int sec_item_delete(id query) {
    std::lock_guard<std::mutex> l(g_keychain_mu);
    keychain_load();
    std::vector<id> keep;
    bool any = false;
    for (id item : arr_items(g_keychain)) { if (item_matches(item, query)) any = true; else keep.push_back(item); }
    if (!any) return -25300;
    send<void>(g_keychain, "setArray:", arr(keep));
    keychain_save();
    return 0;
}
static int sec_zero() { return 0; }
static int sec_trust_evaluate(void*, uint32_t* result) { if (result) *result = 4; return 0; }   // kSecTrustResultProceed
static int sec_random(void*, size_t n, void* bytes) { auto* p = static_cast<uint8_t*>(bytes); for (size_t i = 0; i < n; ++i) p[i] = static_cast<uint8_t>(arc4random()); return 0; }

// SystemConfiguration reachability: always reachable
static void* scr_create_with_name(void*, const char*) { return reinterpret_cast<void*>(0x5c); }
static void* scr_create_with_address(void*, const void*) { return reinterpret_cast<void*>(0x5c); }
static bool scr_get_flags(void*, uint32_t* flags) { if (flags) *flags = 2; return true; }   // kSCNetworkReachabilityFlagsReachable
static bool scr_set_callback(void*, void*, void*) { return true; }
static bool scr_schedule(void*, void*, const void*) { return true; }
static bool scr_unschedule(void*, void*, const void*) { return true; }
static bool scr_set_dispatch(void*, void*) { return true; }

}  // extern "C"

namespace app {

// ---------------------------------------------------------------------------------------------------------------
// CoreMotion (fed from Android sensors when available, otherwise zero)
// ---------------------------------------------------------------------------------------------------------------
bool screen_landscape();                                                    // app_uikit.cpp
extern "C" int host_display_rotation() __attribute__((weak));               // android_app.cpp (APK only)
namespace {
ASensorManager* g_sensor_mgr = nullptr;
const ASensor* g_accel = nullptr;
ASensorEventQueue* g_queue = nullptr;
double g_accel_x = 0, g_accel_y = 0, g_accel_z = -1;   // guest device frame, g, gravity included
double g_grav_x = 0, g_grav_y = 0, g_grav_z = -1;      // low-passed: CMDeviceMotion.gravity
std::mutex g_accel_mu;

// Android reports the reaction to gravity (flat on a table: z = +9.8 m/s^2) along the axes of the display's natural
// orientation; iOS reports gravity itself in g (flat: z = -1) along the device's portrait axes. The reading is turned
// into current-screen axes with the display rotation, then into the device axes implied by the interface orientation
// the guest is told (LandscapeRight, home button on the right, or portrait).
void to_guest_frame(const ASensorVector& a, double& x, double& y, double& z) {
    const bool land = screen_landscape();
    int r = host_display_rotation ? host_display_rotation() : -1;
    if (r < 0) r = land ? 1 : 0;   // unknown (adb shell): a portrait-natural panel turned to the guest's orientation
    double sx, sy;
    switch (r & 3) {
        case 0: sx = a.x; sy = a.y; break;
        case 1: sx = -a.y; sy = a.x; break;
        case 2: sx = -a.x; sy = -a.y; break;
        default: sx = a.y; sy = -a.x; break;
    }
    constexpr double g = 9.80665;
    if (land) { x = -sy / g; y = sx / g; }   // LandscapeRight: device +x points up the screen, +y to its left
    else { x = -sx / g; y = -sy / g; }
    z = -a.z / g;
}

void sensor_poll() {
    if (!g_queue) return;
    ASensorEvent e;
    std::lock_guard<std::mutex> l(g_accel_mu);
    while (ASensorEventQueue_getEvents(g_queue, &e, 1) > 0) {
        to_guest_frame(e.acceleration, g_accel_x, g_accel_y, g_accel_z);
        static bool logged = false;
        if (!logged) {
            logged = true;
            log("motion: rotation %d, android (%.2f %.2f %.2f) m/s^2 -> guest (%.2f %.2f %.2f) g",
                host_display_rotation ? host_display_rotation() : -1, e.acceleration.x, e.acceleration.y, e.acceleration.z,
                g_accel_x, g_accel_y, g_accel_z);
        }
        g_grav_x += 0.1 * (g_accel_x - g_grav_x);
        g_grav_y += 0.1 * (g_accel_y - g_grav_y);
        g_grav_z += 0.1 * (g_accel_z - g_grav_z);
    }
}
bool sensor_start(double hz) {
    if (!g_sensor_mgr) {
#if __ANDROID_API__ >= 26
        g_sensor_mgr = ASensorManager_getInstanceForPackage("com.xnuroid.loader");
#else
        g_sensor_mgr = ASensorManager_getInstance();
#endif
        if (!g_sensor_mgr) return false;
        g_accel = ASensorManager_getDefaultSensor(g_sensor_mgr, ASENSOR_TYPE_ACCELEROMETER);
        g_queue = ASensorManager_createEventQueue(g_sensor_mgr, ALooper_forThread() ? ALooper_forThread() : ALooper_prepare(0), 3, nullptr, nullptr);
    }
    if (!g_accel || !g_queue) return false;
    ASensorEventQueue_enableSensor(g_queue, g_accel);
    int us = hz > 0 ? static_cast<int>(1e6 / hz) : 16666;
    ASensorEventQueue_setEventRate(g_queue, g_accel, us);
    return true;
}
}  // namespace

struct AccelDataExt : app::Ext { double x = 0, y = 0, z = -1, gx = 0, gy = 0, gz = -1; };
static void init_coremotion() {
    Class MM = def_class("CMMotionManager", "NSObject");
    struct MotExt : Ext { bool accel = false, device = false; double interval = 1.0 / 60; uint64_t timer = 0; };
    def(MM, "setAccelerometerUpdateInterval:", +[](id self, SEL, double i) { ext_get<MotExt>(self)->interval = i; });
    def(MM, "accelerometerUpdateInterval", +[](id self, SEL) -> double { return ext_get<MotExt>(self)->interval; });
    def(MM, "setDeviceMotionUpdateInterval:", +[](id self, SEL, double i) { ext_get<MotExt>(self)->interval = i; });
    def(MM, "setGyroUpdateInterval:", +[](id, SEL, double) {});
    def(MM, "isAccelerometerAvailable", +[](id, SEL) -> bool { return true; });
    def(MM, "isDeviceMotionAvailable", +[](id, SEL) -> bool { return true; });
    def(MM, "isGyroAvailable", +[](id, SEL) -> bool { return false; });
    def(MM, "isMagnetometerAvailable", +[](id, SEL) -> bool { return false; });
    def(MM, "isAccelerometerActive", +[](id self, SEL) -> bool { return ext_get<MotExt>(self)->accel; });
    def(MM, "isDeviceMotionActive", +[](id self, SEL) -> bool { return ext_get<MotExt>(self)->device; });
    def(MM, "startAccelerometerUpdates", +[](id self, SEL) { ext_get<MotExt>(self)->accel = sensor_start(1 / ext_get<MotExt>(self)->interval); });
    def(MM, "startDeviceMotionUpdates", +[](id self, SEL) { ext_get<MotExt>(self)->device = sensor_start(1 / ext_get<MotExt>(self)->interval); });
    def(MM, "stopAccelerometerUpdates", +[](id self, SEL) {
        ext_get<MotExt>(self)->accel = false;
        std::lock_guard<std::mutex> l(g_accel_mu);
        if (g_queue && g_accel) ASensorEventQueue_disableSensor(g_queue, g_accel);
    });
    def(MM, "stopDeviceMotionUpdates", +[](id self, SEL) { ext_get<MotExt>(self)->device = false; });
    def(MM, "stopGyroUpdates", +[](id, SEL) {});
    auto read = [](id mgr, const char* which) -> id {
        sensor_poll();
        std::lock_guard<std::mutex> l(g_accel_mu);
        Class dc = class_named(std::strcmp(which, "accel") == 0 ? "CMAccelerometerData" : "CMDeviceMotion");
        id d = autorelease(alloc(dc));
        auto* e = ext_get<AccelDataExt>(d);
        e->x = g_accel_x; e->y = g_accel_y; e->z = g_accel_z;
        e->gx = g_grav_x; e->gy = g_grav_y; e->gz = g_grav_z;
        (void)mgr;
        return d;
    };
    static decltype(read) s_read = read;
    def(MM, "accelerometerData", +[](id self, SEL) -> id { return ext_get<MotExt>(self)->accel ? s_read(self, "accel") : nullptr; });
    def(MM, "deviceMotion", +[](id self, SEL) -> id { return ext_get<MotExt>(self)->device ? s_read(self, "motion") : nullptr; });
    def(MM, "setShowsDeviceMovementDisplay:", +[](id, SEL, bool) {});
    def(MM, "startAccelerometerUpdatesToQueue:withHandler:", +[](id self, SEL, id, id) { ext_get<MotExt>(self)->accel = sensor_start(1 / ext_get<MotExt>(self)->interval); });

    struct Accel3 { double x, y, z; };
    Class AD = def_class("CMAccelerometerData", "NSObject");
    def(AD, "acceleration", +[](id self, SEL) -> Accel3 { auto* e = ext_get<AccelDataExt>(self); return {e->x, e->y, e->z}; });
    def(AD, "timestamp", +[](id, SEL) -> double { return now_s(); });
    Class DM = def_class("CMDeviceMotion", "NSObject");
    def(DM, "userAcceleration", +[](id self, SEL) -> Accel3 { auto* e = ext_get<AccelDataExt>(self); return {e->x - e->gx, e->y - e->gy, e->z - e->gz}; });
    def(DM, "gravity", +[](id self, SEL) -> Accel3 { auto* e = ext_get<AccelDataExt>(self); return {e->gx, e->gy, e->gz}; });
    def(DM, "rotationRate", +[](id, SEL) -> Accel3 { return {0, 0, 0}; });
}

// ---------------------------------------------------------------------------------------------------------------
// GameKit / StoreKit / iAd / analytics: present and inert (no accounts, no network, no purchases)
// ---------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------
// StoreKit. There is no App Store on Android, so by default nothing can be bought (canMakePayments is NO).
// LOADER_APP_OFFLINE_STORE=1 replaces it with a local store: product requests answer with every identifier the
// app asks for, and payments complete immediately (owned products persist in the sandbox and come back on
// restore). LOADER_APP_OFFLINE_STORE=own_all also delivers every requested product as purchased, so an app whose
// store has gone away (a delisted game) gets the content it already ships through its own purchase handling.
// ---------------------------------------------------------------------------------------------------------------
namespace {
int store_mode() {
    const char* e = getenv("LOADER_APP_OFFLINE_STORE");
    if (!e || !*e || !std::strcmp(e, "0")) return 0;
    return std::strcmp(e, "own_all") == 0 ? 2 : 1;
}
struct ProductExt : Ext { id ident = nullptr; ~ProductExt() override { release(ident); } };
struct ResponseExt : Ext { id products = nullptr, invalid = nullptr; ~ResponseExt() override { release(products); release(invalid); } };
struct RequestExt : Ext { id ids = nullptr, delegate = nullptr; ~RequestExt() override { release(ids); } };
struct PaymentExt : Ext { id ident = nullptr; long qty = 1; ~PaymentExt() override { release(ident); } };
struct TxExt : Ext { id payment = nullptr, ident = nullptr, date = nullptr, original = nullptr; long state = 0; ~TxExt() override { release(payment); release(ident); release(date); release(original); } };

std::mutex g_store_mu;
std::vector<id> g_store_observers;            // not retained, like SKPaymentQueue
std::set<std::string> g_store_granted;         // own_all deliveries made this session
id g_store_queue;

std::string owned_path() { return sandbox_dir("Library") + "/xnuroid_store.plist"; }
std::set<std::string> owned_load() {
    std::set<std::string> s;
    std::string data;
    if (read_file(owned_path(), data)) {
        id a = plist_parse(reinterpret_cast<const uint8_t*>(data.data()), data.size(), false);
        for (id x : arr_items(a)) s.insert(utf8(x));
        release(a);
    }
    return s;
}
void owned_add(const std::string& ident) {
    std::set<std::string> s = owned_load();
    if (!s.insert(ident).second) return;
    std::vector<id> v;
    for (auto& x : s) v.push_back(str(x));
    std::string out;
    if (plist_write_xml(arr(v), out)) {
        if (FILE* f = fopen(owned_path().c_str(), "wb")) { fwrite(out.data(), 1, out.size(), f); fclose(f); }
    }
}
id make_payment(id ident) {
    id p = autorelease(alloc(class_named("SKPayment")));
    ext_get<PaymentExt>(p)->ident = retain(ident);
    return p;
}
id make_tx(id payment, long state, id original) {
    id t = autorelease(alloc(class_named("SKPaymentTransaction")));
    auto* e = ext_get<TxExt>(t);
    e->payment = retain(payment);
    e->state = state;
    e->ident = retain(send(send_cls(class_named("NSUUID"), "UUID"), "UUIDString"));
    e->date = retain(send_cls(class_named("NSDate"), "date"));
    e->original = retain(original);
    return t;
}
void deliver(const std::vector<id>& txs) {
    if (txs.empty()) return;
    std::vector<id> obs;
    { std::lock_guard<std::mutex> l(g_store_mu); obs = g_store_observers; }
    id list = arr(txs);
    for (id o : obs)
        if (responds(o, sel("paymentQueue:updatedTransactions:"))) send<void>(o, "paymentQueue:updatedTransactions:", g_store_queue, list);
}
}  // namespace

static void init_storekit() {
    Class PQ = def_class("SKPaymentQueue", "NSObject");
    g_store_queue = alloc(PQ);
    defc(PQ, "defaultQueue", +[](id, SEL) -> id { return g_store_queue; });
    defc(PQ, "canMakePayments", +[](id, SEL) -> bool { return store_mode() != 0; });
    def(PQ, "addTransactionObserver:", +[](id, SEL, id o) {
        std::lock_guard<std::mutex> l(g_store_mu);
        if (o && std::find(g_store_observers.begin(), g_store_observers.end(), o) == g_store_observers.end()) g_store_observers.push_back(o);
    });
    def(PQ, "removeTransactionObserver:", +[](id, SEL, id o) {
        std::lock_guard<std::mutex> l(g_store_mu);
        g_store_observers.erase(std::remove(g_store_observers.begin(), g_store_observers.end(), o), g_store_observers.end());
    });
    def(PQ, "transactions", +[](id, SEL) -> id { return arr({}); });
    def(PQ, "finishTransaction:", +[](id, SEL, id) {});
    def(PQ, "addPayment:", +[](id, SEL, id payment) {
        if (!payment) return;
        std::string ident = utf8(ext_get<PaymentExt>(payment)->ident);
        log("StoreKit: purchase %s (%s)", ident.c_str(), store_mode() ? "offline store: granted" : "no store: failed");
        retain(payment);
        post_main([payment, ident] {
            long state = store_mode() ? 1 : 2;   // Purchased / Failed
            if (state == 1) owned_add(ident);
            deliver({make_tx(payment, state, nullptr)});
            release(payment);
        }, 0.3);
    });
    def(PQ, "restoreCompletedTransactions", +[](id, SEL) {
        post_main([] {
            std::vector<id> txs;
            if (store_mode())
                for (auto& ident : owned_load()) {
                    id p = make_payment(str(ident));
                    txs.push_back(make_tx(p, 3, make_tx(p, 1, nullptr)));   // Restored, with its original purchase
                }
            log("StoreKit: restore -> %zu transactions", txs.size());
            deliver(txs);
            std::vector<id> obs;
            { std::lock_guard<std::mutex> l(g_store_mu); obs = g_store_observers; }
            for (id o : obs)
                if (responds(o, sel("paymentQueueRestoreCompletedTransactionsFinished:"))) send<void>(o, "paymentQueueRestoreCompletedTransactionsFinished:", g_store_queue);
        }, 0.3);
    });

    Class PR = def_class("SKRequest", "NSObject");
    def(PR, "setDelegate:", +[](id self, SEL, id d) { ext_get<RequestExt>(self)->delegate = d; });
    def(PR, "delegate", +[](id self, SEL) -> id { return ext_get<RequestExt>(self)->delegate; });
    def(PR, "cancel", +[](id self, SEL) { ext_get<RequestExt>(self)->delegate = nullptr; });
    Class PRQ = def_class("SKProductsRequest", "SKRequest");
    def(PRQ, "initWithProductIdentifiers:", +[](id self, SEL, id ids) -> id { ext_get<RequestExt>(self)->ids = send(ids, "copy"); return self; });
    def(PRQ, "start", +[](id self, SEL) {
        retain(self);
        post_main([self] {
            auto* e = ext_get<RequestExt>(self);
            id d = e->delegate;
            std::vector<id> products, invalid;
            std::vector<std::string> grant;
            for (id ident : arr_items(send(e->ids, "allObjects"))) {
                if (!store_mode()) { invalid.push_back(ident); continue; }
                id p = autorelease(alloc(class_named("SKProduct")));
                ext_get<ProductExt>(p)->ident = retain(ident);
                products.push_back(p);
                std::lock_guard<std::mutex> l(g_store_mu);
                if (store_mode() == 2 && g_store_granted.insert(utf8(ident)).second) grant.push_back(utf8(ident));
            }
            id resp = autorelease(alloc(class_named("SKProductsResponse")));
            auto* re = ext_get<ResponseExt>(resp);
            re->products = retain(arr(products));
            re->invalid = retain(arr(invalid));
            log("StoreKit: product request -> %zu products, %zu invalid", products.size(), invalid.size());
            if (responds(d, sel("productsRequest:didReceiveResponse:"))) send<void>(d, "productsRequest:didReceiveResponse:", self, resp);
            if (responds(d, sel("requestDidFinish:"))) send<void>(d, "requestDidFinish:", self);
            if (!grant.empty()) {
                // own_all: hand every product over through the app's own purchase handler
                std::vector<id> txs;
                for (auto& ident : grant) { owned_add(ident); txs.push_back(make_tx(make_payment(str(ident)), 1, nullptr)); }
                log("StoreKit: own_all granted %zu products", txs.size());
                deliver(txs);
            }
            release(self);
        }, 0.2);
    });
    Class RR = def_class("SKReceiptRefreshRequest", "SKRequest");
    def(RR, "initWithReceiptProperties:", +[](id self, SEL, id) -> id { return self; });
    def(RR, "start", +[](id self, SEL) {
        id d = ext_get<RequestExt>(self)->delegate;
        if (responds(d, sel("requestDidFinish:"))) send<void>(d, "requestDidFinish:", self);
    });

    Class P = def_class("SKProduct", "NSObject");
    def(P, "productIdentifier", +[](id self, SEL) -> id { return ext_get<ProductExt>(self)->ident; });
    def(P, "localizedTitle", +[](id self, SEL) -> id { return ext_get<ProductExt>(self)->ident; });
    def(P, "localizedDescription", +[](id, SEL) -> id { return str(""); });
    def(P, "price", +[](id, SEL) -> id { return send_cls(class_named("NSDecimalNumber"), "zero"); });
    def(P, "priceLocale", +[](id, SEL) -> id { return send_cls(class_named("NSLocale"), "currentLocale"); });
    def(P, "isDownloadable", +[](id, SEL) -> bool { return false; });
    def(P, "downloadContentLengths", +[](id, SEL) -> id { return arr({}); });
    Class RS = def_class("SKProductsResponse", "NSObject");
    def(RS, "products", +[](id self, SEL) -> id { return ext_get<ResponseExt>(self)->products; });
    def(RS, "invalidProductIdentifiers", +[](id self, SEL) -> id { return ext_get<ResponseExt>(self)->invalid; });

    Class PM = def_class("SKPayment", "NSObject");
    defc(PM, "paymentWithProduct:", +[](id, SEL, id product) -> id { return make_payment(send(product, "productIdentifier")); });
    defc(PM, "paymentWithProductIdentifier:", +[](id, SEL, id ident) -> id { return make_payment(ident); });
    def(PM, "productIdentifier", +[](id self, SEL) -> id { return ext_get<PaymentExt>(self)->ident; });
    def(PM, "quantity", +[](id self, SEL) -> long { return ext_get<PaymentExt>(self)->qty; });
    def(PM, "requestData", +[](id, SEL) -> id { return nullptr; });
    def(PM, "applicationUsername", +[](id, SEL) -> id { return nullptr; });
    def(PM, "copyWithZone:", +[](id self, SEL, void*) -> id { return retain(self); });
    def(PM, "mutableCopyWithZone:", +[](id self, SEL, void*) -> id {
        id m = alloc(class_named("SKMutablePayment"));
        ext_get<PaymentExt>(m)->ident = retain(ext_get<PaymentExt>(self)->ident);
        return m;
    });
    Class MP = def_class("SKMutablePayment", "SKPayment");
    def(MP, "setProductIdentifier:", +[](id self, SEL, id i) { auto* e = ext_get<PaymentExt>(self); release(e->ident); e->ident = send(i, "copy"); });
    def(MP, "setQuantity:", +[](id self, SEL, long q) { ext_get<PaymentExt>(self)->qty = q; });
    def(MP, "setRequestData:", +[](id, SEL, id) {});
    def(MP, "setApplicationUsername:", +[](id, SEL, id) {});

    Class T = def_class("SKPaymentTransaction", "NSObject");
    def(T, "payment", +[](id self, SEL) -> id { return ext_get<TxExt>(self)->payment; });
    def(T, "transactionState", +[](id self, SEL) -> long { return ext_get<TxExt>(self)->state; });
    def(T, "transactionIdentifier", +[](id self, SEL) -> id { return ext_get<TxExt>(self)->ident; });
    def(T, "transactionDate", +[](id self, SEL) -> id { return ext_get<TxExt>(self)->date; });
    def(T, "originalTransaction", +[](id self, SEL) -> id { return ext_get<TxExt>(self)->original; });
    def(T, "transactionReceipt", +[](id, SEL) -> id { return autorelease(data_new("offline", 7)); });
    def(T, "error", +[](id self, SEL) -> id {
        if (ext_get<TxExt>(self)->state != 2) return nullptr;
        return send_cls(class_named("NSError"), "errorWithDomain:code:userInfo:", str("SKErrorDomain"), 0L, static_cast<id>(nullptr));
    });
    def(T, "downloads", +[](id, SEL) -> id { return arr({}); });
    def_class("SKStoreProductViewController", "UIViewController");
}

static void init_services() {
    void* nil_v = nullptr;
    (void)nil_v;
    // GameKit
    Class LP = def_class("GKLocalPlayer", "NSObject");
    static id local = alloc(LP);
    defc(LP, "localPlayer", +[](id, SEL) -> id { return local; });
    def(LP, "isAuthenticated", +[](id, SEL) -> bool { return false; });
    def(LP, "authenticated", +[](id, SEL) -> bool { return false; });
    def(LP, "playerID", +[](id, SEL) -> id { return nullptr; });
    def(LP, "alias", +[](id, SEL) -> id { return nullptr; });
    def(LP, "displayName", +[](id, SEL) -> id { return nullptr; });
    def(LP, "setAuthenticateHandler:", +[](id, SEL, id block) {
        // tell the game authentication failed, as a signed-out device would
        if (!block) return;
        extern void* app_block_copy_public(id); (void)app_block_copy_public;
    });
    def(LP, "authenticateHandler", +[](id, SEL) -> id { return nullptr; });
    def(LP, "authenticateWithCompletionHandler:", +[](id, SEL, id) {});
    def(LP, "loadFriendsWithCompletionHandler:", +[](id, SEL, id) {});
    def(LP, "registerListener:", +[](id, SEL, id) {});
    Class GKS = def_class("GKScore", "NSObject");
    def(GKS, "initWithLeaderboardIdentifier:", +[](id self, SEL, id) -> id { return self; });
    def(GKS, "initWithCategory:", +[](id self, SEL, id) -> id { return self; });
    def(GKS, "setValue:", +[](id, SEL, long long) {});
    def(GKS, "reportScoreWithCompletionHandler:", +[](id, SEL, id) {});
    defc(GKS, "reportScores:withCompletionHandler:", +[](id, SEL, id, id) {});
    Class GKA = def_class("GKAchievement", "NSObject");
    def(GKA, "initWithIdentifier:", +[](id self, SEL, id) -> id { return self; });
    def(GKA, "setPercentComplete:", +[](id, SEL, double) {});
    def(GKA, "reportAchievementWithCompletionHandler:", +[](id, SEL, id) {});
    defc(GKA, "reportAchievements:withCompletionHandler:", +[](id, SEL, id, id) {});
    for (const char* n : {"GKMatchRequest", "GKTurnBasedMatchmakerViewController", "GKTurnBasedEventHandler", "GKPlayer", "GKLeaderboard", "GKAchievementViewController",
                          "GKLeaderboardViewController", "GKMatchmakerViewController"})
        def_class(n, "NSObject");

    init_storekit();

    // iAd / ads (Chartboost, Flurry, AdColony et al. are the guest's own classes; leave them to run, but neuter
    // the Apple ad classes they may fall back to)
    Class ADB = def_class("ADBannerView", "UIView");
    def(ADB, "setDelegate:", +[](id, SEL, id) {});
    def(ADB, "isBannerLoaded", +[](id, SEL) -> bool { return false; });
    def_class("ADInterstitialAd", "NSObject");

    // MessageUI / social share controllers: unavailable
    Class MFM = def_class("MFMailComposeViewController", "UIViewController");
    defc(MFM, "canSendMail", +[](id, SEL) -> bool { return false; });
    Class MFMsg = def_class("MFMessageComposeViewController", "UIViewController");
    defc(MFMsg, "canSendText", +[](id, SEL) -> bool { return false; });
    Class SLC = def_class("SLComposeViewController", "UIViewController");
    defc(SLC, "isAvailableForServiceType:", +[](id, SEL, id) -> bool { return false; });
    defc(SLC, "composeViewControllerForServiceType:", +[](id self, SEL, id) -> id { return autorelease(alloc(reinterpret_cast<Class>(self))); });

    // MPMusicPlayer / MediaPlayer movie player: no media
    Class MP = def_class("MPMoviePlayerController", "NSObject");
    def(MP, "initWithContentURL:", +[](id self, SEL, id) -> id { return self; });
    for (const char* s : {"play", "stop", "pause", "prepareToPlay"}) def(MP, s, +[](id, SEL) {});
    def(MP, "view", +[](id, SEL) -> id { return autorelease(send(send_cls(class_named("UIView"), "alloc"), "initWithFrame:", CGRect{{0, 0}, {0, 0}})); });
    def(MP, "setControlStyle:", +[](id, SEL, long) {});
    def(MP, "setScalingMode:", +[](id, SEL, long) {});

    // Reachability wrappers built on SCNetworkReachability already return reachable; some games use a bundled
    // Reachability class (their own) and ask the shared instance — leave those to the guest's code.
}

// ---------------------------------------------------------------------------------------------------------------
// bits of libSystem the game may reach that bionic lacks or names differently are handled by the loader; here we
// only add a few Darwin-named helpers the app layer itself wants callable by name.
// ---------------------------------------------------------------------------------------------------------------
void init_misc() {
    g_cfbundle = send_cls(class_named("NSBundle"), "mainBundle");
    init_coremotion();
    init_services();
}

void register_misc_symbols(std::unordered_map<std::string, void*>& m) {
    auto r = [&](const char* n, auto f) { m[n] = reinterpret_cast<void*>(f); };
    // CoreFoundation
    r("_CFRetain", &cf_retain);
    r("_CFRelease", &cf_release);
    r("_CFGetRetainCount", &cf_retain_count);
    r("_CFAutorelease", &cf_autorelease);
    r("_CFHash", &cf_hash);
    r("_CFEqual", &cf_equal);
    r("_CFCopyDescription", &cf_copy_description);
    r("_CFGetTypeID", &cf_get_type_id);
    r("_CFStringGetLength", &cf_string_get_length);
    r("_CFStringGetCharacterAtIndex", &cf_string_get_char);
    r("_CFStringGetCStringPtr", &cf_string_get_cstr_ptr);
    r("_CFStringGetCString", &cf_string_get_cstr);
    r("_CFStringGetBytes", &cf_string_get_bytes);
    r("_CFStringCreateWithCString", &cf_string_create_with_cstr);
    r("_CFStringCreateWithBytes", &cf_string_create_with_bytes);
    r("_CFStringCreateCopy", &cf_string_create_copy);
    r("_CFStringCreateMutable", &cf_string_create_mutable);
    r("_CFStringAppend", &cf_string_append);
    r("_CFStringCompare", &cf_string_compare);
    r("_CFStringGetSystemEncoding", +[]() -> uint32_t { return 0x08000100; });
    r("_CFDictionaryGetValue", &cf_dict_get_value);
    r("_CFDictionaryGetValueIfPresent", &cf_dict_get_value_if);
    r("_CFDictionaryGetCount", &cf_dict_get_count);
    r("_CFDictionaryCreateMutable", &cf_dict_create_mutable);
    r("_CFDictionaryCreate", &cf_dict_create);
    r("_CFDictionarySetValue", &cf_dict_set_value);
    r("_CFDictionaryAddValue", &cf_dict_add_value);
    r("_CFDictionaryRemoveValue", &cf_dict_remove_value);
    r("_CFDictionaryContainsKey", &cf_dict_contains);
    r("_CFArrayCreateMutable", &cf_array_create_mutable);
    r("_CFArrayCreate", &cf_array_create);
    r("_CFArrayGetCount", &cf_array_get_count);
    r("_CFArrayGetValueAtIndex", &cf_array_get_value);
    r("_CFArrayAppendValue", &cf_array_append);
    r("_CFArrayContainsValue", &cf_array_contains);
    r("_CFDataGetBytePtr", &cf_data_get_ptr);
    r("_CFDataGetLength", &cf_data_get_length);
    r("_CFDataCreate", &cf_data_create);
    r("_CFNumberCreate", &cf_number_create);
    r("_CFNumberGetValue", &cf_number_get_value);
    r("_CFNumberIsFloatType", &cf_number_is_float);
    r("_CFBundleGetMainBundle", &cf_bundle_get_main);
    r("_CFBundleGetValueForInfoDictionaryKey", &cf_bundle_get_info_value);
    r("_CFBundleGetInfoDictionary", +[](id) -> id { return info_dictionary(); });
    r("_CFBundleCopyBundleURL", &cf_bundle_copy_bundle_url);
    r("_CFBundleCopyResourcesDirectoryURL", &cf_bundle_copy_resources_url);
    r("_CFBundleCopyResourceURL", &cf_bundle_copy_resource_url);
    r("_CFBundleGetIdentifier", &cf_bundle_get_identifier);
    r("_CFURLCreateWithString", &cf_url_create_with_string);
    r("_CFURLCreateWithFileSystemPath", &cf_url_create_with_path);
    r("_CFURLCreateFromFileSystemRepresentation", &cf_url_create_from_filesystem);
    r("_CFURLCopyPath", &cf_url_copy_path);
    r("_CFURLGetFileSystemRepresentation", &cf_url_get_fs_rep);
    r("_CFURLCreateStringByAddingPercentEscapes", &cf_url_create_string_by_adding_escapes);
    r("_CFRunLoopGetCurrent", &cf_runloop_get_current);
    r("_CFRunLoopGetMain", &cf_runloop_get_main);
    r("_CFRunLoopRun", &cf_runloop_run);
    r("_CFRunLoopRunInMode", &cf_runloop_run_in_mode);
    r("_CFRunLoopAddTimer", &cf_runloop_add_timer);
    r("_CFRunLoopAddSource", &cf_runloop_add_timer);
    r("_CFRunLoopStop", &cf_runloop_stop);
    r("_CFRunLoopWakeUp", &cf_runloop_wakeup);
    r("_CFRunLoopTimerInvalidate", &cf_noop_vp);
    r("_CFPreferencesCopyAppValue", &cf_prefs_copy);
    r("_CFPreferencesSetAppValue", &cf_prefs_set);
    r("_CFPreferencesAppSynchronize", &cf_prefs_sync);
    r("_CFLocaleCopyCurrent", &cf_locale_copy_current);
    r("_CFLocaleGetValue", &cf_locale_get_value);
    r("_CFAbsoluteTimeGetCurrent", &cf_absolute_time);
    r("_CFUUIDCreate", &cf_uuid_create);
    r("_CFUUIDCreateString", &cf_uuid_create_string);
    r("_CFTimeZoneCopySystem", &cf_timezone_copy_system);
    r("_CFStreamCreateBoundPair", &cf_stream_pair);
    r("_CFReadStreamOpen", &cf_stream_open);
    r("_CFWriteStreamOpen", &cf_stream_open);
    r("_CFReadStreamSetProperty", &cf_stream_set_prop);
    r("_CFWriteStreamSetProperty", &cf_stream_set_prop);
    // Security
    r("_SecItemCopyMatching", &sec_item_copy);
    r("_SecItemAdd", &sec_item_add);
    r("_SecItemUpdate", &sec_item_update);
    r("_SecItemDelete", &sec_item_delete);
    r("_SecTrustEvaluate", &sec_trust_evaluate);
    r("_SecTrustSetAnchorCertificates", &sec_zero);
    r("_SecTrustSetAnchorCertificatesOnly", &sec_zero);
    r("_SecCertificateCreateWithData", +[](void*, id) -> void* { return nullptr; });
    r("_SecRandomCopyBytes", &sec_random);
    // SystemConfiguration
    r("_SCNetworkReachabilityCreateWithName", &scr_create_with_name);
    r("_SCNetworkReachabilityCreateWithAddress", &scr_create_with_address);
    r("_SCNetworkReachabilityGetFlags", &scr_get_flags);
    r("_SCNetworkReachabilitySetCallback", &scr_set_callback);
    r("_SCNetworkReachabilityScheduleWithRunLoop", &scr_schedule);
    r("_SCNetworkReachabilityUnscheduleFromRunLoop", &scr_unschedule);
    r("_SCNetworkReachabilitySetDispatchQueue", &scr_set_dispatch);
    // CF string-constant globals the guest reads (NSString* bridged)
    auto cfstr = [&](const char* n, const char* v) { id* slot = new id; *slot = str_new(v); m[std::string("_") + n] = slot; };
    cfstr("kCFRunLoopDefaultMode", "kCFRunLoopDefaultMode");
    cfstr("kCFRunLoopCommonModes", "kCFRunLoopCommonModes");
    cfstr("kCFBundleVersionKey", "CFBundleVersion");
    cfstr("kCFBundleIdentifierKey", "CFBundleIdentifier");
    cfstr("kCFBundleNameKey", "CFBundleName");
    cfstr("kCFBundleExecutableKey", "CFBundleExecutable");
    cfstr("kCFLocaleCountryCode", "kCFLocaleCountryCode");
    cfstr("kCFLocaleLanguageCode", "kCFLocaleLanguageCode");
    cfstr("kCFLocaleCurrencyCode", "kCFLocaleCurrencyCode");
    cfstr("kCFLocaleScriptCode", "kCFLocaleScriptCode");
    // keychain / Security constant keys (NSString-keyed dictionaries)
    for (const char* n : {"kSecClass", "kSecClassGenericPassword", "kSecClassInternetPassword", "kSecAttrAccount", "kSecAttrService", "kSecAttrGeneric",
                          "kSecAttrAccessGroup", "kSecAttrAccessible", "kSecAttrAccessibleWhenUnlocked", "kSecAttrAccessibleAfterFirstUnlock",
                          "kSecAttrAccessibleAlways", "kSecValueData", "kSecReturnData", "kSecReturnAttributes", "kSecReturnRef", "kSecMatchLimit",
                          "kSecMatchLimitOne", "kSecMatchLimitAll", "kSecAttrLabel", "kSecAttrDescription", "kSecAttrSynchronizable"}) {
        // the first character after 'kSec' encodes the kind so the keychain matcher can tell keys apart
        std::string key = n[4] == 'C' ? "clas" : std::string("a") + static_cast<char>(tolower(n[4])) + static_cast<char>(tolower(n[5]));
        if (!std::strcmp(n, "kSecValueData")) key = "v_Data";
        else if (!std::strcmp(n, "kSecReturnData")) key = "r_Data";
        else if (!std::strcmp(n, "kSecReturnAttributes")) key = "r_Attributes";
        else if (!std::strcmp(n, "kSecReturnRef")) key = "r_Ref";
        else if (!std::strcmp(n, "kSecMatchLimit")) key = "m_Limit";
        else if (!std::strcmp(n, "kSecMatchLimitOne")) key = "m_LimitOne";
        else if (!std::strcmp(n, "kSecMatchLimitAll")) key = "m_LimitAll";
        else if (!std::strcmp(n, "kSecClass")) key = "aclass";
        else if (!std::strncmp(n, "kSecClass", 9)) key = std::string("class_") + (n + 9);
        else if (!std::strcmp(n, "kSecAttrAccount")) key = "acct";
        else if (!std::strcmp(n, "kSecAttrService")) key = "svce";
        else if (!std::strcmp(n, "kSecAttrGeneric")) key = "gena";
        else if (!std::strncmp(n, "kSecAttrAccessible", 18)) key = "pdmn";
        else if (!std::strncmp(n, "kSecAttr", 8)) key = std::string("a_") + (n + 8);
        id* slot = new id;
        *slot = str_new(key.c_str());
        m[std::string("_") + n] = slot;
    }
}

}  // namespace app
