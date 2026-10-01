#include <cstdio>
#include <iostream>
#include <random>
#include <set>
#include <string>
#include <unordered_map>

#include "extendible_hash_table.h"
#include "test_util.h"

static std::string key_for(int i) { return "key" + std::to_string(i); }

static std::string value_for(int i, size_t len) {
    std::string v = std::to_string(i) + ":";
    v.resize(len, static_cast<char>('a' + i % 26));
    return v;
}

// Walks the table with firstkey/nextkey and checks it yields exactly what
// for_each does, in the same order.
static std::vector<std::string> iterate_keys(ExtendibleHashTable& table) {
    std::vector<std::string> keys;
    for (auto k = table.firstkey(); k; k = table.nextkey(*k)) {
        keys.push_back(*k);
        CHECK(keys.size() <= table.size());  // no repeats or endless loop
    }
    std::vector<std::string> expected;
    table.for_each([&](std::string_view key, std::string_view) { expected.emplace_back(key); });
    CHECK(keys == expected);
    CHECK(keys.size() == table.size());
    return keys;
}

static void test_basics(const std::string& path) {
    std::remove(path.c_str());
    ExtendibleHashTable table(path);

    CHECK(table.size() == 0);
    CHECK(!table.get("missing"));

    table.put("name", "Alice");
    table.put("city", "Paris");
    table.put("empty", "");
    table.put("name", "Bob");  // overwrite
    CHECK(table.size() == 3);
    CHECK(table.get("name") == "Bob");
    CHECK(table.get("city") == "Paris");
    CHECK(table.get("empty") == "");

    CHECK(table.remove("city"));
    CHECK(!table.remove("city"));
    CHECK(!table.contains("city"));
    CHECK(table.size() == 2);

    std::string biggest(ExtendibleHashTable::kMaxRecordSize - 3, 'x');
    table.put("big", biggest);
    CHECK(table.get("big") == biggest);
    CHECK(throws([&] { table.put("big", biggest + "y"); }));
    CHECK(table.get("big") == biggest);  // a rejected put changes nothing
}

static void test_many_keys_small_cache(const std::string& path) {
    std::remove(path.c_str());
    const int n = 20000;
    {
        ExtendibleHashTable table(path, {/*cache_pages=*/8});
        for (int i = 0; i < n; ++i) table.put(key_for(i), value_for(i, 20));
        CHECK(table.size() == static_cast<size_t>(n));
        CHECK(table.global_depth() > 5);
        for (int i = 0; i < n; ++i) CHECK(table.get(key_for(i)) == value_for(i, 20));
        CHECK(table.buffer_pool().misses() > 0);  // the cache really was too small
    }

    ExtendibleHashTable table(path, {8});
    CHECK(table.size() == static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) CHECK(table.get(key_for(i)) == value_for(i, 20));
}

static void test_directory_spans_pages(const std::string& path) {
    std::remove(path.c_str());
    // More than kEntriesPerPage entries needs a second directory page.
    const uint32_t target_depth = 11;
    static_assert((1u << 11) > directory_page::kEntriesPerPage);

    int n = 0;
    uint32_t depth;
    {
        ExtendibleHashTable table(path);
        while (table.global_depth() < target_depth) {
            table.put(key_for(n), value_for(n, 400));
            ++n;
        }
        depth = table.global_depth();
    }

    ExtendibleHashTable table(path);
    CHECK(table.global_depth() == depth);
    CHECK(table.directory_size() == size_t{1} << depth);
    CHECK(table.size() == static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) CHECK(table.get(key_for(i)) == value_for(i, 400));
}

