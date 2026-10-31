// CommonCrypto: the digests (MD5, SHA-1, SHA-256), HMAC over them, and CCCrypt with AES (CBC/ECB, PKCS#7) and RC4.
// Android gives apps no libcrypto to bind to, so these are small self-contained implementations. Context structs
// live in the guest's own CC_*_CTX / CCHmacContext storage, so each fits inside Apple's size for it.
#include "app.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>

namespace {

inline uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
inline uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
inline uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline uint32_t le32(const uint8_t* p) { return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0]; }
inline void put_be32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
inline void put_le32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24); }

// The three digests share one block-buffering shape: state words, a byte count and a 64-byte block.
template <int Words>
struct Md {
    uint32_t h[Words];
    uint64_t n;
    uint8_t buf[64];
};
using Md5 = Md<4>;      // 88 bytes  <= CC_MD5_CTX (92)
using Sha1 = Md<5>;     // 96 bytes  <= CC_SHA1_CTX (96)
using Sha256 = Md<8>;   // 104 bytes <= CC_SHA256_CTX (104)
static_assert(sizeof(Md5) <= 92 && sizeof(Sha1) <= 96 && sizeof(Sha256) <= 104, "CommonCrypto context sizes");

void md5_block(Md5& c, const uint8_t* p) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1,
        0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453,
        0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942,
        0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
        0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d,
        0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int S[16] = {7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21};
    uint32_t m[16];
    for (int i = 0; i < 16; ++i) m[i] = le32(p + 4 * i);
    uint32_t a = c.h[0], b = c.h[1], cc = c.h[2], d = c.h[3];
    for (int i = 0; i < 64; ++i) {
        uint32_t f;
        int g;
        if (i < 16) { f = (b & cc) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & cc); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ cc ^ d; g = (3 * i + 5) & 15; }
        else { f = cc ^ (b | ~d); g = (7 * i) & 15; }
        uint32_t t = d;
        d = cc;
        cc = b;
        b = b + rol(a + f + K[i] + m[g], S[(i >> 4) * 4 + (i & 3)]);
        a = t;
    }
    c.h[0] += a; c.h[1] += b; c.h[2] += cc; c.h[3] += d;
}

void sha1_block(Sha1& c, const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) w[i] = be32(p + 4 * i);
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = c.h[0], b = c.h[1], cc = c.h[2], d = c.h[3], e = c.h[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) { f = (b & cc) | (~b & d); k = 0x5a827999; }
        else if (i < 40) { f = b ^ cc ^ d; k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8f1bbcdc; }
        else { f = b ^ cc ^ d; k = 0xca62c1d6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = rol(b, 30); b = a; a = t;
    }
    c.h[0] += a; c.h[1] += b; c.h[2] += cc; c.h[3] += d; c.h[4] += e;
}

void sha256_block(Sha256& c, const uint8_t* p) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
        0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
        0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
        0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = be32(p + 4 * i);
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t v[8];
    std::memcpy(v, c.h, sizeof v);
    for (int i = 0; i < 64; ++i) {
        uint32_t s1 = ror(v[4], 6) ^ ror(v[4], 11) ^ ror(v[4], 25), ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
        uint32_t t1 = v[7] + s1 + ch + K[i] + w[i];
        uint32_t s0 = ror(v[0], 2) ^ ror(v[0], 13) ^ ror(v[0], 22), maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
        std::memmove(v + 1, v, 7 * sizeof(uint32_t));
        v[4] += t1;
        v[0] = t1 + s0 + maj;
    }
    for (int i = 0; i < 8; ++i) c.h[i] += v[i];
}

template <int W>
void md_update(Md<W>& c, const void* data, size_t len, void (*block)(Md<W>&, const uint8_t*)) {
    auto* p = static_cast<const uint8_t*>(data);
    size_t have = c.n & 63;
    c.n += len;
    if (have) {
        size_t take = std::min<size_t>(64 - have, len);
        std::memcpy(c.buf + have, p, take);
        p += take;
        len -= take;
        if (have + take < 64) return;
        block(c, c.buf);
    }
    for (; len >= 64; p += 64, len -= 64) block(c, p);
    std::memcpy(c.buf, p, len);
}

