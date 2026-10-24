#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
把从片上读出来的 OLED 帧缓冲(1024 字节, 页优先)渲染成 PNG, 便于目视核对。

用法:
    python tools/render_fb.py <hex 文本文件或 - 表示 stdin> <输出 PNG>

输入格式: probe-rs read b8 <addr> 1024 的输出(空格分隔的十六进制字节)

显存排布与固件一致(页优先, 每字节 = 一列上的竖直 8 像素, bit0 在最上):
    fb[page * 128 + col]  ->  列 col, 行 page*8 .. page*8+7

只依赖标准库(zlib / struct), 不需要 PIL。
"""
import sys
import zlib
import struct

WIDTH = 128
PAGES = 8
SCALE = 4          # 放大倍数, 便于看清


def write_png(rows, width, height, path):
    """rows: list[bytearray], 每像素 1 字节(0=黑 255=白), 灰度 PNG"""
    raw = bytearray()
    for r in rows:
        raw.append(0)           # filter type 0 (None)
        raw.extend(r)

    def chunk(tag, data):
        return (struct.pack('>I', len(data)) + tag + data +
                struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF))

    ihdr = struct.pack('>IIBBBBB', width, height, 8, 0, 0, 0, 0)
    png = (b'\x89PNG\r\n\x1a\n' +
           chunk(b'IHDR', ihdr) +
           chunk(b'IDAT', zlib.compress(bytes(raw), 9)) +
           chunk(b'IEND', b''))
    with open(path, 'wb') as f:
        f.write(png)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1

    src, dst = sys.argv[1], sys.argv[2]
    text = sys.stdin.read() if src == '-' else open(src, 'r', encoding='utf-8').read()
    fb = [int(t, 16) for t in text.split()]

    if len(fb) != WIDTH * PAGES:
        print('字节数应为 %d, 实际 %d' % (WIDTH * PAGES, len(fb)))
        return 1

    # 1:1 像素图
    rows = []
    for y in range(WIDTH and PAGES * 8):
        page, bit = y // 8, y % 8
        row = bytearray()
        for x in range(WIDTH):
            on = (fb[page * WIDTH + x] >> bit) & 1
            row.append(255 if on else 16)      # 灭掉的像素用深灰, 便于看清边界
        rows.append(row)

    # 放大
    big = []
    for r in rows:
        line = bytearray()
        for v in r:
            line.extend([v] * SCALE)
        for _ in range(SCALE):
            big.append(line)

    write_png(big, WIDTH * SCALE, PAGES * 8 * SCALE, dst)

    # 顺便打印每页非零字节数, 一眼看出内容分布是否合理
    for p in range(PAGES):
        nz = sum(1 for v in fb[p * WIDTH:(p + 1) * WIDTH] if v)
        print('页 %d: 非零列 %3d / 128' % (p, nz))
    print('已写出 %s (%dx%d, 放大 %d 倍)' % (dst, WIDTH * SCALE, PAGES * 8 * SCALE, SCALE))
    return 0


if __name__ == '__main__':
    sys.exit(main())
