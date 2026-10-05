#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace BadgeJpeg {
constexpr size_t kPrefixBytes = 19;
constexpr size_t kChecksumOffset = 15;
constexpr uint32_t kMaxSlot = INT32_MAX - 1;
constexpr size_t kStorageReserve = 65536;

inline uint32_t little32(const uint8_t* bytes) {
  return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) |
         (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
}
inline bool header(const uint8_t* bytes, size_t count, size_t fileBytes, uint32_t& crc) {
  static const uint8_t prefix[] = {0xff, 0xd8, 0xff, 0xfe, 0x00, 0x0f, 'B', 'D', 'G', 'J', '1'};
  if (count < kPrefixBytes || fileBytes < kPrefixBytes + 2 ||
      memcmp(bytes, prefix, sizeof(prefix)) || little32(bytes + 11) != fileBytes) return false;
  crc = little32(bytes + kChecksumOffset);
  return true;
}
class Integrity {
 public:
  explicit Integrity(const uint32_t* table) : table_(table) {}
  void append(const uint8_t* data, size_t count) {
    for (size_t index = 0; index < count; ++index) {
      const uint8_t byte = data[index];
      const size_t absolute = bytes + index;
      const uint8_t normalized = absolute >= kChecksumOffset && absolute < kChecksumOffset + 4 ? 0 : byte;
      stored_ = table_[(stored_ ^ normalized) & 0xff] ^ (stored_ >> 8);
      transfer_ = table_[(transfer_ ^ byte) & 0xff] ^ (transfer_ >> 8);
      previous_ = last_; last_ = byte;
    }
    bytes += count;
  }
  uint32_t storedCrc() const { return stored_ ^ 0xffffffffU; }
  uint32_t transferCrc() const { return transfer_ ^ 0xffffffffU; }
  const char* finish(size_t length, uint32_t expected) const {
    if (bytes != length) return "storage_read";
    if (previous_ != 0xff || last_ != 0xd9) return "jpeg_eoi";
    return storedCrc() == expected ? nullptr : "stored_crc_mismatch";
  }
  size_t bytes = 0;
 private:
  const uint32_t* table_;
  uint32_t stored_ = 0xffffffffU, transfer_ = 0xffffffffU;
  uint8_t previous_ = 0, last_ = 0;
};

// Row-major MCU rectangles cover every canonical pixel once without allocating
// a per-pixel bitmap, including the short right and bottom edges.
class Coverage {
 public:
  bool accept(unsigned left, unsigned right, unsigned top, unsigned bottom) {
    if (left > right || top > bottom || right >= 466 || bottom >= 466 || left != x_ || top != y_) return false;
    if (!x_) bottom_ = bottom;
    else if (bottom != bottom_) return false;
    x_ = right + 1U;
    if (x_ == 466) { x_ = 0; y_ = bottom + 1U; }
    return true;
  }
  bool complete() const { return x_ == 0 && y_ == 466; }
 private:
  unsigned x_ = 0, y_ = 0, bottom_ = 0;
};

inline size_t writePackedRect(uint8_t* destination, const uint8_t* source,
                              unsigned left, unsigned right, unsigned top, unsigned bottom) {
  if (left > right || top > bottom || right >= 466 || bottom >= 466) return 0;
  for (unsigned y = top; y <= bottom; ++y) {
    uint8_t* row = destination + (y * 466U + left) * 2U;
    for (unsigned x = left; x <= right; ++x) {
      const uint16_t color = ((source[0] & 0xf8U) << 8) | ((source[1] & 0xfcU) << 3) | (source[2] >> 3);
      *row++ = color & 0xffU; *row++ = color >> 8; source += 3;
    }
  }
  return (right - left + 1U) * (bottom - top + 1U);
}
// Canonical decimal file names eliminate aliases for the same stable logical ID.
inline bool slotName(const char* name, uint32_t& slot) {
  if (*name == '/') ++name;
  if (strncmp(name, "slot", 4)) return false;
  name += 4;
  const char* start = name;
  uint32_t value = 0;
  while (*name >= '0' && *name <= '9') {
    const uint32_t digit = *name++ - '0';
    if (value > (kMaxSlot - digit) / 10) return false;
    value = value * 10 + digit;
  }
  if (name == start || (name - start > 1 && *start == '0') || strcmp(name, ".jpg")) return false;
  slot = value;
  return true;
}
}
