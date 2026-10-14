# -*- coding: utf-8 -*-
"""
生成 Intel DSA 沙箱加速能力评估 —— MATLAB 风格数据图 + 实验数据 Excel
数据来源: DSA-/results/*.csv（结合 README 结论）
运行: python generate_matlab_plots.py
"""
import os
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np
from openpyxl import Workbook
from openpyxl.styles import Font, PatternFill, Alignment, Border, Side
from openpyxl.utils import get_column_letter

# ---------- 路径 ----------
BASE = r"d:\workSpace\DSA-"
FIG_DIR = os.path.join(BASE, "report", "figures")
XLSX_PATH = os.path.join(BASE, "report", "DSA沙箱加速_实验数据汇总.xlsx")
os.makedirs(FIG_DIR, exist_ok=True)

# ---------- MATLAB 风格 ----------
MATLAB_COLORS = ['#0072BD', '#D95319', '#EDB120', '#7E2F8E', '#77AC30', '#4DBEEE', '#A2142F']
plt.rcParams.update({
    'font.family': 'sans-serif',
    'font.sans-serif': ['Microsoft YaHei', 'SimHei', 'Arial'],
    'axes.unicode_minus': False,
    'figure.facecolor': 'white',
    'axes.facecolor': 'white',
    'axes.edgecolor': 'black',
    'axes.linewidth': 0.6,
    'axes.grid': True,
    'grid.color': '#D9D9D9',
    'grid.linewidth': 0.5,
    'grid.linestyle': '-',
    'axes.prop_cycle': plt.cycler(color=MATLAB_COLORS),
    'xtick.direction': 'in',
    'ytick.direction': 'in',
    'xtick.major.size': 3.5,
    'ytick.major.size': 3.5,
    'font.size': 10,
    'axes.titlesize': 11,
    'axes.labelsize': 10,
    'legend.frameon': True,
    'legend.framealpha': 1.0,
    'legend.edgecolor': '0.5',
    'legend.facecolor': 'white',
    'legend.fancybox': False,
    'legend.fontsize': 9,
    'lines.linewidth': 1.6,
    'lines.markersize': 5,
})

# ================================================================
# 数据（取自 results/*.csv）
# ================================================================
# 阶段2: memcpy / fill 吞吐 (size 单位 KiB)
size_kib = [4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048]
cpu_memcpy = [63.4016, 89.836, 97.1731, 35.9021, 36.1499, 35.5252, 35.8838, 35.9855, 26.8298, 16.3869]
dsa_memcpy = [0.789, 1.62372, 3.45401, 5.858, 9.94215, 14.9858, 19.7283, 23.4809, 26.1877, 27.3602]
cpu_fill = [103.615, 131.733, 151.454, 161.968, 44.3664, 44.9981, 45.1229, 45.1795, 45.2164, 39.5026]
dsa_fill = [1.04233, 2.03781, 3.42862, 5.56409, 9.93574, 14.8946, 19.8525, 23.7964, 26.2039, 27.8519]

# 阶段2: 同步卸载 (吞吐 / 占用率 / 每GB CPU时间 s)
sync_path = ['CPU memcpy', 'DSA 同步 busy-poll']
sync_tp = [14.38, 27.66]
sync_occ = [99.95, 99.91]
sync_cpu_s = [0.0663, 0.0345]

# 阶段3: 异步卸载
async_mode = ['CPU memcpy', 'DSA 同步 busy-poll', 'DSA 异步 + sleep', 'DSA 异步 + umwait']
async_occ = [99.72, 99.97, 8.19, 99.96]
async_cpu_ms = [82.9, 35.0, 8.5, 35.1]
async_wall = [0.3487, 0.1468, 0.4345, 0.1473]

# 阶段3: 4KB Batch
batch_n = [4, 8, 16, 32, 48, 56, 64, 72, 96, 128, 256]
batch_tp = [2.17, 3.36, 4.04, 4.60, 43.94, 48.96, 53.03, 56.23, 66.23, 75.59, 94.29]
batch_ratio = [0.25, 0.39, 0.47, 0.53, 5.14, 5.73, 6.20, 6.58, 7.75, 8.84, 11.03]
batch_cpu_baseline = 8.55

# 阶段4.1: 散落页场景
scene_n = [32, 48, 64, 128, 256]
scene_contig = [4.50, 43.97, 53.10, 76.89, 97.69]
scene_scatter = [11.88, 43.80, 46.54, 76.74, 97.64]
scene_ratio = [2.64, 1.00, 0.88, 1.00, 1.00]

