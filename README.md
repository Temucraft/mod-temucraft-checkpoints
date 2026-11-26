# mod-temucraft-checkpoints

Dungeon checkpoints, shared by the whole group: killing a boss gives everyone in that run a new respawn point,
whether they were there for the kill or not, and so does anyone who enters the run later. Each dungeon and raid has checkpoints: one at the entrance, awake from the start, and one on the
spawn point of each boss but the last, which awakens when that boss is dead. A checkpoint's stone stays hidden until it awakens
(`HideDormant`), so nothing stands in a boss's room to use before the boss is dead, and each new run of a dungeon
starts with only its entrance awake (a reset run used to keep the old run's awake checkpoints when it got the same
instance id). Releasing your spirit brings you back to
life at the furthest awake checkpoint of your instance instead of the graveyard outside. When a checkpoint awakens the
whole instance is told, and the checkpoint you will come back at is flagged on the minimap and world map. Talking to an
awake checkpoint takes you to the other awake ones.

## The table

`mod_temucraft_checkpoint` (world database): `map`, `name`, how it awakens (`unlock_type`, `unlock_value`), `sort`
(highest awake one wins), position and `gameobject_entry`.

| unlock_type | unlock_value | awake when |
|---|---|---|
| 0 | - | always |
| 1 | creature entry | that creature is dead in the instance (its respawn timer runs, so it survives restarts) |
| 2 | boss index | the instance script marks that boss DONE |
| 3 | yards (0 = config) | a living player comes that close |
| 4 | - (uses `linked`) | every link in `linked` is met |
| 5 | - (uses `linked`) | any one link in `linked` is met |
| 6 | DungeonEncounter.dbc encounter index | the encounter is completed: works for summoned bosses and bosses credited by a spell |

A checkpoint at position 0, 0, 0 is placed where the group stands when it wakes (a stone is summoned there). The
defaults use this for the 53 bosses that have no spawn point.

`linked` lists creature entries (dead), `b<boss index>` (instance boss DONE) and `c<checkpoint id>` (awake), separated
by commas or spaces: `3914, 3886, b2` wakes once both creatures are dead and boss 2 is done. Set it with
`.temucraft checkpoint link <id> <all|any> <links>`.

Entering a run that is past its entrance takes you straight to its furthest awake checkpoint (not when you log in
inside).

Rows without a spawn get one at startup: the server writes the `gameobject` row and stores its guid in `guid`, so the
checkpoints are ordinary gameobjects you can see, move and export like any other. `enabled = 0` removes the spawn at
the next start. Your own rows: id 100000 and up.

The default rows (418 in 92 dungeons and raids) come from `tools/generate_checkpoints.py`: area trigger entrances,
`instance_encounters` and the boss spawns. Summoned bosses and bosses credited by a spell use unlock type 6.

## Commands

- `.temucraft checkpoint list` - checkpoints of your map, awake or dormant in your instance, which one you'd come back at
- `.temucraft checkpoint add <unlock type> <unlock value> <name>` - a checkpoint where you stand
- `.temucraft checkpoint import` - gameobjects of a checkpoint entry without a row get one; rows of checkpoints moved with
  `.gobject move` take the gameobject's position
- `.temucraft checkpoint link <id> <all|any> <links>` - wake on several bosses (types 4/5)
- `.temucraft checkpoint wake` - wakes every checkpoint of your instance (testing)
- `.temucraft checkpoint go <id>`, `.temucraft checkpoint reload`

## Group summon

When part of your group is in a dungeon or raid and you are out in the world, you get the usual summon window. With
half or more of the group's online members inside, it takes you into their instance at its furthest awake checkpoint;
otherwise (or if you can't enter: level, lockout, raid group) it takes you to the meeting stone nearest the dungeon's
entrance. Declined offers come back after 5 minutes, at once if the group moves to another instance, or on `.temucraft rejoin`.

C++ only. Settings: `conf/mod-temucraft-checkpoints.conf.dist`.

## Texts and translations

The 19 player-facing texts (chat messages, gossip options) are in the `module_string` table under the module name
`mod-temucraft-checkpoints`, so they can be changed without rebuilding. Translations go in `module_string_locale` with the same
module and id plus the locale (`deDE`, `esES`, `frFR`, ...). If a row is missing the English text in the code is used.

Part of the Temucraft modules for AzerothCore 3.3.5a.
