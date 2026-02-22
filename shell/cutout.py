"""
cutout.py - FreeCAD utility library for shell cutout and mounting operations.

Composable functions for modifying 3D printed enclosure shells. Each function
takes a Part.Shape and returns a modified Part.Shape, enabling pipelines:

    shape = reposition_cutout(shape, old_x1=..., new_y1=..., z_inner=48.5, z_outer=51.5)
    shape = add_standoff(shape, cx, cy, z_surface=48.5, height=3.0)
    shape = drill_hole(shape, cx, cy, z_bottom=45.5, z_top=51.5, radius=1.25)

All functions assume Z-normal surfaces (ceiling or floor). OCCT boolean
operation quirks are handled internally:
- Oversized plugs for reliable fuse (avoids coincident-face failures)
- Matching Z ranges between plug and cut (avoids residual thin layers)
- Extra cylinder height + trim for standoff fuse into existing geometry
- Through-holes extend past both ends for clean penetration

Usage: place this file alongside your shell modification scripts and import:
    import sys, os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from cutout import reposition_cutout, add_standoff, drill_hole
"""

import Part
from FreeCAD import Vector


# ---------------------------------------------------------------------------
# Geometry primitives
# ---------------------------------------------------------------------------

def wall_ring(x_outer_left, y_outer_front, x_outer_right, y_outer_back,
              x_inner_left, y_inner_front, x_inner_right, y_inner_back,
              z_bottom, z_top):
    """Create a rectangular ring (outer box minus inner box).

    Building block for wall thickening, mating profiles, grooves, and tongues.
    Returns a new solid — does not modify any existing shape.
    """
    outer = Part.makeBox(x_outer_right - x_outer_left,
                         y_outer_back - y_outer_front,
                         z_top - z_bottom,
                         Vector(x_outer_left, y_outer_front, z_bottom))
    inner = Part.makeBox(x_inner_right - x_inner_left,
                         y_inner_back - y_inner_front,
                         z_top - z_bottom,
                         Vector(x_inner_left, y_inner_front, z_bottom))
    return outer.cut(inner)


# ---------------------------------------------------------------------------
# Cutout operations
# ---------------------------------------------------------------------------

def rectangular_cutout(shape, x1, y1, x2, y2, z_inner, z_outer, z_margin=0.2):
    """Cut a rectangular through-hole in a Z-normal surface.

    The cut extends z_margin past both the inner and outer surfaces to
    ensure full penetration (avoids OCCT tolerance leaving a thin skin).

    Args:
        shape:    Shape to modify.
        x1, y1:   Bottom-left corner of cutout.
        x2, y2:   Top-right corner of cutout (x2 > x1, y2 > y1).
        z_inner:  Z of the inner (cavity-side) surface.
        z_outer:  Z of the outer surface.
        z_margin: Extra Z extension past both surfaces (default 0.2mm).
    """
    z_bot = min(z_inner, z_outer) - z_margin
    z_top = max(z_inner, z_outer) + z_margin
    cut_box = Part.makeBox(x2 - x1, y2 - y1, z_top - z_bot,
                           Vector(x1, y1, z_bot))
    return shape.cut(cut_box)


