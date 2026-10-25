/**
 * @file llcleanclustercaps.cpp
 * @brief Cluster LOD (#743): capability tiers from the device's enabled features.
 */
#include "linden_common.h"

#include "llcleanclustercaps.h"
#include "../llcleanclusterformat.h"

#include <algorithm>
#include <cstdio>

LLCleanClusterCaps LLCleanClusterCaps::detect(const VkDeviceContext& ctx, int32_t force)
{
    LLCleanClusterCaps c;
    c.feat = ctx.clusterFeatures();
    const VkClusterFeatures& f = c.feat;
    auto because = [&](const char* s) {
        if (!c.why.empty()) c.why += "; ";
        c.why += s;
    };

    // RASTER
    c.native[CLCAP_RASTER] = CLT_RASTER_MDI_PULL;
    if (f.indexTypeUint8) c.native[CLCAP_RASTER] = CLT_RASTER_MDI_U8;
    if (f.meshShader)
    {
        if (f.meshMaxOutputVertices >= LLClusterFmt::kMaxClusterVerts
            && f.meshMaxOutputPrimitives >= LLClusterFmt::kMaxClusterTris
            && f.meshMaxWorkGroupInvocations >= LLClusterFmt::kMaxClusterVerts)
            c.native[CLCAP_RASTER] = CLT_RASTER_MESH;
        else
            because("mesh shader limits below the cluster size");
    }
    // CMDGEN
    c.native[CLCAP_CMDGEN] = f.dgc ? CLT_CMDGEN_DGC : CLT_CMDGEN_COUNT;
    // UPLOAD
    c.native[CLCAP_UPLOAD] = CLT_UPLOAD_COPY;
    if (f.copyIndirectKHR || f.copyIndirectNV) c.native[CLCAP_UPLOAD] = CLT_UPLOAD_INDIRECT;
    if ((f.memDecompEXT || f.memDecompNV) && c.native[CLCAP_UPLOAD] == CLT_UPLOAD_INDIRECT)
    {
        // GDeflate 1.0 decompression is present, but no GDeflate encoder is in the tree yet:
        // the disk cache stores uncompressed pages, so the stream copies them (indirect
        // copies). The tier stays held there -- the line reports the path actually used --
        // until 1a writes kCompGDeflate pages (CLUSTER_LOD_CONTRACT.md, UPLOAD).
        if (f.decompressionMethods & 1ull)
            because("upload: GPU decompression held at indirect copies (no GDeflate encoder yet; uncompressed cache pages)");
        else
            because("memory decompression without GDeflate 1.0");
    }
    else if (f.memDecompEXT || f.memDecompNV)
        because("memory decompression without indirect copies");
    // RT
    if (!ctx.rayTracingEnabled())
        c.native[CLCAP_RT] = CLT_RT_NONE;
    else
    {
        c.native[CLCAP_RT] = CLT_RT_BLAS;
        if (f.clusterAS)
        {
            if (f.clasMaxVerticesPerCluster >= LLClusterFmt::kMaxClusterVerts
                && f.clasMaxTrianglesPerCluster >= LLClusterFmt::kMaxClusterTris)
                c.native[CLCAP_RT] = f.partitionedAS ? CLT_RT_CLAS_PTLAS : CLT_RT_CLAS;
            else
                because("cluster AS limits below the cluster size");
        }
    }
    // ATOMIC64
    c.native[CLCAP_ATOMIC64] = f.bufferInt64Atomics ? CLT_ATOMIC64 : CLT_ATOMIC32;

    c.asyncCompute = ctx.asyncComputeAvailable();
    c.asyncComputeOnly = ctx.computeQueueComputeOnly();
    VkPhysicalDeviceProperties pp{};
    vkGetPhysicalDeviceProperties(ctx.physicalDevice(), &pp);
    c.maxStorageBufferRange = pp.limits.maxStorageBufferRange;
    c.force = -2;   // force the first apply to fill tier[]
    c.applyForce(force);
    return c;
}

