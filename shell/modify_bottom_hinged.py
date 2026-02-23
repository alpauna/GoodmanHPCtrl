"""Hinged bottom shell: knuckle hinge on left wall, snap clips on right wall."""
import Part
import MeshPart
from FreeCAD import Vector

import os, sys
shell_dir = os.path.dirname(os.path.abspath(__file__)) + "/"
sys.path.insert(0, shell_dir)

from cutout import (wall_ring, wall_opening, drill_hole_lateral,
                    knuckle_hinge, snap_clip, print_volume)

bot_orig = Part.read(shell_dir + "3DShell_GoodmanHPv3_B_3mm.step")
bb = bot_orig.BoundBox

OX_L = -98.262; IX_L = -95.262; IX_R = 1.917; OX_R = 4.917
OY_F = -32.48;  IY_F = -29.48;  IY_B = 94.12;  OY_B = 97.12
THICK = 0.5; CLR = 0.10
NX_L = OX_L - THICK; NX_R = OX_R + THICK
NY_F = OY_F - THICK; NY_B = OY_B + THICK
NEW_HALF = 1.75
NMX_L = IX_L - NEW_HALF; NMX_R = IX_R + NEW_HALF
NMY_F = IY_F - NEW_HALF; NMY_B = IY_B + NEW_HALF
MX_L = -96.762; MX_R = 3.417; MY_F = -30.98; MY_B = 95.62
BOT_GROOVE_Z = 31.0; BOT_RIM_Z = 35.0
FLOOR_Z = bb.ZMin

WALL_COORDS = {"left": OX_L, "right": OX_R, "front": OY_F, "back": OY_B}

BOTTOM_FEATURES = {
    "ACCESS": {"wall": "right", "y1": 47.0, "y2": 88.5, "z1": 7, "z2": 24},
    "ANTENNA": {"wall": "back", "x1": -50.0, "x2": -35.0, "z1": 6, "z2": 13},
    "VENT":    {"wall": "back", "x1": -89.5, "x2": -3.5, "z1": 14, "z2": 17},
}

# Hinge parameters
HINGE_Z = (BOT_GROOVE_Z + BOT_RIM_Z) / 2  # Z=33, midpoint of mating zone
HINGE_INSET = 5.0  # inset from wall ends to avoid corners
HINGE_A_START = OY_F + HINGE_INSET
HINGE_A_END = OY_B - HINGE_INSET
KNUCKLE_RADIUS = 3.0
PIN_RADIUS = 1.0  # ~2mm pin/1.75mm filament

# Snap clip parameters — two clips on right wall
CLIP_Z = 33.0  # same Z as hinge
CLIP_Y_POSITIONS = [20.0, 65.0]  # two clips spread along right wall

print(f"Original volume: {bot_orig.Volume:.0f}")
print(f"BoundBox: X[{bb.XMin:.2f},{bb.XMax:.2f}] Y[{bb.YMin:.2f},{bb.YMax:.2f}] Z[{bb.ZMin:.2f},{bb.ZMax:.2f}]")

# === 1. Wall thickening ring (same as non-hinged) ===
thk_ring = wall_ring(NX_L, NY_F, NX_R, NY_B,
                     OX_L, OY_F, OX_R, OY_B,
                     FLOOR_Z, BOT_GROOVE_Z)
print(f"\n1. Thickening ring: {thk_ring.Volume:.0f}")

# === 2. Cut wall openings to preserve original cutouts ===
for name, feat in BOTTOM_FEATURES.items():
    w = feat["wall"]
    if w in ("left", "right"):
        a1, a2 = feat["y1"], feat["y2"]
    else:
        a1, a2 = feat["x1"], feat["x2"]
    thk_ring = wall_opening(thk_ring, w, a1, a2, feat["z1"], feat["z2"],
                            wall_coord=WALL_COORDS[w], ring_thick=THICK)
    print(f"2. {name}: vol={thk_ring.Volume:.0f}")

# === 3. Fuse ring with original ===
result = bot_orig.fuse(thk_ring)
print(f"\n3. After fuse: {result.Volume:.0f}")

