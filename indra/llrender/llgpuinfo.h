/**
 * @file llgpuinfo.h
 * @brief GPU / driver identity of the Vulkan device (replaces LLGLManager's
 *        identity fields; there is no GL context).
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

#ifndef LL_LLGPUINFO_H
#define LL_LLGPUINFO_H

#include "stdtypes.h"
#include <string>

class LLSD;

// Filled once by the viewer's Vulkan bring-up (initVulkanForRealUI) from
// VkPhysicalDeviceProperties / limits / memory heaps. Until then the strings are
// empty and the accessors report placeholders ("Vulkan", "Vulkan Device").
// Plain data: llrender has no Vulkan dependency.
class LLGPUInfo
{
public:
    std::string mVendor;          // short vendor name: "NVIDIA", "ATI" (AMD), "Intel", else "Vulkan"
    std::string mRenderer;        // device name, e.g. "NVIDIA GeForce RTX 4070"
    std::string mAPIVersion;      // "Vulkan 1.3.280" (device API version)
    std::string mDriverVersion;   // driver name + version / info, e.g. "NVIDIA 570.86.16"
    bool mIsNVIDIA = false;
    bool mIsAMD = false;
    bool mIsIntel = false;
    U32 mVRAM = 0;                // MB, largest device-local heap
    S32 mMaxTextureSize = 0;      // maxImageDimension2D
    F32 mMaxAnisotropy = 0.f;     // 0 when samplerAnisotropy is unsupported

    bool isKnown() const { return !mRenderer.empty(); }
    bool hasAnisotropic() const { return mMaxAnisotropy >= 2.f; }

    const std::string& vendorOrPlaceholder() const;
    const std::string& rendererOrPlaceholder() const;
    const std::string& apiVersionOrPlaceholder() const;

    // gDebugInfo["GLInfo"] (crash reports; key names kept from the GL era).
    void getInfo(LLSD& info) const;
    std::string getInfoString() const;
    void printInfo() const;
    // "<vendor> <renderer>", sent to the simulator in the login ViewerStats blob.
    std::string getRawString() const;
};

extern LLGPUInfo gGPUInfo;

#endif // LL_LLGPUINFO_H
