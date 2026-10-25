#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
解析固件「USB 类请求环形日志」的内存转储。

用法:
    python scripts/req_log_parse.py <dump.txt>

dump.txt 由下列命令产生(纯十六进制字节流, probe-rs read b8 的原始输出):
    probe-rs read b8 0x<base> 220 --chip R7FA6E2BB --probe 0416:5021 --speed 1000

内存布局(g_usb_req_* 系列, 相对起始地址) 由 elf 的 nm 决定, 见 BASE_OFFSETS。
⚠ probe-rs read b8 输出的是**十六进制文本**, 必须按 16 进制解析。
"""
import re
import sys

N = 16  # USB_REQ_LOG_N

BASE_OFFSETS = {
    'data_cnt': 0,
    'nodata_cnt': 4,
    'seq': 8,           # u32 * N
    'length': 8 + 4 * N,        # u16 * N
    'index': 8 + 4 * N + 2 * N,
    'value': 8 + 4 * N + 4 * N,
    'breq': 8 + 4 * N + 6 * N,   # u8 * N
    'bmrt': 8 + 4 * N + 6 * N + N,
    'head': 8 + 4 * N + 6 * N + 2 * N,
    'last': 8 + 4 * N + 6 * N + 2 * N + 4,
    'done_cnt': 8 + 4 * N + 6 * N + 2 * N + 8,
    'cnt': 8 + 4 * N + 6 * N + 2 * N + 12,
}

CDC_NAMES = {
    0x00: 'SEND_ENCAPSULATED_COMMAND',
    0x01: 'GET_ENCAPSULATED_RESPONSE',
    0x20: 'SET_LINE_CODING',
    0x21: 'GET_LINE_CODING',
    0x22: 'SET_CONTROL_LINE_STATE',
    0x23: 'SEND_BREAK',
}


def load(path):
    text = open(path, 'r', encoding='utf-8', errors='ignore').read()
    toks = re.findall(r'\b[0-9a-fA-F]{2}\b', text)
    return [int(t, 16) for t in toks]


def u32(b, o):
    return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)


def u16(b, o):
    return b[o] | (b[o + 1] << 8)


def main():
    if len(sys.argv) < 2:
        print('用法: python scripts/req_log_parse.py <dump.txt>')
        return 1
    b = load(sys.argv[1])
    need = BASE_OFFSETS['cnt'] + 4
    if len(b) < need:
        print(f'✘ 数据太短: 需要 {need} 字节, 实际 {len(b)}')
        return 1

    o = BASE_OFFSETS
    cnt = u32(b, o['cnt'])
    head = u32(b, o['head'])
    data_cnt = u32(b, o['data_cnt'])
    nodata_cnt = u32(b, o['nodata_cnt'])
    last = u32(b, o['last'])
    done = u32(b, o['done_cnt'])

    print()
    print('=============== USB 类请求日志 ===============')
    print(f'请求总数 g_usb_req_cnt        : {cnt}')
    print(f'走了数据阶段 g_usb_req_data_cnt : {data_cnt}')
    print(f'按无数据收尾 g_usb_req_nodata_cnt: {nodata_cnt}')
    print(f'环形写入位置 head             : {head}')
    print(f'最后请求类型 g_usb_req_last    : 0x{last:02x}')
    print(f'g_usb_req_done_cnt            : {done}')
    print()

    if cnt == 0:
        print('⚠ 一条请求都没有 ⇒ 固件没跑起来(设备可能仍停在烧录后的 halt, 需要拔插 Type-C)。')
        return 0

    h = head % N
    print(f'最近 {N} 条请求(按时间顺序; head 指向下一条要写的位置):')
    print(f"{'#':>3}  {'seq':>4} {'bmRT':>6} {'bReq':>6} {'wValue':>7} {'wIndex':>7} {'wLength':>8}  请求名")
    for k in range(N):
        i = (h + k) % N
        s = u32(b, o['seq'] + 4 * i)
        ln = u16(b, o['length'] + 2 * i)
        ix = u16(b, o['index'] + 2 * i)
        va = u16(b, o['value'] + 2 * i)
        rq = b[o['breq'] + i]
        rt = b[o['bmrt'] + i]
        if s == 0:
            continue  # 未写入的槽位
        flag = ''
        if ln > 0:
            flag = '   <<< wLength>0 (有数据阶段!)'
        print(f'{k:>3}  {s:>4}  0x{rt:02x}    0x{rq:02x}   {va:>7} {ix:>7} {ln:>8}  {CDC_NAMES.get(rq, "?")}{flag}')
    print()
    print('判读:')
    print('  data_cnt > 0        ⇒ 确有带数据阶段的请求走了数据阶段(本轮修复生效)')
    print('  出现 wLength>0 的 0x01 GET_ENCAPSULATED_RESPONSE ⇒ 就是主机白等 ~30s 的元凶')
    print('  全部 wLength=0 且无 0x00/0x01 封装请求 ⇒ 30s 另有出处, 需继续查')
    return 0


if __name__ == '__main__':
    sys.exit(main())
