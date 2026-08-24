// ChannelConfig.h —— Phase 1 L1：通道配置 tagged union（ENS-LLD-100 §3.5）。
// SerialConfig / TcpConfig / CanConfig 三套子配置 + type 标签；
// ChannelFactory::create() 按 cfg.type 分派，子类 open(cfg) 用 std::get<...>(cfg.payload) 取自身配置。
// 关键点：
//   - CAN/SPI 是预留扩展位（§3.4 / §3.5 SPI 分支返回 nullptr，ABI 稳定）
//   - 通用重连/超时参数挂在主配置上，避免每个子类重复
#pragma once

#include <QString>
#include <cstdint>
#include <variant>

namespace ens::channel {

enum class ChannelType : uint8_t {
    Serial = 0,    // RTU 物理串口（§3.2）
    TCP    = 1,    // Modbus TCP（§3.3）
    CAN    = 2,    // SocketCAN / ZLG SDK（§3.4，2.1.1 暂不实现）
    SPI    = 3,    // 预留扩展位（未实现；ChannelFactory SPI 分支返回 nullptr）
};

// 串口物理层配置（ENS-LLD-100 §3.2.1 + §4.1.1）
struct SerialConfig {
    QString portName;             // "COM3" / "/dev/ttyUSB0"
    qint32  baudRate  = 9600;
    int     dataBits  = 8;
    int     stopBits  = 1;
    QString parity    = "N";      // "N" 无 / "E" 偶 / "O" 奇
    bool    rs485Enabled = true;  // 半双工 DE/RE 方向控制开关（§3.2.2）
};

// TCP 链路配置（ENS-LLD-100 §3.3.1）
struct TcpConfig {
    QString host = "127.0.0.1";
    quint16 port = 502;
    bool    keepAliveEnabled = true;
};

// CAN 通道预留配置（§3.4，本阶段 CanChannel 未落地）
struct CanConfig {
    QString interfaceName;        // "can0" / "ZLG-CAN1"
    quint32 bitrate = 500000;
};

// 通道主配置：tagged union + 通用重连 / 超时参数
struct ChannelConfig {
    ChannelType type = ChannelType::Serial;
    std::variant<SerialConfig, TcpConfig, CanConfig> payload{};
    QString name;                          // 链路别名（"BMS-LINK-01"），日志/统计用
    int reconnectBaseMs = 1000;            // 指数退避初值（COMM-09）
    int reconnectMaxMs  = 30000;           // 指数退避封顶
    int ioTimeoutMs     = 3000;            // 单请求超时（PollScheduler 用）
};

}  // namespace ens::channel