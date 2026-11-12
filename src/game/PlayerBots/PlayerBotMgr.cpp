#include "Common.h"
#include "Policies/SingletonImp.h"
#include "PlayerBotMgr.h"
#include "ObjectMgr.h"
#include "World.h"
#include "WorldSession.h"
#include "AccountMgr.h"
#include "Config/Config.h"
#include "Chat.h"
#include "Player.h"
#include "Group.h"
#include "PlayerBotAI.h"
#include "PartyBotAI.h"
#include "BattleBotAI.h"
#include "BattleBotWaypoints.h"
#include "BattleGroundMgr.h"
#include "MapManager.h"
#include "Language.h"
#include "Utilities/Random.h"
#include "Spell.h"

#include <algorithm>
#include <sstream>

INSTANTIATE_SINGLETON_1(PlayerBotMgr);


PlayerBotMgr::PlayerBotMgr()
{
    m_totalChance = 0;
    m_maxAccountId = 0;

    // Config
    m_confMinRandomBots         = 3;
    m_confMaxRandomBots         = 10;
    m_confRandomBotsRefresh     = 60000;
    m_confUpdateDiff            = 10000;
    m_confEnableRandomBots      = false;
    m_confDebug                 = false;
    m_confBattleBotAutoJoin     = false;

    // Time
    m_elapsedTime = 0;
    m_lastBotsRefresh = 0;
    m_lastUpdate = 0;
    m_lastBattleBotQueueUpdate = 0;
}

PlayerBotMgr::~PlayerBotMgr()
{

}

void PlayerBotMgr::LoadConfig()
{
    m_confEnableRandomBots = sConfig.GetBoolDefault("RandomBot.Enable", false);
    m_confMinRandomBots = sConfig.GetIntDefault("RandomBot.MinBots", 3);
    m_confMaxRandomBots = sConfig.GetIntDefault("RandomBot.MaxBots", 10);
    m_confRandomBotsRefresh = sConfig.GetIntDefault("RandomBot.Refresh", 60000);
    m_confAllowSaving = sConfig.GetBoolDefault("PlayerBot.AllowSaving", false);
    m_confDebug = sConfig.GetBoolDefault("PlayerBot.Debug", false);
    m_confUpdateDiff = sConfig.GetIntDefault("PlayerBot.UpdateMs", 10000);
    m_confBattleBotAutoJoin = sConfig.GetBoolDefault("BattleBot.AutoJoin", false);

    if (!sWorld.getConfig(CONFIG_BOOL_FORCE_LOGOUT_DELAY))
        m_tempBots.clear();
}

void PlayerBotMgr::Load()
{
    // 1- Clean
    DeleteAll();
    m_bots.clear();
    m_tempBots.clear();
    m_totalChance = 0;

    // 2- Configuration
    LoadConfig();

    // 3- Load usable account ID
    std::unique_ptr<QueryResult> result = LoginDatabase.PQuery(
                              "SELECT MAX(`id`)"
                              " FROM `account`");
    if (!result)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "Playerbot: unable to load max account id.");
        return;
    }
    Field* fields = result->Fetch();
    m_maxAccountId = fields[0].GetUInt32() + 10000;

    // 4- LoadFromDB
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, ">> [PlayerBotMgr] Loading Bots ...");
    result = CharacterDatabase.PQuery(
                 "SELECT char_guid, chance, ai"
                 " FROM playerbot");
    if (!result)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "Table `playerbot` is empty.");
    else
    {
        do
        {
            fields = result->Fetch();
            uint32 guid = fields[0].GetUInt32();
            uint32 acc = GenBotAccountId();
            uint32 chance = fields[1].GetUInt32();

            std::shared_ptr<PlayerBotEntry> entry = std::make_shared<PlayerBotEntry>(guid, acc, chance);
            entry->ai.reset(CreatePlayerBotAI(fields[2].GetCppString()));
            entry->ai->botEntry = entry.get();
            if (!sObjectMgr.GetPlayerNameByGUID(guid, entry->name))
                entry->name = "<Unknown>";
            entry->ai->OnBotEntryLoad(entry.get());
            m_bots.insert({ entry->playerGUID, entry });
            m_totalChance += chance;
        }
        while (result->NextRow());
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "%u bots loaded", m_bots.size());
    }

    // 5- Check config/DB
    if (m_confMinRandomBots >= m_bots.size() && !m_bots.empty())
        m_confMinRandomBots = m_bots.size() - 1;
    if (m_confMaxRandomBots > m_bots.size())
        m_confMaxRandomBots = m_bots.size();
    if (m_confMaxRandomBots <= m_confMinRandomBots)
        m_confMaxRandomBots = m_confMinRandomBots + 1;

    // 6- Start initial bots
    if (m_confEnableRandomBots)
    {
        for (uint32 i = 0; i < m_confMinRandomBots; i++)
            AddRandomBot();
    }

    // 7- Fill stats info
    m_stats.confMaxOnline = m_confMaxRandomBots;
    m_stats.confMinOnline = m_confMinRandomBots;
    m_stats.totalBots = m_bots.size();
    m_stats.confRandomBotsRefresh = m_confRandomBotsRefresh;
    m_stats.confUpdateDiff = m_confUpdateDiff;

    // 8- Show stats if debug
    if (m_confDebug)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[PlayerBotMgr] Between %u and %u bots online", m_confMinRandomBots, m_confMaxRandomBots);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[PlayerBotMgr] %u now loading", m_stats.loadingCount);
    }
}

void PlayerBotMgr::DeleteAll()
{
    m_stats.onlineCount = 0;
    m_stats.loadingCount = 0;

    for (auto i = m_bots.begin(); i != m_bots.end(); i++)
    {
        if (i->second->state != PB_STATE_OFFLINE)
        {
            OnBotLogout(i->second.get());
            m_totalChance += i->second->chance;
        }
    }
    m_tempBots.clear();

    if (m_confDebug)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[PlayerBotMgr] Deleting all bots [OK]");
}

void PlayerBotMgr::OnBotLogin(PlayerBotEntry *e)
{
    e->state = PB_STATE_ONLINE;
    if (m_confDebug)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[PlayerBot][Login]  '%s' GUID:%u Acc:%u", e->name.c_str(), e->playerGUID, e->accountId);
}
void PlayerBotMgr::OnBotLogout(PlayerBotEntry *e)
{
    e->state = PB_STATE_OFFLINE;
    if (m_confDebug)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[PlayerBot][Logout] '%s' GUID:%u Acc:%u", e->name.c_str(), e->playerGUID, e->accountId);
}

void PlayerBotMgr::OnPlayerInWorld(Player* player)
{
    if (PlayerBotEntry* e = player->GetSession()->GetBot())
    {
        player->SetAI(e->ai.get());
        e->ai->SetPlayer(player);
        e->ai->OnPlayerLogin();
    }
}

void PlayerBotMgr::Update(uint32 diff)
{
    // Temporary bots.
    std::map<uint32, uint32>::iterator it;
    for (it = m_tempBots.begin(); it != m_tempBots.end(); ++it)
    {
        if (it->second < diff)
            it->second = 0;
        else
            it->second -= diff;
    }
    it = m_tempBots.begin();
    while (it != m_tempBots.end())
    {
        if (!it->second)
        {
            // Update of "chatBot" too.
            for (auto iter = m_bots.begin(); iter != m_bots.end(); ++iter)
                if (iter->second->accountId == it->first)
                {
                    iter->second->state = PB_STATE_OFFLINE; // Will get logged out at next WorldSession::Update call
                    m_bots.erase(iter);
                    break;
                }
            m_tempBots.erase(it);
            it = m_tempBots.begin();
        }
        else
            ++it;
    }

    m_elapsedTime += diff;
    if (!((m_elapsedTime - m_lastUpdate) > m_confUpdateDiff))
        return; // No need to update
    m_lastUpdate = m_elapsedTime;

    for (auto iter = m_bots.begin(); iter != m_bots.end();)
    {
        if (!m_confEnableRandomBots && !iter->second->customBot)
        {
            ++iter;
            continue;
        }

        if (iter->second->state == PB_STATE_ONLINE)
        {
            if (iter->second->requestRemoval)
            {
                if (iter->second->ai && iter->second->ai->me)
                {
                    if (!iter->second->ai->me->IsAlive())
                    {
                        // don't leave permanent corpse
                        iter->second->ai->me->ResurrectPlayer(1.0f);
                        iter->second->ai->me->SpawnCorpseBones();
                    }
                    iter->second->ai->me->RemoveFromGroup();

                    // Party bot gone: hand out the CC marks again (manual ones are kept).
                    if (PartyBotAI* pPartyAI = dynamic_cast<PartyBotAI*>(iter->second->ai.get()))
                        PartyBotAI::AssignCCMarks(ObjectAccessor::FindPlayer(pPartyAI->m_leaderGuid), false, "bot removed");
                }

                DeleteBot(iter);

                if (WorldSession* sess = sWorld.FindSession(iter->second->accountId))
                    sess->LogoutPlayer(m_confAllowSaving);

                iter->second->requestRemoval = false;

                if (iter->second->customBot)
                    iter = m_bots.erase(iter);
                else
                    ++iter;
                continue;
            }
        }

        // Connection of pending bots
        if (iter->second->state != PB_STATE_LOADING)
        {
            ++iter;
            continue;
        }

        WorldSession* sess = sWorld.FindSession(iter->second->accountId);

        if (!sess)
        {
            // This may happen : just wait for the World to add the session.
            ++iter;
            continue;
        }

        if (iter->second->ai->OnSessionLoaded(iter->second.get(), sess))
        {
            OnBotLogin(iter->second.get());
            m_stats.loadingCount--;

            if (iter->second->isChatBot)
                m_stats.onlineChat++;
            else
                m_stats.onlineCount++;
        }
        else
        {
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "PLAYERBOT: Unable to load session id %u", iter->second->accountId);
            DeleteBot(iter);

            if (iter->second->customBot)
                iter = m_bots.erase(iter);
            else
                ++iter;
            continue;
        }

        ++iter;
    }

    if (m_confBattleBotAutoJoin && (m_lastBattleBotQueueUpdate <= (sWorld.GetGameTime() - 10)))
    {
        m_lastBattleBotQueueUpdate = sWorld.GetGameTime();
        for (uint32 queueType = BATTLEGROUND_QUEUE_AV; queueType < MAX_BATTLEGROUND_QUEUE_TYPES; ++queueType)
        {
            bool hasPlayerInQueue[MAX_BATTLEGROUND_BRACKETS] = {};
            uint32 queuedAllianceCount[MAX_BATTLEGROUND_BRACKETS] = {};
            uint32 queuedHordeCount[MAX_BATTLEGROUND_BRACKETS] = {};
            BattleGroundQueue const& bgQueue = sBattleGroundMgr.m_battleGroundQueues[queueType];
            for (auto const& itr : bgQueue.m_queuedPlayers)
            {
                if (itr.second.groupInfo->isInvitedToBgInstanceGuid)
                    continue;

                if (Player* pPlayer = sObjectAccessor.FindPlayer(itr.first))
                {
                    BattleGroundTypeId bgTypeId = itr.second.groupInfo->bgTypeId;
                    BattleGroundBracketId bgBracketId = pPlayer->GetBattleGroundBracketIdFromLevel(bgTypeId);
                    if (bgBracketId == BG_BRACKET_ID_NONE)
                        continue;

                    if (itr.second.groupInfo->groupTeam == ALLIANCE)
                        ++queuedAllianceCount[bgBracketId];
                    else
                        ++queuedHordeCount[bgBracketId];

                    if (!pPlayer->IsBot())
                        hasPlayerInQueue[bgBracketId] = true;
                }
            }

            for (uint32 bracketId = BG_BRACKET_ID_FIRST; bracketId < MAX_BATTLEGROUND_BRACKETS; ++bracketId)
            {
                if (!hasPlayerInQueue[bracketId])
                    continue;

                if (!queuedAllianceCount[bracketId] && !queuedHordeCount[bracketId])
                    continue;

                BattleGroundTypeId bgTypeId = BattleGroundMgr::BgTemplateId(BattleGroundQueueTypeId(queueType));
                BattleGround* bg = sBattleGroundMgr.GetBattleGroundTemplate(bgTypeId);
                ASSERT(bg);

                uint32 const minLevel = bg->GetMinLevel() + 10 * bracketId;
                ASSERT(minLevel <= PLAYER_MAX_LEVEL);
                uint32 const maxLevel = std::min<uint32>(minLevel + 9, PLAYER_MAX_LEVEL);

                for (uint32 i = queuedAllianceCount[bracketId]; i < bg->GetMinPlayersPerTeam(); ++i)
                {
                    uint32 const botLevel = urand(minLevel, maxLevel);
                    AddBattleBot(BattleGroundQueueTypeId(queueType), ALLIANCE, botLevel, true);
                }
                for (uint32 i = queuedHordeCount[bracketId]; i < bg->GetMinPlayersPerTeam(); ++i)
                {
                    uint32 const botLevel = urand(minLevel, maxLevel);
                    AddBattleBot(BattleGroundQueueTypeId(queueType), HORDE, botLevel, true);
                }
            }
        }
    }

    if (m_confEnableRandomBots)
    {
        uint32 updatesCount = (m_elapsedTime - m_lastBotsRefresh) / m_confRandomBotsRefresh;
        for (uint32 i = 0; i < updatesCount; ++i)
        {
            AddOrRemoveBot();
            m_lastBotsRefresh += m_confRandomBotsRefresh;
        }
    }
}

