# -*- coding: utf-8 -*-
"""生成 DSA 沙箱加速汇报 PPT（.pptx）· v3
变更：华为公司色重构 + 附录图注（是什么/怎么读/要点）+ 全片内容审查修正
（图片素材不动，沿用 fig1~fig7）
"""
import os
from pptx import Presentation
from pptx.util import Inches, Pt
from pptx.dml.color import RGBColor
from pptx.enum.text import PP_ALIGN, MSO_ANCHOR
from pptx.enum.shapes import MSO_SHAPE
from PIL import Image

# ---------- 华为公司色 ----------
RED   = RGBColor(0xC7, 0x00, 0x0B)   # 主红 199/0/11
DEEP  = RGBColor(0xC8, 0x10, 0x2E)   # 深红 200/16/46
MID   = RGBColor(0xD3, 0x39, 0x41)   # 中红 211/57/65（语义：不可卸载/警示）
MAG   = RGBColor(0xD3, 0x38, 0x59)   # 品红 211/56/89
PINK  = RGBColor(0xE2, 0x81, 0x89)   # 浅红 226/129/137（红底上的强调）
PINK2 = RGBColor(0xE2, 0x81, 0x98)   # 粉 226/129/152
# ---------- 中性与语义色 ----------
CARD  = RGBColor(0xF8, 0xF2, 0xF2)   # 卡片暖白
INK   = RGBColor(0x1A, 0x1A, 0x1A)
MUTED = RGBColor(0x6E, 0x6E, 0x6E)
GOOD  = RGBColor(0x1E, 0x9E, 0x6A)   # 语义：收益/可卸载
GOODBG = RGBColor(0xEC, 0xF5, 0xF0)
BADBG  = RGBColor(0xFB, 0xEC, 0xEC)
WHITE = RGBColor(0xFF, 0xFF, 0xFF)
SOFT  = RGBColor(0xF2, 0xD5, 0xD7)   # 红底副文字
FAINT = RGBColor(0xE8, 0xA9, 0xAE)   # 红底弱文字

F = "微软雅黑"
TOTAL = 19

BASE = r"d:\workSpace\DSA-\report\dsa-ppt"
OUT = os.path.join(BASE, "Intel_DSA_沙箱加速_汇报.pptx")
FIG1 = os.path.join(BASE, "assets", "fig1_吞吐盈亏平衡.png")
FIG2 = os.path.join(BASE, "assets", "fig2_异步CPU释放.png")
FIG3 = os.path.join(BASE, "assets", "fig3_4KB批量打包.png")
FIG4 = os.path.join(BASE, "assets", "fig4_散落页场景.png")
FIG5 = os.path.join(BASE, "assets", "fig5_snapshot搬运.png")
FIG6 = os.path.join(BASE, "assets", "fig6_disk场景.png")
FIG7 = os.path.join(BASE, "assets", "fig7_A臂基线.png")

prs = Presentation()
prs.slide_width = Inches(13.333)
prs.slide_height = Inches(7.5)
BLANK = prs.slide_layouts[6]

CUR = [0]

def slide():
    CUR[0] += 1
    return prs.slides.add_slide(BLANK)

def bg(s, c):
    s.background.fill.solid()
    s.background.fill.fore_color.rgb = c

def tb(s, x, y, w, h, text, size=16, color=INK, bold=False, align=PP_ALIGN.LEFT,
       anchor=MSO_ANCHOR.TOP, spacing=1.0, font=F):
    box = s.shapes.add_textbox(Inches(x), Inches(y), Inches(w), Inches(h))
    tf = box.text_frame
    tf.word_wrap = True
    tf.vertical_anchor = anchor
    for i, ln in enumerate(text.split("\n")):
        p = tf.paragraphs[0] if i == 0 else tf.add_paragraph()
        p.alignment = align
        p.line_spacing = spacing
        r = p.add_run(); r.text = ln
        r.font.size = Pt(size); r.font.bold = bold
        r.font.color.rgb = color; r.font.name = font
    return box

def rect(s, x, y, w, h, fill=None, line=None, shape=MSO_SHAPE.ROUNDED_RECTANGLE):
    sp = s.shapes.add_shape(shape, Inches(x), Inches(y), Inches(w), Inches(h))
    if fill is None:
        sp.fill.background()
    else:
        sp.fill.solid(); sp.fill.fore_color.rgb = fill
    if line is None:
        sp.line.fill.background()
    else:
        sp.line.color.rgb = line; sp.line.width = Pt(0.75)
    sp.shadow.inherit = False
    return sp

