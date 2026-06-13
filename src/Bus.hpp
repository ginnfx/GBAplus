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

class Bus {
public:
    enum class BackupType {
        SRAM,
        Flash64,
        Flash128,
        EEPROM,
    };

    static constexpr uint16_t IRQ_VBLANK = 1u << 0;
    static constexpr uint16_t IRQ_HBLANK = 1u << 1;
    static constexpr uint16_t IRQ_VCOUNT = 1u << 2;
    static constexpr uint16_t IRQ_TIMER0 = 1u << 3;
    static constexpr uint16_t IRQ_SERIAL = 1u << 7;
    static constexpr uint16_t IRQ_DMA0   = 1u << 8;

    Bus();

    uint8_t  read8(uint32_t addr);
    uint16_t read16(uint32_t addr);
    uint32_t read32(uint32_t addr);

    void write8(uint32_t addr, uint8_t value);
    void write16(uint32_t addr, uint16_t value);
    void write32(uint32_t addr, uint32_t value);

    uint16_t peek16(uint32_t addr);

    int consumeCycles();

    bool loadROM(const std::string& filepath);

    size_t romSize() const { return rom.size(); }

    bool loadBIOS(const std::string& filepath);
    bool hasBIOS() const { return biosLoaded; }

    void writeIODirect16(uint32_t addr, uint16_t value);

    bool loadCartridgeData(const std::string& filepath);
    bool saveCartridgeData(const std::string& filepath) const;
    bool sramDirty() const { return sramWritten; }
    BackupType backupType() const { return backup; }

    void attachDMA(DMA* controller) { dma = controller; }
    void attachTimers(Timers* controller) { timers = controller; }
    void attachAPU(APU* unit) { apu = unit; }
    void notifyVBlank();
    void notifyHBlank();

    void notifyTimerOverflow(int timer);
    void requestFifoDMA(int fifo);

    void requestInterrupt(uint16_t mask);

    void serialize(Serializer& s) const;
    void deserialize(Deserializer& d);

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
    std::vector<uint8_t>              backupMem;
    std::vector<uint8_t>              rom;

    DMA* dma = nullptr;
    Timers* timers = nullptr;
    APU* apu = nullptr;
    bool sramWritten = false;
    bool biosLoaded = false;

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

    void eepromWriteBit(uint32_t addr, uint8_t value);
    uint8_t eepromReadBit(uint32_t addr);
    void eepromInterpretRequest();
    void eepromSetAddrBits(int addrBits);

    int eepromAddrBits = 0;
    std::vector<uint8_t> eepromBits;
    bool eepromReadActive = false;
    int eepromReadPos = 0;
    uint64_t eepromReadValue = 0;

    uint8_t dispatchRead8(uint32_t addr);
    void dispatchWrite8(uint32_t addr, uint8_t value);

    int accessCycles(uint32_t addr, int width) const;
    void updateWaitstate();
    int64_t cycleAccum = 0;
    int ws0N = 0, ws0S = 0, ws1N = 0, ws1S = 0, ws2N = 0, ws2S = 0;
    int sramWait = 0;

    void finishSerialTransfer();

    bool hasRtc = false;
    uint8_t gpioData = 0;
    uint8_t gpioDir = 0;
    bool gpioReadable = false;
    uint8_t gpioRead(uint32_t addr);
    void gpioWrite(uint32_t addr, uint8_t value);
    void rtcClock();
    void rtcBeginCommand();
    void rtcFillDateTime(uint8_t* out) const;

    enum class RtcState { Idle, Command, Reading, Writing };
    struct Rtc {
        RtcState state = RtcState::Idle;
        uint8_t command = 0;
        int bits = 0;
        uint8_t data[7] = {};
        int length = 0;
        int byteIndex = 0;
        int bitIndex = 0;
        uint8_t buffer = 0;
        uint8_t status = 0x40;
        bool prevSck = false;
        bool prevCs = false;
    } rtc;

    static uint32_t mirrorVRAM(uint32_t addr);
    static uint8_t ioWriteMask(uint32_t offset);
};
