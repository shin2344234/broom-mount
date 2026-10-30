#pragma once
#include <cstdint>
#include <vector>

// A walk over an action chart (.paac) in the layout of exe 1.0.0.3392,
// which finds where each section and record starts so a caller can look
// records up by what they are (an action's key, the branch an action loops
// on) rather than by a fixed offset. The layout is the one the game's own
// readers take (top level +0x214A4C0, node +0x2149030, tail +0x214AE40,
// branch section +0x2149AD0), written out in private/research/newmount's
// paac_schema.py. A walk that does not end on the file's last byte fails.
// Speeder Bike has the same file; a fix to one belongs in both.
namespace bm::chartwalk
{
    // Record sizes and the fields riderfix reads.
    inline constexpr uint32_t kNodeHeader = 0xA8, kActionSize = 0x120, kBranchSize = 56, kSlotSize = 32, kEventSize = 20;
    // A node's length in seconds, and its rider group: 0 for the node every
    // rider plays, a group hash for one spliced in for one rider (Broomy's
    // Damiane nodes carry 4CB714A1).
    inline constexpr uint32_t kNodeDuration = 0x04, kNodeGroup = 0x08;
    inline constexpr uint32_t kActionKey = 0x18, kActionNodeFirst = 0x54, kActionSlotFirst = 0xE4,
                              kActionSlotCount = 0xE8, kActionNodeCount = 0xEE;
    inline constexpr uint32_t kBranchFade = 0x08, kBranchTarget = 0x14, kBranchKind = 0x1C, kBranchFlags = 0x1D,
                              kBranchCondCount = 0x2A;
    inline constexpr uint32_t kSlotBranch = 0x0C;
    // A frame event instance: time, time_end, then the index of its entry's
    // first word in the frame event buffer.
    inline constexpr uint32_t kEventTime = 0x00, kEventTimeEnd = 0x04, kEventIndex = 0x08;

    struct Chart
    {
        const uint8_t* data = nullptr;
        uint32_t size = 0;
        std::vector<uint32_t> nodes;        // each node's header
        std::vector<uint32_t> nodeEvents;   // each node's u8 frame event instance count
        uint32_t actions = 0, actionCount = 0;
        uint32_t branches = 0, branchCount = 0;
        uint32_t slots = 0, slotCount = 0;
        uint32_t events = 0, eventBytes = 0;   // the chart's frame event buffer

        uint32_t U32(uint32_t at) const;
        uint16_t U16(uint32_t at) const;
        uint8_t U8(uint32_t at) const { return data[at]; }
        float F32(uint32_t at) const;

        uint32_t Action(uint32_t i) const { return actions + i * kActionSize; }
        uint32_t Branch(uint32_t i) const { return branches + i * kBranchSize; }
        uint32_t Slot(uint32_t i) const { return slots + i * kSlotSize; }
        // The action whose key is `key`, or -1.
        int FindAction(uint32_t key) const;
        // An action's slot and node runs, checked against the section sizes.
        bool Slots(int action, uint32_t& first, uint32_t& count) const;
        bool Nodes(int action, uint32_t& first, uint32_t& count) const;
        // The first record of node array `array` (0 frame tags, 1 play rates)
        // of node `node`, or 0 when the array is empty.
        uint32_t NodeArray(uint32_t node, int array) const;
        // A frame event entry's word `k` (0 holds the type in its low half),
        // or false past the buffer.
        bool EventWord(uint32_t index, uint32_t k, uint32_t& word) const;
    };

    // Fills `c`; false, with a reason, when the bytes are not a chart of this
    // layout.
    bool Walk(const uint8_t* data, uint32_t size, Chart& c, const char*& why);
}
