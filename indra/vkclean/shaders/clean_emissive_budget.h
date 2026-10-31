// Shared operation limits and integer helpers for the GPU emissive producer.
#ifndef CLEAN_EMISSIVE_BUDGET_H
#define CLEAN_EMISSIVE_BUDGET_H
#define EM_BUDGET_DISCOVERY 1024u
#define EM_BUDGET_OBJECTS 2048u
#define EM_BUDGET_FACE_TRIS 64u
#define EM_BUDGET_REACH 64u
#define EM_BUDGET_RADIUS_STEPS 12u
#define EM_BUDGET_WORDS 12u
#ifdef __cplusplus
#include <cstdint>
#define EB_UINT uint32_t
#define EB_FN static constexpr
// CPU-owned optional identity stream: zero stays unavailable after token exhaustion.
inline uint32_t ebNextIdentity(uint64_t& sequence)
{
    return sequence < 0xFFFFFFFFull ? static_cast<uint32_t>(++sequence) : 0u;
}
#else
#define EB_UINT uint
#define EB_FN
#endif
EB_FN EB_UINT ebDiscoveryEnd(EB_UINT cursor, EB_UINT size)
{
    EB_UINT start = cursor < size ? cursor : 0u;
    EB_UINT left = size - start;
    return start + (left < EM_BUDGET_DISCOVERY ? left : EM_BUDGET_DISCOVERY);
}
EB_FN EB_UINT ebRecordQuota(EB_UINT capacity, EB_UINT admitted)
{
    EB_UINT q = admitted != 0u ? capacity / admitted : 0u;
    return q < EM_BUDGET_FACE_TRIS ? q : EM_BUDGET_FACE_TRIS;
}
EB_FN EB_UINT ebTriangleStride(EB_UINT triangles, EB_UINT quota)
{
    return quota != 0u ? triangles / quota + (triangles % quota != 0u ? 1u : 0u) : 1u;
}
// Cyclic successor order over one frozen discovery span. Zero/out-of-span is unavailable.
EB_FN EB_UINT ebOfferDistance(EB_UINT candidate, EB_UINT owner, EB_UINT limit)
{
    if (candidate == 0u || candidate > limit) return 0xFFFFFFFFu;
    if (owner == 0u || owner > limit) return candidate;
    return candidate > owner ? candidate - owner : limit - owner + candidate;
}
EB_FN EB_UINT ebTriangleCount(EB_UINT triangles, EB_UINT stride, EB_UINT offset)
{
    EB_UINT left = triangles > offset ? triangles - offset : 0u;
    return left / stride + (left % stride != 0u ? 1u : 0u);
}
// Saturating volume: never multiplies unbounded dimensions.
EB_FN EB_UINT ebReachVolume3(EB_UINT x, EB_UINT y, EB_UINT z)
{
    if (x == 0u || y == 0u || z == 0u) return 0u;
    if (x > EM_BUDGET_REACH || y > EM_BUDGET_REACH || z > EM_BUDGET_REACH) return EM_BUDGET_REACH + 1u;
    EB_UINT xy = x * y;
    return xy > EM_BUDGET_REACH || xy > EM_BUDGET_REACH / z ? EM_BUDGET_REACH + 1u : xy * z;
}
#undef EB_FN
#undef EB_UINT
#endif
