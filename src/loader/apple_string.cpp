#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <string>
#include <unordered_map>

namespace apple_abi {

struct String {
    // 24 bytes
    union {
        struct {
            char* data;
            size_t size;
            size_t cap; // bit 63 is is_long flag on little-endian
        } l;
        struct {
            char data[23];
            uint8_t size; // 0..22 for short string; bit 7 is 0
        } s;
        uint8_t raw[24];
    };

    static constexpr size_t SHORT_CAP = 22;
    static constexpr uint64_t LONG_FLAG = (1ULL << 63);

    bool is_long() const {
        return (raw[23] & 0x80) != 0;
    }

    const char* c_str() const {
        return is_long() ? l.data : s.data;
    }

    char* data() {
        return is_long() ? l.data : s.data;
    }

    size_t size() const {
        return is_long() ? l.size : static_cast<size_t>(s.size);
    }

    // libc++ keeps the allocation size (capacity + 1 for the terminator) in the long-mode cap field.
    size_t capacity() const {
        return is_long() ? (l.cap & ~LONG_FLAG) - 1 : SHORT_CAP;
    }

    void init_empty() {
        std::memset(raw, 0, 24);
    }

    void free_buffer() {
        if (is_long() && l.data) {
            free(l.data);
            l.data = nullptr;
        }
    }

    void set_short(const char* str, size_t n) {
        if (n > SHORT_CAP) n = SHORT_CAP;
        if (str && n > 0) std::memcpy(s.data, str, n);
        std::memset(s.data + n, 0, 23 - n);
        s.size = static_cast<uint8_t>(n);
    }

    // cap: usable characters, excluding the terminator (the buffer holds cap + 1 bytes)
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
            size_t new_cap = std::max(n + 1, static_cast<size_t>(32));
            size_t p = 32;
            while (p < new_cap) p <<= 1;
            new_cap = p - 1; // capacity excluding null terminator
            char* buf = static_cast<char*>(malloc(new_cap + 1));
            if (str && n > 0) std::memcpy(buf, str, n);
            buf[n] = 0;
            free_buffer();
            set_long(buf, n, new_cap);
        }
    }

    void assign(const char* str) {
        assign(str, str ? std::strlen(str) : 0);
    }

    void append(const char* str, size_t n) {
        if (!str || n == 0) return;
        size_t cur_sz = size();
        size_t new_sz = cur_sz + n;
        if (!is_long() && new_sz <= SHORT_CAP) {
            std::memcpy(s.data + cur_sz, str, n);
            s.data[new_sz] = 0;
            s.size = static_cast<uint8_t>(new_sz);
        } else {
            size_t cur_cap = capacity();
            if (new_sz > cur_cap) {
                size_t new_cap = std::max(new_sz + 1, cur_cap * 2);
                size_t p = 32;
                while (p < new_cap) p <<= 1;
                new_cap = p - 1;
                char* buf = static_cast<char*>(malloc(new_cap + 1));
                if (cur_sz > 0) std::memcpy(buf, c_str(), cur_sz);
                std::memcpy(buf + cur_sz, str, n);
                buf[new_sz] = 0;
                free_buffer();
                set_long(buf, new_sz, new_cap);
            } else {
                std::memcpy(l.data + cur_sz, str, n);
                l.data[new_sz] = 0;
                l.size = new_sz;
            }
        }
    }

    void append(const char* str) {
        append(str, str ? std::strlen(str) : 0);
    }

    void append(size_t n, char c) {
        for (size_t i = 0; i < n; ++i) append(&c, 1);
    }

    void push_back(char c) {
        append(&c, 1);
    }

    void copy_from(const String& other) {
        if (this == &other) return;
        if (other.is_long()) {
            assign(other.l.data, other.l.size);
        } else {
            free_buffer();
            std::memcpy(raw, other.raw, 24);
        }
    }

    void erase(size_t pos, size_t n) {
        size_t cur_sz = size();
        if (pos >= cur_sz) return;
        if (n > cur_sz - pos) n = cur_sz - pos;
        char* d = data();
        std::memmove(d + pos, d + pos + n, cur_sz - (pos + n) + 1);
        if (is_long()) {
            l.size -= n;
        } else {
            s.size -= static_cast<uint8_t>(n);
        }
    }

    void reserve(size_t n) {
        if (n <= capacity()) return;
        size_t cur_sz = size();
        size_t new_cap = n;
        size_t p = 32;
        while (p < new_cap + 1) p <<= 1;
        new_cap = p - 1;
        char* buf = static_cast<char*>(malloc(new_cap + 1));
        if (cur_sz > 0) std::memcpy(buf, c_str(), cur_sz);
        buf[cur_sz] = 0;
        free_buffer();
        set_long(buf, cur_sz, new_cap);
    }

    void resize(size_t n, char c) {
        size_t cur_sz = size();
        if (n > cur_sz) {
            reserve(n);
            char* d = data();
            std::memset(d + cur_sz, c, n - cur_sz);
            d[n] = 0;
            if (is_long()) l.size = n;
            else s.size = static_cast<uint8_t>(n);
        } else if (n < cur_sz) {
            erase(n, cur_sz - n);
        }
    }

    size_t find(char c, size_t pos) const {
        size_t cur_sz = size();
        if (pos >= cur_sz) return static_cast<size_t>(-1);
        const char* p = c_str();
        for (size_t i = pos; i < cur_sz; ++i) {
            if (p[i] == c) return i;
        }
        return static_cast<size_t>(-1);
    }

    size_t rfind(char c, size_t pos) const {
        size_t cur_sz = size();
        if (cur_sz == 0) return static_cast<size_t>(-1);
        if (pos >= cur_sz) pos = cur_sz - 1;
        const char* p = c_str();
        for (ssize_t i = static_cast<ssize_t>(pos); i >= 0; --i) {
            if (p[i] == c) return static_cast<size_t>(i);
        }
        return static_cast<size_t>(-1);
    }

    int compare(const char* str) const {
        return std::strcmp(c_str(), str ? str : "");
    }
};

} // namespace apple_abi