def add_img(s, path, x, y, w=None, h=None):
    im = Image.open(path); iw, ih = im.size; aspect = iw / ih
    if w and not h: h = w / aspect
    if h and not w: w = h * aspect
    box_aspect = w / h
    if aspect > box_aspect:
        W, H = w, w / aspect
    else:
        H, W = h, h * aspect
    cx, cy = x + (w - W) / 2, y + (h - H) / 2
    s.shapes.add_picture(path, Inches(cx), Inches(cy), Inches(W), Inches(H))

def kicker_title(s, kicker, title, dark=False):
    kc = PINK if dark else RED
    tc = WHITE if dark else INK
    tb(s, 0.6, 0.42, 12.0, 0.35, kicker, size=13, color=kc, bold=True)
    tb(s, 0.6, 0.78, 12.1, 0.8, title, size=30, color=tc, bold=True)
    rect(s, 0.6, 1.55, 12.13, 0.028, fill=kc, shape=MSO_SHAPE.RECTANGLE)

def footer(s, dark=False):
    c = FAINT if dark else MUTED
    tb(s, 0.6, 7.05, 8.0, 0.3, "Intel DSA × AgentENV · 沙箱落地价值评估", size=10, color=c)
    tb(s, 11.1, 7.05, 1.65, 0.3, f"{CUR[0]:02d} / {TOTAL}", size=10, color=c, align=PP_ALIGN.RIGHT)

def divider(kicker, title, sub):
    s = slide()
    bg(s, RED)
    tb(s, 0, 2.4, 13.333, 0.4, kicker, size=15, color=PINK, bold=True, align=PP_ALIGN.CENTER)
    tb(s, 0.6, 2.95, 12.133, 1.1, title, size=42, color=WHITE, bold=True, align=PP_ALIGN.CENTER)
    tb(s, 0, 4.3, 13.333, 0.5, sub, size=16, color=SOFT, align=PP_ALIGN.CENTER)
    return s

def anno_card(s, x, y, w, h, what, how, point):
    """附录图注卡：是什么 / 怎么读 / 要点"""
    rect(s, x, y, w, h, fill=CARD)
    tb(s, x + 0.25, y + 0.15, w - 0.5, 0.3, "这是什么", size=14, color=RED, bold=True)
    tb(s, x + 0.25, y + 0.51, w - 0.45, 0.9, what, size=11.5, color=INK, spacing=1.12)
    tb(s, x + 0.25, y + 1.40, w - 0.5, 0.3, "怎么读", size=14, color=RED, bold=True)
    tb(s, x + 0.25, y + 1.76, w - 0.45, 0.9, how, size=11.5, color=INK, spacing=1.12)
    tb(s, x + 0.25, y + 2.65, w - 0.5, 0.3, "要点", size=14, color=RED, bold=True)
    tb(s, x + 0.25, y + 3.01, w - 0.45, h - 3.2, point, size=11.5, color=INK, spacing=1.12)

def styled_table(s, rows, x, y, w, h, col_w, highlight=None):
    """通用表格：DEEP 表头白字，交替行 CARD/WHITE；highlight={(i,j): color}"""
    nr, nc = len(rows), len(rows[0])
    t = s.shapes.add_table(nr, nc, Inches(x), Inches(y), Inches(w), Inches(h))
    tbl = t.table
    for j, cw in enumerate(col_w):
        tbl.columns[j].width = Inches(cw)
    for i, row in enumerate(rows):
        for j, v in enumerate(row):
            cell = tbl.cell(i, j)
            cell.text = v
            cell.vertical_anchor = MSO_ANCHOR.MIDDLE
            p = cell.text_frame.paragraphs[0]
            p.alignment = PP_ALIGN.CENTER
            if i == 0:
                p.runs[0].font.bold = True; p.runs[0].font.size = Pt(13)
                p.runs[0].font.color.rgb = WHITE; p.runs[0].font.name = F
                cell.fill.solid(); cell.fill.fore_color.rgb = DEEP
            else:
                p.runs[0].font.size = Pt(14); p.runs[0].font.name = F
                p.runs[0].font.color.rgb = INK
                if highlight and (i, j) in highlight:
                    p.runs[0].font.color.rgb = highlight[(i, j)]
                    p.runs[0].font.bold = True
                cell.fill.solid(); cell.fill.fore_color.rgb = CARD if i % 2 == 0 else WHITE
    return tbl

