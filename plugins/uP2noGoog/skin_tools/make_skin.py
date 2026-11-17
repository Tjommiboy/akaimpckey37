#!/usr/bin/env python3
"""Build uP2noGoog's skin into plugins/uP2noGoog/skins/.

The detail pages (OSC, MIXER, FILTER, AMP, MOD) come from the SDK generator (ui.json -> gen_ui.py)
and are restyled here: panel background, black-and-silver knobs, rocker switches, warm focus colour.
This script then puts a PANEL page in front: the whole synth on one screen, laid out left to right
like a 1970s Model D panel (CONTROLLERS | OSCILLATOR BANK | MIXER | MODIFIERS | OUTPUT).

Run after a native build (it needs build/params.json):

    python3 tools/build.py plugins/uP2noGoog --target native
    python3 plugins/uP2noGoog/skin_tools/make_skin.py

build.py then copies skins/ verbatim into the plugin folder on every build. Re-run this script
whenever parameters or ui.json change.
"""
import json, math, os, random, shutil, subprocess, sys, tempfile
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
SDK = os.path.dirname(os.path.dirname(PROJECT))
OUT = os.path.join(PROJECT, "skins")

FONT_DIR = "/usr/share/fonts/truetype/dejavu/"
FONT_LABEL = FONT_DIR + "DejaVuSansCondensed-Bold.ttf"
FONT_PLAIN = FONT_DIR + "DejaVuSansCondensed.ttf"
FONT_LOGO = "/usr/share/fonts/truetype/google-fonts/Poppins-Bold.ttf"

# Colours (R, G, B)
PANEL = (21, 21, 24)
INK = (232, 228, 218)          # printed panel text and lines
INK_DIM = (150, 146, 138)
LIT = (240, 150, 40)           # rocker lamp / focus colour
FOCUS_ARGB = "ffe8963a"
SWEEP = 150.0                  # knob pointer travels -150 .. +150 degrees from straight up
N_FRAMES = 127

# Parameter indices (must match ParamIndex in src/UP2noGoog.cpp)
P = dict(volume=0, tune=1, glide=2, glide_on=3, mod_mix=4, osc_mod=5, osc3_ctrl=6, filt_mod=7, decay_sw=8,
         osc1_range=9, osc1_wave=10, osc2_range=11, osc2_freq=12, osc2_wave=13, osc3_range=14, osc3_freq=15,
         osc3_wave=16, osc1_on=17, osc1_vol=18, osc2_on=19, osc2_vol=20, osc3_on=21, osc3_vol=22, fb_on=23,
         fb_vol=24, noise_on=25, noise_vol=26, noise_col=27, cutoff=28, emphasis=29, contour=30, kbd1=31,
         kbd2=32, f_attack=33, f_decay=34, f_sustain=35, l_attack=36, l_decay=37, l_sustain=38, drive=39,
         drift=40, bend_range=41, velo_loud=42, velo_filt=43, at_mod=44, at_filt=45, a440=46, mod_amt=47)


def font(path, size):
    try:
        return ImageFont.truetype(path, size)
    except OSError:
        return ImageFont.load_default()


# ====================================================================== drawing helpers
def brushed(w, h, base=PANEL, seed=7):
    """Dark panel with a faint horizontal brushed grain."""
    rnd = random.Random(seed)
    im = Image.new("RGB", (w, h), base)
    px = im.load()
    for y in range(h):
        row = rnd.uniform(-3, 3)
        for x in range(0, w):
            v = row + rnd.uniform(-2.5, 2.5)
            r, g, b = base
            px[x, y] = (int(r + v), int(g + v), int(b + v + 1))
    return im.filter(ImageFilter.BoxBlur(0.6)).convert("RGBA")


