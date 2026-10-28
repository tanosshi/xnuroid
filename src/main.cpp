#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {
constexpr uint32_t kMagic64 = 0xfeedfacf;
constexpr int32_t kArm64 = 0x0100000c;
constexpr uint32_t kSegment64 = 0x19;
constexpr uint32_t kDyldInfo = 0x22;
constexpr uint32_t kDyldInfoOnly = 0x80000022;
constexpr uint32_t kEncryption64 = 0x2c;
constexpr uint32_t kChainedFixups = 0x80000034;
constexpr uint32_t kDylibLoad = 0x0c;
constexpr uint32_t kDylibWeak = 0x80000018;
constexpr uint32_t kDylibReexport = 0x8000001f;
constexpr uint32_t kDylibUpward = 0x80000023;
constexpr uint32_t kDylibLazy = 0x20;
constexpr uint32_t kDylibId = 0x0d;
constexpr uint32_t kRpath = 0x8000001c;
constexpr uint32_t kBuildVersion = 0x32;
constexpr uint32_t kUuid = 0x1b;
constexpr uint32_t kCodeSignature = 0x1d;
constexpr uint32_t kSourceVersion = 0x2a;
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
std::string fixed_string(const uint8_t* p, size_t max) {
  size_t n = 0;
  while (n < max && p[n] != 0) ++n;
  return std::string(reinterpret_cast<const char*>(p), n);
}
bool bounded_string(const uint8_t* p, uint64_t size, uint64_t offset, std::string& out) {
  if (offset >= size) return false;
  uint64_t end = offset;
  while (end < size && p[end] != 0) ++end;
  if (end == size) return false;
  out.assign(reinterpret_cast<const char*>(p + offset), static_cast<size_t>(end - offset));
  return true;
}
struct Section { std::string seg, name; uint64_t addr{}, size{}; uint32_t align{}, flags{}; };
struct Segment { std::string name; uint64_t vmaddr{}, vmsize{}, fileoff{}, filesize{}; uint32_t maxprot{}, initprot{}; };
struct Dylib { std::string name; };
struct BindInfo { uint32_t off{}, size{}; };
struct RebaseInfo { uint32_t off{}, size{}; };
struct Command { uint32_t value{}, size{}; };
struct BuildVersion { bool present{}; uint32_t platform{}, minos{}, sdk{}, tool_count{}; };
struct Import { int32_t ordinal{}; bool weak{}; std::string symbol; int64_t addend{}; };
struct ClassicBinding { int32_t ordinal{}; bool weak{}; std::string symbol; int64_t addend{}; uint8_t type{}; uint32_t segment{}; uint64_t offset{}; std::string stream; };
struct FixupSegment { uint32_t index{}; uint16_t page_size{}, pointer_format{}, page_count{}; uint64_t segment_offset{}; uint32_t max_valid_pointer{}; uint32_t starts_none{}, starts_single{}, starts_multi{}, overflow_starts{}; uint64_t chains{}, words{}, rebases{}, binds{}, terminated{}, invalid{}; bool walk_supported{true}; };
struct Inventory {
  int32_t cpu{}, subtype{};
  uint32_t filetype{}, ncmds{};
  bool has_encryption{}; uint32_t cryptid{};
  bool has_fixups{}; uint32_t fixups_off{}, fixups_size{};
  std::vector<Segment> segments; // includes PAGEZERO, preserving load-command indices
  std::vector<Section> sections;
  std::vector<Dylib> dylibs;
  std::vector<std::string> rpaths;
  std::string dylib_id;
  BuildVersion build_version;
  bool code_signature_present{};
  bool uuid_present{};
  std::string uuid;
  uint64_t source_version{};
  std::vector<Command> commands;
  std::vector<uint32_t> unknown_required_commands;
  std::vector<Import> imports;
  std::vector<ClassicBinding> classic_bindings;
  std::vector<FixupSegment> fixup_segments;
  std::vector<uint16_t> pointer_formats;
  std::map<std::string, uint64_t> chained_imports;
  uint64_t chained_import_count{};
  bool chained_imports_supported{true};
  uint32_t imports_format{};
  BindInfo bind, weak_bind, lazy_bind;
  RebaseInfo rebase;
  uint64_t rebase_count{};
  bool classic_streams_valid{true};
  bool threaded_bind_seen{};
  std::vector<std::string> classic_stream_errors;
};

std::string library_for_ordinal(int32_t ordinal, const std::vector<Dylib>& libs) {
  if (ordinal > 0 && static_cast<size_t>(ordinal) <= libs.size()) return libs[static_cast<size_t>(ordinal - 1)].name;
  switch (ordinal) {
    case 0: return "<self>";
    case -1: return "<main-executable>";
    case -2: return "<flat-lookup>";
    case -3: return "<weak-lookup>";
    default: return "<special-ordinal:" + std::to_string(ordinal) + ">";
  }
}

