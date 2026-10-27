# Spurious -- the Tab5 firmware

A StarDict dictionary reader for the M5Stack Tab5 (ESP32-P4).

## Using it

Put a dictionary -- its `.ifo`, `.idx`, and `.dict` or `.dict.dz` -- in
a folder called `dict` on the microSD card or a USB drive, or in a folder
inside that one. `tools/zim2stardict.py` writes them that way:

    tools/zim2stardict.py --whole-page --text wiktionary_en-simple_all_nopic_2026-10.zim out/
    cp -r out/wiktionary_en-simple_all_nopic_2026-10 /media/card/dict/

At power-on it opens the only dictionary it finds, or lists them. Type
on the keyboard and the list shows the headwords from there on, those
that start with what you typed highlighted; a synonym from the `.syn`
shows the headword it leads to after it. *Up* and *Down* page the list.
Tap a word for its definition, *Up* and *Down* to page it, *Back* to
return. Pulling the card closes the dictionary and goes back to looking.

*sounds* switches the list to the words that sound like what was typed
-- "fonetik" finds phonetic, "definately" definitely -- nearest spelling
first. It uses Double Metaphone keys, worked out for every word the
first time it is used (a progress bar; 55 ms for Simple English on a
PC, more on the Tab5), and kept while the dictionary is open.

The `.idx`, and the `.syn` or `.syn.dz`, are read into PSRAM whole:
about 1 MB for Simple English Wiktionary, plus 8 bytes a word once
*sounds* has been used. A `.idx.gz` is not read. If the files are not in
StarDict's order -- pyGlossary has written some that are not quite -- the
list is sorted when the dictionary opens.

## Building

ESP-IDF 5.4.2 or later:

    idf.py set-target esp32p4
    idf.py build
    idf.py merge-bin -o spurious_standalone.bin

The standalone image flashes at 0x0 and boots the reader as the factory
app. `build/spurious.bin` is the app alone, for
[Duplicitous](https://github.com/Sudrien/duplicitous-for-m5tab5) to
install into app2 -- the partition table is Duplicitous's.

The board bring-up is Duplicitous's: the panel, backlight, text, IO
expanders, touch and USB host from
[feckless-graphics-handler-for-m5tab5](https://github.com/Sudrien/feckless-graphics-handler-for-m5tab5)
and
[feckless-drivers-for-m5tab5](https://github.com/Sudrien/feckless-drivers-for-m5tab5),
and the card and the drive from
[feckless-storage-handler-for-m5tab5](https://github.com/Sudrien/feckless-storage-handler-for-m5tab5),
all pinned in `main/idf_component.yml`. exFAT is built in as there:
`cmake/exfat.cmake` runs `tools/enable_exfat.sh` at configure time; set
`TAB5_NO_EXFAT` in the environment to build FAT32 only.

`main/stardict.c` and `main/dmeta.c` are plain C. `test/stardict` builds
them on a host:

    make -C test/stardict
    test/stardict/lookup ../out/<name>/<name>.ifo cat '~fonetik'

With only the `.ifo` it reads every definition once and checks the rows'
order. `test/stardict/dmeta_check.py <.idx>` compares `dmeta.c` with the
Python Metaphone package over every headword (`pip install metaphone`).

## Licences other than MIT

- `components/usb_host_msc`: Espressif's esp-usb MSC driver, Apache-2.0,
  as Duplicitous vendors it (its own licence file and change notes inside).
- `components/tinfl`: the inflater from miniz 3.0.2, MIT (its own
  `LICENSE`). The ESP32-P4's ROM has none.
- The exFAT fatfs is IDF's component (Apache-2.0) around ChaN's FatFs.
- Ark12 in the graphics handler is SIL OFL-1.1.

MIT License; see [LICENSE](../LICENSE).