# === 4. Mating profile: fill groove, create outer tongue ===
groove_fill = wall_ring(NX_L, NY_F, NX_R, NY_B,
                        MX_L, MY_F, MX_R, MY_B,
                        BOT_GROOVE_Z, BOT_RIM_Z)
result = result.fuse(groove_fill)
print(f"4. Groove filled: {result.Volume:.0f}")

# === 5. Cut inner half (tongue) ===
inner_cut = wall_ring(NMX_L, NMY_F, NMX_R, NMY_B,
                      IX_L, IY_F, IX_R, IY_B,
                      BOT_GROOVE_Z, BOT_RIM_Z)
result = result.cut(inner_cut)
print(f"5. Inner cut (tongue): {result.Volume:.0f}")

# === 6. Trim tongue clearance ===
trim_out = wall_ring(NX_L, NY_F, NX_R, NY_B,
                     NX_L+CLR, NY_F+CLR, NX_R-CLR, NY_B-CLR,
                     BOT_GROOVE_Z, BOT_RIM_Z)
result = result.cut(trim_out)
trim_in = wall_ring(NMX_L-CLR, NMY_F-CLR, NMX_R+CLR, NMY_B+CLR,
                    NMX_L, NMY_F, NMX_R, NMY_B,
                    BOT_GROOVE_Z, BOT_RIM_Z)
result = result.cut(trim_in)
print(f"6. Tongue trimmed: {result.Volume:.0f}")

# === 7. Knuckle hinge on left wall ===
result = knuckle_hinge(result, wall='left', wall_coord=NX_L,
                       a_start=HINGE_A_START, a_end=HINGE_A_END,
                       z_center=HINGE_Z,
                       knuckle_count=5, knuckle_radius=KNUCKLE_RADIUS,
                       pin_radius=PIN_RADIUS, clearance=0.3,
                       side="bottom")
print_volume(result, "7. After hinge knuckles")

# === 8. Snap clips on right wall ===
for i, clip_y in enumerate(CLIP_Y_POSITIONS):
    result = snap_clip(result, wall='right', wall_coord=NX_R,
                       a_center=clip_y, z_center=CLIP_Z,
                       clip_width=8.0, clip_height=4.0, clip_depth=2.0,
                       lip_height=1.0, lip_depth=0.8,
                       wall_thick=3.5, side="bottom")
    print_volume(result, f"8.{i+1}. After snap clip at Y={clip_y}")

# === VERIFICATION ===
print("\n=== VERIFICATION ===")
y_mid = (OY_F + OY_B) / 2

print("Right wall cutout (should be OPEN):")
for y, z in [(60, 15), (70, 10), (80, 20)]:
    line = Part.makeLine(Vector(0, y, z), Vector(10, y, z))
    sec = result.section(line)
    xn = sorted([v.X for v in sec.Vertexes]) if sec.Vertexes else []
    print(f"  Y={y},Z={z}: X={[f'{x:.1f}' for x in xn]}")

print("Hinge knuckles (should have material on left exterior):")
for y in [HINGE_A_START + 5, y_mid, HINGE_A_END - 5]:
    line = Part.makeLine(Vector(NX_L - 10, y, HINGE_Z),
                         Vector(NX_L + 5, y, HINGE_Z))
    sec = result.section(line)
    xs = sorted([v.X for v in sec.Vertexes]) if sec.Vertexes else []
    print(f"  Y={y:.0f}: X={[f'{x:.1f}' for x in xs]}")

bb2 = result.BoundBox
print(f"\nBoundBox: X[{bb2.XMin:.2f},{bb2.XMax:.2f}] Y[{bb2.YMin:.2f},{bb2.YMax:.2f}] Z[{bb2.ZMin:.2f},{bb2.ZMax:.2f}]")

# === EXPORT ===
step_f = shell_dir + "3DShell_GoodmanHPv3_B_3.5mm_hinged.step"
stl_f = shell_dir + "3DShell_GoodmanHPv3_B_3.5mm_hinged.stl"
result.exportStep(step_f)
mesh = MeshPart.meshFromShape(Shape=result, LinearDeflection=0.1, AngularDeflection=0.5)
mesh.write(stl_f)
print(f"\nExported: {step_f}")
print(f"Exported: {stl_f}")
