/**
 * @file gmdxmapsharetext.cpp
 * @brief GunMetal-DX: the text of a shared mini-map mark, zone or ping (see gmdxmapsharetext.h).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

// <GMDX> whole file: see gmdxmapsharetext.h.

#include "llviewerprecompiledheaders.h"

#include "gmdxmapsharetext.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace GMDXShareText
{
namespace
{
const char* const kColors[] = { "red", "orange", "yellow", "green", "cyan", "purple" };
const char* const kFills[] = { "none", "solid", "stripes" };
const char* const kKinds[] = { "ping", "enemy", "rally" };
const char* const kPingLabels[] = { "Ping", "Enemy", "Rally" };
const char* const kTags[] = { "[Mark] ", "[Zone] ", "[Ping] " };
const char* const kDash = " \xE2\x80\x94 ";   // " — "
const char* const kSLBases[] = { "http://maps.secondlife.com/secondlife/", "https://maps.secondlife.com/secondlife/" };
constexpr double MAX_COORD = 8192.0;    // region metres (var-regions included)
constexpr double MAX_Z = 10000.0;
constexpr double MAX_SIZE = 2048.0;     // a zone's width / height / radius / corner offset

template <size_t N>
int index_of(const char* const (&names)[N], const std::string& s)
{
    for (size_t i = 0; i < N; ++i)
        if (s == names[i]) return (int)i;
    return -1;
}

// 0.5 m steps: "40", "12.5", "-3"
std::string half(double v)
{
    const double h = std::round(v * 2.0) / 2.0;
    char buf[32];
    if (h == std::floor(h)) std::snprintf(buf, sizeof(buf), "%.0f", h == 0.0 ? 0.0 : h);
    else std::snprintf(buf, sizeof(buf), "%.1f", h);
    return buf;
}

// 0.1 m steps: "1.2", "-0.5", "3"
std::string tenth(double v)
{
    const double t = std::round(v * 10.0) / 10.0;
    char buf[32];
    if (t == std::floor(t)) std::snprintf(buf, sizeof(buf), "%.0f", t == 0.0 ? 0.0 : t);
    else std::snprintf(buf, sizeof(buf), "%.1f", t);
    return buf;
}

bool number(const std::string& s, double lo, double hi, double& out)
{
    if (s.empty() || s.size() > 16) return false;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (!end || *end || !std::isfinite(v) || v < lo || v > hi) return false;
    out = v;
    return true;
}

bool integer(const std::string& s, double lo, double hi, double& out)
{
    if (s.empty() || s.size() > 6) return false;
    for (char c : s)
        if ((c < '0' || c > '9') && c != '-') return false;
    return number(s, lo, hi, out);
}

std::string escape(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s)
    {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~')
        {
            out += (char)c;
        }
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

bool unescape(const std::string& s, std::string& out)
{
    out.clear();
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] != '%') { out += s[i]; continue; }
        if (i + 2 >= s.size()) return false;
        const char hi = s[i + 1], lo = s[i + 2];
        if (!std::isxdigit((unsigned char)hi) || !std::isxdigit((unsigned char)lo)) return false;
        out += (char)std::strtol(std::string{ hi, lo }.c_str(), nullptr, 16);
        i += 2;
    }
    return true;
}

// An id as LLUUID::asString writes it: 8-4-4-4-12 lower-case hex.
bool uuid_ok(const std::string& s)
{
    if (s.size() != 36) return false;
    for (size_t i = 0; i < s.size(); ++i)
    {
        const char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return false; }
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

// The region's name from a link: no control or path characters, 1..64 bytes.
bool region_ok(const std::string& name)
{
    if (name.empty() || name.size() > 64 || name.front() == ' ' || name.back() == ' ') return false;
    for (unsigned char c : name)
        if (c < 0x20 || c == 0x7f || std::string("/\\\"<>[]{}|`").find((char)c) != std::string::npos) return false;
    return clean(name, 64) == name;   // valid UTF-8 too
}

bool parse_url(const std::string& url, const std::string& extra_base, Item& item)
{
    std::string rest;
    for (const char* base : kSLBases)
        if (url.rfind(base, 0) == 0) rest = url.substr(std::string(base).size());
    if (rest.empty() && !extra_base.empty() && extra_base.back() == '/' && url.rfind(extra_base, 0) == 0)
        rest = url.substr(extra_base.size());
    if (rest.empty()) return false;
    std::vector<std::string> parts;
    size_t start = 0;
    while (parts.size() < 5)
    {
        const size_t slash = rest.find('/', start);
        parts.push_back(rest.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    if (parts.size() != 4) return false;
    return unescape(parts[0], item.region) && region_ok(item.region)
        && integer(parts[1], 0, MAX_COORD, item.x) && integer(parts[2], 0, MAX_COORD, item.y)
        && integer(parts[3], 0, MAX_Z, item.z);
}

bool parse_corners(const std::string& s, Item& item)
{
    item.corners.clear();
    size_t start = 0;
    while (start <= s.size())
    {
        const size_t semi = s.find(';', start);
        const std::string pair = s.substr(start, semi == std::string::npos ? std::string::npos : semi - start);
        const size_t comma = pair.find(',');
        double dx, dy;
        if (comma == std::string::npos || !number(pair.substr(0, comma), -MAX_SIZE, MAX_SIZE, dx)
            || !number(pair.substr(comma + 1), -MAX_SIZE, MAX_SIZE, dy))
            return false;
        item.corners.emplace_back(dx, dy);
        if (item.corners.size() > MAX_CORNERS) return false;
        if (semi == std::string::npos) break;
        start = semi + 1;
    }
    return item.corners.size() >= 3;
}

// A well-formed multi-byte character that controls the text instead of showing: C1 controls, the
// bidi overrides and isolates (U+202E turns "evil" into "live"), zero-width and other invisible
// format characters, line / paragraph separators, tags. Dropped from names so one cannot pose as
// another or hide what it says.
bool invisible(const std::string& s, size_t i, size_t len)
{
    unsigned cp = (unsigned char)s[i] & (0x7F >> len);
    for (size_t k = 1; k < len; ++k) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
    return (cp >= 0x80 && cp <= 0x9F) || cp == 0xAD || cp == 0x61C || cp == 0x180E
        || (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x2028 && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x206F)
        || cp == 0xFEFF || (cp >= 0xFFF9 && cp <= 0xFFFB) || (cp >= 0xE0000 && cp <= 0xE007F);
}

std::string trim(const std::string& s)
{
    const size_t a = s.find_first_not_of(' ');
    if (a == std::string::npos) return std::string();
    return s.substr(a, s.find_last_not_of(' ') - a + 1);
}
} // namespace

const char* pingLabel(int kind)
{
    return kPingLabels[(kind >= 0 && kind < 3) ? kind : 0];
}

std::string clean(const std::string& text, size_t max_chars)
{
    std::string out;
    size_t chars = 0;
    for (size_t i = 0; i < text.size() && chars < max_chars;)
    {
        // well-formed UTF-8 only (RFC 3629): no overlong forms, surrogates or code points past U+10FFFF
        const unsigned char c = (unsigned char)text[i];
        size_t len = 0;
        unsigned char lo = 0x80, hi = 0xBF;   // the second byte's range
        if (c < 0x80) len = 1;
        else if (c >= 0xC2 && c <= 0xDF) len = 2;
        else if (c >= 0xE0 && c <= 0xEF) { len = 3; if (c == 0xE0) lo = 0xA0; if (c == 0xED) hi = 0x9F; }
        else if (c >= 0xF0 && c <= 0xF4) { len = 4; if (c == 0xF0) lo = 0x90; if (c == 0xF4) hi = 0x8F; }
        bool ok = len && i + len <= text.size();
        for (size_t k = 1; ok && k < len; ++k)
        {
            const unsigned char b = (unsigned char)text[i + k];
            ok = k == 1 ? (b >= lo && b <= hi) : (b >= 0x80 && b <= 0xBF);
        }
        if (!ok) { ++i; continue; }   // a broken sequence: dropped
        if (len > 1 && invisible(text, i, len)) { i += len; continue; }
        if (len > 1 || (c >= 0x20 && c != 0x7f))
        {
            out.append(text, i, len);
            ++chars;
        }
        i += len;
    }
    return out;
}

std::string compose(const Item& item, const std::string& url_base)
{
    char pos[48];
    std::snprintf(pos, sizeof(pos), "/%.0f/%.0f/%.0f", std::round(item.x), std::round(item.y), std::round(item.z));
    const std::string url = url_base + escape(item.region) + pos;
    const int color = (item.color >= 0 && item.color < 6) ? item.color : 0;
    std::string label = trim(clean(item.label, MAX_LABEL));
    std::string tail;
    switch (item.type)
    {
    case Type::Mark:
        if (label.empty()) label = "Mark";
        tail = std::string("mark c=") + kColors[color];
        break;
    case Type::Ping:
    {
        label = pingLabel(item.ping);
        tail = std::string("ping kind=") + kKinds[(item.ping >= 0 && item.ping < 3) ? item.ping : 0];
        const std::string on = trim(clean(item.on, MAX_LABEL));
        if (uuid_ok(item.obj))
        {
            tail += " obj=" + item.obj;
            if (item.has_off) tail += " off=" + tenth(item.off[0]) + "," + tenth(item.off[1]) + "," + tenth(item.off[2]);
            if (!on.empty()) label += " on " + on;
        }
        if (item.map_only) tail += " map=1";
        break;
    }
    case Type::Zone:
    {
        if (label.empty()) label = "Zone";
        const std::string style = std::string(" c=") + kColors[color] + " fill=" + kFills[(item.fill >= 0 && item.fill < 3) ? item.fill : 0]
                                + (item.dotted ? " line=dotted" : "");
        if (item.shape == Shape::Rect)
        {
            label += ", " + half(item.w) + " x " + half(item.h) + " m";
            tail = "zone rect w=" + half(item.w) + " h=" + half(item.h) + " rot=" + half(item.rot) + style;
        }
        else if (item.shape == Shape::Circle)
        {
            label += ", radius " + half(item.r) + " m";
            tail = "zone circle r=" + half(item.r) + style;
        }
        else
        {
            label += ", " + std::to_string(item.corners.size()) + " corners";
            tail = "zone poly p=";
            for (size_t i = 0; i < item.corners.size(); ++i)
                tail += (i ? ";" : "") + half(item.corners[i].first) + "," + half(item.corners[i].second);
            tail += style;
        }
        break;
    }
    }
    const std::string line = kTags[(int)item.type] + label + kDash + url + " (gmdx:" + tail + ")";
    return line.size() <= MAX_BYTES ? line : std::string();
}

bool parse(const std::string& text, const std::string& extra_base, Item& item)
{
    if (text.size() > MAX_BYTES + 64 || text.empty() || text.back() != ')') return false;
    int type = -1;
    for (int t = 0; t < 3; ++t)
        if (text.rfind(kTags[t], 0) == 0) type = t;
    if (type < 0) return false;
    const size_t tag_len = std::string(kTags[type]).size();
    const size_t tail_at = text.rfind(" (gmdx:");
    if (tail_at == std::string::npos || tail_at < tag_len) return false;
    item = Item();
    item.type = (Type)type;

    // "label — url"
    const std::string head = text.substr(tag_len, tail_at - tag_len);
    const size_t space = head.rfind(' ');
    if (!parse_url(space == std::string::npos ? head : head.substr(space + 1), extra_base, item)) return false;
    std::string label = space == std::string::npos ? std::string() : head.substr(0, space);
    for (const char* dash : { " \xE2\x80\x94", " -", "\xE2\x80\x94" })
    {
        const std::string d(dash);
        if (label.size() >= d.size() && label.compare(label.size() - d.size(), d.size(), d) == 0)
        {
            label.erase(label.size() - d.size());
            break;
        }
    }

    // "kind [shape] key=value ..."
    const size_t tail_from = tail_at + 7;
    const std::string tail = text.substr(tail_from, text.size() - 1 - tail_from);
    std::vector<std::string> words;
    for (size_t start = 0; start < tail.size() && words.size() <= 16;)
    {
        const size_t sp = tail.find(' ', start);
        const std::string w = tail.substr(start, sp == std::string::npos ? std::string::npos : sp - start);
        if (!w.empty()) words.push_back(w);
        if (sp == std::string::npos) break;
        start = sp + 1;
    }
    if (words.empty() || words.size() > 16) return false;
    static const char* const kinds[] = { "mark", "zone", "ping" };
    if (words[0] != kinds[type]) return false;
    size_t first_key = 1;
    if (item.type == Type::Zone)
    {
        if (words.size() < 2) return false;
        if (words[1] == "rect") item.shape = Shape::Rect;
        else if (words[1] == "circle") item.shape = Shape::Circle;
        else if (words[1] == "poly") item.shape = Shape::Poly;
        else return false;
        first_key = 2;
    }
    bool has_w = false, has_h = false, has_r = false, has_p = false;
    for (size_t i = first_key; i < words.size(); ++i)
    {
        const size_t eq = words[i].find('=');
        if (eq == std::string::npos) return false;
        const std::string key = words[i].substr(0, eq), value = words[i].substr(eq + 1);
        if (key == "c") { if ((item.color = index_of(kColors, value)) < 0) return false; }
        else if (key == "fill") { if ((item.fill = index_of(kFills, value)) < 0) return false; }
        else if (key == "line") item.dotted = value == "dotted";
        else if (key == "kind") { if ((item.ping = index_of(kKinds, value)) < 0) return false; }
        else if (key == "w") { if (!(has_w = number(value, 0.5, MAX_SIZE, item.w))) return false; }
        else if (key == "h") { if (!(has_h = number(value, 0.5, MAX_SIZE, item.h))) return false; }
        else if (key == "r") { if (!(has_r = number(value, 0.5, MAX_SIZE, item.r))) return false; }
        else if (key == "rot") { if (!number(value, -360.0, 360.0, item.rot)) return false; }
        else if (key == "p") { if (!(has_p = parse_corners(value, item))) return false; }
        else if (key == "obj") { if (item.type != Type::Ping || !uuid_ok(value)) return false; item.obj = value; }
        else if (key == "map") { if (item.type != Type::Ping || value != "1") return false; item.map_only = true; }
        else if (key == "off")
        {
            // the point on the object, in its own frame (a linkset's root, an avatar)
            const size_t c1 = value.find(','), c2 = c1 == std::string::npos ? c1 : value.find(',', c1 + 1);
            if (item.type != Type::Ping || c2 == std::string::npos
                || !number(value.substr(0, c1), -MAX_SIZE, MAX_SIZE, item.off[0])
                || !number(value.substr(c1 + 1, c2 - c1 - 1), -MAX_SIZE, MAX_SIZE, item.off[1])
                || !number(value.substr(c2 + 1), -MAX_SIZE, MAX_SIZE, item.off[2])) return false;
            item.has_off = true;
        }
        // (other keys: a later version's, ignored)
    }
    if (item.type == Type::Zone)
    {
        if ((item.shape == Shape::Rect && !(has_w && has_h)) || (item.shape == Shape::Circle && !has_r)
            || (item.shape == Shape::Poly && !has_p))
            return false;
        // the size said after the name (", 40 x 60 m", ", radius 12 m", ", 5 corners")
        const size_t comma = label.rfind(", ");
        if (comma != std::string::npos)
        {
            const std::string size = label.substr(comma + 2);
            if ((size.size() > 2 && size.compare(size.size() - 2, 2, " m") == 0)
                || (size.size() > 8 && size.compare(size.size() - 8, 8, " corners") == 0))
                label.erase(comma);
        }
    }
    if (item.type == Type::Ping && !item.obj.empty())
    {
        // "Enemy on <name>": the name of what it is on
        const std::string on = std::string(pingLabel(item.ping)) + " on ";
        if (label.compare(0, on.size(), on) == 0) item.on = trim(clean(label.substr(on.size()), MAX_LABEL));
    }
    item.label = item.type == Type::Ping ? std::string(pingLabel(item.ping)) : trim(clean(label, MAX_LABEL));
    return true;
}
} // namespace GMDXShareText
