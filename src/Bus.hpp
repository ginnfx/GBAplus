#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

class APU;
class DMA;
class Timers;

class Bus {
public:
    static constexpr uint16_t IRQ_VBLANK = 1u << 0;
    static constexpr uint16_t IRQ_HBLANK = 1u << 1;
    static constexpr uint16_t IRQ_VCOUNT = 1u << 2;
    static constexpr uint16_t IRQ_TIMER0 = 1u << 3;
    static constexpr uint16_t IRQ_DMA0   = 1u << 8;

    Bus();

    uint8_t  read8(uint32_t addr) const;
    uint16_t read16(uint32_t addr) const;
    uint32_t read32(uint32_t addr) const;

    void write8(uint32_t addr, uint8_t value);
    void write16(uint32_t addr, uint16_t value);
    void write32(uint32_t addr, uint32_t value);

    bool loadROM(const std::string& filepath);

    size_t romSize() const { return rom.size(); }

    bool loadBIOS(const std::string& filepath);
    bool hasBIOS() const { return biosLoaded; }

    void writeIODirect16(uint32_t addr, uint16_t value);

    bool loadCartridgeData(const std::string& filepath);
    bool saveCartridgeData(const std::string& filepath) const;
    bool sramDirty() const { return sramWritten; }

    void attachDMA(DMA* controller) { dma = controller; }
    void attachTimers(Timers* controller) { timers = controller; }
    void attachAPU(APU* unit) { apu = unit; }
    void notifyVBlank();
    void notifyHBlank();

    void notifyTimerOverflow(int timer);
    void requestFifoDMA(int fifo);

    void requestInterrupt(uint16_t mask);

private:
    static constexpr size_t BIOS_SIZE    = 0x4000;
    static constexpr size_t EWRAM_SIZE   = 0x40000;
    static constexpr size_t IWRAM_SIZE   = 0x8000;
    static constexpr size_t IO_SIZE      = 0x400;
    static constexpr size_t PALETTE_SIZE = 0x400;
    static constexpr size_t VRAM_SIZE    = 0x18000;
    static constexpr size_t OAM_SIZE     = 0x400;
    static constexpr size_t SRAM_SIZE    = 0x8000;
    static constexpr size_t ROM_MAX      = 0x2000000;

    std::array<uint8_t, BIOS_SIZE>    bios{};
    std::array<uint8_t, EWRAM_SIZE>   ewram{};
    std::array<uint8_t, IWRAM_SIZE>   iwram{};
    std::array<uint8_t, IO_SIZE>      io{};
    std::array<uint8_t, PALETTE_SIZE> palette{};
    std::array<uint8_t, VRAM_SIZE>    vram{};
    std::array<uint8_t, OAM_SIZE>     oam{};
    std::array<uint8_t, SRAM_SIZE>    sram{};
    std::vector<uint8_t>              rom;

    DMA* dma = nullptr;
    Timers* timers = nullptr;
    APU* apu = nullptr;
    bool sramWritten = false;
    bool biosLoaded = false;

    static uint32_t mirrorVRAM(uint32_t addr);
    static uint8_t ioWriteMask(uint32_t offset);
};
