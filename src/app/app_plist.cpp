// Property lists (binary bplist00, XML and old-style ASCII/.strings) and JSON, to and from Foundation objects.
#include "app.hpp"

#include <cmath>
#include <cstdlib>
#include <ctime>
#include <map>

namespace app {

id make_date(double ref);     // app_foundation2.cpp
double date_ref(id d);

namespace {

// ---- binary ----
struct BReader {
    const uint8_t* p;
    size_t n;
    unsigned off_size = 0, ref_size = 0;
    uint64_t num = 0, top = 0, table = 0;
    bool mut = false;
    int depth = 0;
    uint64_t be(size_t at, unsigned bytes) const {
        uint64_t v = 0;
        for (unsigned i = 0; i < bytes && at + i < n; ++i) v = (v << 8) | p[at + i];
        return v;
    }
    uint64_t offset_of(uint64_t obj) const { return be(table + obj * off_size, off_size); }
    bool count(size_t& at, uint8_t marker, uint64_t& out) const {
        out = marker & 0xf;
        if (out != 0xf) return true;
        if (at >= n) return false;
        uint8_t m = p[at++];
        if ((m >> 4) != 1) return false;
        unsigned bytes = 1u << (m & 0xf);
        out = be(at, bytes);
        at += bytes;
        return true;
    }
    id object(uint64_t ref) {
        if (ref >= num || ++depth > 512) { --depth; return nullptr; }
        id r = parse_at(offset_of(ref));
        --depth;
        return r;
    }
    id parse_at(size_t at) {
        if (at >= n) return nullptr;
        uint8_t m = p[at++];
        uint64_t c;
        switch (m >> 4) {
            case 0x0:
                if (m == 0x08) return retain(num_b(false));
                if (m == 0x09) return retain(num_b(true));
                return nullptr;
            case 0x1: {
                unsigned bytes = 1u << (m & 0xf);
                uint64_t v = be(at, bytes);
                int64_t sv = bytes == 8 ? static_cast<int64_t>(v) : bytes == 4 ? static_cast<int64_t>(v) : static_cast<int64_t>(v);
                return retain(num_i(sv));
            }
            case 0x2: {
                unsigned bytes = 1u << (m & 0xf);
                uint64_t v = be(at, bytes);
                if (bytes == 4) { uint32_t u = static_cast<uint32_t>(v); float f; std::memcpy(&f, &u, 4); return retain(num_d(f)); }
                double d; std::memcpy(&d, &v, 8); return retain(num_d(d));
            }
            case 0x3: { uint64_t v = be(at, 8); double d; std::memcpy(&d, &v, 8); return retain(make_date(d)); }
            case 0x4: {
                if (!count(at, m, c) || at + c > n) return nullptr;
                id d = data_new(p + at, c);
                if (mut) { id md = send(d, "mutableCopy"); release(d); return md; }
                return d;
            }
            case 0x5: {
                if (!count(at, m, c) || at + c > n) return nullptr;
                std::u16string s(c, u'\0');
                for (uint64_t i = 0; i < c; ++i) s[i] = p[at + i];
                return str16_new(s, mut);
            }
            case 0x6: {
                if (!count(at, m, c) || at + c * 2 > n) return nullptr;
                std::u16string s(c, u'\0');
                for (uint64_t i = 0; i < c; ++i) s[i] = static_cast<char16_t>(p[at + 2 * i] << 8 | p[at + 2 * i + 1]);
                return str16_new(s, mut);
            }
            case 0x8: {
                uint64_t v = be(at, (m & 0xf) + 1);
                id d = dict_new(false);
                dict_set(d, "CF$UID", num_i(static_cast<int64_t>(v)));
                return d;
            }
            case 0xA: case 0xC: {
                if (!count(at, m, c)) return nullptr;
                std::vector<id> items;
                for (uint64_t i = 0; i < c; ++i) {
                    id o = object(be(at + i * ref_size, ref_size));
                    if (o) items.push_back(o);
                }
                id a;
                if ((m >> 4) == 0xC) {
                    a = send_cls(class_named(mut ? "NSMutableSet" : "NSSet"), "setWithArray:", arr(items));
                    retain(a);
                } else {
                    a = arr_new(items, mut);
                }
                for (id o : items) release(o);
                return a;
            }
            case 0xD: {
                if (!count(at, m, c)) return nullptr;
                id d = dict_new(mut);
                for (uint64_t i = 0; i < c; ++i) {
                    id k = object(be(at + i * ref_size, ref_size));
                    id v = object(be(at + (c + i) * ref_size, ref_size));
                    if (k && v) dict_set(d, k, v);
                    release(k);
                    release(v);
                }
                return d;
            }
            default:
                return nullptr;
        }
    }
};

id parse_binary(const uint8_t* p, size_t n, bool mut) {
    if (n < 40) return nullptr;
    BReader r{p, n};
    const uint8_t* t = p + n - 32;
    r.off_size = t[6];
    r.ref_size = t[7];
    r.num = r.be(n - 24, 8);
    r.top = r.be(n - 16, 8);
    r.table = r.be(n - 8, 8);
    r.mut = mut;
    if (!r.off_size || !r.ref_size || r.table >= n) return nullptr;
    return r.object(r.top);
}

// ---- XML ----
struct XReader {
    const std::string& s;
    size_t i = 0;
    bool mut;
    void ws() {
        while (i < s.size()) {
            if (isspace(static_cast<unsigned char>(s[i]))) { ++i; continue; }
            if (s.compare(i, 4, "<!--") == 0) { size_t e = s.find("-->", i); i = e == std::string::npos ? s.size() : e + 3; continue; }
            if (s.compare(i, 2, "<?") == 0) { size_t e = s.find("?>", i); i = e == std::string::npos ? s.size() : e + 2; continue; }
            if (s.compare(i, 9, "<!DOCTYPE") == 0) { size_t e = s.find('>', i); i = e == std::string::npos ? s.size() : e + 1; continue; }
            break;
        }
    }
    // reads "<name ...>" or "<name/>", returns name; self_close set
    bool tag(std::string& name, bool& self_close, bool& closing) {
        ws();
        if (i >= s.size() || s[i] != '<') return false;
        size_t e = s.find('>', i);
        if (e == std::string::npos) return false;
        std::string inner = s.substr(i + 1, e - i - 1);
        i = e + 1;
        closing = !inner.empty() && inner[0] == '/';
        if (closing) inner.erase(0, 1);
        self_close = !inner.empty() && inner.back() == '/';
        if (self_close) inner.pop_back();
        size_t sp = inner.find_first_of(" \t\r\n");
        name = inner.substr(0, sp);
        return true;
    }
    std::string text_until_close(const std::string& name) {
        std::string end = "</" + name + ">";
        size_t e = s.find(end, i);
        if (e == std::string::npos) e = s.size();
        std::string raw = s.substr(i, e - i);
        i = e == s.size() ? e : e + end.size();
        std::string o;
        for (size_t k = 0; k < raw.size(); ++k) {
            if (raw[k] != '&') { o.push_back(raw[k]); continue; }
            size_t sc = raw.find(';', k);
            if (sc == std::string::npos) { o.push_back('&'); continue; }
            std::string ent = raw.substr(k + 1, sc - k - 1);
            if (ent == "lt") o += '<';
            else if (ent == "gt") o += '>';
            else if (ent == "amp") o += '&';
            else if (ent == "quot") o += '"';
            else if (ent == "apos") o += '\'';
            else if (!ent.empty() && ent[0] == '#') {
                uint32_t cp = ent.size() > 1 && ent[1] == 'x' ? static_cast<uint32_t>(strtoul(ent.c_str() + 2, nullptr, 16)) : static_cast<uint32_t>(strtoul(ent.c_str() + 1, nullptr, 10));
                std::u16string u(1, static_cast<char16_t>(cp));
                o += to8(u);
            }
            k = sc;
        }
        return o;
    }
    id value(int depth = 0) {
        if (depth > 256) return nullptr;
        std::string name;
        bool sc, cl;
        if (!tag(name, sc, cl) || cl) return nullptr;
        if (name == "plist") { id v = value(depth + 1); std::string n2; bool a, b; tag(n2, a, b); return v; }
        if (name == "dict") {
            id d = dict_new(mut);
            if (sc) return d;
            while (true) {
                size_t save = i;
                std::string k;
                bool ksc, kcl;
                if (!tag(k, ksc, kcl)) break;
                if (kcl) break;   // </dict>
                if (k != "key") { i = save; id junk = value(depth + 1); release(junk); continue; }
                std::string key = ksc ? "" : text_until_close("key");
                id v = value(depth + 1);
                if (v) { id ks = str_new(key); dict_set(d, ks, v); release(ks); release(v); }
            }
            return d;
        }
        if (name == "array") {
            std::vector<id> items;
            if (!sc) {
                while (true) {
                    size_t save = i;
                    std::string n2;
                    bool a, cl2;
                    if (!tag(n2, a, cl2)) break;
                    if (cl2) break;
                    i = save;
                    id v = value(depth + 1);
                    if (!v) break;
                    items.push_back(v);
                }
            }
            id a = arr_new(items, mut);
            for (id o : items) release(o);
            return a;
        }
        if (name == "true") { if (!sc) text_until_close(name); return retain(num_b(true)); }
        if (name == "false") { if (!sc) text_until_close(name); return retain(num_b(false)); }
        std::string t = sc ? "" : text_until_close(name);
        if (name == "string") return str16_new(to16(t), mut);
        if (name == "integer") return retain(t.find_first_of("-") == std::string::npos && t.size() > 18 ? num_u(strtoull(t.c_str(), nullptr, 10)) : num_i(strtoll(t.c_str(), nullptr, 10)));
        if (name == "real") return retain(num_d(strtod(t.c_str(), nullptr)));
        if (name == "date") {
            struct tm tmv{};
            if (sscanf(t.c_str(), "%d-%d-%dT%d:%d:%d", &tmv.tm_year, &tmv.tm_mon, &tmv.tm_mday, &tmv.tm_hour, &tmv.tm_min, &tmv.tm_sec) >= 3) {
                tmv.tm_year -= 1900; tmv.tm_mon -= 1;
                return retain(make_date(static_cast<double>(timegm(&tmv)) - 978307200.0));
            }
            return retain(make_date(0));
        }
        if (name == "data") {
            static const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::vector<uint8_t> out;
            uint32_t acc = 0;
            int bits = 0;
            for (char c : t) {
                size_t k = chars.find(c);
                if (k == std::string::npos) continue;
                acc = (acc << 6) | static_cast<uint32_t>(k);
                bits += 6;
                if (bits >= 8) { bits -= 8; out.push_back(static_cast<uint8_t>(acc >> bits)); }
            }
            id d = data_new(out.data(), out.size());
            if (mut) { id md = send(d, "mutableCopy"); release(d); return md; }
            return d;
        }
        return nullptr;
    }
};

// ---- old-style ASCII plists (.strings files) ----
struct AReader {
    const std::u16string& s;
    size_t i = 0;
    bool mut;
    void ws() {
        while (i < s.size()) {
            char16_t c = s[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0xfeff) { ++i; continue; }
            if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') { while (i < s.size() && s[i] != '\n') ++i; continue; }
            if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') { size_t e = s.find(u"*/", i + 2); i = e == std::u16string::npos ? s.size() : e + 2; continue; }
            break;
        }
    }
    id string() {
        ws();
        if (i >= s.size()) return nullptr;
        std::u16string out;
        if (s[i] == '"') {
            ++i;
            while (i < s.size() && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < s.size()) {
                    char16_t e = s[++i];
                    switch (e) {
                        case 'n': out.push_back('\n'); break;
                        case 't': out.push_back('\t'); break;
                        case 'r': out.push_back('\r'); break;
                        case 'U': case 'u': {
                            uint32_t v = 0;
                            for (int k = 0; k < 4 && i + 1 < s.size(); ++k) { char16_t h = s[++i]; v = v * 16 + static_cast<uint32_t>(h < 128 && isdigit(h) ? h - '0' : (tolower(h) - 'a' + 10)); }
                            out.push_back(static_cast<char16_t>(v));
                            break;
                        }
                        default: out.push_back(e); break;
                    }
                    ++i;
                    continue;
                }
                out.push_back(s[i++]);
            }
            ++i;
        } else {
            while (i < s.size() && (isalnum(s[i] < 128 ? s[i] : 'a') || s[i] == '_' || s[i] == '.' || s[i] == '$' || s[i] == '/' || s[i] == ':' || s[i] == '-'))
                out.push_back(s[i++]);
            if (out.empty()) return nullptr;
        }
        return str16_new(out, mut);
    }
    id value(int depth = 0) {
        ws();
        if (i >= s.size() || depth > 128) return nullptr;
        if (s[i] == '{') {
            ++i;
            id d = dict_new(mut);
            while (true) {
                ws();
                if (i >= s.size()) break;
                if (s[i] == '}') { ++i; break; }
                id k = string();
                if (!k) break;
                ws();
                if (i < s.size() && s[i] == '=') ++i;
                id v = value(depth + 1);
                ws();
                if (i < s.size() && s[i] == ';') ++i;
                if (v) dict_set(d, k, v);
                release(k);
                release(v);
            }
            return d;
        }
        if (s[i] == '(') {
            ++i;
            std::vector<id> items;
            while (true) {
                ws();
                if (i >= s.size()) break;
                if (s[i] == ')') { ++i; break; }
                id v = value(depth + 1);
                if (!v) break;
                items.push_back(v);
                ws();
                if (i < s.size() && s[i] == ',') ++i;
            }
            id a = arr_new(items, mut);
            for (id o : items) release(o);
            return a;
        }
        return string();
    }
    // a .strings file: a dictionary body without braces
    id strings_file() {
        id d = dict_new(mut);
        while (true) {
            ws();
            if (i >= s.size()) break;
            size_t before = i;
            id k = string();
            if (!k) { if (i == before) break; continue; }
            ws();
            id v = nullptr;
            if (i < s.size() && s[i] == '=') { ++i; v = value(); }
            ws();
            if (i < s.size() && s[i] == ';') ++i;
            if (v) dict_set(d, k, v);
            release(k);
            release(v);
        }
        return d;
    }
};

// ---- writers ----
void xml_escape(const std::string& s, std::string& o) {
    for (char c : s) {
        switch (c) {
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '&': o += "&amp;"; break;
            default: o.push_back(c);
        }
    }
}
std::string b64(const uint8_t* p, size_t n) {
    static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = p[i] << 16 | (i + 1 < n ? p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        o.push_back(chars[(v >> 18) & 63]);
        o.push_back(chars[(v >> 12) & 63]);
        o.push_back(i + 1 < n ? chars[(v >> 6) & 63] : '=');
        o.push_back(i + 2 < n ? chars[v & 63] : '=');
    }
    return o;
}
bool is_bool_number(id o) { return is_number(o) && (o == num_b(true) || o == num_b(false)); }
bool is_float_number(id o) {
    if (!is_number(o)) return false;
    const char* t = send<const char*>(o, "objCType");
    return t && (t[0] == 'f' || t[0] == 'd');
}
bool write_xml_value(id o, std::string& out, int indent) {
    std::string pad(indent, '\t');
    if (!o) return false;
    if (is_string(o)) { out += pad + "<string>"; xml_escape(utf8(o), out); out += "</string>\n"; return true; }
    if (is_bool_number(o)) { out += pad + (num_ival(o) ? "<true/>\n" : "<false/>\n"); return true; }
    if (is_number(o)) {
        char b[64];
        if (is_float_number(o)) snprintf(b, sizeof b, "<real>%.17g</real>", num_dval(o));
        else snprintf(b, sizeof b, "<integer>%lld</integer>", static_cast<long long>(num_ival(o)));
        out += pad + b + "\n";
        return true;
    }
    if (is_data(o)) { out += pad + "<data>" + b64(data_bytes(o), data_len(o)) + "</data>\n"; return true; }
    if (is_kind(o, "NSDate")) {
        time_t t = static_cast<time_t>(date_ref(o) + 978307200.0);
        struct tm g;
        gmtime_r(&t, &g);
        char b[64];
        strftime(b, sizeof b, "<date>%Y-%m-%dT%H:%M:%SZ</date>", &g);
        out += pad + b + "\n";
        return true;
    }
    if (is_array(o)) {
        out += pad + "<array>\n";
        for (id x : arr_items(o)) write_xml_value(x, out, indent + 1);
        out += pad + "</array>\n";
        return true;
    }
    if (is_dict(o)) {
        out += pad + "<dict>\n";
        for (auto& kv : dict_entries(o)) {
            out += pad + "\t<key>";
            xml_escape(describe(kv.first), out);
            out += "</key>\n";
            if (!write_xml_value(kv.second, out, indent + 1)) out += pad + "\t<string></string>\n";
        }
        out += pad + "</dict>\n";
        return true;
    }
    if (is_kind(o, "NSSet")) return write_xml_value(send(o, "allObjects"), out, indent);
    log("plist: cannot write %s", class_name(class_of(o)));
    return false;
}

struct BWriter {
    std::vector<std::string> objs;
    std::map<std::string, uint64_t> uniq;   // de-duplicates scalars
    unsigned ref_size = 2;
    static void count(std::string& s, uint8_t type, uint64_t n) {
        if (n < 15) { s.push_back(static_cast<char>(type << 4 | n)); return; }
        s.push_back(static_cast<char>(type << 4 | 0xf));
        if (n < 256) { s.push_back(0x10); s.push_back(static_cast<char>(n)); }
        else if (n < 65536) { s.push_back(0x11); s.push_back(static_cast<char>(n >> 8)); s.push_back(static_cast<char>(n)); }
        else { s.push_back(0x12); for (int k = 3; k >= 0; --k) s.push_back(static_cast<char>(n >> (8 * k))); }
    }
    static void be(std::string& s, uint64_t v, unsigned bytes) { for (int k = bytes - 1; k >= 0; --k) s.push_back(static_cast<char>(v >> (8 * k))); }
    uint64_t add_scalar(std::string enc) {
        auto it = uniq.find(enc);
        if (it != uniq.end()) return it->second;
        objs.push_back(enc);
        uniq[enc] = objs.size() - 1;
        return objs.size() - 1;
    }
    int64_t add(id o, int depth) {
        if (!o || depth > 256) return -1;
        if (is_string(o)) {
            const auto& u = u16(o);
            bool ascii = true;
            for (char16_t c : u) if (c > 127) { ascii = false; break; }
            std::string s;
            if (ascii) { count(s, 5, u.size()); for (char16_t c : u) s.push_back(static_cast<char>(c)); }
            else { count(s, 6, u.size()); for (char16_t c : u) { s.push_back(static_cast<char>(c >> 8)); s.push_back(static_cast<char>(c)); } }
            return static_cast<int64_t>(add_scalar(s));
        }
        if (is_bool_number(o)) return static_cast<int64_t>(add_scalar(std::string(1, num_ival(o) ? '\x09' : '\x08')));
        if (is_number(o)) {
            std::string s;
            if (is_float_number(o)) { s.push_back(0x23); double d = num_dval(o); uint64_t v; std::memcpy(&v, &d, 8); be(s, v, 8); }
            else { s.push_back(0x13); be(s, static_cast<uint64_t>(num_ival(o)), 8); }
            return static_cast<int64_t>(add_scalar(s));
        }
        if (is_data(o)) { std::string s; count(s, 4, data_len(o)); s.append(reinterpret_cast<const char*>(data_bytes(o)), data_len(o)); return static_cast<int64_t>(add_scalar(s)); }
        if (is_kind(o, "NSDate")) { std::string s; s.push_back(0x33); double d = date_ref(o); uint64_t v; std::memcpy(&v, &d, 8); be(s, v, 8); return static_cast<int64_t>(add_scalar(s)); }
        if (is_kind(o, "NSSet")) return add(send(o, "allObjects"), depth);
        if (is_array(o)) {
            size_t me = objs.size();
            objs.emplace_back();
            std::vector<uint64_t> refs;
            for (id x : arr_items(o)) { int64_t r = add(x, depth + 1); if (r >= 0) refs.push_back(static_cast<uint64_t>(r)); }
            std::string s;
            count(s, 0xA, refs.size());
            for (uint64_t r : refs) be(s, r, ref_size);
            objs[me] = s;
            return static_cast<int64_t>(me);
        }
        if (is_dict(o)) {
            size_t me = objs.size();
            objs.emplace_back();
            std::vector<uint64_t> ks, vs;
            for (auto& kv : dict_entries(o)) {
                int64_t k = add(is_string(kv.first) ? kv.first : str(describe(kv.first)), depth + 1);
                int64_t v = add(kv.second, depth + 1);
                if (k >= 0 && v >= 0) { ks.push_back(static_cast<uint64_t>(k)); vs.push_back(static_cast<uint64_t>(v)); }
            }
            std::string s;
            count(s, 0xD, ks.size());
            for (uint64_t r : ks) be(s, r, ref_size);
            for (uint64_t r : vs) be(s, r, ref_size);
            objs[me] = s;
            return static_cast<int64_t>(me);
        }
        log("bplist: cannot write %s", class_name(class_of(o)));
        return -1;
    }
};

// ---- JSON ----
struct JReader {
    const char* p;
    const char* e;
    int depth = 0;
    void ws() { while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }
    id value() {
        ws();
        if (p >= e || ++depth > 512) return nullptr;
        id r = nullptr;
        if (*p == '{') {
            ++p;
            r = dict_new(true);
            ws();
            if (p < e && *p == '}') { ++p; --depth; return r; }
            while (p < e) {
                ws();
                id k = string();
                ws();
                if (!k || p >= e || *p != ':') { release(k); release(r); --depth; return nullptr; }
                ++p;
                id v = value();
                if (!v) { release(k); release(r); --depth; return nullptr; }
                dict_set(r, k, v);
                release(k);
                release(v);
                ws();
                if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == '}') { ++p; break; }
                release(r);
                --depth;
                return nullptr;
            }
        } else if (*p == '[') {
            ++p;
            std::vector<id> items;
            ws();
            if (p < e && *p == ']') { ++p; }
            else {
                while (p < e) {
                    id v = value();
                    if (!v) { for (id o : items) release(o); --depth; return nullptr; }
                    items.push_back(v);
                    ws();
                    if (p < e && *p == ',') { ++p; continue; }
                    if (p < e && *p == ']') { ++p; break; }
                    for (id o : items) release(o);
                    --depth;
                    return nullptr;
                }
            }
            r = arr_new(items, true);
            for (id o : items) release(o);
        } else if (*p == '"') {
            r = string();
        } else if (e - p >= 4 && std::strncmp(p, "true", 4) == 0) { p += 4; r = retain(num_b(true)); }
        else if (e - p >= 5 && std::strncmp(p, "false", 5) == 0) { p += 5; r = retain(num_b(false)); }
        else if (e - p >= 4 && std::strncmp(p, "null", 4) == 0) { p += 4; r = retain(send_cls(class_named("NSNull"), "null")); }
        else {
            const char* st = p;
            bool flt = false;
            while (p < e && (isdigit(static_cast<unsigned char>(*p)) || *p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E')) {
                if (*p == '.' || *p == 'e' || *p == 'E') flt = true;
                ++p;
            }
            if (p == st) { --depth; return nullptr; }
            std::string num(st, p);
            r = retain(flt ? num_d(strtod(num.c_str(), nullptr)) : num_i(strtoll(num.c_str(), nullptr, 10)));
        }
        --depth;
        return r;
    }
    id string() {
        if (p >= e || *p != '"') return nullptr;
        ++p;
        std::u16string out;
        std::string run;
        auto flush = [&] { if (!run.empty()) { out += to16(run); run.clear(); } };
        while (p < e && *p != '"') {
            if (*p == '\\' && p + 1 < e) {
                flush();
                char c = *++p;
                ++p;
                switch (c) {
                    case 'n': out.push_back('\n'); break;
                    case 't': out.push_back('\t'); break;
                    case 'r': out.push_back('\r'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'u': {
                        uint32_t v = 0;
                        for (int k = 0; k < 4 && p < e; ++k, ++p) v = v * 16 + static_cast<uint32_t>(isdigit(static_cast<unsigned char>(*p)) ? *p - '0' : (tolower(*p) - 'a' + 10));
                        out.push_back(static_cast<char16_t>(v));
                        break;
                    }
                    default: out.push_back(static_cast<char16_t>(c)); break;
                }
                continue;
            }
            run.push_back(*p++);
        }
        flush();
        if (p < e) ++p;
        return str16_new(out, true);
    }
};

void json_string(const std::string& s, std::string& out) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '/': out += "\\/"; break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; }
                else out.push_back(static_cast<char>(c));
        }
    }
    out.push_back('"');
}
bool json_value(id o, std::string& out, bool pretty, int indent) {
    std::string nl = pretty ? "\n" : "", pad = pretty ? std::string(indent * 2, ' ') : "", pad2 = pretty ? std::string((indent + 1) * 2, ' ') : "";
    if (!o || is_kind(o, "NSNull")) { out += "null"; return true; }
    if (is_string(o)) { json_string(utf8(o), out); return true; }
    if (is_bool_number(o)) { out += num_ival(o) ? "true" : "false"; return true; }
    if (is_number(o)) {
        char b[64];
        if (is_float_number(o)) snprintf(b, sizeof b, "%.17g", num_dval(o));
        else snprintf(b, sizeof b, "%lld", static_cast<long long>(num_ival(o)));
        out += b;
        return true;
    }
    if (is_array(o)) {
        out += "[" + nl;
        const auto& v = arr_items(o);
        for (size_t i = 0; i < v.size(); ++i) {
            out += pad2;
            if (!json_value(v[i], out, pretty, indent + 1)) return false;
            if (i + 1 < v.size()) out += ",";
            out += nl;
        }
        out += pad + "]";
        return true;
    }
    if (is_dict(o)) {
        out += "{" + nl;
        auto v = dict_entries(o);
        for (size_t i = 0; i < v.size(); ++i) {
            out += pad2;
            json_string(describe(v[i].first), out);
            out += pretty ? " : " : ":";
            if (!json_value(v[i].second, out, pretty, indent + 1)) return false;
            if (i + 1 < v.size()) out += ",";
            out += nl;
        }
        out += pad + "}";
        return true;
    }
    return false;
}

}  // namespace

