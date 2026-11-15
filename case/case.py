"""Snap-fit (screwless) enclosure for the LTEGTW rev 0.6 board.

    pip install build123d
    python case.py            # writes base.stl and lid.stl

All positions come from hardware/sms.kicad_pcb (KiCad coordinates, board
origin at 100,100, y pointing down). Change the parameters below to adjust
the fit for your printer.
"""

from pathlib import Path

from build123d import (
    Align, Box, Cylinder, Plane, Polygon, Pos, Rot, RectangleRounded,
    SlotCenterToCenter, Text, export_stl, extrude,
)

# --- board (from KiCad) -----------------------------------------------------
PCB = 99.8          # square outline
PCB_T = 1.6
HOLES = [(103.5, 103.5), (162, 103.5), (103.5, 196.5), (196.5, 196.5)]
USB_C = [113, 128]  # J101, J303 x (top edge)
UFL_Y = 167         # J302, left edge -> SMA bulkhead
BUTTONS = [(140, 105, "RST"), (149, 105, "BOOT")]   # SW201, SW202
LEDS_PWR = [(132.5, 127.5)]                          # D101
LEDS_STATUS = [(167.8, 111.31), (167.8, 113.6)]      # D201, D202
MODEM = (132, 167)  # U301 centre, for the vents

# --- printer / fit ----------------------------------------------------------
WALL = 2.0
FLOOR = 2.0
LID_T = 2.0
GAP = 0.5           # board edge to wall
FIT = 0.25          # sliding clearance lid skirt <-> wall
STANDOFF = 5.0      # clearance under the board (THT legs)
HEADROOM = 12.0     # board top to lid underside
R_OUT = 3.0         # outer corner radius

# hooks: (wall, position along that wall in case mm)
HOOK_W, HOOK_T, HOOK_L = 6.0, 1.6, 9.0
BARB, RAMP, CATCH = 1.0, 2.0, 0.8
HOOKS = [("left", 15), ("left", 72), ("right", 15), ("right", 55),
         ("bottom", 30), ("bottom", 78)]

SMA_D = 6.6         # 1/4-36 SMA bulkhead thread
SMA_Z = 6.0         # above board top
PIPE_D = 1.85       # takes a piece of 1.75 mm clear filament as light pipe

# --- derived ----------------------------------------------------------------
W = PCB + 2 * (GAP + WALL)
C = W / 2
O = WALL + GAP                  # board origin offset inside the case
Z_PCB = FLOOR + STANDOFF        # board bottom
Z_TOP_PCB = Z_PCB + PCB_T
Z_TOP = Z_TOP_PCB + HEADROOM    # top of the walls = lid underside
IN = W - 2 * WALL


def kc(x, y):
    """KiCad coordinates -> case coordinates (y flipped, top edge stays top)."""
    return (O + x - 100, O + (100 + PCB) - y)


def prism(cx, cy, w, h, z0, z1, r=0.0):
    sk = RectangleRounded(w, h, r) if r > 0 else RectangleRounded(w, h, 0.01)
    return Pos(cx, cy, z0) * extrude(sk, z1 - z0)


def on_wall(wall, shape):
    """Shapes are modelled on the right wall (outward +X, position = Y)."""
    angle = {"right": 0, "top": 90, "left": 180, "bottom": 270}[wall]
    return Pos(C, C, 0) * Rot(0, 0, angle) * Pos(-C, -C, 0) * shape


def along(wall, p):
    """Position along a wall (case X for top/bottom, Y for left/right)."""
    return {"right": p, "top": W - p, "left": W - p, "bottom": p}[wall]


def hook_window(wall, p):
    t = along(wall, p)
    zb = Z_TOP - HOOK_L
    zc = zb + RAMP + CATCH
    return on_wall(wall, Pos(W - WALL / 2, t, (zb - 0.3 + zc + 0.15) / 2) *
                   Box(WALL + 1, HOOK_W + 1.0, zc + 0.15 - (zb - 0.3)))


def hook(wall, p):
    t = along(wall, p)
    xi = W - WALL - 0.2
    zb = Z_TOP - HOOK_L
    arm = Pos(xi - HOOK_T / 2, t, Z_TOP - HOOK_L / 2) * Box(HOOK_T, HOOK_W, HOOK_L)
    prof = Polygon((xi - 0.01, zb), (xi + BARB, zb + RAMP),
                   (xi + BARB, zb + RAMP + CATCH), (xi - 0.01, zb + RAMP + CATCH),
                   align=None)
    barb = extrude(Plane.XZ * prof, HOOK_W / 2, both=True)
    barb = Pos(0, t, 0) * barb
    return on_wall(wall, arm + barb)


def base():
    b = prism(C, C, W, W, 0, Z_TOP, R_OUT)
    b -= prism(C, C, IN, IN, FLOOR, Z_TOP + 1, 1.0)
    # lead-in chamfer on the top inner edge so the hooks ride over it
    b -= prism(C, C, IN + 0.8, IN + 0.8, Z_TOP - 0.4, Z_TOP + 1, 1.2)

    for x, y in HOLES:
        cx, cy = kc(x, y)
        b += Pos(cx, cy, FLOOR - 0.01) * Cylinder(3.0, STANDOFF + 0.01,
                                                  align=(Align.CENTER, Align.CENTER, Align.MIN))
        b += Pos(cx, cy, Z_PCB) * Cylinder(1.45, PCB_T - 0.2,
                                           align=(Align.CENTER, Align.CENTER, Align.MIN))

    # USB-C: opening + outside recess so the plug overmold reaches the socket
    for x in USB_C:
        cx, _ = kc(x, 104)
        z = Z_TOP_PCB + 1.63
        b -= Pos(cx, W - WALL / 2, z) * Rot(90, 0, 0) * extrude(
            RectangleRounded(9.6, 4.0, 1.6), WALL + 2, both=True)
        b -= Pos(cx, W, z) * Rot(90, 0, 0) * extrude(
            RectangleRounded(13.0, 7.5, 2.5), 1.0, both=True)

    # SMA bulkhead for the LTE antenna (u.FL pigtail to J302)
    _, cy = kc(108.75, UFL_Y)
    b -= Pos(0, cy, Z_TOP_PCB + SMA_Z) * Rot(0, 90, 0) * Cylinder(SMA_D / 2, WALL * 3)

    for wall, p in HOOKS:
        b -= hook_window(wall, p)

    # floor vents under the modem
    mx, my = kc(*MODEM)
    for i in range(-3, 4):
        b -= Pos(mx + i * 4, my, 0) * extrude(SlotCenterToCenter(16, 2.0, rotation=90), FLOOR * 3, both=True)
    return b


