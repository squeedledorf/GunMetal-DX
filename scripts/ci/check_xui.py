#!/usr/bin/env python3
"""Fail if any XUI / skin XML file is not well-formed XML.

The viewer silently drops a floater or panel whose XML does not parse (a duplicate
attribute removed the whole Move & View preferences tab in 1.2.0 Beta 7 / 8), so a
broken file never shows up as a build error. Run from the repository root.
"""
import glob
import sys
import xml.parsers.expat

files = sorted(glob.glob("indra/newview/skins/**/*.xml", recursive=True))
bad = []
for path in files:
    parser = xml.parsers.expat.ParserCreate()
    try:
        with open(path, "rb") as f:
            parser.ParseFile(f)
    except xml.parsers.expat.ExpatError as e:
        bad.append(f"{path}:{e.lineno}:{e.offset}: {xml.parsers.expat.ErrorString(e.code)}")
for line in bad:
    print(line)
print(f"check_xui: {len(files)} files, {len(bad)} malformed")
sys.exit(1 if bad else 0)
