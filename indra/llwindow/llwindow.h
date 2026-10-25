/**
 * @file llwindow.h
 * @brief Basic graphical window class
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

#ifndef LL_LLWINDOW_H
#define LL_LLWINDOW_H

#include <string>
#include <vector>
#include "llrect.h"
#include "llcoord.h"
#include "llstring.h"
#include "llcursortypes.h"
#include "llinstancetracker.h"
#include "llsd.h"

class LLSplashScreen;
class LLPreeditor;
class LLWindowCallbacks;
class LLImageRaw; // <GMDX>

// Refer to llwindow_test in test/common/llwindow for usage example

class LLWindow : public LLInstanceTracker<LLWindow>
{
public:

    struct LLWindowResolution
    {
        S32 mWidth;
        S32 mHeight;
    };
    enum EFlags
    {
        // currently unused
    };
public:
    virtual void show() = 0;
    virtual void hide() = 0;
    virtual void close() = 0;
    virtual bool getVisible() = 0;
    virtual bool getMinimized() = 0;
    // True when the window holds OS keyboard/input focus (i.e. the user is
    // "tabbed in"). Used to decide whether to request taskbar attention.
    virtual bool getFocused() = 0;
    virtual bool getMaximized() = 0;
    virtual bool maximize() = 0;
    virtual void minimize() = 0;
    virtual void restore() = 0;
    virtual bool getFullscreen() = 0;
    // #919 runtime fullscreen toggle, on the display the window is on. exclusive = a
    // display-mode fullscreen at the desktop mode (SDL exclusive fullscreen); false = a
    // borderless window covering the display (desktop fullscreen). Calling it while already
    // fullscreen re-applies the mode live. Leaving restores the windowed position / size /
    // maximized state. Returns false when the windowing system refused. The viewer decides
    // the mode (FullScreenMode) and drives the renderer side (LLViewerWindow::setFullscreen).
    virtual bool setFullscreen(bool fullscreen, bool exclusive) { return false; }
    // The SDL-level mode last applied by setFullscreen (true = exclusive display mode).
    virtual bool getFullscreenExclusive() const { return false; }
    // One line for the log / About: the windowing system's view of the current fullscreen
    // state ("windowed", "borderless desktop fullscreen (wayland)", ...).
    virtual std::string getFullscreenStateText() { return mFullscreen ? "fullscreen" : "windowed"; }
    virtual bool getPosition(LLCoordScreen *position) = 0;
    virtual bool getSize(LLCoordScreen *size) = 0;
    virtual bool getSize(LLCoordWindow *size) = 0;
    virtual bool setPosition(LLCoordScreen position) = 0;
    bool setSize(LLCoordScreen size);
    bool setSize(LLCoordWindow size);
    virtual void setMinSize(U32 min_width, U32 min_height, bool enforce_immediately = true);
    virtual bool switchContext(bool fullscreen, const LLCoordScreen &size, bool enable_vsync, const LLCoordScreen * const posp = NULL) = 0;


    virtual bool setCursorPosition(LLCoordWindow position) = 0;
    virtual bool getCursorPosition(LLCoordWindow *position) = 0;
    virtual bool getCursorDelta(LLCoordCommon* delta) { return false; }
    // SDL3 relative mouse mode: hides the cursor and reports motion via raw
    // device deltas (xrel/yrel) unconstrained by cursor position or window
    // edges. Required on Wayland for camera pan/orbit, where the absolute
    // cursor cannot be warped and would otherwise clamp at window edges.
    // No-op on non-SDL backends.
    virtual void setRelativeMouseMode(bool /*enabled*/) {}
    virtual void showCursor() = 0;
    virtual void hideCursor() = 0;
    virtual bool isCursorHidden() = 0;
    virtual void showCursorFromMouseMove() = 0;
    virtual void hideCursorUntilMouseMove() = 0;

    // Provide a way to set the Viewer window title after the
    // windows has been created. The initial use case for this
    // is described in SL-16102 (update window title with agent
    // name, location etc. for non-interactive viewer) but it
    // may also be useful in other cases.
    virtual void setTitle(const std::string title);

    // These two functions create a way to make a busy cursor instead
    // of an arrow when someone's busy doing something. Draw an
    // arrow/hour if busycount > 0.
    virtual void incBusyCount();
    virtual void decBusyCount();
    virtual void resetBusyCount();
    virtual S32 getBusyCount() const;

    // Sets cursor, may set to arrow+hourglass
    virtual void setCursor(ECursorType cursor) { mNextCursor = cursor; };
    virtual ECursorType getCursor() const;
    virtual ECursorType getNextCursor() const { return mNextCursor; };
    virtual void updateCursor() = 0;

    virtual void captureMouse() = 0;
    virtual void releaseMouse() = 0;
    virtual void setMouseClipping( bool b ) = 0;

    virtual bool isClipboardTextAvailable() = 0;
    virtual bool pasteTextFromClipboard(LLWString &dst) = 0;
    virtual bool copyTextToClipboard(const LLWString &src) = 0;
    // <GMDX> Puts an image on the system clipboard (replacing what is there, text included).
    // False where the platform layer has no image clipboard or the image could not be offered.
    virtual bool copyImageToClipboard(const LLImageRaw* image) { return false; }

    virtual bool isPrimaryTextAvailable();
    virtual bool pasteTextFromPrimary(LLWString &dst);
    virtual bool copyTextToPrimary(const LLWString &src);

    // Request taskbar/dock attention. seconds > 0 flashes for at least that
    // long; seconds <= 0 keeps the window highlighted until it regains focus.
    // No-op when the window already has focus.
    virtual void flashIcon(F32 seconds) = 0;
    virtual F32 getGamma() = 0;
    virtual bool setGamma(const F32 gamma) = 0; // Set the gamma
    virtual bool restoreGamma() = 0;            // Restore original gamma table (before updating gamma)
    virtual void processMiscNativeEvents();
    virtual void gatherInput(bool app_has_focus) = 0;
    virtual void delayInputProcessing() = 0;
    // Leave the input queue alone for the next frame (a pick held for its answer: the events
    // after it wait for its callback). Default: delayInputProcessing.
    virtual void holdInputFrame() { delayInputProcessing(); }
    virtual void bringToFront() = 0;
    virtual void focusClient() { };     // this may not have meaning or be required on other platforms, therefore, it's not abstract
    // handy coordinate space conversion routines
    // NB: screen to window and vice verse won't work on width/height coordinate pairs,
    // as the conversion must take into account left AND right border widths, etc.
    virtual bool convertCoords( LLCoordScreen from, LLCoordWindow *to) = 0;
    virtual bool convertCoords( LLCoordWindow from, LLCoordScreen *to) = 0;
    virtual bool convertCoords( LLCoordWindow from, LLCoordGL *to) = 0;
    virtual bool convertCoords( LLCoordGL from, LLCoordWindow *to) = 0;
    virtual bool convertCoords( LLCoordScreen from, LLCoordGL *to) = 0;
    virtual bool convertCoords( LLCoordGL from, LLCoordScreen *to) = 0;

    // query supported resolutions
    virtual LLWindowResolution* getSupportedResolutions(S32 &num_resolutions) = 0;
    virtual F32 getNativeAspectRatio() = 0;
    virtual F32 getPixelAspectRatio() = 0;
    virtual void setNativeAspectRatio(F32 aspect) = 0;

    virtual void beforeDialog() {}; // prepare to put up an OS dialog (if special measures are required, such as in fullscreen mode)
    virtual void afterDialog() {};  // undo whatever was done in beforeDialog()

    // opens system default color picker, modally
    // Returns true if valid color selected
    virtual bool dialogColorPicker(F32 *r, F32 *g, F32 *b);

