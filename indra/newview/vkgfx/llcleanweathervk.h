/**
 * @file llcleanweathervk.h
 * @brief Weather rendering (#783, milestone 1) for the Vulkan clean renderer: GPU-simulated
 *        rain / snow around the camera, the top-down occlusion map that keeps it (and the
 *        wetness) out from under roofs, and the wet-surface frame info.
 *
 * The weather STATE (sources, transitions, wetness) is slweather.h; this module draws it.
 * Our own implementation of published techniques: precipitation in a camera-centred
 * wrapping volume with a top-down occlusion depth map (Tatarchuk, "Artist-Directable
 * Real-Time Rain Rendering in City Environments", SIGGRAPH 2006 course; Lagarde, "Water
 * drop 3a -- Physically based wet surfaces", 2013), wet surfaces after Lagarde's porosity /
 * water-film model (clean_weather_surface.glsl).
 *
 * PER RECORDING (LLCleanRendererVK::recordWorld):
 *   prepare()          target-ensure step, before anything binds draw set 0: lifetime,
 *                      buffers / maps (device idle when they change; binding 49 then dirty),
 *                      pipelines (background batch), this frame's parameters, which
 *                      occlusion cascade (if any) is due, and the listener-cover readback
 *                      of this frame slot's last simulation -> SLWeather.
 *   [renderer]         LLCleanRendererVK::recordWeatherOcclusion, after the projector tiles
 *                      (the shadow cull scratch is free there): the static casters into the
 *                      map with the projector depth pipelines; then occlusionRendered().
 *   recordSim()        right after it (outside any rendering scope): the particle
 *                      simulation + visible-list compaction (clean_weather_sim.comp), and the
 *                      roof map's openness at the audio listener (one host-visible word); M2:
 *                      the rain splashes' spawns.
 *   recordSnow()       M2, right after it, while snow falls or lies: the persistent cover grid
 *                      and the moving objects' own snow (clean_weather_snow.comp).
 *   recordVeil()       #954, at the start of render pass 2 (after the volumetric fog's apply):
 *                      the fog-off precipitation veil over the open opaque scene + sky.
 *   recordDraw()       inside render pass 2b, after the post-water particles: one indirect
 *                      instanced draw of the visible list (clean_weather.vert / .frag); M2 the splash ring.
 *   fillFrameInfo()    ssr.weather: wetness, ripples, snow cover, the maps' frames (the
 *                      ones RENDERED, not this frame's camera) -- clean_weather_surface.glsl.
 *
 * GPU-DRIVEN: the particles never leave the GPU (positions in a persistent SSBO, type /
 * activity / speed hashed from the index); the CPU writes one 496 B parameter block per
 * frame and records one dispatch + one indirect draw, whatever the count. The intensity is
 * a threshold on the hashed activity (no re-allocation).
 *
 * OCCLUSION MAPS (two cascades, each double-buffered):
 *   COARSE  2 x 256 x 256 D32 (2 x 256 KB) over 4 x RenderVKWeatherRadius metres (clamped
 *           64..256 m): the precipitation's roof test and the surfaces beyond the near map.
 *   NEAR    2 x RenderVKWeatherNearRes^2 D32 (1024: 2 x 4 MB) over RenderVKWeatherNearExtent
 *           metres (32: ~3 cm texels; RenderVKWeatherNearCascade): the surfaces near the
 *           camera, so the dry patches under roofs / trees / furniture have fine edges. The
 *           lit FS blends to the coarse map over the outer tenth of it.
 *   Orthographic from 128 m above to 64 m below the camera, centre snapped to an eighth of the
 *   extent (a whole number of texels: no shimmer). M2 TILTED (RenderVKWeatherTiltedMap): the
 *   projection is SHEARED along the precipitation's mean fall direction d ~ (s, -1) (the slow
 *   mean wind, rain ~9 m/s / snow ~1 m/s, capped at 60 degrees) like a directional shadow map:
 *   the texels stay on the horizontal snapped grid of the box's top plane, the depth axis
 *   leans into the wind, and the centre sits upwind of the camera by the slant (the camera's
 *   own column), so the near field stays covered. Readers project a point back along d onto
 *   the top plane (P.xy - (top - P.z) s) and compare as before. A cascade re-renders when the
 *   direction moves more than 2.5 degrees from the one it was rendered with (the double
 *   buffer swaps it in seamlessly); smaller drifts keep the old shear (no per-frame re-render
 *   during a slow wind change). Caveat: at a steep tilt the near map's footprint slides by
 *   |s| per metre of height, so points well above / below the camera fall back to the coarse
 *   map sooner. A cascade is also due when the camera crosses into another snap cell, after a
 *   camera cut, and every
 *   RenderVKWeatherOcclusionInterval frames otherwise; AT MOST ONE renders per recording
 *   (both due: they alternate), so the per-frame cost stays one projector-tile-sized pass.
 *   Statics only (avatars, worn attachments and -- M2 -- moving objects do not shelter). The
 *   surfaces read them with a jittered,
 *   receiver-plane PCF (clean_weather_surface.glsl): soft drip lines, not texel steps.
 *
 * ZERO COST WHEN OFF: no weather (SLWeather::State::visible() false: no precipitation and
 * the surfaces are dry) -> the renderer never creates this object; once it turns off ->
 * destroy() + delete (no pipeline, buffer or image survives; binding 49 back to the stand-in,
 * ssr.weather zeros: the lit FS's wet branch is one uniform test).
 *
 * MEMORY: particles 16 B + list 4 B per particle (medium: 32768 -> 640 KB), the coarse maps
 * 512 KB, the near maps 8 MB at 1024 (none with the near cascade off), 2 x 496 B parameters,
 * the splash ring 32 KB; while snow falls or lies, the cover grids 8 MB (near res 1024; M3 rgba32f) and the
 * per-instance snow 256 KB per 16k instances (M3: + the avatars, the mesh -- M3 below). GPU: the simulation ~0.02-0.05 ms, the draw a few
 * thousand small blended quads (~0.1-0.3 ms at 1440p, rain at full intensity), the splash ring
 * ~12k vertices (dead slots collapse in the VS), the map ~one projector tile every 30 frames
 * (plus one per 2.5 degree change of the rain direction), the snow passes one 512^2 texel
 * dispatch + one thread per instance slot (~0.02-0.05 ms). CPU: while the weather runs the
 * ingest's mover tracker and change-sphere bookkeeping stay on (gVKProjShadowsUsed): the roof
 * map needs the movers kept out of it.
 *
 * M2 (#783):
 *   TILTED MAP          above (RenderVKWeatherTiltedMap); walls wet only facing into the rain.
 *   LIGHTNING           flashNow(): the state's flash -> ssr.weather[8] / [9] (a soft, unshadowed
 *                       cool-white fill in lightSurface, a brighter sky, lit volumetric clouds);
 *                       never in probe captures or snapshots (RenderVKWeatherLightning).
 *   CLOUD / SKY         the weather's cloud cover -> the cloud layers take max(region, weather);
 *                       the sky darkens a little with cloud and rain (RenderVKWeatherSky).
 *   PERSISTENT SNOW     the cover grid + the moving objects' own snow (recordSnow,
 *                       clean_weather_snow.comp; RenderVKWeatherSnowCover / SnowMovers). Needs
 *                       the near cascade (the grid lies on its top plane).
 *   SPLASHES            rain hits near the camera -> a GPU ring drawn after the streaks
 *                       (RenderVKWeatherSplashes).
 *   WET REFLECTIONS     puddles / soaked flat ground reach the SSR + RT reflection thresholds
 *                       (clean_weather_surface.glsl); no separate path.
 *   PROBES / MIRRORS    the precipitation (streaks, flakes, splashes) is drawn in the main view
 *                       only. Reflection probes are a static environment (a capture holding a
 *                       frozen curtain of drops for minutes would be wrong). The planar mirror
 *                       would need its own culled visible list (the simulation culls against
 *                       the main view) and draw pipelines for the mirror pass's attachments --
 *                       not cheap, left out; the mirror still shows the wet / snowy surfaces,
 *                       the sky and the lightning.
 *
 * M3 (#783): REAL SNOW
 *   DEPTH FIELD         (RenderVKWeatherSnowDepth) the cover grid is rgba32f and its A channel
 *                       the snow depth (m) per texel (~6 cm at the defaults, the near cascade's
 *                       32 m; no coarser ring: past it the coating alone), lying on a FILTERED
 *                       BASE (a paired r32f image): the column tops closed then opened at 2
 *                       texels -- gaps under ~25 cm bridged, features under ~30 cm wide (bars,
 *                       pickets, posts, rails) not surfaces. A texel it has not seen starts at
 *                       the GLOBAL depth (SLWeather::State::snowDepth, from the snowfall's
 *                       intensity x duration, melting with the cover) x the base's slope factor
 *                       (1 level, 0 past ~55 degrees) -- the same on every open surface; the
 *                       snowfall fills a texel toward that level, the melt lowers it. Only
 *                       local edits (drifts, footprints) are lost when the camera leaves.
 *   CONNECTIVITY        (clean_weather_snow_join.glsl) two texels are one snow sheet when their
 *                       snow tops are within 3 cm + 45 degrees, or the lower one's snow has
 *                       filled up to the higher one's base; else a cut.
 *   TRANSPORT           (RenderVKWeatherSnowDrift / Slide / Settle) in the cover kernel, gather
 *                       form from the previous grid, mass conserved, only between joined
 *                       texels: the snow top relaxes to the angle of repose (settle / slide),
 *                       the mean wind carries exposed snow downwind (blocked by a base above
 *                       the snow top: piles against walls, sheltered lee).
 *                       clean_weather_snow.comp.
 *   SNOW MESH           (RenderVKWeatherSnowMesh / MeshShadows) built by prepare() from the
 *                       published grid (one compute: a snow-top + a base vertex per texel;
 *                       triangles wherever any corner is deeper than 2 cm and the edges are
 *                       joined texel by texel; marching-squares diagonals; vertical skirts down
 *                       to each side's own base at cuts and stride seams, so nothing hangs from
 *                       an eave; a spike clamp against the joined neighbours; 4 x 4 blocks at
 *                       stride 1 / 2 / 4 by distance; blocks outside a cone around the view
 *                       skipped) and drawn with one indexed indirect draw in the deferred
 *                       G-buffer (depth-tested and written, LEQUAL) and -- RenderVKWeatherSnowMesh-
 *                       Prepass, default on -- first in the depth pre-pass (depth, AO normal,
 *                       camera-only motion: recordMeshDrawPre), so AO / the motion vectors / the RT
 *                       shadow mask / occlusion culling see the snow, not what it buries (deep
 *                       snow over an avatar: its motion and occlusion ghosted onto the snow with
 *                       ray-traced GI -- the "few centimetres of parallax" assumption broke) and in
 *                       every sun cascade (the per-frame overlay with the #567 split). Forward
 *                       frames, probe faces and the mirror: the coating alone. clean_weather_
 *                       snowmesh.*. RT: the mesh is NOT in the TLAS (ray-traced shadows /
 *                       reflections see the coated ground under it) -- a slow-cadence BLAS
 *                       rebuild is the follow-up.
 *   OUTER RING          (RenderVKWeatherSnowOuterRing / RingExtent 128 / RingRes 256 /
 *                       RingInterval 4) a second, coarser depth grid (rg32f ping-pong: filtered
 *                       base + depth; ~0.5 m texels) on the COARSE map's plane, which grows to
 *                       1.25 x the ring's extent. Same model at its own scale (close / open at
 *                       one texel, the join rule, repose; no drift), stepped every few
 *                       recordings with the elapsed time, snapped to an eighth of its extent
 *                       (exact carry). Hand-off: the inner grid is authoritative -- the ring
 *                       takes the mean of the inner texels over its footprint wherever the
 *                       published inner grid covers it, and the inner grid's entering texels
 *                       resample the ring (bilinear on the same base); the global level only
 *                       for texels neither has seen. clean_weather_snow.comp -DWX_SNOW_RING.
 *   SUPPORT CAP         (#783) loose snow on a raised support stands no taller than the repose
 *                       allows over the support's extent: depth <= 4 cm cohesion + tan 35 x the
 *                       distance along its top to where its base drops away (a drop the lower
 *                       side's snow does not fill). A chamfer distance carried with the grid
 *                       (the base images' .y, the ring's .z: + 1 x 512^2 x 4 B per base image,
 *                       ring rgba32f), converging over a support's half-width in steps; ordinary
 *                       ground has no edge. On a base steeper than the repose the depth is held to
 *                       its fill level + cohesion. The excess is dropped. Small spheres / posts
 *                       carry a small dome / cone, wide tops reach the full depth.
 *   LOD (no pops)       world space around the camera, never grid space (a snap moves no
 *                       band): the inner mesh hands over to the ring at 0.75 .. 1 x its reach
 *                       (half its extent - half a snap cell - 0.5 m: ~10.5 .. 14 m at 32 m),
 *                       (#783 CLOSURE: a sheet continues 2 texels into every column it cannot
 *                       join -- a deck / step / wall above it, a cover -- as a sloped field meshed
 *                       with its own tops, so the snow meets that geometry's face with no gap;
 *                       the APRON, RenderVKWeatherSnowToeReach, runs it further under covers)
 *                       geomorphing onto the ring's own triangles (height + normal) by the
 *                       band's end, where the ring takes over (#783: no screen dither -- two
 *                       interleaved surfaces read as an AO crease; the cascades sink the ring
 *                       under the inner layer in the band); the ring sinks into the coating at 0.8 .. 1 x its
 *                       reach (~44 .. 55 m at 128 m); vertices a coarser stride drops morph onto
 *                       its lattice before the switch. clean_weather_snowmesh.*.
 *   FOOTPRINTS          (RenderVKWeatherSnowFootprints) the avatar pass appends each standing
 *                       avatar's feet (ankle / foot / toe joints of the joint table: GPU- or
 *                       CPU-animated alike) near the field to a stamp list; one 8 x 8 workgroup
 *                       per foot (indirect) presses the snow under the sole. Vehicles / movers:
 *                       MOVER TRACKS (M5) below.
 *   LIMB CONTACT        (#783 follow-up; RenderVKWeatherSnowLimbs / LimbRange) the same avatar pass
 *                       (now BEFORE the cover pass) turns every limb segment of the joint table into a
 *                       box (thigh, shin, upper arm, forearm, hand, pelvis -> chest, chest -> head; the
 *                       Bento hind limbs and the tail only when an animation turns them away from their
 *                       rest pose -- at rest they hang to the ground behind every biped; radius x the
 *                       thigh's length / 0.49) and appends those whose lowest point dips below the snow
 *                       top (the published inner grid) to the MOVER list's own limb range (kLimbStampMax
 *                       slots after the movers': mv_head.w counts them). The cover pass presses them
 *                       compaction-first, the berm pass lays the rim from what each removed: along the
 *                       pelvis's motion, or all around a still press (a kneel, a hand put down). The
 *                       feet are not double-counted: while the feet stamp the shin stops short of the
 *                       sole and there is no foot box (sitting / footprints off: the foot is a box).
 *                       Avatars and animesh within LimbRange of the camera; <= 22 boxes each; GPU only.
 *   BODY CONTACT        (#783; RenderVKWeatherSnowBodyContact / BodySupport / BodyDrift) an avatar with a
 *                       shelter layer presses the snow with its REAL UNDERSIDE instead of joint boxes: the
 *                       shelter raster also writes a BOTTOM map (the lowest skinned / worn surface per
 *                       texel; the raster target's lower half, + each layer's raster-time frame), and
 *                       the avatar pass's extra per-layer workgroups split it into blocks, append the
 *                       blocks whose underside is in the snow as BODY BOXES (the limb range; the cover
 *                       pass presses their texels to the bottom map itself, a contact on the ground at
 *                       full weight; the berm pass lays their rims) and join them into blobs: a
 *                       supporting blob of a hoof's / foot's / hand's size leaves a footprint. So a
 *                       quadruped's front hooves (rigged to the arm bones, the hoof below the joint)
 *                       press like its rear ones. No layer (cluster LOD, past the layer cap) or a map
 *                       rasterized too far from where the avatar is now: the joint boxes, with an arm
 *                       whose hand reaches the ground standing as a leg (RenderVKWeatherSnowArmLegs).
 *   MOVER TRACKS (M5)   (RenderVKWeatherSnowMovers + RenderVKWeatherSnowMoverPress / MoverBerm /
 *                       MoverStamps) moving objects -- vehicles (sat on or not), physical objects,
 *                       keyframed / scripted / glTF-animated movers: the ingest's mover tracker
 *                       (instance bit21, LLCleanIngest::mDynCasters) -- press tracks into the
 *                       depth and push berms beside them. GPU-driven: the instance pass (which
 *                       already reads every slot's current and previous model matrix) appends each
 *                       mover instance whose box centre moved faster than a least speed this step
 *                       and whose box bottom reaches the snow (published inner grid / outer ring at
 *                       its centre) to a bounded stamp list (own set binding 7; per-instance boxes
 *                       = per face / prim: wheels, skids and treads stamp their own narrow tracks,
 *                       a body clearing the snow stamps nothing; worn attachments, rigged / animesh,
 *                       HUDs and terrain are never stamps, a spinner turning in place stays under the speed -- avatars have their
 *                       footprints). It now runs BEFORE the cover pass, which GATHERS the list
 *                       (each workgroup culls it to its tile in shared memory, then per texel: the
 *                       box's underside below the snow top presses the depth down to it -- a packed
 *                       share stays --, a texel beside / ahead of a box takes a berm / bow deposit
 *                       from the snow above its contact, swept-length scaled: volume roughly
 *                       conserved; the repose rounds it, the snowfall refills the track).
 *                       COMPACTION FIRST (#783 feedback): the press is idempotent -- the floor is
 *                       max(underside, the packed layer: the fill level x fresh / packed density
 *                       ~0.3), never raised -- and counts what it took per box (fixed point); a
 *                       second pass (-DWX_SNOW_BERM, after the cover pass) deposits only the berm
 *                       share (RenderVKWeatherSnowMoverBerm, 0.15) of what each box ACTUALLY removed
 *                       this step, a berm at most 1.4 x the undisturbed depth it ploughed and no
 *                       steeper than the repose (band width); removed vs deposited are read back
 *                       (moverStats, logged every ~10 s while movers press). The ring
 *                       presses the same boxes at its texel centres where the inner grid does not
 *                       cover it (coarse; on its step cadence). A resting object (no motion this
 *                       step) stops stamping; its trench stays in the field like any other depth
 *                       (carried across snaps, frozen through snapshots). The CPU gates the feature
 *                       on the tracker having a recently moved caster within the field's reach (no
 *                       movers: the cull is skipped, nothing appended) and reads the list's head
 *                       back one frame slot later for live / cap / peak / overflow (moverStats(),
 *                       MemTrend "vk-weather-movers"). The cap is first come (an atomic append),
 *                       not a sort: everything appended lies within the field's reach, and boxes
 *                       outside the inner grid may only take the first half of it.
 *   SNOW ON AVATARS     (RenderVKWeatherSnowAvatars) one invocation per joint-table slot keeps
 *                       an amount (builds while the pelvis is still and the head sky-open,
 *                       shed by fast motion) and the chest's frame; rigged draws (slot via the
 *                       skin meta / skin cache) and worn rigid attachments (skin_id) take it in
 *                       the lit FS -- never the grid (avatars are not in the roof map, sitting
 *                       or not). The CPU writes one alive / sitting bit per slot (256 B).
 *   COST                the cover kernel ~14 extra texel reads per texel with drift on + the
 *                       base filter (~11 map gathers per texel into a 26^2 shared tile, 8
 *                       separable morphology passes) (512^2: ~0.15-0.3 ms on a GTX 1080 class
 *                       GPU), the avatar pass 1024 threads, the stamps <= 256 tiny workgroups,
 *                       the mesh build one 512^2 dispatch (10 reads per texel; per cell ~40
 *                       reads at stride 1, ~120 at stride 2, ~360 at stride 4: every texel
 *                       along the cell edges, its 4 neighbours re-derived),
 *                       the draws ~100-300k triangles near the camera in the
 *                       G-buffer + each overlaid cascade. Nothing of it exists without snow.
 *   MEMORY              grids 8 MB (2 x 512^2 rgba32f, was 4 MB rgba16f) + bases 4 MB (2 x
 *                       512^2 rg32f: + the support cap), instance image x4
 *                       (rgba32f) + 32 KB of avatar rows, avatar buffers 36 KB, outer ring 2 MB
 *                       (2 x 256^2 rgba32f; allocated with the grids even with the ring off: the
 *                       sets bind it statically), mesh ~25 MB (12 MB inner + 3 MB ring vertices: #783 three per texel,
 *                       9 MB indices) while the snow is deeper than 2 cm.
 *
 * #901 PER-AVATAR SHELTER MAPS (llcleanweathervk_shelter.cpp, clean_weather_shelter.comp/.glsl;
 * RenderVKWeatherAvatarShelter): the roof maps hold static casters only, so an umbrella or a hat
 * did nothing. Now each avatar (animesh too) near the camera owns a LAYER: a top-down height map
 * of itself and what it wears, in its OWN frame (centre = its pelvis, turned with its heading;
 * the square grows to its attachments' reach, capped), so it moves and turns with the avatar and
 * nothing smears.
 *   LAYERS      up to 16, picked on the CPU each recording (a bounded list: the joint-table slots'
 *               avatars by distance): the own avatar and the 3 closest at 256^2 (~1 cm at 2.5 m),
 *               the next 12 at 128^2; sticky (an avatar keeps its layer while it stays in its
 *               tier); RenderVKWeatherAvatarShelterCount / Range.
 *   ATLAS       one r32ui raster target + two rg32f images (ping-pong; x the top height above the
 *               pelvis, y M3+ the avatar's snow depth), the tiles side by side, + 4 rows of per-slot
 *               frames for the lit FS (draw set 0 binding 49 [8] / [9], ssr.weather[6].w).
 *   ONE GPU CHAIN per recording (recordShelter, right after the MAIN cull + pre-skin, before the
 *               G-buffer): PREP (the layers' frames from the joint table, the per-slot rows), FILTER
 *               (the main cull's visible rigged draws / worn attachments of the due layers' avatars
 *               -> a bounded work list), CLEAR (the due tiles), RASTER (one workgroup per draw: its
 *               triangles from the mega index buffer -- the pre-skinned vertices, or the quantized
 *               ones x the model matrix -- rasterized top-down with atomicMax), PUBLISH. The own
 *               avatar every recording, the others RenderVKWeatherAvatarShelterPerFrame at a time.
 *               Only what the main view drew is drawn (an avatar off screen keeps its last map).
 *   USES        the lit FS: an avatar's fragment under a worn surface is sheltered (wetness;
 *               M3+ snow); the precipitation: rain under an avatar's top is hidden, a drop crossing
 *               it splashes on it (an umbrella's canopy).
 *   VRAM        soft allocation, retried later on refusal; tier configurations drop under the VRAM
 *               ladder's pressure: 4 x 256^2 + 12 x 128^2 (atlas 1024 x 512: ~10.5 MB), 16 x 128^2
 *               (~5.3 MB), 8 x 128^2 (~2.7 MB); back up after 30 s calm. Nothing exists without
 *               precipitation / snow (freed 30 s after), avatars near or the setting.
 *   STATS       shelterStats(): layers live / cap / peak, work items live / cap / peak / overflow,
 *               bytes; MemTrend "vk-weather-shelter"; a log line every ~10 s while it runs.
 *
 * #901 REST-POSE ACCUMULATION (llcleanweathervk_accum.cpp, clean_weather_accum.comp/.glsl;
 * RenderVKWeatherAvatarAccum): the shelter maps decide where rain and snow reach an avatar NOW;
 * shading straight from them made a body part under a swaying umbrella flash snowy / wet. Now the
 * avatars with a layer own a GRID over their REST POSE (rigged: the bind-pose position; worn rigid:
 * the attachment joint's rest transform x its joint offset) that integrates, per cell and per
 * rest-normal bucket (up / side / down), the exposure of the vertices in it at their CURRENT pose:
 * snow falls in, slides off, melts with the world and under any cover (#903, per cell); wetness
 * soaks and dries. The lit / G-buffer FS reads the cell of the fragment's rest position (a varying
 * from the VS): one trilinear fetch (two where the buckets blend).
 *   POOL        RenderVKWeatherAvatarAccumCount grids (16), RenderVKWeatherAvatarAccumRes (2: 32 x 32 x
 *               64 cells x 3 buckets, rgba8: 768 KB a grid, ~12 MB for 16; 1: 24 x 24 x 48, 324 KB;
 *               0: 16 x 16 x 32, 96 KB) + 9.4 MB scratch (4 grids an update) + the 16 KB record.
 *   SCHEDULE    the grids due at RenderVKWeatherAvatarAccumRate (15 Hz), at most 4 a recording,
 *               the oldest first; dt per grid. Grids follow their avatar's UUID (a re-slotted
 *               avatar keeps its grid), are freed RenderVKWeatherAvatarAccumGrace s after it lost its
 *               layer; the own avatar's is snapshotted every 2 s and restored into a new module.
 *   STATS       accumStats(); MemTrend "vk-weather-accum"; a log line every ~10 s; GPU mark
 *               c_weather_accum.
 *
 * M4 (#783): RUN-OFF -- water streaming off covered surfaces (llcleanweathervk_runoff.cpp,
 * clean_weather_runoff.comp; RenderVKWeatherRunoff). Only while SLWeather::State::runoff > 0
 * (it rains on wet surfaces / snow melts, and for WeatherRunoffDrainSeconds after: draining).
 *   FLOW                the PUBLISHED near map as a heightfield on a flow grid (R^2,
 *                       RenderVKWeatherRunoffRes, a whole number of near texels per texel),
 *                       rebuilt only when a new near map is published (the roof map's refresh),
 *                       in prepare() -- before every reader of that recording, so the grid and
 *                       its frame (ssr.weather[13] / [14]) always describe the published map:
 *                       outlets (a neighbour more than RenderVKWeatherRunoffEdgeDrop lower: eaves,
 *                       table edges), a same-surface jump flood (a flat roof knows its nearest
 *                       edge), D8 steepest descent on height + a millimetre-per-metre pull toward
 *                       that edge, pointer jumping (every texel's outlet + its rank in log2 R
 *                       passes), the catchment per outlet (atomic), the per-texel flow, the
 *                       bounded EMITTER list (outlets with a catchment: edge position, catchment
 *                       m2, landing height) and the surfaces' flow map. ~25 dispatches of R^2.
 *   DRIPS               a share of the precipitation pool (2 slots per drip, the pool's tail:
 *                       the rain keeps the rest): the spawn kernel (one invocation per emitter,
 *                       indirect) discharges catchment x strength as drips, thinned to the
 *                       quality's drip budget; the simulation drops them under gravity + wind,
 *                       splashes them (the M2 ring) on their landing height and frees the slot;
 *                       they draw as streaks with the rain.
 *   STREAMS             (RenderVKWeatherRunoffStreams) an emitter whose discharge passes the
 *                       stream threshold draws a thin ribbon from its edge to its landing (one
 *                       indirect instanced draw over the emitter list, pass 2b) and splashes there.
 *   LANDING / RIVULETS  (RenderVKWeatherRunoffRivulets) the lit FS: the ground at a drip line's
 *                       landing wets sooner and seeds puddles; walls / steep roofs carry animated
 *                       rivulets (thicker under an outlet, on steep roofs by their flow).
 *   AUDIO               the spawn kernel sums the run-off's loudness near the listener into the
 *                       listener readback (SLWeather::runoffNear -> the drip bed outdoors too).
 *   TIMING              SLWeather::State::runoff: starts once the surfaces are wet, follows the
 *                       rain (and melt water), drains after it, never above the wetness.
 *   COST                off / dry: nothing allocated or recorded (ssr.weather[13].w 0: one
 *                       uniform test in the lit FS). On: the flow build per near-map publish
 *                       (~25 passes over 256^2: ~0.05-0.1 ms on a GTX 1080 class GPU), one
 *                       spawn dispatch + the drips inside the simulation's dispatch per frame, one
 *                       indirect stream draw, the lit FS's flow lookup (2 fetches) near the camera.
 *   MEMORY              scratch 9 x R^2 words (2.25 MB at 256), flow map R^2 rgba32f (1 MB), the
 *                       emitter list 64 B + cap x 32 B (2048 / 4096 / 8192 by quality); the drips
 *                       live in the precipitation pool (1024 / 2048 / 4096 by quality). Stats:
 *                       runoffStats(), the MemTrend probe "vk-weather-runoff".
 *
 * #954 PRECIPITATION RATE (slweather.h: rain mm/h, snow cm/h, bands): the look scales with the
 * physical rate, inside the same fixed particle pool (kCaps; no re-allocation, no extra memory):
 *   DENSITY             the active share follows the amount (the rate ladder), and the box shrinks
 *                       by up to kBoxShrink toward the top of the ladder: the same count, denser near
 *                       the camera; the distance is the veil's.
 *   RAIN                each drop's size is hashed from an exponential spread over a scale that
 *                       grows with the rate (Marshall-Palmer-like: heavy rain, big drops), its fall
 *                       speed the measured terminal velocity of that size, its streak width its size;
 *                       the streak exposure and opacity grow with the rate. Splash / ripple density
 *                       follows the denser drops (and the ripple rows the amount).
 *   SNOW                each flake's size a spread over a clumping factor (big aggregates in heavy
 *                       calm snow, broken up by a strong wind); the wind's eddies swirl them (a smooth
 *                       field of position and time, x the wind); a flake moving farther than its size
 *                       over the snow exposure is drawn motion-blurred along its motion (horizontal
 *                       blizzard streaks).
 *   BLOWING SNOW        (RenderVKWeatherBlowingSnow; not on the low quality) up to kBlowShare of the
 *                       slots stream along the first surface under them (the near / coarse map) at a
 *                       few decimetres while the wind lifts lying snow (SLWeather::State::blowing).
 *   PRECIPITATION MEDIUM (clean_weather_veil.glsl; owner feedback 2026-10-23: no screen-space
 *                       pattern) the precipitation's own extinction (SLWeather::State::precipExt:
 *                       the rates + blowing snow, x WeatherFogAmount -- the one source) as a medium
 *                       only where it falls:
 *                       FOG ON (Frame::volFogOn; ssr.weather[16]): clean_vol_inject.comp adds it per
 *                       froxel x the roof maps' openness at the cell's world point (a room's air is
 *                       clear, from the doorway too) x a value-noise field advected with the gusty
 *                       wind and the fall (rain: tall sheets falling fast; snow: swirls) -- moving,
 *                       world-anchored density the fog lights (shadowed sun, ambient, local lights)
 *                       and filters temporally; the blowing part hugs the first surface under it.
 *                       Motion octaves: 2, 1 on the small VRAM tier, 0 on the low quality
 *                       (RenderVKVolumetricFogPrecipMotion scales it). Every fog consumer (opaque,
 *                       alpha faces, water, particles) gets it through volFogApply; past the grid's
 *                       far plane clean_vol_apply.glsl adds it analytically.
 *                       FOG OFF (RenderVKWeatherVeil; ssr.weather[15]): a noise-free analytic veil,
 *                       T = exp(-sigma d) below kVeilCeiling, the in-scatter colour from the world
 *                       draws' ambient / sun by the snow's share, applied ONLY to surfaces open to
 *                       the sky at their own position (one roof-map tap): recordVeil -- one
 *                       fullscreen draw right after the fog's apply point, before any transparent
 *                       (GPU mark g_weather_veil) -- and the alpha faces, the water (refraction
 *                       un-veiled first) and the "over" particles themselves. Glow / reactive
 *                       write-masked. Not in probes / mirrors.
 *                       Every precipitation particle takes the analytic veil of its own distance.
 *   COST                the same dispatch and draw; the simulation adds one map read per blowing slot.
 *                       Fog on: the inject pass adds <= 3 map taps + <= 2 value-noise lookups per
 *                       froxel (c_vol_inject; ~0.02-0.05 ms on the default grid), no veil pass. Fog
 *                       off: one fullscreen pass, one depth fetch + one map tap per pixel
 *                       (g_weather_veil, ~0.03-0.06 ms at 1440p). No memory (a layout + pipeline).
 */
