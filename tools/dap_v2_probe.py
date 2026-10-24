#!/usr/bin/env python3
"""通过 WebUSB / WinUSB（CMSIS-DAP v2）通道探活 DAPLink。

为什么需要它
------------
同一块探针暴露两条 CMSIS-DAP 通道:

  v1 = HID 中断端点（interface 3）—— probe-rs 0.29 **只认这一条**;
  v2 = WebUSB / WinUSB 批量端点   —— probe-rs 0.29 完全不尝试（trace 原文:
       "Will use interface number 3 for CMSIS-DAPv1"）。

于是出现一个诊断盲区: HID 不回应时, 无法区分
  (a) 探针固件主循环死了      -> v2 也不会回应
  (b) 只有 HID 那条端点坏了    -> v2 仍然正常, 而且可以继续用它读寄存器

本脚本只做 (b) 的判定: 用 WinUSB 打开每一个 0d28 的接口, 发一条 DAP_Info,
看有没有应答。**只发 DAP_Info（纯查询）, 不连接目标、不改任何状态。**

用法
----
    python tools/dap_v2_probe.py
"""

from __future__ import annotations

import ctypes
from ctypes import wintypes as wt

# ---------------------------------------------------------------- Win32 基础

K32 = ctypes.WinDLL("kernel32", use_last_error=True)
CFG = ctypes.WinDLL("cfgmgr32")
WINUSB = ctypes.WinDLL("winusb")

GUID_DEVINTERFACE_USB_DEVICE = "A5DCBF10-6530-11D2-901F-00C04FB951ED"
# WebUSB 那条接口通常由 winusb.sys 承载, 接口 GUID 是下面这个, 不是上面那个。
GUID_DEVINTERFACE_WINUSB = "DEE824EF-729B-4A0E-9C14-B7117D33A817"
GUID_DEVINTERFACE_WINUSB_DEVICE = "88BAE032-5A81-49F0-BC3D-A4FF138216D6"


class GUID(ctypes.Structure):
    _fields_ = [
        ("Data1", ctypes.c_ulong),
        ("Data2", ctypes.c_ushort),
        ("Data3", ctypes.c_ushort),
        ("Data4", ctypes.c_ubyte * 8),
    ]


def _guid(text: str) -> GUID:
    parts = text.split("-")
    d4 = bytes.fromhex(parts[3] + parts[4])
    return GUID(int(parts[0], 16), int(parts[1], 16), int(parts[2], 16),
                (ctypes.c_ubyte * 8)(*d4))


class OVERLAPPED(ctypes.Structure):
    _fields_ = [
        ("Internal", ctypes.c_void_p),
        ("InternalHigh", ctypes.c_void_p),
        ("Offset", wt.DWORD),
        ("OffsetHigh", wt.DWORD),
        ("hEvent", wt.HANDLE),
    ]


class WINUSB_PIPE_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("PipeType", ctypes.c_int),
        ("PipeId", ctypes.c_ubyte),
        ("MaximumPacketSize", ctypes.c_ushort),
        ("Interval", ctypes.c_ubyte),
    ]


class WINUSB_INTERFACE_DESCRIPTOR(ctypes.Structure):
    _fields_ = [
        ("bLength", ctypes.c_ubyte),
        ("bDescriptorType", ctypes.c_ubyte),
        ("bInterfaceNumber", ctypes.c_ubyte),
        ("bAlternateSetting", ctypes.c_ubyte),
        ("bNumEndpoints", ctypes.c_ubyte),
        ("bInterfaceClass", ctypes.c_ubyte),
        ("bInterfaceSubClass", ctypes.c_ubyte),
        ("bInterfaceProtocol", ctypes.c_ubyte),
        ("iInterface", ctypes.c_ubyte),
    ]


CFG.CM_Get_Device_Interface_List_SizeW.argtypes = [
    ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(GUID), ctypes.c_wchar_p, ctypes.c_ulong]
