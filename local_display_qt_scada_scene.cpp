#include "local_display_qt_scada_scene.hpp"

#include "edge_gateway/scada_project_loader.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <utility>

#ifndef _WIN32
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <QBrush>
#include <QColor>
#include <QDir>
#include <QFont>
#include <QFrame>
#include <QGraphicsEllipseItem>
#include <QGraphicsPathItem>
#include <QGraphicsPixmapItem>
#include <QGraphicsProxyWidget>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <QInputDialog>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>

namespace {

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

QColor colorOr(const std::string& value, const char* fallback) {
    const QColor candidate(QString::fromStdString(value));
    return candidate.isValid() ? candidate : QColor(fallback);
}

QString formatNumber(double value) {
    auto text = QString::number(value, 'f', 3);
    while (text.contains('.') && text.endsWith('0')) text.chop(1);
    if (text.endsWith('.')) text.chop(1);
    return text;
}

bool isProgressType(const std::string& type) {
    return type == "progressBar" || type == "batterySoc" || type == "bilateralProgress";
}

bool isTrendType(const std::string& type) {
    return type == "trend" || type == "realtimeTrend" || type == "lineChart";
}

bool isButtonType(const std::string& type) {
    return type == "qtButton" || type == "button";
}

bool isControlAction(const std::string& type) {
    return type == "writeSetpoint" || type == "pulse" || type == "toggle";
}

bool parseDouble(const std::string& text, double* result) {
    if (result == nullptr || text.empty()) return false;
    char* end = nullptr;
    const auto value = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || end == nullptr || *end != '\0' || !std::isfinite(value)) return false;
    *result = value;
    return true;
}

std::string nextCommandId() {
    static unsigned long sequence = 0;
    std::ostringstream out;
    out << "SCADA_LOCAL_" << nowMs() << "_" << ++sequence;
    return out.str();
}

}  // namespace

ScadaSceneView::ScadaSceneView(
    const edge_gateway::ScadaScreen& screen,
    const std::string& projectRoot,
    const std::vector<edge_gateway::ScadaAlarm>& alarms,
    const std::vector<edge_gateway::ScadaTrend>& trends,
    edge_gateway::ScadaRuntimeMap& runtime,
    std::function<void(const std::string&)> navigate,
    QWidget* parent
) : QGraphicsView(parent),
    screen_(screen),
    projectRoot_(projectRoot),
    alarms_(alarms),
    trends_(trends),
    runtime_(runtime),
    navigate_(std::move(navigate)) {
    scene_ = new QGraphicsScene(this);
    setScene(scene_);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform | QPainter::TextAntialiasing);
    setAlignment(Qt::AlignCenter);
    buildScene();
}

