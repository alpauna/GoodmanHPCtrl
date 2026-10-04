"""Build the footprint library and the KiCad PCB from model.json + links.json.

Run with KiCad's Python (flatpak):
  flatpak run --command=python3 org.kicad.KiCad tools/gen_pcb.py
"""
import json
import math
import re
from pathlib import Path

import pcbnew

HERE = Path(__file__).resolve().parents[1]
MODEL = json.loads((HERE / "model.json").read_text())["parts"]
LINKS = json.loads((HERE / "links.json").read_text())
PROJECT = "GoodmanHP"
LIBDIR = HERE / f"{PROJECT}.pretty"
OX, OY = 100.0, 100.0          # board mm -> KiCad: x' = x + OX, y' = OY - y
MM = pcbnew.FromMM

# v8 outline and non-plated holes (from the Gerbers / NPTH drill file)
OUTLINE = (-91.694, 0.127, -0.127, 91.313)
MOUNT_HOLES = [(-10.42403, 5.07746), (-4.96303, 66.16446), (-87.13203, 84.07146), (-87.38603, 23.87346)]
USB_PEGS = [(-6.38493, 78.26312), (-6.38492, 84.04314)]

HV_NETS = {"FAN-PWR", "FAN-OUT"}


def kpt(x, y):
    return pcbnew.VECTOR2I(MM(x + OX), MM(OY - y))


def fp_id(name):
    return re.sub(r"[^A-Za-z0-9_.+-]", "_", name)


def local(px, py, part):
    """Board mm (Y up) -> footprint-local mm (KiCad, Y down, unrotated)."""
    dx, dy = px - part["x"], -(py - part["y"])
    a = math.radians(part["rot"])
    # KiCad rotates footprints counter-clockwise on screen (Y down)
    lx = dx * math.cos(a) - dy * math.sin(a)
    ly = dx * math.sin(a) + dy * math.cos(a)
    return lx, ly


