#include "edge_gateway/mosquitto_mqtt_driver_publisher.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace edge_gateway {

namespace {

struct mosquitto;

constexpr int kMosqOptProtocolVersion = 1;
constexpr int kMqttProtocolV311 = 4;
constexpr int kMqttProtocolV5 = 5;

using mosquitto_lib_init_fn = int (*)();
using mosquitto_lib_cleanup_fn = int (*)();
using mosquitto_new_fn = mosquitto* (*)(const char*, bool, void*);
using mosquitto_destroy_fn = void (*)(mosquitto*);
using mosquitto_username_pw_set_fn = int (*)(mosquitto*, const char*, const char*);
using mosquitto_int_option_fn = int (*)(mosquitto*, int, int);
using mosquitto_connect_bind_fn = int (*)(mosquitto*, const char*, int, int, const char*);
using mosquitto_connect_bind_v5_fn = int (*)(mosquitto*, const char*, int, int, const char*, const void*);
using mosquitto_publish_fn = int (*)(mosquitto*, int*, const char*, int, const void*, int, bool);
using mosquitto_publish_v5_fn = int (*)(mosquitto*, int*, const char*, int, const void*, int, bool, const void*);
using mosquitto_loop_fn = int (*)(mosquitto*, int, int);
using mosquitto_disconnect_fn = int (*)(mosquitto*);

std::string scopedTopic(const std::string& topic, const std::string& machineCode) {
    if (topic.empty() || machineCode.empty()) {
        return topic;
    }
    const std::string suffix = "/" + machineCode;
    if (topic.size() >= suffix.size() &&
        topic.compare(topic.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return topic;
    }
    return topic + suffix;
}

std::string escapeJson(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const auto ch : value) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

struct BrokerEndpoint {
    std::string host = "127.0.0.1";
    int port = 1883;
};

BrokerEndpoint parseBroker(const std::string& broker) {
    BrokerEndpoint endpoint;
    auto address = broker;
    const auto schemePos = address.find("://");
    if (schemePos != std::string::npos) {
        address = address.substr(schemePos + 3);
    }
    const auto colonPos = address.rfind(':');
    if (colonPos != std::string::npos) {
        endpoint.host = address.substr(0, colonPos);
        endpoint.port = std::stoi(address.substr(colonPos + 1));
    } else if (!address.empty()) {
        endpoint.host = address;
    }
    return endpoint;
}

enum class PointValueJsonFormat {
    CompactArray,
    Object
};

PointValueJsonFormat pointValueJsonFormat(const std::string& value) {
    std::string normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return normalized == "object" ? PointValueJsonFormat::Object : PointValueJsonFormat::CompactArray;
}

void appendPointValueJson(std::ostringstream& out, const StoredPointValue& item, PointValueJsonFormat format) {
    if (format == PointValueJsonFormat::CompactArray) {
        out << "[" << item.index
            << ",\"" << escapeJson(item.pointCode) << "\""
            << "," << item.value
            << "," << item.quality
            << "," << item.ts
            << "," << item.expireAt
            << "," << (item.stale ? "true" : "false")
            << "]";
        return;
    }
    out << "{\"index\":" << item.index
        << ",\"pointCode\":\"" << escapeJson(item.pointCode) << "\""
        << ",\"value\":" << item.value
        << ",\"quality\":" << item.quality
        << ",\"ts\":" << item.ts
        << ",\"expireAt\":" << item.expireAt
        << ",\"stale\":" << (item.stale ? "true" : "false")
        << "}";
}

std::string firstMachineCode(const std::vector<StoredPointValue>& values, const std::string& fallback = std::string()) {
    for (const auto& item : values) {
        if (!item.machineCode.empty()) {
            return item.machineCode;
        }
    }
    return fallback;
}

std::vector<std::string> meterCodesFromValues(const std::vector<StoredPointValue>& values) {
    std::vector<std::string> devices;
    for (const auto& item : values) {
        if (item.meterCode.empty()) {
            continue;
        }
        if (std::find(devices.begin(), devices.end(), item.meterCode) == devices.end()) {
            devices.push_back(item.meterCode);
        }
    }
    return devices;
}

std::string encodeValuesJson(
    const std::vector<StoredPointValue>& values,
    PointValueJsonFormat format,
    const std::string& fallbackMachineCode = std::string()
) {
    std::ostringstream out;
    out << "{\"type\":\"telemetry\",\"machineCode\":\"" << escapeJson(firstMachineCode(values, fallbackMachineCode)) << "\",\"meters\":[";
    const auto devices = meterCodesFromValues(values);
    for (std::size_t d = 0; d < devices.size(); ++d) {
        if (d > 0) {
            out << ",";
        }
        out << "{\"meterCode\":\"" << escapeJson(devices[d]) << "\",\"values\":[";
        bool firstValue = true;
        for (const auto& item : values) {
            if (item.meterCode != devices[d]) {
                continue;
            }
            if (!firstValue) {
                out << ",";
            }
            appendPointValueJson(out, item, format);
            firstValue = false;
        }
        out << "]}";
    }
    out << "]}";
    return out.str();
}

std::string encodeFullJson(
    const std::vector<StoredPointValue>& values,
    PointValueJsonFormat format,
    const std::string& fallbackMachineCode = std::string()
) {
    std::ostringstream out;
    out << "{\"type\":\"snapshot\",\"machineCode\":\"" << escapeJson(firstMachineCode(values, fallbackMachineCode)) << "\",\"meters\":[";
    const auto devices = meterCodesFromValues(values);
    for (std::size_t d = 0; d < devices.size(); ++d) {
        if (d > 0) {
            out << ",";
        }
        out << "{\"meterCode\":\"" << escapeJson(devices[d]) << "\",\"values\":[";
        bool firstValue = true;
        for (const auto& item : values) {
            if (item.meterCode != devices[d]) {
                continue;
            }
            if (!firstValue) {
                out << ",";
            }
            appendPointValueJson(out, item, format);
            firstValue = false;
        }
        out << "]}";
    }
    out << "]}";
    return out.str();
}

std::string encodeAlarmJson(std::uint32_t index, const StoredPointValue& value, const std::string& alarmType, bool active) {
    std::ostringstream out;
    out << "{\"type\":\"alarm\""
        << ",\"machineCode\":\"" << escapeJson(value.machineCode) << "\""
        << ",\"meterCode\":\"" << escapeJson(value.meterCode) << "\""
        << ",\"index\":" << index
        << ",\"pointCode\":\"" << escapeJson(value.pointCode) << "\""
        << ",\"alarmType\":\"" << escapeJson(alarmType) << "\""
        << ",\"active\":" << (active ? "true" : "false")
        << ",\"value\":" << value.value
        << ",\"quality\":" << value.quality
        << ",\"ts\":" << value.ts
        << ",\"stale\":" << (value.stale ? "true" : "false")
        << "}";
    return out.str();
}

std::string encodeChangeEventJson(const StoredPointValue& value) {
    std::ostringstream out;
    out << "{\"type\":\"change\""
        << ",\"machineCode\":\"" << escapeJson(value.machineCode) << "\""
        << ",\"meterCode\":\"" << escapeJson(value.meterCode) << "\""
        << ",\"index\":" << value.index
        << ",\"pointCode\":\"" << escapeJson(value.pointCode) << "\""
        << ",\"value\":" << value.value
        << ",\"quality\":" << value.quality
        << ",\"ts\":" << value.ts
        << ",\"expireAt\":" << value.expireAt
        << ",\"stale\":" << (value.stale ? "true" : "false")
        << "}";
    return out.str();
}

std::string encodeCommandReplyJson(const MqttCommandReply& reply) {
    std::ostringstream out;
    out << "{\"cmdId\":\"" << escapeJson(reply.cmdId) << "\""
        << ",\"machineCode\":\"" << escapeJson(reply.machineCode) << "\""
        << ",\"meterCode\":\"" << escapeJson(reply.meterCode) << "\""
        << ",\"pointCode\":\"" << escapeJson(reply.pointCode) << "\""
        << ",\"index\":" << reply.index
        << ",\"success\":" << (reply.success ? "true" : "false")
        << ",\"message\":\"" << escapeJson(reply.message) << "\""
        << ",\"ts\":" << reply.ts
        << "}";
    return out.str();
}

std::string encodeOtaReplyJson(const OtaReply& reply) {
    std::ostringstream out;
    out << "{\"jobId\":\"" << escapeJson(reply.jobId) << "\""
        << ",\"machineCode\":\"" << escapeJson(reply.machineCode) << "\""
        << ",\"accepted\":" << (reply.accepted ? "true" : "false")
        << ",\"message\":\"" << escapeJson(reply.message) << "\""
        << ",\"ts\":" << reply.ts
        << "}";
    return out.str();
}

std::string encodeOtaStatusJson(const OtaStatus& status) {
    std::ostringstream out;
    out << "{\"jobId\":\"" << escapeJson(status.jobId) << "\""
        << ",\"machineCode\":\"" << escapeJson(status.machineCode) << "\""
        << ",\"stage\":\"" << escapeJson(status.stage) << "\""
        << ",\"progress\":" << status.progress
        << ",\"downloadedBytes\":" << status.downloadedBytes
        << ",\"totalBytes\":" << status.totalBytes
        << ",\"message\":\"" << escapeJson(status.message) << "\""
        << ",\"ts\":" << status.ts
        << "}";
    return out.str();
}

void* loadLibraryHandle(const std::string& preferredPath) {
#ifdef _WIN32
    if (!preferredPath.empty()) {
        if (auto* handle = LoadLibraryA(preferredPath.c_str())) {
            return handle;
        }
    }
    if (auto* handle = LoadLibraryA("mosquitto.dll")) {
        return handle;
    }
    return nullptr;
#else
    if (!preferredPath.empty()) {
        if (auto* handle = dlopen(preferredPath.c_str(), RTLD_NOW | RTLD_LOCAL)) {
            return handle;
        }
    }
    if (auto* handle = dlopen("libmosquitto.so.1", RTLD_NOW | RTLD_LOCAL)) {
        return handle;
    }
    if (auto* handle = dlopen("libmosquitto.so", RTLD_NOW | RTLD_LOCAL)) {
        return handle;
    }
    return nullptr;
#endif
}

void unloadLibraryHandle(void* handle) {
    if (handle == nullptr) {
        return;
    }
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
}

void* loadSymbol(void* handle, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle), name));
#else
    return dlsym(handle, name);
