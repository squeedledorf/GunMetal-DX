/**
* @file llperfstats.cpp
* @brief Statistics collection to support autotune and perf flaoter.
*
* $LicenseInfo:firstyear=2022&license=viewerlgpl$
* Second Life Viewer Source Code
* Copyright (C) 2022, Linden Research, Inc.
*
* This library is free software; you can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public
* License as published by the Free Software Foundation;
* version 2.1 of the License only.
*
* This library is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public
* License along with this library; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*
* Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
* $/LicenseInfo$
*/

#include "llviewerprecompiledheaders.h"
#include "llperfstats.h"
#include "llcontrol.h"
#include "pipeline.h"
#include "llagentcamera.h"
#include "llviewerwindow.h"
#include "llvoavatar.h"
#include "llwindow.h"
#include "llworld.h"
#include <llthread.h>

extern LLControlGroup gSavedSettings;

namespace LLPerfStats
{
    // avatar timing metrics in ms (updated once per mainloop iteration)
    std::atomic<F32> sTotalAvatarTime = 0.f;
    std::atomic<F32> sAverageAvatarTime = 0.f;
    std::atomic<F32> sMaxAvatarTime = 0.f;

    std::atomic<int64_t> tunedAvatars{0};
    std::atomic<U64> renderAvatarMaxART_ns{(U64)(ART_UNLIMITED_NANOS)}; // highest render time we'll allow without culling features
    std::mutex bufferToggleLock{};

    F64 cpu_hertz{0.0};

    std::atomic<int>    StatsRecorder::writeBuffer{0};
    bool                StatsRecorder::collectionEnabled{true};
    LLUUID              StatsRecorder::focusAv{LLUUID::null};
    std::array<StatsRecorder::StatsTypeMatrix,2>  StatsRecorder::statsDoubleBuffer{ {} };
    std::array<StatsRecorder::StatsSummaryArray,2> StatsRecorder::max{ {} };
    std::array<StatsRecorder::StatsSummaryArray,2> StatsRecorder::sum{ {} };

    void updateRenderCostLimitFromSettings()
    {
        assert_main_thread();
        const auto newval = gSavedSettings.getF32("RenderAvatarMaxART");
        if(newval < log10(LLPerfStats::ART_UNLIMITED_NANOS/1000))
        {
            LLPerfStats::renderAvatarMaxART_ns = (U64)pow(10,newval)*1000;
        }
        else
        {
            LLPerfStats::renderAvatarMaxART_ns = 0;
        }
    }

    StatsRecorder::StatsRecorder()
    {
        updateRenderCostLimitFromSettings();
        LLPerfStats::cpu_hertz = (F64)LLTrace::BlockTimer::countsPerSecond();
    }

    // static
    void StatsRecorder::toggleBuffer()
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_STATS;
        using ST = StatType_t;

        // Collection off (PerfStatsCaptureEnabled): RecordTime records nothing, so there
        // is nothing to smooth, swap or clear. Clear the write side once on the way out
        // so a later re-enable starts from zero.
        static bool was_enabled{true};
        if (!enabled())
        {
            if (was_enabled)
            {
                clearStatsBuffers();
                was_enabled = false;
            }
            return;
        }
        was_enabled = true;

        bool unreliable{false};
        LLPerfStats::StatsRecorder::getSceneStat(LLPerfStats::StatType_t::RENDER_FRAME);
        auto& sceneStats = statsDoubleBuffer[writeBuffer][static_cast<size_t>(ObjType_t::OT_GENERAL)][LLUUID::null];
        auto& lastStats = statsDoubleBuffer[writeBuffer ^ 1][static_cast<size_t>(ObjType_t::OT_GENERAL)][LLUUID::null];

        static constexpr std::initializer_list<StatType_t> sceneStatsToAvg = {
            StatType_t::RENDER_FRAME,
            StatType_t::RENDER_DISPLAY,
            StatType_t::RENDER_HUDS,
            StatType_t::RENDER_UI,
            StatType_t::RENDER_SWAP,
            // RENDER_LFS,
            // RENDER_MESHREPO,
            StatType_t::RENDER_IDLE };

#if 0
        static constexpr std::initializer_list<StatType_t> avatarStatsToAvg = {
            StatType_t::RENDER_GEOMETRY,
            StatType_t::RENDER_SHADOWS,
            StatType_t::RENDER_COMBINED,
            StatType_t::RENDER_IDLE };
#endif


