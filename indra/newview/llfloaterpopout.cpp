/** Native OS windows for floater view trees; all operations run on the UI thread. */
#include "llviewerprecompiledheaders.h"
#include "llfloaterpopout.h"
#include "llnativewindowgeometry.h"

#include "llviewerwindow.h"
#include "llviewercontrol.h"
#include "llrootview.h"
#include "llpopupview.h"
#include "llparcelselection.h"
#include "llselectmgr.h"
#include "llviewermenu.h"
#include "llwindowsdl.h"
#include "llkeyboard.h"
#include "llkeyboardsdl.h"
#include "llpreeditor.h"
#include "llfontgl.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llfocusmgr.h"
#include "llmultifloater.h"
#include "llmediactrl.h"
#include "lltooldraganddrop.h"
#include "lltooltip.h"
#include "llscrollbar.h"
#include "llscrollcontainer.h"
#include "llscrolllistctrl.h"
#include "lltrans.h"
#include "slkeycommands.h"
#include "vkgfx/llvulkancontext.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <iterator>

bool LLFloaterPopout::sHasWork = false;
bool LLFloaterPopout::sCloseAll = false;
std::vector<LLFloaterPopout::Request> LLFloaterPopout::sRequests;
std::vector<std::unique_ptr<LLFloaterPopout::Window>> LLFloaterPopout::sWindows;
LLHandle<LLView> LLFloaterPopout::sMainFocus;

class LLNativeFloaterView final : public LLFloaterView
{
public:
    explicit LLNativeFloaterView(const Params& params) : LLFloaterView(params) {}
protected:
    // Retained floater recordings currently key the main projection/font scale.
    // Native roots draw live at their own bounded rate so recordings cannot leak
    // a different window's viewport or DPI transform into this target.
    bool drawChildRetained(LLView*) override { return false; }
};

class LLNativeMenuHolder final : public LLViewerMenuHolderGL
{
public:
    explicit LLNativeMenuHolder(const Params& params) : LLViewerMenuHolderGL(params) {}
    const LLRect getMenuRect() const override { return getLocalRect(); }
};

class LLNativePopupView final : public LLPopupView
{
public:
    explicit LLNativePopupView(const Params& params) : LLPopupView(params, false) {}
};

struct LLFloaterPopout::Window final : LLNativeSurface
{
    std::unique_ptr<LLNativeWindow> host;
    SDL_Window* native = nullptr; // borrowed from host
    void nativeEvent(LLNativeWindowID id, const SDL_Event& e) override { LLFloaterPopout::event(id, e); }
    NativeSurfaceId renderHandle = 0;
    LLHandle<LLFloater> floater;
    LLHandle<LLView> parent;
    LLHandle<LLView> focus;
    LLRect dockedRect;
    U32 follows = 0;
    bool canDrag = true;
    bool canResize = false;
    bool canMinimize = false;
    bool closing = false;
    bool savePending = false;
    bool monitorCheck = false;
    bool limitsSet = false;
    std::string title;
    // Floating (unmaximized) bounds in SDL logical coordinates.
    SDL_Rect bounds{};
    LLCoordGL mouse;
    LLCoordGL lastMouse, mouseDelta;
    std::set<LLHandle<LLView>> hovered;
    bool hoverEnabled = false; // rearmed by pointer motion/entry, never stale focus coordinates
    bool leftDown = false, middleDown = false, rightDown = false;
    F32 wheelX = 0.f, wheelY = 0.f;
    F64 nextDraw = 0.;
    LLVector2 scale;
    std::unique_ptr<LLRootView> root;
    LLFloaterView* floaters = nullptr; // owned by root
    LLViewerMenuHolderGL* menus = nullptr; // owned by root
    LLToolTipView* tooltips = nullptr; // owned by root
    LLPopupView* popups = nullptr; // owned by root; popup entries are weak handles
    LLUI::add_popup_t addPopup;
    LLUI::remove_popup_t removePopup;
    LLUI::clear_popups_t clearPopups;

    ~Window()
    {
        // Also covers a failed detach before the view tree was transferred.
        if (native)
        {
            LLVulkanContext::instance().destroyUIWindow(renderHandle, std::move(host));
        }
    }
};

// Legacy UI code addresses LLUI, gFloaterView, gMenuHolder, LLWindow and
// gViewerWindow globally. Scope their UI-facing state to the event/render target.
// World/camera state is never switched and these scopes never nest or escape.
struct LLFloaterPopout::Context
{
    LLViewerWindow& viewer = *gViewerWindow;
    LLWindowSDL& platform = *static_cast<LLWindowSDL*>(viewer.getWindow());
    Window& w;
    LLCoordWindow cursor;
    LLWindowSDL::ScopedUIWindow nativeScope;
    LLRootView* root = viewer.mRootView;
    LLView* uiRoot = LLUI::instance().getRootView();
    LLFloaterView* floaters = gFloaterView;
    LLViewerMenuHolderGL* menus = gMenuHolder;
    LLMenuHolderGL* container = LLMenuGL::sMenuContainer;
    LLToolTipView* tooltips = gToolTipView;
    LLPopupView* popups = viewer.mPopupView;
    LLRect raw = viewer.mWindowRectRaw, scaled = viewer.mWindowRectScaled;
    LLRect dirtyRect = LLView::sDirtyRect;
    LLVector2 displayScale = viewer.mDisplayScale;
    LLVector2 uiScale = LLUI::getScaleFactor();
    LLCoordGL mouse = viewer.mCurrentMousePoint, last = viewer.mLastMousePoint;
    LLCoordGL delta = viewer.mCurrentMouseDelta;
    bool mouseIn = viewer.mMouseInWindow;
    bool leftDown = viewer.mLeftMouseDown, middleDown = viewer.mMiddleMouseDown, rightDown = viewer.mRightMouseDown;
    LLCoordGL fontOrigin = LLFontGL::sCurOrigin;
    F32 fontDepth = LLFontGL::sCurDepth;
    const std::pair<size_t, size_t> uiDepths = gGL.uiStackDepths();
    size_t fontStackDepth = LLFontGL::sOriginStack.size();
    bool active = true;
    S32 viewport[4];

