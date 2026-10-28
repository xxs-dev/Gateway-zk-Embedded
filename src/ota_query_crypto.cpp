#include "edge_gateway/ota_query_crypto.hpp"

#ifndef _WIN32
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <climits>
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <vector>

namespace edge_gateway::ota_query_crypto {
namespace {

struct Crypto {
    using PasswordCallback = int (*)(char*, int, int, void*);
    void* library = nullptr;
    void* (*bioNewMemory)(const void*, int) = nullptr;
    int (*bioFree)(void*) = nullptr;
    void* (*readPrivateKey)(void*, void**, PasswordCallback, void*) = nullptr;
    void* (*readPublicKey)(void*, void**, void*, void*) = nullptr;
    void (*keyFree)(void*) = nullptr;
    int (*keyBits)(const void*) = nullptr;
    int (*keyBaseId)(const void*) = nullptr;
    int (*setRsaPadding)(void*, int) = nullptr;
    int (*rsaContextCtrl)(void*, int, int, int, void*) = nullptr;
    void* (*digestContextNew)() = nullptr;
    void (*digestContextFree)(void*) = nullptr;
    const void* (*sha256)() = nullptr;
    int (*signInit)(void*, void**, const void*, void*, void*) = nullptr;
    int (*signUpdate)(void*, const void*, std::size_t) = nullptr;
    int (*signFinal)(void*, unsigned char*, std::size_t*) = nullptr;
    int (*verifyInit)(void*, void**, const void*, void*, void*) = nullptr;
    int (*verifyUpdate)(void*, const void*, std::size_t) = nullptr;
    int (*verifyFinal)(void*, const unsigned char*, std::size_t) = nullptr;
    int (*encodeBlock)(unsigned char*, const unsigned char*, int) = nullptr;
    int (*decodeBlock)(unsigned char*, const unsigned char*, int) = nullptr;

    template <typename T> T symbol(const char* name) {
        auto* resolved = dlsym(library, name);
        if (!resolved) throw Error(Failure::Library);
        return reinterpret_cast<T>(resolved);
    }

    Crypto() {
        library = dlopen("libcrypto.so.3", RTLD_NOW | RTLD_LOCAL);
        const bool legacy = !library;
        if (legacy) library = dlopen("libcrypto.so.1.1", RTLD_NOW | RTLD_LOCAL);
        if (!library) throw Error(Failure::Library);
        try {
            bioNewMemory = symbol<decltype(bioNewMemory)>("BIO_new_mem_buf");
            bioFree = symbol<decltype(bioFree)>("BIO_free");
            readPrivateKey = symbol<decltype(readPrivateKey)>("PEM_read_bio_PrivateKey");
            readPublicKey = symbol<decltype(readPublicKey)>("PEM_read_bio_PUBKEY");
            keyFree = symbol<decltype(keyFree)>("EVP_PKEY_free");
            keyBits = symbol<decltype(keyBits)>(legacy ? "EVP_PKEY_bits" : "EVP_PKEY_get_bits");
            keyBaseId = symbol<decltype(keyBaseId)>(legacy ? "EVP_PKEY_base_id" : "EVP_PKEY_get_base_id");
            if (legacy) rsaContextCtrl = symbol<decltype(rsaContextCtrl)>("RSA_pkey_ctx_ctrl");
            else setRsaPadding = symbol<decltype(setRsaPadding)>("EVP_PKEY_CTX_set_rsa_padding");
            digestContextNew = symbol<decltype(digestContextNew)>("EVP_MD_CTX_new");
            digestContextFree = symbol<decltype(digestContextFree)>("EVP_MD_CTX_free");
            sha256 = symbol<decltype(sha256)>("EVP_sha256");
            signInit = symbol<decltype(signInit)>("EVP_DigestSignInit");
            signUpdate = symbol<decltype(signUpdate)>(legacy ? "EVP_DigestUpdate" : "EVP_DigestSignUpdate");
            signFinal = symbol<decltype(signFinal)>("EVP_DigestSignFinal");
            verifyInit = symbol<decltype(verifyInit)>("EVP_DigestVerifyInit");
            verifyUpdate = symbol<decltype(verifyUpdate)>(legacy ? "EVP_DigestUpdate" : "EVP_DigestVerifyUpdate");
            verifyFinal = symbol<decltype(verifyFinal)>("EVP_DigestVerifyFinal");
            encodeBlock = symbol<decltype(encodeBlock)>("EVP_EncodeBlock");
            decodeBlock = symbol<decltype(decodeBlock)>("EVP_DecodeBlock");
        } catch (...) {
            dlclose(library);
            throw;
        }
    }

