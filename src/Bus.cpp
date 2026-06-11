#include "Bus.hpp"

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

uint8_t Bus::read8(uint32_t addr) const {
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
        case 0x08:
        case 0x09:
        case 0x0A:
        case 0x0B:
        case 0x0C:
        case 0x0D: {
            uint32_t offset = addr & (ROM_MAX - 1);
            if (offset < rom.size()) {
                return rom[offset];
            }
            TRACE_LOG("read8 past end of ROM 0x%08X", addr);
            return 0xFF;
        }
        case 0x0E:
        case 0x0F:
            return sram[addr & (SRAM_SIZE - 1)];
        default:
            TRACE_LOG("read8 from unmapped address 0x%08X", addr);
            return 0;
    }
}

uint16_t Bus::read16(uint32_t addr) const {
    addr &= ~1u;
    return static_cast<uint16_t>(read8(addr)) |
           static_cast<uint16_t>(read8(addr + 1)) << 8;
}

uint32_t Bus::read32(uint32_t addr) const {
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
            sram[addr & (SRAM_SIZE - 1)] = value;
            sramWritten = true;
            return;
        case 0x00:
        case 0x08:
        case 0x09:
        case 0x0A:
        case 0x0B:
        case 0x0C:
        case 0x0D:
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
    return file.good();
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
    file.read(reinterpret_cast<char*>(sram.data()),
              static_cast<std::streamsize>(SRAM_SIZE));
    return file.gcount() > 0;
}

bool Bus::saveCartridgeData(const std::string& filepath) const {
    std::ofstream file(filepath, std::ios::binary | std::ios::trunc);
    if (!file) {
        TRACE_LOG("saveCartridgeData: cannot write '%s'", filepath.c_str());
        return false;
    }
    file.write(reinterpret_cast<const char*>(sram.data()),
               static_cast<std::streamsize>(SRAM_SIZE));
    return file.good();
}
