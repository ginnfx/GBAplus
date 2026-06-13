# GBAplus

A WIP Game Boy Advance emulator written in C++ with SDL2.

GBAplus emulates the ARM7TDMI CPU (both ARM and Thumb instruction sets), the
tiled and bitmap PPU video modes, 4-channel DMA, hardware timers, and the
full audio unit. a LOT of ts was done using documentation from GBATEK, ARM7TDMI Technical Reference Manual and mGBA. It runs as a desktop app (SDL2 + Dear ImGui) with a cover-art game library, save states, and display options.

## Features

- **CPU** — ARM7TDMI with the 3-stage pipeline modeled, full register
  banking across all processor modes, ARM and Thumb decoders driven by
  function-pointer dispatch tables, data processing with the complete
  barrel shifter, single/halfword/signed/block data transfers, multiplies
  (including the 64-bit long forms), MRS/MSR, and BX state switching.
- **Video** — scanline-accurate timing (1232 cycles/line, 228 lines),
  tiled backgrounds (all four layers, priorities, scrolling, flips,
  4bpp/8bpp, all screen sizes), affine backgrounds (modes 1/2) and the
  mode 3/4 bitmaps, OAM sprites with 1D/2D mapping, affine sprites and
  per-pixel priority, the two windows plus the OBJ window, mosaic, and the
  colour special effects (alpha blend and brightness fades).
- **Audio** — square 1 (with frequency sweep), square 2, programmable wave,
  and noise channels with envelopes and length counters, plus both Direct
  Sound FIFOs fed by DMA and clocked by timer overflows. Stereo 16-bit
  output at 32768 Hz.
- **System** — 4-channel DMA (immediate / VBlank / HBlank / sound FIFO
  timing), 4 cascadable hardware timers, the full interrupt controller
  (IE/IF/IME with write-1-to-clear acknowledge), and a high-level emulation
  of the BIOS IRQ dispatch so games run without a BIOS dump (a real BIOS
  image is also supported).
- **Persistence** — battery-backed saves written alongside the ROM as a
  `.sav` file, covering SRAM, Flash (64/128 KiB), and serial EEPROM. Full
  save states snapshot the whole machine to numbered slots.
- **Frontend** — an SDL2 + Dear ImGui desktop app with a cover-art game
  library that can scan several folders at once, 10 save-state slots (quick
  save/load plus a prompt to resume from the latest save), display options
  (fullscreen, VSync, integer scaling, linear filtering), scanline/CRT/LCD
  overlays, and native UI fonts (SF Pro on macOS, Roboto elsewhere).
- **Tooling** — a standalone test harness (72 checks covering the CPU, PPU,
  DMA, timers, APU, and interrupt paths) and a per-instruction trace mode
  for diffing execution against other emulators.

## Building

GBAplus is portable C++20 with two dependencies: CMake (3.20+) and SDL2.

### macOS

```sh
brew install cmake sdl2
cmake -B build
cmake --build build
```

### Linux (Debian/Ubuntu/change pkg manager accordingly)

```sh
sudo apt install cmake g++ libsdl2-dev
cmake -B build
cmake --build build
```

### Windows

With [vcpkg](https://vcpkg.io) and Visual Studio's C++ workload installed:

```bat
vcpkg install sdl2
cmake -B build -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release
```

MSYS2/MinGW also works: `pacman -S mingw-w64-x86_64-cmake mingw-w64-x86_64-SDL2`
then the same two CMake commands.

The build defaults to an optimized Release configuration and produces two
binaries: `gba_emu` (the emulator) and `gba_test_harness` (the test suite).

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
Most games boot. However some appear to work if given time/skipping logos (AOS)
Visual artifacts also are rare. Stuff like green flashes or weird artifacting for example. 

## Accuracy notes
 Instruction timing is currently approximated (a flat cost per instruction
rather than true S/N/I cycle counting), and bitmap mode 5 is not yet
implemented. Affine backgrounds and sprites, the windows, mosaic, colour
special effects, and Flash/EEPROM backup are all in. Hardware behavior
follows the GBATEK documentation.
