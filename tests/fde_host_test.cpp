// Host test for src/loader/guest_fde.hpp: reads a thin arm64 Mach-O, builds the synthesized CIE/FDE
// section (dry run, no unwinder calls) and writes it to <out.bin> for llvm-dwarfdump --eh-frame.
// Build: g++ -std=c++17 -I src/loader tests/fde_host_test.cpp -o fde_host_test
// Usage: fde_host_test <macho> <out.bin> [fake_personality]
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include "guest_fde.hpp"

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), {});
    uint32_t ncmds; std::memcpy(&ncmds, &d[16], 4);
    size_t pos = 32; uint64_t ui_off = 0, ui_sz = 0;
    for (uint32_t i = 0; i < ncmds; ++i) {
        uint32_t cmd, size; std::memcpy(&cmd, &d[pos], 4); std::memcpy(&size, &d[pos + 4], 4);
        if (cmd == 0x19) {
            uint32_t n; std::memcpy(&n, &d[pos + 64], 4);
            for (uint32_t s = 0; s < n; ++s) {
                size_t o = pos + 72 + 80 * s;
                if (!std::strncmp((char*)&d[o], "__unwind_info", 16)) {
                    uint64_t sz; uint32_t off; std::memcpy(&sz, &d[o + 40], 8); std::memcpy(&off, &d[o + 48], 4);
                    ui_off = off; ui_sz = sz;
                }
            }
        }
        pos += size;
    }
    guest_fde::UnwindInfo ui;
    if (!guest_fde::parse_unwind_info(&d[ui_off], ui_sz, ui)) { fprintf(stderr, "parse failed\n"); return 1; }
    std::vector<uint8_t> fake_image(1 << 20, 0);
    if (argc > 3) { // pretend funcs with pers bits use a personality GOT slot at image+0x800, LSDA at +0x900
        ui.personalities = {0x800};
        for (auto& fn : ui.funcs) if (fn.enc) fn.enc |= 0x10000000 | 0x40000000;
        ui.lsda.clear();
        for (auto& fn : ui.funcs) if (fn.enc) ui.lsda.push_back({fn.start, 0x900});
    }
    guest_fde::Options opt; opt.dry_run = true;
    guest_fde::Result r;
    if (!guest_fde::register_parsed(ui, fake_image.data(), nullptr, 0, opt, r)) return 1;
    std::ofstream o(argv[2], std::ios::binary);
    o.write((const char*)r.buf_ptr, r.buf_size);
    return 0;
}
