// Host (Windows) runner for the armv7 loader: the same code the Android binary uses, so instruction, relocation and
// runtime-shim work can be exercised without a device attached.
#include "../src/loader/armv7.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <thin-armv7-macho>\n", argv[0]);
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.empty()) {
        std::fprintf(stderr, "%s is empty\n", argv[1]);
        return 2;
    }
    return armv7::run_image(data.data(), data.size(), argv[1]);
}
