// AGC/AVC: a setpoint whose write result is unknown may have taken effect. The same value is never
// re-sent, independent of the deadband (including 0) and of PCS telemetry timing out. The transaction
// closes on a final receipt for the same cmdId, on telemetry reaching the target, or when a different
// value is commanded. The only exemption is the best-effort safe-state 0 for an unavailable PCS.
// Simulation with an in-memory point bus; no device, no shared memory.
#include "agc_avc_test_fixture.hpp"

#include "edge_gateway/agc_avc_service.hpp"
#include "edge_gateway/writeback_outcome.hpp"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {

using namespace edge_gateway;
using agc_test::FakePointBus;

constexpr std::uint32_t kActiveTarget = 16;
constexpr std::uint32_t kReactiveTarget = 17;

void require(bool ok, const std::string& what) {
    if (!ok) throw std::runtime_error(what);
}

struct Rig {
    AgcAvcConfig config;
    FakePointBus bus;
    std::int64_t now = 50000;
    std::string lease;
    std::unique_ptr<AgcAvcService> service;
    std::vector<PendingWriteCommand> first;

    explicit Rig(const std::string& name) {
        config = agc_test::baseConfig();
        agc_test::configureServicePoints(config, bus, now);
        bus.values[4].value = 80.0;  // 80 kW active target; reactive target stays 0 kvar
        config.shadowMode = false;
        config.submitWrites = true;
        config.writeResultTimeoutMs = 100;
        config.maxConsecutiveWriteFailures = 10;
        lease = "/tmp/agc_unknown_" + name + "_" + std::to_string(::getpid()) + ".json";
        std::remove(lease.c_str());
        config.ownership.leaseFile = lease;
        for (auto& pcs : config.pcs) pcs.feedbackTimeoutMs = 0;
    }
    ~Rig() {
        service.reset();
        std::remove(lease.c_str());
        std::remove((lease + ".lock").c_str());
    }

    AgcAvcCycleOutput tick(std::int64_t offsetMs) {
        for (auto& entry : bus.values) entry.second.ts = now + offsetMs - 10;
        bus.submitted.clear();
        return service->tick(now + offsetMs);
    }

    // First cycle: both targets submitted.
    void start() {
        service.reset(new AgcAvcService(config, bus));
        tick(1);
        require(bus.submitted.size() == 2, "initial active and reactive targets");
        first = bus.submitted;
    }

    void answer(bool success, const std::string& stage) {
        for (const auto& command : first) {
            WritebackResultRecord r;
            r.cmdId = command.cmdId;
            r.index = command.index;
            r.value = command.value;
            r.success = success;
            r.stage = stage;
            r.completedAt = now + 50;
            normalizeWritebackOutcome(r);
            bus.results[command.cmdId] = r;
        }
    }

    bool sent(std::uint32_t index, double value) const {
        for (const auto& command : bus.submitted)
            if (command.index == index && std::abs(command.value - value) < 1e-9) return true;
        return false;
    }
    bool resentOriginal() const {
        for (const auto& original : first)
            if (sent(original.index, original.value)) return true;
        return false;
    }
};

void requireNoResend(Rig& rig, std::int64_t from, std::int64_t to, const std::string& what) {
    for (std::int64_t t = from; t <= to; t += 300) {
        rig.tick(t);
        require(!rig.resentOriginal(), what + " (t=+" + std::to_string(t) + " ms)");
    }
}

void unknownReceiptNotResent(double deadband) {
    Rig rig("receipt");
    for (auto& pcs : rig.config.pcs) pcs.writeDeadbandKw = pcs.writeDeadbandKvar = deadband;
    rig.start();
    rig.answer(false, kWritebackTimeoutStage);
    const auto out = rig.tick(600);
    require(!rig.resentOriginal(), "unknown receipt: same value re-sent, deadband " + std::to_string(deadband));
    require(out.lastWriteStatus == -1 && out.consecutiveWriteFailures == 2, "each unknown result counts once");
    requireNoResend(rig, 900, 3000, "unknown receipt: same value re-sent later, deadband " + std::to_string(deadband));
}

void mismatchNotResent() {
    Rig rig("mismatch");
    for (auto& pcs : rig.config.pcs) pcs.writeDeadbandKw = pcs.writeDeadbandKvar = 0.0;
    rig.start();
    rig.answer(false, kWritebackMismatchStage);
    requireNoResend(rig, 600, 2400, "feedback mismatch is unknown: same value re-sent");
}

