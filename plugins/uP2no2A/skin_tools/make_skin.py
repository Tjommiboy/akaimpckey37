#!/usr/bin/env python3
"""Build uP2no2A's skin into plugins/uP2no2A/skins/.

Two pages drawn like the hardware this plugin is modelled on:

  FRONT  the grey rack faceplate: LIMIT/COMPRESS and POWER toggles on the left, the big GAIN knob,
         the VU meter in the middle, the big PEAK REDUCTION knob, and the meter selector on the right.
  REAR   the back of the chassis, where the original keeps its service controls: the R37 emphasis
         trim and the stereo link, plus an output trim (parameter 0, not on the original).

Run after a native build (it needs build/params.json):

    python3 tools/build.py plugins/uP2no2A --target native
    python3 plugins/uP2no2A/skin_tools/make_skin.py

build.py copies skins/ verbatim into the plugin folder on every build.
Pass --preview <file.png> to also write a picture of both pages with the controls drawn in.
"""
import json, math, os, random, re, shutil, subprocess, sys, tempfile
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
SDK = os.path.dirname(os.path.dirname(PROJECT))
OUT = os.path.join(PROJECT, "skins")

FONT_DIR = "/usr/share/fonts/truetype/dejavu/"
FONT_BOLD = FONT_DIR + "DejaVuSansCondensed-Bold.ttf"
FONT_PLAIN = FONT_DIR + "DejaVuSansCondensed.ttf"
FONT_LOGO = "/usr/share/fonts/truetype/google-fonts/Poppins-Bold.ttf"
MAC_FONTS = ["/System/Library/Fonts/Supplemental/Arial Narrow Bold.ttf", "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
             "/System/Library/Fonts/Helvetica.ttc"]

# Parameter indices (must match ParamIndex in src/UP2no2A.cpp)
P = dict(output=0, peakred=1, gain=2, mode=3, meter=4, emph=5, link=6, power=7, vu=8)

PLATE = (176, 179, 181)        # grey faceplate
INK = (24, 24, 27)             # printed legends
INK_SOFT = (70, 72, 76)
REAR = (58, 60, 64)            # painted chassis
REAR_INK = (222, 220, 212)
FOCUS_ARGB = "ffd0342c"
SWEEP = 150.0                  # knob pointer travels -150 .. +150 degrees from straight up
BIG, BIG_FRAMES = 160, 70      # 160 x 11200 strip
TRIM, TRIM_FRAMES = 90, 64
ROT, ROT_FRAMES, ROT_SWEEP = 84, 3, 50.0
VU, VU_FRAMES, VU_SWEEP = 220, 50, 37.0     # square frames; only the top VU_FACE_H rows are drawn
VU_FACE_H = 140
VU_PIVOT = (110, 186)
VU_TOP = 1.41254               # +3 VU as a voltage ratio = needle full right (same law as the plugin)
TG_W, TG_H = 70, 120
PW_W, PW_H = 70, 190

# FRONT layout (tab area is 1280 x 628)
X_TGL, Y_MODE, Y_POWER = 150, 196, 420
GAIN_C = (366, 330)
PEAK_C = (914, 330)
VU_XY = (530, 222)             # top-left of the meter face
ROT_C = (1128, 330)
# REAR layout
EMPH_C, LINK_C, OUT_C = (430, 310), (640, 310), (850, 310)


def font(path, size):
    for p in [path] + MAC_FONTS:
        try:
            return ImageFont.truetype(p, size)
        except OSError:
            continue
    return ImageFont.load_default()


def text(g, xy, s, size=14, fill=INK, anchor="mm", f=FONT_BOLD):
    g.text(xy, s, fill=fill, font=font(f, size), anchor=anchor)


def polar(c, ang_deg, rad):
    a = math.radians(ang_deg)
    return (c[0] + math.sin(a) * rad, c[1] - math.cos(a) * rad)


# ====================================================================== artwork
def brushed(w, h, base, seed=7, grain=5.0):
    rnd = random.Random(seed)
    im = Image.new("RGB", (w, h), base)
    px = im.load()
    for y in range(h):
        row = rnd.uniform(-grain, grain)
        for x in range(w):
            v = row + rnd.uniform(-grain * 0.5, grain * 0.5)
            px[x, y] = (int(base[0] + v), int(base[1] + v), int(base[2] + v))
    return im.filter(ImageFilter.BoxBlur(0.6)).convert("RGBA")


