/**
 * @file slfxstack.cpp
 * @brief The built-in ReShade FX compatible effects stack (#542): user-facing state.
 *        See slfxstack.h.
 */
#include "llviewerprecompiledheaders.h"

#include "slfxstack.h"
#include "slfxpresets.h"

#include "llcallbacklist.h"
#include "lldir.h"
#include "llfile.h"
#include "llkeyboard.h"
#include "llnotificationsutil.h"
#include "llsdserialize.h"
#include "llstartup.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"
#include "vkgfx/llfxruntimevk.h"

#if defined(_WIN32)
#include "llwin32headers.h"
#include <tlhelp32.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace fs = std::filesystem;

namespace
{
const char* kStateFile = "fx_state.xml";
constexpr F32 kSaveDelay = 1.f;   // seconds after the last change

fs::path u8(const std::string& s)
{
#if defined(_WIN32)
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
#else
    return fs::path(s);
#endif
}

std::string fromPath(const fs::path& p)
{
#if defined(_WIN32)
    const std::u8string u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
#else
    return p.string();
#endif
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::string baseName(const std::string& p)
{
    const size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}

// Windows virtual-key code (ReShade's "keycode" annotation) -> LL KEY.
KEY vkToKey(int vk)
{
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return (KEY)vk;
    if (vk >= 0x70 && vk <= 0x7B) return (KEY)(KEY_F1 + (vk - 0x70));
    switch (vk)
    {
    case 0x08: return KEY_BACKSPACE;
    case 0x09: return KEY_TAB;
    case 0x0D: return KEY_RETURN;
    case 0x10: return KEY_SHIFT;
    case 0x11: return KEY_CONTROL;
    case 0x12: return KEY_ALT;
    case 0x14: return KEY_CAPSLOCK;
    case 0x1B: return KEY_ESCAPE;
    case 0x20: return ' ';
    case 0x21: return KEY_PAGE_UP;
    case 0x22: return KEY_PAGE_DOWN;
    case 0x23: return KEY_END;
    case 0x24: return KEY_HOME;
    case 0x25: return KEY_LEFT;
    case 0x26: return KEY_UP;
    case 0x27: return KEY_RIGHT;
    case 0x28: return KEY_DOWN;
    case 0x2D: return KEY_INSERT;
    case 0x2E: return KEY_DELETE;
    default: return 0;
    }
}

LLSD llsdFromConstant(const reshadefx::type& t, const reshadefx::constant& c, bool asColor)
{
    const uint32_t n = std::max(1u, (uint32_t)t.rows * std::max(1u, (uint32_t)t.cols));
    auto one = [&](uint32_t i) -> LLSD {
        if (t.is_boolean()) return LLSD(c.as_uint[i] != 0);
        if (t.is_floating_point()) return LLSD((F64)c.as_float[i]);
        if (t.is_signed()) return LLSD((S32)c.as_int[i]);
        return LLSD((S32)c.as_uint[i]);
    };
    if (n == 1 && !asColor) return one(0);
    LLSD a = LLSD::emptyArray();
    for (uint32_t i = 0; i < std::min(n, 16u); ++i) a.append(t.is_floating_point() ? one(i) : LLSD(one(i).asReal()));
    return a;
}

LLSD annotationLLSD(const std::vector<reshadefx::annotation>& anns, const char* name)
{
    const reshadefx::annotation* a = slfx::findAnnotation(anns, name);
    if (!a || a->type.base == reshadefx::type::t_string) return LLSD();
    const uint32_t n = std::max(1u, (uint32_t)a->type.rows);
    auto one = [&](uint32_t i) -> LLSD {
        return a->type.is_floating_point() ? LLSD((F64)a->value.as_float[i]) : LLSD((F64)a->value.as_int[i]);
    };
    if (n == 1) return one(0);
    LLSD arr = LLSD::emptyArray();
    for (uint32_t i = 0; i < std::min(n, 4u); ++i) arr.append(one(i));
    return arr;
}
}

// ============================================================================
SLFxStack::SLFxStack() = default;
SLFxStack::~SLFxStack() = default;

void SLFxStack::initSingleton()
{
    loadState();
    checkStartupMarkers();   // #546: after a GPU loss / dead session, FX off + quarantine
    if (LLControlVariable* c = gSavedSettings.getControl("SLFxEnabled"))
        mEnabledConn = c->getSignal()->connect([this](LLControlVariable*, const LLSD&, const LLSD&) {
            LLFxRuntimeVK::instance().setEnabled(isEnabled());
            if (isEnabled()) { rescan(); requestAllMetadata(); }
            fireChanged();
        });
    LLFxRuntimeVK::instance().setEnabled(isEnabled());
    if (isEnabled()) rescan();
    pushPlan();
    gIdleCallbacks.addFunction(&SLFxStack::onIdle, this);
    SLFxPresets::instance().init();   // #545: restore the active preset (needs SLFxStack to exist)
}

void SLFxStack::cleanupSingleton()
{
    gIdleCallbacks.deleteFunction(&SLFxStack::onIdle, this);
    mEnabledConn.disconnect();
    if (mDirty) saveState();
    // A clean exit: provisional effects did not take the session down (#546).
    LLFile::remove(gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "fx_provisional.xml"));
}

// ============================================================================
// Folders and discovery
// ============================================================================
std::string SLFxStack::getEffectsDir() const
{
    const std::string custom = gSavedSettings.getString("SLFxEffectsDir");
    if (!custom.empty()) return custom;
    return gDirUtilp->getOSUserAppDir() + gDirUtilp->getDirDelimiter() + "effects";
}