#ifndef LL_LLCLEANWEATHERVK_H
#define LL_LLCLEANWEATHERVK_H

#include "vk_device.h"
#include "vk_descpool.h"
#include "vk_pipeline.h"   // PipelineJobSlot (first-use pipelines off the render thread)
#include "lluuid.h"         // #901 the accumulation grids' owners

#include <cmath>
#include <cstdint>
#include <string>

class LLCleanWeatherVK
{
public:
    // What the module needs from the renderer this recording.
    struct Frame
    {
        VkDeviceContext* ctx = nullptr;
        VkFormat hdrFormat = VK_FORMAT_UNDEFINED;     // render pass 2's attachments
        VkFormat glowFormat = VK_FORMAT_UNDEFINED;
        VkFormat reactFormat = VK_FORMAT_UNDEFINED;
        VkFormat depthFormat = VK_FORMAT_UNDEFINED;
        bool     reverseZ = true;                     // the main view's depth convention
        uint32_t rw = 0, rh = 0;                      // render extent
        uint32_t frameSlot = 0;                       // frame-in-flight slot (parameter ring)
        const float* camTime = nullptr;               // camera world pos xyz, time (s)
        const float* sunDir = nullptr;                // VKDrawPush sun_dir (xyz)
        const float* sunColor = nullptr;              // VKDrawPush sun_color (rgb)
        const float* ambient = nullptr;               // VKDrawPush ambient_color (rgb)
        float waterHeight = -1e9f;                    // the region's water plane (m)
        bool  underwater = false;                     // camera under water: no precipitation
        bool  offscreen = false;                      // snapshot: frozen particles
        uint32_t snapSample = 0;                      // #932: a supersampled shot's recording 1..N (0 = none)
        bool  cameraCut = false;                      // the renderer's temporal cut: the roof maps re-render, drips clear
                                                      // (#895: the snow grids are NOT dropped -- shiftOrigin / resetGround)
        // M2 persistent snow (clean_weather_snow.comp): draw set 0's layout (its compute
        // pipelines read the frame info, the maps and the instance rows) and the live
        // instance count (the moving objects' snow, one texel per slot).
        VkDescriptorSetLayout dslDraw0 = VK_NULL_HANDLE;
        uint32_t instanceCount = 0;
        // M3 SNOW MESH (RenderVKWeatherSnowMesh): the main view's WORLD-space view-projection
        // (drawPushIn: its w row is the view direction -- the build's view-cone cull), the
        // world draw layout (draw sets 0 / 1 + the 16 B view push) the mesh pipelines use, and
        // the renderer's templates of the deferred G-buffer and sun-cascade pipelines (vert /
        // frag / layout / vertex input cleared; null: not built -- no mesh).
        const float* viewProj = nullptr;
        VkPipelineLayout drawLayout = VK_NULL_HANDLE;
        const GraphicsPipelineDesc* gbufDesc = nullptr;
        const GraphicsPipelineDesc* shadowDesc = nullptr;
        // The forward lit pass 1's template (LIT_RIG's state, stages / layout / vertex input
        // cleared; null: none -- the mesh is then G-buffer only) and the ray-query draw layout
        // (null: no RT -- the plain forward variant only).
        const GraphicsPipelineDesc* fwdDesc = nullptr;
        VkPipelineLayout drawLayoutRt = VK_NULL_HANDLE;
        // The DEPTH PRE-PASS templates (LLCleanRendererVK mPreDescCl, one per pass 1a attachment
        // set: 0 depth only, 1 + AO normal, 2 + normal / motion / no-history, 3 motion /
        // no-history only; stages / layout / vertex input cleared; null: none -- the mesh then
        // stays out of the pre-pass).
        const GraphicsPipelineDesc* preDesc[4] = {};
        // #901: the VRAM ladder's state (LLVramBudgetVK mode: 0 calm, 1 soft, 2 reserve; its
        // engaged rung): the shelter atlas's tier configuration drops under pressure.
        uint32_t vramMode = 0, vramRung = 0;
        // #946: the VRAM budget's capacity (bytes; min(user cap, OS budget); 0 = unknown). A
        // card whose capacity is below kSmallVramCapacity runs the weather's SMALL tier (fewer,
        // coarser maps / grids: about a quarter of the default footprint and of its GPU time).
        uint64_t vramCapacity = 0;
        // #954: the volumetric fog runs this recording (it then carries the precipitation's medium:
        // ssr.weather[16]; off: the analytic veil, ssr.weather[15] + recordVeil).
        bool volFogOn = false;
    };