def screw(g, cx, cy, r=9, slot_deg=30):
    g.ellipse([cx - r - 1, cy - r, cx + r + 1, cy + r + 2], fill=(60, 60, 62))
    g.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(205, 206, 208), outline=(90, 90, 94))
    a, b = polar((cx, cy), slot_deg, r - 1), polar((cx, cy), slot_deg + 180, r - 1)
    g.line([a, b], fill=(70, 70, 74), width=3)


def knob_frame(size, angle_deg, ss=4, slotted=False):
    """Black bakelite knob with a skirt and a white pointer line. slotted = a screwdriver trim pot."""
    S = size * ss
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    c = S / 2
    r_out = S * 0.47
    sh = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(sh).ellipse([c - r_out + ss * 2, c - r_out + ss * 4, c + r_out + ss * 2, c + r_out + ss * 4], fill=(0, 0, 0, 130))
    im.alpha_composite(sh.filter(ImageFilter.GaussianBlur(ss * 2.5)))
    g = ImageDraw.Draw(im)
    if slotted:
        g.ellipse([c - r_out, c - r_out, c + r_out, c + r_out], fill=(120, 122, 126, 255), outline=(30, 30, 32, 255), width=ss)
        r_in = r_out * 0.72
        for k in range(30, 0, -1):
            t = k / 30
            v = int(150 + 80 * (1 - t))
            g.ellipse([c - r_in * t, c - r_in * t, c + r_in * t, c + r_in * t], fill=(v, v, v + 3, 255))
        a = math.radians(angle_deg)
        dx, dy = math.sin(a) * r_in * 0.92, -math.cos(a) * r_in * 0.92
        g.line([(c - dx, c - dy), (c + dx, c + dy)], fill=(30, 30, 32, 255), width=int(ss * 5))
        tip = polar((c, c), angle_deg, r_out * 0.86)
        g.ellipse([tip[0] - ss * 3, tip[1] - ss * 3, tip[0] + ss * 3, tip[1] + ss * 3], fill=(240, 240, 235, 255))
        return im.resize((size, size), Image.LANCZOS)
    # skirt
    g.ellipse([c - r_out, c - r_out, c + r_out, c + r_out], fill=(20, 20, 22, 255))
    g.ellipse([c - r_out, c - r_out, c + r_out, c + r_out], outline=(58, 58, 62, 255), width=ss)
    # raised body with a soft top-left highlight
    r_body = r_out * 0.74
    for k in range(40, 0, -1):
        t = k / 40
        v = int(14 + 44 * (1 - t) ** 1.6)
        off = (1 - t) * r_body * 0.30
        g.ellipse([c - r_body * t - off, c - r_body * t - off, c + r_body * t - off, c + r_body * t - off], fill=(v, v, v + 2, 255))
    g.ellipse([c - r_body, c - r_body, c + r_body, c + r_body], outline=(6, 6, 7, 255), width=ss)
    # fluted grip marks around the body
    for i in range(24):
        a = i * 15 + angle_deg
        g.line([polar((c, c), a, r_body * 0.86), polar((c, c), a, r_body * 0.98)], fill=(46, 46, 50, 255), width=ss)
    # white pointer: across the skirt and onto the body
    g.line([polar((c, c), angle_deg, r_body * 0.30), polar((c, c), angle_deg, r_out * 0.97)], fill=(246, 244, 236, 255), width=int(ss * 3.2))
    return im.resize((size, size), Image.LANCZOS)


def strip(size, frames, sweep, **kw):
    im = Image.new("RGBA", (size, size * frames), (0, 0, 0, 0))
    for i in range(frames):
        t = i / (frames - 1)
        im.paste(knob_frame(size, -sweep + 2 * sweep * t, **kw), (0, i * size))
    return im


