#include "game/broomy.h"

#include <Windows.h>
#include <cstring>
#include <string>

#include "core/log.h"
#include "game/analogspeed.h"
#include "game/broomchart.h"
#include "game/broomrows.h"
#include "game/captivepatches.h"
#include "game/damiane.h"
#include "game/gamefile.h"
#include "game/riderfix.h"
#include "game/tablefile.h"

namespace
{
    using namespace bm::broomrows;

    constexpr const char* kHeaderExt = ".staticinfoheader";
    constexpr const char* kBodyExt = ".staticinfobody";
    constexpr int kTableCount = sizeof kTables / sizeof kTables[0];

    struct Half { size_t size = 0; uint32_t crc = 0; };
    struct Entry
    {
        SRWLOCK lock = SRWLOCK_INIT;
        bool tried = false;
        bool ok = false;
        Half shipped[2];        // header, body: what ours was built from
        std::string ours[2];
        volatile LONG served[2] = {};
    };
    Entry g_entries[kTableCount];

    SRWLOCK g_factsLock = SRWLOCK_INIT;
    CharacterFacts g_facts;
    volatile LONG g_broomyList = -1;

    bool EndsWith(const char* s, size_t n, const char* tail)
    {
        const size_t t = strlen(tail);
        return n >= t && _stricmp(s + n - t, tail) == 0;
    }

    // Broomy's charts, built together the first time the game reads any of
    // them, since the three share one set of aliases. Once ready, `made` and
    // `aliases` never change again.
    using bm::broomchart::kAttacks;
    using bm::broomchart::kChartCount;
    using bm::broomchart::kCharts;
    using bm::broomchart::kPackageDesc;
    constexpr int kAttackCount = sizeof kAttacks / sizeof kAttacks[0];
    bool g_ownCharts = false;
    int  g_chartMode = bm::broomy::kChartsWyvern;
    SRWLOCK g_chartsLock = SRWLOCK_INIT;
    bool g_chartsTried = false;
    volatile LONG g_chartsReady = 0;
    std::string g_made[kChartCount];
    bm::broomchart::Aliases g_aliases;

    int ChartOf(const char* path)
    {
        for (int i = 0; i < kChartCount; ++i)
            if (_stricmp(path, kCharts[i].path) == 0) return i;
        return -1;
    }

    int AttackOf(const char* path)
    {
        for (int i = 0; i < kAttackCount; ++i)
            if (_stricmp(path, kAttacks[i].path) == 0) return i;
        return -1;
    }

    // A file name the charts' aliases could have: "bm" or "bb", three digits.
    bool AliasLeaf(const char* path)
    {
        const char* slash = strrchr(path, '/');
        const char* leaf = slash ? slash + 1 : path;
        return leaf[0] == 'b' && (leaf[1] == 'm' || leaf[1] == 'b') && leaf[2] >= '0' && leaf[2] <= '9';
    }

    // Kliff's riding charts with Damiane's nodes (damiane.h), and a bounty's
    // chart with his lying idle on Broomy (captivepatches.h), each served only
    // onto the file it was made for.
    const bm::damiane::Chart* const kSpliced[] = { &bm::damiane::kCharts[0], &bm::damiane::kCharts[1],
                                                   &bm::captive::kChart };
    static_assert(sizeof bm::damiane::kCharts / sizeof bm::damiane::kCharts[0] == 2, "a chart is missing from kSpliced");
    constexpr int kSplicedCount = sizeof kSpliced / sizeof kSpliced[0];
    constexpr int kCaptiveChart = kSplicedCount - 1;
    volatile LONG g_loggedSpliced[kSplicedCount] = {};

    int SplicedChartOf(const char* path)
    {
        for (int i = 0; i < kSplicedCount; ++i)
            if (_stricmp(path, kSpliced[i]->path) == 0) return i;
        return -1;
    }

