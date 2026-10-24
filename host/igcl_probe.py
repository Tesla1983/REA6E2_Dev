#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
igcl_probe.py —— Intel Arc A770 只读遥测探针（方案2: IGCL 官方库）

用途
   用 Intel 官方 IGCL（本机 C:\\Windows\\System32\\ControlLib.dll）读取：
     1) 显存占用（free / size）
     2) 温度传感器（GPU / Memory / Global 等）
     3) 风扇转速（RPM / PERCENT，以驱动支持为准）
   并通过命令行输出，供后续串口发给 RA6E2 显示。

安全边界（重要）
   * 只调用 Enum*/Get* 读接口；绝不调用任何 Set*（风扇控制 / 超频 / 功耗）
     —— 对显卡零写入，不会改变硬件状态。
   * 结构体定义严格对齐 igcl_api.h（IGCL 1.1 头 / 本机 DLL 1.3 实测）。
     运行时打印每个结构体的 sizeof，便于核对布局。

用法
   python igcl_probe.py                 # 读一次，人类可读
   python igcl_probe.py --json          # 读一次，输出 JSON（给上位机/串口用）
   python igcl_probe.py --loop 5 --interval 1.0
   python igcl_probe.py --sizes         # 只打印结构体大小（不触碰显卡）

退出码
   0 成功；2 库加载失败；3 ctlInit 失败；4 无设备；5 读遥测部分失败