# ============================================================ Slide 1 封面
s = slide(); bg(s, RED)
tb(s, 0.85, 1.05, 11.6, 0.4, "INTEL DSA × AGENTENV · 沙箱落地价值评估", size=15, color=PINK, bold=True)
tb(s, 0.85, 2.35, 11.8, 2.2, "DSA 卸载 host memcpy\n释放 vCPU 资源", size=48, color=WHITE, bold=True)
rect(s, 0.88, 4.62, 2.2, 0.05, fill=WHITE, shape=MSO_SHAPE.RECTANGLE)
tb(s, 0.85, 4.95, 11.8, 0.6, "mem2mem / mem2disk / disk2mem · AgentENV 真实链路 · Intel Xeon 8480+ · Intel DML",
   size=16, color=SOFT)
tb(s, 0.85, 6.5, 11.8, 0.4, "平台组 · 快照加速预研 · 2026-10-14", size=12, color=FAINT)

# ============================================================ Slide 2 目录
s = slide(); bg(s, WHITE)
tb(s, 0.6, 0.45, 12.0, 0.35, "CONTENTS · 目录", size=13, color=RED, bold=True)
tb(s, 0.6, 0.85, 12.0, 0.8, "三个场景 · 真实链路 · 一条结论", size=32, color=INK, bold=True)
cols = [(0.6, "01", "背景与问题", "快照搬运占用 vCPU，DSA 能否卸载？"),
        (6.75, "02", "实验方法与可信度", "测量手段 · 重复次数 · 口径边界。"),
        (0.6,  "03", "场景一 · mem2mem", "纯内存拷贝，能力上限（-39.7%+）。"),
        (6.75, "04", "场景二/三 · mem2disk & disk2mem", "模拟快照链路，端到端省 10%~13%。"),
        (0.6,  "05", "DSA 覆盖边界", "文件系统路径与 metadata 不在覆盖范围。"),
        (6.75, "06", "真实链路验证 · A 臂基线", "AgentENV 实测 674 ms/GB（进行中）。"),
        (0.6,  "07", "结论与前提", "收益受非拷贝段占比限制；B 臂待验证。"),
        (6.75, "08", "附录 · 结语", "补充图表（含图注与读法）+ 原始数据。")]
for i, (x, num, t, d) in enumerate(cols):
    row = i // 2
    y = 1.85 + row * 1.24
    rect(s, x, y, 5.95, 1.06, fill=CARD)
    rect(s, x, y, 0.07, 1.06, fill=RED, shape=MSO_SHAPE.RECTANGLE)
    tb(s, x + 0.32, y + 0.18, 0.75, 0.7, num, size=22, color=RED, bold=True)
    tb(s, x + 1.15, y + 0.13, 4.6, 0.45, t, size=15, color=INK, bold=True)
    tb(s, x + 1.15, y + 0.56, 4.6, 0.4, d, size=11.5, color=MUTED)
footer(s)

# ============================================================ Slide 3 背景
s = slide(); bg(s, WHITE)
kicker_title(s, "背景 · 问题", "快照/恢复的数据搬运在消耗 vCPU")
rect(s, 0.6, 1.75, 5.95, 3.15, fill=CARD)
tb(s, 0.95, 1.95, 5.3, 0.4, "问题链路", size=17, color=INK, bold=True)
tb(s, 0.95, 2.4, 5.3, 2.4,
   "快照生成（mem2disk）：guest 内存 → 缓冲 → 写盘\n"
   "快照恢复（disk2mem）：读盘 → 缓冲 → guest 内存\n"
   "中间的 memcpy 占用 vCPU，与同 NUMA 业务争抢\n"
   "DSA 引擎用 DMA 搬运，取代 CPU 逐字节 memcpy；异步提交时才真正让出 vCPU（busy-poll 只省 CPU 时间、不释放核）",
   size=13, color=INK, spacing=1.15)