    // Only inside Serve, where the game's loader can be asked for the
    // Wyvern's charts.
    bool EnsureCharts()
    {
        AcquireSRWLockExclusive(&g_chartsLock);
        if (!g_chartsTried)
        {
            g_chartsTried = true;
            std::string wyvern[kChartCount], report, why;
            bool read = true;
            for (int c = 0; c < kChartCount && read; ++c)
                if (!bm::gamefile::ReadFile(kCharts[c].source, wyvern[c]))
                {
                    LOG_ERR("[charts] %s did not read, so Broomy has GoldStar's own charts and will not fly.",
                            kCharts[c].source);
                    read = false;
                }
            if (read && bm::broomchart::BuildCharts(wyvern, g_made, g_aliases, report, why,
                                                    g_chartMode == bm::broomy::kChartsPadded))
            {
                InterlockedExchange(&g_chartsReady, 1);
                LOG("[charts] Broomy's charts are the Wyvern's with the broom's animations: %s.", report.c_str());
            }
            else if (read)
                LOG_ERR("[charts] Broomy's charts could not be made (%s), so it has GoldStar's own and will not fly.",
                        why.c_str());
        }
        ReleaseSRWLockExclusive(&g_chartsLock);
        return g_chartsReady != 0;
    }

    volatile LONG g_loggedCharts[kChartCount] = {}, g_loggedPackages = 0;

    // The package list grows by one sub-package, so it is served as bytes;
    // its loader releases the buffer the standard way.
    bool ServeChartFile(const char* path, bool found, const std::string& game, std::string& out)
    {
        if (_stricmp(path, kPackageDesc) != 0 || !found) return false;
        std::string why, report;
        if (!bm::broomchart::BuildPackageDesc(game, out, report, why))
        {
            LOG_ERR("[charts] the package list is the game's own (%s), so Broomy cannot mount.", why.c_str());
            return false;
        }
        if (InterlockedExchange(&g_loggedPackages, 1) == 0) LOG("[charts] package list: %s.", report.c_str());
        return true;
    }

    // The chart loader frees a chart's buffer itself, with the heap it
    // expects, so a chart is read into the game's own buffer from the
    // Wyvern's file and patched in place: the aliases never change a
    // chart's size. The attack tables and the aliases' metadata are read
    // from their sources the same way.
    bool ReadInstead(const char* path, std::string& instead, std::string& fallback)
    {
        if (!g_ownCharts) return false;
        const int c = ChartOf(path);
        if (c >= 0)
        {
            // Copied: the Wyvern's charts as they are, nothing to build.
            if (g_chartMode != bm::broomy::kChartsCopied && !EnsureCharts()) return false;
            instead = kCharts[c].source;
            return true;
        }
        const int a = AttackOf(path);
        if (a >= 0)
        {
            instead = kAttacks[a].source;
            return true;
        }
        const size_t n = strlen(path);
        return g_chartsReady && EndsWith(path, n, ".paa_metabin") && AliasLeaf(path) &&
               bm::broomchart::Redirect(g_aliases, path, instead, &fallback);
    }

    void Patch(const char* path, uint8_t* data, uint32_t size)
    {
        const int c = ChartOf(path);
        if (c < 0 || !g_chartsReady) return;
        if (size != g_made[c].size())
        {
            LOG_ERR("[charts] %s read %u bytes where the broom's chart has %zu, so it stays the Wyvern's.", path, size,
                    g_made[c].size());
            return;
        }
        memcpy(data, g_made[c].data(), size);
        bm::analogspeed::NoteChart(c, reinterpret_cast<uintptr_t>(data), size);
        bm::riderfix::NoteServed(path, reinterpret_cast<uintptr_t>(data), size);
        if (InterlockedExchange(&g_loggedCharts[c], 1) == 0)
            LOG("[charts] %s is the broom's (%u bytes, from %s) at %p.", path, size, kCharts[c].source, data);
    }

    bool RedirectLoad(const char* path, std::string& to)
    {
        return g_chartsReady && AliasLeaf(path) && bm::broomchart::Redirect(g_aliases, path, to);
    }

