#include "World.h"
#include "CombatBotBaseAI.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Bag.h"
#include "TradeData.h"
#include "Group.h"
#include "PlayerBotMgr.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "Chat.h"
#include "CharacterDatabaseCache.h"
#include "Utilities/Random.h"

#include <random>
#include <unordered_set>

enum CombatBotSpells
{
    SPELL_MAIL_PROFICIENCY = 8737,
    SPELL_PLATE_PROFICIENCY = 750,

    SPELL_SHIELD_SLAM = 23922,
    SPELL_HOLY_SHIELD = 20925,
    SPELL_SANCTITY_AURA = 20218,
    SPELL_SHADOWFORM = 15473,
    SPELL_ELEMENTAL_MASTERY = 16166,
    SPELL_STORMSTRIKE = 17364,
    SPELL_MOONKIN_FORM = 24858,
    SPELL_LEADER_OF_THE_PACK = 17007,

    SPELL_SUMMON_IMP = 688,
    SPELL_SUMMON_VOIDWALKER = 697,
    SPELL_SUMMON_FELHUNTER = 691,
    SPELL_SUMMON_SUCCUBUS = 712,
    SPELL_TAME_BEAST = 13481,
    SPELL_REVIVE_PET = 982,
    SPELL_CALL_PET = 883,

    PET_WOLF    = 565,
    PET_CAT     = 681,
    PET_BEAR    = 822,
    PET_CRAB    = 831,
    PET_GORILLA = 1108,
    PET_BIRD    = 1109,
    PET_BOAR    = 1190,
    PET_BAT     = 1554,
    PET_CROC    = 1693,
    PET_SPIDER  = 1781,
    PET_OWL     = 1997,
    PET_STRIDER = 2322,
    PET_SCORPID = 3127,
    PET_SERPENT = 3247,
    PET_RAPTOR  = 3254,
    PET_TURTLE  = 3461,
    PET_HYENA   = 4127,
};

void CombatBotBaseAI::AutoAssignRole()
{
    switch (me->GetClass())
    {
        case CLASS_WARRIOR:
        {
            if (me->HasSpell(SPELL_SHIELD_SLAM))
                m_role = ROLE_TANK;
            else
                m_role = ROLE_MELEE_DPS;
            return;
        }
        case CLASS_ROGUE:
        {
            m_role = ROLE_MELEE_DPS;
            return;
        }
        case CLASS_HUNTER:
        case CLASS_MAGE:
        case CLASS_WARLOCK:
        {
            m_role = ROLE_RANGE_DPS;
            return;
        }
        case CLASS_PALADIN:
        {
            if (me->HasSpell(SPELL_HOLY_SHIELD))
                m_role = ROLE_TANK;
            else if (me->HasSpell(SPELL_SANCTITY_AURA))
                m_role = ROLE_MELEE_DPS;
            else
                m_role = ROLE_HEALER;
            return;
        }
        case CLASS_PRIEST:
        {
            if (me->HasSpell(SPELL_SHADOWFORM))
                m_role = ROLE_RANGE_DPS;
            else
                m_role = ROLE_HEALER;
            return;
        }
        case CLASS_SHAMAN:
        {
            if (me->HasSpell(SPELL_ELEMENTAL_MASTERY))
                m_role = ROLE_RANGE_DPS;
            else if (me->HasSpell(SPELL_STORMSTRIKE))
                m_role = ROLE_MELEE_DPS;
            else
                m_role = ROLE_HEALER;
            return;
        }
        case CLASS_DRUID:
        {
            if (me->HasSpell(SPELL_MOONKIN_FORM))
                m_role = ROLE_RANGE_DPS;
            else if (me->HasSpell(SPELL_LEADER_OF_THE_PACK))
                m_role = ROLE_MELEE_DPS;
            else
                m_role = ROLE_HEALER;
            return;
        }
    }

    m_role = ROLE_MELEE_DPS;
}

void CombatBotBaseAI::ResetSpellData()
{
    for (auto& ptr : m_spells.raw.spells)
        ptr = nullptr;

    m_resurrectionSpell = nullptr;
    m_spellListDirectHeal.clear();
    m_spellListPeriodicHeal.clear();
    m_spellListTaunt.clear();
}

