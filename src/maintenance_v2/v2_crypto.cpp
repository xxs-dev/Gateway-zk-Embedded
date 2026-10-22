#include "v2_crypto.hpp"

#include "v2_json.hpp"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <vector>

#ifndef _WIN32
#include <dlfcn.h>
#endif

namespace edge_gateway {
namespace maintenance_v2 {

namespace {

const int kNidEd25519 = 1087;           // NID_ED25519 / EVP_PKEY_ED25519
const int kSslCtrlSetMinProto = 123;    // SSL_CTRL_SET_MIN_PROTO_VERSION
const long kTls12Version = 0x0303;      // TLS1_2_VERSION
const int kSslFiletypePem = 1;          // SSL_FILETYPE_PEM
const int kSslErrorWantRead = 2;
const int kSslErrorWantWrite = 3;

void* openLibrary(const char* name) {
#ifndef _WIN32
    return dlopen(name, RTLD_NOW | RTLD_LOCAL);
#else
    (void)name;
    return nullptr;
#endif
}

void* symbol(void* lib, const char* name) {
#ifndef _WIN32
    return lib == nullptr ? nullptr : dlsym(lib, name);
#else
    (void)lib;
    (void)name;
    return nullptr;
#endif
}

template <typename T>
void bind(void* lib, const char* name, T* out) {
    *out = reinterpret_cast<T>(symbol(lib, name));
}

}  // namespace

struct Crypto::Fns {
    unsigned char* (*sha256)(const unsigned char*, std::size_t, unsigned char*) = nullptr;
    int (*randBytes)(unsigned char*, int) = nullptr;
    void* (*pkeyNewRawPublic)(int, void*, const unsigned char*, std::size_t) = nullptr;
    void (*pkeyFree)(void*) = nullptr;
    void* (*mdCtxNew)() = nullptr;
    void (*mdCtxFree)(void*) = nullptr;
    int (*digestVerifyInit)(void*, void**, const void*, void*, void*) = nullptr;
    int (*digestVerify)(void*, const unsigned char*, std::size_t, const unsigned char*, std::size_t) = nullptr;
    void (*errClear)() = nullptr;
    unsigned long (*errGet)() = nullptr;
    void (*errString)(unsigned long, char*, std::size_t) = nullptr;

