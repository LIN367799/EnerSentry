// tests/integration/test_main_loop.cpp
// 切片 14 集成测试：EnerSentryApp（被测主程序）+ SimulatorEngine（测试台）
// 进程内双轨回环 —— 主程序经 TCP 连测试台从站，验证：
//   * 连接建立
//   * 轮询驱动 → Modbus 请求/响应 → 工程值还原 → Sample 流转（L1 + DataBus + AlarmEngine）
//   * 5s 内收到 >= 10 个 Sample，且点表首点（Rack-01_MaxTemp, pointId=1）值在合理温度区间
//
// 依赖前置（tests/CMakeLists.txt）：
//   * test_data/sim_pointtable_sample.json 由 file(COPY) 部署到 build 目录
//   * EnerSentryApp.cpp 显式加入 ens_tests 源列表（测试台 CMake 铁律）
//   * ens_tests 已链接 ens::protocol/datahub/business/channel（PUBLIC 链透传）

#include "app/EnerSentryApp.h"

#include "sim/sim_config.h"
#include "sim/SimulatorEngine.h"
#include "sim/FaultInjector.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_approx.hpp>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {

constexpr quint16 kSimPort = 15020;   // 避开默认 5020，独立测试端口

// 点表 JSON 定位：ctest cwd = build/vs2022-debug/tests（需 ../test_data），
// IDE/直跑 cwd = 项目根或 build 目录（test_data 直下）。多路径尝试（与 test_pointtable 同策略）。
std::filesystem::path findPointTableJson() {
    const char* candidates[] = {
        "test_data/sim_pointtable_sample.json",
        "../test_data/sim_pointtable_sample.json",
    };
    for (const char* p : candidates) {
        if (std::filesystem::exists(p)) return std::filesystem::path(p);
    }
    throw std::runtime_error("cannot locate test_data/sim_pointtable_sample.json");
}

// 告警规则 sample（切片 16）：test_data/alarm_rules_sample.json
std::filesystem::path findAlarmRulesJson() {
    const char* candidates[] = {
        "test_data/alarm_rules_sample.json",
        "../test_data/alarm_rules_sample.json",
    };
    for (const char* p : candidates) {
        if (std::filesystem::exists(p)) return std::filesystem::path(p);
    }
    throw std::runtime_error("cannot locate test_data/alarm_rules_sample.json");
}

// drill 场景（切片 17）：test_data/scenarios/<name>.json
std::filesystem::path findScenarioJson(const char* name) {
    const std::filesystem::path candidates[] = {
        std::filesystem::path(L"test_data/scenarios") / name,
        std::filesystem::path(L"../test_data/scenarios") / name,
    };
    for (const auto& p : candidates) {
        if (std::filesystem::exists(p)) return p;
    }
    throw std::runtime_error(std::string("cannot locate test_data/scenarios/") + name);
}

/// 双轨夹具：起 sim（kSimPort）+ es（CLI 接线器），返回前 es 已 start
struct LoopbackRig {
    ens::sim::SimulatorEngine sim;
    ens::app::EnerSentryApp*  es = nullptr;

    LoopbackRig() = delete;
    explicit LoopbackRig(const ens::app::EnerSentryApp::Options& opts,
                         const std::string& simScenario = {},
                         const std::string& simExportDir = {}) {
        const auto ptPath = findPointTableJson();
        const std::string ptPathStr = ptPath.string();

        auto simPt = ens::sim::SimPointTable::loadFromJsonFile(ptPath);
        ens::sim::SimConfig simCfg;
        simCfg.tcp.enabled    = true;
        simCfg.tcp.port       = kSimPort;
        simCfg.rtu.enabled    = false;
        simCfg.pointtablePath = ptPathStr;
        simCfg.tickMs         = 100;
        simCfg.slaves         = ens::sim::SimConfig::fromPointTable(*simPt);
        simCfg.scenarioPath   = simScenario;   // 切片 17：drill 场景自动驱动
        simCfg.exportDir      = simExportDir;  // 切片 17：场景报告落盘（失败诊断）
        REQUIRE(sim.start(simCfg));

        es = new ens::app::EnerSentryApp(opts);
        REQUIRE(es->start());
    }
    ~LoopbackRig() {
        if (es) { es->stop(); delete es; es = nullptr; }
        sim.stop();
    }
    /// 主线程事件循环驱动（consumeTimer / flushTimer / SBO 定时器）
    void pump(int ms) {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < ms) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            QThread::msleep(10);
        }
    }
};

}  // namespace