/*
Toutes les X minutes, ajoute ou enleve un bot.
*/
bool PlayerBotMgr::AddOrRemoveBot()
{
    uint32 alea = urand(m_confMinRandomBots, m_confMaxRandomBots);
    /*
    10 --- --- --- --- --- --- --- --- --- --- 20 bots
                NumActuel
    [alea ici : remove    ][    ici, add    ]
    */
    if (alea > m_stats.onlineCount)
        return AddRandomBot();
    else
        return DeleteRandomBot();

}

bool PlayerBotMgr::AddBot(PlayerBotAI* ai)
{
    // Find a correct accountid ?
    std::shared_ptr<PlayerBotEntry> e = std::make_shared<PlayerBotEntry>();
    e->ai.reset(ai);
    e->accountId = GenBotAccountId();
    e->playerGUID = sObjectMgr.GeneratePlayerLowGuid();
    e->customBot = true;
    ai->botEntry = e.get();
    m_bots.insert({ e->playerGUID, e });
    return AddBot(e->playerGUID, false);
}

bool PlayerBotMgr::AddBot(uint32 playerGUID, bool chatBot, PlayerBotAI* pAI)
{
    uint32 accountId = 0;
    auto iter = m_bots.find(playerGUID);
    if (iter == m_bots.end())
        accountId = sObjectMgr.GetPlayerAccountIdByGUID(playerGUID);
    else
        accountId = iter->second->accountId;

    if (!accountId)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DETAIL, "Player account %u not found...", playerGUID);
        return false;
    }

    if (sWorld.FindSession(accountId))
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "[PlayerBotMgr] Account %u is already online!", accountId);
        return false;
    }

    std::shared_ptr<PlayerBotEntry> e;
    if (iter != m_bots.end())
    {
        e = iter->second;

        if (pAI) // new AI
        {
            e->ai.reset(pAI);
            e->customBot = true;
        }
    }
    else
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DETAIL, "[PlayerBotMgr] Adding temporary PlayerBot with GUID %u.", playerGUID);
        e = std::make_shared<PlayerBotEntry>();
        e->state        = PB_STATE_LOADING;
        e->playerGUID   = playerGUID;
        e->chance       = 10;
        e->accountId    = accountId;
        e->isChatBot    = chatBot;
        if (pAI)
        {
            e->ai.reset(pAI);
            e->customBot = true;
        }
        else
        {
            e->ai.reset(new PlayerBotAI(nullptr));
            e->customBot = false;
        }
        m_bots.insert({ playerGUID , e });
    }

    e->ai->botEntry = e.get();
    e->state = PB_STATE_LOADING;
    WorldSession* session = new WorldSession(accountId, nullptr, sAccountMgr.GetSecurity(accountId), 0, LOCALE_enUS);
    session->SetBot(e);
    sWorld.AddSession(session);
    m_stats.loadingCount++;
    if (chatBot)
        AddTempBot(accountId, 20000);

    return true;
}

bool PlayerBotMgr::AddRandomBot()
{
    uint32 alea = urand(0, m_totalChance);
    bool done = false;
    for (auto it = m_bots.begin(); it != m_bots.end() && !done; it++)
    {
        if (it->second->state != PB_STATE_OFFLINE)
            continue;
        if (it->second->customBot)
            continue;
        uint32 chance = it->second->chance;

        if (chance >= alea)
        {
            AddBot(it->first);
            done = true;
        }
        alea -= chance;
    }
    return done;
}

void PlayerBotMgr::AddTempBot(uint32 account, uint32 time)
{
    m_tempBots[account] = time;
}

void PlayerBotMgr::RefreshTempBot(uint32 account)
{
    if (m_tempBots.find(account) != m_tempBots.end())
    {
        uint32& delay = m_tempBots[account];
        if (delay < 1000)
            delay = 1000;
    }
}

bool PlayerBotMgr::DeleteBot(uint32 playerGUID)
{
    auto iter = m_bots.find(playerGUID);
    if (iter == m_bots.end())
        return false;

    return DeleteBot(iter);
}

bool PlayerBotMgr::DeleteBot(std::map<uint64, std::shared_ptr<PlayerBotEntry>>::iterator iter)
{
    if (iter->second->state == PB_STATE_LOADING)
        m_stats.loadingCount--;
    else if (iter->second->state == PB_STATE_ONLINE)
        m_stats.onlineCount--;

    OnBotLogout(iter->second.get());
    return true;
}

bool PlayerBotMgr::DeleteRandomBot()
{
    if (m_stats.onlineCount < 1)
        return false;
    uint32 idDelete = urand(0, m_stats.onlineCount);
    uint32 onlinePassed = 0;
    std::map<uint32, PlayerBotEntry*>::iterator iter;
    for (auto iter = m_bots.begin(); iter != m_bots.end(); iter++)
    {
        if (!iter->second->customBot && !iter->second->isChatBot && iter->second->state == PB_STATE_ONLINE)
        {
            onlinePassed++;
            if (onlinePassed == idDelete)
            {
                OnBotLogout(iter->second.get());
                m_stats.onlineCount--;
                return true;
            }
        }
    }
    return false;
}

uint8 SelectRandomRaceForClass(uint8 playerClass, Team playerTeam)
{
    std::vector<uint32> validRaces;
    for (uint32 raceId = 1; raceId < MAX_RACES; ++raceId)
    {
        if (playerTeam == ALLIANCE)
        {
            if (!((1 << (raceId - 1)) & RACEMASK_ALLIANCE))
                continue;
        }
        else if (playerTeam == HORDE)
        {
            if (!((1 << (raceId - 1)) & RACEMASK_HORDE))
                continue;
        }

        if (sObjectMgr.GetPlayerInfo(raceId, playerClass))
            validRaces.push_back(raceId);
    }

    if (validRaces.empty())
        return 0;

    return SelectRandomContainerElement(validRaces);
}

void PlayerBotMgr::AddBattleBot(BattleGroundQueueTypeId queueType, Team botTeam, uint32 botLevel, bool temporary)
{
    std::vector<uint32> availableClasses = { CLASS_WARRIOR, CLASS_HUNTER, CLASS_ROGUE, CLASS_MAGE, CLASS_WARLOCK, CLASS_PRIEST, CLASS_DRUID };
    if (botTeam == HORDE)
        availableClasses.push_back(CLASS_SHAMAN);
    else
        availableClasses.push_back(CLASS_PALADIN);

    uint8 botClass = SelectRandomContainerElement(availableClasses);
    uint8 botRace = SelectRandomRaceForClass(botClass, botTeam);
    if (!botRace)
        return;

    // Spawn bot on GM Island
    uint32 const instanceId = sMapMgr.GetContinentInstanceId(1, 16224.356f, 16284.763f);
    BattleBotAI* ai = new BattleBotAI(botRace, botClass, botLevel, 1, instanceId, 16224.356f, 16284.763f, 13.175f, 4.56f, queueType, temporary);
    AddBot(ai);

    if (botTeam == ALLIANCE)
    {
        if (temporary)
        {
            sWorld.SendWorldTextToBGAndQueue(LANG_ALLIANCE_BATTLEBOT_TEMP_ADDED, botLevel, queueType, botLevel, queueType);
            sLog.Out(LOG_BG, LOG_LVL_BASIC, "[PlayerBotMgr] Adding temporary level %u alliance battlebot to bg queue %u.", botLevel, queueType);
        }
        else
        {
            sWorld.SendWorldTextToBGAndQueue(LANG_ALLIANCE_BATTLEBOT_ADDED, botLevel, queueType, botLevel, queueType);
            sLog.Out(LOG_BG, LOG_LVL_BASIC, "[PlayerBotMgr] Adding level %u alliance battlebot to bg queue %u.", botLevel, queueType);
        }
    }
    else
    {
        if (temporary)
        {
            sWorld.SendWorldTextToBGAndQueue(LANG_HORDE_BATTLEBOT_TEMP_ADDED, botLevel, queueType, botLevel, queueType);
            sLog.Out(LOG_BG, LOG_LVL_BASIC, "[PlayerBotMgr] Adding temporary level %u horde battlebot to bg queue %u.", botLevel, queueType);
        }
        else
        {
            sWorld.SendWorldTextToBGAndQueue(LANG_HORDE_BATTLEBOT_ADDED, botLevel, queueType, botLevel, queueType);
            sLog.Out(LOG_BG, LOG_LVL_BASIC, "[PlayerBotMgr] Adding level %u horde battlebot to bg queue %u.", botLevel, queueType);
        }
    }
}

void PlayerBotMgr::DeleteBattleBots()
{
    for (auto const& itr : m_bots)
    {
        if (dynamic_cast<BattleBotAI*>(itr.second->ai.get()))
            itr.second->requestRemoval = true;
    }
    m_confBattleBotAutoJoin = false;
}

