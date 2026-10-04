#!/usr/bin/env python3
"""Install design/armx360_icon_512.png as the ARMX360 launcher icon.

Run from anywhere:  python3 design/install_icon.py

Requires Pillow. The master artwork is the single source of truth; the mipmap
resources under app/src/main/res/ are generated from it and should never be
edited by hand. Re-running this on a clean tree must leave `git status` empty --
that is the check that the committed artwork really is the icon that shipped.

The supplied PNG is a FINISHED, pre-masked icon: a dark squircle with opaque
white corners (alpha is 255 everywhere, corners included), outlined by a
near-black keyline (3077 px below luminance 8). Using it directly as an adaptive
background would bake four white corners into every launcher mask, so it is
decomposed into real layers instead:

  background  solid #131516, full-bleed and opaque. The launcher applies its own
              mask, which is the point of an adaptive icon -- baking a squircle
              in here double-shapes it.
  foreground  the mark + wordmark only, transparent elsewhere, scaled so its
              CORRISCLED CIRCLE fits the safe zone. A circle (not the bounding
              box) is the binding constraint, because circular masks clip the
              corners and this content is 398x349, not square.
  monochrome  silhouette of the foreground, for themed icons.
  legacy      the squircle itself, corners knocked to transparent, matching the
              alpha convention the existing ic_launcher.webp already uses.
"""
import os

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, ".."))
RES = os.path.join(REPO, "app", "src", "main", "res")
SRC = os.path.join(HERE, "armx360_icon_512.png")

MASTER = 512
DARK = (19, 21, 22)          # sampled from the artwork's field
SAFE = 0.86                  # circumscribed-circle diameter, fraction of 108dp
# 0.86 rather than a timid 0.63: this artwork's content is 398x349, so its
# circumscribed circle is ~1.52x its height. Fitting that into the nominal
# 66/108 safe zone shrinks the mark to a stamp. 0.86 of the 108dp canvas keeps
# the mark filling the mask nicely while the corners of the WORDMARK -- the
# only part near the inscribed circle -- still survive a circular crop.

DENSITIES = {"mdpi": (108, 48), "hdpi": (162, 72), "xhdpi": (216, 96),
             "xxhdpi": (324, 144), "xxxhdpi": (432, 192)}


def lum(p):
    return 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]


def outside_mask(im):
    """Flood fill the white corner region: everything reachable from the border
    without crossing the dark squircle.

    All four borders are seeded, not just pixel (0,0). With a single seed the
    4-neighbour step i-1 at x==0 wraps to the end of the previous row, and the
    fill tunnels along the border and leaks UNDER the squircle -- which then
    marks the dark interior as "outside" and bakes a white halo into the
    foreground. `seen` plus an explicit x/y bounds check keeps every index
    in range and terminates.
    """
    w, h = im.size
    px = im.load()
    out = bytearray(w * h)
    seen = bytearray(w * h)
    stack = []
    for x in range(w):
        stack += [x, (h - 1) * w + x]
    for y in range(h):
        stack += [y * w, y * w + w - 1]
    while stack:
        i = stack.pop()
        if i < 0 or i >= w * h or seen[i]:
            continue
        x, y = i % w, i // w
        # The artwork is outlined by a near-BLACK keyline (3077 px at lum<8,
        # e.g. (0,0,0) at y=5) around a dark #131516 field. Testing for "brighter
        # than 70" stops the fill at that keyline but then classifies the keyline
        # itself as foreground, baking a dark ring into the icon. The only thing
        # that is genuinely outside is the WHITE corner fill, so test for that.
        if lum(px[x, y]) < 200:
            continue
        seen[i] = out[i] = 1
        if x + 1 < w:
            stack.append(i + 1)
        if x:
            stack.append(i - 1)
        if y + 1 < h:
            stack.append(i + w)
        if y:
            stack.append(i - w)
    return out


def content_alpha(p, dmax):
    """Alpha from "distance above the dark field", for GLYPH pixels only.

    Callers must have already excluded the squircle edge (see glyph_mask); this
    maps brightness to coverage for the mark and wordmark. Scaling so bright
    pixels reach full opacity keeps anti-aliased glyph edges smooth and the
    green (max channel 192) solid, which a 255-normalised version would leave
    half-transparent.
    """
    m = max(p)
    if m <= dmax + 6:
        return 0
    return min(255, int(255 * (m - dmax) / (100 - dmax)))


def glyph_mask(im, out):
    """Erode the non-outside region so the squircle's anti-aliased rim is not
    mistaken for glyph.

    The artwork's dark field is bounded by a bright anti-aliased edge (and a
    near-black keyline outside it). Those edge pixels are far brighter than the
    field, so a brightness test alone sweeps them up as "content" and inflates
    the content bbox to x 1..510 -- i.e. the entire canvas, corners included,
    which is what put a white square behind the mark. Dropping a 3px border
    from every non-outside region leaves only the interior, where the field is
    uniform and the glyph is the only bright thing present.
    """
    w, h = im.size
    r = 3
    # Separable erosion: a full 7x7 window per pixel is ~49x the work, and a
    # horizontal pass then a vertical pass gives the identical result.
    notout = bytearray(1 - v for v in out)
    tmp = bytearray(w * h)
    for y in range(h):
        row = y * w
        for x in range(w):
            if x < r or x >= w - r:
                continue
            if all(notout[row + x + dx] for dx in range(-r, r + 1)):
                tmp[row + x] = 1
    inside = bytearray(w * h)
    for y in range(h):
        row = y * w
        if y < r or y >= h - r:
            continue
        for x in range(w):
            if tmp[row + x] and all(tmp[(y + dy) * w + x] for dy in range(-r, r + 1)):
                inside[row + x] = 1
    return inside


