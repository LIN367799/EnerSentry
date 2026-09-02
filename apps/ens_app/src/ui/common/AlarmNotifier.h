// src/ui/common/AlarmNotifier.h —— 严重告警通知决策（切片 37，FR-AL-06）。
// 决策层与展示层分离：
//   * 本类只判定"什么情况该通知用户"——Critical 级 + 1s 防抖聚合 + 风暴模式抑制；
//   * 展示（红色弹窗 / 蜂鸣 / 任务栏闪烁）由 MainWindow 消费 criticalAlarm 信号实现。
// 防抖用单调时钟判定（steady_clock，无 QTimer/事件循环依赖）→ 纯逻辑可脱离 GUI 单测。
#pragma once

#include <QObject>

#include <cstdint>
#include <limits>

#include "AlarmEntities.h"   // AlarmEvent 完整类型（信号参数）

namespace ens::business {
class AlarmEngine;
}  // namespace ens::business

namespace ens::ui {

class AlarmNotifier : public QObject {
    Q_OBJECT
public:
    /// @param alarm 告警引擎（不拥有；null → 本对象空转，容忍）
    explicit AlarmNotifier(ens::business::AlarmEngine* alarm, QObject* parent = nullptr);
    ~AlarmNotifier() override;

    AlarmNotifier(const AlarmNotifier&) = delete;
    AlarmNotifier& operator=(const AlarmNotifier&) = delete;

    // ── 诊断/测试观测 ──
    /// 自构造以来收到的 Critical 事件总数（含被抑制/防抖丢弃的）
    int criticalReceived() const noexcept { return m_criticalReceived; }
    /// 实际发出的通知次数（防抖 + 风暴抑制后）
    int notifyCount() const noexcept { return m_notifyCount; }
    /// 被风暴模式抑制的 Critical 数
    int suppressedByStorm() const noexcept { return m_suppressedByStorm; }
    /// 最近一次通过的通知事件（展示层取内容；未通知过则 triggerTime=0）
    const ens::business::AlarmEvent& lastEvent() const noexcept { return m_last; }

signals:
    /// 决策通过（Critical 且不在风暴且防抖窗口外）→ 展示层弹窗/蜂鸣/闪烁
    void criticalAlarm(const ens::business::AlarmEvent& ev);

private slots:
    void onAlarmTriggered(const ens::business::AlarmEvent& ev);

private:
    static constexpr int64_t kDebounceMs = 1000;   // FR-AL-06 UI 防抖：1s 内至多一弹

    ens::business::AlarmEngine* m_alarm = nullptr;   // 不拥有
    int  m_criticalReceived  = 0;
    int  m_notifyCount       = 0;
    int  m_suppressedByStorm = 0;
    int64_t m_lastNotifyMono = (std::numeric_limits<int64_t>::min)() / 2;  // 首条必通知
    ens::business::AlarmEvent m_last;
};

}  // namespace ens::ui
