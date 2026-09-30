#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Renders icon-preview.png: the launcher icon under circle, squircle and
rounded-square masks, plus a themed (monochrome) rendition. Needs
rsvg-convert and Pillow. Run after make_logo.py."""
import math
import re
import subprocess
import tempfile

from PIL import Image, ImageDraw

fg = open("../android/app/src/main/res/drawable/ic_launcher_foreground.xml").read()
tx, ty, sc = map(float, re.search(
    r'translateX="([\d.-]+)" android:translateY="([\d.-]+)" android:scaleX="([\d.]+)"', fg).groups())
inner = open("logo.svg").read().split(">", 1)[1].rsplit("</svg>", 1)[0]
tmp = tempfile.mkdtemp()


def render(bg, content):
    path = f"{tmp}/icon.svg"
    open(path, "w").write(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 108 108">'
                          f'<rect width="108" height="108" fill="{bg}"/>'
                          f'<g transform="translate({tx} {ty}) scale({sc})">{content}</g></svg>')
    subprocess.run(["rsvg-convert", "-w", "432", "-h", "432", path, "-o", f"{tmp}/icon.png"], check=True)
    return Image.open(f"{tmp}/icon.png").convert("RGBA").crop((72, 72, 360, 360))  # visible 72dp


def mask(img, shape):
    n = img.size[0]
    m = Image.new("L", (n, n), 0)
    d = ImageDraw.Draw(m)
    if shape == "circle":
        d.ellipse([0, 0, n - 1, n - 1], fill=255)
    elif shape == "squircle":
        pts = []
        for i in range(360):
            c, s = math.cos(math.radians(i)), math.sin(math.radians(i))
            pts.append((n / 2 + n / 2 * math.copysign(abs(c) ** 0.5, c), n / 2 + n / 2 * math.copysign(abs(s) ** 0.5, s)))
        d.polygon(pts, fill=255)
    else:
        d.rounded_rectangle([0, 0, n - 1, n - 1], radius=n * 0.22, fill=255)
    out = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    out.paste(img, (0, 0), m)
    return out


color = render("#101828", inner)
mono = re.sub(r"<(linearGradient|stop)[^>]*>|</linearGradient>", "", inner)
mono = re.sub(r'(fill|stroke)="url\(#[a-z]+\)"', r'\1="#2E3A59"', mono)
themed = render("#D6E4FF", mono)
n = color.size[0]
sheet = Image.new("RGBA", (4 * n + 100, n + 40), "#E9E4F5")
for i, shape in enumerate(("circle", "squircle", "rounded")):
    sheet.alpha_composite(mask(color, shape), (20 + i * (n + 20), 20))
sheet.alpha_composite(mask(themed, "circle"), (20 + 3 * (n + 20), 20))
sheet.save("icon-preview.png")
