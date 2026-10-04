"""Write the KiCad project file (net classes), custom DRC rules and library tables."""
import json
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
PARTS = json.loads((HERE / "model.json").read_text())["parts"]
PROJECT = "GoodmanHP"

# --- 240 V side: grow from the line nets through passives, stop at the optocouplers
OPTOS = {"U37", "U33"}
hv = {"FAN-PWR", "FAN-OUT"}
grew = True
while grew:
    grew = False
    for d, p in PARTS.items():
        if d in OPTOS:
            continue
        nets = {x["net"] for x in p["pads"] if x["net"]}
        if nets & hv and not nets <= hv:
            hv |= nets
            grew = True
# the optocouplers' line-side pins
for d, pins in (("U37", {"4", "5", "6"}), ("U33", {"1", "2"})):
    hv |= {x["net"] for x in PARTS[d]["pads"] if x["num"] in pins and x["net"]}
hv.discard("E-GND")          # earth: its own class, not 240 V

power = {"24VAC", "$2N844", "$2N846", "5VPF", "SW", "5V", "VBUS", "LT", "CNT", "W", "O-RV"}
for d in ("F2", "F3", "F4", "F5"):
    power |= {x["net"] for x in PARTS[d]["pads"]}
power -= hv
planes = {"GND", "3.3V", "AGND"}

BASE = {"clearance": 0.1, "track_width": 0.2, "via_diameter": 0.6, "via_drill": 0.3,
        "diff_pair_gap": 0.25, "diff_pair_width": 0.2, "diff_pair_via_gap": 0.25,
        "microvia_diameter": 0.3, "microvia_drill": 0.1, "wire_width": 6, "bus_width": 12,
        "line_style": 0, "pcb_color": "rgba(0, 0, 0, 0.000)", "schematic_color": "rgba(0, 0, 0, 0.000)"}


def cls(name, priority, **kw):
    c = dict(BASE, name=name, priority=priority)
    c.update(kw)
    return c


classes = [
    cls("Default", 2147483647),
    cls("Power", 1, clearance=0.1, track_width=0.6, via_diameter=0.8, via_drill=0.4),
    cls("Supply", 2, clearance=0.1, track_width=0.4),
    cls("Earth", 3, clearance=0.1, track_width=0.5),
    cls("HV", 0, clearance=0.4, track_width=0.8, via_diameter=1.0, via_drill=0.5),
]
patterns = ([{"netclass": "HV", "pattern": n} for n in sorted(hv)] + [{"netclass": "Earth", "pattern": "E-GND"}]
            + [{"netclass": "Power", "pattern": n} for n in sorted(power)]
            + [{"netclass": "Supply", "pattern": n} for n in sorted(planes)])

pro = {
    "board": {"design_settings": {"defaults": {}, "rules": {
        "min_clearance": 0.1, "min_track_width": 0.15, "min_via_diameter": 0.45, "min_through_hole_diameter": 0.25,
        "min_copper_edge_clearance": 0.3, "min_hole_to_hole": 0.25, "min_hole_clearance": 0.25}}},
    "meta": {"filename": f"{PROJECT}.kicad_pro", "version": 1},
    "net_settings": {"classes": classes, "meta": {"version": 4}, "net_colors": None,
                     "netclass_assignments": None, "netclass_patterns": patterns},
    "pcbnew": {"page_layout_descr_file": ""},
    "schematic": {"legacy_lib_dir": "", "legacy_lib_list": []},
    "sheets": [],
    "text_variables": {},
}
(HERE / f"{PROJECT}.kicad_pro").write_text(json.dumps(pro, indent=2))

(HERE / f"{PROJECT}.kicad_dru").write_text("""(version 1)

# 240 V fan circuit: keep 2.5 mm from every low-voltage net (functional
# insulation for 240 VAC).
(rule "HV to LV clearance"
  (condition "A.NetClass == 'HV' && B.NetClass != 'HV' && B.NetClass != 'Earth'")
  (constraint clearance (min 2.5mm)))

""")

(HERE / "fp-lib-table").write_text(
    f'(fp_lib_table (version 7)\n  (lib (name "{PROJECT}")(type "KiCad")(uri "${{KIPRJMOD}}/{PROJECT}.pretty")(options "")(descr "GoodmanHP footprints"))\n)\n')
print("HV nets:", sorted(hv))
print("Power nets:", len(power))
