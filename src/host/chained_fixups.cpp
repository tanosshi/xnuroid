#include "chained_fixups.hpp"

#include <limits>
#include <utility>

namespace macho {
namespace {
constexpr uint16_t kStartNone = 0xffff;
constexpr uint16_t kStartMulti = 0x8000;
constexpr uint16_t kStartLast = 0x8000;

bool range_ok(uint64_t total, uint64_t off, uint64_t size) {
  return off <= total && size <= total - off;
}

uint16_t u16(const uint8_t* p) {
  return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) |
                               static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8));
}
uint32_t u32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t u64(const uint8_t* p) {
  return static_cast<uint64_t>(u32(p)) | (static_cast<uint64_t>(u32(p + 4)) << 32);
}
int64_t sign_extend(uint64_t value, unsigned bits) {
  const uint64_t sign = uint64_t{1} << (bits - 1);
  return static_cast<int64_t>((value ^ sign) - sign);
}
bool add_signed(uint64_t value, int64_t delta, uint64_t& out) {
  if (delta >= 0) {
    const auto d = static_cast<uint64_t>(delta);
    if (value > std::numeric_limits<uint64_t>::max() - d) return false;
    out = value + d;
  } else {
    const auto d = static_cast<uint64_t>(-(delta + 1)) + 1;
    if (value < d) return false;
    out = value - d;
  }
  return true;
}

bool read_symbol(const uint8_t* symbols, uint64_t symbols_size, uint32_t offset,
                 std::string& out) {
  if (offset >= symbols_size) return false;
  uint64_t end = offset;
  while (end < symbols_size && symbols[end] != 0) ++end;
  if (end == symbols_size) return false;
  out.assign(reinterpret_cast<const char*>(symbols + offset), static_cast<size_t>(end - offset));
  return true;
}

struct Pending {
  FixupRecord record;
  uint64_t original{};
};

bool decode_imports(const FixupOptions& o, uint32_t imports_off, uint32_t imports_count,
                    uint32_t imports_format, uint32_t symbols_off,
                    std::vector<Import>& decoded, std::string& error) {
  uint64_t entry_size = 0;
  if (imports_format == 1) entry_size = 4;
  else if (imports_format == 2) entry_size = 8;
  else if (imports_format == 3) entry_size = 16;
  else { error = "unsupported chained imports format"; return false; }
  const uint8_t* p = o.payload;
  if (symbols_off > o.payload_size) { error = "symbols offset is outside fixup payload"; return false; }
  const uint64_t symbols_size = o.payload_size - symbols_off;
  const uint64_t total = static_cast<uint64_t>(imports_count) * entry_size;
  if (!range_ok(o.payload_size, imports_off, total)) {
    error = "imports table extends beyond fixup payload"; return false;
  }
  decoded.reserve(imports_count);
  for (uint32_t i = 0; i < imports_count; ++i) {
    const uint8_t* e = p + imports_off + static_cast<uint64_t>(i) * entry_size;
    uint64_t name_off = 0;
    Import imp;
    if (imports_format == 1 || imports_format == 2) {
      const uint32_t raw = u32(e);
      imp.library_ordinal = static_cast<int8_t>(raw & 0xff);
      imp.weak = ((raw >> 8) & 1) != 0;
      name_off = raw >> 9;
      if (imports_format == 2) imp.addend = static_cast<int32_t>(u32(e + 4));
    } else {
      const uint64_t raw = u64(e);
      imp.library_ordinal = static_cast<int16_t>(raw & 0xffff);
      imp.weak = ((raw >> 16) & 1) != 0;
      if (((raw >> 17) & 0x7fff) != 0) { error = "reserved bits set in ADDEND64 import"; return false; }
      name_off = raw >> 32;
      imp.addend = static_cast<int64_t>(u64(e + 8));
    }
    if (!read_symbol(p + symbols_off, symbols_size, static_cast<uint32_t>(name_off), imp.symbol)) {
      error = "import symbol name offset is invalid or unterminated"; return false;
    }
    decoded.push_back(std::move(imp));
  }
  return true;
}