    ~Crypto() { dlclose(library); }

    int setPkcs1Padding(void* context) const {
        // OpenSSL 1.1 exposes this operation as a macro over RSA_pkey_ctx_ctrl.
        constexpr int rsaPaddingControl = 0x1000 + 1; // EVP_PKEY_ALG_CTRL + 1
        return setRsaPadding ? setRsaPadding(context, 1)
                             : rsaContextCtrl(context, -1, rsaPaddingControl, 1, nullptr);
    }
};

int rejectPassword(char*, int, int, void*) { return 0; }

Crypto& crypto() {
    static Crypto value;
    return value;
}

std::string readPinnedKey(const std::string& path, bool privateKey) {
    if (path.empty() || path.front() != '/')
        throw std::runtime_error("untrusted OTA query key path");
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) throw std::runtime_error("cannot open OTA query key");
    struct stat before{};
    if (fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) || before.st_nlink != 1 ||
        before.st_size <= 0 || before.st_size > 65536 ||
        (before.st_uid != 0 && before.st_uid != geteuid()) ||
        (before.st_mode & (privateKey ? 0077 : 0022)) != 0) {
        close(fd);
        throw std::runtime_error("untrusted OTA query key file");
    }
    std::string bytes;
    bytes.resize(static_cast<std::size_t>(before.st_size));
    std::size_t received = 0;
    while (received < bytes.size()) {
        const auto count = read(fd, &bytes[received], bytes.size() - received);
        if (count <= 0) { close(fd); throw std::runtime_error("short OTA query key read"); }
        received += static_cast<std::size_t>(count);
    }
    char extra;
    struct stat after{};
    const bool stable = read(fd, &extra, 1) == 0 && fstat(fd, &after) == 0 &&
        before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
        before.st_size == after.st_size && before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
        before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
        before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
        before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
    close(fd);
    if (!stable) throw std::runtime_error("changing OTA query key file");
    const auto prefix = privateKey ? "-----BEGIN PRIVATE KEY-----" : "-----BEGIN PUBLIC KEY-----";
    if (bytes.compare(0, std::char_traits<char>::length(prefix), prefix) != 0)
        throw std::runtime_error("unsupported or encrypted OTA query key");
    return bytes;
}

using CryptoObject = std::unique_ptr<void, void (*)(void*)>;

CryptoObject loadKey(const std::string& path, bool privateKey) {
    std::string bytes;
    try { bytes = readPinnedKey(path, privateKey); }
    catch (const std::exception&) { throw Error(Failure::Key); }
    struct Wipe {
        std::string& value;
        ~Wipe() { std::fill(value.begin(), value.end(), '\0'); }
    } wipe{bytes};
    auto& api = crypto();
    void* bio = api.bioNewMemory(bytes.data(), static_cast<int>(bytes.size()));
    if (!bio) throw Error(Failure::Operation);
    void* key = privateKey ? api.readPrivateKey(bio, nullptr, rejectPassword, nullptr)
                           : api.readPublicKey(bio, nullptr, nullptr, nullptr);
    api.bioFree(bio);
    if (!key) throw Error(Failure::Key);
    CryptoObject owned(key, api.keyFree);
    if (api.keyBaseId(key) != 6 || api.keyBits(key) < 2048)
        throw Error(Failure::Key);
    return owned;
}

std::vector<unsigned char> strictBase64(const std::string& value) {
    if (value.empty() || value.size() > 8192 || value.size() % 4 != 0 || value.size() > INT_MAX)
        throw std::invalid_argument("invalid OTA query signature encoding");
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (c == '=') {
            if (i < value.size() - 2) throw std::invalid_argument("invalid OTA query signature padding");
        } else if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                     (c >= '0' && c <= '9') || c == '+' || c == '/')) {
            throw std::invalid_argument("invalid OTA query signature encoding");
        }
    }
    std::vector<unsigned char> bytes(value.size() / 4 * 3);
    const int decoded = crypto().decodeBlock(bytes.data(),
        reinterpret_cast<const unsigned char*>(value.data()), static_cast<int>(value.size()));
    if (decoded < 0) throw std::invalid_argument("invalid OTA query signature encoding");
    const auto padding = (value.back() == '=') + (value[value.size() - 2] == '=');
    if (decoded < padding) throw std::invalid_argument("invalid OTA query signature padding");
    bytes.resize(static_cast<std::size_t>(decoded - padding));
    std::vector<unsigned char> canonical((bytes.size() + 2) / 3 * 4 + 1);
    const int encoded = crypto().encodeBlock(canonical.data(), bytes.data(), static_cast<int>(bytes.size()));
    if (encoded != static_cast<int>(value.size()) ||
        std::string(reinterpret_cast<char*>(canonical.data()), encoded) != value)
        throw std::invalid_argument("noncanonical OTA query signature");
    return bytes;
}

