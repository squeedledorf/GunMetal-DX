/**
 * @file slpanelshapeeditor.cpp
 * @brief The shape wearable's sliders in up to four panels (Shape editor, #653).
 *
 * See slpanelshapeeditor.h. The structure follows Photo Studio's
 * (slfloaterphotostudio.cpp, slpanelphotostudiosection.cpp), and so do the
 * words for it: slot, section, pop out, put back.
 */

#include "llviewerprecompiledheaders.h"
#include "slpanelshapeeditor.h"
#include "slkeycommands.h"

#include "llaccordionctrl.h"
#include "llaccordionctrltab.h"
#include "llagentcamera.h"
#include "llbutton.h"
#include "llcombobox.h"
#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "lllineeditor.h"
#include "llpaneleditwearable.h"
#include "llscrollingpanellist.h"
#include "llscrollingpanelparambase.h"
#include "lltextbox.h"
#include "lluicolortable.h"
#include "llviewercontrol.h"
#include "llviewerwearable.h"
#include "slfloatershapeeditor.h"

#include <algorithm>
#include <cctype>

namespace
{
const char* const EDITOR_FLOATER_NAME = "shape_editor";
const char* const PANE_FLOATER_NAME = "shape_editor_pane";

/// The smallest a panel may be: a slider row is 290 wide, beside the list's
/// scrollbar; and room for a few rows.
constexpr S32 MIN_SLOT_WIDTH = 330;
constexpr S32 MIN_SLOT_HEIGHT = 230;
constexpr S32 SLOT_GAP = 4;

/// How long Find lights the slider it went to.
constexpr F32 LIT_SECONDS = 3.f;

/// What a fresh editor shows: everything in the first panel (as the page always
/// did), then the three regions people tune together.
const std::array<const char*, SLPanelShapeEditor::MAX_SLOTS> DEFAULT_SECTIONS = { { "all", "head", "torso", "legs" } };

SLPanelShapeEditor* sInstance = nullptr;

const std::vector<SLShapeSectionDef> SECTIONS = {
    { "all", -1 },
    { "body", 0 },
    { "head", 1 },
    { "eyes", 2 },
    { "ears", 3 },
    { "nose", 4 },
    { "mouth", 5 },
    { "chin", 6 },
    { "torso", 7 },
    { "legs", 8 },
};

/// How well @a hay matches @a needle (both lower case): 0 exact, 1 prefix,
/// 2 a word's prefix, 3 anywhere; -1 not at all.
S32 matchScore(const std::string& hay, const std::string& needle)
{
    if (needle.empty())
    {
        return -1;
    }
    if (hay == needle)
    {
        return 0;
    }
    const size_t at = hay.find(needle);
    if (at == std::string::npos)
    {
        return -1;
    }
    if (at == 0)
    {
        return 1;
    }
    if (!isalnum((unsigned char)hay[at - 1]))
    {
        return 2;
    }
    return 3;
}
} // namespace

// ============================================================================
// SLShapeSections
// ============================================================================

const std::vector<SLShapeSectionDef>& SLShapeSections::all()
{
    return SECTIONS;
}

const SLShapeSectionDef* SLShapeSections::find(const std::string& id)
{
    for (const SLShapeSectionDef& def : SECTIONS)
    {
        if (id == def.mId)
        {
            return &def;
        }
    }
    return nullptr;
}

const SLShapeSectionDef* SLShapeSections::forSubpart(S32 subpart)
{
    for (const SLShapeSectionDef& def : SECTIONS)
    {
        if (def.mSubpart == subpart)
        {
            return &def;
        }
    }
    return nullptr;
}

// ============================================================================
// SLPanelShapeSection
// ============================================================================

SLPanelShapeSection::SLPanelShapeSection(S32 slot_index, const SLShapeSectionDef& def)
:   LLPanel(),
    mSlotIndex(slot_index),
    mSectionId(def.mId),
    mIsAll(def.mSubpart < 0)
{
}

SLPanelShapeSection::~SLPanelShapeSection()
{
}

bool SLPanelShapeSection::build()
{
    return buildFromFile(mIsAll ? "panel_shape_section_all.xml" : "panel_shape_section.xml");
}

