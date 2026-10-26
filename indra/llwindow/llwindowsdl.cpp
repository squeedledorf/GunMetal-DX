/**
 * @file llwindowsdl.cpp
 * @brief SDL implementation of LLWindow class
 * @author This module has many fathers, and it shows.
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

#include "linden_common.h"

#include "llwindowsdl.h"

#include "llwindowcallbacks.h"
#include "llkeyboardsdl.h"
#include "llgamecontrol.h"

#include "llerror.h"
#include "llstring.h"
#include "lldir.h"
#include "llfindlocale.h"
#include "llpreeditor.h"
#include "llimagebmp.h" // <GMDX> image clipboard
#include "llimagepng.h" // <GMDX>

#if LL_WINDOWS
// #847: DirectInput8 for libndofdev (joystick / 3D mouse), see getInputDevices()
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#endif

#include <sstream>

#ifdef LL_GLIB
#include <glib.h>
#endif

#if LL_LINUX
extern "C" {
# include "fontconfig/fontconfig.h"
}

// not necessarily available on random SDL platforms, so #if LL_LINUX
// for execv(), waitpid(), fork()
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unordered_map>
#include <stdio.h>
#endif // LL_LINUX

static const bool gHiDPISupport = true;   // high-DPI drawable (see createContext)

extern bool gDebugWindowProc;

// Vulkan is the only renderer; the window is always created SDL_WINDOW_VULKAN.

const S32 MAX_NUM_RESOLUTIONS = 200;

//
// LLWindowSDL
//

// TOFU HACK -- (*exactly* the same hack as LLWindowMacOSX for a similar
// set of reasons): Stash a pointer to the LLWindowSDL object here and
// maintain in the constructor and destructor.  This assumes that there will
// be only one object of this class at any time.  Currently this is true.
static LLWindowSDL *gWindowImplementation = nullptr;

LLWindowSDL::LLWindowSDL(LLWindowCallbacks* callbacks,
                         const std::string& title, const std::string& name, S32 x, S32 y, S32 width,
                         S32 height, U32 flags,
                         bool fullscreen, bool clearBg,
                         bool enable_vsync, bool use_gl,
                         bool ignore_pixel_depth, U32 fsaa_samples)
        : LLWindow(callbacks, fullscreen, flags),
        mGamma(1.0f), mFlashing(false)
{
#if LL_WINDOWS
    // #847: the ndof library needs a DirectInput8 interface; the SDL window never made one,
    // so every device read as invalid and all axes stayed 0.
    {
        IDirectInput8* di8 = nullptr;
        HRESULT hr = DirectInput8Create(GetModuleHandle(nullptr), DIRECTINPUT_VERSION,
                                        IID_IDirectInput8, (void**)&di8, nullptr);
        if (SUCCEEDED(hr))
        {
            mDirectInput8 = di8;
        }
        else
        {
            LL_WARNS("Joystick") << "DirectInput8Create failed, hr=" << std::hex << (U32)hr << std::dec << LL_ENDL;
        }
    }
#endif

    // Initialize the keyboard
    gKeyboard = new LLKeyboardSDL();
    gKeyboard->setCallbacks(callbacks);

    // Assume 4:3 aspect ratio until we know better
    mOriginalAspectRatio = 1024.f / 768.f;

    if (title.empty())
        mWindowTitle = "Second Life";
    else
        mWindowTitle = title;

    // Create the Vulkan-capable window, windowed or fullscreen as appropriate.
    if(createContext(x, y, width, height, fullscreen))
    {
        //start with arrow cursor
        initCursors();
        setCursor( UI_CURSOR_ARROW );
    }

    // Stash an object pointer for OSMessageBox()
    gWindowImplementation = this;
}

static SDL_Surface *Load_BMP_Resource(const char *basename)
{
    const int PATH_BUFFER_SIZE=1000;
    char path_buffer[PATH_BUFFER_SIZE]; /* Flawfinder: ignore */

    // Figure out where our BMP is living on the disk
    snprintf(path_buffer, PATH_BUFFER_SIZE-1, "%s%sres-sdl%s%s",
             gDirUtilp->getAppRODataDir().c_str(),
             gDirUtilp->getDirDelimiter().c_str(),
             gDirUtilp->getDirDelimiter().c_str(),
             basename);
    path_buffer[PATH_BUFFER_SIZE-1] = '\0';

    return SDL_LoadBMP(path_buffer);
}

void LLWindowSDL::setTitle(const std::string title)
{
    SDL_SetWindowTitle( mWindow, title.c_str() );
}

void LLWindowSDL::tryFindFullscreenSize( int &width, int &height )
{
    LL_INFOS() << "createContext: setting up fullscreen " << width << "x" << height << LL_ENDL;

    // If the requested width or height is 0, find the best default for the monitor.
    if(width == 0 || height == 0)
    {
        // Scan through the list of modes, looking for one which has:
        //      height between 700 and 800
        //      aspect ratio closest to the user's original mode
        S32 resolutionCount = 0;
        LLWindowResolution *resolutionList = getSupportedResolutions(resolutionCount);

        if(resolutionList != nullptr)
        {
            F32 closestAspect = 0;
            U32 closestHeight = 0;
            U32 closestWidth = 0;

            LL_INFOS() << "createContext: searching for a display mode, original aspect is " << mOriginalAspectRatio << LL_ENDL;

            for(S32 i=0; i < resolutionCount; i++)
            {
                F32 aspect = (F32)resolutionList[i].mWidth / (F32)resolutionList[i].mHeight;

                LL_INFOS() << "createContext: width " << resolutionList[i].mWidth << " height " << resolutionList[i].mHeight << " aspect " << aspect << LL_ENDL;

                if( (resolutionList[i].mHeight >= 700) && (resolutionList[i].mHeight <= 800) &&
                    (fabs(aspect - mOriginalAspectRatio) < fabs(closestAspect - mOriginalAspectRatio)))
                {
                    LL_INFOS() << " (new closest mode) " << LL_ENDL;

                    // This is the closest mode we've seen yet.
                    closestWidth = resolutionList[i].mWidth;
                    closestHeight = resolutionList[i].mHeight;
                    closestAspect = aspect;
                }
            }

            width = closestWidth;
            height = closestHeight;
        }
    }

    if(width == 0 || height == 0)
    {
        // Mode search failed for some reason.  Use the old-school default.
        width = 1024;
        height = 768;
    }
}