void ScadaSceneView::buildScene() {
    scene_->setSceneRect(0, 0, screen_.width, screen_.height);
    scene_->setBackgroundBrush(QColor("#07151F"));

    if (!screen_.background.empty()) {
        const auto path = QDir(QString::fromStdString(projectRoot_)).filePath(QString::fromStdString(screen_.background));
        QPixmap background(path);
        if (!background.isNull()) {
            auto* item = scene_->addPixmap(background.scaled(
                screen_.width,
                screen_.height,
                Qt::IgnoreAspectRatio,
                Qt::SmoothTransformation
            ));
            item->setZValue(-100000);
        }
    }

    for (const auto& widget : screen_.widgets) {
        if (!widget.visible) continue;
        const QRectF geometry(
            widget.geometry.x,
            widget.geometry.y,
            widget.geometry.width,
            widget.geometry.height
        );
        const auto imageReference = property(widget, "qtImageFile");
        if ((widget.type == "qtImage" || widget.type == "image") && !imageReference.empty()) {
            const auto imagePath = QDir(QString::fromStdString(projectRoot_)).filePath(QString::fromStdString(imageReference));
            QPixmap pixmap(imagePath);
            if (!pixmap.isNull()) {
                auto* image = scene_->addPixmap(pixmap.scaled(
                    static_cast<int>(std::round(geometry.width())),
                    static_cast<int>(std::round(geometry.height())),
                    Qt::IgnoreAspectRatio,
                    Qt::SmoothTransformation
                ));
                image->setPos(geometry.x(), geometry.y());
                image->setZValue(widget.zIndex);
                continue;
            }
        }

        if (isButtonType(widget.type)) {
            auto* button = new QPushButton(QString::fromStdString(
                property(widget, "qtText").empty() ? widget.title : property(widget, "qtText")
            ));
            button->setFixedSize(
                static_cast<int>(std::round(geometry.width())),
                static_cast<int>(std::round(geometry.height()))
            );
            button->setStyleSheet(
                "QPushButton{background:#17384A;color:#F3F8FA;border:1px solid #2C6078;font-size:16px;}"
                "QPushButton:pressed{background:#0D2734;}"
            );
            if (widget.action.type != "none" && !widget.action.type.empty()) {
                const auto action = widget.action;
                QObject::connect(button, &QPushButton::clicked, this, [this, action]() {
                    handleAction(action);
                });
            }
            auto* proxy = scene_->addWidget(button);
            proxy->setPos(geometry.topLeft());
            proxy->setZValue(widget.zIndex);
            continue;
        }

        const auto background = colorOr(property(widget, "qtBackgroundColor"), "#102A38");
        auto* panel = scene_->addRect(geometry, QPen(QColor("#275165"), 1), QBrush(background));
        panel->setZValue(widget.zIndex);

        auto* title = scene_->addText(QString::fromStdString(widget.title));
        title->setDefaultTextColor(QColor("#9BB0BB"));
        title->setFont(QFont(QStringLiteral("Microsoft YaHei"), 11));
        title->setPos(geometry.x() + 12, geometry.y() + 7);
        title->setZValue(widget.zIndex + 0.2);

        RuntimeWidget runtimeWidget;
        runtimeWidget.type = widget.type;
        runtimeWidget.action = widget.action;
        runtimeWidget.panel = panel;
        runtimeWidget.defaultColor = background.name().toStdString();
        runtimeWidget.progressMax = numericProperty(widget, "progressMaxValue", 100.0);
        runtimeWidget.trendMaxPoints = std::max(2, static_cast<int>(numericProperty(widget, "chartMaxPoints", 120.0)));
        for (const auto& binding : widget.bindings) {
            if (binding.nodeId != runtime_.resolver().nodeId()) continue;
            const auto resolved = runtime_.resolver().resolveTag(binding.tagId);
            if (resolved && resolved->mapping.index > 0) {
                runtimeWidget.indexes.push_back(resolved->mapping.index);
            }
        }

        if (isTrendType(widget.type) && runtimeWidget.indexes.empty()) {
            const auto trendId = property(widget, "trendId");
            for (const auto& trend : trends_) {
                if (!trendId.empty() && trend.trendId != trendId) continue;
                runtimeWidget.trendMaxPoints = std::max(2, trend.maxPoints);
                for (const auto& series : trend.series) {
                    if (series.nodeId != runtime_.resolver().nodeId()) continue;
                    const auto resolved = runtime_.resolver().resolveTag(series.tagId);
                    if (resolved && resolved->mapping.index > 0) {
                        runtimeWidget.indexes.push_back(resolved->mapping.index);
                    }
                }
                if (!runtimeWidget.indexes.empty()) break;
            }
        }

        for (const auto& sourceRule : widget.stateRules) {
            RuntimeStateRule rule;
            rule.code = sourceRule.code;
            rule.label = sourceRule.label;
            rule.color = sourceRule.color;
            rule.priority = sourceRule.priority;
            rule.matchAny = sourceRule.match == "any";
            bool complete = true;
            for (const auto& sourceCondition : sourceRule.conditions) {
                if (sourceCondition.nodeId != runtime_.resolver().nodeId()) {
                    complete = false;
                    break;
                }
                const auto resolved = runtime_.resolver().resolveTag(sourceCondition.tagId);
                if (!resolved || resolved->mapping.index == 0) {
                    complete = false;
                    break;
                }
                RuntimeCondition condition;
                condition.index = resolved->mapping.index;
                condition.comparison = sourceCondition.comparison;
                condition.expected = sourceCondition.value;
                rule.conditions.push_back(condition);
            }
            if (complete && !rule.conditions.empty()) runtimeWidget.stateRules.push_back(rule);
        }
        std::sort(runtimeWidget.stateRules.begin(), runtimeWidget.stateRules.end(), [](const RuntimeStateRule& lhs, const RuntimeStateRule& rhs) {
            return lhs.priority > rhs.priority;
        });

        if (widget.type == "alarmTable") {
            const auto selectedAlarmId = property(widget, "alarmId");
            for (const auto& sourceAlarm : alarms_) {
                if (sourceAlarm.nodeId != runtime_.resolver().nodeId()) continue;
                if (!selectedAlarmId.empty() && sourceAlarm.alarmId != selectedAlarmId) continue;
                const auto resolved = runtime_.resolver().resolveTag(sourceAlarm.tagId);
                if (!resolved || resolved->mapping.index == 0) continue;
                RuntimeAlarm alarm;
                alarm.label = sourceAlarm.alarmId;
                alarm.severity = sourceAlarm.severity;
                alarm.condition.index = resolved->mapping.index;
                alarm.condition.comparison = sourceAlarm.comparison;
                alarm.condition.expected = sourceAlarm.threshold;
                runtimeWidget.alarms.push_back(alarm);
                runtimeWidget.indexes.push_back(resolved->mapping.index);
            }
        }

        std::sort(runtimeWidget.indexes.begin(), runtimeWidget.indexes.end());
        runtimeWidget.indexes.erase(
            std::unique(runtimeWidget.indexes.begin(), runtimeWidget.indexes.end()),
            runtimeWidget.indexes.end()
        );

        runtimeWidget.valueText = scene_->addText(runtimeWidget.indexes.empty()
            ? QString::fromStdString(property(widget, "qtText"))
            : QStringLiteral("--"));
        runtimeWidget.valueText->setDefaultTextColor(colorOr(property(widget, "qtTextColor"), "#F3F8FA"));
        runtimeWidget.valueText->setFont(QFont(
            QStringLiteral("Microsoft YaHei"),
            static_cast<int>(numericProperty(widget, "qtFontSize", 18.0)),
            QFont::DemiBold
        ));
        runtimeWidget.valueText->setTextWidth(std::max(20.0, geometry.width() - 24.0));
        runtimeWidget.valueText->setPos(geometry.x() + 12, geometry.y() + std::min(38.0, geometry.height() * 0.42));
        runtimeWidget.valueText->setZValue(widget.zIndex + 0.3);

        if (widget.type == "statusLamp" || widget.type == "statusCard") {
            const auto size = std::max(14.0, std::min(32.0, geometry.height() * 0.24));
            runtimeWidget.statusLamp = scene_->addEllipse(
                geometry.right() - size - 14,
                geometry.y() + 12,
                size,
                size,
                QPen(Qt::NoPen),
                QBrush(QColor("#71808A"))
            );
            runtimeWidget.statusLamp->setZValue(widget.zIndex + 0.4);
        }

        if (isProgressType(widget.type)) {
            runtimeWidget.progressX = geometry.x() + 12;
            runtimeWidget.progressWidth = std::max(1.0, geometry.width() - 24);
            runtimeWidget.progressHeight = std::max(8.0, std::min(20.0, geometry.height() * 0.16));
            runtimeWidget.progressY = geometry.bottom() - runtimeWidget.progressHeight - 12;
            auto* track = scene_->addRect(
                runtimeWidget.progressX,
                runtimeWidget.progressY,
                runtimeWidget.progressWidth,
                runtimeWidget.progressHeight,
                QPen(Qt::NoPen),
                QBrush(QColor("#263D48"))
            );
            track->setZValue(widget.zIndex + 0.2);
            runtimeWidget.progressFill = scene_->addRect(
                runtimeWidget.progressX,
                runtimeWidget.progressY,
                0,
                runtimeWidget.progressHeight,
                QPen(Qt::NoPen),
                QBrush(QColor("#20DBE9"))
            );
            runtimeWidget.progressFill->setZValue(widget.zIndex + 0.3);
        }

        if (isTrendType(widget.type)) {
            runtimeWidget.chartX = geometry.x() + 12;
            runtimeWidget.chartY = geometry.y() + 42;
            runtimeWidget.chartWidth = std::max(1.0, geometry.width() - 24);
            runtimeWidget.chartHeight = std::max(1.0, geometry.height() - 56);
            runtimeWidget.trendPath = scene_->addPath(QPainterPath(), QPen(QColor("#20DBE9"), 2));
            runtimeWidget.trendPath->setZValue(widget.zIndex + 0.4);
            runtimeWidget.valueText->setVisible(false);
        }
        runtimeWidgets_.push_back(runtimeWidget);
    }
    fitInView(scene_->sceneRect(), Qt::KeepAspectRatio);
}

