# xdbm

A disk-backed key-value store built on **extendible hashing**, written from scratch in C++17 with no dependencies. It comes as a small storage engine library and an `xdbm` command-line tool.

```sh
$ xdbm put name Alice
$ xdbm get name
Alice
$ xdbm list
name
```

## Features

- **Extendible hashing:** a directory of `2^global_depth` entries points at fixed-size bucket pages. A full bucket splits on its own; only the directory doubles, never a full rehash.
- **Page-based file format:** 4 KB pages, with slotted bucket pages for variable-length keys and values. All integers are stored little-endian, so files are portable between machines.
- **Buffer pool:** an LRU page cache with pinning and dirty-page write-back.
- **Overflow chains:** buckets that can't split any further grow a chain of overflow pages.
- **Iteration:** `for_each`, plus dbm-style `firstkey()` / `nextkey()` that keep no cursor between calls.
- **Compaction:** reclaims space from deleted records and frees overflow pages that are no longer needed.

## Building

You need a C++17 compiler and `make`. It has been tested with Apple clang 17 on macOS, and the code uses only POSIX file I/O.

```sh
make            # builds build/xdbm and the test programs
make test       # C++ unit tests + end-to-end CLI tests
make install    # copies xdbm to ~/.local/bin (override with PREFIX=...)
```

## Using the CLI

```sh
xdbm put <key> <value>      # insert or overwrite
xdbm get <key>              # print the value (exit 1 if missing)
xdbm remove <key>           # delete (exit 1 if missing)
xdbm list                   # every key, one per line, in storage order
xdbm list --limit 100                    # first page of keys
xdbm list --after <last-key> --limit 100 # next page
xdbm stats                  # record count, global depth, file size
xdbm buckets                # each bucket and its page chain
xdbm compact                # reclaim space from deleted records
xdbm help
```

**Database file**, in order of precedence: `--db <file>`, then the `XDBM_DB` environment variable, then `~/.xdbm/data.db`. The file is created on first use.

**Exit codes:** 0 on success, 1 for "not found" or an error, 2 for wrong usage.

To see overflow chains without inserting millions of keys, cap the depth while filling:

```sh
xdbm --db demo.db --max-depth 1 fill 500
xdbm --db demo.db buckets
```

## Using the library

```cpp
#include "extendible_hash_table.h"

ExtendibleHashTable table("my.db");   // opens or creates the file

table.put("name", "Alice");
std::optional<std::string> name = table.get("name");
table.remove("name");

for (auto key = table.firstkey(); key; key = table.nextkey(*key)) {
    // ...
}

table.flush();   // write cached pages to disk and fsync
```

Link against everything in `src/` and add `-Isrc` to the compiler flags.

## How it works

```
page 0      header: magic, format version, page count, free list,
            global depth, record count, directory page ids
page 1..    directory pages: bucket page id for each directory entry
page ...    bucket pages: slot array + records, optional overflow link
```

- **Lookup:** hash the key with FNV-1a 64-bit, use the low `global_depth` bits to pick a directory entry, then read one bucket page. The directory is kept in memory.
- **Insert into a full bucket:** split it using one more hash bit. If its local depth already equals the global depth, double the directory first.
- **Delete:** marks the record's slot as deleted. The space is reclaimed when the page is compacted, which happens automatically when an insert needs the room, or with `xdbm compact`.

The byte-level layouts are documented in [`src/disk_manager.h`](src/disk_manager.h), [`src/bucket_page.h`](src/bucket_page.h) and [`src/directory_page.h`](src/directory_page.h).

## Project layout

```
src/      storage engine: pages, disk manager, buffer pool, hash table
cli/      the xdbm command-line tool
tests/    C++ unit tests (test_*.cpp) and CLI tests (cli_test.sh)
```

## Development

- `make lint` runs clang-tidy using `.clang-tidy`. It needs Homebrew's `llvm` on macOS: `brew install llvm`.
- `make hooks` enables the pre-commit hook, which lints the staged C++ files and blocks the commit on any finding. Run it once after cloning.

## Limitations

- **No crash safety:** changes reach the file on `flush()` or close, and a crash partway through can leave the file inconsistent. There is no write-ahead log.
- **Single-threaded:** there is no locking, and only one process should use a file at a time.
- **Space isn't returned to the OS:** buckets never merge, the directory never shrinks, and the file never gets smaller. Freed pages are reused by later inserts.
- **Size limits:** a key plus its value must fit in one page (4068 bytes). The global depth is capped at 19, about 2 GB of buckets; beyond that, buckets grow overflow chains.
- **Iteration order:** `list`, `for_each` and `nextkey` return keys in storage order, not sorted. Changing the store while iterating can skip or repeat keys.
