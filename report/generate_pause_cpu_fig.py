# -*- coding: utf-8 -*-
# 引用边界见CURRENT_STATUS.md；汇总录入数据，原始日志待补；既有PNG为历史快照。
"""
真实 pause 的 CPU 构成与 DSA 介入边界 —— 生成 fig7
数据来源: measure_copy_share.sh 全量 (2026-10-19, 服务器 5.15.196)
  B 段: perf record -F4000 包 60 个成功 pause 事件 (dirty=256MB), 87024 样本, 符号 100% 解析
  A 段: perf stat task-clock 差分 80 点 (dirty=0/64/256/1024MB x 20 轮), 回归 R2=0.708
  pause 成败: A 0/80, B 0/60 (全部成功)
运行: python generate_pause_cpu_fig.py
2026-10-19 更新: dsa_disk_scene 服务器实测已到位 (60MB×15 次, 冷页 LLC 冲刷, both 方向), (c) 面板已填入:
  mem2disk Δ=sw_2step−dsa_2step=43.2 ms/GiB → 推断比 43.2/516 = 8.4%
  disk2mem Δ=36.2 ms/GiB → 推断比 ≈7.0% (对称假设分母 516, 等 MODE=resume 实测修正)
  交叉验证: 参照臂省比例 1−38.7/85.0=54.5% × 真实搬运段 71.5 = 38.9/516 = 7.5% (同量级, 稳健)
  两步重构实测净亏: mem2disk +10.4% / disk2mem +10.7% (新增跳 74~78 > DSA 省回 36~43 ms/GiB)
"""
import os
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

BASE = r"d:\workSpace\DSA-"
FIG_DIR = os.path.join(BASE, "report", "figures")
os.makedirs(FIG_DIR, exist_ok=True)

# ---- MATLAB 风格（与 generate_disk_scene_fig.py 保持一致）----
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

# ================================================================
# 实测数据（2026-10-19 全量）
# ================================================================
TOTAL = 87024

# (a) 符号构成: (类别, 样本数, 颜色) —— 类内符号见 measure_copy_share top20
CATS = [
    ('搬运·读侧\n_copy_to_iter',               10143, '#0072BD'),   # process_vm_readv 跨进程 copy_to_user
    ('搬运·写侧\ncopy_page_from_iter_atomic',   1911, '#4DBEEE'),   # pwrite → 层文件 page cache
    ('page cache\n/XArray',                      9269, '#EDB120'),   # filemap_add_folio+find_lock+xas_store+folios_put_refs
    ('memcg/LRU\n记账',                          7172, '#D95319'),   # try_charge+mod_state+page_counter+lru_gen*+mod_node
    ('页表/页管理',                              4046, '#7E2F8E'),   # follow_page_pte+free_one_page+clear_page+free_unref
    ('锁/RCU',                                   2937, '#77AC30'),   # spin_lock+rcu_read_lock/unlock
    ('用户态 memset\n(libc)',                    1232, '#A2142F'),
    ('其他（长尾）',                            50314, '#B3B3B3'),
]
READ_N, WRITE_N = 10143, 1911
COPY_N = READ_N + WRITE_N                      # 12054

# (b) A 段差分回归
A_TRANSFER, A_CI, A_FIXED, A_R2, A_N = 516.0, 74.0, 108.3, 0.708, 80
DIRTY_MEANS = [(0, 61.7), (64, 128.1), (256, 315.1), (1024, 606.0)]   # (MB, mean ms)

# (c) 搬运段绝对成本（B 占比 × A 斜率）
copy_pct = COPY_N / TOTAL                                   # 13.85%
read_ms  = READ_N  / TOTAL * A_TRANSFER                     # 60.2 ms/GiB
write_ms = WRITE_N / TOTAL * A_TRANSFER                     # 11.3 ms/GiB
copy_ms  = copy_pct * A_TRANSFER                            # 71.5 ms/GiB
COPY_LO, COPY_HI = 0.1364, 0.1407                           # Wilson 95%CI（12054/87024）

# ================================================================
# fig7
# ================================================================
fig, axes = plt.subplots(1, 3, figsize=(20, 9), dpi=150,
                         gridspec_kw={'width_ratios': [1, 1, 1.8]})

# ---------------- (a) 符号构成 ----------------
ax = axes[0]
labels = [c[0] for c in CATS]
vals   = [c[1] / TOTAL * 100 for c in CATS]
colors = [c[2] for c in CATS]
ypos   = np.arange(len(CATS))[::-1]        # 第一个类别在最上
ax.barh(ypos, vals, color=colors, edgecolor='black', linewidth=0.5, height=0.62)
for y, v, n in zip(ypos, vals, [c[1] for c in CATS]):
    ax.text(v + 0.8, y, f'{v:.2f}%\n({n})', va='center', fontsize=8)
