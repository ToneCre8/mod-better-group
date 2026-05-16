
#include "cs_better_group.h"
#include "better_group.h"
#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "ItemTemplate.h"
#include "LootMgr.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include <cctype>
#include <cmath>
#include <iostream>
#include <string>
#include <algorithm>
#include <vector>
#include "Group.h"
#include "KillRewarder.h"
#include "Formulas.h"

//=================================================================================================================================================================================
//=================================================================================================================================================================================

// ─── Internal state ──────────────────────────────────────────────────────────

struct CreatureScalingState
{
    uint32 originalMaxHealth;   // Stored before scaling; restored on evade/death.
    float  damageMultiplier;    // Applied each hit via damage hooks.
};

// Map from creature GUID to their pre-scaled state.
// Entries only exist while the creature is in active scaled combat.
// Creatures are removed on evade or death.
static std::unordered_map<ObjectGuid, CreatureScalingState> _scaledCreatures;

// ─── Helpers ─────────────────────────────────────────────────────────────────

// Returns false for creature types/ranks that should never be scaled.
static bool ShouldSkipCreature(Creature const* creature)
{
    // Skip elite/rareelite/worldboss; allow normal and rare.
    if (creature->isElite())
        return true;

    // Skip player pets, totems, summons (hunter pet has its own mask too).
    if (creature->IsPet() || creature->IsTotem() || creature->IsSummon())
        return true;

    // Skip critters — they have no meaningful combat.
    if (creature->IsCritter())
        return true;

    // Skip guards and invisible trigger NPCs.
    if (creature->IsGuard() || creature->IsTrigger())
        return true;

    return false;
}

// Extracts the controlling Player from any kind of aggro-initiating unit.
// Returns nullptr if no player is involved (e.g. creature vs creature).
static Player* GetPlayerAttacker(Unit* victim)
{
    if (!victim)
        return nullptr;

    if (victim->IsPlayer())
        return victim->ToPlayer();

    // Cover pets, totems, controlled creatures, mind-controlled players, etc.
    if (Unit* owner = victim->GetCharmerOrOwner())
        if (owner->IsPlayer())
            return owner->ToPlayer();

    return nullptr;
}

// Counts alive group members (including the attacker) within radius of the
// creature.  Returns 1 when the attacker has no group.
static uint32 CountNearbyGroupMembers(Creature const* creature, Player* attacker, float radius)
{
    Group* group = attacker->GetGroup();
    if (!group)
        return 1;

    uint32 count = 0;
    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        if (Player* member = itr->GetSource())
        {
            if (member->IsAlive() &&
                member->IsInMap(creature) &&
                member->GetExactDist(creature) <= radius)
                ++count;
        }
    }

    return std::max(count, 1u);
}

// Returns true when the group's XP for this kill would be halved due to
  // gray-level mismatch (_isFullXP == false in KillRewarder).
  // Replicates the _InitGroupData() logic from KillRewarder.cpp so we can
  // detect this outside the class.
static bool GroupHasGrayMismatch(Group* group, KillRewarder* rewarder)
{
    Unit* victim = rewarder->GetVictim();
    Player* killer = rewarder->GetKiller();

    if (!victim || !killer)
        return false;

    uint8 maxLevel = 0;
    uint8 maxNotGrayLevel = 0;

    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        if (Player* member = itr->GetSource())
        {
            if (!(killer == member || member->IsAtGroupRewardDistance(victim)))
                continue;

            if (!member->IsAlive())
                continue;

            uint8 lvl = member->GetLevel();

            if (lvl > maxLevel)
                maxLevel = lvl;

            uint32 grayLevel = Acore::XP::GetGrayLevel(lvl);
            if (victim->GetLevel() > grayLevel && lvl > maxNotGrayLevel)
                maxNotGrayLevel = lvl;
        }
    }

    // _isFullXP = (maxNotGrayMember != null) && (maxLevel == maxNotGrayMember->GetLevel())
    return maxNotGrayLevel == 0 || maxNotGrayLevel < maxLevel;
}

BetterGroup::BetterGroup() :
    PlayerScript("BetterGroup"),
    UnitScript("BetterGroup"),
    WorldScript("BetterGroup")
{
    // Start the server
    //server_.start();
    LOG_WARN("server.loading", ">>BetterGroup loaded...");
}

BetterGroup::~BetterGroup()
{
    // Stop the server
    //server_.stop();
}

