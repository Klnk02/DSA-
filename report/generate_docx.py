# -*- coding: utf-8 -*-
"""生成 Intel DSA 沙箱加速能力评估 - 预研报告 (.docx)
运行: python generate_docx.py
输出: Intel DSA 沙箱加速能力评估-预研报告.docx
"""
from docx import Document
from docx.shared import Pt, Mm, RGBColor
from docx.enum.text import WD_ALIGN_PARAGRAPH
from docx.oxml.ns import qn
from docx.oxml import OxmlElement

# ---------- 颜色 ----------
INK = RGBColor(0x1A, 0x22, 0x33)
MUTED = RGBColor(0x5B, 0x66, 0x75)
ACCENT = RGBColor(0x2F, 0x6F, 0xED)
GREEN = RGBColor(0x0E, 0x9F, 0x8A)
WHITE = RGBColor(0xFF, 0xFF, 0xFF)

EAST = "微软雅黑"
ASCII = "Calibri"


def set_run(run, size=10.5, bold=False, color=None, italic=False, east=EAST, ascii_font=ASCII):
    run.font.name = ascii_font
    run.font.size = Pt(size)
    run.font.bold = bold
    run.font.italic = italic
    if color is not None:
        run.font.color.rgb = color
    rpr = run._element.get_or_add_rPr()
    rfonts = rpr.find(qn('w:rFonts'))
    if rfonts is None:
        rfonts = OxmlElement('w:rFonts')
        rpr.append(rfonts)
    rfonts.set(qn('w:ascii'), ascii_font)
    rfonts.set(qn('w:hAnsi'), ascii_font)
    rfonts.set(qn('w:eastAsia'), east)


def add_para(text, size=10.5, bold=False, color=None, align=None, before=0, after=6, italic=False):
    p = doc.add_paragraph()
    run = p.add_run(text)
    set_run(run, size=size, bold=bold, color=color, italic=italic)
    if align is not None:
        p.alignment = align
    p.paragraph_format.space_before = Pt(before)
    p.paragraph_format.space_after = Pt(after)
    return p


def add_heading(text, level=1):
    p = doc.add_heading(level=level)
    run = p.add_run(text)
    size = 15 if level == 1 else 12.5
    set_run(run, size=size, bold=True, color=INK)
    p.paragraph_format.space_before = Pt(14 if level == 1 else 10)
    p.paragraph_format.space_after = Pt(6)
    return p


def shade(cell, fill):
    tcPr = cell._tc.get_or_add_tcPr()
    shd = OxmlElement('w:shd')
    shd.set(qn('w:val'), 'clear')
    shd.set(qn('w:color'), 'auto')
    shd.set(qn('w:fill'), fill)
    tcPr.append(shd)


def make_table(headers, rows, header_fill="2F6FED", highlight_rows=None):
    highlight_rows = highlight_rows or []
    table = doc.add_table(rows=1, cols=len(headers))
    table.style = 'Table Grid'
    # 表头
    hdr = table.rows[0].cells
    for i, h in enumerate(headers):
        cell = hdr[i]
        cell.text = ''
        run = cell.paragraphs[0].add_run(h)
        set_run(run, size=9.5, bold=True, color=WHITE)
        shade(cell, header_fill)
    # 数据行
    for ridx, row in enumerate(rows):
        cells = table.add_row().cells
        for i, v in enumerate(row):
            cell = cells[i]
            cell.text = ''
            run = cell.paragraphs[0].add_run(str(v))
            set_run(run, size=9.5)
            if ridx in highlight_rows:
                shade(cell, "E8F5F1")
    return table


def add_bullet(text, bold_prefix=None):
    p = doc.add_paragraph(style='List Bullet')
    if bold_prefix:
        r1 = p.add_run(bold_prefix)
        set_run(r1, size=10.5, bold=True, color=INK)
    r2 = p.add_run(text)
    set_run(r2, size=10.5)
    p.paragraph_format.space_after = Pt(3)
    return p


def add_numbered(text, bold_prefix=None):
    p = doc.add_paragraph(style='List Number')
    if bold_prefix:
        r1 = p.add_run(bold_prefix)
        set_run(r1, size=10.5, bold=True, color=INK)
    r2 = p.add_run(text)
    set_run(r2, size=10.5)
    p.paragraph_format.space_after = Pt(4)
    return p


def add_callout(title, body, green=False):
    color = GREEN if green else ACCENT
    p = doc.add_paragraph()
    run = p.add_run(title)
    set_run(run, size=10.5, bold=True, color=color)
    p.paragraph_format.space_before = Pt(6)
    p.paragraph_format.space_after = Pt(2)
    p2 = doc.add_paragraph()
    run2 = p2.add_run(body)
    set_run(run2, size=10.5)
    p2.paragraph_format.space_after = Pt(8)
    return p2


