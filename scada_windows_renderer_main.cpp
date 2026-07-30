#include "edge_gateway/scada_project_loader.hpp"
#include "local_display_qt_scada_runtime.hpp"
#include "local_display_qt_scada_scene.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <QApplication>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QTimer>

namespace {

std::int64_t jsonInt64(const QJsonObject& object, const char* key, std::int64_t fallback = 0) {
    const auto value = object.value(QString::fromLatin1(key));
    return value.isDouble() ? static_cast<std::int64_t>(value.toDouble()) : fallback;
}

class BridgeRuntimeHub final {
public:
    explicit BridgeRuntimeHub(std::string machineCode) : machineCode_(std::move(machineCode)) {}

    std::vector<edge_gateway::StoredPointValue> readIndexes(
        const std::vector<std::uint32_t>& indexes
    ) const {
        std::lock_guard<std::mutex> lock(valuesMutex_);
        std::vector<edge_gateway::StoredPointValue> result;
        result.reserve(indexes.size());
        for (const auto index : indexes) {
            const auto item = values_.find(index);
            if (item != values_.end()) result.push_back(item->second);
        }
        return result;
    }

    edge_gateway::Optional<edge_gateway::StoredPointValue> readIndex(std::uint32_t index) const {
        std::lock_guard<std::mutex> lock(valuesMutex_);
        const auto item = values_.find(index);
        return item == values_.end()
            ? edge_gateway::Optional<edge_gateway::StoredPointValue>(edge_gateway::NullOpt)
            : edge_gateway::Optional<edge_gateway::StoredPointValue>(item->second);
    }

    void updateSubscription(std::vector<std::uint32_t> indexes) {
        std::sort(indexes.begin(), indexes.end());
        indexes.erase(std::unique(indexes.begin(), indexes.end()), indexes.end());
        {
            std::lock_guard<std::mutex> lock(subscriptionMutex_);
            if (indexes == subscription_) return;
            subscription_ = indexes;
        }

        QJsonArray items;
        for (const auto index : indexes) items.push_back(static_cast<qint64>(index));
        QJsonObject message;
        message.insert(QStringLiteral("type"), QStringLiteral("subscribe"));
        message.insert(QStringLiteral("machineCode"), QString::fromStdString(machineCode_));
        message.insert(QStringLiteral("indexes"), items);
        writeMessage(message);
    }

    ScadaSceneWriteResult submit(
        const ScadaSceneResolvedTag& resolved,
        edge_gateway::PendingWriteCommand command
    ) {
        command.index = resolved.mapping.index;
        QJsonObject message;
        message.insert(QStringLiteral("type"), QStringLiteral("control"));
        message.insert(QStringLiteral("requestId"), QString::fromStdString(command.cmdId));
        message.insert(QStringLiteral("machineCode"), QString::fromStdString(machineCode_));
        message.insert(QStringLiteral("nodeId"), QString::fromStdString(resolved.tag.nodeId));
        message.insert(QStringLiteral("tagId"), QString::fromStdString(resolved.tag.tagId));
        message.insert(QStringLiteral("meterCode"), QString::fromStdString(resolved.tag.meterCode));
        message.insert(QStringLiteral("pointCode"), QString::fromStdString(resolved.tag.pointCode));
        message.insert(QStringLiteral("index"), static_cast<qint64>(command.index));
        message.insert(QStringLiteral("value"), command.value);
        message.insert(QStringLiteral("highPriority"), command.highPriority);
        message.insert(QStringLiteral("ts"), static_cast<qint64>(command.ts));
        writeMessage(message);
        return {true, "control submitted to GatewayDesktop host"};
    }