// Merkle-Damgard padding; MD5 stores the bit length and the digest little-endian, the SHAs big-endian
template <int W>
void md_final(Md<W>& c, uint8_t* out, int out_words, bool little, void (*block)(Md<W>&, const uint8_t*)) {
    uint64_t bits = c.n * 8;
    uint8_t pad[72] = {0x80};
    size_t have = c.n & 63, padlen = have < 56 ? 56 - have : 120 - have;
    uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = uint8_t(little ? bits >> (8 * i) : bits >> (56 - 8 * i));
    md_update(c, pad, padlen, block);
    md_update(c, len, 8, block);
    for (int i = 0; i < out_words; ++i) little ? put_le32(out + 4 * i, c.h[i]) : put_be32(out + 4 * i, c.h[i]);
}

void md5_init(Md5& c) { c = Md5{{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476}, 0, {}}; }
void sha1_init(Sha1& c) { c = Sha1{{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0}, 0, {}}; }
void sha256_init(Sha256& c) {
    c = Sha256{{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}, 0, {}};
}

int cc_md5_init(Md5* c) { md5_init(*c); return 1; }
int cc_md5_update(Md5* c, const void* d, uint32_t n) { md_update(*c, d, n, md5_block); return 1; }
int cc_md5_final(uint8_t* md, Md5* c) { md_final(*c, md, 4, true, md5_block); return 1; }
uint8_t* cc_md5(const void* d, uint32_t n, uint8_t* md) { Md5 c; md5_init(c); md_update(c, d, n, md5_block); md_final(c, md, 4, true, md5_block); return md; }
int cc_sha1_init(Sha1* c) { sha1_init(*c); return 1; }
int cc_sha1_update(Sha1* c, const void* d, uint32_t n) { md_update(*c, d, n, sha1_block); return 1; }
int cc_sha1_final(uint8_t* md, Sha1* c) { md_final(*c, md, 5, false, sha1_block); return 1; }
uint8_t* cc_sha1(const void* d, uint32_t n, uint8_t* md) { Sha1 c; sha1_init(c); md_update(c, d, n, sha1_block); md_final(c, md, 5, false, sha1_block); return md; }
int cc_sha256_init(Sha256* c) { sha256_init(*c); return 1; }
int cc_sha256_update(Sha256* c, const void* d, uint32_t n) { md_update(*c, d, n, sha256_block); return 1; }
int cc_sha256_final(uint8_t* md, Sha256* c) { md_final(*c, md, 8, false, sha256_block); return 1; }
uint8_t* cc_sha256(const void* d, uint32_t n, uint8_t* md) { Sha256 c; sha256_init(c); md_update(c, d, n, sha256_block); md_final(c, md, 8, false, sha256_block); return md; }

// ---- HMAC. CCHmacAlgorithm: SHA1 0, MD5 1, SHA256 2 (SHA384/512/224 are not provided: the result is zeros).
struct Hmac {   // fits CCHmacContext (384 bytes)
    uint32_t alg;
    union { Md5 md5; Sha1 sha1; Sha256 sha256; } inner, outer;
};
static_assert(sizeof(Hmac) <= 384, "CCHmacContext size");

size_t digest_len(uint32_t alg) { return alg == 0 ? 20 : alg == 1 ? 16 : alg == 2 ? 32 : 0; }

