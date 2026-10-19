#!/usr/bin/env bash
# replay_run.sh — 阶段 4.5 B 线：运行态回放全自动编排（透明插桩 · 服务器上一条命令跑完）
# 2026-10-19修订：原方案设计非已验证成果，见CURRENT_STATUS.md。
# anon/nonce CPU/DSA预取窗外，checksum对本地源抽样，不保证guest静止或跨臂同内容。
# GET是否消费/清位需确认目标配置；只GET不CLEAR不保证零扰动，先用专用测试实例。
#
# 目的：不改 AgentENV 源码，在运行态（aenv pause 之前）对同一批 dirty 页做三路回放
#       （pvm 复刻现状 / mmap 直映 / dsa 硬件搬运），量化「AgentENV 现状 CPU 成本
#       中有多少可被 DSA 卸载」，为鲲鹏等价功能决策提供 B 臂证据。
#
# 编排（全自动，无人工步骤）：
#   preflight → 编译（DML 版失败自动降级双臂）→ 起沙箱（同沙箱配对，抵消跨沙箱方差）
#   → 找 firecracker pid → NUMA 绑定（guest RAM 所在 node）→ 环境留痕
#   → 主循环 [dirty 档 × round]：
#       resume → 写魔数 + dd 制脏 → probe（B0/B1，dump 页集合）→ 按 ABBA 序逐臂回放
#       （运行态读 firecracker 的 COW 页；aenv pause 会把 guest RAM flush+unmap，读源消失）
#       → pause（留 pause wall）→ resume → rounds 行留痕
#   → 统计（round 级配对 + 95% CI）→ 汇总打印
#
# 纪律：
#   - 不改 AgentENV 源码；KVM 侧只 GET_DIRTY_LOG、绝不 CLEAR（保护增量快照链）。
#   - checksum 不一致（replay_offload exit 2）立即中止：数据完整性优先。
#   - DML 臂失败（exit 3）自动降级：该轮剔除 dsa，其余臂照常。
#   - ABBA 交错：每 ABBA_BLOCK 轮一块，块间整序反转 + 块内 Fisher-Yates 确定性洗牌
#     （种子=块号）—— 同轮三臂严格同页集合，顺序效应在臂间均衡，且可复现审计。
#
# 用法（AgentENV 服务器上，root）：
#   TEMPLATE=<模板名> sudo -E bash replay_run.sh              # 全量：64/256/1024 MB × 30 轮
#   TEMPLATE=<模板名> SMOKE=1 sudo -E bash replay_run.sh      # 冒烟：64 MB × 2 轮 × 2 reps
#   CLEANUP=1 ...                                             # 跑完删沙箱
#   FIRECRACKER_PID=<pid> ...                                 # 多沙箱机器上显式指定
#
# 可调参数（环境变量）：DIRTY_MB_LIST / REPEAT / ARM_REPEAT / ABBA_BLOCK / FLUSH /
#   SUBSET_MB / PVM_CHUNK_KB / BATCH_N / DSA_WAIT / COLD / CPU_COUNT / MEM_MIB /
#   TEMPLATE(必填) / CLEANUP / SMOKE / FIRECRACKER_PID
#
# 产出（results/stage4-sandbox/，重跑覆盖）：
#   replay_detail.csv   逐 rep 明细（17 列，含 ts/sbx/dirty/round 前缀）
#   replay_summary.csv  臂级统计：dirty_mb × arm 的 mean/sd/95%CI（round 均值后聚合）
#   replay_paired.csv   配对统计：非 pvm 臂 vs pvm 逐轮配对差值的 95%CI（核心结论表）
#   replay_rounds.csv   轮级留痕：pause wall / dirty 来源 / 页数 / ABBA 臂序
#   replay_env.txt      环境留痕（内核/governor/主机busy%/全部参数）
#   replay_err.log      replay_offload 的 stderr（诊断）
#   replay_pages/       每轮页集合 dump（d{MB}_r{ROUND}.txt，可复现）
set -euo pipefail
cd "$(dirname "$0")"

# ---------------- 可调参数 ----------------
TEMPLATE="${TEMPLATE:?须设置环境变量 TEMPLATE=<aenv 模板名>}"
COLD="${COLD:-1}"                             # 1 = aenv start --cold（可配 vCPU/内存）
CPU_COUNT="${CPU_COUNT:-4}"
MEM_MIB="${MEM_MIB:-3584}"                    # 3584MiB = 3.5GiB：guest RAM 全在 MMIO hole
                                              # 之下 → 单 memslot，免 slot1 顺序布局假设
