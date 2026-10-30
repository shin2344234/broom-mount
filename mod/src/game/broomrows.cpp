#include "game/broomrows.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "game/hash.h"
#include "game/lz4.h"
#include "game/tablefile.h"

// Each new row starts as a copy of a shipped one and changes only what makes
// it Broomy's. On build 3392 (Steam 26308950, the DLC build) a record opens
// with its name and then its key, and the fields follow in the order the
// exe's table loaders read them. The layouts below were read from those
// loaders. A builder walks the shipped record field by field first, checks the
// values it is about to replace, and refuses when the walk or a value is not
// what it expects.

namespace bm::broomrows
{
    namespace
    {
        // ---- the record layouts ---------------------------------------------

        enum Kind : uint8_t
        {
            U8, U16, U32, U64,
            Str,         // u32 length, the bytes
            Loc,         // u8 type, u64 (key << 32 | sub), then a Str of its decimal digits
            Raw,         // `size` bytes
            List,        // u32 count, then count elements of `size` bytes
            VisZ,        // a flag byte, and a u32 after it only when the flag is 0 (CharacterInfo._visioningData)
            FilterList,  // UIFilterGroupInfo._uiFilterDataList
            PivotList    // InteractionInfo._interactionPivotList
        };
        struct Def
        {
            const char* name;
            Kind kind;
            uint16_t size = 0;
        };
        constexpr size_t kNone = ~size_t(0);
        struct Span
        {
            size_t at = kNone, size = 0;
        };
        struct Pivot
        {
            size_t start, body, end;   // the name's length, the bone's length (after the name), one past the pivot
        };

        // StringInfo +0x1720990
        constexpr Def kStringDefs[] = {
            { "stringKey", Str }, { "key", U32 }, { "isBlocked", U8 }, { "buffer", Str },
        };

        // CharacterInfo +0x16f2f80, up to _bulletItem. "flags" is the 64 one-byte flags from _isBlocked to
        // _isTeleportable; _isHirable is the 11th. The record goes on with lists and structs that are not walked.
        constexpr size_t kHirableIndex = 10, kFlagCount = 64;
        constexpr Def kCharacterDefs[] = {
            { "name", Str }, { "key", U32 }, { "flags", Raw, 64 }, { "u4c", U32 }, { "teleportSocketName", U32 },
            { "reviveSocketName", U32 }, { "characterAgeRun", Raw, 5 }, { "dailyRoutineInfo", U32 },
            { "interactionCategoryGroupInfo", U16 }, { "activityPresetInfo", U32 }, { "statChangeInfoWrapper", U32 },
            { "detectReactionInfo", U32 }, { "knowledgeInfo", U32 }, { "uiMapTextureInfo", U32 },
            { "talkTreeInfo", U16 }, { "maxAggroCount", U16 }, { "jobInfo", U16 }, { "callVehicleGimmickInfo", U32 },
            { "breakableObjectInfo", U16 }, { "convertItemInfo", U32 }, { "dialogVoiceInfo", U16 },
            { "tribeInfoWrapper", U32 }, { "memo", U32 }, { "lowerActionChartPackageGroupName", U32 },
            { "upperActionChartPackageGroupName", U32 }, { "characterTribeAndGenderString", U32 },
            { "skeletonVariationName", U32 }, { "factionInfo", U32 }, { "characterPrefabPath", U32 },
            { "socketFileName", U32 }, { "skeletonName", U32 }, { "uiDisplayLevelIconTexture", U32 },
            { "projectileInfoPackage", U32 }, { "characterGamePlayDataName", U32 }, { "appearanceName", U32 },
            { "uiIconPath", U32 }, { "playerIndex", U16 }, { "ownedMercenaryCharacterInfo", U32 },
            { "uiPortraitPath", U32 }, { "symbolImage", U32 }, { "category", U32 }, { "equipInfo", U32 },
            { "vehicleInfo", U16 }, { "personalityInfo", U16 }, { "npcInfo", U32 }, { "mercenaryInfo", U8 },
            { "empoweredOverlayColor", U32 }, { "characterWeight", U32 }, { "subZoneTagHash", U32 },
            { "empoweredOverlayColorRatio", U32 }, { "grownLevel", U32 }, { "gamePlayTriggerZone", U32 },
            { "baseMaterialKeyOverride", U32 }, { "armorMaterialKeyOverride", U32 }, { "characterPauseType", U32 },
            { "aiScriptPathHash", U32 }, { "tribeEffectHash", U32 }, { "farmBreedingCoolTime", U32 },
            { "aiScriptPathFocusHash", U32 }, { "interactionDistance", U32 }, { "defaultActionActionIndex", U32 },
            { "visioningData", VisZ }, { "vanishTickCount", U32 }, { "terrainRegionAutoSpawnInfo", U32 },
            { "weatherWeight", U32 }, { "characterScale", U32 }, { "attackByCollisionInfoListKey", U32 },
            { "shareValueNameHash", U32 }, { "terrainRegionSpawnPerCount", U32 }, { "characterThreatDialogInfo", U32 },
            { "bumpTypeHash", U32 }, { "pathFindTableName", U32 }, { "characterName", Loc }, { "characterDesc", Loc },
            { "characterEditName", Str }, { "callMercenaryCoolTime", U64 }, { "callMercenarySpawnDuration", U64 },
            { "defaultFriendlyValue", U64 }, { "miniGameParam", U64 }, { "bulletItem", U64 },
        };

        // MercenaryInfo +0x1715970
        constexpr Def kMercenaryDefs[] = {
            { "name", Str }, { "key", U8 }, { "isBlocked", U8 }, { "useVehicleEquipSlot", U8 }, { "isMapIconPet", U8 },
            { "isPetFriendlyAddition", U8 }, { "isMustHideAndRestore", U8 }, { "useFriendlyGauge", U8 },
            { "isLogoutInAI", U8 }, { "isFocusChangeable", U8 }, { "isUseInventory", U8 },
            { "isConnectionRequired", U8 }, { "isFireable", U8 }, { "isMinHPMakeable", U8 },
            { "mainDischargeableType", U8 }, { "isCostRecovery", U8 }, { "uiAlertOnAttacked", U8 },
            { "uiAlertOnAttackedMinDistance", U32 }, { "uiAlertOnAttackedMessage", U32 }, { "spawnPositionType", U8 },
            { "inventoryOwnerType", U8 }, { "summonOwnerOption", U8 }, { "saveLastValidPosition", U8 },
            { "checkIndoor", U8 }, { "checkInventoryPushOnSpawn", U8 }, { "isShowLevelUpAlert", U8 },
            { "isFarmHire", U8 }, { "canHaveBreedingTargets", U8 }, { "useCampLevel", U8 }, { "isForceStackable", U8 },
            { "applyEquipItemStat", U8 }, { "allowExceedLimitHireCount", U8 }, { "checkItemNoOnPushToItem", U8 },
            { "mercenaryType", U8 }, { "isBuffLevelChangeable", U8 }, { "farFromLeaderOption", U8 },
            { "summonAfterRegist", U8 }, { "isPlayable", U8 }, { "isSelectMercenarySpawn", U8 },
            { "isSummonRecoveryItem", U8 }, { "isRequireLogin", U8 }, { "isMainMercenaryInfo", U8 },
            { "isCheckGrownTargetList", U8 }, { "isShowHorseStatus", U8 }, { "isLossInventoryItem", U8 },
            { "checkOwnedItemForRestoreItem", U8 }, { "useWagonRoad", U8 }, { "isShowDDDStatusDetail", U8 },
            { "feedFromGimmickInfo", U32 }, { "parentMercenaryGroupInfo", U8 }, { "sharedSummonCountTag", U32 },
            { "recoveryCostItemInfo", U32 }, { "cameraPresetCenterHash", U32 }, { "cameraPresetHash", U32 },
            { "defaultLimitHireCount", U32 }, { "maxLimitHireCount", U32 }, { "defaultLimitSummonCount", U32 },
            { "statusUIList", List, 4 }, { "allowedSpawnActorTypeList", List, 1 }, { "expandedSlotList", List, 16 },
        };

        // MercenaryGroupInfo +0x1715630
        constexpr Def kGroupDefs[] = {
            { "name", Str }, { "key", U8 }, { "isBlocked", U8 }, { "parentMercenaryGroupInfo", U8 },
            { "mercenaryInfoList", List, 1 }, { "childMercenaryGroupInfoList", List, 1 },
            { "allowOperationTypeList", List, 1 }, { "isUsingSpawnedUsable", U8 }, { "useChangeMainMercenary", U8 },
            { "useVehicleKey", U8 }, { "useReserveOrMain", U8 }, { "isUsingPortraitTexture", U8 },
            { "isDeadStateUpdatable", U8 }, { "isHyosiUsable", U8 }, { "isItemContributionCountable", U8 },
        };

        // ReserveSlotInfo +0x1718e00
        constexpr Def kSlotDefs[] = {
            { "name", Str }, { "key", U32 }, { "isBlocked", U8 }, { "isSelfPlayerOnly", U8 }, { "isShared", U8 },
            { "autoChangeOnEmpty", U8 }, { "restoreOnRetry", U8 }, { "usingType", U8 }, { "reserveSlotType", U8 },
            { "autoUseItemInfo", U32 }, { "convertItemInfo", U32 }, { "coolTime", U32 }, { "sendGimmickEventKey", U32 },
            { "timeLimit", U64 }, { "fillDataList", List, 12 }, { "memo", Str }, { "enableTribeList", List, 4 },
            { "enableMercenaryList", List, 1 }, { "enableReserveSlotList", List, 8 }, { "enableItemGroupList", List, 4 },
            { "reserveSlotTargetList", List, 2 }, { "enableSpecialNameHashList", List, 4 },
        };

        // VehicleInfo +0x1724de0. "seats" is sixteen (u32, u32) pairs, the second of a pair the seat's bone key.
        constexpr size_t kSeatSize = 8;
        constexpr Def kVehicleDefs[] = {
            { "name", Str }, { "key", U16 }, { "isBlocked", U8 }, { "rideOnCrewAudioState", U8 },
            { "showCountOnUI", U8 }, { "isPlatformVehicle", U8 }, { "sendDamageTo", U8 },
            { "platformHitEventMinAttackImpulseLevel", U8 }, { "contactImpulseEvent", U8 },
            { "characterSwitchable", U8 }, { "rideOffCrewAudioState", U8 }, { "maxParentLinkAttachCount", U8 },
            { "maxVehicleSeat", U8 }, { "escapeRoadGroupType", U8 }, { "iconPath", U32 }, { "riderDetectInfo", U16 },
            { "uiMapTextureInfo", U32 }, { "minPlatformShakeAngularAccThreshold", U32 },
            { "minPlatformShakeAngularVelThreshold", U32 }, { "removeFogRadius", U32 }, { "u28", U32 },
            { "vehicleSpawnUpperAction", U32 }, { "riderSpawnUpperAction", U32 }, { "checkDistanceToGround", U32 },
            { "seatGimmickDockingTag", Str }, { "maxAllowableHeight", U32 }, { "vehicleTypeNameHash", U32 },
            { "seats", Raw, 128 }, { "parentLinks", Raw, 16 }, { "cargoSeatIndexList", List, 1 },
        };

