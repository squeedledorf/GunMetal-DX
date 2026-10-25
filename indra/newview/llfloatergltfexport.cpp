/**
 * @file llfloatergltfexport.cpp
 * @brief Progress / cancel floater for the glTF export (#908).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors.
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

#include "llfloatergltfexport.h"

#include "llbutton.h"
#include "llgltfexporter.h"
#include "llprogressbar.h"
#include "lltextbox.h"

LLFloaterGLTFExport::LLFloaterGLTFExport(const LLSD& key)
    : LLFloater(key)
{
}

bool LLFloaterGLTFExport::postBuild()
{
    mStatus = getChild<LLTextBox>("status");
    mProgress = getChild<LLProgressBar>("progress");
    childSetAction("cancel_btn", [this](LLUICtrl*, const LLSD&)
    {
        LLGLTFExporter::cancel();
        closeFloater();
    });
    return true;
}

void LLFloaterGLTFExport::draw()
{
    if (!LLGLTFExporter::isActive())
    {
        closeFloater();
        return;
    }
    mStatus->setText(LLGLTFExporter::getStatusText());
    mProgress->setValue(LLGLTFExporter::getProgress() * 100.f);
    LLFloater::draw();
}

void LLFloaterGLTFExport::onClose(bool app_quitting)
{
    // Closing the window cancels a running export (nothing is written).
    if (LLGLTFExporter::isActive())
    {
        LLGLTFExporter::cancel();
    }
}
