"""DX7 algorithm graphs, derived from the operator routing table the engine uses
(msfa/fm_core.cc: per operator in processing order OP6..OP1, input bus, output bus, add, feedback).
Drawing them from the engine's own table guarantees the diagrams match the sound."""
from PIL import Image, ImageDraw, ImageFont

ROUTING = [
    (0xc1, 0x11, 0x11, 0x14, 0x01, 0x14), (0x01, 0x11, 0x11, 0x14, 0xc1, 0x14), (0xc1, 0x11, 0x14, 0x01, 0x11, 0x14),
    (0xc1, 0x11, 0x94, 0x01, 0x11, 0x14), (0xc1, 0x14, 0x01, 0x14, 0x01, 0x14), (0xc1, 0x94, 0x01, 0x14, 0x01, 0x14),
    (0xc1, 0x11, 0x05, 0x14, 0x01, 0x14), (0x01, 0x11, 0xc5, 0x14, 0x01, 0x14), (0x01, 0x11, 0x05, 0x14, 0xc1, 0x14),
    (0x01, 0x05, 0x14, 0xc1, 0x11, 0x14), (0xc1, 0x05, 0x14, 0x01, 0x11, 0x14), (0x01, 0x05, 0x05, 0x14, 0xc1, 0x14),
    (0xc1, 0x05, 0x05, 0x14, 0x01, 0x14), (0xc1, 0x05, 0x11, 0x14, 0x01, 0x14), (0x01, 0x05, 0x11, 0x14, 0xc1, 0x14),
    (0xc1, 0x11, 0x02, 0x25, 0x05, 0x14), (0x01, 0x11, 0x02, 0x25, 0xc5, 0x14), (0x01, 0x11, 0x11, 0xc5, 0x05, 0x14),
    (0xc1, 0x14, 0x14, 0x01, 0x11, 0x14), (0x01, 0x05, 0x14, 0xc1, 0x14, 0x14), (0x01, 0x14, 0x14, 0xc1, 0x14, 0x14),
    (0xc1, 0x14, 0x14, 0x14, 0x01, 0x14), (0xc1, 0x14, 0x14, 0x01, 0x14, 0x04), (0xc1, 0x14, 0x14, 0x14, 0x04, 0x04),
    (0xc1, 0x14, 0x14, 0x04, 0x04, 0x04), (0xc1, 0x05, 0x14, 0x01, 0x14, 0x04), (0x01, 0x05, 0x14, 0xc1, 0x14, 0x04),
    (0x04, 0xc1, 0x11, 0x14, 0x01, 0x14), (0xc1, 0x14, 0x01, 0x14, 0x04, 0x04), (0x04, 0xc1, 0x11, 0x14, 0x04, 0x04),
    (0xc1, 0x14, 0x04, 0x04, 0x04, 0x04), (0xc4, 0x04, 0x04, 0x04, 0x04, 0x04),
]


def graph(alg):
    """Returns (edges [(mod_op, target_op)], carriers [op], feedback (from_op, to_op) or None). Ops are 1..6."""
    flags = ROUTING[alg]
    bus = {1: [], 2: []}
    edges, carriers = [], []
    fb_in = fb_out = None
    for k, f in enumerate(flags):
        op = 6 - k
        inbus, outbus, add = (f >> 4) & 3, f & 3, bool(f & 4)
        if f & 0x40: fb_in = op
        if f & 0x80: fb_out = op
        if inbus:
            edges += [(m, op) for m in bus[inbus]]
        if outbus == 0:
            carriers.append(op)
        else:
            bus[outbus] = bus[outbus] + [op] if add else [op]
    fb = (fb_out, fb_in) if fb_in else None
    return edges, sorted(carriers), fb


def layout(alg):
    """Grid positions {op: (x, level)}: carriers on level 0, each modulator above its first target."""
    edges, carriers, _ = graph(alg)
    targets = {op: sorted(t for m, t in edges if m == op) for op in range(1, 7)}
    children = {op: [] for op in range(1, 7)}
    for op in range(1, 7):
        if targets[op]:
            children[targets[op][0]].append(op)
    for c in children.values():
        c.sort()

    def width(op):
        return max(1, sum(width(c) for c in children[op]))

    pos = {}

    def place(op, x0, level):
        w = width(op)
        pos[op] = (x0 + (w - 1) / 2.0, level)
        x = x0
        for c in children[op]:
            place(c, x, level + 1)
            x += width(c)

    x = 0
    for c in carriers:
        place(c, x, 0)
        x += width(c)
    return pos, x


def draw(alg, w, h, ink=(236, 226, 206, 255), box_fill=(52, 38, 30, 255), carrier_fill=None, font_path=None, line=2, number=True):
    """Draws algorithm `alg` (0-based) into a transparent w x h image."""
    edges, carriers, fb = graph(alg)
    pos, cols = layout(alg)
    levels = max(l for _, l in pos.values()) + 1
    cell_w = w / max(cols, 1)
    cell_h = (h * 0.86) / max(levels, 1)
    bw = min(cell_w * 0.62, cell_h * 0.62, w * 0.2)
    ss = 3
    im = Image.new("RGBA", (w * ss, h * ss), (0, 0, 0, 0))
    g = ImageDraw.Draw(im)
    out_y = h * 0.93

    def centre(op):
        x, l = pos[op]
        return ((x + 0.5) * cell_w, out_y - h * 0.07 - (l + 0.5) * cell_h)

    S = lambda p: (p[0] * ss, p[1] * ss)
    lw = max(1, int(line * ss))
    for m, t in edges:
        (mx, my), (tx, ty) = centre(m), centre(t)
        g.line([S((mx, my + bw / 2)), S((mx, (my + ty) / 2)), S((tx, (my + ty) / 2)), S((tx, ty - bw / 2))], fill=ink, width=lw, joint="curve")
    if carriers:
        xs = [centre(c)[0] for c in carriers]
        for c in carriers:
            cx, cy = centre(c)
            g.line([S((cx, cy + bw / 2)), S((cx, out_y))], fill=ink, width=lw)
        g.line([S((min(xs), out_y)), S((max(xs), out_y))], fill=ink, width=lw)
    if fb:
        a, b = fb
        (ax, ay), (bx, by) = centre(a), centre(b)
        r = bw / 2 + bw * 0.28
        if a == b:                         # self feedback: loop over the top and down the right side
            pts = [(ax, ay - bw / 2), (ax, ay - r), (ax + r, ay - r), (ax + r, ay), (ax + bw / 2, ay)]
        else:                              # loop from the lower op back up to the upper one
            pts = [(ax + bw / 2, ay), (ax + r, ay), (ax + r, by - r), (bx, by - r), (bx, by - bw / 2)]
        g.line([S(p) for p in pts], fill=ink, width=lw, joint="curve")
    try:
        f = ImageFont.truetype(font_path, int(bw * 0.62 * ss)) if font_path else ImageFont.load_default()
    except OSError:
        f = ImageFont.load_default()
    for op in range(1, 7):
        cx, cy = centre(op)
        fill = carrier_fill if (carrier_fill and op in carriers) else box_fill
        g.rectangle([S((cx - bw / 2, cy - bw / 2)), S((cx + bw / 2, cy + bw / 2))], fill=fill, outline=ink, width=lw)
        if number:
            g.text(S((cx, cy)), str(op), fill=ink, font=f, anchor="mm")
    return im.resize((w, h), Image.LANCZOS)
