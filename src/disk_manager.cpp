#include "disk_manager.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>

#include "hash.h"

namespace {

constexpr char kMagic[8] = {'E', 'X', 'T', 'H', 'A', 'S', 'H', '1'};
constexpr uint32_t kFormatVersion = 1;

// Header page field offsets; see the layout in disk_manager.h.
constexpr size_t kMagicOffset = 0;
constexpr size_t kVersionOffset = 8;
constexpr size_t kPageSizeOffset = 12;
constexpr size_t kHashFunctionOffset = 16;
constexpr size_t kPageCountOffset = 20;
constexpr size_t kFreeListOffset = 24;
constexpr size_t kGlobalDepthOffset = 28;
constexpr size_t kRecordCountOffset = 32;
constexpr size_t kDirectoryCountOffset = 40;
constexpr size_t kDirectoryPagesOffset = 44;

// Where a freed page stores the next free page's id.
constexpr size_t kFreeNextOffset = 4;

[[noreturn]] void throw_errno(const std::string& what) {
    throw std::runtime_error(what + ": " + std::strerror(errno));
}

off_t page_offset(PageId id) {
    return static_cast<off_t>(id) * static_cast<off_t>(kPageSize);
}

// pread/pwrite may transfer fewer bytes than asked; loop until done.
void read_exact(int fd, uint8_t* buf, size_t len, off_t offset) {
    while (len > 0) {
        ssize_t n = ::pread(fd, buf, len, offset);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw_errno("pread");
        }
        if (n == 0) throw std::runtime_error("pread: unexpected end of file");
        buf += n;
        len -= static_cast<size_t>(n);
        offset += n;
    }
}

void write_exact(int fd, const uint8_t* buf, size_t len, off_t offset) {
    while (len > 0) {
        ssize_t n = ::pwrite(fd, buf, len, offset);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw_errno("pwrite");
        }
        buf += n;
        len -= static_cast<size_t>(n);
        offset += n;
    }
}

}  // namespace

DiskManager::DiskManager(const std::string& path) : path_(path) {
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) throw_errno("open " + path);

    try {
        struct stat st;
        if (::fstat(fd_, &st) != 0) throw_errno("fstat " + path);

        if (st.st_size == 0) {
            created_ = true;
            page_count_ = 1;  // just the header page
            flush();
        } else {
            read_header(static_cast<uint64_t>(st.st_size));
        }
    } catch (...) {
        ::close(fd_);
        throw;
    }
}

DiskManager::~DiskManager() {
    try {
        flush();
    } catch (...) {
        // Destructors must not throw; call flush() directly to see errors.
    }
    ::close(fd_);
}

void DiskManager::read_page(PageId id, Page& page) const {
    check_page_id(id);
    read_exact(fd_, page.data(), kPageSize, page_offset(id));
}

void DiskManager::write_page(PageId id, const Page& page) {
    check_page_id(id);
    write_exact(fd_, page.data(), kPageSize, page_offset(id));
}

PageId DiskManager::allocate_page() {
    PageId id;
    if (free_list_head_ != kInvalidPageId) {
        id = free_list_head_;
        Page freed;
        read_page(id, freed);
        free_list_head_ = get_u32(freed.data() + kFreeNextOffset);
    } else {
        if (page_count_ == std::numeric_limits<PageId>::max()) {
            throw std::runtime_error("database file is full");
        }
        id = page_count_++;
    }
    // Zero the page; when appending, this also extends the file.
    write_page(id, Page{});
    return id;
}

void DiskManager::free_page(PageId id) {
    Page page;
    read_page(id, page);
    if (page[0] == static_cast<uint8_t>(PageType::Free)) {
        throw std::logic_error("page " + std::to_string(id) + " freed twice");
    }

    page.fill(0);
    page[0] = static_cast<uint8_t>(PageType::Free);
    put_u32(page.data() + kFreeNextOffset, free_list_head_);
    write_page(id, page);
    free_list_head_ = id;
}

void DiskManager::flush() {
    write_header();
    if (::fsync(fd_) != 0) throw_errno("fsync " + path_);
}

void DiskManager::write_header() {
    if (header_.directory_pages.size() > kMaxDirectoryPages) {
        throw std::length_error("too many directory pages for the header");
    }

    Page page{};
    uint8_t* p = page.data();
    std::memcpy(p + kMagicOffset, kMagic, sizeof(kMagic));
    put_u32(p + kVersionOffset, kFormatVersion);
    put_u32(p + kPageSizeOffset, kPageSize);
    put_u32(p + kHashFunctionOffset, kHashFunctionId);
    put_u32(p + kPageCountOffset, page_count_);
    put_u32(p + kFreeListOffset, free_list_head_);
    put_u32(p + kGlobalDepthOffset, header_.global_depth);
    put_u64(p + kRecordCountOffset, header_.record_count);
    put_u32(p + kDirectoryCountOffset,
            static_cast<uint32_t>(header_.directory_pages.size()));
    for (size_t i = 0; i < header_.directory_pages.size(); ++i) {
        put_u32(p + kDirectoryPagesOffset + 4 * i, header_.directory_pages[i]);
    }

    write_exact(fd_, p, kPageSize, 0);
}

void DiskManager::read_header(uint64_t file_size) {
    if (file_size < kPageSize) {
        throw std::runtime_error(path_ + ": file too small to be a database");
    }

    Page page;
    read_exact(fd_, page.data(), kPageSize, 0);
    const uint8_t* p = page.data();

    if (std::memcmp(p + kMagicOffset, kMagic, sizeof(kMagic)) != 0) {
        throw std::runtime_error(path_ + ": not a database file");
    }
    if (get_u32(p + kVersionOffset) != kFormatVersion) {
        throw std::runtime_error(path_ + ": unsupported format version");
    }
    if (get_u32(p + kPageSizeOffset) != kPageSize) {
        throw std::runtime_error(path_ + ": page size mismatch");
    }
    if (get_u32(p + kHashFunctionOffset) != kHashFunctionId) {
        throw std::runtime_error(path_ + ": written with a different hash function");
    }

    page_count_ = get_u32(p + kPageCountOffset);
    if (page_count_ == 0 || static_cast<uint64_t>(page_count_) * kPageSize > file_size) {
        throw std::runtime_error(path_ + ": file is truncated");
    }

    free_list_head_ = get_u32(p + kFreeListOffset);
    header_.global_depth = get_u32(p + kGlobalDepthOffset);
    header_.record_count = get_u64(p + kRecordCountOffset);

    uint32_t directory_count = get_u32(p + kDirectoryCountOffset);
    if (directory_count > kMaxDirectoryPages) {
        throw std::runtime_error(path_ + ": corrupt directory page count");
    }
    header_.directory_pages.resize(directory_count);
    for (size_t i = 0; i < directory_count; ++i) {
        header_.directory_pages[i] = get_u32(p + kDirectoryPagesOffset + 4 * i);
    }
}

void DiskManager::check_page_id(PageId id) const {
    if (id == kInvalidPageId || id >= page_count_) {
        throw std::out_of_range("invalid page id " + std::to_string(id));
    }
}
