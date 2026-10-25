/**
 * @file llfxcompile.h
 * @brief ReShade FX compile front end (#543 / #544): preprocess + parse + SPIR-V for one
 *        .fx file, exactly as Slipstream's effects runtime configures reshadefx.
 *
 * Pure C++ (reshadefx + the standard library): no Vulkan, no viewer globals, so it runs
 * on any thread (the runtime's compile worker) and in the slfxc developer tool.
 *
 * CODEGEN CONFIGURATION (matches ReShade's own Vulkan runtime, v6.8.0 runtime.cpp):
 *   create_codegen_spirv(vulkan_semantics = true, debug_info = false,
 *                        uniforms_to_spec_constants = false, enable_16bit_types = false,
 *                        flip_vert_y = false)
 * Y orientation is handled the way ReShade's Vulkan backend does it: NO flip in the
 * shaders, and every effect pass is drawn with a NEGATIVE-HEIGHT viewport
 * (y = height, height = -height; core since Vulkan 1.1). Do not combine with
 * flip_vert_y = true (that would flip twice). Uniforms stay in the UBO (set 0,
 * binding 0) so the UI can edit them live without a recompile.
 *
 * DESCRIPTOR CONTRACT of the generated SPIR-V (per pass, from the codegen):
 *   set 0 binding 0                       the effect's uniform block ("$Globals"; may be absent)
 *   set 1 binding entry_point_binding     combined image samplers (pass.sampler_bindings)
 *   set 2 binding entry_point_binding     storage images (pass.storage_bindings)
 *
 * PREDEFINED MACROS (ReShade's set, plus ours; see compile()):
 *   __RESHADE__ 60800, __RESHADE_PERMUTATION__ 0, __RESHADE_PERFORMANCE_MODE__ 0,
 *   __VENDOR__, __DEVICE__, __RENDERER__ (0x20000 | Vulkan API version, e.g. 0x21300),
 *   __APPLICATION__, BUFFER_WIDTH / HEIGHT / RCP_WIDTH / RCP_HEIGHT,
 *   BUFFER_COLOR_SPACE (1 sRGB, 2 scRGB / linear HDR point), BUFFER_COLOR_FORMAT,
 *   BUFFER_COLOR_BIT_DEPTH, __SLIPSTREAM__ (extension level, see kSlipstreamFxVersion),
 *   and the RESHADE_DEPTH_* convention of Slipstream's DepthBuffer (forced: the depth
 *   input is ours, so a preset's game-specific depth flags must not apply to it).
 * Then ReShade's compatibility prelude (tex2Doffset / tex2Dlodoffset / tex2Dgather*).
 */
#ifndef LL_LLFXCOMPILE_H
#define LL_LLFXCOMPILE_H

#include "effect_module.hpp"   // reshadefx (vkgfx/third_party/reshadefx/src)

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace slfx
{
// __SLIPSTREAM__: bump when an extension (texture semantic, annotation) is added.
// 1 = SLIPSTREAM_NORMAL / SLIPSTREAM_MOTION / SLIPSTREAM_AVATAR_MASK / SLIPSTREAM_SCENE_HDR
//     semantics, technique annotation slipstream_point = "hdr".
constexpr uint32_t kSlipstreamFxVersion = 1;
// What __RESHADE__ reports: the language level of the vendored compiler (6.8.0).
constexpr uint32_t kReShadeVersion = 60800;

// ReShade's api::format values (DXGI numbering) for BUFFER_COLOR_FORMAT.
enum : uint32_t
{
    FX_FORMAT_R8G8B8A8_UNORM = 28,
    FX_FORMAT_B8G8R8A8_UNORM = 87,
    FX_FORMAT_R16G16B16A16_FLOAT = 10,
};

struct CompileRequest
{
    std::string path;                                          // the .fx file (UTF-8)
    std::vector<std::string> includeDirs;                      // UTF-8, searched in order after the file's dir
    std::vector<std::pair<std::string, std::string>> defines;  // global + per-effect (preset) definitions
    uint32_t width = 1920, height = 1080;                      // BUFFER_WIDTH / BUFFER_HEIGHT
    uint32_t colorSpace = 1;                                   // 1 sRGB (display point), 2 scRGB (HDR point)
    uint32_t colorFormat = FX_FORMAT_B8G8R8A8_UNORM;
    uint32_t colorBitDepth = 8;
    float    depthFarPlane = 1000.f;                           // RESHADE_DEPTH_LINEARIZATION_FAR_PLANE
    uint32_t vendorId = 0, deviceId = 0;
    uint32_t rendererId = 0x21300;                             // 0x20000 | VK_API_VERSION major/minor
    uint32_t applicationId = 0;
};

struct CompileResult
{
    bool ok = false;
    std::string errors;     // preprocessor + parser messages (errors and warnings), "" when clean
    reshadefx::effect_module module;
    // One SPIR-V module per entry point (as 32-bit words), keyed by the entry point's
    // unique name (pass.vs_entry_point / ps_entry_point / cs_entry_point).
    std::unordered_map<std::string, std::vector<uint32_t>> spirv;
    std::vector<std::pair<std::string, std::string>> usedDefines;   // effect-tunable #ifdef/#ifndef names
    std::vector<std::string> includedFiles;                          // UTF-8
    double ms = 0.0;        // wall time of the whole compile
};

// Preprocess, parse and assemble every entry point. Thread-safe (no shared state).
// Never throws for bad input: failures come back as ok = false + errors.
bool compile(const CompileRequest& req, CompileResult& out);

// The predefined macros compile() sets, in order (for the UI's definition list / dumps).
std::vector<std::pair<std::string, std::string>> predefinedMacros(const CompileRequest& req);

// Annotation helpers (reshadefx::annotation lists of textures / uniforms / techniques).
const reshadefx::annotation* findAnnotation(const std::vector<reshadefx::annotation>& anns, const char* name);
std::string annotationString(const std::vector<reshadefx::annotation>& anns, const char* name,
                             const std::string& def = std::string());
bool annotationBool(const std::vector<reshadefx::annotation>& anns, const char* name, bool def);
int annotationInt(const std::vector<reshadefx::annotation>& anns, const char* name, int def);
float annotationFloat(const std::vector<reshadefx::annotation>& anns, const char* name, float def);
}

#endif