void CombatBotBaseAI::PopulateSpellData()
{
    // Paladin Seals
    SpellEntry const* pSealOfRighteousness = nullptr;
    SpellEntry const* pSealOfCommand = nullptr;
    SpellEntry const* pSealOfFury = nullptr;

    // Paladin Blessings
    SpellEntry const* pBlessingOfLight = nullptr;
    SpellEntry const* pBlessingOfMight = nullptr;
    SpellEntry const* pBlessingOfWisdom = nullptr;
    SpellEntry const* pBlessingOfKings = nullptr;
    SpellEntry const* pBlessingOfSanctuary = nullptr;

    // Paladin Auras
    SpellEntry const* pDevotionAura = nullptr;
    SpellEntry const* pConcentrationAura = nullptr;
    SpellEntry const* pRetributionAura = nullptr;
    SpellEntry const* pSanctityAura = nullptr;
    SpellEntry const* pShadowResistanceAura = nullptr;
    SpellEntry const* pFrostResistanceAura = nullptr;
    SpellEntry const* pFireResistanceAura = nullptr;

    // Air Totems
    SpellEntry const* pGraceOfAirTotem = nullptr;
    SpellEntry const* pNatureResistanceTotem = nullptr;
    SpellEntry const* pWindfuryTotem = nullptr;
    SpellEntry const* pWindwallTotem = nullptr;
    SpellEntry const* pTranquilAirTotem = nullptr;

    // Earth Totems
    SpellEntry const* pEarthbindTotem = nullptr;
    SpellEntry const* pStoneclawtotem = nullptr;
    SpellEntry const* pStoneskinTotem = nullptr;
    SpellEntry const* pStrengthOfEarthTotem = nullptr;
    SpellEntry const* pTremorTotem = nullptr;

    // Fire Totems
    SpellEntry const* pFireNovaTotem = nullptr;
    SpellEntry const* pMagmaTotem = nullptr;
    SpellEntry const* pSearingTotem = nullptr;
    SpellEntry const* pFlametongueTotem = nullptr;
    SpellEntry const* pFrostResistanceTotem = nullptr;

    // Water Totems
    SpellEntry const* pFireResistanceTotem = nullptr;
    SpellEntry const* pDiseaseCleansingTotem = nullptr;
    SpellEntry const* pHealingStreamTotem = nullptr;
    SpellEntry const* pManaSpringTotem = nullptr;
    SpellEntry const* pPoisonCleansingTotem = nullptr;

    // Shaman Weapon Buffs
    SpellEntry const* pFrostbrandWeapon = nullptr;
    SpellEntry const* pRockbiterWeapon = nullptr;
    SpellEntry const* pWindfuryWeapon = nullptr;

    // Mage Polymorph
    SpellEntry const* pPolymorphSheep = nullptr;
    SpellEntry const* pPolymorphCow = nullptr;
    SpellEntry const* pPolymorphPig = nullptr;
    SpellEntry const* pPolymorphTurtle = nullptr;

    // Mage Frost Armor (to replace ice armor at low level)
    SpellEntry const* pFrostArmor = nullptr;

    bool hasDeadlyPoison = false;
    bool hasInstantPoison = false;
    bool hasCripplingPoison = false;
    bool hasWoundPoison = false;
    bool HasMindNumbingPoison = false;

    for (const auto& spell : me->GetSpellMap())
    {
        if (spell.second.disabled)
            continue;

        if (spell.second.state == PLAYERSPELL_REMOVED)
            continue;

        SpellEntry const* pSpellEntry = sSpellMgr.GetSpellEntry(spell.first);
        if (!pSpellEntry)
            continue;

        if (pSpellEntry->HasAttribute(SPELL_ATTR_PASSIVE))
            continue;

        if (pSpellEntry->HasAttribute(SPELL_ATTR_DO_NOT_DISPLAY))
            continue;

        // Trainer "teach" entries share the real spell's name, have no rank and a higher id, so
        // the rank pick below would choose them; casting one only re-learns the spell.
        if (pSpellEntry->HasEffect(SPELL_EFFECT_LEARN_SPELL))
            continue;

        auto IsHigherRankSpell = [pSpellEntry](SpellEntry const* pOldSpell)
        {
            if (!pOldSpell)
                return true;

            uint32 newRank = pSpellEntry->GetRank();
            if (newRank)
                return newRank > pOldSpell->GetRank();

            return pSpellEntry->Id > pOldSpell->Id;
        };

        switch (me->GetClass())
        {
            case CLASS_PALADIN:
            {
                if (pSpellEntry->SpellName[0].find("Seal of Righteousness") != std::string::npos)
                {
                    if (IsHigherRankSpell(pSealOfRighteousness))
                        pSealOfRighteousness = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Seal of Command") != std::string::npos)
                {
                    if (IsHigherRankSpell(pSealOfCommand))
                        pSealOfCommand = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Judgement") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.paladin.pJudgement))
                        m_spells.paladin.pJudgement = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Hammer of Justice") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.paladin.pHammerOfJustice))
                        m_spells.paladin.pHammerOfJustice = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Blessing of Sacrifice") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.paladin.pBlessingOfSacrifice))
                        m_spells.paladin.pBlessingOfSacrifice = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Blessing of Freedom") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.paladin.pBlessingOfFreedom))
                        m_spells.paladin.pBlessingOfFreedom = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Blessing of Protection") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.paladin.pBlessingOfProtection))
                        m_spells.paladin.pBlessingOfProtection = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Blessing of Sanctuary") != std::string::npos)
                {
                    if (IsHigherRankSpell(pBlessingOfSanctuary))
                        pBlessingOfSanctuary = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Blessing of Kings") != std::string::npos)
                {
                    if (IsHigherRankSpell(pBlessingOfKings))
                        pBlessingOfKings = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Blessing of Wisdom") != std::string::npos)
                {
                    if (IsHigherRankSpell(pBlessingOfWisdom))
                        pBlessingOfWisdom = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Blessing of Might") != std::string::npos)
                {
                    if (IsHigherRankSpell(pBlessingOfMight))
                        pBlessingOfMight = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Blessing of Light") != std::string::npos)
                {
                    if (IsHigherRankSpell(pBlessingOfLight))
                        pBlessingOfLight = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Devotion Aura") != std::string::npos)
                {
                    if (IsHigherRankSpell(pDevotionAura))
                        pDevotionAura = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Retribution Aura") != std::string::npos)
                {
                    if (IsHigherRankSpell(pRetributionAura))
                        pRetributionAura = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Concentration Aura") != std::string::npos)
                {
                    if (IsHigherRankSpell(pConcentrationAura))
                        pConcentrationAura = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Sanctity Aura") != std::string::npos)
                {
                    if (IsHigherRankSpell(pSanctityAura))
                        pSanctityAura = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Shadow Resistance Aura") != std::string::npos)
                {
                    if (IsHigherRankSpell(pShadowResistanceAura))
                        pShadowResistanceAura = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Frost Resistance Aura") != std::string::npos)
                {
                    if (IsHigherRankSpell(pFrostResistanceAura))
                        pFrostResistanceAura = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Fire Resistance Aura") != std::string::npos)
                {
                    if (IsHigherRankSpell(pFireResistanceAura))
                        pFireResistanceAura = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Exorcism") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.paladin.pExorcism))
                        m_spells.paladin.pExorcism = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Consecration") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.paladin.pConsecration))
                        m_spells.paladin.pConsecration = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Hammer of Wrath") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.paladin.pHammerOfWrath))
                        m_spells.paladin.pHammerOfWrath = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Cleanse") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.paladin.pCleanse))
                        m_spells.paladin.pCleanse = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Divine Shield") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.paladin.pDivineShield))
                        m_spells.paladin.pDivineShield = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Lay on Hands") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.paladin.pLayOnHands))
                        m_spells.paladin.pLayOnHands = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Righteous Fury") != std::string::npos) // post 1.9
                {
                    if (IsHigherRankSpell(m_spells.paladin.pRighteousFury))
                        m_spells.paladin.pRighteousFury = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Seal of Fury") != std::string::npos) // pre 1.9
                {
                    if (IsHigherRankSpell(pSealOfFury))
                        pSealOfFury = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Holy Shock") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.paladin.pHolyShock))
                        m_spells.paladin.pHolyShock = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Divine Favor") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.paladin.pDivineFavor))
                        m_spells.paladin.pDivineFavor = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Holy Wrath") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.paladin.pHolyWrath))
                        m_spells.paladin.pHolyWrath = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Turn Undead") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.paladin.pTurnUndead))
                        m_spells.paladin.pTurnUndead = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Holy Shield") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.paladin.pHolyShield))
                        m_spells.paladin.pHolyShield = pSpellEntry;
                }
                break;
            }
            case CLASS_SHAMAN:
            {
                if (pSpellEntry->SpellName[0].find("Lightning Bolt") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.shaman.pLightningBolt))
                        m_spells.shaman.pLightningBolt = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Chain Lightning") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.shaman.pChainLightning))
                        m_spells.shaman.pChainLightning = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Earth Shock") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.shaman.pEarthShock))
                        m_spells.shaman.pEarthShock = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Flame Shock") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.shaman.pFlameShock))
                        m_spells.shaman.pFlameShock = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Frost Shock") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.shaman.pFrostShock))
                        m_spells.shaman.pFrostShock = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Purge") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.shaman.pPurge))
                        m_spells.shaman.pPurge = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Stormstrike") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.shaman.pStormstrike))
                        m_spells.shaman.pStormstrike = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Elemental Mastery") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.shaman.pElementalMastery))
                        m_spells.shaman.pElementalMastery = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Lightning Shield") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.shaman.pLightningShield))
                        m_spells.shaman.pLightningShield = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Ghost Wolf") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.shaman.pGhostWolf))
                        m_spells.shaman.pGhostWolf = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Frostbrand Weapon") != std::string::npos)
                {
                    if (IsHigherRankSpell(pFrostbrandWeapon))
                        pFrostbrandWeapon = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Rockbiter Weapon") != std::string::npos)
                {
                    if (IsHigherRankSpell(pRockbiterWeapon))
                        pRockbiterWeapon = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Windfury Weapon") != std::string::npos)
                {
                    if (IsHigherRankSpell(pWindfuryWeapon))
                        pWindfuryWeapon = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Grace of Air Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(pGraceOfAirTotem))
                        pGraceOfAirTotem = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Nature Resistance Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(pNatureResistanceTotem))
                        pNatureResistanceTotem = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Windfury Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(pWindfuryTotem))
                        pWindfuryTotem = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Windwall Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(pWindwallTotem))
                        pWindwallTotem = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Tranquil Air Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(pTranquilAirTotem))
                        pTranquilAirTotem = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Earthbind Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(pEarthbindTotem))
                        pEarthbindTotem = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Stoneclaw Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(pStoneclawtotem))
                        pStoneclawtotem = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Stoneskin Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(pStoneskinTotem))
                        pStoneskinTotem = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Strength of Earth Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(pStrengthOfEarthTotem))
                        pStrengthOfEarthTotem = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Tremor Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(pTremorTotem))
                        pTremorTotem = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Fire Nova Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(pFireNovaTotem))
                        pFireNovaTotem = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Magma Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(pMagmaTotem))
                        pMagmaTotem = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Searing Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(pSearingTotem))
                        pSearingTotem = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Flametongue Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(pFlametongueTotem))
                        pFlametongueTotem = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Frost Resistance Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(pFrostResistanceTotem))
                        pFrostResistanceTotem = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Fire Resistance Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(pFireResistanceTotem))
                        pFireResistanceTotem = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Disease Resistance Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(pDiseaseCleansingTotem))
                        pDiseaseCleansingTotem = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Healing Stream Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(pHealingStreamTotem))
                        pHealingStreamTotem = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Mana Spring Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(pManaSpringTotem))
                        pManaSpringTotem = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Poison Cleansing Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(pPoisonCleansingTotem))
                        pPoisonCleansingTotem = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Mana Tide Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.shaman.pManaTideTotem))
                        m_spells.shaman.pManaTideTotem = pSpellEntry;
                }
                break;
            }
            case CLASS_HUNTER:
            {
                if (pSpellEntry->SpellName[0].find("Aspect of the Cheetah") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.hunter.pAspectOfTheCheetah))
                        m_spells.hunter.pAspectOfTheCheetah = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Aspect of the Hawk") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.hunter.pAspectOfTheHawk))
                        m_spells.hunter.pAspectOfTheHawk = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Trueshot Aura") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.hunter.pTrueshotAura))
                        m_spells.hunter.pTrueshotAura = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Aspect of the Monkey") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.hunter.pAspectOfTheMonkey))
                        m_spells.hunter.pAspectOfTheMonkey = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Serpent Sting") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.hunter.pSerpentSting))
                        m_spells.hunter.pSerpentSting = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Arcane Shot") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.hunter.pArcaneShot))
                        m_spells.hunter.pArcaneShot = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Aimed Shot") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.hunter.pAimedShot))
                        m_spells.hunter.pAimedShot = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Multi-Shot") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.hunter.pMultiShot))
                        m_spells.hunter.pMultiShot = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Concussive Shot") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.hunter.pConcussiveShot))
                        m_spells.hunter.pConcussiveShot = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Wing Clip") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.hunter.pWingClip))
                        m_spells.hunter.pWingClip = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Hunter's Mark") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.hunter.pHuntersMark))
                        m_spells.hunter.pHuntersMark = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Mongoose Bite") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.hunter.pMongooseBite))
                        m_spells.hunter.pMongooseBite = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Raptor Strike") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.hunter.pRaptorStrike))
                        m_spells.hunter.pRaptorStrike = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Disengage") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.hunter.pDisengage))
                        m_spells.hunter.pDisengage = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Distracting Shot") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.hunter.pDistractingShot))
                        m_spells.hunter.pDistractingShot = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Feign Death") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.hunter.pFeignDeath))
                        m_spells.hunter.pFeignDeath = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Scare Beast") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.hunter.pScareBeast))
                        m_spells.hunter.pScareBeast = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Volley") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.hunter.pVolley))
                        m_spells.hunter.pVolley = pSpellEntry;
                }
                break;
            }
            case CLASS_MAGE:
            {
                if (pSpellEntry->SpellName[0].find("Ice Armor") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pIceArmor))
                        m_spells.mage.pIceArmor = pSpellEntry;
                }
                if (pSpellEntry->SpellName[0].find("Frost Armor") != std::string::npos)
                {
                    if (IsHigherRankSpell(pFrostArmor))
                        pFrostArmor = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Ice Barrier") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pIceBarrier))
                        m_spells.mage.pIceBarrier = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Mana Shield") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pManaShield))
                        m_spells.mage.pManaShield = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Arcane Intellect") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pArcaneIntellect))
                        m_spells.mage.pArcaneIntellect = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Arcane Brilliance") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pArcaneBrilliance))
                        m_spells.mage.pArcaneBrilliance = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Frostbolt") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pFrostbolt))
                        m_spells.mage.pFrostbolt = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Fire Blast") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pFireBlast))
                        m_spells.mage.pFireBlast = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Fireball") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pFireball))
                        m_spells.mage.pFireball = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Arcane Explosion") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pArcaneExplosion))
                        m_spells.mage.pArcaneExplosion = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Frost Nova") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pFrostNova))
                        m_spells.mage.pFrostNova = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Cone of Cold") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pConeofCold))
                        m_spells.mage.pConeofCold = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Blink") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pBlink))
                        m_spells.mage.pBlink = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0] == "Polymorph") // Sheep
                {
                    if (IsHigherRankSpell(pPolymorphSheep))
                        pPolymorphSheep = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Polymorph: Cow") != std::string::npos)
                {
                    if (IsHigherRankSpell(pPolymorphCow))
                        pPolymorphCow = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Polymorph: Pig") != std::string::npos)
                {
                    if (IsHigherRankSpell(pPolymorphPig))
                        pPolymorphPig = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Polymorph: Turtle") != std::string::npos)
                {
                    if (IsHigherRankSpell(pPolymorphTurtle))
                        pPolymorphTurtle = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Counterspell") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pCounterspell))
                        m_spells.mage.pCounterspell = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Presence of Mind") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pPresenceOfMind))
                        m_spells.mage.pPresenceOfMind = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Arcane Power") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pArcanePower))
                        m_spells.mage.pArcanePower = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Remove Lesser Curse") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pRemoveLesserCurse))
                        m_spells.mage.pRemoveLesserCurse = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Scorch") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pScorch))
                        m_spells.mage.pScorch = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Pyroblast") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pPyroblast))
                        m_spells.mage.pPyroblast = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Evocation") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pEvocation))
                        m_spells.mage.pEvocation = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Ice Block") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pIceBlock))
                        m_spells.mage.pIceBlock = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Blizzard") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pBlizzard))
                        m_spells.mage.pBlizzard = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Flamestrike") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pFlamestrike))
                        m_spells.mage.pFlamestrike = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Blast Wave") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pBlastWave))
                        m_spells.mage.pBlastWave = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Combustion") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pCombustion))
                        m_spells.mage.pCombustion = pSpellEntry;
                }
                break;
            }
            case CLASS_PRIEST:
            {
                if (pSpellEntry->SpellName[0].find("Power Word: Fortitude") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pPowerWordFortitude))
                        m_spells.priest.pPowerWordFortitude = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Divine Spirit") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pDivineSpirit))
                        m_spells.priest.pDivineSpirit = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Prayer of Spirit") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pPrayerofSpirit))
                        m_spells.priest.pPrayerofSpirit = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Prayer of Fortitude") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pPrayerofFortitude))
                        m_spells.priest.pPrayerofFortitude = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Prayer of Shadow Protection") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pPrayerofShadowProtection))
                        m_spells.priest.pPrayerofShadowProtection = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Inner Fire") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pInnerFire))
                        m_spells.priest.pInnerFire = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Shadow Protection") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pShadowProtection))
                        m_spells.priest.pShadowProtection = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Power Word: Shield") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pPowerWordShield))
                        m_spells.priest.pPowerWordShield = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Holy Nova") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pHolyNova))
                        m_spells.priest.pHolyNova = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Holy Fire") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pHolyFire))
                        m_spells.priest.pHolyFire = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Mind Blast") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pMindBlast))
                        m_spells.priest.pMindBlast = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Mind Flay") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pMindFlay))
                        m_spells.priest.pMindFlay = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Shadow Word: Pain") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pShadowWordPain))
                        m_spells.priest.pShadowWordPain = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Inner Focus") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pInnerFocus))
                        m_spells.priest.pInnerFocus = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Abolish Disease") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pAbolishDisease))
                        m_spells.priest.pAbolishDisease = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Dispel Magic") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pDispelMagic))
                        m_spells.priest.pDispelMagic = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Mana Burn") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pManaBurn))
                        m_spells.priest.pManaBurn = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Devouring Plague") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pDevouringPlague))
                        m_spells.priest.pDevouringPlague = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Psychic Scream") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pPsychicScream))
                        m_spells.priest.pPsychicScream = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Shadowform") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pShadowform))
                        m_spells.priest.pShadowform = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Vampiric Embrace") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pVampiricEmbrace))
                        m_spells.priest.pVampiricEmbrace = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Silence") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pSilence))
                        m_spells.priest.pSilence = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Fade") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pFade))
                        m_spells.priest.pFade = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Shackle Undead") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pShackleUndead))
                        m_spells.priest.pShackleUndead = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Smite") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pSmite))
                        m_spells.priest.pSmite = pSpellEntry;
                }
                break;
            }
            case CLASS_WARLOCK:
            {
                if (pSpellEntry->SpellName[0].find("Demon Armor") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pDemonArmor))
                        m_spells.warlock.pDemonArmor = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Death Coil") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pDeathCoil))
                        m_spells.warlock.pDeathCoil = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Detect Invisibility") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pDetectInvisibility))
                        m_spells.warlock.pDetectInvisibility = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Shadow Ward") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pShadowWard))
                        m_spells.warlock.pShadowWard = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Shadow Bolt") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pShadowBolt))
                        m_spells.warlock.pShadowBolt = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Corruption") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pCorruption))
                        m_spells.warlock.pCorruption = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Conflagrate") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pConflagrate))
                        m_spells.warlock.pConflagrate = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Shadowburn") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pShadowburn))
                        m_spells.warlock.pShadowburn = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Searing Pain") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pSearingPain))
                        m_spells.warlock.pSearingPain = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Immolate") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pImmolate))
                        m_spells.warlock.pImmolate = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Rain of Fire") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pRainOfFire))
                        m_spells.warlock.pRainOfFire = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Demonic Sacrifice") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pDemonicSacrifice))
                        m_spells.warlock.pDemonicSacrifice = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Drain Life") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pDrainLife))
                        m_spells.warlock.pDrainLife = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Siphon Life") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pSiphonLife))
                        m_spells.warlock.pSiphonLife = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Banish") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pBanish))
                        m_spells.warlock.pBanish = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Fear") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pFear))
                        m_spells.warlock.pFear = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Howl of Terror") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pHowlofTerror))
                        m_spells.warlock.pHowlofTerror = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Curse of Agony") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pCurseofAgony))
                        m_spells.warlock.pCurseofAgony = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Curse of the Elements") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pCurseoftheElements))
                        m_spells.warlock.pCurseoftheElements = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Curse of Shadow") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pCurseofShadow))
                        m_spells.warlock.pCurseofShadow = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Curse of Recklessness") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pCurseofRecklessness))
                        m_spells.warlock.pCurseofRecklessness = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Curse of Tongues") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pCurseofTongues))
                        m_spells.warlock.pCurseofTongues = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Life Tap") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pLifeTap))
                        m_spells.warlock.pLifeTap = pSpellEntry;
                }
                break;
            }
            case CLASS_WARRIOR:
            {
                if (pSpellEntry->SpellName[0].find("Battle Stance") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pBattleStance))
                        m_spells.warrior.pBattleStance = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Berserker Stance") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pBerserkerStance))
                        m_spells.warrior.pBerserkerStance = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Defensive Stance") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pDefensiveStance))
                        m_spells.warrior.pDefensiveStance = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0] == "Charge")
                {
                    if (IsHigherRankSpell(m_spells.warrior.pCharge))
                        m_spells.warrior.pCharge = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Intercept") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pIntercept))
                        m_spells.warrior.pIntercept = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Overpower") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pOverpower))
                        m_spells.warrior.pOverpower = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Heroic Strike") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pHeroicStrike))
                        m_spells.warrior.pHeroicStrike = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Cleave") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pCleave))
                        m_spells.warrior.pCleave = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Execute") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pExecute))
                        m_spells.warrior.pExecute = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Mortal Strike") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pMortalStrike))
                        m_spells.warrior.pMortalStrike = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Bloodthirst") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pBloodthirst))
                        m_spells.warrior.pBloodthirst = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Bloodrage") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pBloodrage))
                        m_spells.warrior.pBloodrage = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Berserker Rage") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pBerserkerRage))
                        m_spells.warrior.pBerserkerRage = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Recklessness") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pRecklessness))
                        m_spells.warrior.pRecklessness = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Retaliation") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pRetaliation))
                        m_spells.warrior.pRetaliation = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Death Wish") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pDeathWish))
                        m_spells.warrior.pDeathWish = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Intimidating Shout") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pIntimidatingShout))
                        m_spells.warrior.pIntimidatingShout = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Pummel") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pPummel))
                        m_spells.warrior.pPummel = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Rend") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pRend))
                        m_spells.warrior.pRend = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Disarm") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pDisarm))
                        m_spells.warrior.pDisarm = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Whirlwind") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pWhirlwind))
                        m_spells.warrior.pWhirlwind = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Battle Shout") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pBattleShout))
                        m_spells.warrior.pBattleShout = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Demoralizing Shout") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pDemoralizingShout))
                        m_spells.warrior.pDemoralizingShout = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Hamstring") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pHamstring))
                        m_spells.warrior.pHamstring = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Thunder Clap") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pThunderClap))
                        m_spells.warrior.pThunderClap = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Sweeping Strikes") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pSweepingStrikes))
                        m_spells.warrior.pSweepingStrikes = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Last Stand") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pLastStand))
                        m_spells.warrior.pLastStand = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Shield Block") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pShieldBlock))
                        m_spells.warrior.pShieldBlock = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Shield Wall") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pShieldWall))
                        m_spells.warrior.pShieldWall = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Shield Bash") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pShieldBash))
                        m_spells.warrior.pShieldBash = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Shield Slam") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pShieldSlam))
                        m_spells.warrior.pShieldSlam = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Sunder Armor") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pSunderArmor))
                        m_spells.warrior.pSunderArmor = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0] == "Revenge") // not "Revenge Stun"
                {
                    if (IsHigherRankSpell(m_spells.warrior.pRevenge))
                        m_spells.warrior.pRevenge = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Concussion Blow") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pConcussionBlow))
                        m_spells.warrior.pConcussionBlow = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Piercing Howl") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pPiercingHowl))
                        m_spells.warrior.pPiercingHowl = pSpellEntry;
                }
                break;
            }
            case CLASS_ROGUE:
            {
                if (pSpellEntry->SpellName[0].find("Slice and Dice") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pSliceAndDice))
                        m_spells.rogue.pSliceAndDice = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Sinister Strike") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pSinisterStrike))
                        m_spells.rogue.pSinisterStrike = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Adrenaline Rush") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pAdrenalineRush))
                        m_spells.rogue.pAdrenalineRush = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Eviscerate") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pEviscerate))
                        m_spells.rogue.pEviscerate = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Stealth") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pStealth))
                        m_spells.rogue.pStealth = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Garrote") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pGarrote))
                        m_spells.rogue.pGarrote = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Ambush") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pAmbush))
                        m_spells.rogue.pAmbush = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Cheap Shot") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pCheapShot))
                        m_spells.rogue.pCheapShot = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Premeditation") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pPremeditation))
                        m_spells.rogue.pPremeditation = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Backstab") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pBackstab))
                        m_spells.rogue.pBackstab = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Hemorrhage") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pHemorrhage))
                        m_spells.rogue.pHemorrhage = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Ghostly Strike") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pGhostlyStrike))
                        m_spells.rogue.pGhostlyStrike = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Gouge") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pGouge))
                        m_spells.rogue.pGouge = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Rupture") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pRupture))
                        m_spells.rogue.pRupture = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Expose Armor") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pExposeArmor))
                        m_spells.rogue.pExposeArmor = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Kidney Shot") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pKidneyShot))
                        m_spells.rogue.pKidneyShot = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Cold Blood") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pColdBlood))
                        m_spells.rogue.pColdBlood = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Blade Flurry") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pBladeFlurry))
                        m_spells.rogue.pBladeFlurry = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Vanish") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pVanish))
                        m_spells.rogue.pVanish = pSpellEntry;
                }
                else if (pSpellEntry->IsFitToFamily<SPELLFAMILY_ROGUE, CF_ROGUE_BLIND>())
                {
                    if (IsHigherRankSpell(m_spells.rogue.pBlind))
                        m_spells.rogue.pBlind = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Preparation") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pPreparation))
                        m_spells.rogue.pPreparation = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Evasion") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pEvasion))
                        m_spells.rogue.pEvasion = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Riposte") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pRiposte))
                        m_spells.rogue.pRiposte = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Kick") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pKick))
                        m_spells.rogue.pKick = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Sprint") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pSprint))
                        m_spells.rogue.pSprint = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Deadly Poison") != std::string::npos)
                {
                    hasDeadlyPoison = true;
                }
                else if (pSpellEntry->SpellName[0].find("Instant Poison") != std::string::npos)
                {
                    hasInstantPoison = true;
                }
                else if (pSpellEntry->SpellName[0].find("Crippling Poison") != std::string::npos)
                {
                    hasCripplingPoison = true;
                }
                else if (pSpellEntry->SpellName[0].find("Wound Poison") != std::string::npos)
                {
                    hasWoundPoison = true;
                }
                else if (pSpellEntry->SpellName[0].find("Mind-numbing Poison") != std::string::npos)
                {
                    HasMindNumbingPoison = true;
                }
                break;
            }
            case CLASS_DRUID:
            {
                if (pSpellEntry->SpellName[0].find("Bear Form") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pBearForm))
                        m_spells.druid.pBearForm = pSpellEntry;
                }
                else if (pSpellEntry->Id == (768)) // Cat Form
                {
                    if (IsHigherRankSpell(m_spells.druid.pCatForm))
                        m_spells.druid.pCatForm = pSpellEntry;
                }
                else if (pSpellEntry->Id == (783)) // Travel Form
                {
                    if (IsHigherRankSpell(m_spells.druid.pTravelForm))
                        m_spells.druid.pTravelForm = pSpellEntry;
                }
                else if (pSpellEntry->Id == (1066)) // Aquatic Form
                {
                    if (IsHigherRankSpell(m_spells.druid.pAquaticForm))
                        m_spells.druid.pAquaticForm = pSpellEntry;
                }
                else if (pSpellEntry->Id == (24858)) // Moonkin Form
                {
                    if (IsHigherRankSpell(m_spells.druid.pMoonkinForm))
                        m_spells.druid.pMoonkinForm = pSpellEntry;
                }
                else if (pSpellEntry->Id == 18562) // Swiftmend (before 1.12 this id was Innervate)
                {
                    if (IsHigherRankSpell(m_spells.druid.pSwiftmend))
                        m_spells.druid.pSwiftmend = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Wrath") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pWrath))
                        m_spells.druid.pWrath = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Moonfire") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pMoonfire))
                        m_spells.druid.pMoonfire = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Starfire") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pStarfire))
                        m_spells.druid.pStarfire = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Hurricane") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pHurricane))
                        m_spells.druid.pHurricane = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Insect Swarm") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pInsectSwarm))
                        m_spells.druid.pInsectSwarm = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Barkskin") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pBarkskin))
                        m_spells.druid.pBarkskin = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Nature's Grasp") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pNaturesGrasp))
                        m_spells.druid.pNaturesGrasp = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Mark of the Wild") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pMarkoftheWild))
                        m_spells.druid.pMarkoftheWild = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Gift of the Wild") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pGiftoftheWild))
                        m_spells.druid.pGiftoftheWild = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Thorns") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pThorns))
                        m_spells.druid.pThorns = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Remove Curse") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pRemoveCurse))
                        m_spells.druid.pRemoveCurse = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Cure Poison") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pCurePoison))
                        m_spells.druid.pCurePoison = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Abolish Poison") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pAbolishPoison))
                        m_spells.druid.pAbolishPoison = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Rebirth") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pRebirth))
                        m_spells.druid.pRebirth = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Innervate") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pInnervate))
                        m_spells.druid.pInnervate = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Nature's Swiftness") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pNaturesSwiftness))
                        m_spells.druid.pNaturesSwiftness = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Entangling Roots") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pEntanglingRoots))
                        m_spells.druid.pEntanglingRoots = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Hibernate") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pHibernate))
                        m_spells.druid.pHibernate = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Pounce") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pPounce))
                        m_spells.druid.pPounce = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Ravage") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pRavage))
                        m_spells.druid.pRavage = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Claw") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pClaw))
                        m_spells.druid.pClaw = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Shred") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pShred))
                        m_spells.druid.pShred = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Rake") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pRake))
                        m_spells.druid.pRake = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Rip") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pRip))
                        m_spells.druid.pRip = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Ferocious Bite") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pFerociousBite))
                        m_spells.druid.pFerociousBite = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Tiger's Fury") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pTigersFury))
                        m_spells.druid.pTigersFury = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Dash") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pDash))
                        m_spells.druid.pDash = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Cower") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pCower))
                        m_spells.druid.pCower = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Faerie Fire (Feral)") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pFaerieFireFeral))
                        m_spells.druid.pFaerieFireFeral = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Faerie Fire") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pFaerieFire))
                        m_spells.druid.pFaerieFire = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Growl") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pGrowl))
                        m_spells.druid.pGrowl = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Challenging Roar") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pChallengingRoar))
                        m_spells.druid.pChallengingRoar = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Demoralizing Roar") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pDemoralizingRoar))
                        m_spells.druid.pDemoralizingRoar = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Enrage") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pEnrage))
                        m_spells.druid.pEnrage = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Frenzied Regeneration") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pFrenziedRegeneration))
                        m_spells.druid.pFrenziedRegeneration = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Swipe") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pSwipe))
                        m_spells.druid.pSwipe = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Maul") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pMaul))
                        m_spells.druid.pMaul = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Bash") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pBash))
                        m_spells.druid.pBash = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Feral Charge") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pFeralCharge))
                        m_spells.druid.pFeralCharge = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Prowl") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pProwl))
                        m_spells.druid.pProwl = pSpellEntry;
                }
                break;
            }
        }

        for (uint32 i = 0; i < MAX_SPELL_EFFECTS; i++)
        {
            switch (pSpellEntry->Effect[i])
            {
                case SPELL_EFFECT_HEAL:
                    // Swiftmend heals from a Rejuvenation/Regrowth on the target (base points 0):
                    // cast on its own in the druid rotation, not picked as a normal direct heal.
                    if (pSpellEntry->Id != 18562)
                        m_spellListDirectHeal.insert(pSpellEntry);
                    break;
                case SPELL_EFFECT_ATTACK_ME:
                    m_spellListTaunt.push_back(pSpellEntry);
                    break;
                case SPELL_EFFECT_RESURRECT:
                case SPELL_EFFECT_RESURRECT_NEW:
                    m_resurrectionSpell = pSpellEntry;
                    break;
                case SPELL_EFFECT_APPLY_AURA:
                {
                    switch (pSpellEntry->EffectApplyAuraName[i])
                    {
                        case SPELL_AURA_PERIODIC_HEAL:
                            m_spellListPeriodicHeal.insert(pSpellEntry);
                            break;
                        case SPELL_AURA_MOD_TAUNT:
                            m_spellListTaunt.push_back(pSpellEntry);
                            break;
                    }
                    break;
                }
            }
        }
    }

    switch (me->GetClass())
    {
        case CLASS_PALADIN:
        {
            if (pSealOfFury && m_role == ROLE_TANK)
                m_spells.paladin.pSeal = pSealOfFury;
            else if (pSealOfCommand)
                m_spells.paladin.pSeal = pSealOfCommand;
            else
                m_spells.paladin.pSeal = pSealOfRighteousness;

            if (pBlessingOfSanctuary && m_role == ROLE_TANK)
                m_spells.paladin.pBlessingBuff = pBlessingOfSanctuary;
            else
            {
                std::vector<SpellEntry const*> blessings;
                if (pBlessingOfLight)
                    blessings.push_back(pBlessingOfLight);
                if (pBlessingOfMight)
                    blessings.push_back(pBlessingOfMight);
                if (pBlessingOfWisdom)
                    blessings.push_back(pBlessingOfWisdom);
                if (pBlessingOfKings)
                    blessings.push_back(pBlessingOfKings);
                if (pBlessingOfSanctuary)
                    blessings.push_back(pBlessingOfSanctuary);
                if (!blessings.empty())
                    m_spells.paladin.pBlessingBuff = SelectRandomContainerElement(blessings);
            }

            std::vector<SpellEntry const*> auras;
            if (pDevotionAura)
                auras.push_back(pDevotionAura);
            if (pConcentrationAura)
                auras.push_back(pConcentrationAura);
            if (pRetributionAura)
                auras.push_back(pRetributionAura);
            if (pSanctityAura)
                auras.push_back(pSanctityAura);
            if (pShadowResistanceAura)
                auras.push_back(pShadowResistanceAura);
            if (pFrostResistanceAura)
                auras.push_back(pFrostResistanceAura);
            if (pFireResistanceAura)
                auras.push_back(pFireResistanceAura);
            if (!auras.empty())
                m_spells.paladin.pAura = SelectRandomContainerElement(auras);

            break;
        }
        case CLASS_SHAMAN:
        {
            std::vector<SpellEntry const*> airTotems;
            if (pGraceOfAirTotem)
                airTotems.push_back(pGraceOfAirTotem);
            if (pNatureResistanceTotem)
                airTotems.push_back(pNatureResistanceTotem);
            if (pWindfuryTotem)
                airTotems.push_back(pWindfuryTotem);
            if (pWindwallTotem)
                airTotems.push_back(pWindwallTotem);
            if (pTranquilAirTotem)
                airTotems.push_back(pTranquilAirTotem);
            if (!airTotems.empty())
                m_spells.shaman.pAirTotem = SelectRandomContainerElement(airTotems);

            std::vector<SpellEntry const*> earthTotems;
            if (pEarthbindTotem)
                earthTotems.push_back(pEarthbindTotem);
            if (pStoneclawtotem)
                earthTotems.push_back(pStoneclawtotem);
            if (pStoneskinTotem)
                earthTotems.push_back(pStoneskinTotem);
            if (pStrengthOfEarthTotem)
                earthTotems.push_back(pStrengthOfEarthTotem);
            if (pTremorTotem)
                earthTotems.push_back(pTremorTotem);
            if (!earthTotems.empty())
                m_spells.shaman.pEarthTotem = SelectRandomContainerElement(earthTotems);

            std::vector<SpellEntry const*> fireTotems;
            if (pFireNovaTotem)
                fireTotems.push_back(pFireNovaTotem);
            if (pMagmaTotem)
                fireTotems.push_back(pMagmaTotem);
            if (pSearingTotem)
                fireTotems.push_back(pSearingTotem);
            if (pFlametongueTotem)
                fireTotems.push_back(pFlametongueTotem);
            if (pFrostResistanceTotem)
                fireTotems.push_back(pFrostResistanceTotem);
            if (!fireTotems.empty())
                m_spells.shaman.pFireTotem = SelectRandomContainerElement(fireTotems);

            std::vector<SpellEntry const*> waterTotems;
            if (pFireResistanceTotem)
                waterTotems.push_back(pFireResistanceTotem);
            if (pDiseaseCleansingTotem)
                waterTotems.push_back(pDiseaseCleansingTotem);
            if (pHealingStreamTotem)
                waterTotems.push_back(pHealingStreamTotem);
            if (pManaSpringTotem)
                waterTotems.push_back(pManaSpringTotem);
            if (pPoisonCleansingTotem)
                waterTotems.push_back(pPoisonCleansingTotem);
            if (!waterTotems.empty())
                m_spells.shaman.pWaterTotem = SelectRandomContainerElement(waterTotems);

            if (pWindfuryWeapon && m_role == ROLE_MELEE_DPS)
                m_spells.shaman.pWeaponBuff = pWindfuryWeapon;
            else
            {
                std::vector<SpellEntry const*> weaponBuffs;
                if (pWindfuryWeapon)
                    weaponBuffs.push_back(pWindfuryWeapon);
                if (pRockbiterWeapon)
                    weaponBuffs.push_back(pRockbiterWeapon);
                if (pFrostbrandWeapon)
                    weaponBuffs.push_back(pFrostbrandWeapon);
                if (!weaponBuffs.empty())
                    m_spells.shaman.pWeaponBuff = SelectRandomContainerElement(weaponBuffs);
            }

            break;
        }
        case CLASS_MAGE:
        {
            if (!m_spells.mage.pIceArmor && pFrostArmor)
                m_spells.mage.pIceArmor = pFrostArmor;

            std::vector<SpellEntry const*> polymorph;
            if (pPolymorphSheep)
                polymorph.push_back(pPolymorphSheep);
            if (pPolymorphCow)
                polymorph.push_back(pPolymorphCow);
            if (pPolymorphPig)
                polymorph.push_back(pPolymorphPig);
            if (pPolymorphTurtle)
                polymorph.push_back(pPolymorphTurtle);
            if (!polymorph.empty())
                m_spells.mage.pPolymorph = SelectRandomContainerElement(polymorph);

            break;
        }
        case CLASS_ROGUE:
        {
            // Rogues can only craft an item that applies the poison, they don't know the actual poison enchant.
            auto GetHighestRankOfPoisonByName = [](std::string name, uint32 level)
            {
                SpellEntry const* pHighestRank = nullptr;
                for (uint32 i = 0; i < sSpellMgr.GetMaxSpellId(); i++)
                {
                    if (SpellEntry const* pSpellEntry = sSpellMgr.GetSpellEntry(i))
                    {
                        if (pSpellEntry->Effect[0] == SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY &&
                            pSpellEntry->SpellName[0] == name && pSpellEntry->spellLevel <= level &&
                           (!pHighestRank || pHighestRank->spellLevel < pSpellEntry->spellLevel))
                        {
                            pHighestRank = pSpellEntry;
                        }
                    }
                }
                return pHighestRank;
            };

            SpellEntry const* pPoisonSpell = nullptr;
            std::vector<SpellEntry const*> vPoisons;
            if (hasDeadlyPoison && (pPoisonSpell = GetHighestRankOfPoisonByName("Deadly Poison", me->GetLevel())))
                vPoisons.push_back(pPoisonSpell);
            if (hasInstantPoison && (pPoisonSpell = GetHighestRankOfPoisonByName("Instant Poison", me->GetLevel())))
                vPoisons.push_back(pPoisonSpell);
            if (hasCripplingPoison && (pPoisonSpell = GetHighestRankOfPoisonByName("Crippling Poison", me->GetLevel())))
                vPoisons.push_back(pPoisonSpell);
            if (hasWoundPoison && (pPoisonSpell = GetHighestRankOfPoisonByName("Wound Poison", me->GetLevel())))
                vPoisons.push_back(pPoisonSpell);
            if (HasMindNumbingPoison && (pPoisonSpell = GetHighestRankOfPoisonByName("Mind-numbing Poison", me->GetLevel())))
                vPoisons.push_back(pPoisonSpell);

            if (!vPoisons.empty())
            {
                m_spells.rogue.pMainHandPoison = SelectRandomContainerElement(vPoisons);
                m_spells.rogue.pOffHandPoison = SelectRandomContainerElement(vPoisons);
            }

            break;
        }
    }
}

