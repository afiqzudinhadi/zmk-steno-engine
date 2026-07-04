# zmk-steno-engine

Clean-room stenography engine for [ZMK Firmware](https://zmk.dev).
Carries the **full Plover main dictionary AND the full Lapwing base
dictionary — 262,309 entries, zero trimming — on a pair of nRF52840
keyboard halves** using the v4 "union split-section" format

**Status:** working end to end on real hardware (corne, 2x nice!nano v2).
Translations, multi-stroke longest-match with retrace, number strokes,
runtime dictionary switching (Plover/Lapwing). Known issues tracked in
the issue tracker; latency optimization in progress.

## How it works

Neither dictionary fits on a single MCU (~700 KB raw translations alone,
~800 KB flash free per half after ZMK + peripherals). The engine solves
this by:

1. **Merging both dicts into one structure** — a union CHD minimal
   perfect hash function (MPHF) indexes all 227K stroke keys once.
   Building two separate indexes would cost 1.8x more flash because
   33K identical entries and the string table get duplicated.
2. **Aggressive compression** — translations are sorted, front-coded
   (common-prefix elimination), then DEFLATE-compressed in 16 KB blocks.
   Both dicts share the same string table (Lapwing is 93% a subset of
   Plover), storing all 73K unique translations in 193 KB (28% of raw).
   MPHF displacements are Huffman-coded (8 bits avg vs 19 fixed).
3. **Splitting the structure across halves** — sections of the single
   index are placed on whichever half has budget, not duplicated:

| Half | Holds | Role |
|------|-------|------|
| Left (central) | MPHF displacements, membership bitmap, fingerprints, conflict table, value-index slice | Every stroke *decision* is resolved locally — zero BLE latency for hit/miss/which-dict |
| Right (peripheral) | Compressed string table, value-index remainder, conflict dup | Serves translation *text* on demand over a custom BLE GATT service |

4. **Custom BLE protocol** — left resolves the stroke to a string ID
   locally, then requests just the text bytes from the right half
   (one round trip, LRU-cached). The right half inflates and walks the
   front-coded block on a dedicated worker thread (16 KB buffer).
5. **Own DEFLATE decoder** — Zephyr ships no zlib; a minimal RFC 1951
   decompressor (~300 lines, no allocation) runs on the peripheral.

## Requirements

- **A split keyboard with two nRF52840 controllers** (e.g. corne with
  2x nice!nano v2). The dictionary spans both halves — **single-MCU
  keyboards are not supported yet.**
- ZMK v0.3.x build environment (GitHub Actions user-config works)
- **Adafruit nRF52 bootloader 0.9.0 or newer on BOTH halves —
  0.11.0 recommended.** See [Bootloader](#bootloader) below; old
  bootloaders silently corrupt large firmware images.

## Usage

`west.yml`:

```yaml
manifest:
  remotes:
    - name: afiqzudinhadi
      url-base: https://github.com/afiqzudinhadi
  projects:
    - name: zmk-steno-engine
      remote: afiqzudinhadi
      revision: main
```

`.conf`:

```
CONFIG_STENO_ENGINE=y
CONFIG_STENO_DICT_BOTH=y            # or STENO_DICT_PLOVER / STENO_DICT_LAPWING
CONFIG_STENO_SPLIT_DICT=y
CONFIG_STENO_DICT_LEFT_MAX_SIZE=440320
CONFIG_STENO_DICT_RIGHT_MAX_SIZE=519168
CONFIG_STENO_SPLIT_TIMEOUT_MS=250
```

The build FAILS if either half's blob exceeds its budget — entries are
never trimmed.

**Budget recommendations** (nRF52840, Adafruit bootloader 0.11.0,
app ceiling 0xEA000 = 802,816 bytes):

| Config | Left | Right | Notes |
|--------|------|-------|-------|
| Both dicts (Plover + Lapwing) | 440320 (430 KB) | 519168 (507 KB) | Tight — little room for extra modules |
| Plover only | 440320 | 519168 | Comfortable — ~35 KB slack per half |
| Lapwing only | 440320 | 519168 | Comfortable |

Left budget is smaller because the central half carries more firmware
code (behavior driver, formatter, output, undo, BLE client, cache) —
leaving less free flash for dictionary data. The peripheral only runs
the GATT server + inflate decoder, so it gets the larger share.

Lower these if you add other flash-heavy modules (large RGB animations,
display assets, etc.) and the linker overflows. The compiler shifts the
value-index split point to fill the right half first; left gets the
remainder. If either budget is too small for the compiled dict, the
build errors out with the exact overshoot.

Keymap: include the behavior and bind the 23 steno keys
(see `include/dt-bindings/zmk/steno_keys.h` for all key names):

```dts
#include <behaviors/steno_engine.dtsi>
#include <dt-bindings/zmk/steno_keys.h>

// example row: &steno STENO_NUM  &steno STENO_SL  &steno STENO_TL ...
```

Digits in dictionary strokes (e.g. `12K` → `12:00`) map to the number
bar plus their positional key, exactly like Plover.

## Flash budget

The app image on each half must stay below the bootloader's app-region
ceiling (`0xEA000`, i.e. **802,816 bytes** with Adafruit 0.11.0).
Firmware blocks above the ceiling are silently dropped and the half
will not boot. The default budgets above keep both halves under it —
check the `FLASH` line in your build log if you change them.

## Bootloader

The nice!nano ships with various versions of the
[Adafruit nRF52 bootloader](https://github.com/adafruit/Adafruit_nRF52_Bootloader).
**Versions from ~2021 (e.g. 0.6.0) corrupt large UF2 flashes** — blocks
go missing mid-image and the firmware crash-loops with no display, no
typing, no USB. This module's images are large (~1.5 MB UF2 per half),
so a current bootloader is mandatory.

### Check your version

Double-tap the reset button. The half mounts as a `NICENANO` USB drive;
open `INFO_UF2.TXT` on it:

```
UF2 Bootloader 0.6.0 ...   ← too old
UF2 Bootloader 0.11.0 ...  ← good
```

(On newer macOS the volume may not auto-mount — run `diskutil list`
and `diskutil mount diskN`.)

### Update

1. Download the updater for your board from the
   [official releases](https://github.com/adafruit/Adafruit_nRF52_Bootloader/releases)
   — for nice!nano: `update-nice_nano_bootloader-<version>_nosd.uf2`
   (the `nosd` variant keeps the installed SoftDevice, which ZMK needs).
2. Enter the bootloader (double-tap reset) and copy the updater UF2 to
   the `NICENANO` drive. The device installs it and reboots.
3. Re-enter the bootloader and confirm the version in `INFO_UF2.TXT`.
4. Repeat for the other half.

To revert, flash any older `update-nice_nano_bootloader-*.uf2` from the
same releases page the same way — the process is not one-way.

### macOS flashing note

Recent macOS versions (FSKit FAT driver) break Finder/`cp` copies to
UF2 bootloader drives — writes are silently dropped or fail with error
-50. Flash from the terminal against the raw disk instead:

```sh
diskutil list                       # find the NICENANO disk number
diskutil unmountDisk disk4          # keep the device attached
sudo dd if=firmware.uf2 of=/dev/rdisk4 bs=512
```

The bootloader flashes any 512-byte sector carrying UF2 magic, so the
filesystem layer is not needed.

## Repository layout

```
src/                 engine, v4 decoder, BLE split protocol, inflate
tools/compile_v4.py  dictionary compiler (both JSONs → two half blobs)
tools/fetch_dict.py  dictionary downloader (Plover main, Lapwing base)
tests/test_dict_v4.c host round-trip test (42,000 vectors)
```

### Development

The dictionary compiler (`tools/compile_v4.py`) requires Python 3 with
`numpy`. GitHub Actions runners have this preinstalled; for local builds
install via `pip install numpy`.

## License

PolyForm Noncommercial 1.0.0 — see [LICENSE](LICENSE).