static void test_overflow_chains(const std::string& path) {
    std::remove(path.c_str());
    ExtendibleHashTable::Options options;
    options.max_global_depth = 1;  // at most 2 buckets, so chains must form

    const int n = 500;
    {
        ExtendibleHashTable table(path, options);
        for (int i = 0; i < n; ++i) table.put(key_for(i), value_for(i, 100));
        CHECK(table.global_depth() == 1);
        CHECK(table.file_pages() > 10);  // header + directory + 2 buckets + overflow
        for (int i = 0; i < n; ++i) CHECK(table.get(key_for(i)) == value_for(i, 100));

        // Two buckets (entries 0 and 1), each a multi-page chain, holding
        // every record between them.
        auto buckets = table.buckets();
        CHECK(buckets.size() == 2);
        size_t total = 0;
        for (const auto& b : buckets) {
            CHECK(b.local_depth == 1);
            CHECK(b.directory_entries.size() == 1);
            CHECK(b.chain.size() > 1);
            for (const auto& page : b.chain) total += page.records;
        }
        CHECK(total == static_cast<size_t>(n));

        for (int i = 0; i < n; i += 2) CHECK(table.remove(key_for(i)));
        table.put(key_for(1), "updated");
    }

    {
        ExtendibleHashTable table(path, options);
        CHECK(table.size() == static_cast<size_t>(n / 2));
        CHECK(table.get(key_for(1)) == "updated");
        for (int i = 3; i < n; i += 2) CHECK(table.get(key_for(i)) == value_for(i, 100));
        for (int i = 0; i < n; i += 2) CHECK(!table.contains(key_for(i)));
    }

    // Reopened with the default max depth, the chained buckets must not be
    // split (that would lose their overflow records), but inserts still work.
    ExtendibleHashTable table(path);
    for (int i = n; i < 2 * n; ++i) table.put(key_for(i), value_for(i, 100));
    CHECK(table.get(key_for(1)) == "updated");
    for (int i = 3; i < n; i += 2) CHECK(table.get(key_for(i)) == value_for(i, 100));
    for (int i = n; i < 2 * n; ++i) CHECK(table.get(key_for(i)) == value_for(i, 100));
    CHECK(table.size() == static_cast<size_t>(n / 2 + n));

    // for_each and firstkey/nextkey walk overflow pages too.
    size_t visited = 0;
    table.for_each([&](std::string_view, std::string_view) { ++visited; });
    CHECK(visited == table.size());
    iterate_keys(table);
}

static void test_firstkey_nextkey(const std::string& path) {
    std::remove(path.c_str());
    ExtendibleHashTable table(path);

    CHECK(!table.firstkey());
    CHECK(throws([&] { table.nextkey("missing"); }));

    table.put("only", "1");
    CHECK(table.firstkey() == "only");
    CHECK(!table.nextkey("only"));
    table.remove("only");

    // Stop right after the directory doubles to depth 4: only the bucket
    // that split has local depth 4, so the rest are each shared by two
    // directory entries, which the walk must visit only once.
    int n = 0;
    while (table.global_depth() < 4) {
        table.put(key_for(n), value_for(n, 20));
        ++n;
    }
    bool has_shared_bucket = false;
    for (const auto& b : table.buckets()) {
        if (b.directory_entries.size() > 1) has_shared_bucket = true;
    }
    CHECK(has_shared_bucket);

    std::vector<std::string> keys = iterate_keys(table);
    CHECK(std::set<std::string>(keys.begin(), keys.end()).size() == static_cast<size_t>(n));

    // Leave a few keys scattered among now-empty buckets.
    std::set<std::string> survivors;
    for (int i = 0; i < n; ++i) {
        if (i % 300 == 7) {
            survivors.insert(key_for(i));
        } else {
            table.remove(key_for(i));
        }
    }
    keys = iterate_keys(table);
    CHECK(std::set<std::string>(keys.begin(), keys.end()) == survivors);

    CHECK(throws([&] { table.nextkey(key_for(8)); }));  // removed key
}

