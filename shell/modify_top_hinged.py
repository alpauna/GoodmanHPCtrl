"""Hinged top shell: knuckle hinge on left wall, snap clip pockets on right wall."""
import Part
import MeshPart
from FreeCAD import Vector

import os, sys
shell_dir = os.path.dirname(os.path.abspath(__file__)) + "/"
sys.path.insert(0, shell_dir)

from cutout import (wall_ring, drill_hole_lateral, reposition_cutout,
                    max6675_mount, labeled_cutout, deboss_text,
                    verify_cutout, z_probe, print_volume,
                    knuckle_hinge, snap_clip)

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

# Ceiling cutout features
FEATURES = {
    "LV": {
        "x1": -88.8, "y1": 0.2, "x2": -4.7, "y2": 35.4,
        "label": "24v Signal Wires", "label_pos": "above",
    },
    "HV": {
        "x1": -90.5, "y1": 49.5, "x2": -80.0, "y2": 69.5,
        "label": "240 V", "label_pos": "above",
    },
    "TC": {
        "x1": -71.0, "y1": 71.5, "x2": -55.5, "y2": 85.0,
        "label_plus": True, "label_liquid_line": True,
    },
    "LCD": {
        "old_x1": -38.48, "old_y1": 59.59, "old_x2": -11.43, "old_y2": 76.59,
        "new_y1": 61.59, "new_y2": 77.59,
    },
}

TC = FEATURES["TC"]
LCD = FEATURES["LCD"]

# Hinge parameters — must match bottom shell
HINGE_Z = 33.0  # midpoint of mating zone
HINGE_INSET = 5.0
HINGE_A_START = OY_F + HINGE_INSET
HINGE_A_END = OY_B - HINGE_INSET
KNUCKLE_RADIUS = 3.0
PIN_RADIUS = 1.0

# Snap clip parameters — must match bottom shell positions
CLIP_Z = 33.0
CLIP_Y_POSITIONS = [20.0, 65.0]

# === 1-4. Mating profile (same as non-hinged) ===
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
# 4. Solid wall above groove
result = result.fuse(wall_ring(NX_L, NY_F, NX_R, NY_B,
                               IX_L, IY_F, IX_R, IY_B,
                               TOP_RIM_Z, CEIL_Z))
# 4b. Clean up internal ribs
rib_cleanup = wall_ring(IX_L, IY_F, IX_R, IY_B,
                        IX_L+5, IY_F+5, IX_R-5, IY_B-5,
                        TOP_TONGUE_Z, 35.5)
result = result.cut(rib_cleanup)
print_volume(result, "After mating profile")

# === 5. MAX6675 thermocouple mount ===
result = max6675_mount(result,
                       cutout_x1=TC["x1"], cutout_y1=TC["y1"],
                       z_inner=CEIL_INNER, z_outer=CEIL_Z,
                       cutout_w=TC["x2"] - TC["x1"],
                       cutout_h=TC["y2"] - TC["y1"],
                       pillar_x=bb.XMin + 35,
                       pillar_y=bb.YMax - 30,
                       pillar_height=6.0)
print_volume(result, "After TC mount")

# === 6. Reposition LCD cutout ===
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

# === 7. Deboss feature labels ===
for name, feat in FEATURES.items():
    if "label" in feat and "label_pos" in feat:
        result = labeled_cutout(result, feat["x1"], feat["y1"],
                                feat["x2"], feat["y2"],
                                CEIL_INNER, CEIL_Z,
                                label=feat["label"],
                                label_pos=feat["label_pos"],
                                cut=False)
        print_volume(result, f"After {name} label")

# TC polarity + text
result = deboss_text(result, "+", x=TC["x1"] + 2, y=TC["y1"] - 5.0,
    z_surface=CEIL_Z, height=5.0, depth=0.8)
result = deboss_text(result, "-", x=TC["x2"] - 4, y=TC["y1"] - 5.0,
    z_surface=CEIL_Z, height=5.0, depth=0.8)
result = deboss_text(result, ["Liquid", "Line"],
    x=(TC["x1"] + TC["x2"]) / 2, y=TC["y1"] - 12.0,
    z_surface=CEIL_Z, height=4.0, depth=0.8, anchor="center")
print_volume(result, "After TC labels")

# === 8. Knuckle hinge on left wall (odd knuckles for top) ===
result = knuckle_hinge(result, wall='left', wall_coord=NX_L,
                       a_start=HINGE_A_START, a_end=HINGE_A_END,
                       z_center=HINGE_Z,
                       knuckle_count=5, knuckle_radius=KNUCKLE_RADIUS,
                       pin_radius=PIN_RADIUS, clearance=0.3,
                       side="top")
print_volume(result, "After hinge knuckles")

# === 9. Snap clip pockets on right wall ===
for i, clip_y in enumerate(CLIP_Y_POSITIONS):
    result = snap_clip(result, wall='right', wall_coord=NX_R,
                       a_center=clip_y, z_center=CLIP_Z,
                       clip_width=8.0, clip_height=4.0, clip_depth=2.0,
                       lip_height=1.0, lip_depth=0.8,
                       wall_thick=3.5, side="top")
    print_volume(result, f"After snap pocket at Y={clip_y}")

# === VERIFICATION ===
print("\n=== VERIFICATION ===")
pil_x = bb.XMin + 35
pil_y = bb.YMax - 30
for dx, dy, label in [(0, 0, "hole center"), (2, 0, "pillar wall")]:
    zs = z_probe(result, pil_x + dx, pil_y + dy)
    print(f"  {label}: Z={[f'{z:.2f}' for z in zs]}" if zs else f"  {label}: OPEN")

print("Hinge knuckles (top side, should have material):")
y_mid = (OY_F + OY_B) / 2
for y in [HINGE_A_START + 15, y_mid - 10]:
    line = Part.makeLine(Vector(NX_L - 10, y, HINGE_Z),
                         Vector(NX_L + 5, y, HINGE_Z))
    sec = result.section(line)
    xs = sorted([v.X for v in sec.Vertexes]) if sec.Vertexes else []
    print(f"  Y={y:.0f}: X={[f'{x:.1f}' for x in xs]}")

bb2 = result.BoundBox
print(f"\nBoundBox: X[{bb2.XMin:.2f},{bb2.XMax:.2f}] Y[{bb2.YMin:.2f},{bb2.YMax:.2f}] Z[{bb2.ZMin:.2f},{bb2.ZMax:.2f}]")

# === EXPORT ===
step_f = shell_dir + "3DShell_GoodmanHPv3_T_3.5mm_TC_hinged.step"
stl_f = shell_dir + "3DShell_GoodmanHPv3_T_3.5mm_TC_hinged.stl"
result.exportStep(step_f)
mesh = MeshPart.meshFromShape(Shape=result, LinearDeflection=0.1, AngularDeflection=0.5)
mesh.write(stl_f)
print(f"\nExported: {step_f}")
print(f"Exported: {stl_f}")