        // CharacterAppearanceIndexInfo +0x16fc4f0
        constexpr Def kAppearanceDefs[] = {
            { "name", Str }, { "key", U64 }, { "isBlocked", U8 }, { "characterEditName", U32 },
            { "appearanceName", U32 }, { "characterScale", U32 }, { "characterInfo", U32 },
        };

        // UIMapTextureInfo +0x1725870
        constexpr Def kMapDefs[] = {
            { "name", Str }, { "key", U32 }, { "isBlocked", U8 }, { "uiHeightDiffSignData", List, 18 },
            { "isUIMapMissionType", U8 }, { "isUIMapNPCType", U8 }, { "isActorType", U8 }, { "isAlwayShowMinimap", U8 },
            { "isPerspectiveIcon", U8 }, { "isUIMapDebugQuestAreaType", U8 }, { "isShowTooltip", U8 },
            { "isRegionKnowledgeIcon", U8 }, { "isUIMapDebugQuestType", U8 }, { "isUIMapQuestType", U8 },
            { "checkHasOwnerActorIcon", U8 }, { "isKeepShowByCharacterInfo", U8 }, { "otherSpaceForceShow", U8 },
            { "isDiscoverGimmickIcon", U8 }, { "isWorldmapFocusable", U8 }, { "uiFilterGroupByInfo", U8 },
            { "indoorStateForceShow", U8 }, { "useChangeScale", U8 }, { "isFixScaleIconImage", U8 },
            { "useChangeScaleWhenZoomOut", U8 }, { "minimapForceUpdateIcon", U8 }, { "useAutoAbyssLayer", U8 },
            { "isSimpleMaterial", U8 }, { "uiMapLayerType", U8 }, { "isFlexibleSize", U8 }, { "isFlexibleIcon", U8 },
            { "isMovingPlatform", U8 }, { "filterType", U8 }, { "uiTemplateName", U32 },
            { "filterGroupParentInfo", U32 }, { "knowledgeInfo", U32 }, { "uiTextureName", U32 },
            { "uiFilterTextureName", U32 }, { "uiFilterGroupComponentName", U32 }, { "uiSmallTextureName", U32 },
            { "mapIconType", U32 }, { "gameplayTriggerInfo", U32 }, { "lerpMaxZoom", U32 }, { "lerpMinZoom", U32 },
            { "changeScaleRatio", U32 }, { "lerpSize", U32 }, { "lerpIconMinSize", U32 }, { "autoRemoveDistance", U32 },
            { "zIndex", U32 }, { "platformZIndex", U32 }, { "minScale", U32 }, { "maxScale", U32 },
            { "worldPosition", Raw, 12 }, { "filterGroupName", Loc }, { "tooltipText", Loc },
            { "canDeactivateStickyIcon", U8 },
        };

        // UIFilterGroupInfo +0x1725570
        constexpr Def kFilterDefs[] = {
            { "name", Str }, { "key", U32 }, { "isBlocked", U8 }, { "filterType", U8 }, { "uiIconPath", U32 },
            { "uiFilterDataList", FilterList }, { "uiGroupName", Loc },
        };

        // InteractionInfo +0x170ee40, up to and including the pivots. "flags" is the 24 one-byte flags from
        // _isBlocked to _checkObjectOnTop; _showWhenTargeted is the second. What follows the pivots is the
        // condition list, whose expressions are polymorphic trees, so it is not walked (see Suffix).
        constexpr size_t kShowWhenTargetedIndex = 1;
        constexpr Def kInteractionDefs[] = {
            { "name", Str }, { "key", U32 }, { "flags", Raw, 24 }, { "rewardDropSetInfo", U32 },
            { "uiKeyTriggerSoundName", U32 }, { "activityGroupInfo", U32 }, { "categoryInfo", U16 },
            { "inputKeyMapName", U32 }, { "onPreemptionSuccessAiEventKey", U32 }, { "dialogSetInfo", U32 },
            { "interactionName", Loc }, { "pivots", PivotList },
        };

        // ---- walking a record ------------------------------------------------

        template <typename T> std::string Bytes(T v) { return std::string(reinterpret_cast<const char*>(&v), sizeof v); }
        template <typename T> void Poke(std::string& s, size_t at, T v) { memcpy(&s[at], &v, sizeof v); }
        template <typename T> T Peek(const std::string& s, size_t at)
        {
            T v{};
            if (at <= s.size() && s.size() - at >= sizeof v) memcpy(&v, s.data() + at, sizeof v);
            return v;
        }

        bool Take(const std::string& s, size_t& p, uint64_t n)
        {
            if (p > s.size() || n > s.size() - p) return false;
            p += static_cast<size_t>(n);
            return true;
        }
        bool SkipStr(const std::string& s, size_t& p)
        {
            if (!Take(s, p, 4)) return false;
            return Take(s, p, Peek<uint32_t>(s, p - 4));
        }
        bool SkipLoc(const std::string& s, size_t& p) { return Take(s, p, 9) && SkipStr(s, p); }

        // One pivot after its name: a bone, three positions with their own names, a pair of floats, a socket
        // and 13 u32s, then a name, a position and 24 bytes whose last 56 bytes' worth of keys the charts read.
        bool SkipPivots(const std::string& s, size_t& p, std::vector<Pivot>* out)
        {
            if (!Take(s, p, 4)) return false;
            const uint32_t count = Peek<uint32_t>(s, p - 4);
            for (uint32_t i = 0; i < count; ++i)
            {
                Pivot v{ p, 0, 0 };
                if (!SkipStr(s, p)) return false;
                v.body = p;
                if (!SkipStr(s, p) || !Take(s, p, 12) || !SkipStr(s, p) || !Take(s, p, 12) || !SkipStr(s, p) ||
                    !Take(s, p, 12) || !Take(s, p, 8) || !SkipStr(s, p) || !Take(s, p, 12) || !Take(s, p, 52) ||
                    !SkipStr(s, p) || !Take(s, p, 12) || !Take(s, p, 24))
                    return false;
                v.end = p;
                if (out) out->push_back(v);
            }
            return true;
        }

        // u32 count; per element a u32 count of 9-byte entries (u32, u32, u8), two u32s, a localized string, a flag.
        bool SkipFilters(const std::string& s, size_t& p)
        {
            if (!Take(s, p, 4)) return false;
            const uint32_t count = Peek<uint32_t>(s, p - 4);
            for (uint32_t i = 0; i < count; ++i)
            {
                if (!Take(s, p, 4) || !Take(s, p, uint64_t(Peek<uint32_t>(s, p - 4)) * 9) || !Take(s, p, 8) ||
                    !SkipLoc(s, p) || !Take(s, p, 1))
                    return false;
            }
            return true;
        }

        struct Walked
        {
            const Def* defs = nullptr;
            size_t count = 0;
            std::vector<Span> span;
            std::vector<Pivot> pivots;
            size_t end = 0;

            Span At(const char* name) const
            {
                for (size_t i = 0; i < span.size(); ++i)
                    if (!strcmp(defs[i].name, name)) return span[i];
                return Span{};
            }
        };

        // Walks `rec` through `defs`, up to the field `stopAfter` when given. `whole` demands that the fields
        // end exactly where the record does.
        bool Walk(const std::string& rec, const Def* defs, size_t count, Walked& w, std::string& why, bool whole,
                  const char* stopAfter = nullptr)
        {
            w.defs = defs;
            w.count = count;
            w.span.clear();
            w.pivots.clear();
            w.span.reserve(count);
            size_t p = 0;
            for (size_t i = 0; i < count; ++i)
            {
                const Def& d = defs[i];
                const size_t at = p;
                bool ok = false;
                switch (d.kind)
                {
                case U8: ok = Take(rec, p, 1); break;
                case U16: ok = Take(rec, p, 2); break;
                case U32: ok = Take(rec, p, 4); break;
                case U64: ok = Take(rec, p, 8); break;
                case Str: ok = SkipStr(rec, p); break;
                case Loc: ok = SkipLoc(rec, p); break;
                case Raw: ok = Take(rec, p, d.size); break;
                case List: ok = Take(rec, p, 4) && Take(rec, p, uint64_t(Peek<uint32_t>(rec, p - 4)) * d.size); break;
                case VisZ: ok = Take(rec, p, 1) && (rec[p - 1] != 0 || Take(rec, p, 4)); break;
                case FilterList: ok = SkipFilters(rec, p); break;
                case PivotList: ok = SkipPivots(rec, p, &w.pivots); break;
                }
                if (!ok)
                {
                    why = std::string("the field ") + d.name + " runs past the end of the record";
                    return false;
                }
                w.span.push_back(Span{ at, p - at });
                if (stopAfter && !strcmp(d.name, stopAfter)) break;
            }
            w.end = p;
            if (whole && p != rec.size())
            {
                why = "the record is longer than its fields";
                return false;
            }
            return true;
        }

        std::string Text(const std::string& rec, Span s) { return rec.substr(s.at + 4, s.size - 4); }
        template <typename T> T Get(const std::string& rec, const Walked& w, const char* name) { return Peek<T>(rec, w.At(name).at); }

        struct Edit
        {
            const char* field;
            std::string bytes;
        };

        // `rec` with the named fields' bytes replaced.
        bool Rewrite(const std::string& rec, const Walked& w, std::vector<Edit> edits, std::string& out, std::string& why)
        {
            std::vector<std::pair<Span, const std::string*>> at;
            for (const Edit& e : edits)
            {
                const Span s = w.At(e.field);
                if (s.at == kNone)
                {
                    why = std::string("the layout has no field ") + e.field;
                    return false;
                }
                at.emplace_back(s, &e.bytes);
            }
            std::sort(at.begin(), at.end(), [](const auto& a, const auto& b) { return a.first.at < b.first.at; });
            out.clear();
            size_t pos = 0;
            for (const auto& [s, bytes] : at)
            {
                if (s.at < pos)
                {
                    why = "two edits overlap";
                    return false;
                }
                out.append(rec, pos, s.at - pos);
                out += *bytes;
                pos = s.at + s.size;
            }
            out.append(rec, pos, std::string::npos);
            return true;
        }

