#include "Bus.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>

#include "APU.hpp"
#include "DMA.hpp"
#include "Log.hpp"
#include "Timers.hpp"

namespace {
constexpr uint32_t IO_KEYINPUT = 0x130;
constexpr uint32_t IO_IF       = 0x202;

// High byte of each channel's DMAxCNT_H register (holds the enable bit).
constexpr uint32_t DMA_CTRL_HI[4] = {0xBB, 0xC7, 0xD3, 0xDF};
}  // namespace

Bus::Bus() {
    // KEYINPUT is active low: all buttons released at power-on.
    io[IO_KEYINPUT] = 0xFF;
    io[IO_KEYINPUT + 1] = 0x03;
    backupMem.assign(SRAM_SIZE, 0);
}

// Game-facing IO write masks: a 0 bit is read-only and survives any game
// write. Hardware updates these bits through writeIODirect16 instead.
uint8_t Bus::ioWriteMask(uint32_t offset) {
    switch (offset) {
        case 0x004:              // DISPSTAT low: VBlank/HBlank/VCount flags
            return 0xF8;
        case 0x006:              // VCOUNT
        case 0x007:
        case 0x130:              // KEYINPUT
        case 0x131:
            return 0x00;
        case 0x084:              // SOUNDCNT_X: bits 0-3 are PSG active flags
            return 0x80;
        default:
            return 0xFF;
    }
}

void Bus::writeIODirect16(uint32_t addr, uint16_t value) {
    const uint32_t offset = addr & (IO_SIZE - 1);
    io[offset] = static_cast<uint8_t>(value);
    io[(offset + 1) & (IO_SIZE - 1)] = static_cast<uint8_t>(value >> 8);
}

// VRAM is 96 KiB but mirrored within a 128 KiB window: the 64 KiB at
// 0x06000000 plus the 32 KiB at 0x06010000, which itself repeats once to
// fill 0x06018000-0x0601FFFF. The whole window then mirrors every 0x20000.
uint32_t Bus::mirrorVRAM(uint32_t addr) {
    uint32_t offset = addr & 0x1FFFF;
    if (offset >= 0x18000) {
        offset -= 0x8000;
    }
    return offset;
}

uint8_t Bus::read8(uint32_t addr) {
    switch (addr >> 24) {
        case 0x00:
            // BIOS is not mirrored; reads past 16 KiB are unmapped.
            if (addr < BIOS_SIZE) {
                return bios[addr];
            }
            TRACE_LOG("read8 from unmapped BIOS region 0x%08X", addr);
            return 0;
        case 0x02:
            return ewram[addr & (EWRAM_SIZE - 1)];
        case 0x03:
            return iwram[addr & (IWRAM_SIZE - 1)];
        case 0x04:
            if ((addr & 0x00FFFFFF) < IO_SIZE) {
                return io[addr & (IO_SIZE - 1)];
            }
            TRACE_LOG("read8 from unmapped IO 0x%08X", addr);
            return 0;
        case 0x05:
            return palette[addr & (PALETTE_SIZE - 1)];
        case 0x06:
            return vram[mirrorVRAM(addr)];
        case 0x07:
            return oam[addr & (OAM_SIZE - 1)];
        case 0x0D:
            if (backup == BackupType::EEPROM) {
                return eepromReadBit(addr);
            }
            [[fallthrough]];
        case 0x08:
        case 0x09:
        case 0x0A:
        case 0x0B:
        case 0x0C: {
            uint32_t offset = addr & (ROM_MAX - 1);
            if (offset < rom.size()) {
                return rom[offset];
            }
            TRACE_LOG("read8 past end of ROM 0x%08X", addr);
            return 0xFF;
        }
        case 0x0E:
        case 0x0F:
            return backupRead(addr);
        default:
            TRACE_LOG("read8 from unmapped address 0x%08X", addr);
            return 0;
    }
}

// 16/32-bit accesses are composed from byte reads in little-endian order.
// The GBA bus force-aligns addresses, so we mask off the low bits here.
uint16_t Bus::read16(uint32_t addr) {
    addr &= ~1u;
    return static_cast<uint16_t>(read8(addr)) |
           static_cast<uint16_t>(read8(addr + 1)) << 8;
}

uint32_t Bus::read32(uint32_t addr) {
    addr &= ~3u;
    return static_cast<uint32_t>(read16(addr)) |
           static_cast<uint32_t>(read16(addr + 2)) << 16;
}

