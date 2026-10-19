# -*- coding: utf-8 -*-
# 当前边界见CURRENT_STATUS.md；数据为汇总录入，原始日志待补；既有xlsx为历史快照。
"""
dsa_disk_scene 方案对比 Excel —— 生成 v6（用户指定结构）
数据来源: 2026-10-19 服务器 ./dsa_disk_scene /var/lib/aenv/disk_scene.bin 60 15 both
  60MB ≤ 64 → 冷页口径（每次窗口前冲 LLC）；onCPU=CLOCK_PROCESS_CPUTIME_ID 主口径
  跨源对账全过: clock=5786ms /proc=5780ms Δ=-6.1ms（容差329ms）→ onCPU 数据可信
结构（用户指定）:
  Sheet1 方案对比主表: 序号 | 方案 | 场景 | utime | stime | onCPU  + 推断比结论块
  Sheet2 详细onCPU: 11 臂完整数据 + snapshot 场景合成表
运行: python generate_disk_scene_xlsx.py
"""
import os
from openpyxl import Workbook
from openpyxl.styles import Font, PatternFill, Alignment, Border, Side
from openpyxl.utils import get_column_letter

BASE = r"d:\workSpace\DSA-"
OUT = os.path.join(BASE, "results", "stage4-sandbox", "disk_scene_60mb_v6.xlsx")

HDR_FILL = PatternFill('solid', fgColor='0072BD')       # MATLAB 蓝
HDR_FONT = Font(name='微软雅黑', bold=True, color='FFFFFF', size=10)
BODY_FONT = Font(name='微软雅黑', size=10)
NOTE_FONT = Font(name='微软雅黑', size=9, color='595959')
KEY_FONT = Font(name='微软雅黑', size=10, bold=True, color='A2142F')
THIN = Side(style='thin', color='BFBFBF')
BORDER = Border(left=THIN, right=THIN, top=THIN, bottom=THIN)
CENTER = Alignment(horizontal='center', vertical='center')
LEFT = Alignment(horizontal='left', vertical='center', wrap_text=True)


def style_header(ws, row, ncols):
    for c in range(1, ncols + 1):
        cell = ws.cell(row=row, column=c)
        cell.fill, cell.font, cell.alignment, cell.border = HDR_FILL, HDR_FONT, CENTER, BORDER


def style_body(ws, r0, r1, ncols):
    for r in range(r0, r1 + 1):
        for c in range(1, ncols + 1):
            cell = ws.cell(row=r, column=c)
            cell.font, cell.border = BODY_FONT, BORDER
            cell.alignment = LEFT if c == 1 and isinstance(cell.value, str) and len(str(cell.value)) > 12 else CENTER


wb = Workbook()

# ================= Sheet1 方案对比主表 =================
ws = wb.active
ws.title = '方案对比主表'
headers = ['序号', '方案', '场景', 'utime (ms)', 'stime (ms)', 'onCPU (ms)']
ws.append(headers)
style_header(ws, 1, len(headers))

# 一次 snapshot = 60MB；utime/stime = getrusage tick 分项；onCPU = 主口径
MAIN = [
    (1, '软件',       'mem2disk', 0.2, 19.2, 19.394),
    (2, 'DSA直接替换（合成）', 'mem2disk', 2.4, 14.3, 16.682),
    (3, '软件',       'disk2mem', 0.2, 20.3, 20.502),
    (4, 'DSA直接替换（合成）', 'disk2mem', 2.3, 15.5, 17.789),
    (5, '软件',       'mem2mem',  5.0, 0.0, 4.983),
    (6, 'DSA',        'mem2mem',  2.2, 0.1, 2.270),
]
for row in MAIN:
    ws.append(row)
style_body(ws, 2, 7, len(headers))

# ---- 推断比结论块（对真实 pause 全链路 516 ± 74 ms/GiB, measure_copy_share 全量）----
r = 9
ws.cell(row=r, column=1, value='推断比结论（模拟臂 Δ → 真实全链路 516±74 ms/GiB）').font = Font(
    name='微软雅黑', bold=True, size=11)
