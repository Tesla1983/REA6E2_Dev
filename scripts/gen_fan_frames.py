#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 3 叶 20x20 风扇位图帧（MSB 在左，u8g2_DrawXBMP 位序）到 src/nt7534/fan_frames.h。

算法与设计预览里的 drawFan() 完全一致：hilbert:
  on = (r < hub) or (r<=R and 命中三片带弧度叶片) or (r>R-0.8 外圈)
3 叶对称周期 120°，共 24 帧、每帧 5°，一圈视觉周期正好 24 帧。
"""
import math

SIZE = 20
BLADES = 3
FRAMES = 24
BYTES_PER_ROW = (SIZE + 7) // 8   # 3
FRAME_BYTES = BYTES_PER_ROW * SIZE  # 60

CX = (SIZE - 1) / 2.0
CY = (SIZE - 1) / 2.0
R = SIZE / 2.0 - 0.4
STEP = 2.0 * math.pi / 3.0          # 3 叶
TWO_PI = 2.0 * math.pi

# 清晰单风扇造型: 距离法画连续外圈 + 中心轴 + 3 片细叶(叶尖与圈留缝, 不糊成一团)
RING_R = 9.2      # 外圈半径
RING_T = 0.95     # 外圈半厚(|r-RING_R|<=RING_T 画圈) —— 保证连续成圈
HUB = 1.7         # 中心轴半径
BLADE_HW = math.radians(17.0)   # 叶片角半宽
SWEEP = 0.8       # 叶片弧度(随半径偏转)
BL_IN = 1.9       # 叶片内边界
BL_OUT = 7.2      # 叶片外边界(与圈留 1px 缝)


def pixel_on(px, py, rot):
    dx = px - CX
    dy = py - CY
    r = math.sqrt(dx * dx + dy * dy)
    if abs(r - RING_R) <= RING_T:
        return True                                   # 连续外圈
    if r <= HUB:
        return True                                   # 中心轴
    if BL_IN <= r <= BL_OUT:
        th = math.atan2(dy, dx) + SWEEP * (r / (SIZE / 2.0))
        for b in range(BLADES):
            d = th - (rot + b * STEP)
            d = math.atan2(math.sin(d), math.cos(d))
            if abs(d) < BLADE_HW:
                return True
    return False


def frame_bytes(rot):
    out = []
    for py in range(SIZE):
        row = [0, 0, 0]
        for px in range(SIZE):
            if pixel_on(px, py, rot):
                row[px // 8] |= (0x01 << (px % 8))   # XBM: bit0=最左 (u8g2_DrawHXBMP mask=1<<)
        out.extend(row)
    assert len(out) == FRAME_BYTES
    return out


def main():
    frames = []
    for f in range(FRAMES):
        rot = f * (STEP / FRAMES)   # 0 .. <120°
        frames.append(frame_bytes(rot))

    lines = []
    lines.append("/* 自动生成, 勿手改: scripts/gen_fan_frames.py")
    lines.append(" * 3 叶 20x20 风扇, 24 帧(5°/帧, 覆盖 120° 视觉周期), LSB 在左 (XBM, u8g2_DrawXBMP)。 */")
    lines.append("#ifndef FAN_FRAMES_H_")
    lines.append("#define FAN_FRAMES_H_")
    lines.append("#include <stdint.h>")
    lines.append("#define FAN_FRAME_W      20")
    lines.append("#define FAN_FRAME_H      20")
    lines.append("#define FAN_FRAME_COUNT  24")
    lines.append("static const uint8_t g_fan_frames[FAN_FRAME_COUNT][%d] = {" % FRAME_BYTES)
    for f, fr in enumerate(frames):
        hexs = ",".join("0x%02X" % b for b in fr)
        lines.append("  { %s },  /* frame %d */" % (hexs, f))
    lines.append("};")
    lines.append("#endif /* FAN_FRAMES_H_ */")
    text = "\n".join(lines) + "\n"

    with open("src/nt7534/fan_frames.h", "w", encoding="utf-8", newline="\n") as fp:
        fp.write(text)
    print("wrote src/nt7534/fan_frames.h  frames=%d  frame_bytes=%d  total=%d B"
          % (FRAMES, FRAME_BYTES, FRAMES * FRAME_BYTES))


if __name__ == "__main__":
    main()
