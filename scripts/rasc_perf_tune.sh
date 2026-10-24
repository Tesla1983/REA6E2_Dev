#!/usr/bin/env bash
# RASC (Eclipse RCP) 启动参数调优 —— 带备份与一键回滚
#
# 用法:
#   bash scripts/rasc_perf_tune.sh status    # 看当前 rasc.ini 与是否已调优
#   bash scripts/rasc_perf_tune.sh apply     # 应用调优(自动备份原文件)
#   bash scripts/rasc_perf_tune.sh restore   # 从备份还原
#
# 说明见 docs/RASC开发文档.md §15。

set -euo pipefail

INI="E:/software/Renesas/RAC/eclipse/rasc.ini"
BAK="E:/software/Renesas/RAC/eclipse/rasc.ini.orig"

die() { echo "错误: $*" >&2; exit 1; }

[[ -f "$INI" ]] || die "找不到 $INI"

do_status() {
    echo "=== 当前 $INI ==="
    cat "$INI"
    echo
    if [[ -f "$BAK" ]]; then
        echo "备份存在: $BAK  (已应用过调优, 可 restore)"
    else
        echo "备份不存在: 尚未应用调优"
    fi
}

do_apply() {
    [[ -f "$BAK" ]] || cp -p "$INI" "$BAK"
    echo "已备份 → $BAK"

    # 只处理 -vmargs 之后的 JVM 参数区
    python - "$INI" << 'PYEOF'
import sys, io, re
p = sys.argv[1]
s = io.open(p, encoding='utf-8').read()
lines = s.splitlines()

# 分离 launcher 段与 vmargs 段
if '-vmargs' in lines:
    i = lines.index('-vmargs')
    head, vm = lines[:i+1], lines[i+1:]
else:
    head, vm = lines, []

DROP = {'-XX:+UseStringDeduplication'}          # GUI 工具收益低, 后台线程常驻开销
SET  = {
    '-Xms': '1024m',                            # 原 512m: 减少运行期反复扩容触发 GC
    '-Xmx': '1536m',                            # 原 3g  : 单工程用不到, 降低常驻内存与退出回收成本
}
ADD  = ['-Declipse.p2.mirrors=false']           # 禁止 p2 联网查镜像(避免启动/更新卡顿)

out = []
seen = set()
for l in vm:
    k = l.strip()
    if k in DROP:
        continue
    for pref, val in SET.items():
        if k.startswith(pref):
            k = pref + val
            seen.add(pref)
            break
    if k.startswith(tuple(ADD)):
        continue
    out.append(k)

for pref, val in SET.items():
    if pref not in seen:
        out.append(pref + val)
for a in ADD:
    out.append(a)

io.open(p, 'w', encoding='utf-8', newline='\n').write('\n'.join(head + out) + '\n')
print("已写入:", p)
PYEOF
    echo
    echo "=== 调优后的 vmargs 段 ==="
    sed -n '/-vmargs/,$p' "$INI"
}

do_restore() {
    [[ -f "$BAK" ]] || die "没有备份, 无需还原"
    cp -p "$BAK" "$INI"
    echo "已从 $BAK 还原"
}

case "${1:-status}" in
    status)  do_status ;;
    apply)   do_apply ;;
    restore) do_restore ;;
    *) echo "用法: $0 {status|apply|restore}" ; exit 1 ;;
esac
