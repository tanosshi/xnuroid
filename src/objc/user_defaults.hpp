// NSUserDefaults (Unity's PlayerPrefs on iOS): typed setters/getters on top of the fake dictionary, persisted as an XML
// plist at <sandbox>/Library/Preferences/<CFBundleIdentifier>.plist. Changes are written on -synchronize and, at most
// once a second, from the run loop, so progress survives the process being killed.
#pragma once

#include <sys/stat.h>

namespace fakeui {

inline bool g_defaults_dirty = false;
inline double g_defaults_saved_at = 0;

inline bool apple_global_key(uint64_t k) {
    std::string s;
    return objc_string_value(k, s) && (s == "AppleLanguages" || s == "AppleLocale");
}

inline uint64_t make_number(double v, bool real) {
    uint64_t h = fake_make(8, "NSNumber");
    FakeObjcObj* n = fake_lookup(h);
    n->props["value"] = static_cast<uint64_t>(static_cast<int64_t>(v));
    setd(n, "value", v);
    if (real) n->props["#real"] = 1;
    return h;
}

inline std::string defaults_path() {
    std::string p = getenv("LOADER_SANDBOX_DIR") ? getenv("LOADER_SANDBOX_DIR") : "/data/local/tmp/sandbox";
    for (const char* sub : {"", "/Library", "/Library/Preferences"}) mkdir((p + sub).c_str(), 0755);
    std::string id = "defaults";
    if (FakeObjcObj* info = fake_lookup(info_dictionary())) {
        for (size_t i = 0; i < info->items.size() && i < info->values.size(); ++i) {
            std::string k;
            if (objc_string_value(info->items[i], k) && k == "CFBundleIdentifier") objc_string_value(info->values[i], id);
        }
    }
    return p + "/Library/Preferences/" + id + ".plist";
}

inline std::string xml_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else out.push_back(c);
    }
    return out;
}

// Appends v as a plist element; false for values a plist cannot hold here (NSData, foreign objects).
inline bool plist_write(std::string& out, uint64_t v, int depth) {
    std::string s;
    if (objc_string_value(v, s)) { out += "<string>" + xml_escape(s) + "</string>"; return true; }
    FakeObjcObj* o = fake_lookup(v);
    if (!o || o->kind != 8 || depth > 32) return false;
    if (o->s == "NSNumber") {
        if (getp(o, "#real")) {
            char buf[40];
            snprintf(buf, sizeof buf, "%.17g", getd(o, "value", 0));
            out += std::string("<real>") + buf + "</real>";
        } else {
            out += "<integer>" + std::to_string(static_cast<int64_t>(getp(o, "value"))) + "</integer>";
        }
        return true;
    }
    const bool dict = o->s == "NSDictionary" || o->s == "NSMutableDictionary";
    if (!dict && o->s != "NSArray" && o->s != "NSMutableArray") return false;
    out += dict ? "<dict>" : "<array>";
    for (size_t i = 0; i < o->items.size(); ++i) {
        std::string elem;
        if (dict) {
            std::string k;
            if (i >= o->values.size() || !objc_string_value(o->items[i], k) || !plist_write(elem, o->values[i], depth + 1)) continue;
            out += "<key>" + xml_escape(k) + "</key>";
        } else if (!plist_write(elem, o->items[i], depth + 1)) {
            continue;
        }
        out += elem;
    }
    out += dict ? "</dict>" : "</array>";
    return true;
}

inline void defaults_save(FakeObjcObj* d) {
    static const bool log = getenv("LOADER_LOG_DEFAULTS") != nullptr;
    std::string body;
    size_t saved = 0;
    for (size_t i = 0; i < d->items.size() && i < d->values.size(); ++i) {
        std::string k, elem;
        if (apple_global_key(d->items[i])) continue;
        if (!objc_string_value(d->items[i], k) || !plist_write(elem, d->values[i], 0)) {
            if (log) printf("[DEFAULTS] cannot store key %#llx\n", static_cast<unsigned long long>(d->items[i]));
            continue;
        }
        body += "<key>" + xml_escape(k) + "</key>" + elem + "\n";
        ++saved;
    }
    if (log) printf("[DEFAULTS] saving %zu keys\n", saved);
    const std::string path = defaults_path(), tmp = path + ".tmp";
    FILE* fp = fopen(tmp.c_str(), "wb");
    if (!fp) { printf("[DEFAULTS] cannot write %s\n", tmp.c_str()); return; }
    std::string doc = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\">\n<dict>\n" + body + "</dict>\n</plist>\n";
    bool ok = fwrite(doc.data(), 1, doc.size(), fp) == doc.size();
    ok = fflush(fp) == 0 && ok;
    fsync(fileno(fp));
    fclose(fp);
    if (ok && rename(tmp.c_str(), path.c_str()) == 0) {
        g_defaults_dirty = false;
        g_defaults_saved_at = now_s();
    }
}

inline void defaults_load(FakeObjcObj* d) {
    const std::string path = defaults_path();
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) return;
    std::string data;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, fp)) > 0) data.append(buf, n);
    fclose(fp);
    FakeObjcObj* saved = fake_lookup(XmlPlist(std::move(data)).parse());
    if (!saved) { printf("[DEFAULTS] unreadable %s\n", path.c_str()); return; }
    for (size_t i = 0; i < saved->items.size() && i < saved->values.size(); ++i) {
        long at = find_key(d, saved->items[i]);
        if (at >= 0) { d->values[at] = saved->values[i]; }
        else { d->items.push_back(saved->items[i]); d->values.push_back(saved->values[i]); }
        fake_retain(saved->items[i]);
        fake_retain(saved->values[i]);
    }
    printf("[DEFAULTS] loaded %zu keys from %s\n", saved->items.size(), path.c_str());
    fflush(stdout);
}

