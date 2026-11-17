#!/usr/bin/env python3
"""Build uP2noDX's skin into plugins/uP2noDX/skins/.

The detail pages come from the SDK generator (ui.json -> gen_ui.py); this script restyles them in
the DX7 colourway (dark brown body, green / orange / maroon membrane colours, cream print) and
groups them onto five function keys with sub-tabs (the MPC has six keys):

    PANEL | OP (OP 1..OP 6) | SCALE (SCL 1..SCL 6) | MOD | VOICE

PANEL is hand-made: the 32-algorithm chart along the top (current one highlighted), a big diagram
of the current algorithm, an LCD-style window and LED, the VOICE and LFO controls, six operator
strips (on/off + level + ratio) and membrane buttons. All algorithm drawings come from the engine's
routing table (skin_tools/algorithms.py), not from Yamaha artwork.

Q-Link banks follow the house rule (one unit per bank of four, one grid column per bank on the
Key 37): PANEL = VOICE | LFO | OP 1-4 levels | OP 5-6 levels.

Run after a native build (it needs build/params.json):

    python3 tools/build.py plugins/uP2noDX --target native
    python3 plugins/uP2noDX/skin_tools/make_skin.py
"""
import json, os, random, shutil, subprocess, sys, tempfile
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import algorithms as ALG

PROJECT = os.path.dirname(HERE)
SDK = os.path.dirname(os.path.dirname(PROJECT))
OUT = os.path.join(PROJECT, "skins")

FONT_B = "/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed-Bold.ttf"
FONT_R = "/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed.ttf"
FONT_LOGO = "/usr/share/fonts/truetype/google-fonts/Poppins-Bold.ttf"

# DX7 colourway (RGB)
BROWN = (54, 40, 32)
BROWN_DARK = (38, 28, 22)
INK = (236, 226, 206)
INK_DIM = (170, 156, 136)
GREEN = (68, 152, 114)
ORANGE = (232, 124, 44)
MAROON = (178, 64, 86)
LCD_BG = (152, 162, 134)
LCD_INK = "ff1e241c"
LED = "ffff3c28"


def argb(rgb, a=255):
    return "%02x%02x%02x%02x" % (a, rgb[0], rgb[1], rgb[2])


def font(path, size):
    try:
        return ImageFont.truetype(path, size)
    except OSError:
        return ImageFont.load_default()


# Parameter indices (src/UP2noDX.cpp)
VOLUME = 0
def OP(op, field): return 1 + (op - 1) * 21 + field
LEVEL, COARSE = 16, 18
VOICE_BASE = 127
ALGO, FDBK, OSYNC, LSPEED, LDELAY, LPMD, LAMD, LSYNC, LWAVE, PMS, TRANSP = (VOICE_BASE + i for i in (8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18))
OP_ON = 146
PERF = 152
POLY, BEND, PORTA_ON = PERF + 0, PERF + 1, PERF + 2
MK1 = PERF + 12

# ---------------------------------------------------------------- PANEL geometry (tab 1280 x 628)
CHART_Y, CHART_H = 6, 112           # 2 rows of 16 small algorithm cells
CELL_W, CELL_H = 77, 52
CHART_X = 24
DIAG = (24, 214, 380, 330)          # big diagram
LCD = (24, 128, 380, 74)
COL_VOICE_X, COL_LFO_X, COL_W = 424, 616, 176
ROW_Y = [158, 262, 366, 470]        # control rows for VOICE / LFO columns
BAR_H = 34
OPS_X0, OPS_DX = 838, 70            # operator strips
OPS_TOP = 136
VBAR = (OPS_TOP + 96, 250)          # level bar y and height
BUTTONS_Y = 556

VOICE_ITEMS = [(ALGO, "ALGORITHM", False), (FDBK, "FEEDBACK", False), (TRANSP, "TRANSPOSE", True), (VOLUME, "VOLUME", False)]
LFO_ITEMS = [(LSPEED, "LFO SPEED", False), (LDELAY, "LFO DELAY", False), (LPMD, "PITCH MOD DEPTH", False), (LAMD, "AMP MOD DEPTH", False)]
BUTTONS = [(POLY, "MONO", MAROON), (PORTA_ON, "PORTA", ORANGE), (OSYNC, "OSC SYNC", GREEN), (MK1, "MK I", MAROON)]