    explicit Context(Window& window) : w(window), nativeScope(platform, w.native, &cursor)
    {
        std::copy(gGLViewport, gGLViewport + 4, viewport);
        int width = 0, height = 0;
        SDL_GetWindowSizeInPixels(w.native, &width, &height);
        viewer.mRootView = w.root.get();
        viewer.mPopupView = w.popups;
        gToolTipView = w.tooltips;
        LLUI::instance().mAddPopupFunc.swap(w.addPopup);
        LLUI::instance().mRemovePopupFunc.swap(w.removePopup);
        LLUI::instance().mClearPopupsFunc.swap(w.clearPopups);
        viewer.mWindowRectRaw = LLRect(0, height, width, 0);
        viewer.mWindowRectScaled = w.root->getRect();
        viewer.mDisplayScale = w.scale;
        viewer.mCurrentMousePoint = w.mouse;
        viewer.mLastMousePoint = w.lastMouse;
        viewer.mCurrentMouseDelta = w.mouseDelta;
        viewer.mLeftMouseDown = w.leftDown;
        viewer.mMiddleMouseDown = w.middleDown;
        viewer.mRightMouseDown = w.rightDown;
        viewer.mMouseInWindow = SDL_GetMouseFocus() == w.native;
        cursor = viewer.mMouseInWindow || gFocusMgr.childHasMouseCapture(w.root.get())
            ? LLCoordWindow(ll_round(w.mouse.mX * w.scale.mV[VX]), height - 1 - ll_round(w.mouse.mY * w.scale.mV[VY]))
            : LLCoordWindow(-10000, -10000);
        LLUI::instance().setRootView(w.root.get());
        // Do not globally invalidate every vector image on every window switch.
        LLUI::getScaleFactor() = w.scale;
        gFloaterView = w.floaters;
        gMenuHolder = w.menus;
        LLMenuGL::sMenuContainer = w.menus;
        gGLViewport[0] = gGLViewport[1] = 0;
        gGLViewport[2] = width;
        gGLViewport[3] = height;
        LLFontGL::sCurOrigin = LLCoordGL(0, 0);
        LLFontGL::sCurDepth = 0.f;
        LLView::sDirtyRect = w.root->getLocalRect();
    }

    void restore()
    {
        if (!active) return;
        active = false;
        nativeScope.restore();
        viewer.mRootView = root;
        viewer.mPopupView = popups;
        gToolTipView = tooltips;
        LLUI::instance().mAddPopupFunc.swap(w.addPopup);
        LLUI::instance().mRemovePopupFunc.swap(w.removePopup);
        LLUI::instance().mClearPopupsFunc.swap(w.clearPopups);
        viewer.mWindowRectRaw = raw;
        viewer.mWindowRectScaled = scaled;
        viewer.mDisplayScale = displayScale;
        viewer.mCurrentMousePoint = mouse;
        viewer.mLastMousePoint = last;
        viewer.mCurrentMouseDelta = delta;
        viewer.mMouseInWindow = mouseIn;
        viewer.mLeftMouseDown = leftDown;
        viewer.mMiddleMouseDown = middleDown;
        viewer.mRightMouseDown = rightDown;
        LLUI::instance().setRootView(uiRoot);
        LLUI::getScaleFactor() = uiScale;
        gFloaterView = floaters;
        gMenuHolder = menus;
        LLMenuGL::sMenuContainer = container;
        std::copy(viewport, viewport + 4, gGLViewport);
        LLFontGL::sCurOrigin = fontOrigin;
        LLFontGL::sCurDepth = fontDepth;
        // A failed UI allocation can unwind out of legacy push/pop drawing code.
        // Drop any unfinished native stack levels before returning to the viewer.
        gGL.unwindUIStacks(uiDepths);
        if (LLFontGL::sOriginStack.size() > fontStackDepth) LLFontGL::sOriginStack.resize(fontStackDepth);
        LLView::sDirtyRect = dirtyRect;
    }
    ~Context() { restore(); }
};

static bool onWayland()
{
    return !LLNativeWindow::absolutePlacement();
}

void LLFloaterPopout::showWorldView()
{
    // Legacy window services point at the auxiliary window during callbacks.
    // Use the stable lifecycle identity without changing the active UI context.
    auto* platform = static_cast<LLWindowSDL*>(gViewerWindow->getWindow());
    SDL_Window* main = platform->getMainSDLWindow();
    if (!main) return;
    if (SDL_GetWindowFlags(main) & SDL_WINDOW_MINIMIZED) SDL_RestoreWindow(main);
    SDL_RaiseWindow(main);
}

bool LLFloaterPopout::isWorldViewMinimized()
{
    auto* platform = static_cast<LLWindowSDL*>(gViewerWindow->getWindow());
    SDL_Window* main = platform->getMainSDLWindow();
    return main && (SDL_GetWindowFlags(main) & SDL_WINDOW_MINIMIZED);
}

static std::string placementKey(LLFloater* floater)
{
    std::string key = LLFloater::getControlName(floater->getInstanceName(), floater->getKey());
    // Script/notecard/IM instances use UUID keys; getControlName only adds string
    // keys, which would otherwise make every editor share one placement.
    if (floater->getKey().isUUID()) key += "_" + floater->getKey().asUUID().asString();
    return key;
}

LLSD LLFloaterPopout::placement(LLFloater* floater)
{
    return gSavedSettings.getLLSD("NativeFloaterWindows")[placementKey(floater)];
}

void LLFloaterPopout::init()
{
    LLFloater::sNativeWindowRequest = [](LLFloater* floater, bool pop)
    {
        if (!floater->canPopOut()) return;
        for (auto& request : sRequests)
            if (request.floater == floater->getHandle()) { request.pop = pop; request.restore = false; return; }
        if (sRequests.size() >= 32) return;
        try { sRequests.push_back({floater->getHandle(), pop, false}); }
        catch (const std::bad_alloc&) { LL_WARNS("Popout") << "No memory for native window request" << LL_ENDL; return; }
        sHasWork = true;
    };
    LLFloater::sNativeWindowOpened = [](LLFloater* floater)
    {
        if (floater->isNativePopout())
        {
            for (auto& window : sWindows)
                if (window->floater.get() == floater) { window->host->raise(); return; }
        }
        if (sRequests.size() >= 32) return;
        for (const auto& request : sRequests) if (request.floater == floater->getHandle()) return;
        if (!floater->isNativePopout() && placement(floater)["popped"].asBoolean())
        {
            try { sRequests.push_back({floater->getHandle(), true, true}); }
            catch (const std::bad_alloc&) { return; }
            sHasWork = true;
        }
    };
    LLNativeWindow::mainEvent = [](const SDL_Event& e) { return event(0, e); };
}

bool LLFloaterPopout::ownsMouse()
{
    SDL_Window* target = SDL_GetMouseFocus();
    for (const auto& w : sWindows)
        if (w->native == target || gFocusMgr.childHasMouseCapture(w->root.get())) return true;
    return false;
}

bool LLFloaterPopout::isVisibleNativeView(const LLView* view)
{
    if (!view || !view->isInVisibleChain()) return false;
    for (const auto& w : sWindows)
        if (!w->closing && view->hasAncestor(w->root.get()) &&
            !(SDL_GetWindowFlags(w->native) & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED))) return true;
    return false;
}

