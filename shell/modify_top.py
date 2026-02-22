import Part
import MeshPart
from FreeCAD import Vector

import os
shell_dir = os.path.dirname(os.path.abspath(__file__)) + "/"

# Start fresh from the last good version (before the plug/redrill)
# Rebuild: original -> mating flip -> wall thicken -> pillar -> drill hole centered
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

def wall_ring(xol, yof, xor_, yob, xil, yif, xir, yib, zb, zt):
    outer = Part.makeBox(xor_-xol, yob-yof, zt-zb, Vector(xol, yof, zb))
    inner = Part.makeBox(xir-xil, yib-yif, zt-zb, Vector(xil, yif, zb))
    return outer.cut(inner)

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

# 5. Pillar - use original BB for coordinates
pil_x = bb.XMin + 35   # -63.76
pil_y = bb.YMax - 30    # 67.14
CEIL_INNER = 48.50
PILLAR_H = 3.0
pil_bottom = CEIL_INNER - PILLAR_H  # 45.50

# Pillar with overlap into ceiling, capped at ceiling surfaces
pillar = Part.makeCylinder(2.5, PILLAR_H + 2,
                           Vector(pil_x, pil_y, pil_bottom),
                           Vector(0, 0, 1))
# Trim above ceiling outer face
pillar = pillar.cut(Part.makeBox(20, 20, 10, Vector(pil_x-10, pil_y-10, CEIL_Z)))
result = result.fuse(pillar)

# 6. M3 hole - same center as pillar, bounded to pillar+ceiling only
hole = Part.makeCylinder(1.25, CEIL_Z - pil_bottom,
                         Vector(pil_x, pil_y, pil_bottom),
                         Vector(0, 0, 1))
result = result.cut(hole)

print(f"Volume: {result.Volume:.0f}")
print(f"ZMax: {result.BoundBox.ZMax:.2f}")

# Verify
for dx, dy, label in [(0, 0, "hole center"), (2, 0, "pillar wall")]:
    line = Part.makeLine(Vector(pil_x+dx, pil_y+dy, 55), Vector(pil_x+dx, pil_y+dy, 40))
    sec = result.section(line)
    if sec.Vertexes:
        zs = sorted([v.Z for v in sec.Vertexes])
        print(f"  {label}: Z={[f'{z:.2f}' for z in zs]}")
    else:
        print(f"  {label}: OPEN")

# Cutout check
line = Part.makeLine(Vector(-65, 93, 55), Vector(-65, 93, 40))
sec = result.section(line)
if sec.Vertexes:
    zs = sorted([v.Z for v in sec.Vertexes])
    print(f"  TC cutout: Z={[f'{z:.2f}' for z in zs]}")
else:
    print(f"  TC cutout: OPEN (preserved)")

# 7. Drill M1.5 screw holes through left and right walls at mating zone midpoint
HOLE_Y = (OY_F + OY_B) / 2  # 32.32
HOLE_Z = 32.65
HOLE_R = 0.75  # M1.5 = 1.5mm diameter
screw_hole = Part.makeCylinder(HOLE_R, NX_R - NX_L + 4,
                                Vector(NX_L - 2, HOLE_Y, HOLE_Z),
                                Vector(1, 0, 0))
result = result.cut(screw_hole)
print(f"Screw holes: volume={result.Volume:.0f}")

# 8. Reposition LCD cutout: shift bottom up 2mm, top up 1mm (27x16mm)
# Original EasyEDA cutout: X=[-38.48,-11.43] Y=[59.59,76.59] = 27.05x17.00
LCD_X1 = -38.48; LCD_X2 = -11.43
LCD_OLD_Y1 = 59.59; LCD_OLD_Y2 = 76.59
LCD_NEW_Y1 = LCD_OLD_Y1 + 2.0  # 61.59
LCD_NEW_Y2 = LCD_OLD_Y2 + 1.0  # 77.59
Z_BOT = CEIL_INNER - 0.2; Z_TOP = CEIL_Z + 0.2
# Plug old cutout (oversized XY for reliable fuse)
plug = Part.makeBox(LCD_X2 - LCD_X1 + 2, LCD_OLD_Y2 - LCD_OLD_Y1 + 2,
                    Z_TOP - Z_BOT,
                    Vector(LCD_X1 - 1, LCD_OLD_Y1 - 1, Z_BOT))
result = result.fuse(plug)
# Cut new cutout (same Z range, no residual layers)
lcd_cut = Part.makeBox(LCD_X2 - LCD_X1, LCD_NEW_Y2 - LCD_NEW_Y1,
                       Z_TOP - Z_BOT,
                       Vector(LCD_X1, LCD_NEW_Y1, Z_BOT))
result = result.cut(lcd_cut)
# Trim plug flush with ceiling outer surface (remove 0.2mm protrusion)
trim = Part.makeBox(LCD_X2 - LCD_X1 + 4, LCD_OLD_Y2 - LCD_OLD_Y1 + 4, 1,
                    Vector(LCD_X1 - 2, LCD_OLD_Y1 - 2, CEIL_Z))
result = result.cut(trim)
print(f"LCD repositioned: volume={result.Volume:.0f}")

step_f = shell_dir + "3DShell_GoodmanHPv3_T_3.5mm_TC_flipped.step"
stl_f = shell_dir + "3DShell_GoodmanHPv3_T_3.5mm_TC_flipped.stl"
result.exportStep(step_f)
mesh = MeshPart.meshFromShape(Shape=result, LinearDeflection=0.1, AngularDeflection=0.5)
mesh.write(stl_f)
print(f"\nExported: {step_f}")
