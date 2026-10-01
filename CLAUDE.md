# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Disk-backed extendible hash table in C++17, with a `xdbm` command-line front end.

- `src/`: the engine (pages, disk manager, buffer pool, hash table), kept flat. Includes are plain `"page.h"` style; the Makefile passes `-Isrc`.
- `cli/main.cpp`: the `xdbm` tool.
- `tests/`: one `test_<name>.cpp` executable per component, plus `test_util.h`.

## Commands

- `make` builds `build/xdbm` and the test executables; `make test` runs every test and stops at the first failure.
- Lint runs only at commit time: `.githooks/pre-commit` runs clang-tidy (config in `.clang-tidy`) on staged C++ files and blocks the commit on any finding. Don't run `make lint` as part of normal changes; if a commit is blocked, fix the findings rather than using `--no-verify`. A fresh clone needs `make hooks` once to enable the hook.
- clang-tidy comes from Homebrew's keg-only `llvm`, so it isn't on PATH: use `make lint` (or `make lint LINT_FILES="..."`) rather than calling `clang-tidy` directly, and don't put llvm on PATH (it would shadow Apple clang).
- `make install` copies `xdbm` to `~/.local/bin`. The installed command is a copy, so reinstall after any change.
- The Makefile picks up files by wildcard: any `src/*.cpp` is linked into every program, and any `tests/test_*.cpp` becomes a test run by `make test`. There are no object files, so every build recompiles all of `src/`.
- If AddressSanitizer hangs at startup (some macOS toolchains do this even for an empty program), use `-fsanitize=undefined -fno-sanitize-recover=all` for sanitizer runs instead.

## Before calling a change done

1. Add or extend tests in the matching `tests/test_*.cpp` for new behavior.
2. `make test` passes.
3. `make install`, so the `xdbm` on PATH matches the code.

## Tests

- `tests/cli_test.sh` tests the `xdbm` binary end to end (output, exit codes, `--db`/`XDBM_DB`/default file); `make test` runs it after the C++ tests. Extend it when changing CLI behavior. It sets `HOME` to a temp dir, so new checks must never use the real `~/.xdbm`.
- C++ tests are plain executables with no framework. Use `CHECK(cond)` and `throws(lambda)` from `tests/test_util.h`, not `assert` (which vanishes under `-DNDEBUG`).
- Put test databases at `temp_db_path("dbm_test_<name>.db")` and `std::remove` the file at the start of each test.
- To test overflow chains, lower `ExtendibleHashTable::Options::max_global_depth` rather than inserting millions of keys. From the CLI: `xdbm --db x.db --max-depth 1 fill 500`, then `xdbm --db x.db buckets` to see each bucket's page chain.

## On-disk format

- Existing `.db` files must stay readable after format changes. Use `/format-change` when changing any page layout or header field.
- Write on-disk integers only with the little-endian helpers in `src/page.h` (`put_u32`/`get_u32` etc.), never `memcpy` of structs.
- Never change `fnv1a_hash`: records are placed by hash bits, so a different function makes every existing file unreadable (`kHashFunctionId` in `src/hash.h` guards this).
- Page 0 is the header, so page id 0 also means "no page" (`kInvalidPageId`).
- Byte layouts are documented in block comments in `src/disk_manager.h`, `src/bucket_page.h` and `src/directory_page.h`; keep them in sync with the code.

## Code conventions

- Errors are exceptions; destructors never throw (they swallow errors, and callers use `flush()` to see them).
- Every `PageHandle` must be destroyed before its `BufferPool`, and the pool before its `DiskManager`. `ExtendibleHashTable` depends on declaring `disk_` before `pool_`.
- Use `BucketPageView` with `PageHandle::page()` for reads. `mutable_page()` marks the page dirty and forces a disk write.

## Git

- Conventional commits: `feat:`, `fix:`, `refactor:`, `test:`, `docs:` with a short imperative subject.
