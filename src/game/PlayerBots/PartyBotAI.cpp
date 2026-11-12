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
#include "Bag.h"
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
#include "PathFinder.h"

#include <cstdio>
#include <fstream>
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

// Task 16c supplies (user 2026-11-11): item of the highest rank whose minimum (level, or First Aid
// skill for bandages) the bot has. Ids checked against the DB.
struct PartyBotSupplyRank { uint32 minValue, itemId; };
static PartyBotSupplyRank const PB_HEALING_POTIONS[] = // Minor .. Major Healing Potion, by level
    { { 1, 118 }, { 3, 858 }, { 12, 929 }, { 21, 1710 }, { 35, 3928 }, { 45, 13446 } };
static PartyBotSupplyRank const PB_MANA_POTIONS[] =    // Minor .. Major Mana Potion, by level
    { { 5, 2455 }, { 14, 3385 }, { 22, 3827 }, { 31, 6149 }, { 41, 13443 }, { 49, 13444 } };
static PartyBotSupplyRank const PB_BANDAGES[] =        // Linen .. Heavy Runecloth Bandage, by skill
    { { 1, 1251 }, { 20, 2581 }, { 50, 3530 }, { 75, 3531 }, { 100, 6450 }, { 125, 6451 },
      { 150, 8544 }, { 175, 8545 }, { 200, 14529 }, { 225, 14530 } };

template <size_t N>
static uint32 GetBestSupplyRank(PartyBotSupplyRank const (&ranks)[N], uint32 value)
{
    uint32 itemId = 0;
    for (auto const& rank : ranks)
        if (value >= rank.minValue)
            itemId = rank.itemId;
    return itemId;
}

// Apprentice, Journeyman, Expert, Artisan First Aid (max skill 75, 150, 225, 300).
static uint32 const PB_FIRST_AID_SPELLS[] = { 3273, 3274, 7924, 10846 };
#define PB_FIRST_AID_RANK_SKILL 75
#define PB_SPELL_RECENTLY_BANDAGED 11196
#define PB_ITEM_BOTTOMLESS_BAG 14156          // 18 slots (no 32-slot bag in 1.12)
#define PB_POTION_HEALTH_PCT 35.0f            // warrior/rogue/tank: healing potion when attacked below this
#define PB_MANA_POTION_PCT 50.0f              // mana users: mana potion at or below this
#define PB_BANDAGE_HEALTH_PCT 50.0f          // tank, melee, ranged: bandage below this
#define PB_HEALER_BANDAGE_HEALTH_PCT 60.0f    // healer without mana for its cheapest heal: bandage below this

// Best mana restore of an energize spell (mana gem, mana potion), 0 if none.
static int32 GetMaxManaRestore(SpellEntry const* pSpellEntry)
{
    for (uint32 i = 0; i < MAX_EFFECT_INDEX; ++i)
        if (pSpellEntry->Effect[i] == SPELL_EFFECT_ENERGIZE && pSpellEntry->EffectMiscValue[i] == POWER_MANA)
            return pSpellEntry->EffectBasePoints[i] + pSpellEntry->EffectDieSides[i];
    return 0;
}

// The bot's item with this entry in the backpack or a bag.
static Item* FindInventoryItem(Player const* pPlayer, uint32 itemId)
{
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (Item* pItem = pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            if (pItem->GetEntry() == itemId)
                return pItem;
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        if (Bag* pBag = dynamic_cast<Bag*>(pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, bag)))
            for (uint8 slot = 0; slot < pBag->GetBagSize(); ++slot)
                if (Item* pItem = pBag->GetItemByPos(slot))
                    if (pItem->GetEntry() == itemId)
                        return pItem;
    return nullptr;
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
#define PB_FOLLOW_ORDER_NO_SIT_MS 10000
#define PB_HOLD_NO_PROGRESS_TIMEOUT (5 * IN_MILLISECONDS)
#define PB_URGENT_HEAL_PCT 50.0f   // healers heal before crowd control below this (19d)
#define PB_STEP_BACK_TRIGGER 8.0f         // B7: hunter dead zone (no Auto Shot closer than this)
#define PB_STEP_BACK_DISTANCE 15.0f       // B7: spot this far from the target (~12 yd walk)
#define PB_STEP_BACK_INTERVAL 3000        // B7: at most one try per 3 s
#define PB_STEP_BACK_MAX_TIME 5000        // B7: a step back that takes longer is over
#define PB_STEP_BACK_CC_CLEARANCE 10.0f   // B7: spot this far from CC'd / CC-marked enemies
#define PB_STEP_BACK_AGGRO_CLEARANCE 20.0f // B7: spot this far from enemies not fighting yet
#define PB_EAT_DRINK_START_PCT 70.0f // out of combat, sit down below this health/mana %
#define PB_AUTO_AOE_RADIUS 10.0f     // pack size = enemies within this of the target (incl. the target)
#define PB_AOE_CC_MARK_RANGE 40.0f   // no auto AoE while a CC-marked enemy is this close
#define PB_TANK_SAFE_LEAD 1.5f       // tank threat >= this x anyone else's = enemy is safe
#define PB_TANK_DANGER_LEAD 1.15f    // tank threat below this x someone else's = about to switch
#define PB_TANK_THREAT_FLOOR 10.0f   // per tank level: threat counted as "anyone else" at minimum
#define PB_SUNDER_REFRESH_MS 5000    // re-apply a full Sunder Armor stack with this much time left
#define PB_SHIELD_BLOCK_HEALTH_PCT 60.0f // tank Shield Block when below this health...
#define PB_SHIELD_BLOCK_SPARE_RAGE 400   // ...or with this much rage (x10) to spare
#define PB_BLOODRAGE_MAX_RAGE 200        // B13: tank Bloodrage in a fight below this rage (x10)...
#define PB_BLOODRAGE_MIN_HEALTH_PCT 50.0f // ...and above this health % (it costs health)
#define PB_KITE_POINT_SPACING 4.0f    // F3: recorded kite path: a point every 4 yd
#define PB_KITE_POINT_REACHED 2.5f    // F3: kiter is at a path point within this
#define PB_KITE_CLAIM_RANGE 50.0f     // F3: kiter takes bosses within this of the path start
#define PB_KITE_OPEN_TIMEOUT 10000    // F3: kiter pull: the boss must be fighting within 10 s
#define PB_KITE_RUN_START 20.0f       // F3: run out once the boss on the kiter is this close
#define PB_KITE_END_WAIT 3000         // F3: at the path end, Feign Death after this long
#define PB_KITE_FEIGN_HEALTH_PCT 50.0f // F3: caught by the boss on the way out below this health: Feign Death
#define PB_KITE_FEIGN_MIN_TIME 2000   // F3: lie still at least this long
#define PB_KITE_BACK_CLEARANCE 30.0f  // F3: get up once the boss has turned away and is this far
#define PB_KITE_BACK_GAP 30.0f        // F3: running back, stay this far behind a walking boss (aggro range 20)

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

    // B7 hunter stepping back: only instant spells (a cast would stop the walk).
    if ((pSpellEntry->IsChanneledSpell() || pSpellEntry->GetCastTime(me) > 0) && IsSteppingBack())
        return false;

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
    else if (pTarget && !pSpellEntry->IsPositiveSpell() && !IsInDuel() && !IsChainSpellSafe(pTarget, pSpellEntry))
        return false;

    return true;
}

// Chain spells are not area spells in the spell data, but jump to enemies next to the target
// (ranged: up to 10 yd per jump, as the core searches; melee such as Cleave: around the caster).
// Same rules as area spells: never break crowd control (CC marks: not for tanks), never pull
// enemies that are not fighting yet (weak ones excepted).
bool PartyBotAI::IsChainSpellSafe(Unit const* pTarget, SpellEntry const* pSpellEntry) const
{
    uint32 jumps = 0;
    for (uint32 i = 0; i < MAX_EFFECT_INDEX; ++i)
        if (pSpellEntry->Effect[i] && pSpellEntry->EffectChainTarget[i] > 1)
            jumps = std::max(jumps, pSpellEntry->EffectChainTarget[i] - 1);
    if (!jumps)
        return true;

    bool const melee = pSpellEntry->DmgClass == SPELL_DAMAGE_CLASS_MELEE;
    std::list<Unit*> targets;
    me->GetEnemyListInRadiusAround(melee ? me : pTarget, melee ? ATTACK_DISTANCE + 2.0f : jumps * CHAIN_SPELL_JUMP_RADIUS,
        targets);
    for (auto const& pEnemy : targets)
    {
        if (pEnemy == pTarget)
            continue;
        if (pEnemy->HasBreakableByDamageCrowdControlAura() || (m_role != ROLE_TANK && IsForbiddenTarget(pEnemy)))
            return false;
        if (!pEnemy->IsInCombat() && (pEnemy->GetLevel() + 5) > me->GetLevel() && pEnemy->IsValidAttackTarget(me))
            return false;
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
            me->GetName(), what, GetLogName(pTarget).c_str(), distance, inSight ? "" : ", no line of sight");
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
// range or line of sight it first walks there, except on stay (it says so once and stays). Never
// the target of its own attack order. Returns true while it casts or walks for it.
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
        if (!pTarget || !IsCCMarkPending(pTarget))
            continue;

        SpellEntry const* pSpellEntry = GetCrowdControlSpellFor(pTarget);
        if (!pSpellEntry)
            continue;

        // Never start a fight for a CC mark: an enemy that is not fighting yet is never crowd-
        // controlled or walked to, also not during a pull timer (B10: a resisted CC before the
        // tank's pull lands gives the CC bot first aggro, and a boss next to the mark with it).
        if (!pTarget->IsInCombat())
        {
            if (m_ccIdleLoggedGuid != pTarget->GetObjectGuid())
            {
                sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[CC] %s: %s waits for %s (not fighting yet, %s)",
                    me->GetName(), pSpellEntry->SpellName[0].c_str(), GetLogName(pTarget).c_str(),
                    IsPullCCWindow() ? "pull not landed" : "no pull order");
                m_ccIdleLoggedGuid = pTarget->GetObjectGuid();
            }
            continue;
        }

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

        // On stay (stay, comestay, move here) the bot never walks off its spot for a CC mark.
        if (m_holdPosition && !IsInCrowdControlReach(pTarget))
        {
            if (m_ccOutOfReachGuid != pTarget->GetObjectGuid())
            {
                sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[CC] %s: on stay, %s out of %s range or sight (%.0f yd), stays",
                    me->GetName(), GetLogName(pTarget).c_str(), pSpellEntry->SpellName[0].c_str(), me->GetDistance(pTarget));
                SayInParty(std::string("Can't reach ") + pTarget->GetName() + " for " + pSpellEntry->SpellName[0] +
                    " from my spot.");
                m_ccOutOfReachGuid = pTarget->GetObjectGuid();
            }
            continue;
        }

        float const range = Spells::GetSpellMaxRange(sSpellRangeStore.LookupEntry(pSpellEntry->rangeIndex));
        if (WalkIntoRange(pTarget, range, "CC", pSpellEntry->SpellName[0].c_str()))
            return true;

        if (!CanTryToCastSpell(pTarget, pSpellEntry))
            continue;

        m_ccCastForMark = true;                  // OnCastStarted: source "mark"
        SpellCastResult const result = DoCastSpell(pTarget, pSpellEntry);
        m_ccCastForMark = false;
        if (result == SPELL_CAST_OK)
        {
            me->ClearUnitState(UNIT_STATE_MELEE_ATTACKING);
            m_ccFailGuid.Clear();
            return true;
        }
        if (m_ccFailGuid != pTarget->GetObjectGuid() || m_ccFailResult != uint32(result))
        {
            sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[CC] %s: %s on %s fails (cast result %u)", me->GetName(),
                pSpellEntry->SpellName[0].c_str(), GetLogName(pTarget).c_str(), uint32(result));
            m_ccFailGuid = pTarget->GetObjectGuid();
            m_ccFailResult = uint32(result);
        }
        if (result == SPELL_FAILED_BAD_TARGETS || result == SPELL_FAILED_IMMUNE || result == SPELL_FAILED_HIGHLEVEL)
            ReleaseCCMark(pTarget, pSpellEntry, result == SPELL_FAILED_HIGHLEVEL ? "level too high" : "not a valid target");
    }
    return false;
}

bool PartyBotAI::IsCCMarkPending(Unit const* pTarget) const
{
    return pTarget->GetObjectGuid() != m_orderedTargetGuid &&
          !m_releasedCC.count(pTarget->GetObjectGuid()) &&
          !pTarget->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL) &&
           IsValidHostileTarget(pTarget) && !AreOthersOnSameTarget(pTarget->GetObjectGuid());
}

