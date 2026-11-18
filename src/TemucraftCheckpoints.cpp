/*
 * mod-temucraft-checkpoints: dungeon checkpoints. A checkpoint wakes up (at once, when a creature is dead, when an
 * instance script marks a boss done, or when a player walks past it) and from then on releasing your spirit in
 * that instance brings you back to life at the furthest awake checkpoint instead of the graveyard outside. The
 * checkpoint you will come back at is flagged on the minimap and world map, and an awake checkpoint can take you to
 * the other awake ones of the instance.
 *
 * Checkpoints live in the world table mod_temucraft_checkpoint. Rows without a spawn get one at startup: the server
 * picks a gameobject guid, writes the gameobject row and stores the guid back in the checkpoint row. Rows you add
 * by hand only need the map, position and how they wake up.
 *
 *   .checkpoint list            the checkpoints of the map you are on and whether they are awake in your instance
 *   .checkpoint add <unlock type> <unlock value> <name>   a checkpoint where you stand (types below)
 *   .checkpoint import          gameobjects of a checkpoint entry that have no checkpoint row get one, and the rows of
 *                             checkpoints moved with .gobject move take the gameobject's position
 *   .checkpoint wake            wakes every checkpoint of your instance (testing)
 *   .checkpoint link <id> <all|any> <links>   wakes when all (or any) of these are met (see unlock types 4 and 5)
 *   .checkpoint go <id>         teleports you to a checkpoint
 *   .checkpoint reload          reads the table again and spawns new rows
 *
 * Unlock types: 0 always awake, 1 awake once creature <value> is dead in the instance (its respawn timer runs, so
 * this survives restarts), 2 awake once the instance script's boss <value> is DONE, 3 awake once a player comes
 * within <value> yards (0: Temucraft.Checkpoints.ProximityRange). Once awake, a checkpoint stays awake until the
 * instance resets. 4 awake once every link in `linked` is met, 5 once any one is: links are creature entries (dead),
 * b<boss index> (DONE) and c<checkpoint id> (awake), e.g. "3914, 3886, b2" - set them with .checkpoint link.
 *
 * 6 awake once DungeonEncounter.dbc encounter <value> is completed (summoned bosses, bosses credited by a spell).
 * A checkpoint at 0, 0, 0 is placed where the group stands when it wakes, with a stone summoned there.
 *
 * Entering a run whose group is past the entrance takes you straight to the furthest awake checkpoint.
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
#include "InstanceScript.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    enum UnlockType : uint8
    {
        UNLOCK_ALWAYS    = 0,
        UNLOCK_CREATURE  = 1,
        UNLOCK_BOSSSTATE = 2,
        UNLOCK_NEARBY    = 3,
        UNLOCK_ALL_OF    = 4,   // every link in `linked` met
        UNLOCK_ANY_OF    = 5,   // one link in `linked` met
        UNLOCK_ENCOUNTER = 6,   // DungeonEncounter.dbc encounter <value> completed (any kind of boss)
    };

    constexpr uint32 TEXT_DORMANT = 608200;
    constexpr uint32 TEXT_AWAKE   = 608201;
    constexpr uint32 SENDER_TRAVEL = 1;

    struct Checkpoint
    {
        uint32 id = 0;
        uint32 map = 0;
        std::string name;
        uint8 unlockType = UNLOCK_ALWAYS;
        uint32 unlockValue = 0;
        uint32 sort = 0;
        float x = 0, y = 0, z = 0, o = 0;
        uint32 entry = 0;
        ObjectGuid::LowType guid = 0;
        bool enabled = true;
        std::string linked;
        std::vector<std::pair<char, uint32>> links;   // 'e' creature entry dead, 'b' boss state DONE, 'c' checkpoint awake
    };

    // "3914, 3886 b2 c17": creature entries, b<boss index>, c<checkpoint id>; commas or spaces between them
    std::vector<std::pair<char, uint32>> ParseLinks(std::string const& text)
    {
        std::vector<std::pair<char, uint32>> links;
        std::string token;
        auto flush = [&]()
        {
            if (token.empty())
                return;
            char kind = 'e';
            std::string digits = token;
            if (token[0] == 'b' || token[0] == 'B' || token[0] == 'c' || token[0] == 'C')
            {
                kind = char(std::tolower(token[0]));
                digits = token.substr(1);
            }
            if (!digits.empty() && std::all_of(digits.begin(), digits.end(), ::isdigit))
                links.emplace_back(kind, uint32(std::stoul(digits)));
            token.clear();
        };
        for (char ch : text)
        {
            if (ch == ',' || ch == ' ' || ch == ';')
                flush();
            else
                token += ch;
        }
        flush();
        return links;
    }

    struct InstanceProgress
    {
        uint32 map = 0;
        bool seeded = false;
        std::set<uint32> awake;
        std::unordered_map<uint32, Checkpoint> placed;   // checkpoints without a position, placed when they woke
    };

    // A checkpoint without a position (0, 0, 0) is placed where the group stands when it wakes
    bool IsPlacedAtWake(Checkpoint const& ws) { return ws.x == 0.0f && ws.y == 0.0f && ws.z == 0.0f; }

    std::unordered_map<ObjectGuid, uint32> summonedStones;            // gameobject -> checkpoint id

    struct Settings
    {
        bool enable = true;
        bool alwaysAwake = true;
        bool teleportOnEnter = true;
        bool resurrect = true;
        float healthPct = 100.0f;
        bool skipDuringEncounter = false;
        bool travel = true;
        bool mapIcon = true;
        uint32 icon = 7;
        bool announce = true;
        uint32 sound = 0;
        float proximity = 30.0f;
        uint32 displayDormant = 0;
        uint32 displayAwake = 0;
    } cfg;

    std::vector<Checkpoint> checkpoints;
    std::unordered_map<uint32, std::vector<size_t>> byMap;
    std::unordered_map<ObjectGuid::LowType, size_t> byGuid;
    std::unordered_set<uint32> entries;
    std::unordered_map<uint64, std::vector<ObjectGuid::LowType>> creatureSpawns;   // (map << 32 | entry) -> spawns
    std::unordered_map<uint32, InstanceProgress> progress;                          // instance id
    std::unordered_map<ObjectGuid, std::pair<uint32, uint32>> playerTick;           // timer, last instance
    std::unordered_set<ObjectGuid> justLoggedIn;

    uint64 SpawnKey(uint32 map, uint32 entry) { return (uint64(map) << 32) | entry; }

    void LoadSettings()
    {
        cfg.enable = sConfigMgr->GetOption<bool>("Temucraft.Checkpoints.Enable", true);
        cfg.alwaysAwake = sConfigMgr->GetOption<bool>("Temucraft.Checkpoints.Entrances", true);
        cfg.teleportOnEnter = sConfigMgr->GetOption<bool>("Temucraft.Checkpoints.TeleportOnEnter", true);
        cfg.resurrect = sConfigMgr->GetOption<bool>("Temucraft.Checkpoints.Resurrect", true);
        cfg.healthPct = std::clamp(sConfigMgr->GetOption<float>("Temucraft.Checkpoints.HealthPct", 100.0f), 1.0f, 100.0f);
        cfg.skipDuringEncounter = sConfigMgr->GetOption<bool>("Temucraft.Checkpoints.SkipDuringEncounter", false);
        cfg.travel = sConfigMgr->GetOption<bool>("Temucraft.Checkpoints.Travel", true);
        cfg.mapIcon = sConfigMgr->GetOption<bool>("Temucraft.Checkpoints.MapIcon", true);
        cfg.icon = sConfigMgr->GetOption<uint32>("Temucraft.Checkpoints.MapIconId", 7);
        cfg.announce = sConfigMgr->GetOption<bool>("Temucraft.Checkpoints.Announce", true);
        cfg.sound = sConfigMgr->GetOption<uint32>("Temucraft.Checkpoints.Sound", 0);
        cfg.proximity = sConfigMgr->GetOption<float>("Temucraft.Checkpoints.ProximityRange", 30.0f);
        cfg.displayDormant = sConfigMgr->GetOption<uint32>("Temucraft.Checkpoints.DisplayDormant", 0);
        cfg.displayAwake = sConfigMgr->GetOption<uint32>("Temucraft.Checkpoints.DisplayAwake", 0);
    }

    void Index()
    {
        byMap.clear();
        byGuid.clear();
        entries.clear();
        for (size_t i = 0; i < checkpoints.size(); ++i)
        {
            Checkpoint const& ws = checkpoints[i];
            entries.insert(ws.entry);
            if (ws.guid)
                byGuid[ws.guid] = i;
            if (ws.enabled)
                byMap[ws.map].push_back(i);
        }
    }

    uint8 SpawnMaskFor(uint32 mapId)
    {
        MapEntry const* map = sMapStore.LookupEntry(mapId);
        if (!map || !map->IsDungeon())
            return 1;
        uint8 mask = 0;
        for (uint8 d = 0; d < MAX_DIFFICULTY; ++d)
            if (GetMapDifficultyData(mapId, Difficulty(d)))
                mask |= 1 << d;
        return mask ? mask : 1;
    }

    // Creates the spawn of a checkpoint (memory and the gameobject table) and stores its guid in the checkpoint row.
    // When a map is given (the instance you stand in), the gameobject also appears there at once.
    bool Spawn(Checkpoint& ws, Map* here = nullptr)
    {
        if (!sObjectMgr->GetGameObjectTemplate(ws.entry) || !sMapStore.LookupEntry(ws.map))
            return false;

        ObjectGuid::LowType guid = sObjectMgr->GenerateGameObjectSpawnId();
        GameObjectData& data = sObjectMgr->NewGOData(guid);
        data.spawnId = guid;
        data.id = ws.entry;
        data.mapid = ws.map;
        data.posX = ws.x;
        data.posY = ws.y;
        data.posZ = ws.z;
        data.orientation = ws.o;
        data.rotation = G3D::Quat(0.0f, 0.0f, std::sin(ws.o / 2.0f), std::cos(ws.o / 2.0f));
        data.spawntimesecs = 0;
        data.animprogress = 100;
        data.go_state = GO_STATE_READY;
        data.spawnMask = SpawnMaskFor(ws.map);
        data.phaseMask = PHASEMASK_NORMAL;
        data.artKit = 0;
        data.dbData = true;
        data.spawnGroupId = 0;
        sObjectMgr->AddGameobjectToGrid(guid, &data);

        std::string comment = "Checkpoint " + std::to_string(ws.id) + ": " + ws.name;
        WorldDatabase.EscapeString(comment);
        WorldDatabase.Execute("INSERT INTO `gameobject` (`guid`, `id`, `map`, `spawnMask`, `phaseMask`, `position_x`, `position_y`, `position_z`, "
                              "`orientation`, `rotation0`, `rotation1`, `rotation2`, `rotation3`, `spawntimesecs`, `animprogress`, `state`, `Comment`) "
                              "VALUES ({}, {}, {}, {}, 1, {}, {}, {}, {}, 0, 0, {}, {}, 0, 100, 1, '{}')", guid, ws.entry, ws.map,
                              uint32(data.spawnMask), ws.x, ws.y, ws.z, ws.o, data.rotation.z, data.rotation.w, comment);
        WorldDatabase.Execute("UPDATE `mod_temucraft_checkpoint` SET `guid` = {} WHERE `id` = {}", guid, ws.id);
        ws.guid = guid;

        if (here && here->GetId() == ws.map)
        {
            GameObject* go = new GameObject();
            if (!go->LoadGameObjectFromDB(guid, here, true))
                delete go;
        }
        return true;
    }

    void Despawn(Checkpoint& ws)
    {
        if (!ws.guid)
            return;
        if (sObjectMgr->GetGameObjectData(ws.guid))
            sObjectMgr->DeleteGOData(ws.guid);
        WorldDatabase.Execute("DELETE FROM `gameobject` WHERE `guid` = {}", ws.guid);
        WorldDatabase.Execute("UPDATE `mod_temucraft_checkpoint` SET `guid` = 0 WHERE `id` = {}", ws.id);
        ws.guid = 0;
    }

    uint32 LoadCheckpoints()
    {
        checkpoints.clear();
        if (QueryResult result = WorldDatabase.Query("SELECT `id`, `map`, `name`, `unlock_type`, `unlock_value`, `sort`, `position_x`, `position_y`, "
                                                     "`position_z`, `orientation`, `gameobject_entry`, `guid`, `enabled`, `linked` FROM `mod_temucraft_checkpoint`"))
        {
            do
            {
                Field* f = result->Fetch();
                Checkpoint ws;
                ws.id = f[0].Get<uint32>();
                ws.map = f[1].Get<uint32>();
                ws.name = f[2].Get<std::string>();
                ws.unlockType = f[3].Get<uint8>();
                ws.unlockValue = f[4].Get<uint32>();
                ws.sort = f[5].Get<uint32>();
                ws.x = f[6].Get<float>();
                ws.y = f[7].Get<float>();
                ws.z = f[8].Get<float>();
                ws.o = f[9].Get<float>();
                ws.entry = f[10].Get<uint32>();
                ws.guid = f[11].Get<uint32>();
                ws.enabled = f[12].Get<bool>();
                ws.linked = f[13].Get<std::string>();
                ws.links = ParseLinks(ws.linked);
                checkpoints.push_back(std::move(ws));
            } while (result->NextRow());
        }

        // Spawn the rows that have none (or whose gameobject was deleted), take away the spawns of disabled rows
        uint32 spawned = 0;
        for (Checkpoint& ws : checkpoints)
        {
            bool exists = ws.guid && sObjectMgr->GetGameObjectData(ws.guid);
            if (!ws.enabled)
            {
                if (exists)
                    Despawn(ws);
                continue;
            }
            if (!exists && !IsPlacedAtWake(ws) && sMapStore.LookupEntry(ws.map) && Spawn(ws))
                ++spawned;
        }

        creatureSpawns.clear();
        std::unordered_set<uint64> wanted;
        for (Checkpoint const& ws : checkpoints)
            if (ws.enabled && ws.unlockType == UNLOCK_CREATURE)
                wanted.insert(SpawnKey(ws.map, ws.unlockValue));
        for (Checkpoint const& ws : checkpoints)
            if (ws.enabled && (ws.unlockType == UNLOCK_ALL_OF || ws.unlockType == UNLOCK_ANY_OF))
                for (auto const& [kind, value] : ws.links)
                    if (kind == 'e')
                        wanted.insert(SpawnKey(ws.map, value));
        for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
        {
            uint64 key = SpawnKey(data.mapid, data.id);
            if (wanted.count(key))
                creatureSpawns[key].push_back(spawnId);
        }

        Index();
        return spawned;
    }

    bool CreatureDead(Map* map, uint32 entry)
    {
        auto itr = creatureSpawns.find(SpawnKey(map->GetId(), entry));
        if (itr == creatureSpawns.end())
            return false;
        time_t now = GameTime::GetGameTime().count();
        for (ObjectGuid::LowType spawnId : itr->second)
        {
            if (map->GetCreatureRespawnTime(spawnId) > now)
                return true;
            auto range = map->GetCreatureBySpawnIdStore().equal_range(spawnId);
            for (auto it = range.first; it != range.second; ++it)
                if (!it->second->IsAlive())
                    return true;
        }
        return false;
    }

    struct InstanceProgress;
    InstanceProgress& ProgressOf(Map* map);

    bool BossDone(Map* map, uint32 boss)
    {
        if (InstanceMap* instance = map->ToInstanceMap())
            if (InstanceScript const* script = instance->GetInstanceScript())
                return script->GetBossState(boss) == DONE;
        return false;
    }

    bool LinkMet(Map* map, char kind, uint32 value);

    bool ConditionMet(Map* map, Checkpoint const& ws, Player const* near)
    {
        switch (ws.unlockType)
        {
            case UNLOCK_ALWAYS:
                return cfg.alwaysAwake;
            case UNLOCK_CREATURE:
                return CreatureDead(map, ws.unlockValue);
            case UNLOCK_BOSSSTATE:
                return BossDone(map, ws.unlockValue);
            case UNLOCK_ENCOUNTER:
                if (InstanceMap* instance = map->ToInstanceMap())
                    if (InstanceScript const* script = instance->GetInstanceScript())
                        return ws.unlockValue < 32 && (script->GetCompletedEncounterMask() & (1u << ws.unlockValue));
                return false;
            case UNLOCK_ALL_OF:
            case UNLOCK_ANY_OF:
            {
                if (ws.links.empty())
                    return false;
                bool any = false, all = true;
                for (auto const& [kind, value] : ws.links)
                {
                    bool met = LinkMet(map, kind, value);
                    any = any || met;
                    all = all && met;
                }
                return ws.unlockType == UNLOCK_ALL_OF ? all : any;
            }
            case UNLOCK_NEARBY:
            {
                float range = ws.unlockValue ? float(ws.unlockValue) : cfg.proximity;
                return near && near->GetExactDist(ws.x, ws.y, ws.z) <= range;
            }
        }
        return false;
    }

    InstanceProgress& ProgressOf(Map* map)
    {
        InstanceProgress& p = progress[map->GetInstanceId()];
        if (p.map != map->GetId())      // an instance id given out again after a reset
        {
            p = InstanceProgress();
            p.map = map->GetId();
        }
        return p;
    }

    bool LinkMet(Map* map, char kind, uint32 value)
    {
        switch (kind)
        {
            case 'e': return CreatureDead(map, value);
            case 'b': return BossDone(map, value);
            case 'c': return ProgressOf(map).awake.count(value) > 0;
        }
        return false;
    }

    Checkpoint const* Furthest(Map* map)
    {
        auto itr = byMap.find(map->GetId());
        if (itr == byMap.end())
            return nullptr;
        InstanceProgress& p = ProgressOf(map);
        Checkpoint const* best = nullptr;
        for (size_t i : itr->second)
        {
            Checkpoint const& ws = checkpoints[i];
            bool placedAtWake = IsPlacedAtWake(ws);
            if (!p.awake.count(ws.id) && (placedAtWake || ws.unlockType == UNLOCK_NEARBY || !ConditionMet(map, ws, nullptr)))
                continue;
            Checkpoint const* where = &ws;
            if (placedAtWake)
            {
                auto placed = p.placed.find(ws.id);
                if (placed == p.placed.end())
                    continue;
                where = &placed->second;
            }
            if (!best || ws.sort > best->sort || (ws.sort == best->sort && ws.id > best->id))
                best = where;
        }
        return best;
    }

    void SendMapIcon(Player* player, Checkpoint const& ws)
    {
        if (!cfg.mapIcon)
            return;
        std::string name = "Checkpoint: " + ws.name;
        WorldPacket data(SMSG_GOSSIP_POI, 4 + 4 + 4 + 4 + 4 + name.size() + 1);
        data << uint32(99);          // flags (as the core's points of interest send them)
        data << float(ws.x);
        data << float(ws.y);
        data << uint32(cfg.icon);
        data << uint32(0);           // importance
        data << name;
        player->SendDirectMessage(&data);
    }

    void ApplyVisual(GameObject* go, bool awake)
    {
        go->SetGoState(awake ? GO_STATE_ACTIVE : GO_STATE_READY);
        uint32 display = awake ? cfg.displayAwake : cfg.displayDormant;
        if (display)
            go->SetDisplayId(display);
    }

    void RefreshVisuals(Map* map, InstanceProgress const& p)
    {
        auto itr = byMap.find(map->GetId());
        if (itr == byMap.end())
            return;
        for (size_t i : itr->second)
        {
            Checkpoint const& ws = checkpoints[i];
            auto range = map->GetGameObjectBySpawnIdStore().equal_range(ws.guid);
            for (auto it = range.first; it != range.second; ++it)
                ApplyVisual(it->second, p.awake.count(ws.id) > 0);
        }
    }

    // Places a checkpoint without a position where a living player of the instance stands, with a stone there
    bool PlaceAtWake(Map* map, Checkpoint const& ws, Player* near, InstanceProgress& p)
    {
        Player* by = near && near->IsAlive() && near->GetMap() == map ? near : nullptr;
        if (!by)
            map->DoForAllPlayers([&](Player* player) { if (!by && player->IsAlive() && !player->IsGameMaster()) by = player; });
        if (!by)
            return false;
        Checkpoint placed = ws;
        placed.x = by->GetPositionX();
        placed.y = by->GetPositionY();
        placed.z = by->GetPositionZ();
        placed.o = by->GetOrientation();
        p.placed[ws.id] = placed;
        if (GameObject* go = by->SummonGameObject(ws.entry, placed.x, placed.y, placed.z, placed.o, 0.0f, 0.0f,
                                                  std::sin(placed.o / 2.0f), std::cos(placed.o / 2.0f), 6 * HOUR))
        {
            summonedStones[go->GetGUID()] = ws.id;
            ApplyVisual(go, true);
        }
        return true;
    }

    // Wakes the checkpoints whose condition is met. The first look at an instance (after a restart, say) wakes them
    // quietly; later ones are announced to everyone in the instance.
    void Evaluate(Map* map, Player* near)
    {
        auto itr = byMap.find(map->GetId());
        if (itr == byMap.end())
            return;
        InstanceProgress& p = ProgressOf(map);
        std::vector<Checkpoint const*> woke;
        bool changed = true;
        while (changed)     // again, for checkpoints linked to ones that just woke
        {
            changed = false;
            for (size_t i : itr->second)
            {
                Checkpoint const& ws = checkpoints[i];
                if (!p.awake.count(ws.id) && ConditionMet(map, ws, near))
                {
                    if (IsPlacedAtWake(ws) && !PlaceAtWake(map, ws, near, p))
                        continue;   // nobody alive to place it by yet
                    p.awake.insert(ws.id);
                    woke.push_back(&ws);
                    changed = true;
                }
            }
        }
        bool quiet = !p.seeded;
        p.seeded = true;
        if (woke.empty())
            return;
        RefreshVisuals(map, p);
        if (quiet)
            return;

        Checkpoint const* furthest = Furthest(map);
        map->DoForAllPlayers([&](Player* player)
        {
            for (Checkpoint const* ws : woke)
            {
                if (ws->unlockType == UNLOCK_ALWAYS)
                    continue;
                if (cfg.announce)
                {
                    player->GetSession()->SendAreaTriggerMessage(TemucraftModuleText("mod-temucraft-checkpoints", player->GetSession(), 1, "Checkpoint awakened: " ) + ws->name);
                    ChatHandler(player->GetSession()).PSendSysMessage(TemucraftModuleText("mod-temucraft-checkpoints", player->GetSession(), 2, "|cff66ccff[Checkpoint]|r {} is awake: you come back here if you fall."), ws->name);
                }
            }
            if (cfg.sound)
                player->PlayDirectSound(cfg.sound, player);
            if (furthest)
                SendMapIcon(player, *furthest);
        });
    }

    Checkpoint* FindById(uint32 id)
    {
        for (Checkpoint& ws : checkpoints)
            if (ws.id == id)
                return &ws;
        return nullptr;
    }

    Checkpoint const* FindBySpawn(ObjectGuid::LowType guid)
    {
        auto itr = byGuid.find(guid);
        return itr == byGuid.end() ? nullptr : &checkpoints[itr->second];
    }

    char const* UnlockText(Checkpoint const& ws)
    {
        switch (ws.unlockType)
        {
            case UNLOCK_ALWAYS: return "always";
            case UNLOCK_CREATURE: return "creature dead";
            case UNLOCK_BOSSSTATE: return "boss done";
            case UNLOCK_NEARBY: return "walk past";
            case UNLOCK_ALL_OF: return "all of";
            case UNLOCK_ANY_OF: return "any of";
            case UNLOCK_ENCOUNTER: return "encounter done";
        }
        return "?";
    }
}

bool TemucraftFurthestCheckpoint(Map* map, Position& out)
{
    if (!cfg.enable || !map || !map->IsDungeon())
        return false;
    Evaluate(map, nullptr);
    Checkpoint const* cp = Furthest(map);
    if (!cp)
        return false;
    out.Relocate(cp->x, cp->y, cp->z, cp->o);
    return true;
}

class TemucraftCheckpoints_World : public WorldScript
{
public:
    TemucraftCheckpoints_World() : WorldScript("TemucraftCheckpoints_World") { }

    void OnAfterConfigLoad(bool /*reload*/) override { LoadSettings(); }

    void OnStartup() override
    {
        uint32 spawned = LoadCheckpoints();
        LOG_INFO("server.loading", ">> mod-temucraft-checkpoints: {} checkpoints, {} new spawns", checkpoints.size(), spawned);
    }

    void OnUpdate(uint32 diff) override
    {
        // Forget instances that no longer exist
        _cleanup += diff;
        if (_cleanup < 5 * MINUTE * IN_MILLISECONDS)
            return;
        _cleanup = 0;
        for (auto itr = progress.begin(); itr != progress.end();)
        {
            if (!sMapMgr->FindMap(itr->second.map, itr->first))
                itr = progress.erase(itr);
            else
                ++itr;
        }
    }

