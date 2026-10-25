/**
 * @file slposeik.cpp
 * @brief Pose Studio IK handles (#530). See slposeik.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
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
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "slposeik.h"

#include "llagentcamera.h"
#include "llavatarjoint.h"
#include "lldir.h"
#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "lljoint.h"
#include "llkeyboard.h"
#include "llrender.h"
#include "m3math.h"
#include "llsdserialize.h"
#include "lltool.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"
#include "llvoavatar.h"

#include <cmath>
#include <functional>

namespace
{
constexpr F32 MIN_PICK_PX = 9.f;            // pick radius floor, UI pixels (follows UI scale)
constexpr F32 DRAG_ROTATE_RAD_PER_PX = 0.01f;
constexpr F32 AXIS_LOCK_PX = 4.f;
constexpr S32 TWOBONE_ITERATIONS = 4;
constexpr S32 CCD_ITERATIONS = 10;
constexpr S32 CCD_REFINE_ITERATIONS = 4;    // after a limited multi-bone solve
constexpr F32 CCD_MAX_STEP_RAD = 25.f * DEG_TO_RAD; // per joint per pass: no flailing
constexpr S32 FABRIK_ITERATIONS = 24;
constexpr F32 REACHED_M = 0.0005f;
constexpr F32 HOLD_EPSILON_M = 0.0005f;
constexpr F32 HOLD_EPSILON_DOT = 0.99999f;
constexpr F32 BONE_CHANGED_DOT = 0.99995f;
constexpr F32 LIMIT_REPRODUCED_DOT = 0.99999f; // ~0.5 degrees: the dials show this rotation
constexpr S32 LIMIT_BISECTIONS = 10;
constexpr F32 LOOKAT_DISTANCE = 0.35f;      // metres in front of the head (scaled with the avatar)
constexpr F32 BASE_HANDLE_LENGTH = 0.075f;  // metres (scaled with the avatar and the setting)
constexpr S32 RING_SEGMENTS = 48;
// Rotate rings, after LLManipRotate: screen-size radius, a generous pick band, edge-on
// below 85 degrees from the view (decided once at mouse down).
constexpr F32 RING_RADIUS_PX = 80.f;
constexpr F32 RING_WIDTH_PX = 2.5f;         // half width of the drawn band
constexpr F32 RING_PICK_PX = 10.f;
const F32 RING_EDGE_ON_DOT = cosf(85.f * DEG_TO_RAD);
constexpr F32 RING_SNAP_DEG = 15.f;         // Shift while turning a ring
// Translate gizmo, after LLManipTranslate: screen-size arrows and plane squares (UI pixels).
constexpr F32 MOVE_ARROW_START_PX = 14.f;
constexpr F32 MOVE_ARROW_LENGTH_PX = 72.f;
constexpr F32 MOVE_HEAD_LENGTH_PX = 14.f;
constexpr F32 MOVE_HEAD_RADIUS_PX = 5.f;
constexpr F32 MOVE_SHAFT_PX = 1.5f;         // half width
constexpr F32 MOVE_PICK_PX = 8.f;
constexpr F32 MOVE_PLANE_LO_PX = 20.f;
constexpr F32 MOVE_PLANE_HI_PX = 38.f;
constexpr F32 MOVE_ARROW_MAX_DOT = 0.985f;  // an arrow pointing at the camera is hidden
constexpr F32 MOVE_PLANE_MIN_DOT = 0.12f;   // an edge-on square is hidden
constexpr F32 POLE_MIN_BEND_DEG = 10.f;     // a set pole keeps at least this much bend to point with
constexpr F32 POLE_DISTANCE = 0.5f;         // pole diamond: this many limb lengths in front of the joint
constexpr F32 TIPS_POLL_SECONDS = 0.5f;     // rigging-key poll for the handle points
constexpr F32 TIP_EXTREMITY = 0.75f;        // handle point: this far from a rigged box's center to its far face

constexpr const char* FLOATER_NAME = "sl_pose_studio";

bool nearlySameRotation(const LLQuaternion& a, const LLQuaternion& b, F32 min_dot)
{
    return llabs(dot(a, b)) >= min_dot;
}

LLVector3 perpendicular(const LLVector3& v)
{
    LLVector3 p = (llabs(v.mV[VX]) < 0.9f) ? (v % LLVector3::x_axis) : (v % LLVector3::y_axis);
    p.normVec();
    return p;
}

// The rotation is one the bone's dials show, every dial inside its range.
bool withinLimits(const SLBoneModel& model, const LLQuaternion& local, const SLPoseAngles& reference)
{
    LLQuaternion target(local);
    target.normalize();
    const SLPoseAngles dial = model.fromQuat(target, reference);
    if (llabs(dot(model.toQuat(dial), target)) < LIMIT_REPRODUCED_DOT)
        return false;
    for (S32 axis = 0; axis < 3; ++axis)
    {
        const F32 v = dial.get(axis);
        if (v < model.mMin.mV[axis] - 0.01f || v > model.mMax.mV[axis] + 0.01f)
            return false;
    }
    return true;
}

LLVector3 readVector(const LLSD& sd, const LLVector3& fallback)
{
    if (!sd.isArray() || sd.size() < 3)
        return fallback;
    return LLVector3((F32)sd[0].asReal(), (F32)sd[1].asReal(), (F32)sd[2].asReal());
}
}

// Holds the mouse while a handle is dragged: hover and mouse-up come here through the
// focus manager's mouse capture, whatever tool is current.
class SLPoseIKTool : public LLTool
{
public:
    SLPoseIKTool() : LLTool("SLPoseIK") {}

    bool handleHover(S32 x, S32 y, MASK mask) override
    {
        if (!hasMouseCapture())
            return false;
        SLPoseIK::instance().onDragHover(x, y, mask);
        gViewerWindow->setCursor(UI_CURSOR_TOOLTRANSLATE);
        return true;
    }

    bool handleMouseUp(S32 x, S32 y, MASK mask) override
    {
        if (hasMouseCapture())
        {
            SLPoseIK::instance().onDragHover(x, y, mask);
            setMouseCapture(false);     // ends the drag in onMouseCaptureLost
        }
        return true;
    }

    bool handleMouseDown(S32, S32, MASK) override { return true; }
    bool handleRightMouseDown(S32, S32, MASK) override { return true; }
    bool handleRightMouseUp(S32, S32, MASK) override { return true; }
    bool clipMouseWhenDown() override { return false; }

    void onMouseCaptureLost() override
    {
        if (SLPoseIK::instanceExists())
            SLPoseIK::instance().onDragEnd(false);
    }
};

SLPoseIK::SLPoseIK()
{
    loadChains();
    mTool = new SLPoseIKTool();
    SLPoseStudio& studio = SLPoseStudio::instance();
    mEditedConnection = studio.setEditedCallback([this](const LLUUID& id, bool committed) { onStudioEdited(id, committed); });
    mPosingConnection = studio.setPosingCallback([this](const LLUUID& id, bool posing) { onStudioPosing(id, posing); });
}

SLPoseIK::~SLPoseIK()
{
    if (mTool && mTool->hasMouseCapture())
        gFocusMgr.setMouseCapture(nullptr);
}

// ---------------------------------------------------------------------------
// Chains
// ---------------------------------------------------------------------------

void SLPoseIK::loadChains()
{
    mChains.clear();
    const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "posestudio", "ik_chains.xml");
    llifstream infile(path.c_str());
    LLSD data;
    if (!infile.is_open() || LLSDSerialize::fromXML(data, infile) == LLSDParser::PARSE_FAILURE || !data.isArray())
    {
        LL_WARNS("PoseStudio") << "Cannot read " << path << "; no IK handles" << LL_ENDL;
        return;
    }

    for (const LLSD& entry : llsd::inArray(data))
    {
        Chain chain;
        chain.mName = entry["name"].asString();
        const std::string solver = entry["solver"].asString();
        if (solver == "twobone")
            chain.mSolver = SOLVER_TWOBONE;
        else if (solver == "lookat")
            chain.mSolver = SOLVER_LOOKAT;
        else if (solver == "pelvis")
            chain.mSolver = SOLVER_PELVIS;
        else
            chain.mSolver = SOLVER_CCD;
        for (const LLSD& joint : llsd::inArray(entry["joints"]))
            chain.mJoints.push_back(joint.asString());
        for (const LLSD& tip : llsd::inArray(entry["tip"]))
        {
            if (SLPoseStudio::instance().findJoint(tip.asString()))
                chain.mTip.push_back(tip.asString());
            else
                LL_WARNS("PoseStudio") << "IK chain " << chain.mName << ": unknown tip joint " << tip.asString() << LL_ENDL;
        }
        chain.mHandBranches = llmax(0, entry["hand_branches"].asInteger());
        if (entry.has("hand_depth"))
            chain.mHandDepth = llmax(0, entry["hand_depth"].asInteger());
        chain.mMirror = entry["mirror"].asString();
        chain.mBento = entry["bento"].asBoolean();
        chain.mGroup = entry["group"].asString();
        chain.mRigged = entry["rigged"].asBoolean();
        if (entry.has("color"))
        {
            const LLVector3 rgb = readVector(entry["color"], LLVector3(0.4f, 0.8f, 1.f));
            chain.mColor = LLColor4(rgb.mV[VX], rgb.mV[VY], rgb.mV[VZ], 1.f);
        }
        chain.mForward = readVector(entry["forward"], chain.mForward);
        chain.mForward.normVec();
        chain.mBend = readVector(entry["bend"], chain.mBend);
        chain.mBend.normVec();
        for (const LLSD& share : llsd::inArray(entry["share"]))
            chain.mShare.push_back(llclamp((F32)share.asReal(), 0.f, 1.f));
        chain.mExtend = llclamp(entry["extend"].asInteger(), 0, 4);
        if (entry.has("max_bend"))
            chain.mMaxBend = llclamp((F32)entry["max_bend"].asReal(), 10.f, 179.f);

        const size_t needed = (chain.mSolver == SOLVER_TWOBONE) ? 3 : (chain.mSolver == SOLVER_PELVIS ? 1 : 2);
        if (chain.mJoints.size() < needed)
        {
            LL_WARNS("PoseStudio") << "IK chain " << chain.mName << ": wrong joint count, skipped" << LL_ENDL;
            continue;
        }
        bool known = true;
        for (const std::string& joint : chain.mJoints)
            known = known && SLPoseStudio::instance().findJoint(joint) != nullptr;
        if (!known)
        {
            LL_WARNS("PoseStudio") << "IK chain " << chain.mName << ": a joint Pose Studio cannot pose, skipped" << LL_ENDL;
            continue;
        }
        mChains.push_back(chain);
    }
    mStates.assign(mChains.size(), ChainState());
    LL_INFOS("PoseStudio") << "Loaded " << mChains.size() << " IK chains" << LL_ENDL;
}

bool SLPoseIK::chainUsable(LLVOAvatar* avatar, S32 chain) const
{
    if (!avatar || chain < 0 || chain >= (S32)mChains.size())
        return false;
    const Chain& c = mChains[chain];
    static LLCachedControl<bool> bento(gSavedSettings, "SLPoseStudioIKBentoChains", false);
    if (c.mGroup == "wings")
    {
        static LLCachedControl<bool> wings(gSavedSettings, "SLPoseStudioIKWings", true);
        if (!wings && !bento)
            return false;
    }
    else if (c.mBento && !bento)
    {
        return false;
    }
    // Only where the outfit has it: no wing / tail handles on an avatar without them.
    if (c.mRigged && (!mTipsValid || mTipsAvatar != avatar->getID() || chain >= (S32)mTips.size() || !mTips[chain].mBound))
        return false;
    for (const std::string& joint : c.mJoints)
    {
        if (!avatar->getJoint(joint))
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

LLVOAvatar* SLPoseIK::getActiveAvatar() const
{
    static LLCachedControl<bool> enabled(gSavedSettings, "SLPoseStudioIKHandles", true);
    if (!enabled || mChains.empty())
        return nullptr;
    if (!LLFloaterReg::instanceVisible(FLOATER_NAME))
        return nullptr;
    if (gAgentCamera.cameraMouselook())
        return nullptr;
    SLPoseStudio& studio = SLPoseStudio::instance();
    LLVOAvatar* avatar = studio.getTarget();
    // Only ever the studio's own target, posed by us, and only while we may pose it:
    // never an avatar that merely shows a synced pose.
    if (!avatar || !studio.isPosing(avatar) || !studio.canPose(avatar) || !studio.isPosedByStudio(avatar->getID()))
        return nullptr;
    return avatar;
}

bool SLPoseIK::isActive() const
{
    return getActiveAvatar() != nullptr;
}

void SLPoseIK::syncAvatar(LLVOAvatar* avatar)
{
    if (avatar && avatar->getID() != mAvatarID)
    {
        reset();
        mAvatarID = avatar->getID();
    }
}

void SLPoseIK::reset()
{
    if (mDrag.mActive && mTool && mTool->hasMouseCapture())
        gFocusMgr.setMouseCapture(nullptr);   // ends the drag (onDragEnd)
    mDrag = DragState();
    mStates.assign(mChains.size(), ChainState());
    mUndo.clear();
    mRedo.clear();
    mHoverChain = -1;
    mHoverPole = false;
    mHoverRing = -1;
    mSelectedChain = -1;
    mAvatarID.setNull();
    mTipsValid = false;     // re-derived (and logged) the next time the handles show
}

F32 SLPoseIK::handleScale(LLVOAvatar* avatar) const
{
    static LLCachedControl<F32> size(gSavedSettings, "SLPoseStudioIKHandleSize", 1.f);
    F32 body = avatar ? avatar->mBodySize.mV[VZ] : 0.f;
    if (body <= 0.1f)
        body = 1.9f;
    return llclamp(body / 1.9f, 0.25f, 4.f) * llclamp((F32)size, 0.25f, 4.f);
}

// ---------------------------------------------------------------------------
// Forward kinematics on the targets
// ---------------------------------------------------------------------------

bool SLPoseIK::fkWorld(LLVOAvatar* avatar, LLJoint* joint, LLVector3& pos, LLQuaternion& rot) const
{
    if (!avatar || !joint)
        return false;
    SLPoseStudio& studio = SLPoseStudio::instance();
    LLJoint* parent = joint->getParent();
    const SLPoseStudio::joint_t* poser_joint = studio.findJoint(joint->getName());
    if (!parent || !poser_joint)
    {
        // Above the posed skeleton (the avatar root): where it is now.
        pos = joint->getWorldPosition();
        rot = joint->getWorldRotation();
        return true;
    }

    LLVector3 parent_pos;
    LLQuaternion parent_rot;
    fkWorld(avatar, parent, parent_pos, parent_rot);

    FSPoserAnimator& animator = studio.getAnimator();
    LLQuaternion local_rot;
    LLVector3 local_pos, local_scale;
    if (!animator.isPosingAvatarJoint(avatar, *poser_joint)
        || !animator.getJointTargetTransform(avatar, *poser_joint, local_rot, local_pos, local_scale))
    {
        // Not held by the pose: the animation plays on it.
        local_rot = joint->getRotation();
        local_pos = joint->getPosition();
    }
    else
    {
        // Easing in (an eased edit, undo, pose load): the rotation it is going to.
        LLQuaternion blend_rot;
        LLVector3 blend_pos, blend_scale;
        if (studio.getBlendTarget(avatar, *poser_joint, blend_rot, blend_pos, blend_scale))
            local_rot = blend_rot;
    }
    // LLXformMatrix::update, on the targets.
    if (parent->getXform()->getScaleChildOffset())
        local_pos.scaleVec(parent->getScale());
    pos = parent_pos + local_pos * parent_rot;
    rot = local_rot * parent_rot;
    rot.normalize();
    return true;
}

bool SLPoseIK::fkJoint(LLVOAvatar* avatar, const std::string& name, LLVector3& pos, LLQuaternion& rot) const
{
    return avatar && fkWorld(avatar, avatar->getJoint(name), pos, rot);
}

bool SLPoseIK::fkParent(LLVOAvatar* avatar, const std::string& name, LLVector3& pos, LLQuaternion& rot) const
{
    LLJoint* joint = avatar ? avatar->getJoint(name) : nullptr;
    return joint && joint->getParent() && fkWorld(avatar, joint->getParent(), pos, rot);
}

void SLPoseIK::updateTips(LLVOAvatar* avatar)
{
    // Never mid-drag: the solve depends on the handle's offset staying put.
    if (!avatar || mDrag.mActive)
        return;
    const bool same_avatar = mTipsValid && mTipsAvatar == avatar->getID();
    if (same_avatar && mTipsTimer.getElapsedTimeF32() < TIPS_POLL_SECONDS)
        return;
    mTipsTimer.reset();
    // The avatar's rigging info: per joint, whether a worn rigged mesh weights vertices to it
    // (weight > 0.2) and the box of those vertices in the joint's space. Keyed by the worn
    // meshes and their LODs, so this is cheap while the outfit is unchanged.
    avatar->updateRiggingInfo();
    if (same_avatar && mTipsKey == avatar->mLastRiggingInfoKey)
        return;

    const std::vector<TipInfo> previous = same_avatar ? mTips : std::vector<TipInfo>();
    std::vector<bool> tip_changed(mChains.size(), false);
    mTipsValid = true;
    mTipsAvatar = avatar->getID();
    mTipsKey = avatar->mLastRiggingInfoKey;
    mTips.assign(mChains.size(), TipInfo());

    SLPoseStudio& studio = SLPoseStudio::instance();
    const LLJointRiggingInfoTab& tab = avatar->mJointRiggingInfoTab;
    auto rigged = [&tab](const LLJoint* joint)
    {
        const S32 num = joint->getJointNum();
        return num >= 0 && num < tab.size() && tab[num].isRiggedTo();
    };
    auto is_volume = [](LLJoint* joint) { return dynamic_cast<LLAvatarJointCollisionVolume*>(joint) != nullptr; };
    // Skeleton children only: bones Pose Studio knows and collision volumes (fitted mesh),
    // never attachment points.
    auto children = [&](LLJoint* joint)
    {
        std::vector<LLJoint*> out;
        for (LLJoint* child : joint->mChildren)
        {
            if (child && (is_volume(child) || studio.findJoint(child->getName())))
                out.push_back(child);
        }
        return out;
    };
    std::function<bool(LLJoint*)> subtree_rigged = [&](LLJoint* joint) -> bool
    {
        if (rigged(joint))
            return true;
        for (LLJoint* child : children(joint))
        {
            if (subtree_rigged(child))
                return true;
        }
        return false;
    };

    for (S32 i = 0; i < (S32)mChains.size(); ++i)
    {
        const Chain& c = mChains[i];
        TipInfo& tip = mTips[i];
        LLJoint* root = avatar->getJoint(c.mJoints.front());
        LLJoint* end = avatar->getJoint(c.mJoints.back());
        if (!root || !end)
            continue;
        for (const std::string& name : c.mJoints)
        {
            LLJoint* joint = avatar->getJoint(name);
            tip.mBound = tip.mBound || (joint && rigged(joint));
        }
        tip.mBound = tip.mBound || subtree_rigged(end);
        if (c.mSolver == SOLVER_LOOKAT || c.mSolver == SOLVER_PELVIS)
            continue;

        // A hand (several rigged fingers): stay near the knuckles, not on a fingertip.
        S32 branches = 0;
        for (LLJoint* child : children(end))
        {
            if (!is_volume(child) && subtree_rigged(child))
                ++branches;
        }
        const bool hand = c.mHandBranches > 0 && branches >= c.mHandBranches;
        const S32 max_depth = hand ? c.mHandDepth : S32_MAX;

        // The farthest rigged region at / below the end joint, measured from the chain's
        // root: its box (rendered space, joint-position overrides and scale included) is
        // pushed toward its far face along the root -> box direction, so a hoof's handle
        // sits at the hoof, a toe's at the toes. A collision volume's region belongs to
        // its bone.
        const LLVector3 root_pos = root->getWorldPosition();
        LLJoint* best_joint = nullptr;
        LLVector3 best_point;
        F32 best_reach = -1.f;
        S32 candidates = 0;
        std::vector<std::pair<LLJoint*, S32>> stack(1, std::make_pair(end, 0));
        while (!stack.empty())
        {
            const auto [joint, depth] = stack.back();
            stack.pop_back();
            if (depth < max_depth && !is_volume(joint))
            {
                for (LLJoint* child : children(joint))
                    stack.emplace_back(child, depth + 1);
            }
            if (!rigged(joint))
                continue;
            LLJoint* owner = is_volume(joint) ? joint->getParent() : joint;
            if (!owner || !studio.findJoint(owner->getName()))
                continue;
            ++candidates;
            const LLVector4a* extents = tab[joint->getJointNum()].getRiggedExtents();
            const LLMatrix4& world = joint->getWorldMatrix();
            LLVector3 corners[8];
            LLVector3 center;
            for (S32 k = 0; k < 8; ++k)
            {
                const LLVector3 local(extents[k & 1][0], extents[(k >> 1) & 1][1], extents[(k >> 2) & 1][2]);
                corners[k] = local * world;
                center += corners[k];
            }
            center *= 1.f / 8.f;
            LLVector3 dir = center - root_pos;
            if (dir.length() < 1.0e-4f)
                continue;
            dir.normVec();
            F32 support = 0.f;
            for (const LLVector3& corner : corners)
                support = llmax(support, (corner - center) * dir);
            const LLVector3 point = center + dir * (TIP_EXTREMITY * support);
            const F32 reach = (point - root_pos).length();
            if (reach > best_reach)
            {
                best_reach = reach;
                best_joint = owner;
                best_point = point;
            }
        }

        std::string why;
        if (best_joint)
        {
            tip.mJoint = best_joint->getName();
            tip.mOffset = (best_point - best_joint->getWorldPosition()) * ~best_joint->getWorldRotation();
            why = llformat("worn rigged mesh: farthest of %d rigged regions at/below %s", candidates, c.mJoints.back().c_str());
            if (hand)
                why += llformat("; a hand (%d rigged finger branches), searched %d joint(s) below the wrist", branches, max_depth);
        }
        else
        {
            for (const std::string& name : c.mTip)
            {
                if (avatar->getJoint(name))
                {
                    tip.mJoint = name;
                    break;
                }
            }
            why = "nothing rigged at/below " + c.mJoints.back() + ": fallback tip list";
        }

        // A hoof (or any handle point) below the end joint of a chain that is not a hand:
        // the chain grows toward it (up to mExtend joints), so the joint carrying the hoof
        // takes the handle's orientation and every joint above it -- the wrist / cannon
        // included -- folds when the limb has to shorten. A human hand keeps the wrist as
        // its end (the fingers are not limb segments). Only from a worn rigged mesh: without
        // one (system avatar, mesh still loading) nothing says whether the end is a hand.
        if (best_joint && c.mExtend > 0 && !hand && !tip.mJoint.empty() && tip.mJoint != c.mJoints.back())
        {
            std::vector<std::string> path;     // tip joint up to (not including) the end
            LLJoint* walk = avatar->getJoint(tip.mJoint);
            while (walk && walk != end && studio.findJoint(walk->getName()))
            {
                path.push_back(walk->getName());
                walk = walk->getParent();
            }
            if (walk == end && !path.empty())
            {
                tip.mJoints = c.mJoints;
                for (S32 k = (S32)path.size() - 1; k >= 0 && (S32)(tip.mJoints.size() - c.mJoints.size()) < c.mExtend; --k)
                    tip.mJoints.push_back(path[k]);
            }
        }

        const bool changed = previous.size() != mTips.size() || previous[i].mJoint != tip.mJoint
                             || previous[i].mBound != tip.mBound || (previous[i].mOffset - tip.mOffset).length() > 0.01f
                             || previous[i].mJoints != tip.mJoints;
        if (changed)
        {
            tip_changed[i] = true;
            std::string solves;
            for (const std::string& name : (tip.mJoints.empty() ? c.mJoints : tip.mJoints))
                solves += (solves.empty() ? "" : " -> ") + name;
            if (c.mSolver == SOLVER_TWOBONE)
                solves += "; pole bends " + c.mJoints[1];
            LLVector3 handle_pos = end->getWorldPosition();
            if (LLJoint* joint = tip.mJoint.empty() ? nullptr : avatar->getJoint(tip.mJoint))
                handle_pos = joint->getWorldPosition() + tip.mOffset * joint->getWorldRotation();
            LL_INFOS("PoseStudio") << "IK chain " << c.mName << ": handle on " << (tip.mJoint.empty() ? c.mJoints.back() : tip.mJoint)
                                   << ", " << (handle_pos - end->getWorldPosition()).length() << " m from " << c.mJoints.back()
                                   << " (" << why << "); solves " << solves
                                   << ((c.mRigged && !tip.mBound) ? "; hidden: no worn rigged mesh binds this chain" : "")
                                   << LL_ENDL;
        }
    }
    // A handle point moved (outfit or mesh LOD changed): a target still on its bones goes
    // to the new point; one the user placed stays exactly where it was put.
    if (same_avatar)
    {
        for (S32 i = 0; i < (S32)mChains.size() && i < (S32)mStates.size(); ++i)
        {
            if (!tip_changed[i] || !usesTarget(i) || mStates[i].mPlaced || !chainUsable(avatar, i))
                continue;
            snapTarget(avatar, i);
            mStates[i].mHasPole = false;
            if (mStates[i].mPinned)
                mStates[i].mPinBones = chainTargets(avatar, i);
        }
    }
}

const std::vector<std::string>& SLPoseIK::chainJoints(S32 chain) const
{
    if (chain >= 0 && chain < (S32)mTips.size() && !mTips[chain].mJoints.empty())
        return mTips[chain].mJoints;
    return mChains[chain].mJoints;
}

bool SLPoseIK::fkEffector(LLVOAvatar* avatar, S32 chain, LLVector3& tip_pos, LLVector3& end_pos, LLQuaternion& end_rot) const
{
    if (!fkJoint(avatar, chainJoints(chain).back(), end_pos, end_rot))
        return false;
    tip_pos = end_pos;
    if (hasTip(chain))
    {
        LLVector3 joint_pos;
        LLQuaternion joint_rot;
        if (fkJoint(avatar, mTips[chain].mJoint, joint_pos, joint_rot))
            tip_pos = joint_pos + mTips[chain].mOffset * joint_rot;
    }
    return true;
}

void SLPoseIK::captureDragStart(LLVOAvatar* avatar)
{
    mDrag.mStart.clear();
    SLPoseStudio& studio = SLPoseStudio::instance();
    FSPoserAnimator& animator = studio.getAnimator();
    // Every bone a drag can set: all chains (pins hold theirs) and, for mirror mode, the
    // opposite of each.
    std::vector<std::string> names;
    for (S32 i = 0; i < (S32)mChains.size(); ++i)
    {
        if (!chainUsable(avatar, i))
            continue;
        for (const std::string& name : chainJoints(i))
        {
            names.push_back(name);
            const SLPoseStudio::joint_t* joint = studio.findJoint(name);
            if (joint && !joint->mirrorJointName().empty())
                names.push_back(joint->mirrorJointName());
        }
    }
    for (const std::string& name : names)
    {
        if (mDrag.mStart.count(name))
            continue;
        const SLPoseStudio::joint_t* joint = studio.findJoint(name);
        LLJoint* lljoint = avatar->getJoint(name);
        if (!joint || !lljoint)
            continue;
        StartBone bone;
        // Exactly what fkWorld reads.
        bone.mPosed = animator.isPosingAvatarJoint(avatar, *joint) && animator.getJointTargetRotation(avatar, *joint, bone.mLocal);
        if (!bone.mPosed)
            bone.mLocal = lljoint->getRotation();
        bone.mLocal.normalize();
        if (!studio.getRememberedAngles(avatar->getID(), name, bone.mReference))
            studio.getBoneAngles(avatar, *joint, bone.mReference);
        mDrag.mStart[name] = bone;
    }
}

void SLPoseIK::restoreDragStart(LLVOAvatar* avatar)
{
    // Silently (no edited signal, no mirror, no ease): only the solve that follows is an edit.
    SLPoseStudio& studio = SLPoseStudio::instance();
    FSPoserAnimator& animator = studio.getAnimator();
    for (const auto& [name, bone] : mDrag.mStart)
    {
        const SLPoseStudio::joint_t* joint = studio.findJoint(name);
        if (!joint)
            continue;
        // Played by the animation then and untouched since: leave it to the animation.
        if (!bone.mPosed && !animator.isPosingAvatarJoint(avatar, *joint))
            continue;
        animator.setJointTargetRotation(avatar, *joint, bone.mLocal);
    }
}

void SLPoseIK::startOf(LLVOAvatar* avatar, const std::string& name, LLQuaternion& local, SLPoseAngles& reference) const
{
    if (mDrag.mActive)
    {
        auto it = mDrag.mStart.find(name);
        if (it != mDrag.mStart.end())
        {
            local = it->second.mLocal;
            reference = it->second.mReference;
            return;
        }
    }
    SLPoseStudio& studio = SLPoseStudio::instance();
    const SLPoseStudio::joint_t* joint = studio.findJoint(name);
    LLJoint* lljoint = avatar ? avatar->getJoint(name) : nullptr;
    local = lljoint ? lljoint->getRotation() : LLQuaternion::DEFAULT;
    reference = SLPoseAngles();
    if (!joint || !avatar)
        return;
    LLQuaternion target;
    if (studio.getAnimator().isPosingAvatarJoint(avatar, *joint) && studio.getAnimator().getJointTargetRotation(avatar, *joint, target))
        local = target;
    if (!studio.getRememberedAngles(avatar->getID(), name, reference))
        studio.getBoneAngles(avatar, *joint, reference);
}

bool SLPoseIK::applyWorldRotation(LLVOAvatar* avatar, const std::string& name, const LLQuaternion& world,
                                  const LLQuaternion& parent_world, bool limits)
{
    SLPoseStudio& studio = SLPoseStudio::instance();
    const SLPoseStudio::joint_t* joint = studio.findJoint(name);
    if (!joint)
        return false;

    // LL order: world = local * parent_world.
    LLQuaternion local = world * ~parent_world;
    local.normalize();
    if (!limits)
        return studio.setBoneRotation(avatar, *joint, local, false);

    // Limits through the dial model (bone_axes.xml ranges). Clamping each decoded dial on
    // its own is not continuous (the swing azimuth flips 180 degrees through the model's
    // poles, equivalent triples swap branches), so a clamped solve could land anywhere:
    // instead the bone turns from where it started toward the solved rotation and stops at
    // the last rotation still inside every range. The decode reference is fixed per drag.
    const SLBoneModel& model = studio.getBoneModel(name);
    LLQuaternion from;
    SLPoseAngles reference;
    startOf(avatar, name, from, reference);
    if (withinLimits(model, local, reference))
        return studio.setBoneRotation(avatar, *joint, local, false);
    // Already outside its ranges before the drag (a loaded pose, a non-human rig's own
    // stance): there is nothing to stop at continuously, so it is not limited.
    if (!withinLimits(model, from, reference))
        return studio.setBoneRotation(avatar, *joint, local, false);

    F32 inside = 0.f;
    F32 outside = 1.f;
    for (S32 i = 0; i < LIMIT_BISECTIONS; ++i)
    {
        const F32 mid = 0.5f * (inside + outside);
        if (withinLimits(model, slerp(mid, from, local), reference))
            inside = mid;
        else
            outside = mid;
    }
    LLQuaternion limited = slerp(inside, from, local);
    limited.normalize();
    return studio.setBoneRotation(avatar, *joint, limited, false);
}

std::vector<LLQuaternion> SLPoseIK::chainTargets(LLVOAvatar* avatar, S32 chain) const
{
    std::vector<LLQuaternion> out;
    SLPoseStudio& studio = SLPoseStudio::instance();
    for (const std::string& name : chainJoints(chain))
    {
        LLQuaternion rot;
        const SLPoseStudio::joint_t* joint = studio.findJoint(name);
        if (!joint || !studio.getAnimator().getJointTargetRotation(avatar, *joint, rot))
            rot = LLQuaternion::DEFAULT;
        out.push_back(rot);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Solvers
// ---------------------------------------------------------------------------

void SLPoseIK::solveChain(LLVOAvatar* avatar, S32 chain, const LLVector3& goal, const LLQuaternion* end_rot)
{
    static LLCachedControl<bool> limits_setting(gSavedSettings, "SLPoseStudioIKLimits", true);
    const bool limits = limits_setting;

    // The goal is for the tip (where the handle is); the solvers move the end joint. The
    // bones between end and tip are not solved, so the tip keeps its offset in the end
    // bone's frame: the end goes where that offset puts the tip on the goal.
    LLVector3 end_goal = goal;
    if (mChains[chain].mSolver != SOLVER_LOOKAT && hasTip(chain))
    {
        LLVector3 tip_pos, end_pos;
        LLQuaternion end_now;
        if (fkEffector(avatar, chain, tip_pos, end_pos, end_now))
        {
            const LLVector3 offset_local = (tip_pos - end_pos) * ~end_now;
            end_goal = goal - offset_local * (end_rot ? *end_rot : end_now);
        }
    }

    switch (mChains[chain].mSolver)
    {
        case SOLVER_TWOBONE:
        case SOLVER_CCD:
            // Two bones: analytic, exact. Longer chains (a pony leg grown to its hoof, the
            // tail) and single bones: FABRIK, every joint taking part.
            if (chainJoints(chain).size() == 3)
                solveTwoBone(avatar, chain, end_goal, end_rot, limits);
            else
                solveMulti(avatar, chain, end_goal, end_rot, limits);
            break;
        case SOLVER_LOOKAT:
            solveLookAt(avatar, chain, goal, end_rot, limits);
            break;
        case SOLVER_PELVIS:
        default:
            break;  // the pelvis is moved directly by its drag
    }
}

bool SLPoseIK::limitJoint(S32 chain, S32 index) const
{
    // Limb hinges (elbow, knee, a pony's cannon) are NOT held to their dial ranges: those
    // are the pose HUD's dial ranges, not anatomy (the knee's bend dial stops at 60
    // degrees, the elbow's at 90), and holding a hinge to them left the shoulder / hip to
    // do all the work while the limb could not shorten. Two-bone hinges are capped
    // geometrically by Chain::mMaxBend instead; the limb's root (a ball joint) keeps its
    // dial ranges, and so does every joint of a CCD chain (tail, ears, wings).
    if (mChains[chain].mSolver == SOLVER_CCD)
        return true;
    return index == 0;
}

void SLPoseIK::setEndRotation(LLVOAvatar* avatar, S32 chain, const LLQuaternion& end_rot, bool limits)
{
    const std::string& end = chainJoints(chain).back();
    LLVector3 parent_pos;
    LLQuaternion parent_rot;
    if (fkParent(avatar, end, parent_pos, parent_rot))
        applyWorldRotation(avatar, end, end_rot, parent_rot, limits);
}

void SLPoseIK::solveTwoBone(LLVOAvatar* avatar, S32 chain, const LLVector3& goal, const LLQuaternion* end_rot, bool limits)
{
    const Chain& c = mChains[chain];
    const ChainState& state = mStates[chain];
    const std::vector<std::string>& joints = chainJoints(chain);
    const std::string& root = joints[0];
    const std::string& mid = joints[1];
    const std::string& end = joints[2];
    const LLVector3 pole = state.mHasPole ? poleWorld(avatar, chain) : LLVector3::zero;
    const bool limit_root = limits && limitJoint(chain, 0);
    const bool limit_mid = limits && limitJoint(chain, 1);

    for (S32 iteration = 0; iteration < TWOBONE_ITERATIONS; ++iteration)
    {
        LLVector3 a, b, e, pa_pos;
        LLQuaternion wa, wb, we, pa;
        if (!fkJoint(avatar, root, a, wa) || !fkJoint(avatar, mid, b, wb) || !fkJoint(avatar, end, e, we)
            || !fkParent(avatar, root, pa_pos, pa))
            return;

        // Already there (e.g. turning the end bone in place) and no pole asking for a
        // bend: only the end rotation is asked for.
        if (!state.mHasPole && (e - goal).length() < REACHED_M)
        {
            if (end_rot)
                setEndRotation(avatar, chain, *end_rot, limits);
            LLVector3 e_now;
            LLQuaternion we_now;
            if (!fkJoint(avatar, end, e_now, we_now) || (e_now - goal).length() < REACHED_M)
                break;
            continue;
        }
        const LLVector3 upper = b - a;
        const LLVector3 lower = e - b;
        const F32 la = upper.length();
        const F32 lb = lower.length();
        const LLVector3 at = goal - a;
        const F32 dt = at.length();
        if (la < 1.0e-4f || lb < 1.0e-4f || dt < 1.0e-4f)
            return;
        const LLVector3 n = at / dt;

        // Reach: out of range, the limb stretches straight toward the goal. Closer in, the
        // hinge folds up to mMaxBend from straight (its inner angle never closes below
        // 180 - mMaxBend). A set pole keeps a little bend, so the elbow / knee always has a
        // direction to point in.
        const F32 inner_min = (180.f - c.mMaxBend) * DEG_TO_RAD;
        const F32 folded = sqrtf(llmax(0.f, la * la + lb * lb - 2.f * la * lb * cosf(inner_min)));
        const F32 min_d = llmax(llabs(la - lb) + 1.0e-3f, folded);
        F32 max_d = la + lb - 1.0e-5f;
        if (state.mHasPole)
        {
            const F32 cos_bend = cosf(POLE_MIN_BEND_DEG * DEG_TO_RAD);
            max_d = llmin(max_d, sqrtf(la * la + lb * lb + 2.f * la * lb * cos_bend));
        }
        const F32 d = (min_d < max_d) ? llclamp(dt, min_d, max_d) : max_d;

        // The bend plane holds the root, the goal and the pole (Blender): the elbow / knee
        // goes to the pole's side of the root -> goal line. No pole: the side it points to
        // now; a straight limb: opposite where the chain's lower bone swings (mBend).
        const LLVector3 bend_hint = c.mBend * wa;
        LLVector3 side = state.mHasPole ? (pole - a) : upper;
        side -= n * (side * n);
        if (side.length() < 0.05f * la)
        {
            side = -bend_hint;
            side -= n * (side * n);
            if (side.length() < 1.0e-6f)
                side = perpendicular(n);
        }
        side.normVec();
        const F32 cos_a = llclamp((la * la + d * d - lb * lb) / (2.f * la * d), -1.f, 1.f);
        const F32 sin_a = sqrtf(llmax(0.f, 1.f - cos_a * cos_a));
        const LLVector3 knee = a + n * (la * cos_a) + side * (la * sin_a);
        const LLVector3 reach = a + n * d;

        // The root carries the upper bone AND its bend plane (upper x lower) onto the new
        // ones: the elbow / knee swings into the pole's plane and then only bends about its
        // hinge. (Twisting the whole limb about the root -> goal line instead only rolled
        // the upper bone when the limb was nearly straight, and the elbow never moved.)
        LLVector3 u0 = upper / la;
        LLVector3 w0 = u0 % lower;
        if (w0.length() < 0.05f * lb)
            w0 = u0 % bend_hint;
        if (w0.length() < 1.0e-6f)
            w0 = perpendicular(u0);
        w0.normVec();
        LLVector3 u1 = knee - a;
        u1.normVec();
        LLVector3 w1 = u1 % (reach - knee);
        if (w1.length() < 1.0e-4f * lb)
            w1 = side % n;
        w1.normVec();
        LLMatrix3 frame_old, frame_new;
        frame_old.setRows(u0, w0, u0 % w0);
        frame_new.setRows(u1, w1, u1 % w1);
        LLQuaternion carry = ~LLQuaternion(frame_old) * LLQuaternion(frame_new);
        carry.normalize();
        applyWorldRotation(avatar, root, wa * carry, pa, limit_root);

        // The middle joint: the lower bone onto knee -> reach, from where the (possibly
        // limited) root actually put it -- a turn in the bend plane, i.e. about the hinge.
        LLVector3 a_now, b_now, e_now;
        LLQuaternion wa_now, wb_now, we_now;
        if (!fkJoint(avatar, root, a_now, wa_now) || !fkJoint(avatar, mid, b_now, wb_now) || !fkJoint(avatar, end, e_now, we_now))
            return;
        if ((e_now - b_now).length() > 1.0e-6f && (reach - b_now).length() > 1.0e-6f)
        {
            LLQuaternion bend;
            bend.shortestArc(e_now - b_now, reach - b_now);
            applyWorldRotation(avatar, mid, wb_now * bend, wa_now, limit_mid);
        }

        if (end_rot)
            setEndRotation(avatar, chain, *end_rot, limits);

        if (!fkJoint(avatar, end, e_now, we_now) || (e_now - goal).length() < REACHED_M)
            break;
        if (!limit_root && !limit_mid && dt <= max_d)
            break;  // analytic: one pass is exact without clamping
    }
}

void SLPoseIK::solveMulti(LLVOAvatar* avatar, S32 chain, const LLVector3& goal, const LLQuaternion* end_rot, bool limits)
{
    // FABRIK on the joint positions, warm-started from the pose the chain is in (during a
    // drag: the pose at mouse down, see restoreDragStart), so the solution is a continuous
    // function of the goal and keeps the limb's own bend pattern (a pony hind leg's stifle
    // forward / hock back zig-zag). Every bone moves: the shortening is shared along the
    // whole chain instead of piling up at the root or curling the tip (CCD). Out of reach
    // the chain points straight at the goal. A pole turns the solved chain about the
    // root -> goal line until its first inner joint faces the pole (Blender). The positions
    // then become rotations root to end, each bone aimed at its solved child from where its
    // (already turned) parent put it, its bend plane carried along with no more twist than
    // needed (never a flip).
    const std::vector<std::string>& joints = chainJoints(chain);
    const S32 count = (S32)joints.size();   // count - 1 bones; the last joint is the end
    if (count < 2)
        return;
    const ChainState& state = mStates[chain];
    // Limited joints may stop short of their solved direction: solve again from there.
    const S32 passes = limits ? 3 : 1;
    auto toward = [](const LLVector3& v, const LLVector3& fallback)
    {
        const F32 l = v.length();
        return (l > 1.0e-6f) ? v * (1.f / l) : fallback;
    };

    for (S32 pass = 0; pass < passes; ++pass)
    {
        std::vector<LLVector3> p(count);
        for (S32 i = 0; i < count; ++i)
        {
            LLQuaternion unused;
            if (!fkJoint(avatar, joints[i], p[i], unused))
                return;
        }
        if (pass > 0 && (p[count - 1] - goal).length() < REACHED_M)
            break;
        std::vector<F32> len(count - 1);
        F32 total = 0.f;
        for (S32 i = 0; i < count - 1; ++i)
        {
            len[i] = (p[i + 1] - p[i]).length();
            total += len[i];
        }
        if (total < 1.0e-4f)
            return;

        const LLVector3 root = p[0];
        std::vector<LLVector3> q = p;
        const LLVector3 to_goal = goal - root;
        const F32 dist = to_goal.length();
        if (dist >= total - 1.0e-5f)
        {
            const LLVector3 dir = toward(to_goal, toward(p[count - 1] - root, LLVector3::x_axis));
            for (S32 i = 0; i < count - 1; ++i)
                q[i + 1] = q[i] + dir * len[i];
        }
        else
        {
            for (S32 iteration = 0; iteration < FABRIK_ITERATIONS; ++iteration)
            {
                q[count - 1] = goal;
                for (S32 i = count - 2; i >= 0; --i)
                    q[i] = q[i + 1] + toward(q[i] - q[i + 1], toward(p[i] - p[i + 1], LLVector3::x_axis)) * len[i];
                q[0] = root;
                for (S32 i = 0; i < count - 1; ++i)
                    q[i + 1] = q[i] + toward(q[i + 1] - q[i], toward(p[i + 1] - p[i], LLVector3::x_axis)) * len[i];
                if ((q[count - 1] - goal).length() < REACHED_M)
                    break;
            }
            if (state.mHasPole && count >= 3)
            {
                LLVector3 axis = q[count - 1] - root;
                if (axis.length() > 1.0e-4f)
                {
                    axis.normVec();
                    LLVector3 from = q[1] - root;
                    LLVector3 to = poleWorld(avatar, chain) - root;
                    from -= axis * (from * axis);
                    to -= axis * (to * axis);
                    if (from.length() > 1.0e-4f && to.length() > 1.0e-4f)
                    {
                        from.normVec();
                        to.normVec();
                        const LLQuaternion swing(atan2f((from % to) * axis, from * to), axis);
                        for (S32 i = 1; i < count - 1; ++i)
                            q[i] = root + (q[i] - root) * swing;
                    }
                }
            }
        }

        for (S32 i = 0; i < count - 1; ++i)
        {
            LLVector3 a, b, parent_pos;
            LLQuaternion ra, rb, parent_rot;
            if (!fkJoint(avatar, joints[i], a, ra) || !fkJoint(avatar, joints[i + 1], b, rb)
                || !fkParent(avatar, joints[i], parent_pos, parent_rot))
                return;
            const LLVector3 u = b - a;
            LLVector3 v = q[i + 1] - a;
            if (u.length() < 1.0e-6f || v.length() < 1.0e-6f)
                continue;
            LLQuaternion turn;
            turn.shortestArc(u, v);
            if (i + 2 < count)
            {
                // Carry the bend plane (this bone x the next one) onto the solved one, to
                // whichever side of it is nearer: a twist of at most 90 degrees.
                LLVector3 c_pos;
                LLQuaternion c_rot;
                if (fkJoint(avatar, joints[i + 2], c_pos, c_rot))
                {
                    v.normVec();
                    LLVector3 n_old = (u % (c_pos - b)) * turn;
                    LLVector3 n_new = v % (q[i + 2] - q[i + 1]);
                    n_old -= v * (n_old * v);
                    n_new -= v * (n_new * v);
                    if (n_old.length() > 1.0e-5f && n_new.length() > 1.0e-5f)
                    {
                        n_old.normVec();
                        n_new.normVec();
                        if (n_old * n_new < 0.f)
                            n_new = -n_new;
                        const F32 twist = atan2f((n_old % n_new) * v, n_old * n_new);
                        turn = turn * LLQuaternion(twist, v);
                        turn.normalize();
                    }
                }
            }
            applyWorldRotation(avatar, joints[i], ra * turn, parent_rot, limits && limitJoint(chain, i));
        }
        if (end_rot)
            setEndRotation(avatar, chain, *end_rot, limits);
    }

    // A chain limited at every joint (tail, ears, wings) may still be short of a goal it
    // could reach: a few damped CCD passes close what the limits allow.
    if (limits && mChains[chain].mSolver == SOLVER_CCD)
    {
        LLVector3 e_now;
        LLQuaternion re_now;
        if (fkJoint(avatar, joints[count - 1], e_now, re_now) && (e_now - goal).length() >= REACHED_M)
        {
            solveCCD(avatar, chain, goal, limits, CCD_REFINE_ITERATIONS);
            if (end_rot)
                setEndRotation(avatar, chain, *end_rot, limits);
        }
    }
}

void SLPoseIK::solveCCD(LLVOAvatar* avatar, S32 chain, const LLVector3& goal, bool limits, S32 iterations)
{
    const std::vector<std::string>& joints = chainJoints(chain);
    const S32 count = (S32)joints.size();
    for (S32 iteration = 0; iteration < iterations; ++iteration)
    {
        for (S32 i = count - 2; i >= 0; --i)
        {
            LLVector3 jp, ep, pp;
            LLQuaternion jr, er, pr;
            if (!fkJoint(avatar, joints[i], jp, jr) || !fkJoint(avatar, joints.back(), ep, er)
                || !fkParent(avatar, joints[i], pp, pr))
                return;
            const LLVector3 to_end = ep - jp;
            const LLVector3 to_goal = goal - jp;
            if (to_end.length() < 1.0e-4f || to_goal.length() < 1.0e-4f)
                continue;
            LLQuaternion turn;
            turn.shortestArc(to_end, to_goal);
            // Damped: no joint turns more than CCD_MAX_STEP_RAD per pass (no flailing).
            const F32 angle = 2.f * acosf(llclamp(llabs(turn.mQ[VW]), 0.f, 1.f));
            if (angle > CCD_MAX_STEP_RAD)
                turn = slerp(CCD_MAX_STEP_RAD / angle, LLQuaternion::DEFAULT, turn);
            applyWorldRotation(avatar, joints[i], jr * turn, pr, limits && limitJoint(chain, i));
        }
        LLVector3 ep;
        LLQuaternion er;
        if (!fkJoint(avatar, joints.back(), ep, er) || (ep - goal).length() < REACHED_M)
            break;
    }
}

void SLPoseIK::solveLookAt(LLVOAvatar* avatar, S32 chain, const LLVector3& goal, const LLQuaternion* end_rot, bool limits)
{
    const Chain& c = mChains[chain];
    const S32 count = (S32)c.mJoints.size();
    if (end_rot)
    {
        // Turning the head handle sets the head's facing directly.
        setEndRotation(avatar, chain, *end_rot, limits);
        return;
    }
    for (S32 i = 0; i < count; ++i)
    {
        LLVector3 ep, jp, pp;
        LLQuaternion er, jr, pr;
        if (!fkJoint(avatar, c.mJoints.back(), ep, er) || !fkJoint(avatar, c.mJoints[i], jp, jr)
            || !fkParent(avatar, c.mJoints[i], pp, pr))
            return;
        const LLVector3 facing = c.mForward * er;
        LLVector3 to_goal = goal - ep;
        if (to_goal.length() < 1.0e-3f)
            return;
        to_goal.normVec();
        LLQuaternion turn;
        turn.shortestArc(facing, to_goal);
        // The joints before the head take their share of the turn, the head the rest.
        const bool last = (i == count - 1);
        const F32 share = last ? 1.f : ((i < (S32)c.mShare.size()) ? c.mShare[i] : 0.4f);
        if (share <= 0.f)
            continue;
        if (!last)
            turn = slerp(share, turn);
        applyWorldRotation(avatar, c.mJoints[i], jr * turn, pr, limits);
    }
}

// ---------------------------------------------------------------------------
// Pins
// ---------------------------------------------------------------------------

void SLPoseIK::pinChain(LLVOAvatar* avatar, S32 chain)
{
    // A pin holds the chain's IK target where it is (out of reach included).
    if (!usesTarget(chain))
        return;
    ChainState& state = mStates[chain];
    if (!state.mHasTarget)
        snapTarget(avatar, chain);
    state.mPinned = true;
    state.mPlaced = true;
    state.mPinBones = chainTargets(avatar, chain);
}

// ---------------------------------------------------------------------------
// IK targets
// ---------------------------------------------------------------------------

bool SLPoseIK::usesTarget(S32 chain) const
{
    if (chain < 0 || chain >= (S32)mChains.size())
        return false;
    const ESolver solver = mChains[chain].mSolver;
    return solver == SOLVER_TWOBONE || solver == SOLVER_CCD;
}

bool SLPoseIK::rootFrame(LLVOAvatar* avatar, LLVector3& pos, LLQuaternion& rot) const
{
    // Targets live in the avatar's frame: they come along when it walks or turns.
    LLJoint* root = avatar ? avatar->getRootJoint() : nullptr;
    if (!root)
        return false;
    pos = root->getWorldPosition();
    rot = root->getWorldRotation();
    return true;
}

bool SLPoseIK::chainPose(LLVOAvatar* avatar, S32 chain, LLVector3& pos, LLQuaternion& rot, LLVector3& cone) const
{
    LLVector3 tip_pos, end_pos, root_pos;
    LLQuaternion end_rot, root_rot;
    if (!fkEffector(avatar, chain, tip_pos, end_pos, end_rot) || !rootFrame(avatar, root_pos, root_rot))
        return false;
    LLVector3 dir = tip_pos - end_pos;
    if (dir.length() < 1.0e-3f)
    {
        const std::vector<std::string>& joints = chainJoints(chain);
        LLVector3 prev_pos;
        LLQuaternion prev_rot;
        if (fkJoint(avatar, joints[joints.size() - 2], prev_pos, prev_rot))
            dir = end_pos - prev_pos;
    }
    if (dir.length() < 1.0e-4f)
        dir = LLVector3::x_axis * end_rot;
    dir.normVec();
    cone = dir * ~end_rot;
    pos = (tip_pos - root_pos) * ~root_rot;
    rot = end_rot * ~root_rot;
    rot.normalize();
    return true;
}

bool SLPoseIK::targetWorld(LLVOAvatar* avatar, S32 chain, LLVector3& pos, LLQuaternion& rot) const
{
    if (chain < 0 || chain >= (S32)mStates.size() || !mStates[chain].mHasTarget)
        return false;
    LLVector3 root_pos;
    LLQuaternion root_rot;
    if (!rootFrame(avatar, root_pos, root_rot))
        return false;
    const ChainState& state = mStates[chain];
    pos = root_pos + state.mTargetPos * root_rot;
    rot = state.mTargetRot * root_rot;
    rot.normalize();
    return true;
}

void SLPoseIK::setTargetWorld(LLVOAvatar* avatar, S32 chain, const LLVector3* pos, const LLQuaternion* rot)
{
    LLVector3 root_pos;
    LLQuaternion root_rot;
    if (!rootFrame(avatar, root_pos, root_rot))
        return;
    ChainState& state = mStates[chain];
    if (pos)
        state.mTargetPos = (*pos - root_pos) * ~root_rot;
    if (rot)
    {
        state.mTargetRot = *rot * ~root_rot;
        state.mTargetRot.normalize();
    }
}

LLVector3 SLPoseIK::poleWorld(LLVOAvatar* avatar, S32 chain) const
{
    LLVector3 root_pos;
    LLQuaternion root_rot;
    if (!rootFrame(avatar, root_pos, root_rot))
        return LLVector3::zero;
    return root_pos + mStates[chain].mPole * root_rot;
}

void SLPoseIK::setPoleWorld(LLVOAvatar* avatar, S32 chain, const LLVector3& pos)
{
    LLVector3 root_pos;
    LLQuaternion root_rot;
    if (!rootFrame(avatar, root_pos, root_rot))
        return;
    mStates[chain].mPole = (pos - root_pos) * ~root_rot;
    mStates[chain].mHasPole = true;
}

LLVector3 SLPoseIK::defaultPole(LLVOAvatar* avatar, S32 chain) const
{
    const Chain& c = mChains[chain];
    LLVector3 a, b, e;
    LLQuaternion wa, wb, we;
    if (c.mJoints.size() < 3 || !fkJoint(avatar, c.mJoints[0], a, wa) || !fkJoint(avatar, c.mJoints[1], b, wb)
        || !fkJoint(avatar, c.mJoints[2], e, we))
        return b;
    LLVector3 n = e - a;
    if (n.length() < 1.0e-4f)
        return b;
    n.normVec();
    // The side the elbow / knee points to (for a straight limb: opposite where the lower
    // bone would swing), half a limb length out in front of it.
    LLVector3 side = b - a;
    side -= n * (side * n);
    if (side.length() < 0.05f * (b - a).length())
    {
        side = -(c.mBend * wa);
        side -= n * (side * n);
    }
    if (side.length() < 1.0e-6f)
        return b;
    side.normVec();
    return b + side * (POLE_DISTANCE * ((b - a).length() + (e - b).length()));
}

void SLPoseIK::snapTarget(LLVOAvatar* avatar, S32 chain)
{
    LLVector3 pos, cone;
    LLQuaternion rot;
    if (!usesTarget(chain) || !chainPose(avatar, chain, pos, rot, cone))
        return;
    ChainState& state = mStates[chain];
    state.mHasTarget = true;
    state.mPlaced = false;
    state.mTargetPos = state.mSeenPos = pos;
    state.mTargetRot = state.mSeenRot = rot;
    state.mConeLocal = cone;
}

void SLPoseIK::markSeen(LLVOAvatar* avatar, S32 chain)
{
    LLVector3 pos, cone;
    LLQuaternion rot;
    if (!usesTarget(chain) || !chainPose(avatar, chain, pos, rot, cone))
        return;
    mStates[chain].mSeenPos = pos;
    mStates[chain].mSeenRot = rot;
}

void SLPoseIK::snapAllTargets(LLVOAvatar* avatar)
{
    for (S32 i = 0; i < (S32)mChains.size(); ++i)
    {
        if (!usesTarget(i) || !chainUsable(avatar, i))
            continue;
        snapTarget(avatar, i);
        mStates[i].mHasPole = false;
        if (mStates[i].mPinned)
            mStates[i].mPinBones = chainTargets(avatar, i);
    }
}

void SLPoseIK::updateTargets(LLVOAvatar* avatar)
{
    for (S32 i = 0; i < (S32)mChains.size(); ++i)
    {
        if (!usesTarget(i) || !chainUsable(avatar, i))
            continue;
        ChainState& state = mStates[i];
        if (!state.mHasTarget)
        {
            // First shown: from the pose as it is.
            snapTarget(avatar, i);
            continue;
        }
        // A placed target (dragged, turned, pinned or given a pole) is the user's: it stays
        // where it was put, position AND orientation, whatever moves the bones (the hips,
        // other handles, dials, mirror). Only Snap, a loaded pose or undo move it.
        if (state.mPlaced || state.mPinned || (mDrag.mActive && mDrag.mChain == i))
            continue;
        LLVector3 pos, cone;
        LLQuaternion rot;
        if (!chainPose(avatar, i, pos, rot, cone))
            continue;
        if ((pos - state.mSeenPos).length() > HOLD_EPSILON_M || !nearlySameRotation(rot, state.mSeenRot, HOLD_EPSILON_DOT))
        {
            // Never placed: the handle still marks where its chain ends, so it follows.
            snapTarget(avatar, i);
            state.mHasPole = false;
        }
    }
}

void SLPoseIK::restoreTargets(LLVOAvatar* avatar, const std::vector<ChainState>& states)
{
    for (S32 i = 0; i < (S32)llmin(states.size(), mStates.size()); ++i)
    {
        ChainState& state = mStates[i];
        state.mHasTarget = states[i].mHasTarget;
        state.mPlaced = states[i].mPlaced;
        state.mTargetPos = states[i].mTargetPos;
        state.mTargetRot = states[i].mTargetRot;
        state.mConeLocal = states[i].mConeLocal;
        state.mHasPole = states[i].mHasPole;
        state.mPole = states[i].mPole;
        markSeen(avatar, i);
        if (state.mPinned)
            state.mPinBones = chainTargets(avatar, i);
    }
}

void SLPoseIK::snapToBones()
{
    LLVOAvatar* avatar = getActiveAvatar();
    if (!avatar || mDrag.mActive)
        return;
    syncAvatar(avatar);
    snapAllTargets(avatar);
}



void SLPoseIK::holdPins(LLVOAvatar* avatar, S32 except)
{
    SLPoseStudio& studio = SLPoseStudio::instance();
    const bool was_applying = mApplying;
    mApplying = true;
    // A pinned chain holds on its own: it never drags its mirror partner along.
    studio.setMirrorSuppressed(true);
    for (S32 i = 0; i < (S32)mChains.size(); ++i)
    {
        ChainState& state = mStates[i];
        if (i == except || !state.mPinned || !state.mHasTarget || !usesTarget(i) || !chainUsable(avatar, i))
            continue;
        LLVector3 pos, cone;
        LLQuaternion rot;
        if (!chainPose(avatar, i, pos, rot, cone))
            continue;
        // Unchanged since it last held: nothing to do (an unreachable target stays unreached).
        if ((pos - state.mSeenPos).length() <= HOLD_EPSILON_M && nearlySameRotation(rot, state.mSeenRot, HOLD_EPSILON_DOT))
            continue;
        LLVector3 target_pos;
        LLQuaternion target_rot;
        if (!targetWorld(avatar, i, target_pos, target_rot))
            continue;
        solveChain(avatar, i, target_pos, &target_rot);
        markSeen(avatar, i);
        state.mPinBones = chainTargets(avatar, i);
    }
    studio.setMirrorSuppressed(false);
    mApplying = was_applying;
}

bool SLPoseIK::hasPins() const
{
    for (const ChainState& state : mStates)
    {
        if (state.mPinned)
            return true;
    }
    return false;
}

void SLPoseIK::clearPins()
{
    for (ChainState& state : mStates)
        state.mPinned = false;
}

// ---------------------------------------------------------------------------
// Studio notifications
// ---------------------------------------------------------------------------

void SLPoseIK::onStudioEdited(const LLUUID& avatar_id, bool committed)
{
    if (mApplying || avatar_id != mAvatarID || mAvatarID.isNull())
        return;

    SLPoseStudio& studio = SLPoseStudio::instance();
    // Edited some other way (dials, load, reset, sync, per-bone undo): the drag history
    // no longer describes the pose. An ease reaching its pose (settling) is not an edit:
    // it must not wipe the history (an eased IK undo ends with one).
    if (committed && !studio.isBlendSettling())
    {
        mUndo.clear();
        mRedo.clear();
    }
    if (mDrag.mActive)
        return;
    // A loaded pose / sequence step (it records its ease-in; a direct edit records none).
    const bool loaded = committed && studio.getLastEditEase(avatar_id) >= 0.f;
    if (!hasPins() && !loaded)
        return;

    LLVOAvatar* avatar = getActiveAvatar();
    if (!avatar || avatar->getID() != avatar_id)
        return;
    if (studio.getBlendRemaining(avatar_id) > 0.f)
        return;     // easing in: wait for it to arrive (its settling edit comes back here)
    if (loaded)
    {
        // The targets -- placed ones too -- move to where the pose put the chains.
        snapAllTargets(avatar);
        return;
    }

    // A pinned chain whose own bones were changed directly re-pins where it now ends (the
    // edit wins); every other pinned chain holds its pin against the change.
    for (S32 i = 0; i < (S32)mChains.size(); ++i)
    {
        ChainState& state = mStates[i];
        if (!state.mPinned)
            continue;
        const std::vector<LLQuaternion> now = chainTargets(avatar, i);
        bool changed = now.size() != state.mPinBones.size();
        for (size_t b = 0; !changed && b < now.size(); ++b)
            changed = !nearlySameRotation(now[b], state.mPinBones[b], BONE_CHANGED_DOT);
        if (changed)
        {
            snapTarget(avatar, i);
            pinChain(avatar, i);
        }
    }
    // Runs inside the edit's own notification: a committed edit is sent with the held
    // chains included, nothing more to notify.
    holdPins(avatar, -1);
}

void SLPoseIK::onStudioPosing(const LLUUID& avatar_id, bool posing)
{
    if (!posing && avatar_id == mAvatarID)
        reset();
}

// ---------------------------------------------------------------------------
// Handles
// ---------------------------------------------------------------------------

void SLPoseIK::buildHandles(LLVOAvatar* avatar, std::vector<Handle>& out) const
{
    out.clear();
    const F32 scale = handleScale(avatar);
    for (S32 i = 0; i < (S32)mChains.size(); ++i)
    {
        if (!chainUsable(avatar, i))
            continue;
        const Chain& c = mChains[i];
        Handle handle;
        handle.mChain = i;
        LLVector3 tip_pos, end_pos;
        LLQuaternion end_rot;
        if (!fkEffector(avatar, i, tip_pos, end_pos, end_rot))
            continue;
        handle.mRot = end_rot;
        switch (c.mSolver)
        {
            case SOLVER_LOOKAT:
            {
                LLVector3 facing = c.mForward * end_rot;
                facing.normVec();
                handle.mDir = facing;
                handle.mPos = end_pos + facing * (LOOKAT_DISTANCE * scale);
                break;
            }
            case SOLVER_PELVIS:
                handle.mPos = end_pos;
                handle.mDir = LLVector3::z_axis * end_rot;
                break;
            default:
            {
                // On the tip (toe / hoof) when the chain has one, pointing on from the end.
                LLVector3 dir = tip_pos - end_pos;
                if (dir.length() < 1.0e-3f)
                {
                    LLVector3 prev_pos;
                    LLQuaternion prev_rot;
                    fkJoint(avatar, chainJoints(i)[chainJoints(i).size() - 2], prev_pos, prev_rot);
                    dir = end_pos - prev_pos;
                }
                if (dir.length() < 1.0e-4f)
                    dir = LLVector3::x_axis * end_rot;
                dir.normVec();
                handle.mPos = tip_pos;
                handle.mDir = dir;
                // An IK target: drawn where it is, turned as it is turned -- never re-derived
                // from the bones it drives.
                LLVector3 target_pos;
                LLQuaternion target_rot;
                if (usesTarget(i) && targetWorld(avatar, i, target_pos, target_rot))
                {
                    handle.mPos = target_pos;
                    handle.mRot = target_rot;
                    handle.mDir = mStates[i].mConeLocal * target_rot;
                    handle.mDir.normVec();
                }
                break;
            }
        }
        // The head's handle being moved sits on the point the drag asks for.
        if (mDrag.mActive && mDrag.mChain == i && !mDrag.mPole && !mDrag.mRotate && c.mSolver == SOLVER_LOOKAT)
            handle.mPos = mDrag.mGoal;
        out.push_back(handle);

        if (c.mSolver == SOLVER_TWOBONE)
        {
            Handle pole;
            pole.mChain = i;
            pole.mPole = true;
            LLVector3 mid_pos;
            LLQuaternion mid_rot;
            fkJoint(avatar, c.mJoints[1], mid_pos, mid_rot);
            // The pole target: where it was put, else in front of the elbow / knee.
            pole.mPos = mStates[i].mHasPole ? poleWorld(avatar, i) : defaultPole(avatar, i);
            if (mDrag.mActive && mDrag.mChain == i && mDrag.mPole)
                pole.mPos = mDrag.mGoal;
            pole.mDir = LLVector3::z_axis;
            pole.mRot = mid_rot;
            out.push_back(pole);
        }
    }
}

bool SLPoseIK::pick(LLVOAvatar* avatar, S32 x, S32 y, Handle& out) const
{
    std::vector<Handle> handles;
    buildHandles(avatar, handles);
    if (handles.empty())
        return false;

    LLViewerCamera* camera = LLViewerCamera::getInstance();
    const LLVector3 origin = camera->getOrigin();
    LLVector3 ray = gViewerWindow->mouseDirectionGlobal(x, y);
    ray.normVec();
    const F32 view_height = (F32)llmax(1, gViewerWindow->getWorldViewHeightScaled());
    const F32 px_to_m = 2.f * tanf(camera->getView() * 0.5f) / view_height;   // per metre of distance
    const F32 scale = handleScale(avatar);
    const F32 length = BASE_HANDLE_LENGTH * scale;

    F32 best_t = F32_MAX;
    bool found = false;
    for (const Handle& handle : handles)
    {
        const bool point = handle.mPole || mChains[handle.mChain].mSolver == SOLVER_PELVIS;   // diamonds
        const LLVector3 center = point ? handle.mPos : handle.mPos + handle.mDir * (length * 0.5f);
        const LLVector3 oc = center - origin;
        const F32 along = oc * ray;
        if (along <= 0.f)
            continue;
        const F32 world_radius = handle.mPole ? length * 0.4f : length * 0.65f;
        const F32 radius = llmax(world_radius, MIN_PICK_PX * (handle.mPole ? 0.8f : 1.f) * px_to_m * along);
        const F32 miss2 = oc.lengthSquared() - along * along;
        if (miss2 > radius * radius)
            continue;
        // End handles win over pole handles at the same depth.
        const F32 t = along - (handle.mPole ? 0.f : radius * 0.5f);
        if (t < best_t)
        {
            best_t = t;
            out = handle;
            found = true;
        }
    }
    return found;
}

bool SLPoseIK::rayPlane(S32 x, S32 y, const LLVector3& point, const LLVector3& normal, LLVector3& hit) const
{
    const LLVector3 origin = LLViewerCamera::getInstance()->getOrigin();
    LLVector3 ray = gViewerWindow->mouseDirectionGlobal(x, y);
    ray.normVec();
    const F32 denom = ray * normal;
    if (llabs(denom) < 1.0e-4f)
        return false;
    const F32 t = ((point - origin) * normal) / denom;
    if (t <= 0.f)
        return false;
    hit = origin + ray * t;
    return true;
}

// ---------------------------------------------------------------------------
// Rotate rings
// ---------------------------------------------------------------------------

bool SLPoseIK::ringsEnabled() const
{
    static LLCachedControl<bool> rings(gSavedSettings, "SLPoseStudioIKRotateRings", true);
    return rings;
}

void SLPoseIK::ringAxes(const Handle& handle, LLVector3 axes[3]) const
{
    // The end bone's own axes (local): they turn with the bone, so the ring about the bone's
    // length twists it and the other two bend it, wherever the limb points.
    axes[0] = LLVector3::x_axis * handle.mRot;
    axes[1] = LLVector3::y_axis * handle.mRot;
    axes[2] = LLVector3::z_axis * handle.mRot;
    for (S32 i = 0; i < 3; ++i)
        axes[i].normVec();
}

bool SLPoseIK::selectedHandle(LLVOAvatar* avatar, Handle& out) const
{
    if (!ringsEnabled() || mSelectedChain < 0)
        return false;
    std::vector<Handle> handles;
    buildHandles(avatar, handles);
    for (const Handle& handle : handles)
    {
        if (handle.mChain == mSelectedChain && !handle.mPole)
        {
            out = handle;
            return true;
        }
    }
    return false;
}

F32 SLPoseIK::metersPerPixel(const LLVector3& point) const
{
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    const F32 view_height = (F32)llmax(1, gViewerWindow->getWorldViewHeightScaled());
    const F32 depth = llmax(0.05f, (point - camera->getOrigin()) * camera->getAtAxis());
    return 2.f * tanf(camera->getView() * 0.5f) / view_height * depth;
}

F32 SLPoseIK::ringRadius(const LLVector3& center) const
{
    return RING_RADIUS_PX * metersPerPixel(center);
}

bool SLPoseIK::pickRing(LLVOAvatar* avatar, S32 x, S32 y, Handle& handle, S32& axis) const
{
    // LLManipRotate::highlightManipulators: the mouse on each ring's plane, its distance
    // from the radius (scaled by how face-on the ring is) within a pick band; the ring
    // half toward the camera wins. Edge-on rings: the line they make across the view.
    axis = -1;
    Handle selected;
    if (!selectedHandle(avatar, selected))
        return false;
    const LLVector3 center = selected.mPos;
    const F32 mpp = metersPerPixel(center);
    const F32 radius = RING_RADIUS_PX * mpp;
    const F32 threshold = RING_PICK_PX * mpp;
    LLVector3 to_cam = LLViewerCamera::getInstance()->getOrigin() - center;
    to_cam.normVec();
    LLVector3 axes[3];
    ringAxes(selected, axes);

    F32 best_front = -F32_MAX;
    for (S32 a = 0; a < 3; ++a)
    {
        const F32 proj = llabs(axes[a] * to_cam);
        LLVector3 hit;
        if (!rayPlane(x, y, center, axes[a], hit))
            continue;
        const LLVector3 v = hit - center;
        const F32 dist = v.length();
        if (llabs(dist - radius) * llmax(0.05f, proj) >= threshold)
            continue;
        const F32 front = (dist > 1.0e-6f) ? (v * to_cam) / dist : 0.f;
        if (front >= -0.05f && front > best_front)
        {
            best_front = front;
            axis = a;
        }
    }
    if (axis < 0)
    {
        LLVector3 hit;
        if (rayPlane(x, y, center, to_cam, hit))
        {
            const LLVector3 v = hit - center;
            for (S32 a = 0; a < 3; ++a)
            {
                if (llabs(axes[a] * to_cam) < 0.1f && llabs(v * axes[a]) < threshold && v.length() < radius + threshold)
                {
                    axis = a;
                    break;
                }
            }
        }
    }
    if (axis < 0)
        return false;
    handle = selected;
    return true;
}

// ---------------------------------------------------------------------------
// Translate gizmo
// ---------------------------------------------------------------------------

bool SLPoseIK::gizmoRotateMode() const
{
    // Frozen while a gizmo is dragged; otherwise Ctrl held (as the build tool's Ctrl swaps
    // move for rotate).
    if (mDrag.mActive)
        return mDrag.mRotate;
    return gKeyboard && (gKeyboard->currentMask(true) & MASK_CONTROL);
}

void SLPoseIK::moveAxes(const LLVector3& center, LLVector3 axes[3]) const
{
    // World axes, each flipped to the side facing the camera (LLManipTranslate) so the
    // arrows and squares are never behind the handle.
    const LLVector3 to_cam = LLViewerCamera::getInstance()->getOrigin() - center;
    for (S32 i = 0; i < 3; ++i)
    {
        axes[i].clear();
        axes[i].mV[i] = (to_cam.mV[i] >= 0.f) ? 1.f : -1.f;
    }
}

F32 SLPoseIK::mouseAlongLine(S32 x, S32 y, const LLVector3& point, const LLVector3& dir) const
{
    // Closest points of the line point + s * dir and the mouse ray (dir and ray unit).
    const LLVector3 origin = LLViewerCamera::getInstance()->getOrigin();
    LLVector3 ray = gViewerWindow->mouseDirectionGlobal(x, y);
    ray.normVec();
    const LLVector3 w0 = point - origin;
    const F32 b = dir * ray;
    const F32 denom = 1.f - b * b;
    if (denom < 1.0e-4f)
        return 0.f;     // looking down the line: no meaningful position along it
    return (b * (ray * w0) - (dir * w0)) / denom;
}

bool SLPoseIK::pickMove(LLVOAvatar* avatar, S32 x, S32 y, Handle& handle, S32& part) const
{
    part = -1;
    Handle selected;
    if (!selectedHandle(avatar, selected) || gizmoRotateMode())
        return false;
    const LLVector3 center = selected.mPos;
    const F32 mpp = metersPerPixel(center);
    LLVector3 to_cam = LLViewerCamera::getInstance()->getOrigin() - center;
    to_cam.normVec();
    LLVector3 axes[3];
    moveAxes(center, axes);
    const LLVector3 origin = LLViewerCamera::getInstance()->getOrigin();
    LLVector3 ray = gViewerWindow->mouseDirectionGlobal(x, y);
    ray.normVec();

    // Plane squares first (they sit inside the arrows' reach, nearer the center).
    for (S32 n = 0; n < 3; ++n)
    {
        if (llabs(axes[n] * to_cam) < MOVE_PLANE_MIN_DOT)
            continue;   // edge-on: hidden
        LLVector3 hit;
        if (!rayPlane(x, y, center, axes[n], hit))
            continue;
        const LLVector3& u = axes[(n + 1) % 3];
        const LLVector3& v = axes[(n + 2) % 3];
        const F32 su = ((hit - center) * u) / mpp;
        const F32 sv = ((hit - center) * v) / mpp;
        if (su >= MOVE_PLANE_LO_PX && su <= MOVE_PLANE_HI_PX && sv >= MOVE_PLANE_LO_PX && sv <= MOVE_PLANE_HI_PX)
        {
            part = 3 + n;
            handle = selected;
            return true;
        }
    }

    // Arrows: the ray's closest approach to each shaft (start to the head's tip).
    F32 best = F32_MAX;
    for (S32 a = 0; a < 3; ++a)
    {
        if (llabs(axes[a] * to_cam) > MOVE_ARROW_MAX_DOT)
            continue;   // pointing at the camera: hidden
        const F32 s = llclamp(mouseAlongLine(x, y, center, axes[a]), MOVE_ARROW_START_PX * mpp,
                              (MOVE_ARROW_LENGTH_PX + MOVE_HEAD_LENGTH_PX) * mpp);
        const LLVector3 on_axis = center + axes[a] * s;
        const LLVector3 oc = on_axis - origin;
        const F32 along = oc * ray;
        if (along <= 0.f)
            continue;
        const F32 miss = sqrtf(llmax(0.f, oc.lengthSquared() - along * along));
        if (miss < MOVE_PICK_PX * mpp && miss < best)
        {
            best = miss;
            part = a;
        }
    }
    if (part < 0)
        return false;
    handle = selected;
    return true;
}

void SLPoseIK::drawRibbon(const LLVector3& from, const LLVector3& to, F32 half_width, const LLColor4& color) const
{
    // A flat band facing the camera, both sides.
    LLVector3 dir = to - from;
    LLVector3 to_cam = LLViewerCamera::getInstance()->getOrigin() - from;
    LLVector3 side = dir % to_cam;
    if (side.length() < 1.0e-6f)
        return;
    side.normVec();
    side *= half_width;
    const LLVector3 a = from - side, b = from + side, c = to + side, d = to - side;
    gGL.color4fv(color.mV);
    gGL.begin(LLRender::TRIANGLES);
    gGL.vertex3fv(a.mV); gGL.vertex3fv(b.mV); gGL.vertex3fv(c.mV);
    gGL.vertex3fv(a.mV); gGL.vertex3fv(c.mV); gGL.vertex3fv(d.mV);
    gGL.vertex3fv(a.mV); gGL.vertex3fv(c.mV); gGL.vertex3fv(b.mV);
    gGL.vertex3fv(a.mV); gGL.vertex3fv(d.mV); gGL.vertex3fv(c.mV);
    gGL.end();
}

void SLPoseIK::drawQuad(const LLVector3& corner, const LLVector3& u, const LLVector3& v, const LLColor4& color) const
{
    const LLVector3 a = corner, b = corner + u, c = corner + u + v, d = corner + v;
    gGL.color4fv(color.mV);
    gGL.begin(LLRender::TRIANGLES);
    gGL.vertex3fv(a.mV); gGL.vertex3fv(b.mV); gGL.vertex3fv(c.mV);
    gGL.vertex3fv(a.mV); gGL.vertex3fv(c.mV); gGL.vertex3fv(d.mV);
    gGL.vertex3fv(a.mV); gGL.vertex3fv(c.mV); gGL.vertex3fv(b.mV);
    gGL.vertex3fv(a.mV); gGL.vertex3fv(d.mV); gGL.vertex3fv(c.mV);
    gGL.end();
}

void SLPoseIK::drawMoveGizmo(const LLVector3& center, bool xray) const
{
    static const LLColor4 AXIS_COLORS[3] = { LLColor4(1.f, 0.3f, 0.3f, 1.f), LLColor4(0.35f, 1.f, 0.35f, 1.f),
                                             LLColor4(0.35f, 0.55f, 1.f, 1.f) };
    const F32 mpp = metersPerPixel(center);
    LLVector3 to_cam = LLViewerCamera::getInstance()->getOrigin() - center;
    to_cam.normVec();
    LLVector3 axes[3];
    moveAxes(center, axes);
    const bool moving = mDrag.mActive && (mDrag.mMoveAxis >= 0 || mDrag.mMovePlane >= 0);
    auto shade = [&](S32 part, const LLColor4& base, F32 alpha)
    {
        const bool active = mDrag.mActive && (mDrag.mMoveAxis == part || (part >= 3 && mDrag.mMovePlane == part - 3));
        const bool hover = !mDrag.mActive && mHoverMove == part;
        LLColor4 color = (active || hover) ? lerp(base, LLColor4::white, 0.35f) : base;
        F32 a = (active || hover) ? 0.95f : alpha;
        if (moving && !active)
            a = 0.15f;      // dragging another part
        color.mV[VALPHA] = xray ? a * 0.3f : a;
        return color;
    };

    for (S32 n = 0; n < 3; ++n)
    {
        if (llabs(axes[n] * to_cam) < MOVE_PLANE_MIN_DOT)
            continue;
        const LLVector3 u = axes[(n + 1) % 3] * ((MOVE_PLANE_HI_PX - MOVE_PLANE_LO_PX) * mpp);
        const LLVector3 v = axes[(n + 2) % 3] * ((MOVE_PLANE_HI_PX - MOVE_PLANE_LO_PX) * mpp);
        const LLVector3 corner = center + (axes[(n + 1) % 3] + axes[(n + 2) % 3]) * (MOVE_PLANE_LO_PX * mpp);
        drawQuad(corner, u, v, shade(3 + n, AXIS_COLORS[n], 0.35f));
    }
    for (S32 a = 0; a < 3; ++a)
    {
        if (llabs(axes[a] * to_cam) > MOVE_ARROW_MAX_DOT)
            continue;
        const LLColor4 color = shade(a, AXIS_COLORS[a], 0.75f);
        const LLVector3 start = center + axes[a] * (MOVE_ARROW_START_PX * mpp);
        const LLVector3 head = center + axes[a] * (MOVE_ARROW_LENGTH_PX * mpp);
        drawRibbon(start, head, MOVE_SHAFT_PX * mpp, color);
        drawCone(head, axes[a], MOVE_HEAD_LENGTH_PX * mpp, MOVE_HEAD_RADIUS_PX * mpp, color);
    }
}

// ---------------------------------------------------------------------------
// Mouse
// ---------------------------------------------------------------------------

bool SLPoseIK::handleHover(S32 x, S32 y, MASK mask)
{
    mHoverChain = -1;
    mHoverPole = false;
    mHoverRing = -1;
    mHoverMove = -1;
    if (mDrag.mActive || (mask & MASK_ALT))
        return false;
    LLVOAvatar* avatar = getActiveAvatar();
    if (!avatar)
        return false;
    syncAvatar(avatar);
    updateTips(avatar);
    updateTargets(avatar);
    Handle handle;
    if (!pick(avatar, x, y, handle))
    {
        // Handles win; then the selected handle's gizmo: the translate arrows / squares,
        // or with Ctrl the rotate rings.
        S32 part = -1;
        if (gizmoRotateMode())
        {
            if (!pickRing(avatar, x, y, handle, part))
                return false;
            mHoverRing = part;
            gViewerWindow->setCursor(UI_CURSOR_TOOLROTATE);
            return true;
        }
        if (!pickMove(avatar, x, y, handle, part))
            return false;
        mHoverMove = part;
        gViewerWindow->setCursor(UI_CURSOR_TOOLTRANSLATE);
        return true;
    }
    mHoverChain = handle.mChain;
    mHoverPole = handle.mPole;
    gViewerWindow->setCursor(UI_CURSOR_HAND);
    return true;
}

bool SLPoseIK::handleMouseClick(S32 x, S32 y, MASK mask, S32 clicktype, bool down)
{
    if (!down || mDrag.mActive || (mask & MASK_ALT))
        return false;
    if (clicktype != CLICK_LEFT && clicktype != CLICK_RIGHT && clicktype != CLICK_DOUBLELEFT)
        return false;
    LLVOAvatar* avatar = getActiveAvatar();
    if (!avatar)
        return false;
    syncAvatar(avatar);
    updateTips(avatar);
    updateTargets(avatar);
    Handle handle;
    S32 ring = -1;
    S32 move = -1;
    if (!pick(avatar, x, y, handle))
    {
        if (clicktype == CLICK_RIGHT)
            return false;
        if (gizmoRotateMode() ? !pickRing(avatar, x, y, handle, ring) : !pickMove(avatar, x, y, handle, move))
            return false;
    }

    const Chain& chain = mChains[handle.mChain];
    if (clicktype == CLICK_RIGHT)
    {
        // Right-click a cone: pin / unpin it (the pelvis and pole handles have no pin).
        if (!handle.mPole && chain.mSolver != SOLVER_PELVIS)
        {
            ChainState& state = mStates[handle.mChain];
            if (state.mPinned)
                state.mPinned = false;
            else
                pinChain(avatar, handle.mChain);
        }
        return true;
    }

    // Left: start a drag.
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    mDrag = DragState();
    mDrag.mStatesBefore = mStates;
    mDrag.mChain = handle.mChain;
    mDrag.mPole = handle.mPole;
    mDrag.mRing = ring;
    mDrag.mMoveAxis = (move >= 0 && move < 3) ? move : -1;
    mDrag.mMovePlane = (move >= 3) ? move - 3 : -1;
    mDrag.mRotate = ring >= 0 || (move < 0 && !handle.mPole && (mask & MASK_CONTROL));
    mDrag.mRoll = ring < 0 && mDrag.mRotate && (mask & MASK_SHIFT);
    mDrag.mWasSelected = (mSelectedChain == handle.mChain);
    // Clicking a handle selects it: its rotate rings show.
    if (!handle.mPole && ringsEnabled())
        mSelectedChain = handle.mChain;
    mDrag.mStartX = x;
    mDrag.mStartY = y;
    mDrag.mStartGoal = handle.mPos;
    mDrag.mGoal = handle.mPos;
    mDrag.mStartRot = handle.mRot;
    mDrag.mGoalRot = handle.mRot;
    // Free drag: on the plane through the handle facing the camera (its depth along the
    // view axis is kept), holding the grab point's offset from the handle in the camera's
    // own frame -- so the depth stays put even if the camera follows the avatar mid-drag.
    mDrag.mPlaneNormal = camera->getAtAxis();
    mDrag.mDepth = (handle.mPos - camera->getOrigin()) * mDrag.mPlaneNormal;
    if (!rayPlane(x, y, handle.mPos, mDrag.mPlaneNormal, mDrag.mStartHit))
        mDrag.mStartHit = handle.mPos;
    mDrag.mGrabLeft = (handle.mPos - mDrag.mStartHit) * camera->getLeftAxis();
    mDrag.mGrabUp = (handle.mPos - mDrag.mStartHit) * camera->getUpAxis();
    if (move >= 0)
    {
        // LLManipTranslate: the axis / plane is frozen at mouse down; the handle keeps its
        // offset from the grab point along it.
        LLVector3 axes[3];
        moveAxes(handle.mPos, axes);
        if (mDrag.mMoveAxis >= 0)
        {
            mDrag.mMoveDir = axes[mDrag.mMoveAxis];
            mDrag.mMoveDown = mouseAlongLine(x, y, handle.mPos, mDrag.mMoveDir);
        }
        else
        {
            mDrag.mMoveDir = axes[mDrag.mMovePlane];
            if (!rayPlane(x, y, handle.mPos, mDrag.mMoveDir, mDrag.mMoveDownHit))
                mDrag.mMoveDownHit = handle.mPos;
        }
    }
    if (ring >= 0)
    {
        // LLManipRotate::handleMouseDownOnPart: freeze the axis, center and radius, and
        // decide edge-on once.
        LLVector3 axes[3];
        ringAxes(handle, axes);
        mDrag.mRingAxis = axes[ring];
        mDrag.mRingCenter = handle.mPos;
        mDrag.mRingRadius = ringRadius(handle.mPos);
        LLVector3 to_cam = camera->getOrigin() - mDrag.mRingCenter;
        to_cam.normVec();
        LLVector3 hit;
        mDrag.mRingEdgeOn = llabs(mDrag.mRingAxis * to_cam) < RING_EDGE_ON_DOT;
        if (!mDrag.mRingEdgeOn && rayPlane(x, y, mDrag.mRingCenter, mDrag.mRingAxis, hit))
        {
            LLVector3 v = hit - mDrag.mRingCenter;
            v -= mDrag.mRingAxis * (v * mDrag.mRingAxis);
            if (v.length() > 1.0e-6f)
            {
                v.normVec();
                mDrag.mRingDown = v;
            }
            else
            {
                mDrag.mRingEdgeOn = true;
            }
        }
        else
        {
            mDrag.mRingEdgeOn = true;
        }
        if (mDrag.mRingEdgeOn)
        {
            // Across the ring's line on the view plane through the center.
            mDrag.mRingCamNormal = to_cam;
            mDrag.mRingUp = to_cam % mDrag.mRingAxis;
            mDrag.mRingUp.normVec();
            if (rayPlane(x, y, mDrag.mRingCenter, to_cam, hit))
                mDrag.mRingDownDist = (hit - mDrag.mRingCenter) * mDrag.mRingUp;
        }
    }
    if (chain.mSolver == SOLVER_PELVIS)
    {
        if (const SLPoseStudio::joint_t* pelvis = SLPoseStudio::instance().findJoint(chain.mJoints[0]))
            mDrag.mStartPelvisPos = SLPoseStudio::instance().getBonePosition(avatar, *pelvis);
    }
    // A pole diamond is only set once it moves (onDragHover): a plain click leaves the
    // limb, its bend and the undo history alone. Bones still easing in (an eased edit or
    // pose load) jump to where they are going: the drag starts from that pose.
    SLPoseStudio::instance().finishBlend(avatar);
    mDrag.mBefore = SLPoseStudio::instance().captureBoneStates(avatar);
    mDrag.mActive = true;
    captureDragStart(avatar);
    // Nothing to solve until the mouse moves: a click leaves the pose exactly as it was.
    mDrag.mSolved = true;
    mDrag.mSolvedGoal = mDrag.mGoal;
    mDrag.mSolvedGoalRot = mDrag.mGoalRot;
    mTool->setMouseCapture(true);
    return true;
}

void SLPoseIK::onDragHover(S32 x, S32 y, MASK mask)
{
    if (!mDrag.mActive)
        return;
    LLVOAvatar* avatar = getActiveAvatar();
    if (!avatar || avatar->getID() != mAvatarID)
        return;
    const Chain& chain = mChains[mDrag.mChain];
    LLViewerCamera* camera = LLViewerCamera::getInstance();

    if (mDrag.mRing >= 0)
    {
        // A rotate ring (LLManipRotate::dragConstrained): in the ring's plane, the angle
        // from the mouse-down direction to the mouse's, about the frozen axis -- absolute,
        // only unwrapped past 180 degrees, so no per-frame error builds up; a missed plane
        // hit keeps the last angle. Edge-on: the mouse's travel across the ring's line.
        LLVector3 hit;
        if (!mDrag.mRingEdgeOn)
        {
            if (rayPlane(x, y, mDrag.mRingCenter, mDrag.mRingAxis, hit))
            {
                LLVector3 v = hit - mDrag.mRingCenter;
                v -= mDrag.mRingAxis * (v * mDrag.mRingAxis);
                if (v.length() > 0.02f * mDrag.mRingRadius)
                {
                    v.normVec();
                    F32 a = atan2f((mDrag.mRingDown % v) * mDrag.mRingAxis, mDrag.mRingDown * v);
                    while (a - mDrag.mRingAngle > F_PI)
                        a -= F_TWO_PI;
                    while (a - mDrag.mRingAngle < -F_PI)
                        a += F_TWO_PI;
                    mDrag.mRingAngle = a;
                }
            }
        }
        else if (rayPlane(x, y, mDrag.mRingCenter, mDrag.mRingCamNormal, hit))
        {
            // The ring's near edge moves along -up for a positive turn.
            const F32 dist = (hit - mDrag.mRingCenter) * mDrag.mRingUp - mDrag.mRingDownDist;
            mDrag.mRingAngle = -dist / llmax(mDrag.mRingRadius, 1.0e-4f);
        }
        F32 angle = mDrag.mRingAngle;
        if (mask & MASK_SHIFT)
        {
            const F32 step = RING_SNAP_DEG * DEG_TO_RAD;
            angle = (F32)ll_round(angle / step) * step;
        }
        static LLCachedControl<bool> limits(gSavedSettings, "SLPoseStudioIKLimits", true);
        if (limits)
        {
            // Limited bones stop along the slerp from their start rotation, which takes the
            // short way past 180 degrees: keep the request on its own side.
            angle = llclamp(angle, -F_PI + 0.02f, F_PI - 0.02f);
        }
        mDrag.mGoalRot = mDrag.mStartRot * LLQuaternion(angle, mDrag.mRingAxis);
        mDrag.mGoalRot.normalize();
    }
    else if (mDrag.mMoveAxis >= 0)
    {
        // Along one world axis: where the mouse ray passes nearest the axis line (looking
        // straight down the axis keeps the last position).
        LLVector3 ray = gViewerWindow->mouseDirectionGlobal(x, y);
        ray.normVec();
        if (llabs(ray * mDrag.mMoveDir) < 0.999f)
            mDrag.mGoal = mDrag.mStartGoal + mDrag.mMoveDir * (mouseAlongLine(x, y, mDrag.mStartGoal, mDrag.mMoveDir) - mDrag.mMoveDown);
    }
    else if (mDrag.mMovePlane >= 0)
    {
        // Within one world plane.
        LLVector3 hit;
        if (!rayPlane(x, y, mDrag.mStartGoal, mDrag.mMoveDir, hit))
            return;
        LLVector3 delta = hit - mDrag.mMoveDownHit;
        delta -= mDrag.mMoveDir * (delta * mDrag.mMoveDir);
        mDrag.mGoal = mDrag.mStartGoal + delta;
    }
    else if (mDrag.mRotate)
    {
        // Ctrl: turn the end bone. Horizontal mouse turns about the camera's up axis,
        // vertical about its left axis; Ctrl+Shift rolls about the view axis.
        const F32 dx = (F32)(x - mDrag.mStartX) * DRAG_ROTATE_RAD_PER_PX;
        const F32 dy = (F32)(y - mDrag.mStartY) * DRAG_ROTATE_RAD_PER_PX;
        LLQuaternion turn;
        if (mDrag.mRoll)
        {
            turn = LLQuaternion(-dx, camera->getAtAxis());
        }
        else
        {
            turn = LLQuaternion(dx, camera->getUpAxis()) * LLQuaternion(-dy, camera->getLeftAxis());
        }
        mDrag.mGoalRot = mDrag.mStartRot * turn;
        mDrag.mGoalRot.normalize();
    }
    else
    {
        // The camera-facing plane at the handle's mouse-down depth, from where the camera
        // is now (the same plane as at mouse down unless the camera moved).
        const LLVector3 at = camera->getAtAxis();
        LLVector3 hit;
        if (!rayPlane(x, y, camera->getOrigin() + at * mDrag.mDepth, at, hit))
            return;
        const LLVector3 free_goal = hit + camera->getLeftAxis() * mDrag.mGrabLeft + camera->getUpAxis() * mDrag.mGrabUp;
        LLVector3 delta = free_goal - mDrag.mStartGoal;
        if (mask & MASK_SHIFT)
        {
            // Shift: lock to the world axis the drag starts along.
            const F32 moved_px = (F32)(llabs(x - mDrag.mStartX) + llabs(y - mDrag.mStartY));
            if (mDrag.mAxisLock < 0 && moved_px >= AXIS_LOCK_PX)
            {
                S32 axis = 0;
                for (S32 i = 1; i < 3; ++i)
                {
                    if (llabs(delta.mV[i]) > llabs(delta.mV[axis]))
                        axis = i;
                }
                mDrag.mAxisLock = axis;
            }
            if (mDrag.mAxisLock >= 0)
            {
                // Along the locked axis: the point on it nearest the mouse ray.
                LLVector3 axis_dir;
                axis_dir.mV[mDrag.mAxisLock] = 1.f;
                const LLVector3 origin = camera->getOrigin();
                LLVector3 ray = gViewerWindow->mouseDirectionGlobal(x, y);
                ray.normVec();
                const LLVector3 w0 = mDrag.mStartGoal - origin;
                const F32 b = axis_dir * ray;
                const F32 denom = 1.f - b * b;
                F32 s = 0.f;
                if (denom > 1.0e-4f)
                    s = (b * (ray * w0) - (axis_dir * w0)) / denom;
                else
                    s = delta.mV[mDrag.mAxisLock];
                delta.clear();
                delta.mV[mDrag.mAxisLock] = s;
            }
            else
            {
                delta.clear();
            }
        }
        else
        {
            mDrag.mAxisLock = -1;
        }
        mDrag.mGoal = mDrag.mStartGoal + delta;
    }

    // The capturing tool gets a hover every frame (LLViewerWindow::updateUI), mouse moving
    // or not. Each solve is a pure function of the drag-start pose and the goal, so an
    // unchanged goal has nothing new to solve.
    if (mDrag.mSolved && mDrag.mGoal == mDrag.mSolvedGoal && mDrag.mGoalRot == mDrag.mSolvedGoalRot)
        return;
    mDrag.mSolved = true;
    mDrag.mSolvedGoal = mDrag.mGoal;
    mDrag.mSolvedGoalRot = mDrag.mGoalRot;

    SLPoseStudio& studio = SLPoseStudio::instance();
    mApplying = true;
    // Solve from the pose at mouse down, never from the last frame's solve: solving on top
    // of its own output every frame let minimal-arc twist, limit clamps and the decode
    // reference drift accumulate, and the limb wandered / flailed even with the mouse still.
    restoreDragStart(avatar);
    const bool target = usesTarget(mDrag.mChain);
    if (mDrag.mPole)
    {
        // Elbow / knee pole: the bend plane turns to it; the hand / foot stays on its target.
        setPoleWorld(avatar, mDrag.mChain, mDrag.mGoal);
        mStates[mDrag.mChain].mPlaced = true;
        LLVector3 target_pos;
        LLQuaternion target_rot;
        if (targetWorld(avatar, mDrag.mChain, target_pos, target_rot))
            solveChain(avatar, mDrag.mChain, target_pos, &target_rot);
    }
    else if (chain.mSolver == SOLVER_PELVIS)
    {
        const SLPoseStudio::joint_t* pelvis = studio.findJoint(chain.mJoints[0]);
        LLVector3 parent_pos;
        LLQuaternion parent_rot;
        if (pelvis && fkParent(avatar, chain.mJoints[0], parent_pos, parent_rot))
        {
            if (mDrag.mRotate)
            {
                // Turned as a whole (no follower counter-rotation, no limits): the body turns.
                applyWorldRotation(avatar, chain.mJoints[0], mDrag.mGoalRot, parent_rot, false);
            }
            else
            {
                // World move -> the pelvis' public position offset, in its parent's frame.
                const LLVector3 local_delta = (mDrag.mGoal - mDrag.mStartGoal) * ~parent_rot;
                studio.setBonePosition(avatar, *pelvis, mDrag.mStartPelvisPos + local_delta, false);
            }
        }
    }
    else if (target)
    {
        // An IK target: moving changes only its position, turning (ring / Ctrl) only its
        // orientation. The chain reaches for its position (straight toward it when out of
        // reach; the handle stays where it was put) and the end bone takes its orientation.
        if (mDrag.mRotate)
            setTargetWorld(avatar, mDrag.mChain, nullptr, &mDrag.mGoalRot);
        else
            setTargetWorld(avatar, mDrag.mChain, &mDrag.mGoal, nullptr);
        mStates[mDrag.mChain].mPlaced = true;
        LLVector3 target_pos;
        LLQuaternion target_rot;
        if (targetWorld(avatar, mDrag.mChain, target_pos, target_rot))
            solveChain(avatar, mDrag.mChain, target_pos, &target_rot);
    }
    else if (mDrag.mRotate)
    {
        // The head turns.
        solveChain(avatar, mDrag.mChain, mDrag.mStartGoal, &mDrag.mGoalRot);
    }
    else
    {
        // The head aims.
        solveChain(avatar, mDrag.mChain, mDrag.mGoal, nullptr);
    }
    if (target)
        markSeen(avatar, mDrag.mChain);
    holdPins(avatar, mDrag.mChain);
    mApplying = false;
}

void SLPoseIK::onDragEnd(bool cancelled)
{
    if (!mDrag.mActive)
        return;
    DragState drag = mDrag;
    mDrag = DragState();

    // A plain click (nothing solved) on the already selected handle hides its rings.
    if (!cancelled && drag.mRing < 0 && drag.mMoveAxis < 0 && drag.mMovePlane < 0 && !drag.mPole && drag.mWasSelected
        && drag.mSolvedGoal == drag.mStartGoal
        && drag.mSolvedGoalRot == drag.mStartRot)
        mSelectedChain = -1;

    LLVOAvatar* avatar = getActiveAvatar();
    if (!avatar || avatar->getID() != mAvatarID)
        return;
    SLPoseStudio& studio = SLPoseStudio::instance();
    if (cancelled)
    {
        mApplying = true;
        studio.restoreBoneStates(avatar, drag.mBefore);
        mApplying = false;
        restoreTargets(avatar, drag.mStatesBefore);
        return;
    }

    // The drag is over whether or not it changed a bone (an out-of-reach target can move
    // with none changing): later edits are not its release.
    studio.endPreview(avatar->getID());

    // A pinned target that was dragged holds at its new place.
    if (mStates[drag.mChain].mPinned)
        mStates[drag.mChain].mPinBones = chainTargets(avatar, drag.mChain);

    // One undo step for the whole drag: only the bones it changed.
    const std::vector<SLPoseStudio::BoneState> after = studio.captureBoneStates(avatar);
    UndoEntry entry;
    for (const SLPoseStudio::BoneState& now : after)
    {
        for (const SLPoseStudio::BoneState& was : drag.mBefore)
        {
            if (was.mJoint != now.mJoint)
                continue;
            if (was.mEdited != now.mEdited || !nearlySameRotation(was.mRotation, now.mRotation, HOLD_EPSILON_DOT)
                || (was.mPosition - now.mPosition).length() > 1.0e-5f)
            {
                entry.mBefore.push_back(was);
                entry.mAfter.push_back(now);
            }
            break;
        }
    }
    // Target / pole moves are part of the step too (an out-of-reach target can move
    // without any bone changing).
    bool targets_moved = false;
    for (size_t i = 0; i < llmin(drag.mStatesBefore.size(), mStates.size()) && !targets_moved; ++i)
    {
        const ChainState& was = drag.mStatesBefore[i];
        const ChainState& now = mStates[i];
        targets_moved = was.mHasTarget != now.mHasTarget || was.mHasPole != now.mHasPole || was.mPlaced != now.mPlaced
                        || (was.mTargetPos - now.mTargetPos).length() > 1.0e-5f
                        || !nearlySameRotation(was.mTargetRot, now.mTargetRot, HOLD_EPSILON_DOT)
                        || (now.mHasPole && (was.mPole - now.mPole).length() > 1.0e-5f);
    }
    if (!entry.mBefore.empty() || targets_moved)
    {
        entry.mTargetsBefore = drag.mStatesBefore;
        entry.mTargetsAfter = mStates;
        mUndo.push_back(entry);
        if (mUndo.size() > 50)
            mUndo.erase(mUndo.begin());
        mRedo.clear();
    }
    if (!entry.mBefore.empty())
    {
        // The drag previewed locally; now it is a real edit (saved, shared, synced).
        mApplying = true;
        studio.notifyEdited(avatar, true);
        mApplying = false;
    }
}

// ---------------------------------------------------------------------------
// Undo
// ---------------------------------------------------------------------------

bool SLPoseIK::canUndo(LLVOAvatar* avatar) const
{
    return avatar && avatar->getID() == mAvatarID && !mUndo.empty() && !mDrag.mActive;
}

bool SLPoseIK::canRedo(LLVOAvatar* avatar) const
{
    return avatar && avatar->getID() == mAvatarID && !mRedo.empty() && !mDrag.mActive;
}

void SLPoseIK::undo(LLVOAvatar* avatar)
{
    if (!canUndo(avatar))
        return;
    UndoEntry entry = mUndo.back();
    mUndo.pop_back();
    mApplying = true;
    SLPoseStudio::instance().restoreBoneStates(avatar, entry.mBefore);
    mApplying = false;
    restoreTargets(avatar, entry.mTargetsBefore);
    mRedo.push_back(entry);
}

void SLPoseIK::redo(LLVOAvatar* avatar)
{
    if (!canRedo(avatar))
        return;
    UndoEntry entry = mRedo.back();
    mRedo.pop_back();
    mApplying = true;
    SLPoseStudio::instance().restoreBoneStates(avatar, entry.mAfter);
    mApplying = false;
    restoreTargets(avatar, entry.mTargetsAfter);
    mUndo.push_back(entry);
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void SLPoseIK::drawCone(const LLVector3& base, const LLVector3& dir, F32 length, F32 radius, const LLColor4& color) const
{
    constexpr S32 SEGMENTS = 16;
    const LLVector3 e1 = perpendicular(dir);
    LLVector3 e2 = dir % e1;
    e2.normVec();
    const LLVector3 apex = base + dir * length;
    const LLVector3 view = LLViewerCamera::getInstance()->getAtAxis();

    LLVector3 ring[SEGMENTS + 1];
    for (S32 i = 0; i <= SEGMENTS; ++i)
    {
        const F32 a = F_TWO_PI * (F32)i / (F32)SEGMENTS;
        ring[i] = base + (e1 * cosf(a) + e2 * sinf(a)) * radius;
    }

    gGL.begin(LLRender::TRIANGLES);
    for (S32 i = 0; i < SEGMENTS; ++i)
    {
        // Facet shading toward the camera, so the cone reads as a solid.
        LLVector3 n = (ring[i + 1] - ring[i]) % (apex - ring[i]);
        n.normVec();
        const F32 shade = 0.55f + 0.45f * llabs(n * view);
        gGL.color4f(color.mV[VRED] * shade, color.mV[VGREEN] * shade, color.mV[VBLUE] * shade, color.mV[VALPHA]);
        gGL.vertex3fv(apex.mV);
        gGL.vertex3fv(ring[i].mV);
        gGL.vertex3fv(ring[i + 1].mV);

        gGL.color4f(color.mV[VRED] * 0.5f, color.mV[VGREEN] * 0.5f, color.mV[VBLUE] * 0.5f, color.mV[VALPHA]);
        gGL.vertex3fv(base.mV);
        gGL.vertex3fv(ring[i + 1].mV);
        gGL.vertex3fv(ring[i].mV);
    }
    gGL.end();
}

void SLPoseIK::drawDiamond(const LLVector3& center, F32 size, const LLColor4& color) const
{
    const LLVector3 tips[6] = { center + LLVector3(size, 0, 0), center - LLVector3(size, 0, 0),
                                center + LLVector3(0, size, 0), center - LLVector3(0, size, 0),
                                center + LLVector3(0, 0, size), center - LLVector3(0, 0, size) };
    // 8 faces: (+-x) x (+-y) x (+-z)
    static const S32 faces[8][3] = { { 0, 2, 4 }, { 2, 1, 4 }, { 1, 3, 4 }, { 3, 0, 4 },
                                     { 2, 0, 5 }, { 1, 2, 5 }, { 3, 1, 5 }, { 0, 3, 5 } };
    gGL.begin(LLRender::TRIANGLES);
    for (S32 f = 0; f < 8; ++f)
    {
        const F32 shade = (f < 4) ? 1.f : 0.7f;
        gGL.color4f(color.mV[VRED] * shade, color.mV[VGREEN] * shade, color.mV[VBLUE] * shade, color.mV[VALPHA]);
        for (S32 v = 0; v < 3; ++v)
            gGL.vertex3fv(tips[faces[f][v]].mV);
    }
    gGL.end();
}

void SLPoseIK::drawRing(const LLVector3& center, const LLVector3& normal, F32 radius, const LLColor4& color) const
{
    constexpr S32 SEGMENTS = 24;
    const LLVector3 e1 = perpendicular(normal);
    LLVector3 e2 = normal % e1;
    e2.normVec();
    gGL.color4fv(color.mV);
    gGL.begin(LLRender::LINES);
    for (S32 i = 0; i < SEGMENTS; ++i)
    {
        const F32 a0 = F_TWO_PI * (F32)i / (F32)SEGMENTS;
        const F32 a1 = F_TWO_PI * (F32)(i + 1) / (F32)SEGMENTS;
        const LLVector3 p0 = center + (e1 * cosf(a0) + e2 * sinf(a0)) * radius;
        const LLVector3 p1 = center + (e1 * cosf(a1) + e2 * sinf(a1)) * radius;
        gGL.vertex3fv(p0.mV);
        gGL.vertex3fv(p1.mV);
    }
    gGL.end();
}

void SLPoseIK::drawBand(const LLVector3& center, const LLVector3& normal, F32 radius, F32 width, const LLColor4& color,
                        F32 back_alpha) const
{
    // A flat annulus in the ring's plane plus a band across it, both double-sided: the
    // ring reads at any angle, edge-on included.
    const LLVector3 e1 = perpendicular(normal);
    LLVector3 e2 = normal % e1;
    e2.normVec();
    const LLVector3 across = normal * width;
    // The half away from the camera is dimmer (the build tool hides it); it cannot be picked.
    LLVector3 to_cam = LLViewerCamera::getInstance()->getOrigin() - center;
    to_cam.normVec();
    LLColor4 back = color;
    back.mV[VALPHA] *= back_alpha;
    gGL.begin(LLRender::TRIANGLES);
    auto quad = [](const LLVector3& a, const LLVector3& b, const LLVector3& c, const LLVector3& d)
    {
        gGL.vertex3fv(a.mV); gGL.vertex3fv(b.mV); gGL.vertex3fv(c.mV);
        gGL.vertex3fv(a.mV); gGL.vertex3fv(c.mV); gGL.vertex3fv(d.mV);
        gGL.vertex3fv(a.mV); gGL.vertex3fv(c.mV); gGL.vertex3fv(b.mV);
        gGL.vertex3fv(a.mV); gGL.vertex3fv(d.mV); gGL.vertex3fv(c.mV);
    };
    for (S32 i = 0; i < RING_SEGMENTS; ++i)
    {
        const F32 a0 = F_TWO_PI * (F32)i / (F32)RING_SEGMENTS;
        const F32 a1 = F_TWO_PI * (F32)(i + 1) / (F32)RING_SEGMENTS;
        const LLVector3 d0 = e1 * cosf(a0) + e2 * sinf(a0);
        const LLVector3 d1 = e1 * cosf(a1) + e2 * sinf(a1);
        gGL.color4fv(((d0 + d1) * to_cam >= 0.f) ? color.mV : back.mV);
        quad(center + d0 * (radius - width), center + d0 * (radius + width),
             center + d1 * (radius + width), center + d1 * (radius - width));
        const LLVector3 p0 = center + d0 * radius;
        const LLVector3 p1 = center + d1 * radius;
        quad(p0 - across, p0 + across, p1 + across, p1 - across);
    }
    gGL.end();
}

void SLPoseIK::render()
{
    LLVOAvatar* avatar = getActiveAvatar();
    if (!avatar)
        return;
    syncAvatar(avatar);
    updateTips(avatar);
    updateTargets(avatar);

    std::vector<Handle> handles;
    buildHandles(avatar, handles);
    if (handles.empty())
        return;

    const F32 scale = handleScale(avatar);
    const F32 length = BASE_HANDLE_LENGTH * scale;
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    // Two passes: faint everywhere (x-ray through the avatar and the scene), then solid
    // where nothing is in front.
    for (S32 pass = 0; pass < 2; ++pass)
    {
        const bool xray = (pass == 0);
        LLRender::DepthScope depth(!xray, false);
        for (const Handle& handle : handles)
        {
            const Chain& chain = mChains[handle.mChain];
            const ChainState& state = mStates[handle.mChain];
            const bool dragging = mDrag.mActive && mDrag.mChain == handle.mChain && mDrag.mPole == handle.mPole;
            const bool hover = !mDrag.mActive && mHoverChain == handle.mChain && mHoverPole == handle.mPole;

            LLColor4 color = chain.mColor;
            F32 alpha = 0.45f;
            if (dragging)
            {
                color = lerp(color, LLColor4::white, 0.6f);
                alpha = 0.8f;
            }
            else if (hover)
            {
                color = lerp(color, LLColor4::white, 0.35f);
                alpha = 0.7f;
            }
            color.mV[VALPHA] = xray ? alpha * 0.35f : alpha;

            if (handle.mPole)
            {
                drawDiamond(handle.mPos, length * 0.22f, color);
                continue;
            }
            if (chain.mSolver == SOLVER_PELVIS)
            {
                drawDiamond(handle.mPos, length * 0.45f, color);
                continue;
            }
            drawCone(handle.mPos, handle.mDir, length, length * 0.38f, color);
            if (state.mPinned)
            {
                LLColor4 pin(1.f, 0.25f, 0.25f, xray ? 0.35f : 0.9f);
                drawRing(handle.mPos, handle.mDir, length * 0.6f, pin);
            }
            if (chain.mSolver == SOLVER_LOOKAT)
            {
                // The line of sight, head to handle.
                LLVector3 head_pos;
                LLQuaternion head_rot;
                if (fkJoint(avatar, chain.mJoints.back(), head_pos, head_rot))
                {
                    LLColor4 line = color;
                    line.mV[VALPHA] *= 0.6f;
                    gGL.color4fv(line.mV);
                    gGL.begin(LLRender::LINES);
                    gGL.vertex3fv(head_pos.mV);
                    gGL.vertex3fv(handle.mPos.mV);
                    gGL.end();
                }
            }
            else if (usesTarget(handle.mChain))
            {
                // Out of reach: a line from where the chain ends to where its target is.
                LLVector3 tip_pos, end_pos;
                LLQuaternion end_rot;
                if (fkEffector(avatar, handle.mChain, tip_pos, end_pos, end_rot) && (tip_pos - handle.mPos).length() > 0.01f)
                {
                    gGL.color4f(1.f, 1.f, 1.f, xray ? 0.25f : 0.7f);
                    gGL.begin(LLRender::LINES);
                    gGL.vertex3fv(tip_pos.mV);
                    gGL.vertex3fv(handle.mPos.mV);
                    gGL.end();
                }
            }
        }

        // The selected handle's gizmo (X red, Y green, Z blue, as in the build tool): the
        // translate arrows / squares, or with Ctrl the rotate rings; hidden while another
        // handle is being dragged, and a free (body) drag shows the arrows it is not using.
        const bool show_gizmo = ringsEnabled() && mSelectedChain >= 0 && (!mDrag.mActive || (mDrag.mChain == mSelectedChain && !mDrag.mPole));
        if (show_gizmo && !gizmoRotateMode())
        {
            for (const Handle& handle : handles)
            {
                if (handle.mChain == mSelectedChain && !handle.mPole)
                {
                    drawMoveGizmo(handle.mPos, xray);
                    break;
                }
            }
        }
        if (show_gizmo && gizmoRotateMode())
        {
            for (const Handle& handle : handles)
            {
                if (handle.mChain != mSelectedChain || handle.mPole)
                    continue;
                // Screen-size, on the handle (frozen while one is turned).
                const LLVector3 center = (mDrag.mActive && mDrag.mRing >= 0) ? mDrag.mRingCenter : handle.mPos;
                const F32 mpp = metersPerPixel(center);
                const F32 radius = (mDrag.mActive && mDrag.mRing >= 0) ? mDrag.mRingRadius : RING_RADIUS_PX * mpp;
                static const LLColor4 RING_COLORS[3] = { LLColor4(1.f, 0.3f, 0.3f, 1.f), LLColor4(0.35f, 1.f, 0.35f, 1.f),
                                                         LLColor4(0.35f, 0.55f, 1.f, 1.f) };
                LLVector3 axes[3];
                ringAxes(handle, axes);
                for (S32 a = 0; a < 3; ++a)
                {
                    const bool active = mDrag.mActive && mDrag.mRing == a;
                    const bool hover = !mDrag.mActive && mHoverRing == a;
                    F32 alpha = 0.5f;
                    if (active || hover)
                        alpha = 0.9f;
                    else if (mDrag.mActive && mDrag.mRing >= 0)
                        alpha = 0.15f;  // turning another ring
                    LLColor4 color = (active || hover) ? lerp(RING_COLORS[a], LLColor4::white, 0.35f) : RING_COLORS[a];
                    color.mV[VALPHA] = xray ? alpha * 0.3f : alpha;
                    const F32 width = RING_WIDTH_PX * mpp * ((active || hover) ? 1.6f : 1.f);
                    // The axis being turned stays as frozen at mouse down.
                    const LLVector3 axis = active ? mDrag.mRingAxis : axes[a];
                    drawBand(center, axis, radius, width, color, 0.35f);
                }
                break;
            }
        }
    }
    gGL.flush();
}
