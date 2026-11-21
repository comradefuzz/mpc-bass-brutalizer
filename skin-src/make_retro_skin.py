#!/usr/bin/env python3
"""Retro Soviet-instrument skin for Bass Brutalizer (stdlib only).

Worn olive enamel panels, bakelite knobs with printed scales, steel faders, key switches
with waveform icons, stencil legends and small-print hints drawn into the page backgrounds.

Usage (from anywhere):
    python3 "plugins/Bass Brutalizer/skin-src/make_retro_skin.py" [--preview]

It runs the SDK's own UI generator on ui.json (so slots, parameters and Q-Link rectangles stay
correct), then restyles the result and writes plugins/Bass Brutalizer/skins/. build.py copies a
skins/ folder verbatim. Run a normal build once first (it creates build/params.json).
Re-run this script whenever ui.json changes.
"""
import copy
import importlib.util
import json
import math
import os
import random
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PLUGIN = os.path.dirname(HERE)
SDK = os.path.abspath(os.path.join(PLUGIN, "..", ".."))
sys.path.insert(0, HERE)
import pixfont  # noqa: E402

_spec = importlib.util.spec_from_file_location("kit", os.path.join(SDK, "tools", "make_ui_kit.py"))
kit = importlib.util.module_from_spec(_spec)
sys.modules["kit"] = kit
_spec.loader.exec_module(kit)

mix, with_alpha, Canvas = kit.mix, kit.with_alpha, kit.Canvas
rrect, rrect_outline, circle, ring, arc, capsule, vgrad = (
    kit.rrect, kit.rrect_outline, kit.circle, kit.ring, kit.arc, kit.capsule, kit.vgrad)

SWEEP = kit.SWEEP
FRAMES = 127

# ---------------------------------------------------------------------------------------------
# Palette
# ---------------------------------------------------------------------------------------------
CREAM = (226, 216, 174, 255)
CREAM_DIM = (150, 142, 112, 255)
AMBER = (246, 180, 66, 255)
AMBER_D = (150, 96, 28, 255)
RED = (196, 58, 44, 255)
INK = (22, 20, 18, 255)
STEEL_L = (212, 212, 198, 255)
STEEL = (150, 150, 138, 255)
STEEL_D = (84, 84, 76, 255)
BAKE_L = (88, 84, 76, 255)
BAKE = (36, 34, 31, 255)
BAKE_D = (14, 13, 12, 255)
PANEL1 = (62, 74, 59, 255)
PANEL2 = (44, 54, 43, 255)

TXT_NAME = "ffc8bd92"    # parameter names
TXT_VALUE = "fff4b240"   # values
TXT_CAPTION = "ffe6dcb4"  # key captions
BG_COLOUR = "ff2c352b"

SLOT_X = (23, 343, 663, 983)
SLOT_Y = (45, 196, 346, 497)
SLOT_W, SLOT_H = 278, 107


# ---------------------------------------------------------------------------------------------
# Knob (90 x 90, 127 frames)
# ---------------------------------------------------------------------------------------------
def knob_static(bipolar, seed):
    rnd = random.Random(seed)
    c = Canvas(90, 90)
    cx = cy = 45.0
    n = 11
    for i in range(n):
        a = -SWEEP / 2 + SWEEP * i / (n - 1)
        long_tick = i in (0, 5, 10)
        r0, r1 = 38.5, (44.5 if long_tick else 42.0)
        col = CREAM if (bipolar and i == 5) else CREAM_DIM
        capsule(c, cx + math.sin(a) * r0, cy - math.cos(a) * r0, cx + math.sin(a) * r1, cy - math.cos(a) * r1,
                2.2 if long_tick else 1.6, col)
    arc(c, cx, cy, 30.5, 35.0, -SWEEP / 2, SWEEP / 2, (20, 24, 20, 255))
    # bakelite body
    circle(c, cx, cy + 1.5, 29.0, with_alpha(INK, 160))
    circle(c, cx, cy, 28.0, kit.light_rim(cx, cy, BAKE_L, BAKE_D))
    circle(c, cx, cy, 24.5, vgrad(cy - 24.5, cy + 24.5, (62, 58, 52, 255), (26, 24, 22, 255)))
    ring(c, cx, cy, 17.0, 18.0, with_alpha(INK, 90))
    ring(c, cx, cy, 11.0, 11.8, with_alpha(INK, 70))
    # scuffs on the bakelite
    for _ in range(14):
        a = rnd.uniform(0, 2 * math.pi)
        r = rnd.uniform(6, 22)
        x, y = cx + math.cos(a) * r, cy + math.sin(a) * r
        l = rnd.uniform(1.5, 5.0)
        b = rnd.uniform(0, math.pi)
        capsule(c, x, y, x + math.cos(b) * l, y + math.sin(b) * l, 0.9, with_alpha(CREAM, rnd.randint(18, 50)))
    return c


def knob_frame(static, t, bipolar):
    c = static.copy()
    cx = cy = 45.0
    a = -SWEEP / 2 + SWEEP * t
    if bipolar:
        if abs(t - 0.5) > 0.002:
            arc(c, cx, cy, 30.5, 35.0, min(0.0, a), max(0.0, a), AMBER)
    elif t > 0.002:
        arc(c, cx, cy, 30.5, 35.0, -SWEEP / 2, a, AMBER)
    x0, y0 = cx + math.sin(a) * 8.0, cy - math.cos(a) * 8.0
    x1, y1 = cx + math.sin(a) * 22.0, cy - math.cos(a) * 22.0
    capsule(c, x0, y0 + 0.8, x1, y1 + 0.8, 5.0, with_alpha(INK, 150))
    capsule(c, x0, y0, x1, y1, 4.0, CREAM)
    circle(c, cx, cy, 5.5, vgrad(cy - 5.5, cy + 5.5, STEEL_L, STEEL_D))
    return c


# ---------------------------------------------------------------------------------------------
# Fader (248 x 36, 127 frames)
# ---------------------------------------------------------------------------------------------
FW, FH = 248, 36
F_FROM, F_TO, F_MID = 12.0, 236.0, 19.0


def fader_static():
    c = Canvas(FW, FH)
    for i in range(11):
        x = F_FROM + (F_TO - F_FROM) * i / 10.0
        capsule(c, x, 1.0, x, 7.0 if i % 5 == 0 else 5.0, 1.6, CREAM_DIM)
    rrect(c, F_FROM - 5, F_MID - 3, F_TO - F_FROM + 10, 10, 3.0, with_alpha(INK, 255))
    rrect(c, F_FROM - 4, F_MID - 2, F_TO - F_FROM + 8, 8, 2.5, (22, 26, 22, 255))
    rrect_outline(c, F_FROM - 5, F_MID - 3, F_TO - F_FROM + 10, 10, 3.0, 1.0, with_alpha(STEEL_D, 220))
    return c


