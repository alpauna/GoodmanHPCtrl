"""Generate the KiCad schematic (6 sheets) and symbol library from model.json.

Every pin gets a short wire and a net label, so connectivity comes straight
from the v8 netlist. Nets used on more than one sheet get global labels.
"""
import json
import math
import re
import uuid
from collections import Counter, defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
MODEL = json.loads((HERE / "model.json").read_text())
PARTS = MODEL["parts"]
PROJECT = "GoodmanHP"
LIB = "GoodmanHP"
VER = "20231120"
GEN = '(generator "goodmanhp_gen")'


def uid(*parts):
    return str(uuid.uuid5(uuid.NAMESPACE_URL, "goodmanhp/" + "/".join(parts)))


ROOT_UUID = uid("root")

# ---------------------------------------------------------------- sheets
SHEETS = [
    ("blades", "Blades & mounting"),
    ("power", "Power"),
    ("inputs", "Inputs & CAN"),
    ("outputs", "Outputs"),
    ("cpu", "CPU"),
    ("usb", "USB"),
]
SEEDS = {
    "blades": [f"U{i}" for i in (1, 2, 4, 5, 6, 7, 9, 10, 12, 13, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27)]
              + ["U3", "U8", "U11", "U14", "U50"],
    "power": ["U28", "U29", "D1", "D2", "D3", "D4", "D5", "F1", "L1", "FB2"],
    "inputs": ["U30", "D14", "D6", "D7", "D10", "D11", "D8", "D9", "D12", "D13"],
    "outputs": ["U31", "U32", "U34", "U36", "U37", "U39", "U33", "U35", "D15", "D17", "D18", "D19", "D22",
                "F2", "F3", "F4", "F5", "Q1", "Q2", "Q3", "Q4", "Q7", "Q8", "Q9", "Q10", "Q11", "Q12", "Q13",
                "Q14", "Q15"],
    "cpu": ["U44", "U45", "U46", "U47", "U48", "U42", "U43", "U51", "H1", "H2", "H3", "H4", "CN1", "CARD1",
            "SW1", "SW2", "Q5", "Q6", "L2", "L3", "L4", "FB1"],
    "usb": ["U49", "USB1", "D16"],
}
# Parts whose nearest neighbour on the PCB is on a different schematic page.
SEEDS["inputs"] += ["C16", "C19", "C22", "C25", "R9", "R12", "R17", "R20"]
SEEDS["outputs"] += ["R28"]


def part_nets(p):
    return {pad["net"] for pad in p["pads"] if pad["net"]}


net_pads = Counter(pad["net"] for p in PARTS.values() for pad in p["pads"] if pad["net"])
BIG = {n for n, c in net_pads.items() if c > 40} | {"GND", "AGND", "3.3V", "5V", "24VAC", "5VPF", "VBUS"}

sheet_of = {}
for s, ds in SEEDS.items():
    for d in ds:
        sheet_of[d] = s
changed = True
while changed:
    changed = False
    for d, p in PARTS.items():
        if d in sheet_of:
            continue
        votes = Counter()
        for n in part_nets(p) - BIG:
            for d2, p2 in PARTS.items():
                if d2 in sheet_of and sheet_of[d2] != "blades" and n in part_nets(p2):
                    votes[sheet_of[d2]] += 1
        top = votes.most_common(2)
        if top and (len(top) == 1 or top[0][1] > top[1][1]):
            sheet_of[d] = top[0][0]
            changed = True
# Ties and power-only parts (decoupling caps): nearest grouped part on the PCB.
for d, p in PARTS.items():
    if d in sheet_of:
        continue
    best = min((math.hypot(p["x"] - q["x"], p["y"] - q["y"]), d2) for d2, q in PARTS.items()
               if d2 in sheet_of and sheet_of[d2] != "blades")
    sheet_of[d] = sheet_of[best[1]]
