#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "APU.hpp"
#include "ARM7TDMI.hpp"
#include "Bus.hpp"
#include "DMA.hpp"
#include "PPU.hpp"
#include "Timers.hpp"

// Host-agnostic facade over the whole machine. Owns the Bus and every
// component, wires them together, and exposes a frame-stepped interface that
// any frontend (the SDL/ImGui GUI, the headless diag runner) can drive. Stays
// free of SDL/UI dependencies so it can live in gba_core.
class Emulator {
public:
    Emulator();

    // Loads a cartridge and resets the CPU into its post-BIOS boot state.
    // Backup type is detected by the Bus during the load.
    bool loadROM(const std::string& path);

    // Optional real BIOS; without it the CPU HLEs the BIOS IRQ vector/SWIs.
    bool loadBIOS(const std::string& path);

    // No-ROM gradient demo: paints Mode 3 VRAM and parks the CPU on a spin
    // loop, matching the old frontend --demo behaviour.
    void loadDemo();

    void reset();

    // Runs the machine until the PPU finishes the current frame.
    void runFrame();

    const std::array<uint32_t, PPU::SCREEN_WIDTH * PPU::SCREEN_HEIGHT>&
    framebuffer() const {
        return ppu.framebuffer();
    }

    // Active-low keypad state (bit 0 = pressed) pushed to REG_KEYINPUT.
    void setKeys(uint16_t keyState);

    Bus::BackupType backupType() const { return bus.backupType(); }
    bool sramDirty() const { return bus.sramDirty(); }
    bool loadSRAM(const std::string& path) {
        return bus.loadCartridgeData(path);
    }
    bool saveSRAM(const std::string& path) const {
        return bus.saveCartridgeData(path);
    }

    // Audio drain for the host's output device.
    size_t pendingAudioFrames() const { return apu.pendingFrames(); }
    size_t drainAudio(int16_t* out, size_t maxFrames) {
        return apu.drainSamples(out, maxFrames);
    }

    void setTraceFile(std::FILE* file) { cpu.setTraceFile(file); }

    // Save states. saveState appends a versioned, ROM-tagged snapshot of the
    // whole machine to out. loadState validates the header (magic, version,
    // and ROM hash) before restoring, returning false on any mismatch.
    void saveState(std::vector<uint8_t>& out) const;
    bool loadState(const std::vector<uint8_t>& in);

private:
    Bus bus;
    ARM7TDMI cpu;
    PPU ppu;
    DMA dma;
    Timers timers;
    APU apu;
};