bool read_uleb(const uint8_t* p, uint64_t size, uint64_t& cursor, uint64_t& value) {
  value = 0; unsigned shift = 0;
  for (unsigned i = 0; i < 10; ++i) {
    if (cursor >= size) return false;
    const uint8_t byte = p[cursor++];
    if (shift == 63 && (byte & 0x7e) != 0) return false;
    value |= static_cast<uint64_t>(byte & 0x7f) << shift;
    if ((byte & 0x80) == 0) return true;
    shift += 7;
  }
  return false;
}
bool read_sleb(const uint8_t* p, uint64_t size, uint64_t& cursor, int64_t& value) {
  uint64_t result = 0; unsigned shift = 0; uint8_t byte = 0;
  for (unsigned i = 0; i < 10; ++i) {
    if (cursor >= size) return false;
    byte = p[cursor++]; result |= static_cast<uint64_t>(byte & 0x7f) << shift; shift += 7;
    if ((byte & 0x80) == 0) {
      if (shift < 64 && (byte & 0x40)) result |= (~uint64_t{0}) << shift;
      value = static_cast<int64_t>(result); return true;
    }
  }
  return false;
}
bool add_checked(uint64_t& value, uint64_t first, uint64_t second = 0) {
  if (first > UINT64_MAX - second || value > UINT64_MAX - first - second) return false;
  value += first + second; return true;
}

bool parse_bind_stream(const std::vector<uint8_t>& file, BindInfo stream, const char* stream_name,
                       const std::vector<Segment>& segments, Inventory& out, std::string& error) {
  if (stream.size == 0) return true;
  const uint8_t* p = file.data() + stream.off; const uint64_t n = stream.size;
  uint64_t cur = 0, address = 0, ordinal = 0; uint32_t seg_index = 0;
  int64_t addend = 0; uint8_t type = 1, symbol_flags = 0; std::string symbol;
  auto emit = [&]() -> bool {
    if (seg_index >= segments.size() || address > segments[seg_index].vmsize || 8 > segments[seg_index].vmsize - address) {
      error = std::string(stream_name) + " bind location exceeds segment bounds"; return false;
    }
    if (out.classic_bindings.size() >= 10000000) { error = "classic bind count exceeds safety limit"; return false; }
    int32_t ord = static_cast<int32_t>(ordinal);
    out.classic_bindings.push_back({ord, (symbol_flags & 1) != 0 || std::string(stream_name) == "weak", symbol, addend, type, seg_index, address, stream_name});
    return true;
  };
  while (cur < n) {
    const uint8_t byte = p[cur++], op = byte & 0xf0, imm = byte & 0x0f;
    uint64_t u = 0, count = 0, skip = 0;
    switch (op) {
      case 0x00: // DONE; lazy streams contain a sequence of independent programs
        if (std::string(stream_name) != "lazy") return true;
        address = 0; ordinal = 0; addend = 0; type = 1; symbol_flags = 0; symbol.clear(); seg_index = 0;
        break;
      case 0x10: ordinal = imm; break;
      case 0x20:
        if (!read_uleb(p, n, cur, ordinal)) { error = "invalid bind ordinal ULEB"; return false; }
        break;
      case 0x30:
        ordinal = imm == 0 ? 0 : static_cast<int8_t>(0xf0 | imm);
        break;
      case 0x40: {
        symbol_flags = imm;
        uint64_t begin = cur;
        while (cur < n && p[cur] != 0) ++cur;
        if (cur == n) { error = "unterminated symbol in bind stream"; return false; }
        symbol.assign(reinterpret_cast<const char*>(p + begin), static_cast<size_t>(cur - begin)); ++cur;
        break;
      }
      case 0x50: type = imm; break;
      case 0x60:
        if (!read_sleb(p, n, cur, addend)) { error = "invalid bind addend SLEB"; return false; }
        break;
      case 0x70:
        seg_index = imm;
        if (!read_uleb(p, n, cur, address)) { error = "invalid bind segment offset"; return false; }
        break;
      case 0x80:
        if (!read_uleb(p, n, cur, u)) { error = std::string("invalid bind address increment in ") + stream_name + " at byte " + std::to_string(cur); return false; }
        address += u; break;
      case 0x90:
        if (!emit() || address > UINT64_MAX - 8) return false;
        address += 8; break;
      case 0xa0:
        if (!read_uleb(p, n, cur, u) || !emit()) { if (error.empty()) error = "invalid bind add-address operation"; return false; }
        address += u + 8; break;
      case 0xb0:
        if (!emit() || address > UINT64_MAX - static_cast<uint64_t>(imm) * 8 - 8) return false;
        address += static_cast<uint64_t>(imm) * 8 + 8; break;
      case 0xc0:
        if (!read_uleb(p, n, cur, count) || !read_uleb(p, n, cur, skip) || count > 10000000) { error = "invalid repeated bind operation"; return false; }
        for (uint64_t i = 0; i < count; ++i) {
          if (!emit()) { if (error.empty()) error = "bind address overflow"; return false; }
          address += skip + 8;
        }
        break;
      case 0xd0:
        out.threaded_bind_seen = true;
        if (imm == 0) { if (!read_uleb(p, n, cur, u)) { error = "invalid threaded bind table size"; return false; } }
        else if (imm == 1) { error = "threaded bind application requires pointer-chain interpretation; inventory marks it unsupported"; return false; }
        else { error = "unknown threaded bind subopcode"; return false; }
        break;
      default: error = "unknown classic bind opcode"; return false;
    }
  }
  return true;
}

