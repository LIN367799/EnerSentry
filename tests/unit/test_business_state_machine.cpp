// tests/unit/test_business_state_machine.cpp
// L4 业务层 ── BusinessStateMachine Tier 2 单测（ENS-LLD-400 §1.2 + DevGuide §4.3.1）。

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>

#include "business/BusinessStateMachine.h"

using ens::business::BusinessState;
using ens::business::BusinessStateMachine;
using ens::business::DeviceId;
using ens::business::PointId;
using ens::business::StationId;

namespace {
void processEvents(int ms = 50) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, ms);
}
}  // namespace

TEST_CASE("business_state_machine: registerDevice creates station/device/points in Config",
          "[master][business][bsm][register]") {
    BusinessStateMachine bsm;
    bsm.registerDevice(/*stationId=*/1, /*deviceId=*/10, {100, 101, 102});

    REQUIRE(bsm.stationCount() == 1);
    REQUIRE(bsm.deviceCount()  == 1);
    REQUIRE(bsm.pointCount()   == 3);
    REQUIRE(bsm.stationState(1) == BusinessState::Config);
    REQUIRE(bsm.deviceState(10) == BusinessState::Config);
    REQUIRE(bsm.pointState(100) == BusinessState::Config);
    REQUIRE(bsm.pointState(101) == BusinessState::Config);
    REQUIRE(bsm.pointState(102) == BusinessState::Config);
}

TEST_CASE("business_state_machine: Device toRunning lifts Station to Running",
          "[master][business][bsm][transition]") {
    BusinessStateMachine bsm;
    bsm.registerDevice(1, 10, {100});
    bsm.registerDevice(1, 11, {101});

    int stateChangedN = 0;
    QObject::connect(&bsm, &BusinessStateMachine::stateChanged,
                     [&stateChangedN](StationId, DeviceId, PointId,
                                      uint8_t, uint8_t) { ++stateChangedN; });

    REQUIRE(bsm.toRunning(10));
    processEvents();

    REQUIRE(bsm.deviceState(10) == BusinessState::Running);
    REQUIRE(bsm.deviceState(11) == BusinessState::Config);
    REQUIRE(bsm.stationState(1) == BusinessState::Running);
    REQUIRE(stateChangedN >= 1);
}

TEST_CASE("business_state_machine: Station toStats allowed when no Device Running",
          "[master][business][bsm][stats]") {
    BusinessStateMachine bsm;
    bsm.registerDevice(1, 10, {100});
    bsm.registerDevice(1, 11, {101});
    bsm.toRunning(10);

    // 10 Running → 拒 Stats
    REQUIRE_FALSE(bsm.toStats(1));
    REQUIRE(bsm.stationState(1) == BusinessState::Running);

    // 10 退回 Config + 11 维持 Config → 允许 Stats
    bsm.toConfigDevice(10);
    REQUIRE(bsm.toStats(1));
    REQUIRE(bsm.stationState(1) == BusinessState::Stats);
}

TEST_CASE("business_state_machine: unregisterDevice blocked while Running",
          "[master][business][bsm][unregister]") {
    BusinessStateMachine bsm;
    bsm.registerDevice(1, 10, {100});
    bsm.toRunning(10);
    REQUIRE(bsm.deviceState(10) == BusinessState::Running);

    bsm.unregisterDevice(10);
    REQUIRE(bsm.deviceCount() == 1);                       // Running 拒删

    bsm.toConfigDevice(10);
    bsm.unregisterDevice(10);
    REQUIRE(bsm.deviceCount() == 0);
    REQUIRE(bsm.pointCount()  == 0);
}