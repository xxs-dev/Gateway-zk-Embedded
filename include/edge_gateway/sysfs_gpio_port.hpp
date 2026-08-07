#pragma once

#include <mutex>
#include <set>
#include <string>
#include <unordered_map>

#include "edge_gateway/interfaces.hpp"

namespace edge_gateway {

class SysfsGpioPort : public IGpioPort {
public:
    explicit SysfsGpioPort(std::string basePath = "/sys/class/gpio");
    ~SysfsGpioPort() override;

    void exportGpio(int gpio) override;
    void setDirection(int gpio, const std::string& direction) override;
    bool readValue(int gpio) override;
    void writeValue(int gpio, bool high) override;

private:
    std::string gpioPath(int gpio, const std::string& file) const;
    void writeFile(const std::string& path, const std::string& value) const;
    std::string readFile(const std::string& path) const;

    std::string basePath_;
    mutable std::mutex mutex_;
    std::set<int> exported_;
    std::unordered_map<int, std::string> directions_;
#ifndef _WIN32
    std::unordered_map<int, int> valueReadFds_;
    std::unordered_map<int, int> valueWriteFds_;
#endif
};

}  // namespace edge_gateway
