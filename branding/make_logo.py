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
    knee = (C[0] + (DISC_R + 20) * math.cos(b), C[1] + (DISC_R + 20) * math.sin(b))
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


# ---- Android adaptive launcher icon (same geometry) ----
# Foreground/monochrome layers are 108dp; launchers may mask everything
# outside the central 66dp circle, so the logo is fitted inside it.
import os
import re

RES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "android", "app", "src", "main", "res")
BG = "#101828"


def circle_path(cx, cy, r):
    return f"M{cx - r:.1f} {cy:.1f} a{r:.1f} {r:.1f} 0 1 0 {2 * r:.1f} 0 a{r:.1f} {r:.1f} 0 1 0 {-2 * r:.1f} 0 Z"


def head_points():
    rot = math.radians(head_rot)
    for x, y in zip(*[iter(map(float, re.findall(r"-?\d+\.?\d*", head)))] * 2):
        yield (neck[0] + x * math.cos(rot) - y * math.sin(rot), neck[1] + x * math.sin(rot) + y * math.cos(rot))


xs, ys = zip(*(list(pts) + list(head_points()) + [(C[0] - DISC_R, C[1] - DISC_R), (C[0] + DISC_R, C[1] + DISC_R)]))
pad = W_BODY / 2
x0, x1, y0, y1 = min(xs) - pad, max(xs) + pad, min(ys) - pad, max(ys) + pad
# Launcher masks are round: fit by the farthest point from the centre, into
# a 31dp radius (the guaranteed-visible circle is 33dp).
mx, my = (x0 + x1) / 2, (y0 + y1) / 2
reach = max(math.hypot(x - mx, y - my) for x, y in zip(xs, ys)) + pad
scale = 31.0 / reach
tx = 54 - mx * scale
ty = 54 - my * scale

A = 'xmlns:android="http://schemas.android.com/apk/res/android" xmlns:aapt="http://schemas.android.com/aapt"'


def gradient(attr, x_1, y_1, x_2, y_2, stops):
    items = "".join(f'<item android:offset="{o}" android:color="{c}"/>' for o, c in stops)
    return (f'<aapt:attr name="android:{attr}"><gradient android:type="linear" android:startX="{x_1}" '
            f'android:startY="{y_1}" android:endX="{x_2}" android:endY="{y_2}">{items}</gradient></aapt:attr>')


BODY_STOPS = [(0, "#FF6C4CF1"), (0.55, "#FF22B8E6"), (1, "#FF3DDC84")]
body_fill = lambda attr: gradient(attr, 90, 450, 420, 120, BODY_STOPS)
disc_fill = gradient("fillColor", C[0], C[1] - DISC_R, C[0], C[1] + DISC_R, [(0, "#FFFFD23F"), (1, "#FFFFA62B")])
head_fill = gradient("fillColor", -6, 0, 92, 0, [(0, "#FF2CC9C4"), (1, "#FF3DDC84")])
w_path = (f"M{C[0]-70:.0f} {C[1]-36:.0f} L{C[0]-35:.0f} {C[1]+54:.0f} L{C[0]:.0f} {C[1]-6:.0f} "
          f"L{C[0]+35:.0f} {C[1]+54:.0f} L{C[0]+70:.0f} {C[1]-36:.0f}")
stroke = 'android:strokeLineCap="round" android:strokeLineJoin="round"'


def layer(mono):
    out = [f'<vector {A} android:width="108dp" android:height="108dp" android:viewportWidth="108" android:viewportHeight="108">',
           f'<group android:translateX="{tx:.3f}" android:translateY="{ty:.3f}" android:scaleX="{scale:.5f}" android:scaleY="{scale:.5f}">']
    ink = "#FFFFFFFF"
    if mono:
        # One colour (the system tints it): the disc becomes a ring.
        out.append(f'<path android:pathData="{circle_path(C[0], C[1], DISC_R - 9)}" android:strokeColor="{ink}" android:strokeWidth="18"/>')
        out.append(f'<path android:pathData="{w_path}" android:strokeColor="{ink}" android:strokeWidth="27" {stroke}/>')
    else:
        out.append(f'<path android:pathData="{circle_path(C[0], C[1], DISC_R)}">{disc_fill}</path>')
        out.append(f'<path android:pathData="{w_path}" android:strokeColor="#FF1B1F3B" android:strokeWidth="27" {stroke}/>')
    for d in legs:
        paint = f'android:strokeColor="{ink}"' if mono else ""
        out.append(f'<path android:pathData="{d}" {paint} android:strokeWidth="17" {stroke}>'
                   f'{"" if mono else body_fill("strokeColor")}</path>')
    for d in (body, circle_path(tip[0], tip[1], tip_w / 2)):
        out.append(f'<path android:pathData="{d}" android:fillColor="{ink}"/>' if mono
                   else f'<path android:pathData="{d}">{body_fill("fillColor")}</path>')
    out.append(f'<group android:translateX="{neck[0]:.2f}" android:translateY="{neck[1]:.2f}" android:rotation="{head_rot:.2f}">')
    if mono:
        # eye punched out of the head
        out.append(f'<path android:fillType="evenOdd" android:fillColor="{ink}" '
                   f'android:pathData="{head} {circle_path(44, -8, 11)}"/>')
    else:
        out.append(f'<path android:pathData="{head}">{head_fill}</path>')
        out.append(f'<path android:pathData="{mouth}" android:strokeColor="#8C1B1F3B" android:strokeWidth="4" android:strokeLineCap="round"/>')
        out.append(f'<path android:pathData="{circle_path(44, -8, 15)}" android:fillColor="#FFFFFFFF"/>')
        out.append(f'<path android:pathData="{circle_path(48, -8, 7.5)}" android:fillColor="#FF1B1F3B"/>')
    out.append("</group></group></vector>")
    return "\n".join(out) + "\n"


if os.path.isdir(RES):
    os.makedirs(os.path.join(RES, "drawable"), exist_ok=True)
    os.makedirs(os.path.join(RES, "mipmap-anydpi"), exist_ok=True)
    open(os.path.join(RES, "drawable", "ic_launcher_foreground.xml"), "w").write(layer(False))
    open(os.path.join(RES, "drawable", "ic_launcher_monochrome.xml"), "w").write(layer(True))
    open(os.path.join(RES, "values", "ic_launcher_colors.xml"), "w").write(
        f'<?xml version="1.0" encoding="utf-8"?>\n<resources>\n    <color name="ic_launcher_background">{BG}</color>\n</resources>\n')
    adaptive = ('<?xml version="1.0" encoding="utf-8"?>\n'
                '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
                '    <background android:drawable="@color/ic_launcher_background"/>\n'
                '    <foreground android:drawable="@drawable/ic_launcher_foreground"/>\n'
                '    <monochrome android:drawable="@drawable/ic_launcher_monochrome"/>\n'
                '</adaptive-icon>\n')
    # An adaptive icon is masked by the launcher, so no separate round icon.
    open(os.path.join(RES, "mipmap-anydpi", "ic_launcher.xml"), "w").write(adaptive)