DIRTY_MB_LIST="${DIRTY_MB_LIST:-64 256 1024}"
REPEAT="${REPEAT:-30}"                        # 轮数（配对样本 n）
ARM_REPEAT="${ARM_REPEAT:-5}"                 # 每轮每臂内部 repeat 次数
ABBA_BLOCK="${ABBA_BLOCK:-5}"                 # 每 5 轮换一块（反转+洗牌）
FLUSH="${FLUSH:-1}"                           # 1 = 每次 repeat 前 256MB LLC 冲刷（冷页口径）
SUBSET_MB="${SUBSET_MB:-256}"                 # bitmap 超出 / whole 退化时的段式采样量
PVM_CHUNK_KB="${PVM_CHUNK_KB:-64}"            # pvm 臂 iovec 合并粒度（假设值，待 A1 校准）
BATCH_N="${BATCH_N:-32}"                      # DML batch 描述符数；上限=shared wq max_batch_size=32
                                             # （2026-10-16 实测：48 在本机报 DML_STATUS_BATCH_SIZE_ERROR=29）
DSA_WAIT="${DSA_WAIT:-idle}"                  # busy | idle
                                             # onCPU 卸载口径必须用 idle(=DML UMWAIT)：
                                             # busy(=BUSY_POLL) 会自旋烧 CPU，dsa 臂 cpu_ms 虚高，
                                             # 反映不出「释放了多少 CPU」。要看功耗换延迟口径才用 busy。
CLEANUP="${CLEANUP:-0}"
FIRECRACKER_PID="${FIRECRACKER_PID:-}"        # 空 = 自动发现（pgrep 命令行含沙箱 uuid）

if [ "${SMOKE:-0}" = "1" ]; then              # 冒烟：验链路，不用于出数
    REPEAT=2; ARM_REPEAT=2; DIRTY_MB_LIST="64"
fi

# ---------------- 产出路径 ----------------
OUT_DIR="results/stage4-sandbox"
PAGES_DIR="$OUT_DIR/replay_pages"
DETAIL="$OUT_DIR/replay_detail.csv"
ROUNDS="$OUT_DIR/replay_rounds.csv"
SUMM="$OUT_DIR/replay_summary.csv"
PAIRED="$OUT_DIR/replay_paired.csv"
ENVF="$OUT_DIR/replay_env.txt"
ERRLOG="$OUT_DIR/replay_err.log"
mkdir -p "$PAGES_DIR"
: > "$ERRLOG"
echo "ts,sbx,dirty_mb,round,arm,src,pages,mb,chunk_kb,flush,irep,cpu_ms,wall_ms,cpu_ms_per_gb,cpu_util,cksum,note" > "$DETAIL"
echo "dirty_mb,round,pause_wall_ms,dirty_src,n_pages,arm_order,resume_wall_ms,resume_srv_cpu_ms" > "$ROUNDS"

# ---------------- 函数 ----------------
fatal()   { echo "FATAL: $*" >&2; exit 1; }

PAUSED=0
SBX=""
cleanup() {
    trap - EXIT INT TERM
    if [ "${PAUSED:-0}" -eq 1 ] && [ -n "${SBX:-}" ]; then
        echo "cleanup: resume 沙箱 $SBX" >&2
        aenv resume "$SBX" >/dev/null 2>&1 || true
    fi
    if [ "${CLEANUP:-0}" = "1" ] && [ -n "${SBX:-}" ]; then
        echo "cleanup: 删除沙箱 $SBX" >&2
        aenv delete "$SBX" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT
trap 'cleanup; exit 130' INT TERM

# ABBA 交错：块 = floor((round-1)/ABBA_BLOCK)；奇数块整序反转，块内 Fisher-Yates
# 确定性洗牌（RANDOM 种子 = 块号）→ 同一轮内臂序可复现，轮间顺序效应均衡
arm_order() {
    local round="$1" i j t
    local block=$(( (round - 1) / ABBA_BLOCK ))
    local arms=("${BASE_ARMS[@]}")
    if [ $((block % 2)) -eq 1 ]; then
        local n=${#arms[@]} rev=()
        for ((i = n - 1; i >= 0; i--)); do rev+=("${arms[i]}"); done
        arms=("${rev[@]}")
    fi
    RANDOM=$(( (block + 1) * 2654435761 % 2147483647 ))
    local n=${#arms[@]}
    for ((i = n - 1; i > 0; i--)); do
        j=$(( RANDOM % (i + 1) ))
        t="${arms[i]}"; arms[i]="${arms[j]}"; arms[j]="$t"
    done
    echo "${arms[*]}"
}

# 主机 busy%（/proc/stat 1s 差分，跨所有 CPU）——留痕用，排除「跑时机器忙」的质疑
stat_idle_total() {
    awk 'NR==1 { idle = $5 + $6; t = 0; for (i = 2; i <= NF; i++) t += $i
                print idle, t; exit }' /proc/stat
}
sample_busy_pct() {
    local i1 t1 i2 t2
    read -r i1 t1 <<<"$(stat_idle_total)"
    sleep 1
    read -r i2 t2 <<<"$(stat_idle_total)"
    awk -v a="$i1" -v b="$t1" -v c="$i2" -v d="$t2" \
        'BEGIN { dt = d - b; if (dt <= 0) { print "n/a"; exit }
                 printf "%.1f", 100 * (dt - (c - a)) / dt }'
}

# server 进程累计 CPU（utime+stime → ms；进程不存在输出空串，不阻塞主流程）
srv_cpu_ms() {
    [ -n "${SERVER_PID:-}" ] || { echo ""; return; }
    local stat
    stat="$(cat "/proc/$SERVER_PID/stat" 2>/dev/null || true)"
    [ -n "$stat" ] || { echo ""; return; }
    [ -n "${CLK_TCK:-}" ] || CLK_TCK=100
    awk -v clk="$CLK_TCK" '{
        rest = $0; sub(/^.*\)/, "", rest)      # 跳过 pid (comm)，comm 可含空格/括号
        split(rest, f, " ")                     # f[12]=utime f[13]=stime（原 stat 字段 14/15）
        printf "%d", (f[12] + f[13]) * 1000 / clk
    }' <<<"$stat"
}