def wood(w, h, seed=3):
    """Walnut-ish end cheek."""
    rnd = random.Random(seed)
    im = Image.new("RGB", (w, h), (92, 52, 30))
    g = ImageDraw.Draw(im)
    for i in range(60):
        x = rnd.uniform(0, w)
        c = rnd.choice([(70, 38, 22), (110, 66, 38), (84, 46, 26), (120, 74, 44)])
        pts = []
        for y in range(0, h + 20, 20):
            x += rnd.uniform(-0.8, 0.8)
            pts.append((x, y))
        g.line(pts, fill=c, width=rnd.choice([1, 1, 2]))
    im = im.filter(ImageFilter.GaussianBlur(0.7))
    sh = Image.new("L", (w, h), 0)
    sg = ImageDraw.Draw(sh)
    for x in range(w):                         # rounded edge shading
        t = x / max(1, w - 1)
        a = int(90 * (1 - math.sin(math.pi * t)))
        sg.line([(x, 0), (x, h)], fill=a)
    dark = Image.new("RGB", (w, h), (20, 10, 5))
    im = Image.composite(dark, im, sh)
    return im.convert("RGBA")


def knob_frame(size, angle_deg, ss=4):
    """One frame of the knob: silver knurled skirt, black cap, white pointer. Supersampled."""
    S = size * ss
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    g = ImageDraw.Draw(im)
    c = S / 2
    r_out = S * 0.47
    # drop shadow
    sh = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(sh).ellipse([c - r_out + ss * 2, c - r_out + ss * 3, c + r_out + ss * 2, c + r_out + ss * 3], fill=(0, 0, 0, 150))
    sh = sh.filter(ImageFilter.GaussianBlur(ss * 2))
    im.alpha_composite(sh)
    g = ImageDraw.Draw(im)
    # knurled black skirt with ridges
    g.ellipse([c - r_out, c - r_out, c + r_out, c + r_out], fill=(16, 16, 18, 255))
    n_ridges = 36
    for i in range(n_ridges):
        a = math.radians(i * 360 / n_ridges + angle_deg)
        x0, y0 = c + math.sin(a) * r_out * 0.80, c - math.cos(a) * r_out * 0.80
        x1, y1 = c + math.sin(a) * r_out * 0.985, c - math.cos(a) * r_out * 0.985
        g.line([(x0, y0), (x1, y1)], fill=(52, 52, 56, 255), width=max(1, ss))
    # silver cap (radial-ish gradient)
    r_cap = r_out * 0.70
    for k in range(40, 0, -1):
        t = k / 40
        v = int(120 + 110 * (1 - t) ** 0.7)
        rr = r_cap * t
        off = (1 - t) * r_cap * 0.25
        g.ellipse([c - rr - off, c - rr - off, c + rr - off, c + rr - off], fill=(v, v, v + 4, 255))
    g.ellipse([c - r_cap, c - r_cap, c + r_cap, c + r_cap], outline=(40, 40, 44, 255), width=ss)
    # black centre with pointer
    r_in = r_cap * 0.62
    g.ellipse([c - r_in, c - r_in, c + r_in, c + r_in], fill=(22, 22, 25, 255))
    a = math.radians(angle_deg)
    px0, py0 = c + math.sin(a) * r_in * 0.15, c - math.cos(a) * r_in * 0.15
    px1, py1 = c + math.sin(a) * r_out * 0.93, c - math.cos(a) * r_out * 0.93
    g.line([(px0, py0), (px1, py1)], fill=(245, 243, 236, 255), width=int(ss * 2.6))
    return im.resize((size, size), Image.LANCZOS)


def knob_strip(size, frames):
    strip = Image.new("RGBA", (size, size * frames), (0, 0, 0, 0))
    for i in range(frames):
        t = i / (frames - 1)
        strip.paste(knob_frame(size, -SWEEP + 2 * SWEEP * t), (0, i * size))
    return strip


