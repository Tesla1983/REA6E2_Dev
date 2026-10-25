#!/usr/bin/env bash
# ============================================================
# 抓取固件里的「USB 类请求环形日志」, 用于定位"COM 口晚 ~30s 才可用"卡在哪个请求。
#
# 用法:
#   ./scripts/dump_req_log.sh              # 轮询等设备出现, 再等 45s, 再读日志解析
#   ./scripts/dump_req_log.sh --now        # 不等待, 立刻读(设备已在位时用)
#
# 依赖: probe-rs, arm-none-eabi-nm, python(系统 3.12 有 pyserial, 这里只用标准库)
# 注意: 符号地址随重编译会变, 脚本每次都从 elf 现取(nm), 不会读到旧地址。
# ============================================================
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ELF="$ROOT/build/ra6e2_fw.elf"
PR="$(command -v probe-rs || true)"
[[ -z "$PR" ]] && PR="E:/.cargo/bin/probe-rs.exe"
NM="$(command -v arm-none-eabi-nm || true)"
[[ -z "$NM" ]] && NM="E:/software/ArmGNUToolchain/14.3 rel1/bin/arm-none-eabi-nm.exe"
CHIP="R7FA6E2BB"
PROBE="${RA_PROBE:-0416:5021}"
SPEED="${RA_SPEED:-1000}"
PS="/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe"

MODE="wait"
[[ "${1:-}" == "--now" ]] && MODE="now"

# ---- 从 elf 现取符号地址 ----
addr_of() {
    "$NM" "$ELF" 2>/dev/null | awk -v s="$1" '$3==s {print $1; exit}'
}

A_HEAD=$(addr_of g_usb_req_log_head)
A_BMRT=$(addr_of g_usb_req_log_bmrt)
A_BREQ=$(addr_of g_usb_req_log_breq)
A_VAL=$(addr_of g_usb_req_log_value)
A_IDX=$(addr_of g_usb_req_log_index)
A_LEN=$(addr_of g_usb_req_log_length)
A_DATA=$(addr_of g_usb_req_data_cnt)
A_NODA=$(addr_of g_usb_req_nodata_cnt)
A_CNT=$(addr_of g_usb_req_cnt)

for v in A_HEAD A_BMRT A_BREQ A_VAL A_IDX A_LEN A_DATA A_NODA A_CNT; do
    if [[ -z "${!v}" ]]; then
        echo "✘ 在 elf 里找不到符号($v 对应的符号)。先重新编译(该日志是新加的)。" 1>&2
        exit 1
    fi
done
echo "符号地址: head=0x$A_HEAD bmrt=0x$A_BMRT breq=0x$A_BREQ val=0x$A_VAL idx=0x$A_IDX len=0x$A_LEN"

read_u32() {  # $1=addr -> 打印十进制值(失败打印 -1)
    local a="$1" out
    for t in 1 2 3; do
        out=$("$PR" read b8 "0x$a" 4 --chip "$CHIP" --probe "$PROBE" --speed "$SPEED" 2>&1)
        if echo "$out" | grep -q "Probe not found\|could not be opened"; then sleep 2; continue; fi
        python -c "
import re,sys
t=re.findall(r'\b[0-9a-fA-F]{1,2}\b', '''$out''')
b=[int(x,16) for x in t]
print(b[0]|(b[1]<<8)|(b[2]<<16)|(b[3]<<24) if len(b)>=4 else -1)
"
        return 0
    done
    echo "-1"
}

if [[ "$MODE" == "wait" ]]; then
    echo ">>> 轮询等待固件跑起来(请现在拔插 Type-C; 通过读 g_usb_req_cnt 判断) ..."
    for i in $(seq 1 14); do
        c=$(read_u32 "$A_CNT")
        if [[ "$c" != "-1" && "$c" != "0" ]]; then
            echo ">>> 固件已在跑(req_cnt=$c)。再等 40 秒, 让 ~30s 阶段走完 ..."
            break
        fi
        echo "  (第 $i 次: req_cnt=$c, 3 秒后重试)"
        sleep 3
    done
    sleep 40
    c2=$(read_u32 "$A_CNT")
    echo ">>> 45 秒后 req_cnt=$c2"
else
    echo ">>> --now: 立刻读取"
fi

