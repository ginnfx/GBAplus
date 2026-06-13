#include "Bus.hpp"

#include <algorithm>
#include <cstring>
#include <ctime>
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
    // The affine background matrices power on as the identity (PA = PD =
    // 1.0 in 8.8 fixed point). Games display affine layers without ever
    // writing these registers and rely on that state; a zero matrix would
    // collapse the whole layer to its (0,0) texel.
    io[0x020] = 0x00; io[0x021] = 0x01;  // BG2PA
    io[0x026] = 0x00; io[0x027] = 0x01;  // BG2PD
    io[0x030] = 0x00; io[0x031] = 0x01;  // BG3PA
    io[0x036] = 0x00; io[0x037] = 0x01;  // BG3PD
    // SOUNDBIAS mid-level bias, as left by the BIOS.
    io[0x088] = 0x00; io[0x089] = 0x02;
    backupMem.assign(SRAM_SIZE, 0);
    rtc.status = 0x40;  // S3511 powers up in 24-hour mode
    updateWaitstate();  // seed the wait-state cost table from WAITCNT = 0
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

// Public memory access. Every access charges the bus its wait-state cost into
// cycleAccum (consumed once per CPU step); the 16/32-bit forms compose from the
// private byte dispatcher so the cost is charged exactly once per access rather
// than once per constituent byte.
uint8_t Bus::read8(uint32_t addr) {
    cycleAccum += accessCycles(addr, 1);
    return dispatchRead8(addr);
}

uint16_t Bus::read16(uint32_t addr) {
    addr &= ~1u;
    cycleAccum += accessCycles(addr, 2);
    return static_cast<uint16_t>(dispatchRead8(addr)) |
           static_cast<uint16_t>(dispatchRead8(addr + 1) << 8);
}

uint32_t Bus::read32(uint32_t addr) {
    addr &= ~3u;
    cycleAccum += accessCycles(addr, 4);
    return static_cast<uint32_t>(dispatchRead8(addr)) |
           (static_cast<uint32_t>(dispatchRead8(addr + 1)) << 8) |
           (static_cast<uint32_t>(dispatchRead8(addr + 2)) << 16) |
           (static_cast<uint32_t>(dispatchRead8(addr + 3)) << 24);
}

// Side-effect-free, cost-free 16-bit read for the CPU's per-instruction
// control-register polls (IME/IE/IF), so those reads don't inflate the clock.
uint16_t Bus::peek16(uint32_t addr) {
    addr &= ~1u;
    return static_cast<uint16_t>(dispatchRead8(addr)) |
           static_cast<uint16_t>(dispatchRead8(addr + 1) << 8);
}

