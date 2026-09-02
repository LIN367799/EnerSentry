// src/ui/common/AlarmNotifier.cpp —— 严重告警通知决策实现（切片 37）。
#include "common/AlarmNotifier.h"

#include "AlarmEngine.h"

#include <chrono>

namespace ens::ui {

namespace {
int64_t monoNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

AlarmNotifier::AlarmNotifier(ens::business::AlarmEngine* alarm, QObject* parent)
    : QObject(parent), m_alarm(alarm) {
    if (m_alarm) {
        connect(m_alarm, &ens::business::AlarmEngine::alarmTriggered,
                this, &AlarmNotifier::onAlarmTriggered);
    }
}

AlarmNotifier::~AlarmNotifier() = default;

void AlarmNotifier::onAlarmTriggered(const ens::business::AlarmEvent& ev) {
    if (ev.level != ens::business::AlarmLevel::Critical) return;   // 仅严重告警声光（FR-AL-06）
    ++m_criticalReceived;

    // 风暴模式：引擎仍实时 emit Critical（设计），弹窗会刷屏 → 抑制，改计数展示
    if (m_alarm && m_alarm->isInStormMode()) {
        ++m_suppressedByStorm;
        return;
    }
    // 1s 防抖（单调时钟）：窗口内已弹过 → 仅累计不重复弹
    const int64_t nowMono = monoNowMs();
    if (nowMono - m_lastNotifyMono < kDebounceMs) return;

    m_lastNotifyMono = nowMono;
    ++m_notifyCount;
    m_last = ev;
    emit criticalAlarm(ev);
}

}  // namespace ens::ui