bool SLPanelShapeSection::postBuild()
{
    mGroups.clear();
    if (mIsAll)
    {
        mAccordion = getChild<LLAccordionCtrl>("wearable_accordion");
        for (const SLShapeSectionDef& def : SLShapeSections::all())
        {
            if (def.mSubpart < 0)
            {
                continue;
            }
            Group group;
            group.mSubpart = def.mSubpart;
            group.mTab = getChild<LLAccordionCtrlTab>(std::string("shape_") + def.mId + "_tab");
            group.mList = group.mTab->getChild<LLScrollingPanelList>(std::string("shape_") + def.mId + "_param_list");
            mGroups.push_back(group);
        }
        // As the page always did: the first tab open. Before the callbacks below,
        // so building a panel does not move the camera.
        mAccordion->expandDefaultTab();
        for (const Group& group : mGroups)
        {
            const S32 slot_index = mSlotIndex;
            const S32 subpart = group.mSubpart;
            group.mTab->setDropDownStateChangedCallback([slot_index, subpart](LLUICtrl*, const LLSD& param)
            {
                if (param.asBoolean())
                {
                    if (SLPanelShapeEditor* editor = SLPanelShapeEditor::findInstance())
                    {
                        editor->onSectionTabExpanded(slot_index, subpart);
                    }
                }
            });
        }
    }
    else
    {
        Group group;
        const SLShapeSectionDef* def = SLShapeSections::find(mSectionId);
        group.mSubpart = def ? def->mSubpart : 0;
        group.mList = getChild<LLScrollingPanelList>("param_list");
        mGroups.push_back(group);
    }

    mParamMovedConnection = LLScrollingPanelParamBase::connectParamMoved(
        [this](LLScrollingPanelParamBase* source) { onParamMoved(source); });
    return LLPanel::postBuild();
}

void SLPanelShapeSection::draw()
{
    if (mLit.get() && mLitTimer.getElapsedTimeF32() > LIT_SECONDS)
    {
        clearLit();
    }
    LLPanel::draw();
}

void SLPanelShapeSection::clearLit()
{
    if (LLPanel* lit = dynamic_cast<LLPanel*>(mLit.get()))
    {
        lit->setBackgroundVisible(false);
    }
    mLit.markDead();
}

void SLPanelShapeSection::refill()
{
    clearLit();
    SLPanelShapeEditor* editor = SLPanelShapeEditor::findInstance();
    LLPanelEditWearable* owner = editor ? editor->getOwner() : nullptr;
    for (Group& group : mGroups)
    {
        bool any = false;
        if (owner)
        {
            any = owner->fillSubpartList((U8)group.mSubpart, group.mList);
        }
        else
        {
            group.mList->clearPanels();
        }
        if (group.mTab)
        {
            // A group the avatar's sex has no sliders in has no tab.
            group.mTab->setVisible(any);
        }
    }
    if (mAccordion)
    {
        mAccordion->arrange();
    }
}

void SLPanelShapeSection::updateValues()
{
    for (Group& group : mGroups)
    {
        group.mList->updatePanels(true);
    }
}

S32 SLPanelShapeSection::getExpandedSubpart() const
{
    for (const Group& group : mGroups)
    {
        if (group.mTab && group.mTab->getVisible() && group.mTab->getDisplayChildren())
        {
            return group.mSubpart;
        }
    }
    return -1;
}

void SLPanelShapeSection::onParamMoved(LLScrollingPanelParamBase* source)
{
    if (!source)
    {
        return;
    }
    // Another panel moved a parameter; ours for it, if any, follow. The
    // parameters are the truth, so this is only the sliders catching up.
    const S32 param_id = source->getParamId();
    for (Group& group : mGroups)
    {
        for (LLScrollingPanel* child : group.mList->getPanelList())
        {
            if (child == source)
            {
                continue;
            }
            LLScrollingPanelParamBase* panel = dynamic_cast<LLScrollingPanelParamBase*>(child);
            if (panel && panel->getParamId() == param_id)
            {
                panel->syncSlider();
            }
        }
    }
}

bool SLPanelShapeSection::showTarget(S32 subpart, S32 param_id)
{
    Group* target = nullptr;
    for (Group& group : mGroups)
    {
        if (group.mSubpart == subpart)
        {
            target = &group;
            break;
        }
    }
    if (!target || (target->mTab && !target->mTab->getVisible()))
    {
        return false;
    }

    // Only the target's own tab is opened. setDisplayChildren does not lay the
    // accordion out again, and the scroll below needs it laid out.
    if (target->mTab && !target->mTab->getDisplayChildren())
    {
        target->mTab->setDisplayChildren(true);
        if (mAccordion)
        {
            mAccordion->arrange();
        }
    }

    clearLit();

    LLScrollingPanelParamBase* found = nullptr;
    if (param_id >= 0)
    {
        for (LLScrollingPanel* child : target->mList->getPanelList())
        {
            LLScrollingPanelParamBase* panel = dynamic_cast<LLScrollingPanelParamBase*>(child);
            if (panel && panel->getParamId() == param_id)
            {
                found = panel;
                break;
            }
        }
    }

    if (found)
    {
        if (LLUICtrl* slider = found->getSlider())
        {
            // Scrolled to the row, not to the group: a tall group scrolled to
            // shows its bottom, which is not where this row need be.
            slider->onUpdateScrollToChild(slider);
            slider->setFocus(true);
        }
        found->setBackgroundVisible(true);
        found->setTransparentColor(LLUIColorTable::instance().getColor("EmphasisColor_13"));
        mLit = static_cast<LLView*>(found)->getHandle();
        mLitTimer.reset();
    }
    else if (target->mTab)
    {
        target->mTab->showAndFocusHeader();
    }
    return true;
}