bool verifyDescriptor(const std::string& descriptor, const std::string& encodedSignature,
                      const std::string& publicKeyPath) {
    std::vector<unsigned char> signature;
    try { signature = strictBase64(encodedSignature); }
    catch (const std::invalid_argument&) { return false; }
    auto key = loadKey(publicKeyPath, false);
    auto& api = crypto();
    CryptoObject context(api.digestContextNew(), api.digestContextFree);
    void* keyContext = nullptr;
    if (!context || api.verifyInit(context.get(), &keyContext, api.sha256(), nullptr, key.get()) != 1 ||
        !keyContext || api.setPkcs1Padding(keyContext) != 1 ||
        api.verifyUpdate(context.get(), descriptor.data(), descriptor.size()) != 1)
        throw Error(Failure::Operation);
    const int result = api.verifyFinal(context.get(), signature.data(), signature.size());
    if (result < 0) throw Error(Failure::Operation);
    return result == 1;
}

std::string signDescriptor(const std::string& descriptor, const std::string& privateKeyPath) {
    auto key = loadKey(privateKeyPath, true);
    auto& api = crypto();
    CryptoObject context(api.digestContextNew(), api.digestContextFree);
    void* keyContext = nullptr;
    if (!context || api.signInit(context.get(), &keyContext, api.sha256(), nullptr, key.get()) != 1 ||
        !keyContext || api.setPkcs1Padding(keyContext) != 1 ||
        api.signUpdate(context.get(), descriptor.data(), descriptor.size()) != 1)
        throw std::runtime_error("OTA query signing failed");
    std::size_t size = 0;
    if (api.signFinal(context.get(), nullptr, &size) != 1 || size == 0 || size > 8192)
        throw std::runtime_error("OTA query signature size invalid");
    std::vector<unsigned char> bytes(size);
    if (api.signFinal(context.get(), bytes.data(), &size) != 1)
        throw std::runtime_error("OTA query signing failed");
    bytes.resize(size);
    std::vector<unsigned char> encoded((size + 2) / 3 * 4 + 1);
    const int length = api.encodeBlock(encoded.data(), bytes.data(), static_cast<int>(size));
    if (length <= 0) throw std::runtime_error("OTA query signature encoding failed");
    return {reinterpret_cast<char*>(encoded.data()), static_cast<std::size_t>(length)};
}

}  // namespace

bool verifyChallenge(const OtaQueryChallenge& challenge, const std::string& publicKeyPath) {
    return verifyDescriptor(ota_query_protocol::challengeDescriptor(challenge), challenge.signature, publicKeyPath);
}

std::string signObservation(const OtaQueryObservation& observation, const std::string& privateKeyPath) {
    return signDescriptor(ota_query_protocol::observationDescriptor(observation), privateKeyPath);
}

bool verifyChallenge(const OtaFullQueryChallenge& challenge, const std::string& publicKeyPath) {
    return verifyDescriptor(ota_full_query_protocol::challengeDescriptor(challenge), challenge.signature, publicKeyPath);
}

std::string signObservation(const OtaFullQueryObservation& observation, const std::string& privateKeyPath) {
    return signDescriptor(ota_full_query_protocol::observationDescriptor(observation), privateKeyPath);
}

}  // namespace edge_gateway::ota_query_crypto
#else
namespace edge_gateway::ota_query_crypto {
bool verifyChallenge(const OtaQueryChallenge&, const std::string&) { return false; }
bool verifyChallenge(const OtaFullQueryChallenge&, const std::string&) { return false; }
std::string signObservation(const OtaQueryObservation&, const std::string&) {
    throw std::runtime_error("OTA query signing is not available on Windows edge");
}
std::string signObservation(const OtaFullQueryObservation&, const std::string&) {
    throw std::runtime_error("OTA query signing is not available on Windows edge");
}
}  // namespace edge_gateway::ota_query_crypto
#endif
