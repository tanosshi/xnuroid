#pragma once
// Unity's Data/globalgamemanagers lists the build's scenes as "Assets/.../<name>.unity" in build order. Reading it
// gives the host the same ordering the player uses, which is what lets a host with no video backend advance past a
// splash-video scene to the scene the player would have loaded next.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static std::vector<std::string> unity_scene_list(const char* game_dir) {
    std::vector<std::string> scenes;
    if (!game_dir || !*game_dir) return scenes;
    const std::string path = std::string(game_dir) + "/Data/globalgamemanagers";
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return scenes;
    std::vector<char> buf;
    char chunk[1 << 16];
    size_t n = 0;
    while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0) buf.insert(buf.end(), chunk, chunk + n);
    std::fclose(f);
    const size_t end = buf.size();
    for (size_t i = 0; i + 7 < end; ++i) {
        if (std::memcmp(buf.data() + i, "Assets/", 7) != 0) continue;
        size_t j = i;
        while (j < end) {
            const unsigned char c = static_cast<unsigned char>(buf[j]);
            if (c < 0x20 || c > 0x7e) break;
            ++j;
        }
        std::string s(buf.data() + i, j - i);
        const size_t dot = s.find(".unity");
        if (dot == std::string::npos) {
            i = j;
            continue;
        }
        s.resize(dot + 6);   // neighbouring length bytes can be printable; keep exactly the path
        const size_t slash = s.find_last_of('/');
        std::string name = slash == std::string::npos ? s : s.substr(slash + 1, s.size() - slash - 1 - 6);
        if (!name.empty()) {
            bool duplicate = false;
            for (const auto& existing : scenes)
                if (existing == name) {
                    duplicate = true;
                    break;
                }
            if (!duplicate) scenes.push_back(name);
        }
        i = j;
    }
    return scenes;
}
