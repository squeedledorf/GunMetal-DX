// vkclean -- GPU RASTER CURSOR PICK (#702 / #704): the result entry clean_pick.frag fills
// and LLCleanPickVK reads back (host-visible: one entry per frame slot + the synchronous
// click entry), and the candidate draw lists clean_pick_select.comp builds.
//
// The pick draws the instances whose bounds the ray crosses into a 1x1 target centred on
// the ray, with the world's own vertex shaders (clean_draw.vert / clean_draw_rigged.vert:
// static stream, pre-skinned rigged stream, GPU-animated worn attachments) and NO depth
// test: every fragment of every candidate along the ray inside the request's depth WINDOW
// is appended here with the raster's alpha verdict and the surface the CPU reports (touch
// UV, normal, tangent). The CPU sorts by distance and applies LL's pick rules
// (llcleancursorpick.h). There is no CPU pick behind it (#704).
//
// OVERFLOW (#704): more fragments in the window than the list holds -- the stored ones are
// an arbitrary subset, so the answer is not used as is. Every in-window fragment is also
// counted in a log-spaced depth histogram; the CPU picks the deepest bin edge whose
// cumulative count fits and asks again for [lo, that edge) (a click: synchronously; hover:
// with the next frame's request). An answer that fits is EXACT for its whole window; when
// LL's rules need fragments past it, the next window starts at its end (paging).
#ifndef CLEAN_PICK_COMMON_GLSL
#define CLEAN_PICK_COMMON_GLSL

// Fragments per layer and entry (C++ LLCleanPickVK::kWorldFrags / kHudFrags).
#define PICK_WORLD_FRAGS 1024u
#define PICK_HUD_FRAGS   128u
// Depth histogram bins per layer (C++ LLCleanPickVK::kHistWorld / kHistHud).
#define PICK_HIST_WORLD  64u
#define PICK_HIST_HUD    32u

// PickFrag.ids.y flags (C++ LLVKPickFrag).
#define PICK_FRAG_ALPHA_PASS 1u   // the raster keeps it (alpha rule of the draw)
#define PICK_FRAG_BACK_FACE  2u   // back face of a double-sided face
#define PICK_FRAG_DERIV_TAN  4u   // no tangent in the vertex stream: tng from screen derivatives

// One fragment. 96 B.
struct PickFrag {
    uvec4 ids;    // x clean Instance id, y PICK_FRAG_* flags, z effective alpha (float bits), w 0
    vec4  pos_t;  // xyz position (agent space; HUD layer: HUD space), w distance along the layer's ray
    vec4  nrm;    // xyz interpolated normal (world / HUD space, unit), w 0
    vec4  tng;    // xyz interpolated vertex-stream tangent (world / HUD space, unit), w bitangent sign
    vec4  st;     // xy raw texcoord0 (LL surface ST: before texgen), zw the base colour UV the
                  //    raster samples (planar texgen + TE / glTF transform / texture animation)
    vec4  duv;    // that UV per screen pixel: x du/dx, y dv/dx, z du/dy, w dv/dy
};

// Entry header. 512 B. The CPU writes serial / mode / rays / window and zeroes the counts
// and histograms before the frame is submitted; the select copies its candidate counts
// into `sel`.
struct PickHdr {
    uint  serial;     // request serial (0 = never written)
    uint  n_world;    // world fragments in the window (may pass PICK_WORLD_FRAGS: overflow)
    uint  n_hud;      // HUD fragments in the window
    uint  mode;       // request mode bits (C++ LLVKPickRequest::MODE_*)
    uvec4 sel;        // candidates: x static, y rigged, z HUD, w overflow bits (1 static, 2 rigged, 4 HUD)
    vec4  w_org;      // world ray origin (agent space), w length
    vec4  w_dir;      // unit direction, w alpha-blend threshold (PickAlphaThreshold)
    vec4  h_org;      // HUD ray origin (HUD space), w length
    vec4  h_dir;      // unit direction (+X), w 0
    vec4  win;        // depth windows along the rays: x / y world [lo, hi), z / w HUD [lo, hi)
    uint  hist_w[PICK_HIST_WORLD];   // in-window world fragments per depth bin (pickBin)
    uint  hist_h[PICK_HIST_HUD];
    uvec4 pad;
};

struct PickEntry {
    PickHdr  hdr;
    PickFrag world[PICK_WORLD_FRAGS];
    PickFrag hud[PICK_HUD_FRAGS];
};   // 512 + 1152 * 96 = 111104 B (C++ LLCleanPickVK::kEntryBytes)

// The depth bin of t in the window [lo, hi): log-spaced (fine near the window start, where
// the nearest-first rules decide). C++ LLCleanPickVK::binEdge is its inverse.
uint pickBin(float t, float lo, float hi, uint nb)
{
    const float span = max(min(hi, 1.0e6) - lo, 1.0e-3);
    const float u = log2(1.0 + max(t - lo, 0.0)) / log2(1.0 + span);
    return min(uint(max(u, 0.0) * float(nb)), nb - 1u);
}

// Draw-list buffer (clean_pick_select.comp writes, the pick draws read): 16 counters, then
// the DrawCmd lists -- static world [0, cap), rigged world [cap, 2 cap), HUD [2 cap, 2 cap + hud cap).
#define PICK_CNT_STATIC   0u
#define PICK_CNT_RIGGED   1u
#define PICK_CNT_HUD      2u
#define PICK_CNT_OVERFLOW 3u

#endif