        // ---- constants -------------------------------------------------------

        constexpr const char* kBroomyName = "Riding_Broomy_1";
        constexpr uint8_t  kMercKey = 87;
        constexpr const char* kMercName = "Vehicle_Broom";
        constexpr uint16_t kVehicleKey = 20000;
        constexpr const char* kVehicleName = "Broom";

        // Riding_AlpineIbex_1, a riding row the game no longer uses.
        constexpr uint32_t kIbexKey = 29448;
        constexpr uint16_t kIbexVehicle = 16993;       // AlpineIbex
        constexpr uint64_t kFaintSeconds = 1;          // the ibex's is 300
        constexpr uint32_t kVanishMs = 1000;           // the ibex's (and every riding row's) is 900000
        constexpr uint8_t  kHorseMerc = 78;            // Vehicle_Horse
        constexpr uint8_t  kSpecialMerc = 81;          // Vehicle_Special, the ibex row's list
        constexpr uint8_t  kVehicleGroup = 65;         // mercenarygroupinfo Vehicle
        constexpr uint32_t kVehicleSlot = 1000006;     // reserveslot VehicleSlot, the saddle wedge
        // interactioninfo Broom_Ride, a cut row that seats the rider on the
        // broom's B_Rider_01 with pivots of its own, whose last field Kliff's
        // riding chart tests to pick the broom's mount; under Wyvern_Ride
        // (1000005) Kliff rode Broomy in the Wyvern's poses. There is no
        // Broom_RideAir, so a mount in the air is still the Wyvern's.
        constexpr uint32_t kWyvernRide[2] = { 1000068, 1000264 };   // Broom_Ride, Wyvern_RideAir
        constexpr uint32_t kIbexRide[2] = { 1000098, 1000085 };
        // Broom_Ride_Catch, Broomy's own row for mounting with a bound captive
        // on Kliff's shoulder. Horse_Ride_Catch (397) on Broomy's list showed
        // no prompt and kept the Character radial from opening (Seth, 19
        // October), so this is Broom_Ride with what makes a catch ride: the
        // condition list and the catch tag 29646F2B, both as
        // AlpineIbex_Ride_Catch differs from AlpineIbex_Ride (their seats are
        // byte for byte the same).
        constexpr uint32_t kHorseCatchRide = 397;
        constexpr uint32_t kBroomCatchRide = 1900068;
        constexpr const char* kBroomCatchName = "Broom_Ride_Catch";
        // The escort mount actions Broom_Ride_Catch's pivots end with, right and
        // left (damianepatches.h for Kliff's, escortmountpatches.h for Broomy's).
        constexpr uint32_t kEscortRight = 0x33F65149;
        constexpr uint32_t kEscortLeft = 0x4DC25870;
        // Broom_Carry_Dead, loading a captive or a body onto Broomy while
        // Kliff stays on foot (Seth, 21 October: "no option to load him onto
        // the broom and stay off the broom"). A horse offers Horse_Carry_Dead
        // (301) from its own row: Kliff's on-foot dispatchers 2820B750 (left)
        // and FDF23C06 (right) pick the move by what he carries, and for a
        // bound man it sends him to the horse's lying idle, held by the horse.
        // Broomy's copy is the horse's row renamed, with wider pivots and no
        // need for focus.
        constexpr uint32_t kHorseCarry = 301;
        constexpr uint32_t kBroomCarry = 1900069;
        constexpr const char* kBroomCarryName = "Broom_Carry_Dead";
        constexpr uint32_t kLoadRight = 0xFDF23C06;
        constexpr uint32_t kLoadLeft = 0x2820B750;
        bool g_offerLoad = true;
        // A load goes onto a cargo seat. Of the 34 vehicleinfo rows only the
        // horse's names one: its _cargoSeatIndexList is [1], and seat 1 is
        // B_Rider_02, ahead of the saddle. The ibex row Broomy is copied from
        // has one seat and no cargo seat. The broom's skeleton has B_Rider_02
        // too, on B_Rider_01.
        constexpr uint32_t kNoBone = 0xEAC5E173;   // what an unused seat holds, and every unset string
        constexpr uint8_t kCargoSeat = 1;
        // Taking the load off is the carried actor's Horse_UnCarry_Dead (302),
        // on 5394 characterinfo rows, and it asks CheckRidingVehicleType(horse)
        // of him: the _vehicleTypeNameHash of the vehicle row, hashlittle
        // "horse". Broomy's is the ibex's. NpcBind_Release (11026) and
        // NpcBind_Down_Release (1000222) ask the same under a NOT. In all
        // three the call becomes horse OR the ibex's type, so "on a horse"
        // means a horse or Broomy (no ibex has a cargo seat). Compiled, a call
        // is 03, the u16 function id, its arguments and a u16 subject, here 0
        // (the actor himself); OR is 00 before its two operands. The function
        // id of CheckRidingVehicleType is 0x0108 on 3392 (0x0103 before).
        constexpr uint32_t kHorseType = 0xA50D2251, kIbexType = 0x5B9EB68A;
        constexpr uint32_t kOnHorseRows[] = { 302, 11026, 1000222 };
        constexpr char kCheckRidingVehicleType[3] = { '\x03', '\x08', '\x01' };
        // A horse's load pivots stand Kliff 0.7 m either side of its root and
        // 1.25 m along it, within 1.5 m and 2 m up, facing within 90 degrees.
        // On Broomy that was a spot by the bristles (Seth, test19: "a very
        // specific spot"), and beside the cargo seat with a 2.25 m reach it
        // was still small (test20: "it should be the same area i see ride
        // in"). Broom_Ride's whole pivots, on B_Rider_01, hid the load
        // altogether (test21). Broomy's keep the horse's pivots, boneless and
        // turning Kliff to face the broom, with Broom_Ride's spot (1.2 m out,
        // 0.1 m along, which is 0.3 m ahead of B_Rider_01) and reach: 2.5 m,
        // 5 m up, any facing. Offsets inside a pivot body, which begins at
        // its bone's length; 3392 has the reach 4 bytes later than 2976 did.
        constexpr size_t kPivotOutAt = 36, kPivotAlongAt = 44, kPivotReachAt = 72, kPivotUpAt = 76, kPivotDownAt = 80,
                         kPivotFacingAt = 84, kPivotKeyFromEnd = 56;
        constexpr float kHorseOut = 0.7f, kBroomOut = 1.2f, kHorseAlong = 1.25f, kBroomAlong = 0.1f;
        constexpr float kHorseReach = 1.5f, kBroomReach = 2.5f, kHorseUp = 2.0f, kBroomUp = 5.0f, kDown = -2.0f;
        constexpr float kHalfTurn = 1.5707964f, kFullTurn = 3.1415927f;

        // The charts of GoldStar, a cut dragon no character uses, which the
        // plugin serves as the broom's own (broomchart.h); the Wyvern's when
        // it cannot.
        constexpr const char* kOwnUpper = "CD_M0004_Ride_GoldStar_Upper";
        constexpr const char* kOwnLower = "CD_M0004_Ride_GoldStar_Lower";
        constexpr const char* kWyvernUpper = "CD_M0004_Dragon";
        constexpr const char* kWyvernLower = "CD_M0004_Dragon_Lower";
        bool g_ownCharts = true;
        constexpr const char* kGameplay = "animal_seal";
        constexpr const char* kSkeleton = "character/model/4_riding/cd_r0032_00_broom/cd_r0032_00_broom.pab";
        constexpr const char* kPortrait = "cd_portraitimage_Broomy";
        constexpr const char* kPortraitFile = "UI/texture/icon/itemicon_prefab_cd_t0000_broom_0001.dds";
        // The map icon. vehicleinfo names a uimaptextureinfo row, the row
        // names a component of the map's markup (minimapicon.thtml and
        // worldmapicon.thtml) in its _uiTemplateName, and the component's CSS
        // class draws a white silhouette that a hired mount's class tints
        // #74d4e5. The ibex's row is 5347; Broomy's is a copy under its own
        // key naming its own component, drawn with the broom item's map icon.
        constexpr uint32_t kIbexMapTexture = 5347;
        constexpr const char* kIbexMapName = "Actor_Vehicle_AlpineIbex_Hired";
        constexpr const char* kIbexMapComponent = "MapIcon_ActorVehicleAlpineIbex_Hired";
        constexpr const char* kMapName = "Actor_Vehicle_Broomy_Hired";
        constexpr const char* kMapComponent = "MapIcon_ActorVehicleBroomy_Hired";
        constexpr const char* kIbexMapImage = "textureid(cd_Icon_map_AlpineIbex)";
        constexpr const char* kMapImage = "textureid(cd_Icon_map_item_broom_00)";
        constexpr uint32_t kUnset = kNoBone;

        // What the ibex row names. 3392 dropped the cd_ prefix of the skeleton's folder names.
        constexpr const char* kIbexUpper = "CD_R0001_Horse";
        constexpr const char* kIbexGameplay = "animal_horse";
        constexpr const char* kIbexAppearance =
            "character/appearance/4_riding/cd_r0002_00_alpineibex/cd_r0002_00_alpineibex_0001_00001.app_xml";
        constexpr const char* kIbexSkeleton = "character/model/4_riding/r0002_00_fourfeet/r0002_00_horse/r0002_00_horse.pab";
        constexpr const char* kIbexVariation =
            "character/binary/skeletonvariation/4_riding/r0002_00_fourfeet/r0002_00_alpineibex/r0002_00_alpineibex_0001.pabc";
        constexpr const char* kIbexPortrait = "cd_portraitimage_Riding_AlpineIbex_1";
        constexpr uint32_t kNameSub = 0x30;

        uint32_t Key(const char* text) { return bm::hash::Little(text); }
        const char* ChartUpper() { return g_ownCharts ? kOwnUpper : kWyvernUpper; }
        const char* ChartLower() { return g_ownCharts ? kOwnLower : kWyvernLower; }

        std::string NameBlock(const char* text) { return Bytes<uint32_t>(static_cast<uint32_t>(strlen(text))) + text; }

        // A localized string field: a u8 type, a u64 of (key << 32) | sub, then
        // the decimal digits of that number behind a u32 length.
        std::string Localized(uint8_t type, uint32_t key, uint32_t sub)
        {
            char digits[32];
            const uint64_t id = (static_cast<uint64_t>(key) << 32) | sub;
            snprintf(digits, sizeof digits, "%llu", static_cast<unsigned long long>(id));
            return std::string(1, static_cast<char>(type)) + Bytes<uint64_t>(id) +
                   Bytes<uint32_t>(static_cast<uint32_t>(strlen(digits))) + digits;
        }

