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
    // Symbols defined in this image. A bind to one of these is not an import: it resolves to guest code or data.
    std::unordered_map<std::string, uint32_t> defined;
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
// Absolute addresses of symbols exported by side images (the bundled Swift runtime), and the byte ranges of every
// loaded image, so a guest call may cross from the app into the runtime.
std::unordered_map<std::string, uint32_t> g_extern;
std::vector<std::pair<uint32_t, uint32_t>> g_code_ranges;
uint32_t g_swift_once = 0;   // resolved _swift_once, bit 0 set when the runtime exports it as Thumb

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

uint16_t mem16(uint32_t addr, bool* ok) {
    if (!in_regions(addr, 2)) { *ok = false; return 0; }
    return rd16(reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(addr)));
}

void store16(uint32_t addr, uint16_t v, bool* ok) {
    if (!in_regions(addr, 2)) { *ok = false; return; }
    std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(addr)), &v, 2);
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

// A stripped app keeps almost nothing in LC_SYMTAB; the export trie is where its own globals (the Swift
// global-initializer function and its token, for one) are named. Self-binds must resolve against it.
void parse_exports(const uint8_t* d, size_t size, uint32_t off, uint32_t len, Image& img) {
    if (!len || off > size || len > size - off) return;
    const uint8_t* const base = d + off;
    const uint8_t* const end = base + len;
    struct Frame { uint32_t node; std::string prefix; };
    std::vector<Frame> stack;
    stack.push_back({0, {}});
    size_t visited = 0;
    while (!stack.empty() && visited++ < 200000) {
        Frame f = std::move(stack.back());
        stack.pop_back();
        if (f.node >= len) continue;
        const uint8_t* p = base + f.node;
        const uint64_t terminal = read_uleb(p);
        if (terminal && p < end) {
            const uint64_t flags = read_uleb(p);
            if (!(flags & 0x08) && p < end) {   // 0x08: a re-export has an ordinal, not an address
                const uint32_t addr = static_cast<uint32_t>(read_uleb(p));
                // The export trie keeps bit 0 for Thumb functions; LC_SYMTAB clears it. Prefer the trie.
                if (addr) img.defined[f.prefix] = addr;
            }
        }
        if (p >= end) continue;
        const uint64_t children = read_uleb(p);
        for (uint64_t c = 0; c < children && p < end; ++c) {
            const char* edge = reinterpret_cast<const char*>(p);
            const size_t n = strnlen(edge, static_cast<size_t>(end - p));
            p += n + 1;
            if (p > end) break;
            const uint64_t child = read_uleb(p);
            stack.push_back({static_cast<uint32_t>(child), f.prefix + std::string(edge, n)});
        }
    }
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
            uint32_t target = 0;
            auto def = img.defined.find(symbol);
            auto ext = g_extern.find(symbol);
            if (def != img.defined.end()) {
                target = img.base_addr + (def->second - img.min_vm);   // self-bind: guest code or data
            } else if (ext != g_extern.end()) {
                target = ext->second;   // a side image (the bundled Swift runtime) defines it
            } else {
                target = add_stub(symbol);
                if (!target) { ++*missing; return true; }
            }
            const uint32_t stub = target;
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
    bool thumb = false;
    uint8_t it_count = 0, it_index = 0;   // pending IT block (Thumb only)
    uint8_t it_cond[4] = {};
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
            const uint32_t target = pc + 8 + off;
            c.thumb = true;   // BLX immediate always switches to Thumb
            c.r[15] = target & ~1u;
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
        c.thumb = (target & 1) != 0;
        c.r[15] = target & ~1u;
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
            c.thumb = (res & 1) != 0;
            c.r[15] = res & ~1u;
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
                c.thumb = (v & 1) != 0;
                c.r[15] = v & ~1u;
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
            c.thumb = (target & 1) != 0;
            c.r[15] = target & ~1u;
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
    uint32_t symoff = 0, nsyms = 0, stroff = 0, strsize = 0;
    bool have_symtab = false;
    uint32_t export_off = 0, export_size = 0;
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
            export_off = rd32(d + off + 40);
            export_size = rd32(d + off + 44);
        } else if (cmd == 0x2) {  // LC_SYMTAB
            symoff = rd32(d + off + 8);
            nsyms = rd32(d + off + 12);
            stroff = rd32(d + off + 16);
            strsize = rd32(d + off + 20);
            have_symtab = true;
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
    if (have_symtab && symoff + static_cast<uint64_t>(nsyms) * 12 <= size && stroff + strsize <= size) {
        for (uint32_t i = 0; i < nsyms; ++i) {
            const uint8_t* sym = d + symoff + i * 12;
            const uint32_t n_strx = rd32(sym);
            const uint8_t n_type = sym[4];
            const uint32_t n_value = rd32(sym + 8);
            if (n_strx >= strsize) continue;
            const uint8_t kind = n_type & 0x0e;
            if (kind != 0x0e && kind != 0x02) continue;   // N_SECT or N_ABS
            if (!n_value) continue;
            const char* name = reinterpret_cast<const char*>(d + stroff + n_strx);
            if (!*name) continue;
            img.defined.emplace(name, n_value);   // first definition wins
        }
    }
    if (img.segments.empty()) { std::fprintf(stderr, "[armv7] no segments\n"); return false; }
    img.min_vm = text_vm;
    img.span = max_vm - img.min_vm;
    parse_exports(d, size, export_off, export_size, img);
    if (have_main && entryoff < img.span) img.entry = img.min_vm + static_cast<uint32_t>(entryoff);
    img.entry_thumb = (img.entry & 1) != 0;
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// Thumb / T32 decoder. libswiftCore is built Thumb-2, so every runtime entry point arrives here. The subset below is
// driven by what the runtime actually executes: 16-bit data processing, loads and stores, branches, push/pop and the
// 32-bit BL, MOVW/MOVT, load/store forms and IT blocks that clang emits.
// ---------------------------------------------------------------------------------------------------------------

void thumb_set_pc(Cpu& c, uint32_t v) {
    c.thumb = (v & 1) != 0;
    c.r[15] = v & ~1u;
}

bool step_thumb(Cpu& c, Stop& stop) {
    const uint32_t pc = c.r[15];
    if (const char* called = stub_name(pc)) {
        stop.hit = true;
        stop.reason = called;
        return false;
    }
    if (pc == 0 || pc == 0xFFFFFFFEu) { stop.reason = "guest returned"; return false; }
    bool ok = true;
    const uint32_t hw = mem16(pc, &ok);
    if (!ok) { stop.reason = "pc outside mapped guest memory"; return false; }

    uint32_t cond = 0xE;
    if (c.it_count) {
        cond = c.it_cond[c.it_index++];
        if (c.it_index >= c.it_count) { c.it_count = 0; c.it_index = 0; }
    }
    const bool is32 = ((hw >> 11) >= 0x1D);   // 11101 / 11110 / 11111 prefixes
    c.r[15] = pc + (is32 ? 4 : 2);
    if (cond != 0xE && !cond_ok(cond, c.cpsr)) return true;

    auto write = [&](uint32_t rd, uint32_t v) {
        if (rd == 15) thumb_set_pc(c, v);   // bx-mov to pc selects the state from bit 0
        else c.r[rd] = v;
    };
    auto addf = [&](uint32_t a, uint32_t b, uint32_t carry) -> uint32_t {
        const uint64_t wide = static_cast<uint64_t>(a) + b + carry;
        const uint32_t res = static_cast<uint32_t>(wide);
        c.cpsr &= ~(kN | kZ | kC | kV);
        if (res & 0x80000000u) c.cpsr |= kN;
        if (!res) c.cpsr |= kZ;
        if (wide >> 32) c.cpsr |= kC;
        if ((~(a ^ b) & (a ^ res)) & 0x80000000u) c.cpsr |= kV;
        return res;
    };
    auto subf = [&](uint32_t a, uint32_t b, uint32_t borrow) -> uint32_t {
        const uint64_t wide = static_cast<uint64_t>(a) - b - borrow;
        const uint32_t res = static_cast<uint32_t>(wide);
        c.cpsr &= ~(kN | kZ | kC | kV);
        if (res & 0x80000000u) c.cpsr |= kN;
        if (!res) c.cpsr |= kZ;
        if (static_cast<uint64_t>(a) >= static_cast<uint64_t>(b) + borrow) c.cpsr |= kC;
        if (((a ^ b) & (a ^ res)) & 0x80000000u) c.cpsr |= kV;
        return res;
    };
    auto branch = [&](uint32_t target, bool link) {
        if (link) c.r[14] = (pc + (is32 ? 4 : 2)) | 1u;
        c.thumb = true;
        c.r[15] = target & ~1u;
    };
    auto unhandled = [&](const char* what) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "%s at pc=0x%08x (hw=0x%04x)", what, pc, hw);
        stop.reason = buf;
        return false;
    };

    if (!is32) {
        if (hw < 0x2000) {
            const uint32_t kind = (hw >> 11) & 3;
            const uint32_t rd = hw & 7, rm = (hw >> 3) & 7;
            if (kind < 3) {
                uint32_t shift = (hw >> 6) & 0x1F;
                uint32_t v = c.r[rm];
                if (kind == 0) v = shift ? (v << shift) : v;
                else if (kind == 1) v = shift ? (v >> shift) : 0;
                else v = shift ? static_cast<uint32_t>(static_cast<int32_t>(v) >> shift) : v;
                write(rd, v);
                set_nz(c, v);
                return true;
            }
            const uint32_t subop = (hw >> 9) & 3;
            const uint32_t b = (subop & 2) ? ((hw >> 6) & 7) : c.r[rm];
            const uint32_t v = (subop & 1) ? subf(c.r[rd], b, 0) : addf(c.r[rd], b, 0);
            write(rd, v);
            return true;
        }
        if (hw < 0x4000) {
            const uint32_t sub = (hw >> 11) & 3;
            const uint32_t rd = (hw >> 8) & 7, imm = hw & 0xFF;
            if (sub == 0) { write(rd, imm); set_nz(c, imm); }
            else if (sub == 1) subf(c.r[rd], imm, 0);
            else if (sub == 2) write(rd, addf(c.r[rd], imm, 0));
            else write(rd, subf(c.r[rd], imm, 0));
            return true;
        }
        if (hw < 0x4400) {   // data processing, register
            const uint32_t op = (hw >> 6) & 0xF, rd = hw & 7, rm = (hw >> 3) & 7;
            const uint32_t a = c.r[rd], b = c.r[rm];
            switch (op) {
                case 0x0: write(rd, a & b); set_nz(c, c.r[rd]); break;
                case 0x1: write(rd, a ^ b); set_nz(c, c.r[rd]); break;
                case 0x2: { uint32_t s = b & 0xFF; write(rd, s >= 32 ? 0 : a << s); set_nz(c, c.r[rd]); break; }
                case 0x3: { uint32_t s = b & 0xFF; write(rd, s >= 32 ? 0 : a >> s); set_nz(c, c.r[rd]); break; }
                case 0x4: { uint32_t s = b & 0xFF; write(rd, s >= 32 ? (a >> 31) : static_cast<uint32_t>(static_cast<int32_t>(a) >> s)); set_nz(c, c.r[rd]); break; }
                case 0x5: write(rd, addf(a, b, (c.cpsr & kC) ? 1 : 0)); break;
                case 0x6: write(rd, subf(a, b, (c.cpsr & kC) ? 1 : 0)); break;
                case 0x7: { uint32_t s = b & 0xFF; s &= 31; write(rd, s ? ((a >> s) | (a << (32 - s))) : a); set_nz(c, c.r[rd]); break; }
                case 0x8: subf(a, b, 0); break;
                case 0x9: write(rd, subf(0, b, 0)); break;
                case 0xA: subf(a, b, 0); break;
                case 0xB: addf(a, b, 0); break;
                case 0xC: write(rd, a | b); set_nz(c, c.r[rd]); break;
                case 0xD: write(rd, a * b); set_nz(c, c.r[rd]); break;
                case 0xE: write(rd, a & ~b); set_nz(c, c.r[rd]); break;
                default: write(rd, ~b); set_nz(c, c.r[rd]); break;
            }
            return true;
        }
        if (hw < 0x4800) {   // high register operations, bx/blx
            const uint32_t op = (hw >> 8) & 3;
            const uint32_t rd = (hw & 7) | ((hw >> 4) & 8), rm = (hw >> 3) & 0xF;
            const uint32_t pc_val = (pc + 4) & ~3u;   // Thumb reads PC with bit 1 cleared
            const uint32_t rm_val = rm == 15 ? pc_val : c.r[rm];
            const uint32_t rd_val = rd == 15 ? pc_val : c.r[rd];
            if (op == 0) { const uint32_t v = rd_val + rm_val; if (rd == 15) branch(v, false); else c.r[rd] = v; }
            else if (op == 1) subf(rd_val, rm_val, 0);
            else if (op == 2) { if (rd == 15) branch(rm_val, false); else c.r[rd] = rm_val; }
            else { if (hw & 0x80) c.r[14] = (pc + 2) | 1u; thumb_set_pc(c, rm_val); }
            return true;
        }
        if (hw < 0x5000) {   // ldr literal
            const uint32_t rt = (hw >> 8) & 7, imm = (hw & 0xFF) << 2;
            const uint32_t v = mem32(((pc + 4) & ~3u) + imm, &ok);
            if (!ok) { stop.reason = "thumb literal outside memory"; return false; }
            write(rt, v);
            return true;
        }
        if (hw < 0x6000) {   // load/store, register offset
            const uint32_t op = (hw >> 9) & 7, rm = (hw >> 6) & 7, rn = (hw >> 3) & 7, rt = hw & 7;
            const uint32_t addr = c.r[rn] + c.r[rm];
            uint32_t v = 0;
            switch (op) {
                case 0: store32(addr, c.r[rt], &ok); break;
                case 1: store16(addr, static_cast<uint16_t>(c.r[rt]), &ok); break;
                case 2: store8(addr, static_cast<uint8_t>(c.r[rt]), &ok); break;
                case 3: v = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(mem8(addr, &ok)))); write(rt, v); break;
                case 4: v = mem32(addr, &ok); write(rt, v); break;
                case 5: v = mem16(addr, &ok); write(rt, v); break;
                case 6: v = mem8(addr, &ok); write(rt, v); break;
                default: v = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(mem16(addr, &ok)))); write(rt, v); break;
            }
            if (!ok) { stop.reason = "thumb transfer outside memory"; return false; }
            return true;
        }
        if (hw < 0x7000) {   // load/store, immediate
            const uint32_t op = (hw >> 11) & 3, imm = (hw >> 6) & 0x1F, rn = (hw >> 3) & 7, rt = hw & 7;
            const uint32_t addr = c.r[rn] + (op == 0 ? imm << 2 : imm);
            if (op == 0) store32(addr, c.r[rt], &ok);
            else if (op == 1) write(rt, mem32(addr, &ok));
            else if (op == 2) store8(addr, static_cast<uint8_t>(c.r[rt]), &ok);
            else write(rt, mem8(addr, &ok));
            if (!ok) { stop.reason = "thumb transfer outside memory"; return false; }
            return true;
        }
        if (hw < 0x8000) {   // load/store byte, register offset
            const uint32_t rn = (hw >> 3) & 7, rm = (hw >> 6) & 7, rt = hw & 7;
            const uint32_t addr = c.r[rn] + c.r[rm];
            if (hw & 0x800) write(rt, mem8(addr, &ok));
            else store8(addr, static_cast<uint8_t>(c.r[rt]), &ok);
            if (!ok) { stop.reason = "thumb transfer outside memory"; return false; }
            return true;
        }
        if (hw < 0x9000) {   // load/store halfword, immediate
            const uint32_t imm = ((hw >> 6) & 0x1F) << 1, rn = (hw >> 3) & 7, rt = hw & 7;
            const uint32_t addr = c.r[rn] + imm;
            if (hw & 0x800) write(rt, mem16(addr, &ok));
            else store16(addr, static_cast<uint16_t>(c.r[rt]), &ok);
            if (!ok) { stop.reason = "thumb transfer outside memory"; return false; }
            return true;
        }
        if (hw < 0xA000) {   // sp-relative load/store
            const uint32_t rt = (hw >> 8) & 7, imm = (hw & 0xFF) << 2, addr = c.r[13] + imm;
            if (hw & 0x800) write(rt, mem32(addr, &ok));
            else store32(addr, c.r[rt], &ok);
            if (!ok) { stop.reason = "thumb transfer outside memory"; return false; }
            return true;
        }
        if (hw < 0xB000) {   // adr
            const uint32_t rd = (hw >> 8) & 7, imm = (hw & 0xFF) << 2;
            const uint32_t base = (hw & 0x800) ? (c.r[13] & ~3u) : ((pc + 4) & ~3u);
            write(rd, base + imm);
            return true;
        }
        if (hw < 0xC000) {   // misc: IT, sp adjust, push/pop, cbz, extend
            if ((hw & 0xFF00) == 0xBF00) {
                const uint32_t mask = hw & 0xF;
                if (!mask) return true;   // nop / yield / wfe
                const uint8_t base = static_cast<uint8_t>((hw >> 4) & 0xF);
                c.it_cond[0] = base;
                c.it_count = 1;
                c.it_index = 0;
                switch (mask) {
                    case 0x8: break;
                    case 0xC: c.it_cond[1] = base; c.it_count = 2; break;
                    case 0x4: c.it_cond[1] = base ^ 1; c.it_count = 2; break;
                    case 0xE: c.it_cond[1] = base; c.it_cond[2] = base; c.it_count = 3; break;
                    case 0x6: c.it_cond[1] = base ^ 1; c.it_cond[2] = base; c.it_count = 3; break;
                    case 0x2: c.it_cond[1] = base ^ 1; c.it_cond[2] = base ^ 1; c.it_count = 3; break;
                    default: return unhandled("thumb IT mask");
                }
                return true;
            }
            if ((hw & 0xFF00) == 0xB000) { c.r[13] += (hw & 0x7F) << 2; return true; }
            if ((hw & 0xFF00) == 0xB080) { c.r[13] -= (hw & 0x7F) << 2; return true; }
            if ((hw & 0xFE00) == 0xB400) {   // push
                uint32_t list = hw & 0xFF, count = 0;
                for (uint32_t i = 0; i < 8; ++i) if (list & (1u << i)) ++count;
                if (hw & 0x100) ++count;
                uint32_t addr = c.r[13] - 4 * count;
                c.r[13] = addr;
                for (uint32_t i = 0; i < 8; ++i) if (list & (1u << i)) { store32(addr, c.r[i], &ok); addr += 4; }
                if (hw & 0x100) store32(addr, c.r[14], &ok);
                if (!ok) { stop.reason = "thumb push outside memory"; return false; }
                return true;
            }
            if ((hw & 0xFE00) == 0xBC00) {   // pop
                uint32_t list = hw & 0xFF, addr = c.r[13];
                for (uint32_t i = 0; i < 8; ++i) if (list & (1u << i)) { c.r[i] = mem32(addr, &ok); addr += 4; }
                if (hw & 0x100) { const uint32_t v = mem32(addr, &ok); addr += 4; c.r[13] = addr; if (!ok) { stop.reason = "thumb pop outside memory"; return false; } branch(v, false); return true; }
                c.r[13] = addr;
                if (!ok) { stop.reason = "thumb pop outside memory"; return false; }
                return true;
            }
            if ((hw & 0xF500) == 0xB100) {   // cbz / cbnz
                const uint32_t rn = hw & 7;
                const uint32_t off = ((hw >> 9) & 1) << 6 | ((hw >> 3) & 0x1F) << 1;
                const bool zero = c.r[rn] == 0;
                if (zero == ((hw & 0x800) == 0)) branch(pc + 4 + off, false);
                return true;
            }
            if ((hw & 0xFF00) == 0xB200) {   // sxth/sxtb/uxth/uxtb
                const uint32_t op = (hw >> 6) & 3, rm = (hw >> 3) & 7, rd = hw & 7;
                const uint32_t v = c.r[rm];
                if (op == 0) write(rd, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(v))));
                else if (op == 1) write(rd, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(v))));
                else if (op == 2) write(rd, v & 0xFFFF);
                else write(rd, v & 0xFF);
                return true;
            }
            return unhandled("thumb misc");
        }
        if (hw >= 0xC000 && hw < 0xD000) {   // ldm/stm
            const bool load = (hw & 0x800) != 0;
            const uint32_t rn = (hw >> 8) & 7;
            const uint32_t list = hw & 0xFF;
            uint32_t addr = c.r[rn];
            bool wrote_base = false;
            for (uint32_t i = 0; i < 8; ++i) {
                if (!(list & (1u << i))) continue;
                if (load) c.r[i] = mem32(addr, &ok);
                else store32(addr, c.r[i], &ok);
                addr += 4;
                if (i == rn) wrote_base = true;
            }
            if (!ok) { stop.reason = "thumb block transfer outside memory"; return false; }
            if (!load || !wrote_base) c.r[rn] = addr;
            return true;
        }
        if (hw < 0xE000) {   // conditional branch
            const uint32_t cc = (hw >> 8) & 0xF;
            if (cc == 0xF) return unhandled("svc");
            if (cc == 0xE) return unhandled("udf");
            if (cond_ok(cc, c.cpsr)) {
                uint32_t off = (hw & 0xFF) << 1;
                if (off & 0x100) off |= 0xFFFFFE00u;
                branch(pc + 4 + off, false);
            }
            return true;
        }
        uint32_t off = (hw & 0x7FF) << 1;
        if (off & 0x800) off |= 0xFFFFF000u;
        branch(pc + 4 + off, false);
        return true;
    }

    // ---------------- 32-bit encodings ----------------
    const uint32_t hw2 = mem16(pc + 2, &ok);
    if (!ok) { stop.reason = "pc outside mapped guest memory"; return false; }
    const uint32_t instr = (hw << 16) | hw2;

    if ((instr & 0xF800D000) == 0xF000D000) {   // BL / BLX immediate
        const uint32_t s = (instr >> 24) & 1, j1 = (instr >> 13) & 1, j2 = (instr >> 11) & 1;
        const uint32_t i1 = (~(j1 ^ s)) & 1, i2 = (~(j2 ^ s)) & 1;
        uint32_t off = (s << 24) | (i1 << 23) | (i2 << 22) | (((instr >> 12) & 0x3FF) << 12) | ((instr & 0x7FF) << 1);
        if (off & 0x02000000u) off |= 0xFC000000u;
        branch(pc + 4 + off, true);
        return true;
    }
    if ((hw & 0xF800) == 0xF000 && (hw2 & 0xD000) == 0x9000) {   // B.W (T4)
        const uint32_t s = (hw >> 10) & 1, j1 = (hw2 >> 13) & 1, j2 = (hw2 >> 11) & 1;
        const uint32_t i1 = (~(j1 ^ s)) & 1, i2 = (~(j2 ^ s)) & 1;
        uint32_t off = (s << 24) | (i1 << 23) | (i2 << 22) | ((hw & 0x3FF) << 12) | ((hw2 & 0x7FF) << 1);
        if (off & 0x02000000u) off |= 0xFC000000u;
        branch(pc + 4 + off, false);
        return true;
    }
    if ((hw & 0xFFF0) == 0xF240 || (hw & 0xFFF0) == 0xF2C0) {   // MOVW / MOVT
        const uint32_t rd = (instr >> 8) & 0xF;
        const uint32_t imm16 = ((instr >> 16) & 0xF) << 12 | (((instr >> 26) & 1) << 11) | (((instr >> 12) & 7) << 8) |
                               (instr & 0xFF);
        if ((instr & 0x00800000) != 0) c.r[rd] = (c.r[rd] & 0xFFFF) | (imm16 << 16);
        else c.r[rd] = imm16;
        return true;
    }
    const uint32_t hw1op = hw & 0xFFF0;
    if (hw1op == 0xF8C0 || hw1op == 0xF8D0 || hw1op == 0xF880 || hw1op == 0xF890 || hw1op == 0xF8A0 ||
        hw1op == 0xF8B0) {   // load/store, imm12
        const uint32_t rn = hw & 0xF, rt = (instr >> 12) & 0xF, imm = instr & 0xFFF;
        const uint32_t addr = c.r[rn] + imm;
        if (hw1op == 0xF8D0) write(rt, mem32(addr, &ok));
        else if (hw1op == 0xF8C0) store32(addr, c.r[rt], &ok);
        else if (hw1op == 0xF890) write(rt, mem8(addr, &ok));
        else if (hw1op == 0xF880) store8(addr, static_cast<uint8_t>(c.r[rt]), &ok);
        else if (hw1op == 0xF8B0) write(rt, mem16(addr, &ok));
        else store16(addr, static_cast<uint16_t>(c.r[rt]), &ok);
        if (!ok) { stop.reason = "thumb transfer outside memory"; return false; }
        return true;
    }
    if (hw1op == 0xF850 || hw1op == 0xF840) {   // load/store, register offset
        const uint32_t rn = hw & 0xF, rt = (instr >> 12) & 0xF, rm = instr & 0xF;
        const uint32_t addr = c.r[rn] + c.r[rm];
        if (hw1op == 0xF850) write(rt, mem32(addr, &ok));
        else store32(addr, c.r[rt], &ok);
        if (!ok) { stop.reason = "thumb transfer outside memory"; return false; }
        return true;
    }
    if ((instr & 0xFFFF) == 0xE92D || (instr & 0xFFFF) == 0xE8BD) {   // push / pop
        const bool pop = (instr & 0xFFFF) == 0xE8BD;
        const uint32_t list = instr & 0xFFFF;
        uint32_t count = 0;
        for (uint32_t i = 0; i < 15; ++i) if (list & (1u << i)) ++count;
        uint32_t addr = pop ? c.r[13] : c.r[13] - 4 * count;
        c.r[13] = addr;
        for (uint32_t i = 0; i < 15; ++i) {
            if (!(list & (1u << i))) continue;
            if (pop) c.r[i] = mem32(addr, &ok);
            else store32(addr, c.r[i], &ok);
            addr += 4;
        }
        if (!ok) { stop.reason = "thumb push/pop outside memory"; return false; }
        if (pop && (list & 0x8000)) {
            const uint32_t v = mem32(addr, &ok);
            if (!ok) { stop.reason = "thumb pop pc outside memory"; return false; }
            c.r[13] = addr + 4;
            thumb_set_pc(c, v);
        }
        return true;
    }
    if ((hw & 0xFFF0) == 0xF10D || (hw & 0xFFF0) == 0xF1AD) {   // add/sub sp, sp, #imm
        const uint32_t imm = (((instr >> 26) & 1) << 11) | (((instr >> 12) & 7) << 8) | (instr & 0xFF);
        if ((hw & 0xFFF0) == 0xF1AD) c.r[13] -= imm; else c.r[13] += imm;
        return true;
    }
    if ((hw & 0xFFF0) == 0xF1B0 || (hw & 0xFFF0) == 0xF010) {   // cmp/tst immediate
        const uint32_t rn = hw & 0xF;
        const uint32_t imm = (((instr >> 26) & 1) << 11) | (((instr >> 12) & 7) << 8) | (instr & 0xFF);
        if ((hw & 0xFFF0) == 0xF1B0) subf(c.r[rn], imm, 0);
        else set_nz(c, c.r[rn] & imm);
        return true;
    }
    return unhandled("thumb32");
}