bool parse_rebase_stream(const std::vector<uint8_t>& file, RebaseInfo stream,
                         const std::vector<Segment>& segments, Inventory& out, std::string& error) {
  if (stream.size == 0) return true;
  const uint8_t* p = file.data() + stream.off; const uint64_t n = stream.size;
  uint64_t cur = 0, address = 0; uint32_t seg_index = 0;
  auto emit = [&]() -> bool {
    if (seg_index >= segments.size() || address > segments[seg_index].vmsize || 8 > segments[seg_index].vmsize - address) {
      error = "rebase location exceeds segment bounds"; return false;
    }
    ++out.rebase_count; return true;
  };
  while (cur < n) {
    const uint8_t byte = p[cur++], op = byte & 0xf0, imm = byte & 0x0f;
    uint64_t u = 0, count = 0, skip = 0;
    switch (op) {
      case 0x00: return true;
      case 0x10: break; // SET_TYPE_IMM
      case 0x20:
        seg_index = imm;
        if (!read_uleb(p, n, cur, address)) { error = "invalid rebase segment offset"; return false; }
        break;
      case 0x30:
        if (!read_uleb(p, n, cur, u) || address > UINT64_MAX - u) { error = "invalid rebase address increment"; return false; }
        address += u; break;
      case 0x40:
        if (address > UINT64_MAX - static_cast<uint64_t>(imm) * 8) { error = "rebase address overflow"; return false; }
        address += static_cast<uint64_t>(imm) * 8; break;
      case 0x50:
        for (uint8_t i = 0; i < imm; ++i) { if (!emit() || address > UINT64_MAX - 8) return false; address += 8; }
        break;
      case 0x60:
        if (!read_uleb(p, n, cur, count) || count > 10000000) { error = "invalid rebase count"; return false; }
        for (uint64_t i = 0; i < count; ++i) { if (!emit() || address > UINT64_MAX - 8) return false; address += 8; }
        break;
      case 0x70:
        if (!emit() || !read_uleb(p, n, cur, u) || !add_checked(address, u, 8)) { if (error.empty()) error = "invalid rebase add-address operation"; return false; }
        break;
      case 0x80:
        if (!read_uleb(p, n, cur, count) || !read_uleb(p, n, cur, skip) || count > 10000000) { error = "invalid repeated rebase operation"; return false; }
        for (uint64_t i = 0; i < count; ++i) { if (!emit() || !add_checked(address, skip, 8)) { if (error.empty()) error = "rebase address overflow"; return false; } }
        break;
      default: error = "unknown classic rebase opcode"; return false;
    }
  }
  return true;
}

