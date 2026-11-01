#pragma once

#include <cmath>
#include <cstdint>
#include <string>

#include "edge_gateway/models.hpp"
#include "edge_gateway/writeback_outcome.hpp"

namespace edge_gateway {

// What an operator screen may say about one control command. Derived only from the receipt fields
// (success / stage / outcome flags / verifyPassed); the receipt protocol itself is unchanged.
//   Submitted           writeback-submitted: handed to the output or transport, no device answer
//   Written             writeback-succeeded with verifyPassed=false: the protocol write succeeded,
//                       the action is not confirmed
//   FeedbackConfirmed   verifyPassed=true: the configured read-back/feedback confirmed the value; what
//                       that proves depends on the feedback source named in the receipt message
//   Committed           a local parameter or mailbox commit (no device involved)
//   Unknown             outcome unknown (timeout, mismatch, unconfirmed): may have taken effect;
//                       never shown as a definite failure, never re-sent automatically
//   Failed              a definite failure; the receipt message carries the actual reason
//   Waiting             no receipt yet (or a non-final ledger replay) and still within the wait
//   NoReceipt           no receipt within the wait: the result is unknown
enum class ControlReceiptState {
    Waiting,
    Submitted,
    Written,
    FeedbackConfirmed,
    Committed,
    Unknown,
    Failed,
    NoReceipt,
};

struct ControlReceiptView {
    ControlReceiptState state = ControlReceiptState::Waiting;
    // A final view never changes again for this command; a non-final one may still be followed by
    // the final receipt (shown as a late result).
    bool final = false;
    bool feedbackMismatch = false;
    std::string stage;
    std::string message;
};

// The receipt must belong to the command the screen sent: same cmdId and point. A receipt of another
// command (or point) is ignored instead of being shown for this one.
inline bool controlReceiptMatches(const WritebackResultRecord& receipt, const std::string& cmdId, std::uint32_t index) {
    return !cmdId.empty() && receipt.cmdId == cmdId && (index == 0 || receipt.index == index);
}

inline ControlReceiptView classifyControlReceipt(const Optional<WritebackResultRecord>& receipt, bool waitExpired) {
    ControlReceiptView view;
    if (!receipt || isUnconfirmedWriteback(*receipt)) {
        // A ledger replay of a reserved dispatch without a recorded result: a final result may follow.
        view.state = waitExpired ? ControlReceiptState::NoReceipt : ControlReceiptState::Waiting;
        view.final = false;
        if (receipt) {
            view.stage = receipt->stage;
            view.message = receipt->message;
        }
        return view;
    }
    const auto& r = *receipt;
    view.stage = r.stage;
    view.message = r.message;
    view.final = true;
    const std::string outcome = writebackOutcome(r);
    if (outcome == "succeeded") {
        if (r.verifyAttempted && r.verifyPassed && r.stage != kWritebackSubmittedStage) {
            view.state = ControlReceiptState::FeedbackConfirmed;
        } else if (r.stage == kWritebackSubmittedStage) {
            view.state = ControlReceiptState::Submitted;
        } else if (r.stage == "local-parameter-committed" || r.stage == "mailbox-committed") {
            view.state = ControlReceiptState::Committed;
        } else {
            view.state = ControlReceiptState::Written;
        }
    } else if (outcome == "unknown") {
        view.state = ControlReceiptState::Unknown;
        view.feedbackMismatch = r.feedbackMismatch || r.stage == kWritebackMismatchStage;
    } else {
        view.state = ControlReceiptState::Failed;
    }
    return view;
}

// Short operator wording (UTF-8). The receipt message is appended by the caller.
inline const char* controlReceiptStateText(const ControlReceiptView& view) {
    switch (view.state) {
    case ControlReceiptState::Waiting: return "命令已受理，等待驱动回执";
    case ControlReceiptState::Submitted: return "已下发，未确认：仅交给输出或通信，设备是否动作未确认";
    case ControlReceiptState::Written: return "协议写入成功，动作未确认";
    case ControlReceiptState::FeedbackConfirmed: return "反馈确认成功（确认范围以回执中的反馈来源为准）";
    case ControlReceiptState::Committed: return "本地参数已提交（未下发设备）";
    case ControlReceiptState::Unknown:
        return view.feedbackMismatch ? "结果未知：已执行写入，但反馈与目标不一致；未自动重发"
                                     : "结果未知：设备可能已执行；未自动重发";
    case ControlReceiptState::Failed: return "执行失败";
    case ControlReceiptState::NoReceipt: return "等待回执超时，结果未知：设备可能仍在执行；未自动重发";
    }
    return "未知状态";
}

// Whether the screen may treat the command as done successfully (for the dialog icon only).
inline bool controlReceiptPositive(const ControlReceiptView& view) {
    return view.state == ControlReceiptState::FeedbackConfirmed || view.state == ControlReceiptState::Committed;
}

// A final view replaces a non-final one; once final, nothing replaces it (a late or replayed older
// receipt cannot overwrite the final result).
inline bool controlReceiptShouldReplace(const ControlReceiptView& current, const ControlReceiptView& next) {
    if (current.final) return false;
    if (next.final) return true;
    return next.state != current.state;
}

// Counts of a multi-point command (e.g. the three PCS phases). Every receipt class is kept apart: a
// summary is never stronger than the weakest evidence of the points it counts.
struct ControlReceiptTally {
    int confirmed = 0;  // FeedbackConfirmed
    int written = 0;    // Written: protocol write succeeded, action unconfirmed
    int submitted = 0;  // Submitted: handed to the output or transport only
    int committed = 0;  // Committed: local parameter / mailbox, no device involved
    int unknown = 0;    // Unknown outcome
    int failed = 0;     // definite failure
    int open = 0;       // Waiting / NoReceipt: no final receipt
};

inline void countControlReceipt(ControlReceiptTally& tally, const ControlReceiptView& view) {
    switch (view.state) {
    case ControlReceiptState::FeedbackConfirmed: ++tally.confirmed; break;
    case ControlReceiptState::Written: ++tally.written; break;
    case ControlReceiptState::Submitted: ++tally.submitted; break;
    case ControlReceiptState::Committed: ++tally.committed; break;
    case ControlReceiptState::Unknown: ++tally.unknown; break;
    case ControlReceiptState::Failed: ++tally.failed; break;
    case ControlReceiptState::Waiting:
    case ControlReceiptState::NoReceipt: ++tally.open; break;
    }
}

// One line per class present, strongest problem first (UTF-8), e.g. for three PCS phases.
inline std::string controlReceiptTallyText(const ControlReceiptTally& tally) {
    std::string text;
    const auto add = [&text](int count, const char* what) {
        if (count <= 0) return;
        if (!text.empty()) text += "；";
        text += std::to_string(count) + " 相" + what;
    };
    add(tally.failed, "执行失败（见各相原因）");
    add(tally.unknown, "结果未知，设备可能已执行，未自动重发");
    add(tally.open, "无最终回执，结果未知，未自动重发");
    add(tally.submitted, "已下发、未确认（仅交给输出或通信，设备是否动作未确认）");
    add(tally.written, "协议写入成功、动作未确认");
    add(tally.committed, "本地参数已提交（未下发设备）");
    add(tally.confirmed, "反馈确认成功（确认范围以各相回执的反馈来源为准）");
    return text;
}

// Screen-level feedback point (controlFeedbackTag): only a good, unexpired value stored strictly after
// both the command and its receipt counts, and only when it equals the expected value.
inline bool screenFeedbackConfirms(
    const Optional<StoredPointValue>& feedback,
    std::int64_t commandTs,
    std::int64_t receiptCompletedAt,
    double expected
) {
    if (!feedback || feedback->quality != 1 || feedback->stale || !std::isfinite(feedback->value)) return false;
    const auto after = commandTs > receiptCompletedAt ? commandTs : receiptCompletedAt;
    return feedback->ts > after && std::fabs(feedback->value - expected) < 0.00001;
}

}  // namespace edge_gateway
