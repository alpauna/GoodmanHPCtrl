"""Hinged bottom shell: knuckle hinge on left wall, snap clips on right wall."""
import Part
import MeshPart
from FreeCAD import Vector

import os, sys
shell_dir = os.path.dirname(os.path.abspath(__file__)) + "/"
sys.path.insert(0, shell_dir)

from cutout import (wall_ring, wall_opening, wall_cutout, drill_hole_lateral,
                    knuckle_hinge, snap_clip, drill_hole, print_volume)
from hinge_config import (NX_L, NX_R, OY_F, OY_B,
                           HINGE_Z, HINGE_A_START, HINGE_A_END,
                           KNUCKLE_COUNT, KNUCKLE_RADIUS, PIN_RADIUS,
                           KNUCKLE_CLEARANCE, PIN_AXIS_X, PIN_AXIS_Z,
                           CLIP_Z, CLIP_Y_POSITIONS, CLIP_WIDTH,
                           CLIP_HEIGHT, CLIP_DEPTH, CLIP_LIP_HEIGHT,
                           CLIP_LIP_DEPTH, WALL_THICK)

bot_orig = Part.read(shell_dir + "3DShell_GoodmanHPv3_B_3mm.step")
bb = bot_orig.BoundBox

OX_L = -98.262; IX_L = -95.262; IX_R = 1.917; OX_R = 4.917
OY_F = -32.48;  IY_F = -29.48;  IY_B = 94.12;  OY_B = 97.12
THICK = 0.5; CLR = 0.10
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

print(f"Original volume: {bot_orig.Volume:.0f}")
print(f"BoundBox: X[{bb.XMin:.2f},{bb.XMax:.2f}] Y[{bb.YMin:.2f},{bb.YMax:.2f}] Z[{bb.ZMin:.2f},{bb.ZMax:.2f}]")
print(f"Pin axis (from hinge_config): X={PIN_AXIS_X:.3f}  Z={PIN_AXIS_Z:.3f}")

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

# NOTE: No tongue removal on left wall — tongue stays to keep wall height
# level with other 3 sides. Top shell has no groove here (step 4c), so
# the tongue is just a flat extension. Hinge provides alignment.

# === 7. Knuckle hinge on left wall (even knuckles: 0, 2, 4) ===
# Struts run from barrel down to shell floor for full stress transfer
result = knuckle_hinge(result, wall='left', wall_coord=NX_L,
                       a_start=HINGE_A_START, a_end=HINGE_A_END,
                       z_center=HINGE_Z,
                       knuckle_count=KNUCKLE_COUNT,
                       knuckle_radius=KNUCKLE_RADIUS,
                       pin_radius=PIN_RADIUS,
                       clearance=KNUCKLE_CLEARANCE,
                       side="bottom",
                       strut_z_min=FLOOR_Z)
print_volume(result, "7. After hinge knuckles + struts")

# === 8. Snap clips on right wall ===
for i, clip_y in enumerate(CLIP_Y_POSITIONS):
    result = snap_clip(result, wall='right', wall_coord=NX_R,
                       a_center=clip_y, z_center=CLIP_Z,
                       clip_width=CLIP_WIDTH, clip_height=CLIP_HEIGHT,
                       clip_depth=CLIP_DEPTH, lip_height=CLIP_LIP_HEIGHT,
                       lip_depth=CLIP_LIP_DEPTH, wall_thick=WALL_THICK,
                       side="bottom")
    print_volume(result, f"8.{i+1}. After snap clip at Y={clip_y}")

# === 9. M2 screw post in front-right corner (matches top shell) ===
SCREW_X = -1.0
SCREW_Y = -27.5   # embeds into front wall (IY_F=-29.48)
MATING_Z = 35.0   # same Z as left hinged wall top (BOT_RIM_Z)
BOTTOM_R = 2.5
SCREW_POST_H = MATING_Z - FLOOR_Z  # floor up to mating line

# Standoff post rising from floor to mating line
post = Part.makeCylinder(BOTTOM_R, SCREW_POST_H,
                         Vector(SCREW_X, SCREW_Y, FLOOR_Z))
result = result.fuse(post)
# M2 pilot hole (0.85mm radius for self-tapping)
result = drill_hole(result, SCREW_X, SCREW_Y,
                    FLOOR_Z, MATING_Z, 0.85, z_margin=0.5)
print_volume(result, "9. After M2 screw post (front-right)")

# === 9b. Restore tongue profile — direct box cuts per wall ===
# Inner cut zone: between NMX/NMY (outer) and IX/IY (inner), Z=31→35.
# Build all cuts into one compound tool to minimize boolean ops on complex shape.
# Use 0.01mm offsets to avoid coincident faces with inner wall surfaces.
CUT_Z0 = BOT_GROOVE_Z - 0.1
CUT_Z1 = BOT_RIM_Z + 0.01
CUT_H = CUT_Z1 - CUT_Z0
E = 0.01  # epsilon to avoid coincident faces
# Front wall inner strip: full X width, NMY_F to IY_F in Y
front_cut = Part.makeBox(NMX_R - NMX_L + 2*E, IY_F - NMY_F + E, CUT_H,
                         Vector(NMX_L - E, NMY_F - E, CUT_Z0))
