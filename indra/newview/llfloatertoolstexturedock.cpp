/**
 * @file llfloatertoolstexturedock.cpp
 * @brief #577: Texture panel companion floater docked to the Build window.
 */

#include "llviewerprecompiledheaders.h"

#include "llfloatertoolstexturedock.h"

#include "llfloatertools.h"
#include "llpanel.h"

namespace
{
    constexpr S32 DOCK_PAD = 4;
}

LLFloaterToolsTextureDock::LLFloaterToolsTextureDock(const LLSD& key)
:   LLFloater(key)
{
    setAutoFocus(false);
}

LLFloaterToolsTextureDock::~LLFloaterToolsTextureDock()
{
    // Never let the panel die with us: it belongs to the Build window.
    if ((mPanel || mPanel2) && gFloaterTools)
    {
        gFloaterTools->undockTexturePanel(false);
    }
    else
    {
        if (mPanel) removeChild(mPanel);
        if (mPanel2) removeChild(mPanel2);
        mPanel = nullptr;
        mPanel2 = nullptr;
    }
}

bool LLFloaterToolsTextureDock::postBuild()
{
    setCanDrag(false);
    mTextureTitle = getTitle();
    return true;
}

void LLFloaterToolsTextureDock::onClose(bool app_quitting)
{
    // The user's X button: ask the Build window to take the panel back.
    if (gFloaterTools)
    {
        gFloaterTools->onTextureDockClosed();
    }
}

void LLFloaterToolsTextureDock::attachPanel(LLPanel* panel)
{
    if (!panel) return;
    mPanel = panel;
    setTitle(mTextureTitle);
    const LLRect prect = panel->getRect();
    const S32 w = prect.getWidth();
    const S32 h = prect.getHeight();

    reshape(w + 2 * DOCK_PAD, h + 2 * DOCK_PAD + getHeaderHeight(), false);
    addChild(panel);
    panel->setOrigin(DOCK_PAD, DOCK_PAD);
    panel->reshape(w, h, true);
    panel->setVisible(true);
    followTools();
}

void LLFloaterToolsTextureDock::attachPanels(LLPanel* left, LLPanel* right)
{
    if (!left || !right) return;
    mPanel = left;
    mPanel2 = right;
    const LLRect lr = left->getRect();
    const LLRect rr = right->getRect();
    const S32 h = llmax(lr.getHeight(), rr.getHeight());

    setTitle(getString("link_title"));
    reshape(lr.getWidth() + rr.getWidth() + 3 * DOCK_PAD, h + 2 * DOCK_PAD + getHeaderHeight(), false);
    addChild(left);
    addChild(right);
    left->setOrigin(DOCK_PAD, DOCK_PAD);
    left->reshape(lr.getWidth(), h, true);
    right->setOrigin(2 * DOCK_PAD + lr.getWidth(), DOCK_PAD);
    right->reshape(rr.getWidth(), h, true);
    left->setVisible(true);
    right->setVisible(true);
    followTools();
}

void LLFloaterToolsTextureDock::detachPanel(LLPanel* panel)
{
    if (!panel) return;
    if (panel == mPanel)
    {
        removeChild(panel);
        mPanel = nullptr;
    }
    else if (panel == mPanel2)
    {
        removeChild(panel);
        mPanel2 = nullptr;
    }
}

void LLFloaterToolsTextureDock::followTools()
{
    if (!gFloaterTools || !getParent()) return;

    const LLRect tools = gFloaterTools->getRect();
    const LLRect bounds = getParent()->getLocalRect();
    const S32 w = getRect().getWidth();
    const S32 h = getRect().getHeight();

    // Right of the Build window, same top. No room: left of it. Then clamp.
    S32 left = tools.mRight;
    if (left + w > bounds.mRight)
    {
        left = tools.mLeft - w;
    }
    left = llmax(bounds.mLeft, llmin(left, bounds.mRight - w));

    S32 bottom = tools.mTop - h;
    bottom = llmax(bounds.mBottom, llmin(bottom, bounds.mTop - h));

    if (left != getRect().mLeft || bottom != getRect().mBottom)
    {
        setOrigin(left, bottom);
    }
}

void LLFloaterToolsTextureDock::draw()
{
    followTools();
    LLFloater::draw();
}
