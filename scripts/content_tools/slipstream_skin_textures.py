#!/usr/bin/env python3
"""Regenerate the Slipstream skin's recoloured textures.

The Slipstream skin (indra/newview/skins/slipstream) started as a copy of the Alchemy
skin. This script rebuilds its textures from the *Alchemy* and *default* skin sources,
so it is safe to re-run after tweaking a colour below (it never reads its own output).

    python3 scripts/content_tools/slipstream_skin_textures.py

Needs Pillow (system python3 has it; the build venv does not).

What it does:
  * grey Alchemy chrome (window, button, tab, field, scroll textures) -> the same
    luminance in a slightly blue-tinted neutral, so Alchemy grey 35 lands exactly on the
    website panel colour #141B27;
  * Alchemy's cyan accent (window icons, build tools, movement/voice/nav glyphs) -> the
    logo blue, keeping each pixel's shading;
  * focused floater title band, active tab, checkbox/radio/slider fills -> logo blue;
  * neutral PushButton_Default_* textures that LLButton tints with ButtonDefaultImageColor
    (purple) for a panel's default/Enter button;
  * Log In button and the login progress bar -> the blue->purple brand gradient.

Keep the colour constants in sync with the palette block at the top of
skins/slipstream/colors.xml.
"""

import colorsys
import os
import shutil
import sys

from PIL import Image

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SKINS = os.path.join(ROOT, "indra", "newview", "skins")
ALCHEMY = os.path.join(SKINS, "alchemy", "textures")
DEFAULT = os.path.join(SKINS, "default", "textures")
OUT = os.path.join(SKINS, "slipstream", "textures")

# --- palette (mirror of colors.xml) -------------------------------------------------
BAR = (0x1C, 0x2A, 0x44)          # SlipstreamBar
BLUE = (0x99, 0xBA, 0xEA)         # SlipstreamBlue (logo)
BLUE_DEEP = (0x2B, 0x48, 0x77)    # SlipstreamBlueDeep (fills carrying light text)
PURPLE = (0xA5, 0x79, 0xD6)       # SlipstreamPurple (logo)
PURPLE_DEEP = (0x6E, 0x48, 0xA8)  # SlipstreamPurpleDeep
BG_DEEP = (0x0B, 0x0F, 0x16)      # SlipstreamBgDeep (website background)
GRAD_BLUE = (0x34, 0x55, 0x92)    # left end of the Log In gradient (white text >= 4.5:1)

NEUTRAL_HUE = 218.0 / 360.0       # hue of the website neutrals (#0B0F16, #141B27)


def clamp8(x):
    return max(0, min(255, int(round(x))))


def mix(a, b, t):
    return tuple(clamp8(x + (y - x) * t) for x, y in zip(a, b))


def scale(c, k):
    return tuple(clamp8(x * k) for x in c)


def neutral(v):
    """Grey level 0..255 -> blue-tinted neutral of slightly lower lightness."""
    lum = (v / 255.0) * 0.845
    sat = 0.32 if lum < 0.25 else max(0.18, 0.32 - (lum - 0.25) * 0.56)
    r, g, b = colorsys.hls_to_rgb(NEUTRAL_HUE, lum, sat)
    return clamp8(r * 255), clamp8(g * 255), clamp8(b * 255)


def blue_ramp(t):
    """Shading 0..~1.2 -> black .. deep blue .. logo blue .. near white."""
    stops = [(0.0, (0, 0, 0)), (0.55, (0x3A, 0x5C, 0x9C)), (1.0, BLUE), (1.2, (0xDD, 0xE8, 0xF8))]
    t = max(0.0, min(stops[-1][0], t))
    for (t0, c0), (t1, c1) in zip(stops, stops[1:]):
        if t <= t1:
            return mix(c0, c1, (t - t0) / (t1 - t0))
    return stops[-1][1]


def load(path):
    return Image.open(path).convert("RGBA")


def save(img, rel):
    dst = os.path.join(OUT, rel)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    img.save(dst, optimize=True)


def grey_of(px):
    return (px[0] + px[1] + px[2]) / 3.0


def is_grey(px):
    return max(px[:3]) - min(px[:3]) <= 6


