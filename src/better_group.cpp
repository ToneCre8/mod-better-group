
#include "cs_better_group.h"
#include "better_group.h"
#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Formulas.h"
#include "Group.h"
#include "ItemTemplate.h"
#include "KillRewarder.h"
#include "LootMgr.h"
#include "ObjectMgr.h"
#include "Random.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

//=================================================================================================================================================================================
//=================================================================================================================================================================================

// ─── Internal state ──────────────────────────────────────────────────────────

struct CreatureScalingState
{
    uint32 originalMaxHealth;   // Stored before scaling; restored on evade/death.
    uint32 nearbyGroupSize;     // Number of nearby group members counted at combat start.
    float  healthMultiplier;    // Applied once at combat start.
    float  damageMultiplier;    // Applied each hit via damage hooks.
};

// Map from creature GUID to their pre-scaled state.
// Entries only exist while the creature is in active scaled combat.
// Creatures are removed on evade or death.
static std::unordered_map<ObjectGuid, CreatureScalingState> _scaledCreatures;

constexpr uint32 kMaxSupportedGroupSize = 10;
constexpr uint8 kDefaultRandomLootRequiredLevelWindow = 3;

struct BetterGroupRandomLootConfig
{
    bool enabled = false;
    bool outdoorOnly = true;
    bool rareOnly = false;
    bool onlyBindOnPickup = false;
    bool onlyBindOnEquip = false;
    bool allowBindOnUse = false;
    uint32 minGroupSize = 6;
    uint8 minPlayerLevel = 1;
    uint8 whiteMaxPlayerLevel = 10;
    uint8 blueMinRequiredLevel = 1;
    uint8 requiredLevelWindow = kDefaultRandomLootRequiredLevelWindow;
    float normalWhiteChancePct = 10.0f;
    float normalGreenChancePct = 8.0f;
    float normalBlueChancePct = 1.0f;
    float eliteWhiteChancePct = 25.0f;
    float eliteGreenChancePct = 35.0f;
    float eliteBlueChancePct = 10.0f;
};

static BetterGroupRandomLootConfig _randomLootConfig;
static bool _randomLootRuntimeReady = false;
static std::unordered_set<uint32> _disabledRandomLootItemIds;
static std::unordered_map<uint8, std::vector<uint32>> _whiteLootByRequiredLevel;
static std::unordered_map<uint8, std::vector<uint32>> _greenLootByRequiredLevel;
static std::unordered_map<uint8, std::vector<uint32>> _blueLootByRequiredLevel;

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

static char const* GetCreatureSkipReason(Creature const* creature)
{
    if (creature->isElite())
        return "elite, rare-elite, or world-boss rank";

    if (creature->IsPet())
        return "pet";

    if (creature->IsTotem())
        return "totem";

    if (creature->IsSummon())
        return "summon";

    if (creature->IsCritter())
        return "critter";

    if (creature->IsGuard())
        return "guard";

    if (creature->IsTrigger())
        return "trigger";

    return nullptr;
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

    if (!groupXPCompensationEnabled)
        return;

    Group* group = player->GetGroup();
    if (!group)
        return; // Solo — rate is already 1.0, nothing to compensate.

    // Optionally skip compensation entirely for raid groups.
    if (group->isRaidGroup() && disableXPCompensationInRaid)
        return;

    // ---------------------------------------------------------------
    // Primary compensation: slide rate toward 1.0 (solo equivalent).
    //
    //   newRate = rate + (1.0 - rate) * CompensationPct
    //
    // CompensationPct = 1.0 -> each member receives solo-equivalent XP
    // CompensationPct = 0.5 -> halfway between group penalty and solo
    // ---------------------------------------------------------------
    if (rate < 1.0f)
        rate = rate + (1.0f - rate) * compensationPct;

    // Cap the ordinary compensation before any optional gray-penalty offset.
    rate = std::min(rate, maxXPCompensationRate);

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
    if (compensateGrayPenalty)
    {
        if (GroupHasGrayMismatch(group, rewarder))
        {
            // KillRewarder halves this later. Allow up to 2x the configured
            // final cap here so the post-halving reward can still reach it.
            rate = std::min(rate * 2.0f, maxXPCompensationRate * 2.0f);
        }
    }
}

