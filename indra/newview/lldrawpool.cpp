/**
 * @file lldrawpool.cpp
 * @brief LLDrawPool class implementation
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
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

#include "lldrawpool.h"

#include "llface.h"
#include "llviewertexture.h"

S32 LLDrawPool::sNumDrawPools = 0;

namespace
{
// Pools are render-free type/face containers: LLFace keeps a pool pointer and the
// pipeline's pool maps key tree/terrain pools by texture.
class LLTexturedFacePool : public LLFacePool
{
public:
    LLTexturedFacePool(U32 type, LLViewerTexture* tex) : LLFacePool(type), mTexture(tex) {}
    LLViewerTexture* getTexture() override { return mTexture; }
private:
    LLPointer<LLViewerTexture> mTexture;
};
} // namespace

LLDrawPool *LLDrawPool::createPool(const U32 type, LLViewerTexture *tex0)
{
    switch (type)
    {
    case POOL_SKY:
    case POOL_WATEREXCLUSION:
    case POOL_TERRAIN:
    case POOL_TREE:
    case POOL_AVATAR:
    case POOL_CONTROL_AV:
    case POOL_VOIDWATER:
    case POOL_WATER:
        return new LLTexturedFacePool(type, tex0);   // face containers
    default:
        if (type == 0 || type >= NUM_POOL_TYPES)
        {
            LL_ERRS() << "Unknown draw pool type " << type << LL_ENDL;
            return nullptr;
        }
        return new LLRenderPass(type);
    }
}

LLDrawPool::LLDrawPool(const U32 type)
{
    mType = type;
    sNumDrawPools++;
    mId = sNumDrawPools;
}

LLDrawPool::~LLDrawPool()
{
}

//=============================
// Face Pool Implementation
//=============================
LLFacePool::LLFacePool(const U32 type)
: LLDrawPool(type)
{
}

LLFacePool::~LLFacePool()
{
    destroy();
}

void LLFacePool::destroy()
{
    if (!mReferences.empty())
    {
        LL_INFOS() << mReferences.size() << " references left on deletion of draw pool!" << LL_ENDL;
    }
}

// virtual
bool LLFacePool::addFace(LLFace *facep)
{
    addFaceReference(facep);
    return true;
}

// virtual
bool LLFacePool::removeFace(LLFace *facep)
{
    removeFaceReference(facep);
    return true;
}

LLViewerTexture *LLFacePool::getTexture()
{
    return NULL;
}

void LLFacePool::removeFaceReference(LLFace *facep)
{
    if (facep->getReferenceIndex() != -1)
    {
        if (facep->getReferenceIndex() != (S32)mReferences.size())
        {
            LLFace *back = mReferences.back();
            mReferences[facep->getReferenceIndex()] = back;
            back->setReferenceIndex(facep->getReferenceIndex());
        }
        mReferences.pop_back();
    }
    facep->setReferenceIndex(-1);
}

void LLFacePool::addFaceReference(LLFace *facep)
{
    if (-1 == facep->getReferenceIndex())
    {
        facep->setReferenceIndex(static_cast<S32>(mReferences.size()));
        mReferences.push_back(facep);
    }
}

void LLFacePool::printDebugInfo() const
{
    LL_INFOS() << "Pool " << this << " Type: " << getType() << LL_ENDL;
}
