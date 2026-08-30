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
#include "sim/sim_config.h"

#include <chrono>
#include <iostream>
#include <utility>

namespace ens::sim {

SimulatorEngine::SimulatorEngine() = default;

SimulatorEngine::~SimulatorEngine() {
    stop();
}

bool SimulatorEngine::start(const SimConfig& cfgIn) noexcept {
    if (m_running.load()) return true;

    // 1) 缓存 tickMs（DataTick 线程用）
    m_tickMs = cfgIn.tickMs;

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

    std::cout << "[SimulatorEngine] started (tickMs=" << m_tickMs
              << ", slaves=" << cfg.slaves.size()
              << ", tcpPort=" << m_emu->tcpPort() << ")\n";
    return true;
}

void SimulatorEngine::stop() noexcept {
    // 幂等:已停止时直接返回
    if (!m_running.load() && !m_tickThread.joinable()) {
        m_emu.reset();
        m_gen.reset();
        m_bank.reset();
        m_fi.reset();
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

    // 3) emu.stop（关 Slave IO）
    if (m_emu) m_emu->stop();

    // 4) 逆序析构
    m_emu.reset();
    m_gen.reset();
    m_bank.reset();
    m_fi.reset();
    m_pt.reset();

    std::cout << "[SimulatorEngine] stopped (tickCount=" << m_tickCount.load() << ")\n";
}

void SimulatorEngine::dataTickLoop() noexcept {
    using namespace std::chrono;
    const auto interval = milliseconds(m_tickMs);
    while (!m_tickStop.load(std::memory_order_acquire)) {
        // 顺序:fi.tickSessions 推进 FSM(RECOVERING 斜率回归 / durationMs 到期)
        //       → gen.generateTick 物理演化 + 故障覆盖 + publish
        if (m_gen && m_fi) {
            m_fi->tickSessions(m_tickMs);
            m_gen->generateTick();
        }
        m_tickCount.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(interval);
    }
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