static bool WorldTableExists(std::string const& tableName)
{
    QueryResult result = WorldDatabase.Query("SHOW TABLES LIKE '{}'", tableName);
    return result != nullptr;
}

static uint32 LoadDisabledItemIdsFromTable(std::string const& tableName)
{
    uint32 loaded = 0;

    if (QueryResult result = WorldDatabase.Query("SELECT item FROM {}", tableName))
    {
        do
        {
            Field* fields = result->Fetch();
            _disabledRandomLootItemIds.insert(fields[0].Get<uint32>());
            ++loaded;
        } while (result->NextRow());
    }

    return loaded;
}

static void LoadDisabledRandomLootItemIds()
{
    _disabledRandomLootItemIds.clear();

    uint32 loaded = 0;
    if (WorldTableExists("mod_bettergroup_disabled_items"))
        loaded = LoadDisabledItemIdsFromTable("mod_bettergroup_disabled_items");

    if (!loaded)
        LOG_WARN("module", "BetterGroup random loot found no entries in mod_bettergroup_disabled_items.");

    LOG_INFO("server.loading", ">> BetterGroup random loot loaded {} disabled item ids.", loaded);
}

static bool IsAllowedRandomLootBonding(ItemTemplate const* itemTemplate)
{
    if (!itemTemplate)
        return false;

    if (itemTemplate->Bonding == BIND_QUEST_ITEM || itemTemplate->Bonding == BIND_QUEST_ITEM1)
        return false;

    if (_randomLootConfig.onlyBindOnPickup || _randomLootConfig.onlyBindOnEquip)
    {
        if (_randomLootConfig.onlyBindOnPickup && itemTemplate->Bonding == BIND_WHEN_PICKED_UP)
            return true;

        if (_randomLootConfig.onlyBindOnEquip && itemTemplate->Bonding == BIND_WHEN_EQUIPPED)
            return true;

        return false;
    }

    if (itemTemplate->Bonding == BIND_WHEN_USE)
        return _randomLootConfig.allowBindOnUse;

    return true;
}

static bool IsSupportedRandomLootItem(ItemTemplate const* itemTemplate)
{
    if (!itemTemplate)
        return false;

    if (_disabledRandomLootItemIds.find(itemTemplate->ItemId) != _disabledRandomLootItemIds.end())
        return false;

    if (itemTemplate->Quality != ITEM_QUALITY_NORMAL &&
        itemTemplate->Quality != ITEM_QUALITY_UNCOMMON &&
        itemTemplate->Quality != ITEM_QUALITY_RARE)
        return false;

    if (itemTemplate->Class != ITEM_CLASS_WEAPON && itemTemplate->Class != ITEM_CLASS_ARMOR)
        return false;

    if (itemTemplate->RequiredLevel == 0)
        return false;

    if (itemTemplate->Duration > 0)
        return false;

    if (!IsAllowedRandomLootBonding(itemTemplate))
        return false;

    if (itemTemplate->Class == ITEM_CLASS_ARMOR)
    {
        if (itemTemplate->SubClass == ITEM_SUBCLASS_ARMOR_LIBRAM ||
            itemTemplate->SubClass == ITEM_SUBCLASS_ARMOR_IDOL ||
            itemTemplate->SubClass == ITEM_SUBCLASS_ARMOR_TOTEM ||
            itemTemplate->SubClass == ITEM_SUBCLASS_ARMOR_SIGIL)
        {
            return false;
        }
    }

    return true;
}

static uint32 CountPoolEntries(std::unordered_map<uint8, std::vector<uint32>> const& pool)
{
    uint32 count = 0;
    for (auto const& [level, itemIds] : pool)
    {
        (void)level;
        count += uint32(itemIds.size());
    }

    return count;
}

