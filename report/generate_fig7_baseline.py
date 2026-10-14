# -*- coding: utf-8 -*-
"""
fig7: A 臂基线 —— 真实 AgentENV snapshot pause 的 wall 与 server CPU 随 dirty 量变化
数据来源: results/stage4-sandbox/baseline_sweep.csv（2026-10-14, repeat=20,
          剔除 perf record rep1/8/15（wall +2.2s）与 64MB rep4（漏 dirty））
运行: python generate_fig7_baseline.py
"""
import os
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

BASE = r"d:\workSpace\DSA-"
FIG_DIR = os.path.join(BASE, "report", "figures")

MATLAB_COLORS = ['#0072BD', '#D95319', '#EDB120', '#7E2F8E', '#77AC30', '#4DBEEE', '#A2142F']
plt.rcParams.update({
    'font.family': 'sans-serif',
    'font.sans-serif': ['Microsoft YaHei', 'SimHei', 'Arial'],
    'axes.unicode_minus': False,
    'figure.facecolor': 'white', 'axes.facecolor': 'white',
    'axes.edgecolor': 'black', 'axes.linewidth': 0.6,
    'axes.grid': True, 'grid.color': '#D9D9D9', 'grid.linewidth': 0.5, 'grid.linestyle': '-',
    'axes.prop_cycle': plt.cycler(color=MATLAB_COLORS),
    'xtick.direction': 'in', 'ytick.direction': 'in',
    'xtick.major.size': 3.5, 'ytick.major.size': 3.5,
    'font.size': 10, 'axes.titlesize': 11, 'axes.labelsize': 10,
    'legend.frameon': True, 'legend.framealpha': 1.0, 'legend.edgecolor': '0.5',
    'legend.facecolor': 'white', 'legend.fancybox': False, 'legend.fontsize': 9,
    'lines.linewidth': 1.6, 'lines.markersize': 5,
})

x = np.arange(3)
labels = ['0 MB', '64 MB', '256 MB']
wall    = [169.6, 1170.8, 1163.6]
wall_sd = [4.0, 12.0, 20.0]
cpu     = [0.0, 22.4, 168.5]
cpu_sd  = [0.0, 8.3, 22.0]
CTRL = '#4DBEEE'   # 0MB 对照组（浅蓝）
MAIN = '#0072BD'   # 主色
DSAC = '#77AC30'   # DSA 靶子（绿）

fig, axes = plt.subplots(1, 2, figsize=(10.0, 4.4), dpi=150)

# ---- (a) pause wall ----
ax = axes[0]
ax.bar(x, wall, yerr=wall_sd, color=[CTRL, MAIN, MAIN], width=0.62,
       edgecolor='black', linewidth=0.5, capsize=3, error_kw=dict(lw=1.0))
for xi, v in zip(x, wall):
    ax.text(xi, v + 30, f'{v:.0f}', ha='center', va='bottom', fontsize=9)
ax.axhline(1170, color='#A2142F', linestyle='--', linewidth=1.1)
ax.annotate('固定等待 ~1s（不占 vCPU）\n64 与 256MB 相同 → 停顿与数据量无关',
            xy=(1.5, 1175), xytext=(0.25, 1400),
            arrowprops=dict(arrowstyle='->', color='black', lw=0.9), fontsize=8.5, ha='left')
ax.annotate('0MB 地板 170ms（固定 CPU ≈ 0）', xy=(0, 178), xytext=(0.30, 520),
            arrowprops=dict(arrowstyle='->', color='black', lw=0.9), fontsize=8.5)
ax.set_xticks(x)
ax.set_xticklabels(labels)
ax.set_ylabel('pause wall (ms)')
ax.set_title('(a) pause 停顿时长：被固定等待锁死')
ax.set_ylim(0, 1650)

# ---- (b) server CPU ----
ax = axes[1]
ax.bar(x, cpu, yerr=cpu_sd, color=[CTRL, DSAC, DSAC], width=0.62,
       edgecolor='black', linewidth=0.5, capsize=3, error_kw=dict(lw=1.0))
for xi, v in zip(x, cpu):
    ax.text(xi, v + 6, f'{v:.1f}', ha='center', va='bottom', fontsize=9)
ax.text(0.02, 0.96, '（4.2 另一沙箱 942 ms/GB，同量级互证）',
        transform=ax.transAxes, fontsize=8, color='#5B6675', va='top')
ax.annotate('折算 ≈ 674 ms/GB\n（DSA 的靶子）', xy=(2, 173), xytext=(0.95, 205),
            arrowprops=dict(arrowstyle='->', color='black', lw=0.9), fontsize=8.5)
ax.annotate('64MB：22.4 ms\n（噪声大 4.7~30）', xy=(1, 26), xytext=(0.55, 95),
            arrowprops=dict(arrowstyle='->', color='black', lw=0.9), fontsize=8.5)
ax.annotate('0MB：CPU ≈ 0', xy=(0.05, 3), xytext=(0.18, 45),
            arrowprops=dict(arrowstyle='->', color='black', lw=0.9), fontsize=8.5)
ax.set_xticks(x)
ax.set_xticklabels(labels)
ax.set_ylabel('server CPU (ms, task-clock)')
ax.set_title('(b) server CPU：随 dirty 线性增长')
ax.set_ylim(0, 235)

fig.suptitle('图7  真实 AgentENV snapshot pause · A 臂基线（repeat=20，已剔 perf record 行）', fontsize=11)
fig.tight_layout(rect=[0, 0, 1, 0.94])
out = os.path.join(FIG_DIR, 'fig7_A臂基线.png')
fig.savefig(out, bbox_inches='tight')
plt.close(fig)
print('fig done:', out)
