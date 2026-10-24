#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""验证 LHM 读到的 CPU 温度是"真温度"还是"固件占位值"。

判据很简单: 真核心温度会随 CPU 负载上升, 占位值是常数。
Windows 的 ACPI 热区(PDH \\Thermal Zone Information)在台式机上多半是
固件写死的常数 —— 实测 6 进程满载 8 秒一字不变, 所以不能当温度用。

本脚本: 空载采样 -> 6 进程满载 -> 再采样 -> 对比。
真温度应上升; 若前后完全相同(< 0.5°C 差), 说明该读数不可信。

用法:  python _probe_lhm_load.py [满载秒数, 默认 8]
"""

import multiprocessing
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sysmon                                        # noqa: E402


def burn(seconds):
    """纯 CPU 密集循环, 不做 IO —— 保证真的把核占满。"""
    end = time.time() + seconds
    x = 0.0
    while time.time() < end:
        for _ in range(20000):
            x = x * 1.0000001 + 1.0
        if x > 1e30:
            x = 0.0


def core_temps(sample):
    """从 lhm_read 的结果里抽出各核温度列表。"""
    return [t["celsius"] for t in sample.get("cpu_temps", [])
            if "cpu core #" in t["name"].lower()]


def show(tag, s):
    if not s:
        print("  %-8s LHM 读不到数据" % tag)
        return None
    cores = core_temps(s)
    print("  %-8s 最高=%.1f  封装=%s  各核均值=%s  各核=%s"
          % (tag,
             s.get("cpu_temp_max") or -1,
             ("%.1f" % s["cpu_package"]) if s.get("cpu_package") is not None else "N/A",
             ("%.1f" % (sum(cores) / len(cores))) if cores else "N/A",
             ",".join("%.0f" % c for c in cores) or "N/A"))
    return s.get("cpu_temp_max")


def main():
    seconds = int(sys.argv[1]) if len(sys.argv) > 1 else 8

    print("=== 步骤 1: 空载基线 ===")
    time.sleep(1.5)
    base = sysmon.lhm_read()
    t_base = show("空载", base)
    if t_base is None:
        print("\n结论: LHM 无数据, 无法验证。先确认 LHM 已启动且 8085 在监听。")
        return 2

    print("\n=== 步骤 2: %d 个进程满载 %d 秒 ===" % (multiprocessing.cpu_count(), seconds))
    procs = [multiprocessing.Process(target=burn, args=(seconds,))
             for _ in range(multiprocessing.cpu_count())]
    for p in procs:
        p.start()

    # 满载期间每 2 秒采一次, 看爬升过程; 同时记峰值(停载后会立即回落, 不能用末次值下结论)
    peak = t_base
    t0 = time.time()
    while time.time() - t0 < seconds - 1.5:
        time.sleep(2.0)
        s = sysmon.lhm_read()
        if s and s.get("cpu_temp_max") is not None:
            peak = max(peak, s["cpu_temp_max"])
            show("满载 %ds" % int(time.time() - t0), s)

    for p in procs:
        p.join()

    time.sleep(1.0)
    show("停载后", sysmon.lhm_read())

    print("\n=== 结论 ===")
    delta = peak - t_base
    print("  空载 %.1f °C -> 峰值 %.1f °C，升温 %.1f °C" % (t_base, peak, delta))
    if delta >= 3.0:
        print("  [可信] 温度随负载明显上升，是真实核心温度（读的是 MSR，不是固件常数）")
        return 0
    if delta >= 0.5:
        print("  [偏弱] 有上升但幅度小：散热较强或负载时长不足，可加长秒数复测")
        return 0
    print("  [可疑] 满载后基本不变，可能读到的是常数占位值，不能当温度用")
    return 1


if __name__ == "__main__":
    multiprocessing.freeze_support()
    sys.exit(main())
