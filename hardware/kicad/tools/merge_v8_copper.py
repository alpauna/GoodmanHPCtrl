"""Copy the v8 routing (tracks, vias, pour outlines, keep-outs) from KiCad's
EasyEDA Pro import onto the clean rebuilt board.

  flatpak run --command=python3 org.kicad.KiCad tools/merge_v8_copper.py

Pours are rebuilt as single zones: the importer split the Inner1 GND plane
into 87 fragments, so that plane becomes one board-sized zone.
"""
import math
from collections import Counter
from pathlib import Path

import pcbnew

HERE = Path(__file__).resolve().parents[1]
PCB = HERE / "GoodmanHP.kicad_pcb"
SRC = HERE / "out" / "v8-import.kicad_pcb"
MM = pcbnew.FromMM

dst = pcbnew.LoadBoard(str(PCB))
src = pcbnew.LoadBoard(str(SRC))

# ------------------------------------------------ alignment via footprints
offsets = Counter()
pairs = []
for fp in src.GetFootprints():
    mine = dst.FindFootprintByReference(fp.GetReference())
    if mine is None:
        continue
    d = mine.GetPosition() - fp.GetPosition()
    offsets[(round(d.x / 1000), round(d.y / 1000))] += 1     # 1 um buckets
    pairs.append((fp, mine))
(ox_um, oy_um), hits = offsets.most_common(1)[0]
off = pcbnew.VECTOR2I(ox_um * 1000, oy_um * 1000)
print(f"offset {ox_um / 1000:.3f}, {oy_um / 1000:.3f} mm agreed by {hits}/{len(pairs)} footprints")

# check pads line up too (catches rotation / mirroring differences)
worst = 0.0
for fp, mine in pairs:
    mp = {}
    for p in mine.Pads():
        mp.setdefault(p.GetNumber(), []).append(p.GetPosition())
    for p in fp.Pads():
        pos = p.GetPosition() + off
        if p.GetNumber() in mp:
            dmin = min(math.hypot(pos.x - q.x, pos.y - q.y) for q in mp[p.GetNumber()]) / 1e6
            worst = max(worst, dmin)
print(f"worst pad misalignment after offset: {worst:.4f} mm")

# ---------------------------------------------------------------- reset
for t in list(dst.GetTracks()):
    dst.Remove(t)
for z in list(dst.Zones()):
    dst.Remove(z)

LAYER_MAP = {pcbnew.F_Cu: pcbnew.F_Cu, pcbnew.In1_Cu: pcbnew.In1_Cu, pcbnew.In2_Cu: pcbnew.In2_Cu,
             pcbnew.In3_Cu: pcbnew.In3_Cu, pcbnew.In4_Cu: pcbnew.In4_Cu, pcbnew.B_Cu: pcbnew.B_Cu}
for lid, name, ltype in ((pcbnew.In1_Cu, "In1.GND", pcbnew.LT_POWER), (pcbnew.In2_Cu, "In2.Signal", pcbnew.LT_SIGNAL),
                         (pcbnew.In3_Cu, "In3.Signal", pcbnew.LT_SIGNAL), (pcbnew.In4_Cu, "In4.Signal", pcbnew.LT_SIGNAL)):
    dst.SetLayerName(lid, name)
    dst.SetLayerType(lid, ltype)


def net(name):
    n = dst.FindNet(name)
    if n is None and name:
        n = pcbnew.NETINFO_ITEM(dst, name)
        dst.Add(n)
    return n


copied = Counter()
for t in src.GetTracks():
    if t.Type() == pcbnew.PCB_VIA_T:
        v = pcbnew.PCB_VIA(dst)
        v.SetPosition(t.GetPosition() + off)
        v.SetWidth(t.GetWidth(pcbnew.F_Cu))
        v.SetDrill(t.GetDrillValue())
        v.SetViaType(t.GetViaType())
        top, bot = t.TopLayer(), t.BottomLayer()
        v.SetLayerPair(LAYER_MAP.get(top, pcbnew.F_Cu), LAYER_MAP.get(bot, pcbnew.B_Cu))
        v.SetNet(net(t.GetNetname()))
        dst.Add(v)
        copied["via"] += 1
    elif t.Type() == pcbnew.PCB_TRACE_T:
        n = pcbnew.PCB_TRACK(dst)
        n.SetStart(t.GetStart() + off)
        n.SetEnd(t.GetEnd() + off)
        n.SetWidth(t.GetWidth())
        n.SetLayer(LAYER_MAP[t.GetLayer()])
        n.SetNet(net(t.GetNetname()))
        dst.Add(n)
        copied["track"] += 1
    elif t.Type() == pcbnew.PCB_ARC_T:
        a = pcbnew.PCB_ARC(dst)
        a.SetStart(t.GetStart() + off)
        a.SetMid(t.GetMid() + off)
        a.SetEnd(t.GetEnd() + off)
        a.SetWidth(t.GetWidth())
        a.SetLayer(LAYER_MAP[t.GetLayer()])
        a.SetNet(net(t.GetNetname()))
        dst.Add(a)
        copied["arc"] += 1
print("copied:", dict(copied))

