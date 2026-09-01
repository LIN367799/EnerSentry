// src/business/AlarmRuleLoader.h
// L4 业务层 —— 告警规则 JSON 加载器（FR-CFG-06 热加载入口；切片 16 静态加载接入）。
//
// schema（与 AlarmRule 字段一一对应；pointName 引用使规则文件与点表解耦）：
//   {
//     "rules": [
//       { "pointName": "Rack-01_MaxTemp", "level": "Warning",
//         "onThreshold": 60.0, "offThreshold": 55.0,
//         "onDelayMs": 3000, "offDelayMs": 3000, "suppressWindowMs": 60000,
//         "enabled": true }
//     ]
//   }
//
// 关键设计：
//   * pointName → pointId 在加载时经 PointTable::allPoints 一次遍历解析
//     （规则文件不感知点表 pointId 分配，联调/热更新零漂移）
//   * 文件读取走 _wfopen 宽字符路径（MSVC 中文路径坑，与 SimPointTable 同策略）
//   * 单条规则解析失败：跳过 + err 累积，不整体失败（部分可用）

#pragma once

#include <ens/export.hpp>
#include "AlarmEntities.h"
#include "../protocol/PointTable.h"

#include <filesystem>
#include <string>
#include <vector>

namespace ens::business {

/// 告警规则 JSON 加载器（切片 16；ens_business 为 SHARED，类需导出宏）
class ENS_BUSINESS_API AlarmRuleLoader {
public:
    /// 加载规则 JSON 并解析 pointName → pointId。
    /// @param jsonPath 规则 JSON 路径（支持中文路径）
    /// @param pt       主程序点表（allPoints 遍历匹配 pointName）
    /// @param out      输出规则（成功的条目追加）
    /// @return 成功解析的规则数；<0 表示文件/JSON 整体失败（err 填原因）
    static int loadFromFile(const std::filesystem::path& jsonPath,
                            const protocol::PointTable& pt,
                            std::vector<AlarmRule>& out,
                            std::string* err = nullptr);

    /// "Warning"/"Critical"/"Info" → AlarmLevel；未知返回 false
    static bool levelFromString(const std::string& s, AlarmLevel& out) noexcept;

private:
    /// pointName → pointId（O(n) 一次遍历；找不到返 0）
    static uint32_t resolvePointId(const protocol::PointTable& pt,
                                   const std::string& name) noexcept;
};

}  // namespace ens::business
