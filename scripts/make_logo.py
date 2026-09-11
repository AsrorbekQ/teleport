"""Converts an RGBA logo PNG into the 1-bit header used by drawImage (1 = white, MSB first,
rows packed; stored rotated 90 degrees like the original Logo120.h)."""
from PIL import Image, ImageOps
import sys
src, out_h, out_png, preview = sys.argv[1:5]
SIZE = 120; INNER = 108
im = Image.open(src).convert('RGBA')
bg = Image.new('RGBA', im.size, (255, 255, 255, 255)); bg.alpha_composite(im)
g = bg.convert('L')
mask = g.point(lambda p: 255 if p < 200 else 0)
box = mask.getbbox()
g = g.crop(box)
w, h = g.size; scale = INNER / max(w, h)
g = g.resize((max(1, round(w * scale)), max(1, round(h * scale))), Image.LANCZOS)
canvas = Image.new('L', (SIZE, SIZE), 255)
canvas.paste(g, ((SIZE - g.width) // 2, (SIZE - g.height) // 2))
bw = canvas.point(lambda p: 255 if p > 150 else 0)
bw.save(preview)
rot = bw.rotate(90, expand=True)  # same storage orientation as the stock logo
rot.convert('RGBA').save(out_png)
px = rot.load(); rowb = SIZE // 8; data = []
for y in range(SIZE):
    for bx in range(rowb):
        b = 0
        for i in range(8):
            if px[bx * 8 + i, y] > 128: b |= 1 << (7 - i)
        data.append(b)
lines = []
for i in range(0, len(data), 19):
    lines.append('    ' + ', '.join(f'0x{b:02x}' for b in data[i:i + 19]) + ',')
open(out_h, 'w').write('#pragma once\n#include <cstdint>\n\n// Image dimensions: 120x120 (Teleport hexagon logo, stored rotated like the stock logo)\nstatic const uint8_t Logo120[] = {\n' + '\n'.join(lines) + '\n};\n')
print('bytes', len(data))
