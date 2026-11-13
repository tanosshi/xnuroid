// armv7: the first stage of 32-bit support. Loads a classic-dyld armv7 Mach-O into a reserved low arena, applies
// rebases and binds with 4-byte slots, then executes the ARM entry point under a small interpreter until guest code
// calls an imported symbol. Imports are trampolines in their own page, so a call is observable even though no iOS
// framework code exists yet.
#include "armv7.hpp"

#include <sys/mman.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace armv7 {
namespace {

constexpr uint32_t kImageHint = 0x10000000;
constexpr uint32_t kStubHint = 0x30000000;
constexpr uint32_t kStackHint = 0x70000000;
constexpr uint32_t kStackSize = 1u << 20;
constexpr uint32_t kStubStride = 8;
constexpr uint32_t kMaxStubs = 4096;
constexpr uint32_t kPage = 4096;

struct Segment {
    std::string name;
    uint32_t vmaddr = 0, vmsize = 0, fileoff = 0, filesize = 0;
};

struct Image {
    uint8_t* base = nullptr;
    uint32_t base_addr = 0;
    uint32_t span = 0;
    uint32_t min_vm = 0;
    uint32_t entry = 0;
    bool entry_thumb = false;
    std::vector<Segment> segments;
};

struct Stub {
    uint32_t addr = 0;
    std::string name;
};

std::vector<Stub> g_stubs;
std::unordered_map<std::string, uint32_t> g_stub_by_name;

struct Region {
    uint32_t lo = 0, hi = 0;
    const char* what = "";
};
std::vector<Region> g_regions;

bool in_regions(uint32_t addr, uint32_t len) {
    if (addr + len < addr) return false;
    for (const Region& r : g_regions)
        if (addr >= r.lo && addr + len <= r.hi) return true;
    return false;
}

uint32_t rd32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

uint16_t rd16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}

uint32_t mem32(uint32_t addr, bool* ok) {
    if (!in_regions(addr, 4)) { *ok = false; return 0; }
    return rd32(reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(addr)));
}

uint8_t mem8(uint32_t addr, bool* ok) {
    if (!in_regions(addr, 1)) { *ok = false; return 0; }
    return *reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(addr));
}

void store32(uint32_t addr, uint32_t v, bool* ok) {
    if (!in_regions(addr, 4)) { *ok = false; return; }
    std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(addr)), &v, 4);
}

void store8(uint32_t addr, uint8_t v, bool* ok) {
    if (!in_regions(addr, 1)) { *ok = false; return; }
    *reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(addr)) = v;
}

bool map_fixed(uint32_t hint, uint32_t size, void** out) {
    void* p = mmap(reinterpret_cast<void*>(static_cast<uintptr_t>(hint)), size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != MAP_FAILED && reinterpret_cast<uintptr_t>(p) == hint) { *out = p; return true; }
    if (p != MAP_FAILED) munmap(p, size);
    p = mmap(reinterpret_cast<void*>(static_cast<uintptr_t>(hint)), size, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return false;
    if (reinterpret_cast<uintptr_t>(p) + size > 0x100000000ull) { munmap(p, size); return false; }
    *out = p;
    return true;
}

uint32_t add_stub(const std::string& name) {
    auto it = g_stub_by_name.find(name);
    if (it != g_stub_by_name.end()) return it->second;
    if (g_stubs.size() >= kMaxStubs) return 0;
    uint32_t addr = kStubHint + static_cast<uint32_t>(g_stubs.size()) * kStubStride;
    g_stubs.push_back({addr, name});
    g_stub_by_name[name] = addr;
    return addr;
}

const char* stub_name(uint32_t addr) {
    if (addr < kStubHint) return nullptr;
    uint32_t idx = (addr - kStubHint) / kStubStride;
    return idx < g_stubs.size() ? g_stubs[idx].name.c_str() : nullptr;
}

// ---------------------------------------------------------------------------------------------------------------
// Rebase and bind opcodes (classic dyld), pointer size 4.
// ---------------------------------------------------------------------------------------------------------------

// dyld stores offsets as unsigned 64-bit values, so a negative delta arrives as a wrapped ULEB (an ADD_ADDR of
// -124 is 0xFFFFFFFFFFFFFF84). Keep 64 bits here and wrap the 32-bit offset at the use site.
uint64_t read_uleb(const uint8_t*& p) {
    uint64_t v = 0;
    int shift = 0;
    while (true) {
        uint8_t b = *p++;
        if (shift < 64) v |= static_cast<uint64_t>(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80)) break;
    }
    return v;
}