def fader_cap():
    w, h = 26, 26
    c = Canvas(w + 2, h + 4)
    rrect(c, 1, 3, w, h, 3.0, with_alpha(INK, 170))
    rrect(c, 1, 1, w, h, 3.0, vgrad(1, 1 + h, STEEL_L, STEEL_D))
    rrect_outline(c, 1, 1, w, h, 3.0, 1.0, with_alpha(INK, 220))
    cx = 1 + w / 2.0
    capsule(c, cx, 6, cx, 1 + h - 5, 3.2, with_alpha(INK, 200))
    capsule(c, cx, 6, cx, 1 + h - 5, 2.0, RED)
    return c


def fader_frame(static, cap, t):
    c = static.copy()
    x = F_FROM + (F_TO - F_FROM) * t
    if x - F_FROM > 0.5:
        rrect(c, F_FROM - 2.5, F_MID, x - F_FROM + 3, 4, 1.5, AMBER)
    c.blit(cap, int(round(x - cap.w / 2.0)), int(round(F_MID + 2 - cap.h / 2.0)))
    return c


# ---------------------------------------------------------------------------------------------
# Toggle (64 x 32) and keys
# ---------------------------------------------------------------------------------------------
def toggle(on):
    w, h = 64, 32
    c = Canvas(w, h)
    rrect(c, 1, 2, w - 2, h - 3, 4.0, with_alpha(INK, 160))
    rrect(c, 1, 1, w - 2, h - 3, 4.0, vgrad(1, h - 2, STEEL, STEEL_D))
    rrect_outline(c, 1, 1, w - 2, h - 3, 4.0, 1.0, with_alpha(INK, 230))
    rrect(c, 6, 7, w - 12, 17, 3.0, BAKE_D)
    lx = 32 if on else 6
    rrect(c, lx, 6, 26, 19, 3.0, vgrad(6, 25, STEEL_L, STEEL_D))
    rrect_outline(c, lx, 6, 26, 19, 3.0, 1.0, with_alpha(INK, 230))
    for k in (-4, 0, 4):
        capsule(c, lx + 13 + k, 10, lx + 13 + k, 21, 1.4, with_alpha(INK, 150))
    lamp_x = 19 if on else 45
    if on:
        circle(c, lamp_x, 16, 8.0, with_alpha(AMBER, 60))
        circle(c, lamp_x, 16, 4.5, AMBER)
        circle(c, lamp_x - 1.2, 14.8, 1.4, (255, 240, 200, 255))
    else:
        circle(c, lamp_x, 16, 4.0, (70, 34, 28, 255))
    return c


def tri(p):
    return 4.0 * p - 1.0 if p < 0.5 else 3.0 - 4.0 * p


def fold(x):
    t = 0.25 * x + 0.25
    return 4.0 * abs(t - math.floor(t + 0.5)) - 1.0


ICONS = {
    "soft": lambda p: math.tanh(2.4 * math.sin(2 * math.pi * p)) / math.tanh(2.4),
    "hard": lambda p: max(-1.0, min(1.0, 2.0 * math.sin(2 * math.pi * p))),
    "fold": lambda p: fold(2.3 * math.sin(2 * math.pi * p)),
    "rect": lambda p: math.tanh(2.4 * abs(math.sin(2 * math.pi * p))) / math.tanh(2.4) * 2.0 - 1.0,
    "sine": lambda p: math.sin(2 * math.pi * p),
    "tri": tri,
    "saw": lambda p: 1.0 - 2.0 * p,
    "square": lambda p: 1.0 if p < 0.5 else -1.0,
}


def _arrow(c, x0, x1, y, col):
    capsule(c, x0, y, x1, y, 2.0, col)
    capsule(c, x1 - 4, y - 3.5, x1, y, 2.0, col)
    capsule(c, x1 - 4, y + 3.5, x1, y, 2.0, col)


def _box(c, x, y, w, h, col):
    rrect_outline(c, x, y, w, h, 2.5, 1.4, with_alpha(col, 200))


def _mini_lowpass(c, x, y, w, h, col):
    # flat passband, small resonant bump, then roll-off
    pts = [(0.0, 0.45), (0.45, 0.45), (0.58, 0.25), (0.72, 0.5), (1.0, 0.95)]
    pts = [(x + px * w, y + py * h) for px, py in pts]
    for a, b in zip(pts, pts[1:]):
        capsule(c, a[0], a[1], b[0], b[1], 1.8, col)


def _mini_clip(c, x, y, w, h, col):
    n = 28
    pts = [(x + w * i / float(n), y + h / 2.0 - (h / 2.0 - 1.0) * ICONS["hard"](i / float(n))) for i in range(n + 1)]
    for a, b in zip(pts, pts[1:]):
        capsule(c, a[0], a[1], b[0], b[1], 1.8, col)


def _icon_off(c, w, h, col):
    cx, cy = w / 2.0, (h - 12) / 2.0 + 3.0
    ring(c, cx, cy, 6.5, 8.5, col)
    capsule(c, cx - 7, cy + 7, cx + 7, cy - 7, 2.2, col)


def _icon_pre(c, w, h, col):
    y0 = 6
    _box(c, 6, y0, 26, 20, col)
    _mini_lowpass(c, 9, y0 + 3, 20, 14, col)
    _arrow(c, 35, 45, y0 + 10, col)
    _box(c, 48, y0, 26, 20, col)
    _mini_clip(c, 51, y0 + 3, 20, 14, col)


def _icon_post(c, w, h, col):
    y0 = 6
    _box(c, 6, y0, 26, 20, col)
    _mini_clip(c, 9, y0 + 3, 20, 14, col)
    _arrow(c, 35, 45, y0 + 10, col)
    _box(c, 48, y0, 26, 20, col)
    _mini_lowpass(c, 51, y0 + 3, 20, 14, col)


CUSTOM_ICONS = {"off": _icon_off, "pre": _icon_pre, "post": _icon_post}