def reposition_cutout(shape, old_x1, old_y1, old_x2, old_y2,
                      new_x1, new_y1, new_x2, new_y2,
                      z_inner, z_outer, z_margin=0.2, plug_margin=1.0):
    """Plug an existing cutout and re-cut at a new position.

    Three-step operation:
    1. Fuse an oversized plug into the old cutout (plug_margin extra per XY side)
    2. Cut the new cutout at the new position (same Z range as plug)
    3. Trim plug flush with the outer surface (removes any protrusion)

    The plug and cut share the same Z range to avoid residual thin layers.
    The plug is oversized in XY because OCCT fuse fails when new geometry
    only touches existing faces at exact boundaries.

    Args:
        shape:       Shape to modify.
        old_x1..y2:  Bounds of the existing cutout to fill.
        new_x1..y2:  Bounds of the new cutout to create.
        z_inner:     Z of the inner (cavity-side) surface.
        z_outer:     Z of the outer surface.
        z_margin:    Extra Z extension past both surfaces (default 0.2mm).
        plug_margin: Extra XY margin on plug for reliable fuse (default 1.0mm).
    """
    z_bot = min(z_inner, z_outer) - z_margin
    z_top = max(z_inner, z_outer) + z_margin
    z_outer_actual = max(z_inner, z_outer)

    # 1. Oversized plug to fill old cutout
    plug = Part.makeBox(
        (old_x2 - old_x1) + 2 * plug_margin,
        (old_y2 - old_y1) + 2 * plug_margin,
        z_top - z_bot,
        Vector(old_x1 - plug_margin, old_y1 - plug_margin, z_bot))
    shape = shape.fuse(plug)

    # 2. New cutout at new position (same Z range — critical)
    new_cut = Part.makeBox(
        new_x2 - new_x1, new_y2 - new_y1, z_top - z_bot,
        Vector(new_x1, new_y1, z_bot))
    shape = shape.cut(new_cut)

    # 3. Trim flush with outer surface (remove protrusion from plug overshoot)
    # Cover union of old+new extents plus generous margin
    trim_x1 = min(old_x1, new_x1) - 2
    trim_y1 = min(old_y1, new_y1) - 2
    trim_x2 = max(old_x2, new_x2) + 2
    trim_y2 = max(old_y2, new_y2) + 2
    trim = Part.makeBox(trim_x2 - trim_x1, trim_y2 - trim_y1, 1.0,
                        Vector(trim_x1, trim_y1, z_outer_actual))
    shape = shape.cut(trim)

    return shape


# ---------------------------------------------------------------------------
# Mounting features
# ---------------------------------------------------------------------------

def add_standoff(shape, cx, cy, z_surface, height, radius=2.5,
                 z_outer=None, extra_height=2.0):
    """Fuse a cylindrical standoff pillar onto a Z-normal surface.

    The pillar extends from (z_surface - height) up to z_surface, with
    extra_height extending into the ceiling/floor for reliable OCCT fuse.
    Material above z_outer is trimmed away.

    Args:
        shape:        Shape to modify.
        cx, cy:       Center of the standoff.
        z_surface:    Z of the inner surface the standoff attaches to.
        height:       Standoff height extending into the cavity (mm).
        radius:       Outer radius of the cylinder (default 2.5mm).
        z_outer:      Z of the outer surface for trimming. Required.
        extra_height: Extra length past surface for reliable fuse (default 2.0mm).
    """
    if z_outer is None:
        raise ValueError("z_outer is required for standoff trimming")

    pil_bottom = z_surface - height
    pillar = Part.makeCylinder(radius, height + extra_height,
                               Vector(cx, cy, pil_bottom),
                               Vector(0, 0, 1))
    # Trim above outer surface
    trim = Part.makeBox(radius * 4, radius * 4, extra_height + 10,
                        Vector(cx - radius * 2, cy - radius * 2, z_outer))
    pillar = pillar.cut(trim)
    return shape.fuse(pillar)


def drill_hole(shape, cx, cy, z_bottom, z_top, radius, z_margin=0.0):
    """Drill a vertical cylindrical hole along the Z axis.

    Args:
        shape:    Shape to modify.
        cx, cy:   Center of the hole.
        z_bottom: Bottom Z of the hole.
        z_top:    Top Z of the hole.
        radius:   Hole radius (e.g., 1.25 for M3, 0.75 for M1.5).
        z_margin: Extra extension past both Z ends (default 0.0mm).
    """
    hole = Part.makeCylinder(radius, (z_top - z_bottom) + 2 * z_margin,
                             Vector(cx, cy, z_bottom - z_margin),
                             Vector(0, 0, 1))
    return shape.cut(hole)