# 找 firecracker：显式指定 > 命令行含沙箱 uuid > 全机唯一。
# 实测（2025-10 B 线 SMOKE 定位）：AgentENV patch 版 fc 命令行不含沙箱 uuid
# （只有 --api-sock 实例随机名 agentenv-fc-XXXXXX），共享服务器多 fc 并存时
# uuid 匹配与「全机唯一」双双失效 → 本函数失败。此时由 nonce 内容定位兜底
# （replay_offload --nonce-hex：每轮制脏时 guest 内写 16B 随机数，probe/臂
# 在全部 fc 的可读大段内内容搜索——命中进程 = 本沙箱 fc 真身，命中段所在
# 同文件连续段组 = guest RAM；实测 patch 版后端是 mmap /dev/ublkbN 拆两段
# VMA，replay_offload 的匿名大小匹配同样失效，nonce 是唯一可靠定位手段）。
find_fc() {
    if [ -n "$FIRECRACKER_PID" ]; then
        kill -0 "$FIRECRACKER_PID" 2>/dev/null || return 1
        echo "$FIRECRACKER_PID"; return 0
    fi
    local p
    p="$(pgrep -af firecracker 2>/dev/null | grep -F "$SBX" \
         | awk '{print $1}' | head -1 || true)"
    if [ -n "$p" ]; then echo "$p"; return 0; fi
    local n
    n="$(pgrep -x firecracker 2>/dev/null | wc -l || true)"
    if [ "${n//[[:space:]]/}" = "1" ]; then pgrep -x firecracker | head -1; return 0; fi
    return 1
}

# 进程驻留内存最多的 NUMA 节点（numa_maps 汇总）
detect_numa_node() {
    [ -r "/proc/$1/numa_maps" ] || return 0
    awk '
        { for (i = 1; i <= NF; i++) if ($i ~ /^N[0-9]+=/) {
              split($i, a, "="); tot[a[1]] += a[2] } }
        END { best = -1; bestv = -1
              for (k in tot) { v = substr(k, 2) + 0
                               if (tot[k] + 0 > bestv) { bestv = tot[k] + 0; best = v } }
              if (best >= 0 && bestv > 0) print best }' "/proc/$1/numa_maps" 2>/dev/null || true
}

# 单臂回放：rc=2 checksum → 中止；rc=3 DML → 降级剔除；其余非 0 → 中止
run_arm() {
    local arm="$1" mb="$2" round="$3" src="$4"
    local out rc
    set +e
    # --fc-pid 仅定位提示（可空：多 fc 并存 find_fc 失败时由 nonce 内容定位；
    # 两者皆空时 C 侧 usage 报错——双定位手段全失效即明确失败，不静默乱跑）
    out=$("${NUMACTL[@]+"${NUMACTL[@]}"}" ./replay_offload \
              ${FCPID:+--fc-pid "$FCPID"} ${NONCE_HEX:+--nonce-hex "$NONCE_HEX"} \
              --guest-mib "$MEM_MIB" --arm "$arm" --pages "$PF" \
              --repeat "$ARM_REPEAT" --cold "$FLUSH" \
              --pvm-chunk-kb "$PVM_CHUNK_KB" --batch-n "$BATCH_N" \
              --dsa-wait "$DSA_WAIT" --src-tag "d${mb}_${src}" 2>>"$ERRLOG")
    rc=$?
    set -e
    if [ "$rc" -eq 2 ]; then
        fatal "checksum 不一致（arm=$arm dirty=${mb}MB round=$round）—— 数据完整性优先，中止（详见 $ERRLOG）"
    elif [ "$rc" -eq 3 ]; then
        echo "    WARN: dsa 臂失败 rc=3（round=$round，详见 $ERRLOG）→ 本轮剔除 dsa 继续"
        DSA_FAILS=$((DSA_FAILS + 1))
        return 0
    elif [ "$rc" -ne 0 ]; then
        fatal "replay_offload arm=$arm 失败 rc=$rc（round=$round，详见 $ERRLOG）"
    fi
    # 前缀 ts,sbx,dirty_mb,round 后追加（13 列 → 17 列）
    printf '%s\n' "$out" | awk -v ts="$(date +%F_%T)" -v sbx="$SBX" -v d="$mb" -v r="$round" \
        'BEGIN { OFS = "," } NF > 0 { print ts, sbx, d, r, $0 }' >> "$DETAIL"
    awk -F, -v a="$arm" '{ n++; s += $10 } END { if (n > 0)
        printf "    %-4s: %d reps, mean cpu %.1f ms/GB\n", a, n, s/n }' <<<"$out"
}

