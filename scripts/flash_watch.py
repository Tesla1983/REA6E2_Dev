#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""flash_watch.py —— 守着 DAPLink 复活，一活就烧，然后自动验证 `$ID?`

为什么不用 scripts/flash_auto.py：
  · 它会在烧之前做 `probe-rs info`（**SWD 事务**）—— 而本项目的纪律是
    「探针刚复活就烧，中间不要插任何 SWD 读」，多一次 SWD 就多一次卡死风险；
  · 它的 ATTEMPTS 里带 `--connect-under-reset`，但这块板 **nRESET 没接线**，无效。

本脚本的判活方式：`tools/daplink_probe.py --quick`（只发 CMSIS-DAP 命令，
不做 SWD 传输）。这也是 NOTES-daplink.md 里定的判据：
  「F: 无 ASSERT.TXT」**且**「DAP_Info 有应答」两条同时满足才算活。

用法:
    python scripts/flash_watch.py                 # 最多等 60 分钟
    python scripts/flash_watch.py --timeout 600
"""

import argparse
import subprocess
import sys
import time

PY = sys.executable
BASH = r"C:\Program Files\Git\bin\bash.exe"
PROBE = "0d28:0204"
LOG_PREFIX = "[watch]"

import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def log(msg):
    print(f"{LOG_PREFIX} {msg}", flush=True)


def probe_alive():
    """DAPLink 命令引擎是否**全部**命令都应答。

    ⚠ 不能用 --quick（只发 1 条 FWVER）：实测出现过 "1/1 条有回应" 但其实已半死，
      随后 probe-rs 依然选不中它。这里跑完整检查并**要求 N == M**。
    返回 (alive, 结论行)。
    """
    try:
        r = subprocess.run(
            [PY, "tools/daplink_probe.py"],
            cwd=ROOT, capture_output=True, text=True,
            encoding="utf-8", errors="replace", timeout=120,
        )
    except subprocess.TimeoutExpired:
        return False, "检测超时"
    out = (r.stdout or "") + (r.stderr or "")
    line = ""
    for l in out.splitlines():
        if "结论" in l:
            line = l.strip()

    # 解析 "[结论] 6/6 条命令有回应"
    import re
    m = re.search(r"(\d+)\s*/\s*(\d+)\s*条命令有回应", out)
    if not m:
        return False, line or "解析不到应答计数"
    ok_n, total = int(m.group(1)), int(m.group(2))
    return (total >= 4 and ok_n == total), line


def probe_rs_sees_it():
    """probe-rs 是否枚举得到 0d28:0204。

    纯枚举（不做 SWD 传输），但很关键：上一次"引擎半活"时 DAP_Info 有回应，
    probe-rs 却报 Probe not found ⇒ 必须再过多这一关才值得烧。
    """
    try:
        r = subprocess.run(
            [r"E:/.cargo/bin/probe-rs.exe", "list"],
            capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60,
        )
    except Exception:  # noqa: BLE001
        return False
    return ("0d28:0204" in (r.stdout or "")) or ("0d28:0204" in (r.stderr or ""))


def sh(cmd, timeout):
    """在 Git Bash 里跑（flash.sh 需要 bash 语义）。"""
    full = [BASH, "-lc", f"cd /e/REA6E2_Dev && {cmd}"]
    try:
        return subprocess.run(full, capture_output=True, text=True,
                              encoding="utf-8", errors="replace", timeout=timeout)
    except subprocess.TimeoutExpired:
        return None


def flash():
    log("探针双关都过了（引擎全应答 + probe-rs 能枚举）→ 立刻烧录")
    r = sh(f"./scripts/flash.sh --probe {PROBE} --speed 400 --no-dblbuf --verify 2>&1", timeout=600)
    if r is None:
        log("✘ 烧录超时（600 s）")
        return False
    tail = [l for l in (r.stdout or "").splitlines() if l.strip()][-8:]
    for l in tail:
        print("    " + l, flush=True)
    return "烧录/校验成功" in (r.stdout or "")


def reset_target():
    log("复位内核（probe-rs reset）")
    r = sh(f'"E:/.cargo/bin/probe-rs.exe" reset --chip R7FA6E2BB --probe {PROBE} --speed 400',
           timeout=120)
    return r is not None and r.returncode == 0


def wait_com15(seconds):
    """等 COM15 出现在 SERIALCOMM（reset 后实测要 ~45 s）。"""
    import winreg

    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            k = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DEVICEMAP\SERIALCOMM")
            vals = []
            i = 0
            while True:
                try:
                    _n, v, _t = winreg.EnumValue(k, i)
                    vals.append(v)
                    i += 1
                except OSError:
                    break
            if "COM15" in vals:
                return True
        except OSError:
            pass
        time.sleep(2)
    return False


def send_id_query():
    """发 $ID? 并打印应答 —— 验证固件 v14 的身份查询。"""
    import serial

    payload = "ID?"
    chk = 0
    for b in payload.encode():
        chk ^= b
    frame = f"${payload}*{chk:02X}\r\n".encode()

    t0 = time.time()
    try:
        ser = serial.Serial("COM15", 115200, timeout=0.5, write_timeout=3.0)
        log(f"COM15 已打开（{time.time() - t0:.2f} s）")
        for i in range(8):
            ser.write(frame)
            ser.flush()
            time.sleep(0.3)
            data = ser.read(200)
            if data:
                log(f"✔ 收到应答: {data!r}")
                ser.close()
                return True
            log(f"  第 {i + 1} 次无应答")
        ser.close()
    except Exception as e:  # noqa: BLE001
        log(f"✘ 串口异常: {type(e).__name__}: {e}")
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=3600, help="最多等多少秒（默认 3600）")
    ap.add_argument("--interval", type=int, default=10, help="检测间隔秒（默认 10）")
    args = ap.parse_args()

    deadline = time.time() + args.timeout
    attempt = 0

    while time.time() < deadline:
        attempt += 1
        alive, note = probe_alive()
        if not alive:
            log(f"第 {attempt} 次: 引擎未全应答  ({note or '无结论'})，{args.interval} s 后再看")
            time.sleep(args.interval)
            continue

        if not probe_rs_sees_it():
            log(f"第 {attempt} 次: 引擎全应答，但 probe-rs 还没枚举到，{args.interval} s 后再看")
            time.sleep(args.interval)
            continue

        log(f"第 {attempt} 次: 双关通过  ({note})")
        break
    else:
        log("✘ 等待超时，探针始终没准备好")
        return 1

    if not flash():
        log("✘ 烧录失败。请再拔一次 DAPLink 满 30 s，然后重跑本脚本。")
        return 2

    log("✔ 烧录成功")
    if not reset_target():
        log("⚠ reset 失败（不影响 flash 内容），请物理拔插 Type-C 让固件跑起来")
        return 3

    log("等 COM15 重新枚举（实测约 45 s）...")
    if not wait_com15(120):
        log("⚠ COM15 没出现。请物理拔插 Type-C 再试。")
        return 4

    if send_id_query():
        log("=== 全部通过：固件 v14 + $ID? 握手正常 ===")
        return 0

    log("⚠ 串口通了但 $ID? 没应答 —— 检查固件是否真的是 v14")
    return 5


if __name__ == "__main__":
    sys.exit(main())
