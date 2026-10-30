#include "game/riderfind.h"

#include <cstring>

#include "game/chartwalk.h"

namespace
{
    using bm::chartwalk::Chart;
    namespace cw = bm::chartwalk;
    using bm::riderfind::Swap;

    // Action keys, the same on every build.
    constexpr uint32_t kGroundDispatch = 0x07407E6A, kAirDispatch = 0x90337E5C;
    constexpr uint32_t kOrcaRider = 0x01B8F8D3;   // the cut orca rider's state, lower chart
    constexpr uint32_t kTakeoff = 0x117F8F51, kSpare = 0xFDA30B13;
    constexpr uint32_t kBroomIdle = 0x588C2001;
    constexpr uint32_t kRideOnIdle = 0xF5150415;
    constexpr uint32_t kCut = 0xBF800000, kTenFrames = 0x41200000, kOneFrame = 0x3F800000, kThreeFrames = 0x40400000;
    constexpr uint32_t kSpareLength = 0x3F19999A, k136Frames = 0x40911111;   // 0.6 s, 4.53 s
    constexpr char kPushOff[] = "1_pc/1_phm/00_riding/cd_phm_rd_wyvern_basic_00_00_air_move_fall_walk_end_00.paa";
    // What riderfix, Broomy's and the speeder's alike, writes once as the
    // chart loads: the push-off's own name, padded with zeros in the old
    // one's room, and 1000 s in each hand IK instance's time. With both mods
    // installed the second to see the chart finds them already written, so
    // the search takes either.
    constexpr char kPushOffBroom[] = "1_pc/1_phm/00_riding/cd_phm_rd_broom_basic_00_00_nor_std_takeoff_00.paa";
    constexpr uint32_t kNever = 0x447A0000;   // 1000.0f
    static_assert(sizeof kPushOffBroom <= sizeof kPushOff, "the new name fits the old one's room");

    bool PushOffAt(const uint8_t* data, uint32_t at)
    {
        if (memcmp(data + at, kPushOff, sizeof kPushOff) == 0) return true;
        if (memcmp(data + at, kPushOffBroom, sizeof kPushOffBroom - 1) != 0) return false;
        for (uint32_t i = sizeof kPushOffBroom - 1; i < sizeof kPushOff; ++i)
            if (data[at + i]) return false;
        return true;
    }

    // Kliff's left hand IK: a frame event of type 0x67 (0x65 on 2976) whose
    // word 4 names the LimbIK's left hand, FE419E03 "lefthand"
    // (posemodifierdata.xml).
    constexpr uint32_t kIkType = 0x67, kIkLimbWord = 4, kLeftHand = 0xFE419E03;

    constexpr uint32_t kNone = 0xFFFFFFFFu;

    // The distinct keys of the actions with a slot on branch `b`, at most
    // `max` of them; returns how many there are, counted past `max`.
    int Owners(const Chart& c, uint32_t b, uint32_t* keys, int max)
    {
        int n = 0;
        for (uint32_t a = 0; a < c.actionCount; ++a)
        {
            uint32_t first, count;
            if (!c.Slots(static_cast<int>(a), first, count)) continue;
            for (uint32_t s = first; s < first + count; ++s)
                if (c.U16(c.Slot(s) + cw::kSlotBranch) == b)
                {
                    const uint32_t key = c.U32(c.Action(a) + cw::kActionKey);
                    bool seen = false;
                    for (int i = 0; i < n && i < max; ++i) seen = seen || keys[i] == key;
                    if (!seen)
                    {
                        if (n < max) keys[n] = key;
                        ++n;
                    }
                    break;
                }
        }
        return n;
    }

    // The one slot of action `key` whose branch passes `test`, or kNone when
    // there is none or more than one.
    template <class Test> uint32_t OneSlot(const Chart& c, uint32_t key, Test test)
    {
        uint32_t first, count, found = kNone;
        if (!c.Slots(c.FindAction(key), first, count)) return kNone;
        for (uint32_t s = first; s < first + count; ++s)
        {
            const uint32_t b = c.U16(c.Slot(s) + cw::kSlotBranch);
            if (b >= c.branchCount || !test(b)) continue;
            if (found != kNone) return kNone;
            found = s;
        }
        return found;
    }

