// Toolkit-independent native UI surfaces. All calls run on the SDL/UI thread.
#pragma once

#include <SDL3/SDL.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

using LLNativeWindowID = std::uint64_t;

// A toolkit adapter implements this contract; the platform never owns its tree.
class LLNativeSurface
{
public:
    virtual ~LLNativeSurface() = default;
    virtual void nativeEvent(LLNativeWindowID window, const SDL_Event& event) = 0;
};

class LLNativeWindow final
{
public:
    struct Options
    {
        std::string title;
        SDL_Rect bounds{}; // SDL window units, not framebuffer pixels
        bool maximized = false;
    };
    static constexpr unsigned capacity = 16;
    static std::unique_ptr<LLNativeWindow> create(const Options& options);
    ~LLNativeWindow();
    LLNativeWindow(const LLNativeWindow&) = delete;
    LLNativeWindow& operator=(const LLNativeWindow&) = delete;

    LLNativeWindowID id() const { return mID; }
    SDL_Window* handle() const { return mWindow; }
    void attach(LLNativeSurface* surface) { mSurface = surface; }
    void setTitle(const std::string& title);
    void raise();
    SDL_Rect floatingBounds() const { return mBounds; }
    bool focused() const;
    bool minimized() const;
    static bool absolutePlacement();

    // Main-window focus transitions are observed before legacy dispatch. Display
    // changes reach each surface. Unknown/dead auxiliary IDs never reach main.
    static std::function<bool(const SDL_Event&)> mainEvent;
    static bool dispatch(const SDL_Event& event, SDL_Window* mainWindow);
    static LLNativeWindowID keyboardOwner();

private:
    LLNativeWindow() = default;
    SDL_Window* mWindow = nullptr;
    LLNativeWindowID mID = 0;
    LLNativeSurface* mSurface = nullptr;
    SDL_Rect mBounds{};
    void observe(const SDL_Event& event);
};
