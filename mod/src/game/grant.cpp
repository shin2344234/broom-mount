#include "game/grant.h"

#include <Windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "core/log.h"
#include "game/broomy.h"
#include "game/callgate.h"
#include "game/farhook.h"
#include "game/mem.h"
#include "game/signatures.h"

using namespace bm::sig;

namespace
{
    typedef uint64_t (*FnSend)(uintptr_t ctx, uintptr_t buffer, uintptr_t kind, uintptr_t message, uintptr_t length,
                               uintptr_t a6, uintptr_t a7);
    typedef uint64_t (*FnHire)(uintptr_t clan, uint32_t* error, uint32_t* actorId, uintptr_t flag, uintptr_t flag2);
    typedef uint64_t (*FnLookup)(uintptr_t manager, uintptr_t out, uintptr_t id);
    FnSend   g_sendOriginal = nullptr;
    FnHire   g_hireOriginal = nullptr;
    FnLookup g_lookupOriginal = nullptr;

    constexpr int kHorseList = 78;   // Vehicle_Horse's key, as characterinfo's file holds it

    // The hire takes any unowned actor of kind 4 to 6; only the plugin asked
    // for a horse. A save loaded where no wild horse grazes waited for one
    // (28 September, and Seth on 19 October: "it should be added as soon as
    // i start"), so after kHorseGraceMs the grant takes another wild animal:
    // a list of mounts or animals, never one of people (Mercenary_*) and
    // never an actor with no list, which is where NPCs and monsters sit.
    constexpr DWORD kHorseGraceMs = 4000;
    constexpr int   kNoRank = 9;
    int Rank(int list)
    {
        switch (list)
        {
        case kHorseList: return 0;
        case 65:   // Vehicle: wolves, bulls, wild mounts
        case 81:   // Vehicle_Special: bears, wolves
        case 68:   // Domestic: cows, goats
            return 1;
        case 84:   // Insect
        case 69:   // Fish
            return 2;
        default: return -1;
        }
    }
    const char* ListName(int list)
    {
        switch (list)
        {
        case kHorseList: return "wild horse";
        case 65: return "wild mount";
        case 81: return "wild bear or wolf";
        case 68: return "farm animal";
        case 84: return "insect";
        case 69: return "fish";
        default: return "actor";
        }
    }
    // Unowned actors of kind 4 to 6 the lookup saw while the grant waited,
    // counted by mercenary list (index 255: no list), for the waiting line.
    volatile LONG g_seenByList[256] = {};

    // Loading: each LoadingComplete bumps the sequence, and the roster is
    // checked once per sequence, kGrantDelayMs after it.
    volatile LONG g_loadSeq = 0;
    volatile LONG g_loadedAt = 0;
    volatile LONG g_checkedSeq = 0;
    DWORD         g_retryAt = 0;
    // A save still loading shows one roster where a loaded one shows two:
    // at 07:58:52 on 2 October one roster had no Broomy and 26 seconds later
    // two did. A check that sees fewer waits and looks again, a few times.
    constexpr int kRostersLoaded = 2;
    constexpr int kThinRetries = 15;
    constexpr DWORD kThinRetryMs = 2000;
    int           g_thinChecks = 0;
    LONG          g_thinSeq = 0;
    // While armed, the roster is looked at again this often, and a Broomy
    // that has turned up stands the grant down before it hires a second.
    constexpr DWORD kArmedRecheckMs = 10000;
    DWORD         g_armedCheckAt = 0;

    // Loading a save from inside the game sends no LoadingComplete, so the
    // rosters the last check read are watched as well: when one is gone, or
    // its element array has moved (a reload, or a hire that grew it), the
    // roster is checked again. A check that finds Broomy does nothing, so a
    // check too many costs only the scan.
    // Only the largest roster is watched, the player's: the other one the
    // scan finds moves on its own every second or so (12:03, 28 September).
    struct Seen { uintptr_t clan; uintptr_t array; };
    Seen  g_seen[1];
    int   g_seenCount = 0;
    DWORD g_watchAt = 0;

    // The grant, armed by a check that found no Broomy.
    volatile LONG g_armed = 0;
    volatile LONG g_candidate = 0;      // the last unowned wild horse seen
    volatile LONG g_candidateRow = 0;
    volatile LONG g_candidateRank = kNoRank;
    volatile LONG g_armedAt = 0;        // when the check that found no Broomy armed the grant
    constexpr DWORD kSameLoadMs = 30000;
    DWORD         g_lastNoBroomyAt = 0; // the last check that found no Broomy, 0 after one that found it
    volatile LONG g_target = 0;         // the actor the server was asked to hire
    volatile LONG g_sentAt = 0;
    volatile LONG g_tries = 0;
    volatile LONG g_waitLogged = 0;
    volatile LONG g_rejected[kGrantTries] = {};
    volatile LONG g_checkWindow = 0, g_checks = 0;

    // Where Broomy arrives, for reports of it hanging out of reach: after a
    // Broomy call, the actor lookup notes the first actor whose row is
    // Broomy's, the movement handler notes the player (the sender), and the
    // log gets both heights at each of kHeightAtMs after the call. Broomy's
    // actor stands on its root and its clips float the broom about 1.5 m
    // above that, so a broom at rest beside Kliff reads close to his height.
    constexpr DWORD kHeightAtMs[] = { 2000, 5000, 10000 };
    constexpr int   kHeightChecks = sizeof kHeightAtMs / sizeof kHeightAtMs[0];
    volatile LONG   g_calledAt = 0;          // 0 when no call is being watched
    volatile LONG   g_heightLogged = 0;      // checkpoints logged so far
    volatile LONG64 g_broomyActor = 0, g_playerActor = 0;