bool PlayerBotMgr::ForceAccountConnection(WorldSession* sess)
{
    if (sess->GetBot())
        return sess->GetBot()->state != PB_STATE_OFFLINE;

    // Temporary bots.
    return m_tempBots.find(sess->GetAccountId()) != m_tempBots.end();
}

bool PlayerBotMgr::IsPermanentBot(uint32 playerGUID)
{
    auto iter = m_bots.find(playerGUID);
    return iter != m_bots.end();
}

bool PlayerBotMgr::IsChatBot(uint32 playerGuid)
{
    auto iter = m_bots.find(playerGuid);
    return iter != m_bots.end() && iter->second->isChatBot;
}

void PlayerBotMgr::AddAllBots()
{
    for (auto it = m_bots.begin(); it != m_bots.end(); it++)
    {
        if (!it->second->isChatBot && it->second->state == PB_STATE_OFFLINE)
            AddBot(it->first);
    }
}

bool ChatHandler::HandleBotReloadCommand(char * args)
{
    sPlayerBotMgr.Load();
    SendSysMessage("PlayerBot system reloaded");
    return true;
}

bool ChatHandler::HandleBotAddRandomCommand(char * args)
{
    uint32 count = 1;
    char* sCount = strtok((char*)args, " ");
    if (sCount)
        count = uint32(atoi(sCount));
    for (uint32 i = 0; i < count; ++i)
        sPlayerBotMgr.AddRandomBot();
    PSendSysMessage("%u bots added", count);
    return true;
}

bool ChatHandler::HandleBotStopCommand(char * args)
{
    sPlayerBotMgr.DeleteAll();
    SendSysMessage("All the bots have been unloaded.");
    return true;
}

bool ChatHandler::HandleBotAddAllCommand(char * args)
{
    sPlayerBotMgr.AddAllBots();
    SendSysMessage("All bots have been loaded.");
    return true;
}

bool ChatHandler::HandleBotAddCommand(char* args)
{
    uint32 guid = 0;
    char *charname = nullptr;
    if (*args)
    {
        charname = strtok((char*)args, " ");
        if (charname && strcmp(charname, "") == 0)
            return false;

        guid = sObjectMgr.GetPlayerGuidByName(charname).GetCounter();
        if (!guid)
        {
            PSendSysMessage("Player not found : '%s'", charname);
            SetSentErrorMessage(true);
            return false;
        }
    }
    if (!guid || !sPlayerBotMgr.AddBot(guid))
    {
        SendSysMessage("[PlayerBotMgr] Unable to load bot.");
        return true;
    }
    PSendSysMessage("[PlayerBotMgr] Bot added : '%s', GUID=%u", charname ? charname : "NULL", guid);
    return true;
}

bool ChatHandler::HandleBotDeleteCommand(char * args)
{
    char *charname = strtok((char*)args, " ");

    if (!charname || strcmp(charname, "") == 0)
    {
        SendSysMessage("Syntax : $playerName");
        SetSentErrorMessage(true);
        return false;
    }
    uint32 lowGuid = sObjectMgr.GetPlayerGuidByName(charname).GetCounter();
    if (!lowGuid)
    {
        PSendSysMessage("Unable to find player: '%s'", charname);
        SetSentErrorMessage(true);
        return false;
    }
    if (sPlayerBotMgr.DeleteBot(lowGuid))
    {
        PSendSysMessage("Bot %s (GUID:%u) disconnected.", charname, lowGuid);
        return true;
    }
    else
    {
        PSendSysMessage("Bot %s (GUID:%u) : unable to disconnect.", charname, lowGuid);
        SetSentErrorMessage(true);
        return false;
    }
}

bool ChatHandler::HandleBotInfoCommand(char * args)
{
    uint32 online = sWorld.GetActiveSessionCount();

    PlayerBotStats stats = sPlayerBotMgr.GetStats();
    SendSysMessage("-- PlayerBot stats --");
    PSendSysMessage("Min:%u Max:%u Total:%u", stats.confMinOnline, stats.confMaxOnline, stats.totalBots);
    PSendSysMessage("Loading : %u, Online : %u, Chat : %u", stats.loadingCount, stats.onlineCount, stats.onlineChat);
    PSendSysMessage("%up + %ub = %u",
                    (online - stats.onlineCount), stats.onlineCount, online);
    return true;
}

bool ChatHandler::HandleBotStartCommand(char * args)
{
    sPlayerBotMgr.Start();
    return true;
}

bool ChatHandler::PartyBotAddRequirementCheck(Player const* pPlayer, Player const* pTarget)
{
    if (pPlayer->IsTaxiFlying())
    {
        SendSysMessage("Cannot add bots while flying.");
        return false;
    }

    // Spawning bots inside BG will cause server crash on BG end.
    if (pPlayer->InBattleGround())
    {
        SendSysMessage("Cannot add bots inside battlegrounds.");
        return false;
    }

    if (pPlayer->GetGroup() && (pPlayer->GetGroup()->IsFull() || sWorld.getConfig(CONFIG_UINT32_PARTY_BOT_MAX_BOTS) &&
        (pPlayer->GetGroup()->GetMembersCount() - 1 >= sWorld.getConfig(CONFIG_UINT32_PARTY_BOT_MAX_BOTS))))
    {
        SendSysMessage("Cannot add more bots. Group is full.");
        return false;
    }

    if (Map const* pMap = pPlayer->GetMap())
    {
        if (pMap->IsDungeon() &&
            pMap->GetPlayers().getSize() >= pMap->GetMapEntry()->maxPlayers)
        {
            SendSysMessage("Cannot add more bots. Instance is full.");
            return false;
        }
    }

    if (pTarget && pTarget->GetTeam() != pPlayer->GetTeam())
    {
        SendSysMessage("Cannot clone enemy faction characters.");
        return false;
    }

    // Restrictions when the command is made public to avoid abuse.
    if (GetSession()->GetSecurity() <= SEC_PLAYER && !sWorld.getConfig(CONFIG_BOOL_PARTY_BOT_SKIP_CHECKS))
    {
        if (pPlayer->IsDead())
        {
            SendSysMessage("Cannot add bots while dead.");
            return false;
        }

        if (pPlayer->IsInCombat())
        {
            SendSysMessage("Cannot add bots while in combat.");
            return false;
        }

        if (pPlayer->GetMap()->IsDungeon())
        {
            SendSysMessage("Cannot add bots while inside instances.");
            return false;
        }

        // Clone command.
        if (pTarget)
        {
            if (pTarget->IsDead())
            {
                SendSysMessage("Cannot clone dead characters.");
                return false;
            }

            if (pTarget->IsInCombat())
            {
                SendSysMessage("Cannot clone characters that are in combat.");
                return false;
            }

            if (pTarget->GetLevel() > pPlayer->GetLevel() + 10)
            {
                SendSysMessage("Cannot clone higher level characters.");
                return false;
            }
        }
    }

    return true;
}

// All .partybot commands look up bots through here, so a commanded bot reacts on the next
// world tick instead of waiting for its next AI update (up to PartyBot.UpdateInterval).
static PartyBotAI* GetCommandedPartyBotAI(Player* pBot)
{
    PartyBotAI* pAI = dynamic_cast<PartyBotAI*>(pBot->AI());
    if (pAI)
        pAI->WakeUp();
    return pAI;
}

// The role "dps" means for a class (add, setrole, slot none). Shamans and druids are casters
// (Elemental, Balance): enhancement and cat specs are not supported.
static CombatBotRoles GetDpsRoleForClass(uint8 playerClass)
{
    if (playerClass == CLASS_SHAMAN || playerClass == CLASS_DRUID)
        return ROLE_RANGE_DPS;
    return CombatBotBaseAI::IsMeleeDamageClass(playerClass) ? ROLE_MELEE_DPS : ROLE_RANGE_DPS;
}

