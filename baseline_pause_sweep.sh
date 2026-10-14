#!/usr/bin/env bash
# baseline_pause_sweep.sh — 阶段 4.4 · A 臂基线（零代码改动）
#
# 目的：把 4.2 的单点（256MB）扩成曲线 —— 真实 AgentENV snapshot pause 的
#       wall + orchestrator(server) CPU 随 dirty 量的变化，作为 B 臂（DSA 接入）的对照组。
#
# 方法（与 4.2 对齐，见 results/stage4-sandbox/snapshot_offload.csv）：
#   dirty  = guest 内 dd 写 /dev/shm（tmpfs，匿名内存不落盘，不走 overlaybd）
#   触发   = aenv pause <sandbox-id>（阻塞到 pause 完成）
#   CPU    = perf stat -p <server_pid> -e task-clock,cycles 包住整个 pause 窗口
#   wall   = aenv pause 命令自身耗时（含 CLI/HTTP 往返，量级 ms，A/B 两臂同口径）
#   符号   = 每 N 次抽 1 次 perf record（server + uvm-ublk-daemon）留档
#
# 前置（一次性）：
#   aenv auth 已配置；server 已运行（systemctl status aenv / curl :8000/health）
#   TEMPLATE 换成你们实际用的模板名（aenv pull --name 的那个）
#   guest 内存 >= 最大 dirty 档 x 1.5（不够就 COLD=1 + MEM_MIB=8192 起新沙箱）
#
# 用法：
#   TEMPLATE=my-ubuntu bash baseline_pause_sweep.sh            # 全量：0/64/256/1024/2048 MB x 20 次
#   TEMPLATE=my-ubuntu REPEAT=3 bash baseline_pause_sweep.sh   # 短版先验链路
#   CLEANUP=1 bash ...                                         # 跑完删沙箱
set -euo pipefail
cd "$(dirname "$0")"

# ---------------- 可调参数 ----------------
TEMPLATE="${TEMPLATE:-ubuntu}"               # 模板名；纯 OCI 镜像用 COLD=1
COLD="${COLD:-0}"                            # 1 = aenv start --cold（OCI 镜像，可配 vCPU/内存）
CPU_COUNT="${CPU_COUNT:-}"                   # 仅 COLD=1 生效
MEM_MIB="${MEM_MIB:-}"                       # 仅 COLD=1 生效，注意要 >= 最大 dirty 档 x 1.5
DIRTY_MB_LIST="${DIRTY_MB_LIST:-0 64 256 1024 2048}"   # 0 = 固定开销对照组（4.2 的 pause_fixed_overhead）
REPEAT="${REPEAT:-20}"
PERF_SAMPLE_EVERY="${PERF_SAMPLE_EVERY:-7}"  # 每 7 次抽 1 次 perf record 留符号
OUT_DIR="results/stage4-sandbox"
GUEST_DIRTY=/dev/shm/aenv_dirty              # guest 内的 dirty 文件（tmpfs）

command -v aenv >/dev/null 2>&1 || { echo "ERROR: aenv CLI 不在 PATH"; exit 1; }
command -v perf >/dev/null 2>&1 || { echo "ERROR: perf 不可用"; exit 1; }

# ---------------- 进程发现（4.2 实测时进程名即 server / uvm-ublk-daemon） ----------------
find_server_pid() { pgrep -x server | head -1 || true; }
find_ublk_pid()   { pgrep -f uvm-ublk-daemon | head -1 || true; }
SERVER_PID="$(find_server_pid)"
UBLK_PID="$(find_ublk_pid)"
if [ -z "$SERVER_PID" ]; then
    echo "ERROR: 找不到 orchestrator(server) 进程，确认 aenv server 在跑（Docker 部署也一样能看到）"
    exit 1
fi
echo "orchestrator(server) = pid $SERVER_PID ; ublk = ${UBLK_PID:-none}"

# ---------------- 沙箱准备（整轮复用一个，pause/resume 循环） ----------------
start_args=(-d)
if [ "$COLD" = "1" ]; then
    start_args+=(--cold)
    if [ -n "$CPU_COUNT" ]; then start_args+=(--cpu "$CPU_COUNT"); fi
    if [ -n "$MEM_MIB" ];   then start_args+=(--memory "$MEM_MIB"); fi
fi
SBX="$(aenv start "${start_args[@]}" "$TEMPLATE" 2>/dev/null \
       | grep -oE '[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}' | head -1 || true)"
if [ -z "$SBX" ]; then
    echo "ERROR: 启动沙箱失败（检查 TEMPLATE 名字 / aenv auth / 镜像是否存在）"
    exit 1
fi
aenv timeout "$SBX" 43200                    # 默认 TTL 300s，必须拉长，否则跑到一半沙箱过期
echo "sandbox = $SBX"
PAUSED=0

make_dirty_mb() {                            # guest 内写 $1 MB tmpfs（0 = 清空，作固定开销对照）
    local mb="$1"
    if [ "$mb" -eq 0 ]; then
        aenv exec "$SBX" -- rm -f "$GUEST_DIRTY"
    else
        # urandom 防零页优化；写 /dev/shm 不落盘、不走 overlaybd（4.2 的坑）
        aenv exec "$SBX" -- dd if=/dev/urandom of="$GUEST_DIRTY" bs=1M count="$mb"
    fi
}

