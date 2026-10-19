from pathlib import Path
from docx import Document
from docx.shared import Cm, Pt, RGBColor
from docx.enum.text import WD_ALIGN_PARAGRAPH
from docx.enum.table import WD_TABLE_ALIGNMENT, WD_CELL_VERTICAL_ALIGNMENT
from docx.oxml import OxmlElement
from docx.oxml.ns import qn

BASE = Path(r'D:\workSpace\DSA-')
OUT = BASE / 'report' / 'AgentENV_snapshot_DSA加速评估汇报_20261019.docx'
doc = Document()
sec = doc.sections[0]
sec.page_width, sec.page_height = Cm(29.7), Cm(21)
sec.top_margin, sec.bottom_margin = Cm(1.3), Cm(1.3)
sec.left_margin, sec.right_margin = Cm(1.6), Cm(1.6)
for name in ['Normal', 'Title', 'Subtitle', 'Heading 1', 'Heading 2', 'Caption']:
    st = doc.styles[name]
    st.font.name = 'Microsoft YaHei'
    st._element.get_or_add_rPr().rFonts.set(qn('w:eastAsia'), 'Microsoft YaHei')
    st.font.color.rgb = RGBColor(0, 0, 0)
normal = doc.styles['Normal']
normal.font.size = Pt(10)
normal.paragraph_format.line_spacing = 1.12
normal.paragraph_format.space_after = Pt(5)
doc.styles['Title'].font.size = Pt(21)
doc.styles['Title'].paragraph_format.space_after = Pt(5)
doc.styles['Heading 1'].font.size = Pt(12)
doc.styles['Heading 1'].paragraph_format.space_before = Pt(9)
doc.styles['Heading 1'].paragraph_format.space_after = Pt(5)

def para(text, boldlead=None, size=None):
    p=doc.add_paragraph()
    if boldlead and text.startswith(boldlead):
        p.add_run(boldlead).bold=True
        p.add_run(text[len(boldlead):])
    else: p.add_run(text)
    if size:
        for r in p.runs: r.font.size=Pt(size)
    return p

doc.add_paragraph('AgentENV snapshot 的 DSA 加速评估', 'Title')
para('测试结果与适配可行性汇报    2026年10月19日', size=9)
para('结论：本轮测试表明，DSA 的 CPU 收益取决于搬运场景与接入方式：mem2mem 参照臂的 onCPU 从4.983ms降至2.270ms；mem2disk、disk2mem 新增缓冲方案则分别增加10.4%、10.7%，AgentENV 真实 snapshot 的端到端收益尚未验证。', '结论：')

doc.add_heading('适配思路与测试数据', level=1)
para('当前 AgentENV 路径尚未适配 DSA，搬运由内核完成，没有可直接替换的用户态 memcpy 跳。本轮对比以下两种适配思路。')
para('方案一 直接替换：假设通过路径改造让 DSA 接管已有搬运且不增加拷贝；结果按“软件基线－软件纯搬运＋DSA纯搬运”合成推算，尚未实现。', '方案一 直接替换：')
para('方案二 新增缓冲：软件一步直接 write(src) 或 read(dst)；DSA 两步为“DSA(src→缓冲)＋write(缓冲)”或“read(缓冲)＋DSA(缓冲→dst)”。原内核拷贝保留，额外增加一次 DSA 搬运。', '方案二 新增缓冲：')

headers=['方案或比较方式','场景','软件 onCPU\nms/次','DSA onCPU\nms/次','CPU 成本变化','数据性质']
rows=[
['本轮纯搬运参照臂','mem2mem','4.983','2.270','降低54.5%','实测'],
['方案一 假设直接替换已有搬运','mem2disk','19.394','16.682','降低14.0%','合成推算'],
['方案一 假设直接替换已有搬运','disk2mem','20.502','17.789','降低13.2%','合成推算'],
['方案二 软件一步 vs DSA新增缓冲两步','mem2disk','19.394','21.419','增加10.4%','微基准实测'],
['方案二 软件一步 vs DSA新增缓冲两步','disk2mem','20.502','22.699','增加10.7%','微基准实测'],
]
table=doc.add_table(rows=1, cols=6)
table.alignment=WD_TABLE_ALIGNMENT.CENTER
table.autofit=False
widths=[9.4,3.2,3.1,3.1,3.8,3.9]
for c,w in zip(table.columns,widths): c.width=Cm(w)
for c,t in zip(table.rows[0].cells,headers): c.text=t
for row in rows:
    for c,t in zip(table.add_row().cells,row): c.text=t
