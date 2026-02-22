import Part
import MeshPart
from FreeCAD import Vector

import os, sys
shell_dir = os.path.dirname(os.path.abspath(__file__)) + "/"
sys.path.insert(0, shell_dir)

from cutout import wall_ring, drill_hole_lateral

bot_orig = Part.read(shell_dir + "3DShell_GoodmanHPv3_B_3mm.step")
bb = bot_orig.BoundBox

OX_L = -98.262; IX_L = -95.262; IX_R = 1.917; OX_R = 4.917
OY_F = -32.48;  IY_F = -29.48;  IY_B = 94.12;  OY_B = 97.12

# ---------------------------------------------------------------------------
# Wall cutout features (coordinates from probed original shell)
# ---------------------------------------------------------------------------
BOTTOM_FEATURES = {
    "ACCESS": {  # General access / venting
        "wall": "right", "y1": 47.0, "y2": 88.5, "z1": 7, "z2": 24,
    },
    "VENT": {  # Air vent slot
        "wall": "back", "x1": -89.5, "x2": -3.5, "z1": 14, "z2": 17,
    },
    "ANTENNA": {  # ESP32 antenna clearance
        "wall": "back", "x1": -50.0, "x2": -35.0, "z1": 6, "z2": 13,
    },
}
THICK = 0.5; CLR = 0.10
NX_L = OX_L - THICK; NX_R = OX_R + THICK
NY_F = OY_F - THICK; NY_B = OY_B + THICK
NEW_HALF = 1.75
NMX_L = IX_L - NEW_HALF; NMX_R = IX_R + NEW_HALF
NMY_F = IY_F - NEW_HALF; NMY_B = IY_B + NEW_HALF
MX_L = -96.762; MX_R = 3.417; MY_F = -30.98; MY_B = 95.62
BOT_GROOVE_Z = 31.0; BOT_RIM_Z = 35.0
FLOOR_Z = bb.ZMin  # actual floor Z

print(f"Original volume: {bot_orig.Volume:.0f}")
print(f"BoundBox: X[{bb.XMin:.2f},{bb.XMax:.2f}] Y[{bb.YMin:.2f},{bb.YMax:.2f}] Z[{bb.ZMin:.2f},{bb.ZMax:.2f}]")

# === 1. Create wall thickening ring (Z=FLOOR to Z=31) ===
# This adds 0.5mm to all 4 walls
thk_ring = wall_ring(NX_L, NY_F, NX_R, NY_B,
                     OX_L, OY_F, OX_R, OY_B,
                     FLOOR_Z, BOT_GROOVE_Z)
print(f"\n1. Raw thickening ring volume: {thk_ring.Volume:.0f}")

# === 2. Cut out holes matching the original wall cutouts ===
# These are openings in the original shell walls that the ring would fill

# Right wall (+X side, at X=OX_R=4.917): large cutout
# Mapped: Y=[47.0, 88.5], Z=[7, 24]
# Use generous margins and span the full ring thickness
right_cutout = Part.makeBox(
    THICK + 2,           # X span: through entire ring + margin
    88.5 - 47.0,         # Y span
    24 - 7,              # Z span
    Vector(OX_R - 0.5, 47.0, 7)  # start inside wall, go outward
)
thk_ring = thk_ring.cut(right_cutout)
print(f"2a. After right wall main cutout: {thk_ring.Volume:.0f}")

# Right wall small cutout near floor
# Mapped: Y=[4.5, 6.0], Z=2 (very small, Z~1-3)
right_small = Part.makeBox(
    THICK + 2,
    6.0 - 4.5 + 1,  # small margin
    3,               # Z=1 to Z=4
    Vector(OX_R - 0.5, 4.0, 1)
)
thk_ring = thk_ring.cut(right_small)
print(f"2b. After right wall small cutout: {thk_ring.Volume:.0f}")

# Back wall (+Y side, at Y=OY_B=97.12): lower cutout
# Mapped: X=[-50.0, -35.0], Z=[6, 13]
back_lower = Part.makeBox(
    -35.0 - (-50.0),    # X span
    THICK + 2,           # Y span: through entire ring + margin
    13 - 6,              # Z span
    Vector(-50.0, OY_B - 0.5, 6)
)
thk_ring = thk_ring.cut(back_lower)
print(f"2c. After back wall lower cutout: {thk_ring.Volume:.0f}")

# Back wall: upper cutout
# Mapped: X=[-89.5, -3.5], Z=[14, 17]
back_upper = Part.makeBox(
    -3.5 - (-89.5),     # X span
    THICK + 2,           # Y span
    17 - 14,             # Z span
    Vector(-89.5, OY_B - 0.5, 14)
)
thk_ring = thk_ring.cut(back_upper)
print(f"2d. After back wall upper cutout: {thk_ring.Volume:.0f}")

# Z=1 floor-level gaps (both walls have large gaps at Z=1)
# Right wall Z=1: Y=[-29, 94]
right_floor = Part.makeBox(
    THICK + 2,
    94 - (-29),
    2,  # Z=0 to Z=2
    Vector(OX_R - 0.5, -29, FLOOR_Z)
)
thk_ring = thk_ring.cut(right_floor)

