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
- **0006 Whole-page mode and heading report for tools/zim2stardict.py.**
  A run on `wiktionary_en-simple_all_nopic_2026-10.zim` (54840 entries)
  kept 3 entries and skipped 54186 for "no English section": the
  per-language `<h2>` lookup does not fit that dump. Adds `--whole-page`
  (keep the page body, no language lookup) and, when pages without the
  section outnumber kept ones 10:1, a warning listing the `<h2>` ids seen
  on a sample, so the next run says what to pass. Pages whose bytes lack
  the language string are now skipped without a parse. Checked on
  hand-built ZIMs only; not yet run on the real Simple English dump, so
  what `--whole-page` keeps from its pages (title, navigation) is unseen.
- **0007 Clean page content in tools/zim2stardict.py.** `--whole-page` on
  the Simple English dump (0006 run, 54189 entries) kept the Vector skin
  chrome, navboxes and audio tables, and `--text` put every inline element
  on its own line. Now only `.mw-parser-output` is kept; audio tables,
  navboxes, images, edit links and empty bullets are dropped; text mode
  is block-aware (numbered `ol`, `-` for `ul`, one line per table row); HTML
  mode drops attributes and unwraps links and spans. Full run on
  `wiktionary_en-simple_all_nopic_2026-10.zim` with `--whole-page --text`:
  `54189 entries, 475 synonyms; skipped 4 namespaced, 0 empty` in 7m44s,
  `.dict.dz` 3.1 MB (was 4.2 MB). `cat` and `!` read as intended; not yet
  seen on a Tab5. The "without English section" count is no longer
  printed under `--whole-page`.
- **0008 Start m5tab5/: a reader on Duplicitous's base.** The build setup
  is duplicitous-for-m5tab5's at 291f83a -- exFAT vendoring, its patched
  usb_host_msc, partition table, `main/idf_component.yml` and
  `dependencies.lock` (copied unchanged, so the lock still matches),
  sdkconfig less the launcher's app rollback, and the CI workflow
  renamed. New: `main/stardict.c` (.ifo, .idx in PSRAM, .dict or
  dictzip'd .dict.dz through miniz 3.0.2's tinfl, vendored as
  `components/tinfl` since the P4's ROM has none; HTML reduced to text),
  and `main/main.c` (find dictionaries under `/dict` on card or drive,
  keyboard search over the .idx, paged definitions). Host test
  `test/stardict` on the Simple English dictionary in `out/`: every one
  of 54189 definitions read through dictzip, ASan/UBSan clean. Built on
  IDF v6.1 with `IDF_COMPONENT_MANAGER=0` and scratch components (see
  CLAUDE.md): no warnings, `spurious.bin binary size 0xe5db0 bytes ...
  (55%) free` of the 2 MB factory slot. Not built with the component
  manager, not on IDF 5.5, not run on a Tab5.
- **0009 Synonyms and a sounds-like search.** `main/stardict.c` reads the
  `.syn` or `.syn.dz` and merges it with the `.idx` into rows; a synonym
  row shows and opens its headword. `out/`'s `.idx` turned out not to be in
  StarDict's order (115 inversions, `AKA` before `Aaqib`), which breaks a
  binary search, so the rows are checked and sorted at open when they are
  not. `main/dmeta.c` is Double Metaphone written from Philips' reference;
  `sd_sounds_like()` keys every row on first use and ranks rows sharing a
  key by edit distance. The search screen's *sounds* key switches to it.
  Host: `54189 words, 475 synonyms, 54664 rows (files out of order: sorted
  at open)`, every definition read, rows in order, keys for all rows in
  55 ms; `dmeta_check.py` against Metaphone 0.6: `53806 agree, 383 differ
  in a known place, 0 otherwise`. Built on IDF v6.1 as 0008 was, no
  warnings, `0xe8900 bytes ... (55%) free`. Not run on a Tab5; 0008 has
  not been either.
- **0010 Inflate .syn.dz without miniz's on-stack decompressor.** First
  run on a Tab5, of 0009 (cbbb2c5, IDF v6.1-829): the 236 GB SDHC/SDXC
  card mounted (`tab5_storage: microSD mounted at /sd`), USB host up, then
  `Guru Meditation Error: Core 0 panic'ed (Stack protection fault)` in
  task main, backtrace `slurp at main/stardict.c:243` -- opening the
  `.syn.dz`. `tinfl_decompress_mem_to_heap()` keeps its ~11 KB
  `tinfl_decompressor` on the stack; the main task has 8 KB. `slurp()` now
  allocates the decompressor and, from the gzip trailer's ISIZE, the
  output, and inflates in one call. Deepest frames in the firmware build
  (`-fstack-usage`): sd_open 864, define 672, scan 624 bytes. Host tests
  as for 0009 pass; built on IDF v6.1; not yet rerun on the Tab5.
- **0011 Keep the vendored inflater's symbols away from the P4's ROM.**
  0010 on the Tab5: the card mounted, then `Guru Meditation Error: Core 0
  panic'ed (Store access fault)` in `tlsf_free`, from `free()` in
  `slurp()` opening the `.syn.dz`; a corrupted heap header read `'d r`, the
  first synonym's text. The P4 before rev 3 links
  `esp32p4.rom.eco0_4.ld`, which defines `tinfl_decompress` and its
  siblings at ROM addresses, and those beat `components/tinfl`'s at link
  time (`nm`: `4fc000f8 A tinfl_decompress`). So the ROM's older inflater
  ran against this miniz's `tinfl_decompressor`, a different struct, and
  wrote past it -- and 0009's crash, PC in ROM, was the same call, not only
  the stack. `miniz_export.h` now prefixes every exported name with
  `spurious_`; the app's two inflate calls `jal spurious_tinfl_decompress`
  and nothing calls the ROM addresses (objdump). Host tests pass; built on
  IDF v6.1; not yet rerun on the Tab5.
- **0012 Quiet the empty-slot probe.** 0011 on the Tab5 works: after the
  card went in, `tab5_storage: microSD mounted at /sd`, then `spurious:
  wiktionary_en-simple_all_nopic_2026-10: 54189 words, 475 synonyms (out
  of order: sorted), opened in 980 ms`. Before it, every second with no
  card logged `sdmmc_common: sdmmc_init_ocr: send_op_cond (1) returned
  0x107` and two `vfs_fat_sdmmc` errors (and idf.py monitor its HINT
  lines). Those two tags are now silent and SD_HOST warnings-only, set in
  app_main before storage starts (CONFIG_LOG_DYNAMIC_LEVEL_CONTROL is on
  by default). Built on IDF v6.1; not yet rerun on the Tab5.
- **0013 A build banner on the console.** app_main's first line is
  `spurious: === Spurious <version>, built <date> <time>, ESP-IDF <ver> ===`,
  from esp_app_get_description(): the version is CI's version.txt (the tag
  or ref), else git describe. Built on IDF v6.1; not yet run on the Tab5.
