#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <sys/prctl.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <QApplication>
#include <QAbstractItemView>
#include <QColor>
#include <QDateTime>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPalette>
#include <QScrollArea>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
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

std::string basenameOf(const std::string& path) {
    const auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::string sanitizeProcessToken(std::string value) {
    for (auto& ch : value) {
        if (ch == '/' || ch == '\\' || ch == '.' || ch == '-' || ch == ' ') {
            ch = '_';
        }
    }
    return value;
}

void setProcessName(const std::string& name) {
    prctl(PR_SET_NAME, name.substr(0, 15).c_str(), 0, 0, 0);
}

QString qs(const std::string& value) {
    return QString::fromStdString(value);
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
    localtime_r(&seconds, &tmValue);
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
    std::string unit;
    std::string deviceName;
    std::string sharedMemoryName;
};

struct DisplayPoint {
    std::uint32_t index = 0;
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
                meta.unit = point.read.unit;
                meta.deviceName = meter.deviceName;
                meta.sharedMemoryName = sharedMemoryName;
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
            meta.unit = point.read.unit;
            meta.deviceName = config.deviceName;
            meta.sharedMemoryName = sharedMemoryName;
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
    if (names.empty()) {
        for (const auto& name : appConfig.mqttDriver.sharedMemoryNames) {
            addName(name);
        }
        addName(appConfig.mqttDriver.sharedMemoryName);
    }
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
    const std::vector<edge_gateway::DeviceConfig>& deviceConfigs
) {
    if (!identity.machineCode.empty()) {
        return identity.machineCode;
    }
    for (const auto& config : deviceConfigs) {
        if (!config.machineCode.empty()) {
            return config.machineCode;
        }
    }
    return "";
}

std::vector<std::uint32_t> collectDisplayIndexes(
    const edge_gateway::LocalDisplayConfig& config,
    const edge_gateway::PointStoreRouter& router
) {
    std::vector<std::uint32_t> indexes;
    std::unordered_set<std::uint32_t> seen;
    auto addIndex = [&](std::uint32_t index) {
        if (index > 0 && seen.insert(index).second) {
            indexes.push_back(index);
        }
    };
    for (const auto& page : config.layout.pages) {
        for (const auto& widget : page.widgets) {
            if (widget.pointIndex > 0) {
                addIndex(widget.pointIndex);
            }
            for (const auto index : widget.pointIndexes) {
                addIndex(index);
            }
        }
    }
    for (const auto& page : config.pages) {
        for (const auto& group : page.groups) {
            for (const auto index : group.pointIndexes) {
                addIndex(index);
            }
        }
    }
    if (indexes.empty() || !config.showOnlyConfiguredPoints) {
        for (const auto index : router.allIndexes()) {
            addIndex(index);
        }
    }
    const auto maxPoints = config.maxPointsPerFrame == 0 ? 500 : config.maxPointsPerFrame;
    if (indexes.size() > maxPoints) {
        indexes.resize(maxPoints);
    }
    return indexes;
}

bool hasLayoutWidgets(const edge_gateway::LocalDisplayConfig& config) {
    for (const auto& page : config.layout.pages) {
        if (!page.widgets.empty()) {
            return true;
        }
    }
    return false;
}

const edge_gateway::LocalDisplayLayoutPageConfig* firstLayoutPage(
    const edge_gateway::LocalDisplayConfig& config
) {
    for (const auto& page : config.layout.pages) {
        if (!page.widgets.empty()) {
            return &page;
        }
    }
    return nullptr;
}

class LocalDisplayWindow : public QWidget {
public:
    LocalDisplayWindow(
        edge_gateway::LocalDisplayConfig config,
        std::string machineCode,
        edge_gateway::PointStoreRouter& router,
        std::unordered_map<std::uint32_t, PointMeta> meta
    )
        : config_(std::move(config)),
          machineCode_(std::move(machineCode)),
          router_(router),
          meta_(std::move(meta)) {
        if (config_.refreshIntervalMs < 500) {
            config_.refreshIntervalMs = 500;
        }
        setWindowTitle("Gateway Local Display");
        buildUi();
        refresh();

        auto* timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this]() { refresh(); });
        timer->start(config_.refreshIntervalMs);
    }