    // Which table and half a path is, or -1.
    int TableOf(const char* path, int& half)
    {
        const size_t dir = strlen(kTableDir);
        if (_strnicmp(path, kTableDir, dir) != 0) return -1;
        const char* leaf = path + dir;
        const size_t n = strlen(leaf);
        size_t stem;
        if (EndsWith(leaf, n, kHeaderExt)) half = 0, stem = n - strlen(kHeaderExt);
        else if (EndsWith(leaf, n, kBodyExt)) half = 1, stem = n - strlen(kBodyExt);
        else return -1;
        for (int i = 0; i < kTableCount; ++i)
            if (strlen(kTables[i]) == stem && _strnicmp(leaf, kTables[i], stem) == 0) return i;
        return -1;
    }

    constexpr int kMapIconFileCount = sizeof kMapIconFiles / sizeof kMapIconFiles[0];

    // Which map markup or style file a path is, or -1.
    int MapIconOf(const char* path, size_t n)
    {
        for (int i = 0; i < kMapIconFileCount; ++i)
            if (EndsWith(path, n, kMapIconFiles[i])) return i;
        return -1;
    }

    bool Wants(const char* path)
    {
        int half;
        const size_t n = strlen(path);
        if (TableOf(path, half) >= 0 || EndsWith(path, n, kPalocLeaf) || _stricmp(path, kPortraitRegistry) == 0 ||
            _stricmp(path, kDescriptionPath) == 0 || _stricmp(path, kAppearancePath) == 0 ||
            _stricmp(path, kPoseModifierPath) == 0 || MapIconOf(path, n) >= 0 || SplicedChartOf(path) >= 0)
            return true;
        return g_ownCharts && (ChartOf(path) >= 0 || AttackOf(path) >= 0 || _stricmp(path, kPackageDesc) == 0 ||
                               (g_chartsReady && AliasLeaf(path) && EndsWith(path, n, ".paa_metabin")));
    }

    void Once(volatile LONG& flag, const char* fmt, const char* a, size_t b)
    {
        if (InterlockedExchange(&flag, 1) == 0) LOG(fmt, a, b);
    }

    bool ServeSpliced(int d, bool found, const std::string& game, std::string& out)
    {
        const bm::damiane::Chart& c = *kSpliced[d];
        const bool captive = d == kCaptiveChart;
        if (!found || !bm::damiane::Build(c, game, out))
        {
            if (captive)
                LOG_ERR("[captive] %s is not the file it was made for, so a bounty rides on Kliff's shoulder.", c.path);
            else
                LOG_ERR("[damiane] %s is not the file her nodes were made for, so she rides with Kliff's clips.", c.path);
            return false;
        }
        Once(g_loggedSpliced[d],
             captive ? "[captive] %s has the bounty's lying idle on Broomy (%zu bytes)."
                     : "[damiane] %s has Damiane's broom nodes (%zu bytes).",
             c.path, out.size());
        return true;
    }

    bool InteractionsServed();