# 阶段4.2: snapshot 搬运
pause_0mb = 53.32
pause_256mb = 288.94
pure_transfer = 235.62
transfer_cpu_cost = 942.0
pause_0mb_runs = [48.41, 56.16, 50.59, 51.53, 59.92]
pause_256mb_runs = [298.47, 286.45, 307.41, 270.79, 281.59]
symbols = [('ext4 文件系统逻辑', 55.0, '持久化簿记（非数据搬运）'),
           ('copy_page_to_iter', 10.18, '数据搬运（process_vm_readv）'),
           ('copy_page_from_iter_atomic', 2.46, '数据搬运（写 page cache）'),
           ('页表遍历+元数据+状态机', 32.0, '逻辑')]
cost_compare = [('CPU memcpy', 82.9), ('DSA 异步卸载', 8.5), ('AgentENV snapshot', 942.0)]
offload = [('4KB + usleep 异步', 155.0, -37.0, '假象'),
           ('4KB + busy-poll', 24.4, 78.4, '正收益'),
           ('64KB + usleep 异步', 9.4, 91.0, '正收益')]

# ================================================================
# 图 1: 吞吐盈亏平衡 (memcpy / fill)
# ================================================================
fig, ax = plt.subplots(figsize=(7.6, 4.4), dpi=150)
ax.semilogx(size_kib, cpu_memcpy, 'o-', label='CPU memcpy')
ax.semilogx(size_kib, dsa_memcpy, 's-', label='DSA memcpy')
ax.semilogx(size_kib, cpu_fill, '^-', label='CPU fill')
ax.semilogx(size_kib, dsa_fill, 'd-', label='DSA fill')
ax.axvline(1024, color='k', linestyle='--', linewidth=1.0)
ax.annotate('盈亏平衡 ≈ 1 MB', xy=(1024, 26.2), xytext=(120, 60),
            arrowprops=dict(arrowstyle='->', color='black', lw=1.0), fontsize=9)
ax.set_xlabel('尺寸 (KiB)')
ax.set_ylabel('吞吐 (GiB/s)')
ax.set_title('图1  CPU vs DSA 吞吐对比（memcpy / fill，单发 dedicated 队列）')
ax.legend(loc='center right', ncol=2)
ax.set_xticks(size_kib)
ax.set_xticklabels([str(k) for k in size_kib])
fig.tight_layout()
fig.savefig(os.path.join(FIG_DIR, 'fig1_吞吐盈亏平衡.png'), bbox_inches='tight')
plt.close(fig)

# ================================================================
# 图 2: 异步卸载 CPU 释放
# ================================================================
fig, ax = plt.subplots(figsize=(7.6, 4.4), dpi=150)
x = np.arange(len(async_mode))
colors = ['#0072BD', '#D95319', '#77AC30', '#A2142F']
bars = ax.bar(x, async_cpu_ms, color=colors, width=0.6, edgecolor='black', linewidth=0.5)
for xi, v in zip(x, async_cpu_ms):
    ax.text(xi, v + 1.5, f'{v:.1f}', ha='center', va='bottom', fontsize=9)
ax.set_xticks(x)
ax.set_xticklabels(async_mode, fontsize=9)
ax.set_ylabel('每 GB CPU 时间 (ms)')
ax.set_title('图2  异步卸载的 CPU 释放（2MB × 2000 = 4GB）')
ax.annotate('释放率 89.76%\n(82.9 → 8.5 ms/GB)', xy=(2, 8.5), xytext=(2.6, 45),
            arrowprops=dict(arrowstyle='->', color='black', lw=1.0), fontsize=9)
ax.set_ylim(0, 95)
fig.tight_layout()
fig.savefig(os.path.join(FIG_DIR, 'fig2_异步CPU释放.png'), bbox_inches='tight')
plt.close(fig)

# ================================================================
# 图 3: 4KB Batch 批量效率阈值
# ================================================================
fig, ax = plt.subplots(figsize=(7.6, 4.4), dpi=150)
ax.plot(batch_n, batch_tp, 'o-', color='#0072BD')
ax.axhline(batch_cpu_baseline, color='#D95319', linestyle='--', linewidth=1.2,
           label=f'CPU 冷拷贝基线 {batch_cpu_baseline} GB/s')
ax.axvline(48, color='k', linestyle=':', linewidth=1.0)
ax.annotate('阈值 N≈48\n4.60 → 43.94 GB/s', xy=(48, 43.94), xytext=(70, 25),
            arrowprops=dict(arrowstyle='->', color='black', lw=1.0), fontsize=9)