    // The default branch of a lower dispatcher: a cut (-1 frames) to it, of
    // kind 4 with flag 2 and no condition.
    uint32_t Dispatcher(const Chart& c, uint32_t target)
    {
        uint32_t found = kNone;
        for (uint32_t b = 0; b < c.branchCount; ++b)
        {
            const uint32_t r = c.Branch(b);
            if (c.U32(r + cw::kBranchTarget) != target || c.U32(r + cw::kBranchFade) != kCut ||
                c.U8(r + cw::kBranchKind) != 4 || !(c.U8(r + cw::kBranchFlags) & 2) || c.U8(r + cw::kBranchCondCount))
                continue;
            if (found != kNone) return kNone;
            found = b;
        }
        return found;
    }

    // A slot's branch index, u16 at +0x0C, as the u32 that starts a byte
    // before it, the way riderfix swaps words.
    Swap SlotSwap(const Chart& c, uint32_t slot, uint32_t to)
    {
        const uint32_t at = c.Slot(slot) + cw::kSlotBranch - 1, w = c.U32(at);
        return { at, w, (w & ~0x00FFFF00u) | (to << 8) };
    }

    bool Fail(const char*& why, const char* what)
    {
        why = what;
        return false;
    }

    // The one node of an action that every rider plays (group 0). Broomy
    // splices a node for Damiane (group 4CB714A1) into some of Kliff's
    // actions beside it; the shipped charts have one node there.
    bool CommonNode(const Chart& c, int action, uint32_t& node)
    {
        uint32_t first, count, found = 0, n = 0;
        if (!c.Nodes(action, first, count)) return false;
        for (uint32_t i = first; i < first + count; ++i)
            if (c.U32(c.nodes[i] + cw::kNodeGroup) == 0) found = i, ++n;
        node = found;
        return n == 1;
    }
}

