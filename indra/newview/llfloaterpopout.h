/** Native presentation of existing floater view trees. */
#pragma once

#include "llfloater.h"
#include "llnativewindow.h"
#include <memory>
#include <vector>

union SDL_Event;

class LLFloaterPopout
{
public:
    static void init();
    static void shutdown();
    static bool hasWork() { return sHasWork; }
    static bool ownsMouse();
    // A bounded host lookup, including OS visibility. Does not switch UI state.
    static bool isVisibleNativeView(const LLView* view);
    // Explicit Studio world actions restore/raise main without changing its
    // camera or borrowing a native root's coordinates.
    static void showWorldView();
    static bool isWorldViewMinimized();
    static bool prepareForQuit();
    static void closeAll();
    static void update();
    static void render(U64 frameBeforeMain);

private:
    struct Window;
    struct Context;
    struct Request { LLHandle<LLFloater> floater; bool pop; bool restore; };
    static bool event(LLNativeWindowID id, const SDL_Event& event);
    static bool detach(LLFloater* floater, bool restore);
    static bool dock(Window& window, bool rememberPopout, bool savePlacement = true);
    static void save(Window& window, bool popped);
    static LLSD placement(LLFloater* floater);
    static void syncGeometry(Window& window);
    static void hoverViews(Window& window, bool inside, MASK mask);
    static void clearHoverStates(Window* keep = nullptr);
    static void dispatchHover(Window& window, MASK mask);
    static bool sHasWork;
    static bool sCloseAll;
    static std::vector<Request> sRequests;
    static std::vector<std::unique_ptr<Window>> sWindows;
    static LLHandle<LLView> sMainFocus;
};
