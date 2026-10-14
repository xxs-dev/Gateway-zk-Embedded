#include "local_display_qt_scada_scene.hpp"
#include "local_display_qt_pcs_power_control.hpp"
#include "local_display_qt_parallel_topology.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <unordered_map>

#include <QApplication>
#include <QDateTime>
#include <QGraphicsTextItem>
#include <QPushButton>
#include <QTableWidget>
#include <QMenu>
#include <QLabel>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QGraphicsProxyWidget>
#include <QGraphicsRectItem>
#include <QStackedWidget>
#include <QTimer>
#include <QDialog>
#include <QMessageBox>
#include <QDir>

class ScadaSceneViewTestAccess {
public:
    static const auto& widgets(const ScadaSceneView& view) { return view.runtimeWidgets_; }
    static auto& widgets(ScadaSceneView& view) { return view.runtimeWidgets_; }
    static void render(ScadaSceneView& view) { for (auto& widget : view.runtimeWidgets_) view.renderTrend(widget); }
    static bool gated(const ScadaRuntimeWindow& window) { return window.showingAccessGate_; }
    static int stackCount(const ScadaRuntimeWindow& window) { return window.stack_->count(); }
    static int screenIndex(const ScadaRuntimeWindow& window, const std::string& id) { return window.screenIndexes_.at(id); }
    static void expire(ScadaRuntimeWindow& window) { window.enforceAccessExpiry(std::numeric_limits<std::int64_t>::max()); }
    static void stopTimer(ScadaRuntimeWindow& window) { window.timer_->stop(); }
    static ScadaSceneRuntimeSource& runtime(ScadaRuntimeWindow& window) { return *window.runtime_; }
    static bool expireWithoutRebuild(ScadaRuntimeWindow& window) {
        return window.accessSession_.authenticate("operator", "lanhu-preview-only", 1);
    }
    static void confirmedWrite(ScadaRuntimeWindow& window) {
        edge_gateway::ScadaWidgetAction action;
        action.type = "writeSetpoint";
        action.tagId = "t1";
        action.value = "1";
        action.requiresConfirmation = true;
        auto* view = dynamic_cast<ScadaSceneView*>(window.stack_->currentWidget());
        view->handleAction(action, {}, {}, false);
    }
};

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class FixtureRuntime final : public ScadaSceneRuntimeSource {
public:
    std::string node = "edge-1";
    std::string cluster;
    int writeCalls = 0;
    int groupWriteCalls = 0;
    std::string clusterStatusJson() const override { return cluster; }
    std::unordered_map<std::uint32_t, edge_gateway::StoredPointValue> values;
    std::unordered_map<std::uint32_t, std::string> devices;
    const std::string& nodeId() const override { return node; }
    edge_gateway::Optional<ScadaSceneResolvedTag> resolveTag(const std::string& tagId) const override {
        if (tagId.size() < 2 || tagId[0] != 't' || tagId[1] < '0' || tagId[1] > '9') return edge_gateway::NullOpt;
        ScadaSceneResolvedTag result;
        result.tag.tagId = tagId;
        result.tag.nodeId = node;
        result.tag.deviceId = "PCS";
        result.tag.displayName = "fixture alarm";
        result.mapping.index = static_cast<std::uint32_t>(std::stoul(tagId.substr(1)));
        const auto device = devices.find(result.mapping.index);
        if (device != devices.end()) result.tag.deviceId = device->second;
        result.mapping.writable = true;
        return result;
    }
    edge_gateway::Optional<edge_gateway::StoredPointValue> readTag(const std::string& tagId, std::int64_t) const override {
        const auto index = resolveTag(tagId)->mapping.index;
        const auto it = values.find(index);
        if (it == values.end()) return edge_gateway::NullOpt;
        return it->second;
    }
    std::vector<edge_gateway::StoredPointValue> readIndexes(const std::vector<std::uint32_t>& indexes, std::int64_t) const override {
        std::vector<edge_gateway::StoredPointValue> result;
        for (auto index : indexes) {
            const auto it = values.find(index);
            if (it != values.end()) result.push_back(it->second);
        }
        return result;
    }
    ScadaSceneWriteResult submitWrite(const std::string&, edge_gateway::PendingWriteCommand) override {
        ++writeCalls;
        return {true, "fixture accepted, not executed"};
    }
    ScadaSceneWriteResult submitWriteGroup(const std::vector<ScadaSceneWriteTarget>&, edge_gateway::PendingWriteCommand) override {
        ++groupWriteCalls;
        return {true, "fixture accepted, not executed"};
    }
    edge_gateway::Optional<edge_gateway::WritebackResultRecord> getWritebackResult(const std::string&, const std::string&) const override { return edge_gateway::NullOpt; }
    void put(std::uint32_t index, double value, bool stale = false, int quality = 1) {
        edge_gateway::StoredPointValue point;
        point.index = index;
        point.value = value;
        point.quality = quality;
        point.stale = stale;
        values[index] = point;
    }
};

