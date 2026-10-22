// Offline check of Broomy's builders (src/game/broomrows.cpp). Runs them on
// the game's own files and compares the result with files built another way,
// byte for byte.
//
//   buildcheck IN OUT
//
// IN holds the shipped NAME.staticinfoheader and NAME.staticinfobody of
// every table Broomy changes, character.paloc, mon_wyvern.xml and
// cd_image_portrait_00.xml; OUT the same tables as the reference build writes
// them, character.paloc.plain (the decompressed list), animal_seal.xml and
// cd_image_portrait_00.xml. `build.bat check IN OUT` builds and runs it.
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#include <Windows.h>

#include "game/broomrows.h"
#include "game/lz4.h"

namespace
{
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

    size_t FirstDifference(const std::string& a, const std::string& b)
    {
        size_t i = 0;
        while (i < a.size() && i < b.size() && a[i] == b[i]) ++i;
        return i;
    }
}

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        fprintf(stderr, "usage: buildcheck IN OUT\n");
        return 2;
    }
    const std::string in = std::string(argv[1]) + "/", out = std::string(argv[2]) + "/";
    int failed = 0;
    for (const char* name : bm::broomrows::kTables)
    {
        std::string h, b, wantH, wantB, gotH, gotB, report, why;
        if (!Load(in + name + ".staticinfoheader", h) || !Load(in + name + ".staticinfobody", b) ||
            !Load(out + name + ".staticinfoheader", wantH) || !Load(out + name + ".staticinfobody", wantB))
        {
            printf("%-30s files missing\n", name);
            ++failed;
            continue;
        }
        bm::broomrows::CharacterFacts facts;
        if (!bm::broomrows::BuildTable(name, h, b, gotH, gotB, report, why, &facts))
        {
            printf("%-30s FAILED: %s\n", name, why.c_str());
            ++failed;
            continue;
        }
        // BUILDCHECK_DUMP=DIR writes each built table there, to read a new row.
        char dump[512] = {};
        if (GetEnvironmentVariableA("BUILDCHECK_DUMP", dump, sizeof dump))
            for (const auto& [ext, data] : { std::pair<const char*, const std::string*>{ ".staticinfoheader", &gotH },
                                            { ".staticinfobody", &gotB } })
            {
                FILE* f = nullptr;
                if (fopen_s(&f, (std::string(dump) + "/" + name + ext).c_str(), "wb") == 0 && f)
                {
                    fwrite(data->data(), 1, data->size(), f);
                    fclose(f);
                }
            }
        const bool same = gotH == wantH && gotB == wantB;
        printf("%-30s %s  %s\n", name, same ? "identical" : "DIFFERENT", report.c_str());
        if (!same)
        {
            printf("    header %zu vs %zu bytes, first difference at %zu; body %zu vs %zu, first difference at %zu\n",
                   gotH.size(), wantH.size(), FirstDifference(gotH, wantH), gotB.size(), wantB.size(),
                   FirstDifference(gotB, wantB));
            ++failed;
        }
        if (facts.broomyRow >= 0)
        {
            int known = 0, horses = 0;
            for (int16_t m : facts.mercInfo)
            {
                known += m >= 0;
                horses += m == 78;   // Vehicle_Horse, as the file stores it (a key, not the index)
            }
            printf("    Broomy's row %d; %d of %zu rows name a mercenary list, %d of them the horses'; Broomy's reads %d\n",
                   facts.broomyRow, known, facts.mercInfo.size(), horses, facts.mercInfo[facts.broomyRow]);
        }
    }

    std::string paloc, wantPlain, got, why;
    if (Load(in + "character.paloc", paloc) && Load(out + "character.paloc.plain", wantPlain))
    {
        if (!bm::broomrows::BuildPaloc(paloc, got, why))
        {
            printf("%-30s FAILED: %s\n", "character.paloc", why.c_str());
            ++failed;
        }
        else
        {
            uint32_t packed = 0, plainSize = 0;
            memcpy(&packed, got.data() + 9, 4);
            memcpy(&plainSize, got.data() + 13, 4);
            std::string plain;
            const bool unpacked = packed == got.size() - 0x200 &&
                                  bm::lz4::Decompress(reinterpret_cast<const uint8_t*>(got.data()) + 0x200, packed,
                                                      plain, plainSize);
            const bool same = unpacked && plain == wantPlain;
            printf("%-30s %s  (%zu bytes, the list %u)\n", "character.paloc", same ? "identical" : "DIFFERENT",
                   got.size(), plainSize);
            if (!same) ++failed;
        }
    }
    else
    {
        printf("%-30s files missing\n", "character.paloc");
        ++failed;
    }
    struct Single
    {
        const char* in;
        const char* out;
        bool (*build)(const std::string&, std::string&, std::string&);
    };
    const Single singles[] = { { "mon_wyvern.xml", "animal_seal.xml", &bm::broomrows::BuildDescription },
                               { "cd_image_portrait_00.xml", "cd_image_portrait_00.xml", &bm::broomrows::BuildPortraits } };
    for (const Single& s : singles)
    {
        std::string game, want, mine;
        if (!Load(in + s.in, game) || !Load(out + s.out, want))
        {
            printf("%-30s files missing\n", s.out);
            ++failed;
            continue;
        }
        if (!s.build(game, mine, why))
        {
            printf("%-30s FAILED: %s\n", s.out, why.c_str());
            ++failed;
            continue;
        }
        const bool same = mine == want;
        printf("%-30s %s  (%zu bytes from %zu)\n", s.out, same ? "identical" : "DIFFERENT", mine.size(), game.size());
        if (!same) ++failed;
    }
    printf(failed ? "%d FAILED\n" : "all identical\n", failed);
    return failed ? 1 : 0;
}
