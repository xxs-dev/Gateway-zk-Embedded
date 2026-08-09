#include "local_display_qt_scada_scene.hpp"

#include "edge_gateway/scada_project_loader.hpp"
#include "local_display_qt_pcs_power_control.hpp"

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
#include <QApplication>
#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QEvent>
#include <QGraphicsEllipseItem>
#include <QGraphicsLineItem>
#include <QGraphicsPathItem>
#include <QGraphicsPixmapItem>
#include <QGraphicsProxyWidget>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QStringList>
#include <QTimer>
#include <QTime>
#include <QTextDocument>
#include <QTextOption>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace {

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

std::int64_t localDayStartMs(std::int64_t timestampMs) {
    auto local = QDateTime::fromMSecsSinceEpoch(timestampMs);
    local.setTime(QTime(0, 0));
    return local.toMSecsSinceEpoch();
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

Qt::PenStyle chartPenStyle(const QString& value) {
    if (value.compare(QStringLiteral("DashLine"), Qt::CaseInsensitive) == 0) return Qt::DashLine;
    if (value.compare(QStringLiteral("DotLine"), Qt::CaseInsensitive) == 0) return Qt::DotLine;
    if (value.compare(QStringLiteral("DashDotLine"), Qt::CaseInsensitive) == 0) return Qt::DashDotLine;
    return Qt::SolidLine;
}

QPoint wheelViewportPosition(const QWheelEvent& event) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event.position().toPoint();
#else
    return event.pos();
#endif
}

bool isProgressType(const std::string& type) {
    return type == "progressBar" || type == "batterySoc" || type == "bilateralProgress" || type == "qtFillProgress";
}

bool isTrendType(const std::string& type) {
    return type == "trend" || type == "realtimeTrend" || type == "lineChart" || type == "qtChart";
}

bool isButtonType(const std::string& type) {
    return type == "qtButton" || type == "button";
}

bool isQtNativeTextType(const std::string& type) {
    return type == "qtLabel" || type == "qtValue" || type == "qtFrame" || type == "qtInput" || type == "qtCheckBox";
}