bool ChatHandler::HandlePartyBotAddCommand(char* args)
{
    Player* pPlayer = m_session->GetPlayer();
    if (!pPlayer)
        return false;

    if (!PartyBotAddRequirementCheck(pPlayer, nullptr))
    {
        SetSentErrorMessage(true);
        return false;
    }

    // Optional words in any order: race, gender, class, role, level. Missing parts are picked
    // at random among the combinations that are valid with the given ones.
    static std::map<std::string, uint8> const classWords =
    {
        { "warrior", CLASS_WARRIOR }, { "paladin", CLASS_PALADIN }, { "hunter", CLASS_HUNTER },
        { "rogue", CLASS_ROGUE }, { "priest", CLASS_PRIEST }, { "shaman", CLASS_SHAMAN },
        { "mage", CLASS_MAGE }, { "warlock", CLASS_WARLOCK }, { "druid", CLASS_DRUID },
    };
    static std::map<std::string, uint8> const raceWords =
    {
        { "human", RACE_HUMAN }, { "dwarf", RACE_DWARF }, { "nightelf", RACE_NIGHTELF },
        { "gnome", RACE_GNOME }, { "orc", RACE_ORC }, { "undead", RACE_UNDEAD },
        { "tauren", RACE_TAUREN }, { "troll", RACE_TROLL },
    };
    static std::map<std::string, CombatBotRoles> const roleWords =
    {
        { "tank", ROLE_TANK }, { "healer", ROLE_HEALER }, { "melee", ROLE_MELEE_DPS },
        { "meleedps", ROLE_MELEE_DPS }, { "ranged", ROLE_RANGE_DPS }, { "rangedps", ROLE_RANGE_DPS },
        { "dps", ROLE_INVALID }, // melee or ranged, depending on the class
    };

    auto refuse = [this](std::string const& message)
    {
        SendSysMessage(message.c_str());
        SetSentErrorMessage(true);
        return false;
    };
    auto nameOf = [](std::map<std::string, uint8> const& words, uint8 value)
    {
        for (auto const& itr : words)
            if (itr.second == value)
            {
                std::string name = itr.first;
                name[0] = toupper(name[0]);
                return name;
            }
        return std::string("?");
    };

    uint8 botClass = 0;
    uint8 botRace = 0;
    uint8 botGender = GENDER_NONE;
    uint32 botLevel = pPlayer->GetLevel();
    bool levelGiven = false;
    bool roleGiven = false;
    std::string roleWord;
    CombatBotRoles botRole = ROLE_INVALID;
    // Custom levels only for GM accounts or when checks are skipped.
    bool const mayChooseLevel = GetSession()->GetSecurity() > SEC_PLAYER || sWorld.getConfig(CONFIG_BOOL_PARTY_BOT_SKIP_CHECKS);

    while (char* arg = ExtractArg(&args))
    {
        std::string word = arg;
        std::transform(word.begin(), word.end(), word.begin(), ::tolower);

        if (classWords.count(word))
        {
            if (botClass)
                return refuse("Two classes given.");
            botClass = classWords.at(word);
        }
        else if (raceWords.count(word))
        {
            if (botRace)
                return refuse("Two races given.");
            botRace = raceWords.at(word);
        }
        else if (word == "male" || word == "female")
        {
            if (botGender != GENDER_NONE)
                return refuse("Two genders given.");
            botGender = word == "male" ? GENDER_MALE : GENDER_FEMALE;
        }
        else if (roleWords.count(word))
        {
            if (roleGiven)
                return refuse("Two roles given.");
            roleGiven = true;
            roleWord = word;
            botRole = roleWords.at(word);
        }
        else if (!word.empty() && std::all_of(word.begin(), word.end(), ::isdigit))
        {
            if (!mayChooseLevel)
                return refuse("You cannot choose the bot level.");
            if (levelGiven)
                return refuse("Two levels given.");
            levelGiven = true;
            botLevel = std::stoul(word);
        }
        else
            return refuse("Unknown word '" + word + "'. Use race, male/female, class, role (tank, healer, dps, melee, ranged) or level.");
    }

    if (!botClass && !botRace && !roleGiven && botGender == GENDER_NONE)
        return refuse("Incorrect syntax. Expected race, male/female, class and/or role.");

    Team const team = pPlayer->GetTeam();
    uint32 const teamRaceMask = team == ALLIANCE ? RACEMASK_ALLIANCE : RACEMASK_HORDE;
    char const* teamName = team == ALLIANCE ? "Alliance" : "Horde";

    if (botRace && !((1 << (botRace - 1)) & teamRaceMask))
        return refuse(nameOf(raceWords, botRace) + " is " + (team == ALLIANCE ? "Horde" : "Alliance") + "; you are " + teamName + ".");

    // A class fits if it can do the role and some allowed race can play it.
    auto roleFits = [&](uint8 playerClass)
    {
        if (!roleGiven)
            return true;
        if (botRole == ROLE_INVALID) // "dps"
            return CombatBotBaseAI::IsMeleeDamageClass(playerClass) || CombatBotBaseAI::IsRangedDamageClass(playerClass);
        return CombatBotBaseAI::CanClassDoRole(playerClass, botRole);
    };
    auto raceFits = [&](uint8 playerClass)
    {
        if (botRace)
            return sObjectMgr.GetPlayerInfo(botRace, playerClass) != nullptr;
        return SelectRandomRaceForClass(playerClass, team) != 0;
    };

    if (botClass)
    {
        if (botRace && !raceFits(botClass))
            return refuse(nameOf(raceWords, botRace) + " cannot be a " + nameOf(classWords, botClass) + ".");
        if (!raceFits(botClass))
            return refuse(std::string("No ") + teamName + " race can be a " + nameOf(classWords, botClass) + ".");
        if (!roleFits(botClass))
            return refuse(nameOf(classWords, botClass) + " cannot be a " + roleWord + ".");
    }
    else
    {
        std::vector<uint8> candidates;
        for (auto const& itr : classWords)
            if (raceFits(itr.second) && roleFits(itr.second))
                candidates.push_back(itr.second);
        if (candidates.empty())
            return refuse(std::string("No valid ") + teamName + " class for that combination.");
        botClass = SelectRandomContainerElement(candidates);
    }

    if (roleGiven && botRole == ROLE_INVALID) // "dps"
        botRole = GetDpsRoleForClass(botClass);
    else if (!roleGiven) // decided now, so it is known and shown at once
        botRole = CombatBotBaseAI::GetDefaultRoleForClass(botClass);

    if (!botRace)
        botRace = SelectRandomRaceForClass(botClass, team);

    float x, y, z;
    pPlayer->GetNearPoint(pPlayer, x, y, z, 0, 5.0f, frand(0.0f, 6.0f));

    PartyBotAI* ai = new PartyBotAI(pPlayer, nullptr, botRole, botRace, botClass, botLevel, pPlayer->GetMapId(), pPlayer->GetMap()->GetInstanceId(), x, y, z, pPlayer->GetOrientation());
    ai->m_gender = botGender;
    if (sPlayerBotMgr.AddBot(ai))
        PSendSysMessage("New party bot added: %s%s %s, %s.",
            botGender == GENDER_NONE ? "" : (botGender == GENDER_MALE ? "male " : "female "),
            nameOf(raceWords, botRace).c_str(), nameOf(classWords, botClass).c_str(),
            CombatBotBaseAI::GetRoleName(botRole));
    else
    {
        delete ai;
        SendSysMessage("Error spawning bot.");
        SetSentErrorMessage(true);
        return false;
    }

    return true;
}

bool ChatHandler::HandlePartyBotCloneCommand(char* args)
{
    Player* pPlayer = m_session->GetPlayer();
    if (!pPlayer)
        return false;

    Player* pTarget = GetSelectedPlayer();
    if (!pTarget)
    {
        SendSysMessage(LANG_NO_CHAR_SELECTED);
        SetSentErrorMessage(true);
        return false;
    }

    if (!PartyBotAddRequirementCheck(pPlayer, pTarget))
    {
        SetSentErrorMessage(true);
        return false;
    }

    uint8 botRace = pTarget->GetRace();
    uint8 botClass = pTarget->GetClass();

    float x, y, z;
    pPlayer->GetNearPoint(pPlayer, x, y, z, 0, 5.0f, frand(0.0f, 6.0f));

    PartyBotAI* ai = new PartyBotAI(pPlayer, pTarget, ROLE_INVALID, botRace, botClass, pPlayer->GetLevel(), pPlayer->GetMapId(), pPlayer->GetMap()->GetInstanceId(), x, y, z, pPlayer->GetOrientation());
    if (sPlayerBotMgr.AddBot(ai))
        SendSysMessage("New party bot added.");
    else
    {
        delete ai;
        SendSysMessage("Error spawning bot.");
        SetSentErrorMessage(true);
        return false;
    }

    return true;
}

bool ChatHandler::HandlePartyBotLoadCommand(char* args)
{
    Player* pPlayer = m_session->GetPlayer();
    if (!pPlayer)
        return false;

    std::string name = ExtractPlayerNameFromLink(&args);
    if (name.empty())
    {
        SendSysMessage(LANG_PLAYER_NOT_FOUND);
        SetSentErrorMessage(true);
        return false;
    }

    ObjectGuid guid = sObjectMgr.GetPlayerGuidByName(name).GetCounter();
    if (!guid)
    {
        SendSysMessage(LANG_PLAYER_NOT_FOUND);
        SetSentErrorMessage(true);
        return false;
    }

    if (sObjectAccessor.FindPlayerNotInWorld(guid))
    {
        SendSysMessage("Player is already online!");
        SetSentErrorMessage(true);
        return false;
    }

    float x, y, z;
    pPlayer->GetNearPoint(pPlayer, x, y, z, 0, 5.0f, frand(0.0f, 6.0f));

    PartyBotAI* pAI = new PartyBotAI(pPlayer, pPlayer->GetMapId(), pPlayer->GetMap()->GetInstanceId(), x, y, z, pPlayer->GetOrientation());

    if (!sPlayerBotMgr.AddBot(guid, false, pAI))
    {
        delete pAI;
        SendSysMessage("Error spawning bot.");
        SetSentErrorMessage(true);
        return false;
    }

    PSendSysMessage("Loading %s as party bot.", name.c_str());
    return true;
}

bool ChatHandler::HandlePartyBotSetRoleCommand(char* args)
{
    if (!args)
        return false;

    Player* pTarget = GetSelectedPlayer();
    if (!pTarget)
    {
        SendSysMessage(LANG_NO_CHAR_SELECTED);
        SetSentErrorMessage(true);
        return false;
    }

    CombatBotRoles role = ROLE_INVALID;
    std::string roleStr = args;

    if (roleStr == "tank")
        role = ROLE_TANK;
    else if (roleStr == "dps")
        role = GetDpsRoleForClass(pTarget->GetClass());
    else if (roleStr == "meleedps")
        role = ROLE_MELEE_DPS;
    else if (roleStr == "rangedps")
        role = ROLE_RANGE_DPS;
    else if (roleStr == "healer")
        role = ROLE_HEALER;

    if (role == ROLE_INVALID)
        return false;

    if (!CombatBotBaseAI::CanClassDoRole(pTarget->GetClass(), role))
    {
        PSendSysMessage("%s cannot be a %s (wrong class).", pTarget->GetName(), roleStr.c_str());
        SetSentErrorMessage(true);
        return false;
    }

    if (pTarget->AI())
    {
        if (PartyBotAI* pAI = GetCommandedPartyBotAI(pTarget))
        {
            bool const roleChanged = pAI->m_role != role;
            pAI->m_role = role;
            if (role != ROLE_TANK)
                pAI->m_tankSlotManual = false;
            pAI->ResetSpellData();
            pAI->PopulateSpellData();
            PSendSysMessage("%s is now a %s.", pTarget->GetName(), roleStr.c_str());

            // Task 17: added bots get the new role's talents once out of combat; saved (loaded)
            // and cloned characters keep theirs.
            if (!pAI->IsLevelSynced())
            {
                PSendSysMessage("%s keeps its talents (saved or cloned character).", pTarget->GetName());
                sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Talent] %s: role %s, talents kept (saved or cloned character)",
                    CombatBotBaseAI::GetLogName(pTarget).c_str(), CombatBotBaseAI::GetRoleName(role));
            }
            else if (roleChanged || pAI->m_pendingRespec)
            {
                pAI->m_pendingRespec = true;
                if (pTarget->IsInCombat())
                {
                    PSendSysMessage("%s gets new talents when the fight ends.", pTarget->GetName());
                    sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Talent] %s: respec for role %s waits for the fight to end",
                        CombatBotBaseAI::GetLogName(pTarget).c_str(), CombatBotBaseAI::GetRoleName(role));
                }
                else
                    PSendSysMessage("%s gets new talents now.", pTarget->GetName());
            }
            return true;
        }
    }

    SendSysMessage("Target is not a party bot.");
    SetSentErrorMessage(true);
    return false;
}

