"""The app icon: an original mark, drawn in the palette of the game's art.

The character in the game's key art is Sega's, so this is not that. What it
borrows is the graphic language -- the striped hot-red ground, the jagged
white flash, teal and acid yellow -- around a mark of its own: a spray can
letting go. It reads at 32 pixels, which is the only test an app icon has to
pass.

Writes an .iconset folder; `iconutil -c icns` turns that into AppIcon.icns.
"""
import math, os, sys
from PIL import Image, ImageDraw

RED_A   = (232,  58,  38)
RED_B   = (206,  44,  28)
TEAL    = ( 42, 138, 148)
TEAL_LT = (120, 205, 208)
YELLOW  = (247, 205,  52)
GREEN   = (142, 194,  38)
INK     = ( 24,  24,  26)
WHITE   = (255, 255, 255)

S = 1024                      # master size; everything is in these units
R = int(S * 0.222)            # macOS-ish corner radius


def rot(points, deg, cx, cy):
    a = math.radians(deg)
    ca, sa = math.cos(a), math.sin(a)
    return [(cx + (x - cx) * ca - (y - cy) * sa,
             cy + (x - cx) * sa + (y - cy) * ca) for x, y in points]


def star(cx, cy, r_out, r_in, spikes, rotate=0.0, jitter=()):
    """A jagged flash. Uneven spikes read as drawn rather than generated."""
    pts = []
    for i in range(spikes * 2):
        ang = math.pi * i / spikes + math.radians(rotate)
        r = r_out if i % 2 == 0 else r_in
        if jitter:
            r *= jitter[(i // 2) % len(jitter)] if i % 2 == 0 else 1.0
        pts.append((cx + r * math.cos(ang), cy + r * math.sin(ang)))
    return pts


def draw_master():
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    # --- the striped ground ------------------------------------------------
    ground = Image.new("RGBA", (S, S), RED_A)
    gd = ImageDraw.Draw(ground)
    stripe = S // 26
    for i in range(0, S // stripe + 1):
        if i % 2:
            gd.rectangle([i * stripe, 0, i * stripe + stripe * 0.62, S], fill=RED_B)

    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, S - 1, S - 1], radius=R, fill=255)
    img.paste(ground, (0, 0), mask)

    # --- the burst, up and to the right where the nozzle points ------------
    bx, by = S * 0.655, S * 0.315
    d.polygon(star(bx, by, S * 0.330, S * 0.150, 9, 12, (1.0, 0.78, 1.08, 0.86, 0.96)),
              fill=WHITE)
    d.polygon(star(bx, by, S * 0.255, S * 0.112, 9, 12, (1.0, 0.78, 1.08, 0.86, 0.96)),
              fill=TEAL_LT)
    d.polygon(star(bx, by, S * 0.150, S * 0.062, 9, 30, (1.0, 0.84, 1.04, 0.9, 1.0)),
              fill=YELLOW)

    # loose paint, thrown clear of the burst
    for ox, oy, rr in ((0.860, 0.180, 0.030), (0.800, 0.400, 0.022),
                       (0.905, 0.320, 0.016), (0.560, 0.120, 0.020),
                       (0.700, 0.075, 0.014)):
        d.ellipse([S*ox - S*rr, S*oy - S*rr, S*ox + S*rr, S*oy + S*rr], fill=TEAL_LT)

    # --- the can, tilted, nozzle into the burst ---------------------------
    cx, cy = S * 0.435, S * 0.620          # centre of the can body
    w, h = S * 0.255, S * 0.440            # body, before rotation
    tilt = -38

    def place(pts):
        return rot(pts, tilt, cx, cy)

    def box(x0, y0, x1, y1):
        return place([(x0, y0), (x1, y0), (x1, y1), (x0, y1)])

    L, Rt = cx - w / 2, cx + w / 2
    T, B = cy - h / 2, cy + h / 2
    ink = int(S * 0.026)

    # nozzle stem and cap, above the body
    d.polygon(box(cx - w*0.16, T - S*0.075, cx + w*0.16, T + S*0.01), fill=INK)
    d.polygon(box(cx - w*0.34, T - S*0.135, cx + w*0.34, T - S*0.060), fill=TEAL)
    d.line(place([(cx - w*0.34, T - S*0.135), (cx + w*0.34, T - S*0.135)]),
           fill=INK, width=ink)

    # body
    d.polygon(box(L, T, Rt, B), fill=INK)
    d.polygon(box(L + ink, T + ink, Rt - ink, B - ink), fill=YELLOW)
    # label band
    d.polygon(box(L + ink, cy - h*0.16, Rt - ink, cy + h*0.16), fill=TEAL)
    d.line(place([(L + ink, cy - h*0.16), (Rt - ink, cy - h*0.16)]), fill=INK, width=ink)
    d.line(place([(L + ink, cy + h*0.16), (Rt - ink, cy + h*0.16)]), fill=INK, width=ink)
    # a highlight down one side, so the can has a form
    d.polygon(box(L + ink*1.8, T + ink*2.2, L + ink*3.4, B - ink*2.2), fill=WHITE)
    # base
    d.polygon(box(L, B - ink*2.4, Rt, B), fill=INK)

    # --- a drip, because it is a game about paint --------------------------
    dx, dy = S * 0.200, S * 0.845
    d.ellipse([dx - S*0.048, dy - S*0.048, dx + S*0.048, dy + S*0.048], fill=TEAL)
    d.polygon([(dx - S*0.028, dy - S*0.040),
               (dx + S*0.028, dy - S*0.040),
               (dx, dy - S*0.165)], fill=TEAL)

    # --- keep the rounded silhouette clean --------------------------------
    out = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    out.paste(img, (0, 0), mask)
    ImageDraw.Draw(out).rounded_rectangle([2, 2, S - 3, S - 3], radius=R,
                                          outline=(0, 0, 0, 70), width=4)
    return out


def main(dest):
    master = draw_master()
    os.makedirs(dest, exist_ok=True)
    master.save(os.path.join(dest, "icon_512x512@2x.png"))
    for px in (16, 32, 128, 256, 512):
        for scale in (1, 2):
            n = px * scale
            name = f"icon_{px}x{px}{'@2x' if scale == 2 else ''}.png"
            master.resize((n, n), Image.LANCZOS).save(os.path.join(dest, name))
    master.resize((256, 256), Image.LANCZOS).save(
        os.path.join(os.path.dirname(dest) or ".", "icon-preview.png"))
    print("wrote", dest)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "AppIcon.iconset")
