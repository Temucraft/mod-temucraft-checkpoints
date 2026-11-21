-- mod-temucraft-checkpoints: its game master commands as RBAC permissions (100201-100207). Each is linked to the
-- stock security role of its old level (194 moderator, 193 game master, 192 administrator) and to the matching
-- Temucraft GM tier (1011 Triager, 1012 Entertainer, 1014 Administrator). Grant or deny them per account with
-- .rbac account grant / deny, or link them to other roles.
DELETE FROM `rbac_linked_permissions` WHERE `linkedId` BETWEEN 100201 AND 100249;
DELETE FROM `rbac_permissions` WHERE `id` BETWEEN 100201 AND 100249;
INSERT INTO `rbac_permissions` (`id`, `name`) VALUES
(100201, 'Command: temucraft checkpoint list'),
(100202, 'Command: temucraft checkpoint add'),
(100203, 'Command: temucraft checkpoint import'),
(100204, 'Command: temucraft checkpoint wake'),
(100205, 'Command: temucraft checkpoint go'),
(100206, 'Command: temucraft checkpoint reload'),
(100207, 'Command: temucraft checkpoint link');
INSERT INTO `rbac_linked_permissions` (`id`, `linkedId`) VALUES
(192, 100201),
(1014, 100201),
(192, 100202),
(1014, 100202),
(192, 100203),
(1014, 100203),
(192, 100204),
(1014, 100204),
(192, 100205),
(1014, 100205),
(192, 100206),
(1014, 100206),
(192, 100207),
(1014, 100207);