static void AddRandomLootItemToPool(uint32 itemId, std::unordered_set<uint32>& seenItemIds, uint32& whiteEntries, uint32& greenEntries, uint32& blueEntries, uint32& skippedEntries)
{
    if (!seenItemIds.insert(itemId).second)
        return;

    ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(itemId);
    if (!IsSupportedRandomLootItem(itemTemplate))
    {
        ++skippedEntries;
        return;
    }

    uint8 const requiredLevel = uint8(itemTemplate->RequiredLevel);
    if (itemTemplate->Quality == ITEM_QUALITY_NORMAL)
    {
        _whiteLootByRequiredLevel[requiredLevel].push_back(itemTemplate->ItemId);
        ++whiteEntries;
    }
    else if (itemTemplate->Quality == ITEM_QUALITY_UNCOMMON)
    {
        _greenLootByRequiredLevel[requiredLevel].push_back(itemTemplate->ItemId);
        ++greenEntries;
    }
    else if (itemTemplate->Quality == ITEM_QUALITY_RARE)
    {
        if (itemTemplate->RequiredLevel < _randomLootConfig.blueMinRequiredLevel)
        {
            ++skippedEntries;
            return;
        }

        _blueLootByRequiredLevel[requiredLevel].push_back(itemTemplate->ItemId);
        ++blueEntries;
    }
}

static uint32 LoadRandomLootItemsFromQuery(char const* sql, std::unordered_set<uint32>& seenItemIds, uint32& whiteEntries, uint32& greenEntries, uint32& blueEntries, uint32& skippedEntries)
{
    uint32 rows = 0;

    if (QueryResult result = WorldDatabase.Query(sql))
    {
        do
        {
            Field* fields = result->Fetch();
            AddRandomLootItemToPool(fields[0].Get<uint32>(), seenItemIds, whiteEntries, greenEntries, blueEntries, skippedEntries);
            ++rows;
        } while (result->NextRow());
    }

    return rows;
}

static void BuildRandomLootEntriesFromInstanceLootTemplates()
{
    _whiteLootByRequiredLevel.clear();
    _greenLootByRequiredLevel.clear();
    _blueLootByRequiredLevel.clear();

    ItemTemplateContainer const* itemTemplateStore = sObjectMgr->GetItemTemplateStore();
    if (!itemTemplateStore)
    {
        LOG_WARN("module", "BetterGroup random loot could not access the item template store.");
        return;
    }

    uint32 whiteEntries = 0;
    uint32 greenEntries = 0;
    uint32 blueEntries = 0;
    uint32 skippedEntries = 0;
    std::unordered_set<uint32> seenItemIds;

    uint32 const directRows = LoadRandomLootItemsFromQuery(
        "SELECT DISTINCT clt.Item "
        "FROM creature c "
        "INNER JOIN creature_template ct ON ct.entry IN (c.id1, c.id2, c.id3) "
        "INNER JOIN creature_loot_template clt ON clt.Entry = ct.lootid "
        "INNER JOIN instance_template it ON it.map = c.map "
        "WHERE ct.lootid > 0 AND clt.Reference = 0 AND clt.QuestRequired = 0 AND clt.Item > 0",
        seenItemIds, whiteEntries, greenEntries, blueEntries, skippedEntries);

    uint32 const referenceRows = LoadRandomLootItemsFromQuery(
        "SELECT DISTINCT rlt.Item "
        "FROM creature c "
        "INNER JOIN creature_template ct ON ct.entry IN (c.id1, c.id2, c.id3) "
        "INNER JOIN creature_loot_template clt ON clt.Entry = ct.lootid "
        "INNER JOIN instance_template it ON it.map = c.map "
        "INNER JOIN reference_loot_template rlt ON rlt.Entry = clt.Reference "
        "WHERE ct.lootid > 0 AND clt.Reference > 0 AND clt.QuestRequired = 0 AND rlt.Reference = 0 AND rlt.QuestRequired = 0 AND rlt.Item > 0",
        seenItemIds, whiteEntries, greenEntries, blueEntries, skippedEntries);

    LOG_INFO("server.loading",
        ">> BetterGroup random loot instance-loot scan: directRows={}, referenceRows={}, whiteEntries={}, greenEntries={}, blueEntries={}, skippedEntries={}",
        directRows, referenceRows, whiteEntries, greenEntries, blueEntries, skippedEntries);
}

