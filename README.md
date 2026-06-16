# GBAplus

A WIP Game Boy Advance emulator written in C++ with SDL2.
 It runs as a desktop app (SDL2 + Dear ImGui) with a cover-art game library, save states, and display options. Hardware behaviour follows the GBATEK documentation and the ARM7TDMI Technical Reference Manual, with mGBA
as the reference emulator for accuracy 

## Download

Grab the latest build for your OS from the
[Releases](https://github.com/ginnfx/GBAplus/releases) page:

| OS | File | Notes |
|---|---|---|
| Windows | `GBA-Emu-Windows-x64.exe` |
just run it, nothing to install. |
| macOS | `GBA-Emu-macOS.dmg` | Open the disk image and drag the app to Applications. First launch: right-click → Open (the app is ad-hoc signed, not notarized). |
| Linux | `GBA-Emu-x86_64.AppImage` | `chmod +x` it and run; works across distros i think. |

## features
i had a whole detailed explanation for each of the features but i think this does a better job:

It’s a GBA emulator that barely manages to deliver the core experience. Games might run smoothly, but i wouldnt count on it. i made this for legacy of goku but alas no solution yet. the graphics are only as good as they can be but i wouldnt reccomend fullscreen because obviously the resolution was meant to be 240x160. you do get scrolling, rotation, scaling, and effects like alpha blending and fades. The sound is accurate across all channels with stereo output, but it’s nothing to write home about. You don’t need a BIOS file to play.  The real-time clock works fine for games that need it, but it’s a basic feature that should be expected. Your progress is saved as .sav files. my computer sadly thinks they're SPSS files bc of the file extension :( and you can also drop a save state at any moment with a screenshot to pick up right where you left off, but it’s nothing revolutionary. there's also a cover art library that scans folders, keyboard and controller support, save slots, rewind, speed control (including a hold-to-fast-forward button), screenshot capture, and a bunch of display options like fullscreen, integer scaling, CRT filters, and auto-pause when you switch windows, why are you still reading this? go install mgba

Cheats are easy to add with a built-in editor, but it’s not exactly groundbreaking, didnt work for the games i want to play. there’s even a trace mode to compare how it runs against other emulators so you can see the god forsaken c++ horribly be excecuted for the wimp that it is



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

