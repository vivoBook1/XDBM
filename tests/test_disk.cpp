#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "bucket_page.h"
#include "disk_manager.h"
#include "hash.h"
#include "test_util.h"

static bool insert(BucketPage& b, const std::string& key, const std::string& value) {
    return b.insert(fnv1a_hash(key), key, value);
}

static std::optional<std::string> find(const BucketPage& b, const std::string& key) {
    return b.find(key, fnv1a_hash(key));
}

static void test_bucket_basics() {
    Page page;
    BucketPage b(page);
    b.init(3);

    CHECK(b.type() == PageType::Bucket);
    CHECK(b.local_depth() == 3);
    CHECK(b.overflow_page() == kInvalidPageId);
    CHECK(b.free_space() == kPageSize - BucketPage::kHeaderSize);

    CHECK(insert(b, "name", "Alice"));
    CHECK(insert(b, "city", "Paris"));
    CHECK(insert(b, "empty", ""));
    CHECK(find(b, "name") == "Alice");
    CHECK(find(b, "city") == "Paris");
    CHECK(find(b, "empty") == "");
    CHECK(!find(b, "missing"));

    CHECK(b.remove("city", fnv1a_hash("city")));
    CHECK(!b.remove("city", fnv1a_hash("city")));
    CHECK(!find(b, "city"));
    CHECK(b.records().size() == 2);

    // Too large for any page.
    CHECK(!insert(b, "big", std::string(BucketPage::kMaxPayload, 'x')));
}

static void test_bucket_fill_and_compact() {
    Page page;
    BucketPage b(page);
    b.init(0);

    // 100-byte values: each record costs 8 (slot) + 8 (hash) + key + 100.
    int count = 0;
    while (insert(b, "key" + std::to_string(count), std::string(100, 'v'))) ++count;
    CHECK(count > 30);
    CHECK(b.records().size() == static_cast<size_t>(count));

    // Deleting frees nothing until compaction...
    for (int i = 0; i < count; i += 2) {
        std::string key = "key" + std::to_string(i);
        CHECK(b.remove(key, fnv1a_hash(key)));
    }
    size_t gap_before = b.free_space();

    // ...which insert() runs on its own when the gap is too small.
    CHECK(insert(b, "after-compact", std::string(100, 'n')));
    CHECK(b.free_space() > gap_before);
    CHECK(b.slot_count() == count - (count + 1) / 2 + 1);

    for (int i = 1; i < count; i += 2) {
        CHECK(find(b, "key" + std::to_string(i)) == std::string(100, 'v'));
    }
    CHECK(find(b, "after-compact") == std::string(100, 'n'));
}

static void test_disk_manager(const std::string& path) {
    std::remove(path.c_str());

    {
        DiskManager dm(path);
        CHECK(dm.is_new());
        CHECK(dm.page_count() == 1);

        CHECK(dm.allocate_page() == 1);
        CHECK(dm.allocate_page() == 2);
        CHECK(dm.allocate_page() == 3);

        Page page;
        BucketPage b(page);
        b.init(2);
        CHECK(insert(b, "hello", "world"));
        CHECK(insert(b, "foo", "bar"));
        dm.write_page(2, page);

        dm.header().global_depth = 2;
        dm.header().record_count = 2;
        dm.header().directory_pages = {1};
    }  // destructor flushes the header

    CHECK(std::filesystem::file_size(path) == 4 * kPageSize);

    {
        DiskManager dm(path);
        CHECK(!dm.is_new());
        CHECK(dm.page_count() == 4);
        CHECK(dm.header().global_depth == 2);
        CHECK(dm.header().record_count == 2);
        CHECK(dm.header().directory_pages == std::vector<PageId>{1});

        Page page;
        dm.read_page(2, page);
        BucketPage b(page);
        CHECK(b.type() == PageType::Bucket);
        CHECK(b.local_depth() == 2);
        CHECK(find(b, "hello") == "world");
        CHECK(find(b, "foo") == "bar");

        // Freed pages are reused, most recently freed first, and come back zeroed.
        dm.free_page(2);
        dm.free_page(3);
        CHECK(throws([&] { dm.free_page(3); }));
        CHECK(dm.allocate_page() == 3);
        CHECK(dm.allocate_page() == 2);
        dm.read_page(2, page);
        CHECK(page == Page{});
        CHECK(dm.allocate_page() == 4);

        dm.free_page(4);
    }

    {
        // The free list survives a reopen.
        DiskManager dm(path);
        CHECK(dm.allocate_page() == 4);
        CHECK(dm.page_count() == 5);

        Page page;
        CHECK(throws([&] { dm.read_page(0, page); }));  // header is off limits
        CHECK(throws([&] { dm.read_page(5, page); }));  // past the end
    }

    std::remove(path.c_str());
}

static void test_rejects_bad_files(const std::string& path) {
    {
        std::ofstream out(path, std::ios::binary);
        out << std::string(kPageSize, 'x');
    }
    CHECK(throws([&] { DiskManager dm(path); }));

    {
        std::ofstream out(path, std::ios::binary);
        out << "short";
    }
    CHECK(throws([&] { DiskManager dm(path); }));

    // A valid header that claims more pages than the file holds.
    std::remove(path.c_str());
    {
        DiskManager dm(path);
        dm.allocate_page();
    }
    std::filesystem::resize_file(path, kPageSize);
    CHECK(throws([&] { DiskManager dm(path); }));

    std::remove(path.c_str());
}

int main() {
    std::string path = temp_db_path("dbm_test_disk.db");

    test_bucket_basics();
    test_bucket_fill_and_compact();
    test_disk_manager(path);
    test_rejects_bad_files(path);

    std::cout << "all disk tests passed\n";
    return 0;
}