bool PartyBotAI::HasPendingCCMark() const
{
    if (!HasCrowdControlSpell())
        return false;

    for (auto mark : m_marksToCC)
        if (Unit const* pTarget = GetMarkedTarget(mark))
            if (IsCCMarkPending(pTarget) && (!m_holdPosition || IsInCrowdControlReach(pTarget)))
                return true;
    return false;
}

bool PartyBotAI::IsInCrowdControlReach(Unit const* pTarget) const
{
    SpellEntry const* pSpellEntry = GetCrowdControlSpellFor(pTarget);
    if (!pSpellEntry)
        return false;
    float const range = Spells::GetSpellMaxRange(sSpellRangeStore.LookupEntry(pSpellEntry->rangeIndex));
    return me->IsWithinDistInMap(pTarget, range - 2.0f) && me->IsWithinLOSInMap(pTarget);
}

void PartyBotAI::ReleaseCCMark(Unit* pTarget, SpellEntry const* pSpellEntry, char const* why)
{
    if (!m_releasedCC.insert(pTarget->GetObjectGuid()).second)
        return;

    sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Mark] %s: cannot %s %s (%s), mark released, attacked normally",
        me->GetName(), pSpellEntry->SpellName[0].c_str(), GetLogName(pTarget).c_str(), why);
    SayInParty(std::string("Can't ") + pSpellEntry->SpellName[0] + " " + pTarget->GetName() + " (" + why +
        ") - attacking it instead.");
}

// B9: an attack order on one of this bot's CC marks releases the mark for this fight (also for
// healers, who get no attack order), so the bot does not crowd-control it again.
void PartyBotAI::ReleaseCCMarkForAttackOrder(Unit* pTarget)
{
    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return;
    for (auto mark : m_marksToCC)
        if (pGroup->GetTargetWithIcon(mark) == pTarget->GetObjectGuid() &&
            m_releasedCC.insert(pTarget->GetObjectGuid()).second)
        {
            sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Mark] %s: CC mark on %s released (attack order)", me->GetName(),
                GetLogName(pTarget).c_str());
            return;
        }
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

        // Every tick, so a cancelled CC cast is timed to ~one world tick.
        UpdateCCCastOutcome();

        UpdateDiagnostics();

        UpdateKiteMovement();

        // Entering combat (attacked, or the group pulled): think on the next tick, not after
        // the rest of the update interval.
        bool const inCombat = me->IsInCombat();
        if (inCombat && !m_wasInCombat)
            WakeUp();
        else if (!inCombat && m_wasInCombat)
        {
            m_releasedCC.clear(); // fight over: released CC marks count again next pull
            m_supplyBagFullLogged = false;
        }
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
        FillEmptyBagSlots();
        AddRangedAmmo();
        UpdateFirstAid();
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

    // setrole respec (added bots), once out of combat.
    if (m_pendingRespec && !me->IsInCombat())
        RespecForRole();

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
        m_wasDead = true;
        if (m_bandageSpellId)
            EndBandage();

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

    if (m_wasDead)
    {
        m_wasDead = false;
        RepairGear("revived");
    }

    // Survival layer (16c): potions, mana gem, bandages; also ends/stops a running bandage.
    if (UpdateSurvival())
        return;

    // F3: a kiter with a boss runs its path (beats casts in progress, orders and stay).
    if (UpdateKite())
        return;

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
        // Being hit does not end the wait (the tank grabs loose enemies; survival runs above).
        if (WorldTimer::getMSTimeDiffToNow(m_pullWaitStart) >= m_pullWaitDuration)
            EndPullWait("pull timer over");
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
    m_groupFighting = groupFighting;

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
        // (stacked: on the leader), while the leader stands still, and not within 10 s of a
        // follow order (the leader is about to move).
        bool const mayEatDrink = !groupFighting && (!mayFollow ||
            (!pLeader->IsMoving() &&
             (!m_followOrderTime || WorldTimer::getMSTimeDiffToNow(m_followOrderTime) >= PB_FOLLOW_ORDER_NO_SIT_MS) &&
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
                // Never hit a CC mark or a kited boss unless ordered to.
                Player const* pKiter = GetKiterOf(pVictim);
                sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Mark] %s: leaves %s (%s%s)", me->GetName(), GetLogName(pVictim).c_str(),
                    pKiter ? "kited by " : "CC mark", pKiter ? pKiter->GetName() : "");
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
                        GetLogName(pVictim).c_str(), GetLogName(pPreferred).c_str(), reason);
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
                    GetLogName(pNewVictim).c_str(), reason);
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
        case PB_LAYER_SURVIVAL:
            return "Survival";
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
    // B9: an attack order also hits a crowd-controlled enemy (and breaks the CC); a pull never does.
    m_breakCCTargetGuid = overrideCC ? pTarget->GetObjectGuid() : ObjectGuid();
    if (overrideCC && pTarget->HasBreakableByDamageCrowdControlAura())
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Attack] %s: attacks crowd-controlled %s (attack order breaks the CC)",
            me->GetName(), GetLogName(pTarget).c_str());
    if (IsForbiddenTarget(pTarget))
    {
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Pull] %s: will not pull %s (CC mark; use attack to override)",
            me->GetName(), GetLogName(pTarget).c_str());
        ClearOrderedTarget();
        return;
    }

    // B9 (19g): a tank holding a boss keeps it; the order is left to the other bots (the tank takes
    // the target once the boss is gone or no longer on it).
    if (m_role == ROLE_TANK)
        if (Unit* pBoss = me->GetVictim())
            if (pBoss != pTarget && pBoss->GetVictim() == me && IsBossTarget(pBoss) && IsValidHostileTarget(pBoss))
            {
                sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Tank] %s: keeps boss %s, order on %s left to the others",
                    me->GetName(), GetLogName(pBoss).c_str(), GetLogName(pTarget).c_str());
                return;
            }

    BreakCurrentActions();
    if (m_holdPosition)
    {
        // After the fight the bot follows the leader again (it does not return to the spot).
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Attack] %s: leaves stay to attack %s", me->GetName(), GetLogName(pTarget).c_str());
        SetHoldPosition(false);
    }

    m_rangedPullGuid.Clear();
    if (ranged)
    {
        if (GetRangedWeaponSpell())
        {
            sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Pull] %s: ranged pull on %s", me->GetName(), GetLogName(pTarget).c_str());
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
            GetLogName(pTarget).c_str());
    }

    if (me->GetVictim() && me->GetVictim() != pTarget)
        me->AttackStop(true);
    AttackStart(pTarget);
}

void PartyBotAI::ClearOrderedTarget()
{
    m_orderedTargetGuid.Clear();
    m_orderOverridesCC = false;
    m_breakCCTargetGuid.Clear();
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
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Pull] %s: shot %s, waiting for it", me->GetName(), GetLogName(pTarget).c_str());
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
    m_healerPullCCDuration = 0;                  // any new order also ends a healer's pull CC
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

    // Stay / move here during the wait: walk to the spot (B11).
    if (m_holdPosition && ReturnToHoldSpot())
        return;

    if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != IDLE_MOTION_TYPE)
    {
        me->StopMoving();
        me->GetMotionMaster()->Clear(false, true);
        me->GetMotionMaster()->MoveIdle();
    }
}

void PartyBotAI::StartHealerPullCC(uint32 duration)
{
    m_healerPullCCStart = WorldTimer::getMSTime();
    m_healerPullCCDuration = duration;
}

// Pull timer running: DPS pull wait or a healer's pull CC (CC marks are CC'd once the pull lands).
bool PartyBotAI::IsPullCCWindow() const
{
    return IsPullWaiting() || (m_healerPullCCDuration &&
        WorldTimer::getMSTimeDiffToNow(m_healerPullCCStart) < m_healerPullCCDuration);
}

// A healer crowd-controls its marks after a pull order (for the pull timer) or while the group
// fights; out of a fight it never starts one with crowd control.
bool PartyBotAI::IsHealerCCActive() const
{
    if (m_role != ROLE_HEALER)
        return false;
    return m_groupFighting || me->IsInCombat() || IsPullCCWindow();
}

// Healers: an ally below PB_URGENT_HEAL_PCT is healed before a pending CC mark (logged once).
bool PartyBotAI::HealsBeforeCC()
{
    if (m_role != ROLE_HEALER)
        return false;

    Unit* pInjured = SelectHealTarget(PB_URGENT_HEAL_PCT, PB_URGENT_HEAL_PCT);
    if (!pInjured)
    {
        m_healFirstLogged = false;
        return false;
    }
    if (!m_healFirstLogged && HasPendingCCMark())
    {
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[CC] %s: heals first, %s at %.0f%% (CC mark waits)", me->GetName(),
            GetLogName(pInjured).c_str(), pInjured->GetHealthPercent());
        m_healFirstLogged = true;
    }
    return true;
}

// Once the last CC cast is no longer being cast: did its aura land on the target, was the cast
// cancelled early (moved, interrupted, stunned...) or did it finish without landing (resisted,
// missed, immune; bots get no hit result)?
// Every cast of the bot's CC spell is tracked, from a CC mark or from the stock class rotation
// (e.g. druid Hibernate on its own attacker). A new one first judges the previous one.
void PartyBotAI::OnCastStarted(Unit* pTarget, SpellEntry const* pSpellEntry)
{
    bool const isCC = pSpellEntry == GetCrowdControlSpell() ||
        (me->GetClass() == CLASS_WARLOCK && pSpellEntry == m_spells.warlock.pFear);
    if (!isCC || pTarget == me)
        return;

    if (m_ccCastSpellId)
        UpdateCCCastOutcome(true);

    m_ccCastSpellId = pSpellEntry->Id;
    m_ccCastSource = m_ccCastForMark ? "mark" : "stock auto-CC";
    m_ccCastTargetGuid = pTarget->GetObjectGuid();
    m_ccCastStart = WorldTimer::getMSTime();
    m_ccCastTime = pSpellEntry->GetCastTime(me);
    m_ccCastX = me->GetPositionX();
    m_ccCastY = me->GetPositionY();
}

// B12 diagnostics, every tick (also while the bot is crowd-controlled or dead):
// - every bot: death, with the enemy last targeting it (area damage is not traced);
// - tanks: loss of control (the auras doing it, time left, caster) and regain;
// - the first live slotted tank: each change of a boss's victim.
void PartyBotAI::UpdateDiagnostics()
{
    bool const alive = me->IsAlive();
    if (alive && !me->IsInCombat())
        m_diagLastAttackerGuid.Clear();
    else if (alive)
    {
        for (auto const pAttacker : me->GetAttackers())
            if (pAttacker->GetVictim() == me)
            {
                m_diagLastAttackerGuid = pAttacker->GetObjectGuid();
                break;
            }
    }
    if (alive != m_diagAlive)
    {
        if (!alive)
        {
            Unit const* pKiller = me->GetMap()->GetUnit(m_diagLastAttackerGuid);
            sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Brain] %s: dies (enemy last on it: %s)", me->GetName(),
                pKiller ? GetLogName(pKiller).c_str() : "unknown");
        }
        m_diagAlive = alive;
        m_diagLastAttackerGuid.Clear();
    }

    if (m_role != ROLE_TANK)
        return;

    bool const inControl = !alive || !me->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL);
    if (inControl != m_diagInControl)
    {
        m_diagInControl = inControl;
        if (!inControl)
        {
            m_diagLostControlTime = WorldTimer::getMSTime();
            std::string causes;
            static AuraType const controlAuras[] = { SPELL_AURA_MOD_CONFUSE, SPELL_AURA_MOD_FEAR, SPELL_AURA_MOD_STUN,
                SPELL_AURA_MOD_CHARM, SPELL_AURA_MOD_POSSESS };
            for (AuraType const type : controlAuras)
                for (Aura const* pAura : me->GetAurasByType(type))
                {
                    Unit const* pCaster = pAura->GetCaster();
                    char buffer[200];
                    snprintf(buffer, sizeof(buffer), "%s%s (%u, %.1f s, by %s)", causes.empty() ? "" : ", ",
                        pAura->GetSpellProto()->SpellName[0].c_str(), pAura->GetId(), pAura->GetAuraDuration() / 1000.0f,
                        pCaster ? GetLogName(pCaster).c_str() : "unknown");
                    causes += buffer;
                }
            sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Tank] %s: loses control: %s", me->GetName(),
                causes.empty() ? "no control aura found" : causes.c_str());
        }
        else if (alive)
            sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Tank] %s: back in control after %.1f s", me->GetName(),
                WorldTimer::getMSTimeDiffToNow(m_diagLostControlTime) / 1000.0f);
    }

    // Boss victims: logged by the first live slotted tank only (one line per change).
    if (!me->IsInCombat())
    {
        m_diagBossVictims.clear();
        return;
    }
    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return;
    PartyBotTankSlotMap const& slots = GetTankSlots();
    uint8 const mySlot = GetSlotOf(slots, me);
    if (mySlot == PB_TANK_SLOT_NONE || !alive)
        return;
    for (auto const& itr : slots)
        if (itr.second < mySlot)
            if (Player const* pTank = ObjectAccessor::FindPlayer(itr.first))
                if (pTank->IsAlive() && pTank->IsInMap(me))
                    return;

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || !pMember->IsInMap(me))
            continue;
        for (auto const pAttacker : pMember->GetAttackers())
        {
            if (!pAttacker->IsAlive() || !IsBossTarget(pAttacker))
                continue;
            Unit const* pBossVictim = pAttacker->GetVictim();
            ObjectGuid const victimGuid = pBossVictim ? pBossVictim->GetObjectGuid() : ObjectGuid();
            auto found = m_diagBossVictims.find(pAttacker->GetObjectGuid());
            if (found != m_diagBossVictims.end() && found->second == victimGuid)
                continue;
            Unit const* pOld = found != m_diagBossVictims.end() ? me->GetMap()->GetUnit(found->second) : nullptr;
            sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Tank] boss %s now attacks %s (was %s)",
                GetLogName(pAttacker).c_str(), pBossVictim ? pBossVictim->GetName() : "nobody",
                pOld ? pOld->GetName() : "nobody");
            m_diagBossVictims[pAttacker->GetObjectGuid()] = victimGuid;
        }
    }
}