bool isQtNativeVisualType(const std::string& type) {
    return type.size() >= 2 && type[0] == 'q' && type[1] == 't';
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
    ScadaSceneRuntimeSource& runtime,
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
    setMouseTracking(true);
    viewport()->setMouseTracking(true);
    buildScene();
    const auto hasEnergyFlow = std::any_of(runtimeWidgets_.begin(), runtimeWidgets_.end(), [](const auto& widget) {
        return widget.type == "energyFlow";
    });
    if (hasEnergyFlow) {
        flowAnimationTimer_ = new QTimer(this);
        flowAnimationTimer_->setInterval(80);
        QObject::connect(flowAnimationTimer_, &QTimer::timeout, this, [this]() {
            const auto timestamp = nowMs();
            for (auto& widget : runtimeWidgets_) {
                if (widget.type == "energyFlow") animateEnergyFlow(widget, timestamp);
            }
        });
        flowAnimationTimer_->start();
    }
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
        const auto imageOverlayText = property(widget, "qtText");
        const auto imageHasTextOverlay =
            (widget.type == "qtImage" || widget.type == "image") && !imageOverlayText.empty();

        if (widget.type == "pcsPhasePowerControl") {
            PcsPhasePowerBindings bindings;
            for (const auto& binding : widget.bindings) {
                if (binding.nodeId != runtime_.nodeId()) continue;
                if (binding.slot == "activeA") bindings.activeTagIds[0] = binding.tagId;
                else if (binding.slot == "activeB") bindings.activeTagIds[1] = binding.tagId;
                else if (binding.slot == "activeC") bindings.activeTagIds[2] = binding.tagId;
                else if (binding.slot == "reactiveA") bindings.reactiveTagIds[0] = binding.tagId;
                else if (binding.slot == "reactiveB") bindings.reactiveTagIds[1] = binding.tagId;
                else if (binding.slot == "reactiveC") bindings.reactiveTagIds[2] = binding.tagId;
            }

            auto* control = new PcsPhasePowerControl(std::move(bindings), runtime_);
            control->setFixedSize(
                static_cast<int>(std::round(geometry.width())),
                static_cast<int>(std::round(geometry.height()))
            );
            auto* proxy = scene_->addWidget(control);
            proxy->setPos(geometry.topLeft());
            proxy->setZValue(widget.zIndex);

            RuntimeWidget runtimeWidget;
            runtimeWidget.type = widget.type;
            runtimeWidget.pcsPowerControl = control;
            runtimeWidgets_.push_back(std::move(runtimeWidget));
            continue;
        }

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
                if (!imageHasTextOverlay && widget.bindings.empty()) continue;
            }
        }

        if (isButtonType(widget.type)) {
            auto* button = new QPushButton(QString::fromStdString(
                property(widget, "qtText").empty() ? widget.title : property(widget, "qtText")
            ));
            button->setObjectName(QString::fromStdString(widget.widgetId));
            button->setFixedSize(
                static_cast<int>(std::round(geometry.width())),
                static_cast<int>(std::round(geometry.height()))
            );
            const auto transparent = property(widget, "qtTransparent") == "true";
            const auto textColor = property(widget, "qtTextColor").empty() ? "#F3F8FA" : property(widget, "qtTextColor");
            const auto backgroundColor = property(widget, "qtBackgroundColor").empty() ? "#17384A" : property(widget, "qtBackgroundColor");
            QString style = QStringLiteral("QPushButton{color:%1;border:%2;background:%3;font-size:%4px;}")
                .arg(QString::fromStdString(textColor))
                .arg(transparent ? QStringLiteral("none") : QStringLiteral("1px solid #2C6078"))
                .arg(transparent ? QStringLiteral("transparent") : QString::fromStdString(backgroundColor))
                .arg(static_cast<int>(numericProperty(widget, "qtFontSize", 16.0)));
            if (!imageReference.empty()) {
                const auto imagePath = QDir(QString::fromStdString(projectRoot_)).filePath(QString::fromStdString(imageReference));
                if (QFileInfo::exists(imagePath)) {
                    style += QStringLiteral("QPushButton{border-image:url(\"%1\");}").arg(imagePath);
                }
            }
            style += QStringLiteral("QPushButton:pressed{background:#0D2734;}");
            bool actionAvailable = widget.action.type == "navigate" && !widget.action.targetScreen.empty();
            if (isControlAction(widget.action.type)) {
                const auto resolved = runtime_.resolveTag(widget.action.tagId);
                actionAvailable = resolved && resolved->tag.nodeId == runtime_.nodeId() &&
                    resolved->mapping.index > 0 && resolved->mapping.writable;
            }
            if (!actionAvailable) {
                style += QStringLiteral(
                    "QPushButton:disabled{color:#80919A;border:1px dashed #52636C;background:#17242B;}"
                );
                button->setEnabled(false);
                button->setToolTip(QString::fromUtf8("未配置可执行动作"));
            }
            button->setStyleSheet(style);
            if (actionAvailable) {
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

        const auto qtNativeText = isQtNativeTextType(widget.type) || imageHasTextOverlay;
        const auto legacyQtWidget = !property(widget, "qtClass").empty();
        const auto qtNativeVisual = isQtNativeVisualType(widget.type) || legacyQtWidget;
        const auto qtFillProgress = widget.type == "qtFillProgress";
        const auto legacyQtProgress = isProgressType(widget.type) && legacyQtWidget;
        const auto alarmTable = widget.type == "alarmTable";
        const auto energyFlow = widget.type == "energyFlow";
        const auto borderless = energyFlow || property(widget, "qtBorderless") == "true";
        const auto configuredBackground = property(widget, "qtBackgroundColor");
        const auto inputNeedsNativeBackground = widget.type == "qtInput" &&
            (configuredBackground.empty() ||
             QString::fromStdString(configuredBackground).compare(QStringLiteral("transparent"), Qt::CaseInsensitive) == 0);
        const auto background = colorOr(
            inputNeedsNativeBackground ? "#D7E6F2" : configuredBackground,
            qtNativeVisual ? "transparent" : "#102A38"
        );
        auto* panel = scene_->addRect(
            geometry,
            alarmTable
                ? QPen(QColor("#2C6078"), 1)
                : borderless
                    ? QPen(Qt::NoPen)
                : widget.type == "qtInput"
                    ? QPen(QColor("#6F8FA7"), 1)
                : qtNativeVisual
                    ? QPen(Qt::NoPen)
                    : QPen(QColor("#275165"), 1),
            (qtFillProgress || legacyQtProgress || energyFlow)
                ? QBrush(Qt::NoBrush)
                : alarmTable
                    ? QBrush(QColor("#071A2D"))
                    : QBrush(background)
        );
        panel->setZValue(widget.zIndex);

        if (!qtNativeVisual && !energyFlow) {
            auto* title = scene_->addText(QString::fromStdString(widget.title));
            title->setDefaultTextColor(QColor("#9BB0BB"));
            QFont titleFont(QStringLiteral("Microsoft YaHei"), 11);
            if (widget.type == "cellularSignal") {
                titleFont.setPixelSize(20);
            }
            title->setFont(titleFont);
            title->setPos(geometry.x() + 12, geometry.y() + 7);
            title->setZValue(widget.zIndex + 0.2);
        }

        RuntimeWidget runtimeWidget;
        runtimeWidget.type = widget.type;
        runtimeWidget.action = widget.action;
        runtimeWidget.panel = panel;
        runtimeWidget.defaultLabel = property(widget, "defaultStateLabel");
        const auto configuredDefaultColor = QColor(QString::fromStdString(property(widget, "defaultStateColor")));
        runtimeWidget.defaultColor = configuredDefaultColor.isValid()
            ? configuredDefaultColor.name().toStdString()
            : background.name().toStdString();
        runtimeWidget.defaultImage = property(widget, "defaultStateImage").empty()
            ? imageReference
            : property(widget, "defaultStateImage");
        runtimeWidget.valueSuffix = property(widget, "valueSuffix");
        runtimeWidget.stateLabelVisible = property(widget, "stateLabelVisible") != "false";
        runtimeWidget.stateColorPanel = property(widget, "stateColorMode") != "indicator";
        runtimeWidget.progressMax = numericProperty(widget, "progressMaxValue", 100.0);
        const auto legacySampleIntervalSeconds = numericProperty(widget, "chartSampleIntervalSec", 30.0);
        runtimeWidget.chartSampleIntervalMs = std::max<std::int64_t>(
            1000,
            static_cast<std::int64_t>(
                numericProperty(widget, "chartSampleIntervalSeconds", legacySampleIntervalSeconds) * 1000.0
            )
        );
        const auto fullDayPointCapacity = static_cast<int>(
            (24LL * 60 * 60 * 1000 + runtimeWidget.chartSampleIntervalMs - 1) /
            runtimeWidget.chartSampleIntervalMs
        ) + 2;
        runtimeWidget.trendMaxPoints = std::max(
            fullDayPointCapacity,
            std::max(2, static_cast<int>(numericProperty(widget, "chartMaxPoints", 120.0)))
        );
        runtimeWidget.chartDefaultWindowMs = std::max<std::int64_t>(
            60 * 1000,
            static_cast<std::int64_t>(numericProperty(widget, "chartDefaultWindowMinutes", 120.0) * 60.0 * 1000.0)
        );
        runtimeWidget.chartViewWindowMs = runtimeWidget.chartDefaultWindowMs;
        runtimeWidget.chartMinWindowMs = std::max<std::int64_t>(
            60 * 1000,
            static_cast<std::int64_t>(numericProperty(widget, "chartMinWindowMinutes", 5.0) * 60.0 * 1000.0)
        );
        runtimeWidget.chartMinWindowMs = std::min(
            runtimeWidget.chartMinWindowMs,
            runtimeWidget.chartDefaultWindowMs
        );
        runtimeWidget.valueMap = parseScadaValueMapJson(property(widget, "valueMapJson"));
        for (const auto& binding : widget.bindings) {
            if (binding.nodeId != runtime_.nodeId()) continue;
            const auto resolved = runtime_.resolveTag(binding.tagId);
            if (resolved && resolved->mapping.index > 0) {
                runtimeWidget.indexes.push_back(resolved->mapping.index);
                if (widget.type == "qtInput" && runtimeWidget.inputTagId.empty()) {
                    runtimeWidget.inputTagId = binding.tagId;
                    runtimeWidget.inputWritable = resolved->mapping.writable;
                    runtimeWidget.inputMinValue = resolved->writeMinValue;
                    runtimeWidget.inputMaxValue = resolved->writeMaxValue;
                    runtimeWidget.inputStep = resolved->writeStep;
                }
            }
        }

        if (widget.type == "qtInput" && !widget.action.tagId.empty()) {
            const auto resolved = runtime_.resolveTag(widget.action.tagId);
            if (resolved && resolved->mapping.index > 0) {
                runtimeWidget.inputTagId = widget.action.tagId;
                runtimeWidget.inputWritable = resolved->mapping.writable;
                runtimeWidget.inputMinValue = resolved->writeMinValue;
                runtimeWidget.inputMaxValue = resolved->writeMaxValue;
                runtimeWidget.inputStep = resolved->writeStep;
            }
        }

        if (isTrendType(widget.type) && runtimeWidget.indexes.empty()) {
            const auto trendId = property(widget, "trendId");
            for (const auto& trend : trends_) {
                if (!trendId.empty() && trend.trendId != trendId) continue;
                runtimeWidget.trendMaxPoints = std::max(fullDayPointCapacity, std::max(2, trend.maxPoints));
                for (const auto& series : trend.series) {
                    if (series.nodeId != runtime_.nodeId()) continue;
                    const auto resolved = runtime_.resolveTag(series.tagId);
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
            rule.image = sourceRule.image;
            rule.priority = sourceRule.priority;
            rule.matchAny = sourceRule.match == "any";
            bool complete = true;
            for (const auto& sourceCondition : sourceRule.conditions) {
                if (sourceCondition.nodeId != runtime_.nodeId()) {
                    complete = false;
                    break;
                }
                const auto resolved = runtime_.resolveTag(sourceCondition.tagId);
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
                if (sourceAlarm.nodeId != runtime_.nodeId()) continue;
                if (!selectedAlarmId.empty() && sourceAlarm.alarmId != selectedAlarmId) continue;
                const auto resolved = runtime_.resolveTag(sourceAlarm.tagId);
                if (!resolved || resolved->mapping.index == 0) continue;
                RuntimeAlarm alarm;
                alarm.label = resolved->tag.displayName.empty()
                    ? sourceAlarm.alarmId
                    : resolved->tag.displayName;
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

        const auto hideNativeValue = legacyQtProgress || widget.type == "qtChart" || energyFlow;
        const auto unboundDataWidget = runtimeWidget.indexes.empty() &&
            (widget.type == "qtValue" || widget.type == "qtInput");
        const auto unboundStateWidget = runtimeWidget.indexes.empty() &&
            runtimeWidget.stateRules.empty() &&
            (widget.type == "statusLamp" || widget.type == "statusCard");
        runtimeWidget.valueText = scene_->addText(hideNativeValue
            ? QString()
            : unboundDataWidget
                ? QStringLiteral("--")
            : unboundStateWidget
                ? QString::fromUtf8("未知")
            : runtimeWidget.indexes.empty()
                ? QString::fromStdString(property(widget, "qtText"))
                : QStringLiteral("--"));
        runtimeWidget.valueText->setDefaultTextColor(alarmTable
            ? QColor("#E8F0F2")
            : colorOr(property(widget, "qtTextColor"), "#F3F8FA"));
        QFont valueFont(QStringLiteral("Microsoft YaHei"));
        valueFont.setPixelSize(std::max(1, static_cast<int>(std::round(
            alarmTable ? 16.0 : numericProperty(widget, "qtFontSize", qtNativeText ? 14.0 : 18.0)
        ))));
        valueFont.setWeight(property(widget, "qtFontWeight") == "Bold" ? QFont::Bold : QFont::Normal);
        runtimeWidget.valueText->setFont(valueFont);
        runtimeWidget.valueText->document()->setDocumentMargin(0);
        QTextOption textOption = runtimeWidget.valueText->document()->defaultTextOption();
        textOption.setWrapMode(QTextOption::NoWrap);
        const auto textAlignment = property(widget, "qtTextAlignment");
        textOption.setAlignment(textAlignment == "Center"
            ? Qt::AlignHCenter
            : textAlignment == "Right"
                ? Qt::AlignRight
                : Qt::AlignLeft);
        runtimeWidget.valueText->document()->setDefaultTextOption(textOption);
        runtimeWidget.valueText->setTextWidth(std::max(
            1.0,
            geometry.width() - (alarmTable ? 36.0 : qtNativeText ? 0.0 : 24.0)
        ));
        auto textX = geometry.x() + (alarmTable ? 18.0 : qtNativeText ? 0.0 : 12.0);
        auto textY = geometry.y() + (alarmTable ? 44.0 : qtNativeText ? 0.0 : std::min(38.0, geometry.height() * 0.42));
        const auto textHeight = runtimeWidget.valueText->boundingRect().height();
        const auto verticalAlignment = property(widget, "qtVerticalAlignment");
        if (verticalAlignment == "Center") {
            textY = geometry.y() + std::max(0.0, (geometry.height() - textHeight) / 2.0);
        } else if (verticalAlignment == "Bottom") {
            textY = geometry.bottom() - textHeight;
        }
        runtimeWidget.valueText->setPos(textX, textY);
        runtimeWidget.valueText->setZValue(widget.zIndex + 0.3);
        runtimeWidget.valueText->setVisible(!hideNativeValue);
        if (widget.type == "cellularSignal") {
            runtimeWidget.valueText->setTextWidth(std::max(1.0, geometry.width() - 150.0));
            runtimeWidget.valueText->setPos(geometry.x() + 18.0, geometry.y() + 56.0);
            const auto baseline = geometry.bottom() - 20.0;
            const auto barWidth = 18.0;
            const auto gap = 9.0;
            const auto startX = geometry.right() - 4.0 * barWidth - 3.0 * gap - 20.0;
            for (int i = 0; i < 4; ++i) {
                const auto barHeight = 18.0 + static_cast<double>(i) * 14.0;
                auto* bar = scene_->addRect(
                    startX + static_cast<double>(i) * (barWidth + gap),
                    baseline - barHeight,
                    barWidth,
                    barHeight,
                    QPen(Qt::NoPen),
                    QBrush(QColor("#35505D"))
                );
                bar->setZValue(widget.zIndex + 0.4);
                runtimeWidget.signalBars.push_back(bar);
            }
        }
        if (energyFlow) {
            runtimeWidget.flowForwardWhenPositive = property(widget, "flowForwardWhen") != "negative";
            runtimeWidget.flowDeadband = std::max(0.0, numericProperty(widget, "flowDeadband", 0.2));
            runtimeWidget.flowRatedPower = std::max(0.001, numericProperty(widget, "flowRatedPower", 30.0));
            runtimeWidget.flowColor = property(widget, "flowColor");
            runtimeWidget.flowIdleColor = property(widget, "flowIdleColor");
            runtimeWidget.flowStart = QPointF(geometry.left() + 5.0, geometry.y() + geometry.height() * 0.72);
            runtimeWidget.flowEnd = QPointF(geometry.right() - 5.0, geometry.y() + geometry.height() * 0.72);

            runtimeWidget.flowLine = scene_->addLine(
                QLineF(runtimeWidget.flowStart, runtimeWidget.flowEnd),
                QPen(colorOr(runtimeWidget.flowIdleColor, "#2C6078"), 3, Qt::DashLine, Qt::RoundCap)
            );
            runtimeWidget.flowLine->setZValue(widget.zIndex + 0.25);

            runtimeWidget.flowArrow = scene_->addPath(QPainterPath());
            runtimeWidget.flowArrow->setZValue(widget.zIndex + 0.45);
            runtimeWidget.flowArrow->setVisible(false);

            runtimeWidget.flowValueText = scene_->addText(QStringLiteral("-- kW"));
            QFont flowFont(QStringLiteral("Microsoft YaHei"));
            flowFont.setPixelSize(18);
            flowFont.setWeight(QFont::Bold);
            runtimeWidget.flowValueText->setFont(flowFont);
            runtimeWidget.flowValueText->setDefaultTextColor(QColor("#D8EEF4"));
            runtimeWidget.flowValueText->document()->setDocumentMargin(0);
            auto flowTextOption = runtimeWidget.flowValueText->document()->defaultTextOption();
            flowTextOption.setAlignment(Qt::AlignHCenter);
            flowTextOption.setWrapMode(QTextOption::NoWrap);
            runtimeWidget.flowValueText->document()->setDefaultTextOption(flowTextOption);
            runtimeWidget.flowValueText->setTextWidth(geometry.width());
            runtimeWidget.flowValueText->setPos(geometry.x(), geometry.y() + 1.0);
            runtimeWidget.flowValueText->setZValue(widget.zIndex + 0.5);

            const auto particleColor = colorOr(runtimeWidget.flowColor, "#20DBE9");
            const auto particleCount = std::max(2, static_cast<int>(numericProperty(widget, "flowParticleCount", 4.0)));
            for (int i = 0; i < particleCount; ++i) {
                auto* particle = scene_->addEllipse(-4, -4, 8, 8, QPen(Qt::NoPen), QBrush(particleColor));
                particle->setZValue(widget.zIndex + 0.55);
                particle->setVisible(false);
                runtimeWidget.flowParticles.push_back(particle);
            }
        }
        if (widget.type == "qtInput" && runtimeWidget.inputWritable) {
            runtimeWidget.panel->setCursor(Qt::IBeamCursor);
        }

        if (widget.type == "statusLamp" || widget.type == "statusCard") {
            const auto imagePath = imageReference.empty()
                ? QString()
                : QDir(QString::fromStdString(projectRoot_)).filePath(QString::fromStdString(imageReference));
            QPixmap pixmap(imagePath);
            const auto hasStateSource = !runtimeWidget.indexes.empty() || !runtimeWidget.stateRules.empty();
            if (hasStateSource && !pixmap.isNull()) {
                runtimeWidget.stateImage = scene_->addPixmap(pixmap.scaled(
                    static_cast<int>(std::round(geometry.width())),
                    static_cast<int>(std::round(geometry.height())),
                    Qt::IgnoreAspectRatio,
                    Qt::SmoothTransformation
                ));
                runtimeWidget.stateImage->setPos(geometry.topLeft());
                runtimeWidget.stateImage->setZValue(widget.zIndex + 0.4);
                runtimeWidget.panel->setBrush(Qt::NoBrush);
                runtimeWidget.valueText->setVisible(false);
            } else {
                const auto size = std::max(14.0, std::min(32.0, geometry.height() * 0.24));
                runtimeWidget.statusLamp = scene_->addEllipse(
                    geometry.right() - size - 14,
                    geometry.y() + std::max(0.0, (geometry.height() - size) / 2.0),
                    size,
                    size,
                    QPen(Qt::NoPen),
                    QBrush(QColor("#71808A"))
                );
                runtimeWidget.statusLamp->setZValue(widget.zIndex + 0.4);
                if (property(widget, "qtTextAlignment") == "Center") {
                    runtimeWidget.valueText->setTextWidth(std::max(1.0, geometry.width() - size - 34.0));
                }
            }
        }

        if (isProgressType(widget.type)) {
            const auto progressOrientation = property(widget, "progressOrientation");
            const auto configuredMaxWidth = numericProperty(widget, "progressMaxWidth", 0.0);
            const auto configuredMaxHeight = numericProperty(widget, "progressMaxHeight", 0.0);
            runtimeWidget.progressVertical = progressOrientation == "vertical" ||
                (progressOrientation.empty() && configuredMaxHeight > 0.0 && configuredMaxWidth <= 0.0);
            runtimeWidget.progressWidth = legacyQtProgress
                ? std::max(1.0, configuredMaxWidth > 0.0 ? configuredMaxWidth : geometry.width())
                : std::max(1.0, geometry.width() - 24);
            runtimeWidget.progressHeight = legacyQtProgress
                ? std::max(1.0, configuredMaxHeight > 0.0 ? configuredMaxHeight : geometry.height())
                : std::max(8.0, std::min(20.0, geometry.height() * 0.16));
            runtimeWidget.progressX = legacyQtProgress ? geometry.x() : geometry.x() + 12;
            runtimeWidget.progressY = legacyQtProgress
                ? (runtimeWidget.progressVertical
                    ? geometry.bottom() - runtimeWidget.progressHeight
                    : geometry.y())
                : geometry.bottom() - runtimeWidget.progressHeight - 12;
            auto* track = scene_->addRect(
                runtimeWidget.progressX,
                runtimeWidget.progressY,
                runtimeWidget.progressWidth,
                runtimeWidget.progressHeight,
                QPen(Qt::NoPen),
                qtFillProgress ? QBrush(Qt::NoBrush) : QBrush(colorOr(property(widget, "qtBackgroundColor"), "#263D48"))
            );
            track->setZValue(widget.zIndex + 0.2);
            const auto fillColor = qtFillProgress
                ? colorOr(property(widget, "qtBackgroundColor"), "#20DBE9")
                : QColor("#20DBE9");
            runtimeWidget.progressFill = scene_->addRect(
                runtimeWidget.progressX,
                runtimeWidget.progressVertical
                    ? runtimeWidget.progressY + runtimeWidget.progressHeight
                    : runtimeWidget.progressY,
                runtimeWidget.progressVertical ? runtimeWidget.progressWidth : 0,
                runtimeWidget.progressVertical ? 0 : runtimeWidget.progressHeight,
                QPen(Qt::NoPen),
                QBrush(fillColor)
            );
            runtimeWidget.progressFill->setZValue(widget.zIndex + 0.3);
        }

        if (isTrendType(widget.type)) {
            const auto legacyChart = widget.type == "qtChart";
            runtimeWidget.chartX = geometry.x() + (legacyChart ? 58 : 44);
            runtimeWidget.chartY = geometry.y() + (legacyChart ? 30 : 42);
            runtimeWidget.chartWidth = std::max(
                1.0,
                geometry.width() * (legacyChart ? 0.76 : 1.0) - (legacyChart ? 82 : 58)
            );
            runtimeWidget.chartHeight = std::max(1.0, geometry.height() - (legacyChart ? 70 : 66));
            QPen gridPen(QColor("#1C506B"), 1, Qt::DashLine);
            for (int line = 0; line <= 10; ++line) {
                const auto x = runtimeWidget.chartX + runtimeWidget.chartWidth * static_cast<double>(line) / 10.0;
                auto* grid = scene_->addLine(x, runtimeWidget.chartY, x, runtimeWidget.chartY + runtimeWidget.chartHeight, gridPen);
                grid->setZValue(widget.zIndex + 0.1);
            }
            for (int line = 0; line <= 5; ++line) {
                const auto y = runtimeWidget.chartY + runtimeWidget.chartHeight * static_cast<double>(line) / 5.0;
                auto* grid = scene_->addLine(runtimeWidget.chartX, y, runtimeWidget.chartX + runtimeWidget.chartWidth, y, gridPen);
                grid->setZValue(widget.zIndex + 0.1);
            }
            auto* border = scene_->addRect(
                runtimeWidget.chartX,
                runtimeWidget.chartY,
                runtimeWidget.chartWidth,
                runtimeWidget.chartHeight,
                QPen(QColor("#347792"), 1),
                QBrush(Qt::NoBrush)
            );
            border->setZValue(widget.zIndex + 0.15);

            const QFont axisFont(QStringLiteral("Microsoft YaHei"), 9);
            for (int line = 0; line <= 5; ++line) {
                auto* label = scene_->addText(QStringLiteral("--"), axisFont);
                label->setDefaultTextColor(QColor("#8FB0BF"));
                label->setTextWidth(50);
                auto option = label->document()->defaultTextOption();
                option.setAlignment(Qt::AlignRight);
                label->document()->setDefaultTextOption(option);
                label->document()->setDocumentMargin(0);
                label->setPos(
                    runtimeWidget.chartX - 54,
                    runtimeWidget.chartY + runtimeWidget.chartHeight * static_cast<double>(line) / 5.0 - 9
                );
                label->setZValue(widget.zIndex + 0.3);
                runtimeWidget.chartYLabels.push_back(label);
            }
            for (int line = 0; line <= 4; ++line) {
                auto* label = scene_->addText(QStringLiteral("--:--:--"), axisFont);
                label->setDefaultTextColor(QColor("#8FB0BF"));
                label->setTextWidth(64);
                auto option = label->document()->defaultTextOption();
                option.setAlignment(Qt::AlignHCenter);
                label->document()->setDefaultTextOption(option);
                label->document()->setDocumentMargin(0);
                label->setPos(
                    runtimeWidget.chartX + runtimeWidget.chartWidth * static_cast<double>(line) / 4.0 - 32,
                    runtimeWidget.chartY + runtimeWidget.chartHeight + 5
                );
                label->setZValue(widget.zIndex + 0.3);
                runtimeWidget.chartXLabels.push_back(label);
            }
            std::unordered_map<std::uint32_t, QColor> seriesColors;
            std::unordered_map<std::uint32_t, QString> seriesNames;
            std::unordered_map<std::uint32_t, QString> seriesUnits;
            std::unordered_map<std::uint32_t, Qt::PenStyle> seriesPenStyles;
            const auto seriesDocument = QJsonDocument::fromJson(
                QString::fromStdString(property(widget, "chartSeriesJson")).toUtf8()
            );
            if (seriesDocument.isArray()) {
                for (const auto& item : seriesDocument.array()) {
                    const auto object = item.toObject();
                    const auto index = static_cast<std::uint32_t>(object.value(QStringLiteral("index")).toInt());
                    const auto color = QColor(object.value(QStringLiteral("color")).toString());
                    if (index > 0 && color.isValid()) seriesColors[index] = color;
                    if (index > 0) {
                        seriesNames[index] = object.value(QStringLiteral("name")).toString();
                        seriesUnits[index] = object.value(QStringLiteral("unit")).toString();
                        seriesPenStyles[index] = chartPenStyle(object.value(QStringLiteral("penStyle")).toString());
                    }
                }
            }
            static const char* fallbackColors[] = {"#F9CC44", "#129A37", "#EF5350", "#FF9626", "#20DBE9", "#CB2B88"};
            std::size_t seriesIndex = 0;
            for (const auto index : runtimeWidget.indexes) {
                RuntimeTrendSeries series;
                series.index = index;
                const auto colorIt = seriesColors.find(index);
                const auto color = colorIt == seriesColors.end()
                    ? QColor(fallbackColors[seriesIndex % 6])
                    : colorIt->second;
                series.name = seriesNames[index].isEmpty()
                    ? std::string("Index ") + std::to_string(index)
                    : seriesNames[index].toStdString();
                series.unit = seriesUnits[index].toStdString();
                const auto styleIt = seriesPenStyles.find(index);
                series.path = scene_->addPath(
                    QPainterPath(),
                    QPen(color, 2, styleIt == seriesPenStyles.end() ? Qt::SolidLine : styleIt->second)
                );
                series.path->setZValue(widget.zIndex + 0.4 + static_cast<double>(seriesIndex) * 0.001);

                if (legacyChart) {
                    const auto legendX = geometry.x() + geometry.width() * 0.80;
                    const auto legendRows = std::max<std::size_t>(1, runtimeWidget.indexes.size());
                    const auto legendRowHeight = std::min(
                        30.0,
                        std::max(20.0, (runtimeWidget.chartHeight - 44.0) / static_cast<double>(legendRows))
                    );
                    const auto legendY = runtimeWidget.chartY + 32.0 +
                        static_cast<double>(seriesIndex) * legendRowHeight;
                    const auto legendPenStyle = styleIt == seriesPenStyles.end() ? Qt::SolidLine : styleIt->second;
                    series.legendSwatch = scene_->addLine(
                        legendX,
                        legendY + 7,
                        legendX + 24,
                        legendY + 7,
                        QPen(color, 3, legendPenStyle)
                    );
                    series.legendSwatch->setZValue(widget.zIndex + 0.35);
                    series.legendText = scene_->addText(
                        QString::fromStdString(series.name + (series.unit.empty() ? "" : " (" + series.unit + ")")),
                        QFont(QStringLiteral("Microsoft YaHei"), 9)
                    );
                    series.legendText->setDefaultTextColor(QColor("#C5D5DC"));
                    series.legendText->document()->setDocumentMargin(0);
                    series.legendText->setTextWidth(std::max(1.0, geometry.right() - legendX - 40.0));
                    auto legendOption = series.legendText->document()->defaultTextOption();
                    legendOption.setWrapMode(QTextOption::NoWrap);
                    series.legendText->document()->setDefaultTextOption(legendOption);
                    series.legendText->setPos(legendX + 32, legendY);
                    series.legendText->setZValue(widget.zIndex + 0.35);
                }
                runtimeWidget.trendSeries.push_back(series);
                ++seriesIndex;
            }
        }
        if (unboundDataWidget || unboundStateWidget ||
            !runtimeWidget.indexes.empty() || !runtimeWidget.stateRules.empty() ||
            !runtimeWidget.alarms.empty() || isTrendType(widget.type) || widget.type == "alarmTable") {
            runtimeWidgets_.push_back(runtimeWidget);
        }
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
        if (widget.pcsPowerControl != nullptr) {
            widget.pcsPowerControl->refresh(now);
            continue;
        }
        if (widget.type == "energyFlow") {
            refreshEnergyFlow(widget, byIndex, now);
            continue;
        }
        if (widget.type == "alarmTable") {
            QStringList active;
            for (const auto& alarm : widget.alarms) {
                if (conditionMatches(alarm.condition, byIndex)) {
                    const auto current = byIndex.find(alarm.condition.index);
                    const auto severity = alarm.severity == "critical"
                        ? QString::fromUtf8("严重")
                        : alarm.severity == "warning"
                            ? QString::fromUtf8("警告")
                            : QString::fromUtf8("提示");
                    active.push_back(QStringLiteral("%1    %2    %3 / %4")
                        .arg(severity)
                        .arg(QString::fromStdString(alarm.label))
                        .arg(current == byIndex.end() ? QStringLiteral("--") : formatNumber(current->second.value))
                        .arg(QString::fromStdString(alarm.condition.expected)));
                }
            }
            const auto header = QString::fromUtf8("级别    报警描述                         当前值 / 阈值");
            const auto text = header + QStringLiteral("\n\n") + (active.empty()
                ? QString::fromUtf8("暂无活动告警")
                : active.join(QStringLiteral("\n")));
            if (text.toStdString() != widget.lastText) {
                widget.valueText->setPlainText(text);
                widget.lastText = text.toStdString();
            }
        } else if (!isTrendType(widget.type) && widget.stateRules.empty()) {
            const auto text = valueText(widget, byIndex);
            const auto utf8 = text.toStdString();
            if (utf8 != widget.lastText) {
                widget.valueText->setPlainText(text);
                widget.lastText = utf8;
            }
        }

        refreshState(widget, byIndex);
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
            if (widget.progressVertical) {
                const auto filledHeight = widget.progressHeight * ratio;
                widget.progressFill->setRect(
                    widget.progressX,
                    widget.progressY + widget.progressHeight - filledHeight,
                    widget.progressWidth,
                    filledHeight
                );
            } else {
                widget.progressFill->setRect(
                    widget.progressX,
                    widget.progressY,
                    widget.progressWidth * ratio,
                    widget.progressHeight
                );
            }
        }
        if (!widget.signalBars.empty()) {
            const auto percent = good ? std::max(0.0, std::min(100.0, value->second.value)) : 0.0;
            const auto activeBars = percent <= 0.0
                ? 0
                : std::min(4, static_cast<int>(std::ceil(percent / 25.0)));
            const auto activeColor = percent < 25.0
                ? QColor("#E45858")
                : percent < 50.0 ? QColor("#D9A441") : QColor("#20C879");
            for (std::size_t i = 0; i < widget.signalBars.size(); ++i) {
                widget.signalBars[i]->setBrush(
                    good && static_cast<int>(i) < activeBars ? activeColor : QColor("#35505D")
                );
            }
        }
        sampleTrend(widget, byIndex, now);
        renderTrend(widget);
    }
}

void ScadaSceneView::refreshEnergyFlow(
    RuntimeWidget& widget,
    const std::unordered_map<std::uint32_t, edge_gateway::StoredPointValue>& values,
    std::int64_t timestamp
) {
    widget.flowValueGood = false;
    if (!widget.indexes.empty()) {
        const auto current = values.find(widget.indexes.front());
        if (current != values.end() && current->second.quality == 1 && !current->second.stale) {
            widget.flowValue = current->second.value;
            widget.flowValueGood = true;
        }
    }
    if (widget.flowValueText != nullptr) {
        widget.flowValueText->setPlainText(widget.flowValueGood
            ? formatNumber(widget.flowValue) + QStringLiteral(" kW")
            : QStringLiteral("-- kW"));
    }
    animateEnergyFlow(widget, timestamp);
}

void ScadaSceneView::animateEnergyFlow(RuntimeWidget& widget, std::int64_t timestamp) {
    if (widget.flowLine == nullptr || widget.flowArrow == nullptr) return;
    const auto active = widget.flowValueGood && std::fabs(widget.flowValue) > widget.flowDeadband;
    const auto forward = widget.flowForwardWhenPositive ? widget.flowValue > 0.0 : widget.flowValue < 0.0;
    const auto activeColor = colorOr(widget.flowColor, "#20DBE9");
    const auto idleColor = colorOr(widget.flowIdleColor, "#2C6078");

    QPen linePen(active ? activeColor : idleColor, active ? 5.0 : 3.0);
    linePen.setCapStyle(Qt::RoundCap);
    if (!active) linePen.setStyle(Qt::DashLine);
    widget.flowLine->setPen(linePen);

    if (!active) {
        widget.flowArrow->setVisible(false);
        for (auto* particle : widget.flowParticles) particle->setVisible(false);
        return;
    }

    const auto tip = forward ? widget.flowEnd : widget.flowStart;
    const auto tail = forward ? widget.flowStart : widget.flowEnd;
    const auto vector = tip - tail;
    const auto length = std::max(1.0, std::hypot(vector.x(), vector.y()));
    const QPointF direction(vector.x() / length, vector.y() / length);
    const QPointF normal(-direction.y(), direction.x());
    const auto arrowBase = tip - direction * 15.0;
    QPainterPath arrow;
    arrow.moveTo(tip);
    arrow.lineTo(arrowBase + normal * 8.0);
    arrow.lineTo(arrowBase - normal * 8.0);
    arrow.closeSubpath();
    widget.flowArrow->setPath(arrow);
    widget.flowArrow->setPen(QPen(activeColor, 1));
    widget.flowArrow->setBrush(QBrush(activeColor));
    widget.flowArrow->setVisible(true);

    const auto loadRatio = std::min(1.0, std::fabs(widget.flowValue) / widget.flowRatedPower);
    const auto cycleMs = 2100.0 - 1200.0 * loadRatio;
    const auto phase = std::fmod(static_cast<double>(timestamp), cycleMs) / cycleMs;
    const auto count = std::max<std::size_t>(1, widget.flowParticles.size());
    for (std::size_t i = 0; i < widget.flowParticles.size(); ++i) {
        auto position = std::fmod(phase + static_cast<double>(i) / static_cast<double>(count), 1.0);
        if (!forward) position = 1.0 - position;
        const auto point = widget.flowStart + (widget.flowEnd - widget.flowStart) * position;
        widget.flowParticles[i]->setPos(point);
        widget.flowParticles[i]->setVisible(position > 0.06 && position < 0.88);
    }
}

void ScadaSceneView::sampleTrends(std::int64_t now) {
    std::vector<std::uint32_t> requestedIndexes;
    for (const auto& widget : runtimeWidgets_) {
        for (const auto& series : widget.trendSeries) requestedIndexes.push_back(series.index);
    }
    if (requestedIndexes.empty()) return;
    std::sort(requestedIndexes.begin(), requestedIndexes.end());
    requestedIndexes.erase(std::unique(requestedIndexes.begin(), requestedIndexes.end()), requestedIndexes.end());
    const auto values = runtime_.readIndexes(requestedIndexes, now);
    std::unordered_map<std::uint32_t, edge_gateway::StoredPointValue> byIndex;
    for (const auto& value : values) byIndex[value.index] = value;
    for (auto& widget : runtimeWidgets_) {
        if (!widget.trendSeries.empty()) sampleTrend(widget, byIndex, now);
    }
}

void ScadaSceneView::handleAction(const edge_gateway::ScadaWidgetAction& action) {
    if (action.type == "navigate") {
        if (!action.targetScreen.empty()) navigate_(action.targetScreen);
        return;
    }
    if (!isControlAction(action.type)) return;

    const auto resolved = runtime_.resolveTag(action.tagId);
    if (!resolved || resolved->tag.nodeId != runtime_.nodeId()) {
        QMessageBox::warning(this, QString::fromUtf8("控制失败"), QString::fromUtf8("当前边端不存在该控制点。"));
        return;
    }

    double target = 0.0;
    if (action.type == "toggle") {
        const auto current = runtime_.readTag(action.tagId, nowMs());
        if (!current || current->quality != 1 || current->stale) {
            QMessageBox::warning(this, QString::fromUtf8("控制失败"), QString::fromUtf8("当前值不可用，已拒绝切换操作。"));
            return;
        }
        target = std::fabs(current->value) > 0.000001 ? 0.0 : 1.0;
    } else if (!action.value.empty()) {
        if (!parseDouble(action.value, &target)) {
            QMessageBox::warning(this, QString::fromUtf8("控制失败"), QString::fromUtf8("配置的控制目标值无效。"));
            return;
        }
    } else if (action.type == "pulse") {
        target = 1.0;
    } else {
        bool accepted = false;
        target = QInputDialog::getDouble(
            this,
            QString::fromUtf8("设定值控制"),
            QString::fromUtf8("目标值"),
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
            QString::fromUtf8("确认控制"),
            QString::fromUtf8("确认将 %1 写入 %2？").arg(target).arg(QString::fromStdString(resolved->tag.displayName)),
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
        QMessageBox::warning(this, QString::fromUtf8("控制失败"), QString::fromStdString(result.message));
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
                QMessageBox::warning(this, QString::fromUtf8("脉冲复位失败"), QString::fromStdString(resetResult.message));
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
    return matchesScadaCondition(current->second.value, condition.comparison, condition.expected);
}

void ScadaSceneView::refreshState(
    RuntimeWidget& widget,
    const std::unordered_map<std::uint32_t, edge_gateway::StoredPointValue>& values
) {
    if (widget.stateRules.empty()) return;
    bool allConditionsAvailable = true;
    for (const auto& rule : widget.stateRules) {
        for (const auto& condition : rule.conditions) {
            const auto current = values.find(condition.index);
            if (current == values.end() || current->second.quality != 1 || current->second.stale) {
                allConditionsAvailable = false;
            }
        }
    }
    const RuntimeStateRule* matched = nullptr;
    for (const auto& rule : widget.stateRules) {
        if (!allConditionsAvailable) break;
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

    const auto visualCode = !allConditionsAvailable
        ? std::string("__unavailable")
        : matched == nullptr ? std::string("__default") : matched->code;
    if (visualCode == widget.lastVisualCode) return;
    widget.lastVisualCode = visualCode;
    const auto color = !allConditionsAvailable
        ? QColor("#71808A")
        : matched == nullptr
        ? colorOr(widget.defaultColor, "#102A38")
        : colorOr(matched->color, "#AAB3BD");
    if (widget.panel != nullptr && widget.stateColorPanel) {
        const auto imageOnlyState = widget.stateImage != nullptr &&
            (widget.type == "statusLamp" || widget.type == "statusCard");
        widget.panel->setBrush(imageOnlyState ? QBrush(Qt::NoBrush) : QBrush(color));
    }
    if (widget.statusLamp != nullptr) widget.statusLamp->setBrush(color);
    if (widget.stateImage != nullptr) {
        const auto imageReference = matched == nullptr ? widget.defaultImage : matched->image;
        if (!imageReference.empty()) {
            const auto imagePath = QDir(QString::fromStdString(projectRoot_)).filePath(QString::fromStdString(imageReference));
            QPixmap pixmap(imagePath);
            if (!pixmap.isNull()) {
                widget.stateImage->setPixmap(pixmap.scaled(
                    static_cast<int>(std::round(widget.stateImage->boundingRect().width())),
                    static_cast<int>(std::round(widget.stateImage->boundingRect().height())),
                    Qt::IgnoreAspectRatio,
                    Qt::SmoothTransformation
                ));
            }
        }
    }
    if (widget.stateImage == nullptr && widget.valueText != nullptr && !isTrendType(widget.type)) {
        const auto label = !allConditionsAvailable
            ? std::string("data unavailable")
            : !widget.stateLabelVisible
            ? std::string()
            : matched != nullptr && !matched->label.empty()
                ? matched->label
                : widget.defaultLabel.empty() ? std::string("--") : widget.defaultLabel;
        widget.valueText->setDefaultTextColor(!allConditionsAvailable || matched == nullptr
            ? QColor("#AAB3BD")
            : QColor("#F3F8FA"));
        widget.valueText->setPlainText(!allConditionsAvailable
            ? QString::fromUtf8("数据不可用")
            : QString::fromStdString(label));
        widget.lastText = label;
    }
}

void ScadaSceneView::sampleTrend(
    RuntimeWidget& widget,
    const std::unordered_map<std::uint32_t, edge_gateway::StoredPointValue>& values,
    std::int64_t now
) {
    if (widget.trendSeries.empty()) return;
    const auto dayStartTs = localDayStartMs(now);
    widget.chartDayStartTs = dayStartTs;
    std::int64_t latestTs = 0;
    for (auto& series : widget.trendSeries) {
        const auto current = values.find(series.index);
        if (current != values.end() && current->second.quality == 1 && !current->second.stale && current->second.ts > 0 &&
            current->second.ts >= dayStartTs && current->second.ts != series.lastSampleTs) {
            series.lastSampleTs = current->second.ts;
            const auto bucketTs = dayStartTs +
                ((current->second.ts - dayStartTs) / widget.chartSampleIntervalMs) * widget.chartSampleIntervalMs;
            if (series.samples.empty() || series.lastBucketTs != bucketTs) {
                series.samples.push_back(std::make_pair(current->second.ts, current->second.value));
                series.lastBucketTs = bucketTs;
            } else {
                series.samples.back() = std::make_pair(current->second.ts, current->second.value);
            }
        }
        while (!series.samples.empty() && series.samples.front().first < dayStartTs) series.samples.pop_front();
        while (static_cast<int>(series.samples.size()) > widget.trendMaxPoints) series.samples.pop_front();
        if (!series.samples.empty()) latestTs = std::max(latestTs, series.samples.back().first);
    }
    widget.chartLatestTs = latestTs;
}

void ScadaSceneView::renderTrend(RuntimeWidget& widget) {
    if (widget.trendSeries.empty()) return;
    const auto dayStartTs = widget.chartDayStartTs;
    const auto latestTs = widget.chartLatestTs;
    if (latestTs <= 0) {
        for (auto& series : widget.trendSeries) {
            if (series.path != nullptr) series.path->setPath(QPainterPath());
        }
        for (auto* label : widget.chartYLabels) label->setPlainText(QStringLiteral("--"));
        return;
    }

    const auto availableSpan = std::max<std::int64_t>(1, latestTs - dayStartTs);
    if (widget.chartFollowLatest || widget.chartViewStartTs <= 0 || widget.chartViewEndTs <= widget.chartViewStartTs) {
        const auto span = std::min(widget.chartViewWindowMs, availableSpan);
        widget.chartViewEndTs = latestTs;
        widget.chartViewStartTs = std::max(dayStartTs, latestTs - span);
    } else {
        auto span = std::max<std::int64_t>(1, widget.chartViewEndTs - widget.chartViewStartTs);
        span = std::min(span, availableSpan);
        if (widget.chartViewEndTs > latestTs) {
            widget.chartViewEndTs = latestTs;
            widget.chartViewStartTs = latestTs - span;
        }
        if (widget.chartViewStartTs < dayStartTs) {
            widget.chartViewStartTs = dayStartTs;
            widget.chartViewEndTs = std::min(latestTs, dayStartTs + span);
        }
    }
    const auto firstTs = widget.chartViewStartTs;
    const auto lastTs = std::max(widget.chartViewStartTs + 1, widget.chartViewEndTs);

    for (std::size_t line = 0; line < widget.chartXLabels.size(); ++line) {
        const auto ratio = widget.chartXLabels.size() <= 1
            ? 0.0
            : static_cast<double>(line) / static_cast<double>(widget.chartXLabels.size() - 1);
        const auto ts = firstTs + static_cast<std::int64_t>(static_cast<double>(lastTs - firstTs) * ratio);
        widget.chartXLabels[line]->setPlainText(
            QDateTime::fromMSecsSinceEpoch(ts).toString(QStringLiteral("HH:mm:ss"))
        );
    }

    double minimum = std::numeric_limits<double>::max();
    double maximum = std::numeric_limits<double>::lowest();
    bool hasVisibleSamples = false;
    for (const auto& series : widget.trendSeries) {
        int visibleSamples = 0;
        for (const auto& sample : series.samples) {
            if (sample.first < firstTs || sample.first > lastTs) continue;
            if (!std::isfinite(sample.second)) continue;
            minimum = std::min(minimum, sample.second);
            maximum = std::max(maximum, sample.second);
            ++visibleSamples;
        }
        hasVisibleSamples = hasVisibleSamples || visibleSamples >= 1;
    }
    if (!hasVisibleSamples) {
        for (auto& series : widget.trendSeries) {
            if (series.path != nullptr) series.path->setPath(QPainterPath());
        }
        for (auto* label : widget.chartYLabels) label->setPlainText(QStringLiteral("--"));
        return;
    }
    if (std::fabs(maximum - minimum) < 1e-9) {
        minimum -= 1.0;
        maximum += 1.0;
    }

    const auto padding = std::max(1e-6, (maximum - minimum) * 0.08);
    minimum -= padding;
    maximum += padding;

    for (std::size_t line = 0; line < widget.chartYLabels.size(); ++line) {
        const auto ratio = widget.chartYLabels.size() <= 1
            ? 0.0
            : static_cast<double>(line) / static_cast<double>(widget.chartYLabels.size() - 1);
        widget.chartYLabels[line]->setPlainText(formatNumber(maximum - (maximum - minimum) * ratio));
    }
    for (auto& series : widget.trendSeries) {
        QPainterPath path;
        QPointF singlePoint;
        int visibleSamples = 0;
        bool pathOpen = false;
        for (const auto& sample : series.samples) {
            if (sample.first < firstTs || sample.first > lastTs) continue;
            if (!std::isfinite(sample.second)) {
                pathOpen = false;
                continue;
            }
            const auto rawXRatio = lastTs <= firstTs
                ? 0.0
                : static_cast<double>(sample.first - firstTs) /
                    static_cast<double>(lastTs - firstTs);
            const auto xRatio = std::max(0.0, std::min(1.0, rawXRatio));
            const auto rawYRatio = (sample.second - minimum) / (maximum - minimum);
            const auto yRatio = std::max(0.0, std::min(1.0, rawYRatio));
            const auto x = widget.chartX + widget.chartWidth * xRatio;
            const auto y = widget.chartY + widget.chartHeight * (1.0 - yRatio);
            if (!std::isfinite(x) || !std::isfinite(y)) {
                pathOpen = false;
                continue;
            }
            singlePoint = QPointF(x, y);
            ++visibleSamples;
            if (!pathOpen) path.moveTo(x, y);
            else path.lineTo(x, y);
            pathOpen = true;
        }
        if (visibleSamples == 1) {
            const auto left = std::max(widget.chartX, singlePoint.x() - 5.0);
            const auto right = std::min(widget.chartX + widget.chartWidth, singlePoint.x() + 5.0);
            path = QPainterPath(QPointF(left, singlePoint.y()));
            path.lineTo(std::max(left + 1.0, right), singlePoint.y());
        }
        if (series.path != nullptr) series.path->setPath(path);
    }
}

ScadaSceneView::RuntimeWidget* ScadaSceneView::trendWidgetAt(const QPoint& viewportPosition) {
    const auto scenePosition = mapToScene(viewportPosition);
    for (auto item = runtimeWidgets_.rbegin(); item != runtimeWidgets_.rend(); ++item) {
        if (item->trendSeries.empty()) continue;
        const QRectF chartRect(item->chartX, item->chartY, item->chartWidth, item->chartHeight);
        if (chartRect.contains(scenePosition)) return &*item;
    }
    return nullptr;
}

ScadaSceneView::RuntimeWidget* ScadaSceneView::inputWidgetAt(const QPoint& viewportPosition) {
    const auto scenePosition = mapToScene(viewportPosition);
    RuntimeWidget* selected = nullptr;
    double selectedZ = std::numeric_limits<double>::lowest();
    for (auto& widget : runtimeWidgets_) {
        if (widget.type != "qtInput" || widget.inputTagId.empty() || widget.panel == nullptr) continue;
        if (!widget.panel->sceneBoundingRect().contains(scenePosition)) continue;
        if (widget.panel->zValue() >= selectedZ) {
            selected = &widget;
            selectedZ = widget.panel->zValue();
        }
    }
    return selected;
}

void ScadaSceneView::editInput(RuntimeWidget& widget) {
    const auto resolved = runtime_.resolveTag(widget.inputTagId);
    if (!widget.inputWritable || !resolved || !resolved->mapping.writable) {
        QMessageBox::warning(
            this,
            QString::fromUtf8("参数写入"),
            QString::fromUtf8("该点位未配置写入权限，已拒绝下发。")
        );
        return;
    }

    const auto current = runtime_.readTag(widget.inputTagId, nowMs());
    const auto hasCurrent = current && current->quality == 1 && !current->stale;
    const auto currentValue = hasCurrent ? current->value : 0.0;
    const auto minimum = widget.inputMinValue ? *widget.inputMinValue : -1000000000.0;
    const auto maximum = widget.inputMaxValue ? *widget.inputMaxValue : 1000000000.0;
    if (!std::isfinite(minimum) || !std::isfinite(maximum) || minimum > maximum) {
        QMessageBox::warning(
            this,
            QString::fromUtf8("参数写入"),
            QString::fromUtf8("该点位的写入范围配置无效，请先修正设备配置。")
        );
        return;
    }

    int decimals = 3;
    if (widget.inputStep > 0.0) {
        decimals = 0;
        auto scaled = widget.inputStep;
        while (decimals < 6 && std::fabs(scaled - std::round(scaled)) > 1e-9) {
            scaled *= 10.0;
            ++decimals;
        }
    }
    bool accepted = false;
    const auto displayName = QString::fromStdString(resolved->tag.displayName.empty()
        ? resolved->tag.tagId
        : resolved->tag.displayName);
    const auto target = QInputDialog::getDouble(
        this,
        QString::fromUtf8("参数写入"),
        QString::fromUtf8("请输入 %1：").arg(displayName),
        std::max(minimum, std::min(maximum, currentValue)),
        minimum,
        maximum,
        decimals,
        &accepted
    );
    if (!accepted) return;

    if (widget.inputStep > 0.0) {
        const auto base = widget.inputMinValue ? *widget.inputMinValue : 0.0;
        const auto ratio = (target - base) / widget.inputStep;
        if (std::fabs(ratio - std::round(ratio)) > 1e-7) {
            QMessageBox::warning(
                this,
                QString::fromUtf8("参数写入"),
                QString::fromUtf8("输入值不符合步长 %1，请重新输入。").arg(formatNumber(widget.inputStep))
            );
            return;
        }
    }

    const auto currentText = hasCurrent ? formatNumber(currentValue) : QStringLiteral("--");
    const auto answer = QMessageBox::question(
        this,
        QString::fromUtf8("确认写入"),
        QString::fromUtf8("%1\n当前值：%2\n目标值：%3\n\n确认写入吗？")
            .arg(displayName)
            .arg(currentText)
            .arg(formatNumber(target)),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No
    );
    if (answer != QMessageBox::Yes) return;

    edge_gateway::PendingWriteCommand command;
    command.cmdId = nextCommandId();
    command.value = target;
    command.source = "scada-local-input";
    command.ts = nowMs();
    command.acceptedAt = command.ts;
    command.highPriority = widget.action.highPriority;
    const auto result = runtime_.submitWrite(widget.inputTagId, command);
    if (!result.accepted) {
        QMessageBox::warning(
            this,
            QString::fromUtf8("写入失败"),
            QString::fromUtf8("边端拒绝写入：%1").arg(QString::fromStdString(result.message))
        );
        return;
    }

    refresh(nowMs());
    QMessageBox::information(
        this,
        QString::fromUtf8("写入请求已受理"),
        QString::fromUtf8("%1 已写入 %2。\n边端返回：%3")
            .arg(displayName)
            .arg(formatNumber(target))
            .arg(QString::fromStdString(result.message))
    );
}

ScadaSceneView::RuntimeTrendSeries* ScadaSceneView::trendLegendSeriesAt(const QPoint& viewportPosition) {
    const auto scenePosition = mapToScene(viewportPosition);
    for (auto widget = runtimeWidgets_.rbegin(); widget != runtimeWidgets_.rend(); ++widget) {
        for (auto series = widget->trendSeries.rbegin(); series != widget->trendSeries.rend(); ++series) {
            if (series->legendSwatch == nullptr || series->legendText == nullptr) continue;
            const auto hitRect = series->legendSwatch->sceneBoundingRect()
                .united(series->legendText->sceneBoundingRect())
                .adjusted(-8.0, -6.0, 8.0, 6.0);
            if (hitRect.contains(scenePosition)) return &*series;
        }
    }
    return nullptr;
}

void ScadaSceneView::toggleTrendSeries(RuntimeTrendSeries& series) {
    series.visible = !series.visible;
    if (series.path != nullptr) series.path->setVisible(series.visible);
    const auto opacity = series.visible ? 1.0 : 0.3;
    if (series.legendSwatch != nullptr) series.legendSwatch->setOpacity(opacity);
    if (series.legendText != nullptr) series.legendText->setOpacity(opacity);
}

void ScadaSceneView::wheelEvent(QWheelEvent* event) {
    const auto viewportPosition = wheelViewportPosition(*event);
    auto* widget = trendWidgetAt(viewportPosition);
    if (widget == nullptr || widget->chartLatestTs <= widget->chartDayStartTs || event->angleDelta().y() == 0) {
        QGraphicsView::wheelEvent(event);
        return;
    }

    const auto maximumSpan = std::max<std::int64_t>(1, widget->chartLatestTs - widget->chartDayStartTs);
    const auto minimumSpan = std::min(widget->chartMinWindowMs, maximumSpan);
    const auto currentSpan = std::max<std::int64_t>(
        1,
        widget->chartViewEndTs - widget->chartViewStartTs
    );
    const auto factor = event->angleDelta().y() > 0 ? 0.8 : 1.25;
    const auto newSpan = std::max(
        minimumSpan,
        std::min(maximumSpan, static_cast<std::int64_t>(std::llround(currentSpan * factor)))
    );
    widget->chartViewWindowMs = newSpan;

    if (widget->chartFollowLatest) {
        widget->chartViewEndTs = widget->chartLatestTs;
        widget->chartViewStartTs = std::max(widget->chartDayStartTs, widget->chartLatestTs - newSpan);
    } else {
        const auto scenePosition = mapToScene(viewportPosition);
        const auto anchorRatio = std::max(
            0.0,
            std::min(1.0, (scenePosition.x() - widget->chartX) / std::max(1.0, widget->chartWidth))
        );
        const auto anchorTs = widget->chartViewStartTs +
            static_cast<std::int64_t>(std::llround(currentSpan * anchorRatio));
        auto startTs = anchorTs - static_cast<std::int64_t>(std::llround(newSpan * anchorRatio));
        auto endTs = startTs + newSpan;
        if (startTs < widget->chartDayStartTs) {
            startTs = widget->chartDayStartTs;
            endTs = startTs + newSpan;
        }
        if (endTs > widget->chartLatestTs) {
            endTs = widget->chartLatestTs;
            startTs = endTs - newSpan;
        }
        widget->chartViewStartTs = std::max(widget->chartDayStartTs, startTs);
        widget->chartViewEndTs = std::min(widget->chartLatestTs, endTs);
    }
    event->accept();
}

void ScadaSceneView::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        auto* series = trendLegendSeriesAt(event->pos());
        if (series != nullptr) {
            toggleTrendSeries(*series);
            event->accept();
            return;
        }
        panningTrend_ = trendWidgetAt(event->pos());
        if (panningTrend_ != nullptr) {
            lastPanScenePosition_ = mapToScene(event->pos());
            viewport()->setCursor(Qt::ClosedHandCursor);
            event->accept();
            return;
        }
    }
    QGraphicsView::mousePressEvent(event);
}

void ScadaSceneView::panTrend(RuntimeWidget& widget, double sceneDeltaX) {
    if (widget.chartWidth <= 0 || widget.chartLatestTs <= widget.chartDayStartTs) return;
    const auto span = std::max<std::int64_t>(1, widget.chartViewEndTs - widget.chartViewStartTs);
    const auto shift = -static_cast<std::int64_t>(std::llround(sceneDeltaX * span / widget.chartWidth));
    auto startTs = widget.chartViewStartTs + shift;
    auto endTs = widget.chartViewEndTs + shift;
    if (startTs < widget.chartDayStartTs) {
        startTs = widget.chartDayStartTs;
        endTs = startTs + span;
    }
    if (endTs > widget.chartLatestTs) {
        endTs = widget.chartLatestTs;
        startTs = endTs - span;
    }
    widget.chartViewStartTs = std::max(widget.chartDayStartTs, startTs);
    widget.chartViewEndTs = std::min(widget.chartLatestTs, endTs);
    widget.chartFollowLatest = false;
}

void ScadaSceneView::mouseMoveEvent(QMouseEvent* event) {
    if (panningTrend_ != nullptr) {
        const auto scenePosition = mapToScene(event->pos());
        panTrend(*panningTrend_, scenePosition.x() - lastPanScenePosition_.x());
        lastPanScenePosition_ = scenePosition;
        event->accept();
        return;
    }
    if (trendLegendSeriesAt(event->pos()) != nullptr) {
        viewport()->setCursor(Qt::PointingHandCursor);
    } else {
        viewport()->unsetCursor();
    }
    QGraphicsView::mouseMoveEvent(event);
}

void ScadaSceneView::mouseReleaseEvent(QMouseEvent* event) {
    if (panningTrend_ != nullptr && event->button() == Qt::LeftButton) {
        panningTrend_ = nullptr;
        viewport()->unsetCursor();
        event->accept();
        return;
    }
    QGraphicsView::mouseReleaseEvent(event);
}

void ScadaSceneView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        auto* input = inputWidgetAt(event->pos());
        if (input != nullptr) {
            editInput(*input);
            event->accept();
            return;
        }
    }
    auto* widget = trendWidgetAt(event->pos());
    if (widget != nullptr && event->button() == Qt::LeftButton) {
        widget->chartFollowLatest = true;
        widget->chartViewWindowMs = widget->chartDefaultWindowMs;
        widget->chartViewStartTs = 0;
        widget->chartViewEndTs = 0;
        event->accept();
        return;
    }
    QGraphicsView::mouseDoubleClickEvent(event);
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
            std::string mapped;
            auto formatted = resolveScadaValueLabel(widget.valueMap, value->second.value, &mapped)
                ? QString::fromStdString(mapped)
                : formatNumber(value->second.value);
            if (!widget.valueSuffix.empty()) {
                formatted += QString::fromStdString(widget.valueSuffix);
            }
            parts.push_back(formatted);
        }
    }
    return parts.join(QStringLiteral("  "));
}