ax.set_xscale('log', base=2)
ax.set_xticks(batch_n)
ax.set_xticklabels([str(n) for n in batch_n])
ax.set_xlabel('Batch 大小 N（4KB 页数）')
ax.set_ylabel('吞吐 (GB/s)')
ax.set_title('图3  4KB 页操作 Batch 打包（冷数据）')
ax.legend(loc='upper left')
fig.tight_layout()
fig.savefig(os.path.join(FIG_DIR, 'fig3_4KB批量打包.png'), bbox_inches='tight')
plt.close(fig)

# ================================================================
# 图 4: 散落页 vs 连续页
# ================================================================
fig, ax = plt.subplots(figsize=(7.6, 4.4), dpi=150)
ax.plot(scene_n, scene_contig, 'o-', label='连续页 batch')
ax.plot(scene_n, scene_scatter, 's-', label='散落页 batch')
ax.axhline(9.22, color='#EDB120', linestyle='--', linewidth=1.0, label='CPU 冷页基线 9.22 GB/s')
ax.annotate('散落/连续 ≈ 1.0 (N≥48)\n物理前提成立', xy=(128, 76.74), xytext=(64, 30),
            arrowprops=dict(arrowstyle='->', color='black', lw=1.0), fontsize=9)
ax.set_xlabel('Batch 大小 N')
ax.set_ylabel('吞吐 (GB/s)')
ax.set_title('图4  散落页 vs 连续页 Batch 吞吐（冷页）')
ax.legend(loc='upper left')
ax.set_xticks(scene_n)
fig.tight_layout()
fig.savefig(os.path.join(FIG_DIR, 'fig4_散落页场景.png'), bbox_inches='tight')
plt.close(fig)

# ================================================================
# 图 5: snapshot 搬运成本 + DSA 卸载收益
# ================================================================
fig, axes = plt.subplots(1, 2, figsize=(9.5, 4.2), dpi=150)
# 左: 每 GB CPU 成本对比
ax = axes[0]
names = [c[0] for c in cost_compare]
vals = [c[1] for c in cost_compare]
bars = ax.bar(range(len(names)), vals, color=['#0072BD', '#77AC30', '#A2142F'],
              width=0.55, edgecolor='black', linewidth=0.5)
ax.set_yscale('log')
for xi, v in zip(range(len(names)), vals):
    ax.text(xi, v * 1.15, f'{v:.0f}', ha='center', va='bottom', fontsize=9)
ax.set_xticks(range(len(names)))
ax.set_xticklabels(names, fontsize=9)
ax.set_ylabel('每 GB CPU 时间 (ms)')
ax.set_title('(a) snapshot 搬运成本对比')

# 右: DSA 卸载 CPU 下降比例
ax = axes[1]
names2 = [o[0] for o in offload]
vals2 = [o[2] for o in offload]
col2 = ['#A2142F', '#77AC30', '#77AC30']
bars = ax.bar(range(len(names2)), vals2, color=col2, width=0.55, edgecolor='black', linewidth=0.5)
ax.axhline(0, color='black', linewidth=0.8)
for xi, v in zip(range(len(names2)), vals2):
    ax.text(xi, v + (2 if v >= 0 else -6), f'{v:+.1f}%', ha='center',
            va='bottom' if v >= 0 else 'top', fontsize=9)
ax.set_xticks(range(len(names2)))
ax.set_xticklabels(names2, fontsize=9)
ax.set_ylabel('相对 CPU memcpy 的 CPU 下降 (%)')
ax.set_title('(b) DSA 卸载 memory copy 的 CPU 下降')
ax.set_ylim(-60, 110)
fig.suptitle('图5  AgentENV snapshot 搬运路径的 DSA 卸载收益', fontsize=11)
fig.tight_layout(rect=[0, 0, 1, 0.95])
fig.savefig(os.path.join(FIG_DIR, 'fig5_snapshot搬运.png'), bbox_inches='tight')
plt.close(fig)

print('figures done:', os.listdir(FIG_DIR))

# ================================================================
# Excel
# ================================================================
HEADER_FILL = PatternFill('solid', fgColor='2F6FED')
HEADER_FONT = Font(name='微软雅黑', size=10, bold=True, color='FFFFFF')
TITLE_FONT = Font(name='微软雅黑', size=13, bold=True, color='1A2233')
NOTE_FONT = Font(name='微软雅黑', size=8.5, color='5B6675')
BODY_FONT = Font(name='微软雅黑', size=10, color='1A2233')
THIN = Side(style='thin', color='C9D2E0')
BORDER = Border(left=THIN, right=THIN, top=THIN, bottom=THIN)
CENTER = Alignment(horizontal='center', vertical='center')
LEFT = Alignment(horizontal='left', vertical='center')