// .partybot slot mt|ot1..ot9|none|auto on the selected bot, or on yourself / another player.
bool ChatHandler::HandlePartyBotSlotCommand(char* args)
{
    char* arg = ExtractArg(&args);
    if (!arg)
    {
        SendSysMessage("Usage: .partybot slot mt|ot1..ot9|none|auto (on the selected bot or yourself).");
        SetSentErrorMessage(true);
        return false;
    }

    std::string slotStr = arg;
    std::transform(slotStr.begin(), slotStr.end(), slotStr.begin(), ::tolower);

    bool manual = true;
    uint8 slot = PB_TANK_SLOT_NONE;
    if (slotStr == "auto")
        manual = false;
    else if (slotStr == "mt")
        slot = PB_TANK_SLOT_MT;
    else if (slotStr.size() == 3 && slotStr.compare(0, 2, "ot") == 0 && slotStr[2] >= '1' && slotStr[2] <= '9')
        slot = PB_TANK_SLOT_OT1 + (slotStr[2] - '1');
    else if (slotStr != "none")
    {
        SendSysMessage("Unknown slot. Use mt, ot1..ot9, none or auto.");
        SetSentErrorMessage(true);
        return false;
    }

    Player* pPlayer = GetSession()->GetPlayer();
    Player* pTarget = GetSelectedPlayer(); // yourself when nothing is selected
    if (!pTarget)
    {
        SendSysMessage(LANG_NO_CHAR_SELECTED);
        SetSentErrorMessage(true);
        return false;
    }

    Group* pGroup = pTarget->GetGroup();
    if (!pGroup || pGroup != pPlayer->GetGroup())
    {
        SendSysMessage("Target is not in your group.");
        SetSentErrorMessage(true);
        return false;
    }

    if (slot != PB_TANK_SLOT_NONE && !CombatBotBaseAI::IsTankClass(pTarget->GetClass()))
    {
        PSendSysMessage("%s cannot tank (wrong class).", pTarget->GetName());
        SetSentErrorMessage(true);
        return false;
    }

    if (manual && slot != PB_TANK_SLOT_NONE)
        PartyBotAI::ClearManualTankSlot(pGroup, slot);

    if (pTarget->AI())
    {
        PartyBotAI* pAI = GetCommandedPartyBotAI(pTarget);
        if (!pAI)
        {
            SendSysMessage("Target is not a party bot.");
            SetSentErrorMessage(true);
            return false;
        }

        // A bot given a slot becomes a tank; "none" turns it into damage dealer.
        CombatBotRoles newRole = pAI->m_role;
        if (manual && slot != PB_TANK_SLOT_NONE)
            newRole = ROLE_TANK;
        else if (manual)
            newRole = GetDpsRoleForClass(pTarget->GetClass());

        pAI->m_tankSlotManual = manual && slot != PB_TANK_SLOT_NONE;
        pAI->m_tankSlotChoice = slot;
        if (newRole != pAI->m_role)
        {
            pAI->m_role = newRole;
            pAI->ResetSpellData();
            pAI->PopulateSpellData();
        }
    }
    else
        PartyBotAI::SetPlayerTankSlotChoice(pTarget->GetObjectGuid(), manual, slot);

    if (!manual)
        PSendSysMessage("%s: tank slot is automatic again.", pTarget->GetName());
    else if (slot == PB_TANK_SLOT_NONE)
        PSendSysMessage("%s: no tank slot%s.", pTarget->GetName(), pTarget->AI() ? " (now dps)" : "");
    else
        PSendSysMessage("%s is now %s.", pTarget->GetName(), PartyBotAI::GetTankSlotName(slot).c_str());

    return true;
}

// .partybot slots: lists the current tank slots of your group.
bool ChatHandler::HandlePartyBotSlotsCommand(char* /*args*/)
{
    Player* pPlayer = GetSession()->GetPlayer();
    Group* pGroup = pPlayer->GetGroup();
    if (!pGroup)
    {
        SendSysMessage("You are not in a group.");
        SetSentErrorMessage(true);
        return false;
    }

    PartyBotTankSlotMap slots;
    PartyBotAI::ResolveTankSlots(pGroup, slots);

    std::vector<std::pair<uint8, ObjectGuid>> ordered;
    for (auto const& itr : slots)
        ordered.emplace_back(itr.second, itr.first);
    std::sort(ordered.begin(), ordered.end());

    if (ordered.empty())
        SendSysMessage("No tanks in the group.");

    for (auto const& itr : ordered)
    {
        Player* pMember = sObjectMgr.GetPlayer(itr.second);
        if (!pMember)
            continue;

        char const* how = "auto";
        bool singleFocus = false;
        if (PartyBotAI* pAI = dynamic_cast<PartyBotAI*>(pMember->AI()))
        {
            if (pAI->m_tankSlotManual)
                how = "set";
            singleFocus = pAI->m_singleFocus;
        }
        else
        {
            uint8 choice;
            if (PartyBotAI::GetPlayerTankSlotChoice(pMember->GetObjectGuid(), choice))
                how = "set";
        }

        PSendSysMessage("%s: %s (%s%s)", PartyBotAI::GetTankSlotName(itr.first).c_str(),
            pMember->GetName(), how, singleFocus ? ", single focus" : "");
    }

    // Everyone else: bots with their role.
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || slots.count(pMember->GetObjectGuid()))
            continue;
        if (PartyBotAI* pAI = dynamic_cast<PartyBotAI*>(pMember->AI()))
            PSendSysMessage("%s: %s", pMember->GetName(), CombatBotBaseAI::GetRoleName(pAI->GetRole()));
    }
    return true;
}

enum PartyBotOnOffSetting : uint8
{
    PB_SETTING_SINGLE_FOCUS,
};

static void SetPartyBotOnOffSetting(PartyBotAI* pAI, uint8 setting, bool on)
{
    switch (setting)
    {
        case PB_SETTING_SINGLE_FOCUS:
            pAI->m_singleFocus = on;
            break;
    }
}

// On/off bot settings: ".partybot <name> on|off [all]". A selected bot wins over "all".
bool ChatHandler::HandlePartyBotOnOffHelper(char* args, char const* name, uint8 setting)
{
    char* arg1 = ExtractArg(&args);
    char* arg2 = ExtractArg(&args);
    if (!arg1 || (strcmp(arg1, "on") != 0 && strcmp(arg1, "off") != 0))
    {
        PSendSysMessage("Usage: .partybot %s on|off [all]", name);
        SetSentErrorMessage(true);
        return false;
    }

    bool const on = strcmp(arg1, "on") == 0;
    bool const all = arg2 && strcmp(arg2, "all") == 0;
    Player* pPlayer = GetSession()->GetPlayer();

    Player* pTarget = GetSelectedPlayer();
    if (pTarget && pTarget != pPlayer && dynamic_cast<PartyBotAI*>(pTarget->AI()))
    {
        SetPartyBotOnOffSetting(GetCommandedPartyBotAI(pTarget), setting, on);
        PSendSysMessage("%s %s %s.", pTarget->GetName(), name, on ? "on" : "off");
        return true;
    }

    if (!all)
    {
        SendSysMessage("Select a party bot or add 'all'.");
        SetSentErrorMessage(true);
        return false;
    }

    Group* pGroup = pPlayer->GetGroup();
    if (!pGroup)
    {
        SendSysMessage("You are not in a group.");
        SetSentErrorMessage(true);
        return false;
    }

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        if (Player* pMember = itr->getSource())
            if (pMember != pPlayer && dynamic_cast<PartyBotAI*>(pMember->AI()))
                SetPartyBotOnOffSetting(GetCommandedPartyBotAI(pMember), setting, on);

    PSendSysMessage("All party bots %s %s.", name, on ? "on" : "off");
    return true;
}

// .partybot singlefocus on|off [all]: tank lock. On: tanks stay on their preferred target (attack
// order, skull or focus mark) only; off: they also grab loose enemies. No effect on other roles.
bool ChatHandler::HandlePartyBotSingleFocusCommand(char* args)
{
    return HandlePartyBotOnOffHelper(args, "singlefocus", PB_SETTING_SINGLE_FOCUS);
}

bool ChatHandler::HandlePartyBotAttackStartCommand(char* args)
{
    Player* pPlayer = GetSession()->GetPlayer();
    Unit* pTarget = GetSelectedUnit();
    if (!pTarget || (pTarget == pPlayer))
    {
        SendSysMessage(LANG_SELECT_CHAR_OR_CREATURE);
        SetSentErrorMessage(true);
        return false;
    }

    Group* pGroup = pPlayer->GetGroup();
    if (!pGroup)
    {
        SendSysMessage("You are not in a group.");
        SetSentErrorMessage(true);
        return false;
    }

    // Healers keep healing (they have no attack duty); everyone else leaves stay and attacks.
    std::string attackers;
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        if (Player* pMember = itr->getSource())
        {
            if (pMember == pPlayer)
                continue;

            if (pMember->AI())
            {
                if (PartyBotAI* pAI = GetCommandedPartyBotAI(pMember))
                {
                    if (pAI->GetRole() != ROLE_HEALER && pMember->IsValidAttackTarget(pTarget))
                    {
                        pAI->OrderAttack(pTarget, true);
                        attackers += attackers.empty() ? pMember->GetName() : std::string(", ") + pMember->GetName();
                    }
                }
            }
        }
    }

    sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Attack] %s: attack %s -> %s", pPlayer->GetName(), CombatBotBaseAI::GetLogName(pTarget).c_str(),
        attackers.empty() ? "nobody" : attackers.c_str());
    PSendSysMessage("All party bots are now attacking %s.", pTarget->GetName());
    return true;
}

void StopPartyBotAttackHelper(PartyBotAI* pAI, Player* pBot)
{
    pAI->ClearOrderedTarget();
    pBot->AttackStop(true);
    pBot->InterruptNonMeleeSpells(false);
    if (!pBot->IsStopped())
        pBot->StopMoving();
    if (pBot->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE)
        pBot->GetMotionMaster()->Clear();
    if (pAI->m_updateTimer.GetExpiry() < 3000)
        pAI->m_updateTimer.Reset(3000);
}

bool ChatHandler::HandlePartyBotAttackStopCommand(char* args)
{
    Player* pPlayer = GetSession()->GetPlayer();
    Unit* pTarget = GetSelectedUnit();
    if (!pTarget || (pTarget == pPlayer))
    {
        SendSysMessage(LANG_SELECT_CHAR_OR_CREATURE);
        SetSentErrorMessage(true);
        return false;
    }

    Group* pGroup = pPlayer->GetGroup();
    if (!pGroup)
    {
        SendSysMessage("You are not in a group.");
        SetSentErrorMessage(true);
        return false;
    }

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        if (Player* pMember = itr->getSource())
        {
            if (pMember == pPlayer)
                continue;

            if (pMember->AI())
            {
                if (PartyBotAI* pAI = GetCommandedPartyBotAI(pMember))
                {
                    if (pMember->GetVictim() == pTarget)
                        StopPartyBotAttackHelper(pAI, pMember);
                }
            }
        }
    }

    PSendSysMessage("All party bots have stopped attacking %s.", pTarget->GetName());
    return true;
}

// .partybot aoe auto|on|off [all]: area spells of damage dealers. auto (default): packs of
// PartyBot.AutoAoE.MinTargets+ and never near crowd control; on: forced for one fight (any pack,
// also near crowd control; ends on fight over, attack, pull, aoe auto/off); off: none.
// A selected bot wins over "all".
bool ChatHandler::HandlePartyBotAoECommand(char* args)
{
    char* arg1 = ExtractArg(&args);
    char* arg2 = ExtractArg(&args);
    PartyBotAoEMode mode;
    if (arg1 && strcmp(arg1, "auto") == 0)
        mode = PB_AOE_AUTO;
    else if (arg1 && strcmp(arg1, "on") == 0)
        mode = PB_AOE_FORCED;
    else if (arg1 && strcmp(arg1, "off") == 0)
        mode = PB_AOE_OFF;
    else
    {
        SendSysMessage("Usage: .partybot aoe auto|on|off [all]");
        SetSentErrorMessage(true);
        return false;
    }

    Player* pPlayer = GetSession()->GetPlayer();
    Player* pTarget = GetSelectedPlayer();
    if (pTarget && pTarget != pPlayer && dynamic_cast<PartyBotAI*>(pTarget->AI()))
    {
        GetCommandedPartyBotAI(pTarget)->SetAoEMode(mode, "command");
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[AoE] %s: aoe %s -> %s", pPlayer->GetName(), arg1, CombatBotBaseAI::GetLogName(pTarget).c_str());
        PSendSysMessage("%s: aoe %s.", pTarget->GetName(), arg1);
        return true;
    }

    if (!arg2 || strcmp(arg2, "all") != 0)
    {
        SendSysMessage("Select a party bot or add 'all'.");
        SetSentErrorMessage(true);
        return false;
    }

    Group* pGroup = pPlayer->GetGroup();
    if (!pGroup)
    {
        SendSysMessage("You are not in a group.");
        SetSentErrorMessage(true);
        return false;
    }

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        if (Player* pMember = itr->getSource())
            if (pMember != pPlayer && dynamic_cast<PartyBotAI*>(pMember->AI()))
                GetCommandedPartyBotAI(pMember)->SetAoEMode(mode, "command");

    sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[AoE] %s: aoe %s -> all bots", pPlayer->GetName(), arg1);
    PSendSysMessage("All party bots: aoe %s.", arg1);
    return true;
}

