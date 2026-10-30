#ifndef RA6E2_TRNG_H
#define RA6E2_TRNG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* RA6E2 片上 TRNG 可复用驱动（裸寄存器，不依赖 r_sce）。
 *
 * 为什么不是 FSP 的 r_trng：FSP 6.6.0 没有独立 r_trng 驱动，TRNG 挂在 SCE 内，
 * 仅 r_sce 的 HW_SCE_RNG_Read() 暴露，且它不自行开时钟、又是私有函数。
 * 本模块做最小且正确的事：开 TRNG 时钟 + 解/上 LPC 保护 + 带超时读寄存器。
 *
 * ⚠ 与 SCE 互斥：若项目引入 r_sce 加密栈，TRNG 归 SCE 管，不要再手动戳寄存器，改走 SCE/PSA 熵源。
 * ⚠ 原始 TRNG 未做健康检测/DRBG，适合游戏抖动、salt、非安全随机；
 *    密钥/IV/token 等加密用途请走 SCE/PSA，不要直接拿它当密钥材料。 */

/* 打开 TRNG 模块时钟（幂等）。所有读操作前调用一次即可。
 * 内部处理 LPC 寄存器保护的解/上锁。未 init 时 trng_u32/trng_bytes 也会防御性自动 init。 */
void trng_init(void);

/* 关闭 TRNG 模块时钟省电。确认无其它使用者时调用。 */
void trng_deinit(void);

/* 取 32 位真随机数。成功返回 true 并写入 *out；失败(超时/硬件异常)返回 false，*out 不变。
 * 注意：绝不用“返回 0 = 失败”——0 是合法随机值，会和无随机混淆。 */
bool trng_u32(uint32_t *out);

/* 取 n 字节真随机，全部成功才返回 true。 */
bool trng_bytes(uint8_t *buf, size_t n);

/* 调试/自检：最近一次读值（0 = 上次失败）。可用 probe-rs 读 RAM 验证。 */
uint32_t trng_last_value(void);

#ifdef __cplusplus
}
#endif

#endif /* RA6E2_TRNG_H */
