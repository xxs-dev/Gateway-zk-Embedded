#pragma once

// Local screen banner for maintenance channel v2 debug mode: while an engineer has the device in
// debug mode (SystemMonitor writes data/maintenance-v2/debug-mode.json, 0644), a strip at the top of
// the local display shows "调试中 · 操作人 … · 原因 … · 自 HH:mm". It does not take touch input.
// Every state change is written to stderr (journal of ky-ems.service) as
// "maintenance debug banner: shown …" / "maintenance debug banner: hidden", so a field check can
// confirm what the screen did without someone looking at it.
// No Q_OBJECT (the device build has no moc step).

#include <QByteArray>
#include <QLabel>
#include <QString>

class QEvent;
class QTimer;

class MaintenanceDebugBanner : public QLabel {
public:
    static const char* defaultStateFile();  // /opt/modbus-gateway/data/maintenance-v2/debug-mode.json

    // The banner sits on top of `parent` and follows its size.
    MaintenanceDebugBanner(QWidget* parent, const QString& stateFile, int pollMs = 2000);

    void refresh();  // read the state file now
    bool debugActive() const { return active_; }

    // Banner text for one debug-mode.json; empty when debug mode is not active (or the file is unreadable).
    // nowMs: a mode whose last activity is older than 40 minutes is shown as possibly stale.
    static QString describe(const QByteArray& json, qint64 nowMs, bool* active);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void place();

    void logState(bool active, const QString& text);

    QString file_;
    QTimer* timer_ = nullptr;
    bool active_ = false;
    int logged_ = -1;  // last state written to the journal (-1: none yet)
};