def rocker(w, h, on, ss=4):
    """Rocker switch seen from the front: the pressed half is lower, the lamp stripe glows when on."""
    W, H = w * ss, h * ss
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    g = ImageDraw.Draw(im)
    g.rounded_rectangle([0, 0, W - 1, H - 1], radius=4 * ss, fill=(10, 10, 12, 255))           # bezel
    inset = 3 * ss
    body = [inset, inset, W - 1 - inset, H - 1 - inset]
    g.rounded_rectangle(body, radius=3 * ss, fill=(46, 46, 50, 255))
    mid = H / 2
    top = (inset, inset, W - 1 - inset, mid)
    bot = (inset, mid, W - 1 - inset, H - 1 - inset)
    hi, lo = (88, 88, 94, 255), (30, 30, 33, 255)
    if on:                                     # top half pressed in: top dark, bottom raised
        g.rounded_rectangle(top, radius=3 * ss, fill=lo)
        g.rounded_rectangle(bot, radius=3 * ss, fill=hi)
    else:
        g.rounded_rectangle(top, radius=3 * ss, fill=hi)
        g.rounded_rectangle(bot, radius=3 * ss, fill=lo)
    lamp = (W * 0.22, mid - 2.2 * ss, W * 0.78, mid + 2.2 * ss)
    if on:
        glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        ImageDraw.Draw(glow).rounded_rectangle([lamp[0] - 4 * ss, lamp[1] - 4 * ss, lamp[2] + 4 * ss, lamp[3] + 4 * ss],
                                               radius=4 * ss, fill=LIT + (150,))
        im.alpha_composite(glow.filter(ImageFilter.GaussianBlur(3 * ss)))
        g = ImageDraw.Draw(im)
        g.rounded_rectangle(lamp, radius=2 * ss, fill=(255, 196, 110, 255))
    else:
        g.rounded_rectangle(lamp, radius=2 * ss, fill=(70, 46, 22, 255))
    return im.resize((w, h), Image.LANCZOS)


def segment(w, h, on):
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    g = ImageDraw.Draw(im)
    g.rounded_rectangle([0, 0, w - 1, h - 1], radius=6, fill=(46, 46, 50, 255) if not on else (70, 48, 22, 255),
                        outline=LIT + (255,) if on else (80, 80, 86, 255), width=2)
    if on:
        g.rounded_rectangle([w * 0.3, h - 8, w * 0.7, h - 5], radius=2, fill=LIT + (255,))
    return im


def fader_strip(w, h, frames):
    strip = Image.new("RGBA", (w, h * frames), (0, 0, 0, 0))
    for i in range(frames):
        t = i / (frames - 1)
        im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        g = ImageDraw.Draw(im)
        ty = h // 2
        g.rounded_rectangle([6, ty - 3, w - 7, ty + 3], radius=3, fill=(10, 10, 12, 255))
        tx = 6 + 12 + t * (w - 12 - 36)
        g.rounded_rectangle([6, ty - 2, tx, ty + 2], radius=2, fill=LIT + (255,))
        g.rounded_rectangle([tx - 12, 3, tx + 12, h - 4], radius=4, fill=(176, 176, 182, 255), outline=(40, 40, 44, 255))
        g.line([(tx, 7), (tx, h - 8)], fill=(30, 30, 34, 255), width=2)
        strip.paste(im, (0, i * h))
    return strip


WAVE_SHAPES = {
    "Tri":    [(0, .5), (.25, 0), (.75, 1), (1, .5)],
    "TriSaw": [(0, 1), (.38, .12), (.5, 1), (.88, .12), (1, .5)],
    "Saw":    [(0, 1), (.5, 0), (.5, 1), (1, 0), (1, 1)],
    "RevSaw": [(0, 0), (.5, 1), (.5, 0), (1, 1), (1, 0)],
    "Square": [(0, 1), (0, 0), (.5, 0), (.5, 1), (1, 1), (1, 0)],
    "Wide":   [(0, 1), (0, 0), (.3, 0), (.3, 1), (1, 1), (1, 0)],
    "Narrow": [(0, 1), (0, 0), (.12, 0), (.12, 1), (1, 1), (1, 0)],
}


def wave_icon(g, cx, cy, name, w=18, h=10):
    pts = [(cx - w / 2 + x * w, cy - h / 2 + y * h) for x, y in WAVE_SHAPES[name]]
    g.line(pts, fill=INK, width=2, joint="curve")


def text(g, xy, s, size=13, fill=INK, anchor="mm", f=FONT_LABEL):
    g.text(xy, s, fill=fill, font=font(f, size), anchor=anchor)


# ====================================================================== PANEL layout
# Each control: (kind, param, cx, cy, label). Kinds: knob, knobbi, rotary (choice), rocker (bool).
KNOB = 60        # knob diameter on the panel
ROT = 60
RK_W, RK_H = 30, 40

SECTIONS = [  # (title, x0, x1)
    ("CONTROLLERS", 24, 196),
    ("OSCILLATOR BANK", 196, 600),
    ("MIXER", 600, 832),
    ("MODIFIERS", 832, 1134),
    ("OUTPUT", 1134, 1256),
]

