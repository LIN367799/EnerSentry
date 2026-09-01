// src/ui/controls/SBOControlWidget.cpp —— SBO 控制面板实现（切片 21）。
#include "controls/SBOControlWidget.h"
#include "ui_SBOControlWidget.h"

namespace ens::ui {

namespace {
QString stateText(ens::business::SBOState s) {
    using ens::business::SBOState;
    switch (s) {
        case SBOState::Idle:      return QStringLiteral("待命 (Idle)");
        case SBOState::Selecting: return QStringLiteral("选择中 (Selecting)");
        case SBOState::Armed:     return QStringLiteral("已武装 (Armed, 5s 内执行)");
        case SBOState::Executed:  return QStringLiteral("已执行 (Executed)");
        case SBOState::Timeout:   return QStringLiteral("超时 (Timeout)");
        case SBOState::Aborted:   return QStringLiteral("已中止 (Aborted)");
    }
    return QStringLiteral("?");
}
}  // namespace

SBOControlWidget::SBOControlWidget(ens::business::SboStateMachine* sm,
                                   SubmitSelectFn select, SubmitOperateFn operate,
                                   SubmitCancelFn cancel, QWidget* parent)
    : QWidget(parent), ui(new Ui::SBOControlWidget), m_sm(sm),
      m_select(std::move(select)), m_operate(std::move(operate)), m_cancel(std::move(cancel)) {
    ui->setupUi(this);

    connect(ui->btnSelect,  &QPushButton::clicked, this, &SBOControlWidget::onSelectClicked);
    connect(ui->btnOperate, &QPushButton::clicked, this, &SBOControlWidget::onOperateClicked);
    connect(ui->btnCancel,  &QPushButton::clicked, this, &SBOControlWidget::onCancelClicked);

    if (m_sm) {
        connect(m_sm, &ens::business::SboStateMachine::sboStateChanged,
                this, &SBOControlWidget::onStateChanged);
        updateButtons(m_sm->currentState());
    } else {
        updateButtons(ens::business::SBOState::Idle);
    }
}

SBOControlWidget::~SBOControlWidget() {
    delete ui;
}

void SBOControlWidget::onStateChanged(ens::business::SBOState s) {
    updateButtons(s);
}

void SBOControlWidget::updateButtons(ens::business::SBOState s) {
    using ens::business::SBOState;
    ui->lblState->setText(stateText(s));
    switch (s) {
        case SBOState::Idle:
            ui->btnSelect->setEnabled(true);
            ui->btnOperate->setEnabled(false);
            ui->btnCancel->setEnabled(false);
            break;
        case SBOState::Selecting:
            ui->btnSelect->setEnabled(false);
            ui->btnOperate->setEnabled(false);
            ui->btnCancel->setEnabled(true);
            break;
        case SBOState::Armed:
            ui->btnSelect->setEnabled(false);
            ui->btnOperate->setEnabled(true);
            ui->btnCancel->setEnabled(true);
            break;
        default:   // Executed / Timeout / Aborted：等待状态机回 Idle
            ui->btnSelect->setEnabled(false);
            ui->btnOperate->setEnabled(false);
            ui->btnCancel->setEnabled(false);
            break;
    }
}

void SBOControlWidget::onSelectClicked() {
    if (!m_sm || !m_select) return;
    ens::business::SboSelectRequest req;
    req.slaveId      = static_cast<uint32_t>(ui->spinSlave->value());
    req.registerAddr = static_cast<uint32_t>(ui->spinReg->value());
    req.value        = static_cast<uint16_t>(ui->spinValue->value());
    req.emergency    = ui->checkEmergency->isChecked();
    if (!m_select(req)) {
        ui->lblState->setText(QStringLiteral("Select 被拒绝（状态机非 Idle）"));
    }
}

void SBOControlWidget::onOperateClicked() {
    if (!m_sm || !m_operate) return;
    const QString seq = m_sm->currentSequenceId();
    if (!m_operate(seq)) {
        ui->lblState->setText(QStringLiteral("Operate 被拒绝（非 Armed 或序列不匹配）"));
    }
}

void SBOControlWidget::onCancelClicked() {
    if (!m_sm || !m_cancel) return;
    const QString seq = m_sm->currentSequenceId();
    m_cancel(seq);
}

}  // namespace ens::ui
