#pragma once

#include "local_display_qt_scada_runtime.hpp"

#include <algorithm>
#include <cmath>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPushButton>
#include <QSet>
#include <QStyle>
#include <QWidget>

// The cluster process owns membership. An empty roster displays the configured
// local vehicle only, without asserting membership, online state or telemetry.
class ParallelTopology final : public QWidget {
public:
    ParallelTopology(ScadaSceneRuntimeSource& runtime, const std::string& root)
        : runtime_(runtime) {
        setObjectName("parallelTopology");
        setAttribute(Qt::WA_TranslucentBackground);
        localMember_.insert("nodeId", QString::fromStdString(runtime_.nodeId()));
        const QDir assets(QString::fromStdString(root) + "/assets/lanhu-assets");
        storage_.load(assets.filePath("storage.png"));
        idle_.load(assets.filePath("storage-idle.png"));
        previous_ = new QPushButton(this);
        next_ = new QPushButton(this);
        previous_->setObjectName("topologyPrevious");
        next_->setObjectName("topologyNext");
        previous_->setIcon(style()->standardIcon(QStyle::SP_ArrowLeft));
        next_->setIcon(style()->standardIcon(QStyle::SP_ArrowRight));
        previous_->setToolTip(QString::fromUtf8("上一组储能车"));
        next_->setToolTip(QString::fromUtf8("下一组储能车"));
        for (auto* button : {previous_, next_}) {
            button->setFixedSize(32, 28);
            button->setStyleSheet("QPushButton{background:#FFFFFF;border:1px solid #DDE3E9;border-radius:3px;}");
        }
        previous_->move(1086, 323);
        next_->move(1192, 323);
        connect(previous_, &QPushButton::clicked, this, [this]() { setPage(page_ - 1); });
        connect(next_, &QPushButton::clicked, this, [this]() { setPage(page_ + 1); });
        setPage(0);
    }

    void refresh(std::int64_t now) {
        const auto json = runtime_.clusterStatusJson();
        QJsonParseError error;
        const auto doc = json.size() <= 1024 * 1024
            ? QJsonDocument::fromJson(QByteArray::fromStdString(json), &error) : QJsonDocument();
        const auto object = doc.object();
        const auto ts = object.value("ts").toDouble(-1);
        bool valid = doc.isObject() && object.value("schemaVersion").toString() == "2.0" &&
            object.value("members").isArray() && object.value("nodeId").isString() &&
            !object.value("nodeId").toString().isEmpty() && std::isfinite(ts) && ts > 0 &&
            ts <= static_cast<double>(now) + 1000;
        const auto array = object.value("members").toArray();
        valid = valid && array.size() <= 256;
        QSet<QString> ids;
        std::vector<QJsonObject> incoming;
        for (const auto& value : array) {
            const auto member = value.toObject();
            const auto id = member.value("nodeId").toString();
            if (!value.isObject() || id.isEmpty() || id.size() > 128 || ids.contains(id) ||
                !member.value("online").isBool() || !member.value("compatible").isBool() ||
                !member.value("duplicateIdentity").isBool()) { valid = false; break; }
            ids.insert(id);
            incoming.push_back(member);
        }
        fresh_ = valid && static_cast<double>(now) - ts <= runtime_.clusterStatusTtlMs();
        if (valid) {
            std::sort(incoming.begin(), incoming.end(), [](const QJsonObject& a, const QJsonObject& b) {
                return a.value("nodeId").toString() < b.value("nodeId").toString();
            });
            // Stale files may restore identities after restart, but may not
            // replace a newer membership snapshot already displayed.
            if (!known_ || ts >= timestamp_) {
                members_ = std::move(incoming);
                timestamp_ = ts;
                known_ = true;
            } else {
                fresh_ = false;
            }
        }
        setPage(page_);
    }

