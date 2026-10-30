#!/usr/bin/env python3
"""Convert a Kiwix Wiktionary ZIM dump to a StarDict dictionary.

    pip install -r tools/requirements.txt
    # or, on Debian/Ubuntu:
    sudo apt install python3-bs4 python3-libzim python3-pyglossary python3-lxml dictzip
    tools/zim2stardict.py wiktionary_en_all_nopic_2026-09.zim out/ --lang English

The script parses with lxml. Compressing .dict.dz needs either python-idzip
(pip only, there is no Debian package) or the dictzip utility on PATH (the
apt route above); pass --no-dictzip to skip compression.

A .dict over 4 GiB (e.g. --whole-page on a full Wiktionary) needs --large-file.

Pages are converted by --jobs worker processes (default: one per CPU), in
chunks committed to out/<name>/<name>.work.sqlite as they finish. An
interrupted run, started again with the same arguments, picks up from the
last committed chunk; --restart throws the work file away and starts over.
The work file is removed once the dictionary is written.

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
import json
import multiprocessing
import os
import re
import sqlite3
import sys
import time
from pathlib import Path

from bs4 import BeautifulSoup, Comment, NavigableString, Tag
from libzim.reader import Archive
from pyglossary.glossary_v2 import Glossary

# Things that carry no definition: media, edit links, navigation boxes.
JUNK = ("script", "style", "link", "meta", "img", "figure", "audio", "video",
        "sup.reference", ".mw-editsection", ".mw-empty-elt", "div.thumb",
        "table.audiotable", "table.navbox", "table.toccolours", "div.NavFrame")
# JUNK as (tag or None, class or None), matched by hand in one pass over the
# tree: soupsieve walking the tree once per selector was most of the run time.
JUNK_TAGS = {s for s in JUNK if "." not in s}
JUNK_CLASSES = [tuple(x or None for x in s.split(".", 1)) for s in JUNK if "." in s]


def is_junk(el):
    if el.name in JUNK_TAGS:
        return True
    classes = el.get("class")
    if not classes:
        return False
    return any(c in classes and (t is None or t == el.name) for t, c in JUNK_CLASSES)

# Tags kept in HTML output; everything else is unwrapped (its text stays).
KEEP = {"p", "br", "b", "i", "u", "sup", "sub", "code", "ul", "ol", "li", "dl",
        "dt", "dd", "h2", "h3", "h4", "h5", "table", "tr", "td", "th", "blockquote"}
BLOCK = {"p", "div", "section", "ul", "ol", "li", "dl", "dt", "dd", "h1", "h2",
         "h3", "h4", "h5", "h6", "table", "tr", "blockquote", "figcaption"}


def _text(node, out):
    """Append the plain text of `node` to list `out`, with newlines at blocks."""
    if isinstance(node, NavigableString):
        if not isinstance(node, Comment):
            out.append(re.sub(r"\s+", " ", str(node)))
        return
    name = node.name
    if name == "br":
        out.append("\n")
        return
    heading = name in ("h2", "h3", "h4", "h5", "h6")
    if name in ("td", "th"):
        # One line per cell, so a table row stays on one line.
        cell = []
        for child in node.children:
            _text(child, cell)
        cell = re.sub(r"\s+", " ", "".join(cell)).strip()
        if cell:
            out.append(f" {cell} | ")
        return
    if name == "li" and not node.get_text(strip=True):
        return
    if name == "li":
        parent = node.parent
        if parent is not None and parent.name == "ol":
            n = 1 + len([x for x in node.find_previous_siblings("li")])
            out.append(f"\n{n}. ")
        else:
            out.append("\n- ")
    elif name == "dd":
        out.append("\n  ")
    elif heading:
        out.append("\n\x00")
    elif name in BLOCK:
        out.append("\n")
    for child in node.children:
        _text(child, out)
    if heading or name in BLOCK:
        out.append("\n")


def section_root(soup, lang):
    """Return the `lang` section of a Wiktionary page as a new <div>, or None.

    Handles both layouts Kiwix has shipped: <section> wrappers around an
    <h2 id=lang>, and a flat run of siblings after the <h2>. The nodes are
    moved, not copied, so clean() need not parse them a second time.
    """
    h2 = soup.find("h2", id=lang)
    if h2 is None:
        return None
    wrapper = h2.find_parent("section")
    if wrapper is not None and wrapper.find("h2") is h2:
        parts = [c for c in wrapper.children if c is not h2]
    else:
        parts = []
        for sib in h2.next_siblings:
            if getattr(sib, "name", None) == "h2":
                break
            parts.append(sib)
    if not any(isinstance(c, Tag) or c.strip() for c in parts):
        return None
    root = soup.new_tag("div")
    for c in parts:
        root.append(c.extract())
    return root


def clean(html, as_text):
    """Clean a section, given as HTML or as a Tag from section_root()."""
    if isinstance(html, Tag):
        root = html.find(class_="mw-parser-output") or html
    else:
        soup = BeautifulSoup(html, "lxml")
        root = soup.find(class_="mw-parser-output") or soup.body or soup
    for el in [el for el in root.descendants if isinstance(el, Tag) and is_junk(el)]:
        if not el.decomposed:  # may sit inside one already removed
            el.decompose()
    if as_text:
        out = []
        for child in root.children:
            _text(child, out)
        lines = [re.sub(r"[ \t]+", " ", l).strip(" |") if "|" in l
                 else re.sub(r"[ \t]+", " ", l).strip()
                 for l in "".join(out).split("\n")]
        # Drop empty lines; a heading (marked \x00) gets one blank line before it.
        text = []
        for l in lines:
            if l.startswith("\x00"):
                text.extend(["", l[1:].strip()])
            elif l:
                text.append(l)
        return "\n".join(text).strip()
    for el in root.find_all(True):
        if el.name not in KEEP:
            el.unwrap()
        else:
            el.attrs = {}
    html = "".join(str(c) for c in root.children)
    return re.sub(r"\n{2,}", "\n", html).strip()


class Progress:
    """Print a status line to stderr at most every `interval` seconds."""

    def __init__(self, label, total, interval, quiet, base=0):
        self.label, self.total, self.interval, self.quiet = label, total, interval, quiet
        self.base = base  # already done before this run: not counted in the rate
        self.start = self.last = time.monotonic()

    def update(self, n, extra=""):
        now = time.monotonic()
        if self.quiet or now - self.last < self.interval:
            return
        self.last = now
        elapsed = now - self.start
        rate = (n - self.base) / elapsed if elapsed else 0
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


def entry_by_id(zim, i):
    # python-libzim 3.x renamed _get_entry_by_id to get_entry_by_id.
    get = getattr(zim, "get_entry_by_id", None) or zim._get_entry_by_id
    return get(i)


SAMPLES_PER_CHUNK = 2  # pages per chunk to inspect for the headings hint
_worker = {}


def _init_worker(zim_path, opts):
    _worker["zim"] = Archive(zim_path)
    _worker["opts"] = opts


def convert_chunk(span):
    """Convert ZIM entries [start, end). Runs in a worker process.

    Returns (start, end, entries, redirects, stats): entries as
    (id, path, title, definition), redirects as (id, target path, title).
    """
    start, end = span
    zim, o = _worker["zim"], _worker["opts"]
    lang, lang_bytes = o["lang"], o["lang"].encode()
    out, redirects = [], []
    stats = dict(namespace=0, no_section=0, empty=0, headings=collections.Counter())
    samples = SAMPLES_PER_CHUNK

    def no_section(content):
        nonlocal samples
        stats["no_section"] += 1
        if samples > 0:
            samples -= 1
            note_headings(content, stats["headings"])

    for i in range(start, end):
        e = entry_by_id(zim, i)
        if e.is_redirect:
            redirects.append((i, e.get_redirect_entry().path, e.title))
            continue
        item = e.get_item()
        if not item.mimetype.startswith("text/html"):
            continue
        title = e.title
        if not o["keep_ns"] and ":" in title:
            stats["namespace"] += 1
            continue
        content = bytes(item.content)
        if o["whole_page"]:
            section = content.decode("utf-8", "replace")
        else:
            # Cheap pre-filter: skip the parse when the heading id cannot be there.
            if lang_bytes not in content:
                no_section(content)
                continue
            section = section_root(BeautifulSoup(content, "lxml"), lang)
            if section is None:
                no_section(content)
                continue
        defi = clean(section, o["as_text"])
        if not defi:
            stats["empty"] += 1
            continue
        out.append((i, e.path, title, defi))
    return start, end, out, redirects, stats


def open_work(path, fingerprint, restart, log):
    """Open (or create) the work file; refuse one made with other arguments."""
    if restart and path.exists():
        path.unlink()
    db = sqlite3.connect(path)
    db.executescript("""
        PRAGMA journal_mode=WAL;
        PRAGMA synchronous=NORMAL;
        CREATE TABLE IF NOT EXISTS meta (k TEXT PRIMARY KEY, v TEXT);
        CREATE TABLE IF NOT EXISTS chunks (start INTEGER PRIMARY KEY, end INTEGER, stats TEXT);
        CREATE TABLE IF NOT EXISTS entries (id INTEGER PRIMARY KEY, path TEXT, title TEXT, defi TEXT);
        CREATE TABLE IF NOT EXISTS redirects (id INTEGER PRIMARY KEY, target TEXT, title TEXT);
    """)
    row = db.execute("SELECT v FROM meta WHERE k='fingerprint'").fetchone()
    if row is None:
        db.execute("INSERT INTO meta VALUES ('fingerprint', ?)", (fingerprint,))
        db.commit()
    elif row[0] != fingerprint:
        sys.exit(f"{path} was made from a different ZIM or with different options;\n"
                 f"  rerun with those, or pass --restart to start over")
    else:
        n = db.execute("SELECT count(*) FROM chunks").fetchone()[0]
        if n:
            log(f"resuming: {n} chunks already converted in {path}")
    return db


def convert(zim_path, total, opts, db, jobs, chunk, limit, stats, interval, quiet):
    """Convert every chunk not yet in the work file, committing each one."""
    done = {r[0] for r in db.execute("SELECT start FROM chunks")}
    kept = db.execute("SELECT count(*) FROM entries").fetchone()[0]
    todo = [(s, min(s + chunk, total)) for s in range(0, total, chunk) if s not in done]
    finished = total - sum(e - s for s, e in todo)
    prog = Progress("converting", total, interval, quiet, base=finished)
    if todo and not (limit and kept >= limit):
        ctx = multiprocessing.get_context("spawn" if sys.platform == "win32" else None)
        with ctx.Pool(jobs, _init_worker, (str(zim_path), opts)) as pool:
            # With --limit, take chunks in order, so the entries kept are the first.
            run = pool.imap if limit else pool.imap_unordered
            for start, end, out, redirects, st in run(convert_chunk, todo):
                with db:  # one transaction: a chunk is all there or not at all
                    db.executemany("INSERT OR REPLACE INTO entries VALUES (?,?,?,?)", out)
                    db.executemany("INSERT OR REPLACE INTO redirects VALUES (?,?,?)", redirects)
                    db.execute("INSERT INTO chunks VALUES (?,?,?)",
                               (start, end, json.dumps(st)))
                kept += len(out)
                finished += end - start
                prog.update(finished, f", kept {kept}")
                if limit and kept >= limit:
                    pool.terminate()
                    break
    if limit:
        # Stopped early: still pick up every redirect, as synonyms for what was kept.
        done = {r[0] for r in db.execute("SELECT start FROM chunks")}
        zim = Archive(zim_path)
        with db:
            for s, e in todo:
                if s in done:
                    continue
                for i in range(s, e):
                    x = entry_by_id(zim, i)
                    if x.is_redirect:
                        db.execute("INSERT OR REPLACE INTO redirects VALUES (?,?,?)",
                                   (i, x.get_redirect_entry().path, x.title))
    for (st,) in db.execute("SELECT stats FROM chunks"):
        st = json.loads(st)
        for k in ("namespace", "no_section", "empty"):
            stats[k] += st[k]
        stats["headings"].update(st["headings"])
    prog.done(f", kept {kept}")


def stored_entries(db, limit, stats):
    """Yield (words, definition) from the work file, in ZIM order."""
    db.execute("CREATE INDEX IF NOT EXISTS redirects_target ON redirects (target)")
    syn = db.cursor()
    q = "SELECT path, title, defi FROM entries ORDER BY id"
    if limit:
        q += f" LIMIT {int(limit)}"
    for path, title, defi in db.execute(q):
        words = [title] + [r for (r,) in syn.execute(
            "SELECT title FROM redirects WHERE target=? ORDER BY id", (path,))
            if r != title]
        stats["entries"] += 1
        stats["synonyms"] += len(words) - 1
        yield words, defi


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
    ap.add_argument("--large-file", action="store_true",
                    help="allow a .dict over 4 GiB (64-bit .idx offsets; implies "
                         "--no-dictzip, and the reader must support idxoffsetbits=64)")
    ap.add_argument("--quiet", action="store_true", help="no progress output")
    ap.add_argument("--progress-interval", type=float, default=5.0, metavar="SECS",
                    help="seconds between progress lines (default: 5)")
    ap.add_argument("--limit", type=int, default=0, help="stop after N entries (for testing)")
    ap.add_argument("--jobs", "-j", type=int, default=os.cpu_count() or 1,
                    help="worker processes (default: one per CPU)")
    ap.add_argument("--chunk", type=int, default=500, metavar="N",
                    help="ZIM entries per committed chunk (default: 500)")
    ap.add_argument("--restart", action="store_true",
                    help="discard an interrupted run's work file and start over")
    args = ap.parse_args(argv)
    if args.large_file:
        args.no_dictzip = True  # dictzip cannot index data this large

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

    outdir = args.outdir / name
    outdir.mkdir(parents=True, exist_ok=True)
    work_path = outdir / f"{name}.work.sqlite"
    opts = dict(lang=args.lang, as_text=args.text, keep_ns=args.keep_namespaces,
                whole_page=args.whole_page)
    st = args.zim.stat()
    fingerprint = json.dumps(dict(opts, zim=str(args.zim.resolve()), size=st.st_size,
                                  mtime=st.st_mtime_ns, chunk=args.chunk), sort_keys=True)
    db = open_work(work_path, fingerprint, args.restart, log)
    convert(args.zim, zim.entry_count, opts, db, max(1, args.jobs), max(1, args.chunk),
            args.limit, stats, args.progress_interval, args.quiet)
    for words, defi in stored_entries(db, args.limit, stats):
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

    log(f"writing StarDict to {outdir} (sorting and "
        f"{'leaving .dict uncompressed' if args.no_dictzip else 'compressing'}; "
        f"this step prints nothing and can take a while)")
    t0 = time.monotonic()
    glos.write(
        str(outdir / f"{name}.ifo"),
        formatName="Stardict",
        dictzip=not args.no_dictzip,
        sametypesequence=fmt,
        large_file=args.large_file,
    )
    log(f"writing: done in {time.monotonic() - t0:.1f}s")
    db.close()
    for suffix in ("", "-wal", "-shm"):
        Path(f"{work_path}{suffix}").unlink(missing_ok=True)
    print(f"{outdir}: {stats['entries']} entries, {stats['synonyms']} synonyms; "
          f"skipped {stats['namespace']} namespaced, "
          + ("" if args.whole_page else f"{stats['no_section']} without "
                                        f"{args.lang} section, ")
          + f"{stats['empty']} empty", file=sys.stderr)


if __name__ == "__main__":
    main()
