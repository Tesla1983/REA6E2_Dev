#!/usr/bin/env bash
# ============================================================
# 烧录固件到 RA6E2 (probe-rs + FireDAP CMSIS-DAP)
#
#   ./scripts/flash.sh                 # 烧录 build/ra6e2_fw.elf (6.6.0 产物)
#   ./scripts/flash.sh --verify        # 烧录并回读校验
#   ./scripts/flash.sh --elf <path>    # 烧指定 elf (会做安全检查)
#   ./scripts/flash.sh --check-only    # 只做安全检查, 不烧
#
# ✅ 默认**自动复位**(DO_RESET=1, 见下方说明):
#   本机现唯一探针是 FireDAP(0416:5021), 复位行为稳定 —— 烧完直接 reset 唤醒内核跑固件,
#   不再需要物理拔插 Type-C。
#   ⚠ 保留事实: `probe-rs reset` 只复位内核、不重走 USB 的 VBUS 枚举, 复位后 USB 仍需等
#     Windows 重新登记 SERIALCOMM(实测约 30 s, 与物理拔插同量级 —— 这是板子本质,
#     与探针无关)。需要干净重枚举时可显式 `--no-reset` 改回拔插。
#
# 推荐配方 (2026-10-25 起, FireDAP 为唯一探针; 实测可成):
#   ./scripts/flash.sh --speed 1000 --verify
#   (探针就是默认值 0416:5021, 不用再写 --probe)
#   不稳时依次: --speed 400 --no-dblbuf   → --speed 100 --no-dblbuf
#
# 安全闸门: 默认拒绝烧录「含 option-setting 段」的 elf。
#   6.6.0 产物 (build/) 不含这些段 —— 烧它绝不会碰 0x0100A100 区, 不会锁死 SWD。
#   5.8.0 产物 (build58/) 含 5 个这样的段, 必须显式 --allow-ofs,
#   脚本会自动补 --skip-section 跳过它们。详见文档 §6.0。
#
# ⚠ 会擦除片上现有程序。执行前确认板是 RA6E2 那一块。
# ⚠⚠ 探针 (2026-10-25 用户最新确立, 覆盖此前"禁止 FireDAP"的旧规矩):
#   当前机器上**只有 FireDAP(0416:5021)** 这一个可用探针, 所以
#   **烧哪个开发板都用它**(RA6E2 / STM32H743 等一律如此), 它就是默认值。
#   旧说明(DAPLink 0d28:0204 为唯一可用、FireDAP 禁用)已作废 —— 那只 DAPLink 已不在本机。
#   仍可显式指定别探针: RA_PROBE=<vid:pid> 或 --probe <vid:pid>。
#   要看当前哪些探针活着:
#       probe-rs list
#       python tools/board_id.py
# ⚠ ESP32-S3(303a:1001) 只有 ESP 的 flash 算法, 烧不了 RA6E2。
# ============================================================
set -euo pipefail

export PATH="/c/Windows/System32:/c/Windows:$PATH"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PR="$(command -v probe-rs || true)"
[[ -z "$PR" ]] && PR="E:/.cargo/bin/probe-rs.exe"

OBJDUMP="$(command -v arm-none-eabi-objdump || true)"
[[ -z "$OBJDUMP" ]] && OBJDUMP="E:/software/ArmGNUToolchain/14.3 rel1/bin/arm-none-eabi-objdump.exe"

CHIP="R7FA6E2BB"          # 256 KB flash 变体
PROBE="${RA_PROBE:-0416:5021}"   # 默认 FireDAP: 本机当前唯一可用探针(见文件头)
ELF="$ROOT/build/ra6e2_fw.elf"

VERIFY=0
ALLOW_OFS=0
CHECK_ONLY=0
NO_DBLBUF=0
CONNECT_UNDER_RESET=0
# ✅ 烧完是否自动 `probe-rs reset`。**默认开（DO_RESET=1）**，2026-10-25 修订:
#   旧铁律"禁止 probe-rs reset"是针对**报废的旧 DAPLink(0d28:0204)**(一碰 SWD 就断言、
#   reset 会把探针状态搅乱)时代的结论, 现已过期。本机现唯一探针 FireDAP(0416:5021) 复位行为正常,
#   烧完直接 reset 唤醒内核即可, 不必再拔插 Type-C。
#   ⚠ 保留事实: `probe-rs reset` **只复位内核**, **不会**重走 USB 的 VBUS 检测 / D+ 上拉 / 枚举。
#     复位后 USB 仍需等 Windows 重新登记 SERIALCOMM(实测约 30 s, 与物理拔插同量级 —— 这是板子本质,
#     换任何探针都成立)。所以 reset 后 COM 要等 ~30 s 才回来, 属正常。
DO_RESET=1
# ⚠ SWD 速率（kHz）。默认 1000 而不是 probe-rs 的自动值:
#   2026-10-23 用 DAPLink 烧这块板时, 默认速率下 DAP_Transfer 反复 USB 超时
#   (`Timeout in USB communication` / `Failed to write register DRW`),
#   超时还会把探针自己卡死(表现为 F:/ASSERT.TXT 重现 + DAP_Info 零应答)。
#   降速后与 rtt.sh 的 --speed 1000 一致。仍不稳就再降: --speed 400 / 100。
SPEED="${RA_SPEED:-1000}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --verify)             VERIFY=1;      shift ;;
        --probe)              PROBE="$2";    shift 2 ;;
        --elf)                ELF="$2";      shift 2 ;;
        --speed)              SPEED="$2";    shift 2 ;;
        --allow-ofs)          ALLOW_OFS=1;   shift ;;
        --check-only)         CHECK_ONLY=1;  shift ;;
        --no-dblbuf)          NO_DBLBUF=1;   shift ;;
        --connect-under-reset) CONNECT_UNDER_RESET=1; shift ;;
        --reset)              DO_RESET=1;     shift ;;
        --no-reset)           DO_RESET=0;     shift ;;
        -h|--help)            sed -n '2,24p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "未知参数: $1 (用 --help 看用法)"; exit 1 ;;
    esac