def drill_hole_lateral(shape, axis, start, end, cy, cz, radius, margin=2.0):
    """Drill a horizontal through-hole along the X or Y axis.

    The hole extends margin mm past both ends for clean wall penetration.

    Args:
        shape:      Shape to modify.
        axis:       'x' or 'y' — the axis the hole runs along.
        start, end: Start and end coordinates along the axis.
        cy:         Center on the perpendicular horizontal axis.
        cz:         Center Z coordinate.
        radius:     Hole radius (e.g., 0.75 for M1.5).
        margin:     Extra length past both ends (default 2.0mm).
    """
    length = abs(end - start) + 2 * margin
    origin_coord = min(start, end) - margin

    if axis == 'x':
        origin = Vector(origin_coord, cy, cz)
        direction = Vector(1, 0, 0)
    elif axis == 'y':
        origin = Vector(cy, origin_coord, cz)
        direction = Vector(0, 1, 0)
    else:
        raise ValueError(f"axis must be 'x' or 'y', got '{axis}'")

    hole = Part.makeCylinder(radius, length, origin, direction)
    return shape.cut(hole)


def add_standoffs_with_holes(shape, positions, z_surface, standoff_height,
                             standoff_radius=2.5, hole_radius=1.25,
                             z_outer=None, extra_height=2.0):
    """Add multiple standoff pillars with centered through-holes.

    Convenience function composing add_standoff() + drill_hole() for each
    position. Common pattern for PCB mounting.

    Args:
        shape:            Shape to modify.
        positions:        List of (cx, cy) tuples for standoff centers.
        z_surface:        Z of the inner surface standoffs attach to.
        standoff_height:  Height of standoffs (mm).
        standoff_radius:  Outer radius (default 2.5mm).
        hole_radius:      Centered hole radius (default 1.25mm for M3).
        z_outer:          Z of the outer surface for trimming.
        extra_height:     Extra cylinder length for fuse (default 2.0mm).
    """
    pil_bottom = z_surface - standoff_height
    for cx, cy in positions:
        shape = add_standoff(shape, cx, cy, z_surface, standoff_height,
                             standoff_radius, z_outer, extra_height)
        shape = drill_hole(shape, cx, cy, pil_bottom, z_outer, hole_radius)
    return shape


# ---------------------------------------------------------------------------
# Verification helpers
# ---------------------------------------------------------------------------

def z_probe(shape, x, y, z_start=None, z_end=None):
    """Probe a shape along the Z axis at an XY point.

    Returns sorted Z coordinates where the probe intersects the shape
    boundary. Empty list = fully open (no material).

    Args:
        shape:   Shape to probe.
        x, y:    XY coordinates.
        z_start: Bottom of probe (default: shape.BoundBox.ZMin - 5).
        z_end:   Top of probe (default: shape.BoundBox.ZMax + 5).

    Returns:
        List of Z floats (sorted ascending).
    """
    bb = shape.BoundBox
    if z_start is None:
        z_start = bb.ZMin - 5
    if z_end is None:
        z_end = bb.ZMax + 5
    line = Part.makeLine(Vector(x, y, z_start), Vector(x, y, z_end))
    sec = shape.section(line)
    if sec.Vertexes:
        return sorted([v.Z for v in sec.Vertexes])
    return []


def verify_cutout(shape, x1, y1, x2, y2, z_inner, z_outer, label="cutout"):
    """Verify a rectangular cutout is fully open.

    Probes at the center and near each corner. Prints PASS/FAIL per point.

    Returns:
        True if all probe points are open, False otherwise.
    """
    cx = (x1 + x2) / 2
    cy = (y1 + y2) / 2
    inset = 0.5  # probe 0.5mm inside the edges
    probes = [
        ("center", cx, cy),
        ("BL", x1 + inset, y1 + inset),
        ("BR", x2 - inset, y1 + inset),
        ("TL", x1 + inset, y2 - inset),
        ("TR", x2 - inset, y2 - inset),
    ]
    all_open = True
    for name, px, py in probes:
        zs = z_probe(shape, px, py)
        # Filter to Z range of the surface
        z_lo = min(z_inner, z_outer) - 1
        z_hi = max(z_inner, z_outer) + 1
        hits_in_range = [z for z in zs if z_lo <= z <= z_hi]
        if len(hits_in_range) >= 2:
            print(f"  {label} {name} ({px:.1f},{py:.1f}): BLOCKED Z={[f'{z:.2f}' for z in hits_in_range]}")
            all_open = False
        else:
            print(f"  {label} {name} ({px:.1f},{py:.1f}): OPEN")
    return all_open


