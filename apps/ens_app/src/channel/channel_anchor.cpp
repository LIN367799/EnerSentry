#include <ens/export.hpp>

namespace ens::channel {

// BUILD 脚手架锚点：为 SHARED 库提供导出符号，避免空库 LNK4286。
// 真实 L1 类按 ENS-LLD-100 落地后，本文件整体删除。
ENS_CHANNEL_API const char* anchorLayerName() noexcept {
    return "ens::channel";
}

} // namespace ens::channel
