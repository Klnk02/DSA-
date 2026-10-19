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
#   TEMPLATE=my-ubuntu bash baseline_pause_sweep.sh            # 全量：0/64/256/1024/2048 MB x 20 次（pause 方向）
#   TEMPLATE=my-ubuntu REPEAT=3 bash baseline_pause_sweep.sh   # 短版先验链路
#   MODE=resume TEMPLATE=my-ubuntu bash ...                    # 反向：测「恢复」窗口（disk2mem 方向）
#                                                             #   每 rep 先 pause 造增量快照，再包住 resume 窗口
#                                                             #   被测进程集合 = server + fc + uvm-ublk-daemon
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
MODE="${MODE:-pause}"                        # pause = mem2disk 方向（快照/卸载 guest RAM）
                                             # resume = disk2mem 方向（恢复：layer→guest RAM）
                                             # 两方向真实路径都在内核里搬，DSA 都插不进去；
                                             # 分别扫一遍才有「真实基线」可用（README §4.5）

command -v aenv >/dev/null 2>&1 || { echo "ERROR: aenv CLI 不在 PATH"; exit 1; }
command -v perf >/dev/null 2>&1 || { echo "ERROR: perf 不可用"; exit 1; }

# ---------------- 进程发现（4.2 实测时进程名即 server / uvm-ublk-daemon） ----------------
find_server_pid() { pgrep -x server | head -1 || true; }
find_ublk_pid()   { pgrep -f uvm-ublk-daemon | head -1 || true; }
# firecracker 仅在唯一时可信（多沙箱共享机上 pgrep 拿不到本沙箱真身）；
# resume 方向的缺页发生在 fc 进程里，拿不到就如实标 n/a（那是口径下界，不是 0）
find_fc_pid() {
    local n; n="$(pgrep -x firecracker 2>/dev/null | wc -l || true)"
    [ "${n//[[:space:]]/}" = "1" ] && pgrep -x firecracker | head -1 || true
}
SERVER_PID="$(find_server_pid)"
UBLK_PID="$(find_ublk_pid)"
FIRECRACKER_PID="${FIRECRACKER_PID:-$(find_fc_pid)}"
if [ -z "$SERVER_PID" ]; then
    echo "ERROR: 找不到 orchestrator(server) 进程，确认 aenv server 在跑（Docker 部署也一样能看到）"
    exit 1
fi
# 被测进程集合：resume 方向把 fc 与 ublk 一起包进来（恢复路径横跨三者），
# pause 方向与 4.2 对齐只包 server（保持历史口径可比）
if [ "$MODE" = "resume" ]; then
    PERF_PIDLIST="$SERVER_PID${FIRECRACKER_PID:+,$FIRECRACKER_PID}${UBLK_PID:+,$UBLK_PID}"
    [ -n "$FIRECRACKER_PID" ] || echo "WARN: fc 不唯一或未找到 → resume 口径不含 fc 缺页 CPU（下界，如实标注）"
else
    PERF_PIDLIST="$SERVER_PID"
fi
echo "mode=$MODE ; orchestrator(server) = pid $SERVER_PID ; ublk = ${UBLK_PID:-none} ; fc = ${FIRECRACKER_PID:-none}"
echo "被测进程集合 = $PERF_PIDLIST"

if [ "$MODE" = "resume" ]; then
    OUT="$OUT_DIR/baseline_resume_sweep.csv"
    SUM="$OUT_DIR/baseline_resume_sweep_summary.csv"
else
    OUT="$OUT_DIR/baseline_sweep.csv"
    SUM="$OUT_DIR/baseline_sweep_summary.csv"
fi

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
mkdir -p "$OUT_DIR/perf"
if [ "$MODE" = "resume" ]; then
    # 列名随方向变，列位保持不变（汇总 awk 按列位取数）
    echo "ts,dirty_mb,rep,resume_wall_ms,restore_taskclk_ms,restore_cycles_m,note" > "$OUT"
else
    echo "ts,dirty_mb,rep,pause_wall_ms,server_taskclk_ms,server_cycles_m,note" > "$OUT"
fi

