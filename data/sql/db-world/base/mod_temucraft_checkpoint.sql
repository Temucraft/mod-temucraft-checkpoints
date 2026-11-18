-- mod-temucraft-checkpoints: dungeon checkpoints
CREATE TABLE IF NOT EXISTS `mod_temucraft_checkpoint` (
  `id`               INT UNSIGNED      NOT NULL COMMENT 'your own rows: 100000 and up',
  `map`              SMALLINT UNSIGNED NOT NULL,
  `name`             VARCHAR(100)      NOT NULL DEFAULT 'Checkpoint',
  `unlock_type`      TINYINT UNSIGNED  NOT NULL DEFAULT 0 COMMENT '0 always, 1 creature entry dead, 2 instance boss state DONE, 3 a player walks past, 4 all of linked, 5 any of linked, 6 DungeonEncounter.dbc encounter completed (position 0,0,0: placed where the group stands)',
  `unlock_value`     INT UNSIGNED      NOT NULL DEFAULT 0 COMMENT 'creature entry, boss index, or yards (0 = config)',
  `linked`           VARCHAR(255)      NOT NULL DEFAULT '' COMMENT 'types 4/5: creature entries (dead), b<boss index> (DONE), c<checkpoint id> (awake), e.g. 3914, 3886, b2',
  `sort`             INT UNSIGNED      NOT NULL DEFAULT 0 COMMENT 'you come back at the awake checkpoint with the highest sort',
  `position_x`       FLOAT             NOT NULL DEFAULT 0,
  `position_y`       FLOAT             NOT NULL DEFAULT 0,
  `position_z`       FLOAT             NOT NULL DEFAULT 0,
  `orientation`      FLOAT             NOT NULL DEFAULT 0,
  `gameobject_entry` INT UNSIGNED      NOT NULL DEFAULT 608200,
  `guid`             INT UNSIGNED      NOT NULL DEFAULT 0 COMMENT 'gameobject.guid, filled in by the server',
  `enabled`          TINYINT UNSIGNED  NOT NULL DEFAULT 1 COMMENT '0 also removes its gameobject at the next start',
  `comment`          VARCHAR(255)      NOT NULL DEFAULT '',
  PRIMARY KEY (`id`),
  KEY `map` (`map`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-temucraft-checkpoints';

-- The checkpoint (a meeting stone model; change displayId to taste)
DELETE FROM `gameobject_template` WHERE `entry` = 608200;
INSERT INTO `gameobject_template` (`entry`, `type`, `displayId`, `name`, `IconName`, `castBarCaption`, `unk1`, `size`, `AIName`, `ScriptName`, `VerifiedBuild`)
VALUES (608200, 10, 5491, 'Checkpoint', '', '', '', 0.8, '', '', 0);

DELETE FROM `npc_text` WHERE `ID` IN (608200, 608201);
INSERT INTO `npc_text` (`ID`, `text0_0`, `text0_1`, `Probability0`) VALUES
(608200, 'The checkpoint is dormant. Press on: it will awaken once the way here is safe.', 'The checkpoint is dormant. Press on: it will awaken once the way here is safe.', 1),
(608201, 'The checkpoint hums with power. Should you fall, you will rise again here.', 'The checkpoint hums with power. Should you fall, you will rise again here.', 1);
