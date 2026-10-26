# -*- coding: utf-8 -*-
"""生成 NT7534 DMA 开发文档（自包含 HTML，内嵌 SVG 思维导图）。"""
import html

# ---------- 思维导图数据 ----------
ROOT = "RA6E2 NT7534 屏\nSPI0 + DMAC 异步刷新"

BRANCHES = [
    {
        "t": "硬件层", "c": "#2a9d8f",
        "ch": [
            "SPI0：P302=SCK，P207=MOSI",
            "DMAC0：g_transfer0（通道0）",
            "NT7534 控制脚：\nCS=P301 / RS=P403 / RES=P409（GPIO）",
            "PCLKA=60MHz → 分频 7.5MHz",
        ],
    },
    {
        "t": "配置层（RASC 生成）", "c": "#e9c46a",
        "ch": [
            "r_spi + r_dmac 两个模块",
            "触发源 ELC_EVENT_SPI0_TXI",
            "单元1字节\n源地址自增 / 目的固定",
            "中断优先级 12",
        ],
    },
    {
        "t": "传输层（nt_bus.c）", "c": "#e76f51",
        "ch": [
            "NT_Bus_Init / Write / WriteAsync / IsIdle",
            "spi0_callback 置完成标志",
            "阻塞写 vs 异步写（忙返回 false）",
        ],
    },
    {
        "t": "刷新泵（bsp_nt7534.c）", "c": "#8e44ad",
        "ch": [
            "双缓冲 g_nt_shadow（DMA 只读它）",
            "8 页状态机 committed/pending/cur",
            "Pump_Start / Poll / Active",
            "ROT 页映射补偿",
        ],
    },
    {
        "t": "应用层（u8g2+nt7534）", "c": "#2980b9",
        "ch": [
            "Flush：全屏 0..7 页",
            "FlushArea：风扇脏区（ROT 反变换）",
            "主循环每 2ms 调 Pump_Poll",
            "1Hz 整屏 + 60fps 风扇",
        ],
    },
    {
        "t": "关键修复", "c": "#c0392b",
        "ch": [
            "unlink 清空 s_data（风扇停）",
            "风扇脏刷页映射反变换",
        ],
    },
    {
        "t": "验证手段", "c": "#27ae60",
        "ch": [
            "probe-rs 读 g_nt_dma_kicks/skip",
            "dump shadow 缓冲截屏比对",
        ],
    },
]

# ---------- 布局计算 ----------
W, H = 880, 800
root_x, root_w, root_h = 20, 220, 90
root_cx = root_x + root_w / 2
root_cy = H / 2

b_x, b_w, b_h = 300, 200, 44
ch_x, ch_w = 520, 290
ch_h1, ch_h2 = 34, 44
ch_gap = 48

n = len(BRANCHES)
b_centers = [90 + i * (H - 180) / (n - 1) for i in range(n)]

def box(x, y, w, h, fill, text, tcolor="#fff", fs=13, bold=True, lines=None):
    rx = x + w / 2
    ry = y + h / 2
    s = f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="9" fill="{fill}"/>'
    if lines is None:
        lines = text.split("\n")
    nlg = len(lines)
    start = ry - (nlg - 1) * 8
    tsp = ""
    for i, ln in enumerate(lines):
        dy = start + i * 16 - ry + (8 if nlg == 1 else 0)
        tsp += f'<tspan x="{rx}" dy="{dy if i==0 else 16}">{html.escape(ln)}</tspan>'
    s += f'<text x="{rx}" y="{ry}" text-anchor="middle" font-size="{fs}" ' \
         f'font-family="Microsoft YaHei, sans-serif" font-weight="{"bold" if bold else "normal"}" ' \
         f'fill="{tcolor}">{tsp}</text>'
    return s

svg = [f'<svg viewBox="0 0 {W} {H}" width="100%" xmlns="http://www.w3.org/2000/svg">']
svg.append('<style>text{fill:#fff}</style>')

# 连接线：root -> branch
for bc in b_centers:
    svg.append(f'<line x1="{root_x+root_w}" y1="{root_cy}" x2="{b_x}" y2="{bc}" '
               f'stroke="#9aa7b3" stroke-width="2"/>')

# 根节点
svg.append(box(root_x, root_cy - root_h/2, root_w, root_h, "#1f3a5f", ROOT, fs=15))

