/**
 * @file llgpuinfo.cpp
 * @brief GPU / driver identity of the Vulkan device.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
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

#include "linden_common.h"

#include "llgpuinfo.h"

#include "llerror.h"
#include "llsd.h"

LLGPUInfo gGPUInfo;

namespace
{
    const std::string PLACEHOLDER_VENDOR("Vulkan");
    const std::string PLACEHOLDER_RENDERER("Vulkan Device");
    const std::string PLACEHOLDER_VERSION("Vulkan");
}

const std::string& LLGPUInfo::vendorOrPlaceholder() const
{
    return mVendor.empty() ? PLACEHOLDER_VENDOR : mVendor;
}

const std::string& LLGPUInfo::rendererOrPlaceholder() const
{
    return mRenderer.empty() ? PLACEHOLDER_RENDERER : mRenderer;
}

const std::string& LLGPUInfo::apiVersionOrPlaceholder() const
{
    return mAPIVersion.empty() ? PLACEHOLDER_VERSION : mAPIVersion;
}

void LLGPUInfo::getInfo(LLSD& info) const
{
    info["GLInfo"]["GLVendor"]   = vendorOrPlaceholder();
    info["GLInfo"]["GLRenderer"] = rendererOrPlaceholder();
    info["GLInfo"]["GLVersion"]  = apiVersionOrPlaceholder();
    if (!mDriverVersion.empty())
    {
        info["GLInfo"]["DriverVersion"] = mDriverVersion;
    }
}

std::string LLGPUInfo::getInfoString() const
{
    std::string info_str;
    info_str += std::string("GPU_VENDOR     ") + vendorOrPlaceholder() + std::string("\n");
    info_str += std::string("GPU_RENDERER   ") + rendererOrPlaceholder() + std::string("\n");
    info_str += std::string("GPU_API        ") + apiVersionOrPlaceholder() + std::string("\n");
    if (!mDriverVersion.empty())
    {
        info_str += std::string("GPU_DRIVER     ") + mDriverVersion + std::string("\n");
    }
    return info_str;
}

void LLGPUInfo::printInfo() const
{
    LL_INFOS("RenderInit") << "GPU_VENDOR:    " << vendorOrPlaceholder() << LL_ENDL;
    LL_INFOS("RenderInit") << "GPU_RENDERER:  " << rendererOrPlaceholder() << LL_ENDL;
    LL_INFOS("RenderInit") << "GPU_API:       " << apiVersionOrPlaceholder() << LL_ENDL;
    if (!mDriverVersion.empty())
    {
        LL_INFOS("RenderInit") << "GPU_DRIVER:    " << mDriverVersion << LL_ENDL;
    }
}

std::string LLGPUInfo::getRawString() const
{
    return vendorOrPlaceholder() + " " + rendererOrPlaceholder();
}
