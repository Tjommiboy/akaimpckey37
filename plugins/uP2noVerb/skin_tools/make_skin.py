#!/usr/bin/env python3
"""Build uP2noVerb's skin into plugins/uP2noVerb/skins/.

A flat, screen-print style skin: solid colour fields, big capital labels, abstract disc knobs, no
shading, no fake hardware. The whole palette follows the Color parameter (1970s / 1980s / Now), so
the screen shows which era is sounding. Layout and artwork are uP2noGood's own.

Pages and Q-Link banks are the same as ui.json (one column of four = one bank):
  VERB  Main | Type | Damping | Modulation        TONE  EQ | Diffusion

Run after a native build (it needs build/params.json):

    python3 tools/build.py plugins/uP2noVerb --target native
    python3 plugins/uP2noVerb/skin_tools/make_skin.py [--preview file.png]

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

# Parameter indices (must match ParamIndex in src/UP2noVerb.cpp)
P = dict(output=0, mix=1, predelay=2, decay=3, mode=4, color=5, size=6, attack=7, highfreq=8, highshelf=9,
         bassxover=10, bassmult=11, modrate=12, moddepth=13, highcut=14, lowcut=15, earlydiff=16, latediff=17)

# One palette per Color choice: page, column field, heading band, ink, value box.
PALETTES = [
    dict(key="70s", page=(196, 98, 40),   field=(232, 206, 160), band=(92, 44, 24),   band_ink=(240, 214, 168), ink=(64, 34, 20),   box=(64, 34, 20),  mark=(92, 44, 24)),
    dict(key="80s", page=(18, 20, 52),    field=(34, 38, 92),    band=(236, 56, 150), band_ink=(18, 20, 52),    ink=(120, 232, 236), box=(8, 9, 28),    mark=(236, 56, 150)),
    dict(key="now", page=(226, 228, 224), field=(250, 250, 248), band=(24, 26, 30),   band_ink=(250, 250, 248), ink=(24, 26, 30),   box=(24, 26, 30),  mark=(0, 146, 132)),
]
BASE = 1                                  # the palette drawn underneath (1980s); the others sit on top when selected
CREAM = (244, 239, 228)
DISC = (22, 22, 26)
VALUE_ARGB = "fff4efe4"
FOCUS_ARGB = "ffffffff"

W, H = 1280, 628
COL_X0, COL_DX, COL_W = 14, 314, 310      # four columns
HEAD_H = 46
ROW_Y0, ROW_DY = 52, 132                  # four rows
CELL_H = 126
KNOB, KNOB_FRAMES, SWEEP = 84, 64, 140.0
SEG_W, SEG_H = 90, 44

PAGES = [
    ("VERB", [("MAIN", ["mix", "predelay", "decay", "output"]),
              ("TYPE", ["mode", "color", "size", "attack"]),
              ("DAMPING", ["highfreq", "highshelf", "bassxover", "bassmult"]),
              ("MODULATION", ["modrate", "moddepth"])]),
    ("TONE", [("EQ", ["highcut", "lowcut"]),
              ("DIFFUSION", ["earlydiff", "latediff"])]),
]
LABELS = dict(mix="MIX", predelay="PREDELAY", decay="DECAY", output="OUTPUT", mode="MODE", color="COLOR", size="SIZE",
              attack="ATTACK", highfreq="HIGH FREQ", highshelf="HIGH SHELF", bassxover="BASS XOVER", bassmult="BASS MULT",
              modrate="RATE", moddepth="DEPTH", highcut="HIGH CUT", lowcut="LOW CUT", earlydiff="EARLY", latediff="LATE")
COLOR_NAMES = ["70s", "80s", "NOW"]


def font(paths, size):
    for p in paths:
        try:
            return ImageFont.truetype(p, size)
        except OSError:
            continue
    return ImageFont.load_default()


def cell_xy(col, row):
    return COL_X0 + col * COL_DX, ROW_Y0 + row * ROW_DY


# ====================================================================== artwork (all flat)
def background(page, pal):
    name, cols = page
    im = Image.new("RGB", (W, H), pal["page"])
    g = ImageDraw.Draw(im)
    for c, (title, keys) in enumerate(cols):
        x = COL_X0 + c * COL_DX
        g.rectangle([x, 0, x + COL_W - 1, ROW_Y0 + 4 * ROW_DY - 6], fill=pal["field"])
        g.rectangle([x, 0, x + COL_W - 1, HEAD_H - 1], fill=pal["band"])
        g.text((x + 16, HEAD_H / 2 + 1), title, fill=pal["band_ink"], font=font(FONT_BOLD, 24), anchor="lm")
        for r, key in enumerate(keys):
            cx, cy = cell_xy(c, r)
            if r > 0:
                g.line([(cx + 14, cy - 3), (cx + COL_W - 15, cy - 3)], fill=pal["ink"], width=2)
            if key == "color":
                g.text((cx + 16, cy + 30), LABELS[key], fill=pal["ink"], font=font(FONT_BOLD, 20), anchor="lm")
            else:
                g.text((cx + 112, cy + 30), LABELS[key], fill=pal["ink"], font=font(FONT_BOLD, 20), anchor="lm")
                g.rectangle([cx + 112, cy + 54, cx + 112 + 180, cy + 54 + 46], fill=pal["box"])
    # free space: wordmark and a decay figure made of bars
    free = len(cols)
    if free < 4 or len(cols[-1][1]) < 4:
        if len(cols[-1][1]) < 4 and free == 4:
            x0, y0, x1 = COL_X0 + 3 * COL_DX + 16, ROW_Y0 + 2 * ROW_DY + 14, COL_X0 + 3 * COL_DX + COL_W - 16
            ink, size = pal["ink"], 40
        else:
            x0, y0, x1 = COL_X0 + free * COL_DX + 20, 40, W - 34
            ink, size = pal["field"], 92
        g.text((x0, y0), "uP2no", fill=ink, font=font(FONT_BOLD, size), anchor="la")
        g.text((x0, y0 + size * 1.02), "Verb", fill=pal["mark"], font=font(FONT_BOLD, size), anchor="la")
        by = y0 + size * 2.45
        n = 14 if size > 60 else 9
        bw = (x1 - x0) / n
        top = H - 40 - by if size > 60 else 86
        for i in range(n):
            h = top * math.exp(-3.0 * i / n)
            g.rectangle([x0 + i * bw, by + top - h, x0 + i * bw + bw * 0.62, by + top], fill=pal["mark"] if i % 2 == 0 else ink)
    g.text((W - 16, H - 16), "uP2noGood", fill=pal["ink"] if pal["key"] != "70s" else pal["field"], font=font(FONT_MED, 14), anchor="rm")
    g.text((COL_X0 + 2, H - 16), name + "  ·  VINTAGE DIGITAL REVERB", fill=pal["ink"] if pal["key"] != "70s" else pal["field"], font=font(FONT_MED, 14), anchor="lm")
    return im.convert("RGBA")


def knob_frame(size, angle_deg, ss=4):
    """An abstract knob: a flat dark disc with a light ring and one line."""
    S = size * ss
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    g = ImageDraw.Draw(im)
    c, r = S / 2, S * 0.48
    g.ellipse([c - r, c - r, c + r, c + r], fill=CREAM + (255,))
    r2 = r - 3.5 * ss
    g.ellipse([c - r2, c - r2, c + r2, c + r2], fill=DISC + (255,))
    a = math.radians(angle_deg)
    g.line([(c + math.sin(a) * r2 * 0.28, c - math.cos(a) * r2 * 0.28), (c + math.sin(a) * r2 * 0.94, c - math.cos(a) * r2 * 0.94)],
           fill=CREAM + (255,), width=int(5 * ss))
    return im.resize((size, size), Image.LANCZOS)


def strip(frames):
    im = Image.new("RGBA", (KNOB, KNOB * frames), (0, 0, 0, 0))
    for i in range(frames):
        im.paste(knob_frame(KNOB, -SWEEP + 2 * SWEEP * i / (frames - 1)), (0, i * KNOB))
    return im


def segment(label, on):
    im = Image.new("RGBA", (SEG_W, SEG_H), (DISC if not on else CREAM) + (255,))
    g = ImageDraw.Draw(im)
    if not on:
        g.rectangle([0, 0, SEG_W - 1, SEG_H - 1], outline=CREAM + (255,), width=2)
    g.text((SEG_W / 2, SEG_H / 2 + 1), label, fill=(CREAM if not on else DISC) + (255,), font=font(FONT_BOLD, 20), anchor="mm")
    return im


# ====================================================================== skin JSON
def bounds(rect, hw="No", invalid="Show", when="Always", only_when=None):
    b = {"version": 2 if only_when else 1, "acceptsHWFocus": hw, "showWhenDataModelInvalid": invalid,
         "whenVisible": when, "boundsType": "Absolute", "bounds": rect}
    if only_when:
        b["additionalInvalidatingHandles"] = only_when
    return b


def entry(name, typ, data, rect, remap=None, **kw):
    return {"version": 2,
            "componentData": {"version": 1, "name": name, "type": typ, "data": data},
            "handle remapping": {"version": 1, "map": remap or []},
            "bounds": bounds(rect, **kw)}


def r(x, y, w, h):
    return "%g %g %g %g" % (x, y, w, h)


def value_label():
    return {"version": 1, "textStyle": {"version": 1, "font": {"version": 1, "name": "Titillium Web", "style": "Regular", "height": 26.0},
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


def color_def():
    kids = [focus()]
    for k, name in enumerate(COLOR_NAMES):
        kids.append(entry("seg %d" % k, "Button", {"version": 2, "onImage": "vv_seg%d_on.png" % k, "offImage": "vv_seg%d_off.png" % k, "buttonId": k,
                                                    "numButtonsInGroup": len(COLOR_NAMES), "handleName": "Data", "gestureBehaviour": "Instant"},
                          r(16 + k * (SEG_W + 6), 54, SEG_W, SEG_H)))
    return component([act("Mouse Down", "Q-Link")], kids)


def definitions():
    return [{"key": "vvKnob", "value": knob_def("vv_knob.png", KNOB_FRAMES, "knob overlay")},
            {"key": "vvKnobBi", "value": knob_def("vv_knob.png", KNOB_FRAMES, "knob overlay bipolar")},
            {"key": "vvMode", "value": knob_def("vv_knob8.png", 8, "menu overlay")},
            {"key": "vvColor", "value": color_def()}]


def make_tab(template, index, page):
    name, cols = page
    comps = [entry("bg", "Image", {"version": 2, "imageType": "Regular", "colour": "0", "image": "vv_%s_%s.png" % (name.lower(), PALETTES[BASE]["key"])}, r(0, 0, W, H))]
    for k, pal in enumerate(PALETTES):
        if k != BASE:   # shown only while Color is at this choice
            comps.append(entry("bg " + pal["key"], "Image", {"version": 2, "imageType": "Regular", "colour": "0", "image": "vv_%s_%s.png" % (name.lower(), pal["key"])},
                               r(0, 0, W, H), only_when=["IndexedEnabling/%d/%d/Parameter %d" % (k, len(PALETTES), P["color"])]))
    for c, (_title, keys) in enumerate(cols):
        for row, key in enumerate(keys):
            x, y = cell_xy(c, row)
            typ = {"mode": "vvMode", "color": "vvColor", "output": "vvKnobBi"}.get(key, "vvKnob")
            comps.append(entry(key, typ, {"version": 1, "handleName": "Data"}, r(x, y, COL_W, CELL_H),
                               remap=[{"key": "Data", "value": "Parameter %d" % P[key]}], hw="Yes", invalid="Hide"))
    tab = {k: template[k] for k in template}
    tab["tabName"], tab["fnKeyIndex"], tab["fnKeySubIndex"] = name, index, 0
    tab["qlinkBoundsData"] = [r(COL_X0 + c * COL_DX, 0, COL_W, ROW_Y0 + 4 * ROW_DY - 6) for c in range(len(cols))]
    root = json.loads(json.dumps(template["componentDefinition"]))
    root["actions"] = []
    root["componentsData"] = comps
    tab["componentDefinition"] = root
    return tab


def qlinks(tab, page):
    q = {"Q-Link %d" % i: -1 for i in range(1, 17)}
    for c, (_t, keys) in enumerate(page[1]):
        for k, key in enumerate(keys):
            q["Q-Link %d" % (13 + c - 4 * k)] = P[key]     # column c, knob k = Q-Link 13, 9, 5, 1 (+c)
    return {"Tab": tab, "SubTab": 1, "Bank Direction": "Column", "Q-Links": q}


def preview(path, images):
    samples = dict(mix="30 %", predelay="12 ms", decay="2.20 s", output="0.0 dB", mode="Concert Hall", size="55 %", attack="30 %",
                   highfreq="5500 Hz", highshelf="-8.0 dB", bassxover="400 Hz", bassmult="1.25 x", modrate="0.60 Hz", moddepth="45 %",
                   highcut="9000 Hz", lowcut="60 Hz", earlydiff="80 %", latediff="70 %")
    def shot(page, k):
        im = images["vv_%s_%s.png" % (page[0].lower(), PALETTES[k]["key"])].copy()
        g = ImageDraw.Draw(im)
        for c, (_t, keys) in enumerate(page[1]):
            for row, key in enumerate(keys):
                x, y = cell_xy(c, row)
                if key == "color":
                    for j in range(3):
                        im.alpha_composite(images["vv_seg%d_%s.png" % (j, "on" if j == k else "off")], (x + 16 + j * (SEG_W + 6), y + 54))
                    continue
                fr = (hash(key) % 40 + 10) if key != "mode" else 0
                src = images["vv_knob.png"] if key != "mode" else images["vv_knob8.png"]
                im.alpha_composite(src.crop((0, fr * KNOB, KNOB, (fr + 1) * KNOB)), (x + 14, y + 22))
                g.text((x + 112 + 90, y + 54 + 23), samples[key], fill=CREAM, font=font(FONT_MED, 22), anchor="mm")
        return im.convert("RGB")
    sheet = Image.new("RGB", (W, H * 2 + 8), (0, 0, 0))
    half = lambda im: im.resize((W // 2 - 4, H // 2), Image.LANCZOS)
    sheet.paste(shot(PAGES[0], 1), (0, 0))
    sheet.paste(half(shot(PAGES[0], 0)), (0, H + 8))
    sheet.paste(half(shot(PAGES[0], 2)), (W // 2 + 4, H + 8))
    sheet.paste(half(shot(PAGES[1], 1)), (0, H + 8 + H // 2 + 8 - 8))
    sheet.paste(half(shot(PAGES[1], 0)), (W // 2 + 4, H + 8 + H // 2))
    sheet.save(path)


def main():
    params = os.path.join(PROJECT, "build", "params.json")
    if not os.path.isfile(params):
        sys.exit("build/params.json is missing: run  python3 tools/build.py plugins/uP2noVerb --target native  first")
    tmp = tempfile.mkdtemp()
    subprocess.check_call([sys.executable, os.path.join(SDK, "tools", "gen_ui.py"), "--params", params,
                           "--ui", os.path.join(PROJECT, "ui.json"), "--out", tmp, "--kit-cache", os.path.join(PROJECT, "build"), "--quiet"])
    tui = json.load(open(os.path.join(tmp, "TUI.json")))
    ql = json.load(open(os.path.join(tmp, "Q-Links.json")))
    shutil.rmtree(tmp)
    pd = tui["pageData"]
    defs = [d for d in pd["componentDefinitions"]["localComponentDefinitions"] if "overlay" in d["key"]]
    pd["componentDefinitions"]["localComponentDefinitions"] = defs + definitions()
    template = pd["tabs"][0]
    pd["tabs"] = [make_tab(template, i, page) for i, page in enumerate(PAGES)]
    ql["Screen Mode Q-Links"]["map"] = [qlinks(i + 1, page) for i, page in enumerate(PAGES)]
    ql["Program Mode Q-Links"] = dict(qlinks(1, PAGES[0])["Q-Links"])

    images = {"vv_knob.png": strip(KNOB_FRAMES), "vv_knob8.png": strip(8)}
    for page in PAGES:
        for pal in PALETTES:
            images["vv_%s_%s.png" % (page[0].lower(), pal["key"])] = background(page, pal)
    for k, name in enumerate(COLOR_NAMES):
        images["vv_seg%d_on.png" % k] = segment(name, True)
        images["vv_seg%d_off.png" % k] = segment(name, False)
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
    print("wrote %s (%d tabs: %s)" % (OUT, len(pd["tabs"]), ", ".join(t["tabName"] for t in pd["tabs"])))


if __name__ == "__main__":
    main()
