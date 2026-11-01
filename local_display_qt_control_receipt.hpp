#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <QObject>
#include <QPointer>
#include <QString>

#include "edge_gateway/control_receipt_view.hpp"
#include "edge_gateway/control_transaction_registry.hpp"
#include "local_display_qt_scada_runtime.hpp"

class QMessageBox;
class QTimer;
class QWidget;

// Follows one local control command from queue admission to its final receipt and shows it in a
// receipt dialog. The wording comes from edge_gateway::classifyControlReceipt; the screen never turns
// "submitted", "written but unconfirmed" or "unknown" into a confirmed or failed action.
//   - Only the receipt of this cmdId and point is used.
//   - A final result is never replaced by an older or non-final one.
//   - Without a receipt by waitMs the command is shown as unknown, and tracking continues until
//     trackMs: a late final receipt then replaces it (marked as late). Nothing is re-sent.
//   - The point lock belongs to the process-wide localControlTransactions(), not to this dialog: it
//     survives the page (logout, session expiry, project reload) and ends on a definite final
//     receipt or at trackMs. Destroying the tracker neither releases nor extends it.
class ControlReceiptTracker final : public QObject {
public:
    struct Request {
        std::string tagId;
        std::uint32_t index = 0;
        std::string cmdId;
        double value = 0.0;
        std::int64_t commandTs = 0;
        // Optional screen-level feedback point checked after the receipt (controlFeedbackTag).
        std::string feedbackTag;
        double feedbackValue = 0.0;
        QString title;
        std::int64_t waitMs = 10000;
        std::int64_t trackMs = 70000;
    };

    // Starts tracking; onWaitEnded runs once when the dialog's waiting phase ends (final receipt or
    // waitMs), so the caller can release its own in-flight lock.
    static ControlReceiptTracker* start(
        QWidget* parent,
        ScadaSceneRuntimeSource& runtime,
        Request request,
        std::function<void()> onWaitEnded
    );

    // Whether a local command on this point is still open in localControlTransactions().
    static bool tagUnresolved(const std::string& tagId, std::string* cmdId = nullptr);

    // Message for one poll (exposed for tests).
    static QString describe(
        const edge_gateway::ControlReceiptView& view,
        bool late,
        const QString& screenFeedback
    );

    const edge_gateway::ControlReceiptView& view() const { return view_; }
    ~ControlReceiptTracker() override;

private:
    ControlReceiptTracker(QWidget* parent, ScadaSceneRuntimeSource& runtime, Request request,
                          std::function<void()> onWaitEnded);
    void poll();
    void finishWait();
    void stop();

    ScadaSceneRuntimeSource& runtime_;
    Request request_;
    std::function<void()> onWaitEnded_;
    QPointer<QMessageBox> dialog_;
    QTimer* timer_ = nullptr;
    edge_gateway::ControlReceiptView view_;
    bool waitEnded_ = false;
    bool shownNoReceipt_ = false;
    bool late_ = false;
    bool screenFeedbackDone_ = false;
    bool screenFeedbackConfirmed_ = false;
    QString screenFeedback_;
};

// Process-wide local control transactions (GUI thread). The runtime window polls it every refresh so
// that a transaction whose page was destroyed is still settled by its late receipt.
edge_gateway::ControlTransactionRegistry& localControlTransactions();
