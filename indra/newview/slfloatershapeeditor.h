/**
 * @file slfloatershapeeditor.h
 * @brief The shape editor's windows (Shape editor, #653): the whole editor out
 *        of the Appearance sidebar, and one panel out of the editor.
 */

#ifndef SL_FLOATERSHAPEEDITOR_H
#define SL_FLOATERSHAPEEDITOR_H

#include "llfloater.h"

class LLPanel;
class SLShapeEditorSlot;

/**
 * The whole shape editor in a window. The sidebar has room for one panel; more
 * than that wants a window the user can size. The editor panel
 * (SLPanelShapeEditor) moves in on open and back to the Shape page on close.
 */
class SLFloaterShapeEditor final : public LLFloater
{
public:
    SLFloaterShapeEditor(const LLSD& key);
    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;
    /// Ctrl+F and Ctrl+1..4 reach the editor from here too.
    bool handleKeyHere(KEY key, MASK mask) override;

    /// The smallest the window may be for the editor's layout, growing it if need be.
    void applyMinimumSize();

private:
    LLPanel* mBody = nullptr;
};

/**
 * A shape editor panel in a window of its own. Keyed by the panel's index;
 * closing it puts the panel back in the editor's grid.
 */
class SLFloaterShapePane final : public LLFloater
{
public:
    SLFloaterShapePane(const LLSD& key);
    bool postBuild() override;
    void onClose(bool app_quitting) override;
    /// Ctrl+F and Ctrl+1..4 reach the editor from here too.
    bool handleKeyHere(KEY key, MASK mask) override;

    S32 getIndex() const { return mIndex; }
    /// The panel's section changed: retitle.
    void refreshTitle();
    SLShapeEditorSlot* getSlot() const { return mSlot; }

private:
    S32 mIndex = 0;
    SLShapeEditorSlot* mSlot = nullptr;
};

#endif // SL_FLOATERSHAPEEDITOR_H
