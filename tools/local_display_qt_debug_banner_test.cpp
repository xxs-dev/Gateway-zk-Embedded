// Local screen "调试中" banner (maintenance v2 debug mode): text from debug-mode.json, shown on top of the
// screen while active, follows the window size, hidden when inactive / unreadable, stale state marked.
// Optional argv[1]: save a screenshot of the active banner (PNG).
#include "local_display_qt_debug_banner.hpp"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QPixmap>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdio>
#include <unistd.h>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

void writeState(const QString& path, const QByteArray& json) {
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(json);
}

}  // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    const QString path = QDir::tempPath() + QStringLiteral("/gz-debug-banner-%1.json").arg(::getpid());
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QByteArray active = "{\"active\":true,\"since\":" + QByteArray::number(now - 600000) +
                              ",\"operator\":\"张工\",\"operatorId\":\"u-1001\",\"reason\":\"现场直推前调试\",\"ticketId\":\"FT-1\",\"lastActivityAt\":" +
                              QByteArray::number(now - 5000) + "}";

    bool on = true;
    check(MaintenanceDebugBanner::describe("not json", now, &on).isEmpty() && !on, "unreadable state: no banner");
    check(MaintenanceDebugBanner::describe("{\"active\":false,\"operator\":\"x\"}", now, &on).isEmpty() && !on, "inactive: no banner");
    const QString text = MaintenanceDebugBanner::describe(active, now, &on);
    check(on && text.startsWith(QStringLiteral("调试中 · 操作人 张工 · 现场直推前调试 · 自 ")), "active text names operator, reason and start");
    check(!text.contains(QStringLiteral("未刷新")), "fresh activity is not marked stale");
    const QByteArray stale = QByteArray(active).replace("\"lastActivityAt\":" + QByteArray::number(now - 5000), "\"lastActivityAt\":" + QByteArray::number(now - 50 * 60000));
    check(MaintenanceDebugBanner::describe(stale, now, &on).contains(QStringLiteral("状态可能未刷新")), "stale activity is marked");

    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* content = new QWidget(&window);
    content->setStyleSheet(QStringLiteral("background: #10324a;"));
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(content);
    window.resize(800, 480);
    writeState(path, "{\"active\":false}");
    MaintenanceDebugBanner banner(&window, path, 200);
    window.show();
    app.processEvents();
    check(!banner.debugActive() && !banner.isVisible(), "hidden while not in debug mode");
    writeState(path, active);
    banner.refresh();
    app.processEvents();
    check(banner.debugActive() && banner.isVisible() && banner.text() == text.left(banner.text().size()), "shown when debug mode starts");
    check(banner.geometry().top() == 0 && banner.width() == 800 && banner.height() > 20, "strip across the top");
    check(banner.testAttribute(Qt::WA_TransparentForMouseEvents), "does not take touch input");
    window.resize(1024, 600);
    app.processEvents();
    check(banner.width() == 1024, "follows the window width");
    if (argc > 1) window.grab().save(QString::fromLocal8Bit(argv[1]));
    writeState(path, "{\"active\":false}");
    // the poll timer picks the change up without an explicit refresh
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + 3000;
    while (banner.isVisible() && QDateTime::currentMSecsSinceEpoch() < deadline) app.processEvents(QEventLoop::AllEvents, 50);
    check(!banner.isVisible(), "hidden again when debug mode ends (polled)");
    QFile::remove(path);
    banner.refresh();
    check(!banner.isVisible(), "missing state file: hidden");
    std::printf("local_display_qt_debug_banner_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