bool parse_fixup_inventory(const std::vector<uint8_t>& b, Inventory& r, std::string& err) {
  if (!r.has_fixups) return true;
  const uint8_t* p = b.data() + r.fixups_off;
  const uint64_t n = r.fixups_size;
  if (n < 28) { err = "chained-fixups payload is shorter than its header"; return false; }
  const uint32_t version = u32(p), starts_off = u32(p + 4), imports_off = u32(p + 8);
  const uint32_t symbols_off = u32(p + 12), import_count = u32(p + 16);
  const uint32_t imports_format = u32(p + 20), symbols_format = u32(p + 24);
  r.imports_format = imports_format; r.chained_import_count = import_count;
  if (version != 0) { err = "unsupported chained-fixups version"; return false; }
  if (symbols_format != 0) { r.chained_imports_supported = false; }
  if (!range_ok(n, starts_off, 4)) { err = "invalid chained starts offset"; return false; }
  const uint32_t seg_count = u32(p + starts_off);
  if (seg_count > r.segments.size() || !range_ok(n, static_cast<uint64_t>(starts_off) + 4,
                                                  static_cast<uint64_t>(seg_count) * 4)) {
    err = "chained starts segment-offset table is invalid"; return false;
  }
  for (uint32_t si = 0; si < seg_count; ++si) {
    const uint32_t rel = u32(p + starts_off + 4 + static_cast<uint64_t>(si) * 4);
    if (rel == 0) continue;
    const uint64_t info = static_cast<uint64_t>(starts_off) + rel;
    if (!range_ok(n, info, 22)) { err = "truncated chained starts-in-segment"; return false; }
    const uint32_t size = u32(p + info);
    const uint16_t page_count = u16(p + info + 20), format = u16(p + info + 6);
    if (size < 22 || !range_ok(n, info, size) || static_cast<uint64_t>(page_count) * 2 > size - 22) {
      err = "invalid chained starts-in-segment size"; return false;
    }
    r.pointer_formats.push_back(format);
    FixupSegment fs; fs.index = si; fs.page_size = u16(p + info + 4); fs.pointer_format = format;
    fs.segment_offset = u64(p + info + 8); fs.max_valid_pointer = u32(p + info + 16); fs.page_count = page_count;
    const uint64_t starts = info + 22;
    const uint64_t overflow_count = (size - 22) / 2;
    const Segment& macho_seg = r.segments[si];
    for (uint32_t pi = 0; pi < page_count; ++pi) {
      const uint16_t start = u16(p + starts + static_cast<uint64_t>(pi) * 2);
      if (start == kStartNone) { ++fs.starts_none; continue; }
      std::vector<uint16_t> page_starts;
      if ((start & kStartMulti) == 0) { ++fs.starts_single; page_starts.push_back(start); }
      else {
        ++fs.starts_multi;
        uint32_t oi = start & static_cast<uint16_t>(~kStartMulti); uint64_t count = 0; bool last = false;
        while (!last && oi < overflow_count && ++count <= overflow_count) {
          const uint16_t entry = u16(p + starts + static_cast<uint64_t>(oi) * 2);
          ++fs.overflow_starts; last = (entry & kStartLast) != 0;
          page_starts.push_back(static_cast<uint16_t>(entry & static_cast<uint16_t>(~kStartLast))); ++oi;
        }
        if (!last) { ++fs.invalid; continue; }
      }
      const uint16_t fmt = fs.pointer_format;
      const bool fmt64 = fmt == 2 || fmt == 6;
      const bool fmtArm64e = fmt == 1 || fmt == 9 || fmt == 12 || fmt == 13;
      if (!fmt64 && !fmtArm64e) { fs.walk_supported = false; continue; }
      const uint64_t stride = fmt64 ? 4 : 8;
      const uint64_t page_off = static_cast<uint64_t>(pi) * fs.page_size;
      for (uint16_t chain_start : page_starts) {
        ++fs.chains;
        uint64_t loc = page_off + chain_start;
        uint64_t steps = 0; bool done = false, chain_valid = true;
        while (!done && ++steps <= (fs.page_size / stride) + 1) {
          if (loc > std::numeric_limits<uint64_t>::max() - 8 || loc + 8 > page_off + fs.page_size ||
              loc + 8 > macho_seg.filesize || !range_ok(b.size(), macho_seg.fileoff + loc, 8)) {
            ++fs.invalid; chain_valid = false; break;
          }
          const uint64_t raw = u64(b.data() + macho_seg.fileoff + loc);
          const bool is_bind = fmt64 ? ((raw >> 63) != 0) : (fmt == 13 ? false : (((raw >> 62) & 1) != 0));
          const uint16_t next = fmt64 ? static_cast<uint16_t>((raw >> 51) & 0xfff)
                                      : static_cast<uint16_t>((raw >> (fmt == 13 ? 52 : 51)) & 0x7ff);
          ++fs.words; if (is_bind) ++fs.binds; else ++fs.rebases;
          if (is_bind) {
            const uint32_t ordinal = static_cast<uint32_t>(raw & (fmt == 12 ? 0xffffffULL : 0xffffULL));
            if (ordinal >= import_count) { ++fs.invalid; chain_valid = false; break; }
          }
          if (next == 0) { ++fs.terminated; done = true; break; }
          const uint64_t delta = static_cast<uint64_t>(next) * stride;
          if (loc > std::numeric_limits<uint64_t>::max() - delta) { ++fs.invalid; chain_valid = false; break; }
          loc += delta;
        }
        if (!done && chain_valid) ++fs.invalid;
      }
    }
    r.fixup_segments.push_back(fs);
  }
  if (!r.chained_imports_supported) return true;
  uint32_t entry_size = imports_format == 1 ? 4 : imports_format == 2 ? 8 : imports_format == 3 ? 16 : 0;
  if (entry_size == 0) { r.chained_imports_supported = false; return true; }
  if (!range_ok(n, imports_off, static_cast<uint64_t>(import_count) * entry_size) || symbols_off > n) {
    err = "chained import or symbol table range is invalid"; return false;
  }
  for (uint32_t i = 0; i < import_count; ++i) {
    const uint8_t* e = p + imports_off + static_cast<uint64_t>(i) * entry_size;
    int32_t ordinal = 0; uint32_t name_off = 0;
    if (imports_format < 3) {
      const uint32_t raw = u32(e);
      ordinal = static_cast<int8_t>(raw & 0xff); name_off = raw >> 9;
    } else {
      const uint64_t raw = u64(e);
      ordinal = static_cast<int16_t>(raw & 0xffff); name_off = static_cast<uint32_t>(raw >> 32);
    }
    std::string symbol;
    if (!bounded_string(p + symbols_off, n - symbols_off, name_off, symbol)) {
      err = "chained import symbol name is invalid"; return false;
    }
    const bool weak = imports_format < 3 ? ((u32(e) >> 8) & 1) != 0 : ((u64(e) >> 16) & 1) != 0;
    int64_t addend = 0;
    if (imports_format == 2) addend = static_cast<int32_t>(u32(e + 4));
    else if (imports_format == 3) addend = static_cast<int64_t>(u64(e + 8));
    r.imports.push_back({ordinal, weak, symbol, addend});
    ++r.chained_imports[library_for_ordinal(ordinal, r.dylibs)];
  }
  return true;
}

