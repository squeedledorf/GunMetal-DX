// Coordinate policy shared by native floater placement and input. No OS calls.
#pragma once

#include <algorithm>
#include <cmath>

namespace native_window
{
struct Rect { int x, y, width, height; };
struct Point { int x, y; };

// Saved x/y are relative to the chosen display's usable area. Resized/rearranged
// displays and corrupt/extreme saved sizes must leave the title bar reachable.
inline Rect restoreRect(Rect area, Rect saved, int minWidth, int minHeight)
{
    const int width = std::clamp(saved.width, std::clamp(minWidth, 1, area.width), area.width);
    const int height = std::clamp(saved.height, std::clamp(minHeight, 1, area.height), area.height);
    return {area.x + std::clamp(saved.x, 0, area.width - width),
            area.y + std::clamp(saved.y, 0, area.height - height), width, height};
}

// SDL mouse coordinates use native window units, UI coordinates use scaled
// bottom-left units. Density and content scale are distinct on Windows/X11 vs
// Wayland; do not assume either one is 1 or round to pixels before division.
inline Point inputPoint(float x, float y, float density, float scaleX, float scaleY, int pixelHeight)
{
    return {static_cast<int>(std::lround(x * density / scaleX)),
            static_cast<int>(std::lround((pixelHeight - 1 - y * density) / scaleY))};
}
}
