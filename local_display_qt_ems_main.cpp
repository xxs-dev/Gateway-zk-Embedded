#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef _WIN32
#include <sys/prctl.h>
#endif

#include <QApplication>
#include <QAbstractItemView>
#include <QColor>
#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/point_store_router.hpp"

namespace {

std::atomic<bool> g_running(true);

void handleSignal(int) {
    g_running = false;
    QTimer::singleShot(0, qApp, &QCoreApplication::quit);
}

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

void setProcessName(const std::string& name) {
#ifndef _WIN32
    prctl(PR_SET_NAME, name.substr(0, 15).c_str(), 0, 0, 0);
#else
    (void)name;
#endif
}

QString qs(const std::string& value) {
    return QString::fromStdString(value);
}

std::string lowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool containsCi(const std::string& value, const std::string& needle) {
    if (needle.empty()) {
        return true;
    }
    return lowerCopy(value).find(lowerCopy(needle)) != std::string::npos;
}

QString formatValue(double value) {
    QString text = QString::number(value, 'f', 3);
    while (text.contains('.') && text.endsWith('0')) {
        text.chop(1);
    }
    if (text.endsWith('.')) {
        text.chop(1);
    }
    return text;
}

QString formatTime(std::int64_t ts) {
    if (ts <= 0) {
        return "-";
    }
    const auto seconds = static_cast<time_t>(ts / 1000);
    std::tm tmValue;
#ifdef _WIN32
    localtime_s(&tmValue, &seconds);
#else
    localtime_r(&seconds, &tmValue);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &tmValue);
    return buffer;
}

struct PointMeta {
    std::uint32_t index = 0;
    std::string machineCode;
    std::string meterCode;
    std::string pointCode;
    std::string name;
    std::string category;
    std::string unit;
    std::string deviceName;
    std::string sharedMemoryName;
    bool writable = false;
};

struct DisplayPoint {
    PointMeta meta;
    bool hasValue = false;
    edge_gateway::StoredPointValue value;
};

std::vector<edge_gateway::PointDefinition> effectiveMeterPoints(
    const edge_gateway::DeviceConfig& config,
    const edge_gateway::LogicalDeviceConfig& device
) {
    if (!device.points.empty()) {
        return device.points;
    }
    if (config.protocol.type != "dlt645_2007" || config.protocol.standardPointsFile.empty()) {
        return {};
    }
    return edge_gateway::ConfigLoader::loadDlt645StandardPointsFromFile(config.protocol.standardPointsFile);
}

std::unordered_map<std::uint32_t, PointMeta> buildPointMeta(
    const std::vector<edge_gateway::DeviceConfig>& deviceConfigs,
    const std::string& fallbackSharedMemoryName
) {
    std::unordered_map<std::uint32_t, PointMeta> result;
    for (const auto& config : deviceConfigs) {
        const auto sharedMemoryName = config.memoryStore.sharedMemoryName.empty()
            ? fallbackSharedMemoryName
            : config.memoryStore.sharedMemoryName;

        std::size_t meterIndex = 0;
        for (const auto& meter : config.meters) {
            auto points = effectiveMeterPoints(config, meter);
            if (config.protocol.type == "dlt645_2007" && meter.points.empty()) {
                const std::uint32_t indexBase = 200000U + static_cast<std::uint32_t>(meterIndex) * 10000U;
                for (std::size_t i = 0; i < points.size(); ++i) {
                    points[i].index = indexBase + static_cast<std::uint32_t>(i);
                }
            }
            for (const auto& point : points) {
                PointMeta meta;
                meta.index = point.index;
                meta.machineCode = config.machineCode;
                meta.meterCode = meter.meterCode;
                meta.pointCode = point.pointCode;
                meta.name = point.name;
                meta.category = point.category;
                meta.unit = point.read.unit;
                meta.deviceName = meter.deviceName;
                meta.sharedMemoryName = sharedMemoryName;
                meta.writable = point.write.enable;
                result[meta.index] = meta;
            }
            ++meterIndex;
        }

        for (const auto& point : config.points) {
            PointMeta meta;
            meta.index = point.index;
            meta.machineCode = config.machineCode;
            meta.meterCode = config.meterCode;
            meta.pointCode = point.pointCode;
            meta.name = point.name;
            meta.category = point.category;
            meta.unit = point.read.unit;
            meta.deviceName = config.deviceName;
            meta.sharedMemoryName = sharedMemoryName;
            meta.writable = point.write.enable;
            result[meta.index] = meta;
        }
    }
    return result;
}

