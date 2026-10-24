#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""守着等 DAPLink 复活 → 预检 SWD → 烧 RA6E2 → 验证 USB CDC 的 30 s 卡点。

为什么是"守着等"
---------------
DAPLink(0d28:0204) 的状态只能靠人工「拔 USB 等满 30 秒」改变，
而且它的"活窗口"很短：一次 SWD 超时就会把自己卡死。
所以这个脚本会**反复等**：探针一活就立刻预检+烧，失败了就退回去继续等下一个窗口，
全程不需要人工确认。

每轮的流程
----------
1. 等探针的 CMSIS-DAP 命令引擎有应答（`DAP_Info(FWVER)`，1 秒一条）
2. 预检：`probe-rs info` —— 只做 SWD 握手，不写 flash。
   成功 ⇒ 目标核能应答，走下面的烧录；失败 ⇒ 也照样试（attach 可能只是被
   连接问题绊住），但会先打印失败原因。
3. 烧录，参数按降级序列：
     --speed 1000 --connect-under-reset
     --speed 400  --connect-under-reset --no-dblbuf
     --speed 100  --connect-under-reset --no-dblbuf
     --speed 400  --no-dblbuf
   （`--connect-under-reset` 放前面：上一轮失败会把内核留在 halt，
     复位后连接才稳。要求 DAPLink 的 nRESET 接到 RA6E2 的 RST。）
4. 烧成功 → `probe-rs reset` → 等 COM15 → 跑 `host/usb_cdc_timing.py` 验 30 s 是否消失
5. 失败 → 回到第 1 步继续等下一个窗口

用法
----
    python scripts/flash_auto.py                # 默认等 30 分钟
    python scripts/flash_auto.py --timeout 3600
    python scripts/flash_auto.py --no-verify    # 烧完不跑 CDC 计时
