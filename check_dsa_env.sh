#!/usr/bin/env bash
#
# check_dsa_env.sh — Intel DSA 环境探测脚本
#
# 用途：
#   在目标 Linux 机器上运行，收集 Intel DSA / IAA 的硬件 / 驱动 / 工具链 / 系统基线信息，
#   为后续「DSA 基准测试」和「沙箱(快照/恢复)集成测试」提供环境依据。
#
# 用法：
#   bash check_dsa_env.sh            # 输出到终端
#   bash check_dsa_env.sh > dsa_env.txt 2>&1   # 重定向保存，便于发回分析
#
# 依赖：
#   仅使用标准命令 + 读取 /sys 文件系统，无第三方依赖；缺失的命令会自动跳过并告警。

set -u

# ---------------------------------------------------------------------------
# 输出辅助函数
# ---------------------------------------------------------------------------
section() { printf '\n========== %s ==========\n' "$1"; }
ok()   { printf '[ OK ]  %s\n' "$1"; }
warn() { printf '[WARN]  %s\n' "$1"; }
err()  { printf '[FAIL]  %s\n' "$1"; }

has_cmd() { command -v "$1" >/dev/null 2>&1; }

# 读取 sysfs 文件：存在可读则返回内容，否则返回空
sysval() { local f="$1"; [ -r "$f" ] && cat "$f" 2>/dev/null; }

# ---------------------------------------------------------------------------
# 1. 系统信息
# ---------------------------------------------------------------------------
section "1. 系统信息"
printf '内核版本: %s\n' "$(uname -r)"
printf '架构:     %s\n' "$(uname -m)"
printf '主机名:   %s\n' "$(hostname 2>/dev/null)"
if [ -r /etc/os-release ]; then
  . /etc/os-release
  printf '发行版:   %s %s\n' "${PRETTY_NAME:-?}" "${VERSION:-}"
fi
ok "系统信息已采集"

# ---------------------------------------------------------------------------
# 2. CPU 与加速器硬件
# ---------------------------------------------------------------------------
section "2. CPU 与加速器硬件"
if has_cmd lscpu; then
  lscpu | grep -E '^(Model name|CPU\(s\)|NUMA node|Thread|Core)' || warn "lscpu 无相关字段"
else
  warn "未找到 lscpu"
fi

printf '\n—— DSA / IAA / 加速器 PCI 设备 ——\n'
if has_cmd lspci; then
  # 0b25 = Sapphire Rapids DSA；0cfe = Sapphire Rapids IAA；0x1200 = Processing accelerators 类
  lspci -nn 2>/dev/null | grep -iE 'DSA|Data Streaming|0b25|0cfe|1200:|QuickAssist|QAT' \
    || warn "lspci 未发现明确的 DSA/IAA/QAT 设备"
else
  warn "未找到 lspci（需要 pciutils 包）"
fi

printf '\n—— CPU 指令集(是否支持 enqcmd/movdir64b) ——\n'
# Intel DSA 提交描述符需要 ENQCMD/ENQCMDS(SVA 可扩展模式)或 MOVDIR64B
grep -o 'enqcmd\|movdir64b' /proc/cpuinfo 2>/dev/null | sort -u | sed 's/^/  支持: /' \
  || warn "未在 flags 中发现 enqcmd/movdir64b（可能被 grep 省略）"
grep -m1 '^flags' /proc/cpuinfo | grep -qw 'enqcmd' && ok "CPU 支持 enqcmd" \
  || warn "CPU 未列出 enqcmd（SVA 共享工作队列可能不可用，仅 dedicated 模式可用）"

# ---------------------------------------------------------------------------
# 3. idxd 驱动状态
# ---------------------------------------------------------------------------
section "3. idxd 驱动状态"
if [ -d /sys/bus/dsa ]; then
  ok "存在 /sys/bus/dsa（内核已识别 DSA 总线）"
  if [ -d /sys/bus/dsa/drivers/idxd ]; then
    ok "idxd 驱动已注册：$(ls /sys/bus/dsa/drivers/idxd 2>/dev/null | tr '\n' ' ')"
  else
    warn "idxd 驱动未注册"
  fi
else
  err "不存在 /sys/bus/dsa，内核可能未配置/未加载 Intel DSA"
fi

if has_cmd lsmod; then
  printf '\n—— 已加载的 idxd 相关模块 ——\n'
  lsmod 2>/dev/null | grep -E '^idxd|^iommu' || warn "lsmod 中未见 idxd/iommu 模块"
else
  warn "未找到 lsmod"
fi

