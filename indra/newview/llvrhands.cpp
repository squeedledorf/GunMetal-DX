/**
 * @file llvrhands.cpp
 * @brief VR milestone M3 (#1076): visible hands and the laser beam. See llvrhands.h.
 */
#include "llviewerprecompiledheaders.h"

#include "llvrhands.h"

#include "llcleaninstancestore.h"
#include "llcleanmeshasset.h"
#include "llviewercamera.h"
#include "llvolume.h"
#include "llvrcamera.h"
#include "vkgfx/llxrsession.h"

#include <chrono>
#include <cstring>

namespace
{
using HandClock = std::chrono::steady_clock;
using HandClockPoint = HandClock::time_point;

// One procedural mesh in the clean mesh store: every face's LOD group (all four levels point
// at the one captured slot, so the cull's distance LOD always resolves) and its bounds.
//
// #1076 (second headset run): the captured slots are ordinary RECLAIMABLE mesh geometry. The
// geometry GC's liveness pass frees a mesh no instance row has referenced for
// RenderVKAssetReleaseGraceSec (30 s) -- it unpublishes the slot from its LOD group, and
// gcLodGroups later frees the group id and hands it to another mesh. The hands were captured
// the instant the store was ready (19:37:12) while the session was VISIBLE, not FOCUSED (no
// input: no hand rows), the 19:37:47 liveness pass freed them, and from 19:37:54 the hand
// rows drew through the cached group ids: nothing, or whatever mesh got the recycled group
// (thin grass cards in place of the controllers, no beam). So the cached groups are checked
// every frame (stillCaptured) and captured again when the store let them go.
struct ProcMesh
{
    LLUUID id;
    bool sphere = true;
    F32 detail = 1.f;
    bool ok = false;
    S32 faces = 0;
    static constexpr S32 kMaxFaces = 8;
    U32 group[kMaxFaces] = {};
    S32 faceIdx[kMaxFaces] = {};   // the volume face each captured entry came from
    U32 slot[kMaxFaces] = {};      // ... its captured slot (published at every level of group)
    HandClockPoint capturedAt{};
    bool reclaimLogged = false;
    F32 lo[kMaxFaces][3] = {};
    F32 hi[kMaxFaces][3] = {};
};

ProcMesh sSphere{LLUUID("6f3b1076-0000-4a00-8000-00000000a001"), true, 2.5f};
ProcMesh sCylinder{LLUUID("6f3b1076-0000-4a00-8000-00000000a002"), false, 1.5f};

// Capture attempts: the clean mesh store is initialised by the first VK world frame
// (llappviewer.cpp), which can come a minute after the head-tracked camera starts (the
// login / loading frames): capture() is a silent no-op before that, so wait for it instead of
// retrying (and logging) every frame. A capture that fails with the store ready is logged
// once and retried once a second.
HandClockPoint sNextCaptureTry{};
bool sCaptureFailLogged = false;

const char* meshName(const ProcMesh& m)
{
    return m.sphere ? "sphere" : "cylinder";
}

// Every captured face still published, by the same group, at the captured level: the GC has
// not freed the slot (unpublished: SENTINEL) nor the group (the (mesh, face) maps to no group
// or to a new one). Two map lookups per face; the meshes have 1 and 3 faces.
bool stillCaptured(const ProcMesh& m, LLCleanMeshAsset& ma, S32 lod)
{
    for (S32 i = 0; i < m.faces; ++i)
    {
        if (m.group[i] == LLCleanMeshAsset::LOD_SENTINEL) return false;
        if (ma.getLodGroupId(m.id, m.faceIdx[i]) != m.group[i]) return false;
        if (ma.groupSlotAt(m.id, m.faceIdx[i], lod) != m.slot[i]) return false;
    }
    return true;
}

bool ensureMesh(ProcMesh& m)
{
    LLCleanMeshAsset& ma = LLCleanMeshAsset::instance();
    const S32 lod = 3;
    if (m.ok)
    {
        if (stillCaptured(m, ma, lod)) return true;
        const F32 age = std::chrono::duration<F32>(HandClock::now() - m.capturedAt).count();
        if (!m.reclaimLogged)
        {
            m.reclaimLogged = true;   // (once per mesh: the reason is the same every time)
            LL_WARNS("VR") << "hand mesh " << meshName(m) << ": the mesh store let it go " << age
                           << " s after the capture (geometry GC: no hand row held it); capturing again (logged once)"
                           << LL_ENDL;
        }
        m.ok = false;
    }
    if (!ma.ready()) return false;   // (no store yet: the first world frame creates it)
    const HandClockPoint now = HandClock::now();
    if (now < sNextCaptureTry) return false;
    LLVolumeParams vp;
    if (m.sphere) vp.setType(LL_PCODE_PROFILE_CIRCLE_HALF, LL_PCODE_PATH_CIRCLE);
    else          vp.setType(LL_PCODE_PROFILE_CIRCLE, LL_PCODE_PATH_LINE);
    vp.setBeginAndEndS(0.f, 1.f);
    vp.setBeginAndEndT(0.f, 1.f);
    vp.setRatio(1.f, 1.f);
    vp.setShear(0.f, 0.f);
    LLPointer<LLVolume> vol = new LLVolume(vp, m.detail, /*generate_single_face=*/false, /*is_unique=*/true);
    const S32 n = llmin(vol->getNumVolumeFaces(), ProcMesh::kMaxFaces);
    if (n <= 0)
    {
        sNextCaptureTry = now + std::chrono::seconds(1);
        LL_WARNS_ONCE("VR") << "hand mesh: the procedural volume has no faces (retrying once a second)" << LL_ENDL;
        return false;
    }
    for (S32 f = 0; f < n; ++f) vol->genTangents(f);
    ma.capture(m.id, lod, vol.get());
    // The geometry GC may have reclaimed an earlier capture (no instance held it; this
    // session's, or a previous session's): capture again in place.
    if (!ma.getSlot(m.id, lod, 0).valid) ma.capture(m.id, lod, vol.get(), /*force=*/true);
    m.faces = 0;
    for (S32 f = 0; f < n; ++f)
    {
        const LLCleanMeshAsset::Slot s = ma.getSlot(m.id, lod, f);
        if (!s.valid) continue;
        U32 g = LLCleanMeshAsset::LOD_SENTINEL;
        for (S32 level = 0; level < LLCleanMeshAsset::NUM_LOD; ++level) g = ma.registerLodSlot(m.id, f, level, s.slot_id);
        m.group[m.faces] = g;
        m.faceIdx[m.faces] = f;
        m.slot[m.faces] = s.slot_id;
        for (int k = 0; k < 3; ++k)
        {
            m.lo[m.faces][k] = s.cap_lo[k];
            m.hi[m.faces][k] = s.cap_hi[k];
        }
        ++m.faces;
    }
    m.ok = m.faces > 0 && stillCaptured(m, ma, lod);
    if (m.ok)
    {
        m.capturedAt = now;
        LL_INFOS("VR") << "hand mesh " << meshName(m) << ": " << m.faces << " faces captured (group " << m.group[0]
                       << ", slot " << m.slot[0] << ")" << LL_ENDL;
    }
    else
    {
        sNextCaptureTry = now + std::chrono::seconds(1);
        if (!sCaptureFailLogged)
        {
            sCaptureFailLogged = true;
            LL_WARNS("VR") << "hand mesh " << meshName(m) << ": the mesh store took no face of the procedural volume; retrying once a second (logged once)"
                           << LL_ENDL;
        }
    }
    return m.ok;
}

// A part: one instance row per mesh face, keyed by the part's own (synthetic) object id. These
// ids belong to no LLViewerObject: every store sweep that looks objects up skips them, the
// GPU pick resolves them to "no object", and release() drops them.
struct Part
{
    LLUUID id;
    bool live = false;
    S32 faces = 0;
};

enum : U32 { PART_BODY = 0, PART_TOP, PART_BEAM, PART_COUNT };
Part sParts[LLXR_HAND_COUNT][PART_COUNT] = {
    {{LLUUID("6f3b1076-0000-4a00-8000-00000000b010")}, {LLUUID("6f3b1076-0000-4a00-8000-00000000b011")},
     {LLUUID("6f3b1076-0000-4a00-8000-00000000b012")}},
    {{LLUUID("6f3b1076-0000-4a00-8000-00000000b020")}, {LLUUID("6f3b1076-0000-4a00-8000-00000000b021")},
     {LLUUID("6f3b1076-0000-4a00-8000-00000000b022")}},
};
F32 sBeam[LLXR_HAND_COUNT] = {0.f, 0.f};
bool sAnyLive = false;

// Unlit: the instance bits a fullbright prim face carries (LLVOVolume's clean registration):
// bit4 (the opaque fullbright bucket) AND bit12 (what clean_draw.frag tests for unlit shading).
// bit4 alone -- what the beam had -- drew it LIT: a 5 mm grey-blue tube nobody saw (#1076).
constexpr U32 kFlagFullbright = 16u | 4096u;
// The beam's diameter: ~3 px at the wall (2.5 m) at the eye render size and 85 % scale.
constexpr F32 kBeamDiameter = 0.008f;
bool sBeamLogged = false;
bool sHandLogged[LLXR_HAND_COUNT] = {false, false};

void releasePart(Part& p)
{
    if (!p.live) return;
    LLCleanInstanceStore::instance().releaseObject(p.id);
    p.live = false;
    p.faces = 0;
}

// Upsert a part: the mesh's unit shape placed by `model` (column-major, agent space).
void upsertPart(Part& p, const ProcMesh& m, const F32 model[16], const F32 tint[4], U32 flags)
{
    LLCleanInstanceStore& st = LLCleanInstanceStore::instance();
    for (S32 f = 0; f < m.faces; ++f)
    {
        LLCleanInstanceStore::Instance inst{};   // (every lane zero)
        std::memcpy(inst.model_matrix, model, sizeof(inst.model_matrix));
        for (int k = 0; k < 3; ++k)
        {
            inst.aabb_min[k] = m.lo[f][k];
            inst.aabb_max[k] = m.hi[f][k];
        }
        inst.aabb_min[3] = 1.f;   // LOD radius: always the captured (only) level
        std::memcpy(inst.tint, tint, sizeof(inst.tint));
        inst.uv_matrix[0] = inst.uv_matrix[5] = inst.uv_matrix[10] = inst.uv_matrix[15] = 1.f;
        inst.lod_group_id = m.group[f];
        inst.material_id = 0;     // the default white material: the tint is the colour
        inst.flags = flags | LLCleanInstanceStore::DYNAMIC_CASTER_FLAG;
        inst.skin_id = 0xFFFFFFFFu;
        st.upsert(p.id, f, inst, /*is_active=*/true);
    }
    p.live = true;
    p.faces = m.faces;
    sAnyLive = true;
}

// model = [ax * sx | ay * sy | az * sz | pos] (agent space).
void makeModel(F32 out[16], const LLVector3& pos, const LLVector3& ax, const LLVector3& ay, const LLVector3& az,
               F32 sx, F32 sy, F32 sz)
{
    for (int k = 0; k < 3; ++k)
    {
        out[k] = ax.mV[k] * sx;
        out[4 + k] = ay.mV[k] * sy;
        out[8 + k] = az.mV[k] * sz;
        out[12 + k] = pos.mV[k];
    }
    out[3] = out[7] = out[11] = 0.f;
    out[15] = 1.f;
}
} // namespace

