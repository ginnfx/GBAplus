# GBAplus

A Game Boy Advance emulator written in modern C++20 with SDL2.

GBAplus emulates the ARM7TDMI CPU (both ARM and Thumb instruction sets), the
tiled and bitmap PPU video modes, 4-channel DMA, hardware timers, and the
full audio unit — the four GB-style PSG channels plus the GBA's two Direct
Sound FIFO channels. Components are strictly decoupled: the CPU, PPU, DMA,
timers, and APU communicate exclusively through a central memory bus,
mirroring how the real hardware hangs off its bus.

## Features

- **CPU** — ARM7TDMI with the 3-stage pipeline modeled, full register
  banking across all processor modes, ARM and Thumb decoders driven by
  function-pointer dispatch tables, data processing with the complete
  barrel shifter, single/halfword/signed/block data transfers, multiplies
  (including the 64-bit long forms), MRS/MSR, and BX state switching.
- **Video** — scanline-accurate timing (1232 cycles/line, 228 lines),
  Mode 0 tiled backgrounds (all four layers, priorities, scrolling, flips,
  4bpp/8bpp, all screen sizes), Mode 3 bitmap, and OAM sprites with 1D/2D
  mapping and per-pixel priority.
- **Audio** — square 1 (with frequency sweep), square 2, programmable wave,
  and noise channels with envelopes and length counters, plus both Direct
  Sound FIFOs fed by DMA and clocked by timer overflows. Stereo 16-bit
  output at 32768 Hz.
- **System** — 4-channel DMA (immediate / VBlank / HBlank / sound FIFO
  timing), 4 cascadable hardware timers, the full interrupt controller
  (IE/IF/IME with write-1-to-clear acknowledge), and a high-level emulation
  of the BIOS IRQ dispatch so games run without a BIOS dump (a real BIOS
  image is also supported).
- **Persistence** — battery-backed SRAM saves written alongside the ROM as
  a `.sav` file.
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

### Linux (Debian/Ubuntu)

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

### Controls

| GBA | Key |
|---|---|
| D-Pad | W / A / S / D |
| A / B | K / L |
| L / R | Q / E |
| Start | Enter |
| Select | Backspace |
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

## Accuracy notes

GBAplus favors clean architecture over cycle accuracy. Instruction timing
is currently approximated (a flat cost per instruction rather than true
S/N/I cycle counting), affine backgrounds/sprites and PPU modes 1/2/4/5 are
not yet implemented, and backup media beyond SRAM (Flash, EEPROM) is still
to come. Hardware behavior follows the GBATEK documentation.
