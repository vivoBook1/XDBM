#pragma once

#include <string>
#include <vector>

#include "page.h"

// Hash-table metadata stored in the header page. DiskManager persists these
// fields but doesn't interpret them.
struct FileHeader {
    uint32_t global_depth = 0;
    uint64_t record_count = 0;
    std::vector<PageId> directory_pages;
};

// Owns the database file. Reads and writes whole pages by id, hands out new
// pages (reusing freed ones first) and maintains the header page (page 0).
//
// Header page layout (byte offsets):
//   0  magic "EXTHASH1"      8
//   8  format version        u32
//   12 page size             u32
//   16 hash function id      u32
//   20 page count            u32
//   24 free list head        u32
//   28 global depth          u32
//   32 record count          u64
//   40 directory page count  u32
//   44 directory page ids    u32 each
//
// A freed page holds PageType::Free in byte 0 and the next free page's id at
// byte 4, forming a linked list that starts at the header's free list head.
class DiskManager {
public:
    static constexpr size_t kMaxDirectoryPages = (kPageSize - 44) / 4;

    // Opens the database at `path`, creating an empty one if the file doesn't
    // exist. Throws std::runtime_error if the file isn't a valid database.
    explicit DiskManager(const std::string& path);

    // Flushes the header and closes the file.
    ~DiskManager();

    DiskManager(const DiskManager&) = delete;
    DiskManager& operator=(const DiskManager&) = delete;

    // Page 0 is reserved for the header; both throw std::out_of_range for it
    // or for ids past the end of the file.
    void read_page(PageId id, Page& page) const;
    void write_page(PageId id, const Page& page);

    // Returns the id of a zero-filled page, reusing a freed page if any.
    PageId allocate_page();

    // Puts a page on the free list for allocate_page() to reuse.
    void free_page(PageId id);

    FileHeader& header() { return header_; }
    const FileHeader& header() const { return header_; }

    // Writes the header page and fsyncs the file.
    void flush();

    uint32_t page_count() const { return page_count_; }

    // True if this DiskManager created the file, so the caller should set up
    // an empty hash table in it.
    bool is_new() const { return created_; }

private:
    void read_header(uint64_t file_size);
    void write_header();
    void check_page_id(PageId id) const;

    int fd_ = -1;
    std::string path_;
    bool created_ = false;
    uint32_t page_count_ = 0;
    PageId free_list_head_ = kInvalidPageId;
    FileHeader header_;
};
