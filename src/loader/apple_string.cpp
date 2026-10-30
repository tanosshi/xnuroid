// Apple-ABI std::string shims.
//
// iOS's libc++ was built with _LIBCPP_ABI_ALTERNATE_STRING_LAYOUT, so a guest std::string uses the alternate
// layout (long: {char* data; size_t size; size_t cap}; short: {char data[23]; uint8_t size}; the is-long flag is
// the HIGH bit of the last byte). The guest INLINES the accessors (c_str/size/operator[]/compare), so every string
// object we hand back must have exactly this layout. The guest only imports a few out-of-line members — __init,
// the copy constructor and the destructor — which we implement here; crucially __init must be shimmed too, or it
// binds to the system libc++ (the default, data-last layout) and every subsequent inlined read is wrong.
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <new>
#include <string>
#include <unordered_map>

namespace apple_abi {

struct String {
    union {
        struct {
            char* data;
            size_t size;
            size_t cap;   // bit 63 (high bit of raw[23]) is the is-long flag
        } l;
        struct {
            char data[23];
            uint8_t size;   // short: value is (len) ; high bit 0 means short
        } s;
        uint8_t raw[24];
    };

    static constexpr size_t SHORT_CAP = 22;
    static constexpr uint64_t LONG_FLAG = (1ULL << 63);

    bool is_long() const { return (raw[23] & 0x80) != 0; }
    const char* c_str() const { return is_long() ? l.data : s.data; }
    char* data() { return is_long() ? l.data : s.data; }
    size_t size() const { return is_long() ? l.size : static_cast<size_t>(s.size); }
    size_t capacity() const { return is_long() ? (l.cap & ~LONG_FLAG) - 1 : SHORT_CAP; }

    void init_empty() { std::memset(raw, 0, 24); }

    void free_buffer() {
        if (is_long() && l.data) { free(l.data); l.data = nullptr; }
    }

    void set_short(const char* str, size_t n) {
        if (n > SHORT_CAP) n = SHORT_CAP;
        if (str && n > 0) std::memcpy(s.data, str, n);
        std::memset(s.data + n, 0, 23 - n);
        s.size = static_cast<uint8_t>(n);   // high bit stays clear = short
    }
    void set_long(char* buf, size_t n, size_t cap) {
        l.data = buf;
        l.size = n;
        l.cap = (cap + 1) | LONG_FLAG;
    }
    void assign(const char* str, size_t n) {
        if (n <= SHORT_CAP) {
            free_buffer();
            set_short(str, n);
        } else {
            size_t p = 32;
            while (p < n + 1) p <<= 1;
            size_t new_cap = p - 1;
            char* buf = static_cast<char*>(malloc(new_cap + 1));
            if (str && n > 0) std::memcpy(buf, str, n);
            buf[n] = 0;
            free_buffer();
            set_long(buf, n, new_cap);
        }
    }
    void assign(const char* str) { assign(str, str ? std::strlen(str) : 0); }

    void append(const char* str, size_t n) {
        if (!str || n == 0) return;
        size_t cur = size(), ns = cur + n;
        if (!is_long() && ns <= SHORT_CAP) {
            std::memcpy(s.data + cur, str, n);
            s.data[ns] = 0;
            s.size = static_cast<uint8_t>(ns);
        } else if (ns <= capacity()) {
            std::memcpy(l.data + cur, str, n);
            l.data[ns] = 0;
            l.size = ns;
        } else {
            size_t p = 32;
            while (p < ns + 1) p <<= 1;
            size_t new_cap = p - 1;
            char* buf = static_cast<char*>(malloc(new_cap + 1));
            if (cur > 0) std::memcpy(buf, c_str(), cur);
            std::memcpy(buf + cur, str, n);
            buf[ns] = 0;
            free_buffer();
            set_long(buf, ns, new_cap);
        }
    }
    void append(const char* str) { append(str, str ? std::strlen(str) : 0); }
    void append(size_t n, char c) { for (size_t i = 0; i < n; ++i) append(&c, 1); }
    void push_back(char c) { append(&c, 1); }

