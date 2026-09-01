// src/business/AlarmEntities.h
// L4 业务层 —— 告警引擎纯数据结构（ENS-LLD-400 §2.2）。
//
// 设计要点：
//   * 与 DBDD alarm_record 字段一一对应（含 confirm_user/confirm_time）。
//   * 不含 QObject 派生；纯 POD + std::string，便于 Qt::QueuedConnection 跨线程
//     marshalling（Q_DECLARE_METATYPE 在 .cpp 中注册）。
//   * AlarmLevel/AlarmStatus 数值与 DBDD §4.4 alarm_record 表 schema 锁死。
//
// Phase 3 切片 10 (4.3.2) 落地，2026-08-30。

#pragma once

#include <cstdint>
#include <string>

namespace ens::business {

/// 告警级别（与 DBDD alarm_record.level 一致：0=Info 1=Warning 2=Critical）
enum class AlarmLevel : uint8_t { Info = 0, Warning = 1, Critical = 2 };

/// 告警状态机（与 DBDD alarm_record.status 对齐）
enum class AlarmStatus : uint8_t { Active = 0, Confirmed = 1, Recovered = 2 };

/// 告警方向（切片 29：低告警支持，如 SOC 低于阈值）
/// High：value > onThreshold 触发，回落 < offThreshold 恢复（迟滞带 [off, on]）
/// Low ：value < onThreshold 触发，回升 > offThreshold 恢复（迟滞带 [on, off]，off > on）
enum class AlarmDirection : uint8_t { High = 0, Low = 1 };

/// 告警规则（配置热加载，FR-CFG-06）
struct AlarmRule {
    uint32_t pointId    = 0;
    AlarmLevel level    = AlarmLevel::Warning;
    AlarmDirection direction = AlarmDirection::High;   ///< 触发方向（默认高告警，兼容既有规则）
    float onThreshold   = 0.0f;   ///< 越过即触发（进入 On-Delay）
    float offThreshold  = 0.0f;   ///< 迟滞：高告警需回落至该值以下恢复；低告警需回升至该值以上恢复
    bool  enabled       = true;
    /// 延时确认（FR-AL-05）：持续越界 onDelayMs 后才正式产生告警；恢复同理需
    /// 持续处于恢复侧 offDelayMs 后才置 Recovered。
    uint32_t onDelayMs  = 3000;   ///< 默认 3s
    uint32_t offDelayMs = 3000;
    /// 同源抑制（FR-AL-04）：同一 pointId 同级别，60s 内已产生过则不再重复触发
    uint32_t suppressWindowMs = 60000;
};

/// 风暴抑制配置（ADR-10）
struct AlarmStormConfig {
    uint32_t windowMs        = 1000;   ///< 滑动窗口 1s
    uint32_t threshold       = 200;    ///< 窗口内告警数超阈值即进入风暴模式
    uint32_t flushIntervalMs = 200;    ///< 合并批刷出间隔
};

/// 告警恢复原因（用于审计与 UI 区分，避免"规则删除"导致告警悬空）
enum class AlarmRecoveryReason : uint8_t {
    Normal      = 0,   ///< 测点值恢复至 offThreshold 以下（默认）
    RuleRemoved = 1,   ///< 管理员热重载/删除规则，强制恢复
    Manual      = 2,   ///< 人工确认恢复（未来扩展）
    LinkDown    = 3,   ///< 通信链路中断（未来扩展）
};

/// 告警事件（引擎内流转 + 落库 + UI 推送）
struct AlarmEvent {
    uint64_t    id          = 0;     ///< 全局唯一（雪花/自增，落库后回填）
    uint32_t    pointId     = 0;
    AlarmLevel  level       = AlarmLevel::Warning;
    AlarmStatus status      = AlarmStatus::Active;
    uint64_t    triggerTime = 0;     ///< Unix ms（落库/显示用）
    uint64_t    recoverTime = 0;
    float       alarmValue  = 0.0f;  ///< 触发时测点值（FR-AL-13）
    float       threshold   = 0.0f;  ///< 阈值（FR-AL-13）
    std::string description;         ///< 告警源描述
    uint64_t    blackboxId  = 0;     ///< 关联黑匣子（0=无）
    AlarmRecoveryReason recoveryReason = AlarmRecoveryReason::Normal; ///< 恢复原因
};

/// 单测点运行时判定状态（所有权归 AlarmEngine::m_states，非 void* 类型擦除）
struct PointAlarmState {
    uint32_t pointId = 0;

    // —— 迟滞锁存 ——
    bool  inAlarmBand = false;   ///< 当前是否处于越界带（含迟滞）
    float lastRaw     = 0.0f;

    // —— 同源抑制（monotonic，仅内部计时用） ——
    int64_t lastRaiseMono = 0;   ///< 上次正式产生告警的时刻（steady_clock ms）

    // —— 延时确认 On/Off-Delay（monotonic，替代逐点 QTimer，避免海量定时器） ——
    bool    pendingOn    = false; ///< 正在 On-Delay 计时
    int64_t onSinceMono  = 0;     ///< 进入候选（越界）起点
    bool    pendingOff   = false; ///< 正在 Off-Delay 计时
    int64_t offSinceMono = 0;     ///< 进入恢复候选起点

    // —— 当前活跃告警 ——
    AlarmStatus status   = AlarmStatus::Recovered;
    uint64_t    activeId = 0;     ///< 当前 Active/Confirmed 记录的 id
};

}  // namespace ens::business