void hmac_feed(Hmac& h, bool outer, const void* d, size_t n) {
    auto& s = outer ? h.outer : h.inner;
    if (h.alg == 0) md_update(s.sha1, d, n, sha1_block);
    else if (h.alg == 1) md_update(s.md5, d, n, md5_block);
    else if (h.alg == 2) md_update(s.sha256, d, n, sha256_block);
}
void hmac_reset(Hmac& h, bool outer) {
    auto& s = outer ? h.outer : h.inner;
    if (h.alg == 0) sha1_init(s.sha1);
    else if (h.alg == 1) md5_init(s.md5);
    else sha256_init(s.sha256);
}
void hmac_digest(Hmac& h, bool outer, uint8_t* out) {
    auto& s = outer ? h.outer : h.inner;
    if (h.alg == 0) md_final(s.sha1, out, 5, false, sha1_block);
    else if (h.alg == 1) md_final(s.md5, out, 4, true, md5_block);
    else md_final(s.sha256, out, 8, false, sha256_block);
}

void cc_hmac_init(Hmac* h, uint32_t alg, const void* key, size_t key_len) {
    std::memset(h, 0, sizeof *h);
    h->alg = alg;
    if (!digest_len(alg)) return;
    uint8_t k[64] = {};
    if (key_len > 64) {   // long keys are hashed first
        hmac_reset(*h, false);
        hmac_feed(*h, false, key, key_len);
        hmac_digest(*h, false, k);
    } else if (key_len) {
        std::memcpy(k, key, key_len);
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    hmac_reset(*h, false);
    hmac_reset(*h, true);
    hmac_feed(*h, false, ipad, 64);
    hmac_feed(*h, true, opad, 64);
}
void cc_hmac_update(Hmac* h, const void* d, size_t n) { if (digest_len(h->alg)) hmac_feed(*h, false, d, n); }
void cc_hmac_final(Hmac* h, uint8_t* mac) {
    size_t len = digest_len(h->alg);
    if (!len) { std::memset(mac, 0, 64); return; }
    uint8_t inner[32];
    hmac_digest(*h, false, inner);
    hmac_feed(*h, true, inner, len);
    hmac_digest(*h, true, mac);
}
void cc_hmac(uint32_t alg, const void* key, size_t key_len, const void* d, size_t n, uint8_t* mac) {
    Hmac h;
    cc_hmac_init(&h, alg, key, key_len);
    cc_hmac_update(&h, d, n);
    cc_hmac_final(&h, mac);
}

// ---- AES (FIPS-197), byte-oriented
const uint8_t kSbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0,
    0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0,
    0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
    0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c,
    0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e,
    0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

struct Aes {
    uint8_t rk[240];
    int rounds;
    uint8_t inv[256];
};

inline uint8_t xt(uint8_t x) { return uint8_t((x << 1) ^ ((x >> 7) * 0x1b)); }
inline uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    for (; b; b >>= 1, a = xt(a)) if (b & 1) r ^= a;
    return r;
}

bool aes_init(Aes& a, const uint8_t* key, size_t len) {
    if (len != 16 && len != 24 && len != 32) return false;
    int nk = static_cast<int>(len / 4);
    a.rounds = nk + 6;
    for (int i = 0; i < 256; ++i) a.inv[kSbox[i]] = uint8_t(i);
    std::memcpy(a.rk, key, len);
    uint8_t rcon = 1;
    for (int i = nk; i < 4 * (a.rounds + 1); ++i) {
        uint8_t t[4];
        std::memcpy(t, a.rk + 4 * (i - 1), 4);
        if (i % nk == 0) {
            uint8_t f = t[0];
            t[0] = kSbox[t[1]] ^ rcon; t[1] = kSbox[t[2]]; t[2] = kSbox[t[3]]; t[3] = kSbox[f];
            rcon = xt(rcon);
        } else if (nk > 6 && i % nk == 4) {
            for (auto& b : t) b = kSbox[b];
        }
        for (int j = 0; j < 4; ++j) a.rk[4 * i + j] = a.rk[4 * (i - nk) + j] ^ t[j];
    }
    return true;
}

