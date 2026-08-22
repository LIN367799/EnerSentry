#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ptgen.py — EnerSentry 设备模拟器点表生成器
============================================
依据 HLD-SIM §3.2~3.7 的权威寄存器映射，生成测试台与主程序共享的
pointtable.json（或代表性样例 sim_pointtable_sample.json）。

权威映射（必须与 ENS-HLD-SIM 保持一致，无"待定"）：
  BMS_BASE(c)   = 0x1000 + (c-1)*0x600        c ∈ [1,16]   从站 1..16   Modbus TCP
  PCS_BASE(p)   = 0x2000 + (p-1)*0x200        p ∈ [1,4]    从站 17..20  Modbus TCP
  METER_BASE    = 0x3000                      从站 21       Modbus TCP
  AUX_COOL_BASE = 0x4000                      从站 22       Modbus RTU
  AUX_FIRE_BASE = 0x4100                      从站 23       Modbus RTU

注意：历史上样例 JSON 曾用 BMS 基址 0x100（且簇内布局偏移与权威不符），
本生成器一律以 HLD-SIM 权威公式为唯一真相，消除陈旧数字与布局偏差。

用法：
  python ptgen.py --sample  [-o data/sim_pointtable_sample.json]
  python ptgen.py --full    [-o data/sim_pointtable_full.json]
  python ptgen.py           # 默认 --sample
