#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "edge_gateway/collector.hpp"
#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/dio_command_executor.hpp"
#include "edge_gateway/interfaces.hpp"
#include "edge_gateway/memory_point_store.hpp"

namespace {

class FakeGpioPort : public edge_gateway::IGpioPort {
public:
    void exportGpio(int gpio) override {
        exported_[gpio] = true;
    }

    void setDirection(int gpio, const std::string& direction) override {
        directions_[gpio] = direction;
    }

    bool readValue(int gpio) override {
        return values_[gpio];
    }

    void writeValue(int gpio, bool high) override {
        values_[gpio] = high;
    }

    void setValue(int gpio, bool high) {
        values_[gpio] = high;
    }

    bool value(int gpio) const {
        const auto it = values_.find(gpio);
        return it != values_.end() && it->second;
    }

    std::string direction(int gpio) const {
        const auto it = directions_.find(gpio);
        return it == directions_.end() ? std::string() : it->second;
    }

private:
    std::unordered_map<int, bool> exported_;
    std::unordered_map<int, bool> values_;
    std::unordered_map<int, std::string> directions_;
};

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

edge_gateway::DeviceConfig buildConfig() {
    const std::string text = R"JSON({
      "schemaVersion": "1.0.0",
      "machineCode": "GW_TEST",
      "protocol": {
        "type": "local_dio",
        "backend": "sysfs_gpio",
        "gpioBasePath": "/tmp/gpio"
      },
      "collect": {
        "defaultIntervalMs": 100,
        "batchOptimize": false,
        "maxBatchRegisters": 1
      },
      "memoryStore": {
        "sharedMemoryName": "gateway_dio_smoke_test",
        "maxLatestPoints": 128,
        "maxPendingWrites": 16,
        "maxPersistentSamples": 16,
        "sqlitePath": "dio_smoke_test.db",
        "writebackIntervalMs": 100,
        "writebackBatchSize": 8
      },
      "meters": [
        {
          "meterCode": "LOCAL_DIO",
          "deviceName": "Local DIO",
          "points": [
            {
              "index": 410006,
              "pointCode": "di_6",
              "name": "DI6",
              "category": "status",
              "address": 6,
              "enabled": true,
              "isStore": false,
              "fullUpload": true,
              "reportOnChange": true,
              "read": {
                "enable": true,
                "function": 0,
                "length": 1,
                "dataType": "digital_input",
                "gpio": 134,
                "activeHigh": true,
                "debounceMs": 0,
                "cachePolicy": {"storeLatest": true, "ttlMs": 600000}
              },
              "write": {"enable": false},
              "alarms": []
            },
            {
              "index": 420001,
              "pointCode": "do_1",
              "name": "DO1",
              "category": "control",
              "address": 1,
              "enabled": true,
              "isStore": false,
              "fullUpload": true,
              "reportOnChange": true,
              "read": {
                "enable": true,
                "function": 0,
                "length": 1,
                "dataType": "digital_output",
                "gpio": 231,
                "activeHigh": false,
                "debounceMs": 0,
                "cachePolicy": {"storeLatest": true, "ttlMs": 600000}
              },
              "write": {
                "enable": true,
                "function": 0,
                "length": 1,
                "dataType": "digital_output",
                "allowedValues": [0, 1],
                "verifyAfterWrite": true,
                "verifyByRead": true
              },
              "alarms": []
            }
          ]
        }
      ]
    })JSON";
    return edge_gateway::ConfigLoader::loadFromText(text);
}

}  // namespace

int main() {
    try {
        auto config = buildConfig();
        require(config.protocol.type == "local_dio", "protocol.type not parsed");
        require(config.protocol.backend == "sysfs_gpio", "protocol.backend not parsed");
        require(config.protocol.gpioBasePath == "/tmp/gpio", "protocol.gpioBasePath not parsed");
        require(config.meters.size() == 1, "expected one meter");
        require(config.meters[0].points.size() == 2, "expected two points");
        require(config.meters[0].points[0].read.gpio == 134, "DI gpio not parsed");
        require(config.meters[0].points[1].read.gpio == 231, "DO gpio not parsed");
        require(!config.meters[0].points[1].read.activeHigh, "DO activeHigh not parsed");

        edge_gateway::DeviceConfig runtime = config;
        runtime.meterCode = config.meters[0].meterCode;
        runtime.deviceName = config.meters[0].deviceName;
        runtime.points = config.meters[0].points;
        runtime.meters.clear();

        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto gpio = std::make_shared<FakeGpioPort>();
        gpio->setValue(134, true);
        gpio->setValue(231, true);

        edge_gateway::DioCollector collector(runtime, store, gpio);
        const auto collected = collector.collectOnce(1000);
        require(collected.values.size() == 2, "expected two local dio values");
        auto di = store.getLatestByIndex(410006, 1000);
        require(static_cast<bool>(di), "DI value missing from store");
        require((*di).value == 1.0, "DI6 dry contact should expose GPIO high as value 1");
        auto doBefore = store.getLatestByIndex(420001, 1000);
        require(static_cast<bool>(doBefore), "DO value missing from store");
        require((*doBefore).value == 0.0, "DO1 active-low GPIO high should expose value 0");

        edge_gateway::DioCommandExecutor executor(runtime, store, gpio);
        const auto result = executor.executeByIndex("CMD_DIO_1", 420001, 1.0, 1100);
        require(result.success, "DO write command should succeed: " + result.message);
        require(!gpio->value(231), "DO1 value=1 should write GPIO low");
        require(gpio->direction(231) == "out", "DO1 write should set direction out");
        auto doAfter = store.getLatestByIndex(420001, 1100);
        require(static_cast<bool>(doAfter), "DO value missing after write");
        require((*doAfter).value == 1.0, "DO latest value should update after write");

        std::cout << "dio_smoke_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "dio_smoke_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
