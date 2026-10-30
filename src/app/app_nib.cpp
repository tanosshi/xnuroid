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
