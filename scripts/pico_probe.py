#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 RP2350(Pico 2) 临时变成 CMSIS-DAP 探针 → 烧 RA6E2 → 再还原成 IMU 固件。

为什么需要这个
--------------
本项目自己的 DAPLink(0d28:0204) 今天反复出问题：
一次 SWD 传输超时就把自己卡死（`F:/ASSERT.TXT` 重现 + `DAP_Info` 0/6），
只能靠人工「拔 USB 等满 30 秒」复活，已经烧掉好几个窗口。
而**树莓派官方 `debugprobe` 固件**跑在 RP2350 上一直很稳（整个项目都是用它烧的）。

流程（每步都在等用户做物理动作，脚本自己盯着）
--------------------------------------------
  阶段 1  等 RPI-RP2 盘出现（用户按住 BOOTSEL 插 USB）→ 拷入 debugprobe_on_pico2.uf2
  阶段 2  等 2e8a:000c 枚举出来 → 自动烧 RA6E2（--speed 1000，不行自动降速重试）
  阶段 3  烧完等用户再次按住 BOOTSEL 插 USB → 拷回 rp2350_mpu6500.uf2 还原 IMU 固件
  （阶段 3 可以用 --no-restore 跳过；之后随时能用 --restore-only 单独还原）

接线（探针阶段）
----------------
    RP2350 GP2 (SWCLK) ──► RA6E2 P300
    RP2350 GP3 (SWDIO) ──► RA6E2 P108
    RP2350 GND        ──► RA6E2 GND
    ⚠ **不要接 3V3** —— RA6E2 用自己的 Type-C 供电

用法
----
    python scripts/pico_probe.py                 # 完整三阶段
    python scripts/pico_probe.py --no-restore    # 只做前两阶段
    python scripts/pico_probe.py --restore-only  # 只还原 IMU 固件
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BAKE = r"C:\Program Files\Git\bin\bash.exe"
PROBE_RS = r"E:/.cargo/bin/probe-rs.exe"

DEBUGPROBE_UF2 = os.path.join(ROOT, "tools", "fw", "debugprobe_on_pico2.uf2")
RESTORE_UF2 = r"E:\RP2350_Dev\rp2350+mpu6500\build\rp2350_mpu6500.uf2"

PICO_VID = "2e8a"
PICO_PID_DEBUGPROBE = "000c"
PICO_PID_IMU = "0009"


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def drives():
    """列出所有可写盘（排除光驱）。返回 {盘符: 根目录}。"""
    out = {}
    try:
        import ctypes
        bitmask = ctypes.windll.kernel32.GetLogicalDrives()
        for i, letter in enumerate("ABCDEFGHIJKLMNOPQRSTUVWXYZ"):
            if not (bitmask >> i) & 1:
                continue
            root = f"{letter}:\\"
            if os.path.exists(root + "INFO_UF2.TXT"):
                out[letter] = root
    except Exception:                                            # noqa: BLE001
        pass
    return out


def wait_bootsel(timeout_s, hint):
    """等 RPI-RP2（BOOTSEL）盘出现。"""
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        d = drives()
        if d:
            letter = next(iter(d))
            log(f"✔ 发现 BOOTSEL 盘 {letter}:\\")
            return letter
        time.sleep(1)
    log(f"✘ 等待 BOOTSEL 盘超时（{timeout_s} s）。{hint}")
    return None


def copy_uf2(letter, src):
    dst = f"{letter}:\\{os.path.basename(src)}"
    log(f"拷贝 {os.path.basename(src)} → {dst} ...")
    shutil.copyfile(src, dst)
    # 等设备自己把文件删掉（RP2 bootloader 拷完就移除该文件）
    for _ in range(60):
        if not os.path.exists(dst):
            log("✔ 拷贝完成（设备已接收并删除该文件）")
            return True
        time.sleep(0.5)
    log("⚠ 文件还在盘上，可能没被 bootloader 取走")
    return False


def probe_rs_list():
    try:
        r = subprocess.run([PROBE_RS, "list"], capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=60)
        return (r.stdout or "") + (r.stderr or "")
    except Exception:                                            # noqa: BLE001
        return ""


def wait_usb(vid, pid, timeout_s):
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        txt = probe_rs_list()
        if f"{vid}:{pid}" in txt:
            log(f"✔ USB {vid}:{pid} 出现了")
            return True
        time.sleep(2)
    log(f"✘ 等待 USB {vid}:{pid} 超时（{timeout_s} s）")
    return False


def wait_com15(timeout_s):
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        try:
            import serial.tools.list_ports as lp
            if any(p.vid == 0x045B and p.pid == 0x0263 for p in lp.comports()):
                log("✔ COM15 (RA6E2 USB CDC) 出现了")
                return True
        except Exception:                                        # noqa: BLE001
            pass
        time.sleep(1)
    return False