void CombatBotBaseAI::AddAllSpellReagents()
{
    for (const auto& pSpell : m_spells.raw.spells)
    {
        if (pSpell)
            EnsureSpellReagents(pSpell);
    }
}

// Supplies are free: before a cast (and on add/ding) the bot gets what the spell needs - every
// reagent up to a full stack (at least the spell's count) and each required totem item.
void CombatBotBaseAI::EnsureSpellReagents(SpellEntry const* pSpellEntry)
{
    for (uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
    {
        if (pSpellEntry->Reagent[i] <= 0)
            continue;

        uint32 const itemId = pSpellEntry->Reagent[i];
        uint32 const need = std::max<uint32>(1, pSpellEntry->ReagentCount[i]);
        uint32 const have = me->GetItemCount(itemId);
        if (have >= need)
            continue;

        ItemPrototype const* pProto = sObjectMgr.GetItemPrototype(itemId);
        if (!pProto)
            continue;

        uint32 const target = std::max<uint32>(need, pProto->GetMaxStackSize());
        AddItemToInventory(itemId, target - have);
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Supply] %s: gets %u %s for %s (had %u)", me->GetName(),
            target - have, pProto->Name1, pSpellEntry->SpellName[0].c_str(), have);
    }

    for (auto const& totem : pSpellEntry->Totem)
    {
        if (totem && !me->HasItemCount(totem, 1))
        {
            AddItemToInventory(totem);
            if (ItemPrototype const* pProto = sObjectMgr.GetItemPrototype(totem))
                sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Supply] %s: gets %s for %s", me->GetName(),
                    pProto->Name1, pSpellEntry->SpellName[0].c_str());
        }
    }
}

bool CombatBotBaseAI::AreOthersOnSameTarget(ObjectGuid guid, bool checkMelee, bool checkSpells) const
{
    Group* pGroup = me->GetGroup();
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        if (Player* pMember = itr->getSource())
        {
            // Not self.
            if (pMember == me)
                continue;

            // Not the target itself.
            if (pMember->GetObjectGuid() == guid)
                continue;

            if (pMember->GetTargetGuid() == guid)
            {
                if (checkMelee && pMember->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
                    return true;

                if (checkSpells && pMember->IsNonMeleeSpellCasted())
                    return true;
            }
        }
    }
    return false;
}

bool CombatBotBaseAI::FindAndHealInjuredAlly(float selfHealPercent, float groupHealPercent)
{
    Unit* pTarget = SelectHealTarget(selfHealPercent, groupHealPercent);
    if (!pTarget)
        return false;

    return HealInjuredTarget(pTarget);
}

template <class T>
SpellEntry const* CombatBotBaseAI::SelectMostEfficientHealingSpell(Unit const* pTarget, std::set<SpellEntry const*, T>& spellList) const
{
    return SelectMostEfficientHealingSpell(pTarget, pTarget->GetMaxHealth() - pTarget->GetHealth(), spellList);
}

