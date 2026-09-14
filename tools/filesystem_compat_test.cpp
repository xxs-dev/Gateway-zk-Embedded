#include "edge_gateway/filesystem_compat.hpp"
#include <iostream>
#include <stdexcept>

int main() {
    using edge_gateway::normalizedAbsolutePath;
    try {
        const char* paths[] = {"/a/b/../ledger.db", "/a/./ledger.db",
                               "/a//ledger.db", "/../a/ledger.db"};
        for (const auto* path : paths) {
            if (normalizedAbsolutePath(path).generic_string() != "/a/ledger.db")
                throw std::runtime_error("absolute ledger path normalization failed");
        }
        if (normalizedAbsolutePath("/a/b/..").generic_string() != "/a/" ||
            normalizedAbsolutePath("/a/../..").generic_string() != "/")
            throw std::runtime_error("root/trailing separator normalization failed");
        bool rejected = false;
        try { normalizedAbsolutePath("ledger.db"); }
        catch (const std::invalid_argument&) { rejected = true; }
        if (!rejected) throw std::runtime_error("relative ledger path accepted");
        std::cout << "filesystem compatibility PASS\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
