// src/datahub/datahub_anchor.cpp
// L3 数据中枢 BUILD 锚点（ENS-DEV-ARCH §3.3 / ENS-LLD-200 §3.1）。
// 当前承载契约：Sample（16B 对齐 lock-free）。新数据中枢类（L1SnapshotStore/RingBuffer/
// DataBus/DownSampler 等）陆续落地后,本文件保留作为层"门面",append 新锚即可。

#include "Sample.h"

namespace ens::datahub {

// 编译期联动：datahub_anchor 引入 Sample.h → 任何构建失败由 Sample 的双 static_assert 触发。
// 这是 L3 层的契约闸口：只要这一翻译单元被编译,Sample 契约就在 C++ 类型系统中生效。
const char* anchorLayerName() noexcept {
    return "ens::datahub";
}

}  // namespace ens::datahub
