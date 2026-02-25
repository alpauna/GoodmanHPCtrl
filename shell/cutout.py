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


def countersink_cup(shape, cx, cy, z_bottom, z_surface, z_outer,
                    outer_radius, wall_thick=1.5, clearance_radius=1.1):
    """Add a hollow countersink cup (boss) hanging from a ceiling surface.

    Creates a cylindrical cup fused into the ceiling with uniform wall
    thickness and a floor at the bottom. A clearance hole through the
    floor allows a screw to pass through without touching.

    The cup is built as a single pre-assembled solid (outer shell minus
    bore minus clearance hole) and fused in one operation for reliable
    OCCT boolean behavior.

    Cross-section:
        |<-- wall_thick -->|<-- bore -->|<-- wall_thick -->|
        +------------------+            +------------------+ z_outer
        |    ceiling       |  (bore)    |    ceiling       |
        +------------------+            +------------------+ z_surface
        |  wall  |         |  (bore)    |         |  wall  |
        |  wall  |         |  (bore)    |         |  wall  |
        |  wall  +---------+---+   +----+---------+  wall  |
        |  wall  |  floor  | clearance  |  floor  |  wall  |
        +--------+---------+---+   +----+---------+--------+ z_bottom

    Args:
        shape:            Shape to modify.
        cx, cy:           Center of the cup.
        z_bottom:         Z of the cup base (lowest point of pillar).
        z_surface:        Z of the inner ceiling surface.
        z_outer:          Z of the outer ceiling surface.
        outer_radius:     Outer radius of the cup cylinder.
        wall_thick:       Uniform wall and floor thickness (default 1.5mm).
        clearance_radius: Screw clearance hole radius (default 1.1mm for M2).

    Returns:
        Modified shape with countersink cup fused in.
    """
    bore_radius = outer_radius - wall_thick
    bore_z_bottom = z_bottom + wall_thick  # floor top

    overshoot = 1.0  # extend past z_outer to avoid coincident-face fuse failures

    # 1. Outer cylinder: z_bottom past z_outer (full ceiling penetration + overshoot)
    cup = Part.makeCylinder(outer_radius, z_outer + overshoot - z_bottom,
                            Vector(cx, cy, z_bottom))
    # 2. Bore out interior: bore_z_bottom to past z_outer
    bore = Part.makeCylinder(bore_radius, z_outer + overshoot - bore_z_bottom,
                             Vector(cx, cy, bore_z_bottom))
    cup = cup.cut(bore)
    # 3. Clearance hole through floor
    clearance = Part.makeCylinder(clearance_radius,
                                  wall_thick + 2.0,
                                  Vector(cx, cy, z_bottom - 1.0))
    cup = cup.cut(clearance)
    # 4. Fuse WITH overshoot — avoids coincident faces at z_outer
    shape = shape.fuse(cup)
    # 5. Trim overshoot flush with outer ceiling surface
    trim = Part.makeBox(outer_radius * 4, outer_radius * 4, overshoot + 10.0,
                        Vector(cx - outer_radius * 2,
                               cy - outer_radius * 2, z_outer))
    return shape.cut(trim)


