#include "edge_gateway/storage_budget.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

using namespace edge_gateway;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    try {
        const StorageUsage usage{100, 200, 300, 400};
        auto result = checkStorageAdmission({1000, 250}, usage, 0, 250);
        require(result.allowed && result.usedBytes == 1000 && std::string(result.reason) == "ok", "baseline admission");

        result = checkStorageAdmission({1000, 0}, usage, 1, 0);
        require(!result.allowed && std::string(result.reason) == "max-bytes", "max bytes rejection");

        result = checkStorageAdmission({0, 100}, {}, 100, 200);
        require(result.allowed, "exact post-write reserve must be accepted");

        result = checkStorageAdmission({0, 100}, {}, 100, 199);
        require(!result.allowed && std::string(result.reason) == "min-free-bytes", "one byte below reserve");

        result = checkStorageAdmission({}, {}, 201, 200);
        require(!result.allowed && std::string(result.reason) == "insufficient-available-bytes", "incoming larger than free space");

        result = checkStorageAdmission({0, 0}, usage, 0, 0);
        require(result.allowed && result.usedBytes == 1000, "zero limits remain disabled");

        result = checkStorageAdmission({0, 1}, {}, std::numeric_limits<std::uint64_t>::max(),
            std::numeric_limits<std::uint64_t>::max());
        require(!result.allowed && std::string(result.reason) == "min-free-bytes", "maximum incoming must leave reserve");

        result = checkStorageAdmission({}, {std::numeric_limits<std::uint64_t>::max(), 0, 0, 0}, 1, 1);
        require(!result.allowed && std::string(result.reason) == "size-overflow", "usage overflow");

        std::cout << "storage budget: 8 assertions PASS\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