    // #901 PER-AVATAR SHELTER: the renderer's buffers the chain reads, at record time (right after
    // the main cull and its pre-skin): the main cull's per-instance candidates (VkDrawIndexed-
    // IndirectCommand per instance id) and their meta (~0 = not drawn), the skinned vertices (null:
    // no rigged content -- the rigged draws are skipped), the mega index buffer, the skin meta
    // words (null: as the skinned vertices), the instance count, the palettes' eye (world).
    struct ShelterInputs
    {
        VkBuffer cand = VK_NULL_HANDLE, candMeta = VK_NULL_HANDLE;
        VkBuffer skinVbo = VK_NULL_HANDLE, ibo = VK_NULL_HANDLE, skinMeta = VK_NULL_HANDLE;
        uint32_t instanceCount = 0;
        float    palEye[3] = {0.f, 0.f, 0.f};
        // #901 / #909 follow-up, CLUSTER LOD (only when this frame's cluster pre-skin was recorded;
        // null otherwise): the cluster set of this frame and its layout (set 2 of the cluster
        // kernels), the indirect arguments of the pre-skin's per-entry grid (buffer + byte offset:
        // clean_cluster_skin_common.glsl CLK_DISPATCH). The cluster-drawn avatar parts are rastered
        // into the shelter maps and sampled into the grids from the skinned arena.
        VkDescriptorSet       clSet = VK_NULL_HANDLE;
        VkDescriptorSetLayout clDsl = VK_NULL_HANDLE;
        VkBuffer              clArgs = VK_NULL_HANDLE;
        VkDeviceSize          clArgsOffset = 0;
    };