    void acceptInput(const std::string& line) {
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(line), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            writeProtocolError("invalid_json", parseError.errorString().toStdString());
            return;
        }
        const auto root = document.object();
        const auto type = root.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("pointBatch")) {
            applyPointBatch(root.value(QStringLiteral("points")).toArray());
            return;
        }
        if (type == QStringLiteral("shutdown")) {
            QMetaObject::invokeMethod(qApp, []() { qApp->quit(); }, Qt::QueuedConnection);
            return;
        }
        if (type == QStringLiteral("ping")) {
            QJsonObject reply;
            reply.insert(QStringLiteral("type"), QStringLiteral("pong"));
            reply.insert(QStringLiteral("machineCode"), QString::fromStdString(machineCode_));
            writeMessage(reply);
            return;
        }
        if (type != QStringLiteral("controlResult") && !type.isEmpty()) {
            writeProtocolError("unsupported_message", type.toStdString());
        }
    }

    void writeReady(const edge_gateway::ScadaProject& project) {
        QJsonObject message;
        message.insert(QStringLiteral("type"), QStringLiteral("ready"));
        message.insert(QStringLiteral("protocolVersion"), 1);
        message.insert(QStringLiteral("machineCode"), QString::fromStdString(machineCode_));
        message.insert(QStringLiteral("projectId"), QString::fromStdString(project.manifest.projectId));
        message.insert(QStringLiteral("packageVersion"), QString::fromStdString(project.manifest.packageVersion));
        message.insert(QStringLiteral("screenCount"), static_cast<int>(project.screens.size()));
        message.insert(QStringLiteral("pid"), static_cast<qint64>(QCoreApplication::applicationPid()));
        writeMessage(message);
    }

private:
    void applyPointBatch(const QJsonArray& points) {
        std::lock_guard<std::mutex> lock(valuesMutex_);
        for (const auto& item : points) {
            if (!item.isObject()) continue;
            const auto object = item.toObject();
            const auto indexValue = jsonInt64(object, "index");
            if (indexValue <= 0 || indexValue > UINT32_MAX) continue;
            edge_gateway::StoredPointValue point;
            point.index = static_cast<std::uint32_t>(indexValue);
            point.machineCode = object.value(QStringLiteral("machineCode")).toString(
                QString::fromStdString(machineCode_)
            ).toStdString();
            point.meterCode = object.value(QStringLiteral("meterCode")).toString().toStdString();
            point.pointCode = object.value(QStringLiteral("pointCode")).toString().toStdString();
            point.value = object.value(QStringLiteral("value")).toDouble();
            point.quality = object.value(QStringLiteral("quality")).toInt(1);
            point.ts = jsonInt64(object, "ts");
            point.expireAt = jsonInt64(object, "expireAt");
            point.stale = object.value(QStringLiteral("stale")).toBool(false);
            values_[point.index] = std::move(point);
        }
    }

    void writeProtocolError(const std::string& code, const std::string& messageText) {
        QJsonObject message;
        message.insert(QStringLiteral("type"), QStringLiteral("error"));
        message.insert(QStringLiteral("code"), QString::fromStdString(code));
        message.insert(QStringLiteral("message"), QString::fromStdString(messageText));
        writeMessage(message);
    }

    void writeMessage(const QJsonObject& message) {
        const auto line = QJsonDocument(message).toJson(QJsonDocument::Compact);
        std::lock_guard<std::mutex> lock(outputMutex_);
        std::cout << line.constData() << std::endl;
    }

    std::string machineCode_;
    mutable std::mutex valuesMutex_;
    mutable std::mutex subscriptionMutex_;
    std::mutex outputMutex_;
    std::unordered_map<std::uint32_t, edge_gateway::StoredPointValue> values_;
    std::vector<std::uint32_t> subscription_;
};

class BridgeRuntimeSource final : public ScadaSceneRuntimeSource {
public:
    BridgeRuntimeSource(
        const edge_gateway::ScadaProject& project,
        const std::string& machineCode,
        std::shared_ptr<BridgeRuntimeHub> hub
    ) : machineCode_(machineCode), hub_(std::move(hub)) {
        const auto node = std::find_if(project.nodes.begin(), project.nodes.end(), [&](const auto& item) {
            return item.machineCode == machineCode_;
        });
        if (node == project.nodes.end()) {
            throw std::runtime_error("SCADA project does not contain machineCode: " + machineCode_);
        }
        nodeId_ = node->nodeId;
        for (const auto& tag : project.tags) {
            if (tag.nodeId == nodeId_) tags_[tag.tagId] = tag;
        }
        for (const auto& mapping : project.runtimeMappings) {
            if (mapping.nodeId == nodeId_) mappings_[mapping.tagId] = mapping;
        }
    }

    const std::string& nodeId() const override { return nodeId_; }

    edge_gateway::Optional<ScadaSceneResolvedTag> resolveTag(const std::string& tagId) const override {
        const auto tag = tags_.find(tagId);
        const auto mapping = mappings_.find(tagId);
        if (tag == tags_.end() || mapping == mappings_.end()) return edge_gateway::NullOpt;
        return ScadaSceneResolvedTag{tag->second, mapping->second};
    }

