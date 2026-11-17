#!/usr/bin/env python3
"""Build uP2noChroma's skin into plugins/uP2noChroma/skins/.

A flat, dark console: four colour-coded module columns (Character red, Movement amber, Diffusion
green, Texture blue), each with its effect selector and On switch on top and one Q-Link bank of four
below. Layout and artwork are uP2noGood's own; no pedal branding is reproduced.

Pages and Q-Link banks (one column of four = one bank; knobs 1-4 = Q-Link 13, 9, 5, 1 + column):
  CONSOLE  Character: Tilt, Amount, Sensitivity, Vol | Movement: Rate, Amount, Drift, Vol |
           Diffusion: Time, Amount, Drift, Vol | Texture: Amount, Vol, Filter Style, Drift
           (+ touch: effect selector and On switch per column)
  SETUP    Movement tempo: Sync, Division, Spread | Diffusion tempo: Sync, Division, Ping-Pong, Duck |
           Texture tone: Tone | Global: Mix, Output, Input Level, Routing

Run after a native build (it needs build/params.json):

    python3 tools/build.py plugins/uP2noChroma --target native
    python3 plugins/uP2noChroma/skin_tools/make_skin.py [--preview file.png]

build.py copies skins/ verbatim into the plugin folder on every build.
"""
import json, math, os, re, shutil, subprocess, sys, tempfile
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
SDK = os.path.dirname(os.path.dirname(PROJECT))
OUT = os.path.join(PROJECT, "skins")

GF = "/usr/share/fonts/truetype/google-fonts/"
FONT_BOLD = [GF + "Poppins-Bold.ttf", "/System/Library/Fonts/Supplemental/Arial Bold.ttf", "/System/Library/Fonts/Helvetica.ttc",
             "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"]
