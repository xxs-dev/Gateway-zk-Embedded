// Operator wording of control receipts: submitted / written-unconfirmed / feedback-confirmed / unknown /
// failed, identity of the receipt, and that a final result is never replaced by an older one.
#include "edge_gateway/control_receipt_view.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

using namespace edge_gateway;

namespace {

void require(bool ok, const std::string& what) {
    if (!ok) throw std::runtime_error(what);
}

WritebackResultRecord receipt(bool success, const std::string& stage) {
    WritebackResultRecord r;
    r.cmdId = "SCADA_LOCAL_1";
    r.index = 1201;
    r.success = success;
    r.stage = stage;
    r.message = "fixture";
    r.completedAt = 2000;
    normalizeWritebackOutcome(r);
    return r;
}

Optional<WritebackResultRecord> some(const WritebackResultRecord& r) {
    return Optional<WritebackResultRecord>(r);
}

StoredPointValue sample(double value, std::int64_t ts, int quality = 1, bool stale = false) {
    StoredPointValue v;
    v.value = value;
    v.ts = ts;
    v.quality = quality;
    v.stale = stale;
    return v;
}

}  // namespace

int main() {
    try {
        const Optional<WritebackResultRecord> none;
        auto v = classifyControlReceipt(none, false);
        require(v.state == ControlReceiptState::Waiting && !v.final, "no receipt yet is waiting, not final");
        v = classifyControlReceipt(none, true);
        require(v.state == ControlReceiptState::NoReceipt && !v.final, "no receipt by the deadline is unknown and not final");
        require(std::string(controlReceiptStateText(v)).find("结果未知") != std::string::npos, "missing receipt says unknown");

        auto submitted = receipt(true, kWritebackSubmittedStage);
        v = classifyControlReceipt(some(submitted), false);
        require(v.state == ControlReceiptState::Submitted && v.final, "writeback-submitted is submitted only");
        require(!controlReceiptPositive(v), "submitted is never shown as a confirmed action");
        submitted.verifyAttempted = submitted.verifyPassed = true;  // inconsistent producer: stage wins
        require(classifyControlReceipt(some(submitted), false).state == ControlReceiptState::Submitted,
                "a submitted stage is never upgraded to a confirmation");

        auto written = receipt(true, kWritebackSucceededStage);
        v = classifyControlReceipt(some(written), false);
        require(v.state == ControlReceiptState::Written && !controlReceiptPositive(v),
                "protocol write without verification: written, action unconfirmed");
        written.verifyAttempted = true;
        written.verifyPassed = false;
        require(classifyControlReceipt(some(written), false).state == ControlReceiptState::Written,
                "verifyPassed=false is not a confirmation");
        written.verifyPassed = true;
        v = classifyControlReceipt(some(written), false);
        require(v.state == ControlReceiptState::FeedbackConfirmed && controlReceiptPositive(v),
                "verifyPassed=true confirms within the scope of its feedback source");

        v = classifyControlReceipt(some(receipt(true, "local-parameter-committed")), false);
        require(v.state == ControlReceiptState::Committed, "local commit is not a device action");

        for (const auto* stage : {kWritebackTimeoutStage, kWritebackMismatchStage}) {
            v = classifyControlReceipt(some(receipt(false, stage)), false);
            require(v.state == ControlReceiptState::Unknown && v.final, std::string("unknown outcome: ") + stage);
            require(std::string(controlReceiptStateText(v)).find("失败") == std::string::npos,
                    "an unknown outcome is never worded as a failure");
            require(std::string(controlReceiptStateText(v)).find("未自动重发") != std::string::npos,
                    "an unknown outcome states that nothing is re-sent");
        }
        require(classifyControlReceipt(some(receipt(false, kWritebackMismatchStage)), false).feedbackMismatch,
                "mismatch keeps its cause");

        v = classifyControlReceipt(some(receipt(false, kWritebackFailedStage)), false);
        require(v.state == ControlReceiptState::Failed && v.message == "fixture", "definite failure keeps the reason");
        v = classifyControlReceipt(some(receipt(false, "control-rejected")), false);
        require(v.state == ControlReceiptState::Failed, "a rejection is a definite failure");

        // A ledger replay without a recorded result is not final; the final result may follow.
        auto unconfirmed = receipt(false, kWritebackUnconfirmedStage);
        v = classifyControlReceipt(some(unconfirmed), false);
        require(v.state == ControlReceiptState::Waiting && !v.final, "unconfirmed ledger replay is not final");
        require(classifyControlReceipt(some(unconfirmed), true).state == ControlReceiptState::NoReceipt,
                "unconfirmed ledger replay past the deadline is unknown");

        // Ordering: a final result replaces a non-final one, nothing replaces a final one.
        const auto noReceipt = classifyControlReceipt(none, true);
        const auto late = classifyControlReceipt(some(written), false);
        require(controlReceiptShouldReplace(noReceipt, late), "late final result replaces the timeout view");
        require(!controlReceiptShouldReplace(late, noReceipt), "an older non-final view never replaces the final one");
        require(!controlReceiptShouldReplace(late, classifyControlReceipt(some(receipt(false, kWritebackFailedStage)), false)),
                "a later contradicting receipt never replaces the final one");

        // Identity.
        require(controlReceiptMatches(written, "SCADA_LOCAL_1", 1201), "own receipt matches");
        require(!controlReceiptMatches(written, "SCADA_LOCAL_2", 1201), "another command's receipt is ignored");
        require(!controlReceiptMatches(written, "SCADA_LOCAL_1", 1202), "another point's receipt is ignored");
        require(!controlReceiptMatches(written, "", 1201), "an empty command id matches nothing");

        // Multi-point summary: each receipt class counted apart, never merged into a stronger one.
        {
            ControlReceiptTally tally;
            for (int i = 0; i < 3; ++i) countControlReceipt(tally, classifyControlReceipt(some(receipt(true, kWritebackSubmittedStage)), false));
            const auto text = controlReceiptTallyText(tally);
            require(text.find("3 相已下发、未确认") != std::string::npos && text.find("协议写入成功") == std::string::npos,
                    "submitted-only phases are not reported as protocol writes");
            ControlReceiptTally mixed;
            auto confirmed = receipt(true, kWritebackSucceededStage);
            confirmed.verifyAttempted = confirmed.verifyPassed = true;
            for (const auto& r : {confirmed, receipt(true, kWritebackSucceededStage), receipt(true, kWritebackSubmittedStage),
                                  receipt(true, "local-parameter-committed"), receipt(false, kWritebackTimeoutStage),
                                  receipt(false, kWritebackFailedStage)})
                countControlReceipt(mixed, classifyControlReceipt(some(r), false));
            countControlReceipt(mixed, classifyControlReceipt(none, true));
            require(mixed.confirmed == 1 && mixed.written == 1 && mixed.submitted == 1 && mixed.committed == 1 &&
                    mixed.unknown == 1 && mixed.failed == 1 && mixed.open == 1, "each class counted once");
            const auto mixedText = controlReceiptTallyText(mixed);
            for (const char* part : {"1 相执行失败", "1 相结果未知", "1 相无最终回执", "1 相已下发、未确认",
                                     "1 相协议写入成功、动作未确认", "1 相本地参数已提交", "1 相反馈确认成功"})
                require(mixedText.find(part) != std::string::npos, std::string("mixed summary lacks: ") + part);
            require(mixedText.find("执行失败") < mixedText.find("反馈确认成功"), "problems are listed first");
        }

        // Screen feedback point: newer than command and receipt, good, unexpired, equal.
        const auto good = Optional<StoredPointValue>(sample(1, 2001));
        require(screenFeedbackConfirms(good, 1500, 2000, 1), "fresh equal feedback confirms");
        require(!screenFeedbackConfirms(Optional<StoredPointValue>(sample(1, 2000)), 1500, 2000, 1),
                "a value not newer than the receipt is not feedback");
        require(!screenFeedbackConfirms(Optional<StoredPointValue>(sample(1, 1400)), 1500, 1000, 1),
                "a value from before the command is not feedback");
        require(!screenFeedbackConfirms(Optional<StoredPointValue>(sample(1, 2001, 0)), 1500, 2000, 1),
                "bad quality is not feedback");
        require(!screenFeedbackConfirms(Optional<StoredPointValue>(sample(1, 2001, 1, true)), 1500, 2000, 1),
                "stale value is not feedback");
        require(!screenFeedbackConfirms(Optional<StoredPointValue>(sample(0, 2001)), 1500, 2000, 1),
                "a different value is not a confirmation");
        require(!screenFeedbackConfirms(Optional<StoredPointValue>(), 1500, 2000, 1), "missing feedback");
    } catch (const std::exception& ex) {
        std::cerr << "control_receipt_view_test failed: " << ex.what() << '\n';
        return 1;
    }
    std::cout << "control_receipt_view_test passed\n";
    return 0;
}
