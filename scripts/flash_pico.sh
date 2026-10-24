#!/usr/bin/env bash
# ============================================================
# 用 Pico 跑的 debugprobe(CMSIS-DAP) 烧录 RA6E2
#
#   前提: Pico 已刷 debugprobe_on_pico.uf2, 接好
#         GP2 -> P300(SWCLK) / GP3 -> P108(SWDIO) / GND -> GND
#         RA6E2 板用 Type-C 自己供电(不要接 Pico 的 3V3)。
#   用法: ./scripts/flash_pico.sh [--verify] [--elf <path>] [--check-only]
#
# 与 flash.sh 的安全闸门完全一致: 默认拒绝烧含 option-setting 段的 elf。
# ⚠ 会擦除片上现有程序, 执行前确认板是 RA6E2。
# ============================================================
set -euo pipefail

export PATH="/c/Windows/System32:/c/Windows:$PATH"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PR="$(command -v probe-rs || true)"
[[ -z "$PR" ]] && PR="E:/.cargo/bin/probe-rs.exe"

OBJDUMP="$(command -v arm-none-eabi-objdump || true)"
[[ -z "$OBJDUMP" ]] && OBJDUMP="E:/software/ArmGNUToolchain/14.3 rel1/bin/arm-none-eabi-objdump.exe"

CHIP="R7FA6E2BB"
ELF="$ROOT/build/ra6e2_fw.elf"

VERIFY=0
CHECK_ONLY=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --verify)     VERIFY=1;     shift ;;
        --elf)        ELF="$2";     shift 2 ;;
        --check-only) CHECK_ONLY=1; shift ;;
        -h|--help)    sed -n '2,16p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "未知参数: $1 (用 --help 看用法)"; exit 1 ;;
    esac
done

if [[ ! -f "$ELF" ]]; then
    echo "找不到 $ELF, 请先编译。" 1>&2
    exit 1
fi

# ---- 安全闸门: option-setting 段 ----
OFS_SECS="$("$OBJDUMP" -h "$ELF" 2>/dev/null \
    | grep -oE '\.(option_setting|id_code)[a-z0-9_]*' | awk '!seen[$0]++' || true)"
SKIP=()
if [[ -n "$OFS_SECS" ]]; then
    echo "✘ 拒绝烧录: 该 elf 含 option-setting 段, 会锁死 SWD。日常请用 6.6.0 产物。" 1>&2
    exit 1
fi

# ---- 自动识别 Pico CMSIS-DAP ----
echo ">>> 寻找 Pico debugprobe ..."
PROBE=""
for cand in 2e8a:000c 2e8a:0004; do
    if "$PR" list 2>/dev/null | grep -qi "$cand"; then
        PROBE="$cand"
        break
    fi
done
if [[ -z "$PROBE" ]]; then
    echo "✘ 没找到 Pico debugprobe (2e8a:000c / 2e8a:0004)。" 1>&2
    echo "  确认: Pico 已刷 debugprobe_on_pico.uf2 并接好 SWD 三线 + 板子 Type-C 供电。" 1>&2
    exit 1
fi

EXTRA=()
[[ "$VERIFY" -eq 1 ]] && EXTRA+=(--verify)

echo ">>> 目标: $CHIP   探针: $PROBE"
echo ">>> 文件: $ELF"
echo ">>> OFS  : 无 option-setting 段 (6.6.0 产物, 安全)"

if [[ "$CHECK_ONLY" -eq 1 ]]; then
    echo ">>> --check-only: 检查通过, 不烧录。"
    exit 0
fi

"$PR" download --chip "$CHIP" --probe "$PROBE" "${EXTRA[@]}" "$ELF"
echo ">>> 烧录完成, 执行复位让新固件运行:"
"$PR" reset --chip "$CHIP" --probe "$PROBE" || true