TEST_CASE("main_loop: app + simulator loopback delivers samples", "[integration][main_loop]") {
    const auto ptPath = findPointTableJson();
    const std::string ptPathStr = ptPath.string();

    // ── 测试台侧：8 从站 / 43 点，DataTick 100ms ──
    auto simPt = ens::sim::SimPointTable::loadFromJsonFile(ptPath);
    ens::sim::SimConfig simCfg;
    simCfg.tcp.enabled    = true;
    simCfg.tcp.port       = kSimPort;
    simCfg.rtu.enabled    = false;
    simCfg.pointtablePath = ptPathStr;
    simCfg.tickMs         = 100;
    simCfg.slaves         = ens::sim::SimConfig::fromPointTable(*simPt);

    ens::sim::SimulatorEngine sim;
    REQUIRE(sim.start(simCfg));

    // ── 被测主程序侧：CLI 接线器 ──
    ens::app::EnerSentryApp::Options opts;
    opts.host           = QStringLiteral("127.0.0.1");
    opts.port           = kSimPort;
    opts.pointTablePath = QString::fromStdString(ptPathStr);
    opts.runSeconds     = 0;

    ens::app::EnerSentryApp es(opts);
    REQUIRE(es.start());

    // ── 等待 Sample 流转（5s 上限；主线程 processEvents 驱动消费节拍）──
    bool connected = false;
    QObject::connect(&es, &ens::app::EnerSentryApp::connected,
                     [&connected]() { connected = true; });

    // 语义校验：记录收到的工程值（须在等待循环前连接，避免错过窗口）
    double lastValid = 0.0;
    bool   gotValid  = false;
    QObject::connect(&es, &ens::app::EnerSentryApp::sampleReady,
                     [&](uint32_t, qint64, double v) {
        if (v > 0.001 && v < 10000.0) { lastValid = v; gotValid = true; }
    });

    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 5000 && es.sampleCount() < 10) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(20);
    }

    INFO("elapsedMs=" << t.elapsed() << " connected=" << connected
         << " sampleCount=" << es.sampleCount());
    REQUIRE(connected);
    REQUIRE(es.sampleCount() >= 10);

    // ── 语义校验：至少一个 Sample 的 value 是合理工程值（非 0/非 NaN）──
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    INFO("lastValid=" << lastValid);
    REQUIRE(gotValid);

    // ── 清理：先停被测主程序，再停测试台 ──
    es.stop();
    sim.stop();
}