        // True when the Loc field at `s` holds (key << 32) | sub with its digits; fills `sub`.
        bool IsLocalizedOf(const std::string& rec, Span s, uint32_t key, uint32_t& sub, uint8_t& type)
        {
            if (s.at == kNone) return false;
            type = static_cast<uint8_t>(rec[s.at]);
            const uint64_t id = Peek<uint64_t>(rec, s.at + 1);
            sub = static_cast<uint32_t>(id);
            return (id >> 32) == key && rec.compare(s.at, s.size, Localized(type, key, sub)) == 0;
        }

        size_t Count(const std::string& hay, const std::string& needle)
        {
            size_t n = 0;
            for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) ++n;
            return n;
        }

        // Where `needle` sits, when it sits there exactly once.
        bool Once(const std::string& rec, const std::string& needle, const char* what, size_t& at, std::string& why)
        {
            const size_t n = Count(rec, needle);
            if (n != 1)
            {
                why = std::string(what) + (n ? " is in the record more than once" : " is not in the record");
                return false;
            }
            at = rec.find(needle);
            return true;
        }

        bool Splice(std::string& rec, const std::string& from, const std::string& to, const char* what, std::string& why)
        {
            size_t at;
            if (!Once(rec, from, what, at, why)) return false;
            rec.replace(at, from.size(), to);
            return true;
        }

        // ---- the tables ----------------------------------------------------

        bool Strings(tablefile::Table& t, std::string& report, std::string& why)
        {
            const char* texts[] = { ChartUpper(), ChartLower(), kGameplay, kAppearancePath, kSkeleton, kPortrait,
                                    kMapComponent };
            int added = 0;
            for (const char* text : texts)
            {
                const uint32_t key = Key(text);
                if (const std::string* rec = t.Record(key))
                {
                    Walked w;
                    if (!Walk(*rec, kStringDefs, std::size(kStringDefs), w, why, true) || Text(*rec, w.At("buffer")) != text)
                    {
                        why = std::string("stringinfo's key for \"") + text + "\" holds another string";
                        return false;
                    }
                    continue;
                }
                // an empty _stringKey, the key, _isBlocked 0, the text
                const std::string rec = Bytes<uint32_t>(0) + Bytes<uint32_t>(key) + std::string(1, '\0') + NameBlock(text);
                if (!t.Append(key, rec))
                {
                    why = "a string row could not be added";
                    return false;
                }
                ++added;
            }
            char line[96];
            snprintf(line, sizeof line, "stringinfo: %d new strings", added);
            report = line;
            return true;
        }

        // Each row's _mercenaryInfo, the key of its mercenary list, read at its place in the record (-1 for 255,
        // which is "none").
        void MercenaryInfoByRow(const tablefile::Table& t, std::vector<int16_t>& out)
        {
            out.assign(t.Rows(), -1);
            Walked w;
            std::string why;
            for (size_t row = 0; row < t.Rows(); ++row)
            {
                const std::string& rec = t.RecordAt(row);
                if (!Walk(rec, kCharacterDefs, std::size(kCharacterDefs), w, why, false, "mercenaryInfo")) continue;
                const uint8_t key = static_cast<uint8_t>(rec[w.At("mercenaryInfo").at]);
                if (key != 255) out[row] = key;
            }
        }

        bool Character(tablefile::Table& t, std::string& report, std::string& why, CharacterFacts* facts,
                       bool interactionsServed)
        {
            const std::string* src = t.Record(kIbexKey);
            if (!src)
            {
                why = "characterinfo has no Riding_AlpineIbex_1 row";
                return false;
            }
            Walked w;
            if (!Walk(*src, kCharacterDefs, std::size(kCharacterDefs), w, why, false))
            {
                why = "characterinfo 29448: " + why;
                return false;
            }
            const std::string& old = *src;
            const Span flagsAt = w.At("flags");
            // The values this changes are the ones the ibex row has.
            const bool same =
                Text(old, w.At("name")) == "Riding_AlpineIbex_1" && Get<uint32_t>(old, w, "key") == kIbexKey &&
                flagsAt.size == kFlagCount && old[flagsAt.at + kHirableIndex] == 0 &&
                Get<uint32_t>(old, w, "upperActionChartPackageGroupName") == Key(kIbexUpper) &&
                Get<uint32_t>(old, w, "lowerActionChartPackageGroupName") == kUnset &&
                Get<uint32_t>(old, w, "characterGamePlayDataName") == Key(kIbexGameplay) &&
                Get<uint32_t>(old, w, "appearanceName") == Key(kIbexAppearance) &&
                Get<uint32_t>(old, w, "skeletonName") == Key(kIbexSkeleton) &&
                Get<uint32_t>(old, w, "skeletonVariationName") == Key(kIbexVariation) &&
                Get<uint16_t>(old, w, "vehicleInfo") == kIbexVehicle && Get<uint8_t>(old, w, "mercenaryInfo") == kSpecialMerc &&
                Get<uint32_t>(old, w, "uiPortraitPath") == Key(kIbexPortrait) &&
                Get<uint32_t>(old, w, "vanishTickCount") == 900000 && Get<uint64_t>(old, w, "callMercenaryCoolTime") == 300;
            if (!same)
            {
                why = "the ibex's row does not hold the values Broomy's was written against (hash of its charts, "
                      "gameplay data, appearance, skeleton, portrait, vehicle, mercenary list, vanish time or cooldown)";
                return false;
            }
            // Its localized strings carry the character key twice, raw and
            // as the decimal digits of (key << 32) | sub.
            uint32_t sub[2] = {};
            uint8_t type[2] = {};
            if (!IsLocalizedOf(old, w.At("characterName"), kIbexKey, sub[0], type[0]) || sub[0] != 0x30 ||
                !IsLocalizedOf(old, w.At("characterDesc"), kIbexKey, sub[1], type[1]) || sub[1] != 0x31)
            {
                why = "the ibex row's name and description strings are not what they were";
                return false;
            }

            std::string flags = old.substr(flagsAt.at, flagsAt.size);
            flags[kHirableIndex] = 1;
            std::string rec;
            if (!Rewrite(old, w,
                         { { "name", NameBlock(kBroomyName) },
                           { "key", Bytes<uint32_t>(kBroomyKey) },
                           { "flags", flags },
                           { "lowerActionChartPackageGroupName", Bytes<uint32_t>(Key(ChartLower())) },
                           { "upperActionChartPackageGroupName", Bytes<uint32_t>(Key(ChartUpper())) },
                           { "skeletonVariationName", Bytes<uint32_t>(kUnset) },
                           { "skeletonName", Bytes<uint32_t>(Key(kSkeleton)) },
                           { "characterGamePlayDataName", Bytes<uint32_t>(Key(kGameplay)) },
                           { "appearanceName", Bytes<uint32_t>(Key(kAppearancePath)) },
                           { "uiPortraitPath", Bytes<uint32_t>(Key(kPortrait)) },
                           { "vehicleInfo", Bytes<uint16_t>(kVehicleKey) },
                           { "mercenaryInfo", Bytes<uint8_t>(kMercKey) },
                           // _vanishTickCount, u32 milliseconds, taken to be how long a fainted body stays in the
                           // world. With the ibex's 15 minutes a fainted Speeder Bike could be called again only
                           // after a reload (Buxunqingmo's 1.0.1 log, 6 October); Speeder Bike has had 1000 ms
                           // since 1.0.2.
                           { "vanishTickCount", Bytes<uint32_t>(kVanishMs) },
                           { "characterName", Localized(type[0], kBroomyKey, 0x30) },
                           { "characterDesc", Localized(type[1], kBroomyKey, 0x31) },
                           // _callMercenaryCoolTime, u64 seconds. Under _mercenaryCoolTimeType 1 it is how long a
                           // fainted mount cannot be called ("Cannot summon because it's fainted"). One second
                           // rather than 0 keeps the timer, and the HP refill at its end, running as for the ibex.
                           { "callMercenaryCoolTime", Bytes<uint64_t>(kFaintSeconds) } },
                         rec, why))
                return false;

            // Past the walked fields: more localized strings (the hire message among them) and the lists. A
            // localized string of the ibex's key is made Broomy's.
            const size_t head = rec.size() - (old.size() - w.end);
            std::string tail = rec.substr(head);
            rec.resize(head);
            for (uint32_t s = 0x30; s < 0x40; ++s)
            {
                const std::string from = Localized(3, kIbexKey, s);
                const size_t n = Count(tail, from);
                if (n > 1)
                {
                    why = "a localized string is in the ibex row twice";
                    return false;
                }
                if (n == 1) tail.replace(tail.find(from), from.size(), Localized(3, kBroomyKey, s));
            }

            // _interactionInfoList and _interactionInfoListWithRecursiveChild, side by side and equal on the ibex:
            // its two ride interactions become Broom_Ride, the Wyvern's mount in the air, Broom_Carry_Dead and
            // Broom_Ride_Catch. Without focus the game offers one interaction per target, the first on its list
            // whose conditions pass; focus lists them all. A horse lists Horse_Ride_Catch before Horse_Carry_Dead,
            // so with a bounty on Kliff's shoulder Ride wins and the load takes focus. Broomy's list has the load
            // first (Seth, test22: "load shows on first call now but i still need focus"); riding with the bounty
            // on his shoulder takes focus. Broom_Ride_Catch and Broom_Carry_Dead are interactioninfo rows of
            // Broomy's own. When the game keeps its own interactioninfo (another mod changed it, so it was not
            // built or not served), the keys name no row, and a list holding them is broken. The list is then
            // Broom_Ride and Wyvern_RideAir alone. The children of these rows are none, so the recursive list is
            // the plain one.
            const bool load = g_offerLoad && interactionsServed;
            const std::string ibexList = Bytes<uint32_t>(2) + Bytes<uint32_t>(kIbexRide[0]) + Bytes<uint32_t>(kIbexRide[1]);
            const std::string wyvernList =
                !interactionsServed
                    ? Bytes<uint32_t>(2) + Bytes<uint32_t>(kWyvernRide[0]) + Bytes<uint32_t>(kWyvernRide[1])
                    : Bytes<uint32_t>(load ? 4 : 3) + Bytes<uint32_t>(kWyvernRide[0]) + Bytes<uint32_t>(kWyvernRide[1]) +
                          (load ? Bytes<uint32_t>(kBroomCarry) : std::string()) + Bytes<uint32_t>(kBroomCatchRide);
            if (!Splice(tail, ibexList + ibexList, wyvernList + wyvernList, "the ibex's two interaction lists", why))
                return false;
            rec += tail;
            if (Count(rec, Bytes<uint32_t>(kIbexKey)) != 0)
            {
                why = "the ibex's key is still somewhere in the record";
                return false;
            }

            const int row = static_cast<int>(t.Rows());
            if (!t.Append(kBroomyKey, rec))
            {
                why = "characterinfo already has key 1900001";
                return false;
            }
            if (facts)
            {
                facts->broomyRow = row;
                MercenaryInfoByRow(t, facts->mercInfo);
            }
            char line[256];
            snprintf(line, sizeof line, "characterinfo: %u %s at row %d, from Riding_AlpineIbex_1%s", kBroomyKey,
                     kBroomyName, row,
                     !interactionsServed ? ", interactions Broom_Ride and Wyvern_RideAir only (interactioninfo is the game's own)"
                     : load ? ", interactions with Broom_Ride_Catch and Broom_Carry_Dead"
                            : ", interactions with Broom_Ride_Catch");
            report = line;
            return true;
        }