void LLFloaterPopout::save(Window& w, bool popped)
try
{
    auto* floater = w.floater.get();
    if (!floater) return;
    LLSD saved = gSavedSettings.getLLSD("NativeFloaterWindows");
    LLSD& value = saved[placementKey(floater)];
    value["popped"] = popped;
    w.bounds = w.host->floatingBounds();
    value["width"] = w.bounds.w;
    value["height"] = w.bounds.h;
    value["maximized"] = bool(SDL_GetWindowFlags(w.native) & SDL_WINDOW_MAXIMIZED);
    SDL_DisplayID display = SDL_GetDisplayForWindow(w.native);
    const char* name = SDL_GetDisplayName(display);
    value["monitor"] = name ? name : "";
    if (!onWayland())
    {
        // Coordinates relative to the display survive monitor rearrangements.
        SDL_Rect area{};
        SDL_GetDisplayUsableBounds(display, &area);
        value["x"] = w.bounds.x - area.x;
        value["y"] = w.bounds.y - area.y;
    }
    // Persist at most 128 distinct floater identities, including multi-instance
    // editor windows. Never store titles, contents, or inventory asset names.
    if (saved.size() > 128)
    {
        for (auto it = saved.beginMap(); it != saved.endMap(); ++it)
        {
            if (it->first != placementKey(floater))
            {
                saved.erase(it->first);
                break;
            }
        }
    }
    gSavedSettings.setLLSD("NativeFloaterWindows", saved);
    w.savePending = false;
}

catch (const std::bad_alloc&)
{
    w.savePending = true;
    LL_WARNS("NativeUI") << "Native window placement deferred under memory pressure" << LL_ENDL;
}

bool LLFloaterPopout::detach(LLFloater* floater, bool restore)
{
    if (!floater || floater->isDead() || !floater->getVisible() || !floater->canPopOut() ||
        floater->isNativePopout() || floater->getHost() || floater->getDependee() ||
        !floater->getParent() || sWindows.size() >= 16 || !LLVulkanContext::instance().isInitialized()) return false;
    LLSD saved = placement(floater);
    SDL_DisplayID display = 0;
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    for (int i = 0; displays && i < count; ++i)
    {
        const char* name = SDL_GetDisplayName(displays[i]);
        if (name && saved["monitor"].asString() == name) { display = displays[i]; break; }
    }
    SDL_free(displays);
    // Restoring a missing monitor means keeping this floater inside the viewer.
    if (restore && !display) return false;
    auto* mainWindow = static_cast<LLWindowSDL*>(gViewerWindow->getWindow());
    if (!display) display = SDL_GetDisplayForWindow(mainWindow->getMainSDLWindow());
    SDL_Rect area{};
    if (!SDL_GetDisplayUsableBounds(display, &area)) return false;
    if (area.w <= 0 || area.h <= 0) return false;
    const F32 density = onWayland() ? 1.f : llmax(0.25f, SDL_GetDisplayContentScale(display));
    const F32 uiPreference = llmax(0.5f, gSavedSettings.getF32("UIScaleFactor"));
    int width = ll_round(floater->getRect().getWidth() * uiPreference * density);
    int height = ll_round(floater->getRect().getHeight() * uiPreference * density);
    if (saved.has("width")) width = saved["width"].asInteger();
    if (saved.has("height")) height = saved["height"].asInteger();
    const auto bounds = native_window::restoreRect({area.x, area.y, area.w, area.h},
        {saved["x"].asInteger(), saved["y"].asInteger(), width, height},
        llmax(200, ll_round(floater->getMinWidth() * uiPreference * density)),
        llmax(100, ll_round(floater->getMinHeight() * uiPreference * density)));
    width = bounds.width;
    height = bounds.height;
    sWindows.reserve(16);
    auto pending = std::unique_ptr<Window>(new (std::nothrow) Window);
    Window* w = pending.get();
    if (!w) return false;
    w->title = floater->getTitle();
    w->host = LLNativeWindow::create({w->title, {bounds.x, bounds.y, width, height}, false});
    if (!w->host) return false;
    w->native = w->host->handle();
    w->host->attach(w);
    w->bounds = w->host->floatingBounds();
    w->renderHandle = LLVulkanContext::instance().createUIWindow(w->native, w->host->id());
    if (!w->renderHandle)
    {
        return false;
    }
    w->floater = floater->getHandle();
    w->parent = floater->getParent()->getHandle();
    w->dockedRect = floater->getRect();
    w->follows = floater->getFollows();
    w->canDrag = floater->getCanDrag();
    w->canResize = floater->isResizable();
    w->canMinimize = floater->isMinimizeable();
    const F32 actualScale = llmax(0.25f, SDL_GetWindowDisplayScale(w->native)) * uiPreference;
    w->scale = LLVector2(actualScale, actualScale);
    int pixelWidth = 0, pixelHeight = 0;
    SDL_GetWindowSizeInPixels(w->native, &pixelWidth, &pixelHeight);
    LLView::Params rp;
    rp.name = "native_floater_root";
    rp.rect = LLRect(0, ll_round(pixelHeight / actualScale), ll_round(pixelWidth / actualScale), 0);
    rp.mouse_opaque = true;
    w->root.reset(LLUICtrlFactory::create<LLRootView>(rp));
    LLFloaterView::Params fp;
    fp.name = "native_floater_view";
    fp.rect = rp.rect;
    fp.follows.flags(FOLLOWS_ALL);
    w->floaters = LLUICtrlFactory::create<LLNativeFloaterView>(fp);
    w->root->addChild(w->floaters);
    LLPopupView::Params pp;
    pp.name = "native_floater_popups";
    pp.rect = rp.rect;
    pp.follows.flags(FOLLOWS_ALL);
    pp.mouse_opaque = false;
    w->popups = LLUICtrlFactory::create<LLNativePopupView>(pp);
    w->root->addChild(w->popups);
    w->addPopup = boost::bind(&LLPopupView::addPopup, w->popups, _1);
    w->removePopup = boost::bind(&LLPopupView::removePopup, w->popups, _1);
    w->clearPopups = boost::bind(&LLPopupView::clearPopups, w->popups);
    LLViewerMenuHolderGL::Params mp;
    mp.name = "native_floater_menus";
    mp.rect = rp.rect;
    mp.follows.flags(FOLLOWS_ALL);
    mp.mouse_opaque = false;
    w->menus = LLUICtrlFactory::create<LLNativeMenuHolder>(mp);
    w->root->addChild(w->menus);
    LLToolTipView::Params tp;
    tp.name = "native_tooltips";
    tp.rect = rp.rect;
    tp.follows.flags(FOLLOWS_ALL);
    tp.mouse_opaque = false;
    w->tooltips = LLUICtrlFactory::create<LLToolTipView>(tp);
    w->root->addChild(w->tooltips);
    // Commit ownership before callbacks/reparenting can allocate. On exception,
    // update() docks this entry instead of deleting the user's editor view tree.
    w->closing = true;
    sWindows.push_back(std::move(pending));
    if (auto* focus = dynamic_cast<LLView*>(gFocusMgr.getKeyboardFocus()))
    {
        if (gFocusMgr.childHasKeyboardFocus(floater)) w->focus = focus->getHandle();
        else sMainFocus = focus->getHandle();
    }
    floater->storeRectControl();
    clearHoverStates();
    floater->releaseFocus();
    floater->setMinimized(false);
    floater->setNativePopout(true);
    floater->setCanDrag(false);
    floater->setCanResize(false);
    floater->setCanMinimize(false);
    floater->setFollows(FOLLOWS_ALL);
    w->floaters->addChild(floater);
    {
        // Browser reshape sizes its media texture using the current UI scale.
        // The initial reshape must use the native host, just like later resizes.
        Context context(*w);
        floater->setOrigin(0, 0);
        floater->reshape(w->root->getRect().getWidth(), w->root->getRect().getHeight());
    }
    syncGeometry(*w);
    if (saved["maximized"].asBoolean()) SDL_MaximizeWindow(w->native);
    w->host->raise();
    save(*w, true);
    w->closing = false;
    return true;
}