for mb in $DIRTY_MB_LIST; do
    echo "===== dirty = ${mb} MB ====="
    for i in $(seq 1 "$REPEAT"); do
        if [ "$MODE" = "pause" ]; then
            # ---- mem2disk 方向：直接测 pause 窗口（4.2 口径） ----
            if [ "$PAUSED" -eq 1 ]; then aenv resume "$SBX"; PAUSED=0; fi
            make_dirty_mb "$mb"
            sleep 1                          # 等 dirty log 稳定
            t0=$(date +%s%N)
            aenv pause "$SBX" & tp=$!
            PHASE="pause"
        else
            # ---- disk2mem 方向：先 pause（造增量快照 + 让 guest RAM 从 fc 撤出），
            #      再测 resume 窗口 —— 恢复路径就是要从 layer 把数据搬回 guest，
            #      pause 本身不计入窗口（它的 wall 记在 note 里，可对账） ----
            make_dirty_mb "$mb"
            sleep 1
            pt0=$(date +%s%N)
            aenv pause "$SBX"
            pt1=$(date +%s%N)
            PREPAUSE_MS=$(( (pt1 - pt0) / 1000000 ))
            PAUSED=1
            t0=$(date +%s%N)
            aenv resume "$SBX" & tp=$!
            PHASE="resume"
        fi
        rec=""
        if [ $(( i % PERF_SAMPLE_EVERY )) -eq 1 ]; then
            perf record -F 999 -g -p "$PERF_PIDLIST" \
                -o "$OUT_DIR/perf/${PHASE}_dirty${mb}mb_rep${i}.data" \
                -- tail --pid="$tp" -f /dev/null >/dev/null 2>&1 || true &
            rec=$!
        fi
        # task-clock(ms) + cycles，恰好覆盖窗口（tail 随 aenv pause/resume 退出而退出）
        stat_out=$(perf stat -p "$PERF_PIDLIST" -x, -e task-clock,cycles \
            -- tail --pid="$tp" -f /dev/null 2>&1 || true)
        if [ -n "$rec" ]; then wait "$rec" || true; fi
        wait "$tp"
        t1=$(date +%s%N)
        if [ "$MODE" = "pause" ]; then PAUSED=1; else PAUSED=0; fi

        wall_ms=$(( (t1 - t0) / 1000000 ))
        taskclk=$(awk -F, '$3 ~ /task-clock/ {v=$1+0; if ($2 ~ /^sec$/) v*=1000; printf "%.1f", v; exit}' <<<"$stat_out")
        cycm=$(awk    -F, '$3 ~ /cycles/     {printf "%.1f", $1/1e6; exit}' <<<"$stat_out")
        if [ -z "$taskclk" ]; then echo "  WARN: task-clock 为空（perf 权限 / 进程号?）"; fi

        note=""
        [ "$MODE" = "resume" ] && note="mode=resume"
        [ -n "$rec" ] && note="${note:+$note; }perfrec"
        [ "$MODE" = "resume" ] && note="${note:+$note; }pre_pause_wall=${PREPAUSE_MS}ms"
        echo "$(date +%F_%T),${mb},${i},${wall_ms},${taskclk},${cycm},${note}" >> "$OUT"
        echo "  rep ${i}: ${PHASE} wall=${wall_ms}ms  taskclk=${taskclk:-?}ms"
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
if [ "$MODE" = "resume" ]; then
    echo "提示: 恢复侧（disk2mem）纯搬运 CPU 同样用「taskclk_avg(dirty=N) − taskclk_avg(0)」除以 N/1024 GB"
    echo "      口径限定: 被测集合含 fc 缺页 + ublk 伺候 + orchestrator；DSA 在这条路径上结构不可介入"
    echo "      （搬运全在内核: ublk/overlaybd 读 + 映射缺页），故这里是「真实软件基线」，不是 DSA 可行域"
else
    echo "提示: 纯搬运 CPU = taskclk_avg(dirty=N) - taskclk_avg(0)，再除以 N/1024 GB 得 ms/GB"
    echo "      （对照 4.2: 256MB 档 (288.94-53.32)/0.25 = 942 ms/GB）"
fi