std::vector<std::string> SLFxStack::getSearchPaths() const
{
    std::vector<std::string> out;
    const LLSD paths = gSavedSettings.getLLSD("SLFxSearchPaths");
    for (LLSD::array_const_iterator it = paths.beginArray(); it != paths.endArray(); ++it)
        if (!it->asString().empty()) out.push_back(it->asString());
    return out;
}

void SLFxStack::setSearchPaths(const std::vector<std::string>& paths)
{
    LLSD a = LLSD::emptyArray();
    for (const std::string& p : paths) a.append(p);
    gSavedSettings.setLLSD("SLFxSearchPaths", a);
    rescan();
    pushPlan();
    fireChanged();
}

void SLFxStack::scanDirs()
{
    std::vector<std::string> roots;
    const std::string mine = getEffectsDir();
    {
        // First run: create <user app dir>/effects/{Shaders,Textures} so users know where to put files.
        std::error_code ec;
        fs::create_directories(u8(mine) / "Shaders", ec);
        fs::create_directories(u8(mine) / "Textures", ec);
    }
    roots.push_back(mine);
    for (const std::string& p : getSearchPaths()) roots.push_back(p);
    // The bundled starter pack (#957) last: a file of the same name in a user folder wins.
    roots.push_back(gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "fx", "pack"));

    mIncludeDirs.clear();
    mTextureDirs.clear();
    std::map<std::string, Effect> found;
    std::set<std::string> seenNames;
    for (const std::string& root : roots)
    {
        std::error_code ec;
        fs::path r = u8(root);
        if (!fs::is_directory(r, ec)) continue;
        // ReShade layout (Shaders / Textures) or a bare folder of effects.
        fs::path shaders = fs::is_directory(r / "Shaders", ec) ? r / "Shaders" : r;
        fs::path textures = fs::is_directory(r / "Textures", ec) ? r / "Textures" : r;
        auto addDir = [](std::vector<std::string>& v, const fs::path& d) {
            const std::string s = fromPath(d);
            if (std::find(v.begin(), v.end(), s) == v.end()) v.push_back(s);
        };
        addDir(mIncludeDirs, shaders);
        addDir(mTextureDirs, textures);
        for (fs::recursive_directory_iterator it(shaders, fs::directory_options::skip_permission_denied, ec), end;
             it != end && !ec; it.increment(ec))
        {
            if (it.depth() > 4) { it.disable_recursion_pending(); continue; }
            if (it->is_directory(ec)) { addDir(mIncludeDirs, it->path()); continue; }
            if (lower(fromPath(it->path().extension())) != ".fx") continue;
            const std::string path = fromPath(it->path());
            const std::string name = fromPath(it->path().filename());
            if (seenNames.count(lower(name))) continue;   // first folder wins (ReShade ids are by file name)
            seenNames.insert(lower(name));
            Effect e;
            e.path = path; e.name = name;
            auto old = mEffects.find(path);
            if (old != mEffects.end()) e = old->second;
            found[path] = e;
        }
        for (fs::recursive_directory_iterator it(textures, fs::directory_options::skip_permission_denied, ec), end;
             it != end && !ec; it.increment(ec))
        {
            if (it.depth() > 4) { it.disable_recursion_pending(); continue; }
            if (it->is_directory(ec)) addDir(mTextureDirs, it->path());
        }
    }
    // The viewer's own headers last (ReShade.fxh, ReShadeUI.fxh).
    mIncludeDirs.push_back(gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "fx", "Shaders"));
    // Keep state of effects that vanished (a folder may be offline); they are just not listed.
    for (auto& kv : mEffects)
        if (!found.count(kv.first) && (kv.second.values.size() || !kv.second.defines.empty() || kv.second.quarantined))
            mHiddenEffects[kv.first] = kv.second;
    for (auto& kv : found)
    {
        auto h = mHiddenEffects.find(kv.first);
        if (h != mHiddenEffects.end()) { kv.second = h->second; mHiddenEffects.erase(h); }
    }
    mEffects.swap(found);
    mScanned = true;
}

void SLFxStack::rescan()
{
    scanDirs();
    syncTechniqueList();
    mMetaRequested = false;
    if (isEnabled()) requestAllMetadata();
    pushPlan();
    fireChanged();
}

void SLFxStack::requestAllMetadata()
{
    if (mMetaRequested) return;
    mMetaRequested = true;
    // The runtime must have the folders scanDirs() found BEFORE the jobs are queued (a job
    // copies the runtime's include dirs). rescan(), reloadAll() and getTechniques() used to
    // request first and push the plan after: on a start with FX on every effect then
    // failed with "could not open included file 'ReShade.fxh'" (the runtime's dirs were
    // still empty) and setPlan() dropped the jobs as stale.
    pushPlan();
    for (const auto& kv : mEffects)
        if (!kv.second.quarantined) LLFxRuntimeVK::instance().requestMetadata(kv.first);
}

std::vector<SLFxEffectInfo> SLFxStack::getAvailableEffects()
{
    if (!mScanned) scanDirs();
    std::vector<SLFxEffectInfo> out;
    for (const auto& kv : mEffects)
    {
        const LLFxRuntimeVK::EffectStatus st = LLFxRuntimeVK::instance().status(kv.first);
        SLFxEffectInfo i;
        i.mPath = kv.first; i.mName = kv.second.name;
        // A metadata-only compile (effect not active yet) leaves compiled false but carries the
        // module: that is a successful compile too, not a failure.
        i.mLoaded = !st.failed && (st.compiled || st.meta != nullptr);
        i.mCompiling = st.compiling;
        // Split the compiler's messages: lines with "error" vs the rest (warnings).
        std::string err, warn;
        size_t a = 0;
        while (a < st.errors.size())
        {
            size_t b = st.errors.find('\n', a);
            if (b == std::string::npos) b = st.errors.size();
            const std::string line = st.errors.substr(a, b - a);
            if (!line.empty()) ((line.find("error") != std::string::npos) ? err : warn) += line + "\n";
            a = b + 1;
        }
        i.mError = err;
        i.mWarnings = warn;
        i.mQuarantined = kv.second.quarantined;
        if (kv.second.quarantined)
            i.mError = "Quarantined: " + kv.second.quarantineReason + " (enable one of its techniques to try it again)\n" + i.mError;
        i.mVramMB = (U32)(st.vramBytes >> 20);
        out.push_back(i);
    }
    std::sort(out.begin(), out.end(), [](const SLFxEffectInfo& x, const SLFxEffectInfo& y) { return lower(x.mName) < lower(y.mName); });
    return out;
}