void Bus::write8(uint32_t addr, uint8_t value) {
    switch (addr >> 24) {
        case 0x02:
            ewram[addr & (EWRAM_SIZE - 1)] = value;
            return;
        case 0x03:
            iwram[addr & (IWRAM_SIZE - 1)] = value;
            return;
        case 0x04: {
            const uint32_t offset = addr & 0x00FFFFFF;
            if (offset >= IO_SIZE) {
                TRACE_LOG("write8 to unmapped IO 0x%08X", addr);
                return;
            }
            // REG_IF is write-1-to-clear: games acknowledge interrupts by
            // writing the bits they have handled.
            if (offset == IO_IF || offset == IO_IF + 1) {
                io[offset] &= static_cast<uint8_t>(~value);
                return;
            }
            const uint8_t mask = ioWriteMask(offset);
            io[offset] = (io[offset] & ~mask) | (value & mask);
            if (dma != nullptr) {
                for (int ch = 0; ch < 4; ++ch) {
                    if (offset == DMA_CTRL_HI[ch]) {
                        dma->onControlWrite(ch);
                    }
                }
            }
            if (timers != nullptr && offset >= 0x100 && offset < 0x110) {
                timers->onRegisterWrite(offset - 0x100);
            }
            if (apu != nullptr) {
                if (offset >= 0x60 && offset < 0xA0) {
                    apu->onRegisterWrite(offset);
                } else if (offset >= 0xA0 && offset < 0xA8) {
                    apu->onFifoWrite(static_cast<int>(offset - 0xA0) / 4,
                                     value);
                }
            }
            return;
        }
        case 0x05:
            palette[addr & (PALETTE_SIZE - 1)] = value;
            return;
        case 0x06:
            vram[mirrorVRAM(addr)] = value;
            return;
        case 0x07:
            oam[addr & (OAM_SIZE - 1)] = value;
            return;
        case 0x0E:
        case 0x0F:
            backupWrite(addr, value);
            return;
        case 0x0D:
            if (backup == BackupType::EEPROM) {
                eepromWriteBit(addr, value);
                return;
            }
            [[fallthrough]];
        case 0x00:
        case 0x08:
        case 0x09:
        case 0x0A:
        case 0x0B:
        case 0x0C:
            TRACE_LOG("ignored write8 to read-only region 0x%08X", addr);
            return;
        default:
            TRACE_LOG("write8 to unmapped address 0x%08X", addr);
            return;
    }
}

void Bus::write16(uint32_t addr, uint16_t value) {
    addr &= ~1u;
    write8(addr, static_cast<uint8_t>(value));
    write8(addr + 1, static_cast<uint8_t>(value >> 8));
}

void Bus::write32(uint32_t addr, uint32_t value) {
    addr &= ~3u;
    write16(addr, static_cast<uint16_t>(value));
    write16(addr + 2, static_cast<uint16_t>(value >> 16));
}

void Bus::notifyVBlank() {
    if (dma != nullptr) {
        dma->onVBlank();
    }
}

void Bus::notifyHBlank() {
    if (dma != nullptr) {
        dma->onHBlank();
    }
}

void Bus::notifyTimerOverflow(int timer) {
    if (apu != nullptr) {
        apu->onTimerOverflow(timer);
    }
}

void Bus::requestFifoDMA(int fifo) {
    if (dma != nullptr) {
        dma->onFifoRequest(fifo);
    }
}

void Bus::requestInterrupt(uint16_t mask) {
    io[IO_IF] |= static_cast<uint8_t>(mask);
    io[IO_IF + 1] |= static_cast<uint8_t>(mask >> 8);
}

// Backup media routing for the 0x0E region. SRAM (and unemulated EEPROM)
// reads/writes go straight to the buffer; Flash goes through the command
// state machine.
uint8_t Bus::backupRead(uint32_t addr) const {
    if (backup == BackupType::Flash64 || backup == BackupType::Flash128) {
        const uint32_t offset = addr & 0xFFFF;
        if (flashIdMode) {
            // Flash64 reports SST (0xBF 0xD4), Flash128 Macronix (0xC2 09).
            if (offset == 0) {
                return backup == BackupType::Flash64 ? 0xBF : 0xC2;
            }
            if (offset == 1) {
                return backup == BackupType::Flash64 ? 0xD4 : 0x09;
            }
        }
        return backupMem[flashBank * 0x10000 + offset];
    }
    return backupMem[addr & (SRAM_SIZE - 1)];
}

