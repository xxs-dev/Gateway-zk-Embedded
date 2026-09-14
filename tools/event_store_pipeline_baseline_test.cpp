// Deterministic negative control for the pre-pipeline synchronous transport.
#define main existing_publisher_suite_main
#include "builtin_mqtt_driver_publisher_test.cpp"
#undef main
#include "edge_gateway/event_store_clock.hpp"

int main() {
    TestMqttBroker broker(8, 0, true, true, 8);
    edge_gateway::MqttConfig config;
    config.broker = "tcp://127.0.0.1:" + std::to_string(broker.port());
    config.clientId = "pipeline-negative-control";
    config.offlineBufferEnabled = false;
    edge_gateway::BuiltinMqttDriverPublisher publisher(config,
        edge_gateway::MqttPublisherMode::TxOnly, edge_gateway::MqttEventOutboxOwnership::External);
    const auto now = edge_gateway::readEventStoreLeaseTime();
    bool failed = false;
    try {
        for (int i = 0; i < 8; ++i)
            publisher.publishLeasedEvent("exact/topic", std::to_string(i), now.bootId, now.milliseconds + 300);
    } catch (...) { failed = true; }
    require(failed && broker.publishCount() == 1, "negative control no longer reproduces serial PUBACK stall");
    std::cout << "REPRODUCED: synchronous leased transport stalls at 1/8 PUBLISH before PUBACK" << std::endl;
}
