# Table screenshots with known cells: HTML tables rendered by headless Edge at
# several scale factors, cropped as a user would select them, each with the
# grid it should paste as (truth.json).
import json, os, random, subprocess, sys
from PIL import Image, ImageChops

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "corpus")
EDGE = r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
random.seed(7)

def c(text, rs=1, cs=1, align=None):
    return {"t": text, "rs": rs, "cs": cs, "align": align}

def norm(table):
    return [[x if isinstance(x, dict) else c(str(x)) for x in row] for row in table]

PRODUCTS = ["不锈钢螺栓", "六角螺母", "平垫圈", "弹簧垫圈", "深沟球轴承", "液压油管", "密封圈", "电磁阀", "接近开关", "伺服电机", "减速机", "联轴器"]
SPECS = ["M8×30", "M10", "Φ12", "6204-2RS", "DN15", "24V DC", "0.75kW", "NBR 70", "L=500mm", "1:20"]
NAMES = ["张伟", "王芳", "李娜", "刘洋", "陈静", "杨磊", "赵敏", "黄强", "周杰", "吴倩"]
NOTES = ["", "加急", "含税", "", "待确认", "", "已发货", ""]

def money(lo, hi):
    return f"{random.uniform(lo, hi):,.2f}"

def basic(rows=8, empties=False):
    t = [["序号", "产品名称", "规格型号", "数量", "单价", "金额", "备注"]]
    for i in range(rows):
        q = random.randint(1, 500)
        p = random.uniform(0.5, 900)
        row = [c(str(i + 1), align="center"), random.choice(PRODUCTS), random.choice(SPECS), c(str(q), align="right"),
               c(f"{p:,.2f}", align="right"), c(f"{q * p:,.2f}", align="right"), random.choice(NOTES)]
        if empties and i % 3 == 1:
            row[2] = ""
        if empties and i % 4 == 2:
            row[4] = c("", align="right")
            row[1] = ""
        t.append(row)
    return t

def merged():
    t = [[c("部门", rs=2), c("姓名", rs=2), c("第一季度", cs=3, align="center"), c("第二季度", cs=3, align="center"), c("合计", rs=2)],
         ["1月", "2月", "3月", "4月", "5月", "6月"]]
    for dept, n in (("销售部", 3), ("技术部", 2), ("财务部", 2)):
        for k in range(n):
            vals = [random.randint(20, 300) for _ in range(6)]
            row = ([c(dept, rs=n)] if k == 0 else []) + [NAMES[(len(t) + k) % len(NAMES)]] + [c(str(v), align="right") for v in vals] + [c(str(sum(vals)), align="right")]
            t.append(row)
    return t

def wrapped():
    desc = ["登录页在高分屏上按钮文字被截断，需要调整布局并重新验证各个缩放比例", "导出报表时偶尔出现空白页", "搜索结果排序与预期不一致，按修改时间排序后仍然混入了旧文件，怀疑是缓存没有刷新",
            "安装包签名过期", "深色模式下表格边框颜色太浅，看不清分隔线，建议统一取主题色", "打印预览卡顿"]
    t = [["编号", "问题描述", "负责人", "状态", "日期"]]
    for i, d in enumerate(desc):
        t.append([f"BUG-{1021 + i}", d, NAMES[i], random.choice(["处理中", "已解决", "待验证"]), f"2026-11-{10 + i:02d}"])
    return t

def english():
    t = [["Task name", "Description", "Owner", "Due date", "Status"]]
    rows = [("Quarterly sales report", "Collect numbers from all regions", "Amy Chen", "Dec 3, 2026", "In progress"),
            ("Update pricing page", "New plans and FAQ", "Bob Li", "Dec 10, 2026", "Not started"),
            ("Fix login timeout", "Session expires too early on mobile", "Carol Wang", "Nov 30, 2026", "Done"),
            ("Customer interview notes", "Summarize the five calls", "David Zhao", "Dec 1, 2026", "In review"),
            ("Server migration", "Move the API to the new cluster", "Erin Sun", "Dec 15, 2026", "Blocked"),
            ("Design system audit", "List unused components", "Frank Wu", "Dec 20, 2026", "Not started")]
    return t + [list(r) for r in rows]

def big(rows=40):
    t = [["日期", "订单号", "客户", "品名", "数量", "单价", "金额", "状态"]]
    for i in range(rows):
        q = random.randint(1, 99)
        p = random.uniform(5, 500)
        t.append([f"2026-10-{i % 28 + 1:02d}", f"SO{26100000 + i * 37}", random.choice(NAMES) + "贸易", random.choice(PRODUCTS),
                  c(str(q), align="right"), c(f"{p:.2f}", align="right"), c(f"{q * p:,.2f}", align="right"), random.choice(["已完成", "待发货", "已取消"])])
    return t

