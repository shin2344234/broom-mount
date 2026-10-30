#include "game/riderfix.h"

#include <Windows.h>
#include <cstring>

#include "core/log.h"
#include "game/farhook.h"
#include "game/gamefile.h"
#include "game/mem.h"
#include "game/riderfind.h"

namespace
{
    // The branch check: (chart component, u32* error, u8 layer, ..., 8th
    // argument a pointer whose first field is the branch record). The error
    // is 0 when the branch may be taken. The record's target action hash is
    // at +0x14.
    // It is found by its head: +0x25152D0 on 3392, +0x2516560 on 3424.
    constexpr char kSig_BranchCheck[] = "48 8B C4 48 89 58 10 4C 89 48 20 44 88 40 18 48 89 48 08";

    typedef uint64_t (*FnCheck)(uintptr_t, uint32_t*, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                                uintptr_t, uintptr_t, uintptr_t, uintptr_t);
    FnCheck g_original = nullptr;

    struct Last { uintptr_t comp; uint8_t layer; uint32_t target; };
    constexpr int kSlots = 64;
    // Test builds: a chart component is followed, every layer of it, from
    // the first time one of its branches goes to a broom mount, an escort
    // mount, a broom idle, the carry idles (a captive or a goat on the
    // shoulder) or flight. Following the first 64 components the game
    // checked would leave Kliff out once the world had loaded.
    // Broomy's body chart (the Wyvern's m0004_dragon_upper) is followed from
    // its fly idles, ground idles and landings, for arrivals in the air.
    constexpr uint32_t kWatched[] = { 0x262C63D3, 0xA272684B, 0x33F65149, 0x4DC25870, 0x588C2001, 0xAD3E621B,
                                      0xE75F3861, 0x0F03B042, 0xE066175D, 0xD54B3D54, 0x5478F08E, 0x23E7F150,
                                      0xD28EB240, 0xCB1CB87C, 0x622E5BEC, 0x05CC1CFD, 0xE11DA708, 0x471C9D06,
                                      0x887F58A8, 0x1DA64383, 0xDCBF95FE, 0x200C5591, 0x7259DAC6, 0x9211B380,
                                      0x968C694E };
    bool Watched(uint32_t target)
    {
        for (uint32_t w : kWatched)
            if (w == target) return true;
        return false;
    }
    Last g_last[kSlots] = {};
    int g_used = 0;
    SRWLOCK g_lock = SRWLOCK_INIT;
    volatile LONG g_lines = 0;
    bool g_log = false;

    // The rider fix: words of Kliff's lower chart, ride_test3_lower.paac,
    // found by what they are as the chart loads (riderfind.cpp), so the same
    // search works on the shipped chart and on the one with Damiane's nodes
    // (broomy.cpp). Each has its shipped word and the one it holds while
    // Broomy is ridden:
    // - The two default branches of Kliff's lower dispatchers (412 and 415
    //   on 3392) go to the broom idle with a 10 frame crossfade.
    // - The push-off. Kliff's lower layer takes the mount's action key, so
    //   the broom's takeoff runs Kliff's 117F8F51, whose default slot shares
    //   the ground dispatcher's branch with seven other states. That slot
    //   alone moves to FDA30B13's loop branch, rewritten as the ground
    //   branch into FDA30B13 with a 3 frame fade. FDA30B13 is a spare Kliff
    //   action no branch or chart reaches; it plays the push-off under a
    //   name of its own, cd_phm_rd_broom_basic_00_00_nor_std_takeoff_00 (Seen
    //   below writes it into the chart, and broomclips.h has the files), and
    //   lasts the clip's 136 frames. Kliff's lower chart moves on only when
    //   a broom chart commands it, never on anim_end; the flight start
    //   commands it (crossfadepatches.h, b28), so the clip's round of broom
    //   idle after the 36 frame kick is only a margin. FDA30B13's loop slot
    //   points at the clip-end exit most states share, in case the end is
    //   ever reached. Seth, 3 October: "his feet touched the ground".
    constexpr DWORD kRiddenMs = 3000;
    constexpr char kPushOffOld[] = "1_pc/1_phm/00_riding/cd_phm_rd_wyvern_basic_00_00_air_move_fall_walk_end_00.paa";
    constexpr char kPushOffNew[] = "1_pc/1_phm/00_riding/cd_phm_rd_broom_basic_00_00_nor_std_takeoff_00.paa";
    static_assert(sizeof kPushOffNew <= sizeof kPushOffOld, "the new name must fit the old one's room");
    using bm::riderfind::Swap;
    constexpr int kSwapCount = bm::riderfind::kLowerSwaps;

