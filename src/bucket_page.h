#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "page.h"

// Slotted-page layout for one bucket. These classes don't own the bytes; they
// read and edit the Page they wrap in place.
//
// The slot array grows forward after the header and the record bytes grow
// backward from the end of the page, with free space in between:
//
//   [header][slot 0][slot 1]...   free   ...[record 1][record 0]
//
// Header (12 bytes): type u8 | local depth u8 | slot count u16 |
//                    free space end u16 | overflow page u32 | reserved u16
// Slot (8 bytes):    record offset u16 | key len u16 | value len u16 | flags u16
// Record:            hash u64 | key bytes | value bytes
//
// remove() only marks a slot deleted. The space is reclaimed by compact(),
// which insert() runs automatically when the free gap is too small.

// Read-only view, for use with PageHandle::page() so lookups don't mark the
// page dirty.
class BucketPageView {
public:
    struct Record {
        uint64_t hash;
        std::string_view key;    // points into the page
        std::string_view value;  // points into the page
    };

    static constexpr size_t kHeaderSize = 12;
    static constexpr size_t kSlotSize = 8;
    static constexpr size_t kRecordHeaderSize = 8;  // the stored hash

    // Largest key + value that fits in an otherwise empty page.
    static constexpr size_t kMaxPayload =
        kPageSize - kHeaderSize - kSlotSize - kRecordHeaderSize;

    explicit BucketPageView(const Page& page) : page_(page) {}

    PageType type() const { return static_cast<PageType>(page_[0]); }
    uint8_t local_depth() const { return page_[1]; }

    // Next page in this bucket's overflow chain, or kInvalidPageId.
    PageId overflow_page() const;

    // Number of slots, including deleted ones.
    uint16_t slot_count() const;

    std::optional<std::string> find(std::string_view key, uint64_t hash) const;

    // Live records in slot order. The views are valid until the page changes.
    std::vector<Record> records() const;

    // Contiguous bytes between the slot array and the record area.
    size_t free_space() const;

protected:
    struct Slot {
        uint16_t offset;
        uint16_t key_len;
        uint16_t value_len;
        uint16_t flags;
    };

    static constexpr uint16_t kDeletedFlag = 1;

    Slot slot(uint16_t index) const;
    uint16_t free_space_end() const;
    std::optional<uint16_t> find_slot(std::string_view key, uint64_t hash) const;
    Record record_at(const Slot& slot) const;

    // Free space if the page were compacted now.
    size_t reclaimable_space() const;

    const Page& page_;
};

// Read-write access, for use with PageHandle::mutable_page().
class BucketPage : public BucketPageView {
public:
    explicit BucketPage(Page& page) : BucketPageView(page), mutable_page_(page) {}

    // Formats the page as an empty bucket.
    void init(uint8_t local_depth);

    void set_local_depth(uint8_t depth) { mutable_page_[1] = depth; }
    void set_overflow_page(PageId id);

    // Adds a record. The caller must ensure the key isn't already present.
    // Returns false if it doesn't fit, even after compaction.
    bool insert(uint64_t hash, std::string_view key, std::string_view value);

    // Returns true if the key was present and removed.
    bool remove(std::string_view key, uint64_t hash);

    // Rewrites the page without deleted records and their slots.
    void compact();

private:
    void set_slot(uint16_t index, const Slot& slot);
    void set_slot_count(uint16_t count);
    void set_free_space_end(uint16_t offset);

    Page& mutable_page_;
};