bool ChatHandler::HandlePartyBotStartCastingCommand(char * args)
{
    return HandlePartyBotToggleCastingCommand(true);
}

bool ChatHandler::HandlePartyBotStopCastingCommand(char * args)
{
    return HandlePartyBotToggleCastingCommand(false);
}

bool ChatHandler::HandlePartyBotToggleCastingCommand(bool allowCasting)
{
    Player* pPlayer = GetSession()->GetPlayer();
    Player* pTarget = GetSelectedPlayer();

    if (pTarget && (pTarget != pPlayer))
    {
        if (pTarget->AI())
        {
            if (PartyBotAI* pAI = GetCommandedPartyBotAI(pTarget))
            {
                if (allowCasting)
                {
                    pAI->m_preventCasting = false;
                    PSendSysMessage("%s will be allowed to cast spells.", pTarget->GetName());
                }
                else
                {
                    pAI->m_preventCasting = true;
                    pTarget->InterruptNonMeleeSpells(false);
                    PSendSysMessage("%s will no longer cast spells.", pTarget->GetName());
                }
                return true;
            }
        }
        SendSysMessage("Target is not a party bot.");
        SetSentErrorMessage(true);
        return false;
    }

    Group* pGroup = pPlayer->GetGroup();
    if (!pGroup)
    {
        SendSysMessage("You are not in a group.");
        SetSentErrorMessage(true);
        return false;
    }

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        if (Player* pMember = itr->getSource())
        {
            if (pMember == pPlayer)
                continue;

            if (pMember->AI())
            {
                if (PartyBotAI* pAI = GetCommandedPartyBotAI(pMember))
                {
                    if (allowCasting)
                    {
                        pAI->m_preventCasting = false;
                    }
                    else
                    {
                        pAI->m_preventCasting = true;
                        pMember->InterruptNonMeleeSpells(false);
                    }
                }
            }
        }
    }

    if (allowCasting)
        SendSysMessage("All bots are now allowed to cast spells again.");
    else
        SendSysMessage("All bots are now forbidden from casting spells.");

    return true;
}

static std::map<std::string, RaidTargetIcon> raidTargetIcons =
{
    { "star",     RAID_TARGET_ICON_STAR     },
    { "circle",   RAID_TARGET_ICON_CIRCLE   },
    { "diamond",  RAID_TARGET_ICON_DIAMOND  },
    { "triangle", RAID_TARGET_ICON_TRIANGLE },
    { "moon",     RAID_TARGET_ICON_MOON     },
    { "square",   RAID_TARGET_ICON_SQUARE   },
    { "cross",    RAID_TARGET_ICON_CROSS    },
    { "skull",    RAID_TARGET_ICON_SKULL    },
};

bool ChatHandler::HandlePartyBotControlMarkCommand(char* args)
{
    std::string mark = args;
    auto itrMark = raidTargetIcons.find(mark);
    if (itrMark == raidTargetIcons.end())
    {
        SendSysMessage("Unknown target mark. Valid names are: star, circle, diamond, triangle, moon, square, cross, skull");
        SetSentErrorMessage(true);
        return false;
    }

    Player* pPlayer = GetSession()->GetPlayer();
    Player* pTarget = GetSelectedPlayer();

    if (pTarget && (pTarget != pPlayer))
    {
        if (pTarget->AI())
        {
            if (PartyBotAI* pAI = GetCommandedPartyBotAI(pTarget))
            {
                PSendSysMessage("%s will crowd control %s.", pTarget->GetName(), args);
                // A manual mark replaces an automatic one; the other automatic marks are redone
                // around it (a bot that had this mark automatically takes a spare one).
                if (pAI->m_ccMarksAuto)
                    pAI->m_marksToCC.clear();
                pAI->m_ccMarksAuto = false;
                pAI->m_marksToCC.push_back(itrMark->second);
                sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Mark] %s: ccmark %s -> %s (no bot attacks it unless ordered)",
                    pPlayer->GetName(), args, CombatBotBaseAI::GetLogName(pTarget).c_str());
                PartyBotAI::AssignCCMarks(pPlayer, false, "ccmark");
                return true;
            }
        }
        SendSysMessage("Target is not a party bot.");
        SetSentErrorMessage(true);
        return false;
    }

    Group* pGroup = pPlayer->GetGroup();
    if (!pGroup)
    {
        SendSysMessage("You are not in a group.");
        SetSentErrorMessage(true);
        return false;
    }

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        if (Player* pMember = itr->getSource())
        {
            if (pMember == pPlayer)
                continue;

            if (pMember->AI())
            {
                if (PartyBotAI* pAI = GetCommandedPartyBotAI(pMember))
                {
                    if (pAI->m_ccMarksAuto)
                        pAI->m_marksToCC.clear();
                    pAI->m_ccMarksAuto = false;
                    pAI->m_marksToCC.push_back(itrMark->second);
                }
            }
        }
    }

    sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Mark] %s: ccmark %s -> all bots (no bot attacks it unless ordered)",
        pPlayer->GetName(), args);
    PSendSysMessage("All party bots will crowd control %s.", args);
    return true;
}

bool ChatHandler::HandlePartyBotFocusMarkCommand(char* args)
{
    std::string mark = args;
    auto itrMark = raidTargetIcons.find(mark);
    if (itrMark == raidTargetIcons.end())
    {
        SendSysMessage("Unknown target mark. Valid names are: star, circle, diamond, triangle, moon, square, cross, skull");
        SetSentErrorMessage(true);
        return false;
    }

    Player* pPlayer = GetSession()->GetPlayer();
    Player* pTarget = GetSelectedPlayer();

    if (pTarget && (pTarget != pPlayer))
    {
        if (pTarget->AI())
        {
            if (PartyBotAI* pAI = GetCommandedPartyBotAI(pTarget))
            {
                if (std::find(pAI->m_marksToFocus.begin(), pAI->m_marksToFocus.end(), itrMark->second) != pAI->m_marksToFocus.end())
                {
                    PSendSysMessage("%s already have focus %s.", pTarget->GetName(), args);
                    return false;
                }

                PSendSysMessage("%s will focus %s.", pTarget->GetName(), args);
                pAI->m_marksToFocus.push_back(itrMark->second);
                sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Mark] %s: focusmark %s -> %s", pPlayer->GetName(), args,
                    CombatBotBaseAI::GetLogName(pTarget).c_str());
                return true;
            }
        }
        SendSysMessage("Target is not a party bot.");
        SetSentErrorMessage(true);
        return false;
    }

    Group* pGroup = pPlayer->GetGroup();
    if (!pGroup)
    {
        SendSysMessage("You are not in a group.");
        SetSentErrorMessage(true);
        return false;
    }

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        if (Player* pMember = itr->getSource())
        {
            if (pMember == pPlayer)
                continue;

            if (pMember->AI())
            {
                if (PartyBotAI* pAI = GetCommandedPartyBotAI(pMember))
                {
                    if (std::find(pAI->m_marksToFocus.begin(), pAI->m_marksToFocus.end(), itrMark->second) != pAI->m_marksToFocus.end())
                    {
                        // Already have focus mark
                        continue;
                    }
                    pAI->m_marksToFocus.push_back(itrMark->second);
                }
            }
        }
    }

    sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Mark] %s: focusmark %s -> all bots", pPlayer->GetName(), args);
    PSendSysMessage("All party bots will focus %s.", args);
    return true;
}

bool ChatHandler::HandlePartyBotClearMarksCommand(char* args)
{
    Player* pPlayer = GetSession()->GetPlayer();
    Player* pTarget = GetSelectedPlayer();

    if (pTarget && (pTarget != pPlayer))
    {
        if (pTarget->AI())
        {
            if (PartyBotAI* pAI = GetCommandedPartyBotAI(pTarget))
            {
                PSendSysMessage("All mark assignments cleared for %s.", pTarget->GetName());
                sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Mark] %s: clearmarks -> %s", pPlayer->GetName(), CombatBotBaseAI::GetLogName(pTarget).c_str());
                pAI->m_marksToCC.clear();
                pAI->m_ccMarksAuto = false;
                pAI->m_releasedCC.clear();
                pAI->m_marksToFocus.clear();
                return true;
            }
        }
        SendSysMessage("Target is not a party bot.");
        SetSentErrorMessage(true);
        return false;
    }

    Group* pGroup = pPlayer->GetGroup();
    if (!pGroup)
    {
        SendSysMessage("You are not in a group.");
        SetSentErrorMessage(true);
        return false;
    }

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        if (Player* pMember = itr->getSource())
        {
            if (pMember == pPlayer)
                continue;

            if (pMember->AI())
            {
                if (PartyBotAI* pAI = GetCommandedPartyBotAI(pMember))
                {
                    pAI->m_marksToCC.clear();
                    pAI->m_ccMarksAuto = false;
                    pAI->m_releasedCC.clear();
                    pAI->m_marksToFocus.clear();
                }
            }
        }
    }

    sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Mark] %s: clearmarks -> all bots", pPlayer->GetName());
    SendSysMessage("Mark assignments cleared for all bots.");
    return true;
}

// .partybot mark: hand out all CC marks fresh (one fixed mark per CC-capable bot; manual ccmark
// assignments are dropped). Focus marks are not touched (the skull is always a focus mark).
bool ChatHandler::HandlePartyBotMarkCommand(char* /*args*/)
{
    Player* pPlayer = GetSession()->GetPlayer();
    if (!pPlayer->GetGroup())
    {
        SendSysMessage("You are not in a group.");
        SetSentErrorMessage(true);
        return false;
    }

    PartyBotAI::AssignCCMarks(pPlayer, true, "mark command");
    SendSysMessage("CC marks handed out again (see party chat).");
    return true;
}

bool HandlePartyBotUseGObjectHelper(Player* pTarget, GameObject* pGo)
{
    if (pTarget->AI())
    {
        if (PartyBotAI* pAI = GetCommandedPartyBotAI(pTarget))
        {
            if (pTarget->IsWithinDist(pGo, INTERACTION_DISTANCE))
            {
                pGo->Use(pTarget);
                return true;
            }
        }
    }

    return false;
}