# ============================================================
doc = Document()

# A4 页面
section = doc.sections[0]
section.page_width = Mm(210)
section.page_height = Mm(297)
section.top_margin = Mm(22)
section.bottom_margin = Mm(22)
section.left_margin = Mm(24)
section.right_margin = Mm(24)

# Normal 默认字体
normal = doc.styles['Normal']
normal.font.name = ASCII
normal.font.size = Pt(10.5)
normal._element.get_or_add_rPr()
rfonts = normal._element.get_or_add_rPr().get_or_add_rFonts()
rfonts.set(qn('w:eastAsia'), EAST)

# ============ 封面 ============
add_para("预研报告 · TECHNICAL EVALUATION", size=9, bold=True, color=ACCENT, after=10)
add_para("Intel DSA 沙箱加速能力评估", size=24, bold=True, color=INK, after=10)
add_para("基于 Intel Sapphire Rapids（Xeon Platinum 8480+）实测，量化「数据搬移硬件卸载」对沙箱关键路径的收益，作为鲲鹏（ARM）是否实现 DSA 等价功能的决策依据。",
         size=11, color=MUTED, after=14)
add_para("日期：2026-10-12    阶段：0–3 摸底    环境：8480+ · 224 vCPU · 2 NUMA · Linux 6.14",
         size=9, color=MUTED, after=20)

# ============ 摘要 ============
add_heading("摘要", 1)
add_para("本报告回答一个核心问题：把 mem2mem 数据搬运卸载到专用加速器，对沙箱（Firecracker 类 microVM）到底值多少？预研设想是鲲鹏 IO die 新增「data stream 加速器」，卸载 mem2mem / disk↔mem 搬运，目标是释放 vCPU 可用资源 5%~10%。")
add_para("")
# 指标表
make_table(
    ["关键指标", "数值", "说明"],
    [
        ["异步卸载 CPU 释放率", "89.76%", "搬运任务自身可释放的 CPU 时间上限"],
        ["Batch 攒批吞吐反超", "5~8×", "≥64 页批量打包后相对 CPU 冷拷贝"],
        ["单发 4KB 相对劣势", "78×", "单发小尺寸 DSA 相对 CPU 的负收益"],
        ["单发盈亏平衡尺寸", "≈1MB", "单发吞吐打平点，小尺寸负收益"],
    ],
)
add_para("")
add_callout("结论一句话：", "DSA 对沙箱的价值成立，但前提是「Batch 攒批 + 异步卸载」两者兼备——单发小尺寸是负收益，只有批量提交并让 CPU 异步空出，才能把 mem2mem 搬运的价值兑现。", green=True)

# ============ 一、背景 ============
add_heading("一、背景与目的", 1)
add_para("鲲鹏（Kunpeng / ARM）目前没有 Intel DSA 这类「内存操作卸载引擎」（memcpy / memfill / compare / delta / CRC 的硬件加速）。鲲鹏 KAE 已覆盖加解密与 zlib/gzip 压缩，缺口集中在数据搬移与内存操作。")
add_para("预研设想：在鲲鹏 IO die 新增 data stream 加速器，卸载两类搬运——mem2mem（内存到内存）与 disk2mem / mem2disk（磁盘与内存之间），以释放 CPU 资源、提升 vCPU 可用 CPU 资源 5%~10%。")
add_para("本项目的做法是：把 Intel DSA 当作「价值标尺」，在 Sapphire Rapids 机器上实测 DSA 的卸载收益，用数据支撑「鲲鹏要不要做、做哪些操作、软件能否替代」的决策。本次交付覆盖摸底第 0–3 阶段：环境探测、设备配置、吞吐基准、CPU 释放率、4KB 批量打包。")

# ============ 二、环境 ============
add_heading("二、测试环境", 1)
make_table(
    ["类别", "配置"],
    [
        ["CPU", "Intel Xeon Platinum 8480+（Sapphire Rapids），224 vCPU，2 NUMA"],
        ["DSA", "dsa0 @ NUMA0、dsa2 @ NUMA1（各 4 engine），已启用"],
        ["工作队列", "每颗 DSA 配 1 个 dedicated + 1 个 shared（ENQCMD/SVA）队列"],
        ["内核", "Linux 6.14，CONFIG_INTEL_IDXD=m、CONFIG_INTEL_IDXD_SVM=y"],
        ["指令集", "支持 enqcmd / movdir64b（DSA 两种提交方式）"],
        ["用户态库", "accel-config 4.1.8（DSA 配置）、Intel DML v1.2（数据搬运库，高层 C++ / 低层 C API）"],
    ],
)
add_para("注：DML 硬件路径运行时依赖 libaccel-config.so，需以 LD_LIBRARY_PATH 指定；iommu=pt 会导致 PASID 挂载失败，内核参数须为 intel_iommu=on,sm_on。", size=9, color=MUTED, before=4)