# ---------------------------------------------------------------------------
# 4. DSA / IAA 设备与工作队列
# ---------------------------------------------------------------------------
section "4. DSA / IAA 设备与工作队列"
found_dev=0
for dev in /sys/bus/dsa/devices/dsa* /sys/bus/dsa/devices/iax*; do
  [ -d "$dev" ] || continue
  found_dev=1
  name=$(basename "$dev")
  printf '\n[设备] %s\n' "$name"
  printf '  名称:         %s\n' "$(sysval "$dev/device_name")"
  printf '  状态:         %s\n' "$(sysval "$dev/state")"
  printf '  引擎数上限:   %s\n' "$(sysval "$dev/max_engines")"
  printf '  工作队列上限: %s\n' "$(sysval "$dev/max_work_queues")"
  printf '  NUMA 节点:    %s\n' "$(sysval "$dev/numa_node")"
  printf '  批量上限:     %s\n' "$(sysval "$dev/max_batch_size")"

  for wq in "$dev"/wq*; do
    [ -d "$wq" ] || continue
    printf '  [WQ] %-10s type=%-9s mode=%-9s size=%-8s state=%s\n' \
      "$(basename "$wq")" \
      "$(sysval "$wq/type")" \
      "$(sysval "$wq/mode")" \
      "$(sysval "$wq/size")" \
      "$(sysval "$wq/state")"
  done
done

if [ "$found_dev" -eq 0 ]; then
  err "未发现任何 dsaN/iaxN 设备（需先通过 accel-config 或内核引导参数启用）"
fi

printf '\n—— /dev/dsa 设备节点（用户态访问入口） ——\n'
if [ -d /dev/dsa ]; then
  ls -l /dev/dsa 2>/dev/null | sed 's/^/  /'
else
  warn "不存在 /dev/dsa（没有可用的 user 类型工作队列，或未映射设备节点）"
fi

# ---------------------------------------------------------------------------
# 5. 用户态工具与库
# ---------------------------------------------------------------------------
section "5. 用户态工具与库"
if has_cmd accel-config; then
  ok "accel-config 已安装"
  accel-config --version 2>/dev/null | sed 's/^/  版本: /' || accel-config -V 2>/dev/null
  printf '\n—— accel-config list（设备/队列配置总览） ——\n'
  accel-config list 2>&1 | sed 's/^/  /' || warn "accel-config list 执行失败"
else
  warn "未找到 accel-config（Intel DSA 配置工具）"
fi

printf '\n—— DML (Data Mover Library) ——\n'
if has_cmd ldconfig; then
  ldconfig -p 2>/dev/null | grep -i 'libdml' | sed 's/^/  /' || warn "未找到 libdml 动态库"
else
  warn "未找到 ldconfig"
fi
[ -d /usr/include/dml ] && ok "存在 DML 头文件目录 /usr/include/dml" \
  || warn "未找到 /usr/include/dml（如需 C 基准需安装 Intel DML dev 包）"

if has_cmd pkg-config; then
  pkg-config --modversion dml 2>/dev/null | sed 's/^/  DML pkg-config 版本: /' || true
fi

# ---------------------------------------------------------------------------
# 6. IOMMU / 内核引导参数
# ---------------------------------------------------------------------------
section "6. IOMMU / 内核引导参数"
printf 'cmdline: %s\n' "$(cat /proc/cmdline 2>/dev/null)"
if cat /proc/cmdline 2>/dev/null | grep -q 'intel_iommu=on'; then
  ok "已开启 intel_iommu=on"
else
  warn "cmdline 未显式包含 intel_iommu=on（DSA SVA/共享模式通常依赖 IOMMU 开启）"
fi

if [ -r "/boot/config-$(uname -r)" ]; then
  printf '\n—— 内核 DSA 相关 CONFIG ——\n'
  grep -E 'CONFIG_INTEL_IDXD|CONFIG_DMAR|CONFIG_INTEL_IOMMU' "/boot/config-$(uname -r)" | sed 's/^/  /' \
    || warn "内核 config 中未找到 DSA 相关选项"
else
  warn "未找到 /boot/config-$(uname -r)，无法检查内核编译选项"
fi

# ---------------------------------------------------------------------------
# 7. 大页 / NUMA（沙箱相关基线）
# ---------------------------------------------------------------------------
section "7. 大页 & NUMA（沙箱相关基线）"
grep -E 'HugePages_Total|HugePages_Free|Hugepagesize' /proc/meminfo 2>/dev/null | sed 's/^/  /'

if has_cmd numactl; then
  printf '\n—— NUMA 拓扑 ——\n'
  numactl --hardware 2>/dev/null | sed 's/^/  /' || warn "numactl --hardware 失败"
else
  printf '\n—— NUMA 节点 (sysfs) ——\n'
  ls -d /sys/devices/system/node/node* 2>/dev/null | sed 's/^/  /'
fi

# ---------------------------------------------------------------------------
# 汇总结论
# ---------------------------------------------------------------------------
section "汇总结论"
ls /sys/bus/dsa/devices/ 2>/dev/null | grep -q '^dsa' && ok "DSA 设备存在" || err "无 DSA 设备"
[ -d /sys/bus/dsa/drivers/idxd ] && ok "idxd 驱动就绪" || err "idxd 驱动缺失"
[ -d /dev/dsa ] && ok "存在 /dev/dsa 设备节点" || warn "无 /dev/dsa 设备节点"
has_cmd accel-config && ok "accel-config 可用" || warn "accel-config 缺失"
cat /proc/cmdline 2>/dev/null | grep -q 'intel_iommu=on' && ok "IOMMU 已开启" || warn "IOMMU 状态待确认"

echo
echo "请把本脚本输出内容发回，用于确定后续 DSA 基准测试方案。"