int32_t read_sleb(const uint8_t*& p) {
    int32_t v = 0;
    int shift = 0;
    uint8_t b;
    do {
        b = *p++;
        v |= static_cast<int32_t>(b & 0x7f) << shift;
        shift += 7;
    } while (b & 0x80);
    if (shift < 32 && (b & 0x40)) v |= -(1 << shift);
    return v;
}

bool apply_rebases(const Image& img, const uint8_t* data, uint32_t off, uint32_t size, uint32_t slide,
                   size_t* count) {
    if (!size) return true;
    const uint8_t* p = data + off;
    const uint8_t* end = p + size;
    uint32_t seg = 0, seg_off = 0;
    while (p < end) {
        uint8_t b = *p++;
        uint8_t op = b & 0xf0, imm = b & 0x0f;
        if (op == 0x00) break;
        if (op == 0x10) continue;  // SET_TYPE_IMM
        if (op == 0x20) { seg = imm; seg_off = static_cast<uint32_t>(read_uleb(p)); continue; }
        if (op == 0x30) { seg_off += static_cast<uint32_t>(read_uleb(p)); continue; }
        if (op == 0x40) { seg_off += imm * 4; continue; }
        auto rebase_one = [&](uint32_t at) -> bool {
            if (seg >= img.segments.size()) { std::fprintf(stderr, "[armv7] rebase segment %u out of range\n", seg); return false; }
            uint32_t loc = img.base_addr + (img.segments[seg].vmaddr - img.min_vm) + at;
            bool ok = true;
            uint32_t v = mem32(loc, &ok);
            if (!ok) { std::fprintf(stderr, "[armv7] rebase outside guest memory: seg %u off 0x%x loc 0x%08x\n", seg, at, loc); return false; }
            store32(loc, v + slide, &ok);
            if (!ok) { std::fprintf(stderr, "[armv7] rebase store failed at 0x%08x\n", loc); return false; }
            ++*count;
            return true;
        };
        if (op == 0x50) {  // DO_REBASE_IMM_TIMES
            for (uint8_t i = 0; i < imm; ++i) { if (!rebase_one(seg_off)) return false; seg_off += 4; }
        } else if (op == 0x60) {  // DO_REBASE_ULEB_TIMES
            uint32_t n = static_cast<uint32_t>(read_uleb(p));
            for (uint32_t i = 0; i < n; ++i) { if (!rebase_one(seg_off)) return false; seg_off += 4; }
        } else if (op == 0x70) {  // DO_REBASE_ADD_ADDR_ULEB
            if (!rebase_one(seg_off)) return false;
            seg_off += 4 + static_cast<uint32_t>(read_uleb(p));
        } else if (op == 0x80) {  // DO_REBASE_ULEB_TIMES_SKIPPING_ULEB
            uint32_t n = static_cast<uint32_t>(read_uleb(p)), skip = static_cast<uint32_t>(read_uleb(p));
            for (uint32_t i = 0; i < n; ++i) { if (!rebase_one(seg_off)) return false; seg_off += 4 + skip; }
        } else {
            std::fprintf(stderr, "[armv7] unknown rebase opcode 0x%02x\n", b);
            return false;
        }
    }
    return true;
}