    void copy_from(const String& o) {
        if (this == &o) return;
        if (o.is_long()) { assign(o.l.data, o.l.size); }
        else { free_buffer(); std::memcpy(raw, o.raw, 24); }
    }
    void erase(size_t pos, size_t n) {
        size_t cur = size();
        if (pos >= cur) return;
        if (n > cur - pos) n = cur - pos;
        char* d = data();
        std::memmove(d + pos, d + pos + n, cur - (pos + n) + 1);
        if (is_long()) l.size -= n; else s.size -= static_cast<uint8_t>(n);
    }
    void reserve(size_t n) {
        if (n <= capacity()) return;
        size_t cur = size(), p = 32;
        while (p < n + 1) p <<= 1;
        size_t new_cap = p - 1;
        char* buf = static_cast<char*>(malloc(new_cap + 1));
        if (cur > 0) std::memcpy(buf, c_str(), cur);
        buf[cur] = 0;
        free_buffer();
        set_long(buf, cur, new_cap);
    }
    void resize(size_t n, char c) {
        size_t cur = size();
        if (n > cur) { reserve(n); char* d = data(); std::memset(d + cur, c, n - cur); d[n] = 0; if (is_long()) l.size = n; else s.size = static_cast<uint8_t>(n); }
        else if (n < cur) erase(n, cur - n);
    }
    size_t find(char c, size_t pos) const { size_t n = size(); const char* p = c_str(); for (size_t i = pos; i < n; ++i) if (p[i] == c) return i; return static_cast<size_t>(-1); }
    size_t rfind(char c, size_t pos) const { size_t n = size(); if (!n) return static_cast<size_t>(-1); if (pos >= n) pos = n - 1; const char* p = c_str(); for (ssize_t i = static_cast<ssize_t>(pos); i >= 0; --i) if (p[i] == c) return static_cast<size_t>(i); return static_cast<size_t>(-1); }
    int compare(const char* str) const { return std::strcmp(c_str(), str ? str : ""); }
};

static String* S(void* p) { return reinterpret_cast<String*>(p); }
static const String* CS(const void* p) { return reinterpret_cast<const String*>(p); }

}  // namespace apple_abi

using apple_abi::String;
using apple_abi::S;
using apple_abi::CS;

extern "C" {

// the primary char*+len constructor body: the object is raw (default-constructed by the guest inline, or garbage)
void* __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6__initEPKcm(void* t, const char* s, size_t n) { S(t)->init_empty(); S(t)->assign(s, n); return t; }
void* __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6__initEmc(void* t, size_t n, char c) { S(t)->init_empty(); S(t)->append(n, c); return t; }

void* __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6assignEPKc(void* t, const char* s) { S(t)->assign(s); return t; }
void* __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6assignEPKcm(void* t, const char* s, size_t n) { S(t)->assign(s, n); return t; }
void* __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6appendEPKc(void* t, const char* s) { S(t)->append(s); return t; }
void* __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6appendEPKcm(void* t, const char* s, size_t n) { S(t)->append(s, n); return t; }
void* __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6appendEmc(void* t, size_t n, char c) { S(t)->append(n, c); return t; }

void __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEED1Ev(void* t) { S(t)->free_buffer(); S(t)->init_empty(); }
void __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEED2Ev(void* t) { S(t)->free_buffer(); S(t)->init_empty(); }

void* __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEC1ERKS5_(void* t, const void* o) { S(t)->init_empty(); if (o) S(t)->copy_from(*CS(o)); return t; }
void* __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEC2ERKS5_(void* t, const void* o) { S(t)->init_empty(); if (o) S(t)->copy_from(*CS(o)); return t; }
void* __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEC1EPKc(void* t, const char* s) { S(t)->init_empty(); S(t)->assign(s); return t; }
void* __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEC1EPKcm(void* t, const char* s, size_t n) { S(t)->init_empty(); S(t)->assign(s, n); return t; }
void* __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEC1ERKS5_mmRKS4_(void* t, const void* o, size_t pos, size_t n, const void*) {
    S(t)->init_empty();
    if (o) { auto* x = CS(o); if (pos < x->size()) S(t)->assign(x->c_str() + pos, std::min(n, x->size() - pos)); }
    return t;
}

void* __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEaSERKS5_(void* t, const void* o) { if (o) S(t)->copy_from(*CS(o)); return t; }
void* __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEaSEPKc(void* t, const char* s) { S(t)->assign(s); return t; }
void* __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEaSEc(void* t, char c) { char b[2] = {c, 0}; S(t)->assign(b, 1); return t; }

void __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE9push_backEc(void* t, char c) { S(t)->push_back(c); }
void __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6resizeEmc(void* t, size_t n, char c) { S(t)->resize(n, c); }
void __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6resizeEm(void* t, size_t n) { S(t)->resize(n, 0); }
void __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE7reserveEm(void* t, size_t n) { S(t)->reserve(n); }
void __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE5clearEv(void* t) { if (S(t)->is_long()) { S(t)->l.data[0] = 0; S(t)->l.size = 0; } else { S(t)->s.data[0] = 0; S(t)->s.size = 0; } }
void* __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE5eraseEmm(void* t, size_t pos, size_t n) { S(t)->erase(pos, n); return t; }

void* __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6insertEmPKc(void* t, size_t pos, const char* s) {
    if (!s) return t;
    auto* str = S(t);
    size_t cur = str->size();
    if (pos > cur) pos = cur;
    std::string tmp = std::string(str->c_str(), pos) + s + std::string(str->c_str() + pos, cur - pos);
    str->assign(tmp.c_str(), tmp.size());
    return t;
}
void* __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE9__grow_byEmmmmmm(void* t, size_t old_cap, size_t delta, size_t, size_t, size_t, size_t) { S(t)->reserve(old_cap + delta); return t; }

size_t __ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE4findEcm(const void* t, char c, size_t pos) { return CS(t)->find(c, pos); }
size_t __ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE5rfindEcm(const void* t, char c, size_t pos) { return CS(t)->rfind(c, pos); }
int __ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE7compareEPKc(const void* t, const char* s) { return CS(t)->compare(s); }
int __ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE7compareEmmPKcm(const void* t, size_t pos1, size_t n1, const char* s2, size_t n2) {
    auto* str = CS(t);
    size_t len1 = std::min(n1, str->size() > pos1 ? str->size() - pos1 : 0);
    int rc = std::memcmp(str->c_str() + pos1, s2 ? s2 : "", std::min(len1, n2));
    if (rc) return rc;
    return len1 < n2 ? -1 : len1 > n2 ? 1 : 0;
}

void __ZNKSt3__121__basic_string_commonILb1EE20__throw_length_errorEv(const void*) { fprintf(stderr, "[FATAL] std::string length error\n"); std::abort(); }
void __ZNKSt3__121__basic_string_commonILb1EE20__throw_out_of_rangeEv(const void*) { fprintf(stderr, "[FATAL] std::string out of range\n"); std::abort(); }

struct RetStr { String s; };
RetStr __ZNSt3__1plIcNS_11char_traitsIcEENS_9allocatorIcEEEENS_12basic_stringIT_T0_T1_EEPKS6_RKS9_(const char* lhs, const void* rhs) {
    RetStr r; r.s.init_empty();
    if (lhs) r.s.assign(lhs);
    if (rhs) { auto* x = CS(rhs); r.s.append(x->c_str(), x->size()); }
    return r;
}
RetStr __ZNSt3__1plIcNS_11char_traitsIcEENS_9allocatorIcEEEENS_12basic_stringIT_T0_T1_EERKS9_PKS6_(const void* lhs, const char* rhs) {
    RetStr r; r.s.init_empty();
    if (lhs) { auto* x = CS(lhs); r.s.assign(x->c_str(), x->size()); }
    if (rhs) r.s.append(rhs);
    return r;
}

}  // extern "C"