    // Builds table `t` once; the caller holds its entry's lock. `game` is the
    // half the game is reading now (`half`), or null to read both through the
    // game's loader, as InteractionsServed does.
    void BuildEntry(int t, int half, bool found, const std::string* game)
    {
        Entry& e = g_entries[t];
        const char* name = kTables[t];
        e.tried = true;
        std::string part[2], report, why;
        const std::string* have[2] = { nullptr, nullptr };
        if (game) have[half] = game;
        if (!found)
        {
            LOG_ERR("[tables] %s: the game's own read of it failed, so it is left alone.", name);
            return;
        }
        for (int i = 0; i < 2; ++i)
        {
            if (have[i]) continue;
            const std::string path = std::string(kTableDir) + name + (i ? kBodyExt : kHeaderExt);
            if (!bm::gamefile::ReadFile(path.c_str(), part[i]))
            {
                LOG_ERR("[tables] %s: its other half could not be read through the game's loader, so the game keeps "
                        "its own table and Broomy's rows in it are missing.", name);
                return;
            }
            have[i] = &part[i];
        }
        const std::string& h = *have[0];
        const std::string& b = *have[1];
        CharacterFacts facts;
        const bool character = !strcmp(name, "characterinfo"), merc = !strcmp(name, "mercenaryinfo");
        // Broomy's character row names two rows of interactioninfo, which is
        // built and served on its own and may be read before or after this
        // table. So its outcome is settled here, whichever the game reads
        // first. The lock order is characterinfo's, then interactioninfo's;
        // building interactioninfo never asks for characterinfo's.
        bool interactions = true;
        if (character)
        {
            interactions = InteractionsServed();
            if (!interactions)
                LOG_ERR("[tables] characterinfo: interactioninfo is not Broomy's (it is the game's own or another "
                        "mod's, see above), so Broomy's row does not list Broom_Ride_Catch or Broom_Carry_Dead, and "
                        "mounting with a bounty on the shoulder and loading one onto Broomy are missing.");
        }
        if (BuildTable(name, h, b, e.ours[0], e.ours[1], report, why, character || merc ? &facts : nullptr, interactions))
        {
            e.ok = true;
            e.shipped[0] = { h.size(), bm::tablefile::Crc32(h.data(), h.size()) };
            e.shipped[1] = { b.size(), bm::tablefile::Crc32(b.data(), b.size()) };
            LOG("[tables] %s", report.c_str());
            if (character)
            {
                AcquireSRWLockExclusive(&g_factsLock);
                g_facts = std::move(facts);
                ReleaseSRWLockExclusive(&g_factsLock);
            }
            if (merc) InterlockedExchange(&g_broomyList, facts.broomyList);
        }
        else
            LOG_ERR("[tables] %s is the game's own: %s. Broomy will be missing whatever this table holds for it.", name,
                    why.c_str());
    }

    // Whether the game is given Broomy's interactioninfo: it is built from the
    // game's own bytes (read here if the game has not asked for them yet) and
    // came out. The game's reads then have to match those bytes, as ServeTable
    // checks, which they do unless the file changes between two reads.
    bool InteractionsServed()
    {
        int t = -1;
        for (int i = 0; i < kTableCount; ++i)
            if (!strcmp(kTables[i], "interactioninfo")) t = i;
        if (t < 0) return false;
        Entry& e = g_entries[t];
        AcquireSRWLockExclusive(&e.lock);
        if (!e.tried) BuildEntry(t, 0, true, nullptr);
        const bool ok = e.ok;
        ReleaseSRWLockExclusive(&e.lock);
        return ok;
    }

    bool ServeTable(int t, int half, bool found, const std::string& game, std::string& out)
    {
        Entry& e = g_entries[t];
        const char* name = kTables[t];
        AcquireSRWLockExclusive(&e.lock);
        if (!e.tried) BuildEntry(t, half, found, &game);
        bool serve = false;
        if (e.ok)
        {
            const Half& s = e.shipped[half];
            if (found && game.size() == s.size && bm::tablefile::Crc32(game.data(), game.size()) == s.crc)
            {
                out = e.ours[half];
                serve = true;
            }
            else
                LOG_ERR("[tables] %s %s: the game read different bytes from the ones Broomy's rows were built on, so "
                        "it keeps its own.", name, half ? "body" : "header");
        }
        ReleaseSRWLockExclusive(&e.lock);
        if (serve)
            Once(e.served[half], "[tables] %s: the game has Broomy's version (%zu bytes).",
                 (std::string(name) + (half ? kBodyExt : kHeaderExt)).c_str(), out.size());
        return serve;
    }

    volatile LONG g_loggedPaloc = 0, g_loggedPortraits = 0, g_loggedDescription = 0, g_loggedAppearance = 0,
                  g_loggedPoseModifiers = 0, g_loggedMapIcons[kMapIconFileCount] = {};

