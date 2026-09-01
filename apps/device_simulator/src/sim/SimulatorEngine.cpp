// src/sim/SimulatorEngine.cpp
// 测试台 ── 编排者实现（ENS-LLD-SIM §2.2.1 + DevGuide §4B 切片 13）。
//
// 线程模型（评审记录 2026-08-30 22:25）：
//   * DataTick 线程（std::thread）独占调 gen.generateTick() + fi.tickSessions(dtMs)
//   * injectFault / recoverFault / abortFault 经 m_cmdMtx 调 fi 的方法（fi 内部 shared_mutex
//     保护 sessions,无锁冲突）
//   * GUI 线程（B10）通过 Qt::QueuedConnection 投到 SimulatorEngine 槽,本切片不做 Qt

#include "sim/SimulatorEngine.h"

#include "core/point_table.h"
#include "sim/FaultInjector.h"
#include "sim/modbus_slave_emulator.h"
#include "sim/point_generator.h"
#include "sim/register_bank.h"
#include "sim/scenario_script.h"
#include "sim/sim_config.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <utility>

namespace ens::sim {

SimulatorEngine::SimulatorEngine() = default;

SimulatorEngine::~SimulatorEngine() {
    stop();
}

bool SimulatorEngine::start(const SimConfig& cfgIn) noexcept {
    if (m_running.load()) return true;

    // noexcept 防御：body 内 SimPointTable::loadFromJsonFile 等会抛 runtime_error
    // （点表路径不存在时），不 try-catch 会穿透 noexcept → std::terminate →
    // debug CRT abort 弹窗挂起进程（切片 15 CLI 实测根因）。
    try {

    // 1) 缓存 tickMs（DataTick 线程用）+ 场景/导出路径
    m_tickMs = cfgIn.tickMs;
    m_scenarioPath = cfgIn.scenarioPath;
    m_exportDir    = cfgIn.exportDir;

    // 2) 构造 FaultInjector（无依赖,先于其他子对象）
    m_fi = std::make_unique<FaultInjector>();

    // 3) 加载点表（缺省路径由 SimConfig.pointtablePath 提供）
    m_pt = SimPointTable::loadFromJsonFile(cfgIn.pointtablePath);
    if (m_pt == nullptr) {
        std::cerr << "[SimulatorEngine] failed to load pointtable: "
                  << cfgIn.pointtablePath << "\n";
        m_fi.reset();
        return false;
    }

    // 4) 派生 slaves 列表（cfg.slaves 为空时 fromPointTable 推导,所见即所建 bank）
    SimConfig cfg = cfgIn;  // 拷贝以补 slaves
    if (cfg.slaves.empty()) {
        cfg.slaves = SimConfig::fromPointTable(*m_pt);
    }

    // 5) 构造 RegisterBank + 装 baseline
    m_bank = std::make_unique<RegisterBank>();
    for (const auto& spec : cfg.slaves) {
        const uint16_t regCount = (spec.regCount > 0) ? spec.regCount : 256;
        auto initial = SlaveRegset::allocate(spec.slaveId, regCount, regCount);
        m_bank->install(spec.slaveId,
                        std::shared_ptr<const SlaveRegset>(new SlaveRegset(std::move(initial))));
    }

    // 6) 构造 PointGenerator（持 m_pt 共享 + cfg 物理参数）
    m_gen = std::make_unique<PointGenerator>(cfg, m_pt);
    m_gen->attach(m_bank.get());
    m_gen->attachFi(m_fi.get());

    // 7) 构造 ModbusSlaveEmulator + 注入 fi + start（启 Slave IO）
    m_emu = std::make_unique<ModbusSlaveEmulator>();
    m_emu->setFaultInjector(m_fi.get());
    if (!m_emu->start(cfg, m_bank.get())) {
        std::cerr << "[SimulatorEngine] emulator.start failed\n";
        m_emu.reset();
        m_gen.reset();
        m_bank.reset();
        m_pt.reset();
        m_fi.reset();
        return false;
    }

    // 8) 启动 DataTick 线程
    m_tickStop.store(false);
    m_running.store(true);
    m_tickCount.store(0);
    m_tickThread = std::thread(&SimulatorEngine::dataTickLoop, this);

    // 9) 自动加载场景（cfg.scenarioPath 非空；加载失败仅告警不阻断运行）
    if (!m_scenarioPath.empty() && !loadScenario(m_scenarioPath)) {
        std::cerr << "[SimulatorEngine] scenario load failed: " << m_scenarioPath << "\n";
    }

    std::cout << "[SimulatorEngine] started (tickMs=" << m_tickMs
              << ", slaves=" << cfg.slaves.size()
              << ", tcpPort=" << m_emu->tcpPort()
              << (m_scenario ? ", scenario=" + m_scenario->name() : "")
              << ")\n";
    return true;

    } catch (const std::exception& e) {
        std::cerr << "[SimulatorEngine] start exception: " << e.what() << "\n";
        m_emu.reset();
        m_gen.reset();
        m_bank.reset();
        m_fi.reset();
        m_scenario.reset();
        m_pt.reset();
        return false;
    } catch (...) {
        std::cerr << "[SimulatorEngine] start unknown exception\n";
        m_emu.reset();
        m_gen.reset();
        m_bank.reset();
        m_fi.reset();
        m_scenario.reset();
        m_pt.reset();
        return false;
    }
}

void SimulatorEngine::stop() noexcept {
    // 幂等:已停止时直接返回
    if (!m_running.load() && !m_tickThread.joinable()) {
        m_emu.reset();
        m_gen.reset();
        m_bank.reset();
        m_fi.reset();
        m_scenario.reset();
        m_pt.reset();
        return;
    }

    // 1) 通知 DataTick 线程退出
    m_tickStop.store(true);
    m_running.store(false);

    // 2) join
    if (m_tickThread.joinable()) {
        m_tickThread.join();
    }

    // 2.5) 场景收尾：未结束 → INCONCLUSIVE；报告/事件流落盘（exportDir 非空时）
    if (m_scenario) {
        m_scenario->finishReport(static_cast<int64_t>(m_tickCount.load()) * m_tickMs,
                                 !m_scenario->allFired());
        if (!m_exportDir.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(m_exportDir, ec);
            const auto write = [&](const std::string& fn, const std::string& content) {
                // _wfopen 绕开 std::ofstream 的 ANSI 中文路径坑（与 load 侧一致）
                const std::filesystem::path out = std::filesystem::path(m_exportDir) / fn;
#ifdef _WIN32
                FILE* f = _wfopen(out.c_str(), L"wb");
#else
                FILE* f = std::fopen(out.c_str(), "wb");
#endif
                if (f != nullptr) {
                    if (!content.empty()) std::fwrite(content.data(), 1, content.size(), f);
                    std::fclose(f);
                }
            };
            write("sim_events.jsonl", m_scenario->eventsJsonl());
            write("sim_report.json",  m_scenario->reportJson());
            std::cout << "[SimulatorEngine] scenario report dumped to " << m_exportDir << "\n";
        }
        std::cout << "[SimulatorEngine] scenario result:\n" << m_scenario->reportJson() << "\n";
    }

    // 3) emu.stop（关 Slave IO）
    if (m_emu) m_emu->stop();

    // 4) 逆序析构
    m_emu.reset();
    m_gen.reset();
    m_bank.reset();
    m_fi.reset();
    m_scenario.reset();
    m_pt.reset();

    std::cout << "[SimulatorEngine] stopped (tickCount=" << m_tickCount.load() << ")\n";
}

void SimulatorEngine::dataTickLoop() noexcept {
    using namespace std::chrono;
    const auto interval = milliseconds(m_tickMs);
    while (!m_tickStop.load(std::memory_order_acquire)) {
        // 顺序:场景驱动(dueSteps 触发 INJECT/RECOVER)
        //       → fi.tickSessions 推进 FSM(RECOVERING 斜率回归 / durationMs 到期)
        //       → gen.generateTick 物理演化 + 故障覆盖 + publish
        if (m_gen && m_fi) {
            if (m_scenario && m_scenarioRunning) {
                const int64_t nowMs = static_cast<int64_t>(m_tickCount.load()) * m_tickMs;
                m_scenario->drive(nowMs, *m_fi, *m_pt);
                // 全部 step 触发后停驱（保留已注入故障到 stop）
                if (m_scenario->allFired()) {
                    m_scenarioRunning = false;
                }
            }
            m_fi->tickSessions(m_tickMs);
            m_gen->generateTick();
        }
        m_tickCount.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(interval);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 场景脚本（B9 切片 15）
// ─────────────────────────────────────────────────────────────────────────────

bool SimulatorEngine::loadScenario(const std::string& path) noexcept {
    std::lock_guard<std::mutex> lock(m_cmdMtx);
    if (path.empty()) return false;
    auto sc = std::make_unique<ScenarioScript>();
    if (!sc->load(path)) {
        std::cerr << "[SimulatorEngine] loadScenario failed: " << path
                  << " notes=" << sc->reportJson() << "\n";
        return false;
    }
    m_scenario = std::move(sc);
    m_scenarioRunning = true;
    std::cout << "[SimulatorEngine] scenario loaded: " << m_scenario->name()
              << " steps=" << m_scenario->stepCount() << "\n";
    return true;
}

bool SimulatorEngine::scenarioLoaded() const noexcept {
    std::lock_guard<std::mutex> lock(m_cmdMtx);
    return m_scenario != nullptr;
}

bool SimulatorEngine::scenarioRunning() const noexcept {
    std::lock_guard<std::mutex> lock(m_cmdMtx);
    return m_scenarioRunning;
}

bool SimulatorEngine::scenarioAllFired() const noexcept {
    std::lock_guard<std::mutex> lock(m_cmdMtx);
    return m_scenario != nullptr && m_scenario->allFired();
}

std::string SimulatorEngine::scenarioReportJson() const {
    std::lock_guard<std::mutex> lock(m_cmdMtx);
    return m_scenario ? m_scenario->reportJson() : std::string();
}

std::string SimulatorEngine::scenarioEventsJsonl() const {
    std::lock_guard<std::mutex> lock(m_cmdMtx);
    return m_scenario ? m_scenario->eventsJsonl() : std::string();
}

std::string SimulatorEngine::scenarioName() const {
    std::lock_guard<std::mutex> lock(m_cmdMtx);
    return m_scenario ? m_scenario->name() : std::string();
}

uint32_t SimulatorEngine::injectFault(const FaultRequest& req) noexcept {
    std::lock_guard<std::mutex> lock(m_cmdMtx);
    if (!m_fi) return 0;
    return m_fi->trigger(req);
}

bool SimulatorEngine::recoverFault(uint32_t handle) noexcept {
    std::lock_guard<std::mutex> lock(m_cmdMtx);
    if (!m_fi) return false;
    return m_fi->recover(handle);
}

bool SimulatorEngine::abortFault(uint32_t handle) noexcept {
    std::lock_guard<std::mutex> lock(m_cmdMtx);
    if (!m_fi) return false;
    return m_fi->abort(handle);
}

uint64_t SimulatorEngine::tickCount() const noexcept {
    return m_tickCount.load(std::memory_order_acquire);
}

}  // namespace ens::sim