    // Kliff's left hand IK. The node of 01B8F8D3 (the cut orca rider) fires
    // a frame event at 0 s (type 0x67 on 3392; twice, events 909 and 922,
    // under two conditions) that switches on the LimbIK's left hand
    // (posemodifierdata.xml, limb "lefthand") aimed at the mount's
    // B_IK_L_Hand. The broom skeleton has no such bone, so on a broom-skeleton
    // mount his left fist goes to the seat at the front of his belt, whatever
    // either layer plays. Found on the Speeder Bike, which rides on this
    // chart (5 October). The node's instances of the events fire at 1000 s
    // instead, past the end of every clip, so they never fire; the events
    // themselves are shared by other states and stay as they are. No shipped
    // mount reaches 01B8F8D3.
    constexpr uint32_t kNever = 0x447A0000;   // 1000.0f
    // The swaps as found in the lower chart the game has.
    Swap g_swaps[kSwapCount] = {};
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
            for (const Swap& w : g_swaps)
            {
                uint32_t* at = reinterpret_cast<uint32_t*>(base + w.offset);
                const uint32_t want = on ? w.now : w.shipped, was = on ? w.shipped : w.now;
                if (*at == was) *at = want;
                else if (*at != want) ok = false;
            }
            // A branch holding the other mount's target when ours go in is
            // Broomy's and the speeder's swaps meeting on the same words:
            // the other mod lets go in its own detour on this same check, so
            // ours go in on the next one. Before, the swap counted as in and
            // those branches kept the other mount's targets (SpeederBike.log,
            // 23 October 2026, 11:23:29).
            static volatile LONG s_heldLines = 0;
            if (on && !ok)
            {
                if (InterlockedIncrement(&s_heldLines) <= 20)
                    LOG("[rider] Kliff's lower chart holds the other mount's targets, so Broomy's go in on the next check.");
            }
            else
            {
                InterlockedExchange(&g_swapped, on ? 1 : 0);
                LOG(ok ? "[rider] Kliff's lower chart %s." : "[rider] Kliff's lower chart %s, but a branch held other bytes.",
                    on ? "takes his broom states (Broomy is ridden)" : "is back to its shipped targets");
            }
        }
        ReleaseSRWLockExclusive(&g_swapLock);
    }

    // Which mount Kliff is on, with the Speeder Bike loaded. Both mounts'
    // RideOn charts are checked every frame from their first ride to the end
    // of the session, ridden or not (SpeederBike.log, 23 October 2026), so
    // Broomy's running said Broomy was ridden while Kliff rode the speeder,
    // and Broomy's swap stayed in under him. A mount Kliff is not on settles
    // into a state and stays there; the one he rides changes state as he
    // gets on, sets off, stops and boosts. So the mount whose chart last went
    // into another state is the one he is on. A state change is a branch the
    // game lets through (the check's error 0) to a state other than the one
    // that chart last went to: a clip-end loop back into the same state is
    // none, and neither is settling into the ground idle. Until 1.0.7 this
    // counted every check outside the ground idle F5150415, but on 3392 the
    // parked speeder rests in a flight state and
    // checks its way back to the ground idle every frame, so both charts
    // counted as moving and Broomy and the speeder each took Kliff in turn
    // every frame: the speeder's engine ran on Broomy (Seth, 29 October).
    // Getting on is the exception. A mount action ends in the ground idle,
    // so getting on and staying on the ground changed no mount's state, and
    // the speeder ridden before still counted as Kliff's until Broomy took
    // off. Getting on Broomy within 3 s of leaving the speeder, while the
    // speeder's chart was still checked, started its engine on Broomy, and
    // Broomy's swap waited on the speeder's (Seth, 30 October; Broomy.log
    // 08:24:35, no "Kliff is on" line until the takeoff at 08:26:15). The
    // mount actions' branch to the ground idle gives the rider 082F0420
    // (+0x18, at +0xC3D1). The game checks it on every frame of the mount,
    // from the first, and lets it through only at the end, 2.1 s later. In
    // f171acd only the pass counted, so the speeder's engine still started
    // as Kliff got on Broomy and stopped 2.1 s later (SpeederBike.log, 30
    // October, startup 08:46:23.338, Broomy's pass 08:46:25.441). So a check
    // of that branch, let through or not, says Kliff is getting on that
    // mount. With every check counted on 3392, only the chart Kliff got on
    // checked it, each frame from 20:09:20.605 to 20:09:22.715, the 2.1 s of
    // the mount; the parked chart checked +0xBF39 and nothing else
    // (SpeederBike.log of 29 October, 20:06 to 20:10).
    // SpeederBike.asi says where its chart is (SpeederBikeRideOnChart).
    typedef int (*FnSpeederChart)(uintptr_t* base, uint32_t* size);
    FnSpeederChart g_speederChart = nullptr;
    volatile uintptr_t g_speederBase = 0;
    volatile uint32_t g_speederSize = 0;
    volatile LONG g_speederAsked = 0;
    volatile LONG g_broomMovedAt = 0, g_speederMovedAt = 0;
    volatile LONG g_onSpeederLogged = 0;
    // The state each chart last went to: Broomy's, the speeder's.
    volatile LONG g_state[2] = {};
    constexpr uint32_t kGroundIdle = 0xF5150415;
    constexpr uint32_t kMountedRider = 0x082F0420;

    bool OnSpeeder()
    {
        const LONG sp = g_speederMovedAt, b = g_broomMovedAt;
        return sp && (!b || static_cast<LONG>(sp - b) > 0);
    }

    // Once a second: where the speeder's chart is now, since a load serves it
    // again.
    void AskSpeeder(DWORD now)
    {
        const LONG asked = g_speederAsked;
        if (asked && now - static_cast<DWORD>(asked) < 1000) return;
        if (InterlockedCompareExchange(&g_speederAsked, static_cast<LONG>(now | 1), asked) != asked) return;
        if (!g_speederChart)
        {
            const HMODULE m = GetModuleHandleW(L"SpeederBike.asi");
            if (!m) return;
            g_speederChart = reinterpret_cast<FnSpeederChart>(GetProcAddress(m, "SpeederBikeRideOnChart"));
            if (!g_speederChart) return;
        }
        uintptr_t b = 0;
        uint32_t n = 0;
        if (!g_speederChart(&b, &n) || b == g_speederBase) return;
        InterlockedExchange(&g_state[1], 0);
        g_speederSize = n;
        g_speederBase = b;
        LOG("[rider] the speeder's riding chart is at %p (%u bytes), so Broomy knows when Kliff is on the speeder.",
            reinterpret_cast<void*>(b), n);
    }

    // Which mount's chart a branch record is in: 0 Broomy's, 1 the speeder's,
    // -1 neither.
    int ChartOf(uintptr_t pre, uintptr_t* base)
    {
        if (In(pre, g_rideOnBase, g_rideOnSize))
        {
            *base = g_rideOnBase;
            return 0;
        }
        if (In(pre, g_speederBase, g_speederSize))
        {
            *base = g_speederBase;
            return 1;
        }
        return -1;
    }

    void Moved(int chart, uint32_t target, uint32_t off, bool gettingOn)
    {
        InterlockedExchange(chart == 0 ? &g_broomMovedAt : &g_speederMovedAt, static_cast<LONG>(GetTickCount() | 1));
        const LONG on = OnSpeeder() ? 1 : 0;
        if (InterlockedExchange(&g_onSpeederLogged, on) != on)
            LOG(on ? "[rider] the speeder's chart %s %08X (+0x%X) after Broomy's last changed state, so Kliff is on "
                     "the speeder."
                   : "[rider] Broomy's chart %s %08X (+0x%X) after the speeder's last changed state, so Kliff is on "
                     "Broomy.",
                gettingOn ? "checked the getting-on branch to" : "went to", target, off);
    }

    // Notes each check of the mount actions' branch, let through or not.
    void GettingOn(uintptr_t pre)
    {
        uintptr_t base = 0;
        const int chart = ChartOf(pre, &base);
        if (chart < 0) return;
        uint32_t target = 0, rider = 0;
        if (!bm::mem::Read32(pre + 0x14, &target) || target != kGroundIdle || !bm::mem::Read32(pre + 0x18, &rider) ||
            rider != kMountedRider)
            return;
        Moved(chart, target, static_cast<uint32_t>(pre - base), true);
    }

    // Notes each branch a mount's chart takes into another state.
    void Track(uintptr_t pre)
    {
        AskSpeeder(GetTickCount());
        uintptr_t base = 0;
        const int chart = ChartOf(pre, &base);
        if (chart < 0) return;
        uint32_t target = 0;
        if (!bm::mem::Read32(pre + 0x14, &target) || !target) return;
        if (static_cast<uint32_t>(InterlockedExchange(&g_state[chart], static_cast<LONG>(target))) == target) return;
        // Settling into the ground idle is what a mount left alone does, so
        // it says nothing about the rider. Getting on ends there too, and
        // GettingOn has counted it from its first frame.
        if (target == kGroundIdle) return;
        Moved(chart, target, static_cast<uint32_t>(pre - base), false);
    }

    uint64_t CheckDetour(uintptr_t comp, uint32_t* err, uintptr_t layer, uintptr_t a4, uintptr_t a5, uintptr_t a6,
                         uintptr_t a7, uintptr_t a8, uintptr_t a9, uintptr_t a10, uintptr_t a11, uintptr_t a12)
    {
        uintptr_t pre = 0;
        const bool known = a8 && bm::mem::ReadPtr(a8, &pre);
        if (known)
        {
            const bool mine = In(pre, g_rideOnBase, g_rideOnSize);
            if (mine) InterlockedExchange(&g_rideOnAt, static_cast<LONG>(GetTickCount()));
            GettingOn(pre);
            // Broomy's own chart and the speeder's are checked every frame;
            // Kliff's lower chart can go unchecked for seconds, and on the
            // speeder after Broomy it went unchecked from the moment he sat
            // down, so the swap waited on it and was never let go (Broomy.log,
            // 23 October 2026, 11:34:38 on).
            if (mine || In(pre, g_speederBase, g_speederSize) || In(pre, g_lowerBase, g_lowerSize))
            {
                const LONG at = g_rideOnAt;
                const bool ridden = at && GetTickCount() - static_cast<DWORD>(at) < kRiddenMs && !OnSpeeder();
                if (ridden != (g_swapped != 0)) SetSwap(ridden);
            }
        }
        const uint64_t r = g_original(comp, err, layer, a4, a5, a6, a7, a8, a9, a10, a11, a12);
        uint32_t passed = 1;
        if (known && err && bm::mem::Read32(reinterpret_cast<uintptr_t>(err), &passed) && passed == 0) Track(pre);
        if (!g_log) return r;
        uintptr_t rec = 0;
        uint32_t code = 1, target = 0;
        if (!a8 || !err || !bm::mem::Read32(reinterpret_cast<uintptr_t>(err), &code) || code != 0) return r;
        if (!bm::mem::ReadPtr(a8, &rec) || !bm::mem::Read32(rec + 0x14, &target)) return r;
        const uint8_t l = static_cast<uint8_t>(layer);
        bool fresh = false;
        AcquireSRWLockExclusive(&g_lock);
        int i = 0;
        bool followed = Watched(target);
        while (i < g_used && !(g_last[i].comp == comp && g_last[i].layer == l))
        {
            followed = followed || g_last[i].comp == comp;
            ++i;
        }
        if (i == g_used && followed && g_used < kSlots) g_last[g_used++] = { comp, l, 0 };
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

    // Whether every swap's word holds its shipped value, as a chart just read
    // does.
    bool Holds(const uint8_t* data, const Swap* swaps)
    {
        for (int i = 0; i < kSwapCount; ++i)
            if (*reinterpret_cast<const uint32_t*>(data + swaps[i].offset) != swaps[i].shipped) return false;
        return true;
    }

    void Seen(const char* path, const uint8_t* data, uint32_t size)
    {
        const size_t n = strlen(path);
        if (n < 5 || _stricmp(path + n - 5, ".paac") != 0) return;
        if (!strstr(path, "ride") && !strstr(path, "riding")) return;
        if (g_log) LOG("[state] chart %s at %p (%u bytes)", path, data, size);
        if (!strstr(path, "/ride_test3_lower.paac")) return;
        bm::riderfind::Lower f;
        const char* why = "";
        const bool known = bm::riderfind::FindLower(data, size, f, why) && Holds(data, f.swaps);
        AcquireSRWLockExclusive(&g_swapLock);
        g_lowerBase = known ? reinterpret_cast<uintptr_t>(data) : 0;
        g_lowerSize = size;
        if (known) memcpy(g_swaps, f.swaps, sizeof g_swaps);
        InterlockedExchange(&g_swapped, 0);
        ReleaseSRWLockExclusive(&g_swapLock);
        if (!known)
        {
            LOG_ERR("[rider] ride_test3_lower.paac (%u bytes) is not a chart this knows (%s), so Kliff keeps the dragon "
                    "pose.", size, *why ? why : "a word to swap holds another value");
            return;
        }
        LOG("[rider] Kliff's lower chart (%u bytes): the dispatchers' default branches are %u and %u, the takeoff's slot "
            "%u moves to branch %u, its loop slot %u to branch %u.",
            size, f.ground, f.air, f.takeoffSlot, f.loop, f.loopSlot, f.exit);
        // The push-off's own name. FDA30B13 plays the clip at that path; the
        // chart hashes its paths as it parses, which is after this read, so
        // the name is written here, padded with zeros inside the old
        // string's room as broomchart.cpp does. Nothing reaches FDA30B13
        // unless Broomy is ridden, so it keeps the new name throughout.
        uint8_t* name = const_cast<uint8_t*>(data) + f.pushOff;
        memset(name, 0, sizeof kPushOffOld);
        memcpy(name, kPushOffNew, sizeof kPushOffNew - 1);
        LOG("[rider] Kliff's push-off plays as %s.", kPushOffNew);
        for (int i = 0; i < f.ikCount; ++i) memcpy(const_cast<uint8_t*>(data) + f.ik[i], &kNever, 4);
        LOG("[rider] 01B8F8D3's left hand IK stays off (%d event%s).", f.ikCount, f.ikCount == 1 ? "" : "s");
    }
}

namespace bm::riderfix
{
    void NoteServed(const char* path, uintptr_t base, uint32_t size)
    {
        if (!strstr(path, "m0002_armadillo.paac")) return;
        InterlockedExchange(&g_state[0], 0);
        g_rideOnSize = size;
        g_rideOnBase = base;
    }

    bool Install(bool logStates)
    {
        g_log = logStates;
        size_t hits = 0;
        const uintptr_t at = bm::mem::FindUnique(kSig_BranchCheck, &hits);
        if (!at)
        {
            LOG_ERR("[rider] the branch check is not on this exe (%zu matches).", hits);
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
        LOG(logStates ? "[rider] Kliff rides Broomy in his broom poses; each action taken by the charts of whoever "
                        "mounts, rides or carries is logged as [state] (test build)."
                      : "[rider] Kliff rides Broomy in his broom poses.");
        return true;
    }
}