    bool Serve(const char* path, bool found, const std::string& game, std::string& out)
    {
        int half = 0;
        const int t = TableOf(path, half);
        if (t >= 0) return ServeTable(t, half, found, game, out);
        if (const int d = SplicedChartOf(path); d >= 0) return ServeSpliced(d, found, game, out);

        std::string why;
        const size_t n = strlen(path);
        if (EndsWith(path, n, kPalocLeaf))
        {
            if (!found) return false;
            if (!BuildPaloc(game, out, why))
            {
                LOG_ERR("[files] %s keeps its own names: %s.", path, why.c_str());
                return false;
            }
            Once(g_loggedPaloc, "[files] %s names Broomy (%zu bytes).", path, out.size());
            return true;
        }
        if (_stricmp(path, kPortraitRegistry) == 0)
        {
            if (!found) return false;
            if (!BuildPortraits(game, out, why))
            {
                LOG_ERR("[files] the portrait list is the game's own: %s.", why.c_str());
                return false;
            }
            Once(g_loggedPortraits, "[files] %s lists Broomy's portrait (%zu bytes).", path, out.size());
            return true;
        }
        if (const int m = MapIconOf(path, n); m >= 0)
        {
            if (!found) return false;
            if (!BuildMapIcons(m < 2, game, out, why))
            {
                LOG_ERR("[files] %s is the game's own, so Broomy shows the ibex's map icon: %s.", path, why.c_str());
                return false;
            }
            Once(g_loggedMapIcons[m], "[files] %s draws Broomy with the broom's map icon (%zu bytes).", path,
                 out.size());
            return true;
        }
        if (_stricmp(path, kPoseModifierPath) == 0)
        {
            if (!found) return false;
            if (!BuildPoseModifiers(game, out, why))
            {
                LOG_ERR("[files] %s is the game's own: %s.", path, why.c_str());
                return false;
            }
            Once(g_loggedPoseModifiers, "[files] %s gives the broom's skeleton aim IK (%zu bytes).", path, out.size());
            return true;
        }
        if (_stricmp(path, kDescriptionPath) == 0)
        {
            std::string wyvern;
            if (!bm::gamefile::ReadFile(kDescriptionSource, wyvern) || !BuildDescription(wyvern, out, why))
            {
                LOG_ERR("[files] the broom's gameplay data could not be made (%s), so %s is the game's own.",
                        why.empty() ? "the Wyvern's description did not read" : why.c_str(), path);
                return false;
            }
            Once(g_loggedDescription, "[files] %s is the broom's gameplay data (%zu bytes).", path, out.size());
            return true;
        }
        if (_stricmp(path, kAppearancePath) == 0)
        {
            out = AppearanceText();
            Once(g_loggedAppearance, "[files] %s read directly is the broom's appearance (%zu bytes).", path, out.size());
            return true;
        }
        return g_ownCharts && ServeChartFile(path, found, game, out);
    }

    const std::string* Appearance(const char* path)
    {
        return _stricmp(path, kAppearancePath) == 0 ? &AppearanceText() : nullptr;
    }
}

namespace bm::broomy
{
    bool Install(int chartMode)
    {
        g_chartMode = chartMode >= kChartsWyvern && chartMode <= kChartsPadded ? chartMode : kChartsPadded;
        const bool ok = bm::gamefile::Install({ &Wants, &Serve, &Appearance, &RedirectLoad, &ReadInstead, &Patch });
        g_ownCharts = ok && g_chartMode != kChartsWyvern &&
                      (g_chartMode != kChartsAliased || bm::gamefile::Redirecting());
        bm::broomrows::UseOwnCharts(g_ownCharts);
        static const char* const kNames[] = { "the Wyvern's", "GoldStar's files holding the Wyvern's charts unchanged",
                                              "the Wyvern's with aliases loaded as the broom's animations",
                                              "the Wyvern's with the broom's own paths written in" };
        LOG("[charts] Broomy runs %s.", kNames[g_ownCharts ? g_chartMode : 0]);
        return ok;
    }

    int Row()
    {
        AcquireSRWLockShared(&g_factsLock);
        const int row = g_facts.broomyRow;
        ReleaseSRWLockShared(&g_factsLock);
        return row;
    }

    int MercenaryKey(uint32_t row)
    {
        AcquireSRWLockShared(&g_factsLock);
        const int key = row < g_facts.mercInfo.size() ? g_facts.mercInfo[row] : -1;
        ReleaseSRWLockShared(&g_factsLock);
        return key;
    }

    int ListIndex() { return g_broomyList; }
}