bool LLWindowSDL::createContext(int x, int y, int width, int height, bool fullscreen)
{
    LL_INFOS() << "createContext, fullscreen=" << fullscreen << " size=" << width << "x" << height << LL_ENDL;

    // captures don't survive contexts
    mGrabbyKeyFlags = 0;

    if (width == 0)
        width = 1024;
    if (height == 0)
        height = 768;
    if (x == 0)
        x = SDL_WINDOWPOS_UNDEFINED;
    if (y == 0)
        y = SDL_WINDOWPOS_UNDEFINED;

    // #919: the window is always CREATED windowed, at the saved windowed placement; a
    // FullScreen session is entered by the viewer right after the renderer is up
    // (LLViewerWindow::setFullscreen), with the chosen FullScreenMode, on the display the
    // window landed on. Creating it fullscreen made SDL's remembered windowed rect the
    // fullscreen one (leaving fullscreen then kept a display-sized window) and could not
    // pick the exclusive / borderless kind.
    mFullscreen = false;
    mFullscreenExclusive = false;
    mHaveWindowedRect = false;
    if (fullscreen)
    {
        LL_INFOS("Window") << "Fullscreen requested: the window is created windowed and enters fullscreen once the renderer is up" << LL_ENDL;
    }

    // Create an SDL_WINDOW_VULKAN window. The viewer's Vulkan bring-up
    // (LLVulkanContext) pulls the surface off this window later.
    SDL_PropertiesID vkprops = SDL_CreateProperties();
    SDL_SetStringProperty(vkprops, SDL_PROP_WINDOW_CREATE_TITLE_STRING, mWindowTitle.c_str());
    SDL_SetNumberProperty(vkprops, SDL_PROP_WINDOW_CREATE_X_NUMBER, x);
    SDL_SetNumberProperty(vkprops, SDL_PROP_WINDOW_CREATE_Y_NUMBER, y);
    SDL_SetNumberProperty(vkprops, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width);
    SDL_SetNumberProperty(vkprops, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height);
    SDL_SetBooleanProperty(vkprops, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(vkprops, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
    SDL_SetBooleanProperty(vkprops, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, false);   // #919: see above
    SDL_SetBooleanProperty(vkprops, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true);
    // HIGH-DPI: ask for a full-resolution drawable. The viewer works in PIXELS
    // throughout (sizes, mouse, swapchain); SDL's logical units are converted at
    // this boundary (toPixels / toLogical), and the display scale feeds the UI
    // scale via getSystemUISize(). Without it, a 125%-scaled desktop gave us a
    // 3072x1670 surface on a 3840x2160 monitor and the compositor upscaled it.
    SDL_SetBooleanProperty(vkprops, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, gHiDPISupport);

    mWindow = SDL_CreateWindowWithProperties(vkprops);
    SDL_DestroyProperties(vkprops);
    if (mWindow && !mFullscreen)
    {
        // Saved/requested window sizes are pixels; SDL sizes windows in logical units.
        const F32 density = pixelDensity();
        if (density > 1.01f)
        {
            SDL_SetWindowSize(mWindow, llmax(1, ll_round((F32)width / density)), llmax(1, ll_round((F32)height / density)));
        }
    }
    if (mWindow)
    {
        S32 pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(mWindow, &pw, &ph);
        LL_INFOS() << "SDL video driver: " << (SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?")
                   << ", pixel density " << pixelDensity() << ", display scale " << SDL_GetWindowDisplayScale(mWindow)
                   << ", drawable " << pw << "x" << ph << LL_ENDL;
    }
    if (mWindow == nullptr)
    {
        LL_WARNS() << "Vulkan window creation failure. SDL: " << SDL_GetError() << LL_ENDL;
        setupFailure("Vulkan window creation error", "Error", OSMB_OK);
        return false;
    }
    LL_INFOS() << "Created SDL_WINDOW_VULKAN window " << width << "x" << height << LL_ENDL;
    // Set the application icon.
    SDL_Surface* vkicon = Load_BMP_Resource("ll_icon.BMP");
    if (vkicon)
    {
        SDL_SetWindowIcon(mWindow, vkicon);
        SDL_DestroySurface(vkicon);
    }
    return true;
}

// changing fullscreen resolution, or switching between windowed and fullscreen mode.
bool LLWindowSDL::switchContext(bool fullscreen, const LLCoordScreen &size, bool enable_vsync, const LLCoordScreen * const posp)
{
    const bool needsRebuild = true;  // Just nuke the context and start over.
    bool result = true;

    LL_INFOS() << "switchContext, fullscreen=" << fullscreen << LL_ENDL;
    if(needsRebuild)
    {
        destroyContext();
        result = createContext(0, 0, size.mX, size.mY, fullscreen);
        if (result)
        {
            //start with arrow cursor
            initCursors();
            setCursor( UI_CURSOR_ARROW );
        }
    }

    return result;
}

void LLWindowSDL::destroyContext()
{
    LL_INFOS() << "destroyContext begins" << LL_ENDL;

    // Stop unicode input
    SDL_StopTextInput(mWindow);

    LL_INFOS() << "Destroying SDL cursors" << LL_ENDL;
    quitCursors();

    if (mWindow)
    {
        LL_INFOS() << "Destroying SDL Window" << LL_ENDL;
        SDL_DestroyWindow(mWindow);
        mWindow = nullptr;
    }
    else
    {
        LL_INFOS() << "SDL Window already destroyed" << LL_ENDL;
    }
    LL_INFOS() << "destroyContext end" << LL_ENDL;
}

namespace { void clear_clipboard_image(); } // <GMDX> (the image clipboard, below)

LLWindowSDL::~LLWindowSDL()
{
    destroyContext();
    clear_clipboard_image(); // <GMDX> the clipboard image, while the image classes are still up

#if LL_WINDOWS
    if (mDirectInput8)
    {
        static_cast<IDirectInput8*>(mDirectInput8)->Release();
        mDirectInput8 = nullptr;
    }
#endif

    delete[] mSupportedResolutions;

    gWindowImplementation = nullptr;
}


#if LL_WINDOWS
void* LLWindowSDL::getDirectInput8()
{
    // Address of the interface pointer (callers deref to LPDIRECTINPUT8), as LLWindowWin32 did.
    return &mDirectInput8;
}

bool LLWindowSDL::getInputDevices(U32 device_type_filter,
                                  std::function<bool(std::string&, LLSD&, void*)> osx_callback,
                                  void* win_callback,
                                  void* userdata)
{
    if (mDirectInput8 && win_callback)
    {
        // The callback runs synchronously, inside EnumDevices
        HRESULT status = static_cast<IDirectInput8*>(mDirectInput8)->EnumDevices(
            device_type_filter, (LPDIENUMDEVICESCALLBACK)win_callback, userdata, DIEDFL_ATTACHEDONLY);
        return status == DI_OK;
    }
    return false;
}
#endif

void LLWindowSDL::show()
{
    if (mWindow)
    {
        SDL_ShowWindow(mWindow);
        SDL_RaiseWindow(mWindow);
    }
}

void LLWindowSDL::hide()
{
    if (mWindow)
    {
        SDL_HideWindow(mWindow);
    }
}

void LLWindowSDL::minimize()
{
    if (mWindow)
    {
        SDL_MinimizeWindow(mWindow);
    }
}

void LLWindowSDL::restore()
{
    if (mWindow)
    {
        SDL_RestoreWindow(mWindow);
    }
}

// close() destroys all OS-specific code associated with a window.
// Usually called from LLWindowManager::destroyWindow()
void LLWindowSDL::close()
{
    // Make sure cursor is visible and we haven't mangled the clipping state.
    setMouseClipping(false);
    showCursor();

    destroyContext();
}

bool LLWindowSDL::isValid()
{
    return mWindow != nullptr;
}

bool LLWindowSDL::getVisible()
{
    bool result = true;
    if (mWindow)
    {
        SDL_WindowFlags flags = SDL_GetWindowFlags(mWindow);
        if (flags & SDL_WINDOW_HIDDEN)
        {
            result = false;
        }
    }
    return result;
}

bool LLWindowSDL::getMinimized()
{
    bool result = false;
    if (mWindow)
    {
        SDL_WindowFlags flags = SDL_GetWindowFlags(mWindow);
        if (flags & SDL_WINDOW_MINIMIZED)
        {
            result = true;
        }
    }
    return result;
}

bool LLWindowSDL::getFocused()
{
    if (!mWindow)
    {
        return false;
    }
    return (SDL_GetWindowFlags(mWindow) & SDL_WINDOW_INPUT_FOCUS) != 0;
}

bool LLWindowSDL::getMaximized()
{
    bool result = false;
    if (mWindow)
    {
        SDL_WindowFlags flags = SDL_GetWindowFlags(mWindow);
        if (flags & SDL_WINDOW_MAXIMIZED)
        {
            result = true;
        }
    }

    return result;
}

bool LLWindowSDL::maximize()
{
    if (mWindow)
    {
        SDL_MaximizeWindow(mWindow);
        return true;
    }
    return false;
}

bool LLWindowSDL::getPosition(LLCoordScreen *position)
{
    if (mWindow)
    {
        SDL_GetWindowPosition(mWindow, &position->mX, &position->mY);
        return true;
    }
    return false;
}

bool LLWindowSDL::getSize(LLCoordScreen *size)
{
    if (mWindow)
    {
        SDL_GetWindowSizeInPixels(mWindow, &size->mX, &size->mY);
        return true;
    }

    return false;
}

bool LLWindowSDL::getSize(LLCoordWindow *size)
{
    if (mWindow)
    {
        SDL_GetWindowSizeInPixels(mWindow, &size->mX, &size->mY);
        return true;
    }

    return false;
}

bool LLWindowSDL::setPosition(const LLCoordScreen position)
{
    if (mWindow)
    {
        SDL_SetWindowPosition(mWindow, position.mX, position.mY);
        return true;
    }

    return false;
}

template< typename T > bool setSizeImpl( const T& newSize, SDL_Window *pWin )
{
    if( !pWin )
        return false;

    SDL_WindowFlags winFlags = SDL_GetWindowFlags( pWin );

    if( winFlags & SDL_WINDOW_MAXIMIZED )
        SDL_RestoreWindow( pWin );

    // newSize is in pixels; SDL takes logical units.
    F32 density = SDL_GetWindowPixelDensity(pWin);
    if (density <= 0.f) density = 1.f;
    SDL_SetWindowSize( pWin, llmax(1, ll_round((F32)newSize.mX / density)), llmax(1, ll_round((F32)newSize.mY / density)) );
    return true;
}

bool LLWindowSDL::setSizeImpl(const LLCoordScreen size)
{
    return ::setSizeImpl( size, mWindow );
}

bool LLWindowSDL::setSizeImpl(const LLCoordWindow size)
{
    return ::setSizeImpl( size, mWindow );
}


F32 LLWindowSDL::getGamma()
{
    return 1.f / mGamma;
}

bool LLWindowSDL::restoreGamma()
{
    return true;
}

bool LLWindowSDL::setGamma(const F32 gamma)
{
    if (mWindow)
    {
        mGamma = gamma;
        if (mGamma == 0)
            mGamma = 0.1f;
        mGamma = 1.f / mGamma;
    }
    return true;
}

bool LLWindowSDL::isCursorHidden()
{
    return mCursorHidden;
}

// Constrains the mouse to the window.
void LLWindowSDL::setMouseClipping(bool b)
{
    if( mWindow )
        SDL_SetWindowMouseGrab(mWindow, b);
}

bool LLWindowSDL::getCursorDelta(LLCoordCommon* delta)
{
    if (!delta)
        return false;
    const S32 ix = (S32)mAccumRelX, iy = (S32)mAccumRelY;   // toward zero; remainder kept
    delta->mX = ix; delta->mY = iy;
    mAccumRelX -= (F32)ix; mAccumRelY -= (F32)iy;
    return true;
}

void LLWindowSDL::setRelativeMouseMode(bool enabled)
{
    if (!mWindow)
        return;
    // Compare against SDL's actual window state, not only our cached flag, so
    // a mode SDL dropped on its own (e.g. focus changes) is re-applied.
    if (enabled == mInRelativeMouseMode && enabled == SDL_GetWindowRelativeMouseMode(mWindow))
        return;
    SDL_SetWindowRelativeMouseMode(mWindow, enabled);
    mInRelativeMouseMode = enabled;
    // Drop any pending accumulated motion when toggling, so the first delta
    // after a mode change reflects only post-toggle input.
    mAccumRelX = mAccumRelY = 0.f;
}

// virtual
void LLWindowSDL::setMinSize(U32 min_width, U32 min_height, bool enforce_immediately)
{
    LLWindow::setMinSize(min_width, min_height, enforce_immediately);

    if (mWindow && min_width > 0 && min_height > 0)
    {
        const F32 density = pixelDensity();
        SDL_SetWindowMinimumSize(mWindow, llmax(1, ll_round((F32)mMinWindowWidth / density)), llmax(1, ll_round((F32)mMinWindowHeight / density)));
    }
}

bool LLWindowSDL::setCursorPosition(const LLCoordWindow position)
{
    bool result = true;
    LLCoordScreen screen_pos;

    if (!convertCoords(position, &screen_pos))
    {
        return false;
    }

    // do the actual forced cursor move (SDL takes logical units).
    const F32 density = pixelDensity();
    SDL_WarpMouseInWindow(mWindow, (F32)screen_pos.mX / density, (F32)screen_pos.mY / density);

    return result;
}

bool LLWindowSDL::getCursorPosition(LLCoordWindow *position)
{
    //Point cursor_point;
    LLCoordScreen screen_pos;

    float x, y;
    SDL_GetMouseState(&x, &y);

    const F32 density = pixelDensity();
    screen_pos.mX = (S32)(x * density);
    screen_pos.mY = (S32)(y * density);

    return convertCoords(screen_pos, position);
}

S32 LLWindowSDL::getRefreshRate()
{
    // #422: live, from the display the window is on (SDL3 reports a float in Hz; 0 when
    // the driver does not know). The last good value is kept for a transient failure.
    if (mWindow)
    {
        const SDL_DisplayID disp = SDL_GetDisplayForWindow(mWindow);
        const SDL_DisplayMode* dm = disp ? SDL_GetCurrentDisplayMode(disp) : nullptr;
        if (dm && dm->refresh_rate > 1.f)
        {
            mRefreshRate = (S32)(dm->refresh_rate + 0.5f);
        }
    }
    return mRefreshRate;
}

F32 LLWindowSDL::getNativeAspectRatio()
{
    if (mOverrideAspectRatio > 0.f)
    {
        return mOverrideAspectRatio;
    }

    return mOriginalAspectRatio;
}

F32 LLWindowSDL::getPixelAspectRatio()
{
    // #919: fullscreen is always the display's own mode (borderless desktop, or exclusive at the
    // desktop mode), so pixels are square. The old non-square guess (FullScreenAspectRatio, 4:3
    // by default, against the screen size) widened the whole UI by a third on a 16:9 display.
    return 1.f;
}


// This is to support 'temporarily windowed' mode so that
// dialogs are still usable in fullscreen.
// #919: a native dialog (file picker, message box) can open behind an EXCLUSIVE (display-
// mode) fullscreen window, so that window goes windowed for the dialog and back to
// fullscreen afterwards -- in the same mode, re-applied from mFullscreenExclusive. A
// borderless fullscreen window is an ordinary top-level window the dialog stacks above, so
// it stays (no flicker per file picker). mFullscreen stays true in between: it is the
// session's choice, not the momentary state. (afterDialog used to call
// SDL_SetWindowFullscreen(.., 0) as well, so a session never came back fullscreen; and the
// leave was gated on SDLReallyCaptureInput(false), which returns false, so it never ran.)
void LLWindowSDL::beforeDialog()
{
    LL_INFOS() << "LLWindowSDL::beforeDialog()" << LL_ENDL;

    SDLReallyCaptureInput(false); // must ungrab input so popup works!
    if (mFullscreen && mFullscreenExclusive && mWindow && !mInDialog)
    {
        mInDialog = true;
        SDL_SetWindowFullscreen(mWindow, false);
    }
}

void LLWindowSDL::afterDialog()
{
    LL_INFOS() << "LLWindowSDL::afterDialog()" << LL_ENDL;

    if (!mInDialog)
    {
        return;
    }
    mInDialog = false;
    if (mFullscreen && mWindow)
    {
        applyFullscreenMode(mFullscreenExclusive);
        if (!SDL_SetWindowFullscreen(mWindow, true))
        {
            LL_WARNS("Window") << "Fullscreen: could not re-enter fullscreen after a dialog: " << SDL_GetError() << LL_ENDL;
        }
    }
}

bool LLWindowSDL::applyFullscreenMode(bool exclusive)
{
    if (!mWindow)
    {
        return false;
    }
    if (!exclusive)
    {
        // NULL: borderless "desktop" fullscreen -- the window covers the display at its
        // current mode, no mode switch.
        if (!SDL_SetWindowFullscreenMode(mWindow, nullptr))
        {
            LL_WARNS("Window") << "Fullscreen: SDL_SetWindowFullscreenMode(borderless) failed: " << SDL_GetError() << LL_ENDL;
            return false;
        }
        return true;
    }
    // Exclusive: a display-mode fullscreen at the desktop mode of the display the window is
    // on (no resolution change). SDL only accepts a mode from its own list for the display,
    // so the desktop mode is matched against that list.
    const SDL_DisplayID disp = SDL_GetDisplayForWindow(mWindow);
    const SDL_DisplayMode* desk = disp ? SDL_GetDesktopDisplayMode(disp) : nullptr;
    if (!desk)
    {
        LL_WARNS("Window") << "Fullscreen: no desktop mode for display " << (U32)disp << ": " << SDL_GetError()
                           << "; using borderless" << LL_ENDL;
        applyFullscreenMode(false);
        return false;
    }
    SDL_DisplayMode closest;
    SDL_zero(closest);
    if (!SDL_GetClosestFullscreenDisplayMode(disp, desk->w, desk->h, desk->refresh_rate, true, &closest))
    {
        LL_WARNS("Window") << "Fullscreen: no exclusive mode matches the desktop mode " << desk->w << "x" << desk->h
                           << "@" << desk->refresh_rate << " on display " << (U32)disp << ": " << SDL_GetError()
                           << "; using borderless" << LL_ENDL;
        applyFullscreenMode(false);
        return false;
    }
    if (!SDL_SetWindowFullscreenMode(mWindow, &closest))
    {
        LL_WARNS("Window") << "Fullscreen: SDL_SetWindowFullscreenMode(exclusive " << closest.w << "x" << closest.h
                           << "@" << closest.refresh_rate << ") failed: " << SDL_GetError() << "; using borderless" << LL_ENDL;
        applyFullscreenMode(false);
        return false;
    }
    return true;
}

bool LLWindowSDL::setFullscreen(bool fullscreen, bool exclusive)
{
    if (!mWindow)
    {
        return false;
    }
    mInDialog = false;
    const char* driver = SDL_GetCurrentVideoDriver();
    const std::string drv = driver ? driver : "?";

    if (fullscreen)
    {
        if (!mFullscreen)
        {
            // Remember the windowed placement (logical units) to come back to.
            const SDL_WindowFlags flags = SDL_GetWindowFlags(mWindow);
            mWindowedMaximized = (flags & SDL_WINDOW_MAXIMIZED) != 0;
            mHaveWindowedRect = SDL_GetWindowPosition(mWindow, &mWindowedX, &mWindowedY)
                                && SDL_GetWindowSize(mWindow, &mWindowedW, &mWindowedH)
                                && mWindowedW > 0 && mWindowedH > 0;
        }
        if (exclusive && drv == "wayland")
        {
            // Wayland has no exclusive mode (SDL would only emulate a mode set by scaling);
            // the compositor decides direct scan-out of a fullscreen surface either way.
            LL_INFOS("Window") << "Fullscreen: exclusive requested, but Wayland has no exclusive mode; "
                                  "using borderless (the compositor decides direct scan-out)" << LL_ENDL;
            exclusive = false;
        }
        // (returns false when exclusive fell back to borderless)
        const bool exclusive_set = applyFullscreenMode(exclusive) && exclusive;
        if (!SDL_SetWindowFullscreen(mWindow, true))
        {
            LL_WARNS("Window") << "Fullscreen: SDL_SetWindowFullscreen(true) failed: " << SDL_GetError() << LL_ENDL;
            return false;
        }
        // Applied asynchronously on some windowing systems: wait (bounded by SDL) so the
        // state and the size the viewer reads next are the fullscreen ones.
        SDL_SyncWindow(mWindow);
        mFullscreen = true;
        mFullscreenExclusive = exclusive_set;
        LL_INFOS("Window") << "Fullscreen: entered, " << getFullscreenStateText() << LL_ENDL;
        return true;
    }

    if (!mFullscreen)
    {
        return true;
    }
    if (!SDL_SetWindowFullscreen(mWindow, false))
    {
        LL_WARNS("Window") << "Fullscreen: SDL_SetWindowFullscreen(false) failed: " << SDL_GetError() << LL_ENDL;
        return false;
    }
    SDL_SyncWindow(mWindow);
    mFullscreen = false;
    mFullscreenExclusive = false;
    if (mHaveWindowedRect)
    {
        if (mWindowedMaximized)
        {
            SDL_MaximizeWindow(mWindow);
        }
        else
        {
            SDL_SetWindowSize(mWindow, mWindowedW, mWindowedH);
            SDL_SetWindowPosition(mWindow, mWindowedX, mWindowedY);   // (ignored on Wayland)
        }
        SDL_SyncWindow(mWindow);
    }
    LL_INFOS("Window") << "Fullscreen: left, windowed " << mWindowedW << "x" << mWindowedH << " at "
                       << mWindowedX << "," << mWindowedY << (mWindowedMaximized ? " (maximized)" : "")
                       << ", video driver " << drv << LL_ENDL;
    return true;
}

std::string LLWindowSDL::getFullscreenStateText()
{
    const char* driver = SDL_GetCurrentVideoDriver();
    const std::string drv = driver ? driver : "?";
    if (!mWindow || !mFullscreen)
    {
        return "windowed, video driver " + drv;
    }
    std::ostringstream text;
    const SDL_DisplayID disp = SDL_GetDisplayForWindow(mWindow);
    const SDL_DisplayMode* mode = SDL_GetWindowFullscreenMode(mWindow);
    if (mode)
    {
        text << "exclusive fullscreen (SDL display mode " << mode->w << "x" << mode->h << "@" << mode->refresh_rate << " Hz)";
        if (drv == "x11")
        {
            text << " -- x11: a fullscreen window, which compositors generally unredirect";
        }
    }
    else
    {
        text << "borderless desktop fullscreen";
        if (drv == "wayland")
        {
            text << " -- wayland: the compositor decides direct scan-out";
        }
    }
    S32 pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(mWindow, &pw, &ph);
    const char* disp_name = disp ? SDL_GetDisplayName(disp) : nullptr;
    text << ", " << pw << "x" << ph << " pixels on display " << (U32)disp;
    if (disp_name)
    {
        text << " \"" << disp_name << "\"";
    }
    text << ", video driver " << drv;
    return text.str();
}

void LLWindowSDL::flashIcon(F32 seconds)
{
    // Requesting attention on the focused window is pointless, and on X11 the
    // urgency hint would linger until the next focus-in (#540).
    if (!mWindow || getFocused())
    {
        return;
    }
    // <GMDX> On Wayland SDL_FlashWindow is an xdg-activation request: compositors with
    // focus-on-activate (Hyprland misc:focus_on_activate, KWin's focus stealing policy)
    // hand the viewer the keyboard focus on every IM. There is no "attention only" request
    // there, so don't ask at all.
    if (const char* driver = SDL_GetCurrentVideoDriver(); driver && std::strcmp(driver, "wayland") == 0)
    {
        return;
    }
    // </GMDX>

    LL_DEBUGS("Window") << "LLWindowSDL::flashIcon(" << seconds << ")" << LL_ENDL;

    if (seconds <= 0.f)
    {
        // Stay highlighted until the user comes back; the focus-gained event
        // clears the bookkeeping (SDL/the OS drop the highlight themselves).
        mFlashUntilFocused = true;
    }
    else if (!mFlashUntilFocused)
    {
        F32 remaining_time = mFlashing ? mFlashTimer.getRemainingTimeF32().value() : 0.f;
        if (remaining_time < seconds)
            remaining_time = seconds;
        mFlashTimer.reset();
        mFlashTimer.setTimerExpirySec(remaining_time);
    }
    // else: an until-focused flash is active; a timed request must not
    // downgrade it to a timed one.

    SDL_FlashWindow(mWindow, SDL_FLASH_UNTIL_FOCUSED);
    mFlashing = true;
}

void LLWindowSDL::maybeStopFlashIcon()
{
    if (mFlashing && !mFlashUntilFocused && mFlashTimer.hasExpired())
    {
        mFlashing = false;
        SDL_FlashWindow( mWindow, SDL_FLASH_CANCEL );
    }
}

// Text inside the viewer uses '\n' line breaks only. The Windows clipboard holds
// CRLF and SDL hands it back verbatim, and the text editors turn each '\r' into a
// line break of its own, so every pasted break doubled (#491). Convert CRLF and
// lone CR to LF on the way in. On the way out, hand SDL LF-only text: it adds
// the CR before each LF itself on Windows, so a copy carries exactly one native
// line ending per break and a paste -> copy -> paste round trip is stable.
static void normalize_line_endings(LLWString& text)
{
    if (text.find('\r') == LLWString::npos)
    {
        return;
    }

    LLWString out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '\r')
        {
            out.push_back('\n');
            if (i + 1 < text.size() && text[i + 1] == '\n')
            {
                ++i;
            }
        }
        else
        {
            out.push_back(text[i]);
        }
    }
    text.swap(out);
}

bool LLWindowSDL::isClipboardTextAvailable()
{
    return SDL_HasClipboardText();
}

bool LLWindowSDL::pasteTextFromClipboard(LLWString &dst)
{
    if (isClipboardTextAvailable())
    {
        char* data = SDL_GetClipboardText();
        if (data)
        {
            dst = LLWString(utf8str_to_wstring(data));
            SDL_free(data);
            normalize_line_endings(dst);
            return true;
        }
    }
    return false;
}

bool LLWindowSDL::copyTextToClipboard(const LLWString& text)
{
    LLWString normalized(text);
    normalize_line_endings(normalized);
    const std::string utf8 = wstring_to_utf8str(normalized);
    return SDL_SetClipboardText(utf8.c_str());
}

// <GMDX> Image clipboard. SDL asks for the data by MIME type, when another application pastes
// (Wayland / X11) or at once (Windows), until the next clipboard change, when it calls the
// cleanup: the encoded images live until then. Windows takes only the first image type
// offered, so it gets image/bmp (CF_DIB, which every Windows program can paste); the others
// get image/png.
namespace
{
    struct ClipboardImage
    {
        LLPointer<LLImageFormatted> png;
        LLPointer<LLImageFormatted> bmp;
    };

    // The images on offer. Held here, NOT through SDL's userdata: on Wayland an offer can still
    // be asked for after SDL has replaced the clipboard data and run its cleanup (a paste that
    // started before the change), which read freed memory and crashed. The images live until
    // the next image copy replaces them (or the viewer exits), so a late request gets valid data.
    std::shared_ptr<ClipboardImage> sClipboardImage;

    const void* SDLCALL clipboard_image_data(void* /*userdata*/, const char* mime_type, size_t* size)
    {
        const ClipboardImage* clip = sClipboardImage.get();
        const LLImageFormatted* image = nullptr;
        if (clip && mime_type)
        {
            if (!strcmp(mime_type, "image/png"))
            {
                image = clip->png;
            }
            else if (!strcmp(mime_type, "image/bmp"))
            {
                image = clip->bmp;
            }
        }
        if (!image || !image->getData())
        {
            if (size)
            {
                *size = 0;
            }
            return nullptr;
        }
        if (size)
        {
            *size = (size_t)image->getDataSize();
        }
        return image->getData();
    }

    void SDLCALL clipboard_image_cleanup(void* /*userdata*/)
    {
        // (nothing: sClipboardImage outlives late requests; the next copy replaces it)
    }

    void clear_clipboard_image()
    {
        sClipboardImage.reset();
    }
}

bool LLWindowSDL::copyImageToClipboard(const LLImageRaw* image)
{
    if (!image || image->isBufferInvalid() || image->getWidth() <= 0 || image->getHeight() <= 0)
    {
        return false;
    }

    std::shared_ptr<ClipboardImage> clip = std::make_shared<ClipboardImage>();
    std::vector<const char*> mime_types;
#if LL_WINDOWS
    LLPointer<LLImageBMP> bmp = new LLImageBMP();
    if (bmp->encode(image, 0.f) && bmp->getDataSize() > 54)
    {
        // LLImageBMP leaves biSizeImage 0 (valid for BI_RGB), but SDL copies biSizeImage
        // bytes of pixels into the DIB: fill it in (the pixel bytes after bfOffBits).
        U8* data = bmp->getData();
        const U32 offset = data[10] | (data[11] << 8) | (data[12] << 16) | ((U32)data[13] << 24);
        const U32 pixels = (U32)bmp->getDataSize() - offset;
        data[34] = (U8)pixels;
        data[35] = (U8)(pixels >> 8);
        data[36] = (U8)(pixels >> 16);
        data[37] = (U8)(pixels >> 24);
        clip->bmp = bmp;
        mime_types.push_back("image/bmp");
    }
#endif
    LLPointer<LLImagePNG> png = new LLImagePNG();
    if (png->encode(image, 0.f))
    {
        clip->png = png;
        mime_types.push_back("image/png");
    }
    if (mime_types.empty())
    {
        LL_WARNS("Window") << "Could not encode the image for the clipboard" << LL_ENDL;
        return false;
    }

    sClipboardImage = clip;   // (replaces the previous image; see sClipboardImage)
    if (!SDL_SetClipboardData(clipboard_image_data, clipboard_image_cleanup, nullptr,
                              mime_types.data(), mime_types.size()))
    {
        LL_WARNS("Window") << "Could not put the image on the clipboard: " << SDL_GetError() << LL_ENDL;
        return false;
    }
    return true;
}
// </GMDX>

bool LLWindowSDL::isPrimaryTextAvailable()
{
    return SDL_HasPrimarySelectionText();
}

bool LLWindowSDL::pasteTextFromPrimary(LLWString &dst)
{
    if (isPrimaryTextAvailable())
    {
        char* data = SDL_GetPrimarySelectionText();
        if (data)
        {
            dst = LLWString(utf8str_to_wstring(data));
            SDL_free(data);
            normalize_line_endings(dst);
            return true;
        }
    }
    return false;
}

bool LLWindowSDL::copyTextToPrimary(const LLWString& text)
{
    LLWString normalized(text);
    normalize_line_endings(normalized);
    const std::string utf8 = wstring_to_utf8str(normalized);
    return SDL_SetPrimarySelectionText(utf8.c_str());
}

LLWindow::LLWindowResolution* LLWindowSDL::getSupportedResolutions(S32 &num_resolutions)
{
    if (!mSupportedResolutions)
    {
        mSupportedResolutions = new LLWindowResolution[MAX_NUM_RESOLUTIONS];
        mNumSupportedResolutions = 0;

        SDL_DisplayID display = SDL_GetPrimaryDisplay();
        int num_modes = 0;
        SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(display, &num_modes);
        num_modes = llclamp(num_modes, 0, MAX_NUM_RESOLUTIONS);
        if (modes) {
            for (int i = 0; i < num_modes; ++i) {
                SDL_DisplayMode *mode = modes[i];
                int w = mode->w;
                int h = mode->h;
                if ((w >= 800) && (h >= 600))
                {
                    // make sure we don't add the same resolution multiple times!
                    if ( (mNumSupportedResolutions == 0) ||
                        ((mSupportedResolutions[mNumSupportedResolutions-1].mWidth != w) &&
                         (mSupportedResolutions[mNumSupportedResolutions-1].mHeight != h)) )
                    {
                        mSupportedResolutions[mNumSupportedResolutions].mWidth = w;
                        mSupportedResolutions[mNumSupportedResolutions].mHeight = h;
                        mNumSupportedResolutions++;
                    }
                }
            }
            SDL_free(modes);
        }
    }

    num_resolutions = mNumSupportedResolutions;
    return mSupportedResolutions;
}

//static
std::vector<std::string> LLWindowSDL::getDisplaysResolutionList()
{
    std::vector<std::string> ret;
    if (gWindowImplementation)
    {
        S32 resolutionCount = 0;
        LLWindowResolution* resolutionList = gWindowImplementation->getSupportedResolutions(resolutionCount);
        if (resolutionList != nullptr)
        {
            for (S32 i = 0; i < resolutionCount; i++)
            {
                const LLWindowResolution& resolution = resolutionList[i];
                ret.push_back(std::to_string(resolution.mWidth) + "x" + std::to_string(resolution.mHeight));
            }
        }
    }
    return ret;
}


bool LLWindowSDL::convertCoords(LLCoordGL from, LLCoordWindow *to)
{
    if (!to)
        return false;

    if (!mWindow)
        return false;
    S32 height;
    SDL_GetWindowSizeInPixels(mWindow, nullptr, &height);

    to->mX = from.mX;
    to->mY = height - from.mY - 1;

    return true;
}

bool LLWindowSDL::convertCoords(LLCoordWindow from, LLCoordGL* to)
{
    if (!to)
        return false;

    if (!mWindow)
        return false;
    S32 height;
    SDL_GetWindowSizeInPixels(mWindow, nullptr, &height);

    to->mX = from.mX;
    to->mY = height - from.mY - 1;

    return true;
}

bool LLWindowSDL::convertCoords(LLCoordScreen from, LLCoordWindow* to)
{
    if (!to)
        return false;

    // In the fullscreen case, window and screen coordinates are the same.
    to->mX = from.mX;
    to->mY = from.mY;
    return true;
}

bool LLWindowSDL::convertCoords(LLCoordWindow from, LLCoordScreen *to)
{
    if (!to)
        return false;

    // In the fullscreen case, window and screen coordinates are the same.
    to->mX = from.mX;
    to->mY = from.mY;
    return true;
}

bool LLWindowSDL::convertCoords(LLCoordScreen from, LLCoordGL *to)
{
    LLCoordWindow window_coord;

    return convertCoords(from, &window_coord) && convertCoords(window_coord, to);
}

bool LLWindowSDL::convertCoords(LLCoordGL from, LLCoordScreen *to)
{
    LLCoordWindow window_coord;

    return convertCoords(from, &window_coord) && convertCoords(window_coord, to);
}

void LLWindowSDL::setupFailure(const std::string& text, const std::string& caption, U32 type)
{
    close();

    OSMessageBox(text, caption, type);
}

bool LLWindowSDL::SDLReallyCaptureInput(bool capture)
{
    if (!mFullscreen && mWindow ) /* only bother if we're windowed anyway */
    {
        SDL_SetWindowMouseGrab(mWindow, capture);
    }

    return capture;
}

// The keycode handed to LLKeyboard for an SDL key event. Shortcuts follow the character the
// layout puts on the key, as LL's keyboards always have (Ctrl+Z on AZERTY is the key marked Z).
// When that character is one LL has no KEY for (a dead key such as German '^', or a non-ASCII
// character such as French '²', German 'ö' or the Czech number row), the event used to be
// dropped, so bindings on ` ; ' [ ] \ , . / - = and the digits could never be reached on
// those layouts. Such keys fall back to the US-layout key at the same physical position,
// which is what SDL scancodes name: Ctrl+` is then the key left of 1 on every layout.
// Key-up and key-down map identically (SDL3 keycodes in events ignore the modifiers).
static LLKeyboard::NATIVE_KEY_TYPE sdl_bindable_keycode(SDL_Keycode key, SDL_Scancode scancode)
{
    // The key left of 1 is the grave key for shortcuts whenever it types punctuation, as
    // Windows reports it (VK_OEM_3) on such layouts: Brazilian ABNT2 types ' there (its `
    // is a dead key elsewhere), so Ctrl+` (Snapshot to Disk) was Ctrl+' and never matched.
    // A letter or digit there (Hungarian 0) keeps its own meaning.
    // Anything else (dead keys such as German '^', SDLK_UNKNOWN, non-ASCII like Swiss '\xc2\xa7') is
    // the grave key too, so Ctrl+` resolves by position on QWERTZ and other layouts.
    if (scancode == SDL_SCANCODE_GRAVE && !(key < 0x80 && isalnum((int)key)))
    {
        return SDLK_GRAVE;
    }

    KEY translated = 0;
    if (!gKeyboard || gKeyboard->translateKey(key, &translated))
    {
        return key;
    }

    switch (scancode)
    {
        case SDL_SCANCODE_GRAVE:        return SDLK_GRAVE;
        case SDL_SCANCODE_MINUS:        return SDLK_MINUS;
        case SDL_SCANCODE_EQUALS:       return SDLK_EQUALS;
        case SDL_SCANCODE_LEFTBRACKET:  return SDLK_LEFTBRACKET;
        case SDL_SCANCODE_RIGHTBRACKET: return SDLK_RIGHTBRACKET;
        case SDL_SCANCODE_BACKSLASH:
        case SDL_SCANCODE_NONUSHASH:    return SDLK_BACKSLASH;
        case SDL_SCANCODE_SEMICOLON:    return SDLK_SEMICOLON;
        case SDL_SCANCODE_APOSTROPHE:   return SDLK_APOSTROPHE;
        case SDL_SCANCODE_COMMA:        return SDLK_COMMA;
        case SDL_SCANCODE_PERIOD:       return SDLK_PERIOD;
        case SDL_SCANCODE_SLASH:        return SDLK_SLASH;
        case SDL_SCANCODE_0:            return SDLK_0;
        default:
            if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9)
            {
                return SDLK_1 + (scancode - SDL_SCANCODE_1);
            }
            return key;
    }
}