#endif
}

}  // namespace

struct MosquittoMqttDriverPublisher::Impl {
    void* libraryHandle = nullptr;
    mosquitto_lib_init_fn libInit = nullptr;
    mosquitto_lib_cleanup_fn libCleanup = nullptr;
    mosquitto_new_fn create = nullptr;
    mosquitto_destroy_fn destroy = nullptr;
    mosquitto_username_pw_set_fn setUserPass = nullptr;
    mosquitto_int_option_fn intOption = nullptr;
    mosquitto_connect_bind_fn connectV3 = nullptr;
    mosquitto_connect_bind_v5_fn connectV5 = nullptr;
    mosquitto_publish_fn publishV3 = nullptr;
    mosquitto_publish_v5_fn publishV5 = nullptr;
    mosquitto_loop_fn loop = nullptr;
    mosquitto_disconnect_fn disconnect = nullptr;
};

MosquittoMqttDriverPublisher::MosquittoMqttDriverPublisher(MqttConfig config)
    : config_(std::move(config)),
      impl_(new Impl()) {
    impl_->libraryHandle = loadLibraryHandle("");
    if (impl_->libraryHandle == nullptr) {
        throw std::runtime_error("failed to load libmosquitto");
    }

    impl_->libInit = reinterpret_cast<mosquitto_lib_init_fn>(loadSymbol(impl_->libraryHandle, "mosquitto_lib_init"));
    impl_->libCleanup = reinterpret_cast<mosquitto_lib_cleanup_fn>(loadSymbol(impl_->libraryHandle, "mosquitto_lib_cleanup"));
    impl_->create = reinterpret_cast<mosquitto_new_fn>(loadSymbol(impl_->libraryHandle, "mosquitto_new"));
    impl_->destroy = reinterpret_cast<mosquitto_destroy_fn>(loadSymbol(impl_->libraryHandle, "mosquitto_destroy"));
    impl_->setUserPass = reinterpret_cast<mosquitto_username_pw_set_fn>(loadSymbol(impl_->libraryHandle, "mosquitto_username_pw_set"));
    impl_->intOption = reinterpret_cast<mosquitto_int_option_fn>(loadSymbol(impl_->libraryHandle, "mosquitto_int_option"));
    impl_->connectV3 = reinterpret_cast<mosquitto_connect_bind_fn>(loadSymbol(impl_->libraryHandle, "mosquitto_connect_bind"));
    impl_->connectV5 = reinterpret_cast<mosquitto_connect_bind_v5_fn>(loadSymbol(impl_->libraryHandle, "mosquitto_connect_bind_v5"));
    impl_->publishV3 = reinterpret_cast<mosquitto_publish_fn>(loadSymbol(impl_->libraryHandle, "mosquitto_publish"));
    impl_->publishV5 = reinterpret_cast<mosquitto_publish_v5_fn>(loadSymbol(impl_->libraryHandle, "mosquitto_publish_v5"));
    impl_->loop = reinterpret_cast<mosquitto_loop_fn>(loadSymbol(impl_->libraryHandle, "mosquitto_loop"));
    impl_->disconnect = reinterpret_cast<mosquitto_disconnect_fn>(loadSymbol(impl_->libraryHandle, "mosquitto_disconnect"));

    if (!impl_->libInit || !impl_->libCleanup || !impl_->create || !impl_->destroy ||
        !impl_->setUserPass || !impl_->intOption || !impl_->connectV3 || !impl_->connectV5 ||
        !impl_->publishV3 || !impl_->publishV5 || !impl_->loop || !impl_->disconnect) {
        unloadLibraryHandle(impl_->libraryHandle);
        throw std::runtime_error("libmosquitto symbols missing");
    }

    if (impl_->libInit() != 0) {
        unloadLibraryHandle(impl_->libraryHandle);
        throw std::runtime_error("mosquitto_lib_init failed");
    }
}

