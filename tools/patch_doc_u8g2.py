#!/usr/bin/env python3
"""在规划文档里补 u8g2 接入章节（§4.6），并同步 §0 / §5.4 的表述。"""
import io

P = 'docs/3.5寸屏开发文档.md'
s = io.open(P, encoding='utf-8').read()

SEC = '''### 4.6 绘图层改用 u8g2（2026-10-22 已接入并实测出图）

**结论：u8g2 官方支持 NT7534**（README 的支持列表里就有），`u8x8_d_nt7534_tg12864r`
就是给 12864 STN 这种屏写的，注释里甚至写了"这块玻璃行方向整体错位"
（*The TG12864R display is also shifted in lines*）。所以画点/画字/画框/画圆不用自己写。

**只拷需要的文件**（`third_party/u8g2/`，796 KB 源码）：

| 拷了 | 没拷 | 为什么 |
|---|---|---|
| `u8g2_*.c` 核心 + `u8x8_*.c` + `u8x8_d_st7565.c` | `u8g2_fonts.c`（**39 MB**，全部 2000+ 字体） | 只要 1 个字体，单独拷 `single_font_files/` 里的 1.99 KB 文件 |
| `u8g2_font_7x13_tf.c` | `u8g2_d_setup.c`（467 KB，2174 个屏型） | `u8g2_SetupDisplay` 本身就是宏，自己写 8 行 setup 即可 |

flash 代价：**22,116 → 28,596 字节（+6.5 KB）**。链接开了 `--gc-sections`，没用到的函数与字库不进镜像。

**移植层 `src/nt7534/u8g2_port.c` 只做三件事：**

1. `U8X8_MSG_BYTE_SEND` → `NT7534_BusWrite()`（软件 SPI，时序仍是原来那套 NOP 半位延时）
2. `U8X8_MSG_GPIO_CS / DC / RESET` → 三个引脚
3. **页地址补偿**：命令字节落在 `0xB0..0xB7`（set page）时改写为 `0xB0 | ((page + 4) & 7)`

⚠ 三个必须知道的点：

| 点 | 说明 |
|---|---|
| **不调 `u8g2_InitDisplay()`** | u8g2 内置 TG12864R 序列是 `0xA0 / 0xC8 / 起始行 0x60`，方向与这块屏相反；而起始行命令对本模块无效（§8 R10）。硬件初始化仍旧走 `NT7534_Init()`，只借 u8g2 的缓冲与绘图。 |
| **`x_offset` 必须覆盖** | u8g2 按 lm6059 给的默认是 1，本模块实测 **3**：`u8g2.u8x8.x_offset = NT7534_COL_OFFSET` |
| **0xB8..0xBF 不是页命令** | 拦截用 `(b & 0xF8) == 0xB0` 精确匹配，否则 `0xBB`(set V0) 之类会被当成页号 |

**字体选 `u8g2_font_7x13_tf`**（1.99 KB，191 字形，ASCII + Latin-1 全覆盖）：
它**自带 `°`（U+00B0）字形**。所以 `℃` 不再需要"小圈 + 大写 C"合成 —— 位置由字体决定，
手画导致的"圈偏左/偏右"从此不会再有。字符串写法有个坑：

```c
#define U8G2_DEG_C   "\\xC2\\xB0" "C"      /* 必须拆成两段: "\\xB0C" 里的 C 会被并进转义序列 */
u8g2_DrawUTF8(g, 0, y, "CPU 48" U8G2_DEG_C " 12% 1050");
```

**看不见屏也能截图**（`tools/screen_shot.py`）：u8g2 的帧缓冲故意做成全局数组 `g_u8g2_fb`（1024 B），
探针 dump 出来就能在电脑上还原成 BMP，判排版、判字符位置不必再靠目视：

```
python tools/screen_shot.py --addr 0x20001a08      # 地址每次重编译会变, nm 现取
```

实测出图（无串口数据时的样子）：

```
CPU --°C --% ----
GPU --°C --% ----
RAM --%
v6 NO LINK
```

---

'''

anchor = '## 5. 显示布局设计（128×64）'
s = s.replace(anchor, SEC + anchor, 1)

# §5.4 更新: 不需要造字了
s = s.replace('''现有 `oled_font.h` 已有 8×16 的 ASCII 字库（`asc2_1608`），方案 A 直接可用，**不用造字**。
方案 B 需要 6×8 字库，要另外找/生成（Phase 4 可选）。''',
'''~~需要新增字模~~ —— **2026-10-22 起不需要了**：界面已改用 u8g2（§4.6），字体直接用
`u8g2_font_7x13_tf`（1.99 KB），要换字号只是换一个字体数组，不必再造字。
`oled_font.h` 的 8×16 字库仍保留，供 1.3" OLED 与 3.5" 的回退路径用。''')

# §0 一页结论: 补一行
s = s.replace('| 驱动复用度 | 与现有 SH1106 驱动**同构**（132 列 × 8 页、页寻址、1=亮），绘图层几乎照搬 |',
'''| 驱动复用度 | 与现有 SH1106 驱动**同构**（132 列 × 8 页、页寻址、1=亮） |
| 绘图层 | **2026-10-22 已换 u8g2**（官方自带 NT7534 驱动），真 `°` 字形、线段/矩形/圆/位图全有（§4.6） |''')

io.open(P, 'w', encoding='utf-8', newline='\n').write(s)
print('doc ok, size =', len(s.encode('utf-8')))
