#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""串口链路自检（找"到底哪个 COM 口连到了 MCU 的 SCI0"）

用途
----
MCU 侧已经把 SCI0 (P100 RXD0 / P101 TXD0, 115200 8N1) 配好并在收，
但"电脑上的哪个 COM 口连着这两根线"没法从软件上猜。本工具用固件的
TX 心跳行 `BEACON <n>` 反查：

  1. 逐个打开本机串口（跳过明显不是目标的口），各听 4 秒；
  2. 能听到 `BEACON` 的口 = 与开发板真的连通（顺带证明波特率和引脚复用都对）；
  3. 找到后再往该口发几帧真数据，最后用 probe-rs 读固件里的
     `g_link_frames` / `g_link_rx_bytes`，确认 MCU 确实解析成功了。

前提：固件里 `src/uart/bsp_uart_link.h` 的 `UART_LINK_BEACON` 必须为 1。

用法
----
    python host/uart_link_check.py                # 全自动
    python host/uart_link_check.py --listen 6     # 每个口听 6 秒
    python host/uart_link_check.py --skip COM3 COM4 COM1
"""

import argparse
import os
import re
import subprocess
import sys
import time

BAUD = 115200
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF = os.path.join(ROOT, "build", "ra6e2_fw.elf")
PROBE_RS = "E:/.cargo/bin/probe-rs.exe"
NM = "arm-none-eabi-nm"
CHIP = "R7FA6E2BB"
PROBE = "0d28:0204"

# 明显不是目标的设备（别的开发板/板载调试口），默认跳过，省得逐个听浪费时间
DEFAULT_SKIP = ["COM1"]


def list_ports():
    from serial.tools import list_ports as lp

    out = []
    for p in lp.comports():
        out.append((p.device, p.description, f"{p.vid:04X}:{p.pid:04X}" if p.vid else ""))
    return out


def listen(dev, seconds):
    """打开 dev 听 seconds 秒，返回收到的字节。"""
    import serial

    data = b""
    try:
        s = serial.Serial(dev, BAUD, timeout=0.2)
    except Exception as e:                                    # noqa: BLE001
        return None, str(e)
    try:
        t0 = time.time()
        while time.time() - t0 < seconds:
            try:
                d = s.read(256)
            except Exception:                                 # noqa: BLE001
                break
            if d:
                data += d
    finally:
        s.close()
    return data, ""


def send_frames(dev, count):
    """用 link_serial 的帧组装逻辑往 dev 发 count 帧（复用同一个 host 脚本）。"""
    cmd = [
        sys.executable,
        os.path.join(ROOT, "host", "link_serial.py"),
        "--port", dev,
        "--loop", str(count),
        "--interval", "1",
    ]
    r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    return r.returncode == 0, (r.stdout or "") + (r.stderr or "")


def symbol_addr(name):
    """从 elf 里取符号地址（RAM 绝对地址）。"""
    try:
        out = subprocess.check_output([NM, ELF], text=True, errors="replace")
    except Exception:                                         # noqa: BLE001
        return None
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == name:
            return int(parts[0], 16)
    return None


def read_u32(addr, words):
    cmd = [PROBE_RS, "read", "--chip", CHIP, "--probe", PROBE, "b32", hex(addr), str(words)]
    try:
        out = subprocess.check_output(cmd, text=True, errors="replace", stderr=subprocess.STDOUT)
    except Exception as e:                                    # noqa: BLE001
        return None, str(e)
    vals = re.findall(r"\b[0-9a-fA-F]{8}\b", out)
    if len(vals) < words:
        return None, out.strip()
    return [int(v, 16) for v in vals[:words]], ""


def main():
    ap = argparse.ArgumentParser(description="找连到 MCU SCI0 的 COM 口，并验证端到端解析")
    ap.add_argument("--listen", type=float, default=4.0, help="每个口听几秒（默认 4）")
    ap.add_argument("--skip", nargs="*", default=DEFAULT_SKIP, help="要跳过的 COM 口")
    ap.add_argument("--frames", type=int, default=4, help="找到后发几帧（默认 4）")
    args = ap.parse_args()

    ports = list_ports()
    if not ports:
        print("本机没有任何串口。")
        return 1

    print("=== 本机串口 ===")
    for dev, desc, vidpid in ports:
        mark = "  <== 跳过" if dev in args.skip else ""
        print(f"  {dev:6s} {desc:34s} {vidpid}{mark}")

    heard = []
    print(f"\n=== 逐个听 {args.listen:g} 秒，找固件发出的 BEACON 行 ===")
    for dev, desc, _vidpid in ports:
        if dev in args.skip:
            continue
        data, err = listen(dev, args.listen)
        if data is None:
            print(f"  {dev:6s} 打不开: {err}")
            continue
        if b"BEACON" in data:
            head = data.split(b"BEACON", 1)[1][:20]
            print(f"  {dev:6s} ★ 收到 BEACON（后面是序号 {head!r}）")
            heard.append(dev)
        elif data:
            print(f"  {dev:6s} 收到 {len(data)} 字节，但不是本固件的心跳: {data[:40]!r}")
        else:
            print(f"  {dev:6s} 没有数据")

    if not heard:
        print("\n结论: 没有任何一个 COM 口收到 BEACON。")
        print("  可能原因（按可能性排序）:")
        print("    1) 板的 USB-TTL 没插到电脑（TYPE-C 未接）—— 端口根本不存在；")
        print("    2) PC 的 TX 没接到 MCU 的 P100（RXD0），MCU 的 P101（TXD0）也没接回 PC；")
        print("    3) 接到的是另外一组脚（不是 P100/P101）。")
        print("  需要的接线只有 3 根: PC_TX->P100, PC_RX->P101, GND->GND（115200 8N1）。")
        return 2

    dev = heard[0]
    print(f"\n=== 用 {dev} 端到端验证 ===")

    # 先读一次基线
    a_frames = symbol_addr("g_link_frames")
    a_rx = symbol_addr("g_link_rx_bytes")
    if not (a_frames and a_rx):
        print("  ! 取不到 elf 里的符号地址，跳过探针复核（只看发送是否成功）")
        a_frames = a_rx = None
    base = None
    if a_frames:
        v, _ = read_u32(a_rx, 2)
        if v:
            base = v
            print(f"  发送前: rx_bytes={v[0]}  frames={v[1]}")

    ok, log = send_frames(dev, args.frames)
    print("  发送" + ("成功" if ok else "失败"))
    if not ok:
        print(log[-400:])
        return 3

    time.sleep(2.0)
    if a_frames:
        v, err = read_u32(a_rx, 2)
        if v:
            print(f"  发送后: rx_bytes={v[0]}  frames={v[1]}")
            if base and v[0] > base[0] and v[1] > base[1]:
                print("  ✅ 端到端通过：MCU 收到字节并解析出帧")
                return 0
            print("  ✘ MCU 没收到：接线或波特率仍不对")
            return 4
        print(f"  ! 探针读取失败: {err}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