static void ReloadRandomLootConfig(bool buildTemplatePool)
{
    _randomLootConfig.enabled = sConfigMgr->GetOption<bool>("BetterGroup.RandomLoot.Enable", false);
    _randomLootConfig.outdoorOnly = sConfigMgr->GetOption<bool>("BetterGroup.RandomLoot.OutdoorOnly", true);
    _randomLootConfig.rareOnly = sConfigMgr->GetOption<bool>("BetterGroup.RandomLoot.RareOnly", false);
    _randomLootConfig.onlyBindOnPickup = sConfigMgr->GetOption<bool>("BetterGroup.RandomLoot.OnlyBindOnPickup", false);
    _randomLootConfig.onlyBindOnEquip = sConfigMgr->GetOption<bool>("BetterGroup.RandomLoot.OnlyBindOnEquip", false);
    _randomLootConfig.allowBindOnUse = sConfigMgr->GetOption<bool>("BetterGroup.RandomLoot.AllowBindOnUse", false);
    _randomLootConfig.minGroupSize = std::clamp(sConfigMgr->GetOption<uint32>("BetterGroup.RandomLoot.MinGroupSize", 6), 1u, kMaxSupportedGroupSize);
    _randomLootConfig.minPlayerLevel = uint8(std::clamp<uint32>(sConfigMgr->GetOption<uint32>("BetterGroup.RandomLoot.MinPlayerLevel", 1), 1u, uint32(std::numeric_limits<uint8>::max())));
    _randomLootConfig.whiteMaxPlayerLevel = uint8(std::clamp<uint32>(sConfigMgr->GetOption<uint32>("BetterGroup.RandomLoot.WhiteMaxPlayerLevel", 10), 1u, uint32(std::numeric_limits<uint8>::max())));
    _randomLootConfig.blueMinRequiredLevel = uint8(std::clamp<uint32>(sConfigMgr->GetOption<uint32>("BetterGroup.RandomLoot.BlueMinRequiredLevel", 1), 1u, uint32(std::numeric_limits<uint8>::max())));
    _randomLootConfig.requiredLevelWindow = uint8(std::clamp<uint32>(sConfigMgr->GetOption<uint32>("BetterGroup.RandomLoot.RequiredLevelWindow", kDefaultRandomLootRequiredLevelWindow), 0u, 20u));
    _randomLootConfig.normalWhiteChancePct = std::clamp(sConfigMgr->GetOption<float>("BetterGroup.RandomLoot.NormalWhiteChancePct", 10.0f), 0.0f, 100.0f);
    _randomLootConfig.normalGreenChancePct = std::clamp(sConfigMgr->GetOption<float>("BetterGroup.RandomLoot.NormalGreenChancePct", 8.0f), 0.0f, 100.0f);
    _randomLootConfig.normalBlueChancePct = std::clamp(sConfigMgr->GetOption<float>("BetterGroup.RandomLoot.NormalBlueChancePct", 1.0f), 0.0f, 100.0f);
    _randomLootConfig.eliteWhiteChancePct = std::clamp(sConfigMgr->GetOption<float>("BetterGroup.RandomLoot.EliteWhiteChancePct", 25.0f), 0.0f, 100.0f);
    _randomLootConfig.eliteGreenChancePct = std::clamp(sConfigMgr->GetOption<float>("BetterGroup.RandomLoot.EliteGreenChancePct", 35.0f), 0.0f, 100.0f);
    _randomLootConfig.eliteBlueChancePct = std::clamp(sConfigMgr->GetOption<float>("BetterGroup.RandomLoot.EliteBlueChancePct", 10.0f), 0.0f, 100.0f);

    if (!_randomLootConfig.enabled)
    {
        _disabledRandomLootItemIds.clear();
        _whiteLootByRequiredLevel.clear();
        _greenLootByRequiredLevel.clear();
        _blueLootByRequiredLevel.clear();
        LOG_INFO("server.loading", ">> BetterGroup random loot config: enabled=0");
        return;
    }

    if (buildTemplatePool)
    {
        LoadDisabledRandomLootItemIds();
        BuildRandomLootEntriesFromInstanceLootTemplates();
    }
    else
    {
        _disabledRandomLootItemIds.clear();
        _whiteLootByRequiredLevel.clear();
        _greenLootByRequiredLevel.clear();
        _blueLootByRequiredLevel.clear();
        LOG_INFO("server.loading", ">> BetterGroup random loot config loaded before runtime item templates are ready. Deferring instance-loot pool build until startup.");
    }

    LOG_INFO("server.loading",
        ">> BetterGroup random loot config: enabled={}, minGroupSize={}, outdoorOnly={}, rareOnly={}, normalWhiteChancePct={}, normalGreenChancePct={}, normalBlueChancePct={}, eliteWhiteChancePct={}, eliteGreenChancePct={}, eliteBlueChancePct={}, whiteEntries={}, greenEntries={}, blueEntries={}",
        _randomLootConfig.enabled ? 1 : 0,
        _randomLootConfig.minGroupSize,
        _randomLootConfig.outdoorOnly ? 1 : 0,
        _randomLootConfig.rareOnly ? 1 : 0,
        _randomLootConfig.normalWhiteChancePct,
        _randomLootConfig.normalGreenChancePct,
        _randomLootConfig.normalBlueChancePct,
        _randomLootConfig.eliteWhiteChancePct,
        _randomLootConfig.eliteGreenChancePct,
        _randomLootConfig.eliteBlueChancePct,
        CountPoolEntries(_whiteLootByRequiredLevel),
        CountPoolEntries(_greenLootByRequiredLevel),
        CountPoolEntries(_blueLootByRequiredLevel));
}

