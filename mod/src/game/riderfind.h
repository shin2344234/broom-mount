#pragma once
#include <cstdint>

// The words riderfix edits in Kliff's lower chart, found by what they are
// (an action's key, the branch an action loops on) as the chart loads, so the
// same search works on the shipped chart, on the one with Damiane's nodes
// spliced in, and on the next build. Speeder Bike's riderfind.cpp, with
// Broomy's broom idle as the target where the speeder's has its own idle.
// private/research/newmount/riderfix_offsets.py finds the same words in
// Python, by another route (paac_schema and branchmap); `build.bat riderfind
// CHARTS SPLICED` checks that both agree.
namespace bm::riderfind
{
    // A word of a chart: its offset, the shipped value and the one riderfix
    // writes.
    struct Swap { uint32_t offset, shipped, now; };

    // ride_test3_lower.paac. `swaps` go in while Broomy is ridden and come
    // out after; the hand IK instances (their time word) and the push-off
    // path (after its length byte) change once, as the chart loads.
    constexpr int kLowerSwaps = 11, kIkMax = 4;
    struct Lower
    {
        Swap swaps[kLowerSwaps];
        uint32_t ik[kIkMax];
        int ikCount;
        uint32_t pushOff;
        uint32_t ground, air, takeoffSlot, loop, loopSlot, exit;   // the indexes, for the log and the check
    };
    bool FindLower(const uint8_t* data, uint32_t size, Lower& out, const char*& why);

    // A RideOn chart built from the Wyvern's m0004_ride_dragon_upper.paac
    // (Broomy's and the speeder's): the branches of its ground idle F5150415,
    // by offset.
    constexpr int kIdleMax = 8;
    bool FindRideOnIdle(const uint8_t* data, uint32_t size, uint32_t* at, int& count, const char*& why);
}