// return a platform-specific window reference (HWND on Windows, (__unsafe_unretained) NSWindow on the Mac, Gtk window on Linux)
    virtual void *getPlatformWindow() = 0;

    // control platform's Language Text Input mechanisms.
    virtual void allowLanguageTextInput(LLPreeditor *preeditor, bool b) {}
    virtual void setLanguageTextInput( const LLCoordGL & pos ) {};
    virtual void updateLanguageTextInputArea() {}
    virtual void interruptLanguageTextInput() {}
    virtual void spawnWebBrowser(const std::string& escaped_url, bool async) {};

    static std::vector<std::string> getDynamicFallbackFontList();
    // First installed font (in the fallback sort order) that covers wch; false when
    // unsupported on this platform or no font has it. Used for on-demand font fallback.
    static bool findFallbackFontForChar(U32 wch, std::string& path, S32& face_index);

    // Provide native key event data
    virtual LLSD getNativeKeyData() { return LLSD::emptyMap(); }

    // Get system UI size based on DPI (for 96 DPI UI size should be 1.0)
    virtual F32 getSystemUISize() { return 1.0; }

    static std::vector<std::string> getDisplaysResolutionList();

    // windows only DirectInput8 for joysticks
    virtual void* getDirectInput8() { return NULL; };
    virtual bool getInputDevices(U32 device_type_filter,
                                 std::function<bool(std::string&, LLSD&, void*)> osx_callback,
                                 void* win_callback,
                                 void* userdata)
    {
        return false;
    };

    virtual S32 getRefreshRate() { return mRefreshRate; }
