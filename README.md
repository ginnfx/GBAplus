# GBAplus

A WIP Game Boy Advance emulator written in C++ with SDL2.
 It runs as a desktop app (SDL2 + Dear ImGui) with a cover-art game library, save states, and display options. Hardware behaviour follows the GBATEK documentation and the ARM7TDMI Technical Reference Manual, with mGBA
as the reference emulator for accuracy cross-checks.

## Download

Grab the latest build for your OS from the
[Releases](https://github.com/ginnfx/GBAplus/releases) page:

| OS | File | Notes |
|---|---|---|
| Windows | `GBA-Emu-Windows-x64.exe` |
just run it, nothing to install. |
| macOS | `GBA-Emu-macOS.dmg` | Open the disk image and drag the app to Applications. First launch: right-click → Open (the app is ad-hoc signed, not notarized). |
| Linux | `GBA-Emu-x86_64.AppImage` | 
`chmod +x` it and run; works across distros. |


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
  (driven by WAITCNT) and models the ROM prefetch buffer, so straight-line
  cartridge code runs at sequential-access speed.
- **Cartridge hardware** — a serial-link stub (transfers complete as
  "no partner connected" so link-aware games don't hang) and the GamePak
  real-time clock (Seiko S3511 over GPIO) for titles that link `SIIRTC_V`.
- **Persistence** — battery-backed saves written alongside the ROM as a
  `.sav` file, covering SRAM, Flash (64/128 KiB), and serial EEPROM. Full
  save states snapshot the whole machine to numbered slots, each embedding a
  screenshot for the slot browser.
- **Frontend** — an SDL2 + Dear ImGui desktop app with a cover-art game
  library that can scan several folders at once, keyboard *and* game-controller
  input, 10 save-state slots with a thumbnail browser (quick save/load plus a
  prompt to resume from the latest save), rewind, a speed control
  (0.25×–8× and unlimited) alongside hold-to-fast-forward, screenshot capture,
  display options (fullscreen, VSync, integer scaling, linear filtering), GBA
  LCD colour correction, scanline/CRT/LCD overlays, auto-pause on focus loss,
  and native UI fonts (SF Pro on macOS, Roboto elsewhere).
- **Cheats** — per-game GameShark / PAR direct-write codes (8/16/32-bit RAM
  patches) with an in-app editor, applied each frame and saved next to the ROM.
- **Tooling** — a standalone test harness covering the CPU, PPU, DMA,
  timers, APU, and interrupt paths, plus a per-instruction trace mode for
  diffing execution against other emulators.

## Building (don't do this plz)

Use the releases tab. Plz. if you want to feel special the only things you need are CMake, a C++20 compiler, and git. The resulting binaries link SDL2 statically and have no
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


Launching without a ROM opens the library. Use **File > Add Games to Library...** to point GBAplus at one or more folders of `.gba` ROMs; box art is read from a `covers/` subfolder next to them (run `tools/fetch_covers.py <folder>` to download it). Click a cover to play, and if the game has save states it offers to resume from the latest one. Display
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
| Rewind | R (hold) |
| Save / load state | F5 / F8 |
| Screenshot | F12 |
| Fullscreen | F11 |
| Quit | Esc |

A game controller is detected automatically when connected (D-pad/left stick,
A/B on the face buttons, L/R shoulders, Start, and Select on the Back button).
Playback speed (0.25×–8×, unlimited), cheats, and the save-state browser live
under the **Emulation** menu; GBA colour correction is under **Video**.

## current state

Most commercial GBA games boot and run.
Visible graphical glitches are rare; occasional green-flash artefacts appear in
a small number of titles but do not affect gameplay. legacy of goku doesnt work but maybe ive just got a crappy rom. kirby works great but i just love kirby so maybe that's skewing my opinion

