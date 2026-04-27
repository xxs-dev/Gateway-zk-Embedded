#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace edge_gateway {

class MqttRealtimeRingBuffer {
public:
    MqttRealtimeRingBuffer(
        std::string filePath,
        std::uint64_t fileSizeBytes,
        std::uint32_t maxMessageBytes,
        std::size_t replayBatchSize
    );

    void enqueue(const std::string& topic, const std::string& payload);
    std::size_t replay(const std::function<void(const std::string&, const std::string&)>& send);

private:
    struct Header {
        char magic[8];
        std::uint32_t version;
        std::uint32_t headerSize;
        std::uint64_t fileSize;
        std::uint64_t readOffset;
        std::uint64_t writeOffset;
        std::uint64_t usedBytes;
        std::uint64_t recordCount;
        std::uint64_t nextSeq;
    };

    void ensureInitialized();
    Header readHeader();
    void writeHeader(const Header& header);
    std::uint64_t dataBegin() const;
    std::uint64_t dataCapacity() const;
    std::uint64_t contiguousFreeAtEnd(const Header& header) const;
    void dropOldest(Header* header);
    void writeWrapMarker(Header* header);
    void writeRecord(Header* header, const std::string& topic, const std::string& payload);
    bool readRecord(const Header& header, std::string* topic, std::string* payload, std::uint64_t* recordSize);

    std::string filePath_;
    std::uint64_t fileSizeBytes_;
    std::uint32_t maxMessageBytes_;
    std::size_t replayBatchSize_;
};

}  // namespace edge_gateway
