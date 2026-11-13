// LC_DYLD_CHAINED_FIXUPS: how every binary built for iOS 15 or later stores its rebases and binds. Instead of the
// two opcode streams of LC_DYLD_INFO, each location that needs relocating is a 64-bit word inside a segment, and
// the word itself says whether it is a pointer into the image (rebase) or an import index (bind), plus the
// distance to the next such word. Walking the chains in every page of every segment reconstructs exactly what
// the old rebase/bind streams described.
//
// Only the on-disk side is handled here; the caller supplies the image mapping, the symbol resolver and the
// callback that records each bind.
#pragma once
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace chained_fixups {

struct Segment {
    const char* name = "";
    uint64_t vmaddr = 0;
    uint64_t vmsize = 0;
};

struct Result {
    size_t rebases = 0;
    size_t binds = 0;
    size_t page_chains = 0;
    std::string error;
};

struct Args {
    const uint8_t* file = nullptr;
    size_t file_size = 0;
    uint32_t dataoff = 0;   // LC_DYLD_CHAINED_FIXUPS dataoff
    uint32_t datasize = 0;  // ...and datasize
    const std::vector<Segment>* segments = nullptr;  // LC_SEGMENT_64 order, __PAGEZERO included
    uint8_t* image_base = nullptr;                  // where the segment at min_vm was mapped
    uint64_t min_vm = 0;
    std::function<void*(const std::string&)> resolve;
    std::function<void(uint64_t loc, void* target, const std::string& symbol, int64_t addend)> record;
};

namespace detail {

inline uint16_t rd16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
inline uint32_t rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline uint64_t rd64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }

// dyld_chained_ptr_arm64e_bind.addend is 19 bits at bit 32.
inline int64_t sext19(uint64_t v) { return (v & 0x40000) ? static_cast<int64_t>(v | ~0x7ffffull) : static_cast<int64_t>(v); }

}  // namespace detail