// ============================================================================
// SLShapeEditorSlot
// ============================================================================

SLShapeEditorSlot::SLShapeEditorSlot(S32 index, bool in_pane)
:   LLPanel(),
    mIndex(index),
    mInPane(in_pane)
{
}

SLShapeEditorSlot::~SLShapeEditorSlot()
{
}

bool SLShapeEditorSlot::build()
{
    return buildFromFile("panel_shape_editor_slot.xml");
}

bool SLShapeEditorSlot::postBuild()
{
    mSectionCombo = getChild<LLComboBox>("section_combo");
    mPopOutButton = getChild<LLButton>("pop_out");
    mBody = getChild<LLPanel>("slot_body");
    mPoppedNote = getChildView("popped_note");
    mPutBack = getChildView("put_back");

    mSectionCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSectionPicked(); });
    mPopOutButton->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPopOutClicked(); });
    if (LLButton* put_back = dynamic_cast<LLButton*>(mPutBack))
    {
        put_back->setCommitCallback([this](LLUICtrl*, const LLSD&)
        {
            if (SLPanelShapeEditor* editor = SLPanelShapeEditor::findInstance())
            {
                editor->putBack(mIndex);
            }
        });
    }

    if (mInPane)
    {
        // Out in a window of its own, the button brings it back.
        mPopOutButton->setImageOverlay("Conv_toolbar_arrow_sw");
        mPopOutButton->setToolTip(getString("put_back_tooltip"));
    }
    return LLPanel::postBuild();
}

void SLShapeEditorSlot::clearBody()
{
    if (mSection)
    {
        SLPanelShapeSection* content = mSection;
        mSection = nullptr;
        if (gFocusMgr.childHasKeyboardFocus(content))
        {
            gFocusMgr.setKeyboardFocus(nullptr);
        }
        mBody->removeChild(content);
        delete content;
    }
}

void SLShapeEditorSlot::rememberSection(const std::string& id)
{
    const SLShapeSectionDef* def = SLShapeSections::find(id);
    mSectionId = def ? def->mId : SECTIONS.front().mId;
    if (mSectionCombo)
    {
        mSectionCombo->selectByValue(mSectionId);
    }
}

void SLShapeEditorSlot::setSection(const std::string& id, bool force)
{
    const SLShapeSectionDef* def = SLShapeSections::find(id);
    if (!def)
    {
        def = &SECTIONS.front();
    }
    if (!mBody)
    {
        return;
    }
    if (def->mId == mSectionId && !force && (mSection || mPoppedOut))
    {
        return;
    }
    rememberSection(def->mId);

    clearBody();
    if (mPoppedOut)
    {
        return;
    }

    SLPanelShapeSection* section = new SLPanelShapeSection(mIndex, *def);
    section->build();
    mSection = section;

    // The body's size, following it. setShape, not setRect + reshape: reshape
    // only lays children out by the change in size, so the section has to be
    // built at its XUI size and then reshaped to the body.
    section->setFollowsAll();
    mBody->addChild(section);
    section->setShape(mBody->getLocalRect());

    if (SLPanelShapeEditor* editor = SLPanelShapeEditor::findInstance())
    {
        editor->fillSection(section);
    }
}

void SLShapeEditorSlot::setPoppedOut(bool popped)
{
    if (mInPane || popped == mPoppedOut)
    {
        return;
    }
    mPoppedOut = popped;
    if (mBody)
    {
        mBody->setVisible(!popped);
    }
    if (mPoppedNote)
    {
        mPoppedNote->setVisible(popped);
    }
    if (mPutBack)
    {
        mPutBack->setVisible(popped);
    }
    // The window has its own dropdown while it is out.
    if (mSectionCombo)
    {
        mSectionCombo->setEnabled(!popped);
    }
    if (mPopOutButton)
    {
        mPopOutButton->setEnabled(!popped);
    }

    if (popped)
    {
        clearBody();
    }
    else
    {
        setSection(mSectionId, true);
    }
}

std::string SLShapeEditorSlot::getSectionLabel() const
{
    return mSectionCombo ? mSectionCombo->getSelectedItemLabel() : mSectionId;
}

void SLShapeEditorSlot::focusSectionCombo()
{
    if (mSectionCombo && mSectionCombo->getEnabled())
    {
        mSectionCombo->setFocus(true);
    }
}

bool SLShapeEditorSlot::hasKeyboardFocusInside() const
{
    return gFocusMgr.childHasKeyboardFocus(this);
}

