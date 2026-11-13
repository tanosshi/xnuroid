#pragma once
// IL2CPP symbol tables.
//
// IL2CPP emits one `Il2CppCodeGenModule` per assembly describing that assembly's method pointer array, laid out in
// metadata token order. Nothing in the Mach-O symbol table names those functions, so the only way to get managed
// names is to pair the runtime pointer arrays with `global-metadata.dat` (parsed by tools/il2cpp_metadata.py).
//
//   LOADER_IL2CPP_MODULES=<path>  scan the loaded image and write every module's method pointers
//   LOADER_IL2CPP_SYMS=<path>     load a joined "<addr-hex> <name>" table so thread dumps name managed methods
//   LOADER_PC_PROFILE=<ms>        sample the main thread's PC and print the hottest methods at exit
//
// `Il2CppCodeGenModule` (Unity 2021.3, metadata v29) starts with:
//     const char* moduleName;  uint32_t methodPointerCount;  const Il2CppMethodPointer* methodPointers;
// which is all this needs; the remaining fields differ between versions and are never read.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

struct Il2CppSymTable {
    std::vector<uint64_t> addrs;      // sorted method entry addresses
    std::vector<uint32_t> name_off;   // offset into arena, parallel to addrs
    std::vector<char> arena;          // stable storage for names
    uint64_t base = 0, span = 0;      // guest image range the table was built for
    bool ready = false;
};

static Il2CppSymTable g_il2cpp_syms;
static std::vector<uint32_t> g_il2cpp_hits;   // per-symbol sample counts (LOADER_PC_PROFILE)
static uint64_t g_il2cpp_hits_outside = 0;

static inline bool il2cpp_ptr_in(uint64_t a, uint64_t base, uint64_t size) {
    return a >= base && a < base + size;
}

// Writes "IMAGE ...", one "MODULE <name> <count> <ptrs>" line per assembly, and one "M <name> <index> <addr>" line per
// method. Returns the number of modules found.
static int il2cpp_dump_modules(uint8_t* image, size_t image_size, const char* path) {
    FILE* out = fopen(path, "w");
    if (!out) return 0;
    const uint64_t base = reinterpret_cast<uint64_t>(image);
    const uint64_t end = base + image_size;
    fprintf(out, "IMAGE %llx %llx\n", static_cast<unsigned long long>(base),
            static_cast<unsigned long long>(image_size));
    std::vector<std::pair<std::string, uint64_t>> seen;
    int modules = 0;
    for (uint64_t p = base; p + 8 <= end; p += 8) {
        const uint64_t cand = *reinterpret_cast<const uint64_t*>(p);
        if (!il2cpp_ptr_in(cand, base, image_size) || (cand & 7)) continue;
        const char* name = *reinterpret_cast<const char* const*>(cand);
        if (!il2cpp_ptr_in(reinterpret_cast<uint64_t>(name), base, image_size)) continue;
        size_t len = 0;
        bool printable = true;
        for (; len < 64; ++len) {
            const char c = name[len];
            if (!c) break;
            if (c < 0x20 || c > 0x7e) { printable = false; break; }
        }
        if (!printable || len < 5 || len >= 64) continue;
        if (std::strcmp(name + len - 4, ".dll") != 0) continue;
        const uint32_t count = *reinterpret_cast<const uint32_t*>(cand + 8);
        if (count == 0 || count > 300000) continue;
        const uint64_t ptrs = *reinterpret_cast<const uint64_t*>(cand + 16);
        if (!il2cpp_ptr_in(ptrs, base, image_size) || (ptrs & 7)) continue;
        // Entries are usually image code, but an assembly with Burst jobs also points at Burst-compiled code in a
        // separately allocated region, so require only that most of the leading entries look like code in the image.
        const uint64_t* table = reinterpret_cast<const uint64_t*>(ptrs);
        int code_like = 0, probed = 0;
        for (uint32_t i = 0; i < count && i < 16; ++i) {
            ++probed;
            if (il2cpp_ptr_in(table[i], base, image_size)) ++code_like;
        }
        if (code_like * 2 < probed) continue;
        bool duplicate = false;
        for (const auto& s : seen)
            if (s.first == name && s.second == ptrs) { duplicate = true; break; }
        if (duplicate) continue;
        seen.emplace_back(name, ptrs);
        fprintf(out, "MODULE %s %u %llx\n", name, count, static_cast<unsigned long long>(ptrs));
        for (uint32_t i = 0; i < count; ++i)
            fprintf(out, "M %s %u %llx\n", name, i, static_cast<unsigned long long>(table[i]));
        ++modules;
    }
    fclose(out);
    return modules;
}

