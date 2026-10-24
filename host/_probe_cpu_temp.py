#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
临时探测脚本（不是交付物）：验证两件事
  1) ctypes 直接调 pdh.dll 读 '\\Thermal Zone Information(*)\\Temperature' 是否可行、速度如何
  2) 该温度是否随 CPU 负载变化（不变 => 是假的/固定值，不能用）
同时探测 PDH 里是否存在内存温度类计数器。
"""
import ctypes as C
import ctypes.wintypes as W
import multiprocessing as mp
import time
import sys
import os

pdh = C.WinDLL("pdh.dll")

# 所有 PDH 函数一律按无符号返回 (0x800007D2 = MORE_DATA, 有符号解释会变负数)
pdh.PdhOpenQueryW.argtypes = [W.LPCWSTR, C.c_void_p, C.POINTER(C.c_void_p)]
pdh.PdhOpenQueryW.restype = C.c_uint32
pdh.PdhAddEnglishCounterW.argtypes = [C.c_void_p, W.LPCWSTR, C.c_void_p, C.POINTER(C.c_void_p)]
pdh.PdhAddEnglishCounterW.restype = C.c_uint32
pdh.PdhCollectQueryData.argtypes = [C.c_void_p]
pdh.PdhCollectQueryData.restype = C.c_uint32
pdh.PdhGetFormattedCounterArrayW.argtypes = [
    C.c_void_p, W.DWORD, C.POINTER(W.DWORD), C.POINTER(W.DWORD), C.c_void_p]
pdh.PdhGetFormattedCounterArrayW.restype = C.c_uint32
pdh.PdhCloseQuery.argtypes = [C.c_void_p]
pdh.PdhCloseQuery.restype = C.c_uint32
pdh.PdhEnumObjectsW.restype = C.c_uint32

PDH_FMT_DOUBLE = 0x00000200
PDH_FMT_NOCAP100 = 0x00008000
PDH_MORE_DATA = 0x800007D2


class PDH_FMT_COUNTERVALUE(C.Structure):
    # 真实定义: DWORD CStatus; union { LONG/double/LONGLONG ... }
    # => CStatus 之后有 4 字节 padding, double 在 offset 8
    _fields_ = [
        ("CStatus", W.DWORD),
        ("doubleValue", C.c_double),
    ]


class PDH_FMT_COUNTERVALUE_ITEM_W(C.Structure):
    _fields_ = [
        ("szName", W.LPWSTR),
        ("FmtValue", PDH_FMT_COUNTERVALUE),
    ]


def _double_from(fv):
    return fv.doubleValue


class PdhCounter(object):
    def __init__(self, path_english):
        self.hq = C.c_void_p()
        rc = pdh.PdhOpenQueryW(None, C.c_void_p(0), C.byref(self.hq))
        if rc != 0:
            raise RuntimeError("PdhOpenQuery rc=0x%08X" % rc)
        self.hc = C.c_void_p()
        # 用 English 版避免中文系统计数器名本地化问题
        rc = pdh.PdhAddEnglishCounterW(self.hq, path_english,
                                       C.c_void_p(0), C.byref(self.hc))
        if rc != 0:
            raise RuntimeError("PdhAddEnglishCounter rc=0x%08X" % rc)
        self._collect()  # 首次采集（建立基线）

    def _collect(self):
        return pdh.PdhCollectQueryData(self.hq)

    def read_array(self):
        """返回 [(instance_name, value), ...]"""
        self._collect()
        size = W.DWORD(0)
        count = W.DWORD(0)
        rc = pdh.PdhGetFormattedCounterArrayW(
            self.hc, PDH_FMT_DOUBLE, C.byref(size), C.byref(count), None)
        if rc != PDH_MORE_DATA and rc != 0:
            raise RuntimeError("size query rc=0x%08X" % rc)
        if size.value == 0:
            return []
        buf = (C.c_ubyte * size.value)()
        rc = pdh.PdhGetFormattedCounterArrayW(
            self.hc, PDH_FMT_DOUBLE, C.byref(size), C.byref(count), buf)
        if rc != 0:
            raise RuntimeError("read rc=0x%08X" % rc)
        items = C.cast(buf, C.POINTER(PDH_FMT_COUNTERVALUE_ITEM_W))
        out = []
        for i in range(count.value):
            it = items[i]
            out.append((it.szName, _double_from(it.FmtValue)))
        return out
    def close(self):
        if self.hq:
            pdh.PdhCloseQuery(self.hq)


def burn(stop_flag, n):
    x = 0
    while not stop_flag.is_set():
        for i in range(200000):
            x = (x * 1103515245 + 12345) & 0x7FFFFFFF


def main():
    print("=== A) 打开 PDH 计数器 ===")
    t0 = time.perf_counter()
    try:
        c = PdhCounter(r"\Thermal Zone Information(*)\Temperature")
    except Exception as e:
        print("  FAILED:", e)
        return 1
    print("  OK, 打开耗时 %.1f ms" % ((time.perf_counter() - t0) * 1000))

    print("\n=== B) 单次读取耗时 ===")
    for _ in range(3):
        t0 = time.perf_counter()
        vals = c.read_array()
        dt = (time.perf_counter() - t0) * 1000
        shown = ", ".join("%s=%.2fK(%.1fC)" % (n, v, v - 273.15) for n, v in vals)
        print("  %.2f ms  ->  %s" % (dt, shown if shown else "(空)"))

    print("\n=== C) 空载基线（5 秒）===")
    idle = []
    for _ in range(5):
        v = c.read_array()
        if v:
            idle.append(v[0][1] - 273.15)
        time.sleep(1)
    print("  空载温度: %s" % ["%.1f" % x for x in idle])

    print("\n=== D) 满载（6 进程 x 8 秒）观察温度是否上升 ===")
    stop = mp.Event()
    procs = [mp.Process(target=burn, args=(stop, i)) for i in range(6)]
    for p in procs:
        p.start()
    loaded = []
    for _ in range(8):
        time.sleep(1)
        v = c.read_array()
        if v:
            loaded.append(v[0][1] - 273.15)
            print("    +%ds  %.1f C" % (len(loaded), loaded[-1]))
    stop.set()
    for p in procs:
        p.join(timeout=3)

    c.close()

    print("\n=== E) 结论 ===")
    if idle and loaded:
        di = max(idle)
        dl = max(loaded)
        print("  空载最高 %.1f C, 满载最高 %.1f C, 差 %+.1f C" % (di, dl, dl - di))
        if dl - di >= 3.0:
            print("  => 温度随负载变化, 是真实 CPU 温度读数, 可用")
        else:
            print("  => 几乎不变, 可能是 ACPI 固定值/主板热区, 不能当 CPU 温度用")
    else:
        print("  读数不稳定, 无法判断")

    print("\n=== F) PDH 里有没有内存/DIMM 温度类计数器 ===")
    # 用 PdhEnumObjects 太啰嗦，直接扫一遍所有 counter 名（英文）
    pdh.PdhEnumObjectsW.restype = C.c_uint32
    buf_len = W.DWORD(0)
    rc = pdh.PdhEnumObjectsW(None, None, None, C.byref(buf_len), 0, 1)  # 1 = explain
    if rc in (0, 0x800007D2):
        buf = C.create_unicode_buffer(buf_len.value)
        rc = pdh.PdhEnumObjectsW(None, None, buf, C.byref(buf_len), 0, 1)
        if rc in (0, 0x800007D2):
            txt = buf[:buf_len.value]
            names = txt.split("\0")
            hits = [n for n in names if n and (
                "thermal" in n.lower() or "mem" in n.lower()
                and "temp" in n.lower() or "dim" in n.lower())]
            print("  相关对象名: %s" % (hits if hits else "(无)"))
            print("  对象总数: %d" % len([n for n in names if n]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
