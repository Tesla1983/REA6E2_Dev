#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
size_report.py -- RA6E2 编译后的内存占用报告（ESP-IDF 风格表格，中文输出）。

数据来源: arm-none-eabi-objdump -h <elf>
  只统计带 ALLOC 标志的段(真正占内存/flash 的), .debug_* / .comment 等自动跳过。
  RA 的链接脚本把同类段聚合成了少数几个大段, 这里翻译回常见叫法:
      __flash_vectors$$     -> .vectors        (向量表, 在 flash)
      __flash_readonly$$    -> .text/.rodata   (代码 + 常量, 在 flash)
      __ram_from_flash$$    -> .data           (有初值的全局变量; VMA 在 RAM,
                                                  LMA 在 flash —— 烧录镜像里也占一份)
      __ram_zero$$          -> .bss            (零初始化全局变量, 只占 RAM)
      __ram_zero_nocache$$  -> .bss(nc)
      __ram_noinit$$        -> .noinit

内存区域(与 cmake/ld/memory_regions.ld 一致; 改了区域要同步这里):
      FLASH       起始 0x00000000  容量 256KB
      RAM         起始 0x20000000  容量  40KB
      DATA_FLASH  起始 0x08000000  容量   4KB

用法(通常由 cmake/PostBuild.cmake 自动调用, 不必手动跑):
  python size_report.py --elf build/ra6e2_fw.elf --bin build/ra6e2_fw.bin \
                        --objdump "E:/.../arm-none-eabi-objdump.exe" [--no-color]
