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

constexpr uint32_t DMA_CTRL_HI[4] = {0xBB, 0xC7, 0xD3, 0xDF};
}

Bus::Bus() {
    io[IO_KEYINPUT] = 0xFF;
    io[IO_KEYINPUT + 1] = 0x03;
    io[0x020] = 0x00; io[0x021] = 0x01;
    io[0x026] = 0x00; io[0x027] = 0x01;
    io[0x030] = 0x00; io[0x031] = 0x01;
    io[0x036] = 0x00; io[0x037] = 0x01;
    io[0x088] = 0x00; io[0x089] = 0x02;
    backupMem.assign(SRAM_SIZE, 0);
    rtc.status = 0x40;
    updateWaitstate();
}

uint8_t Bus::ioWriteMask(uint32_t offset) {
    switch (offset) {
        case 0x004:
            return 0xF8;
        case 0x006:
        case 0x007:
        case 0x130:
        case 0x131:
            return 0x00;
        case 0x084:
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

uint32_t Bus::mirrorVRAM(uint32_t addr) {
    uint32_t offset = addr & 0x1FFFF;
    if (offset >= 0x18000) {
        offset -= 0x8000;
    }
    return offset;
}

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

uint16_t Bus::peek16(uint32_t addr) {
    addr &= ~1u;
    return static_cast<uint16_t>(dispatchRead8(addr)) |
           static_cast<uint16_t>(dispatchRead8(addr + 1) << 8);
}

uint8_t Bus::dispatchRead8(uint32_t addr) {
    if (hasRtc && addr >= 0x080000C4 && addr <= 0x080000C9) {
        return gpioRead(addr);
    }
    switch (addr >> 24) {
        case 0x00:
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
            if (offset == 0x204 || offset == 0x205) {
                updateWaitstate();
            }
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

uint8_t Bus::backupRead(uint32_t addr) const {
    if (backup == BackupType::Flash64 || backup == BackupType::Flash128) {
        const uint32_t offset = addr & 0xFFFF;
        if (flashIdMode) {
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

void Bus::flashWrite(uint32_t offset, uint8_t value) {
    switch (flashState) {
        case FlashState::Program:
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

void Bus::eepromWriteBit(uint32_t addr, uint8_t value) {
    if (addr & 1) {
        return;
    }
    eepromReadActive = false;
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
            return 0;
        }
        return static_cast<uint8_t>(
            (eepromReadValue >> (63 - (pos - 4))) & 1);
    }
    return 1;
}

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

    if (bits[1]) {
        eepromReadValue = 0;
        for (int i = 0; i < 8; ++i) {
            eepromReadValue =
                (eepromReadValue << 8) | backupMem[block * 8 + i];
        }
        eepromReadActive = true;
        eepromReadPos = 0;
    } else {
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
        backupMem.assign(0x2000, 0xFF);
        eepromAddrBits = 0;
        eepromBits.clear();
        eepromReadActive = false;
    } else {
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
        return false;
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

int Bus::accessCycles(uint32_t addr, int width) const {
    switch (addr >> 24) {
        case 0x00:
        case 0x03:
        case 0x04:
        case 0x07:
            return 1;
        case 0x02:
            return width == 4 ? 6 : 3;
        case 0x05:
        case 0x06:
            return width == 4 ? 2 : 1;
        case 0x08:
        case 0x09:
            return width == 4 ? ws0N + ws0S : ws0N;
        case 0x0A:
        case 0x0B:
            return width == 4 ? ws1N + ws1S : ws1N;
        case 0x0C:
        case 0x0D:
            return width == 4 ? ws2N + ws2S : ws2N;
        case 0x0E:
        case 0x0F:
            return sramWait;
        default:
            return 1;
    }
}

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

void Bus::finishSerialTransfer() {
    uint16_t cnt = static_cast<uint16_t>(io[0x128] | (io[0x129] << 8));
    if (!(cnt & (1u << 7))) {
        return;
    }
    const uint16_t rcnt = static_cast<uint16_t>(io[0x134] | (io[0x135] << 8));
    if (rcnt & (1u << 15)) {
        return;
    }

    const int mode = (cnt >> 12) & 3;
    if (mode == 2) {
        const uint16_t self =
            static_cast<uint16_t>(io[0x12A] | (io[0x12B] << 8));
        writeIODirect16(0x04000120, self);
        writeIODirect16(0x04000122, 0xFFFF);
        writeIODirect16(0x04000124, 0xFFFF);
        writeIODirect16(0x04000126, 0xFFFF);
    } else {
        writeIODirect16(0x04000120, 0xFFFF);
        writeIODirect16(0x04000122, 0xFFFF);
    }

    cnt &= static_cast<uint16_t>(~(1u << 7));
    writeIODirect16(0x04000128, cnt);
    if (cnt & (1u << 14)) {
        requestInterrupt(IRQ_SERIAL);
    }
}

uint8_t Bus::gpioRead(uint32_t addr) {
    if (!gpioReadable) {
        return 0;
    }
    switch (addr & 0xFF) {
        case 0xC4: return gpioData & 0xF;
        case 0xC6: return gpioDir & 0xF;
        case 0xC8: return gpioReadable ? 1 : 0;
        default:   return 0;
    }
}

void Bus::gpioWrite(uint32_t addr, uint8_t value) {
    switch (addr & 0xFF) {
        case 0xC4:
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
}

void Bus::rtcFillDateTime(uint8_t* out) const {
    const std::time_t t = std::time(nullptr);
    std::tm lt{};
#if defined(_WIN32)
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    int hour = lt.tm_hour;
    if (!(rtc.status & 0x40) && hour >= 12) {
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
        case 0:
            rtc.status = 0x40;
            rtc.state = RtcState::Idle;
            break;
        case 4:
            rtc.length = 1;
            if (reading) rtc.data[0] = rtc.status;
            rtc.state = reading ? RtcState::Reading : RtcState::Writing;
            break;
        case 2:
            rtc.length = 7;
            if (reading) rtcFillDateTime(rtc.data);
            rtc.state = reading ? RtcState::Reading : RtcState::Writing;
            break;
        case 6: {
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

void Bus::rtcClock() {
    const bool sck = gpioData & 1;
    const bool sio = (gpioData >> 1) & 1;
    const bool cs = (gpioData >> 2) & 1;

    if (!cs) {
        rtc.state = RtcState::Command;
        rtc.bits = 0;
        rtc.command = 0;
        rtc.prevSck = sck;
        rtc.prevCs = cs;
        return;
    }
    if (cs && !rtc.prevCs) {
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
        if (falling) {
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
        if (rising) {
            rtc.buffer = static_cast<uint8_t>(rtc.buffer | ((sio ? 1 : 0) << rtc.bitIndex));
            if (++rtc.bitIndex == 8) {
                rtc.data[rtc.byteIndex] = rtc.buffer;
                rtc.buffer = 0;
                rtc.bitIndex = 0;
                if (++rtc.byteIndex >= rtc.length) {
                    if (rtc.length == 1) {
                        rtc.status = rtc.data[0];
                    }
                    rtc.state = RtcState::Idle;
                }
            }
        }
    }
}
