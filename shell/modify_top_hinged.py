"""Hinged top shell: knuckle hinge on left wall, snap clip pockets on right wall."""
import Part
import MeshPart
from FreeCAD import Vector

import os, sys
shell_dir = os.path.dirname(os.path.abspath(__file__)) + "/"
sys.path.insert(0, shell_dir)

from cutout import (wall_ring, drill_hole_lateral, reposition_cutout,
                    max6675_mount, labeled_cutout, deboss_text,
                    verify_cutout, verify_solid, z_probe, print_volume,
                    knuckle_hinge, snap_clip, coved_corner_countersink)
from hinge_config import (NX_L, NX_R, OY_F, OY_B,
                           HINGE_Z, HINGE_A_START, HINGE_A_END,
                           KNUCKLE_COUNT, KNUCKLE_RADIUS, PIN_RADIUS,
                           KNUCKLE_CLEARANCE, PIN_AXIS_X, PIN_AXIS_Z,
                           CLIP_Z, CLIP_Y_POSITIONS, CLIP_WIDTH,
                           CLIP_HEIGHT, CLIP_DEPTH, CLIP_LIP_HEIGHT,
                           CLIP_LIP_DEPTH, WALL_THICK)

top_orig = Part.read(shell_dir + "3DShell_GoodmanHPv3_T_3mm_TC.step")
bb = top_orig.BoundBox

OX_L = -98.262; IX_L = -95.262; IX_R = 1.917; OX_R = 4.917
OY_F = -32.48;  IY_F = -29.48;  IY_B = 94.12;  OY_B = 97.12
THICK = 0.5
NY_F = OY_F - THICK; NY_B = OY_B + THICK
NEW_HALF = 1.75
NMX_L = IX_L - NEW_HALF; NMX_R = IX_R + NEW_HALF
NMY_F = IY_F - NEW_HALF; NMY_B = IY_B + NEW_HALF
TOP_TONGUE_Z = 32.0; TOP_RIM_Z = 35.0  # 3mm tongue (32-35) matches bottom groove (31-35)
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

print(f"Pin axis (from hinge_config): X={PIN_AXIS_X:.3f}  Z={PIN_AXIS_Z:.3f}")

# === 1-4. Mating profile (same as non-hinged) ===
# 1. Remove tongue (clean from Z=27 to catch all original geometry below new tongue)
ORIG_CLEANUP_Z = 27.0
result = top_orig.cut(wall_ring(OX_L-2, OY_F-2, OX_R+2, OY_B+2,
                                IX_L, IY_F, IX_R, IY_B,
                                ORIG_CLEANUP_Z, TOP_RIM_Z+0.01))
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

# === 4c. Remove left wall below barrel ===
# With TOP_RIM_Z = HINGE_Z = 35, the mating profile steps already clear
# everything below 35 on all walls. This cut is belt-and-suspenders cleanup.
BARREL_Z_BOTTOM = HINGE_Z - KNUCKLE_RADIUS  # 32.0
hinge_cut = Part.makeBox(
    (IX_L + 1) - (NX_L - 1),                       # X: generous span through left wall
    (NY_B + 1) - (NY_F - 1),                        # Y: full span including corners
    HINGE_Z - (TOP_TONGUE_Z - 0.5),                # Z: 27.5→35.0 (mating line)
    Vector(NX_L - 1, NY_F - 1, TOP_TONGUE_Z - 0.5))
result = result.cut(hinge_cut)
print_volume(result, "After mating profile (left wall cleared to barrel)")

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

# === 8. Knuckle hinge on left wall (odd knuckles: 1, 3) ===
# Struts run from barrel up to ceiling for full stress transfer
result = knuckle_hinge(result, wall='left', wall_coord=NX_L,
                       a_start=HINGE_A_START, a_end=HINGE_A_END,
                       z_center=HINGE_Z,
                       knuckle_count=KNUCKLE_COUNT,
                       knuckle_radius=KNUCKLE_RADIUS,
                       pin_radius=PIN_RADIUS,
                       clearance=KNUCKLE_CLEARANCE,
                       side="top",
                       strut_z_max=CEIL_Z)
print_volume(result, "After hinge knuckles + struts")

# === 9. Snap clip pockets on right wall ===
for i, clip_y in enumerate(CLIP_Y_POSITIONS):
    result = snap_clip(result, wall='right', wall_coord=NX_R,
                       a_center=clip_y, z_center=CLIP_Z,
                       clip_width=CLIP_WIDTH, clip_height=CLIP_HEIGHT,
                       clip_depth=CLIP_DEPTH, lip_height=CLIP_LIP_HEIGHT,
                       lip_depth=CLIP_LIP_DEPTH, wall_thick=WALL_THICK,
                       side="top")
    print_volume(result, f"After snap pocket at Y={clip_y}")