bool LLCleanClusterCaps::applyForce(int32_t f)
{
    const bool first = force == -2;
    const int32_t was = force;
    force = f < -1 ? -1 : f;
    bool changed = false;
    for (uint32_t i = 0; i < CLCAP_COUNT; ++i)
    {
        int32_t t = native[i];
        if (t >= 0 && force >= 0) t = std::min(t, force);
        changed = changed || t != tier[i];
        tier[i] = t;
    }
    return changed || first || force != was;   // (a setting change is logged even when no tier moved)
}

const char* LLCleanClusterCaps::capName(uint32_t cap)
{
    static const char* const k[CLCAP_COUNT] = {"raster", "cmdgen", "upload", "rt", "atomic64"};
    return cap < CLCAP_COUNT ? k[cap] : "?";
}

const char* LLCleanClusterCaps::tierName(uint32_t cap, int32_t t)
{
    switch (cap)
    {
    case CLCAP_RASTER:   return t == CLT_RASTER_MESH ? "mesh_shader" : t == CLT_RASTER_MDI_U8 ? "mdi_uint8" : "mdi_pull";
    case CLCAP_CMDGEN:   return t == CLT_CMDGEN_DGC ? "dgc_ext" : "draw_count";
    case CLCAP_UPLOAD:   return t == CLT_UPLOAD_DECOMPRESS ? "gpu_decompress" : t == CLT_UPLOAD_INDIRECT ? "indirect_copy" : "staging_copy";
    case CLCAP_RT:       return t == CLT_RT_CLAS_PTLAS ? "clas+ptlas" : t == CLT_RT_CLAS ? "clas" : t == CLT_RT_BLAS ? "per_cut_blas" : "none";
    case CLCAP_ATOMIC64: return t == CLT_ATOMIC64 ? "int64" : "int32";
    default:             return "?";
    }
}

std::string LLCleanClusterCaps::logLine() const
{
    std::string s = "Cluster LOD tiers (RenderVKClusterForceTier=" + std::to_string(force) + "):";
    for (uint32_t i = 0; i < CLCAP_COUNT; ++i)
    {
        char buf[96];
        if (tier[i] != native[i])
            std::snprintf(buf, sizeof(buf), " %s=%d %s (native %d %s)", capName(i), tier[i], tierName(i, tier[i]),
                          native[i], tierName(i, native[i]));
        else
            std::snprintf(buf, sizeof(buf), " %s=%d %s", capName(i), tier[i], tierName(i, tier[i]));
        s += buf;
        s += (i + 1 < CLCAP_COUNT) ? " |" : "";
    }
    s += asyncCompute ? (asyncComputeOnly ? " | async compute queue (compute-only family)" : " | async compute queue")
                      : " | no async queue";
    if (feat.meshShader)
        s += " | mesh out " + std::to_string(feat.meshMaxOutputVertices) + "v/" + std::to_string(feat.meshMaxOutputPrimitives)
           + "p, " + std::to_string(feat.meshMaxOutputMemory) + " B";
    if (feat.clusterAS)
        s += " | CLAS max " + std::to_string(feat.clasMaxVerticesPerCluster) + "v/" + std::to_string(feat.clasMaxTrianglesPerCluster) + "t";
    if (feat.imageInt64Atomics) s += " | image int64 atomics";
    s += feat.lodAtStartup ? " | device: every cluster extension (RenderVKClusterLOD at startup)"
                           : " | device: RenderVKClusterDeviceFeatures=" + std::to_string(feat.deviceFeatures)
                                 + (feat.deviceFeatures == 0 ? " (none: restart with it >= 1 or RenderVKClusterLOD on)"
                                                             : " (the extensions inert while unused)");
    if (feat.retried) s += " | DEVICE REFUSED THE CLUSTER EXTENSIONS: created without them";
    if (!why.empty()) s += " | held lower: " + why;
    if (!feat.notes.empty()) s += " | " + feat.notes;
    return s;
}