OSC_ROWS = [150, 300, 450]
COL_RANGE, COL_FREQ, COL_WAVE = 284, 406, 530
MIX_ROWS = [("OSC 1", "osc1"), ("FEEDBK", "fb"), ("OSC 2", "osc2"), ("NOISE", "noise"), ("OSC 3", "osc3")]
MIX_Y0, MIX_DY = 104, 92
MOD_COLS = [870, 940, 1010, 1088]
ROW_A, ROW_B, ROW_C = 128, 290, 452

CONTROLS = [
    # CONTROLLERS
    ("knobbi", "tune", 82, 120, "TUNE"),
    ("knob", "glide", 82, 240, "GLIDE"),
    ("knob", "mod_mix", 82, 360, "MOD. MIX"),
    ("knob", "mod_amt", 82, 480, "MOD. AMOUNT"),
    ("rocker", "glide_on", 160, 240, "ON"),
    # OSCILLATOR BANK
    ("rotary", "osc1_range", COL_RANGE, OSC_ROWS[0], None),
    ("rotary", "osc1_wave", COL_WAVE, OSC_ROWS[0], None),
    ("rocker", "osc_mod", COL_FREQ, OSC_ROWS[0] + 6, "OSC. MOD"),
    ("rotary", "osc2_range", COL_RANGE, OSC_ROWS[1], None),
    ("knobbi", "osc2_freq", COL_FREQ, OSC_ROWS[1], None),
    ("rotary", "osc2_wave", COL_WAVE, OSC_ROWS[1], None),
    ("rotary", "osc3_range", COL_RANGE, OSC_ROWS[2], None),
    ("knobbi", "osc3_freq", COL_FREQ, OSC_ROWS[2], None),
    ("rotary", "osc3_wave", COL_WAVE, OSC_ROWS[2], None),
    ("rocker", "osc3_ctrl", COL_FREQ, 552, "OSC. 3 CONTROL"),
    # MIXER
] + [c for i, (lab, key) in enumerate(MIX_ROWS) for c in (
    ("knob", key + "_vol", 652, MIX_Y0 + i * MIX_DY, None),
    ("rocker", key + "_on", 728, MIX_Y0 + i * MIX_DY, None))] + [
    ("rocker", "noise_col", 728, 556, None),
    # MODIFIERS
    ("knob", "cutoff", MOD_COLS[0], ROW_A, "CUTOFF"),
    ("knob", "emphasis", MOD_COLS[1], ROW_A, "EMPHASIS"),
    ("knob", "contour", MOD_COLS[2], ROW_A, "CONTOUR"),
    ("rocker", "kbd1", MOD_COLS[3] - 18, ROW_A + 4, None),
    ("rocker", "kbd2", MOD_COLS[3] + 18, ROW_A + 4, None),
    ("knob", "f_attack", MOD_COLS[0], ROW_B, "ATTACK"),
    ("knob", "f_decay", MOD_COLS[1], ROW_B, "DECAY"),
    ("knob", "f_sustain", MOD_COLS[2], ROW_B, "SUSTAIN"),
    ("rocker", "filt_mod", MOD_COLS[3], ROW_B + 4, "FILTER MOD"),
    ("knob", "l_attack", MOD_COLS[0], ROW_C, "ATTACK"),
    ("knob", "l_decay", MOD_COLS[1], ROW_C, "DECAY"),
    ("knob", "l_sustain", MOD_COLS[2], ROW_C, "SUSTAIN"),
    ("rocker", "decay_sw", MOD_COLS[3], ROW_C + 4, "DECAY"),
    # OUTPUT
    ("knob", "volume", 1195, 128, "VOLUME"),
    ("knob", "drive", 1195, 290, "DRIVE"),
    ("rocker", "a440", 1195, 452, "A-440"),
]