        bool Mercenary(tablefile::Table& t, std::string& report, std::string& why, CharacterFacts* facts)
        {
            const std::string* src = t.Record(kHorseMerc);
            Walked w;
            if (!src || !Walk(*src, kMercenaryDefs, std::size(kMercenaryDefs), w, why, true) ||
                Text(*src, w.At("name")) != "Vehicle_Horse" || Get<uint8_t>(*src, w, "key") != kHorseMerc)
            {
                why = "mercenaryinfo 78 is not Vehicle_Horse";
                return false;
            }
            // _mainDischargeableType 2, _spawnPositionType 3, _summonOwnerOption, _inventoryOwnerType 2, then
            // _allowedSpawnActorTypeList [5]. With the horses' option 3, Damiane's saddle wedge showed her own
            // horse, not Kliff's, and no Broomy (Seth, 4 October). Vehicle_Special's 2 shows her the Wyvern, so
            // Broomy's list takes 2.
            const Span list = w.At("allowedSpawnActorTypeList");
            if (Get<uint8_t>(*src, w, "mainDischargeableType") != 2 || Get<uint8_t>(*src, w, "spawnPositionType") != 3 ||
                Get<uint8_t>(*src, w, "summonOwnerOption") != 3 || Get<uint8_t>(*src, w, "inventoryOwnerType") != 2 ||
                src->compare(list.at, list.size, Bytes<uint32_t>(1) + std::string(1, '\x05')) != 0)
            {
                why = "Vehicle_Horse's summon options are not what they were";
                return false;
            }
            std::string rec;
            if (!Rewrite(*src, w, { { "name", NameBlock(kMercName) }, { "key", Bytes<uint8_t>(kMercKey) },
                                    { "summonOwnerOption", Bytes<uint8_t>(2) } }, rec, why))
                return false;
            const int row = static_cast<int>(t.Rows());
            if (!t.Append(kMercKey, rec))
            {
                why = "mercenaryinfo already has key 87";
                return false;
            }
            if (facts) facts->broomyList = row;
            char line[128];
            snprintf(line, sizeof line, "mercenaryinfo: %u %s at index %d, from Vehicle_Horse with summon owner option 2",
                     kMercKey, kMercName, row);
            report = line;
            return true;
        }

        bool Group(tablefile::Table& t, std::string& report, std::string& why)
        {
            const std::string* src = t.Record(kVehicleGroup);
            Walked w;
            if (!src || !Walk(*src, kGroupDefs, std::size(kGroupDefs), w, why, true) ||
                Text(*src, w.At("name")) != "Vehicle" || Get<uint8_t>(*src, w, "key") != kVehicleGroup)
            {
                why = "mercenarygroupinfo has no Vehicle group";
                return false;
            }
            const std::string six("\x4E\x4F\x50\x52\x51\x41", 6);
            const Span list = w.At("mercenaryInfoList");
            if (src->compare(list.at, list.size, Bytes<uint32_t>(6) + six) != 0)
            {
                why = "the Vehicle group's list is not the one Broomy was written against";
                return false;
            }
            std::string rec;
            if (!Rewrite(*src, w, { { "mercenaryInfoList", Bytes<uint32_t>(7) + six + static_cast<char>(kMercKey) } }, rec, why))
                return false;
            t.Set(kVehicleGroup, rec);
            report = "mercenarygroupinfo: Vehicle lists Vehicle_Broom";
            return true;
        }

        // VehicleSlot, the saddle wedge, takes Broomy's list as its third,
        // after the horses' (78) and Vehicle_Special's (81). A list in two
        // slots crashed the game at boot (28 September), so Broomy has no
        // slot of its own.
        bool Slot(tablefile::Table& t, std::string& report, std::string& why)
        {
            const std::string* src = t.Record(kVehicleSlot);
            Walked w;
            if (!src || !Walk(*src, kSlotDefs, std::size(kSlotDefs), w, why, true) ||
                Text(*src, w.At("name")) != "VehicleSlot" || Get<uint32_t>(*src, w, "key") != kVehicleSlot)
            {
                why = "reserveslot 1000006 is not VehicleSlot";
                return false;
            }
            const std::string lists = std::string(1, static_cast<char>(kHorseMerc)) + static_cast<char>(kSpecialMerc);
            const Span list = w.At("enableMercenaryList");
            if (src->compare(list.at, list.size, Bytes<uint32_t>(2) + lists) != 0)
            {
                why = "VehicleSlot's mercenary lists are not the horses' and Vehicle_Special's";
                return false;
            }
            std::string rec;
            if (!Rewrite(*src, w, { { "enableMercenaryList", Bytes<uint32_t>(3) + lists + static_cast<char>(kMercKey) } }, rec,
                         why))
                return false;
            t.Set(kVehicleSlot, rec);
            report = "reserveslot: VehicleSlot, the saddle wedge, takes Broomy's list after the horses' and "
                     "Vehicle_Special's";
            return true;
        }

        // ---- interactioninfo -------------------------------------------------

        bool WalkInteraction(const std::string& rec, Walked& w, std::string& why)
        {
            return Walk(rec, kInteractionDefs, std::size(kInteractionDefs), w, why, false);
        }

        std::string PivotName(const std::string& rec, const Pivot& p) { return rec.substr(p.start + 4, p.body - p.start - 4); }

        // A ride or load row after its pivots is u32 count of condition sets, the sets, then
        //   u32 name selections (0), u8 approach type, u8 wall check type, u32 pop items (0),
        //   u32 tag count, the tags, u32 child interactions (0), u32 panel name, u32 event name.
        // A set is two optional expression trees (a flag byte and a polymorphic tree whose call nodes depend on
        // their function) and 11 more bytes, so the sets are not walked. They are found from the row's end instead:
        // the last 12 bytes are the child count and the two names, and the tag count is the one place, among the
        // possible counts, where the u32 before the tags is 0 (pop items) and the u32 ten bytes before it is 0.
        // `elementsEnd` is where the sets end, `tagsAt` where the tag count is.
        bool Suffix(const std::string& rec, size_t pivotsEnd, size_t& elementsEnd, size_t& tagsAt, std::vector<uint32_t>& tags)
        {
            if (rec.size() < pivotsEnd + 4 + 16 + 10 || Peek<uint32_t>(rec, rec.size() - 12) != 0) return false;
            int found = -1;
            for (int n = 0; n < 12; ++n)
            {
                if (rec.size() < size_t(16 + 4 * n + 10)) break;
                const size_t a = rec.size() - 16 - 4 * n;
                if (a > pivotsEnd && Peek<uint32_t>(rec, a) == uint32_t(n) && Peek<uint32_t>(rec, a - 4) == 0 &&
                    Peek<uint32_t>(rec, a - 10) == 0)
                {
                    if (found >= 0) return false;
                    found = n;
                }
            }
            if (found < 0) return false;
            tagsAt = rec.size() - 16 - 4 * found;
            elementsEnd = tagsAt - 10;
            tags.clear();
            for (int i = 0; i < found; ++i) tags.push_back(Peek<uint32_t>(rec, tagsAt + 4 + 4 * i));
            return true;
        }

        // The tags of Broom_Ride, and the catch tag Broom_Ride_Catch gets beside them.
        constexpr uint32_t kRideTags[3] = { 0x01878589, 0xFA8FE3E5, 0xE325C262 };
        constexpr uint32_t kCatchTag = 0x29646F2B;
        constexpr uint32_t kRideKeys[2] = { 0x262C63D3, 0xA272684B };

        // Broom_Ride_Catch is Broom_Ride with Horse_Ride_Catch's prompt, condition sets and (beside its own three)
        // catch tag, and with escort mount keys at the end of its pivots in place of the broom mount's: the same
        // climb on with the carry events, which keep the captive on his shoulder. The broom mount has none, and the
        // game let go of the captive as he got on (1.0.5-test2). On 3392 the two rows' heads differ only in the
        // prompt and _showUIAtPivotSocket, which stays Broom_Ride's 1.
        bool CatchRide(tablefile::Table& t, const std::string& broom, const Walked& bw, std::string& why)
        {
            if (t.Record(kBroomCatchRide))
            {
                why = "interactioninfo already has key 1900068";
                return false;
            }
            const std::string* horseRow = t.Record(kHorseCatchRide);
            Walked hw;
            if (!horseRow || !WalkInteraction(*horseRow, hw, why) || Text(*horseRow, hw.At("name")) != "Horse_Ride_Catch" ||
                hw.pivots.size() != 2 || Peek<uint32_t>(*horseRow, hw.end) != 1)
            {
                why = "interactioninfo 397 is not Horse_Ride_Catch with two pivots and one set of conditions";
                return false;
            }
            const std::string horse = *horseRow;   // a copy: Append can move the table's records
            size_t bEnd, bTags, hEnd, hTags;
            std::vector<uint32_t> bTagList, hTagList;
            if (!Suffix(broom, bw.end, bEnd, bTags, bTagList) || !Suffix(horse, hw.end, hEnd, hTags, hTagList) ||
                !std::equal(bTagList.begin(), bTagList.end(), std::begin(kRideTags)) || bTagList.size() != 3)
            {
                why = "a ride row's conditions and tags are not where they were";
                return false;
            }

            const Span bName = bw.At("interactionName"), hName = hw.At("interactionName");
            const size_t headFrom = 4 + Text(broom, bw.At("name")).size() + 4;   // after the key
            std::string rec = NameBlock(kBroomCatchName) + Bytes<uint32_t>(kBroomCatchRide) +
                              broom.substr(headFrom, bName.at - headFrom) + horse.substr(hName.at, hName.size) +
                              broom.substr(bName.at + bName.size, bw.pivots[0].start - (bName.at + bName.size));
            const uint32_t escort[2] = { kEscortRight, kEscortLeft };
            for (int i = 0; i < 2; ++i)
            {
                std::string pivot = broom.substr(bw.pivots[i].body, bw.pivots[i].end - bw.pivots[i].body);
                Poke<uint32_t>(pivot, pivot.size() - kPivotKeyFromEnd, escort[i]);
                rec += NameBlock(i ? "TempPivotKey_Broom_Ride_Catch_2" : "TempPivotKey_Broom_Ride_Catch_1") + pivot;
            }
            rec += Bytes<uint32_t>(1) + horse.substr(hw.end + 4, hEnd - (hw.end + 4));
            rec += broom.substr(bEnd, bTags - bEnd);
            rec += Bytes<uint32_t>(4) + Bytes<uint32_t>(kRideTags[0]) + Bytes<uint32_t>(kRideTags[1]) +
                   Bytes<uint32_t>(kCatchTag) + Bytes<uint32_t>(kRideTags[2]);
            rec += broom.substr(bTags + 4 + 12);
            if (!t.Append(kBroomCatchRide, rec))
            {
                why = "Broom_Ride_Catch could not be added to interactioninfo";
                return false;
            }
            return true;
        }

