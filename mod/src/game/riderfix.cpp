#include "game/riderfix.h"

#include <Windows.h>
#include <cstring>

#include "core/log.h"
#include "game/damiane.h"
#include "game/farhook.h"
#include "game/gamefile.h"
#include "game/mem.h"

namespace
{
    // The branch check: (chart component, u32* error, u8 layer, ..., 8th
    // argument a pointer whose first field is the branch record). The error
    // is 0 when the branch may be taken. The record's target action hash is
    // at +0x14.
    constexpr uintptr_t kRva_BranchCheck = 0x232E3D0;
    constexpr uint8_t kBranchCheckHead[] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x4C, 0x89, 0x48, 0x20, 0x44,
                                             0x88, 0x40, 0x18, 0x48, 0x89, 0x48, 0x08 };

    typedef uint64_t (*FnCheck)(uintptr_t, uint32_t*, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                                uintptr_t, uintptr_t, uintptr_t, uintptr_t);
    FnCheck g_original = nullptr;

    struct Last { uintptr_t comp; uint8_t layer; uint32_t target; };
    constexpr int kSlots = 64;
    Last g_last[kSlots] = {};
    int g_used = 0;
    SRWLOCK g_lock = SRWLOCK_INIT;
    volatile LONG g_lines = 0;
    bool g_log = false;

    // The rider fix: the two default branches of Kliff's lower dispatchers,
    // by their offset in ride_test3_lower.paac, with the shipped word and the
    // one they hold while Broomy is ridden. +0x14 is the target, +0x08 the
    // crossfade in frames (f32). Shipped, the branches cut (-1) between two
    // dragon poses that play the same clip; between the broom's ground and
    // air states they crossfade over 10 frames, as Kliff's upper chart does.
    struct Swap { uint32_t offset; uint32_t shipped; uint32_t broomy; };
    constexpr uint32_t kCut = 0xBF800000, kTenFrames = 0x41200000;
    constexpr Swap kSwaps[] = {
        { 0x6F3F1 + 0x14, 0x07407E6A, 0x588C2001 },   // branch 397, ground states -> broom idle
        { 0x6F3F1 + 0x08, kCut, kTenFrames },
        // The air states also take the ground idle. AD3E621B is the same
        // action in this chart (the same blend, the same branches), and
        // switching to it at takeoff restarted the blend, a hitch.
        { 0x6F48D + 0x14, 0x90337E5C, 0x588C2001 },   // branch 400, air states -> broom idle
        { 0x6F48D + 0x08, kCut, kTenFrames },
        // The push-off. Kliff's lower layer takes the mount's action key, so
        // the broom's takeoff runs Kliff's 117F8F51, whose default slot 934
        // shares branch 397 with seven other states. Slot 934 alone moves to
        // branch 439, rewritten as 397 into FDA30B13 with a 3 frame fade.
        // FDA30B13 is a spare Kliff action no branch or chart reaches; it
        // plays the push-off under a name of its own,
        // cd_phm_rd_broom_basic_00_00_nor_std_takeoff_00 (Seen below writes
        // it into the chart, and broomclips.h has the files), and lasts the
        // clip's 136 frames. Kliff's lower chart moves on only when a broom
        // chart commands it, never on anim_end; the flight start commands it
        // (crossfadepatches.h, b27), so the clip's round of broom idle after
        // the 36 frame kick is only a margin. Slot 1036 points at branch 671
        // in case the end is ever reached. Branch 439 had only slot 1036,
        // FDA30B13 looping on itself. Seth, 3 October: "his feet touched the
        // ground".
        { 0x796B4, 0x00018D7F, 0x0001B77F },          // slot 934 (u16 at +1): branch 397 -> 439
        { 0x6FC79 + 0x08, 0x3F800000, 0x40400000 },   // branch 439 crossfade 1 -> 3 frames (the feet land at frame 6)
        { 0x6FC79 + 0x1C, 0x00000007, 0x00000204 },   // branch 439 kind, as 397
        { 0x6FC79 + 0x24, 0x0000001D, 0x00000000 },   // branch 439 condition, as 397
        { 0x6FC79 + 0x28, 0x00010000, 0x00000000 },
        { 0x7A1DC, 0x0001B77F, 0x00029F7F },          // slot 1036 (u16 at +1): branch 439 -> 671
        { 0x1D4D0, 0x3F19999A, 0x40911111 },          // FDA30B13 duration 0.6 s -> 4.53 s (136 frames)
    };
    constexpr DWORD kRiddenMs = 3000;
    constexpr uint32_t kPushOffAt = 0x31339;   // string 193, after its length byte (80)
    constexpr char kPushOffOld[] = "1_pc/1_phm/00_riding/cd_phm_rd_wyvern_basic_00_00_air_move_fall_walk_end_00.paa";
    constexpr char kPushOffNew[] = "1_pc/1_phm/00_riding/cd_phm_rd_broom_basic_00_00_nor_std_takeoff_00.paa";
    static_assert(sizeof kPushOffNew <= sizeof kPushOffOld, "the new name must fit the old one's room");
    constexpr size_t kSwapCount = sizeof kSwaps / sizeof kSwaps[0];

    // Kliff's left hand IK. The node of 01B8F8D3 (the cut orca rider) fires
    // frame event 777 at 0 s, type 0x65, which switches on the LimbIK's left
    // hand (posemodifierdata.xml, limb "lefthand") aimed at the mount's
    // B_IK_L_Hand. The broom skeleton has no such bone, so on a broom-skeleton
    // mount his left fist goes to the seat at the front of his belt, whatever
    // either layer plays. Found on the Speeder Bike, which rides on this
    // chart (5 October). The node's instance of the event fires at 1000 s
    // instead, past the end of every clip, so it never fires; the event
    // itself is shared by 77 states and stays as it is. No shipped mount
    // reaches 01B8F8D3.
    constexpr uint32_t kIkAt = 0x216BF;   // the instance record: time, time_end, event index
    constexpr uint32_t kIkWords[] = { 0x00000000, 0xBF800000, 777 };
    constexpr uint32_t kNever = 0x447A0000;   // 1000.0f
    // Each swap's offset in the lower chart the game has: the shipped file,
    // or the one with Damiane's nodes (broomy.cpp).
    uint32_t g_at[kSwapCount] = {};
    volatile uintptr_t g_lowerBase = 0;
    volatile uint32_t g_lowerSize = 0;
    volatile uintptr_t g_rideOnBase = 0;
    volatile uint32_t g_rideOnSize = 0;
    volatile LONG g_rideOnAt = 0;
    volatile LONG g_swapped = 0;   // 1 while the broom targets are in
    SRWLOCK g_swapLock = SRWLOCK_INIT;

    bool In(uintptr_t a, uintptr_t base, uint32_t size) { return base && a >= base && a < base + size; }

    void SetSwap(bool on)
    {
        AcquireSRWLockExclusive(&g_swapLock);
        const uintptr_t base = g_lowerBase;
        if (base && (g_swapped != 0) != on)
        {
            bool ok = true;
            for (size_t i = 0; i < kSwapCount; ++i)
            {
                const Swap& w = kSwaps[i];
                uint32_t* at = reinterpret_cast<uint32_t*>(base + g_at[i]);
                const uint32_t want = on ? w.broomy : w.shipped, was = on ? w.shipped : w.broomy;
                if (*at == was) *at = want;
                else if (*at != want) ok = false;
            }
            InterlockedExchange(&g_swapped, on ? 1 : 0);
            LOG(ok ? "[rider] Kliff's lower chart %s." : "[rider] Kliff's lower chart %s, but a branch held other bytes.",
                on ? "takes his broom states (Broomy is ridden)" : "is back to its shipped targets");
        }
        ReleaseSRWLockExclusive(&g_swapLock);
    }

    uint64_t CheckDetour(uintptr_t comp, uint32_t* err, uintptr_t layer, uintptr_t a4, uintptr_t a5, uintptr_t a6,
                         uintptr_t a7, uintptr_t a8, uintptr_t a9, uintptr_t a10, uintptr_t a11, uintptr_t a12)
    {
        uintptr_t pre = 0;
        if (a8 && bm::mem::ReadPtr(a8, &pre))
        {
            if (In(pre, g_rideOnBase, g_rideOnSize)) InterlockedExchange(&g_rideOnAt, static_cast<LONG>(GetTickCount()));
            else if (In(pre, g_lowerBase, g_lowerSize))
            {
                const LONG at = g_rideOnAt;
                const bool ridden = at && GetTickCount() - static_cast<DWORD>(at) < kRiddenMs;
                if (ridden != (g_swapped != 0)) SetSwap(ridden);
            }
        }
        const uint64_t r = g_original(comp, err, layer, a4, a5, a6, a7, a8, a9, a10, a11, a12);
        if (!g_log) return r;
        uintptr_t rec = 0;
        uint32_t code = 1, target = 0;
        if (!a8 || !err || !bm::mem::Read32(reinterpret_cast<uintptr_t>(err), &code) || code != 0) return r;
        if (!bm::mem::ReadPtr(a8, &rec) || !bm::mem::Read32(rec + 0x14, &target)) return r;
        const uint8_t l = static_cast<uint8_t>(layer);
        bool fresh = false;
        AcquireSRWLockExclusive(&g_lock);
        int i = 0;
        while (i < g_used && !(g_last[i].comp == comp && g_last[i].layer == l)) ++i;
        if (i == g_used && g_used < kSlots) g_last[g_used++] = { comp, l, 0 };
        if (i < g_used && g_last[i].target != target)
        {
            g_last[i].target = target;
            fresh = true;
        }
        ReleaseSRWLockExclusive(&g_lock);
        if (fresh && InterlockedIncrement(&g_lines) <= 20000)
            LOG("[state] comp %p layer %u -> %08X (record %p)", reinterpret_cast<void*>(comp), l, target,
                reinterpret_cast<void*>(rec));
        return r;
    }

    // Where a word of the shipped lower chart sits in the one with Damiane's
    // nodes (damiane.h).
    uint32_t Moved(uint32_t offset) { return bm::damiane::Moved(bm::damiane::kCharts[1], offset); }

    // Whether the chart holds every swap's shipped word where the layout puts
    // it; fills `at` with the offsets.
    bool Fits(const uint8_t* data, uint32_t size, bool damiane, uint32_t* at)
    {
        for (size_t i = 0; i < kSwapCount; ++i)
        {
            at[i] = damiane ? Moved(kSwaps[i].offset) : kSwaps[i].offset;
            if (at[i] + 4 > size || *reinterpret_cast<const uint32_t*>(data + at[i]) != kSwaps[i].shipped) return false;
        }
        return true;
    }

    void Seen(const char* path, const uint8_t* data, uint32_t size)
    {
        const size_t n = strlen(path);
        if (n < 5 || _stricmp(path + n - 5, ".paac") != 0) return;
        if (!strstr(path, "ride") && !strstr(path, "riding")) return;
        if (g_log) LOG("[state] chart %s at %p (%u bytes)", path, data, size);
        if (!strstr(path, "/ride_test3_lower.paac")) return;
        static_assert(sizeof bm::damiane::kCharts / sizeof bm::damiane::kCharts[0] == 2, "kCharts[1] is the lower chart");
        uint32_t at[kSwapCount];
        const bool shipped = Fits(data, size, false, at);
        const bool damiane = !shipped && Fits(data, size, true, at);
        const bool known = shipped || damiane;
        AcquireSRWLockExclusive(&g_swapLock);
        g_lowerBase = known ? reinterpret_cast<uintptr_t>(data) : 0;
        g_lowerSize = size;
        if (known) memcpy(g_at, at, sizeof at);
        InterlockedExchange(&g_swapped, 0);
        ReleaseSRWLockExclusive(&g_swapLock);
        if (!known) LOG_ERR("[rider] ride_test3_lower.paac is not the shipped file, so Kliff keeps the dragon pose.");
        const uint32_t pushOffAt = damiane ? Moved(kPushOffAt) : kPushOffAt;
        // The push-off's own name. FDA30B13 plays the clip at string 193;
        // the chart hashes its paths as it parses, which is after this read,
        // so the name is written here, padded with zeros inside the old
        // string's room as broomchart.cpp does. Nothing reaches FDA30B13
        // unless Broomy is ridden, so it keeps the new name throughout.
        if (known && pushOffAt + sizeof kPushOffOld <= size &&
            memcmp(data + pushOffAt, kPushOffOld, sizeof kPushOffOld) == 0)
        {
            uint8_t* name = const_cast<uint8_t*>(data) + pushOffAt;
            memset(name, 0, sizeof kPushOffOld);
            memcpy(name, kPushOffNew, sizeof kPushOffNew - 1);
            LOG("[rider] Kliff's push-off plays as %s%s.", kPushOffNew,
                damiane ? ", in the lower chart with Damiane's nodes" : "");
        }
        const uint32_t ikAt = damiane ? Moved(kIkAt) : kIkAt;
        if (known && ikAt + sizeof kIkWords <= size && memcmp(data + ikAt, kIkWords, sizeof kIkWords) == 0)
        {
            memcpy(const_cast<uint8_t*>(data) + ikAt, &kNever, 4);
            LOG("[rider] 01B8F8D3's left hand IK event stays off%s.",
                damiane ? ", in the lower chart with Damiane's nodes" : "");
        }
        else if (known)
            LOG_ERR("[rider] 01B8F8D3's hand IK event is not where it should be, so Kliff's left hand may go to his belt.");
    }
}

