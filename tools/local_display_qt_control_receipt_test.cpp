// Local screen receipt tracking: submitted / written / feedback-confirmed / unknown / failed wording,
// late receipts, identity, "final is never replaced" and the per-point lock. Simulation only.
#include "local_display_qt_control_receipt.hpp"

#include <QApplication>
#include <QDateTime>
#include <QMessageBox>
#include <QThread>
#include <QWidget>

#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

namespace {

void require(bool ok, const std::string& what) {
    if (!ok) throw std::runtime_error(what);
}

std::int64_t now() { return QDateTime::currentMSecsSinceEpoch(); }

class FakeRuntime final : public ScadaSceneRuntimeSource {
public:
    const std::string& nodeId() const override { return node_; }
    edge_gateway::Optional<ScadaSceneResolvedTag> resolveTag(const std::string&) const override { return edge_gateway::NullOpt; }
    edge_gateway::Optional<edge_gateway::StoredPointValue> readTag(const std::string& tagId, std::int64_t) const override {
        const auto it = values.find(tagId);
        return it == values.end() ? edge_gateway::Optional<edge_gateway::StoredPointValue>()
                                  : edge_gateway::Optional<edge_gateway::StoredPointValue>(it->second);
    }
    std::vector<edge_gateway::StoredPointValue> readIndexes(const std::vector<std::uint32_t>&, std::int64_t) const override { return {}; }
    ScadaSceneWriteResult submitWrite(const std::string&, edge_gateway::PendingWriteCommand) override { ++writes; return {true, "ok"}; }
    ScadaSceneWriteResult submitWriteGroup(const std::vector<ScadaSceneWriteTarget>&, edge_gateway::PendingWriteCommand) override { ++writes; return {true, "ok"}; }
    edge_gateway::Optional<edge_gateway::WritebackResultRecord> getWritebackResult(const std::string&, const std::string&) const override {
        return receipt;
    }
    edge_gateway::Optional<edge_gateway::WritebackResultRecord> receipt;
    std::map<std::string, edge_gateway::StoredPointValue> values;
    int writes = 0;
private:
    std::string node_ = "edge-1";
};

edge_gateway::WritebackResultRecord makeReceipt(const std::string& cmdId, bool success, const std::string& stage,
                                                const std::string& message = "fixture") {
    edge_gateway::WritebackResultRecord r;
    r.cmdId = cmdId;
    r.index = 1201;
    r.success = success;
    r.stage = stage;
    r.message = message;
    r.completedAt = now();
    edge_gateway::normalizeWritebackOutcome(r);
    return r;
}

struct Run {
    QWidget host;
    FakeRuntime runtime;
    ControlReceiptTracker::Request request;
    bool waitEnded = false;
    explicit Run(const std::string& cmdId) {
        request.tagId = "PCS.start";
        request.index = 1201;
        request.cmdId = cmdId;
        request.value = 1;
        request.commandTs = now();
        request.waitMs = 300;
        request.trackMs = 1500;
    }
    QPointer<ControlReceiptTracker> start() {
        return ControlReceiptTracker::start(&host, runtime, request, [this] { waitEnded = true; });
    }
    QString text() const {
        const auto boxes = host.findChildren<QMessageBox*>(QStringLiteral("controlReceiptDialog"));
        require(!boxes.empty(), "receipt dialog missing");
        return boxes.back()->text();
    }
    QMessageBox::Icon icon() const {
        return host.findChildren<QMessageBox*>(QStringLiteral("controlReceiptDialog")).back()->icon();
    }
};

void spin(QApplication& app, int ms) {
    const auto until = now() + ms;
    while (now() < until) {
        app.processEvents();
        QThread::msleep(5);
    }
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        { // submitted: never a confirmation
            Run run("C1");
            run.runtime.receipt = makeReceipt("C1", true, edge_gateway::kWritebackSubmittedStage);
            auto tracker = run.start();
            spin(app, 250);
            require(run.text().contains(QString::fromUtf8("已下发，未确认")), "submitted wording");
            require(run.icon() == QMessageBox::Warning, "submitted is not shown as success");
            require(run.waitEnded && !ControlReceiptTracker::tagUnresolved("PCS.start"), "final receipt releases the point");
        }
        { // written without verification
            Run run("C2");
            run.runtime.receipt = makeReceipt("C2", true, edge_gateway::kWritebackSucceededStage);
            run.start();
            spin(app, 250);
            require(run.text().contains(QString::fromUtf8("协议写入成功，动作未确认")), "written wording");
        }
        { // verified read-back / feedback
            Run run("C3");
            auto r = makeReceipt("C3", true, edge_gateway::kWritebackSucceededStage, "触点反馈(index 998)=1");
            r.verifyAttempted = r.verifyPassed = true;
            run.runtime.receipt = r;
            run.start();
            spin(app, 250);
            require(run.text().contains(QString::fromUtf8("反馈确认成功")) && run.text().contains("998"),
                    "feedback confirmation names its source");
            require(run.icon() == QMessageBox::Information, "confirmed action is positive");
        }
        { // unknown outcome is not a failure
            Run run("C4");
            run.runtime.receipt = makeReceipt("C4", false, edge_gateway::kWritebackMismatchStage);
            run.start();
            spin(app, 250);
            require(run.text().contains(QString::fromUtf8("结果未知")) && run.text().contains(QString::fromUtf8("不一致")) &&
                    !run.text().contains(QString::fromUtf8("执行失败")), "mismatch is unknown, not failed");
        }
        { // definite failure shows the reason
            Run run("C5");
            run.runtime.receipt = makeReceipt("C5", false, edge_gateway::kWritebackFailedStage, "illegal data value");
            run.start();
            spin(app, 250);
            require(run.text().contains(QString::fromUtf8("执行失败")) && run.text().contains("illegal data value"),
                    "failure shows the actual reason");
        }
        { // a receipt of another command is ignored; no receipt -> unknown; late final replaces it
            Run run("C6");
            run.runtime.receipt = makeReceipt("OTHER", true, edge_gateway::kWritebackSucceededStage);
            auto tracker = run.start();
            spin(app, 150);
            require(run.text().contains(QString::fromUtf8("等待")) && !run.waitEnded, "foreign receipt must not complete this command");
            spin(app, 300);
            require(run.waitEnded && run.text().contains(QString::fromUtf8("等待回执超时，结果未知")),
                    "missing receipt by the wait time is unknown");
            require(ControlReceiptTracker::tagUnresolved("PCS.start"), "unknown result keeps the point locked");
            run.runtime.receipt = makeReceipt("C6", true, edge_gateway::kWritebackSucceededStage);
            spin(app, 300);
            require(run.text().startsWith(QString::fromUtf8("晚到回执：")) &&
                    run.text().contains(QString::fromUtf8("协议写入成功")), "late final receipt replaces the unknown view");
            require(!ControlReceiptTracker::tagUnresolved("PCS.start"), "late final receipt releases the point");
        }
        { // the final result is never replaced by a later contradicting record
            Run run("C7");
            run.runtime.receipt = makeReceipt("C7", false, edge_gateway::kWritebackFailedStage, "rejected");
            run.start();
            spin(app, 250);
            require(run.text().contains("rejected"), "first final result shown");
            run.runtime.receipt = makeReceipt("C7", true, edge_gateway::kWritebackSucceededStage);
            spin(app, 450);
            require(run.text().contains("rejected") && !run.text().contains(QString::fromUtf8("协议写入成功")),
                    "a later record never replaces the final result");
        }
        { // no receipt at all: tracking ends unknown, point released, nothing re-sent
            Run run("C8");
            run.start();
            spin(app, 1700);
            require(run.text().contains(QString::fromUtf8("仍无最终回执，结果未知")), "end of tracking stays unknown");
            require(!ControlReceiptTracker::tagUnresolved("PCS.start"), "point released after tracking");
            require(run.runtime.writes == 0, "tracking never re-sends");
        }
        { // screen feedback point: only a value newer than command and receipt confirms
            Run run("C9");
            run.request.feedbackTag = "PCS.running";
            run.request.feedbackValue = 1;
            run.request.waitMs = 600;
            run.runtime.receipt = makeReceipt("C9", true, edge_gateway::kWritebackSucceededStage);
            edge_gateway::StoredPointValue old;
            old.value = 1; old.quality = 1; old.ts = run.request.commandTs - 10;
            run.runtime.values["PCS.running"] = old;
            run.start();
            spin(app, 250);
            require(run.text().contains(QString::fromUtf8("等待命令之后的新值")), "old feedback value does not confirm");
            auto fresh = old;
            fresh.ts = now() + 1;
            run.runtime.values["PCS.running"] = fresh;
            spin(app, 250);
            require(run.text().contains(QString::fromUtf8("新鲜反馈与目标一致")) && run.icon() == QMessageBox::Information,
                    "fresh equal feedback confirms");
        }
    } catch (const std::exception& ex) {
        std::cerr << "local_display_qt_control_receipt_test failed: " << ex.what() << '\n';
        return 1;
    }
    std::cout << "local_display_qt_control_receipt_test passed\n";
    return 0;
}
