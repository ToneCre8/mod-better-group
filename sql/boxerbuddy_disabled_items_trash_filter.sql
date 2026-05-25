INSERT IGNORE INTO `mod_boxerbuddy_disabled_items` (`item`)
SELECT `entry`
FROM `item_template`
WHERE (
    `name` LIKE '%tablet%' OR
    `name` LIKE '%sulfuron%' OR
    `name` LIKE '%nightcrawlers%' OR
    `name` LIKE '%throwing dagger%' OR
    `name` LIKE '%shot pouch%' OR
    `name` LIKE '%brimstone%' OR
    `name` LIKE '%small pouch%' OR
    `name` LIKE '%dye%' OR
    `name` LIKE '%ironwood seed%' OR
    `name` LIKE '%stranglethorn seed%' OR
    `name` LIKE '%simple wood%' OR
    `name` LIKE '%bleach%' OR
    `name` LIKE '%flour%' OR
    `name` LIKE '%brew%' OR
    `name` LIKE '%parchment%' OR
    `name` LIKE '%light quiver%' OR
    `name` LIKE '%honey%' OR
    `name` LIKE '%/%' OR
    `name` LIKE '%creeping anguish%' OR
    `name` LIKE '%felcloth bag%' OR
    `name` LIKE '%elementium ore%' OR
    `name` LIKE '%unused%' OR
    `name` LIKE '%lava core%' OR
    `name` LIKE '%fiery core%' OR
    `name` LIKE '%sulfuron ingot%' OR
    `name` LIKE '%sak%' OR
    `name` LIKE '%gigantique%' OR
    `name` LIKE '%portable hole%' OR
    `name` LIKE '%deptecated%' OR
    `name` LIKE '%durability%' OR
    `name` LIKE '%big sack%' OR
    `name` LIKE '%decoded%' OR
    `name` LIKE '%knowledge:%' OR
    `name` LIKE '%manual%' OR
    `name` LIKE '%gnome head%' OR
    `name` LIKE '%critter enlarger%' OR
    `name` LIKE '%box of%' OR
    `name` LIKE '%summoning%' OR
    `name` LIKE '%turtle egg%' OR
    `name` LIKE '%heavy crate%' OR
    `name` LIKE '%assasin throwing axe%' OR
    `name` LIKE '%sack of gems%' OR
    `name` LIKE '%plans: darkspear%' OR
    `name` LIKE '%of swords%' OR
    `name` LIKE '%gnomish alarm%' OR
    `name` LIKE '%world enlarger%' OR
    `name` LIKE '%tome%' OR
    `name` LIKE '%ornate spyglass%' OR
    `name` LIKE '%test%' OR
    `name` LIKE '%darkmoon prize%' OR
    `name` LIKE '%codex%' OR
    `name` LIKE '%grimoire%' OR
    `name` LIKE '%deprecated%' OR
    `name` LIKE '%book%' OR
    `name` LIKE '%libram%' OR
    `name` LIKE '%guide%'
)
OR `name` COLLATE utf8mb4_bin LIKE '%OLD%'
OR UPPER(`name`) LIKE '%NPC%'
OR UPPER(`name`) LIKE '%QA%'
OR (`class` = 0 AND `subclass` = 5 AND `requiredlevel` < 40);
