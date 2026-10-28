"""Fail if any XUI / skin XML file is not well-formed XML."""
import glob
import xml.parsers.expat

NAME = "xui"
STAGE = "source"
DESCRIPTION = "every XUI / skin XML file under indra/newview/skins parses"
GUARDS = ("the viewer silently drops a floater or panel whose XML does not parse (a duplicate "
          "attribute removed the whole Move & View preferences tab in 1.2.0 Beta 7 / 8), so a "
          "broken file never shows up as a build error")


def run(ctx, result):
    files = sorted(glob.glob(ctx.path("indra/newview/skins/**/*.xml"), recursive=True))
    bad = 0
    for path in files:
        parser = xml.parsers.expat.ParserCreate()
        try:
            with open(path, "rb") as f:
                parser.ParseFile(f)
        except xml.parsers.expat.ExpatError as e:
            bad += 1
            result.fail("%s:%d:%d: %s" % (ctx.relative(path), e.lineno, e.offset,
                                           xml.parsers.expat.ErrorString(e.code)))
    result.summary = "%d files, %d malformed" % (len(files), bad)