# 搬运段合计标注（读+写两跳）
ax.annotate(f'搬运段合计 {copy_pct*100:.2f}%\n= {copy_ms:.1f} ms/GiB',
            xy=(vals[0], ypos[0]), xytext=(30, ypos[0] - 0.05),
            fontsize=9, fontweight='bold', color='#0072BD', va='center',
            arrowprops=dict(arrowstyle='->', color='#0072BD', lw=1.0))
ax.annotate('', xy=(vals[1], ypos[1]), xytext=(30, ypos[0] - 0.05),
            arrowprops=dict(arrowstyle='->', color='#4DBEEE', lw=1.0))
ax.set_yticks(ypos)
ax.set_yticklabels(labels, fontsize=8)
ax.set_xlabel('pause CPU 样本占比 (%)')
ax.set_title('(a) 符号构成（87024 样本）')
ax.set_xlim(0, 72)

# ---------------- (b) A 段差分回归 ----------------
ax = axes[1]
xg = np.array([m for m, _ in DIRTY_MEANS]) / 1024.0
ym = np.array([v for _, v in DIRTY_MEANS])
xx = np.linspace(0, 1.06, 50)
ax.plot(xx, A_FIXED + A_TRANSFER * xx, '-', color='#0072BD', lw=1.6, zorder=2)
ax.plot(xx, A_FIXED + (A_TRANSFER - A_CI) * xx, '--', color='#D95319', lw=1.0, zorder=1)
ax.plot(xx, A_FIXED + (A_TRANSFER + A_CI) * xx, '--', color='#D95319', lw=1.0, zorder=1)
ax.plot(xg, ym, 'o', color='black', markersize=6, zorder=3)
for xi, yi in zip(xg, ym):
    ax.text(xi + 0.015, yi + 14, f'{yi:.0f}', fontsize=8)
ax.text(0.40, 118,
        f'transfer = {A_TRANSFER:.0f} ± {A_CI:.0f} ms/GiB\n'
        f'fixed = {A_FIXED:.1f} ms\nR² = {A_R2:.3f} (n={A_N})',
        fontsize=8.5, bbox=dict(boxstyle='round', facecolor='white', edgecolor='0.5'))
ax.set_xlabel('dirty 数据量 (GiB)')
ax.set_ylabel('pause task-clock (ms)')
ax.set_title('(b) A 段差分回归（分母锚点）')
ax.set_xlim(-0.04, 1.10)
ax.set_ylim(0, 700)

# ---------------- (c) DSA 介入边界与推断比（dsa_disk_scene 实测已填入） ----------------
ax = axes[2]
# dsa_disk_scene 2026-10-19 服务器实测（60MB×15 次, 冷页 LLC 冲刷, idle=UMWAIT 主口径）
SCN_SW1  = 349.9    # disk2mem sw_1step_read_direct（现状: 内核单跳直达）
SCN_SW2  = 423.6    # disk2mem sw_2step_read_stg_memcpy（+staging 跳, 软件 memcpy）
SCN_DSA2 = 387.4    # disk2mem dsa_2step_read_stg_dsa_idle（DSA 替 staging 跳）
DELTA_M2D = 43.2    # mem2disk: sw_2step − dsa_2step = 408.7 − 365.5
DELTA_D2M = SCN_SW2 - SCN_DSA2   # 36.2
ideal_m2d = A_TRANSFER - DELTA_M2D     # 472.8: 若搬运跳可 DSA 化的理想上限

bx = [0.0, 0.85, 2.30, 3.15, 4.00]
bw = 0.68
# 条1: mem2disk 现状（真实全链路拆解）
bot = 0
for v, lab, col in [(read_ms, f'读跳 {read_ms:.1f}', '#0072BD'),
                    (write_ms, f'写跳 {write_ms:.1f}', '#4DBEEE'),
                    (A_TRANSFER - copy_ms, f'其余 {A_TRANSFER-copy_ms:.1f}', '#EDB120')]:
    ax.bar(bx[0], v, bw, bottom=bot, color=col, edgecolor='black', linewidth=0.5)
    if v > 40:
        ax.text(bx[0], bot + v / 2, lab, ha='center', va='center', fontsize=7.5)
    bot += v
ax.text(bx[0], bot + 10, f'CPU斜率 {A_TRANSFER:.0f}\n占比×斜率估算\n读{read_ms:.1f}+写{write_ms:.1f}',
        ha='center', fontsize=7.5, fontweight='bold')