def coved_corner_countersink(shape, cx, cy, z_bottom, z_outer,
                             radius, recess_depth, clearance_radius=1.2,
                             wall_x=None, wall_y=None):
    """Add a coved corner countersink — a solid boss with matching recess.

    Creates a cylindrical screw boss in a wall corner, clipped flush at two
    wall surfaces. A matching quarter-round recess is cut from the ceiling
    surface, with a rectangular corner fill to eliminate artifacts where the
    cylinder curve doesn't reach the wall intersection. A clearance hole is
    drilled through the entire column for the screw.

    The result is an inverse quarter-round profile visible from both sides:
    curved face toward the shell interior, flat faces flush with the walls,
    and a countersunk pocket on the outer ceiling surface.

    Cross-section (top view, +X right wall, -Y front wall):

        interior
            \\
             \\  curved face
              \\
        -------+  screw hole at (cx, cy)
        |      |
        | wall | wall
        +------+  corner (wall_x, wall_y)

    Args:
        shape:            Shape to modify.
        cx, cy:           Screw center position (matches bottom pillar).
        z_bottom:         Z of the boss base (lowest point).
        z_outer:          Z of the outer ceiling surface.
        radius:           Boss cylinder radius (mm).
        recess_depth:     Depth of countersink recess from ceiling (mm).
        clearance_radius: Screw clearance hole radius (default 1.2mm for M2).
        wall_x:           X coordinate of the wall to clip at (e.g., right wall
                          outer face). Boss extends in -X from here. Required.
        wall_y:           Y coordinate of the wall to clip at (e.g., front wall
                          outer face). Boss extends in +Y from here. Required.

    Returns:
        Modified shape with coved corner countersink.
    """
    overshoot = 1.0

    # 1. Solid cylinder at screw position
    cup = Part.makeCylinder(radius, z_outer + overshoot - z_bottom,
                            Vector(cx, cy, z_bottom))

    # 2. Clip at wall outer surfaces — let cylinder extend into interior
    clip_box = Part.makeBox(
        wall_x - (cx - radius),
        (cy + radius) - wall_y,
        z_outer + overshoot - z_bottom + 2,
        Vector(cx - radius, wall_y, z_bottom - 1))
    cup = cup.common(clip_box)

    # 3. Fuse solid boss with shell
    shape = shape.fuse(cup)

    # 4. Trim overshoot above ceiling
    trim = Part.makeBox(radius * 4, radius * 4, overshoot + 10.0,
                        Vector(cx - radius * 2, cy - radius * 2, z_outer))
    shape = shape.cut(trim)

    # 5. Drill clearance hole through entire column + ceiling
    hole = Part.makeCylinder(clearance_radius, 30.0,
                             Vector(cx, cy, z_bottom - 5))
    shape = shape.cut(hole)

    # 6. Quarter-round countersink recess from ceiling surface
    csink = Part.makeCylinder(radius, recess_depth + 1.0,
                              Vector(cx, cy, z_outer - recess_depth))
    csink_clip = Part.makeBox(
        wall_x - (cx - radius),
        (cy + radius) - wall_y,
        recess_depth + 2,
        Vector(cx - radius, wall_y, z_outer - recess_depth - 0.5))
    csink = csink.common(csink_clip)

    # 7. Fill corner gap between cylinder curve and wall intersection
    corner_fill = Part.makeBox(
        wall_x - cx,
        cy - wall_y,
        recess_depth + 1.0,
        Vector(cx, wall_y, z_outer - recess_depth))
    csink = csink.fuse(corner_fill)

    return shape.cut(csink)


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


