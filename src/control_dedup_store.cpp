#include "edge_gateway/control_dedup_store.hpp"
#include "edge_gateway/writeback_service.hpp"

#include <cmath>
#include "edge_gateway/filesystem_compat.hpp"
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <utility>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace edge_gateway {
namespace {
struct sqlite3;
struct sqlite3_stmt;

// Same runtime SQLite dependency as the existing sample/alarm writers, with
// per-connection state and function-local initialization for concurrent callers.
struct Api {
    void* library = nullptr;
    int (*open)(const char*, sqlite3**, int, const char*) = nullptr;
    int (*close)(sqlite3*) = nullptr;
    int (*exec)(sqlite3*, const char*, void*, void*, char**) = nullptr;
    int (*prepare)(sqlite3*, const char*, int, sqlite3_stmt**, const char**) = nullptr;
    int (*bind)(sqlite3_stmt*, int, const char*, int, void (*)(void*)) = nullptr;
    int (*step)(sqlite3_stmt*) = nullptr;
    int (*finalize)(sqlite3_stmt*) = nullptr;
    const unsigned char* (*column)(sqlite3_stmt*, int) = nullptr;
    const char* (*error)(sqlite3*) = nullptr;
    int (*busy)(sqlite3*, int) = nullptr;
    int (*changes)(sqlite3*) = nullptr;
    template<class T> void load(T& fn, const char* name) {
#ifdef _WIN32
        fn = reinterpret_cast<T>(GetProcAddress(static_cast<HMODULE>(library), name));
#else
        fn = reinterpret_cast<T>(dlsym(library, name));
#endif
        if (!fn) throw std::runtime_error("control dedup SQLite symbol unavailable");
    }
    Api() {
#ifdef _WIN32
        library = LoadLibraryA("sqlite3.dll");
#else
        library = dlopen("libsqlite3.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!library) library = dlopen("libsqlite3.so", RTLD_NOW | RTLD_LOCAL);
#endif
        if (!library) throw std::runtime_error("control dedup SQLite unavailable");
        load(open, "sqlite3_open_v2"); load(close, "sqlite3_close_v2");
        load(exec, "sqlite3_exec"); load(prepare, "sqlite3_prepare_v2");
        load(bind, "sqlite3_bind_text"); load(step, "sqlite3_step");
        load(finalize, "sqlite3_finalize"); load(column, "sqlite3_column_text");
        load(error, "sqlite3_errmsg"); load(busy, "sqlite3_busy_timeout");
        load(changes, "sqlite3_changes");
    }
};
Api& api() { static Api value; return value; }

struct Db {
    sqlite3* handle = nullptr;
    explicit Db(const std::string& path, bool write) {
        if (write) edge_gateway::filesystem::create_directories(edge_gateway::filesystem::path(path).parent_path());
        auto& a = api();
        if (a.open(path.c_str(), &handle, (write ? 6 : 1) | 0x10000, nullptr) != 0) {
            if (handle) a.close(handle);
            handle = nullptr;
            throw std::runtime_error("control dedup database unavailable");
        }
        try {
            if (a.busy(handle, 100) != 0) throw std::runtime_error("control dedup busy setup failed");
            if (write) {
                sql("PRAGMA synchronous=FULL;");
                sql("PRAGMA journal_mode=DELETE;");
                sql("CREATE TABLE IF NOT EXISTS control_dedup_v1("
                    "machine TEXT NOT NULL,meter TEXT NOT NULL,cmd TEXT NOT NULL,"
                    "semantic TEXT NOT NULL,wire TEXT NOT NULL,state INTEGER NOT NULL,"
                    "result TEXT NOT NULL,PRIMARY KEY(machine,cmd)) WITHOUT ROWID;");
            }
        } catch (...) { a.close(handle); handle = nullptr; throw; }
    }
    ~Db() { if (handle) api().close(handle); }
    void sql(const char* sql) {
        if (api().exec(handle, sql, nullptr, nullptr, nullptr) != 0)
            throw std::runtime_error(std::string("control dedup: ") + api().error(handle));
    }
};
struct Statement {
    Db& db;
    sqlite3_stmt* stmt = nullptr;
    Statement(Db& db, const char* sql, const std::vector<std::string>& args = {}) : db(db) {
        if (api().prepare(db.handle, sql, -1, &stmt, nullptr) != 0)
            throw std::runtime_error(api().error(db.handle));
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (api().bind(stmt, static_cast<int>(i + 1), args[i].data(), static_cast<int>(args[i].size()),
                           reinterpret_cast<void (*)(void*)>(-1)) != 0) {
                api().finalize(stmt); stmt = nullptr;
                throw std::runtime_error("control dedup bind failed");
            }
        }
    }
    ~Statement() { if (stmt) api().finalize(stmt); }
    bool row() {
        const int rc = api().step(stmt);
        if (rc != 100 && rc != 101) throw std::runtime_error(api().error(db.handle));
        return rc == 100;
    }
    std::string text(int col) const {
        const auto* value = api().column(stmt, col);
        return value ? reinterpret_cast<const char*>(value) : "";
    }
};
struct Transaction {
    Db& db;
    bool done = false;
    explicit Transaction(Db& db) : db(db) { db.sql("BEGIN IMMEDIATE;"); }
    ~Transaction() { if (!done) api().exec(db.handle, "ROLLBACK;", nullptr, nullptr, nullptr); }
    void commit() { db.sql("COMMIT;"); done = true; }
};

std::string semantics(const PendingWriteCommand& c, bool wire) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << c.index << ' ' << std::setprecision(std::numeric_limits<double>::max_digits10)
        << (c.value == 0 ? 0 : c.value) << ' ' << std::quoted(wire ? c.source.substr(0, 31) : c.source)
        << ' ' << c.highPriority << ' ' << c.controlGeneration;
    return out.str();
}
std::string encode(const WritebackResultRecord& r) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(17) << std::quoted(r.cmdId) << ' ' << r.index << ' ' << r.value << ' '
        << r.success << ' ' << std::quoted(r.message.substr(0, 127)) << ' ' << std::quoted(r.stage) << ' '
        << r.requestedAt << ' ' << r.acceptedAt << ' ' << r.startedAt << ' ' << r.completedAt << ' '
        << r.queueDelayMs << ' ' << r.deviceWriteMs << ' ' << r.edgeElapsedMs << ' ' << r.totalElapsedMs << ' '
        << r.verifyAttempted << ' ' << r.verifyPassed << ' ' << r.highPriority;
    return out.str();
}
WritebackResultRecord decode(const std::string& data) {
    WritebackResultRecord r;
    std::istringstream in(data);
    in.imbue(std::locale::classic());
    if (!(in >> std::quoted(r.cmdId) >> r.index >> r.value >> r.success >> std::quoted(r.message)
          >> std::quoted(r.stage) >> r.requestedAt >> r.acceptedAt >> r.startedAt >> r.completedAt
          >> r.queueDelayMs >> r.deviceWriteMs >> r.edgeElapsedMs >> r.totalElapsedMs
          >> r.verifyAttempted >> r.verifyPassed >> r.highPriority))
        throw std::runtime_error("control dedup corrupt result");
    return r;
}
struct Row { std::string semantic, wire, state, result, meter; };
Optional<Row> read(Db& db, const std::string& machine, const std::string& meter, const std::string& id) {
    Statement query(db, "SELECT semantic,wire,state,result,meter FROM control_dedup_v1 WHERE machine=? AND cmd=?;",
                    {machine, id});
    if (!query.row()) return NullOpt;
    return Row{query.text(0), query.text(1), query.text(2), query.text(3), query.text(4)};
}
WritebackResultRecord unknown(WritebackResultRecord result, const std::string& message) {
    result.success = false;
    result.stage = "writeback-timeout";
    result.message = message;
    return result;
}
void bounded(const std::string& text, std::size_t max) {
    if (text.empty() || text.size() > max || text.find('\0') != std::string::npos)
        throw std::invalid_argument("control dedup identity/source is empty or too long");
}
}

