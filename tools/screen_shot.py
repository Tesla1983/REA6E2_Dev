#!/usr/bin/env python3
"""把板子上的 u8g2 帧缓冲 dump 下来，还原成一张 BMP —— "看不见屏也能截图"。

原理: 屏只写不读（NT7534 没有回读通道），但 u8g2 的帧缓冲 g_u8g2_fb 是普通 RAM 数组，
探针能直接读出 1024 字节。布局 = 页优先(8 页 x 128 列)，每字节是该列 8 个像素，bit0 在最上。

用法:
  python tools/screen_shot.py                       # 默认地址/大小, 输出 .workbuddy/shots/screen.bmp
  python tools/screen_shot.py --addr 0x20001a08 --scale 6 --out shot.bmp
"""
import argparse
import struct
import subprocess
import sys

PROBE = 'E:/.cargo/bin/probe-rs.exe'
CHIP = 'R7FA6E2BB'
# 探针选择：默认用 FireDAP（2026-10-24 起；那块 DAPLink 一碰 SWD 就断言）
PROBE_ID = '0416:5021'
W, H = 128, 64


def read_ram(addr, n, probe=None):
    cmd = [PROBE, 'read', '--chip', CHIP, '--probe', probe or PROBE_ID, 'b8', hex(addr), str(n)]
    out = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    if out.returncode != 0:
        sys.exit(f'probe-rs 读取失败: {out.stderr.strip()}')
    data = bytes(int(x, 16) for x in out.stdout.split())
    if len(data) != n:
        sys.exit(f'读到 {len(data)} 字节, 期望 {n}')
    return data


def to_pixels(fb):
    """返回 rows[y][x] = 0/1"""
    rows = [[0] * W for _ in range(H)]
    for page in range(H // 8):
        for col in range(W):
            b = fb[page * W + col]
            for bit in range(8):
                rows[page * 8 + bit][col] = (b >> bit) & 1
    return rows


def write_bmp(path, rows, scale, fg=(0, 0, 0), bg=(255, 255, 255)):
    """24 位 BMP, 白底黑点。BMP 像素行是从下往上、BGR、行宽 4 字节对齐。"""
    sw, sh = W * scale, H * scale
    row_bytes = sw * 3
    pad = (-row_bytes) % 4
    pixel_bytes = (row_bytes + pad) * sh
    header = b'BM' + struct.pack('<IHHI', 14 + 40 + pixel_bytes, 0, 0, 14 + 40)
    dib = struct.pack('<IiiHHIIiiII', 40, sw, sh, 1, 24, 0, pixel_bytes, 2835, 2835, 0, 0)

    body = bytearray()
    for y in range(sh - 1, -1, -1):                 # BMP 自下而上
        line = bytearray()
        src = rows[y // scale]
        for x in range(sw):
            on = src[x // scale]
            r, g, b = fg if on else bg
            line += bytes((b, g, r))                # BGR
        line += b'\x00' * pad
        body += line

    with open(path, 'wb') as fh:
        fh.write(header + dib + bytes(body))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--addr', default='0x20001a08', help='g_u8g2_fb 的地址(每次重编译会变, 用 nm 现取)')
    ap.add_argument('--probe', default=PROBE_ID, help='probe-rs 探针选择器(默认 FireDAP 0416:5021)')
    ap.add_argument('--scale', type=int, default=5)
    ap.add_argument('--out', default='.workbuddy/shots/screen.bmp')
    args = ap.parse_args()

    fb = read_ram(int(args.addr, 16), W * H // 8, args.probe)
    rows = to_pixels(fb)
    write_bmp(args.out, rows, args.scale)
    print(f'已写出 {args.out}  ({W}x{H} 放大 {args.scale} 倍)')
    # 顺便印一份 ASCII 版, 不需要看图也能判断内容
    for y in range(0, H, 2):
        print(''.join('#' if rows[y][x] else '.' for x in range(W)))


if __name__ == '__main__':
    main()
