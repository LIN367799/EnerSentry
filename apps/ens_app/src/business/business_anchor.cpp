#include <ens/export.hpp>

namespace ens::business {

// BUILD 脚手架锚点：为 SHARED 库提供导出符号，避免空库 LNK4286。
// 真实 L4 类按 ENS-LLD-400 落地后，本文件整体删除。
ENS_BUSINESS_API const char* anchorLayerName() noexcept {
    return "ens::business";
}

} // namespace ens::business