template <class T>
SpellEntry const* CombatBotBaseAI::SelectMostEfficientHealingSpell(Unit const* pTarget, int32 missingHealth, std::set<SpellEntry const*, T>& spellList) const
{
    SpellEntry const* pHealSpell = nullptr;
    int32 healthDiff = INT32_MAX;

    // Find most efficient healing spell.
    for (const auto pSpellEntry : spellList)
    {
        if (CanTryToCastSpell(pTarget, pSpellEntry))
        {
            int32 basePoints = 0;
            for (uint32 i = 0; i < MAX_SPELL_EFFECTS; i++)
            {
                switch (pSpellEntry->Effect[i])
                {
                    case SPELL_EFFECT_HEAL:
                        basePoints += pSpellEntry->EffectBasePoints[i];
                        break;
                    case SPELL_EFFECT_APPLY_AURA:
                    case SPELL_EFFECT_PERSISTENT_AREA_AURA:
                    case SPELL_EFFECT_APPLY_AREA_AURA_PARTY:
                        if (pSpellEntry->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_HEAL)
                            basePoints += ((pSpellEntry->GetDuration() / pSpellEntry->EffectAmplitude[i]) * pSpellEntry->EffectBasePoints[i]);
                        break;
                }
            }

            int32 const diff = basePoints - missingHealth;
            if (std::abs(diff) < healthDiff)
            {
                healthDiff = diff;
                pHealSpell = pSpellEntry;
            }

            // Healing spells are sorted from strongest to weakest.
            if (diff < 0)
                break;
        }
    }

    return pHealSpell;
}

int32 CombatBotBaseAI::GetIncomingdamage(Unit const* pTarget) const
{
    int32 damage = 0;
    for (auto const& pAttacker : pTarget->GetAttackers())
        if (pAttacker->CanReachWithMeleeAutoAttack(pTarget))
            damage += int32((pAttacker->GetFloatValue(UNIT_FIELD_MINDAMAGE) + pAttacker->GetFloatValue(UNIT_FIELD_MAXDAMAGE)) / 2);
    return damage;
}

bool CombatBotBaseAI::HealInjuredTarget(Unit* pTarget)
{
    // Put a HoT on the target if only missing a little health.
    if (pTarget->GetHealthPercent() >= 80.0f &&
       !pTarget->HasAuraType(SPELL_AURA_PERIODIC_HEAL))
    {
        if (HealInjuredTargetPeriodic(pTarget))
            return true;
    }

    if (HealInjuredTargetDirect(pTarget))
        return true;

    return false;
}

bool CombatBotBaseAI::HealInjuredTargetPeriodic(Unit* pTarget)
{
    if (SpellEntry const* pHealSpell = SelectMostEfficientHealingSpell(pTarget, m_spellListPeriodicHeal))
    {
        if (CanTryToCastSpell(pTarget, pHealSpell))
        {
            if (DoCastSpell(pTarget, pHealSpell) == SPELL_CAST_OK)
                return true;
        }
    }

    return false;
}

bool CombatBotBaseAI::HealInjuredTargetDirect(Unit* pTarget)
{
    if (SpellEntry const* pHealSpell = SelectMostEfficientHealingSpell(pTarget, m_spellListDirectHeal))
        if (DoCastSpell(pTarget, pHealSpell) == SPELL_CAST_OK)
            return true;

    return false;
}

bool CombatBotBaseAI::IsValidHealTarget(Unit const* pTarget, float healthPercent) const
{
    return (pTarget->GetHealthPercent() < healthPercent) &&
            me->IsValidHelpfulTarget(pTarget) &&
            me->IsWithinLOSInMap(pTarget) &&
            me->IsWithinDist(pTarget, 30.0f);
}

Unit* CombatBotBaseAI::SelectHealTarget(float selfHealPercent, float groupHealPercent) const
{
    if (me->GetHealthPercent() < selfHealPercent)
        return me;

    if (IsInDuel())
        return nullptr;

    Unit* pTarget = nullptr;
    float healthPercent = 100.0f;
    uint8 bestPriority = 0;

    if (Group* pGroup = me->GetGroup())
    {
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            if (Unit* pMember = itr->getSource())
            {
                // We already checked self.
                if (pMember == me)
                    continue;

                uint8 const priority = GetHealPriority(pMember);

                // Avoid all healers picking same target.
                if (pTarget && !priority && !IsTankClass(pMember->GetClass()) && AreOthersOnSameTarget(pMember->GetObjectGuid(), false, true))
                    continue;

                // Check if we should heal party member: higher heal priority (tanks) first,
                // then lowest health.
                if (IsValidHealTarget(pMember, groupHealPercent) &&
                   (priority > bestPriority ||
                   (priority == bestPriority && healthPercent > pMember->GetHealthPercent())))
                {
                    healthPercent = pMember->GetHealthPercent();
                    pTarget = pMember;
                    bestPriority = priority;
                }
                // Or a pet if there are no injured players.
                else if (!pTarget && (pMember = pMember->GetPet()) &&
                          IsValidHealTarget(pMember, groupHealPercent))
                {
                    healthPercent = pMember->GetHealthPercent();
                    pTarget = pMember;
                }
            }
        }
    }

    if (healthPercent == 100.0f)
        return nullptr;

    return pTarget;
}

Unit* CombatBotBaseAI::SelectPeriodicHealTarget(float selfHealPercent, float groupHealPercent) const
{
    if (me->GetHealthPercent() < selfHealPercent &&
       !me->HasAuraType(SPELL_AURA_PERIODIC_HEAL))
        return me;

    if (IsInDuel())
        return nullptr;

    Unit* pTarget = nullptr;
    uint8 bestPriority = 0;

    if (Group* pGroup = me->GetGroup())
    {
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            if (Unit* pMember = itr->getSource())
            {
                // We already checked self.
                if (pMember == me)
                    continue;

                // Check if we should heal party member (higher heal priority first).
                uint8 const priority = GetHealPriority(pMember);
                if ((!pTarget || priority > bestPriority) &&
                    IsValidHealTarget(pMember, groupHealPercent) &&
                   !pMember->HasAuraType(SPELL_AURA_PERIODIC_HEAL))
                {
                    pTarget = pMember;
                    bestPriority = priority;
                }
            }
        }
    }

    return pTarget;
}

bool CombatBotBaseAI::FindAndPreHealTarget()
{
    Unit* pTarget = me;
    int32 maxIncomingDamage = GetIncomingdamage(me);

    if (!IsInDuel())
    {
        if (Group* pGroup = me->GetGroup())
        {
            for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                if (Unit* pMember = itr->getSource())
                {
                    // We already checked self.
                    if (pMember == me)
                        continue;

                    // Avoid all healers picking same target.
                    if (pTarget && !IsTankClass(pMember->GetClass()) && AreOthersOnSameTarget(pMember->GetObjectGuid(), false, true))
                        continue;

                    int32 incomingDamage = GetIncomingdamage(pMember);
                    if (!incomingDamage)
                        continue;

                    // Check if we should heal party member.
                    if (incomingDamage > maxIncomingDamage &&
                        IsValidHealTarget(pMember))
                    {
                        maxIncomingDamage = incomingDamage;
                        pTarget = pMember;
                    }
                }
            }
        }
    }

    if (!maxIncomingDamage)
        return false;

    // Add currently missing health too.
    maxIncomingDamage += int32(pTarget->GetMaxHealth() - pTarget->GetHealth());
    if (maxIncomingDamage < int32(pTarget->GetMaxHealth() / 2))
        return false;

    if (SpellEntry const* pHealSpell = SelectMostEfficientHealingSpell(pTarget, maxIncomingDamage, m_spellListDirectHeal))
    {
        if (pHealSpell->GetCastTime(me) > 1000 && CanTryToCastSpell(pTarget, pHealSpell))
        {
            if (DoCastSpell(pTarget, pHealSpell) == SPELL_CAST_OK)
                return true;
        }
    }

    return pTarget;
}

bool CombatBotBaseAI::IsValidHostileTarget(Unit const* pTarget) const
{
    return me->IsValidAttackTarget(pTarget) &&
           pTarget->IsVisibleForOrDetect(me, me, false) &&
          (!pTarget->HasBreakableByDamageCrowdControlAura() || pTarget->GetObjectGuid() == m_breakCCTargetGuid) &&
           !pTarget->IsTotalImmune() &&
           pTarget->GetTransport() == me->GetTransport();
}

bool CombatBotBaseAI::IsValidDispelTarget(Unit const* pTarget, SpellEntry const* pSpellEntry) const
{
    uint32 dispelMask = 0;
    bool bFoundOneDispell = false;
    // Compute Dispel Mask
    for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        if (pSpellEntry->Effect[i] != SPELL_EFFECT_DISPEL)
            continue;

        // Create dispel mask by dispel type
        uint32 dispel_type = pSpellEntry->EffectMiscValue[i];
        dispelMask |= Spells::GetDispellMask(DispelType(dispel_type));
    }
    bool friendly_dispel = pTarget && pTarget->IsFriendlyTo(me);

    if (pTarget &&
        // Check immune for offensive dispel
        (!pTarget->IsImmuneToSchoolMask(pSpellEntry->GetSpellSchoolMask()) ||
            friendly_dispel))
    {
        if (!friendly_dispel && !me->IsValidAttackTarget(pTarget))
            return false;

        auto const& auras = pTarget->GetSpellAuraHolderMap();
        for (const auto& aura : auras)
        {
            SpellAuraHolder* holder = aura.second;
            if ((1 << holder->GetSpellProto()->Dispel) & dispelMask)
            {
                if (holder->GetSpellProto()->Dispel == DISPEL_MAGIC ||
                    holder->GetSpellProto()->Dispel == DISPEL_DISEASE ||
                    holder->GetSpellProto()->Dispel == DISPEL_POISON)
                {
                    bool positive = holder->IsPositive();
                    // do not remove positive auras if friendly target
                    // do not remove negative auras if non-friendly target
                    // when removing charm auras ignore hostile reaction from the charm
                    if (!friendly_dispel && !positive && holder->GetSpellProto()->IsCharmSpell())
                        if (CharmInfo *charm = pTarget->GetCharmInfo())
                            if (FactionTemplateEntry const* ft = charm->GetOriginalFactionTemplate())
                                if (FactionTemplateEntry const* ft2 = me->GetFactionTemplateEntry())
                                    if (charm->GetOriginalFactionTemplate()->IsFriendlyTo(*ft2))
                                        bFoundOneDispell = true;
                    if (positive == friendly_dispel)
                        continue;
                }
                bFoundOneDispell = true;
                break;
            }
        }
    }

    if (!bFoundOneDispell)
        return false;

    return true;
}

uint8 CombatBotBaseAI::GetAttackersInRangeCount(float range) const
{
    uint8 count = 0;
    for (const auto& pTarget : me->GetAttackers())
    {
        if (me->GetCombatDistance(pTarget) <= range)
            count++;
    }

    return count;
}

Unit* CombatBotBaseAI::SelectAttackerDifferentFrom(Unit const* pExcept) const
{
    for (const auto& pTarget : me->GetAttackers())
    {
        if (pTarget != pExcept)
            return pTarget;
    }

    return nullptr;
}

bool CombatBotBaseAI::IsValidBuffTarget(Unit const* pTarget, SpellEntry const* pSpellEntry) const
{
    std::vector<uint32> morePowerfulSpells;
    sSpellMgr.ListMorePowerfulSpells(pSpellEntry->Id, morePowerfulSpells);

    for (const auto& i : pTarget->GetSpellAuraHolderMap())
    {
        if (i.first == pSpellEntry->Id)
            return false;

        if (sSpellMgr.IsRankSpellDueToSpell(pSpellEntry, i.first))
            return false;

        for (const auto& it : morePowerfulSpells)
            if (it == i.first)
                return false;
    }

    return true;
}

Player* CombatBotBaseAI::SelectBuffTarget(SpellEntry const* pSpellEntry) const
{
    // Pull/attack order (target but not yet in combat): fight first, buff the group later.
    if (me->GetVictim() && !me->IsInCombat())
        return nullptr;

    Group* pGroup = me->GetGroup();
    if (pGroup)
    {
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            if (Player* pMember = itr->getSource())
            {
                if (me->IsValidHelpfulTarget(pMember) &&
                   !pMember->IsGameMaster() &&
                    IsValidBuffTarget(pMember, pSpellEntry) &&
                    me->IsWithinLOSInMap(pMember) &&
                    me->IsWithinDist(pMember, 30.0f))
                    return pMember;
            }
        }
    }

    return nullptr;
}

Player* CombatBotBaseAI::SelectBuffTarget(SpellEntry const* pSingleSpellEntry, SpellEntry const* pGroupSpellEntry, SpellEntry const*& pSelectedSpellEntry) const
{
    pSelectedSpellEntry = nullptr;

    if (!pSingleSpellEntry && !pGroupSpellEntry)
        return nullptr;

    if (!pSingleSpellEntry)
    {
        pSelectedSpellEntry = pGroupSpellEntry;
        return SelectBuffTarget(pGroupSpellEntry);
    }

    if (!pGroupSpellEntry)
    {
        pSelectedSpellEntry = pSingleSpellEntry;
        return SelectBuffTarget(pSingleSpellEntry);
    }

    Player* pFirstMissingMember = nullptr;
    uint8 missingMemberCount = 0;
    Group* pGroup = me->GetGroup();
    if (pGroup)
    {
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            if (Player* pMember = itr->getSource())
            {
                if (!me->IsValidHelpfulTarget(pMember) ||
                    pMember->IsGameMaster() ||
                    !me->IsWithinLOSInMap(pMember) ||
                    !me->IsWithinDist(pMember, 30.0f) ||
                    !IsValidBuffTarget(pMember, pSingleSpellEntry) ||
                    !IsValidBuffTarget(pMember, pGroupSpellEntry))
                    continue;

                if (!pFirstMissingMember)
                    pFirstMissingMember = pMember;

                ++missingMemberCount;
                if (missingMemberCount > 1)
                {
                    pSelectedSpellEntry = pGroupSpellEntry;
                    return pFirstMissingMember;
                }
            }
        }
    }

    if (missingMemberCount == 1)
        pSelectedSpellEntry = pSingleSpellEntry;

    return pFirstMissingMember;
}

Player* CombatBotBaseAI::SelectDispelTarget(SpellEntry const* pSpellEntry) const
{
    Group* pGroup = me->GetGroup();
    if (pGroup)
    {
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            if (Player* pMember = itr->getSource())
            {
                if (me->IsValidHelpfulTarget(pMember) &&
                   !pMember->IsGameMaster() &&
                    IsValidDispelTarget(pMember, pSpellEntry) &&
                    me->IsWithinLOSInMap(pMember) &&
                    me->IsWithinDist(pMember, 30.0f))
                    return pMember;
            }
        }
    }

    return nullptr;
}

void CombatBotBaseAI::SummonPetIfNeeded()
{
    if (me->GetClass() == CLASS_HUNTER)
    {
        if (me->GetCharmGuid())
            return;

        if (me->GetLevel() < 10)
            return;

        if (me->GetPetGuid() || sCharacterDatabaseCache.GetCharacterPetByOwner(me->GetGUIDLow()))
        {
            if (Pet* pPet = me->GetPet())
            {
                if (!pPet->IsAlive())
                    me->CastSpell(pPet, SPELL_REVIVE_PET, true);
            }
            else
                me->CastSpell(me, SPELL_CALL_PET, true);

            return;
        }

        uint32 petId = PickRandomValue( PET_WOLF, PET_CAT, PET_BEAR, PET_CRAB, PET_GORILLA, PET_BIRD,
                                        PET_BOAR, PET_BAT, PET_CROC, PET_SPIDER, PET_OWL, PET_STRIDER,
                                        PET_SCORPID, PET_SERPENT, PET_RAPTOR, PET_TURTLE, PET_HYENA );
        if (Creature* pCreature = me->SummonCreature(petId,
            me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(), 0.0f,
            TEMPSUMMON_TIMED_COMBAT_OR_DEAD_DESPAWN, 3000, false, 3000))
        {
            pCreature->SetLevel(me->GetLevel());
            me->CastSpell(pCreature, SPELL_TAME_BEAST, true);
            sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Pet] %s: tames %s (level %u), pet %s",
                me->GetName(), GetLogName(pCreature).c_str(), pCreature->GetLevel(), me->GetPet() ? "created" : "NOT created");
        }
    }
    else if (me->GetClass() == CLASS_WARLOCK)
    {
        if (me->GetPetGuid() || me->GetCharmGuid())
            return;

        std::vector<uint32> vSummons;
        if (me->HasSpell(SPELL_SUMMON_IMP))
            vSummons.push_back(SPELL_SUMMON_IMP);
        if (me->HasSpell(SPELL_SUMMON_VOIDWALKER))
            vSummons.push_back(SPELL_SUMMON_VOIDWALKER);
        if (me->HasSpell(SPELL_SUMMON_FELHUNTER))
            vSummons.push_back(SPELL_SUMMON_FELHUNTER);
        if (me->HasSpell(SPELL_SUMMON_SUCCUBUS))
            vSummons.push_back(SPELL_SUMMON_SUCCUBUS);
        if (!vSummons.empty())
            me->CastSpell(me, SelectRandomContainerElement(vSummons), true);
    }
}