rect(s, 6.78, 1.75, 5.95, 3.15, fill=CARD)
tb(s, 7.13, 1.95, 5.3, 0.4, "方法口径", size=17, color=INK, bold=True)
tb(s, 7.13, 2.4, 5.3, 2.4,
   "平台：Xeon 8480+（224 vCPU）· Intel DML\n"
   "微基准：2 MB 块 · dedicated 队列 · busy-poll\n"
   "真实链路：AgentENV sandbox（A 臂基线）\n"
   "指标：每 GB CPU 时间（ms/GB）· repeat=20",
   size=13, color=INK, spacing=1.15)
stats = [("224", "vCPU（8480+）"), ("20×", "重复次数（可信度）"), ("<4%", "各段 stddev/avg")]
sx = 0.6
for big, lbl in stats:
    rect(s, sx, 5.2, 3.9, 1.55, fill=DEEP)
    tb(s, sx, 5.4, 3.9, 0.7, big, size=28, color=WHITE, bold=True, align=PP_ALIGN.CENTER)
    tb(s, sx, 6.1, 3.9, 0.4, lbl, size=12, color=SOFT, align=PP_ALIGN.CENTER)
    sx += 4.1
footer(s)

# ============================================================ Slide 4 方法与可信度
s = slide(); bg(s, WHITE)
kicker_title(s, "方法 · 口径", "怎么测的，结果有多稳定")
rect(s, 0.6, 1.75, 6.0, 3.5, fill=CARD)
tb(s, 0.9, 1.92, 5.4, 0.4, "测量手段", size=16, color=INK, bold=True)
tb(s, 0.9, 2.4, 5.45, 2.7,
   "① 指标 CLOCK_PROCESS_CPUTIME_ID：只统计本进程搬运耗 CPU 时间，隔离调度/并发噪声\n\n"
   "② 2 MB 大块搬运：避开单次提交固定开销（~4.8 µs/次），测的是搬运本身\n\n"
   "③ dedicated 单队列：消除多任务竞争\n\n"
   "④ disk 场景：真实磁盘挂载点（非 tmpfs），读/写真实走文件系统",
   size=12.5, color=INK, spacing=1.12)
rect(s, 6.8, 1.75, 5.95, 3.5, fill=CARD)
tb(s, 7.1, 1.92, 5.4, 0.4, "稳定性依据", size=16, color=INK, bold=True)
tb(s, 7.1, 2.4, 5.45, 2.7,
   "① repeat=20，报 avg / stddev / min / max 全维度\n\n"
   "② 各段 stddev/avg < 4%（磁盘写最大 5.0 ms 仅 ~3%）\n\n"
   "③ 三条路径「拷贝段节省 22.2 ms/GB」恒等，交叉验证一致\n\n"
   "④ 结论只对「CPU 释放」成立，不夸大",
   size=12.5, color=INK, spacing=1.12)
rect(s, 0.6, 5.45, 12.15, 1.2, fill=BADBG)
tb(s, 0.9, 5.6, 11.6, 0.9,
   "口径边界（需标注）：disk_read/write 的 wall≈cpu，说明命中 page cache，是「CPU 成本口径」而非冷盘吞吐；\n"
   "因此 10%~13% 是「CPU 时间释放」，不等同于端到端墙钟加速或冷盘 IOPS 提升。",
   size=13, color=INK, spacing=1.15)
footer(s)

# ============================================================ Slide 5 分隔 · 场景一
divider("场景一 · mem2mem", "纯内存拷贝 · 能力上限", "不含磁盘 I/O，DSA 能省多少就给多少 —— 理论上限")

# ============================================================ Slide 6 mem2mem
s = slide(); bg(s, WHITE)
kicker_title(s, "场景一 · mem2mem", "异步卸载：拷贝 CPU 降低 89.8%")
add_img(s, FIG2, 0.6, 1.75, w=6.9)
tb(s, 7.75, 2.0, 4.9, 0.6, "82.9 → 8.5", size=34, color=RED, bold=True)
tb(s, 7.75, 2.7, 4.9, 0.4, "ms/GB · 异步提交 + sleep，CPU 释放 89.76%", size=14, color=MUTED)
rect(s, 7.75, 3.35, 4.9, 1.5, fill=CARD)
tb(s, 8.0, 3.5, 4.4, 1.2, "关键前提：必须 batch 攒批 + 异步提交；同步 busy-poll 只省 CPU 时间（-39.7%），不真正让出核。", size=13, color=INK, spacing=1.1)
rect(s, 7.75, 5.05, 4.9, 1.5, fill=CARD)
tb(s, 8.0, 5.2, 4.4, 1.2, "这是能力上限；真实快照链路含磁盘 I/O，达不到这么高。", size=13, color=INK, spacing=1.1)
footer(s)