# Q-Links on the PANEL page: four blocks of four. The Key 37 (4 knobs) always banks by COLUMNS of
# the 4 x 4 Q-Link grid, knob 1..4 = Q-Link 13, 9, 5, 1 of the column (tested on hardware), so each
# group goes into one grid column and its rectangle (qlinkBoundsData, same order) encloses it on screen.
QL_GROUPS = [
    ("cutoff", "emphasis", "contour", "kbd1"),               # FILTER row
    ("f_attack", "f_decay", "f_sustain", "filt_mod"),        # FILTER CONTOUR row
    ("l_attack", "l_decay", "l_sustain", "decay_sw"),        # LOUDNESS CONTOUR row
    ("tune", "glide", "mod_mix", "mod_amt"),                 # CONTROLLERS column
]
QL_RECTS = ["840 66 286 116", "840 228 286 116", "840 390 286 116", "38 72 90 476"]


def panel_background(choices):
    bg = brushed(1280, 628)
    bg.paste(wood(24, 628, 3), (0, 0))
    bg.paste(wood(24, 628, 11), (1256, 0))
    g = ImageDraw.Draw(bg)
    # section titles and dividing lines
    for i, (title, x0, x1) in enumerate(SECTIONS):
        text(g, ((x0 + x1) / 2, 22), title, 15)
        if i > 0:
            g.line([(x0, 8), (x0, 592)], fill=INK, width=2)
    g.line([(24, 40), (1256, 40)], fill=INK_DIM, width=1)
    g.line([(24, 594), (1256, 594)], fill=INK_DIM, width=1)
    # footer: brand
    text(g, (640, 611), "uP2noGoog", 20, INK, f=FONT_LOGO)
    text(g, (1240, 611), "uP2noGood", 12, INK_DIM, anchor="rm", f=FONT_LOGO)
    text(g, (40, 611), "MONOPHONIC  ·  THREE OSCILLATORS  ·  LADDER FILTER", 11, INK_DIM, anchor="lm", f=FONT_PLAIN)

    # oscillator bank column heads and row numbers
    for x, lab in [(COL_RANGE, "RANGE"), (COL_FREQ, "FREQUENCY"), (COL_WAVE, "WAVEFORM")]:
        text(g, (x, 58), lab, 12, INK_DIM)
    for i, y in enumerate(OSC_ROWS):
        text(g, (212, y), str(i + 1), 16)
        g.ellipse([201, y - 11, 223, y + 11], outline=INK, width=2)

    # mixer labels
    for i, (lab, key) in enumerate(MIX_ROWS):
        y = MIX_Y0 + i * MIX_DY
        text(g, (756, y - 8), lab, 12, anchor="lm")
        text(g, (756, y + 9), "ON/OFF", 9, INK_DIM, anchor="lm", f=FONT_PLAIN)
    text(g, (756, 548), "PINK", 10, anchor="lm")
    text(g, (756, 564), "WHITE", 10, INK_DIM, anchor="lm")
    text(g, (652, 556), "NOISE", 10, INK_DIM)

    # modifiers sub-headers
    text(g, ((832 + 1134) / 2, 56), "FILTER", 12, INK_DIM)
    text(g, ((832 + 1134) / 2, 218), "FILTER CONTOUR", 12, INK_DIM)
    text(g, ((832 + 1134) / 2, 380), "LOUDNESS CONTOUR", 12, INK_DIM)
    text(g, (MOD_COLS[3], ROW_A - 38), "KEYBOARD", 10)
    text(g, (MOD_COLS[3] - 18, ROW_A + 32), "1", 10, INK_DIM)
    text(g, (MOD_COLS[3] + 18, ROW_A + 32), "2", 10, INK_DIM)

    # per-control labels, tick scales and rotary legends
    for kind, key, cx, cy, label in CONTROLS:
        if kind in ("knob", "knobbi"):
            r0, r1 = KNOB / 2 + 3, KNOB / 2 + 7
            for k in range(11):
                a = math.radians(-SWEEP + 2 * SWEEP * k / 10)
                w = 2 if k in (0, 5, 10) else 1
                g.line([(cx + math.sin(a) * r0, cy - math.cos(a) * r0), (cx + math.sin(a) * r1, cy - math.cos(a) * r1)], fill=INK_DIM, width=w)
            if label:
                text(g, (cx, cy - KNOB / 2 - 16), label, 11)
        elif kind == "rotary":
            ch = choices[P[key]]
            for k, c in enumerate(ch):
                a = math.radians(-SWEEP + 2 * SWEEP * k / (len(ch) - 1))
                x, y = cx + math.sin(a) * (ROT / 2 + 17), cy - math.cos(a) * (ROT / 2 + 15)
                if c in WAVE_SHAPES:
                    wave_icon(g, x, y, c)
                else:
                    text(g, (x, y), c.replace("'", "′") if c != "LO" else "LO", 11)
        elif kind == "rocker":
            if label:
                text(g, (cx, cy - RK_H / 2 - 12), label, 10)
    return bg