U32 LLWindowSDL::SDLCheckGrabbyKeys(U32 keysym, bool gain)
{
    /* part of the fix for SL-13243: Some popular window managers like
       to totally eat alt-drag for the purposes of moving windows.  We
       spoil their day by acquiring the exclusive X11 mouse lock for as
       long as ALT is held down, so the window manager can't easily
       see what's happening.  Tested successfully with Metacity.
       And... do the same with CTRL, for other darn WMs.  We don't
       care about other metakeys as SL doesn't use them with dragging
       (for now). */

    /* We maintain a bitmap of critical keys which are up and down
       instead of simply key-counting, because SDL sometimes reports
       misbalanced keyup/keydown event pairs to us for whatever reason. */

    U32 mask = 0;
    switch (keysym)
    {
        case SDLK_LALT:
            mask = 1U << 0; break;
        case SDLK_RALT:
            mask = 1U << 1; break;
        case SDLK_LCTRL:
            mask = 1U << 2; break;
        case SDLK_RCTRL:
            mask = 1U << 3; break;
        default:
            break;
    }

    if (gain)
        mGrabbyKeyFlags |= mask;
    else
        mGrabbyKeyFlags &= ~mask;

    //LL_INFOS() << "mGrabbyKeyFlags=" << mGrabbyKeyFlags << LL_ENDL;

    /* 0 means we don't need to mousegrab, otherwise grab. */
    return mGrabbyKeyFlags;
}


