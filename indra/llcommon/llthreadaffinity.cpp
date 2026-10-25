/**
 * @file llthreadaffinity.cpp
 * @brief Per-thread CPU placement on multi-L3 (multi-CCD) machines.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Alchemy Viewer Project.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "linden_common.h"
#include "llthreadaffinity.h"
#include "slenv.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <sstream>
#include <vector>

#if LL_LINUX
#include <fstream>
#include <map>
#include <pthread.h>
#include <sched.h>
#endif

namespace
{
    std::atomic<bool> sActive{ false };
    std::string       sDescribe = "inactive";
    unsigned          sWorkerCpus = 0;
#if LL_LINUX
    cpu_set_t         sWorkerSet;

    // "0-5,24-29" -> {0..5, 24..29}
    std::vector<int> parseCpuList(const std::string& s)
    {
        std::vector<int> out;
        std::stringstream ss(s);
        std::string part;
        while (std::getline(ss, part, ','))
        {
            if (part.empty()) continue;
            const size_t dash = part.find('-');
            try
            {
                const int a = std::stoi(part.substr(0, dash));
                const int b = dash == std::string::npos ? a : std::stoi(part.substr(dash + 1));
                for (int c = a; c <= b; ++c) out.push_back(c);
            }
            catch (...) {}
        }
        return out;
    }

    std::string readLine(const std::string& path)
    {
        std::ifstream f(path);
        std::string line;
        if (f) std::getline(f, line);
        return line;
    }

    std::string setToString(const cpu_set_t& set)
    {
        std::ostringstream os;
        int run_start = -1, prev = -2;
        bool first = true;
        auto flush = [&]() {
            if (run_start < 0) return;
            if (!first) os << ',';
            first = false;
            os << run_start;
            if (prev != run_start) os << '-' << prev;
        };
        for (int c = 0; c < CPU_SETSIZE; ++c)
        {
            if (!CPU_ISSET(c, &set)) continue;
            if (c != prev + 1) { flush(); run_start = c; }
            prev = c;
        }
        flush();
        return os.str();
    }
#endif
}

namespace LLThreadAffinity
{
void init(bool enabled)
{
#if LL_LINUX
    if (!enabled || sl_getenv("NO_AFFINITY"))
    {
        sDescribe = enabled ? "disabled (SLIPSTREAM_NO_AFFINITY)" : "disabled (setting)";
        return;
    }
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0)
    {
        sDescribe = "disabled (sched_getaffinity failed)";
        return;
    }
    const int n_allowed = CPU_COUNT(&allowed);

    // L3 domains, keyed by their lowest CPU, restricted to the CPUs we may use
    // (respects an outer taskset / cgroup).
    std::map<int, cpu_set_t> domains;
    for (int c = 0; c < CPU_SETSIZE; ++c)
    {
        if (!CPU_ISSET(c, &allowed)) continue;
        std::string list;
        for (int idx = 0; idx < 8 && list.empty(); ++idx)
        {
            const std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(c) +
                                     "/cache/index" + std::to_string(idx) + "/";
            if (readLine(base + "level") == "3") list = readLine(base + "shared_cpu_list");
        }
        if (list.empty()) continue;
        const std::vector<int> cpus = parseCpuList(list);
        if (cpus.empty()) continue;
        int lo = cpus.front();
        for (int x : cpus) lo = std::min(lo, x);
        if (domains.count(lo)) continue;
        cpu_set_t d;
        CPU_ZERO(&d);
        for (int x : cpus)
            if (x >= 0 && x < CPU_SETSIZE && CPU_ISSET(x, &allowed)) CPU_SET(x, &d);
        if (CPU_COUNT(&d) > 0) domains[lo] = d;
    }
    if (domains.size() < 2 || n_allowed < 8)
    {
        std::ostringstream os;
        os << "inactive (" << domains.size() << " L3 domain(s), " << n_allowed << " CPUs)";
        sDescribe = os.str();
        return;
    }

    // Primary = the domain holding the lowest allowed CPU; workers = the rest.
    const cpu_set_t primary = domains.begin()->second;
    CPU_ZERO(&sWorkerSet);
    for (auto it = std::next(domains.begin()); it != domains.end(); ++it)
        CPU_OR(&sWorkerSet, &sWorkerSet, &it->second);
    sWorkerCpus = (unsigned)CPU_COUNT(&sWorkerSet);

    if (pthread_setaffinity_np(pthread_self(), sizeof(primary), &primary) != 0)
    {
        sDescribe = "disabled (pthread_setaffinity_np failed)";
        sWorkerCpus = 0;
        return;
    }
    sActive = true;

    std::ostringstream os;
    os << domains.size() << " L3 domains; main thread -> CPUs " << setToString(primary)
       << " ; workers -> CPUs " << setToString(sWorkerSet) << " (" << sWorkerCpus << " logical)";
    sDescribe = os.str();
#else
    (void)enabled;
    sDescribe = "inactive (platform)";
#endif
}

void applyWorker()
{
#if LL_LINUX
    if (!sActive) return;
    pthread_setaffinity_np(pthread_self(), sizeof(sWorkerSet), &sWorkerSet);
#endif
}

bool active() { return sActive; }
unsigned workerCpuCount() { return sActive ? sWorkerCpus : 0u; }
std::string describe() { return sDescribe; }
}