private:
    uint32 _cleanup = 0;
};

class TemucraftCheckpoints_Player : public PlayerScript
{
public:
    TemucraftCheckpoints_Player() : PlayerScript("TemucraftCheckpoints_Player") { }

    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        if (!cfg.enable)
            return;
        auto& [timer, lastInstance] = playerTick[player->GetGUID()];
        timer += diff;
        if (timer < 1000)
            return;
        timer = 0;
        bool fromLogin = justLoggedIn.erase(player->GetGUID()) > 0;

        Map* map = player->GetMap();
        if (!map || !map->IsDungeon() || !player->IsInWorld() || !byMap.count(map->GetId()))
        {
            lastInstance = 0;
            return;
        }
        if (!player->IsAlive())     // a ghost walking in: wait until alive
            return;
        Evaluate(map, player);
        if (lastInstance != map->GetInstanceId())
        {
            lastInstance = map->GetInstanceId();
            if (Checkpoint const* ws = Furthest(map))
            {
                SendMapIcon(player, *ws);
                // Entering a run that has passed its entrance takes you to the furthest checkpoint (not when you log
                // in inside: you stay where you logged out)
                if (cfg.teleportOnEnter && !fromLogin && ws->unlockType != UNLOCK_ALWAYS && !player->IsInCombat() &&
                    player->GetExactDist(ws->x, ws->y, ws->z) > 30.0f)
                    player->NearTeleportTo(ws->x, ws->y, ws->z, ws->o);
                // Joining a run that already has progress: say where you'd come back
                if (ws->unlockType != UNLOCK_ALWAYS && cfg.announce)
                    ChatHandler(player->GetSession()).PSendSysMessage(TemucraftModuleText("mod-temucraft-checkpoints", player->GetSession(), 3, "|cff66ccff[Checkpoint]|r This run's furthest checkpoint is {}: "
                                                                      "you come back there if you fall."), ws->name);
            }
        }
    }

    void OnPlayerLogin(Player* player) override { justLoggedIn.insert(player->GetGUID()); }

    void OnPlayerLogout(Player* player) override
    {
        playerTick.erase(player->GetGUID());
        justLoggedIn.erase(player->GetGUID());
    }

    // Releasing your spirit in a dungeon brings you back at the furthest awake checkpoint
    bool OnPlayerCanRepopAtGraveyard(Player* player) override
    {
        if (!cfg.enable || player->IsAlive())
            return true;
        Map* map = player->GetMap();
        if (!map || !map->IsDungeon())
            return true;
        if (cfg.skipDuringEncounter)
            if (InstanceMap* instance = map->ToInstanceMap())
                if (InstanceScript const* script = instance->GetInstanceScript())
                    if (script->IsEncounterInProgress())
                        return true;
        Evaluate(map, nullptr);
        Checkpoint const* ws = Furthest(map);
        if (!ws)
            return true;

        if (cfg.resurrect)
        {
            player->ResurrectPlayer(cfg.healthPct / 100.0f);
            player->SpawnCorpseBones();
        }
        player->NearTeleportTo(ws->x, ws->y, ws->z, ws->o);
        SendMapIcon(player, *ws);
        return false;
    }
};