def key(w, h, on, icon=None, ghost=False):
    c = Canvas(w, h)
    rrect(c, 1, 3, w - 2, h - 4, 4.0, with_alpha(INK, 170))
    top, bot = ((78, 68, 48, 255), (42, 36, 26, 255)) if on else ((56, 53, 47, 255), (26, 24, 22, 255))
    rrect(c, 1, 1, w - 2, h - 4, 4.0, vgrad(1, h - 3, top, bot))
    rrect_outline(c, 1, 1, w - 2, h - 4, 4.0, 1.0, with_alpha(AMBER if on else INK, 230))
    capsule(c, 5, 2.5, w - 5, 2.5, 1.0, (255, 255, 255, 38))
    if on:
        rrect(c, 4, h - 11, w - 8, 5, 2.5, with_alpha(AMBER, 70))
        rrect(c, 7, h - 9, w - 14, 2.0, 1.0, AMBER)
    if icon in CUSTOM_ICONS:
        CUSTOM_ICONS[icon](c, w, h, AMBER if on else CREAM)
    elif icon:
        fn = ICONS[icon]
        x0, x1 = 8.0, w - 8.0
        cy = (h - 12) / 2.0 + 3.0
        amp = (h - 22) / 2.0
        if ghost:
            pts = [(x0 + (x1 - x0) * i / 40.0, cy - amp * math.sin(2 * math.pi * i / 40.0)) for i in range(41)]
            for a, b in zip(pts, pts[1:]):
                capsule(c, a[0], a[1], b[0], b[1], 1.0, with_alpha(CREAM_DIM, 110))
        col = AMBER if on else CREAM
        n = 64
        pts = [(x0 + (x1 - x0) * i / float(n), cy - amp * fn(min(i, n - 1e-6) / float(n))) for i in range(n + 1)]
        for a, b in zip(pts, pts[1:]):
            capsule(c, a[0], a[1], b[0], b[1], 2.4, col)
    return c


# ---------------------------------------------------------------------------------------------
# Page backgrounds
# ---------------------------------------------------------------------------------------------

def _icon_sober(c, w, h, col):
    x0, x1, cy, amp = 14.0, w - 14.0, (h - 12) / 2.0 + 3.0, (h - 22) / 2.0 * 0.7
    pts = [(x0 + (x1 - x0) * i / 60.0, cy - amp * math.sin(2 * math.pi * 2.0 * i / 60.0)) for i in range(61)]
    for a, b in zip(pts, pts[1:]):
        capsule(c, a[0], a[1], b[0], b[1], 2.6, col)


def _icon_drunk(c, w, h, col):
    x0, x1, cy, amp = 14.0, w - 14.0, (h - 12) / 2.0 + 3.0, (h - 22) / 2.0
    pts = []
    for i in range(61):
        p = i / 60.0
        y = math.sin(2 * math.pi * 2.0 * p + 1.6 * math.sin(2 * math.pi * 1.3 * p)) * (0.4 + 0.6 * p)
        pts.append((x0 + (x1 - x0) * p, cy - amp * y))
    for a, b in zip(pts, pts[1:]):
        capsule(c, a[0], a[1], b[0], b[1], 2.6, col)


def _icon_crazy(c, w, h, col):
    rnd = random.Random(7)
    x0, x1, cy, amp = 14.0, w - 14.0, (h - 12) / 2.0 + 3.0, (h - 22) / 2.0
    n = 17
    pts = []
    for i in range(n + 1):
        sign = 1.0 if i % 2 == 0 else -1.0
        pts.append((x0 + (x1 - x0) * i / float(n), cy - amp * sign * rnd.uniform(0.35, 1.0)))
    for a, b in zip(pts, pts[1:]):
        capsule(c, a[0], a[1], b[0], b[1], 2.6, col)


def _poly(c, pts, col, w=2.4):
    for a, b in zip(pts, pts[1:]):
        capsule(c, a[0], a[1], b[0], b[1], w, col)


def _vm_geom(w, h):
    return 12.0, w - 12.0, (h - 12) / 2.0 + 3.0, (h - 22) / 2.0


def _icon_vm_manual(c, w, h, col):
    x0, x1, cy, amp = _vm_geom(w, h)
    cx = w / 2.0
    ring(c, cx, cy, 7.5, 9.5, col)
    capsule(c, cx, cy, cx + 5.0, cy - 6.0, 2.2, col)


def _icon_vm_lfo(c, w, h, col):
    x0, x1, cy, amp = _vm_geom(w, h)
    _poly(c, [(x0 + (x1 - x0) * i / 40.0, cy - amp * 0.8 * math.sin(2 * math.pi * i / 40.0)) for i in range(41)], col)


def _icon_vm_env(c, w, h, col):
    x0, x1, cy, amp = _vm_geom(w, h)
    pts = [(0.0, 0.9), (0.12, -0.9), (0.3, -0.4), (0.6, 0.25), (1.0, 0.85)]
    _poly(c, [(x0 + (x1 - x0) * px, cy - amp * py) for px, py in pts], col)


def _icon_vm_step(c, w, h, col):
    x0, x1, cy, amp = _vm_geom(w, h)
    levels = [-0.8, 0.0, 0.8, 0.1]
    pts = []
    for i, lv in enumerate(levels):
        xa = x0 + (x1 - x0) * i / 4.0
        xb = x0 + (x1 - x0) * (i + 1) / 4.0
        pts += [(xa, cy - amp * lv), (xb, cy - amp * lv)]
    _poly(c, pts, col)


def _ellipse(c, cx, cy, rx, ry, col, w=2.0):
    _poly(c, [(cx + rx * math.cos(2 * math.pi * i / 28.0), cy + ry * math.sin(2 * math.pi * i / 28.0)) for i in range(29)], col, w)


def _icon_vc_normal(c, w, h, col):
    x0, x1, cy, amp = _vm_geom(w, h)
    _ellipse(c, w / 2.0, cy, 11.0, 6.5, col, 2.4)


def _icon_vc_choir(c, w, h, col):
    x0, x1, cy, amp = _vm_geom(w, h)
    for dx, dy in ((-10.0, -3.0), (0.0, 3.0), (10.0, -3.0)):
        _ellipse(c, w / 2.0 + dx, cy + dy, 7.0, 4.5, col, 1.8)


def _icon_vc_whisper(c, w, h, col):
    x0, x1, cy, amp = _vm_geom(w, h)
    for i, r in enumerate((3.0, 2.6, 2.2, 1.8, 1.4)):
        circle(c, x0 + 3 + i * (x1 - x0 - 6) / 4.0, cy + (i % 2) * 4 - 2, r, col)


def _icon_vc_robot(c, w, h, col):
    x0, x1, cy, amp = _vm_geom(w, h)
    _poly(c, [(x0, cy + 6), (x0, cy - 6), (x0 + 10, cy - 6), (x0 + 10, cy + 6), (x0 + 20, cy + 6), (x0 + 20, cy - 6),
              (x1, cy - 6)], col)


CUSTOM_ICONS.update({"sober": _icon_sober, "drunk": _icon_drunk, "crazy": _icon_crazy,
                     "vm_manual": _icon_vm_manual, "vm_lfo": _icon_vm_lfo, "vm_env": _icon_vm_env, "vm_step": _icon_vm_step,
                     "vc_normal": _icon_vc_normal, "vc_choir": _icon_vc_choir, "vc_whisper": _icon_vc_whisper,
                     "vc_robot": _icon_vc_robot})