def tint_neutral(img):
    out = img.copy()
    p = out.load()
    for y in range(out.size[1]):
        for x in range(out.size[0]):
            r, g, b, a = p[x, y]
            if a and is_grey((r, g, b)):
                p[x, y] = neutral(grey_of((r, g, b))) + (a,)
    return out


def rehue_cyan(img):
    """Alchemy cyan (0,161,224) family -> logo blue, per pixel, keeping shading."""
    out = img.copy()
    p = out.load()
    for y in range(out.size[1]):
        for x in range(out.size[0]):
            r, g, b, a = p[x, y]
            if not a:
                continue
            h, s, v = colorsys.rgb_to_hsv(r / 255.0, g / 255.0, b / 255.0)
            if s < 0.25 or not (175.0 / 360.0 <= h <= 215.0 / 360.0):
                continue
            target = blue_ramp(v / (224.0 / 255.0))
            # partially saturated (anti-aliased) pixels keep some of their grey
            grey = neutral(grey_of((r, g, b)))
            p[x, y] = mix(grey, target, min(1.0, s / 0.6)) + (a,)
    return out


def chrome_candidate(rel, img):
    base = os.path.basename(rel)
    if not rel.startswith(("containers/", "navbar/NavBar_BG", "widgets/", "windows/")):
        return False
    if base.startswith(("Arrow_", "DisclosureArrow", "Badge_Border")):
        return False
    px = [q for q in img.getdata() if q[3] > 128]
    if not px:
        return True  # fully transparent slices: harmless either way
    sat = max(max(q[:3]) - min(q[:3]) for q in px)
    avg = sum(grey_of(q) for q in px) / len(px)
    return sat <= 6 and avg < 140


SPECIAL = {
    "windows/Window_Foreground.png",
    "widgets/Checkbox_On.png",
    "containers/TabTop_Left_Selected.png",
    "containers/TabTop_Middle_Selected.png",
    "containers/TabTop_Right_Selected.png",
    "widgets/SliderThumb_Off.png",
    "widgets/SliderThumb_Press.png",
}
SKIP_REHUE_PREFIX = ("icons/Parcel_",)  # maturity / parcel status icons keep their meaning


def recolour_alchemy_copies():
    count = 0
    for dirpath, _, files in os.walk(ALCHEMY):
        for name in files:
            if not name.endswith(".png"):
                continue
            src = os.path.join(dirpath, name)
            rel = os.path.relpath(src, ALCHEMY).replace(os.sep, "/")
            if rel in SPECIAL:
                continue
            img = load(src)
            if chrome_candidate(rel, img):
                img = tint_neutral(img)
            elif not rel.startswith(SKIP_REHUE_PREFIX):
                img = rehue_cyan(img)
            save(img, rel)
            count += 1
    return count


def window_foreground():
    """Focused floater: neutral body plus a 24px bar-blue title band (header is 25px)."""
    img = tint_neutral(load(os.path.join(ALCHEMY, "windows/Window_Foreground.png")))
    p = img.load()
    w, h = img.size
    for y in range(1, 25):
        for x in range(1, w - 1):
            a = p[x, y][3]
            if a:
                p[x, y] = (mix(BAR, BLUE, 0.18) if y == 24 else BAR) + (a,)
    save(img, "windows/Window_Foreground.png")


def tab_selected(rel):
    img = load(os.path.join(ALCHEMY, rel))
    p = img.load()
    w, h = img.size
    for y in range(h):
        for x in range(w):
            r, g, b, a = p[x, y]
            if not a:
                continue
            if y <= 1:
                p[x, y] = BLUE + (a,)
            else:
                # keep the texture's edge shading relative to its 81 body grey
                p[x, y] = scale(BLUE_DEEP, grey_of((r, g, b)) / 81.0) + (a,)
    save(img, rel)


def checkbox_on():
    off = load(os.path.join(ALCHEMY, "widgets/Checkbox_Off.png")).load()
    img = load(os.path.join(ALCHEMY, "widgets/Checkbox_On.png"))
    p = img.load()
    w, h = img.size
    for y in range(h):
        for x in range(w):
            r, g, b, a = p[x, y]
            if not a:
                continue
            v = grey_of((r, g, b))
            if v > 110:                       # box interior -> blue fill
                p[x, y] = BLUE + (a,)
            elif v < 30 and grey_of(off[x, y]) > 110:  # check glyph -> dark
                p[x, y] = BG_DEEP + (a,)
            else:
                p[x, y] = neutral(v) + (a,)
    save(img, "widgets/Checkbox_On.png")


