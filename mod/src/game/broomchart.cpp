#include "game/broomchart.h"
#include "game/boostpatches.h"
#include "game/crossfadepatches.h"
#include "game/rightmountpatches.h"
#include "game/escortmountpatches.h"
#include "game/groundpatches.h"
#include "game/takeoffpatches.h"
#include "game/effectpatches.h"
#include "game/flourishpatches.h"
#include "game/ikpatches.h"
#include "game/mountpatches.h"
#include "game/quietpatches.h"
#include "game/rideonblendpatches.h"
#include "game/speedpatches.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

// A port of the first build's scripts/broomchart.py (private/archive), less
// its pack files: what that script wrote as new files, the plugin now loads
// from the broom's own files under the alias names.

namespace bm::broomchart
{
    namespace
    {
        constexpr const char* kBroomMotion = "4_riding/cd_r0032_00_broom/cd_rd_broom_basic_00_00_";
        constexpr const char* kBroomBlend = "character/binary/motionblending/broom_riding_move.motionblending";
        constexpr const char* kMotionDir = "character/motion/";
        constexpr const char* kLodDir = "character/motion/motion_lod__/";
        constexpr const char* kMetaDir = "actionchart/bin__/animmeta/";
        constexpr const char* kAnimTag = "bm";
        constexpr const char* kBlendTag = "bb";

        // Camera shake the charts play alongside the mount; not the mount's.
        constexpr const char* kKeep = "9_cutscene/";

        // Which broom animation stands in for which Wyvern one, by words in
        // its path, first match wins; idle for the rest.
        struct Rule
        {
            const char* words[6];
            const char* broom;
        };
        constexpr Rule kRules[] = {
            { { "dismount_l" }, "nor_std_dismount_l_00" },
            { { "dismount_r" }, "nor_std_dismount_r_00" },
            { { "mount_l" }, "nor_std_mount_l_00" },
            { { "mount_r", "mount_up", "std_mount" }, "nor_std_mount_r_00" },
            // The takeoff lifts off level. The 75u climb tipped the broom 60 degrees
            // under an upright Kliff, and it swung back level as flight began.
            // nor_std_takeoff_00 is a clip of the plugin's own (broomclips.h): every
            // bone held on the idle's first frame, with the climb's rise on Bip01.
            { { "takeoff", "flystart", "landtohover" }, "nor_std_takeoff_00" },
            { { "75u", "45u", "90u" }, "nor_move_walkfast_f_75u_ing_00" },
            { { "75d", "45d", "90d", "dive" }, "nor_move_walkfast_f_75d_ing_00" },
            { { "_land" }, "nor_std_idle_01" },
            { { "nor_move_run_f" }, "nor_move_run_f_ing_00" },
            { { "run_f" }, "nor_move_runfast_f_ing_00" },
            { { "walkfast" }, "nor_move_walkfast_f_ing_00" },
            { { "walk_f", "glide_walk", "fly_b_", "avoid", "flystart", "jump" }, "nor_move_walk_f_ing_00" },
        };
        constexpr const char* kIdle = "nor_std_idle_01";
        const char* g_idle = kIdle;
        bool g_hoverAlways = false;
        bm::broomchart::Speeds g_speeds;

        // TempPivotKey_Wyvern_Ride_2 and TempPivotKey_Broom_Ride_2, as the
        // charts store them.
        constexpr char kWyvernPivot2[4] = { '\x95', '\x25', '\x85', '\xE7' };   // E7852595
        constexpr char kBroomPivot2[4] = { '\x4B', '\x68', '\x72', '\xA2' };    // A272684B

        // One-shot moves: takeoff, landing, brakes, turns, dodges, drops.
        // Their metadata carries the timing and events the charts wait on;
        // with the broom's there, the first build's broom took off only
        // after X was pressed again and again and would not stay landed. So
        // these, and every animation only the upper charts name, keep the
        // Wyvern's metadata.
        constexpr const char* kTransitions[] = { "takeoff", "land", "flystart", "hovertofly", "break", "turn", "drift",
                                                 "avoid", "roll", "surprise", "drop", "mount", "fall", "pushwall" };

