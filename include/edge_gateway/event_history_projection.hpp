#pragma once

#include "edge_gateway/event_store.hpp"

namespace edge_gateway {

// Uses the same loaded SQLite engine as EventStore, but an independent FULL connection.
// Thread-confined. Copies source identity at construction; retains no source connection.
// The source file must remain in place. Never run a legacy alarm writer concurrently.
class EventHistoryProjection {
public:
    EventHistoryProjection(EventStoreDatabase& source, const std::string& path);
    ~EventHistoryProjection();
    EventHistoryProjection(const EventHistoryProjection&) = delete;
    EventHistoryProjection& operator=(const EventHistoryProjection&) = delete;
    // Pass a dedicated read-only EventStoreDatabase on a background worker.
    // afterId comes from reconcileProjection; each call commits at most 64 rows.
    std::int64_t replay(EventStoreDatabase& reader, std::int64_t afterId, std::size_t limit);
    std::int64_t watermark();
    // Neither method opens or reads the source database. Materialize a bounded page
    // on a source reader first, releasing its snapshot before touching history.
    std::int64_t projectRows(const std::vector<EventStoreJournalRow>& rows, std::int64_t afterId);
    std::int64_t auditRows(const std::vector<EventStoreJournalRow>& rows, std::int64_t afterId);
private:
    friend class EventStoreDatabase;
    void verify(const EventStoreJournalRow& row);
    // Caller checks physical identity at the bounded page/transaction boundaries.
    void verifyContent(const EventStoreJournalRow& row);
    std::int64_t checkedWatermark();
    void checkIdentity(EventStoreDatabase& source);
    void checkPhysicalFiles();
    void checkMetadata();
    void* database_ = nullptr;
    int lockFd_ = -1;
    int sourceFd_ = -1;
    std::string requestedPath_;
    std::string canonicalPath_;
    std::string sourcePath_;
    std::string canonicalSourcePath_;
    std::string storeId_;
    std::string generation_;
};

} // namespace edge_gateway