ScadaRuntimeWindow::ScadaRuntimeWindow(
    const std::string& projectDirectory,
    int refreshIntervalMs,
    bool autoReload,
    std::function<std::unique_ptr<ScadaSceneRuntimeSource>(const edge_gateway::ScadaProject&)> runtimeFactory,
    QWidget* parent
) : QWidget(parent),
    projectDirectory_(projectDirectory),
    runtimeFactory_(std::move(runtimeFactory)),
    autoReload_(autoReload) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    stack_ = new QStackedWidget(this);
    layout->addWidget(stack_);

    logoutButton_ = new QPushButton(this);
    logoutButton_->setObjectName(QStringLiteral("scadaLogoutButton"));
    logoutButton_->setStyleSheet(R"CSS(
        QPushButton { background:#3B2B30; color:#F3F8FA; border:1px solid #A45A64;
                      border-radius:5px; font-size:18px; font-weight:700; padding:8px 14px; }
        QPushButton:pressed { background:#5A3038; }
    )CSS");
    logoutButton_->hide();
    QObject::connect(logoutButton_, &QPushButton::clicked, this, [this]() { logoutLocalUser(); });
    qApp->installEventFilter(this);

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
        enforceAccessExpiry(now);
        refreshAll(now);
    });
    timer_->start(std::max(100, refreshIntervalMs));
    refreshAll(nowMs());
}

