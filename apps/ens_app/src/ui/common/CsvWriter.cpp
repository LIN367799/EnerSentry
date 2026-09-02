// src/ui/common/CsvWriter.cpp —— 通用 CSV 导出工具实现（切片 34）。
#include "common/CsvWriter.h"

namespace ens::ui {

namespace {
/// UTF-8 BOM
constexpr char kBom[3] = {'\xEF', '\xBB', '\xBF'};
}  // namespace

CsvWriter::CsvWriter(const std::filesystem::path& path) : m_path(path) {}

CsvWriter::~CsvWriter() {
    close();
}

bool CsvWriter::open() {
    if (m_out.is_open()) return false;
    // ofstream 的 filesystem::path 重载走宽字符路径，中文路径安全（MSVC）
    m_out.open(m_path, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!m_out.is_open()) return false;
    m_out.write(kBom, sizeof(kBom));
    return m_out.good();
}

bool CsvWriter::writeRow(const std::vector<std::string>& fields) {
    if (!m_out.is_open()) return false;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i != 0) m_out.put(',');
        m_out << escapeField(fields[i]);
    }
    m_out << "\r\n";
    return m_out.good();
}

void CsvWriter::close() {
    if (m_out.is_open()) m_out.close();
}

std::string CsvWriter::escapeField(const std::string& field) {
    bool needQuote = false;
    for (char c : field) {
        if (c == ',' || c == '"' || c == '\r' || c == '\n') {
            needQuote = true;
            break;
        }
    }
    if (!needQuote) return field;

    std::string out;
    out.reserve(field.size() + 2);
    out.push_back('"');
    for (char c : field) {
        if (c == '"') out.push_back('"');   // RFC 4180: 内部 " 翻倍
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

}  // namespace ens::ui