# ============================================================ Slide 7 分隔 · 场景二/三（模拟链路）
divider("场景二/三 · 快照链路模拟", "mem2disk & disk2mem",
        "模拟真实链路形态：memcpy + 磁盘读写；DSA 只卸载其中的「内存拷贝段」")

# ============================================================ Slide 8 核心成果（模拟链路）
s = slide(); bg(s, WHITE)
kicker_title(s, "场景二/三 · 模拟链路成果", "端到端搬运 CPU 降低 10.3% ~ 12.7%")
add_img(s, FIG6, 0.6, 1.7, w=7.0)
tb(s, 7.95, 2.0, 4.8, 0.6, "22.2 ms/GB", size=32, color=RED, bold=True)
tb(s, 7.95, 2.7, 4.8, 0.4, "绝对节省（拷贝段 56.0→33.8）", size=13, color=MUTED)
rect(s, 7.95, 3.3, 4.8, 0.85, fill=GOODBG)
tb(s, 8.2, 3.48, 4.4, 0.5, "disk2mem（恢复）174.6→152.4  -12.73%", size=15, color=GOOD, bold=True)
rect(s, 7.95, 4.3, 4.8, 0.85, fill=GOODBG)
tb(s, 8.2, 4.48, 4.4, 0.5, "mem2disk（生成）214.8→192.6  -10.34%", size=15, color=GOOD, bold=True)
rect(s, 7.95, 5.3, 4.8, 1.0, fill=CARD)
tb(s, 8.2, 5.45, 4.4, 0.7, "repeat=20，各段 stddev/avg < 4%。（模拟链路 dsa_disk_scene.c）", size=13, color=MUTED)
footer(s)

# ============================================================ Slide 9 为什么只有 10~13%（模拟链路口径）
s = slide(); bg(s, WHITE)
kicker_title(s, "拆解 · 为什么只有 10%~13%", "端到端收益受非拷贝段占比限制")
rows = [
    ("分段", "每 GB CPU (ms)", "性质"),
    ("CPU memcpy（现状拷贝段）", "56.0", "DSA 可卸载"),
    ("DSA memcpy（busy-poll）", "33.8", "卸载后 -39.7%"),
    ("磁盘读 read（disk2mem 段）", "118.6", "非 DSA 覆盖"),
    ("磁盘写 write（mem2disk 段）", "158.9", "非 DSA 覆盖"),
]
styled_table(s, rows, 0.6, 1.8, 12.1, 3.3, [5.6, 3.1, 3.4],
             highlight={(1, 2): GOOD, (2, 2): GOOD, (3, 2): MID, (4, 2): MID})
rect(s, 0.6, 5.45, 12.1, 1.05, fill=CARD)
tb(s, 0.9, 5.65, 11.5, 0.7,
   "端到端收益 = 拷贝段占比（26% ~ 32%） × 拷贝段释放率（39.7%） ≈ 10% ~ 13%（模拟链路口径，真实链路见 A 臂）",
   size=16, color=INK, bold=True)
footer(s)

# ============================================================ Slide 10 不可卸载段分析
s = slide(); bg(s, WHITE)
kicker_title(s, "边界 · DSA 覆盖范围", "读写路径与 metadata 管理不在 DSA 覆盖范围")
rect(s, 0.6, 1.75, 5.7, 4.9, fill=CARD)
tb(s, 0.9, 1.92, 5.2, 0.4, "read()/write() 走内核存储栈", size=16, color=INK, bold=True)
tb(s, 0.9, 2.42, 5.25, 4.0,
   "VFS → ext4 → block layer → 页缓存(page cache) → 块设备驱动 → NVMe/磁盘\n\n"
   "• DSA 是「内存↔内存」DMA 引擎，只做数据搬运，没有文件系统语义\n\n"
   "• ext4 元数据 / 块管理 / 页缓存映射是内核 CPU 代码\n\n"
   "• 缺页、COW、页表遍历、锁、状态机也都是 CPU 代码\n\n"
   "→ 观察：DSA 覆盖「拷贝」这一跳，存储栈其余不在其操作范围",
   size=12.5, color=INK, spacing=1.14)