done

if [[ -z "$PROBE" ]]; then
    echo "✘ 没有指定探针。当前本机唯一可用探针是 FireDAP(0416:5021):" 1>&2
    echo "      ./scripts/flash.sh --probe 0416:5021 ..." 1>&2
    echo "" 1>&2
    echo "  当前 probe-rs 能看到的:" 1>&2
    "$PR" list 2>&1 | sed 's/^/      /' 1>&2
    exit 1
fi

if [[ ! -f "$ELF" ]]; then
    echo "找不到 $ELF, 请先运行 ./scripts/build.sh"
    exit 1
fi

# ---- 安全闸门: 检查 elf 是否带 option-setting 段 ----
# 注意: 不用 sort —— PATH 里 System32 在前, sort 会命中 Windows 的 sort.exe
OFS_SECS="$("$OBJDUMP" -h "$ELF" 2>/dev/null \
    | grep -oE '\.(option_setting|id_code)[a-z0-9_]*' | awk '!seen[$0]++' || true)"

SKIP=()
if [[ -n "$OFS_SECS" ]]; then
    if [[ "$ALLOW_OFS" -eq 0 ]]; then
        echo "✘ 拒绝烧录: 该 elf 含 option-setting 段, 烧它可能改写 0x0100A100 区并锁死 SWD。" 1>&2
        echo "  检测到的段: $(echo $OFS_SECS | tr '\n' ' ')" 1>&2
        echo "" 1>&2
        echo "  日常请用 6.6.0 产物:  $ROOT/build/ra6e2_fw.elf" 1>&2
        echo "  确实要烧这份, 加 --allow-ofs (脚本会自动 --skip-section 跳过这些段)。" 1>&2
        exit 1
    fi
    echo "⚠ 已 --allow-ofs: 将跳过以下段"
    while read -r sec; do
        [[ -z "$sec" ]] && continue
        echo "    $sec"
        SKIP+=(--skip-section "$sec")
    done <<< "$OFS_SECS"
fi

EXTRA=()
[[ "$VERIFY" -eq 1 ]]     && EXTRA+=(--verify)
[[ "$NO_DBLBUF" -eq 1 ]]  && EXTRA+=(--disable-double-buffering)
[[ "$CONNECT_UNDER_RESET" -eq 1 ]] && EXTRA+=(--connect-under-reset)

echo ">>> 目标: $CHIP   探针: $PROBE   速率: ${SPEED} kHz"
echo ">>> 文件: $ELF"
echo ">>> OFS  : $([[ -n "$OFS_SECS" ]] && echo "有段, 已跳过 $((${#SKIP[@]} / 2)) 个" || echo "无 option-setting 段 (6.6.0 产物, 安全)")"

if [[ "$CHECK_ONLY" -eq 1 ]]; then
    echo ">>> --check-only: 检查通过, 不烧录。"
    exit 0
fi

"$PR" download --chip "$CHIP" --probe "$PROBE" --speed "$SPEED" \
    "${SKIP[@]}" "${EXTRA[@]}" "$ELF"
RC=$?

# ✅ 烧完内核可能停在 halt: USB 会停、COM 口消失、屏不刷新。
# 换 FireDAP(0416:5021) 后默认自动 reset 唤醒内核, 无需拔插。
# ⚠ 保留事实: `probe-rs reset` 只复位内核, 不重走 USB VBUS 枚举,
#   复位后 USB 仍需等 Windows 重新登记 SERIALCOMM(实测约 30 s, 与物理拔插同量级)。
if [[ $RC -eq 0 ]]; then
    if [[ "$DO_RESET" -eq 1 ]]; then
        echo ">>> 烧录/校验成功, 自动复位运行(FireDAP 行为正常)..."
        "$PR" reset --chip "$CHIP" --probe "$PROBE" --speed "$SPEED" 2>&1 | tail -3
        echo ">>> ⏳ 复位后 USB 需等 Windows 重新登记 SERIALCOMM(约 30 s)才出 COM, 属正常。"
    else
        echo ">>> 烧录/校验成功。"
        echo ">>> ⚠ 显式 --no-reset: 内核停在 halt, USB 不会枚举。"
        echo ">>>   要让固件跑起来, 拔插一次 Type-C(真正冷启动)。"
    fi
else
    echo "" 1>&2
    echo "✘ 烧录失败 (rc=$RC)。" 1>&2
    echo "  常见原因与处置:" 1>&2
    echo "   1) SWD 速率过高 ⇒ 再降一档重来: --speed 400   然后 --speed 100" 1>&2
    echo "   2) 双缓冲在小容量 flash 上超时 ⇒ 加 --no-dblbuf" 1>&2
    echo "   3) 内核被留在 halt (COM 口消失/屏不刷新) ⇒ 重烧一次, 或板子断电再上电" 1>&2
    echo "   4) 探针被超时卡死 (F:/ 出现 ASSERT.TXT 且 daplink_probe.py 0/6)" 1>&2
    echo "      ⇒ 拔掉全部线+USB, 等满 30 秒, 只插 USB 再试" 1>&2
fi
exit $RC
