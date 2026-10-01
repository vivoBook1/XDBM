#include "extendible_hash_table.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "bucket_page.h"
#include "hash.h"

namespace {

// Pages a split pins at once: the old bucket, the new bucket and one
// directory page.
constexpr size_t kMinCachePages = 4;

}  // namespace

ExtendibleHashTable::ExtendibleHashTable(const std::string& path)
    : ExtendibleHashTable(path, Options{}) {}

ExtendibleHashTable::ExtendibleHashTable(const std::string& path, Options options)
    : disk_(path),
      pool_(disk_, std::max(options.cache_pages, kMinCachePages)),
      max_global_depth_(std::min(options.max_global_depth,
                                 directory_page::max_global_depth())) {
    if (disk_.is_new()) {
        create_empty();
    } else {
        load_directory();
    }
}

void ExtendibleHashTable::create_empty() {
    PageHandle bucket = pool_.new_page();
    BucketPage(bucket.mutable_page()).init(0);

    PageHandle dir = pool_.new_page();
    directory_page::init(dir.mutable_page());

    FileHeader& header = disk_.header();
    header.global_depth = 0;
    header.record_count = 0;
    header.directory_pages = {dir.id()};

    directory_ = {bucket.id()};
    dir.release();
    bucket.release();
    store_directory(0, 1);
    flush();  // leave a valid, empty database on disk
}

void ExtendibleHashTable::load_directory() {
    const FileHeader& header = disk_.header();
    if (header.global_depth > max_global_depth_) {
        throw std::runtime_error("database global depth exceeds max_global_depth");
    }

    size_t entries = size_t{1} << header.global_depth;
    if (header.directory_pages.size() != directory_page::pages_for(entries)) {
        throw std::runtime_error("corrupt database: wrong directory page count");
    }

    directory_.resize(entries);
    size_t i = 0;
    while (i < entries) {
        size_t page_index = i / directory_page::kEntriesPerPage;
        PageHandle dir = pool_.fetch_page(header.directory_pages[page_index]);
        if (directory_page::type(dir.page()) != PageType::Directory) {
            throw std::runtime_error("corrupt database: bad directory page");
        }
        size_t page_end = std::min(entries, (page_index + 1) * directory_page::kEntriesPerPage);
        for (; i < page_end; ++i) {
            PageId bucket = directory_page::entry(dir.page(), i % directory_page::kEntriesPerPage);
            if (bucket == kInvalidPageId || bucket >= disk_.page_count()) {
                throw std::runtime_error("corrupt database: bad directory entry");
            }
            directory_[i] = bucket;
        }
    }
}

void ExtendibleHashTable::put(std::string_view key, std::string_view value) {
    if (key.size() + value.size() > kMaxRecordSize) {
        throw std::length_error("record too large: " +
                                std::to_string(key.size() + value.size()) + " > " +
                                std::to_string(kMaxRecordSize) + " bytes");
    }

    uint64_t hash = fnv1a_hash(key);
    // An overwrite is a remove plus an insert, since the new value may be a
    // different size or no longer fit in the same bucket.
    remove_record(key, hash);

    while (true) {
        size_t slot = slot_for(hash);
        PageHandle bucket = pool_.fetch_page(directory_[slot]);
        BucketPage page(bucket.mutable_page());

        if (page.insert(hash, key, value)) break;

        // A bucket that already has overflow pages is left as a chain: its
        // records wouldn't fit in the two pages a split produces.
        if (page.local_depth() < max_global_depth_ &&
            page.overflow_page() == kInvalidPageId) {
            bucket.release();
            split(slot);
            continue;  // the key's bucket may have changed; try again
        }

        append_to_overflow_chain(std::move(bucket), hash, key, value);
        break;
    }
    ++disk_.header().record_count;
}

std::optional<std::string> ExtendibleHashTable::get(std::string_view key) {
    uint64_t hash = fnv1a_hash(key);
    PageId id = directory_[slot_for(hash)];
    while (id != kInvalidPageId) {
        PageHandle bucket = pool_.fetch_page(id);
        BucketPageView page(bucket.page());
        if (std::optional<std::string> value = page.find(key, hash)) return value;
        id = page.overflow_page();
    }
    return std::nullopt;
}

bool ExtendibleHashTable::remove(std::string_view key) {
    return remove_record(key, fnv1a_hash(key));
}

bool ExtendibleHashTable::remove_record(std::string_view key, uint64_t hash) {
    PageId id = directory_[slot_for(hash)];
    while (id != kInvalidPageId) {
        PageHandle bucket = pool_.fetch_page(id);
        BucketPageView view(bucket.page());
        if (view.find(key, hash)) {
            BucketPage(bucket.mutable_page()).remove(key, hash);
            --disk_.header().record_count;
            return true;
        }
        id = view.overflow_page();
    }
    return false;
}