void check_vm_bloat()
{
#if LL_LINUX
    // watch our own VM and RSS sizes, warn if we bloated rapidly
    static const std::string STATS_FILE = "/proc/self/stat";
    FILE *fp = fopen(STATS_FILE.c_str(), "r");
    if (fp)
    {
        static long long last_vm_size = 0;
        static long long last_rss_size = 0;
        const long long significant_vm_difference = 250 * 1024*1024;
        const long long significant_rss_difference = 50 * 1024*1024;
        long long this_vm_size = 0;
        long long this_rss_size = 0;

        ssize_t res;
        size_t dummy;
        char *ptr = nullptr;
        for (int i=0; i<22; ++i) // parse past the values we don't want
        {
            res = getdelim(&ptr, &dummy, ' ', fp);
            if (-1 == res)
            {
                LL_WARNS() << "Unable to parse " << STATS_FILE << LL_ENDL;
                goto finally;
            }
            free(ptr);
            ptr = nullptr;
        }
        // 23rd space-delimited entry is vsize
        res = getdelim(&ptr, &dummy, ' ', fp);
        llassert(ptr);
        if (-1 == res)
        {
            LL_WARNS() << "Unable to parse " << STATS_FILE << LL_ENDL;
            goto finally;
        }
        this_vm_size = atoll(ptr);
        free(ptr);
        ptr = nullptr;
        // 24th space-delimited entry is RSS
        res = getdelim(&ptr, &dummy, ' ', fp);
        llassert(ptr);
        if (-1 == res)
        {
            LL_WARNS() << "Unable to parse " << STATS_FILE << LL_ENDL;
            goto finally;
        }
        this_rss_size = getpagesize() * atoll(ptr);
        free(ptr);
        ptr = nullptr;

        LL_INFOS() << "VM SIZE IS NOW " << (this_vm_size/(1024*1024)) << " MB, RSS SIZE IS NOW " << (this_rss_size/(1024*1024)) << " MB" << LL_ENDL;

        if (llabs(last_vm_size - this_vm_size) > significant_vm_difference)
        {
            if (this_vm_size > last_vm_size)
            {
                LL_WARNS() << "VM size grew by " << (this_vm_size - last_vm_size)/(1024*1024) << " MB in last frame" << LL_ENDL;
            }
            else
            {
                LL_INFOS() << "VM size shrank by " << (last_vm_size - this_vm_size)/(1024*1024) << " MB in last frame" << LL_ENDL;
            }
        }

        if (llabs(last_rss_size - this_rss_size) > significant_rss_difference)
        {
            if (this_rss_size > last_rss_size)
            {
                LL_WARNS() << "RSS size grew by " << (this_rss_size - last_rss_size)/(1024*1024) << " MB in last frame" << LL_ENDL;
            }
            else
            {
                LL_INFOS() << "RSS size shrank by " << (last_rss_size - this_rss_size)/(1024*1024) << " MB in last frame" << LL_ENDL;
            }
        }

        last_rss_size = this_rss_size;
        last_vm_size = this_vm_size;

finally:
        if (ptr)
        {
            free(ptr);
            ptr = nullptr;
        }
        fclose(fp);
    }
#endif // LL_LINUX
}


