/* ============================================================
 * 补丁: FSP 6.x 缺失的 bsp_mcu_ofs_cfg.h
 *
 * 为什么需要
 * ----------
 * FSP 6.x 新增 OFS(Option Function Select) 逐项配置机制,
 * ra_cfg/fsp_cfg/bsp/bsp_cfg.h 会 #include "bsp_mcu_ofs_cfg.h"。
 * 这个文件按设计应由 RASC 生成 —— 但只有 configuration.xml 里启用过
 * OFS 组件才会生成。本工程的 XML 源自 5.8.0(当时没有 OFS 机制),
 * 所以 rascc --generate 之后它不存在, 导致 6.6.0 全树编译失败
 * (29 个目标全部卡在 bsp_cfg.h 第 10 行)。
 *
 * 为什么这里是空的
 * ----------------
 * ra/fsp/src/bsp/mcu/<mcu>/bsp_linker.c 里用的是:
 *     #if defined BSP_CFG_OPTION_SETTING_OFS0
 *         static const uint32_t BSP_PLACE_IN_SECTION(".option_setting_ofs0") ...
 * 不定义任何 BSP_CFG_OPTION_SETTING_* 宏 => 不生成 OFS 数据 =>
 * .option_setting_* 段为空 => ELF 里干脆没有这些段。
 *
 * 安全性(重要)
 * ------------
 * OFS 区控制 SWD 调试口锁定 / 独立看门狗 / HOCO 频率, 写错会锁死调试口。
 * 不定义宏 = 完全不写这段 = 沿用芯片上现有的值。
 * 实测本板: OFS0 = 全 0xFF, SAS = 全 0xFF, OFS1 = 0xFFFFFDFF(仅 HOCO 20MHz),
 * 无调试锁定位。且 6.6.0 产物里没有 option-setting 段, 烧录天然不会碰它。
 * 相比 5.8.0(ELF 里带 5 个 option-setting 段、要靠 --skip-section 跳过),
 * 6.6.0 这条路反而更安全。
 *
 * 这个文件不写进 RASC 工程目录, 而是通过 CMake 的 include 路径提供,
 * 这样 rascc 重新生成工程时不会被删掉。
 * ============================================================ */
#ifndef BSP_MCU_OFS_CFG_H_
#define BSP_MCU_OFS_CFG_H_

/* 有意留空: 不使用 OFS 配置, 不改写芯片上已有的 option-setting 区 */

#endif /* BSP_MCU_OFS_CFG_H_ */