bool apply_binds(const Image& img, const uint8_t* data, uint32_t off, uint32_t size, bool lazy, size_t* count,
                 size_t* missing) {
    if (!size) return true;
    const uint8_t* p = data + off;
    const uint8_t* end = p + size;
    uint32_t seg = 0, seg_off = 0, ordinal = 0;
    int32_t addend = 0;
    std::string symbol;
    while (p < end) {
        uint8_t b = *p++;
        uint8_t op = b & 0xf0, imm = b & 0x0f;
        if (op == 0x00) {  // DONE terminates each lazy entry, and the regular stream
            if (!lazy) break;
            continue;
        }
        if (op == 0x10) { ordinal = imm; continue; }
        if (op == 0x20) { ordinal = static_cast<uint32_t>(read_uleb(p)); continue; }
        if (op == 0x30) { ordinal = imm ? 0u - (16u - imm) : 0; continue; }
        if (op == 0x40) { symbol.assign(reinterpret_cast<const char*>(p)); p += symbol.size() + 1; continue; }
        if (op == 0x50) continue;  // SET_TYPE_IMM
        if (op == 0x60) { addend = read_sleb(p); continue; }
        if (op == 0x70) { seg = imm; seg_off = static_cast<uint32_t>(read_uleb(p)); continue; }
        if (op == 0x80) { seg_off += static_cast<uint32_t>(read_uleb(p)); continue; }
        auto bind_one = [&](uint32_t at) -> bool {
            if (seg >= img.segments.size()) { std::fprintf(stderr, "[armv7] bind segment %u out of range\n", seg); return false; }
            if (symbol.empty()) return true;
            uint32_t stub = add_stub(symbol);
            if (!stub) { ++*missing; return true; }
            uint32_t loc = img.base_addr + (img.segments[seg].vmaddr - img.min_vm) + at;
            bool ok = true;
            store32(loc, stub + static_cast<uint32_t>(addend), &ok);
            if (!ok) { std::fprintf(stderr, "[armv7] bind outside guest memory: %s seg %u off 0x%x loc 0x%08x\n", symbol.c_str(), seg, at, loc); return false; }
            ++*count;
            return true;
        };
        if (op == 0x90) { if (!bind_one(seg_off)) return false; seg_off += 4; }
        else if (op == 0xa0) { if (!bind_one(seg_off)) return false; seg_off += 4 + static_cast<uint32_t>(read_uleb(p)); }
        else if (op == 0xb0) { if (!bind_one(seg_off)) return false; seg_off += 4 + imm * 4; }
        else if (op == 0xc0) {
            uint32_t n = static_cast<uint32_t>(read_uleb(p)), skip = static_cast<uint32_t>(read_uleb(p));
            for (uint32_t i = 0; i < n; ++i) { if (!bind_one(seg_off)) return false; seg_off += 4 + skip; }
        } else {
            std::fprintf(stderr, "[armv7] unknown bind opcode 0x%02x\n", b);
            return false;
        }
    }
    (void)ordinal;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// ARM interpreter (ARMv7-A state, enough to run an entry point and reach an import).
// ---------------------------------------------------------------------------------------------------------------

struct Cpu {
    uint32_t r[16] = {};
    uint32_t cpsr = 0x10;
};

constexpr uint32_t kN = 1u << 31, kZ = 1u << 30, kC = 1u << 29, kV = 1u << 28;

bool cond_ok(uint32_t cond, uint32_t cpsr) {
    const bool n = cpsr & kN, z = cpsr & kZ, c = cpsr & kC, v = cpsr & kV;
    switch (cond) {
        case 0x0: return z;
        case 0x1: return !z;
        case 0x2: return c;
        case 0x3: return !c;
        case 0x4: return n;
        case 0x5: return !n;
        case 0x6: return v;
        case 0x7: return !v;
        case 0x8: return c && !z;
        case 0x9: return !c || z;
        case 0xa: return n == v;
        case 0xb: return n != v;
        case 0xc: return !z && n == v;
        case 0xd: return z || n != v;
        default: return true;
    }
}

struct Stop {
    bool hit = false;
    std::string reason;
};

void set_nz(Cpu& c, uint32_t v) {
    c.cpsr &= ~(kN | kZ);
    if (v & 0x80000000u) c.cpsr |= kN;
    if (!v) c.cpsr |= kZ;
}

// Executes one ARM-state instruction. Returns false when the interpreter must stop (and fills stop).
bool step(Cpu& c, Stop& stop) {
    const uint32_t pc = c.r[15];
    if (const char* called = stub_name(pc)) {
        stop.hit = true;
        stop.reason = called;
        return false;
    }
    if (pc == 0 || pc == 0xFFFFFFFEu) {
        stop.reason = "guest returned";
        return false;
    }
    if (pc & 3) {
        stop.reason = "unaligned pc";
        return false;
    }
    bool ok = true;
    const uint32_t instr = mem32(pc, &ok);
    if (!ok) {
        stop.reason = "pc outside mapped guest memory";
        return false;
    }
    const uint32_t cond = instr >> 28;
    c.r[15] = pc + 8;

    if (cond == 0xF) {
        // BLX immediate (unconditional): cond=1111, bits 27:25=101, H=bit24.
        if (((instr >> 25) & 7) == 5) {
            uint32_t off = ((instr & 0x00FFFFFF) << 2) | ((instr >> 24) & 1) << 1;
            if (off & 0x02000000) off |= 0xFC000000;
            c.r[14] = pc + 4;
            uint32_t target = pc + 8 + off;
            if (target & 1) { stop.reason = "entered Thumb state"; return false; }
            c.r[15] = target & ~3u;
            return true;
        }
        stop.reason = "unconditional instruction";
        return false;
    }
    if (!cond_ok(cond, c.cpsr)) { c.r[15] = pc + 4; return true; }

    const uint32_t group = (instr >> 25) & 7;

    // MOVW / MOVT
    if ((instr & 0x0FF00000) == 0x03000000 || (instr & 0x0FF00000) == 0x03400000) {
        const uint32_t rd = (instr >> 12) & 0xF;
        const uint32_t imm16 = ((instr >> 4) & 0xF000) | (instr & 0xFFF);
        if ((instr & 0x0FF00000) == 0x03000000) c.r[rd] = imm16;
        else c.r[rd] = (c.r[rd] & 0xFFFF) | (imm16 << 16);
        c.r[15] = pc + 4;
        return true;
    }

    // BX / BLX register
    if ((instr & 0x0FFFFFF0) == 0x012FFF10 || (instr & 0x0FFFFFF0) == 0x012FFF30) {
        const uint32_t rm = instr & 0xF;
        const uint32_t target = c.r[rm];
        if ((instr & 0x0FFFFFF0) == 0x012FFF30) c.r[14] = pc + 4;
        if (target & 1) { stop.reason = "entered Thumb state"; return false; }
        c.r[15] = target & ~3u;
        return true;
    }

    if (group == 0 || group == 1) {
        // MRS / MSR (flags only).
        if ((instr & 0x0FBF0FFF) == 0x010F0000) {
            c.r[(instr >> 12) & 0xF] = c.cpsr;
            c.r[15] = pc + 4;
            return true;
        }
        if ((instr & 0x0FB0FFF0) == 0x0120F000) {
            c.cpsr = (c.cpsr & 0x0FFFFFFF) | (c.r[instr & 0xF] & 0xF0000000);
            c.r[15] = pc + 4;
            return true;
        }
        // MUL / MLA
        if ((instr & 0x0FC000F0) == 0x00000090) {
            const uint32_t rd = (instr >> 16) & 0xF, rn = (instr >> 12) & 0xF, rs = (instr >> 8) & 0xF,
                           rm = instr & 0xF;
            uint32_t v = c.r[rm] * c.r[rs];
            if (instr & (1 << 21)) v += c.r[rn];
            c.r[rd] = v;
            if (instr & (1 << 20)) set_nz(c, v);
            c.r[15] = pc + 4;
            return true;
        }
        // Halfword / signed transfers are not needed yet: stop where they appear.
        if ((instr & 0x0E000090) == 0x00000090) {
            stop.reason = "halfword transfer";
            return false;
        }
        // Data processing
        const uint32_t op = (instr >> 21) & 0xF;
        const bool s = instr & (1 << 20);
        const uint32_t rn = (instr >> 16) & 0xF, rd = (instr >> 12) & 0xF;
        uint32_t op2 = 0;
        bool shcarry = c.cpsr & kC;
        if (instr & (1 << 25)) {
            const uint32_t imm = instr & 0xFF, rot = ((instr >> 8) & 0xF) * 2;
            op2 = (imm >> rot) | (imm << (32 - rot));
            if (rot) shcarry = op2 >> 31;
        } else {
            const uint32_t rm = instr & 0xF;
            const uint32_t type = (instr >> 5) & 3;
            uint32_t amount = (instr & (1 << 4)) ? c.r[(instr >> 8) & 0xF] & 0xFF : (instr >> 7) & 0x1F;
            uint32_t v = c.r[rm];
            if (amount == 0 && (instr & (1 << 4))) {
                op2 = v;
            } else if (type == 0) {
                if (amount == 0) op2 = v;
                else if (amount < 32) { shcarry = (v >> (32 - amount)) & 1; op2 = v << amount; }
                else { shcarry = amount == 32 ? (v & 1) : 0; op2 = 0; }
            } else if (type == 1) {
                if (amount == 0) amount = 32;
                if (amount < 32) { shcarry = (v >> (amount - 1)) & 1; op2 = v >> amount; }
                else { shcarry = amount == 32 ? (v >> 31) & 1 : 0; op2 = 0; }
            } else if (type == 2) {
                if (amount == 0) amount = 32;
                if (amount < 32) { shcarry = (v >> (amount - 1)) & 1; op2 = static_cast<uint32_t>(static_cast<int32_t>(v) >> amount); }
                else { shcarry = (v >> 31) & 1; op2 = (v & 0x80000000u) ? 0xFFFFFFFFu : 0; }
            } else {
                if (amount == 0) { const uint32_t cin = (c.cpsr & kC) ? 0x80000000u : 0; op2 = (cin | (v >> 1)); shcarry = v & 1; }
                else { const uint32_t a = amount & 31; op2 = a ? ((v >> a) | (v << (32 - a))) : v; shcarry = op2 >> 31; }
            }
        }
        const uint32_t a = c.r[rn];
        uint32_t res = 0;
        const uint32_t flags_before = c.cpsr;
        const uint32_t carry_in = (c.cpsr & kC) ? 1u : 0u;
        switch (op) {
            case 0x0: res = a & op2; c.cpsr = (c.cpsr & ~(kN | kZ | kC)) | (shcarry ? kC : 0); set_nz(c, res); break;
            case 0x1: res = a ^ op2; c.cpsr = (c.cpsr & ~(kN | kZ | kC)) | (shcarry ? kC : 0); set_nz(c, res); break;
            case 0x2: {
                res = a - op2;
                const bool cy = a >= op2;
                const bool ov = ((a ^ op2) & (a ^ res)) >> 31;
                c.cpsr = (c.cpsr & ~(kN | kZ | kC | kV)) | (cy ? kC : 0) | (ov ? kV : 0);
                set_nz(c, res);
                break;
            }
            case 0x3: {
                res = op2 - a;
                const bool cy = op2 >= a;
                const bool ov = ((op2 ^ a) & (op2 ^ res)) >> 31;
                c.cpsr = (c.cpsr & ~(kN | kZ | kC | kV)) | (cy ? kC : 0) | (ov ? kV : 0);
                set_nz(c, res);
                break;
            }
            case 0x4: {
                const uint64_t wide = static_cast<uint64_t>(a) + op2 + carry_in;
                res = static_cast<uint32_t>(wide);
                const bool ov = (~(a ^ op2) & (a ^ res)) >> 31;
                c.cpsr = (c.cpsr & ~(kN | kZ | kC | kV)) | ((wide >> 32) ? kC : 0) | (ov ? kV : 0);
                set_nz(c, res);
                break;
            }
            case 0x5: {
                const uint64_t wide = static_cast<uint64_t>(a) - op2 - (carry_in ? 0 : 1);
                res = static_cast<uint32_t>(wide);
                const bool cy = static_cast<uint64_t>(a) >= static_cast<uint64_t>(op2) + (carry_in ? 0 : 1);
                const bool ov = ((a ^ op2) & (a ^ res)) >> 31;
                c.cpsr = (c.cpsr & ~(kN | kZ | kC | kV)) | (cy ? kC : 0) | (ov ? kV : 0);
                set_nz(c, res);
                break;
            }
            case 0x6: {
                const uint64_t wide = static_cast<uint64_t>(op2) - a - (carry_in ? 0 : 1);
                res = static_cast<uint32_t>(wide);
                const bool cy = static_cast<uint64_t>(op2) >= static_cast<uint64_t>(a) + (carry_in ? 0 : 1);
                const bool ov = ((op2 ^ a) & (op2 ^ res)) >> 31;
                c.cpsr = (c.cpsr & ~(kN | kZ | kC | kV)) | (cy ? kC : 0) | (ov ? kV : 0);
                set_nz(c, res);
                break;
            }
            case 0x7: {
                res = op2 - a - (carry_in ? 0 : 1);
                const bool cy = static_cast<uint64_t>(op2) >= static_cast<uint64_t>(a) + (carry_in ? 0 : 1);
                const bool ov = ((op2 ^ a) & (op2 ^ res)) >> 31;
                c.cpsr = (c.cpsr & ~(kN | kZ | kC | kV)) | (cy ? kC : 0) | (ov ? kV : 0);
                set_nz(c, res);
                break;
            }
            case 0x8: res = a & op2; c.cpsr = (c.cpsr & ~(kN | kZ | kC)) | (shcarry ? kC : 0); set_nz(c, res); break;
            case 0x9: res = a ^ op2; c.cpsr = (c.cpsr & ~(kN | kZ | kC)) | (shcarry ? kC : 0); set_nz(c, res); break;
            case 0xa: {
                res = a - op2;
                const bool cy = a >= op2;
                const bool ov = ((a ^ op2) & (a ^ res)) >> 31;
                c.cpsr = (c.cpsr & ~(kN | kZ | kC | kV)) | (cy ? kC : 0) | (ov ? kV : 0);
                set_nz(c, res);
                break;
            }
            case 0xb: {
                const uint64_t wide = static_cast<uint64_t>(a) + op2;
                res = static_cast<uint32_t>(wide);
                const bool ov = (~(a ^ op2) & (a ^ res)) >> 31;
                c.cpsr = (c.cpsr & ~(kN | kZ | kC | kV)) | ((wide >> 32) ? kC : 0) | (ov ? kV : 0);
                set_nz(c, res);
                break;
            }
            case 0xc: res = a | op2; c.cpsr = (c.cpsr & ~(kN | kZ | kC)) | (shcarry ? kC : 0); set_nz(c, res); break;
            case 0xd: res = op2; c.cpsr = (c.cpsr & ~(kN | kZ | kC)) | (shcarry ? kC : 0); set_nz(c, res); break;
            case 0xe: res = a & ~op2; c.cpsr = (c.cpsr & ~(kN | kZ | kC)) | (shcarry ? kC : 0); set_nz(c, res); break;
            case 0xf: res = ~op2; c.cpsr = (c.cpsr & ~(kN | kZ | kC)) | (shcarry ? kC : 0); set_nz(c, res); break;
        }
        if (!s) c.cpsr = flags_before;   // only the S forms update flags (test forms always have S)
        const bool test_only = (op >= 8 && op <= 0xb);
        if (!test_only && rd != 15) c.r[rd] = res;
        if (!test_only && rd == 15) {
            if (res & 1) { stop.reason = "entered Thumb state"; return false; }
            c.r[15] = res & ~3u;
            return true;
        }
        c.r[15] = pc + 4;
        return true;
    }

    if (group == 2 || group == 3) {
        // LDR / STR (word or byte)
        const bool reg_off = instr & (1 << 25);
        const bool pre = instr & (1 << 24);
        const bool up = instr & (1 << 23);
        const bool byte = instr & (1 << 22);
        const bool wb = instr & (1 << 21);
        const bool load = instr & (1 << 20);
        const uint32_t rn = (instr >> 16) & 0xF, rd = (instr >> 12) & 0xF;
        uint32_t off;
        if (reg_off) {
            const uint32_t rm = instr & 0xF;
            const uint32_t type = (instr >> 5) & 3, amount = (instr >> 7) & 0x1F;
            uint32_t v = c.r[rm];
            if (amount == 0) off = v;
            else if (type == 0) off = v << amount;
            else if (type == 1) off = v >> amount;
            else if (type == 2) off = static_cast<uint32_t>(static_cast<int32_t>(v) >> amount);
            else off = (v >> amount) | (v << (32 - amount));
        } else {
            off = instr & 0xFFF;
        }
        const uint32_t base = c.r[rn];
        const uint32_t addr = pre ? (up ? base + off : base - off) : base;
        bool mem_ok = true;
        if (load) {
            uint32_t v = byte ? mem8(addr, &mem_ok) : mem32(addr, &mem_ok);
            if (!mem_ok) { stop.reason = "load outside guest memory"; return false; }
            if (rd == 15) {
                if (v & 1) { stop.reason = "entered Thumb state"; return false; }
                c.r[15] = v & ~3u;
            } else {
                c.r[rd] = v;
                c.r[15] = pc + 4;
            }
        } else {
            const uint32_t v = rd == 15 ? pc + 8 : c.r[rd];
            if (byte) store8(addr, static_cast<uint8_t>(v), &mem_ok);
            else store32(addr & ~3u, v, &mem_ok);
            if (!mem_ok) { stop.reason = "store outside guest memory"; return false; }
            c.r[15] = pc + 4;
        }
        if (!pre) c.r[rn] = up ? base + off : base - off;
        else if (wb) c.r[rn] = addr;
        return true;
    }

    if (group == 4) {
        // LDM / STM
        const bool pre = instr & (1 << 24);
        const bool up = instr & (1 << 23);
        const bool wb = instr & (1 << 21);
        const bool load = instr & (1 << 20);
        const uint32_t rn = (instr >> 16) & 0xF;
        uint32_t list = instr & 0xFFFF;
        uint32_t count = 0;
        for (uint32_t i = 0; i < 16; ++i) if (list & (1u << i)) ++count;
        uint32_t base = c.r[rn];
        uint32_t addr = up ? base + (pre ? 4 : 0) : base - 4 * count + (pre ? 0 : 4);
        bool mem_ok = true;
        for (uint32_t i = 0; i < 16; ++i) {
            if (!(list & (1u << i))) continue;
            if (load) c.r[i] = mem32(addr, &mem_ok);
            else store32(addr, c.r[i], &mem_ok);
            if (!mem_ok) { stop.reason = "block transfer outside guest memory"; return false; }
            addr += 4;
        }
        if (wb) c.r[rn] = up ? base + 4 * count : base - 4 * count;
        if (load && (list & 0x8000)) {
            const uint32_t target = c.r[15];
            if (target & 1) { stop.reason = "entered Thumb state"; return false; }
            c.r[15] = target & ~3u;
        } else {
            c.r[15] = pc + 4;
        }
        return true;
    }

    if (group == 5) {
        // B / BL
        uint32_t off = (instr & 0x00FFFFFF) << 2;
        if (off & 0x02000000) off |= 0xFC000000;
        if (instr & (1 << 24)) c.r[14] = pc + 4;  // BL
        c.r[15] = pc + 8 + off;
        return true;
    }

    if ((instr & 0x0F000000) == 0x0F000000) {
        stop.reason = "SWI";
        return false;
    }
    stop.reason = "coprocessor or unsupported instruction";
    return false;
}

// ---------------------------------------------------------------------------------------------------------------

bool parse(const uint8_t* d, size_t size, const char* path, Image& img, uint32_t& rebase_off, uint32_t& rebase_size,
           uint32_t& bind_off, uint32_t& bind_size, uint32_t& lazy_off, uint32_t& lazy_size) {
    if (size < 28 || rd32(d) != 0xFEEDFACE) {
        std::fprintf(stderr, "[armv7] %s is not a 32-bit Mach-O\n", path);
        return false;
    }
    const uint32_t ncmds = rd32(d + 16);
    uint32_t off = 28;
    uint32_t text_vm = 0;
    bool have_text = false;
    uint32_t max_vm = 0;
    rebase_off = rebase_size = bind_off = bind_size = lazy_off = lazy_size = 0;
    uint64_t entryoff = 0;
    bool have_main = false;
    for (uint32_t i = 0; i < ncmds && off + 8 <= size; ++i) {
        const uint32_t cmd = rd32(d + off), cmdsize = rd32(d + off + 4);
        if (cmdsize < 8 || off + cmdsize > size) { std::fprintf(stderr, "[armv7] malformed load command\n"); return false; }
        if (cmd == 0x1) {  // LC_SEGMENT
            Segment s;
            s.name.assign(reinterpret_cast<const char*>(d + off + 8), strnlen(reinterpret_cast<const char*>(d + off + 8), 16));
            s.vmaddr = rd32(d + off + 24);
            s.vmsize = rd32(d + off + 28);
            s.fileoff = rd32(d + off + 32);
            s.filesize = rd32(d + off + 36);
            // Keep every segment, including __PAGEZERO: dyld opcodes index the original load-command order.
            if (s.name == "__TEXT" || (!have_text && s.name != "__PAGEZERO")) { text_vm = s.vmaddr; have_text = true; }
            if (s.name != "__PAGEZERO" && s.vmaddr + s.vmsize > max_vm) max_vm = s.vmaddr + s.vmsize;
            img.segments.push_back(std::move(s));
        } else if (cmd == 0x22 || cmd == 0x80000022) {
            rebase_off = rd32(d + off + 8);
            rebase_size = rd32(d + off + 12);
            bind_off = rd32(d + off + 16);
            bind_size = rd32(d + off + 20);
            lazy_off = rd32(d + off + 32);
            lazy_size = rd32(d + off + 36);
        } else if (cmd == 0x28 || cmd == 0x80000028) {
            entryoff = rd32(d + off + 8) | (static_cast<uint64_t>(rd32(d + off + 12)) << 32);
            have_main = true;
        } else if (cmd == 0x5 && !have_main) {  // LC_UNIXTHREAD fallback
            // ARM_THREAD_STATE: r0-r12, sp, lr, pc, cpsr
            if (cmdsize >= 16 + 17 * 4 && rd32(d + off + 8) == 1) {
                img.entry = rd32(d + off + 16 + 15 * 4);
            }
        }
        off += cmdsize;
    }
    if (img.segments.empty()) { std::fprintf(stderr, "[armv7] no segments\n"); return false; }
    img.min_vm = text_vm;
    img.span = max_vm - img.min_vm;
    if (have_main && entryoff < img.span) img.entry = img.min_vm + static_cast<uint32_t>(entryoff);
    if (!img.entry) { std::fprintf(stderr, "[armv7] no entry point\n"); return false; }
    img.entry_thumb = (img.entry & 1) != 0;
    return true;
}

}  // namespace