void ScadaSceneView::refresh(std::int64_t now) {
    std::vector<std::uint32_t> requestedIndexes;
    for (const auto& widget : runtimeWidgets_) {
        requestedIndexes.insert(requestedIndexes.end(), widget.indexes.begin(), widget.indexes.end());
        for (const auto& rule : widget.stateRules) {
            for (const auto& condition : rule.conditions) requestedIndexes.push_back(condition.index);
        }
    }
    std::sort(requestedIndexes.begin(), requestedIndexes.end());
    requestedIndexes.erase(std::unique(requestedIndexes.begin(), requestedIndexes.end()), requestedIndexes.end());
    const auto values = runtime_.readIndexes(requestedIndexes, now);
    std::unordered_map<std::uint32_t, edge_gateway::StoredPointValue> byIndex;
    for (const auto& value : values) byIndex[value.index] = value;

    for (auto& widget : runtimeWidgets_) {
        if (widget.type == "alarmTable") {
            QStringList active;
            for (const auto& alarm : widget.alarms) {
                if (conditionMatches(alarm.condition, byIndex)) {
                    active.push_back(QString::fromStdString("[" + alarm.severity + "] " + alarm.label));
                }
            }
            const auto text = active.empty() ? QStringLiteral("No active alarms") : active.join(QStringLiteral("\n"));
            if (text.toStdString() != widget.lastText) {
                widget.valueText->setPlainText(text);
                widget.lastText = text.toStdString();
            }
        } else if (!isTrendType(widget.type) &&
                   !(widget.statusLamp != nullptr && !widget.stateRules.empty())) {
            const auto text = valueText(widget, byIndex);
            const auto utf8 = text.toStdString();
            if (utf8 != widget.lastText) {
                widget.valueText->setPlainText(text);
                widget.lastText = utf8;
            }
        }

        if (widget.indexes.empty()) continue;
        const auto value = byIndex.find(widget.indexes.front());
        const auto good = value != byIndex.end() && value->second.quality == 1 && !value->second.stale;
        if (widget.statusLamp != nullptr && widget.stateRules.empty()) {
            const auto color = !good ? QColor("#71808A")
                : std::fabs(value->second.value) > 0.000001 ? QColor("#20C879") : QColor("#D9A441");
            widget.statusLamp->setBrush(color);
        }
        if (widget.progressFill != nullptr) {
            const auto ratio = !good || widget.progressMax <= 0
                ? 0.0
                : std::max(0.0, std::min(1.0, value->second.value / widget.progressMax));
            widget.progressFill->setRect(
                widget.progressX,
                widget.progressY,
                widget.progressWidth * ratio,
                widget.progressHeight
            );
        }
        refreshState(widget, byIndex);
        refreshTrend(widget, byIndex);
    }
}

