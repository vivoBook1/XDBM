#include "bucket_page.h"

#include <algorithm>

namespace {

// Header field offsets; see the layout in bucket_page.h.
constexpr size_t kTypeOffset = 0;
constexpr size_t kLocalDepthOffset = 1;
constexpr size_t kSlotCountOffset = 2;
constexpr size_t kFreeSpaceEndOffset = 4;
constexpr size_t kOverflowOffset = 6;

size_t record_size(size_t key_len, size_t value_len) {
    return BucketPageView::kRecordHeaderSize + key_len + value_len;
}

}  // namespace

// ---- BucketPageView ----

PageId BucketPageView::overflow_page() const {
    return get_u32(page_.data() + kOverflowOffset);
}

uint16_t BucketPageView::slot_count() const {
    return get_u16(page_.data() + kSlotCountOffset);
}

std::optional<std::string> BucketPageView::find(std::string_view key, uint64_t hash) const {
    std::optional<uint16_t> index = find_slot(key, hash);
    if (!index) return std::nullopt;
    return std::string(record_at(slot(*index)).value);
}

std::vector<BucketPageView::Record> BucketPageView::records() const {
    std::vector<Record> result;
    for (uint16_t i = 0; i < slot_count(); ++i) {
        Slot s = slot(i);
        if (!(s.flags & kDeletedFlag)) result.push_back(record_at(s));
    }
    return result;
}

size_t BucketPageView::free_space() const {
    return free_space_end() - (kHeaderSize + slot_count() * kSlotSize);
}

BucketPageView::Slot BucketPageView::slot(uint16_t index) const {
    const uint8_t* p = page_.data() + kHeaderSize + index * kSlotSize;
    return {get_u16(p), get_u16(p + 2), get_u16(p + 4), get_u16(p + 6)};
}

uint16_t BucketPageView::free_space_end() const {
    return get_u16(page_.data() + kFreeSpaceEndOffset);
}

std::optional<uint16_t> BucketPageView::find_slot(std::string_view key, uint64_t hash) const {
    for (uint16_t i = 0; i < slot_count(); ++i) {
        Slot s = slot(i);
        if ((s.flags & kDeletedFlag) || s.key_len != key.size()) continue;
        Record r = record_at(s);
        // Compare the stored hash first: it rules out almost every non-match
        // without touching the key bytes.
        if (r.hash == hash && r.key == key) return i;
    }
    return std::nullopt;
}

BucketPageView::Record BucketPageView::record_at(const Slot& s) const {
    const uint8_t* rec = page_.data() + s.offset;
    const char* key = reinterpret_cast<const char*>(rec + kRecordHeaderSize);
    return {get_u64(rec), {key, s.key_len}, {key + s.key_len, s.value_len}};
}

size_t BucketPageView::reclaimable_space() const {
    size_t used = 0;
    for (uint16_t i = 0; i < slot_count(); ++i) {
        Slot s = slot(i);
        if (!(s.flags & kDeletedFlag)) {
            used += kSlotSize + record_size(s.key_len, s.value_len);
        }
    }
    return kPageSize - kHeaderSize - used;
}

// ---- BucketPage ----

void BucketPage::init(uint8_t local_depth) {
    mutable_page_.fill(0);
    mutable_page_[kTypeOffset] = static_cast<uint8_t>(PageType::Bucket);
    mutable_page_[kLocalDepthOffset] = local_depth;
    set_slot_count(0);
    set_free_space_end(static_cast<uint16_t>(kPageSize));
    set_overflow_page(kInvalidPageId);
}

void BucketPage::set_overflow_page(PageId id) {
    put_u32(mutable_page_.data() + kOverflowOffset, id);
}

bool BucketPage::insert(uint64_t hash, std::string_view key, std::string_view value) {
    if (key.size() + value.size() > kMaxPayload) return false;

    size_t rec_size = record_size(key.size(), value.size());
    size_t needed = kSlotSize + rec_size;
    if (free_space() < needed) {
        if (reclaimable_space() < needed) return false;
        compact();
    }

    auto offset = static_cast<uint16_t>(free_space_end() - rec_size);
    uint8_t* rec = mutable_page_.data() + offset;
    put_u64(rec, hash);
    std::copy(key.begin(), key.end(), rec + kRecordHeaderSize);
    std::copy(value.begin(), value.end(), rec + kRecordHeaderSize + key.size());

    uint16_t index = slot_count();
    set_slot(index, {offset, static_cast<uint16_t>(key.size()),
                     static_cast<uint16_t>(value.size()), 0});
    set_slot_count(index + 1);
    set_free_space_end(offset);
    return true;
}

bool BucketPage::remove(std::string_view key, uint64_t hash) {
    std::optional<uint16_t> index = find_slot(key, hash);
    if (!index) return false;
    Slot s = slot(*index);
    s.flags |= kDeletedFlag;
    set_slot(*index, s);
    return true;
}

void BucketPage::compact() {
    Page copy = mutable_page_;
    BucketPageView old(copy);

    init(old.local_depth());
    set_overflow_page(old.overflow_page());
    for (const Record& r : old.records()) {
        insert(r.hash, r.key, r.value);  // always fits: it fit before
    }
}

void BucketPage::set_slot(uint16_t index, const Slot& s) {
    uint8_t* p = mutable_page_.data() + kHeaderSize + index * kSlotSize;
    put_u16(p, s.offset);
    put_u16(p + 2, s.key_len);
    put_u16(p + 4, s.value_len);
    put_u16(p + 6, s.flags);
}

void BucketPage::set_slot_count(uint16_t count) {
    put_u16(mutable_page_.data() + kSlotCountOffset, count);
}

void BucketPage::set_free_space_end(uint16_t offset) {
    put_u16(mutable_page_.data() + kFreeSpaceEndOffset, offset);
}
