// src/ui/controls/SBOControlWidget.h —— L5 SBO 安全控制（ENS-LLD-508，切片 21）。
// 绑定 SboStateMachine::sboStateChanged：按钮态随状态机刷新
// （Idle→Select / Selecting→Cancel / Armed→Operate+Cancel，FR-CTRL-02 Select Before Operate）。
// 下发经注入回调（main.cpp lambda 绑定 EnerSentryApp::submitSboXxx）——
// ens::ui 不依赖 app 层（LLD-500 §0.4 铁律）。
#pragma once

#include <QWidget>

#include <functional>

#include "SboStateMachine.h"   // SBOState/SboSelectRequest 完整类型（信号参数 + 请求构造）

namespace Ui {
class SBOControlWidget;
}

namespace ens::ui {

class SBOControlWidget : public QWidget {
    Q_OBJECT
public:
    using SubmitSelectFn  = std::function<bool(const ens::business::SboSelectRequest&)>;
    using SubmitOperateFn = std::function<bool(const QString& sequenceId)>;
    using SubmitCancelFn  = std::function<bool(const QString& sequenceId)>;

    SBOControlWidget(ens::business::SboStateMachine* sm,
                     SubmitSelectFn select, SubmitOperateFn operate, SubmitCancelFn cancel,
                     QWidget* parent = nullptr);
    ~SBOControlWidget() override;

private slots:
    void onSelectClicked();
    void onOperateClicked();
    void onCancelClicked();
    void onStateChanged(ens::business::SBOState s);

private:
    void updateButtons(ens::business::SBOState s);

    Ui::SBOControlWidget* ui;
    ens::business::SboStateMachine* m_sm;
    SubmitSelectFn  m_select;
    SubmitOperateFn m_operate;
    SubmitCancelFn  m_cancel;
};

}  // namespace ens::ui