extern "C" {

void* __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6assignEPKc(void* this_ptr, const char* s) {
    auto* str = reinterpret_cast<apple_abi::String*>(this_ptr);
    str->assign(s);
    return str;
}

void* __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6appendEPKc(void* this_ptr, const char* s) {
    auto* str = reinterpret_cast<apple_abi::String*>(this_ptr);
    str->append(s);
    return str;
}

void* __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6appendEPKcm(void* this_ptr, const char* s, size_t n) {
    auto* str = reinterpret_cast<apple_abi::String*>(this_ptr);
    str->append(s, n);
    return str;
}

void* __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6appendEmc(void* this_ptr, size_t n, char c) {
    auto* str = reinterpret_cast<apple_abi::String*>(this_ptr);
    str->append(n, c);
    return str;
}

void __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEED1Ev(void* this_ptr) {
    auto* str = reinterpret_cast<apple_abi::String*>(this_ptr);
    str->free_buffer();
    str->init_empty();
}

void* __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEC1ERKS5_(void* this_ptr, const void* other) {
    auto* str = reinterpret_cast<apple_abi::String*>(this_ptr);
    str->init_empty();
    if (other) str->copy_from(*reinterpret_cast<const apple_abi::String*>(other));
    return str;
}

void* __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEC1ERKS5_mmRKS4_(void* this_ptr, const void* other, size_t pos, size_t n, const void*) {
    auto* str = reinterpret_cast<apple_abi::String*>(this_ptr);
    str->init_empty();
    if (other) {
        auto* o = reinterpret_cast<const apple_abi::String*>(other);
        if (pos < o->size()) {
            size_t len = std::min(n, o->size() - pos);
            str->assign(o->c_str() + pos, len);
        }
    }
    return str;
}

void* __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEaSERKS5_(void* this_ptr, const void* other) {
    auto* str = reinterpret_cast<apple_abi::String*>(this_ptr);
    if (other) str->copy_from(*reinterpret_cast<const apple_abi::String*>(other));
    return str;
}

void* __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEaSEc(void* this_ptr, char c) {
    auto* str = reinterpret_cast<apple_abi::String*>(this_ptr);
    char buf[2] = {c, 0};
    str->assign(buf, 1);
    return str;
}

void __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE9push_backEc(void* this_ptr, char c) {
    auto* str = reinterpret_cast<apple_abi::String*>(this_ptr);
    str->push_back(c);
}

void __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6resizeEmc(void* this_ptr, size_t n, char c) {
    auto* str = reinterpret_cast<apple_abi::String*>(this_ptr);
    str->resize(n, c);
}

void __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE7reserveEm(void* this_ptr, size_t n) {
    auto* str = reinterpret_cast<apple_abi::String*>(this_ptr);
    str->reserve(n);
}

void* __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE5eraseEmm(void* this_ptr, size_t pos, size_t n) {
    auto* str = reinterpret_cast<apple_abi::String*>(this_ptr);
    str->erase(pos, n);
    return str;
}

void* __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6insertEmPKc(void* this_ptr, size_t pos, const char* s) {
    auto* str = reinterpret_cast<apple_abi::String*>(this_ptr);
    if (!s) return str;
    size_t cur_sz = str->size();
    if (pos > cur_sz) pos = cur_sz;
    std::string tmp = std::string(str->c_str(), pos) + s + std::string(str->c_str() + pos, cur_sz - pos);
    str->assign(tmp.c_str(), tmp.size());
    return str;
}

void* __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE9__grow_byEmmmmmm(void* this_ptr, size_t old_cap, size_t delta_cap, size_t, size_t, size_t, size_t) {
    auto* str = reinterpret_cast<apple_abi::String*>(this_ptr);
    str->reserve(old_cap + delta_cap);
    return str;
}

size_t __ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE4findEcm(const void* this_ptr, char c, size_t pos) {
    auto* str = reinterpret_cast<const apple_abi::String*>(this_ptr);
    return str->find(c, pos);
}

size_t __ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE5rfindEcm(const void* this_ptr, char c, size_t pos) {
    auto* str = reinterpret_cast<const apple_abi::String*>(this_ptr);
    return str->rfind(c, pos);
}

int __ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE7compareEPKc(const void* this_ptr, const char* s) {
    auto* str = reinterpret_cast<const apple_abi::String*>(this_ptr);
    return str->compare(s);
}

int __ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE7compareEmmPKcm(const void* this_ptr, size_t pos1, size_t n1, const char* s2, size_t n2) {
    auto* str = reinterpret_cast<const apple_abi::String*>(this_ptr);
    size_t len1 = std::min(n1, str->size() > pos1 ? str->size() - pos1 : 0);
    int rc = std::memcmp(str->c_str() + pos1, s2 ? s2 : "", std::min(len1, n2));
    if (rc != 0) return rc;
    if (len1 < n2) return -1;
    if (len1 > n2) return 1;
    return 0;
}

void __ZNKSt3__121__basic_string_commonILb1EE20__throw_length_errorEv(const void*) {
    fprintf(stderr, "[FATAL] std::string length error thrown\n");
    std::abort();
}

void __ZNKSt3__121__basic_string_commonILb1EE20__throw_out_of_rangeEv(const void*) {
    fprintf(stderr, "[FATAL] std::string out of range thrown\n");
    std::abort();
}

struct RetStr {
    apple_abi::String s;
};

RetStr __ZNSt3__1plIcNS_11char_traitsIcEENS_9allocatorIcEEEENS_12basic_stringIT_T0_T1_EEPKS6_RKS9_(const char* lhs, const void* rhs) {
    RetStr res;
    res.s.init_empty();
    if (lhs) res.s.assign(lhs);
    if (rhs) {
        auto* r = reinterpret_cast<const apple_abi::String*>(rhs);
        res.s.append(r->c_str(), r->size());
    }
    return res;
}

} // extern "C"

