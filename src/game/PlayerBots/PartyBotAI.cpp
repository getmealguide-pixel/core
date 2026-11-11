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

#include "PartyBotAI.h"
#include "Player.h"
#include "CreatureAI.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "PlayerBotMgr.h"
#include "Opcodes.h"
#include "World.h"
#include "WorldPacket.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "Chat.h"
#include "Utilities/Random.h"

#include <mutex>
#include <random>

// Time between two AI updates (config PartyBot.UpdateInterval).
static uint32 GetUpdateInterval()
{
    return sWorld.getConfig(CONFIG_UINT32_PARTY_BOT_UPDATE_INTERVAL);
}

// Radius of an area spell (first effect that has one; 10 yd if none).
static float GetAreaRadius(SpellEntry const* pSpellEntry)
{
    for (auto radiusIndex : pSpellEntry->EffectRadiusIndex)
        if (radiusIndex)
            return Spells::GetSpellRadius(sSpellRadiusStore.LookupEntry(radiusIndex));
    return 10.0f;
}

// Self-centred spells (Arcane Explosion, Whirlwind) hit around the caster, others around the target
// (ground spells land on the target's position).
static Unit const* GetAreaCenter(Unit const* pCaster, Unit const* pTarget, SpellEntry const* pSpellEntry)
{
    return pSpellEntry->rangeIndex == SPELL_RANGE_IDX_SELF_ONLY ? pCaster : pTarget;
}

// Free eating/drinking: the effect of vendor food/drink for the bot's level (minimum level of the
// food, food spell, drink spell). Was one fixed level-45 effect for every level.
static struct { uint32 minLevel, food, drink; } const PB_FOOD_DRINK[] =
{
    {  1,   433,   430 },
    {  5,   434,   431 },
    { 15,   435,   432 },
    { 25,  1127,  1133 },
    { 35,  1129,  1135 },
    { 45,  1131,  1137 },
    { 55, 29073, 22734 }, // Conjured Cinnamon Roll / Conjured Crystal Water
};

static uint32 GetFoodSpellId(Player const* pPlayer, bool drink)
{
    uint32 spellId = 0;
    for (auto const& entry : PB_FOOD_DRINK)
        if (pPlayer->GetLevel() >= entry.minLevel)
            spellId = drink ? entry.drink : entry.food;
    return spellId;
}

static bool IsEating(Player const* pPlayer)
{
    for (auto const& entry : PB_FOOD_DRINK)
        if (pPlayer->HasAura(entry.food))
            return true;
    return false;
}

static bool IsDrinking(Player const* pPlayer)
{
    for (auto const& entry : PB_FOOD_DRINK)
        if (pPlayer->HasAura(entry.drink))
            return true;
    return false;
}

static void StopEatingDrinking(Player* pPlayer)
{
    for (auto const& entry : PB_FOOD_DRINK)
    {
        pPlayer->RemoveAurasDueToSpell(entry.food);
        pPlayer->RemoveAurasDueToSpell(entry.drink);
    }
}

enum PartyBotSpells
{
    PB_SPELL_AUTO_SHOT = 75,
    PB_SPELL_SHOOT_WAND = 5019,
    PB_SPELL_SHOOT_BOW = 2480,
    PB_SPELL_SHOOT_GUN = 7918,
    PB_SPELL_SHOOT_CROSSBOW = 7919,
    PB_SPELL_THROW = 2764,
    PB_SPELL_HONORLESS_TARGET = 2479,
};

#define PB_MIN_FOLLOW_DIST 3.0f
#define PB_MAX_FOLLOW_DIST 6.0f
#define PB_MIN_FOLLOW_ANGLE 0.0f
#define PB_MAX_FOLLOW_ANGLE 6.0f
#define PB_STACK_DIST 1.5f
#define PB_RANGED_PULL_WAIT (6 * IN_MILLISECONDS)     // rpull: wait this long after the shot for the target
#define PB_RANGED_PULL_TIMEOUT (10 * IN_MILLISECONDS) // rpull: melee pull if no shot left by then
#define PB_HOLD_SPOT_TOLERANCE 2.0f
#define PB_HOLD_NO_PROGRESS_TIMEOUT (5 * IN_MILLISECONDS)
#define PB_EAT_DRINK_START_PCT 70.0f // out of combat, sit down below this health/mana %
#define PB_AUTO_AOE_RADIUS 10.0f     // pack size = enemies within this of the target (incl. the target)
#define PB_AOE_CC_MARK_RANGE 40.0f   // no auto AoE while a CC-marked enemy is this close
#define PB_TANK_SAFE_LEAD 1.5f       // tank threat >= this x anyone else's = enemy is safe
#define PB_TANK_DANGER_LEAD 1.15f    // tank threat below this x someone else's = about to switch
#define PB_TANK_THREAT_FLOOR 10.0f
#define PB_SUNDER_REFRESH_MS 5000
#define PB_SHIELD_BLOCK_HEALTH_PCT 60.0f // tank Shield Block when below this health...
#define PB_SHIELD_BLOCK_SPARE_RAGE 400   // ...or with this much rage (x10) to spare    // re-apply a full Sunder Armor stack with this much time left   // per tank level: threat counted as "anyone else" at minimum

bool PartyBotAI::OnSessionLoaded(PlayerBotEntry* entry, WorldSession* sess)
{
    if (!m_race && !m_class)
    {
        sess->LoginPlayer(entry->playerGUID);
        return true;
    }

    return SpawnNewPlayer(sess, m_class, m_race, m_mapId, m_instanceId, m_x, m_y, m_z, m_o, sObjectAccessor.FindPlayer(m_cloneGuid), m_gender);
}

void PartyBotAI::CloneFromPlayer(Player const* pPlayer)
{
    if (!pPlayer)
        return;

    if (pPlayer->GetLevel() != me->GetLevel())
    {
        me->GiveLevel(pPlayer->GetLevel());
        me->InitTalentForLevel();
        me->SetUInt32Value(PLAYER_XP, 0);
    }

    // Learn all of the target's spells.
    for (const auto& spell : pPlayer->GetSpellMap())
    {
        if (spell.second.disabled)
            continue;

        if (spell.second.state == PLAYERSPELL_REMOVED)
            continue;

        SpellEntry const* pSpellEntry = sSpellMgr.GetSpellEntry(spell.first);
        if (!pSpellEntry)
            continue;

        uint32 const firstRankId = sSpellMgr.GetFirstSpellInChain(spell.first);
        if (!me->HasSpell(spell.first))
            me->LearnSpell(spell.first, false, (firstRankId == spell.first && GetTalentSpellPos(firstRankId)));
    }

    me->GetHonorMgr().SetHighestRank(pPlayer->GetHonorMgr().GetHighestRank());
    me->GetHonorMgr().SetRank(pPlayer->GetHonorMgr().GetRank());

    // Unequip current gear
    for (int i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
        me->AutoUnequipItemFromSlot(i);

    // Copy gear from target.
    for (int i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
    {
        if (Item* pItem = pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            me->SatisfyItemRequirements(pItem->GetProto());
            me->StoreNewItemInBestSlots(pItem->GetEntry(), 1, pItem->GetEnchantmentId(EnchantmentSlot(0)));
        }
    }
}

Player* PartyBotAI::GetPartyLeader() const
{
    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return nullptr;

    if (Player* originalLeader = ObjectAccessor::FindPlayerNotInWorld(m_leaderGuid))
    {
        if (me->InBattleGround() == originalLeader->InBattleGround())
        {
            // In case the original spawner is not in the same group as the bots anymore.
            if (pGroup != originalLeader->GetGroup())
                return nullptr;

            // In case the current leader is the bot itself and it's not inside a Battleground.
            ObjectGuid currentLeaderGuid = pGroup->GetLeaderGuid();
            if (currentLeaderGuid == me->GetObjectGuid() && !me->InBattleGround())
                return nullptr;
        }

        return originalLeader;
    }
    return nullptr;
}

bool PartyBotAI::DrinkAndEat()
{
    if (m_isBuffing)
        return false;

    if (me->GetVictim())
        return false;

    // Like a player: sit down only when clearly low; once sitting, eat/drink up to full.
    bool const isEating = IsEating(me);
    bool const isDrinking = IsDrinking(me);
    bool const isSitting = isEating || isDrinking;
    float const healthPct = me->GetHealthPercent();
    float const manaPct = me->GetPowerType() == POWER_MANA ? me->GetPowerPercent(POWER_MANA) : 100.0f;

    bool const needToEat = healthPct < 100.0f && (isSitting || healthPct < PB_EAT_DRINK_START_PCT);
    bool const needToDrink = manaPct < 100.0f && (isSitting || manaPct < PB_EAT_DRINK_START_PCT);

    if (!needToEat && !needToDrink)
        return false;

    if (!isSitting)
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Brain] %s: sits down to %s (health %.0f%%, mana %.0f%%, food %u, drink %u)",
            me->GetName(), needToDrink ? (needToEat ? "eat and drink" : "drink") : "eat", healthPct, manaPct,
            GetFoodSpellId(me, false), GetFoodSpellId(me, true));

    // Mage: Evocation first (when ready), drink afterwards for what is still missing.
    if (me->GetClass() == CLASS_MAGE && !isSitting && manaPct < PB_EAT_DRINK_START_PCT &&
        m_spells.mage.pEvocation && CanTryToCastSpell(me, m_spells.mage.pEvocation))
    {
        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
        {
            me->StopMoving();
            me->GetMotionMaster()->Clear(false, true);
            me->GetMotionMaster()->MoveIdle();
        }
        if (DoCastSpell(me, m_spells.mage.pEvocation) == SPELL_CAST_OK)
        {
            sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Supply] %s: Evocation before drinking (mana %.0f%%)",
                me->GetName(), manaPct);
            return true;
        }
    }

    if (!isEating && needToEat)
    {
        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
        {
            me->StopMoving();
            me->GetMotionMaster()->Clear(false, true);
            me->GetMotionMaster()->MoveIdle();
        }
        if (SpellEntry const* pSpellEntry = sSpellMgr.GetSpellEntry(GetFoodSpellId(me, false)))
        {
            me->CastSpell(me, pSpellEntry, true);
            me->RemoveSpellCooldown(pSpellEntry);
        }
        return true;
    }

    if (!isDrinking && needToDrink)
    {
        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
        {
            me->StopMoving();
            me->GetMotionMaster()->Clear(false, true);
            me->GetMotionMaster()->MoveIdle();
        }
        if (SpellEntry const* pSpellEntry = sSpellMgr.GetSpellEntry(GetFoodSpellId(me, true)))
        {
            me->CastSpell(me, pSpellEntry, true);
            me->RemoveSpellCooldown(pSpellEntry);
        }
        return true;
    }

    return needToEat || needToDrink;
}

bool PartyBotAI::ShouldAutoRevive() const
{
    if (me->GetDeathState() == DEAD)
        return true;

    bool alivePlayerNearby = false;
    Group* pGroup = me->GetGroup();
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        if (Player* pMember = itr->getSource())
        {
            if (pMember == me)
                continue;

            if (pMember->IsInCombat())
                return false;

            if (pMember->IsAlive())
            {
                if (IsHealerClass(pMember->GetClass()))
                    return false;

                if (me->IsWithinDistInMap(pMember, 15.0f))
                    alivePlayerNearby = true;
            }
        }
    }

    return alivePlayerNearby;
}

bool PartyBotAI::CanTryToCastSpell(Unit const* pTarget, SpellEntry const* pSpellEntry) const
{
    if (!CombatBotBaseAI::CanTryToCastSpell(pTarget, pSpellEntry))
        return false;

    // Holding position or stacked: no spells that move the caster (Charge, Intercept, Feral Charge, Blink).
    if (!CanMoveOnOwn() &&
       (pSpellEntry->HasEffect(SPELL_EFFECT_CHARGE) || pSpellEntry->HasEffect(SPELL_EFFECT_LEAP)))
        return false;

    // Following a moving leader (toggle: always; other follow modes: out of combat and no
    // target): only instant spells, a cast would stop the bot. A bot with a target fights.
    if (!m_holdPosition && (pSpellEntry->IsChanneledSpell() || pSpellEntry->GetCastTime(me) > 0) &&
       (m_followMode == PB_FOLLOW_STACKED || (!me->IsInCombat() && !me->GetVictim())))
    {
        Player const* pLeader = GetPartyLeader();
        if (me->IsMoving() || (pLeader && pLeader->IsMoving()))
            return false;
    }

    if (pSpellEntry->IsAreaOfEffectSpell() && !pSpellEntry->IsPositiveSpell() && !IsInDuel())
    {
        // AoE is free to use (the tank tab-tanks and catches up). Rules: never break crowd control
        // (forced AoE of damage dealers excepted), never pull enemies that are not fighting yet
        // (weak ones excepted). Damage dealers with aoe off cast no damaging area spells.
        bool const dps = m_role == ROLE_MELEE_DPS || m_role == ROLE_RANGE_DPS;
        if (dps && m_aoeMode == PB_AOE_OFF && IsDamageAreaSpell(pSpellEntry))
            return false;
        bool const forced = dps && m_aoeMode == PB_AOE_FORCED;

        // Not near a CC-marked enemy, even before its crowd control lands (tanks: radius rule only).
        if (!forced && m_role != ROLE_TANK && GetCCMarkedEnemyNearby())
            return false;

        std::list<Unit*> targets;
        me->GetEnemyListInRadiusAround(GetAreaCenter(me, pTarget, pSpellEntry), GetAreaRadius(pSpellEntry), targets);

        for (auto const& pEnemy : targets)
        {
            if (!forced && pEnemy->HasBreakableByDamageCrowdControlAura())
                return false;

            if (!pEnemy->IsInCombat() && (pEnemy->GetLevel() + 5) > me->GetLevel() &&
                pEnemy->IsValidAttackTarget(me))
                return false;
        }
    }

    return true;
}

bool PartyBotAI::CanUseCrowdControl(SpellEntry const* pSpellEntry, Unit* pTarget) const
{
    if (IsInDuel())
        return true;

    if (pSpellEntry->HasAuraInterruptFlag(AURA_INTERRUPT_DAMAGE_CANCELS) &&
        AreOthersOnSameTarget(pTarget->GetObjectGuid()))
        return false;

    if (pSpellEntry->HasSingleTargetAura())
    {
        auto const& singleAuras = me->GetSingleCastSpellTargets();
        if (singleAuras.find(pSpellEntry) != singleAuras.end())
            return false;
    }

    return true;
}

bool PartyBotAI::AttackStart(Unit* pVictim)
{
    m_isBuffing = false;

    if (me->IsMounted())
        me->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);

    if (me->Attack(pVictim, true))
    {
        BeginChasing(pVictim);
        return true;
    }

    return false;
}

Unit* PartyBotAI::GetMarkedTarget(RaidTargetIcon mark) const
{
    ObjectGuid targetGuid = me->GetGroup()->GetTargetWithIcon(mark);
    if (targetGuid.IsUnit())
        return me->GetMap()->GetUnit(targetGuid);

    return nullptr;
}

Unit* PartyBotAI::SelectAttackTarget(Player* pLeader, char const*& reason) const
{
    if (IsInDuel())
    {
        if (me->m_duel->opponent && IsValidHostileTarget(me->m_duel->opponent))
        {
            reason = "duel";
            return me->m_duel->opponent;
        }
    }
    else
    {
        // Ordered target, skull, focus marks.
        if (Unit* pPreferred = GetPreferredTarget(reason))
            return pPreferred;

        // Who is the leader attacking.
        if (Unit* pVictim = pLeader->GetVictim())
        {
            if (IsValidHostileTarget(pVictim) && !IsForbiddenTarget(pVictim))
            {
                reason = "leader's target";
                return pVictim;
            }
        }
    }

    // Who is attacking me.
    for (const auto pAttacker : me->GetAttackers())
    {
        if (IsValidHostileTarget(pAttacker) && !IsForbiddenTarget(pAttacker))
        {
            reason = "attacks me";
            return pAttacker;
        }
    }

    if (!IsInDuel())
    {
        // Check if other group members are under attack.
        if (Unit* pPartyAttacker = SelectPartyAttackTarget())
        {
            reason = "attacks a group member";
            return pPartyAttacker;
        }
    }

    // Assist pet if its in combat.
    if (Pet* pPet = me->GetPet())
    {
        if (Unit* pPetAttacker = pPet->GetAttackerForHelper())
            if (IsValidHostileTarget(pPetAttacker) && !IsForbiddenTarget(pPetAttacker))
            {
                reason = "attacks my pet";
                return pPetAttacker;
            }
    }

    return nullptr;
}

Unit* PartyBotAI::SelectPartyAttackTarget() const
{
    Group* pGroup = me->GetGroup();
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        if (Player* pMember = itr->getSource())
        {
            // We already checked self.
            if (pMember == me)
                continue;

            for (const auto pAttacker : pMember->GetAttackers())
            {
                if (IsValidHostileTarget(pAttacker) && !IsForbiddenTarget(pAttacker) &&
                    me->IsWithinDist(pAttacker, 50.0f))
                    return pAttacker;
            }
        }
    }

    return nullptr;
}

Player* PartyBotAI::SelectResurrectionTarget() const
{
    if (IsInDuel())
        return nullptr;

    Group* pGroup = me->GetGroup();
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        if (Player* pMember = itr->getSource())
        {
            // Can't resurrect self.
            if (pMember == me)
                continue;

            if (pMember->GetDeathState() != CORPSE)
                continue;

            if (!me->IsWithinLOSInMap(pMember))
                continue;

            if (m_resurrectionSpell->IsTargetInRange(me, pMember))
                return pMember;
        }
    }

    return nullptr;
}

Player* PartyBotAI::SelectShieldTarget() const
{
    if (IsInDuel())
        return nullptr;

    Group* pGroup = me->GetGroup();
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        if (Player* pMember = itr->getSource())
        {
            // We already checked self.
            if (pMember == me)
                continue;

            if ((pMember->GetHealthPercent() < 90.0f) &&
                !pMember->GetAttackers().empty() &&
                !pMember->IsImmuneToMechanic(MECHANIC_SHIELD))
                return pMember;
        }
    }

    return nullptr;
}

bool PartyBotAI::WalkIntoRange(Unit* pTarget, float range, char const* tag, char const* what)
{
    bool const inRange = me->IsWithinDistInMap(pTarget, range - 2.0f);
    bool const inSight = me->IsWithinLOSInMap(pTarget);
    if (inRange && inSight)
    {
        if (m_approachGuid == pTarget->GetObjectGuid())
        {
            m_approachGuid.Clear();
            me->StopMoving();
            me->GetMotionMaster()->Clear(false, true);
            me->GetMotionMaster()->MoveIdle();
        }
        return false;
    }

    // A point on the line to the target, well inside range (closer when sight is blocked).
    // Chase movement cannot be used: it stops when the target is not the bot's victim.
    float const wanted = inSight ? range - 8.0f : 5.0f;
    float const distance = me->GetDistance(pTarget);
    float const ratio = distance > wanted ? (distance - wanted) / distance : 0.0f;
    float const x = me->GetPositionX() + (pTarget->GetPositionX() - me->GetPositionX()) * ratio;
    float const y = me->GetPositionY() + (pTarget->GetPositionY() - me->GetPositionY()) * ratio;
    float const z = me->GetPositionZ() + (pTarget->GetPositionZ() - me->GetPositionZ()) * ratio;

    if (m_approachGuid != pTarget->GetObjectGuid())
    {
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[%s] %s: walks into %s range of %s (%.0f yd away%s)", tag,
            me->GetName(), what, pTarget->GetName(), distance, inSight ? "" : ", no line of sight");
        m_approachGuid = pTarget->GetObjectGuid();
        if (m_holdPosition)
            SetHoldPosition(false);
    }
    if (me->GetStandState() != UNIT_STAND_STATE_STAND)
        me->SetStandState(UNIT_STAND_STATE_STAND);
    me->GetMotionMaster()->MovePoint(0, x, y, z, MOVE_PATHFINDING);
    return true;
}

SpellEntry const* PartyBotAI::GetRangedWeaponSpell()
{
    if (me->GetClass() == CLASS_HUNTER) // Auto Shot
        return nullptr;

    Item* pWeapon = me->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
    ItemPrototype const* pProto = pWeapon ? pWeapon->GetProto() : nullptr;
    if (!pProto || pProto->Class != ITEM_CLASS_WEAPON)
        return nullptr;

    uint32 spellId;
    switch (pProto->SubClass)
    {
        case ITEM_SUBCLASS_WEAPON_BOW:      spellId = PB_SPELL_SHOOT_BOW;      break;
        case ITEM_SUBCLASS_WEAPON_GUN:      spellId = PB_SPELL_SHOOT_GUN;      break;
        case ITEM_SUBCLASS_WEAPON_CROSSBOW: spellId = PB_SPELL_SHOOT_CROSSBOW; break;
        case ITEM_SUBCLASS_WEAPON_THROWN:   spellId = PB_SPELL_THROW;          break;
        default:                            return nullptr; // wands: casters use Shoot themselves
    }

    // A player learns these with the weapon skill at the trainer.
    if (!me->HasSpell(spellId))
    {
        me->LearnSpell(spellId, false);
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Shoot] %s: learns spell %u for %s", me->GetName(), spellId,
            pProto->Name1);
    }
    return sSpellMgr.GetSpellEntry(spellId);
}

bool PartyBotAI::ShootRangedWeapon(Unit* pTarget)
{
    SpellEntry const* pSpellEntry = GetRangedWeaponSpell();
    if (!pSpellEntry || !CanTryToCastSpell(pTarget, pSpellEntry) || !me->IsWithinLOSInMap(pTarget))
        return false;

    SpellCastResult result = DoCastSpell(pTarget, pSpellEntry);
    if (result == SPELL_FAILED_NEED_AMMO || result == SPELL_FAILED_NO_AMMO)
    {
        AddRangedAmmo();
        result = DoCastSpell(pTarget, pSpellEntry);
    }
    return result == SPELL_CAST_OK;
}