bool LLFloaterPopout::dock(Window& w, bool rememberPopout, bool savePlacement)
try
{
    {
        Context context(w);
        w.hoverEnabled = false;
        hoverViews(w, false, gKeyboard->currentMask(true));
        LLToolTipMgr::instance().blockToolTips();
    }
    if (savePlacement) save(w, rememberPopout);
    w.popups->clearPopups();
    if (w.tooltips && gToolTipView)
    {
        const auto tips = *w.tooltips->getChildList();
        for (LLView* child : tips) { child->setVisible(false); gToolTipView->addChild(child); }
    }
    if (auto* floater = w.floater.get())
    {
        floater->releaseFocus();
        if (floater->getParent()) floater->getParent()->removeChild(floater);
        LLView* parent = w.parent.get();
        if (!parent) parent = gFloaterView;
        parent->addChild(floater);
        floater->setNativePopout(false);
        floater->setCanDrag(w.canDrag);
        floater->setCanResize(w.canResize);
        floater->setCanMinimize(w.canMinimize);
        floater->setFollows(w.follows);
        // Preserve the native dimensions until reshape propagates their delta
        // to the browser/layout children; origin can be restored independently.
        floater->setOrigin(w.dockedRect.mLeft, w.dockedRect.mBottom);
        floater->reshape(w.dockedRect.getWidth(), w.dockedRect.getHeight());
        gFloaterView->adjustToFitScreen(floater, false);
    }
    auto* platform = static_cast<LLWindowSDL*>(gViewerWindow->getWindow());
    if (platform->textInputWindow() == w.native) platform->setTextInputWindow(nullptr);
    // Any dependent UI created during a native context goes back with the floater.
    auto children = *w.floaters->getChildList();
    for (LLView* child : children)
    {
        w.floaters->removeChild(child);
        gFloaterView->addChild(child);
        if (auto* floater = dynamic_cast<LLFloater*>(child)) gFloaterView->adjustToFitScreen(floater, false);
    }
    // Some menus are cached globally or by controls with raw pointers. Return
    // them (and their branch menus) to the main holder before deleting this root.
    w.menus->hideMenus();
    const auto menus = *w.menus->getChildList();
    for (LLView* child : menus)
    {
        if (child->getParent() != w.menus) continue;
        if (auto* menu = dynamic_cast<LLMenuGL*>(child)) menu->updateParent(gMenuHolder);
        else gMenuHolder->addChild(child);
    }
    LLVulkanContext::instance().destroyUIWindow(w.renderHandle, std::move(w.host));
    w.native = nullptr;
    if (!rememberPopout)
    {
        if (auto* floater = w.floater.get())
        {
            if (floater->getVisible() && !floater->isDead()) floater->setFrontmost(true);
        }
    }
    return true;
}

catch (const std::bad_alloc&)
{
    // Keep the root and live floater handle for another UI boundary. In
    // particular, never delete an editor because reparenting was refused.
    w.closing = true;
    LL_WARNS("NativeUI") << "Native window return deferred under memory pressure" << LL_ENDL;
    return false;
}

void LLFloaterPopout::update()
{
    // Reparent/destroy only outside event propagation, root traversal and draw.
    auto requests = std::move(sRequests);
    sRequests.clear();
    for (const auto& request : requests)
    {
        auto* floater = request.floater.get();
        if (!floater || floater->isDead()) continue;
        if (request.pop)
        {
            try { detach(floater, request.restore); }
            catch (const std::bad_alloc&)
            {
                LL_WARNS("Popout") << "Not enough memory to detach floater; retaining its viewer instance" << LL_ENDL;
            }
        }
        else
            for (auto& w : sWindows) if (w->floater == request.floater) w->closing = true;
    }
    if (sCloseAll)
    {
        sCloseAll = false;
        for (auto& window : sWindows)
        {
            Context context(*window);
            window->floaters->closeAllChildren(false);
        }
    }
    for (auto it = sWindows.begin(); it != sWindows.end();)
    {
        Window& w = **it;
        if (LLVulkanContext::instance().uiWindowFailed(w.renderHandle)) w.closing = true;
        auto* floater = w.floater.get();
        bool missingMonitor = false;
        if (w.monitorCheck)
        {
            w.monitorCheck = false;
            const LLSD saved = floater ? placement(floater) : LLSD();
            int count = 0;
            SDL_DisplayID* displays = SDL_GetDisplays(&count);
            missingMonitor = true;
            for (int i = 0; displays && i < count; ++i)
            {
                const char* name = SDL_GetDisplayName(displays[i]);
                if (name && saved["monitor"].asString() == name) missingMonitor = false;
            }
            SDL_free(displays);
        }
        if (w.closing || !floater || floater->isDead() || !floater->getVisible() || missingMonitor)
        {
            if (!dock(w, !w.closing, !missingMonitor)) { ++it; continue; }
            it = sWindows.erase(it);
            continue;
        }
        if (w.savePending) save(w, true);
        ++it;
    }
    sHasWork = !sWindows.empty() || !sRequests.empty();
}

void LLFloaterPopout::shutdown()
{
    LLNativeWindow::mainEvent = {};
    LLFloater::sNativeWindowRequest = {};
    LLFloater::sNativeWindowOpened = {};
    for (auto& w : sWindows) dock(*w, true);
    sWindows.clear();
    sRequests.clear();
    sHasWork = false;
    sCloseAll = false;
}

void LLFloaterPopout::closeAll()
{
    if (!sWindows.empty()) { sCloseAll = true; sHasWork = true; }
}

bool LLFloaterPopout::prepareForQuit()
{
    if (sWindows.empty()) return false;
    // Include every dirty editor in main's save-confirmation traversal. If a
    // reparent allocation is refused, idleShutdown retries before it can exit.
    for (auto it = sWindows.begin(); it != sWindows.end();)
    {
        if (dock(**it, true)) it = sWindows.erase(it);
        else ++it;
    }
    sRequests.clear();
    sHasWork = !sWindows.empty();
    sCloseAll = false;
    return true;
}

