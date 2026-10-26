#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# 给 RA6E2 的 USB CDC 注入假 link 帧, 制造"风扇转动"的忙态,
# 用于验证 CYCCNT CPU 占用率测量。不动主机风扇, 只给 MCU 喂假数据。
#
# ┌─────────────────────────────────────────────────────────────────────────┐
# │ 安全原则（与烧录脚本一致：按【板子身份】识别，绝不靠默认 COM 号）            │
# │   1. 先用 board_id 的同款身份识别（tools/boards.json 里的 ra6e2-dqx：        │
# │      VID:PID + 序列号）确认目标板在位、且归属本项目 —— 相当于烧录前的       │
# │      `--require ra6e2-dqx` 闸门；                                          │
# │   2. 再通过 pyserial 按同一 VID:PID（045B:0263）找到 CDC 串口的 COM 号；    │
# │   3. 找不到就报错退出，绝不回退到"COM17"之类硬编码端口；                  │
# │   4. CH340（1a86:7523 = STM32H743 的 USB-TTL，COM9，do_not_touch）因为     │
# │      VID:PID 不匹配，天然不会被选到，不会误发数据。                        │
# └─────────────────────────────────────────────────────────────────────────┘
#   - 风扇 RPM 数据经 USB CDC 进入 link 解析链路(UART_Link_PushBytes), 不是本机 SCI0 UART;
#   - 帧格式: $CF:<rpm>,GF:<rpm>*XX\r\n, XX = '$'起到'*'前所有字节的 XOR;
#   - 每帧 s_alive_ms=2000, 需间隔<2s 持续发才能维持 linked+转动。
# 用法:
#   py -3.12 tools/inject_load.py                 # 自动按身份找 RA6E2 的 CDC 口
#   py -3.12 tools/inject_load.py COM15 1500 1200 40   # 显式指定端口(仍走身份闸门)
#   py -3.12 tools/inject_load.py --list-ports    # 只看本机串口及识别结果
import argparse
import os
import sys
import time
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import board_id  # 复用烧录同款身份识别（tools/boards.json + Windows SetupAPI）

try:
    import serial
    from serial.tools import list_ports
    HAVE_SERIAL = True
except ImportError:
    HAVE_SERIAL = False

# RA6E2 板载 TYPE-C 直连 MCU 原生 USB（P814=D+/P815=D-），板上没有 UART 转串口芯片，
# "串口"是固件里实现的 USB CDC 虚拟串口，VID:PID = 045B:0263（bDeviceClass=0x02 → usbser）。
CDC_VID_PID = (0x045B, 0x0263)
BOARD_ID = "ra6e2-dqx"          # boards.json 里登记的目标板身份
CF = 1500
GF = 1200
N = 40                         # 默认 40 帧 × 0.5s ≈ 20s
BAUD = 115200


def find_cdc_ports():
    """按 VID:PID 045B:0263 列出所有 RA6E2 CDC 串口（含序列号），供身份闸门核对。"""
    if not HAVE_SERIAL:
        return []
    out = []
    for p in list_ports.comports():
        if p.vid is not None and (p.vid, p.pid) == CDC_VID_PID:
            out.append((p.device, p.serial_number))
    return out


