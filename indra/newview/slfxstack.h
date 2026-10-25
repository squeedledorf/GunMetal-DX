/**
 * @file slfxstack.h
 * @brief The built-in ReShade FX compatible effects stack (#542): the user-facing state.
 *
 * SLFxStack owns WHAT runs: the effect files found in the effects folders, the technique
 * list (ReShade ids "Technique@File.fx") with enable flags and run order, uniform values,
 * preprocessor definitions, per-effect scope / injection point, quarantine, and their
 * persistence (<user settings>/fx_state.xml). The GPU side is LLFxRuntimeVK
 * (vkgfx/llfxruntimevk.h), which this class feeds every frame (main-thread idle).
 *
 * Effects folder (getEffectsDir): <user app dir>/effects with ReShade's layout --
 * Shaders/ (.fx / .fxh, subfolders searched) and Textures/. Extra folders (e.g. an
 * existing reshade-shaders install) via setSearchPaths. The bundled starter pack
 * (app_settings/fx/pack, SweetFX + prod80, MIT, #957) is scanned after all of those, so
 * a user file of the same name wins. The viewer's own ReShade.fxh / ReShadeUI.fxh
 * (app_settings/fx/Shaders) are searched last.
 *
 * Master switch: setting SLFxEnabled (default off). --no-fx (setting SLFxNoFx, not
 * persisted) keeps it off for the session. All callbacks fire on the main thread.
 *
 * Effect identity: SLFxEffectInfo::mPath is the full path; every API taking
 * `effect_path` also accepts the bare file name ("Vibrance.fx"). Technique identity:
 * SLFxTechniqueInfo::mId = "Name@File.fx" (ReShade preset form); APIs taking a technique
 * accept the id, or a bare name when it is unambiguous.
 */
#ifndef SL_SLFXSTACK_H
#define SL_SLFXSTACK_H

#include "llsingleton.h"
#include "llsd.h"
#include "llframetimer.h"

#include <boost/signals2.hpp>
#include <vulkan/vulkan.h>

#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

struct SLFxEffectInfo
{
    std::string mPath;       // full path of the .fx file
    std::string mName;       // file name ("Vibrance.fx")
    bool        mLoaded = false;   // compiled successfully
    std::string mError;      // errors (compile / runtime / quarantine reason); "" = none
    std::string mWarnings;   // compiler warnings
    bool        mCompiling = false;
    bool        mQuarantined = false;   // disabled after a GPU crash / bad session (#546)
    U32         mVramMB = 0;
};

struct SLFxUniformInfo
{
    std::string mName;
    std::string mType;       // "bool","int","int2".."int4","float","float2","float3","float4","color3","color4"
    std::string mUIType;     // ui_type: "drag","slider","combo","radio","list","color","input",""
    std::string mLabel, mTooltip, mCategory;
    LLSD mMin, mMax, mStep;  // scalar Real, or an array of Real for vectors
    std::vector<std::string> mItems;   // ui_items split on '\0'
    LLSD mValue, mDefault;   // Boolean / Integer / Real, or an array of Real for vectors
    bool mHidden = false;
    // Extra annotations (#545)
    std::string mText;       // ui_text
    S32 mSpacing = 0;        // ui_spacing
    std::string mUnits;      // ui_units
    bool mNoEdit = false;    // noedit
    bool mNoReset = false;   // noreset
    bool mCategoryClosed = false;   // ui_category_closed
};

struct SLFxTechniqueInfo
{
    std::string mName;       // technique name
    std::string mEffectPath; // full path of its .fx
    bool mEnabled = false;
    std::string mLabel, mTooltip;
    F32 mGpuMs = 0.f;
    std::string mId;         // "Name@File.fx" (ReShade preset id)
    bool mEnabledInScreenshot = true;
    bool mHdrPoint = false;  // runs at the HDR injection point (#548)
};

class SLFxStack : public LLSingleton<SLFxStack>
{
    LLSINGLETON(SLFxStack);
    ~SLFxStack() override;
    void initSingleton() override;
    void cleanupSingleton() override;

public:
    // Per-effect scope (#548): which pixels the effect may change.
    enum EScope { SCOPE_ALL = 0, SCOPE_AVATARS = 1, SCOPE_NOT_AVATARS = 2 };
    // Injection point (#548).
    enum EPoint { POINT_DISPLAY = 0, POINT_HDR = 1 };

    // ---- effects ----
    std::vector<SLFxEffectInfo> getAvailableEffects();
    void rescan();
    std::vector<SLFxTechniqueInfo> getTechniques();   // in run order
    bool setTechniqueEnabled(const std::string& technique, bool enabled);
    bool setTechniqueOrder(const std::vector<std::string>& ordered);
    std::vector<SLFxUniformInfo> getUniforms(const std::string& effect_path);
    bool setUniform(const std::string& effect_path, const std::string& name, const LLSD& value);
    bool resetUniform(const std::string& effect_path, const std::string& name);