CFG.CM_Get_Device_Interface_ListW.argtypes = [
    ctypes.POINTER(GUID), ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_wchar),
    ctypes.c_ulong, ctypes.c_ulong]

K32.CreateFileW.restype = ctypes.c_void_p
K32.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_ulong, ctypes.c_ulong,
                            ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong,
                            ctypes.c_void_p]
K32.CreateEventW.restype = wt.HANDLE

WINUSB.WinUsb_Initialize.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
WINUSB.WinUsb_Free.argtypes = [ctypes.c_void_p]
WINUSB.WinUsb_QueryInterfaceSettings.argtypes = [
    ctypes.c_void_p, ctypes.c_ubyte, ctypes.POINTER(WINUSB_INTERFACE_DESCRIPTOR)]
WINUSB.WinUsb_QueryPipe.argtypes = [
    ctypes.c_void_p, ctypes.c_ubyte, ctypes.c_ubyte,
    ctypes.POINTER(WINUSB_PIPE_INFORMATION)]
WINUSB.WinUsb_WritePipe.argtypes = [
    ctypes.c_void_p, ctypes.c_ubyte, ctypes.c_void_p, ctypes.c_ulong,
    ctypes.POINTER(ctypes.c_ulong), ctypes.c_void_p]
WINUSB.WinUsb_ReadPipe.argtypes = [
    ctypes.c_void_p, ctypes.c_ubyte, ctypes.c_void_p, ctypes.c_ulong,
    ctypes.POINTER(ctypes.c_ulong), ctypes.c_void_p]
WINUSB.WinUsb_SetPipePolicy.argtypes = [
    ctypes.c_void_p, ctypes.c_ubyte, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p]
WINUSB.WinUsb_SetCurrentAlternateSetting.argtypes = [ctypes.c_void_p, ctypes.c_ubyte]

PIPE_TRANSFER_TIMEOUT = 0x03
RAW_IO = 0x07
GENERIC_READ_WRITE = 0xC0000000
FILE_FLAG_OVERLAPPED = 0x40000000
OPEN_EXISTING = 3
REPORT_LEN = 64


def list_usb_interfaces():
    """把三个可能承载 CMSIS-DAP v2 的接口 GUID 都扫一遍, 去重后返回路径。"""
    found = []
    for text in (GUID_DEVINTERFACE_USB_DEVICE, GUID_DEVINTERFACE_WINUSB,
                 GUID_DEVINTERFACE_WINUSB_DEVICE):
        g = _guid(text)
        n = ctypes.c_ulong(0)
        if CFG.CM_Get_Device_Interface_List_SizeW(
                ctypes.byref(n), ctypes.byref(g), None, 0) != 0:
            continue
        buf = (ctypes.c_wchar * n.value)()
        CFG.CM_Get_Device_Interface_ListW(ctypes.byref(g), None, buf, n.value, 0)
        for p in buf[:].rstrip("\0").split("\0"):
            if p and "vid_0d28" in p.lower() and p not in found:
                found.append(p)
    return found


