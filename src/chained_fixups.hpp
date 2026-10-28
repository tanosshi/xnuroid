#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace macho {

struct SegmentLayout {
  uint64_t image_offset{}; // destination-buffer offset for this LC_SEGMENT_64
  uint64_t vmaddr{};
  uint64_t vmsize{};
};

struct Import {
  int32_t library_ordinal{}; // negative values are special ordinals
  bool weak{};
  std::string symbol;
  int64_t addend{};
};

struct FixupRecord {
  uint64_t image_offset{};
  bool bind{};
  uint32_t import_index{};
  uint64_t value{}; // relocated value written to the destination
};

// The core sees offsets and integer addresses only. Android can implement this
// over its mapped image; Windows can use a vector-backed sink and a pretend base.
class ImageSink {
public:
  virtual ~ImageSink() = default;
  virtual uint64_t size() const = 0;
  virtual bool read64(uint64_t offset, uint64_t& value) const = 0;
  virtual bool write64(uint64_t offset, uint64_t value) = 0;
};

struct FixupOptions {
  const uint8_t* payload{};
  size_t payload_size{};
  uint64_t preferred_base{}; // lowest mapped VM address in the image
  uint64_t target_base{};    // runtime address corresponding to destination offset 0
  std::vector<SegmentLayout> segments; // in Mach-O LC_SEGMENT_64 order
  std::function<bool(const Import&, uint64_t&)> resolve_import;
};

bool apply_chained_fixups(const FixupOptions& options, ImageSink& image,
                          std::vector<Import>& imports,
                          std::vector<FixupRecord>* records,
                          std::string& error);

} // namespace macho
