#include "edge_gateway/memory_point_store.hpp"
#include "../src/memory_point_store_layout.hpp"

#include <chrono>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace edge_gateway {
struct MemoryPointStoreReaderCacheTestAccess {
    static std::unordered_map<std::uint32_t, std::size_t> cache(const MemoryPointStore& store) {
        ReadLock lock(store.mutex_);
        return store.latestSlotByIndex_;
    }
    static std::size_t occupiedLatestCount(const MemoryPointStore& store) {
        ReadLock lock(store.mutex_);
        auto* layout = static_cast<memory_layout::SharedStoreLayout*>(store.sharedView_);
        if (pthread_mutex_lock(&layout->header.mutex) != 0)
            throw std::runtime_error("cannot inspect shared latest slots");
        std::size_t count = 0;
        for (const auto& slot : layout->latest) {
            if (slot.occupied) ++count;
        }
        pthread_mutex_unlock(&layout->header.mutex);
        return count;
    }
    static void seedOldCache(MemoryPointStore& store, std::size_t count) {
        WriteLock lock(store.mutex_);
        store.latestSlotByIndex_.clear();
        store.latestSlotByIndex_.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            store.latestSlotByIndex_.emplace(static_cast<std::uint32_t>(1000000 + i), 0);
        }
    }
};
}

