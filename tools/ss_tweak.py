#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""在线改 RA6E2 固件里 USB CDC 的 SERIAL_STATE 行为，然后立刻测 DTR 耗时。

为什么需要这个
--------------
"发一次 SERIAL_STATE 通知"没消掉 Windows usbser 的 30 s（DTR 变化时）。
到底是**时机**不对还是**位图内容**不对，光靠猜要反复烧录。
所以固件里把这几个变量做成了运行时可改（见 src/usb/bsp_usb_cdc.c 的说明）：

    g_usb_ss_mode       bit0=burst  bit1=固定位图  bit2=持续模式  bit3=忽略 busy
    g_usb_ss_bitmap     要发的 UART 状态位图（bit0=bDCD bit1=bDSR）
    g_usb_ss_burst_n    burst 次数
    g_usb_ss_gap_ticks  间隔（Poll 次数，1 次 ≈ 2 ms）

本工具用探针直接改 RAM（`probe-rs write`，只支持 RAM 地址），改完立刻跑
`host/usb_cdc_timing.py` 看 SETDTR 还在不在 30 s。

用法
----
    python tools/ss_tweak.py --mode 0x04 --bitmap 0x0003 --test
    python tools/ss_tweak.py --mode 0x00 --test      # 回到默认行为
    python tools/ss_tweak.py --show                   # 只读当前值 + 诊断计数
"""
import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF = os.path.join(ROOT, "build", "ra6e2_fw.elf")
NM = r"E:\software\ArmGNUToolchain\14.3 rel1\bin\arm-none-eabi-nm.exe"
PROBE_RS = "E:/.cargo/bin/probe-rs.exe"
PROBE = "0d28:0204"
CHIP = "R7FA6E2BB"
PY = sys.executable
TIMING = os.path.join(ROOT, "host", "usb_cdc_timing.py")

WRITABLE = {
    "mode": "g_usb_ss_mode",
    "bitmap": "g_usb_ss_bitmap",
    "burst_n": "g_usb_ss_burst_n",
    "gap": "g_usb_ss_gap_ticks",
}

READABLE = [
    "g_usb_ss_mode", "g_usb_ss_bitmap", "g_usb_ss_burst_n", "g_usb_ss_gap_ticks",
    "g_usb_ss_tick", "g_usb_ss_sent", "g_usb_ss_err", "g_usb_ss_err_cnt",
    "g_usb_ss_stuck", "g_usb_line_cnt", "g_usb_line_value",
]


def symbols():
    try:
        out = subprocess.check_output([NM, ELF], text=True, errors="replace")
    except Exception as e:                                        # noqa: BLE001
        print(f"取符号失败: {e}")
        return {}
    sym = {}
    wanted = set(READABLE) | set(WRITABLE.values())
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[2] in wanted:
            sym[p[2]] = int(p[0], 16)
    return sym


def pr(cmd, timeout=120):
    r = subprocess.run(cmd, capture_output=True, text=True,
                       encoding="utf-8", errors="replace", timeout=timeout)
    return r.returncode, (r.stdout or "") + (r.stderr or "")


def read_words(addr, n):
    _, out = pr([PROBE_RS, "read", "--chip", CHIP, "--probe", PROBE,
                 "--speed", "400", "b32", hex(addr), str(n)])
    vals = re.findall(r"\b([0-9a-fA-F]{8})\b", out)
    return [int(v, 16) for v in vals[:n]]


def write_words(addr, vals):
    cmd = [PROBE_RS, "write", "--chip", CHIP, "--probe", PROBE, "--speed", "400",
           "b32", hex(addr)] + [hex(v) for v in vals]
    rc, out = pr(cmd)
    return rc == 0, out


def main():
    ap = argparse.ArgumentParser()
    for k in WRITABLE:
        ap.add_argument(f"--{k}", type=lambda s: int(s, 0), default=None,
                        help=f"写 {WRITABLE[k]}")
    ap.add_argument("--show", action="store_true", help="只读当前值")
    ap.add_argument("--test", action="store_true", help="改完跑 DTR 计时测试")
    args = ap.parse_args()

    sym = symbols()
    if not sym:
        print("取不到符号，先 build。")
        return 2

    todo = [(k, getattr(args, k)) for k in WRITABLE if getattr(args, k) is not None]
    if todo:
        # 同一地址的写要合并成一个 probe-rs write 调用（它支持连续多值）
        addrs = [(sym[WRITABLE[k]], v) for k, v in todo]
        if len(set(a for a, _ in addrs)) == len(addrs):
            base = addrs[0][0]
            if all(a == base + 4 * i for i, (a, _) in enumerate(addrs)):
                ok, out = write_words(base, [v for _, v in addrs])
                print(("✔ 已写 " if ok else "✘ 写失败 ") +
                      ", ".join(f"{WRITABLE[k]}=0x{v:X}" for k, v in todo))
                if not ok:
                    print(out[-400:])
                    return 3
            else:
                for a, v in addrs:
                    ok, out = write_words(a, [v])
                    print(f"{'✔' if ok else '✘'} 0x{a:X} = 0x{v:X}")
        else:
            for a, v in addrs:
                ok, out = write_words(a, [v])
                print(f"{'✔' if ok else '✘'} 0x{a:X} = 0x{v:X}")

    print("\n=== 当前状态 ===")
    for name in READABLE:
        a = sym.get(name)
        if a is None:
            continue
        v = read_words(a, 1)
        print(f"  {name:<20s} = {'0x%08X (%d)' % (v[0], v[0]) if v else '读不到'}")

    if args.test:
        print("\n=== DTR 计时测试 ===")
        r = subprocess.run([PY, TIMING], capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=300)
        out = r.stdout or ""
        print(out)
        bad = re.findall(r"(\S[^\n]*?)\s+(\d+\.\d\d) s\s+<<<<", out)
        if bad:
            print("✘ 仍有 30 s 级卡点:", flush=True)
            for n, t in bad:
                print(f"    {n} = {t} s")
        else:
            print("✔ 没有 30 s 级卡点")
    return 0


if __name__ == "__main__":
    sys.exit(main())