void CombatBotBaseAI::LearnArmorProficiencies()
{
    switch (me->GetClass())
    {
        case CLASS_WARRIOR:
        case CLASS_PALADIN:
        {
            if (me->GetLevel() >= 40 && !me->HasSpell(SPELL_PLATE_PROFICIENCY))
                me->LearnSpell(SPELL_PLATE_PROFICIENCY, false, false);
            break;
        }
        case CLASS_HUNTER:
        case CLASS_SHAMAN:
        {
            if (me->GetLevel() >= 40 && !me->HasSpell(SPELL_MAIL_PROFICIENCY))
                me->LearnSpell(SPELL_MAIL_PROFICIENCY, false, false);
            break;
        }
    }
}

void CombatBotBaseAI::LearnPremadeSpecForClass()
{
    std::vector<PlayerPremadeSpecTemplate const*> vSpecs;
    for (const auto& itr : sObjectMgr.GetPlayerPremadeSpecTemplates())
    {
        if (itr.second.requiredClass == me->GetClass() &&
            itr.second.level == me->GetLevel())
            vSpecs.push_back(&itr.second);
    }
    // Use lower level spec template if there are no templates for the current level.
    if (vSpecs.empty())
    {
        for (const auto& itr : sObjectMgr.GetPlayerPremadeSpecTemplates())
        {
            if (itr.second.requiredClass == me->GetClass() &&
                itr.second.level < me->GetLevel())
                vSpecs.push_back(&itr.second);
        }
    }
    if (!vSpecs.empty())
    {
        PlayerPremadeSpecTemplate const* pSpec = nullptr;
        // Try to find a role appropriate gear template.
        if (m_role != ROLE_INVALID)
        {
            for (const auto itr : vSpecs)
            {
                if (itr->role == m_role &&
                   (!pSpec || pSpec->level < itr->level))
                {
                    pSpec = itr;
                }
            }
        }
        // There is no spec template for this role, pick randomly.
        if (!pSpec)
            pSpec = SelectRandomContainerElement(vSpecs);
        sObjectMgr.ApplyPremadeSpecTemplateToPlayer(pSpec->entry, me);
        if (m_role == ROLE_INVALID)
            m_role = pSpec->role;
    }
    else
    {
        // Use gm command to learn spells on trainers and items.
        LearnRandomTalents();
        char args[] = "";
        ChatHandler(me).HandleLearnAllQuestsCommand(args);
        ChatHandler(me).HandleLearnAllTrainerCommand(args);
        ChatHandler(me).HandleLearnAllItemsCommand(args);
    }
}

void CombatBotBaseAI::LearnRandomTalents()
{
    if (!me->GetFreeTalentPoints())
        return;

    std::vector<uint32> talentTabsForClass;
    for (uint32 talentTab = 0; talentTab < sTalentTabStore.GetNumRows(); ++talentTab)
    {
        TalentTabEntry const* talentTabInfo = sTalentTabStore.LookupEntry(talentTab);
        if (!talentTabInfo)
            continue;

        if ((me->GetClassMask() & talentTabInfo->ClassMask) == 0)
            continue;

        talentTabsForClass.push_back(talentTab);
    }

    if (talentTabsForClass.empty())
        return;

    uint32 chosenTab = SelectRandomContainerElement(talentTabsForClass);

    std::map<uint32 /*row*/, std::vector<std::pair<uint32 /*talent id*/, uint32 /*ranks*/>>> possibleTalents;
    for (uint32 talentId = 0; talentId < sTalentStore.GetNumRows(); ++talentId)
    {
        TalentEntry const* talentInfo = sTalentStore.LookupEntry(talentId);
        if (!talentInfo)
            continue;

        if (talentInfo->TalentTab != chosenTab)
            continue;

        uint32 ranks;
        for (ranks = 0; ranks < MAX_TALENT_RANK && talentInfo->RankID[ranks]; ++ranks);
        possibleTalents[talentInfo->Row].push_back({ talentId, ranks });
    }

    if (possibleTalents.empty())
        return;

    auto seed = std::chrono::system_clock::now().time_since_epoch().count();
    for (auto& itrRow : possibleTalents)
    {
        std::shuffle(itrRow.second.begin(), itrRow.second.end(), std::default_random_engine(seed));
        for (auto const& itrTalent: itrRow.second)
        {
            for (uint32 rank = 0; rank < itrTalent.second; ++rank)
            {
                if (me->LearnTalent(itrTalent.first, rank))
                {
                    if (!me->GetFreeTalentPoints())
                        return;
                }
                else
                    break;
            }
        }
    }
}

void CombatBotBaseAI::EquipPremadeGearTemplate()
{
    std::vector<PlayerPremadeGearTemplate const*> vGear;
    for (const auto& itr : sObjectMgr.GetPlayerPremadeGearTemplates())
    {
        if (itr.second.requiredClass == me->GetClass() &&
            itr.second.level <= me->GetLevel())
        {
            if (!vGear.empty())
            {
                if (vGear.front()->level < itr.second.level)
                    vGear.clear();
                else if (vGear.front()->level > itr.second.level)
                    continue;
            }
            vGear.push_back(&itr.second);
        }
    }

    if (!vGear.empty())
    {
        std::vector<PlayerPremadeGearTemplate const*> vGear2;

        // Try to find a role appropriate gear template.
        if (m_role != ROLE_INVALID)
        {
            for (const auto itr : vGear)
            {
                if (itr->role == m_role)
                    vGear2.push_back(itr);
            }
        }

        PlayerPremadeGearTemplate const* pGear;
        if (vGear2.empty())
            pGear = SelectRandomContainerElement(vGear);
        else
            pGear = SelectRandomContainerElement(vGear2);

        sObjectMgr.ApplyPremadeGearTemplateToPlayer(pGear->entry, me);
    }
}

inline uint32 GetPrimaryItemStatForClassAndRole(uint8 playerClass, uint8 role)
{
    switch (playerClass)
    {
        case CLASS_WARRIOR:
        {
            return ITEM_MOD_STRENGTH;
        }
        case CLASS_PALADIN:
        {
            return ((role == ROLE_HEALER) ? ITEM_MOD_INTELLECT : ITEM_MOD_STRENGTH);
        }
        case CLASS_HUNTER:
        case CLASS_ROGUE:
        {
            return ITEM_MOD_AGILITY;
        }
        case CLASS_SHAMAN:
        case CLASS_DRUID:
        {
            return ((role == ROLE_MELEE_DPS || role == ROLE_TANK) ? ITEM_MOD_AGILITY : ITEM_MOD_INTELLECT);
        }
        case CLASS_PRIEST:
        case CLASS_MAGE:
        case CLASS_WARLOCK:
        {
            return ITEM_MOD_INTELLECT;
        }
    }
    return ITEM_MOD_STAMINA;
}

void CombatBotBaseAI::EquipRandomGearInEmptySlots()
{
    LearnArmorProficiencies();

    bool const onlyPvE = urand(0, 1) != 0;
    uint8 const honorRank = onlyPvE ? 0 : urand(5, 18);

    std::map<uint32 /*slot*/, std::vector<ItemPrototype const*>> itemsPerSlot;
    for (auto const& itr : sObjectMgr.GetItemPrototypeMap())
    {
        ItemPrototype const* pProto = &itr.second;

        if (!IsUsableGearCandidate(pProto, onlyPvE, honorRank))
            continue;

        // Don't equip cloth items on warriors, etc unless bot is a healer
        if (uint32 skill = pProto->GetProficiencySkill())
            if (pProto->Class == ITEM_CLASS_ARMOR &&
                pProto->InventoryType != INVTYPE_CLOAK &&
                pProto->InventoryType != INVTYPE_SHIELD &&
                skill != me->GetHighestKnownArmorProficiency() &&
                m_role != ROLE_HEALER)
                continue;

        uint8 slots[4];
        pProto->GetAllowedEquipSlots(slots, me->GetClass(), me->CanDualWield());

        for (uint8 slot : slots)
        {
            if (slot >= EQUIPMENT_SLOT_START && slot < EQUIPMENT_SLOT_END &&
                !me->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            {
                // Offhand checks
                if (slot == EQUIPMENT_SLOT_OFFHAND)
                {
                    // Only allow shield in offhand for tanks
                    if (pProto->InventoryType != INVTYPE_SHIELD &&
                        m_role == ROLE_TANK && IsShieldClass(me->GetClass()))
                        continue;

                    // Only equip holdables on mana users
                    if (pProto->InventoryType == INVTYPE_HOLDABLE &&
                        m_role != ROLE_HEALER && m_role != ROLE_RANGE_DPS)
                        continue;
                }

                itemsPerSlot[slot].push_back(pProto);

                // Unique item
                if (pProto->MaxCount == 1)
                    break;
            }
        }
    }

    // 1. Remove items that don't have our primary stat from the list
    // 2. Remove non-pvp items if we have a pvp item available
    uint32 const primaryStat = GetPrimaryItemStatForClassAndRole(me->GetClass(), m_role);
    for (auto& itr : itemsPerSlot)
    {
        bool hasPrimaryStatItem = false;

        for (auto const& pItem : itr.second)
        {
            for (auto const& stat : pItem->ItemStat)
            {
                if (stat.ItemStatType == primaryStat && stat.ItemStatValue > 0)
                {
                    hasPrimaryStatItem = true;
                    break;
                }
            }

            if (hasPrimaryStatItem)
                break;
        }

        if (hasPrimaryStatItem)
        {
            itr.second.erase(std::remove_if(itr.second.begin(), itr.second.end(),
            [primaryStat](ItemPrototype const* & pItem)
            {
                bool itemHasPrimaryStat = false;
                for (auto const& stat : pItem->ItemStat)
                {
                    if (stat.ItemStatType == primaryStat && stat.ItemStatValue > 0)
                    {
                        itemHasPrimaryStat = true;
                        break;
                    }
                }

                return !itemHasPrimaryStat;
            }),
                itr.second.end());
        }

        bool hasPvpItem = false;

        for (auto const& pItem : itr.second)
        {
            if (pItem->RequiredHonorRank)
            {
                hasPvpItem = true;
                break;
            }
        }

        if (hasPvpItem)
        {
            itr.second.erase(std::remove_if(itr.second.begin(), itr.second.end(),
                [](ItemPrototype const* & pItem)
            {
                return pItem->RequiredHonorRank == 0;
            }),
                itr.second.end());
        }
    }

    for (auto const& itr : itemsPerSlot)
    {
        // Don't equip offhand if using 2 handed weapon
        if (itr.first == EQUIPMENT_SLOT_OFFHAND)
        {
            if (Item* pMainHandItem = me->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND))
                if (pMainHandItem->GetProto()->InventoryType == INVTYPE_2HWEAPON)
                    continue;
        }

        if (itr.second.empty())
            continue;

        ItemPrototype const* pProto = SelectRandomContainerElement(itr.second);
        if (!pProto)
            continue;

        me->SatisfyItemRequirements(pProto);
        me->StoreNewItemInBestSlots(pProto->ItemId, 1);
    }
}

bool CombatBotBaseAI::IsUsableGearCandidate(ItemPrototype const* pProto, bool onlyPvE, uint8 honorRank) const
{
    // Only items that have already been discovered by someone
    if (!pProto->Discovered)
        return false;

    // Skip unobtainable items
    if (pProto->HasExtraFlag(ITEM_EXTRA_NOT_OBTAINABLE))
        return false;

    // Only gear and weapons
    if (pProto->Class != ITEM_CLASS_WEAPON && pProto->Class != ITEM_CLASS_ARMOR)
        return false;

    // No tabards and shirts
    if (pProto->InventoryType == INVTYPE_TABARD || pProto->InventoryType == INVTYPE_BODY)
        return false;

    if (pProto->SourceQuestRaces && !(pProto->SourceQuestRaces & me->GetRaceMask()))
        return false;

    if (pProto->SourceQuestClasses && !(pProto->SourceQuestClasses & me->GetClassMask()))
        return false;

    if (pProto->SourceQuestLevel < 0)
    {
        // Avoid higher level items with no level requirement
        if (!pProto->RequiredLevel && pProto->ItemLevel > me->GetLevel())
            return false;
    }
    else
    {
        // Item is from a high level quest
        if (uint32(pProto->SourceQuestLevel) > me->GetLevel())
            return false;
    }

    // Avoid low level items
    if ((pProto->ItemLevel + sWorld.getConfig(CONFIG_UINT32_PARTY_BOT_RANDOM_GEAR_LEVEL_DIFFERENCE)) < me->GetLevel())
        return false;

    if (me->CanUseItem(pProto, onlyPvE) != EQUIP_ERR_OK)
        return false;

    if (pProto->RequiredHonorRank > honorRank)
        return false;

    if (pProto->RequiredReputationFaction && uint32(me->GetReputationRank(pProto->RequiredReputationFaction)) < pProto->RequiredReputationRank)
        return false;

    if (uint32 skill = pProto->GetProficiencySkill())
    {
        // Fist weapons use unarmed skill calculations, but we must query fist weapon skill presence to use this item
        if (pProto->SubClass == ITEM_SUBCLASS_WEAPON_FIST)
            skill = SKILL_FIST_WEAPONS;
        if (!me->GetSkillValue(skill))
            return false;
    }

    return true;
}

namespace
{
    struct RoleGearWeights
    {
        float strength, agility, stamina, intellect, spirit, armor, meleeDps, rangedDps;
        bool caster; // casters and healers: no strength/agility items
    };

    RoleGearWeights GetRoleGearWeights(uint8 playerClass, CombatBotRoles role)
    {
        switch (role)
        {
            case ROLE_TANK:
                // Druids fight in bear/cat form: weapon damage does not count, only stats.
                if (playerClass == CLASS_DRUID)
                    return { 0.7f, 1.0f, 1.5f, 0.0f, 0.0f, 0.04f, 0.0f, 0.0f, false };
                return { 1.0f, 0.7f, 1.5f, playerClass == CLASS_PALADIN ? 0.2f : 0.0f, 0.0f, 0.04f, 1.0f, 0.2f, false };
            case ROLE_HEALER:
                return { 0.0f, 0.0f, 0.4f, 1.5f, 1.0f, 0.005f, 0.0f, 0.5f, true };
            case ROLE_RANGE_DPS:
                if (playerClass == CLASS_HUNTER)
                    return { 0.2f, 1.5f, 0.5f, 0.4f, 0.0f, 0.01f, 0.5f, 3.0f, false };
                return { 0.0f, 0.0f, 0.5f, 1.5f, 0.4f, 0.005f, 0.0f, 1.0f, true };
            default: // melee dps
                switch (playerClass)
                {
                    case CLASS_ROGUE:
                        return { 0.8f, 1.5f, 0.5f, 0.0f, 0.0f, 0.01f, 3.0f, 0.3f, false };
                    case CLASS_DRUID: // cat form: stats only
                        return { 0.8f, 1.5f, 0.5f, 0.0f, 0.0f, 0.01f, 0.0f, 0.0f, false };
                    case CLASS_SHAMAN:
                        return { 1.3f, 1.0f, 0.5f, 0.3f, 0.0f, 0.01f, 3.0f, 0.0f, false };
                    case CLASS_HUNTER:
                        return { 0.2f, 1.5f, 0.5f, 0.4f, 0.0f, 0.01f, 0.5f, 3.0f, false };
                    default:
                        return { 1.5f, 1.0f, 0.5f, 0.0f, 0.0f, 0.01f, 3.0f, 0.3f, false };
                }
        }
    }

    bool IsRangedSlotType(uint32 inventoryType)
    {
        return inventoryType == INVTYPE_RANGED || inventoryType == INVTYPE_RANGEDRIGHT || inventoryType == INVTYPE_THROWN;
    }

    bool IsOneHandWeaponType(uint32 inventoryType)
    {
        return inventoryType == INVTYPE_WEAPON || inventoryType == INVTYPE_WEAPONMAINHAND;
    }
}