void ScadaSceneView::handleAction(const edge_gateway::ScadaWidgetAction& action) {
    if (action.type == "navigate") {
        if (!action.targetScreen.empty()) navigate_(action.targetScreen);
        return;
    }
    if (!isControlAction(action.type)) return;

    const auto resolved = runtime_.resolver().resolveTag(action.tagId);
    if (!resolved || resolved->tag.nodeId != runtime_.resolver().nodeId()) {
        QMessageBox::warning(this, QStringLiteral("SCADA"), QStringLiteral("The control tag is not available on this edge."));
        return;
    }

    double target = 0.0;
    if (action.type == "toggle") {
        const auto current = runtime_.readTag(action.tagId, nowMs());
        if (!current || current->quality != 1 || current->stale) {
            QMessageBox::warning(this, QStringLiteral("SCADA"), QStringLiteral("The current value is unavailable; toggle was rejected."));
            return;
        }
        target = std::fabs(current->value) > 0.000001 ? 0.0 : 1.0;
    } else if (!action.value.empty()) {
        if (!parseDouble(action.value, &target)) {
            QMessageBox::warning(this, QStringLiteral("SCADA"), QStringLiteral("The configured control value is invalid."));
            return;
        }
    } else if (action.type == "pulse") {
        target = 1.0;
    } else {
        bool accepted = false;
        target = QInputDialog::getDouble(
            this,
            QStringLiteral("SCADA setpoint"),
            QStringLiteral("Target value"),
            0.0,
            -1000000000.0,
            1000000000.0,
            3,
            &accepted
        );
        if (!accepted) return;
    }

    if (action.requiresConfirmation) {
        const auto answer = QMessageBox::question(
            this,
            QStringLiteral("Confirm control"),
            QStringLiteral("Write %1 to %2?").arg(target).arg(QString::fromStdString(resolved->tag.displayName)),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No
        );
        if (answer != QMessageBox::Yes) return;
    }

    edge_gateway::PendingWriteCommand command;
    command.cmdId = nextCommandId();
    command.value = target;
    command.source = "scada-local";
    command.ts = nowMs();
    command.acceptedAt = command.ts;
    command.highPriority = action.highPriority;
    const auto result = runtime_.submitWrite(action.tagId, command);
    if (!result.accepted) {
        QMessageBox::warning(this, QStringLiteral("SCADA"), QString::fromStdString(result.message));
        return;
    }

    if (action.type == "pulse") {
        const auto tagId = action.tagId;
        const auto highPriority = action.highPriority;
        QTimer::singleShot(500, this, [this, tagId, highPriority]() {
            edge_gateway::PendingWriteCommand reset;
            reset.cmdId = nextCommandId();
            reset.value = 0.0;
            reset.source = "scada-local-pulse-reset";
            reset.ts = nowMs();
            reset.acceptedAt = reset.ts;
            reset.highPriority = highPriority;
            const auto resetResult = runtime_.submitWrite(tagId, reset);
            if (!resetResult.accepted) {
                QMessageBox::warning(this, QStringLiteral("SCADA"), QString::fromStdString(resetResult.message));
            }
        });
    }
}