SLFxStack::Effect* SLFxStack::findEffect(const std::string& effect_path)
{
    auto it = mEffects.find(effect_path);
    if (it != mEffects.end()) return &it->second;
    const std::string n = lower(baseName(effect_path));
    for (auto& kv : mEffects) if (lower(kv.second.name) == n) return &kv.second;
    return nullptr;
}

const SLFxStack::Effect* SLFxStack::findEffect(const std::string& effect_path) const
{
    return const_cast<SLFxStack*>(this)->findEffect(effect_path);
}

std::string SLFxStack::resolveEffect(const std::string& effect_path) const
{
    const Effect* e = findEffect(effect_path);
    return e ? e->path : std::string();
}

// ============================================================================
// Techniques
// ============================================================================
std::string SLFxStack::techniqueId(const std::string& effect_path, const std::string& tech) const
{
    return tech + "@" + baseName(effect_path);
}

void SLFxStack::syncTechniqueList()
{
    // Append newly seen techniques (compiled effects) at the end, in file / declaration order.
    std::vector<std::string> add;
    for (const auto& kv : mEffects)
    {
        const LLFxRuntimeVK::EffectStatus st = LLFxRuntimeVK::instance().status(kv.first);
        if (!st.meta) continue;
        for (const reshadefx::technique& t : st.meta->module.techniques)
        {
            const std::string id = techniqueId(kv.first, t.name);
            if (std::find(mOrder.begin(), mOrder.end(), id) == mOrder.end()) add.push_back(id);
        }
    }
    if (!add.empty())
    {
        mOrder.insert(mOrder.end(), add.begin(), add.end());
        markDirty();
    }
}

std::vector<std::string> SLFxStack::resolveTechnique(const std::string& technique) const
{
    std::vector<std::string> ids;
    if (technique.find('@') != std::string::npos)
    {
        // Case-insensitive on the file part (presets are written on Windows).
        const size_t at = technique.find('@');
        const std::string nm = technique.substr(0, at), file = lower(technique.substr(at + 1));
        for (const std::string& id : mOrder)
        {
            const size_t a2 = id.find('@');
            if (id.substr(0, a2) == nm && lower(id.substr(a2 + 1)) == file) ids.push_back(id);
        }
        if (ids.empty()) ids.push_back(technique);   // not compiled yet: keep the preset's id
        return ids;
    }
    for (const std::string& id : mOrder)
        if (id.compare(0, technique.size() + 1, technique + "@") == 0) ids.push_back(id);
    return ids;
}

std::vector<SLFxTechniqueInfo> SLFxStack::getTechniques()
{
    if (!mScanned) scanDirs();
    requestAllMetadata();
    syncTechniqueList();
    std::vector<SLFxTechniqueInfo> out;
    for (const std::string& id : mOrder)
    {
        const size_t at = id.find('@');
        const std::string name = id.substr(0, at);
        const Effect* e = findEffect(id.substr(at + 1));
        if (!e) continue;
        const LLFxRuntimeVK::EffectStatus st = LLFxRuntimeVK::instance().status(e->path);
        if (!st.meta) continue;
        for (const reshadefx::technique& t : st.meta->module.techniques)
        {
            if (t.name != name) continue;
            SLFxTechniqueInfo i;
            i.mName = name; i.mEffectPath = e->path; i.mId = id;
            i.mEnabled = mEnabledTechs.count(id) > 0;
            i.mLabel = slfx::annotationString(t.annotations, "ui_label", name);
            i.mTooltip = slfx::annotationString(t.annotations, "ui_tooltip");
            i.mEnabledInScreenshot = slfx::annotationBool(t.annotations, "enabled_in_screenshot", true);
            i.mHdrPoint = e->point == POINT_HDR || slfx::annotationString(t.annotations, "slipstream_point") == "hdr";
            i.mGpuMs = LLFxRuntimeVK::instance().techniqueGpuMs(e->path, name);
            out.push_back(i);
            break;
        }
    }
    return out;
}

bool SLFxStack::setTechniqueEnabled(const std::string& technique, bool enabled)
{
    const std::vector<std::string> ids = resolveTechnique(technique);
    if (ids.empty()) return false;
    for (const std::string& id : ids)
    {
        if (enabled)
        {
            mEnabledTechs.insert(id);
            if (std::find(mOrder.begin(), mOrder.end(), id) == mOrder.end()) mOrder.push_back(id);
            // Re-enabling lifts a quarantine (#546): the user asked for it.
            if (Effect* e = findEffect(id.substr(id.find('@') + 1)))
                if (e->quarantined)
                {
                    e->quarantined = false;
                    e->quarantineReason.clear();
                    LLFxRuntimeVK::instance().reloadEffect(e->path);
                }
        }
        else
            mEnabledTechs.erase(id);
    }
    markDirty();
    pushPlan();
    fireChanged();
    return true;
}

