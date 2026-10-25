/**
 * @file llpanelobject.cpp
 * @brief Object editing (position, scale, etc.) in the tools floater
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

#include "llviewerprecompiledheaders.h"

// file include
#include "llpanelobject.h"

#include <charconv>
#include <cmath>

// linden library includes
#include "llerror.h"
#include "llfontgl.h"
#include "llpermissionsflags.h"
#include "llnotificationsutil.h"
#include "llstring.h"
#include "llvolume.h"
#include "m3math.h"

// project includes
#include "llagent.h"
#include "llbutton.h"
#include "llcalc.h"
#include "llcheckboxctrl.h"
#include "llcleaninstancestore.h"
#include "llcleanmeshasset.h"
#include "llcolorswatch.h"
#include "llcombobox.h"
#include "llfocusmgr.h"
#include "llmanipscale.h"
#include "llmenubutton.h"
#include "llmeshrepository.h"
#include "llpreviewscript.h"
#include "llresmgr.h"
#include "llselectmgr.h"
#include "llspinctrl.h"
#include "lltexturectrl.h"
#include "lltextbox.h"
#include "lltool.h"
#include "lltoolcomp.h"
#include "lltoolmgr.h"
#include "llui.h"
#include "lluicolortable.h"
#include "llviewercamera.h"
#include "llviewerobject.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "llvovolume.h"
#include "llworld.h"
#include "pipeline.h"
#include "llviewercontrol.h"
#include "lluictrlfactory.h"
//#include "llfirstuse.h"
// [RLVa:KB] - Checked: 2011-05-22 (RLVa-1.3.1a)
#include "rlvhandler.h"
#include "llvoavatarself.h"
// [/RLVa:KB]

#include "lldrawpool.h"

//
// Constants
//
enum {
    MI_BOX,
    MI_CYLINDER,
    MI_PRISM,
    MI_SPHERE,
    MI_TORUS,
    MI_TUBE,
    MI_RING,
    MI_SCULPT,
    MI_NONE,
    MI_VOLUME_COUNT
};

enum {
    MI_HOLE_SAME,
    MI_HOLE_CIRCLE,
    MI_HOLE_SQUARE,
    MI_HOLE_TRIANGLE,
    MI_HOLE_COUNT
};

const F32 MAX_ATTACHMENT_DIST = 3.5f; // meters

//static const std::string LEGACY_FULLBRIGHT_DESC =LLTrans::getString("Fullbright");

bool    LLPanelObject::postBuild()
{
    setMouseOpaque(false);

    //--------------------------------------------------------
    // Top
    //--------------------------------------------------------

    // Lock checkbox
    mCheckLock = getChild<LLCheckBoxCtrl>("checkbox locked");
    childSetCommitCallback("checkbox locked",onCommitLock,this);

    // Physical checkbox
    mCheckPhysics = getChild<LLCheckBoxCtrl>("Physical Checkbox Ctrl");
    childSetCommitCallback("Physical Checkbox Ctrl",onCommitPhysics,this);

    // Temporary checkbox
    mCheckTemporary = getChild<LLCheckBoxCtrl>("Temporary Checkbox Ctrl");
    childSetCommitCallback("Temporary Checkbox Ctrl",onCommitTemporary,this);

    // Phantom checkbox
    mCheckPhantom = getChild<LLCheckBoxCtrl>("Phantom Checkbox Ctrl");
    childSetCommitCallback("Phantom Checkbox Ctrl",onCommitPhantom,this);

    // Copy all / Paste all (Position + Size + Rotation)
    mBtnCopyAll = getChild<LLButton>("psr_copy_btn");
    mBtnPasteAll = getChild<LLButton>("psr_paste_btn");

    // Position
    mBtnCopyPos = getChild<LLButton>("pos_copy_btn");
    mBtnPastePos = getChild<LLButton>("pos_paste_btn");
    mLabelPosition = getChild<LLTextBox>("label position");
    mCtrlPosX = getChild<LLSpinCtrl>("Pos X");
    childSetCommitCallback("Pos X",onCommitPosition,this);
    mCtrlPosY = getChild<LLSpinCtrl>("Pos Y");
    childSetCommitCallback("Pos Y",onCommitPosition,this);
    mCtrlPosZ = getChild<LLSpinCtrl>("Pos Z");
    childSetCommitCallback("Pos Z",onCommitPosition,this);

    // Scale
    mBtnCopySize = getChild<LLButton>("size_copy_btn");
    mBtnPasteSize = getChild<LLButton>("size_paste_btn");
    mLabelSize = getChild<LLTextBox>("label size");
    mCtrlScaleX = getChild<LLSpinCtrl>("Scale X");
    childSetCommitCallback("Scale X",onCommitScale,this);

    // Scale Y
    mCtrlScaleY = getChild<LLSpinCtrl>("Scale Y");
    childSetCommitCallback("Scale Y",onCommitScale,this);

    // Scale Z
    mCtrlScaleZ = getChild<LLSpinCtrl>("Scale Z");
    childSetCommitCallback("Scale Z",onCommitScale,this);

    // Rotation
    mBtnCopyRot = getChild<LLButton>("rot_copy_btn");
    mBtnPasteRot = getChild<LLButton>("rot_paste_btn");
    mLabelRotation = getChild<LLTextBox>("label rotation");
    mCtrlRotX = getChild<LLSpinCtrl>("Rot X");
    childSetCommitCallback("Rot X",onCommitRotation,this);
    mCtrlRotY = getChild<LLSpinCtrl>("Rot Y");
    childSetCommitCallback("Rot Y",onCommitRotation,this);
    mCtrlRotZ = getChild<LLSpinCtrl>("Rot Z");
    childSetCommitCallback("Rot Z",onCommitRotation,this);

    //--------------------------------------------------------

    // Base Type
    mComboBaseType = getChild<LLComboBox>("comboBaseType");
    childSetCommitCallback("comboBaseType",onCommitParametric,this);

    mMenuClipboardParams = getChild<LLMenuButton>("clipboard_obj_params_btn");

    // Cut
    mLabelCut = getChild<LLTextBox>("text cut");
    mSpinCutBegin = getChild<LLSpinCtrl>("cut begin");
    childSetCommitCallback("cut begin",onCommitParametric,this);
    mSpinCutBegin->setValidateBeforeCommit( precommitValidate );
    mSpinCutEnd = getChild<LLSpinCtrl>("cut end");
    childSetCommitCallback("cut end",onCommitParametric,this);
    mSpinCutEnd->setValidateBeforeCommit( &precommitValidate );

    // Hollow / Skew
    mLabelHollow = getChild<LLTextBox>("text hollow");
    mLabelSkew = getChild<LLTextBox>("text skew");
    mSpinHollow = getChild<LLSpinCtrl>("Scale 1");
    childSetCommitCallback("Scale 1",onCommitParametric,this);
    mSpinHollow->setValidateBeforeCommit( &precommitValidate );
    mSpinSkew = getChild<LLSpinCtrl>("Skew");
    childSetCommitCallback("Skew",onCommitParametric,this);
    mSpinSkew->setValidateBeforeCommit( &precommitValidate );
    mLabelHoleType = getChild<LLTextBox>("Hollow Shape");

    // Hole Type
    mComboHoleType = getChild<LLComboBox>("hole");
    childSetCommitCallback("hole",onCommitParametric,this);

    // Twist
    mLabelTwist = getChild<LLTextBox>("text twist");
    mSpinTwistBegin = getChild<LLSpinCtrl>("Twist Begin");
    childSetCommitCallback("Twist Begin",onCommitParametric,this);
    mSpinTwistBegin->setValidateBeforeCommit( precommitValidate );
    mSpinTwist = getChild<LLSpinCtrl>("Twist End");
    childSetCommitCallback("Twist End",onCommitParametric,this);
    mSpinTwist->setValidateBeforeCommit( &precommitValidate );

    // Scale
    mSpinScaleX = getChild<LLSpinCtrl>("Taper Scale X");
    childSetCommitCallback("Taper Scale X",onCommitParametric,this);
    mSpinScaleX->setValidateBeforeCommit( &precommitValidate );
    mSpinScaleY = getChild<LLSpinCtrl>("Taper Scale Y");
    childSetCommitCallback("Taper Scale Y",onCommitParametric,this);
    mSpinScaleY->setValidateBeforeCommit( &precommitValidate );

    // Shear
    mLabelShear = getChild<LLTextBox>("text topshear");
    mSpinShearX = getChild<LLSpinCtrl>("Shear X");
    childSetCommitCallback("Shear X",onCommitParametric,this);
    mSpinShearX->setValidateBeforeCommit( &precommitValidate );
    mSpinShearY = getChild<LLSpinCtrl>("Shear Y");
    childSetCommitCallback("Shear Y",onCommitParametric,this);
    mSpinShearY->setValidateBeforeCommit( &precommitValidate );

    // Path / Profile
    mCtrlPathBegin = getChild<LLSpinCtrl>("Path Limit Begin");
    childSetCommitCallback("Path Limit Begin",onCommitParametric,this);
    mCtrlPathBegin->setValidateBeforeCommit( &precommitValidate );
    mCtrlPathEnd = getChild<LLSpinCtrl>("Path Limit End");
    childSetCommitCallback("Path Limit End",onCommitParametric,this);
    mCtrlPathEnd->setValidateBeforeCommit( &precommitValidate );

    // Taper
    mLabelTaper = getChild<LLTextBox>("text taper2");
    mSpinTaperX = getChild<LLSpinCtrl>("Taper X");
    childSetCommitCallback("Taper X",onCommitParametric,this);
    mSpinTaperX->setValidateBeforeCommit( precommitValidate );
    mSpinTaperY = getChild<LLSpinCtrl>("Taper Y");
    childSetCommitCallback("Taper Y",onCommitParametric,this);
    mSpinTaperY->setValidateBeforeCommit( precommitValidate );

    // Radius Offset / Revolutions
    mLabelRadiusOffset = getChild<LLTextBox>("text radius delta");
    mLabelRevolutions = getChild<LLTextBox>("text revolutions");
    mSpinRadiusOffset = getChild<LLSpinCtrl>("Radius Offset");
    childSetCommitCallback("Radius Offset",onCommitParametric,this);
    mSpinRadiusOffset->setValidateBeforeCommit( &precommitValidate );
    mSpinRevolutions = getChild<LLSpinCtrl>("Revolutions");
    childSetCommitCallback("Revolutions",onCommitParametric,this);
    mSpinRevolutions->setValidateBeforeCommit( &precommitValidate );

    // Sculpt
    mCtrlSculptTexture = getChild<LLTextureCtrl>("sculpt texture control");
    if (mCtrlSculptTexture)
    {
        mCtrlSculptTexture->setDefaultImageAssetID(SCULPT_DEFAULT_TEXTURE);
        mCtrlSculptTexture->setCommitCallback( boost::bind(&LLPanelObject::onCommitSculpt, this, _2 ));
        mCtrlSculptTexture->setOnCancelCallback( boost::bind(&LLPanelObject::onCancelSculpt, this, _2 ));
        mCtrlSculptTexture->setOnSelectCallback( boost::bind(&LLPanelObject::onSelectSculpt, this, _2 ));
        mCtrlSculptTexture->setDropCallback( boost::bind(&LLPanelObject::onDropSculpt, this, _2 ));
        // Don't allow (no copy) or (no transfer) textures to be selected during immediate mode
        mCtrlSculptTexture->setImmediateFilterPermMask(PERM_COPY | PERM_TRANSFER);
        mCtrlSculptTexture->setDnDFilterPermMask(PERM_COPY | PERM_TRANSFER);
        LLAggregatePermissions texture_perms;
        if (LLSelectMgr::getInstance()->selectGetAggregateTexturePermissions(texture_perms))
        {
            bool can_copy =
                texture_perms.getValue(PERM_COPY) == LLAggregatePermissions::AP_EMPTY ||
                texture_perms.getValue(PERM_COPY) == LLAggregatePermissions::AP_ALL;
            bool can_transfer =
                texture_perms.getValue(PERM_TRANSFER) == LLAggregatePermissions::AP_EMPTY ||
                texture_perms.getValue(PERM_TRANSFER) == LLAggregatePermissions::AP_ALL;
            mCtrlSculptTexture->setCanApplyImmediately(can_copy && can_transfer);
        }
        else
        {
            mCtrlSculptTexture->setCanApplyImmediately(false);
        }
    }

    mLabelSculptType = getChild<LLTextBox>("label sculpt type");
    mCtrlSculptType = getChild<LLComboBox>("sculpt type control");
    childSetCommitCallback("sculpt type control", onCommitSculptType, this);
    mCtrlSculptMirror = getChild<LLCheckBoxCtrl>("sculpt mirror control");
    childSetCommitCallback("sculpt mirror control", onCommitSculptType, this);
    mCtrlSculptInvert = getChild<LLCheckBoxCtrl>("sculpt invert control");
    childSetCommitCallback("sculpt invert control", onCommitSculptType, this);

    mLodTitle = getChild<LLTextBox>("lod_info_title");
    mLodCols[0] = getChild<LLTextBox>("lod_info_lod");
    mLodCols[1] = getChild<LLTextBox>("lod_info_tris");
    mLodCols[2] = getChild<LLTextBox>("lod_info_verts");
    mLodCols[3] = getChild<LLTextBox>("lod_info_dist");
    mLodStatus = getChild<LLTextBox>("lod_info_status");

    // Start with everyone disabled
    clearCtrls();

    return true;
}

LLPanelObject::LLPanelObject()
:   LLPanel(),
    mIsPhysical(false),
    mIsTemporary(false),
    mIsPhantom(false),
    mSelectedType(MI_BOX),
    mSculptTextureRevert(LLUUID::null),
    mSculptTypeRevert(0),
    mHasClipboardPos(false),
    mHasClipboardSize(false),
    mHasClipboardRot(false),
    mSizeChanged(false)
{
    mCommitCallbackRegistrar.add("PanelObject.menuDoToSelected", boost::bind(&LLPanelObject::menuDoToSelected, this, _2));
    mEnableCallbackRegistrar.add("PanelObject.menuEnable", boost::bind(&LLPanelObject::menuEnableItem, this, _2));
}


LLPanelObject::~LLPanelObject()
{
    // Children all cleaned up by default view destructor.
}

void LLPanelObject::getState( )
{
    LLViewerObject* objectp = LLSelectMgr::getInstance()->getSelection()->getFirstRootObject();
    LLViewerObject* root_objectp = objectp;
    if(!objectp)
    {
        objectp = LLSelectMgr::getInstance()->getSelection()->getFirstObject();
        // *FIX: shouldn't we just keep the child?
        if (objectp)
        {
            LLViewerObject* parentp = objectp->getRootEdit();

            if (parentp)
            {
                root_objectp = parentp;
            }
            else
            {
                root_objectp = objectp;
            }
        }
    }

    LLCalc* calcp = LLCalc::getInstance();

    LLVOVolume *volobjp = NULL;
    if ( objectp && (objectp->getPCode() == LL_PCODE_VOLUME))
    {
        volobjp = (LLVOVolume *)objectp;
    }

    if( !objectp )
    {
        //forfeit focus
        if (gFocusMgr.childHasKeyboardFocus(this))
        {
            gFocusMgr.setKeyboardFocus(NULL);
        }

        // Disable all text input fields
        clearCtrls();
        calcp->clearAllVariables();
        return;
    }

    S32 selected_count = LLSelectMgr::getInstance()->getSelection()->getObjectCount();
    bool single_volume = (LLSelectMgr::getInstance()->selectionAllPCode( LL_PCODE_VOLUME ))
                         && (selected_count == 1);

    bool enable_move;
    bool enable_modify;

    LLSelectMgr::getInstance()->selectGetEditMoveLinksetPermissions(enable_move, enable_modify);

    bool enable_scale = enable_modify;
    bool enable_rotate = enable_move; // already accounts for a case of children, which needs permModify() as well

// [RLVa:KB] - Checked: 2010-03-31 (RLVa-1.2.0c) | Modified: RLVa-1.0.0g
    if ( (rlv_handler_t::isEnabled()) && ((gRlvHandler.hasBehaviour(RLV_BHVR_UNSIT)) || (gRlvHandler.hasBehaviour(RLV_BHVR_SITTP))) )
    {
        if ( (isAgentAvatarValid()) && (gAgentAvatarp->isSitting()) && (gAgentAvatarp->getRoot() == objectp->getRootEdit()) )
            enable_move = enable_scale = enable_rotate = false;
    }
// [/RLVa:KB]

    LLVector3 vec;
    if (enable_move)
    {
        vec = objectp->getPositionEdit();
        mCtrlPosX->set( vec.mV[VX] );
        mCtrlPosY->set( vec.mV[VY] );
        mCtrlPosZ->set( vec.mV[VZ] );
        calcp->setVar(LLCalc::X_POS, vec.mV[VX]);
        calcp->setVar(LLCalc::Y_POS, vec.mV[VY]);
        calcp->setVar(LLCalc::Z_POS, vec.mV[VZ]);
    }
    else
    {
        mCtrlPosX->clear();
        mCtrlPosY->clear();
        mCtrlPosZ->clear();
        calcp->clearVar(LLCalc::X_POS);
        calcp->clearVar(LLCalc::Y_POS);
        calcp->clearVar(LLCalc::Z_POS);
    }

    mBtnCopyPos->setEnabled(enable_move);
    mBtnPastePos->setEnabled(enable_move);
    mLabelPosition->setEnabled( enable_move );
    mCtrlPosX->setEnabled(enable_move);
    mCtrlPosY->setEnabled(enable_move);
    mCtrlPosZ->setEnabled(enable_move);

    if (enable_scale)
    {
        vec = objectp->getScale();
        mCtrlScaleX->set( vec.mV[VX] );
        mCtrlScaleY->set( vec.mV[VY] );
        mCtrlScaleZ->set( vec.mV[VZ] );
        calcp->setVar(LLCalc::X_SCALE, vec.mV[VX]);
        calcp->setVar(LLCalc::Y_SCALE, vec.mV[VY]);
        calcp->setVar(LLCalc::Z_SCALE, vec.mV[VZ]);
    }
    else
    {
        mCtrlScaleX->clear();
        mCtrlScaleY->clear();
        mCtrlScaleZ->clear();
        calcp->setVar(LLCalc::X_SCALE, 0.f);
        calcp->setVar(LLCalc::Y_SCALE, 0.f);
        calcp->setVar(LLCalc::Z_SCALE, 0.f);
    }

    mBtnCopySize->setEnabled(enable_scale);
    mBtnPasteSize->setEnabled(enable_scale);
    mLabelSize->setEnabled( enable_scale );
    mCtrlScaleX->setEnabled( enable_scale );
    mCtrlScaleY->setEnabled( enable_scale );
    mCtrlScaleZ->setEnabled( enable_scale );

    LLQuaternion object_rot = objectp->getRotationEdit();
    object_rot.getEulerAngles(&(mCurEulerDegrees.mV[VX]), &(mCurEulerDegrees.mV[VY]), &(mCurEulerDegrees.mV[VZ]));
    mCurEulerDegrees *= RAD_TO_DEG;
    mCurEulerDegrees.mV[VX] = fmod(ll_round(mCurEulerDegrees.mV[VX], OBJECT_ROTATION_PRECISION) + 360.f, 360.f);
    mCurEulerDegrees.mV[VY] = fmod(ll_round(mCurEulerDegrees.mV[VY], OBJECT_ROTATION_PRECISION) + 360.f, 360.f);
    mCurEulerDegrees.mV[VZ] = fmod(ll_round(mCurEulerDegrees.mV[VZ], OBJECT_ROTATION_PRECISION) + 360.f, 360.f);

    if (enable_rotate)
    {
        mCtrlRotX->set( mCurEulerDegrees.mV[VX] );
        mCtrlRotY->set( mCurEulerDegrees.mV[VY] );
        mCtrlRotZ->set( mCurEulerDegrees.mV[VZ] );
        calcp->setVar(LLCalc::X_ROT, mCurEulerDegrees.mV[VX]);
        calcp->setVar(LLCalc::Y_ROT, mCurEulerDegrees.mV[VY]);
        calcp->setVar(LLCalc::Z_ROT, mCurEulerDegrees.mV[VZ]);
    }
    else
    {
        mCtrlRotX->clear();
        mCtrlRotY->clear();
        mCtrlRotZ->clear();
        calcp->clearVar(LLCalc::X_ROT);
        calcp->clearVar(LLCalc::Y_ROT);
        calcp->clearVar(LLCalc::Z_ROT);
    }

    mBtnCopyRot->setEnabled(enable_rotate);
    mBtnPasteRot->setEnabled(enable_rotate);
    updateClipboardButtons();
    mLabelRotation->setEnabled( enable_rotate );
    mCtrlRotX->setEnabled( enable_rotate );
    mCtrlRotY->setEnabled( enable_rotate );
    mCtrlRotZ->setEnabled( enable_rotate );

    LLUUID owner_id;
    std::string owner_name;
    LLSelectMgr::getInstance()->selectGetOwner(owner_id, owner_name);

    // BUG? Check for all objects being editable?
    S32 roots_selected = LLSelectMgr::getInstance()->getSelection()->getRootObjectCount();
    bool editable = root_objectp->permModify();

    bool is_flexible = volobjp && volobjp->isFlexible();
    bool is_permanent = root_objectp->flagObjectPermanent();
    bool is_permanent_enforced = root_objectp->isPermanentEnforced();
    bool is_character = root_objectp->flagCharacter();
    llassert(!is_permanent || !is_character); // should never have a permanent object that is also a character

    // Lock checkbox - only modifiable if you own the object.
    bool self_owned = (gAgent.getID() == owner_id);
    mCheckLock->setEnabled( roots_selected > 0 && self_owned && !is_permanent_enforced);

    // More lock and debit checkbox - get the values
    bool valid;
    U32 owner_mask_on;
    U32 owner_mask_off;
    valid = LLSelectMgr::getInstance()->selectGetPerm(PERM_OWNER, &owner_mask_on, &owner_mask_off);

    if(valid)
    {
        if(owner_mask_on & PERM_MOVE)
        {
            // owner can move, so not locked
            mCheckLock->set(false);
            mCheckLock->setTentative(false);
        }
        else if(owner_mask_off & PERM_MOVE)
        {
            // owner can't move, so locked
            mCheckLock->set(true);
            mCheckLock->setTentative(false);
        }
        else
        {
            // some locked, some not locked
            mCheckLock->set(false);
            mCheckLock->setTentative(true);
        }
    }

    // Physics checkbox
    mIsPhysical = root_objectp->flagUsePhysics();
    llassert(!is_permanent || !mIsPhysical); // should never have a permanent object that is also physical

    mCheckPhysics->set( mIsPhysical );
    mCheckPhysics->setEnabled( roots_selected>0
                                && (editable || gAgent.isGodlike())
                                && !is_flexible && !is_permanent);

    mIsTemporary = root_objectp->flagTemporaryOnRez();
    llassert(!is_permanent || !mIsTemporary); // should never has a permanent object that is also temporary

    mCheckTemporary->set( mIsTemporary );
    mCheckTemporary->setEnabled( roots_selected>0 && editable && !is_permanent);

    mIsPhantom = root_objectp->flagPhantom();
    bool is_volume_detect = root_objectp->flagVolumeDetect();
    llassert(!is_character || !mIsPhantom); // should never have a character that is also a phantom
    mCheckPhantom->set( mIsPhantom );
    mCheckPhantom->setEnabled( roots_selected>0 && editable && !is_flexible && !is_permanent_enforced && !is_character && !is_volume_detect);

    //----------------------------------------------------------------------------

    S32 selected_item   = MI_BOX;
    S32 selected_hole   = MI_HOLE_SAME;
    bool enabled = false;
    bool hole_enabled = false;
    F32 scale_x=1.f, scale_y=1.f;
    bool isMesh = false;

    if( !objectp || !objectp->getVolume() || !editable || !single_volume)
    {
        // Clear out all geometry fields.
        mComboBaseType->clear();
        mSpinHollow->clear();
        mSpinCutBegin->clear();
        mSpinCutEnd->clear();
        mCtrlPathBegin->clear();
        mCtrlPathEnd->clear();
        mSpinScaleX->clear();
        mSpinScaleY->clear();
        mSpinTwist->clear();
        mSpinTwistBegin->clear();
        mComboHoleType->clear();
        mSpinShearX->clear();
        mSpinShearY->clear();
        mSpinTaperX->clear();
        mSpinTaperY->clear();
        mSpinRadiusOffset->clear();
        mSpinRevolutions->clear();
        mSpinSkew->clear();

        mSelectedType = MI_NONE;

        // A read-only or linkset mesh / sculpt gets the (disabled) sculpt layout, which
        // leaves the right column to the LOD info (#272) instead of empty box fields.
        if (objectp && objectp->getVolume() && objectp->getParameterEntryInUse(LLNetworkData::PARAMS_SCULPT))
        {
            selected_item = MI_SCULPT;
        }
    }
    else
    {
        // Only allowed to change these parameters for objects
        // that you have permissions on AND are not attachments.
        enabled = root_objectp->permModify() && !root_objectp->isPermanentEnforced();

        // Volume type
        const LLVolumeParams &volume_params = objectp->getVolume()->getParams();
        U8 path = volume_params.getPathParams().getCurveType();
        U8 profile_and_hole = volume_params.getProfileParams().getCurveType();
        U8 profile  = profile_and_hole & LL_PCODE_PROFILE_MASK;
        U8 hole     = profile_and_hole & LL_PCODE_HOLE_MASK;

        // Scale goes first so we can differentiate between a sphere and a torus,
        // which have the same profile and path types.

        // Scale
        scale_x = volume_params.getRatioX();
        scale_y = volume_params.getRatioY();

        bool linear_path = (path == LL_PCODE_PATH_LINE) || (path == LL_PCODE_PATH_FLEXIBLE);
        if ( linear_path && profile == LL_PCODE_PROFILE_CIRCLE )
        {
            selected_item = MI_CYLINDER;
        }
        else if ( linear_path && profile == LL_PCODE_PROFILE_SQUARE )
        {
            selected_item = MI_BOX;
        }
        else if ( linear_path && profile == LL_PCODE_PROFILE_ISOTRI )
        {
            selected_item = MI_PRISM;
        }
        else if ( linear_path && profile == LL_PCODE_PROFILE_EQUALTRI )
        {
            selected_item = MI_PRISM;
        }
        else if ( linear_path && profile == LL_PCODE_PROFILE_RIGHTTRI )
        {
            selected_item = MI_PRISM;
        }
        else if (path == LL_PCODE_PATH_FLEXIBLE) // shouldn't happen
        {
            selected_item = MI_CYLINDER; // reasonable default
        }
        else if ( path == LL_PCODE_PATH_CIRCLE && profile == LL_PCODE_PROFILE_CIRCLE && scale_y > 0.75f)
        {
            selected_item = MI_SPHERE;
        }
        else if ( path == LL_PCODE_PATH_CIRCLE && profile == LL_PCODE_PROFILE_CIRCLE && scale_y <= 0.75f)
        {
            selected_item = MI_TORUS;
        }
        else if ( path == LL_PCODE_PATH_CIRCLE && profile == LL_PCODE_PROFILE_CIRCLE_HALF)
        {
            selected_item = MI_SPHERE;
        }
        else if ( path == LL_PCODE_PATH_CIRCLE2 && profile == LL_PCODE_PROFILE_CIRCLE )
        {
            // Spirals aren't supported.  Make it into a sphere.  JC
            selected_item = MI_SPHERE;
        }
        else if ( path == LL_PCODE_PATH_CIRCLE && profile == LL_PCODE_PROFILE_EQUALTRI )
        {
            selected_item = MI_RING;
        }
        else if ( path == LL_PCODE_PATH_CIRCLE && profile == LL_PCODE_PROFILE_SQUARE && scale_y <= 0.75f)
        {
            selected_item = MI_TUBE;
        }
        else
        {
            LL_INFOS("FloaterTools") << "Unknown path " << (S32) path << " profile " << (S32) profile << " in getState" << LL_ENDL;
            selected_item = MI_BOX;
        }


        if (objectp->getParameterEntryInUse(LLNetworkData::PARAMS_SCULPT))
        {
            selected_item = MI_SCULPT;
            //LLFirstUse::useSculptedPrim();
        }


        mComboBaseType  ->setCurrentByIndex( selected_item );
        mSelectedType = selected_item;

        // Grab S path
        F32 begin_s = volume_params.getBeginS();
        F32 end_s   = volume_params.getEndS();

        // Compute cut and advanced cut from S and T
        F32 begin_t = volume_params.getBeginT();
        F32 end_t   = volume_params.getEndT();

        // Hollowness
        F32 hollow = 100.f * volume_params.getHollow();
        mSpinHollow->set( hollow );
        calcp->setVar(LLCalc::HOLLOW, hollow);
        // All hollow objects allow a shape to be selected.
        if (hollow > 0.f)
        {
            switch (hole)
            {
            case LL_PCODE_HOLE_CIRCLE:
                selected_hole = MI_HOLE_CIRCLE;
                break;
            case LL_PCODE_HOLE_SQUARE:
                selected_hole = MI_HOLE_SQUARE;
                break;
            case LL_PCODE_HOLE_TRIANGLE:
                selected_hole = MI_HOLE_TRIANGLE;
                break;
            case LL_PCODE_HOLE_SAME:
            default:
                selected_hole = MI_HOLE_SAME;
                break;
            }
            mComboHoleType->setCurrentByIndex( selected_hole );
            hole_enabled = enabled;
        }
        else
        {
            mComboHoleType->setCurrentByIndex( MI_HOLE_SAME );
            hole_enabled = false;
        }

        // Cut interpretation varies based on base object type
        F32 cut_begin, cut_end, adv_cut_begin, adv_cut_end;

        if ( selected_item == MI_SPHERE || selected_item == MI_TORUS ||
             selected_item == MI_TUBE   || selected_item == MI_RING )
        {
            cut_begin       = begin_t;
            cut_end         = end_t;
            adv_cut_begin   = begin_s;
            adv_cut_end     = end_s;
        }
        else
        {
            cut_begin       = begin_s;
            cut_end         = end_s;
            adv_cut_begin   = begin_t;
            adv_cut_end     = end_t;
        }

        mSpinCutBegin   ->set( cut_begin );
        mSpinCutEnd     ->set( cut_end );
        mCtrlPathBegin  ->set( adv_cut_begin );
        mCtrlPathEnd    ->set( adv_cut_end );
        calcp->setVar(LLCalc::CUT_BEGIN, cut_begin);
        calcp->setVar(LLCalc::CUT_END, cut_end);
        calcp->setVar(LLCalc::PATH_BEGIN, adv_cut_begin);
        calcp->setVar(LLCalc::PATH_END, adv_cut_end);

        // Twist
        F32 twist       = volume_params.getTwist();
        F32 twist_begin = volume_params.getTwistBegin();
        // Check the path type for conversion.
        if (path == LL_PCODE_PATH_LINE || path == LL_PCODE_PATH_FLEXIBLE)
        {
            twist       *= OBJECT_TWIST_LINEAR_MAX;
            twist_begin *= OBJECT_TWIST_LINEAR_MAX;
        }
        else
        {
            twist       *= OBJECT_TWIST_MAX;
            twist_begin *= OBJECT_TWIST_MAX;
        }

        mSpinTwist      ->set( twist );
        mSpinTwistBegin ->set( twist_begin );
        calcp->setVar(LLCalc::TWIST_END, twist);
        calcp->setVar(LLCalc::TWIST_BEGIN, twist_begin);

        // Shear
        F32 shear_x = volume_params.getShearX();
        F32 shear_y = volume_params.getShearY();
        mSpinShearX->set( shear_x );
        mSpinShearY->set( shear_y );
        calcp->setVar(LLCalc::X_SHEAR, shear_x);
        calcp->setVar(LLCalc::Y_SHEAR, shear_y);

        // Taper
        F32 taper_x = volume_params.getTaperX();
        F32 taper_y = volume_params.getTaperY();
        mSpinTaperX->set( taper_x );
        mSpinTaperY->set( taper_y );
        calcp->setVar(LLCalc::X_TAPER, taper_x);
        calcp->setVar(LLCalc::Y_TAPER, taper_y);

        // Radius offset.
        F32 radius_offset = volume_params.getRadiusOffset();
        // Limit radius offset, based on taper and hole size y.
        F32 radius_mag = fabs(radius_offset);
        F32 hole_y_mag = fabs(scale_y);
        F32 taper_y_mag  = fabs(taper_y);
        // Check to see if the taper effects us.
        if ( (radius_offset > 0.f && taper_y < 0.f) ||
             (radius_offset < 0.f && taper_y > 0.f) )
        {
            // The taper does not help increase the radius offset range.
            taper_y_mag = 0.f;
        }
        F32 max_radius_mag = 1.f - hole_y_mag * (1.f - taper_y_mag) / (1.f - hole_y_mag);
        // Enforce the maximum magnitude.
        if (radius_mag > max_radius_mag)
        {
            // Check radius offset sign.
            if (radius_offset < 0.f)
            {
                radius_offset = -max_radius_mag;
            }
            else
            {
                radius_offset = max_radius_mag;
            }
        }
        mSpinRadiusOffset->set( radius_offset);
        calcp->setVar(LLCalc::RADIUS_OFFSET, radius_offset);

        // Revolutions
        F32 revolutions = volume_params.getRevolutions();
        mSpinRevolutions->set( revolutions );
        calcp->setVar(LLCalc::REVOLUTIONS, revolutions);

        // Skew
        F32 skew    = volume_params.getSkew();
        // Limit skew, based on revolutions hole size x.
        F32 skew_mag= fabs(skew);
        F32 min_skew_mag = 1.0f - 1.0f / (revolutions * scale_x + 1.0f);
        // Discontinuity; A revolution of 1 allows skews below 0.5.
        if ( fabs(revolutions - 1.0f) < 0.001)
            min_skew_mag = 0.0f;

        // Clip skew.
        if (skew_mag < min_skew_mag)
        {
            // Check skew sign.
            if (skew < 0.0f)
            {
                skew = -min_skew_mag;
            }
            else
            {
                skew = min_skew_mag;
            }
        }
        mSpinSkew->set( skew );
        calcp->setVar(LLCalc::SKEW, skew);
    }

    // Compute control visibility, label names, and twist range.
    // Start with defaults.
    bool cut_visible                = true;
    bool hollow_visible             = true;
    bool top_size_x_visible         = true;
    bool top_size_y_visible         = true;
    bool top_shear_x_visible        = true;
    bool top_shear_y_visible        = true;
    bool twist_visible              = true;
    bool advanced_cut_visible       = false;
    bool taper_visible              = false;
    bool skew_visible               = false;
    bool radius_offset_visible      = false;
    bool revolutions_visible        = false;
    bool sculpt_texture_visible     = false;
    F32  twist_min                  = OBJECT_TWIST_LINEAR_MIN;
    F32  twist_max                  = OBJECT_TWIST_LINEAR_MAX;
    F32  twist_inc                  = OBJECT_TWIST_LINEAR_INC;

    bool advanced_is_dimple = false;
    bool advanced_is_slice = false;
    bool size_is_hole = false;

    // Tune based on overall volume type
    switch (selected_item)
    {
    case MI_SPHERE:
        top_size_x_visible      = false;
        top_size_y_visible      = false;
        top_shear_x_visible     = false;
        top_shear_y_visible     = false;
        //twist_visible         = false;
        advanced_cut_visible    = true;
        advanced_is_dimple      = true;
        twist_min               = OBJECT_TWIST_MIN;
        twist_max               = OBJECT_TWIST_MAX;
        twist_inc               = OBJECT_TWIST_INC;
        break;

    case MI_TORUS:
    case MI_TUBE:
    case MI_RING:
        //top_size_x_visible        = false;
        //top_size_y_visible        = false;
        size_is_hole            = true;
        skew_visible            = true;
        advanced_cut_visible    = true;
        taper_visible           = true;
        radius_offset_visible   = true;
        revolutions_visible     = true;
        twist_min               = OBJECT_TWIST_MIN;
        twist_max               = OBJECT_TWIST_MAX;
        twist_inc               = OBJECT_TWIST_INC;

        break;

    case MI_SCULPT:
        cut_visible             = false;
        hollow_visible          = false;
        twist_visible           = false;
        top_size_x_visible      = false;
        top_size_y_visible      = false;
        top_shear_x_visible     = false;
        top_shear_y_visible     = false;
        skew_visible            = false;
        advanced_cut_visible    = false;
        taper_visible           = false;
        radius_offset_visible   = false;
        revolutions_visible     = false;
        sculpt_texture_visible  = true;

        break;

    case MI_BOX:
        advanced_cut_visible    = true;
        advanced_is_slice       = true;
        break;

    case MI_CYLINDER:
        advanced_cut_visible    = true;
        advanced_is_slice       = true;
        break;

    case MI_PRISM:
        advanced_cut_visible    = true;
        advanced_is_slice       = true;
        break;

    default:
        break;
    }

    // Check if we need to change top size/hole size params.
    switch (selected_item)
    {
    case MI_SPHERE:
    case MI_TORUS:
    case MI_TUBE:
    case MI_RING:
        mSpinScaleX->set( scale_x );
        mSpinScaleY->set( scale_y );
        calcp->setVar(LLCalc::X_HOLE, scale_x);
        calcp->setVar(LLCalc::Y_HOLE, scale_y);
        mSpinScaleX->setMinValue(OBJECT_MIN_HOLE_SIZE);
        mSpinScaleX->setMaxValue(OBJECT_MAX_HOLE_SIZE_X);
        mSpinScaleY->setMinValue(OBJECT_MIN_HOLE_SIZE);
        mSpinScaleY->setMaxValue(OBJECT_MAX_HOLE_SIZE_Y);
        break;
    default:
        if (editable && single_volume)
        {
            mSpinScaleX->set( 1.f - scale_x );
            mSpinScaleY->set( 1.f - scale_y );
            mSpinScaleX->setMinValue(-1.f);
            mSpinScaleX->setMaxValue(1.f);
            mSpinScaleY->setMinValue(-1.f);
            mSpinScaleY->setMaxValue(1.f);

            // Torus' Hole Size is Box/Cyl/Prism's Taper
            calcp->setVar(LLCalc::X_TAPER, 1.f - scale_x);
            calcp->setVar(LLCalc::Y_TAPER, 1.f - scale_y);

            // Box/Cyl/Prism have no hole size
            calcp->setVar(LLCalc::X_HOLE, 0.f);
            calcp->setVar(LLCalc::Y_HOLE, 0.f);
        }
        break;
    }

    // Check if we need to limit the hollow based on the hole type.
    if (  selected_hole == MI_HOLE_SQUARE &&
          ( selected_item == MI_CYLINDER || selected_item == MI_TORUS ||
            selected_item == MI_PRISM    || selected_item == MI_RING  ||
            selected_item == MI_SPHERE ) )
    {
        mSpinHollow->setMinValue(0.f);
        mSpinHollow->setMaxValue(70.f);
    }
    else
    {
        mSpinHollow->setMinValue(0.f);
        mSpinHollow->setMaxValue(95.f);
    }

    // Update field enablement
    mComboBaseType  ->setEnabled( enabled );
    mMenuClipboardParams->setEnabled(enabled);

    mLabelCut       ->setEnabled( enabled );
    mSpinCutBegin   ->setEnabled( enabled );
    mSpinCutEnd     ->setEnabled( enabled );

    mLabelHollow    ->setEnabled( enabled );
    mSpinHollow     ->setEnabled( enabled );
    mLabelHoleType  ->setEnabled( hole_enabled );
    mComboHoleType  ->setEnabled( hole_enabled );

    mLabelTwist     ->setEnabled( enabled );
    mSpinTwist      ->setEnabled( enabled );
    mSpinTwistBegin ->setEnabled( enabled );

    mLabelSkew      ->setEnabled( enabled );
    mSpinSkew       ->setEnabled( enabled );

    getChildView("scale_hole")->setVisible( false);
    getChildView("scale_taper")->setVisible( false);
    if (top_size_x_visible || top_size_y_visible)
    {
        if (size_is_hole)
        {
            getChildView("scale_hole")->setVisible( true);
            getChildView("scale_hole")->setEnabled(enabled);
        }
        else
        {
            getChildView("scale_taper")->setVisible( true);
            getChildView("scale_taper")->setEnabled(enabled);
        }
    }

    mSpinScaleX     ->setEnabled( enabled );
    mSpinScaleY     ->setEnabled( enabled );

    mLabelShear     ->setEnabled( enabled );
    mSpinShearX     ->setEnabled( enabled );
    mSpinShearY     ->setEnabled( enabled );

    getChildView("advanced_cut")->setVisible( false);
    getChildView("advanced_dimple")->setVisible( false);
    getChildView("advanced_slice")->setVisible( false);

    if (advanced_cut_visible)
    {
        if (advanced_is_dimple)
        {
            getChildView("advanced_dimple")->setVisible( true);
            getChildView("advanced_dimple")->setEnabled(enabled);
        }

        else if (advanced_is_slice)
        {
            getChildView("advanced_slice")->setVisible( true);
            getChildView("advanced_slice")->setEnabled(enabled);
        }
        else
        {
            getChildView("advanced_cut")->setVisible( true);
            getChildView("advanced_cut")->setEnabled(enabled);
        }
    }

    mCtrlPathBegin  ->setEnabled( enabled );
    mCtrlPathEnd    ->setEnabled( enabled );

    mLabelTaper     ->setEnabled( enabled );
    mSpinTaperX     ->setEnabled( enabled );
    mSpinTaperY     ->setEnabled( enabled );

    mLabelRadiusOffset->setEnabled( enabled );
    mSpinRadiusOffset ->setEnabled( enabled );

    mLabelRevolutions->setEnabled( enabled );
    mSpinRevolutions ->setEnabled( enabled );

    // Update field visibility
    mLabelCut       ->setVisible( cut_visible );
    mSpinCutBegin   ->setVisible( cut_visible );
    mSpinCutEnd     ->setVisible( cut_visible );

    mLabelHollow    ->setVisible( hollow_visible );
    mSpinHollow     ->setVisible( hollow_visible );
    mLabelHoleType  ->setVisible( hollow_visible );
    mComboHoleType  ->setVisible( hollow_visible );

    mLabelTwist     ->setVisible( twist_visible );
    mSpinTwist      ->setVisible( twist_visible );
    mSpinTwistBegin ->setVisible( twist_visible );
    mSpinTwist      ->setMinValue(  twist_min );
    mSpinTwist      ->setMaxValue(  twist_max );
    mSpinTwist      ->setIncrement( twist_inc );
    mSpinTwistBegin ->setMinValue(  twist_min );
    mSpinTwistBegin ->setMaxValue(  twist_max );
    mSpinTwistBegin ->setIncrement( twist_inc );

    mSpinScaleX     ->setVisible( top_size_x_visible );
    mSpinScaleY     ->setVisible( top_size_y_visible );

    mLabelSkew      ->setVisible( skew_visible );
    mSpinSkew       ->setVisible( skew_visible );

    mLabelShear     ->setVisible( top_shear_x_visible || top_shear_y_visible );
    mSpinShearX     ->setVisible( top_shear_x_visible );
    mSpinShearY     ->setVisible( top_shear_y_visible );

    mCtrlPathBegin  ->setVisible( advanced_cut_visible );
    mCtrlPathEnd    ->setVisible( advanced_cut_visible );

    mLabelTaper     ->setVisible( taper_visible );
    mSpinTaperX     ->setVisible( taper_visible );
    mSpinTaperY     ->setVisible( taper_visible );

    mLabelRadiusOffset->setVisible( radius_offset_visible );
    mSpinRadiusOffset ->setVisible( radius_offset_visible );

    mLabelRevolutions->setVisible( revolutions_visible );
    mSpinRevolutions ->setVisible( revolutions_visible );

    // sculpt texture
    if (selected_item == MI_SCULPT)
    {


        LLUUID id;
        LLSculptParams *sculpt_params = (LLSculptParams *)objectp->getParameterEntry(LLNetworkData::PARAMS_SCULPT);


        if (sculpt_params) // if we have a legal sculpt param block for this object:
        {
            if (mObject != objectp)  // we've just selected a new object, so save for undo
            {
                mSculptTextureRevert = sculpt_params->getSculptTexture();
                mSculptTypeRevert    = sculpt_params->getSculptType();
            }

            U8 sculpt_type = sculpt_params->getSculptType();
            U8 sculpt_stitching = sculpt_type & LL_SCULPT_TYPE_MASK;
            bool sculpt_invert = sculpt_type & LL_SCULPT_FLAG_INVERT;
            bool sculpt_mirror = sculpt_type & LL_SCULPT_FLAG_MIRROR;
            isMesh = (sculpt_stitching == LL_SCULPT_TYPE_MESH);
            const bool params_editable = editable && single_volume;

            LLTextureCtrl*  mTextureCtrl = getChild<LLTextureCtrl>("sculpt texture control");
            if(mTextureCtrl)
            {
                mTextureCtrl->setTentative(false);
                mTextureCtrl->setEnabled(params_editable && !isMesh);
                if (editable)
                    mTextureCtrl->setImageAssetID(sculpt_params->getSculptTexture());
                else
                    mTextureCtrl->setImageAssetID(LLUUID::null);
            }

            mComboBaseType->setEnabled(params_editable && !isMesh);
            mMenuClipboardParams->setEnabled(params_editable && !isMesh);

            if (mCtrlSculptType)
            {
                if (sculpt_stitching == LL_SCULPT_TYPE_NONE)
                {
                    // since 'None' is no longer an option in the combo box
                    // use 'Plane' as an equivalent sculpt type
                    mCtrlSculptType->setSelectedByValue(LLSD(LL_SCULPT_TYPE_PLANE), true);
                }
                else
                {
                    mCtrlSculptType->setSelectedByValue(LLSD(sculpt_stitching), true);
                }
                mCtrlSculptType->setEnabled(params_editable && !isMesh);
            }

            if (mCtrlSculptMirror)
            {
                mCtrlSculptMirror->set(sculpt_mirror);
                mCtrlSculptMirror->setEnabled(params_editable && !isMesh);
            }

            if (mCtrlSculptInvert)
            {
                mCtrlSculptInvert->set(sculpt_invert);
                mCtrlSculptInvert->setEnabled(params_editable && !isMesh);
            }

            if (mLabelSculptType)
            {
                mLabelSculptType->setEnabled(!isMesh);
            }

        }
    }
    else
    {
        mSculptTextureRevert = LLUUID::null;
    }

    mCtrlSculptMirror->setVisible(sculpt_texture_visible && !isMesh);
    mCtrlSculptInvert->setVisible(sculpt_texture_visible && !isMesh);

    //----------------------------------------------------------------------------

    mObject = objectp;
    mRootObject = root_objectp;

    mLodInfoLayout = selected_item == MI_SCULPT;
    updateLodInfo();
}

// static
bool LLPanelObject::precommitValidate( const LLSD& data )
{
    // TODO: Richard will fill this in later.
    return true; // false means that validation failed and new value should not be commited.
}

void LLPanelObject::sendIsPhysical()
{
    bool value = mCheckPhysics->get();
    if( mIsPhysical != value )
    {
        LLSelectMgr::getInstance()->selectionUpdatePhysics(value);
        mIsPhysical = value;

        LL_INFOS("FloaterTools") << "update physics sent" << LL_ENDL;
    }
    else
    {
        LL_INFOS("FloaterTools") << "update physics not changed" << LL_ENDL;
    }
}

void LLPanelObject::sendIsTemporary()
{
    bool value = mCheckTemporary->get();
    if( mIsTemporary != value )
    {
        LLSelectMgr::getInstance()->selectionUpdateTemporary(value);
        mIsTemporary = value;

        LL_INFOS("FloaterTools") << "update temporary sent" << LL_ENDL;
    }
    else
    {
        LL_INFOS("FloaterTools") << "update temporary not changed" << LL_ENDL;
    }
}


void LLPanelObject::sendIsPhantom()
{
    bool value = mCheckPhantom->get();
    if( mIsPhantom != value )
    {
        LLSelectMgr::getInstance()->selectionUpdatePhantom(value);
        mIsPhantom = value;

        LL_INFOS("FloaterTools") << "update phantom sent" << LL_ENDL;
    }
    else
    {
        LL_INFOS("FloaterTools") << "update phantom not changed" << LL_ENDL;
    }
}

// static
void LLPanelObject::onCommitParametric( LLUICtrl* ctrl, void* userdata )
{
    LLPanelObject* self = (LLPanelObject*) userdata;

    if (self->mObject.isNull())
    {
        return;
    }

    if (self->mObject->getPCode() != LL_PCODE_VOLUME)
    {
        // Don't allow modification of non-volume objects.
        return;
    }

    LLVolume *volume = self->mObject->getVolume();
    if (!volume)
    {
        return;
    }

    LLVolumeParams volume_params;
    self->getVolumeParams(volume_params);



    // set sculpting
    S32 selected_type = self->mComboBaseType->getCurrentIndex();

    if (selected_type == MI_SCULPT)
    {
        self->mObject->setParameterEntryInUse(LLNetworkData::PARAMS_SCULPT, true, true);
        LLSculptParams *sculpt_params = (LLSculptParams *)self->mObject->getParameterEntry(LLNetworkData::PARAMS_SCULPT);
        if (sculpt_params)
            volume_params.setSculptID(sculpt_params->getSculptTexture(), sculpt_params->getSculptType());
    }
    else
    {
        LLSculptParams *sculpt_params = (LLSculptParams *)self->mObject->getParameterEntry(LLNetworkData::PARAMS_SCULPT);
        if (sculpt_params)
            self->mObject->setParameterEntryInUse(LLNetworkData::PARAMS_SCULPT, false, true);
    }

    // Update the volume, if necessary.
    self->mObject->updateVolume(volume_params);


    // This was added to make sure thate when changes are made, the UI
    // adjusts to present valid options.
    // *FIX: only some changes, ie, hollow or primitive type changes,
    // require a refresh.
    self->refresh();

}

void LLPanelObject::getVolumeParams(LLVolumeParams& volume_params)
{
    // Figure out what type of volume to make
    S32 was_selected_type = mSelectedType;
    S32 selected_type = mComboBaseType->getCurrentIndex();
    U8 profile;
    U8 path;
    switch ( selected_type )
    {
    case MI_CYLINDER:
        profile = LL_PCODE_PROFILE_CIRCLE;
        path = LL_PCODE_PATH_LINE;
        break;

    case MI_BOX:
        profile = LL_PCODE_PROFILE_SQUARE;
        path = LL_PCODE_PATH_LINE;
        break;

    case MI_PRISM:
        profile = LL_PCODE_PROFILE_EQUALTRI;
        path = LL_PCODE_PATH_LINE;
        break;

    case MI_SPHERE:
        profile = LL_PCODE_PROFILE_CIRCLE_HALF;
        path = LL_PCODE_PATH_CIRCLE;
        break;

    case MI_TORUS:
        profile = LL_PCODE_PROFILE_CIRCLE;
        path = LL_PCODE_PATH_CIRCLE;
        break;

    case MI_TUBE:
        profile = LL_PCODE_PROFILE_SQUARE;
        path = LL_PCODE_PATH_CIRCLE;
        break;

    case MI_RING:
        profile = LL_PCODE_PROFILE_EQUALTRI;
        path = LL_PCODE_PATH_CIRCLE;
        break;

    case MI_SCULPT:
        profile = LL_PCODE_PROFILE_CIRCLE;
        path = LL_PCODE_PATH_CIRCLE;
        break;

    default:
        LL_WARNS("FloaterTools") << "Unknown base type " << selected_type
            << " in getVolumeParams()" << LL_ENDL;
        // assume a box
        selected_type = MI_BOX;
        profile = LL_PCODE_PROFILE_SQUARE;
        path = LL_PCODE_PATH_LINE;
        break;
    }


    if (path == LL_PCODE_PATH_LINE)
    {
        LLVOVolume *volobjp = (LLVOVolume *)(LLViewerObject*)(mObject);
        if (volobjp->isFlexible())
        {
            path = LL_PCODE_PATH_FLEXIBLE;
        }
    }

    S32 selected_hole = mComboHoleType->getCurrentIndex();
    U8 hole;
    switch (selected_hole)
    {
    case MI_HOLE_CIRCLE:
        hole = LL_PCODE_HOLE_CIRCLE;
        break;
    case MI_HOLE_SQUARE:
        hole = LL_PCODE_HOLE_SQUARE;
        break;
    case MI_HOLE_TRIANGLE:
        hole = LL_PCODE_HOLE_TRIANGLE;
        break;
    case MI_HOLE_SAME:
    default:
        hole = LL_PCODE_HOLE_SAME;
        break;
    }

    volume_params.setType(profile | hole, path);
    mSelectedType = selected_type;

    // Compute cut start/end
    F32 cut_begin   = mSpinCutBegin->get();
    F32 cut_end     = mSpinCutEnd->get();

    // Make sure at least OBJECT_CUT_INC of the object survives
    if (cut_begin > cut_end - OBJECT_MIN_CUT_INC)
    {
        cut_begin = cut_end - OBJECT_MIN_CUT_INC;
        mSpinCutBegin->set(cut_begin);
    }

    F32 adv_cut_begin   = mCtrlPathBegin->get();
    F32 adv_cut_end     = mCtrlPathEnd->get();

    // Make sure at least OBJECT_CUT_INC of the object survives
    if (adv_cut_begin > adv_cut_end - OBJECT_MIN_CUT_INC)
    {
        adv_cut_begin = adv_cut_end - OBJECT_MIN_CUT_INC;
        mCtrlPathBegin->set(adv_cut_begin);
    }

    F32 begin_s, end_s;
    F32 begin_t, end_t;

    if (selected_type == MI_SPHERE || selected_type == MI_TORUS ||
        selected_type == MI_TUBE   || selected_type == MI_RING)
    {
        begin_s = adv_cut_begin;
        end_s   = adv_cut_end;

        begin_t = cut_begin;
        end_t   = cut_end;
    }
    else
    {
        begin_s = cut_begin;
        end_s   = cut_end;

        begin_t = adv_cut_begin;
        end_t   = adv_cut_end;
    }

    volume_params.setBeginAndEndS(begin_s, end_s);
    volume_params.setBeginAndEndT(begin_t, end_t);

    // Hollowness
    F32 hollow = mSpinHollow->get() / 100.f;

    if (  selected_hole == MI_HOLE_SQUARE &&
        ( selected_type == MI_CYLINDER || selected_type == MI_TORUS ||
          selected_type == MI_PRISM    || selected_type == MI_RING  ||
          selected_type == MI_SPHERE ) )
    {
        if (hollow > 0.7f) hollow = 0.7f;
    }

    volume_params.setHollow( hollow );

    // Twist Begin,End
    F32 twist_begin = mSpinTwistBegin->get();
    F32 twist       = mSpinTwist->get();
    // Check the path type for twist conversion.
    if (path == LL_PCODE_PATH_LINE || path == LL_PCODE_PATH_FLEXIBLE)
    {
        twist_begin /= OBJECT_TWIST_LINEAR_MAX;
        twist       /= OBJECT_TWIST_LINEAR_MAX;
    }
    else
    {
        twist_begin /= OBJECT_TWIST_MAX;
        twist       /= OBJECT_TWIST_MAX;
    }

    volume_params.setTwistBegin(twist_begin);
    volume_params.setTwist(twist);

    // Scale X,Y
    F32 scale_x = mSpinScaleX->get();
    F32 scale_y = mSpinScaleY->get();
    if ( was_selected_type == MI_BOX || was_selected_type == MI_CYLINDER || was_selected_type == MI_PRISM)
    {
        scale_x = 1.f - scale_x;
        scale_y = 1.f - scale_y;
    }

    // Skew
    F32 skew = mSpinSkew->get();

    // Taper X,Y
    F32 taper_x = mSpinTaperX->get();
    F32 taper_y = mSpinTaperY->get();

    // Radius offset
    F32 radius_offset = mSpinRadiusOffset->get();

    // Revolutions
    F32 revolutions   = mSpinRevolutions->get();

    if ( selected_type == MI_SPHERE )
    {
        // Snap values to valid sphere parameters.
        scale_x         = 1.0f;
        scale_y         = 1.0f;
        skew            = 0.0f;
        taper_x         = 0.0f;
        taper_y         = 0.0f;
        radius_offset   = 0.0f;
        revolutions     = 1.0f;
    }
    else if ( selected_type == MI_TORUS || selected_type == MI_TUBE ||
              selected_type == MI_RING )
    {
        scale_x = llclamp(
            scale_x,
            OBJECT_MIN_HOLE_SIZE,
            OBJECT_MAX_HOLE_SIZE_X);
        scale_y = llclamp(
            scale_y,
            OBJECT_MIN_HOLE_SIZE,
            OBJECT_MAX_HOLE_SIZE_Y);

        // Limit radius offset, based on taper and hole size y.
        F32 radius_mag = fabs(radius_offset);
        F32 hole_y_mag = fabs(scale_y);
        F32 taper_y_mag  = fabs(taper_y);
        // Check to see if the taper effects us.
        if ( (radius_offset > 0.f && taper_y < 0.f) ||
             (radius_offset < 0.f && taper_y > 0.f) )
        {
            // The taper does not help increase the radius offset range.
            taper_y_mag = 0.f;
        }
        F32 max_radius_mag = 1.f - hole_y_mag * (1.f - taper_y_mag) / (1.f - hole_y_mag);
        // Enforce the maximum magnitude.
        if (radius_mag > max_radius_mag)
        {
            // Check radius offset sign.
            if (radius_offset < 0.f)
            {
                radius_offset = -max_radius_mag;
            }
            else
            {
                radius_offset = max_radius_mag;
            }
        }

        // Check the skew value against the revolutions.
        F32 skew_mag= fabs(skew);
        F32 min_skew_mag = 1.0f - 1.0f / (revolutions * scale_x + 1.0f);
        // Discontinuity; A revolution of 1 allows skews below 0.5.
        if ( fabs(revolutions - 1.0f) < 0.001)
            min_skew_mag = 0.0f;

        // Clip skew.
        if (skew_mag < min_skew_mag)
        {
            // Check skew sign.
            if (skew < 0.0f)
            {
                skew = -min_skew_mag;
            }
            else
            {
                skew = min_skew_mag;
            }
        }
    }

    volume_params.setRatio( scale_x, scale_y );
    volume_params.setSkew(skew);
    volume_params.setTaper( taper_x, taper_y );
    volume_params.setRadiusOffset(radius_offset);
    volume_params.setRevolutions(revolutions);

    // Shear X,Y
    F32 shear_x = mSpinShearX->get();
    F32 shear_y = mSpinShearY->get();
    volume_params.setShear( shear_x, shear_y );

    if (selected_type == MI_SCULPT)
    {
        volume_params.setSculptID(LLUUID::null, 0);
        volume_params.setBeginAndEndT   (0, 1);
        volume_params.setBeginAndEndS   (0, 1);
        volume_params.setHollow         (0);
        volume_params.setTwistBegin     (0);
        volume_params.setTwistEnd       (0);
        volume_params.setRatio          (1, 0.5);
        volume_params.setShear          (0, 0);
        volume_params.setTaper          (0, 0);
        volume_params.setRevolutions    (1);
        volume_params.setRadiusOffset   (0);
        volume_params.setSkew           (0);
    }

}

// BUG: Make work with multiple objects
void LLPanelObject::sendRotation(bool btn_down)
{
    if (mObject.isNull()) return;

    LLVector3 new_rot(mCtrlRotX->get(), mCtrlRotY->get(), mCtrlRotZ->get());
    new_rot.mV[VX] = ll_round(new_rot.mV[VX], OBJECT_ROTATION_PRECISION);
    new_rot.mV[VY] = ll_round(new_rot.mV[VY], OBJECT_ROTATION_PRECISION);
    new_rot.mV[VZ] = ll_round(new_rot.mV[VZ], OBJECT_ROTATION_PRECISION);

    // Note: must compare before conversion to radians
    LLVector3 delta = new_rot - mCurEulerDegrees;

    if (delta.magVec() >= 0.0005f)
    {
        mCurEulerDegrees = new_rot;
        new_rot *= DEG_TO_RAD;

        LLQuaternion rotation;
        rotation.setQuat(new_rot.mV[VX], new_rot.mV[VY], new_rot.mV[VZ]);

        if (mRootObject != mObject)
        {
            rotation = rotation * ~mRootObject->getRotationRegion();
        }

        // To include avatars into movements and rotation
        // If false, all children are selected anyway - move avatar
        // If true, not all children are selected - save positions
        bool individual_selection = gSavedSettings.getBOOL("EditLinkedParts");
        std::vector<LLVector3>& child_positions = mObject->mUnselectedChildrenPositions ;
        std::vector<LLQuaternion> child_rotations;
        if (mObject->isRootEdit() && individual_selection)
        {
            mObject->saveUnselectedChildrenRotation(child_rotations) ;
            mObject->saveUnselectedChildrenPosition(child_positions) ;
        }

        mObject->setRotation(rotation);
        LLManip::rebuild(mObject) ;

        // for individually selected roots, we need to counterrotate all the children
        if (mObject->isRootEdit() && individual_selection)
        {
            mObject->resetChildrenRotationAndPosition(child_rotations, child_positions) ;
        }

        if(!btn_down)
        {
            child_positions.clear() ;
            LLSelectMgr::getInstance()->sendMultipleUpdate(UPD_ROTATION | UPD_POSITION);
        }
    }
}


// BUG: Make work with multiple objects
void LLPanelObject::sendScale(bool btn_down)
{
    if (mObject.isNull()) return;

    LLVector3 newscale(mCtrlScaleX->get(), mCtrlScaleY->get(), mCtrlScaleZ->get());

    LLVector3 delta = newscale - mObject->getScale();
    if (delta.magVec() >= 0.0005f || (mSizeChanged && !btn_down))
    {
        // scale changed by more than 1/2 millimeter
        mSizeChanged = btn_down;

        // check to see if we aren't scaling the textures
        // (in which case the tex coord's need to be recomputed)
        bool dont_stretch_textures = !LLManipScale::getStretchTextures();
        if (dont_stretch_textures)
        {
            LLSelectMgr::getInstance()->saveSelectedObjectTransform(SELECT_ACTION_TYPE_SCALE);
        }

        mObject->setScale(newscale, true);

        if(!btn_down)
        {
            LLSelectMgr::getInstance()->sendMultipleUpdate(UPD_SCALE | UPD_POSITION);
        }

        LLSelectMgr::getInstance()->adjustTexturesByScale(true, !dont_stretch_textures);
//      LL_INFOS() << "scale sent" << LL_ENDL;
    }
    else
    {
//      LL_INFOS() << "scale not changed" << LL_ENDL;
    }
}


void LLPanelObject::sendPosition(bool btn_down)
{
    if (mObject.isNull()) return;

    LLVector3 newpos(mCtrlPosX->get(), mCtrlPosY->get(), mCtrlPosZ->get());
    LLViewerRegion* regionp = mObject->getRegion();

    if (!regionp) return;

    if (!mObject->isAttachment())
    {
        // Clamp the Z height
        const F32 height = newpos.mV[VZ];
        const F32 min_height = LLWorld::getInstance()->getMinAllowedZ(mObject, mObject->getPositionGlobal());
        const F32 max_height = LLWorld::getInstance()->getRegionMaxHeight();

        if ( height < min_height)
        {
            newpos.mV[VZ] = min_height;
            mCtrlPosZ->set( min_height );
        }
        else if ( height > max_height )
        {
            newpos.mV[VZ] = max_height;
            mCtrlPosZ->set( max_height );
        }

        // Grass is always drawn on the ground, so clamp its position to the ground
        if (mObject->getPCode() == LL_PCODE_LEGACY_GRASS)
        {
            mCtrlPosZ->set(LLWorld::getInstance()->resolveLandHeightAgent(newpos) + 1.f);
        }
    }
    else
    {
        if (newpos.length() > MAX_ATTACHMENT_DIST)
        {
            newpos.clampLength(MAX_ATTACHMENT_DIST);
            mCtrlPosX->set(newpos.mV[VX]);
            mCtrlPosY->set(newpos.mV[VY]);
            mCtrlPosZ->set(newpos.mV[VZ]);
        }
    }

    // Make sure new position is in a valid region, so the object
    // won't get dumped by the simulator.
    LLVector3d new_pos_global = regionp->getPosGlobalFromRegion(newpos);
    bool is_valid_pos = true;
    if (mObject->isAttachment())
    {
        LLVector3 delta_pos = mObject->getPositionEdit() - newpos;
        LLVector3d attachment_pos = regionp->getPosGlobalFromRegion(mObject->getPositionRegion() + delta_pos);
        is_valid_pos = LLWorld::getInstance()->positionRegionValidGlobal(attachment_pos);
    }
    else
    {
        is_valid_pos = LLWorld::getInstance()->positionRegionValidGlobal(new_pos_global);
    }

    if (is_valid_pos)
    {
        // send only if the position is changed, that is, the delta vector is not zero
        LLVector3d old_pos_global = mObject->getPositionGlobal();
        LLVector3d delta = new_pos_global - old_pos_global;
        // moved more than 1/2 millimeter
        if (delta.magVec() >= 0.0005f)
        {
            if (mRootObject != mObject)
            {
                newpos = newpos - mRootObject->getPositionRegion();
                newpos = newpos * ~mRootObject->getRotationRegion();
                mObject->setPositionParent(newpos);
            }
            else
            {
                mObject->setPositionEdit(newpos);
            }

            LLManip::rebuild(mObject) ;

            // for individually selected roots, we need to counter-translate all unselected children
            if (mObject->isRootEdit())
            {
                // only offset by parent's translation
                mObject->resetChildrenPosition(LLVector3(-delta), true, true) ;
            }

            if(!btn_down)
            {
                LLSelectMgr::getInstance()->sendMultipleUpdate(UPD_POSITION);
            }

            LLSelectMgr::getInstance()->updateSelectionCenter();
        }
    }
    else
    {
        // move failed, so we update the UI with the correct values
        LLVector3 vec = mRootObject->getPositionRegion();
        mCtrlPosX->set(vec.mV[VX]);
        mCtrlPosY->set(vec.mV[VY]);
        mCtrlPosZ->set(vec.mV[VZ]);
    }
}

void LLPanelObject::sendSculpt()
{
    if (mObject.isNull())
        return;

    LLSculptParams sculpt_params;
    LLUUID sculpt_id = LLUUID::null;

    if (mCtrlSculptTexture)
    {
        sculpt_id = mCtrlSculptTexture->getImageAssetID();
    }

    U8 sculpt_type = 0;

    if (mCtrlSculptType)
    {
        sculpt_type |= mCtrlSculptType->getValue().asInteger();
    }

    bool enabled = sculpt_type != LL_SCULPT_TYPE_MESH;

    if (mCtrlSculptMirror)
    {
        mCtrlSculptMirror->setEnabled(enabled);
    }

    if (mCtrlSculptInvert)
    {
        mCtrlSculptInvert->setEnabled(enabled);
    }

    if ((mCtrlSculptMirror) && (mCtrlSculptMirror->get()))
    {
        sculpt_type |= LL_SCULPT_FLAG_MIRROR;
    }

    if ((mCtrlSculptInvert) && (mCtrlSculptInvert->get()))
    {
        sculpt_type |= LL_SCULPT_FLAG_INVERT;
    }

    sculpt_params.setSculptTexture(sculpt_id, sculpt_type);
    mObject->setParameterEntry(LLNetworkData::PARAMS_SCULPT, sculpt_params, true);
}

void LLPanelObject::refresh()
{
    getState();
    if (mObject.notNull() && mObject->isDead())
    {
        mObject = NULL;
    }

    if (mRootObject.notNull() && mRootObject->isDead())
    {
        mRootObject = NULL;
    }

    F32 max_scale = get_default_max_prim_scale(LLPickInfo::isFlora(mObject));

    getChild<LLSpinCtrl>("Scale X")->setMaxValue(max_scale);
    getChild<LLSpinCtrl>("Scale Y")->setMaxValue(max_scale);
    getChild<LLSpinCtrl>("Scale Z")->setMaxValue(max_scale);
}


void LLPanelObject::draw()
{
    const LLColor4  white(  1.0f,   1.0f,   1.0f,   1);
    const LLColor4  red(    1.0f,   0.25f,  0.f,    1);
    const LLColor4  green(  0.f,    1.0f,   0.f,    1);
    const LLColor4  blue(   0.f,    0.5f,   1.0f,   1);

    // Tune the colors of the labels
    LLTool* tool = LLToolMgr::getInstance()->getCurrentTool();

    if (tool == LLToolCompTranslate::getInstance())
    {
        mCtrlPosX   ->setLabelColor(red);
        mCtrlPosY   ->setLabelColor(green);
        mCtrlPosZ   ->setLabelColor(blue);

        mCtrlScaleX ->setLabelColor(white);
        mCtrlScaleY ->setLabelColor(white);
        mCtrlScaleZ ->setLabelColor(white);

        mCtrlRotX   ->setLabelColor(white);
        mCtrlRotY   ->setLabelColor(white);
        mCtrlRotZ   ->setLabelColor(white);
    }
    else if ( tool == LLToolCompScale::getInstance() )
    {
        mCtrlPosX   ->setLabelColor(white);
        mCtrlPosY   ->setLabelColor(white);
        mCtrlPosZ   ->setLabelColor(white);

        mCtrlScaleX ->setLabelColor(red);
        mCtrlScaleY ->setLabelColor(green);
        mCtrlScaleZ ->setLabelColor(blue);

        mCtrlRotX   ->setLabelColor(white);
        mCtrlRotY   ->setLabelColor(white);
        mCtrlRotZ   ->setLabelColor(white);
    }
    else if ( tool == LLToolCompRotate::getInstance() )
    {
        mCtrlPosX   ->setLabelColor(white);
        mCtrlPosY   ->setLabelColor(white);
        mCtrlPosZ   ->setLabelColor(white);

        mCtrlScaleX ->setLabelColor(white);
        mCtrlScaleY ->setLabelColor(white);
        mCtrlScaleZ ->setLabelColor(white);

        mCtrlRotX   ->setLabelColor(red);
        mCtrlRotY   ->setLabelColor(green);
        mCtrlRotZ   ->setLabelColor(blue);
    }
    else
    {
        mCtrlPosX   ->setLabelColor(white);
        mCtrlPosY   ->setLabelColor(white);
        mCtrlPosZ   ->setLabelColor(white);

        mCtrlScaleX ->setLabelColor(white);
        mCtrlScaleY ->setLabelColor(white);
        mCtrlScaleZ ->setLabelColor(white);

        mCtrlRotX   ->setLabelColor(white);
        mCtrlRotY   ->setLabelColor(white);
        mCtrlRotZ   ->setLabelColor(white);
    }

    if (mLodTitle && mLodTitle->getVisible())
    {
        updateLodInfo();
    }

    LLPanel::draw();
}

// virtual
void LLPanelObject::clearCtrls()
{
    LLPanel::clearCtrls();

    mCheckLock      ->set(false);
    mCheckLock      ->setEnabled( false );
    mCheckPhysics   ->set(false);
    mCheckPhysics   ->setEnabled( false );
    mCheckTemporary ->set(false);
    mCheckTemporary ->setEnabled( false );
    mCheckPhantom   ->set(false);
    mCheckPhantom   ->setEnabled( false );

    mBtnCopyAll->setEnabled(false);
    mBtnPasteAll->setEnabled(false);
    mBtnCopyPos->setEnabled(false);
    mBtnPastePos->setEnabled(false);
    mBtnCopySize->setEnabled(false);
    mBtnPasteSize->setEnabled(false);
    mBtnCopyRot->setEnabled(false);
    mBtnPasteRot->setEnabled(false);

    // Disable text labels
    mLabelPosition  ->setEnabled( false );
    mLabelSize      ->setEnabled( false );
    mLabelRotation  ->setEnabled( false );
    mLabelCut       ->setEnabled( false );
    mLabelHollow    ->setEnabled( false );
    mLabelHoleType  ->setEnabled( false );
    mLabelTwist     ->setEnabled( false );
    mLabelSkew      ->setEnabled( false );
    mLabelShear     ->setEnabled( false );
    mLabelTaper     ->setEnabled( false );
    mLabelRadiusOffset->setEnabled( false );
    mLabelRevolutions->setEnabled( false );

    getChildView("scale_hole")->setEnabled(false);
    getChildView("scale_taper")->setEnabled(false);
    getChildView("advanced_cut")->setEnabled(false);
    getChildView("advanced_dimple")->setEnabled(false);
    getChildView("advanced_slice")->setVisible( false);

    mLodInfoLayout = false;
    setLodInfoVisible(false);
}

namespace
{
// One prim as the clean renderer draws it (#272).
struct LodInfo
{
    U32  tris[4] = {}, verts[4] = {};   // loaded levels, summed over faces
    S32  faces = 0;                     // faces in the renderer
    S32  loaded[4] = {};                // faces with the level loaded
    U32  drawn_tris = 0;
    S32  drawn_level = -1;              // -1 none, -2 differs per face
    S32  want_level = -1;               // the cull's distance level (static faces)
    F32  radius = 0.f, dist = 0.f;
    bool worn = false;
};

F32 cull_lod_factor()
{
    const F32 lodf = LLVOVolume::sLODFactor;
    return lodf < 0.01f ? 1.125f : lodf;   // the cull's guard (llappviewer.cpp)
}

// clean_cull.comp detailFromTan(lod_factor * radius / dist) rounds tan to 0.01 first,
// so the switches sit at raw tan 0.035 / 0.065 / 0.245.
constexpr F32 LOD_SWITCH_TAN[4] = { 0.f, 0.035f, 0.065f, 0.245f };

S32 lod_level_at(F32 tan_angle)
{
    return tan_angle < LOD_SWITCH_TAN[1] ? 0 : tan_angle < LOD_SWITCH_TAN[2] ? 1 : tan_angle < LOD_SWITCH_TAN[3] ? 2 : 3;
}

void gather_lod_info(LLViewerObject* obj, LodInfo& out)
{
    if (!obj || obj->isDead() || obj->getPCode() != LL_PCODE_VOLUME || obj->mGLTFAsset) return;
    LLCleanMeshAsset& ma = LLCleanMeshAsset::instance();
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    const F32 lodf = cull_lod_factor();
    for (S32 f = 0; f < (S32)obj->getNumTEs(); ++f)
    {
        LLCleanInstanceStore::Instance in;
        if (!LLCleanInstanceStore::instance().debugGet(obj->getID(), f, in)) continue;
        if (in.flags & (32u | 32768u)) continue;   // terrain / vegetation
        U32 tris[4], verts[4];
        if (!ma.groupLodCounts(in.lod_group_id, tris, verts)) continue;
        // Rigged / HUD / worn: the cull draws the best loaded level, and a worn
        // instance's matrix is joint-local (no camera distance).
        const bool worn = (in.flags & (1u | 8u | 512u)) != 0u;
        S32 level = -1;
        if (worn)
        {
            for (S32 L = 3; L >= 0 && level < 0; --L)
                if (tris[L]) level = L;
        }
        else
        {
            ma.drawnSlot(in.lod_group_id, in.model_matrix, in.aabb_min[3], cam, lodf, &level);
        }
        if (out.faces == 0)
        {
            out.worn = worn;
            out.radius = in.aabb_min[3];
            if (!worn)
            {
                const LLVector3 d(in.model_matrix[12] - cam.mV[0], in.model_matrix[13] - cam.mV[1],
                                  in.model_matrix[14] - cam.mV[2]);
                out.dist = llmax(d.length(), 0.001f);
                out.want_level = lod_level_at(lodf * out.radius / out.dist);
            }
            out.drawn_level = level;
        }
        else if (out.drawn_level != level)
        {
            out.drawn_level = -2;
        }
        ++out.faces;
        for (S32 L = 0; L < 4; ++L)
        {
            out.tris[L] += tris[L];
            out.verts[L] += verts[L];
            if (tris[L]) ++out.loaded[L];
        }
        if (level >= 0) out.drawn_tris += tris[level];
    }
}

std::string lod_count_str(F32 n)
{
    if (n < 10000.f) return llformat("%.0f", n);
    if (n < 1000000.f) return llformat("%.1fk", n / 1000.f);
    return llformat("%.1fM", n / 1000000.f);
}

std::string lod_dist_str(F32 d)
{
    return d < 100.f ? llformat("%.1fm", d) : llformat("%.0fm", d);
}
} // namespace

void LLPanelObject::setLodInfoVisible(bool visible)
{
    if (!mLodTitle) return;
    LLTextBox* const boxes[] = { mLodTitle, mLodCols[0], mLodCols[1], mLodCols[2], mLodCols[3], mLodStatus };
    for (LLTextBox* box : boxes)
    {
        box->setVisible(visible);
        if (visible) box->setEnabled(true);   // clearCtrls disables every ctrl
    }
}

void LLPanelObject::updateLodInfo()
{
    LLViewerObject* obj = mObject.get();
    const bool show = mLodInfoLayout && obj && !obj->isDead() && obj->getPCode() == LL_PCODE_VOLUME
                      && obj->getVolume() && obj->getParameterEntryInUse(LLNetworkData::PARAMS_SCULPT)
                      && !obj->mGLTFAsset;
    setLodInfoVisible(show);
    if (!show)
    {
        mLodInfoObject.setNull();
        return;
    }
    // Selection changes refresh now; otherwise a low rate (getState runs on every drag).
    if (obj->getID() == mLodInfoObject && mLodInfoTimer.getElapsedTimeF32() < 0.5f) return;
    mLodInfoObject = obj->getID();
    mLodInfoTimer.reset();

    LLVOVolume* vobj = (LLVOVolume*)obj;
    LodInfo info;
    gather_lod_info(obj, info);
    const F32 lodf = cull_lod_factor();

    // Levels the renderer hasn't loaded: the mesh header's estimate (size 0 = no such LOD).
    LLMeshCostData cost;
    const bool is_mesh = vobj->isMesh();
    const bool have_cost = is_mesh && gMeshRepo.getCostData(obj->getVolume()->getParams().getSculptID(), cost);

    mLodTitle->setText(llformat("%s LOD (factor %.2f)", is_mesh ? "Mesh" : "Sculpt", lodf));

    static const char* const lod_names[4] = { "Lowest", "Low", "Medium", "High" };
    LLStyle::Params hilite;
    hilite.color = LLUIColorTable::instance().getColor("EmphasisColor");
    LLStyle::Params plain;
    const char* const headers[4] = { "LOD", "Tris", "Verts", "Until" };
    for (S32 c = 0; c < 4; ++c)
    {
        mLodCols[c]->setText(LLStringUtil::null);
        mLodCols[c]->appendText(headers[c], false, plain);
    }
    for (S32 L = 3; L >= 0; --L)
    {
        const bool loaded = info.faces > 0 && info.loaded[L] == info.faces;
        std::string tris = "-", verts = "-", dist = "-";
        if (loaded)
        {
            tris = lod_count_str((F32)info.tris[L]);
            verts = lod_count_str((F32)info.verts[L]);
        }
        else if (have_cost)
        {
            tris = cost.getSizeByLOD(L) > 0 ? "~" + lod_count_str(cost.getEstTrisByLOD(L)) : std::string("none");
        }
        if (info.faces > 0 && !info.worn && L > 0)
            dist = lod_dist_str(lodf * info.radius / LOD_SWITCH_TAN[L]);
        const LLStyle::Params& style = L == info.drawn_level ? hilite : plain;
        mLodCols[0]->appendText(lod_names[L], true, style);
        mLodCols[1]->appendText(tris, true, style);
        mLodCols[2]->appendText(verts, true, style);
        mLodCols[3]->appendText(dist, true, style);
    }

    std::string status;
    if (info.faces == 0)
    {
        status = "Not in the renderer yet.";
    }
    else
    {
        const std::string drawn = info.drawn_level >= 0 ? lod_names[info.drawn_level]
                                : info.drawn_level == -2 ? "mixed per face" : "nothing";
        if (info.worn)
        {
            status = llformat("Drawn: %s (rigged / worn: best loaded LOD)", drawn.c_str());
        }
        else
        {
            status = llformat("Drawn: %s at %s", drawn.c_str(), lod_dist_str(info.dist).c_str());
            if (info.want_level >= 0 && info.drawn_level >= 0 && info.want_level != info.drawn_level)
                status += llformat(" (%s not loaded)", lod_names[info.want_level]);
        }
        status += llformat("\nRadius %.2fm, %s tris drawn", info.radius, lod_count_str((F32)info.drawn_tris).c_str());
    }

    // Linkset total (whole linkset selected, or the root of the part being edited).
    LLViewerObject* root = mRootObject.get();
    if (root && !root->isDead() && !root->getChildren().empty())
    {
        LodInfo total;
        S32 prims = 0;
        gather_lod_info(root, total);
        ++prims;
        for (const LLPointer<LLViewerObject>& child : root->getChildren())
        {
            LodInfo ci;
            gather_lod_info(child.get(), ci);
            total.drawn_tris += ci.drawn_tris;
            ++prims;
        }
        status += llformat("\nLinkset (%d prims): %s tris drawn", prims, lod_count_str((F32)total.drawn_tris).c_str());
    }
    mLodStatus->setText(status);
}

//
// Static functions
//

// static
void LLPanelObject::onCommitLock(LLUICtrl *ctrl, void *data)
{
    // Checkbox will have toggled itself
    LLPanelObject *self = (LLPanelObject *)data;

    if(self->mRootObject.isNull()) return;

    bool new_state = self->mCheckLock->get();

    LLSelectMgr::getInstance()->selectionSetObjectPermissions(PERM_OWNER, !new_state, PERM_MOVE | PERM_MODIFY);
}

// static
void LLPanelObject::onCommitPosition( LLUICtrl* ctrl, void* userdata )
{
    LLPanelObject* self = (LLPanelObject*) userdata;
    bool btn_down = ((LLSpinCtrl*)ctrl)->isMouseHeldDown() ;
    self->sendPosition(btn_down);
}

// static
void LLPanelObject::onCommitScale( LLUICtrl* ctrl, void* userdata )
{
    LLPanelObject* self = (LLPanelObject*) userdata;
    bool btn_down = ((LLSpinCtrl*)ctrl)->isMouseHeldDown() ;
    self->sendScale(btn_down);
}

// static
void LLPanelObject::onCommitRotation( LLUICtrl* ctrl, void* userdata )
{
    LLPanelObject* self = (LLPanelObject*) userdata;
    bool btn_down = ((LLSpinCtrl*)ctrl)->isMouseHeldDown() ;
    self->sendRotation(btn_down);
}

// static
void LLPanelObject::onCommitPhysics( LLUICtrl* ctrl, void* userdata )
{
    LLPanelObject* self = (LLPanelObject*) userdata;
    self->sendIsPhysical();
}

// static
void LLPanelObject::onCommitTemporary( LLUICtrl* ctrl, void* userdata )
{
    LLPanelObject* self = (LLPanelObject*) userdata;
    self->sendIsTemporary();
}

// static
void LLPanelObject::onCommitPhantom( LLUICtrl* ctrl, void* userdata )
{
    LLPanelObject* self = (LLPanelObject*) userdata;
    self->sendIsPhantom();
}

void LLPanelObject::onSelectSculpt(const LLSD& data)
{
    LLTextureCtrl* mTextureCtrl = getChild<LLTextureCtrl>("sculpt texture control");

    if (mTextureCtrl)
    {
        mSculptTextureRevert = mTextureCtrl->getImageAssetID();
    }

    sendSculpt();
}


void LLPanelObject::onCommitSculpt( const LLSD& data )
{
    sendSculpt();
}

bool LLPanelObject::onDropSculpt(LLInventoryItem* item)
{
    LLTextureCtrl* mTextureCtrl = getChild<LLTextureCtrl>("sculpt texture control");

    if (mTextureCtrl)
    {
        LLUUID asset = item->getAssetUUID();

        mTextureCtrl->setImageAssetID(asset);
        mSculptTextureRevert = asset;
    }

    return true;
}


void LLPanelObject::onCancelSculpt(const LLSD& data)
{
    LLTextureCtrl* mTextureCtrl = getChild<LLTextureCtrl>("sculpt texture control");
    if(!mTextureCtrl)
        return;

    if(mSculptTextureRevert == LLUUID::null)
    {
        mSculptTextureRevert = SCULPT_DEFAULT_TEXTURE;
    }
    mTextureCtrl->setImageAssetID(mSculptTextureRevert);

    sendSculpt();
}

// static
void LLPanelObject::onCommitSculptType(LLUICtrl *ctrl, void* userdata)
{
    LLPanelObject* self = (LLPanelObject*) userdata;

    self->sendSculpt();
}

void LLPanelObject::menuDoToSelected(const LLSD& userdata)
{
    std::string command = userdata.asString();

    // paste
    if (command == "psr_paste")
    {
        // "Paste all" only ever uses the internal values from "Copy all"
        onPastePos(false);
        onPasteSize(false);
        onPasteRot(false);
    }
    else if (command == "pos_paste")
    {
        onPastePos();
    }
    else if (command == "size_paste")
    {
        onPasteSize();
    }
    else if (command == "rot_paste")
    {
        onPasteRot();
    }
    else if (command == "params_paste")
    {
        onPasteParams();
    }
    // copy
    else if (command == "psr_copy")
    {
        onCopyPos();
        onCopySize();
        onCopyRot();
    }
    else if (command == "pos_copy")
    {
        onCopyPos();
    }
    else if (command == "size_copy")
    {
        onCopySize();
    }
    else if (command == "rot_copy")
    {
        onCopyRot();
    }
    else if (command == "params_copy")
    {
        onCopyParams();
    }
}

bool LLPanelObject::menuEnableItem(const LLSD& userdata)
{
    std::string command = userdata.asString();

    // paste options
    if (command == "psr_paste")
    {
        S32 selected_count = LLSelectMgr::getInstance()->getSelection()->getObjectCount();
        bool single_volume = (LLSelectMgr::getInstance()->selectionAllPCode(LL_PCODE_VOLUME))
            && (selected_count == 1);

        if (!single_volume)
        {
            return false;
        }

        bool enable_move;
        bool enable_modify;

        LLSelectMgr::getInstance()->selectGetEditMoveLinksetPermissions(enable_move, enable_modify);

        return enable_move && enable_modify && mHasClipboardPos && mHasClipboardSize && mHasClipboardRot;
    }
    else if (command == "pos_paste")
    {
        // assumes that menu won't be active if there is no move permission
        return mHasClipboardPos;
    }
    else if (command == "size_paste")
    {
        return mHasClipboardSize;
    }
    else if (command == "rot_paste")
    {
        return mHasClipboardRot;
    }
    else if (command == "params_paste")
    {
        return mClipboardParams.isMap() && (mClipboardParams.size() != 0);
    }
    // copy options
    else if (command == "psr_copy")
    {
        S32 selected_count = LLSelectMgr::getInstance()->getSelection()->getObjectCount();
        bool single_volume = (LLSelectMgr::getInstance()->selectionAllPCode(LL_PCODE_VOLUME))
            && (selected_count == 1);

        if (!single_volume)
        {
            return false;
        }

        bool enable_move;
        bool enable_modify;

        LLSelectMgr::getInstance()->selectGetEditMoveLinksetPermissions(enable_move, enable_modify);

        // since we forbid seeing values we also should forbid copying them
        return enable_move && enable_modify;
    }
    return false;
}

void LLPanelObject::updateClipboardButtons()
{
    // Per-vector Copy/Paste follow the field's editability (set in getState()).
    // Copy all / Paste all keep the old menu rules: single volume, move+modify
    // permission, and (paste) a full internal PSR copy.
    mBtnCopyAll->setEnabled(menuEnableItem("psr_copy"));
    mBtnPasteAll->setEnabled(menuEnableItem("psr_paste"));
}

// Strict "<x, y, z>" vector parser. The idea and strictness rules (trim, require
// angle brackets, exactly three finite floats, reject trailing garbage) come from
// community PR #602 by TheSlowly.
bool LLPanelObject::parseVectorText(const std::string& text, LLVector3& out)
{
    std::string s = text;
    LLStringUtil::trim(s);
    if (s.size() < 7 || s.front() != '<' || s.back() != '>') return false;
    s = s.substr(1, s.size() - 2);

    F32 v[3];
    size_t pos = 0;
    for (S32 i = 0; i < 3; ++i)
    {
        size_t end = (i < 2) ? s.find(',', pos) : s.size();
        if (end == std::string::npos) return false;
        std::string tok = s.substr(pos, end - pos);
        LLStringUtil::trim(tok);
        const char* b = tok.data();
        const char* e = b + tok.size();
        if (b != e && *b == '+') ++b; // from_chars rejects a leading '+'
        if (b == e || *b == '+') return false;
        double d = 0.0;
        auto res = std::from_chars(b, e, d);
        // rejects empty tokens, "1.0abc", a 4th component ("1,2,3,4" leaves ",4" in the last token), nan/inf
        if (res.ec != std::errc() || res.ptr != e || !std::isfinite(d)) return false;
        F32 f = (F32)d;
        if (!std::isfinite(f)) return false;
        v[i] = f;
        pos = end + 1;
    }
    out.set(v[0], v[1], v[2]);
    return true;
}

// Paste source rule (per-vector Paste buttons):
//  1. If the system clipboard still holds exactly the text our last Copy wrote,
//     use the internal full-precision value (the text itself is rounded to %g).
//  2. Otherwise the clipboard changed outside us: if it parses as a strict
//     "<x, y, z>", use it (e.g. a vector copied from LSL or chat).
//  3. Otherwise fall back to the internal value copied this session, if any.
//  4. Otherwise log and tell the user nothing was pasted.
// "Paste all" passes from_system_clipboard=false and uses internal values only.
bool LLPanelObject::resolvePasteVector(const LLVector3& internal_value, bool has_internal,
                                       bool from_system_clipboard, const char* what, LLVector3& out)
{
    if (from_system_clipboard)
    {
        std::string clip;
        LLWString wclip;
        if (LLView::getWindow() && LLView::getWindow()->pasteTextFromClipboard(wclip))
        {
            clip = wstring_to_utf8str(wclip);
        }
        if (has_internal && clip == mLastCopiedText)
        {
            out = internal_value;
            return true;
        }
        if (parseVectorText(clip, out))
        {
            return true;
        }
    }
    if (has_internal)
    {
        out = internal_value;
        return true;
    }
    LL_INFOS("FloaterTools") << "Paste " << what << ": nothing to paste (no value copied this session and the clipboard is not a valid <x, y, z> vector)" << LL_ENDL;
    LLSD args;
    args["MESSAGE"] = std::string("Cannot paste ") + what + ": the clipboard does not contain a vector like <1.0, 2.0, 3.0>.";
    LLNotificationsUtil::add("SystemMessageTip", args);
    return false;
}

void LLPanelObject::onCopyPos()
{
    mClipboardPos = LLVector3(mCtrlPosX->get(), mCtrlPosY->get(), mCtrlPosZ->get());

    std::string stringVec = llformat("<%g, %g, %g>", mClipboardPos.mV[VX], mClipboardPos.mV[VY], mClipboardPos.mV[VZ]);
    mLastCopiedText = stringVec;
    LLView::getWindow()->copyTextToClipboard(utf8str_to_wstring(stringVec));

    mHasClipboardPos = true;
    updateClipboardButtons();
}

void LLPanelObject::onCopySize()
{
    mClipboardSize = LLVector3(mCtrlScaleX->get(), mCtrlScaleY->get(), mCtrlScaleZ->get());

    std::string stringVec = llformat("<%g, %g, %g>", mClipboardSize.mV[VX], mClipboardSize.mV[VY], mClipboardSize.mV[VZ]);
    mLastCopiedText = stringVec;
    LLView::getWindow()->copyTextToClipboard(utf8str_to_wstring(stringVec));

    mHasClipboardSize = true;
    updateClipboardButtons();
}

void LLPanelObject::onCopyRot()
{
    mClipboardRot = LLVector3(mCtrlRotX->get(), mCtrlRotY->get(), mCtrlRotZ->get());

    std::string stringVec = llformat("<%g, %g, %g>", mClipboardRot.mV[VX], mClipboardRot.mV[VY], mClipboardRot.mV[VZ]);
    mLastCopiedText = stringVec;
    LLView::getWindow()->copyTextToClipboard(utf8str_to_wstring(stringVec));

    mHasClipboardRot = true;
    updateClipboardButtons();
}

void LLPanelObject::onPastePos(bool from_system_clipboard)
{
    if (mObject.isNull()) return;

    LLVector3 value;
    if (!resolvePasteVector(mClipboardPos, mHasClipboardPos, from_system_clipboard, "position", value)) return;

    LLViewerRegion* regionp = mObject->getRegion();
    if (!regionp) return;


    // Clamp pos on non-attachments, just keep the prims within the region
    if (!mObject->isAttachment())
    {
        F32 max_width = regionp->getWidth(); // meters
        value.mV[VX] = llclamp(value.mV[VX], 0.f, max_width);
        value.mV[VY] = llclamp(value.mV[VY], 0.f, max_width);
        //height will get properly clamped by sendPosition
    }
    else
    {
        value.clampLength(MAX_ATTACHMENT_DIST);
    }

    mCtrlPosX->set( value.mV[VX] );
    mCtrlPosY->set( value.mV[VY] );
    mCtrlPosZ->set( value.mV[VZ] );

    sendPosition(false);
}

void LLPanelObject::onPasteSize(bool from_system_clipboard)
{
    LLVector3 value;
    if (!resolvePasteVector(mClipboardSize, mHasClipboardSize, from_system_clipboard, "size", value)) return;

    value.mV[VX] = llclamp(value.mV[VX], MIN_PRIM_SCALE, DEFAULT_MAX_PRIM_SCALE);
    value.mV[VY] = llclamp(value.mV[VY], MIN_PRIM_SCALE, DEFAULT_MAX_PRIM_SCALE);
    value.mV[VZ] = llclamp(value.mV[VZ], MIN_PRIM_SCALE, DEFAULT_MAX_PRIM_SCALE);

    mCtrlScaleX->set(value.mV[VX]);
    mCtrlScaleY->set(value.mV[VY]);
    mCtrlScaleZ->set(value.mV[VZ]);

    sendScale(false);
}

void LLPanelObject::onPasteRot(bool from_system_clipboard)
{
    LLVector3 value;
    if (!resolvePasteVector(mClipboardRot, mHasClipboardRot, from_system_clipboard, "rotation", value)) return;

    mCtrlRotX->set(value.mV[VX]);
    mCtrlRotY->set(value.mV[VY]);
    mCtrlRotZ->set(value.mV[VZ]);

    sendRotation(false);
}

void LLPanelObject::onCopyParams()
{
    LLViewerObject* objectp = mObject;
    if (!objectp || objectp->isMesh())
    {
        return;
    }

    mClipboardParams.clear();

    // Parametrics
    LLVolumeParams params;
    getVolumeParams(params);
    mClipboardParams["volume_params"] = params.asLLSD();

    // Sculpted Prim
    if (objectp->getParameterEntryInUse(LLNetworkData::PARAMS_SCULPT))
    {
        LLSculptParams *sculpt_params = (LLSculptParams *)objectp->getParameterEntry(LLNetworkData::PARAMS_SCULPT);
        if (sculpt_params)
        {
            LLUUID texture_id = sculpt_params->getSculptTexture();
            if (get_can_copy_texture(texture_id))
            {
                LL_DEBUGS("FloaterTools") << "Recording texture" << LL_ENDL;
                mClipboardParams["sculpt"]["id"] = texture_id;
            }
            else
            {
                mClipboardParams["sculpt"]["id"] = SCULPT_DEFAULT_TEXTURE;
            }

            mClipboardParams["sculpt"]["type"] = sculpt_params->getSculptType();
        }
    }
}

void LLPanelObject::onPasteParams()
{
    LLViewerObject* objectp = mObject;
    if (!objectp)
    {
        return;
    }

    // Sculpted Prim
    if (mClipboardParams.has("sculpt"))
    {
        LLSculptParams sculpt_params;
        LLUUID sculpt_id = mClipboardParams["sculpt"]["id"].asUUID();
        U8 sculpt_type = (U8)mClipboardParams["sculpt"]["type"].asInteger();
        sculpt_params.setSculptTexture(sculpt_id, sculpt_type);
        objectp->setParameterEntry(LLNetworkData::PARAMS_SCULPT, sculpt_params, true);
    }
    else
    {
        LLSculptParams *sculpt_params = (LLSculptParams *)objectp->getParameterEntry(LLNetworkData::PARAMS_SCULPT);
        if (sculpt_params)
        {
            objectp->setParameterEntryInUse(LLNetworkData::PARAMS_SCULPT, false, true);
        }
    }

    // volume params
    // make sure updateVolume() won't affect flexible
    if (mClipboardParams.has("volume_params"))
    {
        LLVolumeParams params;
        params.fromLLSD(mClipboardParams["volume_params"]);
        LLVOVolume *volobjp = (LLVOVolume *)objectp;
        if (volobjp->isFlexible())
        {
            if (params.getPathParams().getCurveType() == LL_PCODE_PATH_LINE)
            {
                params.getPathParams().setCurveType(LL_PCODE_PATH_FLEXIBLE);
            }
        }
        else if (params.getPathParams().getCurveType() == LL_PCODE_PATH_FLEXIBLE)
        {
            params.getPathParams().setCurveType(LL_PCODE_PATH_LINE);
        }

        objectp->updateVolume(params);
    }
}