void PartyBotAI::UpdateCCCastOutcome(bool replaced)
{
    if (!m_ccCastSpellId)
        return;

    if (!replaced)
        if (Spell* pSpell = me->GetCurrentSpell(CURRENT_GENERIC_SPELL))
            if (pSpell->m_spellInfo->Id == m_ccCastSpellId && pSpell->getState() != SPELL_STATE_FINISHED)
                return;

    uint32 const spellId = m_ccCastSpellId;
    m_ccCastSpellId = 0;
    SpellEntry const* pSpellEntry = sSpellMgr.GetSpellEntry(spellId);
    std::string const spellName = std::string(pSpellEntry ? pSpellEntry->SpellName[0].c_str() : "CC spell") +
        " (" + m_ccCastSource + ")";
    uint32 const elapsed = WorldTimer::getMSTimeDiffToNow(m_ccCastStart);

    Unit* pTarget = me->GetMap()->GetUnit(m_ccCastTargetGuid);
    if (!pTarget || !pTarget->IsAlive())
    {
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[CC] %s: %s target gone (dead or despawned)", me->GetName(),
            spellName.c_str());
        return;
    }

    std::string const target = GetLogName(pTarget);
    if (pTarget->HasAura(spellId))
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[CC] %s: %s landed on %s", me->GetName(), spellName.c_str(),
            target.c_str());
    else if (elapsed + 100 < m_ccCastTime)
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[CC] %s: %s on %s cancelled after %.1f of %.1f s (moved %.1f yd%s)",
            me->GetName(), spellName.c_str(), target.c_str(), elapsed / 1000.0f, m_ccCastTime / 1000.0f,
            me->GetDistance2d(m_ccCastX, m_ccCastY), replaced ? ", replaced by a new cast" : "");
    else
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[CC] %s: %s on %s did not land (resisted, missed or immune)",
            me->GetName(), spellName.c_str(), target.c_str());
}

// Hold a given spot (comestay: the leader's position; aedm "move here": the clicked ground);
// ReturnToHoldSpot walks there and keeps the bot there like stay. The order wins at once over
// whatever the bot is doing: casts, eating/drinking, attacks and attack orders are dropped.
void PartyBotAI::OrderHoldSpot(uint32 mapId, float x, float y, float z)
{
    BreakCurrentActions();
    ClearOrderedTarget();
    // B11: a move order keeps the pull wait (the bot walks to the spot and still waits).
    if (IsPullWaiting())
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Pull] %s: keeps waiting on move order (%.1f s left)", me->GetName(),
            (m_pullWaitDuration - std::min(m_pullWaitDuration, WorldTimer::getMSTimeDiffToNow(m_pullWaitStart))) / 1000.0f);
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
    m_followOrderTime = WorldTimer::getMSTime();
    ClearOrderedTarget();
    EndPullWait("follow order");
    SetHoldPosition(false);
    SetFollowMode(PB_FOLLOW_ARRIVE);
    BreakActionsAndFollowNow();
}

void PartyBotAI::OrderStackedFollow(bool stacked)
{
    m_followOrderTime = WorldTimer::getMSTime();
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
        int32 const restore = pSpellEntry ? GetMaxManaRestore(pSpellEntry) : 0;
        if (!restore)
            continue;
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

// Task 16c survival layer: every AI update after the death check, before the auto-shot and
// channel returns. Order: running bandage (stop rules), healing potion, mana (gem, potion),
// new bandage. Never two at once: nothing is used while a spell is cast or channeled.
bool PartyBotAI::UpdateSurvival()
{
    if (m_bandageSpellId)
    {
        Spell const* pChannel = me->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
        if (pChannel && pChannel->m_spellInfo->Id == m_bandageSpellId)
        {
            char const* stop = nullptr;
            if (me->GetHealth() >= me->GetMaxHealth())
                stop = "full health";
            else if (HasPendingCCMark())
                stop = "own CC mark needs crowd control";
            else if (m_holdPosition && !IsOnHoldSpot())
                stop = "off the hold spot";

            if (!stop)
                return true;

            sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Supply] %s: stops bandaging (%s, health %.0f%%)",
                me->GetName(), stop, me->GetHealthPercent());
            me->InterruptSpell(CURRENT_CHANNELED_SPELL);
        }
        else // finished, or broken by the core (damage, moving, item use)
            sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Supply] %s: bandage over (health %.0f%%)",
                me->GetName(), me->GetHealthPercent());
        EndBandage();
    }

    if (!me->IsInCombat() || IsInDuel() || me->IsNonMeleeSpellCasted(false, false, true))
        return false;

    // Healing potion: warriors, rogues and tanks only, while hit and low.
    if (!me->GetAttackers().empty() && me->GetHealthPercent() < PB_POTION_HEALTH_PCT &&
       (me->GetClass() == CLASS_WARRIOR || me->GetClass() == CLASS_ROGUE || m_role == ROLE_TANK))
    {
        if (uint32 const itemId = GetBestSupplyRank(PB_HEALING_POTIONS, me->GetLevel()))
        {
            if (UseSupplyItem(itemId, "low health while attacked"))
            {
                SetActiveLayer(PB_LAYER_SURVIVAL);
                return true;
            }
        }
    }

    // Mana users: the mage's own gem first, then a mana potion. Not in Bear/Cat Form (rage/energy:
    // mana is useless there and the potion cooldown would block a healing potion).
    if (me->GetPowerType() == POWER_MANA &&
       ((me->GetClass() == CLASS_MAGE && UseManaGem()) || UseManaPotion()))
    {
        SetActiveLayer(PB_LAYER_SURVIVAL);
        return true;
    }

    char const* why = "";
    if (ShouldBandage(why) && StartBandage(why))
    {
        SetActiveLayer(PB_LAYER_SURVIVAL);
        return true;
    }

    return false;
}

bool PartyBotAI::UseSupplyItem(uint32 itemId, char const* why)
{
    ItemPrototype const* pProto = sObjectMgr.GetItemPrototype(itemId);
    SpellEntry const* pSpellEntry = pProto ? sSpellMgr.GetSpellEntry(pProto->Spells[0].SpellId) : nullptr;
    // Cooldown first (potions share 2 min on the bot): no item is added while it runs.
    if (!pSpellEntry || !me->IsSpellReady(pSpellEntry, pProto))
        return false;

    Item* pItem = FindInventoryItem(me, itemId);
    bool const added = !pItem;
    if (added)
    {
        if (!AddItemToInventory(itemId) || !(pItem = FindInventoryItem(me, itemId)))
        {
            if (!m_supplyBagFullLogged)
            {
                m_supplyBagFullLogged = true;
                sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Supply] %s: bags full, no %s (%s)", me->GetName(),
                    pProto->Name1, why);
            }
            return false;
        }
    }

    float const healthPct = me->GetHealthPercent();
    float const manaPct = me->GetMaxPower(POWER_MANA) ? me->GetPowerPercent(POWER_MANA) : 0.0f;
    if (UseItemEffect(pItem)) // the item may be gone now (used up)
    {
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Supply] %s: uses %s (%s, health %.0f%%, mana %.0f%%)",
            me->GetName(), pProto->Name1, why, healthPct, manaPct);
        return true;
    }

    if (added)
        me->DestroyItemCount(itemId, 1, true);
    sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Supply] %s: could not use %s (%s)%s", me->GetName(),
        pProto->Name1, why, added ? ", added item removed" : "");
    return false;
}

// Mana potion of the bot's level at PB_MANA_POTION_PCT or less (user 2026-11-11: early, so the
// 2 min cooldown runs before the bot is out of mana).
bool PartyBotAI::UseManaPotion()
{
    uint32 const itemId = GetBestSupplyRank(PB_MANA_POTIONS, me->GetLevel());
    if (!itemId || me->GetPowerPercent(POWER_MANA) > PB_MANA_POTION_PCT)
        return false;

    return UseSupplyItem(itemId, "mana");
}

// Bandage rules (in combat, checked by UpdateSurvival): nobody attacks the bot, no Recently
// Bandaged, no pull wait, on its spot when holding one, own CC marks handled; then by role.
bool PartyBotAI::ShouldBandage(char const*& why) const
{
    if (!me->GetAttackers().empty() || me->HasAura(PB_SPELL_RECENTLY_BANDAGED) || IsPullWaiting() ||
       (m_holdPosition && !IsOnHoldSpot()) || HasPendingCCMark())
        return false;

    // Toggle: the bot moves with the leader, a bandage would break at once.
    if (!m_holdPosition && m_followMode == PB_FOLLOW_STACKED)
        if (Player const* pLeader = GetPartyLeader())
            if (pLeader->IsMoving())
                return false;

    float const healthPct = me->GetHealthPercent();
    switch (m_role)
    {
        case ROLE_TANK:
            // Not while anything still fights the group (it may need the tank any second).
            if (me->GetVictim() || healthPct >= PB_BANDAGE_HEALTH_PCT || SelectPartyAttackTarget())
                return false;
            why = "tank, no target";
            return true;
        case ROLE_HEALER:
        {
            if (healthPct >= PB_HEALER_BANDAGE_HEALTH_PCT)
                return false;
            uint32 cheapestHeal = UINT32_MAX;
            for (auto const& pSpellEntry : m_spellListDirectHeal)
                cheapestHeal = std::min(cheapestHeal, Spell::CalculatePowerCost(pSpellEntry, me, nullptr, nullptr, false));
            for (auto const& pSpellEntry : m_spellListPeriodicHeal)
                cheapestHeal = std::min(cheapestHeal, Spell::CalculatePowerCost(pSpellEntry, me, nullptr, nullptr, false));
            if (cheapestHeal != UINT32_MAX && me->GetPower(POWER_MANA) >= cheapestHeal)
                return false;
            why = "healer without mana for a heal";
            return true;
        }
        default:
            if (healthPct >= PB_BANDAGE_HEALTH_PCT)
                return false;
            why = "low health, not attacked";
            return true;
    }
}

// Best bandage for the First Aid skill, used on the bot itself. Standing still first; melee and
// auto shot stop (own attacks break it), the target is attacked again afterwards.
bool PartyBotAI::StartBandage(char const* why)
{
    uint32 const itemId = GetBestSupplyRank(PB_BANDAGES, me->GetSkillValuePure(SKILL_FIRST_AID));
    ItemPrototype const* pProto = itemId ? sObjectMgr.GetItemPrototype(itemId) : nullptr;
    if (!pProto)
        return false;

    if (me->IsMoving() || me->GetMotionMaster()->GetCurrentMovementGeneratorType() != IDLE_MOTION_TYPE)
    {
        me->StopMoving();
        me->GetMotionMaster()->Clear(false, true);
        me->GetMotionMaster()->MoveIdle();
    }

    Unit* pVictim = me->GetVictim();
    if (!UseSupplyItem(itemId, why))
        return false;

    m_bandageSpellId = pProto->Spells[0].SpellId;
    m_bandageTargetGuid = pVictim ? pVictim->GetObjectGuid() : ObjectGuid();
    if (pVictim)
        me->AttackStop();
    if (me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
        me->InterruptSpell(CURRENT_AUTOREPEAT_SPELL, true);
    return true;
}

void PartyBotAI::EndBandage()
{
    m_bandageSpellId = 0;
    ObjectGuid const targetGuid = m_bandageTargetGuid;
    m_bandageTargetGuid.Clear();
    if (targetGuid.IsEmpty() || !me->IsAlive() || me->GetVictim())
        return;

    Unit* pTarget = me->GetMap()->GetUnit(targetGuid);
    if (pTarget && IsValidHostileTarget(pTarget) && !IsForbiddenTarget(pTarget) && AttackStart(pTarget))
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Supply] %s: attacks %s again after bandaging", me->GetName(),
            GetLogName(pTarget).c_str());
}