static bool IsRareCreature(Creature const* creature)
{
    if (!creature)
        return false;

    uint32 const rank = creature->GetCreatureTemplate()->rank;
    return rank == CREATURE_ELITE_RARE || rank == CREATURE_ELITE_RAREELITE;
}

static bool IsRandomLootCreatureEligible(Creature const* creature)
{
    if (!creature)
        return false;

    if (_randomLootConfig.outdoorOnly && creature->GetMap()->Instanceable())
        return false;

    if (_randomLootConfig.rareOnly && !IsRareCreature(creature))
        return false;

    if (creature->IsPet() || creature->IsTotem() || creature->IsSummon())
        return false;

    if (creature->IsCritter() || creature->IsGuard() || creature->IsTrigger())
        return false;

    return true;
}

static std::vector<uint32> GatherCandidateItemIds(std::unordered_map<uint8, std::vector<uint32>> const& pool, Creature const* creature)
{
    std::vector<uint32> candidates;
    if (!creature)
        return candidates;

    uint8 const creatureLevel = creature->GetLevel();
    uint8 const minLevel = creatureLevel > _randomLootConfig.requiredLevelWindow ? creatureLevel - _randomLootConfig.requiredLevelWindow : 1;
    uint8 const maxLevel = std::min<uint32>(creatureLevel + _randomLootConfig.requiredLevelWindow, std::numeric_limits<uint8>::max());

    for (uint32 level = minLevel; level <= maxLevel; ++level)
    {
        auto itr = pool.find(uint8(level));
        if (itr == pool.end())
            continue;

        candidates.insert(candidates.end(), itr->second.begin(), itr->second.end());
    }

    return candidates;
}

static uint32 ChooseRandomLootItemIdFromPool(std::unordered_map<uint8, std::vector<uint32>> const& pool, Creature const* creature)
{
    std::vector<uint32> candidates = GatherCandidateItemIds(pool, creature);
    if (candidates.empty())
        return 0;

    uint32 const startIndex = urand(0, uint32(candidates.size() - 1));
    for (uint32 offset = 0; offset < candidates.size(); ++offset)
    {
        uint32 const itemId = candidates[(startIndex + offset) % candidates.size()];
        if (sObjectMgr->GetItemTemplate(itemId))
            return itemId;
    }

    return 0;
}

static uint32 CountNearbyLootGroupMembers(Creature const* creature, Player* lootOwner, float radius)
{
    if (!creature || !lootOwner)
        return 0;

    return CountNearbyGroupMembers(creature, lootOwner, radius);
}