def red_button(pressed):
    """The big red button, 280 x 280: steel bezel, dark well, red dome with a printed legend."""
    W = 280
    c = Canvas(W, W)
    cx = cy = 140.0
    off = 4.0 if pressed else 0.0
    circle(c, cx + 2, cy + 6, 134.0, with_alpha(INK, 150))
    circle(c, cx, cy, 132.0, kit.light_rim(cx, cy, STEEL_L, STEEL_D))
    circle(c, cx, cy, 118.0, vgrad(cy - 118, cy + 118, (18, 18, 16, 255), (44, 42, 38, 255)))
    if pressed:
        ring(c, cx, cy, 108.0, 122.0, with_alpha(AMBER, 120))
    top, bot = ((156, 30, 24, 255), (96, 16, 14, 255)) if pressed else ((238, 84, 64, 255), (150, 28, 22, 255))
    r = 106.0 - off
    circle(c, cx, cy + off, r, vgrad(cy + off - r, cy + off + r, top, bot))
    ring(c, cx, cy + off, r - 5.0, r, with_alpha(mix(top, (255, 255, 255, 255), 0.35), 90))
    if not pressed:
        circle(c, cx - 36, cy - 48, 30.0, (255, 255, 255, 46))
        circle(c, cx - 40, cy - 52, 13.0, (255, 255, 255, 80))
    word, scale = "PUSH", 8
    wd = pixfont.text_width(word, scale)
    col = AMBER if pressed else CREAM

    def plot(px, py, colr):
        if 0 <= px < W and 0 <= py < W:
            c.blend(py * W + px, colr, 1.0)
    pixfont.draw_text(plot, int(cx - wd / 2.0), int(cy - 28 + off), word, scale, with_alpha(col, 235))
    return c