static MASK uiModifiers(SDL_Keymod modifiers)
{
    MASK mask = MASK_NONE;
    if (modifiers & SDL_KMOD_SHIFT) mask |= MASK_SHIFT;
    if (modifiers & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) mask |= MASK_CONTROL;
    if (modifiers & SDL_KMOD_ALT) mask |= MASK_ALT;
    // AltGr enters text; it is never a viewer Ctrl shortcut.
    if ((modifiers & SDL_KMOD_MODE) ||
        ((modifiers & SDL_KMOD_RALT) && (modifiers & SDL_KMOD_LCTRL) && !(modifiers & SDL_KMOD_RCTRL)))
        mask &= ~(MASK_CONTROL | MASK_ALT);
    return mask;
}

void LLFloaterPopout::syncGeometry(Window& w)
{
    const F32 scale = llmax(0.25f, SDL_GetWindowDisplayScale(w.native)) *
                      llmax(0.5f, gSavedSettings.getF32("UIScaleFactor"));
    const bool changedScale = w.scale != LLVector2(scale, scale);
    w.scale = LLVector2(scale, scale);
    Context geometryContext(w);
    int width = 0, height = 0;
    SDL_GetWindowSizeInPixels(w.native, &width, &height);
    const S32 uiWidth = llmax(1, ll_round(width / scale));
    const S32 uiHeight = llmax(1, ll_round(height / scale));
    if (auto* floater = w.floater.get())
    {
        if (changedScale || w.root->getRect().getWidth() != uiWidth || w.root->getRect().getHeight() != uiHeight)
        {
            w.root->reshape(uiWidth, uiHeight);
            gViewerWindow->mWindowRectScaled = w.root->getLocalRect();
            floater->setOrigin(0, 0);
            floater->reshape(uiWidth, uiHeight);
        }
        if (changedScale || !w.limitsSet)
        {
            const F32 nativeScale = scale / llmax(0.25f, SDL_GetWindowPixelDensity(w.native));
            SDL_Rect area{};
            SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(w.native), &area);
            const int limitWidth = llmin(llmax(100, ll_round(floater->getMinWidth() * nativeScale)), llmax(100, area.w));
            const int limitHeight = llmin(llmax(60, ll_round(floater->getMinHeight() * nativeScale)), llmax(60, area.h));
            SDL_SetWindowMinimumSize(w.native, limitWidth, limitHeight);
            w.limitsSet = true;
        }
    }
}

void LLFloaterPopout::hoverViews(Window& w, bool inside, MASK mask)
try
{
    if (!inside)
    {
        LLView::clearHoverViews(w.hovered, w.mouse.mX, w.mouse.mY, mask);
        return;
    }
    std::set<LLHandle<LLView>> next;
    LLView* root = w.root.get();
    if (auto* captor = dynamic_cast<LLView*>(gFocusMgr.getMouseCapture()))
        if (captor->hasAncestor(w.root.get()))
        {
            root = captor;
            for (LLView* parent = captor->getParent(); parent; parent = parent->getParent())
                next.insert(parent->getHandle());
        }
    for (const auto& handle : w.popups->getCurrentPopups())
        if (auto* popup = handle.get())
            if (popup->hasAncestor(w.root.get()) && popup->calcScreenBoundingRect().pointInRect(w.mouse.mX, w.mouse.mY))
                LLView::collectHoverViews(popup, w.mouse.mX, w.mouse.mY, false, next);
    auto* top = gFocusMgr.getTopCtrl();
    if (top && top->hasAncestor(w.root.get()) && top->calcScreenBoundingRect().pointInRect(w.mouse.mX, w.mouse.mY))
        LLView::collectHoverViews(top, w.mouse.mX, w.mouse.mY, false, next);
    else
        LLView::collectHoverViews(root, w.mouse.mX, w.mouse.mY, true, next);

    // Snapshot before callbacks: an enter/leave may destroy a sibling or close a
    // popup. Never retain raw control pointers, or iterate a callback-owned set.
    std::vector<LLHandle<LLView>> entered, departed;
    std::set_difference(next.begin(), next.end(), w.hovered.begin(), w.hovered.end(), std::back_inserter(entered));
    std::set_difference(w.hovered.begin(), w.hovered.end(), next.begin(), next.end(), std::back_inserter(departed));
    w.hovered.swap(next);
    // Inventory relies on main's enter-before-leave ordering.
    for (const auto& handle : entered)
        if (auto* view = handle.get())
        {
            S32 x = 0, y = 0;
            view->screenPointToLocal(w.mouse.mX, w.mouse.mY, &x, &y);
            view->onMouseEnter(x, y, mask);
        }
    for (const auto& handle : departed)
        if (auto* view = handle.get())
        {
            S32 x = 0, y = 0;
            view->screenPointToLocal(w.mouse.mX, w.mouse.mY, &x, &y);
            view->onMouseLeave(x, y, mask);
        }
}
catch (const std::bad_alloc&)
{
    LLView::clearHoverViews(w.hovered, w.mouse.mX, w.mouse.mY, mask);
    LLToolTipMgr::instance().blockToolTips();
    LL_WARNS_ONCE("NativeUI") << "Hover tracking cleared under memory pressure" << LL_ENDL;
}

void LLFloaterPopout::clearHoverStates(Window* keep)
{
    const MASK mask = uiModifiers(SDL_GetModState());
    // This runs before entering a native Context. Main's updateUI may be
    // skipped while a native window owns the pointer, so drain it explicitly.
    LLView::clearHoverViews(gViewerWindow->mMouseHoverViews,
        gViewerWindow->mCurrentMousePoint.mX, gViewerWindow->mCurrentMousePoint.mY, mask);
    gViewerWindow->mMouseInWindow = false;
    for (auto& window : sWindows)
    {
        if (window.get() == keep) continue;
        Context context(*window);
        window->hoverEnabled = false;
        hoverViews(*window, false, mask);
    }
    LLToolTipMgr::instance().blockToolTips();
}

void LLFloaterPopout::dispatchHover(Window& w, MASK mask)
{
    LLMouseHandler* target = w.root.get();
    if (auto* captor = dynamic_cast<LLView*>(gFocusMgr.getMouseCapture()))
        if (captor->hasAncestor(w.root.get())) target = captor;
    if (gFocusMgr.getMouseCapture() == LLToolDragAndDrop::getInstance())
        target = gFocusMgr.getMouseCapture();
    if (target == w.root.get())
        if (auto* top = gFocusMgr.getTopCtrl())
            if (top->hasAncestor(w.root.get()) && top->calcScreenBoundingRect().pointInRect(w.mouse.mX, w.mouse.mY))
                target = top;
    S32 x = 0, y = 0;
    target->screenPointToLocal(w.mouse.mX, w.mouse.mY, &x, &y);
    target->handleHover(x, y, mask);
}

