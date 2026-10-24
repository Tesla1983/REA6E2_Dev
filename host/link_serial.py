#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
link_serial.py —— 把 sysmon 采集的 7 项指标打成帧，经 USB-TTL 串口发给 RA6E2

这是 3.5" 屏接入的 **P3/P4 准备件**：屏还没接也能跑（--dry-run 看帧），
屏接好后同样的命令就能直接驱动显示。

帧格式（与 docs/3.5寸屏开发文档.md §6 一致）
    $CT:48,CU:12,CF:1050,GT:54,GU:3,GF:1064,MU:69*3C\r\n
     └┬┘                                    └┬┘
      帧头                                   异或校验(两位十六进制大写)

    字段：CT=Cpu温度°C  CU=Cpu使用率%  CF=Cpu风扇RPM
          GT=Gpu核心温度°C  GU=Gpu使用率%  GF=Gpu风扇RPM
          MU=内存使用率%

为什么帧头是 '$'、结尾是 '\r\n'
    串口上电/插拔时会出现半个字节或乱码。固件收到 '$' 才认为"从这里开始是一帧"，
    没收到 '\r\n' 之前一直攒；超长就丢弃重新等 '$'。这样任何一次干扰最多丢一帧，
    不会把后面所有帧都带偏。

用法
    python link_serial.py                       # 自动找 DAPLink 的串口，1Hz 发 10 帧
    python link_serial.py --dry-run             # 不开串口，只打印帧（屏没到也能看）
    python link_serial.py --port COM6 --loop 0  # 0 = 一直发（Ctrl+C 停）
    python link_serial.py --loop 0 --interval 2 # 2 秒一帧
    python link_serial.py --no-checksum         # 不带 *XX 校验（调试固件解析用）

安全说明
    * 只往串口**写**，不读、不配置串口以外的任何硬件。
    * RA6E2 走 SWD 烧录，与这个串口互不影响，发数据不会干扰调试。
    * 目标串口按 USB VID:PID 自动识别，认不出时要求显式 --port，不猜。

退出码
    0 正常；2 串口打开失败；3 IGCL/采集失败
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

try:
    import serial
    from serial.tools import list_ports
    HAVE_SERIAL = True
except ImportError:
    HAVE_SERIAL = False

try:
    import sysmon
except Exception as e:                                  # noqa: BLE001
    print("[FAIL] 无法导入 sysmon.py（需与本脚本同目录）: %s" % e, file=sys.stderr)
    raise SystemExit(3)

# 板载 TYPE-C 直连 MCU 原生 USB（P814=D+ / P815=D-），板上**没有** UART 转串口芯片。
# “串口”是固件里实现的 USB CDC 虚拟串口，VID:PID = 045B:0263
# （设备描述符见 src/usb/usb_cdc_descriptor.c，bDeviceClass=0x02 → Windows 自动挂 usbser.sys）。
CDC_VID_PID = (0x045B, 0x0263)
# DAPLink 自己的 CDC 口：只当调试器用，不是数据通路（P100/P101 没接到它上面）。
DAPLINK_VID_PID = (0x0D28, 0x0204)
# 识别优先级：先认板子自己的 CDC，再退到 DAPLink 的 CDC。
TARGET_VID_PIDS = (CDC_VID_PID, DAPLINK_VID_PID)

FRAME_START = "$"
FRAME_END = "\r\n"


def xor_checksum(payload):
    c = 0
    for ch in payload.encode("ascii"):
        c ^= ch
    return c


def build_frame(line, checksum=True):
    """把 sysmon.to_line() 的字段串包成完整帧（bytes）。"""
    if not line:
        return None
    body = line
    if checksum:
        body = "%s*%02X" % (line, xor_checksum(line))
    return (FRAME_START + body + FRAME_END).encode("ascii")


def find_target_port():
    """按 VID:PID 找板子的串口（优先 USB CDC，其次 DAPLink CDC）。
    认不出就返回 None，交给用户显式 --port。"""
    if not HAVE_SERIAL:
        return None
    ports = list(list_ports.comports())
    for vid_pid in TARGET_VID_PIDS:
        for p in ports:
            if p.vid is not None and (p.vid, p.pid) == vid_pid:
                return p.device
    return None


def list_serial_ports():
    if not HAVE_SERIAL:
        print("pyserial 未安装: pip install pyserial", file=sys.stderr)
        return
    print("=== 本机串口 ===")
    for p in list_ports.comports():
        mark = ""
        if p.vid is not None and (p.vid, p.pid) == CDC_VID_PID:
            mark = "   <== RA6E2 USB CDC (Type-C 虚拟串口)"
        elif p.vid is not None and (p.vid, p.pid) == DAPLINK_VID_PID:
            mark = "   <== DAPLink CDC (调试器, 非数据通路)"
        print("  %-6s %-32s %s%s" % (p.device, (p.description or "")[:32],
                                     p.hwid or "", mark))