r += 1
inf_hdr = ['方向', 'Δ = sw_2step−dsa_2step\n(ms/GiB)', '推断比 = Δ/516', '两步重构净亏\n(dsa_2step vs sw_1step)']
for c, h in enumerate(inf_hdr, 1):
    ws.cell(row=r, column=c, value=h)
style_header(ws, r, len(inf_hdr))
INFERENCE = [
    ('mem2disk', '408.7 − 365.5 = 43.2', '43.2/516 = 8.4%', '365.5−331.0 = +34.5 (+10.4%)'),
    ('disk2mem', '423.6 − 387.4 = 36.2', '≈7.0%（对称假设分母）', '387.4−349.9 = +37.5 (+10.7%)'),
]
for row in INFERENCE:
    r += 1
    for c, v in enumerate(row, 1):
        ws.cell(row=r, column=c, value=v)
style_body(ws, r - 1, r, len(inf_hdr))

# ---- 注释块（精简） ----
r += 2
NOTES = [
    '注1: DSA直接替换 = 软件(1step) − sw_memcpy + dsa_idle，属于合成假设；当前尚无直接接管实现。软件一步不是完整snapshot。',
    '注2: 两步重构净亏——新增 staging 跳 74~78 > DSA 省回 36~43 ms/GiB → 净亏 ~10%。',
    '注3: 71.5=另测copy占比×pause CPU斜率，是跨窗口合成估算；7%～8%是条件推断，非真实snapshot收益或严格上限。',
    '注4: onCPU=CLOCK_PROCESS_CPUTIME_ID 主口径；60MB 冷页（前冲 LLC）；disk2mem 分母 516 为对称假设；'
    '数据 2026-10-19 both 轮，跨源对账 11 臂全 OK。',
]
for note in NOTES:
    ws.cell(row=r, column=1, value=note).font = NOTE_FONT
    ws.merge_cells(start_row=r, start_column=1, end_row=r, end_column=6)
    ws.cell(row=r, column=1).alignment = LEFT
    r += 1

for c, w in zip('ABCDEF', [8, 14, 12, 12, 12, 12]):
    ws.column_dimensions[c].width = w
ws.column_dimensions['B'].width = 26   # Δ 列宽
ws.freeze_panes = 'A2'

# ================= Sheet2 详细onCPU =================
ws2 = wb.create_sheet('详细onCPU')
hdr2 = ['op', 'impl', 'size_mb', 'repeat', 'onCPU_ms\n(单次均值)', 'sd', 'min', 'max',
        'ms/GiB', 'wall_ms', 'rw_read\n_per_gb', 'rw_write\n_per_gb',
        '复核proc_ms\n(整臂)', '复核clock_ms\n(整臂)', 'Δ_ms', 'match']
ws2.append(hdr2)
style_header(ws2, 1, len(hdr2))