// Crowd-controls the CC marks given to this bot (pull timer, in and out of combat). Out of spell
// range or line of sight it first walks there (also off a stay spot). Never the target of its own
// attack order. Returns true while it casts or walks for it.
static char const* GetCreatureTypeName(uint32 creatureType)
{
    switch (creatureType)
    {
        case CREATURE_TYPE_BEAST:      return "beast";
        case CREATURE_TYPE_DRAGONKIN:  return "dragonkin";
        case CREATURE_TYPE_DEMON:      return "demon";
        case CREATURE_TYPE_ELEMENTAL:  return "elemental";
        case CREATURE_TYPE_GIANT:      return "giant";
        case CREATURE_TYPE_UNDEAD:     return "undead";
        case CREATURE_TYPE_HUMANOID:   return "humanoid";
        case CREATURE_TYPE_CRITTER:    return "critter";
        case CREATURE_TYPE_MECHANICAL: return "mechanical";
    }
    return "wrong creature type";
}

bool PartyBotAI::HasCrowdControlSpell() const
{
    return GetCrowdControlSpell() || (me->GetClass() == CLASS_WARLOCK && m_spells.warlock.pFear);
}

// The class CC spell; a warlock fears what it cannot banish (not a demon or elemental, or no
// Banish yet).
SpellEntry const* PartyBotAI::GetCrowdControlSpellFor(Unit const* pTarget) const
{
    SpellEntry const* pSpellEntry = GetCrowdControlSpell();
    if (me->GetClass() == CLASS_WARLOCK && m_spells.warlock.pFear &&
        (!pSpellEntry || (pSpellEntry->TargetCreatureType &&
                         !(pSpellEntry->TargetCreatureType & pTarget->GetCreatureTypeMask()))))
        return m_spells.warlock.pFear;
    return pSpellEntry;
}

bool PartyBotAI::CrowdControlMarkedTargets()
{
    if (!HasCrowdControlSpell())
        return false;

    for (auto mark : m_marksToCC)
    {
        Unit* pTarget = GetMarkedTarget(mark);
        if (!pTarget || pTarget->GetObjectGuid() == m_orderedTargetGuid ||
            m_releasedCC.count(pTarget->GetObjectGuid()) ||
            pTarget->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL) ||
            !IsValidHostileTarget(pTarget) || AreOthersOnSameTarget(pTarget->GetObjectGuid()))
            continue;

        SpellEntry const* pSpellEntry = GetCrowdControlSpellFor(pTarget);
        if (!pSpellEntry)
            continue;

        // Wrong creature type or immune: release the mark instead of trying forever.
        if (pSpellEntry->TargetCreatureType && !(pSpellEntry->TargetCreatureType & pTarget->GetCreatureTypeMask()))
        {
            ReleaseCCMark(pTarget, pSpellEntry, GetCreatureTypeName(pTarget->GetCreatureType()));
            continue;
        }
        if (pTarget->IsImmuneToSpell(pSpellEntry, false) ||
           (pSpellEntry->Mechanic && pTarget->IsImmuneToMechanic(Mechanics(pSpellEntry->Mechanic))))
        {
            ReleaseCCMark(pTarget, pSpellEntry, "immune");
            continue;
        }

        float const range = Spells::GetSpellMaxRange(sSpellRangeStore.LookupEntry(pSpellEntry->rangeIndex));
        if (WalkIntoRange(pTarget, range, "CC", pSpellEntry->SpellName[0].c_str()))
            return true;

        if (!CanTryToCastSpell(pTarget, pSpellEntry))
            continue;

        SpellCastResult const result = DoCastSpell(pTarget, pSpellEntry);
        if (result == SPELL_CAST_OK)
        {
            me->ClearUnitState(UNIT_STATE_MELEE_ATTACKING);
            return true;
        }
        if (result == SPELL_FAILED_BAD_TARGETS || result == SPELL_FAILED_IMMUNE || result == SPELL_FAILED_HIGHLEVEL)
            ReleaseCCMark(pTarget, pSpellEntry, result == SPELL_FAILED_HIGHLEVEL ? "level too high" : "not a valid target");
    }
    return false;
}

void PartyBotAI::ReleaseCCMark(Unit* pTarget, SpellEntry const* pSpellEntry, char const* why)
{
    if (!m_releasedCC.insert(pTarget->GetObjectGuid()).second)
        return;

    sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Mark] %s: cannot %s %s (%s), mark released, attacked normally",
        me->GetName(), pSpellEntry->SpellName[0].c_str(), pTarget->GetName(), why);
    SayInParty(std::string("Can't ") + pSpellEntry->SpellName[0] + " " + pTarget->GetName() + " (" + why +
        ") - attacking it instead.");
}

void PartyBotAI::SayInParty(std::string const& text) const
{
    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return;

    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_PARTY, text.c_str(), LANG_UNIVERSAL, CHAT_TAG_NONE,
        me->GetObjectGuid(), me->GetName());
    pGroup->BroadcastPacket(&data, false);
}

static char const* GetRaidIconName(RaidTargetIcon icon)
{
    static char const* const names[TARGET_ICON_COUNT] =
        { "star", "circle", "diamond", "triangle", "moon", "square", "cross", "skull" };
    return icon < TARGET_ICON_COUNT ? names[icon] : "?";
}

// "moon = Ade (Polymorph), triangle = Ki (Shackle Undead, manual)"; empty if no bot has a CC mark.
static std::string DescribeCCMarks(std::vector<PartyBotAI*> const& bots)
{
    std::string text;
    for (PartyBotAI const* pAI : bots)
    {
        SpellEntry const* pSpell = pAI->GetCrowdControlSpell();
        std::string spellName = pSpell ? pSpell->SpellName[0] : std::string("no CC spell");
        if (pAI->me->GetClass() == CLASS_WARLOCK && pAI->m_spells.warlock.pFear)
            spellName = pSpell ? spellName + ", else Fear" : std::string("Fear");
        for (auto mark : pAI->m_marksToCC)
        {
            text += text.empty() ? "" : ", ";
            text += std::string(GetRaidIconName(mark)) + " = " + pAI->me->GetName() + " (" +
                spellName + (pAI->m_ccMarksAuto ? ")" : ", manual)");
        }
    }
    return text;
}

void PartyBotAI::AssignCCMarks(Player* pLeader, bool reset, char const* why)
{
    Group* pGroup = pLeader ? pLeader->GetGroup() : nullptr;
    if (!pGroup)
        return;

    std::vector<PartyBotAI*> bots;
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        if (Player* pMember = itr->getSource())
            if (PartyBotAI* pAI = dynamic_cast<PartyBotAI*>(pMember->AI()))
                if (pAI->m_initialized)
                    bots.push_back(pAI);

    std::string const before = DescribeCCMarks(bots);

    // Manual assignments stay (and keep their marks) unless reset; automatic ones are redone.
    std::set<RaidTargetIcon> taken;
    for (PartyBotAI* pAI : bots)
    {
        if (reset || pAI->m_ccMarksAuto)
        {
            pAI->m_marksToCC.clear();
            pAI->m_ccMarksAuto = false;
        }
        if (reset)
            pAI->m_releasedCC.clear();
        taken.insert(pAI->m_marksToCC.begin(), pAI->m_marksToCC.end());
    }

    // One fixed mark per class; another bot of the same class takes the next free spare mark.
    static std::pair<uint8, RaidTargetIcon> const classMarks[] =
    {
        { CLASS_MAGE,    RAID_TARGET_ICON_MOON },
        { CLASS_PRIEST,  RAID_TARGET_ICON_TRIANGLE },
        { CLASS_WARLOCK, RAID_TARGET_ICON_DIAMOND },
        { CLASS_ROGUE,   RAID_TARGET_ICON_STAR },
        { CLASS_DRUID,   RAID_TARGET_ICON_CIRCLE },
        { CLASS_PALADIN, RAID_TARGET_ICON_SQUARE },
    };
    static RaidTargetIcon const spareMarks[] =
        { RAID_TARGET_ICON_SQUARE, RAID_TARGET_ICON_CIRCLE, RAID_TARGET_ICON_DIAMOND, RAID_TARGET_ICON_TRIANGLE,
          RAID_TARGET_ICON_STAR };

    for (auto const& classMark : classMarks)
    {
        for (PartyBotAI* pAI : bots)
        {
            if (pAI->me->GetClass() != classMark.first || !pAI->m_marksToCC.empty() || !pAI->HasCrowdControlSpell())
                continue;

            RaidTargetIcon mark = classMark.second;
            if (taken.count(mark))
            {
                auto const itr = std::find_if(std::begin(spareMarks), std::end(spareMarks),
                    [&taken](RaidTargetIcon spare) { return !taken.count(spare); });
                if (itr == std::end(spareMarks))
                    continue;
                mark = *itr;
            }
            pAI->m_marksToCC.push_back(mark);
            pAI->m_ccMarksAuto = true;
            taken.insert(mark);
        }
    }

    std::string const after = DescribeCCMarks(bots);
    if (!reset && after == before)
        return;

    std::string const text = after.empty() ? "no CC marks (no bot can crowd-control)" : after;
    sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Mark] %s: CC marks (%s): %s", pLeader->GetName(), why, text.c_str());
    if (!bots.empty())
        bots.front()->SayInParty("CC marks: " + text);
}

void PartyBotAI::AddToPlayerGroup()
{
    Player* pPlayer = ObjectAccessor::FindPlayer(m_leaderGuid);
    if (!pPlayer)
        return;

    Group* group = pPlayer->GetGroup();
    if (!group)
    {
        group = new Group;
        // new group: if can't add then delete
        if (!group->Create(pPlayer->GetObjectGuid(), pPlayer->GetName()))
        {
            delete group;
            return;
        }
        sObjectMgr.AddGroup(group);
    }

    if (me->GetGroup() != group)
    {
        if (me->GetGroup())
            me->RemoveFromGroup();

        group->AddMember(me->GetObjectGuid(), me->GetName());
    }
}

void PartyBotAI::OnPacketReceived(WorldPacket const* packet)
{
    //printf("Bot received %s\n", LookupOpcodeName(packet->GetOpcode()));
    switch (packet->GetOpcode())
    {
        case SMSG_LEARNED_SPELL:
        case SMSG_SUPERCEDED_SPELL:
        case SMSG_REMOVED_SPELL:
        {
            if (m_initialized)
                m_resetSpellData = true;
            return;
        }
        case SMSG_DUEL_REQUESTED:
        {
            auto data = std::make_unique<WorldPackets::Duel::DuelAccepted>();
            data->playerGuid = me->GetObjectGuid();
            me->GetSession()->QueuePacket(std::move(data));
            return;
        }
        case SMSG_PARTYKILLLOG:
        {
            if (!me)
                return;

            if (Group const* pGroup = me->GetGroup())
            {
                if (pGroup->GetLootMethod() == ROUND_ROBIN ||
                    pGroup->GetLootMethod() == GROUP_LOOT ||
                    pGroup->GetLootMethod() == NEED_BEFORE_GREED)
                {
                    ObjectGuid victimGuid = *(((uint64*)(*packet).contents()) + 1);
                    if (Creature* pCreature = me->GetMap()->GetCreature(victimGuid))
                    {
                        pCreature->m_Events.AddLambdaEventAtOffset([pCreature, guid = me->GetGUID()]()
                        {
                            if (pCreature->loot.roundRobinPlayer == guid)
                            {
                                // unassign loot from bot so real players can loot
                                pCreature->loot.roundRobinPlayer = 0;
                                pCreature->ForceValuesUpdateAtIndex(UNIT_DYNAMIC_FLAGS);
                            }
                        }, 1);
                    }
                }
            }
            
            return;
        }
    }

    CombatBotBaseAI::OnPacketReceived(packet);
}

void PartyBotAI::OnPlayerLogin()
{
    if (!m_initialized)
        me->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SPAWNING);
}

void PartyBotAI::UpdateAI(uint32 const diff)
{
    if (m_initialized && me->IsInWorld())
    {
        // Toggle: always; other follow modes: out of combat (a cast must not hold the bot back).
        if (!m_holdPosition && (m_followMode == PB_FOLLOW_STACKED || !me->IsInCombat()))
            ReactToLeaderMovement();

        if (!m_holdPosition)
            StandUpToFollow();

        FaceCombatTarget();

        // Entering combat (attacked, or the group pulled): think on the next tick, not after
        // the rest of the update interval.
        bool const inCombat = me->IsInCombat();
        if (inCombat && !m_wasInCombat)
            WakeUp();
        else if (!inCombat && m_wasInCombat)
            m_releasedCC.clear(); // fight over: released CC marks count again next pull
        m_wasInCombat = inCombat;

        UpdateForcedAoE();

        CheckAmmoBagHaste();
    }

    m_updateTimer.Update(diff);
    if (m_updateTimer.Passed())
        m_updateTimer.Reset(GetUpdateInterval());
    else
        return;

    if (!me->IsInWorld() || me->IsBeingTeleported())
        return;

    if (!m_initialized)
    {
        AddToPlayerGroup();

        if (m_race && m_class) // temporary character
        {
            if (m_level && m_level != me->GetLevel())
            {
                me->GiveLevel(m_level);
                me->InitTalentForLevel();
                me->SetUInt32Value(PLAYER_XP, 0);
            }

            if (!m_cloneGuid.IsEmpty())
            {
                CloneFromPlayer(sObjectAccessor.FindPlayer(m_cloneGuid));
                AutoAssignRole();
            }
            else
            {
                LearnPremadeSpecForClass();

                if (m_role == ROLE_INVALID)
                    AutoAssignRole();

                // Ding sync: role talents and trainer spells from the start; the bot earns no XP
                // itself and only levels together with the leader.
                LearnSpellsAndTalentsForLevel(true);
                me->SetPersonalXpRate(0.0f);

                // Random/premade setting: scored gear for class, role and level instead.
                if (sWorld.getConfig(CONFIG_UINT32_PARTY_BOT_AUTO_EQUIP) == PLAYER_BOT_AUTO_EQUIP_STARTING_GEAR)
                    AutoEquipGear(PLAYER_BOT_AUTO_EQUIP_STARTING_GEAR);
                else
                    EquipRoleGear();

                // fix client bug causing some item slots to not be visible
                if (Player* pLeader = GetPartyLeader())
                {
                    me->SetVisibility(VISIBILITY_OFF);
                    pLeader->UpdateVisibilityOf(pLeader, me);
                    me->SetVisibility(VISIBILITY_ON);
                }
            }
            me->UpdateSkillsToMaxSkillsForLevel();
        }
        else // loaded from db
        {
            if (m_role == ROLE_INVALID)
                AutoAssignRole();

            if (me->IsGameMaster())
                me->SetGameMaster(false);

            me->TeleportTo(m_mapId, m_x, m_y, m_z, m_o);
        }

        ResetSpellData();
        PopulateSpellData();
        AddAllSpellReagents();
        EquipAmmoBag();
        AddRangedAmmo();
        me->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SPAWNING);
        SummonPetIfNeeded();
        me->SetHealthPercent(100.0f);
        me->SetPowerPercent(me->GetPowerType(), 100.0f);

        uint32 newzone, newarea;
        me->GetZoneAndAreaId(newzone, newarea);
        me->UpdateZone(newzone, newarea);

        m_initialized = true;
        AssignCCMarks(ObjectAccessor::FindPlayer(m_leaderGuid), false, "bot added");
        return;
    }

    if (m_resetSpellData)
    {
        ResetSpellData();
        PopulateSpellData();
        m_resetSpellData = false;
        // A newly learned CC spell (ding) can give this bot a CC mark.
        AssignCCMarks(ObjectAccessor::FindPlayer(m_leaderGuid), false, "spells learned");
    }

    Player* pLeader = GetPartyLeader();
    if (!pLeader)
    {
        botEntry->requestRemoval = true;
        return;
    }

    if (!pLeader->IsInWorld())
        return;

    if (IsLevelSynced() && pLeader->GetLevel() > me->GetLevel())
        SyncLevelWithLeader(pLeader);

    if (pLeader->InBattleGround() &&
        !me->InBattleGround())
    {
        if (m_receivedBgInvite)
        {
            SendBattlefieldPortPacket();
            m_receivedBgInvite = false;
            return;
        }

        // Remain idle until we can join battleground.
        return;
    }

    if (pLeader->IsTaxiFlying())
    {
        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
        {
            me->GetMotionMaster()->Clear(false, true);
            me->GetMotionMaster()->MoveIdle();
        }
        return;
    }

    if (me->HasUnitState(UNIT_STATE_FEIGN_DEATH) && me->HasAuraType(SPELL_AURA_FEIGN_DEATH) &&
       !me->IsInCombat() && (!me->GetPet() || !me->GetPet()->IsInCombat()) &&
       !me->SelectRandomUnfriendlyTarget(nullptr, 20.0f, false, true))
        me->RemoveSpellsCausingAura(SPELL_AURA_FEIGN_DEATH);

    if (me->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL))
    {
        BreakCrowdControlEffects();
        return;
    }

    if (me->IsDead())
    {
        if (me->InBattleGround())
        {
            if (me->GetDeathState() == CORPSE)
            {
                me->BuildPlayerRepop();
                me->RepopAtGraveyard();
            }
        }
        else
        {
            if (ShouldAutoRevive())
            {
                me->ResurrectPlayer(0.5f);
                me->SpawnCorpseBones();
                me->CastSpell(me, PB_SPELL_HONORLESS_TARGET, true);
            }
        }

        return;
    }

    if (me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
    {
        // Stop auto shot if no target.
        if (!me->GetVictim())
            me->InterruptSpell(CURRENT_AUTOREPEAT_SPELL, true);
        else if (me->GetClass() == CLASS_HUNTER)
        {
            if (me->GetCombatDistance(me->GetVictim()) < 8.0f)
                me->InterruptSpell(CURRENT_AUTOREPEAT_SPELL, true);
            else
                UpdateInCombatAI_Hunter();
        }

        return;
    }

    if (Spell* pCurrentSpell = me->GetCurrentSpell(CURRENT_GENERIC_SPELL))
    {
        // Interrupt pre casted heals if target is not injured.
        if (pCurrentSpell->getState() == SPELL_STATE_PREPARING &&
            pCurrentSpell->m_spellInfo->IsHealSpell())
        {
            if (Unit* pTarget = pCurrentSpell->m_targets.getUnitTarget())
            {
                if (pTarget->GetHealth() == pTarget->GetMaxHealth())
                {
                    me->InterruptSpell(CURRENT_GENERIC_SPELL, true);
                }
            }
        }
    }

    if (me->IsNonMeleeSpellCasted(false, false, true))
        return;

    if (!m_rangedPullGuid.IsEmpty() && UpdateRangedPull())
        return;

    if (IsPullWaiting())
    {
        if (WorldTimer::getMSTimeDiffToNow(m_pullWaitStart) >= m_pullWaitDuration)
            EndPullWait("pull timer over");
        else if (!me->GetAttackers().empty())
            EndPullWait("attacked");
        else
        {
            UpdatePullWait();
            return;
        }
    }

    if (me->GetTargetGuid() == me->GetObjectGuid())
        me->ClearTarget();

    SetActiveLayer(SelectLayer());
    bool const mayFollow = !m_holdPosition;
    // "toggle": stay stacked on the leader at all times, also in combat.
    bool const stackedFollow = mayFollow && m_followMode == PB_FOLLOW_STACKED;

    // Holding position: getting back to the spot comes before role duty.
    if (m_holdPosition && ReturnToHoldSpot())
        return;

    // The group is fighting (a member within 50 yd is attacked) but this bot is not hit yet:
    // it joins the fight or heals instead of sitting down or buffing; a sitting bot stands up.
    bool const groupFighting = !me->IsInCombat() && !IsInDuel() && SelectPartyAttackTarget();

    if (!me->IsInCombat())
    {
        if (groupFighting && (IsEating(me) || IsDrinking(me)))
        {
            StopEatingDrinking(me);
            if (me->GetStandState() != UNIT_STAND_STATE_STAND)
                me->SetStandState(UNIT_STAND_STATE_STAND);
            sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Brain] %s: stops eating/drinking, the group is fighting",
                me->GetName());
        }

        // On stay: eat/drink freely. Otherwise following wins: only once near the leader
        // (stacked: on the leader) and while the leader stands still.
        bool const mayEatDrink = !groupFighting && (!mayFollow ||
            (!pLeader->IsMoving() &&
              me->IsWithinDist(pLeader, stackedFollow ? PB_STACK_DIST : PB_MAX_FOLLOW_DIST + 2.0f, true, SizeFactor::None)));
        if (mayEatDrink && DrinkAndEat())
        {
            if (!me->IsWithinDistInMap(pLeader, 100.0f))
            {
                me->SetHealth(me->GetMaxHealth());
                if (me->GetPowerType() == POWER_MANA)
                    me->SetPower(POWER_MANA, me->GetMaxPower(POWER_MANA));
            }
            else if (me->IsMounted())
                me->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);
            return;
        }

        if (!groupFighting)
            KeepManaGem();

        // Teleport to leader if too far away.
        if (mayFollow && !me->IsWithinDistInMap(pLeader, 100.0f) && !IsInDuel())
        {
            if (!me->IsStopped())
                me->StopMoving();
            me->GetMotionMaster()->Clear(false, true);
            me->GetMotionMaster()->MoveIdle();
            char name[128] = {};
            snprintf(name, sizeof(name), "%s", pLeader->GetName());
            ChatHandler(me).HandleGonameCommand(name);
            return;
        }
    }

    if (me->GetStandState() != UNIT_STAND_STATE_STAND)
        me->SetStandState(UNIT_STAND_STATE_STAND);

    if (me->GetSheath() == SHEATH_STATE_UNARMED && !me->IsMounted())
        me->SetSheath(SHEATH_STATE_MELEE);

    // Not while the group fights: damage dealers and tanks go straight to target selection
    // (healers keep this, their out-of-combat code heals the members that are being hit).
    if (!me->IsInCombat() && !me->IsMounted() && (!groupFighting || GetRole() == ROLE_HEALER))
    {
        // Buffs and other instant spells always go out. While the leader moves (not on stay)
        // cast-time spells are refused (CanTryToCastSpell) and the bot keeps following instead
        // of waiting for its buff global cooldown.
        UpdateOutOfCombatAI();

        if (mayFollow && pLeader->IsMoving())
            m_isBuffing = false;
        else
        {
            if (m_isBuffing)
                return;

            if (me->IsNonMeleeSpellCasted())
                return;
        }
    }

    // An attack/pull order ends when its target is gone (dead, despawned).
    if (!m_orderedTargetGuid.IsEmpty())
    {
        Unit* pOrdered = me->GetMap()->GetUnit(m_orderedTargetGuid);
        if (!pOrdered || !pOrdered->IsAlive())
        {
            sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Attack] %s: ordered target gone, back to normal targets", me->GetName());
            ClearOrderedTarget();
        }
    }

    Unit* pVictim = me->GetVictim();

    if (GetRole() != ROLE_HEALER)
    {
        if (pVictim && IsValidHostileTarget(pVictim))
        {
            char const* reason = "";
            if (IsForbiddenTarget(pVictim))
            {
                // Never hit a CC mark unless ordered to (it may be about to be crowd controlled).
                sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Mark] %s: leaves %s (CC mark)", me->GetName(), pVictim->GetName());
                me->AttackStop(true);
                pVictim = nullptr;
            }
            else if (m_role != ROLE_TANK)
            {
                // Damage dealers switch at once to the ordered target, the skull or a focus mark
                // (tanks decide in UpdateTankDuty).
                Unit* pPreferred = GetPreferredTarget(reason);
                if (pPreferred && pPreferred != pVictim)
                {
                    sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Target] %s: %s -> %s (%s)", me->GetName(),
                        pVictim->GetName(), pPreferred->GetName(), reason);
                    me->AttackStop(true);
                    AttackStart(pPreferred);
                    return;
                }
            }
        }

        if (!pVictim || !IsValidHostileTarget(pVictim))
        {
            if (pVictim)
                me->AttackStop();

            char const* reason = "";
            if (Unit* pNewVictim = SelectAttackTarget(pLeader, reason))
            {
                sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Target] %s: picks %s (%s)", me->GetName(),
                    pNewVictim->GetName(), reason);
                AttackStart(pNewVictim);
                return;
            }
        }
    }

    if (mayFollow && !me->IsInCombat())
    {
        // Mount if leader is mounted and we don't have a target.
        if (pLeader->IsMounted() && !me->GetVictim())
        {
            if (!me->IsMounted())
            {
                // Leave shapeshift before mounting.
                if (me->IsInDisallowedMountForm() &&
                    me->GetDisplayId() != me->GetNativeDisplayId() &&
                    me->HasAuraType(SPELL_AURA_MOD_SHAPESHIFT))
                    me->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);

                auto auraList = pLeader->GetAurasByType(SPELL_AURA_MOUNTED);
                if (!auraList.empty())
                {
                    bool oldStateCastTime = me->HasCheatOption(PLAYER_CHEAT_NO_CAST_TIME);
                    bool oldStatePower = me->HasCheatOption(PLAYER_CHEAT_NO_POWER);
                    me->SetCheatOption(PLAYER_CHEAT_NO_CAST_TIME, true);
                    me->SetCheatOption(PLAYER_CHEAT_NO_POWER, true);
                    me->CastSpell(me, (*auraList.begin())->GetId(), true);
                    me->SetCheatOption(PLAYER_CHEAT_NO_CAST_TIME, oldStateCastTime);
                    me->SetCheatOption(PLAYER_CHEAT_NO_POWER, oldStatePower);
                }
            }
        }
        else if (me->IsMounted())
            me->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);
    }

    if (!pVictim && mayFollow)
        UpdateFollowMode(pLeader);

    if (!me->IsMoving())
    {
        if (!pVictim || stackedFollow)
        {
            if (mayFollow && me->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
                StartFollowing(pLeader);
        }

        // Stacked bots keep the follow: AttackStart only starts auto-attack, chasing is blocked.
        if (pVictim)
        {
            if (!me->HasUnitState(UNIT_STATE_MELEE_ATTACKING) &&
               (GetRole() == ROLE_MELEE_DPS || m_role == ROLE_TANK) &&
                IsValidHostileTarget(pVictim) &&
                AttackStart(pVictim))
                return;

            switch (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
            {
                case IDLE_MOTION_TYPE:
                case FOLLOW_MOTION_TYPE:
                    BeginChasing(pVictim);
                    break;
            }
        }
    }

    // In combat, or the group fights and this bot has a target (not hit yet): full rotation.
    if (me->IsInCombat() || (groupFighting && me->GetVictim()))
        UpdateInCombatAI();
}


PartyBotLayer PartyBotAI::SelectLayer() const
{
    if (m_holdPosition || m_followMode == PB_FOLLOW_STACKED)
        return PB_LAYER_COMMAND;

    if (me->IsInCombat() || me->GetVictim())
        return PB_LAYER_ROLE_DUTY;

    return PB_LAYER_FOLLOW;
}

char const* PartyBotAI::GetLayerName(PartyBotLayer layer) const
{
    switch (layer)
    {
        case PB_LAYER_COMMAND:
            return m_holdPosition ? "Command(stay)" : "Command(toggle)";
        case PB_LAYER_ROLE_DUTY:
            return "RoleDuty";
        case PB_LAYER_FOLLOW:
            return "Follow";
        default:
            return "None";
    }
}

void PartyBotAI::SetActiveLayer(PartyBotLayer layer)
{
    char const* const name = GetLayerName(layer);
    if (name == m_activeLayerName)
        return;

    sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Brain] %s: %s -> %s", me->GetName(), m_activeLayerName, name);
    m_activeLayer = layer;
    m_activeLayerName = name;
}

