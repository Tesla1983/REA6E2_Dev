# -*- coding: utf-8 -*-
"""重写 nt7534_test.c 的方案A布局与主循环（一次性工具脚本）。"""
import io

P = 'src/nt7534/nt7534_test.c'
s = io.open(P, encoding='utf-8').read()

NEW_LAYOUT = '''#if (0 != NT7534_OFFSET_SWEEP)
/** 边框 + 满宽 16 字符: 列偏移错时, 一边掉字、另一边空一截, 一眼能看出来 */
static void draw_offset_test(void)
{
    NT7534_Clear();
    NT7534_DrawRect(0, 0, (int16_t) (NT7534_WIDTH - 1u), (int16_t) (NT7534_HEIGHT - 1u));
    NT7534_DrawString(0, 8,  "0123456789ABCDEF");
    NT7534_DrawString(0, 32, "0123456789ABCDEF");
    NT7534_Refresh();
}
#endif

/** 2 位十进制（0..99）写进 p[0..1]。v < 0（本帧没有该项）时写 "--"。
 *  不用 snprintf: 这里要的是"定长 2 字符"的排版, 值超范围时宁可钳位也不让行宽变化。 */
static void fmt2(char * p, int16_t v)
{
    if (v < 0)
    {
        p[0] = '-';
        p[1] = '-';
        return;
    }
    if (v > 99)
    {
        v = 99;
    }
    p[0] = (char) ('0' + (char) (v / 10));
    p[1] = (char) ('0' + (char) (v % 10));
}

/** 4 位十进制（0..9999）写进 p[0..3]。无数据时写 "----"。 */
static void fmt4(char * p, int16_t v)
{
    if (v < 0)
    {
        p[0] = '-'; p[1] = '-'; p[2] = '-'; p[3] = '-';
        return;
    }
    if (v > 9999)
    {
        v = 9999;
    }
    p[0] = (char) ('0' + (char) (v / 1000));
    p[1] = (char) ('0' + (char) ((v / 100) % 10));
    p[2] = (char) ('0' + (char) ((v / 10) % 10));
    p[3] = (char) ('0' + (char) (v % 10));
}

/** 拼"标签 + 温度℃ + 占用% + 风扇"一行（正好 16 字符 = 128 px）。
 *  例: `CPU 48℃ 12% 1050` —— ℃ 由驱动合成, 宽度仍算 1 个字符。 */
static void build_metric_line(char * line, const char * p_label, int16_t temp,
                              int16_t usage, int16_t fan)
{
    line[0] = p_label[0];
    line[1] = p_label[1];
    line[2] = p_label[2];
    line[3] = ' ';
    fmt2(&line[4], temp);
    line[6] = NT7534_CH_DEGC;
    line[7] = ' ';
    fmt2(&line[8], usage);
    line[10] = '%';
    line[11] = ' ';
    fmt4(&line[12], fan);
    line[16] = @@NUL@@;
}

/** 画方案 A 的一帧（真实数据, 来自串口链路）。
 *  行 0 CPU / 行 1 GPU / 行 2 RAM / 行 3 链路状态。每行 16 字符铺满 128 px。 */
static void draw_layout_a(void)
{
    const uart_link_data_t * d = UART_Link_Data();
    char line[17];

    NT7534_Clear();

    build_metric_line(line, "CPU", d->ct, d->cu, d->cf);
    NT7534_DrawString(0, 0, line);

    build_metric_line(line, "GPU", d->gt, d->gu, d->gf);
    NT7534_DrawString(0, 16, line);

    line[0] = 'R'; line[1] = 'A'; line[2] = 'M'; line[3] = ' ';
    fmt2(&line[4], d->mu);
    line[6] = '%';
    line[7] = @@NUL@@;
    NT7534_DrawString(0, 32, line);

    /* 状态行: 固件版本 + 链路状态 + 已收到的帧数。
     * "v3" = 固件版本（对应 g_nt_fw_tag）; 帧数在涨 ⇒ 画面在实时刷新, 不是屏保留的残留图像。 */
    if (UART_Link_Online())
    {
        line[0] = 'v'; line[1] = '3'; line[2] = ' ';
        line[3] = 'L'; line[4] = 'K'; line[5] = ' ';
        fmt4(&line[6], (int16_t) (g_link_frames % 10000u));
        line[10] = @@NUL@@;
    }
    else
    {
        line[0] = 'v'; line[1] = '3'; line[2] = ' ';
        line[3] = 'N'; line[4] = 'O'; line[5] = ' ';
        line[6] = 'L'; line[7] = 'I'; line[8] = 'N'; line[9] = 'K';
        line[10] = @@NUL@@;
    }
    NT7534_DrawString(0, 48, line);

    NT7534_Refresh();
}

'''

NEW_LOOP = '''    RTT_Printf("STAGE 6/6: layout A live, col_offset=%u page_rot=%u\\n",
               (unsigned) NT7534_COL_OFFSET, (unsigned) NT7534_PAGE_ROT);

    /* 串口链路: SCI0 115200 8N1, 收上位机每秒一帧。打不开也要继续跑,
     * 屏幕显示 NO LINK —— 不能因为串口有问题就整块屏停摆。 */
    (void) UART_Link_Init();

    while (1)
    {
        uint8_t k;

        UART_Link_Poll();                /* 先解析已到的字节, 再画 */
        s_frame_no++;
        draw_layout_a();

        if (0u == (uint32_t) (s_frame_no % 15u))
        {
            RTT_Printf("frames=%lu rx=%lu xor_err=%lu online=%u\\n",
                       (unsigned long) g_link_frames, (unsigned long) g_link_rx_bytes,
                       (unsigned long) g_link_err_xor, (unsigned) UART_Link_Online());
        }

        /* 1 s: 4 段 250 ms, 每段之间继续收字节 + 记接收超时 */
        for (k = 0u; k < 4u; k++)
        {
            R_BSP_SoftwareDelay(250u, BSP_DELAY_UNITS_MILLISECONDS);
            UART_Link_Poll();
            UART_Link_Tick(250u);
        }
    }
'''

s = s.replace('@@NUL@@', "'" + chr(92) + "0'")

a0 = s.index('/** 演示数据')
a1 = s.index('/* ---------------- 主流程 ---------------- */')
s = s[:a0] + NEW_LAYOUT + s[a1:]

b0 = s.index('    RTT_Printf("STAGE 6/6: layout A (col_offset=%u)')
b1 = s.index('#endif\n}', b0)
s = s[:b0] + NEW_LOOP + s[b1:]

io.open(P, 'w', encoding='utf-8', newline='\n').write(s)
print('written', len(s))