        const char* BroomFor(const std::string& path)
        {
            for (const Rule& r : kRules)
                for (const char* w : r.words)
                    if (w && path.find(w) != std::string::npos) return strcmp(r.broom, kIdle) == 0 ? g_idle : r.broom;
            return g_idle;
        }

        bool Transition(const std::string& path)
        {
            for (const char* w : kTransitions)
                if (path.find(w) != std::string::npos) return true;
            return false;
        }

        std::string Lower(std::string s)
        {
            for (char& c : s)
            {
                if (c == '\\') c = '/';
                else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            }
            return s;
        }

        bool Word(char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
        bool Printable(char c) { return c >= 0x20 && c <= 0x7E; }

        struct Found
        {
            size_t at;
            std::string text;
        };

        // Every length-prefixed path in a chart that ends in `suffix`: a run
        // of word characters and a slash, printable text, the suffix, a zero,
        // with the byte before it counting the text and the zero.
        std::vector<Found> Paths(const std::string& d, const char* suffix)
        {
            std::vector<Found> out;
            const std::string needle = std::string(suffix) + '\0';
            const size_t tail = strlen(suffix);
            size_t from = 0;
            for (size_t e = d.find(needle); e != std::string::npos; e = d.find(needle, from))
            {
                const size_t end = e + tail;   // the zero
                from = end + 1;
                size_t left = e;
                while (left > 0 && Printable(d[left - 1])) --left;
                for (size_t s = left; s < e; ++s)
                {
                    if (s == 0 || static_cast<uint8_t>(d[s - 1]) != end - s + 1) continue;
                    size_t w = s;
                    while (w < e && Word(d[w])) ++w;
                    if (w == s || w >= e || d[w] != '/') continue;
                    out.push_back({ s, d.substr(s, end - s) });
                    break;
                }
            }
            return out;
        }

        // A new name for `path`, the same length, in the same folder.
        bool AliasLeaf(const std::string& path, const char* tag, size_t n, std::string& leaf, std::string& why)
        {
            const size_t slash = path.find_last_of('/');
            const size_t dot = path.find_last_of('.');
            if (slash == std::string::npos || dot == std::string::npos || dot < slash)
            {
                why = "a chart path has no folder or no extension: " + path;
                return false;
            }
            const size_t stem = dot - slash - 1;
            char head[16];
            snprintf(head, sizeof head, "%s%03zu_", tag, n);
            const size_t h = strlen(head);
            if (n > 999 || stem < h + 1)
            {
                why = "a chart path is too short to alias: " + path;
                return false;
            }
            leaf = head + std::string(stem - h, 'x');
            return true;
        }

        // `path` with its file name's stem replaced, keeping its own case and
        // separators up to there.
        void SwapStem(std::string& data, const Found& f, const std::string& leaf)
        {
            const size_t cut = f.text.find_last_of("/\\");
            const size_t dot = f.text.find_last_of('.');
            data.replace(f.at + cut + 1, dot - cut - 1, leaf);
        }

        template <typename T> T Peek(const std::string& s, size_t at)
        {
            T v{};
            memcpy(&v, s.data() + at, sizeof v);
            return v;
        }
        template <typename T> void Poke(std::string& s, size_t at, T v) { memcpy(&s[at], &v, sizeof v); }

        // A leaf "<tag><3 digits>_...": the number, or -1.
        int TagNumber(const std::string& leaf, const char* tag)
        {
            if (leaf.size() < 6 || leaf.compare(0, 2, tag) != 0 || leaf[5] != '_') return -1;
            int n = 0;
            for (int i = 2; i < 5; ++i)
            {
                if (leaf[i] < '0' || leaf[i] > '9') return -1;
                n = n * 10 + (leaf[i] - '0');
            }
            return n;
        }

        bool EndsWith(const std::string& s, const char* tail, std::string* rest = nullptr)
        {
            const size_t t = strlen(tail);
            if (s.size() < t || s.compare(s.size() - t, t, tail) != 0) return false;
            if (rest) *rest = s.substr(0, s.size() - t);
            return true;
        }
    }