bool parse(const std::vector<uint8_t>& b, Inventory& r, std::string& err) {
  if (b.size() < 32) { err = "file is shorter than mach_header_64"; return false; }
  if (u32(b.data()) != kMagic64) { err = "not thin little-endian Mach-O 64 (fat files are not supported)"; return false; }
  r.cpu = static_cast<int32_t>(u32(b.data() + 4));
  r.subtype = static_cast<int32_t>(u32(b.data() + 8));
  r.filetype = u32(b.data() + 12); r.ncmds = u32(b.data() + 16);
  const uint32_t command_bytes = u32(b.data() + 20);
  if (r.cpu != kArm64) { err = "Mach-O CPU type is not arm64"; return false; }
  if (!range_ok(b.size(), 32, command_bytes)) { err = "load-command region exceeds file"; return false; }
  const uint64_t end = 32ULL + command_bytes;
  uint64_t pos = 32;
  for (uint32_t i = 0; i < r.ncmds; ++i) {
    if (!range_ok(end, pos, 8)) { err = "truncated load-command header"; return false; }
    const uint32_t cmd = u32(b.data() + pos), size = u32(b.data() + pos + 4);
    if (size < 8 || !range_ok(end, pos, size)) { err = "invalid load-command size"; return false; }
    r.commands.push_back({cmd, size});
    if ((cmd & 0x80000000u) != 0) {
      static const uint32_t known_required[] = {0x80000018u, 0x8000001cu, 0x80000022u, 0x8000001fu,
        0x80000023u, 0x80000028u, 0x80000033u, 0x80000034u};
      bool known = false; for (uint32_t k : known_required) if (cmd == k) known = true;
      if (!known) r.unknown_required_commands.push_back(cmd);
    }
    const uint8_t* c = b.data() + pos;
    if (cmd == kSegment64) {
      if (size < 72) { err = "short LC_SEGMENT_64"; return false; }
      const uint32_t nsects = u32(c + 64);
      if (72ULL + static_cast<uint64_t>(nsects) * 80ULL > size) { err = "section array exceeds segment command"; return false; }
      Segment seg{fixed_string(c + 8, 16), u64(c + 24), u64(c + 32), u64(c + 40), u64(c + 48), u32(c + 56), u32(c + 60)};
      if (seg.filesize > seg.vmsize || !range_ok(b.size(), seg.fileoff, seg.filesize)) { err = "invalid segment range"; return false; }
      r.segments.push_back(seg);
      for (uint32_t s = 0; s < nsects; ++s) {
        const uint8_t* sc = c + 72 + static_cast<uint64_t>(s) * 80;
        r.sections.push_back({fixed_string(sc + 16, 16), fixed_string(sc, 16), u64(sc + 32), u64(sc + 40), u32(sc + 52), u32(sc + 64)});
      }
    } else if (cmd == kDylibLoad || cmd == kDylibWeak || cmd == kDylibReexport || cmd == kDylibUpward || cmd == kDylibLazy || cmd == kDylibId) {
      if (size < 24) { err = "short dylib load command"; return false; }
      const uint32_t nameoff = u32(c + 8); std::string name;
      if (nameoff < 8 || nameoff >= size || !bounded_string(c, size, nameoff, name)) { err = "invalid dylib name"; return false; }
      if (cmd == kDylibId) r.dylib_id = std::move(name); else r.dylibs.push_back({std::move(name)});
    } else if (cmd == kRpath) {
      if (size < 12) { err = "short LC_RPATH"; return false; }
      const uint32_t nameoff = u32(c + 8); std::string path;
      if (nameoff < 8 || nameoff >= size || !bounded_string(c, size, nameoff, path)) { err = "invalid LC_RPATH string"; return false; }
      r.rpaths.push_back(std::move(path));
    } else if (cmd == kBuildVersion) {
      if (size < 24) { err = "short LC_BUILD_VERSION"; return false; }
      const uint32_t tools = u32(c + 20);
      if (24ULL + static_cast<uint64_t>(tools) * 8 > size) { err = "LC_BUILD_VERSION tool array exceeds command"; return false; }
      r.build_version = {true, u32(c + 8), u32(c + 12), u32(c + 16), tools};
    } else if (cmd == kCodeSignature) {
      r.code_signature_present = true;
    } else if (cmd == kUuid) {
      if (size < 24) { err = "short LC_UUID"; return false; }
      r.uuid_present = true;
      std::ostringstream hex; hex << std::hex << std::setfill('0');
      for (size_t k = 0; k < 16; ++k) { if (k == 4 || k == 6 || k == 8 || k == 10) hex << '-'; hex << std::setw(2) << static_cast<unsigned>(c[8 + k]); }
      r.uuid = hex.str();
    } else if (cmd == kSourceVersion) {
      if (size < 16) { err = "short LC_SOURCE_VERSION"; return false; }
      r.source_version = u64(c + 8);
    } else if (cmd == kEncryption64) {
      if (size < 24) { err = "short LC_ENCRYPTION_INFO_64"; return false; }
      r.has_encryption = true; r.cryptid = u32(c + 16);
    } else if (cmd == kChainedFixups) {
      if (size < 16) { err = "short LC_DYLD_CHAINED_FIXUPS"; return false; }
      r.has_fixups = true; r.fixups_off = u32(c + 8); r.fixups_size = u32(c + 12);
      if (!range_ok(b.size(), r.fixups_off, r.fixups_size)) { err = "chained-fixups range exceeds file"; return false; }
    } else if (cmd == kDyldInfo || cmd == kDyldInfoOnly) {
      if (size < 48) { err = "short dyld-info command"; return false; }
      r.rebase = {u32(c + 8), u32(c + 12)};
      r.bind = {u32(c + 16), u32(c + 20)};
      r.weak_bind = {u32(c + 24), u32(c + 28)};
      r.lazy_bind = {u32(c + 32), u32(c + 36)};
      if (!range_ok(b.size(), r.rebase.off, r.rebase.size) || !range_ok(b.size(), r.bind.off, r.bind.size) || !range_ok(b.size(), r.weak_bind.off, r.weak_bind.size) ||
          !range_ok(b.size(), r.lazy_bind.off, r.lazy_bind.size)) { err = "dyld bind opcode range exceeds file"; return false; }
    }
    pos += size;
  }
  if (pos != end) { err = "ncmds does not consume sizeofcmds exactly"; return false; }
  if (!parse_fixup_inventory(b, r, err)) return false;
  if (!parse_rebase_stream(b, r.rebase, r.segments, r, err)) { r.classic_streams_valid = false; r.classic_stream_errors.push_back("rebase: " + err); err.clear(); }
  if (!parse_bind_stream(b, r.bind, "regular", r.segments, r, err)) { r.classic_streams_valid = false; r.classic_stream_errors.push_back("regular: " + err); err.clear(); }
  if (!parse_bind_stream(b, r.weak_bind, "weak", r.segments, r, err)) { r.classic_streams_valid = false; r.classic_stream_errors.push_back("weak: " + err); err.clear(); }
  if (!parse_bind_stream(b, r.lazy_bind, "lazy", r.segments, r, err)) { r.classic_streams_valid = false; r.classic_stream_errors.push_back("lazy: " + err); err.clear(); }
  return true;
}