def lid():
    zl = Z_TOP
    l = prism(C, C, W, W, zl, zl + LID_T, R_OUT)

    # locating skirt, interrupted at the hooks and the SMA connector
    s_out = IN - 2 * FIT
    skirt = prism(C, C, s_out, s_out, zl - 3, zl + 0.01, 0.8)
    skirt -= prism(C, C, s_out - 2.4, s_out - 2.4, zl - 4, zl + 1, 0.5)
    for wall, p in HOOKS:
        skirt -= on_wall(wall, Pos(W - WALL, along(wall, p), zl - 2) * Box(8, HOOK_W + 2.0, 6))
    _, cy = kc(108.75, UFL_Y)
    skirt -= Pos(WALL, cy, zl - 2) * Box(8, 14, 6)
    l += skirt

    for wall, p in HOOKS:
        l += hook(wall, p)

    # hold-down tubes on the mounting holes (around the base pegs)
    for x, y in HOLES:
        cx, cy = kc(x, y)
        t = Pos(cx, cy, Z_TOP_PCB) * Cylinder(3.0, zl - Z_TOP_PCB + 0.01,
                                              align=(Align.CENTER, Align.CENTER, Align.MIN))
        t -= Pos(cx, cy, Z_TOP_PCB - 1) * Cylinder(1.8, 3.0,
                                                   align=(Align.CENTER, Align.CENTER, Align.MIN))
        l += t

    # light pipes: power LED alone, D201/D202 in one block with a divider
    for leds, z_gap in ((LEDS_PWR, 1.0), (LEDS_STATUS, 1.2)):
        pts = [kc(x, y) for x, y in leds]
        xs, ys = [p[0] for p in pts], [p[1] for p in pts]
        cx, cy = sum(xs) / len(xs), sum(ys) / len(ys)
        w = max(xs) - min(xs) + PIPE_D + 2.0
        h = max(ys) - min(ys) + PIPE_D + 2.0
        l += prism(cx, cy, w, h, Z_TOP_PCB + z_gap, zl + 0.01, 0.8)
        for px, py in pts:
            l -= Pos(px, py, 0) * Cylinder(PIPE_D / 2, 100)
            l -= Pos(px, py, zl + LID_T - 0.5) * Cylinder(PIPE_D / 2 + 0.6, 1.0,
                                                          align=(Align.CENTER, Align.CENTER, Align.MIN))

    # flexure buttons: a tongue cut in the lid with a stem onto the switch
    for x, y, label in BUTTONS:
        bx, by = kc(x, y)
        tip, length, tw, slot = 2.5, 13.0, 5.5, 0.6
        y0, y1 = by - (length - tip), by + tip
        ring = prism(bx, (y0 + y1) / 2, tw + 2 * slot, (y1 - y0) + slot, zl - 1, zl + LID_T + 1, 1.0)
        ring -= prism(bx, (y0 + y1) / 2 - slot / 2, tw, (y1 - y0), zl - 2, zl + LID_T + 2, 0.8)
        ring -= Pos(bx, y0 - 5, zl) * Box(tw + 4, 10, 10)           # keep the hinge
        l -= ring
        l -= prism(bx, (y0 + y1) / 2, tw, y1 - y0, zl - 1, zl + 0.8, 0.8)  # thin to 1.2 mm
        l += Pos(bx, by, Z_TOP_PCB + 1.5 + 0.3) * Cylinder(
            1.5, zl + 0.81 - (Z_TOP_PCB + 1.8), align=(Align.CENTER, Align.CENTER, Align.MIN))
        l += Pos(bx, by, zl + LID_T - 0.01) * Cylinder(2.0, 0.6,
                                                        align=(Align.CENTER, Align.CENTER, Align.MIN))
        l -= Pos(bx, y0 - 3.0, zl + LID_T - 0.4) * extrude(Text(label, 3.0), 1.0)

    # vents over the modem, and the name
    mx, my = kc(*MODEM)
    for i in range(-3, 4):
        l -= Pos(mx + i * 4, my, 0) * extrude(SlotCenterToCenter(16, 2.0, rotation=90), 200, both=True)
    l -= Pos(C + 22, kc(0, 180)[1], zl + LID_T - 0.6) * extrude(Text("LTEGTW", 7), 1.0)
    return l


if __name__ == "__main__":
    out = Path(__file__).parent
    b, l = base(), lid()
    # print orientation: base as is, lid upside down on its top face
    export_stl(b, str(out / "base.stl"))
    export_stl(Pos(0, 0, Z_TOP + LID_T) * Rot(180, 0, 0) * l, str(out / "lid.stl"))
    print(f"outer {W:.1f} x {W:.1f} x {Z_TOP + LID_T:.1f} mm")
