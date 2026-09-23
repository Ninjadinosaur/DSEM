#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <vector>

// Rewind history (features.md §6). Rather than keeping every snapshot whole, we keep the newest
// snapshot in full and, for each older one, only the 4 KB pages that differ from its successor
// (stored as XOR, compressed with LZ4). Consecutive states differ in a few pages, so minutes of
// history fit in modest memory, and the oldest entries can be dropped at any time.
namespace ds13r
{

class RewindBuffer
{
public:
    void SetBudget(size_t bytes) { budget = bytes; }
    void Clear();

    // Adds a new snapshot (the current emulator state).
    void Push(const uint8_t* state, size_t len);

    // Steps back one snapshot. On success `out` holds the state to load.
    bool Pop(std::vector<uint8_t>& out);

    size_t Count() const { return deltas.size() + (latest.empty() ? 0 : 1); }
    size_t MemoryUsed() const { return used + latest.size(); }

private:
    static constexpr size_t kPage = 4096;

    struct Delta
    {
        std::vector<uint32_t> pages;  // indices of the pages that changed
        std::vector<uint8_t> packed;  // LZ4 of those pages' (newer XOR older), concatenated
        size_t packedRaw = 0;         // uncompressed size of `packed`
        size_t rawSize = 0;           // full state size
    };

    std::vector<uint8_t> latest;
    std::deque<Delta> deltas; // back() = most recent
    std::vector<uint8_t> scratch;
    size_t used = 0;
    size_t budget = 512ull * 1024 * 1024;
};

}