MosquittoMqttDriverPublisher::~MosquittoMqttDriverPublisher() {
    if (impl_) {
        if (impl_->libCleanup) {
            impl_->libCleanup();
        }
        unloadLibraryHandle(impl_->libraryHandle);
    }
}

void MosquittoMqttDriverPublisher::publishFullSnapshot(
    const std::string& topic,
    const std::vector<StoredPointValue>& values,
    const std::string& valueFormat
) {
    publishJson(topic, encodeFullJson(values, pointValueJsonFormat(valueFormat), config_.topicMachineCode));
}

void MosquittoMqttDriverPublisher::publishAlarm(
    const std::string& topic,
    std::uint32_t index,
    const StoredPointValue& value,
    const std::string& alarmType,
    bool active
) {
    publishJson(topic, encodeAlarmJson(index, value, alarmType, active));
}

void MosquittoMqttDriverPublisher::publishOnDemand(
    const std::string& topic,
    const std::vector<StoredPointValue>& values,
    const std::string& valueFormat
) {
    publishJson(topic, encodeValuesJson(values, pointValueJsonFormat(valueFormat), config_.topicMachineCode));
}

void MosquittoMqttDriverPublisher::publishChangeEvent(
    const std::string& topic,
    const StoredPointValue& value
) {
    publishJson(topic, encodeChangeEventJson(value));
}