FONT_MED = [GF + "Poppins-Medium.ttf", "/System/Library/Fonts/Supplemental/Arial.ttf", "/System/Library/Fonts/Helvetica.ttc",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"]

# Parameter indices (must match ParamIndex in src/UP2noChroma.cpp)
NAMES = ["output", "mix", "char_fx", "char_on", "tilt", "char_amt", "sens", "char_vol",
         "move_fx", "move_on", "rate", "move_amt", "move_drift", "move_vol",
         "diff_fx", "diff_on", "time", "diff_amt", "diff_drift", "diff_vol",
         "tex_fx", "tex_on", "tex_amt", "tex_vol", "filter_style", "input_level", "routing",
         "move_sync", "move_div", "diff_sync", "diff_div", "spread", "pingpong", "duck", "tex_drift", "tex_tone"]
P = {n: i for i, n in enumerate(NAMES)}

PAGE = (22, 23, 27)
INK = (238, 234, 226)
BOX = (10, 10, 12)
DISC = (40, 41, 46)
VALUE_ARGB = "fff0ece2"
FOCUS_ARGB = "ffffffff"
COLORS = {"chr": (232, 84, 62), "mov": (240, 182, 52), "dif": (84, 186, 110), "tex": (70, 146, 226), "glb": (178, 178, 170)}

W, H = 1280, 628
COL_X0, COL_DX, COL_W = 14, 314, 310
HEAD_H = 42
SEL_Y, SEL_H = 46, 40
ROW_Y0, ROW_DY, CELL_H = 94, 132, 126
KNOB, KNOB_FRAMES, SWEEP = 84, 64, 140.0
ON_W, ON_H = 74, 30
DIV_N, ROUTE_N = 17, 24

# ---------------------------------------------------------------- layout
# cell: (param key, widget kind)   kinds: knob, bi, menu17, menu24, seg (segments, labels given)
SEGS = {"filter_style": ["LPF", "TILT", "HPF"], "input_level": ["LOW", "MED", "HIGH", "V.HI"],
        "move_sync": ["OFF", "SYNC"], "diff_sync": ["OFF", "SYNC"], "pingpong": ["OFF", "ON"]}
FX = {"chr": ("char_fx", "char_on", ["DRIVE", "SWEET", "FUZZ", "HOWL", "SWELL"]),
      "mov": ("move_fx", "move_on", ["DOUBL", "VIBE", "PHASE", "TREM", "PITCH"]),
      "dif": ("diff_fx", "diff_on", ["CASC", "REELS", "SPACE", "COLL", "REV"]),
      "tex": ("tex_fx", "tex_on", ["FILT", "SQUSH", "CASS", "BRKN", "INTF"])}
PAGES = [
    ("CONSOLE", [("chr", "CHARACTER", [("tilt", "bi"), ("char_amt", "knob"), ("sens", "bi"), ("char_vol", "knob")]),
                 ("mov", "MOVEMENT", [("rate", "knob"), ("move_amt", "knob"), ("move_drift", "knob"), ("move_vol", "knob")]),
                 ("dif", "DIFFUSION", [("time", "knob"), ("diff_amt", "knob"), ("diff_drift", "knob"), ("diff_vol", "knob")]),
                 ("tex", "TEXTURE", [("tex_amt", "knob"), ("tex_vol", "knob"), ("filter_style", "seg"), ("tex_drift", "knob")])]),
    ("SETUP", [("mov", "MOVE TEMPO", [("move_sync", "seg"), ("move_div", "menu17"), ("spread", "knob"), None]),
               ("dif", "DIFF TEMPO", [("diff_sync", "seg"), ("diff_div", "menu17"), ("pingpong", "seg"), ("duck", "knob")]),
               ("tex", "TEXTURE TONE", [("tex_tone", "bi"), None, None, None]),
               ("glb", "GLOBAL", [("mix", "knob"), ("output", "bi"), ("input_level", "seg"), ("routing", "menu24")])]),
]
SUBTITLES = {"mov": "VIBRATO · PHASER · TREMOLO", "dif": "CASCADE · REELS · COLLAGE · REVERSE", "tex": "RESONANCE ON FILTER", "glb": "LEVELS AND ORDER"}
LABELS = dict(tilt="TILT", char_amt="AMOUNT", sens="SENSITIVITY", char_vol="VOL", rate="RATE", move_amt="AMOUNT",
              move_drift="DRIFT", move_vol="VOL", time="TIME", diff_amt="AMOUNT", diff_drift="DRIFT", diff_vol="VOL",
              tex_amt="AMOUNT", tex_vol="VOL", filter_style="FILTER STYLE", tex_drift="DRIFT", move_sync="SYNC",
              move_div="DIVISION", spread="SPREAD", diff_sync="SYNC", diff_div="DIVISION", pingpong="PING-PONG",
              duck="DUCK", tex_tone="TONE", mix="MIX", output="OUTPUT", input_level="INPUT LEVEL", routing="ROUTING")


def font(paths, size):
    for p in paths:
        try:
            return ImageFont.truetype(p, size)
        except OSError:
            continue
    return ImageFont.load_default()


def mixc(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def col_x(c):
    return COL_X0 + c * COL_DX


def cell_xy(c, row):
    return col_x(c), ROW_Y0 + row * ROW_DY


def seg_geom(n):
    w = {2: 120, 3: 86, 4: 64}[n]
    return w, 44, 6


# ====================================================================== artwork (flat)
def background(page):
    name, cols = page
    im = Image.new("RGB", (W, H), PAGE)
    g = ImageDraw.Draw(im)
    for c, (mod, title, cells) in enumerate(cols):
        x, col = col_x(c), COLORS[mod]
        field = mixc(col, PAGE, 0.86)
        g.rectangle([x, 0, x + COL_W - 1, ROW_Y0 + 4 * ROW_DY - 6], fill=field)
        g.rectangle([x, 0, x + COL_W - 1, HEAD_H - 1], fill=col)
        g.text((x + 14, HEAD_H / 2 + 1), title, fill=PAGE, font=font(FONT_BOLD, 22), anchor="lm")
        if name != "CONSOLE":
            g.text((x + 14, SEL_Y + SEL_H / 2), SUBTITLES.get(mod, ""), fill=mixc(col, INK, 0.35), font=font(FONT_MED, 13), anchor="lm")
        g.line([(x, ROW_Y0 - 4), (x + COL_W - 1, ROW_Y0 - 4)], fill=col, width=2)
        for row, cell in enumerate(cells):
            cx, cy = cell_xy(c, row)
            if row > 0:
                g.line([(cx + 14, cy - 3), (cx + COL_W - 15, cy - 3)], fill=mixc(col, PAGE, 0.6), width=1)
            if cell is None:
                continue
            key, kind = cell
            if kind == "seg":
                g.text((cx + 16, cy + 30), LABELS[key], fill=INK, font=font(FONT_BOLD, 18), anchor="lm")
            else:
                g.text((cx + 112, cy + 30), LABELS[key], fill=INK, font=font(FONT_BOLD, 18), anchor="lm")
                g.rectangle([cx + 112, cy + 54, cx + 112 + 180, cy + 54 + 46], fill=BOX)
    # wordmark in the free space of SETUP's third column
    if name == "SETUP":
        x0, y0 = col_x(2) + 18, ROW_Y0 + ROW_DY + 20
        g.text((x0, y0), "uP2no", fill=INK, font=font(FONT_BOLD, 44), anchor="la")
        g.text((x0, y0 + 46), "Chroma", fill=COLORS["tex"], font=font(FONT_BOLD, 44), anchor="la")
        bw = 54
        for i, k in enumerate(["chr", "mov", "dif", "tex"]):
            g.rectangle([x0 + i * (bw + 10), y0 + 120, x0 + i * (bw + 10) + bw, y0 + 150], fill=COLORS[k])
        g.text((x0, y0 + 176), "4-module colour", fill=mixc(INK, PAGE, 0.3), font=font(FONT_MED, 16), anchor="la")
        g.text((x0, y0 + 198), "multi-effect", fill=mixc(INK, PAGE, 0.3), font=font(FONT_MED, 16), anchor="la")
    g.text((W - 16, H - 4), "uP2noGood", fill=mixc(INK, PAGE, 0.5), font=font(FONT_MED, 13), anchor="rb")
    return im.convert("RGBA")


def knob_frame(size, frac, col, bipolar, ss=4):
    S = size * ss
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    g = ImageDraw.Draw(im)
    c, r = S / 2, S * 0.48
    track = mixc(col, PAGE, 0.75)
    w = int(6 * ss)
    box = [c - r + w / 2, c - r + w / 2, c + r - w / 2, c + r - w / 2]
    # PIL arcs: 0 deg = 3 o'clock, clockwise. Knob sweep -SWEEP..+SWEEP around 12 o'clock.
    a0, a1 = -90 - SWEEP, -90 + SWEEP
    g.arc(box, a0, a1, fill=track + (255,), width=w)
    ang = -SWEEP + 2 * SWEEP * frac
    if bipolar:
        lo, hi = sorted([-90.0, -90.0 + ang])
    else:
        lo, hi = a0, -90.0 + ang
    if hi - lo > 0.5:
        g.arc(box, lo, hi, fill=col + (255,), width=w)
    r2 = r - 11 * ss
    g.ellipse([c - r2, c - r2, c + r2, c + r2], fill=DISC + (255,))
    a = math.radians(ang)
    g.line([(c + math.sin(a) * r2 * 0.25, c - math.cos(a) * r2 * 0.25), (c + math.sin(a) * r2 * 0.92, c - math.cos(a) * r2 * 0.92)],
           fill=INK + (255,), width=int(5 * ss))
    return im.resize((size, size), Image.LANCZOS)


def strip(frames, col, bipolar=False):
    im = Image.new("RGBA", (KNOB, KNOB * frames), (0, 0, 0, 0))
    for i in range(frames):
        im.paste(knob_frame(KNOB, i / (frames - 1), col, bipolar), (0, i * KNOB))
    return im


def segment(label, on, col, w, h, size=18):
    im = Image.new("RGBA", (w, h), (col if on else BOX) + (255,))
    g = ImageDraw.Draw(im)
    if not on:
        g.rectangle([0, 0, w - 1, h - 1], outline=mixc(col, PAGE, 0.35) + (255,), width=2)
    g.text((w / 2, h / 2 + 1), label, fill=(PAGE if on else mixc(col, INK, 0.3)) + (255,), font=font(FONT_BOLD, size), anchor="mm")
    return im


def on_button(on, col):
    """Module On switch in the header band: lit = dark pill with a coloured lamp, off = flat outline."""
    im = Image.new("RGBA", (ON_W, ON_H), (0, 0, 0, 0))
    g = ImageDraw.Draw(im)
    if on:
        g.rounded_rectangle([0, 0, ON_W - 1, ON_H - 1], radius=6, fill=PAGE + (255,))
        g.ellipse([9, ON_H / 2 - 6, 21, ON_H / 2 + 6], fill=col + (255,))
        g.text((ON_W / 2 + 8, ON_H / 2 + 1), "ON", fill=INK + (255,), font=font(FONT_BOLD, 15), anchor="mm")
    else:
        g.rounded_rectangle([0, 0, ON_W - 1, ON_H - 1], radius=6, outline=PAGE + (255,), width=2)
        g.ellipse([9, ON_H / 2 - 6, 21, ON_H / 2 + 6], outline=PAGE + (255,), width=2)
        g.text((ON_W / 2 + 8, ON_H / 2 + 1), "OFF", fill=PAGE + (255,), font=font(FONT_BOLD, 15), anchor="mm")
    return im


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
    return "%g %g %g %g" % (x, y, w, h)


def value_label():
    return {"version": 1, "textStyle": {"version": 1, "font": {"version": 1, "name": "Titillium Web", "style": "Regular", "height": 24.0},
                                        "colour": VALUE_ARGB, "justification": "horizontallyCentred verticallyCentred", "case": "Original"},
            "type": "Value", "handleName": "Data"}


def focus():
    return entry("focus", "Focus", {"version": 1, "backgroundColour": "00000000", "outlineColour": FOCUS_ARGB, "backgroundInset": 3.0, "outlineThickness": 3.0},
                 r(0, 0, COL_W, CELL_H), when="WhenFocussed")


def component(actions, children):
    return {"version": 2, "actions": actions,
            "backgroundData": {"version": 1, "focussed": {"version": 1, "colour": "00000000", "image": ""},
                               "unfocussed": {"version": 1, "colour": "00000000", "image": ""}},
            "ignoreMousePresses": False, "disableCoarseDataWheel": False, "componentsData": children}


def act(on, handler, extra=""):
    return {"version": 1, "onAction": on, "handler": handler, "handleName": "Data", "additionalData": extra}


def knob_def(image, frames, overlay):
    kids = [focus(),
            entry("knob", "Knob", {"version": 2, "knobType": "FilmStrip", "filmStrip": image, "numFrames": frames, "invert": False, "handleName": "Data"},
                  r(14, 22, KNOB, KNOB)),
            entry("value", "Label", value_label(), r(112, 54, 180, 46))]
    return component([act("Mouse Down", "Q-Link"), act("Double Click", "Show Overlay", overlay), act("Enter Pressed", "Show Overlay", overlay)], kids)


def buttons(prefix, n, x0, y0, w, h, gap):
    return [entry("seg %d" % k, "Button", {"version": 2, "onImage": "%s%d_on.png" % (prefix, k), "offImage": "%s%d_off.png" % (prefix, k),
                                            "buttonId": k, "numButtonsInGroup": n, "handleName": "Data", "gestureBehaviour": "Instant"},
                  r(x0 + k * (w + gap), y0, w, h)) for k in range(n)]


def seg_cell_def(prefix, n):
    w, h, gap = seg_geom(n)
    return component([act("Mouse Down", "Q-Link")], [focus()] + buttons(prefix, n, 16, 54, w, h, gap))


def selector_def(prefix):
    return component([], buttons(prefix, 5, 4, 1, 58, SEL_H - 2, 3))


def on_def(prefix):
    return component([], [entry("switch", "Button", {"version": 2, "onImage": prefix + "_on.png", "offImage": prefix + "_off.png", "buttonId": 1,
                                                     "numButtonsInGroup": 1, "handleName": "Data", "gestureBehaviour": "Instant"}, r(0, 0, ON_W, ON_H))])


def seg_prefix(mod, key):
    return "ch_%s_%s_" % (mod, key)


def definitions():
    d = []
    for mod in COLORS:
        d.append({"key": "chKnob_" + mod, "value": knob_def("ch_knob_%s.png" % mod, KNOB_FRAMES, "knob overlay")})
        d.append({"key": "chKnobBi_" + mod, "value": knob_def("ch_knobbi_%s.png" % mod, KNOB_FRAMES, "knob overlay bipolar")})
    d.append({"key": "chMenu17", "value": knob_def("ch_menu17.png", DIV_N, "menu overlay")})
    d.append({"key": "chMenu24", "value": knob_def("ch_menu24.png", ROUTE_N, "menu overlay")})
    seen = set()
    for _name, cols in PAGES:
        for mod, _t, cells in cols:
            for cell in cells:
                if cell and cell[1] == "seg" and (mod, cell[0]) not in seen:
                    seen.add((mod, cell[0]))
                    d.append({"key": "chSeg_%s_%s" % (mod, cell[0]), "value": seg_cell_def(seg_prefix(mod, cell[0]), len(SEGS[cell[0]]))})
    for mod, (fxk, onk, _labels) in FX.items():
        d.append({"key": "chSel_" + mod, "value": selector_def("ch_sel_%s_" % mod)})
        d.append({"key": "chOn_" + mod, "value": on_def("ch_on_" + mod)})
    return d


def ctl(name, typ, key, rect, hw="Yes"):
    return entry(name, typ, {"version": 1, "handleName": "Data"}, rect,
                 remap=[{"key": "Data", "value": "Parameter %d" % P[key]}], hw=hw, invalid="Hide")


def make_tab(template, index, page):
    name, cols = page
    comps = [entry("bg", "Image", {"version": 2, "imageType": "Regular", "colour": "0", "image": "ch_%s.png" % name.lower()}, r(0, 0, W, H))]
    for c, (mod, _title, cells) in enumerate(cols):
        x = col_x(c)
        if name == "CONSOLE":
            fxk, onk, _l = FX[mod]
            comps.append(ctl(fxk, "chSel_" + mod, fxk, r(x + 2, SEL_Y, COL_W - 4, SEL_H), hw="No"))
            comps.append(ctl(onk, "chOn_" + mod, onk, r(x + COL_W - ON_W - 6, (HEAD_H - ON_H) / 2, ON_W, ON_H), hw="No"))
        for row, cell in enumerate(cells):
            if cell is None:
                continue
            key, kind = cell
            cx, cy = cell_xy(c, row)
            typ = {"knob": "chKnob_" + mod, "bi": "chKnobBi_" + mod, "menu17": "chMenu17", "menu24": "chMenu24",
                   "seg": "chSeg_%s_%s" % (mod, key)}[kind]
            comps.append(ctl(key, typ, key, r(cx, cy, COL_W, CELL_H)))
    tab = {k: template[k] for k in template}
    tab["tabName"], tab["fnKeyIndex"], tab["fnKeySubIndex"] = name, index, 0
    tab["qlinkBoundsData"] = [r(col_x(c), ROW_Y0 - 2, COL_W, 4 * ROW_DY - 2) for c in range(len(cols))]
    root = json.loads(json.dumps(template["componentDefinition"]))
    root["actions"] = []
    root["componentsData"] = comps
    tab["componentDefinition"] = root
    return tab


def qlinks(tab, page):
    q = {"Q-Link %d" % i: -1 for i in range(1, 17)}
    for c, (_m, _t, cells) in enumerate(page[1]):
        for k, cell in enumerate(cells):
            if cell is not None:
                q["Q-Link %d" % (13 + c - 4 * k)] = P[cell[0]]     # column c, knob k = Q-Link 13, 9, 5, 1 (+c)
    return {"Tab": tab, "SubTab": 1, "Bank Direction": "Column", "Q-Links": q}


def preview(path, images):
    samples = dict(tilt="+12", char_amt="45 %", sens="0", char_vol="+0.0 dB", rate="0.41 Hz", move_amt="25 %", move_drift="30 %",
                   move_vol="+0.0 dB", time="Hall 2.5 s", diff_amt="30 %", diff_drift="0 %", diff_vol="+0.0 dB", tex_amt="55 %",
                   tex_vol="+0.0 dB", tex_drift="20 %", move_div="1/4", spread="50 %", diff_div="1/8D", duck="0 %", tex_tone="0",
                   mix="100 %", output="0.0 dB", routing="C>M>D>T")
    sel = {"chr": 0, "mov": 1, "dif": 2, "tex": 2}
    segsel = {"filter_style": 1, "input_level": 2, "move_sync": 0, "diff_sync": 1, "pingpong": 0}
    frames = dict(tilt=36, char_amt=29, sens=32, rate=22, move_amt=16, move_drift=19, time=32, diff_amt=19, diff_drift=0,
                  tex_amt=35, tex_drift=13, spread=32, duck=0, tex_tone=32, mix=63, output=32)
    shots = []
    for page in PAGES:
        im = images["ch_%s.png" % page[0].lower()].copy()
        g = ImageDraw.Draw(im)
        for c, (mod, _t, cells) in enumerate(page[1]):
            x = col_x(c)
            if page[0] == "CONSOLE":
                for k in range(5):
                    im.alpha_composite(images["ch_sel_%s_%d_%s.png" % (mod, k, "on" if k == sel[mod] else "off")], (x + 2 + 4 + k * 61, SEL_Y + 1))
                im.alpha_composite(images["ch_on_%s_%s.png" % (mod, "off" if mod == "tex" else "on")], (x + COL_W - ON_W - 6, (HEAD_H - ON_H) // 2))
            for row, cell in enumerate(cells):
                if cell is None:
                    continue
                key, kind = cell
                cx, cy = cell_xy(c, row)
                if kind == "seg":
                    n = len(SEGS[key]); w, h, gap = seg_geom(n)
                    for j in range(n):
                        im.alpha_composite(images["%s%d_%s.png" % (seg_prefix(mod, key), j, "on" if j == segsel[key] else "off")], (cx + 16 + j * (w + gap), cy + 54))
                    continue
                if kind.startswith("menu"):
                    src, fr = images["ch_%s.png" % kind], 10 if kind == "menu17" else 0
                elif kind == "bi":
                    src, fr = images["ch_knobbi_%s.png" % mod], frames.get(key, 32)
                else:
                    src, fr = images["ch_knob_%s.png" % mod], frames.get(key, 63 if key.endswith("vol") else 20)
                im.alpha_composite(src.crop((0, fr * KNOB, KNOB, (fr + 1) * KNOB)), (cx + 14, cy + 22))
                g.text((cx + 112 + 90, cy + 54 + 23), samples.get(key, ""), fill=INK, font=font(FONT_MED, 22), anchor="mm")
        shots.append(im.convert("RGB"))
    sheet = Image.new("RGB", (W, H * 2 + 8), (0, 0, 0))
    sheet.paste(shots[0], (0, 0)); sheet.paste(shots[1], (0, H + 8))
    sheet.save(path)


def main():
    params = os.path.join(PROJECT, "build", "params.json")
    if not os.path.isfile(params):
        sys.exit("build/params.json is missing: run  python3 tools/build.py plugins/uP2noChroma --target native  first")
    tmp = tempfile.mkdtemp()
    subprocess.check_call([sys.executable, os.path.join(SDK, "tools", "gen_ui.py"), "--params", params,
                           "--ui", os.path.join(PROJECT, "ui.json"), "--out", tmp, "--kit-cache", os.path.join(PROJECT, "build"), "--quiet"])
    tui = json.load(open(os.path.join(tmp, "TUI.json")))
    ql = json.load(open(os.path.join(tmp, "Q-Links.json")))
    shutil.rmtree(tmp)
    pd = tui["pageData"]
    defs = [d for d in pd["componentDefinitions"]["localComponentDefinitions"] if "overlay" in d["key"]]
    have = {d["key"] for d in defs}
    for need in ("knob overlay", "knob overlay bipolar", "menu overlay"):
        if need not in have:
            sys.exit("the generated template has no '%s' definition (check ui.json)" % need)
    pd["componentDefinitions"]["localComponentDefinitions"] = defs + definitions()
    template = pd["tabs"][0]
    pd["tabs"] = [make_tab(template, i, page) for i, page in enumerate(PAGES)]
    ql["Screen Mode Q-Links"]["map"] = [qlinks(i + 1, page) for i, page in enumerate(PAGES)]
    ql["Program Mode Q-Links"] = dict(qlinks(1, PAGES[0])["Q-Links"])

    images = {}
    for mod, col in COLORS.items():
        images["ch_knob_%s.png" % mod] = strip(KNOB_FRAMES, col)
        images["ch_knobbi_%s.png" % mod] = strip(KNOB_FRAMES, col, bipolar=True)
    images["ch_menu17.png"] = strip(DIV_N, COLORS["glb"])
    images["ch_menu24.png"] = strip(ROUTE_N, COLORS["glb"])
    for page in PAGES:
        images["ch_%s.png" % page[0].lower()] = background(page)
        for mod, _t, cells in page[1]:
            for cell in cells:
                if cell and cell[1] == "seg":
                    labels = SEGS[cell[0]]; w, h, _gap = seg_geom(len(labels))
                    for j, lab in enumerate(labels):
                        for on in (True, False):
                            images["%s%d_%s.png" % (seg_prefix(mod, cell[0]), j, "on" if on else "off")] = segment(lab, on, COLORS[mod], w, h)
    for mod, (_fx, _on, labels) in FX.items():
        for j, lab in enumerate(labels):
            for on in (True, False):
                images["ch_sel_%s_%d_%s.png" % (mod, j, "on" if on else "off")] = segment(lab, on, COLORS[mod], 58, SEL_H - 2, size=14)
        images["ch_on_%s_on.png" % mod] = on_button(True, COLORS[mod])
        images["ch_on_%s_off.png" % mod] = on_button(False, COLORS[mod])
    os.makedirs(OUT, exist_ok=True)
    for name, im in images.items():
        im.save(os.path.join(OUT, name))
    with open(os.path.join(OUT, "TUI.json"), "w") as f:
        json.dump(tui, f, indent=1)
    with open(os.path.join(OUT, "Q-Links.json"), "w") as f:
        json.dump(ql, f, indent=4)
    missing = set(re.findall(r'"([^"]+\.png)"', json.dumps(tui))) - set(images)
    if missing:
        sys.exit("skin refers to images that were not written: %s" % ", ".join(sorted(missing)))
    if "--preview" in sys.argv:
        preview(sys.argv[sys.argv.index("--preview") + 1], images)
    print("wrote %s (%d tabs: %s, %d images)" % (OUT, len(pd["tabs"]), ", ".join(t["tabName"] for t in pd["tabs"]), len(images)))


if __name__ == "__main__":
    main()
