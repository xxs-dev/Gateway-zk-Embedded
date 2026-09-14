#include "edge_gateway/event_store_management.hpp"
#include <array>
#include <chrono>
#include <dlfcn.h>
#include <thread>

namespace edge_gateway {
namespace {
class ManagementSha256 {
public:
    ManagementSha256() {
        // Same system libcrypto ABI families used by the built-in MQTT transport.
        for (const auto* name : {"libcrypto.so.3", "libcrypto.so.1.1", "libcrypto.so"}) {
            library_ = dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (!library_) continue;
            sha256_ = reinterpret_cast<Function>(dlsym(library_, "SHA256"));
            if (sha256_) return;
            dlclose(library_);
            library_ = nullptr;
        }
        throw std::runtime_error("management stable identity requires libcrypto SHA256");
    }
    ~ManagementSha256() { if (library_) dlclose(library_); }
    ManagementSha256(const ManagementSha256&) = delete;
    ManagementSha256& operator=(const ManagementSha256&) = delete;
    std::string digest(const std::string& bytes) const {
        std::array<unsigned char, 32> digest{};
        if (!sha256_(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), digest.data()))
            throw std::runtime_error("management SHA256 identity generation failed");
        static const char hex[] = "0123456789abcdef";
        std::string result;
        result.reserve(64);
        for (const auto byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; }
        return result;
    }
private:
    using Function = unsigned char* (*)(const unsigned char*, std::size_t, unsigned char*);
    void* library_ = nullptr;
    Function sha256_ = nullptr;
};

std::string stableManagementId(const std::string& storeId, const std::string& type,
    const std::string& topic, const std::string& payload, std::int64_t timestamp) {
    if (topic.empty() || topic.size() > 4096 || payload.size() > 256 * 1024 || timestamp <= 0 ||
        topic.find('\0') != std::string::npos || payload.find('\0') != std::string::npos)
        throw std::invalid_argument("invalid management business event");
    std::string identity;
    const auto add = [&](const std::string& field) {
        identity += std::to_string(field.size());
        identity += ':';
        identity += field;
    };
    // Version and byte lengths make the persistent identity unambiguous across
    // encodings/field boundaries. Never include transient producer identities.
    add("edge-gateway:management:v1");
    add(storeId); add("main"); add(type); add(topic); add(payload); add(std::to_string(timestamp));
    static const ManagementSha256 hash;
    return "management:v1:" + hash.digest(identity);
}
} // namespace

EventStoreManagementWriter::EventStoreManagementWriter(EventStoreProducerOptions options)
    : timeoutMs_(options.client.timeoutMs), storeId_(options.client.identity.storeId) {
    options.stateless = true;
    producer_ = std::make_shared<AsyncEventStoreProducer>(std::move(options));
}
void EventStoreManagementWriter::submit(const std::string& type, const std::string& topic,
    const std::string& payload, std::int64_t timestamp) {
    if (type != "ota_status") throw std::invalid_argument("management actor only accepts configured ota_status events");
    const auto eventId = stableManagementId(storeId_, type, topic, payload, timestamp);
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_) {
        if (!producer_->drain(timeoutMs_)) throw std::runtime_error("management event commit remains unresolved");
        pending_ = false;
        if (pendingEvent_.eventType == type && pendingEvent_.topic == topic &&
            pendingEvent_.payload == payload && pendingEvent_.eventTs == timestamp) return;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs_);
    while (producer_->status().phase != EventCommitPhase::Normal) {
        if (producer_->status().phase == EventCommitPhase::Fenced || std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("management event store not ready");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    pendingEvent_ = {type, topic, payload, timestamp, eventId, "main"};
    if (!producer_->trySubmit({pendingEvent_}, {})) throw std::runtime_error("management event was not accepted");
    pending_ = true;
    if (!producer_->drain(timeoutMs_)) throw std::runtime_error("management event commit not yet confirmed");
    pending_ = false;
}
bool EventStoreManagementWriter::drain(int timeoutMs) { return producer_->drain(timeoutMs); }
} // namespace edge_gateway
