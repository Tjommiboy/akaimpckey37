#!/usr/bin/env python3
"""Build Contour EQ's hand-written skin into plugins/Contour-EQ/skins/.

The band and setup pages come from the SDK generator (ui.json -> gen_ui.py). This script then
adds the two graph pages, which the generator cannot make:

  GRAPH : EQ-response bars (160 or 80, bipolar, 0 dB in the middle) over dim analyzer bars (80 or
          40), one draggable dot per band (XYPad: x = Freq, y = Gain), and four band buttons. The
          bar sets switch with the Bars / Ana Bars settings (IndexedEnabling visibility).

Run after a native build (it needs build/params.json):

    python3 tools/build.py plugins/Contour-EQ --target native
    python3 plugins/Contour-EQ/skin_tools/make_skin.py

build.py then copies skins/ verbatim into the plugin folder on every build.
"""
import json, os, shutil, subprocess, sys, tempfile
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
SDK = os.path.dirname(os.path.dirname(PROJECT))
OUT = os.path.join(PROJECT, "skins")

N_BANDS, N_BARS = 4, 160
FIRST_EQ = 29                      # parameter index of the first EQ bar (see ContourEQ.cpp)
FIRST_SP = FIRST_EQ + N_BARS
BARS_P = FIRST_SP + N_BARS          # "Bars" (EQ resolution: 160 / 80)
ANA_P = BARS_P + 1                  # "Ana Bars" (analyzer resolution: 80 / 40)
DISP_P = BARS_P + 2                 # "Display" (Smooth / Fast)
ANALYZER_P = 25                     # "Analyzer" (Off / Pre / Post)
# quick selectors in the row under the graph: (label, param, choices, x of first button, button width)
SELECTORS = [("ANALYZER", ANALYZER_P, ["OFF", "PRE", "POST"], 640, 92), ("DISPLAY", DISP_P, ["SMOOTH", "FAST"], 1020, 110)]
SEL_Y, SEL_H = 436, 36
BAND_BASE, PER_BAND = 1, 6         # Freq, Gain, Q, Shape, Slope, On
F_MIN, F_MAX, DB = 20.0, 20000.0, 18.0

GX, GY, GW, GH = 40, 10, 1200, 400 # graph area in tab coordinates (tab is 1280 x 628)
BAND_COL = ["ffe8735a", "fff2b33d", "ff3fc1a5", "ff9b7bea"]
EQ_COL, SPEC_COL = "ffece7dc", "ff1f3343"
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"


def argb(c):
    v = int(c, 16)
    return ((v >> 16) & 255, (v >> 8) & 255, v & 255, (v >> 24) & 255)


def font(size):
    try:
        return ImageFont.truetype(FONT, size)
    except OSError:
        return ImageFont.load_default()


def x_of(freq):
    import math
    return GX + GW * math.log(freq / F_MIN) / math.log(F_MAX / F_MIN)


def y_of(db):
    return GY + GH * (0.5 - db / (2 * DB))


