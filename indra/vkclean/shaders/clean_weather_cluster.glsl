// vkclean -- #901 / #909 follow-up: the CLUSTER LOD avatars in the weather's avatar chain
// (clean_weather_shelter.comp -DWX_SH_CL_FILTER / -DWX_SH_CL_RASTER, clean_weather_accum.comp
// -DWX_AC_ACCUM_CL; llcleanweathervk_shelter.cpp ensureShelterCluster).
//
// With RenderVKClusterLOD the cluster path OWNS every cluster-eligible rigged mesh: those rows have
// no legacy geometry, draw, pre-skin or candidate (clean_cull.comp, the cluster-owns-rigged mode),
// so the shelter chain's work list -- the main cull's candidates -- never held them: an avatar's
// body / clothes / wings were absent from its shelter map (top and #783 bottom map) and from its
// rest-pose grid. These kernels read the SHARED SKINNED ARENA instead (clean_cluster_skin_common.glsl,
// written by this frame's cluster pre-skin, recorded before the chain): one workgroup per selected
// entry of the skinned view (the pre-skin's own indirect grid, CLK_DISPATCH), the entry's skinned
// cluster s -- its vertices in its character's space (M local + t + the palettes' eye = world) --,
// its pool cluster (the source vertices: bind_shape x position = the bind-space rest position, the
// key the rigged VS and clean_cluster_draw_common.glsl give the fragments) and its triangles (the
// cluster's uint8 local indices). Nothing is copied: the arena and the pool are already resident.
//
// The cluster set (llcleanclustervk.h, its layout / this frame's set) at set 2, after draw set 0 and
// the shelter set: the pipelines' own layout {draw set 0, shelter set, cluster set}; the 128 B push.
#ifndef CLEAN_WEATHER_CLUSTER_GLSL
#define CLEAN_WEATHER_CLUSTER_GLSL

#define CLUSTER_SET 2
#define CL_DECLARE_POOL
#define CL_DECLARE_INSTANCES
#include "clean_cluster_common.glsl"
#include "clean_cluster_select_common.glsl"
#define CL_DECLARE_SKIN_OUT
#define CL_DECLARE_SKIN_SCRATCH
#include "clean_cluster_skin_common.glsl"

// One skinned cluster of an avatar (this workgroup's selected entry).
struct WxClEntry
{
    uint s;         // its skinned-arena slot
    uint slot;      // the wearer's joint-table slot
    uint ri;        // the renderer Instance row (material, flags, skin_id)
    uint pslot;     // its pool slot (page)
    ClRecord rec;   // its cluster record
    uint vc, tc;    // vertices, triangles
    ClChar ch;      // its character's frame (character -> eye-relative)
    vec3 eye;       // the palettes' eye (world)
};

// This workgroup's entry (the pre-skin's 2D indirect grid: e = y * groups x + x). false: none, not
// skinned this frame (the arena was full: not drawn either), not an avatar's drawn rigged cluster.
// Workgroup-uniform.
bool wxClEntry(out WxClEntry x)
{
    x.s = CL_NONE; x.slot = 0xFFFFFFFFu; x.ri = 0xFFFFFFFFu; x.pslot = 0u; x.vc = 0u; x.tc = 0u;
    x.ch.m = mat3(1.0); x.ch.t = vec3(0.0); x.eye = vec3(0.0);
    const uint e = gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x;
    if (uint(cl_rig.length()) < CLK_HDR_WORDS || e >= cl_rig[CLK_SEL_COUNT]) return false;
    const uint sel = cl_rig[CLK_SEL_BASE] + e;
    if (sel >= uint(cl_sel.length())) return false;
    const uint s = clSkinnedCluster(sel);
    if (s == CL_NONE || s >= cl_rig[CLK_CAP] || (s + 1u) * CL_MAX_CLUSTER_VERTS * SV_WORDS > uint(cl_skin_out.length())) return false;
    const ClSelected cs = cl_sel[sel];
    if (cs.instance >= uint(cl_instances.length())) return false;
    const uint cf = cl_instances[cs.instance].flags;
    const uint need = CLI_RIGGED | CLI1B_DRAW | CLI1B_AVATAR;
    if ((cf & need) != need) return false;
    x.s = s;
    x.slot = (cf >> CLI1B_AV_SHIFT) & AV_SLOT_MASK;
    x.ri = cl_instances[cs.instance].render_instance;
    x.pslot = clRefSlot(cs.cluster_ref);
    x.rec = clLoadRecord(x.pslot, clRefIndex(cs.cluster_ref));
    x.vc = min(clRecVertCount(x.rec), CL_MAX_CLUSTER_VERTS);
    x.tc = x.vc == 0u ? 0u : min(clRecTriCount(x.rec), CL_MAX_CLUSTER_TRIS);
    x.ch = clSkinChar(clSkinRow(cf), false);
    x.eye = clSkinEye();
    return x.vc != 0u && x.slot < AV_SLOT_CAP;
}
// Vertex `local` of the entry: its skinned (current-pose) world position / normal.
vec3 wxClPos(WxClEntry x, uint local)
{
    return x.ch.m * SV_POS(cl_skin_out, x.s * CL_MAX_CLUSTER_VERTS + min(local, CL_MAX_CLUSTER_VERTS - 1u)) + x.ch.t + x.eye;
}
vec3 wxClNrm(WxClEntry x, uint local)
{
    return x.ch.m * SV_NRM(cl_skin_out, x.s * CL_MAX_CLUSTER_VERTS + min(local, CL_MAX_CLUSTER_VERTS - 1u));
}
// Its source vertex `local` (mesh space, before the bind shape): position, normal.
void wxClSource(WxClEntry x, uint local, out vec3 p, out vec3 n)
{
    const uint first = clFirstVertex(x.pslot, x.rec);
    const QvHeader qh = QV_LOAD_HEADER(cl_pool, max(first, QV_HEADER_VERTS));
    const uint b = (first + min(local, CL_MAX_CLUSTER_VERTS - 1u)) * QV_WORDS;
    p = qvPos(qh, cl_pool[b], cl_pool[b + 1u]);
    n = qvNormal(cl_pool[b + 3u]);
}
// Its triangle k's corner j (0 .. 2): the cluster-local vertex index (clamped into the cluster).
uint wxClCorner(WxClEntry x, uint k, uint j)
{
    return min(clLocalIndex(x.pslot, x.rec, 3u * k + j), x.vc - 1u);
}

#endif   // CLEAN_WEATHER_CLUSTER_GLSL
