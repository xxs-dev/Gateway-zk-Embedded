#include "local_display_qt_pcs_power_control.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <utility>
#include <vector>

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFont>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

namespace {

std::int64_t currentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

QString displayNumber(double value, int decimals = 3) {
    auto text = QString::number(value, 'f', decimals);
    while (text.contains('.') && text.endsWith('0')) text.chop(1);
    if (text.endsWith('.')) text.chop(1);
    return text;
}

std::string nextPcsCommandId() {
    static unsigned long sequence = 0;
    std::ostringstream out;
    out << "SCADA_PCS_" << currentTimeMs() << '_' << ++sequence;
    return out.str();
}

QString phaseName(std::size_t index) {
    static const char* names[] = {"A", "B", "C"};
    return QString::fromLatin1(names[index]);
}

void showPcsMessage(
    QWidget* parent,
    QMessageBox::Icon icon,
    const QString& title,
    const QString& message
) {
    QMessageBox dialog(icon, title, message, QMessageBox::NoButton, parent);
    auto* confirm = dialog.addButton(QString::fromUtf8("确定"), QMessageBox::AcceptRole);
    confirm->setDefault(true);
    dialog.exec();
}

}  // namespace

PcsPhasePowerPlan makePcsPhasePowerPlan(
    PcsPhasePowerMode mode,
    double total,
    const std::array<double, 3>& phases
) {
    PcsPhasePowerPlan result;
    if (mode == PcsPhasePowerMode::Total) {
        result.total = total;
        const auto perPhase = total / 3.0;
        result.phases = {{perPhase, perPhase, perPhase}};
        return result;
    }
    result.phases = phases;
    result.total = phases[0] + phases[1] + phases[2];
    return result;
}

bool PcsPhasePowerBindings::complete() const {
    const auto populated = [](const std::array<std::string, 3>& values) {
        return std::all_of(values.begin(), values.end(), [](const std::string& value) {
            return !value.empty();
        });
    };
    return populated(activeTagIds) && populated(reactiveTagIds);
}

PcsPhasePowerControl::PcsPhasePowerControl(
    PcsPhasePowerBindings bindings,
    ScadaSceneRuntimeSource& runtime,
    QWidget* parent
) : QWidget(parent), bindings_(std::move(bindings)), runtime_(runtime) {
    setObjectName(QStringLiteral("pcsPhasePowerControl"));
    buildUi();
    setMode(PcsPhasePowerMode::Total);

    pollTimer_ = new QTimer(this);
    pollTimer_->setInterval(200);
    QObject::connect(pollTimer_, &QTimer::timeout, this, [this]() { pollWriteback(); });
}

bool PcsPhasePowerControl::ready() const {
    if (!bindings_.complete()) return false;
    for (const auto* tags : {&bindings_.activeTagIds, &bindings_.reactiveTagIds}) {
        for (const auto& tagId : *tags) {
            const auto resolved = runtime_.resolveTag(tagId);
            if (!resolved || resolved->tag.nodeId != runtime_.nodeId() ||
                resolved->mapping.index == 0 || !resolved->mapping.writable) {
                return false;
            }
        }
    }
    return true;
}