        // Horse_Carry_Dead with its key and three names changed, its pivots widened to Broom_Ride's reach and
        // _showWhenTargeted off, everything else the horse's (1.0.5-test19; pivots test22, _showWhenTargeted
        // test24). The horse's load shows only in focus: the player holds focus (LT or L1), aims the dot at the
        // back of the saddle and presses the action button (itemlevel.net's guide; Seth has loaded tied bounties
        // onto his horse). "Horse" and "Broom" are the same length, so the record keeps its size.
        bool CarryRow(tablefile::Table& t, std::string& why)
        {
            if (t.Record(kBroomCarry))
            {
                why = "interactioninfo already has key 1900069";
                return false;
            }
            const std::string* carry = t.Record(kHorseCarry);
            const std::string from = "Horse_Carry_Dead", to = kBroomCarryName;
            Walked w;
            if (!carry || !WalkInteraction(*carry, w, why) || Text(*carry, w.At("name")) != from ||
                Get<uint32_t>(*carry, w, "key") != kHorseCarry || w.pivots.size() != 2 || Count(*carry, from) != 3 ||
                static_cast<uint8_t>((*carry)[w.At("flags").at + kShowWhenTargetedIndex]) != 1)
            {
                why = "interactioninfo 301 is not Horse_Carry_Dead with two pivots, its name three times and "
                      "_showWhenTargeted 1";
                return false;
            }
            static_assert(sizeof "Horse_Carry_Dead" == sizeof "Broom_Carry_Dead", "the record keeps its size");
            std::string rec = *carry;
            Poke<uint32_t>(rec, w.At("key").at, kBroomCarry);
            Poke<uint8_t>(rec, w.At("flags").at + kShowWhenTargetedIndex, 0);
            uint32_t keys = 0;
            for (int i = 0; i < 2; ++i)
            {
                const size_t body = w.pivots[i].body;
                const float out = Peek<float>(*carry, body + kPivotOutAt);
                const bool right = i == 0;
                if (Peek<uint32_t>(*carry, body) != 0 || out != (right ? kHorseOut : -kHorseOut) ||
                    Peek<float>(*carry, body + kPivotAlongAt) != kHorseAlong ||
                    Peek<float>(*carry, body + kPivotReachAt) != kHorseReach ||
                    Peek<float>(*carry, body + kPivotUpAt) != kHorseUp || Peek<float>(*carry, body + kPivotDownAt) != kDown ||
                    Peek<float>(*carry, body + kPivotFacingAt) != -kHalfTurn ||
                    Peek<float>(*carry, body + kPivotFacingAt + 4) != kHalfTurn)
                {
                    why = "Horse_Carry_Dead's pivots are not boneless, 0.7 m out and 1.25 m along, within 1.5 m and 2 m "
                          "up, facing within 90 degrees";
                    return false;
                }
                keys ^= Peek<uint32_t>(*carry, w.pivots[i].end - kPivotKeyFromEnd);
                Poke<float>(rec, body + kPivotOutAt, right ? kBroomOut : -kBroomOut);
                Poke<float>(rec, body + kPivotAlongAt, kBroomAlong);
                Poke<float>(rec, body + kPivotReachAt, kBroomReach);
                Poke<float>(rec, body + kPivotUpAt, kBroomUp);
                Poke<float>(rec, body + kPivotFacingAt, -kFullTurn);
                Poke<float>(rec, body + kPivotFacingAt + 4, kFullTurn);
            }
            if (keys != (kLoadRight ^ kLoadLeft))
            {
                why = "Horse_Carry_Dead's pivots do not end with the load keys FDF23C06 and 2820B750";
                return false;
            }
            for (size_t at = rec.find(from); at != std::string::npos; at = rec.find(from, at + to.size()))
                rec.replace(at, to.size(), to);
            if (!t.Append(kBroomCarry, rec))
            {
                why = "Broom_Carry_Dead could not be added to interactioninfo";
                return false;
            }
            return true;
        }

        bool OnHorseOrBroomy(tablefile::Table& t, std::string& why)
        {
            const auto onType = [](uint32_t type)
            { return std::string(kCheckRidingVehicleType, 3) + Bytes<uint32_t>(type) + std::string(2, '\0'); };
            const std::string horse = onType(kHorseType), either = std::string(1, '\0') + horse + onType(kIbexType);
            for (uint32_t key : kOnHorseRows)
            {
                const std::string* src = t.Record(key);
                std::string rec = src ? *src : std::string();
                if (!src || !Splice(rec, horse, either, "CheckRidingVehicleType(horse)", why) || !t.Set(key, rec))
                {
                    why = "interactioninfo " + std::to_string(key) + ": " + (src ? why : "no such row");
                    return false;
                }
            }
            return true;
        }

        // Broom_Ride's two pivots are name blocks "TempPivotKey_Broom_Ride_1"
        // and "_2", each followed by the same length of data: the seat bone,
        // the position and turn, and last the key of the mount action. The
        // mount's riding chart (the Wyvern's) has one mount action; Broomy's
        // has it under both keys (broomchart.cpp and rightmountpatches.h),
        // and Broomy enters its riding chart only from a pivot whose key it
        // has. Both stay as shipped; this only checks they still end with the
        // keys the chart is given.
        bool Interaction(tablefile::Table& t, std::string& report, std::string& why)
        {
            const std::string* src = t.Record(kWyvernRide[0]);
            Walked w;
            if (!src || !WalkInteraction(*src, w, why) || Text(*src, w.At("name")) != "Broom_Ride" || w.pivots.size() != 2 ||
                PivotName(*src, w.pivots[0]) != "TempPivotKey_Broom_Ride_1" ||
                PivotName(*src, w.pivots[1]) != "TempPivotKey_Broom_Ride_2" || Peek<uint32_t>(*src, w.end) != 1)
            {
                why = "interactioninfo 1000068 is not Broom_Ride with its two pivots in order";
                return false;
            }
            for (int i = 0; i < 2; ++i)
                if (Peek<uint32_t>(*src, w.pivots[i].end - kPivotKeyFromEnd) != kRideKeys[i])
                {
                    why = "Broom_Ride's pivots do not end with their mount action keys where expected";
                    return false;
                }
            // A copy: each Append can move the table's records.
            const std::string broom = *src;
            if (!CatchRide(t, broom, w, why) || (g_offerLoad && (!CarryRow(t, why) || !OnHorseOrBroomy(t, why)))) return false;
            report = "interactioninfo: Broom_Ride keeps both pivots, mount keys 262C63D3 and A272684B; "
                     "Broom_Ride_Catch (1900068) added for mounting with a captive, mount keys 33F65149 and 4DC25870";
            if (g_offerLoad)
                report += "; Broom_Carry_Dead (1900069) added for loading one onto Broomy, Horse_Carry_Dead renamed, "
                          "keys FDF23C06 and 2820B750, Ride's reach and no focus; Horse_UnCarry_Dead, "
                          "NpcBind_Release and NpcBind_Down_Release take Broomy as a horse";
            return true;
        }

        // ---- the rest of the tables ------------------------------------------

        bool Vehicle(tablefile::Table& t, std::string& report, std::string& why)
        {
            const std::string* src = t.Record(kIbexVehicle);
            Walked w;
            if (!src || !Walk(*src, kVehicleDefs, std::size(kVehicleDefs), w, why, true) ||
                Text(*src, w.At("name")) != "AlpineIbex" || Get<uint16_t>(*src, w, "key") != kIbexVehicle)
            {
                why = "vehicleinfo 16993 is not AlpineIbex";
                return false;
            }
            if (Get<uint32_t>(*src, w, "uiMapTextureInfo") != kIbexMapTexture)
            {
                why = "the ibex's vehicle row does not name its map icon";
                return false;
            }
            std::vector<Edit> edits = { { "name", NameBlock(kVehicleName) },
                                        { "key", Bytes<uint16_t>(kVehicleKey) },
                                        // Broomy's map row, not the ibex's.
                                        { "uiMapTextureInfo", Bytes<uint32_t>(kBroomyKey) } };
            if (g_offerLoad)
            {
                // Seat 1 takes B_Rider_02, the broom's second seat, as a cargo seat.
                const Span seats = w.At("seats"), cargo = w.At("cargoSeatIndexList");
                const size_t seat0 = seats.at + 4, seat1 = seats.at + kSeatSize + 4;
                if (Get<uint32_t>(*src, w, "vehicleTypeNameHash") != kIbexType || Get<uint8_t>(*src, w, "maxVehicleSeat") != 1 ||
                    Peek<uint32_t>(*src, seat0) != Key("B_Rider_01") || Peek<uint32_t>(*src, seat1) != kNoBone ||
                    cargo.size != 4 || Peek<uint32_t>(*src, cargo.at) != 0)
                {
                    why = "the ibex's vehicle row is not of the ibex's type, one seat on B_Rider_01 and an empty cargo list";
                    return false;
                }
                std::string seatBytes = src->substr(seats.at, seats.size);
                Poke<uint32_t>(seatBytes, kSeatSize + 4, Key("B_Rider_02"));
                edits.push_back({ "maxVehicleSeat", Bytes<uint8_t>(2) });
                edits.push_back({ "seats", seatBytes });
                edits.push_back({ "cargoSeatIndexList", Bytes<uint32_t>(1) + static_cast<char>(kCargoSeat) });
            }
            std::string rec;
            if (!Rewrite(*src, w, edits, rec, why)) return false;
            if (!t.Append(kVehicleKey, rec))
            {
                why = "vehicleinfo already has key 20000";
                return false;
            }
            report = g_offerLoad ? "vehicleinfo: 20000 Broom, from AlpineIbex, with the horse's cargo seat (seat 1, B_Rider_02)"
                                 : "vehicleinfo: 20000 Broom, from AlpineIbex";
            return true;
        }