inline uint64_t standard_defaults() {
    static std::once_flag once;
    static uint64_t h = 0;
    std::call_once(once, [] {
        h = fake_make(8, "NSUserDefaults");
        FakeObjcObj* d = fake_lookup(h);
        d->items = {fake_make(2, "AppleLanguages"), fake_make(2, "AppleLocale")};   // global-domain values iOS provides
        d->values = {new_array({fake_make(2, "en-US")}), fake_make(2, "en_US")};
        defaults_load(d);
    });
    return h;
}

// Called every run-loop tick: writes pending changes, at most once a second.
inline void defaults_flush(bool force) {
    if (!g_defaults_dirty || (!force && now_s() - g_defaults_saved_at < 1.0)) return;
    if (FakeObjcObj* d = fake_lookup(standard_defaults())) defaults_save(d);
}

inline void defaults_store(FakeObjcObj* d, uint64_t key, uint64_t value) {
    if (!key) return;
    long i = find_key(d, key);
    if (i >= 0) {
        d->values[i] = value;
    } else {
        d->items.push_back(key);
        d->values.push_back(value);
        fake_retain(key);
    }
    fake_retain(value);
    ++d->mutations;
    g_defaults_dirty = true;
}

// Selectors beyond plain dictionary access; generic dictionary mutations only get marked dirty here.
inline bool defaults_message(CallFrame& frame, FakeObjcObj* d, const std::string& sel) {
    float f;
    std::memcpy(&f, &g_objc_in_d[0], 4);
    if (sel == "setInteger:forKey:") { defaults_store(d, frame.x[3], make_number(static_cast<double>(static_cast<int64_t>(frame.x[2])), false)); return true; }
    if (sel == "setBool:forKey:") { defaults_store(d, frame.x[3], make_number((frame.x[2] & 0xff) ? 1 : 0, false)); return true; }
    if (sel == "setFloat:forKey:") { defaults_store(d, frame.x[2], make_number(f, true)); return true; }
    if (sel == "setDouble:forKey:") { defaults_store(d, frame.x[2], make_number(g_objc_in_d[0], true)); return true; }
    if (sel == "setObject:forKey:" || sel == "setValue:forKey:") {
        if (!frame.x[3]) return true;   // nil key: Cocoa raises, storing it would poison the file
        g_defaults_dirty = true;
        return false;
    }
    if (sel == "removeObjectForKey:") { g_defaults_dirty = true; return false; }
    if (sel == "floatForKey:" || sel == "doubleForKey:") {
        long i = find_key(d, frame.x[2]);
        FakeObjcObj* v = i < 0 ? nullptr : fake_lookup(d->values[i]);
        std::string s;
        double r = v && v->s == "NSNumber" ? getd(v, "value", 0) : i >= 0 && objc_string_value(d->values[i], s) ? strtod(s.c_str(), nullptr) : 0;
        if (sel == "floatForKey:") objc_ret_float(static_cast<float>(r));
        else objc_ret_double(r);
        return true;
    }
    if (sel == "stringForKey:" || sel == "arrayForKey:" || sel == "dictionaryForKey:" || sel == "dataForKey:") {
        long i = find_key(d, frame.x[2]);
        std::string s;
        uint64_t v = i < 0 ? 0 : d->values[i];
        FakeObjcObj* o = fake_lookup(v);
        bool str = objc_string_value(v, s);
        bool match = sel == "stringForKey:" ? str
                   : sel == "arrayForKey:"  ? o && (o->s == "NSArray" || o->s == "NSMutableArray")
                   : sel == "dictionaryForKey:" ? o && (o->s == "NSDictionary" || o->s == "NSMutableDictionary")
                   : false;
        frame.ret_x0 = match ? v : 0;
        return true;
    }
    if (sel == "removePersistentDomainForName:") {
        for (size_t i = d->items.size(); i-- > 0;) {
            if (apple_global_key(d->items[i])) continue;
            d->items.erase(d->items.begin() + i);
            d->values.erase(d->values.begin() + i);
        }
        ++d->mutations;
        g_defaults_dirty = true;
        return true;
    }
    if (sel == "registerDefaults:") {
        if (FakeObjcObj* reg = fake_lookup(frame.x[2]))
            for (size_t i = 0; i < reg->items.size() && i < reg->values.size(); ++i)
                if (find_key(d, reg->items[i]) < 0) {
                    d->items.push_back(reg->items[i]); d->values.push_back(reg->values[i]);
                    fake_retain(reg->items[i]); fake_retain(reg->values[i]);
                }
        return true;
    }
    if (sel == "dictionaryRepresentation") {
        uint64_t h = fake_make(8, "NSDictionary");
        FakeObjcObj* r = fake_lookup(h);
        r->items = d->items;
        r->values = d->values;
        for (uint64_t k : r->items) fake_retain(k);
        for (uint64_t v : r->values) fake_retain(v);
        frame.ret_x0 = h;
        return true;
    }
    if (sel == "synchronize") { defaults_flush(true); frame.ret_x0 = 1; return true; }
    return false;
}

}  // namespace fakeui