void ScadaRuntimeWindow::reloadProject(bool initial) {
    auto loaded = edge_gateway::ScadaProjectLoader::loadFromDirectory(projectDirectory_);
    if (loaded.screens.empty()) throw std::runtime_error("SCADA project has no screens for local runtime");
    project_ = std::move(loaded);
    accessSession_ = ScadaLocalAccessSession(project_.permissions.localAccess);
    runtime_ = runtimeFactory_(project_);
    if (!runtime_) throw std::runtime_error("SCADA runtime source factory returned no source");
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
        screenIndexes_[screen.screenId] = -1;
    }
    for (const auto& screen : project_.screens) {
        const auto containsTrend = std::any_of(screen.widgets.begin(), screen.widgets.end(), [](const auto& widget) {
            return widget.visible && isTrendType(widget.type);
        });
        if ((screen.screenId == project_.manifest.entryScreen || containsTrend) &&
            !accessSession_.requiresAuthentication(screen.screenId)) {
            buildScreen(screen);
        }
    }
    showScreen(project_.manifest.entryScreen);
}

int ScadaRuntimeWindow::buildScreen(const edge_gateway::ScadaScreen& screen) {
    auto* view = new ScadaSceneView(
        screen,
        project_.rootDirectory,
        project_.alarms,
        project_.trends,
        *runtime_,
        [this](const std::string& target) { showScreen(target); },
        stack_
    );
    const auto index = stack_->addWidget(view);
    views_.push_back(view);
    screenIndexes_[screen.screenId] = index;
    return index;
}

