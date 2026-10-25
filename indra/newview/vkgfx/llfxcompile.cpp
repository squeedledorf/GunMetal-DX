/**
 * @file llfxcompile.cpp
 * @brief ReShade FX compile front end (#543 / #544). See llfxcompile.h.
 */
#include "llfxcompile.h"

#include "effect_codegen.hpp"
#include "effect_parser.hpp"
#include "effect_preprocessor.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>

namespace fs = std::filesystem;

namespace
{
// Paths come in as UTF-8 (the viewer's std::string convention). On Windows a plain
// std::string -> path conversion uses the ANSI code page, so go through char8_t.
fs::path utf8Path(const std::string& s)
{
#if defined(_WIN32)
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
#else
    return fs::path(s);
#endif
}

std::string pathUtf8(const fs::path& p)
{
#if defined(_WIN32)
    const std::u8string u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
#else
    return p.string();
#endif
}

std::string fmtFloat(float f)
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.6f", (double)f);
    return buf;
}

// ReShade's compatibility conversions for effects written against older runtimes
// (the same set its runtime prepends; these are API shims, not effect code).
const char* kCompatPrelude =
    "#define tex2Doffset(s, coords, offset) tex2D(s, coords, offset)\n"
    "#define tex2Dlodoffset(s, coords, offset) tex2Dlod(s, coords, offset)\n"
    "#define tex2Dgather(s, t, c) tex2Dgather##c(s, t)\n"
    "#define tex2Dgatheroffset(s, t, o, c) tex2Dgather##c(s, t, o)\n"
    "#define tex2Dgather0 tex2DgatherR\n"
    "#define tex2Dgather1 tex2DgatherG\n"
    "#define tex2Dgather2 tex2DgatherB\n"
    "#define tex2Dgather3 tex2DgatherA\n";
}