def chart_cell(a):
    return CHART_X + (a % 16) * CELL_W, CHART_Y + (a // 16) * (CELL_H + 4)


def panel_background():
    rnd = random.Random(5)
    bg = Image.new("RGB", (1280, 628), BROWN)
    px = bg.load()
    for y in range(628):                            # faint texture
        for x in range(1280):
            v = rnd.uniform(-2.0, 2.0)
            r, g, b = BROWN
            px[x, y] = (int(r + v), int(g + v), int(b + v))
    bg = bg.convert("RGBA")
    g = ImageDraw.Draw(bg)
    # algorithm chart band
    g.rectangle([0, 0, 1280, CHART_Y + CHART_H + 6], fill=BROWN_DARK + (255,))
    for a in range(32):
        x, y = chart_cell(a)
        bg.alpha_composite(ALG.draw(a, CELL_W - 10, CELL_H - 12, ink=INK + (255,), box_fill=BROWN_DARK + (255,), line=1.2, number=False), (x + 12, y + 10))
        g.text((x + 3, y + 1), str(a + 1), fill=INK_DIM + (255,), font=font(FONT_B, 11))
    g.line([(0, CHART_Y + CHART_H + 6), (1280, CHART_Y + CHART_H + 6)], fill=ORANGE + (255,), width=2)
    # LCD window frame + LED box
    x, y, w, h = LCD
    g.rounded_rectangle([x - 4, y - 4, x + w + 4, y + h + 4], radius=6, fill=(20, 16, 14, 255))
    g.rectangle([x, y, x + w - 92, y + h], fill=LCD_BG + (255,))
    g.rectangle([x + w - 84, y, x + w, y + h], fill=(16, 8, 8, 255))
    lcd_f = font(FONT_B, 15)
    g.text((x + 10, y + 12), "ALGORITHM", fill=(30, 36, 28, 255), font=lcd_f)
    g.text((x + 10, y + 42), "FEEDBACK", fill=(30, 36, 28, 255), font=lcd_f)
    g.text((x + 150, y + 42), "TRANSPOSE", fill=(30, 36, 28, 255), font=lcd_f)
    # big diagram frame
    x, y, w, h = DIAG
    g.rounded_rectangle([x, y, x + w, y + h], radius=8, outline=INK_DIM + (255,), width=1)
    g.text((x + 10, y + 8), "ALGORITHM", fill=INK_DIM + (255,), font=font(FONT_B, 12))
    # column titles and control labels
    for cx, title, items in ((COL_VOICE_X, "VOICE", VOICE_ITEMS), (COL_LFO_X, "LFO", LFO_ITEMS)):
        g.rectangle([cx, 128, cx + COL_W, 140], fill=(GREEN if title == "VOICE" else ORANGE) + (255,))
        g.text((cx + COL_W / 2, 134), title, fill=BROWN_DARK + (255,), font=font(FONT_B, 11), anchor="mm")
        for (pidx, label, _bi), ry in zip(items, ROW_Y):
            g.text((cx, ry - 6), label, fill=INK + (255,), font=font(FONT_B, 13), anchor="lb")
    # operator strips
    g.rectangle([OPS_X0 - 26, 128, OPS_X0 - 26 + 6 * OPS_DX, 140], fill=MAROON + (255,))
    g.text((OPS_X0 - 26 + 3 * OPS_DX, 134), "OPERATORS", fill=INK + (255,), font=font(FONT_B, 11), anchor="mm")
    for i in range(6):
        cx = OPS_X0 + i * OPS_DX + 9
        g.text((cx, OPS_TOP + 22), "OP %d" % (i + 1), fill=INK + (255,), font=font(FONT_B, 15), anchor="mm")
        g.text((cx, VBAR[0] - 10), "LEVEL", fill=INK_DIM + (255,), font=font(FONT_B, 10), anchor="mm")
        g.text((cx, VBAR[0] + VBAR[1] + 44), "RATIO", fill=INK_DIM + (255,), font=font(FONT_B, 10), anchor="mm")
    # brand
    g.text((COL_VOICE_X, 610), "uP2noDX", fill=INK + (255,), font=font(FONT_LOGO, 20), anchor="lm")
    g.text((COL_VOICE_X + 110, 611), "6-OPERATOR FM  ·  DX7 VOICE COMPATIBLE", fill=INK_DIM + (255,), font=font(FONT_R, 11), anchor="lm")
    g.text((1256, 611), "uP2noGood", fill=INK_DIM + (255,), font=font(FONT_LOGO, 13), anchor="rm")
    return bg


def diagram_strip():
    x, y, w, h = DIAG
    W, H = w - 40, h - 50
    strip = Image.new("RGBA", (W, H * 32), (0, 0, 0, 0))
    for a in range(32):
        strip.alpha_composite(ALG.draw(a, W, H, ink=INK + (255,), box_fill=BROWN_DARK + (255,), carrier_fill=(70, 110, 86, 255),
                                       font_path=FONT_B, line=2.2), (0, a * H))
    return strip, (x + 20, y + 34, W, H)


def chart_highlight_strip():
    """32 frames the size of the chart band: frame a outlines cell a."""
    W, H = 1280, CHART_Y + CHART_H + 6
    strip = Image.new("RGBA", (W, H * 32), (0, 0, 0, 0))
    g = ImageDraw.Draw(strip)
    for a in range(32):
        x, y = chart_cell(a)
        g.rounded_rectangle([x + 1, a * H + y + 1, x + CELL_W - 2, a * H + y + CELL_H - 1], radius=4, outline=ORANGE + (255,), width=2)
    return strip


def membrane(w, h, colour, on, text):
    """A flat membrane button: printed colour field, lit edge when on."""
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    g = ImageDraw.Draw(im)
    base = tuple(int(c * (1.0 if on else 0.55)) for c in colour)
    g.rounded_rectangle([0, 0, w - 1, h - 1], radius=4, fill=base + (255,), outline=(INK if on else (24, 18, 14)) + (255,), width=2)
    g.text((w / 2, h / 2), text, fill=(INK if on else INK_DIM) + (255,), font=font(FONT_B, 14), anchor="mm")
    return im


# ---------------------------------------------------------------- skin JSON helpers
def bounds(rect, hw="No", invalid="Show", when="Always"):
    return {"version": 1, "acceptsHWFocus": hw, "showWhenDataModelInvalid": invalid,
            "whenVisible": when, "boundsType": "Absolute", "bounds": rect}


def entry(name, typ, data, rect, remap=None, **kw):
    return {"version": 2, "componentData": {"version": 1, "name": name, "type": typ, "data": data},
            "handle remapping": {"version": 1, "map": remap or []}, "bounds": bounds(rect, **kw)}


def r(x, y, w, h):
    return "%g %g %g %g" % (round(x, 2), round(y, 2), round(w, 2), round(h, 2))


def label(text_type, colour, size, handle="Data", just="horizontallyCentred verticallyCentred", text=None):
    d = {"version": 1, "textStyle": {"version": 1, "font": {"version": 1, "name": "Titillium Web", "style": "Regular", "height": float(size)},
                                     "colour": colour, "justification": just, "case": "Original"},
         "type": text_type, "handleName": handle}
    if text is not None:
        d["text"] = text
    return d


def focus():
    return {"version": 1, "backgroundColour": "14ffffff", "outlineColour": argb(ORANGE), "backgroundInset": 2.0, "outlineThickness": 1.5}


def act(on, handler, extra=""):
    return {"version": 1, "onAction": on, "handler": handler, "handleName": "Data", "additionalData": extra}


def component(actions, children):
    return {"version": 2, "actions": actions,
            "backgroundData": {"version": 1, "focussed": {"version": 1, "colour": "00000000", "image": ""},
                               "unfocussed": {"version": 1, "colour": "00000000", "image": ""}},
            "ignoreMousePresses": False, "disableCoarseDataWheel": False, "componentsData": children}


def colour_bar(direction, colour, bipolar):
    return {"version": 2, "sliderType": "ColourBar", "direction": direction, "backgroundColour": "ff1a1410",
            "outlineColour": argb(INK_DIM), "valueColour": argb(colour), "isBipolar": bipolar, "handleName": "Data"}


def panel_definitions():
    edit = [act("Mouse Down", "Q-Link"), act("Double Click", "Show Overlay", "knob overlay"), act("Enter Pressed", "Show Overlay", "knob overlay")]
    edit_bi = [act("Mouse Down", "Q-Link"), act("Double Click", "Show Overlay", "knob overlay bipolar"), act("Enter Pressed", "Show Overlay", "knob overlay bipolar")]
    defs = []
    for key, colour, bi in (("dxBarGreen", GREEN, False), ("dxBarGreenBi", GREEN, True), ("dxBarOrange", ORANGE, False)):
        defs.append({"key": key, "value": component(edit_bi if bi else edit, [
            entry("focus", "Focus", focus(), r(0, 0, COL_W, BAR_H + 4), when="WhenFocussed"),
            entry("bar", "Slider", colour_bar("Horizontal", colour, bi), r(2, 2, COL_W - 64, BAR_H)),
            entry("value", "Label", label("Value", argb(INK), 20, just="right verticallyCentred"), r(COL_W - 60, 2, 56, BAR_H)),
        ])})
    w, h = 40, VBAR[1]
    defs.append({"key": "dxLevel", "value": component(edit, [
        entry("focus", "Focus", focus(), r(0, 0, w + 16, h + 34), when="WhenFocussed"),
        entry("bar", "Slider", colour_bar("Vertical", MAROON, False), r(8, 4, w, h)),
        entry("value", "Label", label("Value", argb(INK), 18), r(0, h + 8, w + 16, 22)),
    ])})
    for key, img in (("dxOnOff", "pnl_op"), ("dxBtnMono", "pnl_btn_mono"), ("dxBtnPorta", "pnl_btn_porta"), ("dxBtnSync", "pnl_btn_sync"), ("dxBtnMk1", "pnl_btn_mk1")):
        bw, bh = (56, 30) if key == "dxOnOff" else (86, 40)
        defs.append({"key": key, "value": component([act("Mouse Down", "Q-Link")], [
            entry("focus", "Focus", focus(), r(0, 0, bw + 8, bh + 8), when="WhenFocussed"),
            entry("btn", "Button", {"version": 2, "onImage": img + "_on.png", "offImage": img + "_off.png", "buttonId": 1,
                                    "numButtonsInGroup": 1, "handleName": "Data", "gestureBehaviour": "Instant"}, r(4, 4, bw, bh)),
        ])})
    return defs


def panel_tab(template_tab):
    comps = [entry("background", "Image", {"version": 2, "imageType": "Regular", "colour": "0", "image": "panel_bg.png"}, "0 0 1280 628")]
    hl_h = CHART_Y + CHART_H + 6
    comps.append(entry("chart hl", "Image", {"version": 2, "imageType": "FilmStrip", "colour": "0", "image": "chart_hl.png", "numFrames": 32,
                                             "handleName": "Parameter %d" % ALGO}, r(0, 0, 1280, hl_h)))
    _strip, (dx, dy, dw, dh) = DIAGRAM_RECT
    comps.append(entry("diagram", "Image", {"version": 2, "imageType": "FilmStrip", "colour": "0", "image": "pnl_diagram.png", "numFrames": 32,
                                            "handleName": "Parameter %d" % ALGO}, r(dx, dy, dw, dh)))
    # LCD values and LED
    x, y, w, h = LCD
    comps.append(entry("lcd alg", "Label", label("Value", LCD_INK, 22, "Parameter %d" % ALGO, "left verticallyCentred"), r(x + 110, y + 4, 60, 30)))
    comps.append(entry("lcd fb", "Label", label("Value", LCD_INK, 22, "Parameter %d" % FDBK, "left verticallyCentred"), r(x + 110, y + 36, 40, 30)))
    comps.append(entry("lcd tr", "Label", label("Value", LCD_INK, 22, "Parameter %d" % TRANSP, "left verticallyCentred"), r(x + 252, y + 36, 40, 30)))
    comps.append(entry("led", "Label", label("Value", LED, 52, "Parameter %d" % ALGO), r(x + w - 84, y, 84, h)))
    # VOICE and LFO columns
    for cx, items, colour in ((COL_VOICE_X, VOICE_ITEMS, "Green"), (COL_LFO_X, LFO_ITEMS, "Orange")):
        for (pidx, _label, bi), ry in zip(items, ROW_Y):
            typ = "dxBar%s%s" % (colour, "Bi" if bi else "")
            comps.append(entry("p%d" % pidx, typ, {"version": 1, "handleName": "Data"}, r(cx, ry, COL_W, BAR_H + 4),
                               remap=[{"key": "Data", "value": "Parameter %d" % pidx}], hw="Yes", invalid="Hide"))
    # operator strips
    for i in range(6):
        op = i + 1
        cx = OPS_X0 + i * OPS_DX
        comps.append(entry("op%d on" % op, "dxOnOff", {"version": 1, "handleName": "Data"}, r(cx - 23, OPS_TOP + 38, 64, 38),
                           remap=[{"key": "Data", "value": "Parameter %d" % (OP_ON + i)}], hw="Yes", invalid="Hide"))
        comps.append(entry("op%d level" % op, "dxLevel", {"version": 1, "handleName": "Data"}, r(cx - 19, VBAR[0], 56, VBAR[1] + 34),
                           remap=[{"key": "Data", "value": "Parameter %d" % OP(op, LEVEL)}], hw="Yes", invalid="Hide"))
        comps.append(entry("op%d ratio" % op, "Label", label("Value", argb(INK), 17, "Parameter %d" % OP(op, COARSE)), r(cx - 24, VBAR[0] + VBAR[1] + 52, 66, 24)))
    # membrane buttons
    for k, (pidx, _text, _c) in enumerate(BUTTONS):
        typ = ["dxBtnMono", "dxBtnPorta", "dxBtnSync", "dxBtnMk1"][k]
        comps.append(entry("btn %d" % pidx, typ, {"version": 1, "handleName": "Data"}, r(COL_VOICE_X - 4 + k * 92, BUTTONS_Y - 22, 94, 48),
                           remap=[{"key": "Data", "value": "Parameter %d" % pidx}], hw="Yes", invalid="Hide"))
    tab = {}
    for k in template_tab:
        tab[k] = template_tab[k]
    tab["tabName"], tab["fnKeyIndex"], tab["fnKeySubIndex"] = "PANEL", 0, 0
    tab["qlinkBoundsData"] = [r(COL_VOICE_X - 6, 122, COL_W + 12, 404), r(COL_LFO_X - 6, 122, COL_W + 12, 404),
                              r(OPS_X0 - 30, 124, 4 * OPS_DX + 8, 440), r(OPS_X0 - 30 + 4 * OPS_DX + 4, 124, 2 * OPS_DX + 8, 440)]
    root = json.loads(json.dumps(template_tab["componentDefinition"]))
    root["actions"] = []
    root["componentsData"] = comps
    tab["componentDefinition"] = root
    return tab


# House rule: one unit per bank; on the Key 37 a bank is one grid column: knob k = Q-Link 13+g-4k.
PANEL_GROUPS = [
    [p for p, _l, _b in VOICE_ITEMS],
    [p for p, _l, _b in LFO_ITEMS],
    [OP(1, LEVEL), OP(2, LEVEL), OP(3, LEVEL), OP(4, LEVEL)],
    [OP(5, LEVEL), OP(6, LEVEL), -1, -1],
]


def panel_qlinks():
    q = {}
    for g, group in enumerate(PANEL_GROUPS):
        for k, pidx in enumerate(group):
            q["Q-Link %d" % (13 + g - 4 * k)] = pidx
    return {"Tab": 1, "SubTab": 1, "Bank Direction": "Column", "Q-Links": {"Q-Link %d" % i: q["Q-Link %d" % i] for i in range(1, 17)}}


# ---------------------------------------------------------------- restyle generated pages
def knob_frame(size, angle, ss=4):
    import math
    S = size * ss
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    g = ImageDraw.Draw(im)
    c, R = S / 2, S * 0.44
    g.ellipse([c - R, c - R, c + R, c + R], fill=(26, 20, 16, 255), outline=INK_DIM + (255,), width=ss * 2)
    a0 = -150
    # value arc in membrane green
    g.arc([c - R - ss * 3, c - R - ss * 3, c + R + ss * 3, c + R + ss * 3], start=a0 - 90, end=angle - 90, fill=GREEN + (255,), width=ss * 3)
    a = math.radians(angle)
    g.line([(c + math.sin(a) * R * 0.2, c - math.cos(a) * R * 0.2), (c + math.sin(a) * R * 0.92, c - math.cos(a) * R * 0.92)], fill=INK + (255,), width=ss * 3)
    return im.resize((size, size), Image.LANCZOS)


def knob_strip(size, frames):
    st = Image.new("RGBA", (size, size * frames), (0, 0, 0, 0))
    for i in range(frames):
        st.paste(knob_frame(size, -150 + 300 * i / (frames - 1)), (0, i * size))
    return st


def fader_strip(w, h, frames):
    st = Image.new("RGBA", (w, h * frames), (0, 0, 0, 0))
    for i in range(frames):
        t = i / (frames - 1)
        im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        g = ImageDraw.Draw(im)
        ty = h // 2
        g.rectangle([6, ty - 4, w - 7, ty + 4], fill=(20, 15, 12, 255), outline=INK_DIM + (255,))
        tx = 6 + 10 + t * (w - 12 - 30)
        g.rectangle([7, ty - 3, tx, ty + 3], fill=GREEN + (255,))
        g.rectangle([tx - 10, 3, tx + 10, h - 4], fill=INK + (255,), outline=(30, 24, 20, 255))
        g.line([(tx, 6), (tx, h - 7)], fill=(60, 48, 40, 255), width=2)
        st.paste(im, (0, i * h))
    return st


def detail_background():
    bg = Image.new("RGBA", (1280, 628), BROWN + (255,))
    g = ImageDraw.Draw(bg)
    for k, c in enumerate((GREEN, ORANGE, MAROON)):
        g.rectangle([1280 - 3 * 40 + k * 40, 0, 1280 - 3 * 40 + k * 40 + 34, 6], fill=c + (255,))
    return bg


def restyle(defs, tabs, choices):
    for d in defs:
        for c in d["value"].get("componentsData", []):
            cd = c["componentData"]
            if cd["type"] == "Focus":
                cd["data"]["outlineColour"] = argb(ORANGE)
            if cd["type"] == "Label" and cd["data"].get("type") == "Name":
                cd["data"]["textStyle"]["colour"] = argb(INK_DIM)
    # print the choice names on the generator's (blank) segment switches
    users = {}
    for t in tabs:
        for c in t["componentDefinition"]["componentsData"]:
            typ, m = c["componentData"]["type"], c["handle remapping"]["map"]
            if typ.startswith("radio") and m:
                users.setdefault(typ, set()).add(tuple(choices.get(int(m[0]["value"].split()[1]), [])))
    for d in defs:
        names = users.get(d["key"])
        if not names or len(names) != 1:
            continue
        ch = list(names.pop())
        for c in [c for c in d["value"]["componentsData"] if c["componentData"]["type"] == "Button"]:
            k = c["componentData"]["data"]["buttonId"]
            if k < len(ch):
                lab = label("Text", argb(INK), 17, handle="Invalid", text=ch[k].upper())
                d["value"]["componentsData"].append(entry("choice %d" % k, "Label", lab, c["bounds"]["bounds"]))
    for t in tabs:
        t["componentDefinition"]["backgroundData"] = {"version": 1, "focussed": {"version": 1, "colour": argb(BROWN), "image": "detail_bg.png"},
                                                      "unfocussed": {"version": 1, "colour": argb(BROWN), "image": "detail_bg.png"}}


def segment(w, h, on, colour):
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    g = ImageDraw.Draw(im)
    base = tuple(int(c * (1.0 if on else 0.45)) for c in colour)
    g.rounded_rectangle([0, 0, w - 1, h - 1], radius=4, fill=base + (255,), outline=(INK if on else (24, 18, 14)) + (255,), width=2)
    return im


DIAGRAM_RECT = None


def main():
    global DIAGRAM_RECT
    params = os.path.join(PROJECT, "build", "params.json")
    if not os.path.isfile(params):
        sys.exit("build/params.json is missing: run  python3 tools/build.py plugins/uP2noDX --target native  first")
    pj = json.load(open(params))
    choices = {i: p["choices"] for i, p in enumerate(pj["params"]) if p.get("choices")}

    tmp = tempfile.mkdtemp()
    subprocess.check_call([sys.executable, os.path.join(SDK, "tools", "gen_ui.py"), "--params", params,
                           "--ui", os.path.join(PROJECT, "ui.json"), "--out", tmp, "--kit-cache", os.path.join(PROJECT, "build"), "--quiet"])
    tui = json.load(open(os.path.join(tmp, "TUI.json")))
    ql = json.load(open(os.path.join(tmp, "Q-Links.json")))
    pd = tui["pageData"]
    defs = pd["componentDefinitions"]["localComponentDefinitions"]
    tabs = pd["tabs"]
    restyle(defs, tabs, choices)

    # group generated pages onto function keys 1..4 with sub-tabs
    key_of = {}
    for t in tabs:
        n = t["tabName"]
        key = 1 if n.startswith("OP ") else 2 if n.startswith("SCL ") else 3 if n == "MOD" else 4
        key_of[n] = key
    sub_count = {}
    old_maps = {m["Tab"]: m for m in ql["Screen Mode Q-Links"]["map"]}
    new_maps = []
    for i, t in enumerate(tabs):
        key = key_of[t["tabName"]]
        sub = sub_count.get(key, 0)
        sub_count[key] = sub + 1
        t["fnKeyIndex"], t["fnKeySubIndex"] = key, sub
        m = dict(old_maps[i + 1])
        m["Tab"], m["SubTab"] = key + 1, sub + 1
        new_maps.append(m)

    DIAGRAM_RECT = diagram_strip()          # (film strip, rect on the PANEL)
    defs += panel_definitions()
    pd["tabs"] = [panel_tab(tabs[0])] + tabs
    ql["Screen Mode Q-Links"]["map"] = [panel_qlinks()] + new_maps
    ql["Program Mode Q-Links"] = dict(panel_qlinks()["Q-Links"])

    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    shutil.copytree(tmp, OUT)
    shutil.rmtree(tmp)
    # restyled kit images (same sizes / frame counts as the generator's)
    for f in os.listdir(OUT):
        p = os.path.join(OUT, f)
        if f.startswith("knob_") and f.endswith(".png"):
            w, h = Image.open(p).size
            knob_strip(w, h // w).save(p)
        elif f.startswith("fader_h") and f.endswith(".png"):
            w, h = Image.open(p).size
            fader_strip(w, h // 127, 127).save(p)
        elif f.startswith("toggle_") and f.endswith(".png"):
            w, h = Image.open(p).size
            segment(w, h, f.endswith("_on.png"), GREEN).save(p)
        elif f.startswith("seg") and f.endswith(".png"):
            w, h = Image.open(p).size
            segment(w, h, f.endswith("_on.png"), ORANGE).save(p)
    detail_background().save(os.path.join(OUT, "detail_bg.png"))
    panel_background().save(os.path.join(OUT, "panel_bg.png"))
    chart_highlight_strip().save(os.path.join(OUT, "chart_hl.png"))
    DIAGRAM_RECT[0].save(os.path.join(OUT, "pnl_diagram.png"))
    for on in (0, 1):
        sfx = "on" if on else "off"
        membrane(56, 30, GREEN, on, "ON" if on else "OFF").save(os.path.join(OUT, "pnl_op_%s.png" % sfx))
        for name, (pidx, text, colour) in zip(("mono", "porta", "sync", "mk1"), BUTTONS):
            membrane(86, 40, colour, on, text).save(os.path.join(OUT, "pnl_btn_%s_%s.png" % (name, sfx)))

    with open(os.path.join(OUT, "TUI.json"), "w") as f:
        json.dump(tui, f, indent=1)
    with open(os.path.join(OUT, "Q-Links.json"), "w") as f:
        json.dump(ql, f, indent=4)
    print("wrote %s (%d tabs: %s)" % (OUT, len(pd["tabs"]), ", ".join("%s[%d.%d]" % (t["tabName"], t["fnKeyIndex"], t["fnKeySubIndex"]) for t in pd["tabs"])))


if __name__ == "__main__":
    main()