protected:
    LLWindow(LLWindowCallbacks* callbacks, bool fullscreen, U32 flags);
    virtual ~LLWindow();
    // Defaults to true
    virtual bool isValid();
    // Defaults to true
    virtual bool canDelete();

    virtual bool setSizeImpl(LLCoordScreen size) = 0;
    virtual bool setSizeImpl(LLCoordWindow size) = 0;

protected:
    LLWindowCallbacks*  mCallbacks;

    bool        mPostQuit;      // should this window post a quit message when destroyed?
    bool        mFullscreen;
    S32         mFullscreenWidth;
    S32         mFullscreenHeight;
    S32         mFullscreenRefresh;
    LLWindowResolution* mSupportedResolutions;
    S32         mNumSupportedResolutions;
    ECursorType mCurrentCursor;
    ECursorType mNextCursor;
    bool        mCursorHidden;
    S32         mBusyCount; // how deep is the "cursor busy" stack?
    bool        mIsMouseClipping;  // Is this window currently clipping the mouse
    bool        mHideCursorPermanent;
    U32         mFlags;
    U16         mHighSurrogate;
    S32         mMinWindowWidth;
    S32         mMinWindowHeight;
    S32         mRefreshRate;

    // Handle a UTF-16 encoding unit received from keyboard.
    // Converting the series of UTF-16 encoding units to UTF-32 data,
    // this method passes the resulting UTF-32 data to mCallback's
    // handleUnicodeChar.  The mask should be that to be passed to the
    // callback.  This method uses mHighSurrogate as a dedicated work
    // variable.
    void handleUnicodeUTF16(U16 utf16, MASK mask);

    friend class LLWindowManager;
};


// LLSplashScreen
// A simple, OS-specific splash screen that we can display
// while initializing the application and before creating a GL
// window


class LLSplashScreen
{
public:
    LLSplashScreen() { };
    virtual ~LLSplashScreen() { };


    // Call to display the window.
    static LLSplashScreen * create();
    static void show();
    static void hide();
    static void update(const std::string& string);

    static bool isVisible();
protected:
    // These are overridden by the platform implementation
    virtual void showImpl() = 0;
    virtual void updateImpl(const std::string& string) = 0;
    virtual void hideImpl() = 0;

    static bool sVisible;

};

// Platform-neutral for accessing the platform specific message box
S32 OSMessageBox(const std::string& text, const std::string& caption, U32 type);
constexpr U32 OSMB_OK = 0;
constexpr U32 OSMB_OKCANCEL = 1;
constexpr U32 OSMB_YESNO = 2;

constexpr S32 OSBTN_YES = 0;
constexpr S32 OSBTN_NO = 1;
constexpr S32 OSBTN_OK = 2;
constexpr S32 OSBTN_CANCEL = 3;

// Message box with caller-labelled buttons (2..4). Returns the 0-based index of the button
// pressed; the LAST button is also the Escape / window-close answer, the FIRST the Enter
// default. Works before any viewer window exists (SDL message box).
S32 OSMessageBoxChoices(const std::string& text, const std::string& caption, const std::vector<std::string>& labels);

//
// LLWindowManager
// Manages window creation and error checking

class LLWindowManager
{
public:
    static LLWindow *createWindow(
        LLWindowCallbacks* callbacks,
        const std::string& title, const std::string& name, S32 x, S32 y, S32 width, S32 height,
        U32 flags = 0,
        bool fullscreen = false,
        bool clearBg = false,
        bool enable_vsync = false,
        bool ignore_pixel_depth = false,
        U32 fsaa_samples = 0,
        U32 max_cores = 0,
        F32 max_gl_version = 4.6f);
    static bool destroyWindow(LLWindow* window);
    static bool isWindowValid(LLWindow *window);
};

//
// helper funcs
//
extern bool gDebugWindowProc;

// Protocols, like "http" and "https" we support in URLs
extern const S32 gURLProtocolWhitelistCount;
extern const std::string gURLProtocolWhitelist[];
//extern const std::string gURLProtocolWhitelistHandler[];

#endif // _LL_window_h_