// ═════════════════════════════════════════════════════════════════════════════
// 切片 16 T3：告警规则 + 黑匣子 + 月库（4.3.4 告警全链路 / 黑匣子 / 持久化接线）
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("main_loop: alarm rules + blackbox + monthly DB wired end-to-end",
          "[integration][main_loop][alarm][blackbox][db][slice16]") {
    // 临时数据目录（月库 + 黑匣子 mmap）
    const auto dbRoot = std::filesystem::temp_directory_path() / "ens_test_db_s16";
    std::error_code ec;
    std::filesystem::remove_all(dbRoot, ec);

    ens::app::EnerSentryApp::Options opts;
    opts.host           = QStringLiteral("127.0.0.1");
    opts.port           = kSimPort;
    opts.pointTablePath = QString::fromStdString(findPointTableJson().string());
    opts.alarmRulesPath = QString::fromStdString(findAlarmRulesJson().string());
    opts.dataDir        = QString::fromStdString(dbRoot.string());
    opts.blackboxDir    = QString::fromStdString(dbRoot.string());

    LoopbackRig rig(opts);

    // ── sim 侧注入 OverTemp：Rack-01_MaxTemp → 65℃（规则 Critical @60/55,onDelay 3s）──
    ens::sim::FaultRequest req;
    req.spec.type        = ens::sim::FaultType::OverTemp;
    req.spec.scope       = ens::sim::Scope::POINT;
    req.spec.slave       = 1;
    req.spec.reg         = 4096;   // Rack-01_MaxTemp
    req.spec.targetValue = 65.0f;
    REQUIRE(rig.sim.injectFault(req) != 0);

    // ── 等待告警触发（onDelay 3s + 轮询 100ms，上限 10s）──
    {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 10000 && rig.es->alarmCount() == 0) rig.pump(50);
        INFO("elapsedMs=" << t.elapsed() << " alarmCount=" << rig.es->alarmCount()
             << " blackbox=" << rig.es->blackboxTriggerCount());
        REQUIRE(rig.es->alarmCount() >= 1);
        // Critical → 黑匣子触发（mmap 已 enable）
        REQUIRE(rig.es->blackboxTriggerCount() >= 1);
    }

    // ── 等月库 flush（1s timer；pending 清空即已落盘）──
    {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 10000 && rig.es->historyPendingCount() > 0) rig.pump(50);
        INFO("pending=" << rig.es->historyPendingCount());
        // 月库目录应已生成 <root>/history
        REQUIRE(std::filesystem::exists(dbRoot / "history", ec));
        // 至少一个月库文件
        int monthDbCount = 0;
        if (std::filesystem::exists(dbRoot / "history", ec)) {
            for (const auto& d : std::filesystem::directory_iterator(dbRoot / "history", ec)) {
                if (d.is_directory(ec)) ++monthDbCount;
            }
        }
        REQUIRE(monthDbCount >= 1);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// 切片 16 T3：SBO select → armed → operate（主程序内部状态机 + FC06 下发）
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("main_loop: SBO select -> armed -> operate through app wiring",
          "[integration][main_loop][sbo][slice16]") {
    ens::app::EnerSentryApp::Options opts;
    opts.host           = QStringLiteral("127.0.0.1");
    opts.port           = kSimPort;
    opts.pointTablePath = QString::fromStdString(findPointTableJson().string());

    LoopbackRig rig(opts);
    rig.pump(300);   // 等连接 + 轮询就绪

    // ── Select：PCS-01(slave 17) 控制寄存器 0x1000 写 1 ──
    REQUIRE(rig.es->submitSboSelect(17, 0x1000, 1));
    rig.pump(100);
    REQUIRE(rig.es->sboState() == ens::business::SBOState::Armed);

    // ── Operate：二次确认 → Executed（armed 5s 窗口内必须完成）──
    REQUIRE(rig.es->submitSboOperate());
    rig.pump(100);
    REQUIRE(rig.es->sboState() == ens::business::SBOState::Executed);
}