    // When the client's call check last found Broomy out and up (0 for none);
    // the call request it sends follows within a few milliseconds and is sent
    // twice (SendDetour).
    volatile LONG   g_outAtCall = 0;
    constexpr DWORD kOutAtCallMs = 500;
    unsigned char   g_callCopy[256];

    bool g_give = true;   // GiveBroomy; without it the roster is read, never added to

    // The actor's position and the transform field it came from (0 when none
    // reads): the first world copy that holds one, else, or with local set,
    // its local position plus its parent's.
    unsigned Position(uintptr_t actor, float* xyz, bool local = false)
    {
        uintptr_t block = 0, tf = 0;
        if (!bm::mem::ReadPtr(actor + kOff_Actor_Block, &block) ||
            !bm::mem::ReadPtr(block + kOff_Block_Transform, &tf))
            return 0;
        auto good = [](const float* v) {
            return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]) &&
                   !(v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f);
        };
        if (!local)
            for (unsigned off : kTf_WorldCopies)
                if (bm::mem::ReadF32x3(tf + off, xyz) && good(xyz)) return off;
        float parent[3] = {};
        uint32_t pid = 0;
        if (!bm::mem::ReadF32x3(tf + kOff_Tf_LocalPos, xyz) || !good(xyz)) return 0;
        if (bm::mem::Read32(tf + kOff_Tf_ParentId, &pid) && pid && pid != 0xFFFFFFFF &&
            bm::mem::ReadF32x3(tf + kOff_Tf_ParentPos, parent) && good(parent))
            for (int i = 0; i < 3; ++i) xyz[i] += parent[i];
        return kOff_Tf_LocalPos;
    }

    void NoteBroomyActor(uintptr_t out)
    {
        if (!g_calledAt || g_broomyActor) return;
        const int broomy = bm::broomy::Row();
        uint8_t found = 0;
        uintptr_t actor = 0, block = 0, status = 0;
        uint16_t row = 0;
        if (broomy >= 0 && bm::mem::Read8(out + kOff_Lookup_Found, &found) && found &&
            bm::mem::ReadPtr(out + kOff_Lookup_Actor, &actor) && bm::mem::ReadPtr(actor + kOff_Actor_Block, &block) &&
            bm::mem::ReadPtr(block + kOff_Block_Status, &status) && bm::mem::Read16(status + kOff_Status_CharRow, &row) &&
            row == broomy)
            InterlockedCompareExchange64(&g_broomyActor, static_cast<LONG64>(actor), 0);
    }

    // On the server thread, about 37 times a second.
    void CheckHeight(uintptr_t message)
    {
        const LONG at = g_calledAt;
        if (!at) return;
        uintptr_t sender = 0, kind = 0;
        uint8_t k = 0;
        if (bm::mem::ReadPtr(message, &sender) && bm::mem::ReadPtr(sender + kOff_Sender_Kind, &kind) &&
            bm::mem::Read8(kind + 1, &k) && k == 1)
            InterlockedExchange64(&g_playerActor, static_cast<LONG64>(sender));
        const LONG done = g_heightLogged;
        // Signed: the call's stamp is ORed with 1, so it can lead the clock by 1 ms.
        const LONG since = static_cast<LONG>(GetTickCount() - static_cast<DWORD>(at));
        if (done >= kHeightChecks || since < static_cast<LONG>(kHeightAtMs[done])) return;
        if (InterlockedCompareExchange(&g_heightLogged, done + 1, done) != done) return;
        const uintptr_t broomy = static_cast<uintptr_t>(g_broomyActor), player = static_cast<uintptr_t>(g_playerActor);
        float b[3] = {}, p[3] = {};
        const double secs = kHeightAtMs[done] / 1000.0;
        unsigned fb = broomy ? Position(broomy, b) : 0, fp = player ? Position(player, p) : 0;
        // One space for both: a world position against a local one put Broomy
        // 6 km from Kliff at his own height.
        if (fb && fp && (fb == kOff_Tf_LocalPos) != (fp == kOff_Tf_LocalPos))
        {
            fb = Position(broomy, b, true);
            fp = Position(player, p, true);
        }
        if (!fb)
            LOG("[height] %.0f s after the call: Broomy's actor %s.", secs, broomy ? "gave no position" : "was not seen");
        else if (!fp)
            LOG("[height] %.0f s after the call: Broomy stands at (%.1f, %.1f, %.1f); the player gave no position.", secs,
                b[0], b[1], b[2]);
        else
        {
            const float dx = b[0] - p[0], dz = b[2] - p[2];
            LOG("[height] %.0f s after the call: Broomy stands %.1f m %s Kliff and %.1f m from him (Broomy at %.1f, Kliff "
                "at %.1f; transform +0x%X and +0x%X).", secs, fabsf(b[1] - p[1]), b[1] >= p[1] ? "above" : "below",
                sqrtf(dx * dx + dz * dz), b[1], p[1], fb, fp);
        }
        if (done + 1 >= kHeightChecks) InterlockedExchange(&g_calledAt, 0);
    }

    // The swap: while the grant's hire runs, its lookup of that actor points
    // the actor's row at Broomy's; the hire puts it back when it returns.
    volatile LONG g_swapId = 0, g_swapTid = 0;
    uintptr_t     g_swapStatus = 0;
    uint16_t      g_swapOld = 0;

    bool Pending() { return g_armed && g_tries < kGrantTries; }

    bool Rejected(LONG id)
    {
        for (const volatile LONG& r : g_rejected)
            if (r == id) return true;
        return false;
    }

    bool WriteU16(uintptr_t at, uint16_t v)
    {
        DWORD old = 0;
        if (!bm::mem::Readable(at, 2)) return false;
        if (!VirtualProtect(reinterpret_cast<void*>(at), 2, PAGE_READWRITE, &old)) return false;
        *reinterpret_cast<volatile uint16_t*>(at) = v;
        DWORD ignored;
        VirtualProtect(reinterpret_cast<void*>(at), 2, old, &ignored);
        return true;
    }

    bool WriteU32(uintptr_t at, uint32_t v)
    {
        DWORD old = 0;
        if (!bm::mem::Readable(at, 4)) return false;
        if (!VirtualProtect(reinterpret_cast<void*>(at), 4, PAGE_READWRITE, &old)) return false;
        *reinterpret_cast<volatile uint32_t*>(at) = v;
        DWORD ignored;
        VirtualProtect(reinterpret_cast<void*>(at), 4, old, &ignored);
        return true;
    }

    // After the hire, Broomy's roster entry names the wild horse it was
    // hired from as its actor out in the world. That actor stays a wild
    // horse, and after a reload it is gone, but the server does not spawn a
    // mount whose entry names an actor: the call is accepted and nothing
    // comes (Seth, 2 October, a save that lost Broomy while the plugin was
    // out and was given a new one). With the field cleared Broomy is stored
    // and the call spawns it. True when the field was cleared.
    bool StoreBroomy(uintptr_t clan, uint32_t hired)
    {
        const int row = bm::broomy::Row();
        uint32_t count = 0;
        uintptr_t arr = 0;
        if (row < 0 || !bm::mem::Read32(clan + kOff_Clan_Count, &count) || !bm::mem::ReadPtr(clan + kOff_Clan_Array, &arr) ||
            count > 1000)
            return false;
        for (uint32_t k = 0; k < count; ++k)
        {
            uintptr_t el = 0, entry = 0;
            uint16_t r = 0;
            uint32_t actor = 0;
            if (bm::mem::ReadPtr(arr + 8ull * k, &el) && bm::mem::ReadPtr(el + kOff_ClanEl_Entry, &entry) &&
                bm::mem::Read16(entry + kOff_ClanEnt_Row, &r) && r == row &&
                bm::mem::Read32(entry + kOff_ClanEnt_Actor, &actor) && actor == hired)
                return WriteU32(entry + kOff_ClanEnt_Actor, 0);
        }
        return false;
    }

    // Is the actor this lookup found one the hire takes (kind 4 to 6, no
    // owner), and a wild animal? A horse beats any other; among the rest the
    // better rank wins. Runs on the game's thread inside its own lookup, so
    // at most a few hundred a second are looked at.
    void Consider(uintptr_t out, uint32_t id)
    {
        if (!id || static_cast<LONG>(id) == g_candidate || Rejected(static_cast<LONG>(id))) return;
        const LONG now = static_cast<LONG>(GetTickCount() / 1000);
        if (g_checkWindow != now)
        {
            InterlockedExchange(&g_checkWindow, now);
            InterlockedExchange(&g_checks, 0);
        }
        if (InterlockedIncrement(&g_checks) > 300) return;
        uint8_t found = 0, kind = 0;
        uintptr_t actor = 0, kindObj = 0, block = 0, status = 0, owner = 0;
        uint32_t ownerId = 1;
        uint16_t row = 0;
        if (!bm::mem::Read8(out + kOff_Lookup_Found, &found) || !found ||
            !bm::mem::ReadPtr(out + kOff_Lookup_Actor, &actor) ||
            !bm::mem::ReadPtr(actor + kOff_Actor_Kind, &kindObj) || !bm::mem::Read8(kindObj + kOff_Kind_Byte, &kind) ||
            kind < kActorKindHireLo || kind > kActorKindHireHi ||
            !bm::mem::ReadPtr(actor + kOff_Actor_Block, &block) || !bm::mem::ReadPtr(block + kOff_Block_Owner, &owner) ||
            !bm::mem::Read32(owner + kOff_Owner_Id, &ownerId) || ownerId != 0 ||
            !bm::mem::ReadPtr(block + kOff_Block_Status, &status) || !bm::mem::Read16(status + kOff_Status_CharRow, &row))
            return;
        const int list = bm::broomy::MercenaryKey(row);
        if (Pending()) InterlockedIncrement(&g_seenByList[list >= 0 && list < 255 ? list : 255]);
        const int rank = Rank(list);
        if (rank < 0 || (g_candidate && rank > g_candidateRank)) return;
        InterlockedExchange(&g_candidateRow, row);
        InterlockedExchange(&g_candidateRank, rank);
        InterlockedExchange(&g_candidate, static_cast<LONG>(id));
    }

    uint64_t LookupDetour(uintptr_t manager, uintptr_t out, uintptr_t id)
    {
        const uint64_t r = g_lookupOriginal(manager, out, id);
        const LONG armed = g_swapId;
        if (!armed || static_cast<uint32_t>(id) != static_cast<uint32_t>(armed) ||
            static_cast<DWORD>(g_swapTid) != GetCurrentThreadId())
        {
            // Wild horses are noted from the moment a save loads, so the
            // hire can go as soon as the check finds no Broomy.
            if ((Pending() || g_loadSeq != g_checkedSeq) && !g_target) Consider(out, static_cast<uint32_t>(id));
            NoteBroomyActor(out);
            return r;
        }
        InterlockedExchange(&g_swapId, 0);
        const int broomy = bm::broomy::Row();
        uintptr_t actor = 0, block = 0, status = 0;
        uint16_t row = 0;
        if (broomy < 0 || !bm::mem::ReadPtr(out + kOff_Lookup_Actor, &actor) ||
            !bm::mem::ReadPtr(actor + kOff_Actor_Block, &block) ||
            !bm::mem::ReadPtr(block + kOff_Block_Status, &status) ||
            !bm::mem::Read16(status + kOff_Status_CharRow, &row))
        {
            LOG_ERR("[grant] the hire's actor %08X could not be read, so nothing was swapped.", static_cast<uint32_t>(id));
            return r;
        }
        if (WriteU16(status + kOff_Status_CharRow, static_cast<uint16_t>(broomy)))
        {
            g_swapStatus = status;
            g_swapOld = row;
        }
        return r;
    }

    void Miss(LONG target, const char* why)
    {
        const LONG n = InterlockedIncrement(&g_tries);
        if (n >= 1 && n <= kGrantTries) InterlockedExchange(&g_rejected[n - 1], target);
        if (g_candidate == target)
        {
            InterlockedExchange(&g_candidate, 0);
            InterlockedExchange(&g_candidateRank, kNoRank);
        }
        InterlockedExchange(&g_waitLogged, 0);
        InterlockedExchange(&g_target, 0);
        if (n >= kGrantTries)
            LOG_ERR("[grant] %08X %s. That was try %ld of %d, so the grant stops until the next load.",
                    static_cast<uint32_t>(target), why, n, kGrantTries);
        else
            LOG("[grant] %08X %s. Try %ld of %d; the next goes to another wild animal.", static_cast<uint32_t>(target),
                why, n, kGrantTries);
    }

    // The grant hires on the server's own thread, from the handler of the
    // client's movement request (0x0B0C), which the server runs about 37
    // times a second on the thread that also runs the mount call. It calls
    // the server's hire as the hire request's own handler (+0x2A2F160) does:
    // (the sender's clan component, u32* error, u32* actor id, 0, 0), and
    // HireDetour does the rest. Sending the hire request (0x0B8F) instead
    // worked only for a horse beside the player: the server dropped the
    // request for any other before its handler ran (2 October, after a
    // save was loaded from inside the game).
    typedef uint64_t (*FnHandler)(uintptr_t self, uint32_t* error, uintptr_t message);
    FnHandler g_moveOriginal = nullptr;

    uint64_t HireDetour(uintptr_t clan, uint32_t* error, uint32_t* actorId, uintptr_t flag, uintptr_t flag2);

    // The sender's clan component, as the hire request's handler reads it:
    // the sender actor is the message's first field; it must be the player
    // (kind byte 1 at [actor+0x88]+1) and ready (+0x96).
    uintptr_t SenderClan(uintptr_t message)
    {
        uintptr_t actor = 0, kind = 0, block = 0, clan = 0;
        uint8_t k = 0, ready = 0;
        if (!bm::mem::ReadPtr(message, &actor) || !bm::mem::ReadPtr(actor + kOff_Sender_Kind, &kind) ||
            !bm::mem::Read8(kind + 1, &k) || k != 1 || !bm::mem::Read8(actor + kOff_Sender_Ready, &ready) || !ready ||
            !bm::mem::ReadPtr(actor + kOff_Sender_Block, &block) || !bm::mem::ReadPtr(block + kOff_Block_Clan, &clan))
            return 0;
        return clan;
    }

    void GrantNow(uintptr_t message)
    {
        if (!Pending() || g_target || bm::broomy::Row() < 0) return;
        const LONG target = g_candidate;
        const DWORD armedFor = GetTickCount() - static_cast<DWORD>(g_armedAt);
        if (target && g_candidateRank > 0 && armedFor < kHorseGraceMs) return;   // a horse may still turn up
        if (!target)
        {
            if (armedFor >= kHorseGraceMs && InterlockedCompareExchange(&g_waitLogged, 1, 0) == 0)
            {
                char seen[256] = {};
                size_t used = 0;
                for (int list = 0; list < 256 && used + 16 < sizeof seen; ++list)
                    if (const LONG n = g_seenByList[list])
                        used += snprintf(seen + used, sizeof seen - used, "%s%s %ld", used ? ", " : "",
                                         list == 255 ? "no list" : std::to_string(list).c_str(), n);
                LOG("[grant] no unowned wild animal is loaded yet, so Broomy waits for the first one the server loads. "
                    "Unowned actors seen by mercenary list: %s.", used ? seen : "none");
            }
            return;
        }
        const uintptr_t clan = SenderClan(message);
        if (!clan || InterlockedCompareExchange(&g_target, target, 0) != 0) return;
        InterlockedExchange(&g_sentAt, static_cast<LONG>(GetTickCount()));
        const int list = bm::broomy::MercenaryKey(static_cast<uint32_t>(g_candidateRow));
        LOG("[grant] hiring %s %08X (characterinfo row %ld, mercenary list %d) as Broomy (row %d).", ListName(list),
            static_cast<uint32_t>(target), g_candidateRow, list, bm::broomy::Row());
        uint32_t error = 0, id = static_cast<uint32_t>(target);
        HireDetour(clan, &error, &id, 0, 0);
        if (g_target == target) Miss(target, "was not hired, for no reason the server gave");
    }

    uint64_t MoveDetour(uintptr_t self, uint32_t* error, uintptr_t message)
    {
        const uint64_t r = g_moveOriginal(self, error, message);
        GrantNow(message);
        CheckHeight(message);
        return r;
    }

    uint64_t SendDetour(uintptr_t ctx, uintptr_t buffer, uintptr_t kind, uintptr_t message, uintptr_t length,
                        uintptr_t a6, uintptr_t a7)
    {
        uint16_t id = 0;
        if (message && bm::mem::Read16(message, &id) && (id == 0x0AC1 || id == 0x0876))
        {
            if (id == 0x0AC1)
            {
                uint32_t slot = 0;
                bm::mem::Read32(message + 5, &slot);
                LOG("[wedge] request 0x0AC1 sent (a wedge was chosen: reserve slot %u).", slot);
            }
            else
                LOG("[wedge] request 0x0876 sent (the mount is called%s).",
                    bm::callgate::BroomyChosen() ? ": the saddle wedge holds Broomy" : "");
            if (id == 0x0876 && bm::callgate::BroomyChosen())
            {
                // Each call watches afresh: Broomy's actor is new every time.
                InterlockedExchange64(&g_broomyActor, 0);
                InterlockedExchange(&g_heightLogged, 0);
                InterlockedExchange(&g_calledAt, static_cast<LONG>(GetTickCount() | 1));
            }
            // A Broomy already out: the game's first call tells it to come by
            // itself, which a broom cannot, and only a later call brings it
            // with the arrival chain the Ride prompt waits for (1.0.5-test15,
            // KNOWLEDGE.md). So the same request goes again at once, while
            // Kliff still plays the call that the server checks it against.
            // Signed: the stamp is ORed with 1, so it can lead the clock by 1 ms,
            // and unsigned that read as 49 days (1.0.5-test16, 16:10:51).
            const LONG out = g_outAtCall;
            const LONG age = static_cast<LONG>(GetTickCount() - static_cast<DWORD>(out));
            const size_t bytes = static_cast<size_t>(length & 0xFFFF);
            if (id == 0x0876 && out && age < static_cast<LONG>(kOutAtCallMs) &&
                InterlockedCompareExchange(&g_outAtCall, 0, out) == out && bytes <= sizeof g_callCopy &&
                bm::mem::ReadBytes(message, g_callCopy, bytes))
            {
                const uint64_t r = g_sendOriginal(ctx, buffer, kind, message, length, a6, a7);
                LOG("[call] Broomy is out, so the call goes to the server twice (%zu bytes).", bytes);
                g_sendOriginal(ctx, buffer, kind, reinterpret_cast<uintptr_t>(g_callCopy), length, a6, a7);
                return r;
            }
        }
        if (id == kReq_LoadingComplete)
        {
            InterlockedExchange(&g_loadedAt, static_cast<LONG>(GetTickCount()));
            InterlockedIncrement(&g_loadSeq);
            InterlockedExchange(&g_armed, 0);
        }
        return g_sendOriginal(ctx, buffer, kind, message, length, a6, a7);
    }

    uint64_t HireDetour(uintptr_t clan, uint32_t* error, uint32_t* actorId, uintptr_t flag, uintptr_t flag2)
    {
        uint32_t id = 0;
        if (actorId) bm::mem::Read32(reinterpret_cast<uintptr_t>(actorId), &id);
        const bool grant = id && g_target && static_cast<LONG>(id) == g_target && bm::broomy::Row() >= 0;
        if (grant)
        {
            g_swapStatus = 0;
            InterlockedExchange(&g_swapTid, static_cast<LONG>(GetCurrentThreadId()));
            InterlockedExchange(&g_swapId, static_cast<LONG>(id));
        }
        const uint64_t r = g_hireOriginal(clan, error, actorId, flag, flag2);
        if (!grant) return r;
        InterlockedExchange(&g_swapId, 0);
        uint32_t err = 0;
        if (error) bm::mem::Read32(reinterpret_cast<uintptr_t>(error), &err);
        const bool swapped = g_swapStatus != 0;
        if (swapped) WriteU16(g_swapStatus + kOff_Status_CharRow, g_swapOld);
        if (err)
        {
            Miss(static_cast<LONG>(id), "could not be hired");
            return r;
        }
        // Hired either way, so no second try: another would add a second
        // mount.
        InterlockedExchange(&g_armed, 0);
        InterlockedExchange(&g_target, 0);
        if (swapped && !StoreBroomy(clan, id))
            LOG_ERR("[grant] Broomy's roster entry still names horse %08X as out in the world, so calling Broomy may "
                    "bring nothing.", id);
        if (swapped)
            LOG_OK("[grant] Broomy has joined your mounts. Save before you quit.");
        else
            LOG_ERR("[grant] %08X joined your mounts as itself: the hire never looked it up the way it used to. "
                    "No second try this load.", id);
        return r;
    }

    // ---- the roster ------------------------------------------------------

    // Committed private read-write memory, where the game's objects live.
    // Plain loop under SEH: a region can be freed while it is read.
    int ScanRegion(const uint8_t* base, size_t size, uint64_t needle, uintptr_t* out, int room)
    {
        int n = 0;
        __try
        {
            const uint64_t* p = reinterpret_cast<const uint64_t*>(base);
            const size_t count = size / 8;
            for (size_t i = 0; i < count && n < room; ++i)
                if (p[i] == needle) out[n++] = reinterpret_cast<uintptr_t>(p + i);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return n;
    }

    // Every server roster in memory, found by its vtable: how many there
    // are, and whether any lists `row`.
    bool RosterHas(int row, int& rosters)
    {
        rosters = 0;
        g_seenCount = 0;
        uintptr_t vt[2] = {};
        if (bm::mem::FindVtablesByName(kRtti_ServerClan, vt, 2) != 1) return false;
        uintptr_t hits[64];
        int found = 0;
        MEMORY_BASIC_INFORMATION mbi;
        uintptr_t at = 0x10000;
        while (found < 64 && VirtualQuery(reinterpret_cast<void*>(at), &mbi, sizeof mbi))
        {
            const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            if (mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE && mbi.Type == MEM_PRIVATE &&
                mbi.RegionSize < (1ull << 30))
                found += ScanRegion(static_cast<const uint8_t*>(mbi.BaseAddress), mbi.RegionSize, vt[0], hits + found,
                                    64 - found);
            at = base + mbi.RegionSize;
            if (at <= base) break;
        }
        bool has = false;
        uint32_t largest = 0;
        for (int i = 0; i < found; ++i)
        {
            uint32_t count = 0;
            uintptr_t arr = 0;
            if (!bm::mem::Read32(hits[i] + kOff_Clan_Count, &count) || !bm::mem::ReadPtr(hits[i] + kOff_Clan_Array, &arr) ||
                count > 1000)
                continue;
            ++rosters;
            if (count > largest)
            {
                largest = count;
                g_seen[0] = { hits[i], arr };
                g_seenCount = 1;
            }
            for (uint32_t k = 0; k < count && !has; ++k)
            {
                uintptr_t el = 0, entry = 0;
                uint16_t r = 0;
                if (bm::mem::ReadPtr(arr + 8ull * k, &el) && bm::mem::ReadPtr(el + kOff_ClanEl_Entry, &entry) &&
                    bm::mem::Read16(entry + kOff_ClanEnt_Row, &r) && r == row)
                    has = true;
            }
        }
        return has;
    }

    uintptr_t CallTarget(uintptr_t call)
    {
        uint8_t op = 0;
        return bm::mem::Read8(call, &op) && op == 0xE8 ? bm::mem::RipAt(call, 5) : 0;
    }

    // True when a roster the last check read is no longer where it was.
    bool RostersChanged(uintptr_t vt)
    {
        for (int i = 0; i < g_seenCount; ++i)
        {
            uintptr_t v = 0, arr = 0;
            if (!bm::mem::ReadPtr(g_seen[i].clan, &v) || v != vt ||
                !bm::mem::ReadPtr(g_seen[i].clan + kOff_Clan_Array, &arr) || arr != g_seen[i].array)
                return true;
        }
        return false;
    }

    // A fainted Broomy (Seth, 21 October: "the broom fainted for some
    // unknown reason. thats not supposed to persist anymore"). Speeder Bike
    // fixed this in 1.0.3 and Broomy never took it: with the row's 1 second
    // cool time the call stayed refused until a reload, because the fainted
    // actor is not loaded again and the roster entry then names no actor.
    // This does the same at the call: when the client's check finds Broomy
    // down, the lookup result is let go as the check itself would, so the
    // check takes its path for a mount that is not out, and the server roster
    // entry's actor id is cleared (StoreBroomy), so the server spawns a fresh
    // Broomy for the call. With Speeder Bike installed both hook this call;
    // each one's detour passes any other row through. Only the dead bytes
    // count, which the game itself reads unlocked. Its groggy test
    // (+0x1794ED0) runs under a reader lock on [[status+8]+8] that a detour
    // here does not hold, and +0x320 reads 2 at every call of a Broomy that
    // is up, so calling it would race on every call; a wrong "groggy" would
    // clear the roster entry of a Broomy still out (review, 21 October). A
    // groggy Broomy waits out its stun as the game says.
    typedef uint64_t (*FnCallLookup)(uintptr_t manager, uintptr_t out, uintptr_t id);
    typedef void (*FnReleaseResult)(uintptr_t out);
    FnCallLookup    g_callLookupOriginal = nullptr;
    FnReleaseResult g_releaseResult = nullptr;

    uint64_t CallLookupDetour(uintptr_t manager, uintptr_t out, uintptr_t id)
    {
        const uint64_t r = g_callLookupOriginal(manager, out, id);
        const uintptr_t res = r ? static_cast<uintptr_t>(r) : out;
        const int row = bm::broomy::Row();
        uint8_t found = 0, deadA = 0, deadB = 0;
        uintptr_t actor = 0, block = 0, comp = 0, self = 0, selfBlock = 0, status = 0;
        uint16_t charRow = 0;
        uint32_t groggy = 0;
        if (row < 0 || !bm::mem::Read8(res + kOff_Lookup_Found, &found) || !found ||
            !bm::mem::ReadPtr(res + kOff_Lookup_Actor, &actor) || !bm::mem::ReadPtr(actor + kOff_Actor_Block, &block) ||
            !bm::mem::ReadPtr(block + kOff_Block_Owner, &comp) || !bm::mem::ReadPtr(comp + kOff_MountComp_Actor, &self) ||
            !bm::mem::ReadPtr(self + kOff_Actor_Block, &selfBlock) ||
            !bm::mem::ReadPtr(selfBlock + kOff_Block_Status, &status) ||
            !bm::mem::Read16(status + kOff_Status_CharRow, &charRow) || charRow != row ||
            !bm::mem::Read8(status + kOff_Status_DeadA, &deadA) || !bm::mem::Read8(status + kOff_Status_DeadB, &deadB) ||
            !bm::mem::Read32(status + kOff_Status_Groggy, &groggy))
            return r;
        if (!deadA && !deadB)
        {
            LOG("[faint] Broomy (actor %08X) is up at the call (+0x272 %u, +0x273 %u, +0x320 %d).",
                static_cast<uint32_t>(id), deadA, deadB, static_cast<int32_t>(groggy));
            InterlockedExchange(&g_outAtCall, static_cast<LONG>(GetTickCount() | 1));
            return r;
        }
        const bool stored = g_seenCount && StoreBroomy(g_seen[0].clan, static_cast<uint32_t>(id));
        if (!stored)
        {
            LOG_ERR("[faint] Broomy (actor %08X) is down at the call (+0x272 %u, +0x273 %u, +0x320 %d), but its "
                    "roster entry was not found to clear, so the game refuses the call.",
                    static_cast<uint32_t>(id), deadA, deadB, static_cast<int32_t>(groggy));
            return r;
        }
        g_releaseResult(res);
        // The result is the check's own stack object.
        *reinterpret_cast<uint8_t*>(res + kOff_Lookup_Found) = 0;
        *reinterpret_cast<uintptr_t*>(res + kOff_Lookup_Actor) = 0;
        LOG("[faint] Broomy (actor %08X) is down at the call (+0x272 %u, +0x273 %u, +0x320 %d); its roster entry "
            "is cleared, so the call brings a fresh one.",
            static_cast<uint32_t>(id), deadA, deadB, static_cast<int32_t>(groggy));
        return r;
    }

    void InstallCallCheck()
    {
        size_t hits = 0;
        const uintptr_t at = bm::mem::FindUnique(kSig_CallCheck, &hits);
        const uintptr_t release = at && bm::mem::MatchAt(at + kOff_CallCheck_ReleaseSig, kSig_CallCheckRelease)
                                      ? CallTarget(at + kOff_CallCheck_ReleaseSig + 4) : 0;
        if (!at || !release)
        {
            LOG_ERR("[faint] the mount call's check was found %zu times (release %s), so a fainted Broomy waits as the "
                    "game says.", hits, release ? "found" : "missing");
            return;
        }
        g_releaseResult = reinterpret_cast<FnReleaseResult>(release);
        char why[96] = {};
        if (!bm::farhook::InstallOverJump("mount call check lookup", at + kOff_CallCheck_Lookup,
                                          reinterpret_cast<void*>(&CallLookupDetour),
                                          reinterpret_cast<void**>(&g_callLookupOriginal), why, sizeof why))
        {
            LOG_ERR("[faint] could not hook the mount call's check (%s), so a fainted Broomy waits as the game says.", why);
            return;
        }
        LOG("[faint] a call while Broomy is down brings a fresh one.");
    }
}

