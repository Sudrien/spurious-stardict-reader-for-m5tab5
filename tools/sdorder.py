#!/usr/bin/env python3
"""Find where a StarDict .idx/.syn pair breaks the reader's order.

Mirrors m5tab5/main/stardict.c: ASCII-only case fold (bytes >= 0x80 kept),
then strcmp; .idx and .syn each checked alone, then merged with ties to .idx.
usage: sdorder.py path/to/name.ifo
"""
import gzip, struct, sys
from functools import cmp_to_key

def fold(b):
    return bytes(c + 32 if 65 <= c <= 90 else c for c in b)

def cmp(x, y):
    a, b = fold(x), fold(y)
    if a != b:
        return -1 if a < b else 1
    return (x > y) - (x < y)

def load(path):
    for p in (path, path + ".dz", path + ".gz"):
        try:
            data = open(p, "rb").read()
        except FileNotFoundError:
            continue
        return gzip.decompress(data) if data[:2] == b"\x1f\x8b" else data
    return None

def words(data, tail):
    out, q = [], 0
    while q < len(data):
        z = data.index(b"\0", q)
        out.append(data[q:z])
        q = z + 1 + tail
    return out

def first_break(name, ws):
    for k in range(1, len(ws)):
        if cmp(ws[k - 1], ws[k]) > 0:
            print(f"{name}: out of order at {k}: {ws[k-1]!r} > {ws[k]!r}")
            return True
    print(f"{name}: {len(ws)} words, in order")
    return False

base = sys.argv[1][:-4]
ifo = open(sys.argv[1], encoding="utf-8").read()
tail = 12 if "idxoffsetbits=64" in ifo else 8
idx = words(load(base + ".idx"), tail)
syn = words(load(base + ".syn") or b"", 4)
bad = first_break(".idx", idx) | first_break(".syn", syn)
if not bad:
    merged = sorted(idx + syn, key=cmp_to_key(cmp))
    print("merged: in order" if len(merged) == len(idx) + len(syn) else "merged: ?")
