# mod-better-group

`mod-better-group` is an AzerothCore module that makes small-group open-world play feel better by:

- scaling normal outdoor creatures when multiple nearby group members engage them
- reducing or removing the normal group XP penalty so grouped players can still level efficiently together
- optionally restoring health/mana after combat for large outdoor groups using mage food/water-style level tiers
- optionally awarding grouped players a Satchel of Helpful Goods on level-up beginning at level 15
- adding optional level-appropriate random armor/weapon loot for 6-10 player outdoor groups

It is especially useful on servers that encourage duo or party leveling, including playerbot-heavy environments where groups may be larger than a typical solo questing setup.

## Features

### Dynamic open-world creature scaling

When a normal outdoor creature enters combat with a player-controlled unit, the module counts nearby alive group members and applies configurable multipliers for:

- maximum health
- outgoing melee damage
- outgoing spell damage
- outgoing periodic damage

Scaling is applied once at combat start, then removed when the creature dies or evades.

The module intentionally does **not** scale:

- elite, rare-elite, or world-boss creatures
- pets, totems, and summons
- critters
- guards
- trigger/invisible NPCs
- creatures inside instanced content such as dungeons or raids

### Group XP compensation

AzerothCore normally reduces each player's XP share while grouped. This module can slide that reward back toward solo-equivalent XP on a configurable basis.

Examples with the default formula:

- `CompensationPct = 0.0` keeps the normal group XP penalty
- `CompensationPct = 0.5` restores half of the lost XP
- `CompensationPct = 1.0` gives each member up to solo-equivalent XP per kill

You can also choose whether raid groups are excluded and whether gray-level mismatch penalties should be compensated.

### Post-combat refresh

The optional post-combat refresh feature restores health and/or mana directly for up to 30 seconds after a player leaves combat, gated by nearby group size and health/mana thresholds. The rates are tuned around mage conjured food/water tiers, but no food or drink aura is applied, so players do not need to sit.

### Level-up satchels

The optional level-up satchel feature awards one level-appropriate Satchel of Helpful Goods when a grouped player gains a level. By default it requires a party or raid, starts at level 15, and stops at level 79, using the highest low-level LFG satchel tier above level 70. If bags are full, the satchel is sent through item retrieval mail.

### Random group loot

The optional random group loot feature adds one bonus armor or weapon item directly to an eligible creature's loot when enough nearby group members participated. It is intended as a gearing path for 6-10 player outdoor raid groups.

The item pool is built from creature loot tables used by spawned creatures inside instance maps, including one level of `reference_loot_template` entries. The resulting items are validated against `item_template` and filtered for quest/temp items, unsupported qualities, non-gear items, relic slot items, and item IDs listed in `mod_bettergroup_disabled_items` when that table exists. It intentionally does not filter by player class, race, armor proficiency, or weapon proficiency, because the bonus is random group loot rather than personal smart loot.

## Installation

1. Place this module in your AzerothCore `modules` directory:

   ```text
   modules/mod-better-group
   ```

2. Re-run CMake and rebuild AzerothCore so the module is compiled into the server.

3. Copy the distributed config file into your server configuration directory:

   ```text
   conf/better_goup_module.conf.dist
   ```

   Rename it to:

   ```text
   better_goup_module.conf
   ```

4. Restart the worldserver.

> Note: the config filename currently uses `goup` rather than `group`; keep the existing spelling unless you also rename the file in the module and update your deployment flow accordingly.

## Configuration

The main switches are:

| Setting | Purpose | Default |
| --- | --- | --- |
| `BetterGroup.Enable` | Master switch for the module | `1` |
| `DynamicCreatureScaling.Enable` | Enables creature scaling | `1` |
| `DynamicCreatureScaling.DetectionRadius` | Radius used to count nearby group members | `100.0` |
| `DynamicCreatureScaling.MaxGroupSize` | Maximum group size considered for scaling, supported up to 10 | `10` |
| `DynamicCreatureScaling.ScaleDamage` | Enables outgoing damage scaling in addition to HP scaling | `1` |
| `GroupXPCompensation.Enable` | Enables XP compensation | `1` |
| `GroupXPCompensation.CompensationPct` | Portion of the normal group XP penalty to restore | `1.0` |
| `GroupXPCompensation.MaxRate` | Cap on per-player XP after compensation | `1.0` |
| `GroupXPCompensation.DisableInRaid` | Prevents compensation in raid groups | `0` |
| `GroupXPCompensation.CompensateGrayPenalty` | Also offsets gray-level mismatch XP reduction | `0` |
| `BetterGroup.PostCombatRefresh.Enable` | Enables automatic post-combat health/mana refresh | `0` |
| `BetterGroup.PostCombatRefresh.MinGroupSize` | Nearby group size required for post-combat refresh | `6` |
| `BetterGroup.PostCombatRefresh.HealthPct` | Health threshold for direct refresh | `95.0` |
| `BetterGroup.PostCombatRefresh.ManaPct` | Mana threshold for direct refresh | `95.0` |
| `BetterGroup.LevelUpSatchel.Enable` | Enables Satchel of Helpful Goods awards on level-up | `1` |
| `BetterGroup.LevelUpSatchel.RequireGroup` | Requires the player to be in a party or raid | `1` |
| `BetterGroup.LevelUpSatchel.MinLevel` | First level that awards a satchel | `15` |
| `BetterGroup.LevelUpSatchel.MaxLevel` | Last level that awards a satchel, or `0` for no cap | `79` |
| `BetterGroup.RandomLoot.Enable` | Enables random bonus group loot | `0` |
| `BetterGroup.RandomLoot.MinGroupSize` | Nearby group size required for bonus loot rolls | `6` |
| `BetterGroup.RandomLoot.RequiredLevelWindow` | Item required-level range around the creature level | `3` |
| `BetterGroup.RandomLoot.NormalGreenChancePct` | Green bonus roll chance for normal mobs | `8.0` |
| `BetterGroup.RandomLoot.NormalBlueChancePct` | Blue bonus roll chance for normal mobs | `1.0` |
| `BetterGroup.RandomLoot.EliteGreenChancePct` | Green bonus roll chance for elite/rare mobs | `35.0` |
| `BetterGroup.RandomLoot.EliteBlueChancePct` | Blue bonus roll chance for elite/rare mobs | `10.0` |

### Default scaling table

| Nearby players | HP multiplier | Damage multiplier |
| --- | ---: | ---: |
| 1 | `1.0x` | `1.00x` |
| 2 | `1.5x` | `1.20x` |
| 3 | `2.0x` | `1.35x` |
| 4 | `2.6x` | `1.50x` |
| 5 | `3.2x` | `1.65x` |
| 6 | `4.5x` | `1.90x` |
| 7 | `5.5x` | `2.20x` |
| 8 | `6.8x` | `2.55x` |
| 9 | `8.2x` | `2.95x` |
| 10 | `10.0x` | `3.40x` |

All values can be tuned in the module config file.

## Behavior notes

- Only group members who are alive, on the same map, and within the configured radius are counted.
- Scaling is locked at combat start; players joining or leaving afterward do not change that creature's multipliers mid-fight.
- Groups larger than the configured maximum are treated as that maximum for scaling lookup.
- On evade, scaled creatures are restored to their original maximum health while preserving their current health percentage.
- With `CompensationPct = 1.0`, total XP awarded across a party can be much higher than a solo kill because each eligible member can receive solo-equivalent XP. Lower the value if you want softer progression.
- Keep `GroupXPCompensation.DisableInRaid = 0` if you want 6-10 player raid groups to receive compensated XP. With it enabled, a same-level 10-player outdoor raid receives AzerothCore's raw split, roughly 14% of solo XP before rounding and other modifiers.
- When `CompensateGrayPenalty = 1`, the module may temporarily raise the internal pre-penalty XP rate above `MaxRate` so the final post-penalty reward can still respect that configured cap.
- Random group loot uses the creature level, not the loot recipient's class or role, when choosing eligible gear.
- To use the disabled-item filter, apply the SQL files in `sql/` to the world database before starting the server.

## Commands

The module exposes GM-only inspection commands:

| Command | Purpose |
| --- | --- |
| `.group better status` | Show the current module configuration and number of actively tracked scaled creatures |
| `.group better scales` | Show the configured HP and damage multipliers up to the configured max group size |
| `.group better target` | Inspect the selected creature's scaling eligibility and live scaling state |

Use `.group better` or `.group better help` to print the command list in-game.

## License

This module is distributed under the GNU General Public License. See [LICENSE](LICENSE).