def wall_cutout(shape, wall, wall_coord, a1, a2, z1, z2,
                wall_thick=3.5, brim_sides=None, brim_inside=True,
                brim_height=2.0, brim_depth=3.0, cut_margin=2.0):
    """Cut a rectangular opening through a wall with optional reinforcement brim.

    Creates a through-hole in the specified wall. Optionally fuses a brim
    (reinforcement lip) around selected edges of the cutout opening.

    Args:
        shape:       Shape to modify.
        wall:        'left' (-X), 'right' (+X), 'front' (-Y), 'back' (+Y).
        wall_coord:  Outermost wall coordinate (e.g., NY_F for thickened front wall).
        a1, a2:      Along-wall range (Y for left/right, X for front/back).
        z1, z2:      Z range of the opening.
        wall_thick:  Total wall thickness from outer to inner face (default 3.5mm).
        brim_sides:  Set of sides to add brim: subset of {'top','bottom','left','right'}.
                     None = no brim (cutout only).
        brim_inside: True = brim on shell interior, False = on exterior.
        brim_height: Brim extends this far from cutout edge along wall (default 2.0mm).
        brim_depth:  Brim protrudes this far from wall surface (default 3.0mm).
        cut_margin:  Extra cut depth past wall for clean boolean (default 2.0mm).

    Returns:
        Modified shape with cutout and optional brim.
    """
    # 1. Cut the opening through the wall
    if wall in ('left', 'right'):
        if wall == 'left':
            cut_x = wall_coord - cut_margin
        else:
            cut_x = wall_coord - wall_thick - cut_margin
        cut_box = Part.makeBox(wall_thick + 2 * cut_margin, a2 - a1, z2 - z1,
                               Vector(cut_x, a1, z1))
    elif wall in ('front', 'back'):
        if wall == 'front':
            cut_y = wall_coord - cut_margin
        else:
            cut_y = wall_coord - wall_thick - cut_margin
        cut_box = Part.makeBox(a2 - a1, wall_thick + 2 * cut_margin, z2 - z1,
                               Vector(a1, cut_y, z1))
    else:
        raise ValueError(f"wall must be 'left','right','front','back', got '{wall}'")

    shape = shape.cut(cut_box)

    # 2. Add brim if requested
    if not brim_sides:
        return shape

    # Compute brim attachment surface (perpendicular to wall)
    # The brim sits on the inner or outer face and protrudes away from the wall.
    if wall in ('left', 'right'):
        if wall == 'left':
            brim_px = (wall_coord + wall_thick) if brim_inside else (wall_coord - brim_depth)
        else:
            brim_px = (wall_coord - wall_thick - brim_depth) if brim_inside else wall_coord

        # Corner-aware Z range for vertical strips
        z_lo = (z1 - brim_height) if 'bottom' in brim_sides else z1
        z_hi = (z2 + brim_height) if 'top' in brim_sides else z2

        brims = []
        if 'top' in brim_sides:
            brims.append(Part.makeBox(brim_depth, a2 - a1, brim_height,
                                      Vector(brim_px, a1, z2)))
        if 'bottom' in brim_sides:
            brims.append(Part.makeBox(brim_depth, a2 - a1, brim_height,
                                      Vector(brim_px, a1, z1 - brim_height)))
        if 'left' in brim_sides:
            brims.append(Part.makeBox(brim_depth, brim_height, z_hi - z_lo,
                                      Vector(brim_px, a1 - brim_height, z_lo)))
        if 'right' in brim_sides:
            brims.append(Part.makeBox(brim_depth, brim_height, z_hi - z_lo,
                                      Vector(brim_px, a2, z_lo)))
    else:
        if wall == 'front':
            brim_py = (wall_coord + wall_thick) if brim_inside else (wall_coord - brim_depth)
        else:
            brim_py = (wall_coord - wall_thick - brim_depth) if brim_inside else wall_coord

        z_lo = (z1 - brim_height) if 'bottom' in brim_sides else z1
        z_hi = (z2 + brim_height) if 'top' in brim_sides else z2

        brims = []
        if 'top' in brim_sides:
            brims.append(Part.makeBox(a2 - a1, brim_depth, brim_height,
                                      Vector(a1, brim_py, z2)))
        if 'bottom' in brim_sides:
            brims.append(Part.makeBox(a2 - a1, brim_depth, brim_height,
                                      Vector(a1, brim_py, z1 - brim_height)))
        if 'left' in brim_sides:
            brims.append(Part.makeBox(brim_height, brim_depth, z_hi - z_lo,
                                      Vector(a1 - brim_height, brim_py, z_lo)))
        if 'right' in brim_sides:
            brims.append(Part.makeBox(brim_height, brim_depth, z_hi - z_lo,
                                      Vector(a2, brim_py, z_lo)))

    if brims:
        brim = brims[0]
        for b in brims[1:]:
            brim = brim.fuse(b)
        shape = shape.fuse(brim)

    return shape


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


# ---------------------------------------------------------------------------
# Hinge and closure features
# ---------------------------------------------------------------------------

