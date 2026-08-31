#include <cstdlib>
#include <cstring>

extern "C" {

struct sqlite3 {};
struct sqlite3_stmt {
    bool finalized = false;
};

namespace {

sqlite3 g_database;
sqlite3_stmt g_statements[32];
int g_statementCount = 0;
int g_finalizeCount = 0;
int g_doubleFinalizeCount = 0;
int g_rollbackCount = 0;
bool g_failCommit = true;

void setErrorMessage(char** errorMessage, const char* message) {
    if (errorMessage == nullptr) {
        return;
    }
    const auto size = std::strlen(message) + 1;
    auto* copy = static_cast<char*>(std::malloc(size));
    std::memcpy(copy, message, size);
    *errorMessage = copy;
}

}  // namespace

void fake_sqlite_reset() {
    for (auto& statement : g_statements) {
        statement = sqlite3_stmt{};
    }
    g_statementCount = 0;
    g_finalizeCount = 0;
    g_doubleFinalizeCount = 0;
    g_rollbackCount = 0;
    g_failCommit = true;
}

void fake_sqlite_set_commit_failure(int enabled) {
    g_failCommit = enabled != 0;
}

int fake_sqlite_finalize_count() {
    return g_finalizeCount;
}

int fake_sqlite_double_finalize_count() {
    return g_doubleFinalizeCount;
}

int fake_sqlite_rollback_count() {
    return g_rollbackCount;
}

int sqlite3_open_v2(const char*, sqlite3** database, int, const char*) {
    *database = &g_database;
    return 0;
}

int sqlite3_close_v2(sqlite3*) {
    return 0;
}

int sqlite3_exec(sqlite3*, const char* sql, int (*)(void*, int, char**, char**), void*, char** errorMessage) {
    if (std::strcmp(sql, "COMMIT;") == 0 && g_failCommit) {
        setErrorMessage(errorMessage, "injected commit failure");
        return 5;
    }
    if (std::strcmp(sql, "ROLLBACK;") == 0) {
        ++g_rollbackCount;
    }
    return 0;
}

int sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt** statement, const char**) {
    if (g_statementCount >= static_cast<int>(sizeof(g_statements) / sizeof(g_statements[0]))) {
        return 7;
    }
    *statement = &g_statements[g_statementCount++];
    return 0;
}

int sqlite3_bind_int(sqlite3_stmt*, int, int) {
    return 0;
}

int sqlite3_bind_int64(sqlite3_stmt*, int, long long) {
    return 0;
}

int sqlite3_bind_double(sqlite3_stmt*, int, double) {
    return 0;
}

int sqlite3_bind_text(sqlite3_stmt*, int, const char*, int, void (*)(void*)) {
    return 0;
}

int sqlite3_step(sqlite3_stmt*) {
    return 101;
}

int sqlite3_reset(sqlite3_stmt*) {
    return 0;
}

int sqlite3_clear_bindings(sqlite3_stmt*) {
    return 0;
}

int sqlite3_finalize(sqlite3_stmt* statement) {
    if (statement->finalized) {
        ++g_doubleFinalizeCount;
        return 21;
    }
    statement->finalized = true;
    ++g_finalizeCount;
    return 0;
}

const char* sqlite3_errmsg(sqlite3*) {
    return "fake sqlite error";
}

void sqlite3_free(void* memory) {
    std::free(memory);
}

int sqlite3_busy_timeout(sqlite3*, int) {
    return 0;
}

}  // extern "C"