def build_masters():
    src = Image.open(SRC).convert("RGB")
    w, h = src.size
    out = outside_mask(src)
    px = src.load()
    dmax = max(DARK)

    bg = Image.new("RGB", (MASTER, MASTER), DARK)

    # Glyph pixels only: interior of the squircle, rim excluded.
    inside = glyph_mask(src, out)

    xs, ys = [], []
    for y in range(h):
        row = y * w
        for x in range(w):
            if not inside[row + x]:
                continue
            p = px[x, y]
            if content_alpha(p, dmax):
                xs.append(x)
                ys.append(y)
    if not xs:
        raise SystemExit("no glyph pixels found -- is the source artwork empty?")

    # Scale so the content's circumscribed circle fits the safe zone.
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
    cw, ch = x1 - x0 + 1, y1 - y0 + 1
    diag = ((cw / ch) ** 2 + 1) ** 0.5
    th = MASTER * SAFE / diag
    tw = th * cw / ch
    crop = src.crop((x0, y0, x1 + 1, y1 + 1))
    ca = Image.new("L", crop.size, 0)
    cp, cap = crop.load(), ca.load()
    for y in range(crop.size[1]):
        for x in range(crop.size[0]):
            # inside[] is indexed in source coordinates; crop starts at (x0,y0).
            if not inside[(y0 + y) * w + (x0 + x)]:
                continue
            cap[x, y] = content_alpha(cp[x, y], dmax)
    art = crop.convert("RGBA")
    art.putalpha(ca)
    art = art.resize((max(1, int(tw)), max(1, int(th))), Image.LANCZOS)

    fg = Image.new("RGBA", (MASTER, MASTER), (0, 0, 0, 0))
    fg.alpha_composite(art, ((MASTER - art.size[0]) // 2,
                             (MASTER - art.size[1]) // 2))

    # Legacy squircle: keep the artwork, knock the corners to transparent.
    legacy = src.convert("RGBA")
    lp = legacy.load()
    for y in range(h):
        for x in range(w):
            if out[y * w + x]:
                lp[x, y] = (255, 255, 255, 0)
    legacy = legacy.resize((MASTER, MASTER), Image.LANCZOS)

    mono = Image.new("RGBA", (MASTER, MASTER), (255, 255, 255, 0))
    mono.putalpha(fg.split()[3])
    return bg, fg, legacy, mono


def rounded_mask(size, rf):
    from PIL import ImageDraw
    m = Image.new("L", (size, size), 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, size - 1, size - 1],
                                        radius=int(size * rf), fill=255)
    return m


def circle_mask(size):
    from PIL import ImageDraw
    m = Image.new("L", (size, size), 0)
    ImageDraw.Draw(m).ellipse([0, 0, size - 1, size - 1], fill=255)
    return m


def save_webp(img, path):
    img.save(path, "WEBP", lossless=True, quality=100, method=6)
    return os.path.getsize(path)


def apply_mask(img, mask):
    """Intersect the artwork's alpha with a launcher shape."""
    img.putalpha(Image.composite(img.split()[3],
                                 Image.new("L", img.size, 0), mask))
    return img


def main():
    if not os.path.isfile(SRC):
        raise SystemExit("missing source: %s" % SRC)
    bg, fg, legacy, mono = build_masters()
    total = 0
    for dens, (a_edge, l_edge) in DENSITIES.items():
        d = os.path.join(RES, "mipmap-" + dens)
        os.makedirs(d, exist_ok=True)
        total += save_webp(bg.resize((a_edge, a_edge), Image.LANCZOS),
                           os.path.join(d, "ic_launcher_background.webp"))
        total += save_webp(fg.resize((a_edge, a_edge), Image.LANCZOS),
                           os.path.join(d, "ic_launcher_foreground.webp"))
        total += save_webp(
            apply_mask(legacy.resize((l_edge, l_edge), Image.LANCZOS),
                       rounded_mask(l_edge, 0.22)),
            os.path.join(d, "ic_launcher.webp"))
        total += save_webp(
            apply_mask(legacy.resize((l_edge, l_edge), Image.LANCZOS),
                       circle_mask(l_edge)),
            os.path.join(d, "ic_launcher_round.webp"))
        print("  mipmap-%-7s %dpx layers" % (dens, a_edge))

    br = os.path.join(REPO, "app", "src", "main", "assets",
                      "ARMX360_foreground.png")
    fg.resize((512, 512), Image.LANCZOS).save(br)
    print("wrote", os.path.relpath(br, REPO))
    print("installed across %d densities (%d bytes)" % (len(DENSITIES), total))


if __name__ == "__main__":
    main()