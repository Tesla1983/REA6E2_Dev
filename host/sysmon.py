#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
sysmon.py —— 上位机整机监控（GPU 走 Intel IGCL，CPU/内存走 psutil）

定位
    igcl_probe.py 只负责 GPU（已实测跑通，保持原样不动）。
    本脚本是"整机上机位"：在 GPU 之外补上 CPU 与内存，并统一成一份数据结构，
    供串口发给 RA6E2 → 显示在 3.5" 128×64 单色屏（VTM88723V01 / NT7534）上。
    原 1.3" OLED(SH1106) 也能吃同一套数据，只是只有 4 行 × 16 字符。

采集项与数据来源
    ┌────────────┬──────────────────────┬──────────────────────────────────┐
    │ 指标       │ 来源                 │ 本机实测情况                     │
    ├────────────┼──────────────────────┼──────────────────────────────────┤
    │ CPU 使用率 │ psutil               │ 可用                             │
    │ CPU 频率   │ psutil               │ 可用                             │
    │ 内存占用率 │ psutil               │ 可用                             │
    │ CPU 温度   │ LHM(优先)/PDH(兜底)  │ 原生只有 ACPI 静态值，不可信     │
    │ CPU 风扇   │ LHM(唯一途径)        │ 原生拿不到转速                   │
    │ 内存温度   │ LHM(仅 DDR5 SPD Hub) │ 本机 DDR4 UDIMM 无传感器 → 不采  │
    │ 显存/GPU 温 │ IGCL (ControlLib)    │ 可用                             │
    │ GPU 使用率 │ DXGI+PDH \\GPU Engine│ 可用（IGCL 没有这个能力）        │
    └────────────┴──────────────────────┴──────────────────────────────────┘

最终上屏的 7 项（就是 --line 的字段，改这里等于改协议，必须同步改固件）
    CT CPU 温度 °C      CU CPU 使用率 %      CF CPU 风扇 RPM
    GT GPU 核心温度 °C   GU GPU 使用率 %      GF GPU 风扇 RPM
    MU 内存(RAM) 使用率 %

    显存占用(原 VR) 与显存温度(原 VT) 已按需求移除；原始数据仍保留在 --json 里，
    需要时改 to_line() 一行就能加回来。

为什么 CPU 温度要分两层（重要，别被数字骗了）
    Windows 用户态**没有**通用途径读 CPU 核心温度：
      * psutil.sensors_temperatures() 仅在 Linux/FreeBSD 存在，Windows 上不存在；
      * WMI  MSAcpi_ThermalZoneTemperature  需管理员，且台式机上多数是固件占位值；
      * PDH  '\\Thermal Zone Information(*)\\Temperature' 不需要管理员，但实测本机
             （i5-12490F）6 进程满载 8 秒后数值**完全不变化**，是静态占位值。
    真正的核心温度在 IA32_THERM_STATUS MSR 里，读它必须内核态驱动 —— 这就是
    LibreHardwareMonitor 存在的原因（它自带驱动读 MSR/SuperIO/EC）。
    因此本脚本：
      * 有 LHM  → 用 LHM 的真实读数，标记 source=lhm-http，可信；
      * 无 LHM  → 用 PDH 的 ACPI 值，标记 source=pdh-acpi，trusted=false，
                  **默认不进入 --line 输出**（除非显式 --allow-acpi-temp），
                  避免把 27.9 这种恒定值当真温度发到屏上。

内存温度
    DDR4 消费级 UDIMM 普遍不带温度传感器（只有部分发烧条/服务器 RDIMM 带 TS，
    DDR5 才普遍有 SPD Hub 温度）。本机 2×16G DDR4-3200（光威 AGU4C3200C16/16G）
    实测：PDH 的 135 个计数器对象里无任何 DIMM/Memory 温度项 → 直接放弃该指标。
    若将来换 DDR5，装了 LHM 后本脚本会自动把它读出来（不用改代码）。

安全边界
    * 只读：IGCL 只调 Enum*/Get*，绝不调任何 Set*（风扇控制/超频/功耗）。
    * 不装任何驱动、不改系统设置：LHM 只是"探测是否在运行"，不代为安装。
    * 采样不使用 CPU 负载法（不烧机）。

用法
    python sysmon.py                       # 读一次，人类可读
    python sysmon.py --json                # 读一次，JSON
    python sysmon.py --line                # 单行紧凑格式（给串口/MCU 用）
    python sysmon.py --loop 5 --interval 1
    python sysmon.py --providers           # 只列数据源探测结果，不读 GPU
    python sysmon.py --selftest            # 自检：结构体 + 各数据源 + 一次采集
    python sysmon.py --no-gpu              # 跳过 GPU（只测 CPU/内存）

退出码
    0 成功；2 IGCL 加载失败（--no-gpu 时不算失败）；3 ctlInit 失败；5 采集部分失败