bool ScadaSceneView::conditionMatches(
    const RuntimeCondition& condition,
    const std::unordered_map<std::uint32_t, edge_gateway::StoredPointValue>& values
) const {
    const auto current = values.find(condition.index);
    if (current == values.end() || current->second.quality != 1 || current->second.stale) return false;
    double expected = 0.0;
    if (!parseDouble(condition.expected, &expected)) {
        if (condition.expected == "true" || condition.expected == "on") expected = 1.0;
        else if (condition.expected == "false" || condition.expected == "off") expected = 0.0;
        else return false;
    }
    const auto actual = current->second.value;
    if (condition.comparison == "ne") return std::fabs(actual - expected) > 1e-9;
    if (condition.comparison == "gt") return actual > expected;
    if (condition.comparison == "gte") return actual >= expected;
    if (condition.comparison == "lt") return actual < expected;
    if (condition.comparison == "lte") return actual <= expected;
    return std::fabs(actual - expected) <= 1e-9;
}

void ScadaSceneView::refreshState(
    RuntimeWidget& widget,
    const std::unordered_map<std::uint32_t, edge_gateway::StoredPointValue>& values
) {
    if (widget.stateRules.empty()) return;
    const RuntimeStateRule* matched = nullptr;
    for (const auto& rule : widget.stateRules) {
        bool result = rule.matchAny ? false : true;
        for (const auto& condition : rule.conditions) {
            const auto item = conditionMatches(condition, values);
            result = rule.matchAny ? (result || item) : (result && item);
        }
        if (result) {
            matched = &rule;
            break;
        }
    }

    const auto visualCode = matched == nullptr ? std::string("__default") : matched->code;
    if (visualCode == widget.lastVisualCode) return;
    widget.lastVisualCode = visualCode;
    const auto color = matched == nullptr
        ? colorOr(widget.defaultColor, "#102A38")
        : colorOr(matched->color, "#AAB3BD");
    if (widget.panel != nullptr) widget.panel->setBrush(color);
    if (widget.statusLamp != nullptr) widget.statusLamp->setBrush(matched == nullptr ? QColor("#71808A") : color);
    if (widget.type == "statusLamp" || widget.type == "statusCard") {
        const auto label = matched != nullptr && !matched->label.empty() ? matched->label : std::string("--");
        widget.valueText->setPlainText(QString::fromStdString(label));
        widget.lastText = label;
    }
}

