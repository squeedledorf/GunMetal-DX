#include "linden_common.h"
#include "llnativewindow.h"
#include "llwindowsdl.h"

#include <algorithm>
#include <array>
#include <new>
#include <string_view>

namespace
{
std::array<LLNativeWindow*, LLNativeWindow::capacity> windows{};
LLNativeWindowID nextID = 1;
}

std::function<bool(const SDL_Event&)> LLNativeWindow::mainEvent;

bool LLNativeWindow::absolutePlacement()
{
    const char* driver = SDL_GetCurrentVideoDriver();
    return !driver || std::string_view(driver) != "wayland";
}

std::unique_ptr<LLNativeWindow> LLNativeWindow::create(const Options& options)
{
    auto slot = std::find(windows.begin(), windows.end(), nullptr);
    if (slot == windows.end() || nextID >= (LLNativeWindowID(1) << 63)) return {};
    auto result = std::unique_ptr<LLNativeWindow>(new (std::nothrow) LLNativeWindow);
    if (!result) return {};
    result->mWindow = SDL_CreateWindow(options.title.c_str(), options.bounds.w, options.bounds.h,
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    if (!result->mWindow) return {};
    result->mID = nextID++;
    *slot = result.get();
    LLWindowSDL::setAppIcon(result->mWindow);
    if (absolutePlacement()) SDL_SetWindowPosition(result->mWindow, options.bounds.x, options.bounds.y);
    SDL_ShowWindow(result->mWindow);
    SDL_GetWindowPosition(result->mWindow, &result->mBounds.x, &result->mBounds.y);
    SDL_GetWindowSize(result->mWindow, &result->mBounds.w, &result->mBounds.h);
    if (options.maximized) SDL_MaximizeWindow(result->mWindow);
    return result;
}

LLNativeWindow::~LLNativeWindow()
{
    mSurface = nullptr;
    for (auto& window : windows) if (window == this) window = nullptr;
    if (mWindow) SDL_DestroyWindow(mWindow);
}

void LLNativeWindow::setTitle(const std::string& title) { SDL_SetWindowTitle(mWindow, title.c_str()); }
void LLNativeWindow::raise() { SDL_RaiseWindow(mWindow); }
bool LLNativeWindow::focused() const { return SDL_GetKeyboardFocus() == mWindow; }
bool LLNativeWindow::minimized() const { return SDL_GetWindowFlags(mWindow) & SDL_WINDOW_MINIMIZED; }

LLNativeWindowID LLNativeWindow::keyboardOwner()
{
    for (auto* window : windows) if (window && window->focused()) return window->id();
    return 0;
}

void LLNativeWindow::observe(const SDL_Event& event)
{
    if ((event.type == SDL_EVENT_WINDOW_MOVED || event.type == SDL_EVENT_WINDOW_RESIZED) &&
        !(SDL_GetWindowFlags(mWindow) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED)))
    {
        if (absolutePlacement()) SDL_GetWindowPosition(mWindow, &mBounds.x, &mBounds.y);
        SDL_GetWindowSize(mWindow, &mBounds.w, &mBounds.h);
    }
    if (mSurface) mSurface->nativeEvent(mID, event);
}

bool LLNativeWindow::dispatch(const SDL_Event& event, SDL_Window* mainWindow)
{
    SDL_Window* source = SDL_GetWindowFromEvent(&event);
    if (event.type == SDL_EVENT_DISPLAY_REMOVED)
        for (auto* window : windows) if (window) window->observe(event);
    if (source == mainWindow) return mainEvent && mainEvent(event);
    for (auto* window : windows)
        if (window && source == window->handle()) { window->observe(event); return true; }
    if (source) return true;
    // SDL can deliver queued input after a window was destroyed. Never let an
    // orphan close terminate the application or an orphan key drive the world.
    if (event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST) return true;
    switch (event.type)
    {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_INPUT: case SDL_EVENT_TEXT_EDITING:
    case SDL_EVENT_MOUSE_MOTION: case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: return true;
    default: return false;
    }
}
