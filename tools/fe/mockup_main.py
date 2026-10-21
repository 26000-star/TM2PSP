"""Mock-up of the PSP front end's main menu in the TrackMania Wii style (16:9, 480 x 272), drawn from the game's own
assets: the DS fonts (Fonts/Cell/Font*_White), the DS top-screen artworks for the cards, the DS icons. The background
is a file given on the command line (the user's TrackMania United picture); without it a DS artwork stands in.

Usage: python -P tools/fe/mockup_main.py <out.png> [--bg background.png] [--select 0..5] [--scale 3]
"""
import os
import sys
import zipfile

from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
sys.path.insert(0, os.path.join(ROOT, "tools", "assets"))
import nitro2d  # noqa: E402

VFS = zipfile.ZipFile(os.path.join(ROOT, "extract", "files", "vfs.zip"))
NAMES = {n.lower(): n for n in VFS.namelist()}


def read(path):
    return VFS.read(NAMES[path.lower()])


def screen(name):
    b = "graphics/2dmaps/common/frontend/screen/" + name
    rows = nitro2d.render_screen(nitro2d.read_ncgr(read(b + ".ncgr")), nitro2d.read_nclr(read(b + ".nclr")),
                                 nitro2d.read_nscr(read(b + ".nsc")))
    im = Image.new("RGBA", (len(rows[0]), len(rows)))
    im.putdata([p for r in rows for p in r])
    return im


