#pragma once

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

// Unlike assert(), stays active in optimized builds.
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::cerr << __FILE__ << ":" << __LINE__ << ": FAILED: " #cond   \
                      << "\n";                                               \
            std::exit(1);                                                    \
        }                                                                    \
    } while (0)

template <typename F>
bool throws(F f) {
    try {
        f();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

inline std::string temp_db_path(const std::string& name) {
    return (std::filesystem::temp_directory_path() / name).string();
}
