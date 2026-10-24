#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""flash_retry.py —— 高频重试烧录，抢探针的"稳定窗口"。

背景（2026-10-24）：
  这块 DAPLink 在 SWD 传输失败后会**自己重新枚举**（USB reset），
  表现为 `probe-rs list` 刚还能看到、下一条命令就 `Probe not found`。
  靠"拔插一次就烧成"已经不成立了，只能高频重试，抓住它连续可用的几秒。

判据只看**烧录本身**成不成，不再做任何额外的 SWD/HID 探测
（daplink_probe 那种 HID 交互反而会扰动它）。

用法:
    python scripts/flash_retry.py                # 默认 40 次, 间隔 6 s
    python scripts/flash_retry.py --tries 20 --gap 8
"""

import argparse
import subprocess
import sys
import time

BASH = r"C:\Program Files\Git\bin\bash.exe"
PROBE = "0d28:0204"

# 速率阶梯：400 今天成功过；100 是最后的保险（SWD 更容易稳）
SPEEDS = [400, 400, 100]


def log(msg):
    print(f"[retry] {msg}", flush=True)


def sh(cmd, timeout):
    try:
        return subprocess.run(
            [BASH, "-lc", f"cd /e/REA6E2_Dev && {cmd}"],
            capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return None


def attempt(speed):
    """返回 (成功?, 关键输出行)。"""
    r = sh(
        f"./scripts/flash.sh --probe {PROBE} --speed {speed} --no-dblbuf --verify 2>&1",
        timeout=420,
    )
    if r is None:
        return False, "超时 420 s"
    out = r.stdout or ""
    if "烧录/校验成功" in out:
        return True, "烧录/校验成功"
    # 取最有信息量的一行
    for key in ("Probe not found", "SwjSequence", "hid_write", "Failed to erase",
                "flash algorithm", "Timeout in USB", "Connecting to the chip"):
        if key in out:
            return False, key
    tail = [l.strip() for l in out.splitlines() if l.strip()]
    return False, (tail[-1] if tail else "无输出")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tries", type=int, default=40)
    ap.add_argument("--gap", type=int, default=6)
    args = ap.parse_args()

    for i in range(1, args.tries + 1):
        speed = SPEEDS[(i - 1) % len(SPEEDS)]
        ok, note = attempt(speed)
        if ok:
            log(f"✔ 第 {i} 次（{speed} kHz）成功")
            return 0
        log(f"第 {i}/{args.tries} 次（{speed} kHz）失败: {note}")
        time.sleep(args.gap)

    log("✘ 全部重试都失败。探针需要彻底断电 30 s 以上（或换调试器）")
    return 1


if __name__ == "__main__":
    sys.exit(main())