def toggle(w, h, up, lamp=None, ss=4):
    """Bat-handle toggle switch. lamp: None, or True/False to draw a jewel lamp above it (lit / dark)."""
    W, H = w * ss, h * ss
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    g = ImageDraw.Draw(im)
    cx = W / 2
    cy = H - 60 * ss if lamp is not None else H / 2
    if lamp is not None:
        ly, lr = 34 * ss, 17 * ss
        g.ellipse([cx - lr - 5 * ss, ly - lr - 5 * ss, cx + lr + 5 * ss, ly + lr + 5 * ss], fill=(200, 201, 204, 255), outline=(80, 80, 84, 255), width=ss)
        if lamp:
            glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
            ImageDraw.Draw(glow).ellipse([cx - lr * 1.5, ly - lr * 1.5, cx + lr * 1.5, ly + lr * 1.5], fill=(255, 60, 40, 170))
            im.alpha_composite(glow.filter(ImageFilter.GaussianBlur(6 * ss)))
            g = ImageDraw.Draw(im)
        col = (236, 52, 40) if lamp else (92, 22, 20)
        for k in range(20, 0, -1):
            t = k / 20
            hl = (1 - t) * (90 if lamp else 40)
            g.ellipse([cx - lr * t - (1 - t) * lr * 0.3, ly - lr * t - (1 - t) * lr * 0.3, cx + lr * t - (1 - t) * lr * 0.3, ly + lr * t - (1 - t) * lr * 0.3],
                      fill=(min(255, int(col[0] + hl)), min(255, int(col[1] + hl)), min(255, int(col[2] + hl)), 255))
    # hex nut and collar
    rn = 22 * ss
    g.regular_polygon((cx, cy, rn), 6, rotation=30, fill=(196, 198, 201, 255), outline=(70, 70, 74, 255))
    g.ellipse([cx - 14 * ss, cy - 14 * ss, cx + 14 * ss, cy + 14 * ss], fill=(120, 122, 126, 255), outline=(60, 60, 64, 255), width=ss)
    g.ellipse([cx - 8 * ss, cy - 8 * ss, cx + 8 * ss, cy + 8 * ss], fill=(30, 30, 32, 255))
    # lever
    d = -1 if up else 1
    tip_y = cy + d * 44 * ss
    sh = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(sh).line([(cx + 4 * ss, cy + 4 * ss), (cx + 5 * ss, tip_y + 5 * ss)], fill=(0, 0, 0, 120), width=12 * ss)
    im.alpha_composite(sh.filter(ImageFilter.GaussianBlur(3 * ss)))
    g = ImageDraw.Draw(im)
    g.polygon([(cx - 5 * ss, cy), (cx + 5 * ss, cy), (cx + 8 * ss, tip_y), (cx - 8 * ss, tip_y)], fill=(214, 216, 219, 255), outline=(90, 90, 94, 255))
    g.ellipse([cx - 9 * ss, tip_y - 9 * ss, cx + 9 * ss, tip_y + 9 * ss], fill=(226, 228, 231, 255), outline=(90, 90, 94, 255), width=ss)
    g.line([(cx - 2 * ss, cy + d * 6 * ss), (cx - 3 * ss, tip_y)], fill=(250, 250, 250, 255), width=2 * ss)
    return im.resize((w, h), Image.LANCZOS)


def vu_angle(pos):
    return -VU_SWEEP + 2 * VU_SWEEP * pos


def vu_pos(db):
    return (10 ** (db / 20.0)) / VU_TOP


def vu_face(ss=3):
    """The meter face (drawn into the background): cream card, VU scale, red zone above 0."""
    W, H = VU * ss, VU_FACE_H * ss
    im = Image.new("RGBA", (W, H), (236, 226, 190, 255))
    g = ImageDraw.Draw(im)
    pv = (VU_PIVOT[0] * ss, VU_PIVOT[1] * ss)
    r0, r1 = 140 * ss, 151 * ss
    box = [pv[0] - r0, pv[1] - r0, pv[0] + r0, pv[1] + r0]
    g.arc(box, 270 + vu_angle(vu_pos(-20)), 270 + vu_angle(vu_pos(0)), fill=(20, 20, 22, 255), width=2 * ss)
    g.arc([pv[0] - r0 - 3 * ss, pv[1] - r0 - 3 * ss, pv[0] + r0 + 3 * ss, pv[1] + r0 + 3 * ss],
          270 + vu_angle(vu_pos(0)), 270 + vu_angle(1.0), fill=(196, 30, 26, 255), width=7 * ss)
    for db, lab in [(-20, "20"), (-10, "10"), (-7, "7"), (-5, "5"), (-3, "3"), (-2, "2"), (-1, "1"), (0, "0"), (1, "1"), (2, "2"), (3, "3")]:
        a = vu_angle(vu_pos(db))
        col = (196, 30, 26, 255) if db > 0 else (20, 20, 22, 255)
        g.line([polar(pv, a, r0), polar(pv, a, r1 + (3 * ss if db in (-20, -10, 0, 3) else 0))], fill=col, width=2 * ss)
        if db not in (-2, 1, 2) or True:
            g.text(polar(pv, a, r1 + 10 * ss), lab, fill=col, font=font(FONT_BOLD, 10 * ss), anchor="mm")
    g.text(polar(pv, vu_angle(vu_pos(-20)) - 7, r1 + 8 * ss), "–", fill=(20, 20, 22, 255), font=font(FONT_BOLD, 13 * ss), anchor="mm")
    g.text(polar(pv, vu_angle(1.0) + 7, r1 + 8 * ss), "+", fill=(196, 30, 26, 255), font=font(FONT_BOLD, 13 * ss), anchor="mm")
    g.text((W / 2, 100 * ss), "VU", fill=(20, 20, 22, 255), font=font(FONT_BOLD, 24 * ss), anchor="mm")
    g.text((W / 2, 122 * ss), "uP2noGood", fill=(110, 100, 80, 255), font=font(FONT_LOGO, 8 * ss), anchor="mm")
    return im.resize((VU, VU_FACE_H), Image.LANCZOS)


