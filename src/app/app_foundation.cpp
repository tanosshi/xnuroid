// App mode Foundation, part 1: NSString (+ constant CFStrings), formatting, NSNumber/NSValue, NSNull, NSArray,
// NSDictionary, NSSet, NSData, NSCharacterSet and enumerators.
#include "app.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <unordered_set>

namespace app {

std::string default_description(id o);   // app_runtime.cpp
id block_copy(id b);
void block_release(id b);
void* block_invoke_ptr(id b);
void declare_protocol(Class c, const char* proto);

static Class C_NSString, C_NSMutableString, C_ConstStr, C_NSNumber, C_NSValue, C_NSNull, C_NSArray, C_NSMutableArray,
    C_NSDictionary, C_NSMutableDictionary, C_NSSet, C_NSMutableSet, C_NSData, C_NSMutableData, C_NSCharacterSet,
    C_NSEnumerator, C_NSDecimalNumber, C_NSCountedSet;

// ---------------------------------------------------------------------------------------------------------------
// UTF-8 / UTF-16
// ---------------------------------------------------------------------------------------------------------------
std::u16string to16(const std::string& s) {
    std::u16string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        uint32_t c = static_cast<uint8_t>(s[i]);
        int n = 0;
        if (c < 0x80) n = 0;
        else if ((c >> 5) == 6) { c &= 0x1f; n = 1; }
        else if ((c >> 4) == 14) { c &= 0x0f; n = 2; }
        else if ((c >> 3) == 30) { c &= 0x07; n = 3; }
        else { out.push_back(0xfffd); ++i; continue; }
        if (n && i + n >= s.size()) { out.push_back(0xfffd); break; }
        bool ok = true;
        for (int k = 1; k <= n; ++k) {
            uint8_t cc = static_cast<uint8_t>(s[i + k]);
            if ((cc >> 6) != 2) { ok = false; break; }
            c = (c << 6) | (cc & 0x3f);
        }
        if (!ok) { out.push_back(0xfffd); ++i; continue; }
        i += n + 1;
        if (c >= 0x10000) {
            c -= 0x10000;
            out.push_back(static_cast<char16_t>(0xd800 + (c >> 10)));
            out.push_back(static_cast<char16_t>(0xdc00 + (c & 0x3ff)));
        } else {
            out.push_back(static_cast<char16_t>(c));
        }
    }
    return out;
}

std::string to8(const std::u16string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        uint32_t c = s[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < s.size() && s[i + 1] >= 0xdc00 && s[i + 1] < 0xe000) {
            c = 0x10000 + ((c - 0xd800) << 10) + (s[i + 1] - 0xdc00);
            ++i;
        }
        if (c < 0x80) out.push_back(static_cast<char>(c));
        else if (c < 0x800) { out.push_back(static_cast<char>(0xc0 | (c >> 6))); out.push_back(static_cast<char>(0x80 | (c & 0x3f))); }
        else if (c < 0x10000) {
            out.push_back(static_cast<char>(0xe0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xf0 | (c >> 18)));
            out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
        }
    }
    return out;
}

static std::u16string latin1_to16(const char* p, size_t n) {
    std::u16string s(n, u'\0');
    for (size_t i = 0; i < n; ++i) s[i] = static_cast<uint8_t>(p[i]);
    return s;
}

// decodes bytes in an NSStringEncoding
static bool decode_bytes(const void* bytes, size_t n, NSUInteger enc, std::u16string& out) {
    const char* p = static_cast<const char*>(bytes);
    switch (enc) {
        case 1: case 4: case 7: case 0:   // ASCII, UTF-8, NonLossyASCII
            out = to16(std::string(p, n));
            return true;
        case 5: case 12: case 30: case 3:   // Latin-1, CP1252, MacRoman, JapaneseEUC(approx)
            out = latin1_to16(p, n);
            return true;
        case 10: case 0x90000100: case 0x94000100: {   // UTF-16 (BOM), BE, LE
            bool le = enc != 0x90000100;
            size_t i = 0;
            if (n >= 2 && enc == 10) {
                auto b0 = static_cast<uint8_t>(p[0]), b1 = static_cast<uint8_t>(p[1]);
                if (b0 == 0xff && b1 == 0xfe) { le = true; i = 2; }
                else if (b0 == 0xfe && b1 == 0xff) { le = false; i = 2; }
            }
            out.clear();
            for (; i + 1 < n; i += 2) {
                auto a = static_cast<uint8_t>(p[i]), b = static_cast<uint8_t>(p[i + 1]);
                out.push_back(static_cast<char16_t>(le ? (a | (b << 8)) : ((a << 8) | b)));
            }
            return true;
        }
        default:
            out = to16(std::string(p, n));
            return true;
    }
}

