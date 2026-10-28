#!/usr/bin/env bash
# GUI 单元测试（不需要管理员权限）。
#
# 为什么需要这个脚本：
#   上位机 exe 的 manifest 是 requireAdministrator（LHM 要读 MSR/SuperIO）。
#   `cargo test` 会把 src 里的 #[cfg(test)] 编成**同一个 bin 的 test harness**，
#   同样被嵌入了提权清单 ⇒ 非管理员 shell 执行时报
#       os error 740 请求的操作需要提升
#   build script 无法区分 build / test：
#     - 对一个 crate 只跑一次，两种命令共用同一次运行；
#     - Cargo 不提供 CARGO_CFG_TEST；
#     - `rustc-link-arg-bin=NAME=` 实测仍会作用到单元测试 harness。
#   ⇒ 这里用 build.rs 已有的逃生口 RA6E2_NO_UAC 临时关掉提权清单。
#
# 跑完会再用**默认配置**重建一次 release exe，保证 target/release 里的产物
# 仍然带提权（否则下次直接双击会读不到 CPU 温度/风扇）。
#
# 用法：
#   ./scripts/test-gui.sh              # 全部测试
#   ./scripts/test-gui.sh collector    # 只跑名字含 collector 的
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT/gui" || exit 1

export RUSTUP_HOME="${RUSTUP_HOME:-/e/.rustup}"
export CARGO_HOME="${CARGO_HOME:-/e/.cargo}"
case ":$PATH:" in
  *":/e/.cargo/bin:"*) ;;
  *) export PATH="/e/.cargo/bin:$PATH" ;;
esac

echo "=== cargo test（RA6E2_NO_UAC=1，测试进程不提权）==="
RA6E2_NO_UAC=1 cargo test "$@"
rc=$?

echo
echo "=== 恢复带提权清单的 release exe ==="
if cargo build --release; then
  echo "OK: target/release/ra6e2-monitor.exe 已按默认配置（requireAdministrator）重建"
else
  echo "⚠ 重建失败：exe 可能正被占用，请关闭正在运行的 ra6e2-monitor.exe 后"
  echo "  手动执行：cd gui && cargo build --release"
fi

exit $rc