namespace bm::riderfix
{
    void NoteServed(const char* path, uintptr_t base, uint32_t size)
    {
        if (!strstr(path, "m0002_armadillo.paac")) return;
        g_rideOnSize = size;
        g_rideOnBase = base;
    }

    bool Install(bool logStates)
    {
        g_log = logStates;
        const uintptr_t at = bm::mem::Game().base + kRva_BranchCheck;
        uint8_t head[sizeof kBranchCheckHead] = {};
        if (!bm::mem::ReadBytes(at, head, sizeof head) || memcmp(head, kBranchCheckHead, sizeof head) != 0)
        {
            LOG_ERR("[rider] the branch check is not at +0x%llX on this exe.", static_cast<unsigned long long>(kRva_BranchCheck));
            return false;
        }
        char why[160] = {};
        if (!bm::farhook::Install("branch check", at, reinterpret_cast<void*>(&CheckDetour),
                              reinterpret_cast<void**>(&g_original), why, sizeof why))
        {
            LOG_ERR("[rider] could not hook the branch check: %s", why);
            return false;
        }
        bm::gamefile::Observe(&Seen);
        LOG(logStates ? "[rider] Kliff rides Broomy in his broom poses; each chart layer's next action is logged."
                      : "[rider] Kliff rides Broomy in his broom poses.");
        return true;
    }
}
