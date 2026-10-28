#pragma once

#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>
#include <string>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStorageInfo>

// Read-only host telemetry, sampled at most once per second. No shell, network
// request, new point store, or electrical device connection is involved.
class LocalSystemInfo {
public:
    LocalSystemInfo(QString root = "/opt/modbus-gateway", QString proc = "/proc", QString sys = "/sys")
        : root_(std::move(root)), proc_(std::move(proc)), sys_(std::move(sys)) {}

    std::map<std::string, std::string> read(const std::string& machineCode) {
        if (timer_.isValid() && timer_.elapsed() < 1000) return cached_;
        timer_.start();
        cached_.clear();
        cached_["machineCode"] = machineCode;
        const auto identity = object(root_ + "/config/runtime/device_identity.json");
        cached_["model"] = identity.value("model").toString().toStdString();
        cached_["firmware"] = identity.value("firmwareVersion").toString().toStdString();
        const auto mqtt = object(root_ + "/config/runtime/apps/mqtt-service.json").value("mqtt").toObject();
        cached_["cloud"] = mqtt.isEmpty() ? QString::fromUtf8("配置不可用").toStdString()
            : mqtt.value("enabled").toBool() ? QString::fromUtf8("已启用").toStdString() : QString::fromUtf8("未启用").toStdString();
        bool foundForward = false, forwardEnabled = false;
        for (const auto& name : QDir(root_ + "/config/runtime/apps").entryList({"*.json"}, QDir::Files)) {
            const auto forward = object(root_ + "/config/runtime/apps/" + name).value("mqttForward").toObject();
            foundForward = foundForward || !forward.isEmpty();
            forwardEnabled = forwardEnabled || forward.value("enabled").toBool();
        }
        cached_["thirdParty"] = (forwardEnabled ? QString::fromUtf8("已启用")
            : foundForward ? QString::fromUtf8("未启用") : QString::fromUtf8("未配置")).toStdString();

        std::istringstream cpu(bytes(proc_ + "/stat").toStdString());
        std::string name;
        unsigned long long user, nice, system, idle, wait, irq, softirq, steal;
        if (cpu >> name >> user >> nice >> system >> idle >> wait >> irq >> softirq >> steal && name == "cpu") {
            const auto total = user + nice + system + idle + wait + irq + softirq + steal;
            const auto inactive = idle + wait;
            if (haveCpu_ && total > total_ && inactive >= idle_ && inactive - idle_ <= total - total_)
                cached_["cpu"] = percent(100.0 * (1.0 - double(inactive - idle_) / double(total - total_)));
            total_ = total; idle_ = inactive; haveCpu_ = true;
        } else haveCpu_ = false;

        std::istringstream memory(bytes(proc_ + "/meminfo").toStdString());
        std::string line;
        double totalMemory = -1, available = -1;
        while (std::getline(memory, line)) {
            std::istringstream row(line);
            std::string key; double value;
            if (!(row >> key >> value)) continue;
            if (key == "MemTotal:") totalMemory = value;
            if (key == "MemAvailable:") available = value;
        }
        if (totalMemory > 0 && available >= 0 && available <= totalMemory)
            cached_["memory"] = percent(100.0 * (1 - available / totalMemory));
        QStorageInfo disk(root_);
        if (disk.isValid() && disk.isReady() && disk.bytesTotal() > 0 && disk.bytesFree() >= 0)
            cached_["disk"] = percent(100.0 * (1 - double(disk.bytesFree()) / disk.bytesTotal()));
        double hottest = -1000;
        for (const auto& zone : QDir(sys_ + "/class/thermal").entryList({"thermal_zone*"}, QDir::Dirs | QDir::NoDotAndDotDot)) {
            bool ok = false;
            const double temperature = bytes(sys_ + "/class/thermal/" + zone + "/temp").trimmed().toDouble(&ok) / 1000;
            if (ok && std::isfinite(temperature) && temperature >= -40 && temperature <= 150)
                hottest = std::max(hottest, temperature);
        }
        if (hottest >= -40) cached_["temperature"] = (QString::number(hottest, 'f', 1) + QString::fromUtf8(" °C")).toStdString();
        return cached_;
    }

private:
    static QByteArray bytes(const QString& path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) return {};
        return file.read(1024 * 1024);
    }
    static QJsonObject object(const QString& path) { return QJsonDocument::fromJson(bytes(path)).object(); }
    static std::string percent(double number) { return (QString::number(number, 'f', 1) + " %").toStdString(); }
    QString root_, proc_, sys_;
    QElapsedTimer timer_;
    std::map<std::string, std::string> cached_;
    bool haveCpu_ = false;
    unsigned long long total_ = 0, idle_ = 0;
};
