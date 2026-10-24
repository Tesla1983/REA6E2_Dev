#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""shot_gui.py —— 把 GUI 拉起来、等它画完、截屏存 PNG，然后关掉。

用途：改界面时自己"看一眼"，不依赖人肉反馈。

前置：exe 不能带 requireAdministrator（否则非提权 shell 起不来），
     所以要用 `RA6E2_NO_UAC=1 cargo build --release` 编一个截图专用版。

用法:
    python tools/shot_gui.py target/release/ra6e2-monitor.exe shot.png
    python tools/shot_gui.py ... shot.png --wait 6
"""

import argparse
import subprocess
import sys
import time


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("out")
    ap.add_argument("--wait", type=float, default=4.5,
                    help="启动后等多久再截（字体 16MB，首帧要点时间）")
    args = ap.parse_args()

    from PIL import ImageGrab

    proc = subprocess.Popen([args.exe], close_fds=True)
    try:
        time.sleep(args.wait)
        img = ImageGrab.grab()
        img.save(args.out)
        print(f"已保存 {args.out}  尺寸={img.size}")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
