#pragma once

#include <vector>

// Thread placement for the Snapdragon 8 Gen 3 in the OnePlus 13R (features.md §13):
//   Prime  = 1x Cortex-X4   (3.3 GHz)  -> emulation thread
//   Mid    = 5x Cortex-A720 (3.2/3.0)  -> rendering and audio helpers
//   Little = 2x Cortex-A520 (2.3 GHz)  -> file I/O and saving
// Clusters are detected from each core's maximum frequency, so nothing is hard-coded.
namespace ds13r
{

enum class CoreClass { Prime, Mid, Little, Any };

struct CpuTopology
{
    std::vector<int> prime;
    std::vector<int> mid;
    std::vector<int> little;

    static const CpuTopology& Get();
};

// Pins the calling thread to the given core class. Returns false if the kernel refused.
bool PinCurrentThread(CoreClass cls);

// Thread IDs are needed for performance hint sessions (ADPF).
int CurrentTid();

}
