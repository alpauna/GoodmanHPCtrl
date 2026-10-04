"""Download EasyEDA/LCSC 3D models (and their reference footprints) with
easyeda2kicad, one per LCSC part number used on the board.

  ~/.local/share/easyeda2kicad-venv/bin/python tools/fetch_models.py

Output: GoodmanHP.3dshapes/*.step|.wrl and out/models.json describing, per
LCSC number, the model file, its offset/rotation and the reference pads.
"""
import json
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
MODEL = json.loads((HERE / "model.json").read_text())["parts"]
VENV_BIN = Path.home() / ".local/share/easyeda2kicad-venv/bin/easyeda2kicad"
WORK = HERE / "out" / "easyeda2kicad"
SHAPES = HERE / "GoodmanHP.3dshapes"


def parse_footprint(text):
    pads = {}
    for m in re.finditer(r'\(pad "?([^"\s]+)"?\s+\S+\s+\S+\s+\(at ([-\d.]+) ([-\d.]+)', text):
        pads.setdefault(m.group(1), []).append((float(m.group(2)), float(m.group(3))))
    mm = re.search(r'\(model "?([^"\n)]+?)"?\s*\(offset \(xyz ([-\d.]+) ([-\d.]+) ([-\d.]+)\)\)\s*'
                   r'\(scale \(xyz [^)]*\)\)\s*\(rotate \(xyz ([-\d.]+) ([-\d.]+) ([-\d.]+)\)\)', text)
    model = None
    if mm:
        model = {"file": mm.group(1), "offset": [float(mm.group(i)) for i in (2, 3, 4)],
                 "rotate": [float(mm.group(i)) for i in (5, 6, 7)]}
    return pads, model


def main():
    WORK.mkdir(parents=True, exist_ok=True)
    SHAPES.mkdir(exist_ok=True)
    codes = sorted({p["lcsc"] for p in MODEL.values() if re.fullmatch(r"C\d+", p["lcsc"] or "")})
    print(f"{len(codes)} LCSC parts")
    out, missing = {}, []
    for c in codes:
        lib = WORK / c / "lib"
        lib.parent.mkdir(parents=True, exist_ok=True)
        fps = list((WORK / c).glob("lib.pretty/*.kicad_mod"))
        delay = 4
        while not fps and delay <= 64:          # EasyEDA rate-limits with HTTP 403
            r = subprocess.run([str(VENV_BIN), "--footprint", "--3d", "--lcsc_id", c, "--output", str(lib),
                                "--overwrite", "--use-cache"], cwd=WORK, capture_output=True, text=True)
            fps = list((WORK / c).glob("lib.pretty/*.kicad_mod"))
            if not fps:
                time.sleep(delay)
                delay *= 2
        time.sleep(1.5)
        if not fps:
            missing.append((c, "no footprint", "API refused after retries"))
            continue
        pads, model = parse_footprint(fps[0].read_text())
        if not model:
            missing.append((c, "footprint has no 3D model", ""))
            continue
        stem = Path(model["file"]).stem
        src_dir = WORK / c / "lib.3dshapes"
        got = []
        for ext in (".step",):          # KiCad renders STEP; skip the duplicate .wrl
            f = src_dir / f"{stem}{ext}"
            if f.exists():
                shutil.copy2(f, SHAPES / f.name)
                got.append(ext)
        if not got:
            missing.append((c, "model file not downloaded", ""))
            continue
        model["file"] = stem + (".step" if ".step" in got else ".wrl")
        out[c] = {"pads": pads, "model": model, "footprint": fps[0].stem}
        print(f"  {c}: {stem} {'+'.join(got)}", flush=True)
    (HERE / "out" / "models.json").write_text(json.dumps(out, indent=1))
    print(f"models: {len(out)}  missing: {len(missing)}")
    for m in missing:
        print("  missing:", m[0], m[1], m[2].strip().replace("\n", " ")[:150])


if __name__ == "__main__":
    sys.exit(main())
