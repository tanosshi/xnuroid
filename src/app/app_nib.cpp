// Compiled NIBs and storyboards: NSKeyedArchiver property lists whose objects are instantiated with -initWithCoder:
// (through a decoder over each object's keys), then wired by the archived outlet and action connections.
#include "app.hpp"

#include <sys/stat.h>

#include <map>

namespace app {

id make_color(double r, double g, double b, double a);   // app_uikit.cpp

namespace {

struct Archive {
    id root = nullptr;              // the parsed plist (+1)
    id objects = nullptr;           // $objects
    id owner = nullptr;
    std::map<int64_t, id> memo;     // UID -> instantiated object
    std::vector<id> created;        // objects instantiated through -initWithCoder:/-init (+1 each)
    std::vector<std::pair<int64_t, id>> connections;
    ~Archive() {
        for (id o : created) release(o);
        release(root);
    }
};

Class C_Decoder;
struct DecExt : Ext { Archive* ar = nullptr; id dict = nullptr; };

int64_t uid_of(id v) {
    if (!is_dict(v)) return -1;
    id u = dict_get(v, "CF$UID");
    return u ? num_ival(u) : -1;
}

id resolve(Archive* ar, id value);

id decoder_for(Archive* ar, id dict) {
    id d = autorelease(alloc(C_Decoder));
    auto* e = ext_get<DecExt>(d);
    e->ar = ar;
    e->dict = dict;
    return d;
}

std::string class_name_of(Archive* ar, id dict, std::vector<std::string>* chain = nullptr) {
    id cls = dict_get(dict, "$class");
    int64_t u = uid_of(cls);
    if (u < 0) return "";
    const auto& objs = arr_items(ar->objects);
    if (static_cast<size_t>(u) >= objs.size()) return "";
    id info = objs[u];
    if (chain) for (id c : arr_items(dict_get(info, "$classes"))) chain->push_back(utf8(c));
    return utf8(dict_get(info, "$classname"));
}

id instantiate(Archive* ar, int64_t uid, id dict) {
    std::vector<std::string> chain;
    std::string cn = class_name_of(ar, dict, &chain);
    auto items = [&](const char* key) {
        std::vector<id> v;
        for (id x : arr_items(dict_get(dict, key))) { id r = resolve(ar, x); if (r) v.push_back(r); }
        return v;
    };
    if (cn == "NSArray" || cn == "NSMutableArray") { id a = arr(items("NS.objects")); ar->memo[uid] = a; return a; }
    if (cn == "NSSet" || cn == "NSMutableSet") { id s = send_cls(class_named("NSMutableSet"), "setWithArray:", arr(items("NS.objects"))); ar->memo[uid] = s; return s; }
    if (cn == "NSDictionary" || cn == "NSMutableDictionary") {
        id d = autorelease(dict_new(true));
        const auto& ks = arr_items(dict_get(dict, "NS.keys"));
        const auto& vs = arr_items(dict_get(dict, "NS.objects"));
        ar->memo[uid] = d;
        for (size_t i = 0; i < ks.size() && i < vs.size(); ++i) dict_set(d, resolve(ar, ks[i]), resolve(ar, vs[i]));
        return d;
    }
    if (cn == "NSString" || cn == "NSMutableString") {
        id s = dict_get(dict, "NS.string");
        if (!s) { id b = dict_get(dict, "NS.bytes"); s = b ? str(std::string(reinterpret_cast<const char*>(data_bytes(b)), data_len(b))) : str(""); }
        ar->memo[uid] = s;
        return s;
    }
    if (cn == "UIColor" || cn == "NSColor") {
        double r = 0, g = 0, b = 0, a = 1;
        if (dict_get(dict, "UIRed") || dict_get(dict, "UIGreen")) {
            r = num_dval(dict_get(dict, "UIRed")); g = num_dval(dict_get(dict, "UIGreen")); b = num_dval(dict_get(dict, "UIBlue"));
            if (id al = dict_get(dict, "UIAlpha")) a = num_dval(al);
        } else if (id w = dict_get(dict, "UIWhite")) {
            r = g = b = num_dval(w);
            if (id al = dict_get(dict, "UIAlpha")) a = num_dval(al);
        } else if (id rgb = dict_get(dict, "NSRGB")) {
            std::string s(reinterpret_cast<const char*>(data_bytes(rgb)), data_len(rgb));
            sscanf(s.c_str(), "%lf %lf %lf %lf", &r, &g, &b, &a);
        } else if (id wh = dict_get(dict, "NSWhite")) {
            std::string s(reinterpret_cast<const char*>(data_bytes(wh)), data_len(wh));
            sscanf(s.c_str(), "%lf %lf", &r, &a);
            g = b = r;
        }
        id c = make_color(r, g, b, a);
        ar->memo[uid] = c;
        return c;
    }
    if (cn == "UIProxyObject") {
        std::string ident = utf8(dict_get(dict, "UIProxiedObjectIdentifier"));
        id o = ident == "IBFilesOwner" ? ar->owner : nullptr;
        ar->memo[uid] = o;
        return o;
    }
    if (cn == "UIImageNibPlaceholder") {
        id img = send_cls(class_named("UIImage"), "imageNamed:", dict_get(dict, "UIResourceName"));
        ar->memo[uid] = img;
        return img;
    }
    if (cn == "UIFont") {
        id f = send_cls(class_named("UIFont"), "fontWithName:size:", dict_get(dict, "UIFontName"), num_dval(dict_get(dict, "UIFontPointSize")));
        ar->memo[uid] = f;
        return f;
    }
    if (cn == "UIRuntimeOutletConnection" || cn == "UIRuntimeEventConnection") {
        ar->memo[uid] = dict;
        return dict;
    }
    if (cn == "NSNull") { id n = send_cls(class_named("NSNull"), "null"); ar->memo[uid] = n; return n; }
    // a real object
    std::string real = cn;
    if (cn == "UIClassSwapper") {
        real = utf8(dict_get(dict, "UIClassName"));
        if (!class_named(real.c_str())) {
            log("nib: class %s not found, using %s", real.c_str(), utf8(dict_get(dict, "UIOriginalClassName")).c_str());
            real = utf8(dict_get(dict, "UIOriginalClassName"));
        }
    } else if (cn == "UICustomObject") {
        real = utf8(dict_get(dict, "UIClassName"));
    }
    Class c = class_named(real.c_str());
    for (size_t i = 0; !c && i < chain.size(); ++i) c = class_named(chain[i].c_str());
    if (!c) { log("nib: no class for %s", real.c_str()); ar->memo[uid] = nullptr; return nullptr; }
    id o = send(cls_obj(c), "alloc");
    ar->memo[uid] = o;
    id inited;
    if (responds(o, sel("initWithCoder:")) && cn != "UICustomObject") inited = send(o, "initWithCoder:", decoder_for(ar, dict));
    else inited = send(o, "init");
    if (inited != o) ar->memo[uid] = inited;
    if (inited) ar->created.push_back(inited);
    if (trace_enabled("nib")) log("nib: %s -> %s %p", cn.c_str(), class_name(class_of(inited)), static_cast<void*>(inited));
    return inited;
}

id resolve(Archive* ar, id value) {
    int64_t u = uid_of(value);
    if (u < 0) return value;
    auto it = ar->memo.find(u);
    if (it != ar->memo.end()) return it->second;
    const auto& objs = arr_items(ar->objects);
    if (static_cast<size_t>(u) >= objs.size()) return nullptr;
    id o = objs[u];
    if (is_string(o) && utf8(o) == "$null") { ar->memo[u] = nullptr; return nullptr; }
    if (is_dict(o) && dict_get(o, "$class")) return instantiate(ar, u, o);
    ar->memo[u] = o;
    return o;
}

// ---------------------------------------------------------------------------------------------------------------
// NIBArchive: the container modern Xcode emits for compiled storyboards/nibs (iOS 6+). It is not a keyed archive:
// five flat tables (objects, keys, values, class names) with varint-coded references. A storyboard whose entry nib
// is in this format could not be instantiated at all before, which is what a modern UIKit app needs at launch.
// Layout (validated against the bundle's Main.storyboardc): header at 10..49, then the tables in the order
// objects(18/22), keys(26/30), values(34/38), class names(42/46).

struct NArchive {
    struct Val { uint32_t key = 0; uint8_t type = 9; int64_t i = 0; double f = 0; std::string bytes; uint32_t ref = 0; };
    struct Obj { uint32_t cls = 0, vstart = 0, vcount = 0; };
    struct Cls { std::string name; int32_t fallback = -1; };
    std::vector<std::string> keys;
    std::vector<Val> vals;
    std::vector<Obj> objs;
    std::vector<Cls> classes;

