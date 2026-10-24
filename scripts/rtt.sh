#!/usr/bin/env bash
######################################################################
# 抓取目标板 RTT 日志（SEGGER RTT 兼容控制块，见 src/rtt/rtt_log.c）
#
# 用法:
#   ./scripts/rtt.sh                 抓 RTT 日志 (Ctrl-C 停止)
#   ./scripts/rtt.sh --file out.txt  同时把 channel 0 存到文件
#   ./scripts/rtt.sh --speed 4000    换速度 (默认 1000 kHz, 见下方说明)
#   ./scripts/rtt.sh --elf <path>    指定固件 (默认 build/ra6e2_fw.elf)
#   ./scripts/rtt.sh --fresh        先复位目标再挂, 程序从头跑 (看启动日志)
#   ./scripts/rtt.sh --allow-ofs    允许烧含 option-setting 段的 elf
#
# ⚠ 关于 "attach 会不会重新烧一遍" —— 实测 (probe-rs 0.29.1): 不一定,
#   取决于 flash 内容是否已一致。
#   `attach <ELF>` 名义上是 "flash and run the ELF, then attach to RTT logging",
#   且 **没有 --no-flash 开关** (已核对 --help)。但实际行为:
#
#   ① 默认: 先回读 flash 比对。
#      · 内容已一致  -> **既不擦写也不复位**, 直接挂上正在跑的程序。
#                       实测证据: 抓到的第一条是 frames=8070(没有启动 banner),
#                       且 g_oled_frames 复位前后连续增长 —— 说明程序一直在跑,
#                       这就是真正的 "运行时抓日志", 不打断程序。
#      · 内容不一致  -> 照常烧录 + 复位 + 从头跑(改完代码后就是这种, 日志带 banner)。
#
#   ② --fresh: 先 `probe-rs reset` 再挂 —— 程序从头跑, 日志含启动 banner,
#      但不擦写 flash。
#      ⚠ 原来以为 "--preverify 只跳过写入, 复位照旧" —— **实测是错的**:
#        flash 一致时 probe-rs 连 reset+run 一起跳过, 所以想看启动日志
#        必须显式 reset。加 --force-flash 也没用(它不是"强制重写"的开关)。
#
#   推论:
#     · 看启动流程               -> ./scripts/rtt.sh --fresh
#     · 看跑了一阵之后的状态     -> ./scripts/rtt.sh   (默认, 不打断程序)
#     · 改完代码                 -> ./scripts/rtt.sh   (内容变了会自动烧+复位)
#     · 默认模式长时间没输出     -> 芯片多半是 halt 的, 加 --fresh
#     · 完全不打断地看变量       -> `probe-rs read` 直读 RAM(见文档 §5.3)
#     · attach 自带烧录, 日常不必先跑 flash.sh —— 直接 rtt.sh 即可。
#
# 为什么默认降速到 1000 kHz:
#   用探针默认时钟时, RTT 控制块的块传输偶尔会错乱
#   (probe-rs 日志: "Expected 8 responses, got 15 responses"),
#   导致反复报 "Failed to attach - control block not found"。
#   降到 1 MHz 后实测稳定。2 MHz / 4 MHz 也可用。
#
# 安全闸门(与 flash.sh 一致):
#   默认拒绝烧录「含 option-setting 段」的 elf —— 那会碰 0x0100A100 区。
#   6.6.0 产物 (build/) 不含这些段, 可以直接烧。
#   5.8.0 产物 (build58/) 含 5 个, 必须 --allow-ofs, 脚本会自动 --skip-section。
#
# 注意: 它会一直跑, 用 Ctrl-C 结束。
######################################################################
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

CHIP="R7FA6E2BB"
PROBE="0d28:0204"
SPEED="1000"
ELF="$ROOT/build/ra6e2_fw.elf"
OUTFILE=""
FRESH=0
ALLOW_OFS=0

PROBE_RS="/e/.cargo/bin/probe-rs"

