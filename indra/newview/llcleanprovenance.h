/**
 * @file llcleanprovenance.h
 * @brief #29 race tripwire -- provenance ring for clean-pipeline geometry state.
 *
 * A lock-free ring of recent mutations to the shared geometry structures (mega-
 * VBO slot alloc/free, LOD-group publish, asset invalidate/capture, instance
 * upsert/release), each stamped with the FRAME and the THREAD that did it. When
 * the #29 detector sees a live instance's draw slot go empty/OOB/sentinel, it
 * calls dumpFor(slot_id, group_id) to print the recent history of that exact
 * slot/group -- turning "geometry vanished, here are some counters" into "thread
 * T freed slot X (op FREE_SLOT) at frame F while instance's group G still pointed
 * at it." A race you can see the write for is a race you can fix.
 *
 * Header-only (function-local statics) so it needs no .cpp / CMake change, and
 * entirely #if CLEAN_DEBUG so it compiles out of production builds.
 */
#pragma once

#include "llcleandebug.h"

// NOTE (2026-08-07): the recording ring is now ALWAYS compiled (not CLEAN_DEBUG-
// gated) so the #29 vertex-wipe self-heal can dump a wiped slot's mutation history
// in the user's playable CLEAN_DEBUG=0 build. record() is a lock-free ring write
// (a handful of stores + one relaxed atomic, no GL, no lock) so it is cheap enough
// to leave on. Only the HEAVY per-upload glGetBufferSubData verification blocks in
// llcleanmeshasset.cpp stay #if CLEAN_DEBUG.

#include "stdtypes.h"
#include "lluuid.h"
#include "llerror.h"

#include <array>
#include <atomic>
#include <thread>
#include <functional>

extern U32 gFrameCount;

namespace CleanProv
{
    enum Op : U32
    {
        ALLOC_VERT = 0, FREE_VERT, ALLOC_IDX, FREE_IDX,
        REG_LODSLOT, INVALIDATE, CAPTURE, GROW_IDX, GROW_VERT,
        UPSERT_INST, RELEASE_INST, RECAP_SLOTID,
        OP_COUNT
    };

    inline const char* opName(U32 op)
    {
        static const char* n[OP_COUNT] = {
            "ALLOC_VERT","FREE_VERT","ALLOC_IDX","FREE_IDX",
            "REG_LODSLOT","INVALIDATE","CAPTURE","GROW_IDX","GROW_VERT",
            "UPSERT_INST","RELEASE_INST","RECAP_SLOTID" };
        return (op < OP_COUNT) ? n[op] : "?";
    }

    struct Entry
    {
        U32 seq;        // global monotonic sequence (ordering across threads)
        U32 frame;      // gFrameCount at record time
        U32 thr;        // hashed std::thread::id (stable per thread this run)
        U32 op;         // Op
        U32 slot_id;    // slot involved (0xFFFFFFFF if n/a)
        U32 group_id;   // lod group involved (0xFFFFFFFF if n/a)
        U32 aux;        // op-specific: count / lod level / face
        U64 id_lo;      // low 64 bits of mesh_id / object uuid (identity hint)
    };

    static constexpr U32 RING_BITS = 14;            // 16384 entries
    static constexpr U32 RING_SIZE = 1u << RING_BITS;
    static constexpr U32 RING_MASK = RING_SIZE - 1u;

    inline std::array<Entry, RING_SIZE>& ring() { static std::array<Entry, RING_SIZE> r{}; return r; }
    inline std::atomic<U32>&             head() { static std::atomic<U32> h{0}; return h; }

    inline U32 threadHash()
    {
        return (U32)(std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xFFFFFFu);
    }

    inline void record(Op op, U32 slot_id, U32 group_id, U32 aux, const LLUUID& id)
    {
        const U32 seq = head().fetch_add(1u, std::memory_order_relaxed);
        Entry& e = ring()[seq & RING_MASK];
        e.seq = seq; e.frame = gFrameCount; e.thr = threadHash();
        e.op = (U32)op; e.slot_id = slot_id; e.group_id = group_id; e.aux = aux;
        U64 lo = 0; memcpy(&lo, id.mData, 8); e.id_lo = lo;
    }
    inline void record(Op op, U32 slot_id, U32 aux) { record(op, slot_id, 0xFFFFFFFFu, aux, LLUUID::null); }