void PartyBotAI::OrderStay()
{
    SetFollowMode(PB_FOLLOW_LOOSE);
    SetHoldPosition(true);
}

void PartyBotAI::OrderAttack(Unit* pTarget, bool overrideCC, bool ranged)
{
    EndPullWait("attack order");
    EndForcedAoE(overrideCC ? "attack order" : "pull order");
    m_orderedTargetGuid = pTarget->GetObjectGuid();
    m_orderOverridesCC = overrideCC;
    if (IsForbiddenTarget(pTarget))
    {
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Pull] %s: will not pull %s (CC mark; use attack to override)",
            me->GetName(), pTarget->GetName());
        ClearOrderedTarget();
        return;
    }

    BreakCurrentActions();
    if (m_holdPosition)
    {
        // After the fight the bot follows the leader again (it does not return to the spot).
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Attack] %s: leaves stay to attack %s", me->GetName(), pTarget->GetName());
        SetHoldPosition(false);
    }

    m_rangedPullGuid.Clear();
    if (ranged)
    {
        if (GetRangedWeaponSpell())
        {
            sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Pull] %s: ranged pull on %s", me->GetName(), pTarget->GetName());
            m_rangedPullGuid = pTarget->GetObjectGuid();
            m_rangedPullStart = WorldTimer::getMSTime();
            m_rangedPullShotTime = 0;
            if (me->GetVictim())
                me->AttackStop(true);
            me->StopMoving();
            me->GetMotionMaster()->Clear(false, true);
            me->GetMotionMaster()->MoveIdle();
            return;
        }
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Pull] %s: no ranged weapon, pulls %s in melee", me->GetName(),
            pTarget->GetName());
    }

    if (me->GetVictim() && me->GetVictim() != pTarget)
        me->AttackStop(true);
    AttackStart(pTarget);
}

void PartyBotAI::ClearOrderedTarget()
{
    m_orderedTargetGuid.Clear();
    m_orderOverridesCC = false;
    m_rangedPullGuid.Clear();
}

// Ranged pull (tank): walk into shooting range, shoot the target, then wait on the spot until it
// reaches melee (or PB_RANGED_PULL_WAIT after the shot), then tank as usual. Returns true while busy.
bool PartyBotAI::UpdateRangedPull()
{
    Unit* pTarget = me->GetMap()->GetUnit(m_rangedPullGuid);
    if (!pTarget || !pTarget->IsAlive() || !IsValidHostileTarget(pTarget))
    {
        EndRangedPull("target gone");
        return false;
    }

    if (m_rangedPullShotTime)
    {
        if (me->CanReachWithMeleeAutoAttack(pTarget))
            EndRangedPull("target arrived");
        else if (WorldTimer::getMSTimeDiffToNow(m_rangedPullShotTime) > PB_RANGED_PULL_WAIT)
            EndRangedPull("target did not come");
        else
            return true;
        return false;
    }

    SpellEntry const* pShot = GetRangedWeaponSpell();
    SpellRangeEntry const* pRange = pShot ? sSpellRangeStore.LookupEntry(pShot->rangeIndex) : nullptr;
    if (!pRange)
    {
        EndRangedPull("no ranged weapon");
        return false;
    }
    if (WorldTimer::getMSTimeDiffToNow(m_rangedPullStart) > PB_RANGED_PULL_TIMEOUT)
    {
        EndRangedPull("could not shoot in time");
        return false;
    }
    if (me->GetCombatDistance(pTarget) < pRange->minRange)
    {
        EndRangedPull("too close to shoot");
        return false;
    }

    if (WalkIntoRange(pTarget, Spells::GetSpellMaxRange(pRange), "Pull", pShot->SpellName[0].c_str()))
        return true;

    if (ShootRangedWeapon(pTarget))
    {
        m_rangedPullShotTime = WorldTimer::getMSTime();
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Pull] %s: shot %s, waiting for it", me->GetName(), pTarget->GetName());
    }
    return true;
}

// Ends the ranged pull; the tank then attacks the pulled target (still its ordered target).
void PartyBotAI::EndRangedPull(char const* why)
{
    sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Pull] %s: ranged pull over (%s)", me->GetName(), why);
    Unit* pTarget = me->GetMap()->GetUnit(m_rangedPullGuid);
    m_rangedPullGuid.Clear();
    if (pTarget && pTarget->IsAlive() && IsValidHostileTarget(pTarget))
        AttackStart(pTarget);
}

void PartyBotAI::StartPullWait(uint32 duration)
{
    m_pullWaitStart = WorldTimer::getMSTime();
    m_pullWaitDuration = duration;
    m_approachGuid.Clear();
    EndForcedAoE("pull order");
    BreakCurrentActions();
    me->StopMoving();
    me->GetMotionMaster()->Clear(false, true);
    me->GetMotionMaster()->MoveIdle();
}

void PartyBotAI::EndPullWait(char const* why)
{
    if (!IsPullWaiting())
        return;

    m_pullWaitDuration = 0;
    sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Pull] %s: stops waiting (%s)", me->GetName(), why);
}

// While the pull timer runs: crowd-control this bot's CC marks (walking there first), otherwise
// stand still.
void PartyBotAI::UpdatePullWait()
{
    if (CrowdControlMarkedTargets())
        return;

    if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != IDLE_MOTION_TYPE)
    {
        me->StopMoving();
        me->GetMotionMaster()->Clear(false, true);
        me->GetMotionMaster()->MoveIdle();
    }
}

// Hold a given spot (comestay: the leader's position; aedm "move here": the clicked ground);
// ReturnToHoldSpot walks there and keeps the bot there like stay. The order wins at once over
// whatever the bot is doing: casts, eating/drinking, attacks and attack orders are dropped.
void PartyBotAI::OrderHoldSpot(uint32 mapId, float x, float y, float z)
{
    BreakCurrentActions();
    ClearOrderedTarget();
    EndPullWait("move order");
    if (me->GetVictim())
        me->AttackStop(true);
    m_moveOrderTime = WorldTimer::getMSTime();

    SetFollowMode(PB_FOLLOW_LOOSE);
    SetHoldPosition(true);

    m_holdMapId = mapId;
    m_holdX = x;
    m_holdY = y;
    m_holdZ = z;
}

void PartyBotAI::OrderFollow()
{
    ClearOrderedTarget();
    EndPullWait("follow order");
    SetHoldPosition(false);
    SetFollowMode(PB_FOLLOW_ARRIVE);
    BreakActionsAndFollowNow();
}

void PartyBotAI::OrderStackedFollow(bool stacked)
{
    ClearOrderedTarget();
    EndPullWait("toggle order");
    SetHoldPosition(false);
    SetFollowMode(stacked ? PB_FOLLOW_STACKED : PB_FOLLOW_LOOSE);
    if (stacked)
        BreakActionsAndFollowNow();
    else
        BreakCurrentActions();
}

// Movement orders win over whatever the bot is doing: stop casting (also channels), eating and
// drinking, and stand up.
void PartyBotAI::BreakCurrentActions()
{
    if (!me->IsAlive() || me->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL))
        return;

    Spell const* pSpell = me->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    if (!pSpell)
        pSpell = me->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
    if (pSpell)
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Order] %s: stops casting %s for the order", me->GetName(),
            pSpell->m_spellInfo->SpellName[0].c_str());
    if (IsEating(me) || IsDrinking(me))
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Order] %s: stops eating/drinking for the order", me->GetName());

    me->InterruptNonMeleeSpells(false);
    StopEatingDrinking(me);
    if (me->GetStandState() != UNIT_STAND_STATE_STAND)
        me->SetStandState(UNIT_STAND_STATE_STAND);
    m_isBuffing = false;
}

// Follow orders: break current actions and start moving to the leader right away (not on the
// next AI update).
void PartyBotAI::BreakActionsAndFollowNow()
{
    if (!me->IsAlive() || me->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL))
        return;

    BreakCurrentActions();

    if (Player* pLeader = GetPartyLeader())
        if (pLeader->IsInWorld() && me->IsInMap(pLeader))
            StartFollowing(pLeader);
}

static char const* GetPartyBotFollowModeName(PartyBotFollowMode mode)
{
    switch (mode)
    {
        case PB_FOLLOW_ARRIVE:
            return "arrive";
        case PB_FOLLOW_STACKED:
            return "stacked";
        default:
            return "loose";
    }
}

void PartyBotAI::SetFollowMode(PartyBotFollowMode mode)
{
    m_arrivedAtLeader = false;

    if (mode == m_followMode)
        return;

    sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Brain] %s: follow mode %s -> %s", me->GetName(),
        GetPartyBotFollowModeName(m_followMode), GetPartyBotFollowModeName(mode));
    m_followMode = mode;

    // Drop the current follow so the next update restarts it with the new distance.
    // Entering "toggle" also drops any chase, so the bot goes straight to the leader.
    MovementGeneratorType const currentMove = me->GetMotionMaster()->GetCurrentMovementGeneratorType();
    if (currentMove == FOLLOW_MOTION_TYPE || (mode == PB_FOLLOW_STACKED && currentMove != IDLE_MOTION_TYPE))
    {
        me->StopMoving();
        me->GetMotionMaster()->Clear(false, true);
        me->GetMotionMaster()->MoveIdle();
    }
}

// "follow": once the bot has stacked on the leader and the leader moves, spread out (loose).
void PartyBotAI::UpdateFollowMode(Player const* pLeader)
{
    if (m_followMode != PB_FOLLOW_ARRIVE)
        return;

    if (!m_arrivedAtLeader)
    {
        if (me->IsWithinDist(pLeader, PB_STACK_DIST, true, SizeFactor::None))
            m_arrivedAtLeader = true;
    }
    else if (pLeader->IsMoving())
        SetFollowMode(PB_FOLLOW_LOOSE);
}

void PartyBotAI::StartFollowing(Player* pLeader)
{
    if (m_followMode == PB_FOLLOW_LOOSE)
        me->GetMotionMaster()->MoveFollow(pLeader, urand(PB_MIN_FOLLOW_DIST, PB_MAX_FOLLOW_DIST), frand(PB_MIN_FOLLOW_ANGLE, PB_MAX_FOLLOW_ANGLE));
    else
        me->GetMotionMaster()->MoveFollow(pLeader, 0.0f, 0.0f, true);
}

// Checked every world tick for every bot: when standing, always face the hostile target it is
// attacking or casting at (like a mob turns to its target), if it is in line of sight.
void PartyBotAI::FaceCombatTarget()
{
    if (!me->IsAlive() || me->IsMoving() || me->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL))
        return;

    Unit* pTarget = me->GetVictim();
    if (Spell* pSpell = me->GetCurrentSpell(CURRENT_GENERIC_SPELL))
        if (Unit* pSpellTarget = pSpell->m_targets.getUnitTarget())
            if (pSpellTarget != me && me->IsHostileTo(pSpellTarget))
                pTarget = pSpellTarget;

    if (!pTarget || pTarget == me || !pTarget->IsAlive() || !pTarget->IsInMap(me))
        return;

    if (!me->HasInArc(pTarget, M_PI_F / 3) && me->IsWithinLOSInMap(pTarget))
        me->SetFacingToObject(pTarget);
}

// Checked every world tick (toggle: always; other follow modes: out of combat). When the leader
// starts moving, drop any cast that would keep the bot in place and run the full AI on the next
// tick so it follows at once.
void PartyBotAI::ReactToLeaderMovement()
{
    Player* pLeader = GetPartyLeader();
    bool const leaderMoving = pLeader && pLeader->IsInWorld() && pLeader->IsMoving();
    bool const startedMoving = leaderMoving && !m_leaderWasMoving;
    m_leaderWasMoving = leaderMoving;

    // Not while paused (an active pause keeps the update timer longer than one interval) or
    // waiting for a pull timer.
    if (!startedMoving || !me->IsAlive() || m_updateTimer.GetExpiry() > GetUpdateInterval() || IsPullWaiting() ||
        !m_rangedPullGuid.IsEmpty())
        return;

    if (Spell* pSpell = me->GetCurrentSpell(CURRENT_GENERIC_SPELL))
        if (pSpell->getState() == SPELL_STATE_PREPARING)
            me->InterruptSpell(CURRENT_GENERIC_SPELL);

    if (me->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
        me->InterruptSpell(CURRENT_CHANNELED_SPELL);

    WakeUp();
}

// Mage, out of combat: carries the mana gem of its highest Conjure Mana spell (free, no cast).
void PartyBotAI::KeepManaGem()
{
    if (me->GetClass() != CLASS_MAGE)
        return;
    SpellEntry const* pSpellEntry = GetBestCreateItemSpell("Conjure Mana");
    if (!pSpellEntry)
        return;
    uint32 const itemId = GetCreatedItemId(pSpellEntry);
    if (me->HasItemCount(itemId, 1))
        return;
    AddItemToInventory(itemId);
    if (ItemPrototype const* pProto = sObjectMgr.GetItemPrototype(itemId))
        if (me->HasItemCount(itemId, 1))
            sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Supply] %s: gets %s", me->GetName(), pProto->Name1);
}

// Mage, in combat: uses its mana gem once the missing mana is at least the gem's best restore
// (nothing wasted). Shares the 2 min cooldown with healthstones.
bool PartyBotAI::UseManaGem()
{
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
    {
        Item* pItem = me->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!pItem || strncmp(pItem->GetProto()->Name1, "Mana ", 5) != 0)
            continue;
        uint32 const spellId = pItem->GetProto()->Spells[0].SpellId;
        SpellEntry const* pSpellEntry = spellId ? sSpellMgr.GetSpellEntry(spellId) : nullptr;
        if (!pSpellEntry || !pSpellEntry->HasEffect(SPELL_EFFECT_ENERGIZE))
            continue;
        int32 const restore = pSpellEntry->EffectBasePoints[0] + pSpellEntry->EffectDieSides[0];
        if (int32(me->GetMaxPower(POWER_MANA) - me->GetPower(POWER_MANA)) < restore ||
            !me->IsSpellReady(pSpellEntry, pItem->GetProto()))
            return false;
        float const manaPct = me->GetPowerPercent(POWER_MANA);
        std::string const name = pItem->GetProto()->Name1;
        if (UseItemEffect(pItem))
        {
            sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Supply] %s: uses %s (mana %.0f%%)", me->GetName(),
                name.c_str(), manaPct);
            return true;
        }
        return false;
    }
    return false;
}

// Checked every world tick unless on stay: following the leader beats eating/drinking, so a
// sitting bot stands up the moment the leader moves (and follows on the next tick).
void PartyBotAI::StandUpToFollow()
{
    bool const isEating = IsEating(me);
    bool const isDrinking = IsDrinking(me);
    if (!isEating && !isDrinking)
        return;

    // Not while paused (an active pause keeps the update timer longer than one interval) or
    // waiting for a pull timer.
    if (m_updateTimer.GetExpiry() > GetUpdateInterval() || IsPullWaiting())
        return;

    Player* pLeader = GetPartyLeader();
    if (!pLeader || !pLeader->IsInWorld() || !pLeader->IsMoving())
        return;

    StopEatingDrinking(me);
    if (me->GetStandState() != UNIT_STAND_STATE_STAND)
        me->SetStandState(UNIT_STAND_STATE_STAND);
    sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Brain] %s: stops %s to follow", me->GetName(),
        isDrinking ? (isEating ? "eating and drinking" : "drinking") : "eating");
    WakeUp();
}

void PartyBotAI::SetHoldPosition(bool hold)
{
    m_holdPosition = hold;
    m_returningToHoldSpot = false;

    if (hold)
    {
        me->StopMoving();
        me->GetMotionMaster()->Clear(false, true);
        me->GetMotionMaster()->MoveIdle();
        SetHoldSpotHere();
    }
}

// Run the AI on the next world tick. An active pause (timer longer than one interval) is kept.
void PartyBotAI::WakeUp()
{
    if (m_updateTimer.GetExpiry() <= GetUpdateInterval())
        m_updateTimer.Reset(0);
}

