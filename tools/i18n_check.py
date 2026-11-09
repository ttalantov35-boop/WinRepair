#!/usr/bin/env python3
"""Validate lang/en.txt: placeholder counts, duplicate keys, risky short templates. Run from the project root."""
import sys
bad = 0
seen = {}
for n, line in enumerate(open('lang/en.txt', encoding='utf-8'), 1):
    line = line.rstrip('\n')
    if not line or line.startswith('#'):
        continue
    if ' ==> ' not in line:
        print(f'{n}: no separator'); bad += 1; continue
    k, v = line.split(' ==> ', 1)
    if k.count('{}') != v.count('{}'):
        print(f'{n}: placeholder mismatch ({k.count("{}")} vs {v.count("{}")}): {k[:70]}'); bad += 1
    if k in seen:
        print(f'{n}: duplicate key (first at {seen[k]}): {k[:70]}'); bad += 1
    seen[k] = n
    lit = len(k.replace('{}', ''))
    if '{}' in k and lit < 4:
        print(f'{n}: note: very short template (the engine captures only numbers/ASCII for it): {k}')
print(f'{len(seen)} entries, problems: {bad}')
sys.exit(1 if bad else 0)
