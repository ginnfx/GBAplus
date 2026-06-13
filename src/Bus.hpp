#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

class APU;
class DMA;
class Timers;
class Serializer;
class Deserializer;

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
    // Cartridge backup media, detected from ID strings in the ROM.
    enum class BackupType {
        SRAM,      // 32 KiB battery SRAM (also the default when undetected)
        Flash64,   // 64 KiB flash (SST ID 0xBF 0xD4)
        Flash128,  // 128 KiB flash, banked (Macronix ID 0xC2 0x09)
        EEPROM,    // 512 B or 8 KiB serial EEPROM on the 0x0D region
    };

    // Interrupt request bits (REG_IE / REG_IF).
    static constexpr uint16_t IRQ_VBLANK = 1u << 0;
    static constexpr uint16_t IRQ_HBLANK = 1u << 1;
    static constexpr uint16_t IRQ_VCOUNT = 1u << 2;
    static constexpr uint16_t IRQ_TIMER0 = 1u << 3;
    static constexpr uint16_t IRQ_DMA0   = 1u << 8;

    Bus();

    // Reads are non-const: like the real hardware, some have side effects
    // (the EEPROM serial interface clocks its state machine on every read).
    uint8_t  read8(uint32_t addr);
    uint16_t read16(uint32_t addr);
    uint32_t read32(uint32_t addr);

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

    // Cartridge backup media (.sav files mirroring the backup buffer).
    bool loadCartridgeData(const std::string& filepath);
    bool saveCartridgeData(const std::string& filepath) const;
    bool sramDirty() const { return sramWritten; }
    BackupType backupType() const { return backup; }

    // The DMA controller is notified through the Bus when games touch its
    // control registers or when the PPU reaches HBlank/VBlank.
    void attachDMA(DMA* controller) { dma = controller; }
    void attachTimers(Timers* controller) { timers = controller; }
    void attachAPU(APU* unit) { apu = unit; }
    void notifyVBlank();
    void notifyHBlank();

    // Timer overflows clock the APU's Direct Sound FIFOs; a half-empty
    // FIFO asks the DMA controller for a refill. Both hops go through the
    // Bus so the Timers/APU/DMA classes never reference each other.
    void notifyTimerOverflow(int timer);
    void requestFifoDMA(int fifo);

    // Sets bits in REG_IF. Used by hardware (PPU, DMA, ...) to raise IRQs;
    // CPU-side writes to REG_IF go through write8/16 and acknowledge
    // (clear) bits instead.
    void requestInterrupt(uint16_t mask);

    // Save-state support: snapshots/restores all mutable memory and backup
    // state (but not the ROM/BIOS, which are reloaded from disk).
    void serialize(Serializer& s) const;
    void deserialize(Deserializer& d);

    // Cheap identity hash over the ROM header, used to reject save states
    // taken from a different cartridge.
    uint32_t romHash() const;

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
    std::vector<uint8_t>              backupMem;  // sized by backup type
    std::vector<uint8_t>              rom;

    DMA* dma = nullptr;
    Timers* timers = nullptr;
    APU* apu = nullptr;
    bool sramWritten = false;
    bool biosLoaded = false;

    // Flash command state machine (0x0E region when backup is Flash).
    enum class FlashState { Ready, Cmd1, Cmd2, Program, Bank };
    BackupType backup = BackupType::SRAM;
    FlashState flashState = FlashState::Ready;
    bool flashIdMode = false;
    bool flashErasePending = false;
    uint32_t flashBank = 0;

    void detectBackupType();
    uint8_t backupRead(uint32_t addr) const;
    void backupWrite(uint32_t addr, uint8_t value);
    void flashWrite(uint32_t offset, uint8_t value);

    // EEPROM serial interface (0x0D region): the game DMAs a bitstream in
    // (bit 0 of each halfword) and clocks responses out one bit per read.
    // The buffered request is interpreted on the write->read transition,
    // which is also when the 6-bit/14-bit address width becomes
    // unambiguous (stream lengths 9/73 vs 17/81).
    void eepromWriteBit(uint32_t addr, uint8_t value);
    uint8_t eepromReadBit(uint32_t addr);
    void eepromInterpretRequest();
    void eepromSetAddrBits(int addrBits);

    int eepromAddrBits = 0;  // 0 until detected; then 6 or 14
    std::vector<uint8_t> eepromBits;
    bool eepromReadActive = false;
    int eepromReadPos = 0;
    uint64_t eepromReadValue = 0;

    static uint32_t mirrorVRAM(uint32_t addr);
    static uint8_t ioWriteMask(uint32_t offset);
};
