#include "edge_gateway/mqtt_realtime_ring_buffer.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace edge_gateway {

namespace {

constexpr std::uint32_t kHeaderSize = 4096;
constexpr std::uint32_t kRecordMagic = 0x4D515452U;  // MQTR
constexpr std::uint32_t kWrapMagic = 0x4D515457U;    // MQTW

struct RecordHeader {
    std::uint32_t magic;
    std::uint32_t totalLen;
    std::uint32_t topicLen;
    std::uint32_t payloadLen;
    std::uint64_t seq;
    std::int64_t createdAt;
};

std::string directoryOf(const std::string& path) {
    const auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? std::string() : path.substr(0, pos);
}

bool directoryExists(const std::string& path) {
    if (path.empty()) {
        return true;
    }
#ifdef _WIN32
    return _access(path.c_str(), 0) == 0;
#else
    struct stat st {};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

void makeDirectory(const std::string& path) {
    if (path.empty() || directoryExists(path)) {
        return;
    }
    makeDirectory(directoryOf(path));
#ifdef _WIN32
    _mkdir(path.c_str());
#else
    mkdir(path.c_str(), 0755);
#endif
}

std::uint64_t fileSize(const std::string& path) {
    std::ifstream input(path.c_str(), std::ios::binary | std::ios::ate);
    if (!input.is_open()) {
        return 0;
    }
    return static_cast<std::uint64_t>(input.tellg());
}

std::int64_t currentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

}  // namespace

MqttRealtimeRingBuffer::MqttRealtimeRingBuffer(
    std::string filePath,
    std::uint64_t fileSizeBytes,
    std::uint32_t maxMessageBytes,
    std::size_t replayBatchSize
) : filePath_(std::move(filePath)),
    fileSizeBytes_(std::max<std::uint64_t>(fileSizeBytes, 16ULL * 1024ULL * 1024ULL)),
    maxMessageBytes_(std::max<std::uint32_t>(maxMessageBytes, 1024U * 1024U)),
    replayBatchSize_(std::max<std::size_t>(1, replayBatchSize)) {
    ensureInitialized();
}

void MqttRealtimeRingBuffer::ensureInitialized() {
    makeDirectory(directoryOf(filePath_));
    const bool needsInit = fileSize(filePath_) != fileSizeBytes_;
    if (needsInit) {
        std::ofstream output(filePath_.c_str(), std::ios::binary | std::ios::trunc);
        if (!output.is_open()) {
            throw std::runtime_error("failed to create mqtt realtime ring file");
        }
        Header header {};
        std::memcpy(header.magic, "MQTRING", 7);
        header.version = 1;
        header.headerSize = kHeaderSize;
        header.fileSize = fileSizeBytes_;
        header.readOffset = kHeaderSize;
        header.writeOffset = kHeaderSize;
        output.write(reinterpret_cast<const char*>(&header), sizeof(header));
        std::vector<char> zeros(kHeaderSize - sizeof(header), 0);
        output.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
        output.seekp(static_cast<std::streamoff>(fileSizeBytes_ - 1));
        char zero = 0;
        output.write(&zero, 1);
        return;
    }
    auto header = readHeader();
    if (std::memcmp(header.magic, "MQTRING", 7) != 0 || header.fileSize != fileSizeBytes_) {
        std::ofstream reset(filePath_.c_str(), std::ios::binary | std::ios::trunc);
        Header clean {};
        std::memcpy(clean.magic, "MQTRING", 7);
        clean.version = 1;
        clean.headerSize = kHeaderSize;
        clean.fileSize = fileSizeBytes_;
        clean.readOffset = kHeaderSize;
        clean.writeOffset = kHeaderSize;
        reset.write(reinterpret_cast<const char*>(&clean), sizeof(clean));
        std::vector<char> zeros(kHeaderSize - sizeof(clean), 0);
        reset.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
        reset.seekp(static_cast<std::streamoff>(fileSizeBytes_ - 1));
        char zero = 0;
        reset.write(&zero, 1);
    }
}

MqttRealtimeRingBuffer::Header MqttRealtimeRingBuffer::readHeader() {
    std::ifstream input(filePath_.c_str(), std::ios::binary);
    Header header {};
    input.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!input.good()) {
        throw std::runtime_error("failed to read mqtt realtime ring header");
    }
    return header;
}

