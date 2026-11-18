// mod-temucraft-checkpoints: what the group summon needs from the checkpoints
#ifndef TEMUCRAFT_CHECKPOINTS_H
#define TEMUCRAFT_CHECKPOINTS_H

#include "Position.h"

class Map;

// The furthest awake checkpoint of a running instance, if it has any
bool TemucraftFurthestCheckpoint(Map* map, Position& out);

#endif