    bool usingLocalFallback() const { return members_.empty(); }
    int vehicleCount() const { return std::max(1, static_cast<int>(members_.size())); }
    int pageCount() const { return std::max(1, (vehicleCount() + 2) / 3); }
    int page() const { return page_; }
    bool fresh() const { return fresh_; }
    QString vehicleId(int i) const { return memberAt(i).value("nodeId").toString(); }
    QString valueText(int i, const QString& key) const {
        const auto& member = memberAt(i);
        const auto capability = member.value("capability").toObject();
        if (!online(member) || !member.value("capabilityFresh").toBool() ||
            !capability.value("ready").toBool()) return "--";
        if (key == "soc") {
            const auto value = capability.value("socPercent");
            return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= 0 && value.toDouble() <= 100
                ? QString::number(value.toDouble(), 'f', 1) : "--";
        }
        const auto actual = capability.value("actual").toArray();
        if (actual.size() != 6) return "--";
        for (const auto& value : actual) if (!value.isDouble() || !std::isfinite(value.toDouble())) return "--";
        if (key == "p" || key == "q") {
            const int first = key == "p" ? 0 : 3;
            const double total = actual[first].toDouble() + actual[first + 1].toDouble() + actual[first + 2].toDouble();
            return std::isfinite(total) ? QString::number(total, 'f', 1) : QStringLiteral("--");
        }
        if (key == "pa" || key == "pb" || key == "pc")
            return QString::number(actual[key == "pa" ? 0 : key == "pb" ? 1 : 2].toDouble(), 'f', 1);
        return "--";
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        const QString summary = usingLocalFallback() ? QString::fromUtf8("本机 1 台 · 组网状态未知")
            : !fresh_ ? QString::fromUtf8("组网数据不可用")
            : QString::fromUtf8("储能车 %1 台 · 在线 %2 台").arg(vehicleCount()).arg(
                std::count_if(members_.begin(), members_.end(), [this](const QJsonObject& m) { return online(m); }));
        text(p, QRect(420, 321, 650, 30), summary, 14, QColor("#777E85"));
        if (pageCount() > 1) text(p, QRect(1118, 323, 74, 28), QString("%1 / %2").arg(page_ + 1).arg(pageCount()), 14);
        const QPoint positions[] = {QPoint(834, 0), QPoint(834, 356), QPoint(420, 356)};
        const int count = std::min(3, vehicleCount() - page_ * 3);
        // These lines represent physical topology only, never breaker closure
        // or verified energization. No animated flow is synthesized.
        if (count > 0) {
            p.setPen(QPen(QColor("#A2BED6"), 3, online(memberAt(page_ * 3)) ? Qt::SolidLine : Qt::DashLine));
            p.drawLine(698, 61, 948, 61);
            if (count > 1) {
                p.setPen(QPen(QColor("#A2BED6"), 3, online(memberAt(page_ * 3)) && online(memberAt(page_ * 3 + 1)) ? Qt::SolidLine : Qt::DashLine));
                p.drawLine(1104, 61, 1228, 61);
                p.drawLine(1228, 61, 1228, 417);
                p.drawLine(1228, 417, 1104, 417);
            }
            if (count > 2) {
                p.setPen(QPen(QColor("#A2BED6"), 3, online(memberAt(page_ * 3 + 1)) && online(memberAt(page_ * 3 + 2)) ? Qt::SolidLine : Qt::DashLine));
                p.drawLine(698, 417, 948, 417);
            }
        }
        for (int i = 0; i < count; ++i) drawVehicle(p, page_ * 3 + i, positions[i]);
    }

private:
    const QJsonObject& memberAt(int index) const {
        if (usingLocalFallback() && index == 0) return localMember_;
        return members_.at(index);
    }
    void setPage(int page) {
        page_ = std::max(0, std::min(page, pageCount() - 1));
        previous_->setVisible(pageCount() > 1);
        next_->setVisible(pageCount() > 1);
        previous_->setEnabled(page_ > 0);
        next_->setEnabled(page_ + 1 < pageCount());
        QStringList identities;
        for (int i = page_ * 3; i < std::min(vehicleCount(), (page_ + 1) * 3); ++i)
            identities.push_back(vehicleId(i));
        setToolTip(identities.join('\n'));
        update();
    }
    bool online(const QJsonObject& m) const {
        return fresh_ && m.value("online").toBool() && m.value("compatible").toBool() &&
            !m.value("duplicateIdentity").toBool();
    }
    static void text(QPainter& p, const QRect& rect, const QString& value, int size,
                     QColor color = QColor("#303438"), bool bold = false) {
        QFont font(QStringLiteral("Microsoft YaHei"));
        font.setPixelSize(size);
        font.setBold(bold);
        p.setFont(font);
        p.setPen(color);
        p.drawText(rect, Qt::AlignCenter, p.fontMetrics().elidedText(value, Qt::ElideRight, rect.width()));
    }
    void drawVehicle(QPainter& p, int i, QPoint origin) {
        p.save();
        p.translate(origin);
        const auto& member = memberAt(i);
        const bool connected = online(member);
        const bool lower = origin.y() > 0;
        p.drawPixmap(QRect(109, 0, 158, 124), connected ? storage_ : idle_);
        p.fillRect(QRect(0, 134, 376, lower ? 160 : 184), connected ? QColor("#FFFFFF") : QColor("#EEF1F5"));
        p.fillRect(QRect(0, 134, 376, 42), QColor("#F7F7F7"));
        const int cabinet = member.value("cabinetNo").toInt();
        const QString title = usingLocalFallback() ? QString::fromUtf8("本机储能车")
            : cabinet > 0 ? QString::fromUtf8("%1#储能车").arg(cabinet) : QString::fromUtf8("储能车");
        text(p, QRect(8, 137, 180, 36), title, 18, QColor("#303438"), true);
        text(p, QRect(194, 137, 172, 36), QString::fromUtf8("PCS断路器  --"), 14);
        QString state = usingLocalFallback() || !fresh_ ? QString::fromUtf8("状态未知") : member.value("duplicateIdentity").toBool()
            ? QString::fromUtf8("身份冲突") : !member.value("compatible").toBool()
            ? QString::fromUtf8("配置不兼容") : connected ? QString::fromUtf8("在线") : QString::fromUtf8("离线");
        text(p, QRect(0, 7, 105, 22), member.value("nodeId").toString(), 12, QColor("#777E85"));
        text(p, QRect(0, 31, 105, 22), state, 12, QColor("#777E85"));
        const QString labels[] = {QString::fromUtf8("P总"), QString::fromUtf8("Q总"), QString::fromUtf8("功率因数"), QString::fromUtf8("频率")};
        const QString keys[] = {"p", "q", "factor", "frequency"};
        const QString units[] = {" kW", " kvar", "", " Hz"};
        for (int col = 0; col < 4; ++col) {
            text(p, QRect(14 + 91 * col, 182, 90, 30), labels[col], 14, QColor("#777E85"));
            text(p, QRect(14 + 91 * col, 214, 90, 30), valueText(i, keys[col]) + units[col], 17);
        }
        p.fillRect(QRect(16, 252, 210, 1), QColor("#DDE3E9"));
        const auto soc = valueText(i, "soc");
        text(p, QRect(236, 241, 124, 19), "SOC " + soc + "%", 12);
        if (soc != "--") p.fillRect(QRectF(16, 251, 210 * soc.toDouble() / 100, 2), QColor("#18CC70"));
        const QString phases[] = {"a", "b", "c"};
        for (int phase = 0; phase < 3; ++phase) {
            if (!lower) text(p, QRect(16 + phase * 122, 259, 112, 25), "U" + phases[phase] + "  -- V", 13);
            text(p, QRect(16 + phase * 122, lower ? 259 : 285, 112, 25), "P" + phases[phase] + "  " + valueText(i, "p" + phases[phase]) + " kW", 13);
        }
        p.restore();
    }
    ScadaSceneRuntimeSource& runtime_;
    QJsonObject localMember_;
    std::vector<QJsonObject> members_;
    bool known_ = false;
    bool fresh_ = false;
    double timestamp_ = 0;
    int page_ = 0;
    QPixmap storage_, idle_;
    QPushButton* previous_ = nullptr;
    QPushButton* next_ = nullptr;
};
