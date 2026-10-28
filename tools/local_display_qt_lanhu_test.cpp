#include "local_display_qt_scada_scene.hpp"
#include "local_display_qt_pcs_power_control.hpp"
#include "local_display_qt_parallel_topology.hpp"
#include "local_display_qt_system_info.hpp"

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
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGraphicsProxyWidget>
#include <QGraphicsRectItem>
#include <QStackedWidget>
#include <QTimer>
#include <QDialog>
#include <QMessageBox>
#include <QInputDialog>
#include <QDir>
#include <QFile>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QTextDocument>
#include <QTextBlock>
#include <QTextLayout>
#include <QTemporaryDir>
#include <QThread>

class ScadaSceneViewTestAccess {
public:
    static void control(ScadaSceneView& view, const edge_gateway::ScadaWidget& widget) {
        view.handleAction(widget.action, {}, {}, false, widget);
    }
    static bool pending(const ScadaSceneView& view) { return view.controlInFlight_; }
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
    std::string machine;
    std::map<std::string, std::string> systemInfo;
    int writeCalls = 0;
    int groupWriteCalls = 0;
    int modeChanges = 0;
    edge_gateway::PendingWriteCommand lastCommand;
    edge_gateway::Optional<edge_gateway::WritebackResultRecord> acknowledgement;
    ScadaSceneWriteResult setEmsControlMode(bool remote) override {
        ++modeChanges;
        systemInfo["controlModeCode"] = remote ? "remote" : "local";
        return {true, "fixture mode saved"};
    }
    std::string clusterStatusJson() const override { return cluster; }
    std::unordered_map<std::uint32_t, edge_gateway::StoredPointValue> values;
    std::unordered_map<std::uint32_t, std::string> devices;
    std::unordered_map<std::string, ScadaSceneResolvedTag> projectTags;
    const std::string& nodeId() const override { return node; }
    std::string machineCode() const override { return machine.empty() ? node : machine; }
    std::map<std::string, std::string> localSystemInfo() const override { return systemInfo; }
    edge_gateway::Optional<ScadaSceneResolvedTag> resolveTag(const std::string& tagId) const override {
        const auto configured = projectTags.find(tagId);
        if (configured != projectTags.end()) return configured->second;
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
        const auto resolved = resolveTag(tagId);
        if (!resolved) return edge_gateway::NullOpt;
        const auto index = resolved->mapping.index;
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
    ScadaSceneWriteResult submitWrite(const std::string&, edge_gateway::PendingWriteCommand command) override {
        ++writeCalls;
        lastCommand = command;
        return {true, "fixture accepted, not executed"};
    }
    ScadaSceneWriteResult submitWriteGroup(const std::vector<ScadaSceneWriteTarget>&, edge_gateway::PendingWriteCommand) override {
        ++groupWriteCalls;
        return {true, "fixture accepted, not executed"};
    }
    edge_gateway::Optional<edge_gateway::WritebackResultRecord> getWritebackResult(const std::string&, const std::string&) const override { return acknowledgement; }
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
        {
            FixtureRuntime controls;
            bool authorized = true;
            controls.setOperatorAuthorization([&] { return authorized; });
            edge_gateway::ScadaScreen screen;
            screen.width = 400; screen.height = 100;
            ScadaSceneView view(screen, ".", {}, {}, controls, [](const std::string&) {});
            auto command = widget("qtButton");
            command.title = "fixture control";
            command.action.type = "writeSetpoint";
            command.action.tagId = "t1";
            command.action.requiresConfirmation = true;
            command.properties["controlOptionsJson"] = R"([{"label":"Auto","value":0,"feedbackValue":4},{"label":"Pump","value":3,"feedbackValue":1}])";
            command.properties["controlFeedbackTag"] = "t2";
            bool cancel = true, expire = false;
            QTimer responder;
            QObject::connect(&responder, &QTimer::timeout, [&] {
                if (auto* input = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
                    input->setTextValue("Pump");
                    if (cancel) input->reject(); else input->accept();
                } else if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                    if (box->standardButtons() & QMessageBox::Yes) {
                        require(box->text().contains("Pump"), "confirmation lost enum meaning");
                        if (expire) authorized = false;
                        box->button(QMessageBox::Yes)->click();
                    } else if (box->windowTitle() != QString::fromUtf8("控制执行结果")) box->accept();
                }
            });
            responder.start(10);
            ScadaSceneViewTestAccess::control(view, command);
            require(controls.writeCalls == 0, "cancelled enum submitted command");
            cancel = false; expire = true;
            ScadaSceneViewTestAccess::control(view, command);
            require(controls.writeCalls == 0, "authorization expiry during confirmation submitted command");
            expire = false; authorized = true;
            ScadaSceneViewTestAccess::control(view, command);
            require(controls.writeCalls == 1 && controls.lastCommand.value == 3, "enum meaning mapped to wrong protocol value");
            responder.stop();
            auto spin = [&](int milliseconds) {
                const auto until = QDateTime::currentMSecsSinceEpoch() + milliseconds;
                while (QDateTime::currentMSecsSinceEpoch() < until) { app.processEvents(); QThread::msleep(5); }
            };
            auto receiptText = [&] {
                const auto boxes = view.findChildren<QMessageBox*>();
                require(!boxes.empty(), "missing execution feedback dialog");
                return boxes.back()->text();
            };
            edge_gateway::WritebackResultRecord ack;
            ack.success = true; ack.completedAt = controls.lastCommand.ts + 1;
            controls.acknowledgement = ack;
            controls.put(2, 1); controls.values[2].ts = controls.lastCommand.ts - 1;
            spin(250);
            require(ScadaSceneViewTestAccess::pending(view), "old matching feedback falsely confirmed command");
            controls.values[2].ts = ack.completedAt + 1;
            controls.values[2].value = 3;
            spin(250);
            require(ScadaSceneViewTestAccess::pending(view), "write enum incorrectly reused for readback enum");
            controls.values[2].value = 1; controls.values[2].stale = true;
            spin(250);
            require(ScadaSceneViewTestAccess::pending(view), "stale feedback confirmed command");
            controls.values[2].stale = false; spin(250);
            require(!ScadaSceneViewTestAccess::pending(view) && receiptText().contains(QString::fromUtf8("新鲜反馈")), "fresh readback not confirmed");
            for (auto* box : view.findChildren<QMessageBox*>()) box->close();
            app.processEvents();
            command.properties.erase("controlOptionsJson");
            command.action.value = "0"; command.action.requiresConfirmation = false;
            ack.success = false; ack.message = "fixture transport error"; controls.acknowledgement = ack;
            ScadaSceneViewTestAccess::control(view, command); spin(250);
            require(!ScadaSceneViewTestAccess::pending(view) && receiptText().contains("fixture transport error"), "driver failure not reported");
            for (auto* box : view.findChildren<QMessageBox*>()) box->close();
            app.processEvents();
            controls.acknowledgement = edge_gateway::NullOpt;
            ScadaSceneViewTestAccess::control(view, command); spin(10300);
            require(!ScadaSceneViewTestAccess::pending(view) && receiptText().contains(QString::fromUtf8("超时")) && controls.writeCalls == 3, "timeout retried command or hid uncertainty");
        }
        {
            FixtureRuntime modeRuntime;
            require(!modeRuntime.setOperatorEmsControlMode(false).accepted && modeRuntime.modeChanges == 0, "EMS mode changed without authorization callback");
            bool authorized = true;
            modeRuntime.setOperatorAuthorization([&] { return authorized; });
            require(modeRuntime.setOperatorEmsControlMode(false).accepted && modeRuntime.modeChanges == 1, "authorized EMS mode change rejected");
            authorized = false;
            require(!modeRuntime.setOperatorEmsControlMode(true).accepted && modeRuntime.modeChanges == 1, "expired session changed EMS mode");
        }
        {
            FixtureRuntime modeRuntime;
            modeRuntime.systemInfo["controlModeCode"] = "remote";
            bool authorized = true;
            modeRuntime.setOperatorAuthorization([&] { return authorized; });
            edge_gateway::ScadaScreen modeScreen;
            modeScreen.width = 400; modeScreen.height = 100;
            for (const auto& target : {std::string("remote"), std::string("local")}) {
                auto b = widget("qtButton");
                b.widgetId = target;
                b.title = target;
                b.geometry = {target == "remote" ? 0.0 : 200.0, 0, 180, 60};
                b.properties["emsControlModeTarget"] = target;
                modeScreen.widgets.push_back(b);
            }
            ScadaSceneView modeView(modeScreen, ".", {}, {}, modeRuntime, [](const std::string&) {});
            modeView.refresh(QDateTime::currentMSecsSinceEpoch());
            const auto& modes = ScadaSceneViewTestAccess::widgets(modeView);
            require(modes.size() == 2, "EMS selectors missing");
            auto* remoteButton = modes[0].toggleButton;
            auto* localButton = modes[1].toggleButton;
            require(remoteButton->isChecked() && !remoteButton->isEnabled() && localButton->isEnabled(), "default remote selector incorrect");
            bool confirm = false, expire = false;
            QTimer responder;
            QObject::connect(&responder, &QTimer::timeout, [&] {
                auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                if (!dialog) return;
                if (dialog->standardButtons() & QMessageBox::Yes) {
                    if (expire) authorized = false;
                    dialog->button(confirm ? QMessageBox::Yes : QMessageBox::No)->click();
                } else dialog->accept();
            });
            responder.start(10);
            localButton->click();
            require(modeRuntime.modeChanges == 0 && remoteButton->isChecked() && !localButton->isChecked(), "cancel changed EMS mode");
            confirm = true;
            localButton->click();
            require(modeRuntime.modeChanges == 1 && localButton->isChecked() && !localButton->isEnabled(), "confirmed local state not refreshed");
            expire = true;
            remoteButton->click();
            require(modeRuntime.modeChanges == 1 && localButton->isChecked(), "expired confirmation changed EMS mode");
            responder.stop();
            modeRuntime.systemInfo["controlModeCode"] = "unknown";
            modeView.refresh(QDateTime::currentMSecsSinceEpoch());
            require(!remoteButton->isEnabled() && !localButton->isEnabled(), "invalid state permits switching");
            require(modeRuntime.writeCalls == 0 && modeRuntime.groupWriteCalls == 0, "EMS selector sent equipment command");
        }
        {
            QTemporaryDir host;
            require(host.isValid(), "host fixture directory failed");
            auto save = [&](const QString& path, const QByteArray& data) {
                QDir().mkpath(QFileInfo(host.path() + path).path());
                QFile file(host.path() + path);
                require(file.open(QIODevice::WriteOnly) && file.write(data) == data.size(), "host fixture write failed");
            };
            save("/config/runtime/device_identity.json", R"({"model":"EDGE-GW","firmwareVersion":"1.0.0"})");
            save("/config/runtime/apps/mqtt-service.json", R"({"mqtt":{"enabled":true}})");
            save("/proc/stat", "cpu 10 0 10 80 0 0 0 0\n");
            save("/proc/meminfo", "MemTotal: 1000 kB\nMemAvailable: 750 kB\n");
            save("/sys/class/thermal/thermal_zone0/temp", "42500\n");
            LocalSystemInfo source(host.path(), host.path()+"/proc", host.path()+"/sys");
            auto first = source.read("COMM202600104");
            require(first.at("machineCode") == "COMM202600104" && first.at("model") == "EDGE-GW", "EMU identity not from host");
            require(first.at("memory") == "25.0 %" && first.at("temperature").find("42.5") == 0, "EMU resource units incorrect");
            require(first.count("cpu") == 0, "CPU first sample invents usage");
            require(first.at("cloud") == QString::fromUtf8("已启用").toStdString(), "cloud configuration mislabeled");
            save("/proc/stat", "cpu 40 0 30 130 0 0 0 0\n");
            QThread::msleep(1010);
            require(source.read("COMM202600104").at("cpu") == "50.0 %", "CPU must use counter deltas");
            QFile::remove(host.path()+"/proc/meminfo");
            QFile::remove(host.path()+"/sys/class/thermal/thermal_zone0/temp");
            QThread::msleep(1010);
            auto missing = source.read("COMM202600104");
            require(!missing.count("memory") && !missing.count("temperature"), "EMU retained stale host metrics");
        }
        {
            FixtureRuntime host;
            host.machine = "COMM202600104";
            QJsonObject tags{{"p","t1"},{"q","t2"},{"soc","t3"},{"online","t4"},{"bmsOnline","t5"},{"breaker","t6"},{"frequency","t7"},{"ua","t8"}};
            host.put(1,12.3); host.put(2,-2); host.put(3,96); host.put(4,1); host.put(5,1); host.put(6,1); host.put(7,50); host.put(8,230);
            ParallelTopology local(host, ".", tags);
            local.refresh(10000);
            require(local.vehicleId(0) == "COMM202600104" && !local.fresh(), "local identity confused with SCADA node or cluster status");
            require(local.valueText(0,"p") == "12.3" && local.valueText(0,"soc") == "96.0" && local.valueText(0,"ua") == "230.0", "valid local data requires cluster membership");
            require(local.valueText(0,"breaker") == QString::fromUtf8("合闸") && local.valueText(0,"frequency") == "50.00", "local feedback binding missing");
            host.put(1,100,true); host.put(3,96,false,0); local.refresh(10001);
            require(local.valueText(0,"p") == "--" && local.valueText(0,"soc") == "--", "invalid local quality shown live");
            host.put(4,0); local.refresh(10002);
            require(local.valueText(0,"q") == "--", "offline PCS data shown live");
            host.values.clear(); local.refresh(10003);
            require(local.valueText(0,"breaker") == "--", "local stale cache retained");
            require(host.writeCalls == 0 && host.groupWriteCalls == 0, "local topology writes control");
            host.systemInfo["machineCode"] = host.machine;
            edge_gateway::ScadaScreen emuScreen;
            auto field = widget("qtLabel"); field.properties["localSystemField"] = "machineCode";
            emuScreen.widgets = {field};
            ScadaSceneView emu(emuScreen,".",{}, {},host,[](const std::string&){});
            emu.refresh(10000);
            require(ScadaSceneViewTestAccess::widgets(emu)[0].valueText->toPlainText() == "COMM202600104", "EMU field not refreshed");
            host.systemInfo.clear(); emu.refresh(10001);
            require(ScadaSceneViewTestAccess::widgets(emu)[0].valueText->toPlainText() == "--", "EMU missing source retains old value");
        }
        // Audit the real renderer without connecting any data/control transport.
        if (argc > 3 && std::string(argv[3]) == "--layout-audit") {
            ScadaRuntimeWindow window(argv[1], 500, false, [](const edge_gateway::ScadaProject&) {
                return std::make_unique<FixtureRuntime>();
            });
            ScadaSceneViewTestAccess::stopTimer(window);
            const auto& project = window.project();
            for (const auto& mapping : project.runtimeMappings) {
                ScadaSceneResolvedTag tag; tag.mapping = mapping;
                for (const auto& candidate : project.tags) if (candidate.tagId == mapping.tagId) tag.tag = candidate;
                runtime.projectTags[mapping.tagId] = tag;
            }
            QDir().mkpath(argv[2]);
            QJsonArray issues, fonts;
            int renders = 0;
            for (const auto& screen : project.screens) {
                ScadaSceneView page(screen, argv[1], project.alarms, project.trends, runtime, [](const std::string&) {});
                page.setWindowFlag(Qt::FramelessWindowHint);
                for (const auto& size : {QSize(1440,810), QSize(1920,1080), QSize(1280,720), QSize(1280,1024)}) {
                    page.resize(size); page.show(); app.processEvents();
                    require(std::abs(page.transform().m11() - page.transform().m22()) < 0.000001, "nonuniform canvas scale");
                    for (int sample = 0; sample < 2; ++sample) {
                        runtime.values.clear();
                        if (sample) for (const auto& mapping : project.runtimeMappings) runtime.put(mapping.index, -123456.78);
                        page.refresh(1792036800000); app.processEvents(); ++renders;
                        if (size == QSize(1920,1080)) {
                            const auto path = QString::fromLocal8Bit(argv[2]) + "/" + QString::fromStdString(screen.screenId)
                                + (sample ? "-values.png" : "-missing.png");
                            require(page.grab().save(path), "layout screenshot failed");
                        }
                        if (size != QSize(1920,1080)) continue;
                        std::vector<std::pair<QString, QRectF>> textLines;
                        for (auto* sceneItem : page.scene()->items()) {
                            auto* valueText = dynamic_cast<QGraphicsTextItem*>(sceneItem);
                            if (!valueText || !valueText->isVisible() || valueText->toPlainText().isEmpty()) continue;
                            const auto id = valueText->data(0).toString().toStdString();
                            const auto source = std::find_if(screen.widgets.begin(), screen.widgets.end(), [&](const auto& w) { return w.widgetId == id; });
                            if (source == screen.widgets.end() || (source->type != "qtLabel" && source->type != "qtValue" && source->type != "qtInput" && source->type != "statusCard")) continue;
                            const auto& g = source->geometry;
                            const QRectF rect(g.x, g.y, g.width, g.height);
                            const auto content = valueText->toPlainText();
                            for (auto block = valueText->document()->begin(); block.isValid(); block = block.next()) {
                                const auto* layout = block.layout();
                                for (int line = 0; line < layout->lineCount(); ++line) {
                                    const auto bounds = layout->lineAt(line).naturalTextRect().translated(layout->position() + valueText->pos());
                                    if (bounds.width() > 0) textLines.push_back({QString::fromStdString(id), bounds});
                                }
                            }
                            double textWidth = 0;
                            for (const auto& line : content.split('\n')) textWidth = std::max(textWidth, QFontMetricsF(valueText->font()).horizontalAdvance(line));
                            const auto bottom = valueText->pos().y() + valueText->boundingRect().height();
                            if (textWidth > valueText->textWidth() + 1 || bottom > rect.bottom() + 1 || valueText->pos().y() < rect.top() - 1) {
                                issues.append(QJsonObject{{"screen", QString::fromStdString(screen.screenId)}, {"widget", QString::fromStdString(id)},
                                    {"sample",sample}, {"text",content}, {"width",textWidth}, {"allowedWidth",valueText->textWidth()},
                                    {"bottom",bottom}, {"allowedBottom",rect.bottom()}, {"fontSize",valueText->font().pixelSize()}});
                            }
                            if (!sample && (id == "lanhu-brand" || id == "lanhu-clock")) {
                                const QFontInfo info(valueText->font());
                                fonts.append(QJsonObject{{"screen",QString::fromStdString(screen.screenId)}, {"widget",QString::fromStdString(id)},
                                    {"family",info.family()}, {"pixelSize",info.pixelSize()}, {"stretch",valueText->font().stretch()},
                                    {"scaleX",page.transform().m11()}, {"scaleY",page.transform().m22()}});
                            }
                        }
                        for (std::size_t a = 0; a < textLines.size(); ++a) for (std::size_t b = a + 1; b < textLines.size(); ++b) {
                            if (textLines[a].first == textLines[b].first) continue;
                            const auto overlap = textLines[a].second.intersected(textLines[b].second);
                            if (overlap.width() > 1 && overlap.height() > 1)
                                issues.append(QJsonObject{{"screen",QString::fromStdString(screen.screenId)}, {"sample",sample},
                                    {"widget",textLines[a].first}, {"overlaps",textLines[b].first},
                                    {"width",overlap.width()}, {"height",overlap.height()}});
                        }
                    }
                }
                page.close();
            }
            QFile report(QString::fromLocal8Bit(argv[2]) + "/layout-audit.json");
            require(report.open(QIODevice::WriteOnly), "cannot write layout report");
            report.write(QJsonDocument(QJsonObject{{"renders",renders}, {"issues",issues}, {"fonts",fonts}}).toJson());
            std::cout << "layout audit: " << renders << " renders, " << issues.size() << " text overflows\n";
            return issues.empty() ? 0 : 1;
        }
        {
            edge_gateway::ScadaScreen labels;
            auto text = widget("qtLabel"); text.widgetId = "static-fit";
            text.geometry = {0, 0, 106, 42};
            text.properties["qtAutoFit"] = "true";
            text.properties["qtFontSize"] = "16";
            text.properties["qtVerticalAlignment"] = "Center";
            text.properties["qtText"] = "MMMMMMMM\nMMMMMMMM";
            labels.widgets = {text};
            ScadaSceneView page(labels, ".", {}, {}, runtime, [](const std::string&) {});
            QGraphicsTextItem* item = nullptr;
            for (auto* candidate : page.scene()->items()) if (candidate->data(0).toString() == "static-fit") item = dynamic_cast<QGraphicsTextItem*>(candidate);
            require(item && item->toPlainText() == QString::fromStdString(text.properties["qtText"]), "static multiline text was lost or elided");
            require(item->font().pixelSize() < 16 && item->font().pixelSize() >= 12, "static label did not fit before refresh");
            require(item->pos().y() + item->boundingRect().height() <= 42, "multiline text exceeds height");
            require(item->font().stretch() == QFont::Unstretched, "text was stretched to fit");
            page.refresh(10000);
            require(item->toPlainText().count('\n') == 1, "refresh destroyed explicit line break");
            auto number = text; number.type = "qtValue"; number.geometry = {0, 0, 80, 24};
            number.bindings = {{"edge-1", "t1", "value"}}; labels.widgets = {number};
            ScadaSceneView values(labels, ".", {}, {}, runtime, [](const std::string&) {});
            runtime.put(1, -123456.78); values.refresh(10000);
            auto* rendered = ScadaSceneViewTestAccess::widgets(values)[0].valueText;
            const auto small = rendered->font().pixelSize();
            runtime.put(1, 2); values.refresh(10001);
            require(rendered->font().pixelSize() > small && rendered->toPlainText() == "2", "short value did not restore font size");
            runtime.values.clear();
        }
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
        require(!topology.fresh() && topology.usingLocalFallback() && topology.vehicleCount() == 1,
                "missing membership must display one local vehicle");
        require(topology.vehicleId(0) == QString::fromStdString(runtime.nodeId()), "default vehicle is not the configured local node");
        for (const QString& key : {"p", "q", "pa", "pb", "pc", "soc", "frequency", "factor"})
            require(topology.valueText(0, key) == "--", "local fallback invented telemetry");
        require(topology.page() == 0 && topology.pageCount() == 1 &&
                topology.findChild<QPushButton*>("topologyNext")->isHidden(), "default vehicle must not paginate");
        runtime.cluster = "{"; topology.refresh(10000);
        require(topology.usingLocalFallback() && topology.vehicleCount() == 1 && !topology.fresh(),
                "malformed first snapshot removed the default local vehicle");
        for (int count : {0, 1, 2, 3, 4, 5, 10}) {
            runtime.cluster = clusterFixture(count, 0, 10000);
            topology.refresh(10000);
            require(topology.fresh() && topology.vehicleCount() == std::max(1, count), "vehicle count differs from membership/default");
            require(topology.usingLocalFallback() == (count == 0), "local fallback was appended to real membership");
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
        require(topology.fresh() && topology.vehicleCount() == 1 && topology.usingLocalFallback() &&
                topology.vehicleId(0) == QString::fromStdString(runtime.nodeId()), "empty membership did not restore the local default");
        require(topology.valueText(0, "p") == "--" && topology.valueText(0, "soc") == "--",
                "empty membership retained peer telemetry in the local default");
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
        require(plotted.chartX == 88 && plotted.chartWidth == 1472, "light chart must reserve only the axis gutters");
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
        axes.trendSeries[0].samples = {{now-60000,3326},{now,3334}};
        axes.trendSeries[1].samples = {{now-60000,-123456.78},{now,123456.78}};
        ScadaSceneViewTestAccess::render(dual);
        for (const auto& labels : {axes.chartYLabels, axes.chartSecondaryYLabels}) {
            QString previous;
            for (auto* label : labels) {
                require(label->document()->defaultTextOption().wrapMode() == QTextOption::NoWrap, "axis tick can wrap");
                require(QFontMetricsF(label->font()).horizontalAdvance(label->toPlainText()) <= label->textWidth(), "axis tick exceeds gutter");
                require(label->toPlainText() != previous, "axis formatting lost tick distinction");
                require(label->document()->begin().layout()->lineCount() == 1, "axis tick has multiple lines");
                previous = label->toPlainText();
            }
        }

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
            const auto singleScreen = std::find_if(project.screens.begin(), project.screens.end(), [](const auto& s) { return s.screenId == "Topology-Single"; });
            require(singleScreen != project.screens.end(), "single topology route missing");
            FixtureRuntime singleRuntime;
            for (const auto& mapping : project.runtimeMappings) {
                ScadaSceneResolvedTag tag; tag.mapping = mapping;
                for (const auto& candidate : project.tags) if (candidate.tagId == mapping.tagId) tag.tag = candidate;
                singleRuntime.projectTags[mapping.tagId] = tag;
            }
            ScadaSceneView singleView(*singleScreen, argv[1], project.alarms, project.trends, singleRuntime, [](const std::string&) {});
            singleView.setWindowFlag(Qt::FramelessWindowHint);
            singleView.resize(1920, 1080); singleView.show(); app.processEvents();
            int images = 0;
            for (auto* item : singleView.scene()->items()) if (auto* bitmap = dynamic_cast<QGraphicsPixmapItem*>(item)) {
                ++images;
                require(!bitmap->pixmap().isNull() && bitmap->transformationMode() == Qt::SmoothTransformation,
                        "single topology artwork missing or enlarged without smoothing");
            }
            require(images >= 5, "single topology device artwork missing");
            for (int scenario = 0; scenario < 6; ++scenario) {
                singleRuntime.values.clear();
                if (scenario != 4) for (const auto& mapping : project.runtimeMappings) singleRuntime.put(mapping.index, 25.8);
                for (auto index : {997u, 998u, 999u}) if (scenario != 4)
                    singleRuntime.put(index, scenario == 1 ? 0 : scenario == 5 ? 2 : 1, scenario == 3, scenario == 2 ? 0 : 1);
                singleRuntime.put(1216, 1); singleRuntime.put(1217, 0); singleRuntime.put(1569, 75);
                singleView.refresh(now); app.processEvents();
                int breakers = 0;
                for (const auto& item : ScadaSceneViewTestAccess::widgets(singleView)) {
                    if (!item.valueText) continue;
                    const auto id = item.valueText->data(0).toString();
                    if (id == "single-load-breaker" || id == "single-grid-breaker" || id == "single-pcs-breaker") {
                        ++breakers;
                        const auto expected = scenario == 0 ? QColor("#F52D59") : scenario == 1 ? QColor("#18C773")
                            : scenario == 5 ? QColor("#90979F") : QColor("#71808A");
                        require(item.panel->brush().color() == expected, "single topology breaker quality/state incorrect");
                    }
                    if (id == "single-bus-0") require(item.valueText->toPlainText() == "--", "reserved bus borrowed PCS telemetry");
                }
                require(breakers == 3, "single topology DI feedback missing");
                if (argc > 2) {
                    QDir().mkpath(argv[2]);
                    require(singleView.grab().save(QString::fromLocal8Bit(argv[2]) + QString("/Topology-Single-state-%1.png").arg(scenario)), "single topology screenshot failed");
                }
            }
            for (int scenario = 0; scenario < 12; ++scenario) {
                singleRuntime.put(1216, scenario == 1 || scenario == 2 ? 0 : 1);
                singleRuntime.put(1217, scenario == 1 || scenario == 3 ? 1 : 0);
                if (scenario == 4) singleRuntime.put(1217, 2);
                if (scenario == 5) singleRuntime.put(1216, 2);
                if (scenario == 6) singleRuntime.values.erase(1217);
                if (scenario == 7) singleRuntime.values.erase(1216);
                if (scenario == 8) singleRuntime.put(1217, 0, true);
                if (scenario == 9) singleRuntime.put(1216, 1, true);
                if (scenario == 10) singleRuntime.put(1217, 0, false, 0);
                if (scenario == 11) singleRuntime.put(1216, 1, false, 0);
                singleView.refresh(now);
                bool found = false;
                for (const auto& item : ScadaSceneViewTestAccess::widgets(singleView)) {
                    if (!item.valueText || item.valueText->data(0).toString() != "single-pcs-grid-state") continue;
                    found = true;
                    const auto expected = scenario == 0 ? QColor("#18C773") : scenario == 1 ? QColor("#F52D59")
                        : scenario < 6 ? QColor("#90979F") : QColor("#71808A");
                    require(item.panel->brush().color() == expected, "single topology mode pair accepted invalid feedback");
                    if (scenario >= 2) require(item.valueText->toPlainText() == "--", "invalid mode pair claims a mode");
                }
                require(found, "single topology mode indicator missing");
            }
            require(singleRuntime.writeCalls == 0 && singleRuntime.groupWriteCalls == 0, "single topology issued a write");
            singleView.close();
            FixtureRuntime topologyRuntime;
            ScadaSceneView topologyView(*topologyScreen, argv[1], project.alarms, project.trends, topologyRuntime, [](const std::string&) {});
            topologyView.setWindowFlag(Qt::FramelessWindowHint);
            topologyView.resize(1440, 810); topologyView.show(); app.processEvents();
            topologyView.refresh(QDateTime::currentMSecsSinceEpoch()); app.processEvents();
            if (argc > 2) {
                QDir().mkpath(argv[2]);
                require(topologyView.grab().save(QString::fromLocal8Bit(argv[2]) + "/Topology-local-default.png"),
                        "default local topology screenshot failed");
            }
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
            require(topologyRuntime.writeCalls == 0 && topologyRuntime.groupWriteCalls == 0, "parallel topology issued a write");
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