void SLShapeEditorSlot::onSectionPicked()
{
    if (!mSectionCombo)
    {
        return;
    }
    const std::string id = mSectionCombo->getValue().asString();
    setSection(id);
    if (SLPanelShapeEditor* editor = SLPanelShapeEditor::findInstance())
    {
        editor->onSlotSectionChanged(mIndex, mSectionId);
    }
}

void SLShapeEditorSlot::onPopOutClicked()
{
    SLPanelShapeEditor* editor = SLPanelShapeEditor::findInstance();
    if (!editor)
    {
        return;
    }
    if (mInPane)
    {
        editor->putBack(mIndex);
    }
    else
    {
        editor->popOut(mIndex);
    }
}

// ============================================================================
// SLPanelShapeEditor
// ============================================================================

SLPanelShapeEditor::SLPanelShapeEditor(LLPanelEditWearable* owner)
:   LLPanel(),
    mOwner(owner)
{
    sInstance = this;
    for (S32 i = 0; i < MAX_SLOTS; ++i)
    {
        mSlotSections[i] = DEFAULT_SECTIONS[i];
    }
}

SLPanelShapeEditor::~SLPanelShapeEditor()
{
    if (sInstance == this)
    {
        sInstance = nullptr;
    }
}

// static
SLPanelShapeEditor* SLPanelShapeEditor::findInstance()
{
    return sInstance;
}

bool SLPanelShapeEditor::build(LLPanel* home)
{
    if (!home)
    {
        return false;
    }
    mHome = static_cast<LLView*>(home)->getHandle();
    if (!buildFromFile("panel_shape_editor.xml"))
    {
        return false;
    }
    setFollowsAll();
    home->addChild(this);
    // setShape, so the panel lays its children out from its XUI size.
    setShape(home->getLocalRect());
    return true;
}

bool SLPanelShapeEditor::postBuild()
{
    mSlotsArea = getChild<LLPanel>("slots_area");
    mLayoutCombo = getChild<LLComboBox>("layout_combo");
    mFindEditor = getChild<LLLineEditor>("find_editor");
    mFindStatus = getChild<LLTextBox>("find_status");
    mFloatButton = getChild<LLButton>("float_button");

    mLayoutCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onLayoutPicked(); });
    mFindEditor->setCommitCallback([this](LLUICtrl*, const LLSD&) { onFind(); });
    mFloatButton->setCommitCallback([this](LLUICtrl*, const LLSD&) { setFloated(!mFloated); });

    loadState();

    for (S32 i = 0; i < MAX_SLOTS; ++i)
    {
        SLShapeEditorSlot* slot = new SLShapeEditorSlot(i, false);
        slot->build();
        slot->setFollows(FOLLOWS_NONE);
        mSlotsArea->addChild(slot);
        mSlots[i] = slot;
        // Built when the Shape page is shown (setActive), not before.
        slot->rememberSection(mSlotSections[i]);
        if (mSlotPoppedOut[i])
        {
            slot->setPoppedOut(true);
        }
    }
    applyLayout();
    refreshTopBar();
    return LLPanel::postBuild();
}

void SLPanelShapeEditor::detachOwner(LLPanelEditWearable* owner)
{
    if (mOwner == owner)
    {
        mOwner = nullptr;
    }
}

void SLPanelShapeEditor::draw()
{
    // The panel that has the keyboard is the one whose region the camera frames.
    if (mActive && gFocusMgr.getKeyboardFocus())
    {
        const S32 focused = findFocusedSlot();
        if (focused >= 0 && focused != mLastFocusedSlot)
        {
            mLastFocusedSlot = focused;
            frameSlot(focused, true);
        }
    }
    LLPanel::draw();
}

void SLPanelShapeEditor::reshape(S32 width, S32 height, bool called_from_parent)
{
    LLPanel::reshape(width, height, called_from_parent);
    arrangeSlots();
}

bool SLPanelShapeEditor::handleKeyHere(KEY key, MASK mask)
{
    if (handleEditorKey(key, mask))
    {
        return true;
    }
    return LLPanel::handleKeyHere(key, mask);
}

bool SLPanelShapeEditor::handleEditorKey(KEY key, MASK mask)
{
    if (SLKeyCommands::instance().matches("find_in_floater", key, mask) && mFindEditor)
    {
        if (mFloated)
        {
            if (LLFloater* window = dynamic_cast<LLFloater*>(mWindow.get()))
            {
                if (window->isMinimized())
                {
                    window->setMinimized(false);
                }
                window->setFrontmost(true);
            }
        }
        mFindEditor->setFocus(true);
        mFindEditor->selectAll();
        return true;
    }
    if (key >= '1' && key <= '4' && mask == MASK_CONTROL)
    {
        const S32 index = key - '1';
        if (index >= effectiveLayout())
        {
            return true;
        }
        if (mSlotPoppedOut[index])
        {
            if (SLFloaterShapePane* pane =
                    LLFloaterReg::findTypedInstance<SLFloaterShapePane>(PANE_FLOATER_NAME, LLSD(index)))
            {
                pane->setFrontmost(true);
            }
        }
        if (SLShapeEditorSlot* slot = slotWidget(index))
        {
            slot->focusSectionCombo();
        }
        mLastFocusedSlot = index;
        return true;
    }
    return false;
}