# ====================================================================== skin JSON
def bounds(rect, hw="No", invalid="Show", when="Always"):
    return {"version": 1, "acceptsHWFocus": hw, "showWhenDataModelInvalid": invalid,
            "whenVisible": when, "boundsType": "Absolute", "bounds": rect}


def entry(name, typ, data, rect, remap=None, **kw):
    return {"version": 2,
            "componentData": {"version": 1, "name": name, "type": typ, "data": data},
            "handle remapping": {"version": 1, "map": remap or []},
            "bounds": bounds(rect, **kw)}


def r(x, y, w, h):
    return "%g %g %g %g" % (round(x, 2), round(y, 2), round(w, 2), round(h, 2))


def label_data(text_type, colour, size, just="horizontallyCentred verticallyCentred"):
    return {"version": 1, "textStyle": {"version": 1, "font": {"version": 1, "name": "Titillium Web", "style": "Regular", "height": float(size)},
                                        "colour": colour, "justification": just, "case": "Original"},
            "type": text_type, "handleName": "Data"}


def focus_data():
    return {"version": 1, "backgroundColour": "14ffffff", "outlineColour": FOCUS_ARGB, "backgroundInset": 2.0, "outlineThickness": 1.5}


def component(actions, children):
    return {"version": 2, "actions": actions,
            "backgroundData": {"version": 1, "focussed": {"version": 1, "colour": "00000000", "image": ""},
                               "unfocussed": {"version": 1, "colour": "00000000", "image": ""}},
            "ignoreMousePresses": False, "disableCoarseDataWheel": False, "componentsData": children}


def act(on, handler, extra=""):
    return {"version": 1, "onAction": on, "handler": handler, "handleName": "Data", "additionalData": extra}


def panel_components():
    """Local component definitions for the PANEL page's controls."""
    W = KNOB + 16
    knob_children = lambda strip, size: [
        entry("focus", "Focus", focus_data(), r(0, 0, W, W + 18), when="WhenFocussed"),
        entry("knob", "Knob", {"version": 2, "knobType": "FilmStrip", "filmStrip": strip, "numFrames": N_FRAMES,
                               "invert": False, "handleName": "Data"}, r((W - size) / 2, 8, size, size)),
        entry("value", "Label", label_data("Value", "ffffffff", 17), r(-10, W - 4, W + 20, 22), when="WhenFocussed"),
    ]
    rot_children = [
        entry("focus", "Focus", focus_data(), r(0, 0, W, W), when="WhenFocussed"),
        entry("knob", "Knob", {"version": 2, "knobType": "FilmStrip", "filmStrip": "pnl_rotary6.png", "numFrames": 6,
                               "invert": False, "handleName": "Data"}, r((W - ROT) / 2, 8, ROT, ROT)),
    ]
    RW, RH = RK_W + 12, RK_H + 12
    rocker_children = [
        entry("focus", "Focus", focus_data(), r(0, 0, RW, RH), when="WhenFocussed"),
        entry("rocker", "Button", {"version": 2, "onImage": "pnl_rocker_on.png", "offImage": "pnl_rocker_off.png", "buttonId": 1,
                                   "numButtonsInGroup": 1, "handleName": "Data", "gestureBehaviour": "Instant"}, r(6, 6, RK_W, RK_H)),
    ]
    knob_actions = [act("Mouse Down", "Q-Link"), act("Double Click", "Show Overlay", "knob overlay"),
                    act("Enter Pressed", "Show Overlay", "knob overlay")]
    knobbi_actions = [act("Mouse Down", "Q-Link"), act("Double Click", "Show Overlay", "knob overlay bipolar"),
                      act("Enter Pressed", "Show Overlay", "knob overlay bipolar")]
    menu_actions = [act("Mouse Down", "Q-Link"), act("Double Click", "Show Overlay", "menu overlay"),
                    act("Enter Pressed", "Show Overlay", "menu overlay")]
    return [
        {"key": "pnlKnob", "value": component(knob_actions, knob_children("pnl_knob.png", KNOB))},
        {"key": "pnlKnobBi", "value": component(knobbi_actions, knob_children("pnl_knob.png", KNOB))},
        {"key": "pnlRotary", "value": component(menu_actions, rot_children)},
        {"key": "pnlRocker", "value": component([act("Mouse Down", "Q-Link")], rocker_children)},
    ]


