/**
 * @file slpanelshapeeditor.h
 * @brief The shape wearable's sliders in up to four panels, each with a
 *        section dropdown (Shape editor, #653).
 *
 * The same design as Photo Studio (#444): SLShapeEditorSlot is a dropdown and a
 * pop-out button over a section; SLPanelShapeSection is one instance of a
 * section per panel, with a widget set of its own, so two panels showing the
 * same section never reach into each other. The visual params are the source of
 * truth; a slider that moves one tells the others (LLScrollingPanelParamBase).
 *
 * SLPanelShapeEditor is the whole editor: a top bar (Find, how many panels, out
 * to a window) over the panels. It lives in the Shape page of LLPanelEditWearable
 * (the Appearance sidebar), or in SLFloaterShapeEditor when the sidebar is too
 * narrow for more than one panel. LLPanelEditWearable keeps what is not the
 * sliders: Save / Save As / Revert, the dirty state, the name, the sex, the
 * height. The other wearable editors could adopt this frame later: nothing in
 * here but the section table is about the shape.
 */

#ifndef SL_PANELSHAPEEDITOR_H
#define SL_PANELSHAPEEDITOR_H

#include "llpanel.h"
#include "llframetimer.h"

#include <boost/signals2.hpp>

#include <array>
#include <string>
#include <vector>

class LLAccordionCtrl;
class LLAccordionCtrlTab;
class LLButton;
class LLComboBox;
class LLLineEditor;
class LLPanelEditWearable;
class LLScrollingPanelList;
class LLScrollingPanelParamBase;
class LLTextBox;
class SLFloaterShapeEditor;
class SLPanelShapeEditor;
class SLPanelShapeSection;

/// What a panel's dropdown can show: everything in an accordion (what the Shape
/// page always was), or one of the wearable's slider groups.
struct SLShapeSectionDef
{
    /// Saved in SLShapeEditorPanels, so never change one.
    const char* mId;
    /// The wearable dictionary's subpart (LLPanelEditWearable::fillSubpartList),
    /// or -1 for all of them.
    S32 mSubpart;
};

namespace SLShapeSections
{
const std::vector<SLShapeSectionDef>& all();
/// Null for an unknown id.
const SLShapeSectionDef* find(const std::string& id);
const SLShapeSectionDef* forSubpart(S32 subpart);
inline bool isAll(const SLShapeSectionDef* def) { return def && def->mSubpart < 0; }
}

/**
 * One section in one panel: the sliders of one slider group (a scrolling list),
 * or of all of them in an accordion. Every instance builds its own sliders.
 */
class SLPanelShapeSection final : public LLPanel
{
public:
    SLPanelShapeSection(S32 slot_index, const SLShapeSectionDef& def);
    ~SLPanelShapeSection() override;

    bool build();
    bool postBuild() override;
    void draw() override;

    const std::string& getSectionId() const { return mSectionId; }
    bool isAll() const { return mIsAll; }

    /// Rebuild the sliders from the wearable (new wearable, new sex).
    void refill();
    /// Put every slider where its parameter is.
    void updateValues();

    /// For "all": the subpart of the tab that is open, else -1.
    S32 getExpandedSubpart() const;

    /// Find: open the group, scroll to the slider of @a param_id (-1: only the
    /// group), focus it and light it up.
    bool showTarget(S32 subpart, S32 param_id);

private:
    struct Group
    {
        S32 mSubpart = 0;
        LLScrollingPanelList* mList = nullptr;
        LLAccordionCtrlTab* mTab = nullptr;
    };

    void onParamMoved(LLScrollingPanelParamBase* source);
    void clearLit();

    S32 mSlotIndex = 0;
    std::string mSectionId;
    bool mIsAll = false;
    LLAccordionCtrl* mAccordion = nullptr;
    std::vector<Group> mGroups;
    boost::signals2::scoped_connection mParamMovedConnection;

    LLHandle<LLView> mLit;
    LLFrameTimer mLitTimer;
};

/**
 * One panel: the section dropdown and pop-out button over the section it
 * shows. Lives in the editor's grid, or alone in a popped-out window
 * (SLFloaterShapePane), where the pop-out button puts it back.
 */
class SLShapeEditorSlot final : public LLPanel
{
public:
    SLShapeEditorSlot(S32 index, bool in_pane);
    ~SLShapeEditorSlot() override;

    bool build();
    bool postBuild() override;

    S32 getIndex() const { return mIndex; }
    const std::string& getSectionId() const { return mSectionId; }
    /// What the dropdown calls it.
    std::string getSectionLabel() const;
    /// Show @a id (an unknown id shows everything). Rebuilds the body only
    /// when the section changes, or when @a force.
    void setSection(const std::string& id, bool force = false);
    /// Name the section without building it (for a panel that is not shown).
    void rememberSection(const std::string& id);
    /// The section panel, or null while popped out.
    SLPanelShapeSection* getSectionPanel() const { return mSection; }
    bool hasContent() const { return mSection != nullptr; }
    /// Drop what the body holds, keeping the section id.
    void releaseContent() { clearBody(); }

    /// In the editor's grid: whether the panel is out in a window of its own,
    /// in which case this shows a note and a way back instead.
    void setPoppedOut(bool popped);
    bool isPoppedOut() const { return mPoppedOut; }

    /// Ctrl+1..4: the keyboard to the section dropdown.
    void focusSectionCombo();
    bool hasKeyboardFocusInside() const;

private:
    void onSectionPicked();
    void onPopOutClicked();
    void clearBody();