void ExtendibleHashTable::split(size_t slot) {
    PageId old_id = directory_[slot];
    PageHandle old_bucket = pool_.fetch_page(old_id);
    uint8_t depth = BucketPageView(old_bucket.page()).local_depth();

    if (depth == global_depth()) double_directory();

    // The bit that now tells the two halves apart.
    uint64_t split_bit = uint64_t{1} << depth;
    auto new_depth = static_cast<uint8_t>(depth + 1);

    // put() never splits a bucket with overflow pages, so the old bucket is a
    // single page. Rebuild both halves from a
    // copy of it; everything fits because it all fit in one page before.
    Page copy = old_bucket.page();
    PageHandle new_bucket = pool_.new_page();
    BucketPage old_page(old_bucket.mutable_page());
    BucketPage new_page(new_bucket.mutable_page());
    old_page.init(new_depth);
    new_page.init(new_depth);
    for (const BucketPageView::Record& r : BucketPageView(copy).records()) {
        BucketPage& target = (r.hash & split_bit) ? new_page : old_page;
        target.insert(r.hash, r.key, r.value);
    }

    // The entries pointing at the old bucket are those whose low `depth` bits
    // match `slot`'s. Repoint the ones that also have split_bit set.
    PageId new_id = new_bucket.id();
    old_bucket.release();
    new_bucket.release();
    size_t first = (slot & (split_bit - 1)) | split_bit;
    for (size_t i = first; i < directory_.size(); i += split_bit << 1) {
        directory_[i] = new_id;
        store_directory(i, i + 1);
    }
}

void ExtendibleHashTable::double_directory() {
    size_t old_size = directory_.size();
    size_t new_size = old_size * 2;

    // The new upper half mirrors the lower half.
    directory_.resize(new_size);
    for (size_t i = 0; i < old_size; ++i) directory_[old_size + i] = directory_[i];

    FileHeader& header = disk_.header();
    while (header.directory_pages.size() < directory_page::pages_for(new_size)) {
        PageHandle dir = pool_.new_page();
        directory_page::init(dir.mutable_page());
        header.directory_pages.push_back(dir.id());
    }
    ++header.global_depth;

    store_directory(old_size, new_size);
}

void ExtendibleHashTable::append_to_overflow_chain(PageHandle bucket, uint64_t hash,
                                                   std::string_view key,
                                                   std::string_view value) {
    // `bucket` is the full primary page. Try each overflow page in turn.
    while (true) {
        PageId next = BucketPageView(bucket.page()).overflow_page();
        if (next == kInvalidPageId) break;
        PageHandle overflow = pool_.fetch_page(next);
        if (BucketPage(overflow.mutable_page()).insert(hash, key, value)) return;
        bucket = std::move(overflow);
    }

    // All full: link a new page onto the end of the chain.
    PageHandle overflow = pool_.new_page();
    BucketPage page(overflow.mutable_page());
    page.init(BucketPageView(bucket.page()).local_depth());
    page.insert(hash, key, value);  // fits: put() checked the size
    BucketPage(bucket.mutable_page()).set_overflow_page(overflow.id());
}

std::vector<ExtendibleHashTable::BucketInfo> ExtendibleHashTable::buckets() {
    std::vector<BucketInfo> result;
    std::unordered_map<PageId, size_t> index_of;  // primary page -> result index

    for (size_t entry = 0; entry < directory_.size(); ++entry) {
        auto [it, inserted] = index_of.try_emplace(directory_[entry], result.size());
        if (!inserted) {
            result[it->second].directory_entries.push_back(entry);
            continue;
        }

        BucketInfo info;
        info.directory_entries.push_back(entry);
        PageId id = directory_[entry];
        while (id != kInvalidPageId) {
            PageHandle page = pool_.fetch_page(id);
            BucketPageView view(page.page());
            if (info.chain.empty()) info.local_depth = view.local_depth();
            info.chain.push_back({id, view.records().size()});
            id = view.overflow_page();
        }
        result.push_back(std::move(info));
    }
    return result;
}

void ExtendibleHashTable::for_each(
    const std::function<void(std::string_view key, std::string_view value)>& fn) {
    // Several directory entries can share a bucket; visit each bucket once.
    std::unordered_set<PageId> visited;
    for (PageId primary : directory_) {
        if (!visited.insert(primary).second) continue;
        PageId id = primary;
        while (id != kInvalidPageId) {
            PageHandle page = pool_.fetch_page(id);
            BucketPageView view(page.page());
            for (const BucketPageView::Record& r : view.records()) fn(r.key, r.value);
            id = view.overflow_page();
        }
    }
}

std::optional<std::string> ExtendibleHashTable::firstkey() {
    return first_key_from_entry(0);
}

std::optional<std::string> ExtendibleHashTable::nextkey(std::string_view key) {
    uint64_t hash = fnv1a_hash(key);
    size_t entry = slot_for(hash);

    // Find the key in its bucket's chain, then continue right after it.
    PageId id = directory_[entry];
    while (id != kInvalidPageId) {
        PageHandle page = pool_.fetch_page(id);
        BucketPageView view(page.page());
        std::vector<BucketPageView::Record> records = view.records();
        for (size_t i = 0; i < records.size(); ++i) {
            if (records[i].hash != hash || records[i].key != key) continue;

            // Later in this page, then later in the chain...
            if (i + 1 < records.size()) return std::string(records[i + 1].key);
            PageId next_page = view.overflow_page();
            uint8_t local_depth = view.local_depth();
            page.release();
            if (auto next = first_key_in_chain(next_page)) return next;

            // ...then the next bucket. This one is visited at its lowest
            // entry: the key's entry with only the low local_depth bits kept.
            size_t first_entry = entry & ((size_t{1} << local_depth) - 1);
            return first_key_from_entry(first_entry + 1);
        }
        id = view.overflow_page();
    }
    throw std::out_of_range("nextkey: key not found: " + std::string(key));
}