// --- Wearable side ----------------------------------------------------------

void SLPanelShapeEditor::setActive(bool active)
{
    if (active == mActive)
    {
        return;
    }
    mActive = active;

    if (active)
    {
        // The panels are built here and filled by the caller (refillAll), once
        // the wearable's page is set up.
        mDeferFill = true;
        if (gSavedSettings.getBOOL("SLShapeEditorFloated") && !mFloated)
        {
            setFloated(true);
        }
        applyLayout();
        mDeferFill = false;
    }
    else
    {
        // The windows go with the page, and are remembered as they are: closing
        // them here must not read as the user putting anything back.
        mClosing = true;
        if (mFloated)
        {
            if (LLFloater* window = dynamic_cast<LLFloater*>(mWindow.get()))
            {
                window->closeFloater();
            }
            if (mFloated)
            {
                dockToHome();
            }
        }
        applyLayout();
        mClosing = false;
        mLastFocusedSlot = -1;
    }
    refreshTopBar();
}

template <typename Fn>
void SLPanelShapeEditor::forEachSection(Fn fn)
{
    for (S32 i = 0; i < MAX_SLOTS; ++i)
    {
        SLShapeEditorSlot* slot = slotWidget(i);
        if (slot && slot->getSectionPanel())
        {
            fn(slot->getSectionPanel());
        }
    }
}

void SLPanelShapeEditor::refillAll()
{
    forEachSection([](SLPanelShapeSection* section) { section->refill(); });
}

void SLPanelShapeEditor::updateValues()
{
    forEachSection([](SLPanelShapeSection* section) { section->updateValues(); });
}

void SLPanelShapeEditor::fillSection(SLPanelShapeSection* section)
{
    if (mDeferFill || !section)
    {
        return;
    }
    section->refill();
    section->updateValues();
}

void SLPanelShapeEditor::frameFocusedSection()
{
    const S32 index = (mLastFocusedSlot >= 0 && mLastFocusedSlot < effectiveLayout()) ? mLastFocusedSlot : 0;
    frameSlot(index, false);
}

void SLPanelShapeEditor::frameSlot(S32 index, bool only_when_customizing)
{
    if (!mOwner || !mOwner->getWearable())
    {
        return;
    }
    if (only_when_customizing && !gAgentCamera.cameraCustomizeAvatar())
    {
        return;
    }
    SLShapeEditorSlot* slot = slotWidget(index);
    if (!slot)
    {
        return;
    }
    const SLShapeSectionDef* def = SLShapeSections::find(slot->getSectionId());
    S32 subpart = def ? def->mSubpart : -1;
    if (subpart < 0)
    {
        // Everything in an accordion: the tab that is open.
        SLPanelShapeSection* section = slot->getSectionPanel();
        subpart = section ? section->getExpandedSubpart() : -1;
        if (subpart < 0)
        {
            subpart = 0;
        }
    }
    mOwner->changeCamera((U8)subpart);
}

void SLPanelShapeEditor::onSectionTabExpanded(S32 slot_index, S32 subpart)
{
    if (!mActive)
    {
        return;
    }
    mLastFocusedSlot = slot_index;
    if (mOwner && mOwner->getWearable() && gAgentCamera.cameraCustomizeAvatar())
    {
        mOwner->changeCamera((U8)subpart);
    }
}

// --- Panels -----------------------------------------------------------------

SLShapeEditorSlot* SLPanelShapeEditor::slotWidget(S32 index) const
{
    if (index < 0 || index >= MAX_SLOTS)
    {
        return nullptr;
    }
    if (mSlotPoppedOut[index])
    {
        SLFloaterShapePane* pane = LLFloaterReg::findTypedInstance<SLFloaterShapePane>(PANE_FLOATER_NAME, LLSD(index));
        return pane ? pane->getSlot() : nullptr;
    }
    return mSlots[index];
}

S32 SLPanelShapeEditor::findFocusedSlot() const
{
    for (S32 i = 0; i < effectiveLayout() && i < MAX_SLOTS; ++i)
    {
        SLShapeEditorSlot* slot = slotWidget(i);
        if (slot && slot->hasKeyboardFocusInside())
        {
            return i;
        }
    }
    return -1;
}

void SLPanelShapeEditor::onLayoutPicked()
{
    const S32 picked = mLayoutCombo->getValue().asInteger();
    mLayout = (picked == 2 || picked == 4) ? picked : 1;
    saveState();
    if (!mFloated && mLayout > 1)
    {
        // The sidebar has room for one panel. More than that is a window's
        // worth: take the editor out (which applies the layout).
        setFloated(true);
    }
    else
    {
        applyLayout();
    }
    refreshTopBar();
}

