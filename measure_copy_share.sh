#!/usr/bin/env bash
# measure_copy_share.sh — 一条命令测「真实 pause CPU 中数据拷贝段占比」（带置信区间）
# 当前边界见CURRENT_STATUS.md：其他命令失败处理待补；A为整体CPU斜率，B为另测窗口占比。
#
# 目的：把 4.2 的 12.6%（285 样本的点估计）换成带置信区间的可靠值。
# 做四件事，最后打印一个 === RESULT === 汇总块：
#   A 差分法   ：perf stat task-clock 随 dirty 量做最小二乘 → 搬运段总量 b(ms/GiB) ± 95%CI + R²
#   B 大样本计数：perf record -F4000 包住 N 个 pause 事件 → 拷贝符号占比 + Wilson 95%CI
#   C 环境留档 ：loadavg / 共存 firecracker 数，前后各采一次（回答「是否纯净」）
#   D 诊断兜底 ：PID 校准 / perf 内核态自检 / /proc/io 字节硬证据 / kprobe 计数
#
# 2026-10-19 那轮（copy=0/80082 且符号全空）暴露的问题，本版修复：
#   1) pgrep 在 aenv start 之前跑 → 新沙箱的 server 还没起来，记到的是别的进程
#      → 改为起沙箱后采集候选（server + uvm-ublk-daemon），
#        并用「一次探针 pause 的 CPU 增量」锁定真正干活的 PID（宿主机共 5 个 firecracker）
#   2) copy_page_* 是内核函数：内核态采样被 perf_event_paranoid/kptr_restrict 限制时
#      符号计数结构性为 0（≠拷贝为 0）→ 开头自检 cycles:k + 尽力放开 sysctl + perf 警告全留 warn.log
#   3) perf script -F sym 符号全空看不出原因 → 改 -F dso,sym（能看出样本落在内核还是用户态）
#   4) 结果提取把 "Wilson95CI"/"95CI" 字样里的 95 当成 CI 数值 → 改为 sed 锚定提取
#   5) A 的 dirty 档位顺序执行，与 loadavg 上升（0.65→1.26）正相关 → 改为轮转交错
#
# 2026-10-19 第 2 轮（339064 样本但符号计数全 0；60×HTTP500；A R2=0.113）暴露的问题，本版修复：
#   6) perf 5.15.196 的 `perf script -F dso,sym` 每行输出空白（同数据默认字段/报告符号均正常，
#      仅 -F 字段路径坏）→ B 解析改走 `perf report --stdio -n --sort dso,symbol`（自带每符号样本数）
#   7) aenv pause 失败（HTTP 500 = overlaybd 层文件磁盘满）混进统计：
#      A 段失败点 task-clock 置空（回归自动剔除）；B 段失败事件样本改名 fail_s*.data 隔离；RESULT 报告成败计数
#   8) 多候选时 resume 后重算 Δcpu 把静默进程混进 PERF_PIDLIST → 改用 pause 窗口内记下的增量筛选
#
# 2026-10-19 第 3 轮（全量跑）暴露的问题，本版修复：
#   9) 「/proc/kallsyms 仍全是 0」自检误报——x86-64 的 percpu 行（A fixed_percpu_data 等）
#      天然是 0 偏移，行首 0 匹配在任何正常系统上必中；改看 _stext/_text 地址判真抹零
#      （B 解析走 perf report 路线，第 2 轮已实证不受 kallsyms 抹零影响）
#
# 一条命令用法：
#   TEMPLATE=<你的模板> bash measure_copy_share.sh              # 自动起沙箱
#   SBX=<已运行沙箱id> bash measure_copy_share.sh               # 复用现有沙箱
#   先 smoke（几十秒，确认 B 的样本数 >>1000 再跑全量）：
#     REPEAT_A=5 REPEAT_B=3 SBX=<id> bash measure_copy_share.sh
#   全量：REPEAT_A=20 REPEAT_B=60 DIRTY_B=256 DIRTY_MB_LIST="0 64 256 1024"
#         COLD=1 CPU_COUNT=4 MEM_MIB=8192 （冷启动镜像时用）
#
# 产物：results/stage4-sandbox/copy_share/{sweep.csv,syms.txt,io_deltas.csv,
#       A_result.txt,B_result.txt,env_before.txt,env_after.txt,perf/{s*.data,warn.log}}
set -uo pipefail
cd "$(dirname "$0")"