bool PartyBotAI::IsOnHoldSpot() const
{
    return me->GetMapId() == m_holdMapId &&
           me->GetDistance(m_holdX, m_holdY, m_holdZ, SizeFactor::None) <= PB_HOLD_SPOT_TOLERANCE;
}

// First Aid: the spell ranks up to the skill, skill = level x 5 (max 300). Never lowered.
// (UpdateSkillsToMaxSkillsForLevel skips professions.)
void PartyBotAI::UpdateFirstAid()
{
    uint16 const skill = uint16(std::min<uint32>(me->GetLevel() * 5, 300));
    uint16 const maxSkill = ((skill + PB_FIRST_AID_RANK_SKILL - 1) / PB_FIRST_AID_RANK_SKILL) * PB_FIRST_AID_RANK_SKILL;
    for (uint32 i = 0; i < 4 && PB_FIRST_AID_RANK_SKILL * i < maxSkill; ++i)
        if (!me->HasSpell(PB_FIRST_AID_SPELLS[i]))
            me->LearnSpell(PB_FIRST_AID_SPELLS[i], false);

    uint16 const oldSkill = me->GetSkillValuePure(SKILL_FIRST_AID);
    uint16 const oldMax = me->GetSkillMaxPure(SKILL_FIRST_AID);
    if (oldSkill >= skill && oldMax >= maxSkill)
        return;

    uint16 const newSkill = std::max(oldSkill, skill);
    me->SetSkill(SKILL_FIRST_AID, newSkill, std::max(oldMax, maxSkill));
    sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Supply] %s: First Aid %u -> %u/%u (bandage %u)", me->GetName(),
        oldSkill, me->GetSkillValuePure(SKILL_FIRST_AID), me->GetSkillMaxPure(SKILL_FIRST_AID),
        GetBestSupplyRank(PB_BANDAGES, newSkill));
}

// Bottomless Bag in every empty bag slot; existing bags stay. Hunters keep one slot free (quiver).
void PartyBotAI::FillEmptyBagSlots()
{
    std::vector<uint8> emptySlots;
    for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
        if (!me->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            emptySlots.push_back(slot);
    if (me->GetClass() == CLASS_HUNTER && !emptySlots.empty())
        emptySlots.pop_back();

    uint32 added = 0;
    for (uint8 slot : emptySlots)
    {
        uint16 dest;
        if (me->CanEquipNewItem(slot, dest, PB_ITEM_BOTTOMLESS_BAG, false) != EQUIP_ERR_OK ||
            !me->EquipNewItem(dest, PB_ITEM_BOTTOMLESS_BAG, true))
        {
            sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Supply] %s: cannot equip Bottomless Bag in bag slot %u",
                me->GetName(), slot);
            break;
        }
        ++added;
    }
    if (added)
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Supply] %s: gets %u Bottomless Bag(s)", me->GetName(), added);
}

void PartyBotAI::RepairGear(char const* why)
{
    me->DurabilityRepairAll(false, 0.0f);
    sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Supply] %s: gear repaired (%s)", me->GetName(), why);
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

        if (m_role != ROLE_TANK && (me->GetVictim() || IsHealerCCActive()) && !HealsBeforeCC() &&
            CrowdControlMarkedTargets())
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

// The highest-level premade spec of the bot's class and role (the level 60 PvE specs).
PlayerPremadeSpecTemplate const* PartyBotAI::GetRoleSpec() const
{
    PlayerPremadeSpecTemplate const* pSpec = nullptr;
    for (auto const& itr : sObjectMgr.GetPlayerPremadeSpecTemplates())
    {
        if (itr.second.requiredClass == me->GetClass() && itr.second.role == m_role &&
           (!pSpec || itr.second.level > pSpec->level))
            pSpec = &itr.second;
    }
    return pSpec;
}

// Ranks of a talent the bot has learned (0 = none).
static uint32 GetLearnedTalentRanks(Player const* pPlayer, TalentEntry const* pTalent)
{
    for (int32 rank = MAX_TALENT_RANK - 1; rank >= 0; --rank)
        if (pTalent->RankID[rank] && pPlayer->HasSpell(pTalent->RankID[rank]))
            return rank + 1;
    return 0;
}

// Spends free talent points on the spec. A spec spell that is a higher trainer rank of a
// talent (e.g. Bloodthirst rank 4) counts as rank 1 of that talent. Each point goes to the
// spec's main tree (most ranks) first, deepest talent the talent rules allow first, then to
// the other trees, so key talents arrive as soon as their tier opens.
void PartyBotAI::LearnRoleSpecTalents(PlayerPremadeSpecTemplate const& spec)
{
    struct SpecTalent
    {
        TalentEntry const* talent;
        uint32 ranks; // ranks the spec wants
    };
    std::vector<SpecTalent> talents;
    std::map<uint32, uint32> ranksPerTab;

    for (uint32 spellId : spec.spells)
    {
        TalentSpellPos const* pPos = GetTalentSpellPos(spellId);
        if (!pPos)
        {
            uint32 const firstRankId = sSpellMgr.GetFirstSpellInChain(spellId);
            if (firstRankId && firstRankId != spellId)
                pPos = GetTalentSpellPos(firstRankId);
        }
        if (!pPos)
            continue; // not a talent (trainer spell)

        TalentEntry const* pTalent = sTalentStore.LookupEntry(pPos->talent_id);
        if (!pTalent)
            continue;

        // A trainer rank maps to rank 1 of its talent; keep the highest rank per talent.
        uint32 const ranks = (GetTalentSpellPos(spellId) ? pPos->rank : 0) + 1;
        auto itr = std::find_if(talents.begin(), talents.end(),
            [pTalent](SpecTalent const& st) { return st.talent == pTalent; });
        if (itr == talents.end())
        {
            talents.push_back({ pTalent, ranks });
            ranksPerTab[pTalent->TalentTab] += ranks;
        }
        else if (ranks > itr->ranks)
        {
            ranksPerTab[pTalent->TalentTab] += ranks - itr->ranks;
            itr->ranks = ranks;
        }
    }

    uint32 mainTab = 0;
    uint32 mainRanks = 0;
    for (auto const& itr : ranksPerTab)
    {
        if (itr.second > mainRanks)
        {
            mainTab = itr.first;
            mainRanks = itr.second;
        }
    }

    // Main tree first, then deepest tier first; spec order otherwise.
    std::stable_sort(talents.begin(), talents.end(), [mainTab](SpecTalent const& a, SpecTalent const& b)
    {
        bool const aMain = a.talent->TalentTab == mainTab;
        bool const bMain = b.talent->TalentTab == mainTab;
        if (aMain != bMain)
            return aMain;
        return a.talent->Row > b.talent->Row;
    });

    std::vector<std::pair<TalentEntry const*, uint32>> learned; // talent, ranks after this call (learn order)
    while (me->GetFreeTalentPoints())
    {
        bool learnedOne = false;
        for (SpecTalent const& st : talents)
        {
            uint32 const current = GetLearnedTalentRanks(me, st.talent);
            if (current >= st.ranks)
                continue;

            if (me->LearnTalent(st.talent->TalentID, current))
            {
                auto itr = std::find_if(learned.begin(), learned.end(),
                    [&st](std::pair<TalentEntry const*, uint32> const& l) { return l.first == st.talent; });
                if (itr == learned.end())
                    learned.emplace_back(st.talent, current + 1);
                else
                    itr->second = current + 1;
                learnedOne = true;
                break;
            }
        }
        if (!learnedOne)
            break;
    }

    if (learned.empty())
        return;

    std::string list;
    for (auto const& itr : learned)
    {
        uint32 maxRanks = 0;
        while (maxRanks < MAX_TALENT_RANK && itr.first->RankID[maxRanks])
            ++maxRanks;
        SpellEntry const* pSpellEntry = sSpellMgr.GetSpellEntry(itr.first->RankID[0]);
        if (!list.empty())
            list += ", ";
        list += (pSpellEntry ? pSpellEntry->SpellName[0] : std::to_string(itr.first->TalentID)) +
            " " + std::to_string(itr.second) + "/" + std::to_string(maxRanks);
    }
    sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Talent] %s: level %u, spec %s: %s",
        me->GetName(), me->GetLevel(), spec.name.c_str(), list.c_str());
}

// Talents of the role's spec, then class quest spells, then trainer spells up to the current level
// (in this order: higher ranks need rank 1, e.g. Bloodthirst needs the talent, Dire Bear Form the
// quest Bear Form). resetTalents starts the talents over (new bot, respec). Points the spec
// cannot use stay unspent and are logged.
void PartyBotAI::LearnSpellsAndTalentsForLevel(bool resetTalents)
{
    if (resetTalents)
        me->ResetTalents(true);

    PlayerPremadeSpecTemplate const* pSpec = GetRoleSpec();
    if (pSpec)
        LearnRoleSpecTalents(*pSpec);

    // Class quest spells (warlock demons and mounts, Berserker Stance, Bear Form, Tame Beast):
    // trainers do not teach them, so without this a bot that dings never gets them.
    char args[] = "";
    ChatHandler(me).HandleLearnAllQuestsCommand(args);
    ChatHandler(me).HandleLearnAllTrainerCommand(args);
    LearnClassBookSpells();

    if (uint32 const unspent = me->GetFreeTalentPoints())
    {
        if (pSpec)
            sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Talent] %s: level %u, %u points unspent (spec %s)",
                me->GetName(), me->GetLevel(), unspent, pSpec->name.c_str());
        else
            sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Talent] %s: level %u, %u points unspent (no spec for role %s)",
                me->GetName(), me->GetLevel(), unspent, GetRoleName(m_role));
    }
}

// Class books that only drop in raids (checked in the loot tables): bots start pre-raid.
static uint32 const RaidOnlyClassBooks[] =
{
    // Ruins of Ahn'Qiraj (Kurinnaxx, Rajaxx, Moam, Buru, Ayamiss, Ossirian)
    21214, 21279, 21280,    // mage: Frostbolt XI, Fireball XII, Arcane Missiles VIII
    21281, 21282, 21283,    // warlock: Shadow Bolt X, Immolate VIII, Corruption VII
    21284, 21285, 21287,    // priest: Greater Heal V, Renew X, Prayer of Healing V
    21288, 21289, 21290,    // paladin: Blessing of Wisdom VI, Blessing of Might VII, Holy Light IX
    21291, 21292, 21293,    // shaman: Healing Wave X, Strength of Earth Totem V, Grace of Air Totem III
    21294, 21295, 21296,    // druid: Healing Touch XI, Starfire VII, Rejuvenation XI
    21297, 21298, 21299,    // warrior: Heroic Strike IX, Battle Shout VII, Revenge VI
    21300, 21302, 21303,    // rogue: Backstab IX, Deadly Poison V, Feint V
    21304, 21306, 21307,    // hunter: Multi-Shot V, Serpent Sting IX, Aspect of the Hawk VII
    16665,                  // Tome of Tranquilizing Shot (Lucifron, Molten Core)
    22739,                  // Tome of Polymorph: Turtle (Gahz'ranka, Zul'Gurub)
};

// Class books (one class, no profession, obtainable, usable at the bot's level) teach spells that no
// trainer or quest gives, e.g. Prayer of Fortitude, Gift of the Wild, Arcane Brilliance. Grimoires
// teach the demon (target pet) and are left out.
void PartyBotAI::LearnClassBookSpells()
{
    std::string learned;
    std::string skipped;
    for (auto const& itr : sObjectMgr.GetItemPrototypeMap())
    {
        ItemPrototype const& proto = itr.second;
        if (proto.Class != ITEM_CLASS_RECIPE || proto.SubClass != ITEM_SUBCLASS_BOOK ||
            proto.HasExtraFlag(ITEM_EXTRA_NOT_OBTAINABLE) || proto.RequiredSkill ||
            (proto.AllowableClass & CLASSMASK_ALL_PLAYABLE) != me->GetClassMask() ||
            proto.RequiredLevel > me->GetLevel() ||
            proto.Spells[0].SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
            continue;

        SpellEntry const* pTeach = sSpellMgr.GetSpellEntry(proto.Spells[0].SpellId);
        if (!pTeach)
            continue;

        for (uint32 i = 0; i < MAX_EFFECT_INDEX; ++i)
        {
            uint32 const spellId = pTeach->EffectTriggerSpell[i];
            if (pTeach->Effect[i] != SPELL_EFFECT_LEARN_SPELL || pTeach->EffectImplicitTargetA[i] != TARGET_UNIT_CASTER ||
                !spellId || me->HasSpell(spellId))
                continue;

            SpellEntry const* pSpell = sSpellMgr.GetSpellEntry(spellId);
            std::string const name = pSpell ? pSpell->SpellName[0] + (pSpell->Rank[0].empty() ? "" : " (" + pSpell->Rank[0] + ")") : std::to_string(spellId);
            if (std::find(std::begin(RaidOnlyClassBooks), std::end(RaidOnlyClassBooks), proto.ItemId) != std::end(RaidOnlyClassBooks))
            {
                skipped += (skipped.empty() ? "" : ", ") + name;
                continue;
            }

            me->LearnSpell(spellId, false);
            learned += (learned.empty() ? "" : ", ") + name;
        }
    }

    if (!learned.empty())
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Book] %s: level %u, learned %s", me->GetName(), me->GetLevel(), learned.c_str());
    if (!skipped.empty())
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Book] %s: level %u, raid-drop books skipped: %s", me->GetName(), me->GetLevel(), skipped.c_str());
}