void BetterGroup::OnPlayerRewardKillRewarder(Player* player, KillRewarder* rewarder, bool isDungeon, float& rate)
{
    if (!enabled)
        return;

    if (!sConfigMgr->GetOption<bool>("GroupXPCompensation.Enable", false))
        return;

    Group* group = player->GetGroup();
    if (!group)
        return; // Solo — rate is already 1.0, nothing to compensate.

    // Optionally skip compensation entirely for raid groups.
    if (group->isRaidGroup() && sConfigMgr->GetOption<bool>("GroupXPCompensation.DisableInRaid", true))
        return;

    // Safety cap applied after all adjustments.
    float maxRate = sConfigMgr->GetOption<float>("GroupXPCompensation.MaxRate", 1.0f);

    // ---------------------------------------------------------------
    // Primary compensation: slide rate toward 1.0 (solo equivalent).
    //
    //   newRate = rate + (1.0 - rate) * CompensationPct
    //
    // CompensationPct = 1.0 -> each member receives solo-equivalent XP
    // CompensationPct = 0.5 -> halfway between group penalty and solo
    // ---------------------------------------------------------------
    float compensationPct = sConfigMgr->GetOption<float>("GroupXPCompensation.CompensationPct", 1.0f);
    compensationPct = std::clamp(compensationPct, 0.0f, 1.0f);

    if (rate < 1.0f)
        rate = rate + (1.0f - rate) * compensationPct;

    // ---------------------------------------------------------------
    // Optional: compensate for the gray-level half-XP penalty.
    //
    // When _isFullXP == false (some members see the victim as gray),
    // KillRewarder halves every member's XP *after* this hook runs.
    // Doubling the rate here offsets that halving exactly.
    //
    // Only useful if you also want to remove the gray mismatch penalty.
    // Leave disabled (default) for more conservative behaviour.
    // ---------------------------------------------------------------
    if (sConfigMgr->GetOption<bool>("GroupXPCompensation.CompensateGrayPenalty", false))
    {
        if (GroupHasGrayMismatch(group, rewarder))
            rate *= 2.0f;
    }

    // Apply safety cap last.
    rate = std::min(rate, maxRate);
}

void BetterGroup::OnAfterConfigLoad(bool reload)
{
    enabled = sConfigMgr->GetOption<bool>("BetterGroup.Enable", false);
    detectionRadius = sConfigMgr->GetOption<float>("DynamicCreatureScaling.DetectionRadius", 100.0f);
    damageScalingEnabled = sConfigMgr->GetOption<bool>("DynamicCreatureScaling.ScaleDamage", true);

    constexpr uint32 kHardCap = 5;
    maxGroupSize = std::min(sConfigMgr->GetOption<uint32>("DynamicCreatureScaling.MaxGroupSize", 5), kHardCap);

    static const char* const hpKeys[] = { nullptr, "DynamicCreatureScaling.HPScale.1",  "DynamicCreatureScaling.HPScale.2",  "DynamicCreatureScaling.HPScale.3",  "DynamicCreatureScaling.HPScale.4",  "DynamicCreatureScaling.HPScale.5" };
    static const char* const dmgKeys[] = { nullptr, "DynamicCreatureScaling.DmgScale.1", "DynamicCreatureScaling.DmgScale.2", "DynamicCreatureScaling.DmgScale.3", "DynamicCreatureScaling.DmgScale.4", "DynamicCreatureScaling.DmgScale.5" };

    for (uint32 i = 1; i <= kHardCap; ++i)
    {
        hpScale[i] = sConfigMgr->GetOption<float>(hpKeys[i], 1.0f);
        dmgScale[i] = sConfigMgr->GetOption<float>(dmgKeys[i], 1.0f);
    }
}

void BetterGroup::OnUnitDeath(Unit* unit, Unit* killer)
{
    if (!enabled)
        return;

    if (Creature* creature = unit->ToCreature())
        _scaledCreatures.erase(creature->GetGUID());
}

// ─── HP scale / restore ───────────────────────────────────────────────────────

// Bumps the creature's maximum health and heals it to the new maximum.
// Called once when scaling is first applied (creature should be at full health).
static void ApplyHPScale(Creature* creature, uint32 newMaxHealth)
{
    creature->SetCreateHealth(newMaxHealth);
    creature->SetStatFlatModifier(UNIT_MOD_HEALTH, BASE_VALUE, float(newMaxHealth));
    creature->UpdateMaxHealth();
    // Creature enters combat at full HP; heal to the new cap.
    creature->SetHealth(creature->GetMaxHealth());
}