    void SetIdleLeaf(const char* leaf) { g_idle = leaf; }
    void SetHoverAlways(bool on) { g_hoverAlways = on; }
    void SetSpeeds(const Speeds& speeds) { g_speeds = speeds; }

    // The broom's own path over a chart path: its text, then zeros to the
    // old length, the length byte and the terminating zero left as they are.
    void Overwrite(std::string& data, const Found& f, const std::string& path)
    {
        data.replace(f.at, f.text.size(), path + std::string(f.text.size() - path.size(), '\0'));
    }

    bool BuildPaddedCharts(const std::string (&wyvern)[kChartCount], std::string (&out)[kChartCount],
                           std::string& report, std::string& why)
    {
        std::set<std::string> anims, blends;
        for (int c = 0; c < kChartCount; ++c)
        {
            out[c] = wyvern[c];
            for (const Found& f : Paths(wyvern[c], ".paa"))
            {
                const std::string key = Lower(f.text);
                if (key.find(kKeep) != std::string::npos) continue;
                const std::string broom = std::string(kBroomMotion) + BroomFor(key) + ".paa";
                if (broom.size() > f.text.size())
                {
                    why = "the broom's path is longer than " + key;
                    return false;
                }
                Overwrite(out[c], f, broom);
                anims.insert(key);
            }
            for (const Found& f : Paths(wyvern[c], ".motionblending"))
            {
                if (strlen(kBroomBlend) > f.text.size())
                {
                    why = "the broom's blend path is longer than " + f.text;
                    return false;
                }
                Overwrite(out[c], f, kBroomBlend);
                blends.insert(Lower(f.text));
            }
            if (out[c].size() != wyvern[c].size())
            {
                why = "a chart changed size";
                return false;
            }
        }
        if (anims.empty() || blends.empty())
        {
            why = "the Wyvern's charts name no animations or no blends where this looks for them";
            return false;
        }
        // An interaction's pivot names an action by key, looked up in the
        // rider's chart and in the mount's. The Wyvern's riding chart has an
        // action keyed by Wyvern_Ride's second pivot; Broomy's, keyed by
        // Broom_Ride's second pivot, is the same action.
        size_t keyed = 0;
        for (size_t at = out[2].find(std::string(kWyvernPivot2, 4)); at != std::string::npos;
             at = out[2].find(std::string(kWyvernPivot2, 4), at + 4), ++keyed)
            out[2].replace(at, 4, std::string(kBroomPivot2, 4));
        // Research, off: branches into the Wyvern's ground states go to its
        // flight state instead (hoverpatches.h). Seth, 2 October: Broomy then
        // took off as soon as it moved; it should move about on the ground
        // and take off only on jump, which is the Wyvern's own chart.
        size_t hover = 0, skipped = 0;
        for (const HoverPatch& p : kHoverPatches)
        {
            if (!g_hoverAlways) break;
            std::string& c = out[p.chart];
            if (p.at + 4 > c.size() || Peek<uint32_t>(c, p.at) != p.wyvern)
            {
                ++skipped;
                continue;
            }
            Poke<uint32_t>(c, p.at, p.broomy);
            ++hover;
        }
        // None of the Wyvern's noises: its own sound events get no sound
        // (quietpatches.h).
        size_t quiet = 0;
        for (const HoverPatch& p : kQuietPatches)
        {
            std::string& c = out[p.chart];
            if (p.at + 4 > c.size() || Peek<uint32_t>(c, p.at) != p.wyvern) continue;
            Poke<uint32_t>(c, p.at, p.broomy);
            ++quiet;
        }
        // The ini's speeds (speedpatches.h): each Wyvern speed times the
        // percentage for its kind.
        size_t speed = 0;
        const float ground = g_speeds.ground / 100.0f, flight = g_speeds.flight / 100.0f;
        const float scale[] = { ground, flight, flight * (g_speeds.boost / 100.0f), g_speeds.climb / 100.0f, 0.0f };
        for (const SpeedPatch& p : kSpeedPatches)
        {
            std::string& c = out[p.chart];
            if (p.at + 4 > c.size() || Peek<uint32_t>(c, p.at) != p.wyvern) continue;
            float v;
            memcpy(&v, &p.wyvern, 4);
            v *= scale[p.kind];
            uint32_t u;
            memcpy(&u, &v, 4);
            Poke<uint32_t>(c, p.at, u);
            ++speed;
        }
        // The run key boosts in the air in Camera flight control too
        // (boostpatches.h); the Wyvern boosts only in Manual.
        size_t boost = 0;
        for (const HoverPatch& p : kBoostPatches)
        {
            std::string& c = out[p.chart];
            if (p.at + 4 > c.size() || Peek<uint32_t>(c, p.at) != p.wyvern) continue;
            Poke<uint32_t>(c, p.at, p.broomy);
            ++boost;
        }
        // No rolls and no glides (flourishpatches.h).
        size_t flourish = 0;
        for (const HoverPatch& p : kFlourishPatches)
        {
            std::string& c = out[p.chart];
            if (p.at + 4 > c.size() || Peek<uint32_t>(c, p.at) != p.wyvern) continue;
            Poke<uint32_t>(c, p.at, p.broomy);
            ++flourish;
        }
        // No footfalls: the Wyvern's camera shakes, rumble, blur and effects
        // do nothing (effectpatches.h).
        size_t effect = 0;
        for (const HoverPatch& p : kEffectPatches)
        {
            std::string& c = out[p.chart];
            if (p.at + 4 > c.size() || Peek<uint32_t>(c, p.at) != p.wyvern) continue;
            Poke<uint32_t>(c, p.at, p.broomy);
            ++effect;
        }
        // The RideOn chart's flight states play Broomy's blend as the lower
        // chart's do, not a fixed level clip (rideonblendpatches.h).
        size_t rideOnBlend = 0;
        for (const HoverPatch& p : kRideOnBlendPatches)
        {
            std::string& c = out[p.chart];
            if (p.at + 4 > c.size() || Peek<uint32_t>(c, p.at) != p.wyvern) continue;
            Poke<uint32_t>(c, p.at, p.broomy);
            ++rideOnBlend;
        }
        // Mounting finishes by itself, without X (mountpatches.h).
        size_t mount = 0;
        for (const HoverPatch& p : kMountPatches)
        {
            std::string& c = out[p.chart];
            if (p.at + 4 > c.size() || Peek<uint32_t>(c, p.at) != p.wyvern) continue;
            Poke<uint32_t>(c, p.at, p.broomy);
            ++mount;
        }
        // The aim IK aims from B_Body_00, not the Wyvern's head (ikpatches.h).
        size_t ik = 0;
        for (const HoverPatch& p : kIkPatches)
        {
            std::string& c = out[p.chart];
            if (p.at + 4 > c.size() || Peek<uint32_t>(c, p.at) != p.wyvern) continue;
            Poke<uint32_t>(c, p.at, p.broomy);
            ++ik;
        }
        // Letting go of the stick eases into the hover (crossfadepatches.h).
        size_t crossfade = 0;
        for (const HoverPatch& p : kCrossfadePatches)
        {
            std::string& c = out[p.chart];
            if (p.at + 4 > c.size() || Peek<uint32_t>(c, p.at) != p.wyvern) continue;
            Poke<uint32_t>(c, p.at, p.broomy);
            ++crossfade;
        }
        // Kliff mounts from the right as well (rightmountpatches.h).
        size_t right = 0;
        for (const HoverPatch& p : kRightMountPatches)
        {
            std::string& c = out[p.chart];
            if (p.at + 4 > c.size() || Peek<uint32_t>(c, p.at) != p.wyvern) continue;
            Poke<uint32_t>(c, p.at, p.broomy);
            ++right;
        }
        // Broomy takes a rider mounting with a captive (escortmountpatches.h).
        size_t escort = 0;
        for (const HoverPatch& p : kEscortMountPatches)
        {
            std::string& c = out[p.chart];
            if (p.at + 4 > c.size() || Peek<uint32_t>(c, p.at) != p.wyvern) continue;
            Poke<uint32_t>(c, p.at, p.broomy);
            ++escort;
        }
        // A Broomy that arrives above the ground starts in the ground idle,
        // not the Wyvern's fly idle (groundpatches.h).
        size_t groundStart = 0;
        for (const HoverPatch& p : kGroundPatches)
        {
            std::string& c = out[p.chart];
            if (p.at + 4 > c.size() || Peek<uint32_t>(c, p.at) != p.wyvern) continue;
            Poke<uint32_t>(c, p.at, p.broomy);
            ++groundStart;
        }
        // The takeoff is not airborne, so the aim IK waits for flight
        // (takeoffpatches.h).
        size_t takeoff = 0;
        for (const HoverPatch& p : kTakeoffPatches)
        {
            std::string& c = out[p.chart];
            if (p.at + 4 > c.size() || Peek<uint32_t>(c, p.at) != p.wyvern) continue;
            Poke<uint32_t>(c, p.at, p.broomy);
            ++takeoff;
        }
        char line[1100];
        snprintf(line, sizeof line,
                 "%zu animation paths and %zu blend paths are the broom's, padded with zeros; %zu riding action%s keyed "
                 "to Broom_Ride; %zu of %zu ground branches go to flight%s; %zu of %zu Wyvern sound events silenced; %zu of %zu boost edits; %zu of %zu speeds set (ground %d%%, flight %d%%, boost %d%%, climb %d%%); %zu of %zu roll and glide branches cut; %zu of %zu shakes, rumbles, effects and voices removed; %zu of %zu RideOn flight nodes on the blend; %zu of %zu mount edits; %zu of %zu aim IK edits; %zu of %zu crossfades eased; %zu of %zu right-side mount edits; %zu of %zu escort mount edits; %zu of %zu ground start edits; %zu of %zu takeoff edits",
                 anims.size(), blends.size(), keyed, keyed == 1 ? "" : "s", hover, hover + skipped,
                 skipped ? " (the rest did not hold the Wyvern's value)" : "", quiet,
                 sizeof kQuietPatches / sizeof kQuietPatches[0], boost, sizeof kBoostPatches / sizeof kBoostPatches[0], speed,
                 sizeof kSpeedPatches / sizeof kSpeedPatches[0], g_speeds.ground, g_speeds.flight, g_speeds.boost,
                 g_speeds.climb, flourish,
                 sizeof kFlourishPatches / sizeof kFlourishPatches[0], effect,
                 sizeof kEffectPatches / sizeof kEffectPatches[0], rideOnBlend,
                 sizeof kRideOnBlendPatches / sizeof kRideOnBlendPatches[0], mount,
                 sizeof kMountPatches / sizeof kMountPatches[0], ik, sizeof kIkPatches / sizeof kIkPatches[0],
                 crossfade, sizeof kCrossfadePatches / sizeof kCrossfadePatches[0], right,
                 sizeof kRightMountPatches / sizeof kRightMountPatches[0], escort,
                 sizeof kEscortMountPatches / sizeof kEscortMountPatches[0], groundStart,
                 sizeof kGroundPatches / sizeof kGroundPatches[0], takeoff,
                 sizeof kTakeoffPatches / sizeof kTakeoffPatches[0]);
        report = line;
        return true;
    }