edge_gateway::ScadaWidget widget(const std::string& type, double y = 0) {
    edge_gateway::ScadaWidget result;
    result.type = type;
    result.geometry = {0, y, 1600, 400};
    result.properties["qtTheme"] = "light";
    return result;
}
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        FixtureRuntime runtime;
        auto clusterFixture = [](int count, int offline, qint64 ts) {
            QJsonArray members;
            for (int i = 0; i < count; ++i) {
                members.append(QJsonObject{{"nodeId", QString("CAR-%1").arg(i + 1, 2, 10, QChar('0'))},
                    {"cabinetNo", i + 1}, {"online", i >= offline}, {"compatible", true},
                    {"duplicateIdentity", false}, {"capabilityFresh", true},
                    {"capability", QJsonObject{{"ready", true}, {"socPercent", 50 + i},
                        {"actual", QJsonArray{10 + i, 20 + i, 30 + i, 1, 2, 3}}}}});
            }
            return QJsonDocument(QJsonObject{{"schemaVersion", "2.0"}, {"ts", ts}, {"nodeId", "fixture"}, {"members", members}}).toJson().toStdString();
        };
        ParallelTopology topology(runtime, argc > 1 ? argv[1] : ".");
        topology.setFixedSize(1230, 653);
        topology.refresh(10000);
        require(!topology.fresh() && topology.vehicleCount() == 0, "missing membership became an invented vehicle");
        for (int count : {0, 1, 2, 3, 4, 5, 10}) {
            runtime.cluster = clusterFixture(count, 0, 10000);
            topology.refresh(10000);
            require(topology.fresh() && topology.vehicleCount() == count, "vehicle count differs from membership");
            require(topology.pageCount() == std::max(1, (count + 2) / 3), "wrong vehicle pagination");
        }
        topology.findChild<QPushButton*>("topologyNext")->click();
        require(topology.page() == 1, "next page did not work");
        runtime.cluster = clusterFixture(2, 0, 10001);
        topology.refresh(10001);
        require(topology.page() == 0 && topology.vehicleId(1) == "CAR-02", "shrink lost identity or page bounds");
        require(topology.valueText(0, "p") == "60.0" && topology.valueText(1, "p") == "63.0", "peer data duplicated local data");
        require(topology.valueText(1, "soc") == "51.0" && topology.valueText(1, "frequency") == "--", "invented peer telemetry");
        runtime.cluster = clusterFixture(2, 1, 10002);
        topology.refresh(10002);
        require(topology.vehicleCount() == 2 && topology.valueText(0, "p") == "--", "offline vehicle removed or live values retained");
        topology.refresh(16003);
        require(!topology.fresh() && topology.vehicleCount() == 2 && topology.valueText(1, "p") == "--", "stale snapshot not invalidated");
        runtime.cluster = "{";
        topology.refresh(16004);
        require(!topology.fresh() && topology.vehicleCount() == 2, "malformed snapshot erased membership");
        runtime.cluster = clusterFixture(3, 0, 50000);
        topology.refresh(16005);
        require(!topology.fresh() && topology.vehicleCount() == 2, "future snapshot accepted");
        auto duplicate = QJsonDocument::fromJson(QByteArray::fromStdString(clusterFixture(2, 0, 16006))).object();
        auto members = duplicate["members"].toArray(); members[1] = members[0]; duplicate["members"] = members;
        runtime.cluster = QJsonDocument(duplicate).toJson().toStdString(); topology.refresh(16006);
        require(!topology.fresh() && topology.vehicleCount() == 2, "duplicate node identity accepted");
        runtime.cluster = clusterFixture(0, 0, 16007); topology.refresh(16007);
        require(topology.fresh() && topology.vehicleCount() == 0, "valid zero membership not cleared");
        for (const QString& field : {QString("capabilityFresh"), QString("compatible"), QString("duplicateIdentity")}) {
            auto object = QJsonDocument::fromJson(QByteArray::fromStdString(clusterFixture(1, 0, 16008))).object();
            auto list = object["members"].toArray(); auto member = list[0].toObject();
            member[field] = field == "duplicateIdentity"; list[0] = member; object["members"] = list;
            runtime.cluster = QJsonDocument(object).toJson().toStdString(); topology.refresh(16008);
            require(topology.valueText(0, "soc") == "--" && topology.valueText(0, "p") == "--", "invalid peer quality accepted");
        }
        auto notReady = QJsonDocument::fromJson(QByteArray::fromStdString(clusterFixture(1, 0, 16009))).object();
        auto readyList = notReady["members"].toArray(); auto readyMember = readyList[0].toObject();
        auto capability = readyMember["capability"].toObject(); capability["ready"] = false;
        readyMember["capability"] = capability; readyList[0] = readyMember; notReady["members"] = readyList;
        runtime.cluster = QJsonDocument(notReady).toJson().toStdString(); topology.refresh(16009);
        require(topology.valueText(0, "soc") == "--", "not-ready capability became zero telemetry");
        runtime.cluster = clusterFixture(3, 0, 15000); topology.refresh(16009);
        require(!topology.fresh() && topology.vehicleCount() == 1, "out-of-order snapshot replaced newer roster");
        edge_gateway::ScadaScreen screen;
        auto clock = widget("qtLabel");
        clock.properties["qtClockFormat"] = "yyyy-MM-dd HH:mm:ss";
        auto count = widget("qtLabel");
        count.properties["alarmActiveCount"] = "true";
        auto table = widget("alarmTable", 100);
        auto toggle = widget("qtButton", 600);
        toggle.properties["qtToggleStyle"] = "true";
        toggle.action.type = "toggle";
        toggle.action.tagId = "t1";
        screen.widgets = {clock, count, table, toggle};
        edge_gateway::ScadaAlarm alarm;
        alarm.nodeId = "edge-1";
        alarm.tagId = "t1";
        alarm.comparison = "ne";
        alarm.threshold = "0";
        ScadaSceneView view(screen, ".", {alarm}, {}, runtime, [](const std::string&) {});
        const auto& widgets = ScadaSceneViewTestAccess::widgets(view);
        require(widgets.size() == 4, "runtime widgets missing");
        const auto now = QDateTime::currentMSecsSinceEpoch();
        view.refresh(now);
        require(widgets[0].valueText->toPlainText() == QDateTime::fromMSecsSinceEpoch(now).toString("yyyy-MM-dd HH:mm:ss"), "clock did not use supplied time");
        require(widgets[1].valueText->toPlainText() == "--", "missing data counted as zero alarms");
        require(widgets[2].alarmTable->item(0, 6)->text() == QString::fromUtf8("数据不可用"), "missing alarm data displayed as healthy");
        require(!widgets[3].toggleButton->isEnabled(), "unavailable toggle enabled");
        runtime.put(1, 1);
        view.refresh(now);
        require(widgets[1].valueText->toPlainText() == "1", "active alarm count incorrect");
        require(widgets[2].alarmTable->item(0, 6)->text() == QString::fromUtf8("活动"), "active alarm missing");
        require(widgets[3].toggleButton->isEnabled() && widgets[3].toggleButton->property("feedbackEnabled").toBool(), "toggle feedback incorrect");
        require(!widgets[3].toggleButton->isCheckable(), "toggle permits optimistic state");
        runtime.put(1, 0);
        view.refresh(now);
        require(widgets[1].valueText->toPlainText() == "0", "normal alarms counted active");
        require(!widgets[3].toggleButton->property("feedbackEnabled").toBool(), "toggle ignored off feedback");
        require(widgets[2].alarmTable->item(0, 3)->text() == QString::fromUtf8("暂无活动告警"), "empty alarm state incorrect");
        for (const auto value : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), 2.0}) {
            runtime.put(1, value);
            view.refresh(now);
            require(!widgets[3].toggleButton->isEnabled(), "nonbinary toggle feedback enabled control");
        }
        runtime.put(1, 1, true);
        view.refresh(now);
        require(widgets[1].valueText->toPlainText() == "--", "stale alarm counted as current");
        require(!widgets[3].toggleButton->isEnabled(), "stale toggle enabled");
        runtime.put(1, 1, false, 0);
        view.refresh(now);
        require(widgets[1].valueText->toPlainText() == "--", "bad quality alarm counted as current");

        auto chart = widget("qtChart");
        for (int i = 1; i <= 7; ++i) chart.bindings.push_back({"edge-1", "t" + std::to_string(i), "value"});
        screen.widgets = {chart};
        ScadaSceneView charts(screen, ".", {}, {}, runtime, [](const std::string&) {});
        const auto& plotted = ScadaSceneViewTestAccess::widgets(charts)[0];
        require(plotted.chartY >= 52, "two-row legend overlaps plot");
        require(plotted.chartWidth > 1500, "light chart retains dark sidebar gap");
        chart.properties.erase("qtTheme");
        screen.widgets = {chart};
        ScadaSceneView legacy(screen, ".", {}, {}, runtime, [](const std::string&) {});
        const auto& old = ScadaSceneViewTestAccess::widgets(legacy)[0];
        require(old.chartY == 30 && old.chartWidth == 1134, "legacy chart geometry changed");

        auto pcs = widget("pcsPhasePowerControl");
        screen.widgets = {pcs};
        ScadaSceneView controls(screen, ".", {}, {}, runtime, [](const std::string&) {});
        auto* control = ScadaSceneViewTestAccess::widgets(controls)[0].pcsPowerControl;
        require(control->styleSheet().contains("background:#FFFFFF") && !control->styleSheet().contains("background:#081E29"), "PCS light theme contains old dark rules");

        table.properties["qtAlarmPresentation"] = "lanhu";
        screen.widgets = {table};
        std::vector<edge_gateway::ScadaAlarm> alarms;
        for (int i = 1; i <= 20; ++i) { auto a = alarm; a.tagId = "t" + std::to_string(i); alarms.push_back(a); runtime.put(i, 1); }
        ScadaSceneView paged(screen, ".", alarms, {}, runtime, [](const std::string&) {});
        paged.refresh(now);
        const auto& p = ScadaSceneViewTestAccess::widgets(paged)[0];
        require(p.alarmTable->rowCount() == 8, "alarm page not limited to eight rows");
        require(p.alarmTable->horizontalHeaderItem(1)->text() == QString::fromUtf8("告警时间"), "Lanhu alarm header mismatch");
        require(p.alarmTable->item(0, 1)->text() == "--" && p.alarmTable->item(0, 6)->text() == "--", "fabricated alarm timestamps");
        p.alarmNext->click();
        require(p.alarmTable->item(0, 0)->text() == "9", "alarm page did not advance");
        p.alarmNext->click();
        require(p.alarmTable->rowCount() == 4 && !p.alarmNext->isEnabled(), "last alarm page incorrect");
        p.alarmPageButtons[0]->click();
        require(p.alarmTable->item(0, 0)->text() == "1", "numbered alarm page navigation failed");

        table.properties["qtAlarmPresentation"] = "summary";
        table.properties["qtAlarmDeviceIdsJson"] = R"(["BMS"])";
        runtime.devices[2] = "BMS";
        runtime.values[2].ts = now;
        screen.widgets = {table};
        ScadaSceneView summary(screen, ".", alarms, {}, runtime, [](const std::string&) {});
        summary.refresh(now);
        const auto& s = ScadaSceneViewTestAccess::widgets(summary)[0];
        require(s.alarms.size() == 1 && s.alarms[0].condition.index == 2, "summary leaks other device alarms");
        require(s.alarmTable->columnCount() == 2 && s.alarmTable->rowCount() == 1, "summary layout incorrect");
        require(s.alarmTable->item(0, 1)->text() == "--", "summary substitutes sample timestamp for occurrence");
        runtime.put(2, 0);
        summary.refresh(now);
        require(s.alarmTable->item(0, 0)->text() == QString::fromUtf8("暂无活动告警"), "summary normal state incorrect");
        runtime.put(2, 1, false, 0);
        summary.refresh(now);
        require(s.alarmTable->item(0, 0)->text().startsWith(QString::fromUtf8("数据不可用")), "summary bad quality shown healthy");
        runtime.put(2, 1, true);
        summary.refresh(now);
        require(s.alarmTable->item(0, 0)->text().startsWith(QString::fromUtf8("数据不可用")), "summary stale data shown healthy");
        for (const auto filter : {"[]", "broken", R"(["UNKNOWN"])"}) {
            table.properties["qtAlarmDeviceIdsJson"] = filter;
            screen.widgets = {table};
            ScadaSceneView reserved(screen, ".", alarms, {}, runtime, [](const std::string&) {});
            reserved.refresh(now);
            const auto& r = ScadaSceneViewTestAccess::widgets(reserved)[0];
            require(r.alarms.empty(), "empty or malformed alarm filter falls back to site");
            require(r.alarmTable->item(0, 0)->text() == QString::fromUtf8("告警未配置"), "unconfigured device claims healthy");
        }

        auto state = widget("statusCard");
        state.properties["qtBackgroundColor"] = "transparent";
        state.properties["stateColorMode"] = "indicator";
        state.properties["qtStateTextColor"] = "true";
        state.properties["qtHideStatusLamp"] = "true";
        state.properties["unavailableStateLabel"] = "--";
        state.bindings = {{"edge-1", "t1", "value"}};
        edge_gateway::ScadaStateRule stateRule;
        stateRule.code = "running"; stateRule.label = "running"; stateRule.color = "#18C773"; stateRule.match = "all";
        stateRule.conditions.push_back({"edge-1", "t1", "eq", "1"});
        state.stateRules = {stateRule}; screen.widgets = {state}; runtime.put(1, 1);
        ScadaSceneView stateView(screen, ".", {}, {}, runtime, [](const std::string&) {});
        stateView.refresh(now);
        const auto& stateWidget = ScadaSceneViewTestAccess::widgets(stateView)[0];
        require(stateWidget.valueText->defaultTextColor() == QColor("#18C773"), "monitor state text ignores rule color");
        require(stateWidget.panel->brush().color().alpha() == 0, "text state paints a badge background");
        runtime.put(1, 2); stateView.refresh(now);
        require(stateWidget.valueText->toPlainText() == "--", "unknown state enum claims running");

        chart.properties["qtTheme"] = "light";
        chart.properties["qtLegendAlignment"] = "Center";
        chart.properties["chartSecondaryUnit"] = "V";
        chart.properties["chartSeriesJson"] = R"([{"index":2,"name":"Voltage","unit":"V","color":"#18CC70"},{"index":1,"name":"Power","unit":"kW","color":"#F42B59"}])";
        chart.bindings = {{"edge-1", "t1", "value"}, {"edge-1", "t2", "value"}};
        screen.widgets = {chart};
        ScadaSceneView dual(screen, ".", {}, {}, runtime, [](const std::string&) {});
        auto& axes = ScadaSceneViewTestAccess::widgets(dual)[0];
        axes.chartDayStartTs = now - 100000; axes.chartLatestTs = now;
        axes.trendSeries[0].samples = {{now - 60000, 220}, {now, 240}};
        axes.trendSeries[1].samples = {{now - 60000, 1}, {now, 2}};
        ScadaSceneViewTestAccess::render(dual);
        require(axes.trendSeries[0].index == 2 && axes.trendSeries[0].secondaryAxis, "legend order or secondary unit wrong");
        require(axes.chartYLabels[0]->toPlainText().toDouble() < 3 && axes.chartSecondaryYLabels[0]->toPlainText().toDouble() > 240, "voltage shares power scale");
        require(!axes.trendSeries[0].path->path().isEmpty(), "secondary series not rendered");

        auto selector = widget("qtButton"); selector.action.type = "navigate"; selector.action.targetScreen = "Fallback";
        selector.properties["qtMenuScreensJson"] = R"([{"text":"Voltage","screen":"Trends-Voltage"}])";
        screen.widgets = {selector}; std::string destination;
        ScadaSceneView menuView(screen, ".", {}, {}, runtime, [&](const std::string& target) { destination = target; });
        QPushButton* menuButton = nullptr;
        for (auto* item : menuView.scene()->items()) {
            auto* proxy = dynamic_cast<QGraphicsProxyWidget*>(item);
            if (proxy != nullptr) menuButton = qobject_cast<QPushButton*>(proxy->widget());
            if (menuButton != nullptr) break;
        }
        require(menuButton != nullptr && menuButton->menu() != nullptr, "trend dropdown not installed");
        menuButton->menu()->actions().front()->trigger();
        require(destination == "Trends-Voltage", "dropdown navigation failed");

        pcs.properties["qtPresentation"] = "lanhu"; screen.widgets = {pcs};
        ScadaSceneView lanhuPcs(screen, ".", {}, {}, runtime, [](const std::string&) {});
        require(ScadaSceneViewTestAccess::widgets(lanhuPcs)[0].pcsPowerControl->findChildren<QLabel*>().size() > 10, "Lanhu PCS native rows missing");
        if (argc > 1) {
            ScadaRuntimeWindow window(argv[1], 500, false, [](const edge_gateway::ScadaProject&) {
                return std::make_unique<FixtureRuntime>();
            });
            ScadaSceneViewTestAccess::stopTimer(window);
            const auto& project = window.project();
            const auto topologyScreen = std::find_if(project.screens.begin(), project.screens.end(), [](const auto& s) { return s.screenId == "Topology"; });
            require(topologyScreen != project.screens.end(), "topology route missing");
            FixtureRuntime topologyRuntime;
            ScadaSceneView topologyView(*topologyScreen, argv[1], project.alarms, project.trends, topologyRuntime, [](const std::string&) {});
            topologyView.setWindowFlag(Qt::FramelessWindowHint);
            topologyView.resize(1440, 810); topologyView.show(); app.processEvents();
            for (const auto& scenario : std::vector<std::pair<int, int>>{{0,0},{1,0},{2,0},{3,0},{3,1},{3,2},{3,3},{5,0}}) {
                const auto now = QDateTime::currentMSecsSinceEpoch();
                topologyRuntime.cluster = clusterFixture(scenario.first, scenario.second, now);
                topologyView.refresh(now); app.processEvents();
                if (argc > 2) {
                    QDir().mkpath(argv[2]);
                    const auto path = QString::fromLocal8Bit(argv[2]) + QString("/Topology-%1-offline-%2.png").arg(scenario.first).arg(scenario.second);
                    require(topologyView.grab().scaled(1440, 810, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).save(path), "topology screenshot failed");
                }
            }
            for (auto& item : ScadaSceneViewTestAccess::widgets(topologyView)) if (item.parallelTopology) {
                item.parallelTopology->findChild<QPushButton*>("topologyNext")->click();
                require(item.parallelTopology->page() == 1, "scene pagination failed");
            }
            app.processEvents();
            if (argc > 2) require(topologyView.grab().scaled(1440, 810, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).save(QString::fromLocal8Bit(argv[2]) + "/Topology-5-page2.png"), "page2 screenshot failed");
            topologyRuntime.cluster.clear(); topologyView.refresh(QDateTime::currentMSecsSinceEpoch()); app.processEvents();
            if (argc > 2) require(topologyView.grab().scaled(1440, 810, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).save(QString::fromLocal8Bit(argv[2]) + "/Topology-stale.png"), "stale screenshot failed");
            topologyView.close();
            window.setWindowFlag(Qt::FramelessWindowHint);
            window.resize(1440, 810); window.show(); app.processEvents();
            window.showScreen("Control-Pcs"); app.processEvents();
            require(ScadaSceneViewTestAccess::gated(window), "unauthenticated control has no gate");
            require(ScadaSceneViewTestAccess::screenIndex(window, "Control-Pcs") < 0, "protected controls instantiated before authentication");
            auto count = ScadaSceneViewTestAccess::stackCount(window);
            ScadaSceneViewTestAccess::expire(window);
            require(ScadaSceneViewTestAccess::stackCount(window) == count, "unauthenticated gate rebuilt on every refresh");
            if (argc > 2) {
                QDir().mkpath(argv[2]);
                require(window.grab().scaled(1440, 810, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).save(QString::fromLocal8Bit(argv[2]) + "/Control-LoggedOut.png"), "gate screenshot failed");
            }
            QTimer::singleShot(0, []() { qobject_cast<QDialog*>(QApplication::activeModalWidget())->reject(); });
            window.showScreen("__local_login__");
            require(ScadaSceneViewTestAccess::gated(window), "cancel exposed protected content");
            std::string message;
            require(window.authenticateLocalAccess("operator", "lanhu-preview-only", &message), "preview credentials failed");
            window.showScreen("Control-Pcs");
            require(!ScadaSceneViewTestAccess::gated(window), "authenticated control stayed gated");
            require(ScadaSceneViewTestAccess::screenIndex(window, "Control-Pcs") >= 0, "authenticated screen not built");
            auto& guarded = ScadaSceneViewTestAccess::runtime(window);
            auto& transport = dynamic_cast<FixtureRuntime&>(guarded);
            require(guarded.submitOperatorWrite("t1", {}).accepted, "valid operator write was blocked");
            require(guarded.submitOperatorWriteGroup({{"t1", 1}}, {}).accepted, "valid operator group was blocked");
            const auto writesBefore = transport.writeCalls;
            const auto groupsBefore = transport.groupWriteCalls;
            bool confirmedAfterExpiry = false;
            QTimer responder;
            QObject::connect(&responder, &QTimer::timeout, &window, [&]() {
                auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                if (!dialog) return;
                if (dialog->standardButtons() & QMessageBox::Yes) {
                    confirmedAfterExpiry = ScadaSceneViewTestAccess::expireWithoutRebuild(window);
                    dialog->button(QMessageBox::Yes)->click();
                } else dialog->accept();
            });
            responder.start(10);
            ScadaSceneViewTestAccess::confirmedWrite(window);
            responder.stop();
            require(confirmedAfterExpiry, "confirmation expiry scenario not reached");
            require(transport.writeCalls == writesBefore, "expired confirmation reached write transport");
            require(!guarded.submitOperatorWriteGroup({{"t1", 1}}, {}).accepted,
                    "expired operator group was accepted");
            require(transport.groupWriteCalls == groupsBefore, "expired group reached write transport");
            require(guarded.submitWrite("t1", {}).accepted,
                    "accepted pulse cleanup transport was blocked by operator expiry");
            ScadaSceneViewTestAccess::expire(window);
            require(ScadaSceneViewTestAccess::gated(window), "expired session did not return to gate");
            require(ScadaSceneViewTestAccess::screenIndex(window, "Control-Pcs") < 0, "expired controls remain built");
            require(window.authenticateLocalAccess("operator", "lanhu-preview-only", &message), "second login failed");
            window.showScreen("Meters-Overview");
            auto* header = window.findChild<QPushButton*>("scadaLogoutButton");
            require(header && header->isVisible() && header->text() == QString::fromUtf8("退出"), "public screen loses logged-in state");
            header->click();
            require(!guarded.submitOperatorWrite("t1", {}).accepted, "logout allowed operator write");
            window.showScreen("Strategy-Overview");
            require(ScadaSceneViewTestAccess::gated(window), "logout did not revoke strategy access");
            window.showScreen("Guide-Setup");
            require(ScadaSceneViewTestAccess::gated(window), "guide authentication regressed");
        }
        std::cout << "local_display_qt_lanhu_test passed\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "local_display_qt_lanhu_test failed: " << ex.what() << '\n';
        return 1;
    }
}