def knuckle_hinge(shape, wall, wall_coord, a_start, a_end,
                  z_center, knuckle_count=5, knuckle_radius=3.0,
                  pin_radius=1.0, clearance=0.3, side="bottom",
                  strut_z_min=None, strut_z_max=None):
    """Add knuckle/barrel hinge cylinders with full-height wall struts.

    Creates alternating interlocking knuckle barrels for a pin hinge.
    Bottom shell gets even-indexed knuckles (0, 2, 4, ...),
    top shell gets odd-indexed knuckles (1, 3, ...).

    Each knuckle has:
    - A full-width rectangular buildup (2x knuckle_radius from wall)
      spanning the barrel diameter in Z, creating a solid D-shaped
      cross-section that encases the barrel on the wall side.
    - A vertical strut running from the barrel to the shell edge
      (floor for bottom, ceiling for top), transferring hinge stress
      through the entire wall height. Strut width = knuckle_radius.

    Args:
        shape:          Shape to modify.
        wall:           Wall side: 'left', 'right', 'front', 'back'.
        wall_coord:     Outer wall coordinate (e.g., NX_L for left wall).
        a_start, a_end: Range along the wall (Y for left/right, X for front/back).
        z_center:       Z center of the hinge barrel axis.
        knuckle_count:  Total number of knuckle segments (default 5).
        knuckle_radius: Outer radius of each knuckle barrel (default 3.0mm).
        pin_radius:     Pin hole radius (default 1.0mm for ~2mm pin/filament).
        clearance:      Gap between adjacent knuckles (default 0.3mm).
        side:           'bottom' or 'top' — determines which knuckles to place.
        strut_z_min:    Z bottom of vertical strut (e.g., floor Z for bottom shell).
                        Strut runs from here up to the barrel. None = no downward strut.
        strut_z_max:    Z top of vertical strut (e.g., ceiling Z for top shell).
                        Strut runs from barrel up to here. None = no upward strut.

    Returns:
        Modified shape with knuckle barrels, buildup, struts, and pin hole.
    """
    total_length = abs(a_end - a_start)
    seg_length = (total_length - clearance * (knuckle_count - 1)) / knuckle_count
    a_min = min(a_start, a_end)

    # Determine axis direction and center position on the wall exterior
    if wall in ('left', 'right'):
        barrel_dir = Vector(0, 1, 0)  # barrels run along Y
        if wall == 'left':
            cx = wall_coord - knuckle_radius  # barrel center (-X)
        else:
            cx = wall_coord + knuckle_radius  # barrel center (+X)
        cz = z_center
    else:
        barrel_dir = Vector(1, 0, 0)  # barrels run along X
        if wall == 'front':
            cy_pos = wall_coord - knuckle_radius
        else:
            cy_pos = wall_coord + knuckle_radius
        cz = z_center

    barrel_z_bottom = cz - knuckle_radius
    barrel_z_top = cz + knuckle_radius

    # Chamfer size for outer buildup corners (slightly > R(sqrt2-1) for clearance)
    chamfer = knuckle_radius * 0.45

    # Select which knuckle indices this side gets
    if side == "bottom":
        indices = range(0, knuckle_count, 2)  # even: 0, 2, 4
    else:
        indices = range(1, knuckle_count, 2)  # odd: 1, 3

    for i in indices:
        seg_start = a_min + i * (seg_length + clearance)

        if wall in ('left', 'right'):
            # 1. Barrel cylinder
            origin = Vector(cx, seg_start, cz)
            knuckle = Part.makeCylinder(knuckle_radius, seg_length,
                                        origin, barrel_dir)
            shape = shape.fuse(knuckle)

            # 2. Full-width buildup: 2x radius from wall, full barrel diameter in Z
            buildup_width = knuckle_radius * 2  # full barrel diameter
            if wall == 'left':
                block = Part.makeBox(
                    buildup_width, seg_length, knuckle_radius * 2,
                    Vector(wall_coord - buildup_width, seg_start,
                           barrel_z_bottom))
            else:
                block = Part.makeBox(
                    buildup_width, seg_length, knuckle_radius * 2,
                    Vector(wall_coord, seg_start, barrel_z_bottom))
            shape = shape.fuse(block)

            # 2b. Chamfer outer corners of buildup
            if wall == 'left':
                outer_x = wall_coord - buildup_width
                dx = chamfer
            else:
                outer_x = wall_coord + buildup_width
                dx = -chamfer
            # Bottom-outer corner
            w = Part.makePolygon([
                Vector(outer_x, seg_start, barrel_z_bottom),
                Vector(outer_x + dx, seg_start, barrel_z_bottom),
                Vector(outer_x, seg_start, barrel_z_bottom + chamfer),
                Vector(outer_x, seg_start, barrel_z_bottom)])
            prism = Part.Face(w).extrude(Vector(0, seg_length, 0))
            shape = shape.cut(prism)
            # Top-outer corner
            w = Part.makePolygon([
                Vector(outer_x, seg_start, barrel_z_top),
                Vector(outer_x + dx, seg_start, barrel_z_top),
                Vector(outer_x, seg_start, barrel_z_top - chamfer),
                Vector(outer_x, seg_start, barrel_z_top)])
            prism = Part.Face(w).extrude(Vector(0, seg_length, 0))
            shape = shape.cut(prism)

            # 3. Vertical strut down to shell floor
            strut_width = knuckle_radius  # half the buildup width
            if strut_z_min is not None and strut_z_min < barrel_z_bottom:
                strut_height = barrel_z_bottom - strut_z_min
                if wall == 'left':
                    strut = Part.makeBox(
                        strut_width, seg_length, strut_height,
                        Vector(wall_coord - strut_width, seg_start,
                               strut_z_min))
                else:
                    strut = Part.makeBox(
                        strut_width, seg_length, strut_height,
                        Vector(wall_coord, seg_start, strut_z_min))
                shape = shape.fuse(strut)

            # 4. Vertical strut up to shell ceiling
            if strut_z_max is not None and strut_z_max > barrel_z_top:
                strut_height = strut_z_max - barrel_z_top
                if wall == 'left':
                    strut = Part.makeBox(
                        strut_width, seg_length, strut_height,
                        Vector(wall_coord - strut_width, seg_start,
                               barrel_z_top))
                else:
                    strut = Part.makeBox(
                        strut_width, seg_length, strut_height,
                        Vector(wall_coord, seg_start, barrel_z_top))
                shape = shape.fuse(strut)

        else:
            # Front/back walls — same pattern, axes swapped
            origin = Vector(seg_start, cy_pos, cz)
            knuckle = Part.makeCylinder(knuckle_radius, seg_length,
                                        origin, barrel_dir)
            shape = shape.fuse(knuckle)

            buildup_width = knuckle_radius * 2
            if wall == 'front':
                block = Part.makeBox(
                    seg_length, buildup_width, knuckle_radius * 2,
                    Vector(seg_start, wall_coord - buildup_width,
                           barrel_z_bottom))
            else:
                block = Part.makeBox(
                    seg_length, buildup_width, knuckle_radius * 2,
                    Vector(seg_start, wall_coord, barrel_z_bottom))
            shape = shape.fuse(block)

            # 2b. Chamfer outer corners of buildup
            if wall == 'front':
                outer_y = wall_coord - buildup_width
                dy = chamfer
            else:
                outer_y = wall_coord + buildup_width
                dy = -chamfer
            # Bottom-outer corner
            w = Part.makePolygon([
                Vector(seg_start, outer_y, barrel_z_bottom),
                Vector(seg_start, outer_y + dy, barrel_z_bottom),
                Vector(seg_start, outer_y, barrel_z_bottom + chamfer),
                Vector(seg_start, outer_y, barrel_z_bottom)])
            prism = Part.Face(w).extrude(Vector(seg_length, 0, 0))
            shape = shape.cut(prism)
            # Top-outer corner
            w = Part.makePolygon([
                Vector(seg_start, outer_y, barrel_z_top),
                Vector(seg_start, outer_y + dy, barrel_z_top),
                Vector(seg_start, outer_y, barrel_z_top - chamfer),
                Vector(seg_start, outer_y, barrel_z_top)])
            prism = Part.Face(w).extrude(Vector(seg_length, 0, 0))
            shape = shape.cut(prism)

            strut_width = knuckle_radius
            if strut_z_min is not None and strut_z_min < barrel_z_bottom:
                strut_height = barrel_z_bottom - strut_z_min
                if wall == 'front':
                    strut = Part.makeBox(
                        seg_length, strut_width, strut_height,
                        Vector(seg_start, wall_coord - strut_width,
                               strut_z_min))
                else:
                    strut = Part.makeBox(
                        seg_length, strut_width, strut_height,
                        Vector(seg_start, wall_coord, strut_z_min))
                shape = shape.fuse(strut)

            if strut_z_max is not None and strut_z_max > barrel_z_top:
                strut_height = strut_z_max - barrel_z_top
                if wall == 'front':
                    strut = Part.makeBox(
                        seg_length, strut_width, strut_height,
                        Vector(seg_start, wall_coord - strut_width,
                               barrel_z_top))
                else:
                    strut = Part.makeBox(
                        seg_length, strut_width, strut_height,
                        Vector(seg_start, wall_coord, barrel_z_top))
                shape = shape.fuse(strut)

    # Drill pin hole through entire hinge length
    margin = 2.0
    if wall in ('left', 'right'):
        pin_origin = Vector(cx, a_min - margin, cz)
        pin = Part.makeCylinder(pin_radius, total_length + 2 * margin,
                                pin_origin, Vector(0, 1, 0))
    else:
        pin_origin = Vector(a_min - margin, cy_pos, cz)
        pin = Part.makeCylinder(pin_radius, total_length + 2 * margin,
                                pin_origin, Vector(1, 0, 0))

    shape = shape.cut(pin)
    return shape