void noReceiptNotResent() {
    Rig rig("noreceipt");
    for (auto& pcs : rig.config.pcs) pcs.writeDeadbandKw = pcs.writeDeadbandKvar = 0.0;
    rig.start();
    requireNoResend(rig, 600, 2400, "no receipt by writeResultTimeoutMs: same value re-sent");
}

// Telemetry not reaching the target after an unknown result does not prove that the command was not
// executed: no re-send, and the transaction is not counted a second time.
void feedbackTimeoutKeepsUnknown(double deadband) {
    Rig rig("feedback");
    for (auto& pcs : rig.config.pcs) {
        pcs.feedbackTimeoutMs = 100;
        pcs.writeDeadbandKw = pcs.writeDeadbandKvar = deadband;
    }
    rig.bus.values[13].value = 5.0;  // reactive telemetry off target too
    rig.start();
    rig.answer(false, kWritebackTimeoutStage);
    rig.tick(600);      // unknown registered, telemetry watch starts
    const auto out = rig.tick(1200);  // telemetry watch times out
    require(!rig.resentOriginal(), "telemetry timeout re-sent an unknown setpoint, deadband " + std::to_string(deadband));
    require(out.consecutiveWriteFailures == 2, "telemetry timeout must not count an unknown result twice");
    requireNoResend(rig, 1500, 3600, "telemetry timeout re-sent an unknown setpoint later");
}

void telemetryReachingTargetResolves() {
    Rig rig("telemetry");
    for (auto& pcs : rig.config.pcs) {
        pcs.feedbackTimeoutMs = 1000;
        pcs.writeDeadbandKw = pcs.writeDeadbandKvar = 0.1;
    }
    rig.start();
    rig.answer(false, kWritebackTimeoutStage);
    rig.tick(600);
    rig.bus.values[12].value = 80.0;  // PCS reports the commanded power
    const auto out = rig.tick(800);
    require(out.consecutiveWriteFailures == 0 && out.lastWriteStatus == 1, "telemetry at target confirms execution");
    require(!rig.resentOriginal(), "a confirmed setpoint inside the deadband is not re-sent");
}

void lateFinalReceipt() {
    {   // late definite failure: not executed, a new attempt is allowed
        Rig rig("late-failed");
        for (auto& pcs : rig.config.pcs) pcs.writeDeadbandKw = pcs.writeDeadbandKvar = 0.1;
        rig.start();
        rig.answer(false, kWritebackTimeoutStage);
        rig.tick(600);
        require(!rig.resentOriginal(), "unknown before the late receipt");
        rig.answer(false, kWritebackFailedStage);
        rig.tick(900);
        require(rig.sent(kActiveTarget, 80.0), "a late definite failure releases the setpoint for a new attempt");
        for (const auto& command : rig.bus.submitted)
            for (const auto& original : rig.first)
                require(command.cmdId != original.cmdId, "a new attempt carries a new cmdId");
    }
    {   // late success: the transaction closes. Deadband 0 refreshes a confirmed setpoint every
        // interval, so the same value may follow as an ordinary refresh, not as a retry.
        Rig rig("late-ok");
        for (auto& pcs : rig.config.pcs) pcs.writeDeadbandKw = pcs.writeDeadbandKvar = 0.0;
        rig.start();
        rig.answer(false, kWritebackTimeoutStage);
        rig.tick(600);
        require(!rig.resentOriginal(), "unknown before the late success");
        rig.answer(true, kWritebackSucceededStage);
        rig.tick(900);
        require(rig.sent(kActiveTarget, 80.0), "after a late success the zero-deadband refresh resumes");
    }
    {   // a later record that is still unknown keeps the transaction open
        Rig rig("late-unknown");
        for (auto& pcs : rig.config.pcs) pcs.writeDeadbandKw = pcs.writeDeadbandKvar = 0.0;
        rig.start();
        rig.answer(false, kWritebackUnconfirmedStage);
        rig.tick(600);  // ledger replay past writeResultTimeoutMs: unknown
        requireNoResend(rig, 900, 1800, "an unconfirmed ledger replay must not release the setpoint");
    }
}

void definiteFailureMayRetry() {
    Rig rig("failed");
    rig.start();
    rig.answer(false, kWritebackFailedStage);
    rig.tick(600);
    require(rig.sent(kActiveTarget, 80.0), "a definitely failed setpoint may be attempted again");
}

