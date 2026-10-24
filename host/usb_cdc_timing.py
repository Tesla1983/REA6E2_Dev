#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把「打开/关闭 COM 口各卡 30 s」拆到 Win32 API 级，定位到底哪一步在等。

背景
----
RA6E2 的 USB CDC（COM15）能枚举、能收数据，但 open/close 各卡整齐的 30.0 s。
pyserial 的 Serial()/close() 是复合动作，看不出卡在哪个 Win32 调用上。
本脚本绕过 pyserial，逐个 API 计时：

    CreateFile → GetCommState → SetCommState → SetupComm → PurgeComm
    → EscapeCommFunction(SETDTR) → GetCommModemStatus → WriteFile
    → EscapeCommFunction(CLRDTR) → EscapeCommFunction(CLRRTS) → CloseHandle

用法
----
    python host/usb_cdc_timing.py            # 自动找 045B:0263
    python host/usb_cdc_timing.py --port COM15
    python host/usb_cdc_timing.py --no-dtr   # 跳过所有 EscapeCommFunction（对照用）

判定
----
- 卡在 CreateFile            ⇒ 设备/驱动层的 open 本身有问题
- 卡在 SetCommState          ⇒ SET_LINE_CODING 没被设备 ACK
- 卡在 EscapeCommFunction    ⇒ SET_CONTROL_LINE_STATE 没被设备 ACK（最可能）
- 卡在 CloseHandle           ⇒ 同上，只是发生在关端口时的 DTR 拉低
"""

import argparse
import ctypes
import os
import sys
import time
from ctypes import wintypes

k32 = ctypes.WinDLL("kernel32", use_last_error=True)

GENERIC_READ_WRITE = 0xC0000000
OPEN_EXISTING = 3
FILE_FLAG_OVERLAPPED = 0x40000000
FILE_ATTRIBUTE_NORMAL = 0x80

SETDTR, CLRDTR = 5, 6
SETRTS, CLRRTS = 3, 4
SETXOFF, SETXON = 1, 2

PURGE_TXCLEAR = 0x0004
PURGE_RXCLEAR = 0x0008

INVALID_HANDLE = ctypes.c_void_p(-1).value


class DCB(ctypes.Structure):
    _fields_ = [
        ("DCBlength", wintypes.DWORD),
        ("BaudRate", wintypes.DWORD),
        ("fBits", wintypes.DWORD),   # 位域打包成 1 个 DWORD（够用于 SetCommState 往返）
        ("wReserved", wintypes.WORD),
        ("XonLim", wintypes.WORD),
        ("XoffLim", wintypes.WORD),
        ("ByteSize", wintypes.BYTE),
        ("Parity", wintypes.BYTE),
        ("StopBits", wintypes.BYTE),
        ("XonChar", ctypes.c_char),
        ("XoffChar", ctypes.c_char),
        ("ErrorChar", ctypes.c_char),
        ("EofChar", ctypes.c_char),
        ("EvtChar", ctypes.c_char),
        ("wReserved1", wintypes.WORD),
    ]


class OVERLAPPED(ctypes.Structure):
    _fields_ = [
        ("Internal", ctypes.c_void_p),
        ("InternalHigh", ctypes.c_void_p),
        ("Offset", wintypes.DWORD),
        ("OffsetHigh", wintypes.DWORD),
        ("hEvent", wintypes.HANDLE),
    ]


k32.CreateFileW.restype = wintypes.HANDLE
k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                            ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
k32.CloseHandle.restype = wintypes.BOOL
k32.GetCommState.argtypes = [wintypes.HANDLE, ctypes.POINTER(DCB)]
k32.SetCommState.argtypes = [wintypes.HANDLE, ctypes.POINTER(DCB)]
k32.GetCommTimeouts.argtypes = [wintypes.HANDLE, ctypes.c_void_p]
k32.SetCommTimeouts.argtypes = [wintypes.HANDLE, ctypes.c_void_p]
k32.SetupComm.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD]
k32.PurgeComm.argtypes = [wintypes.HANDLE, wintypes.DWORD]
k32.EscapeCommFunction.argtypes = [wintypes.HANDLE, wintypes.DWORD]
k32.GetCommModemStatus.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
k32.WriteFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                          ctypes.POINTER(wintypes.DWORD), ctypes.POINTER(OVERLAPPED)]
k32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
k32.GetOverlappedResult.argtypes = [wintypes.HANDLE, ctypes.POINTER(OVERLAPPED),
                                    ctypes.POINTER(wintypes.DWORD), wintypes.BOOL]


def fmt(sec):
    if sec >= 5.0:
        return f"{sec:7.2f} s   <<<<<< 卡在这里"
    if sec >= 0.5:
        return f"{sec:7.2f} s   <<<< 偏慢"
    return f"{sec:7.3f} s"


class Stopwatch:
    def __init__(self):
        self.rows = []

    def run(self, name, fn):
        t0 = time.perf_counter()
        try:
            val = fn()
            ok = True
            err = ""
        except Exception as e:                                   # noqa: BLE001
            val = None
            ok = False
            err = f"{type(e).__name__}: {e}"
        dt = time.perf_counter() - t0
        self.rows.append((name, dt, ok, err, val))
        return val if ok else None

    def dump(self):
        width = max(len(r[0]) for r in self.rows)
        print(f"\n{'步骤':<{width}}  耗时")
        print("-" * (width + 34))
        slow = []
        for name, dt, ok, err, _v in self.rows:
            line = f"{name:<{width}}  {fmt(dt)}"
            if not ok:
                line += f"  [异常 {err}]"
            print(line)
            if dt >= 5.0:
                slow.append((name, dt))
        return slow


def find_port(vidpid=None):
    """按 VID:PID 找串口。默认找 RA6E2 的 045B:0263。"""
    try:
        from serial.tools import list_ports as lp
    except Exception:                                             # noqa: BLE001
        return None
    want = None
    if vidpid:
        want = (int(vidpid.split(":")[0], 16), int(vidpid.split(":")[1], 16))
    for p in lp.comports():
        if want and p.vid == want[0] and p.pid == want[1]:
            return p.device
        if not want and p.vid == 0x045B and p.pid == 0x0263:
            return p.device
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None, help="COM 口（默认自动找 045B:0263）")
    ap.add_argument("--vidpid", default=None,
                    help="按 VID:PID 找口（如 303a:1001），用于拿别的 CDC 设备做对照")
    ap.add_argument("--no-dtr", action="store_true", help="跳过所有 EscapeCommFunction")
    ap.add_argument("--no-setstate", action="store_true", help="跳过 SetCommState")
    ap.add_argument("--only-dtr", action="store_true",
                    help="只做 SETDTR 一个动作（用来把设备侧事件和主机侧操作一一对应）")
    ap.add_argument("--minimal", action="store_true",
                    help="只做 CreateFileW + SETDTR + CloseHandle 三个动作")
    args = ap.parse_args()
    if args.minimal:
        # 只保留 CreateFileW + SETDTR + CloseHandle 三个动作
        args.only_dtr = True
        args.no_setstate = True

    port = args.port or find_port(args.vidpid)
    if not port:
        print("找不到目标串口。用 --port 或 --vidpid 指定。")
        return 2
    print(f"目标端口: {port}")

    path = "\\\\.\\" + port
    sw = Stopwatch()

    def do_create():
        h = k32.CreateFileW(path, GENERIC_READ_WRITE, 0, None, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, None)
        if h == INVALID_HANDLE or h is None:
            raise OSError(ctypes.get_last_error(), "CreateFileW 失败")
        return h

    h = sw.run("CreateFileW (打开端口)", do_create)
    if h is None:
        sw.dump()
        return 3

    dcb = DCB()
    dcb.DCBlength = ctypes.sizeof(DCB)
    if not args.minimal:
        sw.run("GetCommState", lambda: (k32.GetCommState(h, ctypes.byref(dcb)) or
                                        (_ for _ in ()).throw(OSError(ctypes.get_last_error()))))
        print(f"    读到波特率 = {dcb.BaudRate}  ByteSize={dcb.ByteSize} "
              f"Parity={dcb.Parity} StopBits={dcb.StopBits}")

    if not args.no_setstate and not args.minimal:
        dcb.BaudRate = 115200
        dcb.ByteSize = 8
        dcb.Parity = 0
        dcb.StopBits = 0
        sw.run("SetCommState (SET_LINE_CODING)",
               lambda: (k32.SetCommState(h, ctypes.byref(dcb)) or
                        (_ for _ in ()).throw(OSError(ctypes.get_last_error()))))

    if not args.minimal:
        to = (wintypes.DWORD * 5)(0xFFFFFFFF, 0, 0, 0, 0)
        sw.run("SetCommTimeouts",
               lambda: (k32.SetCommTimeouts(h, ctypes.byref(to)) or
                        (_ for _ in ()).throw(OSError(ctypes.get_last_error()))))
        sw.run("SetupComm", lambda: (k32.SetupComm(h, 4096, 4096) or
                                     (_ for _ in ()).throw(OSError(ctypes.get_last_error()))))
        sw.run("PurgeComm",
               lambda: (k32.PurgeComm(h, PURGE_TXCLEAR | PURGE_RXCLEAR) or
                        (_ for _ in ()).throw(OSError(ctypes.get_last_error()))))

    if not args.no_dtr:
        sw.run("EscapeCommFunction(SETDTR)",
               lambda: (k32.EscapeCommFunction(h, SETDTR) or
                        (_ for _ in ()).throw(OSError(ctypes.get_last_error()))))
        if not args.only_dtr:
            sw.run("EscapeCommFunction(SETRTS)",
                   lambda: (k32.EscapeCommFunction(h, SETRTS) or
                            (_ for _ in ()).throw(OSError(ctypes.get_last_error()))))

    if not args.minimal:
        mst = wintypes.DWORD(0)
        sw.run("GetCommModemStatus", lambda: (k32.GetCommModemStatus(h, ctypes.byref(mst)) or
                                              (_ for _ in ()).throw(OSError(ctypes.get_last_error()))))

    payload = b"PING\n"
    ev = k32.CreateEventW(None, True, False, None)
    ov = OVERLAPPED()
    ov.hEvent = ev
    written = wintypes.DWORD(0)
    buf = ctypes.create_string_buffer(payload)

    def do_write():
        ret = k32.WriteFile(h, buf, len(payload), ctypes.byref(written), ctypes.byref(ov))
        if not ret:
            e = ctypes.get_last_error()
            if e != 997:                                          # ERROR_IO_PENDING
                raise OSError(e, "WriteFile 失败")
            k32.WaitForSingleObject(ev, 4000)
        return written.value

    if not args.minimal:
        sw.run("WriteFile (PING)", do_write)

    if not args.no_dtr and not args.only_dtr:
        sw.run("EscapeCommFunction(CLRDTR)",
               lambda: (k32.EscapeCommFunction(h, CLRDTR) or
                        (_ for _ in ()).throw(OSError(ctypes.get_last_error()))))
        sw.run("EscapeCommFunction(CLRRTS)",
               lambda: (k32.EscapeCommFunction(h, CLRRTS) or
                        (_ for _ in ()).throw(OSError(ctypes.get_last_error()))))

    sw.run("CloseHandle (关闭端口)",
           lambda: (k32.CloseHandle(h) or
                    (_ for _ in ()).throw(OSError(ctypes.get_last_error()))))

    slow = sw.dump()
    print()
    if slow:
        print("结论: 卡点在")
        for n, d in slow:
            print(f"  - {n}  ({d:.2f} s)")
        print("\n对照实验:")
        print("  python host/usb_cdc_timing.py --no-dtr    # DTR/RTS 全跳过")
        print("  python host/usb_cdc_timing.py --no-setstate")
    else:
        print("结论: 每一步都在 5 s 以内 —— 30 s 不在这些 API 上，"
              "问题在更上层（pyserial 的读超时或 usbser 的等待掩码）。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