namespace bm::grant
{
    bool Install(bool give)
    {
        g_give = give;
        const uintptr_t send = mem::FindUnique(kSig_SendRequest);
        const uintptr_t hire = mem::FindUnique(kSig_ServerHire);
        const uintptr_t move = mem::FindUnique(kSig_ServerMoveHandler);
        const uintptr_t lookup = hire && mem::MatchAt(hire + kOff_Hire_LookupCallSig, kSig_HireLookupCall)
                                     ? CallTarget(hire + kOff_Hire_LookupCall) : 0;
        if (!send || !hire || !lookup || !move || !mem::MatchAt(lookup, kSig_ActorLookup))
        {
            LOG_ERR("[grant] the request sender (%s), the server's hire (%s), its actor lookup (%s) or the server's "
                    "movement handler (%s) was not found, so Broomy is not given this session.",
                    send ? "found" : "missing", hire ? "found" : "missing", lookup ? "found" : "missing",
                    move ? "found" : "missing");
            return false;
        }
        char why[96] = {}, why2[96] = {}, why3[96] = {}, why4[96] = {};
        const bool a = farhook::Install("request sender", send, reinterpret_cast<void*>(&SendDetour),
                                        reinterpret_cast<void**>(&g_sendOriginal), why, sizeof why);
        const bool b = a && farhook::Install("server hire", hire, reinterpret_cast<void*>(&HireDetour),
                                             reinterpret_cast<void**>(&g_hireOriginal), why2, sizeof why2);
        const bool c = b && farhook::Install("actor lookup", lookup, reinterpret_cast<void*>(&LookupDetour),
                                             reinterpret_cast<void**>(&g_lookupOriginal), why3, sizeof why3);
        const bool d = c && farhook::Install("server movement handler", move, reinterpret_cast<void*>(&MoveDetour),
                                             reinterpret_cast<void**>(&g_moveOriginal), why4, sizeof why4);
        if (!d)
        {
            LOG_ERR("[grant] could not hook (%s%s%s%s), so Broomy is not given this session.", why, why2, why3, why4);
            return false;
        }
        InstallCallCheck();
        LOG("[grant] ready: %u seconds after a save loads, the roster is checked for Broomy.",
            static_cast<unsigned>(kGrantDelayMs / 1000));
        return true;
    }
}