void SLPanelShapeEditor::applyLayout()
{
    const S32 layout = effectiveLayout();
    for (S32 i = 0; i < MAX_SLOTS; ++i)
    {
        SLShapeEditorSlot* slot = mSlots[i];
        if (!slot)
        {
            continue;
        }
        const bool shown = i < layout;
        slot->setVisible(shown);

        LLFloater* pane = LLFloaterReg::findInstance(PANE_FLOATER_NAME, LLSD(i));
        if (shown && mActive)
        {
            if (mSlotPoppedOut[i])
            {
                if (!slot->isPoppedOut())
                {
                    slot->setPoppedOut(true);
                }
                if (!pane)
                {
                    LLFloaterReg::showInstance(PANE_FLOATER_NAME, LLSD(i), false);
                }
            }
            else if (slot->isPoppedOut())
            {
                slot->setPoppedOut(false);
            }
            else if (!slot->hasContent())
            {
                // Coming into view: built now rather than kept alive hidden.
                slot->setSection(mSlotSections[i], true);
            }
        }
        else
        {
            // Not shown, or the page is not: its window goes (and comes back
            // with the layout or the page) and its sliders are not kept.
            if (pane)
            {
                const bool was_closing = mClosing;
                mClosing = true;
                pane->closeFloater();
                mClosing = was_closing;
            }
            slot->releaseContent();
        }
    }

    if (SLFloaterShapeEditor* window = dynamic_cast<SLFloaterShapeEditor*>(mWindow.get()))
    {
        window->applyMinimumSize();
    }
    arrangeSlots();
}

void SLPanelShapeEditor::getMinimumSize(S32& width, S32& height) const
{
    const S32 layout = effectiveLayout();
    const S32 cols = (layout == 1) ? 1 : 2;
    const S32 rows = (layout == MAX_SLOTS) ? 2 : 1;
    const S32 chrome_width = mSlotsArea ? getRect().getWidth() - mSlotsArea->getRect().getWidth() : 8;
    const S32 chrome_height = mSlotsArea ? getRect().getHeight() - mSlotsArea->getRect().getHeight() : 60;
    width = chrome_width + cols * MIN_SLOT_WIDTH + (cols - 1) * SLOT_GAP;
    height = chrome_height + rows * MIN_SLOT_HEIGHT + (rows - 1) * SLOT_GAP;
}

void SLPanelShapeEditor::arrangeSlots()
{
    if (!mSlotsArea)
    {
        return;
    }
    const S32 layout = effectiveLayout();
    const S32 width = mSlotsArea->getRect().getWidth();
    const S32 height = mSlotsArea->getRect().getHeight();
    const S32 half_w = (width - SLOT_GAP) / 2;
    const S32 half_h = (height - SLOT_GAP) / 2;

    std::array<LLRect, MAX_SLOTS> rects;
    switch (layout)
    {
    case 1:
        rects[0] = LLRect(0, height, width, 0);
        break;
    case 2:
        rects[0] = LLRect(0, height, half_w, 0);
        rects[1] = LLRect(width - half_w, height, width, 0);
        break;
    default:
        // Reading order: 1 2 over 3 4.
        rects[0] = LLRect(0, height, half_w, height - half_h);
        rects[1] = LLRect(width - half_w, height, width, height - half_h);
        rects[2] = LLRect(0, half_h, half_w, 0);
        rects[3] = LLRect(width - half_w, half_h, width, 0);
        break;
    }
    for (S32 i = 0; i < layout && i < MAX_SLOTS; ++i)
    {
        if (mSlots[i] && mSlots[i]->getRect() != rects[i])
        {
            mSlots[i]->setShape(rects[i]);
        }
    }
}

void SLPanelShapeEditor::refreshTopBar()
{
    if (mLayoutCombo)
    {
        mLayoutCombo->selectByValue(LLSD(effectiveLayout()));
    }
    if (mFloatButton)
    {
        mFloatButton->setImageOverlay(mFloated ? "Conv_toolbar_arrow_sw" : "Conv_toolbar_arrow_ne");
        mFloatButton->setToolTip(getString(mFloated ? "dock_tooltip" : "float_tooltip"));
    }
}

void SLPanelShapeEditor::onSlotSectionChanged(S32 index, const std::string& id)
{
    if (index < 0 || index >= MAX_SLOTS)
    {
        return;
    }
    mSlotSections[index] = id;
    if (SLFloaterShapePane* pane = LLFloaterReg::findTypedInstance<SLFloaterShapePane>(PANE_FLOATER_NAME, LLSD(index)))
    {
        pane->refreshTitle();
    }
    // The main panel keeps the section too, so putting the window back shows
    // what was last chosen in it.
    if (mSlotPoppedOut[index] && mSlots[index])
    {
        mSlots[index]->rememberSection(id);
    }
    saveState();

    // A panel just given a section is the one in use: frame its region.
    mLastFocusedSlot = index;
    frameSlot(index, true);
}

