#include "CpuTopology.h"
#include "LogBuffer.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <sched.h>
#include <unistd.h>

namespace ds13r
{

namespace
{
long ReadMaxFreq(int cpu)
{
    char path[128];
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    long freq = -1;
    if (fscanf(f, "%ld", &freq) != 1) freq = -1;
    fclose(f);
    return freq;
}

CpuTopology Detect()
{
    CpuTopology topo;
    int ncpu = (int)sysconf(_SC_NPROCESSORS_CONF);

    // Group cores by max frequency, then sort clusters from fastest to slowest.
    std::map<long, std::vector<int>, std::greater<long>> byFreq;
    for (int i = 0; i < ncpu; i++)
    {
        long f = ReadMaxFreq(i);
        if (f > 0) byFreq[f].push_back(i);
    }

    if (byFreq.empty())
    {
        for (int i = 0; i < ncpu; i++) topo.mid.push_back(i);
        LOGW("CPU topology: cpufreq unavailable, treating all %d cores as mid", ncpu);
        return topo;
    }

    // Fastest single-core cluster = prime, slowest cluster = little, everything else = mid.
    // On the 8 Gen 3 that gives X4 / 3.15+2.96 GHz A720s / A520s.
    auto first = byFreq.begin();
    auto last = std::prev(byFreq.end());
    for (auto it = byFreq.begin(); it != byFreq.end(); ++it)
    {
        auto& dst = (it == first) ? topo.prime : (it == last && byFreq.size() > 2) ? topo.little : topo.mid;
        dst.insert(dst.end(), it->second.begin(), it->second.end());
    }

    auto fmt = [](const std::vector<int>& v) {
        std::string s;
        for (int c : v) s += std::to_string(c) + " ";
        return s;
    };
    LOGI("CPU topology: prime [%s] mid [%s] little [%s]", fmt(topo.prime).c_str(), fmt(topo.mid).c_str(), fmt(topo.little).c_str());
    return topo;
}
}

const CpuTopology& CpuTopology::Get()
{
    static CpuTopology topo = Detect();
    return topo;
}

bool PinCurrentThread(CoreClass cls)
{
    const CpuTopology& topo = CpuTopology::Get();
    const std::vector<int>* cores = nullptr;
    switch (cls)
    {
    case CoreClass::Prime: cores = &topo.prime; break;
    case CoreClass::Mid: cores = &topo.mid; break;
    case CoreClass::Little: cores = &topo.little; break;
    case CoreClass::Any: break;
    }

    cpu_set_t set;
    CPU_ZERO(&set);
    if (!cores || cores->empty())
    {
        int ncpu = (int)sysconf(_SC_NPROCESSORS_CONF);
        for (int i = 0; i < ncpu; i++) CPU_SET(i, &set);
    }
    else
    {
        for (int c : *cores) CPU_SET(c, &set);
    }

    if (sched_setaffinity(0, sizeof(set), &set) != 0)
    {
        LOGW("sched_setaffinity failed: %s", strerror(errno));
        return false;
    }
    return true;
}

int CurrentTid()
{
    return gettid();
}

}
