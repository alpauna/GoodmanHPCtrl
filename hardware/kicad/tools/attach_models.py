"""Attach the easyeda2kicad 3D models to the board and library footprints.

The easyeda2kicad footprint for each LCSC part is compared pad-by-pad with
ours to find the translation/rotation between the two origins; the model
offset is corrected by the same amount.

  flatpak run --command=python3 org.kicad.KiCad tools/attach_models.py
"""
import json
import math
import os
from pathlib import Path

import pcbnew

HERE = Path(__file__).resolve().parents[1]
PCB = HERE / "GoodmanHP.kicad_pcb"
PARTS = json.loads((HERE / "model.json").read_text())["parts"]
MODELS = json.loads((HERE / "out" / "models.json").read_text())
LIBDIR = HERE / "GoodmanHP.pretty"


def local_pads(fp):
    """Pad centres in the footprint's own frame (mm, KiCad Y down)."""
    out = {}
    for p in fp.Pads():
        v = p.GetFPRelativePosition()
        out.setdefault(p.GetNumber(), []).append((pcbnew.ToMM(v.x), pcbnew.ToMM(v.y)))
    return out


def rot(pt, deg):
    a = math.radians(deg)
    x, y = pt
    # KiCad: positive angle is counter-clockwise on screen (Y down)
    return (x * math.cos(a) + y * math.sin(a), -x * math.sin(a) + y * math.cos(a))


def fit(mine, theirs):
    """Best (angle, tx, ty, residual) mapping their pads onto ours."""
    common = [n for n in mine if n in theirs]
    if not common:
        return None
    best = None
    for ang in (0, 90, 180, 270):
        pairs = [(mine[n][0], rot(theirs[n][0], ang)) for n in common]
        tx = sum(a[0] - b[0] for a, b in pairs) / len(pairs)
        ty = sum(a[1] - b[1] for a, b in pairs) / len(pairs)
        res = max(math.hypot(a[0] - b[0] - tx, a[1] - b[1] - ty) for a, b in pairs)
        if best is None or res < best[3]:
            best = (ang, tx, ty, res)
    return best


def make_model(info, ang, tx, ty):
    m = info["model"]
    ox, oy, oz = m["offset"]                 # 3D offset: X right, Y up
    # rotate their model offset with the footprint, then add the shift
    px, py = rot((ox, -oy), ang)
    model = pcbnew.FP_3DMODEL()
    model.m_Filename = "${KIPRJMOD}/GoodmanHP.3dshapes/" + m["file"]
    model.m_Offset = pcbnew.VECTOR3D(px + tx, -(py + ty), oz)
    rx, ry, rz = m["rotate"]
    model.m_Rotation = pcbnew.VECTOR3D(rx, ry, rz + ang)
    model.m_Scale = pcbnew.VECTOR3D(1, 1, 1)
    model.m_Show = True
    return model


board = pcbnew.LoadBoard(str(PCB))
io = pcbnew.PCB_IO_KICAD_SEXPR()
done, skipped, by_fp = 0, [], {}
for fp in board.GetFootprints():
    ref = fp.GetReference()
    part = PARTS.get(ref)
    if not part:
        continue
    info = MODELS.get(part["lcsc"])
    if info is None:       # fall back to a part with the same EasyEDA footprint
        same = [q["lcsc"] for q in PARTS.values() if q["footprint"] == part["footprint"] and q["lcsc"] in MODELS]
        if not same:
            skipped.append((ref, "no model downloaded"))
            continue
        info = MODELS[same[0]]
    theirs = {k: [tuple(p) for p in v] for k, v in info["pads"].items()}
    f = fit(local_pads(fp), theirs)
    if f is None or f[3] > (0.25 if len(theirs) <= 2 else 0.15):
        skipped.append((ref, f"pads don't line up (residual {f[3]:.2f} mm)" if f else "no common pads"))
        continue
    ang, tx, ty, _ = f
    fp.Models().clear()
    fp.Add3DModel(make_model(info, ang, tx, ty))
    by_fp.setdefault(fp.GetFPID().GetLibItemName().wx_str(), (info, ang, tx, ty))
    done += 1

# library copies, so the board and library footprints stay identical
for name, (info, ang, tx, ty) in by_fp.items():
    lib_fp = io.FootprintLoad(str(LIBDIR), name)
    if lib_fp is None:
        continue
    lib_fp.Models().clear()
    lib_fp.Add3DModel(make_model(info, ang, tx, ty))
    io.FootprintSave(str(LIBDIR), lib_fp)

board.Save(str(PCB))
print(f"3D models attached to {done} footprints; skipped {len(skipped)}", flush=True)
for s in skipped:
    print("  skipped:", *s, flush=True)
os._exit(0)