const std::string& SLPanelShapeEditor::getSlotSection(S32 index) const
{
    return (index >= 0 && index < MAX_SLOTS) ? mSlotSections[index] : LLStringUtil::null;
}

void SLPanelShapeEditor::popOut(S32 index)
{
    if (index < 0 || index >= effectiveLayout() || mSlotPoppedOut[index] || !mSlots[index])
    {
        return;
    }
    mSlotPoppedOut[index] = true;
    mSlots[index]->setPoppedOut(true);
    LLFloaterReg::showInstance(PANE_FLOATER_NAME, LLSD(index), true);
    saveState();
}

void SLPanelShapeEditor::putBack(S32 index)
{
    if (index < 0 || index >= MAX_SLOTS)
    {
        return;
    }
    if (LLFloater* pane = LLFloaterReg::findInstance(PANE_FLOATER_NAME, LLSD(index)))
    {
        // Its onClose comes back through onPaneClosed with its section.
        pane->closeFloater();
        return;
    }
    onPaneClosed(index, mSlotSections[index]);
}

void SLPanelShapeEditor::onPaneClosed(S32 index, const std::string& id)
{
    if (mClosing || index < 0 || index >= MAX_SLOTS)
    {
        return;
    }
    if (!id.empty())
    {
        mSlotSections[index] = id;
    }
    mSlotPoppedOut[index] = false;
    if (mSlots[index])
    {
        mSlots[index]->rememberSection(mSlotSections[index]);
        mSlots[index]->setPoppedOut(false);
    }
    saveState();
}

// --- The editor's window ----------------------------------------------------

void SLPanelShapeEditor::setHomeNote(bool visible)
{
    if (LLView* home = mHome.get())
    {
        if (LLView* note = home->findChildView("floated_note"))
        {
            note->setVisible(visible);
        }
        if (LLView* dock = home->findChildView("floated_dock"))
        {
            dock->setVisible(visible);
        }
    }
}

void SLPanelShapeEditor::setFloated(bool floated)
{
    if (!mClosing)
    {
        gSavedSettings.setBOOL("SLShapeEditorFloated", floated);
    }
    if (!mActive)
    {
        // Out when the page is next shown.
        return;
    }
    if (floated)
    {
        if (!mFloated)
        {
            LLFloaterReg::showInstance(EDITOR_FLOATER_NAME, LLSD(), true);
        }
    }
    else if (mFloated)
    {
        if (LLFloater* window = dynamic_cast<LLFloater*>(mWindow.get()))
        {
            // Its onClose comes back through onFloaterClosed.
            window->closeFloater();
        }
        if (mFloated)
        {
            dockToHome();
        }
    }
}

void SLPanelShapeEditor::attachTo(LLPanel* parent, SLFloaterShapeEditor* window)
{
    if (!parent || !window)
    {
        return;
    }
    if (getParent())
    {
        getParent()->removeChild(this);
    }
    parent->addChild(this);
    setFollowsAll();
    setShape(parent->getLocalRect());
    mFloated = true;
    mWindow = static_cast<LLView*>(window)->getHandle();
    setHomeNote(true);
    applyLayout();
    refreshTopBar();
}

void SLPanelShapeEditor::dockToHome()
{
    mFloated = false;
    mWindow.markDead();
    if (LLView* home = mHome.get())
    {
        if (getParent() != home)
        {
            if (getParent())
            {
                getParent()->removeChild(this);
            }
            home->addChild(this);
        }
        setShape(home->getLocalRect());
    }
    setHomeNote(false);
    applyLayout();
    refreshTopBar();
}

void SLPanelShapeEditor::onFloaterClosed()
{
    if (!mFloated)
    {
        return;
    }
    if (!mClosing)
    {
        // The user closed the window: the editor stays in the sidebar from now on.
        gSavedSettings.setBOOL("SLShapeEditorFloated", false);
    }
    dockToHome();
}

// --- State ------------------------------------------------------------------

void SLPanelShapeEditor::loadState()
{
    const S32 layout = gSavedSettings.getS32("SLShapeEditorLayout");
    mLayout = (layout == 2 || layout == 4) ? layout : 1;

    const LLSD panels = gSavedSettings.getLLSD("SLShapeEditorPanels");
    if (panels.isArray())
    {
        for (S32 i = 0; i < MAX_SLOTS && i < (S32)panels.size(); ++i)
        {
            const LLSD& entry = panels[i];
            const std::string id = entry["section"].asString();
            if (SLShapeSections::find(id))
            {
                mSlotSections[i] = id;
            }
            mSlotPoppedOut[i] = entry["popped"].asBoolean();
        }
    }
}