// Loads "<image-offset-hex> <name>" lines (tools/il2cpp_symbols.py writes offsets so one file serves every run of a
// build; the pointer tables themselves are ASLR-relative). Adds `base` to each entry. Called before the run loop, so
// the tables are immutable afterwards and can be read from the signal handler.
static bool il2cpp_load_syms(const char* path, uint64_t base, size_t span) {
    FILE* in = fopen(path, "rb");
    if (!in) return false;
    std::vector<std::pair<uint64_t, std::string>> rows;
    char line[512];
    while (std::fgets(line, sizeof line, in)) {
        char* p = line;
        if (*p == '#') continue;
        while (*p == ' ' || *p == '\t') ++p;
        char* after = nullptr;
        const uint64_t addr = std::strtoull(p, &after, 16);
        if (!after || after == p) continue;
        while (*after == ' ' || *after == '\t') ++after;
        size_t n = std::strlen(after);
        while (n && (after[n - 1] == '\n' || after[n - 1] == '\r')) after[--n] = 0;
        if (!n) continue;
        rows.emplace_back(base + addr, std::string(after, n));
    }
    fclose(in);
    if (rows.empty()) return false;
    std::sort(rows.begin(), rows.end(),
              [](const std::pair<uint64_t, std::string>& a, const std::pair<uint64_t, std::string>& b) {
                  return a.first < b.first;
              });
    size_t arena = 0;
    for (const auto& r : rows) arena += r.second.size() + 1;
    g_il2cpp_syms.addrs.clear();
    g_il2cpp_syms.name_off.clear();
    g_il2cpp_syms.arena.assign(arena, 0);
    g_il2cpp_syms.addrs.reserve(rows.size());
    g_il2cpp_syms.name_off.reserve(rows.size());
    uint32_t off = 0;
    for (const auto& r : rows) {
        std::memcpy(g_il2cpp_syms.arena.data() + off, r.second.c_str(), r.second.size() + 1);
        g_il2cpp_syms.addrs.push_back(r.first);
        g_il2cpp_syms.name_off.push_back(off);
        off += static_cast<uint32_t>(r.second.size() + 1);
    }
    g_il2cpp_syms.base = base;
    g_il2cpp_syms.span = span;
    g_il2cpp_syms.ready = true;
    g_il2cpp_hits.assign(g_il2cpp_syms.addrs.size(), 0);
    return true;
}

// Name of the method containing `pc`, with *offset set to pc - method entry. Bounds the search by the next method
// entry, so a PC in unmanaged code between two IL2CPP functions does not borrow the previous function's name.
static const char* il2cpp_sym_lookup(uint64_t pc, uint64_t* offset) {
    if (!g_il2cpp_syms.ready) return nullptr;
    if (!il2cpp_ptr_in(pc, g_il2cpp_syms.base, g_il2cpp_syms.span)) return nullptr;
    const auto& a = g_il2cpp_syms.addrs;
    auto it = std::upper_bound(a.begin(), a.end(), pc);
    if (it == a.begin()) return nullptr;
    const size_t i = static_cast<size_t>(it - a.begin()) - 1;
    if (it != a.end() && *it == pc) return nullptr;   // pc is a different method's entry, not inside this one
    if (offset) *offset = pc - a[i];
    return g_il2cpp_syms.arena.data() + g_il2cpp_syms.name_off[i];
}

// Records one sample; called from the SIGUSR2 handler, so no allocation and no locking.
static void il2cpp_profile_hit(uint64_t pc) {
    if (!g_il2cpp_syms.ready) { ++g_il2cpp_hits_outside; return; }
    uint64_t off = 0;
    const char* name = il2cpp_sym_lookup(pc, &off);
    if (!name) { ++g_il2cpp_hits_outside; return; }
    const size_t i = static_cast<size_t>(std::lower_bound(g_il2cpp_syms.addrs.begin(), g_il2cpp_syms.addrs.end(), pc) -
                                        g_il2cpp_syms.addrs.begin()) - 1;
    if (i < g_il2cpp_hits.size()) ++g_il2cpp_hits[i];
}