def red_page_extra(b, rnd):
    """Decor and text for the RED BTN page background."""
    cx, cy = 230, 240
    # recess behind the button, with screws and a hazard strip
    b.disc(cx, cy, 178, INK, 0.55)
    b.disc(cx, cy, 174, STEEL_D, 1.0)
    b.disc(cx, cy, 168, BAKE_D, 0.92)
    for a in (45, 135, 225, 315):
        screw(b, cx + 154 * math.cos(math.radians(a)), cy + 154 * math.sin(math.radians(a)), 7, rnd)
    for y in range(428, 452):
        for x in range(60, 400):
            if ((x + y) // 14) % 2 == 0:
                b.blend(x, y, (222, 176, 40, 255), 0.88)
            else:
                b.blend(x, y, (20, 18, 16, 255), 0.88)
    b.frame(60, 428, 400, 452, INK, 0.9, 2)
    b.text(52, 36, "EMERGENCY RANDOMIZER", 3, CREAM, 0.95)
    # right side: how it works
    x, y = 500, 262
    b.text(x, y, "HOW IT WORKS", 2, AMBER, 0.95)
    b.hline(x + 12 * 12 + 10, 1240, y + 6, AMBER, 0.45, 2)
    lines = [
        "PRESS THE BIG RED BUTTON: EVERY SOUND SETTING IS",
        "REPLACED BY A NEW RANDOM PATCH. OUTPUT IS SET TO",
        "A SAFE LEVEL FOR THE CHOSEN SANITY.",
        "",
        "SOBER            MUSICAL RANGES, GENTLE SETTINGS.",
        "DRUNKISH         WIDER RANGES, A LITTLE BROKEN.",
        "BAT SHIT CRAZY   FULL RANGE OF EVERYTHING.",
        "",
        "THE RESULT IS NOT A PRESET: SAVE THE PROJECT TO KEEP IT.",
    ]
    y += 24
    for line in lines:
        if line:
            b.text(x, y, line, 2, CREAM, 0.9)
        y += 19


class Buf:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.d = bytearray(w * h * 4)

    def blend(self, x, y, col, a=1.0):
        if x < 0 or y < 0 or x >= self.w or y >= self.h:
            return
        a *= col[3] / 255.0 if len(col) > 3 else 1.0
        if a <= 0:
            return
        i = (y * self.w + x) * 4
        d = self.d
        if a >= 1.0:
            d[i], d[i + 1], d[i + 2] = col[0], col[1], col[2]
        else:
            d[i] = int(d[i] + (col[0] - d[i]) * a)
            d[i + 1] = int(d[i + 1] + (col[1] - d[i + 1]) * a)
            d[i + 2] = int(d[i + 2] + (col[2] - d[i + 2]) * a)

    def rect(self, x0, y0, x1, y1, col, a=1.0):
        for y in range(max(0, int(y0)), min(self.h, int(y1))):
            for x in range(max(0, int(x0)), min(self.w, int(x1))):
                self.blend(x, y, col, a)

    def hline(self, x0, x1, y, col, a=1.0, th=1):
        for k in range(th):
            for x in range(int(x0), int(x1)):
                self.blend(x, int(y) + k, col, a)

    def vline(self, x, y0, y1, col, a=1.0, th=1):
        for k in range(th):
            for y in range(int(y0), int(y1)):
                self.blend(int(x) + k, y, col, a)

    def frame(self, x0, y0, x1, y1, col, a=1.0, th=1):
        self.hline(x0, x1, y0, col, a, th)
        self.hline(x0, x1, y1 - th, col, a, th)
        self.vline(x0, y0, y1, col, a, th)
        self.vline(x1 - th, y0, y1, col, a, th)

    def line(self, x0, y0, x1, y1, col, a=1.0):
        n = int(max(abs(x1 - x0), abs(y1 - y0)) * 2) + 1
        for i in range(n + 1):
            t = i / float(n)
            self.blend(int(round(x0 + (x1 - x0) * t)), int(round(y0 + (y1 - y0) * t)), col, a)

    def disc(self, cx, cy, r, col, a=1.0):
        for y in range(int(cy - r - 1), int(cy + r + 2)):
            for x in range(int(cx - r - 1), int(cx + r + 2)):
                d = math.hypot(x + 0.5 - cx, y + 0.5 - cy)
                if d <= r:
                    self.blend(x, y, col, a)
                elif d <= r + 1:
                    self.blend(x, y, col, a * (r + 1 - d))

    def text(self, x, y, s, scale, col, a=1.0):
        return pixfont.draw_text(lambda px, py, c: self.blend(px, py, c, a), x, y, s, scale, col)

    def canvas(self, cv, ox, oy):
        for y in range(cv.h):
            for x in range(cv.w):
                r, g, b, al = cv.px[y * cv.w + x]
                if al > 0.0:
                    self.blend(ox + x, oy + y, (int(r), int(g), int(b), 255), al)

    def rows(self):
        return [bytes(self.d[y * self.w * 4:(y + 1) * self.w * 4]) for y in range(self.h)]


def screw(b, cx, cy, r, rnd):
    b.disc(cx + 1.2, cy + 1.4, r + 0.5, (0, 0, 0, 255), 0.45)
    b.disc(cx, cy, r, STEEL_D)
    b.disc(cx - 0.6, cy - 0.7, r - 1.2, STEEL)
    b.disc(cx - 1.4, cy - 1.6, max(1.0, r * 0.35), STEEL_L, 0.7)
    a = rnd.uniform(0, math.pi)
    dx, dy = math.cos(a) * (r - 1.5), math.sin(a) * (r - 1.5)
    b.line(cx - dx, cy - dy, cx + dx, cy + dy, INK, 0.85)


def plate(b, x0, y0, x1, y1, rnd, shade=1.0, screws=True):
    for y in range(int(y0), int(y1)):
        t = (y - y0) / float(max(1, y1 - y0))
        col = mix(STEEL_L, STEEL_D, 0.15 + 0.7 * t)
        col = tuple(int(c * shade) for c in col[:3]) + (255,)
        for x in range(int(x0), int(x1)):
            n = rnd.randint(-5, 5)
            b.blend(x, y, (max(0, min(255, col[0] + n)), max(0, min(255, col[1] + n)), max(0, min(255, col[2] + n)), 255))
    b.frame(int(x0), int(y0), int(x1), int(y1), INK, 0.9, 2)
    b.hline(x0 + 2, x1 - 2, y0 + 2, (255, 255, 255, 255), 0.3)
    if screws:
        for sx, sy in ((x0 + 9, y0 + 9), (x1 - 9, y0 + 9), (x0 + 9, y1 - 9), (x1 - 9, y1 - 9)):
            screw(b, sx, sy, 4.5, rnd)


def make_background(spec, seed):
    rnd = random.Random(seed)
    W, H = 1280, 628
    b = Buf(W, H)
    # enamel base with low-frequency blotches and fine grain
    gw, gh = W // 40 + 2, H // 40 + 2
    grid = [[rnd.uniform(-1, 1) for _ in range(gw)] for _ in range(gh)]
    for y in range(H):
        fy = y / 40.0
        iy = int(fy)
        ty = fy - iy
        base_t = y / float(H)
        row_col = mix(PANEL1, PANEL2, 0.15 + 0.7 * base_t)
        for x in range(W):
            fx = x / 40.0
            ix = int(fx)
            tx = fx - ix
            v = ((grid[iy][ix] * (1 - tx) + grid[iy][ix + 1] * tx) * (1 - ty)
                 + (grid[iy + 1][ix] * (1 - tx) + grid[iy + 1][ix + 1] * tx) * ty)
            n = v * 5.0 + rnd.randint(-4, 4)
            i = (y * W + x) * 4
            b.d[i] = max(0, min(255, int(row_col[0] + n)))
            b.d[i + 1] = max(0, min(255, int(row_col[1] + n)))
            b.d[i + 2] = max(0, min(255, int(row_col[2] + n * 0.9)))
            b.d[i + 3] = 255

    rows = spec["rows"]
    # sunken row panels + legends
    for ri, row in enumerate(rows):
        top = SLOT_Y[ri] - 8
        b.rect(15, top, 1265, top + SLOT_H + 16, BAKE_D, 0.30)
        b.frame(15, top, 1265, top + SLOT_H + 16, INK, 0.55, 2)
        b.hline(17, 1263, top + SLOT_H + 14, (255, 255, 255, 255), 0.10)
        cap = spec["captions"][ri] if ri < len(spec["captions"]) else ""
        cy = top - 22
        if cap:
            end = b.text(34, cy, cap, 2, CREAM, 0.95)
            b.hline(end + 10, 1250, cy + 6, CREAM, 0.5, 2)
            b.vline(1249, cy + 1, cy + 12, CREAM, 0.5, 2)
        # blank cover plates for empty slots
        for ci, ctl in enumerate(row):
            if ctl == "empty":
                sx, sy = SLOT_X[ci], SLOT_Y[ri]
                plate(b, sx + 70, sy + 26, sx + 208, sy + 82, rnd, shade=0.55)
    for (px0, py0, px1, py1) in spec.get("panels", []):
        b.rect(px0, py0, px1, py1, BAKE_D, 0.30)
        b.frame(px0, py0, px1, py1, INK, 0.55, 2)
        b.hline(px0 + 2, px1 - 2, py1 - 2, (255, 255, 255, 255), 0.10)
    n_rows = len(rows)
    if not spec.get("no_hints"):
        # small print hints
        y = SLOT_Y[n_rows] - 22 if n_rows < 3 else 466
        b.text(34, y, "NOTES", 2, AMBER, 0.95)
        b.hline(34 + 6 * 2 * 5 + 10, 1250, y + 6, AMBER, 0.45, 2)
        y += 24
        for line in spec["hints"]:
            b.text(34, y, line, 2, CREAM, 0.88)
            y += 19
    if spec.get("extra"):
        spec["extra"](b, rnd)
    # nameplate (always at the bottom)
    plate(b, 30, 548, 700, 612, rnd)
    b.text(52, 556, "BASS BRUTALIZER", 4, (46, 44, 38, 255), 1.0)
    b.text(53, 557, "BASS BRUTALIZER", 4, (255, 255, 255, 255), 0.18)
    b.text(52, 590, "COMRADE FUZZ  MODEL BB-1  45 PARAMETERS", 2, (46, 44, 38, 255), 1.0)
    b.text(730, 556, spec["page_tag"], 3, CREAM, 0.95)
    b.text(730, 586, "SER NO 0417  TY 25-2026", 2, CREAM_DIM, 0.95)
    # indicator lamps (decor)
    for lx, col, glow in ((1196, RED, 0.35), (1236, AMBER, 0.35)):
        b.disc(lx, 580, 16, INK, 0.7)
        b.disc(lx, 580, 14, STEEL_D)
        b.disc(lx, 580, 11, col, 0.95)
        b.disc(lx, 580, 20, col, 0.10)
        b.disc(lx - 3, 576, 3, (255, 245, 220, 255), 0.6)
    # corner screws
    for sx, sy in ((14, 14), (W - 14, 14), (14, H - 14), (W - 14, H - 14)):
        screw(b, sx, sy, 7, rnd)
    # wear: scratches, chips, edge wear, rust stains near screws
    for k in range(110):
        x, y2 = rnd.uniform(0, W), rnd.uniform(0, H)
        ln = rnd.uniform(8, 60) if k > 6 else rnd.uniform(80, 160)
        ang = rnd.choice((0.0, 0.0, 0.2, -0.2, math.pi / 2, 0.7, -0.7, rnd.uniform(0, math.pi)))
        light = rnd.random() < 0.8
        b.line(x, y2, x + math.cos(ang) * ln, y2 + math.sin(ang) * ln,
               (196, 196, 176, 255) if light else (16, 18, 16, 255), rnd.uniform(0.05, 0.20))
    for _ in range(150):
        # paint chips, more of them near the edges
        edge = rnd.random() < 0.6
        if edge:
            side = rnd.randint(0, 3)
            x = rnd.uniform(0, W) if side < 2 else (rnd.uniform(0, 14) if side == 2 else rnd.uniform(W - 14, W))
            y2 = rnd.uniform(0, 14) if side == 0 else (rnd.uniform(H - 14, H) if side == 1 else rnd.uniform(0, H))
        else:
            x, y2 = rnd.uniform(0, W), rnd.uniform(0, H)
        for _k in range(rnd.randint(2, 7)):
            b.blend(int(x + rnd.randint(-3, 3)), int(y2 + rnd.randint(-2, 2)),
                    (150, 148, 132, 255) if rnd.random() < 0.7 else (110, 60, 34, 255), rnd.uniform(0.4, 0.9))
    for sx, sy in ((14, 14), (W - 14, 14), (14, H - 14), (W - 14, H - 14)):
        for _ in range(26):
            b.disc(sx + rnd.uniform(-14, 14), sy + rnd.uniform(-14, 14), rnd.uniform(1, 4), (128, 74, 38, 255), 0.07)
    b.frame(0, 0, W, H, STEEL_D, 0.9, 3)
    return b


# ---------------------------------------------------------------------------------------------
# Page descriptions: captions per row and small-print hints (uppercase, <= 95 chars per line)
# ---------------------------------------------------------------------------------------------
PAGES = {
    "MAIN": {
        "captions": ["DRIVE & OUTPUT", "CHARACTER", "LOW END  (CLEAN SUB SPLIT)"],
        "hints": [
            "MODE: SOFT = TANH SATURATION   HARD = CLIPPER   FOLD = WAVEFOLDER   RECT = RECTIFIER (OCTAVE UP)",
            "SPLIT: EVERYTHING BELOW THE CUTOFF STAYS CLEAN. SPLIT ON OFF OR 0 HZ = THE WHOLE SIGNAL IS DISTORTED.",
            "OCT SUB ADDS AN OCTAVE-DOWN SUB TONE. SUB SETS THE CLEAN LOW BAND LEVEL. MIX BLENDS DRY AND WET.",
        ],
        "page_tag": "1/7  MAIN",
    },
    "BAND": {
        "captions": ["MULTIBAND  (MIDS AND HIGHS SHAPED SEPARATELY)"],
        "hints": [
            "MULTIBAND ON: HI SPLIT DIVIDES THE BODY INTO MIDS AND HIGHS, EACH WITH ITS OWN SHAPER.",
            "HI DRIVE ADDS OR CUTS DRIVE ON THE HIGHS ONLY. MULTIBAND OFF = ONE SHAPER FOR EVERYTHING.",
        ],
        "page_tag": "2/7  BAND",
    },
    "CRUSH": {
        "captions": ["BITCRUSH & DECIMATE", "RING MOD"],
        "hints": [
            "CRUSH ON / RING ON SWITCH EACH SECTION. BITS 16 AND DECIMATE 1X DO NOTHING EVEN WHEN ON.",
            "FEWER BITS = HARSHER QUANTISING. HIGHER DECIMATE = MORE ALIASING. RING HZ = CARRIER (METALLIC).",
        ],
        "page_tag": "3/7  CRUSH",
    },
    "WOBBLE": {
        "captions": ["WOBBLE  (ON / POSITION / FILTER)", "LFO"],
        "hints": [
            "WOBBLE ON SWITCHES THE SECTION. POS: PRE = BEFORE DISTORTION (GROWL)  POST = AFTER CRUSH (WOBBLE).",
            "WOB SYNC: FREE = RATE IN HZ, OTHERWISE LOCKED TO THE MPC TEMPO. DEPTH 100% = PLUS MINUS 4 OCTAVES.",
        ],
        "page_tag": "4/7  WOBBLE",
    },
    "VOWEL": {
        "captions": ["VOWEL STAGE  (FORMANT FILTERS)", "MOTION", "WORDS & TIMING"],
        "hints": [
            "VOWELS NEED HARMONICS: USE POS = POST (AFTER THE DRIVE). PRE FILTERS THE BODY BEFORE IT IS DISTORTED.",
            "MODE: MANUAL = MORPH KNOB  LFO = SWEEPS A TO B  ENV = LOUDER OPENS THE MOUTH  STEP = A WORD.",
            "STEP PLAYS 4 VOWELS PER LOOP: WOW YAY YO YEAH YOI WAH OOH-AAH EOW MAMA. SYNC SETS THE STEP SPEED.",
        ],
        "page_tag": "5/7  VOWEL",
    },
    "VOICE": {
        "captions": ["VOICE"],
        "hints": [
            "GENDER SHIFTS EVERY FORMANT: UP = SMALL AND CARTOONISH, DOWN = GIANT. Q = HOW SHARP THE VOWEL IS.",
            "CHAR: NORMAL  CHOIR (THREE DETUNED SINGERS)  WHISPER (NOISE VOICE)  ROBOT (HARD VOWEL SWITCHING).",
            "VOW MIX BLENDS THE VOWEL SOUND WITH THE ORIGINAL SIGNAL. 0% = NO EFFECT.",
        ],
        "page_tag": "6/7  VOICE",
    },
    "RED BTN": {
        "captions": [],
        "hints": [],
        "no_hints": True,
        "panels": [(30, 30, 462, 470), (482, 30, 1262, 470)],
        "extra": red_page_extra,
        "red": True,
        "page_tag": "7/7  RED BTN",
    },
}


# ---------------------------------------------------------------------------------------------
# Skin assembly
# ---------------------------------------------------------------------------------------------
def walk(node, fn):
    if isinstance(node, dict):
        fn(node)
        for v in node.values():
            walk(v, fn)
    elif isinstance(node, list):
        for v in node:
            walk(v, fn)


def restyle_text(root):
    def fix(d):
        if "textStyle" in d and d.get("type") in ("Name", "Value", "Text"):
            ts = d["textStyle"]
            if d["type"] == "Name":
                ts["colour"] = TXT_NAME
            elif d["type"] == "Value":
                ts["colour"] = TXT_VALUE
            else:
                ts["colour"] = TXT_CAPTION
    walk(root, fix)


def build(preview):
    ui = json.load(open(os.path.join(PLUGIN, "ui.json")))
    params = os.path.join(PLUGIN, "build", "params.json")
    if not os.path.isfile(params):
        sys.exit("build/params.json missing: run  python3 tools/build.py \"plugins/Bass Brutalizer\"  first")
    tmp = tempfile.mkdtemp(prefix="bb_skin_")
    try:
        subprocess.check_call([sys.executable, os.path.join(SDK, "tools", "gen_ui.py"), "--params", params,
                               "--ui", os.path.join(PLUGIN, "ui.json"), "--out", tmp,
                               "--kit-cache", os.path.join(PLUGIN, "build")], stdout=subprocess.DEVNULL)
        tui = json.load(open(os.path.join(tmp, "TUI.json")))
        shutil.copy(os.path.join(tmp, "Q-Links.json"), os.path.join(tmp, "Q-Links.keep.json"))
        qlinks = open(os.path.join(tmp, "Q-Links.json")).read()
    finally:
        pass
    pd = tui["pageData"]
    defs = {e["key"]: e["value"] for e in pd["componentDefinitions"]["localComponentDefinitions"]}

    # --- extra radio4 definitions with icon keys (Mode, Wob Shape) ---
    def icon_def(prefix, base="radio4", kw=58, pitch=62, ky=46, kh=42, size=None):
        d = copy.deepcopy(defs[base])
        for ent in d["componentsData"]:
            cd = ent["componentData"]
            if cd["type"] == "Button" and cd["name"].startswith("seg "):
                k = int(cd["name"].split()[1])
                cd["data"]["onImage"] = "%s_%d_on.png" % (prefix, k)
                cd["data"]["offImage"] = "%s_%d_off.png" % (prefix, k)
                ent["bounds"]["bounds"] = "%d %d %d %d" % (15 + pitch * k, ky, kw, kh)
            elif size and cd["type"] == "Focus":
                ent["bounds"]["bounds"] = "0 0 %d %d" % size
        return d
    pd["componentDefinitions"]["localComponentDefinitions"].append({"key": "radio4Mode", "value": icon_def("mode")})
    pd["componentDefinitions"]["localComponentDefinitions"].append({"key": "radio4Wave", "value": icon_def("wave")})
    pd["componentDefinitions"]["localComponentDefinitions"].append(
        {"key": "radio3Pos", "value": icon_def("pos", "radio3", 80, 86)})
    pd["componentDefinitions"]["localComponentDefinitions"].append(
        {"key": "radio3Sanity", "value": icon_def("sanity", "radio3", 128, 142, 50, 72, (440, 180))})
    # the big red button: focus frame plus one Button; momentary (see ResetOnGestureEnd in the remap below)
    big = copy.deepcopy(defs["toggle"])
    big["actions"] = [a for a in big["actions"] if a["onAction"] == "Mouse Down"]
    keep = []
    for ent in big["componentsData"]:
        cd = ent["componentData"]
        if cd["type"] == "Focus":
            ent["bounds"]["bounds"] = "0 0 360 360"
            keep.append(ent)
        elif cd["type"] == "Button":
            cd["name"] = "red button"
            cd["data"]["onImage"] = "red_on.png"
            cd["data"]["offImage"] = "red_off.png"
            ent["bounds"]["bounds"] = "40 40 280 280"
            keep.append(ent)
    big["componentsData"] = keep
    pd["componentDefinitions"]["localComponentDefinitions"].append({"key": "bigRed", "value": big})
    pd["componentDefinitions"]["localComponentDefinitions"].append({"key": "radio4VMode", "value": icon_def("vmode")})
    pd["componentDefinitions"]["localComponentDefinitions"].append({"key": "radio4VChar", "value": icon_def("vchar")})
    # control name -> (new type, base type, key count, key width, key pitch, caption y offset)
    icon_for = {"Mode": ("radio4Mode", "radio4", 4, 58, 62, 90), "Wob Shape": ("radio4Wave", "radio4", 4, 58, 62, 90),
                "Wob Pos": ("radio3Pos", "radio3", 3, 80, 86, 90), "Vow Pos": ("radio3Pos", "radio3", 3, 80, 86, 90),
                "Vow Mode": ("radio4VMode", "radio4", 4, 58, 62, 90), "Vow Char": ("radio4VChar", "radio4", 4, 58, 62, 90),
                "Sanity": ("radio3Sanity", "radio3", 3, 128, 142, 128)}

    # --- pages ---
    tab_specs = []
    for tab, pspec in zip(pd["tabs"], ui["pages"]):
        name = tab["tabName"]
        info = PAGES[name]
        controls = pspec["controls"]
        rows = [controls[i:i + 4] for i in range(0, len(controls), 4)]
        rows = [[("empty" if c == "empty" else "ctl") for c in r] + ["empty"] * (4 - len(r)) for r in rows]
        spec = dict(info, rows=([] if info.get("red") else rows))
        tab_specs.append((name, spec))
        bgname = "bg_%s.png" % name.lower().replace(" ", "_")
        cdf = tab["componentDefinition"]
        for k in ("focussed", "unfocussed"):
            cdf["backgroundData"][k]["colour"] = BG_COLOUR
            cdf["backgroundData"][k]["image"] = bgname
        comps = cdf["componentsData"]
        by_name = {c["componentData"]["name"]: c for c in comps}
        for c in comps:
            cd = c["componentData"]
            if name == "RED BTN" and cd["name"] == "Red Button":
                cd["type"] = "bigRed"
                c["bounds"]["bounds"] = "50 60 360 360"
                for m in c["handle remapping"]["map"]:
                    if m["key"] == "Data":
                        m["value"] = "ResetOnGestureEnd/" + m["value"]
            if name == "RED BTN" and cd["name"] == "Sanity":
                c["bounds"]["bounds"] = "500 70 440 180"
            if cd["name"] in icon_for and cd["type"] == icon_for[cd["name"]][1]:
                new_type, _base, nkeys, kw, pitch, cap_y = icon_for[cd["name"]]
                cd["type"] = new_type
                x, y, _, _ = [int(float(v)) for v in c["bounds"]["bounds"].split()]
                for k in range(nkeys):
                    lab = by_name.get("%s seg %d" % (cd["name"], k))
                    if lab:
                        lab["bounds"]["bounds"] = "%d %d %d 16" % (x + 15 + pitch * k, y + cap_y, kw)
                        lab["componentData"]["data"]["textStyle"]["font"]["height"] = 15.0
        if name == "RED BTN":
            tab["qlinkBoundsData"] = ["40 50 910 380"]
    restyle_text(pd)

    # --- write the skins folder ---
    out = os.path.join(PLUGIN, "skins")
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)
    json.dump(tui, open(os.path.join(out, "TUI.json"), "w"), indent=2)
    open(os.path.join(out, "Q-Links.json"), "w").write(qlinks)

    def save(name, canvas_or_rows, width=None):
        if isinstance(canvas_or_rows, Canvas):
            kit.write_png(os.path.join(out, name), canvas_or_rows.w, canvas_or_rows.rows())
        else:
            kit.write_png(os.path.join(out, name), width, canvas_or_rows)

    print("rendering knobs ...")
    imgs = {}
    for bip, fname in ((False, "knob_uni.png"), (True, "knob_bi.png")):
        st = knob_static(bip, 11 if not bip else 12)
        rows = []
        for i in range(FRAMES):
            fr = knob_frame(st, i / (FRAMES - 1.0), bip)
            rows.extend(fr.rows())
            if preview and i == 50:
                imgs[fname] = fr
        kit.write_png(os.path.join(out, fname), 90, rows)
    print("rendering faders ...")
    st, cap = fader_static(), fader_cap()
    rows = []
    for i in range(FRAMES):
        fr = fader_frame(st, cap, i / (FRAMES - 1.0))
        rows.extend(fr.rows())
        if preview and i == 90:
            imgs["fader_h_uni.png"] = fr
    kit.write_png(os.path.join(out, "fader_h_uni.png"), FW, rows)
    print("rendering switches and keys ...")
    for on in (True, False):
        s = "on" if on else "off"
        c = toggle(on); save("toggle_%s.png" % s, c); imgs["toggle_%s.png" % s] = c
        c = key(58, 40, on); save("seg4_%s.png" % s, c); imgs["seg4_%s.png" % s] = c
        c = key(80, 40, on); save("seg3_%s.png" % s, c); imgs["seg3_%s.png" % s] = c
        for k, ic in enumerate(("soft", "hard", "fold", "rect")):
            c = key(58, 42, on, ic, ghost=True); save("mode_%d_%s.png" % (k, s), c); imgs["mode_%d_%s.png" % (k, s)] = c
        for k, ic in enumerate(("off", "pre", "post")):
            c = key(80, 42, on, ic); save("pos_%d_%s.png" % (k, s), c); imgs["pos_%d_%s.png" % (k, s)] = c
        c = red_button(on); save("red_%s.png" % s, c); imgs["red_%s.png" % s] = c
        for k, ic in enumerate(("sober", "drunk", "crazy")):
            c = key(128, 72, on, ic); save("sanity_%d_%s.png" % (k, s), c); imgs["sanity_%d_%s.png" % (k, s)] = c
        for k, ic in enumerate(("vm_manual", "vm_lfo", "vm_env", "vm_step")):
            c = key(58, 42, on, ic); save("vmode_%d_%s.png" % (k, s), c); imgs["vmode_%d_%s.png" % (k, s)] = c
        for k, ic in enumerate(("vc_normal", "vc_choir", "vc_whisper", "vc_robot")):
            c = key(58, 42, on, ic); save("vchar_%d_%s.png" % (k, s), c); imgs["vchar_%d_%s.png" % (k, s)] = c
        for k, ic in enumerate(("sine", "tri", "saw", "square")):
            c = key(58, 42, on, ic); save("wave_%d_%s.png" % (k, s), c); imgs["wave_%d_%s.png" % (k, s)] = c
    print("rendering backgrounds ...")
    bgs = {}
    for i, (name, spec) in enumerate(tab_specs):
        b = make_background(spec, 100 + i)
        kit.write_png(os.path.join(out, "bg_%s.png" % name.lower().replace(" ", "_")), b.w, b.rows())
        bgs[name] = b
    if preview:
        previews(tui, tab_specs, bgs, imgs)
    shutil.rmtree(tmp, ignore_errors=True)
    print("skin written to", out)