    bool BuildCharts(const std::string (&wyvern)[kChartCount], std::string (&out)[kChartCount], Aliases& aliases,
                     std::string& report, std::string& why, bool padded)
    {
        aliases = Aliases{};
        if (padded) return BuildPaddedCharts(wyvern, out, report, why);
        std::map<std::string, size_t> animOf, blendOf;
        std::set<std::string> inLower, inUpper;
        for (int c = 0; c < kChartCount; ++c)
        {
            out[c] = wyvern[c];
            const bool lower = strstr(kCharts[c].path, "_lower") != nullptr;
            for (const Found& f : Paths(wyvern[c], ".paa"))
            {
                const std::string key = Lower(f.text);
                if (key.find(kKeep) != std::string::npos) continue;
                (lower ? inLower : inUpper).insert(key);
                auto it = animOf.find(key);
                if (it == animOf.end())
                {
                    Alias a;
                    if (!AliasLeaf(key, kAnimTag, aliases.anims.size(), a.leaf, why)) return false;
                    a.wyvern = key;
                    a.broom = BroomFor(key);
                    it = animOf.emplace(key, aliases.anims.size()).first;
                    aliases.anims.push_back(a);
                }
                SwapStem(out[c], f, aliases.anims[it->second].leaf);
            }
        }
        size_t wyvernMeta = 0;
        for (Alias& a : aliases.anims)
        {
            a.wyvernMeta = (inUpper.count(a.wyvern) && !inLower.count(a.wyvern)) || Transition(a.wyvern);
            wyvernMeta += a.wyvernMeta;
        }
        for (int c = 0; c < kChartCount; ++c)
            for (const Found& f : Paths(wyvern[c], ".motionblending"))
            {
                const std::string key = Lower(f.text);
                auto it = blendOf.find(key);
                if (it == blendOf.end())
                {
                    std::string leaf;
                    if (!AliasLeaf(key, kBlendTag, aliases.blends.size(), leaf, why)) return false;
                    it = blendOf.emplace(key, aliases.blends.size()).first;
                    aliases.blends.push_back(leaf);
                }
                SwapStem(out[c], f, aliases.blends[it->second]);
            }
        for (int c = 0; c < kChartCount; ++c)
            if (out[c].size() != wyvern[c].size())
            {
                why = "a chart changed size, which the aliases never should";
                return false;
            }
        if (aliases.anims.empty() || aliases.blends.empty())
        {
            why = "the Wyvern's charts name no animations or no blends where this looks for them";
            return false;
        }
        char line[200];
        snprintf(line, sizeof line,
                 "%zu animations point at the broom's (%zu keep the Wyvern's timing) and %zu blends at "
                 "broom_riding_move",
                 aliases.anims.size(), wyvernMeta, aliases.blends.size());
        report = line;
        return true;
    }