# ---------------- preflight ----------------
echo "== preflight =="
[ "$(id -u)" -eq 0 ] || fatal "须 root（pidfd_getfd 偷 firecracker 的 fd）"
command -v aenv >/dev/null 2>&1 || fatal "aenv CLI 不在 PATH"
command -v gcc  >/dev/null 2>&1 || fatal "gcc 不可用"
[ -f replay_offload.c ]        || fatal "缺 replay_offload.c（应与 本脚本 同目录）"
KMAJ="$(uname -r | cut -d. -f1)"
KMIN="$(uname -r | cut -d. -f2)"
if [ "$KMAJ" -lt 5 ] || { [ "$KMAJ" -eq 5 ] && [ "$KMIN" -lt 6 ]; }; then
    echo "WARN: 内核 $(uname -r) < 5.6：pidfd_getfd 不可用，B0 probe 预计 NOGO（不中断，交给 probe 判定）"
fi
ND=0; for _ in $DIRTY_MB_LIST; do ND=$((ND + 1)); done
[ "$ND" -gt 0 ] || fatal "DIRTY_MB_LIST 为空"

# ---------------- 编译（DML 版优先，失败自动降级双臂） ----------------
echo "== 编译 replay_offload =="
DML_INC="/home/m00954035/.local/include"
DML_LIB="/home/m00954035/.local/lib"
if gcc -O2 -Wall -DUSE_DML -I"$DML_INC" replay_offload.c -o replay_offload \
        -L"$DML_LIB" -ldml -ldmlhl -luuid -pthread -lstdc++ 2>>"$ERRLOG"; then
    export LD_LIBRARY_PATH="$DML_LIB${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    # DML 运行时自检：编译通过 ≠ 硬件路径可用——DML 只认 shared 模式 wq
    # （2026-10-16 SMOKE 实测：服务器 wq 全 dedicated → 臂 rc=3 全灭，编译期却毫无征兆）
    DML_TEST="$(mktemp /tmp/dml_selftest.XXXXXX)"
    cat > "$DML_TEST.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <dml/dml.h>
int main(void) {
    uint32_t sz = 0;
    if (dml_get_job_size(DML_PATH_HW, &sz) != DML_STATUS_OK || sz == 0) {
        fprintf(stderr, "dml_get_job_size 失败：DSA 设备不可用\n");
        return 1;
    }
    dml_job_t *job = malloc(sz);
    if (!job) { fprintf(stderr, "malloc 失败\n"); return 1; }
    if (dml_init_job(DML_PATH_HW, job) != DML_STATUS_OK) {
        fprintf(stderr, "dml_init_job 失败：DML 硬件路径要求 shared 模式 wq（accel-config list 查各 wq 的 mode）\n");
        free(job);
        return 1;
    }
    dml_finalize_job(job);
    free(job);
    return 0;
}
EOF
    if gcc -O2 -I"$DML_INC" "$DML_TEST.c" -o "$DML_TEST" -L"$DML_LIB" \
            -ldml -ldmlhl -luuid -pthread -lstdc++ 2>>"$ERRLOG" \
            && "$DML_TEST" >/dev/null 2>>"$ERRLOG"; then
        BASE_ARMS=(pvm mmap dsa)
        echo "  DML 硬件版 OK（编译 + 运行时自检通过）→ 三臂 pvm / mmap / dsa（LD_LIBRARY_PATH 含 $DML_LIB）"
    else
        BASE_ARMS=(pvm mmap)
        echo "  WARN: DML 编译 OK 但运行时自检失败（典型原因：无 shared 模式 wq，详见 $ERRLOG 尾部）→ 降级双臂 pvm / mmap"
        tail -n 3 "$ERRLOG" || true
    fi
    rm -f "$DML_TEST" "$DML_TEST.c"
else
    echo "  WARN: DML 版编译失败（$ERRLOG 尾部）→ 降级双臂 pvm / mmap"
    tail -n 3 "$ERRLOG" || true
    gcc -O2 -Wall replay_offload.c -o replay_offload -pthread 2>>"$ERRLOG" \
        || fatal "无 DML 基础版也编译失败"
    BASE_ARMS=(pvm mmap)
fi
DSA_FAILS=0

# ---------------- 沙箱（整轮复用一个：同沙箱配对，抵消跨沙箱 40% 方差教训） ----------------
echo "== 启动沙箱 =="
start_args=(-d)
if [ "$COLD" = "1" ]; then
    start_args+=(--cold --cpu "$CPU_COUNT" --memory "$MEM_MIB")