        bool AppearanceIndex(tablefile::Table& t, std::string& report, std::string& why)
        {
            const std::string* src = t.Record(kIbexKey, -2);
            Walked w;
            if (!src || !Walk(*src, kAppearanceDefs, std::size(kAppearanceDefs), w, why, true) ||
                Get<uint64_t>(*src, w, "key") != ((uint64_t(0xFFFFFFFEu) << 32) | kIbexKey) ||
                Get<uint32_t>(*src, w, "characterInfo") != kIbexKey ||
                Get<uint32_t>(*src, w, "appearanceName") != Key(kIbexAppearance))
            {
                why = "the ibex's appearance index row is not the shape naming its appearance";
                return false;
            }
            std::string rec;
            if (!Rewrite(*src, w, { { "key", Bytes<uint32_t>(kBroomyKey) + Bytes<int32_t>(-2) },
                                    { "appearanceName", Bytes<uint32_t>(Key(kAppearancePath)) },
                                    { "characterScale", Bytes<float>(1.0f) },
                                    { "characterInfo", Bytes<uint32_t>(kBroomyKey) } }, rec, why))
                return false;
            if (!t.Append(kBroomyKey, rec, -2))
            {
                why = "the appearance index already has (1900001, -2)";
                return false;
            }
            report = "characterappearanceindexinfo: (1900001, -2), the broom's appearance at scale 1";
            return true;
        }

        bool MapTexture(tablefile::Table& t, std::string& report, std::string& why)
        {
            const std::string* src = t.Record(kIbexMapTexture);
            Walked w;
            if (!src || !Walk(*src, kMapDefs, std::size(kMapDefs), w, why, true) ||
                Text(*src, w.At("name")) != kIbexMapName || Get<uint32_t>(*src, w, "key") != kIbexMapTexture ||
                Get<uint32_t>(*src, w, "uiTemplateName") != Key(kIbexMapComponent))
            {
                why = "uimaptextureinfo 5347 is not Actor_Vehicle_AlpineIbex_Hired";
                return false;
            }
            // Two localized strings: type 0x19, and the digits of (5347 << 32) | sub.
            uint32_t sub[2] = {};
            uint8_t type[2] = {};
            if (!IsLocalizedOf(*src, w.At("filterGroupName"), kIbexMapTexture, sub[0], type[0]) ||
                !IsLocalizedOf(*src, w.At("tooltipText"), kIbexMapTexture, sub[1], type[1]))
            {
                why = "the ibex's map row does not carry its key where it was";
                return false;
            }
            std::string rec;
            if (!Rewrite(*src, w, { { "name", NameBlock(kMapName) },
                                    { "key", Bytes<uint32_t>(kBroomyKey) },
                                    { "uiTemplateName", Bytes<uint32_t>(Key(kMapComponent)) },
                                    { "filterGroupName", Localized(type[0], kBroomyKey, sub[0]) },
                                    { "tooltipText", Localized(type[1], kBroomyKey, sub[1]) } }, rec, why))
                return false;
            if (Count(rec, Bytes<uint32_t>(kIbexMapTexture)) != 0)
            {
                why = "the ibex's map row still carries its key";
                return false;
            }
            if (!t.Append(kBroomyKey, rec))
            {
                why = "uimaptextureinfo already has key 1900001";
                return false;
            }
            report = "uimaptextureinfo: 1900001 Actor_Vehicle_Broomy_Hired, from the ibex's";
            return true;
        }

        // The game checks at boot that every uimaptextureinfo row is in a
        // filter group, and stops loading when one is not (launcher log:
        // "checkValid ... UIFilterGroup", 4 October). The ibex's is in
        // Group_Invisible (1000015), in the list of one filter data element:
        // entries of a u32 map key, a u32 and a byte. Broomy's goes in after
        // the ibex's.
        bool FilterGroup(tablefile::Table& t, std::string& report, std::string& why)
        {
            constexpr uint32_t kGroup = 1000015;
            const std::string* src = t.Record(kGroup);
            Walked w;
            if (!src || !Walk(*src, kFilterDefs, std::size(kFilterDefs), w, why, true) ||
                Text(*src, w.At("name")) != "Group_Invisible" || Get<uint32_t>(*src, w, "key") != kGroup)
            {
                why = "uifiltergroupinfo 1000015 is not Group_Invisible";
                return false;
            }
            // Walk the list again for the position of the ibex's entry and the count it belongs to.
            size_t p = w.At("uiFilterDataList").at + 4, countAt = kNone, ibexEnd = kNone;
            const uint32_t elements = Peek<uint32_t>(*src, p - 4);
            for (uint32_t i = 0; i < elements; ++i)
            {
                const uint32_t entries = Peek<uint32_t>(*src, p);
                for (uint32_t j = 0; j < entries; ++j)
                {
                    const size_t at = p + 4 + size_t(j) * 9;
                    const uint32_t key = Peek<uint32_t>(*src, at);
                    if (key == kBroomyKey)
                    {
                        why = "Group_Invisible already lists 1900001";
                        return false;
                    }
                    if (key == kIbexMapTexture)
                    {
                        if (ibexEnd != kNone)
                        {
                            why = "Group_Invisible lists the ibex's map row twice";
                            return false;
                        }
                        countAt = p;
                        ibexEnd = at + 9;
                    }
                }
                p += 4 + size_t(entries) * 9 + 8;
                if (!SkipLoc(*src, p)) return false;
                p += 1;
            }
            if (ibexEnd == kNone)
            {
                why = "Group_Invisible does not list the ibex's map row";
                return false;
            }
            std::string rec = *src;
            rec.insert(ibexEnd, Bytes<uint32_t>(kBroomyKey) + std::string(5, '\0'));
            Poke<uint32_t>(rec, countAt, Peek<uint32_t>(*src, countAt) + 1);
            t.Set(kGroup, rec);
            report = "uifiltergroupinfo: Group_Invisible lists Broomy's map row next to the ibex's";
            return true;
        }