tb(s, 6.6, 1.92, 6.1, 0.4, "真实 snapshot 链路 perf 符号构成（实测）", size=15, color=INK, bold=True)
tb(s, 6.6, 2.3, 6.1, 0.3, "来自 AgentENV snapshot 链路 perf 采样（orchestrator + ublk）", size=10.5, color=MUTED)
sym_rows = [
    ("符号", "占比", "DSA 覆盖"),
    ("ext4 文件系统逻辑", "~55%", "否"),
    ("页表遍历 + 元数据 + 状态机", "~32%", "否"),
    ("copy_page_to_iter（读 dirty 页）", "10.18%", "是"),
    ("copy_page_from_iter_atomic（写 page cache）", "2.46%", "是"),
]
styled_table(s, sym_rows, 6.6, 2.7, 6.15, 3.8, [3.35, 1.35, 1.45],
             highlight={(1, 2): MID, (2, 2): MID, (3, 2): GOOD, (4, 2): GOOD})
footer(s)

# ============================================================ Slide 11 真实链路 · A 臂基线
s = slide(); bg(s, WHITE)
kicker_title(s, "真实链路 · A 臂基线（2026-10-14）", "AgentENV 实测：停顿含 ~1s 固定等待，搬运 CPU 674 ms/GB")
add_img(s, FIG7, 0.55, 1.95, w=7.1)
rect(s, 7.85, 1.8, 4.9, 2.35, fill=CARD)
tb(s, 8.1, 1.95, 4.4, 0.35, "测试方法", size=15, color=INK, bold=True)
tb(s, 8.1, 2.35, 4.45, 1.7,
   "对象：真实 AgentENV sandbox，整轮复用\n"
   "触发：aenv pause；dirty 写 /dev/shm\n"
   "指标：wall + server CPU（perf task-clock）\n"
   "repeat=20/档，剔 perf record 行",
   size=11.5, color=INK, spacing=1.12)
rect(s, 7.85, 4.35, 4.9, 2.35, fill=GOODBG)
tb(s, 8.1, 4.5, 4.4, 0.35, "结论（对评估目标）", size=15, color=GOOD, bold=True)
tb(s, 8.1, 4.9, 4.45, 1.7,
   "① 停顿含 ~1s 与 dirty 无关的固定等待（CPU≈0）\n    ——推测与 DSA 无关，另线定位\n"
   "② 搬运 CPU 随 dirty 线性（674 ms/GB）\n    ——memory copy 子路径是 DSA 直接优化对象\n"
   "③ 端到端收益受 filesystem/control-plane 占比限制\n    （拷贝符号 ~12.6%，B 臂预期 9~25%）",
   size=11.5, color=INK, spacing=1.05)
footer(s)

# ============================================================ Slide 12 真实链路 · 目标口径
s = slide(); bg(s, WHITE)
kicker_title(s, "真实链路 · 目标口径", "vCPU 释放 5~10% 的达标路径")
rect(s, 0.6, 1.7, 12.15, 0.9, fill=CARD)
tb(s, 0.9, 1.88, 11.6, 0.6,
   "vCPU 可用资源提升 = 搬运占 vCPU 总负载 × DSA 可卸载占搬运 CPU 的比例（保守 ~12% / 乐观 ~25%）",
   size=15, color=INK, bold=True)
rows = [
    ("搬运占 vCPU 总负载", "可卸载 12%（保守）", "可卸载 25%（乐观）"),
    ("5%", "0.6%", "1.3%"),
    ("10%", "1.2%", "2.5%"),
    ("20%", "2.4%", "5.0%"),
    ("40%", "4.8%", "10.0%"),
]
styled_table(s, rows, 0.6, 2.8, 12.15, 2.25, [4.05, 4.05, 4.05],
             highlight={(3, 1): INK, (3, 2): GOOD, (4, 1): INK, (4, 2): GOOD})
rect(s, 0.6, 5.3, 5.95, 1.35, fill=CARD)
tb(s, 0.9, 5.45, 5.4, 1.1,
   "达标条件：重快照节奏（搬运占比 20~40%）× 乐观可卸载。一半取决于生产参数（快照周期 × dirty 量 × 密度），待确认。",
   size=12, color=INK, spacing=1.1)
