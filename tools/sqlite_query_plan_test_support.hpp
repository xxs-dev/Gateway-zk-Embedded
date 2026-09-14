#pragma once
#include <stdexcept>
#include <string>

namespace sqlite_plan_test {
inline bool usesCoveringRangeSeek(const std::string& plan, const std::string& table,
                                 const std::string& index, const std::string& constraints) {
    const auto suffix = table + " USING COVERING INDEX " + index + " (" + constraints + ")";
    return (plan.find("SEARCH " + suffix) != std::string::npos ||
            plan.find("SEARCH TABLE " + suffix) != std::string::npos) &&
        plan.find("SCAN ") == std::string::npos && plan.find("TEMP B-TREE") == std::string::npos;
}

inline void verifyFixtures() {
    struct Contract { const char* table; const char* index; const char* constraints; };
    const Contract contracts[] = {
        {"point_samples", "idx_point_samples_ts", "ts<?"},
        {"alarm_events", "idx_alarm_events_ts_id", "ts<?"},
        {"mqtt_event_outbox", "idx_mqtt_event_outbox_expiry_days", "sent=? AND event_ts<?"}
    };
    for (const auto& contract : contracts) {
        const auto accepts = [&](const std::string& plan) {
            return usesCoveringRangeSeek(plan, contract.table, contract.index, contract.constraints);
        };
        for (const auto* tableWord : {"", "TABLE "}) {
            const auto prefix = std::string("SEARCH ") + tableWord + contract.table;
            const auto indexed = prefix + " USING COVERING INDEX " + contract.index;
            const auto seek = indexed + " (" + contract.constraints + ")";
            const auto fullPlan = "2|0|0|" + prefix + " USING INTEGER PRIMARY KEY (rowid=?)|"
                "7|0|0|LIST SUBQUERY 1|11|7|0|" + seek + "|";
            if (!accepts(fullPlan)) throw std::runtime_error("valid SQLite query-plan fixture rejected: " + fullPlan);
            const std::string rejected[] = {
                std::string("SCAN ") + tableWord + contract.table + " USING COVERING INDEX " + contract.index,
                prefix + " USING COVERING INDEX wrong_index (" + contract.constraints + ")",
                std::string("SEARCH ") + tableWord + "wrong_table USING COVERING INDEX " + contract.index +
                    " (" + contract.constraints + ")",
                indexed,
                indexed + " (sent=?)",
                indexed + " (ts<=?)",
                indexed + " (ts>?)",
                indexed + " (sent=? AND event_ts>?)",
                prefix + " USING INDEX " + contract.index + " (" + contract.constraints + ")",
                seek + "|SCAN " + contract.table + "|",
                seek + "|SCAN TABLE " + contract.table + "|",
                seek + "|USE TEMP B-TREE FOR ORDER BY|"
            };
            for (const auto& plan : rejected)
                if (accepts(plan)) throw std::runtime_error("unsafe SQLite query-plan fixture accepted: " + plan);
        }
    }
}
}  // namespace sqlite_plan_test