def snap_clip(shape, wall, wall_coord, a_center, z_center,
              clip_width=8.0, clip_height=4.0, clip_depth=2.0,
              lip_height=1.0, lip_depth=0.8, wall_thick=3.5,
              side="bottom"):
    """Add a snap-fit clip feature on a wall.

    Bottom shell gets the flexible cantilever arm with a lip/hook.
    Top shell gets a matching rectangular pocket/recess for the lip.

    The clip is oriented perpendicular to the wall, protruding inward
    from the wall interior.

    Args:
        shape:       Shape to modify.
        wall:        Wall side: 'left', 'right', 'front', 'back'.
        wall_coord:  Outer wall coordinate.
        a_center:    Center position along the wall (Y for left/right).
        z_center:    Z center of the clip.
        clip_width:  Width of clip along the wall (default 8.0mm).
        clip_height: Height of the cantilever arm (default 4.0mm).
        clip_depth:  How far the arm extends inward from the wall (default 2.0mm).
        lip_height:  Height of the hook lip (default 1.0mm).
        lip_depth:   Depth of the hook lip (default 0.8mm).
        wall_thick:  Wall thickness for positioning (default 3.5mm).
        side:        'bottom' (cantilever arm) or 'top' (pocket).

    Returns:
        Modified shape.
    """
    half_w = clip_width / 2
    half_h = clip_height / 2

    if wall == 'right':
        inner_x = wall_coord - wall_thick
        if side == "bottom":
            # Cantilever arm: box extending inward from inner wall
            arm = Part.makeBox(
                clip_depth, clip_width, clip_height,
                Vector(inner_x - clip_depth, a_center - half_w,
                       z_center - half_h))
            shape = shape.fuse(arm)
            # Lip/hook at the tip pointing upward
            lip = Part.makeBox(
                lip_depth, clip_width, lip_height,
                Vector(inner_x - clip_depth - lip_depth, a_center - half_w,
                       z_center + half_h))
            shape = shape.fuse(lip)
        else:
            # Pocket/recess in the top shell inner wall for the lip
            pocket_depth = clip_depth + lip_depth + 0.4  # clearance
            pocket = Part.makeBox(
                pocket_depth, clip_width + 0.4, clip_height + lip_height + 0.4,
                Vector(inner_x - pocket_depth,
                       a_center - half_w - 0.2,
                       z_center - half_h - 0.2))
            shape = shape.cut(pocket)
    elif wall == 'left':
        inner_x = wall_coord + wall_thick
        if side == "bottom":
            arm = Part.makeBox(
                clip_depth, clip_width, clip_height,
                Vector(inner_x, a_center - half_w,
                       z_center - half_h))
            shape = shape.fuse(arm)
            lip = Part.makeBox(
                lip_depth, clip_width, lip_height,
                Vector(inner_x + clip_depth, a_center - half_w,
                       z_center + half_h))
            shape = shape.fuse(lip)
        else:
            pocket_depth = clip_depth + lip_depth + 0.4
            pocket = Part.makeBox(
                pocket_depth, clip_width + 0.4, clip_height + lip_height + 0.4,
                Vector(inner_x,
                       a_center - half_w - 0.2,
                       z_center - half_h - 0.2))
            shape = shape.cut(pocket)

    return shape