bool fix_chain(const FixupOptions& o, ImageSink& image, uint64_t page_start,
               uint64_t page_size, uint16_t pointer_format,
               const std::vector<Import>& imports, std::vector<Pending>& pending,
               std::string& error) {
  if (pointer_format != 2 && pointer_format != 6) {
    error = "unsupported chained pointer format (only DYLD_CHAINED_PTR_64 and _64_OFFSET are implemented)";
    return false;
  }
  constexpr uint64_t stride = 4;
  uint64_t loc = page_start;
  uint64_t steps = 0;
  while (true) {
    if (!range_ok(image.size(), loc, 8) || loc < page_start || loc + 8 > page_start + page_size) {
      error = "fixup chain pointer leaves its segment page or image"; return false;
    }
    // A valid chain has strictly increasing locations. This upper bound also
    // rejects cycles and pathological next values before any bytes are written.
    if (++steps > (page_size / stride) + 1) { error = "fixup chain does not terminate"; return false; }
    uint64_t raw = 0;
    if (!image.read64(loc, raw)) { error = "cannot read fixup pointer from destination image"; return false; }
    const bool bind = (raw >> 63) != 0;
    const uint16_t next = static_cast<uint16_t>((raw >> 51) & 0xfff);
    Pending item;
    item.original = raw;
    item.record.image_offset = loc;
    item.record.bind = bind;
    if (bind) {
      const uint32_t ordinal = static_cast<uint32_t>(raw & 0xffffff);
      if (ordinal >= imports.size()) { error = "chained bind import ordinal is out of range"; return false; }
      const int64_t embedded_addend = sign_extend((raw >> 24) & 0xff, 8);
      uint64_t resolved = 0;
      if (!o.resolve_import || !o.resolve_import(imports[ordinal], resolved)) {
        if (!imports[ordinal].weak) { error = "unresolved non-weak chained import: " + imports[ordinal].symbol; return false; }
        resolved = 0;
      }
      int64_t total_addend = 0;
      if ((embedded_addend > 0 && imports[ordinal].addend > std::numeric_limits<int64_t>::max() - embedded_addend) ||
          (embedded_addend < 0 && imports[ordinal].addend < std::numeric_limits<int64_t>::min() - embedded_addend)) {
        error = "combined import addend overflows"; return false;
      }
      total_addend = imports[ordinal].addend + embedded_addend;
      if (!add_signed(resolved, total_addend, item.record.value)) { error = "bound pointer value overflows"; return false; }
      item.record.import_index = ordinal;
    } else {
      const uint64_t target = (raw & 0xfffffffffULL) | (((raw >> 36) & 0xff) << 56);
      uint64_t relocated = 0;
      if (pointer_format == 2) {
        if (o.target_base >= o.preferred_base) {
          const uint64_t slide = o.target_base - o.preferred_base;
          if (target > std::numeric_limits<uint64_t>::max() - slide) { error = "rebased pointer overflows"; return false; }
          relocated = target + slide;
        } else {
          const uint64_t slide = o.preferred_base - o.target_base;
          if (target < slide) { error = "rebased pointer underflows"; return false; }
          relocated = target - slide;
        }
      } else {
        if (o.target_base > std::numeric_limits<uint64_t>::max() - target) { error = "offset rebase overflows"; return false; }
        relocated = o.target_base + target;
      }
      item.record.value = relocated;
    }
    pending.push_back(item);
    if (next == 0) break;
    const uint64_t delta = static_cast<uint64_t>(next) * stride;
    if (loc > std::numeric_limits<uint64_t>::max() - delta) { error = "fixup chain offset overflows"; return false; }
    loc += delta;
  }
  return true;
}
} // namespace

