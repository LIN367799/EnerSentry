// tests/main.cpp —— 自建 Catch2 v3 main()，跳过默认 Catch2WithMain。
//
// 原因：ens_tests 引入 ModbusEngine（含 std::vector 字段的 ModbusResponse），
// 跨线程 Qt 信号（Qt::QueuedConnection）必须先 qRegisterMetaType<ModbusResponse>()，
// 否则信号跨线程时静默丢弃（参见用户截图 V2 坑:ModbusEngine 跨线程 emit Sample/ModbusResponse
// 前必须 qRegisterMetaType,否则信号静默）。
//
// ⚠ QCoreApplication 单例约束：
//   tests/common/test_helpers.h 已经实现 appInstance() 用 C++11 magic static 创建
//   全进程唯一 QCoreApplication 实例(Meyers singleton,线程安全);我们这里**直接复用**,
//   不再 new QCoreApplication,否则触发 Qt 断言 "there should be only one application object"
//   (kernel/qcoreapplication.cpp:772)。
//
// 切换方式：
//   * tests/CMakeLists.txt 把 Catch2::Catch2WithMain 改为 Catch2::Catch2（不带 main）
//   * 本文件 #define CATCH_CONFIG_RUNNER 关闭 Catch2 自动注入 main
//   * 自建 main 调 appInstance() + qRegisterMetaType + Catch::Session().run(argc, argv)

#define CATCH_CONFIG_RUNNER
#include <catch2/catch_session.hpp>

#include <QMetaType>

#include "protocol/ModbusEngine.h"   // 含 ModbusResponse,触发 Q_DECLARE_METATYPE
#include "common/test_helpers.h"      // appInstance() Meyers singleton

int main(int argc, char* argv[]) {
    // 拿 QCoreApplication 引用（不构造 — 让 appInstance() 持有所有权,Meyers singleton 唯一实例）
    // 必须先于 qRegisterMetaType / 测试体调用,否则 Qt 元对象系统没 instance() 会失败。
    QCoreApplication& app = *ens::test::appInstance();
    (void)app;

    // ModbusEngine 跨线程 emit 必备:ModbusResponse 含 std::vector,需注册到 Qt 元类型系统
    qRegisterMetaType<ens::protocol::ModbusResponse>("ens::protocol::ModbusResponse");

    // 透传 argv 给 Catch2 session
    Catch::Session session;
    const int result = session.run(argc, argv);
    return result;
}