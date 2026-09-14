#include "edge_gateway/mqtt_event_stats_factory.hpp"
#include "edge_gateway/event_stats_reader.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace edge_gateway {
namespace {

class UnavailableStats final : public IEventStatsSource {
public:
    explicit UnavailableStats(std::string error) : error_(std::move(error)) {}
    EventStatsCacheEntry snapshot(const std::string&) const override {
        throw std::runtime_error(error_);
    }
private:
    std::string error_;
};

} // namespace

std::unique_ptr<IEventStatsSource> makeLegacyMqttEventStatsSource(
    const MqttConfig& config, std::vector<EventStatsQuery> queries) {
    try {
        if (config.eventOutboxSqlitePath.find('\0') != std::string::npos) {
            throw std::invalid_argument("invalid event statistics database path");
        }
        // The legacy writer may accept a relative path; resolve it once only
        // after that writer has opened the file. Do not create a missing database.
        char* resolved = realpath(config.eventOutboxSqlitePath.c_str(), nullptr);
        if (!resolved) throw std::runtime_error("event statistics database path unavailable");
        std::unique_ptr<char, decltype(&std::free)> path(resolved, &std::free);
        LegacyEventStatsOptions reader;
        reader.databasePath = path.get();
        reader.sqliteLibraryPath = config.eventOutboxSqliteLibraryPath;
        reader.profile = MqttEventOutbox::StorageProfile::DeleteNormal;
        EventStatsCacheOptions options;
        options.reader = reader;
        options.queries = std::move(queries);
        auto cache = std::make_unique<EventStatsCache>(std::move(options));
        cache->start();
        return cache;
    } catch (const std::exception& ex) {
        const auto error = std::string(ex.what()).substr(0, 512);
        std::cerr << "mqtt event statistics unavailable error=" << error << std::endl;
        return std::make_unique<UnavailableStats>(error);
    }
}

} // namespace edge_gateway