readmem() {  # $1=addr $2=count
    local a="$1" n="$2" out
    for t in 1 2 3; do
        out=$("$PR" read b8 "0x$a" "$n" --chip "$CHIP" --probe "$PROBE" --speed "$SPEED" 2>&1)
        if echo "$out" | grep -q "Probe not found\|could not be opened"; then
            echo "  (探针打开失败, 重试 $t)" 1>&2; sleep 2; continue
        fi
        echo "$out"; return 0
    done
    echo "READFAIL"; return 1
}

echo ">>> 读取中 ..."
R_HEAD=$(readmem "$A_HEAD" 4)
R_BMRT=$(readmem "$A_BMRT" 16)
R_BREQ=$(readmem "$A_BREQ" 16)
R_VAL=$(readmem "$A_VAL" 32)
R_IDX=$(readmem "$A_IDX" 32)
R_LEN=$(readmem "$A_LEN" 32)
R_DATA=$(readmem "$A_DATA" 4)
R_NODA=$(readmem "$A_NODA" 4)
R_CNT=$(readmem "$A_CNT" 4)

python - "$R_HEAD" "$R_BMRT" "$R_BREQ" "$R_VAL" "$R_IDX" "$R_LEN" "$R_DATA" "$R_NODA" "$R_CNT" <<'PYEOF'
import sys, re

def parse(hextext):
    """probe-rs read b8 输出的是十六进制文本, 必须按 16 进制解析(不要当十进制!)"""
    if hextext.strip() == "READFAIL":
        return None
    toks = re.findall(r'\b[0-9a-fA-F]{1,2}\b', hextext)
    # 去掉地址前缀等: 每行形如 "2000430c: 01 00 00 00" 或直接 "01 00 00 00"
    return [int(t, 16) for t in toks]

def u32(b):
    if not b or len(b) < 4: return None
    return b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24)

(head, bmrt, breq, val, idx, ln, data, noda, cnt) = sys.argv[1:10]
H = parse(head); B = parse(bmrt); R = parse(breq)
V = parse(val); I = parse(idx); L = parse(ln)
D = parse(data); N = parse(noda); C = parse(cnt)

print()
print("=============== USB 类请求日志 ===============")
print(f"请求总数 g_usb_req_cnt     : {u32(C)}")
print(f"走了数据阶段 g_usb_req_data_cnt : {u32(D)}")
print(f"按无数据收尾 g_usb_req_nodata_cnt: {u32(N)}")
h = u32(H) or 0
print(f"环形写入位置 head          : {h}")
print()
print("最近 16 条请求(按时间顺序, 最后一条=最新):")
print(f"{'#':>3}  {'bmRT':>6} {'bReq':>6} {'wValue':>7} {'wIndex':>7} {'wLength':>8}")
names = {
    0x20: 'SET_LINE_CODING', 0x21: 'GET_LINE_CODING', 0x22: 'SET_CONTROL_LINE_STATE',
    0x00: 'SEND_ENCAPSULATED_COMMAND', 0x01: 'GET_ENCAPSULATED_RESPONSE',
    0x23: 'SEND_BREAK',
}
n = 16
for k in range(n):
    i = (h + k) % n   # head 指向下一条要写的位置 => 最旧的一条
    def gv(arr, i, width):
        if not arr: return None
        base = i * width
        if base + width > len(arr): return None
        if width == 1: return arr[base]
        return arr[base] | (arr[base+1] << 8)
    b_ = gv(B, i, 1); r_ = gv(R, i, 1); v_ = gv(V, i, 2); x_ = gv(I, i, 2); l_ = gv(L, i, 2)
    if b_ is None or r_ is None: continue
    seq = gv([0], 0, 1)
    print(f"{k:>3}  0x{b_:02x}    0x{r_:02x}   {v_ if v_ is not None else '-':>7} {x_ if x_ is not None else '-':>7} {l_ if l_ is not None else '-':>8}   {names.get(r_, '')}")
print()
print("!!! 重点看: 有没有 wLength>0 的请求(尤其 0x01 GET_ENCAPSULATED_RESPONSE / 0x00 SEND_ENCAPSULATED_COMMAND)。")
print("!!! 如果 data_cnt 从 0 变成非 0, 说明确有带数据阶段的请求此前被误当'无数据'收尾 —— 那就是 30s 元凶。")
PYEOF