TEMPLATE="${TEMPLATE:-ubuntu}"
SBX="${SBX:-}"
COLD="${COLD:-0}"; CPU_COUNT="${CPU_COUNT:-}"; MEM_MIB="${MEM_MIB:-}"
DIRTY_MB_LIST="${DIRTY_MB_LIST:-0 64 256 1024}"
REPEAT_A="${REPEAT_A:-20}"
REPEAT_B="${REPEAT_B:-60}"
DIRTY_B="${DIRTY_B:-256}"
FREQ="${FREQ:-4000}"
OUT_DIR="results/stage4-sandbox/copy_share"
GUEST_DIRTY=/dev/shm/aenv_dirty

command -v aenv >/dev/null || { echo "ERROR: aenv 不在 PATH"; exit 1; }
command -v perf >/dev/null || { echo "ERROR: perf 不可用"; exit 1; }
mkdir -p "$OUT_DIR/perf"
: > "$OUT_DIR/perf/warn.log"   # perf 的 stderr 不再吞掉，全进这里

make_dirty() {
    local mb="$1"
    if [ "$mb" -eq 0 ]; then aenv exec "$SBX" -- rm -f "$GUEST_DIRTY"
    else aenv exec "$SBX" -- dd if=/dev/urandom of="$GUEST_DIRTY" bs=1M count="$mb"; fi
}
env_snap() { echo "$(date +%F_%T) loadavg=$(cut -d' ' -f1-3 /proc/loadavg) firecracker_n=$(pgrep -c -x firecracker 2>/dev/null || echo 0)"; }
# 进程 CPU（utime+stime，tick）—— /proc/PID/stat 的 comm 带括号，先砍掉前缀再取第 12/13 列
pid_cpu() { local v; v=$(sed 's/.*) //' /proc/$1/stat 2>/dev/null | awk '{print $12+$13}'); echo "${v:-0}"; }
# 被测进程的 /proc/PID/io 快照：rchar/wchar = 经 read()/write() 搬运的字节（正好对应两个 copy 段）
io_snap() {
    local p
    for p in ${PERF_PIDLIST//,/ }; do
        awk -v pid="$p" '/^rchar:/{r=$2}/^wchar:/{w=$2}/^read_bytes:/{rb=$2}/^write_bytes:/{wb=$2}
            END{printf "%s %s %s %s %s\n", pid, r+0, w+0, rb+0, wb+0}' /proc/$p/io 2>/dev/null || true
    done
}

# ---------------- 沙箱（必须先起沙箱再选进程！） ----------------
if [ -z "$SBX" ]; then
    start_args=(-d)
    if [ "$COLD" = "1" ]; then
        start_args+=(--cold)
        [ -n "$CPU_COUNT" ] && start_args+=(--cpu "$CPU_COUNT")
        [ -n "$MEM_MIB" ]   && start_args+=(--memory "$MEM_MIB")
    fi
    SBX="$(aenv start "${start_args[@]}" "$TEMPLATE" 2>/dev/null \
           | grep -oE '[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}' | head -1 || true)"
    [ -n "$SBX" ] || { echo "ERROR: 启动沙箱失败（TEMPLATE/aenv auth 检查）"; exit 1; }
    aenv timeout "$SBX" 43200
else
    aenv resume "$SBX" >/dev/null 2>&1 || true
fi
sleep 2   # 等 server / uvm-ublk-daemon 稳定

# ---------------- D1：选定被测进程（多沙箱共存时防止记错进程） ----------------
mapfile -t CANDS < <({ pgrep -x server; pgrep -f uvm-ublk-daemon; } | sort -un)
[ "${#CANDS[@]}" -ge 1 ] || { echo "ERROR: 找不到 server / uvm-ublk-daemon 进程"; exit 1; }
if [ "${#CANDS[@]}" -eq 1 ]; then
    PERF_PIDLIST="${CANDS[0]}"
    echo "被测进程：$PERF_PIDLIST（唯一候选）"
else
    echo "候选进程 ${#CANDS[@]} 个（多沙箱共存）→ 用一次探针 pause 的 CPU 增量锁定真正干活的进程："
    declare -A C0=() D=()
    for p in "${CANDS[@]}"; do C0[$p]=$(pid_cpu "$p"); done
    aenv pause "$SBX" >/dev/null 2>&1 || true
    maxd=0
    for p in "${CANDS[@]}"; do
        d=$(( $(pid_cpu "$p") - ${C0[$p]} ))
        D[$p]=$d                                    # 存下 pause 窗口内的增量（resume 后不重算）
        echo "  pid=$p  Δcpu=${d}tick"
        [ "$d" -gt "$maxd" ] && maxd=$d
    done
    aenv resume "$SBX" >/dev/null 2>&1 || true
    PERF_PIDLIST=""
    for p in "${CANDS[@]}"; do
        # 用 pause 窗口内记下的 D 筛选（旧版 resume 后重算，静默进程被 resume 激活而混入）
        if [ "$maxd" -ge 1 ] && [ $(( D[$p] * 10 )) -ge "$maxd" ]; then
            PERF_PIDLIST="${PERF_PIDLIST:+$PERF_PIDLIST,}$p"
        fi
    done
    if [ -n "$PERF_PIDLIST" ]; then
        echo "选定被测进程：$PERF_PIDLIST（pause 期间 CPU 增量 ≥ 最大者的 1/10）"
    else
        PERF_PIDLIST="${CANDS[0]}"
        echo "WARN: 探针 pause 期间无候选进程有 CPU 增量（tick=10ms 粒度? pause 失败?）→ 退回候选[0]=$PERF_PIDLIST"
    fi
fi
echo "copy 段（copy_page_to_iter / copy_page_from_iter_atomic）是内核函数，在被测进程 read/write 路径上执行"

# ---------------- D2：perf 权限自检（内核态采不到 → copy 符号结构性为 0） ----------------
echo "--- perf 权限自检 ---"
if printf '%s' 0 > /proc/sys/kernel/kptr_restrict 2>/dev/null; then
    echo "  kptr_restrict=0（已放开）"
else
    echo "  kptr_restrict 写入失败（当前=$(cat /proc/sys/kernel/kptr_restrict 2>/dev/null)）→ 内核符号可能解析不出"
fi
if printf -- '-1' > /proc/sys/kernel/perf_event_paranoid 2>/dev/null; then
    echo "  perf_event_paranoid=-1（已放开）"
else
    echo "  perf_event_paranoid 写入失败（当前=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null)）→ 内核态采样可能被禁"
fi
# 判真抹零看 _stext/_text 地址；x86-64 的 percpu 行（A fixed_percpu_data 等）天然是 0 偏移，行首匹配必误报
ka=$(awk '$3=="_stext" || $3=="_text" {print $1; exit}' /proc/kallsyms 2>/dev/null)
[ "${ka:-}" = "0000000000000000" ] && echo "  /proc/kallsyms 地址真被抹零（kptr_restrict）→ 内核符号解析不出来"
kout=$(perf stat -x, -e cycles:k -a -- sleep 0.3 2>&1 >/dev/null || true)
printf '%s\n' "$kout" >> "$OUT_DIR/perf/warn.log"
kval=$(grep -m1 'cycles:k' <<<"$kout" || true); kval=${kval%%,*}
if [[ $kval =~ ^[0-9]+$ ]] && [ "$kval" -gt 0 ]; then
    KERN_OK=1; echo "  cycles:k=${kval} → 内核态采样 OK"
else
    KERN_OK=0; echo "  cycles:k 计不到 → 内核态采样被限制：copy_page_*（内核函数）结构性采不到，B 的符号计数会恒为 0（≠拷贝耗时为 0）"
fi

echo "--- env before ---"; env_snap | tee "$OUT_DIR/env_before.txt"

# ---------------- A：差分法（分母锚点） ----------------
echo "=== A: 差分扫描 dirty=${DIRTY_MB_LIST} x ${REPEAT_A}（档位轮转交错，避免与负载漂移相关） ==="
echo "ts,dirty_mb,rep,taskclk_ms,wall_ms" > "$OUT_DIR/sweep.csv"
PAUSED=0; FAIL_A=0
for i in $(seq 1 "$REPEAT_A"); do
    for mb in $DIRTY_MB_LIST; do
        if [ "$PAUSED" -eq 1 ]; then aenv resume "$SBX" >/dev/null 2>&1 || true; PAUSED=0; fi
        make_dirty "$mb" >/dev/null 2>&1 || true
        sleep 1
        t0=$(date +%s%N)
        # perf 直接以 aenv pause 为子进程：录制窗口精确等于 pause 执行时间（去掉 & + tail 的竞态）
        so=$(perf stat -p "$PERF_PIDLIST" -x, -e task-clock -- aenv pause "$SBX" 2>&1 >/dev/null || true)
        t1=$(date +%s%N); PAUSED=1
        if grep -q "HTTP 500" <<<"$so"; then
            # pause 失败（多为磁盘满）：compact 半途中断，task-clock 与成功事件不可比 → 置空，回归自动剔除
            printf '%s\n' "$so" >> "$OUT_DIR/perf/warn.log"
            FAIL_A=$(( FAIL_A + 1 )); tc=""
        else
            tc=$(awk -F, '$3 ~ /task-clock/ {v=$1+0; if ($2 ~ /^sec$/) v*=1000; else if ($2 ~ /^usec$/) v/=1000; printf "%.1f", v; exit}' <<<"$so")
        fi
        echo "$(date +%F_%T),$mb,$i,${tc:-},$(( (t1-t0)/1000000 ))" >> "$OUT_DIR/sweep.csv"
    done
    echo "  round ${i}/${REPEAT_A} 完成"
done

awk -F, 'NR>1 && $4 ~ /^[0-9.]+$/ { n++; X[n]=$2/1024; Y[n]=$4 }
END {
    if (n<3) { print "A: 样本不足(n<3)，task-clock 可能为空（perf 权限?）"; exit }
    for(i=1;i<=n;i++){sx+=X[i]; sy+=Y[i]}
    mx=sx/n; my=sy/n
    for(i=1;i<=n;i++){d=X[i]-mx; Sxx+=d*d; Sxy+=d*(Y[i]-my)}
    b=Sxy/Sxx; a=my-b*mx
    for(i=1;i<=n;i++){e=Y[i]-(a+b*X[i]); SSE+=e*e; SST+=(Y[i]-my)^2}
    seb=sqrt((SSE/(n-2))/Sxx)
    printf "A: n=%d  fixed=%.1fms  transfer=%.0f ms/GiB  95CI=+-%.0f  R2=%.3f\n", n,a,b,1.96*seb,(SST>0?1-SSE/SST:0)
}' "$OUT_DIR/sweep.csv" | tee "$OUT_DIR/A_result.txt"
awk -F, 'NR>1 && $4 ~ /^[0-9.]+$/ { s[$2]+=$4; n[$2]++ } END { for (m in s) printf "  dirty=%sMB: mean=%.1fms n=%d\n", m, s[m]/n[m], n[m] }' "$OUT_DIR/sweep.csv"

# ---------------- B：大样本符号计数（分子） + /proc/io 字节硬证据 ----------------
echo "=== B: perf record -F${FREQ} 包 ${REPEAT_B} 个 pause 事件（dirty=${DIRTY_B}MB） ==="
rm -f "$OUT_DIR"/perf/s*.data "$OUT_DIR"/perf/fail_s*.data   # 清掉上一轮旧样本与失败隔离样本，避免污染统计
echo "event,pid,d_rchar,d_wchar,d_read_bytes,d_write_bytes" > "$OUT_DIR/io_deltas.csv"
FAIL_B=0
for i in $(seq 1 "$REPEAT_B"); do
    aenv resume "$SBX" >/dev/null 2>&1 || true
    make_dirty "$DIRTY_B" >/dev/null 2>&1 || true
    sleep 1
    io_snap > "$OUT_DIR/io_before.tmp"
    # 同 A：perf 直接以 aenv pause 为子进程，窗口 = pause 执行时间；stderr 单独落盘以判 pause 成败
    perf record -F "$FREQ" -p "$PERF_PIDLIST" -o "$OUT_DIR/perf/s$i.data" \
        -- aenv pause "$SBX" >/dev/null 2>"$OUT_DIR/perf/record_$i.err" || true
    cat "$OUT_DIR/perf/record_$i.err" >> "$OUT_DIR/perf/warn.log"
    if grep -q "HTTP 500" "$OUT_DIR/perf/record_$i.err"; then
        # pause 失败事件：compact 半途中断，CPU 分布与成功事件不可比 → 样本隔离出统计（fail_s*.data）
        FAIL_B=$(( FAIL_B + 1 ))
        mv "$OUT_DIR/perf/s$i.data" "$OUT_DIR/perf/fail_s$i.data" 2>/dev/null || true
    fi
    rm -f "$OUT_DIR/perf/record_$i.err"
    io_snap > "$OUT_DIR/io_after.tmp"
    awk -v ev="$i" 'NR==FNR{r[$1]=$2;w[$1]=$3;rb[$1]=$4;wb[$1]=$5;next}
        {printf "%d,%s,%d,%d,%d,%d\n", ev, $1, $2-r[$1], $3-w[$1], $4-rb[$1], $5-wb[$1]}' \
        "$OUT_DIR/io_before.tmp" "$OUT_DIR/io_after.tmp" >> "$OUT_DIR/io_deltas.csv"
    aenv resume "$SBX" >/dev/null 2>&1 || true
    [ $(( i % 20 )) -eq 0 ] && echo "  ${i}/${REPEAT_B} 事件完成"
done
rm -f "$OUT_DIR/io_before.tmp" "$OUT_DIR/io_after.tmp"

: > "$OUT_DIR/syms.txt"   # 行格式：samples dso tag sym（tag = [k]/[.]）
echo "--- 每符号样本数（perf report -n 聚合；perf script -F dso,sym 在 5.15.196 输出全空行，已弃用）---"
tot=0
for f in "$OUT_DIR"/perf/s*.data; do
    [ -f "$f" ] || continue
    out=$(perf report -i "$f" --stdio -n --no-children --sort dso,symbol 2>>"$OUT_DIR/perf/warn.log" \
          | awk '/^#/ {next} $1 ~ /^[0-9.]+%$/ && NF>=5 { print $2, $3, $4, $5 }')
    n=$(awk '{s+=$1} END{printf "%.0f", s+0}' <<<"$out")
    tot=$(( tot + n ))
    printf '%s\n' "$out" >> "$OUT_DIR/syms.txt"
    [ "$n" -eq 0 ] && echo "  $(basename "$f"): 0（该事件无样本）"
done
echo "  合计样本 = $tot（仅成功事件；失败事件已隔离为 fail_s*.data）"
[ "$tot" -lt 2000 ] && echo "WARN: 样本过少（<2000）→ record 未采样或事件全失败；看 $OUT_DIR/perf/warn.log"

kern_n=$(awk '$2 ~ /^\[kernel/ {s+=$1} END{printf "%.0f", s+0}' "$OUT_DIR/syms.txt")                     # 内核态样本数
kern_named=$(awk '$2 ~ /^\[kernel/ && $4 !~ /^[0-9a-f]{8,}$/ && $4 != "[unknown]" {s+=$1} END{printf "%.0f", s+0}' "$OUT_DIR/syms.txt")   # 其中符号解析出来的
copy_strict=$(awk '$4 ~ /copy_page_to_iter|copy_page_from_iter/ {s+=$1} END{printf "%.0f", s+0}' "$OUT_DIR/syms.txt")
copy_broad=$(awk '$2 ~ /^\[kernel/ && $4 ~ /copy_page_to_iter|copy_page_from_iter|_copy_from_iter|_copy_to_iter|copy_user|__memcpy|memcpy_erms|rep_movs/ {s+=$1} END{printf "%.0f", s+0}' "$OUT_DIR/syms.txt")

awk -v c="$copy_strict" -v t="$tot" 'BEGIN{
    if (t<1) { print "B: 无样本（perf record 失败? 权限? 看 warn.log）"; exit }
    p=c/t; z=1.96; den=1+z*z/t
    ctr=(p+z*z/(2*t))/den
    h=z*sqrt(p*(1-p)/t+z*z/(4*t*t))/den
    printf "B: copy=%d/%d  point=%.2f%%  Wilson95CI=[%.2f%%,%.2f%%]\n", c,t,100*p,100*(ctr-h),100*(ctr+h)
}' | tee "$OUT_DIR/B_result.txt"
echo "B2: 内核态样本=${kern_n}/${tot}（符号解析出来的内核行=${kern_named}）" | tee -a "$OUT_DIR/B_result.txt"
echo "B3: 宽口径(内核 copy/memcpy 类)=${copy_broad}/${tot} = $(awk -v c="$copy_broad" -v t="$tot" 'BEGIN{if(t>0)printf "%.2f%%",100*c/t}')" | tee -a "$OUT_DIR/B_result.txt"