class TemucraftCheckpoints_GameObject : public AllGameObjectScript
{
public:
    TemucraftCheckpoints_GameObject() : AllGameObjectScript("TemucraftCheckpoints_GameObject") { }

    void OnGameObjectAddWorld(GameObject* go) override
    {
        if (!entries.count(go->GetEntry()) || !go->GetMap()->IsDungeon())
            return;
        Checkpoint const* ws = FindBySpawn(go->GetSpawnId());
        if (!ws)
            return;
        auto itr = progress.find(go->GetMap()->GetInstanceId());
        bool awake = itr != progress.end() && itr->second.map == go->GetMapId() && itr->second.awake.count(ws->id);
        ApplyVisual(go, awake);
    }

    bool CanGameObjectGossipHello(Player* player, GameObject* go) override
    {
        if (!entries.count(go->GetEntry()))
            return false;
        Checkpoint const* ws = FindBySpawn(go->GetSpawnId());
        if (!ws)
        {
            auto summoned = summonedStones.find(go->GetGUID());
            ws = summoned != summonedStones.end() ? FindById(summoned->second) : nullptr;
        }
        if (!ws || !cfg.enable)
            return false;

        Map* map = go->GetMap();
        Evaluate(map, player);
        InstanceProgress& p = ProgressOf(map);
        ClearGossipMenuFor(player);
        if (!p.awake.count(ws->id))
        {
            SendGossipMenuFor(player, TEXT_DORMANT, go->GetGUID());
            return true;
        }
        if (cfg.travel)
            for (size_t i : byMap[map->GetId()])
            {
                Checkpoint const& other = checkpoints[i];
                if (other.id != ws->id && p.awake.count(other.id))
                    AddGossipItemFor(player, GOSSIP_ICON_TAXI, TemucraftModuleText("mod-temucraft-checkpoints", player, 4, "Travel to " ) + other.name, SENDER_TRAVEL, other.id);
            }
        SendGossipMenuFor(player, TEXT_AWAKE, go->GetGUID());
        SendMapIcon(player, *ws);
        return true;
    }