void Bus::backupWrite(uint32_t addr, uint8_t value) {
    if (backup == BackupType::Flash64 || backup == BackupType::Flash128) {
        flashWrite(addr & 0xFFFF, value);
        return;
    }
    backupMem[addr & (SRAM_SIZE - 1)] = value;
    sramWritten = true;
}

// Flash command sequences: 0xAA @ 0x5555, 0x55 @ 0x2AAA, then the command
// byte. 0x80 arms erase for the following sequence (0x10 = chip erase,
// 0x30 @ sector = 4 KiB sector erase). 0xA0 programs one byte, 0xB0 selects
// the 64 KiB bank on 128 KiB chips, 0x90/0xF0 enter/leave chip-ID mode.
void Bus::flashWrite(uint32_t offset, uint8_t value) {
    switch (flashState) {
        case FlashState::Program:
            backupMem[flashBank * 0x10000 + offset] = value;
            sramWritten = true;
            flashState = FlashState::Ready;
            return;
        case FlashState::Bank:
            if (offset == 0) {
                flashBank = value & 1;
            }
            flashState = FlashState::Ready;
            return;
        default:
            break;
    }

    if (offset == 0x5555 && value == 0xAA) {
        flashState = FlashState::Cmd1;
        return;
    }
    if (flashState == FlashState::Cmd1 && offset == 0x2AAA &&
        value == 0x55) {
        flashState = FlashState::Cmd2;
        return;
    }
    if (flashState == FlashState::Cmd2) {
        if (offset == 0x5555) {
            switch (value) {
                case 0x90: flashIdMode = true; break;
                case 0xF0: flashIdMode = false; break;
                case 0x80: flashErasePending = true; break;
                case 0x10:
                    if (flashErasePending) {
                        std::fill(backupMem.begin(), backupMem.end(), 0xFF);
                        sramWritten = true;
                        flashErasePending = false;
                    }
                    break;
                case 0xA0: flashState = FlashState::Program; return;
                case 0xB0:
                    if (backup == BackupType::Flash128) {
                        flashState = FlashState::Bank;
                        return;
                    }
                    break;
                default:
                    TRACE_LOG("flash: unknown command 0x%02X", value);
                    break;
            }
        } else if (value == 0x30 && flashErasePending) {
            const uint32_t sector =
                flashBank * 0x10000 + (offset & 0xF000);
            std::fill(backupMem.begin() + sector,
                      backupMem.begin() + sector + 0x1000, 0xFF);
            sramWritten = true;
            flashErasePending = false;
        }
    }
    flashState = FlashState::Ready;
}

// EEPROM serial protocol. Each halfword the game DMAs in contributes one
// bit (bit 0 of the low byte); responses are clocked out one bit per read.
// Read request:  1 1 <addr> 0, answered by 68 read bits (4 dummy + 64 data
// MSB-first). Write request: 1 0 <addr> <64 data bits> 0; the game then
// polls until a read returns 1 (we are ready instantly).
void Bus::eepromWriteBit(uint32_t addr, uint8_t value) {
    if (addr & 1) {
        return;  // the bit lives in the low byte of each halfword
    }
    eepromReadActive = false;  // a new request cancels any pending readout
    eepromBits.push_back(value & 1);
}

uint8_t Bus::eepromReadBit(uint32_t addr) {
    if (addr & 1) {
        return 0;
    }
    if (!eepromReadActive && !eepromBits.empty()) {
        eepromInterpretRequest();
    }
    if (eepromReadActive) {
        const int pos = eepromReadPos++;
        if (eepromReadPos >= 68) {
            eepromReadActive = false;
        }
        if (pos < 4) {
            return 0;  // dummy bits ahead of the data
        }
        return static_cast<uint8_t>(
            (eepromReadValue >> (63 - (pos - 4))) & 1);
    }
    return 1;  // idle / write completed: report ready
}

// Locks in the address width the first time a well-formed request reveals
// it, sizing the backing store to match (64 or 1024 8-byte blocks).
void Bus::eepromSetAddrBits(int addrBits) {
    if (eepromAddrBits == 0) {
        eepromAddrBits = addrBits;
        backupMem.resize(addrBits == 6 ? 512 : 0x2000, 0xFF);
        TRACE_LOG("EEPROM sized: %d-bit addressing", addrBits);
    }
}