    S32 mIndex = 0;
    bool mInPane = false;
    bool mPoppedOut = false;
    std::string mSectionId;

    LLComboBox* mSectionCombo = nullptr;
    LLButton* mPopOutButton = nullptr;
    LLPanel* mBody = nullptr;
    LLView* mPoppedNote = nullptr;
    LLView* mPutBack = nullptr;
    SLPanelShapeSection* mSection = nullptr;
};

/**
 * The shape editor: top bar and panels. See the file comment.
 */
class SLPanelShapeEditor final : public LLPanel
{
public:
    static constexpr S32 MAX_SLOTS = 4;

    explicit SLPanelShapeEditor(LLPanelEditWearable* owner);
    ~SLPanelShapeEditor() override;

    /// The one shape editor, or null.
    static SLPanelShapeEditor* findInstance();

    /// Build from panel_shape_editor.xml into @a home (the Shape page's
    /// "shape_editor_host"), which the editor returns to when its window closes.
    bool build(LLPanel* home);
    bool postBuild() override;
    void draw() override;
    void reshape(S32 width, S32 height, bool called_from_parent = true) override;
    /// Ctrl+F goes to Find and Ctrl+1..4 to a panel's dropdown. Panel-local: a
    /// global Ctrl+F would fire while the user is typing anywhere.
    bool handleKeyHere(KEY key, MASK mask) override;
    /// The editor's keys, for this panel and its popped-out windows.
    bool handleEditorKey(KEY key, MASK mask);

    // --- For LLPanelEditWearable ---

    LLPanelEditWearable* getOwner() const { return mOwner; }
    /// The owner is going away (the editor may live on in a window).
    void detachOwner(LLPanelEditWearable* owner);
    /// The Shape page is shown (the wearable is set) or hidden. Hidden, the
    /// editor's window and the panels' windows close (and come back with the
    /// page); the panels' contents are released.
    void setActive(bool active);
    /// Rebuild the sliders in every panel (new wearable, new sex).
    void refillAll();
    /// Put every slider where its parameter is.
    void updateValues();
    /// Camera framing for the panel that was last used (the first, else).
    void frameFocusedSection();

    // --- For the panels ---

    /// Fill a freshly built section (unless the editor is about to refill
    /// everything).
    void fillSection(SLPanelShapeSection* section);
    void onSlotSectionChanged(S32 index, const std::string& id);
    /// A tab of an "all" panel opened: frame its group.
    void onSectionTabExpanded(S32 slot_index, S32 subpart);
    void popOut(S32 index);
    void putBack(S32 index);
    /// A popped-out window closed.
    void onPaneClosed(S32 index, const std::string& id);
    const std::string& getSlotSection(S32 index) const;
    bool isClosing() const { return mClosing; }

    // --- For the editor's window ---

    bool isFloated() const { return mFloated; }
    /// Take the editor out into its window, or back to the Shape page. The
    /// choice is remembered (SLShapeEditorFloated).
    void setFloated(bool floated);
    /// The window shows the editor: move into @a parent.
    void attachTo(LLPanel* parent, SLFloaterShapeEditor* window);
    /// The window closed: back to the Shape page.
    void onFloaterClosed();
    /// The smallest the editor may be for the layout now.
    void getMinimumSize(S32& width, S32& height) const;

private:
    S32 effectiveLayout() const { return mFloated ? mLayout : 1; }
    SLShapeEditorSlot* slotWidget(S32 index) const;
    S32 findFocusedSlot() const;

    void dockToHome();
    void setHomeNote(bool visible);

    void onLayoutPicked();
    /// Show the panels the layout shows (building them), hide the rest, open
    /// and close windows to match, size, arrange.
    void applyLayout();
    void arrangeSlots();
    void refreshTopBar();
    void loadState();
    void saveState();

    void frameSlot(S32 index, bool only_when_customizing);

    void onFind();
    void runFind(const std::string& text);
    SLShapeEditorSlot* findTargetSlot();

    template <typename Fn> void forEachSection(Fn fn);

    LLPanelEditWearable* mOwner = nullptr;
    LLHandle<LLView> mHome;
    LLHandle<LLView> mWindow;

    LLPanel* mSlotsArea = nullptr;
    LLComboBox* mLayoutCombo = nullptr;
    LLLineEditor* mFindEditor = nullptr;
    LLTextBox* mFindStatus = nullptr;
    LLButton* mFloatButton = nullptr;

    std::array<SLShapeEditorSlot*, MAX_SLOTS> mSlots = { { nullptr, nullptr, nullptr, nullptr } };
    std::array<std::string, MAX_SLOTS> mSlotSections;
    std::array<bool, MAX_SLOTS> mSlotPoppedOut = { { false, false, false, false } };
    /// The layout the user chose (1, 2 or 4). Docked in the sidebar only one
    /// panel fits, whatever this says; it applies again in the window.
    S32 mLayout = 1;
    bool mFloated = false;
    bool mActive = false;
    /// Set while windows are closed by the editor itself (hiding the page,
    /// fewer panels), which must not read as the user putting a panel back.
    bool mClosing = false;
    /// Set while the panels are built and the caller fills them afterwards.
    bool mDeferFill = false;
    S32 mLastFocusedSlot = -1;

    // --- Find ---
    std::string mLastFindText;
    size_t mLastFindIndex = 0;
};

#endif // SL_PANELSHAPEEDITOR_H
