// <GMDX> Self-check of the shared map line format (gmdxmapsharetext.h), outside the viewer build
// (the define skips the viewer's precompiled header), from the repository root:
//   g++ -std=c++20 -DLL_LLVIEWERPRECOMPILEDHEADERS_H -Iindra/newview indra/newview/tests/gmdxmapsharetext_selftest.cpp
//       indra/newview/gmdxmapsharetext.cpp -o /tmp/gmdxmapsharetext_test && /tmp/gmdxmapsharetext_test
#include "gmdxmapsharetext.h"

#include <cassert>
#include <cstdio>

using namespace GMDXShareText;

int main()
{
    const std::string base = "http://maps.secondlife.com/secondlife/";
    Item it;

    // the brief's examples read back
    assert(parse("[Mark] Rally point \xE2\x80\x94 http://maps.secondlife.com/secondlife/Merithia/138/105/22 (gmdx:mark c=red)", "", it));
    assert(it.type == Type::Mark && it.label == "Rally point" && it.region == "Merithia" && it.x == 138 && it.z == 22 && it.color == 0);
    assert(parse("[Zone] Kill box, 40 x 60 m \xE2\x80\x94 http://maps.secondlife.com/secondlife/Merithia/120/90/22 (gmdx:zone rect w=40 h=60 rot=0 c=orange fill=stripes)", "", it));
    assert(it.shape == Shape::Rect && it.w == 40 && it.h == 60 && it.color == 1 && it.fill == 2 && it.label == "Kill box");
    assert(parse("[Ping] Enemy \xE2\x80\x94 http://maps.secondlife.com/secondlife/Merithia/140/110/30 (gmdx:ping kind=enemy)", "", it));
    assert(it.type == Type::Ping && it.ping == 1 && it.label == "Enemy");

    // round trips, a region name with spaces, a polygon
    Item z;
    z.type = Type::Zone;
    z.shape = Shape::Poly;
    z.label = "Yard, east";
    z.region = "Da Boom";
    z.x = 10; z.y = 20; z.z = 30;
    z.corners = { { -10, -4.3 }, { 8.5, -4 }, { 12, 6 } };
    z.color = 3; z.fill = 1; z.dotted = true;
    const std::string line = compose(z, base);
    assert(line.find("Da%20Boom/10/20/30") != std::string::npos);
    assert(parse(line, "", it));
    assert(it.region == "Da Boom" && it.corners.size() == 3 && it.corners[0].second == -4.5 && it.dotted && it.label == "Yard, east");
    Item c;
    c.type = Type::Zone; c.shape = Shape::Circle; c.r = 12.25; c.region = "R"; c.label = "Pit";
    assert(parse(compose(c, base), "", it) && it.r == 12.5 && it.label == "Pit");

    // a ping on an avatar or object: its id in the tail, its name in the text
    Item p;
    p.type = Type::Ping; p.ping = 1; p.region = "R"; p.x = 5;
    p.obj = "0a1b2c3d-4e5f-6071-8293-a4b5c6d7e8f9"; p.on = "Bob \xE2\x80\xAESmith";
    const std::string pl = compose(p, base);
    assert(pl.rfind("[Ping] Enemy on Bob Smith \xE2\x80\x94 ", 0) == 0 && pl.find(" obj=0a1b2c3d-4e5f-6071-8293-a4b5c6d7e8f9)") != std::string::npos);
    assert(parse(pl, "", it) && it.obj == p.obj && it.on == "Bob Smith" && it.label == "Enemy" && it.ping == 1);
    p.on.clear();   // name unknown: the plain label, the id still
    assert(parse(compose(p, base), "", it) && it.obj == p.obj && it.on.empty() && compose(p, base).rfind("[Ping] Enemy \xE2\x80\x94", 0) == 0);
    // ... and where on it: the picked point in its own frame, to 0.1 m; none sent, none read
    p.has_off = true; p.off[0] = -1.46; p.off[1] = 0.04; p.off[2] = 2.25;
    const std::string po = compose(p, base);
    assert(po.find(" obj=0a1b2c3d-4e5f-6071-8293-a4b5c6d7e8f9 off=-1.5,0,2.3)") != std::string::npos);
    assert(parse(po, "", it) && it.has_off && it.off[0] == -1.5 && it.off[1] == 0 && it.off[2] == 2.3 && it.obj == p.obj);
    assert(parse(pl, "", it) && !it.has_off && it.off[0] == 0);
    assert(!parse("[Ping] Ping \xE2\x80\x94 http://maps.secondlife.com/secondlife/A/1/1/1 (gmdx:ping kind=ping obj=0a1b2c3d-4e5f-6071-8293-a4b5c6d7e8f9 off=1,2)", "", it));
    assert(!parse("[Ping] Ping \xE2\x80\x94 http://maps.secondlife.com/secondlife/A/1/1/1 (gmdx:ping kind=ping obj=0a1b2c3d-4e5f-6071-8293-a4b5c6d7e8f9 off=1,2,nan)", "", it));
    assert(!parse("[Ping] Ping \xE2\x80\x94 http://maps.secondlife.com/secondlife/A/1/1/1 (gmdx:ping kind=ping obj=0a1b2c3d-4e5f-6071-8293-a4b5c6d7e8f9 off=1,2,9999)", "", it));
    assert(!parse("[Mark] x \xE2\x80\x94 http://maps.secondlife.com/secondlife/A/1/1/1 (gmdx:mark c=red off=1,2,3)", "", it));
    p.has_off = false;
    // each kind, and a ping placed on a map (shown on maps only): the text reads the same
    for (int k = 0; k < 3; ++k)
    {
        Item q;
        q.type = Type::Ping; q.ping = k; q.region = "R";
        assert(parse(compose(q, base), "", it) && it.ping == k && it.label == pingLabel(k) && !it.map_only);
        q.map_only = true;
        const std::string ql = compose(q, base);
        assert(ql.rfind(std::string("[Ping] ") + pingLabel(k) + " \xE2\x80\x94 ", 0) == 0 && ql.find(" map=1)") != std::string::npos);
        assert(parse(ql, "", it) && it.ping == k && it.map_only);
    }
    assert(!parse("[Ping] Ping \xE2\x80\x94 http://maps.secondlife.com/secondlife/A/1/1/1 (gmdx:ping kind=ping map=yes)", "", it));
    assert(!parse("[Mark] x \xE2\x80\x94 http://maps.secondlife.com/secondlife/A/1/1/1 (gmdx:mark c=red map=1)", "", it));
    p.obj = "not-an-id"; p.on = "X";   // a bad id is left out, and its name with it
    assert(compose(p, base).find("obj=") == std::string::npos && compose(p, base).find(" on ") == std::string::npos);
    assert(!parse("[Ping] Ping \xE2\x80\x94 http://maps.secondlife.com/secondlife/A/1/1/1 (gmdx:ping kind=ping obj=0A1B2C3D-4E5F-6071-8293-A4B5C6D7E8F9)", "", it));
    assert(!parse("[Mark] x \xE2\x80\x94 http://maps.secondlife.com/secondlife/A/1/1/1 (gmdx:mark c=red obj=0a1b2c3d-4e5f-6071-8293-a4b5c6d7e8f9)", "", it));

    // too many corners: refused, not cut
    z.corners.assign(200, { 1000.5, -1000.5 });
    assert(compose(z, base).empty());

    // not shares: offline prefix, wrong host, bad numbers, junk tails, quotes, path tricks
    assert(!parse("(Saved 10/26) [Ping] Ping \xE2\x80\x94 http://maps.secondlife.com/secondlife/A/1/1/1 (gmdx:ping kind=ping)", "", it));
    assert(!parse("[Ping] Ping \xE2\x80\x94 http://evil.example/secondlife/A/1/1/1 (gmdx:ping kind=ping)", "", it));
    assert(!parse("[Mark] x \xE2\x80\x94 http://maps.secondlife.com/secondlife/A/1e9/1/1 (gmdx:mark c=red)", "", it));
    assert(!parse("[Mark] x \xE2\x80\x94 http://maps.secondlife.com/secondlife/A/1/1/1 (gmdx:mark c=pink)", "", it));
    assert(!parse("[Mark] x \xE2\x80\x94 http://maps.secondlife.com/secondlife/A/1/1/1 (gmdx:zone rect w=1 h=1)", "", it));
    assert(!parse("[Mark] x \xE2\x80\x94 http://maps.secondlife.com/secondlife/..%2Fx/1/1/1 (gmdx:mark c=red)", "", it));
    assert(!parse("[Zone] x \xE2\x80\x94 http://maps.secondlife.com/secondlife/A/1/1/1 (gmdx:zone rect w=nan h=1)", "", it));
    assert(!parse("[Zone] x \xE2\x80\x94 http://maps.secondlife.com/secondlife/A/1/1/1 (gmdx:zone poly p=1,1;2,2)", "", it));
    assert(!parse("[Mark] x \xE2\x80\x94 http://maps.secondlife.com/secondlife/A/1/1 (gmdx:mark c=red)", "", it));
    assert(!parse("he said [Mark] x \xE2\x80\x94 http://maps.secondlife.com/secondlife/A/1/1/1 (gmdx:mark c=red)", "", it));
    // another grid's base only when given
    assert(!parse("[Mark] x \xE2\x80\x94 hop://grid.example:8002/A/1/1/1 (gmdx:mark c=red)", "", it));
    assert(parse("[Mark] x \xE2\x80\x94 hop://grid.example:8002/A/1/1/1 (gmdx:mark c=red)", "hop://grid.example:8002/", it));

    // names to draw: control characters and broken UTF-8 dropped, length capped
    assert(clean("a\nb\x01" "c\xC3", 10) == "abc");
    assert(clean("\xC3\xA9t\xC3\xA9", 2) == "\xC3\xA9t");
    // overlong, surrogate, past U+10FFFF and F5+ lead bytes dropped; a 4-byte character kept
    assert(clean("a\xC0\xAF" "b\xED\xA0\x80" "c\xF4\x90\x80\x80" "d\xF5\x80\x80\x80" "e", 20) == "abcde");
    assert(clean("\xF0\x9F\x98\x80", 5) == "\xF0\x9F\x98\x80");
    // spoofing: RLO / PDF, LRI, zero-width space and joiner, BOM, a C1 control, a tag, soft hyphen dropped
    assert(clean("a\xE2\x80\xAE" "b\xE2\x80\xAC" "c\xE2\x81\xA6" "d\xE2\x80\x8B" "e\xE2\x80\x8D" "f\xEF\xBB\xBF" "g\xC2\x85" "h\xF3\xA0\x80\x81" "i\xC2\xAD" "j", 20) == "abcdefghij");
    // ... and not counted: still 3 characters
    assert(clean("\xE2\x80\xAE" "abc", 3) == "abc");
    // ordinary non-ASCII kept: e-acute, an em dash, CJK, a no-break space
    assert(clean("\xC3\xA9\xE2\x80\x94\xE6\x97\xA5\xC2\xA0", 10) == "\xC3\xA9\xE2\x80\x94\xE6\x97\xA5\xC2\xA0");
    // in a share: the label comes back clean, a region name with one is not a share
    assert(parse("[Mark] Ra\xE2\x80\xAEly \xE2\x80\x94 http://maps.secondlife.com/secondlife/A/1/1/1 (gmdx:mark c=red)", "", it) && it.label == "Raly");
    assert(!parse("[Mark] x \xE2\x80\x94 http://maps.secondlife.com/secondlife/A%E2%80%AE/1/1/1 (gmdx:mark c=red)", "", it));
    std::puts("gmdxmapsharetext: OK");
    return 0;
}