    // characteractionpackagedescription.paacdesc: a u16 string count and that
    // many strings (a length byte counting the zero, the text, the zero),
    // then u32 package count, u32 sub-package count, the packages as five
    // u16 (sub-package count, first sub-package, path, slot, name; the last
    // three index the strings), and the sub-packages as two u16 (path,
    // slot). The Wyvern's CD_M0004_Dragon has one sub-package, its riding
    // chart in the RideOnDragon slot; GoldStar's upper group gets one of its
    // own in the same slot, a new sub-package at the end naming the
    // armadillo's chart path, which the list already holds.
    bool BuildPackageDesc(const std::string& game, std::string& out, std::string& report, std::string& why)
    {
        constexpr const char* kWyvern = "CD_M0004_Dragon";
        constexpr const char* kGoldStar = "CD_M0004_Ride_GoldStar_Upper";
        constexpr const char* kRideOn = "upperaction/2_mon/cd_m0002_fourfeet/m0002_armadillo";
        if (game.size() < 2)
        {
            why = "the package list is empty";
            return false;
        }
        const uint16_t count = Peek<uint16_t>(game, 0);
        std::vector<std::string> strs;
        size_t at = 2;
        for (uint16_t i = 0; i < count; ++i)
        {
            if (at >= game.size()) break;
            const uint8_t n = static_cast<uint8_t>(game[at]);
            if (n == 0 || at + n >= game.size() || game[at + n] != 0)
            {
                why = "a string in the package list is not where its layout says";
                return false;
            }
            strs.push_back(game.substr(at + 1, n - 1));
            at += n + 1;
        }
        if (strs.size() != count || at + 8 > game.size())
        {
            why = "the package list ends inside its strings";
            return false;
        }
        const uint32_t packages = Peek<uint32_t>(game, at), subs = Peek<uint32_t>(game, at + 4);
        const size_t base = at + 8;
        if (game.size() != base + 10ull * packages + 4ull * subs)
        {
            why = "the package list is not the size its counts say";
            return false;
        }
        size_t wyvern = 0, gold = 0;
        int foundW = 0, foundG = 0;
        for (uint32_t r = 0; r < packages; ++r)
        {
            const size_t rec = base + 10ull * r;
            const uint16_t name = Peek<uint16_t>(game, rec + 8);
            if (name >= strs.size()) continue;
            if (strs[name] == kWyvern) wyvern = rec, ++foundW;
            if (strs[name] == kGoldStar) gold = rec, ++foundG;
        }
        if (foundW != 1 || foundG != 1)
        {
            why = "the package list does not have the Wyvern's and GoldStar's upper groups once each";
            return false;
        }
        if (Peek<uint16_t>(game, wyvern) != 1 || Peek<uint16_t>(game, gold) != 0)
        {
            why = "the Wyvern's upper group has other than one sub-package, or GoldStar's already has one";
            return false;
        }
        int rideOn = -1, seen = 0;
        for (size_t i = 0; i < strs.size(); ++i)
            if (strs[i] == kRideOn) rideOn = static_cast<int>(i), ++seen;
        if (seen != 1)
        {
            why = "the package list does not name the armadillo's chart exactly once";
            return false;
        }
        const uint16_t firstSub = Peek<uint16_t>(game, wyvern + 2);
        if (firstSub >= subs)
        {
            why = "the Wyvern's sub-package is past the end of the list";
            return false;
        }
        const uint16_t slot = Peek<uint16_t>(game, base + 10ull * packages + 4ull * firstSub + 2);
        out = game;
        Poke<uint32_t>(out, at + 4, subs + 1);
        Poke<uint16_t>(out, gold, 1);
        Poke<uint16_t>(out, gold + 2, static_cast<uint16_t>(subs));
        out += std::string(reinterpret_cast<const char*>(&rideOn), 2);
        out += std::string(reinterpret_cast<const char*>(&slot), 2);
        char line[200];
        snprintf(line, sizeof line, "%s has sub-package %u, %s %s", kGoldStar, subs,
                 slot < strs.size() ? strs[slot].c_str() : "?", kRideOn);
        report = line;
        return true;
    }

