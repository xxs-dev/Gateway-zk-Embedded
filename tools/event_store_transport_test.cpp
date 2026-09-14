#define main legacy_publisher_test_main
#include "builtin_mqtt_driver_publisher_test.cpp"
#undef main
#include "edge_gateway/event_store_runtime.hpp"
#include "edge_gateway/event_store_producer.hpp"
#include "edge_gateway/event_store_sender.hpp"
#include "edge_gateway/event_store_clock.hpp"

int main() {
    using namespace edge_gateway;
    try {
        char directory[] = "/tmp/event-transport-test-XXXXXX";
        require(mkdtemp(directory) != nullptr, "temp directory failed");
        EventStoreRuntimeOptions options;
        options.identity = {"transport", "1"}; options.producers = {"management"};
        options.senders = {{"sender", "main", {"ota_status"}}};
        options.databasePath = std::string(directory) + "/events.db";
        options.socketPath = std::string(directory) + "/events.sock";
        EventStoreRuntime runtime(options); runtime.start();
        EventStoreProducerOptions producerOptions;
        producerOptions.client.identity = options.identity;
        producerOptions.client.socketPath = options.socketPath;
        producerOptions.client.actorId = "management";
        producerOptions.stateless = true;
        AsyncEventStoreProducer producer(producerOptions);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (producer.status().phase != EventCommitPhase::Normal) {
            require(std::chrono::steady_clock::now() < until, "producer did not become ready");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        require(producer.trySubmit({{"ota_status", "test/exact/topic", "one", 1700000000000,
            "transport:one", "main"}}, {}), "enqueue failed");
        require(producer.drain(5000), "commit failed");
        TestMqttBroker broker(1);
        MqttConfig mqtt;
        mqtt.broker = "tcp://127.0.0.1:" + std::to_string(broker.port());
        mqtt.clientId = "transport-test"; mqtt.topicMachineCode = "should-not-be-appended";
        mqtt.qos = 0; mqtt.offlineBufferEnabled = false;
        BuiltinMqttDriverPublisher publisher(mqtt, MqttPublisherMode::TxOnly, MqttEventOutboxOwnership::External);
        EventStoreSenderOptions senderOptions;
        senderOptions.client = producerOptions.client;
        senderOptions.client.actorId = "sender"; senderOptions.client.role = EventStoreClientRole::Sender;
        senderOptions.client.expectedSenderTargetId = "main";
        senderOptions.client.expectedSenderEventTypes = {"ota_status"};
        senderOptions.authorized = [] { return true; };
        senderOptions.send = [&](const EventStoreSenderMessage& message, const EventStoreSenderCall& call) {
            publisher.publishLeasedEvent(message.topic, message.payload, call.bootId, call.deadlineMs);
            return true;
        };
        EventStoreSender sender(senderOptions);
        sender.runOnce();
        const auto messages = broker.messages();
        require(messages.size() == 1 && messages[0].qos == 1 &&
            messages[0].topic == "test/exact/topic" && messages[0].payload == "one",
            "leased transport changed topic/payload/QoS1");
        {
            MqttEventOutbox read(options.databasePath, "", 1, 1, 16, 0,
                MqttEventOutbox::StorageProfile::DeleteFull, MqttEventOutbox::AccessMode::ReadOnly);
            require(read.pendingCount() == 0, "PUBACK was not durably acknowledged");
        }
        auto now = readEventStoreLeaseTime();
        bool refused = false;
        try { publisher.publishLeasedEvent("test/expired", "two", now.bootId, now.milliseconds); }
        catch (const std::exception&) { refused = true; }
        require(refused && broker.publishCount() == 1, "expired lease reached MQTT network");
        runtime.stop();
        std::cout << "EventStore -> actual MQTT QoS1 PUBACK -> durable ACK, exact topic, expired lease: PASS\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
