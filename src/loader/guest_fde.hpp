// Synthesizes DWARF FDEs from the guest's Mach-O compact unwind (__unwind_info) and registers
// them with the system unwinder (LLVM libunwind in bionic), which has no compact-unwind support.
//
// Reference decoder: compact_unwind.py (validated against llvm-objdump on the real binary).
//
// CFI rows are valid at call sites only, so every function gets ONE row for its whole range:
//   FRAME:     CFA = x29 + 16; x29 @ CFA-16; x30 @ CFA-8; pairs descend from CFA-24
//              (x19,x20 ... x27,x28, d8,d9 ... d14,d15)
//   FRAMELESS: CFA = sp + size; x30 stays in its register
//   DWARF:     the guest's own __eh_frame FDE is registered in place
// One CIE per personality index (augmentation "zPLR", personality indirect|absptr -> guest GOT
// slot) plus one "zR" CIE for functions without a personality. Guest is arm64, not arm64e
// (scan_pac.py), so no DW_CFA_AARCH64_negate_ra_state is needed.
#pragma once

#include <dlfcn.h>
#include <sys/mman.h>
#include <time.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace guest_fde {

struct Function {
    uint32_t start, end, enc;
};

struct UnwindInfo {
    std::vector<uint32_t> personalities; // image offsets of GOT slots (1-based index in encodings)
    std::vector<Function> funcs;
    std::vector<std::pair<uint32_t, uint32_t>> lsda; // (function offset, lsda offset), sorted
};

inline bool parse_unwind_info(const uint8_t* ui, size_t size, UnwindInfo& out) {
    auto u32 = [&](size_t o) { uint32_t v; std::memcpy(&v, ui + o, 4); return v; };
    auto u16 = [&](size_t o) { uint16_t v; std::memcpy(&v, ui + o, 2); return v; };
    if (size < 28 || u32(0) != 1) return false;
    uint32_t ce_off = u32(4), ce_cnt = u32(8), p_off = u32(12), p_cnt = u32(16), i_off = u32(20), i_cnt = u32(24);
    if (i_cnt < 2) return false;
    for (uint32_t i = 0; i < p_cnt; ++i) out.personalities.push_back(u32(p_off + 4 * i));
    std::vector<uint32_t> entries_f, entries_e;
    for (uint32_t k = 0; k + 1 < i_cnt; ++k) {
        uint32_t fo = u32(i_off + 12 * k), sl = u32(i_off + 12 * k + 4);
        uint32_t kind = u32(sl);
        if (kind == 2) {
            uint32_t eoff = u16(sl + 4), ecnt = u16(sl + 6);
            for (uint32_t j = 0; j < ecnt; ++j) {
                entries_f.push_back(u32(sl + eoff + 8 * j));
                entries_e.push_back(u32(sl + eoff + 8 * j + 4));
            }
        } else if (kind == 3) {
            uint32_t eoff = u16(sl + 4), ecnt = u16(sl + 6), coff = u16(sl + 8), ccnt = u16(sl + 10);
            for (uint32_t j = 0; j < ecnt; ++j) {
                uint32_t v = u32(sl + eoff + 4 * j);
                uint32_t n = v >> 24;
                entries_f.push_back(fo + (v & 0xFFFFFF));
                entries_e.push_back(n < ce_cnt ? u32(ce_off + 4 * n) : u32(sl + coff + 4 * (n - ce_cnt)));
                if (n >= ce_cnt && n - ce_cnt >= ccnt) return false;
            }
        } else {
            return false;
        }
    }
    uint32_t last_end = u32(i_off + 12 * (i_cnt - 1));
    for (size_t i = 0; i < entries_f.size(); ++i)
        out.funcs.push_back({entries_f[i], i + 1 < entries_f.size() ? entries_f[i + 1] : last_end, entries_e[i]});
    uint32_t l0 = u32(i_off + 8), l1 = u32(i_off + 12 * (i_cnt - 1) + 8);
    for (uint32_t o = l0; o + 8 <= l1; o += 8) out.lsda.push_back({u32(o), u32(o + 4)});
    return true;
}

static constexpr uint32_t kFrameless = 0x02000000, kDwarf = 0x03000000, kFrame = 0x04000000;

