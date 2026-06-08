#include "edge_gateway/priority_control_lease.hpp"

#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

}  // namespace

int main() {
    const std::string path = "/tmp/gateway_priority_control_lease_test.json";
    std::remove(path.c_str());

    try {
        edge_gateway::PriorityControlLease ownerA(path, "mqtt-driver");
        edge_gateway::PriorityControlLease ownerB(path, "modbus:store-a");

        require(!ownerB.isBlocked(1000), "missing lease should not block");
        ownerA.acquire("CMD1", "METER_1", 1001, 1000, 1000);
        require(ownerB.isBlocked(1200), "other owner should be blocked by active lease");
        require(!ownerA.isBlocked(1200), "lease owner should not block itself");
        require(!ownerB.isBlocked(2500), "expired lease should not block");

        ownerA.acquire("CMD2", "METER_1", 1001, 3000, 1000);
        ownerB.release("CMD1");
        require(ownerB.isBlocked(3200), "release with mismatched cmdId should keep lease");
        ownerB.release("CMD2");
        require(!ownerB.isBlocked(3200), "release with matching cmdId should clear lease");

        std::remove(path.c_str());
        std::cout << "priority_control_lease_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::remove(path.c_str());
        std::cerr << "priority_control_lease_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
