#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""LHM data.json 结构探针 / 字段对齐工具。

用途
----
LibreHardwareMonitor (LHM) 的 Web Server 把整棵硬件树序列化成 JSON。
`sysmon.py` 里的解析是"关键词 + 单位"驱动的，但要确认字段名与真实输出一致,
必须先把真机返回的结构打出来看。本工具就是干这个的 —— 只读, 不改任何东西。

前置
----
LHM 已启动, 且 Options > Server > Run 已勾选 (默认端口 8085)。
注意 `listenerIp` 必须是 `127.0.0.1` 或 `+`; 若为 `+` 则要求管理员权限,
否则 HttpListener 会以"拒绝访问"静默失败 (config 已改为 127.0.0.1)。

用法
----
    python lhm_dump.py                 # 打印整棵树 (缩进 + 值)
    python lhm_dump.py --grep 温度      # 只看路径含关键词的叶子
    python lhm_dump.py --grep cpu,fAN  # 多个关键词, 逗号分隔, 忽略大小写
    python lhm_dump.py --raw out.json  # 顺便保存原始 JSON
    python lhm_dump.py --summary       # 只输出归类后的关键指标
    python lhm_dump.py --url http://127.0.0.1:8085/data.json

退出码
------
    0 = 成功读到并解析
    2 = 连不上 LHM (未启动 / Server 未开 / 端口错 / 权限不足)
    3 = 返回体不是合法 JSON