fi
SBX="$(aenv start "${start_args[@]}" "$TEMPLATE" 2>/dev/null \
       | grep -oE '[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}' | head -1 || true)"
[ -n "$SBX" ] || fatal "aenv start 失败（检查 TEMPLATE=$TEMPLATE / aenv auth）"
aenv timeout "$SBX" 43200                    # 默认 TTL 300s，必须拉长
echo "  sandbox = $SBX（cold=$COLD cpu=$CPU_COUNT mem=${MEM_MIB}MiB）"

echo -n "  等 guest ready "
for i in $(seq 1 90); do
    if aenv exec "$SBX" -- true >/dev/null 2>&1; then echo "OK"; break; fi
    echo -n "."
    sleep 2
    if [ "$i" -eq 90 ]; then fatal "guest 180s 未 ready"; fi
done

# ---------------- firecracker / NUMA / 环境留痕 ----------------
if FCPID="$(find_fc)"; then
    echo "  firecracker = pid $FCPID（find_fc；probe 的 nonce 仍会内容级校验/修正）"
else
    FCPID=""
    echo "  WARN: find_fc 定位失败（多 fc 并存且命令行无 uuid）→ FCPID 留空"
    echo "        由 probe 的 nonce 内容定位（每轮制脏时 guest 写 /dev/shm/.b0nonce，见主循环）"
fi

SERVER_PID="$(pgrep -x server | head -1 || true)"
CLK_TCK="$(getconf CLK_TCK 2>/dev/null || echo 100)"
NUMA_NODE=""
NUMA_SRC=""
NUMA_NODE="$(detect_numa_node "$FCPID" || true)"; NUMA_SRC="firecracker"
if [ -z "$NUMA_NODE" ] && [ -n "$SERVER_PID" ]; then
    NUMA_NODE="$(detect_numa_node "$SERVER_PID" || true)"; NUMA_SRC="server"
fi
NUMACTL=()
if [ -n "$NUMA_NODE" ]; then
    if command -v numactl >/dev/null 2>&1; then
        NUMACTL=(numactl "--cpunodebind=$NUMA_NODE" "--membind=$NUMA_NODE")
        echo "  NUMA: 回放进程绑 node $NUMA_NODE（来源 $NUMA_SRC，guest RAM 所在 node，防跨 node 失真）"
    else
        echo "  WARN: 检测到 node $NUMA_NODE 但 numactl 不可用，跳过绑定"
    fi
else
    echo "  NUMA: 无信息（单 node 机器或无 numa_maps），跳过绑定"
fi

BUSY_PCT="$(sample_busy_pct)"
{
    echo "# B 线回放环境留痕（自动生成）"
    echo "date             = $(date -Is)"
    echo "host             = $(hostname) / $(uname -r) / $(uname -m)"
    govs="$(cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null | sort -u | paste -sd/ - || true)"
    echo "governor         = ${govs:-n/a}"
    echo "host_busy_pct    = ${BUSY_PCT}（跑前 1s 采样）"
    echo "sandbox          = $SBX"
    echo "template         = $TEMPLATE（cold=$COLD cpu=$CPU_COUNT mem=${MEM_MIB}MiB）"
    echo "firecracker_pid  = ${FCPID:-n/a（find_fc 失败：多 fc 并存且命令行无 uuid，由 probe 的 nonce 内容定位真身）}"
    echo "server_pid       = ${SERVER_PID:-none}"
    echo "resume_probe     = wall + server utime+stime 差分 ms（窗口 = aenv resume 返回点，保守下界；server 缺失记 n/a）"
    if [ -n "$NUMA_NODE" ]; then
        echo "numa_bind        = node $NUMA_NODE（来源 $NUMA_SRC）"
    else
        echo "numa_bind        = none"
    fi
    echo "ram_access       = nonce 内容定位优先（guest 每轮制脏前写 /dev/shm/.b0nonce 16B 随机数，host 侧全机 fc 进程内容搜索：命中进程 = 沙箱真身、命中段所在同文件连续段组 = guest RAM；实际模式见每轮 B0 行 ram_src=nonce）；无 nonce 退回 memfd 优先（偷 fd 直映）→ anonmap 降级（匿名大小匹配，实测 ublkb mmap 后端失效）"
    echo "b0_magic         = DSAB0BASECALIBv1!（guest 每轮制脏前写 /dev/shm/.b0magic，16 页，anon/nonce 模式 RAM 基址内容级校验双保险）"
    echo "arms             = ${BASE_ARMS[*]}"
    echo "dirty_mb_list    = $DIRTY_MB_LIST"
    echo "repeat           = $REPEAT（每臂每轮 ${ARM_REPEAT} reps，ABBA 块长 $ABBA_BLOCK）"
    echo "cold_flush       = $FLUSH（1 = repeat 前 256MB LLC 冲刷）"
    echo "subset_mb        = $SUBSET_MB（bitmap 超出 / whole 退化的段式采样量）"
    echo "pvm_chunk_kb     = $PVM_CHUNK_KB（假设值，待 A1 bpftrace 校准）"
    echo "batch_n / wait   = $BATCH_N / $DSA_WAIT"
    echo "gcc              = $(gcc --version | head -1)"
    echo "LD_LIBRARY_PATH  = ${LD_LIBRARY_PATH:-none}"
} > "$ENVF"