void PartyBotAI::SetHoldSpotHere()
{
    m_holdMapId = me->GetMapId();
    m_holdX = me->GetPositionX();
    m_holdY = me->GetPositionY();
    m_holdZ = me->GetPositionZ();
}

// Returns true while the bot is walking back to its held spot.
bool PartyBotAI::ReturnToHoldSpot()
{
    if (me->GetMapId() != m_holdMapId)
    {
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Brain] %s: left the map of its spot, holding here", me->GetName());
        m_returningToHoldSpot = false;
        SetHoldSpotHere();
        return false;
    }

    float const distance = me->GetDistance(m_holdX, m_holdY, m_holdZ, SizeFactor::None);
    if (distance <= PB_HOLD_SPOT_TOLERANCE)
    {
        m_moveOrderTime = 0;
        if (m_returningToHoldSpot)
        {
            sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Brain] %s: back on spot", me->GetName());
            m_returningToHoldSpot = false;
        }
        return false;
    }

    if (!m_returningToHoldSpot)
    {
        if (m_moveOrderTime)
            sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Brain] %s: %.1f yd from spot, moving there (%u ms after the order)",
                me->GetName(), distance, WorldTimer::getMSTimeDiffToNow(m_moveOrderTime));
        else
            sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Brain] %s: %.1f yd from spot, moving there", me->GetName(), distance);
        m_moveOrderTime = 0;
        m_returningToHoldSpot = true;
        m_returnBestDistance = distance;
        m_returnProgressTime = WorldTimer::getMSTime();
    }
    else if (distance < m_returnBestDistance - 1.0f)
    {
        m_returnBestDistance = distance;
        m_returnProgressTime = WorldTimer::getMSTime();
    }
    else if (WorldTimer::getMSTimeDiffToNow(m_returnProgressTime) > PB_HOLD_NO_PROGRESS_TIMEOUT)
    {
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Brain] %s: cannot reach spot (%.1f yd away), holding here", me->GetName(), distance);
        m_returningToHoldSpot = false;
        me->StopMoving();
        me->GetMotionMaster()->Clear(false, true);
        me->GetMotionMaster()->MoveIdle();
        SetHoldSpotHere();
        return false;
    }

    if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
    {
        me->GetMotionMaster()->MovePoint(0, m_holdX, m_holdY, m_holdZ, MOVE_PATHFINDING);
    }

    return true;
}

void PartyBotAI::UpdateOutOfCombatAI()
{
    if (!IsInDuel())
    {
        if (m_resurrectionSpell)
            if (Player* pTarget = SelectResurrectionTarget())
                if (CanTryToCastSpell(pTarget, m_resurrectionSpell))
                    if (DoCastSpell(pTarget, m_resurrectionSpell) == SPELL_CAST_OK)
                        return;

        if (m_role != ROLE_TANK && me->GetVictim() && CrowdControlMarkedTargets())
            return;
    }

    if (CheckForDispelTargets())
        return;

    switch (me->GetClass())
    {
        case CLASS_PALADIN:
            UpdateOutOfCombatAI_Paladin();
            break;
        case CLASS_SHAMAN:
            UpdateOutOfCombatAI_Shaman();
            break;
        case CLASS_HUNTER:
            UpdateOutOfCombatAI_Hunter();
            break;
        case CLASS_MAGE:
            UpdateOutOfCombatAI_Mage();
            break;
        case CLASS_PRIEST:
            UpdateOutOfCombatAI_Priest();
            break;
        case CLASS_WARLOCK:
            UpdateOutOfCombatAI_Warlock();
            break;
        case CLASS_WARRIOR:
            UpdateOutOfCombatAI_Warrior();
            break;
        case CLASS_ROGUE:
            UpdateOutOfCombatAI_Rogue();
            break;
        case CLASS_DRUID:
            UpdateOutOfCombatAI_Druid();
            break;
    }
}

// The talent tree with the most talent ranks learned (UINT32_MAX if none).
uint32 PartyBotAI::GetMainTalentTab() const
{
    std::map<uint32, uint32> ranksPerTab;
    for (uint32 talentId = 0; talentId < sTalentStore.GetNumRows(); ++talentId)
    {
        TalentEntry const* talentInfo = sTalentStore.LookupEntry(talentId);
        if (!talentInfo)
            continue;

        TalentTabEntry const* tabInfo = sTalentTabStore.LookupEntry(talentInfo->TalentTab);
        if (!tabInfo || !(me->GetClassMask() & tabInfo->ClassMask))
            continue;

        for (int32 rank = MAX_TALENT_RANK - 1; rank >= 0; --rank)
        {
            if (talentInfo->RankID[rank] && me->HasSpell(talentInfo->RankID[rank]))
            {
                ranksPerTab[talentInfo->TalentTab] += rank + 1;
                break;
            }
        }
    }

    uint32 bestTab = UINT32_MAX;
    uint32 bestRanks = 0;
    for (auto const& itr : ranksPerTab)
    {
        if (itr.second > bestRanks)
        {
            bestTab = itr.first;
            bestRanks = itr.second;
        }
    }
    return bestTab;
}

// Spends free talent points on the highest-level premade spec of the bot's class and role
// (the level 60 PvE specs), in template order, rank by rank. Talents whose tier is not
// reached yet are picked up on later dings. False if no spec matches the role.
bool PartyBotAI::LearnRoleSpecTalents()
{
    PlayerPremadeSpecTemplate const* pSpec = nullptr;
    for (auto const& itr : sObjectMgr.GetPlayerPremadeSpecTemplates())
    {
        if (itr.second.requiredClass == me->GetClass() && itr.second.role == m_role &&
           (!pSpec || itr.second.level > pSpec->level))
            pSpec = &itr.second;
    }

    if (!pSpec)
        return false;

    std::vector<TalentSpellPos const*> talents;
    for (uint32 spellId : pSpec->spells)
        if (TalentSpellPos const* pPos = GetTalentSpellPos(spellId))
            talents.push_back(pPos);

    bool progress = true;
    while (progress && me->GetFreeTalentPoints())
    {
        progress = false;
        for (TalentSpellPos const* pPos : talents)
            for (uint32 rank = 0; rank <= pPos->rank && me->GetFreeTalentPoints(); ++rank)
                if (me->LearnTalent(pPos->talent_id, rank))
                    progress = true;
    }
    return true;
}

// Trainer spells up to the current level, then talents: the role's spec, or (no spec for the
// role) random talents kept in the tree the bot already uses. resetTalents replaces the talents
// a new bot got from the class's generic premade spec.
void PartyBotAI::LearnSpellsAndTalentsForLevel(bool resetTalents)
{
    char args[] = "";
    ChatHandler(me).HandleLearnAllTrainerCommand(args);

    uint32 const mainTab = GetMainTalentTab();
    bool hasRoleSpec = false;
    for (auto const& itr : sObjectMgr.GetPlayerPremadeSpecTemplates())
        if (itr.second.requiredClass == me->GetClass() && itr.second.role == m_role)
            hasRoleSpec = true;

    if (hasRoleSpec)
    {
        if (resetTalents)
            me->ResetTalents(true);
        LearnRoleSpecTalents();
    }

    // No spec for the role, or the spec has fewer points than the bot (e.g. a lower-level
    // template): put the rest into the tree the bot mostly uses.
    if (me->GetFreeTalentPoints())
        LearnRandomTalents(hasRoleSpec ? GetMainTalentTab() : mainTab);
}

void PartyBotAI::SyncLevelWithLeader(Player const* pLeader)
{
    uint32 const oldLevel = me->GetLevel();
    uint32 const newLevel = std::min(pLeader->GetLevel(), sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL));
    if (newLevel <= oldLevel)
        return;

    size_t const oldSpellCount = me->GetSpellMap().size();

    me->GiveLevel(newLevel);
    me->InitTalentForLevel();
    me->SetUInt32Value(PLAYER_XP, 0);
    LearnSpellsAndTalentsForLevel(false);
    me->UpdateSkillsToMaxSkillsForLevel();
    if (sWorld.getConfig(CONFIG_UINT32_PARTY_BOT_AUTO_EQUIP) != PLAYER_BOT_AUTO_EQUIP_STARTING_GEAR)
        EquipRoleGear(); // regear for the new level

    ResetSpellData();
    PopulateSpellData();
    AddAllSpellReagents();
    EquipAmmoBag();
    AddRangedAmmo();
    if (me->GetClass() == CLASS_HUNTER)
        MaintainHunterPet();

    sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Ding] %s: %u -> %u, spell book %+d, %u talent points unspent",
        me->GetName(), oldLevel, newLevel, int32(me->GetSpellMap().size()) - int32(oldSpellCount),
        me->GetFreeTalentPoints());
}

// Manual tank slots of real players (bots keep theirs in their AI). Commands and map threads
// both read it, so it is guarded.
static std::mutex s_playerTankSlotsLock;
static std::map<ObjectGuid, uint8> s_playerTankSlots;

bool PartyBotAI::GetPlayerTankSlotChoice(ObjectGuid guid, uint8& slot)
{
    std::lock_guard<std::mutex> guard(s_playerTankSlotsLock);
    auto itr = s_playerTankSlots.find(guid);
    if (itr == s_playerTankSlots.end())
        return false;
    slot = itr->second;
    return true;
}

void PartyBotAI::SetPlayerTankSlotChoice(ObjectGuid guid, bool manual, uint8 slot)
{
    std::lock_guard<std::mutex> guard(s_playerTankSlotsLock);
    if (manual)
        s_playerTankSlots[guid] = slot;
    else
        s_playerTankSlots.erase(guid);
}

// Removes a manual slot from whoever holds it, so a slot is never given twice.
void PartyBotAI::ClearManualTankSlot(Group* pGroup, uint8 slot)
{
    if (!pGroup || slot == PB_TANK_SLOT_NONE)
        return;

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember)
            continue;

        if (PartyBotAI* pAI = dynamic_cast<PartyBotAI*>(pMember->AI()))
        {
            if (pAI->m_tankSlotManual && pAI->m_tankSlotChoice == slot)
                pAI->m_tankSlotManual = false;
        }
        else
        {
            uint8 choice;
            if (GetPlayerTankSlotChoice(pMember->GetObjectGuid(), choice) && choice == slot)
                SetPlayerTankSlotChoice(pMember->GetObjectGuid(), false, PB_TANK_SLOT_NONE);
        }
    }
}

std::string PartyBotAI::GetTankSlotName(uint8 slot)
{
    if (slot == PB_TANK_SLOT_MT)
        return "MT";
    if (slot >= PB_TANK_SLOT_OT1 && slot < PB_TANK_SLOT_MAX)
        return "OT" + std::to_string(slot - PB_TANK_SLOT_OT1 + 1);
    return "none";
}

void PartyBotAI::ResolveTankSlots(Group* pGroup, PartyBotTankSlotMap& slots)
{
    slots.clear();
    if (!pGroup)
        return;

    bool taken[PB_TANK_SLOT_MAX] = {};
    std::vector<Player*> autoPlayers; // real players who are tanking (shield or tank form)
    std::vector<Player*> autoBots;    // bots with the tank role

    // 1. Manual choices.
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember)
            continue;

        bool manual = false;
        uint8 choice = PB_TANK_SLOT_NONE;
        bool autoTank = false;

        if (PartyBotAI* pAI = dynamic_cast<PartyBotAI*>(pMember->AI()))
        {
            manual = pAI->m_tankSlotManual;
            choice = pAI->m_tankSlotChoice;
            autoTank = pAI->GetRole() == ROLE_TANK;
        }
        else
        {
            manual = GetPlayerTankSlotChoice(pMember->GetObjectGuid(), choice);
            if (IsTankClass(pMember->GetClass()))
            {
                Item* pOffhand = pMember->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
                autoTank = IsTankingForm(pMember->GetShapeshiftForm()) ||
                          (pOffhand && pOffhand->GetProto()->InventoryType == INVTYPE_SHIELD);
            }
        }

        if (manual)
        {
            if (choice != PB_TANK_SLOT_NONE && choice < PB_TANK_SLOT_MAX && !taken[choice])
            {
                slots[pMember->GetObjectGuid()] = choice;
                taken[choice] = true;
            }
        }
        else if (autoTank)
        {
            if (pMember->AI())
                autoBots.push_back(pMember);
            else
                autoPlayers.push_back(pMember);
        }
    }

    // 2. Automatic: tanking players first (the user is MT when tanking), then tank bots.
    std::vector<Player*> autoTanks = autoPlayers;
    autoTanks.insert(autoTanks.end(), autoBots.begin(), autoBots.end());

    uint8 next = PB_TANK_SLOT_MT;
    for (Player* pTank : autoTanks)
    {
        while (next < PB_TANK_SLOT_MAX && taken[next])
            ++next;
        if (next >= PB_TANK_SLOT_MAX)
            break;
        slots[pTank->GetObjectGuid()] = next;
        taken[next] = true;
    }
}

// Resolved once per world tick and shared by all checks of that tick.
PartyBotTankSlotMap const& PartyBotAI::GetTankSlots() const
{
    uint32 const now = WorldTimer::getMSTime();
    if (now != m_tankSlotCacheTime)
    {
        ResolveTankSlots(me->GetGroup(), m_tankSlotCache);
        m_tankSlotCacheTime = now;
    }
    return m_tankSlotCache;
}

uint8 PartyBotAI::GetSlotOf(PartyBotTankSlotMap const& slots, Unit const* pUnit)
{
    if (!pUnit)
        return PB_TANK_SLOT_NONE;
    auto itr = slots.find(pUnit->GetObjectGuid());
    return itr == slots.end() ? PB_TANK_SLOT_NONE : itr->second;
}

uint8 PartyBotAI::GetHealPriority(Unit const* pUnit) const
{
    if (pUnit->GetTypeId() != TYPEID_PLAYER || !me->GetGroup())
        return 0;

    switch (GetSlotOf(GetTankSlots(), pUnit))
    {
        case PB_TANK_SLOT_NONE:
            return 0;
        case PB_TANK_SLOT_MT:
            return 2;
        default:
            return 1;
    }
}

Unit* PartyBotAI::GetPreferredTarget(char const*& reason) const
{
    if (!m_orderedTargetGuid.IsEmpty())
        if (Unit* pOrdered = me->GetMap()->GetUnit(m_orderedTargetGuid))
            if (IsValidHostileTarget(pOrdered) && !IsForbiddenTarget(pOrdered))
            {
                reason = "ordered";
                return pOrdered;
            }

    return GetFocusMarkTarget(reason);
}

// The skull is always a focus mark; focusmark adds more. A mark counts only once it is fighting
// (bots never pull a marked pack on their own) and never when it is a CC mark.
Unit* PartyBotAI::GetFocusMarkTarget(char const*& reason) const
{
    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return nullptr;

    auto const isUsable = [this](Unit* pTarget)
    {
        return pTarget && pTarget->IsInCombat() && IsValidHostileTarget(pTarget) && !IsForbiddenTarget(pTarget);
    };

    if (Unit* pSkull = GetMarkedTarget(RAID_TARGET_ICON_SKULL))
        if (isUsable(pSkull))
        {
            reason = "skull";
            return pSkull;
        }

    for (auto markId : m_marksToFocus)
        if (Unit* pTarget = GetMarkedTarget(markId))
            if (isUsable(pTarget))
            {
                reason = "focus mark";
                return pTarget;
            }

    return nullptr;
}

bool PartyBotAI::IsForbiddenTarget(Unit const* pTarget) const
{
    if (m_orderOverridesCC && pTarget->GetObjectGuid() == m_orderedTargetGuid)
        return false;

    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return false;

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        PartyBotAI const* pAI = pMember ? dynamic_cast<PartyBotAI const*>(pMember->AI()) : nullptr;
        if (!pAI)
            continue;
        for (auto mark : pAI->m_marksToCC)
            if (pGroup->GetTargetWithIcon(mark) == pTarget->GetObjectGuid() &&
               !pAI->m_releasedCC.count(pTarget->GetObjectGuid()))
                return true;
    }
    return false;
}

// Nearest enemy attacking a group member without a tank slot, that no other tank bot is
// already going for (so off-tanks share the adds out).
Unit* PartyBotAI::SelectLooseEnemy(PartyBotTankSlotMap const& slots) const
{
    Group* pGroup = me->GetGroup();
    Unit* pBest = nullptr;
    float bestDistance = 0.0f;

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || pMember == me || GetSlotOf(slots, pMember) != PB_TANK_SLOT_NONE)
            continue;

        for (auto const pAttacker : pMember->GetAttackers())
        {
            if (!IsValidHostileTarget(pAttacker) || IsForbiddenTarget(pAttacker) || !me->IsWithinDist(pAttacker, 50.0f))
                continue;

            bool claimed = false;
            for (GroupReference* itr2 = pGroup->GetFirstMember(); itr2 != nullptr; itr2 = itr2->next())
            {
                Player* pOther = itr2->getSource();
                if (pOther && pOther != me && pOther->AI() && pOther->GetVictim() == pAttacker &&
                    GetSlotOf(slots, pOther) != PB_TANK_SLOT_NONE)
                {
                    claimed = true;
                    break;
                }
            }
            if (claimed)
                continue;

            float const distance = me->GetDistance(pAttacker);
            if (!pBest || distance < bestDistance)
            {
                pBest = pAttacker;
                bestDistance = distance;
            }
        }
    }

    return pBest;
}

// Taunts an enemy that attacks someone else, but never one that attacks another tank
// (bot or player). Area taunts only when this bot is the only tank.
bool PartyBotAI::TauntIfAllowed(Unit* pEnemy, PartyBotTankSlotMap const& slots, bool otherTanks)
{
    Unit* pEnemyVictim = pEnemy->GetVictim();
    if (!pEnemyVictim || pEnemyVictim == me)
        return false;

    if (GetSlotOf(slots, pEnemyVictim) != PB_TANK_SLOT_NONE)
        return false;

    // Several tanks on the same enemy: only the highest slot (MT, then OT1...) taunts it.
    uint8 const mySlot = GetSlotOf(slots, me);
    for (GroupReference* itr = me->GetGroup()->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pOther = itr->getSource();
        if (!pOther || pOther == me || pOther->GetVictim() != pEnemy)
            continue;
        uint8 const otherSlot = GetSlotOf(slots, pOther);
        if (otherSlot != PB_TANK_SLOT_NONE && (mySlot == PB_TANK_SLOT_NONE || otherSlot < mySlot))
            return false;
    }

    for (auto const& pSpellEntry : m_spellListTaunt)
    {
        if (otherTanks && pSpellEntry->IsAreaOfEffectSpell())
            continue;

        if (CanTryToCastSpell(pEnemy, pSpellEntry) &&
            DoCastSpell(pEnemy, pSpellEntry) == SPELL_CAST_OK)
        {
            sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Tank] %s: taunts %s off %s", me->GetName(),
                pEnemy->GetName(), pEnemyVictim->GetName());
            return true;
        }
    }
    return false;
}

// Tank threat on an enemy divided by the highest threat anyone else has on it. withFloor: that
// highest threat is at least a level-based floor, so an enemy nobody else hit yet is not safe
// until the tank built some; without it the lead shows real danger (an enemy about to switch).
float PartyBotAI::GetThreatLead(Unit* pEnemy, bool withFloor) const
{
    if (!pEnemy->CanHaveThreatList())
        return 100.0f;

    float myThreat = 0.0f;
    float othersMax = withFloor ? me->GetLevel() * PB_TANK_THREAT_FLOOR : 0.0f;
    for (auto const* pRef : pEnemy->GetThreatManager().getThreatList())
    {
        if (pRef->getUnitGuid() == me->GetObjectGuid())
            myThreat = pRef->getThreat();
        else
            othersMax = std::max(othersMax, pRef->getThreat());
    }
    if (othersMax <= 0.0f)
        return 100.0f;
    return myThreat / othersMax;
}

// Tab-tanking over the enemies attacking this tank (in melee reach):
// - an enemy about to be pulled by someone else (real lead < 1.15) gets the tank at once,
//   unless the current target is in the same danger;
// - otherwise, once the current target is safe (lead >= 1.5), the tank moves to the least
//   safe enemy. It keeps hitting a target until it is safe, so it rotates over the pack and
//   comes back to the one the damage dealers focus. Returns the (new) victim.
Unit* PartyBotAI::TabForThreat(Unit* pVictim)
{
    if (!pVictim || pVictim->GetVictim() != me)
        return pVictim;

    bool const victimSafe = GetThreatLead(pVictim, true) >= PB_TANK_SAFE_LEAD;
    bool const victimInDanger = GetThreatLead(pVictim, false) < PB_TANK_DANGER_LEAD;

    Unit* pBest = nullptr;
    float bestLead = PB_TANK_SAFE_LEAD;
    bool bestInDanger = false;
    for (auto const pAttacker : me->GetAttackers())
    {
        if (pAttacker == pVictim || pAttacker->GetVictim() != me ||
           !IsValidHostileTarget(pAttacker) || IsForbiddenTarget(pAttacker) ||
            pAttacker->HasBreakableByDamageCrowdControlAura() ||
           !me->CanReachWithMeleeAutoAttack(pAttacker))
            continue;

        float const realLead = GetThreatLead(pAttacker, false);
        if (realLead < PB_TANK_DANGER_LEAD)
        {
            if (!victimInDanger && (!bestInDanger || realLead < bestLead))
            {
                pBest = pAttacker;
                bestLead = realLead;
                bestInDanger = true;
            }
            continue;
        }

        float const lead = GetThreatLead(pAttacker, true);
        if (victimSafe && !bestInDanger && lead < bestLead)
        {
            pBest = pAttacker;
            bestLead = lead;
        }
    }

    if (!pBest)
        return pVictim;

    sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Tank] %s: leaves %s, builds threat on %s (lead %.2f%s)",
        me->GetName(), pVictim->GetName(), pBest->GetName(), bestLead, bestInDanger ? ", about to be pulled" : "");
    me->AttackStop(true);
    AttackStart(pBest);
    return pBest;
}

