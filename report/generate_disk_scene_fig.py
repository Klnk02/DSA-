# -*- coding: utf-8 -*-
"""
补测: mem2disk / disk2mem 场景 —— 生成 fig6 + 追加 Excel Sheet
数据来源: ./dsa_disk_scene /home/m00954035/testCode/dsa_test.bin 20 (2026-10-13 在 8480+ 上跑)
运行: python generate_disk_scene_fig.py
"""
import os
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np
from openpyxl import load_workbook
from openpyxl.styles import Font, PatternFill, Alignment, Border, Side
from openpyxl.utils import get_column_letter

BASE = r"d:\workSpace\DSA-"
FIG_DIR = os.path.join(BASE, "report", "figures")
XLSX_PATH = os.path.join(BASE, "report", "DSA沙箱加速_实验数据汇总.xlsx")

# ---- MATLAB 风格（与 generate_matlab_plots.py 保持一致）----
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

# ---- 实测数据 (ms/GB CPU 时间, repeat=20) ----
seg_labels = ['CPU memcpy', 'DSA memcpy\n(busy-poll)', '磁盘读\nread', '磁盘写\nwrite']
seg_vals    = [56.0, 33.8, 118.6, 158.9]
seg_std     = [0.2, 0.4, 3.2, 5.0]
seg_colors  = ['#0072BD', '#77AC30', '#EDB120', '#D95319']

scn = {
    'disk2mem': dict(cur=174.6, dsa=152.4, save=12.73),
    'mem2disk': dict(cur=214.8, dsa=192.6, save=10.34),
}

COPY_RELEASE = 39.68   # 拷贝段 DSA 释放率 (%)
COPY_SHARE = {'disk2mem': 32.1, 'mem2disk': 26.1}  # 拷贝段占比 (%)

# ================================================================
# fig6: mem2disk / disk2mem 场景的 CPU 成本 + DSA 卸载收益
# ================================================================
fig, axes = plt.subplots(1, 2, figsize=(10.0, 4.3), dpi=150)

# (a) 分段 CPU 成本
ax = axes[0]
x = np.arange(len(seg_labels))
ax.bar(x, seg_vals, yerr=seg_std, color=seg_colors, width=0.62,
       edgecolor='black', linewidth=0.5, capsize=3, error_kw=dict(lw=1.0))
for xi, v in zip(x, seg_vals):
    ax.text(xi, v + 6, f'{v:.1f}', ha='center', va='bottom', fontsize=9)
ax.annotate('DSA 释放 39.7%\n(56.0 → 33.8)', xy=(1, 33.8), xytext=(1, 78),
            arrowprops=dict(arrowstyle='->', color='black', lw=0.9), fontsize=9, ha='center')
ax.annotate('磁盘 I/O 段（不可卸载）占主导',
            xy=(2.5, 158.9), xytext=(2.5, 175), fontsize=8.5, ha='center',
            arrowprops=dict(arrowstyle='->', color='black', lw=0.9))
ax.set_xticks(x)
ax.set_xticklabels(seg_labels, fontsize=8.5)
ax.set_ylabel('每 GB CPU 时间 (ms)')
ax.set_title('(a) 分段 CPU 成本（repeat=20）')
ax.set_ylim(0, 190)

# (b) 端到端场景: 现状 vs DSA 卸载
ax = axes[1]
names = ['disk2mem\n(快照恢复)', 'mem2disk\n(快照生成)']
cur = [scn['disk2mem']['cur'], scn['mem2disk']['cur']]
dsa = [scn['disk2mem']['dsa'], scn['mem2disk']['dsa']]
xx = np.arange(len(names))
w = 0.34
b1 = ax.bar(xx - w/2, cur, w, color='#0072BD', edgecolor='black', linewidth=0.5, label='现状 (CPU memcpy)')
b2 = ax.bar(xx + w/2, dsa, w, color='#77AC30', edgecolor='black', linewidth=0.5, label='DSA 卸载拷贝段')
for i in xx:
    ax.text(i - w/2, cur[i] + 4, f'{cur[i]:.1f}', ha='center', va='bottom', fontsize=9)
    ax.text(i + w/2, dsa[i] + 4, f'{dsa[i]:.1f}', ha='center', va='bottom', fontsize=9)
    sav = [scn['disk2mem']['save'], scn['mem2disk']['save']][i]
    sh  = [COPY_SHARE['disk2mem'], COPY_SHARE['mem2disk']][i]
    ax.text(i, max(cur[i], dsa[i]) + 18, f'省 {sav:.2f}%\n(拷贝段占比 {sh:.1f}%)',
            ha='center', va='bottom', fontsize=8.5, color='#A2142F', fontweight='bold')
ax.set_xticks(xx)
ax.set_xticklabels(names, fontsize=9)
ax.set_ylabel('每 GB CPU 时间 (ms)')
ax.set_title('(b) 端到端 CPU 成本：现状 vs DSA')
ax.set_ylim(0, 260)
ax.legend(loc='upper left', fontsize=8.5)

