#include "edge_gateway/sysfs_gpio_port.hpp"

#include <cerrno>
#include <chrono>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace edge_gateway {

SysfsGpioPort::SysfsGpioPort(std::string basePath) : basePath_(std::move(basePath)) {
    if (basePath_.empty()) {
        basePath_ = "/sys/class/gpio";
    }
}

void SysfsGpioPort::exportGpio(int gpio) {
    if (gpio < 0) {
        throw std::invalid_argument("gpio must be non-negative");
    }
    if (exported_.find(gpio) != exported_.end()) {
        return;
    }
    const auto directionPath = gpioPath(gpio, "direction");
    std::ifstream existing(directionPath.c_str());
    if (!existing.good()) {
        writeFile(basePath_ + "/export", std::to_string(gpio));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    exported_.insert(gpio);
}

void SysfsGpioPort::setDirection(int gpio, const std::string& direction) {
    exportGpio(gpio);
    if (direction != "in" && direction != "out") {
        throw std::invalid_argument("gpio direction must be in or out");
    }
    writeFile(gpioPath(gpio, "direction"), direction);
}

bool SysfsGpioPort::readValue(int gpio) {
    exportGpio(gpio);
    const auto value = readFile(gpioPath(gpio, "value"));
    return !value.empty() && value[0] == '1';
}

void SysfsGpioPort::writeValue(int gpio, bool high) {
    exportGpio(gpio);
    writeFile(gpioPath(gpio, "value"), high ? "1" : "0");
}

std::string SysfsGpioPort::gpioPath(int gpio, const std::string& file) const {
    return basePath_ + "/gpio" + std::to_string(gpio) + "/" + file;
}

void SysfsGpioPort::writeFile(const std::string& path, const std::string& value) const {
    std::ofstream output(path.c_str());
    if (!output.is_open()) {
        throw std::runtime_error("failed to open gpio file for write: " + path);
    }
    output << value;
    if (!output.good()) {
        throw std::runtime_error("failed to write gpio file: " + path);
    }
}

std::string SysfsGpioPort::readFile(const std::string& path) const {
    std::ifstream input(path.c_str());
    if (!input.is_open()) {
        throw std::runtime_error("failed to open gpio file for read: " + path);
    }
    std::stringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

}  // namespace edge_gateway
