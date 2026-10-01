#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "extendible_hash_table.h"

// xdbm: command-line front end for the extendible hash table.
//
// The database file is, in order of precedence: --db <file>, the XDBM_DB
// environment variable, or ~/.xdbm/data.db.

namespace {

void print_usage(std::ostream& os) {
    os << "usage: xdbm [--db <file>] [--max-depth <n>] <command> [args]\n"
          "\n"
          "commands:\n"
          "  put <key> <value>   insert or overwrite a key\n"
          "  get <key>           print a key's value\n"
          "  remove <key>        delete a key\n"
          "  list [--after <key>] [--limit <n>]\n"
          "                      print keys one per line, in storage order (not\n"
          "                      sorted); --after starts after <key>, --limit stops\n"
          "                      after n keys, so the two together page through\n"
          "                      the store\n"
          "  fill <count>        insert key0..key<count-1> (for testing)\n"
          "  stats               show record count and file layout\n"
          "  buckets             list every bucket and its overflow chain\n"
          "  compact             reclaim deleted records' space, free empty\n"
          "                      overflow pages (the file doesn't shrink)\n"
          "  help                show this message\n"
          "\n"
          "database file: --db <file>, else $XDBM_DB, else ~/.xdbm/data.db\n"
          "\n"
          "--max-depth <n> caps the global depth for this run; a full bucket at\n"
          "that depth grows an overflow chain instead of splitting. Use a small n\n"
          "(e.g. 1) with fill to see overflow pages. Opening a file whose depth is\n"
          "already above n is an error.\n";
}

int usage_error() {
    print_usage(std::cerr);
    return 2;
}

std::string default_db_path() {
    const char* home = std::getenv("HOME");
    if (!home || !*home) throw std::runtime_error("HOME is not set; use --db <file>");
    std::filesystem::path dir = std::filesystem::path(home) / ".xdbm";
    std::filesystem::create_directories(dir);
    return (dir / "data.db").string();
}

// Options of the `list` command.
struct ListOptions {
    std::optional<std::string> after;
    std::optional<size_t> limit;
};

std::optional<size_t> parse_count(const std::string& s) {
    if (s.empty() || s.size() > 18 || s.find_first_not_of("0123456789") != std::string::npos) {
        return std::nullopt;
    }
    size_t n = std::stoull(s);
    if (n == 0) return std::nullopt;
    return n;
}

// Parses what follows `list`: --after <key> and --limit <n>, each also
// accepted as --name=value. Returns std::nullopt on a usage error.
std::optional<ListOptions> parse_list_options(const std::vector<std::string>& args) {
    ListOptions options;
    for (size_t i = 1; i < args.size(); ++i) {
        std::string name = args[i];
        std::string value;
        if (size_t eq = name.find('='); eq != std::string::npos) {
            value = name.substr(eq + 1);
            name.resize(eq);
        } else if (i + 1 < args.size()) {
            value = args[++i];
        } else {
            return std::nullopt;
        }

        if (name == "--after") {
            options.after = value;
        } else if (name == "--limit") {
            options.limit = parse_count(value);
            if (!options.limit) return std::nullopt;
        } else {
            return std::nullopt;
        }
    }
    return options;
}

void list_keys(ExtendibleHashTable& table, const ListOptions& options) {
    if (!options.after && !options.limit) {
        // The whole store in one pass, without per-key lookups.
        table.for_each([](std::string_view key, std::string_view) { std::cout << key << '\n'; });
        return;
    }

    std::optional<std::string> key;
    if (options.after) {
        if (!table.contains(*options.after)) {
            throw std::runtime_error("--after: key not found: " + *options.after);
        }
        key = table.nextkey(*options.after);
    } else {
        key = table.firstkey();
    }

    std::string last;
    size_t printed = 0;
    while (key && (!options.limit || printed < *options.limit)) {
        std::cout << *key << '\n';
        last = *key;
        ++printed;
        key = table.nextkey(last);
    }

    // Stopped by --limit with keys left: say how to get the next page. This
    // goes to stderr so stdout stays a plain list of keys.
    if (key) {
        std::cerr << "more keys; continue with: xdbm list --after '" << last << "' --limit "
                  << *options.limit << "\n";
    }
}

std::optional<uint32_t> parse_depth(const std::string& s) {
    if (s.empty() || s.size() > 2 || s.find_first_not_of("0123456789") != std::string::npos) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(std::stoul(s));
}

// Directory entry number as `width` binary digits, e.g. 2 -> "10".
std::string to_binary(size_t value, uint32_t width) {
    if (width == 0) return "(all)";
    std::string bits;
    for (uint32_t i = width; i > 0; --i) bits += ((value >> (i - 1)) & 1) ? '1' : '0';
    return bits;
}

void print_buckets(ExtendibleHashTable& table) {
    auto buckets = table.buckets();
    std::cout << "global depth " << table.global_depth() << ", " << buckets.size()
              << " buckets, " << table.size() << " records\n";

    for (size_t b = 0; b < buckets.size(); ++b) {
        const auto& info = buckets[b];
        std::cout << "\nbucket " << b << ": local depth " << int{info.local_depth}
                  << ", directory entries:";
        for (size_t entry : info.directory_entries) {
            std::cout << " " << to_binary(entry, table.global_depth());
        }
        std::cout << "\n";

        size_t total = 0;
        for (size_t i = 0; i < info.chain.size(); ++i) {
            std::cout << "  page " << info.chain[i].page << ": " << info.chain[i].records
                      << " records" << (i == 0 ? "" : "  (overflow)") << "\n";
            total += info.chain[i].records;
        }
        if (info.chain.size() > 1) {
            std::cout << "  total " << total << " records in " << info.chain.size()
                      << " pages\n";
        }
    }
}

std::string resolve_db_path(const std::string& flag) {
    if (!flag.empty()) return flag;
    if (const char* env = std::getenv("XDBM_DB"); env && *env) return env;
    return default_db_path();
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);