for d in PARTS:
    sheet_of.setdefault(d, "cpu")

# ------------------------------------------------------------ pin names
PIN_NAMES = {
    "GAQV1122S": {1: "+IN", 2: "-IN", 3: "DC-", 4: "DC+"},
    "MOC3041SM": {1: "A", 2: "K", 3: "NC", 4: "MT1", 5: "NC", 6: "MT2"},
    "AT3H4B-CuH-S": {1: "AC1", 2: "AC2", 3: "E", 4: "C"},
    "LGS5145": {1: "BST", 2: "GND", 3: "FB", 4: "EN", 5: "VIN", 6: "SW"},
    "TLV76133DCYR": {1: "GND", 2: "OUT", 3: "IN", 4: "OUT"},
    "SN65HVD230DR-JSM": {1: "D", 2: "GND", 3: "VCC", 4: "R", 5: "VREF", 6: "CANL", 7: "CANH", 8: "RS"},
    "PCA9306DCUR": {1: "GND", 2: "VREF1", 3: "SCL1", 4: "SDA1", 5: "SDA2", 6: "SCL2", 7: "VREF2", 8: "EN"},
    "W25Q128JVSIQ": {1: "CS#", 2: "DO", 3: "WP#", 4: "GND", 5: "DI", 6: "CLK", 7: "HOLD#", 8: "VCC"},
    "CH340K": {1: "UD+", 2: "UD-", 3: "GND", 4: "DTR#", 5: "CTS#", 6: "RTS#", 7: "VCC", 8: "TXD", 9: "RXD",
               10: "V3", 11: "EP"},
    "BT138S-800E": {1: "MT1", 2: "MT2", 3: "G"},
    "HSS2N12A": {1: "G", 2: "S", 3: "D"},
    "L2N7002LT1G": {1: "G", 2: "S", 3: "D"},
    "SS8050": {1: "B", 2: "E", 3: "C"},
    "40MHz": {1: "XTAL1", 2: "GND", 3: "XTAL2", 4: "GND"},
    "ESDCAN24-2BLY": {1: "IO1", 2: "IO2", 3: "GND"},
}
esp = {1: "LNA_IN", 2: "VDD3P3", 3: "VDD3P3", 4: "CHIP_PU", 5: "GPIO0", 20: "VDD3P3_RTC", 21: "XTAL_32K_P",
       22: "XTAL_32K_N", 29: "VDD_SPI", 30: "SPIHD", 31: "SPIWP", 32: "SPICS0", 33: "SPICLK", 34: "SPIQ",
       35: "SPID", 36: "SPICLK_N", 37: "SPICLK_P", 44: "MTCK", 45: "MTDO", 46: "VDD3P3_CPU", 47: "MTDI",
       48: "MTMS", 49: "U0TXD", 50: "U0RXD", 51: "GPIO45", 52: "GPIO46", 53: "XTAL_N", 54: "XTAL_P",
       55: "VDDA", 56: "VDDA", 57: "GND"}
for i in range(1, 15):
    esp[5 + i] = f"GPIO{i}"
for i, n in zip(range(23, 29), ("GPIO17", "GPIO18", "GPIO19", "GPIO20", "GPIO21", "SPICS1")):
    esp[i] = n
for i, n in zip(range(38, 44), ("GPIO33", "GPIO34", "GPIO35", "GPIO36", "GPIO37", "GPIO38")):
    esp[i] = n
PIN_NAMES["ESP32-S3R8"] = esp


def kind(p):
    d, mpn, fp = p["des"], p["mpn"], p["footprint"]
    n = len({pad["num"] for pad in p["pads"]})
    if d.startswith("FB"):
        return "FB"
    if re.match(r"R\d", d):
        return "R"
    if re.match(r"C\d", d):
        return "C"
    if re.match(r"L\d", d):
        return "L"
    if re.match(r"F\d", d):
        return "F"
    if re.match(r"D\d", d) and n == 2:
        if mpn.endswith("CA"):
            return "TVS2"
        if mpn.startswith(("BZX", "SMF", "SMAJ")):
            return "ZD"
        return "D"
    return "BOX"


