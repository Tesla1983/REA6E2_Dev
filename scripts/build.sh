#!/usr/bin/env bash
# ============================================================
# RA6E2 固件构建脚本 (Git Bash)
#
#   ./scripts/build.sh            # 增量构建
#   ./scripts/build.sh --clean    # 清空后重新配置 + 全量构建
#
# 注意: 必须把 C:\Windows\System32 加入 PATH。
#       Git Bash 默认 PATH 里没有它, 会导致 CMake 生成的
#       嵌套 `cmd.exe /C` 报 "'cmd.exe' 不是内部或外部命令"。
# ============================================================
set -euo pipefail

export PATH="/c/Windows/System32:/c/Windows:$PATH"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CMAKE="/c/Program Files/CMake/bin/cmake.exe"

if [[ ! -x "$CMAKE" ]]; then
    CMAKE="$(command -v cmake)"
fi

BUILDDIR="$ROOT/build"

if [[ "${1:-}" == "--clean" ]]; then
    echo ">>> 清理 $BUILDDIR"
    rm -rf "$BUILDDIR"
fi

if [[ ! -f "$BUILDDIR/build.ninja" ]]; then
    echo ">>> 配置 (Ninja + arm-none-eabi)"
    "$CMAKE" -G Ninja \
        -S "$ROOT" \
        -B "$BUILDDIR" \
        -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/arm-none-eabi-ra6e2.cmake"
fi

echo ">>> 构建"
"$CMAKE" --build "$BUILDDIR"

echo ">>> 完成"
ls -l "$BUILDDIR"/*.elf "$BUILDDIR"/*.hex "$BUILDDIR"/*.srec 2>/dev/null || true
