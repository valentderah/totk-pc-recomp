# Items menu

The setup builds the item list from your own game files. Open F1 → Items to search by item name or actor ID, browse categories, and keep favorites and recent selections.

**Spawn near Link** creates 1–10 copies in the world. **Add to inventory** adds one material, weapon, bow, or shield directly to the pouch. After loading a save, pick up any ordinary item once to initialize the game's pickup context; the inventory button then becomes available. Resume play after selecting either action.

The inventory action creates a temporary actor below Link, passes it through the game's pickup grant, and removes it through the pickup cleanup path. It is available only with game version 1.4.3. Armor, meals, key items, and other categories need separate handling and are disabled for inventory adds. World spawning also excludes categories whose actor lifecycle has not been checked.

The item list is generated during setup as `items.tsv` in the install folder. The source repository contains the generator, not the game-derived list.