def erp():
    t = [["工单号", "物料编码", "物料名称", "工序", "计划数", "完成数", "不良数", "良率", "机台", "操作员"]]
    for i in range(20):
        plan = random.randint(100, 2000)
        done = random.randint(50, plan)
        bad = random.randint(0, 20)
        t.append([f"WO-{51000 + i}", f"M{random.randint(100000, 999999)}", random.choice(PRODUCTS), random.choice(["冲压", "焊接", "喷涂", "装配", "检验"]),
                  c(str(plan), align="right"), c(str(done), align="right"), c(str(bad), align="right"), c(f"{(done - bad) / done * 100:.1f}%", align="right"),
                  f"{random.choice('ABCD')}{random.randint(1, 12):02d}", random.choice(NAMES)])
    return t

def keyvalue():
    return [["参数", "值"], ["设备型号", "XJ-2000 Pro"], ["额定电压", "380 V"], ["额定功率", "7.5 kW"], ["最大转速", "3000 r/min"], ["防护等级", "IP65"],
            ["工作温度", "-10 ~ 50 ℃"], ["外形尺寸", "850 × 620 × 1200 mm"], ["整机重量", "186 kg"], ["出厂日期", "2026-09-18"]]

def centered():
    t = [["班次", "早班", "中班", "晚班", "合计"]]
    for d in ["周一", "周二", "周三", "周四", "周五", "周六"]:
        v = [random.randint(80, 160) for _ in range(3)]
        t.append([d] + [str(x) for x in v] + [str(sum(v))])
    return [[c(x, align="center") for x in row] for row in t]

BASE = "body{margin:24px;font-family:'Microsoft YaHei','Segoe UI',sans-serif;font-size:14px;background:#fff;color:#222}table{border-collapse:collapse}td,th{padding:7px 12px;font-weight:normal;text-align:left;vertical-align:middle}th{font-weight:600}"
STYLES = {
    "bordered": "td,th{border:1px solid #c8c8c8}th{background:#f2f2f2}",
    "none": "td,th{padding:7px 16px}",
    "zebra": "tbody tr:nth-child(even){background:#f3f5f7}th{background:#e3e8ee}td,th{padding:8px 16px}",
    "hlines": "td,th{border-bottom:1px solid #e3e3e3;padding:9px 16px}th{color:#666}",
    "dark": "body{background:#1f1f1f;color:#ddd}td,th{border:1px solid #4a4a4a}th{background:#2b2b2b}",
    "darkh": "body{background:#181a1f;color:#d5d8de}td,th{border-bottom:1px solid #333842;padding:9px 16px}",
    "excel": "body{font-family:'等线','DengXian',Calibri,sans-serif;font-size:14.6px}td,th{border:1px solid #d4d4d4;padding:2px 5px;font-weight:normal}",
    "erp": "body{font-size:12px}td,th{border:1px solid #b9c3cf;padding:3px 6px}th{background:#dfe7f1}",
    "accent": "th{background:#2f6fd0;color:#fff}td,th{border:1px solid #9db7e0}",
}

CASES = [
    ("bordered", "bordered", basic(), {}), ("borderless", "none", basic(), {}), ("zebra", "zebra", basic(), {}), ("hlines", "hlines", basic(), {}),
    ("merged-bordered", "bordered", merged(), {"head": 2}), ("merged-zebra", "zebra", merged(), {"head": 2}), ("merged-hlines", "hlines", merged(), {"head": 2}),
    ("wrapped-bordered", "bordered", wrapped(), {"col": {1: 230}}), ("wrapped-hlines", "hlines", wrapped(), {"col": {1: 230}}),
    ("wrapped-borderless", "none", wrapped(), {"col": {1: 230}}),
    ("english-borderless", "none", english(), {}), ("english-hlines", "hlines", english(), {}),
    ("empties-bordered", "bordered", basic(9, True), {}), ("empties-zebra", "zebra", basic(9, True), {}), ("empties-borderless", "none", basic(9, True), {}),
    ("dark-bordered", "dark", basic(), {}), ("dark-hlines", "darkh", basic(), {}), ("accent-header", "accent", basic(), {}),
    ("excel", "excel", basic(12), {}), ("erp-dense", "erp", erp(), {}), ("big-40-rows", "bordered", big(), {}), ("big-zebra", "zebra", big(30), {}),
    ("keyvalue-bordered", "bordered", keyvalue(), {}), ("keyvalue-borderless", "none", keyvalue(), {}), ("centered-borderless", "none", centered(), {}),
    # Closer to what gets selected on a screen: no outer border in the selection, a title and a note, links and badges in the cells.
    ("inside-bordered", "bordered", basic(), {"inside": True}), ("inside-merged", "bordered", merged(), {"head": 2, "inside": True}),
    ("inside-erp", "erp", erp(), {"inside": True}),
    ("caption-bordered", "bordered", basic(), {"caption": "2026 年 11 月采购明细", "note": "注：金额含税，单位为元"}),
    ("caption-borderless", "none", basic(), {"caption": "2026 年 11 月采购明细", "note": "注：金额含税，单位为元"}),
    ("caption-hlines", "hlines", basic(), {"caption": "2026 年 11 月采购明细"}),
    ("decor-bordered", "bordered", basic(8, True), {"decor": True}), ("decor-hlines", "hlines", basic(8, True), {"decor": True}),
    ("decor-zebra", "zebra", basic(8, True), {"decor": True}),
]
SCALES = {"inside-bordered": [1, 1.5], "caption-bordered": [1, 1.5], "decor-bordered": [1, 1.25, 1.5], "bordered": [1, 1.5, 2], "borderless": [1, 1.5], "zebra": [1, 1.5], "merged-bordered": [1, 1.5], "wrapped-hlines": [1, 1.5], "erp-dense": [1, 1.25, 1.5], "excel": [1, 1.25]}