"""

import ctypes as C
import sys
import time
import json
import argparse

# ----------------------------------------------------------------------------
# 常量
# ----------------------------------------------------------------------------
CTL_RESULT_SUCCESS = 0x00000000
APP_VERSION_1_1 = (1 << 16) | 1   # CTL_MAKE_VERSION(1,1)
APP_VERSION_1_3 = (1 << 16) | 3   # 本机 DLL 报 1.3，必要时回退尝试

# CTL_INIT_FLAG_USE_LEVEL_ZERO：头文件明确“遥测通常必须开启 Level Zero”
CTL_INIT_FLAG_USE_LEVEL_ZERO = 1 << 0
INIT_FLAGS = CTL_INIT_FLAG_USE_LEVEL_ZERO

RESULT_NAMES = {
    0x40000001: "ERROR_NOT_INITIALIZED",
    0x40000002: "ERROR_ALREADY_INITIALIZED",
    0x40000003: "ERROR_DEVICE_LOST",
    0x40000004: "ERROR_OUT_OF_HOST_MEMORY",
    0x40000005: "ERROR_OUT_OF_DEVICE_MEMORY",
    0x40000006: "ERROR_INSUFFICIENT_PERMISSIONS",
    0x40000007: "ERROR_NOT_AVAILABLE",
    0x40000008: "ERROR_UNINITIALIZED",
    0x40000009: "ERROR_UNSUPPORTED_VERSION",
    0x4000000A: "ERROR_UNSUPPORTED_FEATURE",
    0x4000000B: "ERROR_INVALID_ARGUMENT",
    0x4000000C: "ERROR_INVALID_API_HANDLE",
    0x4000000D: "ERROR_INVALID_NULL_HANDLE",
    0x4000000E: "ERROR_INVALID_NULL_POINTER",
    0x4000000F: "ERROR_INVALID_SIZE",
    0x40000010: "ERROR_UNSUPPORTED_SIZE",
    0x40000015: "ERROR_NOT_IMPLEMENTED",
    0x40000017: "ERROR_KMD_CALL",
    0x40000019: "ERROR_ZE_LOADER",
    0x40000021: "ERROR_UNKNOWN_APPLICATION_UID",
    0x40000024: "ERROR_RESET_DEVICE_REQUIRED",
    0x40000027: "ERROR_DEVICE_UNAVAILABLE",
    0x40000028: "ERROR_0x40000028(1.3新增; 实测=未开LevelZero)",
}

TEMP_SENSOR_NAMES = {
    0: "GLOBAL(max)",
    1: "GPU(max)",
    2: "MEMORY(max)",
    3: "GLOBAL(min)",
    4: "GPU(min)",
    5: "MEMORY(min)",
}

MEM_TYPE_NAMES = {
    0: "HBM", 1: "DDR", 2: "DDR3", 3: "DDR4", 4: "DDR5", 5: "LPDDR",
    6: "LPDDR3", 7: "LPDDR4", 8: "LPDDR5", 9: "GDDR4", 10: "GDDR5",
    11: "GDDR5X", 12: "GDDR6", 13: "GDDR6X", 14: "GDDR7", 15: "UNKNOWN",
}
MEM_LOC_NAMES = {0: "SYSTEM", 1: "DEVICE"}

FAN_UNIT_RPM = 0
FAN_UNIT_PERCENT = 1


def rc_name(rc):
    return RESULT_NAMES.get(rc, "0x%08X" % rc)


# ----------------------------------------------------------------------------
# 结构体（严格对齐 igcl_api.h）
# ----------------------------------------------------------------------------
class CtlApplicationId(C.Structure):
    _fields_ = [
        ("Data1", C.c_uint32),
        ("Data2", C.c_uint16),
        ("Data3", C.c_uint16),
        ("Data4", C.c_uint8 * 8),
    ]


class CtlInitArgs(C.Structure):
    _fields_ = [
        ("Size", C.c_uint32),
        ("Version", C.c_uint8),
        ("AppVersion", C.c_uint32),
        ("flags", C.c_uint32),
        ("SupportedVersion", C.c_uint32),
        ("ApplicationUID", CtlApplicationId),
    ]


class CtlTempProperties(C.Structure):
    _fields_ = [
        ("Size", C.c_uint32),
        ("Version", C.c_uint8),
        ("type", C.c_int32),          # ctl_temp_sensors_t
        ("maxTemperature", C.c_double),
    ]


class CtlFanProperties(C.Structure):
    _fields_ = [
        ("Size", C.c_uint32),
        ("Version", C.c_uint8),
        ("canControl", C.c_bool),
        ("supportedModes", C.c_uint32),
        ("supportedUnits", C.c_uint32),
        ("maxRPM", C.c_int32),
        ("maxPoints", C.c_int32),
    ]


class CtlMemProperties(C.Structure):
    _fields_ = [
        ("Size", C.c_uint32),
        ("Version", C.c_uint8),
        ("type", C.c_int32),          # ctl_mem_type_t
        ("location", C.c_int32),      # ctl_mem_loc_t
        ("physicalSize", C.c_uint64),
        ("busWidth", C.c_int32),
        ("numChannels", C.c_int32),
    ]


class CtlMemState(C.Structure):
    _fields_ = [
        ("Size", C.c_uint32),
        ("Version", C.c_uint8),
        ("free", C.c_uint64),
        ("size", C.c_uint64),
    ]


EXPECTED_SIZES = {
    "CtlApplicationId": 16,
    "CtlInitArgs": 36,
    "CtlTempProperties": 24,
    "CtlFanProperties": 24,
    "CtlMemProperties": 32,
    "CtlMemState": 24,
}


class IgclError(Exception):
    pass


# ----------------------------------------------------------------------------
# 包装
# ----------------------------------------------------------------------------
class Igcl(object):
    def __init__(self, dll_name="ControlLib.dll"):
        try:
            self.dll = C.WinDLL(dll_name)
        except OSError as e:
            raise IgclError("无法加载 %s: %s" % (dll_name, e))
        self._setup_proto()

    def _setup_proto(self):
        d = self.dll
        h = C.c_void_p

        d.ctlInit.argtypes = [C.POINTER(CtlInitArgs), C.POINTER(h)]
        d.ctlInit.restype = C.c_uint32

        d.ctlClose.argtypes = [h]
        d.ctlClose.restype = C.c_uint32

        d.ctlEnumerateDevices.argtypes = [h, C.POINTER(C.c_uint32), C.POINTER(h)]
        d.ctlEnumerateDevices.restype = C.c_uint32

        d.ctlEnumTemperatureSensors.argtypes = [h, C.POINTER(C.c_uint32), C.POINTER(h)]
        d.ctlEnumTemperatureSensors.restype = C.c_uint32
        d.ctlTemperatureGetProperties.argtypes = [h, C.POINTER(CtlTempProperties)]
        d.ctlTemperatureGetProperties.restype = C.c_uint32
        d.ctlTemperatureGetState.argtypes = [h, C.POINTER(C.c_double)]
        d.ctlTemperatureGetState.restype = C.c_uint32

        d.ctlEnumFans.argtypes = [h, C.POINTER(C.c_uint32), C.POINTER(h)]
        d.ctlEnumFans.restype = C.c_uint32
        d.ctlFanGetProperties.argtypes = [h, C.POINTER(CtlFanProperties)]
        d.ctlFanGetProperties.restype = C.c_uint32
        d.ctlFanGetState.argtypes = [h, C.c_int, C.POINTER(C.c_int32)]
        d.ctlFanGetState.restype = C.c_uint32

        d.ctlEnumMemoryModules.argtypes = [h, C.POINTER(C.c_uint32), C.POINTER(h)]
        d.ctlEnumMemoryModules.restype = C.c_uint32
        d.ctlMemoryGetProperties.argtypes = [h, C.POINTER(CtlMemProperties)]
        d.ctlMemoryGetProperties.restype = C.c_uint32
        d.ctlMemoryGetState.argtypes = [h, C.POINTER(CtlMemState)]
        d.ctlMemoryGetState.restype = C.c_uint32

    # --- 基础 ---
    def init(self):
        for ver in (APP_VERSION_1_1, APP_VERSION_1_3):
            args = CtlInitArgs()
            C.memset(C.byref(args), 0, C.sizeof(args))
            args.Size = C.sizeof(args)
            args.Version = 0
            args.AppVersion = ver
            args.flags = INIT_FLAGS
            h = C.c_void_p()
            rc = self.dll.ctlInit(C.byref(args), C.byref(h))
            if rc == CTL_RESULT_SUCCESS:
                self.h_api = h
                self.supported_version = args.SupportedVersion
                self.app_version = ver
                return h
            if rc != 0x40000009:  # 不是 UNSUPPORTED_VERSION 就直接报错
                raise IgclError("ctlInit 失败: %s (AppVersion=0x%08X)"
                                % (rc_name(rc), ver))
        raise IgclError("ctlInit 版本被拒: UNSUPPORTED_VERSION (both 1.1/1.3)")

    def close(self):
        if getattr(self, "h_api", None):
            self.dll.ctlClose(self.h_api)
            self.h_api = None

    def enum(self, fn, handle):
        count = C.c_uint32(0)
        rc = fn(handle, C.byref(count), None)
        if rc != CTL_RESULT_SUCCESS:
            raise IgclError("枚举计数失败: %s" % rc_name(rc))
        n = count.value
        if n == 0:
            return []
        arr = (C.c_void_p * n)()
        rc = fn(handle, C.byref(count), arr)
        if rc != CTL_RESULT_SUCCESS:
            raise IgclError("枚举句柄失败: %s" % rc_name(rc))
        return [arr[i] for i in range(count.value)]

    # --- 读设备 ---
    def read_device(self, dev):
        out = {"temperatures": [], "fans": [], "memory": []}
        errors = []

        # 温度
        try:
            for h in self.enum(self.dll.ctlEnumTemperatureSensors, dev):
                p = CtlTempProperties()
                C.memset(C.byref(p), 0, C.sizeof(p))
                p.Size = C.sizeof(p)
                p.Version = 0
                rc_p = self.dll.ctlTemperatureGetProperties(h, C.byref(p))
                val = C.c_double(0.0)
                rc_v = self.dll.ctlTemperatureGetState(h, C.byref(val))
                out["temperatures"].append({
                    "type": p.type if rc_p == 0 else None,
                    "type_name": TEMP_SENSOR_NAMES.get(p.type, "?") if rc_p == 0 else "?",
                    "max_temperature": round(p.maxTemperature, 2) if rc_p == 0 else None,
                    "celsius": round(val.value, 2) if rc_v == 0 else None,
                    "rc_props": rc_p, "rc_state": rc_v,
                })
        except IgclError as e:
            errors.append("temperature: %s" % e)

        # 风扇
        try:
            for h in self.enum(self.dll.ctlEnumFans, dev):
                p = CtlFanProperties()
                C.memset(C.byref(p), 0, C.sizeof(p))
                p.Size = C.sizeof(p)
                p.Version = 0
                rc_p = self.dll.ctlFanGetProperties(h, C.byref(p))
                rpm = C.c_int32(-1)
                rc_rpm = self.dll.ctlFanGetState(h, FAN_UNIT_RPM, C.byref(rpm))
                pct = C.c_int32(-1)
                rc_pct = self.dll.ctlFanGetState(h, FAN_UNIT_PERCENT, C.byref(pct))
                out["fans"].append({
                    "can_control": bool(p.canControl) if rc_p == 0 else None,
                    "max_rpm": p.maxRPM if rc_p == 0 else None,
                    "rpm": rpm.value if rc_rpm == 0 else None,
                    "percent": pct.value if rc_pct == 0 else None,
                    "rc_props": rc_p, "rc_rpm": rc_rpm, "rc_pct": rc_pct,
                })
        except IgclError as e:
            errors.append("fan: %s" % e)

        # 显存
        try:
            for h in self.enum(self.dll.ctlEnumMemoryModules, dev):
                p = CtlMemProperties()
                C.memset(C.byref(p), 0, C.sizeof(p))
                p.Size = C.sizeof(p)
                p.Version = 0
                rc_p = self.dll.ctlMemoryGetProperties(h, C.byref(p))
                st = CtlMemState()
                C.memset(C.byref(st), 0, C.sizeof(st))
                st.Size = C.sizeof(st)
                st.Version = 0
                rc_s = self.dll.ctlMemoryGetState(h, C.byref(st))
                used = None
                pct_used = None
                if rc_s == 0 and st.size:
                    used = st.size - st.free
                    pct_used = round(100.0 * used / st.size, 1)
                out["memory"].append({
                    "type": p.type if rc_p == 0 else None,
                    "type_name": MEM_TYPE_NAMES.get(p.type, "?") if rc_p == 0 else "?",
                    "location": p.location if rc_p == 0 else None,
                    "location_name": MEM_LOC_NAMES.get(p.location, "?") if rc_p == 0 else "?",
                    "physical_size": p.physicalSize if rc_p == 0 else None,
                    "bus_width": p.busWidth if rc_p == 0 else None,
                    "size": st.size if rc_s == 0 else None,
                    "free": st.free if rc_s == 0 else None,
                    "used": used,
                    "used_percent": pct_used,
                    "rc_props": rc_p, "rc_state": rc_s,
                })
        except IgclError as e:
            errors.append("memory: %s" % e)

        if errors:
            out["errors"] = errors
        return out

    def read_all(self):
        devices = self.enum(self.dll.ctlEnumerateDevices, self.h_api)
        result = []
        for i, dev in enumerate(devices):
            result.append({"device_index": i, "data": self.read_device(dev)})
        return result


# ----------------------------------------------------------------------------
# 输出
# ----------------------------------------------------------------------------
def humansize(n):
    if n is None:
        return "?"
    units = ["B", "KiB", "MiB", "GiB", "TiB"]
    f = float(n)
    for u in units:
        if f < 1024.0 or u == units[-1]:
            return "%.2f %s" % (f, u) if u != "B" else "%d B" % n
        f /= 1024.0


def print_report(infos):
    for dev in infos:
        d = dev["data"]
        print("=== Device #%d ===" % dev["device_index"])
        mem = [m for m in d["memory"] if m.get("location") == 1] or d["memory"]
        if mem:
            for m in mem:
                print("  显存  [%s %s] %s / %s used (%.1f%%)  size=%s free=%s"
                      % (m["type_name"], m["location_name"],
                         humansize(m.get("used")), humansize(m.get("size")),
                         m["used_percent"] if m["used_percent"] is not None else -1,
                         humansize(m.get("size")), humansize(m.get("free"))))
        else:
            print("  显存  : 无 memory 模块")
        if d["temperatures"]:
            for t in d["temperatures"]:
                maxt = t["max_temperature"]
                maxt_s = ("%.1f" % maxt) if (maxt is not None and maxt > 0) else "N/A"
                print("  温度  [%s] %s C  (max %s C)"
                      % (t["type_name"],
                         ("%.2f" % t["celsius"]) if t["celsius"] is not None else "N/A",
                         maxt_s))
        else:
            print("  温度  : 无传感器")
        if d["fans"]:
            for f in d["fans"]:
                print("  风扇  rpm=%s  percent=%s  max_rpm=%s  can_control=%s"
                      % (f["rpm"] if f["rpm"] is not None else "N/A",
                         f["percent"] if f["percent"] is not None else "N/A",
                         f["max_rpm"] if f["max_rpm"] not in (None, 0) else "N/A",
                         f["can_control"]))
        else:
            print("  风扇  : 无风扇传感器")
        if "errors" in d:
            for e in d["errors"]:
                print("  [部分失败] %s" % e)


# ----------------------------------------------------------------------------
# main
# ----------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="Intel Arc A770 IGCL 只读遥测")
    ap.add_argument("--json", action="store_true", help="输出 JSON")
    ap.add_argument("--loop", type=int, default=0, help="循环次数(0=只读一次)")
    ap.add_argument("--interval", type=float, default=1.0, help="循环间隔秒")
    ap.add_argument("--sizes", action="store_true", help="只打印结构体大小并退出")
    args = ap.parse_args()

    if args.sizes:
        ok = True
        for name, exp in EXPECTED_SIZES.items():
            got = C.sizeof(globals()[name])
            flag = "OK" if got == exp else "MISMATCH"
            if got != exp:
                ok = False
            print("%-22s sizeof=%-3d expect=%-3d %s" % (name, got, exp, flag))
        print("SIZES_OK" if ok else "SIZES_BAD")
        return 0 if ok else 1

    try:
        igcl = Igcl()
    except IgclError as e:
        print("[FAIL] 加载: %s" % e, file=sys.stderr)
        return 2

    try:
        try:
            igcl.init()
        except IgclError as e:
            print("[FAIL] ctlInit: %s" % e, file=sys.stderr)
            return 3
        print("[OK] IGCL init, supportedVersion=0x%08X (app=0x%08X)"
              % (igcl.supported_version, igcl.app_version), file=sys.stderr)

        runs = args.loop if args.loop > 0 else 1
        for i in range(runs):
            infos = igcl.read_all()
            if not infos:
                print("[FAIL] 未枚举到任何设备", file=sys.stderr)
                return 4
            if args.json:
                print(json.dumps(infos, ensure_ascii=False, indent=2))
            else:
                print_report(infos)
            if i + 1 < runs:
                time.sleep(args.interval)
        return 0
    finally:
        igcl.close()


if __name__ == "__main__":
    sys.exit(main())