std::vector<std::string> collectSharedMemoryNames(
    const edge_gateway::AppConfig& appConfig,
    const std::vector<edge_gateway::DeviceConfig>& deviceConfigs
) {
    std::vector<std::string> names;
    std::unordered_set<std::string> seen;
    auto addName = [&](const std::string& name) {
        if (!name.empty() && seen.insert(name).second) {
            names.push_back(name);
        }
    };
    for (const auto& name : appConfig.localDisplay.sharedMemoryNames) {
        addName(name);
    }
    for (const auto& name : appConfig.mqttDriver.sharedMemoryNames) {
        addName(name);
    }
    addName(appConfig.mqttDriver.sharedMemoryName);
    for (const auto& config : deviceConfigs) {
        addName(config.memoryStore.sharedMemoryName);
    }
    if (names.empty()) {
        addName("gateway_point_store");
    }
    return names;
}

std::string resolveMachineCode(
    const edge_gateway::DeviceIdentity& identity,
    const edge_gateway::AppConfig& appConfig,
    const std::vector<edge_gateway::DeviceConfig>& deviceConfigs
) {
    if (!identity.machineCode.empty()) {
        return identity.machineCode;
    }
    if (!appConfig.mqtt.clientId.empty()) {
        return appConfig.mqtt.clientId;
    }
    for (const auto& config : deviceConfigs) {
        if (!config.machineCode.empty()) {
            return config.machineCode;
        }
    }
    return "UNKNOWN";
}

class EmsQtWindow : public QWidget {
public:
    EmsQtWindow(
        edge_gateway::LocalDisplayConfig config,
        std::string machineCode,
        edge_gateway::PointStoreRouter& router,
        std::unordered_map<std::uint32_t, PointMeta> pointMeta,
        int refreshMs,
        std::size_t maxPoints
    )
        : config_(std::move(config)),
          machineCode_(std::move(machineCode)),
          router_(router),
          pointMeta_(std::move(pointMeta)),
          refreshMs_(std::max(refreshMs, 500)),
          maxPoints_(maxPoints == 0 ? 500 : maxPoints) {
        setWindowTitle(QString::fromUtf8("KY EMS Local Display PoC"));
        buildPointList();
        buildUi();
        refresh();

        auto* timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this]() { refresh(); });
        timer->start(refreshMs_);
    }