bool ScadaRuntimeWindow::screenRequiresLocalAuthentication(const std::string& screenId) const {
    return accessSession_.requiresAuthentication(screenId);
}

bool ScadaRuntimeWindow::authenticateLocalAccess(
    const std::string& username,
    const std::string& password,
    std::string* message
) {
    return accessSession_.authenticate(username, password, nowMs(), message);
}

void ScadaRuntimeWindow::showScreen(const std::string& screenId) {
    auto item = screenIndexes_.find(screenId);
    if (item != screenIndexes_.end()) {
        const auto timestamp = nowMs();
        if (!accessSession_.authorizedForScreen(screenId, timestamp)) {
            if (!requestScadaLocalLogin(this, accessSession_, timestamp)) return;
        }
        accessSession_.touch(timestamp);
        auto index = item->second;
        if (index < 0) {
            const auto screen = std::find_if(project_.screens.begin(), project_.screens.end(), [&](const auto& value) {
                return value.screenId == screenId;
            });
            if (screen == project_.screens.end()) return;
            index = buildScreen(*screen);
        }
        stack_->setCurrentIndex(index);
        currentScreenId_ = screenId;
        updateAccessUi();
        refreshCurrent();
    } else if (stack_->count() > 0) {
        stack_->setCurrentIndex(0);
        updateAccessUi();
    }
}