void Bus::eepromInterpretRequest() {
    const std::vector<uint8_t> bits = std::move(eepromBits);
    eepromBits.clear();
    const int len = static_cast<int>(bits.size());
    if (len < 3 || bits[0] != 1) {
        TRACE_LOG("EEPROM: malformed %d-bit request", len);
        return;
    }

    // Address width from the stream length: reads are 2+addr+1 bits,
    // writes 2+addr+64+1.
    const int addrBits = bits[1] ? len - 3 : len - 67;
    if (addrBits != 6 && addrBits != 14) {
        TRACE_LOG("EEPROM: bad request length %d", len);
        return;
    }
    eepromSetAddrBits(addrBits);

    uint32_t block = 0;
    for (int i = 0; i < addrBits; ++i) {
        block = (block << 1) | bits[2 + i];
    }
    block &= (backupMem.size() / 8) - 1;

    if (bits[1]) {  // read request: latch the block for the 68-bit answer
        eepromReadValue = 0;
        for (int i = 0; i < 8; ++i) {
            eepromReadValue =
                (eepromReadValue << 8) | backupMem[block * 8 + i];
        }
        eepromReadActive = true;
        eepromReadPos = 0;
    } else {  // write request: store the 64 data bits
        uint64_t value = 0;
        for (int i = 0; i < 64; ++i) {
            value = (value << 1) | bits[2 + addrBits + i];
        }
        for (int i = 0; i < 8; ++i) {
            backupMem[block * 8 + i] =
                static_cast<uint8_t>(value >> (8 * (7 - i)));
        }
        sramWritten = true;
    }
}

// Scans the ROM for the backup ID strings the official SDK embeds.
void Bus::detectBackupType() {
    auto contains = [this](const char* id) {
        const size_t len = std::strlen(id);
        if (rom.size() < len) {
            return false;
        }
        return std::search(rom.begin(), rom.end(), id, id + len) !=
               rom.end();
    };

    if (contains("FLASH1M_V")) {
        backup = BackupType::Flash128;
        backupMem.assign(0x20000, 0xFF);
    } else if (contains("FLASH512_V") || contains("FLASH_V")) {
        backup = BackupType::Flash64;
        backupMem.assign(0x10000, 0xFF);
    } else if (contains("EEPROM_V")) {
        backup = BackupType::EEPROM;
        // 8 KiB until the first request reveals the address width.
        backupMem.assign(0x2000, 0xFF);
        eepromAddrBits = 0;
        eepromBits.clear();
        eepromReadActive = false;
    } else {
        // Explicit SRAM_V / SRAM_F_V, or no ID at all. The fallthrough
        // default is also SRAM, but matching the strings keeps the
        // detection honest for ROMs like Aria of Sorrow (SRAM_F_V102).
        backup = BackupType::SRAM;
        backupMem.assign(SRAM_SIZE, 0);
        if (!contains("SRAM_V") && !contains("SRAM_F_V")) {
            TRACE_LOG("no backup ID string; defaulting to SRAM");
        }
    }
    flashState = FlashState::Ready;
    flashIdMode = false;
    flashErasePending = false;
    flashBank = 0;
}

bool Bus::loadROM(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file) {
        TRACE_LOG("loadROM: cannot open '%s'", filepath.c_str());
        return false;
    }

    auto size = static_cast<size_t>(file.tellg());
    if (size > ROM_MAX) {
        TRACE_LOG("loadROM: '%s' is %zu bytes, exceeds 32 MiB limit",
                  filepath.c_str(), size);
        return false;
    }

    rom.resize(size);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(rom.data()),
              static_cast<std::streamsize>(size));
    if (file.good()) {
        detectBackupType();
        return true;
    }
    return false;
}

bool Bus::loadBIOS(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file) {
        TRACE_LOG("loadBIOS: cannot open '%s'", filepath.c_str());
        return false;
    }
    file.read(reinterpret_cast<char*>(bios.data()),
              static_cast<std::streamsize>(BIOS_SIZE));
    biosLoaded = file.gcount() > 0;
    return biosLoaded;
}

bool Bus::loadCartridgeData(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file) {
        return false;  // no save yet; not an error
    }
    file.read(reinterpret_cast<char*>(backupMem.data()),
              static_cast<std::streamsize>(backupMem.size()));
    return file.gcount() > 0;
}

bool Bus::saveCartridgeData(const std::string& filepath) const {
    std::ofstream file(filepath, std::ios::binary | std::ios::trunc);
    if (!file) {
        TRACE_LOG("saveCartridgeData: cannot write '%s'", filepath.c_str());
        return false;
    }
    file.write(reinterpret_cast<const char*>(backupMem.data()),
               static_cast<std::streamsize>(backupMem.size()));
    return file.good();
}