void register_apple_string_shims(std::unordered_map<std::string, void*>& shims) {
    shims["__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6assignEPKc"] = (void*)&__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6assignEPKc;
    shims["__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6appendEPKc"] = (void*)&__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6appendEPKc;
    shims["__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6appendEPKcm"] = (void*)&__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6appendEPKcm;
    shims["__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6appendEmc"] = (void*)&__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6appendEmc;
    shims["__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEED1Ev"] = (void*)&__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEED1Ev;
    shims["__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEC1ERKS5_"] = (void*)&__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEC1ERKS5_;
    shims["__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEC1ERKS5_mmRKS4_"] = (void*)&__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEC1ERKS5_mmRKS4_;
    shims["__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEaSERKS5_"] = (void*)&__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEaSERKS5_;
    shims["__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEaSEc"] = (void*)&__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEaSEc;
    shims["__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE9push_backEc"] = (void*)&__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE9push_backEc;
    shims["__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6resizeEmc"] = (void*)&__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6resizeEmc;
    shims["__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE7reserveEm"] = (void*)&__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE7reserveEm;
    shims["__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE5eraseEmm"] = (void*)&__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE5eraseEmm;
    shims["__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6insertEmPKc"] = (void*)&__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6insertEmPKc;
    shims["__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE9__grow_byEmmmmmm"] = (void*)&__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE9__grow_byEmmmmmm;
    shims["__ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE4findEcm"] = (void*)&__ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE4findEcm;
    shims["__ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE5rfindEcm"] = (void*)&__ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE5rfindEcm;
    shims["__ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE7compareEPKc"] = (void*)&__ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE7compareEPKc;
    shims["__ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE7compareEmmPKcm"] = (void*)&__ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE7compareEmmPKcm;
    shims["__ZNKSt3__121__basic_string_commonILb1EE20__throw_length_errorEv"] = (void*)&__ZNKSt3__121__basic_string_commonILb1EE20__throw_length_errorEv;
    shims["__ZNKSt3__121__basic_string_commonILb1EE20__throw_out_of_rangeEv"] = (void*)&__ZNKSt3__121__basic_string_commonILb1EE20__throw_out_of_rangeEv;
    shims["__ZNSt3__1plIcNS_11char_traitsIcEENS_9allocatorIcEEEENS_12basic_stringIT_T0_T1_EEPKS6_RKS9_"] = (void*)&__ZNSt3__1plIcNS_11char_traitsIcEENS_9allocatorIcEEEENS_12basic_stringIT_T0_T1_EEPKS6_RKS9_;
}