SAMPLE = {"Drive": "18.0 DB", "Tone": "6000 HZ", "Mix": "100 %", "Output": "0.0 DB", "Bias": "0 %", "Pre Tilt": "0.0 DB",
          "Split On": "ON", "Split": "100 HZ", "Sub": "100 %", "Oct Sub": "0 %", "Hi Split": "3000 HZ", "Hi Drive": "0.0 DB", "Vowel On": "ON", "Morph": "0 %", "Vow Depth": "70 %", "Vow Glide": "25 MS", "Vow Word": "WOW", "Vow Sync": "1/8", "Vow Rate": "2.00 HZ", "Vowel A": "OO", "Vowel B": "AH", "Gender": "0.0 ST", "Vow Q": "60 %", "Vow Mix": "100 %", "Sanity": "DRUNKISH", "Crush On": "ON", "Ring On": "ON", "Wobble On": "ON",
          "Multiband": "OFF", "Bits": "OFF", "Decimate": "OFF", "Ring": "0 %", "Ring Hz": "150 HZ", "Wob Rate": "2.00 HZ",
          "Wob Depth": "50 %", "Wob Freq": "600 HZ", "Wob Reso": "40 %"}


def previews(tui, tab_specs, bgs, imgs):
    """Compose approximate page mock-ups (pixel-font labels instead of the MPC's own text)."""
    pd = tui["pageData"]
    defs = {e["key"]: e["value"] for e in pd["componentDefinitions"]["localComponentDefinitions"]}
    outdir = os.path.join(PLUGIN, "build")
    for tab, (name, _spec) in zip(pd["tabs"], tab_specs):
        b = Buf(1280, 628)
        b.d[:] = bgs[name].d
        for c in tab["componentDefinition"]["componentsData"]:
            cd = c["componentData"]
            x, y, w, h = [int(float(v)) for v in c["bounds"]["bounds"].split()]
            if cd["type"] == "Label":
                data = cd["data"]
                col = (244, 178, 64, 255) if data["type"] == "Value" else CREAM
                b.text(x, y + 2, data.get("text", "").upper(), 2, col)
                continue
            d = defs.get(cd["type"])
            if not d:
                continue
            for ent in d["componentsData"]:
                e = ent["componentData"]
                ex, ey, ew, eh = [int(float(v)) for v in ent["bounds"]["bounds"].split()]
                if e["type"] == "Knob":
                    fn = e["data"]["filmStrip"]
                    if fn in imgs:
                        b.canvas(imgs[fn], x + ex, y + ey)
                elif e["type"] == "Button":
                    on = e["data"]["buttonId"] == 0 or e["data"]["numButtonsInGroup"] == 1
                    if e["data"]["onImage"].startswith("red_"):
                        on = False
                    img = imgs.get(e["data"]["onImage" if on else "offImage"])
                    if img:
                        b.canvas(img, x + ex, y + ey)
                elif e["type"] == "Label":
                    t = e["data"]["type"]
                    if t == "Name":
                        b.text(x + ex, y + ey + 4, cd["name"].upper(), 3, (200, 189, 146, 255))
                    elif t == "Value":
                        b.text(x + ex, y + ey + 4, SAMPLE.get(cd["name"], "--"), 3, (244, 178, 64, 255))
        path = os.path.join(outdir, "preview_%s.png" % name.lower().replace(" ", "_"))
        kit.write_png(path, b.w, b.rows())
        print("preview:", path)


if __name__ == "__main__":
    build("--preview" in sys.argv)
