#pragma once

#include <stdexcept>
#include <system_error>

namespace edge_gateway {

enum class ModbusFailureKind {
    Transport,
    Timeout,
    ProtocolException,
    MalformedFrame,
    Configuration
};

class ModbusError : public std::runtime_error {
public:
    ModbusError(ModbusFailureKind kind, const std::string& message, bool requestSent = false)
        : std::runtime_error(message), kind_(kind), requestSent_(requestSent) {
    }

    ModbusFailureKind kind() const noexcept {
        return kind_;
    }

    // True once any byte of the request reached the transport. A write that fails after this point
    // (timeout, broken reply, connection loss) may have been executed by the device.
    bool requestSent() const noexcept {
        return requestSent_;
    }

private:
    ModbusFailureKind kind_;
    bool requestSent_;
};

// A Modbus write whose request reached the device side but whose result is unknown.
inline bool isWriteOutcomeUnknown(const std::exception& error) {
    const auto* modbusError = dynamic_cast<const ModbusError*>(&error);
    return modbusError != nullptr && modbusError->requestSent() &&
        modbusError->kind() != ModbusFailureKind::ProtocolException &&
        modbusError->kind() != ModbusFailureKind::Configuration;
}

inline ModbusFailureKind classifyModbusFailure(const std::exception& error) {
    if (const auto* modbusError = dynamic_cast<const ModbusError*>(&error)) {
        return modbusError->kind();
    }
    if (dynamic_cast<const std::system_error*>(&error) != nullptr) {
        return ModbusFailureKind::Transport;
    }
    if (dynamic_cast<const std::invalid_argument*>(&error) != nullptr) {
        return ModbusFailureKind::Configuration;
    }

    const std::string message = error.what();
    if (message.find("connect failed") != std::string::npos ||
        message.find("recv failed") != std::string::npos ||
        message.find("send failed") != std::string::npos ||
        message.find("serial port") != std::string::npos ||
        message.find("serial device") != std::string::npos) {
        return ModbusFailureKind::Transport;
    }
    if (message.find("timeout") != std::string::npos ||
        message.find("no response") != std::string::npos) {
        return ModbusFailureKind::Timeout;
    }

    // Existing test and plugin clients may still throw plain runtime_error for
    // device protocol exceptions. Keep those eligible for adaptive isolation.
    return ModbusFailureKind::ProtocolException;
}

inline bool isTransportFailure(ModbusFailureKind kind) {
    return kind == ModbusFailureKind::Transport || kind == ModbusFailureKind::Timeout;
}

inline bool canAdaptiveSplitFailure(ModbusFailureKind kind) {
    return kind == ModbusFailureKind::ProtocolException;
}

}  // namespace edge_gateway