def style_cell(c, font=BODY_FONT, align=CENTER, border=True, fill=None):
    c.font = font
    c.alignment = align
    if border:
        c.border = BORDER
    if fill:
        c.fill = fill


def add_sheet(ws, title, note, headers, rows, col_widths=None, numeric_cols=None):
    numeric_cols = numeric_cols or set()
    ws.merge_cells(start_row=1, start_column=1, end_row=1, end_column=len(headers))
    t = ws.cell(1, 1, title)
    t.font = TITLE_FONT
    t.alignment = LEFT
    if note:
        ws.merge_cells(start_row=2, start_column=1, end_row=2, end_column=len(headers))
        n = ws.cell(2, 1, note)
        n.font = NOTE_FONT
        n.alignment = LEFT
    hr = 3 if note else 2
    for j, h in enumerate(headers, 1):
        c = ws.cell(hr, j, h)
        c.font = HEADER_FONT
        c.fill = HEADER_FILL
        c.alignment = CENTER
        c.border = BORDER
    for i, row in enumerate(rows):
        rr = hr + 1 + i
        for j, v in enumerate(row, 1):
            c = ws.cell(rr, j, v)
            if (j - 1) in numeric_cols:
                c.number_format = '0.00'
            style_cell(c)
    if col_widths:
        for j, w in enumerate(col_widths, 1):
            ws.column_dimensions[get_column_letter(j)].width = w
    ws.freeze_panes = ws.cell(hr + 1, 1)


wb = Workbook()

# Sheet 0: 总览
ws = wb.active
ws.title = '总览'
ws.merge_cells('A1:B1')
ws.cell(1, 1, 'Intel DSA 沙箱加速能力评估 · 实验数据汇总')
ws.cell(1, 1).font = Font(name='微软雅黑', size=14, bold=True, color='1A2233')
ws.merge_cells('A2:B2')
ws.cell(2, 1, '2026-10-12 实测 · Intel Xeon Platinum 8480+ (Sapphire Rapids) · 224 vCPU · 2 NUMA · Linux 6.14')
ws.cell(2, 1).font = NOTE_FONT
conclusions = [
    ('核心结论（一句话）', 'DSA 对沙箱的价值成立，但前提是「Batch 攒批 + 异步卸载」两者兼备。'),
    ('结论 1 · 盈亏平衡', '单发盈亏平衡 ≈ 1 MB；4KB 单发 DSA 慢 CPU 78×（固定提交开销 ~4.8 µs）。'),
    ('结论 2 · CPU 释放', '异步卸载把搬运占用的 CPU 时间从 82.9 降到 8.5 ms/GB，释放率 89.76%。'),
    ('结论 3 · Batch 攒批', '4KB 页操作 Batch 阈值 N≈48，≥48 后反超 CPU 冷拷贝 5~11 倍。'),
    ('结论 4 · 散落页', '散落页 batch 物理前提成立（N≥48 散落/连续 ≈ 1.0）；5~11× 仅在冷页成立。'),
    ('结论 5 · snapshot', 'AgentENV snapshot 搬运 942 ms/GB；DSA 卸载 4KB 页 CPU 降 78.4%（busy-poll）。'),
]
r = 4
for k, v in conclusions:
    ws.cell(r, 1, k).font = Font(name='微软雅黑', size=10, bold=True, color='0072BD')
    ws.cell(r, 2, v).font = BODY_FONT
    ws.cell(r, 2).alignment = LEFT
    r += 1
ws.column_dimensions['A'].width = 22
ws.column_dimensions['B'].width = 88

# Sheet 1: 吞吐盈亏平衡
rows = []
for i in range(len(size_kib)):
    rows.append([size_kib[i], cpu_memcpy[i], dsa_memcpy[i], cpu_fill[i], dsa_fill[i]])
add_sheet(wb.create_sheet('吞吐盈亏平衡'),
          'CPU vs DSA 吞吐对比（memcpy / fill，单发 dedicated 队列，单位 GiB/s）',
          '盈亏平衡 ≈ 1 MB；4KB 单发 DSA 慢 CPU 78×（memcpy）。数据见 results/stage2-benchmark/memcpy_fill_cpu_vs_dsa.csv',
          ['尺寸 (KiB)', 'CPU memcpy', 'DSA memcpy', 'CPU fill', 'DSA fill'],
          rows, col_widths=[14, 14, 14, 14, 14], numeric_cols={1, 2, 3, 4})

