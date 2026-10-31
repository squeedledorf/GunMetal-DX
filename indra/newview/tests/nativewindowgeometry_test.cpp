// Standalone: g++ -std=c++20 -Wall -Wextra -Werror nativewindowgeometry_test.cpp -o <scratch>/geometry_test
#include "../../llwindow/llnativewindowgeometry.h"
#include <cassert>
#include <climits>

int main()
{
    using namespace native_window;
    // A display left of the primary; saved coordinates are display-relative.
    Rect restored = restoreRect({-1920, -200, 1920, 1040}, {1500, 900, 800, 600}, 500, 200);
    assert(restored.x == -800 && restored.y == 240);
    assert(restored.width == 800 && restored.height == 600);
    // Removed taskbar space, smaller monitor, and malformed persisted values.
    restored = restoreRect({0, 32, 1024, 700}, {INT_MAX, INT_MIN, INT_MAX, -1}, 1200, 500);
    assert(restored.x == 0 && restored.y == 32);
    assert(restored.width == 1024 && restored.height == 500);
    // Windows/X11 200%: native coordinates already are pixels.
    Point pointer = inputPoint(200.f, 100.f, 1.f, 2.f, 2.f, 1200);
    assert(pointer.x == 100 && pointer.y == 550);
    // Wayland 200%: native coordinates are points, framebuffer has twice the pixels.
    pointer = inputPoint(100.f, 50.f, 2.f, 2.f, 2.f, 1200);
    assert(pointer.x == 100 && pointer.y == 550);
    // Fractional scaling and asymmetric UI scale, with an outside capture point.
    pointer = inputPoint(-10.f, 40.f, 1.5f, 1.25f, 1.5f, 1200);
    assert(pointer.x == -12 && pointer.y == 759);
}