static void TryAddRandomGroupLoot(Creature* creature, float detectionRadius)
{
    if (!_randomLootRuntimeReady && _randomLootConfig.enabled)
    {
        LOG_INFO("module", "BetterGroup random loot detected runtime pools before startup. Retrying instance-loot pool build.");
        _randomLootRuntimeReady = true;
        LoadDisabledRandomLootItemIds();
        BuildRandomLootEntriesFromInstanceLootTemplates();
    }

    if (!_randomLootConfig.enabled || !creature)
        return;

    if (!IsRandomLootCreatureEligible(creature))
        return;

    Player* lootOwner = creature->GetLootRecipient();
    if (!lootOwner)
        return;

    if (lootOwner->GetLevel() < _randomLootConfig.minPlayerLevel)
        return;

    uint32 groupSize = 1;
    if (auto itr = _scaledCreatures.find(creature->GetGUID()); itr != _scaledCreatures.end())
        groupSize = itr->second.nearbyGroupSize;
    else
        groupSize = CountNearbyLootGroupMembers(creature, lootOwner, detectionRadius);

    if (groupSize < _randomLootConfig.minGroupSize)
        return;

    bool const isEliteOrRare = creature->isElite() || IsRareCreature(creature);
    float const whiteChance = isEliteOrRare ? _randomLootConfig.eliteWhiteChancePct : _randomLootConfig.normalWhiteChancePct;
    float const greenChance = isEliteOrRare ? _randomLootConfig.eliteGreenChancePct : _randomLootConfig.normalGreenChancePct;
    float const blueChance = isEliteOrRare ? _randomLootConfig.eliteBlueChancePct : _randomLootConfig.normalBlueChancePct;

    uint32 itemId = 0;
    bool const isWhiteOnlyPlayer = lootOwner->GetLevel() <= _randomLootConfig.whiteMaxPlayerLevel;

    if (isWhiteOnlyPlayer && roll_chance_f(whiteChance))
        itemId = ChooseRandomLootItemIdFromPool(_whiteLootByRequiredLevel, creature);
    else if (!isWhiteOnlyPlayer)
    {
        if (roll_chance_f(blueChance))
            itemId = ChooseRandomLootItemIdFromPool(_blueLootByRequiredLevel, creature);

        if (!itemId && roll_chance_f(greenChance))
            itemId = ChooseRandomLootItemIdFromPool(_greenLootByRequiredLevel, creature);
    }

    if (!itemId)
        return;

    Loot& loot = creature->loot;
    bool const hadLoot = !loot.empty();
    size_t const itemCountBefore = loot.items.size();

    if (loot.lootOwnerGUID.IsEmpty())
        loot.lootOwnerGUID = lootOwner->GetGUID();

    LootStoreItem bonusItem(itemId, 0, 100.0f, false, creature->GetLootMode(), 0, 1, 1);
    loot.AddItem(bonusItem);

    if (loot.items.size() == itemCountBefore)
        return;

    if (ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(itemId))
    {
        if (Group* group = creature->GetLootRecipientGroup())
        {
            LootItem& addedItem = loot.items.back();
            if (itemTemplate->Quality < uint32(group->GetLootThreshold()))
                addedItem.is_underthreshold = true;

            if (!hadLoot && !loot.empty())
                group->UpdateLooterGuid(creature);
        }

        LOG_INFO("module",
            "BetterGroup random loot added item {} ('{}', quality={}) to creature '{}' (entry={}, guid={}) for group size {}.",
            itemId, itemTemplate->Name1, itemTemplate->Quality, creature->GetName(), creature->GetEntry(),
            creature->GetGUID().ToString(), groupSize);
    }
}

void BetterGroup::OnAfterConfigLoad(bool reload)
{
    enabled = sConfigMgr->GetOption<bool>("BetterGroup.Enable", false);
    creatureScalingEnabled = sConfigMgr->GetOption<bool>("DynamicCreatureScaling.Enable", false);
    groupXPCompensationEnabled = sConfigMgr->GetOption<bool>("GroupXPCompensation.Enable", false);
    disableXPCompensationInRaid = sConfigMgr->GetOption<bool>("GroupXPCompensation.DisableInRaid", false);
    compensateGrayPenalty = sConfigMgr->GetOption<bool>("GroupXPCompensation.CompensateGrayPenalty", false);
    detectionRadius = sConfigMgr->GetOption<float>("DynamicCreatureScaling.DetectionRadius", 100.0f);
    damageScalingEnabled = sConfigMgr->GetOption<bool>("DynamicCreatureScaling.ScaleDamage", true);
    compensationPct = std::clamp(sConfigMgr->GetOption<float>("GroupXPCompensation.CompensationPct", 1.0f), 0.0f, 1.0f);
    maxXPCompensationRate = std::max(sConfigMgr->GetOption<float>("GroupXPCompensation.MaxRate", 1.0f), 0.0f);

    maxGroupSize = std::clamp(sConfigMgr->GetOption<uint32>("DynamicCreatureScaling.MaxGroupSize", 10), 1u, kMaxSupportedGroupSize);

    for (uint32 i = 1; i <= kMaxSupportedGroupSize; ++i)
    {
        std::string suffix = std::to_string(i);
        hpScale[i] = sConfigMgr->GetOption<float>("DynamicCreatureScaling.HPScale." + suffix, i > 1 ? hpScale[i - 1] : 1.0f);
        dmgScale[i] = sConfigMgr->GetOption<float>("DynamicCreatureScaling.DmgScale." + suffix, i > 1 ? dmgScale[i - 1] : 1.0f);
    }

    ReloadRandomLootConfig(_randomLootRuntimeReady && reload);
}