TOTAL_ARM=$(( ND * REPEAT * ${#BASE_ARMS[@]} ))
echo
echo "== 开跑：arms=[${BASE_ARMS[*]}] dirty=[${DIRTY_MB_LIST}] × ${REPEAT} 轮，共 ${TOTAL_ARM} 次臂调用 =="

# ---------------- 主循环：dirty 档 × round ----------------
for MB in $DIRTY_MB_LIST; do
    echo "===== dirty = ${MB} MB ====="
    for ROUND in $(seq 1 "$REPEAT"); do
        if [ "$PAUSED" -eq 1 ]; then aenv resume "$SBX" || true; PAUSED=0; fi

        # 1) 制脏：guest 内写 tmpfs（urandom 防零页优化；不落盘不走 overlaybd）
        #    先写魔数文件（64KB）：AgentENV 的 patch 版 Firecracker 无 memfd（匿名
        #    mmap），probe 走 anonmap 推断路径时靠它做基址内容级校验；上游 memfd
        #    环境下无害（多 16 页脏页，量级可忽略）。
        #    yes|head 写法不依赖 guest 有 seq（printf $(seq) 在精简 guest 上可能写
        #    出空/16B 文件）；写完 stat 自检——魔数 miss 时先分清「没写进 guest」
        #    vs「基址推断错」。写失败不中止（probe 魔数校验会 NOGO 并给诊断）。
        aenv exec "$SBX" -- sh -c \
            'yes DSAB0BASECALIBv1! | head -c 65536 > /dev/shm/.b0magic' || true
        MSZ="$(aenv exec "$SBX" -- stat -c %s /dev/shm/.b0magic 2>/dev/null | tail -n 1 | tr -d '[:space:]')"
        [ "$MSZ" = "65536" ] || echo "    WARN: .b0magic 大小异常（${MSZ:-空}，期望 65536）——aenv exec sh -c/stat 在 guest 可能不可用；anon 模式 probe 魔数校验将 NOGO"

        #    nonce（16B 随机数）：内容级定位锚——多 fc 并存且 uuid 匹配失效
        #    （见 find_fc 注释）时，probe/臂用 --nonce-hex 在全机 fc 的可读大段
        #    内搜索：命中进程 = 本沙箱 fc 真身，命中段所在同文件连续段组 =
        #    guest RAM（实测后端 mmap /dev/ublkbN 拆两段，匿名大小匹配失效）。
        #    guest 内 dd 生成（urandom；同制脏工具链，已验证可用）+ od 读回
        #    （确保真的进了 guest 内存，读回失败降级回 --fc-pid 定位）。
        #    每轮新 nonce（防跨轮陈旧内容命中；旧 nonce 残留页同属本沙箱 fc，
        #    即使命中也不影响定位正确性）。
        aenv exec "$SBX" -- dd if=/dev/urandom of=/dev/shm/.b0nonce \
            bs=16 count=1 status=none || true
        NONCE_HEX="$(aenv exec "$SBX" -- od -An -tx1 /dev/shm/.b0nonce 2>/dev/null \
            | tr -d '[:space:]')"
        if [ "${#NONCE_HEX}" -eq 32 ]; then
            echo "  [d${MB}MB r${ROUND}] nonce = $NONCE_HEX（probe/臂内容定位锚）"
        else
            NONCE_HEX=""
            echo "    WARN: .b0nonce 生成/读回失败（${NONCE_HEX:-空}，期望 32 hex）——nonce 定位不可用，退回 --fc-pid 定位"
        fi

        #    .b0loc（16 页 × 4KB 位置编码）：probe 布局求解的端到端门禁锚。
        #    每页页首 16B = "B0LOC" + 8 位页号(00000000..00000015) + "END"，
        #    其余 4080B 零填充。probe 求解 memslot 布局候选后逐页 pread 校验：
        #    位图 gpa → 线性偏移 → hva 映射全链路正确 ⇒ ≥12/16 命中；错位
        #    候选 ≈ 0 命中（16 页经错误映射恰好全落在编码页首的概率天文小）。
        #    显式数字列表代替 seq（busybox 兼容，同 .b0magic 教训）；stat
        #    自检同上。失败不中止（probe loc 校验 NOGO loc-miss 时有诊断）。
        aenv exec "$SBX" -- sh -c 'rm -f /dev/shm/.b0loc; for i in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do { printf "B0LOC%08dEND" "$i"; head -c 4080 /dev/zero; } >> /dev/shm/.b0loc; done' || true
        LOCSZ="$(aenv exec "$SBX" -- stat -c %s /dev/shm/.b0loc 2>/dev/null | tail -n 1 | tr -d '[:space:]')"
        [ "$LOCSZ" = "65536" ] || echo "    WARN: .b0loc 大小异常（${LOCSZ:-空}，期望 65536）——probe 布局校验将 NOGO loc-miss"

        if [ "$MB" -gt 0 ]; then
            aenv exec "$SBX" -- dd if=/dev/urandom of=/dev/shm/aenv_dirty \
                bs=1M count="$MB" status=none
        else
            aenv exec "$SBX" -- rm -f /dev/shm/aenv_dirty
        fi
        sleep 1                                 # 等 dirty log 稳定（A 臂同口径）

        # 2) probe：B0 直映 go/no-go + B1 只读 dirty log + dump 页集合。
        #    必须在运行态跑：aenv pause 会把 firecracker 的 guest RAM COW 页
        #    flush+unmap（取证③实锤——pause 后 fc 全 maps 最大段只剩 3MiB r-xp），
        #    nonce 内容定位与三臂回放都读运行态 fc 的 rw-p COW 页；pause 之后
        #    读源消失 → nonce miss（SMOKE 根因）。
        PF="$PAGES_DIR/d${MB}_r${ROUND}.txt"
        if ! PROBE_OUT="$("${NUMACTL[@]+"${NUMACTL[@]}"}" ./replay_offload \
                ${FCPID:+--fc-pid "$FCPID"} ${NONCE_HEX:+--nonce-hex "$NONCE_HEX"} \
                --probe --dump-pages "$PF" \
                --subset-mb "$SUBSET_MB" --seed "$ROUND" \
                --guest-mib "$MEM_MIB" 2>>"$ERRLOG")"; then
            fatal "probe 失败（dirty=${MB}MB round=$ROUND）：$PROBE_OUT（详见 $ERRLOG）"
        fi
        DSLINE="$(grep -m1 '^DIRTYSRC=' <<<"$PROBE_OUT" || true)"
        [ -n "$DSLINE" ] || fatal "probe 输出缺 DIRTYSRC：$PROBE_OUT"
        SRC="${DSLINE#DIRTYSRC=}"; SRC="${SRC%% *}"
        NPAGES="$(grep -oE 'pages=[0-9]+' <<<"$DSLINE" | head -1 | cut -d= -f2 || true)"
        [ -n "$NPAGES" ] || fatal "probe 输出缺 pages：$PROBE_OUT"
        echo "  [d${MB}MB r${ROUND}/${REPEAT}] $(grep -m1 '^B0=' <<<"$PROBE_OUT" || true)"
        echo "  [d${MB}MB r${ROUND}/${REPEAT}] $DSLINE（运行态回放）"

        # 3) 三臂回放（ABBA 序；同轮严格同页集合；运行态读 fc 的 COW 页）
        ORDER="$(arm_order "$ROUND")"
        for ARM in $ORDER; do
            run_arm "$ARM" "$MB" "$ROUND" "$SRC"
        done

        # 4) pause（留 pause wall，供与 A 臂对账；三臂已回放完）
        T0=$(date +%s%3N)
        aenv pause "$SBX"
        T1=$(date +%s%3N)
        PAUSED=1
        PAUSE_MS=$((T1 - T0))

        # 5) resume（补测窗口：wall + server CPU 差分）+ 轮级留痕
        SRV0="$(srv_cpu_ms)"
        RT0=$(date +%s%3N)
        aenv resume "$SBX"
        RT1=$(date +%s%3N)
        SRV1="$(srv_cpu_ms)"
        PAUSED=0
        RESUME_MS=$((RT1 - RT0))
        RESUME_CPU="$(awk -v a="$SRV0" -v b="$SRV1" \
            'BEGIN { print (a == "" || b == "") ? "n/a" : b - a }')"
        echo "${MB},${ROUND},${PAUSE_MS},${SRC},${NPAGES},${ORDER},${RESUME_MS},${RESUME_CPU}" >> "$ROUNDS"
    done
done

# ---------------- 统计：round 级配对 + 95% CI ----------------
echo
echo "== 统计（round 均值 → 臂级聚合；配对差值 95%CI = 1.96·sd/√n） =="
awk -F, \
    -v dlist="$DIRTY_MB_LIST" -v alist="${BASE_ARMS[*]}" -v maxr="$REPEAT" \
    -v summ="$SUMM" -v paired="$PAIRED" '
FNR == 1  { next }                       # 跳表头
$16 != 1  { bad++; next }                # cksum 异常行剔除（编排层双保险）
{ k = $3 SUBSEP $4 SUBSEP $5; s[k] += $14; c[k]++ }
END {
    if (bad > 0) printf "# WARN: %d 行 cksum!=1 已剔除\n", bad > "/dev/stderr"
    # ---- round 级均值（每轮每臂 ARM_REPEAT 个 rep 先平均） ----
    for (k in s) {
        split(k, p, SUBSEP); d = p[1] + 0; r = p[2] + 0; a = p[3]
        rv[d, r, a] = s[k] / c[k]; has[d, r, a] = 1
        dset[d] = 1; aset[a] = 1
    }
    nd = split(dlist, DL, " "); na = split(alist, AL, " ")
    printf "dirty_mb,arm,n_rounds,mean_cpu_ms_per_gb,sd,ci95_lo,ci95_hi\n" > summ
    printf "dirty_mb,arm,n_pairs,pvm_mean,arm_mean,delta_mean_ms_per_gb,drop_ci95_lo_pct,drop_ci95_hi_pct,vs_pvm_drop_pct\n" > paired
    for (i = 1; i <= nd; i++) {
        d = DL[i] + 0
        if (!(d in dset)) continue
        # ---- 臂级统计（n = 轮数） ----
        for (j = 1; j <= na; j++) {
            a = AL[j]
            if (!(a in aset)) continue
            n = 0; sum = 0
            for (r = 1; r <= maxr; r++) if ((d, r, a) in has) { n++; sum += rv[d, r, a] }
            if (n == 0) continue
            mean = sum / n; vv = 0
            for (r = 1; r <= maxr; r++) if ((d, r, a) in has) vv += (rv[d, r, a] - mean)^2
            sd = (n > 1) ? sqrt(vv / (n - 1)) : 0
            ci = 1.96 * sd / sqrt(n)
            if (n < 2) printf "# WARN: %dMB %s 仅 %d 轮，CI 无意义\n", d, a, n > "/dev/stderr"
            printf "%d,%s,%d,%.1f,%.1f,%.1f,%.1f\n", d, a, n, mean, sd, mean - ci, mean + ci > summ
        }
        # ---- 配对（非 pvm 臂 vs pvm，逐轮差值；缺臂轮自动剔除） ----
        if (!("pvm" in aset)) continue
        for (j = 1; j <= na; j++) {
            a = AL[j]
            if (a == "pvm" || !(a in aset)) continue
            kn = 0; dsum = 0; psum = 0; asum = 0
            for (r = 1; r <= maxr; r++)
                if (((d, r, "pvm") in has) && ((d, r, a) in has)) {
                    kn++
                    dsum += rv[d, r, a] - rv[d, r, "pvm"]
                    psum += rv[d, r, "pvm"]; asum += rv[d, r, a]
                }
            if (kn == 0) { printf "# WARN: %dMB %s 无可配对轮（自动剔除）\n", d, a > "/dev/stderr"; continue }
            dmean = dsum / kn; pm = psum / kn; am = asum / kn
            dci = 0
            if (kn > 1) {
                vv = 0
                for (r = 1; r <= maxr; r++)
                    if (((d, r, "pvm") in has) && ((d, r, a) in has))
                        vv += ((rv[d, r, a] - rv[d, r, "pvm"]) - dmean)^2
                dci = 1.96 * sqrt(vv / (kn - 1)) / sqrt(kn)
            }
            if (pm <= 0) continue
            drop = -100 * dmean / pm
            lo = drop - 100 * dci / pm
            hi = drop + 100 * dci / pm
            printf "%d,%s,%d,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f\n",
                   d, a, kn, pm, am, dmean, lo, hi, drop > paired
            tag = (a == "dsa") ? "# UNLOADABLE" : "# DIRECTMAP "
            printf "%s dirty=%dMB: %s vs pvm CPU %.1f → %.1f ms/GB，下降 %.1f%% [95%%CI %.1f~%.1f%%, n=%d]\n",
                   tag, d, a, pm, am, drop, lo, hi, kn > "/dev/stderr"
        }
    }
    close(summ); close(paired)
}' "$DETAIL"

# ---------------- 汇总 ----------------
echo
echo "== 臂级统计（replay_summary.csv） =="
cat "$SUMM"
echo
echo "== 配对统计（replay_paired.csv，核心结论表） =="
cat "$PAIRED"
echo
if [ "$DSA_FAILS" -gt 0 ]; then
    echo "WARN: dsa 臂共失败 ${DSA_FAILS} 轮（已自动剔除；查 $ERRLOG 尾部 [dsa] 行 / LD_LIBRARY_PATH=$DML_LIB）"
fi
echo
echo "解读：vs_pvm_drop_pct > 0 = 该臂比 pvm 基线省 CPU（dsa 为正 → DSA 卸载收益成立）；"
echo "     drop 的 95%CI 不跨 0 才可下结论（配对 n=$REPEAT，ABBA 交错，同沙箱同页集合）。"
echo
echo "产物：detail=$DETAIL  summary=$SUMM  paired=$PAIRED"
echo "      rounds=$ROUNDS  env=$ENVF  err=$ERRLOG  pages=$PAGES_DIR/"
echo "提示：pvm 臂 $PVM_CHUNK_KB""KB iovec 合并是假设值，待 A1 bpftrace 实测后用 PVM_CHUNK_KB=<实测值> 重跑对照。"
if [ "$CLEANUP" != "1" ]; then
    echo "沙箱 $SBX 保留（要删：CLEANUP=1 重跑或手动 aenv delete $SBX）"
fi