class Font:
    """A DS sprite font: cell i is character 0x21 + i; glyphs keep their OAM offsets from the cell origin."""

    def __init__(self, size):
        b = "graphics/2dsprites/common/fonts/cell/font%d_white" % size
        self.ncgr = nitro2d.read_ncgr(read(b + ".ncgr"))
        self.nclr = nitro2d.read_nclr(read("graphics/2dsprites/common/fonts/cell/font.nclr"))
        self.cells = nitro2d.read_ncer(read(b + ".ncer"))
        self.size = size
        self.cache = {}
        # Top of the capitals relative to the cell origin, and their height: texts are placed by their top.
        self.top = min(self.glyph(c)[2] for c in "AMHd")
        self.cap = self.glyph("M")[0].getbbox()[3] - self.glyph("M")[0].getbbox()[1]

    def glyph(self, ch):
        i = ord(ch) - 0x21
        if i in self.cache:
            return self.cache[i]
        oams = self.cells["cells"][i]
        px = nitro2d.render_cell(self.ncgr, self.nclr, oams, self.cells["boundary"], self.cells["srcs"][i])
        x0 = min(((a1 & 0x1FF) - 512 if (a1 & 0x1FF) >= 256 else a1 & 0x1FF) for _, a1, _ in oams)
        y0 = min(((a0 & 0xFF) - 256 if (a0 & 0xFF) >= 128 else a0 & 0xFF) for a0, _, _ in oams)
        im = Image.new("RGBA", (len(px[0]), len(px)))
        im.putdata([p for r in px for p in r])
        bbox = im.getbbox() or (0, 0, 1, 1)
        g = (im, x0, y0, x0 + bbox[2])
        self.cache[i] = g
        return g

    def width(self, text):
        return sum(self.size // 3 if c == " " else self.glyph(c)[3] + 1 for c in text)

    def draw(self, dst, text, x, y, color=(255, 255, 255)):
        """Draws text with the top of its capitals at y (glyph offsets as the DS draws them)."""
        y -= self.top
        for c in text:
            if c == " ":
                x += self.size // 3
                continue
            im, gx, gy, adv = self.glyph(c)
            tint = Image.new("RGBA", im.size, color + (255,))
            tint.putalpha(im.getchannel("A"))
            dst.alpha_composite(tint, (int(x + gx), int(y + gy)))
            x += adv + 1


def rounded(dst, box, r, fill, outline=None, width=1):
    d = ImageDraw.Draw(dst)
    d.rounded_rectangle(box, r, fill=fill, outline=outline, width=width)


def card(dst, box, art, label, font, selected):
    x0, y0, x1, y1 = box
    shadow = Image.new("RGBA", dst.size, (0, 0, 0, 0))
    rounded(shadow, (x0 + 2, y0 + 3, x1 + 2, y1 + 3), 8, (0, 0, 0, 90))
    dst.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(2)))
    rounded(dst, box, 8, (250, 250, 250, 255), (60, 140, 230, 255) if selected else (200, 200, 205, 255),
            3 if selected else 1)
    ih = int((y1 - y0) * 0.72)
    # Cover the picture area (the user's card pictures, assets/fe).
    tw, th = x1 - x0 - 8, ih - 4
    s = max(tw / art.size[0], th / art.size[1])
    a = art.resize((max(tw, int(art.size[0] * s + 0.5)), max(th, int(art.size[1] * s + 0.5))), Image.LANCZOS)
    a = a.crop(((a.size[0] - tw) // 2, (a.size[1] - th) // 2, (a.size[0] - tw) // 2 + tw, (a.size[1] - th) // 2 + th))
    dst.alpha_composite(a, (x0 + 4, y0 + 4))
    w = font.width(label)
    font.draw(dst, label, (x0 + x1 - w) // 2, y0 + ih + (y1 - y0 - ih - font.cap) // 2, (40, 40, 45))


def button(dst, box, icon, label, font, selected):
    x0, y0, x1, y1 = box
    rounded(dst, box, 7, (250, 250, 250, 255), (60, 140, 230, 255) if selected else (190, 190, 195, 255),
            3 if selected else 1)
    x = x0 + 8
    if icon is not None:
        ic = icon.resize((int(icon.size[0] * (y1 - y0 - 8) / icon.size[1]), y1 - y0 - 8), Image.LANCZOS)
        dst.alpha_composite(ic, (x, y0 + 4))
        x += ic.size[0] + 6
    font.draw(dst, label, x, (y0 + y1 - font.cap) // 2 - 2, (40, 40, 45))


def main():
    args = sys.argv[1:]
    out = args[0]
    opt = lambda k, d: args[args.index(k) + 1] if k in args else d
    sel = int(opt("--select", "0"))
    scale = int(opt("--scale", "1"))
    W, H = 480, 272
    bgpath = opt("--bg", None)
    if bgpath:
        bg = Image.open(bgpath).convert("RGBA")
    else:
        bg = screen("MainMenuTopBG")
    # Cover 480 x 272, softened as the Wii menu's photo.
    s = max(W / bg.size[0], H / bg.size[1])
    bg = bg.resize((int(bg.size[0] * s + 0.5), int(bg.size[1] * s + 0.5)), Image.LANCZOS)
    bg = bg.crop(((bg.size[0] - W) // 2, (bg.size[1] - H) // 2, (bg.size[0] - W) // 2 + W, (bg.size[1] - H) // 2 + H))
    img = bg.filter(ImageFilter.GaussianBlur(1.2))
    f28, f20 = Font(28), Font(20)
    # Title tab.
    rounded(img, (-20, 18, 138, 52), 17, (252, 252, 252, 255), (60, 140, 230, 255), 2)
    f28.draw(img, "Main Menu", 14, (18 + 52 - f28.cap) // 2, (40, 40, 45))
    fe = os.path.join(ROOT, "assets", "fe")
    cards = [("solo", "Single Player"), ("track", "Track Editor"), ("multi", "Multiplayer")]
    xs = [32, 183, 325]
    for i, (art, label) in enumerate(cards):
        x = xs[i]
        box = (x - 6, 62, x + 128 + 6, 204) if i == sel else (x, 72, x + 116, 194)
        card(img, box, Image.open(os.path.join(fe, art + ".png")).convert("RGBA"), label, f20, i == sel)
    icons = {n: Image.open(os.path.join(fe, n + ".png")).convert("RGBA") for n in ("shop", "profile", "options")}
    buttons = [("shop", "Shop"), ("profile", "Profile"), ("options", "Options")]
    for i, (ic, label) in enumerate(buttons):
        x = xs[i] + 4
        button(img, (x, 214, x + 118, 242), icons.get(ic), label, f20, sel == 3 + i)
    # PSP button hints.
    f20.draw(img, "X  OK     O  Back", W - 150, H - 24, (90, 90, 100))
    if scale > 1:
        img = img.resize((W * scale, H * scale), Image.NEAREST)
    img.convert("RGB").save(out)


if __name__ == "__main__":
    main()