def html(style, table, extra):
    rows = []
    for r, row in enumerate(table):
        cells = []
        col = 0
        for x in row:
            tag = "th" if r < extra.get("head", 1) else "td"
            attrs = (f' rowspan="{x["rs"]}"' if x["rs"] > 1 else "") + (f' colspan="{x["cs"]}"' if x["cs"] > 1 else "")
            css = (f'text-align:{x["align"]};' if x["align"] else "")
            if col in extra.get("col", {}) and x["cs"] == 1:
                css += f'width:{extra["col"][col]}px;'
            text = x["t"]
            if extra.get("decor") and r > 0 and text:
                if col == 1:
                    text = f'<a style="color:#0969da;text-decoration:underline">{text}</a>'
                elif col == len(table[0]) - 1:
                    text = f'<span style="background:#dafbe1;color:#1a7f37;padding:2px 9px;border-radius:10px">{text}</span>'
            cells.append(f'<{tag}{attrs} style="{css}">{text}</{tag}>')
            col += x["cs"]
        rows.append("<tr>" + "".join(cells) + "</tr>")
    # One row group: a header cell may span into the next header row.
    before = f'<div style="font-size:17px;font-weight:600;margin-bottom:12px">{extra["caption"]}</div>' if extra.get("caption") else ""
    after = f'<div style="color:#777;font-size:12px;margin-top:10px">{extra["note"]}</div>' if extra.get("note") else ""
    return f'<!doctype html><meta charset="utf-8"><style>{BASE}{STYLES[style]}</style>{before}<table><tbody>{"".join(rows)}</tbody></table>{after}'

def grid(table):
    # The cells as they paste: a merged cell's text at its top left, the rest of it empty.
    out, spans, taken = [], [], {}
    for r, row in enumerate(table):
        col = 0
        line = {}
        for x in row:
            while (r, col) in taken:
                col += 1
            line[col] = x["t"]
            if x["rs"] > 1 or x["cs"] > 1:
                spans.append([r, col, x["rs"], x["cs"]])
            for dr in range(x["rs"]):
                for dc in range(x["cs"]):
                    taken[(r + dr, col + dc)] = True
            col += x["cs"]
        out.append(line)
    width = max(k[1] for k in taken) + 1
    return [[line.get(i, "") for i in range(width)] for line in out], spans

def render(name, style, table, extra, scale):
    os.makedirs(OUT, exist_ok=True)
    page = os.path.join(OUT, name + ".html")
    with open(page, "w", encoding="utf-8") as f:
        f.write(html(style, table, extra))
    stem = f"{name}@{int(scale * 100)}"
    shot = os.path.join(OUT, stem + ".png")
    subprocess.run([EDGE, "--headless=new", "--disable-gpu", "--hide-scrollbars", f"--force-device-scale-factor={scale}", "--window-size=1500,2200",
                    "--user-data-dir=" + os.path.join(os.path.dirname(OUT), "edge-profile"), "--screenshot=" + shot, "file:///" + page.replace("\\", "/")],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=120)
    image = Image.open(shot).convert("RGB")
    background = Image.new("RGB", image.size, image.getpixel((2, 2)))
    box = ImageChops.difference(image, background).getbbox()
    m = -int(2 * scale) if extra.get("inside") else int(6 * scale)  # inside: selected within the outer border
    image.crop((max(0, box[0] - m), max(0, box[1] - m), min(image.width, box[2] + m), min(image.height, box[3] + m))).save(shot)
    os.remove(page)
    return stem

truth = {}
for name, style, table, extra in CASES:
    table = norm(table)
    cells, spans = grid(table)
    if extra.get("caption"):
        cells.insert(0, [extra["caption"]] + [""] * (len(cells[0]) - 1))
    if extra.get("note"):
        cells.append([extra["note"]] + [""] * (len(cells[0]) - 1))
    for scale in SCALES.get(name, [1]):
        stem = render(name, style, table, extra, scale)
        truth[stem] = {"cells": cells, "spans": spans}
        print(stem, len(cells), "x", len(cells[0]))
with open(os.path.join(OUT, "truth.json"), "w", encoding="utf-8") as f:
    json.dump(truth, f, ensure_ascii=False, indent=1)
