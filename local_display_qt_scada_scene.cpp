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
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
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
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QStringList>
#include <QTimer>
#include <QTextDocument>
#include <QTextOption>
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

Qt::PenStyle chartPenStyle(const QString& value) {
    if (value.compare(QStringLiteral("DashLine"), Qt::CaseInsensitive) == 0) return Qt::DashLine;
    if (value.compare(QStringLiteral("DotLine"), Qt::CaseInsensitive) == 0) return Qt::DotLine;
    if (value.compare(QStringLiteral("DashDotLine"), Qt::CaseInsensitive) == 0) return Qt::DashDotLine;
    return Qt::SolidLine;
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
        const auto imageOverlayText = property(widget, "qtText");
        const auto imageHasTextOverlay =
            (widget.type == "qtImage" || widget.type == "image") && !imageOverlayText.empty();
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
            button->setStyleSheet(style);
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

        const auto qtNativeText = isQtNativeTextType(widget.type) || imageHasTextOverlay;
        const auto legacyQtWidget = !property(widget, "qtClass").empty();
        const auto qtNativeVisual = isQtNativeVisualType(widget.type) || legacyQtWidget;
        const auto qtFillProgress = widget.type == "qtFillProgress";
        const auto legacyQtProgress = isProgressType(widget.type) && legacyQtWidget;
        const auto alarmTable = widget.type == "alarmTable";
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
                : widget.type == "qtInput"
                    ? QPen(QColor("#6F8FA7"), 1)
                : qtNativeVisual
                    ? QPen(Qt::NoPen)
                    : QPen(QColor("#275165"), 1),
            (qtFillProgress || legacyQtProgress)
                ? QBrush(Qt::NoBrush)
                : alarmTable
                    ? QBrush(QColor("#071A2D"))
                    : QBrush(background)
        );
        panel->setZValue(widget.zIndex);

        if (!qtNativeVisual) {
            auto* title = scene_->addText(QString::fromStdString(widget.title));
            title->setDefaultTextColor(QColor("#9BB0BB"));
            title->setFont(QFont(QStringLiteral("Microsoft YaHei"), 11));
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
        runtimeWidget.stateLabelVisible = property(widget, "stateLabelVisible") != "false";
        runtimeWidget.progressMax = numericProperty(widget, "progressMaxValue", 100.0);
        runtimeWidget.trendMaxPoints = std::max(2, static_cast<int>(numericProperty(widget, "chartMaxPoints", 120.0)));
        runtimeWidget.valueMap = parseScadaValueMapJson(property(widget, "valueMapJson"));
        for (const auto& binding : widget.bindings) {
            if (binding.nodeId != runtime_.nodeId()) continue;
            const auto resolved = runtime_.resolveTag(binding.tagId);
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

        const auto hideNativeValue = legacyQtProgress || widget.type == "qtChart";
        runtimeWidget.valueText = scene_->addText(hideNativeValue
            ? QString()
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
        auto textY = geometry.y() + (alarmTable ? 18.0 : qtNativeText ? 0.0 : std::min(38.0, geometry.height() * 0.42));
        if (imageHasTextOverlay) {
            const auto textHeight = runtimeWidget.valueText->boundingRect().height();
            const auto verticalAlignment = property(widget, "qtVerticalAlignment");
            if (verticalAlignment == "Center") {
                textY = geometry.y() + std::max(0.0, (geometry.height() - textHeight) / 2.0);
            } else if (verticalAlignment == "Bottom") {
                textY = geometry.bottom() - textHeight;
            }
        }
        runtimeWidget.valueText->setPos(textX, textY);
        runtimeWidget.valueText->setZValue(widget.zIndex + 0.3);
        runtimeWidget.valueText->setVisible(!hideNativeValue);

        if (widget.type == "statusLamp" || widget.type == "statusCard") {
            const auto imagePath = imageReference.empty()
                ? QString()
                : QDir(QString::fromStdString(projectRoot_)).filePath(QString::fromStdString(imageReference));
            QPixmap pixmap(imagePath);
            if (!pixmap.isNull()) {
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
                    geometry.y() + 12,
                    size,
                    size,
                    QPen(Qt::NoPen),
                    QBrush(QColor("#71808A"))
                );
                runtimeWidget.statusLamp->setZValue(widget.zIndex + 0.4);
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
                runtimeWidget.trendSeries.push_back(series);

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
                    auto* swatch = scene_->addLine(
                        legendX,
                        legendY + 7,
                        legendX + 24,
                        legendY + 7,
                        QPen(color, 3, legendPenStyle)
                    );
                    swatch->setZValue(widget.zIndex + 0.35);
                    auto* legend = scene_->addText(
                        QString::fromStdString(series.name + (series.unit.empty() ? "" : " (" + series.unit + ")")),
                        QFont(QStringLiteral("Microsoft YaHei"), 9)
                    );
                    legend->setDefaultTextColor(QColor("#C5D5DC"));
                    legend->document()->setDocumentMargin(0);
                    legend->setTextWidth(std::max(1.0, geometry.right() - legendX - 40.0));
                    auto legendOption = legend->document()->defaultTextOption();
                    legendOption.setWrapMode(QTextOption::NoWrap);
                    legend->document()->setDefaultTextOption(legendOption);
                    legend->setPos(legendX + 32, legendY);
                    legend->setZValue(widget.zIndex + 0.35);
                }
                ++seriesIndex;
            }
        }
        if (!runtimeWidget.indexes.empty() || !runtimeWidget.stateRules.empty() ||
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
        if (widget.type == "alarmTable") {
            QStringList active;
            int serial = 1;
            for (const auto& alarm : widget.alarms) {
                if (conditionMatches(alarm.condition, byIndex)) {
                    const auto current = byIndex.find(alarm.condition.index);
                    active.push_back(QStringLiteral("%1    %2    %3    %4    %5    %6")
                        .arg(serial++)
                        .arg(QString::fromUtf8("实时报警"))
                        .arg(QString::fromStdString(alarm.label))
                        .arg(QString::fromStdString(alarm.severity))
                        .arg(current == byIndex.end() ? QStringLiteral("--") : formatNumber(current->second.value))
                        .arg(QString::fromStdString(alarm.condition.expected)));
                }
            }
            const auto header = QString::fromUtf8("序号    报警类型    报警描述    报警级别    当前值    报警值");
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
        refreshTrend(widget, byIndex);
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
    return matchesScadaCondition(current->second.value, condition.comparison, condition.expected);
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
    if (widget.panel != nullptr) {
        const auto imageOnlyState = widget.stateImage != nullptr &&
            (widget.type == "statusLamp" || widget.type == "statusCard");
        widget.panel->setBrush(imageOnlyState ? QBrush(Qt::NoBrush) : QBrush(color));
    }
    if (widget.statusLamp != nullptr) widget.statusLamp->setBrush(matched == nullptr ? QColor("#71808A") : color);
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
        const auto label = !widget.stateLabelVisible
            ? std::string()
            : matched != nullptr && !matched->label.empty()
                ? matched->label
                : widget.defaultLabel.empty() ? std::string("--") : widget.defaultLabel;
        widget.valueText->setDefaultTextColor(matched == nullptr ? QColor("#AAB3BD") : QColor("#F3F8FA"));
        widget.valueText->setPlainText(QString::fromStdString(label));
        widget.lastText = label;
    }
}

void ScadaSceneView::refreshTrend(
    RuntimeWidget& widget,
    const std::unordered_map<std::uint32_t, edge_gateway::StoredPointValue>& values
) {
    if (widget.trendSeries.empty()) return;
    double minimum = std::numeric_limits<double>::max();
    double maximum = std::numeric_limits<double>::lowest();
    bool hasRenderableSeries = false;
    for (auto& series : widget.trendSeries) {
        const auto current = values.find(series.index);
        if (current != values.end() && current->second.quality == 1 && !current->second.stale && current->second.ts > 0 &&
            current->second.ts != series.lastSampleTs) {
            series.lastSampleTs = current->second.ts;
            series.samples.push_back(std::make_pair(current->second.ts, current->second.value));
            while (static_cast<int>(series.samples.size()) > widget.trendMaxPoints) series.samples.pop_front();
        }
        if (series.samples.size() < 2) continue;
        hasRenderableSeries = true;
        for (const auto& sample : series.samples) {
            minimum = std::min(minimum, sample.second);
            maximum = std::max(maximum, sample.second);
        }
    }
    if (!hasRenderableSeries) return;
    if (std::fabs(maximum - minimum) < 1e-9) {
        minimum -= 1.0;
        maximum += 1.0;
    }

    const auto padding = std::max(1e-6, (maximum - minimum) * 0.08);
    minimum -= padding;
    maximum += padding;

    std::int64_t firstTs = std::numeric_limits<std::int64_t>::max();
    std::int64_t lastTs = 0;
    for (const auto& series : widget.trendSeries) {
        if (series.samples.empty()) continue;
        firstTs = std::min(firstTs, series.samples.front().first);
        lastTs = std::max(lastTs, series.samples.back().first);
    }
    if (firstTs == std::numeric_limits<std::int64_t>::max()) return;

    for (std::size_t line = 0; line < widget.chartYLabels.size(); ++line) {
        const auto ratio = widget.chartYLabels.size() <= 1
            ? 0.0
            : static_cast<double>(line) / static_cast<double>(widget.chartYLabels.size() - 1);
        widget.chartYLabels[line]->setPlainText(formatNumber(maximum - (maximum - minimum) * ratio));
    }
    for (std::size_t line = 0; line < widget.chartXLabels.size(); ++line) {
        const auto ratio = widget.chartXLabels.size() <= 1
            ? 0.0
            : static_cast<double>(line) / static_cast<double>(widget.chartXLabels.size() - 1);
        const auto ts = firstTs + static_cast<std::int64_t>(static_cast<double>(lastTs - firstTs) * ratio);
        widget.chartXLabels[line]->setPlainText(
            QDateTime::fromMSecsSinceEpoch(ts).toString(QStringLiteral("HH:mm:ss"))
        );
    }

    for (auto& series : widget.trendSeries) {
        QPainterPath path;
        for (const auto& sample : series.samples) {
            const auto x = lastTs <= firstTs
                ? widget.chartX
                : widget.chartX + widget.chartWidth * static_cast<double>(sample.first - firstTs) /
                    static_cast<double>(lastTs - firstTs);
            const auto ratio = (sample.second - minimum) / (maximum - minimum);
            const auto y = widget.chartY + widget.chartHeight * (1.0 - ratio);
            if (path.elementCount() == 0) path.moveTo(x, y);
            else path.lineTo(x, y);
        }
        if (series.path != nullptr) series.path->setPath(path);
    }
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
            parts.push_back(resolveScadaValueLabel(widget.valueMap, value->second.value, &mapped)
                ? QString::fromStdString(mapped)
                : formatNumber(value->second.value));
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

void ScadaRuntimeWindow::showScreen(const std::string& screenId) {
    auto item = screenIndexes_.find(screenId);
    if (item != screenIndexes_.end()) {
        auto index = item->second;
        if (index < 0) {
            const auto screen = std::find_if(project_.screens.begin(), project_.screens.end(), [&](const auto& value) {
                return value.screenId == screenId;
            });
            if (screen == project_.screens.end()) return;
            index = buildScreen(*screen);
        }
        stack_->setCurrentIndex(index);
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