    bool CanGameObjectGossipSelect(Player* player, GameObject* go, uint32 sender, uint32 action) override
    {
        if (!entries.count(go->GetEntry()) || sender != SENDER_TRAVEL)
            return false;
        CloseGossipMenuFor(player);
        Checkpoint const* target = FindById(action);
        Map* map = go->GetMap();
        if (!target || target->map != map->GetId() || !ProgressOf(map).awake.count(target->id) || player->IsInCombat())
            return true;
        auto placed = ProgressOf(map).placed.find(target->id);
        if (placed != ProgressOf(map).placed.end())
            target = &placed->second;
        player->NearTeleportTo(target->x, target->y, target->z, target->o);
        return true;
    }
};

class TemucraftCheckpoints_Commands : public CommandScript
{
public:
    TemucraftCheckpoints_Commands() : CommandScript("TemucraftCheckpoints_Commands") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable checkpointTable =
        {
            { "list",   HandleList,   100201 /* SEC_GAMEMASTER -> RBAC */,    Console::No },
            { "add",    HandleAdd,    100202 /* SEC_ADMINISTRATOR -> RBAC */, Console::No },
            { "import", HandleImport, 100203 /* SEC_ADMINISTRATOR -> RBAC */, Console::Yes },
            { "wake",   HandleWake,   100204 /* SEC_GAMEMASTER -> RBAC */,    Console::No },
            { "go",     HandleGo,     100205 /* SEC_GAMEMASTER -> RBAC */,    Console::No },
            { "reload", HandleReload, 100206 /* SEC_ADMINISTRATOR -> RBAC */, Console::Yes },
            { "link",   HandleLink,   100207 /* SEC_ADMINISTRATOR -> RBAC */, Console::Yes },
        };
        static ChatCommandTable commandTable =
        {
            { "checkpoint", checkpointTable },
        };
        return commandTable;
    }

    static bool HandleList(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        Map* map = player->GetMap();
        auto itr = byMap.find(map->GetId());
        if (itr == byMap.end())
        {
            handler->SendSysMessage(TemucraftModuleText("mod-temucraft-checkpoints", handler, 5, "No checkpoints on this map."));
            return true;
        }
        InstanceProgress& p = ProgressOf(map);
        Checkpoint const* furthest = map->IsDungeon() ? Furthest(map) : nullptr;
        for (size_t i : itr->second)
        {
            Checkpoint const& ws = checkpoints[i];
            handler->PSendSysMessage(TemucraftModuleText("mod-temucraft-checkpoints", handler, 6, "{} [{}] {} - {} {} - {}{} (guid {}, {:.0f} yd)"), ws.id, ws.sort, ws.name, UnlockText(ws), ws.unlockValue,
                                     p.awake.count(ws.id) ? "awake" : "dormant", furthest == &ws ? ", respawn here" : "", ws.guid,
                                     player->GetExactDist(ws.x, ws.y, ws.z));
        }
        return true;
    }

    static bool HandleAdd(ChatHandler* handler, uint8 unlockType, uint32 unlockValue, Tail name)
    {
        if (unlockType > UNLOCK_NEARBY)
        {
            handler->SendSysMessage(TemucraftModuleText("mod-temucraft-checkpoints", handler, 7, "Unlock type: 0 always, 1 creature entry dead, 2 boss state done, 3 walk past (value = yards)."));
            return false;
        }
        Player* player = handler->GetPlayer();
        uint32 id = 100000;
        uint32 sort = 0;
        for (Checkpoint const& ws : checkpoints)
        {
            id = std::max(id, ws.id + 1);
            if (ws.map == player->GetMapId())
                sort = std::max(sort, ws.sort + 1);
        }
        Checkpoint ws;
        ws.id = id;
        ws.map = player->GetMapId();
        ws.name = name.empty() ? "Checkpoint" : std::string(name);
        ws.unlockType = unlockType;
        ws.unlockValue = unlockValue;
        ws.sort = sort;
        ws.x = player->GetPositionX();
        ws.y = player->GetPositionY();
        ws.z = player->GetPositionZ();
        ws.o = player->GetOrientation();
        ws.entry = sConfigMgr->GetOption<uint32>("Temucraft.Checkpoints.GameObjectEntry", 608200);
        std::string escaped = ws.name;
        WorldDatabase.EscapeString(escaped);
        MapEntry const* mapEntry = sMapStore.LookupEntry(ws.map);
        std::string comment = Acore::StringFormat("{}: {} (unlock: {} {}), added in game by {}", mapEntry ? mapEntry->name[LOCALE_enUS] : "?",
                                                  ws.name, UnlockText(ws), ws.unlockValue, player->GetName());
        WorldDatabase.EscapeString(comment);
        WorldDatabase.DirectExecute("INSERT INTO `mod_temucraft_checkpoint` (`id`, `map`, `name`, `unlock_type`, `unlock_value`, `sort`, `position_x`, "
                                    "`position_y`, `position_z`, `orientation`, `gameobject_entry`, `guid`, `enabled`, `comment`) VALUES "
                                    "({}, {}, '{}', {}, {}, {}, {}, {}, {}, {}, {}, 0, 1, '{}')", ws.id, ws.map, escaped,
                                    uint32(ws.unlockType), ws.unlockValue, ws.sort, ws.x, ws.y, ws.z, ws.o, ws.entry, comment);
        checkpoints.push_back(ws);
        if (!Spawn(checkpoints.back(), player->GetMap()))
            handler->PSendSysMessage(TemucraftModuleText("mod-temucraft-checkpoints", handler, 8, "Gameobject entry {} does not exist: the row was added without a spawn."), ws.entry);
        Index();
        handler->PSendSysMessage(TemucraftModuleText("mod-temucraft-checkpoints", handler, 9, "Checkpoint {} '{}' added (sort {}, guid {})."), ws.id, ws.name, ws.sort, checkpoints.back().guid);
        return true;
    }

    static bool HandleImport(ChatHandler* handler)
    {
        uint32 added = 0, moved = 0;
        uint32 nextId = 100000;
        for (Checkpoint const& ws : checkpoints)
            nextId = std::max(nextId, ws.id + 1);
        for (auto const& [guid, data] : sObjectMgr->GetAllGOData())
        {
            if (!entries.count(data.id) && data.id != sConfigMgr->GetOption<uint32>("Temucraft.Checkpoints.GameObjectEntry", 608200))
                continue;
            if (Checkpoint const* known = FindBySpawn(guid))
            {
                Checkpoint* ws = FindById(known->id);
                if (std::fabs(ws->x - data.posX) + std::fabs(ws->y - data.posY) + std::fabs(ws->z - data.posZ) > 0.1f ||
                    std::fabs(ws->o - data.orientation) > 0.01f)
                {
                    ws->x = data.posX; ws->y = data.posY; ws->z = data.posZ; ws->o = data.orientation;
                    WorldDatabase.Execute("UPDATE `mod_temucraft_checkpoint` SET `position_x` = {}, `position_y` = {}, `position_z` = {}, "
                                          "`orientation` = {} WHERE `id` = {}", ws->x, ws->y, ws->z, ws->o, ws->id);
                    ++moved;
                }
                continue;
            }
            Checkpoint ws;
            ws.id = nextId++;
            ws.map = data.mapid;
            ws.name = "Checkpoint " + std::to_string(ws.id);
            ws.unlockType = UNLOCK_NEARBY;
            ws.sort = ws.id;
            ws.x = data.posX; ws.y = data.posY; ws.z = data.posZ; ws.o = data.orientation;
            ws.entry = data.id;
            ws.guid = guid;
            MapEntry const* mapEntry = sMapStore.LookupEntry(ws.map);
            std::string comment = Acore::StringFormat("{}: imported from gameobject {} (unlock: walk past until you set it)",
                                                      mapEntry ? mapEntry->name[LOCALE_enUS] : "?", guid);
            WorldDatabase.EscapeString(comment);
            WorldDatabase.Execute("INSERT INTO `mod_temucraft_checkpoint` (`id`, `map`, `name`, `unlock_type`, `unlock_value`, `sort`, `position_x`, "
                                  "`position_y`, `position_z`, `orientation`, `gameobject_entry`, `guid`, `enabled`, `comment`) VALUES "
                                  "({}, {}, '{}', 3, 0, {}, {}, {}, {}, {}, {}, {}, 1, '{}')", ws.id, ws.map, ws.name,
                                  ws.sort, ws.x, ws.y, ws.z, ws.o, ws.entry, guid, comment);
            checkpoints.push_back(ws);
            ++added;
        }
        Index();
        handler->PSendSysMessage(TemucraftModuleText("mod-temucraft-checkpoints", handler, 10, "Checkpoints: {} imported from gameobject (unlock: walk past; set name/unlock in mod_temucraft_checkpoint), "
                                 "{} positions updated from their gameobject."), added, moved);
        return true;
    }

    static bool HandleWake(ChatHandler* handler)
    {
        Map* map = handler->GetPlayer()->GetMap();
        auto itr = byMap.find(map->GetId());
        if (!map->IsDungeon() || itr == byMap.end())
        {
            handler->SendSysMessage(TemucraftModuleText("mod-temucraft-checkpoints", handler, 11, "No checkpoints in this instance."));
            return true;
        }
        InstanceProgress& p = ProgressOf(map);
        p.seeded = true;
        for (size_t i : itr->second)
            p.awake.insert(checkpoints[i].id);
        RefreshVisuals(map, p);
        handler->PSendSysMessage(TemucraftModuleText("mod-temucraft-checkpoints", handler, 12, "{} checkpoints awake in this instance."), itr->second.size());
        return true;
    }

    static bool HandleGo(ChatHandler* handler, uint32 id)
    {
        Checkpoint const* ws = FindById(id);
        if (!ws)
        {
            handler->PSendSysMessage(TemucraftModuleText("mod-temucraft-checkpoints", handler, 13, "No checkpoint {}."), id);
            return false;
        }
        handler->GetPlayer()->TeleportTo(ws->map, ws->x, ws->y, ws->z, ws->o);
        return true;
    }

    // .checkpoint link <id> <all|any> <links>: e.g. ".checkpoint link 7 all 3914 3886 b2 c5"
    static bool HandleLink(ChatHandler* handler, uint32 id, std::string mode, Tail linkText)
    {
        Checkpoint* ws = FindById(id);
        auto links = ParseLinks(std::string(linkText));
        if (!ws || (mode != "all" && mode != "any") || links.empty())
        {
            handler->SendSysMessage(TemucraftModuleText("mod-temucraft-checkpoints", handler, 14, "Usage: .checkpoint link <id> <all|any> <links>  (creature entries, b<boss index>, c<checkpoint id>)"));
            return false;
        }
        std::string text = std::string(linkText);
        WorldDatabase.EscapeString(text);
        WorldDatabase.DirectExecute("UPDATE `mod_temucraft_checkpoint` SET `unlock_type` = {}, `unlock_value` = 0, `linked` = '{}' WHERE `id` = {}",
                                    mode == "all" ? uint32(UNLOCK_ALL_OF) : uint32(UNLOCK_ANY_OF), text, id);
        LoadCheckpoints();
        handler->PSendSysMessage(TemucraftModuleText("mod-temucraft-checkpoints", handler, 15, "Checkpoint {} now wakes when {} of: {} ({} links)."), id, mode, std::string(linkText), links.size());
        return true;
    }

    static bool HandleReload(ChatHandler* handler)
    {
        LoadSettings();
        uint32 spawned = LoadCheckpoints();
        handler->PSendSysMessage(TemucraftModuleText("mod-temucraft-checkpoints", handler, 16, "Checkpoints reloaded: {} rows, {} new spawns (they appear in running instances when their grid loads again)."),
                                 checkpoints.size(), spawned);
        return true;
    }
};

void AddTemucraftGroupSummonScripts();

void AddTemucraftCheckpointsScripts()
{
    AddTemucraftGroupSummonScripts();
    new TemucraftCheckpoints_World();
    new TemucraftCheckpoints_Player();
    new TemucraftCheckpoints_GameObject();
    new TemucraftCheckpoints_Commands();
}