// ═════════════════════════════════════════════════════════════════════════════
// 切片 17 T3：overheat_fast drill 全流程（告警 → 黑匣子 → 月库 → 恢复）
// 4.3.4 联调验收：alarmWord 链路 + [L4][ALM] + 黑匣子落盘 + 月库生成 + 恢复复位
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("main_loop: overheat_fast drill drives alarm+blackbox+monthly DB then recovers",
          "[integration][main_loop][drill][overheat][slice17]") {
    const auto dbRoot = std::filesystem::temp_directory_path() / "ens_test_db_s17";
    std::error_code ec;
    std::filesystem::remove_all(dbRoot, ec);
    std::filesystem::create_directories(dbRoot, ec);

    ens::app::EnerSentryApp::Options opts;
    opts.host           = QStringLiteral("127.0.0.1");
    opts.port           = kSimPort;
    opts.pointTablePath = QString::fromStdString(findPointTableJson().string());
    opts.alarmRulesPath = QString::fromStdString(findAlarmRulesJson().string());
    opts.dataDir        = QString::fromStdString(dbRoot.string());
    opts.blackboxDir    = QString::fromStdString(dbRoot.string());

    // sim 加载 overheat_fast：t=0 立即 65℃（Critical 越限 60），t=5s 回归 35℃
    LoopbackRig rig(opts, findScenarioJson("overheat_fast.json").string(), dbRoot.string());
    // 告警事件时序收集（失败诊断：哪条点号、何时触发）
    std::vector<QString> alarms;
    QObject::connect(rig.es, &ens::app::EnerSentryApp::alarmRaised,
                     [&alarms](const QString& t) { alarms.push_back(t); });
    // MaxTemp 值序列收集（失败诊断：主程序读到 65 后是否回落到 55 以下）
    std::vector<std::string> temps;
    QObject::connect(rig.es, &ens::app::EnerSentryApp::sampleReady,
                     [&temps](uint32_t pid, qint64, double v) {
        if (pid == 1 || pid == 26) {
            char b[64];
            std::snprintf(b, sizeof(b), "pt=%u v=%.2f", pid, v);
            temps.emplace_back(b);
        }
    });

    // ── 告警触发（onDelay 3s；总上限 10s）──
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 10000 && rig.es->alarmCount() == 0) rig.pump(50);
    INFO("elapsedMs=" << t.elapsed() << " alarmCount=" << rig.es->alarmCount()
         << " blackbox=" << rig.es->blackboxTriggerCount());
    REQUIRE(rig.es->alarmCount() >= 1);
    REQUIRE(rig.es->blackboxTriggerCount() >= 1);   // Critical → 黑匣子

    // ── 月库落盘（1s flush 后 history 目录出现）──
    while (t.elapsed() < 10000 && rig.es->historyPendingCount() > 0) rig.pump(50);
    REQUIRE(std::filesystem::exists(dbRoot / "history", ec));

    // ── 恢复：RECOVER t=5s + 快速回归 + offDelay 3s + 段轮转稀疏样本（MaxTemp 每 ~2-4s
    //    一次，恢复判定依赖样本间隔）→ 上限放宽 20s（切片 17 实测 12s 不够）──
    while (t.elapsed() < 20000 && rig.es->alarmCount() > 0) rig.pump(50);
    INFO("finalElapsedMs=" << t.elapsed() << " alarmCount=" << rig.es->alarmCount());
    for (const auto& a : alarms) UNSCOPED_INFO("alarm_event: " << qPrintable(a));
    for (const auto& s : temps) UNSCOPED_INFO("temp: " << s);
    REQUIRE(rig.es->alarmCount() == 0);
}

// ═════════════════════════════════════════════════════════════════════════════
// 切片 17 T3：SBO armed 断链清锁 + 自动重连（random_linkloss 的确定性单点版）
// 4.3.4 联调验收：FR-CTRL-07 断线自动清锁；COMM-09 重连
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("main_loop: SBO armed clears on link loss and reconnects after",
          "[integration][main_loop][sbo][linkloss][slice17]") {
    ens::app::EnerSentryApp::Options opts;
    opts.host           = QStringLiteral("127.0.0.1");
    opts.port           = kSimPort;
    opts.pointTablePath = QString::fromStdString(findPointTableJson().string());

    LoopbackRig rig(opts);
    rig.pump(800);   // 等连接（异步 connectToHost）
    REQUIRE(rig.es->isConnected());

    // ── Select → Armed ──
    REQUIRE(rig.es->submitSboSelect(17, 0x1000, 1));
    rig.pump(200);
    REQUIRE(rig.es->sboState() == ens::business::SBOState::Armed);

    // ── CommLoss 注入 slave 17（SLAVE scope，2.5s 后自动恢复）→ 断链清锁 ──
    ens::sim::FaultRequest req;
    req.spec.type       = ens::sim::FaultType::CommLoss;
    req.spec.scope      = ens::sim::Scope::SLAVE;
    req.spec.slave      = 17;
    req.spec.durationMs = 2500;
    REQUIRE(rig.sim.injectFault(req) != 0);

    // 断链 → 抖动窗口 500ms → 清锁 Idle
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 6000 && rig.es->sboState() != ens::business::SBOState::Idle) {
        rig.pump(50);
    }
    INFO("linkloss elapsedMs=" << t.elapsed() << " sboState="
         << static_cast<int>(rig.es->sboState())
         << " connected=" << rig.es->isConnected());
    REQUIRE(rig.es->sboState() == ens::business::SBOState::Idle);
    REQUIRE_FALSE(rig.es->isConnected());

    // ── CommLoss 到期 → TcpChannel 退避重连（1s 起）──
    while (t.elapsed() < 10000 && !rig.es->isConnected()) rig.pump(50);
    INFO("reconnect elapsedMs=" << t.elapsed() << " connected=" << rig.es->isConnected());
    REQUIRE(rig.es->isConnected());
}