rect(s, 6.8, 5.3, 5.95, 1.35, fill=GOODBG)
tb(s, 7.1, 5.45, 5.4, 1.1,
   "下一步：B 臂实测可卸载比例（预期 pause CPU 降 9~25%）；C 臂（O_DIRECT）另测写盘段（371 vs 85 ms/GB），归因分开。",
   size=12, color=INK, spacing=1.1)
footer(s)

# ============================================================ Slide 13 结论与前提
s = slide(); bg(s, WHITE)
kicker_title(s, "结论 · 前提", "结论与必须说明的前提")
rect(s, 0.6, 1.8, 5.95, 3.9, fill=GOODBG)
tb(s, 0.95, 2.0, 5.3, 0.4, "结论", size=18, color=GOOD, bold=True)
tb(s, 0.95, 2.5, 5.3, 3.0,
   "模拟链路：搬运 CPU 降低 10%~13%（22.2 ms/GB）\n\n"
   "真实链路：搬运 CPU ≈ 674 ms/GB（A 臂锚点）\n\n"
   "价值口径：释放 vCPU —— 停顿时长受固定等待约束（与 DSA 解耦）",
   size=14, color=INK, spacing=1.2)
rect(s, 6.78, 1.8, 5.95, 3.9, fill=BADBG)
tb(s, 7.13, 2.0, 5.3, 0.4, "前提（需明确标注）", size=18, color=MID, bold=True)
tb(s, 7.13, 2.5, 5.3, 3.0,
   "① 端到端 A/B 验证进行中：A 臂基线已完成，B 臂（DSA 接入）待跑\n\n"
   "② 磁盘 I/O 段为 CPU 口径 / page cache 命中，非冷盘吞吐\n\n"
   "③ 10%~13% 为 busy-poll 保守值，异步提交有望更高",
   size=14, color=INK, spacing=1.15)
footer(s)

# ============================================================ Slide 14 结语
s = slide(); bg(s, RED)
tb(s, 0, 2.9, 13.333, 1.2, "谢谢", size=54, color=WHITE, bold=True, align=PP_ALIGN.CENTER)
tb(s, 0, 4.4, 13.333, 0.5, "DSA 在大粒度异步批量搬运场景具备 CPU 卸载价值 · B 臂端到端验证进行中（A 臂已锚定 674 ms/GB）",
   size=16, color=SOFT, align=PP_ALIGN.CENTER)

# ============================================================ Slide 15 附录分隔
divider("附录 · Appendix", "补充图表与原始数据", "能力关卡图（含图注与读法）+ 关键原始数据，供复核")

# ============================================================ Slide 16 附录A · 盈亏平衡（含图注）
s = slide(); bg(s, WHITE)
kicker_title(s, "附录 A · 能力关卡", "图1  CPU vs DSA 单发吞吐与盈亏平衡")
add_img(s, FIG1, 0.6, 1.85, w=7.2)
anno_card(s, 8.0, 1.85, 4.75, 4.75,
          "单发（dedicated 队列）下 CPU 与 DSA 的 memcpy / fill 吞吐对比，单次搬运尺寸扫 4KB ~ 2MB。",
          "横轴 = 单次搬运尺寸（对数）；纵轴 = 吞吐 (GiB/s)。四条线 = CPU/DSA × memcpy/fill。",
          "盈亏平衡 ≈ 1MB：4KB 单发 DSA 慢 78×（固定提交开销 ~4.8µs）；2MB 反超（27.4 vs 16.4 Gi/s，CPU 跨 NUMA 掉速）。→ 小页操作需攒 batch 才划算。")
footer(s)

# ============================================================ Slide 17 附录B · 批量 + 散落页（含图注）
s = slide(); bg(s, WHITE)
kicker_title(s, "附录 B · 批量效率", "图3  4KB Batch 打包   ·   图4  散落页 vs 连续页")
add_img(s, FIG3, 0.6, 1.8, w=5.9)
add_img(s, FIG4, 6.85, 1.8, w=5.9)
rect(s, 0.6, 5.35, 5.9, 1.5, fill=CARD)
tb(s, 0.85, 5.48, 5.4, 0.3, "图3 · 4KB 页操作 Batch 打包（冷数据）", size=12.5, color=RED, bold=True)
tb(s, 0.85, 5.82, 5.45, 0.95,
   "横轴 = 一个 batch 打包的 4KB 页数 N。阈值 N≈48：N≤32 未真正批量；≥48 吞吐跳变 4.6 → 43.9 GB/s，反超 CPU 冷拷贝 5~11×。",
   size=11, color=INK, spacing=1.1)