// Loads the bundled Swift runtime (or any other armv7 dylib) into the same low arena. Its exports are recorded so the
// app's Swift imports bind to real runtime code instead of trampolines.
bool load_side_image(const char* path, uint32_t hint, std::vector<uint8_t>& store, Image& img) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::rewind(f);
    if (n <= 0) { std::fclose(f); return false; }
    store.resize(static_cast<size_t>(n));
    const size_t got = std::fread(store.data(), 1, store.size(), f);
    std::fclose(f);
    if (got != store.size()) return false;

    uint32_t ro = 0, rs = 0, bo = 0, bs = 0, lo = 0, ls = 0;
    if (!parse(store.data(), store.size(), path, img, ro, rs, bo, bs, lo, ls)) return false;
    const uint32_t span = (img.span + kPage - 1) & ~(kPage - 1);
    void* map = nullptr;
    if (!map_fixed(hint, span, &map)) return false;
    img.base = static_cast<uint8_t*>(map);
    img.base_addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(map));
    for (const Segment& s : img.segments) {
        if (!s.filesize) continue;
        if (s.fileoff + s.filesize > store.size()) return false;
        std::memcpy(img.base + (s.vmaddr - img.min_vm), store.data() + s.fileoff, s.filesize);
    }
    g_regions.push_back({img.base_addr, img.base_addr + span, "side"});
    g_code_ranges.push_back({img.base_addr, img.base_addr + span});
    for (const auto& kv : img.defined) g_extern.emplace(kv.first, img.base_addr + (kv.second - img.min_vm));
    std::printf("[armv7] runtime image %s at 0x%08x, %zu exports\n", path, img.base_addr, img.defined.size());

    // The runtime has its own fixups (including its own stubs); resolve them the same way so a stub reachable from the
    // app's call path reports the symbol it wants instead of jumping into zeros.
    const uint32_t side_slide = img.base_addr - img.min_vm;
    size_t side_rebases = 0, side_binds = 0, side_missing = 0;
    if (!apply_rebases(img, store.data(), ro, rs, side_slide, &side_rebases)) return false;
    if (!apply_binds(img, store.data(), bo, bs, false, &side_binds, &side_missing)) return false;
    if (!apply_binds(img, store.data(), lo, ls, true, &side_binds, &side_missing)) return false;
    std::printf("[armv7] runtime image: %zu rebases, %zu binds, %zu unresolved\n", side_rebases, side_binds,
                side_missing);
    return true;
}

