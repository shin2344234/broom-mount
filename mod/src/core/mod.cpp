#include "core/mod.h"

#include <atomic>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/paths.h"
#include "game/broomblend.h"
#include "game/broomclips.h"
#include "game/broomchart.h"
#include "game/analogspeed.h"
#include "game/broomrows.h"
#include "game/broomy.h"
#include "game/callgate.h"
#include "game/captivepatches.h"
#include "game/damianeclips.h"
#include "game/equipfix.h"
#include "game/farhook.h"
#include "game/gamefile.h"
#include "game/grant.h"
#include "game/leanblend.h"
#include "game/mem.h"
#include "game/aimrate.h"
#include "game/riderfix.h"
#include "ini_default.h"
#include "version.h"

namespace
{
    std::atomic<bool> g_stop{false};
    HANDLE g_thread = nullptr;

    // The game is not the only process that loads this plugin.
    // crashpad_handler.exe does too, with a 671,744-byte image.
    constexpr size_t kMinGameImage = 64ull * 1024 * 1024;

    bool IsGame() { return bm::mem::Game().base && bm::mem::Game().size >= kMinGameImage; }

    int Clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

    int ReadSetting(const wchar_t* key, int fallback)
    {
        return static_cast<int>(GetPrivateProfileIntW(L"settings", key, fallback, bm::Paths::File(BM_INI).c_str()));
    }

    // A DMM install is the plugin on its own, so there is no ini beside it.
    // Write the documented one out when there is none. An existing file is
    // never touched.
    void WriteDefaultIni()
    {
        const std::wstring path = bm::Paths::File(BM_INI);
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return;
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f)
        {
            LOG("[ini] %ls could not be written; every default is compiled in, so the plugin still runs.", BM_INI);
            return;
        }
        const bool ok = fwrite(kDefaultIni, 1, kDefaultIniSize, f) == kDefaultIniSize;
        fclose(f);
        LOG(ok ? "[ini] no %ls beside the plugin, so one was written with every setting at its default."
               : "[ini] %ls was created but not written in full. Delete it and it will be written again.", BM_INI);
    }

    bool g_serving = false;

    // Broomy's riding blend (broomblend.h): the broom's own clips, laid out
    // for Broomy's speeds so its nose pitches with the angle it flies at.
    // Every blend path in Broomy's charts names this file. Kliff's
    // broom_rider_move gets the same layout with rings of its own, so his
    // lean follows Broomy's speed.
    const std::string g_blend(reinterpret_cast<const char*>(bm::broomchart::kBroomBlendBytes),
                              sizeof bm::broomchart::kBroomBlendBytes);
    const std::string g_riderBlend(reinterpret_cast<const char*>(bm::broomchart::kRiderBlendBytes),
                                   sizeof bm::broomchart::kRiderBlendBytes);
    constexpr char kBlendLeaf[] = "/broom_riding_move.motionblending";
    constexpr char kRiderBlendLeaf[] = "/broom_rider_move.motionblending";

    // Damiane's copy of Kliff's lean blend, which her nodes in his riding
    // charts name (damianepatches.h): his rings, her clips (damianeclips.h)
    // and her skeleton, phw_01, as the game's own phw_ blends name it. No
    // pack holds it, so find and exists see his in its place.
    constexpr char kHerBlend[] = "character/binary/motionblending/phw_locomotion/phw_broom_rider_move.motionblending";
    constexpr char kHisBlend[] = "character/binary/motionblending/phm/broom_rider_move.motionblending";
    constexpr char kHisClips[] = "1_pc/1_phm/00_riding/cd_phm_rd_broom", kHerClips[] = "1_pc/2_phw/00_riding/cd_phw_rd_broom";
    constexpr char kHisSkeleton[] = "character/model/1_pc/1_phm/phm_01.pab",
                   kHerSkeleton[] = "character/model/1_pc/2_phw/phw_01.pab";
    static_assert(sizeof kHisClips == sizeof kHerClips && sizeof kHisSkeleton == sizeof kHerSkeleton,
                  "a name in the blend keeps its length");

    std::string Renamed(std::string bytes, const char* from, const char* to)
    {
        for (size_t at = bytes.find(from); at != std::string::npos; at = bytes.find(from, at))
            bytes.replace(at, strlen(to), to);
        return bytes;
    }

    const std::string g_herBlend = Renamed(Renamed(g_riderBlend, kHisClips, kHerClips), kHisSkeleton, kHerSkeleton);

    // The clips Broomy carries: the broom's level takeoff and hover idle,
    // Kliff's push-off and leans, every broom clip with its cargo seat behind
    // Kliff (broomclips.h), Damiane's (damianeclips.h), and a bounty lying
    // across the broom (captivepatches.h).
    // A clip no pack holds has a stand-in, a shipped file find and exists see
    // in its place; the hover replaces the idle.
    struct Carried
    {
        const bm::broomchart::BroomClip* clip;
        std::string bytes;
    };
    const std::vector<Carried> g_clips = [] {
        std::vector<Carried> v;
        for (const bm::broomchart::BroomClip& c : bm::broomchart::kBroomClips)
            v.push_back({ &c, std::string(reinterpret_cast<const char*>(c.data), c.size) });
        for (const bm::broomchart::BroomClip& c : bm::damiane::kClips)
            v.push_back({ &c, std::string(reinterpret_cast<const char*>(c.data), c.size) });
        for (const bm::broomchart::BroomClip& c : bm::captive::kClips)
            v.push_back({ &c, std::string(reinterpret_cast<const char*>(c.data), c.size) });
        return v;
    }();

    bool EndsWithI(const char* path, size_t n, const char* tail)
    {
        const size_t t = strlen(tail);
        return n >= t && _stricmp(path + n - t, tail) == 0;
    }

    // Called on every async load, Read, find and existence check.
    const std::string* Supply(const char* path, std::string* standIn)
    {
        const size_t n = strlen(path);
        if (EndsWithI(path, n, kHerBlend))
        {
            if (standIn) *standIn = kHisBlend;
            static volatile LONG logged = 0;
            if (!standIn && InterlockedIncrement(&logged) <= 4) LOG("[blend] Damiane's lean blend goes in for %s.", path);
            return &g_herBlend;
        }
        const bool rider = EndsWithI(path, n, kRiderBlendLeaf);
        if (rider || EndsWithI(path, n, kBlendLeaf))
        {
            static volatile LONG logged = 0;
            if (InterlockedIncrement(&logged) <= 8)
                LOG("[blend] %s goes in for %s.", rider ? "Kliff's lean blend" : "Broomy's riding blend", path);
            return rider ? &g_riderBlend : &g_blend;
        }
        for (const Carried& k : g_clips)
        {
            const bm::broomchart::BroomClip& c = *k.clip;
            if (!EndsWithI(path, n, c.path)) continue;
            if (standIn && c.standIn) *standIn = c.standIn;
            static volatile LONG logged = 0;
            if (!standIn && InterlockedIncrement(&logged) <= 60)
                LOG("[clips] Broomy's own %s goes in for %s.", strrchr(c.path, '/') + 1, path);
            return &k.bytes;
        }
        return nullptr;
    }

    DWORD WINAPI Worker(LPVOID)
    {
        WriteDefaultIni();
        const bool give = ReadSetting(L"GiveBroomy", 1) != 0;
        LOG("[mod] %s %s for Crimson Desert exe 1.0.0.3424. GiveBroomy=%d", BM_NAME, BM_VERSION,
            give ? 1 : 0);
        if (!g_serving)
            LOG_ERR("[mod] Broomy's files are not being served (see the [files] line above), so Broomy does not exist "
                    "this session.");
        const bool granting = g_serving && bm::grant::Install(give);
        if (g_serving) bm::callgate::Install();
        if (g_serving) bm::equipfix::Install();
        if (g_serving) bm::leanblend::Install();
        bm::analogspeed::SetSlowest(ReadSetting(L"SlowestPush", 15));
        if (g_serving) bm::analogspeed::Install();
        if (g_serving && !give) LOG("[grant] GiveBroomy=0, so no save is given Broomy.");
        while (!g_stop.load())
        {
            Sleep(500);
            if (granting) bm::grant::Tick();
        }
        return 0;
    }
}