    const Val* find(uint32_t obj, const char* key) const {
        if (obj >= objs.size()) return nullptr;
        const Obj& o = objs[obj];
        for (uint32_t i = 0; i < o.vcount; ++i) {
            size_t at = o.vstart + i;
            if (at >= vals.size()) break;
            if (vals[at].key < keys.size() && keys[vals[at].key] == key) return &vals[at];
        }
        return nullptr;
    }
};

static bool nib_archive_parse(const std::string& data, NArchive& a) {
    if (data.size() < 50 || std::memcmp(data.data(), "NIBArchive", 10) != 0) return false;
    const uint8_t* d = reinterpret_cast<const uint8_t*>(data.data());
    const size_t n = data.size();
    auto u32 = [&](size_t o) -> uint32_t { uint32_t v = 0; if (o + 4 <= n) std::memcpy(&v, d + o, 4); return v; };
    auto varint = [&](size_t& p) -> uint64_t {
        uint64_t r = 0;
        int shift = 0;
        while (p < n) {
            uint8_t b = d[p++];
            r |= static_cast<uint64_t>(b & 0x7f) << shift;
            shift += 7;
            if (b & 0x80) return r;
        }
        return r;
    };
    const uint32_t obj_count = u32(18), obj_off = u32(22);
    const uint32_t key_count = u32(26), key_off = u32(30);
    const uint32_t val_count = u32(34), val_off = u32(38);
    const uint32_t cls_count = u32(42), cls_off = u32(46);
    if (obj_off >= n || key_off >= n || val_off >= n || cls_off >= n) return false;

    size_t p = obj_off;
    for (uint32_t i = 0; i < obj_count; ++i) {
        NArchive::Obj o;
        o.cls = static_cast<uint32_t>(varint(p));
        o.vstart = static_cast<uint32_t>(varint(p));
        o.vcount = static_cast<uint32_t>(varint(p));
        a.objs.push_back(o);
    }
    p = key_off;
    for (uint32_t i = 0; i < key_count; ++i) {
        size_t len = static_cast<size_t>(varint(p));
        if (p + len > n) return false;
        a.keys.emplace_back(reinterpret_cast<const char*>(d + p), len);
        p += len;
    }
    p = val_off;
    for (uint32_t i = 0; i < val_count; ++i) {
        NArchive::Val v;
        v.key = static_cast<uint32_t>(varint(p));
        if (p >= n) return false;
        v.type = d[p++];
        switch (v.type) {
            case 0: if (p + 1 > n) return false; v.i = *reinterpret_cast<const int8_t*>(d + p); p += 1; break;
            case 1: { if (p + 2 > n) return false; int16_t s16; std::memcpy(&s16, d + p, 2); v.i = s16; p += 2; break; }
            case 2: { if (p + 4 > n) return false; int32_t s32; std::memcpy(&s32, d + p, 4); v.i = s32; p += 4; break; }
            case 3: if (p + 8 > n) return false; std::memcpy(&v.i, d + p, 8); p += 8; break;
            case 4: v.i = 1; break;
            case 5: v.i = 0; break;
            case 6: { if (p + 4 > n) return false; float f32; std::memcpy(&f32, d + p, 4); v.f = f32; p += 4; break; }
            case 7: if (p + 8 > n) return false; std::memcpy(&v.f, d + p, 8); p += 8; break;
            case 8: {
                size_t len = static_cast<size_t>(varint(p));
                if (p + len > n) return false;
                v.bytes.assign(reinterpret_cast<const char*>(d + p), len);
                p += len;
                break;
            }
            case 9: break;                                   // nil
            case 10: if (p + 4 > n) return false; std::memcpy(&v.ref, d + p, 4); p += 4; break;
            default: return false;
        }
        a.vals.push_back(std::move(v));
    }
    p = cls_off;
    for (uint32_t i = 0; i < cls_count; ++i) {
        NArchive::Cls c;
        size_t len = static_cast<size_t>(varint(p));
        uint64_t extra = varint(p);
        for (uint64_t k = 0; k < extra; ++k) {
            if (p + 4 > n) return false;
            int32_t e; std::memcpy(&e, d + p, 4); p += 4;
            if (k == 0) c.fallback = e;
        }
        if (p + len > n) return false;
        c.name.assign(reinterpret_cast<const char*>(d + p), len);
        while (!c.name.empty() && c.name.back() == '\0') c.name.pop_back();
        p += len;
        a.classes.push_back(std::move(c));
    }
    return true;
}

static CGRect nib_rect_from_bytes(const std::string& b, bool point) {
    CGRect r{{0, 0}, {0, 0}};
    // NIBArchive struct payload: one 0x07 type byte, then the CGFloats packed immediately after it (no alignment
    // padding). Verified against the bundle: UIBounds = 0x07 + 4 doubles (414x896), UICenter = 0x07 + 2 doubles.
    const size_t need = 1 + (point ? 16 : 32);
    if (b.size() < need) return r;
    double v[4] = {0, 0, 0, 0};
    std::memcpy(v, b.data() + 1, point ? 16 : 32);
    if (point) { r.origin.x = v[0]; r.origin.y = v[1]; }
    else { r.origin.x = v[0]; r.origin.y = v[1]; r.size.width = v[2]; r.size.height = v[3]; }
    return r;
}

id load_nib(const std::string& name, id owner, const std::string& extra_dir);

// Instantiates one archive. References are object indices, so class names that are themselves archived NSStrings are
// resolved recursively. A Swift class that the runtime cannot name falls back to the archive's UIOriginalClassName
// ("UIViewController", "UINavigationController"), which keeps the storyboard's UIKit geometry usable.
static id nib_archive_instantiate(NArchive& a, id owner, const std::string& path) {
    std::vector<id> memo(a.objs.size(), nullptr);
    std::vector<uint8_t> busy(a.objs.size(), 0);
    std::vector<id> created;

    std::function<std::string(uint32_t)> string_at = [&](uint32_t idx) -> std::string {
        if (idx >= a.objs.size()) return "";
        const NArchive::Val* b = a.find(idx, "NS.bytes");
        if (!b) return "";
        return b->bytes;
    };

    std::function<id(uint32_t)> inst = [&](uint32_t idx) -> id {
        if (idx >= a.objs.size()) return nullptr;
        if (memo[idx]) return memo[idx];
        if (busy[idx]) return nullptr;   // reference cycle through a class name
        busy[idx] = 1;
        const NArchive::Obj& o = a.objs[idx];
        std::string cn = o.cls < a.classes.size() ? a.classes[o.cls].name : "";
        auto ref_string = [&](const char* key) -> std::string {
            const NArchive::Val* v = a.find(idx, key);
            if (!v || v->type != 10) return "";
            return string_at(v->ref);
        };
        if (cn == "NSString" || cn == "NSMutableString") {
            id s = str(string_at(idx));
            memo[idx] = s;
            busy[idx] = 0;
            return s;
        }
        if (cn == "NSArray" || cn == "NSMutableArray") {
            std::vector<id> items;
            const NArchive::Obj& ao = a.objs[idx];
            for (uint32_t k = 0; k < ao.vcount; ++k) {
                size_t at = ao.vstart + k;
                if (at >= a.vals.size()) break;
                if (a.vals[at].type == 10) { id x = inst(a.vals[at].ref); if (x) items.push_back(x); }
            }
            id list = arr(items);
            memo[idx] = list;
            busy[idx] = 0;
            return list;
        }
        if (cn == "NSDictionary" || cn == "NSMutableDictionary") {
            id dd = autorelease(dict_new(false));
            memo[idx] = dd;
            busy[idx] = 0;
            return dd;
        }
        if (cn == "UIColor" || cn == "NSColor") {
            double r = 1, g = 1, b = 1, al = 1;
            if (const NArchive::Val* w = a.find(idx, "UIWhite")) { r = g = b = w->f; }
            if (const NArchive::Val* av = a.find(idx, "UIAlpha")) al = av->f;
            id color = make_color(r, g, b, al);
            memo[idx] = color;
            busy[idx] = 0;
            return color;
        }
        if (cn == "UIProxyObject") {
            std::string ident = ref_string("UIProxiedObjectIdentifier");
            id p = ident == "IBFilesOwner" ? owner : nullptr;
            memo[idx] = p;
            return p;
        }
        if (cn == "UIClassSwapper" || cn == "UICustomObject") {
            std::string named = ref_string("UIClassName");
            std::string original = ref_string("UIOriginalClassName");
            Class c = 0;
            if (!named.empty()) c = class_named(named.c_str());
            if (!c && !original.empty()) c = class_named(original.c_str());
            if (!c && o.cls < a.classes.size() && a.classes[o.cls].fallback >= 0) {
                size_t fb = static_cast<size_t>(a.classes[o.cls].fallback);
                if (fb < a.classes.size()) c = class_named(a.classes[fb].name.c_str());
            }
            if (!c) { log("nibarchive %s: no class for %s (original %s)", path.c_str(), named.c_str(), original.c_str()); busy[idx] = 0; return nullptr; }
            if (!named.empty() && named != class_name(c)) log("nibarchive: %s degraded to %s", named.c_str(), class_name(c));
            cn = class_name(c);
        }
        Class c = class_named(cn.c_str());
        if (!c && o.cls < a.classes.size() && a.classes[o.cls].fallback >= 0) {
            size_t fb = static_cast<size_t>(a.classes[o.cls].fallback);
            if (fb < a.classes.size()) c = class_named(a.classes[fb].name.c_str());
        }
        if (!c) { log("nibarchive %s: unknown class %s", path.c_str(), cn.c_str()); busy[idx] = 0; return nullptr; }
        id obj = send(cls_obj(c), "alloc");
        memo[idx] = obj;
        if (obj) {
            if (responds(obj, sel("initWithCoder:"))) { /* no decoder for this container: plain init below */ }
            if (responds(obj, sel("init"))) obj = send(obj, "init");
            memo[idx] = obj;
            created.push_back(obj);
        }
        busy[idx] = 0;
        return obj;
    };

    for (uint32_t i = 0; i < a.objs.size(); ++i) inst(i);

    // Apply the values that affect what the first frame looks like. Everything else (layout guides, bar metrics,
    // trait storage) is left at its default; the geometry keys are the ones the storyboard actually carries.
    for (uint32_t i = 0; i < a.objs.size(); ++i) {
        id o = memo[i];
        if (!o || !has_header(o)) continue;
        const NArchive::Obj& oo = a.objs[i];
        for (uint32_t k = 0; k < oo.vcount; ++k) {
            size_t at = oo.vstart + k;
            if (at >= a.vals.size()) break;
            const NArchive::Val& v = a.vals[at];
            std::string key = v.key < a.keys.size() ? a.keys[v.key] : "";
            if ((key == "UIBounds" || key == "UIFrame") && v.type == 8) {
                if (responds(o, sel("setFrame:"))) send<void>(o, "setFrame:", nib_rect_from_bytes(v.bytes, false));
            } else if (key == "UICenter" && v.type == 8) {
                if (responds(o, sel("setCenter:"))) send<void>(o, "setCenter:", nib_rect_from_bytes(v.bytes, true).origin);
            } else if (key == "UIBackgroundColor" && v.type == 10) {
                id color = memo[v.ref];
                if (color && responds(o, sel("setBackgroundColor:"))) send<void>(o, "setBackgroundColor:", color);
            } else if (key == "UIViewControllers" || key == "UIChildViewControllers") {
                id list = v.type == 10 ? memo[v.ref] : nullptr;
                if (list && responds(o, sel("setViewControllers:"))) send<void>(o, "setViewControllers:", list);
            }
        }
    }

    // Connections: the storyboard wires outlets and actions through archived connection objects, exactly like the
    // keyed-archive path does. Without this a controller never receives its "view" outlet.
    auto ref_of = [&](uint32_t idx, const char* key) -> uint32_t {
        const NArchive::Val* v = a.find(idx, key);
        return (v && v->type == 10) ? v->ref : 0xffffffffu;
    };
    for (uint32_t i = 0; i < a.objs.size(); ++i) {
        // The connection object itself has no UIKit class, so it is never instantiated; what matters is its source
        // and destination, which are ordinary archived objects.
        std::string cn = a.objs[i].cls < a.classes.size() ? a.classes[a.objs[i].cls].name : "";
        if (cn != "UIRuntimeOutletConnection" && cn != "UIRuntimeEventConnection") continue;
        uint32_t sr = ref_of(i, "UISource"), dr = ref_of(i, "UIDestination"), lr = ref_of(i, "UILabel");
        id dst = dr < memo.size() ? memo[dr] : nullptr;
        std::string label = lr < a.objs.size() ? string_at(lr) : "";
        if (sr >= memo.size() || !memo[sr] || label.empty()) continue;
        id src = memo[sr];
        if (cn == "UIRuntimeOutletConnection") {
            std::string setter = "set" + std::string(1, static_cast<char>(toupper(label[0]))) + label.substr(1) + ":";
            log("nibarchive outlet %s.%s", class_name(class_of(src)), label.c_str());
            if (responds(src, sel(setter.c_str()))) send<void>(src, setter.c_str(), dst);
            else send<void>(src, "setValue:forKey:", dst, str(label));
        } else {
            const NArchive::Val* ev = a.find(i, "UIEventMask");
            NSUInteger mask = ev ? static_cast<NSUInteger>(ev->i) : 0;
            send<void>(src, "addTarget:action:forControlEvents:", dst, sel(label.c_str()), mask);
        }
    }

    // A view controller archives the name of the nib that holds its view (UINibName). Load it now so the controller
    // has a real view by the time the window takes its first frame; the nothing-else case leaves -view to lazy-load.
    for (uint32_t i = 0; i < a.objs.size(); ++i) {
        id o = memo[i];
        if (!o || !responds(o, sel("viewIfLoaded"))) continue;
        const NArchive::Val* nv = a.find(i, "UINibName");
        if (!nv || nv->type != 10 || send(o, "viewIfLoaded")) continue;
        std::string vn = string_at(nv->ref);
        if (vn.empty()) continue;
        size_t slash = path.rfind('/');
        std::string dir = slash == std::string::npos ? std::string() : path.substr(0, slash);
        log("nibarchive: loading view nib %s for %s", vn.c_str(), class_name(class_of(o)));
        load_nib(vn, o, dir);
    }

    id tops = nullptr;
    for (size_t i = 0; i < a.vals.size(); ++i) {
        const NArchive::Val& v = a.vals[i];
        if (v.type != 10 || v.key >= a.keys.size() || a.keys[v.key] != "UINibTopLevelObjectsKey") continue;
        id list = memo[v.ref];
        if (list) tops = arr(arr_items(list));
    }
    if (!tops) {
        std::vector<id> all;
        for (id o : created) if (o) all.push_back(o);
        tops = arr(all);
    }
    log("nibarchive %s: %zu objects", path.c_str(), a.objs.size());
    return tops;
}

std::string find_nib(const std::string& name, const std::string& extra_dir) {
    std::vector<std::string> dirs;
    if (!extra_dir.empty()) dirs.push_back(extra_dir);
    std::string b = bundle_dir();
    dirs.push_back(b);
    dirs.push_back(b + "/en.lproj");
    dirs.push_back(b + "/Base.lproj");
    dirs.push_back(b + "/English.lproj");
    for (auto& d : dirs) {
        for (std::string cand : {d + "/" + name + ".nib", d + "/" + name}) {
            struct stat st;
            if (stat(cand.c_str(), &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) {
                for (const char* f : {"/runtime.nib", "/objects-8.0+.nib", "/objects.nib"})
                    if (stat((cand + f).c_str(), &st) == 0) return cand + f;
                continue;
            }
            return cand;
        }
    }
    return "";
}

std::string g_storyboard_dir;

// returns the top-level objects (autoreleased array) or nil
id load_nib(const std::string& name, id owner, const std::string& extra_dir) {
    std::string path = find_nib(name, extra_dir);
    if (path.empty()) { log("nib %s not found", name.c_str()); return nullptr; }
    std::string data;
    if (!read_file(path, data)) return nullptr;
    if (data.size() >= 50 && std::memcmp(data.data(), "NIBArchive", 10) == 0) {
        NArchive a;
        if (!nib_archive_parse(data, a)) { log("nib %s: damaged NIBArchive", path.c_str()); return nullptr; }
        return nib_archive_instantiate(a, owner, path);
    }
    auto* ar = new Archive();
    ar->root = plist_parse(reinterpret_cast<const uint8_t*>(data.data()), data.size(), false);
    ar->objects = dict_get(ar->root, "$objects");
    ar->owner = owner;
    id top = dict_get(ar->root, "$top");
    if (!ar->objects || !top) { log("nib %s: not a keyed archive", path.c_str()); delete ar; return nullptr; }
    log("loading nib %s (owner %s)", path.c_str(), owner ? class_name(class_of(owner)) : "nil");
    id all = resolve(ar, dict_get(top, "UINibObjectsKey"));
    id conns = resolve(ar, dict_get(top, "UINibConnectionsKey"));
    id tops = resolve(ar, dict_get(top, "UINibTopLevelObjectsKey"));
    for (id c : arr_items(conns)) {
        if (!is_dict(c)) continue;
        std::string cn = class_name_of(ar, c);
        id src = resolve(ar, dict_get(c, "UISource"));
        id dst = resolve(ar, dict_get(c, "UIDestination"));
        std::string label = utf8(dict_get(c, "UILabel"));
        if (!src || label.empty()) continue;
        if (cn == "UIRuntimeOutletConnection") {
            std::string setter = "set" + std::string(1, static_cast<char>(toupper(label[0]))) + label.substr(1) + ":";
            if (trace_enabled("nib")) log("nib outlet %s.%s = %s", class_name(class_of(src)), label.c_str(), dst ? class_name(class_of(dst)) : "nil");
            if (responds(src, sel(setter.c_str()))) send<void>(src, setter.c_str(), dst);
            else send<void>(src, "setValue:forKey:", dst, str(label));
        } else if (cn == "UIRuntimeEventConnection") {
            NSUInteger mask = static_cast<NSUInteger>(num_ival(dict_get(c, "UIEventMask")));
            send<void>(src, "addTarget:action:forControlEvents:", dst, sel(label.c_str()), mask);
        }
    }
    for (id o : arr_items(all))
        if (o && o != owner && responds(o, sel("awakeFromNib"))) send<void>(o, "awakeFromNib");
    id result = tops ? arr(arr_items(tops)) : arr({});
    retain(result);
    delete ar;   // instantiated objects stay alive through their owners/outlets
    return autorelease(result);
}

}  // namespace

id nib_instantiate_storyboard_initial(const std::string& name) {
    std::string dir;
    for (std::string d : {bundle_dir() + "/" + name + ".storyboardc", bundle_dir() + "/en.lproj/" + name + ".storyboardc", bundle_dir() + "/Base.lproj/" + name + ".storyboardc"}) {
        struct stat st;
        if (stat(d.c_str(), &st) == 0) { dir = d; break; }
    }
    if (dir.empty()) { log("storyboard %s not found", name.c_str()); return nullptr; }
    g_storyboard_dir = dir;
    std::string data;
    if (!read_file(dir + "/Info.plist", data)) return nullptr;
    id info = autorelease(plist_parse(reinterpret_cast<const uint8_t*>(data.data()), data.size(), false));
    id entry = dict_get(info, "UIStoryboardDesignatedEntryPointIdentifier");
    id nib = dict_get(dict_get(info, "UIViewControllerIdentifiersToNibNames"), entry);
    if (!nib) { log("storyboard %s: no entry point", name.c_str()); return nullptr; }
    id tops = load_nib(utf8(nib), nullptr, dir);
    for (id o : arr_items(tops))
        if (is_kind(o, "UIViewController")) { log("storyboard initial view controller: %s", class_name(class_of(o))); return o; }
    return nullptr;
}

bool nib_load_view(id vc) {
    id name = send(vc, "nibName");
    if (!name) return false;
    load_nib(utf8(name), vc, g_storyboard_dir);
    return send(vc, "viewIfLoaded") != nullptr;
}

id nib_load_main(const std::string& name, id owner) { return load_nib(name, owner, ""); }

void init_nib() {
    C_Decoder = def_class("UINibDecoder", "NSObject");
    Class D = C_Decoder;
    def(D, "decodeObjectForKey:", +[](id self, SEL, id k) -> id { auto* e = ext_get<DecExt>(self); return resolve(e->ar, dict_get(e->dict, k)); });
    def(D, "decodeObjectOfClass:forKey:", +[](id self, SEL, Class, id k) -> id { auto* e = ext_get<DecExt>(self); return resolve(e->ar, dict_get(e->dict, k)); });
    def(D, "containsValueForKey:", +[](id self, SEL, id k) -> bool { return dict_get(ext_get<DecExt>(self)->dict, k) != nullptr; });
    def(D, "decodeBoolForKey:", +[](id self, SEL, id k) -> bool { return num_ival(dict_get(ext_get<DecExt>(self)->dict, k)) != 0; });
    def(D, "decodeIntForKey:", +[](id self, SEL, id k) -> int { return static_cast<int>(num_ival(dict_get(ext_get<DecExt>(self)->dict, k))); });
    def(D, "decodeInt32ForKey:", +[](id self, SEL, id k) -> int { return static_cast<int>(num_ival(dict_get(ext_get<DecExt>(self)->dict, k))); });
    def(D, "decodeInt64ForKey:", +[](id self, SEL, id k) -> long long { return num_ival(dict_get(ext_get<DecExt>(self)->dict, k)); });
    def(D, "decodeIntegerForKey:", +[](id self, SEL, id k) -> long { return num_ival(dict_get(ext_get<DecExt>(self)->dict, k)); });
    def(D, "decodeFloatForKey:", +[](id self, SEL, id k) -> float { return static_cast<float>(num_dval(dict_get(ext_get<DecExt>(self)->dict, k))); });
    def(D, "decodeDoubleForKey:", +[](id self, SEL, id k) -> double { return num_dval(dict_get(ext_get<DecExt>(self)->dict, k)); });
    def(D, "decodeCGRectForKey:", +[](id self, SEL, id k) -> CGRect {
        CGRect r{{0, 0}, {0, 0}};
        id v = resolve(ext_get<DecExt>(self)->ar, dict_get(ext_get<DecExt>(self)->dict, k));
        if (is_string(v)) sscanf(utf8(v).c_str(), "{{%lf, %lf}, {%lf, %lf}}", &r.origin.x, &r.origin.y, &r.size.width, &r.size.height);
        return r;
    });
    def(D, "decodeCGPointForKey:", +[](id self, SEL, id k) -> CGPoint {
        CGPoint p{0, 0};
        id v = resolve(ext_get<DecExt>(self)->ar, dict_get(ext_get<DecExt>(self)->dict, k));
        if (is_string(v)) sscanf(utf8(v).c_str(), "{%lf, %lf}", &p.x, &p.y);
        return p;
    });
    def(D, "decodeCGSizeForKey:", +[](id self, SEL, id k) -> CGSize {
        CGSize s{0, 0};
        id v = resolve(ext_get<DecExt>(self)->ar, dict_get(ext_get<DecExt>(self)->dict, k));
        if (is_string(v)) sscanf(utf8(v).c_str(), "{%lf, %lf}", &s.width, &s.height);
        return s;
    });
    def(D, "decodeBytesForKey:returnedLength:", +[](id self, SEL, id k, NSUInteger* n) -> const void* {
        id d = dict_get(ext_get<DecExt>(self)->dict, k);
        if (n) *n = data_len(d);
        return data_bytes(d);
    });
    def(D, "allowsKeyedCoding", +[](id, SEL) -> bool { return true; });
    def(D, "requiresSecureCoding", +[](id, SEL) -> bool { return false; });
}

}  // namespace app
