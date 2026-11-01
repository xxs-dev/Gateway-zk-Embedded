// Local control transactions independent of pages: locks per point, settlement only by a definite
// final receipt of the same command and point, the deadline as the only other end, and no cross-talk
// between an old and a newer command on the same point. Simulation with a controlled clock.
#include "edge_gateway/control_transaction_registry.hpp"

#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

using namespace edge_gateway;

namespace {

void require(bool ok, const std::string& what) {
    if (!ok) throw std::runtime_error(what);
}

WritebackResultRecord receipt(const std::string& cmdId, std::uint32_t index, bool success, const std::string& stage) {
    WritebackResultRecord r;
    r.cmdId = cmdId;
    r.index = index;
    r.success = success;
    r.stage = stage;
    r.message = "fixture";
    normalizeWritebackOutcome(r);
    return r;
}

struct Receipts {
    std::map<std::pair<std::string, std::string>, WritebackResultRecord> byTagAndCmd;
    ControlTransactionRegistry::ReceiptReader reader() {
        return [this](const std::string& tagId, const std::string& cmdId) {
            const auto it = byTagAndCmd.find({tagId, cmdId});
            return it == byTagAndCmd.end() ? Optional<WritebackResultRecord>()
                                           : Optional<WritebackResultRecord>(it->second);
        };
    }
};

}  // namespace

int main() {
    try {
        std::int64_t clock = 1000;
        ControlTransactionRegistry registry([&clock] { return clock; });
        Receipts receipts;
        std::string holder;

        // Three phases of one batch: each phase is settled on its own definite receipt.
        registry.begin("B1", {{"PA", 0, false}, {"PB", 0, false}, {"PC", 0, false}}, 70000);
        require(registry.locked("PA", &holder) && holder == "B1", "a new batch locks its phases");
        require(!registry.locked("QA"), "other points stay free");
        receipts.byTagAndCmd[{"PA", "B1"}] = receipt("B1", 1, true, kWritebackSubmittedStage);
        receipts.byTagAndCmd[{"PB", "B1"}] = receipt("B1", 2, false, kWritebackTimeoutStage);
        registry.poll(receipts.reader());
        require(!registry.locked("PA"), "a definite receipt (even submitted-only) settles its phase");
        require(registry.locked("PB"), "an unknown outcome keeps its phase locked");
        require(registry.locked("PC"), "a phase without receipt stays locked");
        receipts.byTagAndCmd[{"PC", "B1"}] = receipt("OTHER", 3, true, kWritebackSucceededStage);
        registry.poll(receipts.reader());
        require(registry.locked("PC"), "a receipt of another command never settles this one");
        receipts.byTagAndCmd[{"PC", "B1"}] = receipt("B1", 3, false, kWritebackUnconfirmedStage);
        registry.poll(receipts.reader());
        require(registry.locked("PC"), "a non-final ledger replay does not settle");
        receipts.byTagAndCmd[{"PB", "B1"}] = receipt("B1", 2, false, kWritebackFailedStage);
        receipts.byTagAndCmd[{"PC", "B1"}] = receipt("B1", 3, true, kWritebackSucceededStage);
        registry.poll(receipts.reader());
        require(!registry.locked("PB") && !registry.locked("PC") && registry.openCount() == 0,
                "late definite receipts settle the remaining phases and close the batch");

        // Point identity: an explicit index must match.
        registry.begin("S1", {{"T1", 1201, false}}, 70000);
        receipts.byTagAndCmd[{"T1", "S1"}] = receipt("S1", 1202, true, kWritebackSucceededStage);
        registry.poll(receipts.reader());
        require(registry.locked("T1"), "a receipt for another index does not settle the point");
        registry.settle("S1", "T1");
        require(!registry.locked("T1"), "a page may settle on the receipt it observed");

        // No receipt at all: the lock ends exactly at the deadline, never later.
        registry.begin("S2", {{"T2", 0, false}}, 70000);
        clock += 69999;
        require(registry.locked("T2"), "locked until the deadline");
        clock += 1;
        require(!registry.locked("T2") && registry.openCount() == 0, "the deadline ends the transaction");

        // An unknown final receipt keeps the lock until the deadline.
        registry.begin("S3", {{"T3", 0, false}}, 70000);
        receipts.byTagAndCmd[{"T3", "S3"}] = receipt("S3", 1, false, kWritebackMismatchStage);
        registry.poll(receipts.reader());
        clock += 30000;
        require(registry.locked("T3"), "unknown outcome stays locked within the tracking period");
        clock += 40000;
        require(!registry.locked("T3"), "unknown outcome released at the deadline");

        // An old command never releases a newer one on the same point.
        registry.begin("OLD", {{"T4", 0, false}}, 1000);
        clock += 1000;
        require(!registry.locked("T4"), "old command expired");
        registry.begin("NEW", {{"T4", 0, false}}, 70000);
        receipts.byTagAndCmd[{"T4", "OLD"}] = receipt("OLD", 1, true, kWritebackSucceededStage);
        registry.poll(receipts.reader());
        registry.settle("OLD", "T4");
        require(registry.locked("T4", &holder) && holder == "NEW", "a late receipt of the old command keeps the new lock");

        // Settling ignores unknown views by contract.
        ControlReceiptView unknown;
        unknown.state = ControlReceiptState::Unknown;
        unknown.final = true;
        require(!ControlTransactionRegistry::settlesTransaction(unknown), "unknown never settles");
    } catch (const std::exception& ex) {
        std::cerr << "control_transaction_registry_test failed: " << ex.what() << '\n';
        return 1;
    }
    std::cout << "control_transaction_registry_test passed\n";
    return 0;
}
