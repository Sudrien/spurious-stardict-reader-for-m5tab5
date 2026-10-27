#!/usr/bin/env python3
"""Convert a Kiwix Wiktionary ZIM dump to a StarDict dictionary.

    pip install -r tools/requirements.txt
    # or, on Debian/Ubuntu:
    sudo apt install python3-bs4 python3-libzim python3-pyglossary python3-lxml dictzip
    tools/zim2stardict.py wiktionary_en_all_nopic_2026-09.zim out/ --lang English

The script parses with lxml. Compressing .dict.dz needs either python-idzip
(pip only, there is no Debian package) or the dictzip utility on PATH (the
apt route above); pass --no-dictzip to skip compression.

Writes out/<name>/<name>.{ifo,idx,dict[.dz],syn}.

Each ZIM article is a whole Wiktionary page, with one section per language.
Only the section named by --lang is kept as the definition; pages without
one are skipped. ZIM redirects become StarDict synonyms of their target,
so lookups by inflected or alternate spelling still land on the entry.

Definitions are stored as HTML ('h') by default, or as plain text ('m')
with --text, which is what a reader without an HTML renderer wants.
"""

import argparse
import collections
import re
import sys
import time
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


class Progress:
    """Print a status line to stderr at most every `interval` seconds."""

    def __init__(self, label, total, interval, quiet):
        self.label, self.total, self.interval, self.quiet = label, total, interval, quiet
        self.start = self.last = time.monotonic()

    def update(self, n, extra=""):
        now = time.monotonic()
        if self.quiet or now - self.last < self.interval:
            return
        self.last = now
        elapsed = now - self.start
        rate = n / elapsed if elapsed else 0
        eta = f", ~{(self.total - n) / rate:.0f}s left" if rate and self.total else ""
        print(f"{self.label}: {n}/{self.total} ({100 * n / self.total:.1f}%), "
              f"{rate:.0f}/s{eta}{extra}", file=sys.stderr, flush=True)

    def done(self, extra=""):
        if not self.quiet:
            print(f"{self.label}: done in {time.monotonic() - self.start:.1f}s{extra}",
                  file=sys.stderr, flush=True)


def note_headings(content, counter):
    """Count the h2 ids of a page, to tell the user what the dump does use."""
    for h2 in BeautifulSoup(content, "lxml").find_all("h2"):
        counter[h2.get("id") or h2.get_text(strip=True)] += 1


def entries(zim, lang, as_text, keep_ns, limit, stats, interval, quiet, whole_page):
    total = zim.entry_count
    lang_bytes = lang.encode()
    sample_budget = [300]  # pages to inspect for the "what headings exist" hint

    # Redirects first, so each real entry can carry them as synonyms.
    redirects = {}
    prog = Progress("scanning redirects", total, interval, quiet)
    for i in range(total):
        e = zim._get_entry_by_id(i)
        if e.is_redirect:
            target = e.get_redirect_entry().path
            redirects.setdefault(target, []).append(e.title)
        prog.update(i + 1, f", {len(redirects)} targets")
    prog.done(f", {len(redirects)} redirect targets")

    done = 0
    prog = Progress("converting", total, interval, quiet)
    for i in range(total):
        prog.update(i, f", kept {stats['entries']}")
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
        content = bytes(item.content)
        if whole_page:
            soup = BeautifulSoup(content, "lxml")
            section = str((soup.body or soup).decode_contents())
        else:
            # Cheap pre-filter: skip the parse when the heading id cannot be there.
            if lang_bytes not in content:
                stats["no_section"] += 1
                if sample_budget[0] > 0:
                    sample_budget[0] -= 1
                    note_headings(content, stats["headings"])
                continue
            soup = BeautifulSoup(content, "lxml")
            section = language_section(soup, lang)
            if section is None:
                stats["no_section"] += 1
                if sample_budget[0] > 0:
                    sample_budget[0] -= 1
                    note_headings(content, stats["headings"])
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
            break
    prog.done(f", kept {stats['entries']}")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("zim", type=Path, help="Kiwix ZIM file")
    ap.add_argument("outdir", type=Path, help="directory to create the dictionary in")
    ap.add_argument("--name", help="dictionary name (default: ZIM filename stem)")
    ap.add_argument("--lang", default="English", help="language section to keep (default: English)")
    ap.add_argument("--whole-page", action="store_true",
                    help="keep the whole page instead of one language section "
                         "(for dumps with no per-language h2, e.g. Simple English)")
    ap.add_argument("--text", action="store_true", help="store plain text instead of HTML")
    ap.add_argument("--keep-namespaces", action="store_true",
                    help="keep titles containing ':' (Category:, Appendix:, ...)")
    ap.add_argument("--no-dictzip", action="store_true", help="leave .dict uncompressed")
    ap.add_argument("--quiet", action="store_true", help="no progress output")
    ap.add_argument("--progress-interval", type=float, default=5.0, metavar="SECS",
                    help="seconds between progress lines (default: 5)")
    ap.add_argument("--limit", type=int, default=0, help="stop after N entries (for testing)")
    args = ap.parse_args(argv)

    name = args.name or args.zim.stem
    log = (lambda *a: None) if args.quiet else (lambda *a: print(*a, file=sys.stderr, flush=True))
    zim = Archive(args.zim)
    log(f"{args.zim}: {zim.entry_count} entries, keeping "
        f"{'the whole page' if args.whole_page else 'the ' + args.lang + ' section'} "
        f"as {'text' if args.text else 'HTML'}")
    stats = dict(entries=0, synonyms=0, namespace=0, no_section=0, empty=0,
                 headings=collections.Counter())

    Glossary.init()
    glos = Glossary()
    glos.setInfo("title", name)
    glos.setInfo("description", f"{args.zim.name}" if args.whole_page
                 else f"{args.lang} section of {args.zim.name}")
    glos.sourceLangName = args.lang
    fmt = "m" if args.text else "h"
    for words, defi in entries(zim, args.lang, args.text, args.keep_namespaces,
                               args.limit, stats, args.progress_interval, args.quiet,
                               args.whole_page):
        glos.addEntry(glos.newEntry(words, defi, defiFormat=fmt))

    if stats["no_section"] > 10 * max(stats["entries"], 1):
        common = ", ".join(f"{h!r} ({n})" for h, n in stats["headings"].most_common(12))
        print(f"warning: {stats['no_section']} pages had no '{args.lang}' section but "
              f"{stats['entries']} did.\n  h2 headings on a sample of those pages: "
              f"{common or 'none'}\n  If this dump has no per-language headings, "
              f"use --whole-page; otherwise pick --lang from the list.",
              file=sys.stderr)
    if not stats["entries"]:
        sys.exit(f"no '{args.lang}' sections found in {args.zim}")

    outdir = args.outdir / name
    outdir.mkdir(parents=True, exist_ok=True)
    log(f"writing StarDict to {outdir} (sorting and "
        f"{'leaving .dict uncompressed' if args.no_dictzip else 'compressing'}; "
        f"this step prints nothing and can take a while)")
    t0 = time.monotonic()
    glos.write(
        str(outdir / f"{name}.ifo"),
        formatName="Stardict",
        dictzip=not args.no_dictzip,
        sametypesequence=fmt,
    )
    log(f"writing: done in {time.monotonic() - t0:.1f}s")
    print(f"{outdir}: {stats['entries']} entries, {stats['synonyms']} synonyms; "
          f"skipped {stats['namespace']} namespaced, {stats['no_section']} without "
          f"{args.lang} section, {stats['empty']} empty", file=sys.stderr)


if __name__ == "__main__":
    main()
