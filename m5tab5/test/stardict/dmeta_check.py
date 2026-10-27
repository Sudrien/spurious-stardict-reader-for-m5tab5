#!/usr/bin/env python3
"""Compare main/dmeta.c with the Python Metaphone package's doublemetaphone().

    pip install metaphone
    make -C test/stardict
    test/stardict/dmeta_check.py ../out/<name>/<name>.idx

Every headword of the .idx goes through both; the keys are compared at
dmeta.c's length (4). The package follows the reference implementation
less closely than dmeta.c in three places, and words that differ only
because of them are counted, not listed:

  GH   after a vowel at the second or third letter, the package keeps the
       previous letter's result instead of applying Parker's rule;
  MB   it looks for -UMB- from the M rather than from the letter before;
  W    an initial W before a vowel stops there, where the reference goes
       on to the Arnow/-OWSKI checks;

and it puts a space in an alternate key where dmeta.c puts nothing. Two
more come from its own structure: a character that is neither a letter
nor a space (an apostrophe, a full stop, a digit) repeats the previous
letter's code, and its list of letters after CH that make it K leaves
out the space the reference has (Czech Republic).
Anything else that differs is listed, and is a bug in one or the other.
"""
import subprocess
import sys
import unicodedata
from pathlib import Path

from metaphone import doublemetaphone

N = 4


def headwords(idx):
    data = Path(idx).read_bytes()
    p = 0
    while p < len(data):
        z = data.index(b"\0", p)
        yield data[p:z].decode("utf-8", "replace")
        p = z + 9


def explained(w):
    u = "".join(c for c in unicodedata.normalize("NFD", w)
                if unicodedata.category(c) != "Mn").upper()
    return ("GH" in u[1:4] or "MB" in u or (u[:1] == "W" and len(u) > 1)
            or "J" in u or "LL" in u or "CH " in u
            or any(not ("A" <= c <= "Z" or c == " ") for c in u))


def main():
    words = [w for w in headwords(sys.argv[1]) if "\n" not in w]
    exe = Path(__file__).with_name("dmeta_dump")
    out = subprocess.run([str(exe)], input="\n".join(words) + "\n",
                         capture_output=True, text=True, check=True).stdout
    same = explained_n = 0
    odd = []
    for line, w in zip(out.splitlines(), words):
        _, cp, ca = line.split("\t")
        pp, pa = doublemetaphone(w)
        pa = pa or pp
        pp, pa = pp.replace(" ", "")[:N], pa.replace(" ", "")[:N]
        if (cp, ca) == (pp, pa):
            same += 1
        elif explained(w):
            explained_n += 1
        else:
            odd.append(f"{w!r}: dmeta.c {cp}/{ca}, package {pp}/{pa}")
    print(f"{len(words)} words: {same} agree, {explained_n} differ in a "
          f"known place, {len(odd)} otherwise")
    for o in odd[:40]:
        print("  " + o)
    return 1 if odd else 0


if __name__ == "__main__":
    sys.exit(main())
