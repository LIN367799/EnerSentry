// src/ui/common/CsvWriter.h —— 通用 CSV 导出工具（切片 34，FR-EXP-01/03）。
// 纯 std 实现（零 Qt 依赖）：UTF-8 带 BOM 输出（Windows Excel 直接识别中文），
// RFC 4180 字段转义（含逗号/双引号/换行的字段整体双引号包裹，内部 " 翻倍为 ""）。
// 历史趋势导出（FR-EXP-01）与告警历史导出（FR-EXP-03，后续切片）复用本工具。
// 线程非安全，单线程顺序写。
#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace ens::ui {

class CsvWriter {
public:
    /// @param path 输出路径（std::filesystem::path 宽字符重载，中文路径安全）
    explicit CsvWriter(const std::filesystem::path& path);
    ~CsvWriter();

    CsvWriter(const CsvWriter&) = delete;
    CsvWriter& operator=(const CsvWriter&) = delete;

    bool isOpen() const noexcept { return m_out.is_open(); }

    /// 打开文件（trunc）并写入 UTF-8 BOM（EF BB BF）。
    /// 已打开或打开失败（目录不存在/无权限）返回 false。
    bool open();

    /// 写一行：字段间逗号分隔，行尾 CRLF（RFC 4180）。
    /// 未 open 时返回 false；单字段内自动转义。
    bool writeRow(const std::vector<std::string>& fields);

    /// 关闭输出流（析构时自动调用）
    void close();

    /// 静态字段转义（供单测直接验证）
    static std::string escapeField(const std::string& field);

private:
    std::filesystem::path m_path;
    std::ofstream         m_out;
};

}  // namespace ens::ui