void aes_encrypt(const Aes& a, uint8_t* s) {
    for (int i = 0; i < 16; ++i) s[i] ^= a.rk[i];
    for (int r = 1; r <= a.rounds; ++r) {
        uint8_t t[16];
        for (int i = 0; i < 16; ++i) t[i] = kSbox[s[(i + 4 * (i % 4)) % 16]];   // SubBytes + ShiftRows
        if (r < a.rounds)
            for (int c = 0; c < 4; ++c) {   // MixColumns
                uint8_t* p = t + 4 * c;
                uint8_t x = p[0] ^ p[1] ^ p[2] ^ p[3], p0 = p[0];
                p[0] ^= x ^ xt(p[0] ^ p[1]); p[1] ^= x ^ xt(p[1] ^ p[2]); p[2] ^= x ^ xt(p[2] ^ p[3]); p[3] ^= x ^ xt(p[3] ^ p0);
            }
        for (int i = 0; i < 16; ++i) s[i] = t[i] ^ a.rk[16 * r + i];
    }
}

void aes_decrypt(const Aes& a, uint8_t* s) {
    for (int i = 0; i < 16; ++i) s[i] ^= a.rk[16 * a.rounds + i];
    for (int r = a.rounds - 1; r >= 0; --r) {
        uint8_t t[16];
        for (int i = 0; i < 16; ++i) t[(i + 4 * (i % 4)) % 16] = a.inv[s[i]];   // InvShiftRows + InvSubBytes
        for (int i = 0; i < 16; ++i) t[i] ^= a.rk[16 * r + i];
        if (r > 0)
            for (int c = 0; c < 4; ++c) {   // InvMixColumns
                uint8_t* p = t + 4 * c;
                uint8_t b0 = p[0], b1 = p[1], b2 = p[2], b3 = p[3];
                p[0] = gmul(b0, 14) ^ gmul(b1, 11) ^ gmul(b2, 13) ^ gmul(b3, 9);
                p[1] = gmul(b0, 9) ^ gmul(b1, 14) ^ gmul(b2, 11) ^ gmul(b3, 13);
                p[2] = gmul(b0, 13) ^ gmul(b1, 9) ^ gmul(b2, 14) ^ gmul(b3, 11);
                p[3] = gmul(b0, 11) ^ gmul(b1, 13) ^ gmul(b2, 9) ^ gmul(b3, 14);
            }
        std::memcpy(s, t, 16);
    }
}

constexpr int32_t kCCSuccess = 0, kCCParamError = -4300, kCCBufferTooSmall = -4301, kCCAlignmentError = -4303, kCCDecodeError = -4304,
                  kCCUnimplemented = -4305;