    LLCleanWeatherVK() = default;
    ~LLCleanWeatherVK() = default;   // destroy() is explicit (needs the device)
    LLCleanWeatherVK(const LLCleanWeatherVK&) = delete;
    LLCleanWeatherVK& operator=(const LLCleanWeatherVK&) = delete;

    // Once per recording, BEFORE anything binds draw set 0. true = weather this recording
    // (wet surfaces at least; the precipitation when precipNow()). Off: frees everything.
    bool prepare(VkCommandBuffer cmd, const Frame& f);
    // #895 ORIGIN RE-BASE (a region crossing; before prepare()): LL's agent frame moved by `off`
    // (new agent = old agent + off). Every map, grid and ring is stored in agent space by its
    // frame (corner, top plane, shear) -- the frames move by `off`, the images stay as they are,
    // so the snow, its footprints / tracks / drifts, the outer ring and the roof maps lie on in
    // place (the snow kernels follow each texel back into the previous grid by its world point).
    // A vertical shift (never at a crossing: regions share z) falls back to resetGround().
    void shiftOrigin(const float off[3]);
    // #895 NEW PLACE (a teleport; before prepare()): the ground's persistent state -- the snow
    // grid and outer ring (their depths, footprints, tracks, drifts), the run-off flow and drips,
    // the precipitation particles -- starts over at the destination (every texel enters at the
    // global depth: SLWeather seeds it); the roof maps re-render. The module is not freed here, so
    // the avatars' snow / wetness and the #901 shelter atlas carry on while it lives -- it still
    // frees as usual once nothing is visible and no avatar is wet (a teleport out of region weather
    // zeroes the global amounts first; the per-avatar wetness outlives the module on the CPU side,
    // the avatar snow is capped by the global cover).
    void resetGround(const char* why);
    // M2 LIGHTNING: the current flash's fill radiance (linear scene units, cool white, scaled by
    // the flash and the strike's distance) and the unit direction to the strike, from the
    // weather state (RenderVKWeatherLightning). false = no flash now (outputs untouched). Shared
    // with the volumetric clouds' composite (it lights its clouds with it).
    static bool flashNow(float radiance[3], float dir[3]);
    // ssr.weather (zeros when not running).
    static constexpr int kInfoRows = 17;  // ssr.weather[17] (clean_weather_surface.glsl has the contract; M4 [13] / [14]; #954 [15] / [16] the precipitation medium)
    void fillFrameInfo(float out[kInfoRows][4]) const;

    // ---- occlusion maps (rendered by LLCleanRendererVK::recordWeatherOcclusion) ----
    // Two cascades (OCCL_COARSE, OCCL_NEAR), each DOUBLE-BUFFERED: a render goes into the
    // cascade's map NOT published this recording and is published by the next prepare(), so
    // every reader of a recording -- whether it runs before the render (an early G-buffer,
    // probe faces) or after it -- sees the map its frame info describes. At most one cascade
    // renders per recording; the accessors below describe that pending render. All maps sit
    // in DEPTH_READ_ONLY_OPTIMAL between renders.
    enum : uint32_t { OCCL_COARSE = 0, OCCL_NEAR = 1, OCCL_COUNT = 2 };
    bool occlusionDue() const
    {
        return mRunning && mPendMap >= 0 && mMap[mPendMap].img[mMap[mPendMap].target].image;
    }
    VkImage  occlusionImage() const { return pend().img[pend().target].image; }   // this recording's target
    VkImageView occlusionAttachView() const { return pend().img[pend().target].view; }
    uint32_t occlusionRes() const { return pend().res; }
    // The pending render's WORLD view-projection (column-major, VK clip, conventional depth:
    // 0 = the map's top) and its six cull planes (ax + by + cz + d >= 0 inside, world).
    const float* occlusionViewProj() const { return pend().pendVP; }
    // The sub-texel caster cull's apex (world) and its k (texel per unit distance): far above
    // the box, so k x distance ~ one texel anywhere inside it.
    // M2 TILTED MAP: the apex sits up the precipitation direction (-d) from the box's top
    // centre, so it stays "far above" along the map's own projection axis.
    void occlusionApex(float apex[3], float& k) const
    {
        const OcclMap& m = pend();
        const float sx = m.pend[6], sy = m.pend[7];
        apex[0] = m.pend[0] + 0.5f * m.pend[2] - sx * kApexHeight;
        apex[1] = m.pend[1] + 0.5f * m.pend[2] - sy * kApexHeight;
        apex[2] = m.pend[3] + kApexHeight;
        k = m.pend[5] / (kApexHeight * std::sqrt(1.f + sx * sx + sy * sy));
    }
    static constexpr float kApexHeight = 1.0e4f;
    void occlusionPlanes(float out[6][4]) const
    {
        for (int i = 0; i < 6; ++i)
            for (int k = 0; k < 4; ++k) out[i][k] = pend().pendPlanes[i][k];
    }
    // The renderer recorded the pending map this recording: published by the next prepare().
    void occlusionRendered();

    // The precipitation runs this recording (pipelines built, not under water).
    bool precipNow() const { return mRunning && mPrecip; }
    // Simulation + compaction (outside any rendering scope, after the occlusion map).
    // viewProjRel / eye: the main view's JITTERED camera-relative view-projection and its
    // origin (the pass-2 draw view), so the streaks sit exactly on the scene depth.
    void recordSim(VkCommandBuffer cmd, const float* viewProjRel, const float* eye);
    // Inside render pass 2b (HDR + glow + reactive + read-only depth; viewport / scissor set).
    void recordDraw(VkCommandBuffer cmd);
    // #954 PRECIPITATION VEIL, the FOG-OFF fallback (clean_weather_veil.frag): the precipitation's
    // extinction over the opaque scene + sky, noise-free, only on surfaces open to the sky; one
    // fullscreen draw at the start of render pass 2 (the transparents veil themselves). With the
    // volumetric fog running the froxels carry the precipitation instead and this is not drawn.
    // dsetDraw0: this recording's draw set 0 (binding 40 the opaque depth, 49 the roof maps).
    // Rebinds set 0 (the caller rebinds its draw sets after). veilNow(): the simulation ran this
    // recording and the fallback is on.
    bool veilNow() const { return mRunning && mSimRecorded && mVeilPipe != VK_NULL_HANDLE && mVeilPass; }
    void recordVeil(VkCommandBuffer cmd, VkDescriptorSet dsetDraw0);

    // ---- M2 PERSISTENT SNOW (clean_weather_snow.comp) ----
    // While it snows or snow lies (and RenderVKWeatherSnowCover / SnowMovers): the snow-cover
    // grid on the near cascade's top plane (2 x rgba16f, ping-pong: the compute writes the one
    // not published, the next prepare() publishes it -- every reader of a recording sees the
    // grid its frame info describes) and the moving objects' own amounts (r32f, one texel per
    // instance slot). Nothing exists without snow.
    bool snowDue() const { return mRunning && (mCoverDue || mInstDue || mAvDue); }
    // After recordSim (outside any rendering scope): the cover grid's integration and the
    // moving objects' snow. dsetDraw0: this recording's draw set 0.
    void recordSnow(VkCommandBuffer cmd, VkDescriptorSet dsetDraw0);

    // ---- M3 SNOW SURFACE MESH (clean_weather_snowmesh.*; RenderVKWeatherSnowMesh) ----
    // Built by prepare() from the PUBLISHED grid (before the pre-pass, the cascades and the
    // G-buffer of this recording), drawn with one indexed indirect draw per pass. The caller
    // has bound draw sets 0 / 1 with the world draw layout and pushed the pass's draw view.
    // shadow: the sun cascades' depth-only pipeline (RenderVKWeatherSnowMeshShadows), else
    // the deferred G-buffer one (depth-tested and WRITTEN: the mesh is not in the pre-pass).
    // (#901: + the avatar snow mesh, built by the shelter chain mid-recording -- before every pass that draws it)
    // #946 SNOW MESH BUILD (planned by prepare()): true = this recording rebuilds the mesh; the
    // renderer records it with recordMeshBuild right after its frame timers began (before any pass
    // that draws it), so its GPU time shows as the c_snow_mesh_build sub-mark. A recording that does
    // not rebuild draws the last build (world-space vertices: valid until the grid snaps, the
    // camera moves / turns past the thresholds, or RenderVKWeatherSnowMeshInterval recordings pass).
    bool meshBuildDue() const { return mMeshBuildDue; }
    void recordMeshBuild(VkCommandBuffer cmd);
    // #946: the sun cascades that draw the mesh (0 .. this - 1; RenderVKWeatherSnowMeshShadowCascades,
    // the SMALL tier: the nearest only). The far cascades' texels are coarser than the snow's relief.
    uint32_t meshShadowCascades() const;
    bool meshReady(bool shadow) const { return mRunning && (mMeshBuilt || mShMeshBuilt) && (shadow ? mMeshShadowPipe != VK_NULL_HANDLE && mMeshShadowOn : mMeshGbufPipe != VK_NULL_HANDLE); }
    void recordMeshDraw(VkCommandBuffer cmd, bool shadow) const;
    // FORWARD frames (no G-buffer: a supersampled snapshot with RT shadows, RenderVKSnapshot-
    // Deferred off, debug views): the forward lit pass 1's draw of the same mesh -- rt: the
    // ray-query lit FS variant (the frame traces without an RT mask). The caller bound the draw
    // sets (bindDrawSets: + the RT set in a ray-query frame) and pushed the main view.
    bool meshFwdReady(bool rt) const { return mRunning && (mMeshBuilt || mShMeshBuilt) && mMeshFwdPipe[rt ? 1 : 0] != VK_NULL_HANDLE; }
    void recordMeshDrawFwd(VkCommandBuffer cmd, bool rt) const;
    // DEPTH PRE-PASS (RenderVKWeatherSnowMeshPrepass; the GI "ghost avatar silhouettes on snow"
    // fix): the same mesh into pass 1a with the variant of the attachment set in force (variant:
    // Frame::preDesc's index), so the pre-pass depth / motion / normals are the snow's, not the
    // buried surface's. The caller bound the draw sets and pushed the main view (the G-buffer /
    // forward draw's own view: the same VS, so the later LEQUAL draw keeps every fragment).
    bool meshPreReady(uint32_t variant) const
    {
        return mRunning && (mMeshBuilt || mShMeshBuilt) && variant < 4u && mMeshPrePipe[variant] != VK_NULL_HANDLE;
    }
    void recordMeshDrawPre(VkCommandBuffer cmd, uint32_t variant) const;