# ------------------------------------------------------------ symbols
def fmt(v):
    s = f"{v:.4f}".rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


def pin(num, name, x, y, ang, length=2.54, hide_name=False):
    return (f'(pin passive line (at {fmt(x)} {fmt(y)} {ang}) (length {fmt(length)})'
            f' (name "{name}" (effects (font (size 1.016 1.016))))'
            f' (number "{num}" (effects (font (size 1.016 1.016)))))')


def two_pin_graphics(k):
    if k == "R":
        return "(rectangle (start -2.54 -1.016) (end 2.54 1.016) (stroke (width 0.254) (type default)) (fill (type none)))"
    if k == "C":
        return ("(polyline (pts (xy -0.508 -2.032) (xy -0.508 2.032)) (stroke (width 0.381) (type default)) (fill (type none)))"
                "(polyline (pts (xy 0.508 -2.032) (xy 0.508 2.032)) (stroke (width 0.381) (type default)) (fill (type none)))")
    if k == "L":
        arcs = ""
        for i in range(4):
            x0 = -2.54 + i * 1.27
            arcs += (f"(arc (start {fmt(x0)} 0) (mid {fmt(x0 + 0.635)} 0.635) (end {fmt(x0 + 1.27)} 0)"
                     " (stroke (width 0.254) (type default)) (fill (type none)))")
        return arcs
    if k == "FB":
        return "(rectangle (start -2.54 -1.016) (end 2.54 1.016) (stroke (width 0.254) (type default)) (fill (type outline)))"
    if k == "F":
        return ("(rectangle (start -2.54 -1.016) (end 2.54 1.016) (stroke (width 0.254) (type default)) (fill (type none)))"
                "(polyline (pts (xy -2.032 -1.524) (xy 2.032 1.524)) (stroke (width 0.254) (type default)) (fill (type none)))")
    tri_r = "(polyline (pts (xy -1.27 1.27) (xy -1.27 -1.27) (xy 1.27 0) (xy -1.27 1.27)) (stroke (width 0.254) (type default)) (fill (type none)))"
    if k == "D":
        return tri_r + "(polyline (pts (xy 1.27 1.27) (xy 1.27 -1.27)) (stroke (width 0.254) (type default)) (fill (type none)))"
    if k == "ZD":
        return tri_r + "(polyline (pts (xy 0.762 1.778) (xy 1.27 1.27) (xy 1.27 -1.27) (xy 1.778 -1.778)) (stroke (width 0.254) (type default)) (fill (type none)))"
    if k == "TVS2":
        return ("(polyline (pts (xy -2.54 1.27) (xy -2.54 -1.27) (xy 0 0) (xy -2.54 1.27)) (stroke (width 0.254) (type default)) (fill (type none)))"
                "(polyline (pts (xy 2.54 1.27) (xy 2.54 -1.27) (xy 0 0) (xy 2.54 1.27)) (stroke (width 0.254) (type default)) (fill (type none)))"
                "(polyline (pts (xy -0.508 1.778) (xy 0 1.27) (xy 0 -1.27) (xy 0.508 -1.778)) (stroke (width 0.254) (type default)) (fill (type none)))")
    raise ValueError(k)


