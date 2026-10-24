#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""按【板子身份】识别开发板，并检查归属项目。

为什么要有这个工具（2026-10-23 事故后建立）:
    曾经用 **COM 号** 来认板子。COM 号是枚举时动态分配的，
    同一块 Pico 的 CDC 从 COM4 变成过 COM14 ⇒ 认错板子的概率极高。
    更严重的一次：把 E:/RP2350_Dev 项目的 RP2350（Pico 2）当成"本项目的调试器"，
    直接刷成了 debugprobe，把对方项目的 IMU 固件覆盖掉了。

本工具的规矩:
    1. 只认不可变身份 —— USB **VID:PID + 序列号**，或芯片 UID；
    2. **不读取、不显示、不使用 COM 号**（它不稳定，不是身份）；
    3. 认不出来的设备一律报"未登记"，不猜；
    4. 板子归属 ≠ 当前项目时，明确告警（默认只告警，--strict 直接失败）。

用法:
    python tools/board_id.py                  # 列出当前插着的板子 + 归属
    python tools/board_id.py --strict         # 有"不属于本项目的板子"就返回非 0
    python tools/board_id.py --require ra6e2-dqx    # 烧录前硬闸门：必须插着并匹配
"""
from __future__ import annotations

import argparse
import ctypes
import json
import sys
from ctypes import wintypes as wt
from pathlib import Path

# ----------------------------------------------------------------------------
# Windows SetupAPI：枚举【当前在位】的 USB 设备实例 ID
# ----------------------------------------------------------------------------
SETUPAPI = ctypes.WinDLL("setupapi", use_last_error=True)

DIGCF_PRESENT = 0x02
DIGCF_ALLCLASSES = 0x04
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value


class GUID(ctypes.Structure):
    _fields_ = [
        ("Data1", wt.DWORD), ("Data2", wt.WORD), ("Data3", wt.WORD),
        ("Data4", ctypes.c_ubyte * 8),
    ]


class SP_DEVINFO_DATA(ctypes.Structure):
    _fields_ = [
        ("cbSize", wt.DWORD),
        ("ClassGuid", GUID),
        ("DevInst", wt.DWORD),
        ("Reserved", ctypes.POINTER(wt.ULONG)),
    ]


SETUPAPI.SetupDiGetClassDevsW.argtypes = [ctypes.c_void_p, wt.LPCWSTR,
                                         ctypes.c_void_p, wt.DWORD]
SETUPAPI.SetupDiGetClassDevsW.restype = ctypes.c_void_p
SETUPAPI.SetupDiEnumDeviceInfo.argtypes = [ctypes.c_void_p, wt.DWORD,
                                           ctypes.POINTER(SP_DEVINFO_DATA)]
SETUPAPI.SetupDiEnumDeviceInfo.restype = wt.BOOL
SETUPAPI.SetupDiGetDeviceInstanceIdW.argtypes = [
    ctypes.c_void_p, ctypes.POINTER(SP_DEVINFO_DATA), wt.LPWSTR, wt.DWORD,
    ctypes.POINTER(wt.DWORD)]
SETUPAPI.SetupDiGetDeviceInstanceIdW.restype = wt.BOOL
SETUPAPI.SetupDiDestroyDeviceInfoList.argtypes = [ctypes.c_void_p]
SETUPAPI.SetupDiDestroyDeviceInfoList.restype = wt.BOOL


def list_present_usb() -> list[str]:
    """返回当前在位的 USB 设备实例 ID，形如 USB\\VID_2E8A&PID_000C\\BAD9..."""
    h = SETUPAPI.SetupDiGetClassDevsW(None, "USB", None,
                                      DIGCF_PRESENT | DIGCF_ALLCLASSES)
    if not h or h == INVALID_HANDLE_VALUE:
        raise OSError("SetupDiGetClassDevsW 失败, err=%d" % ctypes.get_last_error())

    ids: list[str] = []
    try:
        i = 0
        while True:
            dev = SP_DEVINFO_DATA()
            dev.cbSize = ctypes.sizeof(SP_DEVINFO_DATA)
            if not SETUPAPI.SetupDiEnumDeviceInfo(h, i, ctypes.byref(dev)):
                break
            i += 1
            need = wt.DWORD(0)
            SETUPAPI.SetupDiGetDeviceInstanceIdW(h, ctypes.byref(dev), None, 0,
                                                 ctypes.byref(need))
            if need.value <= 1:
                continue
            buf = ctypes.create_unicode_buffer(need.value)
            if SETUPAPI.SetupDiGetDeviceInstanceIdW(h, ctypes.byref(dev), buf,
                                                    need.value, ctypes.byref(need)):
                ids.append(buf.value)
    finally:
        SETUPAPI.SetupDiDestroyDeviceInfoList(h)
    return ids


def parse_instance(instance_id: str):
    """USB\\VID_2E8A&PID_000C\\BAD9F7B2EDDE496E -> (vid, pid, serial)；
    非 USB 复合形式（带 &MI_xx）返回 None —— 那是接口节点，不是板子本身。"""
    parts = instance_id.split("\\")
    if len(parts) != 3 or parts[0].upper() != "USB":
        return None
    hwid, serial = parts[1], parts[2]
    if "&MI_" in hwid.upper():          # 接口节点，跳过
        return None
    if not hwid.upper().startswith("VID_") or "&PID_" not in hwid.upper():
        return None
    vid = hwid[4:8].lower()
    pid = hwid.split("&PID_")[1][:4].lower()
    return vid, pid, serial


# ----------------------------------------------------------------------------
# 登记表
# ----------------------------------------------------------------------------
def load_registry(path: Path) -> list[dict]:
    data = json.loads(path.read_text(encoding="utf-8"))
    return data.get("boards", [])


def match(board: dict, vid: str, pid: str, serial: str) -> bool:
    """identity.usb 允许是单个对象，也允许是**列表**——同一块板子在
    不同固件下 VID:PID 会变（RP2350: IMU 固件 2e8a:0009 / debugprobe 2e8a:000c），
    但序列号（芯片 UID）不变。列表里任意一项匹配即算这块板子。"""
    usb = board.get("identity", {}).get("usb") or {}
    candidates = usb if isinstance(usb, list) else [usb]
    for u in candidates:
        if not u:
            continue
        if u.get("vid", "").lower() != vid or u.get("pid", "").lower() != pid:
            continue
        want = u.get("serial")
        if want and want.lower() != serial.lower():
            continue
        return True
    return False


def main() -> int:
    here = Path(__file__).resolve().parent
    proj = here.parent
    reg_path = here / "boards.json"

    ap = argparse.ArgumentParser(description="按板子身份（VID:PID + 序列号）识别开发板")
    ap.add_argument("--registry", default=str(reg_path))
    ap.add_argument("--strict", action="store_true",
                    help="存在不属于本项目的板子时返回非 0")
    ap.add_argument("--require", metavar="BOARD_ID",
                    help="要求指定 board_id 必须当前在位（烧录前闸门）")
    args = ap.parse_args()

    boards = load_registry(Path(args.registry))
    present = [t for t in (parse_instance(x) for x in list_present_usb()) if t]

    print(f"当前项目: {proj.as_posix()}")
    print(f"登记表  : {Path(args.registry).as_posix()}")
    print()
    print(f"{'身份（VID:PID / 序列号）':<54} {'归属':<20} {'角色':<26} 判定")
    print("-" * 124)

    unknown, foreign, found = [], [], set()
    for vid, pid, serial in sorted(present):
        ident = f"{vid}:{pid} / {serial}"
        hit = next((b for b in boards if match(b, vid, pid, serial)), None)
        if hit is None:
            unknown.append(ident)
            continue
        found.add(hit["id"])
        owner = hit.get("owning_project", "?")
        allowed = hit.get("allowed_in")
        if allowed is None:
            ok = owner.replace("/", "\\").lower() == str(proj).replace("/", "\\").lower()
        else:
            ok = any(Path(a).as_posix().lower() == proj.as_posix().lower() for a in allowed)
        if not ok:
            foreign.append(hit["id"])
        verdict = "✓ 本项目" if ok else "✗ 属于别的项目，不要动它"
        print(f"{ident:<54} {owner:<20} {hit.get('role', ''):<26} {verdict}")

    if not found:
        print("（没有识别到已登记的板子）")
    print()
    if args.require:
        if args.require in found:
            print(f"[闸门] ✓ 要求的目标板 {args.require} 在位")
        else:
            print(f"[闸门] ✗ 要求的目标板 {args.require} **不在位** —— 停止操作")
            return 2

    if foreign:
        print("[告警] 下面这些板子属于**其它项目**，不要给它烧本项目的固件：")
        for bid in foreign:
            b = next(x for x in boards if x["id"] == bid)
            print(f"        - {bid}：{b.get('label')}（归属 {b.get('owning_project')}）")
            if b.get("restore_firmware"):
                print(f"          如需恢复原状：{b['restore_firmware']}")
    if unknown:
        print(f"[未登记] {len(unknown)} 个 USB 设备不在登记表里（多半是集线器/键盘/串口芯片，")
        print("         但也可能是没入册的板子）—— 要用它之前先确认它的归属，别默认它是本项目的：")
        for u in unknown:
            print(f"         · {u}")

    print()
    print("[提醒] COM 号不是身份：每次枚举都可能变，禁止用 COM 号判断“这块板子是哪个”。")

    if args.strict and foreign:
        return 3
    return 0


if __name__ == "__main__":
    sys.exit(main())