echo "--- top20 symbols（按样本数；列=样本数 dso tag sym；copy_page_* 应为 [kernel.kallsyms] [k]）---"
awk '{k=$2" "$3" "$4; s[k]+=$1} END{for(x in s) print s[x], x}' "$OUT_DIR/syms.txt" | sort -rn | head -20
echo "--- 附：s1.data 的 perf report 前 25 行（符号解析是否正常的直接证据） ---"
if [ -f "$OUT_DIR/perf/s1.data" ]; then
    perf report -i "$OUT_DIR/perf/s1.data" --stdio --sort dso,symbol 2>>"$OUT_DIR/perf/warn.log" | sed -n '1,25p' || true
fi

echo "--- /proc/io 硬证据（pause 窗口内经被测进程 read/write 搬运的字节） ---"
awk -F, 'NR>1{dr[$2]+=$3; dw[$2]+=$4; rb[$2]+=$5; wb[$2]+=$6; n[$2]++}
    END{for(p in dr) printf "  pid=%s 事件=%d  ΣΔrchar=%.0fMB ΣΔwchar=%.0fMB ΣΔread_bytes=%.0fMB ΣΔwrite_bytes=%.0fMB\n",
        p,n[p],dr[p]/1048576,dw[p]/1048576,rb[p]/1048576,wb[p]/1048576}' "$OUT_DIR/io_deltas.csv"