bool LLFloaterPopout::event(LLNativeWindowID id, const SDL_Event& e)
{
    if (sWindows.empty()) return false;
    auto* platform = static_cast<LLWindowSDL*>(gViewerWindow->getWindow());
    SDL_Window* source = SDL_GetWindowFromEvent(&e);
    if (e.type == SDL_EVENT_WINDOW_FOCUS_GAINED || e.type == SDL_EVENT_WINDOW_FOCUS_LOST ||
        e.type == SDL_EVENT_WINDOW_MOUSE_LEAVE)
        clearHoverStates();
    if (e.type == SDL_EVENT_DISPLAY_REMOVED)
    {
        for (auto& window : sWindows) window->monitorCheck = true;
        return false;
    }
    // SDL capture is unnecessary for inventory cargo: the cargo remains in
    // LLToolDragAndDrop while each target window receives its own local events.
    // Ordinary text selection/slider capture keeps its existing behavior.
    if (gFocusMgr.getMouseCapture() == LLToolDragAndDrop::getInstance()) SDL_CaptureMouse(false);
    if (source == platform->getMainSDLWindow())
    {
        if (e.type == SDL_EVENT_WINDOW_MOUSE_LEAVE && gFocusMgr.getMouseCapture() == LLToolDragAndDrop::getInstance())
        {
            // Cargo capture intentionally survives entry into another window.
            // The main callback assumes that a mouse-leave has no capture.
            gViewerWindow->mMouseInWindow = false;
            LLToolTipMgr::instance().blockToolTips();
            return true;
        }
        if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST)
        {
            if (gFocusMgr.childHasKeyboardFocus(gViewerWindow->getRootView()))
                if (auto* focus = dynamic_cast<LLView*>(gFocusMgr.getKeyboardFocus())) sMainFocus = focus->getHandle();
            for (const auto& window : sWindows)
                if (SDL_GetKeyboardFocus() == window->native)
                {
                    // Release world tools/relative capture and exclusive mode,
                    // while keeping application focus for its native UI window.
                    gViewerWindow->handleFocusLost(platform);
                    gFocusMgr.setAppHasFocus(true);
                    if (gMenuHolder) gMenuHolder->hideMenus();
                    return true;
                }
        }
        else if (e.type == SDL_EVENT_WINDOW_FOCUS_GAINED)
        {
            platform->setTextInputWindow(nullptr);
            gKeyboard->resetKeys();
            for (auto& window : sWindows)
            {
                if (gFocusMgr.childHasKeyboardFocus(window->root.get()))
                {
                    if (auto* focus = dynamic_cast<LLView*>(gFocusMgr.getKeyboardFocus())) window->focus = focus->getHandle();
                    if (auto* previous = dynamic_cast<LLUICtrl*>(sMainFocus.get())) previous->setFocus(true);
                    else gFocusMgr.setKeyboardFocus(nullptr);
                    break;
                }
            }
        }
        return false;
    }
    auto it = std::find_if(sWindows.begin(), sWindows.end(),
        [id](const auto& window) { return window->host->id() == id; });
    if (it == sWindows.end()) return false;
    Window& w = **it;
    if (e.type == SDL_EVENT_WINDOW_MOUSE_ENTER) clearHoverStates(&w);
    auto* floater = w.floater.get();
    if (!floater || floater->isDead() || w.closing) return true;
    if (e.type == SDL_EVENT_WINDOW_FOCUS_GAINED) platform->setTextInputWindow(w.native);
    syncGeometry(w);
    Context context(w);
    MASK mask = uiModifiers(SDL_GetModState());
    const F32 pixelDensity = llmax(0.25f, SDL_GetWindowPixelDensity(w.native));
    auto mousePosition = [&](F32 x, F32 y)
    {
        int width = 0, height = 0;
        SDL_GetWindowSizeInPixels(w.native, &width, &height);
        const auto point = native_window::inputPoint(x, y, pixelDensity, w.scale.mV[VX], w.scale.mV[VY], height);
        w.lastMouse = w.mouse;
        w.mouse = LLCoordGL(point.x, point.y);
        w.mouseDelta = LLCoordGL(w.mouse.mX - w.lastMouse.mX, w.mouse.mY - w.lastMouse.mY);
        gViewerWindow->mCurrentMousePoint = w.mouse;
        gViewerWindow->mLastMousePoint = w.lastMouse;
        gViewerWindow->mCurrentMouseDelta = w.mouseDelta;
        context.cursor = LLCoordWindow(ll_round(x * pixelDensity), ll_round(y * pixelDensity));
        LLUI::instance().resetMouseIdleTimer();
    };
    auto mouseTarget = [&]() -> LLMouseHandler*
    {
        LLMouseHandler* captor = gFocusMgr.getMouseCapture();
        if (captor == LLToolDragAndDrop::getInstance()) return captor;
        if (auto* view = dynamic_cast<LLView*>(captor))
            if (view->hasAncestor(w.root.get())) return captor;
        return w.root.get();
    };
    switch (e.type)
    {
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        // The OS close control docks. The in-floater close control retains the
        // original close/save-confirmation behavior of the same LLFloater.
        w.closing = true;
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        w.nextDraw = 0.;
        gKeyboard->resetKeys();
        gFocusMgr.setAppHasFocus(true);
        if (auto* focus = dynamic_cast<LLUICtrl*>(w.focus.get())) focus->setFocus(true);
        else floater->setFocus(true);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        platform->interruptLanguageTextInput();
        w.menus->hideMenus();
        w.popups->clearPopups();
        if (gFocusMgr.childHasKeyboardFocus(w.root.get()))
            if (auto* focus = dynamic_cast<LLView*>(gFocusMgr.getKeyboardFocus())) w.focus = focus->getHandle();
        gKeyboard->resetKeys();
        // A selection/slider capture belongs to this host even when focus
        // moves to another viewer window. Do not leave it armed on main.
        w.leftDown = w.middleDown = w.rightDown = false;
        if (gFocusMgr.childHasMouseCapture(w.root.get()))
        {
            gFocusMgr.setMouseCapture(nullptr);
            SDL_CaptureMouse(false);
        }
        if (!SDL_GetKeyboardFocus())
        {
            gFocusMgr.setAppHasFocus(false);
            SDL_CaptureMouse(false);
        }
        break;
    case SDL_EVENT_MOUSE_MOTION:
    {
        mousePosition(e.motion.x, e.motion.y);
        w.hoverEnabled = SDL_GetMouseFocus() == w.native;
        hoverViews(w, w.hoverEnabled, mask);
        LLToolTipMgr::instance().hideToolTips();
        if (w.hoverEnabled) LLToolTipMgr::instance().unblockToolTips();
        else if (auto* captured = dynamic_cast<LLView*>(gFocusMgr.getMouseCapture()))
            if (captured->hasAncestor(w.root.get()))
            {
                // A captured drag still needs off-window coordinates, but it
                // must not repaint hover after the OS leave cleared the set.
                auto handle = captured->getHandle();
                S32 x = 0, y = 0;
                captured->screenPointToLocal(w.mouse.mX, w.mouse.mY, &x, &y);
                captured->handleHover(x, y, mask);
                if (auto* live = handle.get()) live->onMouseLeave(x, y, mask);
            }
        platform->updateCursor();
        break;
    }
    case SDL_EVENT_WINDOW_MOUSE_ENTER:
    {
        F32 x = 0.f, y = 0.f;
        SDL_GetMouseState(&x, &y);
        mousePosition(x, y);
        w.hoverEnabled = true;
        w.nextDraw = 0.;
        LLToolTipMgr::instance().unblockToolTips();
        break;
    }
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
    case SDL_EVENT_WINDOW_MINIMIZED:
    case SDL_EVENT_WINDOW_HIDDEN:
        w.hoverEnabled = false;
        hoverViews(w, false, mask);
        LLToolTipMgr::instance().blockToolTips();
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    {
        mousePosition(e.button.x, e.button.y);
        LLMouseHandler* target = mouseTarget();
        S32 x = 0, y = 0;
        target->screenPointToLocal(w.mouse.mX, w.mouse.mY, &x, &y);
        const bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        if (e.button.button == SDL_BUTTON_LEFT) gViewerWindow->mLeftMouseDown = w.leftDown = down;
        if (e.button.button == SDL_BUTTON_MIDDLE) gViewerWindow->mMiddleMouseDown = w.middleDown = down;
        if (e.button.button == SDL_BUTTON_RIGHT) gViewerWindow->mRightMouseDown = w.rightDown = down;
        EMouseClickType click = CLICK_LEFT;
        if (e.button.button == SDL_BUTTON_RIGHT) click = CLICK_RIGHT;
        else if (e.button.button == SDL_BUTTON_MIDDLE) click = CLICK_MIDDLE;
        else if (e.button.button != SDL_BUTTON_LEFT) break;
        else if (down && e.button.clicks == 2) click = CLICK_DOUBLELEFT;
        target->handleAnyMouseClick(x, y, mask, click, down);
        // Capture outside-window release for text selection/resize; cargo must
        // instead be free to enter another viewer window.
        SDL_CaptureMouse(down && gFocusMgr.getMouseCapture() &&
                         gFocusMgr.getMouseCapture() != LLToolDragAndDrop::getInstance());
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
    {
        mousePosition(e.wheel.mouse_x, e.wheel.mouse_y);
        const F32 direction = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.f : 1.f;
        const F32 wheelY = -e.wheel.y * direction;
        const F32 wheelX = -e.wheel.x * direction;
        w.wheelY += wheelY;
        w.wheelX += wheelX;
        const S32 vertical = static_cast<S32>(w.wheelY);
        const S32 horizontal = static_cast<S32>(w.wheelX);
        w.wheelY -= vertical;
        w.wheelX -= horizontal;
        const bool smooth = gSavedSettings.getBOOL("UISmoothScroll");
        if (smooth)
        {
            LLScrollbar::sWheelDelta = wheelY;
            LLScrollbar::sHWheelDelta = wheelX;
            // Fractional events target the nearest scrolling ancestor, using the
            // same scroll easing code as the main UI, without camera fallback.
            LLView* under = w.root->childFromPoint(w.mouse.mX, w.mouse.mY, true);
            for (LLView* view = under; view; view = view->getParent())
            {
                auto* container = dynamic_cast<LLScrollContainer*>(view);
                auto* list = dynamic_cast<LLScrollListCtrl*>(view);
                if (!vertical && e.wheel.y != 0.f)
                {
                    if (container && container->handleScrollWheelFraction(wheelY, false)) break;
                    if (list && list->handleScrollWheelFraction(wheelY, false)) break;
                }
                if (!horizontal && e.wheel.x != 0.f)
                {
                    if (container && container->handleScrollWheelFraction(wheelX, true)) break;
                    if (list && list->handleScrollWheelFraction(wheelX, true)) break;
                }
            }
        }
        if (vertical) w.root->handleScrollWheel(w.mouse.mX, w.mouse.mY, vertical);
        if (horizontal) w.root->handleScrollHWheel(w.mouse.mX, w.mouse.mY, horizontal);
        LLScrollbar::sWheelDelta = LLScrollbar::sHWheelDelta = 0.f;
        break;
    }
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    {
        LLToolTipMgr::instance().blockToolTips();
        platform->setUIKeyData(e.key.key, e.key.mod);
        KEY key = KEY_NONE;
        if (!static_cast<LLKeyboardSDL*>(gKeyboard)->translateUIKey(e.key.key, e.key.mod, &key)) break;
        mask = uiModifiers(e.key.mod);
        auto* focus = gFocusMgr.getKeyboardFocus();
        if (!gFocusMgr.childHasKeyboardFocus(w.root.get())) focus = nullptr;
        if (e.type == SDL_EVENT_KEY_UP)
        {
            if (focus) focus->handleKeyUp(key, mask, false);
            break;
        }
        // Candidate navigation belongs to the native IME, not the web page or
        // application shortcuts. The commit arrives separately as text input.
        if (platform->preeditActive() && dynamic_cast<LLMediaCtrl*>(focus)) break;
        bool handled = w.menus->hasVisibleMenu() && w.menus->handleKey(key, mask, true);
        // Media deliberately consumes Ctrl+letters without forwarding them to
        // CEF. Its edit commands therefore need the same menu dispatch as main.
        if (!handled && dynamic_cast<LLMediaCtrl*>(focus) && gEditMenu)
            handled = gEditMenu->handleAcceleratorKey(key, mask);
        if (focus && !handled)
        {
            LLHandle<LLView> mediaFocus;
            if (auto* media = dynamic_cast<LLMediaCtrl*>(focus)) mediaFocus = media->getHandle();
            handled = focus->handleKey(key, mask, false);
            // CEF needs a character for Enter as well as the key event.
            // Main dispatch supplies this character pass separately. Keep it
            // local to the focused browser; do not invoke world key bindings.
            if (key == KEY_RETURN && !mediaFocus.isDead() &&
                gFocusMgr.getKeyboardFocus() == dynamic_cast<LLMediaCtrl*>(mediaFocus.get()) &&
                gFocusMgr.childHasKeyboardFocus(w.root.get()))
            {
                platform->setUIKeyData(SDLK_RETURN, e.key.mod, true);
                mediaFocus.get()->handleKey(key, mask, false);
                platform->setUIKeyData(e.key.key, e.key.mod);
            }
        }
        if (!handled && gEditMenu) handled = gEditMenu->handleAcceleratorKey(key, mask);
        // Give the entire focused control chain first refusal. Only explicit
        // command shortcuts enter the shared menu accelerators, never plain text
        // or the world's gesture/held-key/movement binding path.
        if (!handled && SLKeyCommands::instance().matchesMenuItem("viewer", "Close Window", key, mask))
        {
            floater->closeFloater();
            handled = true;
        }
        if (!handled && (mask & (MASK_CONTROL | MASK_ALT)))
        {
            // Application commands (quit/fullscreen/preferences) must see the
            // main viewer context. In particular requestQuit closes main floaters.
            context.restore();
            if (gMenuBarView) handled = gMenuBarView->handleAcceleratorKey(key, mask);
        }
        if (!handled && key == KEY_TAB && !(mask & (MASK_CONTROL | MASK_ALT)))
        {
            if (mask & MASK_SHIFT) w.root->focusPrevRoot();
            else w.root->focusNextRoot();
        }
        break;
    }
    case SDL_EVENT_TEXT_INPUT:
    {
        if (!gFocusMgr.childHasKeyboardFocus(w.root.get())) break;
        platform->setUIKeyData(0, SDL_GetModState());
        // End preedit before inserting committed Unicode, as the native editor
        // API expects (selection/caret state otherwise still includes preedit).
        if (platform->preeditor() && platform->preeditActive()) platform->preeditor()->resetPreedit();
        platform->setPreeditActive(false);
        const LLWString text = utf8str_to_wstring(e.text.text);
        for (llwchar ch : text)
        {
            // A commit callback may close the editor or move focus to another
            // root. Never send the remainder of this host's commit there.
            if (!gFocusMgr.childHasKeyboardFocus(w.root.get())) break;
            if (auto* focus = gFocusMgr.getKeyboardFocus()) focus->handleUnicodeChar(ch, false);
        }
        break;
    }
    case SDL_EVENT_TEXT_EDITING:
    {
        if (!gFocusMgr.childHasKeyboardFocus(w.root.get())) break;
        platform->setPreeditActive(e.edit.text && *e.edit.text);
        LLPreeditor* editor = platform->preeditor();
        // CEF accepts committed Unicode but exposes no inline preedit API.
        // Keep native composition ownership even without an LL text editor so
        // changing windows cancels it instead of committing into another root.
        if (!editor) break;
        const LLWString text = utf8str_to_wstring(e.edit.text);
        editor->resetPreedit();
        platform->setPreeditActive(!text.empty());
        if (!text.empty())
        {
            // SDL uses UTF-8 character indices for the composition selection.
            const S32 start = llclamp(e.edit.start, 0, static_cast<S32>(text.size()));
            const S32 length = llclamp(e.edit.length, 0, static_cast<S32>(text.size()) - start);
            LLPreeditor::segment_lengths_t lengths;
            LLPreeditor::standouts_t standouts;
            if (start) { lengths.push_back(start); standouts.push_back(false); }
            if (length) { lengths.push_back(length); standouts.push_back(true); }
            const S32 tail = static_cast<S32>(text.size()) - start - length;
            if (tail) { lengths.push_back(tail); standouts.push_back(false); }
            editor->updatePreedit(text, lengths, standouts, start + length);
        }
        break;
    }
    case SDL_EVENT_WINDOW_MOVED:
    case SDL_EVENT_WINDOW_RESIZED:
        if (!(SDL_GetWindowFlags(w.native) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED)))
        {
            w.bounds = w.host->floatingBounds();
        }
        w.savePending = true;
        break;
    case SDL_EVENT_WINDOW_MAXIMIZED:
    case SDL_EVENT_WINDOW_RESTORED:
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        LLVulkanContext::instance().invalidateUIWindow(w.renderHandle);
        w.savePending = true;
        break;
    default:
        break;
    }
    return true;
}

