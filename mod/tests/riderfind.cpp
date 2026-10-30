// Offline check of riderfind.cpp, which riderfix searches Kliff's lower chart
// and the RideOn charts with. On the shipped ride_test3_lower.paac and
// m0004_ride_dragon_upper.paac the search must find every word
// private/research/newmount/riderfix_offsets.py found (riderwords.h), with the
// same shipped and new values. Then the same:
// - on the lower chart with a node array grown by a record in the middle, so
//   every record past it moves;
// - on the lower chart after riderfix's one-time writes (the push-off's own
//   name, the hand IK at 1000 s), as the second of Broomy and the Speeder
//   Bike to see the chart finds it;
// - on the lower chart with Damiane's nodes spliced in, where each word must
//   be where damiane.h's Moved puts it;
// - on Broomy's own RideOn chart, as broomchart.cpp builds it.
//
//   riderfind CHARTS SPLICED
//
// CHARTS holds the shipped charts; SPLICED is chartcheck's OUT, with
// damiane_ride_test3_lower.paac and padded_m0002_armadillo.paac.
// `build.bat riderfind CHARTS SPLICED` builds and runs it.
#include <cstdio>
#include <cstring>
#include <string>

#include "game/chartwalk.h"
#include "game/damiane.h"
#include "game/riderfind.h"
#include "riderwords.h"

namespace
{
    int g_failed = 0;

    void Expect(bool ok, const char* what)
    {
        printf("    %-4s %s\n", ok ? "ok" : "FAIL", what);
        if (!ok) ++g_failed;
    }

