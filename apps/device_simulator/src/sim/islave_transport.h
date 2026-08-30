// sim/islave_transport.h —— 抽象传输层（ENS-DEV-GUIDE §2B Track B 骨架）。
// TCP 监听（ModbusTcpServer）/ RTU 串口从站（RtuSlavePort, B4）共用同一接口。
// B3 最小版 RequestHandler 用 std::vector<uint8_t>（拷贝语义）；
// Phase 2 若需热路径零拷贝，可升级为 WireFrame 线缓冲（ENS-LLD-SIM §4.2），接口形态不变。
//
// B6 增量补丁：RequestHandlerWithUnit 增加 unitId 参数（MBAP Unit ID / RTU 首字节），
// 让 ModbusSlaveEmulator 把请求路由到对应的 SlaveRegset；原 RequestHandler 保留作为
// 单元 ID 已经无关的"伪从站"使用路径（B3 测试代码兼容）。
//
// B8 增量补丁：setFaultInjector 让 IO 层在 invokeHandler 完成后,编码响应前查
// FaultInjector::linkEffect(slave) 决定 corruptCrc / corruptByte / dropLink / delayMs。
// 基类默认空实现,ModbusTcpServer 与 RtuSlavePort 子类 override。非拥有指针,
// 由 ModbusSlaveEmulator.start() 注入。
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ens::sim {

class FaultInjector;  // 前向声明,避免头文件循环依赖

class ISlaveTransport {
public:
    using RequestHandler         = std::function<std::vector<uint8_t>(const std::vector<uint8_t>& reqPdu)>;
    using RequestHandlerWithUnit = std::function<std::vector<uint8_t>(uint8_t unitId,
                                                                       const std::vector<uint8_t>& reqPdu)>;

    virtual ~ISlaveTransport() = default;

    virtual bool open() noexcept = 0;             // bind+listen（TCP）/ 打开串口（RTU）
    virtual void close() noexcept = 0;            // 幂等
    virtual bool isOpen() const noexcept = 0;
    virtual void setRequestHandler(RequestHandler cb) = 0;
    // B6 新增：ModbusSlaveEmulator 通过 unitId 路由到不同 SlaveRegset
    virtual void setRequestHandler(RequestHandlerWithUnit cb) noexcept { (void)cb; }   // 默认空操作(子类可 override)

    // B8 新增：注入 FaultInjector（不拥有）。默认空实现：测试或旧 transport 不感知 fault 注入
    // 时,等同于无故障。子类的 override 在 invokeHandler 完成后查 linkEffect 决定 dropLink /
    // delayMs / corruptCrc / corruptByte。
    virtual void setFaultInjector(FaultInjector* /*fi*/) noexcept {}
};

}  // namespace ens::sim