namespace {
using namespace edge_gateway;
using Access = MemoryPointStoreReaderCacheTestAccess;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct Segment {
    std::string name = "gateway_reader_cache_" + std::to_string(getpid()) + "_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    ~Segment() { MemoryPointStore::cleanupOrphanedSegment(name); }
};

PointDefinition point(std::uint32_t index) {
    PointDefinition result;
    result.index = index;
    result.pointCode = "POINT_" + std::to_string(index);
    result.enabled = true;
    result.read.cachePolicy.storeLatest = true;
    result.read.cachePolicy.ttlMs = 1000;
    return result;
}

PointValue value(std::uint32_t index, double number = 42.5) {
    PointValue result;
    result.index = index;
    result.machineCode = "GW_TEST";
    result.meterCode = "METER_TEST";
    result.pointCode = point(index).pointCode;
    result.value = number;
    result.quality = -7;
    result.ts = 1000;
    result.expireAt = 2000;
    return result;
}

std::vector<StoredPointValue> read(MemoryPointStore& store, bool batch,
                                 const std::vector<std::uint32_t>& indexes, std::int64_t now = 1000) {
    if (batch) return store.getLatestByIndexes(indexes, now);
    std::vector<StoredPointValue> result;
    for (const auto index : indexes) {
        const auto item = store.getLatestByIndex(index, now);
        if (item) result.push_back(*item);
    }
    return result;
}

void readerBeforeWriter(bool batch) {
    Segment segment;
    MemoryPointStore reader(segment.name);
    require(Access::cache(reader).empty(), "reader must start with an empty cache");
    require(read(reader, batch, {101}).empty(), "unregistered point must be absent");
    require(Access::cache(reader).empty(), "miss must not create a negative cache entry");
    MemoryPointStore writer(segment.name);
    writer.registerPoint("GW_TEST", "METER_TEST", point(101));
    writer.putLatest(value(101));
    const auto first = read(reader, batch, {101});
    require(first.size() == 1 && first[0].value == 42.5, "late writer point is not visible");
    const auto cached = Access::cache(reader);
    require(cached.count(101) == 1, "successful read did not populate reader cache");
    require(cached.at(101) == Access::cache(writer).at(101), "reader cached the wrong slot");
    require(first[0].machineCode.empty() && first[0].meterCode.empty() && first[0].pointCode.empty(),
        "unbound reader unexpectedly acquired producer-local strings");
    writer.putLatest(value(101, 88.0));
    for (int i = 0; i < 8; ++i) {
        const auto hot = read(reader, batch, {101});
        require(hot.size() == 1 && hot[0].value == 88.0, "hot cache did not read the current slot value");
        require(Access::cache(reader) == cached, "hot read lost or changed a valid cache entry");
    }
}

void missingAndDuplicateIndexes(bool batch) {
    Segment segment;
    MemoryPointStore reader(segment.name);
    require(reader.getLatestByIndexes({}, 1000).empty(), "empty batch must be empty");
    const auto absent = std::numeric_limits<std::uint32_t>::max();
    require(read(reader, batch, {0, absent, absent}).empty(), "missing indexes returned a point");
    require(Access::cache(reader).empty(), "missing indexes polluted positive cache");
    MemoryPointStore writer(segment.name);
    writer.putLatest(value(201, 1.0));
    auto results = read(reader, batch, {0, 202, 201, 201, absent});
    require(results.size() == 2 && results[0].index == 201 && results[1].index == 201,
        "missing or duplicate index semantics changed");
    require(Access::cache(reader).size() == 1 && Access::cache(reader).count(201) == 1,
        "only discovered positive indexes should be cached");
    writer.registerPoint("GW_TEST", "METER_TEST", point(202));
    writer.putLatest(value(202, 2.0));
    results = read(reader, batch, {202, 201, 202, absent});
    require(results.size() == 3 && results[0].value == 2.0 &&
        results[1].value == 1.0 && results[2].value == 2.0,
        "a formerly missing point remained invisible or batch order changed");
    require(Access::cache(reader).size() == 2, "late discovery did not populate positive cache");
}

void deletedAndReusedSlots(bool batch) {
    Segment segment;
    MemoryPointStore vacantReader(segment.name);
    MemoryPointStore reusedReader(segment.name);
    MemoryPointStore relocatedReader(segment.name);
    MemoryPointStore writer(segment.name);
    writer.putLatest(value(301));
    for (auto* reader : {&vacantReader, &reusedReader, &relocatedReader}) {
        require(read(*reader, batch, {301}).size() == 1, "initial read failed");
        require(Access::cache(*reader).count(301) == 1, "reuse fixture cache was not populated");
    }
    const auto oldSlot = Access::cache(writer).at(301);
    writer.removeExpired(2001);
    require(read(vacantReader, batch, {301}).empty(), "unoccupied cached slot returned a point");
    require(Access::cache(vacantReader).count(301) == 0, "deleted slot remained cached");
    writer.putLatest(value(302, 302.0));
    require(Access::cache(writer).at(302) == oldSlot, "fixture did not reuse the deleted slot");
    require(read(reusedReader, batch, {301}).empty(), "cached slot returned a different index");
    require(Access::cache(reusedReader).count(301) == 0, "reused invalid slot remained cached");
    writer.putLatest(value(301, 301.0));
    const auto relocated = read(relocatedReader, batch, {301, 302});
    require(relocated.size() == 2 && relocated[0].index == 301 && relocated[0].value == 301.0 &&
        relocated[1].index == 302 && relocated[1].value == 302.0, "slot reuse mixed point values");
    require(Access::cache(relocatedReader).at(301) == Access::cache(writer).at(301) &&
        Access::cache(relocatedReader).at(301) != oldSlot, "relocated positive slot was not recached");
}

void remappingClearsOldCache(bool batch) {
    Segment segment;
    MemoryPointStore reader(segment.name);
    {
        MemoryPointStore writer(segment.name);
        writer.putLatest(value(401));
        writer.putLatest(value(402));
        require(read(reader, batch, {401, 402}).size() == 2, "initial mapping read failed");
        require(Access::cache(reader).size() == 2, "initial mapping cache was not populated");
    }
    require(MemoryPointStore::cleanupOrphanedSegment(segment.name), "old segment was not unlinked");
    MemoryPointStore replacement(segment.name);
    replacement.putLatest(value(403, 403.0));
    const auto current = read(reader, batch, {403});
    require(current.size() == 1 && current[0].value == 403.0, "reader did not remap");
    const auto cache = Access::cache(reader);
    require(cache.size() == 1 && cache.count(403) == 1,
        "remap retained entries for unqueried indexes from the old mapping");
    require(read(reader, batch, {401, 402}).empty(), "old mapping values leaked after remap");
    replacement.putLatest(value(401, 401.0));
    const auto late = read(reader, batch, {401});
    require(late.size() == 1 && late[0].value == 401.0 && Access::cache(reader).count(401) == 1,
        "positive discovery failed in the replacement mapping");
}

void valueSemantics(bool batch) {
    Segment segment;
    MemoryPointStore reader(segment.name);
    MemoryPointStore writer(segment.name);
    writer.putLatest(value(501));
    require(read(reader, batch, {501}).size() == 1, "initial semantic read failed");
    reader.registerPoint("GW_TEST", "METER_TEST", point(501));
    for (const auto now : {2000, 2001}) {
        const auto result = read(reader, batch, {501}, now).at(0);
        require(result.index == 501 && result.value == 42.5 && result.quality == -7 &&
            result.ts == 1000 && result.expireAt == 2000 && result.stale == (now > 2000),
            "value/quality/timestamp/TTL boundary semantics changed");
        require(result.machineCode == "GW_TEST" && result.meterCode == "METER_TEST" &&
            result.pointCode == "POINT_501", "bound point strings changed");
    }
    auto updated = value(501, -12.25);
    updated.quality = 0;
    updated.ts = 3000;
    updated.expireAt = 0;
    updated.stale = true;
    writer.putLatest(updated);
    const auto result = read(reader, batch, {501}, 9000).at(0);
    require(result.value == -12.25 && result.quality == 0 && result.ts == 3000 &&
        result.expireAt == 0 && !result.stale, "stale must retain existing expiry-derived semantics");
}

void cacheCapacity(bool batch) {
    // Matches the physical slot count without 100,000 SHM scans or stored points.
    constexpr std::size_t capacity = 100000;
    Segment segment;
    MemoryPointStore reader(segment.name);
    MemoryPointStore writer(segment.name);
    writer.putLatest(value(701, 701.0));
    writer.putLatest(value(702, 702.0));
    Access::seedOldCache(reader, capacity - 1);
    require(read(reader, batch, {701}).at(0).value == 701.0, "read at cache capacity failed");
    require(Access::cache(reader).size() == capacity, "fixture did not reach cache capacity");
    require(read(reader, batch, {701}).at(0).value == 701.0, "hot read at capacity failed");
    require(Access::cache(reader).size() == capacity, "existing cache hit unnecessarily cleared cache");
    // In batch mode, 701 is in the initial slot snapshot but must be recached after 702 clears it.
    const auto result = read(reader, batch, {702, 701, 702});
    require(result.size() == 3 && result[0].value == 702.0 && result[1].value == 701.0 &&
        result[2].value == 702.0, "cache overflow changed reader values or duplicate ordering");
    const auto cache = Access::cache(reader);
    require(cache.size() <= capacity, "reader cache exceeded the physical slot limit");
    require(cache.size() == 2 && cache.count(701) == 1 && cache.count(702) == 1,
        "cache overflow must discard old keys and remember subsequent positive hits");
    require(writer.getStats().latestCount == 2, "clearing reader cache changed shared point data");
}

void concurrentReaders() {
    Segment segment;
    MemoryPointStore reader(segment.name);
    MemoryPointStore writer(segment.name);
    writer.putLatest(value(601));
    writer.putLatest(value(602));
    std::promise<void> start;
    const auto ready = start.get_future().share();
    auto worker = [&](bool batch) {
        ready.wait();
        for (int i = 0; i < 64; ++i) {
            const auto result = read(reader, batch, {601, 602});
            require(result.size() == 2 && result[0].index == 601 && result[1].index == 602,
                "concurrent reader returned mismatched indexes");
        }
    };
    auto single = std::async(std::launch::async, worker, false);
    auto batch = std::async(std::launch::async, worker, true);
    start.set_value();
    single.get();
    batch.get();
    require(Access::cache(reader).size() == 2, "concurrent discoveries did not populate cache");
}

void requireLatestCount(MemoryPointStore& store, std::size_t expected) {
    // getAllLatest merges duplicate indexes, so inspect physical slots as well.
    const auto actual = Access::occupiedLatestCount(store);
    require(actual == expected, "expected " + std::to_string(expected) +
        " occupied latest slots, got " + std::to_string(actual));
    require(store.getStats().latestCount == expected, "shared latest count changed");
    require(store.getAllLatest(1000).size() == expected, "latest indexes are not unique");
}

void writersAfterSharedInsert(std::uint32_t version) {
    Segment segment;
    MemoryStoreConfig config;
    config.sharedMemoryName = segment.name;
    config.sharedMemoryCreateVersion = version;
    MemoryPointStore first(config);
    MemoryPointStore second(config);
    require(Access::cache(first).empty() && Access::cache(second).empty(),
        "both writers must attach before the first insertion");
    first.putLatest(value(801, 1.0));
    require(Access::cache(second).count(801) == 0, "fixture must exercise a writer cache miss");
    second.putLatest(value(801, 2.0));
    requireLatestCount(second, 1);
    const auto cached = Access::cache(second);
    require(cached.at(801) == Access::cache(first).at(801),
        "writer miss did not cache the existing shared slot");
    for (int i = 0; i < 8; ++i) {
        auto& writer = i % 2 == 0 ? first : second;
        writer.putLatest(value(801, 10.0 + i));
        for (auto* observer : {&first, &second}) {
            for (const bool batch : {false, true}) {
                const auto result = read(*observer, batch, {801});
                require(result.size() == 1 && result[0].value == 10.0 + i,
                    "writers disagree on the latest value after a hot write");
            }
        }
    }
    require(Access::cache(second) == cached, "hot writes changed a valid slot cache");
    requireLatestCount(first, 1);
}

void writerAfterSlotReuse(bool reuse, bool relocate) {
    Segment segment;
    MemoryPointStore first(segment.name);
    first.putLatest(value(811, 1.0));
    MemoryPointStore second(segment.name);
    const auto oldSlot = Access::cache(first).at(811);
    second.removeExpired(2001);
    requireLatestCount(second, 0);
    if (reuse) {
        second.putLatest(value(812, 12.0));
        require(Access::cache(second).at(812) == oldSlot, "fixture did not reuse the old slot");
    }
    if (relocate) {
        second.putLatest(value(811, 2.0));
        require(Access::cache(second).at(811) != oldSlot, "fixture did not relocate the index");
    }
    require(Access::cache(first).at(811) == oldSlot, "fixture lost its stale writer cache");
    first.putLatest(value(811, 3.0));
    requireLatestCount(first, reuse ? 2 : 1);
    if (relocate) {
        require(Access::cache(first).at(811) == Access::cache(second).at(811),
            "stale writer cache did not discover the relocated shared slot");
    }
    for (auto* observer : {&first, &second}) {
        for (const bool batch : {false, true}) {
            require(read(*observer, batch, {811}).at(0).value == 3.0,
                "slot reuse left a writer reading an older duplicate");
            if (reuse) require(read(*observer, batch, {812}).at(0).value == 12.0,
                "stale writer cache overwrote another index");
        }
    }
}

void writerMissAtConfiguredLimit() {
    Segment segment;
    MemoryStoreConfig config;
    config.sharedMemoryName = segment.name;
    config.maxLatestPoints = 1;
    MemoryPointStore first(config);
    MemoryPointStore second(config);
    first.putLatest(value(821, 1.0));
    second.putLatest(value(821, 2.0));
    requireLatestCount(second, 1);
    require(Access::cache(second).at(821) == Access::cache(first).at(821),
        "update at capacity did not backfill the existing shared slot");
    bool refused = false;
    try {
        second.putLatest(value(822));
    } catch (const std::runtime_error& ex) {
        refused = std::string(ex.what()).find("configured limit") != std::string::npos;
    }
    require(refused, "a genuinely new index must still respect the configured limit");
    requireLatestCount(second, 1);
    require(second.getLatestByIndex(821, 1000)->value == 2.0,
        "capacity refusal changed the existing value");
}

}  // namespace