const char* pointer_name(uint16_t f) {
  switch (f) {
    case 1: return "DYLD_CHAINED_PTR_ARM64E";
    case 2: return "DYLD_CHAINED_PTR_64";
    case 3: return "DYLD_CHAINED_PTR_32";
    case 4: return "DYLD_CHAINED_PTR_32_CACHE";
    case 5: return "DYLD_CHAINED_PTR_32_FIRMWARE";
    case 6: return "DYLD_CHAINED_PTR_64_OFFSET";
    case 7: return "DYLD_CHAINED_PTR_ARM64E_KERNEL/OFFSET";
    case 8: return "DYLD_CHAINED_PTR_64_KERNEL_CACHE";
    case 9: return "DYLD_CHAINED_PTR_ARM64E_USERLAND";
    case 10: return "DYLD_CHAINED_PTR_ARM64E_FIRMWARE";
    case 11: return "DYLD_CHAINED_PTR_X86_64_KERNEL_CACHE";
    case 12: return "DYLD_CHAINED_PTR_ARM64E_USERLAND24";
    case 13: return "DYLD_CHAINED_PTR_ARM64E_SHARED_CACHE";
    case 14: return "DYLD_CHAINED_PTR_ARM64E_SEGMENTED";
    default: return "UNKNOWN";
  }
}
const char* platform_name(uint32_t p) {
  switch (p) { case 1: return "macOS"; case 2: return "iOS"; case 3: return "tvOS"; case 4: return "watchOS";
    case 5: return "bridgeOS"; case 6: return "Mac Catalyst"; case 7: return "iOS Simulator";
    case 8: return "tvOS Simulator"; case 9: return "watchOS Simulator"; case 10: return "DriverKit"; default: return "unknown"; }
}
std::string packed_version(uint32_t v) {
  return std::to_string((v >> 16) & 0xffff) + "." + std::to_string((v >> 8) & 0xff) + "." + std::to_string(v & 0xff);
}
std::string json_string(const std::string& s) {
  std::ostringstream o; o << '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': o << "\\\""; break; case '\\': o << "\\\\"; break;
      case '\b': o << "\\b"; break; case '\f': o << "\\f"; break;
      case '\n': o << "\\n"; break; case '\r': o << "\\r"; break; case '\t': o << "\\t"; break;
      default: if (c < 0x20) o << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c) << std::dec;
               else o << static_cast<char>(c);
    }
  }
  o << '"'; return o.str();
}
void print_json(const Inventory& r, const std::string& source) {
  const uint32_t subtype = static_cast<uint32_t>(r.subtype);
  struct BindCounts { uint64_t total{}, weak{}, pointer{}, text{}; };
  std::map<std::string, BindCounts> classic_histogram;
  for (const auto& b : r.classic_bindings) {
    auto& c = classic_histogram[library_for_ordinal(b.ordinal, r.dylibs)];
    ++c.total; if (b.weak) ++c.weak; if (b.type == 1) ++c.pointer; else ++c.text;
  }
  std::cout << "{\n  \"schema\": \"ipa-macho-inventory-v1\",\n  \"source\": " << json_string(source)
            << ",\n  \"architecture\": {\"cputype\": " << r.cpu << ", \"cpusubtype_raw\": " << r.subtype
            << ", \"cpusubtype_hex\": " << json_string([&]{ std::ostringstream s; s << "0x" << std::hex << subtype; return s.str(); }())
            << ", \"subtype_value\": " << (subtype & 0x00ffffffu) << ", \"capability_bits\": " << (subtype >> 24)
            << "},\n  \"filetype\": " << r.filetype << ",\n  \"ncmds\": " << r.ncmds
            << ",\n  \"encryption\": {\"command_present\": " << (r.has_encryption ? "true" : "false")
            << ", \"cryptid\": " << (r.has_encryption ? std::to_string(r.cryptid) : "null") << "},\n";
  std::cout << "  \"build_version\": {\"present\": " << (r.build_version.present ? "true" : "false")
            << ", \"platform\": " << (r.build_version.present ? std::to_string(r.build_version.platform) : "null")
            << ", \"platform_name\": " << json_string(r.build_version.present ? platform_name(r.build_version.platform) : "unknown")
            << ", \"minos\": " << json_string(r.build_version.present ? packed_version(r.build_version.minos) : "")
            << ", \"sdk\": " << json_string(r.build_version.present ? packed_version(r.build_version.sdk) : "")
            << ", \"tool_count\": " << r.build_version.tool_count << "},\n"
            << "  \"dylib_id\": " << json_string(r.dylib_id) << ",\n  \"rpaths\": [";
  for (size_t i = 0; i < r.rpaths.size(); ++i) { if (i) std::cout << ','; std::cout << json_string(r.rpaths[i]); }
  std::cout << "],\n  \"code_signature_command_present\": " << (r.code_signature_present ? "true" : "false")
            << ",\n  \"uuid\": " << (r.uuid_present ? json_string(r.uuid) : "null")
            << ",\n  \"source_version_raw\": " << r.source_version << ",\n";
  std::cout << "  \"load_commands\": [";
  for (size_t i = 0; i < r.commands.size(); ++i) { if (i) std::cout << ','; std::cout << "{\"cmd\": " << r.commands[i].value << ", \"cmdsize\": " << r.commands[i].size << '}'; }
  std::cout << "],\n  \"unknown_required_commands\": [";
  for (size_t i = 0; i < r.unknown_required_commands.size(); ++i) { if (i) std::cout << ','; std::cout << r.unknown_required_commands[i]; }
  std::cout << "],\n  \"compatibility_hard_stop\": " << (!r.unknown_required_commands.empty() ? "true" : "false") << ",\n  \"segments\": [";
  for (size_t i = 0; i < r.segments.size(); ++i) {
    const auto& s = r.segments[i]; if (i) std::cout << ',';
    std::cout << "{\"index\": " << i << ", \"name\": " << json_string(s.name) << ", \"vmaddr\": " << s.vmaddr
              << ", \"vmsize\": " << s.vmsize << ", \"fileoff\": " << s.fileoff << ", \"filesize\": " << s.filesize
              << ", \"maxprot\": " << s.maxprot << ", \"initprot\": " << s.initprot << '}';
  }
  std::cout << "],\n  \"sections\": [";
  for (size_t i = 0; i < r.sections.size(); ++i) {
    const auto& s = r.sections[i]; if (i) std::cout << ',';
    std::cout << "{\"segment\": " << json_string(s.seg) << ", \"name\": " << json_string(s.name)
              << ", \"addr\": " << s.addr << ", \"size\": " << s.size << ", \"align_exponent\": " << s.align
              << ", \"flags\": " << s.flags << ", \"section_type\": " << (s.flags & 0xff)
              << ", \"attributes\": " << (s.flags & 0xffffff00u) << '}';
  }
  std::cout << "],\n  \"dylibs\": [";
  for (size_t i = 0; i < r.dylibs.size(); ++i) { if (i) std::cout << ','; std::cout << json_string(r.dylibs[i].name); }
  std::cout << "],\n  \"dyld_info\": {\"rebase_bytes\": " << r.rebase.size << ", \"rebase_pointers\": " << r.rebase_count
            << ", \"bind_bytes\": " << r.bind.size << ", \"weak_bind_bytes\": " << r.weak_bind.size
            << ", \"lazy_bind_bytes\": " << r.lazy_bind.size << ", \"threaded_bind_seen\": " << (r.threaded_bind_seen ? "true" : "false")
            << ", \"streams_valid\": " << (r.classic_streams_valid ? "true" : "false") << ", \"bindings_by_dylib\": {";
  size_t hi = 0; for (const auto& item : classic_histogram) {
    if (hi++) std::cout << ',';
    std::cout << json_string(item.first) << ":{\"total\": " << item.second.total << ", \"weak\": " << item.second.weak
              << ", \"pointer\": " << item.second.pointer << ", \"text\": " << item.second.text << '}';
  }
  std::cout << "}, \"bindings\": [";
  for (size_t i = 0; i < r.classic_bindings.size(); ++i) {
    const auto& x = r.classic_bindings[i]; if (i) std::cout << ',';
    std::cout << "{\"stream\": " << json_string(x.stream) << ", \"ordinal\": " << x.ordinal
              << ", \"library\": " << json_string(library_for_ordinal(x.ordinal, r.dylibs))
              << ", \"symbol\": " << json_string(x.symbol) << ", \"weak\": " << (x.weak ? "true" : "false")
              << ", \"addend\": " << x.addend << ", \"type\": " << static_cast<unsigned>(x.type)
              << ", \"segment_index\": " << x.segment << ", \"segment\": " << json_string(x.segment < r.segments.size() ? r.segments[x.segment].name : "")
              << ", \"offset\": " << x.offset << '}';
  }
  std::cout << "], \"errors\": [";
  for (size_t i = 0; i < r.classic_stream_errors.size(); ++i) { if (i) std::cout << ','; std::cout << json_string(r.classic_stream_errors[i]); }
  std::cout << "]},\n";
  std::cout << "  \"chained_fixups\": {\"present\": " << (r.has_fixups ? "true" : "false")
            << ", \"imports_format\": " << (r.has_fixups ? std::to_string(r.imports_format) : "null")
            << ", \"imports_count\": " << r.chained_import_count << ", \"imports_supported\": " << (r.chained_imports_supported ? "true" : "false")
            << ", \"segments\": [";
  for (size_t i = 0; i < r.fixup_segments.size(); ++i) {
    const auto& s = r.fixup_segments[i]; if (i) std::cout << ',';
    std::cout << "{\"segment_index\": " << s.index << ", \"segment_name\": " << json_string(s.index < r.segments.size() ? r.segments[s.index].name : "")
              << ", \"page_size\": " << s.page_size << ", \"pointer_format\": " << s.pointer_format
              << ", \"pointer_format_name\": " << json_string(pointer_name(s.pointer_format)) << ", \"segment_offset\": " << s.segment_offset
              << ", \"max_valid_pointer\": " << s.max_valid_pointer << ", \"page_count\": " << s.page_count
              << ", \"pages_none\": " << s.starts_none << ", \"pages_single_start\": " << s.starts_single
              << ", \"pages_multi_start\": " << s.starts_multi << ", \"overflow_starts\": " << s.overflow_starts
              << ", \"chain_walk_supported\": " << (s.walk_supported ? "true" : "false")
              << ", \"chains\": " << s.chains << ", \"pointer_words\": " << s.words
              << ", \"rebases\": " << s.rebases << ", \"binds\": " << s.binds
              << ", \"terminated_chains\": " << s.terminated << ", \"invalid_chains\": " << s.invalid << '}';
  }
  std::cout << "],\n    \"imports_by_dylib\": {";
  size_t ix = 0; for (const auto& item : r.chained_imports) { if (ix++) std::cout << ','; std::cout << json_string(item.first) << ':' << item.second; }
  std::cout << "},\n    \"imports\": [";
  for (size_t i = 0; i < r.imports.size(); ++i) {
    const auto& x = r.imports[i]; if (i) std::cout << ',';
    std::cout << "{\"ordinal\": " << x.ordinal << ", \"library\": " << json_string(library_for_ordinal(x.ordinal, r.dylibs))
              << ", \"symbol\": " << json_string(x.symbol) << ", \"weak\": " << (x.weak ? "true" : "false") << ", \"addend\": " << x.addend << '}';
  }
  std::cout << "]},\n";
  std::map<std::string, uint64_t> objc;
  uint64_t init_count = 0; std::map<std::string, uint64_t> tls_section_bytes; bool auth_pointer_sections = false;
  for (const auto& s : r.sections) {
    if (s.name == "__objc_classrefs" || s.name == "__objc_selrefs" || s.name == "__objc_superrefs" || s.name == "__objc_protorefs")
      objc[s.name] += s.size / 8;
    if (s.name == "__mod_init_func" || s.name == "__init_offsets") init_count += s.size / (s.name == "__init_offsets" ? 4 : 8);
    if (s.name.rfind("__thread_", 0) == 0) tls_section_bytes[s.name] += s.size;
    if (s.name == "__auth_got" || s.name == "__auth_stubs" || s.name == "__auth_ptr") auth_pointer_sections = true;
  }
  std::cout << "  \"objc_reference_slots\": {";
  ix = 0; for (const auto& item : objc) { if (ix++) std::cout << ','; std::cout << json_string(item.first) << ':' << item.second; }
  std::cout << "},\n  \"initializer_entries_estimate\": " << init_count << ",\n  \"tls_section_bytes\": {";
  ix = 0; for (const auto& item : tls_section_bytes) { if (ix++) std::cout << ','; std::cout << json_string(item.first) << ':' << item.second; }
  std::cout << "},\n  \"auth_pointer_sections_present\": " << (auth_pointer_sections ? "true" : "false")
            << ",\n  \"instruction_scans\": null,\n  \"reported_metadata_sections\": [";
  bool first = true;
  for (const auto& s : r.sections) {
    if (s.name.find("__objc_") == 0 || s.name.rfind("__thread_", 0) == 0 || s.name == "__mod_init_func" || s.name == "__init_offsets" || s.name == "__eh_frame" || s.name == "__unwind_info" || s.name == "__gcc_except_tab" || s.name == "__auth_got" || s.name == "__auth_stubs" || s.name == "__auth_ptr")
      { if (!first) std::cout << ','; first = false; std::cout << "{\"segment\": " << json_string(s.seg) << ", \"name\": " << json_string(s.name) << ", \"size\": " << s.size << '}'; }
  }
  std::cout << "]\n}\n";
}
} // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--version") { std::cout << "macho-inspect 0.2.0\n"; return 0; }
  if (argc != 2) { std::cerr << "Usage: macho-inspect <thin-arm64-mach-o>\n"; return 2; }
  std::ifstream in(argv[1], std::ios::binary | std::ios::ate);
  if (!in) { std::cerr << "Error: cannot open input file\n"; return 2; }
  const auto end = in.tellg();
  if (end < 0 || static_cast<uintmax_t>(end) > std::numeric_limits<size_t>::max()) { std::cerr << "Error: invalid or oversized file\n"; return 2; }
  std::vector<uint8_t> bytes(static_cast<size_t>(end)); in.seekg(0);
  if (!bytes.empty() && !in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
    std::cerr << "Error: failed to read file\n"; return 2;
  }
  Inventory inventory; std::string error;
  if (!parse(bytes, inventory, error)) { std::cerr << "Error: " << error << '\n'; return 1; }
  print_json(inventory, argv[1]);
  return inventory.has_encryption && inventory.cryptid != 0 ? 3 : 0;
}