namespace bm::riderfind
{
    bool FindLower(const uint8_t* data, uint32_t size, Lower& out, const char*& why)
    {
        Chart c;
        if (!cw::Walk(data, size, c, why)) return false;
        // The two default branches of Kliff's lower dispatchers: 397 and 400
        // on 2976, 412 and 415 on 3392.
        const uint32_t ground = Dispatcher(c, kGroundDispatch), air = Dispatcher(c, kAirDispatch);
        if (ground == kNone || air == kNone) return Fail(why, "the dispatchers' default branches are not there once each");
        // The takeoff's slot on the ground branch (934 on 2976).
        const uint32_t takeoffSlot = OneSlot(c, kTakeoff, [&](uint32_t b) { return b == ground; });
        if (takeoffSlot == kNone) return Fail(why, "117F8F51 has no one slot on the ground branch");
        // The spare action FDA30B13: its common node, and its slot whose
        // branch loops on itself at the clip's end (slot 1036, branch 439 on
        // 2976), which no other slot uses.
        const int spare = c.FindAction(kSpare);
        uint32_t node;
        if (!CommonNode(c, spare, node)) return Fail(why, "FDA30B13 has no one common node");
        const uint32_t loopSlot =
            OneSlot(c, kSpare, [&](uint32_t b) { return c.U32(c.Branch(b) + cw::kBranchTarget) == kSpare; });
        if (loopSlot == kNone) return Fail(why, "FDA30B13 has no one loop");
        const uint32_t loop = c.U16(c.Slot(loopSlot) + cw::kSlotBranch);
        uint32_t users = 0;
        for (uint32_t s = 0; s < c.slotCount; ++s) users += c.U16(c.Slot(s) + cw::kSlotBranch) == loop;
        if (c.U8(c.Branch(loop) + cw::kBranchKind) != 7 || users != 1)
            return Fail(why, "FDA30B13's loop is not a clip end of its own");
        // The clip-end exit to the broom idle that most states share (branch
        // 671 on 2976, 14 states).
        uint32_t exit = kNone;
        int most = 0, next = 0;
        for (uint32_t b = 0; b < c.branchCount; ++b)
        {
            const uint32_t r = c.Branch(b);
            if (c.U8(r + cw::kBranchKind) != 7 || c.U32(r + cw::kBranchTarget) != kBroomIdle) continue;
            uint32_t keys[1];
            const int n = Owners(c, b, keys, 0);
            if (n > most) next = most, most = n, exit = b;
            else if (n > next) next = n;
        }
        if (exit == kNone || most == next) return Fail(why, "no clip end to the broom idle is shared most");
        const uint32_t spareLength = c.nodes[node] + cw::kNodeDuration;
        if (c.U32(spareLength) != kSpareLength) return Fail(why, "FDA30B13 does not last 0.6 s");
        // The push-off clip's path, once in the file.
        const uint32_t len = sizeof kPushOff;
        uint32_t pushOff = kNone;
        for (uint32_t at = 1; at + len <= size; ++at)
            if (data[at - 1] == len && data[at] == '1' && PushOffAt(data, at))
            {
                if (pushOff != kNone) return Fail(why, "the push-off path is there twice");
                pushOff = at;
            }
        if (pushOff == kNone) return Fail(why, "the push-off path is not there");
        // 01B8F8D3's hand IK instances: at 0 s (or 1000 s, written already),
        // open-ended.
        uint32_t orca, orcaNodes;
        if (!c.Nodes(c.FindAction(kOrcaRider), orca, orcaNodes)) return Fail(why, "01B8F8D3 is not there");
        out.ikCount = 0;
        for (uint32_t n = orca; n < orca + orcaNodes; ++n)
        {
            const uint32_t at = c.nodeEvents[n];
            for (uint32_t i = 0; i < c.U8(at); ++i)
            {
                const uint32_t e = at + 1 + i * cw::kEventSize;
                uint32_t type, limb;
                if (!c.EventWord(c.U32(e + cw::kEventIndex), 0, type) || (type & 0xFFFF) != kIkType ||
                    (c.U32(e + cw::kEventTime) != 0 && c.U32(e + cw::kEventTime) != kNever) ||
                    c.U32(e + cw::kEventTimeEnd) != kCut)
                    continue;
                if (!c.EventWord(c.U32(e + cw::kEventIndex), kIkLimbWord, limb) || limb != kLeftHand)
                    return Fail(why, "01B8F8D3 has a limb IK event for another limb");
                if (out.ikCount == kIkMax) return Fail(why, "01B8F8D3 has too many hand IK events");
                out.ik[out.ikCount++] = e + cw::kEventTime;
            }
        }
        if (!out.ikCount) return Fail(why, "01B8F8D3 has no hand IK event");

        const uint32_t g = c.Branch(ground), a = c.Branch(air), l = c.Branch(loop);
        const Swap swaps[kLowerSwaps] = {
            // Shipped, the branches cut between two dragon poses that play
            // the same clip; between the broom's ground and air states they
            // crossfade over 10 frames, as Kliff's upper chart does. The air
            // states take the ground idle too: AD3E621B is the same action in
            // this chart, and switching to it at takeoff restarted the blend.
            { g + cw::kBranchTarget, kGroundDispatch, kBroomIdle },     // ground states -> the broom idle
            { g + cw::kBranchFade, kCut, kTenFrames },
            { a + cw::kBranchTarget, kAirDispatch, kBroomIdle },        // air states -> the broom idle
            { a + cw::kBranchFade, kCut, kTenFrames },
            SlotSwap(c, takeoffSlot, loop),                             // the takeoff's slot -> the loop branch
            { l + cw::kBranchFade, c.U32(l + cw::kBranchFade), kThreeFrames },   // 1 -> 3 frames (the feet land at frame 6)
            // The loop branch's kind, condition and counts, as the ground
            // branch has them, so it is taken the same way.
            { l + 0x1C, c.U32(l + 0x1C), c.U32(g + 0x1C) },
            { l + 0x24, c.U32(l + 0x24), c.U32(g + 0x24) },
            { l + 0x28, c.U32(l + 0x28), c.U32(g + 0x28) },
            SlotSwap(c, loopSlot, exit),                                // FDA30B13's loop slot -> the shared exit
            { spareLength, kSpareLength, k136Frames },                  // FDA30B13 lasts 136 frames
        };
        if (c.U32(l + cw::kBranchFade) != kOneFrame) return Fail(why, "FDA30B13's loop does not fade over 1 frame");
        memcpy(out.swaps, swaps, sizeof swaps);
        out.pushOff = pushOff;
        out.ground = ground, out.air = air, out.takeoffSlot = takeoffSlot, out.loop = loop, out.loopSlot = loopSlot,
        out.exit = exit;
        return true;
    }

    bool FindRideOnIdle(const uint8_t* data, uint32_t size, uint32_t* at, int& count, const char*& why)
    {
        count = 0;
        Chart c;
        if (!cw::Walk(data, size, c, why)) return false;
        uint32_t first, n;
        if (!c.Slots(c.FindAction(kRideOnIdle), first, n)) return Fail(why, "F5150415 is not there");
        for (uint32_t s = first; s < first + n; ++s)
        {
            const uint32_t b = c.U16(c.Slot(s) + cw::kSlotBranch);
            if (b >= c.branchCount || count == kIdleMax) return Fail(why, "F5150415's branches do not fit");
            at[count++] = c.Branch(b);
        }
        return count > 0 || Fail(why, "F5150415 has no branch");
    }
}