bool ExtendibleHashTable::is_first_entry(size_t entry) const {
    if (entry == 0) return true;
    // Clearing entry's highest set bit gives a lower entry that agrees on
    // every bit below it. If the bucket's local depth doesn't reach that
    // bit, the lower entry points at the same bucket, so this one isn't
    // first. If it does reach it, the lower entry is a different bucket.
    size_t high_bit = 1;
    while (high_bit <= (entry >> 1)) high_bit <<= 1;
    return directory_[entry & ~high_bit] != directory_[entry];
}

std::optional<std::string> ExtendibleHashTable::first_key_in_chain(PageId id) {
    while (id != kInvalidPageId) {
        PageHandle page = pool_.fetch_page(id);
        BucketPageView view(page.page());
        std::vector<BucketPageView::Record> records = view.records();
        if (!records.empty()) return std::string(records.front().key);
        id = view.overflow_page();
    }
    return std::nullopt;
}

std::optional<std::string> ExtendibleHashTable::first_key_from_entry(size_t entry) {
    for (; entry < directory_.size(); ++entry) {
        if (!is_first_entry(entry)) continue;
        if (auto key = first_key_in_chain(directory_[entry])) return key;
    }
    return std::nullopt;
}

ExtendibleHashTable::CompactStats ExtendibleHashTable::compact() {
    CompactStats stats;
    for (size_t entry = 0; entry < directory_.size(); ++entry) {
        if (is_first_entry(entry)) compact_bucket(directory_[entry], stats);
    }
    return stats;
}

void ExtendibleHashTable::compact_bucket(PageId primary, CompactStats& stats) {
    struct OwnedRecord {
        uint64_t hash;
        std::string key;
        std::string value;
    };

    // Read the whole chain: its page ids and its live records, in order.
    std::vector<PageId> chain;
    std::vector<OwnedRecord> records;
    uint8_t local_depth = 0;
    size_t dead_slots = 0;
    for (PageId id = primary; id != kInvalidPageId;) {
        PageHandle page = pool_.fetch_page(id);
        BucketPageView view(page.page());
        if (chain.empty()) local_depth = view.local_depth();
        chain.push_back(id);
        std::vector<BucketPageView::Record> live = view.records();
        dead_slots += view.slot_count() - live.size();
        for (const BucketPageView::Record& r : live) {
            records.push_back({r.hash, std::string(r.key), std::string(r.value)});
        }
        id = view.overflow_page();
    }

    // Pack them into fresh pages in memory, filling each before starting
    // the next. Records keep their order, so iteration order is unchanged.
    std::vector<Page> packed(1);
    BucketPage(packed.back()).init(local_depth);
    for (const OwnedRecord& r : records) {
        if (BucketPage(packed.back()).insert(r.hash, r.key, r.value)) continue;
        packed.emplace_back();
        BucketPage page(packed.back());
        page.init(local_depth);
        page.insert(r.hash, r.key, r.value);  // fits: put() checked the size
    }

    if (dead_slots == 0 && packed.size() == chain.size()) return;  // already compact
    // Each page of the old chain held a run of these records in this order,
    // and filling pages greedily never uses more pages than that.
    if (packed.size() > chain.size()) {
        throw std::logic_error("compaction produced a longer chain");
    }

    // Overwrite the start of the chain, then free the pages left over.
    for (size_t i = 0; i < packed.size(); ++i) {
        BucketPage(packed[i]).set_overflow_page(i + 1 < packed.size() ? chain[i + 1]
                                                                      : kInvalidPageId);
        PageHandle page = pool_.fetch_page(chain[i]);
        page.mutable_page() = packed[i];
    }
    for (size_t i = packed.size(); i < chain.size(); ++i) pool_.free_page(chain[i]);

    ++stats.buckets_rewritten;
    stats.deleted_records_removed += dead_slots;
    stats.overflow_pages_freed += chain.size() - packed.size();
}

void ExtendibleHashTable::store_directory(size_t begin, size_t end) {
    const std::vector<PageId>& pages = disk_.header().directory_pages;
    size_t i = begin;
    while (i < end) {
        size_t page_index = i / directory_page::kEntriesPerPage;
        PageHandle dir = pool_.fetch_page(pages[page_index]);
        Page& page = dir.mutable_page();
        size_t page_end = std::min(end, (page_index + 1) * directory_page::kEntriesPerPage);
        for (; i < page_end; ++i) {
            directory_page::set_entry(page, i % directory_page::kEntriesPerPage, directory_[i]);
        }
    }
}