    bool running() const { return mRunning; }
    // ---- #901 PER-AVATAR SHELTER (llcleanweathervk_shelter.cpp) ----
    // Right after the main cull's candidates and the pre-skin are complete (before the HUD /
    // offscreen culls reuse them), before the G-buffer: the chain (header). dsetDraw0: this
    // recording's draw set 0. Nothing when not due.
    bool shelterDue() const { return mRunning && mShDue; }
    void recordShelter(VkCommandBuffer cmd, VkDescriptorSet dsetDraw0, const ShelterInputs& in);
    struct ShelterStats
    {
        uint32_t config = 3;                          // 0 full, 1 all 128^2, 2 eight 128^2, 3 none
        uint32_t layers = 0, layerCap = 0, layerPeak = 0;   // avatars with a layer (CPU, this recording)
        uint32_t due = 0;                             // #901 layers re-rastered this recording (covered / nothing falling: fewer)
        uint32_t items = 0, itemCap = 0, itemPeak = 0, itemDropped = 0;   // the raster's work list (readback)
        uint64_t overflowSteps = 0;
        uint64_t bytes = 0, peakBytes = 0;            // VRAM of the atlas + record
    };
    static ShelterStats shelterStats() { return sShelterStats; }
    // ---- #901 REST-POSE ACCUMULATION (llcleanweathervk_accum.cpp, clean_weather_accum.*) ----
    // RenderVKWeatherAvatarAccum: the avatars with a shelter layer get a GRID over their rest pose
    // (a fixed pool, RenderVKWeatherAvatarAccumCount; freed RenderVKWeatherAvatarAccumGrace s after
    // the avatar has no layer) that their snow and wetness accumulate in, per cell, only while
    // exposed (the shelter maps + the roof maps at the vertex's CURRENT pose); the lit / G-buffer FS
    // shows the cell of the fragment's rest position. Recorded right after recordShelter (its work
    // list and atlas), when due: the grids due this recording (at most kAcSlots, each at
    // RenderVKWeatherAvatarAccumRate, staggered) and the directory. Draw set 0 binding 50 / 51.
    bool accumDue() const { return mRunning && mAcDue; }
    void recordAccum(VkCommandBuffer cmd, VkDescriptorSet dsetDraw0, const ShelterInputs& in);
    VkBuffer    accumRecord() const { return mAcRec.buffer; }
    VkImageView accumView() const { return mAcImg.view; }
    static constexpr uint32_t kAcRecordBytes = 16544;   // clean_weather_accum.glsl WxAccum
    struct AccumStats
    {
        uint32_t grids = 0, gridCap = 0, gridPeak = 0;  // avatars with a grid (CPU)
        uint32_t res[3] = {0, 0, 0};
        uint64_t bytes = 0, peakBytes = 0;              // VRAM (grids + record + scratch)
        uint64_t updates = 0;                           // grid updates recorded (cumulative)
        uint32_t failures = 0;                          // refused allocations
    };
    static AccumStats accumStats() { return sAccumStats; }
    // #901 / #904: weather master off -- the own avatar's grid snapshot (it outlives the module) goes.
    static void clearAccumSnapshot();
    // #895: the own avatar still carries snow (the last readback): the module stays / comes back so
    // it melts on its own clock (with avatarsWetHold()).
    static bool avatarSnowHold();
    // #901 PER-AVATAR WETNESS: true while an avatar the shelter chain tracked is still wet (the
    // CPU's estimate from the last readbacks): the module stays (or comes back, after a teleport's
    // clean slate) so it dries on its own clock even where no weather is.
    static bool avatarsWetHold();
    // Draw set 0 binding 49 [0..9]: [0] / [1] the coarse maps, [2] / [3] the near maps
    // (DEPTH_READ_ONLY), M2 [4] / [5] the snow-cover grids, [6] the per-instance snow, M4 [7]
    // the run-off flow map, #901 [8] / [9] the shelter atlas (GENERAL), while they exist (else
    // null: the renderer's stand-in).
    static constexpr uint32_t kBindingSlots = 10;
    VkImageView bindingView(uint32_t i, VkImageLayout& layout) const
    {
        layout = i < 4 ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
        if (i < 4) return mMap[(i >> 1) & 1u].img[i & 1u].view;
        if (i < 6) return mCover[i - 4].view;
        if (i == 6) return mInstSnow.view;
        if (i == 7) return mFlowImg.view;
        return i < 10 ? mShS[i - 8].view : VK_NULL_HANDLE;
    }
    // #930 ACOUSTICS (vkgfx/llcleanaudiortvk.cpp, read only): this recording's PUBLISHED snow
    // depth grids -- the inner cover grid (rgba32f, .a depth) + its filtered base (rg32f, .x m
    // below the frame's top plane), and the outer ring (rgba32f, .x base, .y depth) when one is
    // published -- with their frames {x0, y0, extent, top plane, -, texel, shear x, shear y} and
    // the inner grid's height tolerance. GENERAL layout, storage images; recordSnow writes the
    // OTHER image of each pair and ends with a compute -> compute read barrier. false: no grid.
    struct AcousticsSnow
    {
        VkImageView cover = VK_NULL_HANDLE, base = VK_NULL_HANDLE, ring = VK_NULL_HANDLE;
        float    coverF[8] = {}, ringF[8] = {};
        uint32_t coverRes = 0, ringRes = 0;
        float    tol = 0.f;
    };
    bool acousticsSnow(AcousticsSnow& out) const
    {
        out = AcousticsSnow{};
        if (!mRunning || mCoverPub < 0 || mCoverPub > 1 || !mCover[mCoverPub].view || !mSnowBase[mCoverPub].view || !mCoverRes)
            return false;
        out.cover = mCover[mCoverPub].view;
        out.base = mSnowBase[mCoverPub].view;
        for (int i = 0; i < 8; ++i) out.coverF[i] = mCoverPubF[i];
        out.coverRes = mCoverRes;
        out.tol = mSnowPush[4];
        if (mRingPub >= 0 && mRingPub <= 1 && mRing[mRingPub].view && mRingRes)
        {
            out.ring = mRing[mRingPub].view;
            for (int i = 0; i < 8; ++i) out.ringF[i] = mRingPubF[i];
            out.ringRes = mRingRes;
        }
        return true;
    }
    // true once after the maps were (re)created or freed: the renderer rewrites binding 49
    // (the device is idle then -- prepare() waited).
    bool takeBindingDirty() { const bool d = mBindingDirty; mBindingDirty = false; return d; }
    // Nothing allocated (the renderer deletes the object then).
    bool idle() const
    {
        return !mMap[OCCL_COARSE].img[0].image && !mMap[OCCL_NEAR].img[0].image && !mParticles.buffer && !mSimPipe
            && !mDsl && !mJob.pending() && !mCover[0].image && !mInstSnow.image && !mSnowLayout && !mMeshLayout
            && !mMeshVerts.buffer && !mAvState.buffer && !mMovers.buffer && !mRunoffScratch.buffer && !mRunoffLayout
            && !mShLayout && !mShBuf.buffer && !mShH.image && !mAcRec.buffer && !mAcImg.image;
    }
    // Frees everything (waits for the device). Safe to call repeatedly.
    void destroy(VkDevice dev);

    static constexpr uint32_t kOcclRes = 256;   // the coarse cascade

    // The parameter block (clean_weather_common.glsl WeatherParams, std140).
    struct Params
    {
        float view_proj[16];
        float eye[4], box[4], wind[4], amounts[4], occl[4], occl2[4];
        float sun[4], amb[4], screen[4], shape[4], misc[4], ear[4];
        float tilt[4];   // M2: xy the coarse map's shear, zw the near map's (clean_weather_common.glsl)
        float splash[4]; // M2 splashes: on, ring size, lifetime (s), spawn radius (m)
        float occlN[4], occlN2[4];   // M2: the near map's frame (splash heights)
        float runoff[4], runoff2[4]; // M4: strength, drips / s / m2, stream threshold, drip budget; drips, first drip slot, streams, emitter cap
        float shelter[4];            // #901: layers in use (0 none), the atlas image written this recording, unused x2
        // #954 PRECIPITATION RATE: rain mm/h, snow cm/h, blowing snow's slot share, its layer height (m);
        // drop size scale (mm), flake size factor, snow exposure (s), snow swirl; the veil (rgb, 1/m);
        // its ceiling (m), curtain strength, octaves, snow share; the inverse of view_proj.
        float fall[4], shape2[4], veil[4], veil2[4];
        float inv_view_proj[16];
    };
    static_assert(sizeof(Params) == 496, "Params = clean_weather_common.glsl WeatherParams");

    // ---- M4 RUN-OFF stats (live / cap / peak; static: the module is deleted when idle) ----
    struct RunoffStats
    {
        uint32_t flowRes = 0;                         // the flow grid (0: none allocated)
        uint32_t emitters = 0, emitterCap = 0, emitterPeak = 0;   // the emitter list (GPU readback, a frame or two late)
        uint32_t emitterOverflow = 0;                 // readbacks whose outlets passed the cap (the extra ones dropped)
        uint32_t drips = 0, dripCap = 0, dripPeak = 0;   // drips spawned in a frame (readback), the drip slots
        uint64_t bytes = 0, peakBytes = 0;            // VRAM held by the run-off (scratch + flow map + list)
        uint64_t builds = 0;                          // flow builds since start
    };
    static RunoffStats runoffStats() { return sRunoffStats; }

