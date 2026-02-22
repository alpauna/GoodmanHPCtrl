import Part
import MeshPart
from FreeCAD import Vector

import os, sys
shell_dir = os.path.dirname(os.path.abspath(__file__)) + "/"
sys.path.insert(0, shell_dir)

from cutout import (wall_ring, add_standoff, drill_hole, drill_hole_lateral,
                    reposition_cutout, deboss_text,
                    verify_cutout, z_probe, print_volume)

# Rebuild: original -> mating flip -> wall thicken -> pillar -> drill -> LCD reposition -> labels
top_orig = Part.read(shell_dir + "3DShell_GoodmanHPv3_T_3mm_TC.step")
bb = top_orig.BoundBox

OX_L = -98.262; IX_L = -95.262; IX_R = 1.917; OX_R = 4.917
OY_F = -32.48;  IY_F = -29.48;  IY_B = 94.12;  OY_B = 97.12
THICK = 0.5
NX_L = OX_L - THICK; NX_R = OX_R + THICK
NY_F = OY_F - THICK; NY_B = OY_B + THICK
NEW_HALF = 1.75
NMX_L = IX_L - NEW_HALF; NMX_R = IX_R + NEW_HALF
NMY_F = IY_F - NEW_HALF; NMY_B = IY_B + NEW_HALF
TOP_TONGUE_Z = 28.0; TOP_RIM_Z = 32.0
CEIL_Z = bb.ZMax  # 51.50
CEIL_INNER = 48.50

# ---------------------------------------------------------------------------
# Ceiling cutout features (coordinates from probed original shell)
# ---------------------------------------------------------------------------
FEATURES = {
    "LV": {  # Low Voltage — 24v signal wires
        "x1": -88.8, "y1": 0.2, "x2": -4.7, "y2": 35.4,
        "label": "24v Signal Wires", "label_pos": "above",
    },
    "HV": {  # High Voltage — 240V power
        "x1": -90.5, "y1": 49.5, "x2": -80.0, "y2": 69.5,
        "label": "240 V", "label_pos": "above",
    },
    "TC": {  # Thermocouple — MAX6675 terminal block
        "x1": -71.0, "y1": 71.5, "x2": -55.5, "y2": 85.0,
        "label_plus": True, "label_liquid_line": True,
    },
    "LCD": {  # LCD display — no label
        "old_x1": -38.48, "old_y1": 59.59, "old_x2": -11.43, "old_y2": 76.59,
        "new_y1": 61.59, "new_y2": 77.59,
    },
}

LV = FEATURES["LV"]
HV = FEATURES["HV"]
TC = FEATURES["TC"]
LCD = FEATURES["LCD"]

# 1. Remove tongue
result = top_orig.cut(wall_ring(OX_L-2, OY_F-2, OX_R+2, OY_B+2,
                                IX_L, IY_F, IX_R, IY_B,
                                TOP_TONGUE_Z-0.1, TOP_RIM_Z+0.01))
# 2. Inner lip
result = result.fuse(wall_ring(NMX_L, NMY_F, NMX_R, NMY_B,
                               IX_L, IY_F, IX_R, IY_B,
                               TOP_TONGUE_Z, TOP_RIM_Z+2))
# 3. Outer groove
result = result.cut(wall_ring(NX_L-1, NY_F-1, NX_R+1, NY_B+1,
                              NMX_L, NMY_F, NMX_R, NMY_B,
                              TOP_TONGUE_Z-0.1, TOP_RIM_Z))
# 4. Thicken walls only
result = result.fuse(wall_ring(NX_L, NY_F, NX_R, NY_B,
                               OX_L, OY_F, OX_R, OY_B,
                               TOP_RIM_Z, CEIL_Z))

# 5. M3 pillar below ceiling (below TC cutout, board screw mount)
pil_x = bb.XMin + 35   # -63.76 — centered under TC cutout in X
pil_y = bb.YMax - 30    # 67.14 — below TC cutout bottom edge (71.5)
result = add_standoff(result, pil_x, pil_y, z_surface=CEIL_INNER,
                      height=3.0, radius=2.5, z_outer=CEIL_Z)

# 6. M3 hole through pillar + ceiling
result = drill_hole(result, pil_x, pil_y,
                    z_bottom=CEIL_INNER - 3.0, z_top=CEIL_Z, radius=1.25)
print_volume(result, "After pillar+hole")

# Verify pillar and hole
for dx, dy, label in [(0, 0, "hole center"), (2, 0, "pillar wall")]:
    zs = z_probe(result, pil_x + dx, pil_y + dy)
    print(f"  {label}: Z={[f'{z:.2f}' for z in zs]}" if zs else f"  {label}: OPEN")

# 6. M1.5 screw holes through left and right walls at mating zone midpoint
result = drill_hole_lateral(result, axis='x', start=NX_L, end=NX_R,
                            cy=(OY_F + OY_B) / 2, cz=32.65, radius=0.75)
print_volume(result, "After screw holes")

# 7. Reposition LCD cutout: shift bottom up 2mm, top up 1mm (27x16mm)
result = reposition_cutout(
    result,
    old_x1=LCD["old_x1"], old_y1=LCD["old_y1"],
    old_x2=LCD["old_x2"], old_y2=LCD["old_y2"],
    new_x1=LCD["old_x1"], new_y1=LCD["new_y1"],
    new_x2=LCD["old_x2"], new_y2=LCD["new_y2"],
    z_inner=CEIL_INNER, z_outer=CEIL_Z)
print_volume(result, "After LCD reposition")
verify_cutout(result, LCD["old_x1"], LCD["new_y1"],
              LCD["old_x2"], LCD["new_y2"], CEIL_INNER, CEIL_Z, "LCD")

# 8. Deboss feature labels (0.8mm depth for slicer visibility)
# LV: "24v Signal Wires" centered above cutout
result = deboss_text(result, LV["label"],
    x=(LV["x1"] + LV["x2"]) / 2, y=LV["y2"] + 2.0,
    z_surface=CEIL_Z, height=5.0, depth=0.8, anchor="center")
print_volume(result, "After LV label")

# HV: "240 V" centered above cutout
result = deboss_text(result, HV["label"],
    x=(HV["x1"] + HV["x2"]) / 2, y=HV["y2"] + 2.0,
    z_surface=CEIL_Z, height=5.0, depth=0.8, anchor="center")
print_volume(result, "After HV label")

# TC: "+" below-left, "-" below-right
result = deboss_text(result, "+", x=TC["x1"] + 2, y=TC["y1"] - 5.0,
    z_surface=CEIL_Z, height=5.0, depth=0.8)
result = deboss_text(result, "-", x=TC["x2"] - 4, y=TC["y1"] - 5.0,
    z_surface=CEIL_Z, height=5.0, depth=0.8)
# TC: "Liquid" / "Line" below the +/- symbols
result = deboss_text(result, ["Liquid", "Line"],
    x=(TC["x1"] + TC["x2"]) / 2, y=TC["y1"] - 12.0,
    z_surface=CEIL_Z, height=4.0, depth=0.8, anchor="center")
print_volume(result, "After TC labels")

# Export
step_f = shell_dir + "3DShell_GoodmanHPv3_T_3.5mm_TC_flipped.step"
stl_f = shell_dir + "3DShell_GoodmanHPv3_T_3.5mm_TC_flipped.stl"
result.exportStep(step_f)
mesh = MeshPart.meshFromShape(Shape=result, LinearDeflection=0.1, AngularDeflection=0.5)
mesh.write(stl_f)
print(f"\nExported: {step_f}")