    bool Load(const std::string& path, std::string& out)
    {
        out.clear();
        FILE* f = nullptr;
        if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return false;
        char chunk[65536];
        size_t n;
        while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) out.append(chunk, n);
        fclose(f);
        return true;
    }

    const uint8_t* Bytes(const std::string& s) { return reinterpret_cast<const uint8_t*>(s.data()); }

    // Where a word of the shipped chart is in the chart under test.
    typedef uint32_t (*Place)(uint32_t);
    uint32_t g_shift = 0, g_from = 0;
    uint32_t Same(uint32_t at) { return at; }
    uint32_t Grown(uint32_t at) { return at + (at >= g_from ? g_shift : 0); }
    uint32_t Spliced(uint32_t at) { return bm::damiane::Moved(bm::damiane::kCharts[1], at); }

    // The chart with a node's play-rate array (16-byte records) grown by a
    // copy of its first record, in a node a third of the way in or later:
    // every byte after it moves on by 16.
    bool Grow(std::string& d, uint32_t& from)
    {
        bm::chartwalk::Chart c;
        const char* why = "";
        if (!bm::chartwalk::Walk(Bytes(d), static_cast<uint32_t>(d.size()), c, why)) return false;
        for (uint32_t n = static_cast<uint32_t>(c.nodes.size() / 3); n < c.nodes.size(); ++n)
        {
            const uint32_t h = c.nodes[n], rate = c.NodeArray(n, 1);
            const uint8_t count = static_cast<uint8_t>(d[h + 0x76]);
            if (!rate || count == 255) continue;
            d.insert(rate, d.substr(rate, 16));
            d[h + 0x76] = static_cast<char>(count + 1);
            for (int i = 2; i < 11; ++i)   // the later arrays' offsets
            {
                uint16_t off;
                memcpy(&off, &d[h + 0x70 + 4 * i], 2);
                off = static_cast<uint16_t>(off + 16);
                memcpy(&d[h + 0x70 + 4 * i], &off, 2);
            }
            from = rate;
            return true;
        }
        return false;
    }

    void CheckLower(const char* label, const std::string& lower, Place place)
    {
        printf("%s\n", label);
        const char* why = "";
        bm::riderfind::Lower lo{};
        const bool l = bm::riderfind::FindLower(Bytes(lower), static_cast<uint32_t>(lower.size()), lo, why);
        if (!l) printf("         %s\n", why);
        bool swaps = l;
        for (int i = 0; swaps && i < bm::riderfind::kLowerSwaps; ++i)
        {
            const riderwords::Row& w = riderwords::kLower[i];
            const bm::riderfind::Swap& g = lo.swaps[i];
            swaps = g.offset == place(w.offset) && g.shipped == w.shipped && g.now == w.now;
            if (!swaps)
                printf("         row %d: +0x%X %08X -> %08X, want +0x%X %08X -> %08X\n", i, g.offset, g.shipped, g.now,
                       place(w.offset), w.shipped, w.now);
        }
        Expect(swaps, "the swaps");
        bool ik = l && lo.ikCount == static_cast<int>(sizeof riderwords::kIk / sizeof riderwords::kIk[0]);
        for (int i = 0; ik && i < lo.ikCount; ++i) ik = lo.ik[i] == place(riderwords::kIk[i]);
        Expect(ik, "01B8F8D3's hand IK events");
        Expect(l && lo.pushOff == place(riderwords::kPushOff), "the push-off path");
        if (l)
            printf("         branches %u, %u; slot %u -> %u; slot %u -> %u\n", lo.ground, lo.air, lo.takeoffSlot,
                   lo.loop, lo.loopSlot, lo.exit);
    }

    void CheckIdle(const char* label, const std::string& rideOn)
    {
        const char* why = "";
        uint32_t at[bm::riderfind::kIdleMax];
        int n = 0;
        const bool r = bm::riderfind::FindRideOnIdle(Bytes(rideOn), static_cast<uint32_t>(rideOn.size()), at, n, why);
        if (!r) printf("         %s\n", why);
        bool same = r && n == static_cast<int>(sizeof riderwords::kRideOnIdle / sizeof riderwords::kRideOnIdle[0]);
        for (int i = 0; same && i < n; ++i) same = at[i] == riderwords::kRideOnIdle[i];
        Expect(same, label);
    }

    // The lower chart after riderfix's one-time writes, done the way
    // riderfix does them.
    std::string Written(const std::string& lower)
    {
        constexpr char kOld[] = "1_pc/1_phm/00_riding/cd_phm_rd_wyvern_basic_00_00_air_move_fall_walk_end_00.paa";
        constexpr char kNew[] = "1_pc/1_phm/00_riding/cd_phm_rd_broom_basic_00_00_nor_std_takeoff_00.paa";
        constexpr uint32_t kNever = 0x447A0000;
        std::string d = lower;
        memset(&d[riderwords::kPushOff], 0, sizeof kOld);
        memcpy(&d[riderwords::kPushOff], kNew, sizeof kNew - 1);
        for (uint32_t at : riderwords::kIk) memcpy(&d[at], &kNever, 4);
        return d;
    }
}

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        fprintf(stderr, "usage: riderfind CHARTS SPLICED\n");
        return 2;
    }
    const std::string in = std::string(argv[1]) + "/", sp = std::string(argv[2]) + "/";
    std::string lower, rideOn, spliced, served;
    if (!Load(in + "ride_test3_lower.paac", lower) || !Load(in + "m0004_ride_dragon_upper.paac", rideOn))
    {
        fprintf(stderr, "ride_test3_lower.paac or m0004_ride_dragon_upper.paac missing in %s\n", argv[1]);
        return 2;
    }
    if (!Load(sp + "damiane_ride_test3_lower.paac", spliced) || !Load(sp + "padded_m0002_armadillo.paac", served))
    {
        fprintf(stderr, "damiane_ride_test3_lower.paac or padded_m0002_armadillo.paac missing in %s\n", argv[2]);
        return 2;
    }

    CheckLower("shipped lower chart", lower, Same);
    printf("shipped RideOn chart\n");
    CheckIdle("the ground idle's branches", rideOn);

    std::string grown = lower;
    const bool g = Grow(grown, g_from);
    g_shift = 16;
    printf("lower chart with a node array grown by 16 bytes\n");
    Expect(g, "a node array grows");
    if (g) CheckLower("    (the search on it)", grown, Grown);

    CheckLower("lower chart with riderfix's one-time writes in", Written(lower), Same);

    Expect(spliced.size() == bm::damiane::kCharts[1].builtSize, "the spliced lower chart is the size damiane.h builds");
    CheckLower("lower chart with Damiane's nodes (where Moved puts each word)", spliced, Spliced);

    printf("Broomy's own RideOn chart, m0002_armadillo.paac as served\n");
    CheckIdle("the ground idle's branches (the same offsets: Broomy's edits keep the size)", served);

    std::string cut = lower.substr(0, lower.size() - 1);
    bm::riderfind::Lower lo{};
    const char* why = "";
    Expect(!bm::riderfind::FindLower(Bytes(cut), static_cast<uint32_t>(cut.size()), lo, why),
           "a chart one byte short is refused");
    printf(g_failed ? "%d FAILED\n" : "all ok\n", g_failed);
    return g_failed ? 1 : 0;
}