// Delete keys from a completely full page, check iteration skips them, then
// insert new keys that only fit if the deleted space is reused.
static void test_iteration_after_delete_and_reinsert(const std::string& path) {
    // Fixed-width keys and values, so every record is exactly the same size
    // and a new record fits exactly where a deleted one was.
    auto old_key = [](int i) { return "old" + std::string(4 - std::to_string(i).size(), '0') + std::to_string(i); };
    auto new_key = [](int i) { return "new" + std::string(4 - std::to_string(i).size(), '0') + std::to_string(i); };
    const std::string value(20, 'v');

    ExtendibleHashTable::Options one_bucket;
    one_bucket.max_global_depth = 0;  // a single bucket; overflow means "page full"

    // How many records fill the bucket's page exactly?
    std::remove(path.c_str());
    int capacity = 0;
    {
        ExtendibleHashTable probe(path, one_bucket);
        while (probe.buckets()[0].chain.size() == 1) probe.put(old_key(capacity++), value);
        --capacity;  // the last put spilled into an overflow page
    }

    std::remove(path.c_str());
    std::set<std::string> expected_after;
    {
        ExtendibleHashTable table(path, one_bucket);
        for (int i = 0; i < capacity; ++i) table.put(old_key(i), value);
        CHECK(table.buckets()[0].chain.size() == 1);  // exactly full, no overflow
        const uint32_t pages_when_full = table.file_pages();
        const std::vector<std::string> before = iterate_keys(table);

        // 1. Delete every third key: iteration no longer returns them, and the
        //    rest keep their original order.
        std::set<std::string> deleted;
        for (int i = 0; i < capacity; i += 3) {
            CHECK(table.remove(old_key(i)));
            deleted.insert(old_key(i));
        }
        std::vector<std::string> expected;
        for (const std::string& k : before) {
            if (!deleted.count(k)) expected.push_back(k);
        }
        CHECK(iterate_keys(table) == expected);

        // 2. Insert the same number of same-sized new keys. They fit only by
        //    reusing the deleted records' space, so no overflow page appears.
        for (size_t i = 0; i < deleted.size(); ++i) table.put(new_key(static_cast<int>(i)), value);
        CHECK(table.buckets()[0].chain.size() == 1);
        CHECK(table.file_pages() == pages_when_full);

        // 3. Iteration returns every surviving old key and every new key, once.
        std::vector<std::string> after = iterate_keys(table);
        expected_after.insert(expected.begin(), expected.end());
        for (size_t i = 0; i < deleted.size(); ++i) expected_after.insert(new_key(static_cast<int>(i)));
        CHECK(std::set<std::string>(after.begin(), after.end()) == expected_after);
        CHECK(after.size() == static_cast<size_t>(capacity));
        for (const std::string& k : deleted) CHECK(!table.contains(k));
    }

    // Same result after closing and reopening the file.
    ExtendibleHashTable reopened(path, one_bucket);
    std::vector<std::string> reopened_keys = iterate_keys(reopened);
    CHECK(std::set<std::string>(reopened_keys.begin(), reopened_keys.end()) == expected_after);
}

static void test_compact(const std::string& path) {
    // A single page with deleted records.
    std::remove(path.c_str());
    {
        ExtendibleHashTable table(path);
        for (int i = 0; i < 100; ++i) table.put(key_for(i), value_for(i, 5));
        CHECK(table.global_depth() == 0);  // all in one bucket
        for (int i = 0; i < 30; ++i) table.remove(key_for(i));
        std::vector<std::string> order = iterate_keys(table);

        ExtendibleHashTable::CompactStats stats = table.compact();
        CHECK(stats.deleted_records_removed == 30);
        CHECK(stats.buckets_rewritten == 1);
        CHECK(stats.overflow_pages_freed == 0);
        CHECK(iterate_keys(table) == order);  // same keys, same order
        for (int i = 30; i < 100; ++i) CHECK(table.get(key_for(i)) == value_for(i, 5));

        // Nothing left to do the second time.
        stats = table.compact();
        CHECK(stats.buckets_rewritten == 0 && stats.deleted_records_removed == 0);
    }

    // Overflow chains: deleting most records leaves loosely packed chains.
    std::remove(path.c_str());
    ExtendibleHashTable::Options options;
    options.max_global_depth = 1;
    const int n = 1500;
    std::set<std::string> survivors;
    uint32_t pages_after_compact;
    {
        ExtendibleHashTable table(path, options);
        for (int i = 0; i < n; ++i) table.put(key_for(i), value_for(i, 100));
        size_t chain_pages_before = 0;
        for (const auto& b : table.buckets()) chain_pages_before += b.chain.size();

        for (int i = 0; i < n; ++i) {
            if (i % 5 == 0) {
                survivors.insert(key_for(i));
            } else {
                table.remove(key_for(i));
            }
        }

        ExtendibleHashTable::CompactStats stats = table.compact();
        CHECK(stats.deleted_records_removed == static_cast<size_t>(n - n / 5));
        CHECK(stats.overflow_pages_freed > 0);
        size_t chain_pages_after = 0;
        for (const auto& b : table.buckets()) chain_pages_after += b.chain.size();
        CHECK(chain_pages_after == chain_pages_before - stats.overflow_pages_freed);

        CHECK(table.size() == survivors.size());
        std::vector<std::string> keys = iterate_keys(table);
        CHECK(std::set<std::string>(keys.begin(), keys.end()) == survivors);
        for (int i = 0; i < n; i += 5) CHECK(table.get(key_for(i)) == value_for(i, 100));

        // New overflow pages come from the free list before the file grows.
        pages_after_compact = table.file_pages();
        for (int i = 0; i < 100; ++i) table.put("again" + std::to_string(i), value_for(i, 100));
        CHECK(table.file_pages() == pages_after_compact);
        for (int i = 0; i < 100; ++i) table.remove("again" + std::to_string(i));
    }

    // Compaction survives a reopen.
    {
        ExtendibleHashTable table(path, options);
        std::vector<std::string> keys = iterate_keys(table);
        CHECK(std::set<std::string>(keys.begin(), keys.end()) == survivors);
    }

    // Deleting everything collapses every chain to its primary page.
    {
        ExtendibleHashTable table(path, options);
        for (const std::string& k : survivors) table.remove(k);
        table.compact();
        CHECK(table.size() == 0);
        CHECK(!table.firstkey());
        for (const auto& b : table.buckets()) CHECK(b.chain.size() == 1);
    }

    // An empty database has nothing to compact.
    std::remove(path.c_str());
    ExtendibleHashTable empty(path);
    ExtendibleHashTable::CompactStats stats = empty.compact();
    CHECK(stats.buckets_rewritten == 0);
}

