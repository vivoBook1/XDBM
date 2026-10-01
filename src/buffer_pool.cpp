#include "buffer_pool.h"

#include <stdexcept>
#include <string>
#include <utility>

PageHandle::PageHandle(PageHandle&& other) noexcept
    : pool_(std::exchange(other.pool_, nullptr)), frame_(other.frame_) {}

PageHandle& PageHandle::operator=(PageHandle&& other) noexcept {
    if (this != &other) {
        release();
        pool_ = std::exchange(other.pool_, nullptr);
        frame_ = other.frame_;
    }
    return *this;
}

PageId PageHandle::id() const { return pool_->frames_[frame_].id; }

const Page& PageHandle::page() const { return pool_->frames_[frame_].page; }

Page& PageHandle::mutable_page() {
    BufferPool::Frame& frame = pool_->frames_[frame_];
    frame.dirty = true;
    return frame.page;
}

void PageHandle::release() {
    if (pool_) {
        pool_->unpin(frame_);
        pool_ = nullptr;
    }
}

BufferPool::BufferPool(DiskManager& disk, size_t capacity)
    : disk_(disk), frames_(capacity) {
    if (capacity == 0) throw std::invalid_argument("buffer pool capacity must be > 0");
    page_table_.reserve(capacity);
    free_frames_.reserve(capacity);
    for (size_t i = capacity; i > 0; --i) free_frames_.push_back(i - 1);
}

BufferPool::~BufferPool() {
    try {
        flush_all();
    } catch (...) {
        // Destructors must not throw.
    }
}

PageHandle BufferPool::fetch_page(PageId id) {
    auto it = page_table_.find(id);
    if (it != page_table_.end()) {
        ++hits_;
        pin(it->second);
        return PageHandle(this, it->second);
    }

    ++misses_;
    size_t frame = acquire_frame();
    try {
        disk_.read_page(id, frames_[frame].page);
    } catch (...) {
        free_frames_.push_back(frame);
        throw;
    }
    return install(frame, id);
}

PageHandle BufferPool::new_page() {
    // Get the frame first so a full pool doesn't leak a newly allocated page.
    size_t frame = acquire_frame();
    PageId id;
    try {
        id = disk_.allocate_page();
    } catch (...) {
        free_frames_.push_back(frame);
        throw;
    }
    // allocate_page() already zeroed it on disk, so it starts clean.
    frames_[frame].page.fill(0);
    return install(frame, id);
}

void BufferPool::free_page(PageId id) {
    auto it = page_table_.find(id);
    if (it != page_table_.end() && frames_[it->second].pin_count > 0) {
        throw std::logic_error("cannot free pinned page " + std::to_string(id));
    }

    // Free on disk first: if that throws, the cache is left untouched.
    disk_.free_page(id);

    if (it != page_table_.end()) {
        size_t frame = it->second;
        lru_.erase(frames_[frame].lru_pos);
        page_table_.erase(it);
        frames_[frame].id = kInvalidPageId;
        frames_[frame].dirty = false;
        free_frames_.push_back(frame);
    }
}

void BufferPool::flush_page(PageId id) {
    auto it = page_table_.find(id);
    if (it != page_table_.end()) write_back(frames_[it->second]);
}

void BufferPool::flush_all() {
    for (Frame& frame : frames_) {
        if (frame.id != kInvalidPageId) write_back(frame);
    }
    disk_.flush();
}

size_t BufferPool::acquire_frame() {
    if (!free_frames_.empty()) {
        size_t frame = free_frames_.back();
        free_frames_.pop_back();
        return frame;
    }
    if (lru_.empty()) {
        throw std::runtime_error("buffer pool: all " + std::to_string(capacity()) +
                                 " frames are pinned");
    }

    size_t frame = lru_.front();
    Frame& victim = frames_[frame];
    // Write back before touching any bookkeeping, so a failed write leaves
    // the victim cached and still dirty.
    write_back(victim);
    lru_.pop_front();
    page_table_.erase(victim.id);
    victim.id = kInvalidPageId;
    return frame;
}

PageHandle BufferPool::install(size_t frame, PageId id) {
    Frame& f = frames_[frame];
    f.id = id;
    f.pin_count = 1;
    f.dirty = false;
    page_table_[id] = frame;
    return PageHandle(this, frame);
}

void BufferPool::pin(size_t frame) {
    Frame& f = frames_[frame];
    if (f.pin_count++ == 0) lru_.erase(f.lru_pos);
}

void BufferPool::unpin(size_t frame) {
    Frame& f = frames_[frame];
    if (--f.pin_count == 0) f.lru_pos = lru_.insert(lru_.end(), frame);
}

void BufferPool::write_back(Frame& frame) {
    if (frame.dirty) {
        disk_.write_page(frame.id, frame.page);
        frame.dirty = false;
    }
}