// CCCrypt(op, alg, options, key, keyLength, iv, dataIn, dataInLength, dataOut, dataOutAvailable, dataOutMoved)
// op: 0 encrypt / 1 decrypt; alg: 0 AES, 4 RC4; options: 1 PKCS7 padding, 2 ECB
int32_t cc_crypt(uint32_t op, uint32_t alg, uint32_t options, const void* key, size_t key_len, const void* iv, const void* in,
                            size_t in_len, void* out, size_t out_avail, size_t* moved) {
    if (moved) *moved = 0;
    if (!key || (in_len && (!in || !out))) return kCCParamError;
    auto* src = static_cast<const uint8_t*>(in);
    auto* dst = static_cast<uint8_t*>(out);
    if (alg == 4) {   // RC4
        if (out_avail < in_len) return kCCBufferTooSmall;
        uint8_t S[256];
        for (int i = 0; i < 256; ++i) S[i] = uint8_t(i);
        auto* k = static_cast<const uint8_t*>(key);
        for (int i = 0, j = 0; i < 256; ++i) {
            j = (j + S[i] + k[key_len ? i % key_len : 0]) & 255;
            std::swap(S[i], S[j]);
        }
        for (size_t n = 0, i = 0, j = 0; n < in_len; ++n) {
            i = (i + 1) & 255;
            j = (j + S[i]) & 255;
            std::swap(S[i], S[j]);
            dst[n] = src[n] ^ S[(S[i] + S[j]) & 255];
        }
        if (moved) *moved = in_len;
        return kCCSuccess;
    }
    if (alg != 0) {
        app::log("CCCrypt: algorithm %u not provided", alg);
        return kCCUnimplemented;
    }
    Aes aes;
    if (!aes_init(aes, static_cast<const uint8_t*>(key), key_len)) return kCCParamError;
    const bool pad = options & 1, ecb = options & 2;
    uint8_t chain[16] = {};
    if (iv && !ecb) std::memcpy(chain, iv, 16);
    if (op == 0) {
        size_t total = pad ? (in_len / 16 + 1) * 16 : in_len;
        if (!pad && in_len % 16) return kCCAlignmentError;
        if (out_avail < total) { if (moved) *moved = total; return kCCBufferTooSmall; }
        for (size_t off = 0; off < total; off += 16) {
            uint8_t blk[16];
            size_t take = off + 16 <= in_len ? 16 : (off < in_len ? in_len - off : 0);
            std::memcpy(blk, src + off, take);
            std::memset(blk + take, int(16 - take), 16 - take);   // PKCS#7
            if (!ecb) for (int i = 0; i < 16; ++i) blk[i] ^= chain[i];
            aes_encrypt(aes, blk);
            std::memcpy(chain, blk, 16);
            std::memcpy(dst + off, blk, 16);
        }
        if (moved) *moved = total;
        return kCCSuccess;
    }
    if (in_len % 16) return kCCAlignmentError;
    if (out_avail < in_len - (pad && in_len ? 16 : 0)) { if (moved) *moved = in_len; return kCCBufferTooSmall; }
    uint8_t last[16];
    size_t written = 0;
    for (size_t off = 0; off < in_len; off += 16) {
        uint8_t blk[16], ct[16];
        std::memcpy(ct, src + off, 16);
        std::memcpy(blk, ct, 16);
        aes_decrypt(aes, blk);
        if (!ecb) for (int i = 0; i < 16; ++i) blk[i] ^= chain[i];
        std::memcpy(chain, ct, 16);
        if (pad && off + 16 == in_len) { std::memcpy(last, blk, 16); break; }   // held back to strip the padding
        if (written + 16 > out_avail) return kCCBufferTooSmall;
        std::memcpy(dst + written, blk, 16);
        written += 16;
    }
    if (pad && in_len) {
        uint8_t n = last[15];
        if (n == 0 || n > 16) return kCCDecodeError;
        for (int i = 16 - n; i < 16; ++i) if (last[i] != n) return kCCDecodeError;
        if (written + (16 - n) > out_avail) return kCCBufferTooSmall;
        std::memcpy(dst + written, last, 16 - n);
        written += 16 - n;
    }
    if (moved) *moved = written;
    return kCCSuccess;
}

}  // namespace

namespace app {

void register_crypto_symbols(std::unordered_map<std::string, void*>& m) {
    auto r = [&](const char* n, auto f) { m[n] = reinterpret_cast<void*>(f); };
    r("_CC_MD5", &cc_md5);
    r("_CC_MD5_Init", &cc_md5_init);
    r("_CC_MD5_Update", &cc_md5_update);
    r("_CC_MD5_Final", &cc_md5_final);
    r("_CC_SHA1", &cc_sha1);
    r("_CC_SHA1_Init", &cc_sha1_init);
    r("_CC_SHA1_Update", &cc_sha1_update);
    r("_CC_SHA1_Final", &cc_sha1_final);
    r("_CC_SHA256", &cc_sha256);
    r("_CC_SHA256_Init", &cc_sha256_init);
    r("_CC_SHA256_Update", &cc_sha256_update);
    r("_CC_SHA256_Final", &cc_sha256_final);
    r("_CCHmac", &cc_hmac);
    r("_CCHmacInit", &cc_hmac_init);
    r("_CCHmacUpdate", &cc_hmac_update);
    r("_CCHmacFinal", &cc_hmac_final);
    r("_CCCrypt", &cc_crypt);
}

}  // namespace app