// Restores health to originalMaxHealth, preserving the creature's current
// health percentage so a damaged evader doesn't appear to instantly full-heal.
static void RestoreHPScale(Creature* creature, uint32 originalMaxHealth)
{
    float healthPct = (creature->GetMaxHealth() > 0)
        ? float(creature->GetHealth()) / float(creature->GetMaxHealth())
        : 1.0f;

    creature->SetCreateHealth(originalMaxHealth);
    creature->SetStatFlatModifier(UNIT_MOD_HEALTH, BASE_VALUE, float(originalMaxHealth));
    creature->UpdateMaxHealth();

    // Clamp restored HP to [1, originalMax].
    uint32 restoredHealth = uint32(float(originalMaxHealth) * healthPct);
    creature->SetHealth(std::max(restoredHealth, 1u));
}

void BetterGroup::OnUnitEnterCombat(Unit* unit, Unit* victim)
{
    if (!enabled)
        return;

    if (!sConfigMgr->GetOption<bool>("DynamicCreatureScaling.Enable", false))
        return;

    Creature* creature = unit->ToCreature();
    if (!creature)
        return;

    Player* attacker = GetPlayerAttacker(victim);

    if (ShouldSkipCreature(creature))
        return;

    // Skip all instanced content (dungeons, heroics, raids).
    if (creature->GetMap()->Instanceable())
        return;

    // Guard: never scale the same creature twice in one combat.
    if (_scaledCreatures.count(creature->GetGUID()))
        return;

    if (!attacker)
        return;

    uint32 groupSize = CountNearbyGroupMembers(creature, attacker, detectionRadius);
    groupSize = std::min(groupSize, maxGroupSize);

    float hpMult = (groupSize > 0 && groupSize <= 5) ? hpScale[groupSize] : 1.0f;
    float dmgMult = damageScalingEnabled ? ((groupSize > 0 && groupSize <= 5) ? dmgScale[groupSize] : 1.0f) : 1.0f;

    if (hpMult <= 1.0f && dmgMult <= 1.0f)
        return;

    uint32 originalMaxHp = creature->GetMaxHealth();

    _scaledCreatures[creature->GetGUID()] = { originalMaxHp, dmgMult };

    if (hpMult > 1.0f)
    {
        uint32 newMaxHp = uint32(float(originalMaxHp) * hpMult);
        ApplyHPScale(creature, newMaxHp);
    }
}

void BetterGroup::OnUnitEnterEvadeMode(Unit* unit, uint8 evadeReason)
{
    if (!enabled)
        return;

    if (!sConfigMgr->GetOption<bool>("DynamicCreatureScaling.Enable", false))
        return;

    Creature* creature = unit->ToCreature();
    if (!creature)
        return;

    auto it = _scaledCreatures.find(creature->GetGUID());
    if (it == _scaledCreatures.end())
        return;

    uint32 originalMaxHp = it->second.originalMaxHealth;
    _scaledCreatures.erase(it);

    RestoreHPScale(creature, originalMaxHp);
}

void BetterGroup::ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage)
{
    if (!enabled || !attacker)
        return;

    if (!sConfigMgr->GetOption<bool>("DynamicCreatureScaling.Enable", false))
        return;

    auto it = _scaledCreatures.find(attacker->GetGUID());
    if (it == _scaledCreatures.end())
        return;

    damage = uint32(float(damage) * it->second.damageMultiplier);
}

void BetterGroup::ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* spellInfo)
{
    if (!enabled || !attacker || damage <= 0)
        return;

    if (!sConfigMgr->GetOption<bool>("DynamicCreatureScaling.Enable", false))
        return;

    auto it = _scaledCreatures.find(attacker->GetGUID());
    if (it == _scaledCreatures.end())
        return;

    damage = int32(float(damage) * it->second.damageMultiplier);
}

void BetterGroup::ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage, SpellInfo const* spellInfo)
{
    if (!enabled || !attacker)
        return;

    if (!sConfigMgr->GetOption<bool>("DynamicCreatureScaling.Enable", false))
        return;

    auto it = _scaledCreatures.find(attacker->GetGUID());
    if (it == _scaledCreatures.end())
        return;

    damage = uint32(float(damage) * it->second.damageMultiplier);
}

bool BetterGroup::HandleBetterGroupCommand(ChatHandler* handler, char const* args)
{
    std::istringstream stream(args ? args : "");
    std::string command;
    std::string qualifier;

    stream >> command >> qualifier;
    std::transform(command.begin(), command.end(), command.begin(), ::tolower);
    std::transform(qualifier.begin(), qualifier.end(), qualifier.begin(), ::tolower);

    if (command.empty())
    {
        handler->PSendSysMessage("Usage: .better group not-implemented");
        return true;
    }

   

    handler->PSendSysMessage("Unknown bettergroup command: %s", command.c_str());
    return true;
}
//============================================================================================


//===================================================================================================
//===================================================================================================

void AddBetterGroupScripts()
{
    new BetterGroup();
    AddSC_bettergroup_commandscript();
}




















