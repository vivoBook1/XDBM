#pragma once

#include <cstdint>
#include <string_view>

// Identifies the hash function in the database file header. Keys are placed
// on disk by their hash bits, so a file must always be read back with the
// same function it was written with. Bump this if the function ever changes.
constexpr uint32_t kHashFunctionId = 1;  // FNV-1a 64-bit

// FNV-1a 64-bit hash: simple, fast, and well-distributed for string keys.
inline uint64_t fnv1a_hash(std::string_view key) {
    uint64_t hash = 14695981039346656037ULL;  // FNV offset basis
    for (unsigned char c : key) {
        hash ^= c;
        hash *= 1099511628211ULL;  // FNV prime
    }
    return hash;
}
