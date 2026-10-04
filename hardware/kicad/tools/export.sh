#!/usr/bin/env bash
# Manufacturing and documentation outputs into out/.
set -euo pipefail
cd "$(dirname "$0")/.."
CLI="flatpak run --command=kicad-cli org.kicad.KiCad"
rm -rf out/gerbers && mkdir -p out/gerbers
$CLI pcb export gerbers --layers "F.Cu,In1.Cu,In2.Cu,In3.Cu,In4.Cu,B.Cu,F.Paste,B.Paste,F.Silkscreen,B.Silkscreen,F.Mask,B.Mask,Edge.Cuts" -o out/gerbers/ GoodmanHP.kicad_pcb
$CLI pcb export drill --format excellon --excellon-separate-th -o out/gerbers/ GoodmanHP.kicad_pcb
$CLI pcb export pos --format csv --units mm -o out/GoodmanHP-positions.csv GoodmanHP.kicad_pcb
$CLI sch export bom --fields 'Reference,Value,Footprint,LCSC,Manufacturer,${QUANTITY}' --group-by "Value,Footprint,LCSC" -o out/GoodmanHP-bom.csv GoodmanHP.kicad_sch
$CLI sch export pdf -o out/GoodmanHP-schematic.pdf GoodmanHP.kicad_sch
$CLI pcb render --side top --rotate "-40,0,-20" --perspective --quality high --zoom 0.95 -w 1800 -h 1200 -o out/GoodmanHP-3d.png GoodmanHP.kicad_pcb
flatpak run --command=python3 org.kicad.KiCad ~/.local/share/InteractiveHtmlBom/InteractiveHtmlBom/generate_interactive_bom.py \
  --no-browser --dest-dir out --name-format GoodmanHP_ibom --extra-fields LCSC,Manufacturer \
  --include-tracks --include-nets --dark-mode GoodmanHP.kicad_pcb