    int (*initSsl)(std::uint64_t, const void*) = nullptr;
    const void* (*serverMethod)() = nullptr;
    void* (*ctxNew)(const void*) = nullptr;
    void (*ctxFree)(void*) = nullptr;
    long (*ctxCtrl)(void*, int, long, void*) = nullptr;
    int (*ctxUseChain)(void*, const char*) = nullptr;
    int (*ctxUseKey)(void*, const char*, int) = nullptr;
    int (*ctxCheckKey)(const void*) = nullptr;
    void* (*sslNew)(void*) = nullptr;
    void (*sslFree)(void*) = nullptr;
    int (*sslSetFd)(void*, int) = nullptr;
    int (*sslAccept)(void*) = nullptr;
    int (*sslRead)(void*, void*, int) = nullptr;
    int (*sslWrite)(void*, const void*, int) = nullptr;
    int (*sslShutdown)(void*) = nullptr;
    int (*sslGetError)(const void*, int) = nullptr;
};

Crypto& Crypto::instance() {
    static Crypto crypto;
    return crypto;
}

Crypto::Crypto() : fns_(new Fns()) {
    load();
}

void Crypto::load() {
    const char* pairs[][2] = {
        {"libcrypto.so.3", "libssl.so.3"},
        {"libcrypto.so.1.1", "libssl.so.1.1"},
        {"libcrypto.so", "libssl.so"},
    };
    for (const auto& pair : pairs) {
        cryptoLib_ = openLibrary(pair[0]);
        if (cryptoLib_ == nullptr) continue;
        sslLib_ = openLibrary(pair[1]);
        break;
    }
    if (cryptoLib_ == nullptr) {
        error_ = "libcrypto is not available";
        return;
    }
    Fns& f = *fns_;
    bind(cryptoLib_, "SHA256", &f.sha256);
    bind(cryptoLib_, "RAND_bytes", &f.randBytes);
    bind(cryptoLib_, "EVP_PKEY_new_raw_public_key", &f.pkeyNewRawPublic);
    bind(cryptoLib_, "EVP_PKEY_free", &f.pkeyFree);
    bind(cryptoLib_, "EVP_MD_CTX_new", &f.mdCtxNew);
    bind(cryptoLib_, "EVP_MD_CTX_free", &f.mdCtxFree);
    bind(cryptoLib_, "EVP_DigestVerifyInit", &f.digestVerifyInit);
    bind(cryptoLib_, "EVP_DigestVerify", &f.digestVerify);
    bind(cryptoLib_, "ERR_clear_error", &f.errClear);
    bind(cryptoLib_, "ERR_get_error", &f.errGet);
    bind(cryptoLib_, "ERR_error_string_n", &f.errString);
    cryptoOk_ = f.sha256 && f.randBytes && f.pkeyNewRawPublic && f.pkeyFree && f.mdCtxNew && f.mdCtxFree &&
                f.digestVerifyInit && f.digestVerify;
    if (!cryptoOk_) {
        error_ = "libcrypto lacks SHA256/Ed25519 (OpenSSL 1.1.1 or newer required)";
        return;
    }
    if (sslLib_ == nullptr) {
        error_ = "libssl is not available";
        return;
    }
    bind(sslLib_, "OPENSSL_init_ssl", &f.initSsl);
    bind(sslLib_, "TLS_server_method", &f.serverMethod);
    bind(sslLib_, "SSL_CTX_new", &f.ctxNew);
    bind(sslLib_, "SSL_CTX_free", &f.ctxFree);
    bind(sslLib_, "SSL_CTX_ctrl", &f.ctxCtrl);
    bind(sslLib_, "SSL_CTX_use_certificate_chain_file", &f.ctxUseChain);
    bind(sslLib_, "SSL_CTX_use_PrivateKey_file", &f.ctxUseKey);
    bind(sslLib_, "SSL_CTX_check_private_key", &f.ctxCheckKey);
    bind(sslLib_, "SSL_new", &f.sslNew);
    bind(sslLib_, "SSL_free", &f.sslFree);
    bind(sslLib_, "SSL_set_fd", &f.sslSetFd);
    bind(sslLib_, "SSL_accept", &f.sslAccept);
    bind(sslLib_, "SSL_read", &f.sslRead);
    bind(sslLib_, "SSL_write", &f.sslWrite);
    bind(sslLib_, "SSL_shutdown", &f.sslShutdown);
    bind(sslLib_, "SSL_get_error", &f.sslGetError);
    sslOk_ = f.serverMethod && f.ctxNew && f.ctxFree && f.ctxCtrl && f.ctxUseChain && f.ctxUseKey && f.ctxCheckKey && f.sslNew &&
             f.sslFree && f.sslSetFd && f.sslAccept && f.sslRead && f.sslWrite && f.sslShutdown && f.sslGetError;
    if (!sslOk_) {
        error_ = "libssl lacks the TLS server functions";
        return;
    }
    if (f.initSsl) f.initSsl(0, nullptr);
}

std::string Crypto::sha256(const std::string& data) const {
    if (!cryptoOk_) return std::string();
    unsigned char digest[32];
    fns_->sha256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest);
    return std::string(reinterpret_cast<const char*>(digest), sizeof(digest));
}

std::string Crypto::sha256Hex(const std::string& data) const {
    const auto raw = sha256(data);
    return raw.empty() ? std::string() : toHex(raw);
}

bool Crypto::randomBytes(std::size_t count, std::string* out) const {
    if (!cryptoOk_ || count > 1024) return false;
    std::vector<unsigned char> buffer(count);
    if (fns_->randBytes(buffer.data(), static_cast<int>(count)) != 1) return false;
    out->assign(reinterpret_cast<const char*>(buffer.data()), buffer.size());
    return true;
}

bool Crypto::verifyEd25519(const std::string& publicKeyRaw, const std::string& message, const std::string& signature) const {
    if (!cryptoOk_ || publicKeyRaw.size() != 32 || signature.size() != 64) return false;
    void* key = fns_->pkeyNewRawPublic(kNidEd25519, nullptr, reinterpret_cast<const unsigned char*>(publicKeyRaw.data()), publicKeyRaw.size());
    if (key == nullptr) {
        if (fns_->errClear) fns_->errClear();
        return false;
    }
    void* ctx = fns_->mdCtxNew();
    bool ok = false;
    if (ctx != nullptr && fns_->digestVerifyInit(ctx, nullptr, nullptr, nullptr, key) == 1) {
        ok = fns_->digestVerify(ctx, reinterpret_cast<const unsigned char*>(signature.data()), signature.size(),
                                reinterpret_cast<const unsigned char*>(message.data()), message.size()) == 1;
    }
    if (ctx != nullptr) fns_->mdCtxFree(ctx);
    fns_->pkeyFree(key);
    if (fns_->errClear) fns_->errClear();
    return ok;
}

TlsServerContext::~TlsServerContext() {
    if (ctx_ != nullptr) Crypto::instance().fns_->ctxFree(ctx_);
}

bool TlsServerContext::load(const std::string& certFile, const std::string& keyFile, std::string* error) {
    auto& c = Crypto::instance();
    if (!c.tlsAvailable()) {
        *error = c.error().empty() ? "OpenSSL TLS is not available" : c.error();
        return false;
    }
    auto* f = c.fns_;
    void* ctx = f->ctxNew(f->serverMethod());
    if (ctx == nullptr) {
        *error = "SSL_CTX_new failed";
        return false;
    }
    f->ctxCtrl(ctx, kSslCtrlSetMinProto, kTls12Version, nullptr);
    auto fail = [&](const std::string& message) {
        unsigned long code = f->errGet ? f->errGet() : 0;
        std::string detail;
        if (code != 0 && f->errString) {
            char buf[256];
            f->errString(code, buf, sizeof(buf));
            detail = std::string(": ") + buf;
        }
        if (f->errClear) f->errClear();
        f->ctxFree(ctx);
        *error = message + detail;
        return false;
    };
    if (f->ctxUseChain(ctx, certFile.c_str()) != 1) return fail("cannot load certificate " + certFile);
    if (f->ctxUseKey(ctx, keyFile.c_str(), kSslFiletypePem) != 1) return fail("cannot load private key " + keyFile);
    if (f->ctxCheckKey(ctx) != 1) return fail("certificate and private key do not match");
    if (ctx_ != nullptr) f->ctxFree(ctx_);
    ctx_ = ctx;
    return true;
}

TlsConnection::TlsConnection(const TlsServerContext& context, int fd) {
    auto* f = Crypto::instance().fns_;
    if (context.handle() == nullptr) return;
    ssl_ = f->sslNew(context.handle());
    if (ssl_ != nullptr && f->sslSetFd(ssl_, fd) != 1) {
        f->sslFree(ssl_);
        ssl_ = nullptr;
    }
}

TlsConnection::~TlsConnection() {
    if (ssl_ != nullptr) Crypto::instance().fns_->sslFree(ssl_);
}

bool TlsConnection::accept(std::string* error) {
    auto* f = Crypto::instance().fns_;
    if (ssl_ == nullptr) {
        *error = "tls session unavailable";
        return false;
    }
    const int rc = f->sslAccept(ssl_);
    if (rc != 1) {
        *error = "tls handshake failed (" + std::to_string(f->sslGetError(ssl_, rc)) + ")";
        if (f->errClear) f->errClear();
        return false;
    }
    accepted_ = true;
    return true;
}

int TlsConnection::read(char* buffer, int size) {
    auto* f = Crypto::instance().fns_;
    if (!accepted_) return -1;
    const int rc = f->sslRead(ssl_, buffer, size);
    if (rc > 0) return rc;
    const int err = f->sslGetError(ssl_, rc);
    if (f->errClear) f->errClear();
    if (err == 6 /* SSL_ERROR_ZERO_RETURN */) return 0;
    if (err == kSslErrorWantRead || err == kSslErrorWantWrite) return -1;  // timeout on a blocking socket
    return rc == 0 ? 0 : -1;
}

bool TlsConnection::writeAll(const std::string& data) {
    auto* f = Crypto::instance().fns_;
    if (!accepted_) return false;
    std::size_t sent = 0;
    while (sent < data.size()) {
        const int chunk = static_cast<int>(std::min<std::size_t>(data.size() - sent, 1 << 20));
        const int rc = f->sslWrite(ssl_, data.data() + sent, chunk);
        if (rc <= 0) {
            if (f->errClear) f->errClear();
            return false;
        }
        sent += static_cast<std::size_t>(rc);
    }
    return true;
}

void TlsConnection::shutdown() {
    if (accepted_ && ssl_ != nullptr) {
        Crypto::instance().fns_->sslShutdown(ssl_);
        auto* f = Crypto::instance().fns_;
        if (f->errClear) f->errClear();
    }
}

std::string certificateFingerprint(const std::string& certPem) {
    const std::string begin = "-----BEGIN CERTIFICATE-----";
    const std::string end = "-----END CERTIFICATE-----";
    const auto b = certPem.find(begin);
    const auto e = certPem.find(end, b == std::string::npos ? 0 : b);
    if (b == std::string::npos || e == std::string::npos) return std::string();
    std::string body;
    for (std::size_t i = b + begin.size(); i < e; ++i) {
        const char c = certPem[i];
        if (c != '\r' && c != '\n' && c != ' ' && c != '\t') body.push_back(c);
    }
    std::string der;
    if (!base64Decode(body, &der) || der.empty()) return std::string();
    const auto digest = Crypto::instance().sha256(der);
    if (digest.empty()) return std::string();
    static const char* digits = "0123456789ABCDEF";
    std::string out;
    for (std::size_t i = 0; i < digest.size(); ++i) {
        if (i > 0) out += ':';
        const auto c = static_cast<unsigned char>(digest[i]);
        out += digits[c >> 4];
        out += digits[c & 15];
    }
    return out;
}

}  // namespace maintenance_v2
}  // namespace edge_gateway