float CombatBotBaseAI::GetRoleGearScore(ItemPrototype const* pProto) const
{
    RoleGearWeights const w = GetRoleGearWeights(me->GetClass(), m_role);

    // Junk: poor quality, profession gear (needs or raises a profession, e.g. fishing hats).
    if (pProto->Quality == ITEM_QUALITY_POOR || pProto->RequiredSkill)
        return -1.0f;
    for (auto const& itemSpell : pProto->Spells)
        if (SpellEntry const* pSpell = sSpellMgr.GetSpellEntry(itemSpell.SpellId))
            for (uint32 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                if (pSpell->EffectApplyAuraName[i] == SPELL_AURA_MOD_SKILL ||
                    pSpell->EffectApplyAuraName[i] == SPELL_AURA_MOD_SKILL_TALENT)
                    return -1.0f;

    // Armor type: the best one the class knows (cloaks, shields, jewelry, off-hands excluded).
    if (pProto->Class == ITEM_CLASS_ARMOR && pProto->InventoryType != INVTYPE_CLOAK &&
        pProto->InventoryType != INVTYPE_SHIELD)
        if (uint32 skill = pProto->GetProficiencySkill())
            if (skill != me->GetHighestKnownArmorProficiency())
                return -1.0f;

    // Shields: tanks, and casters/healers of shield classes. Off-hand frills: casters only.
    if (pProto->InventoryType == INVTYPE_SHIELD && m_role != ROLE_TANK && !w.caster)
        return -1.0f;
    if (pProto->InventoryType == INVTYPE_HOLDABLE && !w.caster)
        return -1.0f;

    float strength = 0, agility = 0, stamina = 0, intellect = 0, spirit = 0;
    for (auto const& stat : pProto->ItemStat)
    {
        switch (stat.ItemStatType)
        {
            case ITEM_MOD_STRENGTH: strength += stat.ItemStatValue; break;
            case ITEM_MOD_AGILITY: agility += stat.ItemStatValue; break;
            case ITEM_MOD_STAMINA: stamina += stat.ItemStatValue; break;
            case ITEM_MOD_INTELLECT: intellect += stat.ItemStatValue; break;
            case ITEM_MOD_SPIRIT: spirit += stat.ItemStatValue; break;
        }
    }

    // Wrong stats: casters never take strength/agility; physical roles never take caster-only items.
    if (w.caster && (strength > 0 || agility > 0))
        return -1.0f;
    if (!w.caster && w.intellect < 0.3f && (intellect > 0 || spirit > 0) && strength <= 0 && agility <= 0)
        return -1.0f;

    float const statScore = strength * w.strength + agility * w.agility + stamina * w.stamina +
                            intellect * w.intellect + spirit * w.spirit;

    // Trinkets only when they actually help the role (no pure on-use novelty trinkets).
    if (pProto->InventoryType == INVTYPE_TRINKET && statScore <= 0)
        return -1.0f;

    float score = statScore + pProto->Armor * w.armor;

    if (pProto->Class == ITEM_CLASS_WEAPON && pProto->Delay)
    {
        float damage = 0;
        for (auto const& dmg : pProto->Damage)
            damage += (dmg.DamageMin + dmg.DamageMax) / 2.0f;
        float const dps = damage * 1000.0f / pProto->Delay;
        score += dps * (IsRangedSlotType(pProto->InventoryType) ? w.rangedDps : w.meleeDps);
    }

    return score;
}

// Picks scored gear for every slot and replaces what the bot wears. Level 60 and up: the
// class/role pre-raid BiS template when one exists (never the raid BiS one).
void CombatBotBaseAI::EquipRoleGear()
{
    LearnArmorProficiencies();

    bool const maxLevel = me->GetLevel() >= sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL);
    if (maxLevel)
    {
        for (auto const& itr : sObjectMgr.GetPlayerPremadeGearTemplates())
        {
            if (itr.second.requiredClass == me->GetClass() && itr.second.role == m_role &&
                itr.second.level == me->GetLevel() && itr.second.name.find("preraid") != std::string::npos)
            {
                for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
                    if (me->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                        me->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
                sObjectMgr.ApplyPremadeGearTemplateToPlayer(itr.first, me);
                sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Gear] %s: pre-raid set '%s'", me->GetName(), itr.second.name.c_str());
                return;
            }
        }
    }

    // Candidates per slot, best first.
    std::map<uint8, std::vector<std::pair<float, ItemPrototype const*>>> candidates;
    for (auto const& itr : sObjectMgr.GetItemPrototypeMap())
    {
        ItemPrototype const* pProto = &itr.second;

        if (!IsUsableGearCandidate(pProto, true, 0))
            continue;

        // No pre-raid set for this spec at max level: keep raid gear out.
        if (maxLevel && pProto->ItemLevel > 63)
            continue;

        float const score = GetRoleGearScore(pProto);
        if (score < 0)
            continue;

        uint8 slots[4];
        pProto->GetAllowedEquipSlots(slots, me->GetClass(), me->CanDualWield());
        for (uint8 slot : slots)
            if (slot >= EQUIPMENT_SLOT_START && slot < EQUIPMENT_SLOT_END)
                candidates[slot].push_back({ score, pProto });
    }
    for (auto& itr : candidates)
        std::sort(itr.second.begin(), itr.second.end(),
            [](std::pair<float, ItemPrototype const*> const& a, std::pair<float, ItemPrototype const*> const& b) { return a.first > b.first; });

    // One of the top 3 items within 90% of the best score (a little variety); filter decides
    // which items of the slot qualify; excluded item ids are skipped (unique second ring etc.).
    auto pick = [&](uint8 slot, auto filter, uint32 excludeId) -> std::pair<float, ItemPrototype const*>
    {
        std::vector<std::pair<float, ItemPrototype const*>> top;
        for (auto const& entry : candidates[slot])
        {
            if (entry.second->ItemId == excludeId || !filter(entry.second))
                continue;
            if (!top.empty() && entry.first < top.front().first * 0.9f)
                break;
            top.push_back(entry);
            if (top.size() == 3)
                break;
        }
        if (top.empty())
            return { 0.0f, nullptr };
        return SelectRandomContainerElement(top);
    };
    auto any = [](ItemPrototype const*) { return true; };


    // Weapons by role.
    RoleGearWeights const w = GetRoleGearWeights(me->GetClass(), m_role);
    auto is2H = [](ItemPrototype const* p) { return p->InventoryType == INVTYPE_2HWEAPON; };
    auto is1H = [](ItemPrototype const* p) { return IsOneHandWeaponType(p->InventoryType); };
    auto isShield = [](ItemPrototype const* p) { return p->InventoryType == INVTYPE_SHIELD; };
    // Off-hand weapons only for classes that can dual wield (druids never).
    bool const canDualWield = me->CanDualWield();
    auto offhandForRole = [&](ItemPrototype const* p)
    {
        if (w.caster)
            return p->InventoryType == INVTYPE_HOLDABLE || p->InventoryType == INVTYPE_SHIELD;
        return canDualWield && (p->InventoryType == INVTYPE_WEAPON || p->InventoryType == INVTYPE_WEAPONOFFHAND);
    };

    auto const twoHand = pick(EQUIPMENT_SLOT_MAINHAND, is2H, 0);
    auto const oneHand = pick(EQUIPMENT_SLOT_MAINHAND, is1H, 0);
    ItemPrototype const* pNewMainHand = nullptr;
    ItemPrototype const* pNewOffHand = nullptr;
    float newWeaponScore = 0.0f;
    if (m_role == ROLE_TANK && IsShieldClass(me->GetClass()))
    {
        // Tanks with shields: always one-hand + shield.
        auto const shield = pick(EQUIPMENT_SLOT_OFFHAND, isShield, 0);
        pNewMainHand = oneHand.second;
        pNewOffHand = shield.second;
        newWeaponScore = oneHand.first + shield.first;
    }
    else
    {
        auto const offHand = pick(EQUIPMENT_SLOT_OFFHAND, offhandForRole,
            oneHand.second && oneHand.second->MaxCount == 1 ? oneHand.second->ItemId : 0);
        float const pairScore = oneHand.first + offHand.first;

        // Melee that can use both: a fixed per-bot mix of two-handers and dual wielding (two
        // weapons always out-score one, so a pure score comparison would make everyone dual wield).
        bool useTwoHand = twoHand.second && twoHand.first >= pairScore;
        if (!w.caster && canDualWield && twoHand.second && oneHand.second && offHand.second)
        {
            if (!m_weaponStyle)
                m_weaponStyle = urand(1, 2);
            useTwoHand = m_weaponStyle == 1;
        }

        if (useTwoHand)
        {
            pNewMainHand = twoHand.second;
            newWeaponScore = twoHand.first;
        }
        else
        {
            pNewMainHand = oneHand.second;
            pNewOffHand = offHand.second;
            newWeaponScore = pairScore;
        }
    }

    // A slot is only changed when the new item scores higher than what is worn (empty or
    // unsuitable counts as -1), so nothing good is ever taken away.
    auto wornScore = [&](uint8 slot) -> float
    {
        Item* pItem = me->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!pItem)
            return -1.0f;
        return std::max(-1.0f, GetRoleGearScore(pItem->GetProto()));
    };
    auto equip = [&](uint8 slot, ItemPrototype const* pProto)
    {
        if (me->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            me->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
        if (!pProto)
            return false;
        me->SatisfyItemRequirements(pProto);
        uint16 dest;
        return me->CanEquipNewItem(slot, dest, pProto->ItemId, false) == EQUIP_ERR_OK &&
               me->EquipNewItem(dest, pProto->ItemId, true);
    };

    uint32 changed = 0;

    // Weapons as one set (two-hander vs one-hand + off-hand).
    float const wornWeaponScore = wornScore(EQUIPMENT_SLOT_MAINHAND) +
        (me->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND) ? wornScore(EQUIPMENT_SLOT_OFFHAND) : 0.0f);
    if (pNewMainHand && newWeaponScore > wornWeaponScore)
    {
        changed += equip(EQUIPMENT_SLOT_MAINHAND, pNewMainHand);
        changed += equip(EQUIPMENT_SLOT_OFFHAND, pNewOffHand);
    }

    // Everything else; rings and trinkets twice without the same item twice.
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        if (slot == EQUIPMENT_SLOT_MAINHAND || slot == EQUIPMENT_SLOT_OFFHAND)
            continue;

        uint32 exclude = 0;
        if (slot == EQUIPMENT_SLOT_FINGER2 || slot == EQUIPMENT_SLOT_TRINKET2)
            if (Item* pFirst = me->GetItemByPos(INVENTORY_SLOT_BAG_0, slot - 1))
                exclude = pFirst->GetEntry();

        auto const item = pick(slot, any, exclude);
        if (item.second && item.first > wornScore(slot))
            changed += equip(slot, item.second);
    }

    sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Gear] %s: %u slots upgraded for %s at level %u", me->GetName(),
        changed, GetRoleName(m_role), me->GetLevel());
}

void CombatBotBaseAI::AutoEquipGear(uint32 option)
{
    switch (option)
    {
        case PLAYER_BOT_AUTO_EQUIP_STARTING_GEAR:
            me->AddStartingItems();
            break;
        case PLAYER_BOT_AUTO_EQUIP_RANDOM_GEAR:
            EquipRandomGearInEmptySlots();
            break;
        case PLAYER_BOT_AUTO_EQUIP_PREMADE_GEAR:
            EquipPremadeGearTemplate();
            break;
    }

    UpdateVisualHonorRankBasedOnItems();
}

// A cast-time nuke whose direct damage is bigger than the aura it leaves (Frostbolt's slow,
// Fireball's and Pyroblast's burn): worth casting again while that aura is still on the target.
// Damage-over-time-first spells (Immolate) and instants (Hamstring, Frost Shock) are not.
static bool IsNukeWithSideAura(SpellEntry const* pSpellEntry, Player const* pCaster)
{
    if (!pSpellEntry->GetCastTime(pCaster))
        return false;

    int32 direct = 0;
    int32 periodic = 0;
    for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        SpellEffectIndex const eff = SpellEffectIndex(i);
        if (pSpellEntry->Effect[i] == SPELL_EFFECT_SCHOOL_DAMAGE)
            direct += pSpellEntry->CalculateSimpleValue(eff);
        else if (Spells::IsEffectAppliesAura(pSpellEntry->Effect[i]) &&
                 pSpellEntry->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_DAMAGE &&
                 pSpellEntry->EffectAmplitude[i])
            periodic += pSpellEntry->CalculateSimpleValue(eff) *
                (pSpellEntry->GetDuration() / int32(pSpellEntry->EffectAmplitude[i]));
    }
    return direct > 0 && direct > periodic;
}

bool CombatBotBaseAI::CanTryToCastSpell(Unit const* pTarget, SpellEntry const* pSpellEntry) const
{
    if (m_preventCasting)
        return false;

    if (!me->IsSpellReady(pSpellEntry))
        return false;

    if (me->HasGCD(pSpellEntry))
        return false;

    if (pSpellEntry->TargetAuraState &&
       !pTarget->HasAuraState(AuraState(pSpellEntry->TargetAuraState)))
        return false;

    if (pSpellEntry->CasterAuraState &&
        !me->HasAuraState(AuraState(pSpellEntry->CasterAuraState)))
        return false;

    uint32 const powerCost = Spell::CalculatePowerCost(pSpellEntry, me);
    Powers const powerType = Powers(pSpellEntry->powerType);

    if (powerType == POWER_HEALTH)
    {
        if (me->GetHealth() <= powerCost)
            return false;
        return true;
    }

    if (me->GetPower(powerType) < powerCost)
        return false;

    if (pTarget->IsImmuneToSpell(pSpellEntry, false))
        return false;

    if (pSpellEntry->GetErrorAtShapeshiftedCast(me->GetShapeshiftForm()) != SPELL_CAST_OK)
        return false;

    // Aura already on the target: not again. Exceptions: stacking warrior debuffs (Sunder
    // Armor; the caller decides on stacks and refresh) and nukes with a side aura (Frostbolt).
    if (pSpellEntry->IsSpellAppliesAura() && pTarget->HasAura(pSpellEntry->Id) &&
       !(pSpellEntry->SpellFamilyName == SPELLFAMILY_WARRIOR && pSpellEntry->GetStackAmount() > 1) &&
       !IsNukeWithSideAura(pSpellEntry, me))
        return false;

    SpellRangeEntry const* srange = sSpellRangeStore.LookupEntry(pSpellEntry->rangeIndex);
    if (me != pTarget && pSpellEntry->EffectImplicitTargetA[0] != TARGET_UNIT_CASTER)
    {
        float const dist = me->GetCombatDistance(pTarget);

        if (dist > srange->maxRange)
            return false;
        if (srange->minRange && dist < srange->minRange)
            return false;
    }

    return true;
}

std::string CombatBotBaseAI::GetLogName(Unit const* pUnit)
{
    if (!pUnit->IsCreature())
        return pUnit->GetName();
    return std::string(pUnit->GetName()) + "#" + std::to_string(pUnit->GetGUIDLow());
}

SpellCastResult CombatBotBaseAI::DoCastSpell(Unit* pTarget, SpellEntry const* pSpellEntry)
{
    if (m_preventCasting)
        return SPELL_FAILED_DONT_REPORT;

    if (me != pTarget)
        me->SetFacingToObject(pTarget);

    if (me->IsMounted())
        me->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);

    me->SetTargetGuid(pTarget->GetObjectGuid());
    EnsureSpellReagents(pSpellEntry);
    auto result = me->CastSpell(pTarget, pSpellEntry, false);

    if (result == SPELL_CAST_OK)
    {
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Cast] %s: %s (%u) on %s", me->GetName(),
            pSpellEntry->SpellName[0].c_str(), pSpellEntry->Id, GetLogName(pTarget).c_str());
        OnCastStarted(pTarget, pSpellEntry);
    }

    if ((result == SPELL_FAILED_MOVING ||
        result == SPELL_CAST_OK) &&
        (pSpellEntry->GetCastTime(me) > 0) &&
        (me->IsMoving() || !me->IsStopped()))
        me->StopMoving();

    return result;
}

bool CombatBotBaseAI::AddItemToInventory(uint32 itemId, uint32 count)
{
    ItemPosCountVec dest;
    uint8 msg = me->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemId, count);
    if (msg != EQUIP_ERR_OK)
        return false;

    // StoreNewItem already splits the count over the stacks it merges into.
    return me->StoreNewItem(dest, itemId, true, Item::GenerateItemRandomPropertyId(itemId)) != nullptr;
}

// Ammo, quivers and ammo pouches bots may use (user 2026-11-11): any source - vendor, quest,
// crafted, dungeon or world drop - but never raid loot, epics, test/deprecated/monster items,
// items nothing gives, or Alliance-only quest rewards (bots are Horde). Checked against the DB.
// On an item-level tie the earlier entry wins (Hunting Quiver before Medium Quiver).
static uint32 const PB_ALLOWED_ARROWS[] = { 2512, 2515, 3030, 9399, 10579, 11285, 19316, 18042, 12654 };
static uint32 const PB_ALLOWED_BULLETS[] = { 2516, 8067, 2519, 5568, 8068, 3033, 8069, 3034, 10512, 11284,
    10513, 11630, 19317, 15997, 13377 };
