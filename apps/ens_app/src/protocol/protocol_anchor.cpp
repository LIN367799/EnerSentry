namespace ens::protocol {

// L2 协议层 BUILD 锚（ENS-DEV-ARCH §3.2）。
// 当前承载契约：
//   - ModbusStreamAccumulator（§4.2.1 字节流拼帧  +  §4.2.1.1 异常帧 5B/9B）
//   - TransactionIdAllocator（§4.3.6 / DevGuide §2A TCP 16-bit 事务 ID 位图）
//   - Crc16                （§4.1.2 CRC-16/MODBUS 查表,与 sim 侧 byte-by-byte 等价）
//   - ModbusFrame         （§4.1 组帧/§4.1.1 RTU/TCP/§4.1.3 FC01~06+0F+10+异常 5B9B）
// 后续 3.1.3 ModbusEngine / 3.1.4 PollScheduler / 3.1.5 PointTable append 即可。
const char* anchorLayerName() noexcept {
    return "ens::protocol";
}

} // namespace ens::protocol