void LLFloaterPopout::render(U64 frameBeforeMain)
{
    auto& renderer = LLVulkanContext::instance();
    bool advanced = renderer.uiFrameCounter() != frameBeforeMain;
    const F64 now = LLFrameTimer::getTotalSeconds();
    for (auto& window : sWindows)
    {
        Window& w = *window;
        auto* floater = w.floater.get();
        if (!floater || floater->isDead() || !floater->getVisible() || w.closing) continue;
        const bool focused = SDL_GetKeyboardFocus() == w.native;
        // Unfocused UI still refreshes model changes, but at 10 Hz; focused UI is
        // capped at 60 Hz independently of the world's presentation policy.
        if (now < w.nextDraw) continue;
        syncGeometry(w);
        const F32 scale = w.scale.mV[VX];
        int width = 0, height = 0;
        SDL_GetWindowSizeInPixels(w.native, &width, &height);
        const bool inside = w.hoverEnabled && SDL_GetMouseFocus() == w.native &&
            !(SDL_GetWindowFlags(w.native) & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED));
        if (inside)
        {
            // Layout, smooth scrolling and DPI can move the row under a still
            // pointer. Resample in this host's units at its bounded UI cadence.
            F32 x = 0.f, y = 0.f;
            SDL_GetMouseState(&x, &y);
            const auto point = native_window::inputPoint(x, y,
                llmax(0.25f, SDL_GetWindowPixelDensity(w.native)), scale, scale, height);
            w.mouse = LLCoordGL(point.x, point.y);
        }
        if (w.title != floater->getTitle())
        {
            w.title = floater->getTitle();
            w.host->setTitle(w.title);
        }
        Context context(w);
        const MASK mask = uiModifiers(SDL_GetModState());
        hoverViews(w, inside, mask);
        if (inside)
        {
            dispatchHover(w, mask);
            static_cast<LLWindowSDL*>(gViewerWindow->getWindow())->updateCursor();
            if (!gFocusMgr.getMouseCapture()) w.root->handleToolTip(w.mouse.mX, w.mouse.mY, mask);
            LLToolTipMgr::instance().updateToolTipVisibility();
        }
        const bool rendered = renderer.renderUIWindow(w.renderHandle, [&]
        {
            gl_state_for_2d(width, height);
            // Font atlases are shared and remain at the main window's raster
            // scale. Convert the whole UI (glyphs included) to this window's DPI
            // without rebuilding the global font registry on every window switch.
            gGL.scalef(scale / LLFontGL::sScaleX, scale / LLFontGL::sScaleY, 1.f);
            gGL.pushUIMatrix();
            gGL.loadUIIdentity();
            gGL.scaleUI(LLFontGL::sScaleX, LLFontGL::sScaleY, 1.f);
            w.root->draw();
            gGL.popUIMatrix();
            gGL.flush();
        }, !advanced);
        if (renderer.uiWindowFailed(w.renderHandle))
        {
            LL_WARNS("Popout") << "Native UI presentation failed; docking the existing floater" << LL_ENDL;
            w.closing = true;
        }
        if (rendered)
        {
            advanced = true;
            w.lastMouse = w.mouse;
            w.mouseDelta = LLCoordGL(0, 0);
            w.nextDraw = now + (focused ? 1. / 60. : 0.1);
        }
    }
    renderer.finishUIWindowsFrame();
}
