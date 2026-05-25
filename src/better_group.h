#pragma once
#include "Player.h"
#include "PlayerScript.h"
#include "WorldScript.h"
#include "ServerScript.h"
#include "UnitScript.h"
#include "AllSpellScript.h"


class BetterGroup :
    public PlayerScript,
    public UnitScript,
    public WorldScript
{
public:
    BetterGroup();
    ~BetterGroup();

    //PlayerScript
    void OnPlayerRewardKillRewarder(Player* player, KillRewarder* rewarder, bool isDungeon, float& rate) override;
    void OnPlayerLeaveCombat(Player* player) override;
    void OnPlayerUpdate(Player* player, uint32 p_time) override;
    void OnPlayerLogout(Player* player) override;

    //WorldScript
    void OnAfterConfigLoad(bool reload) override;
    void OnStartup() override;

    //UnitScript
    void OnUnitDeath(Unit* unit, Unit* /*killer*/) override;
    void OnUnitEnterCombat(Unit* unit, Unit* victim) override;
    // ── Evade: undo scaling so the creature resets to normal HP ───────────────
    void OnUnitEnterEvadeMode(Unit* unit, uint8 /*evadeReason*/) override;
    // ── Per-hit melee damage multiplier ───────────────────────────────────────
    void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage) override;
    // ── Per-hit spell damage multiplier ───────────────────────────────────────
    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* spellInfo) override;
    // ── Per-tick DoT damage multiplier ────────────────────────────────────────
    void ModifyPeriodicDamageAurasTick(Unit* /*target*/, Unit* attacker, uint32& damage, SpellInfo const* /*spellInfo*/) override;

    static bool HandleBetterGroupCommand(ChatHandler* handler, char const* args);
   

private:
    bool    enabled = false;
    bool    creatureScalingEnabled = false;
    bool    groupXPCompensationEnabled = false;
    bool    disableXPCompensationInRaid = false;
    bool    compensateGrayPenalty = false;
    float   detectionRadius = 100.0f;
    float   compensationPct = 1.0f;
    float   maxXPCompensationRate = 1.0f;
    uint32  maxGroupSize = 10;
    bool    damageScalingEnabled = true;
    bool    postCombatRefreshEnabled = false;
    bool    postCombatRefreshOutdoorOnly = true;
    uint32  postCombatRefreshMinGroupSize = 6;
    float   postCombatRefreshHealthPct = 95.0f;
    float   postCombatRefreshManaPct = 95.0f;
    float   hpScale[11] = { 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
    float   dmgScale[11] = { 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
};