bool ChatHandler::HandlePartyBotUseGObjectCommand(char* args)
{
    Player* pPlayer = GetSession()->GetPlayer();
    Player* pTarget = GetSelectedPlayer();

    GameObject* pGo = getSelectedGameObject();
    if (!pGo)
    {
        SendSysMessage(LANG_COMMAND_NOGAMEOBJECTFOUND);
        return false;
    }

    bool ok = false;

    if (pTarget && pTarget != pPlayer)
    {
        if (ok = HandlePartyBotUseGObjectHelper(pTarget, pGo))
            PSendSysMessage("%s has used the object.", pTarget->GetName());
        else
            PSendSysMessage("%s is not in range or is not a party bot.", pTarget->GetName());
        return ok;
    }
    else if (Group* pGroup = pPlayer->GetGroup())
    {
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            if (Player* pMember = itr->getSource())
                ok = HandlePartyBotUseGObjectHelper(pMember, pGo) || ok;
        }

        if (ok)
            SendSysMessage("All party bots in range have used the object.");
        else
            SendSysMessage("There are no party bots in range of the object.");
        return ok;
    }

    SendSysMessage("You are not in a group.");
    SetSentErrorMessage(true);
    return false;
}

bool HandlePartyBotPauseApplyHelper(Player* pTarget, uint32 duration)
{
    if (pTarget->AI())
    {
        if (PartyBotAI* pAI = GetCommandedPartyBotAI(pTarget))
        {
            pAI->m_updateTimer.Reset(duration);

            if (duration)
            {
                pTarget->StopMoving();
                pTarget->GetMotionMaster()->MoveIdle();
            }

            return true;
        }
    }

    return false;
}

bool ChatHandler::HandlePartyBotPauseHelper(char* args, bool pause)
{
    bool all = false;
    uint32 duration = 0;
    if (char* arg1 = ExtractArg(&args))
    {
        if (!(all = (strcmp(arg1, "all") == 0)) && pause)
            duration = atoi(arg1);

        if (char* arg2 = ExtractArg(&args))
        {
            if (!duration && pause)
                duration = atoi(arg2);
            else if (!all)
                all = strcmp(arg2, "all") == 0;
        }
    }

    if (pause && !duration)
        duration = 5 * MINUTE * IN_MILLISECONDS;

    // A selected party bot always wins over "all" (same rule as the movement orders).
    if (all)
        if (Player* pTarget = GetSelectedPlayer())
            if (pTarget != GetSession()->GetPlayer() && dynamic_cast<PartyBotAI*>(pTarget->AI()))
                all = false;

    if (all)
    {
        Player* pPlayer = GetSession()->GetPlayer();
        Group* pGroup = pPlayer->GetGroup();
        if (!pGroup)
        {
            SendSysMessage("You are not in a group.");
            SetSentErrorMessage(true);
            return false;
        }

        bool success = false;
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            if (Player* pMember = itr->getSource())
            {
                if (pMember == pPlayer)
                    continue;

                if (HandlePartyBotPauseApplyHelper(pMember, duration))
                    success = true;
            }
        }

        if (success)
        {
            if (pause)
                PSendSysMessage("All party bots paused for %u seconds.", (duration / IN_MILLISECONDS));
            else
                SendSysMessage("All party bots unpaused.");
        }
        else
            SendSysMessage("No party bots in group.");
    }
    else
    {
        Player* pTarget = GetSelectedPlayer();
        if (!pTarget)
        {
            SendSysMessage(LANG_NO_CHAR_SELECTED);
            SetSentErrorMessage(true);
            return false;
        }

        if (HandlePartyBotPauseApplyHelper(pTarget, duration))
        {
            if (pause)
                PSendSysMessage("%s paused for %u seconds.", pTarget->GetName(), (duration / IN_MILLISECONDS));
            else
                PSendSysMessage("%s unpaused.", pTarget->GetName());
        }

        else
            SendSysMessage("Target is not a party bot.");
    }

    return true;
}

bool ChatHandler::HandlePartyBotPauseCommand(char* args)
{
    return HandlePartyBotPauseHelper(args, true);
}

bool ChatHandler::HandlePartyBotUnpauseCommand(char* args)
{
    return HandlePartyBotPauseHelper(args, false);
}

enum PartyBotMoveOrder : uint8
{
    PB_ORDER_STAY,
    PB_ORDER_FOLLOW,
    PB_ORDER_TOGGLE,
    PB_ORDER_COMESTAY,
};

static bool HandlePartyBotMoveOrderApplyHelper(Player* pTarget, Player const* pLeader, uint8 order, bool stacked)
{
    PartyBotAI* pAI = GetCommandedPartyBotAI(pTarget);
    if (!pAI)
        return false;

    switch (order)
    {
        case PB_ORDER_STAY:
            pAI->OrderStay();
            break;
        case PB_ORDER_FOLLOW:
            pAI->OrderFollow();
            break;
        case PB_ORDER_TOGGLE:
            pAI->OrderStackedFollow(stacked);
            break;
        case PB_ORDER_COMESTAY:
            pAI->OrderHoldSpot(pLeader->GetMapId(), pLeader->GetPositionX(), pLeader->GetPositionY(),
                pLeader->GetPositionZ());
            break;
    }

    return true;
}

bool ChatHandler::HandlePartyBotMoveOrderHelper(char* args, uint8 order)
{
    bool all = false;
    if (char* arg1 = ExtractArg(&args))
        all = strcmp(arg1, "all") == 0;

    Player* pPlayer = GetSession()->GetPlayer();
    std::vector<Player*> bots;

    // A selected party bot always wins over "all", so one macro serves both cases.
    Player* pTarget = GetSelectedPlayer();
    if (pTarget && pTarget != pPlayer && dynamic_cast<PartyBotAI*>(pTarget->AI()))
    {
        all = false;
        bots.push_back(pTarget);
    }
    else if (all)
    {
        Group* pGroup = pPlayer->GetGroup();
        if (!pGroup)
        {
            SendSysMessage("You are not in a group.");
            SetSentErrorMessage(true);
            return false;
        }

        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
            if (Player* pMember = itr->getSource())
                if (pMember != pPlayer && dynamic_cast<PartyBotAI*>(pMember->AI()))
                    bots.push_back(pMember);

        if (bots.empty())
        {
            SendSysMessage("No party bots in group.");
            return true;
        }
    }
    else if (!pTarget)
    {
        SendSysMessage(LANG_NO_CHAR_SELECTED);
        SetSentErrorMessage(true);
        return false;
    }
    else
    {
        SendSysMessage("Target is not a party bot.");
        return true;
    }

    // Toggle: if any of the commanded bots is not stacked yet, stack them all; otherwise unstack.
    bool stacked = false;
    if (order == PB_ORDER_TOGGLE)
        for (Player* pBot : bots)
            if (!static_cast<PartyBotAI*>(pBot->AI())->IsStackedFollowing())
                stacked = true;

    for (Player* pBot : bots)
        HandlePartyBotMoveOrderApplyHelper(pBot, pPlayer, order, stacked);

    char const* result;
    switch (order)
    {
        case PB_ORDER_STAY:
            result = "holding position";
            break;
        case PB_ORDER_FOLLOW:
            result = "coming to you, then following";
            break;
        case PB_ORDER_COMESTAY:
            result = "coming to your position and staying there";
            break;
        default:
            result = stacked ? "following stacked (toggle on)" : "following loosely (toggle off)";
            break;
    }

    if (all)
        PSendSysMessage("All party bots %s.", result);
    else
        PSendSysMessage("%s %s.", bots.front()->GetName(), result);

    return true;
}

bool ChatHandler::HandlePartyBotStayCommand(char* args)
{
    return HandlePartyBotMoveOrderHelper(args, PB_ORDER_STAY);
}

bool ChatHandler::HandlePartyBotFollowCommand(char* args)
{
    return HandlePartyBotMoveOrderHelper(args, PB_ORDER_FOLLOW);
}

bool ChatHandler::HandlePartyBotToggleCommand(char* args)
{
    return HandlePartyBotMoveOrderHelper(args, PB_ORDER_TOGGLE);
}

bool ChatHandler::HandlePartyBotComeStayCommand(char* args)
{
    return HandlePartyBotMoveOrderHelper(args, PB_ORDER_COMESTAY);
}

void PlayerBotMgr::SetMoveHereChoice(ObjectGuid leaderGuid, MoveHereChoice const& choice)
{
    std::lock_guard<std::mutex> guard(m_moveHereLock);
    m_moveHereChoices[leaderGuid] = choice;
}

static std::string ToLowerCopy(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

// Called when the leader casts aedm on the ground: the chosen party bots hold the clicked spot.
void PlayerBotMgr::OrderMoveHere(Player* pLeader, float x, float y, float z)
{
    MoveHereChoice choice;
    {
        std::lock_guard<std::mutex> guard(m_moveHereLock);
        auto itr = m_moveHereChoices.find(pLeader->GetObjectGuid());
        if (itr != m_moveHereChoices.end())
            choice = itr->second;
    }

    ChatHandler handler(pLeader->GetSession());
    Group* pGroup = pLeader->GetGroup();
    if (!pGroup)
    {
        handler.SendSysMessage("Move here: you are not in a group.");
        return;
    }

    std::string moved;
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || pMember == pLeader || !pMember->IsInMap(pLeader))
            continue;
        PartyBotAI* pAI = dynamic_cast<PartyBotAI*>(pMember->AI());
        if (!pAI)
            continue;

        std::string const name = ToLowerCopy(pMember->GetName());
        bool const chosen = choice.all ||
            (choice.roleMask & (1 << pAI->GetRole())) ||
            (choice.selected && pLeader->GetSelectionGuid() == pMember->GetObjectGuid()) ||
            std::find(choice.names.begin(), choice.names.end(), name) != choice.names.end();
        if (!chosen)
            continue;

        GetCommandedPartyBotAI(pMember)->OrderHoldSpot(pLeader->GetMapId(), x, y, z);
        moved += moved.empty() ? pMember->GetName() : std::string(", ") + pMember->GetName();
    }

    sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Order] %s: move here (%.1f, %.1f, %.1f) map %u -> %s",
        pLeader->GetName(), x, y, z, pLeader->GetMapId(), moved.empty() ? "nobody" : moved.c_str());
    if (moved.empty())
        handler.SendSysMessage("Move here: no chosen bot in your group (see .partybot moveto).");
    else
        handler.PSendSysMessage("Move here: %s.", moved.c_str());
}

