#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// The database file is an array of fixed-size pages; page N starts at byte
// N * kPageSize. Pages refer to each other by PageId, never by pointer.
using PageId = uint32_t;

constexpr size_t kPageSize = 4096;

// Page 0 is always the file header, so 0 doubles as "no page".
constexpr PageId kInvalidPageId = 0;

using Page = std::array<uint8_t, kPageSize>;

// First byte of every non-header page.
enum class PageType : uint8_t {
    Unused = 0,  // freshly allocated, not yet formatted
    Bucket = 1,
    Free = 2,    // on the free list, waiting to be reused
    Directory = 3,
};

// All on-disk integers are little-endian, written byte by byte, so a file
// is readable on any machine regardless of its native byte order.

inline void put_u16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

inline void put_u32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

inline void put_u64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

inline uint16_t get_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

inline uint32_t get_u32(const uint8_t* p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (8 * i);
    return v;
}

inline uint64_t get_u64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}