    // ---- M5 MOVER TRACKS stats (live / cap / peak; static: the module is deleted when idle) ----
    struct MoverStats
    {
        uint32_t tracked = 0;                         // the tracker's recently moved casters within reach (CPU, this frame)
        uint32_t stamps = 0, cap = 0, peak = 0;       // boxes stamped in a step (GPU readback, a frame or two late), the cap
        uint32_t dropped = 0;                         // boxes past the cap in that step
        uint64_t overflowSteps = 0;                   // readbacks that dropped boxes
        // CONSERVATION (that step): the snow the presses removed and the berms deposited (m3; the
        // berms take at most the berm share of it), the texels pressed / deposited on.
        float    removedM3 = 0.f, depositedM3 = 0.f;
        uint32_t pressedTexels = 0, bermTexels = 0;
        double   removedTotalM3 = 0.0, depositedTotalM3 = 0.0;   // since the start
        uint64_t bytes = 0;                           // VRAM of the list (0: not allocated)
        // LIMB CONTACT (#783 follow-up): the limb boxes the avatar pass appended in a step (GPU
        // readback), their slots (kLimbStampMax), the most seen, the boxes past the slots that step
        // and the steps that dropped some.
        uint32_t limbs = 0, limbCap = 0, limbPeak = 0, limbDropped = 0;
        uint64_t limbOverflowSteps = 0;
        // #783 BODY CONTACT (that step): avatars pressed by their bottom map, their supporting /
        // other contacts and footprints; avatars on the joint fallback, their footprints, arms that
        // stood as legs; the most contacts on one avatar.
        uint32_t bodyAvatars = 0, bodySupport = 0, bodyOther = 0, bodyStamps = 0;
        uint32_t jointAvatars = 0, jointStamps = 0, armLegs = 0, bodyMaxContacts = 0;
    };
    static MoverStats moverStats() { return sMoverStats; }
    static constexpr uint32_t kMoverStampMax = 512;   // the movers' slots (RenderVKWeatherSnowMoverStamps clamps to it)
    static constexpr uint32_t kLimbStampMax = 256;    // the limbs' slots after them (clean_weather_snow.comp kMvLimbMax)
    static constexpr uint32_t kSplashRing = 2048;   // M2: splash slots (16 B each)

    // ---- #946 SYNC / VRAM DIAGNOSTICS (static: the module is deleted when idle) ----
    // Every device wait the weather causes (its own re-creations and the renderer's binding
    // 49 / 50 / 51 rewrites), timed; the re-creations; the frames that waited. Logged every
    // ~10 s while the weather runs (the "weather sync" line) with the module's VRAM by part, and
    // a warning at once when waits repeat frame after frame (a re-creation loop).
    struct SyncStats
    {
        uint64_t waits = 0, recreates = 0, bindingRewrites = 0;   // cumulative
        double   waitMs = 0.0, worstWaitMs = 0.0;                 // cumulative / the worst single wait
        uint64_t framesWithWait = 0, frames = 0;                  // live recordings
        uint32_t streak = 0, worstStreak = 0;                     // consecutive live recordings with a wait
        const char* lastWhy = "";                                 // the last wait's reason
        uint64_t meshBuilds = 0, meshSkips = 0;                   // #946 snow mesh: rebuilt / reused (live recordings)
        uint64_t meshPatches = 0;                                 // #946 ... of the reused: local patches recorded
    };
    static SyncStats syncStats() { return sSync; }
    // The renderer's waits for the weather (binding 49 / 50 / 51 rewrites): counted with the module's.
    static void noteRendererWait(double ms, const char* why);
    // The VRAM tier (#946): true = the SMALL tier (a card whose budget capacity is under
    // kSmallVramCapacity); the effective resolutions / counts follow it.
    static bool smallVram() { return sSmallVram; }
    static constexpr uint64_t kSmallVramCapacity = 4608ull << 20;   // 4.5 GB: every 4 GB card (and 3 GB ones)

private:
    // #946: vkDeviceWaitIdle, timed and counted (sSync); `why` names the caller in the log.
    static void wxWaitIdle(VkDevice dev, const char* why);
    // #946: the VRAM tier for this recording (hysteresis: small at once, back after kTierCalm s).
    void updateVramTier(const Frame& f);
    // #946: the module's live VRAM (device-local bytes), `parts` a per-part breakdown for the log.
    uint64_t vramBytes(std::string* parts) const;
    // #946: the sync / VRAM line (every ~10 s) and the re-creation-loop warning.
    void logSync(const Frame& f, bool waitedThisFrame);
    // #946: the snow mesh's triangle cap for the current grid / ring resolution.
    uint32_t meshTriCap() const;
    bool ensurePipelines(const Frame& f);
    bool ensureBuffers(VkCommandBuffer cmd, uint32_t cap);
    void destroyBuffers();
    void writeSets();
    void planOcclusion(const Frame& f, float radius, const float tilt[2]);
    bool ensureNear(VkCommandBuffer cmd);
    void destroyNear();
    // Clears freshly created maps to open sky and leaves them in their sampled layout.
    static void initMaps(VkCommandBuffer cmd, GpuImage* maps, uint32_t n);
    // M2 snow: the grids / per-instance image while wanted (device wait on a change; binding 49
    // then dirty), freed otherwise. A refused allocation skips the feature and retries later.
    void ensureSnow(VkCommandBuffer cmd, bool want, uint32_t instCount);
    void destroySnow();
    void planSnow(VkCommandBuffer cmd, const Frame& f);
    // M3: the avatars' CPU bits (alive / sitting per joint-table slot) for this recording.
    void gatherAvatarBits();
    // M3 snow mesh: its buffers / set while wanted (device wait on a change), and this
    // recording's build (prepare(), after planSnow published the grid).
    bool ensureMesh(VkCommandBuffer cmd, bool want);
    void destroyMesh();
    // #946: prepare()'s half -- the buffers, whether this recording rebuilds (planMeshBuild), the
    // push block; the dispatches go to recordMeshBuild(cmd), which the renderer records after its
    // frame timers began (the c_snow_mesh_build sub-mark).
    void planMeshBuild(VkCommandBuffer cmd, const Frame& f);
    // #946 LOCAL PATCH: the tiles around this recording's local edits (the published grid's marks),
    // on a recording that does not rebuild (recordMeshBuild records it: c_snow_mesh_build).
    void recordMeshPatch(VkCommandBuffer cmd);
    // M3 outer ring: its settings and this recording's step (planSnow).
    static float    groundUnderCamera(const Frame& f);
    static bool     ringWanted();
    static float    ringExtent();
    static uint32_t ringResolution();
    void planRing(const Frame& f, float tol, float depthBuild, float depthMelt, uint32_t feat);
    void writeSnowSets();
    // M5 mover tracks: whether this step stamps (the tracker has a recently moved caster within
    // reach), its list parameters, the readback of a frame slot's last list head. limbs: the avatar
    // pass appends limb boxes this step (#783 follow-up) -- the list's head / parameters are written
    // for either; sets mMoverDue / mLimbDue.
    void planMovers(const Frame& f, bool movers, bool limbs);
    void readMoverHead(const uint32_t* head);
    static void registerMoverProbe();
    static MoverStats sMoverStats;
    // M4 run-off (llcleanweathervk_runoff.cpp): its buffers / set while wanted (device wait on a
    // change; binding 49 then dirty; a refused allocation retries later), this recording's flow
    // build (prepare(), after the near map's publish) and its frame-info rows / parameters.
    bool ensureRunoff(VkCommandBuffer cmd, bool want, uint32_t res, uint32_t emitCap);
    void destroyRunoff();
    void writeRunoffSets();
    void planRunoff(VkCommandBuffer cmd, const Frame& f, uint32_t quality);
    void recordRunoffBuild(VkCommandBuffer cmd);
    void recordRunoffSpawn(VkCommandBuffer cmd);
    void readRunoffWords(const uint32_t* words);
    static void registerRunoffProbe();
    static RunoffStats sRunoffStats;
    // #901 per-avatar shelter (llcleanweathervk_shelter.cpp): its layouts / pipelines (with the
    // first pipeline batch), this recording's plan (prepare(): the layers, the due ones, the
    // header; the atlas allocated / re-tiered / freed behind a device wait -- binding 49 then
    // dirty), its set per frame slot (the renderer's buffers written at record time), the stats.
    void createShelterLayouts(VkDevice dev, VkDescriptorSetLayout dslDraw0);
    void buildShelterPipelines(VkDevice dev);
    void destroyShelterPipelines(VkDevice dev);
    bool planShelter(VkCommandBuffer cmd, const Frame& f);
    bool ensureShelter(VkCommandBuffer cmd, uint32_t config);
    void destroyShelter();
    void writeShelterSets(uint32_t slot, const ShelterInputs* in);
    void readShelterHead(const uint32_t* head);
    // #901 / #909 follow-up: the CLUSTER LOD kernels (clean_weather_cluster.glsl: the shelter
    // chain's filter / raster and the accumulation over the skinned arena). Their layout {draw set
    // 0, shelter set, the cluster set} is made once the cluster module exists (its set layout;
    // rebuilt when a new module brings a new one), the three pipelines in their own background
    // batch. true = ready for in (bind with bindShelterSets). Until then -- or without cluster
    // LOD -- the chain runs as before (the cluster-drawn parts then miss the maps / grids).
    bool ensureShelterCluster(const ShelterInputs& in);
    // Binds draw set 0 + this slot's shelter set (+ with cl the cluster set: the cluster layout,
    // compatible for sets 0 / 1 and the push range) and pushes p.
    void bindShelterSets(VkCommandBuffer cmd, VkDescriptorSet dsetDraw0, const ShelterInputs& in, bool cl,
                         const void* push, uint32_t pushBytes);
    VkDescriptorSetLayout mShDslDraw0 = VK_NULL_HANDLE;     // (the shelter layout's set 0: the cluster layout's too)
    VkDescriptorSetLayout mShClDslKey = VK_NULL_HANDLE;     // the cluster set layout mShClLayout was made with
    VkPipelineLayout      mShClLayout = VK_NULL_HANDLE;
    static constexpr uint32_t kShClKernels = 3;             // shelter filter, shelter raster, accumulation
    VkPipeline            mShClPipe[kShClKernels] = {};
    VkPipeline            mShClStage[kShClKernels] = {};    // (the batch's outputs; moved to mShClPipe on landing)
    PipelineJobSlot       mShClJob;
    bool                  mShClFailed = false;
    float     mShSnowPush[12] = {};                    // this recording's snow parameters (ShPush d, e, f)
    float     mShWetPush[8] = {};                      // #901 per-avatar wetness parameters (ShPush g, h)
    uint32_t  mShResets[20] = {};                      // #901 this recording's wetness resets (record rhead + rst[8])
    float     mShExpTop[16] = {};                      // #901 per layer: the top exposure last read back (< 0 unknown)
    double    mShLastDue[16] = {};                     // #901 per layer: when it was last re-rastered (mNow)
    uint32_t  mShStatPrev[3] = {};                     // #901 the cumulative counters at the last log
    bool      mShStatHave = false;
    uint32_t  mShFlags = 0;                            // this recording's snow / mesh / motion flags
    uint32_t  mShSelfSlot = 0xFFFFFFFFu;               // the own avatar's joint-table slot (the log)
    static void registerShelterProbe();
    static ShelterStats sShelterStats;
    // #946 diagnostics / VRAM tier
    static SyncStats sSync;
    static bool      sSmallVram;
    double    mTierCalmSince = -1.0;               // the last recording that wanted the small tier
    uint64_t  mSyncWaitsAtFrame = sSync.waits;     // sSync.waits at the start of this recording
    double    mSyncNextLog = 0.0;
    double    mSyncLoopWarnAt = 0.0;
    SyncStats mSyncPrev = sSync;                   // the counters at the last log (the window's deltas)

