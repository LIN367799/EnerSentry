// tests/unit/test_sbo_state_machine.cpp
// L4 业务层 ── SboStateMachine + DeviceSboGuard Tier 2 单测。

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>

#include <chrono>
#include <thread>

#include "business/DeviceSboGuard.h"
#include "business/SboStateMachine.h"

using ens::business::ArmedOccupant;
using ens::business::DeviceSboGuard;
using ens::business::SBOState;
using ens::business::SboCommand;
using ens::business::SboDeviceKey;
using ens::business::SboSelectRequest;
using ens::business::SboStateMachine;

namespace {
void processEvents(int ms = 50) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, ms);
}
void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

SboSelectRequest mkReq(uint32_t link, uint32_t slave, uint32_t reg,
                       bool emergency = false, uint16_t value = 100) {
    SboSelectRequest r;
    r.linkId       = link;
    r.slaveId      = slave;
    r.registerAddr = reg;
    r.value        = value;
    r.emergency    = emergency;
    return r;
}
}  // namespace

// DeviceSboGuard 二维分桶互斥
TEST_CASE("device_sbo_guard: same key rejected, different key allowed concurrently",
          "[master][business][sbo][guard][tier2]") {
    DeviceSboGuard guard;
    SboDeviceKey k1{1, 1, 0x100};
    SboDeviceKey k2{1, 1, 0x101};
    SboDeviceKey k3{1, 2, 0x100};

    ArmedOccupant o;
    REQUIRE(guard.tryAcquire(k1, "seq1", "op1", &o));
    REQUIRE(o.sequenceId == "seq1");

    REQUIRE_FALSE(guard.tryAcquire(k1, "seq2", "op2"));
    REQUIRE(guard.activeCount() == 1);

    REQUIRE(guard.tryAcquire(k2, "seq3", "op3"));
    REQUIRE(guard.tryAcquire(k3, "seq4", "op4"));
    REQUIRE(guard.activeCount() == 3);

    auto q = guard.query(k1);
    REQUIRE(q.has_value());
    REQUIRE(q->operatorName == "op1");

    guard.release(k1, "wrong-seq");
    REQUIRE(guard.activeCount() == 3);
    guard.release(k1, "seq1");
    REQUIRE(guard.activeCount() == 2);
}

// 全状态机
TEST_CASE("sbo_state_machine: submitSelect -> Armed, submitOperate -> Executed -> Idle",
          "[master][business][sbo][fsm][tier2]") {
    DeviceSboGuard guard;
    SboStateMachine sm;
    sm.setGuard(&guard);

    int armedAcqN = 0, execSuccN = 0, cmdReadyN = 0;
    QObject::connect(&sm, &SboStateMachine::armedAcquired,
                     [&armedAcqN](const QString&, const SboDeviceKey&) { ++armedAcqN; });
    QObject::connect(&sm, &SboStateMachine::executingSucceeded,
                     [&execSuccN](const QString&, const QString&) { ++execSuccN; });
    QObject::connect(&sm, &SboStateMachine::commandReady,
                     [&cmdReadyN](const SboCommand&) { ++cmdReadyN; });

    REQUIRE(sm.currentState() == SBOState::Idle);
    REQUIRE(sm.submitSelect(mkReq(1, 1, 0x100), "alice"));
    processEvents();
    REQUIRE(sm.currentState() == SBOState::Armed);
    REQUIRE(armedAcqN == 1);

    const QString seqId = sm.currentSequenceId();
    REQUIRE(sm.submitOperate(seqId));
    processEvents();
    REQUIRE(sm.currentState() == SBOState::Executed);
    REQUIRE(cmdReadyN == 1);

    sm.onDeviceAck(seqId, /*success=*/true);
    processEvents();
    REQUIRE(sm.currentState() == SBOState::Idle);
    REQUIRE(execSuccN == 1);
    REQUIRE(guard.activeCount() == 0);
}

// SBO 断链自动清锁（截图要求）
TEST_CASE("sbo_state_machine: LinkDown during Armed auto-clears lock to Idle",
          "[master][business][sbo][link-down][tier3]") {
    DeviceSboGuard guard;
    SboStateMachine sm;
    sm.setGuard(&guard);

    int clearedN = 0;
    QObject::connect(&sm, &SboStateMachine::armedCleared,
                     [&clearedN](const QString&) { ++clearedN; });

    REQUIRE(sm.submitSelect(mkReq(1, 1, 0x100), "alice"));
    processEvents();
    REQUIRE(sm.currentState() == SBOState::Armed);
    REQUIRE(guard.activeCount() == 1);

    sm.forceLinkDownForTest(/*flappingMs=*/0);
    processEvents();
    REQUIRE(clearedN == 0);                                // 抖动窗口内未清

    sleepMs(600);
    processEvents(200);
    REQUIRE(sm.currentState() == SBOState::Idle);
    REQUIRE(guard.activeCount() == 0);
    REQUIRE(clearedN == 1);
}

// LinkDown + recovery within flapping window 不误清（LLD §3.4 盲点 ③）
TEST_CASE("sbo_state_machine: LinkDown + recovery within flapping window does NOT abort",
          "[master][business][sbo][flapping][tier3]") {
    DeviceSboGuard guard;
    SboStateMachine sm;
    sm.setGuard(&guard);

    REQUIRE(sm.submitSelect(mkReq(1, 1, 0x200), "alice"));
    processEvents();
    REQUIRE(sm.currentState() == SBOState::Armed);

    sm.forceLinkDownForTest(/*flappingMs=*/200);
    processEvents();
    sm.onLinkStatusChanged(/*connected=*/true);
    sleepMs(250);
    processEvents(200);
    REQUIRE(sm.currentState() == SBOState::Armed);          // 未误清
    REQUIRE(guard.activeCount() == 1);
}

// 超时清锁（5s ADR-16）
TEST_CASE("sbo_state_machine: Armed timeout via force hook releases lock",
          "[master][business][sbo][timeout][tier2]") {
    DeviceSboGuard guard;
    SboStateMachine sm;
    sm.setGuard(&guard);

    int clearedN = 0;
    QObject::connect(&sm, &SboStateMachine::armedCleared,
                     [&clearedN](const QString&) { ++clearedN; });

    REQUIRE(sm.submitSelect(mkReq(1, 1, 0x300), "bob"));
    processEvents();
    REQUIRE(sm.currentState() == SBOState::Armed);

    sm.forceArmedTimeoutForTest();
    processEvents();
    REQUIRE(sm.currentState() == SBOState::Idle);
    REQUIRE(guard.activeCount() == 0);
    REQUIRE(clearedN == 1);
}

// 同设备同寄存器二次请求被拒
TEST_CASE("sbo_state_machine: second Select on same device/register rejected",
          "[master][business][sbo][busy][tier2]") {
    DeviceSboGuard guard;
    SboStateMachine sm;
    sm.setGuard(&guard);

    int rejN = 0;
    QObject::connect(&guard, &DeviceSboGuard::armedRejected,
                     [&rejN](const QString&, const SboDeviceKey&,
                             const QString&, int64_t) { ++rejN; });

    REQUIRE(sm.submitSelect(mkReq(1, 1, 0x400), "alice"));
    processEvents();

    REQUIRE_FALSE(sm.submitSelect(mkReq(1, 1, 0x400), "charlie"));
    REQUIRE(rejN == 1);
    REQUIRE(sm.currentState() == SBOState::Armed);
}