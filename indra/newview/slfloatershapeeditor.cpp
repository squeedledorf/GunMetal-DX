/**
 * @file slfloatershapeeditor.cpp
 * @brief The shape editor's windows (Shape editor, #653).
 */

#include "llviewerprecompiledheaders.h"
#include "slfloatershapeeditor.h"

#include "llpanel.h"
#include "slpanelshapeeditor.h"

// ============================================================================
// SLFloaterShapeEditor
// ============================================================================

SLFloaterShapeEditor::SLFloaterShapeEditor(const LLSD& key)
:   LLFloater(key)
{
}

bool SLFloaterShapeEditor::postBuild()
{
    mBody = getChild<LLPanel>("editor_body");
    return LLFloater::postBuild();
}

void SLFloaterShapeEditor::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);

    // Here rather than in postBuild: the window is kept when closed, and the
    // editor comes in each time it opens.
    SLPanelShapeEditor* editor = SLPanelShapeEditor::findInstance();
    if (!editor)
    {
        closeFloater();
        return;
    }
    editor->attachTo(mBody, this);
    applyMinimumSize();
}

void SLFloaterShapeEditor::onClose(bool app_quitting)
{
    // Closing the window puts the editor back in the sidebar -- unless the
    // viewer is closing, which takes both.
    if (!app_quitting)
    {
        if (SLPanelShapeEditor* editor = SLPanelShapeEditor::findInstance())
        {
            editor->onFloaterClosed();
        }
    }
    LLFloater::onClose(app_quitting);
}

bool SLFloaterShapeEditor::handleKeyHere(KEY key, MASK mask)
{
    if (SLPanelShapeEditor* editor = SLPanelShapeEditor::findInstance())
    {
        if (editor->handleEditorKey(key, mask))
        {
            return true;
        }
    }
    return LLFloater::handleKeyHere(key, mask);
}

void SLFloaterShapeEditor::applyMinimumSize()
{
    SLPanelShapeEditor* editor = SLPanelShapeEditor::findInstance();
    if (!editor || !mBody)
    {
        return;
    }
    S32 editor_width = 0;
    S32 editor_height = 0;
    editor->getMinimumSize(editor_width, editor_height);
    const S32 chrome_width = getRect().getWidth() - mBody->getRect().getWidth();
    const S32 chrome_height = getRect().getHeight() - mBody->getRect().getHeight();
    const S32 min_width = chrome_width + editor_width;
    const S32 min_height = chrome_height + editor_height;
    setResizeLimits(min_width, min_height);

    const LLRect rect = getRect();
    if (rect.getWidth() < min_width || rect.getHeight() < min_height)
    {
        // Grown from the top left corner, which is where the eye is.
        LLRect grown;
        grown.setLeftTopAndSize(rect.mLeft, rect.mTop, llmax(rect.getWidth(), min_width), llmax(rect.getHeight(), min_height));
        setShape(grown);
    }
}

// ============================================================================
// SLFloaterShapePane
// ============================================================================

SLFloaterShapePane::SLFloaterShapePane(const LLSD& key)
:   LLFloater(key),
    mIndex(llclamp((S32)key.asInteger(), 0, SLPanelShapeEditor::MAX_SLOTS - 1))
{
}

bool SLFloaterShapePane::postBuild()
{
    SLPanelShapeEditor* editor = SLPanelShapeEditor::findInstance();
    LLPanel* body = getChild<LLPanel>("pane_body");

    mSlot = new SLShapeEditorSlot(mIndex, true);
    mSlot->build();
    // setShape, so the panel lays its children out from its XUI size.
    mSlot->setFollowsAll();
    body->addChild(mSlot);
    mSlot->setShape(body->getLocalRect());
    mSlot->setSection(editor ? editor->getSlotSection(mIndex) : std::string("all"), true);

    refreshTitle();
    return LLFloater::postBuild();
}

void SLFloaterShapePane::refreshTitle()
{
    if (!mSlot)
    {
        return;
    }
    LLStringUtil::format_map_t args;
    args["[SECTION]"] = mSlot->getSectionLabel();
    setTitle(getString("title_format", args));
}

void SLFloaterShapePane::onClose(bool app_quitting)
{
    // Closing the window puts the panel back -- unless the editor itself is
    // closing windows (it remembers these as out) or the viewer is.
    SLPanelShapeEditor* editor = SLPanelShapeEditor::findInstance();
    if (editor && !editor->isClosing() && !app_quitting && mSlot)
    {
        editor->onPaneClosed(mIndex, mSlot->getSectionId());
    }
    LLFloater::onClose(app_quitting);
}

bool SLFloaterShapePane::handleKeyHere(KEY key, MASK mask)
{
    if (SLPanelShapeEditor* editor = SLPanelShapeEditor::findInstance())
    {
        if (editor->handleEditorKey(key, mask))
        {
            return true;
        }
    }
    return LLFloater::handleKeyHere(key, mask);
}