// setrole on an added bot: talents start over for the new role (the caller waits until the
// bot is out of combat). Spell data is rebuilt on the next update.
void PartyBotAI::RespecForRole()
{
    m_pendingRespec = false;
    sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Talent] %s: respec for role %s",
        me->GetName(), GetRoleName(m_role));
    LearnSpellsAndTalentsForLevel(true);
    m_resetSpellData = true;
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
    FillEmptyBagSlots();
    AddRangedAmmo();
    UpdateFirstAid();
    RepairGear("level up");
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

    // F3: a boss kited by a group hunter is left to it.
    if (Player const* pKiter = GetKiterOf(pTarget))
        return pKiter != me;

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

bool PartyBotAI::IsBossTarget(Unit const* pUnit) const
{
    if (pUnit->GetTypeId() == TYPEID_UNIT && static_cast<Creature const*>(pUnit)->IsWorldBoss())
        return true;

    for (auto markId : m_marksToFocus)
        if (markId != RAID_TARGET_ICON_SKULL && GetMarkedTarget(markId) == pUnit)
            return true;
    return false;
}

// The boss owner is the highest-slot tank (MT, then OT1...) that is alive within 50 yd and in
// control of itself: a disoriented (Conflagration), feared or stunned MT hands the boss to OT1.
bool PartyBotAI::IsBossTankOwner(PartyBotTankSlotMap const& slots) const
{
    uint8 const mySlot = GetSlotOf(slots, me);
    for (auto const& itr : slots)
    {
        if (itr.first == me->GetObjectGuid() || (mySlot != PB_TANK_SLOT_NONE && itr.second >= mySlot))
            continue;
        Player const* pTank = ObjectAccessor::FindPlayer(itr.first);
        if (pTank && pTank->IsAlive() && pTank->IsInMap(me) && me->IsWithinDist(pTank, 50.0f) &&
           !pTank->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL))
            return false; // a higher tank can take it
    }
    return true;
}

// Nearest enemy attacking a group member without a tank slot, that no other tank bot is
// already going for (so off-tanks share the adds out). Bosses only: the boss owner takes them
// even when another tank is going for them.
Unit* PartyBotAI::SelectLooseEnemy(PartyBotTankSlotMap const& slots, LooseBossFilter filter) const
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

            if (filter != PB_LOOSE_ANY && IsBossTarget(pAttacker) != (filter == PB_LOOSE_BOSSES_ONLY))
                continue;

            bool claimed = false;
            for (GroupReference* itr2 = pGroup->GetFirstMember(); filter != PB_LOOSE_BOSSES_ONLY && itr2 != nullptr; itr2 = itr2->next())
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
                GetLogName(pEnemy).c_str(), GetLogName(pEnemyVictim).c_str());
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
        me->GetName(), GetLogName(pVictim).c_str(), GetLogName(pBest).c_str(), bestLead, bestInDanger ? ", about to be pulled" : "");
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
                pVictim ? GetLogName(pVictim).c_str() : "none", GetLogName(pMark).c_str(), markReason);
            me->AttackStop(true);
            AttackStart(pMark);
        }
        return TauntIfAllowed(pMark, slots, otherTanks);
    }

    // Boss rule (19b, 19g): a tank stays on a boss while it holds it, and the boss owner (MT) also
    // while the boss is loose. Marks, pull and attack orders, loose adds and threat rescue never
    // pull it off (only singlefocus above does): other tanks take them. Loose bosses go to the owner.
    bool const bossOwner = IsBossTankOwner(slots);
    if (pVictim && IsBossTarget(pVictim) &&
       (pVictim->GetVictim() == me ||
       (bossOwner && GetSlotOf(slots, pVictim->GetVictim()) == PB_TANK_SLOT_NONE)))
    {
        if (m_heldBossGuid != pVictim->GetObjectGuid())
        {
            sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Tank] %s (%s): stays on boss %s", me->GetName(),
                GetTankSlotName(mySlot).c_str(), GetLogName(pVictim).c_str());
            m_heldBossGuid = pVictim->GetObjectGuid();
        }
        // An add no other tank goes for is left to the group (CC, damage dealers).
        if (Unit* pLoose = SelectLooseEnemy(slots, PB_LOOSE_NO_BOSSES))
            if (m_looseAddLoggedGuid != pLoose->GetObjectGuid())
            {
                sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Tank] %s: keeps boss %s, no free off-tank for %s (attacks %s)",
                    me->GetName(), GetLogName(pVictim).c_str(), GetLogName(pLoose).c_str(),
                    pLoose->GetVictim() ? pLoose->GetVictim()->GetName() : "nobody");
                m_looseAddLoggedGuid = pLoose->GetObjectGuid();
            }
        return TauntIfAllowed(pVictim, slots, otherTanks);
    }
    m_heldBossGuid.Clear();

    if (bossOwner)
    {
        if (Unit* pBoss = SelectLooseEnemy(slots, PB_LOOSE_BOSSES_ONLY))
        {
            sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Tank] %s (%s): grabs boss %s from %s", me->GetName(),
                GetTankSlotName(mySlot).c_str(), GetLogName(pBoss).c_str(),
                pBoss->GetVictim() ? pBoss->GetVictim()->GetName() : "nobody");
            me->AttackStop(true);
            AttackStart(pBoss);
            m_heldBossGuid = pBoss->GetObjectGuid();
            return TauntIfAllowed(pBoss, slots, otherTanks);
        }
    }

    // With a mark: free to leave it while it is covered - on us (even with a thin threat lead:
    // enemies already hitting group members come first) or held by another tank. A boss mark is
    // left to the boss owner.
    Unit* pMarkVictim = pMark ? pMark->GetVictim() : nullptr;
    bool const markHeld = (pMarkVictim &&
        (pMarkVictim == me || GetSlotOf(slots, pMarkVictim) != PB_TANK_SLOT_NONE)) ||
        (pMark && !bossOwner && IsBossTarget(pMark));
    // Back to the mark when it hits someone else; a mark that fights nobody yet (pull target on its
    // way, or not pulled) waits until no loose enemy is left.
    LooseBossFilter const looseFilter = bossOwner ? PB_LOOSE_ANY : PB_LOOSE_NO_BOSSES;
    if (pMark && pVictim != pMark && !markHeld && (pMarkVictim || !SelectLooseEnemy(slots, looseFilter)))
    {
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Target] %s: %s -> %s (%s)", me->GetName(),
            pVictim ? GetLogName(pVictim).c_str() : "none", GetLogName(pMark).c_str(), markReason);
        me->AttackStop(true);
        AttackStart(pMark);
        return TauntIfAllowed(pMark, slots, otherTanks);
    }

    // Grab a loose enemy unless the current target is one already (a loose boss counts only for
    // the boss owner).
    bool const victimIsLoose = pVictim && pVictim->GetVictim() && pVictim->GetVictim() != me &&
        GetSlotOf(slots, pVictim->GetVictim()) == PB_TANK_SLOT_NONE && (bossOwner || !IsBossTarget(pVictim));
    if (!victimIsLoose)
    {
        if (Unit* pLoose = SelectLooseEnemy(slots, looseFilter))
        {
            if (pLoose != pVictim)
            {
                sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Tank] %s (%s): grabs %s from %s", me->GetName(),
                    GetTankSlotName(mySlot).c_str(), GetLogName(pLoose).c_str(),
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

    if ((bossOwner || !IsBossTarget(pVictim)) && TauntIfAllowed(pVictim, slots, otherTanks))
        return true;
    return pMark && (bossOwner || !IsBossTarget(pMark)) && TauntIfAllowed(pMark, slots, otherTanks);
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
                        me->GetName(), GetLogName(pEnemy).c_str(), pSpell->SpellName[0].c_str());
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

// Task 18b shared interrupts.
// Interrupts with a cooldown above this are "long" (Counterspell, Silence, Bash): used only when
// no short one (Kick, Pummel, Shield Bash, Earth Shock) of the group is ready for that cast.
static uint32 const PB_INTERRUPT_LONG_COOLDOWN = 15 * IN_MILLISECONDS;
// A long interrupt still fires when less than this is left of the cast.
static uint32 const PB_INTERRUPT_LAST_CHANCE = 1 * IN_MILLISECONDS;
// How long a claimed cast stays reserved for the bot that interrupted it.
static uint32 const PB_INTERRUPT_CLAIM_TIME = 1500;

struct PartyBotInterruptClaim
{
    ObjectGuid botGuid;
    uint32 spellId;
    uint32 claimTime;
};
// Enemy caster -> the bot that interrupted its current cast. Shared by all bots (map threads).
static std::mutex s_interruptClaimsLock;
static std::map<ObjectGuid, PartyBotInterruptClaim> s_interruptClaims;

// The enemy's cast that an interrupt would stop (same rule as Spell::EffectInterruptCast:
// silence-type spells with a cast time, or channels that actions cancel), or nullptr.
static Spell const* GetInterruptibleCast(Unit const* pCaster)
{
    if (Spell const* pSpell = pCaster->GetCurrentSpell(CURRENT_GENERIC_SPELL))
    {
        if (pSpell->GetCastTime() > 0 && pSpell->getState() == SPELL_STATE_PREPARING &&
            pSpell->m_spellInfo->PreventionType == SPELL_PREVENTION_TYPE_SILENCE &&
            pSpell->m_spellInfo->HasSpellInterruptFlag(SPELL_INTERRUPT_FLAG_DAMAGE_PUSHBACK))
            return pSpell;
    }
    if (Spell const* pSpell = pCaster->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
    {
        if (pSpell->getState() == SPELL_STATE_CASTING &&
            pSpell->m_spellInfo->PreventionType == SPELL_PREVENTION_TYPE_SILENCE &&
            pSpell->m_spellInfo->HasChannelInterruptFlag(AURA_INTERRUPT_ACTION_CANCELS))
            return pSpell;
    }
    return nullptr;
}

// True if another bot already interrupted this cast (claims of ended casts are dropped).
static bool IsInterruptClaimed(Unit const* pCaster, uint32 spellId)
{
    std::lock_guard<std::mutex> guard(s_interruptClaimsLock);
    uint32 const now = WorldTimer::getMSTime();
    for (auto itr = s_interruptClaims.begin(); itr != s_interruptClaims.end();)
    {
        if (WorldTimer::getMSTimeDiff(itr->second.claimTime, now) >= PB_INTERRUPT_CLAIM_TIME)
            itr = s_interruptClaims.erase(itr);
        else
            ++itr;
    }
    auto itr = s_interruptClaims.find(pCaster->GetObjectGuid());
    return itr != s_interruptClaims.end() && itr->second.spellId == spellId;
}

static void ClaimInterrupt(Unit const* pCaster, uint32 spellId, ObjectGuid botGuid)
{
    std::lock_guard<std::mutex> guard(s_interruptClaimsLock);
    s_interruptClaims[pCaster->GetObjectGuid()] = { botGuid, spellId, WorldTimer::getMSTime() };
}

SpellEntry const* PartyBotAI::GetReadyInterrupt(Unit* pCaster, bool shortCooldownOnly) const
{
    if (m_role == ROLE_HEALER || m_preventCasting)
        return nullptr;

    SpellEntry const* candidates[2] = { nullptr, nullptr };
    switch (me->GetClass())
    {
        case CLASS_WARRIOR: // Pummel needs Berserker Stance, Shield Bash a shield (no stance dancing)
            candidates[0] = m_spells.warrior.pPummel;
            if (IsWearingShield(me))
                candidates[1] = m_spells.warrior.pShieldBash;
            break;
        case CLASS_ROGUE:
            candidates[0] = m_spells.rogue.pKick;
            break;
        case CLASS_SHAMAN:
            candidates[0] = m_spells.shaman.pEarthShock;
            break;
        case CLASS_MAGE:
            candidates[0] = m_spells.mage.pCounterspell;
            break;
        case CLASS_PRIEST:
            candidates[0] = m_spells.priest.pSilence;
            break;
        case CLASS_DRUID: // Bear Form only (the shapeshift check in CanTryToCastSpell)
            candidates[0] = m_spells.druid.pBash;
            break;
    }

    for (SpellEntry const* pSpellEntry : candidates)
    {
        if (!pSpellEntry)
            continue;
        if (shortCooldownOnly &&
            std::max(pSpellEntry->RecoveryTime, pSpellEntry->CategoryRecoveryTime) > PB_INTERRUPT_LONG_COOLDOWN)
            continue;
        if (CanTryToCastSpell(pCaster, pSpellEntry) && me->IsWithinLOSInMap(pCaster))
            return pSpellEntry;
    }
    return nullptr;
}

bool PartyBotAI::GroupHasShortInterruptReady(Unit* pCaster) const
{
    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return false;

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || pMember == me || !pMember->IsAlive() || !pMember->IsInWorld() ||
            pMember->GetMap() != me->GetMap() || pMember->IsNonMeleeSpellCasted(false, false, true))
            continue;

        if (PartyBotAI const* pAI = dynamic_cast<PartyBotAI const*>(pMember->AI()))
        {
            if (!pAI->IsPullWaiting() && pAI->GetReadyInterrupt(pCaster, true))
                return true;
        }
    }
    return false;
}

