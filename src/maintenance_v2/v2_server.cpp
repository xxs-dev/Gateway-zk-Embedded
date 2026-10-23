// Maintenance v2: HTTPS listener (one short-lived thread per connection, bounded),
// device certificate initialisation and the transaction helper entry point.
#include "edge_gateway/maintenance_v2.hpp"

#include "edge_gateway/system_monitor_direct_maintenance_bridge.hpp"
#include "v2_crypto.hpp"
#include "v2_files.hpp"
#include "v2_json.hpp"
#include "v2_process.hpp"
#include "v2_service.hpp"
#include "v2_tx.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <memory>
#include <thread>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace edge_gateway {
namespace maintenance_v2 {

namespace bridge = system_monitor_direct_maintenance::bridge;

namespace {

std::atomic<bool> g_running(false);
std::atomic<int> g_active(0);

std::string certFile(const Options& o) { return o.materialDirectory + "/server.crt"; }
std::string keyFile(const Options& o) { return o.materialDirectory + "/server.key"; }

const char* reasonPhrase(int status) {
    switch (status) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    default: return "Status";
    }
}

std::string serialize(const Response& r) {
    std::string out = "HTTP/1.1 " + std::to_string(r.status) + " " + reasonPhrase(r.status) + "\r\n";
    out += "Content-Type: " + r.contentType + "\r\n";
    out += "Cache-Control: no-store\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\n";
    out += "Content-Length: " + std::to_string(r.body.size()) + "\r\n\r\n";
    out += r.body;
    return out;
}

std::string trim(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

#ifndef _WIN32

// Reads one request. Returns 0 when complete, otherwise an HTTP status to answer with
// (or -1 to just close the connection).
int readRequest(TlsConnection& tls, const DirectConfig& config, Request* request) {
    std::string data;
    char buffer[16384];
    std::size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos) {
        const int n = tls.read(buffer, sizeof(buffer));
        if (n <= 0) return -1;
        data.append(buffer, static_cast<std::size_t>(n));
        headerEnd = data.find("\r\n\r\n");
        if (headerEnd == std::string::npos && data.size() > config.maxHeaderBytes) return 431;
    }
    if (headerEnd > config.maxHeaderBytes) return 431;
    const auto head = data.substr(0, headerEnd);
    std::size_t lineEnd = head.find("\r\n");
    const auto requestLine = head.substr(0, lineEnd);
    const auto sp1 = requestLine.find(' ');
    const auto sp2 = requestLine.find(' ', sp1 == std::string::npos ? 0 : sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return 400;
    request->method = requestLine.substr(0, sp1);
    auto target = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);
    const auto q = target.find('?');
    request->path = target.substr(0, q);
    request->query = q == std::string::npos ? std::string() : target.substr(q + 1);
    std::size_t pos = lineEnd == std::string::npos ? head.size() : lineEnd + 2;
    while (pos < head.size()) {
        auto next = head.find("\r\n", pos);
        if (next == std::string::npos) next = head.size();
        const auto line = head.substr(pos, next - pos);
        pos = next + 2;
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        auto name = trim(line.substr(0, colon));
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        request->headers[name] = trim(line.substr(colon + 1));
    }
    if (!request->header("transfer-encoding").empty()) return 411;
    std::size_t length = 0;
    const auto lengthHeader = request->header("content-length");
    if (!lengthHeader.empty()) {
        try {
            const auto parsed = std::stoull(lengthHeader);
            if (parsed > config.maxRequestBytes) return 413;
            length = static_cast<std::size_t>(parsed);
        } catch (...) {
            return 400;
        }
    } else if (request->method == "POST") {
        return 411;
    }
    std::string body = data.substr(headerEnd + 4);
    while (body.size() < length) {
        const int n = tls.read(buffer, sizeof(buffer));
        if (n <= 0) return -1;
        body.append(buffer, static_cast<std::size_t>(n));
    }
    if (body.size() > length) body.resize(length);
    request->body.swap(body);
    return 0;
}

void serveConnection(int fd, std::string clientIp, std::shared_ptr<TlsServerContext> tlsContext, std::shared_ptr<Service> service) {
    const auto& config = service->config();
    timeval timeout {};
    timeout.tv_sec = std::max(1, config.requestTimeoutMs / 1000);
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    timeval sendTimeout {};
    sendTimeout.tv_sec = 30;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &sendTimeout, sizeof(sendTimeout));
    {
        TlsConnection tls(*tlsContext, fd);
        std::string error;
        if (tls.accept(&error)) {
            Request request;
            request.clientIp = clientIp;
            const int rc = readRequest(tls, config, &request);
            if (rc >= 0) {
                Response response = rc == 0 ? service->handle(request) : errorResponse(rc, "bad_request", "malformed request");
                if (response.stream) {
                    // Server-sent events: no Content-Length; the handler writes until the client leaves or we stop.
                    const std::string head = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream; charset=utf-8\r\n"
                                             "Cache-Control: no-store\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\n\r\n";
                    bool alive = tls.writeAll(head);
                    if (alive) {
                        response.stream([&](const std::string& chunk) {
                            if (!alive || !g_running) return false;
                            if (!chunk.empty()) alive = tls.writeAll(chunk);
                            return alive;
                        });
                    }
                } else {
                    tls.writeAll(serialize(response));
                }
            }
            tls.shutdown();
        }
    }
    ::close(fd);
    --g_active;
}