def run_flash(extra):
    cmd = f"cd /e/REA6E2_Dev && ./scripts/flash.sh --probe {PICO_VID}:{PICO_PID_DEBUGPROBE} {extra}"
    log(f"$ flash.sh --probe {PICO_VID}:{PICO_PID_DEBUGPROBE} {extra}")
    try:
        r = subprocess.run([BAKE, "-lc", cmd], capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=900)
    except subprocess.TimeoutExpired:
        log("✘ 烧录超时")
        return False
    out = (r.stdout or "") + (r.stderr or "")
    for line in out.splitlines():
        print("    " + line, flush=True)
    return r.returncode == 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-restore", action="store_true", help="不还原 IMU 固件")
    ap.add_argument("--restore-only", action="store_true", help="只还原 IMU 固件")
    ap.add_argument("--wait", type=int, default=600, help="每个阶段的等待秒数")
    args = ap.parse_args()

    for p in (DEBUGPROBE_UF2, RESTORE_UF2):
        if not os.path.exists(p):
            log(f"✘ 找不到 {p}")
            return 2

    # ---------- 阶段 3（可选，--restore-only）：只还原 ----------
    if args.restore_only:
        log("=== 只还原 RP2350 的 IMU 固件 ===")
        letter = wait_bootsel(args.wait, "请按住 BOOTSEL 插 USB，看到 RPI-RP2 盘")
        if not letter:
            return 1
        copy_uf2(letter, RESTORE_UF2)
        wait_usb(PICO_VID, PICO_PID_IMU, 60)
        log("✔ RP2350 已还原成 IMU 固件")
        return 0

    # ---------- 阶段 1：刷成 debugprobe ----------
    log("=== 阶段 1：把 RP2350 刷成 CMSIS-DAP 探针 ===")
    log("请按住 Pico 的 BOOTSEL 键，同时插 USB（或重新插 USB），"
        "直到电脑上出现 RPI-RP2 盘")
    letter = wait_bootsel(args.wait, "请按住 BOOTSEL 插 USB，看到 RPI-RP2 盘")
    if not letter:
        return 1
    copy_uf2(letter, DEBUGPROBE_UF2)
    if not wait_usb(PICO_VID, PICO_PID_DEBUGPROBE, 90):
        log("刷完没枚举成 debugprobe，停止。")
        return 1

    # ---------- 阶段 2：烧 RA6E2 ----------
    log("=== 阶段 2：烧 RA6E2 ===")
    log("接线确认：GP2→P300(SWCLK)、GP3→P108(SWDIO)、GND→GND，**不要接 3V3**；"
        "RA6E2 自己的 Type-C 要插好供电")
    time.sleep(3)

    attempts = ["--speed 1000 --verify",
                "--speed 400 --no-dblbuf --verify",
                "--speed 100 --no-dblbuf --verify"]
    ok = False
    for i, extra in enumerate(attempts, 1):
        ok = run_flash(extra)
        if ok:
            log(f"✔ 第 {i} 次尝试烧录成功")
            break
        log(f"--- 第 {i} 次失败，换下一组参数 ---")

    if not ok:
        log("✘ 烧录失败。若探针还是活的，再跑一次本脚本（--no-restore）即可重试。")
        return 3

    log("=== 烧录完成，等 RA6E2 重新枚举 ===")
    wait_com15(60)

    # ---------- 阶段 4：验证 30 s 卡点 ----------
    log("=== 验证 USB CDC 的 30 s 卡点 ===")
    time.sleep(2)
    r = subprocess.run([sys.executable, os.path.join(ROOT, "host", "usb_cdc_timing.py")],
                       capture_output=True, text=True, encoding="utf-8",
                       errors="replace", timeout=300)
    print(r.stdout or "", flush=True)
    if r.stderr:
        print(r.stderr, flush=True)

    # ---------- 阶段 3：还原 IMU 固件 ----------
    if not args.no_restore:
        log("=== 阶段 3：把 RP2350 还原成 IMU 固件 ===")
        log("⚠ 先把 GP2/GP3/GND 三根 SWD 线从 RA6E2 上拔掉，再按住 BOOTSEL 插 USB")
        letter = wait_bootsel(args.wait, "请按住 BOOTSEL 插 USB，看到 RPI-RP2 盘")
        if letter:
            copy_uf2(letter, RESTORE_UF2)
            wait_usb(PICO_VID, PICO_PID_IMU, 60)
            log("✔ RP2350 已还原成 IMU 固件（2e8a:0009）")
        else:
            log("⚠ 没等到 BOOTSEL 盘，RP2350 还停在 debugprobe 状态。"
                "稍后用 --restore-only 单独还原。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
