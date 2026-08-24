// sim/islave_transport.h —— 抽象传输层（ENS-DEV-GUIDE §2B Track B 骨架）。
// TCP 监听（ModbusTcpServer）/ RTU 串口从站（RtuSlavePort, B4）共用同一接口。
// B3 最小版 RequestHandler 用 std::vector<uint8_t>（拷贝语义）；
// Phase 2 若需热路径零拷贝，可升级为 WireFrame 线缓冲（ENS-LLD-SIM §4.2），接口形态不变。
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ens::sim {

class ISlaveTransport {
public:
    using RequestHandler = std::function<std::vector<uint8_t>(const std::vector<uint8_t>& reqPdu)>;

    virtual ~ISlaveTransport() = default;

    virtual bool open() noexcept = 0;             // bind+listen（TCP）/ 打开串口（RTU）
    virtual void close() noexcept = 0;            // 幂等
    virtual bool isOpen() const noexcept = 0;
    virtual void setRequestHandler(RequestHandler cb) = 0;
};

}  // namespace ens::sim