bool PartyBotAI::UpdateInterrupts()
{
    if (m_role == ROLE_HEALER || m_preventCasting)
        return false;

    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return false;

    // Enemies fighting the group: everyone attacking a member, and every member's target.
    // Own target first.
    std::vector<Unit*> enemies;
    auto addEnemy = [&](Unit* pUnit)
    {
        if (pUnit && pUnit->IsAlive() && std::find(enemies.begin(), enemies.end(), pUnit) == enemies.end())
            enemies.push_back(pUnit);
    };
    addEnemy(me->GetVictim());
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || !pMember->IsInWorld() || pMember->GetMap() != me->GetMap())
            continue;
        addEnemy(pMember->GetVictim());
        for (Unit* pAttacker : pMember->GetAttackers())
            addEnemy(pAttacker);
    }

    for (Unit* pCaster : enemies)
    {
        // Under crowd control or a CC mark: left to the crowd control.
        if (!IsValidHostileTarget(pCaster) || IsForbiddenTarget(pCaster))
            continue;

        Spell const* pCast = GetInterruptibleCast(pCaster);
        if (!pCast || IsInterruptClaimed(pCaster, pCast->m_spellInfo->Id))
            continue;

        SpellEntry const* pInterrupt = GetReadyInterrupt(pCaster, false);
        if (!pInterrupt)
            continue;

        // Long cooldown: leave the cast to a short interrupt of the group, unless it is about to land.
        if (std::max(pInterrupt->RecoveryTime, pInterrupt->CategoryRecoveryTime) > PB_INTERRUPT_LONG_COOLDOWN &&
            pCast->GetCastedTime() > PB_INTERRUPT_LAST_CHANCE && GroupHasShortInterruptReady(pCaster))
            continue;

        uint32 const castSpellId = pCast->m_spellInfo->Id;
        std::string const castName = pCast->m_spellInfo->SpellName[0];
        uint32 const timeLeft = pCast->GetCastedTime();
        if (DoCastSpell(pCaster, pInterrupt) == SPELL_CAST_OK)
        {
            ClaimInterrupt(pCaster, castSpellId, me->GetObjectGuid());
            sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Interrupt] %s: %s -> %s (%s, %.1f s left)%s", me->GetName(),
                pInterrupt->SpellName[0].c_str(), GetLogName(pCaster).c_str(), castName.c_str(), timeLeft / 1000.0f,
                pCaster == me->GetVictim() ? "" : " [not own target]");
            return true;
        }
    }
    return false;
}

void PartyBotAI::UpdateInCombatAI()
{
    if (!IsInDuel() && UpdateInterrupts())
        return;

    if (!IsInDuel())
    {
        if (m_role == ROLE_TANK)
        {
            if (UpdateTankDuty())
                return;
        }
        else if (!HealsBeforeCC() && CrowdControlMarkedTargets())
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
            me->GetName(), GetLogName(pPet).c_str(), pPet->GetLevel());
    }

    if (pPet->GetLevel() != me->GetLevel())
    {
        uint32 const oldLevel = pPet->GetLevel();
        pPet->GivePetLevel(me->GetLevel());
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Pet] %s: pet %s level %u -> %u (hunter %u)",
            me->GetName(), GetLogName(pPet).c_str(), oldLevel, pPet->GetLevel(), me->GetLevel());
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

    // Party aura, lasts until death; CanTryToCastSpell skips it while it is up.
    if (m_spells.hunter.pTrueshotAura &&
        CanTryToCastSpell(me, m_spells.hunter.pTrueshotAura))
    {
        if (DoCastSpell(me, m_spells.hunter.pTrueshotAura) == SPELL_CAST_OK)
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

bool PartyBotAI::IsSteppingBack() const
{
    return m_steppingBack && WorldTimer::getMSTimeDiffToNow(m_stepBackTime) < PB_STEP_BACK_MAX_TIME &&
        me->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE;
}

bool PartyBotAI::IsSafeStepBackSpot(Unit const* pVictim, float x, float y, float z,
    std::list<Unit*> const& enemies) const
{
    if (!pVictim->IsWithinLOS(x, y, z))
        return false;

    for (auto const& pEnemy : enemies)
    {
        if (!pEnemy->IsAlive())
            continue;
        float const distance = pEnemy->GetDistance(x, y, z);
        if (distance < PB_STEP_BACK_CC_CLEARANCE &&
           (pEnemy->HasBreakableByDamageCrowdControlAura() || IsForbiddenTarget(pEnemy)))
            return false;
        if (distance < PB_STEP_BACK_AGGRO_CLEARANCE && !pEnemy->IsInCombat() &&
           (pEnemy->GetLevel() + 5) > me->GetLevel() && pEnemy->IsValidAttackTarget(me))
            return false;
    }

    // Walkable and no long detour (cliffs, walls).
    PathFinder path(me);
    path.ExcludeSteepSlopes();
    path.calculate(x, y, z);
    if (!(path.getPathType() & PATHFIND_NORMAL))
        return false;
    return path.Length() <= me->GetDistance(x, y, z, SizeFactor::None) * 1.5f + 3.0f;
}

// ---------------------------------------------------------------------------------------------
// F3 kiting (19g). The leader records a one-way path (.partybot kitepath record / save), stored per
// map in kitepath_map<id>.txt in the server folder. A hunter set as kiter (.partybot kiter on):
// TAKE  a boss attacking the group: Distracting Shot and shots until the boss attacks the hunter;
// OUT   runs the path to its end without stopping (Aspect of the Cheetah, instant shots only);
// END   waits there PB_KITE_END_WAIT, then Feign Death (at once if the boss reaches it);
// FEIGN lies still until the boss has turned away and is PB_KITE_BACK_CLEARANCE away;
// BACK  runs the path back, staying PB_KITE_BACK_GAP behind the boss, then fights normally.
// From TAKE to END the other bots leave the boss alone (IsForbiddenTarget); after Feign Death the
// boss is released for the rest of the fight. Feign Death of bots is never resisted (SpellAuras.cpp).
// ---------------------------------------------------------------------------------------------
struct PartyBotKitePoint
{
    float x, y, z;
};
struct PartyBotKiteRecording
{
    uint32 mapId = 0;
    std::vector<PartyBotKitePoint> points;
};
static std::mutex s_kiteMutex;
static std::map<uint32, std::vector<PartyBotKitePoint>> s_kitePaths;   // map id -> path
static std::set<uint32> s_kitePathsLoaded;
static std::map<ObjectGuid, PartyBotKiteRecording> s_kiteRecordings;   // leader -> recording

static std::string GetKitePathFile(uint32 mapId)
{
    return "kitepath_map" + std::to_string(mapId) + ".txt";
}

// Caller holds s_kiteMutex.
static std::vector<PartyBotKitePoint> const& LoadKitePathLocked(uint32 mapId)
{
    if (s_kitePathsLoaded.insert(mapId).second)
    {
        std::ifstream file(GetKitePathFile(mapId));
        PartyBotKitePoint point;
        while (file >> point.x >> point.y >> point.z)
            s_kitePaths[mapId].push_back(point);
    }
    return s_kitePaths[mapId];
}

static std::vector<PartyBotKitePoint> GetKitePath(uint32 mapId)
{
    std::lock_guard<std::mutex> lock(s_kiteMutex);
    return LoadKitePathLocked(mapId);
}

static float GetKitePointDistance(Unit const* pUnit, PartyBotKitePoint const& point)
{
    return pUnit->GetDistance(point.x, point.y, point.z, SizeFactor::None);
}

static char const* GetKitePhaseName(PartyBotKitePhase phase)
{
    switch (phase)
    {
        case PB_KITE_TAKE: return "take";
        case PB_KITE_OUT: return "out";
        case PB_KITE_END: return "end";
        case PB_KITE_FEIGN: return "feign";
        case PB_KITE_BACK: return "back";
        default: return "none";
    }
}

// Every world tick, with or without bots: each recording leader adds a point every few yards.
void PartyBotAI::UpdateKitePathRecordings()
{
    std::lock_guard<std::mutex> lock(s_kiteMutex);
    for (auto& itr : s_kiteRecordings)
    {
        Player* pLeader = ObjectAccessor::FindPlayer(itr.first);
        if (!pLeader || !pLeader->IsInWorld() || pLeader->GetMapId() != itr.second.mapId)
            continue;
        if (GetKitePointDistance(pLeader, itr.second.points.back()) >= PB_KITE_POINT_SPACING)
            itr.second.points.push_back({ pLeader->GetPositionX(), pLeader->GetPositionY(), pLeader->GetPositionZ() });
    }
}

void PartyBotAI::StartKitePathRecording(Player* pLeader)
{
    std::lock_guard<std::mutex> lock(s_kiteMutex);
    PartyBotKiteRecording& recording = s_kiteRecordings[pLeader->GetObjectGuid()];
    recording.mapId = pLeader->GetMapId();
    recording.points.clear();
    recording.points.push_back({ pLeader->GetPositionX(), pLeader->GetPositionY(), pLeader->GetPositionZ() });
    sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Kite] %s: records a kite path on map %u", pLeader->GetName(), recording.mapId);
}

int32 PartyBotAI::SaveKitePathRecording(Player* pLeader)
{
    std::lock_guard<std::mutex> lock(s_kiteMutex);
    auto const itr = s_kiteRecordings.find(pLeader->GetObjectGuid());
    if (itr == s_kiteRecordings.end())
        return -1;

    PartyBotKiteRecording recording = itr->second;
    s_kiteRecordings.erase(itr);
    // The end is where the leader stands now.
    if (pLeader->GetMapId() == recording.mapId &&
        GetKitePointDistance(pLeader, recording.points.back()) >= PB_KITE_POINT_REACHED)
        recording.points.push_back({ pLeader->GetPositionX(), pLeader->GetPositionY(), pLeader->GetPositionZ() });

    std::ofstream file(GetKitePathFile(recording.mapId), std::ios::trunc);
    if (!file)
        return -2;
    file.setf(std::ios::fixed);
    file.precision(2);
    for (auto const& point : recording.points)
        file << point.x << ' ' << point.y << ' ' << point.z << '\n';

    s_kitePaths[recording.mapId] = recording.points;
    s_kitePathsLoaded.insert(recording.mapId);
    sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Kite] %s: kite path saved for map %u (%u points, %s)", pLeader->GetName(),
        recording.mapId, uint32(recording.points.size()), GetKitePathFile(recording.mapId).c_str());
    return int32(recording.points.size());
}

bool PartyBotAI::ClearKitePath(uint32 mapId)
{
    std::lock_guard<std::mutex> lock(s_kiteMutex);
    bool const existed = !LoadKitePathLocked(mapId).empty();
    s_kitePaths[mapId].clear();
    std::remove(GetKitePathFile(mapId).c_str());
    return existed;
}

uint32 PartyBotAI::GetKitePathSize(uint32 mapId)
{
    std::lock_guard<std::mutex> lock(s_kiteMutex);
    return uint32(LoadKitePathLocked(mapId).size());
}

// On (again): bosses kited earlier in this fight may be kited once more.
void PartyBotAI::SetKiter(bool on)
{
    m_kiter = on;
    m_kiteNoPathLogged = false;
    m_kiteReleased.clear();
    sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Kite] %s: kiter %s", me->GetName(), on ? "on" : "off");
    if (!on && m_kitePhase != PB_KITE_NONE)
        EndKite("kiter off");
}