ControlDedupStore::ControlDedupStore(std::string path, std::size_t capacity)
    : path_(std::move(path)), capacity_(capacity) {
    if (!edge_gateway::filesystem::path(path_).is_absolute() || capacity_ == 0 || capacity_ > 100000)
        throw std::invalid_argument("control dedup requires an absolute persistent path and capacity 1..100000");
}

Optional<WritebackResultRecord> ControlDedupStore::bind(const std::string& machine, const std::string& meter,
                                                     const PendingWriteCommand& c) {
    bounded(machine, 128); bounded(meter, 128); bounded(c.source, 256); validateControlCommandId(c.cmdId);
    if (!c.index || !std::isfinite(c.value)) throw std::invalid_argument("invalid control target/value");
    const auto inspect = [&](const Row& row) -> Optional<WritebackResultRecord> {
        if (row.meter != meter || row.semantic != semantics(c, false)) throw std::invalid_argument("cmdId conflicts with retained control semantics");
        if (row.state == "2") return decode(row.result);
        if (row.state == "1") return unknown(decode(row.result), "retained control outcome unknown; not redispatched");
        if (row.state != "0") throw std::runtime_error("control dedup corrupt state");
        return NullOpt;
    };
    // Existing receipts remain readable even when writes/capacity are unavailable.
    if (edge_gateway::filesystem::exists(path_)) {
        Db db(path_, false);
        const auto row = read(db, machine, meter, c.cmdId);
        if (row) return inspect(*row);
    }
    Db db(path_, true);
    Transaction tx(db);
    const auto row = read(db, machine, meter, c.cmdId);
    if (row) { auto result = inspect(*row); tx.commit(); return result; }
    Statement count(db, "SELECT count(*) FROM control_dedup_v1;");
    if (!count.row() || std::stoull(count.text(0)) >= capacity_)
        throw std::runtime_error("control dedup capacity exhausted; new operation rejected");
    Statement insert(db, "INSERT INTO control_dedup_v1 VALUES(?,?,?,?,?,0,?);",
                     {machine, meter, c.cmdId, semantics(c, false), semantics(c, true), encode(beginWritebackResult(c, 0))});
    insert.row();
    tx.commit();
    return NullOpt;
}

