/*
 * mod-temucraft-checkpoints: group summon. When part of your group is in a dungeon or raid and you are out in the
 * world, you get the usual summon window ("<member> wants to summon you to <zone>"):
 *   - if at least Temucraft.GroupSummon.InsidePercent of the group (online members, you included) is inside, the
 *     summon takes you into their instance, at its furthest awake checkpoint (else its entrance);
 *   - otherwise, or if you can't enter (level, lockout, raid group), it takes you to the meeting stone nearest the
 *     dungeon's entrance.
 * Declining is fine: you are asked again after Temucraft.GroupSummon.Cooldown seconds, at once if the group moves to
 * another instance, or when you type .temucraft rejoin.
 */

#include "TemucraftCheckpoints.h"

#include "TemucraftModuleText.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "Config.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Group.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <mutex>
#include <unordered_map>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    // Temucraft: hooks run on several map threads at once; this lock guards the module's shared state
    std::recursive_mutex summonLock;
    struct Spot
    {
        uint32 map = 0;
        float x = 0, y = 0, z = 0, o = 0;
    };

    struct Offer
    {
        uint64 target = 0;     // (map << 32) | instance id
        time_t at = 0;
    };

    struct SummonSettings
    {
        bool enable = true;
        uint32 insidePercent = 50;
        uint32 interval = 10;
        uint32 cooldown = 300;
        bool stones = true;
        float stoneRange = 1500.0f;
        float atStoneRange = 40.0f;
    } scfg;

    std::unordered_map<uint32, Spot> stoneFor;               // dungeon map -> meeting stone outside it
    std::unordered_map<ObjectGuid, Offer> offers;
    std::unordered_map<ObjectGuid, uint32> ticks;

    void LoadSummonSettings()
    {
        scfg.enable = sConfigMgr->GetOption<bool>("Temucraft.GroupSummon.Enable", true);
        scfg.insidePercent = std::min<uint32>(sConfigMgr->GetOption<uint32>("Temucraft.GroupSummon.InsidePercent", 50), 100);
        scfg.interval = std::max<uint32>(sConfigMgr->GetOption<uint32>("Temucraft.GroupSummon.CheckInterval", 10), 1);
        scfg.cooldown = sConfigMgr->GetOption<uint32>("Temucraft.GroupSummon.Cooldown", 300);
        scfg.stones = sConfigMgr->GetOption<bool>("Temucraft.GroupSummon.MeetingStones", true);
        scfg.stoneRange = sConfigMgr->GetOption<float>("Temucraft.GroupSummon.StoneRange", 1500.0f);
        scfg.atStoneRange = sConfigMgr->GetOption<float>("Temucraft.GroupSummon.AtStoneRange", 40.0f);
    }

    // For each dungeon and raid: the meeting stone closest to an area trigger that leads into it from outside
    void BuildStones()
    {
        stoneFor.clear();
        std::unordered_map<uint32, std::vector<Spot>> stonesByMap;
        for (auto const& [guid, data] : sObjectMgr->GetAllGOData())
            if (GameObjectTemplate const* t = sObjectMgr->GetGameObjectTemplate(data.id))
                if (t->type == GAMEOBJECT_TYPE_MEETINGSTONE)
                    stonesByMap[data.mapid].push_back({ data.mapid, data.posX, data.posY, data.posZ, data.orientation });

        std::unordered_map<uint32, float> bestDist;
        if (QueryResult result = WorldDatabase.Query("SELECT `ID` FROM `areatrigger_teleport`"))
        {
            do
            {
                uint32 triggerId = result->Fetch()[0].Get<uint32>();
                AreaTriggerTeleport const* tp = sObjectMgr->GetAreaTriggerTeleport(triggerId);
                AreaTrigger const* at = sObjectMgr->GetAreaTrigger(triggerId);
                if (!tp || !at || at->map == tp->target_mapId)
                    continue;
                MapEntry const* target = sMapStore.LookupEntry(tp->target_mapId);
                if (!target || !target->IsDungeon())
                    continue;
                auto itr = stonesByMap.find(at->map);
                if (itr == stonesByMap.end())
                    continue;
                for (Spot const& stone : itr->second)
                {
                    float d = std::hypot(stone.x - at->x, stone.y - at->y);
                    if (d > scfg.stoneRange)
                        continue;
                    auto best = bestDist.find(tp->target_mapId);
                    if (best == bestDist.end() || d < best->second)
                    {
                        bestDist[tp->target_mapId] = d;
                        stoneFor[tp->target_mapId] = stone;
                    }
                }
            } while (result->NextRow());
        }
    }

    void SendSummon(Player* player, Player* summoner, Spot const& to)
    {
        player->SetSummonPoint(to.map, to.x, to.y, to.z, 0, false);
        WorldPacket data(SMSG_SUMMON_REQUEST, 8 + 4 + 4);
        data << summoner->GetGUID();
        data << uint32(sMapMgr->GetZoneId(PHASEMASK_NORMAL, to.map, to.x, to.y, to.z));
        data << uint32(MAX_PLAYER_SUMMON_DELAY * IN_MILLISECONDS);
        player->SendDirectMessage(&data);
    }

    // Offers the summon if the group is somewhere to be summoned to. force skips the cooldown.
    bool Check(Player* player, bool force)
    {
        Group* group = player->GetGroup();
        if (!group || !player->IsAlive() || player->IsInCombat() || player->IsInFlight() || player->GetMap()->Instanceable())
            return false;
        time_t now = GameTime::GetGameTime().count();
        if (player->GetSummonExpireTimer() > now)   // a summon is already waiting for an answer
            return false;

        // Where the group is: the instance holding the most members
        std::unordered_map<uint64, std::pair<uint32, Player*>> inside;
        uint32 online = 0;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || !member->IsInWorld())
                continue;
            ++online;
            if (member == player || !member->GetMap()->IsDungeon())
                continue;
            auto& entry = inside[(uint64(member->GetMapId()) << 32) | member->GetInstanceId()];
            ++entry.first;
            entry.second = member;
        }
        uint64 target = 0;
        uint32 count = 0;
        Player* summoner = nullptr;
        for (auto const& [key, value] : inside)
            if (value.first > count)
            {
                target = key;
                count = value.first;
                summoner = value.second;
            }
        if (!summoner)
            return false;

        Offer& offer = offers[player->GetGUID()];
        if (!force && offer.target == target && now - offer.at < time_t(scfg.cooldown))
            return false;

        uint32 mapId = uint32(target >> 32);
        uint32 instanceId = uint32(target & 0xFFFFFFFF);
        MapEntry const* mapEntry = sMapStore.LookupEntry(mapId);
        std::string mapName = mapEntry ? mapEntry->name[LOCALE_enUS] : "the dungeon";
        offer.target = target;
        offer.at = now;

        bool most = count * 100 >= online * scfg.insidePercent;
        if (most && sMapMgr->PlayerCannotEnter(mapId, player, false) == Map::CAN_ENTER)
        {
            Spot to;
            to.map = mapId;
            Position pos;
            if (TemucraftFurthestCheckpoint(sMapMgr->FindMap(mapId, instanceId), pos))
            {
                to.x = pos.GetPositionX(); to.y = pos.GetPositionY(); to.z = pos.GetPositionZ(); to.o = pos.GetOrientation();
            }
            else if (AreaTriggerTeleport const* entrance = sObjectMgr->GetMapEntranceTrigger(mapId))
            {
                to.x = entrance->target_X; to.y = entrance->target_Y; to.z = entrance->target_Z; to.o = entrance->target_Orientation;
            }
            else
                to = { mapId, summoner->GetPositionX(), summoner->GetPositionY(), summoner->GetPositionZ(), summoner->GetOrientation() };
            SendSummon(player, summoner, to);
            ChatHandler(player->GetSession()).PSendSysMessage(TemucraftModuleText("mod-temucraft-checkpoints", player->GetSession(), 17, "|cff66ccff[Group]|r {} of your group are in {}. Accept the summon to join them."),
                                                              count, mapName);
            return true;
        }

        if (!scfg.stones)
            return false;
        auto stone = stoneFor.find(mapId);
        if (stone == stoneFor.end())
            return false;
        Spot const& to = stone->second;
        if (player->GetMapId() == to.map && player->GetExactDist(to.x, to.y, to.z) < scfg.atStoneRange)
            return false;
        SendSummon(player, summoner, to);
        ChatHandler(player->GetSession()).PSendSysMessage(TemucraftModuleText("mod-temucraft-checkpoints", player->GetSession(), 18, "|cff66ccff[Group]|r {} of your group {} in {}. Accept the summon to go to the meeting stone outside."),
                                                          count, count == 1 ? "is" : "are", mapName);
        return true;
    }
}