static uint32 const PB_ALLOWED_QUIVERS[] = { 2101, 5439, 7278, 3573, 11362, 7371, 8217, 2662, 19319 };
static uint32 const PB_ALLOWED_POUCHES[] = { 5441, 7279, 3574, 7372, 8218, 2663, 19320 };

// Usable at this level: the required level, or for items without one, 5 below the item level.
static bool IsSupplyLevelOk(ItemPrototype const* pProto, uint32 level)
{
    if (pProto->RequiredLevel)
        return pProto->RequiredLevel <= level;
    return pProto->ItemLevel <= level + 5;
}

// Ammo type (arrow/bullet) the equipped ranged weapon fires, 0 = none (wand, thrown, no weapon).
static uint32 GetRangedAmmoType(Player const* pPlayer)
{
    Item* pWeapon = pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
    if (!pWeapon || pWeapon->GetProto()->Class != ITEM_CLASS_WEAPON)
        return 0;

    switch (pWeapon->GetProto()->SubClass)
    {
        case ITEM_SUBCLASS_WEAPON_GUN:
            return ITEM_SUBCLASS_BULLET;
        case ITEM_SUBCLASS_WEAPON_BOW:
        case ITEM_SUBCLASS_WEAPON_CROSSBOW:
            return ITEM_SUBCLASS_ARROW;
    }
    return 0;
}

// Free ammo: the best allowed arrows/bullets for the bot's level (highest item level), topped
// up to a full stack. Ammo of other kinds (older level, other weapon) is removed; nothing else.
void CombatBotBaseAI::AddRangedAmmo()
{
    uint32 const ammoType = GetRangedAmmoType(me);
    if (!ammoType)
        return;

    ItemPrototype const* pAmmoProto = nullptr;
    auto pickAmmo = [&](uint32 itemId)
    {
        ItemPrototype const* pProto = sObjectMgr.GetItemPrototype(itemId);
        if (pProto && IsSupplyLevelOk(pProto, me->GetLevel()) &&
            (!pAmmoProto || pAmmoProto->ItemLevel < pProto->ItemLevel) &&
            me->CanUseAmmo(itemId) == EQUIP_ERR_OK)
            pAmmoProto = pProto;
    };
    if (ammoType == ITEM_SUBCLASS_BULLET)
        for (uint32 itemId : PB_ALLOWED_BULLETS)
            pickAmmo(itemId);
    else
        for (uint32 itemId : PB_ALLOWED_ARROWS)
            pickAmmo(itemId);

    if (!pAmmoProto)
        return;

    // Remove other ammo (backpack and bags, incl. quiver/pouch).
    auto destroyOtherAmmo = [this, pAmmoProto](uint8 bag, uint8 slot)
    {
        Item* pItem = me->GetItemByPos(bag, slot);
        if (pItem && pItem->GetProto()->Class == ITEM_CLASS_PROJECTILE && pItem->GetEntry() != pAmmoProto->ItemId)
            me->DestroyItem(bag, slot, true);
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        destroyOtherAmmo(INVENTORY_SLOT_BAG_0, slot);
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        if (Bag* pBag = dynamic_cast<Bag*>(me->GetItemByPos(INVENTORY_SLOT_BAG_0, bag)))
            for (uint8 slot = 0; slot < pBag->GetBagSize(); ++slot)
                destroyOtherAmmo(bag, slot);

    uint32 const have = me->GetItemCount(pAmmoProto->ItemId);
    uint32 const stack = pAmmoProto->GetMaxStackSize();
    if (have < stack)
    {
        AddItemToInventory(pAmmoProto->ItemId, stack - have);
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Supply] %s: gets %u %s (had %u)", me->GetName(),
            stack - have, pAmmoProto->Name1, have);
    }
    me->SetAmmo(pAmmoProto->ItemId);
}

// Hunters: the best allowed quiver (bow/crossbow) or ammo pouch (gun) for the level, in a bag
// slot (ranged attack speed bonus). A different quiver/pouch is replaced; ammo is refilled after.
void CombatBotBaseAI::EquipAmmoBag()
{
    if (me->GetClass() != CLASS_HUNTER)
        return;

    uint32 const ammoType = GetRangedAmmoType(me);
    if (!ammoType)
        return;
    uint32 const bagType = ammoType == ITEM_SUBCLASS_BULLET ? ITEM_SUBCLASS_AMMO_POUCH : ITEM_SUBCLASS_QUIVER;

    ItemPrototype const* pBagProto = nullptr;
    auto pickBag = [&](uint32 itemId)
    {
        ItemPrototype const* pProto = sObjectMgr.GetItemPrototype(itemId);
        if (pProto && IsSupplyLevelOk(pProto, me->GetLevel()) &&
            (!pBagProto || pBagProto->ItemLevel < pProto->ItemLevel ||
             (pBagProto->ItemLevel == pProto->ItemLevel && pBagProto->ContainerSlots < pProto->ContainerSlots)))
            pBagProto = pProto;
    };
    if (bagType == ITEM_SUBCLASS_AMMO_POUCH)
        for (uint32 itemId : PB_ALLOWED_POUCHES)
            pickBag(itemId);
    else
        for (uint32 itemId : PB_ALLOWED_QUIVERS)
            pickBag(itemId);
    if (!pBagProto)
        return;

    // Already carried? Otherwise remove the old quiver/pouch (only ammo inside, refilled after).
    for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
    {
        Item* pBag = me->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!pBag || pBag->GetProto()->Class != ITEM_CLASS_QUIVER)
            continue;
        if (pBag->GetEntry() == pBagProto->ItemId)
            return;
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Supply] %s: removes %s", me->GetName(), pBag->GetProto()->Name1);
        me->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
    }

    uint8 const slot = me->FindEquipSlot(pBagProto, NULL_SLOT, false);
    uint16 dest;
    if (slot == NULL_SLOT || me->CanEquipNewItem(slot, dest, pBagProto->ItemId, false) != EQUIP_ERR_OK ||
        !me->EquipNewItem(dest, pBagProto->ItemId, true))
    {
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Supply] %s: cannot equip %s (no free bag slot)", me->GetName(),
            pBagProto->Name1);
        return;
    }

    uint32 const hasteSpell = pBagProto->Spells[0].SpellId;
    sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Supply] %s: equips %s (%u slots, haste aura %u %s)", me->GetName(),
        pBagProto->Name1, pBagProto->ContainerSlots, hasteSpell,
        hasteSpell && me->HasAura(hasteSpell) ? "active" : "MISSING");
}

// The quiver/pouch haste aura can be missing right after equipping (seen on bot add). Re-apply
// the item's equip spell once and log the result; tried again only after the aura was seen.
void CombatBotBaseAI::CheckAmmoBagHaste()
{
    if (me->GetClass() != CLASS_HUNTER || !me->IsAlive())
        return;

    for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
    {
        Item* pBag = me->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!pBag || pBag->GetProto()->Class != ITEM_CLASS_QUIVER)
            continue;

        uint32 const hasteSpell = pBag->GetProto()->Spells[0].SpellId;
        if (!hasteSpell || me->HasAura(hasteSpell))
        {
            m_ammoBagHasteRetried = false;
            return;
        }
        if (m_ammoBagHasteRetried)
            return;

        m_ammoBagHasteRetried = true;
        me->ApplyItemEquipSpell(pBag, false);
        me->ApplyItemEquipSpell(pBag, true);
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Supply] %s: haste aura %u of %s was missing, re-applied: %s",
            me->GetName(), hasteSpell, pBag->GetProto()->Name1, me->HasAura(hasteSpell) ? "active" : "STILL MISSING");
        return;
    }
}

#define PB_TRADE_BUFF_MIN_DURATION (10 * MINUTE * IN_MILLISECONDS)

// On-use spell of a traded consumable that the bot uses at once: a long buff (elixir, flask,
// scroll, protection potion; >= 10 min). Heals, mana, food/drink, bandages and short buffs are kept.
static SpellEntry const* GetTradeBuffSpell(ItemPrototype const* pProto)
{
    for (auto const& itr : pProto->Spells)
    {
        if (!itr.SpellId || itr.SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
            continue;
        SpellEntry const* pSpellEntry = sSpellMgr.GetSpellEntry(itr.SpellId);
        if (!pSpellEntry || !pSpellEntry->IsSpellAppliesAura() ||
            pSpellEntry->HasEffect(SPELL_EFFECT_HEAL) || pSpellEntry->HasEffect(SPELL_EFFECT_ENERGIZE) ||
            pSpellEntry->HasAura(SPELL_AURA_PERIODIC_HEAL) || pSpellEntry->HasAura(SPELL_AURA_PERIODIC_ENERGIZE) ||
            pSpellEntry->HasAura(SPELL_AURA_MOD_REGEN) || pSpellEntry->HasAura(SPELL_AURA_MOD_POWER_REGEN) ||
            pSpellEntry->GetDuration() < PB_TRADE_BUFF_MIN_DURATION)
            return nullptr;
        return pSpellEntry;
    }
    return nullptr;
}

#define PB_SPELL_PICK_LOCK 1804

// Rogue: the trader put a locked item in the "will not be traded" slot. Like a player's client,
// cast Pick Lock on that slot: the core checks the lock and skill, remembers it as this side's
// trade spell and casts it when both accept (the item stays with its owner).
void CombatBotBaseAI::PickLockInTrade()
{
    TradeData* pTrade = me->GetTradeData();
    if (!pTrade || pTrade->GetSpell() || !me->HasSpell(PB_SPELL_PICK_LOCK))
        return;

    Item* pItem = pTrade->GetTraderData()->GetItem(TRADE_SLOT_NONTRADED);
    if (!pItem || !pItem->GetProto()->LockID || pItem->HasFlag(ITEM_FIELD_FLAGS, ITEM_DYNFLAG_UNLOCKED))
        return;

    SpellEntry const* pSpellEntry = sSpellMgr.GetSpellEntry(PB_SPELL_PICK_LOCK);
    if (!pSpellEntry)
        return;

    // Pick Lock needs Thieves' Tools in the bags (spell totem item), free like other supplies.
    EnsureSpellReagents(pSpellEntry);

    SpellCastTargets targets;
    targets.setTradeItemTarget(me);
    SpellCastResult const result = (new Spell(me, pSpellEntry, false))->prepare(std::move(targets));

    if (pTrade->GetSpell() == PB_SPELL_PICK_LOCK)
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Supply] %s: will pick the lock of %s when the trade is accepted",
            me->GetName(), pItem->GetProto()->Name1);
    else
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Supply] %s: cannot pick the lock of %s (Lockpicking %u, cast result %u)",
            me->GetName(), pItem->GetProto()->Name1, me->GetSkillValue(SKILL_LOCKPICKING), uint32(result));
}

#define PB_SUPPLY_TRADE_COUNT 20

void CombatBotBaseAI::FillSupplyTrade()
{
    if (me->GetClass() != CLASS_MAGE && me->GetClass() != CLASS_WARLOCK)
        return;
    TradeData* pTrade = me->GetTradeData();
    Player* pTrader = pTrade ? pTrade->GetTrader() : nullptr;
    if (!pTrader)
        return;

    uint8 tradeSlot = 0;
    std::string names;
    auto offerItem = [&](uint32 itemId, uint32 count)
    {
        ItemPrototype const* pProto = sObjectMgr.GetItemPrototype(itemId);
        if (!pProto)
            return;
        if (pProto->MaxCount && pTrader->GetItemCount(itemId) >= pProto->MaxCount)
        {
            sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Supply] %s: %s already has %s", me->GetName(),
                pTrader->GetName(), pProto->Name1);
            return;
        }
        uint32 const have = me->GetItemCount(itemId);
        if (have < count)
            AddItemToInventory(itemId, count - have);
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        {
            Item* pItem = me->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (pItem && pItem->GetEntry() == itemId && pItem->GetCount() >= count)
            {
                auto data = std::make_unique<WorldPackets::Trade::SetTradeItem>();
                data->tradeSlot = tradeSlot++;
                data->bag = INVENTORY_SLOT_BAG_0;
                data->slot = slot;
                me->GetSession()->QueuePacket(std::move(data));
                if (!names.empty())
                    names += ", ";
                names += std::to_string(count) + " " + pProto->Name1;
                return;
            }
        }
    };

    auto offerConjured = [&](char const* spellPrefix, uint32 count)
    {
        if (SpellEntry const* pSpellEntry = GetBestCreateItemSpell(spellPrefix))
            offerItem(GetCreatedItemId(pSpellEntry), count);
        else
            sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Supply] %s: no %s spell learned", me->GetName(), spellPrefix);
    };

    if (me->GetClass() == CLASS_MAGE)
    {
        offerConjured("Conjure Food", PB_SUPPLY_TRADE_COUNT);
        // Water only for mana users (by class: a druid in bear form still gets it).
        if (pTrader->GetClass() != CLASS_WARRIOR && pTrader->GetClass() != CLASS_ROGUE)
            offerConjured("Conjure Water", PB_SUPPLY_TRADE_COUNT);
    }
    else
    {
        // Healthstone by warlock level (trainer levels of Create Healthstone). Not via the learned
        // spell: in 1.12 every Create Healthstone rank is a script effect (picks the item by the
        // Improved Healthstone talent), so GetBestCreateItemSpell cannot see its item.
        static struct { uint32 minLevel, itemId; } const healthstones[] =
            { { 58, 9421 }, { 46, 5510 }, { 34, 5509 }, { 22, 5511 }, { 10, 5512 } };
        for (auto const& entry : healthstones)
        {
            if (me->GetLevel() >= entry.minLevel)
            {
                offerItem(entry.itemId, 1);
                break;
            }
        }
    }

    if (!names.empty())
        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Supply] %s: puts %s in the trade with %s", me->GetName(),
            names.c_str(), pTrader->GetName());
}

SpellEntry const* CombatBotBaseAI::GetBestCreateItemSpell(char const* namePrefix) const
{
    SpellEntry const* pBest = nullptr;
    for (auto const& itr : me->GetSpellMap())
    {
        if (itr.second.state == PLAYERSPELL_REMOVED || itr.second.disabled)
            continue;
        SpellEntry const* pSpellEntry = sSpellMgr.GetSpellEntry(itr.first);
        if (!pSpellEntry || pSpellEntry->SpellName[0].rfind(namePrefix, 0) != 0 || !GetCreatedItemId(pSpellEntry))
            continue;
        if (!pBest || pBest->spellLevel < pSpellEntry->spellLevel)
            pBest = pSpellEntry;
    }
    return pBest;
}

uint32 CombatBotBaseAI::GetCreatedItemId(SpellEntry const* pSpellEntry)
{
    for (uint32 i = 0; i < MAX_EFFECT_INDEX; ++i)
        if (pSpellEntry->Effect[i] == SPELL_EFFECT_CREATE_ITEM && pSpellEntry->EffectItemType[i])
            return uint32(pSpellEntry->EffectItemType[i]);
    return 0;
}

void CombatBotBaseAI::EquipOrUseNewItem()
{
    std::unordered_set<uint32> usedSpells; // one of each buff per trade
    for (int i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
    {
        Item* pItem = me->GetItemByPos(INVENTORY_SLOT_BAG_0, i);
        if (pItem && !pItem->IsEquipped())
        {
            switch (pItem->GetProto()->Class)
            {
                case ITEM_CLASS_CONSUMABLE:
                {
                    // Was: every consumable in the backpack used at once (potions wasted at full health).
                    SpellEntry const* pBuff = GetTradeBuffSpell(pItem->GetProto());
                    if (!pBuff || me->HasAura(pBuff->Id) || !usedSpells.insert(pBuff->Id).second)
                    {
                        sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Supply] %s: keeps %u %s in the bag", me->GetName(),
                            pItem->GetCount(), pItem->GetProto()->Name1);
                        break;
                    }
                    SpellCastTargets targets;
                    targets.setUnitTarget(me);
                    sLog.Out(LOG_PLAYERBOTS, LOG_LVL_BASIC, "[Supply] %s: uses traded %s", me->GetName(),
                        pItem->GetProto()->Name1);
                    me->CastItemUseSpell(pItem, targets);
                    break;
                }
                case ITEM_CLASS_WEAPON:
                case ITEM_CLASS_ARMOR:
                {
                    uint32 slot = me->FindEquipSlot(pItem->GetProto(), NULL_SLOT, true);
                    if (slot != NULL_SLOT)
                    {
                        if (Item* pItem2 = me->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                            me->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);

                        // Learn required proficiency
                        if (uint32 proficiencySpellId = pItem->GetProto()->GetProficiencySpell())
                            if (!me->HasSpell(proficiencySpellId))
                                me->LearnSpell(proficiencySpellId, false, false);

                        me->RemoveItem(INVENTORY_SLOT_BAG_0, i, false);
                        me->EquipItem(slot, pItem, true);
                    }
                    break;
                }
            }
        }
    }
}