void LLVRHands::setBeam(U32 hand, F32 length)
{
    if (hand < LLXR_HAND_COUNT) sBeam[hand] = length;
}

void LLVRHands::update()
{
    LLXrInputState in;
    LLXrSession::instance().inputState(in);
    if (!ensureMesh(sSphere) || !ensureMesh(sCylinder))
    {
        release();
        return;
    }
    // Stylized controller, in the grip pose's own (OpenXR) axes: an elongated body along the
    // handle (Z), and a flatter accent disc at its top (-Z) end. Sizes are diameters (the unit
    // sphere is 1 m across).
    static const F32 kBodyTint[4] = {0.72f, 0.74f, 0.78f, 1.f};
    static const F32 kTopTint[4] = {0.25f, 0.62f, 0.95f, 1.f};
    static const F32 kBeamTint[4] = {0.35f, 0.85f, 1.0f, 1.f};
    for (U32 h = 0; h < LLXR_HAND_COUNT; ++h)
    {
        const LLXrHandState& hs = in.hand[h];
        LLVector3 gp;
        if (!hs.active || !hs.grip.valid || !LLVRCamera::appToAgent(hs.grip.position, gp))
        {
            for (Part& p : sParts[h]) releasePart(p);
            sBeam[h] = 0.f;
            continue;
        }
        const F32* q = hs.grip.orientation;
        const LLVector3 gx = LLVRCamera::appAxisToAgent(q, 1.f, 0.f, 0.f);
        const LLVector3 gy = LLVRCamera::appAxisToAgent(q, 0.f, 1.f, 0.f);
        const LLVector3 gz = LLVRCamera::appAxisToAgent(q, 0.f, 0.f, 1.f);
        F32 model[16];
        makeModel(model, gp + gz * 0.01f, gx, gy, gz, 0.042f, 0.05f, 0.15f);
        upsertPart(sParts[h][PART_BODY], sSphere, model, kBodyTint, 0u);
        makeModel(model, gp + gy * 0.02f - gz * 0.07f, gx, gy, gz, 0.062f, 0.028f, 0.062f);
        upsertPart(sParts[h][PART_TOP], sSphere, model, kTopTint, 0u);
        if (!sHandLogged[h])
        {
            // Once per hand: where it is drawn against the camera, and through which group /
            // slot (a hand drawn far from the eye, or through a group that no longer names
            // the captured slot, shows here).
            sHandLogged[h] = true;
            const LLVector3 eye = LLViewerCamera::getInstance()->getOrigin();
            LL_INFOS("VR") << "hand: " << (h == LLXR_HAND_LEFT ? "left" : "right") << " first drawn at agent ("
                           << gp.mV[VX] << ", " << gp.mV[VY] << ", " << gp.mV[VZ] << "), " << (gp - eye).magVec()
                           << " m from the camera, sphere group " << sSphere.group[0] << " slot " << sSphere.slot[0]
                           << " (published now: " << LLCleanMeshAsset::instance().groupSlotAt(sSphere.id, sSphere.faceIdx[0], 3)
                           << ")" << LL_ENDL;
        }

        // The beam: the unit cylinder (Z from -0.5 to 0.5) along the aim ray's -Z, unlit.
        LLVector3 ap;
        const F32 len = sBeam[h];
        sBeam[h] = 0.f;   // one frame: LLVRInput sets it again while the ray hits the wall
        if (len > 0.01f && hs.aim.valid && LLVRCamera::appToAgent(hs.aim.position, ap))
        {
            const F32* a = hs.aim.orientation;
            const LLVector3 ax = LLVRCamera::appAxisToAgent(a, 1.f, 0.f, 0.f);
            const LLVector3 ay = LLVRCamera::appAxisToAgent(a, 0.f, -1.f, 0.f);   // (x, -y, -z): right-handed
            const LLVector3 az = LLVRCamera::appAxisToAgent(a, 0.f, 0.f, -1.f);
            makeModel(model, ap + az * (0.5f * len), ax, ay, az, kBeamDiameter, kBeamDiameter, len);
            upsertPart(sParts[h][PART_BEAM], sCylinder, model, kBeamTint, kFlagFullbright);
            if (!sBeamLogged)
            {
                sBeamLogged = true;
                LL_INFOS("VR") << "beam: first drawn (" << (h == LLXR_HAND_LEFT ? "left" : "right") << " hand, " << len
                               << " m, " << sParts[h][PART_BEAM].faces << " rows)" << LL_ENDL;
            }
        }
        else
        {
            releasePart(sParts[h][PART_BEAM]);
        }
    }
}

void LLVRHands::release()
{
    if (!sAnyLive) return;
    for (auto& hand : sParts)
        for (Part& p : hand) releasePart(p);
    sBeam[0] = sBeam[1] = 0.f;
    sAnyLive = false;
    // With no row holding them the geometry GC may reclaim the meshes: re-check (and capture
    // again if needed) when the hands come back (a session that resumes after the headset
    // was taken off).
    sSphere.ok = sCylinder.ok = false;
}