// A boss this kiter would kite: kiter on, alive, a path on this map, the boss not released this
// fight and near the path start (other bosses on the map are fought normally).
bool PartyBotAI::IsKiteCandidate(Unit const* pBoss) const
{
    if (!m_kiter || me->GetClass() != CLASS_HUNTER || !me->IsAlive() || !pBoss || !pBoss->IsAlive() ||
        !pBoss->IsInMap(me) || m_kiteReleased.count(pBoss->GetObjectGuid()) || !IsBossTarget(pBoss))
        return false;
    std::vector<PartyBotKitePoint> const path = GetKitePath(me->GetMapId());
    return path.size() >= 2 && GetKitePointDistance(pBoss, path.front()) <= PB_KITE_CLAIM_RANGE;
}

// .partybot kiter pull: the kiter opens on the boss itself. Returns why not, nullptr when started.
char const* PartyBotAI::StartKitePull(Unit* pBoss)
{
    if (m_kitePhase != PB_KITE_NONE && m_kiteTargetGuid == pBoss->GetObjectGuid())
        return nullptr;
    m_kiteReleased.erase(pBoss->GetObjectGuid());
    if (!IsBossTarget(pBoss))
        return "not a boss";
    if (GetKitePathSize(me->GetMapId()) < 2)
        return "no kite path on this map";
    if (!IsKiteCandidate(pBoss))
        return "boss too far from the kite path start";
    if (m_kitePhase != PB_KITE_NONE)
        EndKite("new kiter pull");

    m_kiteTargetGuid = pBoss->GetObjectGuid();
    m_kiteIndex = -1;
    m_kiteDistracted = false;
    SetKitePhase(PB_KITE_TAKE);
    sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Kite] %s: opens on %s (kiter pull, %.0f yd)", me->GetName(),
        GetLogName(pBoss).c_str(), me->GetDistance(pBoss));
    SayInParty(std::string("Pulling and kiting ") + pBoss->GetName() + ".");
    WakeUp();
    return nullptr;
}

// The group bot that has this boss for its kite: from before the pull (no tank or damage dealer
// touches a boss an armed kiter would kite) until Feign Death releases it.
Player* PartyBotAI::GetKiterOf(Unit const* pEnemy) const
{
    Group* pGroup = me->GetGroup();
    if (!pGroup || !pEnemy)
        return nullptr;
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || !pMember->IsAlive())
            continue;
        PartyBotAI const* pAI = dynamic_cast<PartyBotAI const*>(pMember->AI());
        if (!pAI || !pAI->m_kiter)
            continue;
        if (pAI->m_kiteTargetGuid == pEnemy->GetObjectGuid() &&
           (pAI->m_kitePhase == PB_KITE_TAKE || pAI->m_kitePhase == PB_KITE_OUT || pAI->m_kitePhase == PB_KITE_END))
            return pMember;
        if (pAI->m_kitePhase == PB_KITE_NONE && pAI->IsKiteCandidate(pEnemy))
            return pMember;
    }
    return nullptr;
}

void PartyBotAI::SetKitePhase(PartyBotKitePhase phase)
{
    m_kitePhase = phase;
    m_kitePhaseTime = WorldTimer::getMSTime();
    m_kiteWaitLogged = false;
}

void PartyBotAI::EndKite(char const* why)
{
    Unit* pBoss = me->GetMap()->GetUnit(m_kiteTargetGuid);
    sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Kite] %s: kite over (%s, step %s, boss %s)", me->GetName(), why,
        GetKitePhaseName(m_kitePhase), pBoss ? GetLogName(pBoss).c_str() : "gone");
    if (m_kitePhase == PB_KITE_OUT || m_kitePhase == PB_KITE_BACK)
    {
        me->StopMoving();
        me->GetMotionMaster()->Clear(false, true);
        me->GetMotionMaster()->MoveIdle();
    }
    if (!m_kiteTargetGuid.IsEmpty())
        m_kiteReleased.insert(m_kiteTargetGuid);
    m_kiteTargetGuid.Clear();
    m_kiteIndex = -1;
    SetKitePhase(PB_KITE_NONE);
}

// Every tick (running must not stall between AI updates): path points out and back, waiting
// behind the boss, getting up from Feign Death.
void PartyBotAI::UpdateKiteMovement()
{
    if (m_kitePhase == PB_KITE_NONE)
        return;
    if (!me->IsAlive())
    {
        EndKite("kiter died");
        return;
    }

    Unit* pBoss = me->GetMap()->GetUnit(m_kiteTargetGuid);
    bool const bossThere = pBoss && pBoss->IsAlive() && pBoss->IsInCombat();

    if (m_kitePhase == PB_KITE_FEIGN)
    {
        if (WorldTimer::getMSTimeDiffToNow(m_kitePhaseTime) >= PB_KITE_FEIGN_MIN_TIME &&
           (!me->HasAuraType(SPELL_AURA_FEIGN_DEATH) || !bossThere ||
           (pBoss->GetVictim() != me && !me->IsWithinDist(pBoss, PB_KITE_BACK_CLEARANCE))))
        {
            sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Kite] %s: gets up after %.1f s (boss %s, on %s, %.0f yd), runs back",
                me->GetName(), WorldTimer::getMSTimeDiffToNow(m_kitePhaseTime) / 1000.0f,
                bossThere ? GetLogName(pBoss).c_str() : "gone",
                bossThere && pBoss->GetVictim() ? pBoss->GetVictim()->GetName() : "nobody",
                bossThere ? me->GetDistance(pBoss) : 0.0f);
            me->RemoveSpellsCausingAura(SPELL_AURA_FEIGN_DEATH);
            SetKitePhase(PB_KITE_BACK);
            WakeUp();
        }
        return;
    }

    if ((m_kitePhase != PB_KITE_OUT && m_kitePhase != PB_KITE_BACK) || m_kiteIndex < 0 ||
        me->HasUnitState(UNIT_STATE_CAN_NOT_MOVE | UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL))
        return;

    std::vector<PartyBotKitePoint> const path = GetKitePath(me->GetMapId());
    if (path.size() < 2)
    {
        EndKite("kite path gone");
        return;
    }
    int32 const last = int32(path.size()) - 1;
    m_kiteIndex = std::min(m_kiteIndex, last);

    // Back: never closer than PB_KITE_BACK_GAP to the boss (it walks back the same way).
    if (m_kitePhase == PB_KITE_BACK && pBoss && pBoss->IsAlive() && pBoss->GetVictim() != me && pBoss->IsMoving() &&
        me->IsWithinDist(pBoss, PB_KITE_BACK_GAP))
    {
        if (!m_kiteWaitLogged)
        {
            sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Kite] %s: waits, boss %.0f yd ahead", me->GetName(), me->GetDistance(pBoss));
            m_kiteWaitLogged = true;
        }
        if (me->IsMoving())
        {
            me->StopMoving();
            me->GetMotionMaster()->Clear(false, true);
            me->GetMotionMaster()->MoveIdle();
        }
        return;
    }
    m_kiteWaitLogged = false;

    bool moveOn = me->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE;
    if (GetKitePointDistance(me, path[m_kiteIndex]) < PB_KITE_POINT_REACHED)
    {
        if (m_kitePhase == PB_KITE_OUT && m_kiteIndex == last)
        {
            me->StopMoving();
            me->GetMotionMaster()->Clear(false, true);
            me->GetMotionMaster()->MoveIdle();
            sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Kite] %s: at the path end after %.1f s (boss %.0f yd), Feign Death in %.0f s",
                me->GetName(), WorldTimer::getMSTimeDiffToNow(m_kitePhaseTime) / 1000.0f,
                bossThere ? me->GetDistance(pBoss) : 0.0f, PB_KITE_END_WAIT / 1000.0f);
            SetKitePhase(PB_KITE_END);
            WakeUp();
            return;
        }
        if (m_kitePhase == PB_KITE_BACK && m_kiteIndex == 0)
        {
            EndKite("back at the path start");
            WakeUp();
            return;
        }
        m_kiteIndex += m_kitePhase == PB_KITE_OUT ? 1 : -1;
        moveOn = true;
    }
    if (moveOn)
    {
        PartyBotKitePoint const& point = path[m_kiteIndex];
        me->GetMotionMaster()->MovePoint(0, point.x, point.y, point.z, MOVE_PATHFINDING | MOVE_RUN_MODE);
    }
}

