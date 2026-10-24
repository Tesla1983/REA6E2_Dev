# -*- coding: utf-8 -*-
"""v18 收尾：删掉因移除屏幕自检而失效的静态函数，保持零 warning。

一次性脚本。按函数名定位 `static ... name(` 并做括号配平删除。
"""
import io
import re

P = "src/nt7534/nt7534_test.c"
s = io.open(P, encoding="utf-8").read()

TARGETS = [
    "oled_wiring_page",
    "oled_status",
    "fail_blink_forever",
    "stage_wait",
    "draw_v_stripes",
    "draw_checker",
    "draw_frame_stage",
]


def remove_fn(src: str, name: str) -> str:
    """删掉 static 函数定义（含前面的文档注释块）。"""
    m = re.search(r"\nstatic [^\n;{]*\b" + re.escape(name) + r"\s*\(", src)
    if not m:
        print("  ! 未找到:", name)
        return src

    start = m.start() + 1                      # 跳过换行
    # 往前吞掉紧邻的文档注释 / 行注释
    head = src[:start]
    while True:
        stripped = head.rstrip("\n")
        lines = stripped.split("\n")
        if not lines:
            break
        last = lines[-1].strip()
        if last.endswith("*/") or last.startswith("*") or last.startswith("/*"):
            # 继续往上找 /** 起点
            k = len(lines) - 1
            while k >= 0 and not lines[k].strip().startswith("/*"):
                k -= 1
            if k >= 0:
                start = len("\n".join(lines[:k])) + 1
                head = src[:start]
                continue
        if last.startswith("//"):
            start = len("\n".join(lines[:-1])) + 1
            head = src[:start]
            continue
        break

    # 从函数名所在的 '{' 开始配平
    brace = src.index("{", m.end() - 1)
    depth = 0
    i = brace
    while i < len(src):
        c = src[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                i += 1
                break
        i += 1
    # 连同行尾换行一起删
    while i < len(src) and src[i] == "\n":
        i += 1
    return src[:start] + src[i:]


for name in TARGETS:
    before = s
    s = remove_fn(s, name)
    print(("  已删除 " if s != before else "  跳过 ") + name)

# s_usb_started 现在没有任何读者，连声明和赋值一起去掉
s = s.replace("/** USB 是否已经初始化过。自检期间要排空 USB 事件, 但初始化之前绝不能碰。 */\nstatic bool s_usb_started = false;\n\n", "")
s = s.replace("    s_usb_started = (FSP_SUCCESS == USB_CDC_Init());", "    (void) USB_CDC_Init();")
s = s.replace('    RTT_Printf("usb cdc init: %s\\n", s_usb_started ? "OK" : "FAILED");\n', "")

io.open(P, "w", encoding="utf-8", newline="\n").write(s)
print("完成，行数 =", s.count("\n") + 1)
