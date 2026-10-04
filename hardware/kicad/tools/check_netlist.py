"""Compare KiCad's exported netlist with the v8 model: every pin must sit in
the same group of connected pins (net names may differ for local labels)."""
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
net_file = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE / "GoodmanHP.net"
model = json.loads((HERE / "model.json").read_text())["parts"]

text = net_file.read_text()
kicad = {}
nets_part = text[text.index("(nets"):]
for block in re.split(r"\n\s*\(net\s*\n", nets_part)[1:]:
    name = re.search(r'\(name "([^"]*)"\)', block).group(1)
    pins = frozenset(re.findall(r'\(ref "([^"]+)"\)\s*\(pin "([^"]+)"\)', block))
    if pins:
        kicad[pins] = name

src = {}
for d, p in model.items():
    for pad in p["pads"]:
        if pad["net"]:
            src.setdefault(pad["net"], set()).add((d, pad["num"]))
src_groups = {frozenset(v): k for k, v in src.items()}

missing = [src_groups[g] for g in src_groups if g not in kicad]
extra = [kicad[g] for g in kicad if g not in src_groups and len(g) > 1]
print(f"model nets: {len(src_groups)}  kicad nets (2+ pins): {sum(1 for g in kicad if len(g) > 1)}")
print(f"mismatched model nets: {len(missing)}  unexpected kicad nets: {len(extra)}")
for n in missing[:20]:
    print("  model net not reproduced:", n)
for n in extra[:20]:
    print("  kicad net not in model:", n)
sys.exit(1 if missing or extra else 0)
