# CLAUDE.md

How to work on this repository. Read it before writing a patch.

## Project

A StarDict dictionary reader for the M5Stack Tab5. See `README.md` for the
plan. Built on the feckless libraries, pinned in `m5tab5/main/idf_component.yml`.

## Patches

Claude creates `git am`-able patches authored as
`Claude <noreply@anthropic.com>`, and presents each one as soon as it
exists -- a session can end with patches stuck behind it.

Claude must not commit to, push to, branch on, or open a pull request
against the repository. The URL a session is given is for pulling only.
Local commits made to run `git format-patch` stay local.

**Patches only, never binaries.** Claude does not hand over a `.bin`,
built or merged, for testing or otherwise. Firmware that goes on a Tab5
is built by CI from a pushed tree (once a workflow exists), so what was
tested is what is in the history -- a binary from a session's scratch
build is neither.

**Patches are cumulative.** Each applies on top of what is already here,
including patches handed over and not yet pushed. Never hand back a
rewritten copy of a file, and never reissue a corrected version of a
patch that has been handed over: send a follow-up, with the next number,
that changes what needs changing. A patch already handed over is
history.

**Handed over, not pushed, is the line.** A patch may be applied and
pushed before the session's next message, and a session cannot see
whether it was. A second file under the same number is two patches with
one name, and there is no telling which one the next patch was built on.

**A patch has one number, and it is written down once.** The number
runs on from patch to patch across sessions. It is supplied by
`--start-number`, never put in the subject:

    git format-patch --start-number 2 -1 -o out/

gives `0002-<subject>.patch`. A subject reading `0002: ...` would come
out as `0002-0002-...`; write the subject as the subject.

**Before choosing a number, look at the last one used:** the newest
entry in the series at the end of `ARCHITECTURE.md`, which every patch
adds itself to. `git log` has the subjects but not the numbers. Take
the next one; never start from a round number without checking it is
free.

**Where the numbering is now: the 0000 series, toward v0.1.0.** A
series is named for the release it leads to -- the 0000s toward v0.1.0,
the 2000s toward v0.2.0, the 3000s toward v0.3.0 -- so a patch number
says which release it went into. A point release (v0.1.1) does not
start a series. **The first patch after a tag updates this paragraph**
and opens the next series' heading in `ARCHITECTURE.md`; the tag is the
reminder, and this file is what the next session reads.

**What a patch's `ARCHITECTURE.md` entry says:** its number and
subject, one or two lines of what it does, and -- once a board has run
it -- what the log showed, with the line that proves it. A patch that
changes `sdkconfig.defaults` names the log line that proves the new
value took. Hardware results for a patch already handed over go in the
next patch's entry, not into the old one.

## m5tab5/

ESP-IDF (>= 5.4.2) firmware, on the feckless components as
duplicitous-for-m5tab5 has them; its build setup (exFAT, usb_host_msc,
partitions, CI) is Duplicitous's, copied. `main/stardict.c` is plain C:
`test/stardict` builds it on a host, and a change to it runs there first.

- **Every dependency is pinned to a tag.**
- **The Espressif registry is not reachable from a session.** A session
  can still build by fetching the registry components' sources from
  GitHub into a scratch component directory and building with
  `IDF_COMPONENT_MANAGER=0`; a patch says whether it was built that way,
  or not at all. Neither way updates `dependencies.lock`, so a patch that
  touches an `idf_component.yml` says so. What that took for 0008 on
  IDF v6.1: `IDF_PYTHON_CHECK_CONSTRAINTS=no` for `install.sh` and
  `export.sh` (dl.espressif.com is blocked); the components from esp-bsp,
  esp-iot-solution and esp-usb (main), and the three feckless repos at
  their tags under their component names; and, in the scratch copies
  only, `usb` added to `usb_host_uac`'s and `usb_host_msc`'s REQUIRES and
  `esp_lcd_touch` to `esp_lcd_touch_st7123`'s -- the component manager
  adds those from the manifests, and nothing else does.
- **Any change to `sdkconfig.defaults` needs `rm sdkconfig`** before the
  next build. Say so in the commit message.

### Learned on hardware

Nothing yet. Add findings here only once a Tab5 has shown them with this
code, each with the patch number that showed it.
