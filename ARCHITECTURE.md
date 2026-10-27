# Architecture

## Series

### 0000s -- toward v0.1.0

- **0001 Clean up CLAUDE.md for this project.** Removes hardware notes and
  patch history inherited from another project; keeps the patch workflow
  and ESP-IDF build rules. Documentation only, not built or run.
- **0002 Add tools/zim2stardict.py.** Converts a Kiwix Wiktionary ZIM to a
  StarDict dictionary with pyGlossary: keeps one language section per page
  (`--lang`, default English), turns ZIM redirects into `.syn` synonyms,
  skips `Category:`-style titles, stores HTML or plain text (`--text`).
  Run only on a small hand-built ZIM (2 entries, 1 synonym, dictzip
  output read back); not run on a real Kiwix dump, so the section
  layout handling is unproven against one.
- **0003 Note the apt packages for tools/zim2stardict.py.** Docstring now
  lists `python3-bs4 python3-libzim python3-pyglossary` as the Debian
  route, plus the lxml and dictzip needs. Documentation only; the apt
  package names were not checked against a Debian mirror from the session.
- **0004 Progress output for tools/zim2stardict.py.** Prints an opening
  line, then count/percent/rate/ETA lines for the redirect scan and the
  conversion every 5 s (`--progress-interval`), a notice before the silent
  pyGlossary write, and timings. `--quiet` leaves only the final summary.
  Checked on the small test ZIM only; the rate and ETA are untested at
  real-dump scale.
- **0005 Fix the apt route in tools/zim2stardict.py's docstring.** 0003
  implied a Debian package for python-idzip; there is none. The apt line
  now adds `python3-lxml dictzip`, and python-idzip is marked pip-only.
  Checked on Ubuntu 24.04: `dictzip` is in the archive, and with
  python-idzip uninstalled the script wrote a `.dict.dz` that
  `dictzip -d -c` read back.
