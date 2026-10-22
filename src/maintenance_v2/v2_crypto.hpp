#pragma once

// Maintenance channel v2: OpenSSL loaded at run time (dlopen, like the MQTT TLS
// publisher and the OTA query crypto) - no new link-time dependency.
// Provides SHA-256, random bytes, Ed25519 verification and a TLS server context.

#include <cstddef>
#include <string>

namespace edge_gateway {
namespace maintenance_v2 {

class Crypto {
public:
    static Crypto& instance();

    bool available() const { return cryptoOk_; }
    bool tlsAvailable() const { return sslOk_; }
    const std::string& error() const { return error_; }

    std::string sha256(const std::string& data) const;  // raw 32 bytes ("" when unavailable)
    std::string sha256Hex(const std::string& data) const;
    bool randomBytes(std::size_t count, std::string* out) const;
    // Ed25519 over `message` with a raw 32-byte public key.
    bool verifyEd25519(const std::string& publicKeyRaw, const std::string& message, const std::string& signature) const;

private:
    Crypto();
    void load();

    void* cryptoLib_ = nullptr;
    void* sslLib_ = nullptr;
    bool cryptoOk_ = false;
    bool sslOk_ = false;
    std::string error_;

    friend class TlsServerContext;
    friend class TlsConnection;
    struct Fns;
    Fns* fns_ = nullptr;
};

// Server-side TLS context (certificate + private key PEM files, TLS 1.2+).
class TlsServerContext {
public:
    TlsServerContext() = default;
    ~TlsServerContext();
    TlsServerContext(const TlsServerContext&) = delete;
    TlsServerContext& operator=(const TlsServerContext&) = delete;

    bool load(const std::string& certFile, const std::string& keyFile, std::string* error);
    void* handle() const { return ctx_; }

private:
    void* ctx_ = nullptr;
};

// One accepted connection. The socket must already have SO_RCVTIMEO/SO_SNDTIMEO.
class TlsConnection {
public:
    TlsConnection(const TlsServerContext& context, int fd);
    ~TlsConnection();
    TlsConnection(const TlsConnection&) = delete;
    TlsConnection& operator=(const TlsConnection&) = delete;

    bool accept(std::string* error);
    // >0 bytes, 0 = closed, <0 = error / timeout
    int read(char* buffer, int size);
    bool writeAll(const std::string& data);
    void shutdown();

private:
    void* ssl_ = nullptr;
    bool accepted_ = false;
};

// SHA-256 of the DER inside a PEM certificate, "AA:BB:..." (uppercase, colon separated).
std::string certificateFingerprint(const std::string& certPem);

}  // namespace maintenance_v2
}  // namespace edge_gateway