ControlDedupStore::Claim ControlDedupStore::claim(const std::string& machine, const std::string& meter,
                                                const PendingWriteCommand& c, std::int64_t now) {
    const auto inspect = [&](const Optional<Row>& row) -> Claim {
        if (!row) throw std::runtime_error("durable control registration missing; no dispatch");
        if (row->meter != meter || row->wire != semantics(c, true)) throw std::invalid_argument("cmdId conflicts with registered control");
        if (row->state == "2") return {false, decode(row->result)};
        if (row->state == "1") return {false, unknown(decode(row->result), "retained control outcome unknown; not redispatched")};
        if (row->state != "0") throw std::runtime_error("control dedup corrupt state");
        return {true, beginWritebackResult(c, now)};
    };
    { Db db(path_, false); auto found = inspect(read(db, machine, meter, c.cmdId)); if (!found.owner) return found; }
    Db db(path_, true);
    Transaction tx(db);
    auto found = inspect(read(db, machine, meter, c.cmdId));
    if (found.owner) {
        Statement update(db, "UPDATE control_dedup_v1 SET state=1,result=? WHERE machine=? AND meter=? AND cmd=? AND state=0;",
                         {encode(found.result), machine, meter, c.cmdId});
        update.row();
        if (api().changes(db.handle) != 1) throw std::runtime_error("control reservation failed");
    }
    tx.commit();
    return found;
}

void ControlDedupStore::finish(const std::string& machine, const std::string& meter, const WritebackResultRecord& result) {
    Db db(path_, true);
    Transaction tx(db);
    Statement update(db, "UPDATE control_dedup_v1 SET state=2,result=? WHERE machine=? AND meter=? AND cmd=? AND state=1;",
                     {encode(result), machine, meter, result.cmdId});
    update.row();
    if (api().changes(db.handle) != 1) throw std::runtime_error("control finalize lost reservation");
    tx.commit();
}

Optional<WritebackResultRecord> ControlDedupStore::result(const std::string& machine, const std::string& meter,
                                                       const std::string& id) const {
    Db db(path_, false);
    const auto row = read(db, machine, meter, id);
    if (row && row->meter != meter) throw std::invalid_argument("cmdId meter conflict");
    return row && row->state == "2" ? Optional<WritebackResultRecord>(decode(row->result)) : NullOpt;
}

WritebackResultRecord ControlDedupStore::dispatch(const std::string& machine, const std::string& meter,
                                                const PendingWriteCommand& c, std::int64_t now,
                                                const std::function<WritebackResultRecord()>& execute) {
    bool reserved = false;
    try {
        const auto claimed = claim(machine, meter, c, now);
        if (!claimed.owner) return claimed.result;
        reserved = true;
        auto result = execute();
        if (!result.success) result = unknown(result, result.message);
        finish(machine, meter, result);
        return result;
    } catch (const std::exception& ex) {
        auto result = beginWritebackResult(c, now);
        completeWritebackResult(result, false, ex.what(), reserved ? "writeback-timeout" : "writeback-failed", now);
        return result;
    } catch (...) {
        auto result = beginWritebackResult(c, now);
        completeWritebackResult(result, false, "non-standard control exception",
                               reserved ? "writeback-timeout" : "writeback-failed", now);
        return result;
    }
}
} // namespace edge_gateway