void PcsPhasePowerControl::buildUi() {
    setStyleSheet(QString::fromUtf8(R"CSS(
        QWidget#pcsPhasePowerControl { background:#081E29; color:#EAF6FA; border:1px solid #285164; }
        QLabel { color:#D8EAF0; background:transparent; border:none; }
        QLabel#pcsTitle { font-size:18px; font-weight:700; color:#F5FBFD; }
        QLabel#pcsHint { color:#7FA8BA; }
        QLabel#pcsStatus { color:#80D8E8; padding:5px 8px; background:#0B2633; border:1px solid #204858; }
        QLabel#pcsResult { color:#A8C8D5; padding:5px 7px; background:#0A202B; border:1px solid #193D4B; }
        QPushButton { min-height:30px; padding:3px 12px; color:#EAF6FA; background:#0C2B38; border:1px solid #2A6074; }
        QPushButton:hover { background:#123A49; border-color:#48C7DC; }
        QPushButton:pressed { background:#071A23; }
        QPushButton:disabled { color:#68818A; background:#10232B; border-color:#28404A; }
        QPushButton:checked { color:#03151D; background:#62D6E6; border-color:#8BE7F1; font-weight:700; }
        QDoubleSpinBox { min-height:28px; padding:2px 7px; color:#D9F7FF; background:#071A24; border:1px solid #285164; selection-background-color:#2A7A91; }
        QDoubleSpinBox:read-only { color:#8EB3C1; background:#0B222D; }
        QCheckBox { color:#D8EAF0; spacing:8px; }
        QMessageBox { background:#081E29; color:#EAF6FA; border:1px solid #285164; }
        QMessageBox QLabel { color:#D8EAF0; background:transparent; border:none; }
    )CSS"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 10, 12, 10);
    root->setSpacing(7);

    auto* header = new QHBoxLayout();
    auto* title = new QLabel(QString::fromUtf8("PCS 三相功率设定"));
    title->setObjectName(QStringLiteral("pcsTitle"));
    header->addWidget(title);
    header->addStretch(1);
    totalModeButton_ = new QPushButton(QString::fromUtf8("合相设定"));
    totalModeButton_->setCheckable(true);
    totalModeButton_->setObjectName(QStringLiteral("pcsTotalModeButton"));
    phaseModeButton_ = new QPushButton(QString::fromUtf8("分相设定"));
    phaseModeButton_->setCheckable(true);
    phaseModeButton_->setObjectName(QStringLiteral("pcsPhaseModeButton"));
    header->addWidget(totalModeButton_);
    header->addWidget(phaseModeButton_);
    root->addLayout(header);

    QObject::connect(totalModeButton_, &QPushButton::clicked, this, [this]() {
        setMode(PcsPhasePowerMode::Total);
    });
    QObject::connect(phaseModeButton_, &QPushButton::clicked, this, [this]() {
        setMode(PcsPhasePowerMode::Phase);
    });

    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(7);
    grid->setVerticalSpacing(5);
    const QStringList headings = {
        QString::fromUtf8("参数"), QString::fromUtf8("A 相"), QString::fromUtf8("B 相"),
        QString::fromUtf8("C 相"), QString::fromUtf8("合相"), QString::fromUtf8("操作")
    };
    for (int column = 0; column < headings.size(); ++column) {
        auto* label = new QLabel(headings[column]);
        label->setAlignment(Qt::AlignCenter);
        grid->addWidget(label, 0, column);
    }

    const auto makeInput = [this](const QString& objectName) {
        auto* input = new QDoubleSpinBox(this);
        input->setObjectName(objectName);
        input->setRange(-100000.0, 100000.0);
        input->setDecimals(3);
        input->setSingleStep(0.1);
        input->setKeyboardTracking(false);
        input->setAlignment(Qt::AlignRight);
        return input;
    };

    auto addPowerRow = [&](
        int row,
        PowerKind kind,
        const QString& labelText,
        std::array<QDoubleSpinBox*, 3>& phases,
        QDoubleSpinBox*& total,
        QPushButton*& submit
    ) {
        auto* label = new QLabel(labelText);
        grid->addWidget(label, row, 0);
        for (std::size_t index = 0; index < phases.size(); ++index) {
            phases[index] = makeInput(QStringLiteral("pcs%1Phase%2")
                .arg(kind == PowerKind::Active ? QStringLiteral("Active") : QStringLiteral("Reactive"))
                .arg(phaseName(index)));
            grid->addWidget(phases[index], row, static_cast<int>(index) + 1);
            QObject::connect(
                phases[index],
                qOverload<double>(&QDoubleSpinBox::valueChanged),
                this,
                [this, kind](double) { updateDerivedInputs(kind); }
            );
        }
        total = makeInput(QStringLiteral("pcs%1Total")
            .arg(kind == PowerKind::Active ? QStringLiteral("Active") : QStringLiteral("Reactive")));
        grid->addWidget(total, row, 4);
        QObject::connect(
            total,
            qOverload<double>(&QDoubleSpinBox::valueChanged),
            this,
            [this, kind](double) { updateDerivedInputs(kind); }
        );
        submit = new QPushButton(QString::fromUtf8("校验并确认"));
        submit->setObjectName(kind == PowerKind::Active
            ? QStringLiteral("pcsActiveSubmitButton")
            : QStringLiteral("pcsReactiveSubmitButton"));
        grid->addWidget(submit, row, 5);
        QObject::connect(submit, &QPushButton::clicked, this, [this, kind]() { prepareWrite(kind); });
    };

    addPowerRow(
        1, PowerKind::Active, QString::fromUtf8("有功 P (kW)"),
        activePhaseInputs_, activeTotalInput_, activeSubmitButton_
    );
    addPowerRow(
        2, PowerKind::Reactive, QString::fromUtf8("无功 Q (kVar)"),
        reactivePhaseInputs_, reactiveTotalInput_, reactiveSubmitButton_
    );
    root->addLayout(grid);

    auto* currentGrid = new QGridLayout();
    currentGrid->setHorizontalSpacing(7);
    currentGrid->addWidget(new QLabel(QString::fromUtf8("当前回读")), 0, 0);
    for (std::size_t index = 0; index < 3; ++index) {
        activeCurrentLabels_[index] = new QLabel(QStringLiteral("%1相 P: --").arg(phaseName(index)));
        reactiveCurrentLabels_[index] = new QLabel(QStringLiteral("%1相 Q: --").arg(phaseName(index)));
        currentGrid->addWidget(activeCurrentLabels_[index], 0, static_cast<int>(index) + 1);
        currentGrid->addWidget(reactiveCurrentLabels_[index], 1, static_cast<int>(index) + 1);
    }
    root->addLayout(currentGrid);

    auto* options = new QHBoxLayout();
    auto* hint = new QLabel(QString::fromUtf8("合相模式由边端按总值 / 3 生成三相目标；此按钮只做校验，确认弹窗后才真实写入。"));
    hint->setObjectName(QStringLiteral("pcsHint"));
    options->addWidget(hint, 1);
    highPriorityCheckBox_ = new QCheckBox(QString::fromUtf8("高优先级控制"));
    highPriorityCheckBox_->setObjectName(QStringLiteral("pcsHighPriorityCheckBox"));
    options->addWidget(highPriorityCheckBox_);
    root->addLayout(options);

    statusLabel_ = new QLabel(bindings_.complete()
        ? QString::fromUtf8("等待操作")
        : QString::fromUtf8("配置错误：缺少 PCS 六个分相写入点绑定"));
    statusLabel_->setObjectName(QStringLiteral("pcsStatus"));
    root->addWidget(statusLabel_);

    auto* resultGrid = new QGridLayout();
    resultGrid->setHorizontalSpacing(7);
    for (std::size_t index = 0; index < resultLabels_.size(); ++index) {
        resultLabels_[index] = new QLabel(QStringLiteral("%1相：--").arg(phaseName(index)));
        resultLabels_[index]->setObjectName(QStringLiteral("pcsResult"));
        resultLabels_[index]->setWordWrap(true);
        resultGrid->addWidget(resultLabels_[index], 0, static_cast<int>(index));
    }
    root->addLayout(resultGrid);
}

void PcsPhasePowerControl::setMode(PcsPhasePowerMode mode) {
    mode_ = mode;
    totalModeButton_->setChecked(mode == PcsPhasePowerMode::Total);
    phaseModeButton_->setChecked(mode == PcsPhasePowerMode::Phase);
    for (auto* input : activePhaseInputs_) input->setReadOnly(mode == PcsPhasePowerMode::Total);
    for (auto* input : reactivePhaseInputs_) input->setReadOnly(mode == PcsPhasePowerMode::Total);
    activeTotalInput_->setReadOnly(mode == PcsPhasePowerMode::Phase);
    reactiveTotalInput_->setReadOnly(mode == PcsPhasePowerMode::Phase);
    updateDerivedInputs(PowerKind::Active);
    updateDerivedInputs(PowerKind::Reactive);
}

void PcsPhasePowerControl::updateDerivedInputs(PowerKind kind) {
    auto& phases = phaseInputs(kind);
    auto* total = totalInput(kind);
    if (mode_ == PcsPhasePowerMode::Total) {
        const auto plan = makePcsPhasePowerPlan(mode_, total->value(), {{0.0, 0.0, 0.0}});
        for (std::size_t index = 0; index < phases.size(); ++index) {
            const QSignalBlocker blocker(phases[index]);
            phases[index]->setValue(plan.phases[index]);
        }
        return;
    }
    const auto plan = makePcsPhasePowerPlan(
        mode_,
        0.0,
        {{phases[0]->value(), phases[1]->value(), phases[2]->value()}}
    );
    const QSignalBlocker blocker(total);
    total->setValue(plan.total);
}

PcsPhasePowerPlan PcsPhasePowerControl::planFor(PowerKind kind) const {
    const auto& phases = phaseInputs(kind);
    return makePcsPhasePowerPlan(
        mode_,
        totalInput(kind)->value(),
        {{phases[0]->value(), phases[1]->value(), phases[2]->value()}}
    );
}

bool PcsPhasePowerControl::validatePlan(
    PowerKind kind,
    const PcsPhasePowerPlan& plan,
    QString* details
) const {
    QStringList errors;
    if (!bindings_.complete()) errors.push_back(QString::fromUtf8("缺少六个分相写入点绑定"));
    if (!std::isfinite(plan.total)) errors.push_back(QString::fromUtf8("合相目标不是有效数字"));

    const auto& tags = tagIds(kind);
    for (std::size_t index = 0; index < tags.size(); ++index) {
        if (!std::isfinite(plan.phases[index])) {
            errors.push_back(QStringLiteral("%1相目标不是有效数字").arg(phaseName(index)));
            continue;
        }
        const auto resolved = runtime_.resolveTag(tags[index]);
        if (!resolved) {
            errors.push_back(QStringLiteral("%1相点位未找到").arg(phaseName(index)));
            continue;
        }
        if (resolved->tag.nodeId != runtime_.nodeId()) {
            errors.push_back(QStringLiteral("%1相点位不属于当前设备").arg(phaseName(index)));
        }
        if (resolved->tag.access == edge_gateway::ScadaTagAccess::Read || !resolved->mapping.writable) {
            errors.push_back(QStringLiteral("%1相点位只读").arg(phaseName(index)));
        }
        const auto current = runtime_.readTag(tags[index], currentTimeMs());
        if ((!current || current->quality != 1 || current->stale) &&
            std::fabs(plan.phases[index]) > 1e-9) {
            errors.push_back(QStringLiteral("%1相实时数据不可用，当前仅允许零功率联调")
                .arg(phaseName(index)));
        }
        if (resolved->writeMinValue && plan.phases[index] < *resolved->writeMinValue) {
            errors.push_back(QStringLiteral("%1相低于允许下限 %2")
                .arg(phaseName(index)).arg(displayNumber(*resolved->writeMinValue)));
        }
        if (resolved->writeMaxValue && plan.phases[index] > *resolved->writeMaxValue) {
            errors.push_back(QStringLiteral("%1相高于允许上限 %2")
                .arg(phaseName(index)).arg(displayNumber(*resolved->writeMaxValue)));
        }
        if (resolved->writeStep > 0.0 && mode_ == PcsPhasePowerMode::Phase) {
            const auto base = resolved->writeMinValue ? *resolved->writeMinValue : 0.0;
            const auto ratio = (plan.phases[index] - base) / resolved->writeStep;
            if (std::fabs(ratio - std::round(ratio)) > 1e-7) {
                errors.push_back(QStringLiteral("%1相不符合步长 %2")
                    .arg(phaseName(index)).arg(displayNumber(resolved->writeStep)));
            }
        }
    }
    if (details != nullptr) *details = errors.join(QStringLiteral("\n"));
    return errors.empty();
}

void PcsPhasePowerControl::prepareWrite(PowerKind kind) {
    if (pending_.active) {
        showPcsMessage(
            this,
            QMessageBox::Information,
            QString::fromUtf8("PCS 功率写入"),
            QString::fromUtf8("上一批命令仍在等待边端回执。")
        );
        return;
    }

    const auto plan = planFor(kind);
    QString validation;
    if (!validatePlan(kind, plan, &validation)) {
        showPcsMessage(
            this,
            QMessageBox::Warning,
            QString::fromUtf8("校验未通过"),
            QString::fromUtf8("未向设备发送任何命令。\n\n") + validation
        );
        return;
    }

    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("pcsWriteConfirmDialog"));
    dialog.setWindowTitle(QString::fromUtf8("确认 PCS 三相功率写入"));
    dialog.setModal(true);
    dialog.setMinimumWidth(600);
    dialog.setStyleSheet(QString::fromUtf8(R"CSS(
        QDialog#pcsWriteConfirmDialog {
            background:#081E29;
            color:#EAF6FA;
            border:1px solid #285164;
        }
        QDialog#pcsWriteConfirmDialog QLabel {
            color:#D8EAF0;
            background:transparent;
            border:none;
        }
        QDialog#pcsWriteConfirmDialog QPushButton {
            min-width:96px;
            min-height:32px;
            padding:4px 14px;
            color:#EAF6FA;
            background:#0C2B38;
            border:1px solid #2A6074;
        }
        QDialog#pcsWriteConfirmDialog QPushButton:hover {
            background:#123A49;
            border-color:#48C7DC;
        }
        QDialog#pcsWriteConfirmDialog QPushButton:default {
            color:#03151D;
            background:#62D6E6;
            border-color:#8BE7F1;
            font-weight:700;
        }
    )CSS"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* title = new QLabel(QString::fromUtf8("校验通过，以下数据尚未下发"));
    QFont titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 2);
    title->setFont(titleFont);
    layout->addWidget(title);

    const auto unitText = unit(kind);
    const auto kindText = kind == PowerKind::Active ? QString::fromUtf8("有功 P") : QString::fromUtf8("无功 Q");
    auto* summary = new QLabel(QString::fromUtf8(
        "%1\n模式：%2\n合相目标：%3 %4\n三相目标：A %5 / B %6 / C %7 %4\n优先级：%8\n\n点击“确认下发”后将真实写入设备。"
    ).arg(kindText)
     .arg(mode_ == PcsPhasePowerMode::Total ? QString::fromUtf8("合相，总值由边端按 / 3 复算") : QString::fromUtf8("分相"))
     .arg(displayNumber(plan.total, 6))
     .arg(unitText)
     .arg(displayNumber(plan.phases[0], 6))
     .arg(displayNumber(plan.phases[1], 6))
     .arg(displayNumber(plan.phases[2], 6))
     .arg(highPriorityCheckBox_->isChecked() ? QString::fromUtf8("高优先级") : QString::fromUtf8("普通")));
    summary->setWordWrap(true);
    layout->addWidget(summary);

    auto* buttons = new QDialogButtonBox();
    auto* confirm = buttons->addButton(QString::fromUtf8("确认下发"), QDialogButtonBox::AcceptRole);
    buttons->addButton(QString::fromUtf8("取消"), QDialogButtonBox::RejectRole);
    confirm->setDefault(true);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) return;
    submitWrite(kind, plan);
}

void PcsPhasePowerControl::submitWrite(PowerKind kind, const PcsPhasePowerPlan& plan) {
    std::vector<ScadaSceneWriteTarget> targets;
    const auto& tags = tagIds(kind);
    for (std::size_t index = 0; index < tags.size(); ++index) {
        targets.push_back({tags[index], plan.phases[index]});
    }

    edge_gateway::PendingWriteCommand command;
    command.cmdId = nextPcsCommandId();
    command.source = "scada-local-pcs-power";
    command.ts = currentTimeMs();
    command.acceptedAt = command.ts;
    command.highPriority = highPriorityCheckBox_->isChecked();
    const auto result = runtime_.submitWriteGroup(targets, command);
    if (!result.accepted) {
        showPcsMessage(
            this,
            QMessageBox::Warning,
            QString::fromUtf8("下发失败"),
            QString::fromUtf8("边端拒绝命令：%1").arg(QString::fromStdString(result.message))
        );
        statusLabel_->setText(QString::fromUtf8("下发失败：%1").arg(QString::fromStdString(result.message)));
        return;
    }

    pending_ = PendingBatch{};
    pending_.active = true;
    pending_.kind = kind;
    pending_.cmdId = command.cmdId;
    pending_.tagIds = tags;
    pending_.targets = plan.phases;
    pending_.requestedAt = command.ts;
    pending_.deadlineAt = command.ts + 10000;
    activeSubmitButton_->setEnabled(false);
    reactiveSubmitButton_->setEnabled(false);
    for (std::size_t index = 0; index < resultLabels_.size(); ++index) {
        resultLabels_[index]->setText(QStringLiteral("%1相：目标 %2 %3，等待边端回执")
            .arg(phaseName(index))
            .arg(displayNumber(plan.phases[index], 6))
            .arg(unit(kind)));
    }
    statusLabel_->setText(QString::fromUtf8("命令已受理，正在等待 A/B/C 三相写入回执：%1")
        .arg(QString::fromStdString(command.cmdId)));
    pollTimer_->start();
}

void PcsPhasePowerControl::pollWriteback() {
    if (!pending_.active) {
        pollTimer_->stop();
        return;
    }
    bool complete = true;
    for (std::size_t index = 0; index < pending_.tagIds.size(); ++index) {
        if (!pending_.results[index]) {
            pending_.results[index] = runtime_.getWritebackResult(pending_.tagIds[index], pending_.cmdId);
        }
        if (!pending_.results[index]) {
            complete = false;
            continue;
        }
        const auto& result = *pending_.results[index];
        const auto actual = runtime_.readTag(pending_.tagIds[index], currentTimeMs());
        const auto actualText = actual && actual->quality == 1 && !actual->stale
            ? displayNumber(actual->value, 6)
            : QStringLiteral("--");
        resultLabels_[index]->setText(QString::fromUtf8(
            "%1相：%2\n目标 %3，回读 %4 %5\n排队 %6 ms，写入 %7 ms，总耗时 %8 ms"
        ).arg(phaseName(index))
         .arg(result.success ? QString::fromUtf8("成功") : QString::fromUtf8("失败"))
         .arg(displayNumber(pending_.targets[index], 6))
         .arg(actualText)
         .arg(unit(pending_.kind))
         .arg(result.queueDelayMs)
         .arg(result.deviceWriteMs)
         .arg(result.totalElapsedMs));
        resultLabels_[index]->setToolTip(QString::fromStdString(result.message));
    }
    if (complete) {
        finishPending(false);
    } else if (currentTimeMs() >= pending_.deadlineAt) {
        finishPending(true);
    }
}

void PcsPhasePowerControl::finishPending(bool timedOut) {
    pollTimer_->stop();
    bool allSucceeded = !timedOut;
    std::int64_t elapsed = 0;
    for (std::size_t index = 0; index < pending_.results.size(); ++index) {
        if (!pending_.results[index]) {
            allSucceeded = false;
            resultLabels_[index]->setText(QString::fromUtf8("%1相：回执超时，设备可能仍在执行")
                .arg(phaseName(index)));
            continue;
        }
        allSucceeded = allSucceeded && pending_.results[index]->success;
        elapsed = std::max(elapsed, pending_.results[index]->totalElapsedMs);
    }
    statusLabel_->setText(allSucceeded
        ? QString::fromUtf8("三相写入完成，边端总耗时 %1 ms").arg(elapsed)
        : timedOut
            ? QString::fromUtf8("等待回执超过 10 秒，请查看各相结果；超时不代表设备一定未执行")
            : QString::fromUtf8("三相写入存在失败，请查看各相原因"));
    pending_.active = false;
    activeSubmitButton_->setEnabled(true);
    reactiveSubmitButton_->setEnabled(true);
}

void PcsPhasePowerControl::refresh(std::int64_t now) {
    refreshRow(PowerKind::Active, bindings_.activeTagIds, activeCurrentLabels_, now);
    refreshRow(PowerKind::Reactive, bindings_.reactiveTagIds, reactiveCurrentLabels_, now);
    if (!pending_.active) {
        const auto available = ready();
        activeSubmitButton_->setEnabled(available);
        reactiveSubmitButton_->setEnabled(available);
        if (!available) statusLabel_->setText(QString::fromUtf8("控制不可用：点位缺失、只读或未映射到当前设备"));
    }
}

void PcsPhasePowerControl::refreshRow(
    PowerKind kind,
    const std::array<std::string, 3>& tags,
    std::array<QLabel*, 3>& currentLabels,
    std::int64_t now
) {
    std::array<double, 3> currentValues{{0.0, 0.0, 0.0}};
    bool allGood = true;
    for (std::size_t index = 0; index < tags.size(); ++index) {
        const auto current = tags[index].empty() ? edge_gateway::Optional<edge_gateway::StoredPointValue>(edge_gateway::NullOpt)
                                                : runtime_.readTag(tags[index], now);
        const auto good = current && current->quality == 1 && !current->stale;
        allGood = allGood && good;
        if (good) currentValues[index] = current->value;
        currentLabels[index]->setText(QStringLiteral("%1相 %2: %3 %4")
            .arg(phaseName(index))
            .arg(kind == PowerKind::Active ? QStringLiteral("P") : QStringLiteral("Q"))
            .arg(good ? displayNumber(current->value, 6) : QStringLiteral("--"))
            .arg(unit(kind)));
    }

    bool& initialized = kind == PowerKind::Active ? activeInitialized_ : reactiveInitialized_;
    if (initialized || !allGood || pending_.active) return;
    initialized = true;
    auto& inputs = phaseInputs(kind);
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        const QSignalBlocker blocker(inputs[index]);
        inputs[index]->setValue(currentValues[index]);
    }
    const QSignalBlocker blocker(totalInput(kind));
    totalInput(kind)->setValue(currentValues[0] + currentValues[1] + currentValues[2]);
    updateDerivedInputs(kind);
}

std::array<QDoubleSpinBox*, 3>& PcsPhasePowerControl::phaseInputs(PowerKind kind) {
    return kind == PowerKind::Active ? activePhaseInputs_ : reactivePhaseInputs_;
}

const std::array<QDoubleSpinBox*, 3>& PcsPhasePowerControl::phaseInputs(PowerKind kind) const {
    return kind == PowerKind::Active ? activePhaseInputs_ : reactivePhaseInputs_;
}

QDoubleSpinBox* PcsPhasePowerControl::totalInput(PowerKind kind) const {
    return kind == PowerKind::Active ? activeTotalInput_ : reactiveTotalInput_;
}

const std::array<std::string, 3>& PcsPhasePowerControl::tagIds(PowerKind kind) const {
    return kind == PowerKind::Active ? bindings_.activeTagIds : bindings_.reactiveTagIds;
}

QString PcsPhasePowerControl::unit(PowerKind kind) const {
    return kind == PowerKind::Active ? QStringLiteral("kW") : QStringLiteral("kVar");
}
