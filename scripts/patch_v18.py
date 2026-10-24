# -*- coding: utf-8 -*-
"""v18：删除 3.5\" 屏的 6 段自检，USB 提到最前面初始化。

一次性脚本（改完可删）。纯粹做文本替换，不做语义判断。
"""
import io

P = "src/nt7534/nt7534_test.c"
s = io.open(P, encoding="utf-8").read()

START = "    /* \u2b50 v17: **USB / \u4e32\u53e3\u94fe\u8def\u63d0\u5230\u5c4f\u5e55\u81ea\u68c0\u4e4b\u524d\u521d\u59cb\u5316**\u3002"
END = "    while (1)\n    {\n        uint16_t k;"

i = s.index(START)
j = s.index(END)

NEW = r'''    /* ⭐ v18：**USB / 串口链路最先初始化，屏幕自检已删除**。
     *
     * 历史：v17 之前 `USB_CDC_Init()` 排在 6 段屏幕自检之后，而自检光等待就有
     * 13.7 s（2500+1200+3000+3000+4000）⇒ 复位/拔插后主机要等十几秒才看到设备。
     * 3.5" 屏的显示部分已经验收完毕（版面 A + linked/unlink 都稳定），
     * 那些"全亮/全灭/条纹/棋盘/边框"自检画面已经没有价值 ⇒ 整体删除。
     *
     * 现在开机路径：初始化 USB → 初始化屏 → 直接进版面 A 主循环。
     *
     * ⚠ 必须放在 `OSC_Probe_Run()` 之后：晶振探测要求 P212/P213 无任何负载。
     */
    (void) UART_Link_Init();

    /* Type-C 虚拟串口: 板上没有 UART 桥芯片, Type-C 直连 P814/P815 原生 USB，
     * 所以数据通路必须由固件实现 CDC。失败也不影响屏幕（会一直显示 unlink）。 */
    s_usb_started = (FSP_SUCCESS == USB_CDC_Init());
    RTT_Printf("usb cdc init: %s\n", s_usb_started ? "OK" : "FAILED");

    /* 1.3" OLED 只当"第二指示灯"：3.5" 屏黑屏时，它上面有字就说明 MCU 起来了。 */
    s_oled_ok = (FSP_SUCCESS == OLED_Init());

    /* 3.5" 屏初始化 */
    RTT_Printf("nt7534 init: start_line=0x%02X col_offset=%u contrast=0x%02X\n",
               (unsigned) (0x40u | NT7534_START_LINE), (unsigned) NT7534_COL_OFFSET,
               (unsigned) NT7534_CONTRAST);
    (void) NT7534_Init();
    RTT_Printf("nt7534 init done, bytes_sent=%lu\n", (unsigned long) g_nt_bytes_sent);
    RTT_Printf("layout A live, col_offset=%u page_rot=%u\n",
               (unsigned) NT7534_COL_OFFSET, (unsigned) NT7534_PAGE_ROT);

'''

s = s[:i] + NEW + s[j:]
io.open(P, "w", encoding="utf-8", newline="\n").write(s)
print("已替换启动段；文件行数 =", s.count("\n") + 1)
