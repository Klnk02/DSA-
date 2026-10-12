#!/usr/bin/env bash
#
# setup_dsa.sh — 启用并配置 Intel DSA 设备与工作队列
#
# 背景：目标机有 2 颗 Intel DSA（dsa0@NUMA0、dsa2@NUMA1），
#       本脚本用 accel-config 将 DSA 启用，并为每颗 DSA 配置：
#         - wq{idx}.0：dedicated 用户队列（单进程独占 engine{idx}.0，测极限吞吐/延迟）
#         - wq{idx}.1：shared   用户队列（ENQCMD/SVA 共享 engine{idx}.1，测多客户端并发）
#       其中 {idx} 是设备编号（dsa0 -> 0，dsa2 -> 2），engine/wq 命名随设备编号。
#
# 前置条件（踩过的坑）：
#   1. 内核启动参数必须去掉 iommu=pt，否则 PASID 挂载失败，user 队列无法启用。
#      正确：intel_iommu=on,sm_on   （不要带 iommu=pt）
#   2. user 队列必须显式 --driver-name=user，否则 enable-wq 报
#      IDXD_SCMD_WQ_NO_DRV_NAME (0x80200000)。注意 driver_name ≠ name，是不同字段。
#   3. shared 队列必须显式 --threshold（>0 且 <= wq-size-1），否则报
#      threshold not set (0x80060000)。dedicated 不需要 threshold。
#
# 用法（需 root）：
#   sudo bash setup_dsa.sh
#
# 回滚（彻底停用 DSA）：
#   sudo accel-config disable-wq     dsa0/wq0.0
#   sudo accel-config disable-wq     dsa0/wq0.1
#   sudo accel-config disable-wq     dsa2/wq2.0
#   sudo accel-config disable-wq     dsa2/wq2.1
#   sudo accel-config disable-device dsa0
#   sudo accel-config disable-device dsa2

set -euo pipefail

ACCEL=accel-config

if ! command -v "$ACCEL" >/dev/null 2>&1; then
  echo "错误：未找到 accel-config，请先安装：sudo apt install accel-config" >&2
  exit 1
fi

if [ "$(id -u)" -ne 0 ]; then
  echo "错误：需要 root 权限，请用 sudo 运行" >&2
  exit 1
fi

# 动态发现所有 DSA 设备（dsa0/dsa2 ...，用 dsa[0-9]* 排除 enable 后多出的 dsa!wqX.Y 条目）
shopt -s nullglob
devices=(/sys/bus/dsa/devices/dsa[0-9]*)
if [ "${#devices[@]}" -eq 0 ]; then
  echo "错误：未发现任何 dsa 设备" >&2
  exit 1
fi

echo "发现 DSA 设备："
for d in "${devices[@]}"; do echo "  - $(basename "$d")"; done
echo

for d in "${devices[@]}"; do
  name=$(basename "$d")
  idx=${name#dsa}   # 设备编号：dsa0 -> 0，dsa2 -> 2
  echo "========== 配置 $name =========="

  # 若设备已启用，先整体禁用（含队列），保证脚本可重复执行
  if [ "$(cat "$d/state" 2>/dev/null)" = "enabled" ]; then
    for w in "$d"/wq*; do
      [ -d "$w" ] && "$ACCEL" disable-wq "$name/$(basename "$w")" 2>/dev/null || true
    done
    "$ACCEL" disable-device "$name" 2>/dev/null || true
    echo "  已重置设备"
  fi

  # 1) dedicated 用户队列：engine${idx}.0 -> group0 <- wq${idx}.0
  "$ACCEL" config-engine "$name/engine${idx}.0" --group-id=0
  "$ACCEL" config-wq "$name/wq${idx}.0" \
    --group-id=0 --type=user --mode=dedicated \
    --wq-size=32 --priority=10 --max-batch-size=1024 --max-transfer-size=2097152 \
    --driver-name=user --name="dsa${idx}_dedicated"
  echo "  已配置 dedicated 队列 wq${idx}.0（绑定 engine${idx}.0）"

  # 2) shared 用户队列：engine${idx}.1 -> group1 <- wq${idx}.1（ENQCMD/SVA 并发）
  #    注：shared 不支持 batch（不设 max-batch-size），但必须设 threshold（>0 且 <= wq-size-1）
  "$ACCEL" config-engine "$name/engine${idx}.1" --group-id=1
  "$ACCEL" config-wq "$name/wq${idx}.1" \
    --group-id=1 --type=user --mode=shared \
    --wq-size=64 --threshold=63 --priority=10 --max-transfer-size=2097152 \
    --driver-name=user --name="dsa${idx}_shared"
  echo "  已配置 shared 队列 wq${idx}.1（绑定 engine${idx}.1）"

  # 3) 启用设备与队列
  "$ACCEL" enable-device "$name"
  "$ACCEL" enable-wq "$name/wq${idx}.0"
  "$ACCEL" enable-wq "$name/wq${idx}.1"
  echo "  已启用 $name 及其两个队列"
  echo
done

echo "========== 配置结果（accel-config list）=========="
"$ACCEL" list || true
echo
echo "========== /dev/dsa 设备节点 =========="
ls -l /dev/dsa 2>/dev/null || echo "仍未生成 /dev/dsa，请检查上面 accel-config list 输出"