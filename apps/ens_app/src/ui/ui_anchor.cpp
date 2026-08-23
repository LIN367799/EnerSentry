namespace ens::ui {

// BUILD 脚手架锚点：为 STATIC 库提供符号，避免空库告警。
// STATIC：禁引 ENS_*_API 导出宏（ENS-DEV-ARCH §3.7）。
// 真实 L5 类按 ENS-LLD-500 落地后，本文件整体删除。
const char* anchorLayerName() noexcept {
    return "ens::ui";
}

} // namespace ens::ui