private:
    struct RuntimeWidget {
        edge_gateway::LocalDisplayWidgetConfig config;
        QLabel* valueLabel = nullptr;
        QLabel* subLabel = nullptr;
        QTableWidget* table = nullptr;
    };

    void buildUi() {
        setStyleSheet(R"CSS(
            QWidget { background: #101820; color: #e8f0f2; font-family: "Noto Sans CJK SC", "DejaVu Sans"; }
            QLabel#title { font-size: 34px; font-weight: 700; }
            QLabel#sub { color: #8fa4ad; font-size: 16px; }
            QFrame.card { background: #182129; border: 1px solid #26323d; border-radius: 12px; }
            QLabel.cardTitle { color: #8fa4ad; font-size: 15px; }
            QLabel.cardValue { font-size: 34px; font-weight: 700; }
            QLabel.groupTitle { color: #e8f0f2; font-size: 28px; font-weight: 700; }
            QLabel.widgetSub { color: #8fa4ad; font-size: 13px; }
            QTableWidget { background: #182129; alternate-background-color: #141c24; gridline-color: #26323d; selection-background-color: #24465c; font-size: 18px; }
            QHeaderView::section { background: #151d25; color: #8fa4ad; border: 1px solid #26323d; padding: 8px; font-size: 16px; }
        )CSS");

        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(26, 20, 26, 20);
        root->setSpacing(16);

        auto* header = new QGridLayout();
        const auto* page = firstLayoutPage(config_);
        title_ = new QLabel(page == nullptr || page->title.empty() ? "Gateway Local Display" : qs(page->title));
        title_->setObjectName("title");
        machineLabel_ = new QLabel();
        machineLabel_->setObjectName("sub");
        clockLabel_ = new QLabel();
        clockLabel_->setObjectName("sub");
        clockLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        header->addWidget(title_, 0, 0);
        header->addWidget(clockLabel_, 0, 1);
        header->addWidget(machineLabel_, 1, 0, 1, 2);
        root->addLayout(header);

        if (hasLayoutWidgets(config_)) {
            buildLayoutUi(root);
            return;
        }

        buildLegacyUi(root);
    }

    void buildLegacyUi(QVBoxLayout* root) {
        auto* cards = new QGridLayout();
        cards->setSpacing(14);
        pointCount_ = addCard(cards, 0, "POINTS", "#e8f0f2");
        onlineCount_ = addCard(cards, 1, "ONLINE", "#16c784");
        badCount_ = addCard(cards, 2, "BAD/STALE", "#ff5c5c");
        refreshLabel_ = addCard(cards, 3, "REFRESH", "#56b6ff");
        root->addLayout(cards);

        table_ = new QTableWidget(0, 8);
        table_->setHorizontalHeaderLabels({"Index", "Meter", "Point", "Name", "Value", "Unit", "Quality", "Time"});
        table_->verticalHeader()->setVisible(false);
        table_->setAlternatingRowColors(true);
        table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table_->setSelectionMode(QAbstractItemView::NoSelection);
        table_->horizontalHeader()->setStretchLastSection(true);
        table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
        table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
        table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
        table_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
        table_->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
        table_->horizontalHeader()->setSectionResizeMode(7, QHeaderView::ResizeToContents);
        root->addWidget(table_, 1);
    }

    void buildLayoutUi(QVBoxLayout* root) {
        const auto* page = firstLayoutPage(config_);
        if (page == nullptr) {
            buildLegacyUi(root);
            return;
        }

        auto* scroll = new QScrollArea();
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto* container = new QWidget();
        auto* grid = new QGridLayout(container);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setSpacing(14);
        const int columns = config_.layout.columns <= 0 ? 12 : config_.layout.columns;
        for (int column = 0; column < columns; ++column) {
            grid->setColumnStretch(column, 1);
        }

        int fallbackRow = 0;
        for (const auto& widget : page->widgets) {
            RuntimeWidget runtime;
            runtime.config = widget;
            auto* view = createLayoutWidget(runtime);
            if (view == nullptr) {
                continue;
            }
            int row = runtime.config.grid.row < 0 ? fallbackRow : runtime.config.grid.row;
            int col = runtime.config.grid.col < 0 ? 0 : runtime.config.grid.col;
            int rowSpan = runtime.config.grid.rowSpan < 1 ? 1 : runtime.config.grid.rowSpan;
            int colSpan = runtime.config.grid.colSpan < 1 ? 1 : runtime.config.grid.colSpan;
            if (col >= columns) {
                col = 0;
            }
            if (col + colSpan > columns) {
                colSpan = columns - col;
            }
            grid->addWidget(view, row, col, rowSpan, colSpan);
            fallbackRow = std::max(fallbackRow, row + rowSpan);
            runtimeWidgets_.push_back(runtime);
        }

        scroll->setWidget(container);
        root->addWidget(scroll, 1);
    }

    QLabel* addCard(QGridLayout* layout, int column, const QString& title, const QString& color) {
        auto* frame = new QFrame();
        frame->setProperty("class", "card");
        auto* box = new QVBoxLayout(frame);
        box->setContentsMargins(18, 14, 18, 14);
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

    QWidget* createLayoutWidget(RuntimeWidget& runtime) {
        const auto type = runtime.config.type;
        if (type == "groupTitle") {
            auto* label = new QLabel(runtime.config.text.empty() ? qs(runtime.config.title) : qs(runtime.config.text));
            label->setProperty("class", "groupTitle");
            label->setWordWrap(true);
            return label;
        }
        if (type == "pointTable" || type == "alarmSummary") {
            return createTableWidget(runtime);
        }
        return createValueWidget(runtime);
    }

    QWidget* createValueWidget(RuntimeWidget& runtime) {
        auto* frame = new QFrame();
        frame->setProperty("class", "card");
        auto* box = new QVBoxLayout(frame);
        box->setContentsMargins(18, 14, 18, 14);
        auto* titleLabel = new QLabel(runtime.config.title.empty()
            ? QString(runtime.config.type == "statusCard" ? "Status" : "Value")
            : qs(runtime.config.title));
        titleLabel->setProperty("class", "cardTitle");
        runtime.valueLabel = new QLabel("-");
        runtime.valueLabel->setProperty("class", "cardValue");
        runtime.subLabel = new QLabel("-");
        runtime.subLabel->setProperty("class", "widgetSub");
        runtime.subLabel->setWordWrap(true);
        box->addWidget(titleLabel);
        box->addWidget(runtime.valueLabel);
        box->addWidget(runtime.subLabel);
        box->addStretch(1);
        return frame;
    }

    QWidget* createTableWidget(RuntimeWidget& runtime) {
        auto* frame = new QFrame();
        frame->setProperty("class", "card");
        auto* box = new QVBoxLayout(frame);
        box->setContentsMargins(14, 12, 14, 14);
        auto* titleLabel = new QLabel(runtime.config.title.empty() ? "Points" : qs(runtime.config.title));
        titleLabel->setProperty("class", "cardTitle");
        runtime.table = new QTableWidget(0, 1);
        runtime.table->verticalHeader()->setVisible(false);
        runtime.table->setAlternatingRowColors(true);
        runtime.table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        runtime.table->setSelectionMode(QAbstractItemView::NoSelection);
        configureTableColumns(runtime.table, runtime.config.columns);
        box->addWidget(titleLabel);
        box->addWidget(runtime.table, 1);
        return frame;
    }

    std::vector<std::string> effectiveColumns(const std::vector<std::string>& columns) const {
        if (!columns.empty()) {
            return columns;
        }
        return {"index", "meterCode", "pointCode", "name", "value", "unit", "quality", "time"};
    }

    QString columnLabel(const std::string& column) const {
        if (column == "index") {
            return "Index";
        }
        if (column == "meterCode") {
            return "Meter";
        }
        if (column == "pointCode") {
            return "Point";
        }
        if (column == "name") {
            return "Name";
        }
        if (column == "value") {
            return "Value";
        }
        if (column == "unit") {
            return "Unit";
        }
        if (column == "quality") {
            return "Quality";
        }
        if (column == "time") {
            return "Time";
        }
        if (column == "statusText") {
            return "Status";
        }
        return qs(column);
    }

    void configureTableColumns(QTableWidget* target, const std::vector<std::string>& configuredColumns) const {
        const auto columns = effectiveColumns(configuredColumns);
        target->setColumnCount(static_cast<int>(columns.size()));
        QStringList labels;
        for (const auto& column : columns) {
            labels << columnLabel(column);
        }
        target->setHorizontalHeaderLabels(labels);
        target->horizontalHeader()->setStretchLastSection(true);
        for (int column = 0; column < static_cast<int>(columns.size()); ++column) {
            const auto& name = columns[static_cast<std::size_t>(column)];
            if (name == "pointCode" || name == "name") {
                target->horizontalHeader()->setSectionResizeMode(column, QHeaderView::Stretch);
            } else {
                target->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
            }
        }
    }

    std::vector<DisplayPoint> loadPoints() const {
        const auto indexes = collectDisplayIndexes(config_, router_);
        const auto values = router_.getLatestByIndexes(indexes, nowMs());
        std::unordered_map<std::uint32_t, edge_gateway::StoredPointValue> valueByIndex;
        for (const auto& value : values) {
            valueByIndex[value.index] = value;
        }

        std::vector<DisplayPoint> result;
        result.reserve(indexes.size());
        for (const auto index : indexes) {
            DisplayPoint point;
            point.index = index;
            const auto metaIt = meta_.find(index);
            if (metaIt != meta_.end()) {
                point.meta = metaIt->second;
            }
            const auto valueIt = valueByIndex.find(index);
            if (valueIt != valueByIndex.end()) {
                point.hasValue = true;
                point.value = valueIt->second;
            }
            result.push_back(point);
        }
        return result;
    }

    void refresh() {
        const auto points = loadPoints();
        std::size_t online = 0;
        std::size_t bad = 0;
        for (const auto& point : points) {
            const bool good = point.hasValue && point.value.quality == 1 && !point.value.stale;
            if (good) {
                ++online;
            } else {
                ++bad;
            }
        }

        machineLabel_->setText("machine: " + qs(machineCode_));
        clockLabel_->setText(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
        if (!runtimeWidgets_.empty()) {
            refreshLayoutWidgets(points);
            return;
        }

        pointCount_->setText(QString::number(points.size()));
        onlineCount_->setText(QString::number(online));
        badCount_->setText(QString::number(bad));
        refreshLabel_->setText(QDateTime::currentDateTime().toString("HH:mm:ss"));

        table_->setRowCount(static_cast<int>(points.size()));
        for (int row = 0; row < static_cast<int>(points.size()); ++row) {
            const auto& point = points[static_cast<std::size_t>(row)];
            const bool good = point.hasValue && point.value.quality == 1 && !point.value.stale;
            const QColor color = pointColor(point);
            const QString value = point.hasValue ? formatValue(point.value.value) : "-";
            const QString quality = point.hasValue ? QString::number(point.value.quality) : "-";
            const QString time = point.hasValue ? formatTime(point.value.ts) : "-";
            setItem(row, 0, QString::number(point.index), color);
            setItem(row, 1, qs(point.meta.meterCode), QColor("#e8f0f2"));
            setItem(row, 2, qs(point.meta.pointCode), QColor("#e8f0f2"));
            setItem(row, 3, qs(point.meta.name), QColor("#e8f0f2"));
            setItem(row, 4, value, good ? QColor("#16c784") : color);
            setItem(row, 5, qs(point.meta.unit), QColor("#8fa4ad"));
            setItem(row, 6, quality, color);
            setItem(row, 7, time, QColor("#8fa4ad"));
            table_->setRowHeight(row, 38);
        }
    }

    bool pointGood(const DisplayPoint& point) const {
        return point.hasValue && point.value.quality == 1 && !point.value.stale;
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

    QString pointStatusText(const DisplayPoint& point) const {
        if (!point.hasValue) {
            return "missing";
        }
        if (point.value.stale) {
            return "stale";
        }
        return point.value.quality == 1 ? "ok" : "bad";
    }

    QString widgetValueText(const edge_gateway::LocalDisplayWidgetConfig& widget, const DisplayPoint& point) const {
        if (!point.hasValue) {
            return "-";
        }
        if (widget.valueFormat == "boolOnline") {
            return point.value.value == 1.0 ? QString::fromUtf8("在线") : QString::fromUtf8("离线");
        }
        if (widget.valueFormat == "raw") {
            return QString::number(point.value.value, 'g', 12);
        }
        return formatValue(point.value.value);
    }

    QString pointColumnText(const DisplayPoint& point, const std::string& column) const {
        if (column == "index") {
            return QString::number(point.index);
        }
        if (column == "meterCode") {
            return qs(point.meta.meterCode);
        }
        if (column == "pointCode") {
            return qs(point.meta.pointCode);
        }
        if (column == "name") {
            return qs(point.meta.name);
        }
        if (column == "value") {
            return point.hasValue ? formatValue(point.value.value) : "-";
        }
        if (column == "unit") {
            return qs(point.meta.unit);
        }
        if (column == "quality") {
            return point.hasValue ? QString::number(point.value.quality) : "-";
        }
        if (column == "time") {
            return point.hasValue ? formatTime(point.value.ts) : "-";
        }
        if (column == "statusText") {
            return pointStatusText(point);
        }
        return "";
    }

    void refreshLayoutWidgets(const std::vector<DisplayPoint>& points) {
        std::unordered_map<std::uint32_t, const DisplayPoint*> pointByIndex;
        for (const auto& point : points) {
            pointByIndex[point.index] = &point;
        }

        for (auto& widget : runtimeWidgets_) {
            if (widget.valueLabel != nullptr) {
                const auto index = widget.config.pointIndex > 0
                    ? widget.config.pointIndex
                    : (widget.config.pointIndexes.empty() ? 0 : widget.config.pointIndexes.front());
                const auto it = pointByIndex.find(index);
                if (it == pointByIndex.end()) {
                    widget.valueLabel->setText("-");
                    widget.valueLabel->setStyleSheet("color:#ff5c5c;");
                    if (widget.subLabel != nullptr) {
                        widget.subLabel->setText(QString("index %1 missing").arg(index));
                    }
                    continue;
                }
                const auto& point = *it->second;
                widget.valueLabel->setText(widgetValueText(widget.config, point));
                widget.valueLabel->setStyleSheet("color:" + pointColor(point).name() + ";");
                if (widget.subLabel != nullptr) {
                    widget.subLabel->setText(QString("%1 / %2 / %3")
                        .arg(point.meta.meterCode.empty() ? "-" : qs(point.meta.meterCode))
                        .arg(point.meta.pointCode.empty() ? QString::number(point.index) : qs(point.meta.pointCode))
                        .arg(point.hasValue ? formatTime(point.value.ts) : "-"));
                }
            }
            if (widget.table != nullptr) {
                refreshWidgetTable(widget, points, pointByIndex);
            }
        }
    }

    void refreshWidgetTable(
        RuntimeWidget& widget,
        const std::vector<DisplayPoint>& allPoints,
        const std::unordered_map<std::uint32_t, const DisplayPoint*>& pointByIndex
    ) {
        std::vector<const DisplayPoint*> selected;
        if (widget.config.pointIndexes.empty()) {
            for (const auto& point : allPoints) {
                selected.push_back(&point);
            }
        } else {
            for (const auto index : widget.config.pointIndexes) {
                const auto it = pointByIndex.find(index);
                if (it != pointByIndex.end()) {
                    selected.push_back(it->second);
                }
            }
        }
        if (widget.config.type == "alarmSummary") {
            std::vector<const DisplayPoint*> filtered;
            for (const auto* point : selected) {
                if (point != nullptr && !pointGood(*point)) {
                    filtered.push_back(point);
                }
            }
            selected.swap(filtered);
        }
        const auto columns = effectiveColumns(widget.config.columns);
        widget.table->setRowCount(static_cast<int>(selected.size()));
        for (int row = 0; row < static_cast<int>(selected.size()); ++row) {
            const auto* point = selected[static_cast<std::size_t>(row)];
            if (point == nullptr) {
                continue;
            }
            const QColor color = pointColor(*point);
            for (int column = 0; column < static_cast<int>(columns.size()); ++column) {
                const auto& columnName = columns[static_cast<std::size_t>(column)];
                const QColor textColor = columnName == "value" && pointGood(*point) ? QColor("#16c784") : color;
                setTableItem(widget.table, row, column, pointColumnText(*point, columnName), textColor);
            }
            widget.table->setRowHeight(row, 36);
        }
    }

    void setItem(int row, int column, const QString& text, const QColor& color) {
        setTableItem(table_, row, column, text, color);
    }

    void setTableItem(QTableWidget* target, int row, int column, const QString& text, const QColor& color) {
        if (target == nullptr) {
            return;
        }
        auto* item = target->item(row, column);
        if (item == nullptr) {
            item = new QTableWidgetItem();
            target->setItem(row, column, item);
        }
        item->setText(text);
        item->setForeground(color);
    }

    edge_gateway::LocalDisplayConfig config_;
    std::string machineCode_;
    edge_gateway::PointStoreRouter& router_;
    std::unordered_map<std::uint32_t, PointMeta> meta_;
    QLabel* title_ = nullptr;
    QLabel* machineLabel_ = nullptr;
    QLabel* clockLabel_ = nullptr;
    QLabel* pointCount_ = nullptr;
    QLabel* onlineCount_ = nullptr;
    QLabel* badCount_ = nullptr;
    QLabel* refreshLabel_ = nullptr;
    QTableWidget* table_ = nullptr;
    std::vector<RuntimeWidget> runtimeWidgets_;
};

}  // namespace

int main(int argc, char* argv[]) {
    using namespace edge_gateway;

    std::string appConfigPath = "config/runtime/apps/monitor-service.json";
    bool fullscreen = true;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--app-config" && i + 1 < argc) {
            appConfigPath = argv[++i];
        } else if (arg == "--windowed") {
            fullscreen = false;
        }
    }

    auto appConfig = ConfigLoader::loadAppConfigFromFile(appConfigPath);
    setProcessName("modbus-qt-" + sanitizeProcessToken(basenameOf(appConfigPath)));

    DeviceIdentity identity;
    if (!appConfig.identityConfigFile.empty()) {
        identity = ConfigLoader::loadDeviceIdentityFromFile(appConfig.identityConfigFile);
    }

    const auto deviceConfigs = ConfigLoader::loadMany(appConfig.deviceConfigFiles, identity);
    const auto sharedMemoryNames = collectSharedMemoryNames(appConfig, deviceConfigs);

    PointStoreRouter router;
    std::vector<std::unique_ptr<MemoryPointStore>> stores;
    stores.reserve(sharedMemoryNames.size());
    for (const auto& name : sharedMemoryNames) {
        std::unique_ptr<MemoryPointStore> store(new MemoryPointStore(name));
        router.addStore(name, *store);
        stores.push_back(std::move(store));
    }

    const auto fallbackSharedMemory = sharedMemoryNames.empty() ? std::string("gateway_point_store") : sharedMemoryNames.front();
    const auto pointMeta = buildPointMeta(deviceConfigs, fallbackSharedMemory);
    const auto machineCode = resolveMachineCode(identity, deviceConfigs);

    QApplication app(argc, argv);
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    LocalDisplayWindow window(appConfig.localDisplay, machineCode, router, pointMeta);
    if (fullscreen) {
        window.showFullScreen();
    } else {
        window.resize(1280, 720);
        window.show();
    }
    return app.exec();
}
     