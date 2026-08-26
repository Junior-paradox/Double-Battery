#!/usr/bin/env python3
"""Generate src/city_data.h from the WIB open hospital dataset.

    scripts/gen_city_data.py [--input hospitals.json] [--out src/city_data.h]

The engine ships the table compiled in rather than fetching it at runtime: a
dispatch daemon that needs an internet round trip to learn which hospitals
exist is exactly the thing this project is arguing against, and a rural
district is where the connection is worst. Regenerating is a deliberate,
reviewable act -- `make city-data`, then read the diff.

Source: https://wibest.in/data/json/hospitals.json  (CC-BY 4.0)

WHAT IS REAL AND WHAT IS NOT
  real       hospital names, cities, street localities, ownership, reported
             bed counts, NABH accreditation, listed specialties
  derived    the 8 capability bits, mapped from the free-text specialty
             strings by SPEC_MAP below
  inferred   a capability granted by the referral fallback, because the city
             had no hospital listing it at all (see designate_referral)
  synthetic  road layout, junction positions, ambulances, doctors, shift
             rotas, medicine stock, queue lengths, live bed occupancy

Nothing in the dataset carries coordinates, so hospitals are placed on the
generated street grid, not at their real locations. The UI says so.
"""

import argparse
import json
import sys
import urllib.request

URL = 'https://wibest.in/data/json/hospitals.json'

# Capability bits. Must stay in step with the CAP_* enum in src/dispatch.h.
CAPS = ['TRAUMA', 'CARDIAC', 'NEURO', 'BURNS', 'OBSTETRIC', 'PAEDS', 'TOXICOL', 'ICU']
BIT = {c: 1 << i for i, c in enumerate(CAPS)}

# Free-text specialty -> capability. Substring match, lowercased.
#
# Two of these are judgement calls worth stating out loud:
#   TOXICOL  no Indian hospital advertises a "toxicology department". Acute
#            poisoning is managed by emergency medicine and critical care, so
#            that is what the bit is mapped from.
#   TRAUMA   likewise: outside the dedicated trauma centres, the department
#            that receives a road accident is orthopaedics or general surgery.
SPEC_MAP = {
    'TRAUMA':    ['trauma', 'emergency', 'accident', 'orthopedic', 'orthopaedic',
                  'joint replacement', 'spine', 'sports medicine', 'general surgery',
                  'surgery'],
    'CARDIAC':   ['cardi'],
    'NEURO':     ['neuro', 'stroke'],
    'BURNS':     ['burn', 'plastic', 'reconstructive'],
    'OBSTETRIC': ['gynae', 'gynec', 'obstet', 'maternity', 'ivf', 'fertility'],
    'PAEDS':     ['paediatric', 'pediatric', 'neonat', 'child'],
    'TOXICOL':   ['toxic', 'poison', 'critical care', 'intensive',
                  'emergency', 'general medicine', 'internal medicine'],
    'ICU':       ['critical care', 'intensive', 'icu', 'transplant',
                  'cardiac surgery', 'cardiothoracic', 'neurosurgery', 'robotic'],
}

TYPE_CODE = {'Private': 0, 'Government': 1, 'Trust': 2}

# Cities that lead the picker, in this order. Everything else follows by size.
FEATURED = ['Mumbai', 'Delhi', 'Bangalore', 'Pune', 'Chennai', 'Hyderabad',
            'Kolkata', 'Ahmedabad']
# Below this a city cannot fill eight departments plausibly, so it is dropped
# rather than padded out with inferred referral centres.
MIN_HOSPITALS = 8


def caps_of(hospital):
    mask = 0
    for spec in hospital['specialties']:
        low = spec.lower()
        for cap, keys in SPEC_MAP.items():
            if any(k in low for k in keys):
                mask |= BIT[cap]
    return mask


def locality(address, city):
    """The neighbourhood, for tooltips: the segment before the city+pincode."""
    parts = [p.strip() for p in address.split(',') if p.strip()]
    if len(parts) >= 2:
        part = parts[-2]
    elif parts:
        part = parts[0]
    else:
        return city
    # drop a leading house/plot number: "1 Press Enclave Marg" -> unchanged,
    # but "21 Greams Lane" keeps its name and "1-8-31/1" alone is useless.
    if part and not any(ch.isalpha() for ch in part):
        return city
    return part