static std::string encode_string(const std::u16string& s, NSUInteger enc, bool* lossy = nullptr) {
    switch (enc) {
        case 1: case 5: case 12: case 30: {
            std::string o;
            for (char16_t c : s) {
                unsigned lim = enc == 1 ? 0x80 : 0x100;
                if (c < lim) o.push_back(static_cast<char>(c));
                else { o.push_back('?'); if (lossy) *lossy = true; }
            }
            return o;
        }
        case 10: case 0x94000100: case 0x90000100: {
            std::string o;
            if (enc == 10) { o.push_back('\xff'); o.push_back('\xfe'); }
            for (char16_t c : s) {
                if (enc == 0x90000100) { o.push_back(static_cast<char>(c >> 8)); o.push_back(static_cast<char>(c)); }
                else { o.push_back(static_cast<char>(c)); o.push_back(static_cast<char>(c >> 8)); }
            }
            return o;
        }
        default:
            return to8(s);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// string storage
// ---------------------------------------------------------------------------------------------------------------
struct StrExt : Ext {
    std::u16string s;
    std::string u8;
    bool u8_valid = false;
    std::vector<std::string*> old_u8;   // UTF8String results stay valid while the string lives
    ~StrExt() override { for (auto* p : old_u8) delete p; }
};

static std::mutex g_const_mu;
static std::unordered_map<const void*, StrExt*>* g_const_strs;
static const std::u16string kEmpty16;

static bool is_const_string(id o) { return o && o->isa == C_ConstStr; }

static StrExt* const_ext(id o) {
    std::lock_guard<std::mutex> l(g_const_mu);
    if (!g_const_strs) g_const_strs = new std::unordered_map<const void*, StrExt*>();
    auto it = g_const_strs->find(o);
    if (it != g_const_strs->end()) return it->second;
    struct CFConst { uintptr_t isa; uint32_t flags, pad; const void* data; uint64_t len; };
    auto* c = reinterpret_cast<CFConst*>(o);
    auto* e = new StrExt();
    if (c->flags & 0x10) {
        auto* u = static_cast<const char16_t*>(c->data);
        e->s.assign(u, u + c->len);
    } else {
        e->s = to16(std::string(static_cast<const char*>(c->data), c->len));
    }
    (*g_const_strs)[o] = e;
    return e;
}

static StrExt* sx(id o) {
    if (!o) return nullptr;
    if (is_const_string(o)) return const_ext(o);
    return ext_get<StrExt>(o);
}

bool is_string(id o) {
    if (!o) return false;
    Class c = o->isa;
    if (c == C_NSString || c == C_NSMutableString || c == C_ConstStr) return true;
    return !is_class_object(o) && is_subclass(c, C_NSString);
}

const std::u16string& u16(id o) {
    if (!is_string(o)) return kEmpty16;
    StrExt* e = sx(o);
    return e ? e->s : kEmpty16;
}

std::string utf8(id o) { return is_string(o) ? to8(u16(o)) : std::string(); }

const char* cstr(id o) {
    StrExt* e = is_string(o) ? sx(o) : nullptr;
    if (!e) return nullptr;
    if (!e->u8_valid) {
        if (!e->u8.empty() || e->u8_valid) e->old_u8.push_back(new std::string(e->u8));
        e->u8 = to8(e->s);
        e->u8_valid = true;
    }
    return e->u8.c_str();
}

static void str_set(id o, std::u16string s) {
    StrExt* e = sx(o);
    if (!e) return;
    if (e->u8_valid) { e->old_u8.push_back(new std::string(std::move(e->u8))); e->u8.clear(); }
    e->u8_valid = false;
    e->s = std::move(s);
}

id str16_new(const std::u16string& s, bool mutable_) {
    id o = alloc(mutable_ ? C_NSMutableString : C_NSString);
    ext_get<StrExt>(o)->s = s;
    return o;
}
id str_new(const std::string& s) { return str16_new(to16(s)); }
id str(const std::string& s) { return autorelease(str_new(s)); }
id str16(const std::u16string& s) { return autorelease(str16_new(s)); }

static size_t hash16(const std::u16string& s) {
    uint64_t h = 1469598103934665603ull;
    for (char16_t c : s) { h ^= c; h *= 1099511628211ull; }
    return static_cast<size_t>(h);
}

// ---------------------------------------------------------------------------------------------------------------
// numbers
// ---------------------------------------------------------------------------------------------------------------
struct NumExt : Ext {
    char type = 'q';    // c B s i l q C S I L Q f d
    int64_t i = 0;
    double d = 0;
    bool is_float() const { return type == 'f' || type == 'd'; }
    bool is_unsigned() const { return type == 'C' || type == 'S' || type == 'I' || type == 'L' || type == 'Q'; }
};

bool is_number(id o) { return o && !is_class_object(o) && (o->isa == C_NSNumber || is_subclass(o->isa, C_NSNumber)); }

static id g_true, g_false;

static id num_make(char t, int64_t i, double d) {
    id o = alloc(C_NSNumber);
    auto* e = ext_get<NumExt>(o);
    e->type = t;
    if (t == 'f' || t == 'd') { e->d = d; e->i = static_cast<int64_t>(d); }
    else { e->i = i; e->d = t == 'Q' ? static_cast<double>(static_cast<uint64_t>(i)) : static_cast<double>(i); }
    return autorelease(o);
}
id num_i(int64_t v) { return num_make('q', v, 0); }
id num_u(uint64_t v) { return num_make('Q', static_cast<int64_t>(v), 0); }
id num_d(double v) { return num_make('d', 0, v); }
id num_f(float v) { return num_make('f', 0, v); }
id num_b(bool v) { return v ? g_true : g_false; }
static NumExt* nx(id o) { return is_number(o) ? ext_as<NumExt>(o) : nullptr; }
int64_t num_ival(id o) {
    if (NumExt* e = nx(o)) return e->is_float() ? static_cast<int64_t>(e->d) : e->i;
    if (is_string(o)) return strtoll(utf8(o).c_str(), nullptr, 10);
    return 0;
}
double num_dval(id o) {
    if (NumExt* e = nx(o)) return e->d;
    if (is_string(o)) return strtod(utf8(o).c_str(), nullptr);
    return 0;
}
static std::string num_string(NumExt* e) {
    char buf[64];
    if (e->type == 'c' || e->type == 'B') snprintf(buf, sizeof buf, "%d", static_cast<int>(e->i));
    else if (e->is_float()) {
        snprintf(buf, sizeof buf, "%.*g", e->type == 'f' ? 7 : 15, e->d);
    } else if (e->is_unsigned()) snprintf(buf, sizeof buf, "%llu", static_cast<unsigned long long>(e->i));
    else snprintf(buf, sizeof buf, "%lld", static_cast<long long>(e->i));
    return buf;
}

// ---------------------------------------------------------------------------------------------------------------
// values
// ---------------------------------------------------------------------------------------------------------------
struct ValExt : Ext { std::string type; std::vector<uint8_t> bytes; };
id value_bytes(const void* p, size_t n, const char* t) {
    id o = alloc(C_NSValue);
    auto* e = ext_get<ValExt>(o);
    e->type = t ? t : "";
    e->bytes.assign(static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
    return autorelease(o);
}
static ValExt* vx(id o) { return ext_as<ValExt>(o); }
template <class T> static T val_get(id o) {
    T t{};
    if (ValExt* e = vx(o)) std::memcpy(&t, e->bytes.data(), std::min(sizeof t, e->bytes.size()));
    return t;
}

// ---------------------------------------------------------------------------------------------------------------
// equality / hashing
// ---------------------------------------------------------------------------------------------------------------
bool obj_equal(id a, id b) {
    if (a == b) return true;
    if (!a || !b) return false;
    bool sa = is_string(a), sb = is_string(b);
    if (sa && sb) return u16(a) == u16(b);
    if (sa != sb && (sa || sb) && (is_number(a) || is_number(b))) return false;
    NumExt* na = nx(a);
    NumExt* nb = na ? nx(b) : nullptr;
    if (na && nb) {
        if (na->is_float() || nb->is_float()) return na->d == nb->d;
        return na->i == nb->i;
    }
    return send<bool>(a, "isEqual:", b);
}
size_t obj_hash(id a) {
    if (!a) return 0;
    if (is_string(a)) return hash16(u16(a));
    if (NumExt* n = nx(a)) return std::hash<double>()(n->d);
    return send<NSUInteger>(a, "hash");
}

// ---------------------------------------------------------------------------------------------------------------
// formatting
// ---------------------------------------------------------------------------------------------------------------
std::string describe(id o) {
    if (!o) return "(null)";
    if (is_string(o)) return utf8(o);
    return utf8(send(o, "description"));
}

std::string format(const std::string& fmt, const uint64_t* va) {
    std::string out;
    size_t next = 0;
    auto arg = [&](int pos) -> uint64_t { return va ? va[pos >= 0 ? pos : next++] : 0; };
    for (size_t i = 0; i < fmt.size(); ++i) {
        char ch = fmt[i];
        if (ch != '%') { out.push_back(ch); continue; }
        if (i + 1 < fmt.size() && fmt[i + 1] == '%') { out.push_back('%'); ++i; continue; }
        size_t j = i + 1;
        int pos = -1;
        {   // positional "%n$"
            size_t k = j;
            int n = 0;
            while (k < fmt.size() && isdigit(static_cast<unsigned char>(fmt[k]))) n = n * 10 + (fmt[k++] - '0');
            if (k < fmt.size() && fmt[k] == '$' && n > 0) { pos = n - 1; j = k + 1; }
        }
        std::string spec = "%";
        while (j < fmt.size() && std::strchr("-+ #0'", fmt[j])) { if (fmt[j] != '\'') spec.push_back(fmt[j]); ++j; }
        if (j < fmt.size() && fmt[j] == '*') { spec += std::to_string(static_cast<int>(arg(-1))); ++j; }
        while (j < fmt.size() && isdigit(static_cast<unsigned char>(fmt[j]))) spec.push_back(fmt[j++]);
        if (j < fmt.size() && fmt[j] == '.') {
            spec.push_back('.');
            ++j;
            if (j < fmt.size() && fmt[j] == '*') { spec += std::to_string(static_cast<int>(arg(-1))); ++j; }
            while (j < fmt.size() && isdigit(static_cast<unsigned char>(fmt[j]))) spec.push_back(fmt[j++]);
        }
        int lng = 0;   // 0 int, 1 long/long long, -1 short, -2 char
        while (j < fmt.size() && std::strchr("hlqLzjt", fmt[j])) {
            char l = fmt[j++];
            if (l == 'h') lng = lng == -1 ? -2 : -1;
            else lng = 1;
        }
        if (j >= fmt.size()) break;
        char conv = fmt[j];
        i = j;
        char buf[512];
        uint64_t v;
        switch (conv) {
            case '@': {
                id o = reinterpret_cast<id>(arg(pos));
                std::string d = describe(o);
                if (spec.size() > 1) {   // width/precision on %@
                    spec += "s";
                    snprintf(buf, sizeof buf, spec.c_str(), d.c_str());
                    out += buf;
                } else {
                    out += d;
                }
                break;
            }
            case 'd': case 'i': {
                v = arg(pos);
                long long x = lng == 1 ? static_cast<long long>(v) : lng == -1 ? static_cast<short>(v) : lng == -2 ? static_cast<signed char>(v) : static_cast<int>(v);
                spec += "lld";
                snprintf(buf, sizeof buf, spec.c_str(), x);
                out += buf;
                break;
            }
            case 'D': spec += "lld"; snprintf(buf, sizeof buf, spec.c_str(), static_cast<long long>(arg(pos))); out += buf; break;
            case 'u': case 'x': case 'X': case 'o': case 'U': case 'O': {
                v = arg(pos);
                unsigned long long x = lng == 1 || conv == 'U' || conv == 'O' ? v : lng == -1 ? static_cast<unsigned short>(v) : lng == -2 ? static_cast<unsigned char>(v) : static_cast<unsigned>(v);
                spec += "ll";
                spec.push_back(conv == 'U' ? 'u' : conv == 'O' ? 'o' : conv);
                snprintf(buf, sizeof buf, spec.c_str(), x);
                out += buf;
                break;
            }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
                v = arg(pos);
                double d;
                std::memcpy(&d, &v, 8);
                spec.push_back(conv);
                snprintf(buf, sizeof buf, spec.c_str(), d);
                out += buf;
                break;
            }
            case 'c': {
                spec += "c";
                snprintf(buf, sizeof buf, spec.c_str(), static_cast<int>(arg(pos)));
                out += buf;
                break;
            }
            case 'C': {
                std::u16string u(1, static_cast<char16_t>(arg(pos)));
                out += to8(u);
                break;
            }
            case 's': {
                auto* s = reinterpret_cast<const char*>(arg(pos));
                spec += "s";
                snprintf(buf, sizeof buf, spec.c_str(), s ? s : "(null)");
                out += buf;
                break;
            }
            case 'S': {
                auto* s = reinterpret_cast<const char16_t*>(arg(pos));
                if (s) { std::u16string u; while (*s) u.push_back(*s++); out += to8(u); }
                else out += "(null)";
                break;
            }
            case 'p': snprintf(buf, sizeof buf, "%p", reinterpret_cast<void*>(arg(pos))); out += buf; break;
            case 'n': arg(pos); break;
            default: out.push_back('%'); out.push_back(conv); break;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// collections storage
// ---------------------------------------------------------------------------------------------------------------
struct ArrExt : Ext {
    std::vector<id> v;
    unsigned long mutations = 0;
    ~ArrExt() override { for (id o : v) release(o); }
};

struct DictExt : Ext {
    struct E { id k, v; size_t h; bool live; };
    std::vector<E> e;
    std::unordered_multimap<size_t, size_t> idx;
    size_t live = 0;
    unsigned long mutations = 0;
    bool is_set = false;
    ~DictExt() override { for (auto& x : e) if (x.live) { release(x.k); if (!is_set) release(x.v); } }
    long find(id k, size_t h) const {
        auto r = idx.equal_range(h);
        for (auto it = r.first; it != r.second; ++it) {
            const E& x = e[it->second];
            if (x.live && obj_equal(x.k, k)) return static_cast<long>(it->second);
        }
        return -1;
    }
    id get(id k) const {
        if (!k) return nullptr;
        long i = find(k, obj_hash(k));
        return i < 0 ? nullptr : (is_set ? e[i].k : e[i].v);
    }
    void compact() {
        std::vector<E> ne;
        ne.reserve(live);
        for (auto& x : e) if (x.live) ne.push_back(x);
        e.swap(ne);
        idx.clear();
        for (size_t i = 0; i < e.size(); ++i) idx.emplace(e[i].h, i);
    }
    void remove(id k) {
        if (!k) return;
        size_t h = obj_hash(k);
        long i = find(k, h);
        if (i < 0) return;
        E x = e[i];
        e[i].live = false;
        auto r = idx.equal_range(h);
        for (auto it = r.first; it != r.second; ++it)
            if (it->second == static_cast<size_t>(i)) { idx.erase(it); break; }
        --live;
        ++mutations;
        release(x.k);
        if (!is_set) release(x.v);
        if (e.size() > 16 && live * 2 < e.size()) compact();
    }
    // takes ownership of k (+1) and v (+1)
    void put_owned(id k, id v) {
        size_t h = obj_hash(k);
        long i = find(k, h);
        ++mutations;
        if (i >= 0) {
            id ov = e[i].v;
            if (is_set) { release(k); return; }
            e[i].v = v;
            release(ov);
            release(k);
            return;
        }
        e.push_back({k, v, h, true});
        idx.emplace(h, e.size() - 1);
        ++live;
    }
    void clear() {
        for (auto& x : e) if (x.live) { release(x.k); if (!is_set) release(x.v); }
        e.clear();
        idx.clear();
        live = 0;
        ++mutations;
    }
    std::vector<id> keys() const { std::vector<id> k; for (auto& x : e) if (x.live) k.push_back(x.k); return k; }
    std::vector<id> values() const { std::vector<id> k; for (auto& x : e) if (x.live) k.push_back(x.v); return k; }
};

bool is_array(id o) { return o && !is_class_object(o) && is_subclass(o->isa, C_NSArray); }
bool is_dict(id o) { return o && !is_class_object(o) && is_subclass(o->isa, C_NSDictionary); }
static bool is_set_obj(id o) { return o && !is_class_object(o) && is_subclass(o->isa, C_NSSet); }
static ArrExt* ax(id o) { return ext_get<ArrExt>(o); }
static DictExt* dx(id o) { return ext_get<DictExt>(o); }
static DictExt* setx(id o) { DictExt* d = ext_get<DictExt>(o); if (d) d->is_set = true; return d; }

static const std::vector<id> kNoItems;
const std::vector<id>& arr_items(id a) { ArrExt* e = is_array(a) ? ax(a) : nullptr; return e ? e->v : kNoItems; }
id arr_new(const std::vector<id>& items, bool mutable_) {
    id a = alloc(mutable_ ? C_NSMutableArray : C_NSArray);
    ArrExt* e = ax(a);
    for (id o : items) if (o) e->v.push_back(retain(o));
    return a;
}
id arr(const std::vector<id>& items) { return autorelease(arr_new(items)); }
void arr_add(id a, id o) { if (ArrExt* e = ax(a)) { if (o) { e->v.push_back(retain(o)); e->mutations++; } } }

static id copy_key(id k) { return is_string(k) && k->isa == C_NSMutableString ? str16_new(u16(k)) : retain(k); }

id dict_new(bool mutable_) { id d = alloc(mutable_ ? C_NSMutableDictionary : C_NSDictionary); dx(d); return d; }
id dict_get(id d, id k) { DictExt* e = is_dict(d) ? dx(d) : nullptr; return e ? e->get(k) : nullptr; }
id dict_get(id d, const char* k) { void* t = pool_push(); id v = dict_get(d, str(k)); pool_pop(t); return v; }
void dict_set(id d, id k, id v) {
    DictExt* e = dx(d);
    if (!e || !k) return;
    if (!v) { e->remove(k); return; }
    e->put_owned(copy_key(k), retain(v));
}
void dict_set(id d, const char* k, id v) { id ks = str_new(k); dict_set(d, ks, v); release(ks); }
std::vector<std::pair<id, id>> dict_entries(id d) {
    std::vector<std::pair<id, id>> r;
    if (DictExt* e = is_dict(d) ? dx(d) : nullptr)
        for (auto& x : e->e) if (x.live) r.push_back({x.k, x.v});
    return r;
}

// ---------------------------------------------------------------------------------------------------------------
// data
// ---------------------------------------------------------------------------------------------------------------
struct DataExt : Ext {
    std::vector<uint8_t> v;
    uint8_t* borrowed = nullptr;
    size_t borrowed_len = 0;
    bool free_borrowed = false;
    ~DataExt() override { if (free_borrowed) free(borrowed); }
    uint8_t* ptr() { return borrowed ? borrowed : v.data(); }
    size_t len() const { return borrowed ? borrowed_len : v.size(); }
    void own() {
        if (!borrowed) return;
        v.assign(borrowed, borrowed + borrowed_len);
        if (free_borrowed) free(borrowed);
        borrowed = nullptr;
        free_borrowed = false;
    }
};
bool is_data(id o) { return o && !is_class_object(o) && is_subclass(o->isa, C_NSData); }
static DataExt* dtx(id o) { return ext_get<DataExt>(o); }
id data_new(const void* p, size_t n) {
    id d = alloc(C_NSData);
    if (p && n) dtx(d)->v.assign(static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
    else dtx(d);
    return d;
}
id data_new_nocopy(void* p, size_t n, bool fr) {
    id d = alloc(C_NSData);
    DataExt* e = dtx(d);
    e->borrowed = static_cast<uint8_t*>(p);
    e->borrowed_len = n;
    e->free_borrowed = fr;
    return d;
}
const uint8_t* data_bytes(id d) { DataExt* e = is_data(d) ? dtx(d) : nullptr; return e ? e->ptr() : nullptr; }
size_t data_len(id d) { DataExt* e = is_data(d) ? dtx(d) : nullptr; return e ? e->len() : 0; }

// ---------------------------------------------------------------------------------------------------------------
// helpers shared by the method tables
// ---------------------------------------------------------------------------------------------------------------
struct FastEnumState { unsigned long state; id* itemsPtr; unsigned long* mutationsPtr; unsigned long extra[5]; };

static NSUInteger fast_enum(const std::vector<id>& items, unsigned long* mutations, FastEnumState* st, id* buf, NSUInteger len) {
    if (st->state == 0) st->mutationsPtr = mutations;
    size_t start = st->state;
    if (start >= items.size()) return 0;
    NSUInteger n = 0;
    for (size_t i = start; i < items.size() && n < len; ++i) buf[n++] = items[i];
    st->state = start + n;
    st->itemsPtr = buf;
    return n;
}

typedef long (*Comparator)(id, id, void*);
static long call_block_cmp(id block, id a, id b) {
    return reinterpret_cast<long (*)(id, id, id)>(block_invoke_ptr(block))(block, a, b);
}

static id new_enumerator(std::vector<id> items);   // +1

static std::string indent_lines(const std::string& s) {
    std::string out;
    for (char c : s) { out.push_back(c); if (c == '\n') out += "    "; }
    return out;
}
static std::string quote_if_needed(id o) {
    std::string d = describe(o);
    if (!is_string(o)) return d;
    bool plain = !d.empty();
    for (char c : d) if (!(isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '/')) { plain = false; break; }
    return plain ? d : "\"" + d + "\"";
}
static std::string describe_array(const std::vector<id>& v) {
    std::string s = "(";
    for (size_t i = 0; i < v.size(); ++i) s += (i ? ",\n    " : "\n    ") + indent_lines(quote_if_needed(v[i]));
    s += v.empty() ? ")" : "\n)";
    return s;
}

static NSRange clamp_range(NSRange r, size_t n) {
    if (r.location > n) r.location = n;
    if (r.location + r.length > n) r.length = n - r.location;
    return r;
}

static std::vector<id> collect_va_objects(id first, const uint64_t* va) {
    std::vector<id> v;
    if (!first) return v;
    v.push_back(first);
    for (size_t i = 0; va && va[i]; ++i) v.push_back(reinterpret_cast<id>(va[i]));
    return v;
}

}  // namespace app

using namespace app;

// ---------------------------------------------------------------------------------------------------------------
// variadic method bodies (Darwin: variadic slots on the stack; thunks pass sp)
// ---------------------------------------------------------------------------------------------------------------
extern "C" id app_impl_stringWithFormat(id self, SEL, id fmt, const uint64_t* va) {
    id s = alloc(is_class_object(self) && is_subclass(reinterpret_cast<Class>(self), C_NSMutableString) ? C_NSMutableString : C_NSString);
    ext_get<StrExt>(s)->s = to16(format(utf8(fmt), va));
    return autorelease(s);
}
extern "C" id app_impl_initWithFormat(id self, SEL, id fmt, const uint64_t* va) {
    ext_get<StrExt>(self)->s = to16(format(utf8(fmt), va));
    return self;
}
extern "C" id app_impl_stringByAppendingFormat(id self, SEL, id fmt, const uint64_t* va) {
    return str16(u16(self) + to16(format(utf8(fmt), va)));
}
extern "C" void app_impl_appendFormat(id self, SEL, id fmt, const uint64_t* va) {
    str_set(self, u16(self) + to16(format(utf8(fmt), va)));
}
extern "C" id app_impl_arrayWithObjects(id self, SEL, id first, const uint64_t* va) {
    bool m = is_subclass(reinterpret_cast<Class>(self), C_NSMutableArray);
    return autorelease(arr_new(collect_va_objects(first, va), m));
}
extern "C" id app_impl_initWithObjects(id self, SEL, id first, const uint64_t* va) {
    for (id o : collect_va_objects(first, va)) arr_add(self, o);
    return self;
}
extern "C" id app_impl_setWithObjects(id self, SEL, id first, const uint64_t* va) {
    id s = alloc(reinterpret_cast<Class>(self));
    DictExt* e = setx(s);
    for (id o : collect_va_objects(first, va)) e->put_owned(retain(o), nullptr);
    return autorelease(s);
}
static void fill_dict_va(id d, id first, const uint64_t* va) {
    if (!first) return;
    std::vector<id> v = {first};
    for (size_t i = 0; va && va[i]; i += 2) {   // value, key, value, key, ... nil
        v.push_back(reinterpret_cast<id>(va[i]));
        if (!va[i]) break;
        v.push_back(reinterpret_cast<id>(va[i + 1]));
        if (!va[i + 1]) break;
    }
    // v = obj0, key0, obj1, key1, ...
    for (size_t i = 0; i + 1 < v.size(); i += 2) dict_set(d, v[i + 1], v[i]);
}
extern "C" id app_impl_dictionaryWithObjectsAndKeys(id self, SEL, id first, const uint64_t* va) {
    id d = alloc(reinterpret_cast<Class>(self));
    dx(d);
    fill_dict_va(d, first, va);
    return autorelease(d);
}
extern "C" id app_impl_initWithObjectsAndKeys(id self, SEL, id first, const uint64_t* va) {
    fill_dict_va(self, first, va);
    return self;
}

APP_VARIADIC(app_th_stringWithFormat, app_impl_stringWithFormat, 3)
APP_VARIADIC(app_th_initWithFormat, app_impl_initWithFormat, 3)
APP_VARIADIC(app_th_stringByAppendingFormat, app_impl_stringByAppendingFormat, 3)
APP_VARIADIC(app_th_appendFormat, app_impl_appendFormat, 3)
APP_VARIADIC(app_th_arrayWithObjects, app_impl_arrayWithObjects, 3)
APP_VARIADIC(app_th_initWithObjects, app_impl_initWithObjects, 3)
APP_VARIADIC(app_th_setWithObjects, app_impl_setWithObjects, 3)
APP_VARIADIC(app_th_dictionaryWithObjectsAndKeys, app_impl_dictionaryWithObjectsAndKeys, 3)
APP_VARIADIC(app_th_initWithObjectsAndKeys, app_impl_initWithObjectsAndKeys, 3)

// NSLog(format, ...) and CFShow: format is the fixed x0 argument, varargs start at the stack pointer
extern "C" void app_impl_NSLog(id fmt, const uint64_t* va) {
    std::string s = format(utf8(fmt), va);
    fprintf(stderr, "%s\n", s.c_str());
}
APP_VARIADIC(app_th_NSLog, app_impl_NSLog, 1)
extern "C" void app_NSLogv(id fmt, void* darwin_ap) { fprintf(stderr, "%s\n", format(utf8(fmt), static_cast<const uint64_t*>(darwin_ap)).c_str()); }

namespace app {

// ---------------------------------------------------------------------------------------------------------------
// NSString methods
// ---------------------------------------------------------------------------------------------------------------
static bool ci_eq(char16_t a, char16_t b) {
    if (a < 128 && b < 128) return tolower(a) == tolower(b);
    return a == b;
}
static long compare16(const std::u16string& a, const std::u16string& b, NSUInteger opts) {
    bool ci = opts & 1, numeric = opts & 64;
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (numeric && isdigit(a[i]) && isdigit(b[j])) {
            size_t ie = i, je = j;
            while (ie < a.size() && isdigit(a[ie])) ++ie;
            while (je < b.size() && isdigit(b[je])) ++je;
            unsigned long long na = std::stoull(to8(a.substr(i, ie - i))), nb = std::stoull(to8(b.substr(j, je - j)));
            if (na != nb) return na < nb ? -1 : 1;
            i = ie; j = je;
            continue;
        }
        char16_t ca = a[i], cb = b[j];
        if (ci && ca < 128 && cb < 128) { ca = static_cast<char16_t>(tolower(ca)); cb = static_cast<char16_t>(tolower(cb)); }
        if (ca != cb) return ca < cb ? -1 : 1;
        ++i; ++j;
    }
    if (i == a.size() && j == b.size()) return 0;
    return i == a.size() ? -1 : 1;
}
static NSRange find16(const std::u16string& hay, const std::u16string& needle, NSUInteger opts, NSRange r) {
    r = clamp_range(r, hay.size());
    if (needle.empty() || needle.size() > r.length) return {NSNotFound, 0};
    bool ci = opts & 1, back = opts & 4, anchored = opts & 8;
    size_t lo = r.location, hi = r.location + r.length - needle.size();
    auto match = [&](size_t p) {
        for (size_t k = 0; k < needle.size(); ++k)
            if (ci ? !ci_eq(hay[p + k], needle[k]) : hay[p + k] != needle[k]) return false;
        return true;
    };
    if (back) {
        for (size_t p = hi + 1; p-- > lo;) {
            if (match(p)) return {p, needle.size()};
            if (anchored) break;
        }
    } else {
        for (size_t p = lo; p <= hi; ++p) {
            if (match(p)) return {p, needle.size()};
            if (anchored) break;
        }
    }
    return {NSNotFound, 0};
}
static std::u16string replace_all16(std::u16string s, const std::u16string& a, const std::u16string& b, NSUInteger opts = 0) {
    if (a.empty()) return s;
    std::u16string out;
    size_t i = 0;
    while (i < s.size()) {
        NSRange r = find16(s, a, opts & 1, {i, s.size() - i});
        if (r.location == NSNotFound) break;
        out += s.substr(i, r.location - i);
        out += b;
        i = r.location + r.length;
    }
    out += s.substr(std::min(i, s.size()));
    return out;
}
static std::string path_last(const std::string& p) {
    std::string s = p;
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    size_t k = s.rfind('/');
    return k == std::string::npos ? s : (s.size() == 1 ? s : s.substr(k + 1));
}
static std::string path_ext(const std::string& p) {
    std::string l = path_last(p);
    size_t d = l.rfind('.');
    return d == std::string::npos || d == 0 ? "" : l.substr(d + 1);
}
static std::string path_append(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    std::string r = a;
    while (r.size() > 1 && r.back() == '/') r.pop_back();
    std::string c = b;
    while (!c.empty() && c[0] == '/') c.erase(0, 1);
    return r == "/" ? "/" + c : r + "/" + c;
}

// character sets: a predicate over UTF-16 code units
struct CharSetExt : Ext { std::function<bool(char16_t)> pred; };
static id charset(std::function<bool(char16_t)> f) {
    id o = alloc(C_NSCharacterSet);
    ext_get<CharSetExt>(o)->pred = std::move(f);
    return autorelease(o);
}
static bool cs_has(id cs, char16_t c) { CharSetExt* e = ext_as<CharSetExt>(cs); return e && e->pred && e->pred(c); }

static std::u16string trim16(const std::u16string& s, id cs) {
    size_t a = 0, b = s.size();
    while (a < b && cs_has(cs, s[a])) ++a;
    while (b > a && cs_has(cs, s[b - 1])) --b;
    return s.substr(a, b - a);
}

static id str_alloc_like(id self) {
    Class c = is_class_object(self) ? reinterpret_cast<Class>(self) : C_NSString;
    if (c == C_ConstStr) c = C_NSString;
    return alloc(is_subclass(c, C_NSMutableString) ? C_NSMutableString : C_NSString);
}

static void init_strings() {
    C_NSString = def_class("NSString", "NSObject");
    C_NSMutableString = def_class("NSMutableString", "NSString");
    C_ConstStr = def_class("__NSCFConstantString", "NSString");
    def_class("__NSCFString", "NSMutableString");
    declare_protocol(C_NSString, "NSCopying");
    declare_protocol(C_NSString, "NSCoding");
    Class S = C_NSString, M = C_NSMutableString;

    // constant strings are immortal
    def(C_ConstStr, "retain", +[](id self, SEL) -> id { return self; });
    def(C_ConstStr, "release", +[](id, SEL) {});
    def(C_ConstStr, "autorelease", +[](id self, SEL) -> id { return self; });
    def(C_ConstStr, "copyWithZone:", +[](id self, SEL, void*) -> id { return self; });

    defc(S, "string", +[](id self, SEL) -> id { return autorelease(str_alloc_like(self)); });
    defc(S, "stringWithString:", +[](id self, SEL, id s) -> id { id o = str_alloc_like(self); str_set(o, u16(s)); return autorelease(o); });
    defc(S, "stringWithUTF8String:", +[](id self, SEL, const char* p) -> id {
        if (!p) return nullptr;
        id o = str_alloc_like(self); str_set(o, to16(p)); return autorelease(o);
    });
    defc(S, "stringWithCString:encoding:", +[](id self, SEL, const char* p, NSUInteger enc) -> id {
        if (!p) return nullptr;
        std::u16string s; decode_bytes(p, strlen(p), enc, s);
        id o = str_alloc_like(self); str_set(o, s); return autorelease(o);
    });
    defc(S, "stringWithCString:", +[](id self, SEL, const char* p) -> id {
        if (!p) return nullptr;
        id o = str_alloc_like(self); str_set(o, to16(p)); return autorelease(o);
    });
    defc(S, "stringWithCharacters:length:", +[](id self, SEL, const char16_t* p, NSUInteger n) -> id {
        id o = str_alloc_like(self); str_set(o, std::u16string(p, p + n)); return autorelease(o);
    });
    defc(S, "stringWithFormat:", &app_th_stringWithFormat);
    defc(S, "localizedStringWithFormat:", &app_th_stringWithFormat);
    defc(S, "stringWithCapacity:", +[](id self, SEL, NSUInteger) -> id { return autorelease(str_alloc_like(self)); });
    auto contents_of_file = +[](id self, SEL, id path, NSUInteger enc, id* err) -> id {
        std::string data;
        if (!read_file(remap_path(utf8(path)), data)) { if (err) *err = nullptr; return nullptr; }
        std::u16string s; decode_bytes(data.data(), data.size(), enc, s);
        id o = str_alloc_like(self); str_set(o, s); return autorelease(o);
    };
    defc(S, "stringWithContentsOfFile:encoding:error:", contents_of_file);
    defc(S, "stringWithContentsOfFile:usedEncoding:error:", +[](id self, SEL, id path, NSUInteger* enc, id* err) -> id {
        std::string data;
        if (!read_file(remap_path(utf8(path)), data)) { if (err) *err = nullptr; return nullptr; }
        if (enc) *enc = 4;
        id o = str_alloc_like(self); str_set(o, to16(data)); return autorelease(o);
    });
    defc(S, "stringWithContentsOfFile:", +[](id self, SEL, id path) -> id {
        std::string data;
        if (!read_file(remap_path(utf8(path)), data)) return nullptr;
        id o = str_alloc_like(self); str_set(o, to16(data)); return autorelease(o);
    });
    defc(S, "stringWithContentsOfURL:encoding:error:", +[](id self, SEL, id url, NSUInteger enc, id* err) -> id {
        id path = send(url, "path");
        std::string data;
        if (!read_file(remap_path(utf8(path)), data)) { if (err) *err = nullptr; return nullptr; }
        std::u16string s; decode_bytes(data.data(), data.size(), enc, s);
        id o = str_alloc_like(self); str_set(o, s); return autorelease(o);
    });
    defc(S, "pathWithComponents:", +[](id, SEL, id comps) -> id {
        std::string p;
        for (id c : arr_items(comps)) p = p.empty() && utf8(c) == "/" ? "/" : path_append(p, utf8(c));
        return str(p);
    });
    defc(S, "defaultCStringEncoding", +[](id, SEL) -> NSUInteger { return 4; });

    def(S, "init", +[](id self, SEL) -> id { ext_get<StrExt>(self); return self; });
    def(S, "initWithString:", +[](id self, SEL, id s) -> id { str_set(self, u16(s)); return self; });
    def(S, "initWithUTF8String:", +[](id self, SEL, const char* p) -> id { if (!p) { release(self); return nullptr; } str_set(self, to16(p)); return self; });
    def(S, "initWithCString:encoding:", +[](id self, SEL, const char* p, NSUInteger enc) -> id {
        if (!p) { release(self); return nullptr; }
        std::u16string s; decode_bytes(p, strlen(p), enc, s); str_set(self, s); return self;
    });
    def(S, "initWithCString:", +[](id self, SEL, const char* p) -> id { str_set(self, to16(p ? p : "")); return self; });
    def(S, "initWithCharacters:length:", +[](id self, SEL, const char16_t* p, NSUInteger n) -> id { str_set(self, std::u16string(p, p + n)); return self; });
    def(S, "initWithBytes:length:encoding:", +[](id self, SEL, const void* p, NSUInteger n, NSUInteger enc) -> id {
        std::u16string s; decode_bytes(p, n, enc, s); str_set(self, s); return self;
    });
    def(S, "initWithBytesNoCopy:length:encoding:freeWhenDone:", +[](id self, SEL, void* p, NSUInteger n, NSUInteger enc, bool fr) -> id {
        std::u16string s; decode_bytes(p, n, enc, s); str_set(self, s);
        if (fr) free(p);
        return self;
    });
    def(S, "initWithData:encoding:", +[](id self, SEL, id d, NSUInteger enc) -> id {
        std::u16string s; decode_bytes(data_bytes(d), data_len(d), enc, s); str_set(self, s); return self;
    });
    def(S, "initWithFormat:", &app_th_initWithFormat);
    def(S, "initWithFormat:arguments:", +[](id self, SEL, id fmt, const uint64_t* va) -> id { return app_impl_initWithFormat(self, nullptr, fmt, va); });
    def(S, "initWithFormat:locale:arguments:", +[](id self, SEL, id fmt, id, const uint64_t* va) -> id { return app_impl_initWithFormat(self, nullptr, fmt, va); });
    def(S, "initWithContentsOfFile:encoding:error:", +[](id self, SEL, id path, NSUInteger enc, id* err) -> id {
        std::string data;
        if (!read_file(remap_path(utf8(path)), data)) { if (err) *err = nullptr; release(self); return nullptr; }
        std::u16string s; decode_bytes(data.data(), data.size(), enc, s); str_set(self, s); return self;
    });
    def(S, "initWithContentsOfFile:usedEncoding:error:", +[](id self, SEL, id path, NSUInteger* enc, id* err) -> id {
        std::string data;
        if (!read_file(remap_path(utf8(path)), data)) { if (err) *err = nullptr; release(self); return nullptr; }
        if (enc) *enc = 4;
        str_set(self, to16(data)); return self;
    });

    def(S, "length", +[](id self, SEL) -> NSUInteger { return u16(self).size(); });
    def(S, "characterAtIndex:", +[](id self, SEL, NSUInteger i) -> char16_t { const auto& s = u16(self); return i < s.size() ? s[i] : 0; });
    def(S, "getCharacters:range:", +[](id self, SEL, char16_t* buf, NSRange r) {
        const auto& s = u16(self); r = clamp_range(r, s.size());
        std::memcpy(buf, s.data() + r.location, r.length * 2);
    });
    def(S, "getCharacters:", +[](id self, SEL, char16_t* buf) { const auto& s = u16(self); std::memcpy(buf, s.data(), s.size() * 2); });
    def(S, "UTF8String", +[](id self, SEL) -> const char* { return cstr(self); });
    def(S, "fileSystemRepresentation", +[](id self, SEL) -> const char* { return cstr(self); });
    def(S, "cString", +[](id self, SEL) -> const char* { return cstr(self); });
    def(S, "lossyCString", +[](id self, SEL) -> const char* { return cstr(self); });
    def(S, "cStringUsingEncoding:", +[](id self, SEL, NSUInteger enc) -> const char* {
        if (enc == 4 || enc == 1) return cstr(self);
        id d = data_new(nullptr, 0);
        std::string b = encode_string(u16(self), enc);
        b.push_back('\0'); b.push_back('\0');
        ext_get<DataExt>(d)->v.assign(b.begin(), b.end());
        autorelease(d);
        return reinterpret_cast<const char*>(data_bytes(d));
    });
    def(S, "getBytes:maxLength:usedLength:encoding:options:range:remainingRange:",
        +[](id self, SEL, void* buf, NSUInteger maxLen, NSUInteger* used, NSUInteger enc, NSUInteger, NSRange range, NSRange* remaining) -> bool {
            const auto& s = u16(self);
            range = clamp_range(range, s.size());
            std::string bytes = encode_string(s.substr(range.location, range.length), enc);
            NSUInteger n = std::min<NSUInteger>(maxLen, bytes.size());
            if (buf && n) std::memcpy(buf, bytes.data(), n);
            if (used) *used = n;
            if (remaining) { *remaining = {range.location + range.length, 0}; }
            return n == bytes.size();
        });
    def(S, "getCString:maxLength:encoding:", +[](id self, SEL, char* buf, NSUInteger max, NSUInteger enc) -> bool {
        std::string b = encode_string(u16(self), enc);
        if (b.size() + 1 > max) return false;
        std::memcpy(buf, b.c_str(), b.size() + 1);
        return true;
    });
    def(S, "getFileSystemRepresentation:maxLength:", +[](id self, SEL, char* buf, NSUInteger max) -> bool {
        std::string b = utf8(self);
        if (b.size() + 1 > max) return false;
        std::memcpy(buf, b.c_str(), b.size() + 1);
        return true;
    });
    def(S, "lengthOfBytesUsingEncoding:", +[](id self, SEL, NSUInteger enc) -> NSUInteger { return encode_string(u16(self), enc).size(); });
    def(S, "maximumLengthOfBytesUsingEncoding:", +[](id self, SEL, NSUInteger) -> NSUInteger { return u16(self).size() * 4; });
    def(S, "canBeConvertedToEncoding:", +[](id self, SEL, NSUInteger enc) -> bool { bool lossy = false; encode_string(u16(self), enc, &lossy); return !lossy; });
    def(S, "dataUsingEncoding:", +[](id self, SEL, NSUInteger enc) -> id { std::string b = encode_string(u16(self), enc); return autorelease(data_new(b.data(), b.size())); });
    def(S, "dataUsingEncoding:allowLossyConversion:", +[](id self, SEL, NSUInteger enc, bool) -> id { std::string b = encode_string(u16(self), enc); return autorelease(data_new(b.data(), b.size())); });
    def(S, "smallestEncoding", +[](id, SEL) -> NSUInteger { return 4; });
    def(S, "fastestEncoding", +[](id, SEL) -> NSUInteger { return 4; });

    def(S, "description", +[](id self, SEL) -> id { return self; });
    def(S, "copyWithZone:", +[](id self, SEL, void*) -> id {
        if (self->isa == C_NSString) return retain(self);
        return str16_new(u16(self));
    });
    def(S, "mutableCopyWithZone:", +[](id self, SEL, void*) -> id { return str16_new(u16(self), true); });
    def(S, "hash", +[](id self, SEL) -> NSUInteger { return hash16(u16(self)); });
    def(S, "isEqual:", +[](id self, SEL, id o) -> bool { return self == o || (is_string(o) && u16(self) == u16(o)); });
    def(S, "isEqualToString:", +[](id self, SEL, id o) -> bool { return self == o || (o && u16(self) == u16(o)); });
    def(S, "compare:", +[](id self, SEL, id o) -> long { return compare16(u16(self), u16(o), 0); });
    def(S, "compare:options:", +[](id self, SEL, id o, NSUInteger opts) -> long { return compare16(u16(self), u16(o), opts); });
    def(S, "compare:options:range:", +[](id self, SEL, id o, NSUInteger opts, NSRange r) -> long {
        const auto& s = u16(self); r = clamp_range(r, s.size());
        return compare16(s.substr(r.location, r.length), u16(o), opts);
    });
    def(S, "caseInsensitiveCompare:", +[](id self, SEL, id o) -> long { return compare16(u16(self), u16(o), 1); });
    def(S, "localizedCompare:", +[](id self, SEL, id o) -> long { return compare16(u16(self), u16(o), 0); });
    def(S, "localizedCaseInsensitiveCompare:", +[](id self, SEL, id o) -> long { return compare16(u16(self), u16(o), 1); });
    def(S, "localizedStandardCompare:", +[](id self, SEL, id o) -> long { return compare16(u16(self), u16(o), 65); });
    def(S, "hasPrefix:", +[](id self, SEL, id p) -> bool {
        const auto& s = u16(self); const auto& q = u16(p);
        return !q.empty() && s.size() >= q.size() && s.compare(0, q.size(), q) == 0;
    });
    def(S, "hasSuffix:", +[](id self, SEL, id p) -> bool {
        const auto& s = u16(self); const auto& q = u16(p);
        return !q.empty() && s.size() >= q.size() && s.compare(s.size() - q.size(), q.size(), q) == 0;
    });
    def(S, "rangeOfString:", +[](id self, SEL, id n) -> NSRange { const auto& s = u16(self); return find16(s, u16(n), 0, {0, s.size()}); });
    def(S, "rangeOfString:options:", +[](id self, SEL, id n, NSUInteger o) -> NSRange { const auto& s = u16(self); return find16(s, u16(n), o, {0, s.size()}); });
    def(S, "rangeOfString:options:range:", +[](id self, SEL, id n, NSUInteger o, NSRange r) -> NSRange { return find16(u16(self), u16(n), o, r); });
    def(S, "containsString:", +[](id self, SEL, id n) -> bool { const auto& s = u16(self); return find16(s, u16(n), 0, {0, s.size()}).location != NSNotFound; });
    def(S, "rangeOfCharacterFromSet:", +[](id self, SEL, id cs) -> NSRange {
        const auto& s = u16(self);
        for (size_t i = 0; i < s.size(); ++i) if (cs_has(cs, s[i])) return {i, 1};
        return {NSNotFound, 0};
    });
    def(S, "substringFromIndex:", +[](id self, SEL, NSUInteger i) -> id { const auto& s = u16(self); return str16(i < s.size() ? s.substr(i) : u""); });
    def(S, "substringToIndex:", +[](id self, SEL, NSUInteger i) -> id { const auto& s = u16(self); return str16(s.substr(0, std::min<size_t>(i, s.size()))); });
    def(S, "substringWithRange:", +[](id self, SEL, NSRange r) -> id { const auto& s = u16(self); r = clamp_range(r, s.size()); return str16(s.substr(r.location, r.length)); });
    def(S, "stringByAppendingString:", +[](id self, SEL, id o) -> id { return str16(u16(self) + u16(o)); });
    def(S, "stringByAppendingFormat:", &app_th_stringByAppendingFormat);
    def(S, "stringByPaddingToLength:withString:startingAtIndex:", +[](id self, SEL, NSUInteger len, id pad, NSUInteger start) -> id {
        std::u16string s = u16(self); const auto& p = u16(pad);
        if (s.size() > len) s.resize(len);
        for (size_t k = start; s.size() < len && !p.empty(); ++k) s.push_back(p[k % p.size()]);
        return str16(s);
    });
    def(S, "stringByReplacingOccurrencesOfString:withString:", +[](id self, SEL, id a, id b) -> id { return str16(replace_all16(u16(self), u16(a), u16(b))); });
    def(S, "stringByReplacingOccurrencesOfString:withString:options:range:", +[](id self, SEL, id a, id b, NSUInteger o, NSRange) -> id { return str16(replace_all16(u16(self), u16(a), u16(b), o)); });
    def(S, "stringByReplacingCharactersInRange:withString:", +[](id self, SEL, NSRange r, id w) -> id {
        std::u16string s = u16(self); r = clamp_range(r, s.size());
        s.replace(r.location, r.length, u16(w));
        return str16(s);
    });
    def(S, "componentsSeparatedByString:", +[](id self, SEL, id sep) -> id {
        const auto& s = u16(self); const auto& p = u16(sep);
        std::vector<id> parts;
        size_t i = 0;
        while (true) {
            NSRange r = p.empty() ? NSRange{NSNotFound, 0} : find16(s, p, 0, {i, s.size() - i});
            if (r.location == NSNotFound) { parts.push_back(str16(s.substr(i))); break; }
            parts.push_back(str16(s.substr(i, r.location - i)));
            i = r.location + r.length;
        }
        return arr(parts);
    });
    def(S, "componentsSeparatedByCharactersInSet:", +[](id self, SEL, id cs) -> id {
        const auto& s = u16(self);
        std::vector<id> parts;
        size_t st = 0;
        for (size_t i = 0; i <= s.size(); ++i)
            if (i == s.size() || cs_has(cs, s[i])) { parts.push_back(str16(s.substr(st, i - st))); st = i + 1; }
        return arr(parts);
    });
    def(S, "stringByTrimmingCharactersInSet:", +[](id self, SEL, id cs) -> id { return str16(trim16(u16(self), cs)); });
    def(S, "lowercaseString", +[](id self, SEL) -> id { std::u16string s = u16(self); for (auto& c : s) if (c < 128) c = static_cast<char16_t>(tolower(c)); return str16(s); });
    def(S, "uppercaseString", +[](id self, SEL) -> id { std::u16string s = u16(self); for (auto& c : s) if (c < 128) c = static_cast<char16_t>(toupper(c)); return str16(s); });
    def(S, "capitalizedString", +[](id self, SEL) -> id {
        std::u16string s = u16(self); bool start = true;
        for (auto& c : s) {
            if (c < 128 && isalpha(c)) { c = static_cast<char16_t>(start ? toupper(c) : tolower(c)); start = false; }
            else start = c == ' ' || c == '\t' || c == '\n';
        }
        return str16(s);
    });
    def(S, "intValue", +[](id self, SEL) -> int { return static_cast<int>(strtol(utf8(self).c_str(), nullptr, 10)); });
    def(S, "integerValue", +[](id self, SEL) -> long { return strtol(utf8(self).c_str(), nullptr, 10); });
    def(S, "longLongValue", +[](id self, SEL) -> long long { return strtoll(utf8(self).c_str(), nullptr, 10); });
    def(S, "unsignedLongLongValue", +[](id self, SEL) -> unsigned long long { return strtoull(utf8(self).c_str(), nullptr, 10); });
    def(S, "floatValue", +[](id self, SEL) -> float { return strtof(utf8(self).c_str(), nullptr); });
    def(S, "doubleValue", +[](id self, SEL) -> double { return strtod(utf8(self).c_str(), nullptr); });
    def(S, "boolValue", +[](id self, SEL) -> bool {
        std::string s = utf8(self);
        size_t i = s.find_first_not_of(" \t\n+-0");
        if (i == std::string::npos) return false;
        char c = static_cast<char>(toupper(s[i]));
        return c == 'Y' || c == 'T' || (c >= '1' && c <= '9');
    });
    def(S, "lastPathComponent", +[](id self, SEL) -> id { return str(path_last(utf8(self))); });
    def(S, "pathExtension", +[](id self, SEL) -> id { return str(path_ext(utf8(self))); });
    def(S, "stringByDeletingPathExtension", +[](id self, SEL) -> id {
        std::string p = utf8(self), e = path_ext(p);
        if (!e.empty()) p.resize(p.size() - e.size() - 1);
        return str(p);
    });
    def(S, "stringByDeletingLastPathComponent", +[](id self, SEL) -> id {
        std::string p = utf8(self);
        while (p.size() > 1 && p.back() == '/') p.pop_back();
        size_t k = p.rfind('/');
        if (k == std::string::npos) return str("");
        return str(k == 0 ? "/" : p.substr(0, k));
    });
    def(S, "stringByAppendingPathComponent:", +[](id self, SEL, id c) -> id { return str(path_append(utf8(self), utf8(c))); });
    def(S, "stringByAppendingPathExtension:", +[](id self, SEL, id e) -> id { return str(utf8(self) + "." + utf8(e)); });
    def(S, "stringByStandardizingPath", +[](id self, SEL) -> id { return self; });
    def(S, "stringByResolvingSymlinksInPath", +[](id self, SEL) -> id { return self; });
    def(S, "stringByExpandingTildeInPath", +[](id self, SEL) -> id {
        std::string p = utf8(self);
        if (!p.empty() && p[0] == '~') p = sandbox_dir("") + p.substr(1);
        return str(p);
    });
    def(S, "stringByAbbreviatingWithTildeInPath", +[](id self, SEL) -> id { return self; });
    def(S, "isAbsolutePath", +[](id self, SEL) -> bool { std::string p = utf8(self); return !p.empty() && (p[0] == '/' || p[0] == '~'); });
    def(S, "pathComponents", +[](id self, SEL) -> id {
        std::string p = utf8(self);
        std::vector<id> v;
        if (!p.empty() && p[0] == '/') v.push_back(str("/"));
        size_t i = 0;
        while (i < p.size()) {
            size_t k = p.find('/', i);
            if (k == std::string::npos) k = p.size();
            if (k > i) v.push_back(str(p.substr(i, k - i)));
            i = k + 1;
        }
        return arr(v);
    });
    def(S, "stringByAddingPercentEscapesUsingEncoding:", +[](id self, SEL, NSUInteger) -> id {
        std::string s = utf8(self), o;
        for (unsigned char c : s) {
            if (isalnum(c) || std::strchr("-_.!~*'();/?:@&=+$,#", c)) o.push_back(static_cast<char>(c));
            else { char b[4]; snprintf(b, sizeof b, "%%%02X", c); o += b; }
        }
        return str(o);
    });
    def(S, "stringByAddingPercentEncodingWithAllowedCharacters:", +[](id self, SEL, id cs) -> id {
        std::string s = utf8(self), o;
        for (unsigned char c : s) {
            if (c < 128 && (cs ? cs_has(cs, c) : isalnum(c))) o.push_back(static_cast<char>(c));
            else { char b[4]; snprintf(b, sizeof b, "%%%02X", c); o += b; }
        }
        return str(o);
    });
    auto unescape = +[](id self, SEL, NSUInteger) -> id {
        std::string s = utf8(self), o;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '%' && i + 2 < s.size() && isxdigit(static_cast<unsigned char>(s[i + 1])) && isxdigit(static_cast<unsigned char>(s[i + 2]))) {
                o.push_back(static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16)));
                i += 2;
            } else o.push_back(s[i]);
        }
        return str(o);
    };
    def(S, "stringByReplacingPercentEscapesUsingEncoding:", unescape);
    def(S, "stringByRemovingPercentEncoding", +[](id self, SEL) -> id {
        return reinterpret_cast<id (*)(id, SEL, NSUInteger)>(lookup(self, sel("stringByReplacingPercentEscapesUsingEncoding:")))(self, nullptr, 4);
    });
    def(S, "writeToFile:atomically:encoding:error:", +[](id self, SEL, id path, bool, NSUInteger enc, id* err) -> bool {
        std::string b = encode_string(u16(self), enc);
        FILE* f = fopen(remap_path(utf8(path)).c_str(), "wb");
        if (!f) { if (err) *err = nullptr; return false; }
        fwrite(b.data(), 1, b.size(), f);
        fclose(f);
        return true;
    });
    def(S, "writeToFile:atomically:", +[](id self, SEL, id path, bool) -> bool {
        std::string b = utf8(self);
        FILE* f = fopen(remap_path(utf8(path)).c_str(), "wb");
        if (!f) return false;
        fwrite(b.data(), 1, b.size(), f);
        fclose(f);
        return true;
    });
    def(S, "propertyList", +[](id self, SEL) -> id { std::string b = utf8(self); return autorelease(plist_parse(reinterpret_cast<const uint8_t*>(b.data()), b.size(), false)); });
    def(S, "enumerateLinesUsingBlock:", +[](id self, SEL, id block) {
        const auto& s = u16(self);
        size_t st = 0;
        bool stop = false;
        for (size_t i = 0; i <= s.size() && !stop; ++i)
            if (i == s.size() || s[i] == '\n') {
                if (i > st || i < s.size()) reinterpret_cast<void (*)(id, id, bool*)>(block_invoke_ptr(block))(block, str16(s.substr(st, i - st)), &stop);
                st = i + 1;
            }
    });

    // mutable
    def(M, "appendString:", +[](id self, SEL, id o) { if (o) str_set(self, u16(self) + u16(o)); });
    def(M, "appendFormat:", &app_th_appendFormat);
    def(M, "setString:", +[](id self, SEL, id o) { str_set(self, u16(o)); });
    def(M, "insertString:atIndex:", +[](id self, SEL, id o, NSUInteger i) {
        std::u16string s = u16(self); s.insert(std::min<size_t>(i, s.size()), u16(o)); str_set(self, s);
    });
    def(M, "deleteCharactersInRange:", +[](id self, SEL, NSRange r) {
        std::u16string s = u16(self); r = clamp_range(r, s.size()); s.erase(r.location, r.length); str_set(self, s);
    });
    def(M, "replaceCharactersInRange:withString:", +[](id self, SEL, NSRange r, id w) {
        std::u16string s = u16(self); r = clamp_range(r, s.size()); s.replace(r.location, r.length, u16(w)); str_set(self, s);
    });
    def(M, "replaceOccurrencesOfString:withString:options:range:", +[](id self, SEL, id a, id b, NSUInteger o, NSRange) -> NSUInteger {
        std::u16string s = u16(self), r = replace_all16(s, u16(a), u16(b), o);
        str_set(self, r);
        return s != r;
    });
    def(M, "initWithCapacity:", +[](id self, SEL, NSUInteger) -> id { ext_get<StrExt>(self); return self; });
    def(M, "copyWithZone:", +[](id self, SEL, void*) -> id { return str16_new(u16(self)); });
}

// ---------------------------------------------------------------------------------------------------------------
// NSNumber / NSValue / NSNull
// ---------------------------------------------------------------------------------------------------------------
static void init_numbers() {
    C_NSValue = def_class("NSValue", "NSObject");
    C_NSNumber = def_class("NSNumber", "NSValue");
    C_NSDecimalNumber = def_class("NSDecimalNumber", "NSNumber");
    declare_protocol(C_NSNumber, "NSCopying");
    Class N = C_NSNumber, V = C_NSValue;
    {
        id t = alloc(C_NSNumber); auto* e = ext_get<NumExt>(t); e->type = 'c'; e->i = 1; e->d = 1;
        id f = alloc(C_NSNumber); auto* g = ext_get<NumExt>(f); g->type = 'c';
        g_true = t; g_false = f;
    }
#define NUM_CTOR(selname, T, code, isf)                                                                         \
    defc(N, "numberWith" selname ":", +[](id, SEL, T v) -> id { return isf ? num_make(code, 0, static_cast<double>(v)) : num_make(code, static_cast<int64_t>(v), 0); }); \
    def(N, "initWith" selname ":", +[](id self, SEL, T v) -> id {                                               \
        auto* e = ext_get<NumExt>(self); e->type = code;                                                          \
        if (isf) { e->d = static_cast<double>(v); e->i = static_cast<int64_t>(v); } else { e->i = static_cast<int64_t>(v); e->d = static_cast<double>(v); } \
        return self; });
    NUM_CTOR("Char", signed char, 'c', false)
    NUM_CTOR("UnsignedChar", unsigned char, 'C', false)
    NUM_CTOR("Short", short, 's', false)
    NUM_CTOR("UnsignedShort", unsigned short, 'S', false)
    NUM_CTOR("Int", int, 'i', false)
    NUM_CTOR("UnsignedInt", unsigned int, 'I', false)
    NUM_CTOR("Long", long, 'q', false)
    NUM_CTOR("UnsignedLong", unsigned long, 'Q', false)
    NUM_CTOR("LongLong", long long, 'q', false)
    NUM_CTOR("UnsignedLongLong", unsigned long long, 'Q', false)
    NUM_CTOR("Integer", long, 'q', false)
    NUM_CTOR("UnsignedInteger", unsigned long, 'Q', false)
    NUM_CTOR("Float", float, 'f', true)
    NUM_CTOR("Double", double, 'd', true)
#undef NUM_CTOR
    defc(N, "numberWithBool:", +[](id, SEL, bool v) -> id { return num_b(v); });
    def(N, "initWithBool:", +[](id self, SEL, bool v) -> id { release(self); return retain(num_b(v)); });
#define NUM_GET(selname, T)                                                                                      \
    def(N, selname, +[](id self, SEL) -> T { NumExt* e = nx(self); if (!e) return static_cast<T>(0); return e->is_float() ? static_cast<T>(e->d) : static_cast<T>(e->i); });
    NUM_GET("charValue", signed char)
    NUM_GET("unsignedCharValue", unsigned char)
    NUM_GET("shortValue", short)
    NUM_GET("unsignedShortValue", unsigned short)
    NUM_GET("intValue", int)
    NUM_GET("unsignedIntValue", unsigned int)
    NUM_GET("longValue", long)
    NUM_GET("unsignedLongValue", unsigned long)
    NUM_GET("longLongValue", long long)
    NUM_GET("unsignedLongLongValue", unsigned long long)
    NUM_GET("integerValue", long)
    NUM_GET("unsignedIntegerValue", unsigned long)
#undef NUM_GET
    def(N, "floatValue", +[](id self, SEL) -> float { NumExt* e = nx(self); return e ? static_cast<float>(e->d) : 0.f; });
    def(N, "doubleValue", +[](id self, SEL) -> double { NumExt* e = nx(self); return e ? e->d : 0.0; });
    def(N, "boolValue", +[](id self, SEL) -> bool { NumExt* e = nx(self); return e && (e->is_float() ? e->d != 0 : e->i != 0); });
    def(N, "objCType", +[](id self, SEL) -> const char* {
        NumExt* e = nx(self);
        static const char* codes[] = {"c", "C", "s", "S", "i", "I", "q", "Q", "f", "d", "B"};
        static const char order[] = "cCsSiIqQfdB";
        const char* p = e ? std::strchr(order, e->type) : nullptr;
        return p ? codes[p - order] : "q";
    });
    def(N, "stringValue", +[](id self, SEL) -> id { NumExt* e = nx(self); return str(e ? num_string(e) : "0"); });
    def(N, "description", +[](id self, SEL) -> id { NumExt* e = nx(self); return str(e ? num_string(e) : "0"); });
    def(N, "descriptionWithLocale:", +[](id self, SEL, id) -> id { NumExt* e = nx(self); return str(e ? num_string(e) : "0"); });
    def(N, "hash", +[](id self, SEL) -> NSUInteger { return obj_hash(self); });
    def(N, "isEqual:", +[](id self, SEL, id o) -> bool { return is_number(o) && obj_equal(self, o); });
    def(N, "isEqualToNumber:", +[](id self, SEL, id o) -> bool { return obj_equal(self, o); });
    def(N, "compare:", +[](id self, SEL, id o) -> long {
        NumExt* a = nx(self); NumExt* b = nx(o);
        if (!a || !b) return 0;
        if (a->is_float() || b->is_float()) return a->d < b->d ? -1 : a->d > b->d ? 1 : 0;
        return a->i < b->i ? -1 : a->i > b->i ? 1 : 0;
    });
    def(N, "copyWithZone:", +[](id self, SEL, void*) -> id { return retain(self); });
    def(N, "retain", +[](id self, SEL) -> id { return (self == g_true || self == g_false) ? self : retain(self); });
    def(N, "release", +[](id self, SEL) { if (self != g_true && self != g_false) release(self); });
    def(N, "autorelease", +[](id self, SEL) -> id { return (self == g_true || self == g_false) ? self : autorelease(self); });
    defc(C_NSDecimalNumber, "decimalNumberWithString:", +[](id, SEL, id s) -> id { return num_d(strtod(utf8(s).c_str(), nullptr)); });
    defc(C_NSDecimalNumber, "zero", +[](id, SEL) -> id { return num_d(0); });
    defc(C_NSDecimalNumber, "one", +[](id, SEL) -> id { return num_d(1); });

    // NSValue
    defc(V, "valueWithCGPoint:", +[](id, SEL, CGPoint p) -> id { return value_bytes(&p, sizeof p, "{CGPoint=dd}"); });
    defc(V, "valueWithCGSize:", +[](id, SEL, CGSize p) -> id { return value_bytes(&p, sizeof p, "{CGSize=dd}"); });
    defc(V, "valueWithCGRect:", +[](id, SEL, CGRect p) -> id { return value_bytes(&p, sizeof p, "{CGRect={CGPoint=dd}{CGSize=dd}}"); });
    defc(V, "valueWithRange:", +[](id, SEL, NSRange p) -> id { return value_bytes(&p, sizeof p, "{_NSRange=QQ}"); });
    defc(V, "valueWithCGAffineTransform:", +[](id, SEL, CGAffineTransform* p) -> id { return value_bytes(p, sizeof *p, "{CGAffineTransform=dddddd}"); });
    defc(V, "valueWithCATransform3D:", +[](id, SEL, const double* p) -> id { return value_bytes(p, 16 * 8, "{CATransform3D=dddddddddddddddd}"); });
    defc(V, "valueWithPointer:", +[](id, SEL, const void* p) -> id { return value_bytes(&p, sizeof p, "^v"); });
    defc(V, "valueWithNonretainedObject:", +[](id, SEL, id p) -> id { return value_bytes(&p, sizeof p, "@"); });
    defc(V, "valueWithBytes:objCType:", +[](id, SEL, const void* p, const char* t) -> id {
        size_t n = 8;
        if (t && t[0] == '{') n = std::strstr(t, "CGRect") ? 32 : std::strstr(t, "CGAffine") ? 48 : 16;
        else if (t && (t[0] == 'c' || t[0] == 'C' || t[0] == 'B')) n = 1;
        else if (t && (t[0] == 'i' || t[0] == 'I' || t[0] == 'f')) n = 4;
        return value_bytes(p, n, t);
    });
    def(V, "CGPointValue", +[](id self, SEL) -> CGPoint { return val_get<CGPoint>(self); });
    def(V, "CGSizeValue", +[](id self, SEL) -> CGSize { return val_get<CGSize>(self); });
    def(V, "CGRectValue", +[](id self, SEL) -> CGRect { return val_get<CGRect>(self); });
    def(V, "rangeValue", +[](id self, SEL) -> NSRange { return val_get<NSRange>(self); });
    def(V, "pointerValue", +[](id self, SEL) -> void* { return val_get<void*>(self); });
    def(V, "nonretainedObjectValue", +[](id self, SEL) -> id { return val_get<id>(self); });
    def(V, "getValue:", +[](id self, SEL, void* out) { if (ValExt* e = vx(self)) std::memcpy(out, e->bytes.data(), e->bytes.size()); });
    def(V, "objCType", +[](id self, SEL) -> const char* { ValExt* e = vx(self); return e ? e->type.c_str() : ""; });
    def(V, "isEqual:", +[](id self, SEL, id o) -> bool { ValExt* a = vx(self); ValExt* b = vx(o); return self == o || (a && b && a->bytes == b->bytes); });
    def(V, "copyWithZone:", +[](id self, SEL, void*) -> id { return retain(self); });

    C_NSNull = def_class("NSNull", "NSObject");
    static id null_obj = alloc(C_NSNull);
    defc(C_NSNull, "null", +[](id, SEL) -> id { return null_obj; });
    def(C_NSNull, "description", +[](id, SEL) -> id { return str("<null>"); });
    def(C_NSNull, "release", +[](id, SEL) {});
    def(C_NSNull, "copyWithZone:", +[](id self, SEL, void*) -> id { return self; });
}

// ---------------------------------------------------------------------------------------------------------------
// NSArray
// ---------------------------------------------------------------------------------------------------------------
static long sort_by_descriptors(id a, id b, id descs);   // app_foundation2.cpp provides descriptors via send

static void sort_vec(std::vector<id>& v, std::function<long(id, id)> cmp) {
    std::stable_sort(v.begin(), v.end(), [&](id a, id b) { return cmp(a, b) < 0; });
}

static void init_arrays() {
    C_NSArray = def_class("NSArray", "NSObject");
    C_NSMutableArray = def_class("NSMutableArray", "NSArray");
    declare_protocol(C_NSArray, "NSCopying");
    declare_protocol(C_NSArray, "NSFastEnumeration");
    Class A = C_NSArray, M = C_NSMutableArray;
    auto mk = [](id self) -> id { return alloc(reinterpret_cast<Class>(self)); };
    (void)mk;
    defc(A, "array", +[](id self, SEL) -> id { id a = alloc(reinterpret_cast<Class>(self)); ax(a); return autorelease(a); });
    defc(A, "arrayWithCapacity:", +[](id self, SEL, NSUInteger) -> id { id a = alloc(reinterpret_cast<Class>(self)); ax(a); return autorelease(a); });
    defc(A, "arrayWithObject:", +[](id self, SEL, id o) -> id { id a = alloc(reinterpret_cast<Class>(self)); arr_add(a, o); return autorelease(a); });
    defc(A, "arrayWithObjects:", &app_th_arrayWithObjects);
    defc(A, "arrayWithObjects:count:", +[](id self, SEL, id* objs, NSUInteger n) -> id {
        id a = alloc(reinterpret_cast<Class>(self)); for (NSUInteger i = 0; i < n; ++i) arr_add(a, objs[i]); return autorelease(a);
    });
    defc(A, "arrayWithArray:", +[](id self, SEL, id o) -> id { id a = alloc(reinterpret_cast<Class>(self)); for (id x : arr_items(o)) arr_add(a, x); return autorelease(a); });
    defc(A, "arrayWithContentsOfFile:", +[](id self, SEL, id path) -> id {
        std::string d;
        if (!read_file(remap_path(utf8(path)), d)) return nullptr;
        id p = plist_parse(reinterpret_cast<const uint8_t*>(d.data()), d.size(), is_subclass(reinterpret_cast<Class>(self), C_NSMutableArray));
        if (!is_array(p)) { release(p); return nullptr; }
        return autorelease(p);
    });
    def(A, "init", +[](id self, SEL) -> id { ax(self); return self; });
    def(A, "initWithCapacity:", +[](id self, SEL, NSUInteger) -> id { ax(self); return self; });
    def(A, "initWithArray:", +[](id self, SEL, id o) -> id { for (id x : arr_items(o)) arr_add(self, x); return self; });
    def(A, "initWithArray:copyItems:", +[](id self, SEL, id o, bool copy) -> id {
        for (id x : arr_items(o)) { if (copy) { id c = send(x, "copy"); arr_add(self, c); release(c); } else arr_add(self, x); }
        return self;
    });
    def(A, "initWithObjects:", &app_th_initWithObjects);
    def(A, "initWithObjects:count:", +[](id self, SEL, id* objs, NSUInteger n) -> id { for (NSUInteger i = 0; i < n; ++i) arr_add(self, objs[i]); return self; });
    def(A, "initWithContentsOfFile:", +[](id self, SEL, id path) -> id {
        std::string d;
        if (!read_file(remap_path(utf8(path)), d)) { release(self); return nullptr; }
        id p = plist_parse(reinterpret_cast<const uint8_t*>(d.data()), d.size(), false);
        for (id x : arr_items(p)) arr_add(self, x);
        release(p);
        return self;
    });
    def(A, "count", +[](id self, SEL) -> NSUInteger { return ax(self)->v.size(); });
    auto at = +[](id self, SEL, NSUInteger i) -> id {
        auto& v = ax(self)->v;
        if (i >= v.size()) { log("-[NSArray objectAtIndex:]: index %lu beyond bounds [0 .. %ld]", i, static_cast<long>(v.size()) - 1); return nullptr; }
        return v[i];
    };
    def(A, "objectAtIndex:", at);
    def(A, "objectAtIndexedSubscript:", at);
    def(A, "firstObject", +[](id self, SEL) -> id { auto& v = ax(self)->v; return v.empty() ? nullptr : v.front(); });
    def(A, "lastObject", +[](id self, SEL) -> id { auto& v = ax(self)->v; return v.empty() ? nullptr : v.back(); });
    def(A, "containsObject:", +[](id self, SEL, id o) -> bool { for (id x : ax(self)->v) if (obj_equal(x, o)) return true; return false; });
    def(A, "indexOfObject:", +[](id self, SEL, id o) -> NSUInteger {
        auto& v = ax(self)->v; for (size_t i = 0; i < v.size(); ++i) if (obj_equal(v[i], o)) return i; return NSNotFound;
    });
    def(A, "indexOfObjectIdenticalTo:", +[](id self, SEL, id o) -> NSUInteger {
        auto& v = ax(self)->v; for (size_t i = 0; i < v.size(); ++i) if (v[i] == o) return i; return NSNotFound;
    });
    def(A, "objectEnumerator", +[](id self, SEL) -> id { return autorelease(new_enumerator(ax(self)->v)); });
    def(A, "reverseObjectEnumerator", +[](id self, SEL) -> id { std::vector<id> v = ax(self)->v; std::reverse(v.begin(), v.end()); return autorelease(new_enumerator(v)); });
    def(A, "allObjects", +[](id self, SEL) -> id { return arr(ax(self)->v); });
    def(A, "arrayByAddingObject:", +[](id self, SEL, id o) -> id { std::vector<id> v = ax(self)->v; v.push_back(o); return arr(v); });
    def(A, "arrayByAddingObjectsFromArray:", +[](id self, SEL, id o) -> id { std::vector<id> v = ax(self)->v; for (id x : arr_items(o)) v.push_back(x); return arr(v); });
    def(A, "componentsJoinedByString:", +[](id self, SEL, id sep) -> id {
        std::string s, p = utf8(sep);
        auto& v = ax(self)->v;
        for (size_t i = 0; i < v.size(); ++i) { if (i) s += p; s += describe(v[i]); }
        return str(s);
    });
    def(A, "subarrayWithRange:", +[](id self, SEL, NSRange r) -> id {
        auto& v = ax(self)->v; r = clamp_range(r, v.size());
        return arr(std::vector<id>(v.begin() + r.location, v.begin() + r.location + r.length));
    });
    def(A, "getObjects:range:", +[](id self, SEL, id* out, NSRange r) {
        auto& v = ax(self)->v; r = clamp_range(r, v.size());
        for (size_t i = 0; i < r.length; ++i) out[i] = v[r.location + i];
    });
    def(A, "sortedArrayUsingSelector:", +[](id self, SEL, SEL s) -> id {
        std::vector<id> v = ax(self)->v; sort_vec(v, [s](id a, id b) { return send<long>(a, s, b); }); return arr(v);
    });
    def(A, "sortedArrayUsingFunction:context:", +[](id self, SEL, Comparator f, void* ctx) -> id {
        std::vector<id> v = ax(self)->v; sort_vec(v, [f, ctx](id a, id b) { return f(a, b, ctx); }); return arr(v);
    });
    def(A, "sortedArrayUsingComparator:", +[](id self, SEL, id block) -> id {
        std::vector<id> v = ax(self)->v; sort_vec(v, [block](id a, id b) { return call_block_cmp(block, a, b); }); return arr(v);
    });
    def(A, "sortedArrayWithOptions:usingComparator:", +[](id self, SEL, NSUInteger, id block) -> id {
        std::vector<id> v = ax(self)->v; sort_vec(v, [block](id a, id b) { return call_block_cmp(block, a, b); }); return arr(v);
    });
    def(A, "sortedArrayUsingDescriptors:", +[](id self, SEL, id descs) -> id {
        std::vector<id> v = ax(self)->v; sort_vec(v, [descs](id a, id b) { return sort_by_descriptors(a, b, descs); }); return arr(v);
    });
    def(A, "makeObjectsPerformSelector:", +[](id self, SEL, SEL s) { std::vector<id> v = ax(self)->v; for (id x : v) send(x, s); });
    def(A, "makeObjectsPerformSelector:withObject:", +[](id self, SEL, SEL s, id o) { std::vector<id> v = ax(self)->v; for (id x : v) send(x, s, o); });
    def(A, "enumerateObjectsUsingBlock:", +[](id self, SEL, id block) {
        std::vector<id> v = ax(self)->v;
        bool stop = false;
        auto fn = reinterpret_cast<void (*)(id, id, NSUInteger, bool*)>(block_invoke_ptr(block));
        for (size_t i = 0; i < v.size() && !stop; ++i) fn(block, v[i], i, &stop);
    });
    def(A, "enumerateObjectsWithOptions:usingBlock:", +[](id self, SEL, NSUInteger opts, id block) {
        std::vector<id> v = ax(self)->v;
        bool stop = false;
        auto fn = reinterpret_cast<void (*)(id, id, NSUInteger, bool*)>(block_invoke_ptr(block));
        if (opts & 2) { for (size_t i = v.size(); i-- > 0 && !stop;) fn(block, v[i], i, &stop); }
        else for (size_t i = 0; i < v.size() && !stop; ++i) fn(block, v[i], i, &stop);
    });
    def(A, "indexOfObjectPassingTest:", +[](id self, SEL, id block) -> NSUInteger {
        std::vector<id> v = ax(self)->v;
        bool stop = false;
        auto fn = reinterpret_cast<bool (*)(id, id, NSUInteger, bool*)>(block_invoke_ptr(block));
        for (size_t i = 0; i < v.size() && !stop; ++i) if (fn(block, v[i], i, &stop)) return i;
        return NSNotFound;
    });
    def(A, "isEqualToArray:", +[](id self, SEL, id o) -> bool {
        auto& a = ax(self)->v; const auto& b = arr_items(o);
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) if (!obj_equal(a[i], b[i])) return false;
        return true;
    });
    def(A, "isEqual:", +[](id self, SEL, id o) -> bool { return self == o || (is_array(o) && send<bool>(self, "isEqualToArray:", o)); });
    def(A, "hash", +[](id self, SEL) -> NSUInteger { return ax(self)->v.size(); });
    def(A, "description", +[](id self, SEL) -> id { return str(describe_array(ax(self)->v)); });
    def(A, "copyWithZone:", +[](id self, SEL, void*) -> id { if (self->isa == C_NSArray) return retain(self); return arr_new(ax(self)->v); });
    def(A, "mutableCopyWithZone:", +[](id self, SEL, void*) -> id { return arr_new(ax(self)->v, true); });
    def(A, "countByEnumeratingWithState:objects:count:", +[](id self, SEL, FastEnumState* st, id* buf, NSUInteger len) -> NSUInteger {
        ArrExt* e = ax(self); return fast_enum(e->v, &e->mutations, st, buf, len);
    });
    def(A, "valueForKey:", +[](id self, SEL, id key) -> id {
        std::vector<id> out;
        for (id x : ax(self)->v) { id v = send(x, "valueForKey:", key); out.push_back(v ? v : send_cls(C_NSNull, "null")); }
        return arr(out);
    });
    def(A, "writeToFile:atomically:", +[](id self, SEL, id path, bool) -> bool {
        std::string out;
        if (!plist_write_xml(self, out)) return false;
        FILE* f = fopen(remap_path(utf8(path)).c_str(), "wb");
        if (!f) return false;
        fwrite(out.data(), 1, out.size(), f);
        fclose(f);
        return true;
    });

    def(M, "addObject:", +[](id self, SEL, id o) { if (!o) { log("-[NSMutableArray addObject:] nil"); return; } arr_add(self, o); });
    def(M, "addObjectsFromArray:", +[](id self, SEL, id o) { std::vector<id> v = arr_items(o); for (id x : v) arr_add(self, x); });
    def(M, "insertObject:atIndex:", +[](id self, SEL, id o, NSUInteger i) {
        if (!o) return;
        ArrExt* e = ax(self); if (i > e->v.size()) i = e->v.size();
        e->v.insert(e->v.begin() + i, retain(o)); e->mutations++;
    });
    def(M, "removeObject:", +[](id self, SEL, id o) {
        ArrExt* e = ax(self); std::vector<id> dead, keep;
        for (id x : e->v) (obj_equal(x, o) ? dead : keep).push_back(x);
        if (dead.empty()) return;
        e->v.swap(keep); e->mutations++;
        for (id x : dead) release(x);
    });
    def(M, "removeObjectIdenticalTo:", +[](id self, SEL, id o) {
        ArrExt* e = ax(self); std::vector<id> dead, keep;
        for (id x : e->v) (x == o ? dead : keep).push_back(x);
        if (dead.empty()) return;
        e->v.swap(keep); e->mutations++;
        for (id x : dead) release(x);
    });
    def(M, "removeObjectAtIndex:", +[](id self, SEL, NSUInteger i) {
        ArrExt* e = ax(self); if (i >= e->v.size()) { log("removeObjectAtIndex: %lu out of range", i); return; }
        id x = e->v[i]; e->v.erase(e->v.begin() + i); e->mutations++; release(x);
    });
    def(M, "removeLastObject", +[](id self, SEL) { ArrExt* e = ax(self); if (e->v.empty()) return; id x = e->v.back(); e->v.pop_back(); e->mutations++; release(x); });
    def(M, "removeAllObjects", +[](id self, SEL) { ArrExt* e = ax(self); std::vector<id> v; v.swap(e->v); e->mutations++; for (id x : v) release(x); });
    def(M, "removeObjectsInArray:", +[](id self, SEL, id o) {
        std::vector<id> rm = arr_items(o);
        for (id x : rm) send(self, "removeObject:", x);
    });
    def(M, "removeObjectsInRange:", +[](id self, SEL, NSRange r) {
        ArrExt* e = ax(self); r = clamp_range(r, e->v.size());
        std::vector<id> dead(e->v.begin() + r.location, e->v.begin() + r.location + r.length);
        e->v.erase(e->v.begin() + r.location, e->v.begin() + r.location + r.length); e->mutations++;
        for (id x : dead) release(x);
    });
    def(M, "replaceObjectAtIndex:withObject:", +[](id self, SEL, NSUInteger i, id o) {
        ArrExt* e = ax(self); if (i >= e->v.size() || !o) return;
        id old = e->v[i]; e->v[i] = retain(o); e->mutations++; release(old);
    });
    def(M, "setObject:atIndexedSubscript:", +[](id self, SEL, id o, NSUInteger i) {
        ArrExt* e = ax(self); if (!o) return;
        if (i == e->v.size()) { arr_add(self, o); return; }
        if (i > e->v.size()) return;
        id old = e->v[i]; e->v[i] = retain(o); e->mutations++; release(old);
    });
    def(M, "exchangeObjectAtIndex:withObjectAtIndex:", +[](id self, SEL, NSUInteger a, NSUInteger b) {
        ArrExt* e = ax(self); if (a < e->v.size() && b < e->v.size()) { std::swap(e->v[a], e->v[b]); e->mutations++; }
    });
    def(M, "setArray:", +[](id self, SEL, id o) {
        std::vector<id> nv; for (id x : arr_items(o)) nv.push_back(retain(x));
        ArrExt* e = ax(self); std::vector<id> old; old.swap(e->v); e->v = nv; e->mutations++;
        for (id x : old) release(x);
    });
    def(M, "sortUsingSelector:", +[](id self, SEL, SEL s) { ArrExt* e = ax(self); sort_vec(e->v, [s](id a, id b) { return send<long>(a, s, b); }); e->mutations++; });
    def(M, "sortUsingFunction:context:", +[](id self, SEL, Comparator f, void* ctx) { ArrExt* e = ax(self); sort_vec(e->v, [f, ctx](id a, id b) { return f(a, b, ctx); }); e->mutations++; });
    def(M, "sortUsingComparator:", +[](id self, SEL, id block) { ArrExt* e = ax(self); sort_vec(e->v, [block](id a, id b) { return call_block_cmp(block, a, b); }); e->mutations++; });
    def(M, "sortUsingDescriptors:", +[](id self, SEL, id descs) { ArrExt* e = ax(self); sort_vec(e->v, [descs](id a, id b) { return sort_by_descriptors(a, b, descs); }); e->mutations++; });
    def(M, "copyWithZone:", +[](id self, SEL, void*) -> id { return arr_new(ax(self)->v); });
}