namespace bm::Mod
{
    void EarlyInstall(HMODULE module)
    {
        // On the loading thread, before the game's own start-up code runs,
        // so the files it reads at boot are seen too.
        Paths::Init(module);
        if (!IsGame()) return;
        bm::broomchart::Speeds speeds;
        speeds.ground = Clamp(ReadSetting(L"GroundSpeed", speeds.ground), 10, 1000);
        speeds.flight = Clamp(ReadSetting(L"FlightSpeed", speeds.flight), 10, 1000);
        speeds.boost = Clamp(ReadSetting(L"BoostSpeed", speeds.boost), 100, 500);
        speeds.climb = Clamp(ReadSetting(L"ClimbSpeed", speeds.climb), 10, 1000);
        bm::broomchart::SetSpeeds(speeds);
        g_serving = bm::broomy::Install(bm::broomy::kChartsPadded);
        if (g_serving) bm::gamefile::SupplyLoads(&Supply);
        if (g_serving) bm::riderfix::Install(false);
        if (g_serving) bm::aimrate::Install();
    }

    void Initialize(HMODULE module)
    {
        Paths::Init(module);
        if (!IsGame())
        {
            wchar_t name[96];
            _snwprintf_s(name, _countof(name), _TRUNCATE, L"%s.other", BM_FILEBASE);
            Log::ClaimSingle(name);
            wchar_t exe[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            const wchar_t* leaf = wcsrchr(exe, L'\\');
            LOG("[mod] %ls (pid %lu) is not the game, so nothing is done here. The game's own log is %ls.log.",
                leaf ? leaf + 1 : exe, GetCurrentProcessId(), BM_FILEBASE);
            Log::Shutdown();
            return;
        }
        Log::Claim(BM_FILEBASE);
        g_thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    }

    void Shutdown(bool processExiting)
    {
        g_stop.store(true);
        if (processExiting)
        {
            Log::Shutdown();
            return;
        }
        bm::analogspeed::Stop();
        if (g_thread)
        {
            WaitForSingleObject(g_thread, 3000);
            CloseHandle(g_thread);
            g_thread = nullptr;
        }
        farhook::RemoveAll();
        Log::Shutdown();
    }
}