"""
import argparse
import os
import re
import subprocess
import sys

# ---- 内存区域: (显示名, 起始地址, 容量, 大类颜色) ----
#     颜色是 ANSI 前景色: 31=红 33=黄 36=青
REGIONS = [
    ("FLASH 代码+常量", 0x00000000, 0x40000, "31"),
    ("RAM 数据",        0x20000000, 0xA000,  "33"),
    ("DATA_FLASH",      0x08000000, 0x1000,  "36"),
]

# 段名翻译表(RA 链接器的聚合段名 -> 常见叫法); 未列出的原样显示
PRETTY = {
    "__flash_vectors$$":    ".vectors",
    "__flash_readonly$$":   ".text/.rodata",
    "__flash_init_array$$": ".init_array",
    "__flash_fini_array$$": ".fini_array",
    "__ram_from_flash$$":   ".data",
    "__ram_zero$$":         ".bss",
    "__ram_zero_nocache$$": ".bss(nc)",
    "__ram_noinit$$":       ".noinit",
}

SEC_RE = re.compile(
    r"^\s*\d+\s+(\S+)\s+([0-9A-Fa-f]+)\s+([0-9A-Fa-f]+)\s+([0-9A-Fa-f]+)\s+[0-9A-Fa-f]+\s+2\*\*\d+\s*$")

# 列宽
W_SEC, W_USED, W_PCT, W_REM, W_TOT = 24, 14, 10, 16, 15


# ---- 显示宽度: 中文/全角字符占 2 列 ----
def dwidth(s):
    n = 0
    for ch in s:
        c = ord(ch)
        if 0x1100 <= c <= 0x115F or 0x2E80 <= c <= 0xA4CF \
           or 0xAC00 <= c <= 0xD7A3 or 0xF900 <= c <= 0xFAFF \
           or 0xFE30 <= c <= 0xFE6F or 0xFF00 <= c <= 0xFF60 \
           or 0xFFE0 <= c <= 0xFFE6:
            n += 2
        else:
            n += 1
    return n


def ljust(s, w):
    return s + " " * max(0, w - dwidth(s))


def rjust(s, w):
    return " " * max(0, w - dwidth(s)) + s


# 颜色开关(main 里按 --no-color 设置); 放模块级是因为 trow()/hline() 也要用
USE_COLOR = True


def c(code, s):
    if not USE_COLOR:
        return s
    return f"\x1b[{code}m{s}\x1b[0m"


def hline(kind="mid"):
    """表格横线, 用 Unicode 制表符画实线(不是 ASCII 的 +---+ 那种虚线效果)。

    kind: top = ┌─┬─┐   mid = ├─┼─┤   bottom = └─┴─┘
    边框统一染成黄色(与 ESP-IDF 的观感接近)。
    """
    left = {"top": "┌", "bottom": "└"}.get(kind, "├")
    right = {"top": "┐", "bottom": "┘"}.get(kind, "┤")
    mid = {"top": "┬", "bottom": "┴"}.get(kind, "┼")
    widths = [W_SEC, W_USED, W_PCT, W_REM, W_TOT]
    return c("33", left + mid.join("─" * w for w in widths) + right)


def trow(cells, colors=None):
    """表格行: │ c1 │ c2 │ ... │  第 1 列左对齐, 其余右对齐; 不足 5 列补空。

    ⚠ 转义序列要尽量少: 竖线并入**前一个单元格**的着色范围里, 而不是每个竖线
    单独包一层颜色。一行十来个转义序列切换时, 个别终端(xterm.js)会渲染错位 ——
    竖线断开/漂移, 但把同一行写到文件里看文本又是对齐的, 很迷惑。
    合并后每行只有"有色单元格数"个转义对。
    """
    widths = [W_SEC, W_USED, W_PCT, W_REM, W_TOT]
    colors = list(colors or []) + [""] * (5 - len(colors or []))
    cells = list(cells) + [""] * (5 - len(cells))
    out = []
    for i, txt in enumerate(cells):
        s = ljust(txt, widths[i]) if i == 0 else rjust(txt, widths[i])
        col = colors[i]
        out.append(c(col, "│" + s) if col else "│" + s)
    tail = colors[-1]
    out.append(c(tail, "│") if tail else "│")
    return "".join(out)


def parse_sections(objdump, elf):
    """跑 objdump -h, 返回 [(段名, 大小, VMA, LMA), ...]，只保留 ALLOC 且大小 > 0 的段。

    objdump -h 的段信息占两行: 第一行是 序号/名字/大小/VMA/LMA/偏移/对齐,
    第二行才是属性标志(CONTENTS, ALLOC, LOAD...), 所以要用一个状态机配起来。
    """
    out = subprocess.run([objdump, "-h", elf], capture_output=True, text=True,
                         encoding="utf-8", errors="ignore")
    if out.returncode != 0:
        sys.exit("objdump 执行失败: " + out.stderr[:400])
    secs, cur = [], None
    for line in out.stdout.splitlines():
        m = SEC_RE.match(line)
        if m:
            cur = (m.group(1), int(m.group(2), 16), int(m.group(3), 16), int(m.group(4), 16))
            continue
        if cur is not None and "ALLOC" in line:
            if cur[1] > 0:
                secs.append(cur)
            cur = None
    return secs


def human(n):
    """千分位, 与 ESP-IDF 一致"""
    return f"{n:,}"


def main():
    ap = argparse.ArgumentParser(description="RA6E2 内存占用报告(ESP-IDF 风格)")
    ap.add_argument("--elf", required=True, help="编译产物 elf")
    ap.add_argument("--bin", required=True, help="编译产物 bin, 用于镜像总大小")
    ap.add_argument("--objdump", required=True, help="arm-none-eabi-objdump 路径")
    ap.add_argument("--no-color", action="store_true", help="关闭颜色")
    a = ap.parse_args()

    # 默认开颜色: 本脚本经由 CMake execute_process 管道运行, isatty 恒为 False,
    # 按 tty 检测的话永远没颜色。VSCode / Windows Terminal 都能解析 ANSI。
    # 输出重定向到文件时请自行加 --no-color, 免得日志里混进转义序列。
    global USE_COLOR
    USE_COLOR = not a.no_color

    # Windows 上重定向时 Python 默认用 GBK 编码 stdout, 会乱码; 统一强制 UTF-8。
    # newline="\n": 文本模式默认会把 \n 转成 \r\n, 实测个别终端对着色行 + \r 的
    # 组合渲染会把竖线画歪(文本层明明是对齐的), 干脆只用 \n。
    try:
        sys.stdout.reconfigure(encoding="utf-8", newline="\n")
    except Exception:
        pass

    secs = parse_sections(a.objdump, a.elf)

    # 按区域分组
    groups = {r[0]: [] for r in REGIONS}
    flash_image = 0        # 烧录镜像字节数(所有 LMA 落在 FLASH 的段)
    for name, size, vma, lma in secs:
        for rname, start, length, _ in REGIONS:
            if start <= vma < start + length:
                groups[rname].append((name, size))
                if start == 0x00000000:                       # 段本体就在 flash
                    flash_image += size
                elif lma != vma and 0x00000000 <= lma < 0x40000:
                    flash_image += size                       # .data 类: 加载镜像在 flash
                break

    print()
    # 只显示"有段"的区域; 最后一个区域收尾要用 └─┴─┘
    active = [r for r in REGIONS if groups[r[0]]]

    print(hline("top"))
    # 表头(先按显示宽度补齐, 再套颜色 —— 顺序反了会把 ANSI 码算进宽度导致错位)
    print(trow(["内存类型 / 段", "已用 [字节]", "占比 [%]", "剩余 [字节]", "总容量 [字节]"],
               ["1", "1", "1", "1", "1"]))
    print(hline("mid"))

    for ri, (rname, start, length, color) in enumerate(active):
        items = sorted(groups[rname], key=lambda x: -x[1])
        last = (ri == len(active) - 1)
        used = sum(s for _, s in items)
        remain, total = length - used, length
        pct = used * 100.0 / total if total else 0.0
        pct_col = "31" if pct >= 90 else ("33" if pct >= 75 else "32")

        print(trow([rname], ["1;" + color]))          # 大类行(粗体 + 区域色)
        for name, size in items:                      # 子行: 只有"已用"一列
            print(trow(["  " + PRETTY.get(name, name), human(size)], ["94", "96"]))
        print(trow(["  小计", human(used), f"{pct:.2f}", human(remain), human(total)],
                   ["90", "1;96", pct_col, "90", "90"]))
        print(hline("bottom" if last else "mid"))

    bin_size = os.path.getsize(a.bin) if os.path.exists(a.bin) else flash_image
    print()
    print(c("1;33", f"镜像总大小: {human(bin_size)} 字节"
                    f"（.bin 可能因对齐而略大）"))
    print()


if __name__ == "__main__":
    main()