uint8 CombatBotBaseAI::GetHighestHonorRankFromEquippedItems() const
{
    uint8 maxRank = 0;
    for (int i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
    {
        if (Item* pItem = me->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            if (pItem->GetProto()->RequiredHonorRank > maxRank)
                maxRank = pItem->GetProto()->RequiredHonorRank;
        }
    }
    return maxRank;
}

void CombatBotBaseAI::UpdateVisualHonorRankBasedOnItems()
{
    uint8 rank = GetHighestHonorRankFromEquippedItems();
    if (rank > m_visualHonorRank)
        m_visualHonorRank = rank;

    // This is purely visual.
    me->SetByteValue(PLAYER_BYTES_3, PLAYER_BYTES_3_OFFSET_HONOR_RANK, m_visualHonorRank);
    me->SetByteValue(PLAYER_FIELD_BYTES, PLAYER_FIELD_BYTES_OFFSET_HIGHEST_HONOR_RANK, m_visualHonorRank);
}

void CombatBotBaseAI::BeginChasing(Unit* pVictim) const
{
    if (!CanMoveOnOwn())
        return;

    if ((m_role == ROLE_RANGE_DPS || m_role == ROLE_HEALER) &&
        IsRangedDamageClass(me->GetClass()) &&
       !IsAttackSpeedOverridenForm(me->GetShapeshiftForm()) &&
       (me->GetPowerPercent(POWER_MANA) > 10.0f || me->GetWeaponForAttack(RANGED_ATTACK, true, true)))
        me->SetCasterChaseDistance(25.0f);
    else if (me->HasDistanceCasterMovement())
        me->SetCasterChaseDistance(0.0f);

    // we use dist = 1 always so we can specify angle, instead of spreading around target like mobs
    me->GetMotionMaster()->MoveChase(pVictim, 1.0f, m_role == ROLE_MELEE_DPS ? M_PI_F : 0.0f);
}

bool CombatBotBaseAI::SummonShamanTotems()
{
    if (m_spells.shaman.pAirTotem &&
        !me->GetTotem(TOTEM_SLOT_AIR) &&
        CanTryToCastSpell(me, m_spells.shaman.pAirTotem))
    {
        if (DoCastSpell(me, m_spells.shaman.pAirTotem) == SPELL_CAST_OK)
            return true;
    }

    if (m_spells.shaman.pEarthTotem &&
        !me->GetTotem(TOTEM_SLOT_EARTH) &&
        CanTryToCastSpell(me, m_spells.shaman.pEarthTotem))
    {
        if (DoCastSpell(me, m_spells.shaman.pEarthTotem) == SPELL_CAST_OK)
            return true;
    }

    if (m_spells.shaman.pFireTotem &&
        !me->GetTotem(TOTEM_SLOT_FIRE) &&
        CanTryToCastSpell(me, m_spells.shaman.pFireTotem))
    {
        if (DoCastSpell(me, m_spells.shaman.pFireTotem) == SPELL_CAST_OK)
            return true;
    }

    if (m_spells.shaman.pWaterTotem &&
        !me->GetTotem(TOTEM_SLOT_WATER) &&
        CanTryToCastSpell(me, m_spells.shaman.pWaterTotem))
    {
        if (DoCastSpell(me, m_spells.shaman.pWaterTotem) == SPELL_CAST_OK)
            return true;
    }

    return false;
}

SpellCastResult CombatBotBaseAI::CastWeaponBuff(SpellEntry const* pSpellEntry, EquipmentSlots slot)
{
    Item* pWeapon = me->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
    if (!pWeapon)
        return SPELL_FAILED_ITEM_NOT_FOUND;
    if (pWeapon->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT))
        return SPELL_FAILED_ITEM_ALREADY_ENCHANTED;

    Spell* spell = new Spell(me, pSpellEntry, true, ObjectGuid(), nullptr, nullptr, nullptr);
    SpellCastTargets targets;
    targets.setItemTarget(pWeapon);
    return spell->prepare(std::move(targets), nullptr);
}

bool CombatBotBaseAI::UseTrinketEffects(bool onlyToBreakCC)
{
    if (Item* pItem = me->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_TRINKET1))
        if (UseItemEffect(pItem, onlyToBreakCC))
            return true;
    if (Item* pItem = me->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_TRINKET2))
        if (UseItemEffect(pItem, onlyToBreakCC))
            return true;

    return false;
}

bool CombatBotBaseAI::UseItemEffect(Item* pItem, bool onlyToBreakCC)
{
    ItemPrototype const* pProto = pItem->GetProto();
    for (auto const& itr : pProto->Spells)
    {
        if (itr.SpellId && itr.SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
        {
            if (SpellEntry const* pSpellEntry = sSpellMgr.GetSpellEntry(itr.SpellId))
            {
                if (me->IsSpellReady(pSpellEntry, pProto))
                {
                    if (onlyToBreakCC && !pSpellEntry->HasAttribute(SPELL_ATTR_EX_IMMUNITY_PURGES_EFFECT))
                        continue;

                    if (pSpellEntry->IsPositiveSpell())
                        return me->CastSpell(me, pSpellEntry, false, pItem) == SPELL_CAST_OK;
                    else if (me->GetVictim())
                        return me->CastSpell(me->GetVictim(), pSpellEntry, false, pItem) == SPELL_CAST_OK;
                }
            }
        }
    }
    return false;
}

void CombatBotBaseAI::BreakCrowdControlEffects()
{
    if (UseTrinketEffects(true))
        return;

    switch (me->GetClass())
    {
        case CLASS_PALADIN:
        {
            if (m_spells.paladin.pDivineShield &&
                CanTryToCastSpell(me, m_spells.paladin.pDivineShield))
            {
                if (DoCastSpell(me, m_spells.paladin.pDivineShield) == SPELL_CAST_OK)
                {
                    if (m_role != ROLE_HEALER)
                    {
                        me->m_Events.AddLambdaEventAtOffset([player = me, spellId = m_spells.paladin.pDivineShield->Id]()
                        {
                            if (player->GetHealthPercent() > 75.0f && player->GetAttackers().size() < 3)
                                player->RemoveAurasDueToSpellByCancel(spellId);
                        }, 1 * IN_MILLISECONDS);
                    }
                    return;
                }
            }
            break;
        }
        case CLASS_MAGE:
        {
            if (me->HasUnitState(UNIT_STATE_STUNNED) && m_spells.mage.pBlink &&
                CanTryToCastSpell(me, m_spells.mage.pBlink))
            {
                if (DoCastSpell(me, m_spells.mage.pBlink) == SPELL_CAST_OK)
                    return;
            }
            if (m_spells.mage.pIceBlock &&
                CanTryToCastSpell(me, m_spells.mage.pIceBlock))
            {
                if (DoCastSpell(me, m_spells.mage.pIceBlock) == SPELL_CAST_OK)
                {
                    me->m_Events.AddLambdaEventAtOffset([player = me, spellId = m_spells.mage.pIceBlock->Id]()
                    {
                        if (player->GetHealthPercent() > 75.0f && player->GetAttackers().size() < 3)
                            player->RemoveAurasDueToSpellByCancel(spellId);
                    }, 1 * IN_MILLISECONDS);
                    return;
                }
            }
            break;
        }
        case CLASS_DRUID:
        {
            bool polymorphed = false;
            auto const& auraList = me->GetAurasByType(SPELL_AURA_MOD_CONFUSE);
            for (auto const& pAura : auraList)
            {
                if (pAura->GetSpellProto()->Mechanic == MECHANIC_POLYMORPH)
                {
                    polymorphed = true;
                    break;
                }
            }

            if (polymorphed)
            {
                SpellEntry const* pShapeshift = nullptr;

                if (m_role == ROLE_TANK && m_spells.druid.pBearForm && CanTryToCastSpell(me, m_spells.druid.pBearForm))
                    pShapeshift = m_spells.druid.pBearForm;
                else if (m_role == ROLE_MELEE_DPS && m_spells.druid.pCatForm && CanTryToCastSpell(me, m_spells.druid.pCatForm))
                    pShapeshift = m_spells.druid.pCatForm;
                else if (m_role == ROLE_RANGE_DPS && m_spells.druid.pMoonkinForm && CanTryToCastSpell(me, m_spells.druid.pMoonkinForm))
                    pShapeshift = m_spells.druid.pMoonkinForm;
                else
                {
                    for (auto const& pSpell : m_spells.raw.spells)
                    {
                        if (pSpell && pSpell->HasAura(SPELL_AURA_MOD_SHAPESHIFT) && CanTryToCastSpell(me, pSpell))
                        {
                            pShapeshift = pSpell;
                            break;
                        }
                    }
                }

                if (pShapeshift && DoCastSpell(me, pShapeshift) == SPELL_CAST_OK)
                {
                    if (m_role == ROLE_HEALER)
                    {
                        me->m_Events.AddLambdaEventAtOffset([player = me, spellId = pShapeshift->Id]()
                        {
                            player->RemoveAurasDueToSpellByCancel(spellId);
                        }, 1 * IN_MILLISECONDS);
                    }
                    return;
                }
            }
            break;
        }
    }
}

bool CombatBotBaseAI::IsWearingShield(Player* pPlayer) const
{
    Item* pItem = pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
    if (!pItem)
        return false;

    if (pItem->GetProto()->InventoryType == INVTYPE_SHIELD)
        return true;

    return false;
}

bool CombatBotBaseAI::IsInDuel() const
{
    return me->m_duel && me->m_duel->startTime != 0;
}

CombatBotRoles CombatBotBaseAI::GetRole() const
{
    if (m_role == ROLE_HEALER && IsInDuel())
    {
        if (IsMeleeDamageClass(me->GetClass()))
            return ROLE_MELEE_DPS;
        else
            return ROLE_RANGE_DPS;
    }

    return m_role;
}

void CombatBotBaseAI::SendBattlefieldPortPacket()
{
    for (uint32 i = BATTLEGROUND_QUEUE_AV; i <= BATTLEGROUND_QUEUE_AB; i++)
    {
        if (me->IsInvitedForBattleGroundQueueType(static_cast<BattleGroundQueueTypeId>(i)))
        {
            WorldPackets::Battleground::BattleFieldPort packet;
#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_8_4
            packet.mapId = GetBattleGrounMapIdByTypeId(static_cast<BattleGroundTypeId>(i));
#endif
            packet.action = 1;
            me->GetSession()->HandleBattleFieldPortOpcode(packet);
            break;
        }
    }
}

void CombatBotBaseAI::SendBattlemasterJoinPacket(uint8 battlegroundId)
{
    uint32 instanceId = 0; // first available
    uint32 mapId;
    switch (battlegroundId)
    {
        case BATTLEGROUND_QUEUE_AV:
            mapId = MAP_ALTERAC_VALLEY;
            break;
        case BATTLEGROUND_QUEUE_WS:
            mapId = MAP_WARSONG_GULCH;
            break;
        case BATTLEGROUND_QUEUE_AB:
            mapId = MAP_ARATHI_BASIN;
            break;
        default:
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "BattleBot: Invalid BG queue type!");
            botEntry->requestRemoval = true;
            return;
    }

    me->GetSession()->RequestBgJoinQueue(me->GetObjectGuid(), instanceId, mapId, false);
}

void CombatBotBaseAI::SendAreaTriggerPacket(uint32 areaTriggerId)
{
    WorldPackets::Misc::AreaTrigger packet;
    packet.triggerId = areaTriggerId;
    me->GetSession()->HandleAreaTriggerOpcode(packet);
}

void CombatBotBaseAI::ActivateNearbyAreaTrigger()
{
    for (auto const& itr : sObjectMgr.GetAreaTriggersMap())
    {
        AreaTriggerEntry const* pTrigger = &itr.second;
        if (!pTrigger)
            continue;

        if (!IsPointInAreaTriggerZone(pTrigger, me->GetMapId(), me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(), 5.0f))
            continue;

        SendAreaTriggerPacket(pTrigger->id);
        break;
    }
}

void CombatBotBaseAI::OnPacketReceived(WorldPacket const* packet)
{
    // Must always check "me" player pointer here!
    //printf("Bot received %s\n", LookupOpcodeName(packet->GetOpcode()));
    switch (packet->GetOpcode())
    {
        case SMSG_NEW_WORLD:
        {
            if (!me)
                return;

            auto data = std::make_unique<NullClientPacket>(MSG_MOVE_WORLDPORT_ACK);
            me->GetSession()->QueuePacket(std::move(data));
            break;
        }
        case MSG_MOVE_TELEPORT_ACK:
        {
            if (!me)
                return;

            auto data = std::make_unique<WorldPackets::Movement::MoveTeleportAck>();
            data->guid = me->GetObjectGuid();
#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_9_4
            data->movementCounter = me->GetLastCounterForMovementChangeType(TELEPORT);
#endif
            data->time = uint32(time(nullptr));
            me->GetSession()->QueuePacket(std::move(data));
            break;
        }
        case SMSG_LOGIN_SETTIMESPEED:
        {
            if (!me)
                return;

            UpdateVisualHonorRankBasedOnItems();
            break;
        }
        case SMSG_TRADE_STATUS:
        {
            if (!me)
                return;

            uint32 status = *((uint32*)(*packet).contents());
            if (status == TRADE_STATUS_BEGIN_TRADE)
            {
                auto data = std::make_unique<NullClientPacket>(CMSG_BEGIN_TRADE);
                me->GetSession()->QueuePacket(std::move(data));
            }
            else if (status == TRADE_STATUS_OPEN_WINDOW)
                FillSupplyTrade();
            else if (status == TRADE_STATUS_TRADE_ACCEPT)
            {
                auto data = std::make_unique<WorldPackets::Trade::AcceptTrade>();
                me->GetSession()->QueuePacket(std::move(data));
            }
            else if (status == TRADE_STATUS_TRADE_COMPLETE)
            {
                EquipOrUseNewItem();
                UpdateVisualHonorRankBasedOnItems();
            }
            break;
        }
        case SMSG_TRADE_STATUS_EXTENDED:
        {
            // Trade window changed. Sent before the core clears this side's trade spell for a
            // changed "will not be traded" slot, so the rogue looks one update later.
            if (!me || me->GetClass() != CLASS_ROGUE)
                return;

            Player* pRogue = me;
            pRogue->m_Events.AddLambdaEventAtOffset([pRogue]()
            {
                if (CombatBotBaseAI* pAI = dynamic_cast<CombatBotBaseAI*>(pRogue->AI()))
                    pAI->PickLockInTrade();
            }, 1);
            break;
        }
        case SMSG_RESURRECT_REQUEST:
        {
            if (!me)
                return;

            auto data = std::make_unique<WorldPackets::Misc::ResurrectResponse>();
            data->resurrectorGuid = me->GetResurrector();
            data->accept = true;
            me->GetSession()->QueuePacket(std::move(data));
            break;
        }
        case SMSG_BATTLEFIELD_STATUS:
        {
            if (!me)
                return;

            if (me->IsBeingTeleported() || me->InBattleGround())
                m_receivedBgInvite = false;
            else
            {
                for (uint32 i = BATTLEGROUND_QUEUE_AV; i <= BATTLEGROUND_QUEUE_AB; i++)
                {
                    if (me->IsInvitedForBattleGroundQueueType(BattleGroundQueueTypeId(i)))
                    {
                        m_receivedBgInvite = true;
                        break;
                    }
                }
            }
            return;
        }
        case SMSG_LOOT_START_ROLL:
        {
            if (!me)
                return;

            uint64 guid = *((uint64*)(*packet).contents());
            uint32 slot = *(((uint32*)(*packet).contents()) + 2);

            auto data = std::make_unique<WorldPackets::Loot::LootRoll>();
            data->lootedTarget = ObjectGuid(guid);
            data->itemSlot = slot;
            data->rollType = ROLL_PASS;
            me->GetSession()->QueuePacket(std::move(data));
            return;
        }
    }
}
