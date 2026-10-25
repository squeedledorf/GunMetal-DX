/**
 * @file llfloaterimagepreview.h
 * @brief LLFloaterImagePreview class definition
 *
 * $LicenseInfo:firstyear=2004&license=viewerlgpl$
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

#ifndef LL_LLFLOATERIMAGEPREVIEW_H
#define LL_LLFLOATERIMAGEPREVIEW_H

#include "llfloaternamedesc.h"
#include "lldynamictexture.h"
#include "llpointer.h"
#include "llquaternion.h"

class LLComboBox;
class LLJoint;
class LLViewerJointMesh;
class LLVOAvatar;
class LLTextBox;
class LLVolume;

class LLImagePreviewSculpted : public LLViewerDynamicTexture
{
protected:
    virtual ~LLImagePreviewSculpted();

 public:
    LLImagePreviewSculpted(S32 width, S32 height);

    S8 getType() const override;

    void setPreviewTarget(LLImageRaw *imagep, F32 distance);

    bool render() override;
    bool vkWantsDepth() const override { return true; }   // #83: 3D preview
    void refresh();
    void rotate(F32 yaw_radians, F32 pitch_radians);
    void zoom(F32 zoom_amt);
    void pan(F32 right, F32 up);
    virtual bool needsRender() override { return mNeedsUpdate; }

 protected:
    bool        mNeedsUpdate;
    F32         mCameraDistance;
    F32         mCameraYaw;
    F32         mCameraPitch;
    F32         mCameraZoom;
    LLVector3   mCameraOffset;
    LLPointer<LLVolume> mVolume;
};


class LLImagePreviewAvatar : public LLViewerDynamicTexture
{
protected:
    virtual ~LLImagePreviewAvatar();

public:
    LLImagePreviewAvatar(S32 width, S32 height);

    S8 getType() const override;

    void setPreviewTarget(const std::string& joint_name, const std::string& mesh_name, LLImageRaw* imagep, F32 distance, bool male);
    // #83 (Vulkan): the uploaded image as a texture object (a GL name means nothing).
    void setPreviewImage(LLViewerTexture* image) { mPreviewImage = image; }
    bool vkWantsDepth() const override { return true; }   // 3D preview

    bool    render() override;
    void    refresh();
    void    rotate(F32 yaw_radians, F32 pitch_radians);
    void    zoom(F32 zoom_amt);
    void    pan(F32 right, F32 up);
    virtual bool needsRender() override { return mNeedsUpdate; }

protected:
    bool        mNeedsUpdate;
    LLJoint*    mTargetJoint;
    LLViewerJointMesh*  mTargetMesh;
    F32         mCameraDistance;
    F32         mCameraYaw;
    F32         mCameraPitch;
    F32         mCameraZoom;
    LLVector3   mCameraOffset;
    LLPointer<LLVOAvatar> mDummyAvatar;
    LLPointer<LLViewerTexture> mPreviewImage;
};

class LLFloaterImagePreview : public LLFloaterNameDesc
{
public:
    LLFloaterImagePreview(const LLSD& args);
    virtual ~LLFloaterImagePreview();

    bool postBuild() override;

    S32 getExpectedUploadCost() const override;

    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleScrollWheel(S32 x, S32 y, S32 clicks) override;

    static void onMouseCaptureLostImagePreview(LLMouseHandler*);


    void onBtnOK();

protected:
    static void     onPreviewTypeCommit(LLUICtrl*,void*);
    void            draw() override;
    bool            loadImage(const std::string& filename);

    LLPointer<LLImageRaw> mRawImagep;
    LLPointer<LLImagePreviewAvatar> mAvatarPreview;
    LLPointer<LLImagePreviewSculpted> mSculptedPreview;
    S32             mLastMouseX;
    S32             mLastMouseY;
    LLRect          mPreviewRect;
    LLRectf         mPreviewImageRect;
    S32             mSourceWidth = 0;   // <GMDX> size of the file before the power-of-two resize,
    S32             mSourceHeight = 0;  // so the preview shows the picture in its own proportions
    LLPointer<LLViewerTexture> mImagep ;

    std::string mImageLoadError;
};

#endif  // LL_LLFLOATERIMAGEPREVIEW_H
