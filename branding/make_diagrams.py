#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Writes the README's diagrams, each for GitHub's light and dark theme:
architecture-{light,dark}.svg and frame-{light,dark}.svg, next to this file.

usage: make_diagrams.py"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SANS = "-apple-system, 'Segoe UI', Helvetica, Arial, sans-serif"
import sys
# GitHub's own greys, so the pictures sit quietly in a README; one accent,
# the logo's green, softened, marks what is Chameleon's.
THEMES = {
    "light": dict(ink="#1F2328", soft="#59636E", line="#D1D9E0", fill="#FFFFFF", tint="#F6F8FA",
                  accent="#1A7F4B", accent_fill="#EEF8F1", accent_line="#9FD4B5"),
    "dark": dict(ink="#E6EDF3", soft="#9198A1", line="#3D444D", fill="#151B23", tint="#212830",
                 accent="#56D364", accent_fill="#12261B", accent_line="#2A6B41"),
}
if len(sys.argv) > 1 and sys.argv[1] == "blue":  # a try-out, not the default
    THEMES["light"].update(accent="#0969DA", accent_fill="#EEF5FE", accent_line="#A5C8F3")
    THEMES["dark"].update(accent="#4493F8", accent_fill="#111D2E", accent_line="#264F86")
W = 960


def head(height, t):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {height}" width="{W}" height="{height}" '
            f'role="img">\n'
            f'<style>text{{font-family:{SANS};fill:{t["ink"]}}}.label{{font-size:14px;'
            f'font-weight:600;fill:{t["ink"]}}}.name{{font-size:15px;font-weight:600}}'
            f'.sub{{font-size:12px;fill:{t["soft"]}}}.pill{{font-size:12px;fill:{t["soft"]}}}'
            f'.num{{font-size:12px;font-weight:700;fill:{t["fill"]}}}</style>\n')


def box(x, y, w, h, name, sub, t, brand=False):
    stroke = t["accent_line"] if brand else t["line"]
    fill = t["accent_fill"] if brand else t["fill"]
    return (f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="8" fill="{fill}" stroke="{stroke}"/>\n<text class="name" x="{x + w / 2}" y="{y + h / 2 - 3}" '
            f'text-anchor="middle">{name}</text>\n<text class="sub" x="{x + w / 2}" y="{y + h / 2 + 16}" '
            f'text-anchor="middle">{sub}</text>\n')


def row(y, h, items, t, brand=False, left=180, right=W - 30, gap=14):
    w = (right - left - gap * (len(items) - 1)) / len(items)
    return "".join(box(left + i * (w + gap), y, w, h, name, sub, t, brand) for i, (name, sub) in enumerate(items))


def pills(y, label, items, t, left=180, right=W - 30):
    out = f'<text class="label" x="30" y="{y + 4}">{label}</text>\n'
    out += f'<line x1="{left}" y1="{y}" x2="{right}" y2="{y}" stroke="{t["line"]}"/>\n'
    step = (right - left) / len(items)
    for i, item in enumerate(items):
        w = len(item) * 6.6 + 24
        cx = left + step * (i + 0.5)
        out += (f'<rect x="{cx - w / 2}" y="{y - 12}" width="{w}" height="24" rx="12" fill="{t["tint"]}"/>\n<text class="pill" x="{cx}" y="{y + 4}" text-anchor="middle">{item}</text>\n')
    return out


def architecture(t):
    out = head(476, t)
    out += f'<text class="label" x="30" y="78">Linux</text>\n<text class="sub" x="30" y="96">as packaged</text>\n'
    out += row(40, 76, [("Compositor", "KWin · wlroots · Weston · Mutter"), ("Wayland apps", "Qt · GTK · Firefox"),
                        ("Vulkan apps", "native · games")], t)
    out += pills(158, "Interfaces", ["DRM/KMS", "GBM", "EGL · GLES", "Vulkan", "udev · evdev", "input method"], t)
    out += (f'<text class="label" x="30" y="238" style="fill:{t["accent"]}">Chameleon</text>\n'
            f'<text class="sub" x="30" y="256">the device layer</text>\n')
    out += row(200, 76, [("chameleon-kernel", "display · input devices"), ("libgbm", "GPU buffers"),
                         ("EGL vendor", "OpenGL ES"), ("Vulkan driver", "Vulkan")], t, brand=True)
    out += pills(318, "Android APIs", ["AHardwareBuffer", "sync fences", "ASurfaceControl", "AImageReader"], t)
    out += f'<text class="label" x="30" y="398">Android</text>\n<text class="sub" x="30" y="416">no root</text>\n'
    out += row(360, 76, [("Chameleon app", "screen · touch · keyboard"), ("GPU driver", "Mali · Adreno · …"),
                         ("SurfaceFlinger", "the phone's display")], t)
    return out + "</svg>\n"


def frame(t):
    """The one moving picture: a dot runs the frame's path, and each step
    answers as it passes. Still for readers who ask for reduced motion."""
    steps = [("Render", "the compositor draws", "into a GPU buffer"),
             ("Commit", "atomic commit on the", "card, with a fence"),
             ("Present", "the app copies it on the", "GPU, hands it to Android"),
             ("Display", "shown at the", "panel's next vsync"),
             ("Page flip", "the compositor is told,", "timed on that vsync")]
    left, right, y, loop = 90, W - 90, 60, 7.5
    leg = 80 / (len(steps) - 1)  # % of the loop from one step to the next
    out = head(190, t)
    out += (f'<style>.ring{{fill:none;stroke:{t["accent"]};stroke-width:2;opacity:0;transform-box:fill-box;'
            f'transform-origin:center;animation:ring {loop}s ease-out infinite}}'
            f'.dot{{fill:{t["accent"]};animation:run {loop}s linear infinite}}'
            f'@keyframes run{{0%{{transform:translateX(0);opacity:1}}80%{{transform:translateX({right - left}px);'
            f'opacity:1}}88%,100%{{transform:translateX({right - left}px);opacity:0}}}}'
            f'@keyframes ring{{0%{{opacity:.8;transform:scale(1)}}14%{{opacity:0;transform:scale(1.9)}}'
            f'100%{{opacity:0;transform:scale(1.9)}}}}'
            f'@media (prefers-reduced-motion:reduce){{.ring,.dot{{animation:none;opacity:0}}}}</style>\n')
    out += f'<line x1="{left}" y1="{y}" x2="{right}" y2="{y}" stroke="{t["line"]}" stroke-width="2"/>\n'
    out += f'<circle class="dot" cx="{left}" cy="{y}" r="6"/>\n'
    for i, (name, a, b) in enumerate(steps):
        x = left + (right - left) * i / (len(steps) - 1)
        out += (f'<circle class="ring" cx="{x}" cy="{y}" r="15" style="animation-delay:{loop * leg * i / 100:.2f}s"/>\n'
                f'<circle cx="{x}" cy="{y}" r="13" fill="{t["accent"]}"/>\n'
                f'<text class="num" x="{x}" y="{y + 4}" text-anchor="middle">{i + 1}</text>\n'
                f'<text class="name" x="{x}" y="{y + 44}" text-anchor="middle">{name}</text>\n'
                f'<text class="sub" x="{x}" y="{y + 66}" text-anchor="middle">{a}</text>\n'
                f'<text class="sub" x="{x}" y="{y + 83}" text-anchor="middle">{b}</text>\n')
    return out + "</svg>\n"


for theme, t in THEMES.items():
    for name, draw in (("architecture", architecture), ("frame", frame)):
        with open(os.path.join(HERE, f"{name}-{theme}.svg"), "w") as f:
            f.write(draw(t))