void ScadaRuntimeWindow::refreshCurrent() {
    auto* view = dynamic_cast<ScadaSceneView*>(stack_->currentWidget());
    if (view != nullptr) view->refresh(nowMs());
}

void ScadaRuntimeWindow::refreshAll(std::int64_t now) {
    auto* current = dynamic_cast<ScadaSceneView*>(stack_->currentWidget());
    for (auto* view : views_) {
        if (view == nullptr) continue;
        if (view == current) view->refresh(now);
        else view->sampleTrends(now);
    }
}

void ScadaRuntimeWindow::enforceAccessExpiry(std::int64_t timestamp) {
    if (!accessSession_.requiresAuthentication(currentScreenId_) ||
        accessSession_.authenticated(timestamp)) {
        return;
    }
    accessSession_.logout();
    rebuildScreens();
}

void ScadaRuntimeWindow::updateAccessUi() {
    if (logoutButton_ == nullptr) return;
    const auto visible = accessSession_.requiresAuthentication(currentScreenId_) &&
        accessSession_.authenticated(nowMs());
    logoutButton_->setVisible(visible);
    if (visible) {
        logoutButton_->setText(
            QString::fromUtf8("%1 · 退出").arg(QString::fromStdString(accessSession_.username()))
        );
        logoutButton_->raise();
    }
}

void ScadaRuntimeWindow::logoutLocalUser() {
    accessSession_.logout();
    rebuildScreens();
}

bool ScadaRuntimeWindow::eventFilter(QObject* watched, QEvent* event) {
    if (accessSession_.authenticated(nowMs()) && event != nullptr) {
        const auto type = event->type();
        const auto activity = type == QEvent::MouseButtonPress || type == QEvent::KeyPress ||
            type == QEvent::TouchBegin || type == QEvent::Wheel;
        auto* widget = qobject_cast<QWidget*>(watched);
        if (activity && widget != nullptr && (widget == this || isAncestorOf(widget))) {
            accessSession_.touch(nowMs());
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ScadaRuntimeWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (logoutButton_ != nullptr) {
        logoutButton_->setGeometry(std::max(8, width() - 190), 18, 170, 58);
        logoutButton_->raise();
    }
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
