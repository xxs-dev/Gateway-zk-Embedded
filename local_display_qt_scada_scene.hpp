#pragma once

#include <functional>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <QGraphicsView>
#include <QPointF>
#include <QStackedWidget>
#include <QWidget>

#include "edge_gateway/scada_models.hpp"
#include "local_display_qt_access_control.hpp"
#include "local_display_qt_scada_runtime.hpp"
#include "local_display_qt_value_map.hpp"

class QGraphicsEllipseItem;
class QGraphicsLineItem;
class QGraphicsPathItem;
class QGraphicsPixmapItem;
class QGraphicsRectItem;
class QGraphicsScene;
class QGraphicsTextItem;
class QEvent;
class QMouseEvent;
class QObject;
class QPushButton;
class QResizeEvent;
class QTimer;
class QWheelEvent;
class PcsPhasePowerControl;
class ScadaSceneViewTestAccess;

class ScadaSceneView final : public QGraphicsView {
public:
    ScadaSceneView(
        const edge_gateway::ScadaScreen& screen,
        const std::string& projectRoot,
        const std::vector<edge_gateway::ScadaAlarm>& alarms,
        const std::vector<edge_gateway::ScadaTrend>& trends,
        ScadaSceneRuntimeSource& runtime,
        std::function<void(const std::string&)> navigate,
        QWidget* parent = nullptr
    );

    void refresh(std::int64_t nowMs);
    void sampleTrends(std::int64_t nowMs);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    friend class ScadaSceneViewTestAccess;

    struct RuntimeCondition {
        std::uint32_t index = 0;
        std::string comparison;
        std::string expected;
    };

    struct RuntimeStateRule {
        std::string code;
        std::string label;
        std::string color;
        std::string image;
        int priority = 0;
        bool matchAny = false;
        std::vector<RuntimeCondition> conditions;
    };

    struct RuntimeAlarm {
        std::string label;
        std::string severity;
        RuntimeCondition condition;
    };

    struct RuntimeTrendSeries {
        std::uint32_t index = 0;
        std::string name;
        std::string unit;
        QGraphicsPathItem* path = nullptr;
        QGraphicsLineItem* legendSwatch = nullptr;
        QGraphicsTextItem* legendText = nullptr;
        bool visible = true;
        std::int64_t lastSampleTs = 0;
        std::int64_t lastBucketTs = 0;
        std::deque<std::pair<std::int64_t, double>> samples;
    };

    struct RuntimeWidget {
        std::string type;
        std::string defaultLabel;
        std::string defaultColor;
        std::string defaultImage;
        std::string valueSuffix;
        std::string inputTagId;
        edge_gateway::Optional<double> inputMinValue;
        edge_gateway::Optional<double> inputMaxValue;
        double inputStep = 0.0;
        bool inputWritable = false;
        edge_gateway::ScadaWidgetAction action;
        std::vector<std::uint32_t> indexes;
        ScadaValueMap valueMap;
        std::vector<RuntimeStateRule> stateRules;
        std::vector<RuntimeAlarm> alarms;
        QGraphicsRectItem* panel = nullptr;
        QGraphicsTextItem* valueText = nullptr;
        QGraphicsRectItem* progressFill = nullptr;
        std::vector<QGraphicsRectItem*> signalBars;
        QGraphicsEllipseItem* statusLamp = nullptr;
        QGraphicsPixmapItem* stateImage = nullptr;
        QGraphicsLineItem* flowLine = nullptr;
        QGraphicsPathItem* flowArrow = nullptr;
        QGraphicsTextItem* flowValueText = nullptr;
        std::vector<QGraphicsEllipseItem*> flowParticles;
        PcsPhasePowerControl* pcsPowerControl = nullptr;
        std::vector<RuntimeTrendSeries> trendSeries;
        std::vector<QGraphicsTextItem*> chartYLabels;
        std::vector<QGraphicsTextItem*> chartXLabels;
        double progressX = 0.0;
        double progressY = 0.0;
        double progressWidth = 0.0;
        double progressHeight = 0.0;
        double progressMax = 100.0;
        bool progressVertical = false;
        bool stateLabelVisible = true;
        bool stateColorPanel = true;
        bool flowForwardWhenPositive = true;
        bool flowValueGood = false;
        double flowValue = 0.0;
        double flowDeadband = 0.2;
        double flowRatedPower = 30.0;
        QPointF flowStart;
        QPointF flowEnd;
        std::string flowColor;
        std::string flowIdleColor;
        double chartX = 0.0;
        double chartY = 0.0;
        double chartWidth = 0.0;
        double chartHeight = 0.0;
        int trendMaxPoints = 120;
        std::int64_t chartDayStartTs = 0;
        std::int64_t chartLatestTs = 0;
        std::int64_t chartViewStartTs = 0;
        std::int64_t chartViewEndTs = 0;
        std::int64_t chartDefaultWindowMs = 2 * 60 * 60 * 1000;
        std::int64_t chartViewWindowMs = 2 * 60 * 60 * 1000;
        std::int64_t chartMinWindowMs = 5 * 60 * 1000;
        std::int64_t chartSampleIntervalMs = 30 * 1000;
        bool chartFollowLatest = true;
        std::string lastText;
        std::string lastVisualCode;
    };

