#include "local_display_qt_soe.hpp"
#include <QApplication>
#include <QDir>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory");
    const auto path = (directory.path() + "/soe.db").toStdString();
    edge_gateway::SoeStore writer(path);
    std::vector<edge_gateway::Iec103SoeEvent> events;
    for (int n = 0; n < 55; ++n) {
        edge_gateway::Iec103SoeEvent e;
        e.eventId = "fixture:" + std::to_string(n);
        e.machineCode = "SIMULATED"; e.deviceCode = "AM5SE-F";
        e.name = u8"过流一段保护"; e.state = n % 2 ? u8"动作" : u8"返回";
        e.receivedMs = QDateTime::currentMSecsSinceEpoch() - (55 - n) * 10;
        e.deviceTimeMs = e.receivedMs - 1;
        e.typeId = 1; e.cause = 1; e.commonAddress = 1;
        e.functionType = 160; e.informationNumber = 1;
        e.dpi = e.rawDpi = n % 2 + 1;
        e.timeValid = e.valueValid = e.mapped = true;
        if (n == 54) { e.state = "DPI=2"; e.mapped = false; }
        if (n == 53) { e.timeValid = false; e.valueValid = false; e.state = "DPI=3"; }
        if(n>=52){e.typeId=2;e.faultNumber=n==52?16:15;e.deviceTimeMs=1793232005000LL;}
        events.push_back(e);
    }
    writer.append(events);
    edge_gateway::DeviceConfig config;config.machineCode="SIMULATED";config.meterCode="AM5SE-F";
    config.protocol.iec.commonAddress=1;config.protocol.iec.soe.utcOffsetMinutes=0;
    edge_gateway::Iec103DisturbanceRecord record;record.fan=15;record.state=6;record.rawTimeHex="881300001d0a1a";
    writer.recordDirectory(config,{record},true,true,QDateTime::currentMSecsSinceEpoch());
    SoeHistoryView view(path, "SIMULATED");
    view.resize(1230, 600); view.show(); app.processEvents();
    auto* table = view.findChild<QTableWidget*>("soeTable");
    auto* next = view.findChild<QPushButton*>("soeNext");
    auto* previous = view.findChild<QPushButton*>("soePrevious");
    require(table && next && previous, "history controls");
    require(table->rowCount() == 50 && next->isEnabled() && !previous->isEnabled(), "first page");
    require(table->item(0, 6)->text().contains(QString::fromUtf8("含义待确认")), "unknown semantics marked");
    require(table->item(1, 1)->text() == "--", "invalid time not fabricated");
    require(table->columnCount()==8 && table->item(0,7)->text()==QString::fromUtf8("目录已关联"),"recording association visible");
    require(table->item(1,7)->text()==QString::fromUtf8("关联不确定"),"bad event time cannot link");
    require(table->item(2,7)->text().contains(QString::fromUtf8("未找到")),"missing directory state visible");
    table->selectRow(0);view.findChild<QPushButton*>("soeRecordingDetails")->click();app.processEvents();
    auto* details=view.findChild<QDialog*>("soeRecordingDialog");
    require(details&&details->findChild<QTextEdit*>()->isReadOnly(),"recording details are read-only");
    require(details->findChild<QTextEdit*>()->toPlainText().contains("FAN: 15"),"details include original FAN");
    details->close();app.processEvents();
    require(table->editTriggers() == QAbstractItemView::NoEditTriggers, "history read-only");
    for (int c = 0; c < table->columnCount(); ++c) {
        require(table->columnWidth(c) >= 40, "column collapsed");
        require(table->item(0, c)->textAlignment() == Qt::AlignCenter, "cell alignment");
    }
    if (argc > 1) require(view.grab().save(QString::fromLocal8Bit(argv[1])), "save simulated screenshot");
    next->click(); app.processEvents();
    require(table->rowCount() == 5 && previous->isEnabled() && !next->isEnabled(), "second page");
    previous->click(); require(table->rowCount() == 50, "previous page");
    view.findChild<QPushButton*>("soeLatest")->click();
    require(table->rowCount() == 50 && !previous->isEnabled(), "latest page");
    SoeHistoryView isolated(path, "OTHER");
    require(isolated.findChild<QTableWidget*>("soeTable")->rowCount() == 0, "machine isolation");
    SoeHistoryView missing(path + ".missing", "SIMULATED");
    require(missing.findChild<QLabel*>("soeStatus")->text() == QString::fromUtf8("SOE 数据不可用"), "missing storage visible");
    require(!QFile::exists(QString::fromStdString(path + ".missing")), "display must not create database");
    std::cout << "SOE view pagination, quality, alignment and read-only tests passed\n";
}