// virtual
void LLWindowSDL::processMiscNativeEvents()
{
#if LL_GLIB
    // Pump until we've nothing left to do or used this frame's slice.
    // Anything still pending is picked up next frame, so glib (media, dbus,
    // portal) is never starved but can't stall a frame for tens of ms either.
    constexpr F32 GLIB_PUMP_SLICE_SEC = 0.004f;
    GMainContext* ctx = g_main_context_default();
    static LLTimer pump_timer;
    pump_timer.reset();
    pump_timer.setTimerExpirySec(GLIB_PUMP_SLICE_SEC);
    do
    {
        g_main_context_iteration(ctx, false);
    } while (g_main_context_pending(ctx) && !pump_timer.hasExpired());
#endif

    // hack - doesn't belong here - but this is just for debugging
    // The environment is read once; nothing sets LL_DEBUG_BLOAT at runtime.
    static const bool debug_bloat = getenv("LL_DEBUG_BLOAT") != nullptr;
    if (debug_bloat)
    {
        check_vm_bloat();
    }
}

void LLWindowSDL::gatherInput(bool app_has_focus)
{
#if !LL_SDL_APP
    SDL_Event event;

    // Handle all outstanding SDL events -- unless a held pick asked to leave them for a frame
    // (holdInputFrame: the events after a click are handled after its pick callback).
    if (mDelayInput)
    {
        mDelayInput = false;
    }
    else
    {
        while (SDL_PollEvent(&event))
        {
            handleEvent(event, app_has_focus);
        }
    }
#endif
    LLGameControl::processEvents(app_has_focus);

    updateCursor();

    // This is a good time to stop flashing the icon if our mFlashTimer has
    // expired (timed flashes only; until-focused flashes end on focus gain).
    maybeStopFlashIcon();
}

SDL_AppResult LLWindowSDL::handleEvent(const SDL_Event& event, bool app_has_focus)
{
    switch(event.type)
    {
        case SDL_EVENT_MOUSE_MOTION:
        {
            // Accumulate raw relative motion. Under SDL3 relative mouse mode
            // (used during camera capture on Wayland) these come from the
            // underlying input device and are unconstrained by cursor position
            // or window edges -- the only deltas that work correctly when
            // SDL_WarpMouseInWindow is a no-op.
            // xrel/yrel are SDL logical units; hand out pixels like every other
            // coordinate this class reports (toPixels).
            mAccumRelX += event.motion.xrel * pixelDensity();
            mAccumRelY += event.motion.yrel * pixelDensity();

            LLCoordWindow winCoord(toPixels(event.motion.x), toPixels(event.motion.y));
            LLCoordGL openGlCoord;
            convertCoords(winCoord, &openGlCoord);
            mCallbacks->handleMouseMove(this, openGlCoord, gKeyboard->currentMask(true));
            break;
        }

        case SDL_EVENT_MOUSE_WHEEL:
        {
            // SDL3 reports float notches. Flooring each event dropped small positive
            // fractions (0.25 -> 0) and turned small negative ones into a full notch
            // (-0.25 -> -1), so smooth-scrolling wheels and touchpads scrolled one way only.
            // Accumulate and deliver whole steps; a direction change drops the leftover.
            auto steps = [](F32& acc, F32 delta) -> S32
            {
                if (delta == 0.f) return 0;
                if ((acc > 0.f && delta < 0.f) || (acc < 0.f && delta > 0.f)) acc = 0.f;
                acc += delta;
                const S32 whole = (S32)acc;   // truncates toward zero
                acc -= (F32)whole;
                return whole;
            };
            const S32 dy = steps(mWheelAccumY, event.wheel.y);
            const S32 dx = steps(mWheelAccumX, event.wheel.x);
            // Smooth scrolling gets the fractional deltas too (and consumes the event).
            if ((event.wheel.y != 0.f || event.wheel.x != 0.f)
                && mCallbacks->handleScrollWheelSmooth(this, -event.wheel.y, -event.wheel.x, -dy, -dx))
            {
                break;
            }
            if (dy)
            {
                mCallbacks->handleScrollWheel(this, -dy);
            }
            if (dx)
            {
                mCallbacks->handleScrollHWheel(this, -dx);
            }
            break;
        }

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        {
            LLCoordWindow winCoord(toPixels(event.button.x), toPixels(event.button.y));
            LLCoordGL openGlCoord;
            convertCoords(winCoord, &openGlCoord);
            MASK mask = gKeyboard->currentMask(true);

            if (event.button.button == SDL_BUTTON_LEFT)  // left
            {
                if (event.button.clicks == 2)
                    mCallbacks->handleDoubleClick(this, openGlCoord, mask);
                else
                    mCallbacks->handleMouseDown(this, openGlCoord, mask);
            }
            else if (event.button.button == SDL_BUTTON_RIGHT)
            {
                mCallbacks->handleRightMouseDown(this, openGlCoord, mask);
            }
            else if (event.button.button == SDL_BUTTON_MIDDLE)  // middle
            {
                mCallbacks->handleMiddleMouseDown(this, openGlCoord, mask);
            }
            else
            {
                mCallbacks->handleOtherMouseDown(this, openGlCoord, mask, event.button.button);
            }

            break;
        }

        case SDL_EVENT_MOUSE_BUTTON_UP:
        {
            LLCoordWindow winCoord(toPixels(event.button.x), toPixels(event.button.y));
            LLCoordGL openGlCoord;
            convertCoords(winCoord, &openGlCoord);
            MASK mask = gKeyboard->currentMask(true);

            if (event.button.button == SDL_BUTTON_LEFT)  // left
            {
                mCallbacks->handleMouseUp(this, openGlCoord, mask);
            }
            else if (event.button.button == SDL_BUTTON_RIGHT)  // right
            {
                mCallbacks->handleRightMouseUp(this, openGlCoord, mask);
            }
            else if (event.button.button == SDL_BUTTON_MIDDLE)  // middle
            {
                mCallbacks->handleMiddleMouseUp(this, openGlCoord, mask);
            }
            else
            {
                mCallbacks->handleOtherMouseUp(this, openGlCoord, mask, event.button.button);
            }

            break;
        }

        case SDL_EVENT_KEY_DOWN:
        {
            mKeyVirtualKey = event.key.key;
            mKeyModifiers = event.key.mod;

            if (mKeyVirtualKey == SDLK_RETURN2 || mKeyVirtualKey == SDLK_KP_ENTER)
            {
                mKeyVirtualKey = SDLK_RETURN;
            }

            gKeyboard->handleKeyDown(sdl_bindable_keycode(mKeyVirtualKey, event.key.scancode), mKeyModifiers);

            // AltGr must not count as Ctrl (#604): SDL reports it as SDL_KMOD_MODE, or on
            // Windows as LCTRL+RALT (the OS synthesizes a left Ctrl for AltGr).
            bool altgr = (mKeyModifiers & SDL_KMOD_MODE) != 0;
#if LL_WINDOWS
            altgr = altgr || ((mKeyModifiers & SDL_KMOD_RALT) && (mKeyModifiers & SDL_KMOD_LCTRL)
                              && !(mKeyModifiers & SDL_KMOD_RCTRL));
#endif
            const bool ctrl_shortcut = (mKeyModifiers & SDL_KMOD_GUI) || ((mKeyModifiers & SDL_KMOD_CTRL) && !altgr);

            // Focused web media is unaffected by the ctrl_shortcut skip: media focus turns
            // language text input on (LLViewerMediaFocus::onFocusReceived, #525), so this
            // forced pass never ran for it except for Return, and the media key-down path
            // already swallows Ctrl+letter (LLViewerMediaImpl::handleKeyHere).
            if (mKeyVirtualKey == SDLK_RETURN
                || (!mLanguageTextInputAllowed && !ctrl_shortcut))
            {
                // Return reaches focused media a second time through this character
                // pass (LLViewerWindow::handleUnicodeChar -> handleKey(KEY_RETURN)); tag
                // it so the CEF plugin sends the character, not another key-down (#525).
                mInKeyCharPass = true;
                mCallbacks->handleUnicodeChar(mKeyVirtualKey, gKeyboard->currentMask(false));
                mInKeyCharPass = false;
            }

            // part of the fix for SL-13243
            if (SDLCheckGrabbyKeys(mKeyVirtualKey, true) != 0)
                SDLReallyCaptureInput(true);
            break;
        }

        case SDL_EVENT_KEY_UP:
        {
            mKeyVirtualKey = event.key.key;
            mKeyModifiers = event.key.mod;

            if (mKeyVirtualKey == SDLK_RETURN2 || mKeyVirtualKey == SDLK_KP_ENTER)
            {
                mKeyVirtualKey = SDLK_RETURN;
            }

            if (SDLCheckGrabbyKeys(mKeyVirtualKey, false) == 0)
                SDLReallyCaptureInput(false); // part of the fix for SL-13243

            gKeyboard->handleKeyUp(sdl_bindable_keycode(mKeyVirtualKey, event.key.scancode), mKeyModifiers);
            break;
        }

        case SDL_EVENT_TEXT_INPUT:
        {
            auto string = utf8str_to_wstring(event.text.text);
            for (auto key : string)
            {
                {
                    mCallbacks->handleUnicodeChar(key, gKeyboard->currentMask(false));
                }
            }
            break;
        }

        case SDL_EVENT_WINDOW_EXPOSED:
        {
            mCallbacks->handlePaint(this, 0, 0, 0, 0);
            break;
        }

        case SDL_EVENT_WINDOW_RESIZED:
            break;   // logical size; the pixel-size event below drives the viewer
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        {
            LL_INFOS() << "Handling a resize event: " << event.window.data1 << "x" << event.window.data2 << " pixels" << LL_ENDL;
            S32 width = llmax(event.window.data1, (S32)mMinWindowWidth);
            S32 height = llmax(event.window.data2, (S32)mMinWindowHeight);

            mCallbacks->handleResize(this, width, height);
            break;
        }
        case SDL_EVENT_WINDOW_MOUSE_ENTER:
            break;
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            mCallbacks->handleMouseLeave(this);
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            //SDL_SetWindowKeyboardGrab(mWindow, true);
            if (mFlashing)
            {
                // The user is back: drop any taskbar highlight (timed or
                // until-focused) and reset so the next request flashes again.
                SDL_FlashWindow(mWindow, SDL_FLASH_CANCEL);
                mFlashing = false;
                mFlashUntilFocused = false;
            }
            mCallbacks->handleFocus(this);
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            mCallbacks->handleFocusLost(this);
            //SDL_SetWindowKeyboardGrab(mWindow, false);
            break;
        case SDL_EVENT_WINDOW_RESTORED:
            mCallbacks->handleActivate(this, true);
            break;
        case SDL_EVENT_WINDOW_MAXIMIZED:
            mCallbacks->handleActivate(this, true);
            break;
        case SDL_EVENT_WINDOW_MINIMIZED:
            mCallbacks->handleActivate(this, false);
            break;
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        {
            mCallbacks->handleDisplayChanged();
            // The pixel size may be unchanged (or its event reordered) when the window
            // lands on a monitor with a different size / DPI: resync from the live size.
            S32 w = 0, h = 0;
            if (SDL_GetWindowSizeInPixels(mWindow, &w, &h) && w > 0 && h > 0)
            {
                mCallbacks->handleResize(this, llmax(w, (S32)mMinWindowWidth), llmax(h, (S32)mMinWindowHeight));
            }
            break;
        }
        case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
        case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
        {
            // #919: compare the LIVE state, not the event type: our own setFullscreen() calls
            // queue these too, and a quick toggle would otherwise be undone by its stale
            // first event. A difference (outside the temporary windowed state of a native
            // dialog) is the window manager's doing: adopt it.
            const bool live = mWindow && (SDL_GetWindowFlags(mWindow) & SDL_WINDOW_FULLSCREEN) != 0;
            if (!mInDialog && live != mFullscreen)
            {
                LL_INFOS("Window") << "Fullscreen: the window manager " << (live ? "entered" : "left")
                                   << " fullscreen" << LL_ENDL;
                mFullscreen = live;
                if (!live)
                {
                    mFullscreenExclusive = false;
                }
                mCallbacks->handleFullscreenChanged(this, live);
            }
            break;
        }
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        {
            S32 w = 0, h = 0;
            SDL_GetWindowSizeInPixels(mWindow, &w, &h);
            LL_INFOS() << "Display scale changed: " << getSystemUISize() << ", pixel density " << pixelDensity() << LL_ENDL;
            mCallbacks->handleDPIChanged(this, getSystemUISize(), w, h);
            break;
        }
        //case SDL_EVENT_WINDOW_SHOWN:
        //case SDL_EVENT_WINDOW_HIDDEN:
        //{
        //    bool hidden = (flags & SDL_WINDOW_HIDDEN);
        //    mCallbacks->handleActivate(this, !hidden);
        //    break;
        //}
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            if(mCallbacks->handleCloseRequest(this))
            {
                // Get the app to initiate cleanup.
                mCallbacks->handleQuit(this);
                // The app is responsible for calling destroyWindow when done.
            }
            break;
        default:
            LLGameControl::handleEvent(event, app_has_focus);
            break;
    }

    return SDL_APP_CONTINUE;
}

