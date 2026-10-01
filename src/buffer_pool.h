#pragma once

#include <cstddef>
#include <list>
#include <unordered_map>
#include <vector>

#include "disk_manager.h"
#include "page.h"

class BufferPool;

// Pins one cached page. While a handle exists its page stays in memory and
// can't be evicted; destroying the handle (or calling release()) unpins it.
// Handles are move-only. Only use page()/mutable_page()/id() on a valid handle.
class PageHandle {
public:
    PageHandle() = default;
    PageHandle(PageHandle&& other) noexcept;
    PageHandle& operator=(PageHandle&& other) noexcept;
    ~PageHandle() { release(); }

    PageHandle(const PageHandle&) = delete;
    PageHandle& operator=(const PageHandle&) = delete;

    PageId id() const;

    // Read-only access. Doesn't mark the page dirty.
    const Page& page() const;

    // Write access. Marks the page dirty so it's written back to disk before
    // being evicted.
    Page& mutable_page();

    bool valid() const { return pool_ != nullptr; }

    // Unpins the page early. The handle becomes invalid.
    void release();

private:
    friend class BufferPool;
    PageHandle(BufferPool* pool, size_t frame) : pool_(pool), frame_(frame) {}

    BufferPool* pool_ = nullptr;
    size_t frame_ = 0;
};

// Fixed-size page cache between the hash table and the DiskManager.
//
// Pages are read from disk on first use into one of `capacity` frames and
// stay there across uses. Changes are made in memory and only written to
// disk when a dirty page is evicted or flushed. When all frames are in use,
// the least recently used unpinned page is evicted.
//
// Destroy every PageHandle before the BufferPool, and the BufferPool before
// its DiskManager.
class BufferPool {
public:
    BufferPool(DiskManager& disk, size_t capacity);

    // Calls flush_all(), ignoring errors; call flush_all() yourself to see them.
    ~BufferPool();

    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    // Pins a page, reading it from disk if it isn't cached. Throws
    // std::runtime_error if every frame is pinned.
    PageHandle fetch_page(PageId id);

    // Allocates a zero-filled page on disk and pins it. Throws
    // std::runtime_error if every frame is pinned.
    PageHandle new_page();

    // Drops a page from the cache and returns it to the disk's free list.
    // Throws std::logic_error if the page is pinned.
    void free_page(PageId id);

    // Writes the page to disk if it's cached and dirty.
    void flush_page(PageId id);

    // Writes every dirty page to disk, then flushes the file header and
    // fsyncs.
    void flush_all();

    size_t capacity() const { return frames_.size(); }

    // Fetches served from memory vs. read from disk.
    size_t hits() const { return hits_; }
    size_t misses() const { return misses_; }

private:
    friend class PageHandle;

    struct Frame {
        Page page;
        PageId id = kInvalidPageId;  // kInvalidPageId while the frame is empty
        size_t pin_count = 0;
        bool dirty = false;
        std::list<size_t>::iterator lru_pos;  // valid while in lru_
    };

    // Returns an empty frame, evicting the least recently used page if needed.
    size_t acquire_frame();

    // Loads `id` into an empty frame with a pin count of 1.
    PageHandle install(size_t frame, PageId id);

    void pin(size_t frame);
    void unpin(size_t frame);
    void write_back(Frame& frame);

    DiskManager& disk_;
    std::vector<Frame> frames_;
    std::unordered_map<PageId, size_t> page_table_;  // cached page -> frame
    std::vector<size_t> free_frames_;
    // Frames holding an unpinned page, least recently used at the front.
    // A frame is in here exactly when it holds a page with pin_count == 0.
    std::list<size_t> lru_;
    size_t hits_ = 0;
    size_t misses_ = 0;
};
