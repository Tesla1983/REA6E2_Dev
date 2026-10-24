#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""直接向 CMSIS-DAP 探针发 DAP_Info，判断"命令引擎"是死是活。

为什么要这个工具
----------------
`probe-rs` 连不上时分不清到底是
  ① 探针没枚举到（USB 层没通）
  ② 探针枚举到了但 HID 打不开（被独占 / 权限）
  ③ HID 打得开、USB 控制传输正常，但 **CMSIS-DAP 命令没回应**
三步的修法完全不同。probe-rs 对这三种情况都只说一句
`Error: Probe not found`（trace 里才有 "Attempt N to find packet size"）。

本脚本只做和 probe-rs 完全相同的那一件事：往 HID 中断 OUT 端点写一个
report id 0 + 64 字节的 `DAP_Info` 包，然后在中断 IN 端点上等回应。
- 有回应 ⇒ 探针命令引擎活着，问题在主机侧（probe-rs / 驱动 / 选择器）
- 无回应 ⇒ 探针固件主循环卡死，得给探针本身断电或重刷固件

⚠ 全部是只读诊断命令（DAP_Info 只取字符串和计数器），不碰目标、不写 flash。
⚠ 建议先关掉所有会占用探针的程序（probe-rs / pyOCD / IDE 调试会话）。

用法
----
    python tools/daplink_probe.py                # 默认找 0d28:0204
    python tools/daplink_probe.py --vid 0d28 --pid 0204
    python tools/daplink_probe.py --raw          # 连每条命令的收发字节都打出来

判据
----
    DAP_Info(VENDOR)  -> 响应: 00 01 02 06 41 52 4d 00 ...   ('ARM')
    DAP_Info(FWVER)   -> 响应: 00 04 02 03 30 32 35 33 ...   ('0253')