rect(s, 6.85, 5.35, 5.9, 1.5, fill=CARD)
tb(s, 7.1, 5.48, 5.4, 0.3, "图4 · 散落页 vs 连续页 Batch 吞吐（冷页）", size=12.5, color=RED, bold=True)
tb(s, 7.1, 5.82, 5.45, 0.95,
   "snapshot 的 dirty 页物理散落。N≥48 时散落/连续 ≈ 1.0（散落页未见吞吐损失）；5~11× 仅在冷页观察到，热页缩水至 1~2.5×。",
   size=11, color=INK, spacing=1.1)
footer(s)

# ============================================================ Slide 18 附录C · snapshot 搬运（含图注）
s = slide(); bg(s, WHITE)
kicker_title(s, "附录 C · 真实搬运路径", "图5  AgentENV snapshot 搬运路径的成本与卸载收益")
add_img(s, FIG5, 0.6, 1.85, w=7.2)
anno_card(s, 8.0, 1.85, 4.75, 4.75,
          "snapshot 搬运链路成本量化：左图 = 三种路径每 GB CPU 成本（对数轴）；右图 = DSA 卸载 memory copy 的 CPU 下降比例。",
          "左图纵轴为对数（942 与 8.5 相差百倍）；右图负值 = 反而更慢。",
          "真实 snapshot 搬运 942 ms/GB，是 CPU memcpy 的 11×；DSA 卸载 4KB 页（busy-poll）CPU 降 78.4%。「4KB + usleep 的 -37%」经对照实验确认源于 usleep 系统调用开销。")
footer(s)

# ============================================================ Slide 19 附录D · 原始数据
s = slide(); bg(s, WHITE)
kicker_title(s, "附录 D · 原始数据", "mem2disk / disk2mem 实测明细（repeat=20）")
hdr = ["segment", "avg ms/GB", "stddev", "min", "max", "wall s/GB"]
segs = [
    ("cpu_memcpy", "56.0", "0.2", "55.8", "56.9", "0.0560"),
    ("dsa_memcpy_busypoll", "33.8", "0.4", "33.6", "35.6", "0.0338"),
    ("disk_read", "118.6", "3.2", "115.8", "124.7", "0.1189"),
    ("disk_write", "158.9", "5.0", "157.3", "179.9", "0.1590"),
]
styled_table(s, [hdr] + segs, 0.6, 1.85, 12.1, 2.4, [3.6, 1.9, 1.6, 1.6, 1.6, 1.8])
tb(s, 0.6, 4.55, 12.0, 0.35, "端到端场景（现状 vs DSA 卸载拷贝段）", size=14, color=INK, bold=True)
hdr2 = ["scenario", "current ms/GB", "DSA 卸载 ms/GB", "节省 %", "拷贝段占比 %", "拷贝段 DSA 释放 %"]
scens = [
    ("disk2mem（读盘 + memcpy）", "174.6", "152.4", "12.73", "32.1", "39.68"),
    ("mem2disk（memcpy + 写盘）", "214.8", "192.6", "10.34", "26.1", "39.68"),
]
styled_table(s, [hdr2] + scens, 0.6, 4.95, 12.1, 1.5, [3.6, 1.9, 1.6, 1.6, 1.6, 1.8])
tb(s, 0.6, 6.55, 12.0, 0.4,
   "口径：CLOCK_PROCESS_CPUTIME_ID 每 GB CPU 时间；DSA 只卸载「内存拷贝段」，磁盘 I/O 段不可卸载。",
   size=11, color=MUTED)
footer(s)

try:
    prs.save(OUT)
    print("OK", OUT)
except PermissionError:
    OUT2 = OUT.replace(".pptx", "_v2.pptx")
    prs.save(OUT2)
    print("OK (原文件被 PowerPoint 占用，已另存为)", OUT2)