def collect_line(igcl, args, warm):
    r = sysmon.collect(igcl, lhm_url=args.lhm_url, warm=warm,
                       no_gpu=(igcl is None), allow_acpi=args.allow_acpi_temp,
                       cpu_fan_name=args.cpu_fan)
    return sysmon.to_line(r, allow_acpi=args.allow_acpi_temp)


def main():
    ap = argparse.ArgumentParser(
        description="把 7 项主机指标打成帧发给 RA6E2（3.5\" 屏数据源）")
    ap.add_argument("--port", help="串口号，如 COM6（省略则按 VID:PID 自动识别 DAPLink）")
    ap.add_argument("--baud", type=int, default=115200, help="波特率，默认 115200")
    ap.add_argument("--loop", type=int, default=10, help="发送帧数，0 = 无限")
    ap.add_argument("--interval", type=float, default=1.0, help="间隔秒，默认 1.0")
    ap.add_argument("--dry-run", action="store_true", help="不开串口，只打印帧")
    ap.add_argument("--no-checksum", action="store_true", help="不附加 *XX 异或校验")
    ap.add_argument("--no-gpu", action="store_true", help="跳过 GPU（只测 CPU/内存链路）")
    ap.add_argument("--lhm-url", default=sysmon.LHM_DEFAULT_URL, help="LHM data.json 地址")
    ap.add_argument("--allow-acpi-temp", action="store_true",
                    help="允许使用 ACPI 占位温度（默认禁止，避免把 27.9 恒定值发上屏）")
    ap.add_argument("--list-ports", action="store_true", help="只列出串口后退出")
    ap.add_argument("--cpu-fan", default=None, metavar="通道名",
                    help="指定哪个风扇通道作为 CPU 风扇（默认 %r）；"
                         "传 auto 回到旧行为" % sysmon.CPU_FAN_NAME)
    ap.add_argument("--list-fans", action="store_true",
                    help="只列出 LHM 的风扇通道名后退出")
    args = ap.parse_args()

    if args.list_fans:
        return sysmon.list_fans(args.lhm_url)

    if args.list_ports:
        list_serial_ports()
        return 0

    # ---- IGCL（GPU 数据源）----
    igcl = None
    if not args.no_gpu:
        if not sysmon.HAVE_IGCL_MODULE:
            print("[FAIL] igcl_probe.py 导入失败", file=sys.stderr)
            return 3
        try:
            igcl = sysmon.Igcl()
            igcl.init()
            print("[OK] IGCL init, supportedVersion=0x%08X"
                  % igcl.supported_version, file=sys.stderr)
        except sysmon.IgclError as e:
            print("[FAIL] IGCL: %s" % e, file=sys.stderr)
            return 3

    # ---- 串口 ----
    ser = None
    port = args.port
    if not args.dry_run:
        if not HAVE_SERIAL:
            print("[FAIL] pyserial 未安装: pip install pyserial", file=sys.stderr)
            return 2
        if not port:
            port = find_target_port()
            if not port:
                print("[FAIL] 没找到 RA6E2 的 USB-TTL 串口（VID:PID 045B:0263）。\n"
                      "       请确认 TYPE-C 已插、或显式 --port COMx；"
                      "用 --list-ports 看现有端口。", file=sys.stderr)
                return 2
        try:
            ser = serial.Serial(port, args.baud, timeout=0.2, write_timeout=1.0)
            print("[OK] 串口已打开 %s @ %d 8N1" % (port, args.baud), file=sys.stderr)
        except Exception as e:                         # noqa: BLE001
            print("[FAIL] 打开 %s 失败: %s" % (port, e), file=sys.stderr)
            return 2

    runs = args.loop if args.loop > 0 else None
    sent, t0 = 0, time.monotonic()
    print("目标帧率 %.1f Hz%s" % (1.0 / args.interval,
                                  "（dry-run，不写串口）" if args.dry_run else ""),
          file=sys.stderr)
    print("-" * 60)

    try:
        while True:
            line = collect_line(igcl, args, warm=(sent == 0))
            frame = build_frame(line, checksum=not args.no_checksum)
            if frame is None:
                print("[WARN] 这一轮没采到任何可用字段，跳过", file=sys.stderr)
            else:
                if ser is not None:
                    ser.write(frame)
                    ser.flush()
                print("%s  %s" % (time.strftime("%H:%M:%S"),
                                  frame.decode("ascii").strip()),
                      flush=True)
                sent += 1
            if runs is not None and sent >= runs:
                break
            time.sleep(max(0.0, args.interval))
    except KeyboardInterrupt:
        print("\n[中断]", file=sys.stderr)
    finally:
        if ser is not None:
            try:
                ser.close()
            except Exception:                          # noqa: BLE001
                pass

    dt = time.monotonic() - t0
    print("-" * 60)
    print("共 %d 帧 / %.1f 秒（实际 %.2f Hz）%s"
          % (sent, dt, sent / dt if dt > 0 else 0,
             "" if args.dry_run else "，已写入 %s" % port))
    return 0


if __name__ == "__main__":
    sys.exit(main())