def require_target_board():
    """复用 board_id 的同款身份识别：确认 ra6e2-dqx 在位且归属本项目。

    返回匹配到的 (device, serial)；认不出或归属不符则报错退出。
    这等价于烧录前的 `python tools/board_id.py --require ra6e2-dqx` 硬闸门。"""
    reg_path = os.path.join(HERE, "boards.json")
    boards = board_id.load_registry(Path(reg_path))
    target = next((b for b in boards if b.get("id") == BOARD_ID), None)
    if target is None:
        print("[FAIL] boards.json 里没有 %s 登记项" % BOARD_ID, file=sys.stderr)
        sys.exit(3)

    # 收集在位的 USB 设备身份
    try:
        present = [t for t in (board_id.parse_instance(x)
                               for x in board_id.list_present_usb()) if t]
    except Exception as e:  # noqa: BLE001  —— 非 Windows / SetupAPI 失败
        print("[FAIL] 无法枚举本机 USB 设备（%s）。请确认在 Windows 上运行，"
              "或用 --port 显式指定 RA6E2 的 CDC 端口。" % e, file=sys.stderr)
        sys.exit(3)

    # 闸门一：boards.json 身份必须匹配到在位的 USB 设备
    matched = [(v, p, s) for (v, p, s) in present if board_id.match(target, v, p, s)]
    if not matched:
        print("[FAIL] 没识别到 RA6E2（%s）在位 —— 请插好 TYPE-C，"
              "或用 `python tools/board_id.py --require %s` 自查。"
              % (BOARD_ID, BOARD_ID), file=sys.stderr)
        sys.exit(2)

    # 闸门二：归属必须是本项目（与 board_id.py 同款：Path(...).as_posix() 归一化）
    proj = Path(HERE).resolve().parent
    allowed = target.get("allowed_in")
    if allowed is not None:
        ok = any(Path(a).resolve().as_posix().lower() == proj.as_posix().lower()
                 for a in allowed)
    else:
        ok = (target.get("owning_project", "").replace("/", "\\").lower()
              == str(proj).replace("/", "\\").lower())
    if not ok:
        print("[FAIL] %s 当前归属不是本项目（%s），停止注入。"
              % (BOARD_ID, target.get("owning_project")), file=sys.stderr)
        sys.exit(3)

    # 在匹配到的 CDC 串口里，挑一个 serial 与身份一致的（一致则强校验，否则退到首个）
    cdc = find_cdc_ports()
    for dev, ser in cdc:
        if any(ser and ser.lower() == s.lower() for (_v, _p, s) in matched):
            return dev, ser
    if cdc:
        return cdc[0]  # 本机只有一块 RA6E2，VID:PID 已足够区分
    print("[FAIL] 身份识别通过，但没找到 045B:0263 对应的 CDC 串口（COM 号）。"
          "请确认 Windows 已为该 CDC 装好 usbser 驱动。", file=sys.stderr)
    sys.exit(2)


def list_serial_ports():
    if not HAVE_SERIAL:
        print("pyserial 未安装: pip install pyserial", file=sys.stderr)
        return
    print("=== 本机串口 ===")
    for p in list_ports.comports():
        mark = ""
        if p.vid is not None and (p.vid, p.pid) == CDC_VID_PID:
            mark = "   <== RA6E2 USB CDC (Type-C 虚拟串口, 045B:0263)"
        print("  %-6s %-34s %s%s" % (p.device, (p.description or "")[:34],
                                     p.hwid or "", mark))


def frame(cf, gf):
    # 协议: XOR 覆盖 '$' 之后到 '*' 之前的所有字符(固件 bsp_uart_link.c:334 从 i=1 跳开 '$')
    payload = "CF:%d,GF:%d" % (cf, gf)
    x = 0
    for b in payload.encode("ascii"):
        x ^= b
    return ("$" + payload + "*%02X" % x).encode("ascii") + b"\r\n"


def main():
    ap = argparse.ArgumentParser(
        description="给 RA6E2 USB CDC 注入假 link 帧（按 ra6e2-dqx 身份识别端口）")
    ap.add_argument("port", nargs="?", default=None,
                    help="显式 COM 端口；省略则按身份自动识别 RA6E2 CDC")
    ap.add_argument("cf", nargs="?", type=int, default=CF, help="CPU 风扇 RPM（默认 %d）" % CF)
    ap.add_argument("gf", nargs="?", type=int, default=GF, help="GPU 风扇 RPM（默认 %d）" % GF)
    ap.add_argument("n", nargs="?", type=int, default=N, help="帧数（默认 %d）" % N)
    ap.add_argument("--baud", type=int, default=BAUD, help="波特率（默认 %d）" % BAUD)
    ap.add_argument("--list-ports", action="store_true",
                    help="只列出本机串口及 RA6E2 识别结果后退出")
    args = ap.parse_args()

    if args.list_ports:
        list_serial_ports()
        return

    if not HAVE_SERIAL:
        print("[FAIL] pyserial 未安装: pip install pyserial", file=sys.stderr)
        sys.exit(2)

    # 身份闸门（与烧录同款）：必须确认 ra6e2-dqx 在位且归属本项目
    dev, ser = require_target_board()

    # 用户显式给端口时尊重，但仍需是识别到的那一个（避免误指到别的口）
    if args.port and args.port.upper() != dev.upper():
        print("[WARN] 显式端口 %s 与身份识别到的 %s 不一致 —— 以身份识别为准。"
              % (args.port, dev), file=sys.stderr)
    port = dev

    ser_h = serial.Serial(port, args.baud, timeout=1)
    print("[inject] open %s %d (RA6E2 CDC, 身份 %s), CF=%d GF=%d x%d"
          % (ser_h.name, args.baud, BOARD_ID, args.cf, args.gf, args.n))
    try:
        for _ in range(args.n):
            ser_h.write(frame(args.cf, args.gf))
            time.sleep(0.5)
    except KeyboardInterrupt:
        pass
    finally:
        ser_h.close()
        print("[inject] done")


if __name__ == "__main__":
    main()