    VkDeviceContext*      mCtx = nullptr;
    // Own set (clean_weather_common.glsl): 0 params (UBO), 1 particles, 2 visible list,
    // 3 indirect args, 4 the coarse occlusion maps, 5 the listener-cover readback. One per
    // frame slot (the parameter / readback ring).
    VkDescriptorSetLayout mDsl = VK_NULL_HANDLE;
    VkDescPool            mPool;
    VkDescriptorSet       mDset[2] = {};
    VkPipelineLayout      mLayout = VK_NULL_HANDLE;   // {own}, no push constants
    VkPipeline            mSimPipe = VK_NULL_HANDLE, mDrawPipe = VK_NULL_HANDLE;
    PipelineJobSlot       mJob;
    // sim, draw, M2 snow cover, M2 snow instances, M2 splash draw, M3 avatars, M3 stamps,
    // M3 mesh build, M3 mesh G-buffer draw, M3 mesh shadow draw, M3 outer ring, M4 run-off
    // flow build, M4 drip spawn, M4 stream draw, M3 mesh forward draw (plain, ray-query)
    // (+ M5 the mover berm gather, [16]; #901 the shelter chain [17..])
    static constexpr uint32_t kShStage0 = 17, kShKernels = 6;   // prep, filter, clear, raster, publish (+ avatar snow), avatar snow mesh
    static constexpr uint32_t kAcStage0 = kShStage0 + kShKernels, kAcKernels = 4;   // #901 accumulation: prep, accum, stage, resolve
    static constexpr uint32_t kPreStage0 = kAcStage0 + kAcKernels;   // the mesh's 4 depth pre-pass draws
    static constexpr uint32_t kVeilStage = kPreStage0 + 4u;           // #954 the precipitation veil
    static constexpr uint32_t kStages = kVeilStage + 1u;
    VkPipeline            mStage[kStages] = {};
    VkPipeline            mSplashPipe = VK_NULL_HANDLE;
    // #954 PRECIPITATION VEIL: {draw set 0, own} (no push), first use only; its fullscreen pipeline.
    VkPipelineLayout      mVeilLayout = VK_NULL_HANDLE;
    VkPipeline            mVeilPipe = VK_NULL_HANDLE;
    bool                  mVeilPass = false;   // this recording draws the fog-off fallback
    bool                  mPipeFailed = false;
    VkFormat              mPipeFormats[4] = {};
    bool                  mPipeReverseZ = true;
    VkSampler             mSampler = VK_NULL_HANDLE;   // nearest, open-sky (depth 1) border

    GpuBuffer mParticles, mVisible, mArgs;
    GpuBuffer mSplashes, mSplashHead;   // M2: the splash ring (kSplashRing x 16 B) and its head
    GpuBuffer mParams[2];   // host-visible, per frame slot
    // LISTENER COVER READBACK (weather audio): the simulation writes the roof map's openness at
    // the audio listener into its slot's word; the next recording on that slot (its fence
    // waited) hands it to SLWeather. mListenWritten: the slot's last recording ran the
    // simulation (else its word is stale).
    GpuBuffer mListen[2];
    bool      mListenWritten[2] = {};
    uint32_t  mCap = 0;
    bool      mBindingDirty = false;
    bool      mReset = true;

    // Latched per recording.
    bool      mRunning = false;
    bool      mPrecip = false;
    bool      mSimRecorded = false;   // recordSim ran this recording (recordDraw may draw)
    uint32_t  mSlot = 0;
    Params    mP{};
    float     mWx[kInfoRows][4] = {};   // ssr.weather
    double    mLastTime = -1.0;
    double    mNow = 0.0;              // this recording's time (s)
    double    mRetryAt = 0.0;          // a refused allocation retries from here
    double    mLastSplashOn = -1e9;    // M2: the last recording that spawned splashes
    uint32_t  mFrame = 0;

    // One occlusion cascade: its two maps (D32 top-down depth), the pending render (this
    // recording, into `target`), the one rendered last recording (published by this prepare)
    // and the published one (frame info, simulation). Frames: x0, y0, extent, top, span, texel,
    // shear sx, sy (M2 tilted map: a point P lies on map column P.xy - (top - P.z) * s).
    struct OcclMap
    {
        GpuImage  img[2];
        uint32_t  res = 0;
        uint32_t  target = 0;          // this recording's render target (never the published map)
        int       pubIdx = -1;         // the published map (-1: none yet)
        int       readyIdx = -1;       // rendered, waiting for the next prepare
        uint32_t  age = 0;             // recordings since the last render
        bool      forced = false;      // a camera cut asked for a render (kept until it happens)
        double    cell[3] = {1e30, 1e30, 1e30};   // the snapped centre the last render used
        double    pendCell[3] = {};                // ... and the pending one
        float     cellTilt[2] = {0.f, 0.f};        // the shear the last render used
        bool      haveTilt = false;
        float     pendVP[16] = {};
        float     pendPlanes[6][4] = {};
        float     pend[8] = {};
        float     ready[8] = {};
        float     pub[8] = {};
        void resetSchedule()
        {
            pubIdx = readyIdx = -1;
            target = 0;
            forced = false;
            haveTilt = false;
            for (double& c : cell) c = 1e30;
        }
    };
    const OcclMap& pend() const { return mMap[mPendMap >= 0 ? mPendMap : 0]; }
    bool      mOcclOn = true;
    int       mPendMap = -1;           // the cascade rendering this recording (-1: none)
    uint32_t  mOcclTurn = 0;           // both due: alternates
    float     mTilt[2] = {0.f, 0.f};   // M2: the precipitation's shear this recording (kept while none falls)
    OcclMap   mMap[OCCL_COUNT];
    double    mNearRetryAt = 0.0;      // a refused near-map allocation retries from here

    // ---- M2 persistent snow ----
    // Layout {draw set 0, own}: own 0 the two cover grids (storage), 1 the per-instance image,
    // M3: 2 the avatars' CPU bits, 3 their motion state, 4 the footprint stamps.
    VkDescriptorSetLayout mSnowDsl = VK_NULL_HANDLE;
    VkDescPool            mSnowPool;
    VkDescriptorSet       mSnowSet = VK_NULL_HANDLE;
    VkPipelineLayout      mSnowLayout = VK_NULL_HANDLE;
    VkPipeline            mCoverPipe = VK_NULL_HANDLE, mInstPipe = VK_NULL_HANDLE;
    VkPipeline            mAvPipe = VK_NULL_HANDLE, mStampPipe = VK_NULL_HANDLE;   // M3
    VkPipeline            mBermPipe = VK_NULL_HANDLE;   // M5: the mover berm gather (-DWX_SNOW_BERM)
    GpuImage  mCover[2];                   // rgba32f (M3: + the depth; fp32 heights for the mesh)
    GpuImage  mSnowBase[2];                // M3 rg32f: the filtered snow base + the support cap, paired with mCover
    // M3 OUTER RING (RenderVKWeatherSnowOuterRing): rgba32f ping-pong (z the support cap; x the filtered base, m below
    // its top plane, 1e30 none; y the depth) on the coarse map's plane, its schedule as the
    // grid's (published / written last recording / this recording's frame: x0, y0, extent,
    // top, span, texel, shear x, y), the time since its last step, its push block.
    GpuImage  mRing[2];
    uint32_t  mRingRes = 0;
    int       mRingPub = -1, mRingReady = -1;
    uint32_t  mRingTarget = 0, mRingAge = 0;
    float     mRingPubF[8] = {}, mRingReadyF[8] = {}, mRingPendF[8] = {};
    bool      mRingDue = false, mRingFresh = true;
    float     mRingDt = 0.f;
    float     mRingPush[32] = {};
    VkPipeline mRingPipe = VK_NULL_HANDLE;
    uint32_t  mCoverRes = 0;
    GpuImage  mInstSnow;                   // rgba32f: the instance rows, then M3 two avatar rows
    uint32_t  mInstRows = 0;               // the per-instance image's INSTANCE rows (kInstSnowWidth wide)
    // M3 avatars: the CPU bits (2 x 32 words: alive, sitting), the per-slot motion state
    // (1024 x 32 B) and the stamp list (16 B head + kMaxStamps x 32 B; also the stamp kernel's
    // indirect dispatch). Allocated with the grids while RenderVKWeatherSnowAvatars or
    // RenderVKWeatherSnowFootprints is on.
    GpuBuffer mAvBits, mAvState, mStamps;
    GpuImage  mBodyStandIn;                // #783: 1 x 1 r32ui, the snow set's binding 8 without the shelter atlas
    bool      mBodyDue = false;            // #783 BODY CONTACT: the avatar pass carries the layers' workgroups
    // #946 LOCAL PATCHES (clean_weather_snowmesh.comp): the snow mesh's local-edit marks, allocated
    // with the grids (their resolution): an 8-word head (the patch's two indirect dispatches, the
    // overflow flag, the list cap), one tile mask per grid (set by the cover / berm / stamp passes
    // writing that grid; cleared by recordSnow before the cover pass) and the two tile lists the
    // patch's expand step writes. The snow set's binding 9 and the mesh set's binding 6.
    GpuBuffer mMeshMarks;
    static constexpr uint32_t kMkHead = 8u;    // (clean_weather_snowmesh.comp kMkHead)
    static uint32_t meshTiles(uint32_t res) { return (res + 7u) / 8u; }                  // per side
    static uint32_t meshMaskWords(uint32_t res) { const uint32_t t = meshTiles(res); return (t * t + 31u) / 32u; }
    static VkDeviceSize meshMarksBytes(uint32_t res)
    {
        const uint32_t t = meshTiles(res);
        return ((VkDeviceSize)kMkHead + 2u * meshMaskWords(res) + 2u * (VkDeviceSize)t * t) * 4u;
    }
    uint64_t  mCoverGen = 0;               // #946 grids published (planSnow)
    // M5 MOVER TRACKS: the stamp list (96 B head / parameters / totals / #783 body parameters + counters + 12 KB per-box volumes +
    // kMoverStampMax x 64 B; own set binding 7), allocated with the avatar buffers; its head's
    // readback per frame slot (host-visible 48 B), whether that slot's last recording copied it;
    // this step's head / parameters / zeroed totals (recordSnow).
    GpuBuffer mMovers;
    GpuBuffer mMoverRead[2];
    bool      mMoverReadWritten[2] = {};
    bool      mMoverDue = false;          // the instance pass appends mover boxes this step
    bool      mLimbDue = false;           // #783 follow-up: the avatar pass appends limb boxes this step
    bool listDue() const { return mMoverDue || mLimbDue; }   // the stamp list is live (reset, gathered, read back)
    uint32_t  mMoverHead[24] = {};
    static constexpr uint32_t kStampSlots = kMoverStampMax + kLimbStampMax;   // (movers, then limbs)
    static constexpr uint32_t kMoverHeadBytes = 96, kMoverVolBytes = kStampSlots * 16u;   // (clean_weather_snow.comp WxMovers)
    uint32_t  mAvBitsCpu[64] = {};
    bool      mAvDue = false, mStampDue = false;
    double    mSnowRetryAt = 0.0;
    // The cover grid's schedule (as the maps'): the published grid and its frame (corner x, y,
    // extent, top, span, texel, shear x, y = the near map frame it was integrated in), the one
    // written last recording (published by this prepare) and this recording's.
    int       mCoverPub = -1, mCoverReady = -1;
    uint32_t  mCoverTarget = 0;
    float     mCoverPubF[8] = {}, mCoverReadyF[8] = {}, mCoverPendF[8] = {};
    bool      mCoverDue = false, mInstDue = false;
    bool      mCoverFresh = true;          // no previous grid to carry (first use / a new place: resetGround)
    float     mSnowPush[32] = {};          // this recording's push block (clean_weather_snow.comp SnowPush, 128 B)
    // ---- M3 snow mesh ----
    // Layout {own}: 0 the two cover grids (storage, read), 1 vertices, 2 indices, 3 the draw's
    // indirect arguments (+ the quad counter). The draw pipelines use Frame::drawLayout.
    VkDescriptorSetLayout mMeshDsl = VK_NULL_HANDLE;
    VkDescPool            mMeshPool;
    VkDescriptorSet       mMeshSet = VK_NULL_HANDLE;
    VkPipelineLayout      mMeshLayout = VK_NULL_HANDLE;
    VkPipeline            mMeshBuildPipe = VK_NULL_HANDLE, mMeshGbufPipe = VK_NULL_HANDLE, mMeshShadowPipe = VK_NULL_HANDLE;
    VkPipeline            mMeshFwdPipe[2] = {};   // the forward lit pass 1's draw: plain, ray-query
    VkPipeline            mMeshPrePipe[4] = {};   // the depth pre-pass draws (Frame::preDesc variants)
    GpuBuffer mMeshVerts, mMeshIdx, mMeshArgs;
    uint32_t  mMeshRes = 0, mMeshRingRes = 0, mMeshTris = 0;
    bool      mMeshBuilt = false;          // this recording built the mesh (meshReady)
    bool      mMeshShadowOn = false;       // RenderVKWeatherSnowMeshShadows, latched by prepare()
    // #946 the build's cadence: the planned push block / dispatch, whether the buffers hold a built
    // mesh, what it was built for (camera, view direction, the grid / ring frames), its age.
    float     mMeshPush[32] = {};
    uint32_t  mMeshGroups = 0;
    bool      mMeshRingNow = false;
    bool      mMeshBuildDue = false;       // planned: recordMeshBuild dispatches this recording
    bool      mMeshValid = false;          // the buffers hold a finished build
    bool      mMeshDirty = true;           // force the next build (origin shift, ground reset, a snapshot)
    uint32_t  mMeshAge = 0;                // live recordings since the last build
    // #946 LOCAL PATCHES: planned this recording (the last build's push block, this recording's
    // grids), the list cap, the published grid the last build / patch saw (mCoverGen).
    bool      mMeshPatchDue = false;
    float     mMeshPatchPush[32] = {};
    uint32_t  mMeshPatchCap = 0;
    uint64_t  mMeshGen = 0;
    float     mMeshCam[3] = {}, mMeshDir[3] = {};
    float     mMeshCoverF[8] = {}, mMeshRingF[8] = {};
    double    mMeshRetryAt = 0.0;
    // #783 THE SNOW DUMP (RenderVKWeatherSnowDump / DumpWindow; clean_weather_snowmesh.comp binding 7,
    // scripts/dev/snowdump.py): a one-shot numeric readback of a full build around the camera's focus.
    // planMeshBuild arms it (forces a full build; the dump buffer grown with a device wait + set
    // rewrite, the prestep's rule), recordMeshBuild copies the mesh as drawn before the build, the
    // build's per-texel records and the mesh after it into a host-visible staging buffer, and the
    // next prepare() on that frame slot (its fence waited) writes logs/snowdump-<time>.bin and frees
    // the buffers (back to the 256 B placeholder binding 7 always names).
    GpuBuffer mDumpBuf, mDumpStage;
    bool      mDumpArmed = false, mDumpWritten = false;
    uint32_t  mDumpSlot = 0, mDumpW = 0;
    int32_t   mDumpOrg[4] = {};            // the inner / ring windows' first texel
    VkDeviceSize mDumpOff[7] = {};         // in the staging: pre verts / idx / args, post verts / idx / args, records
    float     mDumpFocus[3] = {}, mDumpCam[3] = {};
    bool      mDumpFocusOk = false;
    float     mDumpPush[32] = {};          // the build's push block
    uint32_t  mDumpMeshAge = 0;            // recordings the drawn (pre) mesh had lived since its build
    float     mDumpGlobalDepth = 0.f, mDumpWater = 0.f;
    bool      planDump(const Frame& f, bool ring);   // planMeshBuild: true = armed (a full build is due)
    void      readDump();                            // prepare(): the slot's fence waited
    void      freeDump(bool keepPlaceholder);        // (the device idle)
    float     mDt = 0.f;                   // this recording's simulation step (s)
    float     mPrevGlobalDepth = -1.f;     // #895: SLWeather's snowDepth at the last live snow step (-1: none)
    float     mPrevGlobalCover = -1.f;     // #783: SLWeather's snowCover there (the grid's cover follows a jump up)

