#!/usr/bin/env python3
"""Pin icons A-Z and 0-9: the character (Barlow SemiBold, OFL, the chat log's font) inside a
rounded square drawn like Phosphor's regular icons (256 grid, 16 px stroke).
Writes indra/newview/skins/default/textures/phosphor_authored/{letter,number}-*.svg."""
import os, sys
from fontTools.ttLib import TTFont
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.boundsPen import BoundsPen
from fontTools.pens.transformPen import TransformPen

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
font = TTFont(os.path.join(root, "indra/newview/fontsets/Barlow-SemiBold.ttf"))
out_dir = os.path.join(root, "indra/newview/skins/default/textures/phosphor_authored")
gs = font.getGlyphSet(); cmap = font.getBestCmap()
CAP = 112.0                     # character height in the 256 grid (the frame's inside is 176)
cap_units = font["OS/2"].sCapHeight or 700

def icon(ch, name):
    g = gs[cmap[ord(ch)]]
    bp = BoundsPen(gs); g.draw(bp)
    x0, y0, x1, y1 = bp.bounds
    s = CAP / cap_units
    w, h = (x1 - x0) * s, (y1 - y0) * s
    # font units (y up) -> grid (y down), centred in the frame
    tx = 128 - w / 2 - x0 * s
    ty = 128 + h / 2 + y0 * s
    pen = SVGPathPen(gs)
    g.draw(TransformPen(pen, (s, 0, 0, -s, tx, ty)))
    svg = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256">'
           '<rect x="40" y="40" width="176" height="176" rx="24" fill="none" stroke="currentColor" stroke-width="16"/>'
           '<path fill="currentColor" d="%s"/></svg>\n') % pen.getCommands()
    with open(os.path.join(out_dir, name + ".svg"), "w") as f:
        f.write(svg)

for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ":
    icon(c, "letter-" + c.lower())
for c in "0123456789":
    icon(c, "number-" + c)
print("36 icons in", out_dir)
