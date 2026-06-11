#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

#include "ARM7TDMI.hpp"
#include "Bus.hpp"
#include "DMA.hpp"
#include "PPU.hpp"
#include "Timers.hpp"

namespace {

constexpr int STEPS_PER_FRAME =
    PPU::CYCLES_SCANLINE * PPU::LINES_TOTAL / 4;

const char* modeName(uint16_t dispcnt) {
    static const char* names[8] = {"0 (text)",   "1 (text+affine)",
                                   "2 (affine)", "3 (bitmap16)",
                                   "4 (bitmap8)", "5 (bitmap-small)",
                                   "6 (invalid)", "7 (invalid)"};
    return names[dispcnt & 7];
}

}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: gba_diag <rom.gba> [frames]\n");
        return 1;
    }
    const int frames = argc > 2 ? std::atoi(argv[2]) : 1200;

    Bus bus;
    ARM7TDMI cpu(bus);
    PPU ppu(bus);
    DMA dma(bus);
    Timers timers(bus);
    bus.attachDMA(&dma);
    bus.attachTimers(&timers);

    if (!bus.loadROM(argv[1])) {
        std::fprintf(stderr, "Failed to load ROM: %s\n", argv[1]);
        return 1;
    }
    std::printf("ROM loaded: %zu bytes, backup type %d\n", bus.romSize(),
                static_cast<int>(bus.backupType()));
    cpu.reset();
    bus.writeIODirect16(0x04000130, 0x03FF);

    uint16_t lastDispcnt = bus.read16(0x04000000);
    uint16_t lastDispstat = 0xFFFF;
    uint16_t lastDmaCtrl[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};

    std::printf("frame 0: DISPCNT=%04X mode=%s\n", lastDispcnt,
                modeName(lastDispcnt));

    for (int frame = 0; frame < frames; ++frame) {
        uint16_t keys = 0x03FF;
        const int phase = frame % 120;
        if (phase < 5) {
            keys &= static_cast<uint16_t>(
                ~((frame / 120) % 2 == 0 ? (1u << 3) : (1u << 0)));
        }
        bus.writeIODirect16(0x04000130, keys);

        std::set<uint32_t> pcs;
        int haltedSteps = 0;
        for (int s = 0; s < STEPS_PER_FRAME; ++s) {
            cpu.step();
            ppu.step(4);
            timers.step(4);
            if (cpu.isHalted()) {
                ++haltedSteps;
            }
            if ((s & 63) == 0) {
                pcs.insert(cpu.reg(15));
            }

            if (s % (PPU::CYCLES_SCANLINE / 4) == 0) {
                const uint16_t dispcnt = bus.read16(0x04000000);
                if (dispcnt != lastDispcnt) {
                    std::printf(
                        "frame %d: DISPCNT %04X -> %04X mode=%s page=%d "
                        "BG=%d%d%d%d OBJ=%d\n",
                        frame, lastDispcnt, dispcnt, modeName(dispcnt),
                        (dispcnt >> 4) & 1, (dispcnt >> 8) & 1,
                        (dispcnt >> 9) & 1, (dispcnt >> 10) & 1,
                        (dispcnt >> 11) & 1, (dispcnt >> 12) & 1);
                    lastDispcnt = dispcnt;
                }
                const uint16_t dispstat = bus.read16(0x04000004) & 0xFFF8;
                if (dispstat != lastDispstat) {
                    std::printf(
                        "frame %d: DISPSTAT=%04X vbl_irq=%d hbl_irq=%d "
                        "vcount_irq=%d vcount_target=%d\n",
                        frame, dispstat, (dispstat >> 3) & 1,
                        (dispstat >> 4) & 1, (dispstat >> 5) & 1,
                        dispstat >> 8);
                    lastDispstat = dispstat;
                }
                for (int ch = 0; ch < 4; ++ch) {
                    const uint16_t ctrl = bus.read16(
                        0x040000B0 + static_cast<uint32_t>(ch) * 12 + 0xA);
                    if (ctrl != lastDmaCtrl[ch]) {
                        std::printf(
                            "frame %d: DMA%d ctrl %04X (en=%d timing=%d "
                            "repeat=%d)\n",
                            frame, ch, ctrl, ctrl >> 15, (ctrl >> 12) & 3,
                            (ctrl >> 9) & 1);
                        lastDmaCtrl[ch] = ctrl;
                    }
                }
            }
        }

        if (frame % 60 == 59 || frame == frames - 1) {
            uint32_t lo = 0xFFFFFFFF;
            uint32_t hi = 0;
            for (uint32_t pc : pcs) {
                lo = pc < lo ? pc : lo;
                hi = pc > hi ? pc : hi;
            }
            std::printf(
                "frame %d: PC samples=%zu range=[%08X..%08X] halted=%d%% "
                "IE=%04X IF=%04X IME=%d\n",
                frame, pcs.size(), lo, hi,
                haltedSteps * 100 / STEPS_PER_FRAME,
                bus.read16(0x04000200), bus.read16(0x04000202),
                bus.read16(0x04000208) & 1);
        }
    }

    std::printf(
        "final: PC=%08X CPSR=%08X halted=%d thumb=%d DISPCNT=%04X "
        "mode=%s\n",
        cpu.reg(15), cpu.getCPSR(), cpu.isHalted(), cpu.inThumbState(),
        bus.read16(0x04000000), modeName(bus.read16(0x04000000)));
    return 0;
}
