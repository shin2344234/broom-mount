#include "game/equipfix.h"

#include <Windows.h>
#include <cstdio>

#include "core/log.h"
#include "game/farhook.h"
#include "game/mem.h"

namespace
{
    constexpr const char* kSig_EquipCopy =
        "48 89 54 24 10 53 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 68 4C 8B F2 48 8B E9 4C 8B 61 08";
    constexpr unsigned kOff_Slots = 0xA0;     // the component's equipment list (0x90 on 2976)
    constexpr unsigned kOff_Entries = 0x08;
    constexpr unsigned kOff_Count = 0x10;
    constexpr unsigned kEntrySize = 0xD0;     // first u32 the item key
    constexpr uint32_t kNoItem = 0xFFFFFFFFu;

    // The same copy from a list straight into a given array, reached through
    // the thunk at +0x20CEB90 from the server's mount call (+0x2BAEF5F, both
    // on 2976; the copy is +0xCCCF700 on 3392):
    // (list, array).
    constexpr const char* kSig_EquipCopyTo =
        "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 31 DB 48 89 D7 48 89 CE 39 5A 08 76 1C 0F 1F 40 00 "
        "89 D8 48 69 C8 C8 00 00";

    typedef uintptr_t (*FnCopy)(uintptr_t self, uintptr_t arg);
    FnCopy g_original = nullptr;
    FnCopy g_copyToOriginal = nullptr;
    volatile LONG g_dropped = 0;

    uint32_t WithoutTrailingBlanks(uintptr_t entries, uint32_t n)
    {
        while (n > 0)
        {
            uint32_t item = 0;
            if (!bm::mem::Read32(entries + (n - 1) * kEntrySize, &item) || item != kNoItem) break;
            --n;
        }
        return n;
    }

    // The copies only read the entries, so the count is lowered for the call
    // and put back after.
    uintptr_t Guarded(FnCopy original, uintptr_t list, uintptr_t a, uintptr_t b)
    {
        uintptr_t entries = 0;
        uint32_t n = 0;
        if (!bm::mem::ReadPtr(list + kOff_Entries, &entries) || !bm::mem::Read32(list + kOff_Count, &n) || n == 0 ||
            n > 64)
            return original(a, b);
        const uint32_t keep = WithoutTrailingBlanks(entries, n);
        if (keep == n) return original(a, b);
        if (InterlockedIncrement(&g_dropped) <= 8)
        {
            uint8_t raw[kEntrySize] = {};
            bm::mem::ReadBytes(entries + (n - 1) * kEntrySize, raw, sizeof raw);
            char hex[kEntrySize * 3 + 1] = {};
            for (unsigned i = 0; i < kEntrySize; ++i) snprintf(hex + i * 3, 4, "%02X ", raw[i]);
            LOG("[equip] %u blank equipment entr%s (item FFFFFFFF) left out of a copy of %u (%s); the last: %s", n - keep,
                n - keep == 1 ? "y" : "ies", n, original == g_original ? "into the component" : "into an array", hex);
        }
        uint32_t* count = reinterpret_cast<uint32_t*>(list + kOff_Count);
        *count = keep;
        const uintptr_t r = original(a, b);
        *count = n;
        return r;
    }

    uintptr_t Detour(uintptr_t self, uintptr_t arg)
    {
        uintptr_t list = 0;
        if (!bm::mem::ReadPtr(self + kOff_Slots, &list)) return g_original(self, arg);
        return Guarded(g_original, list, self, arg);
    }

    uintptr_t CopyToDetour(uintptr_t list, uintptr_t out) { return Guarded(g_copyToOriginal, list, list, out); }
}

namespace bm::equipfix
{
    bool Install()
    {
        const uintptr_t fn = mem::FindUnique(kSig_EquipCopy);
        char why[96] = "not found";
        if (!fn || !farhook::Install("equipment copy", fn, reinterpret_cast<void*>(&Detour),
                                     reinterpret_cast<void**>(&g_original), why, sizeof why))
        {
            LOG_ERR("[equip] the equipment copy is not guarded (%s), so a summon of Broomy can crash.", why);
            return false;
        }
        const uintptr_t to = mem::FindUnique(kSig_EquipCopyTo);
        char why2[96] = "not found";
        if (!to || !farhook::Install("equipment copy to array", to, reinterpret_cast<void*>(&CopyToDetour),
                                     reinterpret_cast<void**>(&g_copyToOriginal), why2, sizeof why2))
        {
            LOG_ERR("[equip] the second equipment copy is not guarded (%s), so a summon of Broomy can crash.", why2);
            return false;
        }
        LOG("[equip] guarding both equipment copies, +0x%llX and +0x%llX.", static_cast<unsigned long long>(mem::Rva(fn)),
            static_cast<unsigned long long>(mem::Rva(to)));
        return true;
    }
}