def verify_solid(shape, x, y, z_inner, z_outer, label="point"):
    """Verify material exists at an XY point between two Z levels.

    Returns:
        True if material boundaries found, False otherwise.
    """
    zs = z_probe(shape, x, y)
    z_lo = min(z_inner, z_outer)
    z_hi = max(z_inner, z_outer)
    hits = [z for z in zs if z_lo - 1 <= z <= z_hi + 1]
    if len(hits) >= 2:
        print(f"  {label} ({x:.1f},{y:.1f}): SOLID Z={[f'{z:.2f}' for z in hits]}")
        return True
    else:
        print(f"  {label} ({x:.1f},{y:.1f}): OPEN — expected solid")
        return False


def print_volume(shape, label=""):
    """Print shape volume with optional label. Returns the volume."""
    v = shape.Volume
    if label:
        print(f"{label}: volume={v:.0f}")
    else:
        print(f"Volume: {v:.0f}")
    return v


# ---------------------------------------------------------------------------
# Text operations (deboss / emboss)
# ---------------------------------------------------------------------------

# Default font — available on all Ubuntu/Debian systems
_DEFAULT_FONT_DIR = "/usr/share/fonts/truetype/dejavu/"
_DEFAULT_FONT_FILE = "DejaVuSans.ttf"


def _make_text_solid(text, height, font_dir=None, font_file=None, track=0):
    """Create a 3D solid from text string(s), 1mm thick, at the origin.

    Handles multi-wire glyphs (letters with holes like D, O, A, B, P, etc.)
    by using the outer wire as the face and inner wires as holes.

    Args:
        text:      String or list of strings (multi-line).
        height:    Font height in mm.
        font_dir:  Directory containing the font file.
        font_file: TrueType font filename.
        track:     Extra spacing between characters (default 0).

    Returns:
        (solid, width, total_height) — the fused solid, its X extent, and Y extent.
    """
    if font_dir is None:
        font_dir = _DEFAULT_FONT_DIR
    if font_file is None:
        font_file = _DEFAULT_FONT_FILE

    if isinstance(text, str):
        lines = [text]
    else:
        lines = list(text)

    line_spacing = height * 1.4
    all_chars = []
    max_width = 0.0
    total_height = height + line_spacing * (len(lines) - 1)

    for line_idx, line_text in enumerate(lines):
        y_offset = -line_idx * line_spacing
        wire_sets = Part.makeWireString(line_text, font_dir, font_file,
                                        height, track)
        if not wire_sets:
            continue

        for char_wires in wire_sets:
            if not char_wires:
                continue

            # Use FaceMakerBullseye for reliable hole detection in
            # multi-wire glyphs (0, 4, a, b, d, e, g, o, p, q, etc.)
            try:
                face = Part.makeFace(list(char_wires),
                                     "Part::FaceMakerBullseye")
            except Exception:
                # Fallback: outer wire only (loses counter holes)
                face = Part.Face(char_wires[0])

            char_solid = face.extrude(Vector(0, 0, 1.0))
            if y_offset != 0:
                char_solid.translate(Vector(0, y_offset, 0))
            all_chars.append(char_solid)

        # Track max width across lines
        line_bb_max_x = max(c.BoundBox.XMax for c in all_chars) if all_chars else 0
        if line_bb_max_x > max_width:
            max_width = line_bb_max_x

    if not all_chars:
        return None, 0, 0

    solid = all_chars[0]
    for c in all_chars[1:]:
        solid = solid.fuse(c)

    return solid, solid.BoundBox.XLength, total_height


