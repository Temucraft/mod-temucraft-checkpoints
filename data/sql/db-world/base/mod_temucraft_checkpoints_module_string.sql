-- mod-temucraft-checkpoints: player-facing texts. Translations go in module_string_locale (same module and id, plus the locale).
DELETE FROM `module_string` WHERE `module` = 'mod-temucraft-checkpoints';
INSERT INTO `module_string` (`module`, `id`, `string`) VALUES
('mod-temucraft-checkpoints', 1, 'Checkpoint awakened: '),
('mod-temucraft-checkpoints', 2, '|cff66ccff[Checkpoint]|r {} is awake: you come back here if you fall.'),
('mod-temucraft-checkpoints', 3, '|cff66ccff[Checkpoint]|r This run''s furthest checkpoint is {}: you come back there if you fall.'),
('mod-temucraft-checkpoints', 4, 'Travel to '),
('mod-temucraft-checkpoints', 5, 'No checkpoints on this map.'),
('mod-temucraft-checkpoints', 6, '{} [{}] {} - {} {} - {}{} (guid {}, {:.0f} yd)'),
('mod-temucraft-checkpoints', 7, 'Unlock type: 0 always, 1 creature entry dead, 2 boss state done, 3 walk past (value = yards).'),
('mod-temucraft-checkpoints', 8, 'Gameobject entry {} does not exist: the row was added without a spawn.'),
('mod-temucraft-checkpoints', 9, 'Checkpoint {} ''{}'' added (sort {}, guid {}).'),
('mod-temucraft-checkpoints', 10, 'Checkpoints: {} imported from gameobject (unlock: walk past; set name/unlock in mod_temucraft_checkpoint), {} positions updated from their gameobject.'),
('mod-temucraft-checkpoints', 11, 'No checkpoints in this instance.'),
('mod-temucraft-checkpoints', 12, '{} checkpoints awake in this instance.'),
('mod-temucraft-checkpoints', 13, 'No checkpoint {}.'),
('mod-temucraft-checkpoints', 14, 'Usage: .checkpoint link <id> <all|any> <links>  (creature entries, b<boss index>, c<checkpoint id>)'),
('mod-temucraft-checkpoints', 15, 'Checkpoint {} now wakes when {} of: {} ({} links).'),
('mod-temucraft-checkpoints', 16, 'Checkpoints reloaded: {} rows, {} new spawns (they appear in running instances when their grid loads again).'),
('mod-temucraft-checkpoints', 17, '|cff66ccff[Group]|r {} of your group are in {}. Accept the summon to join them.'),
('mod-temucraft-checkpoints', 18, '|cff66ccff[Group]|r {} of your group {} in {}. Accept the summon to go to the meeting stone outside.'),
('mod-temucraft-checkpoints', 19, 'Nobody in your group is in a dungeon you can be summoned to right now (or you are in combat, dead, flying or in an instance).');