def panel_tab(template_tab):
    W = KNOB + 16
    comps = [entry("background", "Image", {"version": 2, "imageType": "Regular", "colour": "0", "image": "panel_bg.png"}, "0 0 1280 628")]
    types = {"knob": "pnlKnob", "knobbi": "pnlKnobBi", "rotary": "pnlRotary", "rocker": "pnlRocker"}
    for kind, key, cx, cy, _label in CONTROLS:
        if kind == "rocker":
            w, h = RK_W + 12, RK_H + 12
        elif kind == "rotary":
            w, h = W, W
        else:
            w, h = W, W + 18
        rect = r(cx - W / 2, cy - W / 2, w, h) if kind != "rocker" else r(cx - w / 2, cy - h / 2, w, h)
        comps.append(entry(key, types[kind], {"version": 1, "handleName": "Data"}, rect,
                           remap=[{"key": "Data", "value": "Parameter %d" % P[key]}], hw="Yes", invalid="Hide"))
    tab = {}
    for k in template_tab:                       # copy the generator's key order exactly
        tab[k] = template_tab[k]
    tab["tabName"], tab["fnKeyIndex"], tab["fnKeySubIndex"] = "PANEL", 0, 0
    tab["qlinkBoundsData"] = QL_RECTS
    root = json.loads(json.dumps(template_tab["componentDefinition"]))
    root["actions"] = []
    root["componentsData"] = comps
    tab["componentDefinition"] = root
    return tab


def panel_qlinks():
    q = {}
    for gi, group in enumerate(QL_GROUPS):        # group gi = grid column gi; knob k = Q-Link 13, 9, 5, 1
        for k, key in enumerate(group):
            q["Q-Link %d" % (13 + gi - 4 * k)] = P[key]
    return {"Tab": 1, "SubTab": 1, "Bank Direction": "Column", "Q-Links": {"Q-Link %d" % i: q["Q-Link %d" % i] for i in range(1, 17)}}


# ====================================================================== restyle the generated pages
def restyle_definitions(defs):
    for d in defs:
        val = d["value"]
        for c in val.get("componentsData", []):
            cd = c["componentData"]
            if cd["type"] == "Focus":
                cd["data"]["outlineColour"] = FOCUS_ARGB
            if cd["type"] == "Label" and cd["data"].get("type") == "Name":
                cd["data"]["textStyle"]["colour"] = "ffb4aea2"


def label_radio_buttons(defs, tabs, choices):
    """The generator's segment switches are blank images: print the choice names on them."""
    users = {}
    for t in tabs:
        for c in t["componentDefinition"]["componentsData"]:
            typ = c["componentData"]["type"]
            m = c["handle remapping"]["map"]
            if typ.startswith("radio") and m:
                users.setdefault(typ, set()).add(tuple(choices.get(int(m[0]["value"].split()[1]), [])))
    for d in defs:
        names = users.get(d["key"])
        if not names or len(names) != 1:
            continue
        ch = list(names.pop())
        buttons = [c for c in d["value"]["componentsData"] if c["componentData"]["type"] == "Button"]
        for c in buttons:
            k = c["componentData"]["data"]["buttonId"]
            if k < len(ch):
                lab = label_data("Text", "ffe8e4da", 22)
                lab["handleName"] = "Invalid"
                lab["text"] = ch[k].upper()
                d["value"]["componentsData"].append(entry("choice %d" % k, "Label", lab, c["bounds"]["bounds"]))


def detail_background():
    bg = brushed(1280, 628, seed=21)
    bg.paste(wood(14, 628, 5), (0, 0))
    bg.paste(wood(14, 628, 9), (1266, 0))
    return bg