# === 10. Countersunk M2 screw mount in front-right corner ===
SCREW_X = NMX_R + 0.5   # offset so cove extends 1.5mm past interior right wall
SCREW_Y = NMY_F - 0.5   # offset so cove extends 1.5mm past interior front wall
MATING_Z = 35.0   # same Z as left hinged wall top (BOT_RIM_Z)
SCREW_GAP = 0.2   # gap between top and bottom pillars for secure clamping
BOTTOM_R = 2.5    # bottom pillar radius (must match bottom shell)
TOP_R = 2.0       # 1.5mm visible protrusion, 0.8mm wall around clearance hole
SCREW_PILLAR_Z = TOP_RIM_Z                     # 35.0 — cove bottom at mating surface

RECESS_DEPTH = CEIL_Z - SCREW_PILLAR_Z - 1.5   # leave 1.5mm solid above mating edge
result = coved_corner_countersink(result, SCREW_X, SCREW_Y,
                                  z_bottom=SCREW_PILLAR_Z, z_outer=CEIL_Z,
                                  radius=TOP_R, recess_depth=RECESS_DEPTH,
                                  clearance_radius=1.2,
                                  wall_x=NX_R, wall_y=NY_F)
print_volume(result, "After M2 coved corner countersink (front-right)")

# === 10b. Restore groove profile on 3 walls (not hinge/left) ===
# Re-cut outer groove — same as step 3 — in case cup affected front-right corner
result = result.cut(wall_ring(NX_L-1, NY_F-1, NX_R+1, NY_B+1,
                              NMX_L, NMY_F, NMX_R, NMY_B,
                              TOP_TONGUE_Z-0.1, TOP_RIM_Z))
print_volume(result, "After groove restore")

# === VERIFICATION ===
print("\n=== VERIFICATION ===")

# Pin axis alignment check — probe through each knuckle center
print(f"Pin axis alignment (expected X={PIN_AXIS_X:.3f}, Z={PIN_AXIS_Z:.3f}):")
total_length = abs(HINGE_A_END - HINGE_A_START)
seg_length = (total_length - KNUCKLE_CLEARANCE * (KNUCKLE_COUNT - 1)) / KNUCKLE_COUNT
a_min = min(HINGE_A_START, HINGE_A_END)
for i in range(1, KNUCKLE_COUNT, 2):  # top knuckles: 1, 3
    seg_start = a_min + i * (seg_length + KNUCKLE_CLEARANCE)
    seg_mid_y = seg_start + seg_length / 2
    # Probe in X direction through pin center
    line = Part.makeLine(Vector(PIN_AXIS_X - 5, seg_mid_y, PIN_AXIS_Z),
                         Vector(PIN_AXIS_X + 5, seg_mid_y, PIN_AXIS_Z))
    sec = result.section(line)
    xs = sorted([v.X for v in sec.Vertexes]) if sec.Vertexes else []
    print(f"  Knuckle {i} (Y={seg_mid_y:.1f}): X={[f'{x:.2f}' for x in xs]}")

# Hinge barrel Z extent check
print(f"Hinge barrel Z extent (expected bottom={HINGE_Z - KNUCKLE_RADIUS:.1f}, top={HINGE_Z + KNUCKLE_RADIUS:.1f}):")
for i in range(1, KNUCKLE_COUNT, 2):  # top knuckles: 1, 3
    seg_start = a_min + i * (seg_length + KNUCKLE_CLEARANCE)
    seg_mid_y = seg_start + seg_length / 2
    # Z probe at barrel center X
    zs = z_probe(result, PIN_AXIS_X, seg_mid_y)
    print(f"  Knuckle {i} (Y={seg_mid_y:.1f}): Z={[f'{z:.2f}' for z in zs]}")
# Left wall Z at non-knuckle position (between knuckles)
gap_y = a_min + 0.5 * (seg_length + KNUCKLE_CLEARANCE) + seg_length  # gap between knuckle 0 and 1
zs_wall = z_probe(result, NX_L - 1.0, gap_y)
zs_wall2 = z_probe(result, NX_L + 1.0, gap_y)
print(f"  Left wall exterior (X={NX_L-1:.1f}, Y={gap_y:.1f}): Z={[f'{z:.2f}' for z in zs_wall]}")
print(f"  Left wall interior (X={NX_L+1:.1f}, Y={gap_y:.1f}): Z={[f'{z:.2f}' for z in zs_wall2]}")

# M2 boss verification (probes in interior where boss extends)
print("M2 boss verification:")
# Probe wall side (+X from center): should be solid wall/boss material
verify_solid(result, SCREW_X + 1.0, SCREW_Y,
             SCREW_PILLAR_Z - 1, CEIL_Z, "boss wall-side (+X)")
# Probe wall side (-Y from center)
verify_solid(result, SCREW_X, SCREW_Y - 1.0,
             SCREW_PILLAR_Z - 1, CEIL_Z, "boss wall-side (-Y)")
# Screw hole center: should be OPEN (clearance drilled)
zs = z_probe(result, SCREW_X, SCREW_Y)
print(f"  screw center: Z={[f'{z:.2f}' for z in zs]}")

# TC mount
pil_x = bb.XMin + 35
pil_y = bb.YMax - 30
for dx, dy, label in [(0, 0, "hole center"), (2, 0, "pillar wall")]:
    zs = z_probe(result, pil_x + dx, pil_y + dy)
    print(f"  {label}: Z={[f'{z:.2f}' for z in zs]}" if zs else f"  {label}: OPEN")

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