def vu_needle_frame(pos, ss=3):
    S = VU * ss
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    pv = (VU_PIVOT[0] * ss, VU_PIVOT[1] * ss)
    a = vu_angle(pos)
    sh = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(sh).line([polar((pv[0] + 3 * ss, pv[1] + 3 * ss), a, 50 * ss), polar((pv[0] + 3 * ss, pv[1] + 3 * ss), a, 158 * ss)], fill=(0, 0, 0, 70), width=3 * ss)
    im.alpha_composite(sh.filter(ImageFilter.GaussianBlur(1.5 * ss)))
    ImageDraw.Draw(im).line([polar(pv, a, 50 * ss), polar(pv, a, 160 * ss)], fill=(14, 14, 16, 255), width=2 * ss)
    im = im.resize((VU, VU), Image.LANCZOS)
    im.paste((0, 0, 0, 0), (0, VU_FACE_H, VU, VU))        # nothing below the face: the pivot is hidden
    return im


def vu_strip():
    im = Image.new("RGBA", (VU, VU * VU_FRAMES), (0, 0, 0, 0))
    for i in range(VU_FRAMES):
        im.paste(vu_needle_frame(i / (VU_FRAMES - 1)), (0, i * VU))
    return im


def scale_ring(g, c, radius, ink=INK, numbers=True, size=13):
    for k in range(51):
        a = -SWEEP + 2 * SWEEP * k / 50
        major = k % 5 == 0
        g.line([polar(c, a, radius), polar(c, a, radius + (11 if major else 6))], fill=ink, width=2 if major else 1)
        if numbers and major:
            text(g, polar(c, a, radius + 25), str(k * 2), size, ink)