void BetterGroup::OnStartup()
{
    _randomLootRuntimeReady = true;
    ReloadRandomLootConfig(true);
}

void BetterGroup::OnUnitDeath(Unit* unit, Unit* killer)
{
    if (Creature* creature = unit->ToCreature())
    {
        TryAddRandomGroupLoot(creature, detectionRadius);
        _scaledCreatures.erase(creature->GetGUID());
    }
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

    if (!creatureScalingEnabled)
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

    float hpMult = (groupSize > 0 && groupSize <= kMaxSupportedGroupSize) ? hpScale[groupSize] : 1.0f;
    float dmgMult = damageScalingEnabled ? ((groupSize > 0 && groupSize <= kMaxSupportedGroupSize) ? dmgScale[groupSize] : 1.0f) : 1.0f;

    if (hpMult <= 1.0f && dmgMult <= 1.0f)
        return;

    uint32 originalMaxHp = creature->GetMaxHealth();

    _scaledCreatures[creature->GetGUID()] = { originalMaxHp, groupSize, hpMult, dmgMult };

    if (hpMult > 1.0f)
    {
        uint32 newMaxHp = uint32(float(originalMaxHp) * hpMult);
        ApplyHPScale(creature, newMaxHp);
    }
}

void BetterGroup::OnUnitEnterEvadeMode(Unit* unit, uint8 evadeReason)
{
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
    if (!enabled || !creatureScalingEnabled || !attacker)
        return;

    auto it = _scaledCreatures.find(attacker->GetGUID());
    if (it == _scaledCreatures.end())
        return;

    damage = uint32(float(damage) * it->second.damageMultiplier);
}

void BetterGroup::ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* spellInfo)
{
    if (!enabled || !creatureScalingEnabled || !attacker || damage <= 0)
        return;

    auto it = _scaledCreatures.find(attacker->GetGUID());
    if (it == _scaledCreatures.end())
        return;

    damage = int32(float(damage) * it->second.damageMultiplier);
}