echo "  参考：若拷贝走 read/write，则 ΣΔ(rchar+wchar) ≈ $(( REPEAT_B * DIRTY_B * 2 ))MB（${REPEAT_B}事件×${DIRTY_B}MB×读+写）；接近 0 则拷贝不走这些进程的 read/write"

# ---------------- kprobe 兜底（copy 计数为 0 时的页数硬证据） ----------------
if [ "${copy_strict:-0}" -eq 0 ]; then
    echo "WARN: copy_page_* 计数为 0 → 试 kprobe 计数"
    mountpoint -q /sys/kernel/debug   2>/dev/null || mount -t debugfs debugfs /sys/kernel/debug   2>/dev/null || true
    mountpoint -q /sys/kernel/tracing 2>/dev/null || mount -t tracefs tracefs /sys/kernel/tracing 2>/dev/null || true
    kp=""
    for fn in copy_page_to_iter copy_page_from_iter_atomic; do
        perf probe --del "$fn" >/dev/null 2>&1 || true   # 清掉上轮残留，避免 "already exists"
        if perf probe --add "$fn" >>"$OUT_DIR/perf/warn.log" 2>&1; then kp="${kp:+$kp,}probe:$fn"; fi
    done
    if [ -n "$kp" ]; then
        so=$(perf stat -p "$PERF_PIDLIST" -x, -e "$kp,task-clock" -- aenv pause "$SBX" 2>&1 >/dev/null || true)
        aenv resume "$SBX" >/dev/null 2>&1 || true
        echo "  单次 pause(dirty=${DIRTY_B}MB) probe 命中：$(awk -F, '$3 ~ /^probe:/ {printf "%s=%s ", $3, $1}' <<<"$so")"
        echo "  参考：dirty=${DIRTY_B}MB ≈ $(( DIRTY_B * 256 )) 个 4KiB 页，命中数同数量级即证明拷贝真实发生"
    else
        echo "  kprobe 不可用（perf probe 失败；详情见 warn.log —— 容器内核可能禁用 kprobe/debugfs）"
    fi