def front_background():
    bg = brushed(1280, 628, PLATE)
    g = ImageDraw.Draw(bg)
    # bevelled edge and rack ears
    g.rectangle([0, 0, 1279, 627], outline=(110, 112, 116), width=3)
    g.line([(3, 3), (1276, 3)], fill=(226, 228, 230), width=2)
    for x in (30, 1250):
        for y in (96, 532):
            g.rounded_rectangle([x - 15, y - 9, x + 15, y + 9], radius=9, fill=(22, 22, 24), outline=(96, 98, 102), width=2)
    for x, y in ((70, 34), (1210, 34), (70, 594), (1210, 594)):
        screw(g, x, y, slot_deg=(x + y) % 90)
    # toggles
    text(g, (X_TGL, Y_MODE - 76), "LIMIT", 15)
    text(g, (X_TGL, Y_MODE + 76), "COMPRESS", 15)
    text(g, (X_TGL, Y_POWER + 152), "POWER", 15)
    text(g, (X_TGL + 50, Y_POWER + 28), "ON", 12, INK_SOFT)
    text(g, (X_TGL + 50, Y_POWER + 112), "OFF", 12, INK_SOFT)
    # big knobs
    for c, lab in ((GAIN_C, "GAIN"), (PEAK_C, "PEAK REDUCTION")):
        scale_ring(g, c, BIG / 2 + 8)
        text(g, (c[0], c[1] - BIG / 2 - 72), lab, 22)
    # VU meter: black bezel, face, glass glint
    x, y = VU_XY
    g.rounded_rectangle([x - 22, y - 22, x + VU + 26, y + VU_FACE_H + 34], radius=12, fill=(96, 98, 102))
    g.rounded_rectangle([x - 20, y - 20, x + VU + 20, y + VU_FACE_H + 28], radius=10, fill=(14, 14, 16), outline=(60, 60, 64), width=2)
    bg.paste(vu_face(), (x, y))
    g = ImageDraw.Draw(bg)
    g.rectangle([x - 1, y - 1, x + VU, y + VU_FACE_H], outline=(40, 40, 44), width=2)
    g.ellipse([x + VU / 2 - 5, y + VU_FACE_H + 8, x + VU / 2 + 5, y + VU_FACE_H + 18], fill=(70, 70, 74), outline=(120, 120, 124))
    # name plate under the meter
    text(g, (640, 470), "uP2no2A", 40, INK, f=FONT_LOGO)
    text(g, (640, 512), "LEVELING  AMPLIFIER", 17)
    g.line([(520, 532), (760, 532)], fill=INK, width=2)
    text(g, (640, 552), "ELECTRO-OPTICAL  ·  uP2noGood", 11, INK_SOFT, f=FONT_PLAIN)
    # meter selector legends
    for k, lab in enumerate(("+10", "GAIN\nREDUCTION", "+4")):
        a = -ROT_SWEEP + ROT_SWEEP * k
        g.line([polar(ROT_C, a, ROT / 2 + 4), polar(ROT_C, a, ROT / 2 + 12)], fill=INK, width=2)
        p = polar(ROT_C, a, ROT / 2 + (38 if k == 1 else 30))
        g.multiline_text(p, lab, fill=INK, font=font(FONT_BOLD, 13), anchor="mm", align="center", spacing=2)
    text(g, (ROT_C[0], ROT_C[1] + ROT / 2 + 30), "METER", 15)
    return bg


def rear_background():
    bg = brushed(1280, 628, REAR, seed=19, grain=2.5)
    g = ImageDraw.Draw(bg)
    g.rectangle([0, 0, 1279, 627], outline=(30, 30, 33), width=3)
    for x, y in ((40, 40), (1240, 40), (40, 588), (1240, 588)):
        screw(g, x, y, r=8, slot_deg=(x * 3 + y) % 120)
    text(g, (640, 52), "REAR PANEL", 20, REAR_INK)
    text(g, (640, 80), "SERVICE CONTROLS", 12, (150, 150, 146), f=FONT_PLAIN)
    # decoration: XLR sockets and a barrier strip (not controls)
    for cx, lab in ((170, "INPUT"), (1110, "OUTPUT")):
        g.rounded_rectangle([cx - 62, 248, cx + 62, 372], radius=10, fill=(150, 152, 156), outline=(30, 30, 33), width=2)
        g.ellipse([cx - 46, 264, cx + 46, 356], fill=(18, 18, 20), outline=(90, 90, 94), width=3)
        for a in (-60, 60, 180):
            px, py = polar((cx, 310), a, 22)
            g.ellipse([px - 6, py - 6, px + 6, py + 6], fill=(170, 150, 80) if lab == "INPUT" else (60, 60, 64))
        screw(g, cx - 48, 262, r=5); screw(g, cx + 48, 358, r=5)
        text(g, (cx, 398), lab, 14, REAR_INK)
    g.rounded_rectangle([420, 500, 860, 560], radius=6, fill=(20, 20, 22), outline=(90, 90, 94), width=2)
    for i in range(8):
        x = 448 + i * 55
        g.rectangle([x - 20, 508, x + 20, 552], fill=(34, 34, 37))
        screw(g, x, 530, r=10, slot_deg=17 * i)
        text(g, (x, 574), str(i + 1), 11, (150, 150, 146), f=FONT_PLAIN)
    # control legends
    scale_ring(g, EMPH_C, TRIM / 2 + 6, ink=REAR_INK, numbers=False)
    text(g, (EMPH_C[0], EMPH_C[1] - 100), "HF EMPHASIS", 16, REAR_INK)
    text(g, (EMPH_C[0], EMPH_C[1] - 80), "R37", 11, (150, 150, 146), f=FONT_PLAIN)
    text(g, polar(EMPH_C, -150, TRIM / 2 + 34), "HF", 12, REAR_INK)
    text(g, polar(EMPH_C, 150, TRIM / 2 + 36), "FLAT", 12, REAR_INK)
    text(g, (LINK_C[0], LINK_C[1] - 100), "STEREO LINK", 16, REAR_INK)
    text(g, (LINK_C[0], LINK_C[1] - 72), "ON", 11, (150, 150, 146), f=FONT_PLAIN)
    text(g, (LINK_C[0], LINK_C[1] + 74), "OFF", 11, (150, 150, 146), f=FONT_PLAIN)
    scale_ring(g, OUT_C, TRIM / 2 + 6, ink=REAR_INK, numbers=False)
    text(g, (OUT_C[0], OUT_C[1] - 100), "OUTPUT TRIM", 16, REAR_INK)
    text(g, polar(OUT_C, -150, TRIM / 2 + 34), "–12", 12, REAR_INK)
    text(g, polar(OUT_C, 0, TRIM / 2 + 28), "0", 12, REAR_INK)
    text(g, polar(OUT_C, 150, TRIM / 2 + 34), "+12", 12, REAR_INK)
    text(g, (640, 610), "uP2no2A  ·  uP2noGood", 11, (130, 130, 128), f=FONT_PLAIN)
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