    edge_gateway::Optional<edge_gateway::StoredPointValue> readTag(
        const std::string& tagId,
        std::int64_t
    ) const override {
        const auto mapping = mappings_.find(tagId);
        return mapping == mappings_.end() ? edge_gateway::NullOpt : hub_->readIndex(mapping->second.index);
    }

    std::vector<edge_gateway::StoredPointValue> readIndexes(
        const std::vector<std::uint32_t>& indexes,
        std::int64_t
    ) const override {
        hub_->updateSubscription(indexes);
        return hub_->readIndexes(indexes);
    }

    ScadaSceneWriteResult submitWrite(
        const std::string& tagId,
        edge_gateway::PendingWriteCommand command
    ) override {
        const auto resolved = resolveTag(tagId);
        if (!resolved) return {false, "SCADA control tag or runtime mapping is missing"};
        if (resolved->tag.access == edge_gateway::ScadaTagAccess::Read || !resolved->mapping.writable) {
            return {false, "SCADA tag is read-only"};
        }
        return hub_->submit(*resolved, std::move(command));
    }

private:
    std::string machineCode_;
    std::string nodeId_;
    std::shared_ptr<BridgeRuntimeHub> hub_;
    std::unordered_map<std::string, edge_gateway::ScadaTag> tags_;
    std::unordered_map<std::string, edge_gateway::ScadaRuntimeMapping> mappings_;
};

void printUsage(const char* executable) {
    std::cout << "usage: " << executable
              << " --project-dir <directory> --machine-code <code>"
              << " [--fullscreen] [--refresh-ms 500] [--auto-reload] [--screenshot <png>]"
              << std::endl;
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string projectDirectory;
    std::string machineCode;
    int refreshMs = 500;
    bool fullscreen = false;
    bool autoReload = false;
    std::string screenshotPath;

    try {
        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            auto requireValue = [&](const std::string& name) {
                if (index + 1 >= argc) throw std::runtime_error("missing value for " + name);
                return std::string(argv[++index]);
            };
            if (argument == "--project-dir") projectDirectory = requireValue(argument);
            else if (argument == "--machine-code") machineCode = requireValue(argument);
            else if (argument == "--refresh-ms") refreshMs = std::stoi(requireValue(argument));
            else if (argument == "--fullscreen") fullscreen = true;
            else if (argument == "--auto-reload") autoReload = true;
            else if (argument == "--screenshot") screenshotPath = requireValue(argument);
            else if (argument == "--help" || argument == "-h") {
                printUsage(argv[0]);
                return 0;
            } else {
                throw std::runtime_error("unknown argument: " + argument);
            }
        }
        if (projectDirectory.empty()) throw std::runtime_error("--project-dir is required");
        if (machineCode.empty()) throw std::runtime_error("--machine-code is required");

        std::cerr << "renderer stage=application project=" << projectDirectory
                  << " machineCode=" << machineCode << std::endl;
        QApplication app(argc, argv);
        auto hub = std::make_shared<BridgeRuntimeHub>(machineCode);
        std::cerr << "renderer stage=loading-project" << std::endl;
        ScadaRuntimeWindow window(
            projectDirectory,
            refreshMs,
            autoReload,
            [hub, machineCode](const edge_gateway::ScadaProject& project) {
                return std::unique_ptr<ScadaSceneRuntimeSource>(
                    new BridgeRuntimeSource(project, machineCode, hub)
                );
            }
        );
        std::cerr << "renderer stage=project-ready screens=" << window.project().screens.size() << std::endl;
        std::thread([hub]() {
            std::string line;
            while (std::getline(std::cin, line)) {
                if (!line.empty()) hub->acceptInput(line);
            }
            QMetaObject::invokeMethod(qApp, []() { qApp->quit(); }, Qt::QueuedConnection);
        }).detach();

        if (fullscreen) window.showFullScreen();
        else {
            window.resize(1280, 720);
            window.showMaximized();
        }
        hub->writeReady(window.project());
        if (!screenshotPath.empty()) {
            QTimer::singleShot(750, &window, [&window, screenshotPath]() {
                if (!window.grab().save(QString::fromStdString(screenshotPath))) {
                    std::cerr << "failed to save SCADA screenshot: " << screenshotPath << std::endl;
                }
                qApp->quit();
            });
        }
        return app.exec();
    } catch (const std::exception& ex) {
        std::cerr << "KY-SCADA-Windows failed: " << ex.what() << std::endl;
        return 1;
    }
}
