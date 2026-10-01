---
name: format-change
description: Checklist for changing the on-disk database format (header fields, bucket/directory page layout, record encoding) while keeping existing .db files readable. Use before editing any byte layout in src/disk_manager, src/bucket_page or src/directory_page.
---

Existing `.db` files must keep working after a format change. Follow these steps in order.

## 1. Decide whether this is a format change

It is if an older build would misread a file written by the new code, or the new code would misread an old file. Examples: adding, moving or resizing a header field; changing slot, record or directory entry encoding; giving a new meaning to a reserved byte or flag bit.

If it isn't (for example, a new flag bit that old readers already ignore and that has a safe default), say so and skip the version bump. Still update the layout comments.

## 2. Bump the version, keep accepting older ones

- Increment `kFormatVersion` in `src/disk_manager.cpp`.
- `read_header` currently rejects any version other than `kFormatVersion`. Change it to accept every version from 1 to `kFormatVersion`, and still reject newer versions ("written by a newer xdbm").
- Store the file's version (for example on `DiskManager`) so page readers can branch on it.

## 3. Choose read-compatibility or migration

Pick one and state which in the change summary:

- **Read both formats.** Decoders branch on the file's version, and new writes use the new format. Best when the difference is small and local.
- **Migrate on open.** When an old version is detected, rewrite the affected pages to the new format, then write the header with the new version and `flush()`. Best when supporting both formats would spread through the code. Rewrite pages before updating the header, so a crash partway through leaves a file that still says "old version".

Never silently reinterpret old bytes under the new layout.

## 4. Update the layout documentation

Update the byte layout block comment in whichever of `src/disk_manager.h`, `src/bucket_page.h` or `src/directory_page.h` changed, and note the version in which the layout changed.

## 5. Test with a real old-format file

- Add a test that creates an old-format file (with a small helper that writes the old layout with the `src/page.h` put helpers), opens it with the new code, and checks every record reads back correctly.
- If migrating: reopen after migration and check the header now reports the new version.
- Add a test that a file claiming a version above `kFormatVersion` is rejected.
- `make test` must pass. Then `make install`.