// Walks every chain in the payload. Returns with error set (and the counts so far) if the metadata is malformed;
// the caller treats that as fatal, because a half-relocated image crashes wherever it is next used.
inline Result apply(const Args& a) {
    using namespace detail;
    Result r;
    if (!a.file || !a.segments || !a.image_base || !a.dataoff) {
        r.error = "chained fixups has no payload to walk";
        return r;
    }
    if (a.dataoff > a.file_size || a.datasize > a.file_size - a.dataoff || a.datasize < 28) {
        r.error = "the chained fixups payload lies outside the file";
        return r;
    }
    const uint8_t* data = a.file + a.dataoff;
    const uint8_t* data_end = data + a.datasize;

    const uint32_t version = rd32(data);
    const uint32_t starts_offset = rd32(data + 4);
    const uint32_t imports_offset = rd32(data + 8);
    const uint32_t symbols_offset = rd32(data + 12);
    const uint32_t imports_count = rd32(data + 16);
    const uint32_t imports_format = rd32(data + 20);
    const uint32_t symbols_format = rd32(data + 24);
    if (version != 0) { r.error = "unsupported chained fixups version " + std::to_string(version); return r; }
    if (symbols_format != 0) { r.error = "zlib-compressed chained import names are not supported"; return r; }
    if (imports_format < 1 || imports_format > 3) {
        r.error = "unknown chained import format " + std::to_string(imports_format);
        return r;
    }
    if (starts_offset < 28 || starts_offset > a.datasize || imports_offset > a.datasize || symbols_offset > a.datasize) {
        r.error = "a chained fixups table starts outside the payload";
        return r;
    }

    // dyld_chained_import { lib_ordinal:8, weak:1, name_offset:23 }, _addend { + addend:i32 },
    // _addend64 { lib_ordinal:16, weak:1, reserved:15, name_offset:32, addend:i64 }.
    const size_t import_size = imports_format == 3 ? 16 : imports_format == 2 ? 8 : 4;
    auto read_import = [&](uint32_t ordinal, std::string& symbol, int64_t& addend) -> bool {
        if (ordinal >= imports_count) return false;
        const uint8_t* imp = data + imports_offset + static_cast<size_t>(ordinal) * import_size;
        if (imp + import_size > data_end) return false;
        uint64_t name_off = 0;
        addend = 0;
        if (imports_format == 3) {
            name_off = rd32(imp + 4);
            addend = static_cast<int64_t>(rd64(imp + 8));
        } else {
            name_off = rd32(imp) >> 9;
            if (imports_format == 2) addend = static_cast<int32_t>(rd32(imp + 4));
        }
        const uint8_t* name = data + symbols_offset + name_off;
        if (name >= data_end || symbols_offset + name_off < symbols_offset) return false;
        const void* nul = std::memchr(name, 0, static_cast<size_t>(data_end - name));
        if (!nul) return false;
        symbol.assign(reinterpret_cast<const char*>(name), static_cast<const char*>(nul) - reinterpret_cast<const char*>(name));
        return true;
    };

    // dyld_chained_starts_in_image { seg_count, seg_info_offset[seg_count] }
    if (starts_offset + 4 > a.datasize) { r.error = "chained starts are truncated"; return r; }
    const uint8_t* starts = data + starts_offset;
    const uint32_t seg_count = rd32(starts);
    if (starts_offset + 4 + 4ull * seg_count > a.datasize) { r.error = "chained segment table is truncated"; return r; }

    for (uint32_t i = 0; i < seg_count; ++i) {
        const uint32_t info_off = rd32(starts + 4 + 4 * i);
        if (!info_off) continue;  // segment has no fixups
        if (i >= a.segments->size()) { r.error = "chained starts name segment " + std::to_string(i) + " which the image does not have"; return r; }
        const Segment& seg = (*a.segments)[i];
        // dyld_chained_starts_in_segment { size, page_size, pointer_format, segment_offset, max_valid_pointer,
        //                                   page_count, page_start[page_count] }
        const uint8_t* sis = starts + info_off;
        if (sis + 22 > data_end) { r.error = "chained starts for segment " + std::to_string(i) + " are truncated"; return r; }
        const uint16_t page_size = rd16(sis + 4);
        const uint16_t pointer_format = rd16(sis + 6);
        const uint64_t segment_offset = rd64(sis + 8);
        const uint16_t page_count = rd16(sis + 20);
        if (sis + 22 + 2ull * page_count > data_end) { r.error = "chained page table is truncated"; return r; }

        // stride is what the 11/12-bit `next` field is counted in: 4 bytes for the 64-bit formats, 8 for arm64e.
        uint64_t stride = 0;
        switch (pointer_format) {
            case 1:   // DYLD_CHAINED_PTR_ARM64E
            case 9:   // DYLD_CHAINED_PTR_ARM64E_USERLAND
            case 12:  // DYLD_CHAINED_PTR_ARM64E_USERLAND24
                stride = 8;
                break;
            case 2:   // DYLD_CHAINED_PTR_64
            case 6:   // DYLD_CHAINED_PTR_64_OFFSET
                stride = 4;
                break;
            default:
                r.error = "unsupported chained pointer format " + std::to_string(pointer_format) + " in segment " + std::to_string(i);
                return r;
        }
        const uint8_t* page_start = sis + 22;
        bool failed = false;

        auto walk = [&](uint64_t off) {
            for (;;) {
                if (off < segment_offset || off + 8 > segment_offset + seg.vmsize) {
                    r.error = std::string("chain in segment ") + seg.name + " leaves the segment";
                    failed = true;
                    return;
                }
                uint8_t* loc = a.image_base + off;
                const uint64_t v = rd64(loc);
                bool is_bind = false;
                uint64_t rebase_target = 0;
                uint64_t high8 = 0;
                uint32_t ordinal = 0;
                int64_t addend = 0;
                uint64_t next = 0;
                if (pointer_format == 2 || pointer_format == 6) {
                    is_bind = ((v >> 63) & 1) != 0;
                    next = (v >> 51) & 0x7ff;
                    if (is_bind) {
                        ordinal = static_cast<uint32_t>(v & 0xffffff);
                        addend = static_cast<int8_t>((v >> 24) & 0xff);
                    } else {
                        rebase_target = v & 0xfffffffffull;
                        high8 = (v >> 36) & 0xff;
                    }
                } else if (pointer_format == 12) {
                    is_bind = ((v >> 54) & 1) != 0;
                    next = (v >> 43) & 0x7ff;
                    if (is_bind) {
                        ordinal = static_cast<uint32_t>(v & 0xffffff);
                        addend = sext19((v >> 24) & 0x7ffff);
                    } else {
                        rebase_target = v & 0x7ffffffffffull;
                        high8 = (v >> 43) & 0xff;
                    }
                } else {
                    is_bind = ((v >> 62) & 1) != 0;
                    next = (v >> 51) & 0x7ff;
                    if (is_bind) {
                        ordinal = static_cast<uint32_t>(v & 0xffff);
                        addend = sext19((v >> 32) & 0x7ffff);
                    } else if ((v >> 63) & 1) {  // auth rebase: a 32-bit offset from the image base
                        rebase_target = v & 0xffffffffull;
                    } else {
                        rebase_target = v & 0x7ffffffffffull;
                        high8 = (v >> 43) & 0xff;
                    }
                }

                if (is_bind) {
                    std::string symbol;
                    int64_t import_addend = 0;
                    if (!read_import(ordinal, symbol, import_addend)) {
                        r.error = "bind ordinal " + std::to_string(ordinal) + " is outside the import table (" +
                                  std::to_string(imports_count) + " entries)";
                        failed = true;
                        return;
                    }
                    const int64_t total = addend + import_addend;
                    void* target = a.resolve ? a.resolve(symbol) : nullptr;
                    *reinterpret_cast<uint64_t*>(loc) = reinterpret_cast<uint64_t>(target) + static_cast<uint64_t>(total);
                    if (a.record) a.record(reinterpret_cast<uint64_t>(loc), target, symbol, total);
                    ++r.binds;
                } else {
                    uint64_t target = 0;
                    if (rebase_target) {
                        target = reinterpret_cast<uint64_t>(a.image_base) +
                                 (pointer_format == 2 ? rebase_target - a.min_vm : rebase_target);
                        if (high8 && pointer_format != 2) target |= high8 << 56;
                    }
                    *reinterpret_cast<uint64_t*>(loc) = target;
                    ++r.rebases;
                }
                if (!next) return;
                off += next * stride;
            }
        };

        for (uint16_t page = 0; page < page_count && !failed; ++page) {
            const uint16_t entry = rd16(page_start + 2 * page);
            if (entry == 0xffff) continue;  // DYLD_CHAINED_PTR_START_NONE
            const uint64_t page_off = segment_offset + static_cast<uint64_t>(page) * page_size;
            if (entry & 0x8000) {
                // DYLD_CHAINED_PTR_START_MULTI: several chains start in this page; the low bits index page_start
                // and each entry's LAST bit ends the list. Offsets there are page-relative.
                uint32_t idx = entry & 0x7fff;
                while (idx < page_count && !failed) {
                    const uint16_t sub = rd16(page_start + 2 * idx);
                    walk(page_off + (sub & 0x7fff));
                    ++r.page_chains;
                    if (sub & 0x8000) break;
                    ++idx;
                }
                continue;
            }
            walk(page_off + entry);
            ++r.page_chains;
        }
        if (failed || !r.error.empty()) return r;
    }
    return r;
}

}  // namespace chained_fixups
