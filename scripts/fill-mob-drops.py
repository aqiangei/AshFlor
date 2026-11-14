"""Every authored item drops on every loot trial.

A mob tier whose chances total under 100% hands the missing share to a lower
rarity: the lowest one already authored when the tier spans several, otherwise
the rarity just below the authored one. Petals that only exist above common
(Third Eye and the like) are rare by design and keep their reference chances,
and so does a row authored at no more than RARE_DROP_PERCENT on every tier
(Ruby from a Server, Meteor from a Rock, Mimic, Splitter): those are prizes,
not the mob's ordinary loot. Rows that share a roll group are one draw, so the
group is topped up as a whole, in proportion to each row's authored share.

Usage: python scripts/fill-mob-drops.py [report.json]
"""
import json
import sys
from pathlib import Path

RARITIES = ['common', 'unusual', 'rare', 'epic', 'legendary', 'mythic', 'ultra', 'super', 'unique', 'apex']
POLICY = 'fill-to-certain-with-lower-rarity'
RARE_DROP_PERCENT = 2.5


def designed_rare(row):
    totals = [sum(percent for _, percent in tier) for tier in row['rarityChances']]
    return max(totals, default=0) <= RARE_DROP_PERCENT


def fill_target(pairs, floor):
    if floor > 0:
        return -1
    lowest = min(rarity for rarity, _ in pairs)
    return lowest if len(pairs) > 1 else max(0, lowest - 1)


def add_share(pairs, rarity, percent):
    for pair in pairs:
        if pair[0] == rarity:
            pair[1] = round(pair[1] + percent, 6)
            break
    else:
        pairs.append([rarity, round(percent, 6)])
    pairs.sort(key=lambda pair: pair[0])


def fill_mob_drops(tables, floor_of):
    report = []
    for mob, table in tables.items():
        if table.get('fillPolicy') == POLICY:
            continue
        rows = [row for row in table.get('drops', []) if isinstance(row.get('rarityChances'), list)]
        prizes = {id(row) for row in rows if designed_rare(row)}
        tiers = max((len(row['rarityChances']) for row in rows), default=0)
        changed = False
        for tier in range(tiers):
            draws, group = [], []
            for row in rows:
                if tier < len(row['rarityChances']) and row['rarityChances'][tier]:
                    if row.get('rollGroup'):
                        group.append(row)
                    else:
                        draws.append([row])
            if group:
                draws.append(group)
            for draw in draws:
                authored = [sum(percent for _, percent in row['rarityChances'][tier]) for row in draw]
                missing = round(100 - sum(authored), 6)
                if missing <= 0:
                    continue
                fillable = [(row, share, fill_target(row['rarityChances'][tier], floor_of(row['itemType'])))
                            for row, share in zip(draw, authored)]
                fillable = [entry for entry in fillable
                            if entry[2] >= 0 and entry[1] > 0 and id(entry[0]) not in prizes]
                weight = sum(share for _, share, _ in fillable)
                if weight <= 0:
                    continue
                for row, share, target in fillable:
                    pairs = row['rarityChances'][tier]
                    before = [list(pair) for pair in pairs]
                    add_share(pairs, target, missing * share / weight)
                    report.append({'mob': mob, 'item': row['itemType'], 'tier': RARITIES[tier],
                                   'before': before, 'after': [list(pair) for pair in pairs]})
                changed = True
        if changed:
            table['fillPolicy'] = POLICY
    return report


def petal_floors(petals):
    def floor_of(item):
        name = str((petals.get(item) or {}).get('minRarity') or 'common').lower()
        return RARITIES.index(name) if name in RARITIES else 0
    return floor_of


def main():
    data = Path(__file__).resolve().parent.parent / 'data'
    path = data / 'mob_drops.json'
    newline = '\r\n' if b'\r\n' in path.read_bytes()[:4096] else '\n'
    tables = json.loads(path.read_text(encoding='utf-8'))
    petals = json.loads((data / 'petals.json').read_text(encoding='utf-8'))
    report = fill_mob_drops(tables, petal_floors(petals))
    with open(path, 'w', encoding='utf-8', newline=newline) as out:
        out.write(json.dumps(tables, indent=2) + '\n')
    if len(sys.argv) > 1:
        Path(sys.argv[1]).write_text(json.dumps(report, ensure_ascii=False, indent=1) + '\n', encoding='utf-8')
    print(f'filled {len(report)} tier rows in {len({entry["mob"] for entry in report})} mobs')


if __name__ == '__main__':
    main()
