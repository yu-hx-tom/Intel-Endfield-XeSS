"""Rasterize our small vector mark into a multi-size Windows icon; Pillow is a build-only tool."""
from pathlib import Path
from PIL import Image, ImageDraw

root = Path(__file__).resolve().parents[1]
size = 1024
image = Image.new('RGBA', (size, size))
mask = Image.new('L', (size, size))
ImageDraw.Draw(mask).rounded_rectangle((0, 0, size - 1, size - 1), radius=232, fill=255)
pixels = image.load()
for y in range(size):
    for x in range(size):
        weight = (x + y) / (2 * (size - 1))
        pixels[x, y] = tuple(round(a + (b - a) * weight) for a, b in zip((22, 59, 89), (8, 23, 39))) + (255,)
image.putalpha(mask)
draw = ImageDraw.Draw(image)
def points(values):
    return [(x * 4, y * 4) for x, y in values]
draw.line(points([(48, 116), (48, 58), (58, 48), (142, 48)]), fill='#3c849e', width=32, joint='curve')
draw.line(points([(208, 140), (208, 198), (198, 208), (114, 208)]), fill='#3c849e', width=32, joint='curve')
draw.polygon(points([(66, 68), (104, 68), (190, 188), (152, 188)]), fill='#b9f5ff')
draw.polygon(points([(152, 68), (190, 68), (104, 188), (66, 188)]), fill='#4cd9ea')
image = image.resize((256, 256), Image.Resampling.LANCZOS)
image.save(root / 'assets/launcher.png')
image.save(root / 'assets/launcher.ico', sizes=[(n, n) for n in (16, 20, 24, 32, 40, 48, 64, 128, 256)])