"""
import argparse
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PY = sys.executable
PROBE = "0d28:0204"
CHIP = "R7FA6E2BB"
PROBE_RS = "E:/.cargo/bin/probe-rs.exe"
DAP_PROBE = os.path.join(ROOT, "tools", "daplink_probe.py")
FLASH = os.path.join(ROOT, "scripts", "flash.sh")
TIMING = os.path.join(ROOT, "host", "usb_cdc_timing.py")
BAKE = r"C:\Program Files\Git\bin\bash.exe"

ATTEMPTS = [
    # 唯一验证过能成的配方（2026-10-23 15:37）：低速 + 关双缓冲
    "--speed 400 --no-dblbuf --verify",
    "--speed 1000 --no-dblbuf --verify",
    "--speed 100 --no-dblbuf --verify",
    "--speed 400 --connect-under-reset --no-dblbuf --verify",
]


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def sh(cmd, timeout=600):
    return subprocess.run([BAKE, "-lc", cmd], capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=timeout)


def com15_present():
    """RA6E2 的 USB CDC 口在不在（内核 halt 时不在，属正常）。"""
    try:
        import serial.tools.list_ports as lp
        return any(p.vid == 0x045B and p.pid == 0x0263 for p in lp.comports())
    except Exception:                                            # noqa: BLE001
        return False


def probe_state():
    """(状态, 说明)。状态: absent 没插 / dead 插着但命令引擎死 / alive 活。"""
    try:
        r = subprocess.run([PY, DAP_PROBE, "--quick", "--timeout", "800"],
                           capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=15)
    except Exception:                                            # noqa: BLE001
        return "absent", "探测异常"
    out = (r.stdout or "") + (r.stderr or "")
    if "没有枚举到 HID 接口" in out:
        return "absent", "USB 没枚举到（没插 / 线松 / 口没供电）"
    if "[✓]" in out:
        return "alive", "DAP_Info 有应答"
    return "dead", "HID 能开但 CMSIS-DAP 不应答（主循环卡死）"


def wait_alive(deadline):
    """等探针活。返回 True 表示已活。"""
    last = None
    while time.time() < deadline:
        st, desc = probe_state()
        com = com15_present()
        state = (st, com)
        if state != last:
            log(f"探针={st:<6s} {desc}   RA6E2(COM15)={'在' if com else '无'}")
            last = state
        if st == "alive":
            return True
        time.sleep(2)
    return False


def precheck():
    """probe-rs info：只做 SWD 握手，不写 flash。返回 (成功?, 输出)。"""
    cmd = [PROBE_RS, "info", "--chip", CHIP, "--probe", PROBE, "--speed", "1000"]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=90)
    except subprocess.TimeoutExpired:
        return False, "probe-rs info 超时（90 s）"
    out = (r.stdout or "") + (r.stderr or "")
    ok = bool(re.search(r"Cortex-M\d+", out)) and "Error" not in out
    return ok, out


def reset_target():
    """把目标核复位（短事务）。上一轮失败会把内核留在 halt，复位后才好烧。"""
    cmd = [PROBE_RS, "reset", "--chip", CHIP, "--probe", PROBE, "--speed", "400"]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=90)
    except subprocess.TimeoutExpired:
        return False
    out = (r.stdout or "") + (r.stderr or "")
    return r.returncode == 0 and "Error" not in out


def do_flash(extra):
    cmd = f"cd /e/REA6E2_Dev && ./scripts/flash.sh --probe {PROBE} {extra}"
    log(f"$ flash.sh --probe {PROBE} {extra}")
    try:
        r = sh(cmd, timeout=900)
    except subprocess.TimeoutExpired:
        log("✘ 烧录超时（>15 分钟）")
        return False
    out = (r.stdout or "") + (r.stderr or "")
    for line in out.splitlines():
        print("    " + line, flush=True)
    return r.returncode == 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=1800, help="总等待秒数")
    ap.add_argument("--no-verify", action="store_true", help="烧完不跑 CDC 计时验证")
    args = ap.parse_args()

    deadline = time.time() + args.timeout
    print("=" * 70)
    print("守着等 DAPLink 复活 → 预检 → 烧录 → 验证。请完成物理动作：")
    print("  ① DAPLink 的 USB 线拔掉，数满 30 秒，再插上")
    print("  ② **DAPLink 的 3V3 那根不要接** —— RA6E2 只用自己的 Type-C 供电")
    print("     （两路 3.3V 打架会让 SWD 握手时好时坏，这是今天失败的头号嫌疑）")
    print("  ③ RA6E2 的 Type-C 插好，等 COM15 出现")
    print("  ④ SWD 三根：SWCLK→P300、SWDIO→P108、GND→GND；"
          "若 DAPLink 有 nRESET/复位脚，也接到 RA6E2 的 RST")
    print("=" * 70, flush=True)

    attempt = 0
    while time.time() < deadline:
        log("等待探针复活 ...")
        if not wait_alive(deadline):
            log("✘ 等待超时")
            return 1
        log("✔ 探针活了")

        # ⚠⚠ 关键纪律（2026-10-23 实测）：**探针刚复活就立刻烧，中间不要插别的 SWD 事务**。
        # 今天 3 次失败里有 2 次是在探针已经做过一串读操作之后才烧的；
        # 唯一成功的那次是"预检 1 次 info → 立刻烧"就过了。
        # 所以这里只做一次极短的 reset（把上一轮 halt 的内核唤醒），然后马上烧。
        if reset_target():
            log("目标已复位")
            for _ in range(25):
                if com15_present():
                    log("✔ COM15 已出现（目标在跑）")
                    break
                time.sleep(1)
        else:
            log("⚠ reset 失败，直接尝试烧录")

        attempt += 1
        extra = ATTEMPTS[min(attempt - 1, len(ATTEMPTS) - 1)]
        if do_flash(extra):
            log("✔ 烧录成功")
            break
        log("✘ 这一轮失败。若探针又卡死，退回等待下一个 30 秒窗口 ...")
    else:
        log("✘ 总时间到，放弃")
        return 1

    # 复位运行，让 RA6E2 重新枚举
    log("复位运行，等 COM15 回来 ...")
    try:
        sh(f'"{PROBE_RS}" reset --chip {CHIP} --probe {PROBE} --speed 1000', timeout=120)
    except subprocess.TimeoutExpired:
        pass
    for _ in range(40):
        if com15_present():
            log("✔ COM15 已出现")
            break
        time.sleep(1)
    else:
        log("⚠ 40 秒内 COM15 没出现（板子可能没上电，或固件没跑起来）")

    if args.no_verify:
        return 0

    time.sleep(3)
    log("验证 USB CDC 的 30 s 卡点 ...")
    r = subprocess.run([PY, TIMING], capture_output=True, text=True,
                       encoding="utf-8", errors="replace", timeout=300)
    print(r.stdout or "", flush=True)
    if r.stderr:
        print(r.stderr, flush=True)

    text = r.stdout or ""
    # ⚠ 必须先确认测试真的跑起来了：CreateFileW 失败（口还没就绪）时下面没有任何
    #   计时数据，绝不能当成"通过"。
    if "CreateFileW (打开端口)" not in text or "异常" in text:
        log("✘ 计时测试没跑起来（端口打不开），不能判定")
        return 5
    bad = re.findall(r"(\S[^\n]*?)\s+(\d+\.\d\d) s\s+<<<<", text)
    if bad:
        log("✘ 仍有 30 s 级卡点:")
        for n, t in bad:
            log(f"    {n} = {t} s")
        return 4
    log("✔ 所有 Win32 调用都在 5 s 以内 ⇒ 30 s 卡点已消失")
    return 0


if __name__ == "__main__":
    sys.exit(main())
