#include "local_display_qt_control_receipt.hpp"

#include <chrono>
#include <unordered_map>
#include <utility>

#include <QMessageBox>
#include <QTimer>
#include <QWidget>

namespace {

std::int64_t receiptNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// tagId -> cmdId of the unresolved local command (GUI thread only).
std::unordered_map<std::string, std::string>& unresolvedTags() {
    static std::unordered_map<std::string, std::string> tags;
    return tags;
}

bool successfulOutcome(const edge_gateway::ControlReceiptView& view) {
    using edge_gateway::ControlReceiptState;
    return view.state == ControlReceiptState::Submitted || view.state == ControlReceiptState::Written ||
           view.state == ControlReceiptState::FeedbackConfirmed;
}

}  // namespace

bool ControlReceiptTracker::tagUnresolved(const std::string& tagId, std::string* cmdId) {
    const auto found = unresolvedTags().find(tagId);
    if (found == unresolvedTags().end()) return false;
    if (cmdId != nullptr) *cmdId = found->second;
    return true;
}

void ControlReceiptTracker::markUnresolved(const std::string& tagId, const std::string& cmdId) {
    if (!tagId.empty()) unresolvedTags()[tagId] = cmdId;
}

void ControlReceiptTracker::clearUnresolved(const std::string& tagId, const std::string& cmdId) {
    const auto found = unresolvedTags().find(tagId);
    if (found != unresolvedTags().end() && found->second == cmdId) unresolvedTags().erase(found);
}

QString ControlReceiptTracker::describe(
    const edge_gateway::ControlReceiptView& view,
    bool late,
    const QString& screenFeedback
) {
    QString text = late ? QString::fromUtf8("晚到回执：") : QString();
    text += QString::fromUtf8(edge_gateway::controlReceiptStateText(view));
    if (!view.stage.empty()) text += QString::fromUtf8("\n阶段：") + QString::fromStdString(view.stage);
    if (!view.message.empty()) {
        text += (view.state == edge_gateway::ControlReceiptState::Failed ? QString::fromUtf8("\n原因：")
                                                                         : QString::fromUtf8("\n回执："))
             + QString::fromStdString(view.message);
    }
    if (!screenFeedback.isEmpty()) text += QStringLiteral("\n") + screenFeedback;
    return text;
}

ControlReceiptTracker* ControlReceiptTracker::start(
    QWidget* parent,
    ScadaSceneRuntimeSource& runtime,
    Request request,
    std::function<void()> onWaitEnded
) {
    return new ControlReceiptTracker(parent, runtime, std::move(request), std::move(onWaitEnded));
}

ControlReceiptTracker::ControlReceiptTracker(
    QWidget* parent,
    ScadaSceneRuntimeSource& runtime,
    Request request,
    std::function<void()> onWaitEnded
) : QObject(parent), runtime_(runtime), request_(std::move(request)), onWaitEnded_(std::move(onWaitEnded)) {
    markUnresolved(request_.tagId, request_.cmdId);
    auto* dialog = new QMessageBox(QMessageBox::Information,
        request_.title.isEmpty() ? QString::fromUtf8("控制执行结果") : request_.title,
        QString::fromUtf8("命令已受理，等待驱动回执。\n命令：%1").arg(QString::fromStdString(request_.cmdId)),
        QMessageBox::Close, parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setObjectName(QStringLiteral("controlReceiptDialog"));
    dialog->show();
    dialog_ = dialog;
    timer_ = new QTimer(this);
    timer_->setInterval(200);
    QObject::connect(timer_, &QTimer::timeout, this, [this]() { poll(); });
    timer_->start();
}

ControlReceiptTracker::~ControlReceiptTracker() {
    clearUnresolved(request_.tagId, request_.cmdId);
    if (!waitEnded_ && onWaitEnded_) {
        waitEnded_ = true;
        onWaitEnded_();
    }
}

void ControlReceiptTracker::poll() {
    const auto now = receiptNowMs();
    const auto elapsed = now - request_.commandTs;
    const bool waitExpired = elapsed >= request_.waitMs;
    auto receipt = runtime_.getWritebackResult(request_.tagId, request_.cmdId);
    if (receipt && !edge_gateway::controlReceiptMatches(*receipt, request_.cmdId, request_.index)) {
        receipt = edge_gateway::Optional<edge_gateway::WritebackResultRecord>(edge_gateway::NullOpt);
    }
    const auto next = edge_gateway::classifyControlReceipt(receipt, waitExpired);
    if (edge_gateway::controlReceiptShouldReplace(view_, next)) {
        if (next.final && shownNoReceipt_) late_ = true;
        view_ = next;
        if (view_.state == edge_gateway::ControlReceiptState::NoReceipt) shownNoReceipt_ = true;
    }

    // Screen feedback point: only after a successful receipt, only a value newer than both.
    if (view_.final && successfulOutcome(view_) && !request_.feedbackTag.empty() && !screenFeedbackDone_) {
        const auto feedback = runtime_.readTag(request_.feedbackTag, now);
        if (receipt && edge_gateway::screenFeedbackConfirms(feedback, request_.commandTs, receipt->completedAt,
                                                             request_.feedbackValue)) {
            screenFeedback_ = QString::fromUtf8("画面反馈点：命令之后的新鲜反馈与目标一致。");
            screenFeedbackDone_ = true;
            screenFeedbackConfirmed_ = true;
        } else if (waitExpired) {
            screenFeedback_ = QString::fromUtf8("画面反馈点：未确认（无效、过期、命令之前的旧值或与目标不一致）。");
            screenFeedbackDone_ = true;
        } else {
            screenFeedback_ = QString::fromUtf8("画面反馈点：等待命令之后的新值。");
        }
    }

    const bool screenPending = view_.final && successfulOutcome(view_) && !request_.feedbackTag.empty() &&
                               !screenFeedbackDone_;
    const bool finished = view_.final && !screenPending;
    if (dialog_) {
        dialog_->setText(describe(view_, late_, screenFeedback_) + QString::fromUtf8("\n命令：") +
                         QString::fromStdString(request_.cmdId));
        if (finished || waitExpired) {
            const bool positive = edge_gateway::controlReceiptPositive(view_) || screenFeedbackConfirmed_;
            dialog_->setIcon(finished && positive ? QMessageBox::Information : QMessageBox::Warning);
        }
    }
    if (!finished && elapsed >= request_.trackMs && dialog_) {
        dialog_->setText(describe(view_, false, screenFeedback_) +
            QString::fromUtf8("\n已等待 %1 秒仍无最终回执，结果未知；未自动重发，请先在现场核实。\n命令：%2")
                .arg(request_.trackMs / 1000).arg(QString::fromStdString(request_.cmdId)));
    }
    if (finished || waitExpired) finishWait();
    if (finished || elapsed >= request_.trackMs) stop();
}

void ControlReceiptTracker::finishWait() {
    if (waitEnded_) return;
    waitEnded_ = true;
    if (onWaitEnded_) onWaitEnded_();
}

void ControlReceiptTracker::stop() {
    if (timer_ != nullptr) timer_->stop();
    clearUnresolved(request_.tagId, request_.cmdId);
    finishWait();
    deleteLater();
}
