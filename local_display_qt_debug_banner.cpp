#include "local_display_qt_debug_banner.hpp"

#include <QDateTime>
#include <QEvent>
#include <QFileInfo>
#include <QGuiApplication>
#include <QPixmap>
#include <QPoint>
#include <QScreen>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRect>
#include <QStringList>
#include <QTimer>
#include <QWidget>
#include <QWindow>

#include <cstdio>

namespace {

const qint64 kStaleMs = 40LL * 60 * 1000;  // longer than the 30-minute idle exit of debug mode

QString clock(qint64 ms) {
    return QDateTime::fromMSecsSinceEpoch(ms).toString(QStringLiteral("HH:mm"));
}

}  // namespace

const char* MaintenanceDebugBanner::defaultStateFile() {
    return "/opt/modbus-gateway/data/maintenance-v2/debug-mode.json";
}

const char* MaintenanceDebugBanner::defaultSnapshotDirectory() {
    return "/run/ky-ems";
}

int MaintenanceDebugBanner::topOffset(const QRect& windowGlobal, const QRect& workArea) {
    if (!windowGlobal.isValid() || !workArea.isValid()) return 0;
    const int offset = workArea.top() - windowGlobal.top();
    if (offset <= 0) return 0;
    return offset < windowGlobal.height() / 4 ? offset : windowGlobal.height() / 4;
}

MaintenanceDebugBanner::MaintenanceDebugBanner(QWidget* parent, const QString& stateFile, int pollMs)
    : QLabel(parent), file_(stateFile), snapshotDir_(QString::fromLatin1(defaultSnapshotDirectory())) {
    setTextFormat(Qt::PlainText);
    setAlignment(Qt::AlignCenter);
    setWordWrap(false);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);  // never steals touches from the screen below
    setStyleSheet(QStringLiteral("QLabel { background: rgba(214, 120, 0, 230); color: #ffffff; font-size: 20px; font-weight: 600;"
                                 " padding: 6px 16px; border-bottom: 2px solid #8a4b00; }"));
    hide();
    if (parent != nullptr) parent->installEventFilter(this);
    timer_ = new QTimer(this);
    timer_->setInterval(pollMs > 200 ? pollMs : 200);
    QObject::connect(timer_, &QTimer::timeout, this, [this]() { refresh(); });
    timer_->start();
    refresh();
}

QString MaintenanceDebugBanner::describe(const QByteArray& json, qint64 nowMs, bool* active) {
    *active = false;
    QJsonParseError error;
    const QJsonObject o = QJsonDocument::fromJson(json, &error).object();
    if (error.error != QJsonParseError::NoError || !o.value(QStringLiteral("active")).toBool()) return QString();
    *active = true;
    QStringList parts;
    parts << QStringLiteral("调试中");
    const QString who = o.value(QStringLiteral("operator")).toString().trimmed();
    parts << QStringLiteral("操作人 %1").arg(who.isEmpty() ? QStringLiteral("--") : who.left(32));
    const QString reason = o.value(QStringLiteral("reason")).toString().simplified();
    if (!reason.isEmpty()) parts << reason.left(60);
    const qint64 since = static_cast<qint64>(o.value(QStringLiteral("since")).toDouble());
    if (since > 0) parts << QStringLiteral("自 %1").arg(clock(since));
    const qint64 last = static_cast<qint64>(o.value(QStringLiteral("lastActivityAt")).toDouble());
    if (last > 0 && nowMs - last > kStaleMs) parts << QStringLiteral("最后操作 %1，状态可能未刷新").arg(clock(last));
    return parts.join(QStringLiteral(" · "));
}

void MaintenanceDebugBanner::refresh() {
    QFile f(file_);
    QByteArray json;
    if (f.open(QIODevice::ReadOnly)) json = f.read(16384);
    bool active = false;
    const QString text = describe(json, QDateTime::currentMSecsSinceEpoch(), &active);
    active_ = active;
    logState(active, text);
    if (!active) {
        if (isVisible()) {
            const QRect area = geometry();
            hide();
            if (QWidget* p = parentWidget()) p->update(area);  // repaint what the strip covered right away
        } else {
            hide();
        }
        return;
    }
    if (text != this->text()) setText(text);
    place();
    raise();
    show();
}

void MaintenanceDebugBanner::logState(bool active, const QString& text) {
    const int state = active ? 1 : 0;
    if (state == logged_) return;
    const bool first = logged_ < 0;
    logged_ = state;
    QByteArray line("maintenance debug banner: ");
    if (active) {
        line += "shown: ";
        line += text.toUtf8();
    } else if (first) {
        line += "watching ";
        line += file_.toUtf8();
        line += " (debug mode not active)";
    } else {
        line += "hidden";
    }
    std::fprintf(stderr, "%s\n", line.constData());
    std::fflush(stderr);
    if (!first) snapshot(active);
}

void MaintenanceDebugBanner::snapshot(bool active) {
    if (snapshotDir_.isEmpty() || !QFileInfo::exists(snapshotDir_ + QStringLiteral("/banner-snapshot.request"))) return;
    const QString path = snapshotDir_ + (active ? QStringLiteral("/banner-shown.png") : QStringLiteral("/banner-hidden.png"));
    QTimer::singleShot(1500, this, [path]() {
        QScreen* screen = QGuiApplication::primaryScreen();
        const QPixmap shot = screen != nullptr ? screen->grabWindow(0) : QPixmap();
        QByteArray line("maintenance debug banner: snapshot ");
        if (!shot.isNull() && shot.save(path, "PNG")) {
            line += path.toUtf8() + ' ' + QByteArray::number(shot.width()) + 'x' + QByteArray::number(shot.height());
        } else {
            line += "failed: " + path.toUtf8();
        }
        std::fprintf(stderr, "%s\n", line.constData());
        std::fflush(stderr);
    });
}

bool MaintenanceDebugBanner::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parentWidget() && (event->type() == QEvent::Resize || event->type() == QEvent::Show || event->type() == QEvent::Move)) {
        place();
        if (active_) raise();
    }
    return QLabel::eventFilter(watched, event);
}

void MaintenanceDebugBanner::place() {
    QWidget* p = parentWidget();
    if (p == nullptr) return;
    const int h = sizeHint().height();
    int top = 0;
    if (p->isWindow() && p->isVisible()) {
        // a desktop panel (e.g. GNOME's top bar) drawn over the top of the window would hide the strip
        const QPoint origin = p->mapToGlobal(QPoint(0, 0));
        QScreen* screen = p->windowHandle() != nullptr ? p->windowHandle()->screen() : nullptr;  // Qt 5.0+ (device Qt is old)
        if (screen == nullptr) screen = QGuiApplication::primaryScreen();
        if (screen != nullptr) top = topOffset(QRect(origin, p->size()), screen->availableGeometry());
    }
    setGeometry(0, top, p->width(), h);
}
