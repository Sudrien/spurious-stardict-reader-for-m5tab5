#!/usr/bin/env python3
"""Convert a Kiwix Wiktionary ZIM dump to a StarDict dictionary.

    pip install -r tools/requirements.txt
    # or, on Debian/Ubuntu:
    sudo apt install python3-bs4 python3-libzim python3-pyglossary
    tools/zim2stardict.py wiktionary_en_all_nopic_2026-09.zim out/ --lang English

The script parses with lxml (python3-lxml), and .dict.dz needs python-idzip
or the dictzip utility on PATH; pass --no-dictzip to skip compression.

Writes out/<name>/<name>.{ifo,idx,dict[.dz],syn}.

Each ZIM article is a whole Wiktionary page, with one section per language.
Only the section named by --lang is kept as the definition; pages without
one are skipped. ZIM redirects become StarDict synonyms of their target,
so lookups by inflected or alternate spelling still land on the entry.

Definitions are stored as HTML ('h') by default, or as plain text ('m')
with --text, which is what a reader without an HTML renderer wants.
"""

import argparse
import re
import sys
from pathlib import Path

from bs4 import BeautifulSoup
from libzim.reader import Archive
from pyglossary.glossary_v2 import Glossary

# Tags that carry no definition text.
JUNK = ("script", "style", "link", "meta", "sup.reference", ".mw-editsection",
        "table.navbox", "div.NavFrame", "figure", "img")


def language_section(soup, lang):
    """Return the HTML of the `lang` section of a Wiktionary page, or None.

    Handles both layouts Kiwix has shipped: <section> wrappers around an
    <h2 id=lang>, and a flat run of siblings after the <h2>.
    """
    h2 = soup.find("h2", id=lang)
    if h2 is None:
        return None
    wrapper = h2.find_parent("section")
    if wrapper is not None and wrapper.find("h2") is h2:
        parts = [str(c) for c in wrapper.children if c is not h2]
    else:
        parts = []
        for sib in h2.next_siblings:
            if getattr(sib, "name", None) == "h2":
                break
            parts.append(str(sib))
    body = "".join(parts).strip()
    return body or None


def clean(html, as_text):
    soup = BeautifulSoup(html, "lxml")
    for sel in JUNK:
        for el in soup.select(sel):
            el.decompose()
    if as_text:
        text = soup.get_text("\n")
        text = re.sub(r"[ \t]+\n", "\n", text)
        return re.sub(r"\n{3,}", "\n\n", text).strip()
    body = soup.body or soup
    return "".join(str(c) for c in body.children).strip()


def entries(zim, lang, as_text, keep_ns, limit, stats):
    # Redirects first, so each real entry can carry them as synonyms.
    redirects = {}
    for i in range(zim.entry_count):
        e = zim._get_entry_by_id(i)
        if e.is_redirect:
            target = e.get_redirect_entry().path
            redirects.setdefault(target, []).append(e.title)

    done = 0
    for i in range(zim.entry_count):
        e = zim._get_entry_by_id(i)
        if e.is_redirect:
            continue
        item = e.get_item()
        if not item.mimetype.startswith("text/html"):
            continue
        title = e.title
        if not keep_ns and ":" in title:
            stats["namespace"] += 1
            continue
        soup = BeautifulSoup(bytes(item.content), "lxml")
        section = language_section(soup, lang)
        if section is None:
            stats["no_section"] += 1
            continue
        defi = clean(section, as_text)
        if not defi:
            stats["empty"] += 1
            continue
        words = [title] + [r for r in redirects.get(e.path, []) if r != title]
        stats["entries"] += 1
        stats["synonyms"] += len(words) - 1
        yield words, defi
        done += 1
        if limit and done >= limit:
            return


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("zim", type=Path, help="Kiwix ZIM file")
    ap.add_argument("outdir", type=Path, help="directory to create the dictionary in")
    ap.add_argument("--name", help="dictionary name (default: ZIM filename stem)")
    ap.add_argument("--lang", default="English", help="language section to keep (default: English)")
    ap.add_argument("--text", action="store_true", help="store plain text instead of HTML")
    ap.add_argument("--keep-namespaces", action="store_true",
                    help="keep titles containing ':' (Category:, Appendix:, ...)")
    ap.add_argument("--no-dictzip", action="store_true", help="leave .dict uncompressed")
    ap.add_argument("--limit", type=int, default=0, help="stop after N entries (for testing)")
    args = ap.parse_args(argv)

    name = args.name or args.zim.stem
    zim = Archive(args.zim)
    stats = dict(entries=0, synonyms=0, namespace=0, no_section=0, empty=0)

    Glossary.init()
    glos = Glossary()
    glos.setInfo("title", name)
    glos.setInfo("description", f"{args.lang} section of {args.zim.name}")
    glos.sourceLangName = args.lang
    fmt = "m" if args.text else "h"
    for words, defi in entries(zim, args.lang, args.text, args.keep_namespaces,
                               args.limit, stats):
        glos.addEntry(glos.newEntry(words, defi, defiFormat=fmt))

    if not stats["entries"]:
        sys.exit(f"no '{args.lang}' sections found in {args.zim}")

    outdir = args.outdir / name
    outdir.mkdir(parents=True, exist_ok=True)
    glos.write(
        str(outdir / f"{name}.ifo"),
        formatName="Stardict",
        dictzip=not args.no_dictzip,
        sametypesequence=fmt,
    )
    print(f"{outdir}: {stats['entries']} entries, {stats['synonyms']} synonyms; "
          f"skipped {stats['namespace']} namespaced, {stats['no_section']} without "
          f"{args.lang} section, {stats['empty']} empty", file=sys.stderr)


if __name__ == "__main__":
    main()
