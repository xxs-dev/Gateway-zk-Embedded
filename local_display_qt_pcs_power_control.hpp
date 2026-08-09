#pragma once

#include <array>
#include <cstdint>
#include <string>

#include <QWidget>

#include "edge_gateway/compat.hpp"
#include "edge_gateway/models.hpp"
#include "local_display_qt_scada_runtime.hpp"

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QTimer;

enum class PcsPhasePowerMode {
    Total,
    Phase
};

struct PcsPhasePowerPlan {
    double total = 0.0;
    std::array<double, 3> phases{{0.0, 0.0, 0.0}};
};

PcsPhasePowerPlan makePcsPhasePowerPlan(
    PcsPhasePowerMode mode,
    double total,
    const std::array<double, 3>& phases
);

struct PcsPhasePowerBindings {
    std::array<std::string, 3> activeTagIds;
    std::array<std::string, 3> reactiveTagIds;

    bool complete() const;
};

class PcsPhasePowerControl final : public QWidget {
public:
    PcsPhasePowerControl(
        PcsPhasePowerBindings bindings,
        ScadaSceneRuntimeSource& runtime,
        QWidget* parent = nullptr
    );

    void refresh(std::int64_t nowMs);
    bool ready() const;

private:
    enum class PowerKind {
        Active,
        Reactive
    };

    struct PendingBatch {
        bool active = false;
        PowerKind kind = PowerKind::Active;
        std::string cmdId;
        std::array<std::string, 3> tagIds;
        std::array<double, 3> targets{{0.0, 0.0, 0.0}};
        std::array<edge_gateway::Optional<edge_gateway::WritebackResultRecord>, 3> results;
        std::int64_t requestedAt = 0;
        std::int64_t deadlineAt = 0;
    };

    void buildUi();
    void setMode(PcsPhasePowerMode mode);
    void updateDerivedInputs(PowerKind kind);
    void prepareWrite(PowerKind kind);
    void submitWrite(PowerKind kind, const PcsPhasePowerPlan& plan);
    void pollWriteback();
    void finishPending(bool timedOut);
    void refreshRow(
        PowerKind kind,
        const std::array<std::string, 3>& tagIds,
        std::array<QLabel*, 3>& currentLabels,
        std::int64_t nowMs
    );
    bool validatePlan(PowerKind kind, const PcsPhasePowerPlan& plan, QString* details) const;
    PcsPhasePowerPlan planFor(PowerKind kind) const;
    std::array<QDoubleSpinBox*, 3>& phaseInputs(PowerKind kind);
    const std::array<QDoubleSpinBox*, 3>& phaseInputs(PowerKind kind) const;
    QDoubleSpinBox* totalInput(PowerKind kind) const;
    const std::array<std::string, 3>& tagIds(PowerKind kind) const;
    QString unit(PowerKind kind) const;

    PcsPhasePowerBindings bindings_;
    ScadaSceneRuntimeSource& runtime_;
    PcsPhasePowerMode mode_ = PcsPhasePowerMode::Total;
    QPushButton* totalModeButton_ = nullptr;
    QPushButton* phaseModeButton_ = nullptr;
    QCheckBox* highPriorityCheckBox_ = nullptr;
    std::array<QDoubleSpinBox*, 3> activePhaseInputs_{{nullptr, nullptr, nullptr}};
    std::array<QDoubleSpinBox*, 3> reactivePhaseInputs_{{nullptr, nullptr, nullptr}};
    QDoubleSpinBox* activeTotalInput_ = nullptr;
    QDoubleSpinBox* reactiveTotalInput_ = nullptr;
    QPushButton* activeSubmitButton_ = nullptr;
    QPushButton* reactiveSubmitButton_ = nullptr;
    std::array<QLabel*, 3> activeCurrentLabels_{{nullptr, nullptr, nullptr}};
    std::array<QLabel*, 3> reactiveCurrentLabels_{{nullptr, nullptr, nullptr}};
    std::array<QLabel*, 3> resultLabels_{{nullptr, nullptr, nullptr}};
    QLabel* statusLabel_ = nullptr;
    QTimer* pollTimer_ = nullptr;
    PendingBatch pending_;
    bool activeInitialized_ = false;
    bool reactiveInitialized_ = false;
};
