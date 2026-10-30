#pragma once

#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>

#include "edge_gateway/interfaces.hpp"

namespace edge_gateway {

// Linux sysfs GPIO. Every control-file write is checked (open/write/close); a failed export is not
// remembered, so the next call exports again. value of an output reads back the output latch that
// the SoC drives, not the relay contact.
class SysfsGpioPort : public IGpioPort {
public:
    explicit SysfsGpioPort(std::string basePath = "/sys/class/gpio");
    ~SysfsGpioPort() override;

    void exportGpio(int gpio) override;
    void setDirection(int gpio, const std::string& direction) override;
    bool readValue(int gpio) override;
    void writeValue(int gpio, bool high) override;
    void setOutputDirection(int gpio, bool initialHigh) override;
    bool ensureOutput(int gpio, bool initialHigh) override;

private:
    std::string gpioPath(int gpio, const std::string& file) const;
    void exportGpioLocked(int gpio);
    bool currentlyOutputLocked(int gpio);
    void writeFile(const std::string& path, const std::string& value) const;
    std::string readFile(const std::string& path) const;

    std::string basePath_;
    mutable std::mutex mutex_;
    std::set<int> exported_;
    // After a failed export the gpio is retried no earlier than this (steady clock, ms).
    std::unordered_map<int, std::int64_t> exportRetryAtMs_;
    std::unordered_map<int, std::string> directions_;
#ifndef _WIN32
    std::unordered_map<int, int> valueReadFds_;
    std::unordered_map<int, int> valueWriteFds_;
#endif
};

}  // namespace edge_gateway
