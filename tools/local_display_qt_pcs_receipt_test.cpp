// PCS three-phase power control: the summary keeps every receipt class apart (submitted-only, local
// commit, protocol write, feedback confirmation, unknown, failure), and the phase locks belong to
// localControlTransactions(): destroying the widget (logout, session expiry, project reload) neither
// leaves a permanent lock nor releases an open batch early. Simulation only, no device.
#include "local_display_qt_control_receipt.hpp"
#include "local_display_qt_pcs_power_control.hpp"

#include <QApplication>
#include <QDateTime>
#include <QLabel>
#include <QMessageBox>
#include <QThread>
#include <QTimer>

#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

class PcsPhasePowerControlTestAccess {
public:
    static void submitActive(PcsPhasePowerControl& control, double perPhase) {
        PcsPhasePowerPlan plan;
        plan.total = perPhase * 3;
        plan.phases = {{perPhase, perPhase, perPhase}};
        control.submitWrite(PcsPhasePowerControl::PowerKind::Active, plan);
    }
    static void poll(PcsPhasePowerControl& control) { control.pollWriteback(); }
    static QString status(const PcsPhasePowerControl& control) { return control.statusLabel_->text(); }
    static std::string cmdId(const PcsPhasePowerControl& control) { return control.pending_.cmdId; }
    static bool pending(const PcsPhasePowerControl& control) { return control.pending_.active; }
};

namespace {

void require(bool ok, const std::string& what) {
    if (!ok) throw std::runtime_error(what);
}

const std::array<std::string, 3> kPhases{{"PCS.PA", "PCS.PB", "PCS.PC"}};

class PhaseRuntime final : public ScadaSceneRuntimeSource {
public:
    const std::string& nodeId() const override { return node_; }
    edge_gateway::Optional<ScadaSceneResolvedTag> resolveTag(const std::string&) const override { return edge_gateway::NullOpt; }
    edge_gateway::Optional<edge_gateway::StoredPointValue> readTag(const std::string&, std::int64_t) const override {
        return edge_gateway::NullOpt;
    }
    std::vector<edge_gateway::StoredPointValue> readIndexes(const std::vector<std::uint32_t>&, std::int64_t) const override { return {}; }
    ScadaSceneWriteResult submitWrite(const std::string&, edge_gateway::PendingWriteCommand) override { ++writes; return {true, "ok"}; }
    ScadaSceneWriteResult submitWriteGroup(const std::vector<ScadaSceneWriteTarget>&, edge_gateway::PendingWriteCommand) override {
        ++writes;
        return {true, "ok"};
    }
    edge_gateway::Optional<edge_gateway::WritebackResultRecord> getWritebackResult(const std::string& tagId, const std::string& cmdId) const override {
        const auto it = receipts.find(tagId);
        if (it == receipts.end() || it->second.cmdId != cmdId) return edge_gateway::NullOpt;
        return it->second;
    }
    // stage per phase; "" = no receipt; "confirmed" = succeeded with verifyPassed
    void answer(const std::string& cmdId, const std::array<std::string, 3>& stages) {
        receipts.clear();
        for (std::size_t i = 0; i < 3; ++i) {
            if (stages[i].empty()) continue;
            edge_gateway::WritebackResultRecord r;
            r.cmdId = cmdId;
            r.index = static_cast<std::uint32_t>(1201 + i);
            r.stage = stages[i] == "confirmed" ? edge_gateway::kWritebackSucceededStage : stages[i];
            r.success = stages[i] == "confirmed" || stages[i] == edge_gateway::kWritebackSucceededStage ||
                        stages[i] == edge_gateway::kWritebackSubmittedStage || stages[i] == "local-parameter-committed";
            r.verifyAttempted = r.verifyPassed = stages[i] == "confirmed";
            r.message = "fixture";
            r.completedAt = QDateTime::currentMSecsSinceEpoch();
            edge_gateway::normalizeWritebackOutcome(r);
            receipts[kPhases[i]] = r;
        }
    }
    std::map<std::string, edge_gateway::WritebackResultRecord> receipts;
    int writes = 0;
private:
    std::string node_ = "edge-1";
};

PcsPhasePowerBindings bindings() {
    PcsPhasePowerBindings b;
    b.activeTagIds = kPhases;
    b.reactiveTagIds = {{"PCS.QA", "PCS.QB", "PCS.QC"}};
    return b;
}

void windowPoll(PhaseRuntime& runtime) {
    localControlTransactions().poll([&runtime](const std::string& tagId, const std::string& cmdId) {
        return runtime.getWritebackResult(tagId, cmdId);
    });
}

bool anyPhaseLocked() {
    for (const auto& tag : kPhases)
        if (ControlReceiptTracker::tagUnresolved(tag)) return true;
    return false;
}

// Closes any message box (lock refusals, the result dialog) so that exec() returns.
struct Responder {
    QTimer timer;
    int closed = 0;
    QString lastText;
    Responder() {
        QObject::connect(&timer, &QTimer::timeout, [this]() {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                lastText = box->text();
                ++closed;
                box->done(0);
            }
        });
        timer.start(10);
    }
};