id plist_parse(const uint8_t* p, size_t n, bool mut) {
    if (!p || !n) return nullptr;
    if (n >= 8 && std::memcmp(p, "bplist00", 8) == 0) return parse_binary(p, n, mut);
    std::string s(reinterpret_cast<const char*>(p), n);
    // UTF-16 text (.strings files are often UTF-16)
    bool utf16 = n >= 2 && ((p[0] == 0xff && p[1] == 0xfe) || (p[0] == 0xfe && p[1] == 0xff));
    if (!utf16) {
        size_t k = 0;
        while (k < s.size() && isspace(static_cast<unsigned char>(s[k]))) ++k;
        if (s.compare(k, 5, "<?xml") == 0 || s.compare(k, 6, "<plist") == 0 || s.compare(k, 9, "<!DOCTYPE") == 0) {
            XReader r{s, 0, mut};
            return r.value();
        }
    }
    std::u16string u;
    if (utf16) {
        bool le = p[0] == 0xff;
        for (size_t i = 2; i + 1 < n; i += 2) u.push_back(static_cast<char16_t>(le ? (p[i] | p[i + 1] << 8) : (p[i] << 8 | p[i + 1])));
    } else {
        u = to16(s);
    }
    AReader a{u, 0, mut};
    a.ws();
    if (a.i < u.size() && (u[a.i] == '{' || u[a.i] == '(')) return a.value();
    return a.strings_file();
}

