#!/usr/bin/env python3
"""解析 u8g2 单字体 .c 文件，列出它实际包含的字符码点。

用途: 判断某个 u8g2 字体里到底有没有需要的符号（例如 ℃ 用到的 U+00B0 度符号），
避免"选了字体、烧进去才发现缺字"。

u8g2 字体格式（见 csrc/u8g2_font.c 顶部注释）:
  offset 0      glyph_cnt
  offset 1..16  RLE 参数 / 包围盒
  offset 17..18 start_pos 'A'   (uint16 BE)
  offset 19..20 start_pos 'a'
  offset 21..22 start_pos unicode index table
  数据区从 offset 23 开始，每个 glyph = [编码 1B][总长 1B][RLE 数据 ...]
  < 0x41 的码点从 base 0 扫；0x41..0x60 从 start_pos_A 扫；>= 0x61 从 start_pos_a 扫。

用法: python tools/u8g2_font_probe.py <font.c> [<font.c> ...] [--sym 00B0 2103]
"""
import re
import sys

HDR = 23


def load_bytes(path):
    """把 C 源里的字符串字面量按 C 转义规则还原成字节串。"""
    text = open(path, encoding='utf-8', errors='replace').read()
    eq = text.find('=')
    body = text[eq:] if eq >= 0 else text

    out = bytearray()
    for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', body):
        t = m.group(1)
        i = 0
        n = len(t)
        while i < n:
            ch = t[i]
            if ch == chr(92):                     # 反斜杠
                i += 1
                if i >= n:
                    break
                c2 = t[i]
                if c2 in '01234567':
                    oct_digits = ''
                    while i < n and t[i] in '01234567' and len(oct_digits) < 3:
                        oct_digits += t[i]
                        i += 1
                    out.append(int(oct_digits, 8) & 0xFF)
                else:
                    table = {'n': 10, 'r': 13, 't': 9, 'v': 11, 'f': 12}
                    out.append(table.get(c2, ord(c2)))
                    i += 1
            else:
                out.append(ord(ch))
                i += 1
    return bytes(out)


def parse(path):
    d = load_bytes(path)
    start_a = (d[17] << 8) | d[18]
    start_lower = (d[19] << 8) | d[20]
    start_uni = (d[21] << 8) | d[22] if len(d) > 22 else 0

    codes = set()

    def scan(base_off):
        pos = HDR + base_off
        while pos + 1 < len(d):
            size = d[pos + 1]
            if size == 0:
                break
            codes.add(d[pos])
            pos += size

    scan(0)
    scan(start_a)
    scan(start_lower)
    return d, codes, start_a, start_lower, start_uni


def main(argv):
    args = [a for a in argv[1:] if not a.startswith('--')]
    want = []
    for i, a in enumerate(argv):
        if a == '--sym':
            want = [int(x, 16) for x in argv[i + 1:]]
    for path in args:
        try:
            d, codes, sa, sl, su = parse(path)
        except Exception as exc:                  # noqa: BLE001 - 诊断脚本，出错直接报告
            print(f'{path}: 解析失败 {exc}')
            continue
        name = path.replace(chr(92), '/').split('/')[-1]
        ascii_ok = all(c in codes for c in range(0x20, 0x7F))
        print(f'{name}: 共 {len(d)}B, glyph_cnt={d[0]}, 解析出 {len(codes)} 个码点, '
              f'纯 ASCII 全覆盖={ascii_ok}, 最高码点=U+{max(codes):04X}')
        if want:
            for w in want:
                print(f'  U+{w:04X}: {"有" if w in codes else "没有"}')
        if not ascii_ok:
            missing = [hex(c) for c in range(0x20, 0x7F) if c not in codes]
            print(f'  缺 ASCII: {missing[:12]}')


if __name__ == '__main__':
    main(sys.argv)
