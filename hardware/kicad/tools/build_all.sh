#!/usr/bin/env bash
# Rebuild the KiCad project from the EasyEDA v8 data, in the right order.
set -euo pipefail
cd "$(dirname "$0")/.."
K="flatpak run --command=python3 org.kicad.KiCad"
CLI="flatpak run --command=kicad-cli org.kicad.KiCad"
python3 tools/build_model.py
# (model.json comes from the EasyEDA v8 exports in docs/schematics)
python3 tools/gen_schematic.py
$K tools/gen_pcb.py
$K tools/merge_v8_copper.py
[ -f out/models.json ] && $K tools/attach_models.py   # 3D models (tools/fetch_models.py downloads them)
python3 tools/gen_project.py          # after every pcbnew save: it resets net classes
$CLI pcb drc --refill-zones --save-board --format report -o out/drc.rpt GoodmanHP.kicad_pcb
python3 tools/gen_project.py
$CLI pcb drc --format report -o out/drc.rpt GoodmanHP.kicad_pcb
