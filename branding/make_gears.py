#!/usr/bin/env python3
"""Two meshing gears as pixel art, turning: gears.gif (transparent).
usage: make_gears.py [OUT.gif]"""
import math, sys
from PIL import Image

W, H, SCALE, FRAMES = 60, 40, 12, 16      # the mascot's pixel size
M = 2.5                                    # module: pitch radius = M * teeth / 2
BIG = dict(n=12, cx=20.5, cy=21.5, face=(0x8C, 0x9A, 0xB0), shade=(0x6B, 0x7A, 0x90))
SMALL = dict(n=6, face=(0xB8, 0xC4, 0xD6), shade=(0x8C, 0x9A, 0xB0))
HOLE = (0x24, 0x30, 0x2A)
PHI = math.radians(-24)                    # from the big gear's centre to the small one's
dist = M * (BIG["n"] + SMALL["n"]) / 2
SMALL.update(cx=BIG["cx"] + dist * math.cos(PHI), cy=BIG["cy"] + dist * math.sin(PHI))


def colour(g, turn, x, y):
    dx, dy = x + 0.5 - g["cx"], y + 0.5 - g["cy"]
    r, pitch = math.hypot(dx, dy), 2 * math.pi / g["n"]
    rp = M * g["n"] / 2
    root, tip = rp - M * 0.75, rp + M * 0.75
    if r > tip:
        return None
    if r < M * 0.75:
        return HOLE
    if r < root:
        return g["shade"] if r < root - M * 0.9 and r > M * 1.5 else g["face"]
    a = (math.atan2(dy, dx) - turn + pitch / 2) % pitch - pitch / 2   # from the nearest tooth's middle
    half = pitch * (0.29 - 0.10 * (r - root) / (tip - root))
    return g["face"] if abs(a) <= half else None


frames = []
for f in range(FRAMES):
    a = 2 * math.pi / BIG["n"] * f / FRAMES            # one tooth per loop
    big_turn = PHI + a
    small_turn = PHI + math.pi + math.pi / SMALL["n"] - a * BIG["n"] / SMALL["n"]
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    px = im.load()
    for y in range(H):
        for x in range(W):
            c = colour(BIG, big_turn, x, y) or colour(SMALL, small_turn, x, y)
            if c:
                px[x, y] = c + (255,)
    frames.append(im.resize((W * SCALE, H * SCALE), Image.NEAREST))
frames[0].save(sys.argv[1] if len(sys.argv) > 1 else "gears.gif", save_all=True, append_images=frames[1:],
               duration=90, loop=0, disposal=2)
