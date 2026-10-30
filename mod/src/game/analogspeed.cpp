#include "game/analogspeed.h"

#include <Windows.h>
#include <Xinput.h>
#include <atomic>
#include <cmath>

#include "core/log.h"
#include "game/farhook.h"
#include "game/mem.h"
#include "game/speedpatches.h"

namespace
{
    // The movement update (+0xBC41D0: object, seconds, speed record out,
    // flag out) has one caller, +0xBC4930, which calls it at +0xBC4B6E.
    // The pattern starts at +0xBC4B60 (the lea's register differs from
    // 2976's); the call is 14 bytes in.
    constexpr const char* kSig_MoveUpdateCall =
        "4D 8D ?? 04 01 00 00 4C 8D 45 B0 48 8B CF E8 ?? ?? ?? ?? C5 7A 10 55 C4";
    constexpr unsigned kCallOffset = 14;

    // The speed record's word 6 (+0x18) is the state's speed in m/s. The
    // caller keeps the speed it started moving at in the object at +0x11C
    // (-1 while stopped, set at +0xBC53E6) and only lowers it on a timer
    // (+0x114) that does not run out in flight; it takes the record's speed
    // whenever that is higher or the latch is -1. 3392 moved the object's
    // timers and latch by 8: on 2976 the latch was +0x114, which is now the
    // timer the game zeroes.
    constexpr unsigned kRecordSpeed = 0x18;
    constexpr unsigned kLatch = 0x11C;
    constexpr float kUnlatched = -1.0f;

    // XInput's own left stick dead zone, and the push that counts as full:
    // a stick held hard on a diagonal often stops short of 1.
    constexpr float kDeadZone = XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE / 32767.0f;
    constexpr float kFullPush = 0.92f;
    // The speed at the lightest push, as a share of the full one.
    float g_slowest = 0.15f;

    typedef uint64_t (*FnMoveUpdate)(uintptr_t self, float seconds, uintptr_t record, uintptr_t flag);
    FnMoveUpdate g_original = nullptr;

    constexpr int kCharts = 3;
    std::atomic<uintptr_t> g_lo[kCharts] = {};
    std::atomic<uintptr_t> g_hi[kCharts] = {};

    // Left stick push, 0 to 1, or -1 with no pad connected.
    std::atomic<float> g_push{ -1.0f };
    std::atomic<bool> g_stop{ false };
    HANDLE g_thread = nullptr;

    typedef DWORD(WINAPI* FnXInputGetState)(DWORD, XINPUT_STATE*);
    FnXInputGetState g_getState = nullptr;

    std::atomic<bool> g_boostSeen{ false };

    // Which of Broomy's charts holds p, with p's offset in it, or -1.
    int ChartOf(uintptr_t p, uint32_t* offset)
    {
        for (int i = 0; i < kCharts; ++i)
        {
            const uintptr_t lo = g_lo[i].load(std::memory_order_relaxed);
            if (lo && p >= lo && p < g_hi[i].load(std::memory_order_relaxed))
            {
                *offset = static_cast<uint32_t>(p - lo);
                return i;
            }
        }
        return -1;
    }

    // Seth, 2 October: "make the boost always full speed".
    bool IsBoost(int chart, uint32_t offset)
    {
        for (const bm::broomchart::NodeRange& n : bm::broomchart::kBoostNodes)
            if (n.chart == chart && offset >= n.begin && offset < n.end) return true;
        return false;
    }