# 分支 + 子节点
for idx, br in enumerate(BRANCHES):
    bc = b_centers[idx]
    svg.append(box(b_x, bc - b_h/2, b_w, b_h, br["c"], br["t"], fs=13))
    chs = br["ch"]
    m = len(chs)
    off = (m - 1) * ch_gap / 2
    for j, ch_text in enumerate(chs):
        cy = bc - off + j * ch_gap
        h = ch_h2 if "\n" in ch_text else ch_h1
        # 连接线 branch -> child
        svg.append(f'<line x1="{b_x+b_w}" y1="{bc}" x2="{ch_x}" y2="{cy}" '
                   f'stroke="{br["c"]}" stroke-width="1.6"/>')
        svg.append(box(ch_x, cy - h/2, ch_w, h, "#ffffff", ch_text,
                       tcolor="#222", fs=11, bold=False))

svg.append('</svg>')

mindmap = "\n".join(svg)

# ---------- 文档正文 ----------
body = """
<h2>一、一句话看懂：DMA 在这里干嘛</h2>
<p>屏（NT7534）每次刷新要搬 1024 字节。如果让 CPU 一个一个字节地"手动"搬到 SPI 线上，
CPU 会被完全占住（旧版软件 SPI 就是这样，风扇每帧要占住主循环 ~2.46 ms）。</p>
<p><b>DMA 就是"雇了个搬运工"</b>：CPU 只要说一句"把这 128 字节从内存搬到 SPI 数据寄存器"，
剩下的搬运由 DMAC 硬件自动完成，CPU 转头去干别的事（轮询 USB/串口、算数据）。
本工程用 <b>SPI0（硬件 SPI）+ DMAC0</b> 组合，并把刷新做成了<b>异步分页状态机 + 双缓冲</b>。</p>

<h2>二、硬件与配置：DMA 是怎么接上的</h2>
<ul>
  <li><b>引脚分工</b>：SCK=P302、MOSI=P207 交给 SPI0 硬件；但 /CS(P301)、RS(P403)、/RES(P409)
      仍用 GPIO 手动控制（NT7534 要求"3 字节页命令 + 128 字节数据"在同一个 CS 低电平窗口内完成，
      所以不能把 CS 交给 SPI0 的自动片选）。</li>
  <li><b>时钟</b>：SPI 挂在 PCLKA=60 MHz 上，分频得到 7.5 MHz（<code>SPBR=3</code> 已用 probe-rs 实测确认）。</li>
  <li><b>RASC 配置</b>（自动生成到 <code>hal_data.c</code>）：
    <pre>g_transfer0_info:
    .mode           = TRANSFER_MODE_NORMAL
    .size           = TRANSFER_SIZE_1_BYTE
    .src_addr_mode  = TRANSFER_ADDR_MODE_INCREMENTED   // 内存地址自增
    .dest_addr_mode = TRANSFER_ADDR_MODE_FIXED          // SPI 数据寄存器地址固定
    .activation_source = ELC_EVENT_SPI0_TXI             // SPI 发空了就触发 DMA 搬下一字节
g_spi0_cfg.p_transfer_tx = &g_transfer0;                // SPI 与 DMA 绑定</pre>
    一句话：<b>SPI 每发完一个字节腾出空间，就自动"踢"DMA 搬下一个字节</b>，无需 CPU 介入。</li>
</ul>

<h2>三、三层代码结构（从下到上）</h2>
<table>
  <tr><th>层</th><th>文件</th><th>职责</th><th>通俗比喻</th></tr>
  <tr><td>传输层</td><td>nt_bus.c</td><td>把"发 N 字节"封装成阻塞/异步两种接口；回调里置完成标志</td><td>搬运工的操作手册</td></tr>
  <tr><td>刷新泵</td><td>bsp_nt7534.c</td><td>8 页状态机 + 双缓冲，把一屏拆成 8 页异步发出</td>导演：一页一页拍，拍完再拍下一页</td></tr>
  <tr><td>应用层</td><td>u8g2_port.c / nt7534.c</td><td>决定"整屏刷"还是"只刷风扇那几页"，并在主循环里推进泵</td><td>编剧：决定这帧拍什么</td></tr>
</table>

<h3>3.1 传输层 nt_bus.c（最关键的两行）</h3>
<pre>bool NT_Bus_WriteAsync(const uint8_t * p, uint32_t n){
    if (s_async_busy) return false;          // 上一笔还在飞 → 直接拒，调用方下轮再试
    ...
    R_SPI_Write(...);                         // 启动 DMA 传输（立即返回，不等待）
    s_async_busy = 1u;
    return true;
}
// SPI 发完 → 中断回调里把标志清掉
void spi0_callback(spi_callback_args_t * a){
    if (a->event == SPI_EVENT_TRANSFER_COMPLETE){ s_spi_done=1u; s_async_busy=0u; }
}</pre>
<p>注意 <b>异步写不等完成</b>——它只是"下单"，真正搬数据的是 DMA。这就是 CPU 不被占住的原因。</p>

<h3>3.2 刷新泵 bsp_nt7534.c（异步分页状态机）</h3>
<p>一屏 64 行、每页 8 行，共 8 页。泵的工作方式：</p>
<ul>
  <li><code>Pump_Start(0,7)</code>：请求刷新全部 8 页（或 <code>StartBitmap(mask)</code> 只刷指定页）。</li>
  <li>主循环每 2 ms 调一次 <code>NT7534_Pump_Poll()</code>：如果当前页 DMA 发完了，就拉高 CS、挑下一页继续发；
      如果上一笔还在飞，就立刻返回（让出 CPU）。</li>
  <li>三个标志位像"待办清单"：<code>s_committed</code>=已拷进缓冲待发的页，
      <code>s_pending</code>=新请求排队页，<code>s_cur</code>=正在发的页。忙时来的请求进 <code>s_pending</code>，
      <b>不丢帧、不跳过</b>。</li>
</ul>

<h3>3.3 双缓冲：为什么数值不再闪</h3>
<p>早期 bug：DMA 直接读 live 帧缓冲 <code>g_u8g2_fb</code>，但渲染任务在 DMA 飞的时候又在改这个缓冲，
导致"一屏里一半旧一半新"→ 看到闪烁/撕裂。</p>
<p><b>修法</b>：加一个影子缓冲 <code>g_nt_shadow[1024]</code>。<b>DMA 永远只读 shadow</b>；
只有在"泵空闲 / 刚发完一页"的安全边界，才把 live 缓冲按页 <code>memcpy</code> 进 shadow。
渲染随便改 live，永远碰不到正在发送的 shadow → 闪烁根因消除。</p>
<p style="background:#f3f6fb;padding:8px;border-left:4px solid #2980b9">
<b>打个比方</b>：以前是"直播改剧本还同步放映"，现在改成"导演先把一整页拍进胶片(shadow)，
放映机(DMA)只放胶片，编剧改 live 剧本不影响正在放的内容"。</p>

<h2>四、一次刷屏的完整数据流</h2>
<pre>nt7534.c 主循环（每 2ms 一轮）
   │
   ├─ 1Hz：画好整屏 → U8G2_Port_Flush()
   │        → NT7534_Pump_Start(0,7)   // 请求刷 8 页
   │
   ├─ 60fps：只改风扇格 → U8G2_Port_FlushArea(6,4,10,4)
   │        → ROT 反变换得到物理页 0..3 → Pump_StartBitmap(mask)
   │
   └─ NT7534_Pump_Poll()  // 每轮推一页
          └─ 空闲且有页要发：
               ① nt_pump_commit()：live→shadow 拷这一页
               ② nt_pump_send_page()：发 3 字节页命令(CS低/RS命令) + 128 字节数据(异步DMA)
               ③ DMA 在后台搬；CPU 立刻返回干别的
               ④ DMA 发完 → 回调置 s_async_busy=0
               ⑤ 下一轮 Poll 发现空闲 → 拉高 CS，发下一页</pre>

<h2>五、两个关键坑（都修过）</h2>
<ol>
  <li><b>linked→unlink 风扇不停</b>：断链后没人清旧数据，屏上残留读数、风扇按旧转速继续转。
      修法：<code>UART_Link_Tick()</code> 在 <b>2 秒收不到合法帧</b> 的边界把 <code>s_data</code> 全部置 -1，
      风扇门控 <code>if(cf>=0||gf>=0)</code> 失效 → 风扇静止、显示 <code>--</code>/<code>unlink</code>。</li>
  <li><b>风扇"扇叶在跳"不是转动</b>：<code>FlushArea</code> 把 u8g2 的页号直接当物理页刷，
      结果刷的是上半屏（没有风扇）。风扇只能靠每秒整屏才上屏 → 每秒跳一格。
      修法：<b>对页号做 ROT 反变换</b> <code>phys=(p+8-ROT)&7</code>，让脏刷正确落到风扇所在的物理页。</li>
</ol>

<h2>六、怎么验证 DMA 真的在跑</h2>
<p>用 probe-rs 直读几个 volatile 计数器（编译后地址会变，读前先 <code>arm-none-eabi-nm</code> 取址）：</p>
<pre>g_nt_dma_kicks  // 成功发起的异步 DMA 页数，应持续增长
g_nt_dma_skip   // 被跳过的刷新（正常应为 0）
g_u8g2_frames   // 刷新帧数，涨说明界面在实时更新</pre>
<p>另外可 <code>python tools/screen_shot.py --addr &lt;g_nt_shadow&gt;</code> 把 DMA 实际发送的影子缓冲
dump 成图，确认布局无花屏/撕裂。</p>

<h2>七、改动文件速查</h2>
<table>
  <tr><th>文件</th><th>作用</th></tr>
  <tr><td>src/nt7534/nt_bus.c / .h</td><td>新增传输层抽象（软件SPI/SPI0阻塞/SPI0+DMA 三档）</td></tr>
  <tr><td>src/nt7534/bsp_nt7534.c</td><td>双缓冲 + 8 页异步刷新泵状态机</td></tr>
  <tr><td>src/nt7534/u8g2_port.c</td><td>Flush / FlushArea（含 ROT 反变换）</td></tr>
  <tr><td>src/nt7534/nt7534.c</td><td>主循环每 2ms 调 Pump_Poll；风扇/整屏调度</td></tr>
  <tr><td>src/uart/bsp_uart_link.c</td><td>unlink 清空 s_data 修复</td></tr>
  <tr><td>configuration.xml + ra_gen/hal_data.*</td><td>RASC 引入 r_spi / r_dmac 模块与绑定</td></tr>
</table>
<p style="color:#666">切换档位只需改 <code>bsp_nt7534.h</code> 里的 <code>NT7534_BUS_MODE</code>：
<code>2</code>=SPI0+DMA（现用）、<code>1</code>=SPI0 阻塞（秒降级）、<code>0</code>=软件 SPI 位打（原始行为）。</p>
"""