void differentValueSupersedes() {
    Rig rig("supersede");
    for (auto& pcs : rig.config.pcs) pcs.writeDeadbandKw = pcs.writeDeadbandKvar = 0.0;
    rig.start();
    rig.answer(false, kWritebackTimeoutStage);
    rig.tick(600);
    rig.bus.values[4].value = 60.0;
    rig.bus.values[5].value = 2.0;
    rig.bus.values[6].value = static_cast<double>(rig.now + 650);
    rig.tick(900);
    require(rig.sent(kActiveTarget, 60.0), "a new target value is a new command");
    require(!rig.sent(kActiveTarget, 80.0), "the unknown value itself is not repeated");
}

// Repeated unknown results reach maxConsecutiveWriteFailures: the controller leaves Active, and the
// unknown values are still not repeated. A failsafe 0 equal to an unknown 0 is not re-sent either;
// only the unavailable-PCS safe state is exempt (see below).
void failureThreshold() {
    Rig rig("threshold");
    rig.config.maxConsecutiveWriteFailures = 2;
    for (auto& pcs : rig.config.pcs) pcs.writeDeadbandKw = pcs.writeDeadbandKvar = 0.0;
    rig.start();
    rig.answer(false, kWritebackTimeoutStage);
    const auto out = rig.tick(600);
    require(out.state != AgcAvcRuntimeState::Active, "repeated unknown results leave Active");
    require(!rig.resentOriginal(), "failsafe must not repeat the unknown values");
    requireNoResend(rig, 900, 1800, "failsafe must not repeat the unknown values later");
}

// Safety exception kept as before: an enabled PCS that becomes unavailable receives the best-effort
// safe-state 0, even when 0 is the value of an open unknown transaction.
void unavailablePcsZeroIsExempt() {
    Rig rig("zero");
    rig.config.zeroUnavailablePcs = true;
    for (auto& pcs : rig.config.pcs) pcs.writeDeadbandKw = pcs.writeDeadbandKvar = 0.1;
    rig.start();
    rig.answer(false, kWritebackTimeoutStage);
    rig.tick(600);
    rig.bus.values[11].quality = 0;  // SOC unusable: PCS unavailable
    for (auto& entry : rig.bus.values) entry.second.ts = rig.now + 1190;
    rig.bus.values[11].quality = 0;
    rig.bus.submitted.clear();
    rig.service->tick(rig.now + 1200);
    require(rig.sent(kActiveTarget, 0.0), "unavailable PCS: safe-state 0 kW");
    require(rig.sent(kReactiveTarget, 0.0), "unavailable PCS: safe-state 0 kvar even though 0 kvar was unknown");
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::pair<std::string, void (*)()>> cases = {
        {"unknown-receipt-deadband", [] { unknownReceiptNotResent(0.1); }},
        {"unknown-receipt-zero-deadband", [] { unknownReceiptNotResent(0.0); }},
        {"mismatch", mismatchNotResent},
        {"no-receipt", noReceiptNotResent},
        {"feedback-timeout-deadband", [] { feedbackTimeoutKeepsUnknown(0.1); }},
        {"feedback-timeout-zero-deadband", [] { feedbackTimeoutKeepsUnknown(0.0); }},
        {"telemetry-resolves", telemetryReachingTargetResolves},
        {"late-final-receipt", lateFinalReceipt},
        {"definite-failure-retry", definiteFailureMayRetry},
        {"different-value", differentValueSupersedes},
        {"failure-threshold", failureThreshold},
        {"unavailable-pcs-zero", unavailablePcsZeroIsExempt},
    };
    // Every case runs and is reported; an optional argument selects one case.
    int failures = 0;
    for (const auto& entry : cases) {
        if (argc > 1 && entry.first != argv[1]) continue;
        try {
            entry.second();
            std::cout << "PASS " << entry.first << '\n';
        } catch (const std::exception& ex) {
            ++failures;
            std::cout << "FAIL " << entry.first << ": " << ex.what() << '\n';
        }
    }
    if (failures != 0) {
        std::cerr << "agc_avc_unknown_result_test failed: " << failures << " case(s)\n";
        return 1;
    }
    std::cout << "agc_avc_unknown_result_test passed\n";
    return 0;
}
