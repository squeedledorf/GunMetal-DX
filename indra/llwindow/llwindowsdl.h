/**
 * @file llwindowsdl.h
 * @brief SDL implementation of LLWindow class
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 *
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#ifndef LL_LLWINDOWSDL_H
#define LL_LLWINDOWSDL_H

// Simple Directmedia Layer (http://libsdl.org/) implementation of LLWindow class

#include "llwindow.h"
#include "lltimer.h"

#include "SDL3/SDL.h"
#include "SDL3/SDL_endian.h"

class LLPreeditor;

class LLWindowSDL final : public LLWindow
{
public:
    void show() override;
    void hide() override;
    void restore() override;

    void close() override;

    bool getVisible() override;

    bool getMinimized() override;

    bool getFocused() override;

    bool getMaximized() override;

    bool maximize() override;
    void minimize() override;

    bool getPosition(LLCoordScreen *position) override;

    bool getSize(LLCoordScreen *size) override;
    bool getSize(LLCoordWindow *size) override;

    bool setPosition(LLCoordScreen position) override;

    bool setSizeImpl(LLCoordScreen size) override;
    bool setSizeImpl(LLCoordWindow size) override;

    bool switchContext(bool fullscreen, const LLCoordScreen &size, bool enable_vsync,
                                   const LLCoordScreen *const posp = NULL) override;

    bool setCursorPosition(LLCoordWindow position) override;

    bool getCursorPosition(LLCoordWindow *position) override;

    void showCursor() override;
    void hideCursor() override;
    bool isCursorHidden() override;

    void showCursorFromMouseMove() override;
    void hideCursorUntilMouseMove() override;

    void updateCursor() override;

    void captureMouse() override;
    void releaseMouse() override;

    void setMouseClipping(bool b) override;

    bool getCursorDelta(LLCoordCommon* delta) override;
    void setRelativeMouseMode(bool enabled) override;

    void setMinSize(U32 min_width, U32 min_height, bool enforce_immediately = true) override;

    bool isClipboardTextAvailable() override;
    bool pasteTextFromClipboard(LLWString &dst) override;
    bool copyTextToClipboard(const LLWString &src) override;
    bool copyImageToClipboard(const LLImageRaw* image) override; // <GMDX>

    bool isPrimaryTextAvailable() override;
    bool pasteTextFromPrimary(LLWString &dst) override;
    bool copyTextToPrimary(const LLWString &src) override;

    void flashIcon(F32 seconds) override;
    void maybeStopFlashIcon();

    F32 getGamma() override;
    bool setGamma(const F32 gamma) override; // Set the gamma
    bool restoreGamma() override;            // Restore original gamma table (before updating gamma)


    void processMiscNativeEvents() override;

    void gatherInput(bool app_has_focus) override;

    SDL_AppResult handleEvent(const SDL_Event& event, bool app_has_focus);
    static SDL_AppResult handleEvents(const SDL_Event& event, bool app_has_focus);


    void delayInputProcessing()  override {};
    // A click's pick held for its GPU answer (LLViewerWindow::performPick) holds the events
    // after it in SDL's queue for the next frame, so the input that follows the click is
    // handled after the click's pick callback.
    void holdInputFrame() override { mDelayInput = true; }

    // handy coordinate space conversion routines
    bool convertCoords(LLCoordScreen from, LLCoordWindow *to) override;
    bool convertCoords(LLCoordWindow from, LLCoordScreen *to) override;
    bool convertCoords(LLCoordWindow from, LLCoordGL *to) override;
    bool convertCoords(LLCoordGL from, LLCoordWindow *to) override;
    bool convertCoords(LLCoordScreen from, LLCoordGL *to) override;
    bool convertCoords(LLCoordGL from, LLCoordScreen *to) override;

    LLWindowResolution *getSupportedResolutions(S32 &num_resolutions) override;

    F32 getNativeAspectRatio() override;
    // #422: the refresh rate of the display the window is on (Hz, rounded; 0 = unknown),
    // queried live so moving the window to another monitor is picked up. LLWindow's
    // mRefreshRate was never set on SDL, so every caller saw 0 (TargetFPS clamped to 0).
    S32 getRefreshRate() override;
    F32 getPixelAspectRatio() override;
    void setNativeAspectRatio(F32 ratio)  override { mOverrideAspectRatio = ratio; }

    void beforeDialog() override;
    void afterDialog() override;

    bool dialogColorPicker(F32 *r, F32 *g, F32 *b) override;

    void *getPlatformWindow() override;

    // Raw SDL_Window* — needed by the experimental Vulkan path to create a
    // VkSurfaceKHR via SDL_Vulkan_CreateSurface. Returns nullptr if not created.
    SDL_Window* getSDLWindow() const { return mWindow; }

    void bringToFront() override;

    void allowLanguageTextInput(LLPreeditor *preeditor, bool b) override;
    void setLanguageTextInput(const LLCoordGL& pos) override;

    void spawnWebBrowser(const std::string &escaped_url, bool async) override;

    void setTitle(const std::string title) override;

    static std::vector<std::string> getDynamicFallbackFontList();
    static bool findFallbackFontForChar(U32 wch, std::string& path, S32& face_index);


    F32 getSystemUISize() override;
    // HIGH-DPI: SDL reports logical units; the viewer works in pixels.
    F32 pixelDensity() const;
    S32 toPixels(F32 logical) const;

    bool getFullscreen() override { return mFullscreen; };
    bool setFullscreen(bool fullscreen, bool exclusive) override;   // #919
    bool getFullscreenExclusive() const override { return mFullscreenExclusive; }
    std::string getFullscreenStateText() override;

    static std::vector<std::string> getDisplaysResolutionList();


protected:
    LLWindowSDL(LLWindowCallbacks *callbacks,
                const std::string &title, const std::string& name, int x, int y, int width, int height, U32 flags,
                bool fullscreen, bool clearBg, bool enable_vsync, bool use_gl,
                bool ignore_pixel_depth, U32 fsaa_samples);

    ~LLWindowSDL();

    bool isValid() override;

    LLSD getNativeKeyData() override;

    void initCursors();
    void quitCursors();

protected:
    //
    // Platform specific methods
    //

    // create or re-create the window.  Called from the constructor and switchContext().
    bool createContext(int x, int y, int width, int height, bool fullscreen);
    void destroyContext();

    void setupFailure(const std::string &text, const std::string &caption, U32 type);

    bool SDLReallyCaptureInput(bool capture);
    U32 SDLCheckGrabbyKeys(U32 keysym, bool gain);

    //
    // Platform specific variables
    //
    U32 mGrabbyKeyFlags = 0;

    // Accumulated SDL3 relative-motion deltas (event.motion.xrel/yrel) since
    // the last getCursorDelta() call. Consumed-on-read.
    // Float accumulation: SDL3 xrel/yrel are sub-pixel floats; truncating each
    // event dropped slow/small motion (jerky, then "touchy" drags). Whole pixels are
    // handed out by getCursorDelta; the fractional remainder carries over.
    F32  mAccumRelX = 0.f, mAccumRelY = 0.f;
    bool mInRelativeMouseMode = false;

    SDL_Window *mWindow = nullptr;
    SDL_Cursor *mSDLCursors[UI_CURSOR_COUNT];

    std::string mWindowTitle;
    F32 mOriginalAspectRatio = 1.0f;
    F32 mOverrideAspectRatio = 0.0f;
    F32 mGamma = 0.0f;

    friend class LLWindowManager;

private:
    bool mFlashing = false;
    bool mDelayInput = false;       // holdInputFrame: gatherInput leaves the queue for a frame
    // Fractional wheel travel not yet delivered as whole steps (high-resolution wheels /
    // touchpads report fractions of a notch per event).
    F32 mWheelAccumX = 0.f;
    F32 mWheelAccumY = 0.f;
    bool mFlashUntilFocused = false; // flashIcon(<= 0): no timed cancel, focus gain ends it
    LLTimer mFlashTimer;
    U32 mKeyVirtualKey = 0;
    U32 mKeyModifiers = SDL_KMOD_NONE;
    bool mInKeyCharPass = false;    // dispatching a key-down's character (getNativeKeyData "is_char")

    bool mLanguageTextInputAllowed = false;
    LLPreeditor* mPreeditor = nullptr;

    void tryFindFullscreenSize(int &aWidth, int &aHeight);
    // #919: SDL_SetWindowFullscreenMode for the requested kind on the window's current display
    // (NULL = borderless desktop; the display's desktop mode = exclusive). False if SDL refused.
    bool applyFullscreenMode(bool exclusive);

    // #919 fullscreen state. mFullscreen (LLWindow) is the INTENT: it stays true across the
    // temporary windowed state of beforeDialog() / afterDialog().
    bool mFullscreenExclusive = false;
    bool mInDialog = false;            // beforeDialog() left fullscreen; afterDialog() re-enters
    // The windowed placement to restore on leaving fullscreen (SDL logical units). SDL keeps
    // its own floating rect too; this is the explicit copy (a compositor may resize the
    // window while it is fullscreen, and a maximized window comes back maximized).
    bool mHaveWindowedRect = false;
    int  mWindowedX = 0, mWindowedY = 0, mWindowedW = 0, mWindowedH = 0;
    bool mWindowedMaximized = false;
};

class LLSplashScreenSDL : public LLSplashScreen
{
public:
    LLSplashScreenSDL();
    virtual ~LLSplashScreenSDL();

    void showImpl();
    void updateImpl(const std::string& mesg);
    void hideImpl();
};

S32 OSMessageBoxSDL(const std::string& text, const std::string& caption, U32 type);
S32 OSMessageBoxChoicesSDL(const std::string& text, const std::string& caption, const std::vector<std::string>& labels);

#endif //LL_LLWINDOWSDL_H
