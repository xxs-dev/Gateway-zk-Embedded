#include "edge_gateway/storage_budget.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>

using namespace edge_gateway;

int main() {
    const StorageUsage usage{100, 200, 300, 400};
    auto result = checkStorageAdmission({1000, 250}, usage, 0, 250);
    assert(result.allowed && result.usedBytes == 1000 && result.reason == std::string("ok"));

    result = checkStorageAdmission({1000, 0}, usage, 1, 0);
    assert(!result.allowed && std::string(result.reason) == "max-bytes");

    result = checkStorageAdmission({0, 250}, usage, 0, 249);
    assert(!result.allowed && std::string(result.reason) == "min-free-bytes");

    result = checkStorageAdmission({}, usage, 0, 0);
    assert(result.allowed && result.usedBytes == 1000);

    result = checkStorageAdmission({}, {std::numeric_limits<std::uint64_t>::max(), 0, 0, 0}, 1, 0);
    assert(!result.allowed && std::string(result.reason) == "size-overflow");

    std::cout << "storage budget: 5 assertions PASS\n";
}
