"""Build a part/pad/net model of the GoodmanHP v8 board from EasyEDA exports.

Inputs (from docs/schematics):
  - Goodman_HP_InteractiveBOM_v8.html : designators, footprints, placement, body size
  - GoodmanHP-Schematic-v8-Gerber.zip : FlyingProbeTesting.json (every pad + net)

Output: model.json next to this script's parent directory.
"""
import json
import math
import re
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]          # GoodmanHPCtrl/
SCH = ROOT / "docs" / "schematics"
OUT = Path(__file__).resolve().parents[1] / "model.json"
MIL = 0.0254

# ECO-1: microSD chip select had no pull-up while CLK had one. Move R51
# (10k) from SCLK to SPCS so the card stays deselected during ESP32 reset.
CHANGES = {"R51": {"1": "SPCS"}}

# ECO-2: the U50 SMT standoff footprint had a 4.2 mm plated hole in EasyEDA,
# but it never reached the v8 drill files, so the In4 AGND trace stopped
# under a top-only pad. Make the pad plated-through so AGND reaches it.
PAD_CHANGES = {"U50": {"1": {"type": "DIP", "drill": 4.2}}}


def load_bom():
    s = (SCH / "Goodman_HP_InteractiveBOM_v8.html").read_text(encoding="utf-8")
    i = s.find("window.files = ") + len("window.files = ")
    files, _ = json.JSONDecoder().raw_decode(s[i:])
    return json.loads(json.loads(files["bom_merge"])["data"])


def load_probe():
    with zipfile.ZipFile(SCH / "GoodmanHP-Schematic-v8-Gerber.zip") as z:
        d = json.loads(z.read("FlyingProbeTesting.json"))
    comps = [dict(zip(d["components"]["fields"], r)) for r in d["components"]["rows"]]
    pins = [dict(zip(d["pins"]["fields"], r)) for r in d["pins"]["rows"]]
    return comps, pins


def to_local(px, py, part):
    """Board mm -> part-local mm (unrotated, origin at part centre)."""
    a = math.radians(part["rot"])
    dx, dy = px - part["x"], py - part["y"]
    return (dx * math.cos(a) + dy * math.sin(a), -dx * math.sin(a) + dy * math.cos(a))


def main():
    bom = load_bom()
    comps, pins = load_probe()
    info = bom["comp_info"]
    parts = {}
    for side_key, side in (("top", "T"), ("bottom", "B")):
        for e in bom["designator_info"][0].get(side_key, []):
            ci = info.get(e["lc_code"], {})
            ec = e["ec"]
            parts[e["des"]] = {
                "des": e["des"], "side": side,
                "x": ec["x"], "y": ec["y"], "rot": ec["ang"],
                "bw": ec["dx"], "bh": ec["dy"],
                "footprint": e["ft_name"], "npins": e["ft_pins"],
                "mpn": e.get("cm") or ci.get("Manufacturer Part", ""),
                "value": ci.get("value") or ci.get("Name", ""),
                "lcsc": (e["lc_code"] or "").split(",")[0],
                "manufacturer": ci.get("Manufacturer", ""),
                "pkg": ci.get("Supplier Footprint") or ci.get("lc_pkg", ""),
                "pads": [],
            }

    named = {c["COMPONENT_NAME"] for c in comps if not c["COMPONENT_NAME"].startswith("PAD")}
    orphans = []
    for p in pins:
        owner, num = p["PIN_NAME"].rsplit("_", 1)
        pad = {
            "num": num, "x": p["PIN_X"] * MIL, "y": p["PIN_Y"] * MIL,
            "layer": p["LAYER"], "type": p["PIN_TYPE"], "net": p["NET_NAME"],
            "shape": p["PAD_SHAPE"], "w": p["PAD_SIZEX"] * MIL, "h": p["PAD_SIZEY"] * MIL,
            "drill": p["HOLE_SIZE"] * MIL, "angle": p["PAD_ANGLE"],
        }
        if owner in named and owner in parts:
            parts[owner]["pads"].append(pad)
        else:
            orphans.append(pad)

    # Assign anonymous pads to the part whose body box contains them.
    unassigned = []
    for pad in orphans:
        best, best_d = None, 1e9
        for part in parts.values():
            if part["side"] != pad["layer"] and pad["type"] == "SMD":
                continue
            lx, ly = to_local(pad["x"], pad["y"], part)
            ex = max(0.0, abs(lx) - part["bw"] / 2)
            ey = max(0.0, abs(ly) - part["bh"] / 2)
            d = math.hypot(ex, ey)
            if d < best_d:
                best, best_d = part, d
        if best is not None and best_d < 0.35:
            best["pads"].append(pad)
        else:
            unassigned.append((pad, best["des"] if best else None, round(best_d, 3)))

    # The probe file lists each pad more than once (named + anonymous, and
    # once per copper side for through-hole). Keep one per number/position.
    for part in parts.values():
        seen, uniq = set(), []
        for p in part["pads"]:
            k = (p["num"], round(p["x"], 3), round(p["y"], 3))
            if k in seen:
                continue
            seen.add(k)
            uniq.append(p)
        part["pads"] = uniq

    problems = []
    for part in parts.values():
        nums = [p["num"] for p in part["pads"]]
        if len(part["pads"]) != part["npins"]:
            problems.append((part["des"], part["footprint"], part["npins"], len(part["pads"]), sorted(nums)))
        elif len(set(nums)) != len(nums):
            problems.append((part["des"], part["footprint"], "dup pad numbers", sorted(nums)))

    # Design changes on top of v8 (ECOs): part -> {pad: new net}
    for d, pads in CHANGES.items():
        for pad in parts[d]["pads"]:
            if pad["num"] in pads:
                pad["net"] = pads[pad["num"]]

    for d, pads in PAD_CHANGES.items():
        for pad in parts[d]["pads"]:
            pad.update(pads.get(pad["num"], {}))

    model = {"parts": parts, "unassigned": [u[0] for u in unassigned], "changes": CHANGES,
             "pad_changes": PAD_CHANGES}
    OUT.write_text(json.dumps(model, indent=1))
    print(f"{len(parts)} parts, {len(pins)} pads, {len(unassigned)} unassigned, {len(problems)} pad-count problems")
    for u in unassigned[:20]:
        print("  unassigned:", u[0]["num"], u[0]["net"], round(u[0]["x"], 2), round(u[0]["y"], 2), "nearest", u[1], u[2])
    for pr in problems[:40]:
        print("  problem:", pr)


if __name__ == "__main__":
    sys.exit(main())
