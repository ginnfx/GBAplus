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

class Emulator {
public:
    Emulator();

    bool loadROM(const std::string& path);

    bool loadBIOS(const std::string& path);

    void loadDemo();

    void reset();

    void runFrame();

    const std::array<uint32_t, PPU::SCREEN_WIDTH * PPU::SCREEN_HEIGHT>&
    framebuffer() const {
        return ppu.framebuffer();
    }

    void setKeys(uint16_t keyState);

    Bus::BackupType backupType() const { return bus.backupType(); }
    bool sramDirty() const { return bus.sramDirty(); }
    bool loadSRAM(const std::string& path) {
        return bus.loadCartridgeData(path);
    }
    bool saveSRAM(const std::string& path) const {
        return bus.saveCartridgeData(path);
    }

    size_t pendingAudioFrames() const { return apu.pendingFrames(); }
    size_t drainAudio(int16_t* out, size_t maxFrames) {
        return apu.drainSamples(out, maxFrames);
    }

    void setTraceFile(std::FILE* file) { cpu.setTraceFile(file); }

    void applyCheat(uint32_t addr, uint32_t value, int width) {
        bus.cheatWrite(addr, value, width);
    }

    void setPrefetch(bool on) { bus.setPrefetch(on); }

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