// Returns true while the kite runs (the rest of the AI update is skipped).
bool PartyBotAI::UpdateKite()
{
    if (!m_kiter || me->GetClass() != CLASS_HUNTER)
        return false;

    Unit* pBoss = m_kiteTargetGuid.IsEmpty() ? nullptr : me->GetMap()->GetUnit(m_kiteTargetGuid);
    bool const bossThere = pBoss && pBoss->IsAlive() && pBoss->IsInCombat() &&
        !(pBoss->ToCreature() && pBoss->ToCreature()->IsInEvadeMode());

    if (m_kitePhase == PB_KITE_TAKE && pBoss && pBoss->IsAlive() && !pBoss->IsInCombat())
    {
        // Opening (kiter pull): the boss is not fighting yet.
        if (WorldTimer::getMSTimeDiffToNow(m_kitePhaseTime) > PB_KITE_OPEN_TIMEOUT)
        {
            EndKite("could not open on the boss in time");
            return false;
        }
    }
    else if (m_kitePhase == PB_KITE_TAKE || m_kitePhase == PB_KITE_OUT || m_kitePhase == PB_KITE_END)
    {
        if (!bossThere)
        {
            EndKite("boss dead, reset or fight over");
            return false;
        }
        if (m_orderOverridesCC && m_orderedTargetGuid == m_kiteTargetGuid)
        {
            EndKite("attack order on the boss");
            return false;
        }
    }

    // Start: a boss attacking a group member.
    if (m_kitePhase == PB_KITE_NONE)
    {
        Group* pGroup = me->GetGroup();
        if (!pGroup)
            return false;
        pBoss = nullptr;
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr && !pBoss; itr = itr->next())
        {
            Player* pMember = itr->getSource();
            if (!pMember || !pMember->IsInMap(me))
                continue;
            for (auto const pAttacker : pMember->GetAttackers())
                if (IsKiteCandidate(pAttacker))
                {
                    pBoss = pAttacker;
                    break;
                }
        }
        if (!pBoss)
            return false;

        uint32 const points = GetKitePathSize(me->GetMapId());
        if (points < 2)
        {
            if (!m_kiteNoPathLogged)
            {
                sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Kite] %s: no kite path for map %u, fights %s normally",
                    me->GetName(), me->GetMapId(), GetLogName(pBoss).c_str());
                SayInParty("No kite path recorded here - I fight normally.");
                m_kiteNoPathLogged = true;
            }
            return false;
        }

        m_kiteTargetGuid = pBoss->GetObjectGuid();
        m_kiteIndex = -1;
        m_kiteDistracted = false;
        SetKitePhase(PB_KITE_TAKE);
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Kite] %s: takes %s (on %s, %.0f yd; path %u points)", me->GetName(),
            GetLogName(pBoss).c_str(), pBoss->GetVictim() ? pBoss->GetVictim()->GetName() : "nobody",
            me->GetDistance(pBoss), points);
        SayInParty(std::string("Kiting ") + pBoss->GetName() + ".");
    }

    if (m_kitePhase == PB_KITE_FEIGN)
        return true; // lying still (UpdateKiteMovement gets up)

    if (m_kitePhase == PB_KITE_BACK)
    {
        // Feigned before running out: not on the path, nothing to run back.
        if (m_kiteIndex < 0)
        {
            EndKite("feigned before running out");
            return false;
        }
        // Running back (UpdateKiteMovement); no shots at the boss on the way.
        if (m_spells.hunter.pAspectOfTheCheetah && !me->HasAura(m_spells.hunter.pAspectOfTheCheetah->Id) &&
            CanTryToCastSpell(me, m_spells.hunter.pAspectOfTheCheetah))
            DoCastSpell(me, m_spells.hunter.pAspectOfTheCheetah);
        return true;
    }

    // Attack state only once the boss fights (attacking first would wake it before the opener).
    if (pBoss->IsInCombat() && me->GetVictim() != pBoss)
    {
        if (me->GetVictim())
            me->AttackStop(true);
        me->Attack(pBoss, false);
    }

    bool const onMe = pBoss->GetVictim() == me;
    bool const caught = onMe && pBoss->CanReachWithMeleeAutoAttack(me);

    // Feign Death: at the end after PB_KITE_END_WAIT or once the boss reaches the kiter; on the
    // way out only when caught and hurt.
    bool const feignNow = (m_kitePhase == PB_KITE_END &&
        (caught || WorldTimer::getMSTimeDiffToNow(m_kitePhaseTime) >= PB_KITE_END_WAIT)) ||
        (caught && me->GetHealthPercent() < PB_KITE_FEIGN_HEALTH_PCT);
    if (feignNow && m_spells.hunter.pFeignDeath)
    {
        if (CanTryToCastSpell(me, m_spells.hunter.pFeignDeath) &&
            DoCastSpell(me, m_spells.hunter.pFeignDeath) == SPELL_CAST_OK)
        {
            sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Kite] %s: Feign Death at step %s (boss %.0f yd%s, health %.0f%%), boss released",
                me->GetName(), GetKitePhaseName(m_kitePhase), me->GetDistance(pBoss), caught ? ", caught" : "",
                me->GetHealthPercent());
            me->StopMoving();
            me->GetMotionMaster()->Clear(false, true);
            me->GetMotionMaster()->MoveIdle();
            m_kiteReleased.insert(m_kiteTargetGuid);
            SetKitePhase(PB_KITE_FEIGN);
            return true;
        }
        if (m_kitePhase == PB_KITE_END && !m_kiteWaitLogged)
        {
            sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Kite] %s: Feign Death not possible (%s)", me->GetName(),
                me->IsSpellReady(m_spells.hunter.pFeignDeath) ? "cast failed" : "on cooldown");
            m_kiteWaitLogged = true;
        }
    }

    // Faster than the boss: Aspect of the Cheetah once running (not while taking the boss: a hit
    // dazes a Cheetah hunter).
    if (m_kitePhase == PB_KITE_OUT &&
        m_spells.hunter.pAspectOfTheCheetah && !me->HasAura(m_spells.hunter.pAspectOfTheCheetah->Id) &&
        CanTryToCastSpell(me, m_spells.hunter.pAspectOfTheCheetah) &&
        DoCastSpell(me, m_spells.hunter.pAspectOfTheCheetah) == SPELL_CAST_OK)
        return true;

    if (m_kitePhase == PB_KITE_END)
    {
        me->SetFacingToObject(pBoss);
        return true;
    }

    if (m_kitePhase == PB_KITE_OUT)
    {
        // Running without stopping (UpdateKiteMovement); instant shots only.
        if (m_spells.hunter.pArcaneShot && CanTryToCastSpell(pBoss, m_spells.hunter.pArcaneShot))
            DoCastSpell(pBoss, m_spells.hunter.pArcaneShot);
        return true;
    }

    // Never cut the own cast (Aimed Shot opener).
    if (me->IsNonMeleeSpellCasted(false, false, true))
        return true;

    // No Cheetah while taking the boss (a guard hit would daze the hunter before it runs).
    if (m_spells.hunter.pAspectOfTheCheetah && me->HasAura(m_spells.hunter.pAspectOfTheCheetah->Id))
        me->RemoveAurasDueToSpell(m_spells.hunter.pAspectOfTheCheetah->Id);

    // Take: the boss attacks the hunter and is close -> run out from the nearest path point. Until
    // then the guards (faster) reach the group first and the tanks taunt them off the hunter.
    if (onMe && me->GetCombatDistance(pBoss) < PB_KITE_RUN_START)
    {
        std::vector<PartyBotKitePoint> const path = GetKitePath(me->GetMapId());
        int32 nearest = 0;
        for (int32 i = 1; i < int32(path.size()); ++i)
            if (GetKitePointDistance(me, path[i]) < GetKitePointDistance(me, path[nearest]))
                nearest = i;
        m_kiteIndex = nearest;
        if (me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
            me->InterruptSpell(CURRENT_AUTOREPEAT_SPELL, true);
        if (me->GetCurrentSpell(CURRENT_GENERIC_SPELL))
            me->InterruptSpell(CURRENT_GENERIC_SPELL);
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Kite] %s: boss on me after %.1f s, runs out from point %i of %u (boss %.0f yd)",
            me->GetName(), WorldTimer::getMSTimeDiffToNow(m_kitePhaseTime) / 1000.0f, nearest, uint32(path.size()),
            me->GetDistance(pBoss));
        SetKitePhase(PB_KITE_OUT);
        UpdateKiteMovement();
        return true;
    }

    // Take: into shooting range, Distracting Shot, then shots.
    me->SetFacingToObject(pBoss);
    SpellEntry const* pShot = GetRangedWeaponSpell();
    float const range = pShot ? Spells::GetSpellMaxRange(sSpellRangeStore.LookupEntry(pShot->rangeIndex)) : 30.0f;
    if (WalkIntoRange(pBoss, range, "Kite", "shots"))
        return true;

    // Opener on a boss not fighting yet: Aimed Shot (3 s cast; Arcane Shot without it).
    if (!pBoss->IsInCombat())
    {
        SpellEntry const* pOpener = m_spells.hunter.pAimedShot && CanTryToCastSpell(pBoss, m_spells.hunter.pAimedShot) ?
            m_spells.hunter.pAimedShot : m_spells.hunter.pArcaneShot;
        if (pOpener && CanTryToCastSpell(pBoss, pOpener) && DoCastSpell(pBoss, pOpener) == SPELL_CAST_OK)
        {
            sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Kite] %s: opens with %s on %s (%.0f yd)", me->GetName(),
                pOpener->SpellName[0].c_str(), GetLogName(pBoss).c_str(), me->GetDistance(pBoss));
            return true;
        }
    }

    // Distracting Shot right after the opener, then whenever the boss is on someone else.
    if ((!m_kiteDistracted || !onMe) && m_spells.hunter.pDistractingShot &&
        CanTryToCastSpell(pBoss, m_spells.hunter.pDistractingShot) &&
        DoCastSpell(pBoss, m_spells.hunter.pDistractingShot) == SPELL_CAST_OK)
    {
        m_kiteDistracted = true;
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Kite] %s: Distracting Shot on %s (on %s, %.0f yd)", me->GetName(),
            GetLogName(pBoss).c_str(), pBoss->GetVictim() ? pBoss->GetVictim()->GetName() : "nobody", me->GetDistance(pBoss));
        return true;
    }
    if (m_spells.hunter.pArcaneShot && CanTryToCastSpell(pBoss, m_spells.hunter.pArcaneShot) &&
        DoCastSpell(pBoss, m_spells.hunter.pArcaneShot) == SPELL_CAST_OK)
        return true;
    if (!me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
        ShootRangedWeapon(pBoss);
    return true;
}

bool PartyBotAI::HunterStepBack(Unit* pVictim)
{
    if (m_steppingBack && !IsSteppingBack())
        m_steppingBack = false;

    if (m_role != ROLE_RANGE_DPS || !CanMoveOnOwn() || IsPullWaiting() || m_steppingBack ||
        me->GetCombatDistance(pVictim) >= PB_STEP_BACK_TRIGGER || pVictim->GetVictim() == me ||
        (m_stepBackTime && WorldTimer::getMSTimeDiffToNow(m_stepBackTime) < PB_STEP_BACK_INTERVAL))
        return false;

    // An enemy that attacks the hunter would only follow: stock melee until the tank takes it.
    for (auto const& pAttacker : me->GetAttackers())
        if (me->GetCombatDistance(pAttacker) < PB_STEP_BACK_TRIGGER)
            return false;

    m_stepBackTime = WorldTimer::getMSTime();

    std::list<Unit*> enemies;
    me->GetEnemyListInRadiusAround(me, PB_STEP_BACK_DISTANCE + PB_STEP_BACK_AGGRO_CLEARANCE + 10.0f, enemies);

    // Away from the target, towards the leader (straight away when the leader stands at the target);
    // then up to 60 degrees to either side.
    Player* pLeader = GetPartyLeader();
    float const baseAngle = pLeader && !pLeader->IsWithinDist(pVictim, PB_STEP_BACK_TRIGGER) ?
        pVictim->GetAngle(pLeader) : pVictim->GetAngle(me);
    static float const offsets[] = { 0.0f, M_PI_F / 6, -M_PI_F / 6, M_PI_F / 3, -M_PI_F / 3 };
    for (float offset : offsets)
    {
        float const angle = baseAngle + offset;
        float x = pVictim->GetPositionX() + PB_STEP_BACK_DISTANCE * cos(angle);
        float y = pVictim->GetPositionY() + PB_STEP_BACK_DISTANCE * sin(angle);
        float z = me->GetPositionZ();
        me->UpdateAllowedPositionZ(x, y, z);
        if (!IsSafeStepBackSpot(pVictim, x, y, z, enemies))
            continue;

        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Move] %s: steps back from %s (%.0f yd, on %s) -> %.0f yd",
            me->GetName(), GetLogName(pVictim).c_str(), me->GetCombatDistance(pVictim),
            pVictim->GetVictim() ? GetLogName(pVictim->GetVictim()).c_str() : "nobody", PB_STEP_BACK_DISTANCE);
        m_steppingBack = true;
        m_stepBackFailGuid.Clear();
        if (me->GetCurrentSpell(CURRENT_GENERIC_SPELL))
            me->InterruptSpell(CURRENT_GENERIC_SPELL);
        me->GetMotionMaster()->MovePoint(0, x, y, z, MOVE_PATHFINDING);
        return true;
    }

    if (m_stepBackFailGuid != pVictim->GetObjectGuid())
    {
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Move] %s: no safe spot to step back from %s, stays",
            me->GetName(), GetLogName(pVictim).c_str());
        m_stepBackFailGuid = pVictim->GetObjectGuid();
    }
    return false;
}

void PartyBotAI::UpdateInCombatAI_Hunter()
{
    MaintainHunterPet();

    if (Unit* pVictim = me->GetVictim())
    {
        CommandPetAttack(pVictim);

        // Walking away: instant shots only (CanTryToCastSpell), no melee strikes.
        if (HunterStepBack(pVictim) || IsSteppingBack())
        {
            if (m_spells.hunter.pArcaneShot && CanTryToCastSpell(pVictim, m_spells.hunter.pArcaneShot))
                DoCastSpell(pVictim, m_spells.hunter.pArcaneShot);
            return;
        }

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

    // No Bloodrage out of combat (B13): it puts the warrior in combat, healers heal it and the group
    // counts as fighting. Without rage the shout waits for the fight (UpdateInCombatAI_Warrior).
    if (m_spells.warrior.pBattleShout &&
       !me->HasAura(m_spells.warrior.pBattleShout->Id) &&
        CanTryToCastSpell(me, m_spells.warrior.pBattleShout))
        DoCastSpell(me, m_spells.warrior.pBattleShout);

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
        // Tank short of rage in a real fight: Bloodrage for early threat (B13).
        if (m_role == ROLE_TANK && m_spells.warrior.pBloodrage && me->IsInCombat() &&
            me->GetPower(POWER_RAGE) < PB_BLOODRAGE_MAX_RAGE &&
            me->GetHealthPercent() > PB_BLOODRAGE_MIN_HEALTH_PCT &&
            CanTryToCastSpell(me, m_spells.warrior.pBloodrage) &&
            DoCastSpell(me, m_spells.warrior.pBloodrage) == SPELL_CAST_OK)
        {
            sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Tank] %s: Bloodrage (rage %u, health %.0f%%)",
                me->GetName(), me->GetPower(POWER_RAGE) / 10, me->GetHealthPercent());
            return;
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
                        me->GetName(), GetLogName(pVictim).c_str(), me->GetPower(POWER_RAGE) / 10);
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
                        me->GetName(), m_spells.warrior.pSunderArmor->Id, GetLogName(pVictim).c_str(), stacks + 1, maxStacks,
                        me->GetPower(POWER_RAGE) / 10);
                else
                    sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Tank] %s: Sunder Armor (%u) on %s, refresh %u/%u (%.1f s left, rage %u)",
                        me->GetName(), m_spells.warrior.pSunderArmor->Id, GetLogName(pVictim).c_str(), stacks, maxStacks, remaining / 1000.0f,
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

        // Battle Shout in the fight (B13: out of combat there is often no rage for it).
        if (m_spells.warrior.pBattleShout &&
           !me->HasAura(m_spells.warrior.pBattleShout->Id) &&
            CanTryToCastSpell(me, m_spells.warrior.pBattleShout))
        {
            if (DoCastSpell(me, m_spells.warrior.pBattleShout) == SPELL_CAST_OK)
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

        // Direct heal: Swiftmend first (instant) when the target has a druid's Rejuvenation or
        // Regrowth (same check as the core's Swiftmend script).
        if (Unit* pTarget = SelectHealTarget(60.0f, 70.0f))
        {
            if (m_spells.druid.pSwiftmend &&
                pTarget->GetAura(SPELL_AURA_PERIODIC_HEAL, SPELLFAMILY_DRUID, uint64(0x50)) &&
                CanTryToCastSpell(pTarget, m_spells.druid.pSwiftmend) &&
                DoCastSpell(pTarget, m_spells.druid.pSwiftmend) == SPELL_CAST_OK)
                return;

            if (HealInjuredTargetDirect(pTarget))
                return;
        }

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
