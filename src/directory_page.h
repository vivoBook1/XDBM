#pragma once

#include <cstddef>

#include "disk_manager.h"
#include "page.h"

// A directory page holds a run of consecutive directory entries, each the
// page id of the bucket that entry points to:
//
//   type u8 | reserved 3 bytes | entry 0 u32 | entry 1 u32 | ... | entry 1022 u32
//
// The type byte keeps a directory page distinguishable from bucket and free
// pages. Directory entry i lives in directory page i / kEntriesPerPage, at
// position i % kEntriesPerPage; the header page lists the directory pages
// in order.
namespace directory_page {

constexpr size_t kHeaderSize = 4;
constexpr size_t kEntriesPerPage = (kPageSize - kHeaderSize) / sizeof(PageId);

// Directory entries the file header can reach.
constexpr size_t kMaxEntries = DiskManager::kMaxDirectoryPages * kEntriesPerPage;

// Largest global depth whose 2^depth entries fit in kMaxEntries.
constexpr uint32_t max_global_depth() {
    uint32_t depth = 0;
    while ((size_t{2} << depth) <= kMaxEntries) ++depth;
    return depth;
}

constexpr size_t pages_for(size_t entries) {
    return (entries + kEntriesPerPage - 1) / kEntriesPerPage;
}

inline void init(Page& page) {
    page.fill(0);
    page[0] = static_cast<uint8_t>(PageType::Directory);
}

inline PageType type(const Page& page) { return static_cast<PageType>(page[0]); }

inline PageId entry(const Page& page, size_t index) {
    return get_u32(page.data() + kHeaderSize + index * sizeof(PageId));
}

inline void set_entry(Page& page, size_t index, PageId bucket) {
    put_u32(page.data() + kHeaderSize + index * sizeof(PageId), bucket);
}

}  // namespace directory_page
