#!/bin/bash
# =============================================================================
# 阶段 4.5 · A2 —— 全机 CPU 口径标定：把 copy 占比从「进程口径上界」修正为「系统口径」
# 仓库：DSA-/calibrate_cpu_scope.sh
#
# 背景（4.4 / 评审反馈）：
#   既有 CPU 口径（CLOCK_PROCESS_CPUTIME_ID / perf -p server task-clock）只含被测
#   进程；写盘侧的 kworker 回写与 jbd2 日志线程没算 → copy 占比（26~32%）是上界。
#   本脚本用 /proc/stat 差分测全机 CPU，与进程侧对比，残差 ≈ 系统侧开销（推测级）。
#
# 用法：
#   sudo ./calibrate_cpu_scope.sh scene -- <命令及参数>   # 包住任意既有二进制（如 ./dsa_disk_scene）
#   sudo ./calibrate_cpu_scope.sh pause <sandbox_id>      # 包住一次 aenv pause
#   环境变量：
#     SERVER_PID=<pid>  覆盖 orchestrator 进程（默认 pgrep -x server 取第一个）
#     PERFREC=1         pause 模式附带 perf record -a -g（A3：看 1s 窗口全机符号）
#     PERFSEC=20        PERFREC 采样时长（秒，默认 20，需 ≥ pause wall）
#
# 产出（stdout）：
#   进程 CPU vs 全机 CPU（ms）+ 残差；scene 模式给系统口径 copy 占比换算提示。
#   PERFREC=1 另出 /tmp/calib.perf.data：
#     perf report -i /tmp/calib.perf.data --stdio --sort=symbol | head -40
#     （A3 重点：1s 窗口内 jbd2 / writeback / fsync 相关符号是否活跃）
#   建议每模式跑 3 次取中位；共享测试机上残差含其他进程噪声，选安静时段。
# =============================================================================
set -euo pipefail

TCK=$(getconf CLK_TCK)
MS_PER_JIFFY=$((1000 / TCK))

machine_busy_jiffies() {
  # /proc/stat cpu 行：user nice system idle iowait irq softirq steal ...
  # busy = user+nice+system+irq+softirq+steal（排除 idle 与 iowait）
  awk '/^cpu /{print $2+$3+$4+$7+$8+$9}' /proc/stat
}

proc_cpu_jiffies() {  # $1=pid → utime+stime（/proc/<pid>/stat 字段 14+15；comm 可能含空格，先剥掉 "pid (comm) " 前缀）
  awk -v pid="$1" '{
    s = $0
    sub(/^[0-9]+ \([^)]*\) /, "", s)
    split(s, a, " ")
    print a[12] + a[13]
  }' /proc/$1/stat
}

now_ms() { date +%s%N | awk '{printf "%.0f", $1/1e6}'; }

# ---------------------------------------------------------------- scene 模式
if [ "${1:-}" = "scene" ]; then
  shift
  [ "${1:-}" = "--" ] && shift
  [ $# -ge 1 ] || { echo "用法: $0 scene -- <命令>"; exit 1; }
  command -v perf >/dev/null || { echo "需要 perf"; exit 1; }

  B_busy=$(machine_busy_jiffies)
  perf stat -x, -e task-clock -o /tmp/calib.perf.stat -- "${@}" >/dev/null 2>&1
  A_busy=$(machine_busy_jiffies)

  taskclk_ms=$(awk -F, '/task-clock/{printf "%.1f", $1}' /tmp/calib.perf.stat)
  machine_ms=$(( (A_busy - B_busy) * MS_PER_JIFFY ))
  residual=$(awk -v m="$machine_ms" -v p="$taskclk_ms" 'BEGIN{printf "%.1f", m - p}')

  echo "──── scene 标定 ────"
  echo "命令: $*"
  echo "进程 CPU (perf task-clock)      : ${taskclk_ms} ms"
  echo "全机 CPU (/proc/stat 差分)      : ${machine_ms} ms"
  echo "残差 (≈ kworker 回写+jbd2+irq, 推测): ${residual} ms"
  echo "提示: 按命令实际搬运 GB 数把残差折成 ms/GB 后，"
  echo "      mem2disk 系统口径 copy 占比 = 56.0 / (56.0 + 158.9 + 残差_ms_per_GB)"

# ---------------------------------------------------------------- pause 模式
elif [ "${1:-}" = "pause" ]; then
  SB="${2:?用法: $0 pause <sandbox_id>}"
  command -v aenv >/dev/null || { echo "需要 aenv CLI"; exit 1; }
  SRV="${SERVER_PID:-$(pgrep -x server | head -n1 || true)}"
  [ -n "$SRV" ] || { echo "找不到 orchestrator 进程（pgrep -x server），或用 SERVER_PID 指定"; exit 1; }
  echo "orchestrator pid = $SRV   sandbox = $SB"

  PERF_PID=""
  if [ "${PERFREC:-0}" = "1" ]; then
    command -v perf >/dev/null || { echo "PERFREC=1 需要 perf"; exit 1; }
    perf record -a -g -o /tmp/calib.perf.data -- sleep "${PERFSEC:-20}" >/dev/null 2>&1 &
    PERF_PID=$!
    sleep 1   # 等 perf 挂稳
  fi

  B_busy=$(machine_busy_jiffies)
  B_proc=$(proc_cpu_jiffies "$SRV")
  T0=$(now_ms)
  aenv pause "$SB"
  T1=$(now_ms)
  A_busy=$(machine_busy_jiffies)
  A_proc=$(proc_cpu_jiffies "$SRV")

  proc_ms=$(( (A_proc - B_proc) * MS_PER_JIFFY ))
  machine_ms=$(( (A_busy - B_busy) * MS_PER_JIFFY ))
  residual=$(awk -v m="$machine_ms" -v p="$proc_ms" 'BEGIN{printf "%.1f", m - p}')

  echo "──── pause 标定 ────"
  echo "pause wall                       : $((T1 - T0)) ms"
  echo "server 进程 CPU (utime+stime)    : ${proc_ms} ms"
  echo "全机 CPU (/proc/stat 差分)       : ${machine_ms} ms"
  echo "残差 (≈ kworker+jbd2+irq+其他进程, 推测): ${residual} ms"

  if [ -n "$PERF_PID" ]; then
    wait "$PERF_PID" || true
    echo "perf 数据: /tmp/calib.perf.data"
    echo "查看: perf report -i /tmp/calib.perf.data --stdio --sort=symbol | head -40"
    echo "（A3：重点看 1s 窗口内 jbd2 / writeback / fsync 相关符号是否活跃）"
  fi

else
  sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
  exit 1
fi