// static
SDL_AppResult LLWindowSDL::handleEvents(const SDL_Event& event, bool app_has_focus)
{
    if(!gWindowImplementation) return SDL_APP_CONTINUE;

    return gWindowImplementation->handleEvent(event, app_has_focus);
}

static SDL_Cursor *makeSDLCursorFromBMP(const char *filename, int hotx, int hoty)
{
    SDL_Cursor *sdlcursor = nullptr;
    SDL_Surface *bmpsurface;

    // Load cursor pixel data from BMP file
    bmpsurface = Load_BMP_Resource(filename);
    if (bmpsurface && bmpsurface->w%8==0)
    {
        SDL_Surface *cursurface;
        LL_DEBUGS() << "Loaded cursor file " << filename << " "
                    << bmpsurface->w << "x" << bmpsurface->h << LL_ENDL;
        SDL_PixelFormat pix_format = SDL_GetPixelFormatForMasks(32,
            SDL_Swap32LE(0xFFU),
            SDL_Swap32LE(0xFF00U),
            SDL_Swap32LE(0xFF0000U),
            SDL_Swap32LE(0xFF000000U));
        if (pix_format == SDL_PIXELFORMAT_UNKNOWN)
        {
            return nullptr;
        }

        const SDL_PixelFormatDetails* pix_format_details = SDL_GetPixelFormatDetails(pix_format);
        if(!pix_format_details)
        {
            return nullptr;
        }

        cursurface = SDL_CreateSurface(bmpsurface->w,
                                           bmpsurface->h,
                                           pix_format);
        SDL_FillSurfaceRect(cursurface, nullptr, SDL_Swap32LE(0x00000000U));

        // Blit the cursor pixel data onto a 32-bit RGBA surface so we
        // only have to cope with processing one type of pixel format.
        if (SDL_BlitSurface(bmpsurface, nullptr,
                                 cursurface, nullptr))
        {
            // n.b. we already checked that width is a multiple of 8.
            const int bitmap_bytes = (cursurface->w * cursurface->h) / 8;
            unsigned char *cursor_data = new unsigned char[bitmap_bytes];
            unsigned char *cursor_mask = new unsigned char[bitmap_bytes];
            memset(cursor_data, 0, bitmap_bytes);
            memset(cursor_mask, 0, bitmap_bytes);
            int i,j;
            // Walk the RGBA cursor pixel data, extracting both data and
            // mask to build SDL-friendly cursor bitmaps from.  The mask
            // is inferred by color-keying against 200,200,200
            for (i=0; i<cursurface->h; ++i) {
                for (j=0; j<cursurface->w; ++j) {
                    U8 *pixelp =
                            ((U8*)cursurface->pixels)
                            + cursurface->pitch * i
                            + j*pix_format_details->bytes_per_pixel;
                    U8 srcred = pixelp[0];
                    U8 srcgreen = pixelp[1];
                    U8 srcblue = pixelp[2];
                    bool mask_bit = (srcred != 200)
                                    || (srcgreen != 200)
                                    || (srcblue != 200);
                    bool data_bit = mask_bit && (srcgreen <= 80);//not 0x80
                    unsigned char bit_offset = (cursurface->w/8) * i
                                               + j/8;
                    cursor_data[bit_offset] |= (data_bit) << (7 - (j&7));
                    cursor_mask[bit_offset] |= (mask_bit) << (7 - (j&7));
                }
            }
            sdlcursor = SDL_CreateCursor((Uint8*)cursor_data,
                                         (Uint8*)cursor_mask,
                                         cursurface->w, cursurface->h,
                                         hotx, hoty);
            delete[] cursor_data;
            delete[] cursor_mask;
        } else {
            LL_WARNS() << "CURSOR BLIT FAILURE, cursurface: " << cursurface << LL_ENDL;
        }
        SDL_DestroySurface(cursurface);
        SDL_DestroySurface(bmpsurface);
    } else {
        LL_WARNS() << "CURSOR LOAD FAILURE " << filename << LL_ENDL;
    }

    return sdlcursor;
}



void LLWindowSDL::updateCursor()
{
    if (mCurrentCursor != mNextCursor)
    {
        if (mNextCursor < UI_CURSOR_COUNT)
        {
            SDL_Cursor *sdlcursor = mSDLCursors[mNextCursor];
            // Try to default to the arrow for any cursors that
            // did not load correctly.
            if (!sdlcursor && mSDLCursors[UI_CURSOR_ARROW])
                sdlcursor = mSDLCursors[UI_CURSOR_ARROW];
            if (sdlcursor)
                SDL_SetCursor(sdlcursor);

            mCurrentCursor = mNextCursor;
        }
        else
        {
            LL_WARNS() << "Tried to set invalid cursor number " << mNextCursor << LL_ENDL;
        }
    }
}