"""
from __future__ import annotations

import argparse
import ctypes
import sys
from ctypes import wintypes as wt

# ---- CMSIS-DAP 命令 ---------------------------------------------------------
DAP_INFO = 0x00
INFO_VENDOR = 0x01
INFO_PRODUCT = 0x02
INFO_SERNUM = 0x03
INFO_FWVER = 0x04
INFO_PACKET_COUNT = 0xFF
INFO_CAPS = 0xF0

REPORT_LEN = 65  # report id(1) + 64 字节载荷


class GUID(ctypes.Structure):
    _fields_ = [("D1", ctypes.c_ulong), ("D2", ctypes.c_ushort),
                ("D3", ctypes.c_ushort), ("D4", ctypes.c_ubyte * 8)]


class OVERLAPPED(ctypes.Structure):
    _fields_ = [("Internal", ctypes.c_void_p), ("InternalHigh", ctypes.c_void_p),
                ("Offset", wt.DWORD), ("OffsetHigh", wt.DWORD), ("hEvent", wt.HANDLE)]


CFGMGR = ctypes.WinDLL("cfgmgr32")
K32 = ctypes.WinDLL("kernel32", use_last_error=True)

CFGMGR.CM_Get_Device_Interface_List_SizeW.argtypes = [
    ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(GUID), ctypes.c_wchar_p, ctypes.c_ulong]
CFGMGR.CM_Get_Device_Interface_ListW.argtypes = [
    ctypes.POINTER(GUID), ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_wchar),
    ctypes.c_ulong, ctypes.c_ulong]
K32.CreateFileW.restype = ctypes.c_void_p
K32.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_ulong, ctypes.c_ulong,
                            ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p]
K32.ReadFile.argtypes = [ctypes.c_void_p, ctypes.c_void_p, wt.DWORD,
                         ctypes.POINTER(wt.DWORD), ctypes.POINTER(OVERLAPPED)]
K32.WriteFile.argtypes = [ctypes.c_void_p, ctypes.c_void_p, wt.DWORD,
                          ctypes.POINTER(wt.DWORD), ctypes.POINTER(OVERLAPPED)]
K32.CreateEventW.restype = wt.HANDLE
K32.WaitForSingleObject.argtypes = [wt.HANDLE, wt.DWORD]
K32.GetOverlappedResult.argtypes = [ctypes.c_void_p, ctypes.POINTER(OVERLAPPED),
                                    ctypes.POINTER(wt.DWORD), wt.BOOL]
K32.CancelIoEx.argtypes = [ctypes.c_void_p, ctypes.POINTER(OVERLAPPED)]

HID_GUID = GUID(0x4D1E55B2, 0xF16F, 0x11CF,
                (ctypes.c_ubyte * 8)(0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30))
INVALID_HANDLE = ctypes.c_void_p(-1).value
FILE_FLAG_OVERLAPPED = 0x40000000
ERROR_IO_PENDING = 997


def list_hid_paths(vid: int, pid: int) -> list[str]:
    ln = ctypes.c_ulong(0)
    if CFGMGR.CM_Get_Device_Interface_List_SizeW(
            ctypes.byref(ln), ctypes.byref(HID_GUID), None, 0) != 0:
        return []
    buf = (ctypes.c_wchar * ln.value)()
    CFGMGR.CM_Get_Device_Interface_ListW(
        ctypes.byref(HID_GUID), None, buf, ln.value, 0)
    paths = [p for p in buf[:].rstrip("\0").split("\0") if p]
    tag = f"vid_{vid:04x}&pid_{pid:04x}"
    return [p for p in paths if tag in p.lower()]


def dap_info(handle: int, info_id: int, wait_ms: int, raw: bool = False):
    """发一条 DAP_Info，返回 (是否成功, 数据字节串)。

    ⚠ **必须先把读请求挂上去，再写命令**。这条 HID 通道（DAPLink v2 / v1 回退）
    如果先写后读，响应会被丢掉 —— 表现为"命令引擎无回应"，其实设备是好的。
    这是 2026-10-22 实测确认的时序，不要改回去。
    """
    inbuf = (ctypes.c_ubyte * REPORT_LEN)()

    # ① 先挂读
    ov_r = OVERLAPPED()
    ov_r.hEvent = K32.CreateEventW(None, True, False, None)
    nr = wt.DWORD(0)
    pend_r = False
    ok = K32.ReadFile(ctypes.c_void_p(handle), inbuf, REPORT_LEN,
                      ctypes.byref(nr), ctypes.byref(ov_r))
    if not ok:
        if ctypes.get_last_error() != ERROR_IO_PENDING:
            K32.CloseHandle(ov_r.hEvent)
            return False, b""
        pend_r = True

    # ② 再写命令
    out = (ctypes.c_ubyte * REPORT_LEN)(*([0] * REPORT_LEN))
    out[0] = 0x00       # report id
    out[1] = DAP_INFO
    out[2] = info_id
    if raw:
        print("   TX:", " ".join("%02x" % out[i] for i in range(8)), "...")
    nw = wt.DWORD(0)
    ov_w = OVERLAPPED()
    ov_w.hEvent = K32.CreateEventW(None, True, False, None)
    ok = K32.WriteFile(ctypes.c_void_p(handle), out, REPORT_LEN,
                       ctypes.byref(nw), ctypes.byref(ov_w))
    if not ok:
        if ctypes.get_last_error() != ERROR_IO_PENDING:
            K32.CloseHandle(ov_w.hEvent)
            if pend_r:
                K32.CancelIoEx(ctypes.c_void_p(handle), ctypes.byref(ov_r))
            K32.CloseHandle(ov_r.hEvent)
            return False, b""
        if K32.WaitForSingleObject(ov_w.hEvent, wait_ms) != 0:
            K32.CancelIoEx(ctypes.c_void_p(handle), ctypes.byref(ov_w))
            K32.CloseHandle(ov_w.hEvent)
            if pend_r:
                K32.CancelIoEx(ctypes.c_void_p(handle), ctypes.byref(ov_r))
            K32.CloseHandle(ov_r.hEvent)
            return False, b""
        K32.GetOverlappedResult(ctypes.c_void_p(handle), ctypes.byref(ov_w),
                                ctypes.byref(nw), False)
    K32.CloseHandle(ov_w.hEvent)

    # ③ 等回应
    if pend_r:
        if K32.WaitForSingleObject(ov_r.hEvent, wait_ms) != 0:
            K32.CancelIoEx(ctypes.c_void_p(handle), ctypes.byref(ov_r))
            K32.CloseHandle(ov_r.hEvent)
            return False, b""
        K32.GetOverlappedResult(ctypes.c_void_p(handle), ctypes.byref(ov_r),
                                ctypes.byref(nr), False)
    K32.CloseHandle(ov_r.hEvent)

    data = bytes(inbuf[1:])          # 丢掉 report id
    if raw:
        print("   RX:", " ".join("%02x" % b for b in data[:16]), "...")

    # DAP_Info 响应格式（CMSIS-DAP 规范，**没有状态字节**）：
    #   data[0] = 命令回显 (DAP_INFO = 0x00)
    #   data[1] = 数据长度 L
    #   data[2 : 2+L] = 数据
    # ⚠ 2026-10-23 修：原先把 data[1] 当 0x02(DAP_OK) 校验、data[2] 当长度，
    #   整体错位一字节 ⇒ 只有 L==2 的 PKT_COUNT 被误判为"唯一有回应"，
    #   其余（含 FWVER/CAPS）一律报无应答，会把好探针诊断成死的。
    if len(data) < 2 or data[0] != DAP_INFO:
        return False, data
    ln = data[1]
    if ln == 0:
        # 长度 0 = 该字段为空。本机定制固件（Local Mods: 1）的 VENDOR 就是空，
        # 这属于"引擎正常应答"，不能算失败。
        return True, b""
    if len(data) < 2 + ln:
        return False, data
    return True, data[2:2 + ln]


CHECKS = [
    ("厂商 (VENDOR)", INFO_VENDOR, True),
    ("产品 (PRODUCT)", INFO_PRODUCT, True),
    ("序列号 (SERNUM)", INFO_SERNUM, True),
    ("固件版本 (FWVER)", INFO_FWVER, True),
    ("能力 (CAPS)", INFO_CAPS, False),
    ("包大小 (PACKET COUNT)", INFO_PACKET_COUNT, False),
]


def main() -> int:
    ap = argparse.ArgumentParser(description="CMSIS-DAP 探针命令引擎探活")
    ap.add_argument("--vid", default="0d28")
    ap.add_argument("--pid", default="0204")
    ap.add_argument("--timeout", type=int, default=600, help="单条命令等待毫秒数")
    ap.add_argument("--raw", action="store_true", help="打印收发原始字节")
    ap.add_argument("--quick", action="store_true",
                    help="只发 1 条 DAP_Info(FWVER) 快速探活（轮询用，约 1 秒）")
    args = ap.parse_args()

    vid, pid = int(args.vid, 16), int(args.pid, 16)
    paths = list_hid_paths(vid, pid)
    if not paths:
        print(f"[!] 没有枚举到 HID 接口 {vid:04x}:{pid:04x} —— 探针没插 / USB 层就不通")
        return 2
    print(f"[i] 找到 {len(paths)} 个 HID 接口: {paths[0]}")

    handle = K32.CreateFileW(paths[0], 0xC0000000, 3, None, 3,
                             FILE_FLAG_OVERLAPPED, None)
    if not handle or handle == INVALID_HANDLE:
        print(f"[!] HID 打不开, Win32 error={ctypes.get_last_error()}"
              "（多半被别的程序独占: probe-rs / IDE 调试会话）")
        return 3

    ok_count = 0
    checks = CHECKS
    if args.quick:
        checks = [("固件版本 (FWVER)", INFO_FWVER, True)]
    try:  # noqa: SIM105
        for name, info_id, as_text in checks:
            ok, data = dap_info(handle, info_id, args.timeout, args.raw)
            if not ok:
                print(f"[X] {name:<22} 无回应或应答异常")
                continue
            ok_count += 1
            if info_id == INFO_PACKET_COUNT:
                # DAP_Info(0xFF) = DAP_PACKET_COUNT：设备最多能缓存多少个包。
                # 注意这不是包大小（包大小是 DAP_PACKET_SIZE = 0xFE），别混。
                cnt = int.from_bytes(data[:2], "little") if len(data) >= 2 else 0
                print(f"[✓] {name:<22} {cnt} 个包（DAP_PACKET_COUNT）")
            elif info_id == INFO_CAPS:
                print(f"[✓] {name:<22} 0x{data[0]:02x}" if data else f"[✓] {name:<22} (空)")
            elif as_text:
                txt = data.decode("ascii", "replace")
                print(f"[✓] {name:<22} {txt if txt else '(空)'}")
            else:
                print(f"[✓] {name:<22} {' '.join('%02x' % b for b in data)}")
    finally:
        K32.CloseHandle(ctypes.c_void_p(handle))

    print()
    if ok_count == 0:
        print("[结论] HID 能打开、但 CMSIS-DAP 一条命令都不回应。")
        print("       ⇒ 探针固件主循环卡住，不是主机侧问题。")
        print("       处理顺序：")
        print("         ① 拔掉 USB **等满 30 秒**再插回（2026-10-23 实测这条能修好）；")
        print("         ② 重测时若 F:/ASSERT.TXT 被重新生成（内容是 usbd_user_hid.c:133），")
        print("            说明命令队列排不出去 = 主循环仍卡着，不是偶发；")
        print("         ③ 仍不行才考虑重刷 interface 固件，或换调试器")
        print("            （例如把树莓派 Pico 刷成官方 debugprobe）。")
        return 1
    print(f"[结论] {ok_count}/{len(checks)} 条命令有回应 ⇒ 探针活着。"
          "连不上就是 probe-rs 选择器 / 驱动侧的事，别怀疑硬件。")
    print("       ⚠ 若 VENDOR/PRODUCT 显示 (空)，是本机固件定制所致，不算故障。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