static void test_rejects_depth_beyond_max(const std::string& path) {
    std::remove(path.c_str());
    {
        ExtendibleHashTable table(path);
        for (int i = 0; i < 2000; ++i) table.put(key_for(i), value_for(i, 50));
        CHECK(table.global_depth() > 2);
    }
    ExtendibleHashTable::Options small;
    small.max_global_depth = 2;
    CHECK(throws([&] { ExtendibleHashTable table(path, small); }));
}

// Random puts, overwrites and removes checked against std::unordered_map,
// reopening the file partway through.
static void test_matches_reference(const std::string& path) {
    std::remove(path.c_str());
    std::unordered_map<std::string, std::string> reference;
    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> key_dist(0, 3000);
    std::uniform_int_distribution<int> op_dist(0, 9);
    std::uniform_int_distribution<int> len_dist(0, 300);

    auto run_ops = [&](ExtendibleHashTable& table, int count) {
        for (int step = 0; step < count; ++step) {
            std::string key = key_for(key_dist(rng));
            int op = op_dist(rng);
            if (op < 6) {
                std::string value = value_for(step, static_cast<size_t>(len_dist(rng)));
                table.put(key, value);
                reference[key] = value;
            } else if (op < 8) {
                CHECK(table.remove(key) == (reference.erase(key) == 1));
            } else {
                auto it = reference.find(key);
                auto got = table.get(key);
                CHECK(got.has_value() == (it != reference.end()));
                if (got) CHECK(*got == it->second);
            }
        }
        CHECK(table.size() == reference.size());
    };

    {
        ExtendibleHashTable table(path, {6});
        run_ops(table, 30000);
        table.compact();  // the second half runs against compacted pages
        for (const auto& [key, value] : reference) CHECK(table.get(key) == value);
    }
    ExtendibleHashTable table(path, {6});
    run_ops(table, 30000);
    for (const auto& [key, value] : reference) CHECK(table.get(key) == value);

    // for_each visits every live record exactly once.
    std::unordered_map<std::string, std::string> seen;
    table.for_each([&](std::string_view key, std::string_view value) {
        CHECK(seen.emplace(std::string(key), std::string(value)).second);
    });
    CHECK(seen == reference);

    std::vector<std::string> keys = iterate_keys(table);
    CHECK(std::set<std::string>(keys.begin(), keys.end()).size() == reference.size());
}

int main() {
    std::string path = temp_db_path("dbm_test_hash_table.db");

    test_basics(path);
    test_many_keys_small_cache(path);
    test_directory_spans_pages(path);
    test_overflow_chains(path);
    test_firstkey_nextkey(path);
    test_iteration_after_delete_and_reinsert(path);
    test_compact(path);
    test_rejects_depth_beyond_max(path);
    test_matches_reference(path);

    std::remove(path.c_str());
    std::cout << "all hash table tests passed\n";
    return 0;
}
