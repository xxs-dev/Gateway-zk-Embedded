#pragma once

#include <set>
#include <string>

#include "edge_gateway/interfaces.hpp"

namespace edge_gateway {

class SysfsGpioPort : public IGpioPort {
public:
    explicit SysfsGpioPort(std::string basePath = "/sys/class/gpio");

    void exportGpio(int gpio) override;
    void setDirection(int gpio, const std::string& direction) override;
    bool readValue(int gpio) override;
    void writeValue(int gpio, bool high) override;

private:
    std::string gpioPath(int gpio, const std::string& file) const;
    void writeFile(const std::string& path, const std::string& value) const;
    std::string readFile(const std::string& path) const;

    std::string basePath_;
    std::set<int> exported_;
};

}  // namespace edge_gateway
