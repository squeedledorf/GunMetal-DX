/**
 * @file gmdxmapsharetext.h
 * @brief GunMetal-DX: the text of a shared mini-map mark, zone or ping (see gmdxmapshare.h).
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

// <GMDX> A shared map item is one line of ordinary text, readable in any viewer: what it is, its
// name, a map link (SLURL), then a short visible tail GMDX reads to draw it:
//
//   [Mark] Rally point — http://maps.secondlife.com/secondlife/Merithia/138/105/22 (gmdx:mark c=red)
//   [Zone] Kill box, 40 x 60 m — http://maps.secondlife.com/secondlife/Merithia/120/90/22 (gmdx:zone rect w=40 h=60 rot=0 c=orange fill=stripes)
//   [Zone] Pit, radius 12 m — <SLURL of the centre> (gmdx:zone circle r=12 c=red fill=none line=dotted)
//   [Zone] Yard, 5 corners — <SLURL of the middle> (gmdx:zone poly p=-10,-4;8.5,-4;12,6;0,11;-9,5 c=green fill=solid)
//   [Ping] Enemy — http://maps.secondlife.com/secondlife/Merithia/140/110/30 (gmdx:ping kind=enemy)
//   [Ping] Enemy on Bob Smith — <SLURL of where he was> (gmdx:ping kind=enemy obj=<his avatar id>)
//   [Ping] Ping on Tank — <SLURL of the turret> (gmdx:ping kind=ping obj=<the tank's root id> off=-1.5,0,2.2)
//   [Ping] Rally — <SLURL> (gmdx:ping kind=rally map=1)
//
// The link's x / y / z are region metres (whole numbers); a zone's sizes and a polygon's corners
// (offsets in metres from the link's point, east and north) are rounded to 0.5 m. c = red, orange,
// yellow, green, cyan, purple; fill = none, solid, stripes; line=dotted or absent; kind = ping,
// enemy, rally; rot (degrees, counter-clockwise) turns a rectangle about its centre; obj (pings) is
// the object or avatar the ping is on (a linkset's root, an avatar for its attachments), named after
// "on" in the text, which receivers that have it follow as it moves; off (optional, with obj) is the
// picked point on it, metres in its own frame to 0.1 m, so they follow that spot as it turns; map=1
// (pings) is a ping placed on a map, shown on maps only (not over the world). Unknown keys are
// ignored. A line must start with its tag and end with the tail; anything else is not a share (an
// offline IM's "(Saved ...)" prefix, a quote of one, a mangled line) and stays plain text.
// Only standard C++ here: gmdxmapsharetext_selftest.cpp tests it without the viewer.

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace GMDXShareText
{
enum class Type { Mark, Zone, Ping };
enum class Shape { Rect, Circle, Poly };

constexpr size_t MAX_BYTES = 1023;     // an IM / group chat line (MAX_MSG_BUF_SIZE - 1)
constexpr size_t MAX_LABEL = 48;       // characters of a name sent or shown
constexpr size_t MAX_CORNERS = 64;

struct Item
{
    Type type = Type::Mark;
    std::string label;          // the mark's / zone's name (a ping's is its kind)
    std::string region;         // the region's exact name
    double x = 0, y = 0, z = 0; // region metres, whole numbers in the link
    int color = 0;              // 0..5: red, orange, yellow, green, cyan, purple (GMDXMinimapMarks)
    Shape shape = Shape::Rect;  // zones
    double w = 0, h = 0, rot = 0, r = 0;
    std::vector<std::pair<double, double>> corners;   // polygons: offsets from (x, y)
    int fill = 0;               // 0 none, 1 solid, 2 stripes (GMDXMapZone::Fill)
    bool dotted = false;
    int ping = 0;               // 0 ping, 1 enemy, 2 rally
    std::string obj;            // pings: the id it is on ("" none), lower-case 8-4-4-4-12 hex
    std::string on;             // pings: that object's / avatar's name ("" unknown)
    bool has_off = false;       // pings on an object: where on it (off=x,y,z, metres in its own frame, 0.1 m steps)
    double off[3] = { 0, 0, 0 };
    bool map_only = false;      // pings placed on a map: drawn on maps only (map=1)
};

const char* pingLabel(int kind);   // "Ping", "Enemy", "Rally"

// The line for item, with url_base the grid's SLURL base (".../secondlife/"); "" when it does not
// fit in MAX_BYTES (a polygon with too many corners).
std::string compose(const Item& item, const std::string& url_base);

// Reads a line someone sent: true (item filled) only for a well-formed share. Links are accepted on
// maps.secondlife.com and on extra_base (this grid's SLURL base; "" for none).
bool parse(const std::string& text, const std::string& extra_base, Item& item);

// Text safe to draw: no control characters, bidi overrides or zero-width / invisible format
// characters, at most max_chars characters (UTF-8).
std::string clean(const std::string& text, size_t max_chars);
}
