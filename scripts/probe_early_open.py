"""在 COM 号刚被 PnP 确认、但还没登记进 SERIALCOMM 时, 反复尝试打开它。

目的: 回答"串口是不是其实早就**能打开**, 只是**扫描不到**"。
若成立, 上位机(GUI)就能绕过那 ~30s: 从 PnP 拿 COM 号直接打开,
而不必等 pyserial / SERIALCOMM 枚举出它。

用法: python probe_early_open.py COM17 [最长秒数]
"""
import sys
import time

import serial


def reg_has(name):
    """该 COM 名是否已登记进 HKLM\\HARDWARE\\DEVICEMAP\\SERIALCOMM(扫描工具读的表)"""
    try:
        import winreg
        k = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DEVICEMAP\SERIALCOMM")
        vals = []
        i = 0
        while True:
            try:
                _n, v, _t = winreg.EnumValue(k, i)
                vals.append(v)
                i += 1
            except OSError:
                break
        return name in vals
    except Exception:
        return False


def main():
    if len(sys.argv) < 2:
        print("用法: probe_early_open.py COM17 [最长秒数]")
        return 2
    name = sys.argv[1]
    dur = float(sys.argv[2]) if len(sys.argv) > 2 else 60.0

    t0 = time.time()
    t_open = None
    t_reg = None
    while time.time() - t0 < dur:
        if t_reg is None and reg_has(name):
            t_reg = time.time() - t0
        if t_open is None:
            try:
                s = serial.Serial(name, timeout=0.2)
                t_open = time.time() - t0
                s.close()
            except Exception:
                pass
        if t_open and t_reg:
            break
        time.sleep(0.4)

    print("[probe] 目标端口        : %s" % name)
    print("[probe] 首次成功打开    : %s" % (("%.2f s" % t_open) if t_open else "失败(超时)"))
    print("[probe] 登记进SERIALCOMM : %s" % (("%.2f s" % t_reg) if t_reg else "未出现"))
    if t_open and t_reg:
        if t_reg - t_open > 1.0:
            print("[probe] ⇒ 打开比『可扫描』早 %.2f s —— 端口其实早就可用, 只是扫描不到" % (t_reg - t_open))
        else:
            print("[probe] ⇒ 打开与登记几乎同时, 没有提前可用的窗口")
    return 0


if __name__ == "__main__":
    sys.exit(main())