def main():
    params = os.path.join(PROJECT, "build", "params.json")
    if not os.path.isfile(params):
        sys.exit("build/params.json is missing: run  python3 tools/build.py plugins/uP2noGoog --target native  first")
    pj = json.load(open(params))
    choices = {}
    for i, p in enumerate(pj["params"]):
        if p.get("choices"):
            choices[i] = p["choices"]

    tmp = tempfile.mkdtemp()
    subprocess.check_call([sys.executable, os.path.join(SDK, "tools", "gen_ui.py"), "--params", params,
                           "--ui", os.path.join(PROJECT, "ui.json"), "--out", tmp, "--kit-cache", os.path.join(PROJECT, "build"), "--quiet"])
    tui = json.load(open(os.path.join(tmp, "TUI.json")))
    ql = json.load(open(os.path.join(tmp, "Q-Links.json")))
    pd = tui["pageData"]

    # --- restyle generated pages
    defs = pd["componentDefinitions"]["localComponentDefinitions"]
    restyle_definitions(defs)
    label_radio_buttons(defs, pd["tabs"], choices)
    for t in pd["tabs"]:
        t["componentDefinition"]["backgroundData"] = {"version": 1, "focussed": {"version": 1, "colour": "ff151518", "image": "detail_bg.png"},
                                                      "unfocussed": {"version": 1, "colour": "ff151518", "image": "detail_bg.png"}}

    # --- PANEL page in front
    defs += panel_components()
    template = pd["tabs"][0]
    for t in pd["tabs"]:
        t["fnKeyIndex"] += 1
    pd["tabs"] = [panel_tab(template)] + pd["tabs"]
    for m in ql["Screen Mode Q-Links"]["map"]:
        m["Tab"] += 1
    ql["Screen Mode Q-Links"]["map"] = [panel_qlinks()] + ql["Screen Mode Q-Links"]["map"]
    pq = panel_qlinks()["Q-Links"]
    ql["Program Mode Q-Links"] = dict(pq)

    # --- images
    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    shutil.copytree(tmp, OUT)
    shutil.rmtree(tmp)
    # replace the kit's images with the uP2noGoog look (same sizes and frame counts)
    big = knob_strip(90, N_FRAMES)
    big.save(os.path.join(OUT, "knob_uni.png"))
    big.save(os.path.join(OUT, "knob_bi.png"))
    for name, (w, h) in {"toggle_on.png": (64, 32), "toggle_off.png": (64, 32)}.items():
        # toggles in the generated pages: a sideways rocker
        rk = rocker(32, 64, name.endswith("on.png")).rotate(90, expand=True)
        rk.save(os.path.join(OUT, name))
    for f in os.listdir(OUT):
        if f.startswith("seg") and f.endswith(".png"):
            w, h = Image.open(os.path.join(OUT, f)).size
            segment(w, h, f.endswith("_on.png")).save(os.path.join(OUT, f))
        if f.startswith("fader_h") and f.endswith(".png"):
            w, hh = Image.open(os.path.join(OUT, f)).size
            fader_strip(w, hh // N_FRAMES, N_FRAMES).save(os.path.join(OUT, f))
    detail_background().save(os.path.join(OUT, "detail_bg.png"))
    knob_strip(KNOB, N_FRAMES).save(os.path.join(OUT, "pnl_knob.png"))
    knob_strip(ROT, 6).save(os.path.join(OUT, "pnl_rotary6.png"))
    rocker(RK_W, RK_H, True).save(os.path.join(OUT, "pnl_rocker_on.png"))
    rocker(RK_W, RK_H, False).save(os.path.join(OUT, "pnl_rocker_off.png"))
    panel_background(choices).save(os.path.join(OUT, "panel_bg.png"))

    with open(os.path.join(OUT, "TUI.json"), "w") as f:
        json.dump(tui, f, indent=1)
    with open(os.path.join(OUT, "Q-Links.json"), "w") as f:
        json.dump(ql, f, indent=4)
    print("wrote %s (%d tabs: %s)" % (OUT, len(pd["tabs"]), ", ".join(t["tabName"] for t in pd["tabs"])))


if __name__ == "__main__":
    main()
