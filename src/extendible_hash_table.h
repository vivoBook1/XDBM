#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "bucket_page.h"
#include "buffer_pool.h"
#include "directory_page.h"
#include "disk_manager.h"

// Disk-backed extendible hash table mapping string keys to string values.
//
// Each bucket is one page. The directory's 2^global_depth entries are stored
// in directory pages and also kept in memory, so a lookup costs one bucket
// page fetch. When a bucket fills up it splits, doubling the directory first
// if its local depth equals the global depth. Once a bucket reaches
// max_global_depth it can't split, so extra records go to a chain of
// overflow pages instead.
//
// Changes are cached in the buffer pool and reach the file on flush() or
// destruction. A crash before then can leave the file inconsistent.
class ExtendibleHashTable {
public:
    struct Options {
        size_t cache_pages = 64;  // buffer pool frames; at least 4
        // Lower it to force overflow chains in tests. A file must always be
        // reopened with at least the depth it reached.
        uint32_t max_global_depth = directory_page::max_global_depth();
    };

    static constexpr size_t kMaxRecordSize = BucketPageView::kMaxPayload;

    // One page in a bucket's chain.
    struct ChainPage {
        PageId page;
        size_t records;  // live records in this page
    };

    // Layout of one bucket, for inspection and debugging.
    struct BucketInfo {
        uint8_t local_depth = 0;
        std::vector<size_t> directory_entries;  // entries pointing at it
        std::vector<ChainPage> chain;           // primary page first
    };

    // Opens the database at `path`, creating an empty one if needed.
    explicit ExtendibleHashTable(const std::string& path);
    ExtendibleHashTable(const std::string& path, Options options);

    // Inserts the key, or overwrites its value if it already exists. Throws
    // std::length_error if key.size() + value.size() > kMaxRecordSize.
    void put(std::string_view key, std::string_view value);

    std::optional<std::string> get(std::string_view key);
    bool contains(std::string_view key) { return get(key).has_value(); }

    // Returns true if the key was present and removed. Buckets aren't merged
    // and the directory never shrinks.
    bool remove(std::string_view key);

    // Writes all cached changes to the file and fsyncs.
    void flush() { pool_.flush_all(); }

    size_t size() const { return disk_.header().record_count; }
    uint32_t global_depth() const { return disk_.header().global_depth; }
    size_t directory_size() const { return directory_.size(); }
    uint32_t file_pages() const { return disk_.page_count(); }
    const BufferPool& buffer_pool() const { return pool_; }

    // Every distinct bucket, in order of the first directory entry that
    // points at it. Reads every bucket page.
    std::vector<BucketInfo> buckets();

    // Calls fn(key, value) once for every record, in storage order (grouped
    // by bucket, not sorted). The views are only valid during the call, and
    // fn must not modify the table.
    void for_each(const std::function<void(std::string_view key, std::string_view value)>& fn);

    // dbm-style key iteration, in the same order as for_each():
    //
    //   for (auto k = table.firstkey(); k; k = table.nextkey(*k)) { ... }
    //
    // nextkey() finds its place from the key itself, so no cursor state is
    // kept between calls. It returns std::nullopt after the last key, and
    // throws std::out_of_range if `key` isn't in the table. Inserting or
    // removing keys mid-iteration can make it skip or repeat keys, and
    // removing the current key makes the next nextkey() call throw.
    std::optional<std::string> firstkey();
    std::optional<std::string> nextkey(std::string_view key);

    struct CompactStats {
        size_t buckets_rewritten = 0;
        size_t deleted_records_removed = 0;  // dead slots dropped
        size_t overflow_pages_freed = 0;
    };

    // Rewrites every bucket that has deleted records or a loosely packed
    // overflow chain: live records are packed in order into as few pages as
    // possible and the leftover overflow pages go on the free list for later
    // inserts. The file doesn't shrink, and buckets aren't merged.
    CompactStats compact();

private:
    void create_empty();
    void load_directory();

    size_t slot_for(uint64_t hash) const {
        return static_cast<size_t>(hash & (directory_.size() - 1));
    }

    bool remove_record(std::string_view key, uint64_t hash);

    // True if `entry` is the lowest directory entry pointing at its bucket,
    // which is where iteration visits that bucket.
    bool is_first_entry(size_t entry) const;

    // First key in the chain starting at page `id`, if any.
    std::optional<std::string> first_key_in_chain(PageId id);

    // First key in the first non-empty bucket whose first entry is >= entry.
    std::optional<std::string> first_key_from_entry(size_t entry);

    void compact_bucket(PageId primary, CompactStats& stats);
    void split(size_t slot);
    void double_directory();
    void append_to_overflow_chain(PageHandle bucket, uint64_t hash,
                                  std::string_view key, std::string_view value);

    // Copies directory_[begin, end) into the directory pages.
    void store_directory(size_t begin, size_t end);

    // Declared in this order so the pool is flushed and destroyed before the
    // disk manager it writes through.
    DiskManager disk_;
    BufferPool pool_;
    uint32_t max_global_depth_;
    std::vector<PageId> directory_;
};