bool SLFxStack::setTechniqueOrder(const std::vector<std::string>& ordered)
{
    std::vector<std::string> next;
    for (const std::string& t : ordered)
        for (const std::string& id : resolveTechnique(t))
            if (std::find(next.begin(), next.end(), id) == next.end()) next.push_back(id);
    for (const std::string& id : mOrder)
        if (std::find(next.begin(), next.end(), id) == next.end()) next.push_back(id);
    mOrder.swap(next);
    markDirty();
    pushPlan();
    fireChanged();
    return true;
}

// ============================================================================
// Uniforms
// ============================================================================
std::vector<SLFxUniformInfo> SLFxStack::getUniforms(const std::string& effect_path)
{
    std::vector<SLFxUniformInfo> out;
    Effect* e = findEffect(effect_path);
    if (!e) return out;
    const LLFxRuntimeVK::EffectStatus st = LLFxRuntimeVK::instance().status(e->path);
    if (!st.meta)
    {
        if (!st.compiling && !st.failed) LLFxRuntimeVK::instance().requestMetadata(e->path);
        return out;   // the changed callback fires when it arrives
    }
    for (const reshadefx::uniform& u : st.meta->module.uniforms)
    {
        if (!slfx::annotationString(u.annotations, "source").empty()) continue;   // runtime-fed
        SLFxUniformInfo i;
        i.mName = u.name;
        i.mUIType = slfx::annotationString(u.annotations, "ui_type");
        const uint32_t rows = std::max(1u, (uint32_t)u.type.rows);
        const bool color = i.mUIType == "color" && u.type.is_floating_point() && (rows == 3 || rows == 4);
        if (u.type.is_boolean()) i.mType = "bool";
        else if (color) i.mType = rows == 3 ? "color3" : "color4";
        else if (u.type.is_floating_point()) i.mType = rows == 1 ? "float" : "float" + std::to_string(rows);
        else i.mType = rows == 1 ? "int" : "int" + std::to_string(rows);
        i.mLabel = slfx::annotationString(u.annotations, "ui_label", u.name);
        i.mTooltip = slfx::annotationString(u.annotations, "ui_tooltip");
        i.mCategory = slfx::annotationString(u.annotations, "ui_category");
        i.mMin = annotationLLSD(u.annotations, "ui_min");
        i.mMax = annotationLLSD(u.annotations, "ui_max");
        i.mStep = annotationLLSD(u.annotations, "ui_step");
        const std::string items = slfx::annotationString(u.annotations, "ui_items");
        size_t a = 0;
        while (a < items.size())
        {
            size_t b = items.find('\0', a);
            if (b == std::string::npos) b = items.size();
            i.mItems.push_back(items.substr(a, b - a));
            a = b + 1;
        }
        if (!items.empty() && items.back() == '\0' && !i.mItems.empty() && i.mItems.back().empty()) i.mItems.pop_back();
        i.mDefault = u.has_initializer_value ? llsdFromConstant(u.type, u.initializer_value, color)
                                             : (rows > 1 ? LLSD::emptyArray() : LLSD(0.0));
        i.mValue = e->values.has(u.name) ? e->values[u.name] : i.mDefault;
        i.mHidden = slfx::annotationBool(u.annotations, "hidden", false);
        i.mText = slfx::annotationString(u.annotations, "ui_text");
        i.mSpacing = slfx::annotationInt(u.annotations, "ui_spacing", 0);
        i.mUnits = slfx::annotationString(u.annotations, "ui_units");
        i.mNoEdit = slfx::annotationBool(u.annotations, "noedit", false);
        i.mNoReset = slfx::annotationBool(u.annotations, "noreset", false);
        i.mCategoryClosed = slfx::annotationBool(u.annotations, "ui_category_closed", false);
        out.push_back(i);
    }
    return out;
}

bool SLFxStack::setUniform(const std::string& effect_path, const std::string& name, const LLSD& value)
{
    Effect* e = findEffect(effect_path);
    if (!e || name.empty()) return false;
    if (e->values.isUndefined()) e->values = LLSD::emptyMap();
    e->values[name] = value;
    LLFxRuntimeVK::instance().setUniformValues(e->path, e->values);
    markDirty();
    fireChanged();
    return true;
}

bool SLFxStack::resetUniform(const std::string& effect_path, const std::string& name)
{
    Effect* e = findEffect(effect_path);
    if (!e || !e->values.has(name)) return false;
    e->values.erase(name);
    LLFxRuntimeVK::instance().setUniformValues(e->path, e->values);
    markDirty();
    fireChanged();
    return true;
}

// ============================================================================
// Definitions / scope / point
// ============================================================================
void SLFxStack::setPreprocessorDefinitions(const std::string& effect_path,
                                           const std::vector<std::pair<std::string, std::string>>& defs)
{
    if (effect_path.empty())
    {
        if (mGlobalDefines == defs) return;
        mGlobalDefines = defs;
    }
    else
    {
        Effect* e = findEffect(effect_path);
        if (!e || e->defines == defs) return;
        e->defines = defs;
    }
    markDirty();
    pushPlan();   // the runtime recompiles effects whose definitions changed
    fireChanged();
}

std::vector<std::pair<std::string, std::string>> SLFxStack::getPreprocessorDefinitions(const std::string& effect_path) const
{
    if (effect_path.empty()) return mGlobalDefines;
    const Effect* e = findEffect(effect_path);
    return e ? e->defines : std::vector<std::pair<std::string, std::string>>();
}

std::vector<std::pair<std::string, std::string>> SLFxStack::getUsedDefinitions(const std::string& effect_path)
{
    const Effect* e = findEffect(effect_path);
    if (!e) return {};
    const LLFxRuntimeVK::EffectStatus st = LLFxRuntimeVK::instance().status(e->path);
    return st.meta ? st.meta->usedDefines : std::vector<std::pair<std::string, std::string>>();
}