void LLWindowSDL::initCursors()
{
    // Blank the cursor pointer array for those we may miss.
    for (int i=0; i<UI_CURSOR_COUNT; ++i)
    {
        mSDLCursors[i] = nullptr;
    }

    // Pre-make an SDL cursor for each of the known cursor types.
    // We hardcode the hotspots - to avoid that we'd have to write
    // a .cur file loader.
    // NOTE: SDL doesn't load RLE-compressed BMP files.
    mSDLCursors[UI_CURSOR_ARROW] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    mSDLCursors[UI_CURSOR_WAIT] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_WAIT);
    mSDLCursors[UI_CURSOR_HAND] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
    mSDLCursors[UI_CURSOR_IBEAM] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_TEXT);
    mSDLCursors[UI_CURSOR_CROSS] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_CROSSHAIR);
    mSDLCursors[UI_CURSOR_SIZENWSE] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NWSE_RESIZE);
    mSDLCursors[UI_CURSOR_SIZENESW] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NESW_RESIZE);
    mSDLCursors[UI_CURSOR_SIZEWE] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_EW_RESIZE);
    mSDLCursors[UI_CURSOR_SIZENS] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NS_RESIZE);
    mSDLCursors[UI_CURSOR_SIZEALL] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_MOVE);
    mSDLCursors[UI_CURSOR_NO] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NOT_ALLOWED);
    mSDLCursors[UI_CURSOR_WORKING] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_PROGRESS);
    mSDLCursors[UI_CURSOR_TOOLGRAB] = makeSDLCursorFromBMP("lltoolgrab.BMP",2,13);
    mSDLCursors[UI_CURSOR_TOOLLAND] = makeSDLCursorFromBMP("lltoolland.BMP",1,6);
    mSDLCursors[UI_CURSOR_TOOLFOCUS] = makeSDLCursorFromBMP("lltoolfocus.BMP",8,5);
    mSDLCursors[UI_CURSOR_TOOLCREATE] = makeSDLCursorFromBMP("lltoolcreate.BMP",7,7);
    mSDLCursors[UI_CURSOR_ARROWDRAG] = makeSDLCursorFromBMP("arrowdrag.BMP",0,0);
    mSDLCursors[UI_CURSOR_ARROWCOPY] = makeSDLCursorFromBMP("arrowcop.BMP",0,0);
    mSDLCursors[UI_CURSOR_ARROWDRAGMULTI] = makeSDLCursorFromBMP("llarrowdragmulti.BMP",0,0);
    mSDLCursors[UI_CURSOR_ARROWCOPYMULTI] = makeSDLCursorFromBMP("arrowcopmulti.BMP",0,0);
    mSDLCursors[UI_CURSOR_NOLOCKED] = makeSDLCursorFromBMP("llnolocked.BMP",8,8);
    mSDLCursors[UI_CURSOR_ARROWLOCKED] = makeSDLCursorFromBMP("llarrowlocked.BMP",0,0);
    mSDLCursors[UI_CURSOR_GRABLOCKED] = makeSDLCursorFromBMP("llgrablocked.BMP",2,13);
    mSDLCursors[UI_CURSOR_TOOLTRANSLATE] = makeSDLCursorFromBMP("lltooltranslate.BMP",0,0);
    mSDLCursors[UI_CURSOR_TOOLROTATE] = makeSDLCursorFromBMP("lltoolrotate.BMP",0,0);
    mSDLCursors[UI_CURSOR_TOOLSCALE] = makeSDLCursorFromBMP("lltoolscale.BMP",0,0);
    mSDLCursors[UI_CURSOR_TOOLCAMERA] = makeSDLCursorFromBMP("lltoolcamera.BMP",7,5);
    mSDLCursors[UI_CURSOR_TOOLPAN] = makeSDLCursorFromBMP("lltoolpan.BMP",7,5);
    mSDLCursors[UI_CURSOR_TOOLZOOMIN] = makeSDLCursorFromBMP("lltoolzoomin.BMP",7,5);
    mSDLCursors[UI_CURSOR_TOOLZOOMOUT] = makeSDLCursorFromBMP("lltoolzoomout.BMP", 7, 5);
    mSDLCursors[UI_CURSOR_TOOLPICKOBJECT3] = makeSDLCursorFromBMP("toolpickobject3.BMP",0,0);
    mSDLCursors[UI_CURSOR_TOOLPLAY] = makeSDLCursorFromBMP("toolplay.BMP",0,0);
    mSDLCursors[UI_CURSOR_TOOLPAUSE] = makeSDLCursorFromBMP("toolpause.BMP",0,0);
    mSDLCursors[UI_CURSOR_TOOLMEDIAOPEN] = makeSDLCursorFromBMP("toolmediaopen.BMP",0,0);
    mSDLCursors[UI_CURSOR_PIPETTE] = makeSDLCursorFromBMP("lltoolpipette.BMP",2,28);
    mSDLCursors[UI_CURSOR_TOOLSIT] = makeSDLCursorFromBMP("toolsit.BMP",20,15);
    mSDLCursors[UI_CURSOR_TOOLBUY] = makeSDLCursorFromBMP("toolbuy.BMP",20,15);
    mSDLCursors[UI_CURSOR_TOOLOPEN] = makeSDLCursorFromBMP("toolopen.BMP",20,15);
    mSDLCursors[UI_CURSOR_TOOLPATHFINDING] = makeSDLCursorFromBMP("lltoolpathfinding.BMP", 16, 16);
    mSDLCursors[UI_CURSOR_TOOLPATHFINDING_PATH_START] = makeSDLCursorFromBMP("lltoolpathfindingpathstart.BMP", 16, 16);
    mSDLCursors[UI_CURSOR_TOOLPATHFINDING_PATH_START_ADD] = makeSDLCursorFromBMP("lltoolpathfindingpathstartadd.BMP", 16, 16);
    mSDLCursors[UI_CURSOR_TOOLPATHFINDING_PATH_END] = makeSDLCursorFromBMP("lltoolpathfindingpathend.BMP", 16, 16);
    mSDLCursors[UI_CURSOR_TOOLPATHFINDING_PATH_END_ADD] = makeSDLCursorFromBMP("lltoolpathfindingpathendadd.BMP", 16, 16);
    mSDLCursors[UI_CURSOR_TOOLNO] = makeSDLCursorFromBMP("llno.BMP",8,8);
}

void LLWindowSDL::quitCursors()
{
    if (mWindow)
    {
        for (int i=0; i<UI_CURSOR_COUNT; ++i)
        {
            if (mSDLCursors[i])
            {
                SDL_DestroyCursor(mSDLCursors[i]);
                mSDLCursors[i] = nullptr;
            }
        }
    }
    else
    {
        // SDL doesn't refcount cursors, so if the window has
        // already been destroyed then the cursors have gone with it.
        LL_INFOS() << "Skipping quitCursors: mWindow already gone." << LL_ENDL;
        for (int i=0; i<UI_CURSOR_COUNT; ++i)
            mSDLCursors[i] = nullptr;
    }
}

void LLWindowSDL::captureMouse()
{
    // SDL already enforces the semantics that captureMouse is
    // used for, i.e. that we continue to get mouse events as long
    // as a button is down regardless of whether we left the
    // window, and in a less obnoxious way than SDL_WM_GrabInput
    // which would confine the cursor to the window too.

    LL_DEBUGS() << "LLWindowSDL::captureMouse" << LL_ENDL;
}

void LLWindowSDL::releaseMouse()
{
    // see LWindowSDL::captureMouse()

    LL_DEBUGS() << "LLWindowSDL::releaseMouse" << LL_ENDL;
}

void LLWindowSDL::hideCursor()
{
    if(!mCursorHidden)
    {
        // LL_INFOS() << "hideCursor: hiding" << LL_ENDL;
        mCursorHidden = true;
        mHideCursorPermanent = true;
        SDL_HideCursor();
    }
    else
    {
        // LL_INFOS() << "hideCursor: already hidden" << LL_ENDL;
    }
}

void LLWindowSDL::showCursor()
{
    if(mCursorHidden)
    {
        // LL_INFOS() << "showCursor: showing" << LL_ENDL;
        mCursorHidden = false;
        mHideCursorPermanent = false;
        SDL_ShowCursor();
    }
    else
    {
        // LL_INFOS() << "showCursor: already visible" << LL_ENDL;
    }
}

void LLWindowSDL::showCursorFromMouseMove()
{
    if (!mHideCursorPermanent)
    {
        showCursor();
    }
}

void LLWindowSDL::hideCursorUntilMouseMove()
{
    if (!mHideCursorPermanent)
    {
        hideCursor();
        mHideCursorPermanent = false;
    }
}

//
// LLSplashScreenSDL - I don't think we'll bother to implement this; it's
// fairly obsolete at this point.
//
LLSplashScreenSDL::LLSplashScreenSDL()
{
}

LLSplashScreenSDL::~LLSplashScreenSDL()
{
}

void LLSplashScreenSDL::showImpl()
{
}

void LLSplashScreenSDL::updateImpl(const std::string& mesg)
{
}

void LLSplashScreenSDL::hideImpl()
{
}

S32 OSMessageBoxSDL(const std::string& text, const std::string& caption, U32 type)
{
    SDL_MessageBoxData oData = { SDL_MESSAGEBOX_INFORMATION, nullptr, caption.c_str(), text.c_str(), 0, nullptr, nullptr };
    SDL_MessageBoxButtonData btnOk[] = {{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, OSBTN_OK, "OK" }};
    SDL_MessageBoxButtonData btnOkCancel [] =  {{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, OSBTN_OK, "OK" }, {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, OSBTN_CANCEL, "Cancel"} };
    SDL_MessageBoxButtonData btnYesNo[] = { {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, OSBTN_YES, "Yes" }, {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, OSBTN_NO, "No"} };

    switch (type)
    {
        default:
        case OSMB_OK:
            oData.flags = SDL_MESSAGEBOX_WARNING;
            oData.buttons = btnOk;
            oData.numbuttons = 1;
            break;
        case OSMB_OKCANCEL:
            oData.flags = SDL_MESSAGEBOX_INFORMATION;
            oData.buttons = btnOkCancel;
            oData.numbuttons = 2;
            break;
        case OSMB_YESNO:
            oData.flags = SDL_MESSAGEBOX_INFORMATION;
            oData.buttons = btnYesNo;
            oData.numbuttons = 2;
            break;
    }

    if(gWindowImplementation != nullptr)
        gWindowImplementation->beforeDialog();

    int btn{0};
    if(SDL_ShowMessageBox( &oData, &btn ))
    {
        if(gWindowImplementation != nullptr)
            gWindowImplementation->afterDialog();
        return btn;
    }

    if(gWindowImplementation != nullptr)
        gWindowImplementation->afterDialog();

    return OSBTN_CANCEL;
}

S32 OSMessageBoxChoicesSDL(const std::string& text, const std::string& caption, const std::vector<std::string>& labels)
{
    if (labels.empty())
        return OSBTN_CANCEL;
    // SDL lays buttons out in array order; the first answers Enter, the last Escape / close.
    std::vector<SDL_MessageBoxButtonData> btns;
    for (size_t i = 0; i < labels.size(); ++i)
    {
        SDL_MessageBoxButtonData b{};
        b.flags = (i == 0 ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : 0) |
                  (i + 1 == labels.size() ? SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT : 0);
        b.buttonID = (int)i;
        b.text = labels[i].c_str();
        btns.push_back(b);
    }
    SDL_MessageBoxData oData = { SDL_MESSAGEBOX_WARNING, nullptr, caption.c_str(), text.c_str(),
                                 (int)btns.size(), btns.data(), nullptr };
    if (gWindowImplementation != nullptr)
        gWindowImplementation->beforeDialog();
    int btn = (int)labels.size() - 1;
    if (!SDL_ShowMessageBox(&oData, &btn))
        btn = (int)labels.size() - 1;
    if (gWindowImplementation != nullptr)
        gWindowImplementation->afterDialog();
    return btn;
}

bool LLWindowSDL::dialogColorPicker( F32 *r, F32 *g, F32 *b)
{
    return false;
}

/*
        Make the raw keyboard data available - used to poke through to LLQtWebKit so
        that Qt/Webkit has access to the virtual keycodes etc. that it needs
*/
LLSD LLWindowSDL::getNativeKeyData()
{
    LLSD result = LLSD::emptyMap();

    U32 modifiers = 0; // pretend-native modifiers... oh what a tangled web we weave!

    // we go through so many levels of device abstraction that I can't really guess
    // what a plugin under GDK under Qt under SL under SDL under X11 considers
    // a 'native' modifier mask.  this has been sort of reverse-engineered... they *appear*
    // to match GDK consts, but that may be co-incidence.
    modifiers |= (mKeyModifiers & SDL_KMOD_LSHIFT) ? 0x0001 : 0;
    modifiers |= (mKeyModifiers & SDL_KMOD_RSHIFT) ? 0x0001 : 0;// munge these into the same shift
    modifiers |= (mKeyModifiers & SDL_KMOD_CAPS)   ? 0x0002 : 0;
    modifiers |= (mKeyModifiers & SDL_KMOD_LCTRL)  ? 0x0004 : 0;
    modifiers |= (mKeyModifiers & SDL_KMOD_RCTRL)  ? 0x0004 : 0;// munge these into the same ctrl
    modifiers |= (mKeyModifiers & SDL_KMOD_LALT)   ? 0x0008 : 0;// untested
    modifiers |= (mKeyModifiers & SDL_KMOD_RALT)   ? 0x0008 : 0;// untested
    // *todo: test ALTs - I don't have a case for testing these.  Do you?
    // *todo: NUM? - I don't care enough right now (and it's not a GDK modifier).

    result["virtual_key"] = (S32)mKeyVirtualKey;
    result["virtual_key_win"] = (S32)LLKeyboardSDL::mapSDL2toWin( mKeyVirtualKey );
    result["modifiers"] = (S32)modifiers;
    result["is_char"] = mInKeyCharPass;
    return result;
}

