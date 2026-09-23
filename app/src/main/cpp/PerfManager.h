#pragma once

#include <cstdint>
#include <vector>

struct APerformanceHintSession;
struct AThermalManager;

// Android performance APIs, tuned for sustained play on the 13R (features.md §13):
//  - ADPF performance hints: each frame's actual CPU work is reported so the kernel can pick
//    the lowest clock speed that still hits 60 fps.
//  - Thermal headroom: predicts overheating ~10 s ahead so quality can be lowered *before*
//    the phone throttles, instead of stuttering afterwards.
namespace ds13r
{

class PerfManager
{
public:
    PerfManager();
    ~PerfManager();

    // Starts a hint session for the given threads (emulation thread plus helpers).
    void StartSession(const std::vector<int32_t>& tids, int64_t targetNs);
    void SetTarget(int64_t targetNs);
    void ReportWork(int64_t actualNs);

    // Thermal. Headroom is 0..1+ (1.0 = throttling starts); -1 when unknown.
    // Polled at most once per second (the platform rate-limits it anyway).
    void PollThermal();
    float Headroom() const { return headroom; }
    int Status() const { return status; }

private:
    APerformanceHintSession* session = nullptr;
    AThermalManager* thermal = nullptr;
    float headroom = -1.0f;
    int status = 0;
    int64_t lastPoll = 0;
};

}