QString runSummary(PhaseRuntime& runtime, const std::array<std::string, 3>& stages) {
    PcsPhasePowerControl control(bindings(), runtime);
    PcsPhasePowerControlTestAccess::submitActive(control, 1.0);
    runtime.answer(PcsPhasePowerControlTestAccess::cmdId(control), stages);
    PcsPhasePowerControlTestAccess::poll(control);
    require(!PcsPhasePowerControlTestAccess::pending(control), "all phases final ends the batch");
    return PcsPhasePowerControlTestAccess::status(control);
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    Responder responder;
    std::int64_t clock = 1000000;
    localControlTransactions().setClock([&clock] { return clock; });
    try {
        using namespace edge_gateway;
        PhaseRuntime runtime;
        {   // only submitted: never reported as a protocol write
            const auto text = runSummary(runtime, {{kWritebackSubmittedStage, kWritebackSubmittedStage, kWritebackSubmittedStage}});
            require(text.contains(QString::fromUtf8("3 相已下发、未确认")) && !text.contains(QString::fromUtf8("协议写入成功")),
                    "submitted-only phases reported as protocol write: " + text.toStdString());
            require(!anyPhaseLocked(), "definite submitted receipts release the phases");
        }
        {   // mixed: confirmed / written / submitted
            const auto text = runSummary(runtime, {{"confirmed", kWritebackSucceededStage, kWritebackSubmittedStage}});
            for (const char* part : {"1 相反馈确认成功", "1 相协议写入成功、动作未确认", "1 相已下发、未确认"})
                require(text.contains(QString::fromUtf8(part)), std::string("mixed summary lacks ") + part + ": " + text.toStdString());
            require(!text.contains(QString::fromUtf8("三相写入均经")), "mixed batch reported as fully confirmed");
        }
        {   // failure, unknown and local commit stay apart; the unknown phase keeps its lock
            const auto text = runSummary(runtime, {{kWritebackFailedStage, kWritebackTimeoutStage, "local-parameter-committed"}});
            for (const char* part : {"1 相执行失败", "1 相结果未知", "1 相本地参数已提交"})
                require(text.contains(QString::fromUtf8(part)), std::string("summary lacks ") + part + ": " + text.toStdString());
            require(!ControlReceiptTracker::tagUnresolved(kPhases[0]) && ControlReceiptTracker::tagUnresolved(kPhases[1]) &&
                    !ControlReceiptTracker::tagUnresolved(kPhases[2]), "only the unknown phase stays locked");
            clock += 70000;
            require(!anyPhaseLocked(), "the unknown phase is released at the end of the tracking period");
        }
        {   // all confirmed
            const auto text = runSummary(runtime, {{"confirmed", "confirmed", "confirmed"}});
            require(text.contains(QString::fromUtf8("3 相反馈确认成功")), "confirmed batch: " + text.toStdString());
        }
        {   // widget destroyed with an open batch: locks stay, a rebuilt widget cannot bypass them,
            // the late receipt settles them through the window poll
            runtime.receipts.clear();
            auto* control = new PcsPhasePowerControl(bindings(), runtime);
            PcsPhasePowerControlTestAccess::submitActive(*control, 2.0);
            const auto cmdId = PcsPhasePowerControlTestAccess::cmdId(*control);
            delete control;
            windowPoll(runtime);
            require(anyPhaseLocked(), "destroying the widget released an open batch");
            const auto writes = runtime.writes;
            PcsPhasePowerControl rebuilt(bindings(), runtime);
            const auto refusals = responder.closed;
            PcsPhasePowerControlTestAccess::submitActive(rebuilt, 2.0);
            require(runtime.writes == writes && responder.closed == refusals + 1 &&
                    responder.lastText.contains(QString::fromStdString(cmdId)),
                    "a rebuilt widget (re-login) bypassed the open batch");
            runtime.answer(cmdId, {{kWritebackSucceededStage, kWritebackSucceededStage, kWritebackSucceededStage}});
            windowPoll(runtime);
            require(!anyPhaseLocked(), "the late receipt settled the batch without its widget");
        }
        {   // widget destroyed, no receipt ever: released exactly at the deadline (no permanent lock)
            runtime.receipts.clear();
            auto* control = new PcsPhasePowerControl(bindings(), runtime);
            PcsPhasePowerControlTestAccess::submitActive(*control, 3.0);
            delete control;
            clock += 69999;
            windowPoll(runtime);
            require(anyPhaseLocked(), "locked within the tracking period");
            clock += 1;
            require(!anyPhaseLocked(), "a destroyed widget left a permanent lock");
        }
    } catch (const std::exception& ex) {
        std::cerr << "local_display_qt_pcs_receipt_test failed: " << ex.what() << '\n';
        return 1;
    }
    std::cout << "local_display_qt_pcs_receipt_test passed\n";
    return 0;
}
