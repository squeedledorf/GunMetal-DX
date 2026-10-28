#!/usr/bin/env python3
"""Build the night sky's bundled assets (gmdxnightsky.cpp loads them).

Inputs (download them yourself):
  catalog.gz      Yale Bright Star Catalogue, 5th Revised Ed. (Hoffleit & Warren 1991),
                  https://cdsarc.cds.unistra.fr/ftp/V/50/catalog.gz (CDS V/50)
  eso0932a.jpg    The Milky Way panorama, ESO/S. Brunier, CC BY 4.0,
                  https://cdn.eso.org/images/large/eso0932a.jpg (6000 x 3000, Galactic
                  coordinates, equirectangular: l = 0 at the centre, l growing to the left,
                  b = +90 at the top)

Outputs (into indra/newview/app_settings):
  gmdx_bsc5.bin       b"GMDXBSC1", uint32 count, then count records of 8 bytes, brightest first:
                      uint16 RA (J2000, 0..65535 = 0..360 deg), int16 Dec (J2000, +-32767 =
                      +-90 deg), uint8 V magnitude code (1 + round((V + 2) * 25)), uint8 r, g, b
                      (the star's blackbody colour in linear sRGB, brightest channel 255)
  gmdx_milkyway.jpg   2048 x 1024, the panorama with its resolved stars taken out (the catalogue
                      draws those), black level removed, scaled so the galactic core's brightest
                      0.1% is 1.0 in linear light, stored sRGB-encoded

Usage: gmdx_night_sky_assets.py <catalog.gz> <eso0932a.jpg> <out dir>
"""
import gzip
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter

# Typical B-V by spectral class, for the ~300 stars the catalogue gives no B-V.
BV_BY_CLASS = {"O": -0.30, "B": -0.15, "A": 0.10, "F": 0.40, "G": 0.70, "K": 1.15, "M": 1.60,
               "C": 2.00, "S": 1.70, "N": 2.00, "R": 1.50}


def bv_to_kelvin(bv):
    # Ballesteros 2012 (EPL 97, 34008)
    return 4600.0 * (1.0 / (0.92 * bv + 1.7) + 1.0 / (0.92 * bv + 0.62))


def cie_xyz(lam):
    # CIE 1931 2-degree observer, Wyman, Sloan & Shirley 2013 (JCGT 2(2)) multi-lobe fit.
    def g(mu, s1, s2):
        s = np.where(lam < mu, s1, s2)
        return np.exp(-0.5 * ((lam - mu) / s) ** 2)
    x = 1.056 * g(599.8, 37.9, 31.0) + 0.362 * g(442.0, 16.0, 26.7) - 0.065 * g(501.1, 20.4, 26.2)
    y = 0.821 * g(568.8, 46.9, 40.5) + 0.286 * g(530.9, 16.3, 31.1)
    z = 1.217 * g(437.0, 11.8, 36.0) + 0.681 * g(459.0, 26.0, 13.8)
    return x, y, z


def blackbody_rgb(t):
    lam = np.arange(380.0, 781.0, 5.0)
    l_m = lam * 1e-9
    planck = 1.0 / (l_m ** 5 * (np.exp(1.4387769e-2 / (l_m * t)) - 1.0))
    x, y, z = (np.sum(planck * c) for c in cie_xyz(lam))
    rgb = np.array([[3.2406, -1.5372, -0.4986],
                    [-0.9689, 1.8758, 0.0415],
                    [0.0557, -0.2040, 1.0570]]) @ np.array([x, y, z])
    rgb = np.maximum(rgb, 0.0)
    return rgb / rgb.max()


def read_catalogue(path):
    stars = []
    for line in gzip.open(path, "rt", encoding="latin-1"):
        line = line.rstrip("\n").ljust(197)
        if not line[75:77].strip():   # the 14 removed entries (novae, clusters)
            continue
        ra = (int(line[75:77]) + int(line[77:79]) / 60.0 + float(line[79:83]) / 3600.0) * 15.0
        dec = int(line[84:86]) + int(line[86:88]) / 60.0 + int(line[88:90]) / 3600.0
        if line[83] == "-":
            dec = -dec
        v = float(line[102:107])
        bv = line[109:114].strip()
        bv = float(bv) if bv else BV_BY_CLASS.get(line[127:147].strip()[:1], 0.6)
        stars.append((v, ra, dec, bv))
    stars.sort()
    return stars


def write_catalogue(stars, out):
    rec = bytearray(b"GMDXBSC1" + struct.pack("<I", len(stars)))
    for v, ra, dec, bv in stars:
        r, g, b = np.round(blackbody_rgb(bv_to_kelvin(bv)) * 255.0).astype(int)
        rec += struct.pack("<HhBBBB", int(round(ra / 360.0 * 65536.0)) & 0xFFFF,
                           int(round(dec / 90.0 * 32767.0)),
                           1 + int(round((min(max(v, -2.0), 7.96) + 2.0) * 25.0)), r, g, b)
    out.write_bytes(bytes(rec))


def srgb_to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(c):
    c = np.clip(c, 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1.0 / 2.4) - 0.055)


def write_milky_way(src, out):
    img = Image.open(src).convert("RGB").resize((2048, 1024), Image.LANCZOS)
    # Resolved stars: pixels well above their 7 x 7 median are put back to it (the dust lanes and
    # the unresolved glow stay; the catalogue draws the stars themselves).
    med = np.asarray(img.filter(ImageFilter.MedianFilter(7)), np.float32) / 255.0
    a = np.asarray(img, np.float32) / 255.0
    lum = a.mean(axis=2, keepdims=True)
    lum_med = med.mean(axis=2, keepdims=True)
    a = np.where(lum > lum_med * 1.12 + 0.015, med, a)
    lin = srgb_to_linear(a)
    y = lin @ np.array([0.2126, 0.7152, 0.0722])
    lin = np.maximum(lin - np.percentile(y, 2.0), 0.0) / max(np.percentile(y, 99.9) - np.percentile(y, 2.0), 1e-6)
    out_img = Image.fromarray(np.round(linear_to_srgb(lin) * 255.0).astype(np.uint8), "RGB")
    out_img.save(out, quality=90, optimize=True, progressive=False)


def main():
    cat, pano, out = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    stars = read_catalogue(cat)
    write_catalogue(stars, out / "gmdx_bsc5.bin")
    write_milky_way(pano, out / "gmdx_milkyway.jpg")
    print(f"{len(stars)} stars, V {stars[0][0]} .. {stars[-1][0]}")


if __name__ == "__main__":
    main()