    // Dump the last `count` mutations chronologically (oldest->newest). Called by
    // the #29 tripwire the moment it fires -- the offending FREE/INVALIDATE that
    // zeroed a live slot is in this window, stamped with its thread + frame.
    inline void dumpRecent(U32 count)
    {
        const U32 h = head().load(std::memory_order_relaxed);
        const U32 avail = (h < RING_SIZE) ? h : RING_SIZE;
        const U32 n = (count < avail) ? count : avail;
        LL_INFOS("Clean29Prov") << "--- last " << n << " geometry-state mutations (oldest first) ---" << LL_ENDL;
        for (U32 k = 0; k < n; ++k)
        {
            const Entry& e = ring()[(h - n + k) & RING_MASK];
            LL_INFOS("Clean29Prov") << "  seq=" << e.seq << " frame=" << e.frame
                << " thr=" << e.thr << " " << opName(e.op)
                << " slot=" << (S32)e.slot_id << " group=" << (S32)e.group_id
                << " aux=" << e.aux << " id_lo=" << e.id_lo << LL_ENDL;
        }
    }

    // Dump (chronologically) every recent ring entry that touched slot_id or
    // group_id -- the history that led to the #29 corruption of this slot.
    inline void dumpFor(U32 slot_id, U32 group_id)
    {
        const U32 h = head().load(std::memory_order_relaxed);
        const U32 n = (h < RING_SIZE) ? h : RING_SIZE;   // how many valid entries
        // Walk oldest->newest of the live window.
        U32 hits = 0;
        for (U32 k = 0; k < n; ++k)
        {
            const Entry& e = ring()[(h - n + k) & RING_MASK];
            const bool match = (slot_id != 0xFFFFFFFFu && e.slot_id == slot_id)
                            || (group_id != 0xFFFFFFFFu && e.group_id == group_id);
            if (!match) continue;
            ++hits;
            LL_INFOS("Clean29Prov") << "  seq=" << e.seq << " frame=" << e.frame
                << " thr=" << e.thr << " " << opName(e.op)
                << " slot=" << (S32)e.slot_id << " group=" << (S32)e.group_id
                << " aux=" << e.aux << " id_lo=" << e.id_lo << LL_ENDL;
        }
        if (hits == 0)
            LL_INFOS("Clean29Prov") << "  (no provenance for slot=" << (S32)slot_id
                << " group=" << (S32)group_id << " in last " << n << " ops)" << LL_ENDL;
    }

    // Dump (chronologically) every recent VERT-range op (ALLOC_VERT / FREE_VERT /
    // CAPTURE) whose aux == vbase, i.e. everything that touched this exact mega-VBO
    // vertex offset. This is the decisive #29 view: after our CAPTURE(vbase) of a
    // slot, a FREE_VERT(vbase) or a second ALLOC_VERT(vbase) to a DIFFERENT slot
    // means the range was recycled out from under a live slot -> its data reads
    // zero. Names the frame + thread that did it.
    inline void dumpForVbase(U32 vbase)
    {
        const U32 h = head().load(std::memory_order_relaxed);
        const U32 n = (h < RING_SIZE) ? h : RING_SIZE;
        U32 hits = 0;
        for (U32 k = 0; k < n; ++k)
        {
            const Entry& e = ring()[(h - n + k) & RING_MASK];
            const bool vert_range_op = (e.op == ALLOC_VERT || e.op == FREE_VERT || e.op == CAPTURE);
            if (!vert_range_op || e.aux != vbase) continue;
            ++hits;
            LL_INFOS("Clean29Prov") << "  [vbase=" << vbase << "] seq=" << e.seq
                << " frame=" << e.frame << " thr=" << e.thr << " " << opName(e.op)
                << " slot=" << (S32)e.slot_id << " group=" << (S32)e.group_id
                << " id_lo=" << e.id_lo << LL_ENDL;
        }
        if (hits == 0)
            LL_INFOS("Clean29Prov") << "  (no vert-range provenance for vbase=" << vbase
                << " in last " << n << " ops)" << LL_ENDL;
    }
}
