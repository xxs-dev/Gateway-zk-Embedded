#include "edge_gateway/manual_recovery.hpp"
#include <iostream>
#include <stdexcept>
using edge_gateway::ManualRecovery;
using Inputs = ManualRecovery::Inputs;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Fixture {
    ManualRecovery latch;
    Inputs inputs;
    std::int64_t time = 1000;
    Fixture() {
        inputs.healthy = inputs.stopped = 1;
        inputs.fault = inputs.permit = inputs.reset = inputs.start = 0;
    }
    ManualRecovery::Result step() {
        time += 100;
        inputs.resetTs = inputs.startTs = time;
        return latch.update(inputs, time, 500);
    }
    void run() {
        require(step().state == ManualRecovery::Locked, "startup locked");
        inputs.reset = 1;
        require(step().state == ManualRecovery::Ready, "reset only arms");
        inputs.reset = 0;
        require(step().state == ManualRecovery::Ready, "observe start released after reset");
        inputs.permit = inputs.start = 1;
        require(step().permitted, "separate start edge grants permission");
        inputs.start = 0;
        inputs.stopped = 0;
        require(step().permitted, "moving during authorized run");
    }
};
int main() {
    try {
        {
            Fixture f;
            f.inputs.reset = f.inputs.start = f.inputs.permit = 1;
            for (int i = 0; i < 5; ++i) require(!f.step().permitted, "held boot commands cannot start");
        }
        {
            Fixture f; f.run();
            f.inputs.fault = 1;
            auto r = f.step();
            require(r.state == ManualRecovery::Locked && r.reason == ManualRecovery::Fault, "fault trips");
            f.inputs.fault = 0;
            for (int i = 0; i < 5; ++i) require(!f.step().permitted, "fault recovery must not restart");
            f.inputs.reset = 1;
            require(f.step().state == ManualRecovery::Locked, "reset rejected while moving and permitted");
            f.inputs.permit = 0; f.inputs.stopped = 1;
            require(f.step().state == ManualRecovery::Locked, "held rejected reset not retried");
            f.inputs.reset = 0; f.step();
            f.inputs.reset = 1;
            require(f.step().state == ManualRecovery::Ready, "fresh reset accepted after stopped");
            f.inputs.reset = 0; f.step();
            f.inputs.permit = f.inputs.start = 1;
            require(f.step().permitted, "fresh start accepted after reset");
        }
        for (int kind = 0; kind < 6; ++kind) {
            Fixture f; f.run();
            if (kind == 0) f.inputs.healthy = 0;
            if (kind == 1) f.inputs.fault = -1;
            if (kind == 2) f.inputs.permit = 0;
            if (kind == 3) f.inputs.reset = -1;
            if (kind == 4) f.inputs.start = -1;
            if (kind == 5) f.inputs.stopped = -1;
            require(!f.step().permitted, "invalid inputs and withdrawn permit trip");
            f.inputs.healthy = f.inputs.permit = f.inputs.stopped = 1;
            f.inputs.fault = f.inputs.reset = f.inputs.start = 0;
            require(!f.step().permitted, "valid data alone never restores permission");
        }
        {
            Fixture f; f.run();
            f.inputs.reset = f.inputs.start = 1;
            require(f.step().reason == ManualRecovery::ConflictingActions, "simultaneous actions trip");
        }
        for (int kind = 0; kind < 3; ++kind) {
            Fixture f; f.run();
            const auto now = kind == 0 ? f.time : kind == 1 ? f.time - 1 : f.time + 501;
            const auto r = f.latch.update(f.inputs, now, 500);
            require(!r.permitted && r.reason == ManualRecovery::ScanGap, "clock or scan discontinuity trips");
        }
        {
            Fixture f; f.step();
            f.inputs.reset = 1;
            // A changed value with the identical sample timestamp is not a fresh command edge.
            require(f.latch.update(f.inputs, f.time + 1, 500).state == ManualRecovery::Locked,
                    "same-timestamp command rejected");
            f.inputs.resetTs = f.time + 2;
            require(f.latch.update(f.inputs, f.time + 2, 500).state == ManualRecovery::Locked,
                    "rejected high command does not become an edge later");
        }
        {
            Fixture f; f.run();
            ManualRecovery restarted;
            require(!restarted.update(f.inputs, f.time + 1, 500).permitted,
                    "new process never inherits running authorization");
        }
        std::cout << "manual recovery lifecycle, fault, freshness and restart checks passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
