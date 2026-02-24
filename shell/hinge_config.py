"""Shared hinge and closure constants for hinged shell variants.

Both modify_bottom_hinged.py and modify_top_hinged.py import from here
to guarantee the pin axis, knuckle positions, and snap clip positions
are identical between the two halves.
"""

# Shell wall coordinates (must match both scripts)
OX_L = -98.262; OX_R = 4.917
OY_F = -32.48;  OY_B = 97.12
THICK = 0.5
NX_L = OX_L - THICK   # -98.762 — thickened left wall exterior
NX_R = OX_R + THICK   #   5.417 — thickened right wall exterior

# --- Hinge on left wall ---
# Pin axis: X = NX_L - KNUCKLE_RADIUS, Z = HINGE_Z
# Both halves MUST produce the same (X, Z) for the pin to thread through
KNUCKLE_RADIUS = 3.0                   # barrel outer radius (mm)
_LEFT_WALL_Z = 35.0                    # bottom shell left wall top (BOT_RIM_Z, tongue kept)
_BARREL_GAP = 0.2                      # clearance between barrel bottom and wall
HINGE_Z = _LEFT_WALL_Z + _BARREL_GAP + KNUCKLE_RADIUS  # 38.2
HINGE_INSET = 5.0                     # inset from wall ends to avoid corners
HINGE_A_START = OY_F + HINGE_INSET    # -27.48  — first knuckle Y start
HINGE_A_END = OY_B - HINGE_INSET      #  92.12  — last knuckle Y end
KNUCKLE_COUNT = 5                      # total segments (bottom=0,2,4  top=1,3)
PIN_RADIUS = 1.0                       # pin hole radius (~2mm pin / 1.75mm filament)
KNUCKLE_CLEARANCE = 0.3               # gap between adjacent knuckles (mm)

# Derived: pin axis position
PIN_AXIS_X = NX_L - KNUCKLE_RADIUS    # -101.762
PIN_AXIS_Z = HINGE_Z                  #   38.2

# --- Snap clips on right wall ---
CLIP_Z = 33.0                         # Z center of clips
CLIP_Y_POSITIONS = [20.0, 65.0]       # two clips spread along right wall
CLIP_WIDTH = 8.0
CLIP_HEIGHT = 4.0
CLIP_DEPTH = 2.0
CLIP_LIP_HEIGHT = 1.0
CLIP_LIP_DEPTH = 0.8
WALL_THICK = 3.5