inline void put_uleb(std::vector<uint8_t>& b, uint64_t v) {
    do { uint8_t x = v & 0x7f; v >>= 7; b.push_back(v ? (x | 0x80) : x); } while (v);
}
inline void put_sleb(std::vector<uint8_t>& b, int64_t v) {
    for (;;) {
        uint8_t x = v & 0x7f;
        v >>= 7;
        bool done = (v == 0 && !(x & 0x40)) || (v == -1 && (x & 0x40));
        b.push_back(done ? x : (x | 0x80));
        if (done) return;
    }
}
inline void put32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(v >> (8 * i)); }
inline void put64(std::vector<uint8_t>& b, uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back(v >> (8 * i)); }
inline void pad8(std::vector<uint8_t>& b, size_t rec_start) {
    while ((b.size() - rec_start) % 8) b.push_back(0); // DW_CFA_nop
}
inline void patch_len(std::vector<uint8_t>& b, size_t rec_start) {
    uint32_t len = static_cast<uint32_t>(b.size() - rec_start - 4);
    std::memcpy(&b[rec_start], &len, 4);
}

// DWARF register numbers: x0-x30 = 0-30, sp = 31, v8-v15 = 72-79.
struct Save { int reg; int64_t cfa_off; const char* name; };
inline std::vector<Save> frame_saves(uint32_t enc) {
    static const struct { uint32_t bit; int r1; const char *n1; int r2; const char *n2; } kPairs[] = {
        {0x1, 19, "x19", 20, "x20"}, {0x2, 21, "x21", 22, "x22"}, {0x4, 23, "x23", 24, "x24"},
        {0x8, 25, "x25", 26, "x26"}, {0x10, 27, "x27", 28, "x28"},
        {0x100, 72, "d8", 73, "d9"}, {0x200, 74, "d10", 75, "d11"},
        {0x400, 76, "d12", 77, "d13"}, {0x800, 78, "d14", 79, "d15"}};
    std::vector<Save> s;
    int64_t off = -24;
    for (const auto& p : kPairs) {
        if (!(enc & p.bit)) continue;
        s.push_back({p.r1, off, p.n1});
        s.push_back({p.r2, off - 8, p.n2});
        off -= 16;
    }
    return s;
}

// Instruction stream for one function; empty for modes that are not synthesized.
inline bool build_cfi(uint32_t enc, std::vector<uint8_t>& ins) {
    uint32_t mode = enc & 0x0F000000;
    if (mode == kFrameless) {
        ins.push_back(0x0c); put_uleb(ins, 31); put_uleb(ins, ((enc & 0x00FFF000) >> 12) * 16); // def_cfa sp, size
        return true;
    }
    if (mode == kFrame) {
        ins.push_back(0x0c); put_uleb(ins, 29); put_uleb(ins, 16);          // def_cfa x29, 16
        ins.push_back(0x05); put_uleb(ins, 29); put_uleb(ins, 2);           // offset_extended x29, CFA-16 (factor -8)
        ins.push_back(0x05); put_uleb(ins, 30); put_uleb(ins, 1);           // offset_extended x30, CFA-8
        for (const Save& s : frame_saves(enc)) {
            ins.push_back(0x05); put_uleb(ins, s.reg); put_uleb(ins, static_cast<uint64_t>(-s.cfa_off) / 8);
        }
        return true;
    }
    return false;
}

