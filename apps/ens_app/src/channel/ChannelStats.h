// ChannelStats.h —— Phase 1 L1：原子统计快照（ENS-LLD-100 §3.6 / ICD §2.4）。
// 全 atomic 字段，getStats() 即时返回无锁快照；采集线程高频更新、UI 线程低频读取。
// 60s 滑动窗口（SlidingQualityEstimator）延后 Phase 2 补，避免 2.1.1 范围爆炸。
#pragma once

#include "ens/export.hpp"
#include <atomic>
#include <cstdint>

namespace ens::channel {

struct ENS_CHANNEL_API ChannelStats {
    std::atomic<uint64_t> requestTotal{0};      // 下发请求总次数
    std::atomic<uint64_t> responseSuccess{0};   // 成功响应次数（含校验通过）
    std::atomic<uint64_t> timeoutCount{0};      // 超时次数
    std::atomic<uint64_t> crcErrorCount{0};     // CRC 校验失败次数
    std::atomic<uint64_t> bytesSent{0};
    std::atomic<uint64_t> bytesReceived{0};
    std::atomic<int64_t>  avgRttUs{0};          // 平均往返时延（微秒，指数加权移动平均）

    // 成功率（%）；requestTotal==0 时返回 100（链路空闲不报警）
    double qualityPercent() const {
        const uint64_t total   = requestTotal.load(std::memory_order_acquire);
        if (total == 0) return 100.0;
        const uint64_t success = responseSuccess.load(std::memory_order_acquire);
        return (static_cast<double>(success) / static_cast<double>(total)) * 100.0;
    }

    // 注意：std::atomic 不可拷贝（deleted copy ctor）→ ChannelStats 整体不可拷贝；
    // 因此取消 LLD §3.6 的 snapshot()（"return *this" 无法编译），改为由 IChannel::getStats()
    // 直接返回 const ChannelStats&，调用方对每个字段单独 load()（仍是无锁读）。
    // 典型用法：const auto& s = ch->getStats(); auto n = s.requestTotal.load();
};

}  // namespace ens::channel