fi

echo "--- env after ---"; env_snap | tee "$OUT_DIR/env_after.txt"

# ---------------- RESULT 汇总（sed 锚定提取，别把 "95CI"/"Wilson95CI" 里的 95 当数值） ----------------
B_PCT=$(sed -nE 's/.*point=([0-9.]+)%.*/\1/p' "$OUT_DIR/B_result.txt" | head -1)
B_LO=$(sed -nE 's/.*Wilson95CI=\[([0-9.]+)%,([0-9.]+)%\].*/\1/p' "$OUT_DIR/B_result.txt" | head -1)
B_HI=$(sed -nE 's/.*Wilson95CI=\[([0-9.]+)%,([0-9.]+)%\].*/\2/p' "$OUT_DIR/B_result.txt" | head -1)
A_B=$(sed -nE 's/.*transfer=([0-9.]+) ms\/GiB.*/\1/p' "$OUT_DIR/A_result.txt" | head -1)
A_CI=$(sed -nE 's/.*95CI=\+-([0-9.]+).*/\1/p' "$OUT_DIR/A_result.txt" | head -1)

B_VALID=1; B_WHY=""
[ "$tot" -gt 0 ]               || { B_VALID=0; B_WHY="无样本"; }
[ "${kern_n:-0}" -gt 0 ]       || { B_VALID=0; B_WHY="0 个内核态样本（内核采样被禁）"; }
[ "${kern_named:-0}" -gt 0 ]   || { B_VALID=0; B_WHY="内核符号全是未解析地址"; }

