"""ECO-2 on the working board: make U50's AGND pad plated-through (4.2 mm),
in the board and in the project library. Keeps all hand edits.

  flatpak run --command=python3 org.kicad.KiCad tools/eco2_u50_hole.py
"""
import os
from pathlib import Path

import pcbnew

HERE = Path(__file__).resolve().parents[1]
PCB = HERE / "GoodmanHP.kicad_pcb"
MM = pcbnew.FromMM


def make_pth(pad):
    pad.SetAttribute(pcbnew.PAD_ATTRIB_PTH)
    pad.SetDrillShape(pcbnew.PAD_DRILL_SHAPE_CIRCLE)
    pad.SetDrillSize(pcbnew.VECTOR2I(MM(4.2), MM(4.2)))
    pad.SetLayerSet(pad.PTHMask())


board = pcbnew.LoadBoard(str(PCB))
fp = board.FindFootprintByReference("U50")
for pad in fp.Pads():
    if pad.GetNumber() == "1":
        make_pth(pad)
        print("board U50 pad 1:", pad.GetNetname(), "PTH", pcbnew.ToMM(pad.GetDrillSize().x), "mm drill,",
              pcbnew.ToMM(pad.GetSize(pcbnew.F_Cu).x), "mm pad", flush=True)
board.Save(str(PCB))

io = pcbnew.PCB_IO_KICAD_SEXPR()
lib = str(HERE / "GoodmanHP.pretty")
name = fp.GetFPID().GetLibItemName().wx_str()
lf = io.FootprintLoad(lib, name)
for pad in lf.Pads():
    if pad.GetNumber() == "1":
        make_pth(pad)
lf.SetAttributes(pcbnew.FP_THROUGH_HOLE)
io.FootprintSave(lib, lf)
fp.SetAttributes(pcbnew.FP_THROUGH_HOLE)
board.Save(str(PCB))
print("library footprint updated:", name, flush=True)
os._exit(0)