void SLPanelShapeEditor::saveState()
{
    LLSD panels = LLSD::emptyArray();
    for (S32 i = 0; i < MAX_SLOTS; ++i)
    {
        LLSD entry;
        entry["section"] = mSlotSections[i];
        entry["popped"] = mSlotPoppedOut[i];
        panels.append(entry);
    }
    gSavedSettings.setLLSD("SLShapeEditorPanels", panels);
    gSavedSettings.setS32("SLShapeEditorLayout", mLayout);
}

// --- Find -------------------------------------------------------------------

void SLPanelShapeEditor::onFind()
{
    if (mFindEditor)
    {
        runFind(mFindEditor->getText());
    }
}

SLShapeEditorSlot* SLPanelShapeEditor::findTargetSlot()
{
    const S32 layout = effectiveLayout();
    if (mLastFocusedSlot >= 0 && mLastFocusedSlot < layout)
    {
        if (SLShapeEditorSlot* focused = slotWidget(mLastFocusedSlot))
        {
            return focused;
        }
    }
    // Else the first one shown here, then the first anywhere.
    for (S32 i = 0; i < layout; ++i)
    {
        if (!mSlotPoppedOut[i] && mSlots[i])
        {
            return mSlots[i];
        }
    }
    return slotWidget(0);
}

void SLPanelShapeEditor::runFind(const std::string& raw_text)
{
    std::string text = raw_text;
    LLStringUtil::trim(text);
    LLStringUtil::toLower(text);
    if (text.empty())
    {
        mFindStatus->setText(LLStringUtil::null);
        mLastFindText.clear();
        return;
    }

    // Groups as well as sliders: "legs" should reach the Legs group even
    // though none of its sliders says legs.
    struct Match
    {
        S32 mScore;
        S32 mSubpart;
        S32 mParamId;
        std::string mLabel;
    };
    std::vector<Match> matches;
    for (const SLShapeSectionDef& def : SLShapeSections::all())
    {
        if (def.mSubpart < 0)
        {
            continue;
        }
        const std::string title = getString(std::string("section_") + def.mId);
        std::string title_lower = title;
        LLStringUtil::toLower(title_lower);
        const S32 group_score = matchScore(title_lower, text);
        if (group_score >= 0)
        {
            matches.push_back({ group_score, def.mSubpart, -1, title });
        }

        std::vector<std::pair<S32, std::string>> params;
        if (mOwner)
        {
            mOwner->getSubpartParams((U8)def.mSubpart, params);
        }
        for (const auto& param : params)
        {
            std::string name = param.second;
            LLStringUtil::toLower(name);
            const S32 score = matchScore(name, text);
            if (score >= 0)
            {
                matches.push_back({ score, def.mSubpart, param.first, param.second + " (" + title + ")" });
            }
        }
    }
    std::stable_sort(matches.begin(), matches.end(),
                     [](const Match& a, const Match& b) { return a.mScore < b.mScore; });

    if (matches.empty())
    {
        LLStringUtil::format_map_t args;
        args["[TEXT]"] = raw_text;
        mFindStatus->setText(getString("find_none", args));
        mLastFindText.clear();
        return;
    }

    // Enter again on the same text is the next match.
    if (text == mLastFindText)
    {
        mLastFindIndex = (mLastFindIndex + 1) % matches.size();
    }
    else
    {
        mLastFindText = text;
        mLastFindIndex = 0;
    }
    const Match& match = matches[mLastFindIndex];

    LLStringUtil::format_map_t args;
    args["[INDEX]"] = llformat("%d", (S32)mLastFindIndex + 1);
    args["[COUNT]"] = llformat("%d", (S32)matches.size());
    args["[WHAT]"] = match.mLabel;
    mFindStatus->setText(getString("find_result", args));

    SLShapeEditorSlot* slot = findTargetSlot();
    if (!slot)
    {
        return;
    }

    // The panel keeps its section when that already shows the target (everything
    // does); else it switches to the group.
    const SLShapeSectionDef* current = SLShapeSections::find(slot->getSectionId());
    if (!current || (!SLShapeSections::isAll(current) && current->mSubpart != match.mSubpart))
    {
        if (const SLShapeSectionDef* def = SLShapeSections::forSubpart(match.mSubpart))
        {
            slot->setSection(def->mId);
            onSlotSectionChanged(slot->getIndex(), def->mId);
        }
    }

    if (LLFloater* window = slot->getParentByType<LLFloater>())
    {
        if (window->isMinimized())
        {
            window->setMinimized(false);
        }
        window->setFrontmost(false);
    }
    if (SLPanelShapeSection* section = slot->getSectionPanel())
    {
        section->showTarget(match.mSubpart, match.mParamId);
    }
}
