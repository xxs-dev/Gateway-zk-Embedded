#include "local_display_qt_debug_banner.hpp"

#include <QDateTime>
#include <QEvent>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QTimer>
#include <QWidget>

namespace {

const qint64 kStaleMs = 40LL * 60 * 1000;  // longer than the 30-minute idle exit of debug mode

QString clock(qint64 ms) {
    return QDateTime::fromMSecsSinceEpoch(ms).toString(QStringLiteral("HH:mm"));
}

}  // namespace

const char* MaintenanceDebugBanner::defaultStateFile() {
    return "/opt/modbus-gateway/data/maintenance-v2/debug-mode.json";
}

MaintenanceDebugBanner::MaintenanceDebugBanner(QWidget* parent, const QString& stateFile, int pollMs)
    : QLabel(parent), file_(stateFile) {
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
    if (!active) {
        hide();
        return;
    }
    if (text != this->text()) setText(text);
    place();
    raise();
    show();
}

bool MaintenanceDebugBanner::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parentWidget() && (event->type() == QEvent::Resize || event->type() == QEvent::Show)) {
        place();
        if (active_) raise();
    }
    return QLabel::eventFilter(watched, event);
}

void MaintenanceDebugBanner::place() {
    QWidget* p = parentWidget();
    if (p == nullptr) return;
    const int h = sizeHint().height();
    setGeometry(0, 0, p->width(), h);
}