    // ---- M4 run-off ----
    // Own set {0 the near maps (sampled), 1 scratch, 2 the flow map (storage), 3 the emitters};
    // the build pipeline's layout = {that set} + its 80 B push. The spawn kernel / stream draw use
    // the module's own set (binding 8 = the emitters; the splash ring stands in while there are none).
    VkDescriptorSetLayout mRunoffDsl = VK_NULL_HANDLE;
    VkDescPool            mRunoffPool;
    VkDescriptorSet       mRunoffSet = VK_NULL_HANDLE;
    VkPipelineLayout      mRunoffLayout = VK_NULL_HANDLE;
    VkPipeline            mRunoffPipe = VK_NULL_HANDLE, mSpawnPipe = VK_NULL_HANDLE, mStreamPipe = VK_NULL_HANDLE;
    GpuBuffer mRunoffScratch, mEmitters;
    GpuImage  mFlowImg;                    // rgba32f R^2 (clean_weather_runoff.comp pass 7)
    uint32_t  mRunoffRes = 0, mRunoffFactor = 1, mEmitCap = 0;
    uint64_t  mNearGen = 0, mFlowGen = ~0ull;   // near-map publishes / the one the flow was built from
    float     mFlowF[8] = {};              // the flow grid's near frame (x0, y0, extent, top, span, texel, sx, sy)
    bool      mFlowValid = false;          // the flow map describes the published near map
    bool      mFlowDue = false;            // this recording builds it (prepare)
    bool      mRunoffNow = false;          // this recording runs the drips / streams
    bool      mDripsClean = false;         // the drip slots were cleared since run-off (re)started
    uint32_t  mDripCount = 0, mDripBase = 0;   // this recording's drip slots (drips) and the first slot
    double    mRunoffRetryAt = 0.0;

    // ---- #901 per-avatar shelter ----
    // Layout {draw set 0, own} + the 128 B push; own: 0 the raster target (r32ui), 1 the two atlas
    // images (rg32f), 2 the record, 3 / 4 the main cull's candidates / meta, 5 the skinned
    // vertices, 6 the mega index buffer, 7 the skin meta, 8 the avatars' motion state (stand-ins:
    // the record). One set per frame slot (the renderer's buffers written at record time).
    static constexpr uint32_t kShLayers = 16;          // clean_weather_shelter.glsl WX_SH_LAYERS
    static constexpr uint32_t kShItemCap = 8192;       // the raster's work list
    static constexpr uint32_t kShHeadBytes = 384;      // the CPU-written part of the record (head .. count)
    static constexpr uint32_t kShRecordBytes = 1632;   // the record before its items (+ wet / attach / stat / resets)
    static constexpr uint32_t kShResetOffset = 1552;   // the CPU-written wetness resets (rhead + rst[8], 80 B)
    static constexpr uint32_t kShSetBindings = 15;   // (#901: + 12 the accumulation record, 13 its scratch, 14 its grids)
    // The AVATAR SNOW MESH (clean_weather_shelter.comp -DWX_SH_MESH; RenderVKWeatherAvatarSnowMesh):
    // one vertex per second atlas texel (16 B), up to two triangles per cell, its indexed indirect
    // draw (+ the index capacity); allocated while the avatars carry snow.
    GpuBuffer mShMeshV, mShMeshI, mShMeshA;
    uint32_t  mShMeshIdxCap = 0;
    bool      mShMeshBuilt = false;                    // this recording's chain built it (meshReady)
    double    mShMeshRetryAt = 0.0;
    bool ensureShelterMesh(VkCommandBuffer cmd, bool want);
    void destroyShelterMesh();
    VkDescriptorSetLayout mShDsl = VK_NULL_HANDLE;
    VkDescPool            mShPool;
    VkDescriptorSet       mShSet[2] = {};
    VkPipelineLayout      mShLayout = VK_NULL_HANDLE;
    VkPipeline            mShPipe[kShKernels] = {};
    VkBuffer              mShBound[2][6] = {};         // the renderer buffers each set names (cand, meta, skin, ibo, skin meta, motion)
    GpuImage  mShH, mShS[2];
    GpuBuffer mShBuf;
    GpuBuffer mShRead[2];                              // the record (before its items), per frame slot (stats, the own layer's log)
    bool      mShReadWritten[2] = {};
    uint32_t  mShConfig = 3;                           // the allocated tier configuration (3: none)
    uint32_t  mShTilesH = 0;                           // the tiles' rows (the per-slot rows follow)
    int32_t   mShLayerSlot[kShLayers];                 // the joint-table slot of each layer (-1 free)
    bool      mShFresh[kShLayers] = {};                // newly assigned: no history (reset)
    uint32_t  mShRound = 0;                            // the round-robin cursor (the other avatars)
    uint32_t  mShHead[kShHeadBytes / 4] = {};
    uint32_t  mShLayersUsed = 0;                       // the layers' range this recording (0: none)
    int       mShPub = -1, mShDst = 0;                 // the atlas image last written / written this recording
    bool      mShDue = false;
    double    mShRetryAt = 0.0, mShLastNeed = -1e9, mShCalmSince = 0.0;
    uint32_t  mShWant = 0;                             // the configuration the pressure allows
    bool      mShInit = false;
    LLUUID    mShLayerId[kShLayers];                   // #901 the avatar of each layer this recording (the grids' owners)
    bool      mShLayerSelf[kShLayers] = {};

    // ---- #901 rest-pose accumulation (llcleanweathervk_accum.cpp) ----
    // The pool: one 3D rgba8 image (res.x x 3 buckets, res.y, res.z x grids), the record (draw set 0
    // binding 50 / shelter set 12), the scratch (kAcSlots grids x cells x buckets x 3 words), per frame
    // slot a host-visible readback of the record's GPU part (boxes, stats) and of the own grid (its
    // snapshot: it outlives the module). Soft allocation; a refusal retries kAcRetrySecs later with
    // half the grids (down to the own avatar's alone), logged.
    static constexpr uint32_t kAcGrids = 16;           // clean_weather_accum.glsl WX_AC_GRIDS
    static constexpr uint32_t kAcSlots = 4;            // grids updated per recording at most (scratch slots)
    static constexpr uint32_t kAcHeadBytes = 288;      // the CPU-written part (head, kgrid, ctl)
    static constexpr uint32_t kAcGpuOffset = 288, kAcGpuBytes = 1792;   // box .. stat (the readback)
    static constexpr uint32_t kAcRestOffset = 6176;    // the rest skeleton (216 x 48 B)
    struct AcGrid
    {
        LLUUID   id;                                   // owner (null: free)
        int32_t  slot = -1;                            // its joint-table slot now
        bool     self = false;
        bool     fresh = false;                        // newly assigned: its first update resets it
        bool     seed = false;                         // newly assigned own grid restored from the snapshot
        double   lastUpdate = -1.0, lastSeen = 0.0;    // mNow
    };
    VkPipeline mAcPipe[kAcKernels] = {};               // (aliases of mStage[kAcStage0 ..]: destroyed there)
    GpuImage  mAcImg;
    GpuBuffer mAcRec, mAcScratch;
    GpuBuffer mAcRead[2], mAcSnap[2];
    bool      mAcReadWritten[2] = {}, mAcSnapWritten[2] = {};
    int32_t   mAcSnapGrid[2] = {-1, -1};               // the grid a slot's snapshot copy holds
    LLUUID    mAcSnapId[2];
    uint32_t  mAcRes[3] = {0, 0, 0};
    uint32_t  mAcPoolGrids = 0;                        // grids in the allocated pool
    uint32_t  mAcFailures = 0;
    double    mAcRetryAt = 0.0, mAcSnapAt = 0.0;
    AcGrid    mAcGrid[kAcGrids];
    uint32_t  mAcHead[kAcHeadBytes / 4] = {};
    float     mAcPush[16] = {};                        // this recording's rates (AcPush b .. e)
    uint32_t  mAcFlags = 0;                            // AcPush a.y (snow / wet / motion)
    bool      mAcLayerDue[kShLayers] = {};             // this recording's accum-due layers (the shelter header bit 4)
    bool      mAcDue = false, mAcHeadDirty = true, mAcRestSent = false;
    uint32_t  mAcSlotsUsed = 0, mAcCounter = 0;
    int32_t   mAcSeedGrid = -1;                        // the grid restored from the snapshot this recording
    int32_t   mAcSelfGrid = -1;
    static AccumStats sAccumStats;
    static void registerAccumProbe();
    bool ensureAccum(VkCommandBuffer cmd, bool want);
    void destroyAccum();
    void planAccum(VkCommandBuffer cmd, const Frame& f, uint32_t used);
    void readAccumBack();
    void writeAccumSets(uint32_t slot, VkWriteDescriptorSet* wr, VkDescriptorBufferInfo* bi, VkDescriptorImageInfo* ii, uint32_t& n);
};

#endif
