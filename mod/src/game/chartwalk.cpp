#include "game/chartwalk.h"

#include <cstring>

namespace
{
    // Record sizes of the eleven node arrays, in file order.
    constexpr uint32_t kArraySizes[] = { 12, 16, 0x68, 0x94, 24, 36, 12, 96, 12, 12, 16 };
    constexpr int kArrays = sizeof kArraySizes / sizeof kArraySizes[0];
    constexpr uint32_t kArrayTable = 0x70;   // u16 offset, u8 count, u8 0, per array

    struct Reader
    {
        const uint8_t* d;
        uint32_t size, p = 0;
        bool ok = true;

        bool Skip(uint64_t n)
        {
            if (!ok || p + n > size)
                return ok = false;
            p += static_cast<uint32_t>(n);
            return true;
        }
        uint32_t Get(int n)
        {
            uint32_t v = 0;
            if (Skip(n)) memcpy(&v, d + p - n, n);
            return v;
        }
        uint32_t U32() { return Get(4); }
        uint32_t U16() { return Get(2); }
        uint32_t U8() { return Get(1); }
    };
}

namespace bm::chartwalk
{
    uint32_t Chart::U32(uint32_t at) const
    {
        uint32_t v;
        memcpy(&v, data + at, 4);
        return v;
    }

    uint16_t Chart::U16(uint32_t at) const
    {
        uint16_t v;
        memcpy(&v, data + at, 2);
        return v;
    }

    float Chart::F32(uint32_t at) const
    {
        float v;
        memcpy(&v, data + at, 4);
        return v;
    }

    int Chart::FindAction(uint32_t key) const
    {
        for (uint32_t i = 0; i < actionCount; ++i)
            if (U32(Action(i) + kActionKey) == key) return static_cast<int>(i);
        return -1;
    }

    bool Chart::Slots(int action, uint32_t& first, uint32_t& count) const
    {
        if (action < 0) return false;
        first = U16(Action(action) + kActionSlotFirst);
        count = U16(Action(action) + kActionSlotCount);
        return first + count <= slotCount;
    }

    bool Chart::Nodes(int action, uint32_t& first, uint32_t& count) const
    {
        if (action < 0) return false;
        first = U32(Action(action) + kActionNodeFirst);
        count = U16(Action(action) + kActionNodeCount);
        return first < nodes.size() && count <= nodes.size() - first;
    }

    uint32_t Chart::NodeArray(uint32_t node, int array) const
    {
        const uint32_t h = nodes[node];
        if (array < 0 || array >= kArrays || !data[h + kArrayTable + 4 * array + 2]) return 0;
        uint32_t at = h + kNodeHeader;
        for (int i = 0; i < array; ++i) at += data[h + kArrayTable + 4 * i + 2] * kArraySizes[i];
        return at;
    }

    bool Chart::EventWord(uint32_t index, uint32_t k, uint32_t& word) const
    {
        const uint64_t at = (static_cast<uint64_t>(index) + k) * 4;
        if (at + 4 > eventBytes) return false;
        word = U32(events + static_cast<uint32_t>(at));
        return true;
    }

    bool Walk(const uint8_t* data, uint32_t size, Chart& c, const char*& why)
    {
        c = Chart();
        c.data = data;
        c.size = size;
        Reader r{ data, size };
        const uint32_t nodeCount = r.U32();
        if (!r.ok || nodeCount > size / kNodeHeader)
        {
            why = "the node count does not fit the file";
            return false;
        }
        c.nodes.reserve(nodeCount);
        c.nodeEvents.reserve(nodeCount);
        for (uint32_t n = 0; n < nodeCount && r.ok; ++n)
        {
            const uint32_t h = r.p;
            if (!r.Skip(kNodeHeader)) break;
            uint64_t bytes = 0;
            for (int i = 0; i < kArrays; ++i)
            {
                if (data[h + kArrayTable + 4 * i + 3])
                {
                    why = "a node array count is over 255";
                    return false;
                }
                bytes += data[h + kArrayTable + 4 * i + 2] * kArraySizes[i];
            }
            r.Skip(bytes);
            c.nodes.push_back(h);
            c.nodeEvents.push_back(r.p);
            r.Skip(static_cast<uint64_t>(r.U8()) * kEventSize);
        }
        r.Skip(static_cast<uint64_t>(r.U32()) * 8);   // u64 list
        r.Skip(static_cast<uint64_t>(r.U32()) * 4);   // u32 lists
        r.Skip(static_cast<uint64_t>(r.U32()) * 4);
        for (int t = 0; t < 10 && r.ok; ++t)          // string tables
            for (uint32_t s = r.U16(); s && r.ok; --s) r.Skip(r.U8());
        r.Skip(18 + 22);                              // scalars, the empty reflection stub
        const uint32_t hashes = r.U16(), sockets = r.U16(), includes = r.U16(), actions = r.U16(), rec12 = r.U16();
        r.Skip(hashes * 4ull + sockets * 40ull + includes * 2ull);
        c.actions = r.p;
        c.actionCount = actions;
        r.Skip(actions * static_cast<uint64_t>(kActionSize) + rec12 * 12ull);
        const uint32_t condBytes = r.U32();
        r.Skip(condBytes);
        r.Skip(r.U32() * 2ull);                       // condition offsets
        c.branchCount = r.U32();
        c.branches = r.p;
        r.Skip(c.branchCount * static_cast<uint64_t>(kBranchSize));
        r.Skip(r.U32() * 8ull);                       // branch_rec8
        c.slotCount = r.U32();
        c.slots = r.p;
        r.Skip(c.slotCount * static_cast<uint64_t>(kSlotSize));
        r.Skip(r.U16() * 28ull);                      // key guide instances
        r.Skip(r.U32() * 8ull);                       // key guide pairs
        r.Skip(r.U32());                              // key guide event buffer
        r.Skip(r.U8() * static_cast<uint64_t>(kEventSize));
        c.eventBytes = r.U32();
        c.events = r.p;
        r.Skip(c.eventBytes);
        r.Skip(r.U8() * static_cast<uint64_t>(kEventSize));
        r.Skip(r.U32());                              // 3392's last section
        if (!r.ok || r.p != size)
        {
            why = r.ok ? "the walk ends before the file does" : "a section runs past the end of the file";
            return false;
        }
        if (condBytes % 4 || c.eventBytes % 4)
        {
            why = "a word buffer is not whole words";
            return false;
        }
        return true;
    }
}