class TemucraftGroupSummon_World : public WorldScript
{
public:
    TemucraftGroupSummon_World() : WorldScript("TemucraftGroupSummon_World") { }

    void OnAfterConfigLoad(bool /*reload*/) override { std::lock_guard<std::recursive_mutex> guard(summonLock); LoadSummonSettings(); }

    void OnStartup() override
    {
        std::lock_guard<std::recursive_mutex> guard(summonLock);
        BuildStones();
        LOG_INFO("server.loading", ">> mod-temucraft-checkpoints: meeting stones found for {} dungeons and raids", stoneFor.size());
    }
};

class TemucraftGroupSummon_Player : public PlayerScript
{
public:
    TemucraftGroupSummon_Player() : PlayerScript("TemucraftGroupSummon_Player") { }

    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        std::lock_guard<std::recursive_mutex> guard(summonLock);
        if (!scfg.enable)
            return;
        uint32& timer = ticks[player->GetGUID()];
        timer += diff;
        if (timer < scfg.interval * IN_MILLISECONDS)
            return;
        timer = 0;
        if (player->IsInWorld())
            Check(player, false);
    }

    void OnPlayerLogout(Player* player) override
    {
        std::lock_guard<std::recursive_mutex> guard(summonLock);
        ticks.erase(player->GetGUID());
        offers.erase(player->GetGUID());
    }
};

class TemucraftGroupSummon_Commands : public CommandScript
{
public:
    TemucraftGroupSummon_Commands() : CommandScript("TemucraftGroupSummon_Commands") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable commandTable =
        {
            { "temucraft rejoin", HandleRejoin, 100250, Console::No },
        };
        return commandTable;
    }

    static bool HandleRejoin(ChatHandler* handler)
    {
        std::lock_guard<std::recursive_mutex> guard(summonLock);
        if (!scfg.enable)
            return true;
        if (!Check(handler->GetPlayer(), true))
            handler->SendSysMessage(TemucraftModuleText("mod-temucraft-checkpoints", handler, 19, "Nobody in your group is in a dungeon you can be summoned to right now (or you are in combat, dead, "
                                    "flying or in an instance)."));
        return true;
    }
};

void AddTemucraftGroupSummonScripts()
{
    new TemucraftGroupSummon_World();
    new TemucraftGroupSummon_Player();
    new TemucraftGroupSummon_Commands();
}
