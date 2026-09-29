#!/usr/bin/env python3
"""Generates logo.svg: a minimalist chameleon wrapped around a Wayland-style
"W" disc. The body is one tapered shape: an arc around the disc that flows
into a spiral tail, so there are no seams."""
import math

C = (268.0, 270.0)       # disc centre
DISC_R = 112.0
BODY_R = 146.0           # body centreline radius
HEAD_AT = 292.0          # body ends here (degrees, SVG coords: 90 = down)
TAIL_AT = 150.0          # arc turns into the spiral here
W_BODY, W_TIP = 40.0, 12.0


def centerline():
    pts = []
    # arc from the head backwards (decreasing angle) to the tail start
    for i in range(0, 81):
        a = math.radians(HEAD_AT - (HEAD_AT - TAIL_AT + 360) % 360 * i / 80)
        pts.append((C[0] + BODY_R * math.cos(a), C[1] + BODY_R * math.sin(a)))
    # spiral: keeps the arc's tangent, then curls clockwise (away from the
    # disc) with a shrinking radius
    a0 = math.radians(TAIL_AT)
    d = (math.sin(a0), -math.cos(a0))               # travel direction
    n = (-d[1], d[0])                               # clockwise normal
    r0 = 52.0
    s = (pts[-1][0] + n[0] * r0, pts[-1][1] + n[1] * r0)
    phi0 = math.atan2(pts[-1][1] - s[1], pts[-1][0] - s[0])
    turns = 1.3
    for i in range(1, 121):
        t = i / 120
        phi = phi0 + t * turns * 2 * math.pi       # clockwise on screen
        r = r0 * (1 - 0.78 * t)
        pts.append((s[0] + r * math.cos(phi), s[1] + r * math.sin(phi)))
    return pts


def widths(n_arc, n_total):
    w = []
    for i in range(n_total):
        if i <= n_arc:
            w.append(W_BODY)
        else:
            t = (i - n_arc) / (n_total - 1 - n_arc)
            w.append(W_BODY + (W_TIP - W_BODY) * t ** 0.8)
    return w


def outline(pts, ws):
    left, right = [], []
    for i, (p, w) in enumerate(zip(pts, ws)):
        a = pts[max(i - 1, 0)]
        b = pts[min(i + 1, len(pts) - 1)]
        dx, dy = b[0] - a[0], b[1] - a[1]
        l = math.hypot(dx, dy) or 1
        nx, ny = -dy / l, dx / l
        left.append((p[0] + nx * w / 2, p[1] + ny * w / 2))
        right.append((p[0] - nx * w / 2, p[1] - ny * w / 2))
    tip = pts[-1]
    poly = left + [(tip[0], tip[1])] + right[::-1]
    return "M" + " L".join(f"{x:.1f} {y:.1f}" for x, y in poly) + " Z"


pts = centerline()
body = outline(pts, widths(80, len(pts)))
tip = pts[-1]
tip_w = W_TIP

# head, drawn pointing along +x from the neck, then placed at the arc end
ha = math.radians(HEAD_AT)
neck = (C[0] + BODY_R * math.cos(ha), C[1] + BODY_R * math.sin(ha))
head_rot = HEAD_AT + 90 - 360
head = ("M -6 -21 C -6 -40 6 -54 22 -56 C 46 -44 74 -22 90 -2 "
        "C 97 7 92 19 79 21 C 54 26 22 28 -6 21 Z")
mouth = "M 86 8 C 70 13 52 14 38 12"


def leg(angle_deg, toe_dir):
    """A short leg from the body onto the disc edge, with a small foot."""
    a = math.radians(angle_deg)
    hip = (C[0] + (BODY_R - 6) * math.cos(a), C[1] + (BODY_R - 6) * math.sin(a))
    b = math.radians(angle_deg + 9 * toe_dir)
    knee = (C[0] + (DISC_R + 20) * math.cos(b), C[1] + (DISC_R + 16) * math.sin(b))
    c = math.radians(angle_deg + 17 * toe_dir)
    foot = (C[0] + (DISC_R + 2) * math.cos(c), C[1] + (DISC_R + 2) * math.sin(c))
    return f"M{hip[0]:.1f} {hip[1]:.1f} L{knee[0]:.1f} {knee[1]:.1f} L{foot[0]:.1f} {foot[1]:.1f}"


legs = [leg(262, 1), leg(196, -1)]

svg = f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512">
  <title>Project Chameleon</title>
  <defs>
    <linearGradient id="body" x1="90" y1="450" x2="420" y2="120" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="#6C4CF1"/>
      <stop offset="0.55" stop-color="#22B8E6"/>
      <stop offset="1" stop-color="#3DDC84"/>
    </linearGradient>
    <linearGradient id="head" x1="-6" y1="0" x2="92" y2="0" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="#2CC9C4"/>
      <stop offset="1" stop-color="#3DDC84"/>
    </linearGradient>
    <linearGradient id="disc" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#FFD23F"/>
      <stop offset="1" stop-color="#FFA62B"/>
    </linearGradient>
  </defs>
  <circle cx="{C[0]:.0f}" cy="{C[1]:.0f}" r="{DISC_R:.0f}" fill="url(#disc)"/>
  <path d="M{C[0]-70:.0f} {C[1]-36:.0f} L{C[0]-35:.0f} {C[1]+54:.0f} L{C[0]:.0f} {C[1]-6:.0f} L{C[0]+35:.0f} {C[1]+54:.0f} L{C[0]+70:.0f} {C[1]-36:.0f}"
        fill="none" stroke="#1B1F3B" stroke-width="27" stroke-linecap="round" stroke-linejoin="round"/>
  {"".join(f'<path d="{d}" fill="none" stroke="url(#body)" stroke-width="17" stroke-linecap="round" stroke-linejoin="round"/>' for d in legs)}
  <path d="{body}" fill="url(#body)"/>
  <circle cx="{tip[0]:.1f}" cy="{tip[1]:.1f}" r="{tip_w/2:.1f}" fill="url(#body)"/>
  <g transform="translate({neck[0]:.1f} {neck[1]:.1f}) rotate({head_rot:.1f})">
    <path d="{head}" fill="url(#head)"/>
    <path d="{mouth}" fill="none" stroke="#1B1F3B" stroke-width="4" stroke-linecap="round" opacity="0.55"/>
    <circle cx="44" cy="-8" r="15" fill="#FFFFFF"/>
    <circle cx="48" cy="-8" r="7.5" fill="#1B1F3B"/>
  </g>
</svg>
"""
open("logo.svg", "w").write(svg)