for i,row in enumerate(table.rows):
    for j,c in enumerate(row.cells):
        c.width=Cm(widths[j]); c.vertical_alignment=WD_CELL_VERTICAL_ALIGNMENT.CENTER
        tcPr=c._tc.get_or_add_tcPr()
        borders=OxmlElement('w:tcBorders')
        for side in ['top','left','bottom','right']:
            e=OxmlElement('w:'+side); e.set(qn('w:val'),'single'); e.set(qn('w:sz'),'4'); e.set(qn('w:color'),'D9D9D9'); borders.append(e)
        tcPr.append(borders)
        sh=OxmlElement('w:shd'); sh.set(qn('w:fill'),'DCE6F1' if i==0 else ('F4F6F8' if i%2==0 else 'FFFFFF')); tcPr.append(sh)
        margins=OxmlElement('w:tcMar')
        for side in ['top','bottom','left','right']:
            e=OxmlElement('w:'+side); e.set(qn('w:w'),'85'); e.set(qn('w:type'),'dxa'); margins.append(e)
        tcPr.append(margins)
        for p in c.paragraphs:
            p.alignment=WD_ALIGN_PARAGRAPH.LEFT if j==0 else WD_ALIGN_PARAGRAPH.CENTER
            p.paragraph_format.space_after=Pt(0); p.paragraph_format.line_spacing=1.05
            for r in p.runs: r.font.size=Pt(9); r.bold=i==0
    trPr=row._tr.get_or_add_trPr(); trPr.append(OxmlElement('w:cantSplit'))
repeat=OxmlElement('w:tblHeader'); table.rows[0]._tr.get_or_add_trPr().append(repeat)
para('测试条件：每次60MiB（脚本标记60MB）、每臂15次、窗口前冲刷LLC；onCPU 为进程 CPU 时间，不是端到端耗时。54.5%仅适用于本轮 mem2mem 参照条件，不推广到其他场景。', size=9)

doc.add_heading('验证边界', level=1)
para('原定通过透明插桩完成真实 snapshot 的 DSA 替换验证，在不改源码的约束下受阻：用户态 DML 无法直接接管内核搬运，LD_PRELOAD 无法覆盖 Go 裸 syscall，ptrace 截获也未解决数据访问与替换问题；实测 pause 还会解除 guest RAM 的 COW 映射。因此，本轮交付为真实路径观测、微基准实测与条件性推算，尚未形成真实 snapshot 接入 DSA 的端到端 A/B 结果。')

doc.add_page_break()
doc.add_heading('真实 pause 构成与测量依据', level=1)
p=doc.add_paragraph(); p.paragraph_format.space_after=Pt(4)
p.add_run().add_picture(str(BASE/'report'/'figures'/'fig7_pause构成.png'), width=Cm(26.5))
para('图解：左图为真实 pause 的 CPU 样本构成，搬运占13.85%；中图回归得到约108.3ms固定项与516±74ms/GiB斜率（±74为脚本计算的近似95%置信区间半宽）；右图展示条件性收益外推与新增缓冲的成本。图中7%～8%相对真实 pause 的数据量相关成本，与表中13%～14%的微基准分母不同。', '图解：', size=9)
para('可信依据：同一沙箱循环 resume→在 guest /dev/shm 制脏→等待1秒→aenv pause；制脏与 resume 在采样窗外。A段按轮交错0/64/256/1024MiB，共80事件，用 perf stat 测 CPU 总量；B段另用 perf record -F4000 测60个成功事件的符号构成，共87024样本。探针 pause 筛选活跃 PID，检查内核采样和符号解析，隔离HTTP500事件，并记录前后负载及共存VM数。', '可信依据：', size=9)
para('干扰边界：测量不改 AgentENV 源码和原搬运逻辑，但 perf 有采样开销，共享进程可能含其他请求；上述措施减少污染，不等于零扰动或完全隔离。', '干扰边界：', size=9)
para('数据依据：measure_copy_share.sh；dsa_disk_scene.c；report/generate_disk_scene_xlsx.py；report/generate_pause_cpu_fig.py。', size=8)

doc.core_properties.title='AgentENV snapshot 的 DSA 加速评估'
doc.core_properties.subject='2026年10月19日测试结果与适配可行性汇报'
doc.core_properties.author=''
doc.save(OUT)
print(OUT)