def symbol_def(name, p, k):
    """Return (lib symbol s-expr, pin geometry {num: (x, y, side)}, half-width)."""
    nums = sorted({pad["num"] for pad in p["pads"]}, key=lambda s: (len(s), s))
    props = (f'(property "Reference" "{re.match(r"[A-Z]+", p["des"]).group(0)}" (at 0 3.81 0) (effects (font (size 1.27 1.27))))'
             f'(property "Value" "{name}" (at 0 -3.81 0) (effects (font (size 1.27 1.27))))'
             '(property "Footprint" "" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))'
             '(property "Datasheet" "" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))')
    geom = {}
    if k != "BOX":
        if k in ("D", "ZD"):           # pin 1 = cathode on the right
            body = two_pin_graphics(k)
            pins = pin("1", "K", 5.08, 0, 180) + pin("2", "A", -5.08, 0, 0)
            geom = {"1": (5.08, 0, "R"), "2": (-5.08, 0, "L")}
        else:
            body = two_pin_graphics(k)
            pins = pin("1", "~", -5.08, 0, 0) + pin("2", "~", 5.08, 0, 180)
            geom = {"1": (-5.08, 0, "L"), "2": (5.08, 0, "R")}
        hide = "(pin_numbers hide) (pin_names hide)"
        sym = (f'(symbol "{name}" {hide} (in_bom yes) (on_board yes) {props}'
               f'(symbol "{name}_0_1" {body}) (symbol "{name}_1_1" {pins}))')
        return sym, geom, 5.08
    names = PIN_NAMES.get(p["mpn"], {})
    labels = {n: names.get(int(n), "") if n.isdigit() else "" for n in nums}
    for n in nums:
        if not labels[n]:
            nets = sorted({pad["net"] for pad in p["pads"] if pad["num"] == n and pad["net"]})
            labels[n] = nets[0] if nets else f"P{n}"
    half = (len(nums) + 1) // 2
    longest = max(len(v) for v in labels.values())
    w = max(10.16, round((longest * 2 * 1.0 + 6) / 2.54) * 2.54)
    h = (half + 1) * 2.54
    pins = ""
    for i, n in enumerate(nums):
        side = "L" if i < half else "R"
        row = i if side == "L" else i - half
        y = h / 2 - 2.54 * (row + 1)
        y = round(y / 1.27) * 1.27
        x = -w / 2 - 2.54 if side == "L" else w / 2 + 2.54
        pins += pin(n, labels[n], x, y, 0 if side == "L" else 180)
        geom[n] = (x, y, side)
    body = (f"(rectangle (start {fmt(-w / 2)} {fmt(h / 2)}) (end {fmt(w / 2)} {fmt(-h / 2)})"
            " (stroke (width 0.254) (type default)) (fill (type background)))")
    props = props.replace("(at 0 3.81 0)", f"(at 0 {fmt(h / 2 + 1.27)} 0)").replace("(at 0 -3.81 0)", f"(at 0 {fmt(-h / 2 - 1.27)} 0)")
    sym = (f'(symbol "{name}" (in_bom yes) (on_board yes) {props}'
           f'(symbol "{name}_0_1" {body}) (symbol "{name}_1_1" {pins}))')
    return sym, geom, w / 2 + 2.54


def sym_name(p, k):
    if k in ("R", "C", "L", "FB", "F", "D", "ZD", "TVS2"):
        return k
    base = re.sub(r"[^A-Za-z0-9_.+-]", "_", p["mpn"] or p["footprint"])
    return f"{base}_{len({pad['num'] for pad in p['pads']})}p"


symdefs, part_sym = {}, {}
for d, p in PARTS.items():
    k = kind(p)
    nm = sym_name(p, k)
    if nm not in symdefs:
        symdefs[nm] = symbol_def(nm, p, k)
    elif k == "BOX":
        # same MPN but a different pin set -> make it unique
        g = symdefs[nm][1]
        if set(g) != {pad["num"] for pad in p["pads"]}:
            nm = f"{nm}_{d}"
            symdefs[nm] = symbol_def(nm, p, k)
    part_sym[d] = nm

lib_text = f"(kicad_symbol_lib (version {VER}) {GEN}\n" + "\n".join(
    s.replace(f'(symbol "{n}"', f'(symbol "{n}"', 1) for n, (s, _, _) in symdefs.items()) + "\n)\n"