    // The node header the record comes from, by the same chain the movement
    // update walks before it fetches the record (+0xBC427F to +0xBC42B3),
    // then the copier's [node+0x28] (+0x2146B9F; [node+0x38] on 2976).
    uintptr_t NodeHeader(uintptr_t self)
    {
        __try
        {
            const uintptr_t lower = *reinterpret_cast<uintptr_t*>(self + 0x18);
            if (!lower) return 0;
            const uintptr_t chart = *reinterpret_cast<uintptr_t*>(lower + 0x30);
            if (!chart) return 0;
            const uintptr_t a = *reinterpret_cast<uintptr_t*>(chart);
            const uintptr_t b = a ? *reinterpret_cast<uintptr_t*>(a + 0x68) : 0;
            const uintptr_t layers = b ? *reinterpret_cast<uintptr_t*>(b + 0x40) : 0;
            if (!layers) return 0;
            const uintptr_t t = *reinterpret_cast<uintptr_t*>(layers + 0x30);
            if (!t) return 0;
            const uint8_t index = *reinterpret_cast<uint8_t*>(t + 0x1B);
            const uintptr_t layer = *reinterpret_cast<uintptr_t*>(layers + index * 8 + 0x88);
            const uintptr_t node = layer ? *reinterpret_cast<uintptr_t*>(layer + 0x18) : 0;
            return node ? *reinterpret_cast<uintptr_t*>(node + 0x28) : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    // How much of the full speed this push gives, or 1 when the stick is not
    // in use (no pad, or moving on the keyboard).
    float Share()
    {
        const float push = g_push.load(std::memory_order_relaxed);
        if (push < kDeadZone || g_slowest >= 1.0f) return 1.0f;
        float t = (push - kDeadZone) / (kFullPush - kDeadZone);
        if (t > 1.0f) t = 1.0f;
        return g_slowest + (1.0f - g_slowest) * t;
    }

    uint64_t Detour(uintptr_t self, float seconds, uintptr_t record, uintptr_t flag)
    {
        const uint64_t r = g_original(self, seconds, record, flag);
        uint32_t offset = 0;
        const int chart = ChartOf(NodeHeader(self), &offset);
        if (chart < 0) return r;
        if (IsBoost(chart, offset))
        {
            if (!g_boostSeen.exchange(true)) LOG("[speed] boosting, which stays at full speed whatever the stick does.");
            return r;
        }
        float* speed = reinterpret_cast<float*>(record + kRecordSpeed);
        const float full = *speed;
        if (!(full > 0.0f)) return r;
        const float share = Share();
        if (share >= 1.0f) return r;
        *speed = full * share;
        float* latch = reinterpret_cast<float*>(self + kLatch);
        if (*latch != kUnlatched && *latch > *speed) *latch = kUnlatched;
        return r;
    }

    // Reads the first connected pad's left stick about 120 times a second.
    // XInputGetState on an empty slot is slow, so empty slots are only
    // tried again every two seconds.
    DWORD WINAPI Poll(LPVOID)
    {
        int pad = -1;
        ULONGLONG lastScan = 0;
        while (!g_stop.load())
        {
            XINPUT_STATE st = {};
            if (pad >= 0 && g_getState(static_cast<DWORD>(pad), &st) != ERROR_SUCCESS) pad = -1;
            if (pad < 0)
            {
                g_push.store(-1.0f, std::memory_order_relaxed);
                const ULONGLONG now = GetTickCount64();
                if (now - lastScan >= 2000)
                {
                    lastScan = now;
                    for (DWORD i = 0; i < XUSER_MAX_COUNT && pad < 0; ++i)
                        if (g_getState(i, &st) == ERROR_SUCCESS)
                        {
                            pad = static_cast<int>(i);
                            LOG("[speed] reading the left stick of pad %d.", pad);
                        }
                }
                if (pad < 0)
                {
                    Sleep(100);
                    continue;
                }
            }
            const float x = st.Gamepad.sThumbLX / 32767.0f, y = st.Gamepad.sThumbLY / 32767.0f;
            float push = std::sqrt(x * x + y * y);
            if (push > 1.0f) push = 1.0f;
            g_push.store(push, std::memory_order_relaxed);
            Sleep(8);
        }
        return 0;
    }
}

namespace bm::analogspeed
{
    void NoteChart(int index, uintptr_t data, uint32_t size)
    {
        if (index < 0 || index >= kCharts) return;
        g_lo[index].store(0);
        g_hi[index].store(data + size);
        g_lo[index].store(data);
    }

    void SetSlowest(int percent)
    {
        g_slowest = (percent < 5 ? 5 : percent > 100 ? 100 : percent) / 100.0f;
    }

    bool Install()
    {
        if (g_slowest >= 1.0f)
        {
            LOG("[speed] SlowestPush=100, so the stick does not change Broomy's speed.");
            return true;
        }
        HMODULE xinput = LoadLibraryW(L"xinput1_4.dll");
        if (!xinput) xinput = LoadLibraryW(L"xinput9_1_0.dll");
        if (xinput) g_getState = reinterpret_cast<FnXInputGetState>(GetProcAddress(xinput, "XInputGetState"));
        if (!g_getState)
        {
            LOG_ERR("[speed] XInput is missing, so Broomy flies at full speed whatever the stick does.");
            return false;
        }
        size_t hits = 0;
        const uintptr_t at = mem::FindUnique(kSig_MoveUpdateCall, &hits);
        if (!at)
        {
            LOG_ERR("[speed] the movement update's call was found %zu times, so Broomy flies at full speed whatever "
                    "the stick does.", hits);
            return false;
        }
        char why[128] = {};
        if (!farhook::InstallOverJump("movement update call", at + kCallOffset, reinterpret_cast<void*>(&Detour),
                                      reinterpret_cast<void**>(&g_original), why, sizeof why))
        {
            LOG_ERR("[speed] could not hook the movement update's call: %s", why);
            return false;
        }
        g_thread = CreateThread(nullptr, 0, &Poll, nullptr, 0, nullptr);
        LOG("[speed] Broomy's speed follows the left stick: %.0f%% at the lightest push, full from %.0f%%.",
            g_slowest * 100.0f, kFullPush * 100.0f);
        return true;
    }

    void Stop()
    {
        g_stop.store(true);
    }
}