doc = f"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<title>NT7534 屏 DMA 开发文档（RA6E2）</title>
<style>
  body{{font-family:"Microsoft YaHei",sans-serif;max-width:960px;margin:24px auto;padding:0 20px;color:#222;line-height:1.7}}
  h1{{color:#1f3a5f;border-bottom:3px solid #1f3a5f;padding-bottom:8px}}
  h2{{color:#1f3a5f;margin-top:34px}}
  h3{{color:#2980b9;margin-top:22px}}
  table{{border-collapse:collapse;width:100%;margin:12px 0;font-size:14px}}
  th,td{{border:1px solid #cfd8e3;padding:8px 10px;text-align:left;vertical-align:top}}
  th{{background:#eef3f8}}
  pre{{background:#1e1e2e;color:#e6e6e6;padding:12px 14px;border-radius:8px;overflow:auto;font-size:13px;line-height:1.5}}
  code{{background:#eef3f8;padding:1px 5px;border-radius:4px;color:#c0392b;font-size:13px}}
  .mm{{border:1px solid #e2e8f0;border-radius:10px;padding:10px;background:#fafcff;margin:10px 0}}
  .foot{{color:#888;font-size:12px;margin-top:30px;border-top:1px solid #eee;padding-top:10px}}
</style>
</head>
<body>
<h1>RA6E2 NT7534 屏 · SPI0 + DMAC 异步刷新开发文档</h1>
<p style="color:#555">面向「想看懂本项目怎么用 DMA 刷屏」的开发者。代码与结构对齐已提交版本
（<code>NT7534_BUS_MODE=2</code>，7.5 MHz）。</p>

<div class="mm">
<h3 style="margin-top:6px">🧠 思维导图（图片渲染）</h3>
{mindmap}
</div>
{body}
<div class="foot">由开发会话自动生成 · 代码以仓库实际提交为准 · 验证手段依赖 probe-rs 与 screen_shot.py</div>
</body>
</html>"""

with open(r"E:\REA6E2_Dev\docs\NT7534_DMA开发文档.html", "w", encoding="utf-8") as f:
    f.write(doc)
print("written, bytes=", len(doc))
