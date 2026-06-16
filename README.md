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
| Linux | `GBA-Emu-x86_64.AppImage` | `chmod +x` it and run; works across distros. |


## features

· CPU: ARM7TDMI featuring comprehensive register banking, ARM and Thumb decoders, a full range of barrel shifter and multiply operations, as well as MRS/MSR and BX state switching capabilities.

· Video: Scanline-accurate timing, tiled backgrounds with all layers capable of scrolling, flipping, and supporting 4bpp/8bpp, affine backgrounds, bitmap modes, sprites with 1D/2D mapping and affine sprites, windows mosaic, and colour effects such as alpha blending and fades.

· Audio: Includes Square 1 with sweep, Square 2, programmable wave, and noise, all equipped with envelopes and length counters, alongside two Direct Sound FIFOs fed by DMA, clocked by timer overflows, and stereo output.

· System: Comprises four DMA channels, four timers, a full interrupt controller, high-level BIOS IRQ emulation eliminating the need for a BIOS dump, and wait-state aware memory timing that accurately models the prefetch buffer for optimal cartridge code execution.

· Cartridge hardware: Features a serial link stub to prevent link-aware games from hanging, along with real-time clock support.

· Persistence: Supports battery-backed saves in .sav files and full save states with screenshots stored in numbered slots.

· Frontend: Utilises SDL2 and Dear ImGui, includes a cover-art library that scans folders, supports keyboard and controller input, offers 10 save slots with a thumbnail browser, quick save/load functionality, rewind, speed control, hold-to-fast-forward, screenshot capture, display options (fullscreen, VSync scaling, filtering), colour correction, CRT/LCD overlays, auto-pause on focus loss, and native fonts.

· Cheats: Supports GameShark/PAR codes with an in-app editor, applied to every frame saved adjacent to the ROM.

· Tooling: Provides a standalone test harness for CPU, PPU, DMA, timers, APU, and interrupts, along with a trace mode for comparison against other emulators.

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