# Back wall Z=1: X=[-95, -6]
back_floor = Part.makeBox(
    -6 - (-95),
    THICK + 2,
    2,
    Vector(-95, OY_B - 0.5, FLOOR_Z)
)
thk_ring = thk_ring.cut(back_floor)
print(f"2e. After floor-level cutouts: {thk_ring.Volume:.0f}")

# === 3. Fuse trimmed ring with original ===
result = bot_orig.fuse(thk_ring)
print(f"\n3. After fuse: {result.Volume:.0f}")

# === 4. Mating profile: fill groove, create outer tongue ===
groove_fill = wall_ring(NX_L, NY_F, NX_R, NY_B,
                        MX_L, MY_F, MX_R, MY_B,
                        BOT_GROOVE_Z, BOT_RIM_Z)
result = result.fuse(groove_fill)
print(f"4. Groove filled: {result.Volume:.0f}")

# === 5. Cut inner half (Z=31-35) to make tongue ===
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

# === VERIFICATION ===
print("\n=== VERIFICATION ===")
y_mid = (OY_F + OY_B) / 2

# Check cutouts are preserved
print("Right wall cutout (should be OPEN = fewer hits):")
for y, z in [(60, 15), (70, 10), (80, 20), (50, 12)]:
    line_o = Part.makeLine(Vector(0, y, z), Vector(10, y, z))
    line_n = Part.makeLine(Vector(0, y, z), Vector(10, y, z))
    sec_o = bot_orig.section(line_o)
    sec_n = result.section(line_n)
    xo = sorted([v.X for v in sec_o.Vertexes]) if sec_o.Vertexes else []
    xn = sorted([v.X for v in sec_n.Vertexes]) if sec_n.Vertexes else []
    match = "OK" if len(xo) == len(xn) else "MISMATCH"
    print(f"  Y={y},Z={z}: orig={[f'{x:.1f}' for x in xo]} new={[f'{x:.1f}' for x in xn]} {match}")

print("Back wall cutout (should be OPEN):")
for x, z in [(-45, 10), (-40, 8), (-50, 15), (-70, 15), (-20, 16)]:
    line_o = Part.makeLine(Vector(x, 88, z), Vector(x, 102, z))
    line_n = Part.makeLine(Vector(x, 88, z), Vector(x, 102, z))
    sec_o = bot_orig.section(line_o)
    sec_n = result.section(line_n)
    yo = sorted([v.Y for v in sec_o.Vertexes]) if sec_o.Vertexes else []
    yn = sorted([v.Y for v in sec_n.Vertexes]) if sec_n.Vertexes else []
    match = "OK" if len(yo) == len(yn) else "MISMATCH"
    print(f"  X={x},Z={z}: orig={[f'{y:.1f}' for y in yo]} new={[f'{y:.1f}' for y in yn]} {match}")

# Wall thickness where wall EXISTS (no cutout)
print("Wall thickness (solid sections):")
for z in [5, 20, 25]:
    line = Part.makeLine(Vector(NX_L-5, y_mid, z), Vector(NX_R+5, y_mid, z))
    sec = result.section(line)
    if sec.Vertexes:
        xs = sorted([v.X for v in sec.Vertexes])
        segs = []
        for i in range(0, len(xs)-1, 2):
            w = xs[i+1]-xs[i]
            if w > 0.01:
                segs.append(f"[{xs[i]:.2f},{xs[i+1]:.2f}]={w:.2f}mm")
        print(f"  Z={z}: {' | '.join(segs)}")

# Mating profile check
print("Mating profile (Z=32-34):")
for z in [32, 33, 34]:
    line = Part.makeLine(Vector(NX_L-5, y_mid, z), Vector(NX_R+5, y_mid, z))
    sec = result.section(line)
    if sec.Vertexes:
        xs = sorted([v.X for v in sec.Vertexes])
        segs = []
        for i in range(0, len(xs)-1, 2):
            w = xs[i+1]-xs[i]
            if w > 0.01:
                segs.append(f"[{xs[i]:.2f},{xs[i+1]:.2f}]={w:.2f}mm")
        print(f"  Z={z}: {' | '.join(segs)}")

# === 7. Drill M1.5 screw holes through left and right walls ===
# Original EasyEDA holes were at wall Y-midpoint, Z=32.65, lost during profile flip
result = drill_hole_lateral(result, axis='x', start=NX_L, end=NX_R,
                            cy=(OY_F + OY_B) / 2, cz=32.65, radius=0.75)
print(f"7. Screw holes drilled: {result.Volume:.0f}")

print(f"\nBoundBox: X[{result.BoundBox.XMin:.2f},{result.BoundBox.XMax:.2f}] Y[{result.BoundBox.YMin:.2f},{result.BoundBox.YMax:.2f}] Z[{result.BoundBox.ZMin:.2f},{result.BoundBox.ZMax:.2f}]")

# === EXPORT ===
step_f = shell_dir + "3DShell_GoodmanHPv3_B_3.5mm_flipped.step"
stl_f = shell_dir + "3DShell_GoodmanHPv3_B_3.5mm_flipped.stl"
result.exportStep(step_f)
mesh = MeshPart.meshFromShape(Shape=result, LinearDeflection=0.1, AngularDeflection=0.5)
mesh.write(stl_f)
print(f"\nExported: {step_f}")
print(f"Exported: {stl_f}")