"""

import argparse
import ctypes as C
import ctypes.wintypes as W
import json
import os
import platform
import re
import sys
import time
import urllib.error
import urllib.request

# 只放宽错误处理，**不要**强制 encoding="utf-8"：
# Windows 上 PowerShell 5.1 按 GBK(ACP) 解码子进程输出，强制 UTF-8 会让中文全变乱码；
# 而不指定编码时 Python 跟随终端 locale，Git Bash 与 PowerShell 都能正常显示。
try:
    sys.stdout.reconfigure(errors="replace")
    sys.stderr.reconfigure(errors="replace")
except Exception:
    pass

try:
    import psutil
    HAVE_PSUTIL = True
except ImportError:
    HAVE_PSUTIL = False

# GPU 部分复用已跑通的探针，避免重复维护 ctypes 结构体
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
try:
    from igcl_probe import (Igcl, IgclError, rc_name,
                            TEMP_SENSOR_NAMES, MEM_TYPE_NAMES)
    HAVE_IGCL_MODULE = True
except Exception:                                    # noqa: BLE001
    HAVE_IGCL_MODULE = False


# ============================================================================
# 一、PDH（Windows 性能计数器）—— 读 ACPI 热区温度
# ============================================================================
pdh = None
if os.name == "nt":
    try:
        pdh = C.WinDLL("pdh.dll")
        # 一律按无符号返回：0x800007D2 = PDH_MORE_DATA，有符号解释会变负数
        pdh.PdhOpenQueryW.argtypes = [W.LPCWSTR, C.c_void_p, C.POINTER(C.c_void_p)]
        pdh.PdhOpenQueryW.restype = C.c_uint32
        pdh.PdhAddEnglishCounterW.argtypes = [C.c_void_p, W.LPCWSTR, C.c_void_p,
                                              C.POINTER(C.c_void_p)]
        pdh.PdhAddEnglishCounterW.restype = C.c_uint32
        pdh.PdhCollectQueryData.argtypes = [C.c_void_p]
        pdh.PdhCollectQueryData.restype = C.c_uint32
        pdh.PdhGetFormattedCounterArrayW.argtypes = [
            C.c_void_p, W.DWORD, C.POINTER(W.DWORD), C.POINTER(W.DWORD), C.c_void_p]
        pdh.PdhGetFormattedCounterArrayW.restype = C.c_uint32
        pdh.PdhCloseQuery.argtypes = [C.c_void_p]
        pdh.PdhCloseQuery.restype = C.c_uint32
    except Exception:                                # noqa: BLE001
        pdh = None

PDH_FMT_DOUBLE = 0x00000200
PDH_MORE_DATA = 0x800007D2
KELVIN_OFFSET = 273.15


class PDH_FMT_COUNTERVALUE(C.Structure):
    """真实定义: DWORD CStatus; union {LONG/double/LONGLONG ...}
    CStatus 占 4 字节后有 padding，double 落在 offset 8。
    （踩过的坑：把 union 当成 offset 0 会读到 CStatus=0 → 温度恒为 -273.15）"""
    _fields_ = [("CStatus", W.DWORD), ("doubleValue", C.c_double)]


class PDH_FMT_COUNTERVALUE_ITEM_W(C.Structure):
    _fields_ = [("szName", W.LPWSTR), ("FmtValue", PDH_FMT_COUNTERVALUE)]


class PdhCounter(object):
    def __init__(self, path_english):
        if pdh is None:
            raise RuntimeError("pdh.dll 不可用（非 Windows？）")
        self.hq = C.c_void_p()
        rc = pdh.PdhOpenQueryW(None, C.c_void_p(0), C.byref(self.hq))
        if rc != 0:
            raise RuntimeError("PdhOpenQuery rc=0x%08X" % rc)
        self.hc = C.c_void_p()
        rc = pdh.PdhAddEnglishCounterW(self.hq, path_english,
                                       C.c_void_p(0), C.byref(self.hc))
        if rc != 0:
            pdh.PdhCloseQuery(self.hq)
            raise RuntimeError("PdhAddEnglishCounter rc=0x%08X" % rc)
        pdh.PdhCollectQueryData(self.hq)             # 建立基线

    def read_array(self):
        pdh.PdhCollectQueryData(self.hq)
        size, count = W.DWORD(0), W.DWORD(0)
        rc = pdh.PdhGetFormattedCounterArrayW(
            self.hc, PDH_FMT_DOUBLE, C.byref(size), C.byref(count), None)
        if rc not in (0, PDH_MORE_DATA):
            raise RuntimeError("size query rc=0x%08X" % rc)
        if size.value == 0:
            return []
        buf = (C.c_ubyte * size.value)()
        rc = pdh.PdhGetFormattedCounterArrayW(
            self.hc, PDH_FMT_DOUBLE, C.byref(size), C.byref(count), buf)
        if rc != 0:
            raise RuntimeError("read rc=0x%08X" % rc)
        items = C.cast(buf, C.POINTER(PDH_FMT_COUNTERVALUE_ITEM_W))
        return [(items[i].szName, items[i].FmtValue.doubleValue)
                for i in range(count.value)]

    def close(self):
        if self.hq:
            pdh.PdhCloseQuery(self.hq)
            self.hq = C.c_void_p()


class AcpiTemperature(object):
    """PDH 的 ACPI 热区温度。台式机上多数是固件占位值（实测不随负载变化），
    所以 trusted 恒为 False —— 只作参照，默认不进串口输出。"""

    counter = None
    last_error = None

    def probe(self):
        try:
            if AcpiTemperature.counter is None:
                AcpiTemperature.counter = PdhCounter(
                    r"\Thermal Zone Information(*)\Temperature")
            return True
        except Exception as e:                       # noqa: BLE001
            AcpiTemperature.last_error = str(e)
            return False

    def read(self):
        """返回 (celsius 或 None, 实例名)"""
        if not self.probe():
            return None, None
        try:
            vals = self.counter.read_array()
        except Exception as e:                       # noqa: BLE001
            AcpiTemperature.last_error = str(e)
            return None, None
        if not vals:
            return None, None
        name, kelvin = vals[0]
        # 占位值常见 0 K / 273.15 K(0 °C) 之外明显异常的一律弃用
        if kelvin is None or kelvin <= 200.0:
            return None, name
        return round(kelvin - KELVIN_OFFSET, 1), name


# ============================================================================
# 二、LibreHardwareMonitor 接入（可选；装了就能拿到真实 CPU 温度/风扇）
# ============================================================================
LHM_DEFAULT_URL = "http://127.0.0.1:8085/data.json"

# --- CPU 风扇通道名（2026-10-24 实测定值）---------------------------------
# LHM 对 NCT6687D 用的是**通用通道名**（CPU Fan / System Fan #1..#6），
# 与主板丝印**不一定对应**。本机（MSI H610M BOMBER DDR4 / MS-7D46）实测:
#
#   主板丝印 CPU_FAN1  ⟶  LHM 里显示为 **"System Fan #3"**
#   LHM 里那个叫 "CPU Fan" 的通道是**空的**：0 RPM 且 PWM 恒 0%
#   （15 s 单核负载：System Fan #3 转速 624→849、PWM 13%→27%，明显跟随 CPU 负载；
#     "CPU Fan" 通道 0 RPM / 0%，纹丝不动）
#
# ⇒ 默认从 "System Fan #3" 取 CPU 风扇转速，否则 CF 永远是 0、屏上显示 `0`。
# 换主板/换机器时用 `python sysmon.py --list-fans` 看实际通道名，再改这里
# 或命令行 `--cpu-fan "<通道名>"`。设为 "auto" 则回到旧行为：
# 只认 LHM 的 HardwareType=Cpu 那一路。
CPU_FAN_NAME = "System Fan #3"

# --- 2026-10-22 实机实测后补的三个判据（此前是扁平关键词匹配，会读错） ---
# 1) LHM 某些"温度"其实是阈值/余量, 不是实测值。实测踩到的:
#    INTEL SSDPEKKW256G8 / Temperatures / Critical Temperature = 79.0 °C  ← 临界阈值
#    INTEL SSDPEKKW256G8 / Temperatures / Warning Temperature  = 74.0 °C  ← 告警阈值
#    12th Gen i5-12490F  / Temperatures / CPU Core #1 Distance to TjMax = 58 °C ← 距上限余量
#    这些必须排除, 否则会把 79°C 当成 CPU 温度。
_LHM_NON_MEASURE = (
    "warning temperature", "critical temperature",
    "distance to tjmax", "resolution", "offset",
)

# 2) SuperIO 芯片(主板上的传感器控制器)之下的 "CPU"/"CPU Socket" 是主板测的
#    CPU 附近温度, 不是 CPU 内部核心温度 -> 只能当兜底。
_LHM_SUPERIO = ("nuvoton", "winbond", "fintek", "superio", "it86", "it87", "nct")

# 3) CPU 硬件名特征。实测: "12th Gen Intel Core i5-12490F" 命中 "core i";
#    而 "INTEL SSDPEKKW256G8" 虽然含 "intel" 却不命中 -> 不会被误判成 CPU。
#    这正是旧版用 "intel" 做关键词导致 SSD 阈值被读成 CPU 温度的原因。
_LHM_CPU_KEYS = ("core i", "core(tm)", "core ultra", "ryzen", "xeon",
                 "threadripper", "athlon", "pentium", "celeron", "epyc",
                 "processor")
_LHM_STORAGE = ("ssd", "hdd", "nvme", "disk", "drive", "hard drive")

# data.json 里叶子上一级一定是这些分组节点之一, 用于反推硬件名。
_LHM_GROUPS = ("temperatures", "fans", "controls", "load", "data",
               "powers", "clocks", "voltages", "throughput", "level",
               "factors", "timings")


def _lhm_hw_name(parts):
    """从叶子路径反推它属于哪个硬件。

    data.json 的层级形如 Computer / <机器名> / <硬件名> / <分组> / <叶子>。
    但根节点名可能为空(会被 join 过滤), 硬编码下标会整体错位 ——
    所以这里改为"找到已知分组节点, 它前面那一段就是硬件名"。
    """
    for i in range(len(parts) - 1, 0, -1):
        if parts[i].strip().lower() in _LHM_GROUPS:
            return parts[i - 1].strip().lower()
    return parts[-3].strip().lower() if len(parts) >= 3 else ""



def _lhm_walk(node, parents, leaves):
    """递归把 data.json 树摊平成 [(path_text, leaf_text, value_text), ...]"""
    text = (node.get("Text") or "").strip()
    value = (node.get("Value") or "").strip()
    children = node.get("Children") or []
    here = parents + [text]
    if children:
        for ch in children:
            _lhm_walk(ch, here, leaves)
    elif value:
        leaves.append((" / ".join(p for p in here if p), text, value))


def _num(s):
    """从 '45.0 °C' / '1234 RPM' / '62 %' 里抠出数字"""
    buf, out = [], []
    for ch in s:
        if ch.isdigit() or ch in ".-":
            buf.append(ch)
        elif buf:
            out.append("".join(buf))
            buf = []
    if buf:
        out.append("".join(buf))
    for tok in out:
        try:
            return float(tok)
        except ValueError:
            continue
    return None


def lhm_read(url=LHM_DEFAULT_URL, timeout=1.5, cpu_fan_name=None):
    """探测并解析 LHM 的 Web 服务器。返回 dict 或 None（未运行/解析不到）。

    2026-10-22 在 LHM 0.9.6 + i5-12490F + Arc A770 实机实测通过（222 个叶子）。
    层级：Computer / <机器名> / <硬件名> / <分组> / <叶子>。
    归属用"分组节点前一段即硬件名"反推，不硬编码下标（根节点名可能为空会错位）。

    实测踩到并已排除的两类假阳性（旧版按整条路径关键词匹配时会把它们读成 CPU 温度）：
      · INTEL SSDPEKKW256G8 / Temperatures / Critical Temperature = 79.0 °C  ← 临界阈值
      · CPU Core #1 / Distance to TjMax = 58.0 °C                            ← 距上限余量
    """
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "sysmon/1.0"})
        with urllib.request.urlopen(req, timeout=timeout) as r:
            raw = r.read()
        data = json.loads(raw.decode("utf-8", "replace"))
    except (urllib.error.URLError, OSError, ValueError):
        return None

    leaves = []
    try:
        _lhm_walk(data, [], leaves)
    except Exception:                                # noqa: BLE001
        return None
    if not leaves:
        return None

    out = {"cpu_temps": [], "cpu_core_max": None, "cpu_package": None,
           "cpu_fans": [], "all_fans": [], "mem_temps": [],
           "cpu_load": None, "cpu_fan_pwm_percent": None,
           "cpu_temp_avg": None, "leaf_count": len(leaves)}

    # 期望的 CPU 风扇通道名。None ⇒ 用模块级 CPU_FAN_NAME（本机实测值）；
    # 传 "auto"/"" ⇒ 回到旧行为（只认 HardwareType=Cpu 那一路）。
    pick = (CPU_FAN_NAME if cpu_fan_name is None else cpu_fan_name) or ""
    pick = pick.strip()
    if pick.lower() in ("auto", "none"):
        pick = ""
    out["pick_fan_pwm"] = None

    board_cpu_temps = []          # 主板 SuperIO 测的 CPU 附近温度（仅兜底）

    for path, leaf, value in leaves:
        parts = path.split(" / ")
        hw = _lhm_hw_name(parts)
        low_l, low_v = leaf.lower(), value.lower()
        num = _num(value)
        if num is None:
            continue

        # 结构化归属: 按硬件名判定, 不再用整条路径做关键词混判
        is_storage = any(k in hw for k in _LHM_STORAGE)
        is_superio = any(k in hw for k in _LHM_SUPERIO)
        is_cpu_hw = (not is_storage) and any(k in hw for k in _LHM_CPU_KEYS)
        is_mem_hw = any(k in hw for k in ("memory", "ddr", "dimm", "spd"))

        if "°c" in low_v:
            # 阈值 / 余量不是实测温度, 直接丢弃
            if any(k in low_l for k in _LHM_NON_MEASURE):
                continue
            item = {"name": path, "celsius": num, "hw": hw}
            if is_cpu_hw:
                out["cpu_temps"].append(item)
                if "core max" in low_l:
                    out["cpu_core_max"] = num
                elif "package" in low_l or "tctl" in low_l or "tdie" in low_l:
                    out["cpu_package"] = num
            elif is_superio and "cpu" in low_l:
                board_cpu_temps.append(item)
            elif is_mem_hw:
                out["mem_temps"].append(item)
            continue

        if "rpm" in low_v:
            item = {"name": path, "rpm": int(round(num))}
            out["all_fans"].append(item)
            if is_cpu_hw or (is_superio and "cpu" in low_l):
                out["cpu_fans"].append(item)
            continue

        # SuperIO 的 "Controls/xxx" 是 PWM 占空比(%), 用于交叉验证转速。
        # 注意: 名字叫 "CPU Fan" 的那一路未必真接着 CPU 风扇（见 CPU_FAN_NAME 的说明），
        # 所以除了旧的 "cpu" 关键词, 也把**指定通道**的 PWM 单独记下来。
        if "%" in low_v and is_superio:
            if "cpu" in low_l:
                out["cpu_fan_pwm_percent"] = num
            if pick and (low_l == pick.lower()):
                out["pick_fan_pwm"] = num
            continue

        if "%" in low_v and is_cpu_hw and "total" in low_l:
            out["cpu_load"] = num
            continue

    # ---- CPU 温度: 核心温度优先, 主板 CPU Socket 只当兜底 ----
    if out["cpu_core_max"] is not None:
        out["cpu_temp_max"], out["cpu_temp_pick"] = out["cpu_core_max"], "core-max"
    elif out["cpu_package"] is not None:
        out["cpu_temp_max"], out["cpu_temp_pick"] = out["cpu_package"], "package"
    elif out["cpu_temps"]:
        out["cpu_temp_max"] = round(max(t["celsius"] for t in out["cpu_temps"]), 1)
        out["cpu_temp_pick"] = "cpu-hw-max"
    elif board_cpu_temps:
        out["cpu_temp_max"] = round(max(t["celsius"] for t in board_cpu_temps), 1)
        out["cpu_temp_pick"] = "superio-cpu-socket"
    else:
        out["cpu_temp_max"], out["cpu_temp_pick"] = None, None

    # 平均温度: LHM 自带 "Core Average" 就用它, 否则按 CPU Core #n 求均值
    core_avg = next((t["celsius"] for t in out["cpu_temps"]
                     if "core average" in t["name"].lower()), None)
    cores = [t["celsius"] for t in out["cpu_temps"]
             if "cpu core #" in t["name"].lower()]
    if core_avg is not None:
        out["cpu_temp_avg"] = core_avg
    elif cores:
        out["cpu_temp_avg"] = round(sum(cores) / len(cores), 1)

    # ---- CPU 风扇: 按通道名取（默认 "System Fan #3" = 主板丝印 CPU_FAN1）----
    if pick:
        chosen = None
        for f in out["all_fans"]:
            if f["name"].rsplit(" / ", 1)[-1].strip().lower() == pick.lower():
                chosen = f
                break
        if chosen is not None:
            out["cpu_fans"] = [chosen]
            out["cpu_fan_rpm"] = chosen["rpm"]
            out["cpu_fan_pick"] = chosen["name"]
            out["cpu_fan_pwm_percent"] = out["pick_fan_pwm"]
            out["cpu_fan_confidence"] = "channel:%s" % pick
            if chosen["rpm"] == 0:
                out["cpu_fan_note"] = "指定通道 %s 读到 0 RPM（风扇停转或测速线未接）" % pick
        else:
            out["cpu_fan_rpm"] = None
            out["cpu_fan_pick"] = None
            out["cpu_fan_confidence"] = "channel-missing"
            names = [f["name"].rsplit(" / ", 1)[-1] for f in out["all_fans"]]
            out["cpu_fan_note"] = (
                "LHM 里找不到名为 '%s' 的风扇通道（现有: %s）。"
                "用 --list-fans 看实际通道名，或 --cpu-fan auto 回到旧行为。"
                % (pick, ", ".join(names) if names else "无"))
    else:
        # 旧行为：只认 LHM 的 HardwareType=Cpu 那一路。
        # 不 fallback 到 all_fans[0] —— 机箱风扇会冒充 CPU 风扇。
        out["cpu_fan_rpm"] = out["cpu_fans"][0]["rpm"] if out["cpu_fans"] else None
        if out["cpu_fan_rpm"] == 0 and out["cpu_fan_pwm_percent"]:
            out["cpu_fan_confidence"] = "pwm-only"
            out["cpu_fan_note"] = (
                "主板报 0 RPM 但 CPU 风扇 PWM 为 %d%%: 风扇在转, 转速线未回读"
                "（3pin 风扇无测速线, 或该接口未接转速）"
                % int(round(out["cpu_fan_pwm_percent"])))

    out["mem_temp_max"] = (round(max(t["celsius"] for t in out["mem_temps"]), 1)
                           if out["mem_temps"] else None)
    return out


# ---------------------------------------------------------------------------
# LHM 的 WMI 通路：root\LibreHardwareMonitor
#
# 这个 namespace 是 LHM 安装包用 mofcomp 注册的（元数据落在
#   C:\Windows\System32\wbem\Framework\root\LibreHardwareMonitor\
#       LibreHardwareMonitor_SN__Version_<ver>.mof / .cs
# ），注册内容就是两个类：
#   Hardware { Identifier, Name, HardwareType, Parent, InstanceId, ProcessId }
#   Sensor   { Identifier, Name, SensorType, Value, Min, Max, Parent,
#              Index, InstanceId, ProcessId }
# 关键点：MOF 里写的是 MSFT_DecoupledProvider / HostingModel="Decoupled:Com"，
# CLSID {54D8502C-527D-43f7-A506-A9DA075E229C} 实为
# System.Management.Instrumentation.ManagedCommonProvider（指向 mscoree.dll）。
# 也就是说 —— **类定义常驻，但实例由 LHM.exe 进程在运行时塞进来**。
# 进程没跑 ⇒ ExecQuery 返回 0 条（这不是"路径错了"，是"没人喂数据"）。
# ---------------------------------------------------------------------------
LHM_WMI_NS = r"root\LibreHardwareMonitor"


def _classify_lhm_sensors(hardwares, sensors):
    """把 WMI 的 Hardware/Sensor 列表归成与 lhm_read() 同构的 dict。

    hardwares: [{Identifier, HardwareType, Name}, ...]
    sensors:   [{Identifier, Name, SensorType, Value, Parent}, ...]
    用 Parent -> HardwareType 关联，比猜 Identifier 字符串前缀可靠。
    """
    hw_type = {}
    for h in hardwares:
        ident = (h.get("Identifier") or "").strip()
        if ident:
            hw_type[ident] = (h.get("HardwareType") or "").strip()

    out = {"cpu_temps": [], "cpu_package": None, "cpu_fans": [], "all_fans": [],
           "mem_temps": [], "cpu_load": None, "leaf_count": len(sensors),
           "source": "lhm-wmi", "hardwares": hardwares}

    for s in sensors:
        stype = (s.get("SensorType") or "").strip()
        name = (s.get("Name") or "").strip()
        parent = (s.get("Parent") or "").strip()
        ptype = hw_type.get(parent, "")
        val = s.get("Value")
        if val is None:
            continue
        try:
            val = float(val)
        except (TypeError, ValueError):
            continue

        if stype == "Temperature":
            item = {"name": ("%s / %s" % (ptype or parent, name)), "celsius": val}
            if ptype == "Memory":
                out["mem_temps"].append(item)
            elif ptype == "Cpu":
                out["cpu_temps"].append(item)
                if "package" in name.lower() or "core max" in name.lower():
                    out["cpu_package"] = val
        elif stype == "Fan":
            item = {"name": ("%s / %s" % (ptype or parent, name)),
                    "rpm": int(round(val))}
            out["all_fans"].append(item)
            if ptype == "Cpu":
                out["cpu_fans"].append(item)
        elif stype == "Load":
            low = name.lower()
            if ptype == "Cpu" and ("total" in low or "cpu total" in low):
                out["cpu_load"] = val

    if out["cpu_temps"]:
        out["cpu_temp_max"] = round(max(t["celsius"] for t in out["cpu_temps"]), 1)
        out["cpu_temp_avg"] = round(
            sum(t["celsius"] for t in out["cpu_temps"]) / len(out["cpu_temps"]), 1)
    else:
        out["cpu_temp_max"] = None
        out["cpu_temp_avg"] = None

    # CPU 风扇：只认 HardwareType=Cpu 那一组。不再 fallback 到 all_fans[0] ——
    # 主板上的 Fan#1..n 哪个接 CPU 只有装机的人知道，拿第一个冒充会给出错误结论。
    if out["cpu_fans"]:
        out["cpu_fan_rpm"] = out["cpu_fans"][0]["rpm"]
        out["cpu_fan_confidence"] = "hardware-type=Cpu"
    else:
        out["cpu_fan_rpm"] = None
        out["cpu_fan_confidence"] = None

    out["mem_temp_max"] = (round(max(t["celsius"] for t in out["mem_temps"]), 1)
                           if out["mem_temps"] else None)
    return out


def _lhm_wmi_via_com(timeout_hint=None):
    """走 win32com（pywin32）。快，~10ms 级。未安装 pywin32 时抛 ImportError。"""
    import pythoncom
    import win32com.client

    pythoncom.CoInitialize()
    svc = None
    try:
        svc = win32com.client.GetObject("winmgmts:\\\\.\\" + LHM_WMI_NS)
        hardwares = []
        for h in svc.ExecQuery("SELECT Identifier, HardwareType, Name FROM Hardware"):
            hardwares.append({"Identifier": h.Identifier,
                              "HardwareType": h.HardwareType,
                              "Name": h.Name})
        sensors = []
        for s in svc.ExecQuery("SELECT Identifier, Name, SensorType, Value, "
                               "Parent, Min, Max FROM Sensor"):
            sensors.append({"Identifier": s.Identifier, "Name": s.Name,
                            "SensorType": s.SensorType, "Value": s.Value,
                            "Parent": s.Parent, "Min": s.Min, "Max": s.Max})
        # 先显式放掉 COM 引用再 CoUninitialize，否则会报
        # "Win32 exception occurred releasing IUnknown"（释放顺序颠倒）
        svc = None
        return hardwares, sensors
    finally:
        svc = None
        pythoncom.CoUninitialize()


def _lhm_wmi_via_powershell():
    """零依赖兜底：调 PowerShell 查一次，返回 JSON。约 300-500ms。"""
    import subprocess
    ps = (
        "$ErrorActionPreference='Stop';"
        "$h=@(Get-CimInstance -Namespace root/LibreHardwareMonitor -ClassName Hardware"
        " -ErrorAction Stop | Select-Object Identifier,HardwareType,Name);"
        "$s=@(Get-CimInstance -Namespace root/LibreHardwareMonitor -ClassName Sensor"
        " -ErrorAction Stop | Select-Object Identifier,Name,SensorType,Value,Parent,Min,Max);"
        "ConvertTo-Json -Compress -Depth 4 -InputObject @{h=$h;s=$s}"
    )
    r = subprocess.run(
        ["powershell", "-NoProfile", "-NonInteractive", "-Command", ps],
        capture_output=True, timeout=15)
    if r.returncode != 0:
        raise RuntimeError("PowerShell 查询失败: %s"
                           % r.stderr.decode("utf-8", "replace")[:200])
    txt = r.stdout.decode("utf-8", "replace").strip()
    if not txt:
        raise RuntimeError("PowerShell 无输出")
    data = json.loads(txt)
    h = data.get("h") or []
    s = data.get("s") or []
    if isinstance(h, dict):
        h = [h]
    if isinstance(s, dict):
        s = [s]
    return h, s


def lhm_wmi_read(prefer="auto"):
    """读 LHM 的 WMI namespace。

    返回 dict（结构同 lhm_read）或 None。
    None 有两种含义，用 lhm_wmi_status() 区分：
      a) namespace 不存在（LHM 从没装过）
      b) namespace 在但实例为 0（LHM 装了但进程没跑）← 本机就是这种
    """
    if prefer in ("auto", "com"):
        try:
            hw, sn = _lhm_wmi_via_com()
            if not sn:
                return None
            return _classify_lhm_sensors(hw, sn)
        except ImportError:
            if prefer == "com":
                raise
        except Exception:                                    # noqa: BLE001
            if prefer == "com":
                raise
    try:
        hw, sn = _lhm_wmi_via_powershell()
    except Exception:                                        # noqa: BLE001
        return None
    if not sn:
        return None
    return _classify_lhm_sensors(hw, sn)


def lhm_process_running():
    """LHM.exe 是否在运行。返回 (bool, pid 或 None)。

    ⚠ 必须单独判断：WMI 的 Sensor=0 有**两种**完全不同的原因 ——
       ① 进程没跑（decoupled provider 没数据源）
       ② 进程在跑，但该版本根本不发布 WMI（0.9.6 起已移除 WMI，实测 exe 内
          无 Instrumentation/System.Management 任何字符串；残留的 namespace 是 0.9.4 的）
    以前把两种情况都报成"没在运行"，会误导排查方向。
    """
    if not HAVE_PSUTIL:
        return (None, None)
    try:
        for p in psutil.process_iter(["pid", "name"]):
            n = (p.info.get("name") or "").lower()
            if "librehardwaremonitor" in n:
                return (True, p.info.get("pid"))
    except Exception:                                        # noqa: BLE001
        return (None, None)
    return (False, None)


def lhm_wmi_status():
    """报告 WMI 通路状态，用于 --providers 和错误提示。"""
    st = {"namespace": False, "sensor_count": None, "hardware_count": None,
          "backend": None, "error": None}
    try:
        hw, sn = _lhm_wmi_via_com()
        st.update(namespace=True, sensor_count=len(sn),
                  hardware_count=len(hw), backend="pywin32")
        return st
    except ImportError:
        st["error"] = "pywin32 未安装（将用 PowerShell 兜底）"
    except Exception as e:                                   # noqa: BLE001
        st["error"] = str(e)[:200]
    try:
        hw, sn = _lhm_wmi_via_powershell()
        st.update(namespace=True, sensor_count=len(sn),
                  hardware_count=len(hw), backend="powershell")
    except Exception as e:                                   # noqa: BLE001
        st["error"] = str(e)[:200]
    return st


# ============================================================================
# 三、CPU / 内存（psutil）
# ============================================================================
def cpu_model_name():
    if os.name == "nt":
        try:
            import winreg
            k = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                               r"HARDWARE\DESCRIPTION\System\CentralProcessor\0")
            try:
                v, _ = winreg.QueryValueEx(k, "ProcessorNameString")
            finally:
                winreg.CloseKey(k)
            return " ".join(str(v).split())
        except OSError:
            pass
    return platform.processor() or platform.machine() or "?"


def read_cpu(warm=False):
    out = {"name": cpu_model_name()}
    if not HAVE_PSUTIL:
        out["available"] = False
        out["note"] = "psutil 未安装： pip install psutil"
        return out
    out["available"] = True
    out["cores_physical"] = psutil.cpu_count(logical=False)
    out["cores_logical"] = psutil.cpu_count(logical=True)

    # 第一次调用只建基线、返回 0.0，需要预热；loop 模式下由上一次调用充当基线
    pct = psutil.cpu_percent(interval=0.3 if warm else None)
    out["usage_percent"] = round(pct, 1)
    try:
        out["per_core_percent"] = [round(x, 1) for x in
                                   psutil.cpu_percent(interval=None, percpu=True)]
    except Exception:                                # noqa: BLE001
        out["per_core_percent"] = []
    try:
        f = psutil.cpu_freq()
        out["freq_current_mhz"] = int(round(f.current)) if f and f.current else None
        out["freq_max_mhz"] = int(round(f.max)) if f and f.max else None
    except Exception:                                # noqa: BLE001
        out["freq_current_mhz"] = None
        out["freq_max_mhz"] = None
    return out


def read_ram():
    out = {"available": False}
    if not HAVE_PSUTIL:
        out["note"] = "psutil 未安装"
        return out
    vm = psutil.virtual_memory()
    out.update({
        "available": True,
        "total": vm.total,
        "used": vm.used,
        "free": vm.available,
        "percent": round(vm.percent, 1),
    })
    try:
        sm = psutil.swap_memory()
        out["swap_percent"] = round(sm.percent, 1)
    except Exception:                                # noqa: BLE001
        pass
    return out


# ============================================================================
# 三·B、GPU 使用率（DXGI 拿 LUID + PDH 的 \GPU Engine 计数器）
#
# 为什么不用 IGCL：IGCL 1.1 是"控制库"，只有温度/风扇/显存这类遥测，
#   ctl_device_adapter_properties_t 里也**没有 luid 字段**（grep 全头文件为 0 命中），
#   所以它给不出"这块 GPU 有多忙"。
# Windows 侧的 GPU 利用率只有一个可靠来源：性能计数器 \GPU Engine(*)\Utilization Percentage，
#   每个实例形如
#       pid_1234_luid_0x00000000_0x00014890_phys_0_eng_0_engtype_3D
#   即"某进程在某适配器的某引擎上的占用"，单位是百分比。
#
# 口径（与任务管理器一致）：
#   1) 只保留目标适配器（A770）的 LUID —— 本机还有一个
#      Microsoft Basic Render Driver (vendor 0x1414)，它是软件光栅化器，
#      混进来会把它那点占用算成 GPU 的；
#   2) 同一 engtype 跨进程求和（多进程分时共用同一个 3D 引擎）；
#   3) 各 engtype 之间取**最大值**而不是求和 —— 3D / Copy / VideoDecode 是并行
#      引擎，直接相加会超过 100%。任务管理器显示的"GPU 利用率"就是这个 max。
# ============================================================================
# ctypes 的结构体必须放在**模块级**：类体内再嵌套 struct 并互相引用时，
# 内层 class body 看不到外层 class 的局部名字（NameError: LUID 未定义，已踩）。
class _DxgiLuid(C.Structure):
    _fields_ = [("LowPart", W.DWORD), ("HighPart", W.LONG)]


class _DxgiAdapterDesc1(C.Structure):
    # 真实大小 312 字节（WCHAR[128]=256 + 4×DWORD=16 + 3×SIZE_T=24 + LUID=8 + UINT=4，含 padding）
    _fields_ = [("Description", W.WCHAR * 128),
                ("VendorId", W.DWORD), ("DeviceId", W.DWORD),
                ("SubSysId", W.DWORD), ("Revision", W.DWORD),
                ("DedicatedVideoMemory", C.c_size_t),
                ("DedicatedSystemMemory", C.c_size_t),
                ("SharedSystemMemory", C.c_size_t),
                ("AdapterLuid", _DxgiLuid),
                ("Flags", W.UINT)]


class DxgiAdapters(object):
    """DXGI 枚举适配器，只为拿 LUID 与显存大小做交叉校验。

    IGCL 的显存 17179869184 (16GiB) 与 DXGI 的 DedicatedVideoMemory 应该一致，
    对得上就说明"IGCL 第 0 号设备 == DXGI 第 0 号适配器"，可以放心用 LUID 关联。"""

    available = None
    last_error = None
    LUID = _DxgiLuid
    DXGI_ADAPTER_DESC1 = _DxgiAdapterDesc1

    # IID_IDXGIFactory1
    _IID_FACTORY1 = (0x770aae78, 0xf26f, 0x4dba, (0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87))

    @staticmethod
    def _vcall(ptr, index, restype, *argtypes):
        """按 COM vtable 下标取函数指针并调用（x64 只有一个调用约定，安全）。"""
        vtbl = C.cast(ptr, C.POINTER(C.c_void_p))[0]
        fn = C.cast(vtbl + index * C.sizeof(C.c_void_p), C.POINTER(C.c_void_p))[0]
        return C.WINFUNCTYPE(restype, C.c_void_p, *argtypes)(fn)

    @classmethod
    def list(cls):
        if os.name != "nt":
            cls.last_error = "非 Windows"
            return []
        try:
            dxgi = C.WinDLL("dxgi.dll")

            class GUID(C.Structure):
                _fields_ = [("d1", W.DWORD), ("d2", C.c_ushort), ("d3", C.c_ushort),
                            ("d4", C.c_ubyte * 8)]

            iid = GUID(*cls._IID_FACTORY1)
            factory = C.c_void_p()
            rc = dxgi.CreateDXGIFactory1(C.byref(iid), C.byref(factory))
            if rc != 0 or not factory.value:
                cls.last_error = "CreateDXGIFactory1 rc=0x%08X" % (rc & 0xFFFFFFFF)
                return []
            out, i = [], 0
            enum_fn = cls._vcall(factory, 12, C.c_long, W.UINT, C.POINTER(C.c_void_p))
            while True:
                adapter = C.c_void_p()
                if enum_fn(factory, i, C.byref(adapter)) != 0 or not adapter.value:
                    break
                desc = cls.DXGI_ADAPTER_DESC1()
                desc_fn = cls._vcall(adapter, 10, C.c_long,
                                     C.POINTER(cls.DXGI_ADAPTER_DESC1))
                if desc_fn(adapter, C.byref(desc)) == 0:
                    out.append({
                        "index": i,
                        "name": desc.Description,
                        "vendor_id": desc.VendorId,
                        "device_id": desc.DeviceId,
                        # LUID 在计数器实例名里是 "0x高32_0x低32"，注意顺序
                        "luid": (desc.AdapterLuid.HighPart & 0xFFFFFFFF,
                                 desc.AdapterLuid.LowPart),
                        "vram": desc.DedicatedVideoMemory,
                        # Flags & 2 = DXGI_ADAPTER_FLAG_SOFTWARE（Basic Render Driver）
                        "software": bool(desc.Flags & 2),
                    })
                cls._vcall(adapter, 2, C.c_ulong)(adapter)      # Release
                i += 1
            cls._vcall(factory, 2, C.c_ulong)(factory)
            cls.available = True
            return out
        except Exception as e:                           # noqa: BLE001
            cls.last_error = "%s: %s" % (type(e).__name__, e)
            cls.available = False
            return []


# 实例名正则。engtype 可能是空（引擎还没被调度过，值为 0），也可能是 VideoDecode 等
_GPU_ENGINE_RE = re.compile(
    r"^pid_(\d+)_luid_(0x[0-9a-fA-F]+)_(0x[0-9a-fA-F]+)_phys_(\d+)_eng_(\d+)_engtype_(\w*)$")


class GpuUsage(object):
    r"""\GPU Engine 汇总。目标 LUID 由 DXGI + IGCL 显存交叉校验后确定。

    时间间隔是硬约束：这类"速率型"计数器算的是两次 Collect 之间的增量，
    间隔太短（<几十 ms）分母趋近 0，读数会乱跳甚至恒为 0。
    所以这里强制两次读取至少隔 min_interval 秒。"""

    COUNTER = r"\GPU Engine(*)\Utilization Percentage"

    def __init__(self, luid=None, min_interval=0.2):
        self.counter = PdhCounter(self.COUNTER)   # 构造时已 Collect 一次 = 基线
        self.luid = tuple(luid) if luid else None
        self.min_interval = min_interval
        self._last = time.monotonic()
        self.engines = {}          # 最近一次各引擎占比（已 clamp）
        self.total = None          # 最近一次总利用率（各引擎取 max）
        self.instances = 0
        self.dropped = 0           # 非目标 LUID 的实例数（Basic Render Driver 等）
        self.gap_ms = None         # 两次采样的实际间隔，调试用

    def read(self):
        gap = time.monotonic() - self._last
        if gap < self.min_interval:
            time.sleep(self.min_interval - gap)
            gap = self.min_interval
        rows = self.counter.read_array()
        self.gap_ms = int(round((time.monotonic() - self._last) * 1000))
        self._last = time.monotonic()
        self.instances = len(rows)
        per_eng = {}
        dropped = 0
        for name, val in rows:
            m = _GPU_ENGINE_RE.match(name)
            if not m:
                continue
            luid = (int(m.group(2), 16), int(m.group(3), 16))
            if self.luid is not None and luid != self.luid:
                dropped += 1
                continue
            et = m.group(6) or "(idle)"
            per_eng[et] = per_eng.get(et, 0.0) + (val or 0.0)
        self.dropped = dropped
        # 单个引擎占比不可能 >100%，跨进程相加理论上也不该超；超了就截断
        self.engines = {k: min(100.0, round(v, 1)) for k, v in per_eng.items()}
        self.total = max(self.engines.values()) if self.engines else 0.0
        return self.total

    def close(self):
        self.counter.close()


# 同一个 LUID 复用同一个 PDH query：loop 模式下第二次起就不用再等 min_interval
_GPU_USAGE_CACHE = {}


def get_gpu_usage(luid):
    key = tuple(luid)
    u = _GPU_USAGE_CACHE.get(key)
    if u is None:
        u = GpuUsage(luid)
        _GPU_USAGE_CACHE[key] = u
    return u


# ============================================================================
# 四、GPU（IGCL，复用 igcl_probe）
# ============================================================================
def _pick_dxgi_adapter(vram_total):
    """在 DXGI 适配器里找出与 IGCL 第 0 号设备对应的那块。

    判据顺序：
      1) 排除 DXGI_ADAPTER_FLAG_SOFTWARE（Basic Render Driver 是纯软件光栅化，
         显存 0，混进来会让 GPU 利用率失真）；
      2) 显存大小与 IGCL 报的一致 → 就是它（本机 16GiB 独一档）；
      3) 只剩一块时直接用它；
      4) 实在对不上就退到第一块非软件适配器，并在 note 里说明。"""
    adapters = [a for a in DxgiAdapters.list() if not a["software"]]
    if not adapters:
        return None, "DXGI 未枚举到非软件适配器（%s）" % (DxgiAdapters.last_error or "未知")
    if vram_total:
        for a in adapters:
            if a["vram"] == vram_total:
                return a, None
    if len(adapters) == 1:
        return adapters[0], None
    return adapters[0], "DXGI 显存(%s)与 IGCL(%s)对不上，按第一块适配器处理" % (
        humansize(adapters[0]["vram"]), humansize(vram_total))


def read_gpu(igcl, with_usage=True):
    """把 igcl_probe 的输出压成一行关心的字段"""
    infos = igcl.read_all()
    if not infos:
        return {"present": False, "note": "未枚举到设备"}
    d = infos[0]["data"]
    out = {"present": True, "device_index": 0}

    mems = [m for m in d.get("memory", []) if m.get("location") == 1] \
        or d.get("memory", [])
    if mems:
        m = mems[0]
        out.update({
            "vram_type": m.get("type_name"),
            "vram_total": m.get("size"),
            "vram_free": m.get("free"),
            "vram_used": m.get("used"),
            "vram_percent": m.get("used_percent"),
        })
    else:
        out["note"] = "无 memory 模块"

    temps = d.get("temperatures", [])
    out["temps"] = [{"type_name": t["type_name"], "celsius": t["celsius"],
                     "max": t.get("max_temperature")} for t in temps]
    # 按 IGCL 的传感器类型取：1=GPU(max) 2=MEMORY(max) 0=GLOBAL(max)
    for t in temps:
        if t["celsius"] is None:
            continue
        if t["type"] == 1 and out.get("temp_gpu_c") is None:
            out["temp_gpu_c"] = round(t["celsius"], 1)
        elif t["type"] == 2 and out.get("temp_vram_c") is None:
            out["temp_vram_c"] = round(t["celsius"], 1)
        elif t["type"] == 0 and out.get("temp_global_c") is None:
            out["temp_global_c"] = round(t["celsius"], 1)
    if out.get("temp_gpu_c") is None:
        cand = [t["celsius"] for t in temps if t["celsius"] is not None]
        out["temp_gpu_c"] = round(max(cand), 1) if cand else None

    fans = d.get("fans", [])
    if fans:
        f = fans[0]
        out["fan_rpm"] = f.get("rpm")
        out["fan_percent"] = f.get("percent")   # A770 实测为 None（驱动不支持）
        out["fan_max_rpm"] = f.get("max_rpm")
    else:
        out["fan_rpm"] = None
        out["fan_percent"] = None
    if d.get("errors"):
        out["errors"] = d["errors"]

    # ---- GPU 使用率（DXGI 定 LUID → PDH \GPU Engine 汇总）----
    if with_usage:
        if pdh is None:
            out["usage_note"] = "PDH 不可用（非 Windows？）"
        else:
            adapter, warn = _pick_dxgi_adapter(out.get("vram_total"))
            if warn:
                out["usage_note"] = warn
            if adapter:
                out["usage_target"] = adapter["name"]
                out["usage_luid"] = adapter["luid"]
                try:
                    u = get_gpu_usage(adapter["luid"])
                    out["usage_percent"] = round(u.read(), 1)
                    out["usage_engines"] = dict(u.engines)
                    out["usage_instances"] = u.instances
                    out["usage_dropped"] = u.dropped
                    out["usage_gap_ms"] = u.gap_ms
                except Exception as e:               # noqa: BLE001
                    out["usage_note"] = "GPU 利用率读取失败: %s: %s" % (
                        type(e).__name__, e)
    return out


# ============================================================================
# 五、汇总
# ============================================================================
def collect(igcl, lhm_url=LHM_DEFAULT_URL, warm=False, no_gpu=False,
            allow_acpi=True, cpu_fan_name=None):
    res = {"ts": time.strftime("%Y-%m-%dT%H:%M:%S"), "providers": {}}

    cpu = read_cpu(warm=warm)
    ram = read_ram()

    # ---- CPU 温度 / 风扇：LHM 优先（WMI 优先，HTTP 次之），PDH 兜底 ----
    # WMI 优先的理由：SensorType 是结构化枚举（"Temperature"/"Fan"/"Load"），
    # 且有 Parent->HardwareType 可精确归属到 CPU/Memory；
    # HTTP 的 data.json 只有文本路径，靠关键词猜，不如 WMI 稳。
    # 但两者都要求 LHM.exe 进程在跑（WMI 是 decoupled provider，HTTP 是内置服务器）。
    lhm, lhm_via = None, None
    want_pick = (CPU_FAN_NAME if cpu_fan_name is None else cpu_fan_name) or ""
    want_pick = want_pick.strip()
    use_pick = want_pick.lower() not in ("", "auto", "none")

    if use_pick:
        # 指定了风扇通道 ⇒ 只能走 HTTP（WMI 不暴露通道名）。
        lhm_http = lhm_read(lhm_url, cpu_fan_name=want_pick)
        if lhm_http:
            lhm, lhm_via = lhm_http, "lhm-http"
    if lhm is None:
        lhm_wmi = lhm_wmi_read()
        if lhm_wmi:
            lhm, lhm_via = lhm_wmi, "lhm-wmi"
    if lhm is None:
        lhm_http = lhm_read(lhm_url, cpu_fan_name=want_pick)
        if lhm_http:
            lhm, lhm_via = lhm_http, "lhm-http"

    cpu_temp, cpu_temp_src, cpu_temp_trusted = None, None, False
    cpu_fan, cpu_fan_src = None, None
    mem_temp, mem_temp_src = None, None

    if lhm:
        res["providers"]["lhm"] = lhm_via
        if lhm.get("cpu_temp_max") is not None:
            cpu_temp = lhm["cpu_temp_max"]
            cpu_temp_src = lhm_via
            cpu_temp_trusted = True
        if lhm.get("cpu_fan_rpm") is not None:
            cpu_fan = lhm["cpu_fan_rpm"]
            cpu_fan_src = lhm_via
        if lhm.get("mem_temp_max") is not None:
            mem_temp = lhm["mem_temp_max"]
            mem_temp_src = lhm_via
        cpu["lhm_detail"] = {
            "cpu_temps": lhm.get("cpu_temps", [])[:8],
            "cpu_package_c": lhm.get("cpu_package"),
            "fans": lhm.get("all_fans", [])[:6],
            "leaf_count": lhm.get("leaf_count"),
            "source": lhm_via,
        }
        if lhm.get("cpu_fan_confidence"):
            cpu["fan_confidence"] = lhm["cpu_fan_confidence"]
        # 记录这次温度是从哪个传感器取的, 便于核对（core-max / package / 兜底）
        if lhm.get("cpu_temp_pick"):
            cpu["temperature_pick"] = lhm["cpu_temp_pick"]
    else:
        res["providers"]["lhm"] = "not-running"

    if cpu_temp is None and allow_acpi:
        acpi = AcpiTemperature()
        c, inst = acpi.read()
        if c is not None:
            cpu_temp = c
            cpu_temp_src = "pdh-acpi"
            cpu_temp_trusted = False
            res["providers"]["acpi_instance"] = inst
            if AcpiTemperature.last_error:
                res["providers"]["acpi_error"] = AcpiTemperature.last_error

    cpu["temperature_c"] = cpu_temp
    cpu["temperature_source"] = cpu_temp_src
    cpu["temperature_trusted"] = cpu_temp_trusted
    if cpu_temp_src == "pdh-acpi":
        cpu["temperature_note"] = (
            "ACPI 热区值，台式机上多为固件占位值（实测满载也不变化），仅供参考；"
            "在 LHM 里开 Options > Server > Run 后自动改用真实核心温度")
    elif cpu_temp is None:
        cpu["temperature_note"] = (
            "未取到。Windows 用户态无法读核心温度（MSR 需内核驱动）；"
            "在 LHM 里开 Options > Server > Run（默认 8085）后自动接入")

    cpu["fan_rpm"] = cpu_fan
    cpu["fan_source"] = cpu_fan_src
    if cpu_fan is None:
        cpu["fan_note"] = ("仅 LibreHardwareMonitor 能读（需其驱动访问 SuperIO/EC 芯片）；"
                           "Win32_Fan 只有 DeviceID，没有转速")
    elif lhm and lhm.get("cpu_fan_note"):
        # 0 RPM 但 PWM 有输出 -> 说明风扇在转, 只是转速线没回读, 不能当成"风扇停转"
        cpu["fan_note"] = lhm["cpu_fan_note"]

    ram["temperature_c"] = mem_temp
    ram["temperature_source"] = mem_temp_src
    if mem_temp is None:
        ram["temperature_note"] = (
            "本机 2×16G DDR4-3200 UDIMM 无温度传感器（消费级 DDR4 普遍不带 TS；"
            "PDH 的 135 个计数器对象中亦无 DIMM 温度项）。DDR5 的 SPD Hub 才有，"
            "换 DDR5 且装 LHM 后本脚本会自动读出，无需改代码")

    res["cpu"] = cpu
    res["ram"] = ram

    # ---- GPU ----
    if no_gpu:
        res["gpu"] = {"present": False, "skipped": True}
        res["providers"]["gpu"] = "skipped"
    elif igcl is None:
        res["gpu"] = {"present": False,
                      "note": "IGCL 不可用（见 --no-gpu / 驱动是否安装 ControlLib.dll）"}
        res["providers"]["gpu"] = "unavailable"
    else:
        try:
            res["gpu"] = read_gpu(igcl)
            res["providers"]["gpu"] = "igcl"
        except IgclError as e:
            res["gpu"] = {"present": False, "note": str(e)}
            res["providers"]["gpu"] = "error"

    res["providers"]["cpu_temp"] = cpu_temp_src
    res["providers"]["cpu_fan"] = cpu_fan_src
    res["providers"]["mem_temp"] = mem_temp_src
    return res


# ---------------------------------------------------------------------------
# 输出
# ---------------------------------------------------------------------------
def humansize(n):
    if n is None:
        return "?"
    f, units = float(n), ["B", "KiB", "MiB", "GiB", "TiB"]
    for i, u in enumerate(units):
        if f < 1024.0 or i == len(units) - 1:
            return ("%d B" % n) if i == 0 else ("%.2f %s" % (f, u))
        f /= 1024.0


def _fmt(v, unit="", nd=1, none="N/A"):
    if v is None:
        return none
    if isinstance(v, float):
        return ("%." + str(nd) + "f%s") % (v, unit)
    return "%s%s" % (v, unit)


def print_report(r):
    cpu, ram, gpu = r["cpu"], r["ram"], r["gpu"]
    print("=" * 62)
    print(" CPU")
    print("=" * 62)
    print("  型号        %s" % cpu.get("name", "?"))
    print("  核心/线程   %s / %s" % (cpu.get("cores_physical", "?"),
                                     cpu.get("cores_logical", "?")))
    print("  使用率      %s" % _fmt(cpu.get("usage_percent"), " %"))
    cur, mx = cpu.get("freq_current_mhz"), cpu.get("freq_max_mhz")
    if cur and mx:
        freq_s = "%d MHz (max %d MHz)" % (cur, mx)
    elif cur:
        freq_s = "%d MHz" % cur
    elif mx:
        freq_s = "max %d MHz" % mx
    else:
        freq_s = "N/A"
    print("  频率        %s" % freq_s)
    src = cpu.get("temperature_source")
    # temperature_pick 是英文键名, 这里翻成中文, 让人知道这个数字取自哪个传感器
    pick_label = {
        "core-max": "处理器核心最高温",
        "package": "处理器封装温度",
        "cpu-hw-max": "处理器节点内最高温",
        "superio-cpu-socket": "主板 CPU 附近温度(兜底)",
    }.get(cpu.get("temperature_pick"), "")
    print("  温度        %s   [%s%s%s]"
          % (_fmt(cpu.get("temperature_c"), " C"),
             src or "无",
             (" · " + pick_label) if pick_label else "",
             "" if cpu.get("temperature_trusted") else " · 不可信"))
    print("  风扇        %s   [%s]"
          % (_fmt(cpu.get("fan_rpm"), " RPM", none="取不到"),
             cpu.get("fan_source") or "原生不可读"))
    if cpu.get("temperature_note"):
        print("  └ %s" % cpu["temperature_note"])
    if cpu.get("fan_note"):
        print("  └ %s" % cpu["fan_note"])
    if cpu.get("per_core_percent"):
        print("  各核        %s" % ", ".join(str(x) for x in cpu["per_core_percent"]))

    print("=" * 62)
    print(" 内存 (系统 RAM)")
    print("=" * 62)
    print("  占用        %s / %s  (%s)"
          % (humansize(ram.get("used")), humansize(ram.get("total")),
             _fmt(ram.get("percent"), " %")))
    print("  可用        %s" % humansize(ram.get("free")))
    if ram.get("swap_percent") is not None:
        print("  交换分区    %s" % _fmt(ram.get("swap_percent"), " %"))
    print("  温度        %s" % (_fmt(ram.get("temperature_c"), " C", none="无传感器")
                                 if ram.get("temperature_c") is not None
                                 else "无传感器（本机 DDR4 无 TS）"))

    print("=" * 62)
    print(" GPU (Intel Arc, IGCL)")
    print("=" * 62)
    if not gpu.get("present"):
        print("  不可用：%s" % gpu.get("note", "未知"))
    else:
        print("  显存        %s / %s  (%s)  [%s]"
              % (humansize(gpu.get("vram_used")), humansize(gpu.get("vram_total")),
                 _fmt(gpu.get("vram_percent"), " %", none="N/A"),
                 gpu.get("vram_type") or "?"))
        print("  温度        GPU %s / 全局 %s / 显存 %s"
              % (_fmt(gpu.get("temp_gpu_c"), " C"),
                 _fmt(gpu.get("temp_global_c"), " C"),
                 _fmt(gpu.get("temp_vram_c"), " C")))
        print("  使用率      %s   [%s]"
              % (_fmt(gpu.get("usage_percent"), " %", none="取不到"),
                 gpu.get("usage_target") or "PDH \\GPU Engine"))
        if gpu.get("usage_engines"):
            top = sorted(gpu["usage_engines"].items(), key=lambda kv: -kv[1])
            print("  └ 各引擎    %s"
                  % ", ".join("%s=%g%%" % (k, v) for k, v in top))
            print("  └ 实例 %s 个，舍弃非目标适配器 %s 个，采样间隔 %s ms"
                  % (gpu.get("usage_instances", "?"), gpu.get("usage_dropped", "?"),
                     gpu.get("usage_gap_ms", "?")))
        if gpu.get("usage_note"):
            print("  └ %s" % gpu["usage_note"])
        print("  风扇        %s / 百分比 %s"
              % (_fmt(gpu.get("fan_rpm"), " RPM", none="N/A"),
                 _fmt(gpu.get("fan_percent"), " %", none="驱动不支持")))

    print("-" * 62)
    print(" 数据源: " + ", ".join("%s=%s" % (k, v)
                                 for k, v in r["providers"].items() if v))
    if cpu.get("temperature_source") == "pdh-acpi":
        print(" 提示: CPU 温度当前是 ACPI 占位值，装 LHM 后可拿到真实核心温度。")


def to_line(r, allow_acpi=False):
    """单行紧凑格式，给串口 -> 显示端（3.5" 屏 / 原 OLED 同一套）。

    字段（顺序固定，某项取不到就整项省略，绝不用 0/-1 冒充）：
      CT = CPU 温度 °C        CU = CPU 使用率 %      CF = CPU 风扇 RPM
      GT = GPU 核心温度 °C    GU = GPU 使用率 %      GF = GPU 风扇 RPM
      MU = 内存(RAM) 使用率 %

    示例: CT:48,CU:9,CF:1050,GT:54,GU:3,GF:1047,MU:74

    为什么固件必须按 "key:value" 解析、不能按位置取：
      任何一项取不到（例如没开 LHM 就没有 CF）都会让字段数变化，
      位置解析会把后面的值整体错位；key 解析天然容错。
      也正因为如此，字段名一旦定下就别改，改了要同步改固件。

    注意 CF:0 / GF:0 是真实读数（Intel 显卡有 Zero-RPM 停转特性，
    低负载时风扇真的停转并报 0），与"字段缺失"是两件事，固件要区别显示。
    """
    cpu, ram, gpu = r["cpu"], r["ram"], r["gpu"]
    parts = []

    # ---- CPU ----
    ct = cpu.get("temperature_c")
    # 默认只输出"可信"温度；ACPI 占位值需要 --allow-acpi-temp 才带出去
    if ct is not None and (cpu.get("temperature_trusted") or allow_acpi):
        parts.append("CT:%d" % round(ct))
    if cpu.get("usage_percent") is not None:
        parts.append("CU:%d" % round(cpu["usage_percent"]))
    if cpu.get("fan_rpm") is not None:
        parts.append("CF:%d" % round(cpu["fan_rpm"]))

    # ---- GPU ----
    if gpu.get("present"):
        if gpu.get("temp_gpu_c") is not None:
            parts.append("GT:%d" % round(gpu["temp_gpu_c"]))
        if gpu.get("usage_percent") is not None:
            parts.append("GU:%d" % round(gpu["usage_percent"]))
        if gpu.get("fan_rpm") is not None:
            parts.append("GF:%d" % round(gpu["fan_rpm"]))

    # ---- RAM ----
    if ram.get("percent") is not None:
        parts.append("MU:%d" % round(ram["percent"]))

    return ",".join(parts)


def print_providers():
    print("=== 数据源探测 ===")
    print("psutil              : %s" % ("OK %s" % psutil.__version__
                                       if HAVE_PSUTIL else "缺失 (pip install psutil)"))
    print("igcl_probe 模块     : %s" % ("OK" if HAVE_IGCL_MODULE else "导入失败"))
    print("pdh.dll             : %s" % ("OK" if pdh is not None else "不可用"))
    a = AcpiTemperature()
    if a.probe():
        c, inst = a.read()
        print("ACPI 热区温度       : %s (实例 %s) —— 台式机多为占位值, 不可信"
              % (_fmt(c, " C"), inst))
    else:
        print("ACPI 热区温度       : 不可用 (%s)" % AcpiTemperature.last_error)
    st = lhm_wmi_status()
    running, pid = lhm_process_running()
    if st["namespace"] and st["sensor_count"]:
        print("LHM WMI  (# root\\LibreHardwareMonitor): OK  Sensor=%d Hardware=%d  后端=%s"
              % (st["sensor_count"], st["hardware_count"], st["backend"]))
    elif st["namespace"]:
        if running:
            print("LHM WMI  (# root\\LibreHardwareMonitor): namespace 已注册, 但 Sensor=0")
            print("         └ LHM 进程在跑 (PID %s), 说明**该版本已移除 WMI 发布**"
                  "（0.9.6 起不再有；namespace 是旧版 0.9.4 的残留）" % pid)
            print("         └ → 请改用 Web Server：Options > Server > 勾选 Run（默认 8085）")
        elif running is False:
            print("LHM WMI  (# root\\LibreHardwareMonitor): namespace 已注册, 但 Sensor=0"
                  " → LHM.exe 没在运行 (decoupled provider 需要进程供数据)")
            print("         └ 注册元数据: C:\\Windows\\System32\\wbem\\Framework\\root\\"
                  "LibreHardwareMonitor\\LibreHardwareMonitor_SN__Version_*.mof")
        else:
            print("LHM WMI  (# root\\LibreHardwareMonitor): namespace 已注册, 但 Sensor=0"
                  " (psutil 缺失, 无法判断进程是否在跑)")
    else:
        print("LHM WMI  (# root\\LibreHardwareMonitor): 不可用 (%s)"
              % (st["error"] or "namespace 不存在"))

    lhm = lhm_read()
    if lhm:
        print("LHM HTTP (127.0.0.1:8085)      : 运行中 (叶子 %d 个, CPU 最高温 %s, 风扇 %s)"
              % (lhm["leaf_count"], _fmt(lhm.get("cpu_temp_max"), " C"),
                 _fmt(lhm.get("cpu_fan_rpm"), " RPM")))
    else:
        print("LHM HTTP (127.0.0.1:8085)      : 未响应")
        if running:
            print("         └ LHM 进程在跑, 但 Web Server 没起来")
            print("         └ 查 config 的 listenerIp: 值为 \"+\" 时绑定全网卡, "
                  "需管理员权限; 非管理员会以'拒绝访问'静默失败 → 应为 127.0.0.1")
            print("         └ 或界面里勾选 Options > Server > Run (默认端口 8085)")
        else:
            print("         └ LHM 未运行 (先启动 LibreHardwareMonitor.exe)")
    print("内存温度            : 本机 DDR4 UDIMM 无传感器 (PDH 无 DIMM 温度计数器)")


def selftest(igcl):
    print("--- 1) 数据源 ---")
    print_providers()
    print("\n--- 2) 采集一次 ---")
    r = collect(igcl, warm=True)
    print("CPU: %s %s%%  RAM: %s%%  GPU: %s"
          % (r["cpu"].get("name"),
             r["cpu"].get("usage_percent"), r["ram"].get("percent"),
             "present" if r["gpu"].get("present") else "absent"))
    print("\n--- 3) --line 输出（上屏 7 项）---")
    line = to_line(r)
    print(line)
    got = {p.split(":", 1)[0]: p.split(":", 1)[1] for p in line.split(",") if ":" in p}
    for k, desc in (("CT", "CPU 温度"), ("CU", "CPU 使用率"), ("CF", "CPU 风扇"),
                    ("GT", "GPU 核心温度"), ("GU", "GPU 使用率"), ("GF", "GPU 风扇"),
                    ("MU", "RAM 使用率")):
        print("    %-3s %-12s %s" % (k, desc, got.get(k) or "-- 缺失 --"))
    print("\n--- 4) JSON 键完整性 ---")
    need = [("cpu", "usage_percent"), ("cpu", "temperature_c"),
            ("cpu", "fan_rpm"), ("ram", "percent"),
            ("ram", "temperature_c"), ("gpu", "present")]
    ok = True
    for parent, key in need:
        has = key in r[parent]
        ok = ok and has
        print("  %-6s.%-18s %s" % (parent, key, "OK" if has else "MISSING"))
    if r["gpu"].get("present"):
        for key in ("temp_gpu_c", "fan_rpm", "usage_percent"):
            has = key in r["gpu"]
            ok = ok and has
            print("  %-6s.%-18s %s" % ("gpu", key, "OK" if has else "MISSING"))
    print("\nSELFTEST_%s" % ("OK" if ok else "BAD"))
    return 0 if ok else 5


# ---------------------------------------------------------------------------
def list_fans(url=LHM_DEFAULT_URL):
    """列出 LHM 里所有风扇通道名 —— 用来确定 --cpu-fan 该填什么。

    为什么需要它：LHM 对 NCT6687D 用的是通用通道名（CPU Fan / System Fan #N），
    与主板丝印不一定对应。换主板或换机器后先跑这个再决定。
    """
    lhm = lhm_read(url, cpu_fan_name="auto")
    if not lhm:
        print("[FAIL] LHM 没运行或读不到 data.json。\n"
              "       先启动 LibreHardwareMonitor，并勾选 Options > Server > Run",
              file=sys.stderr)
        return 2
    fans = lhm.get("all_fans") or []
    if not fans:
        print("LHM 里没有任何风扇通道（叶子数 %d）" % lhm.get("leaf_count", 0))
        return 1
    print("=== LHM 风扇通道（共 %d 个）===" % len(fans))
    for f in fans:
        mark = "   <== 当前 CPU_FAN_NAME" if \
            f["name"].rsplit(" / ", 1)[-1].strip().lower() == CPU_FAN_NAME.lower() else ""
        print("  %-8s %s%s" % ("%d RPM" % f["rpm"], f["name"], mark))
    print("\n当前 CPU_FAN_NAME = %r（可用 --cpu-fan 覆盖）" % CPU_FAN_NAME)
    return 0


def main():
    ap = argparse.ArgumentParser(
        description="整机监控：GPU(IGCL+PDH) + CPU/内存(psutil)，供 RA6E2 串口屏显示")
    ap.add_argument("--json", action="store_true", help="输出 JSON")
    ap.add_argument("--line", action="store_true", help="单行紧凑格式(串口用)")
    ap.add_argument("--loop", type=int, default=0, help="循环次数(0=一次)")
    ap.add_argument("--interval", type=float, default=1.0, help="循环间隔秒")
    ap.add_argument("--no-gpu", action="store_true", help="跳过 GPU")
    ap.add_argument("--providers", action="store_true", help="只列数据源探测结果")
    ap.add_argument("--selftest", action="store_true", help="自检")
    ap.add_argument("--lhm-url", default=LHM_DEFAULT_URL, help="LHM data.json 地址")
    ap.add_argument("--allow-acpi-temp", action="store_true",
                    help="允许在 --line 里使用 ACPI 占位温度(默认禁止)")
    ap.add_argument("--cpu-fan", default=None, metavar="通道名",
                    help="指定哪个风扇通道作为 CPU 风扇。默认 %r；"
                         "传 auto 回到旧行为(只认 HardwareType=Cpu)" % CPU_FAN_NAME)
    ap.add_argument("--list-fans", action="store_true",
                    help="只列出 LHM 里的所有风扇通道名(用于确定 --cpu-fan 该填什么)")
    args = ap.parse_args()

    if args.list_fans:
        return list_fans(args.lhm_url)

    if not HAVE_PSUTIL:
        print("[WARN] psutil 未安装，CPU/内存指标将缺失: pip install psutil",
              file=sys.stderr)

    if args.providers:
        print_providers()
        return 0

    igcl = None
    if not args.no_gpu:
        if not HAVE_IGCL_MODULE:
            print("[FAIL] igcl_probe.py 导入失败（同目录?）", file=sys.stderr)
            return 2
        try:
            igcl = Igcl()
            igcl.init()
            print("[OK] IGCL init, supportedVersion=0x%08X"
                  % igcl.supported_version, file=sys.stderr)
        except IgclError as e:
            print("[FAIL] IGCL: %s" % e, file=sys.stderr)
            return 3

    try:
        if args.selftest:
            return selftest(igcl)

        runs = args.loop if args.loop > 0 else 1
        for i in range(runs):
            r = collect(igcl, lhm_url=args.lhm_url,
                        warm=(i == 0 and runs == 1),
                        cpu_fan_name=args.cpu_fan)
            if args.json:
                print(json.dumps(r, ensure_ascii=False, indent=2))
            elif args.line:
                print(to_line(r, allow_acpi=args.allow_acpi_temp), flush=True)
            else:
                print_report(r)
            if i + 1 < runs:
                time.sleep(args.interval)
        return 0
    finally:
        if igcl is not None:
            igcl.close()


if __name__ == "__main__":
    sys.exit(main())
