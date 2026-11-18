#!/usr/bin/env python3
"""Builds the default mod_temucraft_checkpoint rows for every dungeon and raid the server has map data for.

Inputs (tab separated, exported from a world database):
  enc.tsv  instance_encounters: entry, creditType, creditEntry, lastEncounterDungeon, comment
  at.tsv   areatrigger_teleport: id, target_map, x, y, z, o
  cre.tsv  creature: guid, id, map, x, y, z, o, spawnMask
  maps.tsv Map.dbc scan: id, instanceType, hasData, name, directory, corpseMap
and DungeonEncounter.dbc. One checkpoint at each dungeon's entrance (always awake), then one on the spawn point of
each boss killed by a plain creature kill (awake once that creature is dead), in encounter order. The last boss
of a dungeon gets none."""
import struct, sys, os
d = sys.argv[1]; dbcpath = sys.argv[2]; out = sys.argv[3]
def tsv(n): return [l.rstrip('\n').split('\t') for l in open(os.path.join(d, n)) if l.strip()]
raw = open(dbcpath, 'rb').read(); n, f, rs, ss = struct.unpack('<4x4I', raw[:20]); strs = raw[20 + n * rs:]
def st(o): return strs[o:strs.index(b'\0', o)].decode('utf8', 'replace')
dbc = {}
for i in range(n):
    r = struct.unpack_from('<%dI' % f, raw, 20 + i * rs)
    dbc[r[0]] = (r[1], r[2], r[3], st(r[5]), r[4])    # map, difficulty, order, name, encounter bit
maps = {int(r[0]): r for r in tsv('maps.tsv') if r[1] in ('1', '2') and r[2] == 'True'}
spawns = {}
for g, e, m, x, y, z, o, sm in tsv('cre.tsv'):
    spawns.setdefault((int(m), int(e)), (x, y, z, o))
entr = {}
for i, m, x, y, z, o in tsv('at.tsv'):
    entr.setdefault(int(m), (x, y, z, o))
per = {}
for e, ct, ce, last, cm in tsv('enc.tsv'):
    if int(e) not in dbc: continue
    m, diff, order, name, bit = dbc[int(e)]
    if m not in maps or last != '0' or diff != 0: continue
    p = spawns.get((m, int(ce))) if ct == '0' else None
    # spawned bosses: on their spawn point, awake when dead; the others (summoned, or credited by a spell): awake
    # when the encounter is completed, placed where the group stands at that moment
    per.setdefault(m, []).append((order, name or cm, int(ce) if p else bit, p))
def q(s): return "'" + s.replace("\\", "\\\\").replace("'", "''") + "'"
rows = []; wid = 1
for m in sorted(maps):
    if m in entr:
        x, y, z, o = entr[m]
        rows.append(f"({wid}, {m}, {q('Entrance')}, 0, 0, 0, {x}, {y}, {z}, {o}, 608200, 0, 1, {q(maps[m][3] + ': entrance')})"); wid += 1
    for rank, (order, name, value, p) in enumerate(sorted(per.get(m, []), key=lambda t: (t[0], t[1])), 1):
        if p:
            x, y, z, o = p
            rows.append(f"({wid}, {m}, {q(name)}, 1, {value}, {rank}, {x}, {y}, {z}, {o}, 608200, 0, 1, {q(maps[m][3] + ': after ' + name)})")
        else:
            rows.append(f"({wid}, {m}, {q(name)}, 6, {value}, {rank}, 0, 0, 0, 0, 608200, 0, 1, {q(maps[m][3] + ': after ' + name + ' (where the group stands)')})")
        wid += 1
with open(out, 'w') as fo:
    fo.write("-- Default checkpoints (tools/generate_checkpoints.py): every dungeon's and raid's entrance, then each boss.\n")
    fo.write("-- INSERT IGNORE keeps the rows you changed and the guid the server filled in. Your own rows: id 100000 and up.\n")
    fo.write("INSERT IGNORE INTO `mod_temucraft_checkpoint` (`id`, `map`, `name`, `unlock_type`, `unlock_value`, `sort`, `position_x`, `position_y`, `position_z`, `orientation`, `gameobject_entry`, `guid`, `enabled`, `comment`) VALUES\n")
    fo.write(",\n".join(rows) + ";\n")
print(len(rows), "rows,", len(maps), "maps,", sum(len(v) for v in per.values()), "boss checkpoints,", sum(1 for v in per.values() for t in v if not t[3]), "placed at the kill")