bool SLFxStack::setEffectScope(const std::string& effect_path, S32 scope, S32 feather_px)
{
    Effect* e = findEffect(effect_path);
    if (!e) return false;
    e->scope = llclamp(scope, (S32)SCOPE_ALL, (S32)SCOPE_NOT_AVATARS);
    e->feather = llclamp(feather_px, 0, 8);
    markDirty();
    pushPlan();
    fireChanged();
    return true;
}

S32 SLFxStack::getEffectScope(const std::string& effect_path) const
{
    const Effect* e = findEffect(effect_path);
    return e ? e->scope : (S32)SCOPE_ALL;
}

bool SLFxStack::setEffectPoint(const std::string& effect_path, S32 point)
{
    Effect* e = findEffect(effect_path);
    if (!e) return false;
    e->point = llclamp(point, (S32)POINT_DISPLAY, (S32)POINT_HDR);
    markDirty();
    pushPlan();
    fireChanged();
    return true;
}

S32 SLFxStack::getEffectPoint(const std::string& effect_path) const
{
    const Effect* e = findEffect(effect_path);
    return e ? e->point : (S32)POINT_DISPLAY;
}

void SLFxStack::setAvatarMaskProvider(std::function<VkImageView()> provider)
{
    LLFxRuntimeVK::setAvatarMaskProvider(std::move(provider));
}

// ============================================================================
// Master switch / reload / callbacks
// ============================================================================
void SLFxStack::setEnabled(bool enabled)
{
    gSavedSettings.setBOOL("SLFxEnabled", enabled);   // the control signal does the rest
}

bool SLFxStack::isEnabled() const
{
    return gSavedSettings.getBOOL("SLFxEnabled") && !gSavedSettings.getBOOL("SLFxNoFx");
}

void SLFxStack::reloadAll()
{
    scanDirs();
    LLFxRuntimeVK::instance().reloadAll();
    mMetaRequested = false;
    requestAllMetadata();
    pushPlan();
    fireChanged();
}

boost::signals2::connection SLFxStack::setChangedCallback(const std::function<void()>& cb)
{
    return mChanged.connect(cb);
}

void SLFxStack::fireChanged()
{
    mChanged();
}

void SLFxStack::warmSnapshotSize(S32 width, S32 height)
{
    if (width > 0 && height > 0) LLFxRuntimeVK::instance().warmSize((uint32_t)width, (uint32_t)height);
}

std::string SLFxStack::getLastSnapshotNote() const
{
    return LLFxRuntimeVK::instance().lastSnapshotNote();
}

SLFxSuppressScope::SLFxSuppressScope(const char* what)
{
    LLFxRuntimeVK::instance().pushSuppress();
    static bool s_told = false;
    if (!s_told && SLFxStack::instanceExists() && SLFxStack::getInstance()->isEnabled())
    {
        s_told = true;
        LL_INFOS("FX") << "effects suppressed for " << (what ? what : "a capture") << LL_ENDL;
        LLNotificationsUtil::add("SystemMessageTip", LLSD().with("MESSAGE",
            std::string("Effects are not applied to ") + (what ? what : "this capture")
            + ": screen-space effects would not line up across the cube faces. Cel-shaded lighting still applies."));
    }
}

SLFxSuppressScope::~SLFxSuppressScope()
{
    LLFxRuntimeVK::instance().popSuppress();
}

std::vector<std::string> SLFxStack::getQuarantined() const
{
    std::vector<std::string> out;
    for (const auto& kv : mEffects) if (kv.second.quarantined) out.push_back(kv.first);
    return out;
}

// ============================================================================
// Plan -> runtime
// ============================================================================
void SLFxStack::pushPlan()
{
    LLFxRuntimeVK& rt = LLFxRuntimeVK::instance();
    std::vector<LLFxRuntimeVK::PlanTech> plan;
    std::map<std::string, LLFxRuntimeVK::EffectConfig> cfgs;
    for (const std::string& id : mOrder)
    {
        if (!mEnabledTechs.count(id)) continue;
        const size_t at = id.find('@');
        const Effect* e = findEffect(id.substr(at + 1));
        if (!e || e->quarantined) continue;
        LLFxRuntimeVK::PlanTech t;
        t.effect = e->path;
        t.technique = id.substr(0, at);
        t.scope = (uint8_t)e->scope;
        t.feather = (uint8_t)e->feather;
        plan.push_back(t);
        LLFxRuntimeVK::EffectConfig& c = cfgs[e->path];
        c.defines = mGlobalDefines;
        for (const auto& d : e->defines)
        {
            auto it = std::find_if(c.defines.begin(), c.defines.end(), [&](const auto& x) { return x.first == d.first; });
            if (it != c.defines.end()) it->second = d.second; else c.defines.push_back(d);
        }
        c.point = (uint8_t)e->point;
    }
    // Configs for every known effect (the UI's reflection compiles use them too).
    for (const auto& kv : mEffects)
        if (!cfgs.count(kv.first))
        {
            LLFxRuntimeVK::EffectConfig& c = cfgs[kv.first];
            c.defines = mGlobalDefines;
            for (const auto& d : kv.second.defines) c.defines.push_back(d);
            c.point = (uint8_t)kv.second.point;
        }
    for (const auto& kv : mEffects)
        if (kv.second.values.isMap()) rt.setUniformValues(kv.first, kv.second.values);
    rt.setPlan(plan, cfgs, mIncludeDirs, mTextureDirs);
}

