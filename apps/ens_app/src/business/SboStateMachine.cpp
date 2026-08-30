// src/business/SboStateMachine.cpp
// L4 业务层 —— SboStateMachine 实现（ENS-LLD-400 §3.3/3.4）。
//
// 设计要点：
//   * 状态机仅在「SBO 线程」内运行；所有 QTimer 在本对象创建并由 DirectConnection
//     触发（无跨线程信号排队）。
//   * DeviceSboGuard 由外部注入（IoC），state machine 负责所有时序与决策。
//   * 链路抖动窗口（500ms）防瞬时闪断误清；恢复时显式 stop（LLD §3.4 盲点 ③）。

#include "SboStateMachine.h"

#include <atomic>
#include <chrono>

#include <QCoreApplication>
#include <QDebug>

namespace ens::business {

namespace {
/// 跨平台安全 PID（无 QCoreApplication::applicationPid 依赖；用 tick counter）
inline qint64 QCoreApplication_getPidSafe() noexcept {
    static std::atomic<quint64> seq{0};
    return static_cast<qint64>(seq.fetch_add(1, std::memory_order_relaxed));
}
}  // namespace

SboStateMachine::SboStateMachine(QObject* parent) : QObject(parent) {
    m_armedTimer = new QTimer(this);
    m_armedTimer->setSingleShot(true);
    connect(m_armedTimer, &QTimer::timeout, this, &SboStateMachine::onArmedTimeout);

    m_flappingTimer = new QTimer(this);
    m_flappingTimer->setSingleShot(true);
    m_flappingTimer->setInterval(500);                          // 500ms 抖动窗口
    connect(m_flappingTimer, &QTimer::timeout, this, &SboStateMachine::onFlappingTimeout);

    m_execTimer = new QTimer(this);
    m_execTimer->setSingleShot(true);
    m_execTimer->setInterval(2000);                              // Executing 2s 监护
    connect(m_execTimer, &QTimer::timeout, this, &SboStateMachine::onExecutingTimeout);
}

SboStateMachine::~SboStateMachine() {
    if (m_armedTimer != nullptr)    m_armedTimer->stop();
    if (m_flappingTimer != nullptr) m_flappingTimer->stop();
    if (m_execTimer != nullptr)     m_execTimer->stop();
}

// ════════════════════════════════════════════════════════════
// 控制流入口
// ════════════════════════════════════════════════════════════

bool SboStateMachine::submitSelect(const SboSelectRequest& req, const QString& operatorName) {
    SboDeviceKey key{req.linkId, req.slaveId, req.registerAddr};

    if (m_state != SBOState::Idle) {
        // 非 Idle 拒:同 key → 设备忙(让 Guard 发 rejected 信号);异 key → Idle 但 Guard 也会拒
        if (m_guard != nullptr) {
            m_guard->tryAcquire(key, "rejected-" + QString::number(QCoreApplication_getPidSafe()),
                                 operatorName, nullptr);   // 期望 false → emit armedRejected
        }
        return false;
    }

    m_sequenceId    = QStringLiteral("sbo-") + QString::number(
        static_cast<qulonglong>(QCoreApplication_getPidSafe()) * 1000ULL);
    m_operator      = operatorName;
    m_pendingCommand = SboCommand{req.linkId, req.slaveId, req.registerAddr, req.value};

    if (m_guard == nullptr) {
        m_sequenceId.clear();
        m_operator.clear();
        return false;                                       // 未注入 Guard
    }
    ArmedOccupant occ;
    if (!m_guard->tryAcquire(key, m_sequenceId, operatorName, &occ)) {
        // 已被占用:armedRejected 信号由 Guard 自身发射;状态机保持 Idle
        m_sequenceId.clear();
        m_operator.clear();
        return false;
    }

    m_heldKey = key;
    enterArmedState(req, operatorName);
    return true;
}

bool SboStateMachine::submitOperate(const QString& sequenceId) {
    if (m_state != SBOState::Armed) return false;
    if (sequenceId != m_sequenceId) return false;               // 序列不匹配拒

    // 停倒计时 → 进 Executed
    m_armedTimer->stop();
    transitionTo(SBOState::Executed);
    m_execTimer->start();

    // 释放 Guard 锁（按 §3.1 时序：Execute 时设备已占用通信，下发后即释放）
    if (m_guard != nullptr && m_heldKey.has_value()) {
        m_guard->release(*m_heldKey, m_sequenceId);
    }
    emit commandReady(m_pendingCommand);
    return true;
}

bool SboStateMachine::submitCancel(const QString& sequenceId) {
    if (sequenceId != m_sequenceId) return false;
    if (m_state != SBOState::Armed && m_state != SBOState::Selecting) return false;
    forceAbort(QStringLiteral("user_cancel"));
    return true;
}

void SboStateMachine::onDeviceAck(const QString& sequenceId, bool success,
                                   const QString& reason) {
    if (sequenceId != m_sequenceId) return;
    if (m_state != SBOState::Executed) return;

    m_execTimer->stop();
    if (success) {
        emit executingSucceeded(sequenceId, QStringLiteral("dev-%1").arg(m_pendingCommand.slaveId));
    } else {
        emit executingFailed(sequenceId, QStringLiteral("dev-%1").arg(m_pendingCommand.slaveId),
                             reason.isEmpty() ? QStringLiteral("unknown") : reason);
    }
    // 无论成败 → 回 Idle
    m_heldKey.reset();
    m_sequenceId.clear();
    m_operator.clear();
    transitionTo(SBOState::Idle);
}

void SboStateMachine::onLinkStatusChanged(bool connected) {
    m_linkConnected = connected;
    if (!connected) {
        // 启动 500ms 抖动过滤窗口
        m_flappingTimer->start();
    } else {
        // 链路恢复：立即 stop（LLD §3.4 盲点 ③）
        if (m_flappingTimer->isActive()) m_flappingTimer->stop();
    }
}

// ════════════════════════════════════════════════════════════
// 内部转换
// ════════════════════════════════════════════════════════════

void SboStateMachine::enterArmedState(const SboSelectRequest& req,
                                      const QString& /*operatorName*/) {
    transitionTo(SBOState::Armed);
    const int timeoutMs = req.emergency ? 3000 : 5000;          // 急停 3s / 常规 5s
    m_armedTimer->start(timeoutMs);
    if (m_heldKey.has_value()) {
        emit armedAcquired(m_sequenceId, *m_heldKey);
    }
}

void SboStateMachine::transitionTo(SBOState next) {
    m_state = next;
}

void SboStateMachine::forceAbort(const QString& reason) {
    if (m_state != SBOState::Armed) {
        // 可能在 Selecting/Idle 收到；仅 Armed 态需要真正 abort
        m_heldKey.reset();
        m_sequenceId.clear();
        m_operator.clear();
        return;
    }
    if (m_armedTimer->isActive())    m_armedTimer->stop();
    if (m_execTimer->isActive())     m_execTimer->stop();
    // 释放 Guard 锁
    if (m_guard != nullptr && m_heldKey.has_value()) {
        m_guard->release(*m_heldKey, m_sequenceId);
    }
    transitionTo(SBOState::Aborted);
    emit armedCleared(reason);
    // Aborted 是瞬时态，立即回 Idle
    m_heldKey.reset();
    m_sequenceId.clear();
    m_operator.clear();
    transitionTo(SBOState::Idle);
}

// ════════════════════════════════════════════════════════════
// 定时器槽
// ════════════════════════════════════════════════════════════

void SboStateMachine::onArmedTimeout() {
    if (m_state != SBOState::Armed) return;
    // 倒计时归零 → Timeout（FR-CTRL-07：必须保证 Armed 不卡死）
    if (m_guard != nullptr && m_heldKey.has_value()) {
        m_guard->release(*m_heldKey, m_sequenceId);
    }
    transitionTo(SBOState::Timeout);
    emit armedCleared(QStringLiteral("timeout"));
    m_heldKey.reset();
    m_sequenceId.clear();
    m_operator.clear();
    transitionTo(SBOState::Idle);
}

void SboStateMachine::onFlappingTimeout() {
    // 500ms 后仍未恢复 → 真实断线 → forceAbort（FR-CTRL-07）
    if (!m_linkConnected) {
        forceAbort(QStringLiteral("link_down"));
    }
}

void SboStateMachine::onExecutingTimeout() {
    if (m_state != SBOState::Executed) return;
    emit executingFailed(m_sequenceId,
                         QStringLiteral("dev-%1").arg(m_pendingCommand.slaveId),
                         QStringLiteral("device_ack_timeout"));
    m_heldKey.reset();
    m_sequenceId.clear();
    m_operator.clear();
    transitionTo(SBOState::Idle);
}

// ════════════════════════════════════════════════════════════
// 测试钩子
// ════════════════════════════════════════════════════════════

void SboStateMachine::forceArmedTimeoutForTest() {
    onArmedTimeout();
}

void SboStateMachine::forceLinkDownForTest(int flappingMs) {
    m_linkConnected = false;
    m_flappingTimer->setInterval(flappingMs > 0 ? flappingMs : 500);
    m_flappingTimer->start();
}

}  // namespace ens::business