int run_image(uint8_t* data, size_t size, const char* path) {
    Image img;
    uint32_t rebase_off = 0, rebase_size = 0, bind_off = 0, bind_size = 0, lazy_off = 0, lazy_size = 0;
    if (!parse(data, size, path, img, rebase_off, rebase_size, bind_off, bind_size, lazy_off, lazy_size)) return 1;

    void* image_map = nullptr;
    uint32_t map_span = (img.span + kPage - 1) & ~(kPage - 1);
    if (!map_fixed(kImageHint, map_span, &image_map)) {
        std::fprintf(stderr, "[armv7] cannot reserve %u bytes below 4 GB for the image\n", map_span);
        return 1;
    }
    img.base = static_cast<uint8_t*>(image_map);
    img.base_addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(image_map));
    const uint32_t slide = img.base_addr - img.min_vm;

    for (const Segment& s : img.segments) {
        if (!s.filesize) continue;   // __TEXT legitimately starts at file offset 0
        if (s.fileoff + s.filesize > size) { std::fprintf(stderr, "[armv7] segment %s beyond file\n", s.name.c_str()); return 1; }
        std::memcpy(img.base + (s.vmaddr - img.min_vm), data + s.fileoff, s.filesize);
    }

    void* stub_map = nullptr;
    if (!map_fixed(kStubHint, kPage, &stub_map)) { std::fprintf(stderr, "[armv7] cannot reserve stub page\n"); return 1; }
    void* stack_map = nullptr;
    if (!map_fixed(kStackHint, kStackSize, &stack_map)) { std::fprintf(stderr, "[armv7] cannot reserve stack\n"); return 1; }

    g_regions.push_back({img.base_addr, img.base_addr + map_span, "image"});
    g_regions.push_back({kStubHint, kStubHint + kPage, "stubs"});
    g_regions.push_back({kStackHint, kStackHint + kStackSize, "stack"});

    size_t rebases = 0, binds = 0, missing = 0;
    if (!apply_rebases(img, data, rebase_off, rebase_size, slide, &rebases)) return 1;
    for (const Segment& s : img.segments)
        if (s.vmaddr == img.min_vm) {
            std::printf("[armv7] mapped %s at 0x%08x (span 0x%x), slide 0x%08x\n", s.name.c_str(), img.base_addr,
                        map_span, slide);
        }
    if (!apply_binds(img, data, bind_off, bind_size, false, &binds, &missing)) return 1;
    if (!apply_binds(img, data, lazy_off, lazy_size, true, &binds, &missing)) return 1;

    const uint32_t entry_va = img.entry;
    img.entry = img.base_addr + (entry_va - img.min_vm);
    std::printf("[armv7] %zu rebases, %zu binds, %zu import stubs (%zu skipped)\n", rebases, binds, g_stubs.size(),
                missing);
    std::printf("[armv7] entry 0x%08x (%s)\n", img.entry, img.entry_thumb ? "Thumb" : "ARM");
    if (img.entry_thumb) {
        std::printf("[armv7] stopped: Thumb entry is not implemented yet\n");
        return 1;
    }

    Cpu cpu;
    cpu.r[15] = img.entry;
    cpu.r[14] = 0xFFFFFFFEu;
    cpu.r[13] = kStackHint + kStackSize - 64;
    cpu.r[0] = 1;  // argc; no argv yet
    std::printf("[armv7] executing guest code...\n");

    const char* trace_env = std::getenv("LOADER_ARMV7_TRACE");
    const int trace = trace_env ? std::atoi(trace_env) : 12;
    const char* steps_env = std::getenv("LOADER_ARMV7_STEPS");
    const uint64_t budget = steps_env ? std::strtoull(steps_env, nullptr, 0) : 2000000ull;

    Stop stop;
    uint64_t steps = 0;
    for (; steps < budget; ++steps) {
        const uint32_t pc = cpu.r[15];
        if (static_cast<int>(steps) < trace)
            std::printf("[armv7] %08llx  %08x  r0=%08x r1=%08x sp=%08x lr=%08x\n",
                        static_cast<unsigned long long>(pc), [&] { bool ok = true; return mem32(pc, &ok); }(),
                        cpu.r[0], cpu.r[1], cpu.r[13], cpu.r[14]);
        if (!step(cpu, stop)) break;
    }
    if (!stop.hit && stop.reason.empty() && steps >= budget) stop.reason = "instruction budget exhausted";

    auto describe = [](uint32_t v) {
        const char* n = stub_name(v);
        return n ? std::string(n) : std::string();
    };
    if (stop.hit) {
        std::printf("[armv7] sign of life: ran %llu guest instructions, then called %s\n",
                    static_cast<unsigned long long>(steps), stop.reason.c_str());
        for (int i = 0; i < 4; ++i) {
            const std::string named = describe(cpu.r[i]);
            std::printf("[armv7]   r%d = 0x%08x%s\n", i, cpu.r[i], named.empty() ? "" : ("  (" + named + ")").c_str());
        }
    }
    else
        std::printf("[armv7] stopped after %llu instructions: %s (pc=0x%08x)\n",
                    static_cast<unsigned long long>(steps), stop.reason.c_str(), cpu.r[15]);
    return stop.hit ? 0 : 1;
}

}  // namespace armv7