def deboss_text(shape, text, x, y, z_surface, height=4.0, depth=0.4,
                font_dir=None, font_file=None, anchor="left"):
    """Cut text into a Z-normal surface (recessed lettering).

    Args:
        shape:     Shape to modify.
        text:      String or list of strings (multi-line, top line first).
        x, y:      Position of the text baseline.
        z_surface: Z of the outer surface to deboss into.
        height:    Font height in mm (default 4.0).
        depth:     Cut depth in mm (default 0.4).
        font_dir:  Font directory (default: system DejaVuSans).
        font_file: Font filename (default: DejaVuSans.ttf).
        anchor:    'left', 'center', or 'right' horizontal alignment.

    Returns:
        Modified shape with text cut into the surface.
    """
    solid, width, _ = _make_text_solid(text, height, font_dir, font_file)
    if solid is None:
        return shape

    # Scale Z to desired depth
    bb = solid.BoundBox
    import FreeCAD
    mat = FreeCAD.Matrix()
    mat.A33 = depth / bb.ZLength if bb.ZLength > 0 else 1.0
    solid = solid.transformGeometry(mat)

    # Position: anchor alignment
    if anchor == "center":
        dx = x - width / 2
    elif anchor == "right":
        dx = x - width
    else:
        dx = x

    solid.translate(Vector(dx, y, z_surface - depth))
    return shape.cut(solid)


def emboss_text(shape, text, x, y, z_surface, height=4.0, depth=0.4,
                font_dir=None, font_file=None, anchor="left"):
    """Raise text above a Z-normal surface (protruding lettering).

    Args:
        Same as deboss_text, but text protrudes outward from z_surface.

    Returns:
        Modified shape with text raised above the surface.
    """
    solid, width, _ = _make_text_solid(text, height, font_dir, font_file)
    if solid is None:
        return shape

    # Scale Z to desired depth
    bb = solid.BoundBox
    import FreeCAD
    mat = FreeCAD.Matrix()
    mat.A33 = depth / bb.ZLength if bb.ZLength > 0 else 1.0
    solid = solid.transformGeometry(mat)

    # Position: anchor alignment
    if anchor == "center":
        dx = x - width / 2
    elif anchor == "right":
        dx = x - width
    else:
        dx = x

    solid.translate(Vector(dx, y, z_surface))
    return shape.fuse(solid)


# ---------------------------------------------------------------------------
# Component mount templates
# ---------------------------------------------------------------------------

def labeled_cutout(shape, x1, y1, x2, y2, z_inner, z_outer,
                   label=None, label_pos="above", label_gap=2.0,
                   label_height=5.0, label_depth=0.8, cut=True):
    """Rectangular cutout with optional debossed text label.

    Combines rectangular_cutout() + deboss_text() for labeled openings
    like wire pass-throughs and connector slots.

    Args:
        shape:        Shape to modify.
        x1, y1:       Bottom-left corner of cutout.
        x2, y2:       Top-right corner of cutout (x2 > x1, y2 > y1).
        z_inner:      Z of the inner (cavity-side) surface.
        z_outer:      Z of the outer surface.
        label:        Text string for debossed label (None to skip).
        label_pos:    'above' or 'below' the cutout (default 'above').
        label_gap:    Gap between cutout edge and label baseline (default 2.0mm).
        label_height: Font height in mm (default 5.0).
        label_depth:  Deboss depth in mm (default 0.8).
        cut:          Whether to cut the rectangular hole (default True).
                      Set False if the cutout already exists in the shell.

    Returns:
        Modified shape with cutout (if cut=True) and label (if label set).
    """
    if cut:
        shape = rectangular_cutout(shape, x1, y1, x2, y2, z_inner, z_outer)

    if label:
        cx = (x1 + x2) / 2
        if label_pos == "above":
            ly = y2 + label_gap
        else:
            ly = y1 - label_gap - label_height
        z_surface = max(z_inner, z_outer)
        shape = deboss_text(shape, label, x=cx, y=ly,
                            z_surface=z_surface, height=label_height,
                            depth=label_depth, anchor="center")
    return shape