# ============ 三、方法 ============
add_heading("三、测试方法", 1)
add_para("围绕「吞吐 / 延迟 / CPU 占用」三个维度，用三种定制程序覆盖不同卸载方式：")
make_table(
    ["程序", "接口", "测什么", "关键指标"],
    [
        ["dsa_bench.cc", "DML 高层 C++ + Google Benchmark", "CPU vs DSA 逐尺寸吞吐对比", "吞吐（GB/s）、盈亏平衡点"],
        ["dsa_async_offload.c", "DML 低层 C（dml_submit_job）", "真异步卸载的 CPU 释放", "每 GB CPU 时间、释放率"],
        ["dsa_batch_4k.c", "DML 低层 C（batch 接口）", "4KB 页操作批量打包", "batch 大小 vs 吞吐"],
    ],
)
add_para("核心测量原则：「每 GB CPU 时间」（搬运 1GB 数据，CPU 真正忙的时长，用 CLOCK_PROCESS_CPUTIME_ID 纳秒计时）是评估「释放」的干净指标——它不受 sleep/等待方式干扰，直接反映「CPU 花在搬运上的算力成本」。", before=6)
add_para("释放率 = (CPU 路径每 GB CPU 时间 − DSA 异步路径每 GB CPU 时间) / CPU 路径每 GB CPU 时间",
         align=WD_ALIGN_PARAGRAPH.CENTER, before=4, after=8, bold=True)

# ============ 四、结果 ============
add_heading("四、结果与分析", 1)

add_heading("4.1 吞吐与盈亏平衡", 2)
add_para("逐尺寸扫描 4KB–2MB，单发（1 个描述符搬 1 块数据）对比 glibc memcpy 与 DSA 硬件搬运的吞吐。DSA 每次提交有固定开销约 4.8µs（构造描述符 + 写提交 + 等完成），这决定了小尺寸下的严重劣势。")
make_table(
    ["尺寸", "CPU memcpy (GiB/s)", "DSA 单发 (GiB/s)", "盈亏"],
    [
        ["4 KB", "63.4", "0.81", "慢 78×"],
        ["64 KB", "36.1", "9.94", "慢 3.6×"],
        ["256 KB", "35.9", "19.7", "慢 1.8×"],
        ["1 MB", "26.8", "26.2", "打平"],
        ["2 MB", "16.4", "27.4", "反超 1.7×"],
    ],
    highlight_rows=[4],
)
add_callout("结论 4.1：", "单发盈亏平衡 ≈ 1MB。小尺寸（<256KB）DSA 相比 CPU 是负收益；2MB 时 DSA 反超 1.7×，主因是 CPU 跨 NUMA 访问掉速而 DSA 不受影响。沙箱热点（4KB 页操作）单发 DSA 完全不划算，必须批量。", green=True)

add_heading("4.2 CPU 释放率：异步卸载", 2)
add_para("高层的同步接口（dml::execute）内部是 busy-poll——CPU 提交后一直空转等 DSA 完成，占用率仍 ~100%，并未释放。真正的释放需要低层异步接口 dml_submit_job：提交后立即返回，CPU 让出，DSA 在后台搬。")
make_table(
    ["模式", "占用率", "每 GB CPU 时间", "含义"],
    [
        ["CPU memcpy", "99.7%", "82.9 ms", "基准（CPU 全忙）"],
        ["DSA 同步 busy-poll", "100.0%", "35.0 ms", "吞吐翻倍，但 CPU 仍空转"],
        ["DSA 异步 + sleep", "8.2%", "8.5 ms", "真释放"],
        ["DSA 异步 + umwait", "100.0%", "35.1 ms", "只省电，不释放"],
    ],
    highlight_rows=[2],
)
add_callout("结论 4.2：", "低层 dml_submit_job 是真异步：提交后 CPU 让出、DSA 后台搬，搬运占用的 CPU 时间从 82.9ms/GB 降到 8.5ms/GB，释放率 89.76%。而 umwait 只是进入低功耗态、核心仍被占用，只省电不释放 CPU——真释放靠「让出 CPU」，不是「低功耗等待」。", green=True)