echo
echo "==================== RESULT（把这一段贴给我） ===================="
cat "$OUT_DIR/A_result.txt"
cat "$OUT_DIR/B_result.txt"
echo "选定PID=$PERF_PIDLIST  内核态采样自检=$([ "$KERN_OK" = 1 ] && echo OK || echo 受限)  sandbox=$SBX"
nA=$(( REPEAT_A * $(wc -w <<<"$DIRTY_MB_LIST") ))
echo "pause成败：A段=${FAIL_A}/${nA}  B段=${FAIL_B}/${REPEAT_B}（HTTP 500=失败；A 失败点已剔除回归，B 失败样本已隔离）"
[ $(( FAIL_A + FAIL_B )) -gt 0 ] && echo "→ pause 失败多为磁盘满（overlaybd 层文件堆积）→ 清盘（停旧沙箱/删层文件，df -h 确认）后重跑，A/B 才可信"
if [ "$B_VALID" = 1 ]; then
    awk -v pct="$B_PCT" -v lo="$B_LO" -v hi="$B_HI" -v b="$A_B" -v bci="$A_CI" 'BEGIN{
        if (pct==""||b=="") { print "（缺 A 或 B 结果，无法合成）"; exit }
        printf "合成：拷贝段 ≈ %.2f ms/GiB（点）  [%.2f, %.2f]  （B比例 × A斜率%.0f±%.0f ms/GiB）\n", pct/100*b, lo/100*b, hi/100*b, b, bci
        printf "      已识别copy符号样本占比 = %.2f%%  [%.2f%%, %.2f%%]；非DSA端到端收益\n", pct, lo, hi
        print "      跨窗口乘积仅作条件估算；区间只传播B计数，不含A斜率、事件相关与分类不确定性"
    }'
else
    echo "→ B 符号计数无效（${B_WHY}）→ 「拷贝=0」是结构性假象而非结论；请看上方 /proc/io 与 kprobe 证据"
fi
echo "env: before=$(cat "$OUT_DIR/env_before.txt") | after=$(cat "$OUT_DIR/env_after.txt")"
echo "样本数(目标>=20000) = $tot   CI半宽(目标<=2pp) = $(awk -v a="${B_LO:-0}" -v b="${B_HI:-0}" 'BEGIN{printf "%.2f", (b-a)/2}')"
if [ -s "$OUT_DIR/perf/warn.log" ]; then echo "perf 警告(warn.log 前 3 行)："; head -3 "$OUT_DIR/perf/warn.log"; fi
echo "产物：$OUT_DIR/{sweep.csv,syms.txt,io_deltas.csv,A_result.txt,B_result.txt,env_before.txt,env_after.txt,perf/}"
echo "================================================================"