    // ---- preprocessor definitions (ReShade PreprocessorDefinitions=; recompiles) ----
    // effect_path "" = the global list (applies to every effect, before the effect's own).
    void setPreprocessorDefinitions(const std::string& effect_path,
                                    const std::vector<std::pair<std::string, std::string>>& defs);
    std::vector<std::pair<std::string, std::string>> getPreprocessorDefinitions(const std::string& effect_path) const;
    // The definitions an effect checks with #ifdef / #ifndef (tunables for the UI).
    std::vector<std::pair<std::string, std::string>> getUsedDefinitions(const std::string& effect_path);

    // ---- master switch / folders ----
    void setEnabled(bool enabled);
    bool isEnabled() const;
    void reloadAll();   // hot reload (rescan + recompile)
    std::string getEffectsDir() const;
    std::vector<std::string> getSearchPaths() const;            // extra folders (each with Shaders/ / Textures/ or .fx files)
    void setSearchPaths(const std::vector<std::string>& paths);

    // ---- per-effect scope / injection point (#548) ----
    bool setEffectScope(const std::string& effect_path, S32 scope, S32 feather_px = 1);
    S32  getEffectScope(const std::string& effect_path) const;
    bool setEffectPoint(const std::string& effect_path, S32 point);
    S32  getEffectPoint(const std::string& effect_path) const;

    boost::signals2::connection setChangedCallback(const std::function<void()>& cb);

    // P5: the avatar-id mask (R16_UINT, display extent, unjittered, 0 = no avatar). Until
    // set, the avatar-mask semantic binds a zero mask and "avatars only" changes nothing.
    static void setAvatarMaskProvider(std::function<VkImageView()> provider);

    // ---- safety (#546) ----
    // Effects disabled after a GPU crash / killed session: listed with mQuarantined; a user
    // enabling one of their techniques again lifts the quarantine.
    std::vector<std::string> getQuarantined() const;
    // Called by the device-lost handler (any thread, no singleton access): records which
    // effects were active / provisional so the next launch starts with FX off and names
    // them. Returns a line for the GPU-lost report.
    static std::string deviceLostSummary();

    // ---- snapshots (#547) ----
    void warmSnapshotSize(S32 width, S32 height);
    std::string getLastSnapshotNote() const;

    // Resolve a path or bare file name to a known effect's full path ("" = unknown).
    std::string resolveEffect(const std::string& effect_path) const;

private:
    struct Effect
    {
        std::string path, name;
        LLSD values;   // name -> value
        std::vector<std::pair<std::string, std::string>> defines;
        S32 scope = SCOPE_ALL, feather = 1, point = POINT_DISPLAY;
        bool quarantined = false;
        std::string quarantineReason;
    };

    static void onIdle(void* self);
    void idle();
    void scanDirs();
    void loadState();
    void saveState();
    void markDirty() { mDirty = true; mSaveTimer.reset(); }
    void pushPlan();
    void requestAllMetadata();
    void fireChanged();
    Effect* findEffect(const std::string& effect_path);
    const Effect* findEffect(const std::string& effect_path) const;
    std::string techniqueId(const std::string& effect_path, const std::string& tech) const;
    std::vector<std::string> resolveTechnique(const std::string& technique) const;
    void syncTechniqueList();
    void checkStartupMarkers();
    void updateSafety();

    std::map<std::string, Effect> mEffects;          // by full path (files found now)
    std::map<std::string, Effect> mHiddenEffects;    // saved state of effects not found this scan
    std::vector<std::string> mOrder;                 // technique ids, run order
    std::set<std::string> mEnabledTechs;             // technique ids
    std::vector<std::pair<std::string, std::string>> mGlobalDefines;
    std::vector<std::string> mIncludeDirs, mTextureDirs;
    boost::signals2::signal<void()> mChanged;
    boost::signals2::connection mEnabledConn;
    U64 mSeenMetaGen = 0;
    bool mDirty = false;
    LLFrameTimer mSaveTimer;
    bool mScanned = false;
    bool mMetaRequested = false;
    std::string mStartupNotice;
    std::set<std::string> mProvisional;              // effect paths not yet proven stable
};

// No effects for the renders inside this scope (#547: 360 capture -- six faces through a
// screen-space effect stack would not line up at the seams). With effects on, the first
// use per session tells the user why the capture has none.
struct SLFxSuppressScope
{
    explicit SLFxSuppressScope(const char* what);
    ~SLFxSuppressScope();
    SLFxSuppressScope(const SLFxSuppressScope&) = delete;
    SLFxSuppressScope& operator=(const SLFxSuppressScope&) = delete;
};

#endif