def make_footprint(name, part, board):
    fp = pcbnew.FOOTPRINT(board)
    fp.SetFPID(pcbnew.LIB_ID(PROJECT, fp_id(name)))
    smd_only = all(p["type"] == "SMD" for p in part["pads"])
    fp.SetAttributes(pcbnew.FP_SMD if smd_only else pcbnew.FP_THROUGH_HOLE)
    bottom = part["side"] == "B"
    for p in part["pads"]:
        pad = pcbnew.PAD(fp)
        pad.SetNumber(p["num"])
        lx, ly = local(p["x"], p["y"], part)
        if bottom:
            lx = -lx
        pad.SetPosition(pcbnew.VECTOR2I(MM(lx), MM(ly)))
        w, h = max(p["w"], 0.05), max(p["h"], 0.05)
        shape = {"R": pcbnew.PAD_SHAPE_RECT, "C": pcbnew.PAD_SHAPE_CIRCLE,
                 "O": pcbnew.PAD_SHAPE_OVAL, "P": pcbnew.PAD_SHAPE_OVAL}[p["shape"]]
        if p["shape"] == "C":
            w = h = max(w, h)
        if p["shape"] == "P":  # polygon pads (Faston tabs etc.) -> rounded rect of the same box
            shape = pcbnew.PAD_SHAPE_ROUNDRECT
        pad.SetShape(shape)
        pad.SetSize(pcbnew.VECTOR2I(MM(w), MM(h)))
        rel = (p["angle"] - part["rot"]) % 360
        if bottom:
            rel = (180 - rel) % 360
        pad.SetOrientationDegrees(rel)
        if p["type"] == "DIP" and p["drill"] > 0:
            pad.SetAttribute(pcbnew.PAD_ATTRIB_PTH)
            d = p["drill"]
            pad.SetDrillSize(pcbnew.VECTOR2I(MM(d), MM(d)))
            pad.SetLayerSet(pad.PTHMask())
        else:
            pad.SetAttribute(pcbnew.PAD_ATTRIB_SMD)
            pad.SetLayerSet(pad.SMDMask())
        fp.Add(pad)
    # fab outline = EasyEDA body box; courtyard = pads + body, whichever is smaller,
    # grown by 0.25 mm (EasyEDA body boxes include silkscreen and overlap a lot)
    w, h = part["bw"], part["bh"]
    xs, ys = [], []
    for p in part["pads"]:
        lx, ly = local(p["x"], p["y"], part)
        if bottom:
            lx = -lx
        r = max(p["w"], p["h"]) / 2
        xs += [lx - r, lx + r]
        ys += [ly - r, ly + r]
    cx0, cx1 = max(min(xs), -w / 2), min(max(xs), w / 2)
    cy0, cy1 = max(min(ys), -h / 2), min(max(ys), h / 2)
    if cx0 >= cx1 or cy0 >= cy1:
        cx0, cx1, cy0, cy1 = min(xs), max(xs), min(ys), max(ys)
    for layer, (ax, ay, bx, by), width in ((pcbnew.F_CrtYd, (cx0 - 0.25, cy0 - 0.25, cx1 + 0.25, cy1 + 0.25), 0.05),
                                           (pcbnew.F_Fab, (-w / 2, -h / 2, w / 2, h / 2), 0.1)):
        r = pcbnew.PCB_SHAPE(fp)
        r.SetShape(pcbnew.SHAPE_T_RECT)
        r.SetStart(pcbnew.VECTOR2I(MM(ax), MM(ay)))
        r.SetEnd(pcbnew.VECTOR2I(MM(bx), MM(by)))
        r.SetLayer(layer)
        r.SetWidth(MM(width))
        fp.Add(r)
    fp.Reference().SetPosition(pcbnew.VECTOR2I(0, MM(-h / 2 - 1.0)))
    fp.Reference().SetLayer(pcbnew.F_SilkS)
    fp.Value().SetPosition(pcbnew.VECTOR2I(0, MM(h / 2 + 1.0)))
    fp.Value().SetLayer(pcbnew.F_Fab)
    for t in (fp.Reference(), fp.Value()):
        t.SetTextSize(pcbnew.VECTOR2I(MM(0.8), MM(0.8)))
        t.SetTextThickness(MM(0.12))
    return fp


