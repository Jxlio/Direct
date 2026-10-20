"""Bundle upstream SVG renderings as native multi-resolution Windows icons."""
from pathlib import Path
from PIL import Image

root = Path(__file__).with_name("lucide")
lines = ["// Generated from the pinned Lucide SVG files; see LICENSE and REVISION.txt."]
for i in range(10):
    for color in range(4):
        source = root / "generated" / f"{i}-{color}.png"
        image = Image.open(source).convert("RGBA")
        image.save(source.with_suffix(".ico"), sizes=[(n, n) for n in (16, 18, 20, 24, 28, 32, 36, 40, 48, 56, 64, 96, 128)])
        lines.append(f'{100 + i * 4 + color} ICON "assets/lucide/generated/{i}-{color}.ico"')
(root / "icons.rc.inc").write_text("\n".join(lines)+"\n", encoding="utf-8")