// Open a URL with the user's default web browser.
// Must begin with protocol identifier.
void LLWindowSDL::spawnWebBrowser(const std::string& escaped_url, bool async)
{
    // The scheme is everything before the first ':'. It must be a valid
    // scheme name (RFC 3986: ALPHA *( ALPHA / DIGIT / "+" / "-" / "." )) and
    // match a whitelisted scheme exactly, ignoring case.
    std::string scheme;
    const size_t colon = escaped_url.find(':');
    bool valid = (colon != std::string::npos) && (colon > 0);
    for (size_t pos = 0; valid && pos < colon; ++pos)
    {
        char c = escaped_url[pos];
        if (c >= 'A' && c <= 'Z')
        {
            c = static_cast<char>(c - 'A' + 'a');
        }
        const bool is_alpha = (c >= 'a' && c <= 'z');
        const bool is_other = (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
        valid = is_alpha || (pos > 0 && is_other);
        scheme += c;
    }

    // No control characters anywhere in what we pass to the OS
    for (size_t pos = 0; valid && pos < escaped_url.size(); ++pos)
    {
        const unsigned char c = static_cast<unsigned char>(escaped_url[pos]);
        valid = (c >= 0x20 && c != 0x7f);
    }

    bool found = false;
    for (S32 i = 0; valid && i < gURLProtocolWhitelistCount; i++)
    {
        if (scheme == gURLProtocolWhitelist[i])
        {
            found = true;
            break;
        }
    }

    if (!found)
    {
        LL_WARNS() << "spawn_web_browser called for url with protocol not on whitelist: " << escaped_url << LL_ENDL;
        return;
    }

    LL_INFOS() << "spawn_web_browser: " << escaped_url << LL_ENDL;

    if (!SDL_OpenURL(escaped_url.c_str()))
    {
        LL_WARNS() << "spawn_web_browser failed with error: " << SDL_GetError() << LL_ENDL;
    }

    LL_INFOS() << "spawn_web_browser returning." << LL_ENDL;
}

void* LLWindowSDL::getPlatformWindow()
{
    void* ret = nullptr;
    if (mWindow)
    {
#if LL_WINDOWS
        ret = SDL_GetPointerProperty(SDL_GetWindowProperties(mWindow), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#endif
    }
    return ret;
}

void LLWindowSDL::bringToFront()
{
    // This is currently used when we are 'launched' to a specific
    // map position externally.
    LL_INFOS() << "bringToFront" << LL_ENDL;
    if (mWindow && !mFullscreen)
    {
        SDL_RaiseWindow(mWindow);
    }
}

//static
bool LLWindowSDL::findFallbackFontForChar(U32 wch, std::string& path, S32& face_index)
{
#if LL_LINUX
    // The eager list (getDynamicFallbackFontList) is capped at the first 40 fonts of
    // the sort, which can drop the only fonts covering a script (here Noto Sans CJK
    // ranked ~181 -> enclosed alphanumerics, Noto Sans Kannada ~65). Search the FULL
    // sorted set -- same pattern, so priority order is unchanged -- for the first font
    // whose charset has the character. Results (incl. misses) are cached per char.
    struct Entry { std::string file; S32 index; FcCharSet* cs; };
    static std::vector<Entry> s_fonts;
    static bool s_built = false;
    static std::unordered_map<U32, S32> s_cache;   // char -> index in s_fonts, -1 = none
    if (!s_built)
    {
        s_built = true;
        std::string sort_order("slant=0:index=0:weight=80:spacing=0");
        FL_Locale* locale = nullptr;
        if (FL_FindLocale(&locale, FL_MESSAGES) >= 2 && locale && locale->lang)
            sort_order = "lang=" + std::string(locale->lang) + ":" + sort_order;
        FL_FreeLocale(&locale);
        if (FcInit())
        {
            if (FcPattern* pat = FcNameParse((FcChar8*)sort_order.c_str()))
            {
                FcResult result;
                if (FcFontSet* fs = FcFontSort(nullptr, pat, FcTrue, nullptr, &result))
                {
                    for (int i = 0; i < fs->nfont; ++i)
                    {
                        FcChar8* file = nullptr;
                        FcCharSet* cs = nullptr;
                        int idx = 0;
                        if (FcPatternGetString(fs->fonts[i], FC_FILE, 0, &file) != FcResultMatch || !file) continue;
                        if (FcPatternGetCharSet(fs->fonts[i], FC_CHARSET, 0, &cs) != FcResultMatch || !cs) continue;
                        FcPatternGetInteger(fs->fonts[i], FC_INDEX, 0, &idx);
                        s_fonts.push_back({ std::string((const char*)file), (S32)idx, FcCharSetCopy(cs) });
                    }
                    FcFontSetDestroy(fs);
                }
                FcPatternDestroy(pat);
            }
        }
        LL_INFOS("Font") << "on-demand font fallback: " << s_fonts.size() << " system fonts indexed" << LL_ENDL;
    }
    auto it = s_cache.find(wch);
    S32 hit = -1;
    if (it != s_cache.end())
    {
        hit = it->second;
    }
    else
    {
        for (size_t i = 0; i < s_fonts.size(); ++i)
        {
            if (FcCharSetHasChar(s_fonts[i].cs, (FcChar32)wch)) { hit = (S32)i; break; }
        }
        s_cache.emplace(wch, hit);
    }
    if (hit < 0) return false;
    path = s_fonts[hit].file;
    face_index = s_fonts[hit].index;
    return true;
#else
    return false;
#endif
}

//static
std::vector<std::string> LLWindowSDL::getDynamicFallbackFontList()
{
    std::vector<std::string> rtns;
#if LL_LINUX
    // Use libfontconfig to find us a nice ordered list of fallback fonts
    // specific to this system.
    std::string final_fallback("/usr/share/fonts/truetype/kochi/kochi-gothic.ttf");
    const int max_font_count_cutoff = 40; // fonts are expensive in the current system, don't enumerate an arbitrary number of them
    // Our 'ideal' font properties which define the sorting results.
    // slant=0 means Roman, index=0 means the first face in a font file
    // (the one we actually use), weight=80 means medium weight,
    // spacing=0 means proportional spacing.
    std::string sort_order("slant=0:index=0:weight=80:spacing=0");
    // elide_unicode_coverage removes fonts from the list whose unicode
    // range is covered by fonts earlier in the list.  This usually
    // removes ~90% of the fonts as redundant (which is great because
    // the font list can be huge), but might unnecessarily reduce the
    // renderable range if for some reason our FreeType actually fails
    // to use some of the fonts we want it to.
    const bool elide_unicode_coverage = true;

    FcFontSet *fs = nullptr;
    FcPattern *sortpat = nullptr;

    LL_INFOS() << "Getting system font list from FontConfig..." << LL_ENDL;

    // If the user has a system-wide language preference, then favor
    // fonts from that language group.  This doesn't affect the types
    // of languages that can be displayed, but ensures that their
    // preferred language is rendered from a single consistent font where
    // possible.
    FL_Locale *locale = nullptr;
    FL_Success success = FL_FindLocale(&locale, FL_MESSAGES);
    if (success != 0)
    {
        if (success >= 2 && locale->lang) // confident!
        {
            LL_INFOS("AppInit") << "Language " << locale->lang << LL_ENDL;
            LL_INFOS("AppInit") << "Location " << locale->country << LL_ENDL;
            LL_INFOS("AppInit") << "Variant " << locale->variant << LL_ENDL;

            LL_INFOS() << "Preferring fonts of language: "
                       << locale->lang
                       << LL_ENDL;
            sort_order = "lang=" + std::string(locale->lang) + ":"
                         + sort_order;
        }
    }
    FL_FreeLocale(&locale);

    if (!FcInit())
    {
        LL_WARNS() << "FontConfig failed to initialize." << LL_ENDL;
        rtns.push_back(final_fallback);
        return rtns;
    }

    sortpat = FcNameParse((FcChar8*) sort_order.c_str());
    if (sortpat)
    {
        // Sort the list of system fonts from most-to-least-desirable.
        FcResult result;
        fs = FcFontSort(nullptr, sortpat, elide_unicode_coverage, nullptr, &result);
        FcPatternDestroy(sortpat);
    }

    int found_font_count = 0;
    if (fs)
    {
        // Get the full pathnames to the fonts, where available,
        // which is what we really want.
        found_font_count = fs->nfont;
        for (int i=0; i<fs->nfont; ++i)
        {
            FcChar8 *filename;
            if (FcResultMatch == FcPatternGetString(fs->fonts[i], FC_FILE, 0, &filename) && filename)
            {
                rtns.push_back(std::string((const char*)filename));
                if (rtns.size() >= max_font_count_cutoff)
                    break; // hit limit
            }
        }
        FcFontSetDestroy (fs);
    }

    LL_DEBUGS() << "Using font list: " << LL_ENDL;
    for (auto it = rtns.begin(); it != rtns.end(); ++it)
    {
        LL_DEBUGS() << "  file: " << *it << LL_ENDL;
    }

    LL_INFOS() << "Using " << rtns.size() << "/" << found_font_count << " system fonts." << LL_ENDL;

    rtns.push_back(final_fallback);
#endif
    return rtns;
}

void LLWindowSDL::allowLanguageTextInput(LLPreeditor *preeditor, bool b)
{
    if (preeditor != mPreeditor && !b)
    {
        // This condition may occur by a call to
        // setEnabled(bool) against LLTextEditor or LLLineEditor
        // when the control is not focused.
        // We need to silently ignore the case so that
        // the language input status of the focused control
        // is not disturbed.
        return;
    }

    // Take care of old and new preeditors.
    if (preeditor != mPreeditor || !b)
    {
        // We need to interrupt before updating mPreeditor,
        // so that the fix string from input method goes to
        // the old preeditor.
        if (mLanguageTextInputAllowed)
        {
            interruptLanguageTextInput();
        }
        mPreeditor = (b ? preeditor : nullptr);
    }

    if ((bool)b == mLanguageTextInputAllowed)
    {
        return;
    }

    mLanguageTextInputAllowed = b;

    if(!mWindow) return;

    if (mLanguageTextInputAllowed)
    {
        SDL_StartTextInput(mWindow);
    }
    else
    {
        SDL_StopTextInput(mWindow);
    }
}

void LLWindowSDL::setLanguageTextInput(const LLCoordGL& position)
{
    if (mLanguageTextInputAllowed && mPreeditor)
    {
        LLCoordGL caret_coord;
        LLRect preedit_bounds;
        if (mPreeditor->getPreeditLocation(-1, &caret_coord, &preedit_bounds, nullptr))
        {
            LLCoordWindow window_pos;
            convertCoords(position, &window_pos);

            const F32 density = pixelDensity();
            SDL_Rect coords;
            coords.x = ll_round((F32)window_pos.mX / density);
            coords.y = ll_round((F32)window_pos.mY / density);
            coords.w = ll_round((F32)preedit_bounds.getWidth() / density) - coords.x;
            coords.h = ll_round((F32)preedit_bounds.getHeight() / density) - coords.y;
            SDL_SetTextInputArea(mWindow, &coords, 0);
        }
    }
}

F32 LLWindowSDL::pixelDensity() const
{
    const F32 d = mWindow ? SDL_GetWindowPixelDensity(mWindow) : 1.f;
    return d > 0.f ? d : 1.f;
}

S32 LLWindowSDL::toPixels(F32 logical) const
{
    return (S32)(logical * pixelDensity());
}

F32 LLWindowSDL::getSystemUISize()
{
    if(mWindow)
    {
        F32 scale = SDL_GetWindowDisplayScale(mWindow);
        if (scale > 0.0f)
        {
            return scale;
        }
    }
    return 1.f;
}


