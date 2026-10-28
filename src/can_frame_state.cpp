#include "can_frame_state.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include "edge_gateway/json_value.hpp"

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace edge_gateway {
namespace can_frame_state {

namespace {

constexpr int kVersion = 1;

std::string hexBytes(const std::vector<std::uint8_t>& bytes) {
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setfill('0');
    for (const auto byte : bytes) {
        out << std::setw(2) << static_cast<int>(byte);
    }
    return out.str();
}

std::vector<std::uint8_t> parseHexBytes(const std::string& text) {
    if (text.size() % 2 != 0 || text.size() > 128 ||
        text.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
        throw std::runtime_error("CAN frame state payload is not hex");
    }
    std::vector<std::uint8_t> bytes;
    for (std::size_t i = 0; i < text.size(); i += 2) {
        bytes.push_back(static_cast<std::uint8_t>(std::stoul(text.substr(i, 2), nullptr, 16)));
    }
    return bytes;
}

std::string escape(const std::string& value) {
    std::string out;
    for (const auto ch : value) {
        if (ch == '"' || ch == '\\') out.push_back('\\');
        out.push_back(ch);
    }
    return out;
}

const json::JsonValue& member(const json::JsonValue& object, const char* key) {
    const auto* value = object.find(key);
    if (value == nullptr) {
        throw std::runtime_error(std::string("CAN frame state lacks \"") + key + "\"");
    }
    return *value;
}

std::string directoryOf(const std::string& path) {
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string(".") : (slash == 0 ? std::string("/") : path.substr(0, slash));
}

}  // namespace

std::string layoutHash(const std::string& signature) {
    std::uint64_t hash = 1469598103934665603ULL;  // FNV-1a 64
    for (const auto ch : signature) {
        hash ^= static_cast<unsigned char>(ch);
        hash *= 1099511628211ULL;
    }
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

std::string serialize(const std::string& interfaceName, const std::vector<Entry>& entries) {
    std::ostringstream out;
    out << "{\"version\":" << kVersion << ",\"interfaceName\":\"" << escape(interfaceName) << "\",\"frames\":[";
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        std::ostringstream id;
        id << "0x" << std::uppercase << std::hex << entry.frameId;
        out << (i == 0 ? "" : ",") << "\n {\"frameId\":\"" << id.str() << "\",\"extended\":"
            << (entry.extended ? "true" : "false") << ",\"fd\":" << (entry.fd ? "true" : "false")
            << ",\"remoteRequest\":" << (entry.remoteRequest ? "true" : "false")
            << ",\"dlc\":" << entry.payload.size() << ",\"layout\":\"" << escape(entry.layout)
            << "\",\"payload\":\"" << hexBytes(entry.payload) << "\"}";
    }
    out << "\n]}\n";
    return out.str();
}

std::vector<Entry> parse(const std::string& text, const std::string& interfaceName) {
    const auto root = json::JsonParser(text, 8, 4096).parse();
    if (!root.isObject()) {
        throw std::runtime_error("CAN frame state is not a JSON object");
    }
    if (static_cast<int>(member(root, "version").asNumber()) != kVersion) {
        throw std::runtime_error("CAN frame state has an unknown version");
    }
    if (member(root, "interfaceName").asString() != interfaceName) {
        throw std::runtime_error("CAN frame state belongs to interface " + member(root, "interfaceName").asString());
    }
    std::vector<Entry> entries;
    for (const auto& item : member(root, "frames").asArray().values) {
        Entry entry;
        const auto& id = member(*item, "frameId").asString();
        if (id.size() < 3 || id.compare(0, 2, "0x") != 0) {
            throw std::runtime_error("CAN frame state frameId is not hex");
        }
        entry.frameId = static_cast<std::uint32_t>(std::stoul(id.substr(2), nullptr, 16));
        entry.extended = member(*item, "extended").asBool();
        entry.fd = member(*item, "fd").asBool();
        entry.remoteRequest = member(*item, "remoteRequest").asBool();
        entry.layout = member(*item, "layout").asString();
        entry.payload = parseHexBytes(member(*item, "payload").asString());
        if (static_cast<std::size_t>(member(*item, "dlc").asNumber()) != entry.payload.size()) {
            throw std::runtime_error("CAN frame state dlc does not match its payload");
        }
        entries.push_back(entry);
    }
    return entries;
}

void writeAtomically(const std::string& path, const std::string& content) {
#ifndef _WIN32
    const auto temporary = path + ".tmp";
    const auto fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        throw std::runtime_error("cannot create " + temporary + ": " + std::strerror(errno));
    }
    const auto written = ::write(fd, content.data(), content.size());
    const auto writeError = errno;
    if (written != static_cast<ssize_t>(content.size()) || ::fsync(fd) != 0) {
        const auto error = written < 0 ? writeError : errno;
        ::close(fd);
        std::remove(temporary.c_str());
        throw std::runtime_error("cannot write " + temporary + ": " + std::strerror(error));
    }
    if (::close(fd) != 0 || std::rename(temporary.c_str(), path.c_str()) != 0) {
        const auto error = errno;
        std::remove(temporary.c_str());
        throw std::runtime_error("cannot replace " + path + ": " + std::strerror(error));
    }
    const auto directory = ::open(directoryOf(path).c_str(), O_RDONLY | O_CLOEXEC);
    if (directory >= 0) {
        (void)::fsync(directory);
        ::close(directory);
    }
#else
    (void)path;
    (void)content;
    throw std::runtime_error("CAN frame state is not supported on Windows");
#endif
}

bool readFile(const std::string& path, std::string& content) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        if (errno == ENOENT) {
            return false;
        }
        throw std::runtime_error("cannot open " + path + ": " + std::strerror(errno));
    }
    content.clear();
    char buffer[4096];
    std::size_t count = 0;
    while ((count = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        content.append(buffer, count);
        if (content.size() > 1024 * 1024) {
            std::fclose(file);
            throw std::runtime_error(path + " is too large for a CAN frame state");
        }
    }
    const bool failed = std::ferror(file) != 0;
    std::fclose(file);
    if (failed) {
        throw std::runtime_error("cannot read " + path);
    }
    return true;
}

}  // namespace can_frame_state
}  // namespace edge_gateway
