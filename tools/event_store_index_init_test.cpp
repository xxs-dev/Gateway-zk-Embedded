#define main history_runtime_regression_main
#include "event_store_history_runtime_test.cpp"
#undef main

int main() {
    try {
        Temp temp;
        auto options = temp.options(Outbox::StorageProfile::WalFull);
        const auto indexCount = [&] {
            Sql sql(options.databasePath);
            return sql.scalar("SELECT count(*) FROM sqlite_master WHERE name='idx_event_store_delivery_order';");
        };
        {
            Outbox legacy(options.databasePath, "", 12, 24, 100, 0, options.profile);
            require(indexCount() == 0, "legacy Outbox created EventStore index");
            EventStoreDatabase writer(legacy, options.identity, options.producers);
            require(indexCount() == 1, "EventStore writer missing index");
        }
        Sql inspect(options.databasePath);
        inspect.run("INSERT INTO mqtt_event_outbox(event_id,event_type,topic,payload,event_ts,event_month,created_at) "
            "VALUES('retained','change','fixture','original',100,'2026-09',100);");
        inspect.run("DROP INDEX idx_event_store_delivery_order;");
        const auto schema = inspect.scalar("PRAGMA schema_version;");
        {
            Outbox box(options.databasePath, "", 12, 24, 100, 0, options.profile, Outbox::AccessMode::ReadOnly);
            EventStoreDatabase reader(box, options.identity, options.producers, true);
            require(indexCount() == 0 && inspect.scalar("PRAGMA schema_version;") == schema,
                "read-only EventStore performed DDL");
        }
        {
            Outbox legacy(options.databasePath, "", 12, 24, 100, 0, options.profile);
            require(indexCount() == 0, "legacy Outbox repaired EventStore index");
        }
        for (int reopen = 0; reopen < 2; ++reopen) {
            Outbox box(options.databasePath, "", 12, 24, 100, 0, options.profile);
            const auto before = inspect.scalar("PRAGMA schema_version;");
            EventStoreDatabase writer(box, options.identity, options.producers);
            require(indexCount() == 1, "writer did not create missing index on existing DB");
            if (reopen) require(inspect.scalar("PRAGMA schema_version;") == before, "repeat init changed schema");
            require(inspect.scalar("SELECT count(*) FROM mqtt_event_outbox WHERE event_id='retained' AND payload='original' AND sent=0;") == 1,
                "index upgrade changed retained event");
        }
        std::cout << "PASS writer-only index, read-only no-DDL, legacy unchanged, existing DB upgrade and repeated init\n";
        return 0;
    } catch (const std::exception& ex) { std::cerr << "FAIL: " << ex.what() << '\n'; return 1; }
}