// NSSortDescriptor support: each descriptor answers -compareObject:toObject:
static long sort_by_descriptors(id a, id b, id descs) {
    for (id d : arr_items(descs)) {
        long r = send<long>(d, "compareObject:toObject:", a, b);
        if (r) return r;
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// NSEnumerator
// ---------------------------------------------------------------------------------------------------------------
struct EnumExt : Ext {
    std::vector<id> v;
    size_t i = 0;
    unsigned long mutations = 0;
    ~EnumExt() override { for (id o : v) release(o); }
};
static id new_enumerator(std::vector<id> items) {
    id e = alloc(C_NSEnumerator);
    auto* x = ext_get<EnumExt>(e);
    for (id o : items) x->v.push_back(retain(o));
    return e;
}
static void init_enumerator() {
    C_NSEnumerator = def_class("NSEnumerator", "NSObject");
    def(C_NSEnumerator, "nextObject", +[](id self, SEL) -> id { auto* e = ext_get<EnumExt>(self); return e->i < e->v.size() ? e->v[e->i++] : nullptr; });
    def(C_NSEnumerator, "allObjects", +[](id self, SEL) -> id {
        auto* e = ext_get<EnumExt>(self);
        std::vector<id> rest(e->v.begin() + std::min(e->i, e->v.size()), e->v.end());
        e->i = e->v.size();
        return arr(rest);
    });
    def(C_NSEnumerator, "countByEnumeratingWithState:objects:count:", +[](id self, SEL, FastEnumState* st, id* buf, NSUInteger len) -> NSUInteger {
        auto* e = ext_get<EnumExt>(self);
        if (st->state == 0) st->mutationsPtr = &e->mutations;
        NSUInteger n = 0;
        while (n < len && e->i < e->v.size()) buf[n++] = e->v[e->i++];
        st->state = 1;
        st->itemsPtr = buf;
        return n;
    });
}

// ---------------------------------------------------------------------------------------------------------------
// NSDictionary / NSSet
// ---------------------------------------------------------------------------------------------------------------
static std::string describe_dict(DictExt* e) {
    std::string s = "{";
    bool any = false;
    for (auto& x : e->e) {
        if (!x.live) continue;
        s += "\n    " + quote_if_needed(x.k) + " = " + indent_lines(quote_if_needed(x.v)) + ";";
        any = true;
    }
    s += any ? "\n}" : "}";
    return s;
}

static void init_dicts() {
    C_NSDictionary = def_class("NSDictionary", "NSObject");
    C_NSMutableDictionary = def_class("NSMutableDictionary", "NSDictionary");
    declare_protocol(C_NSDictionary, "NSCopying");
    declare_protocol(C_NSDictionary, "NSFastEnumeration");
    Class D = C_NSDictionary, M = C_NSMutableDictionary;
    defc(D, "dictionary", +[](id self, SEL) -> id { id d = alloc(reinterpret_cast<Class>(self)); dx(d); return autorelease(d); });
    defc(D, "dictionaryWithCapacity:", +[](id self, SEL, NSUInteger) -> id { id d = alloc(reinterpret_cast<Class>(self)); dx(d); return autorelease(d); });
    defc(D, "dictionaryWithObject:forKey:", +[](id self, SEL, id o, id k) -> id { id d = alloc(reinterpret_cast<Class>(self)); dict_set(d, k, o); return autorelease(d); });
    defc(D, "dictionaryWithObjects:forKeys:", +[](id self, SEL, id os, id ks) -> id {
        id d = alloc(reinterpret_cast<Class>(self)); dx(d);
        const auto& a = arr_items(os); const auto& b = arr_items(ks);
        for (size_t i = 0; i < a.size() && i < b.size(); ++i) dict_set(d, b[i], a[i]);
        return autorelease(d);
    });
    defc(D, "dictionaryWithObjects:forKeys:count:", +[](id self, SEL, id* os, id* ks, NSUInteger n) -> id {
        id d = alloc(reinterpret_cast<Class>(self)); dx(d);
        for (NSUInteger i = 0; i < n; ++i) dict_set(d, ks[i], os[i]);
        return autorelease(d);
    });
    defc(D, "dictionaryWithObjectsAndKeys:", &app_th_dictionaryWithObjectsAndKeys);
    defc(D, "dictionaryWithDictionary:", +[](id self, SEL, id o) -> id {
        id d = alloc(reinterpret_cast<Class>(self)); dx(d);
        for (auto& kv : dict_entries(o)) dict_set(d, kv.first, kv.second);
        return autorelease(d);
    });
    defc(D, "dictionaryWithContentsOfFile:", +[](id self, SEL, id path) -> id {
        std::string data;
        if (!read_file(remap_path(utf8(path)), data)) return nullptr;
        id p = plist_parse(reinterpret_cast<const uint8_t*>(data.data()), data.size(), is_subclass(reinterpret_cast<Class>(self), C_NSMutableDictionary));
        if (!is_dict(p)) { release(p); return nullptr; }
        return autorelease(p);
    });
    defc(D, "dictionaryWithContentsOfURL:", +[](id self, SEL, id url) -> id {
        std::string data;
        if (!read_file(remap_path(utf8(send(url, "path"))), data)) return nullptr;
        id p = plist_parse(reinterpret_cast<const uint8_t*>(data.data()), data.size(), is_subclass(reinterpret_cast<Class>(self), C_NSMutableDictionary));
        if (!is_dict(p)) { release(p); return nullptr; }
        return autorelease(p);
    });
    def(D, "init", +[](id self, SEL) -> id { dx(self); return self; });
    def(D, "initWithCapacity:", +[](id self, SEL, NSUInteger) -> id { dx(self); return self; });
    def(D, "initWithDictionary:", +[](id self, SEL, id o) -> id { dx(self); for (auto& kv : dict_entries(o)) dict_set(self, kv.first, kv.second); return self; });
    def(D, "initWithDictionary:copyItems:", +[](id self, SEL, id o, bool copy) -> id {
        dx(self);
        for (auto& kv : dict_entries(o)) {
            if (copy) { id c = send(kv.second, "copy"); dict_set(self, kv.first, c); release(c); }
            else dict_set(self, kv.first, kv.second);
        }
        return self;
    });
    def(D, "initWithObjects:forKeys:", +[](id self, SEL, id os, id ks) -> id {
        dx(self);
        const auto& a = arr_items(os); const auto& b = arr_items(ks);
        for (size_t i = 0; i < a.size() && i < b.size(); ++i) dict_set(self, b[i], a[i]);
        return self;
    });
    def(D, "initWithObjects:forKeys:count:", +[](id self, SEL, id* os, id* ks, NSUInteger n) -> id {
        dx(self); for (NSUInteger i = 0; i < n; ++i) dict_set(self, ks[i], os[i]); return self;
    });
    def(D, "initWithObjectsAndKeys:", &app_th_initWithObjectsAndKeys);
    def(D, "initWithContentsOfFile:", +[](id self, SEL, id path) -> id {
        std::string data;
        if (!read_file(remap_path(utf8(path)), data)) { release(self); return nullptr; }
        id p = plist_parse(reinterpret_cast<const uint8_t*>(data.data()), data.size(), true);
        if (!is_dict(p)) { release(p); release(self); return nullptr; }
        dx(self);
        for (auto& kv : dict_entries(p)) dict_set(self, kv.first, kv.second);
        release(p);
        return self;
    });
    def(D, "count", +[](id self, SEL) -> NSUInteger { return dx(self)->live; });
    def(D, "objectForKey:", +[](id self, SEL, id k) -> id { return dx(self)->get(k); });
    def(D, "objectForKeyedSubscript:", +[](id self, SEL, id k) -> id { return dx(self)->get(k); });
    def(D, "valueForKey:", +[](id self, SEL, id k) -> id { return dx(self)->get(k); });
    def(D, "valueForKeyPath:", +[](id self, SEL, id kp) -> id {
        std::string p = utf8(kp);
        id cur = self;
        size_t i = 0;
        while (cur && i <= p.size()) {
            size_t k = p.find('.', i);
            if (k == std::string::npos) k = p.size();
            cur = send(cur, "valueForKey:", str(p.substr(i, k - i)));
            i = k + 1;
            if (k == p.size()) break;
        }
        return cur;
    });
    def(D, "allKeys", +[](id self, SEL) -> id { return arr(dx(self)->keys()); });
    def(D, "allValues", +[](id self, SEL) -> id { return arr(dx(self)->values()); });
    def(D, "allKeysForObject:", +[](id self, SEL, id o) -> id {
        std::vector<id> k; for (auto& x : dx(self)->e) if (x.live && obj_equal(x.v, o)) k.push_back(x.k); return arr(k);
    });
    def(D, "keyEnumerator", +[](id self, SEL) -> id { return autorelease(new_enumerator(dx(self)->keys())); });
    def(D, "objectEnumerator", +[](id self, SEL) -> id { return autorelease(new_enumerator(dx(self)->values())); });
    def(D, "objectsForKeys:notFoundMarker:", +[](id self, SEL, id ks, id marker) -> id {
        std::vector<id> v; for (id k : arr_items(ks)) { id o = dx(self)->get(k); v.push_back(o ? o : marker); } return arr(v);
    });
    def(D, "keysSortedByValueUsingSelector:", +[](id self, SEL, SEL s) -> id {
        DictExt* e = dx(self);
        std::vector<std::pair<id, id>> kv; for (auto& x : e->e) if (x.live) kv.push_back({x.k, x.v});
        std::stable_sort(kv.begin(), kv.end(), [s](auto& a, auto& b) { return send<long>(a.second, s, b.second) < 0; });
        std::vector<id> k; for (auto& p : kv) k.push_back(p.first);
        return arr(k);
    });
    def(D, "enumerateKeysAndObjectsUsingBlock:", +[](id self, SEL, id block) {
        auto entries = dict_entries(self);
        bool stop = false;
        auto fn = reinterpret_cast<void (*)(id, id, id, bool*)>(block_invoke_ptr(block));
        for (auto& kv : entries) { fn(block, kv.first, kv.second, &stop); if (stop) break; }
    });
    def(D, "enumerateKeysAndObjectsWithOptions:usingBlock:", +[](id self, SEL, NSUInteger, id block) {
        auto entries = dict_entries(self);
        bool stop = false;
        auto fn = reinterpret_cast<void (*)(id, id, id, bool*)>(block_invoke_ptr(block));
        for (auto& kv : entries) { fn(block, kv.first, kv.second, &stop); if (stop) break; }
    });
    def(D, "keysOfEntriesPassingTest:", +[](id self, SEL, id block) -> id {
        auto entries = dict_entries(self);
        bool stop = false;
        auto fn = reinterpret_cast<bool (*)(id, id, id, bool*)>(block_invoke_ptr(block));
        id set = autorelease(alloc(C_NSSet));
        DictExt* se = setx(set);
        for (auto& kv : entries) { if (fn(block, kv.first, kv.second, &stop)) se->put_owned(retain(kv.first), nullptr); if (stop) break; }
        return set;
    });
    def(D, "isEqualToDictionary:", +[](id self, SEL, id o) -> bool {
        if (!is_dict(o)) return false;
        DictExt* a = dx(self); DictExt* b = dx(o);
        if (a->live != b->live) return false;
        for (auto& x : a->e) if (x.live && !obj_equal(x.v, b->get(x.k))) return false;
        return true;
    });
    def(D, "isEqual:", +[](id self, SEL, id o) -> bool { return self == o || send<bool>(self, "isEqualToDictionary:", o); });
    def(D, "hash", +[](id self, SEL) -> NSUInteger { return dx(self)->live; });
    def(D, "description", +[](id self, SEL) -> id { return str(describe_dict(dx(self))); });
    def(D, "descriptionInStringsFileFormat", +[](id self, SEL) -> id { return str(describe_dict(dx(self))); });
    def(D, "copyWithZone:", +[](id self, SEL, void*) -> id {
        if (self->isa == C_NSDictionary) return retain(self);
        id d = dict_new(false); for (auto& kv : dict_entries(self)) dict_set(d, kv.first, kv.second); return d;
    });
    def(D, "mutableCopyWithZone:", +[](id self, SEL, void*) -> id { id d = dict_new(true); for (auto& kv : dict_entries(self)) dict_set(d, kv.first, kv.second); return d; });
    def(D, "countByEnumeratingWithState:objects:count:", +[](id self, SEL, FastEnumState* st, id* buf, NSUInteger len) -> NSUInteger {
        DictExt* e = dx(self);
        if (st->state == 0) st->mutationsPtr = &e->mutations;
        // walk live entries from the saved raw index (extra[0])
        NSUInteger n = 0;
        size_t i = st->extra[0];
        while (i < e->e.size() && n < len) { if (e->e[i].live) buf[n++] = e->e[i].k; ++i; }
        st->extra[0] = i;
        st->state = 1;
        st->itemsPtr = buf;
        return n;
    });
    def(D, "writeToFile:atomically:", +[](id self, SEL, id path, bool) -> bool {
        std::string out;
        if (!plist_write_xml(self, out)) return false;
        std::string p = remap_path(utf8(path)), tmp = p + ".tmp";
        FILE* f = fopen(tmp.c_str(), "wb");
        if (!f) return false;
        fwrite(out.data(), 1, out.size(), f);
        fclose(f);
        return rename(tmp.c_str(), p.c_str()) == 0;
    });
    def(D, "writeToURL:atomically:", +[](id self, SEL, id url, bool a) -> bool { return send<bool>(self, "writeToFile:atomically:", send(url, "path"), a); });
    def(D, "fileSize", +[](id self, SEL) -> unsigned long long { return static_cast<unsigned long long>(num_ival(dict_get(self, "NSFileSize"))); });
    def(D, "fileModificationDate", +[](id self, SEL) -> id { return dict_get(self, "NSFileModificationDate"); });

    def(M, "setObject:forKey:", +[](id self, SEL, id o, id k) {
        if (!k) { log("-[NSMutableDictionary setObject:forKey:] nil key"); return; }
        if (!o) { log("-[NSMutableDictionary setObject:forKey:] nil object for key %s", describe(k).c_str()); return; }
        dict_set(self, k, o);
    });
    def(M, "setObject:forKeyedSubscript:", +[](id self, SEL, id o, id k) { if (k) dict_set(self, k, o); });
    def(M, "setValue:forKey:", +[](id self, SEL, id o, id k) { if (k) dict_set(self, k, o); });
    def(M, "removeObjectForKey:", +[](id self, SEL, id k) { dx(self)->remove(k); });
    def(M, "removeObjectsForKeys:", +[](id self, SEL, id ks) { std::vector<id> v = arr_items(ks); for (id k : v) dx(self)->remove(k); });
    def(M, "removeAllObjects", +[](id self, SEL) { dx(self)->clear(); });
    def(M, "addEntriesFromDictionary:", +[](id self, SEL, id o) { for (auto& kv : dict_entries(o)) dict_set(self, kv.first, kv.second); });
    def(M, "setDictionary:", +[](id self, SEL, id o) {
        auto entries = dict_entries(o);
        std::vector<id> hold; for (auto& kv : entries) { hold.push_back(retain(kv.first)); hold.push_back(retain(kv.second)); }
        dx(self)->clear();
        for (auto& kv : entries) dict_set(self, kv.first, kv.second);
        for (id h : hold) release(h);
    });
    def(M, "copyWithZone:", +[](id self, SEL, void*) -> id { id d = dict_new(false); for (auto& kv : dict_entries(self)) dict_set(d, kv.first, kv.second); return d; });

    // NSSet
    C_NSSet = def_class("NSSet", "NSObject");
    C_NSMutableSet = def_class("NSMutableSet", "NSSet");
    C_NSCountedSet = def_class("NSCountedSet", "NSMutableSet");
    Class S = C_NSSet, MS = C_NSMutableSet;
    defc(S, "set", +[](id self, SEL) -> id { id s = alloc(reinterpret_cast<Class>(self)); setx(s); return autorelease(s); });
    defc(S, "setWithCapacity:", +[](id self, SEL, NSUInteger) -> id { id s = alloc(reinterpret_cast<Class>(self)); setx(s); return autorelease(s); });
    defc(S, "setWithObject:", +[](id self, SEL, id o) -> id { id s = alloc(reinterpret_cast<Class>(self)); if (o) setx(s)->put_owned(retain(o), nullptr); else setx(s); return autorelease(s); });
    defc(S, "setWithObjects:", &app_th_setWithObjects);
    defc(S, "setWithArray:", +[](id self, SEL, id a) -> id { id s = alloc(reinterpret_cast<Class>(self)); DictExt* e = setx(s); for (id o : arr_items(a)) e->put_owned(retain(o), nullptr); return autorelease(s); });
    defc(S, "setWithSet:", +[](id self, SEL, id o) -> id { id s = alloc(reinterpret_cast<Class>(self)); DictExt* e = setx(s); if (is_set_obj(o)) for (id x : setx(o)->keys()) e->put_owned(retain(x), nullptr); return autorelease(s); });
    def(S, "init", +[](id self, SEL) -> id { setx(self); return self; });
    def(S, "initWithCapacity:", +[](id self, SEL, NSUInteger) -> id { setx(self); return self; });
    def(S, "initWithArray:", +[](id self, SEL, id a) -> id { DictExt* e = setx(self); for (id o : arr_items(a)) e->put_owned(retain(o), nullptr); return self; });
    def(S, "initWithSet:", +[](id self, SEL, id o) -> id { DictExt* e = setx(self); if (is_set_obj(o)) for (id x : setx(o)->keys()) e->put_owned(retain(x), nullptr); return self; });
    def(S, "count", +[](id self, SEL) -> NSUInteger { return setx(self)->live; });
    def(S, "member:", +[](id self, SEL, id o) -> id { return setx(self)->get(o); });
    def(S, "containsObject:", +[](id self, SEL, id o) -> bool { return setx(self)->get(o) != nullptr; });
    def(S, "anyObject", +[](id self, SEL) -> id { for (auto& x : setx(self)->e) if (x.live) return x.k; return nullptr; });
    def(S, "allObjects", +[](id self, SEL) -> id { return arr(setx(self)->keys()); });
    def(S, "objectEnumerator", +[](id self, SEL) -> id { return autorelease(new_enumerator(setx(self)->keys())); });
    def(S, "setByAddingObject:", +[](id self, SEL, id o) -> id {
        id s = autorelease(alloc(C_NSSet)); DictExt* e = setx(s);
        for (id x : setx(self)->keys()) e->put_owned(retain(x), nullptr);
        if (o) e->put_owned(retain(o), nullptr);
        return s;
    });
    def(S, "isSubsetOfSet:", +[](id self, SEL, id o) -> bool { for (id x : setx(self)->keys()) if (!setx(o)->get(x)) return false; return true; });
    def(S, "intersectsSet:", +[](id self, SEL, id o) -> bool { for (id x : setx(self)->keys()) if (setx(o)->get(x)) return true; return false; });
    def(S, "makeObjectsPerformSelector:", +[](id self, SEL, SEL s) { for (id x : setx(self)->keys()) send(x, s); });
    def(S, "makeObjectsPerformSelector:withObject:", +[](id self, SEL, SEL s, id o) { for (id x : setx(self)->keys()) send(x, s, o); });
    def(S, "enumerateObjectsUsingBlock:", +[](id self, SEL, id block) {
        auto k = setx(self)->keys(); bool stop = false;
        auto fn = reinterpret_cast<void (*)(id, id, bool*)>(block_invoke_ptr(block));
        for (id x : k) { fn(block, x, &stop); if (stop) break; }
    });
    def(S, "description", +[](id self, SEL) -> id { return str("{(" + describe_array(setx(self)->keys()) + ")}"); });
    def(S, "copyWithZone:", +[](id self, SEL, void*) -> id { id s = alloc(C_NSSet); DictExt* e = setx(s); for (id x : setx(self)->keys()) e->put_owned(retain(x), nullptr); return s; });
    def(S, "mutableCopyWithZone:", +[](id self, SEL, void*) -> id { id s = alloc(C_NSMutableSet); DictExt* e = setx(s); for (id x : setx(self)->keys()) e->put_owned(retain(x), nullptr); return s; });
    def(S, "countByEnumeratingWithState:objects:count:", +[](id self, SEL, FastEnumState* st, id* buf, NSUInteger len) -> NSUInteger {
        DictExt* e = setx(self);
        if (st->state == 0) st->mutationsPtr = &e->mutations;
        NSUInteger n = 0;
        size_t i = st->extra[0];
        while (i < e->e.size() && n < len) { if (e->e[i].live) buf[n++] = e->e[i].k; ++i; }
        st->extra[0] = i;
        st->state = 1;
        st->itemsPtr = buf;
        return n;
    });
    def(S, "isEqualToSet:", +[](id self, SEL, id o) -> bool {
        if (!is_set_obj(o) || setx(self)->live != setx(o)->live) return false;
        for (id x : setx(self)->keys()) if (!setx(o)->get(x)) return false;
        return true;
    });
    def(MS, "addObject:", +[](id self, SEL, id o) { if (o) setx(self)->put_owned(retain(o), nullptr); });
    def(MS, "addObjectsFromArray:", +[](id self, SEL, id a) { for (id o : arr_items(a)) setx(self)->put_owned(retain(o), nullptr); });
    def(MS, "unionSet:", +[](id self, SEL, id o) { if (is_set_obj(o)) for (id x : setx(o)->keys()) setx(self)->put_owned(retain(x), nullptr); });
    def(MS, "minusSet:", +[](id self, SEL, id o) { if (is_set_obj(o)) for (id x : setx(o)->keys()) setx(self)->remove(x); });
    def(MS, "intersectSet:", +[](id self, SEL, id o) { for (id x : setx(self)->keys()) if (!setx(o)->get(x)) setx(self)->remove(x); });
    def(MS, "removeObject:", +[](id self, SEL, id o) { setx(self)->remove(o); });
    def(MS, "removeAllObjects", +[](id self, SEL) { setx(self)->clear(); });
    def(MS, "setSet:", +[](id self, SEL, id o) {
        std::vector<id> k = is_set_obj(o) ? setx(o)->keys() : std::vector<id>();
        for (id x : k) retain(x);
        setx(self)->clear();
        for (id x : k) setx(self)->put_owned(x, nullptr);
    });
}

// ---------------------------------------------------------------------------------------------------------------
// NSData
// ---------------------------------------------------------------------------------------------------------------
static void init_data() {
    C_NSData = def_class("NSData", "NSObject");
    C_NSMutableData = def_class("NSMutableData", "NSData");
    declare_protocol(C_NSData, "NSCopying");
    Class D = C_NSData, M = C_NSMutableData;
    defc(D, "data", +[](id self, SEL) -> id { id d = alloc(reinterpret_cast<Class>(self)); dtx(d); return autorelease(d); });
    defc(D, "dataWithBytes:length:", +[](id self, SEL, const void* p, NSUInteger n) -> id {
        id d = alloc(reinterpret_cast<Class>(self)); if (p && n) dtx(d)->v.assign(static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n); else dtx(d);
        return autorelease(d);
    });
    defc(D, "dataWithBytesNoCopy:length:", +[](id self, SEL, void* p, NSUInteger n) -> id {
        id d = alloc(reinterpret_cast<Class>(self)); DataExt* e = dtx(d); e->borrowed = static_cast<uint8_t*>(p); e->borrowed_len = n; e->free_borrowed = true;
        if (is_subclass(reinterpret_cast<Class>(self), C_NSMutableData)) e->own();
        return autorelease(d);
    });
    defc(D, "dataWithBytesNoCopy:length:freeWhenDone:", +[](id self, SEL, void* p, NSUInteger n, bool fr) -> id {
        id d = alloc(reinterpret_cast<Class>(self)); DataExt* e = dtx(d); e->borrowed = static_cast<uint8_t*>(p); e->borrowed_len = n; e->free_borrowed = fr;
        if (is_subclass(reinterpret_cast<Class>(self), C_NSMutableData)) e->own();
        return autorelease(d);
    });
    defc(D, "dataWithData:", +[](id self, SEL, id o) -> id {
        id d = alloc(reinterpret_cast<Class>(self)); dtx(d)->v.assign(data_bytes(o), data_bytes(o) + data_len(o)); return autorelease(d);
    });
    auto from_file = +[](id self, SEL, id path) -> id {
        std::string s;
        if (!read_file(remap_path(utf8(path)), s)) return nullptr;
        id d = alloc(reinterpret_cast<Class>(self)); dtx(d)->v.assign(s.begin(), s.end()); return autorelease(d);
    };
    defc(D, "dataWithContentsOfFile:", from_file);
    defc(D, "dataWithContentsOfMappedFile:", from_file);
    defc(D, "dataWithContentsOfFile:options:error:", +[](id self, SEL, id path, NSUInteger, id* err) -> id {
        std::string s;
        if (!read_file(remap_path(utf8(path)), s)) { if (err) *err = nullptr; return nullptr; }
        id d = alloc(reinterpret_cast<Class>(self)); dtx(d)->v.assign(s.begin(), s.end()); return autorelease(d);
    });
    defc(D, "dataWithContentsOfURL:", +[](id self, SEL, id url) -> id {
        if (!send<bool>(url, "isFileURL")) return nullptr;
        std::string s;
        if (!read_file(remap_path(utf8(send(url, "path"))), s)) return nullptr;
        id d = alloc(reinterpret_cast<Class>(self)); dtx(d)->v.assign(s.begin(), s.end()); return autorelease(d);
    });
    defc(D, "dataWithContentsOfURL:options:error:", +[](id self, SEL, id url, NSUInteger, id* err) -> id {
        if (err) *err = nullptr;
        if (!send<bool>(url, "isFileURL")) return nullptr;
        std::string s;
        if (!read_file(remap_path(utf8(send(url, "path"))), s)) return nullptr;
        id d = alloc(reinterpret_cast<Class>(self)); dtx(d)->v.assign(s.begin(), s.end()); return autorelease(d);
    });
    defc(M, "dataWithCapacity:", +[](id self, SEL, NSUInteger n) -> id { id d = alloc(reinterpret_cast<Class>(self)); dtx(d)->v.reserve(n); return autorelease(d); });
    defc(M, "dataWithLength:", +[](id self, SEL, NSUInteger n) -> id { id d = alloc(reinterpret_cast<Class>(self)); dtx(d)->v.assign(n, 0); return autorelease(d); });
    def(D, "init", +[](id self, SEL) -> id { dtx(self); return self; });
    def(D, "initWithBytes:length:", +[](id self, SEL, const void* p, NSUInteger n) -> id {
        if (p && n) dtx(self)->v.assign(static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n); else dtx(self);
        return self;
    });
    def(D, "initWithBytesNoCopy:length:", +[](id self, SEL, void* p, NSUInteger n) -> id {
        DataExt* e = dtx(self); e->borrowed = static_cast<uint8_t*>(p); e->borrowed_len = n; e->free_borrowed = true;
        if (is_kind(self, C_NSMutableData)) e->own();
        return self;
    });
    def(D, "initWithBytesNoCopy:length:freeWhenDone:", +[](id self, SEL, void* p, NSUInteger n, bool fr) -> id {
        DataExt* e = dtx(self); e->borrowed = static_cast<uint8_t*>(p); e->borrowed_len = n; e->free_borrowed = fr;
        if (is_kind(self, C_NSMutableData)) e->own();
        return self;
    });
    def(D, "initWithData:", +[](id self, SEL, id o) -> id { dtx(self)->v.assign(data_bytes(o), data_bytes(o) + data_len(o)); return self; });
    def(D, "initWithContentsOfFile:", +[](id self, SEL, id path) -> id {
        std::string s;
        if (!read_file(remap_path(utf8(path)), s)) { release(self); return nullptr; }
        dtx(self)->v.assign(s.begin(), s.end()); return self;
    });
    def(D, "initWithContentsOfURL:", +[](id self, SEL, id url) -> id {
        std::string s;
        if (!send<bool>(url, "isFileURL") || !read_file(remap_path(utf8(send(url, "path"))), s)) { release(self); return nullptr; }
        dtx(self)->v.assign(s.begin(), s.end()); return self;
    });
    def(D, "initWithBase64EncodedString:options:", +[](id self, SEL, id s, NSUInteger) -> id {
        static const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string in = utf8(s);
        std::vector<uint8_t> out;
        uint32_t acc = 0; int bits = 0;
        for (char c : in) {
            size_t k = chars.find(c);
            if (k == std::string::npos) continue;
            acc = (acc << 6) | static_cast<uint32_t>(k); bits += 6;
            if (bits >= 8) { bits -= 8; out.push_back(static_cast<uint8_t>(acc >> bits)); }
        }
        dtx(self)->v = out;
        return self;
    });
    def(D, "initWithBase64EncodedData:options:", +[](id self, SEL, id d, NSUInteger o) -> id {
        id s = str(std::string(reinterpret_cast<const char*>(data_bytes(d)), data_len(d)));
        return send(self, "initWithBase64EncodedString:options:", s, o);
    });
    auto b64 = [](id self) {
        static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        const uint8_t* p = data_bytes(self); size_t n = data_len(self);
        std::string o;
        for (size_t i = 0; i < n; i += 3) {
            uint32_t v = p[i] << 16 | (i + 1 < n ? p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
            o.push_back(chars[(v >> 18) & 63]); o.push_back(chars[(v >> 12) & 63]);
            o.push_back(i + 1 < n ? chars[(v >> 6) & 63] : '='); o.push_back(i + 2 < n ? chars[v & 63] : '=');
        }
        return o;
    };
    static decltype(b64) s_b64 = b64;
    def(D, "base64EncodedStringWithOptions:", +[](id self, SEL, NSUInteger) -> id { return str(s_b64(self)); });
    def(D, "base64Encoding", +[](id self, SEL) -> id { return str(s_b64(self)); });
    def(D, "base64EncodedDataWithOptions:", +[](id self, SEL, NSUInteger) -> id { std::string s = s_b64(self); return autorelease(data_new(s.data(), s.size())); });
    def(D, "bytes", +[](id self, SEL) -> const void* { return dtx(self)->ptr(); });
    def(D, "length", +[](id self, SEL) -> NSUInteger { return dtx(self)->len(); });
    def(D, "getBytes:length:", +[](id self, SEL, void* out, NSUInteger n) { DataExt* e = dtx(self); std::memcpy(out, e->ptr(), std::min<size_t>(n, e->len())); });
    def(D, "getBytes:range:", +[](id self, SEL, void* out, NSRange r) { DataExt* e = dtx(self); r = clamp_range(r, e->len()); std::memcpy(out, e->ptr() + r.location, r.length); });
    def(D, "getBytes:", +[](id self, SEL, void* out) { DataExt* e = dtx(self); std::memcpy(out, e->ptr(), e->len()); });
    def(D, "subdataWithRange:", +[](id self, SEL, NSRange r) -> id { DataExt* e = dtx(self); r = clamp_range(r, e->len()); return autorelease(data_new(e->ptr() + r.location, r.length)); });
    def(D, "isEqualToData:", +[](id self, SEL, id o) -> bool { return data_len(self) == data_len(o) && std::memcmp(data_bytes(self), data_bytes(o), data_len(o)) == 0; });
    def(D, "isEqual:", +[](id self, SEL, id o) -> bool { return self == o || (is_data(o) && data_len(self) == data_len(o) && std::memcmp(data_bytes(self), data_bytes(o), data_len(o)) == 0); });
    def(D, "hash", +[](id self, SEL) -> NSUInteger { return data_len(self); });
    def(D, "description", +[](id self, SEL) -> id {
        std::string s = "<"; const uint8_t* p = data_bytes(self); size_t n = data_len(self);
        for (size_t i = 0; i < n && i < 256; ++i) { char b[4]; snprintf(b, sizeof b, "%02x", p[i]); s += b; if (i % 4 == 3 && i + 1 < n) s += " "; }
        return str(s + ">");
    });
    def(D, "copyWithZone:", +[](id self, SEL, void*) -> id { if (self->isa == C_NSData) return retain(self); return data_new(data_bytes(self), data_len(self)); });
    def(D, "mutableCopyWithZone:", +[](id self, SEL, void*) -> id { id d = alloc(C_NSMutableData); dtx(d)->v.assign(data_bytes(self), data_bytes(self) + data_len(self)); return d; });
    auto write = +[](id self, SEL, id path, NSUInteger) -> bool {
        std::string p = remap_path(utf8(path));
        FILE* f = fopen(p.c_str(), "wb");
        if (!f) { log("NSData write %s failed", p.c_str()); return false; }
        fwrite(data_bytes(self), 1, data_len(self), f);
        fclose(f);
        return true;
    };
    def(D, "writeToFile:atomically:", write);
    def(D, "writeToFile:options:error:", +[](id self, SEL, id path, NSUInteger o, id* err) -> bool {
        if (err) *err = nullptr;
        return send<bool>(self, "writeToFile:atomically:", path, static_cast<int>(o & 1));
    });
    def(D, "writeToURL:atomically:", +[](id self, SEL, id url, bool a) -> bool { return send<bool>(self, "writeToFile:atomically:", send(url, "path"), static_cast<int>(a)); });
    def(D, "writeToURL:options:error:", +[](id self, SEL, id url, NSUInteger o, id* err) -> bool {
        if (err) *err = nullptr;
        return send<bool>(self, "writeToFile:atomically:", send(url, "path"), static_cast<int>(o & 1));
    });

    def(M, "init", +[](id self, SEL) -> id { dtx(self); return self; });
    def(M, "initWithCapacity:", +[](id self, SEL, NSUInteger n) -> id { dtx(self)->v.reserve(n); return self; });
    def(M, "initWithLength:", +[](id self, SEL, NSUInteger n) -> id { dtx(self)->v.assign(n, 0); return self; });
    def(M, "mutableBytes", +[](id self, SEL) -> void* { DataExt* e = dtx(self); e->own(); return e->v.data(); });
    def(M, "appendBytes:length:", +[](id self, SEL, const void* p, NSUInteger n) {
        DataExt* e = dtx(self); e->own(); e->v.insert(e->v.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
    });
    def(M, "appendData:", +[](id self, SEL, id o) {
        DataExt* e = dtx(self); e->own();
        std::vector<uint8_t> tmp(data_bytes(o), data_bytes(o) + data_len(o));
        e->v.insert(e->v.end(), tmp.begin(), tmp.end());
    });
    def(M, "setLength:", +[](id self, SEL, NSUInteger n) { DataExt* e = dtx(self); e->own(); e->v.resize(n, 0); });
    def(M, "increaseLengthBy:", +[](id self, SEL, NSUInteger n) { DataExt* e = dtx(self); e->own(); e->v.resize(e->v.size() + n, 0); });
    def(M, "setData:", +[](id self, SEL, id o) { DataExt* e = dtx(self); e->own(); std::vector<uint8_t> t(data_bytes(o), data_bytes(o) + data_len(o)); e->v = t; });
    def(M, "replaceBytesInRange:withBytes:", +[](id self, SEL, NSRange r, const void* p) {
        DataExt* e = dtx(self); e->own();
        if (r.location + r.length > e->v.size()) e->v.resize(r.location + r.length);
        std::memcpy(e->v.data() + r.location, p, r.length);
    });
    def(M, "replaceBytesInRange:withBytes:length:", +[](id self, SEL, NSRange r, const void* p, NSUInteger n) {
        DataExt* e = dtx(self); e->own(); r = clamp_range(r, e->v.size());
        std::vector<uint8_t> repl(static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
        e->v.erase(e->v.begin() + r.location, e->v.begin() + r.location + r.length);
        e->v.insert(e->v.begin() + r.location, repl.begin(), repl.end());
    });
    def(M, "resetBytesInRange:", +[](id self, SEL, NSRange r) { DataExt* e = dtx(self); e->own(); r = clamp_range(r, e->v.size()); std::memset(e->v.data() + r.location, 0, r.length); });
    def(M, "copyWithZone:", +[](id self, SEL, void*) -> id { return data_new(data_bytes(self), data_len(self)); });
}

// ---------------------------------------------------------------------------------------------------------------
// NSCharacterSet
// ---------------------------------------------------------------------------------------------------------------
static void init_charsets() {
    C_NSCharacterSet = def_class("NSCharacterSet", "NSObject");
    def_class("NSMutableCharacterSet", "NSCharacterSet");
    Class C = C_NSCharacterSet;
    defc(C, "whitespaceCharacterSet", +[](id, SEL) -> id { return charset([](char16_t c) { return c == ' ' || c == '\t' || c == 0xa0; }); });
    defc(C, "whitespaceAndNewlineCharacterSet", +[](id, SEL) -> id { return charset([](char16_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0xa0 || c == 0x85 || c == 0x0b || c == 0x0c; }); });
    defc(C, "newlineCharacterSet", +[](id, SEL) -> id { return charset([](char16_t c) { return c == '\n' || c == '\r' || c == 0x85 || c == 0x2028 || c == 0x2029; }); });
    defc(C, "decimalDigitCharacterSet", +[](id, SEL) -> id { return charset([](char16_t c) { return c >= '0' && c <= '9'; }); });
    defc(C, "letterCharacterSet", +[](id, SEL) -> id { return charset([](char16_t c) { return (c < 128 && isalpha(c)) || c >= 0xc0; }); });
    defc(C, "alphanumericCharacterSet", +[](id, SEL) -> id { return charset([](char16_t c) { return (c < 128 && isalnum(c)) || c >= 0xc0; }); });
    defc(C, "punctuationCharacterSet", +[](id, SEL) -> id { return charset([](char16_t c) { return c < 128 && ispunct(c); }); });
    defc(C, "uppercaseLetterCharacterSet", +[](id, SEL) -> id { return charset([](char16_t c) { return c < 128 && isupper(c); }); });
    defc(C, "lowercaseLetterCharacterSet", +[](id, SEL) -> id { return charset([](char16_t c) { return c < 128 && islower(c); }); });
    defc(C, "controlCharacterSet", +[](id, SEL) -> id { return charset([](char16_t c) { return c < 32 || c == 127; }); });
    defc(C, "URLQueryAllowedCharacterSet", +[](id, SEL) -> id { return charset([](char16_t c) { return c < 128 && (isalnum(c) || std::strchr("-._~!$&'()*+,;=:@/?", c)); }); });
    defc(C, "URLPathAllowedCharacterSet", +[](id, SEL) -> id { return charset([](char16_t c) { return c < 128 && (isalnum(c) || std::strchr("-._~!$&'()*+,;=:@/", c)); }); });
    defc(C, "URLHostAllowedCharacterSet", +[](id, SEL) -> id { return charset([](char16_t c) { return c < 128 && (isalnum(c) || std::strchr("-._~!$&'()*+,;=:[]", c)); }); });
    defc(C, "characterSetWithCharactersInString:", +[](id, SEL, id s) -> id {
        std::u16string chars = u16(s);
        return charset([chars](char16_t c) { return chars.find(c) != std::u16string::npos; });
    });
    defc(C, "characterSetWithRange:", +[](id, SEL, NSRange r) -> id { return charset([r](char16_t c) { return c >= r.location && c < r.location + r.length; }); });
    def(C, "invertedSet", +[](id self, SEL) -> id {
        auto f = ext_get<CharSetExt>(self)->pred;
        return charset([f](char16_t c) { return !(f && f(c)); });
    });
    def(C, "characterIsMember:", +[](id self, SEL, char16_t c) -> bool { return cs_has(self, c); });
    def(C, "copyWithZone:", +[](id self, SEL, void*) -> id { return retain(self); });
    def(C, "mutableCopyWithZone:", +[](id self, SEL, void*) -> id { return retain(self); });
    def(C, "addCharactersInString:", +[](id self, SEL, id s) {
        auto* e = ext_get<CharSetExt>(self); auto f = e->pred; std::u16string chars = u16(s);
        e->pred = [f, chars](char16_t c) { return (f && f(c)) || chars.find(c) != std::u16string::npos; };
    });
    def(C, "formUnionWithCharacterSet:", +[](id self, SEL, id o) {
        auto* e = ext_get<CharSetExt>(self); auto f = e->pred; auto g = ext_get<CharSetExt>(o)->pred;
        e->pred = [f, g](char16_t c) { return (f && f(c)) || (g && g(c)); };
    });
}

void init_foundation() {
    init_strings();
    init_numbers();
    init_enumerator();
    init_arrays();
    init_dicts();
    init_data();
    init_charsets();
}

}  // namespace app
