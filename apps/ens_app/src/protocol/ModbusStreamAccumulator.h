// src/protocol/ModbusStreamAccumulator.h
// RTU/TCP 字节流环形累加器：拼帧 + RTU CRC 校验，零动态分配（ENS-LLD-100 §4.2.1）。
// 关键点：覆盖前移 m_read；脏数据前滑达 4KB 清空；异常帧按 5B/9B 定长提取。

#pragma once

#include <array>
#include <algorithm>
#include <cstdint>
#include <cstddef>

namespace ens::protocol {

class ModbusStreamAccumulator {
public:
    static constexpr size_t kCapacity = 4096;      // 环形缓冲容量
    static constexpr size_t kMaxHuntBytes = 4096;  // 脏数据前滑预算上限

    // 追加字节；覆盖时同步前移读指针（V1.2）。
    void append(const uint8_t* data, size_t len) noexcept {
        if (data == nullptr || len == 0) return;
        if (m_size + len > kCapacity) {
            const size_t overflow = (m_size + len) - kCapacity;
            m_read = (m_read + overflow) % kCapacity;
        }
        for (size_t i = 0; i < len; ++i)
            m_buf[(m_write + i) % kCapacity] = data[i];
        m_write = (m_write + len) % kCapacity;
        m_size  = std::min(m_size + len, kCapacity);   // 溢出丢最旧（脏数据）
    }

    // 提取一帧（RTU 含 CRC 校验）；成功填充 out/outLen 返回 true。
    bool tryExtractFrame(uint8_t* out, size_t& outLen, bool isTcp) noexcept;

    // 丢弃前 n 字节（脏数据 HUNT 前滑）；累计达预算直接清空。
    void popFront(size_t n) noexcept {
        if (n == 0) return;
        advance(n);
        m_huntSlidBytes += n;
        if (m_huntSlidBytes >= kMaxHuntBytes) clear();
    }

    // 提帧成功后复位前滑预算（同步已恢复）。
    void onFrameExtracted() noexcept { m_huntSlidBytes = 0; }

    void clear() noexcept {
        m_read = m_write = m_size = 0;
        m_huntCount = 0;
        m_huntSlidBytes = 0;
    }

    size_t huntCount() const noexcept { return m_huntCount; }

private:
    uint8_t peek(size_t offset) const noexcept {
        return m_buf[(m_read + offset) % kCapacity];
    }
    // 消费 n 字节（合法帧路径，不计脏数据预算）。
    void consume(size_t n) noexcept { if (n != 0) advance(n); }
    // 读出 n 字节并消费（不校验）。
    void pop(size_t n, uint8_t* out, size_t& outLen) noexcept {
        outLen = n;
        for (size_t i = 0; i < n; ++i)
            out[i] = m_buf[(m_read + i) % kCapacity];
        consume(n);
    }
    // 前移读指针并收缩 m_size（popFront/consume 共用）。
    void advance(size_t n) noexcept {
        m_read = (m_read + n) % kCapacity;
        m_size = (m_size >= n) ? (m_size - n) : 0;
    }
    // 校验环形缓冲中前 len 字节的 CRC-16/MODBUS（RTU 帧）。
    bool crcValid(size_t len) const noexcept;

    std::array<uint8_t, kCapacity> m_buf{};
    size_t m_read = 0, m_write = 0, m_size = 0;
    uint32_t m_huntCount = 0;      // 历史累计同步丢失次数（诊断）
    size_t   m_huntSlidBytes = 0;  // 当前同步周期前滑字节数
};

}  // namespace ens::protocol
