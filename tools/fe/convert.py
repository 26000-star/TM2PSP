"""Converts the front end's pictures to the small texture files read by src/app/fe_canvas.cpp (FeImage).

Sources: the user's pictures in assets/fe (background, card pictures, button icons) and the DS top-screen artworks
Graphics/2dMaps/common/FrontEnd/Screen/<name>TopBG (their upper part, above the DS title band), resized here once to
the size the menus draw them at (480 x 272, 1:1 on the PSP), so that the PSP keeps them in RAM with no scaling work.

File format (.fet, little endian): "FET1", u16 width, u16 height, u16 stride (= texture width, a power of two),
u16 texture height (a power of two), u8 format (the GE's: 0 = 5650, 2 = 4444, 3 = 8888), 3 zero bytes, then
stride x (height + 1) pixels (the last row repeated once for bilinear filtering at the bottom edge). 5650 / 4444
pixels are u16 with red in the low bits, 8888 is R, G, B, A bytes.

Usage: python -P tools/fe/convert.py [out_dir]   (default build/fe/data)
"""
import os
import struct
import sys

from PIL import Image, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")
sys.path.insert(0, HERE)
import mockup_main  # noqa: E402  (the DS screen reader of the validated mock-up)

W, H = 480, 272
# Picture area of a card (mockup_main.card, unselected 116 x 122 -> 108 x 83, selected 140 x 142 -> 132 x 98).
CARD = (128, 96)
# DS top screens used by the menus (card pictures and the right-hand panel of the list pages).
DS_SCREENS = [
    "SinglePlayerTopBG", "EditorTopBG", "MultiplayerTopBG", "RaceTopBg", "PuzzleTopBG", "PlatformTopBG",
    "WelcomeToQuickRaceTopBG", "MyTracksTopBG", "EnvironmentTopBG", "StadiumTopBG", "IslandTopBG", "SnowTopBG",
    "CoastTopBG", "DifficultySelectTopBG", "SunriseTopBG", "DaytimeTopBG", "SunsetTopBG", "VehicleSelectTopBG",
    "ResultsTopBG", "ProfileTopBG", "OptionsTopScreenBG", "SoundTopBG", "DisplayTopBG", "ControlsTopBG",
    "CreditsTopBG", "DeleteGameDataTopBG", "ShopTopScreenBG", "ShopStadiumTopBG", "ShopIslandTopBG", "ShopSnowTopBG",
    "ShopCoastTopBG", "GameModeTopBG", "MainMenuTopBG",
]


def pow2(n):
    p = 8
    while p < n:
        p *= 2
    return p


def cover(im, w, h):
    s = max(w / im.size[0], h / im.size[1])
    im = im.resize((max(w, int(im.size[0] * s + 0.5)), max(h, int(im.size[1] * s + 0.5))), Image.LANCZOS)
    x, y = (im.size[0] - w) // 2, (im.size[1] - h) // 2
    return im.crop((x, y, x + w, y + h))


def write(path, im, fmt):
    im = im.convert("RGBA")
    w, h = im.size
    stride, th = pow2(w), pow2(h + 1)
    px = list(im.getdata())
    out = bytearray(b"FET1" + struct.pack("<HHHHB3x", w, h, stride, th, fmt))
    for y in range(h + 1):
        row = px[min(y, h - 1) * w:(min(y, h - 1) + 1) * w]
        row += [row[-1]] * (stride - w)
        for r, g, b, a in row:
            if fmt == 0:
                out += struct.pack("<H", (r >> 3) | (g >> 2) << 5 | (b >> 3) << 11)
            elif fmt == 2:
                out += struct.pack("<H", (r >> 4) | (g >> 4) << 4 | (b >> 4) << 8 | (a >> 4) << 12)
            else:
                out += bytes((r, g, b, a))
    with open(path, "wb") as f:
        f.write(out)
    return len(out)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "fe", "data")
    os.makedirs(out, exist_ok=True)
    fe = os.path.join(ROOT, "assets", "fe")
    total = 0
    # Background: covers 480 x 272, softened as the Wii menu's photo (mockup_main).
    bg = cover(Image.open(os.path.join(fe, "background.png")).convert("RGBA"), W, H).filter(ImageFilter.GaussianBlur(1.2))
    total += write(os.path.join(out, "background.fet"), bg, 0)
    for name in ("solo", "track", "multi"):
        total += write(os.path.join(out, name + ".fet"), cover(Image.open(os.path.join(fe, name + ".png")), *CARD), 0)
    # Button icons: 32 pixels high (drawn 20 high on the PSP, 32 on the host's larger window), with their alpha.
    for name in ("shop", "profile", "options"):
        im = Image.open(os.path.join(fe, name + ".png")).convert("RGBA")
        im = im.resize((max(1, im.size[0] * 32 // im.size[1]), 32), Image.LANCZOS)
        total += write(os.path.join(out, name + ".fet"), im, 3)
    for name in DS_SCREENS:
        im = mockup_main.screen(name).crop((0, 0, 256, 128))  # above the DS title band
        total += write(os.path.join(out, name.lower() + ".fet"), im.resize(CARD, Image.LANCZOS), 0)
    print("%d files, %d bytes in %s" % (7 + len(DS_SCREENS), total, os.path.normpath(out)))


if __name__ == "__main__":
    main()