namespace slfx
{
std::vector<std::pair<std::string, std::string>> predefinedMacros(const CompileRequest& req)
{
    std::vector<std::pair<std::string, std::string>> m;
    m.emplace_back("__RESHADE__", std::to_string(kReShadeVersion));
    m.emplace_back("__RESHADE_PERMUTATION__", "0");
    m.emplace_back("__RESHADE_PERFORMANCE_MODE__", "0");
    m.emplace_back("__VENDOR__", std::to_string(req.vendorId));
    m.emplace_back("__DEVICE__", std::to_string(req.deviceId));
    m.emplace_back("__RENDERER__", std::to_string(req.rendererId));
    m.emplace_back("__APPLICATION__", std::to_string(req.applicationId));
    m.emplace_back("__SLIPSTREAM__", std::to_string(kSlipstreamFxVersion));
    m.emplace_back("BUFFER_WIDTH", std::to_string(req.width));
    m.emplace_back("BUFFER_HEIGHT", std::to_string(req.height));
    m.emplace_back("BUFFER_RCP_WIDTH", "(1.0 / BUFFER_WIDTH)");
    m.emplace_back("BUFFER_RCP_HEIGHT", "(1.0 / BUFFER_HEIGHT)");
    m.emplace_back("BUFFER_COLOR_SPACE", std::to_string(req.colorSpace));
    m.emplace_back("BUFFER_COLOR_FORMAT", std::to_string(req.colorFormat));
    m.emplace_back("BUFFER_COLOR_BIT_DEPTH", std::to_string(req.colorBitDepth));
    // Slipstream's DepthBuffer convention (llfxruntimevk.cpp, fx_depth.comp): display
    // resolution, unjittered, reversed (near 1, far 0), encoded so ReShade.fxh's
    // GetLinearizedDepth() returns view depth / FAR_PLANE exactly. Forced: presets carry
    // game-specific values for these that do not describe our depth.
    m.emplace_back("RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN", "0");
    m.emplace_back("RESHADE_DEPTH_INPUT_IS_REVERSED", "1");
    m.emplace_back("RESHADE_DEPTH_INPUT_IS_MIRRORED", "0");
    m.emplace_back("RESHADE_DEPTH_INPUT_IS_LOGARITHMIC", "0");
    m.emplace_back("RESHADE_DEPTH_MULTIPLIER", "1");
    m.emplace_back("RESHADE_DEPTH_LINEARIZATION_FAR_PLANE", fmtFloat(req.depthFarPlane));
    m.emplace_back("RESHADE_DEPTH_INPUT_Y_SCALE", "1");
    m.emplace_back("RESHADE_DEPTH_INPUT_X_SCALE", "1");
    m.emplace_back("RESHADE_DEPTH_INPUT_Y_OFFSET", "0");
    m.emplace_back("RESHADE_DEPTH_INPUT_X_OFFSET", "0");
    m.emplace_back("RESHADE_DEPTH_INPUT_Y_PIXEL_OFFSET", "0");
    m.emplace_back("RESHADE_DEPTH_INPUT_X_PIXEL_OFFSET", "0");
    return m;
}

bool compile(const CompileRequest& req, CompileResult& out)
{
    const auto t0 = std::chrono::steady_clock::now();
    out = CompileResult{};
    try
    {
        reshadefx::preprocessor pp;
        const fs::path file = utf8Path(req.path);
        pp.add_include_path(file.parent_path());
        for (const std::string& dir : req.includeDirs)
            if (!dir.empty()) pp.add_include_path(utf8Path(dir));
        std::vector<std::pair<std::string, std::string>> predefined = predefinedMacros(req);
        for (const auto& d : predefined) pp.add_macro_definition(d.first, d.second);
        for (const auto& d : req.defines)
        {
            if (d.first.empty()) continue;
            bool forced = false;
            for (const auto& p : predefined) forced = forced || p.first == d.first;
            if (forced) continue;   // the runtime's values win (see predefinedMacros)
            pp.add_macro_definition(d.first, d.second.empty() ? "1" : d.second);
        }
        pp.append_string(kCompatPrelude);
        if (!pp.append_file(file))
        {
            out.errors = pp.errors();
            if (out.errors.empty()) out.errors = "error: cannot read " + req.path;
            out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            return false;
        }
        for (const fs::path& p : pp.included_files()) out.includedFiles.push_back(pathUtf8(p));
        for (const auto& d : pp.used_macro_definitions())
        {
            const std::string& n = d.first;
            if (n.size() < 8 || n[0] == '_' || n.compare(0, 7, "BUFFER_") == 0 || n.compare(0, 8, "RESHADE_") == 0
                || n.find("INCLUDE_") != std::string::npos)
                continue;
            out.usedDefines.push_back(d);
        }

        std::unique_ptr<reshadefx::codegen> cg(reshadefx::create_codegen_spirv(true, false, false, false, false));
        reshadefx::parser parser;
        const bool parsed = parser.parse(pp.output(), cg.get());
        out.errors = pp.errors() + parser.errors();
        if (!parsed)
        {
            out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            return false;
        }
        out.module = cg->module();
        for (const auto& ep : out.module.entry_points)
        {
            std::string bin, assembly, errs;
            if (!cg->assemble_code_for_entry_point(ep.first, bin, assembly, errs) || bin.size() < 20 || (bin.size() & 3u))
            {
                out.errors += "error: SPIR-V assembly failed for entry point " + ep.first + (errs.empty() ? "" : ": " + errs) + "\n";
                out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                return false;
            }
            std::vector<uint32_t> words(bin.size() / 4);
            std::memcpy(words.data(), bin.data(), bin.size());
            out.spirv.emplace(ep.first, std::move(words));
        }
        out.ok = true;
    }
    catch (const std::exception& e)
    {
        out.ok = false;
        out.errors += std::string("error: internal compiler exception: ") + e.what() + "\n";
    }
    catch (...)
    {
        out.ok = false;
        out.errors += "error: internal compiler exception\n";
    }
    out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return out.ok;
}

const reshadefx::annotation* findAnnotation(const std::vector<reshadefx::annotation>& anns, const char* name)
{
    for (const reshadefx::annotation& a : anns)
        if (a.name == name) return &a;
    return nullptr;
}

std::string annotationString(const std::vector<reshadefx::annotation>& anns, const char* name, const std::string& def)
{
    const reshadefx::annotation* a = findAnnotation(anns, name);
    if (!a || a->type.base != reshadefx::type::t_string) return def;
    return a->value.string_data;
}

bool annotationBool(const std::vector<reshadefx::annotation>& anns, const char* name, bool def)
{
    const reshadefx::annotation* a = findAnnotation(anns, name);
    if (!a) return def;
    if (a->type.is_floating_point()) return a->value.as_float[0] != 0.f;
    if (a->type.is_numeric()) return a->value.as_int[0] != 0;
    return def;
}

int annotationInt(const std::vector<reshadefx::annotation>& anns, const char* name, int def)
{
    const reshadefx::annotation* a = findAnnotation(anns, name);
    if (!a) return def;
    if (a->type.is_floating_point()) return (int)a->value.as_float[0];
    if (a->type.is_numeric()) return a->value.as_int[0];
    return def;
}

float annotationFloat(const std::vector<reshadefx::annotation>& anns, const char* name, float def)
{
    const reshadefx::annotation* a = findAnnotation(anns, name);
    if (!a) return def;
    if (a->type.is_floating_point()) return a->value.as_float[0];
    if (a->type.is_numeric()) return (float)a->value.as_int[0];
    return def;
}
}