# 条2: mem2disk 理想上限（若搬运跳可 DSA 化, 按模拟臂 Δ 缩短）
bot = 0
for v, lab, col in [(copy_ms - DELTA_M2D, f'DSA后 {copy_ms-DELTA_M2D:.1f}', '#77AC30'),
                    (A_TRANSFER - copy_ms, '', '#EDB120')]:
    ax.bar(bx[1], v, bw, bottom=bot, color=col, edgecolor='black', linewidth=0.5)
    if lab:
        ax.text(bx[1], bot + v / 2, lab, ha='center', va='center', fontsize=7.5)
    bot += v
ax.text(bx[1], bot + 10, f'若搬运跳可DSA化\n{ideal_m2d:.1f} (−{DELTA_M2D:.1f})\n推断比 −{DELTA_M2D/A_TRANSFER*100:.1f}%',
        ha='center', fontsize=7.5, fontweight='bold', color='#77AC30')
# 条3-5: disk2mem 模拟臂（两步重构净亏判据）
for x, v, col, lab in [(bx[2], SCN_SW1, '#B3B3B3', f'现状 1step\n{SCN_SW1:.1f}'),
                       (bx[3], SCN_SW2, '#808080', f'+staging跳·软件\n{SCN_SW2:.1f} (+{SCN_SW2-SCN_SW1:.1f})'),
                       (bx[4], SCN_DSA2, '#77AC30', f'+staging跳·DSA\n{SCN_DSA2:.1f}')]:
    ax.bar(x, v, bw, color=col, edgecolor='black', linewidth=0.5)
    ax.text(x, v + 10, lab, ha='center', fontsize=7.5)
ax.annotate('', xy=(bx[4], 505), xytext=(bx[2], 505),
            arrowprops=dict(arrowstyle='->', color='#A2142F', lw=1.2))
ax.text((bx[2] + bx[4]) / 2, 518, f'vs 1step +{SCN_DSA2-SCN_SW1:.1f} (+{(SCN_DSA2-SCN_SW1)/SCN_SW1*100:.1f}%) 净亏',
        ha='center', fontsize=8, color='#A2142F', fontweight='bold')
ax.text((bx[4]), SCN_DSA2 + 52, f'−{DELTA_D2M:.1f} vs 软件2step', ha='center', fontsize=7, color='#77AC30')
# 组标题（充当 x 轴标签）
ax.text((bx[0] + bx[1]) / 2, 640, 'pause斜率与条件合成', ha='center', fontsize=8.5, fontweight='bold')
ax.text((bx[2] + bx[4]) / 2, 640, 'disk2mem · 模拟臂实测', ha='center', fontsize=8.5, fontweight='bold')
ax.set_xlim(-0.65, 4.65)
ax.set_ylim(0, 660)
ax.set_xticks([])
ax.set_ylabel('每 GiB CPU 时间 (ms)')
ax.set_title('(c) DSA 介入边界与推断比')
fig.suptitle('图7  真实 pause 的 CPU 构成与 DSA 介入边界（真实路径观测 + 微基准实测 / 条件性推算）', fontsize=14)
fig.tight_layout(rect=[0, 0.36, 1, 0.94])
notes = [
    ('测量方法｜measure_copy_share.sh', '#0072BD', True),
    ('同一沙箱循环 resume → guest 内 /dev/shm 制脏 → 等待1秒 → aenv pause；制脏与 resume 均在采样窗外。', 'black', False),
    ('A：0/64/256/1024MiB ×20轮，按轮交错档位；perf stat -p PID 记录 pause 命令期间的 task-clock，回归分离固定项与斜率。', 'black', False),
    ('B：256MiB ×60个成功 pause；perf record -F4000 单独测符号构成，共87024样本；不把高频采样轮用于 A 的 CPU 回归。', 'black', False),
    ('可信控制：探针 pause 的 CPU 增量筛选活跃 PID；检查内核采样 / 符号解析；隔离 HTTP500 失败事件；记录前后负载及共存 VM 数。', 'black', False),
    ('边界：透明观测不改 AgentENV 源码和搬运逻辑，但 perf 有开销，共享进程可能含其他请求；上述控制不等于零扰动或完全隔离。', '#A2142F', False),
    ('收益口径：占比×斜率是跨窗口合成，非独立copy实测；7%～8%非严格上限，resume分母待测；约+10%为60MiB微基准净增。', '#A2142F', False),
]
for i, (note, color, bold) in enumerate(notes):
    fig.text(0.035, 0.295 - i * 0.039, note, fontsize=10, color=color,
             fontweight='bold' if bold else 'normal', va='top')
fig.savefig(os.path.join(FIG_DIR, 'fig7_pause构成.png'), bbox_inches='tight')
plt.close(fig)
print('fig done:', os.path.join(FIG_DIR, 'fig7_pause构成.png'))
print('OK')