OBJDUMP="$(command -v arm-none-eabi-objdump || true)"
[[ -z "$OBJDUMP" ]] && OBJDUMP="E:/software/ArmGNUToolchain/14.3 rel1/bin/arm-none-eabi-objdump.exe"

while [ $# -gt 0 ]; do
    case "$1" in
        --speed)       SPEED="$2";      shift 2 ;;
        --elf)         ELF="$2";        shift 2 ;;
        --file)        OUTFILE="$2";    shift 2 ;;
        --chip)        CHIP="$2";       shift 2 ;;
        --probe)       PROBE="$2";      shift 2 ;;
        --fresh|--force-flash) FRESH=1;  shift ;;
        --allow-ofs)   ALLOW_OFS=1;     shift ;;
        -h|--help) sed -n '2,45p' "$0"; exit 0 ;;
        *) echo "未知参数: $1 (用 --help 看用法)"; exit 1 ;;
    esac
done

if [ ! -f "$ELF" ]; then
    echo "找不到固件: $ELF"
    exit 1
fi

# ---- 安全闸门: 检查 elf 是否带 option-setting 段 ----
# 注意: 不用 sort —— PATH 里 System32 在前, sort 会命中 Windows 的 sort.exe
OFS_SECS="$("$OBJDUMP" -h "$ELF" 2>/dev/null \
    | grep -oE '\.(option_setting|id_code)[a-z0-9_]*' | awk '!seen[$0]++' || true)"

SKIP=()
if [ -n "$OFS_SECS" ]; then
    if [ "$ALLOW_OFS" -eq 0 ]; then
        echo "✘ 拒绝烧录: 该 elf 含 option-setting 段, 烧它可能改写 0x0100A100 区并锁死 SWD。" 1>&2
        echo "  检测到的段: $(echo $OFS_SECS | tr '\n' ' ')" 1>&2
        echo "" 1>&2
        echo "  日常请用 6.6.0 产物:  $ROOT/build/ra6e2_fw.elf" 1>&2
        echo "  确实要烧这份, 加 --allow-ofs (脚本会自动 --skip-section 跳过这些段)。" 1>&2
        exit 1
    fi
    echo "⚠ 已 --allow-ofs: 将跳过以下段"
    while read -r sec; do
        [ -z "$sec" ] && continue
        echo "    $sec"
        SKIP+=(--skip-section "$sec")
    done <<< "$OFS_SECS"
fi

echo ">>> RTT 抓日志 | chip=$CHIP probe=$PROBE speed=${SPEED}kHz"
echo ">>> 固件: $ELF"
echo ">>> OFS  : $( [ -n "$OFS_SECS" ] && echo "有段, 已跳过 $((${#SKIP[@]} / 2)) 个" || echo "无 option-setting 段 (6.6.0 产物, 安全)")"
if [ "$FRESH" -eq 1 ]; then
    echo ">>> 模式: --fresh —— 先复位目标再挂, 程序从头跑, 日志含启动 banner"
else
    echo ">>> 模式: 热挂载 —— flash 内容一致则既不擦写也不复位, 直接挂上在跑的程序"
    echo ">>>       代码改过则自动烧录+复位从头跑; 长时间无输出就用 --fresh"
fi
echo ">>> 停止: Ctrl-C"
echo

# --fresh: 先复位, 让程序从头跑 (flash 一致时 attach 自己不会复位)
if [ "$FRESH" -eq 1 ]; then
    echo ">>> 复位目标 ..."
    "$PROBE_RS" reset --chip "$CHIP" --probe "$PROBE" --speed "$SPEED" \
        || echo "⚠ reset 失败, 继续尝试挂载"
fi

ARGS=(attach --chip "$CHIP" --probe "$PROBE" --speed "$SPEED" --preverify)

if [ ${#SKIP[@]} -gt 0 ]; then
    ARGS+=("${SKIP[@]}")
fi

if [ -n "$OUTFILE" ]; then
    # channel 0 同时写入文件
    ARGS+=(--target-output-file "$OUTFILE")
fi

ARGS+=("$ELF")

exec "$PROBE_RS" "${ARGS[@]}"
