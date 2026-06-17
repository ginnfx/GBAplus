#include "Emulator.hpp"

#include "SaveState.hpp"

namespace {
constexpr uint32_t STATE_MAGIC   = 0x31535347;
constexpr uint32_t STATE_VERSION = 1;

constexpr uint32_t REG_DISPCNT  = 0x04000000;
constexpr uint32_t REG_KEYINPUT = 0x04000130;
constexpr uint32_t VRAM_BASE    = 0x06000000;
constexpr uint32_t EWRAM_BASE   = 0x02000000;

// Escape hatch so a wedged frame can't spin forever; two frames is plenty.
constexpr long long MAX_FRAME_CYCLES =
    2LL * PPU::CYCLES_SCANLINE * PPU::LINES_TOTAL;
constexpr int HALT_STEP_CYCLES = 8;
}

Emulator::Emulator()
    : cpu(bus), ppu(bus), dma(bus), timers(bus), apu(bus) {
    bus.attachDMA(&dma);
    bus.attachTimers(&timers);
    bus.attachAPU(&apu);
}

bool Emulator::loadROM(const std::string& path) {
    if (!bus.loadROM(path)) {
        return false;
    }
    cpu.reset();
    return true;
}

bool Emulator::loadBIOS(const std::string& path) {
    return bus.loadBIOS(path);
}

void Emulator::loadDemo() {
    bus.write16(REG_DISPCNT, 0x0403);
    for (int y = 0; y < PPU::SCREEN_HEIGHT; ++y) {
        for (int x = 0; x < PPU::SCREEN_WIDTH; ++x) {
            const uint16_t r = static_cast<uint16_t>(x * 31 / 239);
            const uint16_t g = static_cast<uint16_t>(y * 31 / 159);
            const uint16_t b = static_cast<uint16_t>(31 - x * 31 / 239);
            bus.write16(VRAM_BASE + (y * 240 + x) * 2,
                        static_cast<uint16_t>(r | (g << 5) | (b << 10)));
        }
    }
    bus.write32(EWRAM_BASE, 0xEAFFFFFE);
    cpu.reset();
    cpu.setReg(15, EWRAM_BASE);
}

void Emulator::reset() {
    cpu.reset();
}

void Emulator::runFrame() {
    long long budget = 0;
    do {
        cpu.step();
        int cycles = bus.consumeCycles();
        if (cycles <= 0) {
            // CPU's halted and burning nothing, so nudge the clock ourselves.
            cycles = HALT_STEP_CYCLES;
        }
        ppu.step(cycles);
        timers.step(cycles);
        apu.step(cycles);
        bus.consumeCycles();
        budget += cycles;
    } while (!ppu.frameReady() && budget < MAX_FRAME_CYCLES);
}

void Emulator::setKeys(uint16_t keyState) {
    bus.writeIODirect16(REG_KEYINPUT, keyState);
}

void Emulator::saveState(std::vector<uint8_t>& out) const {
    Serializer s(out);
    s.pod(STATE_MAGIC);
    s.pod(STATE_VERSION);
    s.pod(bus.romHash());
    bus.serialize(s);
    cpu.serialize(s);
    ppu.serialize(s);
    dma.serialize(s);
    timers.serialize(s);
    apu.serialize(s);
}

bool Emulator::loadState(const std::vector<uint8_t>& in) {
    Deserializer d(in.data(), in.size());
    uint32_t magic = 0, version = 0, hash = 0;
    d.pod(magic);
    d.pod(version);
    d.pod(hash);
    // Reject anything that isn't ours: wrong format, old version, or a different ROM.
    if (!d.ok() || magic != STATE_MAGIC || version != STATE_VERSION ||
        hash != bus.romHash()) {
        return false;
    }
    bus.deserialize(d);
    cpu.deserialize(d);
    ppu.deserialize(d);
    dma.deserialize(d);
    timers.deserialize(d);
    apu.deserialize(d);
    return d.ok();
}