void SLFxStack::onIdle(void* self)
{
    static_cast<SLFxStack*>(self)->idle();
}

void SLFxStack::idle()
{
    LLFxRuntimeVK& rt = LLFxRuntimeVK::instance();
    // Per-frame inputs for the uniform "source" specials.
    LLFxRuntimeVK::Inputs in;
    if (gViewerWindow)
    {
        const S32 ww = gViewerWindow->getWindowWidthScaled(), wh = gViewerWindow->getWindowHeightScaled();
        in.windowW = (F32)llmax(1, ww); in.windowH = (F32)llmax(1, wh);
        in.mouseX = (F32)gViewerWindow->getCurrentMouseX();
        in.mouseY = (F32)(wh - gViewerWindow->getCurrentMouseY());
        in.mouseDown[0] = gViewerWindow->getLeftMouseDown();
        in.mouseDown[1] = gViewerWindow->getRightMouseDown();
        in.mouseDown[2] = gViewerWindow->getMiddleMouseDown();
    }
    in.keyDown = [](int vk) { const KEY k = vkToKey(vk); return k && gKeyboard && gKeyboard->getKeyDown(k); };
    in.farClip = LLViewerCamera::getInstance()->getFar();
    rt.setInputs(in);

    updateSafety();

    // A snapshot that went without (some) effects says why (#547).
    {
        static std::string s_lastNote;
        const std::string& note = rt.lastSnapshotNote();
        if (note != s_lastNote)
        {
            s_lastNote = note;
            if (!note.empty()) LLNotificationsUtil::add("SystemMessageTip", LLSD().with("MESSAGE", note));
        }
    }

    // New compile results: technique list + the UI.
    rt.tick();
    // The compile watchdog stopped an effect (llfxruntimevk.h, COMPILE WATCHDOG): quarantine
    // it (persisted, so the next session does not hang on it again); enabling one of its
    // techniques lifts it, as for the #546 quarantines.
    for (const auto& q : rt.takeQuarantineRequests())
    {
        Effect* e = findEffect(q.first);
        if (!e) continue;
        e->quarantined = true;
        e->quarantineReason = q.second;
        markDirty();
        pushPlan();
        LLNotificationsUtil::add("SystemMessageTip", LLSD().with("MESSAGE", "Effect " + e->name + ": " + q.second + "."));
    }
    const U64 gen = rt.metaGeneration();
    if (gen != mSeenMetaGen)
    {
        mSeenMetaGen = gen;
        const size_t before = mOrder.size();
        syncTechniqueList();
        if (mOrder.size() != before) pushPlan();
        fireChanged();
    }
    if (mDirty && mSaveTimer.getElapsedTimeF32() > kSaveDelay) saveState();
}

// ============================================================================
// Persistence
// ============================================================================
void SLFxStack::loadState()
{
    const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, kStateFile);
    llifstream f(path.c_str());
    if (!f.is_open()) return;
    LLSD sd;
    if (LLSDSerialize::fromXML(sd, f) <= 0 || !sd.isMap()) return;
    mOrder.clear();
    mEnabledTechs.clear();
    for (const LLSD& t : llsd::inArray(sd["techniques"]))
    {
        const std::string id = t["id"].asString();
        if (id.empty() || std::find(mOrder.begin(), mOrder.end(), id) != mOrder.end()) continue;
        mOrder.push_back(id);
        if (t["enabled"].asBoolean()) mEnabledTechs.insert(id);
    }
    mGlobalDefines.clear();
    for (const LLSD& d : llsd::inArray(sd["defines"])) mGlobalDefines.emplace_back(d[0].asString(), d[1].asString());
    for (LLSD::map_const_iterator it = sd["effects"].beginMap(); it != sd["effects"].endMap(); ++it)
    {
        Effect e;
        e.path = it->first;
        e.name = baseName(it->first);
        const LLSD& v = it->second;
        e.values = v["values"].isMap() ? v["values"] : LLSD::emptyMap();
        for (const LLSD& d : llsd::inArray(v["defines"])) e.defines.emplace_back(d[0].asString(), d[1].asString());
        e.scope = v.has("scope") ? v["scope"].asInteger() : (S32)SCOPE_ALL;
        e.feather = v.has("feather") ? v["feather"].asInteger() : 1;
        e.point = v.has("point") ? v["point"].asInteger() : (S32)POINT_DISPLAY;
        e.quarantined = v["quarantined"].asBoolean();
        e.quarantineReason = v["quarantine_reason"].asString();
        mHiddenEffects[e.path] = e;   // matched to the files by scanDirs
    }
}

void SLFxStack::saveState()
{
    mDirty = false;
    LLSD sd = LLSD::emptyMap();
    LLSD techs = LLSD::emptyArray();
    for (const std::string& id : mOrder)
    {
        LLSD t;
        t["id"] = id;
        t["enabled"] = mEnabledTechs.count(id) > 0;
        techs.append(t);
    }
    sd["techniques"] = techs;
    LLSD defs = LLSD::emptyArray();
    for (const auto& d : mGlobalDefines) { LLSD p = LLSD::emptyArray(); p.append(d.first); p.append(d.second); defs.append(p); }
    sd["defines"] = defs;
    LLSD effects = LLSD::emptyMap();
    auto put = [&](const Effect& e) {
        const bool interesting = (e.values.isMap() && e.values.size()) || !e.defines.empty() || e.scope != SCOPE_ALL
                              || e.point != POINT_DISPLAY || e.quarantined || e.feather != 1;
        if (!interesting) return;
        LLSD v;
        v["values"] = e.values.isMap() ? e.values : LLSD::emptyMap();
        LLSD ed = LLSD::emptyArray();
        for (const auto& d : e.defines) { LLSD p = LLSD::emptyArray(); p.append(d.first); p.append(d.second); ed.append(p); }
        v["defines"] = ed;
        v["scope"] = e.scope;
        v["feather"] = e.feather;
        v["point"] = e.point;
        v["quarantined"] = e.quarantined;
        v["quarantine_reason"] = e.quarantineReason;
        effects[e.path] = v;
    };
    for (const auto& kv : mHiddenEffects) put(kv.second);
    for (const auto& kv : mEffects) put(kv.second);
    sd["effects"] = effects;
    const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, kStateFile);
    const std::string tmp = path + ".tmp";
    {
        llofstream f(tmp.c_str(), std::ios::out | std::ios::trunc);
        if (!f.is_open()) return;
        LLSDSerialize::toPrettyXML(sd, f);
    }
    LLFile::rename(tmp, path);
}

