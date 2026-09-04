// src/sim/sim_config.cpp —— 见 sim_config.h
#include "sim_config.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <stdexcept>

namespace ens::sim {

using json = nlohmann::json;

SimConfig SimConfig::loadFromJsonFile(const std::filesystem::path& path) {
    std::string content;
#if defined(_WIN32)
    std::FILE* fp = ::_wfopen(path.wstring().c_str(), L"rb");
    if (!fp) throw std::runtime_error("SimConfig: cannot open '" + path.generic_string() + "'");
    char buf[4096]; size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) content.append(buf, n);
    std::fclose(fp);
#else
    std::FILE* fp = std::fopen(path.string().c_str(), "rb");
    if (!fp) throw std::runtime_error("SimConfig: cannot open '" + path.generic_string() + "'");
    char buf[4096]; size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) content.append(buf, n);
    std::fclose(fp);
#endif
    json root;
    try {
        root = json::parse(content.begin(), content.end());
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("SimConfig: JSON parse error in ")
                                 + path.generic_string() + ": " + e.what());
    }
    SimConfig cfg;
    if (root.contains("tcp")) {
        const auto& t = root["tcp"];
        cfg.tcp.enabled = t.value("enabled", true);
        cfg.tcp.bindIp  = t.value("bindIp", "127.0.0.1");
        cfg.tcp.port    = t.value("port",    5020);
    }
    if (root.contains("rtu")) {
        const auto& r = root["rtu"];
        cfg.rtu.enabled  = r.value("enabled",  true);
        cfg.rtu.dev      = r.value("dev",      "COM4");
        cfg.rtu.baudRate = r.value("baudRate", 115200);
    }
    cfg.tickMs         = root.value("tickMs", 100u);
    cfg.seed           = root.value("seed",   0u);
    cfg.pointtablePath = root.value("pointtablePath", std::string{"data/sim_pointtable_sample.json"});
    if (root.contains("slaves")) {
        for (const auto& js : root["slaves"]) {
            SlaveSpec s;
            s.slaveId   = js.at("slaveId").get<uint8_t>();
            const std::string kind = js.value("kind", "Bms");
            if      (kind == "Bms")    s.kind = DeviceKind::Bms;
            else if (kind == "Pcs")    s.kind = DeviceKind::Pcs;
            else if (kind == "Meter")  s.kind = DeviceKind::Meter;
            else if (kind == "Liquid") s.kind = DeviceKind::Liquid;
            else if (kind == "Fire")   s.kind = DeviceKind::Fire;
            const std::string tr = js.value("transport", "Tcp");
            s.transport = (tr == "Rtu") ? Transport::Rtu : Transport::Tcp;
            s.regCount  = js.value("regCount", 0u);
            cfg.slaves.push_back(s);
        }
    }
    return cfg;
}

SimConfig SimConfig::defaultsFullTopology(uint32_t seed) {
    SimConfig cfg;
    cfg.tickMs = 100;
    cfg.seed   = seed;

    auto add = [&](uint8_t id, DeviceKind k, Transport t, uint16_t regs) {
        SlaveSpec s; s.slaveId = id; s.kind = k; s.transport = t; s.regCount = regs;
        cfg.slaves.push_back(s);
    };
    // BMS 16 簇(TCP,每簇 256 寄存器足以容纳 14 簇级 + 1280 单体 + 余量)
    for (uint8_t c = 1; c <= 16; ++c) add(c, DeviceKind::Bms, Transport::Tcp, 256);
    // PCS 4(TCP,每台 64)
    for (uint8_t p = 17; p <= 20; ++p) add(p, DeviceKind::Pcs, Transport::Tcp, 64);
    // 辅机 3(RTU)：meter(21)实际按 HLD §3.4 是 TCP,但 Phase 1 教学版 sample.json
    // 把 meter 归到 tcp;按 SIM-IMP §3.1 给完整拓扑做参考,Phase B6 启动时按
    // channels.json 分链路。
    add(21, DeviceKind::Meter,  Transport::Tcp,  32);
    add(22, DeviceKind::Liquid, Transport::Rtu,  64);
    add(23, DeviceKind::Fire,   Transport::Rtu,  64);
    return cfg;
}

std::vector<SlaveSpec> SimConfig::fromPointTable(const SimPointTable& pt) {
    std::vector<SlaveSpec> out;
    const auto ids = pt.allSlaveIds();
    out.reserve(ids.size());
    for (uint8_t id : ids) {
        // 看 point 上是否有 CellVolt/CellTemp/SOC/DCBus 等关键字启发 kind
        const auto& vs = pt.onSlave(id);
        DeviceKind kind = DeviceKind::Bms;   // default
        // 看 linkId / 命名启发
        bool pcsLike = false, meterLike = false, auxLike = false;
        for (const auto& p : vs) {
            if (p.pointName.find("PCS_") != std::string::npos ||
                p.pointName.find("Pcs") != std::string::npos) pcsLike = true;
            else if (p.pointName.find("Meter") != std::string::npos ||
                     p.pointName.find("ActiveP") != std::string::npos ||
                     p.pointName.find("TotalE")  != std::string::npos) meterLike = true;
            else if (p.pointName.find("Liquid") != std::string::npos ||
                     p.pointName.find("Fire")   != std::string::npos) auxLike = true;
        }
        if (pcsLike)        kind = DeviceKind::Pcs;
        else if (meterLike) kind = DeviceKind::Meter;
        else if (auxLike)   kind = (vs[0].pointName.find("Liquid") != std::string::npos)
                                  ? DeviceKind::Liquid
                                  : DeviceKind::Fire;

        // transport 启发 22/23 → RTU, 其余 TCP
        Transport tr = (id == 22 || id == 23) ? Transport::Rtu : Transport::Tcp;

        // regCount 取包含的最高寄存器地址 + 1(下界)
        uint16_t maxAddr = 0;
        for (const auto& p : vs) {
            if (p.registerAddr > maxAddr) maxAddr = p.registerAddr;
        }
        SlaveSpec s;
        s.slaveId   = id;
        s.kind      = kind;
        s.transport = tr;
        s.regCount  = static_cast<uint16_t>(maxAddr + 16);   // 16 寄存器 slack
        out.push_back(s);
    }
    return out;
}

}  // namespace ens::sim