    // Leading options, as "--name value" or "--name=value".
    std::string db_flag;
    ExtendibleHashTable::Options options;
    while (!args.empty() && args[0].rfind("--", 0) == 0 && args[0] != "--help") {
        std::string name = args[0];
        std::string value;
        if (size_t eq = name.find('='); eq != std::string::npos) {
            value = name.substr(eq + 1);
            name.resize(eq);
            args.erase(args.begin());
        } else {
            if (args.size() < 2) return usage_error();
            value = args[1];
            args.erase(args.begin(), args.begin() + 2);
        }

        if (name == "--db") {
            db_flag = value;
        } else if (name == "--max-depth") {
            std::optional<uint32_t> depth = parse_depth(value);
            if (!depth) {
                std::cerr << "xdbm: --max-depth needs a number from 0 to 99\n";
                return 2;
            }
            options.max_global_depth = *depth;
        } else {
            return usage_error();
        }
    }

    if (args.empty()) return usage_error();
    const std::string& cmd = args[0];
    if (cmd == "help" || cmd == "-h" || cmd == "--help") {
        print_usage(std::cout);
        return 0;
    }

    try {
        std::string path = resolve_db_path(db_flag);
        ExtendibleHashTable table(path, options);

        if (cmd == "put" && args.size() == 3) {
            table.put(args[1], args[2]);
        } else if (cmd == "get" && args.size() == 2) {
            auto value = table.get(args[1]);
            if (!value) {
                std::cerr << "not found: " << args[1] << "\n";
                return 1;
            }
            std::cout << *value << "\n";
        } else if (cmd == "remove" && args.size() == 2) {
            if (!table.remove(args[1])) {
                std::cerr << "not found: " << args[1] << "\n";
                return 1;
            }
        } else if (cmd == "list") {
            std::optional<ListOptions> list_options = parse_list_options(args);
            if (!list_options) return usage_error();
            list_keys(table, *list_options);
        } else if (cmd == "fill" && args.size() == 2) {
            int count = std::stoi(args[1]);
            for (int i = 0; i < count; ++i) {
                table.put("key" + std::to_string(i), "value" + std::to_string(i));
            }
        } else if (cmd == "stats" && args.size() == 1) {
            std::cout << "file:           " << path << "\n"
                      << "records:        " << table.size() << "\n"
                      << "global depth:   " << table.global_depth() << "\n"
                      << "directory size: " << table.directory_size() << "\n"
                      << "file pages:     " << table.file_pages() << " ("
                      << table.file_pages() * kPageSize / 1024 << " KiB)\n";
        } else if (cmd == "buckets" && args.size() == 1) {
            print_buckets(table);
        } else if (cmd == "compact" && args.size() == 1) {
            ExtendibleHashTable::CompactStats stats = table.compact();
            std::cout << "removed " << stats.deleted_records_removed << " deleted records, freed "
                      << stats.overflow_pages_freed << " overflow pages (rewrote "
                      << stats.buckets_rewritten << " buckets)\n";
        } else {
            return usage_error();
        }
        table.flush();
    } catch (const std::exception& e) {
        std::cerr << "xdbm: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
