namespace ens::protocol {

// L2 协议层 BUILD 锚（ENS-DEV-ARCH §3.2）。
// 当前承载契约：
//   - ModbusStreamAccumulator（§4.2.1 字节流拼帧  +  §4.2.1.1 异常帧 5B/9B）
//   - TransactionIdAllocator（§4.3.6 / DevGuide §2A TCP 16-bit 事务 ID 位图）
//   - Crc16                 （§4.1.2 CRC-16/MODBUS 查表，与 sim 侧 byte-by-byte 等价）
//   - ModbusFrame           （§4.1 组帧 / §4.1.1 RTU/TCP / §4.1.3 FC01~06+0F+10+异常 5B9B）
//   - PointTable            （§4.4 点表解析器 / DevGuide §3A 3.1.5；JSON 加载 + resolve / pointIdOf / 字节序重组 / 工程值还原）
//   - ModbusEngine          （§4.2.2 协议语义核心 / DevGuide §3A 3.1.3；QObject + signal/slot + moveToThread；
//                            含 onBytesReceived / writeRequest / responseParsed / frameError 信号槽链）
// 后续 3.1.4 PollScheduler 落 .cpp 时继续 append。
const char* anchorLayerName() noexcept {
    return "ens::protocol";
}

} // namespace ens::protocol