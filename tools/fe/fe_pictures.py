"""The front end's seven pictures: the user's own in assets/fe (git-ignored), else pictures from the ROM.

The menus draw a background, three card pictures (solo, track, multi) and three button icons (shop, profile,
options). Whoever has other pictures puts them in assets/fe under these names (PNG, any size). Without them the
DS's own artworks and sprites stand in, read from the user's ROM like every other game file:
  background  the DS main menu's top screen (MainMenuTopBG) above its title band, washed toward white
  solo, track, multi  the top screens of the DS's single player, editor and multiplayer menus, above the title band
  shop, profile, options  the front end's sprites: a gold copper, the player's helmet, the DS console icon
TM_FE_ROM_PICTURES=1 in the environment ignores assets/fe (to see what a build without them looks like).

Usage: python -P tools/fe/fe_pictures.py [out_dir]   writes the seven pictures as PNG (default build/fe/pictures)
"""
import os
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools", "assets"))
import mockup_main  # noqa: E402  (the DS screen reader of the validated mock-up)
import nitro2d  # noqa: E402

USER_DIR = os.path.join(ROOT, "assets", "fe")
NAMES = ("background", "solo", "track", "multi", "shop", "profile", "options")
SCREENS = {"background": "MainMenuTopBG", "solo": "SinglePlayerTopBG", "track": "EditorTopBG",
           "multi": "MultiplayerTopBG"}
# (sprite file in Graphics/2dSprites/common/FE/Cell, cell index)
SPRITES = {"shop": ("Coppers", 2), "profile": ("PlayerIcon", 0), "options": ("Wireless", 0)}


def sprite(name, index):
    b = "graphics/2dsprites/common/fe/cell/" + name.lower()
    cells = nitro2d.read_ncer(mockup_main.read(b + ".ncer"))
    px = nitro2d.render_cell(nitro2d.read_ncgr(mockup_main.read(b + ".ncgr")),
                             nitro2d.read_nclr(mockup_main.read("graphics/2dsprites/common/fe/cell/frontend.nclr")),
                             cells["cells"][index], cells["boundary"], cells["srcs"][index])
    im = Image.new("RGBA", (len(px[0]), len(px)))
    im.putdata([p for r in px for p in r])
    im = im.crop(im.getbbox())
    # Pixel art: enlarged by whole steps here, so that the later smooth resize keeps its edges.
    return im.resize((im.size[0] * 4, im.size[1] * 4), Image.NEAREST)


def picture(name):
    """The picture `name` as RGBA: assets/fe/<name>.png when the user has it, else the ROM's."""
    path = os.path.join(USER_DIR, name + ".png")
    if os.path.exists(path) and os.environ.get("TM_FE_ROM_PICTURES") != "1":
        return Image.open(path).convert("RGBA")
    if name in SCREENS:
        im = mockup_main.screen(SCREENS[name]).crop((0, 0, 256, 128))  # above the DS title band
        if name == "background":  # light, as the menus' white panels and grey hints expect
            im = Image.blend(im, Image.new("RGBA", im.size, (255, 255, 255, 255)), 0.5)
        return im
    return sprite(*SPRITES[name])


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "fe", "pictures")
    os.makedirs(out, exist_ok=True)
    for name in NAMES:
        picture(name).save(os.path.join(out, name + ".png"))
    print("%d pictures in %s" % (len(NAMES), os.path.normpath(out)))


if __name__ == "__main__":
    main()
