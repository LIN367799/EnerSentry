// src/business/SboStateMachine.h
// L4 业务层 —— SBO 控制状态机（ENS-LLD-400 §3）。
//
// 状态：Idle → Selecting → Armed → Executed / Timeout / Aborted → Idle
// 关键设计：
//   * QTimer 全部在本对象（独立 SBO 线程）创建/启停 — 绝不传给 DeviceSboGuard
//     （LLD §3.2 Code Review 修复项 ②）
//   * 三定时器：m_armedTimer(5s/3s 单发) / m_flappingTimer(500ms 抖动窗口)
//               / m_execTimer(2s 执行监护)
//   * 链路恢复时显式 stop m_flappingTimer（LLD §3.4 盲点 ③）
//   * onLinkStatusChanged(false) 启动 500ms 抖动窗口（防瞬时闪断误清）
//
// Phase 3 切片 10 (4.3.3) 落地，2026-08-30。

#pragma once

#include <ens/export.hpp>
#include "DeviceSboGuard.h"

#include <QObject>
#include <QTimer>
#include <QString>
#include <cstdint>
#include <optional>

namespace ens::business {

/// SBO 状态机状态（与 HLD §5.5 / LLD §3.1 对齐）
enum class SBOState : uint8_t {
    Idle     = 0,
    Selecting = 1,
    Armed    = 2,
    Executed = 3,
    Timeout  = 4,
    Aborted  = 5,
};

/// SBO Select 请求（来自 UI / 控制流）
struct SboSelectRequest {
    uint32_t linkId       = 0;
    uint32_t slaveId      = 0;
    uint32_t registerAddr = 0;
    uint16_t value        = 0;    ///< 预置值（FC06 写单保持寄存器）
    bool     emergency    = false; ///< 急停：超时窗口缩为 3s（LLD §3.4 ADR-16）
};

/// SBO 控制命令（实际下发到设备的字节由 L1/L2 组装，此处只表示意图）
struct SboCommand {
    uint32_t linkId       = 0;
    uint32_t slaveId      = 0;
    uint32_t registerAddr = 0;
    uint16_t value        = 0;
};

class ENS_BUSINESS_API SboStateMachine : public QObject {
    Q_OBJECT
public:
    explicit SboStateMachine(QObject* parent = nullptr);
    ~SboStateMachine() override;

    /// 注入设备级锁守卫（IoC；不持所有权）
    void setGuard(DeviceSboGuard* guard) { m_guard = guard; }

    SBOState currentState() const noexcept { return m_state; }
    QString currentSequenceId() const { return m_sequenceId; }
    std::optional<SboDeviceKey> heldKey() const { return m_heldKey; }

    // ═══ 控制流入口 ═══
    /// UI/控制层调用：发起 Select（前置 RBAC 校验）
    /// @return true 状态机接受；false 拒绝（如非 Idle 态）
    bool submitSelect(const SboSelectRequest& req, const QString& operatorName);

    /// UI/控制层调用：Operate 二次确认
    /// @return true 接受并进入 Executed；false 拒绝（非 Armed 或 sequenceId 不匹配）
    bool submitOperate(const QString& sequenceId);

    /// UI/控制层调用：取消（Select/Operate 中任意时刻）
    bool submitCancel(const QString& sequenceId);

    /// 设备回执：成功/失败
    void onDeviceAck(const QString& sequenceId, bool success, const QString& reason = {});

    // ═══ 外部订阅 ─══
    /// 由 ModbusEngine / IChannel::connectionChanged 转发
    void onLinkStatusChanged(bool connected);

    /// 测试钩子：直接强制 timeout（无需等真实 5s）
    void forceArmedTimeoutForTest();
    void forceLinkDownForTest(int flappingMs = 0);

signals:
    /// 切片 21：统一状态变更信号（UI 按钮态刷新；LLD-500 5.1.3 ControlPanel 绑定）
    void sboStateChanged(ens::business::SBOState state);
    void armedAcquired(const QString& sequenceId, const SboDeviceKey& key);
    void armedRejected(const QString& sequenceId, const SboDeviceKey& key,
                       const QString& busyBy, int64_t elapsedMs);
    void armedCleared(const QString& reason);                     ///< 进入 Idle
    void executingSucceeded(const QString& sequenceId, const QString& device);
    void executingFailed(const QString& sequenceId, const QString& device,
                         const QString& reason);
    /// 内部 → 外部下发（由 ModbusEngine 订阅）
    void commandReady(const SboCommand& cmd);

private slots:
    void onArmedTimeout();
    void onFlappingTimeout();
    void onExecutingTimeout();

private:
    void enterArmedState(const SboSelectRequest& req, const QString& operatorName);
    void transitionTo(SBOState next);
    void forceAbort(const QString& reason);

    SBOState                     m_state = SBOState::Idle;
    QString                      m_sequenceId;
    QString                      m_operator;
    SboCommand                   m_pendingCommand;
    std::optional<SboDeviceKey>  m_heldKey;
    DeviceSboGuard*              m_guard = nullptr;
    QTimer*                      m_armedTimer    = nullptr;   ///< 5s/3s 单发倒计时
    QTimer*                      m_flappingTimer = nullptr;   ///< 链路抖动 500ms 窗口
    QTimer*                      m_execTimer     = nullptr;   ///< Executing 2s 监护
    bool                         m_linkConnected = true;
};

}  // namespace ens::business