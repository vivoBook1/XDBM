#include <cstdio>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "buffer_pool.h"
#include "disk_manager.h"
#include "test_util.h"

// Each test page carries a 4-byte marker at its start.
static void write_marker(PageHandle& h, uint32_t marker) {
    put_u32(h.mutable_page().data(), marker);
}

static uint32_t read_marker(const PageHandle& h) { return get_u32(h.page().data()); }

static void test_eviction_writes_back_dirty_pages(const std::string& path) {
    std::remove(path.c_str());
    DiskManager disk(path);
    BufferPool pool(disk, 3);

    // Five pages through three frames: the first two get evicted.
    std::vector<PageId> ids;
    for (uint32_t i = 0; i < 5; ++i) {
        PageHandle h = pool.new_page();
        ids.push_back(h.id());
        write_marker(h, 100 + i);
    }

    // Every page is either cached or was written back when evicted. Each
    // fetch here evicts a page the loop no longer needs, so all five miss.
    size_t misses_before = pool.misses();
    for (uint32_t i = 0; i < 5; ++i) {
        PageHandle h = pool.fetch_page(ids[i]);
        CHECK(read_marker(h) == 100 + i);
    }
    CHECK(pool.misses() - misses_before == 5);
}

static void test_lru_order(const std::string& path) {
    std::remove(path.c_str());
    DiskManager disk(path);
    BufferPool pool(disk, 3);

    PageId a = pool.new_page().id();
    PageId b = pool.new_page().id();
    PageId c = pool.new_page().id();
    // LRU order, oldest first: a, b, c.

    pool.fetch_page(a);  // now b, c, a
    pool.new_page();     // evicts b -> c, a, d

    size_t hits = pool.hits();
    size_t misses = pool.misses();
    pool.fetch_page(a);
    pool.fetch_page(c);
    CHECK(pool.hits() - hits == 2);
    CHECK(pool.misses() == misses);

    pool.fetch_page(b);
    CHECK(pool.misses() - misses == 1);
}

static void test_pinning(const std::string& path) {
    std::remove(path.c_str());
    DiskManager disk(path);
    BufferPool pool(disk, 2);

    PageHandle h1 = pool.new_page();
    PageHandle h2 = pool.new_page();
    uint32_t pages_before = disk.page_count();
    CHECK(throws([&] { pool.new_page(); }));
    CHECK(disk.page_count() == pages_before);  // failed new_page leaked nothing

    // The same page can be pinned twice; it stays pinned until both let go.
    PageHandle h2_again = pool.fetch_page(h2.id());
    h2.release();
    CHECK(!h2.valid());
    CHECK(throws([&] { pool.new_page(); }));

    h1.release();
    PageHandle h3 = pool.new_page();  // evicts h1's page

    PageHandle moved = std::move(h3);
    CHECK(!h3.valid());  // NOLINT(bugprone-use-after-move): checks the moved-from state
    CHECK(moved.valid());
    CHECK(throws([&] { pool.new_page(); }));  // moved still pins its page
}

static void test_free_page(const std::string& path) {
    std::remove(path.c_str());
    DiskManager disk(path);
    BufferPool pool(disk, 2);

    PageHandle h = pool.new_page();
    PageId id = h.id();
    write_marker(h, 42);
    CHECK(throws([&] { pool.free_page(id); }));

    h.release();
    pool.free_page(id);

    PageHandle reused = pool.new_page();
    CHECK(reused.id() == id);
    CHECK(reused.page() == Page{});
}

static void test_writes_are_deferred(const std::string& path) {
    std::remove(path.c_str());
    DiskManager disk(path);
    BufferPool pool(disk, 2);

    PageHandle h = pool.new_page();
    write_marker(h, 7);

    Page on_disk;
    disk.read_page(h.id(), on_disk);
    CHECK(get_u32(on_disk.data()) == 0);  // only changed in memory so far

    pool.flush_page(h.id());
    disk.read_page(h.id(), on_disk);
    CHECK(get_u32(on_disk.data()) == 7);

    // Reading through page() doesn't dirty it.
    PageId id = h.id();
    h.release();
    PageHandle again = pool.fetch_page(id);
    (void)read_marker(again);
    put_u32(on_disk.data(), 99);
    disk.write_page(id, on_disk);  // simulate an outside change
    again.release();
    pool.flush_all();
    disk.read_page(id, on_disk);
    CHECK(get_u32(on_disk.data()) == 99);  // the clean page wasn't rewritten
}

static void test_persists_across_reopen(const std::string& path) {
    std::remove(path.c_str());
    std::vector<PageId> ids;
    {
        DiskManager disk(path);
        BufferPool pool(disk, 2);
        for (uint32_t i = 0; i < 4; ++i) {
            PageHandle h = pool.new_page();
            ids.push_back(h.id());
            write_marker(h, 500 + i);
        }
    }  // pool flushes the pages still cached, then disk flushes the header

    DiskManager disk(path);
    BufferPool pool(disk, 2);
    for (uint32_t i = 0; i < 4; ++i) {
        CHECK(read_marker(pool.fetch_page(ids[i])) == 500 + i);
    }
}

int main() {
    std::string path = temp_db_path("dbm_test_buffer_pool.db");

    test_eviction_writes_back_dirty_pages(path);
    test_lru_order(path);
    test_pinning(path);
    test_free_page(path);
    test_writes_are_deferred(path);
    test_persists_across_reopen(path);

    std::remove(path.c_str());
    std::cout << "all buffer pool tests passed\n";
    return 0;
}