def label_data(colour, size):
    return {"version": 1, "textStyle": {"version": 1, "font": {"version": 1, "name": "Titillium Web", "style": "Regular", "height": float(size)},
                                        "colour": colour, "justification": "horizontallyCentred verticallyCentred", "case": "Original"},
            "type": "Value", "handleName": "Data"}


def focus_data():
    return {"version": 1, "backgroundColour": "10000000", "outlineColour": FOCUS_ARGB, "backgroundInset": 2.0, "outlineThickness": 2.0}


def component(actions, children, ignore_touch=False):
    return {"version": 2, "actions": actions,
            "backgroundData": {"version": 1, "focussed": {"version": 1, "colour": "00000000", "image": ""},
                               "unfocussed": {"version": 1, "colour": "00000000", "image": ""}},
            "ignoreMousePresses": ignore_touch, "disableCoarseDataWheel": False, "componentsData": children}


def act(on, handler, extra=""):
    return {"version": 1, "onAction": on, "handler": handler, "handleName": "Data", "additionalData": extra}


def film(image, frames):
    return {"version": 2, "knobType": "FilmStrip", "filmStrip": image, "numFrames": frames, "invert": False, "handleName": "Data"}


def button(on, off):
    return {"version": 2, "onImage": on, "offImage": off, "buttonId": 1, "numButtonsInGroup": 1, "handleName": "Data", "gestureBehaviour": "Instant"}


PAD = 8


def knob_def(image, size, frames, overlay, value_colour):
    w = size + 2 * PAD
    kids = [entry("focus", "Focus", focus_data(), r(0, 0, w, w), when="WhenFocussed"),
            entry("knob", "Knob", film(image, frames), r(PAD, PAD, size, size))]
    if value_colour:
        kids.append(entry("value", "Label", label_data(value_colour, 20), r(-20, w + 2, w + 40, 24), when="WhenFocussed"))
    return component([act("Mouse Down", "Q-Link"), act("Double Click", "Show Overlay", overlay), act("Enter Pressed", "Show Overlay", overlay)], kids)


def toggle_def(on, off, w, h):
    kids = [entry("focus", "Focus", focus_data(), r(0, 0, w + 2 * PAD, h + 2 * PAD), when="WhenFocussed"),
            entry("switch", "Button", button(on, off), r(PAD, PAD, w, h))]
    return component([act("Mouse Down", "Q-Link")], kids)


def definitions():
    return [
        {"key": "laBigKnob", "value": knob_def("la_knob.png", BIG, BIG_FRAMES, "knob overlay", "ff18181b")},
        {"key": "laTrim", "value": knob_def("la_trim.png", TRIM, TRIM_FRAMES, "knob overlay", "ffdedcd4")},
        {"key": "laTrimBi", "value": knob_def("la_trim.png", TRIM, TRIM_FRAMES, "knob overlay bipolar", "ffdedcd4")},
        {"key": "laRotary", "value": knob_def("la_rotary3.png", ROT, ROT_FRAMES, "menu overlay", None)},
        {"key": "laToggle", "value": toggle_def("la_toggle_up.png", "la_toggle_down.png", TG_W, TG_H)},
        {"key": "laPower", "value": toggle_def("la_power_on.png", "la_power_off.png", PW_W, PW_H)},
        {"key": "laVu", "value": component([], [entry("needle", "Knob", film("la_vu_needle.png", VU_FRAMES), r(0, 0, VU, VU))], ignore_touch=True)},
    ]