private:
    enum class PageKind {
        Overview,
        Run,
        Pcs,
        Bms,
        All
    };

    struct TablePage {
        PageKind kind;
        QTableWidget* table = nullptr;
    };

    void buildUi() {
        setStyleSheet(R"CSS(
            QWidget { background:#101820; color:#e8f0f2; font-family:"Microsoft YaHei","Noto Sans CJK SC","DejaVu Sans"; }
            QLabel#title { font-size:30px; font-weight:700; }
            QLabel#sub { color:#8fa4ad; font-size:14px; }
            QLineEdit { background:#0f171e; border:1px solid #26323d; border-radius:6px; padding:8px; color:#e8f0f2; font-size:16px; }
            QTabWidget::pane { border:1px solid #26323d; top:-1px; }
            QTabBar::tab { background:#151d25; color:#8fa4ad; padding:10px 18px; border:1px solid #26323d; min-width:120px; }
            QTabBar::tab:selected { color:#e8f0f2; background:#24465c; }
            QFrame.card { background:#182129; border:1px solid #26323d; border-radius:8px; }
            QLabel.cardTitle { color:#8fa4ad; font-size:14px; }
            QLabel.cardValue { font-size:28px; font-weight:700; }
            QTableWidget { background:#182129; alternate-background-color:#141c24; gridline-color:#26323d; selection-background-color:#24465c; font-size:15px; }
            QHeaderView::section { background:#151d25; color:#8fa4ad; border:1px solid #26323d; padding:7px; font-size:14px; }
        )CSS");

        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(22, 18, 22, 18);
        root->setSpacing(12);

        auto* header = new QGridLayout();
        titleLabel_ = new QLabel(QString::fromUtf8("KY EMS 本地显示 PoC"));
        titleLabel_->setObjectName("title");
        machineLabel_ = new QLabel();
        machineLabel_->setObjectName("sub");
        clockLabel_ = new QLabel();
        clockLabel_->setObjectName("sub");
        clockLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        searchEdit_ = new QLineEdit();
        searchEdit_->setPlaceholderText(QString::fromUtf8("搜索 index / 设备 / 表计 / 点位 / 名称"));
        connect(searchEdit_, &QLineEdit::textChanged, this, [this]() { refresh(); });
        header->addWidget(titleLabel_, 0, 0);
        header->addWidget(clockLabel_, 0, 1);
        header->addWidget(machineLabel_, 1, 0);
        header->addWidget(searchEdit_, 1, 1);
        root->addLayout(header);

        auto* cards = new QGridLayout();
        cards->setSpacing(12);
        pointCount_ = addCard(cards, 0, QString::fromUtf8("配置点位"), "#e8f0f2");
        goodCount_ = addCard(cards, 1, QString::fromUtf8("正常"), "#16c784");
        badCount_ = addCard(cards, 2, QString::fromUtf8("异常/无值"), "#ff5c5c");
        writableCount_ = addCard(cards, 3, QString::fromUtf8("可写点"), "#56b6ff");
        root->addLayout(cards);

        tabs_ = new QTabWidget();
        addTablePage(QString::fromUtf8("首页"), PageKind::Overview);
        addTablePage(QString::fromUtf8("运行监测"), PageKind::Run);
        addTablePage(QString::fromUtf8("PCS"), PageKind::Pcs);
        addTablePage(QString::fromUtf8("BMS"), PageKind::Bms);
        addTablePage(QString::fromUtf8("全部点位"), PageKind::All);
        root->addWidget(tabs_, 1);
    }

    QLabel* addCard(QGridLayout* layout, int column, const QString& title, const QString& color) {
        auto* frame = new QFrame();
        frame->setProperty("class", "card");
        auto* box = new QVBoxLayout(frame);
        box->setContentsMargins(16, 12, 16, 12);
        auto* titleLabel = new QLabel(title);
        titleLabel->setProperty("class", "cardTitle");
        auto* valueLabel = new QLabel("0");
        valueLabel->setProperty("class", "cardValue");
        valueLabel->setStyleSheet("color:" + color + ";");
        box->addWidget(titleLabel);
        box->addWidget(valueLabel);
        layout->addWidget(frame, 0, column);
        return valueLabel;
    }

    void addTablePage(const QString& title, PageKind kind) {
        auto* table = new QTableWidget(0, 10);
        table->setHorizontalHeaderLabels({
            "Index",
            QString::fromUtf8("设备"),
            QString::fromUtf8("表计"),
            QString::fromUtf8("点位编码"),
            QString::fromUtf8("名称"),
            QString::fromUtf8("值"),
            QString::fromUtf8("单位"),
            QString::fromUtf8("质量"),
            QString::fromUtf8("时间"),
            QString::fromUtf8("写")
        });
        table->verticalHeader()->setVisible(false);
        table->setAlternatingRowColors(true);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->horizontalHeader()->setStretchLastSection(false);
        table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
        table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(7, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(8, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(9, QHeaderView::ResizeToContents);
        pages_.push_back(TablePage{kind, table});
        tabs_->addTab(table, title);
    }

    void buildPointList() {
        allIndexes_.clear();
        std::unordered_set<std::uint32_t> seen;
        for (const auto& item : router_.routes()) {
            if (seen.insert(item.first).second) {
                allIndexes_.push_back(item.first);
            }
            auto metaIt = pointMeta_.find(item.first);
            if (metaIt == pointMeta_.end()) {
                PointMeta meta;
                meta.index = item.first;
                meta.machineCode = item.second.machineCode;
                meta.meterCode = item.second.meterCode;
                meta.pointCode = item.second.pointCode;
                meta.sharedMemoryName = item.second.sharedMemoryName;
                meta.writable = item.second.writable;
                pointMeta_[item.first] = meta;
            } else {
                metaIt->second.writable = metaIt->second.writable || item.second.writable;
                if (metaIt->second.sharedMemoryName.empty()) {
                    metaIt->second.sharedMemoryName = item.second.sharedMemoryName;
                }
            }
        }
        std::sort(allIndexes_.begin(), allIndexes_.end());
    }

    std::vector<std::uint32_t> configuredOverviewIndexes() const {
        std::vector<std::uint32_t> result;
        std::unordered_set<std::uint32_t> seen;
        auto add = [&](std::uint32_t index) {
            if (index > 0 && pointMeta_.find(index) != pointMeta_.end() && seen.insert(index).second) {
                result.push_back(index);
            }
        };
        for (const auto& screen : config_.screens) {
            for (const auto& widget : screen.widgets) {
                add(widget.pointIndex);
                for (const auto index : widget.pointIndexes) {
                    add(index);
                }
            }
        }
        for (const auto& page : config_.pages) {
            for (const auto& group : page.groups) {
                for (const auto index : group.pointIndexes) {
                    add(index);
                }
            }
        }
        return result;
    }

    bool matchesPage(PageKind kind, const PointMeta& meta) const {
        if (kind == PageKind::All || kind == PageKind::Run || kind == PageKind::Overview) {
            return true;
        }
        const auto combined = lowerCopy(
            meta.deviceName + " " + meta.meterCode + " " + meta.pointCode + " " + meta.name + " " + meta.category
        );
        if (kind == PageKind::Pcs) {
            return combined.find("pcs") != std::string::npos
                || (meta.index >= 600 && meta.index <= 650)
                || (meta.index >= 1300 && meta.index <= 1399);
        }
        if (kind == PageKind::Bms) {
            return combined.find("bms") != std::string::npos
                || combined.find("bat") != std::string::npos
                || combined.find("soc") != std::string::npos
                || combined.find("soh") != std::string::npos
                || (meta.index >= 1500 && meta.index <= 1699);
        }
        return true;
    }

    bool matchesSearch(const PointMeta& meta) const {
        const auto text = searchEdit_ == nullptr ? QString() : searchEdit_->text().trimmed();
        if (text.isEmpty()) {
            return true;
        }
        const auto needle = text.toStdString();
        if (std::to_string(meta.index).find(needle) != std::string::npos) {
            return true;
        }
        return containsCi(meta.deviceName, needle)
            || containsCi(meta.meterCode, needle)
            || containsCi(meta.pointCode, needle)
            || containsCi(meta.name, needle)
            || containsCi(meta.category, needle);
    }

    std::vector<std::uint32_t> selectIndexes(PageKind kind) const {
        std::vector<std::uint32_t> indexes;
        if (kind == PageKind::Overview) {
            indexes = configuredOverviewIndexes();
            if (indexes.empty()) {
                static const std::uint32_t curated[] = {
                    1039, 1043, 1139, 1143, 1399, 1570,
                    627, 628, 629, 630, 631, 632,
                    1318, 1319, 1320, 1321, 1322, 1323
                };
                for (const auto index : curated) {
                    if (pointMeta_.find(index) != pointMeta_.end()) {
                        indexes.push_back(index);
                    }
                }
            }
        }

        if (indexes.empty()) {
            for (const auto index : allIndexes_) {
                const auto metaIt = pointMeta_.find(index);
                if (metaIt == pointMeta_.end()) {
                    continue;
                }
                if (matchesPage(kind, metaIt->second) && matchesSearch(metaIt->second)) {
                    indexes.push_back(index);
                }
            }
        } else {
            std::vector<std::uint32_t> filtered;
            for (const auto index : indexes) {
                const auto metaIt = pointMeta_.find(index);
                if (metaIt != pointMeta_.end() && matchesSearch(metaIt->second)) {
                    filtered.push_back(index);
                }
            }
            indexes.swap(filtered);
        }

        if (indexes.size() > maxPoints_) {
            indexes.resize(maxPoints_);
        }
        return indexes;
    }

    std::vector<DisplayPoint> loadDisplayPoints(const std::vector<std::uint32_t>& indexes) const {
        const auto values = router_.getLatestByIndexes(indexes, nowMs());
        std::unordered_map<std::uint32_t, edge_gateway::StoredPointValue> byIndex;
        for (const auto& value : values) {
            byIndex[value.index] = value;
        }
        std::vector<DisplayPoint> result;
        result.reserve(indexes.size());
        for (const auto index : indexes) {
            DisplayPoint point;
            const auto metaIt = pointMeta_.find(index);
            if (metaIt != pointMeta_.end()) {
                point.meta = metaIt->second;
            } else {
                point.meta.index = index;
            }
            const auto valueIt = byIndex.find(index);
            if (valueIt != byIndex.end()) {
                point.hasValue = true;
                point.value = valueIt->second;
            }
            result.push_back(point);
        }
        return result;
    }

    void refresh() {
        machineLabel_->setText(QString("machine: %1  points: %2  refresh: %3ms")
            .arg(qs(machineCode_))
            .arg(allIndexes_.size())
            .arg(refreshMs_));
        clockLabel_->setText(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));

        const auto allValues = router_.getLatestByIndexes(allIndexes_, nowMs());
        std::unordered_set<std::uint32_t> goodIndexes;
        std::unordered_set<std::uint32_t> badIndexes;
        for (const auto& value : allValues) {
            if (value.quality == 1 && !value.stale) {
                goodIndexes.insert(value.index);
            } else {
                badIndexes.insert(value.index);
            }
        }
        std::size_t writable = 0;
        for (const auto& index : allIndexes_) {
            const auto metaIt = pointMeta_.find(index);
            if (metaIt != pointMeta_.end() && metaIt->second.writable) {
                ++writable;
            }
        }
        pointCount_->setText(QString::number(allIndexes_.size()));
        goodCount_->setText(QString::number(goodIndexes.size()));
        badCount_->setText(QString::number(allIndexes_.size() - goodIndexes.size()));
        writableCount_->setText(QString::number(writable));

        for (auto& page : pages_) {
            const auto indexes = selectIndexes(page.kind);
            fillTable(page.table, loadDisplayPoints(indexes));
        }
    }

    QColor pointColor(const DisplayPoint& point) const {
        if (!point.hasValue) {
            return QColor("#ff5c5c");
        }
        if (point.value.stale) {
            return QColor("#f5a623");
        }
        return point.value.quality == 1 ? QColor("#16c784") : QColor("#ff5c5c");
    }

    void fillTable(QTableWidget* table, const std::vector<DisplayPoint>& points) {
        table->setRowCount(static_cast<int>(points.size()));
        for (int row = 0; row < static_cast<int>(points.size()); ++row) {
            const auto& point = points[static_cast<std::size_t>(row)];
            const auto color = pointColor(point);
            setItem(table, row, 0, QString::number(point.meta.index), color);
            setItem(table, row, 1, qs(point.meta.deviceName), QColor("#e8f0f2"));
            setItem(table, row, 2, qs(point.meta.meterCode), QColor("#e8f0f2"));
            setItem(table, row, 3, qs(point.meta.pointCode), QColor("#e8f0f2"));
            setItem(table, row, 4, qs(point.meta.name), QColor("#e8f0f2"));
            setItem(table, row, 5, point.hasValue ? formatValue(point.value.value) : "-", color);
            setItem(table, row, 6, qs(point.meta.unit), QColor("#8fa4ad"));
            setItem(table, row, 7, point.hasValue ? QString::number(point.value.quality) : "-", color);
            setItem(table, row, 8, point.hasValue ? formatTime(point.value.ts) : "-", QColor("#8fa4ad"));
            setItem(table, row, 9, point.meta.writable ? QString::fromUtf8("是") : QString::fromUtf8("否"), point.meta.writable ? QColor("#56b6ff") : QColor("#8fa4ad"));
            table->setRowHeight(row, 34);
        }
    }

    void setItem(QTableWidget* table, int row, int column, const QString& text, const QColor& color) {
        auto* item = table->item(row, column);
        if (item == nullptr) {
            item = new QTableWidgetItem();
            table->setItem(row, column, item);
        }
        item->setText(text);
        item->setForeground(color);
    }

    edge_gateway::LocalDisplayConfig config_;
    std::string machineCode_;
    edge_gateway::PointStoreRouter& router_;
    std::unordered_map<std::uint32_t, PointMeta> pointMeta_;
    int refreshMs_ = 1000;
    std::size_t maxPoints_ = 500;
    std::vector<std::uint32_t> allIndexes_;
    QLabel* titleLabel_ = nullptr;
    QLabel* machineLabel_ = nullptr;
    QLabel* clockLabel_ = nullptr;
    QLineEdit* searchEdit_ = nullptr;
    QLabel* pointCount_ = nullptr;
    QLabel* goodCount_ = nullptr;
    QLabel* badCount_ = nullptr;
    QLabel* writableCount_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    std::vector<TablePage> pages_;
};

void printUsage(const char* argv0) {
    std::cout << "usage: " << argv0
              << " --app-config <app.json>"
              << " [--windowed]"
              << " [--refresh-ms 1000]"
              << " [--max-points 500]"
              << std::endl;
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string appConfigPath = "config/runtime/apps/monitor-service.json";
    bool fullscreen = true;
    int refreshMs = 1000;
    std::size_t maxPoints = 500;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto requireValue = [&](const std::string& name) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error("missing value for " + name);
            }
            return argv[++i];
        };
        if (arg == "--app-config") {
            appConfigPath = requireValue(arg);
        } else if (arg == "--windowed") {
            fullscreen = false;
        } else if (arg == "--refresh-ms") {
            refreshMs = std::stoi(requireValue(arg));
        } else if (arg == "--max-points") {
            maxPoints = static_cast<std::size_t>(std::stoul(requireValue(arg)));
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    setProcessName("ems_qt_display");

    try {
        auto appConfig = edge_gateway::ConfigLoader::loadAppConfigFromFile(appConfigPath);
        edge_gateway::DeviceIdentity identity;
        if (!appConfig.identityConfigFile.empty()) {
            try {
                identity = edge_gateway::ConfigLoader::loadDeviceIdentityFromFile(appConfig.identityConfigFile);
            } catch (const std::exception& ex) {
                std::cerr << "load identity failed: " << ex.what() << std::endl;
            }
        }

        auto deviceConfigs = appConfig.identityConfigFile.empty()
            ? edge_gateway::ConfigLoader::loadMany(appConfig.deviceConfigFiles)
            : edge_gateway::ConfigLoader::loadMany(appConfig.deviceConfigFiles, identity);

        const auto sharedMemoryNames = collectSharedMemoryNames(appConfig, deviceConfigs);
        std::vector<std::unique_ptr<edge_gateway::MemoryPointStore>> stores;
        edge_gateway::PointStoreRouter router;
        for (const auto& name : sharedMemoryNames) {
            stores.emplace_back(new edge_gateway::MemoryPointStore(name));
            router.addStore(name, *stores.back());
        }

        const auto fallbackSharedMemoryName = sharedMemoryNames.empty()
            ? std::string("gateway_point_store")
            : sharedMemoryNames.front();
        const auto machineCode = resolveMachineCode(identity, appConfig, deviceConfigs);
        router.addRoutesFromDeviceConfigs(deviceConfigs, fallbackSharedMemoryName);
        router.addRoutesFromCameraServiceConfig(appConfig.cameraService, machineCode);
        const auto pointMeta = buildPointMeta(deviceConfigs, fallbackSharedMemoryName);

        QApplication app(argc, argv);
        EmsQtWindow window(
            appConfig.localDisplay,
            machineCode,
            router,
            pointMeta,
            refreshMs,
            maxPoints
        );
        if (fullscreen) {
            window.showFullScreen();
        } else {
            window.resize(1280, 720);
            window.show();
        }
        return app.exec();
    } catch (const std::exception& ex) {
        std::cerr << "local EMS Qt display failed: " << ex.what() << std::endl;
        return 1;
    }
}