bool plist_write_xml(id root, std::string& out) {
    out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
          "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
          "<plist version=\"1.0\">\n";
    if (!write_xml_value(root, out, 0)) return false;
    out += "</plist>\n";
    return true;
}

bool plist_write_binary(id root, std::string& out) {
    BWriter w;
    w.ref_size = 4;
    if (w.add(root, 0) < 0) return false;
    out = "bplist00";
    std::vector<uint64_t> offsets;
    for (auto& o : w.objs) { offsets.push_back(out.size()); out += o; }
    uint64_t table = out.size();
    for (uint64_t off : offsets) BWriter::be(out, off, 8);
    std::string trailer(6, '\0');
    trailer.push_back(8);
    trailer.push_back(static_cast<char>(w.ref_size));
    BWriter::be(trailer, w.objs.size(), 8);
    BWriter::be(trailer, 0, 8);
    BWriter::be(trailer, table, 8);
    out += trailer;
    return true;
}

id json_parse(const uint8_t* p, size_t n) {
    if (!p || !n) return nullptr;
    JReader r{reinterpret_cast<const char*>(p), reinterpret_cast<const char*>(p) + n};
    return r.value();
}

bool json_write(id root, std::string& out, bool pretty) {
    out.clear();
    return json_value(root, out, pretty, 0);
}

}  // namespace app