void MqttRealtimeRingBuffer::writeHeader(const Header& header) {
    std::fstream file(filePath_.c_str(), std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(0);
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.flush();
}

std::uint64_t MqttRealtimeRingBuffer::dataBegin() const {
    return kHeaderSize;
}

std::uint64_t MqttRealtimeRingBuffer::dataCapacity() const {
    return fileSizeBytes_ - kHeaderSize;
}

std::uint64_t MqttRealtimeRingBuffer::contiguousFreeAtEnd(const Header& header) const {
    if (header.writeOffset >= header.readOffset) {
        return fileSizeBytes_ - header.writeOffset;
    }
    return header.readOffset - header.writeOffset;
}

void MqttRealtimeRingBuffer::writeWrapMarker(Header* header) {
    std::fstream file(filePath_.c_str(), std::ios::binary | std::ios::in | std::ios::out);
    RecordHeader marker {};
    marker.magic = kWrapMagic;
    marker.totalLen = static_cast<std::uint32_t>(fileSizeBytes_ - header->writeOffset);
    file.seekp(static_cast<std::streamoff>(header->writeOffset));
    file.write(reinterpret_cast<const char*>(&marker), sizeof(marker));
    header->usedBytes += marker.totalLen;
    header->writeOffset = dataBegin();
}

void MqttRealtimeRingBuffer::dropOldest(Header* header) {
    if (header->recordCount == 0 || header->usedBytes == 0) {
        header->readOffset = dataBegin();
        header->writeOffset = dataBegin();
        header->usedBytes = 0;
        return;
    }
    std::fstream file(filePath_.c_str(), std::ios::binary | std::ios::in);
    RecordHeader record {};
    file.seekg(static_cast<std::streamoff>(header->readOffset));
    file.read(reinterpret_cast<char*>(&record), sizeof(record));
    if (!file.good() || record.totalLen == 0 || record.totalLen > dataCapacity()) {
        header->readOffset = dataBegin();
        header->writeOffset = dataBegin();
        header->usedBytes = 0;
        header->recordCount = 0;
        return;
    }
    header->usedBytes -= std::min<std::uint64_t>(header->usedBytes, record.totalLen);
    header->readOffset += record.totalLen;
    if (header->readOffset >= fileSizeBytes_) {
        header->readOffset = dataBegin();
    }
    if (record.magic == kRecordMagic && header->recordCount > 0) {
        --header->recordCount;
    }
}

void MqttRealtimeRingBuffer::writeRecord(Header* header, const std::string& topic, const std::string& payload) {
    const auto totalLen = sizeof(RecordHeader) + topic.size() + payload.size();
    if (totalLen > maxMessageBytes_ || totalLen >= dataCapacity()) {
        return;
    }
    while (dataCapacity() - header->usedBytes < totalLen + sizeof(RecordHeader)) {
        dropOldest(header);
    }
    if (contiguousFreeAtEnd(*header) < totalLen) {
        writeWrapMarker(header);
        while (dataCapacity() - header->usedBytes < totalLen + sizeof(RecordHeader)) {
            dropOldest(header);
        }
    }

    RecordHeader record {};
    record.magic = kRecordMagic;
    record.totalLen = static_cast<std::uint32_t>(totalLen);
    record.topicLen = static_cast<std::uint32_t>(topic.size());
    record.payloadLen = static_cast<std::uint32_t>(payload.size());
    record.seq = header->nextSeq++;
    record.createdAt = currentTimeMs();

    std::fstream file(filePath_.c_str(), std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(static_cast<std::streamoff>(header->writeOffset));
    file.write(reinterpret_cast<const char*>(&record), sizeof(record));
    file.write(topic.data(), static_cast<std::streamsize>(topic.size()));
    file.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    file.flush();

    header->writeOffset += totalLen;
    if (header->writeOffset >= fileSizeBytes_) {
        header->writeOffset = dataBegin();
    }
    header->usedBytes += totalLen;
    ++header->recordCount;
}

bool MqttRealtimeRingBuffer::readRecord(const Header& header, std::string* topic, std::string* payload, std::uint64_t* recordSize) {
    if (header.recordCount == 0 || header.usedBytes == 0) {
        return false;
    }
    std::ifstream file(filePath_.c_str(), std::ios::binary);
    RecordHeader record {};
    file.seekg(static_cast<std::streamoff>(header.readOffset));
    file.read(reinterpret_cast<char*>(&record), sizeof(record));
    if (!file.good() || record.totalLen == 0) {
        return false;
    }
    *recordSize = record.totalLen;
    if (record.magic == kWrapMagic) {
        return false;
    }
    if (record.magic != kRecordMagic || record.topicLen + record.payloadLen + sizeof(RecordHeader) != record.totalLen) {
        return false;
    }
    topic->assign(record.topicLen, '\0');
    payload->assign(record.payloadLen, '\0');
    file.read(&(*topic)[0], record.topicLen);
    file.read(&(*payload)[0], record.payloadLen);
    return file.good();
}

void MqttRealtimeRingBuffer::enqueue(const std::string& topic, const std::string& payload) {
    auto header = readHeader();
    writeRecord(&header, topic, payload);
    writeHeader(header);
}

std::size_t MqttRealtimeRingBuffer::replay(const std::function<void(const std::string&, const std::string&)>& send) {
    std::size_t sent = 0;
    auto header = readHeader();
    while (sent < replayBatchSize_ && header.recordCount > 0) {
        std::string topic;
        std::string payload;
        std::uint64_t size = 0;
        if (!readRecord(header, &topic, &payload, &size)) {
            dropOldest(&header);
            continue;
        }
        send(topic, payload);
        dropOldest(&header);
        ++sent;
    }
    writeHeader(header);
    return sent;
}

}  // namespace edge_gateway
