#include "edge_gateway/sysfs_gpio_port.hpp"

#include <cerrno>
#include <chrono>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace edge_gateway {

SysfsGpioPort::SysfsGpioPort(std::string basePath) : basePath_(std::move(basePath)) {
    if (basePath_.empty()) {
        basePath_ = "/sys/class/gpio";
    }
}

SysfsGpioPort::~SysfsGpioPort() {
#ifndef _WIN32
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : valueReadFds_) {
        if (entry.second >= 0) ::close(entry.second);
    }
    for (const auto& entry : valueWriteFds_) {
        if (entry.second >= 0) ::close(entry.second);
    }
#endif
}

void SysfsGpioPort::exportGpio(int gpio) {
    if (gpio < 0) {
        throw std::invalid_argument("gpio must be non-negative");
    }
    std::lock_guard<std::mutex> lock(mutex_);
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
    std::lock_guard<std::mutex> lock(mutex_);
    const auto current = directions_.find(gpio);
    if (current != directions_.end() && current->second == direction) {
        return;
    }
    writeFile(gpioPath(gpio, "direction"), direction);
    directions_[gpio] = direction;
}

bool SysfsGpioPort::readValue(int gpio) {
    exportGpio(gpio);
#ifndef _WIN32
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = valueReadFds_.find(gpio);
    if (it == valueReadFds_.end()) {
        const auto fd = ::open(gpioPath(gpio, "value").c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            throw std::runtime_error("failed to open gpio value for read: " + gpioPath(gpio, "value"));
        }
        it = valueReadFds_.emplace(gpio, fd).first;
    }
    char value = '0';
    if (::lseek(it->second, 0, SEEK_SET) < 0 || ::read(it->second, &value, 1) != 1) {
        ::close(it->second);
        valueReadFds_.erase(it);
        exported_.erase(gpio);
        throw std::runtime_error("failed to read gpio value: " + gpioPath(gpio, "value"));
    }
    return value == '1';
#else
    const auto value = readFile(gpioPath(gpio, "value"));
    return !value.empty() && value[0] == '1';
#endif
}

void SysfsGpioPort::writeValue(int gpio, bool high) {
    exportGpio(gpio);
#ifndef _WIN32
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = valueWriteFds_.find(gpio);
    if (it == valueWriteFds_.end()) {
        const auto fd = ::open(gpioPath(gpio, "value").c_str(), O_WRONLY | O_CLOEXEC);
        if (fd < 0) {
            throw std::runtime_error("failed to open gpio value for write: " + gpioPath(gpio, "value"));
        }
        it = valueWriteFds_.emplace(gpio, fd).first;
    }
    const char value = high ? '1' : '0';
    if (::lseek(it->second, 0, SEEK_SET) < 0 || ::write(it->second, &value, 1) != 1) {
        ::close(it->second);
        valueWriteFds_.erase(it);
        exported_.erase(gpio);
        throw std::runtime_error("failed to write gpio value: " + gpioPath(gpio, "value"));
    }
#else
    writeFile(gpioPath(gpio, "value"), high ? "1" : "0");
#endif
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