bool apply_chained_fixups(const FixupOptions& o, ImageSink& image,
                          std::vector<Import>& imports,
                          std::vector<FixupRecord>* records,
                          std::string& error) {
  if (!o.payload || o.payload_size < 28) { error = "fixup payload is missing or shorter than its header"; return false; }
  const uint8_t* p = o.payload;
  const uint32_t version = u32(p), starts_off = u32(p + 4), imports_off = u32(p + 8);
  const uint32_t symbols_off = u32(p + 12), imports_count = u32(p + 16);
  const uint32_t imports_format = u32(p + 20), symbols_format = u32(p + 24);
  if (version != 0) { error = "unsupported chained fixups version"; return false; }
  if (symbols_format != 0) { error = "compressed chained symbol pools are not supported"; return false; }
  if (!range_ok(o.payload_size, starts_off, 4)) { error = "starts table offset is invalid"; return false; }
  if (starts_off + 4ULL > o.payload_size) { error = "truncated starts table"; return false; }
  const uint32_t seg_count = u32(p + starts_off);
  if (seg_count > o.segments.size()) { error = "starts table has more segments than Mach-O segment commands"; return false; }
  const uint64_t offsets_bytes = static_cast<uint64_t>(seg_count) * 4;
  if (!range_ok(o.payload_size, static_cast<uint64_t>(starts_off) + 4, offsets_bytes)) {
    error = "segment starts offsets exceed fixup payload"; return false;
  }

  std::vector<Import> decoded;
  if (!decode_imports(o, imports_off, imports_count, imports_format, symbols_off, decoded, error)) return false;
  std::vector<Pending> pending;
  for (uint32_t si = 0; si < seg_count; ++si) {
    const uint32_t rel = u32(p + starts_off + 4 + static_cast<uint64_t>(si) * 4);
    if (rel == 0) continue;
    const uint64_t info = static_cast<uint64_t>(starts_off) + rel;
    if (!range_ok(o.payload_size, info, 22)) { error = "segment starts record is truncated"; return false; }
    const uint32_t struct_size = u32(p + info);
    const uint16_t page_size = u16(p + info + 4), pointer_format = u16(p + info + 6);
    const uint64_t seg_offset = u64(p + info + 8);
    const uint16_t page_count = u16(p + info + 20);
    if (struct_size < 22 || !range_ok(o.payload_size, info, struct_size) ||
        static_cast<uint64_t>(page_count) * 2 > struct_size - 22) {
      error = "invalid segment starts record size"; return false;
    }
    if (page_size == 0 || si >= o.segments.size()) { error = "invalid page size or segment index"; return false; }
    const SegmentLayout& seg = o.segments[si];
    if (seg_offset != seg.image_offset || !range_ok(image.size(), seg.image_offset, seg.vmsize)) {
      error = "fixup segment offset does not match the supplied image layout"; return false;
    }
    const uint64_t starts_array = info + 22;
    const uint64_t overflow_count = (struct_size - 22) / 2;
    for (uint32_t pi = 0; pi < page_count; ++pi) {
      uint16_t start = u16(p + starts_array + static_cast<uint64_t>(pi) * 2);
      if (start == kStartNone) continue;
      std::vector<uint16_t> page_starts;
      if ((start & kStartMulti) == 0) page_starts.push_back(start);
      else {
        uint32_t oi = start & ~kStartMulti;
        bool last = false;
        uint64_t n = 0;
        while (!last) {
          if (oi >= overflow_count || ++n > overflow_count) { error = "multi-start chain list is out of bounds or unterminated"; return false; }
          const uint16_t entry = u16(p + starts_array + static_cast<uint64_t>(oi) * 2);
          last = (entry & kStartLast) != 0;
          page_starts.push_back(static_cast<uint16_t>(entry & static_cast<uint16_t>(~kStartLast)));
          ++oi;
        }
      }
      const uint64_t page_off = static_cast<uint64_t>(pi) * page_size;
      if (page_off >= seg.vmsize) { error = "page start exceeds segment virtual size"; return false; }
      const uint64_t this_page_size = std::min<uint64_t>(page_size, seg.vmsize - page_off);
      for (uint16_t page_start : page_starts) {
        if (page_start >= this_page_size) { error = "chain start offset exceeds page"; return false; }
        const uint64_t base = seg.image_offset + page_off;
        if (base > std::numeric_limits<uint64_t>::max() - page_start) { error = "chain start offset overflows"; return false; }
        if (!fix_chain(o, image, base + page_start, this_page_size - page_start,
                       pointer_format, decoded, pending, error)) return false;
      }
    }
  }

  // Resolve and validate the whole image before writing, so malformed input or
  // a missing required symbol never leaves a partially relocated buffer.
  for (const Pending& item : pending) {
    if (!image.write64(item.record.image_offset, item.record.value)) {
      error = "destination rejected a validated fixup write"; return false;
    }
  }
  imports = std::move(decoded);
  if (records) {
    records->clear(); records->reserve(pending.size());
    for (const Pending& item : pending) records->push_back(item.record);
  }
  return true;
}
} // namespace macho