# Right wall inner strip: IX_R to NMX_R in X, full Y width
right_cut = Part.makeBox(NMX_R - IX_R + E, NMY_B - NMY_F + 2*E, CUT_H,
                         Vector(IX_R - E, NMY_F - E, CUT_Z0))
# Back wall inner strip: full X width, IY_B to NMY_B in Y
back_cut = Part.makeBox(NMX_R - NMX_L + 2*E, NMY_B - IY_B + E, CUT_H,
                        Vector(NMX_L - E, IY_B - E, CUT_Z0))
# Left wall skipped — hinge side keeps tongue for wall height
# Fuse tool pieces together, then cut once
tool = front_cut.fuse(right_cut).fuse(back_cut)
result = result.cut(tool)
# Tongue clearance trim (same as step 6)
trim_out = wall_ring(NX_L, NY_F, NX_R, NY_B,
                     NX_L+CLR, NY_F+CLR, NX_R-CLR, NY_B-CLR,
                     BOT_GROOVE_Z, BOT_RIM_Z)
trim_in = wall_ring(NMX_L-CLR, NMY_F-CLR, NMX_R+CLR, NMY_B+CLR,
                    NMX_L, NMY_F, NMX_R, NMY_B,
                    BOT_GROOVE_Z, BOT_RIM_Z)
trim_tool = trim_out.fuse(trim_in)
result = result.cut(trim_tool)
print_volume(result, "9b. After tongue restore")

# === 10. Additional low voltage wire access (front wall + brim) ===
# 75mm wide (X) x 18mm tall (Z), centered in X, offset 5mm above Z center
# Interior brim on top/left/right: 2mm height, 3mm depth
WALL_CX = (OX_L + OX_R) / 2                     # -46.67 — shell center X
WALL_CZ = (FLOOR_Z + BOT_RIM_Z) / 2 + 5.0      #  21.75 — Z center + 5mm offset
LV_W = 75.0; LV_H = 15.0
result = wall_cutout(result, wall='front', wall_coord=NY_F,
                     a1=WALL_CX - LV_W/2, a2=WALL_CX + LV_W/2,
                     z1=WALL_CZ - LV_H/2, z2=WALL_CZ + LV_H/2,
                     wall_thick=IY_F - NY_F,
                     brim_sides={'top', 'left', 'right'},
                     brim_inside=True, brim_height=2.0, brim_depth=3.0)
print_volume(result, "10. After LV wire access + brim (front wall)")

# === VERIFICATION ===
print("\n=== VERIFICATION ===")

# Tongue profile check near screw post
print("Tongue profile near screw post (inner cut zone should be OPEN):")
from cutout import z_probe, verify_solid
# Probe inner cut zone on front wall near post: between IY_F and NMY_F
for px, py, label in [
    (0.0, -30.5, "front inner cut (X=0, Y=-30.5)"),      # center of inner cut zone
    (SCREW_X, -30.0, "post edge Y (X=-1, Y=-30)"),        # post Y edge
    (0.0, -29.9, "just inside IY_F (X=0, Y=-29.9)"),      # near inner boundary
    (0.0, -31.0, "tongue body (X=0, Y=-31.0)"),            # should be SOLID (tongue)
]:
    zs = z_probe(result, px, py)
    hits_in_tongue = [z for z in zs if BOT_GROOVE_Z - 0.5 <= z <= BOT_RIM_Z + 0.5]
    print(f"  {label}: Z={[f'{z:.2f}' for z in hits_in_tongue]}"
          f" {'OPEN' if len(hits_in_tongue) < 2 else 'SOLID'}")

# Pin axis alignment check — probe through each knuckle center
print(f"Pin axis alignment (expected X={PIN_AXIS_X:.3f}, Z={PIN_AXIS_Z:.3f}):")
total_length = abs(HINGE_A_END - HINGE_A_START)
seg_length = (total_length - KNUCKLE_CLEARANCE * (KNUCKLE_COUNT - 1)) / KNUCKLE_COUNT
a_min = min(HINGE_A_START, HINGE_A_END)
for i in range(0, KNUCKLE_COUNT, 2):  # bottom knuckles: 0, 2, 4
    seg_start = a_min + i * (seg_length + KNUCKLE_CLEARANCE)
    seg_mid_y = seg_start + seg_length / 2
    # Probe in X direction through pin center
    line = Part.makeLine(Vector(PIN_AXIS_X - 5, seg_mid_y, PIN_AXIS_Z),
                         Vector(PIN_AXIS_X + 5, seg_mid_y, PIN_AXIS_Z))
    sec = result.section(line)
    xs = sorted([v.X for v in sec.Vertexes]) if sec.Vertexes else []
    print(f"  Knuckle {i} (Y={seg_mid_y:.1f}): X={[f'{x:.2f}' for x in xs]}")

print("Right wall cutout (should be OPEN):")
for y, z in [(60, 15), (70, 10), (80, 20)]:
    line = Part.makeLine(Vector(0, y, z), Vector(10, y, z))
    sec = result.section(line)
    xn = sorted([v.X for v in sec.Vertexes]) if sec.Vertexes else []
    print(f"  Y={y},Z={z}: X={[f'{x:.1f}' for x in xn]}")

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