DETAIL = [
    ('mem2mem', 'sw_memcpy',                 60, 15, 4.983, 0.034, 4.942, 5.050, 85.0, 4.986, 0.000, 0.000, 330.0, 330.1, -0.1, 'OK'),
    ('mem2mem', 'dsa_idle',                  60, 15, 2.270, 0.567, 2.074, 4.316, 38.7, 2.273, 0.000, 0.000, 280.0, 284.3, -4.3, 'OK'),
    ('mem2mem', 'dsa_busy',                  60, 15, 2.119, 0.016, 2.106, 2.163, 36.2, 2.125, 0.000, 0.000, 280.0, 278.0, 2.0, 'OK'),
    ('disk2mem', 'sw_1step_read_direct',     60, 15, 20.502, 0.478, 19.953, 21.593, 349.9, 20.635, 1.000, 0.000, 610.0, 610.1, -0.1, 'OK'),
    ('disk2mem', 'sw_2step_read_stg_memcpy', 60, 15, 24.819, 0.582, 24.117, 26.199, 423.6, 24.973, 1.000, 0.000, 680.0, 678.0, 2.0, 'OK'),
    ('disk2mem', 'dsa_2step_read_stg_dsa_idle', 60, 15, 22.701, 0.494, 21.937, 23.433, 387.4, 22.856, 1.000, 0.000, 650.0, 646.0, 4.0, 'OK'),
    ('disk2mem', 'dsa_2step_read_stg_dsa_busy', 60, 15, 23.046, 0.596, 22.289, 24.405, 393.3, 23.221, 1.000, 0.000, 640.0, 649.1, -9.1, 'OK'),
    ('mem2disk', 'sw_1step_write_direct',    60, 15, 19.394, 0.799, 18.994, 22.225, 331.0, 19.911, 0.000, 1.000, 550.0, 546.5, 3.5, 'OK'),
    ('mem2disk', 'sw_2step_memcpy_stg_write', 60, 15, 23.946, 0.098, 23.841, 24.108, 408.7, 24.479, 0.000, 1.000, 610.0, 613.4, -3.4, 'OK'),
    ('mem2disk', 'dsa_2step_dsa_stg_write_idle', 60, 15, 21.419, 0.072, 21.337, 21.562, 365.5, 21.958, 0.000, 1.000, 570.0, 575.5, -5.5, 'OK'),
    ('mem2disk', 'dsa_2step_dsa_stg_write_busy', 60, 15, 21.512, 0.086, 21.374, 21.728, 367.1, 22.046, 0.000, 1.000, 580.0, 575.1, 4.9, 'OK'),
]
for row in DETAIL:
    ws2.append(row)
style_body(ws2, 2, 1 + len(DETAIL), len(hdr2))

# ---- 附: snapshot 场景合成 ----
r = len(DETAIL) + 4
ws2.cell(row=r, column=1, value='snapshot 场景合成（每 snapshot = 60MB，onCPU 单位 ms）').font = \
    Font(name='微软雅黑', bold=True, size=11)
r += 1
syn_hdr = ['scenario', 'structure', 'sw_oncpu_ms', 'dsa_oncpu_ms', 'delta_ms', 'delta_pct', 'source']
for c, h in enumerate(syn_hdr, 1):
    ws2.cell(row=r, column=c, value=h)
style_header(ws2, r, len(syn_hdr))
SYN = [
    ('mem2disk', 'current_kernel_single_hop', 19.394, 19.394, 0.000, '0.0%', '现状实测（DSA 不可介入）'),
    ('mem2disk', 'assumed_staging_hop',       23.946, 21.419, -2.527, '-10.6%', '实测；DSA 替第二跳'),
    ('disk2mem', 'current_kernel_single_hop', 20.502, 20.502, 0.000, '0.0%', '现状实测（DSA 不可介入）'),
    ('disk2mem', 'assumed_staging_hop',       24.819, 22.701, -2.118, '-8.5%', '实测；DSA 替第二跳'),
    ('mem2mem',  'reference_pure_memory',      4.983,  2.270, -2.712, '-54.4%', '参照臂（纯搬运）'),
    ('mem2mem',  'reference_busy_poll',        4.983,  2.119, -2.864, '-57.5%', '自旋口径，仅横向比'),
]
for row in SYN:
    r += 1
    for c, v in enumerate(row, 1):
        ws2.cell(row=r, column=c, value=v)
style_body(ws2, r - len(SYN), r, len(syn_hdr))

note2 = '注: 复核两列为独立记账路径（/proc tick vs clock ns），整臂区间含窗口外开销属预期；rw_*=1.0 即全真读盘。'
r += 2
ws2.cell(row=r, column=1, value=note2).font = NOTE_FONT
ws2.merge_cells(start_row=r, start_column=1, end_row=r, end_column=9)
ws2.cell(row=r, column=1).alignment = LEFT

widths = [10, 30, 9, 8, 11, 8, 8, 8, 9, 9, 9, 9, 12, 12, 8, 7]
for i, w in enumerate(widths, 1):
    ws2.column_dimensions[get_column_letter(i)].width = w
ws2.freeze_panes = 'A2'

wb.save(OUT)
print('xlsx done:', OUT)
print('OK')