# ---------------- 主循环 ----------------
OUT="$OUT_DIR/baseline_sweep.csv"
SUM="$OUT_DIR/baseline_sweep_summary.csv"
mkdir -p "$OUT_DIR/perf"
echo "ts,dirty_mb,rep,pause_wall_ms,server_taskclk_ms,server_cycles_m,note" > "$OUT"

for mb in $DIRTY_MB_LIST; do
    echo "===== dirty = ${mb} MB ====="
    for i in $(seq 1 "$REPEAT"); do
        if [ "$PAUSED" -eq 1 ]; then aenv resume "$SBX"; PAUSED=0; fi
        make_dirty_mb "$mb"
        sleep 1                              # 等 dirty log 稳定

        t0=$(date +%s%N)
        aenv pause "$SBX" & tp=$!
        rec=""
        if [ $(( i % PERF_SAMPLE_EVERY )) -eq 1 ]; then
            pids="$SERVER_PID${UBLK_PID:+,$UBLK_PID}"
            perf record -F 999 -g -p "$pids" \
                -o "$OUT_DIR/perf/dirty${mb}mb_rep${i}.data" \
                -- tail --pid="$tp" -f /dev/null >/dev/null 2>&1 || true &
            rec=$!
        fi
        # task-clock(ms) + cycles，恰好覆盖 pause 窗口（tail 随 aenv pause 退出而退出）
        stat_out=$(perf stat -p "$SERVER_PID" -x, -e task-clock,cycles \
            -- tail --pid="$tp" -f /dev/null 2>&1 || true)
        if [ -n "$rec" ]; then wait "$rec" || true; fi
        wait "$tp"
        t1=$(date +%s%N)
        PAUSED=1

        wall_ms=$(( (t1 - t0) / 1000000 ))
        taskclk=$(awk -F, '$3 ~ /task-clock/ {v=$1+0; if ($2 ~ /^sec$/) v*=1000; printf "%.1f", v; exit}' <<<"$stat_out")
        cycm=$(awk    -F, '$3 ~ /cycles/     {printf "%.1f", $1/1e6; exit}' <<<"$stat_out")
        if [ -z "$taskclk" ]; then echo "  WARN: task-clock 为空（perf 权限 / 进程号?）"; fi

        echo "$(date +%F_%T),${mb},${i},${wall_ms},${taskclk},${cycm},${rec:+perfrec}" >> "$OUT"
        echo "  rep ${i}: wall=${wall_ms}ms  server_taskclk=${taskclk:-?}ms"
    done
done

# ---------------- 汇总（wall 与 taskclk 各算 avg/stddev/min/max） ----------------
# 剔除 note=perfrec 的行：perf record 的 profiling 开销会把 wall 抬高 ~2.2s（实测 3.3~3.5s vs 1.17s）
awk -F, '
NR>1 && $7 !~ /perfrec/ {
    k=$2; n[k]++
    if ($4 ~ /^[0-9]+(\.[0-9]+)?$/) { nw[k]++; w[k]+=$4; ws[k]+=$4*$4;
        if (!(k in wmn) || $4<wmn[k]) wmn[k]=$4; if ($4>wmx[k]) wmx[k]=$4 }
    if ($5 ~ /^[0-9]+(\.[0-9]+)?$/) { nc[k]++; c[k]+=$5; cs[k]+=$5*$5;
        if (!(k in cmn) || $5<cmn[k]) cmn[k]=$5; if ($5>cmx[k]) cmx[k]=$5 }
}
END {
    printf "dirty_mb,n,wall_avg_ms,wall_sd,taskclk_avg_ms,taskclk_sd,taskclk_min,taskclk_max\n"
    for (k in n) {
        wa  = (k in nw) ? w[k]/nw[k] : 0
        wsd = (k in nw && nw[k]>1) ? sqrt(ws[k]/nw[k]-wa*wa) : 0
        ca  = (k in nc) ? c[k]/nc[k] : 0
        csd = (k in nc && nc[k]>1) ? sqrt(cs[k]/nc[k]-ca*ca) : 0
        printf "%s,%d,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f\n", k, n[k], wa, wsd, ca, csd, \
               (k in cmn)?cmn[k]:0, (k in cmx)?cmx[k]:0
    }
}' "$OUT" | sort -t, -k1,1n > "$SUM"

echo
echo "疑似漏 dirty 的行（dirty_mb>0 但 taskclk≈0，统计时应剔除或复查）："
awk -F, 'NR>1 && ($2+0)>0 && ($5=="" || ($5+0)<1) {print}' "$OUT"

if [ "${CLEANUP:-0}" = "1" ]; then
    aenv delete "$SBX"
else
    echo "sandbox $SBX 保留（要删：CLEANUP=1，或手动 aenv delete $SBX）"
fi

echo
echo "detail  -> $OUT"
echo "summary -> $SUM"
cat "$SUM"
echo
echo "提示: 纯搬运 CPU = taskclk_avg(dirty=N) - taskclk_avg(0)，再除以 N/1024 GB 得 ms/GB"
echo "      （对照 4.2: 256MB 档 (288.94-53.32)/0.25 = 942 ms/GB）"