void ScadaSceneView::refreshTrend(
    RuntimeWidget& widget,
    const std::unordered_map<std::uint32_t, edge_gateway::StoredPointValue>& values
) {
    if (widget.trendPath == nullptr || widget.indexes.empty()) return;
    const auto current = values.find(widget.indexes.front());
    if (current == values.end() || current->second.quality != 1 || current->second.stale || current->second.ts <= 0) return;
    if (current->second.ts != widget.lastSampleTs) {
        widget.lastSampleTs = current->second.ts;
        widget.trendSamples.push_back(std::make_pair(current->second.ts, current->second.value));
        while (static_cast<int>(widget.trendSamples.size()) > widget.trendMaxPoints) widget.trendSamples.pop_front();
    }
    if (widget.trendSamples.size() < 2) return;

    double minimum = std::numeric_limits<double>::max();
    double maximum = std::numeric_limits<double>::lowest();
    for (const auto& sample : widget.trendSamples) {
        minimum = std::min(minimum, sample.second);
        maximum = std::max(maximum, sample.second);
    }
    if (std::fabs(maximum - minimum) < 1e-9) {
        minimum -= 1.0;
        maximum += 1.0;
    }

    QPainterPath path;
    const auto count = widget.trendSamples.size();
    std::size_t index = 0;
    for (const auto& sample : widget.trendSamples) {
        const auto x = widget.chartX + widget.chartWidth * static_cast<double>(index) / static_cast<double>(count - 1);
        const auto ratio = (sample.second - minimum) / (maximum - minimum);
        const auto y = widget.chartY + widget.chartHeight * (1.0 - ratio);
        if (index == 0) path.moveTo(x, y);
        else path.lineTo(x, y);
        ++index;
    }
    widget.trendPath->setPath(path);
}

void ScadaSceneView::resizeEvent(QResizeEvent* event) {
    QGraphicsView::resizeEvent(event);
    fitInView(scene_->sceneRect(), Qt::KeepAspectRatio);
}

std::string ScadaSceneView::property(const edge_gateway::ScadaWidget& widget, const std::string& key) const {
    const auto item = widget.properties.find(key);
    return item == widget.properties.end() ? std::string() : item->second;
}

double ScadaSceneView::numericProperty(
    const edge_gateway::ScadaWidget& widget,
    const std::string& key,
    double fallback
) const {
    const auto value = property(widget, key);
    double parsed = fallback;
    return parseDouble(value, &parsed) ? parsed : fallback;
}

QString ScadaSceneView::valueText(
    const RuntimeWidget& widget,
    const std::unordered_map<std::uint32_t, edge_gateway::StoredPointValue>& values
) const {
    if (widget.indexes.empty()) return widget.valueText->toPlainText();
    QStringList parts;
    for (const auto index : widget.indexes) {
        const auto value = values.find(index);
        if (value == values.end() || value->second.quality != 1 || value->second.stale) {
            parts.push_back(QStringLiteral("--"));
        } else {
            parts.push_back(formatNumber(value->second.value));
        }
    }
    return parts.join(QStringLiteral("  "));
}

