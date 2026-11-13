#pragma once
// Reads the guest Mach-O symbol table so the loader can call functions the image exports.
//
// The loader resolves the guest's *imports* through shims; it never needed the guest's own exports until managed
// introspection, which calls Unity's exported `il2cpp_*` API. Those functions exist as 4-byte `b <impl>` thunks in
// __TEXT and are listed in LC_SYMTAB (1493 symbols in the build this was measured on), so parsing the symbol table gives
// name -> vmaddr, and guest address = image_base + vmaddr.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

struct GuestSymtab {
    std::unordered_map<std::string, uint64_t> vmaddr;   // name without the leading underscore
    bool loaded = false;
};

static GuestSymtab g_guest_symtab;

static bool guest_symtab_load(const char* path) {
    if (g_guest_symtab.loaded) return true;
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 32) {
        std::fclose(f);
        return false;
    }
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    const size_t got = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (got != buf.size()) return false;
    if (*reinterpret_cast<const uint32_t*>(buf.data()) != 0xfeedfacfu) return false;   // MH_MAGIC_64
    const uint32_t ncmds = *reinterpret_cast<const uint32_t*>(buf.data() + 16);
    uint32_t symoff = 0, nsyms = 0, stroff = 0, strsize = 0;
    size_t off = 32;
    for (uint32_t i = 0; i < ncmds && off + 8 <= buf.size(); ++i) {
        const uint32_t cmd = *reinterpret_cast<const uint32_t*>(buf.data() + off);
        const uint32_t cmdsize = *reinterpret_cast<const uint32_t*>(buf.data() + off + 4);
        if (cmdsize < 8) break;
        if (cmd == 0x2 && cmdsize >= 24) {   // LC_SYMTAB
            symoff = *reinterpret_cast<const uint32_t*>(buf.data() + off + 8);
            nsyms = *reinterpret_cast<const uint32_t*>(buf.data() + off + 12);
            stroff = *reinterpret_cast<const uint32_t*>(buf.data() + off + 16);
            strsize = *reinterpret_cast<const uint32_t*>(buf.data() + off + 20);
        }
        off += cmdsize;
    }
    if (!symoff || !nsyms || !stroff || stroff + strsize > buf.size()) return false;
    for (uint32_t i = 0; i < nsyms; ++i) {
        const size_t p = symoff + static_cast<size_t>(i) * 16;
        if (p + 16 > buf.size()) break;
        const uint32_t strx = *reinterpret_cast<const uint32_t*>(buf.data() + p);
        const uint8_t type = *reinterpret_cast<const uint8_t*>(buf.data() + p + 4);
        const uint64_t value = *reinterpret_cast<const uint64_t*>(buf.data() + p + 8);
        if ((type & 0x0e) != 0x0e || !value) continue;   // N_SECT with an address
        if (strx >= strsize) continue;
        const char* name = reinterpret_cast<const char*>(buf.data() + stroff + strx);
        size_t len = 0;
        while (len < strsize - strx && name[len]) ++len;
        if (!len) continue;
        std::string s(name, len);
        if (s[0] == '_') s.erase(0, 1);
        g_guest_symtab.vmaddr[s] = value;
    }
    g_guest_symtab.loaded = true;
    return true;
}

// Guest address of an exported symbol, or 0.
static uint64_t guest_sym_addr(uint64_t image_base, const char* name) {
    if (!g_guest_symtab.loaded) return 0;
    auto it = g_guest_symtab.vmaddr.find(name);
    return it == g_guest_symtab.vmaddr.end() ? 0 : image_base + it->second;
}