def designate_referral(rows):
    """Give a city every capability, and record which ones we made up.

    A city with no listed burns unit is a gap in the dataset, not a city where
    burns patients are turned away -- hospital websites list what they market,
    and burns and poisoning are not marketed. Left alone, every burns call in
    29 of the 34 cities would fail with "no such department", which says
    something about the source data and nothing about the dispatch problem.

    So the largest government or trust hospital -- which is what a real state
    referral network leans on for exactly these cases -- is designated for any
    capability nobody lists. The bit is flagged as inferred and the UI labels
    it, so no one mistakes it for something the dataset said.
    """
    for cap in CAPS:
        if any(r['caps'] & BIT[cap] for r in rows):
            continue
        public = [r for r in rows if r['type'] in ('Government', 'Trust')]
        target = max(public or rows, key=lambda r: r['beds'] or 0)
        target['caps'] |= BIT[cap]
        target['inferred'] |= BIT[cap]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--input', help='local hospitals.json (default: fetch from wibest.in)')
    ap.add_argument('--out', default='src/city_data.h')
    args = ap.parse_args()

    if args.input:
        with open(args.input) as f:
            doc = json.load(f)
    else:
        print(f'fetching {URL}', file=sys.stderr)
        with urllib.request.urlopen(URL, timeout=30) as r:
            doc = json.load(r)

    rows = []
    for h in doc['data']:
        rows.append({
            'name': h['name'],
            'city': h['city'],
            'area': locality(h['address'], h['city']),
            'beds': int(h['beds'] or 0),
            'caps': caps_of(h),
            'inferred': 0,
            'type': TYPE_CODE.get(h['type'], 0),
            'nabh': 1 if h['nabh'] else 0,
            'rating10': int(round((h['rating'] or 0) * 10)),
        })

    by_city = {}
    for r in rows:
        by_city.setdefault(r['city'], []).append(r)
    by_city = {c: rs for c, rs in by_city.items() if len(rs) >= MIN_HOSPITALS}

    order = [c for c in FEATURED if c in by_city]
    order += sorted((c for c in by_city if c not in order),
                    key=lambda c: (-len(by_city[c]), c))

    flat, cities = [], []
    for city in order:
        # Biggest first: index 0 is the city's tertiary referral centre, which
        # is what the placement and staffing rules key off.
        rs = sorted(by_city[city], key=lambda r: (-r['beds'], r['name']))
        designate_referral(rs)
        cities.append((city, city.lower().replace(' ', '-'), len(flat), len(rs)))
        flat.extend(rs)

    out = []
    w = out.append
    w('/* Generated by scripts/gen_city_data.py -- DO NOT EDIT.')
    w(' * Run `make city-data` to refresh from the source dataset.')
    w(' *')
    w(f' * {doc.get("name", "hospital dataset")}, version {doc.get("version", "?")}')
    w(f' * Source: {doc.get("source", URL)}   License: {doc.get("license", "CC-BY-4.0")}')
    w(' *')
    w(' * Real: names, cities, localities, ownership, bed counts, accreditation,')
    w(' * and the specialties each hospital lists. Derived: the 8 capability bits.')
    w(' * The `inferred` mask marks bits granted by the referral fallback because')
    w(' * no hospital in that city listed the department at all -- read it as "the')
    w(' * dataset is silent here", not as a fact about the hospital.')
    w(' *')
    w(' * Everything else in the simulation -- streets, coordinates, ambulances,')
    w(' * doctors, shifts, medicine, queues -- is synthetic. The dataset carries')
    w(' * no coordinates, so these hospitals sit on a generated grid.')
    w(' */')
    w('#ifndef CITY_DATA_H')
    w('#define CITY_DATA_H')
    w('')
    w(f'#define CITY_DATA_VERSION "{doc.get("version", "?")}"')
    w('#define CITY_DATA_SOURCE  "WIB Open Data (wibest.in) CC-BY 4.0"')
    w('')
    w('/* name, area, beds, caps, inferred, type (0 private 1 govt 2 trust), nabh, rating x10 */')
    w('static const CityHospital CITY_HOSP[] = {')
    for r in flat:
        w('    {"%s","%s",%d,0x%02X,0x%02X,%d,%d,%d},'
          % (r['name'], r['area'], r['beds'], r['caps'], r['inferred'],
             r['type'], r['nabh'], r['rating10']))
    w('};')
    w('')
    w('/* name, slug, first index into CITY_HOSP, hospital count */')
    w('static const CityInfo CITY_LIST[] = {')
    for name, slug, first, count in cities:
        w('    {"%s","%s",%d,%d},' % (name, slug, first, count))
    w('};')
    w('')
    w('#define N_CITY   (sizeof(CITY_LIST) / sizeof(CITY_LIST[0]))')
    w('#define N_CITY_HOSP (sizeof(CITY_HOSP) / sizeof(CITY_HOSP[0]))')
    w('')
    w('#endif')
    w('')

    text = '\n'.join(out)
    if any(ord(ch) > 126 for ch in text):
        sys.exit('refusing to write non-ASCII into a C source file')
    with open(args.out, 'w') as f:
        f.write(text)

    inferred = sum(bin(r['inferred']).count('1') for r in flat)
    print(f'{args.out}: {len(cities)} cities, {len(flat)} hospitals, '
          f'{inferred} inferred referral designations', file=sys.stderr)
    for name, _, first, count in cities[:len(FEATURED)]:
        beds = sum(r['beds'] for r in flat[first:first + count])
        print(f'  {name:<12} {count:3d} hospitals  {beds:6d} reported beds', file=sys.stderr)


if __name__ == '__main__':
    main()