def main():
    board = pcbnew.BOARD()
    board.SetCopperLayerCount(6)
    ds = board.GetDesignSettings()
    ds.m_TrackMinWidth = MM(0.15)
    ds.m_ViasMinSize = MM(0.45)
    ds.m_MinClearance = MM(0.15)

    # footprint library: one footprint per EasyEDA footprint name
    LIBDIR.mkdir(exist_ok=True)
    io = pcbnew.PCB_IO_KICAD_SEXPR()
    def signature(p):
        sig = []
        for q in p["pads"]:
            lx, ly = local(q["x"], q["y"], p)
            sig.append((q["num"], round(lx, 3), round(ly, 3), round(q["w"], 3), round(q["h"], 3),
                        round((q["angle"] - p["rot"]) % 180, 1), q["type"], round(q["drill"], 3)))
        return tuple(sorted(sig)) + (p["side"],)

    variants, fp_name = {}, {}
    for d, p in sorted(MODEL.items()):
        sig = signature(p)
        names = variants.setdefault(p["footprint"], [])
        for i, (s2, nm) in enumerate(names):
            if s2 == sig:
                fp_name[d] = nm
                break
        else:
            nm = p["footprint"] if not names else f"{p['footprint']}_v{len(names) + 1}"
            names.append((sig, nm))
            fp_name[d] = nm
            lib_fp = make_footprint(nm, p, board)
            if p["side"] == "B":
                lib_fp.Flip(lib_fp.GetPosition(), pcbnew.FLIP_DIRECTION_LEFT_RIGHT)
            io.FootprintSave(str(LIBDIR), lib_fp)
    rep = {n for v in variants.values() for _, n in v}

    nets = {}

    def net(name):
        if name not in nets:
            n = pcbnew.NETINFO_ITEM(board, name)
            board.Add(n)
            nets[name] = n
        return nets[name]

    for d, p in sorted(MODEL.items()):
        fp = make_footprint(fp_name[d], p, board)
        fp.SetReference(d)
        fp.SetValue(p["mpn"] or p["value"])
        fp.SetPosition(kpt(p["x"], p["y"]))
        fp.SetOrientationDegrees(p["rot"] if p["side"] == "T" else p["rot"])
        board.Add(fp)
        if p["side"] == "B":
            fp.Flip(fp.GetPosition(), pcbnew.FLIP_DIRECTION_LEFT_RIGHT)
        path = pcbnew.KIID_PATH(f"/{LINKS['sheet_uuid'][LINKS['sheet_of'][d]]}/{LINKS['sym_uuid'][d]}")
        fp.SetPath(path)
        fp.SetField("LCSC", p["lcsc"])
        f = fp.GetField("LCSC")
        if f is not None:
            f.SetVisible(False)
            f.SetLayer(pcbnew.F_Fab)
        by_num = {}
        for pad in p["pads"]:
            by_num.setdefault(pad["num"], pad["net"])
        for pad in fp.Pads():
            n = by_num.get(pad.GetNumber(), "")
            if n:
                pad.SetNet(net(n))

    # outline
    x1, y1, x2, y2 = OUTLINE
    for (ax, ay), (bx, by) in (((x1, y1), (x2, y1)), ((x2, y1), (x2, y2)), ((x2, y2), (x1, y2)), ((x1, y2), (x1, y1))):
        s = pcbnew.PCB_SHAPE(board)
        s.SetShape(pcbnew.SHAPE_T_SEGMENT)
        s.SetStart(kpt(ax, ay))
        s.SetEnd(kpt(bx, by))
        s.SetLayer(pcbnew.Edge_Cuts)
        s.SetWidth(MM(0.1))
        board.Add(s)

    # non-plated holes as stand-alone footprints
    for i, (hx, hy, dia) in enumerate([(x, y, 4.25) for x, y in MOUNT_HOLES] + [(x, y, 0.7) for x, y in USB_PEGS]):
        fp = pcbnew.FOOTPRINT(board)
        fp.SetReference(f"MH{i + 1}")
        fp.Reference().SetVisible(False)
        fp.SetAttributes(pcbnew.FP_EXCLUDE_FROM_BOM | pcbnew.FP_EXCLUDE_FROM_POS_FILES)
        pad = pcbnew.PAD(fp)
        pad.SetAttribute(pcbnew.PAD_ATTRIB_NPTH)
        pad.SetShape(pcbnew.PAD_SHAPE_CIRCLE)
        pad.SetSize(pcbnew.VECTOR2I(MM(dia), MM(dia)))
        pad.SetDrillSize(pcbnew.VECTOR2I(MM(dia), MM(dia)))
        pad.SetLayerSet(pad.UnplatedHoleMask())
        fp.Add(pad)
        fp.SetPosition(kpt(hx, hy))
        board.Add(fp)

    out = HERE / f"{PROJECT}.kicad_pcb"
    board.Save(str(out))

    # verify pad positions against the source data
    worst, bad = 0.0, []
    for d, p in MODEL.items():
        fp = board.FindFootprintByReference(d)
        got = {}
        for pad in fp.Pads():
            got.setdefault(pad.GetNumber(), []).append(pad.GetPosition())
        for src in p["pads"]:
            ex, ey = src["x"] + OX, OY - src["y"]
            dmin = min(math.hypot(pcbnew.ToMM(v.x) - ex, pcbnew.ToMM(v.y) - ey) for v in got[src["num"]])
            worst = max(worst, dmin)
            if dmin > 0.01:
                bad.append((d, src["num"], round(dmin, 3)))
    print(f"saved {out.name}: {len(MODEL)} parts, {len(nets)} nets, {len(rep)} footprints")
    print(f"pad position check: worst error {worst:.4f} mm, {len(bad)} pads off")
    for b in bad[:15]:
        print("  off:", b)


main()
