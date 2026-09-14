#pragma once
#include <dlfcn.h>
#include "edge_gateway/filesystem_compat.hpp"
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace dedup_test {
inline void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
struct Database {
    void* lib = dlopen("libsqlite3.so.0", RTLD_NOW | RTLD_LOCAL);
    void* db = nullptr;
    int (*exec)(void*, const char*, int (*)(void*, int, char**, char**), void*, char**) = nullptr;
    int (*close)(void*) = nullptr;
    explicit Database(const std::string& path) {
        require(lib != nullptr, "SQLite runtime missing");
        const auto open = reinterpret_cast<int (*)(const char*, void**)>(dlsym(lib, "sqlite3_open"));
        exec = reinterpret_cast<decltype(exec)>(dlsym(lib, "sqlite3_exec"));
        close = reinterpret_cast<decltype(close)>(dlsym(lib, "sqlite3_close"));
        require(open && exec && close && open(path.c_str(), &db) == 0, "SQLite test open failed");
    }
    ~Database() { if (db) close(db); if (lib) dlclose(lib); }
    void sql(const std::string& sql) {
        require(exec(db, sql.c_str(), nullptr, nullptr, nullptr) == 0, "test SQL failed: " + sql);
    }
    int count() {
        int n = -1;
        require(exec(db, "SELECT count(*) FROM control_dedup_v1;", [](void* p, int, char** v, char**) {
            *static_cast<int*>(p) = std::stoi(v[0]); return 0;
        }, &n, nullptr) == 0, "count failed");
        return n;
    }
    void failFinalize() {
        sql("CREATE TRIGGER fail_finalize BEFORE UPDATE OF state ON control_dedup_v1 WHEN new.state=2 "
            "BEGIN SELECT RAISE(ABORT,'injected finalize disk failure'); END;");
    }
};
struct Directory {
    edge_gateway::filesystem::path path;
    Directory() {
        static unsigned sequence = 0;
        path = edge_gateway::filesystem::absolute("control-fixture-" + std::to_string(getpid()) + "-" + std::to_string(++sequence));
        edge_gateway::filesystem::create_directories(path);
    }
    ~Directory() { edge_gateway::filesystem::remove_all(path); }
    std::string file(const char* name) const { return (path / name).string(); }
};
}