int main() {
    int failures = 0;
    const auto check = [&](const std::string& name, const std::function<void()>& test) {
        try {
            test();
            std::cout << name << " passed\n";
        } catch (const std::exception& ex) {
            ++failures;
            std::cerr << name << " failed: " << ex.what() << "\n";
        }
    };
    for (const bool batch : {false, true}) {
        const auto prefix = std::string(batch ? "batch " : "single ");
        check(prefix + "reader-before-writer", [&] { readerBeforeWriter(batch); });
        check(prefix + "missing/duplicates", [&] { missingAndDuplicateIndexes(batch); });
        check(prefix + "deleted/reused/relocated", [&] { deletedAndReusedSlots(batch); });
        check(prefix + "remap", [&] { remappingClearsOldCache(batch); });
        check(prefix + "value/quality/stale", [&] { valueSemantics(batch); });
        check(prefix + "cache capacity", [&] { cacheCapacity(batch); });
    }
    check("concurrent readers", concurrentReaders);
    // Legacy formats are exercised by the frozen-layout offline migration suite.
    for (const std::uint32_t version : {11}) {
        check("writers shared insert v" + std::to_string(version), [&] { writersAfterSharedInsert(version); });
    }
    check("writer stale vacant slot", [] { writerAfterSlotReuse(false, false); });
    check("writer stale reused slot", [] { writerAfterSlotReuse(true, false); });
    check("writer stale relocated index", [] { writerAfterSlotReuse(true, true); });
    check("writer miss at configured limit", writerMissAtConfiguredLimit);
    return failures == 0 ? 0 : 1;
}