def ctl(name, typ, key, cx, cy, w, h, hw="Yes"):
    return entry(name, typ, {"version": 1, "handleName": "Data"}, r(cx - w / 2 - PAD, cy - h / 2 - PAD, w + 2 * PAD, h + 2 * PAD),
                 remap=[{"key": "Data", "value": "Parameter %d" % P[key]}], hw=hw, invalid="Hide")


FRONT = [("mode", "laToggle", "mode", X_TGL, Y_MODE, TG_W, TG_H),
         ("power", "laPower", "power", X_TGL, Y_POWER + 35, PW_W, PW_H),
         ("gain", "laBigKnob", "gain", GAIN_C[0], GAIN_C[1], BIG, BIG),
         ("peak reduction", "laBigKnob", "peakred", PEAK_C[0], PEAK_C[1], BIG, BIG),
         ("meter", "laRotary", "meter", ROT_C[0], ROT_C[1], ROT, ROT)]
REAR_CTL = [("emphasis", "laTrim", "emph", EMPH_C[0], EMPH_C[1], TRIM, TRIM),
            ("stereo link", "laToggle", "link", LINK_C[0], LINK_C[1], TG_W, TG_H),
            ("output", "laTrimBi", "output", OUT_C[0], OUT_C[1], TRIM, TRIM)]

# Q-Links: one bank of four per page (house rule: a bank is one unit; here the front panel, then the
# rear panel). The Key 37 banks by grid columns, knob 1..4 = Q-Link 13, 9, 5, 1.
QL_FRONT = ("gain", "peakred", "mode", "meter")
QL_REAR = ("emph", "link", "output", None)
QL_RECT_FRONT = "96 92 1124 440"
QL_RECT_REAR = "350 180 580 240"


def make_tab(template, name, index, image, controls, rect, extra=()):
    comps = [entry("background", "Image", {"version": 2, "imageType": "Regular", "colour": "0", "image": image}, "0 0 1280 628")]
    comps += list(extra)
    comps += [ctl(*c) for c in controls]
    tab = {k: template[k] for k in template}
    tab["tabName"], tab["fnKeyIndex"], tab["fnKeySubIndex"] = name, index, 0
    tab["qlinkBoundsData"] = [rect]
    root = json.loads(json.dumps(template["componentDefinition"]))
    root["actions"] = []
    root["componentsData"] = comps
    tab["componentDefinition"] = root
    return tab


def qlinks(tab, group):
    q = {"Q-Link %d" % i: -1 for i in range(1, 17)}
    for k, key in enumerate(group):
        if key:
            q["Q-Link %d" % (13 - 4 * k)] = P[key]
    return {"Tab": tab, "SubTab": 1, "Bank Direction": "Column", "Q-Links": q}