void register_apple_string_shims(std::unordered_map<std::string, void*>& m) {
    auto r = [&](const char* n, void* f) { m[n] = f; };
#define A(sym) r(#sym, reinterpret_cast<void*>(&sym))
    A(__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6__initEPKcm);
    A(__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6__initEmc);
    A(__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6assignEPKc);
    A(__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6assignEPKcm);
    A(__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6appendEPKc);
    A(__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6appendEPKcm);
    A(__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6appendEmc);
    A(__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEED1Ev);
    A(__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEED2Ev);
    A(__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEC1ERKS5_);
    A(__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEC2ERKS5_);
    A(__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEC1EPKc);
    A(__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEC1EPKcm);
    A(__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEC1ERKS5_mmRKS4_);
    A(__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEaSERKS5_);
    A(__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEaSEPKc);
    A(__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEaSEc);
    A(__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE9push_backEc);
    A(__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6resizeEmc);
    A(__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6resizeEm);
    A(__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE7reserveEm);
    A(__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE5clearEv);
    A(__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE5eraseEmm);
    A(__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6insertEmPKc);
    A(__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE9__grow_byEmmmmmm);
    A(__ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE4findEcm);
    A(__ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE5rfindEcm);
    A(__ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE7compareEPKc);
    A(__ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE7compareEmmPKcm);
    A(__ZNKSt3__121__basic_string_commonILb1EE20__throw_length_errorEv);
    A(__ZNKSt3__121__basic_string_commonILb1EE20__throw_out_of_rangeEv);
    A(__ZNSt3__1plIcNS_11char_traitsIcEENS_9allocatorIcEEEENS_12basic_stringIT_T0_T1_EEPKS6_RKS9_);
    A(__ZNSt3__1plIcNS_11char_traitsIcEENS_9allocatorIcEEEENS_12basic_stringIT_T0_T1_EERKS9_PKS6_);
#undef A
}
