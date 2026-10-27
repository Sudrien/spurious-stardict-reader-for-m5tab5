# CLAUDE.md

How to work on this repository. Read it before writing a patch.

## Patches

Claude creates `git am`-able patches authored as
`Claude <noreply@anthropic.com>`, and presents each one as soon as it
exists -- a session can end with patches stuck behind it.

Claude must not commit to, push to, branch on, or open a pull request
against the repository. The URL a session is given is for pulling only.
Local commits made to run `git format-patch` stay local.

**Patches only, never binaries.** Claude does not hand over a `.bin`,
built or merged, for testing or otherwise. The firmware that goes on a
Tab5 is what `.github/workflows/m5tab5.yml` builds from a pushed tree,
so what was tested is what is in the history -- a binary from a
session's scratch build is neither.

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

    git format-patch --start-number 9 -1 -o out/

gives `0009-<subject>.patch`. A subject reading `0009: ...` would come
out as `0009-0009-...`; write the subject as the subject.

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

ESP-IDF (>= 5.4.2), on `feckless_graphics_handler` and `feckless_drivers`
from defeatist-music-player-for-m5tab5's family of components.

- **Every dependency is pinned to a tag.**
- **The Espressif registry is not reachable from a session.** A session
  can still build by fetching the registry components' sources from
  GitHub (esp-bsp, esp-iot-solution, esp-usb) into a scratch component
  directory and building with `IDF_COMPONENT_MANAGER=0`; a patch says
  whether it was built that way, or not at all. Neither way updates
  `dependencies.lock`, so a patch that touches an `idf_component.yml`
  says so.
- **Any change to `sdkconfig.defaults` needs `rm sdkconfig`** before the
  next build. Say so in the commit message.
- **Run on hardware:** see the series in `ARCHITECTURE.md` for what each
  build did on a Tab5.

### Learned on hardware

- **ESP-IDF's USB-Serial/JTAG driver drops what does not fit its ring
  buffer**, after USB has acknowledged it (`xRingbufferSendFromISR()`
  unchecked, 5.5 and 6.1). Anything that streams into the Tab5 must not
  go through it: `rb` masks the receive interrupt and reads the FIFO
  itself, so unread bytes make the hardware NAK and USB holds the
  sender (0011). Logs are off during a transfer: a log line is protocol
  bytes to the sender.
- **Cyan is two different things.** A DSI underrun (the scanout missing
  its PSRAM fetch) -- esp_lcd logs `underrun happens` for each; check
  the log before assuming one. And a panel with no DSI signal, between
  a reset and the next app's panel init -- no log at all. 0013's
  markers showed the second; 0014 blanks the panel and backlight on
  every `esp_restart()`. Long flash erases are a candidate for the
  first; `install` erases a sector ahead of each write (0012).
- **Choosing factory erases otadata.** An app's "exit to Launcher"
  (`esp_ota_set_boot_partition(factory)`) leaves app2 with no record; a
  reset out of app2 comes back by rollback with app2 ABORTED. That is how
  the countdown tells the two apart (0015).
- **Two programs can hold the port on Linux** and split the Tab5's
  output between them, a few bytes each. `idf.py monitor` left open does
  it. The page catches it at `version` and with `echo` (0018, 0019).
- **The card was exFAT** -- four `sdmmc_read_blocks failed (0x104)` and a
  failed mount under FAT32-only FatFs. exFAT is vendored at configure
  time (`cmake/exfat.cmake`), and storage is feckless_storage_handler
  (0007).
- **`PHASE()` markers in main.c are temporary** (0013): out once a Tab5
  shows the restart dark after 0014.