def radio_buttons():
    off = load(os.path.join(DEFAULT, "widgets/RadioButton_Off.png")).load()
    for name in ("RadioButton_Off", "RadioButton_Press", "RadioButton_Disabled",
                 "RadioButton_On", "RadioButton_On_Press", "RadioButton_On_Disabled"):
        img = load(os.path.join(DEFAULT, "widgets", name + ".png"))
        p = img.load()
        dot = name in ("RadioButton_On", "RadioButton_On_Press")
        for y in range(img.size[1]):
            for x in range(img.size[0]):
                r, g, b, a = p[x, y]
                if not a:
                    continue
                v = grey_of((r, g, b))
                base = neutral(v)
                if dot:
                    ov = grey_of(off[x, y])
                    t = max(0.0, (v - ov) / max(1.0, 255.0 - ov))
                    if t > 0.05:
                        base = mix(base, BLUE, min(1.0, t * 1.2))
                p[x, y] = base + (a,)
        save(img, "widgets/" + name + ".png")


def slider_thumbs():
    for name in ("SliderThumb_Off", "SliderThumb_Press"):
        img = load(os.path.join(ALCHEMY, "widgets", name + ".png"))
        p = img.load()
        for y in range(img.size[1]):
            for x in range(img.size[0]):
                r, g, b, a = p[x, y]
                if a:
                    p[x, y] = scale(BLUE, grey_of((r, g, b)) / 87.0) + (a,)
        save(img, "widgets/" + name + ".png")


def default_buttons():
    """Neutral (near white) push buttons; LLButton tints them ButtonDefaultImageColor."""
    for src, dst, body, level in (("PushButton_Off", "PushButton_Default_Off", 51.0, 1.0),
                                  ("PushButton_Press", "PushButton_Default_Press", 45.0, 0.8)):
        img = load(os.path.join(ALCHEMY, "widgets", src + ".png"))
        p = img.load()
        for y in range(img.size[1]):
            for x in range(img.size[0]):
                r, g, b, a = p[x, y]
                if a:
                    v = clamp8(grey_of((r, g, b)) / body * 255.0 * level)
                    p[x, y] = (v, v, v, a)
        save(img, "widgets/" + dst + ".png")


def gradient_fill(src_path, dst_rel, left, right, shade_top=1.08, shade_bottom=0.92):
    """Paint a left->right gradient into src's alpha shape, with soft vertical shading."""
    img = load(src_path)
    p = img.load()
    w, h = img.size
    for y in range(h):
        k = shade_top + (shade_bottom - shade_top) * (y / max(1, h - 1))
        for x in range(w):
            a = p[x, y][3]
            if a:
                p[x, y] = scale(mix(left, right, x / max(1, w - 1)), k) + (a,)
    save(img, dst_rel)


def login_and_progress():
    login = os.path.join(DEFAULT, "widgets/PushButton_Login.png")
    gradient_fill(login, "widgets/PushButton_Login.png", GRAD_BLUE, PURPLE_DEEP)
    gradient_fill(login, "widgets/PushButton_Login_Over.png", GRAD_BLUE, PURPLE_DEEP, 1.18, 1.02)
    gradient_fill(login, "widgets/PushButton_Login_Pressed.png", GRAD_BLUE, PURPLE_DEEP, 0.80, 0.92)
    gradient_fill(os.path.join(DEFAULT, "widgets/ProgressBarSolid.png"),
                  "widgets/ProgressBarSolid.png", BLUE, PURPLE, 1.0, 1.0)


def main():
    if not os.path.isdir(OUT):
        sys.exit("missing %s (copy skins/alchemy to skins/slipstream first)" % OUT)
    n = recolour_alchemy_copies()
    window_foreground()
    for side in ("Left", "Middle", "Right"):
        tab_selected("containers/TabTop_%s_Selected.png" % side)
    checkbox_on()
    radio_buttons()
    slider_thumbs()
    default_buttons()
    login_and_progress()
    print("recoloured %d Alchemy textures plus specials into %s" % (n, OUT))


if __name__ == "__main__":
    main()
