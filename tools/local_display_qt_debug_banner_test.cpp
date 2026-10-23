// Local screen "调试中" banner (maintenance v2 debug mode): text from debug-mode.json, shown on top of the
// screen while active, follows the window size, hidden when inactive / unreadable, stale state marked.
// Optional argv[1]: save a screenshot of the active banner (PNG); argv[2]: keep the screen snapshots in that directory.
#include "local_display_qt_debug_banner.hpp"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStringList>
#include <QPixmap>
#include <QRect>
#include <QScreen>
#include <QWindow>
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
        std::printf("FAIL: %s\n", what);
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
    // the banner writes its state changes to stderr (the journal on the device): capture them
    const QString logPath = path + QStringLiteral(".log");
    std::fflush(stderr);
    if (std::freopen(logPath.toLocal8Bit().constData(), "w", stderr) == nullptr) check(false, "capture stderr");
    MaintenanceDebugBanner banner(&window, path, 200);
    window.show();
    app.processEvents();
    check(!banner.debugActive() && !banner.isVisible(), "hidden while not in debug mode");
    writeState(path, active);
    banner.refresh();
    app.processEvents();
    check(banner.debugActive() && banner.isVisible() && banner.text() == text.left(banner.text().size()), "shown when debug mode starts");
    // at the top of the window, or just below a desktop panel that covers it (work area)
    const QScreen* scr = window.windowHandle() != nullptr ? window.windowHandle()->screen() : QGuiApplication::primaryScreen();
    const int expectTop = scr != nullptr ? MaintenanceDebugBanner::topOffset(QRect(window.mapToGlobal(QPoint(0, 0)), window.size()), scr->availableGeometry()) : 0;
    check(banner.geometry().top() == expectTop && banner.width() == 800 && banner.height() > 20, "strip across the top (below a desktop panel)");
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
    std::fflush(stderr);
    QFile logFile(logPath);
    QStringList log;
    if (logFile.open(QIODevice::ReadOnly)) {
        for (const QByteArray& l : logFile.readAll().split('\n'))
            if (l.startsWith("maintenance debug banner: ")) log << QString::fromUtf8(l.mid(26));
    }
    check(log.size() == 3, "three state lines: watching, shown, hidden (no repeats while unchanged)");
    check(log.value(0).startsWith(QStringLiteral("watching ")) && log.value(0).endsWith(QStringLiteral("(debug mode not active)")), "first line: watching the state file");
    check(log.value(1).startsWith(QStringLiteral("shown: 调试中 · 操作人 张工")), "shown line carries the banner text");
    check(log.value(2) == QStringLiteral("hidden"), "hidden line when debug mode ends");
    logFile.close();
    QFile::remove(logPath);

    // keeps clear of a desktop panel over the top of the window (work area)
    check(MaintenanceDebugBanner::topOffset(QRect(0, 0, 1920, 1080), QRect(0, 32, 1920, 1048)) == 32, "panel over the window top: strip moves below it");
    check(MaintenanceDebugBanner::topOffset(QRect(0, 32, 1920, 1048), QRect(0, 32, 1920, 1048)) == 0, "window already below the panel: no offset");
    check(MaintenanceDebugBanner::topOffset(QRect(0, 0, 800, 100), QRect(0, 50, 800, 430)) == 25, "offset capped at a quarter of the window");
    check(MaintenanceDebugBanner::topOffset(QRect(), QRect(0, 32, 10, 10)) == 0, "unknown geometry: no offset");

    // screenshots only while the request file exists
    // optional argv[2]: keep the screenshots there (to look at them / feed the client's checker)
    const QString shots = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QDir::tempPath() + QStringLiteral("/gz-banner-shots-%1").arg(::getpid());
    QDir().mkpath(shots);
    const QString shotLog = shots + QStringLiteral("/stderr.log");
    std::fflush(stderr);
    if (std::freopen(shotLog.toLocal8Bit().constData(), "w", stderr) == nullptr) check(false, "capture stderr (snapshots)");
    writeState(path, "{\"active\":false}");
    MaintenanceDebugBanner second(&window, path, 100000);
    second.setSnapshotDirectory(shots);
    auto settle = [&app](int ms) {
        const qint64 until = QDateTime::currentMSecsSinceEpoch() + ms;
        while (QDateTime::currentMSecsSinceEpoch() < until) app.processEvents(QEventLoop::AllEvents, 50);
    };
    writeState(path, active);
    second.refresh();  // no request file: no screenshot
    settle(1800);
    writeState(path, "{\"active\":false}");
    second.refresh();
    settle(1800);
    writeState(shots + QStringLiteral("/banner-snapshot.request"), "");
    writeState(path, active);
    second.refresh();
    settle(1800);
    writeState(path, "{\"active\":false}");
    second.refresh();
    settle(1800);
    std::fflush(stderr);
    QStringList snaps;
    QFile sl(shotLog);
    if (sl.open(QIODevice::ReadOnly)) {
        for (const QByteArray& l : sl.readAll().split('\n'))
            if (l.startsWith("maintenance debug banner: snapshot ")) snaps << QString::fromUtf8(l.mid(35));
    }
    check(snaps.size() == 2, "one screenshot per change, only while requested");
    check(snaps.value(0).contains(QStringLiteral("banner-shown.png")) && snaps.value(1).contains(QStringLiteral("banner-hidden.png")), "shown, then hidden screenshot");
    std::printf("  snapshots: %s\n", snaps.join(QStringLiteral(" | ")).toUtf8().constData());
    if (argc > 2) {
        QFile::remove(shotLog);
        QFile::remove(shots + QStringLiteral("/banner-snapshot.request"));
    } else {
        QDir(shots).removeRecursively();
    }
    QFile::remove(path);
    std::printf("local_display_qt_debug_banner_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