# ---------------------------------------------------------------- images
def make_images(d):
    bg = Image.new("RGBA", (1280, 628), (24, 24, 27, 255))
    g = ImageDraw.Draw(bg)
    g.rectangle([GX, GY, GX + GW, GY + GH], fill=(15, 16, 19, 255), outline=(60, 62, 68, 255))
    for f in [30, 40, 50, 60, 70, 80, 90, 200, 300, 400, 500, 600, 700, 800, 900, 2000, 3000, 4000, 5000, 6000, 7000, 8000, 9000]:
        x = x_of(f); g.line([x, GY, x, GY + GH], fill=(34, 36, 41, 255))
    for f, lab in [(50, "50"), (100, "100"), (200, "200"), (500, "500"), (1000, "1k"), (2000, "2k"), (5000, "5k"), (10000, "10k")]:
        x = x_of(f); g.line([x, GY, x, GY + GH], fill=(55, 58, 64, 255))
        g.text((x, GY + GH + 4), lab, fill=(140, 142, 150, 255), font=font(16), anchor="mt")
    for db in [-12, -6, 6, 12]:
        y = y_of(db); g.line([GX, y, GX + GW, y], fill=(40, 42, 48, 255))
        g.text((GX + 6, y - 2), "%+d" % db, fill=(110, 112, 120, 255), font=font(14), anchor="lb")
    g.line([GX, y_of(0), GX + GW, y_of(0)], fill=(85, 88, 96, 255), width=1)
    g.text((GX + 6, y_of(0) - 2), "0 dB", fill=(150, 152, 160, 255), font=font(14), anchor="lb")
    for lab, _p, _c, x0, _w in SELECTORS:
        g.text((x0 - 12, SEL_Y + SEL_H / 2), lab, fill=(150, 152, 160, 255), font=font(16), anchor="rm")
    bg.save(os.path.join(d, "graph_bg.png"))
    for _lab, _p, choices, _x0, w in SELECTORS:
        for c in choices:
            for on in (0, 1):
                im = Image.new("RGBA", (w, SEL_H), (0, 0, 0, 0)); g = ImageDraw.Draw(im)
                g.rounded_rectangle([1, 1, w - 2, SEL_H - 2], radius=7, fill=(220, 214, 200, 255) if on else (34, 36, 41, 255),
                                    outline=(120, 122, 130, 255), width=1)
                g.text((w / 2, SEL_H / 2), c, fill=(20, 20, 22, 255) if on else (170, 172, 180, 255), font=font(16), anchor="mm")
                im.save(os.path.join(d, "sel_%s_%s.png" % (c.lower(), "on" if on else "off")))

    for bw in (6, 13):
        Image.new("RGBA", (bw, GH - 2), (0, 0, 0, 0)).save(os.path.join(d, "blank_%d.png" % bw))
        im = Image.new("RGBA", (bw, GH - 2), (0, 0, 0, 0)); g = ImageDraw.Draw(im)
        for y in range(GH - 2):                  # analyzer fill: brighter towards the top
            a = int(70 + 110 * (1 - y / (GH - 2)))
            g.line([0, y, bw - 1, y], fill=(52, 92, 122, a))
        im.save(os.path.join(d, "spec_%d.png" % bw))
    for b in range(N_BANDS):
        col = argb(BAND_COL[b])
        # passive dot
        im = Image.new("RGBA", (34, 34), (0, 0, 0, 0)); g = ImageDraw.Draw(im)
        g.ellipse([2, 2, 31, 31], fill=col, outline=(15, 16, 19, 255), width=2)
        g.text((17, 17), str(b + 1), fill=(15, 16, 19, 255), font=font(18), anchor="mm")
        im.save(os.path.join(d, "dot%d.png" % (b + 1)))
        # selected: ring around the dot
        im = Image.new("RGBA", (54, 54), (0, 0, 0, 0)); g = ImageDraw.Draw(im)
        g.ellipse([2, 2, 51, 51], outline=(255, 255, 255, 255), width=3)
        g.ellipse([12, 12, 41, 41], fill=col, outline=(15, 16, 19, 255), width=2)
        g.text((27, 27), str(b + 1), fill=(15, 16, 19, 255), font=font(18), anchor="mm")
        im.save(os.path.join(d, "dotsel%d.png" % (b + 1)))
        # band select buttons, 248 x 56
        for on in (0, 1):
            im = Image.new("RGBA", (248, 56), (0, 0, 0, 0)); g = ImageDraw.Draw(im)
            fill = col if on else (34, 36, 41, 255)
            g.rounded_rectangle([1, 1, 246, 54], radius=10, fill=fill, outline=col, width=2)
            g.text((124, 28), "BAND %d" % (b + 1), fill=(15, 16, 19, 255) if on else col, font=font(24), anchor="mm")
            im.save(os.path.join(d, "band%d_%s.png" % (b + 1, "on" if on else "off")))


# ---------------------------------------------------------------- skin entries
def bounds(rect, hw="No", invalid="Show", extra=None):
    b = {"version": 2 if extra else 1, "acceptsHWFocus": hw, "showWhenDataModelInvalid": invalid,
         "whenVisible": "Always", "boundsType": "Absolute", "bounds": rect}
    if extra:
        b["additionalInvalidatingHandles"] = extra
    return b


def entry(name, typ, data, rect, **kw):
    return {"version": 2,
            "componentData": {"version": 1, "name": name, "type": typ, "data": data},
            "handle remapping": {"version": 1, "map": []},
            "bounds": bounds(rect, **kw)}


def r(x, y, w, h):
    return "%g %g %g %g" % (round(x, 2), round(y, 2), round(w, 2), round(h, 2))


