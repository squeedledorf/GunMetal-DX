/**
 * @file gmdxtoolbarcluster.cpp
 * @brief Where each toolbar's buttons sit along its strip (see gmdxtoolbarcluster.h).
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

// <GMDX> whole file: see gmdxtoolbarcluster.h.

#include "llviewerprecompiledheaders.h"

#include "gmdxtoolbarcluster.h"

#include "gmdxchatlog.h"
#include "llfocusmgr.h"
#include "llrender2dutils.h"
#include "lltoolbar.h"
#include "lltoolbarview.h"
#include "lluictrlfactory.h"
#include "llviewercontrol.h"
#include "llwindow.h"

namespace
{
    constexpr S32 GRIP = 6;         // a grip's thickness along the strip
    constexpr S32 MIN_SPAN = 40;    // a free part shorter than this holds no buttons at all
    const char* const kKeys[LLToolBarEnums::TOOLBAR_COUNT] = { "", "left", "right", "bottom", "top" };
    const char* const kSetting = "GMDXToolbarClusterOffsets";

    struct State
    {
        bool known = false;
        S32 inset_lo = 0, inset_hi = 0;   // the toolbar's own rect in its strip, as the skin has it
        LLHandle<LLView> buttons;         // its button panel
        LLHandle<LLView> grips[2];        // at the low (bottom / left) and the high end of the buttons
        S32 side = -1;                    // the free part last used: 0 low, 1 high, -1 the whole strip
        S32 applied = 0;                  // the offset the buttons sit at (clamped to the free part)
        bool dragging = false;
        S32 drag_from = 0, drag_mouse = 0, drag_offset = 0;
    };
    State sState[LLToolBarEnums::TOOLBAR_COUNT];

    bool isVertical(const LLToolBar* t) { return LLToolBarEnums::getOrientation(t->getSideType()) == LLLayoutStack::VERTICAL; }
    S32 lo(const LLRect& r, bool v) { return v ? r.mBottom : r.mLeft; }
    S32 hi(const LLRect& r, bool v) { return v ? r.mTop : r.mRight; }
    void setAlong(LLRect& r, bool v, S32 l, S32 h) { if (v) { r.mBottom = l; r.mTop = h; } else { r.mLeft = l; r.mRight = h; } }

    // the mouse along the strip, in the strip's coordinates
    S32 mouseAlong(const LLToolBar* t)
    {
        S32 x = 0, y = 0;
        LLUI::getInstance()->getMousePositionLocal(t->getParent(), &x, &y);
        return isVertical(t) ? y : x;
    }

    void saveOffset(S32 i, S32 offset)
    {
        LLSD o = gSavedPerAccountSettings.getLLSD(kSetting);
        if (!o.isMap()) o = LLSD::emptyMap();
        if (offset) o[kKeys[i]] = offset;
        else o.erase(kKeys[i]);
        gSavedPerAccountSettings.setLLSD(kSetting, o);
    }

    // The buttons in one row, with the toolbar's padding (LLToolBar::updateLayoutAsNeeded): what a
    // free part must hold for them not to wrap. Button lengths don't depend on wrapping.
    S32 rowLength(const LLView* panel, bool v)
    {
        const LLToolBar::Params& p = LLUICtrlFactory::getDefaultParams<LLToolBar>();
        S32 len = v ? p.pad_top() + p.pad_bottom() : p.pad_left() + p.pad_right();
        S32 n = 0;
        for (const LLView* child : *panel->getChildList())
        {
            if (dynamic_cast<const LLToolBarButton*>(child))
            {
                len += v ? child->getRect().getHeight() : child->getRect().getWidth();
                ++n;
            }
        }
        return len + llmax(n - 1, 0) * p.pad_between();
    }

    // A grip at one end of a centred toolbar's buttons: drag to slide them, double-click to centre them.
    class Grip : public LLView
    {
    public:
        Grip(const LLView::Params& p, S32 index) : LLView(p), mIndex(index) {}

        void draw() override
        {
            LLToolBar* t = dynamic_cast<LLToolBar*>(getParent());
            if (!t || getRect().isEmpty()) return;   // (no room, or not a centred toolbar)
            S32 x = 0, y = 0;
            LLUI::getInstance()->getMousePositionLocal(t, &x, &y);
            const bool hot = hasMouseCapture() || pointInView(x - getRect().mLeft, y - getRect().mBottom);
            if (!hot && !t->getLocalRect().pointInRect(x, y)) return;   // (shown while the mouse is over the toolbar)
            static LLUIColor color = LLUIColorTable::instance().getColor("IconColor");
            const LLColor4 c = color.get() % (hot ? 0.9f : 0.35f);
            const LLRect r = getLocalRect();
            if (isVertical(t))
            {
                const S32 cy = r.getHeight() / 2;
                gl_line_2d(r.mLeft + 7, cy - 1, r.mRight - 7, cy - 1, c);
                gl_line_2d(r.mLeft + 7, cy + 1, r.mRight - 7, cy + 1, c);
            }
            else
            {
                const S32 cx = r.getWidth() / 2;
                gl_line_2d(cx - 1, r.mBottom + 7, cx - 1, r.mTop - 7, c);
                gl_line_2d(cx + 1, r.mBottom + 7, cx + 1, r.mTop - 7, c);
            }
        }

        bool handleMouseDown(S32 x, S32 y, MASK mask) override
        {
            LLToolBar* t = dynamic_cast<LLToolBar*>(getParent());
            if (!t) return false;
            State& st = sState[mIndex];
            gFocusMgr.setMouseCapture(this);
            st.dragging = true;
            st.drag_from = st.drag_offset = st.applied;
            st.drag_mouse = mouseAlong(t);
            return true;
        }

        bool handleHover(S32 x, S32 y, MASK mask) override
        {
            LLToolBar* t = dynamic_cast<LLToolBar*>(getParent());
            if (!t) return false;
            State& st = sState[mIndex];
            if (hasMouseCapture()) st.drag_offset = st.drag_from + mouseAlong(t) - st.drag_mouse;
            getWindow()->setCursor(isVertical(t) ? UI_CURSOR_SIZENS : UI_CURSOR_SIZEWE);
            return true;
        }

        bool handleMouseUp(S32 x, S32 y, MASK mask) override
        {
            if (hasMouseCapture()) gFocusMgr.setMouseCapture(nullptr);   // (onMouseCaptureLost keeps the place)
            return true;
        }

        void onMouseCaptureLost() override
        {
            State& st = sState[mIndex];
            if (!st.dragging) return;
            st.dragging = false;
            saveOffset(mIndex, st.applied);   // where the buttons stopped (at the chat unit's edge, say)
        }

        bool handleDoubleClick(S32 x, S32 y, MASK mask) override
        {
            if (hasMouseCapture()) gFocusMgr.setMouseCapture(nullptr);
            sState[mIndex].dragging = false;
            saveOffset(mIndex, 0);
            return true;
        }

    private:
        S32 mIndex;
    };

    void makeGrips(LLToolBar* t, S32 i)
    {
        for (LLHandle<LLView>& handle : sState[i].grips)
        {
            LLView::Params p;
            p.name("gmdx_cluster_grip");
            p.rect(LLRect());
            p.mouse_opaque(true);
            p.follows.flags(FOLLOWS_NONE);
            p.tool_tip("Drag to move these buttons along the toolbar. Double-click to centre them.");
            Grip* grip = new Grip(p, i);
            t->addChild(grip);
            handle = grip->getHandle();
        }
    }

    // Where along its strip one toolbar lays out (its rect there), and its grips.
    void place(LLToolBar* t, S32 i, const LLSD& offsets)
    {
        LLView* strip = t->getParent();
        if (!strip) return;
        State& st = sState[i];
        const bool v = isVertical(t);
        const LLRect rect = t->getRect();
        const LLRect s = strip->getLocalRect();
        if (!st.known)
        {
            st.known = true;
            st.inset_lo = lo(rect, v) - lo(s, v);
            st.inset_hi = hi(s, v) - hi(rect, v);
            if (LLView* panel = t->findChild<LLView>("button_panel")) st.buttons = panel->getHandle();
            makeGrips(t, i);
        }
        const S32 a0 = lo(s, v) + st.inset_lo, b0 = hi(s, v) - st.inset_hi;   // the whole strip
        S32 w0 = a0, w1 = b0;   // the part the toolbar gets
        const bool centred = t->getLayoutType() == LLToolBarEnums::LAYOUT_NONE && !t->isReadOnly();
        bool grips = false;
        LLView* panel = st.buttons.get();
        if (panel && t->hasButtons())
        {
            const S32 want = !centred ? 0 : st.dragging ? st.drag_offset : (offsets.has(kKeys[i]) ? offsets[kKeys[i]].asInteger() : 0);
            // the chat unit over the strip (across its thickness and along it): the free parts each side
            LLRect unit;
            const bool over = GMDXChatLog::unitPlace(strip, unit)
                              && lo(unit, !v) < hi(rect, !v) && hi(unit, !v) > lo(rect, !v)
                              && lo(unit, v) < b0 && hi(unit, v) > a0;
            const S32 spans[2][2] = { { a0, llclamp(lo(unit, v), a0, b0) }, { llclamp(hi(unit, v), a0, b0), b0 } };
            const S32 need = rowLength(panel, v) + 2 * GRIP;
            const S32 mid = (a0 + b0) / 2;
            const S32 c = mid + want;
            S32 pick = -1;
            if (over)
            {
                auto len = [&](S32 k) { return spans[k][1] - spans[k][0]; };
                auto fits = [&](S32 k) { return len(k) >= need; };
                auto miss = [&](S32 k) { return llmax(spans[k][0] + need / 2 - c, c - (spans[k][1] - need / 2), 0); };
                if (st.dragging && st.side >= 0 && fits(st.side)) pick = st.side;   // (a grip drag stops at the unit)
                else if (t->getLayoutType() != LLToolBarEnums::LAYOUT_FILL && (fits(0) || fits(1))) pick = !fits(1) || (fits(0) && miss(0) <= miss(1)) ? 0 : 1;
                else pick = len(0) >= len(1) ? 0 : 1;   // too short both (or filled): the longer one, wrapped
                if (len(pick) < MIN_SPAN) pick = -1;     // ponytail: the unit covers the whole strip; the buttons stay under it
            }
            st.side = pick;
            const S32 a = pick < 0 ? a0 : spans[pick][0], b = pick < 0 ? b0 : spans[pick][1];
            if (pick < 0 && want == 0)
            {
                st.applied = 0;   // as the skin has it
                grips = centred;
            }
            else if (centred && b - a >= need)
            {
                // a part centred on where the buttons should be, so the toolbar's centring puts them there
                const S32 cc = llclamp(c, a + need / 2, b - (need - need / 2));
                const S32 h = llmin(cc - a, b - cc);
                w0 = cc - h;
                w1 = cc + h;
                st.applied = cc - mid;
                grips = true;
            }
            else
            {
                w0 = a;
                w1 = b;
                st.applied = want;
                grips = centred;
            }
        }
        LLRect r = rect;
        setAlong(r, v, w0, w1);
        if (r != rect) t->setShape(r);   // (the toolbar lays out again on its next draw)

        // the grips just outside the ends of the buttons, where there is room in the toolbar
        LLRect b;
        if (panel) panel->localRectToOtherView(panel->getLocalRect(), &b, t);
        for (S32 k = 0; k < 2; ++k)
        {
            LLView* grip = st.grips[k].get();
            if (!grip) continue;
            LLRect g;
            if (grips)
            {
                g = b;
                if (k == 0) setAlong(g, v, lo(b, v) - GRIP, lo(b, v));
                else setAlong(g, v, hi(b, v), hi(b, v) + GRIP);
                if (!t->getLocalRect().contains(g)) g = LLRect();
            }
            if (g != grip->getRect()) grip->setRect(g);
        }
    }
}

namespace GMDXToolbarCluster
{
    void update()
    {
        if (!gToolBarView) return;
        static LLCachedControl<LLSD> offsets(gSavedPerAccountSettings, kSetting, LLSD());
        const LLSD saved = offsets;
        for (S32 i = LLToolBarEnums::TOOLBAR_FIRST; i <= LLToolBarEnums::TOOLBAR_LAST; ++i)
        {
            if (LLToolBar* t = gToolBarView->getToolbar((LLToolBarEnums::EToolBarLocation)i)) place(t, i, saved);
        }
    }
}