// Tank role duty. "Mark" below = the preferred target: attack/pull order, else skull, else a
// focus mark (GetPreferredTarget).
// - Mark + single focus: only the mark (raids).
// - Otherwise the tank always goes for loose enemies (attacking a group member without a tank
//   slot): it switches to them, taunts and builds threat. With a mark it leaves the mark
//   while the mark attacks this tank or another tank, and returns once the mark hits someone else.
// - Between loose enemies it builds threat on every enemy hitting it (TabForThreat).
bool PartyBotAI::UpdateTankDuty()
{
    PartyBotTankSlotMap const& slots = GetTankSlots();
    uint8 const mySlot = GetSlotOf(slots, me);
    if (mySlot != m_loggedTankSlot)
    {
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Tank] %s: slot %s -> %s", me->GetName(),
            GetTankSlotName(m_loggedTankSlot).c_str(), GetTankSlotName(mySlot).c_str());
        m_loggedTankSlot = mySlot;
    }

    bool otherTanks = false;
    for (auto const& itr : slots)
        if (itr.first != me->GetObjectGuid())
            otherTanks = true;

    Unit* pVictim = me->GetVictim();
    if (pVictim && !IsValidHostileTarget(pVictim))
        pVictim = nullptr;

    char const* markReason = "";
    Unit* pMark = GetPreferredTarget(markReason);
    if (pMark && m_singleFocus)
    {
        if (pVictim != pMark)
        {
            sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Target] %s: %s -> %s (%s, singlefocus)", me->GetName(),
                pVictim ? pVictim->GetName() : "none", pMark->GetName(), markReason);
            me->AttackStop(true);
            AttackStart(pMark);
        }
        return TauntIfAllowed(pMark, slots, otherTanks);
    }

    // With a mark: free to leave it while it is covered - on us (even with a thin threat lead:
    // enemies already hitting group members come first) or held by another tank.
    Unit* pMarkVictim = pMark ? pMark->GetVictim() : nullptr;
    bool const markHeld = pMarkVictim &&
        (pMarkVictim == me || GetSlotOf(slots, pMarkVictim) != PB_TANK_SLOT_NONE);
    // Back to the mark when it hits someone else; a mark that fights nobody yet (pull target on its
    // way, or not pulled) waits until no loose enemy is left.
    if (pMark && pVictim != pMark && !markHeld && (pMarkVictim || !SelectLooseEnemy(slots)))
    {
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Target] %s: %s -> %s (%s)", me->GetName(),
            pVictim ? pVictim->GetName() : "none", pMark->GetName(), markReason);
        me->AttackStop(true);
        AttackStart(pMark);
        return TauntIfAllowed(pMark, slots, otherTanks);
    }

    // Grab a loose enemy unless the current target is one already.
    bool const victimIsLoose = pVictim && pVictim->GetVictim() && pVictim->GetVictim() != me &&
        GetSlotOf(slots, pVictim->GetVictim()) == PB_TANK_SLOT_NONE;
    if (!victimIsLoose)
    {
        if (Unit* pLoose = SelectLooseEnemy(slots))
        {
            if (pLoose != pVictim)
            {
                sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Tank] %s (%s): grabs %s from %s", me->GetName(),
                    GetTankSlotName(mySlot).c_str(), pLoose->GetName(),
                    pLoose->GetVictim() ? pLoose->GetVictim()->GetName() : "nobody");
                me->AttackStop(true);
                AttackStart(pLoose);
                pVictim = pLoose;
            }
        }
    }

    if (!pVictim)
        return false;

    pVictim = TabForThreat(pVictim);

    if (TauntIfAllowed(pVictim, slots, otherTanks))
        return true;
    return pMark && TauntIfAllowed(pMark, slots, otherTanks);
}

// Damaging area spells only: fears, roots, stuns and spells without damage (shouts, taunts) are left
// to the class code.
bool PartyBotAI::IsDamageAreaSpell(SpellEntry const* pSpellEntry)
{
    if (!pSpellEntry->IsAreaOfEffectSpell() || pSpellEntry->IsPositiveSpell())
        return false;

    bool damage = false;
    for (uint32 i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        switch (pSpellEntry->EffectApplyAuraName[i])
        {
            case SPELL_AURA_MOD_FEAR:
            case SPELL_AURA_MOD_ROOT:
            case SPELL_AURA_MOD_STUN:
            case SPELL_AURA_MOD_CONFUSE:
                return false;
            case SPELL_AURA_PERIODIC_DAMAGE:
                damage = true;
                break;
        }
        switch (pSpellEntry->Effect[i])
        {
            case SPELL_EFFECT_SCHOOL_DAMAGE:
            case SPELL_EFFECT_HEALTH_LEECH:
            case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
            case SPELL_EFFECT_WEAPON_PERCENT_DAMAGE:
            case SPELL_EFFECT_WEAPON_DAMAGE:
            case SPELL_EFFECT_NORMALIZED_WEAPON_DMG:
                damage = true;
                break;
        }
    }
    return damage;
}

bool PartyBotAI::CastAreaOfEffectSpell(Unit* pTarget)
{
    for (auto const& pSpell : m_spells.raw.spells)
    {
        if (!pSpell || !IsDamageAreaSpell(pSpell) || !pSpell->IsTargetInRange(me, pTarget))
            continue;

        if (!CanTryToCastSpell(pTarget, pSpell) || DoCastSpell(pTarget, pSpell) != SPELL_CAST_OK)
            continue;

        if (m_aoeMode == PB_AOE_FORCED)
        {
            // Name crowd-controlled or CC-marked enemies inside the area.
            std::list<Unit*> targets;
            me->GetEnemyListInRadiusAround(GetAreaCenter(me, pTarget, pSpell), GetAreaRadius(pSpell), targets);
            for (auto const& pEnemy : targets)
                if (pEnemy->HasBreakableByDamageCrowdControlAura() || IsForbiddenTarget(pEnemy))
                    sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[AoE] %s: AoE forced, CC mob hit (%s by %s)",
                        me->GetName(), pEnemy->GetName(), pSpell->SpellName[0].c_str());
        }
        return true;
    }
    return false;
}

Unit* PartyBotAI::GetCCMarkedEnemyNearby() const
{
    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return nullptr;

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        PartyBotAI const* pAI = pMember ? dynamic_cast<PartyBotAI const*>(pMember->AI()) : nullptr;
        if (!pAI)
            continue;
        for (auto mark : pAI->m_marksToCC)
        {
            Unit* pEnemy = GetMarkedTarget(mark);
            if (pEnemy && pEnemy->IsAlive() && pEnemy->IsInMap(me) &&
                me->IsWithinDistInMap(pEnemy, PB_AOE_CC_MARK_RANGE) && IsForbiddenTarget(pEnemy))
                return pEnemy;
        }
    }
    return nullptr;
}

void PartyBotAI::SetAoEMode(PartyBotAoEMode mode, char const* why)
{
    static char const* const names[] = { "auto", "on", "off" };
    if (mode == m_aoeMode)
        return;

    sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[AoE] %s: aoe %s -> %s (%s)", me->GetName(), names[m_aoeMode],
        names[mode], why);
    m_aoeMode = mode;
    m_forcedAoEFought = false;
}

void PartyBotAI::EndForcedAoE(char const* why)
{
    if (m_aoeMode == PB_AOE_FORCED)
        SetAoEMode(PB_AOE_AUTO, why);
}

// Forced AoE lasts one fight: it ends once the bot has fought and is out of combat (pack dead).
void PartyBotAI::UpdateForcedAoE()
{
    if (m_aoeMode != PB_AOE_FORCED)
        return;

    if (me->IsInCombat())
        m_forcedAoEFought = true;
    else if (m_forcedAoEFought)
        EndForcedAoE("fight over");
}

void PartyBotAI::UpdateInCombatAI()
{
    if (!IsInDuel())
    {
        if (m_role == ROLE_TANK)
        {
            if (UpdateTankDuty())
                return;
        }
        else if (CrowdControlMarkedTargets())
            return;
    }

    if (CheckForDispelTargets())
        return;

    // Melee and tanks that may not move (stay, move-here spot, toggle): an enemy out of melee
    // reach is shot with the ranged weapon (bow, gun, crossbow, thrown) instead.
    if (!CanMoveOnOwn() && (m_role == ROLE_MELEE_DPS || m_role == ROLE_TANK))
        if (Unit* pVictim = me->GetVictim())
            if (!me->CanReachWithMeleeAutoAttack(pVictim) && ShootRangedWeapon(pVictim))
                return;

    // Area spells of damage dealers: auto on a pack of PartyBot.AutoAoE.MinTargets+ enemies around
    // the target, forced on any pack. CanTryToCastSpell keeps the crowd control and not-yet-pulled
    // rules (forced: not-yet-pulled only).
    if (m_aoeMode != PB_AOE_OFF && !IsInDuel() && (m_role == ROLE_MELEE_DPS || m_role == ROLE_RANGE_DPS))
    {
        if (Unit* pVictim = me->GetVictim())
        {
            uint32 const minTargets = m_aoeMode == PB_AOE_FORCED ? 1 :
                sWorld.getConfig(CONFIG_UINT32_PARTY_BOT_AUTO_AOE_MIN_TARGETS);
            if (me->GetEnemyCountInRadiusAround(pVictim, PB_AUTO_AOE_RADIUS) >= minTargets &&
                CastAreaOfEffectSpell(pVictim))
                return;
        }
    }

    switch (me->GetClass())
    {
        case CLASS_PALADIN:
            UpdateInCombatAI_Paladin();
            break;
        case CLASS_SHAMAN:
            UpdateInCombatAI_Shaman();
            break;
        case CLASS_HUNTER:
            UpdateInCombatAI_Hunter();
            break;
        case CLASS_MAGE:
            UpdateInCombatAI_Mage();
            break;
        case CLASS_PRIEST:
            UpdateInCombatAI_Priest();
            break;
        case CLASS_WARLOCK:
            UpdateInCombatAI_Warlock();
            break;
        case CLASS_WARRIOR:
            UpdateInCombatAI_Warrior();
            break;
        case CLASS_ROGUE:
            UpdateInCombatAI_Rogue();
            break;
        case CLASS_DRUID:
            UpdateInCombatAI_Druid();
            break;
    }

    if (me->GetVictim())
        UseTrinketEffects();
}

bool PartyBotAI::CheckForDispelTargets()
{
    if (me->GetShapeshiftForm() != FORM_NONE)
        return false;

    switch (me->GetClass())
    {
        case CLASS_PALADIN:
        {
            if (m_spells.paladin.pCleanse)
            {
                if (Unit* pFriend = SelectDispelTarget(m_spells.paladin.pCleanse))
                {
                    if (CanTryToCastSpell(pFriend, m_spells.paladin.pCleanse))
                    {
                        if (DoCastSpell(pFriend, m_spells.paladin.pCleanse) == SPELL_CAST_OK)
                            return true;
                    }
                }
            }
            break;
        }
        case CLASS_SHAMAN:
        {
            if (m_spells.shaman.pCureDisease)
            {
                if (Unit* pFriend = SelectDispelTarget(m_spells.shaman.pCureDisease))
                {
                    if (CanTryToCastSpell(pFriend, m_spells.shaman.pCureDisease))
                    {
                        if (DoCastSpell(pFriend, m_spells.shaman.pCureDisease) == SPELL_CAST_OK)
                            return true;
                    }
                }
            }

            if (m_spells.shaman.pCurePoison)
            {
                if (Unit* pFriend = SelectDispelTarget(m_spells.shaman.pCurePoison))
                {
                    if (CanTryToCastSpell(pFriend, m_spells.shaman.pCurePoison))
                    {
                        if (DoCastSpell(pFriend, m_spells.shaman.pCurePoison) == SPELL_CAST_OK)
                            return true;
                    }
                }
            }
            break;
        }
        case CLASS_MAGE:
        {
            if (m_spells.mage.pRemoveLesserCurse)
            {
                if (Unit* pFriend = SelectDispelTarget(m_spells.mage.pRemoveLesserCurse))
                {
                    if (CanTryToCastSpell(pFriend, m_spells.mage.pRemoveLesserCurse))
                    {
                        if (DoCastSpell(pFriend, m_spells.mage.pRemoveLesserCurse) == SPELL_CAST_OK)
                            return true;
                    }
                }
            }
            break;
        }
        case CLASS_PRIEST:
        {
            if (m_spells.priest.pDispelMagic)
            {
                if (Unit* pFriend = SelectDispelTarget(m_spells.priest.pDispelMagic))
                {
                    if (CanTryToCastSpell(pFriend, m_spells.priest.pDispelMagic))
                    {
                        if (DoCastSpell(pFriend, m_spells.priest.pDispelMagic) == SPELL_CAST_OK)
                            return true;
                    }
                }
            }
            if (m_spells.priest.pAbolishDisease)
            {
                if (Unit* pFriend = SelectDispelTarget(m_spells.priest.pAbolishDisease))
                {
                    if (CanTryToCastSpell(pFriend, m_spells.priest.pAbolishDisease))
                    {
                        if (DoCastSpell(pFriend, m_spells.priest.pAbolishDisease) == SPELL_CAST_OK)
                            return true;
                    }
                }
            }
            break;
        }
        case CLASS_DRUID:
        {
            SpellEntry const* pDispelSpell = m_spells.druid.pAbolishPoison ?
                m_spells.druid.pAbolishPoison :
                m_spells.druid.pCurePoison;

            if (pDispelSpell)
            {
                if (Unit* pFriend = SelectDispelTarget(pDispelSpell))
                {
                    if (CanTryToCastSpell(pFriend, pDispelSpell))
                    {
                        if (DoCastSpell(pFriend, pDispelSpell) == SPELL_CAST_OK)
                            return true;
                    }
                }
            }

            if (m_spells.druid.pRemoveCurse)
            {
                if (Unit* pFriend = SelectDispelTarget(m_spells.druid.pRemoveCurse))
                {
                    if (CanTryToCastSpell(pFriend, m_spells.druid.pRemoveCurse))
                    {
                        if (DoCastSpell(pFriend, m_spells.druid.pRemoveCurse) == SPELL_CAST_OK)
                            return true;
                    }
                }
            }
            break;
        }
    }

    return false;
}

void PartyBotAI::UpdateOutOfCombatAI_Paladin()
{
    if (m_spells.paladin.pAura &&
        CanTryToCastSpell(me, m_spells.paladin.pAura))
    {
        if (DoCastSpell(me, m_spells.paladin.pAura) == SPELL_CAST_OK)
            return;
    }

    if (m_role == ROLE_TANK &&
        m_spells.paladin.pRighteousFury &&
        CanTryToCastSpell(me, m_spells.paladin.pRighteousFury))
    {
        if (DoCastSpell(me, m_spells.paladin.pRighteousFury) == SPELL_CAST_OK)
            return;
    }

    if (m_spells.paladin.pBlessingBuff)
    {
        if (Player* pTarget = SelectBuffTarget(m_spells.paladin.pBlessingBuff))
        {
            if (CanTryToCastSpell(pTarget, m_spells.paladin.pBlessingBuff))
            {
                if (DoCastSpell(pTarget, m_spells.paladin.pBlessingBuff) == SPELL_CAST_OK)
                {
                    m_isBuffing = true;
                    me->ClearTarget();
                    return;
                }
            }
        }
    }

    if (m_isBuffing &&
       (!m_spells.paladin.pBlessingBuff ||
        !me->HasGCD(m_spells.paladin.pBlessingBuff)))
    {
        m_isBuffing = false;
    }

    if (m_role == ROLE_HEALER &&
        FindAndHealInjuredAlly())
        return;
}

void PartyBotAI::UpdateInCombatAI_Paladin()
{
    if (m_spells.paladin.pDivineShield &&
       (me->GetHealthPercent() < 20.0f) &&
       (m_role != ROLE_TANK) &&
        CanTryToCastSpell(me, m_spells.paladin.pDivineShield))
    {
        if (DoCastSpell(me, m_spells.paladin.pDivineShield) == SPELL_CAST_OK)
            return;
    }

    if (Unit* pFriend = me->FindLowestHpFriendlyUnit(30.0f, 70, true, me))
    {
        if (m_spells.paladin.pBlessingOfProtection &&
           !IsPhysicalDamageClass(pFriend->GetClass()) &&
            CanTryToCastSpell(pFriend, m_spells.paladin.pBlessingOfProtection))
        {
            if (DoCastSpell(pFriend, m_spells.paladin.pBlessingOfProtection) == SPELL_CAST_OK)
                return;
        }
        if (m_spells.paladin.pBlessingOfSacrifice &&
           (me->GetHealthPercent() > 80.0f) &&
            CanTryToCastSpell(pFriend, m_spells.paladin.pBlessingOfSacrifice))
        {
            if (DoCastSpell(pFriend, m_spells.paladin.pBlessingOfSacrifice) == SPELL_CAST_OK)
                return;
        }
        if (m_spells.paladin.pLayOnHands &&
           (pFriend->GetHealthPercent() < 15.0f) &&
            CanTryToCastSpell(pFriend, m_spells.paladin.pLayOnHands))
        {
            if (DoCastSpell(pFriend, m_spells.paladin.pLayOnHands) == SPELL_CAST_OK)
                return;
        }
    }

    if (!me->GetAttackers().empty())
    {
        if (m_spells.paladin.pHolyShield &&
            CanTryToCastSpell(me, m_spells.paladin.pHolyShield))
        {
            if (DoCastSpell(me, m_spells.paladin.pHolyShield) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.paladin.pTurnUndead &&
            m_role != ROLE_TANK)
        {
            Unit* pAttacker = SelectAttackerDifferentFrom(me->GetVictim());
            if (pAttacker && pAttacker->GetCreatureType() == CREATURE_TYPE_UNDEAD &&
                CanTryToCastSpell(pAttacker, m_spells.paladin.pTurnUndead))
            {
                if (DoCastSpell(pAttacker, m_spells.paladin.pTurnUndead) == SPELL_CAST_OK)
                    return;
            }
        }
    }

    if (GetRole() == ROLE_HEALER)
    {
        if (m_spells.paladin.pHolyShock &&
            me->GetHealthPercent() < 50.0f &&
            CanTryToCastSpell(me, m_spells.paladin.pHolyShock))
        {
            if (m_spells.paladin.pDivineFavor &&
                CanTryToCastSpell(me, m_spells.paladin.pDivineFavor))
            {
                DoCastSpell(me, m_spells.paladin.pDivineFavor);
            }

            if (DoCastSpell(me, m_spells.paladin.pHolyShock) == SPELL_CAST_OK)
                return;
        }

        if (FindAndHealInjuredAlly(80.0f, 90.0f))
            return;

        if (FindAndPreHealTarget())
            return;
    }
    else
    {
        if (m_spells.paladin.pLayOnHands &&
           (me->GetHealthPercent() < 15.0f) &&
            CanTryToCastSpell(me, m_spells.paladin.pLayOnHands))
        {
            if (DoCastSpell(me, m_spells.paladin.pLayOnHands) == SPELL_CAST_OK)
                return;
        }

        bool const hasSeal = m_spells.paladin.pSeal && me->HasAura(m_spells.paladin.pSeal->Id);

        if (!hasSeal &&
            m_spells.paladin.pSeal &&
            CanTryToCastSpell(me, m_spells.paladin.pSeal))
        {
            me->CastSpell(me, m_spells.paladin.pSeal, false);
        }

        if (Unit* pVictim = me->GetVictim())
        {
            if (hasSeal && m_spells.paladin.pJudgement &&
               (me->GetPowerPercent(POWER_MANA) > 30.0f) &&
                CanTryToCastSpell(pVictim, m_spells.paladin.pJudgement))
            {
                if (DoCastSpell(pVictim, m_spells.paladin.pJudgement) == SPELL_CAST_OK)
                    return;
            }
            if (m_spells.paladin.pHammerOfJustice &&
               (pVictim->IsNonMeleeSpellCasted() ||
               (me->GetHealthPercent() < 20.0f && !me->GetAttackers().empty())) &&
                CanTryToCastSpell(pVictim, m_spells.paladin.pHammerOfJustice))
            {
                if (DoCastSpell(pVictim, m_spells.paladin.pHammerOfJustice) == SPELL_CAST_OK)
                    return;
            }
            if (m_spells.paladin.pHammerOfWrath &&
                pVictim->GetHealthPercent() < 20.0f &&
                CanTryToCastSpell(pVictim, m_spells.paladin.pHammerOfWrath))
            {
                if (DoCastSpell(pVictim, m_spells.paladin.pHammerOfWrath) == SPELL_CAST_OK)
                    return;
            }
            if (m_spells.paladin.pConsecration &&
               (GetAttackersInRangeCount(10.0f) > 2) &&
                CanTryToCastSpell(me, m_spells.paladin.pConsecration))
            {
                if (DoCastSpell(me, m_spells.paladin.pConsecration) == SPELL_CAST_OK)
                    return;
            }
            if (m_spells.paladin.pHolyShock &&
                CanTryToCastSpell(pVictim, m_spells.paladin.pHolyShock))
            {
                if (m_spells.paladin.pDivineFavor &&
                    CanTryToCastSpell(me, m_spells.paladin.pDivineFavor))
                {
                    DoCastSpell(me, m_spells.paladin.pDivineFavor);
                }

                if (DoCastSpell(pVictim, m_spells.paladin.pHolyShock) == SPELL_CAST_OK)
                    return;
            }
            if (m_spells.paladin.pExorcism &&
                pVictim->IsCreature() &&
                (pVictim->GetCreatureType() == CREATURE_TYPE_UNDEAD) &&
                CanTryToCastSpell(pVictim, m_spells.paladin.pExorcism))
            {
                if (DoCastSpell(pVictim, m_spells.paladin.pExorcism) == SPELL_CAST_OK)
                    return;
            }
            if (m_spells.paladin.pHolyWrath &&
                pVictim->IsCreature() &&
               (pVictim->GetCreatureType() == CREATURE_TYPE_UNDEAD ||
                pVictim->GetCreatureType() == CREATURE_TYPE_DEMON) &&
               (me->GetAttackers().size() < 3) && // too much pushback
                CanTryToCastSpell(pVictim, m_spells.paladin.pHolyWrath))
            {
                if (DoCastSpell(pVictim, m_spells.paladin.pHolyWrath) == SPELL_CAST_OK)
                    return;
            }
            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
                && !me->CanReachWithMeleeAutoAttack(pVictim))
            {
                BeginChasing(pVictim);
            }
        }
    }

    if (m_spells.paladin.pBlessingOfFreedom &&
       (me->HasUnitState(UNIT_STATE_ROOT) || me->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED)) &&
        CanTryToCastSpell(me, m_spells.paladin.pBlessingOfFreedom))
    {
        if (DoCastSpell(me, m_spells.paladin.pBlessingOfFreedom) == SPELL_CAST_OK)
            return;
    }

    if (GetRole() != ROLE_HEALER &&
        me->GetHealthPercent() < 30.0f)
        HealInjuredTarget(me);
}

