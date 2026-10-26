"""Builds direct.ico: the Murmur petals (the logo of the interface) in coral on an ink tile.

    python src/windows/assets/build_icon.py

The geometry is the one of the "logo-petals" symbol in ui/index.html (a 32 x 32 grid); change both together.
"""
from pathlib import Path
from PIL import Image, ImageDraw

INK, CORAL = (11, 12, 14, 255), (255, 91, 61)
BIG = 1024
SIZES = [256, 128, 64, 48, 32, 24, 16]
here = Path(__file__).parent


def petal(x, y, w, h, rx, angle, pivot, k, ox, oy, alpha):
    layer = Image.new('RGBA', (BIG, BIG), (0, 0, 0, 0))
    ImageDraw.Draw(layer).rounded_rectangle(
        [ox + x * k, oy + y * k, ox + (x + w) * k, oy + (y + h) * k], radius=rx * k, fill=CORAL + (round(255 * alpha),))
    # SVG rotate(-24) turns counter-clockwise on screen, which is a positive angle in Pillow
    return layer.rotate(angle, center=(ox + pivot[0] * k, oy + pivot[1] * k), resample=Image.BICUBIC)


def render():
    tile = Image.new('RGBA', (BIG, BIG), (0, 0, 0, 0))
    ImageDraw.Draw(tile).rounded_rectangle([0, 0, BIG - 1, BIG - 1], radius=int(BIG * 0.22), fill=INK)
    k = BIG * 0.64 / 32                    # the 32-unit logo takes 64 % of the tile
    ox = oy = (BIG - 32 * k) / 2
    tile.alpha_composite(petal(2.5, 4, 15, 23, 7.5, 24, (10, 15.5), k, ox, oy, 1.0))
    tile.alpha_composite(petal(15, 8, 14, 20, 7, 24, (22, 18), k, ox, oy, 0.8))
    return tile


if __name__ == '__main__':
    big = render()
    big.save(here / 'direct.ico', sizes=[(s, s) for s in SIZES])
    big.resize((256, 256), Image.LANCZOS).save(here / 'direct-preview.png')
    (here / 'direct.svg').write_text(
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32"><rect width="32" height="32" rx="7" fill="#0b0c0e"/>'
        '<g transform="translate(16 16) scale(.64) translate(-16 -16)" fill="#ff5b3d">'
        '<rect x="2.5" y="4" width="15" height="23" rx="7.5" transform="rotate(-24 10 15.5)"/>'
        '<rect x="15" y="8" width="14" height="20" rx="7" transform="rotate(-24 22 18)" opacity=".8"/></g></svg>\n', encoding='utf-8')
    print('written', here / 'direct.ico')