def meter(peak_img, peak, rms_img, rms, hold, hold_size, hold_colour, blank):
    # Meter widget (ui-reference 6.15): the MPC animates it from a parameter the plugin writes.
    return {"version": 3, "direction": "Up", "invert": False, "inactiveImage": blank,
            "peakImage": peak_img, "rmsImage": rms_img, "peakHandle": peak, "rmsHandle": rms,
            "peakHoldHandle": hold, "peakHoldSize": hold_size, "peakHoldColour": hold_colour}


def colour_bar(handle, colour, bipolar):
    return {"version": 2, "sliderType": "ColourBar", "direction": "Vertical",
            "backgroundColour": "00000000", "outlineColour": "00000000", "valueColour": colour,
            "isBipolar": bipolar, "handleName": handle}


def label(handle, text_type, colour, size, just="horizontallyCentred verticallyCentred", text=None):
    d = {"version": 1, "textStyle": {"version": 1, "font": {"version": 1, "name": "Titillium Web", "style": "Regular", "height": float(size)},
                                     "colour": colour, "justification": just, "case": "Original"},
         "type": text_type, "handleName": handle}
    if text is not None:
        d["text"] = text
    return d


def bar_layer(kind, first, step, colour, bipolar, vis):
    """One set of bars across the graph. Only visible while its resolution setting is chosen."""
    pitch = GW / N_BARS * step
    barw = max(2.0, pitch - 2.0)
    out = []
    for i in range(0, N_BARS, step):
        x = GX + (i // step) * pitch + (pitch - barw) / 2
        out.append(entry("%s %d/%d" % (kind, i, step), "Slider", colour_bar("Parameter %d" % (first + i), colour, bipolar),
                         r(x, GY + 1, barw, GH - 2), invalid="Hide", extra=[vis]))
    return out


def graph_tab(name, template_tab, fn_key):
    comps = [entry("background", "Image", {"version": 2, "imageType": "Regular", "colour": "0", "image": "graph_bg.png"}, "0 0 1280 628")]
    # analyzer behind: 80 or 40 bars ("Ana Bars"), each bound to the first parameter of its group
    comps += bar_layer("spec", FIRST_SP, 2, SPEC_COL, False, "IndexedEnabling/0/2/Parameter %d" % ANA_P)
    comps += bar_layer("spec", FIRST_SP, 4, SPEC_COL, False, "IndexedEnabling/1/2/Parameter %d" % ANA_P)
    # EQ curve in front: 160 or 80 bars ("Bars")
    comps += bar_layer("eq", FIRST_EQ, 1, EQ_COL, True, "IndexedEnabling/0/2/Parameter %d" % BARS_P)
    comps += bar_layer("eq", FIRST_EQ, 2, EQ_COL, True, "IndexedEnabling/1/2/Parameter %d" % BARS_P)
    # dots: XYPads over the whole graph, x = Freq (log), y = Gain. Passive ones first, then the
    # selected band's pad on top; it is the only one that receives touches.
    graph = r(GX, GY, GW, GH)
    for b in range(N_BANDS):
        f, gn = BAND_BASE + b * PER_BAND, BAND_BASE + b * PER_BAND + 1
        comps.append(entry("dot %d" % (b + 1), "XYPad", {"version": 1, "xPositionHandle": "Parameter %d" % f,
                           "yPositionHandle": "Parameter %d" % gn, "image": "dot%d.png" % (b + 1)}, graph))
    for b in range(N_BANDS):
        f, gn = BAND_BASE + b * PER_BAND, BAND_BASE + b * PER_BAND + 1
        comps.append(entry("pad %d" % (b + 1), "XYPad", {"version": 1, "xPositionHandle": "Parameter %d" % f,
                           "yPositionHandle": "Parameter %d" % gn, "image": "dotsel%d.png" % (b + 1)}, graph,
                           hw="Yes", invalid="Hide", extra=["IndexedEnabling/%d/4/Local/4/selBand" % b]))
    # quick selectors: Analyzer Off/Pre/Post and Display Smooth/Fast
    for lab, pidx, choices, x0, w in SELECTORS:
        for k, c in enumerate(choices):
            comps.append(entry("%s %s" % (lab.lower(), c.lower()), "Button", {"version": 2, "onImage": "sel_%s_on.png" % c.lower(),
                               "offImage": "sel_%s_off.png" % c.lower(), "buttonId": k, "numButtonsInGroup": len(choices),
                               "handleName": "Parameter %d" % pidx, "gestureBehaviour": "Instant"}, r(x0 + k * (w + 4), SEL_Y, w, SEL_H), hw="Yes"))
    # band buttons + readouts under the graph, one column per band (= one Q-Link column)
    for b in range(N_BANDS):
        cx = 38 + 320 * b
        comps.append(entry("band %d" % (b + 1), "Button", {"version": 2, "onImage": "band%d_on.png" % (b + 1), "offImage": "band%d_off.png" % (b + 1),
                           "buttonId": b, "numButtonsInGroup": 4, "handleName": "Local/4/selBand", "gestureBehaviour": "Instant"}, r(cx, 482, 248, 56), hw="Yes"))
        base = BAND_BASE + b * PER_BAND
        for k, (pidx, y) in enumerate([(base + 0, 546), (base + 1, 546), (base + 3, 584), (base + 2, 584)]):
            x = cx + (0 if k % 2 == 0 else 124)
            comps.append(entry("val %d %d" % (b + 1, k), "Label", label("Parameter %d" % pidx, "Value", BAND_COL[b], 24), r(x, y, 124, 34)))

    tab = {}
    for key in template_tab:                      # copy the generator's key order exactly
        tab[key] = template_tab[key]
    tab["tabName"], tab["fnKeyIndex"], tab["fnKeySubIndex"] = name, fn_key, 0
    tab["qlinkBoundsData"] = [r(30 + 320 * b, 476, 264, 148) for b in range(N_BANDS)]
    root = json.loads(json.dumps(template_tab["componentDefinition"]))
    root["actions"] = []
    root["componentsData"] = comps
    tab["componentDefinition"] = root
    return tab


def graph_qlinks(tab_no):
    q = {}
    for b in range(N_BANDS):                      # column b = band b: Freq (top), Gain, Q, Shape (bottom)
        base = BAND_BASE + b * PER_BAND
        q["Q-Link %d" % (13 + b)] = base + 0
        q["Q-Link %d" % (9 + b)] = base + 1
        q["Q-Link %d" % (5 + b)] = base + 2
        q["Q-Link %d" % (1 + b)] = base + 3
    return {"Tab": tab_no, "SubTab": 1, "Bank Direction": "Column", "Q-Links": {"Q-Link %d" % i: q["Q-Link %d" % i] for i in range(1, 17)}}


def main():
    params = os.path.join(PROJECT, "build", "params.json")
    if not os.path.isfile(params):
        sys.exit("build/params.json is missing: run  python3 tools/build.py plugins/Contour-EQ --target native  first")
    tmp = tempfile.mkdtemp()
    subprocess.check_call([sys.executable, os.path.join(SDK, "tools", "gen_ui.py"), "--params", params,
                           "--ui", os.path.join(PROJECT, "ui.json"), "--out", tmp, "--kit-cache", os.path.join(PROJECT, "build"), "--quiet"])
    tui = json.load(open(os.path.join(tmp, "TUI.json")))
    ql = json.load(open(os.path.join(tmp, "Q-Links.json")))
    pd = tui["pageData"]
    tabs = pd["tabs"]
    template = tabs[0]
    new_tabs = [graph_tab("GRAPH", template, 0)]
    for t in tabs:
        t["fnKeyIndex"] += 1
    pd["tabs"] = new_tabs + tabs

    for m in ql["Screen Mode Q-Links"]["map"]:
        m["Tab"] += 1
    ql["Screen Mode Q-Links"]["map"] = [graph_qlinks(1)] + ql["Screen Mode Q-Links"]["map"]
    prog = {}
    for b in range(N_BANDS):                      # Program mode knobs: the same band columns
        base = BAND_BASE + b * PER_BAND
        prog[13 + b], prog[9 + b], prog[5 + b], prog[1 + b] = base, base + 1, base + 2, base + 3
    ql["Program Mode Q-Links"] = {"Q-Link %d" % i: prog[i] for i in range(1, 17)}

    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    shutil.copytree(tmp, OUT)
    make_images(OUT)
    with open(os.path.join(OUT, "TUI.json"), "w") as f:
        json.dump(tui, f, indent=1)
    with open(os.path.join(OUT, "Q-Links.json"), "w") as f:
        json.dump(ql, f, indent=4)
    shutil.rmtree(tmp)
    print("wrote %s (%d tabs: %s)" % (OUT, len(pd["tabs"]), ", ".join(t["tabName"] for t in pd["tabs"])))


if __name__ == "__main__":
    main()