"""

from __future__ import print_function

import argparse
import json
import sys
import urllib.request
import urllib.error

DEFAULT_URL = "http://127.0.0.1:8085/data.json"


def fetch(url, timeout=5.0):
    """取 data.json。抛 urllib.error.URLError 表示连不上。"""
    req = urllib.request.Request(url, headers={"User-Agent": "lhm_dump/1.0"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def walk(node, prefix="", out=None):
    """LHM 的树形结构: {Text, Value, Children:[...]}。递归展平成 (路径, 值)。"""
    if out is None:
        out = []
    text = (node.get("Text") or "").strip()
    value = (node.get("Value") or "").strip()
    kids = node.get("Children") or []

    path = (prefix + "/" + text) if text else prefix

    if kids:
        for k in kids:
            walk(k, path, out)
    else:
        out.append((path, value))
    return out


def short_path(path, keep=3):
    """路径太长, 只留末几段, 便于一行一条地看。"""
    parts = [p for p in path.split("/") if p]
    if len(parts) <= keep:
        return "/".join(parts)
    return ".../" + "/".join(parts[-keep:])


def looks_like_temp(v):
    s = v.replace(" ", "")
    return s.endswith("°C") or s.endswith("C") and any(c.isdigit() for c in s)


def looks_like_rpm(v):
    s = v.upper().replace(" ", "")
    return "RPM" in s


# LHM 里有些"温度"是阈值/余量而非实测值（实测：Intel SSD 的 Critical Temperature
# = 79°C、CPU 的 Distance to TjMax），归类时必须排除，否则会混进 CPU 温度。
_NON_MEASURE = ("warning temperature", "critical temperature",
                "distance to tjmax", "resolution", "offset")

# 叶子上一级一定是这些分组节点之一，用它反推硬件名（比关键词混判可靠）
_GROUPS = ("temperatures", "fans", "controls", "load", "data", "powers",
           "clocks", "voltages", "throughput", "level", "factors", "timings")

_CPU_KEYS = ("core i", "core(tm)", "core ultra", "ryzen", "xeon", "epyc",
             "threadripper", "athlon", "pentium", "celeron", "processor")
_STORAGE = ("ssd", "hdd", "nvme", "disk", "drive", "hard drive")
# 主板上的传感器控制器：CPU 风扇的转速/PWM 挂在这类节点下，不在 CPU 节点下
_SUPERIO = ("nuvoton", "winbond", "fintek", "superio", "it86", "it87", "nct")


def hw_of(path):
    """从 'Computer/机器名/硬件名/分组/叶子' 反推硬件名（分组前一段）。"""
    parts = path.split("/")
    for i in range(len(parts) - 1, 0, -1):
        if parts[i].strip().lower() in _GROUPS:
            return parts[i - 1].strip().lower()
    return parts[-3].strip().lower() if len(parts) >= 3 else ""


def is_non_measure(path):
    leaf = path.rsplit("/", 1)[-1].strip().lower()
    return any(k in leaf for k in _NON_MEASURE)


def summary(leaves):
    """按用途归类, 只打印我们关心的几项, 方便和 sysmon.py 的字段对照。"""
    groups = {
        "CPU 温度": [],
        "CPU 风扇": [],
        "CPU 负载": [],
        "内存 温度": [],
        "内存 负载": [],
        "GPU 温度": [],
        "GPU 风扇": [],
        "GPU 显存": [],
    }
    for path, val in leaves:
        low = path.lower()
        hw = hw_of(path)
        is_storage = any(k in hw for k in _STORAGE)
        is_gpu = any(k in hw for k in ("arc", "geforce", "radeon", "graphics",
                                       "gpu"))
        is_mem = any(k in hw for k in ("memory", "ddr", "dimm", "spd"))
        is_cpu = (not is_storage) and (not is_gpu) and \
                 any(k in hw for k in _CPU_KEYS)

        if "temperature" in low or looks_like_temp(val):
            if is_non_measure(path):
                continue            # 阈值/余量, 不是实测温度
            if is_gpu:
                groups["GPU 温度"].append((path, val))
            elif is_mem:
                groups["内存 温度"].append((path, val))
            elif is_cpu:
                groups["CPU 温度"].append((path, val))
        elif "fan" in low or looks_like_rpm(val):
            is_superio = any(k in hw for k in _SUPERIO)
            if is_gpu:
                groups["GPU 风扇"].append((path, val))
            elif is_cpu or (is_superio and "cpu" in low):
                # SuperIO 下的 CPU Fan 才是主板真正接 CPU 的那一路
                groups["CPU 风扇"].append((path, val))
        elif "load" in low:
            if is_gpu:
                pass
            elif is_mem:
                groups["内存 负载"].append((path, val))
            elif is_cpu:
                groups["CPU 负载"].append((path, val))
        elif "memory" in low and ("used" in low or "available" in low):
            if is_gpu:
                groups["GPU 显存"].append((path, val))
            else:
                groups["内存 负载"].append((path, val))

    for name, items in groups.items():
        if not items:
            print("  %-10s : (无)" % name)
            continue
        print("  %-10s :" % name)
        for path, val in items[:8]:
            print("      %-46s = %s" % (short_path(path, 4), val))
        if len(items) > 8:
            print("      ... 另有 %d 项" % (len(items) - 8))


def main():
    ap = argparse.ArgumentParser(description="打印 LibreHardwareMonitor data.json 结构")
    ap.add_argument("--url", default=DEFAULT_URL, help="data.json 地址")
    ap.add_argument("--grep", default=None, help="只用路径含关键词的叶子, 逗号分隔")
    ap.add_argument("--raw", default=None, help="把原始 JSON 存到这个文件")
    ap.add_argument("--summary", action="store_true", help="只打印归类后的关键指标")
    ap.add_argument("--timeout", type=float, default=5.0)
    args = ap.parse_args()

    try:
        body = fetch(args.url, args.timeout)
    except urllib.error.HTTPError as e:
        print("[FAIL] HTTP %s %s" % (e.code, e.reason), file=sys.stderr)
        print("       地址对但返回错误; 检查 LHM 版本是否支持该路径", file=sys.stderr)
        return 2
    except urllib.error.URLError as e:
        print("[FAIL] 连不上 %s" % args.url, file=sys.stderr)
        print("       原因: %s" % (e.reason,), file=sys.stderr)
        print("", file=sys.stderr)
        print("       排查顺序:", file=sys.stderr)
        print("       1) LHM 进程是否在运行", file=sys.stderr)
        print("       2) Options > Server > Run 是否勾选", file=sys.stderr)
        print("       3) config 里 listenerIp 是否为 127.0.0.1 (为 '+' 需管理员权限)",
              file=sys.stderr)
        print("       4) config 里 listenerPort 是否为 8085", file=sys.stderr)
        return 2
    except Exception as e:                                  # noqa: BLE001
        print("[FAIL] %r" % (e,), file=sys.stderr)
        return 2

    print("[OK] 收到 %d 字节" % len(body))

    if args.raw:
        with open(args.raw, "wb") as f:
            f.write(body)
        print("[OK] 原始 JSON 已存 -> %s" % args.raw)

    try:
        root = json.loads(body.decode("utf-8-sig"))
    except Exception as e:                                  # noqa: BLE001
        print("[FAIL] 不是合法 JSON: %r" % (e,), file=sys.stderr)
        try:
            print("       前 200 字节: %r" % (body[:200].decode("utf-8", "replace"),),
                  file=sys.stderr)
        except Exception:
            pass
        return 3

    leaves = walk(root)
    print("[OK] 叶子节点 %d 个" % len(leaves))
    print()

    if args.grep:
        keys = [k.strip().lower() for k in args.grep.split(",") if k.strip()]
        hit = [(p, v) for p, v in leaves if any(k in p.lower() for k in keys)]
        print("=== 命中 %d 条 (grep=%s) ===" % (len(hit), args.grep))
        for p, v in hit:
            print("  %-52s = %s" % (short_path(p, 4), v))
        if not hit:
            print("  (没有匹配项; 去掉 --grep 看全量)")
            print()
            print("  提示: 可用的顶层硬件名如下")
            for c in (root.get("Children") or []):
                print("    - %s" % (c.get("Text") or ""))
        return 0

    if args.summary:
        print("=== 归类摘要 ===")
        summary(leaves)
        return 0

    print("=== 完整树 ===")
    for p, v in leaves:
        print("  %-56s = %s" % (short_path(p, 4), v))
    return 0


if __name__ == "__main__":
    sys.exit(main())