        // Every occurrence of `from` in `s` replaced by `to`.
        std::string Replaced(std::string s, const std::string& from, const std::string& to)
        {
            for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
                s.replace(at, from.size(), to);
            return s;
        }
    }

    void UseOwnCharts(bool own) { g_ownCharts = own; }
    void OfferLoad(bool on) { g_offerLoad = on; }

    bool BuildTable(const char* name, const std::string& header, const std::string& body, std::string& outHeader,
                    std::string& outBody, std::string& report, std::string& why, CharacterFacts* facts,
                    bool interactionsServed)
    {
        tablefile::Table t;
        std::string parse;
        if (!t.Parse(header, body, parse))
        {
            why = std::string("the shipped table does not parse: ") + parse;
            return false;
        }
        bool ok = false;
        if (!strcmp(name, "stringinfo")) ok = Strings(t, report, why);
        else if (!strcmp(name, "characterinfo")) ok = Character(t, report, why, facts, interactionsServed);
        else if (!strcmp(name, "mercenaryinfo")) ok = Mercenary(t, report, why, facts);
        else if (!strcmp(name, "mercenarygroupinfo")) ok = Group(t, report, why);
        else if (!strcmp(name, "reserveslot")) ok = Slot(t, report, why);
        else if (!strcmp(name, "vehicleinfo")) ok = Vehicle(t, report, why);
        else if (!strcmp(name, "characterappearanceindexinfo")) ok = AppearanceIndex(t, report, why);
        else if (!strcmp(name, "interactioninfo")) ok = Interaction(t, report, why);
        else if (!strcmp(name, "uimaptextureinfo")) ok = MapTexture(t, report, why);
        else if (!strcmp(name, "uifiltergroupinfo")) ok = FilterGroup(t, report, why);
        else why = "not a table Broomy changes";
        return ok && t.Build(outHeader, outBody, why);
    }

    // "paloc", four zero bytes, u32 packed size at +9, u32 plain size at
    // +13, zeros to +0x200, then one LZ4 block. The plain list is entries of
    // u64 3, u32 length and the key's decimal digits, u32 length and the
    // text, then a u32 entry count. A character's name is keyed
    // (characterKey << 32) | 0x30.
    bool BuildPaloc(const std::string& game, std::string& out, std::string& why)
    {
        constexpr size_t kWrap = 0x200;
        if (game.size() < kWrap || game.compare(0, 5, "paloc") != 0)
        {
            why = "character.paloc is not wrapped the way this reader expects";
            return false;
        }
        const uint32_t packed = Peek<uint32_t>(game, 9), plainSize = Peek<uint32_t>(game, 13);
        if (game.size() != kWrap + packed)
        {
            why = "character.paloc's packed size does not match the file";
            return false;
        }
        std::string plain;
        if (!lz4::Decompress(reinterpret_cast<const uint8_t*>(game.data()) + kWrap, packed, plain, plainSize))
        {
            why = "character.paloc's block does not decompress";
            return false;
        }
        // Walk the entries: the count must come out right, and Broomy must
        // not be in it yet.
        char key[32];
        snprintf(key, sizeof key, "%llu", (static_cast<unsigned long long>(kBroomyKey) << 32) | kNameSub);
        size_t at = 0;
        uint32_t entries = 0;
        uint64_t lastKind = 3;
        while (plain.size() - at > 4)
        {
            if (plain.size() - at < 16)
            {
                why = "character.paloc's list ends inside an entry";
                return false;
            }
            lastKind = Peek<uint64_t>(plain, at);
            const uint32_t klen = Peek<uint32_t>(plain, at + 8);
            if (klen > plain.size() - at - 12)
            {
                why = "character.paloc's list ends inside a key";
                return false;
            }
            const bool ours = klen == strlen(key) && plain.compare(at + 12, klen, key) == 0;
            at += 12 + klen;
            if (plain.size() - at < 4)
            {
                why = "character.paloc's list ends inside an entry";
                return false;
            }
            const uint32_t vlen = Peek<uint32_t>(plain, at);
            if (vlen > plain.size() - at - 4)
            {
                why = "character.paloc's list ends inside a text";
                return false;
            }
            at += 4 + vlen;
            ++entries;
            if (ours)
            {
                why = "character.paloc already names Broomy";
                return false;
            }
        }
        if (plain.size() - at != 4 || Peek<uint32_t>(plain, at) != entries)
        {
            why = "character.paloc's entry count does not match its entries";
            return false;
        }
        std::string entry = Bytes<uint64_t>(lastKind) + Bytes<uint32_t>(static_cast<uint32_t>(strlen(key))) + key +
                            Bytes<uint32_t>(static_cast<uint32_t>(strlen(kDisplayName))) + kDisplayName;
        plain.replace(at, 4, entry + Bytes<uint32_t>(entries + 1));

        const std::string block = lz4::StoreLiterals(plain);
        out.assign(game, 0, kWrap);
        Poke<uint32_t>(out, 9, static_cast<uint32_t>(block.size()));
        Poke<uint32_t>(out, 13, static_cast<uint32_t>(plain.size()));
        out += block;
        return true;
    }

    bool BuildPortraits(const std::string& game, std::string& out, std::string& why)
    {
        if (game.find(std::string("Name=\"") + kPortrait + "\"") != std::string::npos)
        {
            why = "the portrait list already names Broomy's portrait";
            return false;
        }
        if (game.find("<Texture ") == std::string::npos)
        {
            why = "the portrait list has no Texture entries";
            return false;
        }
        size_t end = game.size();
        while (end && game[end - 1] == '\n') --end;
        out.assign(game, 0, end);
        out += std::string("\n<Texture Name=\"") + kPortrait + "\" Filename=\"" + kPortraitFile +
               "\" Type=\"Image\" GetRect=\"0,0,256,256\"/>\n";
        return true;
    }

    bool BuildMapIcons(bool markup, const std::string& game, std::string& out, std::string& why)
    {
        if (game.find("Broomy") != std::string::npos)
        {
            why = "it already names Broomy";
            return false;
        }
        if (markup)
        {
            // The ibex's component, copied after itself as Broomy's.
            const size_t start = game.find(std::string("<component name=\"") + kIbexMapComponent + "\"");
            const size_t close = start == std::string::npos ? start : game.find("</component>", start);
            if (close == std::string::npos)
            {
                why = "it has no component for the hired ibex";
                return false;
            }
            const size_t end = close + strlen("</component>");
            out.assign(game, 0, end);
            out += "\n\t" + Replaced(game.substr(start, end - start), "AlpineIbex", "Broomy");
            out.append(game, end, std::string::npos);
            return true;
        }
        // Each line of the ibex's classes, copied after itself as Broomy's,
        // drawn with the broom.
        size_t copied = 0, from = 0;
        out.clear();
        while (from < game.size())
        {
            size_t eol = game.find('\n', from);
            eol = eol == std::string::npos ? game.size() : eol + 1;
            const std::string line = game.substr(from, eol - from);
            out += line;
            if (line.find("-AlpineIbex") != std::string::npos)
            {
                std::string ours = Replaced(Replaced(line, kIbexMapImage, kMapImage), "AlpineIbex", "Broomy");
                if (ours.empty() || ours.back() != '\n') ours += '\n';
                out += ours;
                ++copied;
            }
            from = eol;
        }
        if (!copied || out.find(kMapImage) == std::string::npos)
        {
            why = "it has no class drawing the ibex";
            return false;
        }
        return true;
    }

    namespace
    {
        // A u32-length string of printable bytes at `p`; its text and end.
        bool StringAt(const std::string& d, size_t p, std::string& text, size_t& end)
        {
            if (p + 4 > d.size()) return false;
            const uint32_t n = Peek<uint32_t>(d, p);
            if (n > 4096 || p + 4 + n > d.size()) return false;
            for (size_t i = p + 4; i < p + 4 + n; ++i)
                if (static_cast<uint8_t>(d[i]) < 0x20 || static_cast<uint8_t>(d[i]) >= 0x7F) return false;
            text.assign(d, p + 4, n);
            end = p + 4 + n;
            return true;
        }

        // The end of the compiled component header at `p`: a name starting
        // MapIcon_, its classes, its script and a byte, its attribute pairs.
        bool HeaderEnd(const std::string& d, size_t p, size_t& end)
        {
            std::string s;
            if (!StringAt(d, p, s, p) || s.compare(0, 8, "MapIcon_") != 0) return false;
            for (int lists : { 1, 0, 2 })
            {
                if (lists == 0)
                {
                    if (!StringAt(d, p, s, p) || p + 1 > d.size()) return false;
                    p += 1;
                    continue;
                }
                if (p + 4 > d.size()) return false;
                const uint32_t n = Peek<uint32_t>(d, p);
                if (n > 64) return false;
                p += 4;
                for (uint32_t i = 0; i < n * lists; ++i)
                    if (!StringAt(d, p, s, p)) return false;
            }
            end = p;
            return true;
        }
    }

    bool BuildMapBinary(const std::string& game, std::string& out, std::string& why)
    {
        if (game.find("Broomy") != std::string::npos)
        {
            why = "it already names Broomy";
            return false;
        }
        // u8 0, the style sheets' paths, u32 0, the component count
        size_t p = 1;
        std::string s;
        if (game.size() < 5)
        {
            why = "it is too short";
            return false;
        }
        const uint32_t sheets = Peek<uint32_t>(game, p);
        p += 4;
        for (uint32_t i = 0; i < sheets; ++i)
            if (sheets > 64 || !StringAt(game, p, s, p))
            {
                why = "its style sheet list does not read";
                return false;
            }
        const size_t countAt = p + 4;
        if (countAt + 4 > game.size() || Peek<uint32_t>(game, p) != 0)
        {
            why = "it has no component count where it was";
            return false;
        }
        const std::string needle = Bytes<uint32_t>(static_cast<uint32_t>(strlen(kIbexMapComponent))) + kIbexMapComponent;
        size_t nameAt;
        if (!Once(game, needle, "the hired ibex's component", nameAt, why)) return false;
        size_t end = 0, start = 0;
        if (!HeaderEnd(game, nameAt, end))
        {
            why = "the hired ibex's component header does not read";
            return false;
        }
        // Children come before a component's header, so the ibex's record
        // starts where the header before it ends.
        bool found = false;
        for (size_t q = nameAt; !found && q > countAt + 4; --q)
        {
            size_t e;
            if (HeaderEnd(game, q - 1, e) && e <= nameAt) start = e, found = true;
        }
        if (!found)
        {
            why = "no component comes before the hired ibex's";
            return false;
        }
        const std::string rec = game.substr(start, end - start);
        // Every string naming AlpineIbex, renamed with its length.
        std::string ours;
        size_t from = 0;
        int renamed = 0;
        for (size_t at = rec.find("AlpineIbex"); at != std::string::npos; at = rec.find("AlpineIbex", from))
        {
            bool inString = false;
            size_t q = at >= 4 ? at - 4 : 0, strEnd = 0;
            for (int back = 0; back < 256 && q >= from; ++back, --q)
            {
                if (StringAt(rec, q, s, strEnd) && q + 4 <= at && strEnd >= at + 10)
                {
                    inString = true;
                    break;
                }
                if (q == 0) break;
            }
            if (!inString)
            {
                why = "the hired ibex's record names AlpineIbex outside a string";
                return false;
            }
            const std::string renamedText = Replaced(s, "AlpineIbex", "Broomy");
            ours.append(rec, from, q - from);
            ours += Bytes<uint32_t>(static_cast<uint32_t>(renamedText.size())) + renamedText;
            from = strEnd;
            ++renamed;
        }
        ours.append(rec, from, std::string::npos);
        if (renamed < 2)
        {
            why = "the hired ibex's record names AlpineIbex in fewer than two strings";
            return false;
        }
        out.assign(game, 0, end);
        out += ours;
        out.append(game, end, std::string::npos);
        Poke<uint32_t>(out, countAt, Peek<uint32_t>(game, countAt) + 1);
        return true;
    }

    bool BuildPoseModifiers(const std::string& game, std::string& out, std::string& why)
    {
        constexpr const char* kList = "<PoseModifierDataList Type=\"AimIK\">";
        constexpr const char* kSkeleton = "cd_r0032_00_broom.pab";
        if (game.find(kSkeleton) != std::string::npos)
        {
            why = "the pose modifier list already names the broom's skeleton";
            return false;
        }
        const size_t at = game.find(kList);
        if (at == std::string::npos || game.find(kList, at + 1) != std::string::npos)
        {
            why = "the pose modifier list does not have exactly one aim IK list";
            return false;
        }
        const size_t after = at + strlen(kList);
        out.assign(game, 0, after);
        out += std::string("\n    <PoseModifierData>\n\t\t<KeyList>\n\t\t\t<KeyName>") + kSkeleton +
               "</KeyName>\n\t\t</KeyList>\n\t\t<DefaultData>\n\t\t\t<AimIK MaxRotationAngle=\"90\">\n"
               "\t\t\t\t<AimBone AlignWeight=\"1.0\">B_Body_00</AimBone>\n\t\t\t</AimIK>\n\t\t</DefaultData>\n"
               "    </PoseModifierData>\n";
        out.append(game, after, std::string::npos);
        return true;
    }

    // The Wyvern's description with the parts that fit only the Wyvern cut:
    // under it, quadruped foot IK looked for feet the broom does not have and
    // pulled it half under the ground whenever it moved on the ground.
    bool BuildDescription(const std::string& wyvern, std::string& out, std::string& why)
    {
        struct Cut { const char* open; const char* close; };
        const Cut cuts[] = { { "<PoseModifierDesc>", "</PoseModifierDesc>" },
                             { "<FreeClimbPartInfo ", "/>" },
                             { "<ClimbSocketInfo>", "</ClimbSocketInfo>" },
                             { "<HitPartGroupInfo>", "</HitPartGroupInfo>" } };
        out = wyvern;
        for (const Cut& c : cuts)
        {
            const size_t a = out.find(c.open);
            if (a == std::string::npos || out.find(c.open, a + 1) != std::string::npos)
            {
                why = std::string("the Wyvern's description does not have exactly one ") + c.open;
                return false;
            }
            size_t b = out.find(c.close, a);
            if (b == std::string::npos)
            {
                why = std::string("the Wyvern's description does not close ") + c.open;
                return false;
            }
            b += strlen(c.close);
            // and the whitespace after it, as the old build's regex took it
            while (b < out.size() && (out[b] == ' ' || out[b] == '\t' || out[b] == '\r' || out[b] == '\n')) ++b;
            out.erase(a, b - a);
        }
        if (out.find("FootIK") != std::string::npos || out.find("<PhysicsData") == std::string::npos)
        {
            why = "the broom's description did not come out as intended";
            return false;
        }
        return true;
    }

    const std::string& AppearanceText()
    {
        static const std::string text =
            "\xEF\xBB\xBF\r\n<Appearance>\r\n\t<Customization CustomizationFile=\"\" DecorationParamFile=\"\"/>\r\n"
            "\t<Nude>\r\n\t\t<Prefab Name=\"cd_r0032_00_broom_0001\"/>\r\n\t</Nude>\r\n</Appearance>";
        return text;
    }
}