def preview(path, images):
    """Both pages with the controls pasted in at typical settings (for a quick look without an MPC)."""
    def frame(img, size, i):
        return img.crop((0, i * size, size, (i + 1) * size))
    front = images["la_front_bg.png"].copy()
    front.alpha_composite(images["la_toggle_down.png"], (X_TGL - TG_W // 2, Y_MODE - TG_H // 2))
    front.alpha_composite(images["la_power_on.png"], (X_TGL - PW_W // 2, Y_POWER + 35 - PW_H // 2))
    front.alpha_composite(frame(images["la_knob.png"], BIG, int(0.40 * (BIG_FRAMES - 1))), (GAIN_C[0] - BIG // 2, GAIN_C[1] - BIG // 2))
    front.alpha_composite(frame(images["la_knob.png"], BIG, int(0.35 * (BIG_FRAMES - 1))), (PEAK_C[0] - BIG // 2, PEAK_C[1] - BIG // 2))
    front.alpha_composite(frame(images["la_rotary3.png"], ROT, 1), (ROT_C[0] - ROT // 2, ROT_C[1] - ROT // 2))
    front.alpha_composite(frame(images["la_vu_needle.png"], VU, int(vu_pos(-4) * (VU_FRAMES - 1))), VU_XY)
    rear = images["la_rear_bg.png"].copy()
    rear.alpha_composite(frame(images["la_trim.png"], TRIM, TRIM_FRAMES - 1), (EMPH_C[0] - TRIM // 2, EMPH_C[1] - TRIM // 2))
    rear.alpha_composite(images["la_toggle_up.png"], (LINK_C[0] - TG_W // 2, LINK_C[1] - TG_H // 2))
    rear.alpha_composite(frame(images["la_trim.png"], TRIM, TRIM_FRAMES // 2), (OUT_C[0] - TRIM // 2, OUT_C[1] - TRIM // 2))
    both = Image.new("RGB", (1280, 628 * 2 + 12), (0, 0, 0))
    both.paste(front.convert("RGB"), (0, 0))
    both.paste(rear.convert("RGB"), (0, 640))
    both.save(path)


def main():
    params = os.path.join(PROJECT, "build", "params.json")
    if not os.path.isfile(params):
        sys.exit("build/params.json is missing: run  python3 tools/build.py plugins/uP2no2A --target native  first")
    tmp = tempfile.mkdtemp()
    subprocess.check_call([sys.executable, os.path.join(SDK, "tools", "gen_ui.py"), "--params", params,
                           "--ui", os.path.join(PROJECT, "ui.json"), "--out", tmp, "--kit-cache", os.path.join(PROJECT, "build"), "--quiet"])
    tui = json.load(open(os.path.join(tmp, "TUI.json")))
    ql = json.load(open(os.path.join(tmp, "Q-Links.json")))
    shutil.rmtree(tmp)
    pd = tui["pageData"]
    # keep only the overlays from the generated definitions; every control on screen is ours
    defs = [d for d in pd["componentDefinitions"]["localComponentDefinitions"] if "overlay" in d["key"]]
    pd["componentDefinitions"]["localComponentDefinitions"] = defs + definitions()
    template = pd["tabs"][0]
    needle = entry("vu needle", "laVu", {"version": 1, "handleName": "Data"}, r(VU_XY[0], VU_XY[1], VU, VU),
                   remap=[{"key": "Data", "value": "Parameter %d" % P["vu"]}], hw="No", invalid="Hide")
    pd["tabs"] = [make_tab(template, "FRONT", 0, "la_front_bg.png", FRONT, QL_RECT_FRONT, extra=[needle]),
                  make_tab(template, "REAR", 1, "la_rear_bg.png", REAR_CTL, QL_RECT_REAR)]
    ql["Screen Mode Q-Links"]["map"] = [qlinks(1, QL_FRONT), qlinks(2, QL_REAR)]
    prog = {"Q-Link %d" % i: -1 for i in range(1, 17)}
    for col, group in enumerate((QL_FRONT, QL_REAR[:3] + ("power",))):
        for k, key in enumerate(group):
            prog["Q-Link %d" % (13 + col - 4 * k)] = P[key]
    ql["Program Mode Q-Links"] = prog

    images = {
        "la_front_bg.png": front_background(),
        "la_rear_bg.png": rear_background(),
        "la_knob.png": strip(BIG, BIG_FRAMES, SWEEP),
        "la_trim.png": strip(TRIM, TRIM_FRAMES, SWEEP, slotted=True),
        "la_rotary3.png": strip(ROT, ROT_FRAMES, ROT_SWEEP),
        "la_vu_needle.png": vu_strip(),
        "la_toggle_up.png": toggle(TG_W, TG_H, True),
        "la_toggle_down.png": toggle(TG_W, TG_H, False),
        "la_power_on.png": toggle(PW_W, PW_H, True, lamp=True),
        "la_power_off.png": toggle(PW_W, PW_H, False, lamp=False),
    }
    os.makedirs(OUT, exist_ok=True)
    for f in os.listdir(OUT):
        if f.endswith(".png") or f.endswith(".json"):
            os.replace(os.path.join(OUT, f), os.path.join(OUT, f))   # overwritten below; nothing is deleted
    for name, im in images.items():
        im.save(os.path.join(OUT, name))
    with open(os.path.join(OUT, "TUI.json"), "w") as f:
        json.dump(tui, f, indent=1)
    with open(os.path.join(OUT, "Q-Links.json"), "w") as f:
        json.dump(ql, f, indent=4)
    used = set(re.findall(r'"([^"]+\.png)"', json.dumps(tui)))
    missing = used - set(images)
    if missing:
        sys.exit("skin refers to images that were not written: %s" % ", ".join(sorted(missing)))
    if "--preview" in sys.argv:
        preview(sys.argv[sys.argv.index("--preview") + 1], images)
    print("wrote %s (%d tabs: %s)" % (OUT, len(pd["tabs"]), ", ".join(t["tabName"] for t in pd["tabs"])))


if __name__ == "__main__":
    main()
