#include "RewindBuffer.h"

#include "lz4.h"

#include <cstring>

namespace ds13r
{

namespace
{
void XorInto(uint8_t* dst, const uint8_t* a, const uint8_t* b, size_t len)
{
    size_t words = len / 8;
    auto* d = reinterpret_cast<uint64_t*>(dst);
    auto* x = reinterpret_cast<const uint64_t*>(a);
    auto* y = reinterpret_cast<const uint64_t*>(b);
    for (size_t i = 0; i < words; i++) d[i] = x[i] ^ y[i];
    for (size_t i = words * 8; i < len; i++) dst[i] = a[i] ^ b[i];
}
}

void RewindBuffer::Clear()
{
    latest.clear();
    deltas.clear();
    used = 0;
}

void RewindBuffer::Push(const uint8_t* state, size_t len)
{
    if (!latest.empty() && latest.size() != len)
    {
        // State layout changed (e.g. a different console mode); older history is unusable.
        Clear();
    }

    if (latest.empty())
    {
        latest.assign(state, state + len);
        return;
    }

    // Only pages that changed since the previous snapshot are stored, as (new XOR old).
    // Consecutive snapshots differ in a small fraction of pages, so this is far cheaper than
    // diffing and compressing the whole state (19 MB on the DS) every time.
    Delta delta;
    delta.rawSize = len;
    scratch.clear();
    size_t pages = (len + kPage - 1) / kPage;
    for (size_t p = 0; p < pages; p++)
    {
        size_t off = p * kPage;
        size_t n = std::min(kPage, len - off);
        if (memcmp(state + off, latest.data() + off, n) == 0) continue;
        delta.pages.push_back((uint32_t)p);
        size_t at = scratch.size();
        scratch.resize(at + n);
        XorInto(scratch.data() + at, state + off, latest.data() + off, n);
        memcpy(latest.data() + off, state + off, n);
    }

    if (!delta.pages.empty())
    {
        delta.packed.resize((size_t)LZ4_compressBound((int)scratch.size()));
        int n = LZ4_compress_default(reinterpret_cast<const char*>(scratch.data()), reinterpret_cast<char*>(delta.packed.data()),
                                     (int)scratch.size(), (int)delta.packed.size());
        if (n <= 0)
        {
            Clear();
            latest.assign(state, state + len);
            return;
        }
        delta.packed.resize((size_t)n);
        delta.packed.shrink_to_fit();
    }
    delta.packedRaw = scratch.size();
    delta.pages.shrink_to_fit();
    used += delta.packed.size() + delta.pages.size() * sizeof(uint32_t);
    deltas.push_back(std::move(delta));

    while (used + latest.size() > budget && !deltas.empty())
    {
        used -= deltas.front().packed.size() + deltas.front().pages.size() * sizeof(uint32_t);
        deltas.pop_front();
    }
}

bool RewindBuffer::Pop(std::vector<uint8_t>& out)
{
    if (deltas.empty()) return false;

    Delta& delta = deltas.back();
    if (!delta.pages.empty())
    {
        scratch.resize(delta.packedRaw);
        int n = LZ4_decompress_safe(reinterpret_cast<const char*>(delta.packed.data()), reinterpret_cast<char*>(scratch.data()),
                                    (int)delta.packed.size(), (int)delta.packedRaw);
        if (n != (int)delta.packedRaw)
        {
            Clear();
            return false;
        }

        // older = newer XOR delta, page by page
        size_t at = 0;
        for (uint32_t p : delta.pages)
        {
            size_t off = (size_t)p * kPage;
            size_t len = std::min(kPage, latest.size() - off);
            XorInto(latest.data() + off, latest.data() + off, scratch.data() + at, len);
            at += len;
        }
    }

    used -= delta.packed.size() + delta.pages.size() * sizeof(uint32_t);
    deltas.pop_back();

    out = latest;
    return true;
}

}
