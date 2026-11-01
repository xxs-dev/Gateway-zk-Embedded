#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "edge_gateway/control_receipt_view.hpp"
#include "edge_gateway/models.hpp"

namespace edge_gateway {

// Local control transactions of one operator screen process, independent of any page or widget.
// A page can be destroyed while its command is still open (logout, session expiry, project reload
// rebuild every page); the transaction and its point lock stay here.
//   - begin() opens a transaction for a command the edge accepted, over one or more points.
//   - A point is settled only by a definite final receipt of the same cmdId and point (success of any
//     kind, or a definite failure). No receipt, a non-final ledger replay and an unknown outcome
//     (timeout, mismatch) keep it open: the command may still have taken effect.
//   - Each transaction ends at its deadline (monotonic clock), settled or not: no permanent lock.
//   - Transactions are keyed by cmdId, so a late receipt of an old command never settles a newer one.
// GUI-thread use only. Kept in memory: a restart of the screen process starts without locks.
class ControlTransactionRegistry {
public:
    using Clock = std::function<std::int64_t()>;
    using ReceiptReader = std::function<Optional<WritebackResultRecord>(const std::string& tagId, const std::string& cmdId)>;

    struct Point {
        std::string tagId;
        std::uint32_t index = 0;  // 0: any index of that tag
        bool settled = false;
    };

    static std::int64_t steadyNowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    explicit ControlTransactionRegistry(Clock clock = &ControlTransactionRegistry::steadyNowMs)
        : clock_(std::move(clock)) {}

    void setClock(Clock clock) { clock_ = std::move(clock); }

    void begin(const std::string& cmdId, std::vector<Point> points, std::int64_t trackMs) {
        purge();
        if (cmdId.empty() || points.empty()) return;
        open_.push_back({cmdId, std::move(points), clock_() + std::max<std::int64_t>(trackMs, 0)});
    }

    // Whether an open transaction still holds tagId; cmdId receives the newest such command.
    bool locked(const std::string& tagId, std::string* cmdId = nullptr) {
        purge();
        for (auto it = open_.rbegin(); it != open_.rend(); ++it) {
            for (const auto& point : it->points) {
                if (point.tagId == tagId && !point.settled) {
                    if (cmdId != nullptr) *cmdId = it->cmdId;
                    return true;
                }
            }
        }
        return false;
    }

    // A definite final receipt of (cmdId, tagId) was observed by a page.
    void settle(const std::string& cmdId, const std::string& tagId) {
        for (auto& transaction : open_) {
            if (transaction.cmdId != cmdId) continue;
            for (auto& point : transaction.points)
                if (point.tagId == tagId) point.settled = true;
        }
        purge();
    }

    // Reads the receipts of every open point (called by the window, not by a page).
    void poll(const ReceiptReader& read) {
        purge();
        for (auto& transaction : open_) {
            for (auto& point : transaction.points) {
                if (point.settled) continue;
                const auto receipt = read(point.tagId, transaction.cmdId);
                if (receipt && controlReceiptMatches(*receipt, transaction.cmdId, point.index) &&
                    settlesTransaction(classifyControlReceipt(receipt, false))) {
                    point.settled = true;
                }
            }
        }
        purge();
    }

    std::size_t openCount() {
        purge();
        return open_.size();
    }

    // Whether this receipt view ends the lock of its point before the deadline.
    static bool settlesTransaction(const ControlReceiptView& view) {
        return view.final && view.state != ControlReceiptState::Unknown;
    }

private:
    struct Transaction {
        std::string cmdId;
        std::vector<Point> points;
        std::int64_t deadline = 0;
    };

    void purge() {
        const auto now = clock_();
        open_.erase(std::remove_if(open_.begin(), open_.end(), [now](const Transaction& transaction) {
            return now >= transaction.deadline ||
                   std::all_of(transaction.points.begin(), transaction.points.end(),
                               [](const Point& point) { return point.settled; });
        }), open_.end());
    }

    Clock clock_;
    std::vector<Transaction> open_;
};

}  // namespace edge_gateway
