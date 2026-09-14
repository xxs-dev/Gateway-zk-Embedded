#define main event_store_client_contract_test_main
#include "event_store_client_test.cpp"
#undef main
#include "edge_gateway/event_history_projection.hpp"

int main(int argc, char** argv) {
    try {
        require(argc >= 3, "usage: probe DB bad|claim|projection [HISTORY]");
        Directory directory;
        auto options = directory.options();
        options.databasePath = argv[1]; options.identity = {"store-a", "g1"};
        options.producers = {"producer-a"};
        const std::string mode = argv[2];
        if (mode == "projection") {
            require(argc == 4, "history path required");
            MqttEventOutbox outbox(options.databasePath, "", 12, 24, 100, 0, Profile::DeleteFull);
            EventStoreDatabase db(outbox, options.identity, options.producers);
            EventHistoryProjection projection(db, argv[3]);
            auto cursor = db.reconcileProjection(projection);
            const auto rows = db.readJournal(cursor, 64);
            if (!rows.empty()) {
                const auto previous = cursor;
                cursor = projection.projectRows(rows, cursor);
                db.commitProjectionCursor(db.journalGeneration(), previous, cursor);
            }
            require(projection.watermark() == cursor, "projection watermark mismatch");
            std::cout << "PROJECTION_OK " << cursor << '\n';
            return 0;
        }
        require(mode == "bad" || mode == "claim", "unknown probe mode");
        EventStoreRuntime runtime(options); runtime.start();
        ReplyProxy proxy(directory.path + "/proxy.sock", options.socketPath);
        EventStoreClient sender(clientOptions(options, Role::Sender, directory.path + "/proxy.sock"));
        sender.start();
        if (mode == "bad") {
            const auto error = clientError([&] { sender.execute("ClaimBatch", claimArgs()); });
            require(error.code() == "BAD_RESPONSE" && error.outcomeUnknown() && sender.hasPending(),
                "empty legacy identity did not leave client Claim pending/unknown");
            proxy.waitForExchanges("ClaimBatch", 1);
            const auto reply = parse(proxy.exchanges("ClaimBatch").front().runtimeReply);
            require(field(reply, "ok").asBool(), "runtime did not really commit legacy Claim");
            bool empty = false;
            for (const auto& item : field(reply, "messages").asArray().values)
                empty |= stringField(*item, "eventId").empty();
            require(empty, "fault fixture did not return empty eventId");
            std::cout << "EMPTY_ID_BAD_RESPONSE_UNKNOWN\n";
        } else {
            std::size_t total = 0;
            for (int batch = 0; batch < 16; ++batch) {
                const auto claimed = sender.execute("ClaimBatch", claimArgs());
                const auto& messages = field(claimed, "messages").asArray().values;
                if (messages.empty()) break;
                for (const auto& item : messages) {
                    require(!stringField(*item, "eventId").empty(), "migrated eventId empty");
                    std::cout << "CLAIM " << stringField(*item, "eventId") << '\n';
                }
                total += messages.size();
                sender.execute("AckBatch", finishArgs(claimed));
            }
            require(total > 0 && pendingCount(options) == 0, "migrated Claim/ACK did not drain all pending rows");
            std::cout << "CLAIM_ACK_OK " << total << '\n';
        }
        proxy.checked();
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