        if( /*sceneStats[static_cast<size_t>(StatType_t::RENDER_FPSLIMIT)] != 0 ||*/ sceneStats[static_cast<size_t>(StatType_t::RENDER_SLEEP)] != 0 )
        {
            unreliable = true;
            //lastStats[static_cast<size_t>(StatType_t::RENDER_FPSLIMIT)] = sceneStats[static_cast<size_t>(StatType_t::RENDER_FPSLIMIT)];
            lastStats[static_cast<size_t>(StatType_t::RENDER_SLEEP)] = sceneStats[static_cast<size_t>(StatType_t::RENDER_SLEEP)];
            lastStats[static_cast<size_t>(StatType_t::RENDER_FRAME)] = sceneStats[static_cast<size_t>(StatType_t::RENDER_FRAME)]; //  bring over the total frame render time to deal with region crossing overlap issues
        }

        if(!unreliable)
        {
            // only use these stats when things are reliable.

            for(auto & statEntry : sceneStatsToAvg)
            {
                auto avg = lastStats[static_cast<size_t>(statEntry)];
                auto val = sceneStats[static_cast<size_t>(statEntry)];
                sceneStats[static_cast<size_t>(statEntry)] = avg + (val / SMOOTHING_PERIODS) - (avg / SMOOTHING_PERIODS);
                // LL_INFOS("scenestats") << "Scenestat: " << static_cast<size_t>(statEntry) << " before=" << avg << " new=" << val << " newavg=" << statsDoubleBuffer[writeBuffer][static_cast<size_t>(ObjType_t::OT_GENERAL)][LLUUID::null][static_cast<size_t>(statEntry)] << LL_ENDL;
            }
        }

        // swap the buffers
        if(enabled())
        {
            std::lock_guard<std::mutex> lock{bufferToggleLock};
            writeBuffer ^= 1;
        }; // note we are relying on atomic updates here. The risk is low and would cause minor errors in the stats display.

        // clean the write maps in all cases. Zero the entries in place rather than
        // clear(): the same few keys (the scene stats' null id) come back every frame,
        // and clear() freed and re-allocated their nodes every frame. A zeroed entry
        // reads exactly like an absent one (operator[] inserts zeros). Only a map that
        // grew past a handful of keys is cleared, so it cannot grow without bound.
        static constexpr size_t MAX_KEPT_KEYS = 64;
        auto& statsTypeMatrix = statsDoubleBuffer[writeBuffer];
        for(auto& statsMapByType : statsTypeMatrix)
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_STATS("Clear stats maps");
            if (statsMapByType.size() > MAX_KEPT_KEYS)
            {
                statsMapByType.clear();
                continue;
            }
            for(auto& stat_entry : statsMapByType)
            {
                std::fill_n(stat_entry.second.begin() ,static_cast<size_t>(ST::STATS_COUNT),0);
            }
        }
        for(int i=0; i< static_cast<size_t>(ObjType_t::OT_COUNT); i++)
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_STATS("clear max/sum");
            max[writeBuffer][i].fill(0);
            sum[writeBuffer][i].fill(0);
        }
    }

    // clear buffers when we change region or need a hard reset.
    // static
    void StatsRecorder::clearStatsBuffers()
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_STATS;
        using ST = StatType_t;

        auto& statsTypeMatrix = statsDoubleBuffer[writeBuffer];
        for(auto& statsMap : statsTypeMatrix)
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_STATS("Clear stats maps");
            for(auto& stat_entry : statsMap)
            {
                std::fill_n(stat_entry.second.begin() ,static_cast<size_t>(ST::STATS_COUNT),0);
            }
            statsMap.clear();
        }
        for(int i=0; i< static_cast<size_t>(ObjType_t::OT_COUNT); i++)
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_STATS("clear max/sum");
            max[writeBuffer][i].fill(0);
            sum[writeBuffer][i].fill(0);
        }
        // swap the clean buffers in
        if(enabled())
        {
            std::lock_guard<std::mutex> lock{bufferToggleLock};
            writeBuffer ^= 1;
        };
        // repeat before we start processing new stuff
        for(auto& statsMap : statsTypeMatrix)
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_STATS("Clear stats maps");
            for(auto& stat_entry : statsMap)
            {
                std::fill_n(stat_entry.second.begin() ,static_cast<size_t>(ST::STATS_COUNT),0);
            }
            statsMap.clear();
        }
        for(int i=0; i< static_cast<size_t>(ObjType_t::OT_COUNT); i++)
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_STATS("clear max/sum");
            max[writeBuffer][i].fill(0);
            sum[writeBuffer][i].fill(0);
        }
    }

    // called once per main loop iteration on main thread
    void updateClass()
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_STATS;

        sTotalAvatarTime = LLVOAvatar::getTotalGPURenderTime();
        sAverageAvatarTime = LLVOAvatar::getAverageGPURenderTime();
        sMaxAvatarTime = LLVOAvatar::getMaxGPURenderTime();
    }

}