void PartyBotAI::UpdateOutOfCombatAI_Shaman()
{
    if (m_spells.shaman.pWeaponBuff &&
        CanTryToCastSpell(me, m_spells.shaman.pWeaponBuff))
    {
        if (CastWeaponBuff(m_spells.shaman.pWeaponBuff, EQUIPMENT_SLOT_MAINHAND) == SPELL_CAST_OK)
            return;
    }

    if (m_spells.shaman.pLightningShield &&
        CanTryToCastSpell(me, m_spells.shaman.pLightningShield))
    {
        if (DoCastSpell(me, m_spells.shaman.pLightningShield) == SPELL_CAST_OK)
            return;
    }

    if (m_role == ROLE_HEALER &&
        FindAndHealInjuredAlly())
        return;

    if (me->GetVictim())
    {
        if (SummonShamanTotems())
            return;

        UpdateInCombatAI_Shaman();
    }
}

void PartyBotAI::UpdateInCombatAI_Shaman()
{
    if (m_spells.shaman.pManaTideTotem &&
       (me->GetPowerPercent(POWER_MANA) < 50.0f) &&
        CanTryToCastSpell(me, m_spells.shaman.pManaTideTotem))
    {
        if (DoCastSpell(me, m_spells.shaman.pManaTideTotem) == SPELL_CAST_OK)
            return;
    }

    if (GetRole() != ROLE_HEALER)
    {
        if (Unit* pVictim = me->GetVictim())
        {
            if (m_spells.shaman.pElementalMastery &&
                me->GetAttackers().empty() &&
                CanTryToCastSpell(me, m_spells.shaman.pElementalMastery))
            {
                if (DoCastSpell(me, m_spells.shaman.pElementalMastery) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.shaman.pEarthShock &&
                pVictim->IsNonMeleeSpellCasted(false, false, true) &&
                CanTryToCastSpell(pVictim, m_spells.shaman.pEarthShock))
            {
                if (DoCastSpell(pVictim, m_spells.shaman.pEarthShock) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.shaman.pFrostShock &&
                pVictim->IsMoving() &&
                CanTryToCastSpell(pVictim, m_spells.shaman.pFrostShock))
            {
                if (DoCastSpell(pVictim, m_spells.shaman.pFrostShock) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.shaman.pStormstrike &&
                CanTryToCastSpell(pVictim, m_spells.shaman.pStormstrike))
            {
                if (DoCastSpell(pVictim, m_spells.shaman.pStormstrike) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.shaman.pChainLightning &&
                CanTryToCastSpell(pVictim, m_spells.shaman.pChainLightning))
            {
                if (DoCastSpell(pVictim, m_spells.shaman.pChainLightning) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.shaman.pPurge &&
                IsValidDispelTarget(pVictim, m_spells.shaman.pPurge) &&
                CanTryToCastSpell(pVictim, m_spells.shaman.pPurge))
            {
                if (DoCastSpell(pVictim, m_spells.shaman.pPurge) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.shaman.pFlameShock &&
                CanTryToCastSpell(pVictim, m_spells.shaman.pFlameShock))
            {
                if (DoCastSpell(pVictim, m_spells.shaman.pFlameShock) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.shaman.pLightningBolt &&
               (GetRole() == ROLE_RANGE_DPS || !me->CanReachWithMeleeAutoAttack(pVictim)) &&
                CanTryToCastSpell(pVictim, m_spells.shaman.pLightningBolt))
            {
                if (DoCastSpell(pVictim, m_spells.shaman.pLightningBolt) == SPELL_CAST_OK)
                    return;
            }
        }
    }

    if (SummonShamanTotems())
        return;

    if (GetRole() == ROLE_HEALER)
    {
        if (FindAndHealInjuredAlly(50.0f, 90.0f))
            return;

        if (FindAndPreHealTarget())
            return;
    }
    else if (me->GetHealthPercent() < 20.0f)
        HealInjuredTarget(me);
}

// Hunter pet upkeep: passive (attacks only on the hunter's order, so it waits out a pull timer
// like the hunter), level = hunter level, always happy (bots carry no pet food).
void PartyBotAI::MaintainHunterPet()
{
    Pet* pPet = me->GetPet();
    if (!pPet || !pPet->IsAlive() || pPet->GetPetType() != HUNTER_PET)
        return;

    if (!pPet->HasReactState(REACT_PASSIVE))
    {
        pPet->SetReactState(REACT_PASSIVE);
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Pet] %s: pet %s (level %u) set passive",
            me->GetName(), pPet->GetName(), pPet->GetLevel());
    }

    if (pPet->GetLevel() != me->GetLevel())
    {
        uint32 const oldLevel = pPet->GetLevel();
        pPet->GivePetLevel(me->GetLevel());
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Pet] %s: pet %s level %u -> %u (hunter %u)",
            me->GetName(), pPet->GetName(), oldLevel, pPet->GetLevel(), me->GetLevel());
    }

    if (pPet->GetPower(POWER_HAPPINESS) < pPet->GetMaxPower(POWER_HAPPINESS))
        pPet->SetPower(POWER_HAPPINESS, pPet->GetMaxPower(POWER_HAPPINESS));
}

// The pet attacks what the hunter attacks (passive pets only act on orders).
void PartyBotAI::CommandPetAttack(Unit* pVictim)
{
    Pet* pPet = me->GetPet();
    if (!pPet || !pPet->IsAlive() || pPet->GetVictim() == pVictim || !pPet->GetCharmInfo())
        return;

    pPet->GetCharmInfo()->SetIsCommandAttack(true);
    pPet->AI()->AttackStart(pVictim);
}

void PartyBotAI::UpdateOutOfCombatAI_Hunter()
{
    MaintainHunterPet();

    if (m_spells.hunter.pAspectOfTheHawk &&
        CanTryToCastSpell(me, m_spells.hunter.pAspectOfTheHawk))
    {
        if (DoCastSpell(me, m_spells.hunter.pAspectOfTheHawk) == SPELL_CAST_OK)
            return;
    }

    if (Unit* pVictim = me->GetVictim())
    {
        if (m_spells.hunter.pHuntersMark &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pHuntersMark))
        {
            if (DoCastSpell(pVictim, m_spells.hunter.pHuntersMark) == SPELL_CAST_OK)
                return;
        }

        CommandPetAttack(pVictim);

        UpdateInCombatAI_Hunter();
    }
    else
        SummonPetIfNeeded();
}

void PartyBotAI::UpdateInCombatAI_Hunter()
{
    MaintainHunterPet();

    if (Unit* pVictim = me->GetVictim())
    {
        CommandPetAttack(pVictim);

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
            && me->GetDistance(pVictim) > 30.0f)
        {
            BeginChasing(pVictim);
        }

        if (m_spells.hunter.pVolley &&
           (me->GetEnemyCountInRadiusAround(pVictim, 10.0f) > 2) &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pVolley))
        {
            if (DoCastSpell(pVictim, m_spells.hunter.pVolley) == SPELL_CAST_OK)
                return;
        }

        if (me->HasSpell(PB_SPELL_AUTO_SHOT) &&
            !me->IsMoving() &&
            (me->GetCombatDistance(pVictim) > 8.0f) &&
            !me->IsNonMeleeSpellCasted())
        {
            switch (me->CastSpell(pVictim, PB_SPELL_AUTO_SHOT, false))
            {
                case SPELL_FAILED_NEED_AMMO:
                case SPELL_FAILED_NO_AMMO:
                {
                    AddRangedAmmo();
                    break;
                }
            }
        }

        if (m_spells.hunter.pConcussiveShot &&
            pVictim->IsMoving() && (pVictim->GetVictim() == me) &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pConcussiveShot))
        {
            if (DoCastSpell(pVictim, m_spells.hunter.pConcussiveShot) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.hunter.pAimedShot &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pAimedShot))
        {
            if (DoCastSpell(pVictim, m_spells.hunter.pAimedShot) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.hunter.pArcaneShot &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pArcaneShot))
        {
            if (DoCastSpell(pVictim, m_spells.hunter.pArcaneShot) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.hunter.pSerpentSting &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pSerpentSting))
        {
            if (DoCastSpell(pVictim, m_spells.hunter.pSerpentSting) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.hunter.pMultiShot &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pMultiShot))
        {
            if (DoCastSpell(pVictim, m_spells.hunter.pMultiShot) == SPELL_CAST_OK)
                return;
        }

        if (GetAttackersInRangeCount(8.0f))
        {
            Unit* pAttacker = *me->GetAttackers().begin();

            if (m_spells.hunter.pScareBeast &&
                CanTryToCastSpell(pAttacker, m_spells.hunter.pScareBeast))
            {
                if (DoCastSpell(pAttacker, m_spells.hunter.pScareBeast) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.hunter.pDisengage &&
                CanTryToCastSpell(pAttacker, m_spells.hunter.pDisengage))
            {
                if (DoCastSpell(pAttacker, m_spells.hunter.pDisengage) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.hunter.pAspectOfTheMonkey &&
                CanTryToCastSpell(me, m_spells.hunter.pAspectOfTheMonkey))
            {
                if (DoCastSpell(me, m_spells.hunter.pAspectOfTheMonkey) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.hunter.pFeignDeath &&
               (me->GetHealthPercent() < 20.0f) &&
                CanTryToCastSpell(me, m_spells.hunter.pFeignDeath))
            {
                if (DoCastSpell(me, m_spells.hunter.pFeignDeath) == SPELL_CAST_OK)
                    return;
            }
        }

        if (pVictim->CanReachWithMeleeAutoAttack(me))
        {
            if (m_spells.hunter.pWingClip &&
                CanTryToCastSpell(pVictim, m_spells.hunter.pWingClip))
            {
                DoCastSpell(pVictim, m_spells.hunter.pWingClip);
            }

            if (m_spells.hunter.pMongooseBite &&
                CanTryToCastSpell(pVictim, m_spells.hunter.pMongooseBite))
            {
                DoCastSpell(pVictim, m_spells.hunter.pMongooseBite);
            }

            if (m_spells.hunter.pRaptorStrike &&
                CanTryToCastSpell(pVictim, m_spells.hunter.pRaptorStrike))
            {
                DoCastSpell(pVictim, m_spells.hunter.pRaptorStrike);
            }
        }
        else
        {
            if (m_spells.hunter.pAspectOfTheHawk &&
                CanTryToCastSpell(me, m_spells.hunter.pAspectOfTheHawk))
            {
                if (DoCastSpell(me, m_spells.hunter.pAspectOfTheHawk) == SPELL_CAST_OK)
                    return;
            }
        }
    }
}

void PartyBotAI::UpdateOutOfCombatAI_Mage()
{
    SpellEntry const* pBuffSpell = nullptr;
    if (Player* pTarget = SelectBuffTarget(m_spells.mage.pArcaneIntellect, m_spells.mage.pArcaneBrilliance, pBuffSpell))
    {
        if (CanTryToCastSpell(pTarget, pBuffSpell))
        {
            if (DoCastSpell(pTarget, pBuffSpell) == SPELL_CAST_OK)
            {
                m_isBuffing = true;
                me->ClearTarget();
                return;
            }
        }
    }

    if (m_spells.mage.pIceArmor &&
        CanTryToCastSpell(me, m_spells.mage.pIceArmor))
    {
        if (DoCastSpell(me, m_spells.mage.pIceArmor) == SPELL_CAST_OK)
        {
            m_isBuffing = true;
            me->ClearTarget();
            return;
        }
    }

    if (m_spells.mage.pIceBarrier &&
        CanTryToCastSpell(me, m_spells.mage.pIceBarrier))
    {
        if (DoCastSpell(me, m_spells.mage.pIceBarrier) == SPELL_CAST_OK)
        {
            m_isBuffing = true;
            me->ClearTarget();
            return;
        }
    }

    if (m_isBuffing &&
       (!m_spells.mage.pArcaneIntellect ||
        !me->HasGCD(m_spells.mage.pArcaneIntellect)))
    {
        m_isBuffing = false;
    }

    if (me->GetVictim())
        UpdateInCombatAI_Mage();
}

void PartyBotAI::UpdateInCombatAI_Mage()
{
    if (Unit* pVictim = me->GetVictim())
    {
        if (UseManaGem())
            return;

        if (m_spells.mage.pCombustion &&
            CanTryToCastSpell(me, m_spells.mage.pCombustion))
        {
            if (DoCastSpell(me, m_spells.mage.pCombustion) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pPyroblast &&
           ((m_spells.mage.pPresenceOfMind && me->HasAura(m_spells.mage.pPresenceOfMind->Id)) ||
            (!pVictim->IsInCombat() && (pVictim->GetMaxHealth() > me->GetMaxHealth()) && (me->GetDistance(pVictim) > 30.0f))) &&
            CanTryToCastSpell(pVictim, m_spells.mage.pPyroblast))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pPyroblast) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pIceBlock &&
           (me->GetHealthPercent() < 10.0f) &&
            CanTryToCastSpell(me, m_spells.mage.pIceBlock))
        {
            if (DoCastSpell(me, m_spells.mage.pIceBlock) == SPELL_CAST_OK)
                return;
        }

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
            && me->GetDistance(pVictim) > 30.0f)
        {
            BeginChasing(pVictim);
        }
        else if (GetAttackersInRangeCount(10.0f))
        {
            // Party bots never run away from what hits them (the tank comes for it); they
            // shield or root it and keep casting.
            if (m_spells.mage.pManaShield &&
               (me->GetPowerPercent(POWER_MANA) > 20.0f) &&
                CanTryToCastSpell(me, m_spells.mage.pManaShield))
            {
                if (DoCastSpell(me, m_spells.mage.pManaShield) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.mage.pFrostNova &&
               !pVictim->HasUnitState(UNIT_STATE_ROOT) &&
               !pVictim->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL) &&
                CanTryToCastSpell(me, m_spells.mage.pFrostNova))
            {
                if (DoCastSpell(me, m_spells.mage.pFrostNova) == SPELL_CAST_OK)
                    return;
            }
        }

        if (me->GetEnemyCountInRadiusAround(me, 10.0f) > 1)
        {
            if (m_spells.mage.pConeofCold && !me->IsMoving() &&
                CanTryToCastSpell(me, m_spells.mage.pConeofCold))
            {
                if (DoCastSpell(pVictim, m_spells.mage.pConeofCold) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.mage.pBlastWave &&
                CanTryToCastSpell(me, m_spells.mage.pBlastWave))
            {
                if (DoCastSpell(me, m_spells.mage.pBlastWave) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.mage.pArcaneExplosion &&
                CanTryToCastSpell(me, m_spells.mage.pArcaneExplosion))
            {
                if (DoCastSpell(me, m_spells.mage.pArcaneExplosion) == SPELL_CAST_OK)
                    return;
            }
        }

        if (m_spells.mage.pCounterspell &&
            pVictim->IsNonMeleeSpellCasted(false, false, true) &&
            CanTryToCastSpell(pVictim, m_spells.mage.pCounterspell))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pCounterspell) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pBlizzard &&
           (me->GetEnemyCountInRadiusAround(pVictim, 10.0f) > 2) &&
            CanTryToCastSpell(pVictim, m_spells.mage.pBlizzard))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pBlizzard) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pPolymorph)
        {
            if (Unit* pTarget = SelectAttackerDifferentFrom(pVictim))
            {
                if (pTarget->GetHealthPercent() > 20.0f &&
                    CanTryToCastSpell(pTarget, m_spells.mage.pPolymorph) &&
                    CanUseCrowdControl(m_spells.mage.pPolymorph, pTarget))
                {
                    if (DoCastSpell(pTarget, m_spells.mage.pPolymorph) == SPELL_CAST_OK)
                        return;
                }
            }
        }

        if (m_spells.mage.pArcanePower &&
            (me->GetPowerPercent(POWER_MANA) > 50.0f) &&
            CanTryToCastSpell(me, m_spells.mage.pArcanePower))
        {
            if (DoCastSpell(me, m_spells.mage.pArcanePower) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pPresenceOfMind &&
           (me->GetPowerPercent(POWER_MANA) > 50.0f) &&
            CanTryToCastSpell(me, m_spells.mage.pPresenceOfMind))
        {
            if (DoCastSpell(me, m_spells.mage.pPresenceOfMind) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pScorch &&
           (pVictim->GetHealthPercent() < 20.0f) &&
            CanTryToCastSpell(pVictim, m_spells.mage.pScorch))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pScorch) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pFrostbolt &&
            CanTryToCastSpell(pVictim, m_spells.mage.pFrostbolt))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pFrostbolt) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pFireBlast &&
            CanTryToCastSpell(pVictim, m_spells.mage.pFireBlast))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pFireBlast) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pFireball &&
            CanTryToCastSpell(pVictim, m_spells.mage.pFireball))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pFireball) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pEvocation &&
           (me->GetPowerPercent(POWER_MANA) < 30.0f) &&
           (GetAttackersInRangeCount(10.0f) == 0) &&
            CanTryToCastSpell(me, m_spells.mage.pEvocation))
        {
            if (DoCastSpell(me, m_spells.mage.pEvocation) == SPELL_CAST_OK)
                return;
        }

        if (me->HasSpell(PB_SPELL_SHOOT_WAND) &&
           !me->IsMoving() &&
           (me->GetPowerPercent(POWER_MANA) < 5.0f) &&
           !me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
            me->CastSpell(pVictim, PB_SPELL_SHOOT_WAND, false);
    }
}

void PartyBotAI::UpdateOutOfCombatAI_Priest()
{
    SpellEntry const* pBuffSpell = nullptr;
    if (Player* pTarget = SelectBuffTarget(m_spells.priest.pPowerWordFortitude, m_spells.priest.pPrayerofFortitude, pBuffSpell))
    {
        if (CanTryToCastSpell(pTarget, pBuffSpell))
        {
            if (DoCastSpell(pTarget, pBuffSpell) == SPELL_CAST_OK)
            {
                m_isBuffing = true;
                me->ClearTarget();
                return;
            }
        }
    }

    if (Player* pTarget = SelectBuffTarget(m_spells.priest.pDivineSpirit, m_spells.priest.pPrayerofSpirit, pBuffSpell))
    {
        if (CanTryToCastSpell(pTarget, pBuffSpell))
        {
            if (DoCastSpell(pTarget, pBuffSpell) == SPELL_CAST_OK)
            {
                m_isBuffing = true;
                me->ClearTarget();
                return;
            }
        }
    }

    if (Player* pTarget = SelectBuffTarget(m_spells.priest.pShadowProtection, m_spells.priest.pPrayerofShadowProtection, pBuffSpell))
    {
        if (CanTryToCastSpell(pTarget, pBuffSpell))
        {
            if (DoCastSpell(pTarget, pBuffSpell) == SPELL_CAST_OK)
            {
                m_isBuffing = true;
                me->ClearTarget();
                return;
            }
        }
    }

    if (m_spells.priest.pInnerFire &&
        CanTryToCastSpell(me, m_spells.priest.pInnerFire))
    {
        if (DoCastSpell(me, m_spells.priest.pInnerFire) == SPELL_CAST_OK)
        {
            m_isBuffing = true;
            me->ClearTarget();
            return;
        }
    }

    if (m_isBuffing &&
       (!m_spells.priest.pPowerWordFortitude ||
        !me->HasGCD(m_spells.priest.pPowerWordFortitude)))
    {
        m_isBuffing = false;
    }

    if (m_role == ROLE_HEALER &&
        FindAndHealInjuredAlly())
        return;

    if (me->GetVictim())
        UpdateInCombatAI_Priest();
}