// .partybot moveto <words>: who walks to the spot of the next aedm cast. Words (spaces or commas):
// all, tank, melee, ranged, healer, target (the bot selected at cast time), or bot names.
bool ChatHandler::HandlePartyBotMoveToCommand(char* args)
{
    Player* pPlayer = GetSession()->GetPlayer();
    PlayerBotMgr::MoveHereChoice choice;
    choice.all = false;

    std::string text = args ? args : "";
    std::replace(text.begin(), text.end(), ',', ' ');
    std::istringstream words(text);
    std::string word;
    std::string summary;
    while (words >> word)
    {
        std::string const lower = ToLowerCopy(word);
        if (lower == "all")
            choice.all = true;
        else if (lower == "tank" || lower == "tanks")
            choice.roleMask |= 1 << ROLE_TANK;
        else if (lower == "melee")
            choice.roleMask |= 1 << ROLE_MELEE_DPS;
        else if (lower == "ranged" || lower == "range")
            choice.roleMask |= 1 << ROLE_RANGE_DPS;
        else if (lower == "healer" || lower == "healers")
            choice.roleMask |= 1 << ROLE_HEALER;
        else if (lower == "target")
            choice.selected = true;
        else
        {
            bool found = false;
            if (Group* pGroup = pPlayer->GetGroup())
                for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
                    if (Player* pMember = itr->getSource())
                        if (dynamic_cast<PartyBotAI*>(pMember->AI()) && ToLowerCopy(pMember->GetName()) == lower)
                            found = true;
            if (!found)
            {
                PSendSysMessage("Unknown word '%s'. Use: all, tank, melee, ranged, healer, target, or a party bot's name.",
                    word.c_str());
                SetSentErrorMessage(true);
                return false;
            }
            choice.names.push_back(lower);
        }
        summary += summary.empty() ? lower : std::string(", ") + lower;
    }

    if (summary.empty())
    {
        SendSysMessage("Usage: .partybot moveto all|tank|melee|ranged|healer|target|<bot name> (combine with spaces or commas)");
        SetSentErrorMessage(true);
        return false;
    }

    sPlayerBotMgr.SetMoveHereChoice(pPlayer->GetObjectGuid(), choice);
    PSendSysMessage("Move here: %s. Cast aedm and click the spot.", summary.c_str());
    return true;
}

bool ChatHandler::HandlePartyBotPullCommand(char* args)
{
    return HandlePartyBotPullHelper(args, false);
}

// .partybot rpull [seconds]: like pull, but the tank shoots the target and waits for it.
bool ChatHandler::HandlePartyBotRangedPullCommand(char* args)
{
    return HandlePartyBotPullHelper(args, true);
}

bool ChatHandler::HandlePartyBotPullHelper(char* args, bool ranged)
{
    Player* pPlayer = GetSession()->GetPlayer();
    Unit* pTarget = GetSelectedUnit();
    if (!pTarget || !pPlayer->IsValidAttackTarget(pTarget, true))
    {
        SendSysMessage(LANG_SELECT_CHAR_OR_CREATURE);
        SetSentErrorMessage(true);
        return false;
    }

    Group* pGroup = pPlayer->GetGroup();
    if (!pGroup)
    {
        SendSysMessage("You are not in a group.");
        SetSentErrorMessage(true);
        return false;
    }

    // .partybot pull [seconds]: damage dealers wait this long (default 10 s).
    uint32 seconds;
    if (!ExtractUInt32(&args, seconds))
        seconds = 10;
    uint32 const duration = seconds * IN_MILLISECONDS;

    std::string tanks, paused, healerCC;
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        if (Player* pMember = itr->getSource())
        {
            if (pMember == pPlayer)
                continue;

            if (pMember->AI())
            {
                if (PartyBotAI* pAI = GetCommandedPartyBotAI(pMember))
                {
                    if (pAI->m_role == ROLE_MELEE_DPS || pAI->m_role == ROLE_RANGE_DPS)
                    {
                        pAI->StartPullWait(duration);
                        paused += paused.empty() ? pMember->GetName() : std::string(", ") + pMember->GetName();
                        continue;
                    }
                    else if (pAI->m_role == ROLE_TANK)
                    {
                        if (pMember->IsValidAttackTarget(pTarget))
                        {
                            pAI->OrderAttack(pTarget, false, ranged);
                            tanks += tanks.empty() ? pMember->GetName() : std::string(", ") + pMember->GetName();
                        }
                    }
                    else if (pAI->m_role == ROLE_HEALER && pAI->HasPendingCCMark())
                    {
                        // Healers never wait: they crowd-control their marks at once (19d).
                        pAI->StartHealerPullCC(duration);
                        healerCC += healerCC.empty() ? pMember->GetName() : std::string(", ") + pMember->GetName();
                    }
                }
            }
        }
    }

    sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Pull] %s: %s %s, tank %s, DPS paused %u s: %s, healer CC: %s",
        pPlayer->GetName(), ranged ? "ranged pull" : "pull", CombatBotBaseAI::GetLogName(pTarget).c_str(),
        tanks.empty() ? "none" : tanks.c_str(), seconds, paused.empty() ? "none" : paused.c_str(),
        healerCC.empty() ? "none" : healerCC.c_str());
    PSendSysMessage("Tank party bots are %s %s, DPS party bots are paused for %u seconds.",
        ranged ? "shooting to pull" : "pulling", pTarget->GetName(), seconds);
    return true;
}

bool ChatHandler::HandlePartyBotUnequipCommand(char* args)
{
    Player* pTarget = GetSelectedPlayer();
    if (!pTarget)
    {
        SendSysMessage(LANG_NO_CHAR_SELECTED);
        SetSentErrorMessage(true);
        return false;
    }

    char* cId = ExtractKeyFromLink(&args, "Hitem");
    if (!cId)
        return false;

    uint32 itemId = 0;
    if (!ExtractUInt32(&cId, itemId))
        return false;

    uint32 count = pTarget->GetItemCount(itemId, false);
    if (!count)
    {
        SendSysMessage("Target does not have that item.");
        SetSentErrorMessage(true);
        return false;
    }

    if (pTarget->AI())
    {
        if (PartyBotAI* pAI = GetCommandedPartyBotAI(pTarget))
        {
            pTarget->DestroyItemCount(itemId, count, true);
            return true;
        }
    }

    SendSysMessage("Target is not a party bot.");
    SetSentErrorMessage(true);
    return false;
}

bool ChatHandler::HandlePartyBotRemoveCommand(char* args)
{
    Player* pTarget = GetSelectedPlayer();
    if (!pTarget)
    {
        SendSysMessage(LANG_NO_CHAR_SELECTED);
        SetSentErrorMessage(true);
        return false;
    }

    if (pTarget->AI())
    {
        if (PartyBotAI* pAI = GetCommandedPartyBotAI(pTarget))
        {
            pAI->botEntry->requestRemoval = true;
            return true;
        }
    }

    SendSysMessage("Target is not a party bot.");
    SetSentErrorMessage(true);
    return false;
}

bool ChatHandler::HandleBattleBotAddAlteracCommand(char* args)
{
    return HandleBattleBotAddCommand(args, BATTLEGROUND_QUEUE_AV);
}

bool ChatHandler::HandleBattleBotAddArathiCommand(char* args)
{
    return HandleBattleBotAddCommand(args, BATTLEGROUND_QUEUE_AB);
}

bool ChatHandler::HandleBattleBotAddWarsongCommand(char* args)
{
    return HandleBattleBotAddCommand(args, BATTLEGROUND_QUEUE_WS);
}

bool ChatHandler::HandleBattleBotAddCommand(char* args, uint8 bg)
{
    if (!*args)
    {
        SendSysMessage("Incorrect syntax. Expected faction");
        SetSentErrorMessage(true);
        return false;
    }

    Team botTeam = HORDE;
    uint32 botLevel = sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL);
    bool isTemporary = false;
    std::string option;
    if (char* arg1 = ExtractArg(&args))
    {
        option = arg1;
        if (option == "horde")
            botTeam = HORDE;
        else if (option == "alliance")
            botTeam = ALLIANCE;
        else
        {
            SendSysMessage("Incorrect syntax. Expected faction");
            SetSentErrorMessage(true);
            return false;
        }


        ExtractUInt32(&args, botLevel);


        if (char* tempStr = ExtractArg(&args))
        {
            if (strcmp(tempStr, "temp") == 0)
                isTemporary = true;
        }
    }

    sPlayerBotMgr.AddBattleBot(BattleGroundQueueTypeId(bg), botTeam, botLevel, isTemporary);
    return true;
}

bool ChatHandler::HandleBattleBotRemoveCommand(char* args)
{
    Player* pTarget = GetSelectedPlayer();
    if (!pTarget)
    {
        SendSysMessage(LANG_NO_CHAR_SELECTED);
        SetSentErrorMessage(true);
        return false;
    }

    if (pTarget->AI())
    {
        if (BattleBotAI* pAI = dynamic_cast<BattleBotAI*>(pTarget->AI()))
        {
            pAI->botEntry->requestRemoval = true;
            return true;
        }
    }

    SendSysMessage("Target is not a battle bot.");
    SetSentErrorMessage(true);
    return false;
}

bool ChatHandler::HandleBattleBotRemoveAllCommand(char* args)
{
    sPlayerBotMgr.DeleteBattleBots();
    SendSysMessage("Removed all battlebots.");
    return true;
}

#define SPELL_RED_GLOW 20370

void ShowBattleBotPathHelper(Map* pMap, BattleBotPath* pPath, uint32 id)
{
    for (const auto& point : *pPath)
    {
        if (Creature* pWaypoint = pMap->SummonCreature(VISUAL_WAYPOINT, point.x, point.y, point.z, 0.0f, TEMPSUMMON_TIMED_DESPAWN, 120000, true))
        {
            // Show path id as level to distinguish individual paths.
            pWaypoint->SetUInt32Value(UNIT_FIELD_LEVEL, id);

            // Mark points that have script attached.
            if (point.pFunc)
                pWaypoint->CastSpell(pWaypoint, SPELL_RED_GLOW, true);
        }
    }
}

bool ChatHandler::HandleBattleBotShowPathCommand(char* args)
{
    Player* pTarget = GetSelectedPlayer();
    if (!pTarget)
    {
        SendSysMessage(LANG_NO_CHAR_SELECTED);
        SetSentErrorMessage(true);
        return false;
    }

    if (pTarget->AI())
    {
        if (BattleBotAI* pAI = dynamic_cast<BattleBotAI*>(pTarget->AI()))
        {
            if (pAI->m_currentPath)
                ShowBattleBotPathHelper(pTarget->GetMap(), pAI->m_currentPath, 1);
            else
                SendSysMessage("Target is not following a path.");

            return true;
        }
    }

    SendSysMessage("Target is not a battle bot.");
    SetSentErrorMessage(true);
    return false;
}

bool ChatHandler::HandleBattleBotShowAllPathsCommand(char* args)
{
    Player* pPlayer = m_session->GetPlayer();
    BattleGround* pBG = pPlayer->GetBattleGround();
    if (!pBG)
    {
        SendSysMessage("You are not in a battleground.");
        return false;
    }

    std::vector<BattleBotPath*> const* pPaths;

    switch (pBG->GetTypeID())
    {
        case BATTLEGROUND_AB:
        {
            pPaths = &vPaths_AB;
            break;
        }
        case BATTLEGROUND_AV:
        {
            pPaths = &vPaths_AV;
            break;
        }
        case BATTLEGROUND_WS:
        {
            pPaths = &vPaths_WS;
            break;
        }
        default:
            return false;
    }

    uint32 id = 1;
    for (const auto& path : *pPaths)
    {
        ShowBattleBotPathHelper(pPlayer->GetMap(), path, id++);
    }

    PSendSysMessage("Showing %u paths for battleground.", id);
    return true;
}
