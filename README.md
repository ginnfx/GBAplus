# GBAplus

A WIP Game Boy Advance emulator written in C++ with SDL2.

GBAplus emulates the ARM7TDMI CPU (both ARM and Thumb instruction sets), the
tiled and bitmap PPU video modes, 4-channel DMA, hardware timers, and the
full audio unit. a LOT of ts was done using documentation from GBATEK, ARM7TDMI Technical Reference Manual and mGBA. It runs as a desktop app (SDL2 + Dear ImGui) with a cover-art game library, save states, and display options.

## Download

Grab the latest build for your OS from the
[Releases](https://github.com/ginnfx/GBAplus/releases) page:

| OS | File | Notes |
|---|---|---|
| Windows | `GBA-Emu-Windows-x64.exe` | Portable — just run it, nothing to install. |
| macOS | `GBA-Emu-macOS.dmg` | Open the disk image and drag the app to Applications. First launch: right-click → Open (the app is ad-hoc signed, not notarized). |
| Linux | `GBA-Emu-x86_64.AppImage` | `chmod +x` it and run; works across distros. |


## Features

- **CPU** — ARM7TDMI with the 3-stage pipeline modeled, full register
  banking across all processor modes, ARM and Thumb decoders driven by
  function-pointer dispatch tables, data processing with the complete
  barrel shifter, single/halfword/signed/block data transfers, multiplies
  (including the 64-bit long forms), MRS/MSR, and BX state switching.
- **Video** — scanline-accurate timing (1232 cycles/line, 228 lines),
  tiled backgrounds (all four layers, priorities, scrolling, flips,
  4bpp/8bpp, all screen sizes), affine backgrounds (modes 1/2), all bitmap
  modes (3/4/5) sampled through the BG2 affine transform, OAM sprites with
  1D/2D mapping, affine sprites and per-pixel priority, the two windows plus
  the OBJ window, mosaic, and the colour special effects (alpha blend and
  brightness fades).
- **Audio** — square 1 (with frequency sweep), square 2, programmable wave,
  and noise channels with envelopes and length counters, plus both Direct
  Sound FIFOs fed by DMA and clocked by timer overflows. Stereo 16-bit
  output at 32768 Hz.
- **System** — 4-channel DMA (immediate / VBlank / HBlank / sound FIFO
  timing), 4 cascadable hardware timers, the full interrupt controller
  (IE/IF/IME with write-1-to-clear acknowledge), and a high-level emulation
  of the BIOS IRQ dispatch so games run without a BIOS dump (a real BIOS
  image is also supported). Memory access timing is wait-state aware
  (driven by WAITCNT) rather than a flat per-instruction cost.
- **Cartridge hardware** — a serial-link stub (transfers complete as
  "no partner connected" so link-aware games don't hang) and the GamePak
  real-time clock (Seiko S3511 over GPIO) for titles that link `SIIRTC_V`.
- **Persistence** — battery-backed saves written alongside the ROM as a
  `.sav` file, covering SRAM, Flash (64/128 KiB), and serial EEPROM. Full
  save states snapshot the whole machine to numbered slots.
- **Frontend** — an SDL2 + Dear ImGui desktop app with a cover-art game
  library that can scan several folders at once, 10 save-state slots (quick
  save/load plus a prompt to resume from the latest save), display options
  (fullscreen, VSync, integer scaling, linear filtering), scanline/CRT/LCD
  overlays, and native UI fonts (SF Pro on macOS, Roboto elsewhere).
- **Tooling** — a standalone test harness covering the CPU, PPU, DMA,
  timers, APU, and interrupt paths, plus a per-instruction trace mode for
  diffing execution against other emulators.

## Building

Use the releases tab, as that tends to be updated the most & has a GUI.

GBAplus is portable C++20. SDL2 is vendored and built from source (CMake
`FetchContent`), so the only things you need are CMake (3.20+), a C++20
compiler, and git. The resulting binaries link SDL2 statically and have no
runtime SDL dependency.

### macOS

```sh
brew install cmake
cmake -B build
cmake --build build
```

### Linux (Debian/Ubuntu — adjust for your package manager)

SDL2 still needs its backend headers to build from source:

```sh
sudo apt install cmake g++ git \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev \
  libwayland-dev wayland-protocols libxkbcommon-dev libgl1-mesa-dev \
  libasound2-dev libpulse-dev libdbus-1-dev libudev-dev
cmake -B build
cmake --build build
```

### Windows

With CMake and Visual Studio's C++ workload installed:

```bat
cmake -B build
cmake --build build --config Release
```

The build defaults to an optimized Release configuration and produces
`gba_emu` (the emulator), `gba_test_harness` (the test suite), and `gba_diag`
(a headless diagnostic runner). Tagged pushes (`v*`) build the per-OS release
bundles automatically via GitHub Actions.

## Usage

```sh
./build/gba_emu game.gba                 # run a ROM
./build/gba_emu game.gba --bios bios.bin # use a real BIOS image
./build/gba_emu game.gba --trace         # write per-instruction CPU state
./build/gba_emu --demo                   # no ROM: renders a test gradient
```

Save data is read from and written to `game.sav` next to the ROM.

Launching without a ROM opens the library. Use **File > Add Games to
Library...** to point GBAplus at one or more folders of `.gba` ROMs; box art
is read from a `covers/` subfolder next to them (run
`tools/fetch_covers.py <folder>` to download it). Click a cover to play, and
if the game has save states it offers to resume from the latest one. Display
overlays live under **Video > Graphics Settings > Shaders**.

### Controls

| Action | Key |
|---|---|
| D-Pad | W / A / S / D |
| A / B | K / L |
| L / R | Q / E |
| Start | Enter |
| Select | Backspace |
| Pause | P |
| Fast-forward | Tab (hold) |
| Save / load state | F5 / F8 |
| Fullscreen | F11 |
| Quit | Esc |

## Testing

```sh
cmake --build build --target gba_test_harness
./build/gba_test_harness
```

The harness needs no SDL or display and exits non-zero on any failure,
making it suitable for CI.

For accuracy work, `--trace` writes `emu_trace.log` and
`compare_logs.py` diffs it against a trace from a reference emulator such
as mGBA.

## Current state
Most games boot. some appear to work if given time/skipping logos (AOS)
Visual artifacts also are rare. Stuff like green flashes or weird artifacting for example. 

## Accuracy notes
Memory access timing is wait-state aware (driven by WAITCNT) but is a
per-access model — it does not yet classify sequential vs non-sequential
fetches or emulate the ROM prefetch buffer. All video modes (0-5, including
affine backgrounds and sprites, windows, mosaic, and the colour special
effects) and all four backup types (SRAM, Flash 64/128 KiB, EEPROM) are in,
along with a serial-link stub and the GamePak RTC. Hardware behavior follows
the GBATEK documentation.