ScadaRuntimeWindow::ScadaRuntimeWindow(
    const std::string& projectDirectory,
    const std::string& machineCode,
    edge_gateway::PointStoreRouter& router,
    int refreshIntervalMs,
    bool autoReload,
    QWidget* parent
) : QWidget(parent),
    projectDirectory_(projectDirectory),
    machineCode_(machineCode),
    router_(router),
    autoReload_(autoReload) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    stack_ = new QStackedWidget(this);
    layout->addWidget(stack_);

    reloadProject(true);
    timer_ = new QTimer(this);
    QObject::connect(timer_, &QTimer::timeout, this, [this]() {
        const auto now = nowMs();
        if (autoReload_ && now - lastReloadCheckMs_ >= 1000) {
            lastReloadCheckMs_ = now;
            const auto revision = projectRevision();
            if (!revision.empty() && revision != loadedRevision_) {
                try {
                    reloadProject(false);
                } catch (const std::exception& ex) {
                    std::cerr << "SCADA auto reload failed: " << ex.what() << std::endl;
                }
            }
        }
        refreshCurrent();
    });
    timer_->start(std::max(100, refreshIntervalMs));
    refreshCurrent();
}

void ScadaRuntimeWindow::reloadProject(bool initial) {
    auto loaded = edge_gateway::ScadaProjectLoader::loadFromDirectory(projectDirectory_);
    if (loaded.screens.empty()) throw std::runtime_error("SCADA project has no screens for local runtime");
    project_ = std::move(loaded);
    runtime_.reset(new edge_gateway::ScadaRuntimeMap(project_, machineCode_, router_));
    setWindowTitle(QString::fromStdString(project_.manifest.projectName));
    rebuildScreens();
    loadedRevision_ = projectRevision();
    if (!initial) {
        std::cout << "SCADA project reloaded revision=" << loadedRevision_ << std::endl;
    }
}

void ScadaRuntimeWindow::rebuildScreens() {
    while (stack_->count() > 0) {
        auto* widget = stack_->widget(0);
        stack_->removeWidget(widget);
        delete widget;
    }
    views_.clear();
    screenIndexes_.clear();
    for (const auto& screen : project_.screens) {
        auto* view = new ScadaSceneView(
            screen,
            project_.rootDirectory,
            project_.alarms,
            project_.trends,
            *runtime_,
            [this](const std::string& target) { showScreen(target); },
            stack_
        );
        screenIndexes_[screen.screenId] = stack_->addWidget(view);
        views_.push_back(view);
    }
    showScreen(project_.manifest.entryScreen);
}

void ScadaRuntimeWindow::showScreen(const std::string& screenId) {
    const auto item = screenIndexes_.find(screenId);
    if (item != screenIndexes_.end()) {
        stack_->setCurrentIndex(item->second);
        refreshCurrent();
    } else if (stack_->count() > 0) {
        stack_->setCurrentIndex(0);
    }
}

void ScadaRuntimeWindow::refreshCurrent() {
    auto* view = dynamic_cast<ScadaSceneView*>(stack_->currentWidget());
    if (view != nullptr) view->refresh(nowMs());
}

std::string ScadaRuntimeWindow::projectRevision() const {
#ifdef _WIN32
    return projectDirectory_;
#else
    char resolved[PATH_MAX] = {};
    if (realpath(projectDirectory_.c_str(), resolved) == nullptr) return std::string();
    std::ostringstream revision;
    revision << resolved;
    const char* files[] = {"manifest.json", "checksums.json", "runtime-map.json"};
    for (const auto* file : files) {
        struct stat info {};
        const auto path = std::string(resolved) + "/" + file;
        if (stat(path.c_str(), &info) != 0) return std::string();
        revision << ':' << static_cast<long long>(info.st_mtime) << ':' << static_cast<long long>(info.st_size);
    }
    return revision.str();
#endif
}