(HERE / f"{LIB}.kicad_sym").write_text(lib_text)

# --------------------------------------------------------- schematics
net_sheets = defaultdict(set)
for d, p in PARTS.items():
    for n in part_nets(p):
        net_sheets[n].add(sheet_of[d])


def label_name(n):
    return n.replace('"', "'")


def esc(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')


def sheet_file(key):
    return f"{PROJECT}_{key}.kicad_sch"


def build_sheet(key, title, page_no):
    sheet_uuid = uid("sheet", key)
    items = []
    libs = set()
    ds = sorted((d for d in PARTS if sheet_of[d] == key),
                key=lambda d: (kind(PARTS[d]) != "BOX", re.match(r"[A-Z]+", d).group(0),
                               int(re.search(r"\d+", d).group(0))))
    x0, y0, x, y, row_h = 30.48, 30.48, 30.48, 30.48, 0
    page_w = 400
    for d in ds:
        p = PARTS[d]
        nm = part_sym[d]
        libs.add(nm)
        sym, geom, half_w = symdefs[nm]
        ys = [g[1] for g in geom.values()]
        h = (max(ys) - min(ys)) + 12.7
        cell_w = 2 * half_w + 2 * 22.86
        if x + cell_w > page_w:
            x = x0
            y += row_h + 7.62
            row_h = 0
        cx = round((x + cell_w / 2) / 2.54) * 2.54
        cy = round((y + h / 2) / 2.54) * 2.54
        su = uid("sym", d)
        ref_y = cy - (max(ys) + 2.54) if kind(p) == "BOX" else cy - 3.81
        val_y = cy + (-min(ys) + 2.54) if kind(p) == "BOX" else cy + 3.81
        fpname = f"{PROJECT}:{re.sub(r'[^A-Za-z0-9_.+-]', '_', p['footprint'])}"
        items.append(
            f'(symbol (lib_id "{LIB}:{nm}") (at {fmt(cx)} {fmt(cy)} 0) (unit 1) (in_bom yes) (on_board yes) (dnp no)'
            f' (uuid "{su}")'
            f' (property "Reference" "{d}" (at {fmt(cx)} {fmt(ref_y)} 0) (effects (font (size 1.27 1.27))))'
            f' (property "Value" "{esc(p["mpn"] or p["value"])}" (at {fmt(cx)} {fmt(val_y)} 0) (effects (font (size 1.27 1.27))))'
            f' (property "Footprint" "{fpname}" (at {fmt(cx)} {fmt(cy)} 0) (effects (font (size 1.27 1.27)) hide))'
            f' (property "Datasheet" "" (at {fmt(cx)} {fmt(cy)} 0) (effects (font (size 1.27 1.27)) hide))'
            f' (property "LCSC" "{esc(p["lcsc"])}" (at {fmt(cx)} {fmt(cy)} 0) (effects (font (size 1.27 1.27)) hide))'
            f' (property "Manufacturer" "{esc(p["manufacturer"])}" (at {fmt(cx)} {fmt(cy)} 0) (effects (font (size 1.27 1.27)) hide))'
            + "".join(f' (pin "{n}" (uuid "{uid("pin", d, n)}"))' for n in geom)
            + f' (instances (project "{PROJECT}" (path "/{ROOT_UUID}/{sheet_uuid}" (reference "{d}") (unit 1)))))')
        pad_net = {}
        for pad in p["pads"]:
            pad_net.setdefault(pad["num"], pad["net"])
        for n, (px, py, side) in geom.items():
            ax, ay = cx + px, cy - py
            net = pad_net.get(n, "")
            if not net:
                items.append(f'(no_connect (at {fmt(ax)} {fmt(ay)}) (uuid "{uid("nc", d, n)}"))')
                continue
            ex = ax - 2.54 if side == "L" else ax + 2.54
            items.append(f'(wire (pts (xy {fmt(ax)} {fmt(ay)}) (xy {fmt(ex)} {fmt(ay)})) (stroke (width 0) (type default)) (uuid "{uid("w", d, n)}"))')
            ang = 180 if side == "L" else 0
            just = "right" if side == "L" else "left"
            lbl = label_name(net)
            if len(net_sheets[net]) > 1:
                items.append(f'(global_label "{lbl}" (shape passive) (at {fmt(ex)} {fmt(ay)} {ang}) (fields_autoplaced yes)'
                             f' (effects (font (size 1.27 1.27)) (justify {just})) (uuid "{uid("gl", d, n)}"))')
            else:
                items.append(f'(label "{lbl}" (at {fmt(ex)} {fmt(ay)} {ang}) (fields_autoplaced yes)'
                             f' (effects (font (size 1.27 1.27)) (justify {just} bottom)) (uuid "{uid("l", d, n)}"))')
        x += cell_w
        row_h = max(row_h, h)
    height = y + row_h + 30
    paper = "A3" if height <= 290 else ("A2" if height <= 410 else "A1")
    lib_syms = "\n".join(symdefs[n][0].replace(f'(symbol "{n}"', f'(symbol "{LIB}:{n}"', 1) for n in sorted(libs))
    text = (f'(kicad_sch (version {VER}) {GEN} (uuid "{sheet_uuid}") (paper "{paper}")\n'
            f'(title_block (title "Goodman Heatpump Controller - {title}") (rev "v8") (company "alshowto.com"))\n'
            f'(lib_symbols\n{lib_syms}\n)\n' + "\n".join(items) + "\n)\n")
    (HERE / sheet_file(key)).write_text(text)
    return sheet_uuid, len(ds), paper


root_items = []
for i, (key, title) in enumerate(SHEETS):
    su, n, paper = build_sheet(key, title, i + 2)
    sx, sy = 30 + (i % 3) * 80, 40 + (i // 3) * 60
    root_items.append(
        f'(sheet (at {sx} {sy}) (size 60 40) (fields_autoplaced yes) (stroke (width 0.1524) (type solid)) (fill (color 0 0 0 0.0000))'
        f' (uuid "{su}")'
        f' (property "Sheetname" "{title}" (at {sx} {sy - 0.7} 0) (effects (font (size 1.27 1.27)) (justify left bottom)))'
        f' (property "Sheetfile" "{sheet_file(key)}" (at {sx} {sy + 40.6} 0) (effects (font (size 1.27 1.27)) (justify left top)))'
        f' (instances (project "{PROJECT}" (path "/{ROOT_UUID}" (page "{i + 2}")))))')
    print(f"{key:8s} {n:3d} parts  {paper}")

root = (f'(kicad_sch (version {VER}) {GEN} (uuid "{ROOT_UUID}") (paper "A4")\n'
        '(title_block (title "Goodman Heatpump Controller") (rev "v8") (company "alshowto.com")'
        ' (comment 1 "Rebuilt in KiCad from the EasyEDA v8 netlist"))\n'
        '(lib_symbols)\n' + "\n".join(root_items) +
        f'\n(sheet_instances (path "/" (page "1")))\n)\n')
(HERE / f"{PROJECT}.kicad_sch").write_text(root)

(HERE / "sym-lib-table").write_text(
    f'(sym_lib_table (version 7)\n  (lib (name "{LIB}")(type "KiCad")(uri "${{KIPRJMOD}}/{LIB}.kicad_sym")(options "")(descr "GoodmanHP project symbols"))\n)\n')
json.dump({"sheet_of": sheet_of, "part_sym": part_sym,
           "sheet_uuid": {k: uid("sheet", k) for k, _ in SHEETS}, "root_uuid": ROOT_UUID,
           "sym_uuid": {d: uid("sym", d) for d in PARTS}},
          open(HERE / "links.json", "w"), indent=1)
print(f"{len(symdefs)} symbols")