uint8_t Bus::dispatchRead8(uint32_t addr) {
    // Cartridge GPIO port (RTC etc.) overlays ROM at 0x080000C4-C9.
    if (hasRtc && addr >= 0x080000C4 && addr <= 0x080000C9) {
        return gpioRead(addr);
    }
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

void Bus::write8(uint32_t addr, uint8_t value) {
    cycleAccum += accessCycles(addr, 1);
    dispatchWrite8(addr, value);
}

// 16/32-bit writes force-align and decompose into little-endian byte writes so
// the per-byte IO side effects (DMA/timer/APU/SIO notifications) fire in the
// same order as before; the wait-state cost is charged once for the access.
void Bus::write16(uint32_t addr, uint16_t value) {
    addr &= ~1u;
    cycleAccum += accessCycles(addr, 2);
    dispatchWrite8(addr, static_cast<uint8_t>(value));
    dispatchWrite8(addr + 1, static_cast<uint8_t>(value >> 8));
}

void Bus::write32(uint32_t addr, uint32_t value) {
    addr &= ~3u;
    cycleAccum += accessCycles(addr, 4);
    dispatchWrite8(addr, static_cast<uint8_t>(value));
    dispatchWrite8(addr + 1, static_cast<uint8_t>(value >> 8));
    dispatchWrite8(addr + 2, static_cast<uint8_t>(value >> 16));
    dispatchWrite8(addr + 3, static_cast<uint8_t>(value >> 24));
}

void Bus::dispatchWrite8(uint32_t addr, uint8_t value) {
    // Cartridge GPIO port (RTC etc.) overlays ROM at 0x080000C4-C9.
    if (hasRtc && addr >= 0x080000C4 && addr <= 0x080000C9) {
        gpioWrite(addr, value);
        return;
    }
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
            // WAITCNT (0x204) reshapes the cartridge wait-state cost table.
            if (offset == 0x204 || offset == 0x205) {
                updateWaitstate();
            }
            // A write to SIOCNT's high byte (containing the IRQ-enable bit,
            // alongside the start bit just written to the low byte) drives a
            // serial transfer; with no link partner we finish it immediately.
            if (offset == 0x129) {
                finishSerialTransfer();
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
            // Flash programming can only clear bits (1->0); an erase is what
            // sets them back to 1. AND so a program over un-erased cells keeps
            // the existing zero bits, matching real flash behaviour.
            backupMem[flashBank * 0x10000 + offset] &= value;
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

    // The Seiko S3511 RTC sits behind the cartridge GPIO port. Games that use
    // it link the official "SIIRTC_V" library; detect that so the GPIO ports
    // (0x080000C4-C8) overlay ROM only for carts that actually have the chip.
    hasRtc = contains("SIIRTC_V");
    if (hasRtc) {
        TRACE_LOG("RTC (SIIRTC) detected; GPIO port enabled");
    }
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

// ---------------------------------------------------------------------------
// Wait-state-aware access timing
// ---------------------------------------------------------------------------
// Per-access bus cost in CPU cycles, derived from the region and (for the
// cartridge) the current WAITCNT setting. This is a per-access model — it does
// not distinguish sequential from non-sequential fetches or emulate the ROM
// prefetch buffer — but it is far closer than the previous flat cost and lets
// games that retune WAITCNT change their effective speed.
int Bus::accessCycles(uint32_t addr, int width) const {
    switch (addr >> 24) {
        case 0x00:  // BIOS  (32-bit bus, no wait)
        case 0x03:  // IWRAM (32-bit bus)
        case 0x04:  // IO    (32-bit bus)
        case 0x07:  // OAM   (32-bit bus)
            return 1;
        case 0x02:  // EWRAM (16-bit bus, 2 wait states)
            return width == 4 ? 6 : 3;
        case 0x05:  // Palette (16-bit bus)
        case 0x06:  // VRAM    (16-bit bus)
            return width == 4 ? 2 : 1;
        case 0x08:
        case 0x09:  // ROM wait-state region 0
            return width == 4 ? ws0N + ws0S : ws0N;
        case 0x0A:
        case 0x0B:  // ROM wait-state region 1
            return width == 4 ? ws1N + ws1S : ws1N;
        case 0x0C:
        case 0x0D:  // ROM wait-state region 2 (0x0D also EEPROM)
            return width == 4 ? ws2N + ws2S : ws2N;
        case 0x0E:
        case 0x0F:  // SRAM/Flash (8-bit bus)
            return sramWait;
        default:
            return 1;
    }
}

// Recomputes the cartridge wait-state cost table from WAITCNT (0x4000204).
// First-access (N) waits come from a shared {4,3,2,8}-cycle table; the
// second-access (S) waits are region-specific two-way selects (GBATEK).
void Bus::updateWaitstate() {
    const uint16_t w = static_cast<uint16_t>(io[0x204] | (io[0x205] << 8));
    static const int firstWait[4] = {4, 3, 2, 8};
    sramWait = 1 + firstWait[w & 3];
    ws0N = 1 + firstWait[(w >> 2) & 3];
    ws0S = 1 + (((w >> 4) & 1) ? 1 : 2);
    ws1N = 1 + firstWait[(w >> 5) & 3];
    ws1S = 1 + (((w >> 7) & 1) ? 1 : 4);
    ws2N = 1 + firstWait[(w >> 8) & 3];
    ws2S = 1 + (((w >> 10) & 1) ? 1 : 8);
}

int Bus::consumeCycles() {
    const int64_t c = cycleAccum;
    cycleAccum = 0;
    return static_cast<int>(c);
}

// ---------------------------------------------------------------------------
// Serial I/O (link cable) — no partner connected
// ---------------------------------------------------------------------------
// We do not emulate a second console, so any transfer the game starts finishes
// instantly with the line pulled high (all-ones received data) and the serial
// IRQ raised if requested. This keeps link-aware titles from hanging on a
// transfer that would otherwise never complete.
void Bus::finishSerialTransfer() {
    uint16_t cnt = static_cast<uint16_t>(io[0x128] | (io[0x129] << 8));
    if (!(cnt & (1u << 7))) {
        return;  // start/busy bit not set: nothing to do
    }
    const uint16_t rcnt = static_cast<uint16_t>(io[0x134] | (io[0x135] << 8));
    if (rcnt & (1u << 15)) {
        return;  // GPIO / JOYBUS mode selected, not a serial transfer
    }

    const int mode = (cnt >> 12) & 3;  // 0/1 Normal, 2 Multiplayer, 3 UART
    if (mode == 2) {
        // Multiplayer: this console is the only player. Its own send data
        // appears in SIOMULTI0; the absent players read back as 0xFFFF.
        const uint16_t self =
            static_cast<uint16_t>(io[0x12A] | (io[0x12B] << 8));
        writeIODirect16(0x04000120, self);
        writeIODirect16(0x04000122, 0xFFFF);
        writeIODirect16(0x04000124, 0xFFFF);
        writeIODirect16(0x04000126, 0xFFFF);
    } else {
        // Normal/UART: received shift register reads as all ones.
        writeIODirect16(0x04000120, 0xFFFF);
        writeIODirect16(0x04000122, 0xFFFF);
    }

    cnt &= static_cast<uint16_t>(~(1u << 7));  // clear start/busy
    writeIODirect16(0x04000128, cnt);
    if (cnt & (1u << 14)) {  // IRQ enable
        requestInterrupt(IRQ_SERIAL);
    }
}

// ---------------------------------------------------------------------------
// Cartridge GPIO port + Seiko S3511 real-time clock (0x080000C4-C8)
// ---------------------------------------------------------------------------
uint8_t Bus::gpioRead(uint32_t addr) {
    // When read-enable (control bit 0) is off the port is write-only and reads
    // back 0. Only the low byte of each 16-bit port carries data.
    if (!gpioReadable) {
        return 0;
    }
    switch (addr & 0xFF) {
        case 0xC4: return gpioData & 0xF;
        case 0xC6: return gpioDir & 0xF;
        case 0xC8: return gpioReadable ? 1 : 0;
        default:   return 0;  // high bytes
    }
}

void Bus::gpioWrite(uint32_t addr, uint8_t value) {
    switch (addr & 0xFF) {
        case 0xC4:
            // Drive the GBA's output pins; the RTC keeps ownership of input
            // pins (it sets gpioData's SIO bit when shifting out read data).
            gpioData = static_cast<uint8_t>((value & gpioDir & 0xF) |
                                            (gpioData & ~gpioDir & 0xF));
            rtcClock();
            break;
        case 0xC6:
            gpioDir = value & 0xF;
            break;
        case 0xC8:
            gpioReadable = value & 1;
            break;
        default:
            break;
    }
}

namespace {
uint8_t reverse8(uint8_t b) {
    b = static_cast<uint8_t>((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = static_cast<uint8_t>((b & 0xCC) >> 2 | (b & 0x33) << 2);
    b = static_cast<uint8_t>((b & 0xAA) >> 1 | (b & 0x55) << 1);
    return b;
}
uint8_t toBcd(int v) {
    return static_cast<uint8_t>(((v / 10) << 4) | (v % 10));
}
}  // namespace

// Fills the datetime register set (7 BCD bytes: year, month, day, weekday,
// hour, minute, second) from the host clock.
void Bus::rtcFillDateTime(uint8_t* out) const {
    const std::time_t t = std::time(nullptr);
    std::tm lt{};
#if defined(_WIN32)
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    int hour = lt.tm_hour;
    if (!(rtc.status & 0x40) && hour >= 12) {  // 12-hour mode: PM flag in bit 7
        out[4] = static_cast<uint8_t>(toBcd(hour > 12 ? hour - 12 : hour) | 0x80);
    } else {
        out[4] = toBcd(hour);
    }
    out[0] = toBcd(lt.tm_year % 100);
    out[1] = toBcd(lt.tm_mon + 1);
    out[2] = toBcd(lt.tm_mday);
    out[3] = toBcd(lt.tm_wday);
    out[5] = toBcd(lt.tm_min);
    out[6] = toBcd(lt.tm_sec);
}

void Bus::rtcBeginCommand() {
    // Command byte: 0b0110 <reg[2:0]> <rw>. Some games clock it LSB-first, in
    // which case the fixed 0b0110 lands in the low nibble; detect and reverse.
    uint8_t c = rtc.command;
    if ((c & 0xF0) != 0x60 && (c & 0x0F) == 0x06) {
        c = reverse8(c);
    }
    const int reg = (c >> 1) & 7;
    const bool reading = c & 1;
    rtc.byteIndex = 0;
    rtc.bitIndex = 0;
    rtc.buffer = 0;
    switch (reg) {
        case 0:  // reset
            rtc.status = 0x40;
            rtc.state = RtcState::Idle;
            break;
        case 4:  // control/status: 1 byte
            rtc.length = 1;
            if (reading) rtc.data[0] = rtc.status;
            rtc.state = reading ? RtcState::Reading : RtcState::Writing;
            break;
        case 2:  // datetime: 7 bytes
            rtc.length = 7;
            if (reading) rtcFillDateTime(rtc.data);
            rtc.state = reading ? RtcState::Reading : RtcState::Writing;
            break;
        case 6: {  // time: 3 bytes (hour, minute, second)
            rtc.length = 3;
            if (reading) {
                uint8_t dt[7];
                rtcFillDateTime(dt);
                rtc.data[0] = dt[4];
                rtc.data[1] = dt[5];
                rtc.data[2] = dt[6];
            }
            rtc.state = reading ? RtcState::Reading : RtcState::Writing;
            break;
        }
        default:
            rtc.state = RtcState::Idle;
            break;
    }
}

// Bit-banged S3511 protocol. CS frames a transaction; the command byte is
// clocked in MSB-first on SCK rising edges, then data bytes follow LSB-first
// (driven out on falling edges for reads, sampled on rising edges for writes).
void Bus::rtcClock() {
    const bool sck = gpioData & 1;
    const bool sio = (gpioData >> 1) & 1;
    const bool cs = (gpioData >> 2) & 1;

    if (!cs) {  // chip deselected: idle, await a fresh command
        rtc.state = RtcState::Command;
        rtc.bits = 0;
        rtc.command = 0;
        rtc.prevSck = sck;
        rtc.prevCs = cs;
        return;
    }
    if (cs && !rtc.prevCs) {  // CS just rose
        rtc.state = RtcState::Command;
        rtc.bits = 0;
        rtc.command = 0;
    }
    const bool rising = sck && !rtc.prevSck;
    const bool falling = !sck && rtc.prevSck;
    rtc.prevSck = sck;
    rtc.prevCs = cs;

    if (rtc.state == RtcState::Command) {
        if (rising) {
            rtc.command = static_cast<uint8_t>((rtc.command << 1) | (sio ? 1 : 0));
            if (++rtc.bits == 8) {
                rtcBeginCommand();
            }
        }
    } else if (rtc.state == RtcState::Reading) {
        if (falling) {  // present the next bit (LSB first) on SIO
            const int bit = (rtc.data[rtc.byteIndex] >> rtc.bitIndex) & 1;
            gpioData = static_cast<uint8_t>((gpioData & ~0x2u) | (bit << 1));
            if (++rtc.bitIndex == 8) {
                rtc.bitIndex = 0;
                if (++rtc.byteIndex >= rtc.length) {
                    rtc.state = RtcState::Idle;
                }
            }
        }
    } else if (rtc.state == RtcState::Writing) {
        if (rising) {  // sample the incoming bit (LSB first)
            rtc.buffer = static_cast<uint8_t>(rtc.buffer | ((sio ? 1 : 0) << rtc.bitIndex));
            if (++rtc.bitIndex == 8) {
                rtc.data[rtc.byteIndex] = rtc.buffer;
                rtc.buffer = 0;
                rtc.bitIndex = 0;
                if (++rtc.byteIndex >= rtc.length) {
                    if (rtc.length == 1) {
                        rtc.status = rtc.data[0];  // control register write
                    }
                    rtc.state = RtcState::Idle;
                }
            }
        }
    }
}
