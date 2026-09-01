// src/sim/scenario_script.h
// 测试台 ── 场景脚本驱动（DevGuide §4B B9 切片 15 / ENS-SIM-IMP §7/§8 / ENS-LLD-SIM §2.2.6）。
//
// 职责：
//   - 解析 `scenarios/*.json`（steps:[{t, action, fault, scope, slave, register, count,
//     targetValue, rampRate, durationMs, alarmBit}]，schema 见 SIM-IMP §7）
//   - 按 t（相对场景开始毫秒）排程驱动 FaultInjector：INJECT 触发 / RECOVER 显式恢复；
//     durationMs>0 由 FaultSession 自然到期自动 RECOVERING（随机断链压测依赖）
//   - 产出 sim_events.jsonl（FAULT_INJECT / FAULT_RECOVER 事件流）+ sim_report.json
//     （result ∈ {PASS, FAIL, INCONCLUSIVE}，SIM-IMP §8.2 schema）
//
// 关键设计决策（切片 15，2026-09-01）：
//   R1 不持有自己的线程：SimulatorEngine::dataTickLoop 每 tick 调 drive(nowMs)，
//      时钟 = tickIndex × tickMs（确定性，NFR-TEST-01）
//   R2 无 Qt / 无 nlohmann 之外的依赖：纯 C++17 + nlohmann_json
//   R3 scope 字面量（cluster/cell/pcs/link）→ 展开为多个 POINT/SLAVE FaultRequest：
//      - cluster/cell：按 pointName 后缀 "_<regName>"（或 "_<regName>_%03d"）从点表解析
//        registerAddr（全量/样例布局通用，不硬码偏移）
//      - pcs/link（CommLoss）：SLAVE scope 单请求（B8 教训：POINT 写精确 key 不命中
//        linkEffect 的 SLAVE/ALL 通配）
//   R4 alarmBit 仅记录进事件 detail / report notes（告警字置位已在 PointGenerator 完成）
//   R5 slave=ALL 展开 = 遍历点表全部 slave 中含目标寄存器后缀的点
//   R6 单个 step 触发失败不中断其余 step；任一失败 → report.result=FAIL
//   R7 RECOVER 匹配：key = fault|scope|slaveSpec|regName → INJECT 时的 handle 栈（FIFO）

#pragma once

#include "core/point_table.h"
#include "sim/FaultInjector.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ens::sim {

// ─────────────────────────────────────────────────────────────────────────────
// 场景步骤（drill JSON steps[] 元素的 C++ 镜像）
// ─────────────────────────────────────────────────────────────────────────────
struct ScenarioStep {
    int64_t     tMs        = 0;     ///< 相对场景开始的毫秒偏移
    std::string action;             ///< "INJECT" / "RECOVER"
    std::string fault;              ///< FR-SIM-05a..05e
    std::string scope;              ///< 原始字面量：cluster / cell / pcs / link
    std::string slaveSpec;          ///< "ALL" 或十进制字符串
    std::string regName;            ///< 寄存器名后缀：MaxTemp / CellV / ...（可选）
    int         count       = 1;    ///< cell 展开数（voltage_fault_drill count=8）
    float       targetValue = 0.0f; ///< 目标工程值（℃ / V）
    float       rampRate    = 0.0f; ///< 每秒变化量（0=瞬时）
    int64_t     durationMs  = 0;    ///< ACTIVE 持续；0=永久（显式 RECOVER）
    int         alarmBit    = -1;   ///< -1=未指定；0=OverTemp 1=OverVolt 2=UnderVolt（记录用）
    bool        fired       = false;///< 已触发（防重）
};

// ─────────────────────────────────────────────────────────────────────────────
// 汇总报告（sim_report.json 的 C++ 镜像，SIM-IMP §8.2）
// ─────────────────────────────────────────────────────────────────────────────
struct ScenarioReport {
    std::string scenarioName;
    int64_t     durationMs = 0;
    std::vector<std::pair<std::string, int>> faultsInjected;  ///< fault code -> count
    int         badFramesDropped = 0;
    int         linkLossEvents   = 0;
    std::string result;             ///< PASS / FAIL / INCONCLUSIVE
    std::string notes;
};

// ─────────────────────────────────────────────────────────────────────────────
// ScenarioScript —— 场景脚本（无线程，由引擎 tick 驱动）
// ─────────────────────────────────────────────────────────────────────────────
class ScenarioScript {
public:
    ScenarioScript() = default;

    /// 解析 drill JSON；失败返回 false（错误信息写入 notes）
    bool load(const std::string& path);

    /// 重置：清 fired 标志 / handle 栈 / 事件流 / 报告（重跑同一脚本）
    void reset();

    const std::string& name() const noexcept { return m_name; }
    bool loaded() const noexcept { return !m_steps.empty(); }
    size_t stepCount() const noexcept { return m_steps.size(); }
    /// 最后 step 的结束时刻（t + durationMs），场景总时长参考
    int64_t lastStepEndMs() const noexcept;
    /// 全部 step 已触发
    bool allFired() const noexcept;
    bool finished() const noexcept { return m_finished; }

    /// 时钟驱动：nowMs 相对场景开始；触发到期 step。
    /// @return 本次新触发数（INJECT + RECOVER）
    int drive(int64_t nowMs, FaultInjector& fi, const SimPointTable& pt);

    /// 结束场景并生成最终报告（aborted=true 表示被提前终止 → INCONCLUSIVE）
    void finishReport(int64_t durationMs, bool aborted);

    /// 序列化（SIM-IMP §8.1/§8.2 schema）
    std::string reportJson() const;
    std::string eventsJsonl() const;

    /// 解析辅助（单测可见）：FR-SIM-05x → FaultType；未知码返回 nullopt
    static std::optional<FaultType> faultTypeFromCode(const std::string& code) noexcept;

private:
    void fireInject(const ScenarioStep& s, FaultInjector& fi, const SimPointTable& pt);
    void fireRecover(const ScenarioStep& s, FaultInjector& fi);
    /// 按 pointName 后缀找 registerAddr；idx>=0 时匹配 "_<regName>_%03d" / "_<regName>_<idx>"
    static const SimPoint* findPointBySuffix(const SimPointTable& pt, uint8_t slave,
                                             const std::string& regName, int idx) noexcept;
    void appendEvent(const char* level, const char* event, uint8_t slave, uint16_t reg,
                     const std::string& fault, const std::string& detail);
    std::string handleKey(const ScenarioStep& s) const noexcept;

    std::vector<ScenarioStep> m_steps;
    std::string m_name;
    int64_t m_startMs    = 0;     ///< 场景开始时刻（单调时钟 ms，drive 首次调用记录）
    bool    m_started    = false;
    bool    m_finished   = false;
    bool    m_anyFail    = false;
    std::vector<std::string> m_events;   ///< JSONL 行（内存累积，main_gui 落盘）
    ScenarioReport m_report;
    /// RECOVER 匹配栈：key -> 对应 INJECT 的 handle（FIFO）
    std::unordered_map<std::string, std::vector<FaultHandle>> m_handles;
};

}  // namespace ens::sim