void PartyBotAI::UpdateInCombatAI_Priest()
{
    // Shield self only when hit or hurt, not at every pull.
    if (m_spells.priest.pPowerWordShield &&
       (!me->GetAttackers().empty() || me->GetHealthPercent() < 50.0f) &&
        CanTryToCastSpell(me, m_spells.priest.pPowerWordShield))
    {
        if (DoCastSpell(me, m_spells.priest.pPowerWordShield) == SPELL_CAST_OK)
            return;
    }

    if (!me->GetAttackers().empty() &&
        m_role != ROLE_TANK)
    {
        if (m_spells.priest.pFade &&
            CanTryToCastSpell(me, m_spells.priest.pFade))
        {
            if (DoCastSpell(me, m_spells.priest.pFade) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pShackleUndead)
        {
            Unit* pAttacker = *me->GetAttackers().begin();
            if ((pAttacker->GetHealth() > me->GetHealth()) &&
                CanTryToCastSpell(pAttacker, m_spells.priest.pShackleUndead) &&
                CanUseCrowdControl(m_spells.priest.pShackleUndead, pAttacker))
            {
                if (DoCastSpell(pAttacker, m_spells.priest.pShackleUndead) == SPELL_CAST_OK)
                    return;
            }
        }
    }

    if (m_spells.priest.pInnerFocus &&
       (me->GetPowerPercent(POWER_MANA) < 50.0f) &&
        CanTryToCastSpell(me, m_spells.priest.pInnerFocus))
    {
        DoCastSpell(me, m_spells.priest.pInnerFocus);
    }

    if (GetRole() == ROLE_HEALER || (!me->GetVictim() && me->GetShapeshiftForm() == FORM_NONE))
    {
        // Shield allies being attacked.
        if (m_spells.priest.pPowerWordShield)
        {
            if (Player* pTarget = SelectShieldTarget())
            {
                if (CanTryToCastSpell(pTarget, m_spells.priest.pPowerWordShield))
                {
                    if (DoCastSpell(pTarget, m_spells.priest.pPowerWordShield) == SPELL_CAST_OK)
                        return;
                }
            }
        }

        // Direct heal more seriously injured.
        if (Unit* pTarget = SelectHealTarget(60.0f, 80.0f))
            if (HealInjuredTargetDirect(pTarget))
                return;

        // Apply HoT aura for small injuries.
        if (Unit* pTarget = SelectPeriodicHealTarget(80.0f, 90.0f))
            if (HealInjuredTargetPeriodic(pTarget))
                return;

        if (GetRole() == ROLE_HEALER && FindAndPreHealTarget())
            return;
    }
    else if (Unit* pVictim = me->GetVictim())
    {
        if (m_spells.priest.pShadowform &&
            CanTryToCastSpell(me, m_spells.priest.pShadowform))
        {
            if (DoCastSpell(me, m_spells.priest.pShadowform) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pSilence &&
            pVictim->IsNonMeleeSpellCasted() &&
            CanTryToCastSpell(pVictim, m_spells.priest.pSilence))
        {
            if (DoCastSpell(pVictim, m_spells.priest.pSilence) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pVampiricEmbrace &&
            CanTryToCastSpell(pVictim, m_spells.priest.pVampiricEmbrace))
        {
            if (DoCastSpell(pVictim, m_spells.priest.pVampiricEmbrace) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pMindBlast &&
            CanTryToCastSpell(pVictim, m_spells.priest.pMindBlast))
        {
            if (DoCastSpell(pVictim, m_spells.priest.pMindBlast) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pShadowWordPain &&
            CanTryToCastSpell(pVictim, m_spells.priest.pShadowWordPain))
        {
            if (DoCastSpell(pVictim, m_spells.priest.pShadowWordPain) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pDevouringPlague &&
            CanTryToCastSpell(pVictim, m_spells.priest.pDevouringPlague))
        {
            if (DoCastSpell(pVictim, m_spells.priest.pDevouringPlague) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pPsychicScream &&
            GetAttackersInRangeCount(10.0f) &&
            CanTryToCastSpell(me, m_spells.priest.pPsychicScream))
        {
            if (DoCastSpell(me, m_spells.priest.pPsychicScream) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pManaBurn &&
           (pVictim->GetPowerType() == POWER_MANA) &&
            CanTryToCastSpell(pVictim, m_spells.priest.pManaBurn))
        {
            if (DoCastSpell(pVictim, m_spells.priest.pManaBurn) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pMindFlay &&
           (!GetAttackersInRangeCount(10.0f) || me->HasAuraType(SPELL_AURA_SCHOOL_ABSORB)) &&
            CanTryToCastSpell(pVictim, m_spells.priest.pMindFlay))
        {
            if (DoCastSpell(pVictim, m_spells.priest.pMindFlay) == SPELL_CAST_OK)
                return;
        }

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
            && me->GetDistance(pVictim) > 30.0f)
        {
            BeginChasing(pVictim);
        }

        if (me->GetShapeshiftForm() == FORM_NONE)
        {
            if (m_spells.priest.pHolyNova &&
                GetAttackersInRangeCount(10.0f) > 2 &&
                CanTryToCastSpell(me, m_spells.priest.pHolyNova))
            {
                if (DoCastSpell(me, m_spells.priest.pHolyNova) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.priest.pSmite &&
                CanTryToCastSpell(pVictim, m_spells.priest.pSmite))
            {
                if (DoCastSpell(pVictim, m_spells.priest.pSmite) == SPELL_CAST_OK)
                    return;
            }
        }

        if (me->HasSpell(PB_SPELL_SHOOT_WAND) &&
           !me->IsMoving() &&
           (me->GetPowerPercent(POWER_MANA) < 10.0f) &&
           !me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
            me->CastSpell(pVictim, PB_SPELL_SHOOT_WAND, false);
    }
}

void PartyBotAI::UpdateOutOfCombatAI_Warlock()
{
    if (m_spells.warlock.pDetectInvisibility)
    {
        if (Player* pTarget = SelectBuffTarget(m_spells.warlock.pDetectInvisibility))
        {
            if (CanTryToCastSpell(pTarget, m_spells.warlock.pDetectInvisibility))
            {
                if (DoCastSpell(pTarget, m_spells.warlock.pDetectInvisibility) == SPELL_CAST_OK)
                {
                    m_isBuffing = true;
                    me->ClearTarget();
                    return;
                }
            }
        }
    }

    if (m_spells.warlock.pDemonArmor &&
        CanTryToCastSpell(me, m_spells.warlock.pDemonArmor))
    {
        if (DoCastSpell(me, m_spells.warlock.pDemonArmor) == SPELL_CAST_OK)
        {
            m_isBuffing = true;
            me->ClearTarget();
            return;
        }
    }

    if (m_isBuffing &&
       (!m_spells.warlock.pDetectInvisibility ||
        !me->HasGCD(m_spells.warlock.pDetectInvisibility)))
    {
        m_isBuffing = false;
    }

    if (Unit* pVictim = me->GetVictim())
    {
        if (Pet* pPet = me->GetPet())
        {
            if (!pPet->GetVictim())
            {
                pPet->GetCharmInfo()->SetIsCommandAttack(true);
                pPet->AI()->AttackStart(pVictim);
            }
        }

        UpdateInCombatAI_Warlock();
    }
    else
        SummonPetIfNeeded();
}

void PartyBotAI::UpdateInCombatAI_Warlock()
{
    if (Unit* pVictim = me->GetVictim())
    {
        if (m_spells.warlock.pDeathCoil &&
           (pVictim->CanReachWithMeleeAutoAttack(me) || pVictim->IsNonMeleeSpellCasted()) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pDeathCoil))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pDeathCoil) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pShadowburn &&
           (pVictim->GetHealthPercent() < 10.0f) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pShadowburn))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pShadowburn) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pSearingPain &&
           (pVictim->GetHealthPercent() < 20.0f) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pSearingPain))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pSearingPain) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pBanish &&
            me->GetAttackers().size() > 1)
        {
            Unit* pAttacker = *me->GetAttackers().begin();
            if ((pAttacker->GetHealth() > me->GetHealth()) &&
                CanTryToCastSpell(pAttacker, m_spells.warlock.pBanish))
            {
                if (DoCastSpell(pAttacker, m_spells.warlock.pBanish) == SPELL_CAST_OK)
                    return;
            }
        }

        if (m_spells.warlock.pRainOfFire &&
           (me->GetEnemyCountInRadiusAround(pVictim, 10.0f) > 2) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pRainOfFire))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pRainOfFire) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pDemonicSacrifice)
        {
            if (Pet* pPet = me->GetPet())
            {
                if (pPet->IsAlive() &&
                    CanTryToCastSpell(pPet, m_spells.warlock.pDemonicSacrifice))
                {
                    if (DoCastSpell(pPet, m_spells.warlock.pDemonicSacrifice) == SPELL_CAST_OK)
                        return;
                }
            }
        }

        if (m_spells.warlock.pImmolate &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pImmolate))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pImmolate) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pConflagrate &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pConflagrate))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pConflagrate) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pCorruption &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pCorruption))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pCorruption) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pSiphonLife &&
           (me->GetHealthPercent() < 80.0f) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pSiphonLife))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pSiphonLife) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pDrainLife &&
           (me->GetHealthPercent() < 30.0f) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pDrainLife))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pDrainLife) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pFear &&
            pVictim->GetVictim() == me &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pFear))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pFear) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pCurseofAgony &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pCurseofAgony))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pCurseofAgony) == SPELL_CAST_OK)
                return;
        }

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
            && me->GetDistance(pVictim) > 30.0f)
        {
            BeginChasing(pVictim);
        }

        if (m_spells.warlock.pHowlofTerror &&
            GetAttackersInRangeCount(10.0f) > 1 &&
            CanTryToCastSpell(me, m_spells.warlock.pHowlofTerror))
        {
            if (DoCastSpell(me, m_spells.warlock.pHowlofTerror) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pShadowBolt &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pShadowBolt))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pShadowBolt) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pLifeTap &&
           (me->GetPowerPercent(POWER_MANA) < 10.0f) &&
           (me->GetHealthPercent() > 70.0f) &&
            CanTryToCastSpell(me, m_spells.warlock.pLifeTap))
        {
            if (DoCastSpell(me, m_spells.warlock.pLifeTap) == SPELL_CAST_OK)
                return;
        }

        if (me->HasSpell(PB_SPELL_SHOOT_WAND) &&
           !me->IsMoving() &&
           (me->GetPowerPercent(POWER_MANA) < 5.0f) &&
           !me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
            me->CastSpell(pVictim, PB_SPELL_SHOOT_WAND, false);
    }
}

void PartyBotAI::UpdateOutOfCombatAI_Warrior()
{
    if (m_spells.warrior.pBattleStance &&
        CanTryToCastSpell(me, m_spells.warrior.pBattleStance))
    {
        if (DoCastSpell(me, m_spells.warrior.pBattleStance) == SPELL_CAST_OK)
            return;
    }

    if (m_spells.warrior.pBattleShout &&
       !me->HasAura(m_spells.warrior.pBattleShout->Id))
    {
        if (CanTryToCastSpell(me, m_spells.warrior.pBattleShout))
            DoCastSpell(me, m_spells.warrior.pBattleShout);
        else if (m_spells.warrior.pBloodrage &&
            (me->GetPower(POWER_RAGE) < 100) &&
            CanTryToCastSpell(me, m_spells.warrior.pBloodrage))
        {
            DoCastSpell(me, m_spells.warrior.pBloodrage);
        }
    }

    if (Unit* pVictim = me->GetVictim())
    {
        if (m_spells.warrior.pCharge &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pCharge))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pCharge) == SPELL_CAST_OK)
                return;
        }
    }
}

void PartyBotAI::UpdateInCombatAI_Warrior()
{
    // Tank: Defensive Stance first in every fight (more threat, Revenge); out of combat the bot
    // returns to Battle Stance for Charge.
    if (m_role == ROLE_TANK && m_spells.warrior.pDefensiveStance &&
        !me->HasAura(m_spells.warrior.pDefensiveStance->Id) &&
        CanTryToCastSpell(me, m_spells.warrior.pDefensiveStance) &&
        DoCastSpell(me, m_spells.warrior.pDefensiveStance) == SPELL_CAST_OK)
        return;

    if (Unit* pVictim = me->GetVictim())
    {
        if (pVictim->IsNonMeleeSpellCasted(false, false, true))
        {
            if (m_spells.warrior.pPummel &&
                CanTryToCastSpell(pVictim, m_spells.warrior.pPummel))
            {
                if (DoCastSpell(pVictim, m_spells.warrior.pPummel) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.warrior.pShieldBash &&
                IsWearingShield(me) &&
                CanTryToCastSpell(pVictim, m_spells.warrior.pShieldBash))
            {
                if (DoCastSpell(pVictim, m_spells.warrior.pShieldBash) == SPELL_CAST_OK)
                    return;
            }
        }

        if (m_spells.warrior.pExecute &&
           (pVictim->GetHealthPercent() < 20.0f) &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pExecute))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pExecute) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pOverpower &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pOverpower))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pOverpower) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pLastStand &&
            me->GetHealthPercent() < 20.0f &&
            CanTryToCastSpell(me, m_spells.warrior.pLastStand))
        {
            if (DoCastSpell(me, m_spells.warrior.pLastStand) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pConcussionBlow &&
           (pVictim->IsNonMeleeSpellCasted() || pVictim->IsMoving() || (me->GetHealthPercent() < 50.0f)) &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pConcussionBlow))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pConcussionBlow) == SPELL_CAST_OK)
                return;
        }

        // Tank rage order: Revenge (cheap, most threat) > Sunder stacks > Shield Block only when
        // hurt or with spare rage, so Shield Block cannot starve Sunder of rage.
        // Revenge: Defensive Stance, after a block/dodge/parry (aura state checked by the cast rules).
        if (m_spells.warrior.pRevenge &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pRevenge))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pRevenge) == SPELL_CAST_OK)
            {
                if (m_role == ROLE_TANK)
                    sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Tank] %s: Revenge on %s (rage %u)",
                        me->GetName(), pVictim->GetName(), me->GetPower(POWER_RAGE) / 10);
                return;
            }
        }

        // Sunder Armor: stack to the maximum (5), then refresh shortly before it runs out.
        // While a Sunder is due and the tank is in reach, rage is saved for it (see below).
        bool saveRageForSunder = false;
        if (m_spells.warrior.pSunderArmor && m_role == ROLE_TANK)
        {
            uint32 stacks = 0;
            int32 remaining = 0;
            if (SpellAuraHolder const* pHolder = pVictim->GetSpellAuraHolder(m_spells.warrior.pSunderArmor->Id))
            {
                stacks = pHolder->GetStackAmount();
                remaining = pHolder->GetAuraDuration();
            }
            uint32 const maxStacks = std::max<uint32>(1, m_spells.warrior.pSunderArmor->GetStackAmount());

            bool const sunderDue = stacks < maxStacks || remaining < PB_SUNDER_REFRESH_MS;
            saveRageForSunder = sunderDue && me->CanReachWithMeleeAutoAttack(pVictim) &&
                me->GetPower(POWER_RAGE) < Spell::CalculatePowerCost(m_spells.warrior.pSunderArmor, me);

            if (sunderDue &&
                CanTryToCastSpell(pVictim, m_spells.warrior.pSunderArmor) &&
                DoCastSpell(pVictim, m_spells.warrior.pSunderArmor) == SPELL_CAST_OK)
            {
                if (stacks < maxStacks)
                    sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Tank] %s: Sunder Armor (%u) on %s, stack %u/%u (rage %u)",
                        me->GetName(), m_spells.warrior.pSunderArmor->Id, pVictim->GetName(), stacks + 1, maxStacks,
                        me->GetPower(POWER_RAGE) / 10);
                else
                    sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Tank] %s: Sunder Armor (%u) on %s, refresh %u/%u (%.1f s left, rage %u)",
                        me->GetName(), m_spells.warrior.pSunderArmor->Id, pVictim->GetName(), stacks, maxStacks, remaining / 1000.0f,
                        me->GetPower(POWER_RAGE) / 10);
                return;
            }
        }

        if (me->GetShapeshiftForm() == FORM_DEFENSIVESTANCE &&
            IsWearingShield(me))
        {
            if (!me->GetAttackers().empty())
            {
                if (m_spells.warrior.pShieldBlock &&
                    (m_role != ROLE_TANK || me->GetHealthPercent() < PB_SHIELD_BLOCK_HEALTH_PCT ||
                     me->GetPower(POWER_RAGE) >= PB_SHIELD_BLOCK_SPARE_RAGE) &&
                    CanTryToCastSpell(me, m_spells.warrior.pShieldBlock))
                {
                    if (DoCastSpell(me, m_spells.warrior.pShieldBlock) == SPELL_CAST_OK)
                    {
                        if (m_role == ROLE_TANK)
                            sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Tank] %s: Shield Block (health %.0f%%, rage %u)",
                                me->GetName(), me->GetHealthPercent(), me->GetPower(POWER_RAGE) / 10);
                        return;
                    }
                }

                if (m_spells.warrior.pShieldWall &&
                   (me->GetHealthPercent() < 40.0f) &&
                    CanTryToCastSpell(me, m_spells.warrior.pShieldWall))
                {
                    if (DoCastSpell(me, m_spells.warrior.pShieldWall) == SPELL_CAST_OK)
                        return;
                }
            }

            if (m_spells.warrior.pShieldSlam &&
                CanTryToCastSpell(pVictim, m_spells.warrior.pShieldSlam))
            {
                if (DoCastSpell(pVictim, m_spells.warrior.pShieldSlam) == SPELL_CAST_OK)
                    return;
            }
        }

        // Not enough rage for a due Sunder: spend nothing else (Revenge, Shield Block when hurt and
        // taunts above still go out), auto-attack builds the rage.
        if (saveRageForSunder)
            return;

        if (m_spells.warrior.pThunderClap &&
            m_role == ROLE_TANK &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pThunderClap))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pThunderClap) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pHamstring &&
            pVictim->IsMoving() &&
           !pVictim->HasUnitState(UNIT_STATE_ROOT) &&
           !pVictim->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED) &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pHamstring))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pHamstring) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pRend &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pRend))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pRend) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pIntimidatingShout &&
           (me->GetHealthPercent() < 30.0f) &&
           (GetAttackersInRangeCount(10.0f) > 2) &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pIntimidatingShout))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pIntimidatingShout) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pRetaliation &&
           (GetAttackersInRangeCount(10.0f) > 2) &&
            CanTryToCastSpell(me, m_spells.warrior.pRetaliation))
        {
            if (DoCastSpell(me, m_spells.warrior.pRetaliation) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pSweepingStrikes &&
            CanTryToCastSpell(me, m_spells.warrior.pSweepingStrikes) &&
           (me->GetEnemyCountInRadiusAround(pVictim, 10.0f) > 2))
        {
            if (DoCastSpell(me, m_spells.warrior.pSweepingStrikes) == SPELL_CAST_OK)
                return;
        }

        if (m_role != ROLE_TANK &&
           (me->GetHealthPercent() > 60.0f) && (pVictim->GetHealthPercent() > 40.0f) &&
           !me->HasUnitState(UNIT_STATE_ROOT) &&
           !me->IsImmuneToMechanic(MECHANIC_FEAR))
        {
            if (m_spells.warrior.pRecklessness &&
                CanTryToCastSpell(me, m_spells.warrior.pRecklessness))
            {
                if (DoCastSpell(me, m_spells.warrior.pRecklessness) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.warrior.pDeathWish &&
                CanTryToCastSpell(me, m_spells.warrior.pDeathWish))
            {
                if (DoCastSpell(me, m_spells.warrior.pDeathWish) == SPELL_CAST_OK)
                    return;
            }
        }

        if (m_spells.warrior.pMortalStrike &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pMortalStrike))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pMortalStrike) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pBloodthirst &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pBloodthirst))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pBloodthirst) == SPELL_CAST_OK)
                return;
        }

        if ((me->GetHealthPercent() < 20.0f) ||
            m_role == ROLE_TANK)
        {
            if (m_spells.warrior.pDefensiveStance &&
                CanTryToCastSpell(me, m_spells.warrior.pDefensiveStance))
            {
                DoCastSpell(me, m_spells.warrior.pDefensiveStance);
            }
        }
        else
        {
            if (m_spells.warrior.pBerserkerStance &&
                CanTryToCastSpell(me, m_spells.warrior.pBerserkerStance))
            {
                DoCastSpell(me, m_spells.warrior.pBerserkerStance);
            }
        }

        if (m_spells.warrior.pIntercept &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pIntercept))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pIntercept) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pWhirlwind &&
            CanTryToCastSpell(me, m_spells.warrior.pWhirlwind))
        {
            if (DoCastSpell(me, m_spells.warrior.pWhirlwind) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pDisarm &&
            IsMeleeWeaponClass(pVictim->GetClass()) &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pDisarm))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pDisarm) == SPELL_CAST_OK)
                return;
        }

        // Self-cast area debuff: the "aura already on target" rule checks the caster, so check the enemy.
        if (m_spells.warrior.pDemoralizingShout &&
            m_role == ROLE_TANK &&
           !pVictim->HasAura(m_spells.warrior.pDemoralizingShout->Id) &&
            CanTryToCastSpell(me, m_spells.warrior.pDemoralizingShout))
        {
            if (DoCastSpell(me, m_spells.warrior.pDemoralizingShout) == SPELL_CAST_OK)
                return;
        }

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
            && !me->CanReachWithMeleeAutoAttack(pVictim))
        {
            BeginChasing(pVictim);
        }

        if (me->GetPower(POWER_RAGE) > 300)
        {
            if (m_spells.warrior.pCleave && me->GetEnemyCountInRadiusAround(pVictim, 8.0f) > 1)
            {
                if (CanTryToCastSpell(pVictim, m_spells.warrior.pCleave))
                {
                    if (DoCastSpell(pVictim, m_spells.warrior.pCleave) == SPELL_CAST_OK)
                        return;
                }
            }
            else
            {
                if (m_spells.warrior.pHeroicStrike &&
                    CanTryToCastSpell(pVictim, m_spells.warrior.pHeroicStrike))
                {
                    if (DoCastSpell(pVictim, m_spells.warrior.pHeroicStrike) == SPELL_CAST_OK)
                        return;
                }
            }
        }
    }
    else // no victim
    {
        if (m_spells.warrior.pBattleShout &&
            CanTryToCastSpell(me, m_spells.warrior.pBattleShout))
        {
            if (DoCastSpell(me, m_spells.warrior.pBattleShout) == SPELL_CAST_OK)
                return;
        }
    }
}

