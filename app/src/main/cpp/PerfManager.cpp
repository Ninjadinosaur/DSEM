#include "PerfManager.h"
#include "LogBuffer.h"

#include <android/performance_hint.h>
#include <android/thermal.h>
#include <cmath>
#include <ctime>

namespace ds13r
{

namespace
{
int64_t NowNs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
}

PerfManager::PerfManager()
{
    thermal = AThermal_acquireManager();
}

PerfManager::~PerfManager()
{
    if (session) APerformanceHint_closeSession(session);
    if (thermal) AThermal_releaseManager(thermal);
}

void PerfManager::StartSession(const std::vector<int32_t>& tids, int64_t targetNs)
{
    if (session)
    {
        APerformanceHint_closeSession(session);
        session = nullptr;
    }

    APerformanceHintManager* mgr = APerformanceHint_getManager();
    if (!mgr || tids.empty())
    {
        LOGW("Perf: ADPF performance hints unavailable");
        return;
    }
    session = APerformanceHint_createSession(mgr, tids.data(), tids.size(), targetNs);
    if (session)
        LOGI("Perf: ADPF hint session for %zu threads, target %.2f ms", tids.size(), targetNs / 1e6);
    else
        LOGW("Perf: failed to create ADPF hint session");
}

void PerfManager::SetTarget(int64_t targetNs)
{
    if (session) APerformanceHint_updateTargetWorkDuration(session, targetNs);
}

void PerfManager::ReportWork(int64_t actualNs)
{
    if (session && actualNs > 0) APerformanceHint_reportActualWorkDuration(session, actualNs);
}

void PerfManager::PollThermal()
{
    if (!thermal) return;
    int64_t now = NowNs();
    if (now - lastPoll < 1000000000LL) return;
    lastPoll = now;

    status = (int)AThermal_getCurrentThermalStatus(thermal);
    float h = AThermal_getThermalHeadroom(thermal, 10);
    if (!std::isnan(h)) headroom = h;
}

}