def try_interface(path: str, timeout_ms: int = 1200) -> str:
    """打开一个接口, 找批量端点, 发一条 DAP_Info, 返回一句人话结果。"""
    h = K32.CreateFileW(path, GENERIC_READ_WRITE, 3, None, OPEN_EXISTING,
                        FILE_FLAG_OVERLAPPED, None)
    invalid = ctypes.c_void_p(-1).value
    if not h or h == invalid:
        return "打不开 (err=%d)" % ctypes.get_last_error()

    winusb = ctypes.c_void_p()
    if not WINUSB.WinUsb_Initialize(ctypes.c_void_p(h), ctypes.byref(winusb)):
        K32.CloseHandle(ctypes.c_void_p(h))
        return "不是 WinUSB 设备 (err=%d)" % ctypes.get_last_error()

    try:
        iface = WINUSB_INTERFACE_DESCRIPTOR()
        if not WINUSB.WinUsb_QueryInterfaceSettings(winusb, 0, ctypes.byref(iface)):
            return "QueryInterfaceSettings 失败"
        if iface.bNumEndpoints < 2:
            # WebUSB 接口常把端点放在备用设置里: 依次看 alt 1..3, 谁有两条批量端点就用谁。
            picked = None
            for alt in range(1, 4):
                alt_desc = WINUSB_INTERFACE_DESCRIPTOR()
                if not WINUSB.WinUsb_QueryInterfaceSettings(
                        winusb, alt, ctypes.byref(alt_desc)):
                    break
                if alt_desc.bNumEndpoints >= 2:
                    if not WINUSB.WinUsb_SetCurrentAlternateSetting(winusb, alt):
                        break
                    picked = alt
                    iface = alt_desc
                    break
            if picked is None:
                return ("非 CMSIS-DAP v2 (alt0 端点 %d, 备用设置也没有 2 条批量端点)"
                        % iface.bNumEndpoints)

        out_id = in_id = None
        for i in range(iface.bNumEndpoints):
            pipe = WINUSB_PIPE_INFORMATION()
            if not WINUSB.WinUsb_QueryPipe(winusb, 0, i, ctypes.byref(pipe)):
                continue
            if pipe.PipeType != 2:          # 2 = 批量传输
                continue
            if pipe.PipeId & 0x80:
                in_id = pipe.PipeId
            else:
                out_id = pipe.PipeId

        if out_id is None or in_id is None:
            return "没找到一进一出两条批量端点"

        tmo = ctypes.c_ulong(timeout_ms)
        for pid in (out_id, in_id):
            WINUSB.WinUsb_SetPipePolicy(winusb, pid, PIPE_TRANSFER_TIMEOUT,
                                        ctypes.sizeof(tmo), ctypes.byref(tmo))

        tx = (ctypes.c_ubyte * REPORT_LEN)()
        tx[0] = 0x00            # DAP_Info
        tx[1] = 0x01            # ID = Vendor
        sent = ctypes.c_ulong(0)
        if not WINUSB.WinUsb_WritePipe(winusb, out_id, tx, REPORT_LEN,
                                       ctypes.byref(sent), None):
            return "写批量端点失败 (err=%d)" % ctypes.get_last_error()

        rx = (ctypes.c_ubyte * REPORT_LEN)()
        got = ctypes.c_ulong(0)
        if not WINUSB.WinUsb_ReadPipe(winusb, in_id, rx, REPORT_LEN,
                                      ctypes.byref(got), None):
            return "批量端点无回应 (err=%d) => 主循环可能已死" % ctypes.get_last_error()

        n = got.value or REPORT_LEN
        head = " ".join("%02x" % rx[i] for i in range(min(12, n)))
        if n >= 3 and rx[0] == 0x00:
            ln = rx[2]
            text = bytes(rx[3:3 + ln]).decode("ascii", "replace")
            return "有回应: %s   原始 %s" % (text or "(空)", head)
        return "回应格式异常: %s" % head
    finally:
        WINUSB.WinUsb_Free(winusb)
        K32.CloseHandle(ctypes.c_void_p(h))


def main() -> int:
    paths = list_usb_interfaces()
    if not paths:
        print("[X] 没有找到 VID_0D28 的 USB 设备接口")
        return 1

    print("[i] VID_0D28 的 USB 设备接口 %d 个:" % len(paths))
    alive = 0
    for p in paths:
        short = p.split("#")[1] if p.count("#") > 1 else p
        res = try_interface(p)
        if "有回应" in res:
            alive += 1
        print("  %-42s %s" % (short[:42], res))

    print()
    if alive:
        print("[✓] 至少一条 v2 通道有应答 => 探针主循环活着, 只有 HID 端点坏了。")
        print("    下一步可以用这条通道直接读寄存器, 不必等烧录。")
        return 0
    print("[X] 所有通道都无应答 => 探针固件确实卡死, 必须断电复位或换调试器。")
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