# ECO: remove v8 copper that now lands on a pad with a different net
# (e.g. the SCLK stub to R51 pad 1, now SPCS). Walk the stub from the pad
# until it meets a via, another pad, or a branch.
import json as _json
changes = _json.loads((HERE / "model.json").read_text()).get("changes", {})
removed = 0
for ref, pads in changes.items():
    fp = dst.FindFootprintByReference(ref)
    for pad in fp.Pads():
        if pad.GetNumber() not in pads:
            continue
        old = None
        frontier = [None]
        seen = set()
        while frontier:
            pt = frontier.pop()
            for t in list(dst.GetTracks()):
                if t.Type() != pcbnew.PCB_TRACE_T or id(t) in seen or t.GetNetname() == pads[pad.GetNumber()]:
                    continue
                ends = (t.GetStart(), t.GetEnd())
                if pt is None:      # first hop: anything ending on the pad itself
                    hit = [e for e in ends if pad.HitTest(e)]
                else:
                    hit = [e for e in ends if (e - pt).EuclideanNorm() < 50000]
                if not hit or (old and t.GetNetname() != old):
                    continue
                old = old or t.GetNetname()
                other = ends[1] if hit[0] == ends[0] else ends[0]
                seen.add(id(t))
                dst.Remove(t)
                removed += 1
                # keep walking unless the far end reaches a via or another pad
                stop = any(v.Type() == pcbnew.PCB_VIA_T and (v.GetPosition() - other).EuclideanNorm() < 50000
                           for v in dst.GetTracks())
                stop = stop or any(q.HitTest(other) for f in dst.GetFootprints() for q in f.Pads() if q is not pad)
                users = sum(1 for u in dst.GetTracks() if u.Type() == pcbnew.PCB_TRACE_T and
                            min((u.GetStart() - other).EuclideanNorm(), (u.GetEnd() - other).EuclideanNorm()) < 50000)
                if not stop and users == 1:
                    frontier.append(other)
print(f"ECO: removed {removed} v8 track segment(s) from changed pads", flush=True)

# ---------------------------------------------------------------- zones
bbox = dst.GetBoardEdgesBoundingBox()


def new_zone(layer, netname, outline_pts=None, priority=0, clearance=0.2):
    z = pcbnew.ZONE(dst)
    z.SetLayer(layer)
    z.SetNet(net(netname))
    o = z.Outline()
    o.NewOutline()
    pts = outline_pts or [(bbox.GetLeft(), bbox.GetTop()), (bbox.GetRight(), bbox.GetTop()),
                          (bbox.GetRight(), bbox.GetBottom()), (bbox.GetLeft(), bbox.GetBottom())]
    for x, y in pts:
        o.Append(int(x), int(y))
    z.SetLocalClearance(MM(clearance))
    z.SetMinThickness(MM(0.15))         # close to EasyEDA's pour settings
    z.SetThermalReliefGap(MM(0.2))
    z.SetThermalReliefSpokeWidth(MM(0.3))
    z.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
    z.SetAssignedPriority(priority)
    z.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)
    dst.Add(z)
    return z


def outline_of(z):
    poly = z.Outline().Outline(0)
    return [(poly.CPoint(i).x + off.x, poly.CPoint(i).y + off.y) for i in range(poly.PointCount())]


new_zone(pcbnew.In1_Cu, "GND")                      # one solid inner plane
for lay in (pcbnew.F_Cu, pcbnew.B_Cu):              # outer ground pours: solid
    new_zone(lay, "GND", priority=0).SetPadConnection(pcbnew.ZONE_CONNECTION_FULL)  # pad joins, as v8 does
kept = Counter()
for z in src.Zones():
    layers = list(z.GetLayerSet().Seq())
    if z.GetIsRuleArea():
        # EasyEDA keep-outs here only stop copper pours; v8 routes tracks
        # through them. All-layer ones came in on Edge.Cuts -> all copper.
        k = pcbnew.ZONE(z)
        k.Move(off)
        if not any(pcbnew.IsCopperLayer(l) for l in layers) or len(layers) > 1:
            # all-layer keep-outs: not on F.Cu, where v8's own FAN-OUT pour
            # lives inside them (the 2.5 mm HV rule keeps GND away there)
            ls = pcbnew.LSET.AllCuMask(dst.GetCopperLayerCount())
            ls.RemoveLayer(pcbnew.F_Cu)
            k.SetLayerSet(ls)
        k.SetDoNotAllowTracks(False)
        k.SetDoNotAllowVias(False)
        k.SetDoNotAllowPads(False)
        k.SetDoNotAllowFootprints(False)
        k.SetDoNotAllowZoneFills(True)
        dst.Add(k)
        kept["keep-out"] += 1
    elif z.GetNetname() not in ("GND",) and len(layers) == 1:
        new_zone(LAYER_MAP[layers[0]], z.GetNetname(), outline_of(z), priority=5)
        kept[f"{z.GetNetname()} pour"] += 1
print("zones kept from v8:", dict(kept))

pcbnew.ZONE_FILLER(dst).Fill(dst.Zones())
dst.Save(str(PCB))
print("saved", PCB.name, flush=True)
import os
os._exit(0)   # skip SWIG teardown, which crashes after tracks were removed