def wall_opening(shape, wall, a1, a2, z1, z2,
                 wall_coord, ring_thick, margin=2.0, inset=0.5):
    """Cut an opening in a wall-parallel region.

    For preserving existing wall cutouts when adding a thickening ring.
    Produces a box oriented perpendicular to the specified wall, spanning
    from inset mm inside the wall to margin mm past the ring.

    Args:
        shape:       Shape to modify (typically the thickening ring).
        wall:        Wall side: 'left' (-X), 'right' (+X), 'front' (-Y), 'back' (+Y).
        a1, a2:      Along-wall coordinate range (Y for left/right, X for front/back).
        z1, z2:      Z range of the opening.
        wall_coord:  Original outer wall coordinate (e.g., OX_R for right wall).
        ring_thick:  Thickening ring thickness (e.g., 0.5mm).
        margin:      Extra depth past the ring for reliable cut (default 2.0mm).
        inset:       How far inside the wall to start the cut (default 0.5mm).

    Returns:
        Modified shape with the wall opening cut.
    """
    through = ring_thick + margin
    if wall == 'right':
        box = Part.makeBox(through + inset, a2 - a1, z2 - z1,
                           Vector(wall_coord - inset, a1, z1))
    elif wall == 'left':
        box = Part.makeBox(through + inset, a2 - a1, z2 - z1,
                           Vector(wall_coord - through, a1, z1))
    elif wall == 'back':
        box = Part.makeBox(a2 - a1, through + inset, z2 - z1,
                           Vector(a1, wall_coord - inset, z1))
    elif wall == 'front':
        box = Part.makeBox(a2 - a1, through + inset, z2 - z1,
                           Vector(a1, wall_coord - through, z1))
    else:
        raise ValueError(f"wall must be 'left', 'right', 'front', or 'back', got '{wall}'")
    return shape.cut(box)


def max6675_mount(shape, cutout_x1, cutout_y1, z_inner, z_outer,
                  cutout_w=16.0, cutout_h=14.0,
                  pillar_x=None, pillar_y=None,
                  pillar_radius=2.5, pillar_height=6.0,
                  hole_radius=1.25):
    """Add MAX6675 thermocouple board mount: terminal cutout + pillar + screw hole.

    Creates a rectangular cutout for the terminal block, a support pillar,
    and a screw hole through the pillar and ceiling. The pillar is
    positioned below the cutout by default for board offset/clearance.

    Args:
        shape:          Shape to modify.
        cutout_x1:      Left X of terminal cutout.
        cutout_y1:      Bottom Y of terminal cutout.
        z_inner:        Z of inner (cavity-side) ceiling surface.
        z_outer:        Z of outer ceiling surface.
        cutout_w:       Cutout width in X (default 16.0mm).
        cutout_h:       Cutout height in Y (default 14.0mm).
        pillar_x:       Absolute X of pillar center (default: centered in cutout).
        pillar_y:       Absolute Y of pillar center (default: 4.5mm below cutout).
        pillar_radius:  Pillar outer radius (default 2.5mm).
        pillar_height:  Pillar height below inner surface (default 6.0mm).
        hole_radius:    Screw hole radius (default 1.25mm for M3).

    Returns:
        Modified shape with cutout, pillar, and hole.
    """
    cutout_x2 = cutout_x1 + cutout_w
    cutout_y2 = cutout_y1 + cutout_h

    # 1. Terminal block cutout
    shape = rectangular_cutout(shape, cutout_x1, cutout_y1,
                               cutout_x2, cutout_y2, z_inner, z_outer)

    # 2. Support pillar — default: centered in X, 4.5mm below cutout in Y
    px = pillar_x if pillar_x is not None else (cutout_x1 + cutout_x2) / 2
    py = pillar_y if pillar_y is not None else cutout_y1 - 4.5
    shape = add_standoff(shape, px, py, z_surface=z_inner,
                         height=pillar_height, radius=pillar_radius,
                         z_outer=z_outer)

    # 3. Screw hole through pillar + ceiling
    shape = drill_hole(shape, px, py,
                       z_bottom=z_inner - pillar_height,
                       z_top=z_outer, radius=hole_radius)

    return shape