void BetterGroup::ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage, SpellInfo const* spellInfo)
{
    if (!enabled || !creatureScalingEnabled || !attacker)
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

    if (command.empty() || command == "help")
    {
        handler->PSendSysMessage("BetterGroup commands:");
        handler->PSendSysMessage("  .group better status  - show current module settings");
        handler->PSendSysMessage("  .group better scales  - show configured scaling table");
        handler->PSendSysMessage("  .group better target  - inspect the selected creature");
        return true;
    }

    if (command == "status")
    {
        handler->PSendSysMessage("BetterGroup status:");
        handler->PSendSysMessage("  Master enabled: {}", sConfigMgr->GetOption<bool>("BetterGroup.Enable", false));
        handler->PSendSysMessage("  Creature scaling enabled: {}", sConfigMgr->GetOption<bool>("DynamicCreatureScaling.Enable", false));
        handler->PSendSysMessage("  Detection radius: {:.1f}", sConfigMgr->GetOption<float>("DynamicCreatureScaling.DetectionRadius", 100.0f));
        handler->PSendSysMessage("  Max group size: {}", std::clamp(sConfigMgr->GetOption<uint32>("DynamicCreatureScaling.MaxGroupSize", 10), 1u, kMaxSupportedGroupSize));
        handler->PSendSysMessage("  Damage scaling enabled: {}", sConfigMgr->GetOption<bool>("DynamicCreatureScaling.ScaleDamage", true));
        handler->PSendSysMessage("  Group XP compensation enabled: {}", sConfigMgr->GetOption<bool>("GroupXPCompensation.Enable", false));
        handler->PSendSysMessage("  XP compensation percent: {:.2f}", std::clamp(sConfigMgr->GetOption<float>("GroupXPCompensation.CompensationPct", 1.0f), 0.0f, 1.0f));
        handler->PSendSysMessage("  XP max rate: {:.2f}", std::max(sConfigMgr->GetOption<float>("GroupXPCompensation.MaxRate", 1.0f), 0.0f));
        handler->PSendSysMessage("  XP disabled in raids: {}", sConfigMgr->GetOption<bool>("GroupXPCompensation.DisableInRaid", false));
        handler->PSendSysMessage("  Gray penalty compensation: {}", sConfigMgr->GetOption<bool>("GroupXPCompensation.CompensateGrayPenalty", false));
        handler->PSendSysMessage("  Random group loot enabled: {}", sConfigMgr->GetOption<bool>("BetterGroup.RandomLoot.Enable", false));
        handler->PSendSysMessage("  Random group loot min group size: {}", std::clamp(sConfigMgr->GetOption<uint32>("BetterGroup.RandomLoot.MinGroupSize", 6), 1u, kMaxSupportedGroupSize));
        handler->PSendSysMessage("  Random group loot pools: white={}, green={}, blue={}",
            CountPoolEntries(_whiteLootByRequiredLevel),
            CountPoolEntries(_greenLootByRequiredLevel),
            CountPoolEntries(_blueLootByRequiredLevel));
        handler->PSendSysMessage("  Creatures currently tracked as scaled: {}", _scaledCreatures.size());
        return true;
    }

    if (command == "scales")
    {
        handler->PSendSysMessage("BetterGroup scaling table:");
        handler->PSendSysMessage("  Players | HP x | Damage x");
        uint32 maxConfiguredGroupSize = std::clamp(sConfigMgr->GetOption<uint32>("DynamicCreatureScaling.MaxGroupSize", 10), 1u, kMaxSupportedGroupSize);
        float hpScale = 1.0f;
        float dmgScale = 1.0f;
        for (uint32 i = 1; i <= maxConfiguredGroupSize; ++i)
        {
            std::string suffix = std::to_string(i);
            hpScale = sConfigMgr->GetOption<float>("DynamicCreatureScaling.HPScale." + suffix, hpScale);
            dmgScale = sConfigMgr->GetOption<float>("DynamicCreatureScaling.DmgScale." + suffix, dmgScale);

            handler->PSendSysMessage("  {:>7} | {:.2f} | {:.2f}",
                i,
                hpScale,
                dmgScale);
        }
        return true;
    }

    if (command == "target")
    {
        Creature* creature = handler->getSelectedCreature();
        if (!creature)
        {
            handler->SendErrorMessage("Select a creature first.");
            return true;
        }

        handler->PSendSysMessage("BetterGroup target inspection:");
        handler->PSendSysMessage("  Name: {}", creature->GetName());
        handler->PSendSysMessage("  Entry: {}", creature->GetEntry());
        handler->PSendSysMessage("  GUID: {}", creature->GetGUID().ToString());
        handler->PSendSysMessage("  Map is instanceable: {}", creature->GetMap()->Instanceable());

        if (char const* reason = GetCreatureSkipReason(creature))
            handler->PSendSysMessage("  Scaling eligibility: no ({})", reason);
        else if (creature->GetMap()->Instanceable())
            handler->PSendSysMessage("  Scaling eligibility: no (instanced content)");
        else
            handler->PSendSysMessage("  Scaling eligibility: yes");

        auto it = _scaledCreatures.find(creature->GetGUID());
        if (it == _scaledCreatures.end())
        {
            handler->PSendSysMessage("  Currently tracked as scaled: no");
            handler->PSendSysMessage("  Current HP: {}/{}", creature->GetHealth(), creature->GetMaxHealth());
            return true;
        }

        CreatureScalingState const& state = it->second;
        handler->PSendSysMessage("  Currently tracked as scaled: yes");
        handler->PSendSysMessage("  Nearby group members counted: {}", state.nearbyGroupSize);
        handler->PSendSysMessage("  HP multiplier: {:.2f}", state.healthMultiplier);
        handler->PSendSysMessage("  Damage multiplier: {:.2f}", state.damageMultiplier);
        handler->PSendSysMessage("  Original max HP: {}", state.originalMaxHealth);
        handler->PSendSysMessage("  Current HP: {}/{}", creature->GetHealth(), creature->GetMaxHealth());
        return true;
    }

    handler->PSendSysMessage("Unknown BetterGroup command: {}", command);
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




















