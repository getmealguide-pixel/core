/*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#ifndef MANGOS_PARTYBOTAI_H
#define MANGOS_PARTYBOTAI_H

#include "CombatBotBaseAI.h"
#include "Group.h"
#include "ObjectAccessor.h"

// Brain layers, highest priority first. Each update the top layer that wants control
// becomes active; higher layers may also restrict lower ones (e.g. "stay" forbids movement
// while role duty still heals and fights).
enum PartyBotLayer : uint8
{
    PB_LAYER_NONE,
    PB_LAYER_COMMAND,   // player orders (stay)
    PB_LAYER_ROLE_DUTY, // class/role logic: tank, heal, dps
    PB_LAYER_FOLLOW,    // follow the leader
};

// How the follow layer follows the leader.
enum PartyBotFollowMode : uint8
{
    PB_FOLLOW_LOOSE,   // spread out 3-6 yd around the leader
    PB_FOLLOW_ARRIVE,  // run to the leader and stack; turns loose once the leader moves
    PB_FOLLOW_STACKED, // "toggle": stay stacked on the leader all the time
};

// Tank slots: main tank and numbered off-tanks. Everyone else has PB_TANK_SLOT_NONE.
enum PartyBotTankSlot : uint8
{
    PB_TANK_SLOT_NONE = 0,
    PB_TANK_SLOT_MT   = 1,
    PB_TANK_SLOT_OT1  = 2,  // OTn = PB_TANK_SLOT_OT1 + n - 1
    PB_TANK_SLOT_MAX  = 11, // OT1..OT9
};

typedef std::map<ObjectGuid, uint8> PartyBotTankSlotMap;

class PartyBotAI : public CombatBotBaseAI
{
public:

    PartyBotAI(Player* pLeader, Player* pClone, CombatBotRoles role, uint8 race, uint8 class_, uint8 level, uint32 mapId, uint32 instanceId, float x, float y, float z, float o)
        : CombatBotBaseAI(), m_race(race), m_class(class_), m_level(level), m_mapId(mapId), m_instanceId(instanceId), m_x(x), m_y(y), m_z(z), m_o(o)
    {
        m_role = role;
        m_leaderGuid = pLeader->GetObjectGuid();
        m_cloneGuid = pClone ? pClone->GetObjectGuid() : ObjectGuid();
        m_updateTimer.Reset(2000);
    }
    PartyBotAI(Player* pLeader, uint32 mapId, uint32 instanceId, float x, float y, float z, float o)
        : CombatBotBaseAI(), m_mapId(mapId), m_instanceId(instanceId), m_x(x), m_y(y), m_z(z), m_o(o)
    {
        m_role = ROLE_INVALID;
        m_leaderGuid = pLeader->GetObjectGuid();
        m_updateTimer.Reset(2000);
    }

    bool OnSessionLoaded(PlayerBotEntry* entry, WorldSession* sess) final;
    void OnPlayerLogin() final;
    void UpdateAI(uint32 const diff) final;
    void OnPacketReceived(WorldPacket const* packet) final;

    void CloneFromPlayer(Player const* pPlayer);
    void AddToPlayerGroup();

    bool CanTryToCastSpell(Unit const* pTarget, SpellEntry const* pSpellEntry) const final;
    Player* GetPartyLeader() const;
    bool AttackStart(Unit* pVictim);
    Unit* SelectAttackTarget(Player* pLeader) const;
    Unit* SelectPartyAttackTarget() const;
    Player* SelectResurrectionTarget() const;
    Player* SelectShieldTarget() const;
    Unit* GetMarkedTarget(RaidTargetIcon mark) const;
    bool CanUseCrowdControl(SpellEntry const* pSpellEntry, Unit* pTarget) const;
    bool DrinkAndEat();
    bool ShouldAutoRevive() const;
    bool IsValidDistancingTarget(Unit* pTarget, Unit* pEnemy);
    Unit* GetDistancingTarget(Unit* pEnemy);
    bool RunAwayFromTarget(Unit* pEnemy);
    bool CrowdControlMarkedTargets();
    bool EnterCombatDruidForm();
    bool ShouldEnterStealth() const;
    bool EnterStealthIfNeeded(SpellEntry const* pStealthSpell);

    PartyBotLayer SelectLayer() const;
    void SetActiveLayer(PartyBotLayer layer);
    void WakeUp();
    char const* GetLayerName(PartyBotLayer layer) const;
    void OrderStay();
    void OrderComeStay(Player const* pLeader);
    void OrderFollow();
    void OrderStackedFollow(bool stacked);
    bool IsStackedFollowing() const { return m_followMode == PB_FOLLOW_STACKED; }
    void SetFollowMode(PartyBotFollowMode mode);
    void UpdateFollowMode(Player const* pLeader);
    void StartFollowing(Player* pLeader);
    void ReactToLeaderMovement();
    void StandUpToFollow();
    void BreakActionsAndFollowNow();
    void FaceCombatTarget();
    void SetHoldPosition(bool hold);
    void SetHoldSpotHere();
    bool ReturnToHoldSpot();
    // Stay and toggle both forbid moving on its own (no chasing, running away, movement spells).
    bool CanMoveOnOwn() const final { return !m_holdPosition && m_followMode != PB_FOLLOW_STACKED; }

    // Tank slots. Manual choices win; then the leader's tanking players and tank bots fill
    // MT, OT1, OT2... in group order. Players' manual choices live in a shared table (not bots).
    static void ResolveTankSlots(Group* pGroup, PartyBotTankSlotMap& slots);
    static std::string GetTankSlotName(uint8 slot);
    static bool GetPlayerTankSlotChoice(ObjectGuid guid, uint8& slot);
    static void SetPlayerTankSlotChoice(ObjectGuid guid, bool manual, uint8 slot);
    static void ClearManualTankSlot(Group* pGroup, uint8 slot);
    PartyBotTankSlotMap const& GetTankSlots() const;
    static uint8 GetSlotOf(PartyBotTankSlotMap const& slots, Unit const* pUnit);
    uint8 GetHealPriority(Unit const* pUnit) const final;
    Unit* GetFocusMarkTarget() const;
    Unit* SelectLooseEnemy(PartyBotTankSlotMap const& slots) const;
    bool TauntIfAllowed(Unit* pEnemy, PartyBotTankSlotMap const& slots, bool otherTanks);
    bool UpdateTankDuty();
    // Threat on every enemy: threat lead on an enemy, and switching to the least safe enemy
    // on this tank once the current target is safe (tab-tanking).
    float GetThreatLead(Unit* pEnemy, bool withFloor) const;
    Unit* TabForThreat(Unit* pVictim);

    // Ding sync (temporary .partybot add bots only): match the leader's level, then learn
    // trainer spells and talents of the bot's role.
    bool IsLevelSynced() const { return m_race && m_class && m_cloneGuid.IsEmpty(); }
    void SyncLevelWithLeader(Player const* pLeader);
    void LearnSpellsAndTalentsForLevel(bool resetTalents);
    bool LearnRoleSpecTalents();
    uint32 GetMainTalentTab() const;

    // Casts the first usable hostile area spell at the target. forced (.partybot aoe): no
    // usability checks and interrupts a non-area cast; otherwise the normal casting rules apply.
    bool CastAreaOfEffectSpell(Unit* pTarget, bool forced);

    bool CheckForDispelTargets();
    void UpdateInCombatAI() final;
    void UpdateOutOfCombatAI() final;
    void UpdateInCombatAI_Paladin() final;
    void UpdateOutOfCombatAI_Paladin() final;
    void UpdateInCombatAI_Shaman() final;
    void UpdateOutOfCombatAI_Shaman() final;
    void UpdateInCombatAI_Hunter() final;
    void UpdateOutOfCombatAI_Hunter() final;
    void UpdateInCombatAI_Mage() final;
    void UpdateOutOfCombatAI_Mage() final;
    void UpdateInCombatAI_Priest() final;
    void UpdateOutOfCombatAI_Priest() final;
    void UpdateInCombatAI_Warlock() final;
    void UpdateOutOfCombatAI_Warlock() final;
    void UpdateInCombatAI_Warrior() final;
    void UpdateOutOfCombatAI_Warrior() final;
    void UpdateInCombatAI_Rogue() final;
    void UpdateOutOfCombatAI_Rogue() final;
    void UpdateInCombatAI_Druid() final;
    void UpdateOutOfCombatAI_Druid() final;

    std::vector<RaidTargetIcon> m_marksToCC;
    std::vector<RaidTargetIcon> m_marksToFocus;
    ShortTimeTracker m_updateTimer;
    ObjectGuid m_leaderGuid;
    ObjectGuid m_cloneGuid;
    uint8 m_race = 0;
    uint8 m_class = 0;
    uint8 m_level = 0;
    uint8 m_gender = GENDER_NONE; // requested gender of a new bot (GENDER_NONE = random)
    uint32 m_mapId = 0;
    uint32 m_instanceId = 0;
    float m_x = 0.0f;
    float m_y = 0.0f;
    float m_z = 0.0f;
    float m_o = 0.0f;
    bool m_resetSpellData = false;
    bool m_holdPosition = false;
    PartyBotFollowMode m_followMode = PB_FOLLOW_LOOSE;
    bool m_arrivedAtLeader = false;
    bool m_leaderWasMoving = false;
    bool m_returningToHoldSpot = false;
    uint32 m_returnProgressTime = 0;
    float m_returnBestDistance = 0.0f;
    uint32 m_holdMapId = 0;
    float m_holdX = 0.0f;
    float m_holdY = 0.0f;
    float m_holdZ = 0.0f;
    bool m_tankSlotManual = false;               // slot set by command (else automatic)
    uint8 m_tankSlotChoice = PB_TANK_SLOT_NONE;  // the manual slot
    bool m_singleFocus = false;                  // tank only its focus mark, ignore loose enemies
    bool m_autoAoE = true;                       // damage dealers use area spells on big packs
    uint8 m_loggedTankSlot = PB_TANK_SLOT_NONE;
    mutable PartyBotTankSlotMap m_tankSlotCache;
    mutable uint32 m_tankSlotCacheTime = 0;
    PartyBotLayer m_activeLayer = PB_LAYER_NONE;
    char const* m_activeLayerName = "None";
};

#endif
