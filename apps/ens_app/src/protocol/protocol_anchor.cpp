namespace ens::protocol {

// BUILD 脚手架锚点：为 STATIC 库提供符号，避免空库告警。
// 真实 L2 类按 ENS-LLD-100 落地后，本文件整体删除。
const char* anchorLayerName() noexcept {
    return "ens::protocol";
}

} // namespace ens::protocol
