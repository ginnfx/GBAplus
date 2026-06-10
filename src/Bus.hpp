#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

class DMA;
class Timers;

// Central memory bus. All components (CPU, PPU, DMA, ...) talk to each
// other exclusively through this class — they never reference each other
// directly.
//
// GBA memory map (GBATEK):
//   0x00000000 - 0x00003FFF  BIOS        (16 KiB)
//   0x02000000 - 0x0203FFFF  EWRAM       (256 KiB, mirrored every 0x40000)
//   0x03000000 - 0x03007FFF  IWRAM       (32 KiB,  mirrored every 0x8000)
//   0x04000000 - 0x040003FF  IO          (1 KiB)
//   0x05000000 - 0x050003FF  Palette RAM (1 KiB, mirrored)
//   0x06000000 - 0x06017FFF  VRAM        (96 KiB, mirrored every 0x20000;
//                                         upper 32 KiB mirrors 0x10000-0x17FFF)
//   0x07000000 - 0x070003FF  OAM         (1 KiB, mirrored)
//   0x08000000 - 0x09FFFFFF  Cartridge ROM (up to 32 MiB; also visible at
//                                         0x0A and 0x0C wait-state mirrors)
//   0x0E000000 - 0x0E007FFF  Cartridge SRAM (32 KiB, 8-bit interface)
class Bus {
public:
    // Interrupt request bits (REG_IE / REG_IF).
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

    // Loads a raw binary into the cartridge ROM region (0x08000000).
    // Returns false if the file could not be opened or exceeds 32 MiB.
    bool loadROM(const std::string& filepath);

    size_t romSize() const { return rom.size(); }

    // Loads a 16 KiB BIOS image at 0x00000000. When absent, the CPU HLEs
    // the BIOS IRQ vector instead of executing the zeroed BIOS region.
    bool loadBIOS(const std::string& filepath);
    bool hasBIOS() const { return biosLoaded; }

    // Hardware-side IO write (PPU status bits, frontend key state, ...).
    // Bypasses the game-facing write masks and the IF acknowledge logic.
    void writeIODirect16(uint32_t addr, uint16_t value);

    // Cartridge backup media (.sav files mirroring the SRAM buffer).
    bool loadCartridgeData(const std::string& filepath);
    bool saveCartridgeData(const std::string& filepath) const;
    bool sramDirty() const { return sramWritten; }

    // The DMA controller is notified through the Bus when games touch its
    // control registers or when the PPU reaches HBlank/VBlank.
    void attachDMA(DMA* controller) { dma = controller; }
    void attachTimers(Timers* controller) { timers = controller; }
    void notifyVBlank();
    void notifyHBlank();

    // Sets bits in REG_IF. Used by hardware (PPU, DMA, ...) to raise IRQs;
    // CPU-side writes to REG_IF go through write8/16 and acknowledge
    // (clear) bits instead.
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
    bool sramWritten = false;
    bool biosLoaded = false;

    static uint32_t mirrorVRAM(uint32_t addr);
    static uint8_t ioWriteMask(uint32_t offset);
};
