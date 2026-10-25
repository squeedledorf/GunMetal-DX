// vkclean — SNOW CONNECTIVITY (#783 M3): the one rule the snow depth's transport
// (clean_weather_snow.comp) and the snow mesh (clean_weather_snowmesh.comp) share.
//
// Every snow texel has a FILTERED BASE B (the near map's column tops, closed then opened at
// kSnowBaseR texels: clean_weather_snow.comp) and a snow top S = B + depth. Two texels dist
// metres apart are JOINED (one snow sheet, mass may move between them) when
//   their snow tops are within a seam + a slope allowance:  |S_a - S_b| <= kSnowSeam + kSnowJoinSlope x dist
//   or the lower one's snow has filled up to the higher one's base: S_low >= B_high
// ("low" / "high" by base). Otherwise it is a CUT: each side's sheet ends in a skirt down to
// its own base (never to the lower surface: nothing hangs from an eave). So a path a few cm
// above the grass is one sheet at once; a 15 cm curb stays two until the grass snow reaches
// the path's surface, then they join with a ramp.

const float kSnowSeam = 0.03;        // the seam (m): small steps between flat-adjacent surfaces
const float kSnowJoinSlope = 1.0;    // the slope allowance (dz / dx: 45 degrees)
const float kSnowRepose = 0.7;       // the angle of repose's default (dz / dx: ~35 degrees; the live value is
                                     // RenderVKWeatherSnowReposeAngle, pushed to each kernel)

bool snowJoined(float Ba, float Sa, float Bb, float Sb, float dist)
{
    if (abs(Sa - Sb) <= kSnowSeam + kSnowJoinSlope * dist) return true;
    return Ba <= Bb ? Sa >= Bb : Sb >= Ba;
}

// #783 THE REPOSE EXCHANGE (talus relaxation; the depth's cover / ring kernels): the change of a
// texel's depth (base bx, depth dx, snow top sx) from ONE joined neighbour dist metres away (base
// hn, depth dn, snow top sn) -- where the tops differ by more than tanR x dist, half the excess x
// share moves down, at most kSnowReposeDonor of the donor's depth. Computed identically from
// both sides (mass conserved). Eight neighbours (the diagonals at their true distance): the slope
// bound is the same in every direction, so a slumped wall rounds into a cone / a straight ramp
// instead of the four-neighbour rule's diamond with texel-aligned terraces.
const float kSnowReposeDonor = 0.09;   // (x 8 neighbours + the drift's <= 0.25 x 5 cm: never overdrawn)
// A base step up to kSnowStepFlow is one ground for the SLUMP even where the sheets are cut (a kerb,
// a boardwalk, a low deck above thin snow): snow slides off its edge onto the lower side and a deep
// pile on the lower side spills onto it -- a ramp at the repose instead of a wall. (The mesh keeps
// the cut: thin snow is not stretched over the step's face; its CLOSURE fills the cell.)
const float kSnowStepFlow = 0.2;
float snowReposeFlow(float bx, float dx, float hn, float dn, float dist, float tanR, float share)
{
    const float sx = bx + dx, sn = hn + dn;
    if (abs(bx - hn) > kSnowStepFlow && !snowJoined(bx, sx, hn, sn, dist)) return 0.0;   // a cut: no exchange
    const float dS = sn - sx;
    const float ex = abs(dS) - tanR * dist;
    if (ex <= 0.0) return 0.0;
    const float f = share * 0.5 * ex;
    return dS > 0.0 ? min(f, kSnowReposeDonor * dn) : -min(f, kSnowReposeDonor * dx);
}
// #783 THE COVER TOE (the repose across a blocked outline; the depth's cover / ring kernels): a
// neighbour whose base stands over this snow top and that this sheet cannot join (a cut toward a
// HIGHER column) is a COVER or a WALL -- the top-down maps cannot tell an eave from a wall, so every
// such outline is treated alike, at any height: the floor under it is assumed to continue at this
// texel's base. Snow slumps in under it: the texel's depth relaxes toward the TOE HEIGHT, the repose
// x the toe length (a one-sided exchange with a virtual neighbour on that floor; the snow that leaves
// is the part that slid in under the cover), and the ordinary repose then lays the sheet behind it
// at the repose. So the depth field ITSELF is a repose slope down to the outline, at every grid's
// resolution, and the mesh's apron (clean_weather_snowmesh.comp) continues it at the same slope onto
// the floor within the toe length -- the apron's reach is at least that long (kSnowCoverToe; at most
// kSnowApronTexels texels: the toe length shrinks with it on a very fine grid). Against a WALL the
// apron lies inside the wall; the sheet keeps its full depth wherever it is no deeper than the toe
// height (~0.55 m), and only deeper snow -- a drift pile -- is trimmed to it at the wall's face.
// Before (#783 rounds 1-8): the cut kept the sheet at full depth right up to the outline (round 8
// slumped only beside bases 6 m above it -- airships), and the apron, which must end on the floor
// within its reach (12 texels: 0.75 m on the inner grid; 2 texels -- 12.5 cm -- at
// RenderVKWeatherSnowToeReach 0), dropped at depth / reach: a vertical wall along the texel outline
// wherever the snow was deeper than the repose allows over that reach; the outer ring's edge depth
// is the mean of 4 x 4 inner texels and its reach >= 1 m (0.5 m texels), so the far layer showed a toe.
const float kSnowCoverAbove = 6.0;   // (the DRIFT's cover: clean_weather_snow.comp kDriftCover)
const float kSnowCoverToe = 0.7;     // the toe length (m) the depth leaves for the apron
const int   kSnowApronTexels = 12;   // the mesh apron's longest reach (texels; clean_weather_snowmesh.comp kAK)
float snowToeLength(float w) { return min(kSnowCoverToe, float(kSnowApronTexels) * w); }
// (bx, dx: this texel's base / depth; hn, dn: the neighbour's; dist: their distance; w: the grid's texel)
float snowCoverSink(float bx, float dx, float hn, float dn, float dist, float w, float tanR, float share)
{
    const float sx = bx + dx;
    if (hn <= sx + kSnowSeam || snowJoined(bx, sx, hn, hn + dn, dist)) return 0.0;
    const float ex = dx - tanR * snowToeLength(w);
    if (ex <= 0.0) return 0.0;
    return -min(share * 0.5 * ex, kSnowReposeDonor * dx);
}
const ivec2 kSnowN8[8] = ivec2[8](ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1),
                                  ivec2(1, 1), ivec2(-1, -1), ivec2(-1, 1), ivec2(1, -1));