bool PartyBotAI::ShouldEnterStealth() const
{
    if (me->IsMounted())
        return false;

    if (me->GetVictim() || me->InBattleGround() || me->IsFFAPvP())
        return true;

    if (me->GetHealthPercent() < 10.0f)
        return true;

    if (Player* pLeader = GetPartyLeader())
    {
        if (pLeader->IsDead() || pLeader->IsFeigningDeathSuccessfully() ||
            pLeader->HasAuraType(SPELL_AURA_MOD_STEALTH) ||
            pLeader->HasAuraType(SPELL_AURA_MOD_INVISIBILITY))
            return true;
    }

    return false;
}

bool PartyBotAI::EnterStealthIfNeeded(SpellEntry const* pStealthSpell)
{
    if (pStealthSpell)
    {
        bool const shouldStealth = ShouldEnterStealth();

        if (me->HasAura(pStealthSpell->Id))
        {
            if (!shouldStealth)
                me->RemoveAurasDueToSpellByCancel(pStealthSpell->Id);
        }
        else
        {
            if (shouldStealth &&
                CanTryToCastSpell(me, pStealthSpell) &&
                DoCastSpell(me, pStealthSpell) == SPELL_CAST_OK)
                return true;
        }
    }

    return false;
}

void PartyBotAI::UpdateOutOfCombatAI_Rogue()
{
    if (m_spells.rogue.pMainHandPoison &&
        CanTryToCastSpell(me, m_spells.rogue.pMainHandPoison))
    {
        if (CastWeaponBuff(m_spells.rogue.pMainHandPoison, EQUIPMENT_SLOT_MAINHAND) == SPELL_CAST_OK)
            return;
    }

    if (m_spells.rogue.pOffHandPoison &&
        CanTryToCastSpell(me, m_spells.rogue.pOffHandPoison))
    {
        if (CastWeaponBuff(m_spells.rogue.pOffHandPoison, EQUIPMENT_SLOT_OFFHAND) == SPELL_CAST_OK)
            return;
    }

    if (EnterStealthIfNeeded(m_spells.rogue.pStealth))
        return;

    if (me->GetVictim())
        UpdateInCombatAI_Rogue();
}

void PartyBotAI::UpdateInCombatAI_Rogue()
{
    if (Unit* pVictim = me->GetVictim())
    {
        if (me->HasAuraType(SPELL_AURA_MOD_STEALTH))
        {
            if (m_spells.rogue.pPremeditation &&
                CanTryToCastSpell(pVictim, m_spells.rogue.pPremeditation))
            {
                DoCastSpell(pVictim, m_spells.rogue.pPremeditation);
            }

            if (pVictim->IsCaster())
            {
                if (m_spells.rogue.pGarrote &&
                    CanTryToCastSpell(pVictim, m_spells.rogue.pGarrote))
                {
                    if (DoCastSpell(pVictim, m_spells.rogue.pGarrote) == SPELL_CAST_OK)
                        return;
                }
            }
            else
            {
                if (m_spells.rogue.pAmbush &&
                    CanTryToCastSpell(pVictim, m_spells.rogue.pAmbush))
                {
                    if (DoCastSpell(pVictim, m_spells.rogue.pAmbush) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.rogue.pCheapShot &&
                    CanTryToCastSpell(pVictim, m_spells.rogue.pCheapShot))
                {
                    if (DoCastSpell(pVictim, m_spells.rogue.pCheapShot) == SPELL_CAST_OK)
                        return;
                }
            }
        }
        else
        {
            if (m_spells.rogue.pVanish &&
                (me->GetHealthPercent() < 10.0f))
            {
                if (m_spells.rogue.pPreparation &&
                    !me->IsSpellReady(m_spells.rogue.pVanish) &&
                    CanTryToCastSpell(me, m_spells.rogue.pPreparation))
                {
                    if (DoCastSpell(me, m_spells.rogue.pPreparation) == SPELL_CAST_OK)
                        return;
                }

                if (CanTryToCastSpell(me, m_spells.rogue.pVanish))
                {
                    if (DoCastSpell(me, m_spells.rogue.pVanish) == SPELL_CAST_OK)
                        return;
                }
            }
        }

        if (me->GetComboPoints() > 4)
        {
            std::vector<SpellEntry const*> vSpells;

            // Give priority to Slice and Dice over other finishing moves.
            if (m_spells.rogue.pSliceAndDice &&
               !me->HasAura(m_spells.rogue.pSliceAndDice->Id) &&
                pVictim->GetHealthPercent() > 10.0f)
                vSpells.push_back(m_spells.rogue.pSliceAndDice);
            else
            {
                if (m_spells.rogue.pEviscerate)
                    vSpells.push_back(m_spells.rogue.pEviscerate);
                if (m_spells.rogue.pKidneyShot && !pVictim->IsImmuneToMechanic(MECHANIC_STUN))
                    vSpells.push_back(m_spells.rogue.pKidneyShot);
                if (m_spells.rogue.pExposeArmor)
                    vSpells.push_back(m_spells.rogue.pExposeArmor);
                if (m_spells.rogue.pRupture)
                    vSpells.push_back(m_spells.rogue.pRupture);
            }

            if (!vSpells.empty())
            {
                SpellEntry const* pComboSpell = SelectRandomContainerElement(vSpells);
                if (CanTryToCastSpell(pVictim, pComboSpell))
                {
                    if (DoCastSpell(pVictim, pComboSpell) == SPELL_CAST_OK)
                        return;
                }
            }
        }

        if (m_spells.rogue.pBlind)
        {
            if (Unit* pTarget = SelectAttackerDifferentFrom(pVictim))
            {
                if (CanTryToCastSpell(pTarget, m_spells.rogue.pBlind) &&
                    CanUseCrowdControl(m_spells.rogue.pBlind, pTarget))
                {
                    if (DoCastSpell(pTarget, m_spells.rogue.pBlind) == SPELL_CAST_OK)
                    {
                        me->AttackStop();
                        AttackStart(pVictim);
                        return;
                    }
                }
            }
        }

        if (m_spells.rogue.pAdrenalineRush &&
           !me->GetPower(POWER_ENERGY) &&
            CanTryToCastSpell(me, m_spells.rogue.pAdrenalineRush))
        {
            if (DoCastSpell(me, m_spells.rogue.pAdrenalineRush) == SPELL_CAST_OK)
                return;
        }

        if (pVictim->IsNonMeleeSpellCasted())
        {
            if (m_spells.rogue.pGouge &&
                CanTryToCastSpell(pVictim, m_spells.rogue.pGouge))
            {
                if (DoCastSpell(pVictim, m_spells.rogue.pGouge) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.rogue.pKick &&
                CanTryToCastSpell(pVictim, m_spells.rogue.pKick))
            {
                if (DoCastSpell(pVictim, m_spells.rogue.pKick) == SPELL_CAST_OK)
                    return;
            }
        }

        if (!me->HasAuraType(SPELL_AURA_MOD_STEALTH))
        {
            if (m_spells.rogue.pEvasion &&
               (me->GetHealthPercent() < 80.0f) &&
               ((GetAttackersInRangeCount(10.0f) > 2) || !IsRangedDamageClass(pVictim->GetClass())) &&
                CanTryToCastSpell(me, m_spells.rogue.pEvasion))
            {
                if (DoCastSpell(me, m_spells.rogue.pEvasion) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.rogue.pColdBlood &&
                CanTryToCastSpell(me, m_spells.rogue.pColdBlood))
            {
                DoCastSpell(me, m_spells.rogue.pColdBlood);
            }

            if (m_spells.rogue.pBladeFlurry &&
                CanTryToCastSpell(me, m_spells.rogue.pBladeFlurry))
            {
                if (DoCastSpell(me, m_spells.rogue.pBladeFlurry) == SPELL_CAST_OK)
                    return;
            }
        }

        if (m_spells.rogue.pBackstab &&
            CanTryToCastSpell(pVictim, m_spells.rogue.pBackstab))
        {
            if (DoCastSpell(pVictim, m_spells.rogue.pBackstab) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.rogue.pGhostlyStrike &&
            CanTryToCastSpell(pVictim, m_spells.rogue.pGhostlyStrike))
        {
            if (DoCastSpell(pVictim, m_spells.rogue.pGhostlyStrike) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.rogue.pHemorrhage &&
            CanTryToCastSpell(pVictim, m_spells.rogue.pHemorrhage))
        {
            if (DoCastSpell(pVictim, m_spells.rogue.pHemorrhage) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.rogue.pSinisterStrike &&
            CanTryToCastSpell(pVictim, m_spells.rogue.pSinisterStrike))
        {
            if (DoCastSpell(pVictim, m_spells.rogue.pSinisterStrike) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.rogue.pSprint &&
           !me->HasUnitState(UNIT_STATE_ROOT) &&
           !me->CanReachWithMeleeAutoAttack(pVictim) &&
            CanTryToCastSpell(me, m_spells.rogue.pSprint))
        {
            if (DoCastSpell(me, m_spells.rogue.pSprint) == SPELL_CAST_OK)
                return;
        }
    }
}

bool PartyBotAI::EnterCombatDruidForm()
{
    if (m_spells.druid.pCatForm &&
        GetRole() == ROLE_MELEE_DPS &&
        CanTryToCastSpell(me, m_spells.druid.pCatForm))
    {
        if (DoCastSpell(me, m_spells.druid.pCatForm) == SPELL_CAST_OK)
            return true;
    }

    if (m_spells.druid.pBearForm &&
       (m_role == ROLE_TANK || GetRole() == ROLE_MELEE_DPS) &&
        CanTryToCastSpell(me, m_spells.druid.pBearForm))
    {
        if (DoCastSpell(me, m_spells.druid.pBearForm) == SPELL_CAST_OK)
            return true;
    }

    if (m_spells.druid.pMoonkinForm &&
        GetRole() == ROLE_RANGE_DPS &&
        CanTryToCastSpell(me, m_spells.druid.pMoonkinForm))
    {
        if (DoCastSpell(me, m_spells.druid.pMoonkinForm) == SPELL_CAST_OK)
            return true;
    }

    return false;
}

void PartyBotAI::UpdateOutOfCombatAI_Druid()
{
    // Make sure bot leaves combat form if his role is changed to healer.
    if (GetRole() == ROLE_HEALER && me->GetShapeshiftForm() != FORM_NONE &&
        me->HasAuraType(SPELL_AURA_MOD_SHAPESHIFT))
    {
        me->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);
        return;
    }

    // Feral druids (tank: bear, melee: cat) always keep enough mana to shift back after buffing.
    SpellEntry const* pFeralForm = m_role == ROLE_TANK ? m_spells.druid.pBearForm :
                                   (m_role == ROLE_MELEE_DPS ? m_spells.druid.pCatForm : nullptr);
    auto keepsBearFormMana = [this, pFeralForm](SpellEntry const* pSpell)
    {
        if (!pFeralForm)
            return true;
        return me->GetPower(POWER_MANA) >= Spell::CalculatePowerCost(pSpell, me) +
               Spell::CalculatePowerCost(pFeralForm, me);
    };

    // Out of combat and no target: a feral druid leaves its form when someone needs Mark of the
    // Wild or Thorns and the mana allows it; it buffs below and shifts back afterwards.
    if (pFeralForm && !me->GetVictim() && me->GetShapeshiftForm() != FORM_NONE &&
        me->HasAuraType(SPELL_AURA_MOD_SHAPESHIFT))
    {
        SpellEntry const* pNeededBuff = nullptr;
        bool needsBuff = SelectBuffTarget(m_spells.druid.pMarkoftheWild, m_spells.druid.pGiftoftheWild, pNeededBuff) &&
                         pNeededBuff && keepsBearFormMana(pNeededBuff);
        if (!needsBuff && m_spells.druid.pThorns)
            needsBuff = SelectBuffTarget(m_spells.druid.pThorns) && keepsBearFormMana(m_spells.druid.pThorns);
        if (needsBuff)
        {
            me->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);
            return;
        }
    }

    SpellEntry const* pBuffSpell = nullptr;
    if (Player* pTarget = SelectBuffTarget(m_spells.druid.pMarkoftheWild, m_spells.druid.pGiftoftheWild, pBuffSpell))
    {
        if (keepsBearFormMana(pBuffSpell) && CanTryToCastSpell(pTarget, pBuffSpell))
        {
            if (DoCastSpell(pTarget, pBuffSpell) == SPELL_CAST_OK)
            {
                m_isBuffing = true;
                me->ClearTarget();
                return;
            }
        }
    }

    if (m_spells.druid.pThorns)
    {
        if (Player* pTarget = SelectBuffTarget(m_spells.druid.pThorns))
        {
            if (keepsBearFormMana(m_spells.druid.pThorns) && CanTryToCastSpell(pTarget, m_spells.druid.pThorns))
            {
                if (DoCastSpell(pTarget, m_spells.druid.pThorns) == SPELL_CAST_OK)
                {
                    m_isBuffing = true;
                    me->ClearTarget();
                    return;
                }
            }
        }
    }

    if (m_spells.druid.pNaturesGrasp &&
        CanTryToCastSpell(me, m_spells.druid.pNaturesGrasp))
    {
        if (DoCastSpell(me, m_spells.druid.pNaturesGrasp) == SPELL_CAST_OK)
            return;
    }

    if (m_isBuffing &&
       (!m_spells.druid.pMarkoftheWild ||
        !me->HasGCD(m_spells.druid.pMarkoftheWild)))
    {
        m_isBuffing = false;
    }

    if (me->GetShapeshiftForm() == FORM_NONE)
    {
        if (EnterCombatDruidForm())
            return;

        if ((me->GetPowerPercent(POWER_MANA) > 80.0f) &&
            FindAndHealInjuredAlly())
            return;
    }
    else if (me->GetShapeshiftForm() == FORM_CAT)
    {
        if (EnterStealthIfNeeded(m_spells.druid.pProwl))
            return;
    }

    if (me->GetVictim())
        UpdateInCombatAI_Druid();
}

void PartyBotAI::UpdateInCombatAI_Druid()
{
    ShapeshiftForm const form = me->GetShapeshiftForm();

    if (m_spells.druid.pBarkskin &&
        (form == FORM_NONE || form == FORM_MOONKIN) &&
        (me->GetHealthPercent() < 50.0f) &&
        CanTryToCastSpell(me, m_spells.druid.pBarkskin))
    {
        if (DoCastSpell(me, m_spells.druid.pBarkskin) == SPELL_CAST_OK)
            return;
    }

    // Tanks shift to bear form before anything else (no healing/HoTs in caster form first).
    if (form == FORM_NONE && m_role == ROLE_TANK && EnterCombatDruidForm())
        return;

    if (form == FORM_NONE)
    {
        if (m_spells.druid.pHibernate &&
            m_role != ROLE_TANK &&
            !me->GetAttackers().empty())
        {
            Unit* pAttacker = *me->GetAttackers().begin();
            if (CanTryToCastSpell(pAttacker, m_spells.druid.pHibernate) &&
                CanUseCrowdControl(m_spells.druid.pHibernate, pAttacker))
            {
                if (DoCastSpell(pAttacker, m_spells.druid.pHibernate) == SPELL_CAST_OK)
                    return;
            }
        }

        // Prioritize applying HoTs.
        if (Unit* pTarget = SelectPeriodicHealTarget(80.0f, 90.0f))
            if (HealInjuredTargetPeriodic(pTarget))
                return;

        // Direct heal.
        if (Unit* pTarget = SelectHealTarget(60.0f, 70.0f))
            if (HealInjuredTargetDirect(pTarget))
                return;

        if (m_spells.druid.pInnervate &&
           (me->GetHealthPercent() > 40.0f) &&
           (me->GetPowerPercent(POWER_MANA) < 10.0f) &&
            CanTryToCastSpell(me, m_spells.druid.pInnervate))
        {
            if (DoCastSpell(me, m_spells.druid.pInnervate) == SPELL_CAST_OK)
                return;
        }

        if (GetRole() == ROLE_HEALER && FindAndPreHealTarget())
            return;

        if (EnterCombatDruidForm())
            return;
    }

    Unit* pVictim = me->GetVictim();
    if (!pVictim)
        return;

    if (form != FORM_NONE &&
        me->HasUnitState(UNIT_STATE_ROOT) &&
        me->HasAuraType(SPELL_AURA_MOD_SHAPESHIFT) &&
        (m_role != ROLE_TANK || !me->CanReachWithMeleeAutoAttack(pVictim)))
        me->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);

    if (GetRole() == ROLE_HEALER)
        return;

    switch (form)
    {
        case FORM_CAT:
        {
            if (me->HasDistanceCasterMovement())
                me->SetCasterChaseDistance(0.0f);

            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
                && !me->CanReachWithMeleeAutoAttack(pVictim))
            {
                BeginChasing(pVictim);
            }

            if (me->HasAuraType(SPELL_AURA_MOD_STEALTH))
            {
                if (m_spells.druid.pPounce &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pPounce))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pPounce) == SPELL_CAST_OK)
                        return;
                }
                if (m_spells.druid.pRavage &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pRavage))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pRavage) == SPELL_CAST_OK)
                        return;
                }
                if (m_spells.druid.pTigersFury &&
                    CanTryToCastSpell(me, m_spells.druid.pTigersFury))
                {
                    if (DoCastSpell(me, m_spells.druid.pTigersFury) == SPELL_CAST_OK)
                        return;
                }
                return;
            }

            if (m_spells.druid.pCower &&
                GetAttackersInRangeCount(8.0f))
            {
                Unit* pAttacker = *me->GetAttackers().begin();
                if (CanTryToCastSpell(me, m_spells.druid.pCower))
                {
                    if (DoCastSpell(me, m_spells.druid.pCower) == SPELL_CAST_OK)
                        return;
                }
            }

            if (me->GetComboPoints() > 4)
            {
                if (m_spells.druid.pFerociousBite &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pFerociousBite))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pFerociousBite) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.druid.pRip &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pRip))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pRip) == SPELL_CAST_OK)
                        return;
                }
            }

            if (!me->CanReachWithMeleeAutoAttack(pVictim))
            {
                if (m_spells.druid.pFaerieFireFeral &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pFaerieFireFeral))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pFaerieFireFeral) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.druid.pDash &&
                    pVictim->IsMoving() &&
                    CanTryToCastSpell(me, m_spells.druid.pDash))
                {
                    if (DoCastSpell(me, m_spells.druid.pDash) == SPELL_CAST_OK)
                        return;
                }
            }

            if (m_spells.druid.pShred &&
                CanTryToCastSpell(pVictim, m_spells.druid.pShred))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pShred) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.druid.pRake &&
                CanTryToCastSpell(pVictim, m_spells.druid.pRake))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pRake) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.druid.pClaw &&
                CanTryToCastSpell(pVictim, m_spells.druid.pClaw))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pClaw) == SPELL_CAST_OK)
                    return;
            }

            break;
        }
        case FORM_BEAR:
        case FORM_DIREBEAR:
        {
            if (me->HasDistanceCasterMovement())
                me->SetCasterChaseDistance(0.0f);

            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
                && !me->CanReachWithMeleeAutoAttack(pVictim))
            {
                BeginChasing(pVictim);
            }

            if (m_spells.druid.pFeralCharge &&
                CanTryToCastSpell(pVictim, m_spells.druid.pFeralCharge))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pFeralCharge) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.druid.pBash &&
                CanTryToCastSpell(pVictim, m_spells.druid.pBash))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pBash) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.druid.pFrenziedRegeneration &&
                (me->GetHealthPercent() < 30.0f) &&
                CanTryToCastSpell(me, m_spells.druid.pFrenziedRegeneration))
            {
                if (DoCastSpell(me, m_spells.druid.pFrenziedRegeneration) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.druid.pFaerieFireFeral &&
                CanTryToCastSpell(pVictim, m_spells.druid.pFaerieFireFeral))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pFaerieFireFeral) == SPELL_CAST_OK)
                    return;
            }

            if ((me->GetPower(POWER_RAGE) > 800) ||
                (GetAttackersInRangeCount(10.0f) > 1))
            {
                if (m_spells.druid.pDemoralizingRoar &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pDemoralizingRoar))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pDemoralizingRoar) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.druid.pSwipe &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pSwipe))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pSwipe) == SPELL_CAST_OK)
                        return;
                }
            }

            if (m_spells.druid.pMaul &&
                CanTryToCastSpell(pVictim, m_spells.druid.pMaul))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pMaul) == SPELL_CAST_OK)
                    return;
            }
            break;
        }
        case FORM_NONE:
        case FORM_MOONKIN:
        {
            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE &&
                me->GetDistance(pVictim) > 30.0f)
            {
                BeginChasing(pVictim);
            }
            else if (pVictim->CanReachWithMeleeAutoAttack(me) &&
                    (pVictim->GetVictim() == me))
            {
                // Rooted, not run from (party bots never run away; the tank comes for it).
                if (m_spells.druid.pEntanglingRoots &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pEntanglingRoots))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pEntanglingRoots) == SPELL_CAST_OK)
                        return;
                }
            }

            if (m_spells.druid.pFaerieFire &&
               (pVictim->GetClass() == CLASS_ROGUE) &&
                CanTryToCastSpell(pVictim, m_spells.druid.pFaerieFire))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pFaerieFire) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.druid.pInsectSwarm &&
                CanTryToCastSpell(pVictim, m_spells.druid.pInsectSwarm))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pInsectSwarm) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.druid.pHurricane &&
               (me->GetEnemyCountInRadiusAround(pVictim, 10.0f) > 2) &&
                CanTryToCastSpell(pVictim, m_spells.druid.pHurricane))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pHurricane) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.druid.pMoonfire &&
                CanTryToCastSpell(pVictim, m_spells.druid.pMoonfire))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pMoonfire) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.druid.pStarfire &&
               (pVictim->GetHealthPercent() > 50.0f) &&
                CanTryToCastSpell(pVictim, m_spells.druid.pStarfire))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pStarfire) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.druid.pWrath &&
                CanTryToCastSpell(pVictim, m_spells.druid.pWrath))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pWrath) == SPELL_CAST_OK)
                    return;
            }

            break;
        }
    }
}