inline double now_ms() {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

struct Options {
    bool only = false;                 // register only functions containing one of `only_offsets`
    std::vector<uint64_t> only_offsets; // image-relative; both a and a-1 are matched (return addresses)
    bool dump = false;
    bool dry_run = false;              // build only (host tests); do not call the unwinder
};

struct Result {
    size_t total = 0, registered = 0, synthesized = 0, dwarf_inplace = 0, skipped_none = 0, skipped_other = 0;
    double parse_ms = 0, build_ms = 0, register_ms = 0;
    const char* api = "none";
    const uint8_t* buf_ptr = nullptr; // synthesized CIE/FDE section (host tests read it back)
    size_t buf_size = 0;
};

// text_base: loaded address of image offset 0 (the __TEXT vmaddr). eh_frame: loaded __eh_frame.
inline bool register_parsed(const UnwindInfo& ui, const uint8_t* text_base, const uint8_t* eh_frame, size_t eh_size,
                            const Options& opt, Result& res) {
    double t0 = now_ms();
    res.total = ui.funcs.size();
    fprintf(stderr, "[FDE] __unwind_info: %zu functions, %zu lsda, %zu personalities\n", ui.funcs.size(),
            ui.lsda.size(), ui.personalities.size());

    // Log what the personality GOT slots hold (they must already be bound).
    for (size_t i = 0; i < ui.personalities.size(); ++i) {
        uint64_t v = *reinterpret_cast<const uint64_t*>(text_base + ui.personalities[i]);
        Dl_info di{};
        bool ok = dladdr(reinterpret_cast<void*>(v), &di);
        fprintf(stderr, "[FDE] personality[%zu]: GOT slot image+%#x = %#llx -> %s (%s)\n", i + 1, ui.personalities[i],
                static_cast<unsigned long long>(v), ok && di.dli_sname ? di.dli_sname : "?",
                ok && di.dli_fname ? di.dli_fname : "?");
    }

    t0 = now_ms();
    std::vector<uint8_t> buf;
    buf.reserve(res.total * 64);
    // CIEs: index 0 = no personality (zR), 1..N = zPLR with that personality.
    std::vector<size_t> cie_off(ui.personalities.size() + 1);
    for (size_t p = 0; p <= ui.personalities.size(); ++p) {
        cie_off[p] = buf.size();
        put32(buf, 0); put32(buf, 0); // length (patched), CIE id
        buf.push_back(1);             // version
        if (p == 0) { buf.push_back('z'); buf.push_back('R'); buf.push_back(0); }
        else { for (char c : {'z', 'P', 'L', 'R'}) buf.push_back(c); buf.push_back(0); }
        put_uleb(buf, 4);             // code alignment
        put_sleb(buf, -8);            // data alignment
        buf.push_back(30);            // return address register = x30
        if (p == 0) {
            put_uleb(buf, 1); buf.push_back(0x00);              // R: absptr
        } else {
            put_uleb(buf, 1 + 8 + 1 + 1);
            buf.push_back(0x80);                                 // P: indirect | absptr
            put64(buf, reinterpret_cast<uint64_t>(text_base + ui.personalities[p - 1]));
            buf.push_back(0x00);                                 // L: absptr
            buf.push_back(0x00);                                 // R: absptr
        }
        pad8(buf, cie_off[p]);
        patch_len(buf, cie_off[p]);
    }

    auto wanted = [&](const Function& f) {
        if (!opt.only) return true;
        for (uint64_t a : opt.only_offsets)
            if ((a >= f.start && a < f.end) || (a - 1 >= f.start && a - 1 < f.end)) return true;
        return false;
    };

    std::vector<size_t> fde_off;         // synthesized FDEs, relative to buf
    std::vector<const uint8_t*> inplace; // guest-owned DWARF FDEs
    for (const Function& f : ui.funcs) {
        if (f.enc == 0) { res.skipped_none++; continue; }
        if (!wanted(f)) continue;
        uint32_t mode = f.enc & 0x0F000000;
        if (mode == kDwarf) {
            uint32_t o = f.enc & 0x00FFFFFF;
            if (!eh_frame || o + 8 > eh_size) { res.skipped_other++; continue; }
            inplace.push_back(eh_frame + o);
            res.dwarf_inplace++;
            if (opt.dump) fprintf(stderr, "[FDE] %#010x-%#010x enc=%#010x DWARF in place (eh_frame+%#x)\n", f.start, f.end, f.enc, o);
            continue;
        }
        std::vector<uint8_t> ins;
        if (!build_cfi(f.enc, ins)) { res.skipped_other++; continue; }
        uint32_t pidx = (f.enc >> 28) & 3;
        if (pidx > ui.personalities.size()) { res.skipped_other++; continue; }
        uint64_t lsda_addr = 0;
        if (f.enc & 0x40000000) {
            auto it = std::lower_bound(ui.lsda.begin(), ui.lsda.end(), std::make_pair(f.start, uint32_t(0)));
            if (it != ui.lsda.end() && it->first == f.start) lsda_addr = reinterpret_cast<uint64_t>(text_base + it->second);
        }
        size_t at = buf.size();
        fde_off.push_back(at);
        put32(buf, 0);
        put32(buf, static_cast<uint32_t>(at + 4 - cie_off[pidx])); // CIE pointer: distance back to the CIE
        put64(buf, reinterpret_cast<uint64_t>(text_base + f.start));
        put64(buf, f.end - f.start);
        if (pidx == 0) put_uleb(buf, 0);
        else { put_uleb(buf, 8); put64(buf, lsda_addr); }
        buf.insert(buf.end(), ins.begin(), ins.end());
        pad8(buf, at);
        patch_len(buf, at);
        res.synthesized++;
        if (opt.dump)
            fprintf(stderr, "[FDE] %#010x-%#010x enc=%#010x %s pers=%u lsda=%#llx\n", f.start, f.end, f.enc,
                    mode == kFrame ? "FRAME" : "FRAMELESS", pidx, static_cast<unsigned long long>(lsda_addr));
    }
    buf.insert(buf.end(), 4, 0); // terminator for section-style registration
    res.build_ms = now_ms() - t0;

    // The unwinder keeps pointers into this memory forever: mmap it, never free.
    size_t map_size = (buf.size() + 4095) & ~size_t(4095);
    void* mem = mmap(nullptr, map_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) { perror("[FDE] mmap"); return false; }
    std::memcpy(mem, buf.data(), buf.size());
    const uint8_t* base = static_cast<const uint8_t*>(mem);
    res.buf_ptr = base;
    res.buf_size = buf.size();

    if (opt.dry_run) { res.registered = fde_off.size() + inplace.size(); return true; }

    typedef void (*reg_fn)(const void*);
    auto add_section = reinterpret_cast<reg_fn>(dlsym(RTLD_DEFAULT, "__unw_add_dynamic_eh_frame_section"));
    auto add_fde = reinterpret_cast<reg_fn>(dlsym(RTLD_DEFAULT, "__unw_add_dynamic_fde"));
    auto reg_frame = reinterpret_cast<reg_fn>(dlsym(RTLD_DEFAULT, "__register_frame"));
    fprintf(stderr, "[FDE] unwinder API: __unw_add_dynamic_eh_frame_section=%p __unw_add_dynamic_fde=%p __register_frame=%p\n",
            reinterpret_cast<void*>(add_section), reinterpret_cast<void*>(add_fde), reinterpret_cast<void*>(reg_frame));
    reg_fn single = reg_frame ? reg_frame : add_fde;

    t0 = now_ms();
    if (add_section && !fde_off.empty()) {
        add_section(base); // CIEs then FDEs, zero-terminated
        res.api = "__unw_add_dynamic_eh_frame_section";
    } else if (single) {
        for (size_t o : fde_off) single(base + o);
        res.api = reg_frame ? "__register_frame(per FDE)" : "__unw_add_dynamic_fde(per FDE)";
    } else if (!fde_off.empty()) {
        fprintf(stderr, "[FDE] no registration API found in the process\n");
        return false;
    }
    if (!inplace.empty()) {
        if (!single) { fprintf(stderr, "[FDE] cannot register in-place DWARF FDEs: no per-FDE API\n"); return false; }
        for (const uint8_t* p : inplace) single(p);
    }
    res.registered = fde_off.size() + inplace.size();
    res.register_ms = now_ms() - t0;
    fprintf(stderr,
            "[FDE] registered %zu of %zu (synth %zu, dwarf in place %zu, no-unwind %zu, skipped %zu) via %s; "
            "parse %.1f ms, build %.1f ms (%zu bytes at %p), register %.1f ms\n",
            res.registered, res.total, res.synthesized, res.dwarf_inplace, res.skipped_none, res.skipped_other, res.api,
            res.parse_ms, res.build_ms, buf.size(), mem, res.register_ms);
    return true;
}

inline bool register_all(const uint8_t* text_base, const uint8_t* unwind_info, size_t ui_size,
                         const uint8_t* eh_frame, size_t eh_size, const Options& opt, Result& res) {
    double t0 = now_ms();
    UnwindInfo ui;
    if (!parse_unwind_info(unwind_info, ui_size, ui)) {
        fprintf(stderr, "[FDE] failed to parse __unwind_info\n");
        return false;
    }
    res.parse_ms = now_ms() - t0;
    return register_parsed(ui, text_base, eh_frame, eh_size, opt, res);
}

} // namespace guest_fde
