#include "game/aimrate.h"

#include <Windows.h>
#include <cstdint>
#include <cstring>

#include "core/log.h"
#include "game/farhook.h"
#include "game/hash.h"
#include "game/mem.h"

namespace
{
    // In the AimIK apply, with rdi the modifier, xmm7 the target distance
    // and xmm8 the smoothing rate (the event's, read from [rdi+0x84] and
    // reset there to -1 at once, or 10 when it holds nothing):
    //   +0x12DB749  vxorps  xmm1, xmm1, xmm1
    //   +0x12DB74D  vcomiss xmm7, xmm1
    //   +0x12DB751  jbe     +0x12DB793        ; no distance: rate stays as it is
    //   +0x12DB753  vcomiss xmm8, xmm10       ; ... rate += min(|lag|, 8) / 8 * (4 - rate)
    //   +0x12DB793  mov     r9, rbp           ; the solve, which smooths at rate xmm8
    // (2976: +0x111A0C5, the pull `vcomiss xmm8, xmm9` and the solve
    // `mov r9, r14` at +0x111A10F.) [rdi+0x60] is the hash of the aim's
    // reference bone (looked up at +0x12DB461). For the broom it is B_Body_00, which ikpatches.h puts in
    // the RideOn and body charts in place of the Wyvern's B_IK_Head_00. The
    // game used one of the two spellings' hashes, so both are checked. The
    // broom's aim gets kRate on every frame and skips the pull; every other
    // aim IK runs the shipped instructions.
    constexpr uintptr_t kRva_Site = 0x12DB749;
    constexpr uintptr_t kRva_Pull = 0x12DB753;
    constexpr uintptr_t kRva_Solve = 0x12DB793;
    constexpr unsigned char kSite[] = { 0xC5, 0xF0, 0x57, 0xC9, 0xC5, 0xF8, 0x2F, 0xF9, 0x76, 0x40 };
    constexpr unsigned char kPullHead[] = { 0xC4, 0x41, 0x78, 0x2F, 0xC2 };
    constexpr float kRate = 1.0f;   // per second: a 90 degree swing is 90 percent done in 2.3 s

    unsigned AbsJump(unsigned char* p, uintptr_t to)
    {
        p[0] = 0xFF; p[1] = 0x25; p[2] = p[3] = p[4] = p[5] = 0;
        memcpy(p + 6, &to, 8);
        return 14;
    }
}

namespace bm::aimrate
{
    bool Install()
    {
        const uintptr_t base = bm::mem::Game().base;
        unsigned char pull[sizeof kPullHead] = {};
        if (!bm::mem::ReadBytes(base + kRva_Pull, pull, sizeof pull) || memcmp(pull, kPullHead, sizeof pull) != 0)
        {
            LOG_ERR("[aim] the aim IK's rate code is not at +0x%llX on this exe, so the broom turns to the camera at "
                    "the game's speed.", static_cast<unsigned long long>(kRva_Pull));
            return false;
        }
        const uint32_t body = bm::hash::Little("b_body_00");
        const uint32_t bodyAsWritten = bm::hash::Little("B_Body_00");
        uint32_t rate;
        memcpy(&rate, &kRate, 4);

        unsigned char c[96];
        memset(c, 0xCC, sizeof c);
        const unsigned char head[] = {
            0xC5, 0xF0, 0x57, 0xC9,                     // vxorps  xmm1, xmm1, xmm1
            0x81, 0x7F, 0x60, 0, 0, 0, 0,               // cmp     dword [rdi+0x60], hash(B_Body_00)
            0x74, 9,                                    // je      broom
            0x81, 0x7F, 0x60, 0, 0, 0, 0,               // cmp     dword [rdi+0x60], hash(b_body_00)
            0x75, 0,                                    // jne     other
        };
        memcpy(c, head, sizeof head);
        memcpy(c + 7, &bodyAsWritten, 4);
        memcpy(c + 16, &body, 4);
        unsigned n = sizeof head;
        const unsigned jneAt = n - 1;
        // broom: the rate is kRate, and the pull is skipped.
        c[n++] = 0xB8; memcpy(c + n, &rate, 4); n += 4;                      // mov     eax, kRate
        c[n++] = 0xC5; c[n++] = 0x79; c[n++] = 0x6E; c[n++] = 0xC0;          // vmovd   xmm8, eax
        n += AbsJump(c + n, base + kRva_Solve);
        // other: the shipped instructions.
        c[jneAt] = static_cast<unsigned char>(n - (jneAt + 1));
        c[n++] = 0xC5; c[n++] = 0xF8; c[n++] = 0x2F; c[n++] = 0xF9;          // vcomiss xmm7, xmm1
        c[n++] = 0x76; c[n++] = 14;                                         // jbe     solve
        n += AbsJump(c + n, base + kRva_Pull);
        n += AbsJump(c + n, base + kRva_Solve);

        char why[160] = {};
        if (!bm::farhook::InstallBranch("aim rate", base + kRva_Site, kSite, sizeof kSite, c, n, nullptr, why,
                                        sizeof why))
        {
            LOG_ERR("[aim] could not patch the aim IK's rate: %s", why);
            return false;
        }
        LOG("[aim] the broom's aim (B_Body_00, hash %08X or %08X) turns to the camera at %.2f per second, without the "
            "distance pull.", bodyAsWritten, body, kRate);
        return true;
    }
}
