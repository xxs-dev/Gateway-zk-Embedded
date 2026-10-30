#pragma once

#include <cmath>
#include <QIcon>
#include <QMessageBox>
#include <QPushButton>
#include <QStyle>
#include <QVariant>
#include "local_display_qt_scada_runtime.hpp"

// This is PCS feedback, not the independent EMS platform-control permission.
inline QString pcsRemoteBlockReason(const ScadaSceneRuntimeSource& runtime,
                                    const std::string& modeTag, const std::string& targetTag,
                                    std::int64_t now) {
    if (modeTag.empty()) return {};
    const auto mode = runtime.resolveTag(modeTag);
    const auto target = runtime.resolveTag(targetTag);
    if (!mode || !target || mode->tag.nodeId != runtime.nodeId() ||
        target->tag.nodeId != runtime.nodeId() || mode->tag.deviceId.empty() ||
        mode->tag.deviceId != target->tag.deviceId || mode->mapping.index == 0)
        return QString::fromUtf8("PCS 远程/就地反馈绑定无效，未下发命令。");
    const auto value = runtime.readTag(modeTag, now);
    if (!value || value->quality != 1 || value->stale || !std::isfinite(value->value) ||
        value->ts <= 0 || value->ts > now || now - value->ts > 5000 ||
        (value->expireAt > 0 && value->expireAt <= now) ||
        (value->value != 0.0 && value->value != 1.0))
        return QString::fromUtf8("PCS 远程/就地反馈无效或已过期，未下发命令。请检查设备通信。");
    return value->value == 1.0 ? QString() : QString::fromUtf8(
        "PCS 当前处于就地模式，不接受本界面的控制，未下发命令。\n"
        "请由现场人员确认安全条件并将 PCS 切换到远程模式。\n"
        "右上角的 EMS 控制权限不能替代 PCS 自身的远程/就地设置。");
}

inline bool requirePcsRemote(QWidget* parent, const ScadaSceneRuntimeSource& runtime,
                             const std::string& modeTag, const std::string& targetTag,
                             std::int64_t now) {
    const auto reason = pcsRemoteBlockReason(runtime, modeTag, targetTag, now);
    if (reason.isEmpty()) return true;
    QMessageBox::warning(parent, QString::fromUtf8("PCS 控制不可用"), reason);
    return false;
}

inline void refreshPcsRemoteButton(QPushButton* button, const QString& reason) {
    // Keep it clickable so a blocked control can explain why it is unavailable.
    if (!button->property("pcsGuardStyled").toBool()) {
        button->setProperty("pcsGuardStyled", true);
        button->setProperty("pcsNormalIcon", button->icon());
        button->setStyleSheet(button->styleSheet() + QStringLiteral(
            "QPushButton[pcsRemoteBlocked=\"true\"]{color:#858B91;background:#EEF0F2;border-color:#C9CED3;}"
            "QPushButton[pcsRemoteBlocked=\"true\"]:pressed{background:#E3E6E9;}"));
    }
    const bool blocked = !reason.isEmpty();
    if (!button->property("pcsRemoteBlocked").isValid() ||
        button->property("pcsRemoteBlocked").toBool() != blocked) {
        button->setProperty("pcsRemoteBlocked", blocked);
        const auto icon = qvariant_cast<QIcon>(button->property("pcsNormalIcon"));
        button->setIcon(blocked ? QIcon(icon.pixmap(button->iconSize(), QIcon::Disabled)) : icon);
        button->style()->unpolish(button);
        button->style()->polish(button);
        button->update();
    }
    button->setToolTip(blocked ? reason : QString::fromUtf8("PCS 远程模式，允许控制"));
    button->setAccessibleDescription(button->toolTip());
}