    void buildScene();
    void handleAction(const edge_gateway::ScadaWidgetAction& action);
    bool conditionMatches(
        const RuntimeCondition& condition,
        const std::unordered_map<std::uint32_t, edge_gateway::StoredPointValue>& values
    ) const;
    void refreshState(
        RuntimeWidget& widget,
        const std::unordered_map<std::uint32_t, edge_gateway::StoredPointValue>& values
    );
    void refreshEnergyFlow(
        RuntimeWidget& widget,
        const std::unordered_map<std::uint32_t, edge_gateway::StoredPointValue>& values,
        std::int64_t nowMs
    );
    void animateEnergyFlow(RuntimeWidget& widget, std::int64_t nowMs);
    void sampleTrend(
        RuntimeWidget& widget,
        const std::unordered_map<std::uint32_t, edge_gateway::StoredPointValue>& values,
        std::int64_t nowMs
    );
    void renderTrend(RuntimeWidget& widget);
    RuntimeWidget* trendWidgetAt(const QPoint& viewportPosition);
    RuntimeWidget* inputWidgetAt(const QPoint& viewportPosition);
    RuntimeTrendSeries* trendLegendSeriesAt(const QPoint& viewportPosition);
    void toggleTrendSeries(RuntimeTrendSeries& series);
    void panTrend(RuntimeWidget& widget, double sceneDeltaX);
    void editInput(RuntimeWidget& widget);
    std::string property(const edge_gateway::ScadaWidget& widget, const std::string& key) const;
    double numericProperty(const edge_gateway::ScadaWidget& widget, const std::string& key, double fallback) const;
    QString valueText(const RuntimeWidget& widget, const std::unordered_map<std::uint32_t, edge_gateway::StoredPointValue>& values) const;

    edge_gateway::ScadaScreen screen_;
    std::string projectRoot_;
    std::vector<edge_gateway::ScadaAlarm> alarms_;
    std::vector<edge_gateway::ScadaTrend> trends_;
    ScadaSceneRuntimeSource& runtime_;
    std::function<void(const std::string&)> navigate_;
    QGraphicsScene* scene_ = nullptr;
    QTimer* flowAnimationTimer_ = nullptr;
    std::vector<RuntimeWidget> runtimeWidgets_;
    RuntimeWidget* panningTrend_ = nullptr;
    QPointF lastPanScenePosition_;
};

class ScadaRuntimeWindow final : public QWidget {
public:
    ScadaRuntimeWindow(
        const std::string& projectDirectory,
        int refreshIntervalMs,
        bool autoReload,
        std::function<std::unique_ptr<ScadaSceneRuntimeSource>(const edge_gateway::ScadaProject&)> runtimeFactory,
        QWidget* parent = nullptr
    );

    const edge_gateway::ScadaProject& project() const { return project_; }
    bool screenRequiresLocalAuthentication(const std::string& screenId) const;
    bool authenticateLocalAccess(
        const std::string& username,
        const std::string& password,
        std::string* message = nullptr
    );
    void showScreen(const std::string& screenId);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    friend class ScadaSceneViewTestAccess;

    void reloadProject(bool initial);
    void rebuildScreens();
    int buildScreen(const edge_gateway::ScadaScreen& screen);
    void refreshCurrent();
    void refreshAll(std::int64_t nowMs);
    void enforceAccessExpiry(std::int64_t nowMs);
    void updateAccessUi();
    void logoutLocalUser();
    std::string projectRevision() const;

    QStackedWidget* stack_ = nullptr;
    QTimer* timer_ = nullptr;
    std::string projectDirectory_;
    edge_gateway::ScadaProject project_;
    std::unique_ptr<ScadaSceneRuntimeSource> runtime_;
    std::function<std::unique_ptr<ScadaSceneRuntimeSource>(const edge_gateway::ScadaProject&)> runtimeFactory_;
    std::string loadedRevision_;
    std::int64_t lastReloadCheckMs_ = 0;
    bool autoReload_ = false;
    ScadaLocalAccessSession accessSession_;
    QPushButton* logoutButton_ = nullptr;
    std::string currentScreenId_;
    std::vector<ScadaSceneView*> views_;
    std::unordered_map<std::string, int> screenIndexes_;
};
