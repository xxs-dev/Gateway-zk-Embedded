#pragma once

#include <functional>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <QGraphicsView>
#include <QStackedWidget>
#include <QWidget>

#include "edge_gateway/scada_models.hpp"
#include "edge_gateway/scada_runtime_map.hpp"

class QGraphicsEllipseItem;
class QGraphicsPathItem;
class QGraphicsPixmapItem;
class QGraphicsRectItem;
class QGraphicsScene;
class QGraphicsTextItem;
class QResizeEvent;
class QTimer;

class ScadaSceneView final : public QGraphicsView {
public:
    ScadaSceneView(
        const edge_gateway::ScadaScreen& screen,
        const std::string& projectRoot,
        const std::vector<edge_gateway::ScadaAlarm>& alarms,
        const std::vector<edge_gateway::ScadaTrend>& trends,
        edge_gateway::ScadaRuntimeMap& runtime,
        std::function<void(const std::string&)> navigate,
        QWidget* parent = nullptr
    );

    void refresh(std::int64_t nowMs);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
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
        QGraphicsPathItem* path = nullptr;
        std::int64_t lastSampleTs = 0;
        std::deque<std::pair<std::int64_t, double>> samples;
    };

    struct RuntimeWidget {
        std::string type;
        std::string defaultColor;
        std::string defaultImage;
        edge_gateway::ScadaWidgetAction action;
        std::vector<std::uint32_t> indexes;
        std::vector<RuntimeStateRule> stateRules;
        std::vector<RuntimeAlarm> alarms;
        QGraphicsRectItem* panel = nullptr;
        QGraphicsTextItem* valueText = nullptr;
        QGraphicsRectItem* progressFill = nullptr;
        QGraphicsEllipseItem* statusLamp = nullptr;
        QGraphicsPixmapItem* stateImage = nullptr;
        std::vector<RuntimeTrendSeries> trendSeries;
        double progressX = 0.0;
        double progressY = 0.0;
        double progressWidth = 0.0;
        double progressHeight = 0.0;
        double progressMax = 100.0;
        bool progressVertical = false;
        double chartX = 0.0;
        double chartY = 0.0;
        double chartWidth = 0.0;
        double chartHeight = 0.0;
        int trendMaxPoints = 120;
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
    void refreshTrend(
        RuntimeWidget& widget,
        const std::unordered_map<std::uint32_t, edge_gateway::StoredPointValue>& values
    );
    std::string property(const edge_gateway::ScadaWidget& widget, const std::string& key) const;
    double numericProperty(const edge_gateway::ScadaWidget& widget, const std::string& key, double fallback) const;
    QString valueText(const RuntimeWidget& widget, const std::unordered_map<std::uint32_t, edge_gateway::StoredPointValue>& values) const;

    edge_gateway::ScadaScreen screen_;
    std::string projectRoot_;
    std::vector<edge_gateway::ScadaAlarm> alarms_;
    std::vector<edge_gateway::ScadaTrend> trends_;
    edge_gateway::ScadaRuntimeMap& runtime_;
    std::function<void(const std::string&)> navigate_;
    QGraphicsScene* scene_ = nullptr;
    std::vector<RuntimeWidget> runtimeWidgets_;
};

class ScadaRuntimeWindow final : public QWidget {
public:
    ScadaRuntimeWindow(
        const std::string& projectDirectory,
        const std::string& machineCode,
        edge_gateway::PointStoreRouter& router,
        int refreshIntervalMs,
        bool autoReload,
        QWidget* parent = nullptr
    );

private:
    void reloadProject(bool initial);
    void rebuildScreens();
    void showScreen(const std::string& screenId);
    void refreshCurrent();
    std::string projectRevision() const;

    QStackedWidget* stack_ = nullptr;
    QTimer* timer_ = nullptr;
    std::string projectDirectory_;
    std::string machineCode_;
    edge_gateway::PointStoreRouter& router_;
    edge_gateway::ScadaProject project_;
    std::unique_ptr<edge_gateway::ScadaRuntimeMap> runtime_;
    std::string loadedRevision_;
    std::int64_t lastReloadCheckMs_ = 0;
    bool autoReload_ = false;
    std::vector<ScadaSceneView*> views_;
    std::unordered_map<std::string, int> screenIndexes_;
};