// ============================================================================
// Safety (#546)
//
// Two markers in the user settings folder tell the next launch what happened:
//  - fx_device_lost.xml: written by the device-lost handler (deviceLostSummary) when
//    effects were running: the active effects, the provisional ones, the techniques of
//    the last recorded frame. Next launch: FX OFF, provisional effects quarantined (all
//    active ones when none was provisional), a notice naming them.
//  - fx_provisional.xml: effects that are new / edited and not yet proven (fewer than
//    SLFxProvisionalFrames good frames). Written while the set is non-empty, deleted when
//    it empties and at a clean exit. Found at launch = the session died (crash, hang the
//    driver did not recover, killed) while they were on: FX off + quarantine + notice.
// A quarantined effect is never compiled or run until the user enables one of its
// techniques again.
// ============================================================================
namespace
{
std::mutex sSafeMutex;
std::vector<std::string> sSafeActive, sSafeProvisional;   // effect paths (handler copies)
std::string sLostMarkerPath;
const char* kLostMarker = "fx_device_lost.xml";
const char* kProvisionalMarker = "fx_provisional.xml";

std::string markerPath(const char* name)
{
    return gDirUtilp ? gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, name) : std::string();
}

bool readMarker(const std::string& path, LLSD& out)
{
    llifstream f(path.c_str());
    if (!f.is_open()) return false;
    return LLSDSerialize::fromXML(out, f) > 0;
}

std::string namesOf(const LLSD& arr)
{
    std::string s;
    for (const LLSD& e : llsd::inArray(arr)) s += (s.empty() ? "" : ", ") + baseName(e.asString());
    return s;
}

// Injected post-processing on top of ours (ReShade proxy / Vulkan layer, vkBasalt): effects
// would apply twice. One warning per session. Only what is actually LOADED into this process
// counts (#956): an installed ReShade layer is registered system-wide even when it is only
// enabled for other programs, so listing the installed layers or files on disk warns falsely.
// Runs after the Vulkan instance exists, so any layer the loader injected is already mapped.
std::string detectInjectedFx()
{
    std::vector<std::string> hits;
    auto addHit = [&hits](const std::string& h) {
        if (std::find(hits.begin(), hits.end(), h) == hits.end()) hits.push_back(h);
    };
#if defined(_WIN32)
    // Loaded modules: anything named ReShade (the layer and the proxy ship as ReShade64.dll),
    // and graphics-API proxy DLLs loaded from the viewer's own folder (the system copies live
    // in System32; we load dxgi.dll ourselves for adapter enumeration).
    const std::string exeDir = lower(gDirUtilp->getExecutableDir());
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snap != INVALID_HANDLE_VALUE)
    {
        MODULEENTRY32W me;
        me.dwSize = sizeof(me);
        for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me))
        {
            const std::string name = ll_convert_wide_to_string(std::wstring(me.szModule));
            const std::string path = ll_convert_wide_to_string(std::wstring(me.szExePath));
            const std::string lname = lower(name);
            if (lname.find("reshade") != std::string::npos)
            {
                addHit("ReShade (" + name + ")");
                continue;
            }
            for (const char* proxy : {"dxgi.dll", "d3d9.dll", "d3d11.dll", "d3d12.dll", "opengl32.dll"})
            {
                if (lname != proxy) continue;
                const std::string lpath = lower(path);
                const size_t slash = lpath.find_last_of("\\/");
                if (slash != std::string::npos && lpath.substr(0, slash) == exeDir)
                    addHit("a " + name + " proxy in the viewer's folder (usually ReShade)");
            }
        }
        CloseHandle(snap);
    }
#else
    // Shared objects mapped into this process (Linux / macOS-style /proc).
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line))
    {
        const size_t slash = line.find_last_of('/');
        if (slash == std::string::npos) continue;
        const std::string name = line.substr(slash + 1);
        const std::string lname = lower(name);
        if (lname.find("vkbasalt") != std::string::npos) addHit("vkBasalt (" + name + ")");
        else if (lname.find("reshade") != std::string::npos) addHit("ReShade (" + name + ")");
    }
#endif
    std::string s;
    for (const std::string& h : hits) s += (s.empty() ? "" : "; ") + h;
    return s;
}
}

