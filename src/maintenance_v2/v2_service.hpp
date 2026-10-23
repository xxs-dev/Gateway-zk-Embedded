#pragma once

// Maintenance v2 request handling (transport-independent; v2_server.cpp owns TLS).

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "edge_gateway/maintenance_v2.hpp"
#include "v2_state.hpp"
#include "v2_ota.hpp"
#include "v2_ticket.hpp"
#include "v2_tx.hpp"

namespace edge_gateway {
namespace maintenance_v2 {

struct Request {
    std::string method;
    std::string path;
    std::string query;
    std::string body;
    std::string clientIp;
    std::map<std::string, std::string> headers;  // lower-case names
    // POST /ota/uploads only: the server does not buffer the body. `body` holds the bytes that arrived with
    // the headers; readBody reads the rest (>0 bytes, 0 closed, <0 error / timeout) up to contentLength.
    bool bodyStreamed = false;
    std::uint64_t contentLength = 0;
    std::function<int(char*, int)> readBody;
    std::string header(const std::string& name) const;
    std::string queryValue(const std::string& name) const;
};

// Long-lived responses (text/event-stream). The writer returns false once the client is gone or the
// server is stopping; an empty chunk writes nothing and only reports that.
using StreamWriter = std::function<bool(const std::string& chunk)>;

struct Response {
    int status = 200;
    std::string body;
    std::string contentType = "application/json; charset=utf-8";
    std::function<void(const StreamWriter&)> stream;  // set: SSE; status/body ignored
};

std::string sseEvent(const std::string& event, const std::string& data);

Response jsonResponse(int status, const std::string& body);
Response errorResponse(int status, const std::string& code, const std::string& message = std::string());

class Service {
public:
    Service(const DirectConfig& config, const Options& options, std::string certFingerprint);
    Response handle(const Request& request);
    // Endpoints answered without a session (identity, status, read-only points, login).
    static bool isPublic(const std::string& method, const std::string& path);
    // Before reading a request body: does it carry a live session token? (No side effects.)
    bool sessionValid(const Request& request);
    void housekeeping();  // debug-mode idle exit, interrupted transactions and OTA jobs
    const DirectConfig& config() const { return config_; }

private:
    struct Auth {
        std::string token;
    };
    // read-only, no token
    Response health();
    Response identity();
    Response authState();
    Response systemStatus();
    Response telemetryFull();
    Response pointsQuery(const Request& request);
    Response login(const Request& request);
    // token
    Response logout(const Request& request, const Auth& auth);
    Response otaStatus();
    Response snapshot(const Request& request);
    Response diag(const Request& request);
    Response debugGet();
    Response debugPost(const Request& request);
    Response auditGet(const Request& request);
    Response control(const Request& request);
    // streams and logs (v2_stream.cpp)
    Response realtimeStream(const Request& request);
    Response logs(const Request& request);
    std::vector<std::string> logUnits();
    // config (v2_config.cpp)
    Response configApply(const Request& request);
    Response configRestore(const Request& request);
    Response configDelete(const Request& request);
    Response transaction(const std::string& id);
    // offline upgrade (v2_ota_api.cpp)
    Response otaUpload(const Request& request);
    Response otaJob(const Request& request);
    Response otaJobStatus(const std::string& id);
    void otaHousekeeping();

    bool authenticate(const Request& request, Response* deny, Auth* auth);
    // Steps 1-8; `replayOf` = ticketId already bound to the same request (idempotent resend).
    TicketVerdict checkTicket(const Request& request, const std::string& action, int level, const std::vector<std::string>& paths,
                              const std::vector<std::uint32_t>& points, const std::string& replayOf, bool consume);
    std::vector<std::string> expectedUnits();
    std::string deviceMachineCode();
    void record(const std::string& action, const Ticket& ticket, const std::string& target, const std::string& result,
                const std::string& transactionId = std::string());

    DirectConfig config_;
    Options options_;
    std::string fingerprint_;
    SessionStore sessions_;
    ReplayStore replay_;
    AuditLog audit_;
    DebugModeStore debug_;
    TxStore txs_;
    OtaStore otas_;
    std::mutex configMutex_;  // apply / restore admission
    std::mutex unitsMutex_;
    std::atomic<int> streams_{0};
    std::vector<std::string> unitsCache_;
    std::int64_t unitsCachedAt_ = 0;
};

}  // namespace maintenance_v2
}  // namespace edge_gateway
