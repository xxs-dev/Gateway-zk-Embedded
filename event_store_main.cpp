#include "edge_gateway/event_store_runtime.hpp"

#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

namespace {
volatile std::sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }
}

int main(int argc, char** argv) {
    if (argc != 3 || std::string(argv[1]) != "--lab-config") {
        std::cerr << "Usage: EventStore --lab-config FILE\nLaboratory-only; not a production migration tool.\n";
        return 2;
    }
    try {
        auto options = edge_gateway::loadEventStoreLabConfig(argv[2]);
        edge_gateway::EventStoreRuntime runtime(std::move(options));
        std::signal(SIGTERM, stop);
        std::signal(SIGINT, stop);
        runtime.start();
        std::cout << "READY laboratory event store; history projection "
                  << (runtime.historyStatus().enabled ? "enabled (observe GetHistoryProjectionStatus)" : "disabled") << std::endl;
        while (!stopped && runtime.ready()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        runtime.stop();
        return stopped ? 0 : 1;
    } catch (const std::exception& ex) {
        std::cerr << "EventStore: " << ex.what() << std::endl;
        return 1;
    }
}
