# Case

Screwless two-part enclosure for the rev 0.6 board: a tray that holds the PCB on four pegs, and a lid that clips on with six snap hooks and presses the board down at the mounting holes.

- Outside: 104.8 x 104.8 x 22.6 mm
- `base.stl`: print as is. `lid.stl`: print top face down. PLA or PETG, 0.2 mm layers, no supports.
- Regenerate after changing a parameter: `pip install build123d && python case.py`

## Openings

- USB-C J101 and J303 on the top edge, with a recess so the plug reaches the socket.
- 6.6 mm hole on the left side for an SMA bulkhead with a u.FL pigtail to J302.
- RESET and BOOT are flexible tabs in the lid with a stem onto each switch.
- LED holes above D101, D201 and D202. Push in a short piece of clear 1.75 mm filament as a light pipe, or leave them open.
- Vents above and below the modem.

The SIM holder is in the middle of the board, so take the lid off to change the SIM. To open the case, push the hooks in through the side windows with a small screwdriver.

If the fit is too tight or too loose, adjust `GAP`, `FIT` and the `BARB`/`CATCH` hook sizes at the top of `case.py`.
