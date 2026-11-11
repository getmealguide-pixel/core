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

// Area spells of damage dealers (.partybot aoe auto|on|off).
enum PartyBotAoEMode : uint8
{
    PB_AOE_AUTO,   // packs of PartyBot.AutoAoE.MinTargets+, never near crowd control
    PB_AOE_FORCED, // "on": any pack size, also near crowd control; back to auto after the fight
    PB_AOE_OFF,    // no damaging area spells
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
    // reason: why this target (for the [Target] log line).
    Unit* SelectAttackTarget(Player* pLeader, char const*& reason) const;
    Unit* SelectPartyAttackTarget() const;
    Player* SelectResurrectionTarget() const;
    Player* SelectShieldTarget() const;
    Unit* GetMarkedTarget(RaidTargetIcon mark) const;
    bool CanUseCrowdControl(SpellEntry const* pSpellEntry, Unit* pTarget) const;
    bool DrinkAndEat();
    bool ShouldAutoRevive() const;
    bool CrowdControlMarkedTargets();
    bool EnterCombatDruidForm();
    bool ShouldEnterStealth() const;
    bool EnterStealthIfNeeded(SpellEntry const* pStealthSpell);

    PartyBotLayer SelectLayer() const;
    void SetActiveLayer(PartyBotLayer layer);
    void WakeUp();
    char const* GetLayerName(PartyBotLayer layer) const;
    void OrderStay();
    // Player's attack/pull order: leaves stay/hold and attacks; the target beats the skull and
    // other marks until it dies or another order replaces it. overrideCC (attack, not pull): the
    // target may be a CC mark (the bot's own CC duty then skips it too). ranged (rpull): shoot the
    // target and wait for it instead of running in (melee pull without a ranged weapon).
    void OrderAttack(Unit* pTarget, bool overrideCC, bool ranged = false);
    bool UpdateRangedPull();
    void EndRangedPull(char const* why);
    // Ranged weapon (bow, gun, crossbow, thrown) of non-hunters: its shoot/throw spell (learned
    // when missing), and one shot with it (ammo added when needed).
    SpellEntry const* GetRangedWeaponSpell();
    bool ShootRangedWeapon(Unit* pTarget);
    // Walks until the enemy is within range and in sight (point movement), leaving a stay spot.
    // Returns true while walking. tag/what: for the log line.
    bool WalkIntoRange(Unit* pTarget, float range, char const* tag, char const* what);
    void ClearOrderedTarget();
    // Pull timer for damage dealers: they hold still until it ends; a bot with a CC mark first
    // walks into range of the marked enemy and crowd-controls it. Any new order ends the wait.
    void StartPullWait(uint32 duration);
    void EndPullWait(char const* why);
    bool IsPullWaiting() const { return m_pullWaitDuration != 0; }
    void UpdatePullWait();
    void OrderHoldSpot(uint32 mapId, float x, float y, float z);
    void OrderFollow();
    void OrderStackedFollow(bool stacked);
    bool IsStackedFollowing() const { return m_followMode == PB_FOLLOW_STACKED; }
    void SetFollowMode(PartyBotFollowMode mode);
    void UpdateFollowMode(Player const* pLeader);
    void StartFollowing(Player* pLeader);
    void ReactToLeaderMovement();
    void StandUpToFollow();
    void BreakActionsAndFollowNow();
    void BreakCurrentActions();
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
    // Target that beats any other: the ordered target, else the skull, else other focus marks
    // (marks only once they are fighting). reason: "ordered", "skull" or "focus mark".
    Unit* GetPreferredTarget(char const*& reason) const;
    Unit* GetFocusMarkTarget(char const*& reason) const;
    // A mark given to any bot of the group with ccmark: no bot attacks it unless ordered to, or
    // unless every bot holding the mark released it (could not crowd-control that enemy).
    bool IsForbiddenTarget(Unit const* pTarget) const;
    // Automatic CC mark per CC-capable bot (one fixed mark per class). reset: all CC marks are
    // handed out fresh (.partybot mark); otherwise manual ccmark assignments are kept. Announced
    // in party chat and logged when the assignment changed (always on reset).
    static void AssignCCMarks(Player* pLeader, bool reset, char const* why);
    bool HasCrowdControlSpell() const;
    SpellEntry const* GetCrowdControlSpellFor(Unit const* pTarget) const;
    // The bot cannot crowd-control this enemy (creature type or immunity): it says so once in
    // party chat and the enemy is attacked normally.
    void ReleaseCCMark(Unit* pTarget, SpellEntry const* pSpellEntry, char const* why);
    void SayInParty(std::string const& text) const;
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

    // Area spells. Damage area spells only (no fears, roots, shouts or taunts); the first one in
    // range that the casting rules allow is cast at the target (ground spells land on its spot).
    static bool IsDamageAreaSpell(SpellEntry const* pSpellEntry);
    bool CastAreaOfEffectSpell(Unit* pTarget);
    // A living enemy with any bot's CC mark within PB_AOE_CC_MARK_RANGE of this bot.
    Unit* GetCCMarkedEnemyNearby() const;
    void SetAoEMode(PartyBotAoEMode mode, char const* why);
    void EndForcedAoE(char const* why);
    void UpdateForcedAoE();

    void MaintainHunterPet();
    // Task 16b: mage own mana gem (kept out of combat, used in combat).
    void KeepManaGem();
    bool UseManaGem();
    void CommandPetAttack(Unit* pVictim);

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
    bool m_ccMarksAuto = false;                  // m_marksToCC was handed out by AssignCCMarks
    std::set<ObjectGuid> m_releasedCC;           // CC-marked enemies this bot could not crowd-control
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
    bool m_wasInCombat = false;
    bool m_returningToHoldSpot = false;
    uint32 m_returnProgressTime = 0;
    float m_returnBestDistance = 0.0f;
    uint32 m_holdMapId = 0;
    float m_holdX = 0.0f;
    float m_holdY = 0.0f;
    float m_holdZ = 0.0f;
    bool m_tankSlotManual = false;               // slot set by command (else automatic)
    uint8 m_tankSlotChoice = PB_TANK_SLOT_NONE;  // the manual slot
    bool m_singleFocus = false;                  // tank only: stays on its preferred target, ignores loose enemies
    ObjectGuid m_orderedTargetGuid;              // target of the last attack/pull order
    uint32 m_moveOrderTime = 0;                  // when the last move order came (for the reaction log)
    uint32 m_pullWaitStart = 0;                  // pull timer start (ms)
    uint32 m_pullWaitDuration = 0;               // pull timer length (ms), 0 = not waiting
    ObjectGuid m_approachGuid;                   // enemy this bot walks towards (CC, ranged pull)
    ObjectGuid m_rangedPullGuid;                 // rpull target until it arrives
    uint32 m_rangedPullStart = 0;                // when the rpull order came
    uint32 m_rangedPullShotTime = 0;             // when the rpull shot left, 0 = not yet
    bool m_orderOverridesCC = false;             // attack order (not pull) may hit a CC mark
    PartyBotAoEMode m_aoeMode = PB_AOE_AUTO;     // damage dealers' area spells
    bool m_forcedAoEFought = false;              // forced AoE: the bot has fought since it was set
    uint8 m_loggedTankSlot = PB_TANK_SLOT_NONE;
    mutable PartyBotTankSlotMap m_tankSlotCache;
    mutable uint32 m_tankSlotCacheTime = 0;
    PartyBotLayer m_activeLayer = PB_LAYER_NONE;
    char const* m_activeLayerName = "None";
};

#endif
