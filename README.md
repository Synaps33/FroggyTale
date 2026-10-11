<div align="center">

# 🥧 FroggyTale

**Undertale on the GB300V2 / SF2000**

</div>

---

A port of [Butterscotch](https://github.com/ButterscotchRunner/Butterscotch) to the
Ingenic JZ4775 handhelds — **GB300V2** and **SF2000**.

Those consoles have **no GPU and no OpenGL**. This port supplies a software renderer
that draws straight into the 320x240 RGB565 panel, plus the platform layer for firmware
input, video, audio and boot-time loading.

Verified against **Undertale 1.08** (WAD version 16 / GameMaker: Studio 1.4.1767+).

---

## Status

| Area | State |
|---|---|
| Core boots, loads the WAD, renders the game | Works |
| Background music from SD card | Works |
| Text rendering at 320x240 | Works |
| Desktop build of the software renderer | Compiles and runs, **output not yet verified** |
| **Menu navigation (A / arrows)** | **Broken — see [Troubleshooting](#troubleshooting)** |

Read the broken row before you install: you will get in-game, but you cannot currently
move the cursor in menus.

---

## Requirements

* A **GB300V2** or **SF2000** with a FAT-formatted SD card.
* **[`sf2000_multicore`](https://github.com/Trademarked69/sf2000_multicore)** — the
  loader that wraps the core into a `.sf2k`. Not vendored here.
* **Your own copy of Undertale.** The WAD is Toby Fox's copyrighted data and is
  deliberately not distributed with this project.

---

## Installation

### 1. Get the core

Download `butterscotch.sf2k` from the
[Releases page](https://github.com/Synaps33/FroggyTale/releases).

### 2. Build the texture atlas

The console has no GPU, so textures cannot be unpacked at runtime. Prebuild them into an
atlas from your own copy of the WAD:

```bash
python3 tools/pack_assets.py --datawin /path/to/game.unx --out build
```

This takes a few seconds and writes:

| File | Size | Contents |
|---|---|---|
| `build/ATLAS.BIN` | ~0.2 MB | index, one record per source rect |
| `build/ATLAS.RGBA4444` | ~51 MB | the pixels |

Useful flags: `--atlas-cap N` box-filters textures larger than `N` pixels,
`--report out.json` dumps a breakdown.

### 3. Install to the card

```bash
SD_ROOT=/media/<your-card> ./tools/install_to_card.sh
```

The script refuses to run unless `SD_ROOT` is a real FAT mount point, is idempotent
(skips files that already match), and verifies every file after copying. Override any
source path if your layout differs:

```bash
SD_ROOT=/media/<card> \
WAD_SRC=/path/to/game.unx \
CORE_SRC=./butterscotch.sf2k \
  ./tools/install_to_card.sh
```

### Resulting card layout

```
<card>/system/Deimos/cores/butterscotch.sf2k   the core
<card>/cores/butterscotch/core_87000000         the core (legacy path)
<card>/ROMS/butterscotch/game.unx               your game
<card>/ROMS/butterscotch/ATLAS.BIN              atlas index
<card>/ROMS/butterscotch/ATLAS.RGBA4444         atlas pixels
<card>/ROMS/butterscotch/*.ogg                  background music
<card>/ROMS/butterscotch/undertale              menu stub
```

Eject the card, boot the handheld, and launch **Undertale** from the main menu.

---

## Controls

Buttons are read from the firmware joypad registers and fanned out to **both** the
GameMaker keyboard and gamepad APIs, so games written against either path respond.

| Action | Buttons |
|---|---|
| **Confirm / Interact / Advance dialogue** | `A`, `R`, `START` |
| **Cancel / Run / Skip text** | `B`, `Y`, `L` |
| **Menu / Items / Stats** | `X`, `SELECT` |
| Move / aim | D-pad |

`R` doubles as confirm and `L` as cancel so both shoulder triggers work one-handed.
Firmware bitmasks are read directly: `A=0x2000`, `B=0x4000`, `X=0x0400`, `Y=0x0800`,
`L=0x1000`, `R=0x8000`, `START=0x0008`, `SELECT=0x0001`, D-pad `0x0010/0x0040/0x0080/0x0020`.

---

## How the rendering works

Undertale is authored for **640x480**: GML coordinates, sprite sizes and cursor step
increments are all hardcoded for that resolution. The panel is **320x240**.

This port renders into a 320x240 framebuffer and scales 2:1 on the fly. Two details
matter, both learned the hard way:

**Area-coverage downscaling, not nearest-neighbour.** Plain nearest-neighbour drops every
odd coordinate, which deletes 1px strokes — the dots on `i`, the crossbars on `t` and
`e`. The renderer instead averages a 2x2 block through a 64-entry perceptual gamma LUT
(`g_downscaleLut` in `src/sw_renderer/sw_renderer.c`).

**No dilation.** A morphological dilation pass would also restore the missing pixels, but
Undertale's fonts (`fnt_maintext`, `fnt_small`) have exactly 1px gaps between letters.
Any dilation strong enough to fatten a 1px stroke also closes that gap, fusing words into
solid white bars. Coverage filtering keeps the strokes *and* the spacing.

`blendOn` is also driven purely by `sw->blendEnable`; gating it on `gAlpha < 255` silently
disables blending of glyph anti-aliasing against the background.

---

## Building from source

The core is a static archive that `sf2000_multicore` links into the final `.sf2k`.
Both steps need `MULTICORE_SRC`, because the core's Makefile pulls `libretro.h` and
`deimos.h` from that tree:

```bash
# 1. build the core archive
make -C cores/butterscotch MULTICORE_SRC=/path/to/sf2000_multicore

# 2. link it into a .sf2k
make -C /path/to/sf2000_multicore MULTICORE_SRC=/path/to/sf2000_multicore \
     CORE=$PWD/cores/butterscotch CONSOLE=butterscotch FROGGY_TYPE=GB300V2
```

`MULTICORE_SRC` defaults to `../sf2000_multicore` next to this repo, which is only correct
if you cloned the loader as a sibling directory. Otherwise the build stops with an
explicit error rather than a cascade of missing headers.

### Toolchain constraints

Builds use **frog-toolchain** (`mipsel-mti-elf-gcc` 7.4.0):

```
-EL -march=mips32 -mtune=mips32r2 -msoft-float -G0 -mno-abicalls -fno-pic
```

**`-march` must stay plain `mips32`.** The JZ4775 has no MIPS32r2 bit-manipulation unit;
`seb`, `seh`, `ins`, `ext`, `wsbh` and `rotr` raise ExcCode 10 and kill the core.
`-mtune=mips32r2` is only a scheduling hint and is safe. There is no FPU, hence
`-msoft-float`; soft-float helpers are the single biggest performance risk on this port.
`-mno-abicalls -fno-pic -G0` make firmware calls resolve to absolute addresses from the
linker script with no GOT.

Verify a build before shipping it:

```bash
mipsel-mti-elf-objdump -f build/core.elf | grep architecture   # expect: mips:isa32
mipsel-mti-elf-objdump -d build/core.elf \
  | grep -oE '\b(seb|seh|ins|ext|wsbh|rotr)\b' | sort | uniq -c   # expect: no output
```

### Desktop build

The software renderer also builds on a desktop host, which is useful for running the VM
and input code without hardware. The Makefile builds in-tree, so run it from the repo root:

```bash
make RENDERER=sw BACKEND=sdl2
./build/butterscotch game.unx --renderer software
```

`RENDERER=sw` only *compiles* the renderer — you still need `--renderer software` at
runtime, otherwise `gfx` defaults to GL and the software path is never taken. On a host
build the frame ends in `tools/sw_present_sink.c`, which writes PNGs driven by
`SW_SHOT_PREFIX` (default `sw`) and `SW_SHOT_MAX` (default `0` = unlimited).

> [!WARNING]
> Desktop output is **not yet verified as correct**. The host path emits correctly sized
> 320x240 frames, but they come out fully black, while the GL reference at 640x480 draws
> real content from the same rooms. Treat the desktop build as a compile-and-run harness,
> not a correctness check. Validate rendering on real hardware.

---

## Troubleshooting

| Symptom | Cause |
|---|---|
| **Menus do not respond to A / arrows** | **Known issue.** `obj_time_Step_1` calls `trophy_init()` every frame; unless it reports initialised, the step event returns early and `control_update()` never runs, so `control_check_pressed()` stays false and nothing is selectable. Being tracked. |
| Black screen, core never loads | `bisrv.asd` loader missing or stale. Rebuild `sf2000_multicore` and run its `install` target. |
| Text renders as solid white bars | You are on an older `.sf2k` without the coverage-downscale fix (see [How the rendering works](#how-the-rendering-works)). |
| 1px strokes missing from glyphs | Same as above — needs area-coverage downscaling, not nearest-neighbour and not dilation. |
| Nothing appears except backgrounds | `ATLAS.BIN` / `ATLAS.RGBA4444` missing or stale. Re-run `tools/pack_assets.py`. |
| Music silent | `*.ogg` files were not copied to `ROMS/butterscotch/`. |
| Core crashes with `ExcCode 10` | Built with `-march=mips32r2`. Rebuild with plain `-march=mips32`. |
| Script aborts with `libretro.h not found` | `MULTICORE_SRC` is unset or wrong. See [Building from source](#building-from-source). |

---

## Repository layout

```
src/sf2000/         platform layer: input, video, audio, boot overlay
src/sw_renderer/    software RGB565 renderer
cores/butterscotch/ Makefile producing the core archive
tools/              pack_assets.py, install_to_card.sh, sw_present_sink.c
```

---

## Credits

Built on top of **[Butterscotch](https://github.com/ButterscotchRunner/Butterscotch)** by
[Derek Yu / mrpowergamerbr](https://mrpowergamerbr.com), an open source
re-implementation of the GameMaker: Studio runner. Thanks to
[Trademarked69](https://github.com/Trademarked69) for `sf2000_multicore`.

**Undertale** was created by **Toby Fox**. This project contains none of its assets and
requires you to supply your own copy.

---

## License

**AGPL-3.0** — inherited from Butterscotch. See [`LICENSE`](LICENSE).

## Disclaimer

No association with, endorsement of, or connection to any software this project
facilitates. You must provide your own game files.