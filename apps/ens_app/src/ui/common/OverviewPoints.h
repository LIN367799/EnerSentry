// src/ui/common/OverviewPoints.h —— 总览测点分类（切片 40，FR-OV-06/07）。
// 点表无 kind 字段，簇归属与量测类型按 pointName 约定解析：`Rack-<no>_<measure>`。
//   覆核点表样例：Rack-01_SOC / Rack-01_MaxTemp / Rack-01_AvgTemp / Liquid_SupplyTemp。
// 纯函数（无 Qt Widget 依赖）→ 可独立单测。
#pragma once

#include <QString>

#include <cstdint>

namespace ens::ui {

enum class OvrKind { None, Soc, ClusterMaxTemp };

struct OvrPointInfo {
    OvrKind kind   = OvrKind::None;
    int     rackNo = -1;   // -1 = 无簇归属
};

/// 解析测点名 → 总览分类。规则：
///   * 以 "Rack-" 开头 → 提取簇号（数字段）；再按量测后缀识别（"_SOC" / "_MaxTemp"）
///   * 其余（非 Rack 前缀或量测非目标）→ None
inline OvrPointInfo ovrClassifyName(const QString& name) {
    OvrPointInfo info;
    if (!name.startsWith(QStringLiteral("Rack-"))) return info;
    // 簇号：Rack-<digits>_
    int i = 5;   // 跳过 "Rack-"
    int rackNo = 0;
    bool any = false;
    while (i < name.size() && name[i].isDigit()) {
        rackNo = rackNo * 10 + (name[i].unicode() - '0');
        ++i;
        any = true;
    }
    if (!any || i >= name.size() || name[i] != '_') return info;
    info.rackNo = rackNo;
    const QString measure = name.mid(i + 1);
    if (measure == QStringLiteral("SOC")) {
        info.kind = OvrKind::Soc;
    } else if (measure.contains(QStringLiteral("MaxTemp"))) {
        info.kind = OvrKind::ClusterMaxTemp;
    }
    return info;
}

/// 簇显示标签（Rack-01）
inline QString ovrRackLabel(int rackNo) {
    return QStringLiteral("Rack-%1").arg(rackNo, 2, 10, QLatin1Char('0'));
}

}  // namespace ens::ui