void SLFxStack::checkStartupMarkers()
{
    sLostMarkerPath = markerPath(kLostMarker);
    const std::string provPath = markerPath(kProvisionalMarker);
    LLSD lost, prov;
    const bool wasLost = readMarker(sLostMarkerPath, lost);
    const bool wasProv = readMarker(provPath, prov);
    if (!wasLost && !wasProv) return;
    std::vector<std::string> culprits;
    std::string reason;
    if (wasLost)
    {
        for (const LLSD& e : llsd::inArray(lost["provisional"])) culprits.push_back(e.asString());
        if (culprits.empty())
            for (const LLSD& e : llsd::inArray(lost["active"])) culprits.push_back(e.asString());
        reason = "the graphics device was lost while it ran";
        mStartupNotice = "The graphics device was lost last session while effects were on ("
                       + namesOf(lost["active"]) + "). Effects start OFF this time.";
    }
    else
    {
        for (const LLSD& e : llsd::inArray(prov)) culprits.push_back(e.asString());
        reason = "the viewer stopped while this new effect was running";
        mStartupNotice = "Last session ended unexpectedly while new effects were running. Effects start OFF this time.";
    }
    if (!culprits.empty())
    {
        std::string names;
        for (const std::string& c : culprits)
        {
            Effect& e = mHiddenEffects[c];   // scanDirs matches it to the file
            e.path = c;
            e.name = baseName(c);
            e.quarantined = true;
            e.quarantineReason = reason;
            names += (names.empty() ? "" : ", ") + baseName(c);
        }
        mStartupNotice += " Quarantined until you enable them again: " + names + ".";
        markDirty();
    }
    gSavedSettings.setBOOL("SLFxEnabled", false);
    LL_WARNS("FX") << mStartupNotice << LL_ENDL;
    LLFile::remove(sLostMarkerPath);
    LLFile::remove(provPath);
}

void SLFxStack::updateSafety()
{
    static LLFrameTimer s_tick;
    if (s_tick.getElapsedTimeF32() < 0.5f) return;
    s_tick.reset();
    LLFxRuntimeVK& rt = LLFxRuntimeVK::instance();

    // The notice from the startup check, once the viewer can show it.
    if (!mStartupNotice.empty() && LLStartUp::getStartupState() >= STATE_STARTED)
    {
        LLNotificationsUtil::add("GenericAlert", LLSD().with("MESSAGE", mStartupNotice));
        mStartupNotice.clear();
    }
    // Injected ReShade / vkBasalt: one warning per session, when FX is first on.
    static bool s_injectChecked = false;
    if (!s_injectChecked && isEnabled() && LLStartUp::getStartupState() >= STATE_STARTED)
    {
        s_injectChecked = true;
        const std::string inj = detectInjectedFx();
        if (inj.empty())
        {
            LL_INFOS("FX") << "no other post-processing loaded into the viewer" << LL_ENDL;
        }
        else
        {
            LL_WARNS("FX") << "other post-processing loaded into the viewer: " << inj << LL_ENDL;
            LLNotificationsUtil::add("GenericAlert", LLSD().with("MESSAGE",
                "Slipstream's built-in effects are on and keep working. Another post-processing tool is also "
                "loaded into the viewer: " + inj + ". Its effects are drawn on top of Slipstream's, so if the "
                "picture looks doubled or too strong, turn one of them off (for ReShade: remove Slipstream from "
                "ReShade's setup, or delete its files from the viewer's folder)."));
        }
    }

    // Provisional: enabled effects that have not yet run SLFxProvisionalFrames frames.
    static LLCachedControl<U32> s_frames(gSavedSettings, "SLFxProvisionalFrames", 300);
    std::set<std::string> prov;
    std::vector<std::string> active;
    if (isEnabled())
    {
        active = rt.activeEffects();
        for (const std::string& id : mOrder)
        {
            if (!mEnabledTechs.count(id)) continue;
            const Effect* e = findEffect(id.substr(id.find('@') + 1));
            if (!e || e->quarantined) continue;
            const LLFxRuntimeVK::EffectStatus st = rt.status(e->path);
            if (st.failed || st.parked) continue;
            if (st.goodFrames < (U32)s_frames) prov.insert(e->path);
        }
    }
    {
        std::lock_guard<std::mutex> lk(sSafeMutex);
        sSafeActive = active;
        sSafeProvisional.assign(prov.begin(), prov.end());
    }
    if (prov != mProvisional)
    {
        mProvisional = prov;
        const std::string path = markerPath(kProvisionalMarker);
        if (prov.empty())
            LLFile::remove(path);
        else
        {
            LLSD a = LLSD::emptyArray();
            for (const std::string& p : prov) a.append(p);
            llofstream f(path.c_str(), std::ios::out | std::ios::trunc);
            if (f.is_open()) LLSDSerialize::toPrettyXML(a, f);
        }
    }
}

std::string SLFxStack::deviceLostSummary()
{
    const std::string last = LLFxRuntimeVK::lastRecordedSummary();
    std::vector<std::string> active, prov;
    {
        std::lock_guard<std::mutex> lk(sSafeMutex);
        active = sSafeActive;
        prov = sSafeProvisional;
    }
    if (active.empty() && prov.empty())
        return "Effects (FX): none running";
    // Tell the next launch: FX off, quarantine (any thread; plain file write).
    if (!sLostMarkerPath.empty())
    {
        LLSD sd;
        LLSD a = LLSD::emptyArray(), p = LLSD::emptyArray();
        for (const std::string& s : active) a.append(s);
        for (const std::string& s : prov) p.append(s);
        sd["active"] = a;
        sd["provisional"] = p;
        sd["last_frame"] = last;
        llofstream f(sLostMarkerPath.c_str(), std::ios::out | std::ios::trunc);
        if (f.is_open()) LLSDSerialize::toPrettyXML(sd, f);
    }
    std::string s = "Effects (FX) running: ";
    for (size_t i = 0; i < active.size(); ++i) s += (i ? ", " : "") + baseName(active[i]);
    if (!prov.empty())
    {
        s += "; provisional (new, will be quarantined): ";
        for (size_t i = 0; i < prov.size(); ++i) s += (i ? ", " : "") + baseName(prov[i]);
    }
    if (!last.empty()) s += "; last frame's techniques: " + last;
    return s;
}