    bool Redirect(const Aliases& aliases, const char* path, std::string& to, std::string* fallback)
    {
        const std::string p = Lower(path);
        const size_t slash = p.find_last_of('/');
        const std::string leaf = slash == std::string::npos ? p : p.substr(slash + 1);
        std::string stem;
        const int anim = TagNumber(leaf, kAnimTag);
        if (anim >= 0 && static_cast<size_t>(anim) < aliases.anims.size())
        {
            const Alias& a = aliases.anims[anim];
            const std::string broom = std::string(kBroomMotion) + a.broom;
            if (EndsWith(leaf, ".paa_metabin", &stem) && stem == a.leaf)
            {
                const std::string own = kMetaDir + broom + ".paa_metabin";
                const std::string wyv = kMetaDir + a.wyvern + "_metabin";
                to = a.wyvernMeta ? wyv : own;
                if (fallback) *fallback = a.wyvernMeta ? own : std::string();
                return true;
            }
            if (EndsWith(leaf, "_lod.paa", &stem) && stem == a.leaf && p.find("motion_lod__/") != std::string::npos)
            {
                to = kLodDir + broom + "_lod.paa";
                return true;
            }
            if (EndsWith(leaf, ".paa", &stem) && stem == a.leaf)
            {
                to = kMotionDir + broom + ".paa";
                return true;
            }
            return false;
        }
        const int blend = TagNumber(leaf, kBlendTag);
        if (blend >= 0 && static_cast<size_t>(blend) < aliases.blends.size() &&
            EndsWith(leaf, ".motionblending", &stem) && stem == aliases.blends[blend])
        {
            to = kBroomBlend;
            return true;
        }
        return false;
    }
}
