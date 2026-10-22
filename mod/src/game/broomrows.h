#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Broomy's own rows and files, built from the game's own bytes each time
// the game reads them, so a player's install is the plugin alone. Pure
// functions: no game memory, no hooks, so the offline check
// (mod/tests/buildcheck.cpp) runs them against the shipped files.
//
// Broomy is characterinfo 1900001, Riding_Broomy_1, with a mercenary list
// (87) and a vehicle row (20000) of its own. Nothing a shipped row uses
// changes except two lists: the Vehicle mercenary group gains Vehicle_Broom,
// and VehicleSlot, the saddle wedge on the Character radial, takes Broomy's
// list after the horses' and Vehicle_Special's, so Broomy is one more mount
// to scroll to there (Seth, 4 October: "move Broomy to the mount wedge as a
// new entry that doesnt reuse another entry").
namespace bm::broomrows
{
    inline constexpr uint32_t kBroomyKey = 1900001;
    inline constexpr const char* kDisplayName = "Broomy";

    inline constexpr const char* kTableDir = "gamedata/binarystaticinfo__/bin/";
    // Every table Broomy changes, in no particular order.
    inline constexpr const char* kTables[] = { "stringinfo", "characterinfo", "mercenaryinfo", "mercenarygroupinfo",
                                               "reserveslot", "vehicleinfo", "characterappearanceindexinfo",
                                               "interactioninfo", "uimaptextureinfo", "uifiltergroupinfo" };

    // The broom has no appearance file, and a file the packs do not hold
    // cannot be read, so Broomy's appearance names one that ships and that
    // no table names: the cut Phoenix's. The plugin hands the appearance
    // loader the broom's text whenever it loads this path.
    inline constexpr const char* kAppearancePath =
        "character/appearance/2_mon/cd_m0004_00_dragon/cd_m0004_00_phoenix/cd_m0004_00_phoenix_0001_00000.app_xml";
    // Its gameplay data the same way: a character description that ships
    // and that no table names, served as the Wyvern's without its foot IK
    // and climbing. The descriptions are read by listing their folder, so a
    // new file name would never be read at all.
    inline constexpr const char* kDescriptionPath = "character/descriptors/characterdescription/animal_seal.xml";
    inline constexpr const char* kDescriptionSource = "character/descriptors/characterdescription/mon_wyvern.xml";
    // The UI finds a portrait by name in this list, not by building a path.
    inline constexpr const char* kPortraitRegistry = "ui/xml/texture/cd_image_portrait_00.xml";
    // The map's markup and styles, by the end of their path: Broomy's map
    // icon is a component of each markup file and a class of each style
    // sheet. Markup first.
    inline constexpr const char* kMapIconFiles[] = { "/minimapicon.thtml", "/worldmapicon.thtml", "/minimapicon.css",
                                                     "/worldmapicon.css" };
    // Each language's character names; the game reads one.
    inline constexpr const char* kPalocLeaf = "/character.paloc";
    // Every skeleton's pose modifiers (aim IK, foot IK, vehicle tilt and so
    // on), keyed by skeleton file name. The game lists the folder and reads
    // this, its one file, through Read.
    inline constexpr const char* kPoseModifierPath =
        "character/descriptors/posemodifierdata/posemodifierdata.xml";

    // Whether Broomy's row names its own charts (GoldStar's, served as the
    // broom's) or the Wyvern's. Own unless the plugin cannot redirect the
    // broom's animations; set before the tables are built.
    void UseOwnCharts(bool own);
    // Whether Broomy's row lists Broom_Carry_Dead, loading a captive onto
    // Broomy, and its vehicle row has the cargo seat a load goes onto. On
    // unless a test build turns it off; set before the tables are built.
    void OfferLoad(bool on);

    // What the characterinfo build learns that the grant needs: Broomy's
    // row, and each row's _mercenaryInfo (-1 where the record does not say).
    // The mercenaryinfo build fills in Broomy's list index for the call gate.
    struct CharacterFacts
    {
        int broomyRow = -1;
        std::vector<int16_t> mercInfo;
        int broomyList = -1;
    };

    // A table's new header and body from the game's. `report` is one line
    // for the log. False, with `why`, when the game's rows are not the ones
    // this was written against; the game then keeps its own table.
    bool BuildTable(const char* name, const std::string& header, const std::string& body, std::string& outHeader,
                    std::string& outBody, std::string& report, std::string& why, CharacterFacts* facts = nullptr);

    // character.paloc with Broomy's name added.
    bool BuildPaloc(const std::string& game, std::string& out, std::string& why);
    // The portrait list with an entry for Broomy's portrait, which is the
    // broom item's icon.
    bool BuildPortraits(const std::string& game, std::string& out, std::string& why);
    // A map markup file with a component for Broomy (`markup`), or a map
    // style sheet with its classes, copies of the hired ibex's drawn with
    // the broom item's map icon.
    bool BuildMapIcons(bool markup, const std::string& game, std::string& out, std::string& why);
    // The pose modifier list with an aim IK for the broom's skeleton: its
    // body bone turns toward where the player aims, as the Wyvern's neck
    // does. The Wyvern's riding charts already switch aim IK on in flight.
    bool BuildPoseModifiers(const std::string& game, std::string& out, std::string& why);
    // The broom's description from the Wyvern's.
    bool BuildDescription(const std::string& wyvern, std::string& out, std::string& why);
    // The broom's appearance file: the Wyvern's, naming the broom's prefab.
    const std::string& AppearanceText();
}
