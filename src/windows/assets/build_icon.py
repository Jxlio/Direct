"""Rebuild the Windows icon from the same geometric mark as direct.svg."""
from pathlib import Path
from PIL import Image, ImageDraw

scale = 4
image = Image.new("RGBA", (256 * scale, 256 * scale))
draw = ImageDraw.Draw(image)
def bounds(values):
    return tuple(v * scale for v in values)
draw.rounded_rectangle(bounds((8, 8, 248, 248)), radius=64*scale, fill="#3a67f4")
draw.rounded_rectangle(bounds((66, 70, 196, 170)), radius=16*scale, outline="white", width=12*scale)
draw.polygon([bounds(p) for p in ((107, 164), (107, 200), (148, 164))], fill="white")
draw.polygon([bounds(p) for p in ((119, 158), (119, 174), (137, 158))], fill="#3a67f4")
image = image.resize((256, 256), Image.Resampling.LANCZOS)
image.save(Path(__file__).with_name("direct.ico"), sizes=[(16,16), (24,24), (32,32), (48,48), (64,64), (128,128), (256,256)])
