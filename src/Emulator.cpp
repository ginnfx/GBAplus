#include "Emulator.hpp"

#include "SaveState.hpp"

namespace {
// Save-state container header: 'GSS1' magic, format version, ROM identity.
constexpr uint32_t STATE_MAGIC   = 0x31535347;  // "GSS1" little-endian
constexpr uint32_t STATE_VERSION = 1;

constexpr uint32_t REG_DISPCNT  = 0x04000000;
constexpr uint32_t REG_KEYINPUT = 0x04000130;
constexpr uint32_t VRAM_BASE    = 0x06000000;
constexpr uint32_t EWRAM_BASE   = 0x02000000;

// The PPU completes one frame every 228 scanlines; the safety cap (two frames
// of cycles) stops a pathological state (PPU somehow never reaching VBlank)
// from spinning here.
constexpr long long MAX_FRAME_CYCLES =
    2LL * PPU::CYCLES_SCANLINE * PPU::LINES_TOTAL;
// While the CPU is halted it makes no counted bus accesses, so consumeCycles()
// reports zero; advance the rest of the machine by a small quantum so time
// still flows and the interrupt that wakes the CPU can arrive.
constexpr int HALT_STEP_CYCLES = 8;
}  // namespace

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
    bus.write16(REG_DISPCNT, 0x0403);  // Mode 3, BG2 on
    for (int y = 0; y < PPU::SCREEN_HEIGHT; ++y) {
        for (int x = 0; x < PPU::SCREEN_WIDTH; ++x) {
            const uint16_t r = static_cast<uint16_t>(x * 31 / 239);
            const uint16_t g = static_cast<uint16_t>(y * 31 / 159);
            const uint16_t b = static_cast<uint16_t>(31 - x * 31 / 239);
            bus.write16(VRAM_BASE + (y * 240 + x) * 2,
                        static_cast<uint16_t>(r | (g << 5) | (b << 10)));
        }
    }
    bus.write32(EWRAM_BASE, 0xEAFFFFFE);  // B .
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
        int cycles = bus.consumeCycles();  // wait-state cost of this step
        if (cycles <= 0) {
            cycles = HALT_STEP_CYCLES;  // CPU halted: no bus traffic to charge
        }
        ppu.step(cycles);
        timers.step(cycles);
        apu.step(cycles);
        bus.consumeCycles();  // discard PPU/timer/APU (and HBlank-DMA) traffic
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
    if (!d.ok() || magic != STATE_MAGIC || version != STATE_VERSION ||
        hash != bus.romHash()) {
        return false;  // not a state for this build / this ROM
    }
    bus.deserialize(d);
    cpu.deserialize(d);
    ppu.deserialize(d);
    dma.deserialize(d);
    timers.deserialize(d);
    apu.deserialize(d);
    return d.ok();
}