namespace bm::grant
{
    void Tick()
    {
        const DWORD now0 = GetTickCount();
        if (g_seenCount && g_loadSeq == g_checkedSeq && now0 - g_watchAt >= 2000)
        {
            g_watchAt = now0;
            uintptr_t vt[2] = {};
            if (mem::FindVtablesByName(kRtti_ServerClan, vt, 2) == 1 && RostersChanged(vt[0]))
            {
                g_seenCount = 0;
                InterlockedExchange(&g_armed, 0);
                InterlockedExchange(&g_loadedAt, static_cast<LONG>(now0));
                InterlockedIncrement(&g_loadSeq);
                LOG("[grant] the roster moved (a save loaded, or it grew); checking it again in %u seconds.",
                    static_cast<unsigned>(kGrantDelayMs / 1000));
            }
        }
        if (g_armed && !g_target && now0 - g_armedCheckAt >= kArmedRecheckMs)
        {
            g_armedCheckAt = now0;
            const int broomyRow = broomy::Row();
            int rosters = 0;
            if (broomyRow >= 0 && RosterHas(broomyRow, rosters))
            {
                InterlockedExchange(&g_armed, 0);
                InterlockedExchange(&g_candidate, 0);
                InterlockedExchange(&g_candidateRank, kNoRank);
                LOG("[grant] Broomy is in the roster after all (%d roster%s), so nothing is given.", rosters,
                    rosters == 1 ? "" : "s");
            }
        }
        const LONG seq = g_loadSeq;
        if (!seq || seq == g_checkedSeq) return;
        const DWORD now = GetTickCount();
        if (now - static_cast<DWORD>(g_loadedAt) < kGrantDelayMs || (g_retryAt && static_cast<LONG>(now - g_retryAt) < 0))
            return;
        const int row = broomy::Row();
        if (row < 0)
        {
            g_checkedSeq = seq;
            LOG_ERR("[grant] Broomy has no characterinfo row this session (see the [tables] lines), so there is nothing "
                    "to give.");
            return;
        }
        const DWORD t0 = GetTickCount();
        int rosters = 0;
        const bool has = RosterHas(row, rosters);
        const DWORD took = GetTickCount() - t0;
        if (!rosters)
        {
            g_retryAt = now + kThinRetryMs;
            LOG("[grant] no roster found in memory yet (%lu ms); looking again in 2 seconds.", took);
            return;
        }
        if (g_thinSeq != seq)
        {
            g_thinSeq = seq;
            g_thinChecks = 0;
        }
        if (!has && rosters < kRostersLoaded && g_thinChecks < kThinRetries)
        {
            ++g_thinChecks;
            g_retryAt = now + kThinRetryMs;
            LOG("[grant] only %d roster in memory and no Broomy in it; the save may still be loading, so looking again "
                "in 2 seconds.", rosters);
            return;
        }
        g_checkedSeq = seq;
        g_retryAt = 0;
        if (has)
        {
            g_lastNoBroomyAt = 0;
            LOG("[grant] Broomy (row %d) is already in this save's roster (%d roster%s looked through in %lu ms), so "
                "nothing is given.", row, rosters, rosters == 1 ? "" : "s", took);
            return;
        }
        if (!g_give)
        {
            LOG("[grant] this save has no Broomy (%d roster%s looked through in %lu ms), and GiveBroomy=0, so none is "
                "given.", rosters, rosters == 1 ? "" : "s", took);
            return;
        }
        // A load sends LoadingComplete up to four times about 5 s apart, and
        // each one disarms the grant until the next check (19 October: four
        // checks from 13:47:49 to 13:48:04, the hire at 13:48:09). A check
        // that follows another within kSameLoadMs carries on where that one
        // was, so the wait for a horse runs once per load. Once the tries are
        // used up a check starts over, since another save loaded within
        // kSameLoadMs would otherwise never be given Broomy (Miss() tells the
        // player the grant waits for the next load).
        const DWORD nowArm = GetTickCount();
        if (g_lastNoBroomyAt && nowArm - g_lastNoBroomyAt < kSameLoadMs && g_tries < kGrantTries)
        {
            g_lastNoBroomyAt = nowArm;
            g_armedCheckAt = nowArm;
            InterlockedExchange(&g_target, 0);
            InterlockedExchange(&g_armed, 1);
            return;
        }
        g_lastNoBroomyAt = nowArm;
        // The last wild animal seen while the save loaded stays the
        // candidate: a hire of one that has gone fails and the next is tried.
        InterlockedExchange(&g_tries, 0);
        InterlockedExchange(&g_target, 0);
        InterlockedExchange(&g_waitLogged, 0);
        for (volatile LONG& r : g_rejected) InterlockedExchange(&r, 0);
        for (volatile LONG& n : g_seenByList) InterlockedExchange(&n, 0);
        g_armedCheckAt = GetTickCount();
        InterlockedExchange(&g_armedAt, static_cast<LONG>(g_armedCheckAt));
        InterlockedExchange(&g_armed, 1);
        LOG("[grant] this save has no Broomy (%d roster%s looked through in %lu ms). A wild horse the server has "
            "loaded is hired as Broomy, or after %u seconds any unowned wild animal.", rosters, rosters == 1 ? "" : "s",
            took, static_cast<unsigned>(kHorseGraceMs / 1000));
    }
}