struct Listener {
    int fd = -1;
    std::string address;
};

bool openListener(const std::string& host, int port, Listener* out, std::string* error) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        *error = std::strerror(errno);
        return false;
    }
    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        *error = "invalid IPv4 address " + host;
        return false;
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(fd, 16) != 0) {
        *error = std::strerror(errno);
        ::close(fd);
        return false;
    }
    out->fd = fd;
    out->address = host + ":" + std::to_string(port);
    return true;
}

#endif

}  // namespace

void requestStop() {
    g_running = false;
}

int run(const DirectConfig& config, const Options& options) {
#ifdef _WIN32
    (void)config;
    (void)options;
    return 3;
#else
    if (!config.v2.enabled) return 3;
    if (config.allowedClientCidrs.empty() && !config.allowAnyClient) {
        std::cerr << "maintenance v2: allowedClientCidrs is empty; not starting" << std::endl;
        return 3;
    }
    auto& crypto = Crypto::instance();
    if (!crypto.tlsAvailable()) {
        std::cerr << "maintenance v2: OpenSSL not usable (" << crypto.error() << "); not starting" << std::endl;
        return 3;
    }
    auto tls = std::make_shared<TlsServerContext>();
    std::string error;
    if (!tls->load(certFile(options), keyFile(options), &error)) {
        std::cerr << "maintenance v2: " << error << " (run SystemMonitor --init-maintenance-tls); not starting" << std::endl;
        return 3;
    }
    const auto fingerprint = certificateFingerprint(readFileOr(certFile(options)));
    try {
        makeDirectories(options.stateDirectory + "/transactions", 0700);
    } catch (const std::exception& ex) {
        std::cerr << "maintenance v2: " << ex.what() << std::endl;
        return 3;
    }
    std::vector<std::string> hosts = config.v2.listenHosts.empty() ? config.listenHosts : config.v2.listenHosts;
    if (hosts.empty()) hosts.push_back(config.listenHost);
    std::vector<Listener> listeners;
    for (const auto& host : hosts) {
        Listener l;
        if (openListener(host, config.v2.listenPort, &l, &error)) {
            listeners.push_back(l);
        } else {
            // One unavailable address must not take the whole channel down.
            std::cerr << "maintenance v2: skipped " << host << ":" << config.v2.listenPort << ": " << error << std::endl;
        }
    }
    if (listeners.empty()) {
        std::cerr << "maintenance v2: no listen address could be bound; not starting" << std::endl;
        return 3;
    }
    auto service = std::make_shared<Service>(config, options, fingerprint);
    g_running = true;
    for (const auto& l : listeners) std::cerr << "maintenance v2 listening on https://" << l.address << " fingerprint " << fingerprint << std::endl;
    service->housekeeping();
    auto lastHousekeeping = std::chrono::steady_clock::now();
    while (g_running) {
        std::vector<pollfd> fds;
        for (const auto& l : listeners) {
            pollfd p {};
            p.fd = l.fd;
            p.events = POLLIN;
            fds.push_back(p);
        }
        const int rc = ::poll(fds.data(), fds.size(), 500);
        if (std::chrono::steady_clock::now() - lastHousekeeping > std::chrono::seconds(30)) {
            lastHousekeeping = std::chrono::steady_clock::now();
            try {
                service->housekeeping();
            } catch (const std::exception& ex) {
                std::cerr << "maintenance v2 housekeeping: " << ex.what() << std::endl;
            }
        }
        if (rc <= 0) continue;
        for (const auto& p : fds) {
            if (!(p.revents & POLLIN)) continue;
            sockaddr_in client {};
            socklen_t len = sizeof(client);
            const int fd = ::accept4(p.fd, reinterpret_cast<sockaddr*>(&client), &len, SOCK_CLOEXEC);
            if (fd < 0) continue;
            char ip[INET_ADDRSTRLEN] = {0};
            ::inet_ntop(AF_INET, &client.sin_addr, ip, sizeof(ip));
            if (!bridge::clientAllowed(config, ntohl(client.sin_addr.s_addr)) || g_active.load() >= config.v2.maxConnections) {
                ::close(fd);
                continue;
            }
            ++g_active;
            try {
                std::thread(serveConnection, fd, std::string(ip), tls, service).detach();
            } catch (...) {
                --g_active;
                ::close(fd);
            }
        }
    }
    for (const auto& l : listeners) ::close(l.fd);
    for (int i = 0; i < 100 && g_active.load() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return 0;
#endif
}

int initTls(const DirectConfig& config, const Options& options, std::ostream& out, std::ostream& err) {
#ifdef _WIN32
    (void)config;
    (void)options;
    (void)err;
    out << "unsupported=1" << std::endl;
    return 0;
#else
    auto& crypto = Crypto::instance();
    if (!crypto.available()) {
        err << "libcrypto not usable: " << crypto.error() << std::endl;
        out << "unsupported=1" << std::endl;
        return 0;
    }
    const auto cert = certFile(options);
    const auto key = keyFile(options);
    if (!isRegularFile(cert) || !isRegularFile(key)) {
        std::string openssl;
        if (!findExecutable("openssl", &openssl)) {
            err << "openssl command not found; cannot create the maintenance certificate" << std::endl;
            out << "unsupported=1" << std::endl;
            return 0;
        }
        std::string machine = bridge::machineCode(config);
        std::string cn;
        for (const char c : machine) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.') cn += c;
        }
        if (cn.empty()) cn = "gateway";
        try {
            makeDirectories(options.materialDirectory, 0700);
        } catch (const std::exception& ex) {
            err << ex.what() << std::endl;
            return 1;
        }
        const auto tmpKey = key + ".new";
        const auto tmpCert = cert + ".new";
        removeFile(tmpKey);
        removeFile(tmpCert);
        const auto oldMask = ::umask(077);
        const auto r = runCommand({openssl, "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes", "-keyout", tmpKey,
                                   "-out", tmpCert, "-days", "3650", "-sha256", "-subj", "/CN=" + cn + "/O=Gateway-zk device"},
                                  60000, 8192);
        ::umask(oldMask);
        if (r.exitCode != 0 || !isRegularFile(tmpKey) || !isRegularFile(tmpCert)) {
            removeFile(tmpKey);
            removeFile(tmpCert);
            // Never echo openssl output wholesale (it never contains the key, but stay strict).
            err << "openssl req failed (exit " << r.exitCode << ")" << std::endl;
            return 1;
        }
        ::chmod(tmpKey.c_str(), 0600);
        ::chmod(tmpCert.c_str(), 0644);
        if (::rename(tmpKey.c_str(), key.c_str()) != 0 || ::rename(tmpCert.c_str(), cert.c_str()) != 0) {
            err << "cannot install the certificate: " << std::strerror(errno) << std::endl;
            return 1;
        }
    }
    TlsServerContext check;
    std::string error;
    if (crypto.tlsAvailable() && !check.load(cert, key, &error)) {
        err << "existing certificate is unusable: " << error << std::endl;
        return 1;
    }
    const auto fingerprint = certificateFingerprint(readFileOr(cert));
    if (fingerprint.empty()) {
        err << "cannot read " << cert << std::endl;
        return 1;
    }
    out << "fingerprint=" << fingerprint << std::endl;
    out << "certFile=" << cert << std::endl;
    return 0;
#endif
}

int runTransactionHelper(const DirectConfig& config, const Options& options, const std::string& transactionId, const std::string& mode) {
    TxRunner runner(config, options);
    if (mode == "apply") return runner.apply(transactionId);
    if (mode == "restart") return runner.restartAfterRestore(transactionId);
    if (mode == "recover") {
        TxStore store(config, options);
        FileLock lock(store.directory(transactionId) + "/lock", true);
        return runner.recover(transactionId);
    }
    std::cerr << "unknown transaction helper mode " << mode << std::endl;
    return 2;
}

}  // namespace maintenance_v2
}  // namespace edge_gateway