"""

import argparse
import json
import sys

# ----------------------------- 权威常量 ----------------------------------
N_BMS = 16
N_PCS = 4

BMS_BASE = lambda c: 0x1000 + (c - 1) * 0x600       # c in 1..16
PCS_BASE = lambda p: 0x2000 + (p - 1) * 0x200       # p in 1..4
METER_BASE = 0x3000
AUX_COOL_BASE = 0x4000
AUX_FIRE_BASE = 0x4100

BMS_SLAVE = lambda c: c                              # 1..16
PCS_SLAVE = lambda p: 16 + p                         # 17..20
METER_SLAVE = 21
LIQUID_SLAVE = 22
FIRE_SLAVE = 23

ABC = "ABCD"  # 浮点/整型统一大端(Motorola)，与 ICD 示例一致


def entry(name, slave, rtype, addr, dtype, scale, unit, poll, pri):
    """构造单条点表项。linkId 采用与 slaveAddress 一致的逻辑——
    主程序按 slaveAddress 寻址，linkId 作设备唯一标识，消除旧样例
    linkId≠slaveAddress 的混乱（HLD 未定义 linkId 编号方案，此为本实现约定）。"""
    return {
        "pointId": 0,  # 占位，生成后统一回填
        "pointName": name,
        "linkId": slave,
        "slaveAddress": slave,
        "regType": rtype,
        "registerAddr": addr,
        "dataType": dtype,
        "byteOrder": ABC,
        "scaleFactor": scale,
        "offset": 0.0,
        "unit": unit,
        "pollIntervalMs": poll,
        "priority": pri,
        "enabled": True,
    }


# ----------------------------- BMS --------------------------------------
def bms_points(c, full=False):
    s = BMS_SLAVE(c)
    base = BMS_BASE(c)
    pts = []
    # 簇级标量块（HLD-SIM §3.3，占 +0x00..+0x0E）
    scalars = [
        ("MaxTemp", 0x00, "Float32", 0.1, "C", 1),
        ("SOC", 0x02, "Float32", 0.01, "%", 1),
        ("SOH", 0x04, "Float32", 0.01, "%", 2),
        ("AvgTemp", 0x06, "Float32", 0.1, "C", 1),
        ("TotalV", 0x08, "Float32", 0.01, "V", 1),
        ("Current", 0x0A, "Float32", 0.01, "A", 1),
        ("BalanceWord", 0x0C, "Uint16", 1, "bit", 2),
        ("AlarmWord", 0x0D, "Uint16", 1, "bit", 1),
        ("StatusWord", 0x0E, "Uint16", 1, "bit", 2),
    ]
    for nm, off, dt, sc, unit, pri in scalars:
        pts.append(entry(f"Rack-{c:02d}_{nm}", s, "HoldingRegister",
                         base + off, dt, sc, unit, 1000, pri))
    # 单体电压 640 点（HLD-SIM §3.4，从 +0x10 起，uint16/mV）
    n_v = 640 if full else (8 if c == 1 else 0)
    for i in range(n_v):
        pts.append(entry(f"Rack-{c:02d}_CellV_{i:03d}", s, "InputRegister",
                         base + 0x10 + i, "Uint16", 0.001, "V", 5000, 3))
    # 单体温度 640 点（HLD-SIM §3.4，从 +0x290 起）
    n_t = 640 if full else (8 if c == 1 else 0)
    for i in range(n_t):
        pts.append(entry(f"Rack-{c:02d}_CellT_{i:03d}", s, "InputRegister",
                         base + 0x290 + i, "Uint16", 0.1, "C", 5000, 3))
    return pts


# ----------------------------- PCS --------------------------------------
def pcs_points(p):
    s = PCS_SLAVE(p)
    base = PCS_BASE(p)
    pts = []
    regs = [
        ("ActiveP", 0x00, "Float32", 0.01, "kW", 1),
        ("ReactiveQ", 0x02, "Float32", 0.01, "kVar", 1),
        ("Voltage", 0x04, "Float32", 0.01, "V", 1),
        ("Current", 0x06, "Float32", 0.01, "A", 1),
        ("Freq", 0x08, "Float32", 0.01, "Hz", 1),
        ("Mode", 0x0A, "Uint16", 1, "", 2),
        ("FaultWord", 0x0B, "Uint16", 1, "bit", 1),
        ("Status", 0x0C, "Uint16", 1, "bit", 2),
    ]
    for nm, off, dt, sc, unit, pri in regs:
        pts.append(entry(f"PCS-{p:02d}_{nm}", s, "HoldingRegister",
                         base + off, dt, sc, unit, 1000, pri))
    # 控制线圈（供 SBO 写，HLD-SIM §3.5：PCS_BASE+0x1000 排风 / +0x2000 液冷）
    pts.append(entry(f"PCS-{p:02d}_ExhaustCtrl", s, "Coil",
                     base + 0x1000, "Bool", 1, "", 1000, 2))
    pts.append(entry(f"PCS-{p:02d}_LiquidCtrl", s, "Coil",
                     base + 0x2000, "Bool", 1, "", 1000, 2))
    return pts


# --------------------------- 电表 / 辅机 --------------------------------
def meter_points():
    s = METER_SLAVE
    base = METER_BASE
    # HLD-SIM §3.6：正向总有功电能为 int32（累计量）
    return [entry("Meter_ActiveEnergy", s, "InputRegister",
                  base + 0x00, "Int32", 0.01, "kWh", 2000, 2)]


def aux_points():
    pts = []
    # 液冷辅机（HLD-SIM §3.7，从站 22，RTU）
    pts.append(entry("Liquid_SupplyTemp", LIQUID_SLAVE, "InputRegister",
                     AUX_COOL_BASE + 0x00, "Float32", 0.1, "C", 2000, 2))
    # 消防辅机（从站 23，RTU）
    pts.append(entry("Fire_Alarm", FIRE_SLAVE, "DiscreteInput",
                     AUX_FIRE_BASE + 0x00, "Bool", 1, "", 1000, 1))
    return pts


# --------------------------- 全量 / 样例 --------------------------------
def gen_full():
    pts = []
    for c in range(1, N_BMS + 1):
        pts += bms_points(c, full=True)
    for p in range(1, N_PCS + 1):
        pts += pcs_points(p)
    pts += meter_points()
    pts += aux_points()
    return pts


def gen_sample():
    """代表性样例：对齐旧样例的 43 点结构，但所有地址/从站以权威公式重算。"""
    pts = []
    pts += bms_points(1, full=False)          # Rack-01：9 标量 + 8 电压 + 8 温度
    pts.append(entry("Rack-02_MaxTemp", BMS_SLAVE(2), "HoldingRegister",
                     BMS_BASE(2) + 0x00, "Float32", 0.1, "C", 1000, 1))
    pts.append(entry("Rack-02_SOC", BMS_SLAVE(2), "HoldingRegister",
                     BMS_BASE(2) + 0x02, "Float32", 0.01, "%", 1000, 1))
    pts += pcs_points(1)                       # PCS-01 全 10 点
    pts.append(entry("PCS-02_ActiveP", PCS_SLAVE(2), "HoldingRegister",
                     PCS_BASE(2) + 0x00, "Float32", 0.01, "kW", 1000, 1))
    pts.append(entry("PCS-02_FaultWord", PCS_SLAVE(2), "HoldingRegister",
                     PCS_BASE(2) + 0x0B, "Uint16", 1, "bit", 1000, 1))
    pts += meter_points()
    pts += aux_points()
    pts.append(entry("Rack-16_AlarmWord", BMS_SLAVE(16), "HoldingRegister",
                     BMS_BASE(16) + 0x0D, "Uint16", 1, "bit", 1000, 1))
    return pts


def finalize(pts, generator_note):
    for i, p in enumerate(pts, start=1):
        p["pointId"] = i
    slaves = sorted({p["slaveAddress"] for p in pts})
    meta = {
        "generator": "ptgen.py (ENS-HLD-SIM §3.2~3.7 权威映射)",
        "schemaVersion": "1.1",
        "deviceCount": len(slaves),
        "pointCount": len(pts),
        "slaveAddresses": slaves,
        "note": generator_note,
    }
    return {"meta": meta, "points": pts}


def main():
    ap = argparse.ArgumentParser(description="EnerSentry 点表生成器")
    grp = ap.add_mutually_exclusive_group()
    grp.add_argument("--sample", action="store_true", help="生成代表性样例(默认)")
    grp.add_argument("--full", action="store_true", help="生成全量点表(~20800点)")
    ap.add_argument("-o", "--output", default=None, help="输出文件路径")
    args = ap.parse_args()

    if args.full:
        pts = gen_full()
        note = ("全量点表：16 BMS 簇(各 9 标量+640 电压+640 温度) + 4 PCS + 1 电表 + "
                "2 辅机；registerAddr 采用 HLD-SIM §3.3~3.7 权威基址。")
        out = args.output or "data/sim_pointtable_full.json"
    else:
        pts = gen_sample()
        note = ("代表性样例(43点)：已对齐 HLD-SIM §3.2 默认拓扑——PCS 从站 17~20、"
                "BMS 基址 0x1000+(c-1)*0x600；linkId 与 slaveAddress 一致；"
                "全量请用 ptgen.py --full 生成。registerAddr 为各设备 BASE 常量下的绝对地址。")
        out = args.output or "data/sim_pointtable_sample.json"

    doc = finalize(pts, note)
    text = json.dumps(doc, indent=2, ensure_ascii=False)
    if out == "-":
        sys.stdout.write(text + "\n")
    else:
        with open(out, "w", encoding="utf-8") as f:
            f.write(text + "\n")
        print(f"[ptgen] wrote {len(pts)} points -> {out}")


if __name__ == "__main__":
    main()
