#!/usr/bin/env python3
"""Convert the SDL window's cursor BMPs to 32-bit BMPs with a real alpha channel.

LLWindowSDL loads its custom cursors from indra/newview/res-sdl/*.BMP and hands them to
SDL_CreateColorCursor as they are. LL's BMPs had no transparency: the background was
painted 200,200,200 grey and the old loader colour-keyed it. This script turns that key
into alpha once, so the viewer needs no key or pixel fix-up at load time.

    python3 scripts/content_tools/sdl_cursor_alpha.py

Needs Pillow (system python3 has it; the build venv does not).

What it does, for every cursor LLWindowSDL::initCursors() loads:
  * a cursor whose res/*.cur original is 32-bit (sit, buy, open) is taken from that .cur,
    which keeps its soft drop shadow (the BMP copies had lost it);
  * any other BMP: 200,200,200 -> transparent; colours are kept as they are;
  * transparent pixels are written black: Windows mixes a cursor's colour into the screen
    even where alpha is 0, so anything else shows as a box around the cursor.

Safe to re-run: a BMP that already has alpha is kept as it is.
"""

import os
import re
import struct
import sys

from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
RES_SDL = os.path.join(ROOT, 'indra', 'newview', 'res-sdl')
RES_CUR = os.path.join(ROOT, 'indra', 'newview', 'res')
LOADER = os.path.join(ROOT, 'indra', 'llwindow', 'llwindowsdl.cpp')

COLOUR_KEY = (200, 200, 200)


def cursor_files():
    """The BMPs initCursors() actually loads."""
    with open(LOADER) as f:
        return sorted(set(re.findall(r'makeSDLCursorFromBMP\("([^"]+)"', f.read())))


def cur_bpp(path):
    with open(path, 'rb') as f:
        data = f.read()
    offset = struct.unpack('<I', data[18:22])[0]
    return struct.unpack('<H', data[offset + 14:offset + 16])[0]


def load_rgba(name):
    base = os.path.splitext(name)[0]
    cur = os.path.join(RES_CUR, base + '.cur')
    if os.path.exists(cur) and cur_bpp(cur) == 32:
        return Image.open(cur).convert('RGBA'), os.path.relpath(cur, ROOT)

    path = os.path.join(RES_SDL, name)
    image = Image.open(path)
    if image.mode == 'RGBA':
        return image, 'already has alpha'
    image = image.convert('RGBA')
    pixels = image.load()
    for y in range(image.height):
        for x in range(image.width):
            if pixels[x, y][:3] == COLOUR_KEY:
                pixels[x, y] = (0, 0, 0, 0)
    return image, 'colour key'


def write_bmp(path, image):
    """32-bit BMP with a V5 header and an alpha mask, which SDL_LoadBMP reads as RGBA."""
    width, height = image.size
    pixels = bytearray()
    for y in range(height - 1, -1, -1):  # BMP rows are bottom-up
        for x in range(width):
            r, g, b, a = image.getpixel((x, y))
            if a == 0:
                r = g = b = 0
            pixels += bytes((b, g, r, a))

    header = struct.pack('<IiiHHIIiiII', 124, width, height, 1, 32, 3,  # BI_BITFIELDS
                         len(pixels), 2835, 2835, 0, 0)
    header += struct.pack('<IIII', 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    header += b'BGRs' + bytes(36) + struct.pack('<III', 0, 0, 0)  # sRGB, no endpoints/gamma
    header += struct.pack('<IIII', 4, 0, 0, 0)  # LCS_GM_IMAGES, no profile
    offset = 14 + len(header)
    with open(path, 'wb') as f:
        f.write(b'BM' + struct.pack('<IHHI', offset + len(pixels), 0, 0, offset))
        f.write(header)
        f.write(pixels)


def main():
    names = cursor_files()
    if not names:
        sys.exit('no makeSDLCursorFromBMP() calls found in ' + LOADER)
    for name in names:
        image, source = load_rgba(name)
        write_bmp(os.path.join(RES_SDL, name), image)
        print(f'{name:36} {image.size[0]}x{image.size[1]}  from {source}')


if __name__ == '__main__':
    main()