# Sheet 2: 同步卸载
rows = [(sync_path[i], sync_tp[i], sync_occ[i], sync_cpu_s[i]) for i in range(2)]
add_sheet(wb.create_sheet('同步卸载吞吐'),
          'DSA 同步 busy-poll 吞吐（搬运 4GB，dedicated 队列）',
          '同步 busy-poll 占用率仍 ~100%，吞吐 1.92×；真正释放需异步。数据见 stage2-benchmark/cpu_time_offload.csv',
          ['路径', '吞吐 (GB/s)', 'CPU 占用率 (%)', '每 GB CPU 时间 (s)'],
          rows, col_widths=[24, 16, 16, 20], numeric_cols={1, 2, 3})

# Sheet 3: 异步释放
rows = [(async_mode[i], async_occ[i], async_cpu_ms[i], async_wall[i]) for i in range(4)]
add_sheet(wb.create_sheet('异步CPU释放'),
          '真异步卸载的 CPU 释放（2MB × 2000 = 4GB）',
          '释放率（async+sleep vs CPU）= 89.76%；umwait 只省电不释放。数据见 stage3-async/cpu_offload_async.csv',
          ['模式', 'CPU 占用率 (%)', '每 GB CPU 时间 (ms)', '墙钟时间 (s)'],
          rows, col_widths=[24, 16, 22, 16], numeric_cols={1, 2, 3})

# Sheet 4: 4KB 批量
rows = [(batch_n[i], batch_n[i] * 4096, batch_tp[i], batch_ratio[i]) for i in range(len(batch_n))]
add_sheet(wb.create_sheet('4KB批量打包'),
          '4KB 页操作 Batch 打包吞吐（冷数据，CPU 冷拷贝基线 8.55 GB/s）',
          '存在尖锐批量效率阈值 N≈48（N≤32 未真正批量）。数据见 stage3-batch/batch_4k.csv',
          ['Batch 大小 N', '总数据量 (B)', '吞吐 (GB/s)', 'vs CPU 比值'],
          rows, col_widths=[16, 16, 14, 14], numeric_cols={1, 2, 3})

# Sheet 5: 散落页场景
rows = [(scene_n[i], scene_contig[i], scene_scatter[i], scene_ratio[i]) for i in range(len(scene_n))]
add_sheet(wb.create_sheet('散落页场景'),
          '散落页 vs 连续页 Batch 吞吐（冷页，CPU 冷页基线 9.22 GB/s）',
          'N≥48 散落/连续 ≈ 1.00（物理前提成立）；热页（38.57 GB/s）下反超缩水到 1~2.5×。数据见 stage4-scene/scene_4k.csv',
          ['Batch 大小 N', '连续页 (GB/s)', '散落页 (GB/s)', '散落/连续比值'],
          rows, col_widths=[16, 16, 16, 16], numeric_cols={1, 2, 3})

# Sheet 6: snapshot 搬运
ws = wb.create_sheet('snapshot搬运')
add_sheet(ws, 'AgentENV snapshot 搬运成本量化（5 次取平均）', None,
          ['指标', '值 (ms)'],
          [('pause 固定开销 (0MB dirty)', pause_0mb),
           ('pause (256MB dirty)', pause_256mb),
           ('纯搬运 (256MB)', pure_transfer),
           ('搬运 CPU 成本 (ms/GB)', transfer_cpu_cost)],
          col_widths=[40, 20], numeric_cols={1})

ws2 = wb.create_sheet('snapshot符号构成')
add_sheet(ws2, 'snapshot 搬运链路符号构成（perf 采样 orchestrator + ublk）', None,
          ['符号', '占比 (%)', '属性'],
          [(s[0], s[1], s[2]) for s in symbols],
          col_widths=[30, 12, 40], numeric_cols={1})

ws3 = wb.create_sheet('DSA卸载收益')
add_sheet(ws3, 'DSA 卸载 memory copy 的 CPU 下降比例（散落页 → 连续 target）',
          'CPU memcpy 散落 4KB 冷页基线 = 112.7 ms/GB；usleep 异步是假象（~25µs 系统调用开销）。',
          ['配置', '每 GB CPU (ms)', 'vs CPU memcpy (%)', '结论'],
          [(o[0], o[1], o[2], o[3]) for o in offload],
          col_widths=[22, 16, 20, 12], numeric_cols={1, 2})

wb.save(XLSX_PATH)
print('excel done:', XLSX_PATH)
print('OK')