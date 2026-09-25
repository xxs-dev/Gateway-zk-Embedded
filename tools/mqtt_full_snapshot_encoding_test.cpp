// Exercise the actual private encoder without constructing a publisher or opening sockets.
#include "../src/builtin_mqtt_driver_publisher.cpp"
#include "edge_gateway/json_value.hpp"
#include "edge_gateway/mqtt_full_snapshot.hpp"
#include <iostream>
#include <set>

namespace {

void checkFullEncoding(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

const edge_gateway::json::JsonValue& fullField(const edge_gateway::json::JsonValue& value, const char* name) {
    const auto* field = value.find(name);
    checkFullEncoding(field != nullptr, "missing full JSON field");
    return *field;
}

void testFullEncodingWithoutNetwork() {
    using namespace edge_gateway;
    PointStoreRouter router;
    std::vector<StoredPointValue> raw;
    std::vector<std::uint32_t> indexes;
    for (std::uint32_t index = 4500; index <= 4650; ++index) {
        const auto slave = index <= 4561 ? "1" : (index <= 4624 ? "2" : "3");
        PointStoreRoute route;
        route.index = route.sourceIndex = index;
        route.machineCode = "GW_FULL_IDENTITY";
        route.meterCode = std::string("T2216_modbusRTU_2_slave_") + slave;
        route.pointCode = std::string("mb2_s") + slave + "_" + std::to_string(index);
        route.sharedMemoryName = "unused_encoding_only";
        router.addRoute(route);
        indexes.push_back(index);
        if (index != 4561) {
            StoredPointValue value;
            value.index = index;
            value.machineCode = route.machineCode;
            value.meterCode = route.meterCode;
            value.pointCode = route.pointCode;
            value.value = 12.5;
            value.ts = 1790332918320LL;
            value.expireAt = 1790333518320LL;
            raw.push_back(value);
        }
    }
    indexes.push_back(4561);
    indexes.push_back(99999);
    const auto values = completeMqttFullSnapshot(raw, indexes, router);
    checkFullEncoding(values.size() == 151, "completion duplicated a missing index or fabricated an unrouted identity");
    for (const auto format : {PointValueJsonFormat::CompactArray, PointValueJsonFormat::Object}) {
        for (const std::size_t limit : {4096u, 65536u}) {
            const auto payloads = encodeRealtimeChunks("snapshot", values, limit, format, "GW_FULL_IDENTITY");
            checkFullEncoding((limit == 4096 && payloads.size() > 1) ||
                (limit == 65536 && payloads.size() == 1), "chunk boundary was not exercised");
            std::set<std::uint32_t> seen;
            std::string chunkId;
            for (std::size_t chunk = 0; chunk < payloads.size(); ++chunk) {
                const auto document = json::JsonParser(payloads[chunk]).parse();
                checkFullEncoding(payloads[chunk].size() <= limit, "full exceeded configured payload limit");
                checkFullEncoding(fullField(document, "type").asString() == "snapshot" &&
                    fullField(document, "machineCode").asString() == "GW_FULL_IDENTITY" &&
                    document.find("sessionId") == nullptr, "full envelope changed");
                const auto id = fullField(document, "chunkId").asString();
                if (chunk == 0) chunkId = id;
                checkFullEncoding(id == chunkId && fullField(document, "chunkIndex").asNumber() == chunk + 1 &&
                    fullField(document, "chunkCount").asNumber() == payloads.size(), "inconsistent full chunks");
                for (const auto& meter : fullField(document, "meters").asArray().values) {
                    for (const auto& item : fullField(*meter, "values").asArray().values) {
                        const bool compact = format == PointValueJsonFormat::CompactArray;
                        const auto field = [&](std::size_t position, const char* name) -> const json::JsonValue& {
                            return compact ? *item->asArray().values.at(position) : fullField(*item, name);
                        };
                        if (compact) checkFullEncoding(item->asArray().values.size() == 7, "compact schema changed");
                        const auto index = static_cast<std::uint32_t>(field(0, "index").asNumber());
                        const auto route = router.routeByIndex(index);
                        checkFullEncoding(route && seen.insert(index).second &&
                            fullField(*meter, "meterCode").asString() == route->meterCode &&
                            field(1, "pointCode").asString() == route->pointCode, "lost, duplicated or foreign full identity");
                        if (index == 4561) {
                            checkFullEncoding(std::isnan(values.at(61).value) && field(2, "value").isNull(),
                                "missing value must be JSON null, never a fabricated numeric zero");
                            checkFullEncoding(field(3, "quality").asNumber() == 0 &&
                                field(4, "ts").asNumber() == 0 && field(5, "expireAt").asNumber() == 0 &&
                                field(6, "stale").asBool(), "missing full sample became valid on wire");
                        } else {
                            checkFullEncoding(field(2, "value").asNumber() == 12.5 &&
                                field(3, "quality").asNumber() == 1 && !field(6, "stale").asBool() &&
                                field(4, "ts").asNumber() == 1790332918320LL &&
                                field(5, "expireAt").asNumber() == 1790333518320LL, "valid sample encoding changed");
                        }
                    }
                }
            }
            checkFullEncoding(seen.size() == 151, "full wire identity set is not exact 151");
        }
    }
}

} // namespace

int main() {
    try {
        testFullEncodingWithoutNetwork();
        std::cout << "full encoding: compact/object x single/multiple chunks, exact 151 identities passed (no sockets)\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "mqtt_full_snapshot_encoding_test failed: " << ex.what() << '\n';
        return 1;
    }
}