fig.suptitle('图6  mem2disk / disk2mem 场景的 CPU 成本与 DSA 卸载收益（CPU 口径，非冷盘 wall）', fontsize=11)
fig.tight_layout(rect=[0, 0, 1, 0.94])
fig.savefig(os.path.join(FIG_DIR, 'fig6_disk场景.png'), bbox_inches='tight')
plt.close(fig)
print('fig done:', os.path.join(FIG_DIR, 'fig6_disk场景.png'))

# ================================================================
# 追加 Excel Sheet
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

wb = load_workbook(XLSX_PATH)
ws = wb.create_sheet('mem2disk与disk2mem')

def cell(r, c, v, font=BODY_FONT, align=CENTER, fill=None):
    cc = ws.cell(r, c, v)
    cc.font = font
    cc.alignment = align
    cc.border = BORDER
    if fill:
        cc.fill = fill
    return cc

# 标题
ws.merge_cells('A1:F1')
t = ws.cell(1, 1, 'mem2disk / disk2mem 场景 CPU 成本（补测，8480+ 实测）')
t.font = TITLE_FONT
t.alignment = LEFT
ws.merge_cells('A2:F2')
n = ws.cell(2, 1, '口径: CLOCK_PROCESS_CPUTIME_ID 每 GB CPU 时间 (ms), repeat=20; DSA 只卸载「内存拷贝段」, 磁盘 I/O 段不可卸载')
n.font = NOTE_FONT
n.alignment = LEFT

# 分段指标
ws.merge_cells('A4:F4')
h = ws.cell(4, 1, '① 分段每 GB CPU 时间（avg / stddev / min / max / wall）')
h.font = Font(name='微软雅黑', size=10, bold=True, color='0072BD')
h.alignment = LEFT

headers = ['segment', 'avg_cpu_ms_per_gb', 'stddev_ms', 'min_ms', 'max_ms', 'wall_s_per_gb']
for j, hd in enumerate(headers, 1):
    c = cell(5, j, hd, font=HEADER_FONT, fill=HEADER_FILL)

segs = [
    ('cpu_memcpy', 56.0, 0.2, 55.8, 56.9, 0.0560),
    ('dsa_memcpy_busypoll', 33.8, 0.4, 33.6, 35.6, 0.0338),
    ('disk_read', 118.6, 3.2, 115.8, 124.7, 0.1189),
    ('disk_write', 158.9, 5.0, 157.3, 179.9, 0.1590),
]
for i, row in enumerate(segs):
    r = 6 + i
    for j, v in enumerate(row, 1):
        c = cell(r, j, v)
        if j >= 2:
            c.number_format = '0.00'

# 场景组合
ws.merge_cells('A11:F11')
h2 = ws.cell(11, 1, '② 端到端场景 CPU 成本（现状 vs DSA 卸载内存拷贝段）')
h2.font = Font(name='微软雅黑', size=10, bold=True, color='0072BD')
h2.alignment = LEFT

headers2 = ['scenario', 'current_ms_per_gb', 'dsa_offload_ms_per_gb', 'savings_pct', '拷贝段占比%', '拷贝段 DSA 释放%']
for j, hd in enumerate(headers2, 1):
    cell(12, j, hd, font=HEADER_FONT, fill=HEADER_FILL)

scens = [
    ('disk2mem (读盘+memcpy)', 174.6, 152.4, 12.73, 32.1, 39.68),
    ('mem2disk (memcpy+写盘)', 214.8, 192.6, 10.34, 26.1, 39.68),
]
for i, row in enumerate(scens):
    r = 13 + i
    for j, v in enumerate(row, 1):
        c = cell(r, j, v)
        if j >= 2:
            c.number_format = '0.00'

# 结论与注意
ws.merge_cells('A16:F16')
c1 = ws.cell(16, 1, '结论: 端到端 CPU 节省 10.3% ~ 12.7%，收益被磁盘 I/O 段锁死上限（拷贝段仅占 26% ~ 32% × 该段 DSA 释放 39.68%）。')
c1.font = Font(name='微软雅黑', size=9, bold=True, color='A2142F')
c1.alignment = LEFT
ws.merge_cells('A17:F17')
c2 = ws.cell(17, 1, '注意: disk_read/write 的 wall≈cpu，说明本测读写基本命中 page cache（RAM），是「CPU 成本口径」而非「冷盘 wall 吞吐」；结论只对 CPU 释放成立。')
c2.font = NOTE_FONT
c2.alignment = LEFT

for j, w in enumerate([26, 18, 20, 14, 14, 20], 1):
    ws.column_dimensions[get_column_letter(j)].width = w

wb.save(XLSX_PATH)
print('excel done:', XLSX_PATH)
print('OK')