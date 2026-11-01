// AGC/AVC: a setpoint whose write result is unknown may have taken effect. The same value is never
// re-sent, independent of the deadband (including 0) and of PCS telemetry timing out. The transaction
// closes on a final receipt for the same cmdId, on telemetry (sampled after the command) reaching its
// own target, or when a different value is commanded; late results are accounted like on-time ones.
// The only exemption is the best-effort safe-state 0 for an unavailable PCS.
// Simulation with an in-memory point bus; no device, no shared memory.
#include "agc_avc_test_fixture.hpp"

#include "edge_gateway/agc_avc_service.hpp"
#include "edge_gateway/writeback_outcome.hpp"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <map>
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
    std::map<std::uint32_t, std::int64_t> sampleTime;  // fixed sample times, e.g. taken before a command

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
        for (const auto& fixed : sampleTime) bus.values[fixed.first].ts = fixed.second;
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

    const PendingWriteCommand& firstFor(std::uint32_t index) const {
        for (const auto& command : first)
            if (command.index == index) return command;
        throw std::runtime_error("no initial command for index " + std::to_string(index));
    }
    void answerOne(const std::string& cmdId, std::uint32_t index, bool success, const std::string& stage,
                   const std::string& receiptCmdId = std::string()) {
        WritebackResultRecord r;
        r.cmdId = receiptCmdId.empty() ? cmdId : receiptCmdId;
        r.index = index;
        r.success = success;
        r.stage = stage;
        r.completedAt = now + 50;
        normalizeWritebackOutcome(r);
        bus.results[cmdId] = r;
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


// ---- Late results: accounted exactly like on-time results ------------------------------------

void setDeadband(Rig& rig, double deadband, int feedbackTimeoutMs) {
    for (auto& pcs : rig.config.pcs) {
        pcs.writeDeadbandKw = pcs.writeDeadbandKvar = deadband;
        pcs.feedbackTimeoutMs = feedbackTimeoutMs;
    }
}

// Telemetry reaching the target only after the telemetry watch would have timed out still settles
// the unknown transactions, as a success, without a second failure count and without a retry.
void lateTelemetryAfterWatchTimeout(double deadband) {
    Rig rig("late-telemetry");
    setDeadband(rig, deadband, 100);
    rig.bus.values[13].value = 5.0;
    rig.start();
    rig.answer(false, kWritebackTimeoutStage);
    rig.tick(600);
    auto out = rig.tick(1200);
    require(rig.service->unknownWriteCount() == 2 && out.consecutiveWriteFailures == 2,
            "telemetry timeout: still unknown, counted once");
    rig.bus.values[12].value = 80.0;
    rig.bus.values[13].value = 0.0;
    out = rig.tick(1500);
    require(rig.service->unknownWriteCount() == 0, "late telemetry at the target settles the unknown transactions");
    require(out.consecutiveWriteFailures == 0 && out.lastWriteStatus == 1, "late telemetry is accounted as success");
    if (deadband > 0.0) require(!rig.resentOriginal(), "settled values inside the deadband are not written again");
    // Deadband 0 refreshes a confirmed setpoint every interval; that refresh is not a retry.
}

// A sample taken before the command was submitted is not evidence for this command.
void telemetrySampledBeforeCommand() {
    Rig rig("telemetry-old-sample");
    setDeadband(rig, 0.1, 1000);
    rig.bus.values[12].value = 80.0;
    rig.bus.values[13].value = 0.0;
    rig.sampleTime[12] = rig.sampleTime[13] = rig.now - 5;  // already at the target, but sampled earlier
    rig.start();
    rig.answer(false, kWritebackTimeoutStage);
    rig.tick(600);
    rig.tick(900);
    require(rig.service->unknownWriteCount() == 2, "a sample from before the command settled it");
    rig.sampleTime.clear();
    const auto out = rig.tick(1200);
    require(rig.service->unknownWriteCount() == 0 && out.lastWriteStatus == 1, "a fresh sample after the command settles it");
}

// Late success receipt for the same cmdId: same accounting as an on-time success. With all-zero
// targets and a failure threshold of 2 the write-failure failsafe ends on that evidence, as it does
// after an on-time success; nothing is replayed.
void lateSuccessReceipt(bool zeroTargetsFailsafe) {
    Rig rig(zeroTargetsFailsafe ? "late-ok-failsafe" : "late-ok");
    setDeadband(rig, 0.1, 0);
    if (zeroTargetsFailsafe) {
        rig.config.maxConsecutiveWriteFailures = 2;
        rig.bus.values[4].value = 0.0;
    }
    rig.start();
    rig.answer(false, kWritebackTimeoutStage);
    auto out = rig.tick(600);
    if (zeroTargetsFailsafe) require(out.state == AgcAvcRuntimeState::Failsafe, "threshold reached: failsafe");
    rig.tick(1200);
    rig.answer(true, kWritebackSucceededStage);
    out = rig.tick(1500);
    require(rig.service->unknownWriteCount() == 0, "late success settles the transactions");
    require(out.consecutiveWriteFailures == 0 && out.lastWriteStatus == 1, "late success is accounted as success");
    require(!rig.resentOriginal(), "late success replays nothing");
    if (zeroTargetsFailsafe) require(out.state != AgcAvcRuntimeState::Failsafe, "write-failure failsafe ends like after an on-time success");
}

// Late definite failure: not counted a second time, keeps the failed status, allows a new attempt.
void lateDefiniteFailureAccounting() {
    Rig rig("late-failed-accounting");
    setDeadband(rig, 0.1, 0);
    rig.start();
    rig.answer(false, kWritebackTimeoutStage);
    rig.tick(600);
    rig.answer(false, kWritebackFailedStage);
    const auto out = rig.tick(900);
    require(rig.service->unknownWriteCount() == 0, "late definite failure settles the transaction");
    require(out.consecutiveWriteFailures == 2 && out.lastWriteStatus == -1, "late definite failure: counted once, status failed");
    require(rig.sent(kActiveTarget, 80.0), "a new attempt is allowed after a definite failure");
}

// A record that names another command is ignored.
void receiptOfAnotherCommand() {
    Rig rig("late-other-cmd");
    setDeadband(rig, 0.1, 0);
    rig.start();
    rig.answer(false, kWritebackTimeoutStage);
    rig.tick(600);
    for (const auto& command : rig.first)
        rig.answerOne(command.cmdId, command.index, true, kWritebackSucceededStage, command.cmdId + "-OTHER");
    const auto out = rig.tick(900);
    require(rig.service->unknownWriteCount() == 2 && out.consecutiveWriteFailures == 2 && out.lastWriteStatus == -1,
            "a receipt of another command settled this one");
    require(!rig.resentOriginal(), "no re-send");
}

// A late success and a late definite failure of another point in the same cycle: no health reset.
void sameCycleFailureKeepsHealth() {
    Rig rig("late-same-cycle");
    setDeadband(rig, 0.1, 0);
    rig.start();
    rig.answer(false, kWritebackTimeoutStage);
    rig.tick(600);
    rig.answerOne(rig.firstFor(kActiveTarget).cmdId, kActiveTarget, true, kWritebackSucceededStage);
    rig.answerOne(rig.firstFor(kReactiveTarget).cmdId, kReactiveTarget, false, kWritebackFailedStage);
    const auto out = rig.tick(900);
    require(rig.service->unknownWriteCount() == 0, "both transactions settled");
    require(out.consecutiveWriteFailures == 2 && out.lastWriteStatus == -1,
            "a success must not clear a failure seen in the same cycle");
}

// A newer command on the same target replaces the old transaction; neither the old command's late
// receipt nor telemetry at the old target settles the new one.
void oldTransactionDoesNotSettleNew() {
    Rig rig("late-old-new");
    setDeadband(rig, 0.1, 1000);
    rig.bus.values[13].value = 5.0;  // reactive stays off target throughout
    rig.start();
    rig.answer(false, kWritebackTimeoutStage);
    rig.tick(600);
    rig.bus.values[4].value = 60.0;
    rig.bus.values[5].value = 2.0;
    rig.bus.values[6].value = static_cast<double>(rig.now + 650);
    rig.tick(900);
    require(rig.sent(kActiveTarget, 60.0), "new target commanded");
    std::string newCmd;
    for (const auto& command : rig.bus.submitted)
        if (command.index == kActiveTarget) newCmd = command.cmdId;
    rig.answerOne(rig.firstFor(kActiveTarget).cmdId, kActiveTarget, true, kWritebackSucceededStage);  // old: late success
    rig.answerOne(newCmd, kActiveTarget, false, kWritebackTimeoutStage);                              // new: unknown
    rig.bus.values[12].value = 80.0;  // telemetry at the old target
    rig.tick(1200);
    rig.tick(1500);
    require(rig.service->unknownWriteCount() == 2, "old receipt or old-target telemetry settled the new command");
    require(!rig.sent(kActiveTarget, 60.0) && !rig.sent(kActiveTarget, 80.0), "no re-send of either value");
    rig.bus.values[12].value = 60.0;  // telemetry at the new target
    const auto out = rig.tick(1800);
    require(rig.service->unknownWriteCount() == 1 && out.lastWriteStatus == 1, "new-target telemetry settles only the new command");
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
        {"late-telemetry-after-timeout-deadband", [] { lateTelemetryAfterWatchTimeout(0.1); }},
        {"late-telemetry-after-timeout-zero-deadband", [] { lateTelemetryAfterWatchTimeout(0.0); }},
        {"telemetry-sampled-before-command", telemetrySampledBeforeCommand},
        {"late-success-receipt", [] { lateSuccessReceipt(false); }},
        {"late-success-receipt-zero-targets-failsafe", [] { lateSuccessReceipt(true); }},
        {"late-definite-failure-accounting", lateDefiniteFailureAccounting},
        {"receipt-of-another-command", receiptOfAnotherCommand},
        {"same-cycle-failure-keeps-health", sameCycleFailureKeepsHealth},
        {"old-transaction-does-not-settle-new", oldTransactionDoesNotSettleNew},
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