void MosquittoMqttDriverPublisher::publishCommandReply(
    const std::string& topic,
    const MqttCommandReply& reply
) {
    publishJson(topic, encodeCommandReplyJson(reply));
}

void MosquittoMqttDriverPublisher::publishOtaReply(
    const std::string& topic,
    const OtaReply& reply
) {
    publishJson(topic, encodeOtaReplyJson(reply));
}

void MosquittoMqttDriverPublisher::publishOtaStatus(
    const std::string& topic,
    const OtaStatus& status
) {
    publishJson(topic, encodeOtaStatusJson(status));
}

void MosquittoMqttDriverPublisher::publishJsonMessage(
    const std::string& topic,
    const std::string& payload
) {
    publishJson(topic, payload);
}

std::vector<MqttIncomingMessage> MosquittoMqttDriverPublisher::pollIncoming(int) {
    return {};
}

void MosquittoMqttDriverPublisher::publishJson(const std::string& topic, const std::string& payload) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* client = impl_->create(config_.clientId.c_str(), config_.cleanSession, nullptr);
    if (client == nullptr) {
        throw std::runtime_error("mosquitto_new failed");
    }

    const auto cleanup = [this, client]() {
        impl_->disconnect(client);
        impl_->destroy(client);
    };

    if (!config_.username.empty()) {
        const auto rc = impl_->setUserPass(
            client,
            config_.username.c_str(),
            config_.password.empty() ? nullptr : config_.password.c_str()
        );
        if (rc != 0) {
            cleanup();
            throw std::runtime_error("mosquitto_username_pw_set failed");
        }
    }

    const auto protocol = config_.protocolVersion == "mqtt5" ? kMqttProtocolV5 : kMqttProtocolV311;
    if (impl_->intOption(client, kMosqOptProtocolVersion, protocol) != 0) {
        cleanup();
        throw std::runtime_error("mosquitto protocol option failed");
    }

    const auto endpoint = parseBroker(config_.broker);
    int rc = 0;
    if (protocol == kMqttProtocolV5) {
        rc = impl_->connectV5(client, endpoint.host.c_str(), endpoint.port, config_.keepAliveSec, nullptr, nullptr);
    } else {
        rc = impl_->connectV3(client, endpoint.host.c_str(), endpoint.port, config_.keepAliveSec, nullptr);
    }
    if (rc != 0) {
        cleanup();
        throw std::runtime_error("mosquitto connect failed");
    }

    const std::string scoped = scopedTopic(topic, config_.topicMachineCode);
    if (protocol == kMqttProtocolV5) {
        rc = impl_->publishV5(
            client,
            nullptr,
            scoped.c_str(),
            static_cast<int>(payload.size()),
            payload.data(),
            qosForTopic(scoped),
            false,
            nullptr
        );
    } else {
        rc = impl_->publishV3(
            client,
            nullptr,
            scoped.c_str(),
            static_cast<int>(payload.size()),
            payload.data(),
            qosForTopic(scoped),
            false
        );
    }
    if (rc != 0) {
        cleanup();
        throw std::runtime_error("mosquitto publish failed");
    }

    impl_->loop(client, 1000, 1);
    cleanup();
}

int MosquittoMqttDriverPublisher::qosForTopic(const std::string& scopedTopicValue) const {
    const auto matches = [&](const std::string& topic) {
        return !topic.empty() && scopedTopic(topic, config_.topicMachineCode) == scopedTopicValue;
    };
    if (matches(config_.commandReplyTopic) ||
        matches(config_.otaReplyTopic) ||
        matches(config_.otaStatusTopic) ||
        matches(config_.diagReplyTopic) ||
        matches(config_.configApplyReplyTopic) ||
        matches(config_.configDeleteReplyTopic) ||
        matches(config_.configRestoreReplyTopic)) {
        return std::max(0, std::min(2, config_.controlQos));
    }
    return std::max(0, std::min(2, config_.qos));
}

}  // namespace edge_gateway