add_heading("4.3 4KB 页操作的 Batch 打包", 2)
add_para("既然单发 4KB 慢 78×，问题就变成：把 N 个 4KB 页操作打包成一个 batch、只付一次提交开销，能否挽回？在冷数据（16MB 连续 4KB 块，防 cache 预热假象）下扫描 batch 大小 N。")
make_table(
    ["batch 大小 N", "总数据量", "吞吐 (GB/s)", "vs CPU(7.9)"],
    [
        ["4", "16 KB", "1.62", "0.21×"],
        ["8", "32 KB", "3.08", "0.39×"],
        ["16", "64 KB", "3.80", "0.48×"],
        ["32", "128 KB", "4.43", "0.56×"],
        ["64", "256 KB", "39.77", "5.03×"],
        ["128", "512 KB", "52.61", "6.66×"],
        ["256", "1 MB", "65.28", "8.26×"],
    ],
    highlight_rows=[4, 6],
)
add_callout("结论 4.3：", "存在明显的批量效率阈值 N≈64：N<64 时 DML/DSA 未真正批量（仍逐个提交），吞吐仅 1.6~4.4 GB/s；N≥64 才发挥批量效率，吞吐跳到 39.8 GB/s 起、最多 65.3 GB/s，反超 CPU 冷拷贝 5~8 倍。沙箱的快照/dirty-page/zeropage 天然是海量 4KB 页批量操作，正合适攒批。", green=True)

# ============ 五、综合结论 ============
add_heading("五、综合结论", 1)
add_para("三条贯穿始终的判断，在数据上全部得到验证或修正：")
add_numbered("盈亏平衡存在：单发 DSA 在小尺寸是负收益（4KB 慢 78×，固定开销 ~4.8µs），盈亏平衡约 1MB。「随手卸载小内存操作」不可行。", bold_prefix="")
add_numbered("真正的价值在「CPU 释放」而非「吞吐提速」：大块搬运 DSA 吞吐只是略快/打平，但异步卸载能把搬运占用的 CPU 时间省掉 89.76%——这才是沙箱收益的来源。", bold_prefix="")
add_numbered("小尺寸靠 Batch 攒批兑现：4KB 页操作打包到 ≥64 个，DSA 反超 CPU 冷拷贝 5~8 倍。没有 Batch 能力，加速器对沙箱基本无用。", bold_prefix="")

# ============ 六、建议 ============
add_heading("六、对鲲鹏决策的建议", 1)
add_callout("核心建议：", "判定「鲲鹏补 DSA 等价功能」方向成立，但有硬性前提：加速器必须原生支持 Batch 批量描述符与异步提交。若只做同步单发搬运，在沙箱场景是负收益。", green=True)
add_bullet("优先做 memcpy / memfill（数据搬运与填零，沙箱快照/zeropage 高频命中）；compare/delta 次之（依赖具体 dirty-page 检测实现）。", bold_prefix="优选操作：")
add_bullet("提交链路必须支持异步 + 批量（对标 Intel ENQCMD/MOVDIR64B + Batch descriptor），否则无法释放 CPU、也吃不到小尺寸收益。", bold_prefix="架构红线：")
add_bullet("vCPU 可用资源提升 = 搬运占比 × 89.76%。若搬运占 vCPU 总负载 6%~11%，即可达成 5%~10% 目标——「搬运占比」需在沙箱实测。", bold_prefix="KPI 落地路径：")

# ============ 七、局限 ============
add_heading("七、局限与后续工作", 1)
add_bullet("89.76% 是「搬运任务自身的 CPU 释放上限」，要换算成「vCPU 总资源提升 5%~10%」，还需在沙箱实测 mem2mem 占 vCPU 总负载的比例（阶段 4）。", bold_prefix="搬运占比未测：")
add_bullet("DSA 加速比是「Intel 硬件 vs 同平台 CPU」，外推到鲲鹏需用 NEON/SVE 软件基线做对照，才能量化「软件能补多少、硬件额外价值多少」（阶段 5 软件替代分析）。", bold_prefix="跨架构外推：")
add_bullet("本次 batch 只测了 memcpy，fill/compare 的批量行为需补充验证。", bold_prefix="填充/比较批量：")

# ============ 来源 ============
add_heading("来源与参考", 1)
add_para("1. Intel DML v1.2 文档 — Low-level C API（dml_submit_job / dml_check_job / dml_wait_job 异步接口、Wait Mode 枚举、batch 接口）。https://intel.github.io/DML/documentation/api_docs/low_level_api.html", size=9, color=MUTED)
add_para("2. Intel DML v1.2 文档 — Installation（硬件路径依赖 libaccel-config、DSA 加速器配置、Linux/host 要求）。https://intel.github.io/DML/documentation/get_started_docs/installation.html", size=9, color=MUTED)
add_para("3. 本项目实测数据（阶段 0–3），见代码仓 results/ 目录各 CSV 与 benchmark 源码。d:\\workSpace\\DSA-\\results\\", size=9, color=MUTED)
add_para("本报告数据全部来自 2026-10-12 在 Intel Xeon Platinum 8480+ 上的实测；吞吐单位 GiB/s 与 GB/s 均为二进制度量，差异在图表中已统一标注。", size=9, color=MUTED, italic=True, before=8)

out = r"d:\workSpace\DSA-\report\Intel DSA 沙箱加速能力评估-预研报告.docx"
doc.save(out)
print("saved:", out)
