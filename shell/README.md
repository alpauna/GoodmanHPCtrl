# 3D Shell Enclosure

3D-printable two-piece enclosure for the GoodmanHP controller board. Designed in EasyEDA (original STEP exports), modified with FreeCAD scripted operations.

## Files

| File | Purpose |
|------|---------|
| `cutout.py` | Reusable FreeCAD utility library for shell operations |
| `modify_top.py` | Top shell modifications (mating profile, wall thickening, cutouts, labels) |
| `modify_bottom.py` | Bottom shell modifications (mating profile, wall thickening, screw holes) |
| `3DShell_GoodmanHPv3_T_3mm_TC.step` | Original top shell (3mm walls, EasyEDA export) |
| `3DShell_GoodmanHPv3_B_3mm.step` | Original bottom shell (3mm walls, EasyEDA export) |
| `*_3.5mm_*_flipped.step/.stl` | Modified shells (3.5mm walls, flipped mating profile) |

## Building

Requires FreeCAD installed with command-line access:

```bash
freecadcmd shell/modify_top.py
freecadcmd shell/modify_bottom.py
```

## Shell Dimensions

- Outer: 103.2 x 129.6 mm (after 0.5mm wall thickening)
- Inner cavity: 97.2 x 123.6 mm
- Wall thickness: 3.5mm (original 3mm + 0.5mm thickening)
- Mating zone: Z=28-35mm, tongue-and-groove with 0.1mm clearance
- M1.5 screw holes at wall midpoints for assembly

## Top Shell Features

| ID | Description | Dimensions | Position (XY) | Label |
|----|-------------|-----------|---------------|-------|
| LV | Low Voltage (24v signals) | 84x35mm | X[-89,-5] Y[0,35] | "24v Signal Wires" (debossed above) |
| HV | High Voltage (240V power) | 10.5x20mm | X[-91,-80] Y[50,70] | "240 V" (debossed above) |
| TC | Thermocouple (MAX6675) | 16x14mm | X[-71,-56] Y[72,85] | "+"/"-" polarity, "Liquid Line" (debossed below) |
| LCD | LCD Display | 27x16mm | X[-39,-11] Y[62,78] | (none) |

## Bottom Shell Features

| ID | Description | Wall | Dimensions | Position |
|----|-------------|------|-----------|----------|
| ACCESS | General access/venting | Right (+X) | 41.5x17mm | Y[47,89] Z[7,24] |
| VENT | Air vent slot | Back (+Y) | 86x3mm | X[-90,-4] Z[14,17] |
| ANTENNA | ESP32 antenna clearance | Back (+Y) | 15x7mm | X[-50,-35] Z[6,13] |

## cutout.py Function Reference

### Geometry Primitives

| Function | Description |
|----------|-------------|
| `wall_ring(...)` | Rectangular ring (outer box minus inner box) for walls, grooves, tongues |

### Cutout Operations

| Function | Description |
|----------|-------------|
| `rectangular_cutout(shape, x1, y1, x2, y2, z_inner, z_outer)` | Cut rectangular through-hole in Z-normal surface |
| `reposition_cutout(shape, old_*, new_*, z_inner, z_outer)` | Plug existing cutout, re-cut at new position |
| `labeled_cutout(shape, x1, y1, x2, y2, z_inner, z_outer, label, ...)` | Rectangular cutout with optional debossed text label (above/below) |
| `wall_opening(shape, wall, a1, a2, z1, z2, wall_coord, ring_thick)` | Cut opening in a wall-parallel region (for thickening ring cutouts) |

### Mounting Features

| Function | Description |
|----------|-------------|
| `add_standoff(shape, cx, cy, z_surface, height)` | Fuse cylindrical pillar onto Z-normal surface |
| `drill_hole(shape, cx, cy, z_bottom, z_top, radius)` | Vertical cylindrical hole along Z axis |
| `drill_hole_lateral(shape, axis, start, end, cy, cz, radius)` | Horizontal through-hole along X or Y axis |
| `add_standoffs_with_holes(shape, positions, z_surface, height)` | Multiple standoffs with centered screw holes |
| `max6675_mount(shape, cutout_x1, cutout_y1, z_inner, z_outer, ...)` | MAX6675 board mount: terminal cutout + 6mm offset pillar + screw hole |

### Text Operations

| Function | Description |
|----------|-------------|
| `deboss_text(shape, text, x, y, z_surface)` | Cut text into Z-normal surface (recessed lettering) |
| `emboss_text(shape, text, x, y, z_surface)` | Raise text above Z-normal surface (protruding lettering) |

### Verification Helpers

| Function | Description |
|----------|-------------|
| `z_probe(shape, x, y)` | Probe Z intersections at an XY point |
| `verify_cutout(shape, x1, y1, x2, y2, z_inner, z_outer)` | Verify rectangular cutout is fully open |
| `verify_solid(shape, x, y, z_inner, z_outer)` | Verify material exists at XY between Z levels |
| `print_volume(shape, label)` | Print shape volume |
