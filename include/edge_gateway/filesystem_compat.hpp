#pragma once
#include <stdexcept>

// GCC 6's C++17 mode predates the final library facilities.
#if defined(__GNUC__) && !defined(__clang__) && !defined(_WIN32) && __GNUC__ < 8
#include <experimental/filesystem>
#include <vector>
namespace edge_gateway {
namespace filesystem = std::experimental::filesystem;
inline filesystem::path normalizedAbsolutePath(const filesystem::path& path) {
    if (!path.is_absolute()) throw std::invalid_argument("controlDedupPath must be absolute");
    std::vector<filesystem::path> parts;
    for (const auto& part : path.relative_path()) {
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else {
            parts.push_back(part);
        }
    }
    filesystem::path result = path.root_path();
    for (const auto& part : parts) result /= part;
    const auto last = path.filename();
    if (!parts.empty() && (last.empty() || last == "." || last == "..")) result += "/";
    return result;
}
}
#else
#include <filesystem>
namespace edge_gateway {
namespace filesystem = std::filesystem;
inline filesystem::path normalizedAbsolutePath(const filesystem::path& path) {
    if (!path.is_absolute()) throw std::invalid_argument("controlDedupPath must be absolute");
    return path.lexically_normal();
}
}
#endif