int run_image(uint8_t* data, size_t size, const char* path) {
    Image img;
    uint32_t rebase_off = 0, rebase_size = 0, bind_off = 0, bind_size = 0, lazy_off = 0, lazy_size = 0;
    if (!parse(data, size, path, img, rebase_off, rebase_size, bind_off, bind_size, lazy_off, lazy_size)) return 1;
    if (!img.entry) { std::fprintf(stderr, "[armv7] no entry point\n"); return 1; }

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
    g_code_ranges.push_back({img.base_addr, img.base_addr + map_span});

    // Optional side image: the app bundle ships the armv7 Swift runtime. When it is present, Swift imports bind to it.
    std::vector<uint8_t> side_store;
    if (const char* side = std::getenv("LOADER_ARMV7_SWIFTCORE"); side && *side) {
        Image side_img;
        if (!load_side_image(side, 0x20000000u, side_store, side_img))
            std::fprintf(stderr, "[armv7] could not load %s; Swift imports stay stubs\n", side);
        else {
            auto it = g_extern.find("_swift_once");
            if (it != g_extern.end()) g_swift_once = it->second;
        }
    }

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
    cpu.thumb = (img.entry & 1) != 0;
    cpu.r[15] = img.entry & ~1u;
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
    auto describe = [](uint32_t v) {
        const char* n = stub_name(v);
        return n ? std::string(n) : std::string();
    };

    // Runs a guest function to its return address. Used by the Swift runtime shim below.
    auto call_guest = [&](uint32_t fn, uint32_t ctx) -> bool {
        const uint32_t ret = 0xFFFFFFF0u;
        bool mapped = false;
        for (const auto& range : g_code_ranges)
            if (fn >= range.first && fn < range.second) mapped = true;
        if (!mapped) {
            stop.reason = "initializer address outside every loaded image";
            return false;
        }
        const uint32_t saved_lr = cpu.r[14], saved_pc = cpu.r[15];
        cpu.r[0] = ctx;
        cpu.r[14] = ret;
        cpu.thumb = (fn & 1) != 0;
        cpu.r[15] = fn & ~1u;
        while (cpu.r[15] != ret) {
            if (steps >= budget) { stop.reason = "instruction budget inside initializer"; return false; }
            if (const char* n = stub_name(cpu.r[15])) { stop.hit = true; stop.reason = n; return false; }
            const bool ran = cpu.thumb ? step_thumb(cpu, stop) : step(cpu, stop);
            if (!ran) return false;
            ++steps;
        }
        cpu.r[14] = saved_lr;
        cpu.r[15] = saved_pc;
        return true;
    };

    // _swift_once(token, function, context): the runtime's own implementation uses dispatch_once, so keep running it
    // here and call the initializer directly. Works whether the call landed on a stub or on the real runtime export.
    auto handle_swift_once = [&]() -> bool {
        const uint32_t token = cpu.r[0], fn = cpu.r[1], ctx = cpu.r[2];
        bool ok = true;
        const uint32_t before = mem32(token, &ok);
        if (ok && before == 0) {
            store32(token, 1, &ok);
            std::printf("[armv7] swift_once -> running initializer 0x%08x\n", fn);
            const uint64_t at = steps;
            if (!ok || !call_guest(fn, ctx)) return false;
            std::printf("[armv7] swift_once -> initializer returned after %llu instructions\n",
                        static_cast<unsigned long long>(steps - at));
        }
        cpu.r[0] = 0;
        const uint32_t ret = cpu.r[14];
        if (ret == 0 || ret == 0xFFFFFFFEu || ret == 0xFFFFFFF0u || stub_name(ret)) {
            stop.reason = "_swift_once has no usable return address";
            return false;
        }
        cpu.r[15] = ret;
        ++steps;
        return true;
    };

    while (true) {
        const uint32_t pc = cpu.r[15];
        if (static_cast<int>(steps) < trace) {
            bool tok = true;
            const uint32_t word = cpu.thumb ? mem16(pc, &tok) : mem32(pc, &tok);
            std::printf("[armv7] %c%08llx  %08x  r0=%08x r1=%08x r12=%08x sp=%08x lr=%08x\n", cpu.thumb ? 'T' : 'A',
                        static_cast<unsigned long long>(pc), word, cpu.r[0], cpu.r[1], cpu.r[12], cpu.r[13], cpu.r[14]);
        }

        if (g_swift_once && ((pc | 1u) == (g_swift_once | 1u))) {
            if (!handle_swift_once()) break;
            continue;
        }

        if (const char* name = stub_name(pc)) {
            if (!std::strcmp(name, "_swift_once")) {
                if (!handle_swift_once()) break;
                continue;
            }
            if (!std::strcmp(name, "_swift_retain") || !std::strcmp(name, "_swift_release")) {
                const uint32_t ret = cpu.r[14];
                if (ret == 0 || ret == 0xFFFFFFFEu || ret == 0xFFFFFFF0u || stub_name(ret)) {
                    stop.reason = std::string(name) + " has no usable return address";
                    break;
                }
                cpu.r[15] = ret;
                ++steps;
                continue;
            }
            stop.hit = true;
            stop.reason = name;
            break;
        }

        if (steps >= budget) { stop.reason = "instruction budget exhausted"; break; }
        const bool ran = cpu.thumb ? step_thumb(cpu, stop) : step(cpu, stop);
        if (!ran) break;
        ++steps;
    }

    if (stop.hit) {
        std::printf("[armv7] sign of life: ran %llu guest instructions, then called %s\n",
                    static_cast<unsigned long long>(steps), stop.reason.c_str());
        for (int i = 0; i < 4; ++i) {
            const std::string named = describe(cpu.r[i]);
            std::printf("[armv7]   r%d = 0x%08x%s\n", i, cpu.r[i], named.empty() ? "" : ("  (" + named + ")").c_str());
        }
    } else
        std::printf("[armv7] stopped after %llu instructions: %s (pc=0x%08x)\n",
                    static_cast<unsigned long long>(steps), stop.reason.c_str(), cpu.r[15]);
    return stop.hit ? 0 : 1;
}

}  // namespace armv7
