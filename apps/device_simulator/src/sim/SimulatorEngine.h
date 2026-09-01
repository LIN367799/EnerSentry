// src/sim/SimulatorEngine.h
// 测试台 ── 编排者（ENS-LLD-SIM §2.2.1 + DevGuide §4B B7 切片 13）。
//
// 职责：
//   - 持有并生命周期管理全部子对象（PointGenerator + ModbusSlaveEmulator + FaultInjector + RegisterBank）
//   - start(): 装 bank baseline → emu.start（启 Slave IO）→ 启动 DataTick 线程
//   - stop(): 通知 DataTick 线程退出 → join → emu.stop → 析构
//   - injectFault / recoverFault / abortFault: 线程安全（GUI 经 Qt signal queued 投递）
//
// 不持有 GUI（B10 范围）。main_gui.cpp 用 CLI 模式直接构造 + 跑。
//
// 关键设计（评审记录 2026-08-30 22:25）：
//   R1 DataTick 线程: std::thread + sleep_for(tickMs) 循环（不引入 QThread,主程序未来用 QThread 时再改）
//   R2 不做 Qt signal: 提供 C++ 公开方法 injectFault(recoverFault/abortFault),内部加锁
//   R3 start 顺序: 构造子对象 → 装 bank baseline → emu.start → 启动 DataTick 线程
//   R4 stop 逆序: 通知退出 → join → emu.stop → 析构
//   R5 线程安全: DataTick 线程独占 gen.generateTick + fi.tickSessions;
//      injectFault 等持 m_cmdMtx 调 fi 的 trigger/recover/abort
//   R6 main_gui 替换: CLI 模式（QApplication + QTimer 定时停）

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace ens::sim {

class PointGenerator;
class ModbusSlaveEmulator;
class FaultInjector;
class RegisterBank;
class SimPointTable;
class ScenarioScript;
struct SimConfig;       // forward decl 必须与 sim_config.h 定义一致(struct),否则 ABI 不匹配(MSVC mangling U vs V)
struct FaultRequest;

class SimulatorEngine {
public:
    SimulatorEngine();
    ~SimulatorEngine();

    SimulatorEngine(const SimulatorEngine&) = delete;
    SimulatorEngine& operator=(const SimulatorEngine&) = delete;

    /// 启动编排:① 构造子对象 ② 装 bank baseline ③ emu.start(启 Slave IO) ④ 启动 DataTick
    /// @return true 成功;false 任一必需子对象构造/start 失败
    bool start(const SimConfig& cfg) noexcept;

    /// 优雅关闭:① 通知 DataTick 退出 ② join 线程 ③ emu.stop ④ 析构
    void stop() noexcept;

    /// 状态查询
    bool isRunning() const noexcept { return m_running.load(std::memory_order_acquire); }

    /// 线程安全接口（GUI 线程可调,内部 m_cmdMtx 保护 + FaultInjector 内部 shared_mutex）
    /// 失败返回 0 / false
    uint32_t injectFault(const FaultRequest& req) noexcept;
    bool     recoverFault(uint32_t handle) noexcept;
    bool     abortFault(uint32_t handle) noexcept;

    /// 场景脚本（B9 切片 15）：加载 / 运行 / 查询。
    /// loadScenario 在 start 前后均可；start 时若 cfg.scenarioPath 非空自动加载。
    bool loadScenario(const std::string& path) noexcept;
    bool scenarioLoaded() const noexcept;
    /// 是否仍在驱动场景（dataTickLoop 内 allFired 后置 false）
    bool scenarioRunning() const noexcept;
    /// 场景是否全部 step 已触发
    bool scenarioAllFired() const noexcept;
    /// 报告 / 事件流（落盘用，未结束场景返回当前快照）
    std::string scenarioReportJson() const;
    std::string scenarioEventsJsonl() const;
    /// 场景名称（诊断）
    std::string scenarioName() const;

    /// 诊断:DataTick 已经循环的次数（每调一次 generateTick 增 1）
    uint64_t tickCount() const noexcept;

    /// 诊断:cfg 传入的 tickMs（供 CLI 模式 sleep_for 使用）
    uint32_t tickMs() const noexcept { return m_tickMs; }

    /// 诊断（切片 17）：暴露 RCU 寄存器库（测试/复现用；DevGuide §4B 骨架定义）
    RegisterBank* bank() noexcept { return m_bank.get(); }

private:
    void dataTickLoop() noexcept;  // DataTick 线程入口

    // 子对象（unique_ptr 持有）
    std::unique_ptr<RegisterBank>          m_bank;
    std::shared_ptr<SimPointTable>         m_pt;     // 共享给 PointGenerator
    std::unique_ptr<PointGenerator>        m_gen;
    std::unique_ptr<ModbusSlaveEmulator>   m_emu;
    std::unique_ptr<FaultInjector>         m_fi;
    std::unique_ptr<ScenarioScript>        m_scenario;   // B9 切片 15

    // 场景驱动状态（dataTickLoop 独占读写；查询经 m_cmdMtx）
    std::string m_scenarioPath;     // start 时缓存 cfg.scenarioPath
    bool        m_scenarioRunning = false;
    std::string m_exportDir;

    // DataTick 线程
    std::thread       m_tickThread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_tickStop{false};  // stop() 通知线程退出

    // 命令接口用的 mutex
    mutable std::mutex m_cmdMtx;

    // 诊断 + 配置缓存
    std::atomic<uint64_t> m_tickCount{0};
    uint32_t              m_tickMs = 100;
};

}  // namespace ens::sim
