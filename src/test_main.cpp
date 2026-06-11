#include <cstdio>
#include <string>

#include "APU.hpp"
#include "ARM7TDMI.hpp"
#include "Bus.hpp"
#include "DMA.hpp"
#include "PPU.hpp"
#include "Timers.hpp"

namespace {

int failures = 0;

#define CHECK(cond, fmt, ...)                                            \
    do {                                                                 \
        if (cond) {                                                      \
            std::printf("  PASS: " fmt "\n" __VA_OPT__(, ) __VA_ARGS__); \
        } else {                                                         \
            std::printf("  FAIL: " fmt "\n" __VA_OPT__(, ) __VA_ARGS__); \
            ++failures;                                                  \
        }                                                                \
    } while (0)

void testDataProcessingLoop(Bus& bus) {
    std::printf("Test: data processing loop\n");
    ARM7TDMI cpu(bus);
    cpu.reset();

    const uint32_t base = 0x02000100;
    bus.write32(base + 0,  0xE3A00000);
    bus.write32(base + 4,  0xE2800001);
    bus.write32(base + 8,  0xE3500005);
    bus.write32(base + 12, 0x1AFFFFFC);
    bus.write32(base + 16, 0xEAFFFFFE);
    cpu.setReg(15, base);

    for (int i = 0; i < 16; ++i) {
        cpu.step();
    }

    CHECK(cpu.reg(0) == 5, "r0 == 5 (got %u)", cpu.reg(0));
    CHECK(cpu.getCPSR() & ARM7TDMI::FLAG_Z,
          "Z flag set by final CMP (CPSR=0x%08X)", cpu.getCPSR());
    CHECK(cpu.reg(15) == base + 16 + 8,
          "PC parked at 0x%08X + pipeline (got 0x%08X)", base + 16,
          cpu.reg(15));
}

void testBranchWithLink(Bus& bus) {
    std::printf("Test: branch with link\n");
    ARM7TDMI cpu(bus);
    cpu.reset();

    const uint32_t base = 0x02000200;
    bus.write32(base + 0,  0xEB000001);
    bus.write32(base + 12, 0xEAFFFFFE);
    cpu.setReg(15, base);
    cpu.step();

    CHECK(cpu.reg(14) == base + 4, "r14 == 0x%08X (got 0x%08X)", base + 4,
          cpu.reg(14));
    CHECK(cpu.reg(15) == base + 12 + 8, "branched to 0x%08X (got PC 0x%08X)",
          base + 12, cpu.reg(15));
}

void testConditionCodes(Bus& bus) {
    std::printf("Test: condition codes\n");
    ARM7TDMI cpu(bus);
    cpu.reset();

    const uint32_t base = 0x02000300;
    bus.write32(base + 0,  0xE3A00007);
    bus.write32(base + 4,  0xE3500007);
    bus.write32(base + 8,  0x13A00063);
    bus.write32(base + 12, 0x03A0102A);
    bus.write32(base + 16, 0xEAFFFFFE);
    cpu.setReg(15, base);
    for (int i = 0; i < 4; ++i) {
        cpu.step();
    }

    CHECK(cpu.reg(0) == 7, "MOVNE skipped, r0 == 7 (got %u)", cpu.reg(0));
    CHECK(cpu.reg(1) == 42, "MOVEQ taken, r1 == 42 (got %u)", cpu.reg(1));
}

void testPPUMode3(Bus& bus) {
    std::printf("Test: PPU mode 3 rendering\n");
    PPU ppu(bus);

    bus.write16(0x04000000, 0x0403);

    bus.write16(0x06000000, 0x001F);
    bus.write16(0x06000002, 0x03E0);
    bus.write16(0x06000004, 0x7C00);
    bus.write16(0x06000000 + (80 * 240 + 120) * 2, 0x7FFF);

    ppu.step(PPU::CYCLES_SCANLINE * PPU::LINES_TOTAL);

    CHECK(ppu.frameReady(), "frame completed");
    const auto& fb = ppu.framebuffer();
    CHECK(fb[0] == 0xFF0000FF, "(0,0) red    -> 0xFF0000FF (got 0x%08X)",
          fb[0]);
    CHECK(fb[1] == 0x00FF00FF, "(1,0) green  -> 0x00FF00FF (got 0x%08X)",
          fb[1]);
    CHECK(fb[2] == 0x0000FFFF, "(2,0) blue   -> 0x0000FFFF (got 0x%08X)",
          fb[2]);
    CHECK(fb[80 * 240 + 120] == 0xFFFFFFFF,
          "(120,80) white -> 0xFFFFFFFF (got 0x%08X)", fb[80 * 240 + 120]);
    CHECK(bus.read16(0x04000006) == 0, "VCOUNT wrapped to 0 (got %u)",
          bus.read16(0x04000006));
}

void testThumbSwitch(Bus& bus) {
    std::printf("Test: ARM/Thumb state switching\n");
    ARM7TDMI cpu(bus);
    cpu.reset();

    const uint32_t base = 0x02000400;
    bus.write32(base + 0x00, 0xE28F4010);
    bus.write32(base + 0x04, 0xE28F0005);
    bus.write32(base + 0x08, 0xE12FFF10);
    bus.write16(base + 0x10, 0x2014);
    bus.write16(base + 0x12, 0x3016);
    bus.write16(base + 0x14, 0x4720);
    bus.write32(base + 0x18, 0xE3A01001);
    bus.write32(base + 0x1C, 0xEAFFFFFE);
    cpu.setReg(15, base);

    cpu.step();
    cpu.step();
    cpu.step();
    CHECK(cpu.inThumbState(), "T bit set after BX with bit0=1");
    cpu.step();
    cpu.step();
    cpu.step();
    CHECK(!cpu.inThumbState(), "T bit cleared after BX with bit0=0");
    cpu.step();
    cpu.step();

    CHECK(cpu.reg(0) == 42, "Thumb additions: r0 == 42 (got %u)", cpu.reg(0));
    CHECK(cpu.reg(1) == 1, "ARM resumed: r1 == 1 (got %u)", cpu.reg(1));
}

void testThumbMemory(Bus& bus) {
    std::printf("Test: Thumb load/store and ALU\n");
    ARM7TDMI cpu(bus);
    cpu.reset();

    const uint32_t base = 0x02000600;
    bus.write16(base + 0x0, 0x2055);
    bus.write16(base + 0x2, 0x4902);
    bus.write16(base + 0x4, 0x6008);
    bus.write16(base + 0x6, 0x680A);
    bus.write16(base + 0x8, 0x1880);
    bus.write16(base + 0xA, 0xE7FE);
    bus.write32(base + 0xC, 0x02000700);

    cpu.setCPSR(cpu.getCPSR() | ARM7TDMI::BIT_T);
    cpu.setReg(15, base);
    for (int i = 0; i < 5; ++i) {
        cpu.step();
    }

    CHECK(cpu.reg(2) == 0x55, "LDR round trip: r2 == 0x55 (got 0x%X)",
          cpu.reg(2));
    CHECK(cpu.reg(0) == 0xAA, "ADD reg: r0 == 0xAA (got 0x%X)", cpu.reg(0));
    CHECK(bus.read32(0x02000700) == 0x55,
          "STR hit memory (got 0x%X)", bus.read32(0x02000700));
}

void testDMAImmediate(Bus& bus) {
    std::printf("Test: immediate DMA transfer\n");
    DMA dma(bus);
    bus.attachDMA(&dma);

    const uint32_t src = 0x02000800;
    const uint32_t dst = 0x02000900;
    for (uint32_t i = 0; i < 4; ++i) {
        bus.write32(src + i * 4, 0xCAFE0000 + i);
    }
    bus.write32(0x040000D4, src);
    bus.write32(0x040000D8, dst);
    bus.write16(0x040000DC, 4);
    bus.write16(0x040000DE, 0x8400);

    bool match = true;
    for (uint32_t i = 0; i < 4; ++i) {
        match = match && bus.read32(dst + i * 4) == 0xCAFE0000 + i;
    }
    CHECK(match, "4 words copied 0x%08X -> 0x%08X", src, dst);
    CHECK(!(bus.read16(0x040000DE) & 0x8000),
          "enable bit cleared after one-shot (ctrl=0x%04X)",
          bus.read16(0x040000DE));
}

void testInterruptFlags(Bus& bus) {
    std::printf("Test: interrupt registers\n");
    PPU ppu(bus);

    bus.write16(0x04000004, 1u << 3);
    bus.write16(0x04000200, 0x0001);
    bus.write16(0x04000208, 0x0001);

    ppu.step(PPU::CYCLES_SCANLINE * (PPU::LINES_VISIBLE + 1));

    CHECK(bus.read16(0x04000202) & 1, "VBlank flag raised in IF (IF=0x%04X)",
          bus.read16(0x04000202));

    bus.write16(0x04000202, 0x0001);
    CHECK((bus.read16(0x04000202) & 1) == 0,
          "IF write-1-to-clear acknowledged (IF=0x%04X)",
          bus.read16(0x04000202));
}

void testMode0AndSprites(Bus& bus) {
    std::printf("Test: mode 0 tiles and sprites\n");
    PPU ppu(bus);

    bus.write16(0x04000000, 0x1140);
    bus.write16(0x04000008, 0x0800);

    bus.write16(0x05000000, 0x7C00);
    bus.write16(0x05000002, 0x001F);
    bus.write16(0x05000204, 0x03E0);

    for (uint32_t i = 0; i < 32; ++i) {
        bus.write8(0x06000000 + 32 + i, 0x11);
    }
    bus.write16(0x06004000, 0x0001);

    for (uint32_t i = 0; i < 32; ++i) {
        bus.write8(0x06010000 + 2 * 32 + i, 0x22);
    }
    bus.write16(0x07000000, 0x0000);
    bus.write16(0x07000002, 0x0004);
    bus.write16(0x07000004, 0x0002);

    ppu.step(PPU::CYCLES_SCANLINE * PPU::LINES_TOTAL);
    ppu.frameReady();

    const auto& fb = ppu.framebuffer();
    CHECK(fb[0] == 0xFF0000FF, "(0,0) BG tile red (got 0x%08X)", fb[0]);
    CHECK(fb[4] == 0x00FF00FF, "(4,0) sprite green over BG (got 0x%08X)",
          fb[4]);
    CHECK(fb[16] == 0x0000FFFF, "(16,0) backdrop blue (got 0x%08X)", fb[16]);
    CHECK(fb[20 * 240] == 0x0000FFFF, "(0,20) backdrop blue (got 0x%08X)",
          fb[20 * 240]);
}

void testSRAMPersistence(Bus& bus) {
    std::printf("Test: SRAM persistence\n");

    bus.write8(0x0E000000, 0xAB);
    bus.write8(0x0E000123, 0xCD);
    CHECK(bus.sramDirty(), "SRAM marked dirty after write");
    CHECK(bus.read8(0x0E000123) == 0xCD, "SRAM readback (got 0x%02X)",
          bus.read8(0x0E000123));

    const std::string savPath = "/tmp/gba_emu_test.sav";
    CHECK(bus.saveCartridgeData(savPath), "saved to %s", savPath.c_str());

    Bus fresh;
    CHECK(fresh.loadCartridgeData(savPath), "loaded into fresh Bus");
    CHECK(fresh.read8(0x0E000000) == 0xAB && fresh.read8(0x0E000123) == 0xCD,
          "data survived round trip (0x%02X, 0x%02X)",
          fresh.read8(0x0E000000), fresh.read8(0x0E000123));
    std::remove(savPath.c_str());
}

void testARMSingleDataTransfer(Bus& bus) {
    std::printf("Test: ARM LDR/STR\n");
    ARM7TDMI cpu(bus);
    cpu.reset();

    const uint32_t base = 0x02000A00;
    const uint32_t data = 0x02000B00;

    cpu.setReg(0, data);
    cpu.setReg(1, 0x11223344);
    bus.write32(base + 0x00, 0xE5A01004);
    bus.write32(base + 0x04, 0xE4902008);
    bus.write32(base + 0x08, 0xE5D03000);
    bus.write32(base + 0x0C, 0xEAFFFFFE);
    bus.write8(data + 12, 0x5A);
    cpu.setReg(15, base);

    cpu.step();
    CHECK(bus.read32(data + 4) == 0x11223344,
          "STR pre-indexed stored (got 0x%08X)", bus.read32(data + 4));
    CHECK(cpu.reg(0) == data + 4, "writeback: r0 == base+4 (got 0x%08X)",
          cpu.reg(0));

    cpu.step();
    CHECK(cpu.reg(2) == 0x11223344, "LDR post-indexed value (got 0x%08X)",
          cpu.reg(2));
    CHECK(cpu.reg(0) == data + 12, "post-index: r0 == base+12 (got 0x%08X)",
          cpu.reg(0));

    cpu.step();
    CHECK(cpu.reg(3) == 0x5A, "LDRB byte (got 0x%02X)", cpu.reg(3));
}

void testARMBlockDataTransfer(Bus& bus) {
    std::printf("Test: ARM LDM/STM\n");
    ARM7TDMI cpu(bus);
    cpu.reset();

    const uint32_t base = 0x02000C00;
    const uint32_t sp = 0x03007F00;
    cpu.setReg(0, 0xAAAA0000);
    cpu.setReg(1, 0xBBBB1111);
    cpu.setReg(2, 0xCCCC2222);
    cpu.setReg(13, sp);

    bus.write32(base + 0x00, 0xE92D0007);
    bus.write32(base + 0x04, 0xE3A00000);
    bus.write32(base + 0x08, 0xE3A01000);
    bus.write32(base + 0x0C, 0xE3A02000);
    bus.write32(base + 0x10, 0xE8BD0007);
    bus.write32(base + 0x14, 0xEAFFFFFE);
    cpu.setReg(15, base);

    cpu.step();
    CHECK(cpu.reg(13) == sp - 12, "push write-back: sp -= 12 (got 0x%08X)",
          cpu.reg(13));
    CHECK(bus.read32(sp - 12) == 0xAAAA0000 &&
              bus.read32(sp - 8) == 0xBBBB1111 &&
              bus.read32(sp - 4) == 0xCCCC2222,
          "registers stored ascending from lowest address");

    cpu.step();
    cpu.step();
    cpu.step();
    CHECK(cpu.reg(0) == 0 && cpu.reg(1) == 0 && cpu.reg(2) == 0,
          "registers cleared");

    cpu.step();
    CHECK(cpu.reg(0) == 0xAAAA0000 && cpu.reg(1) == 0xBBBB1111 &&
              cpu.reg(2) == 0xCCCC2222,
          "pop restored r0-r2 (got 0x%08X 0x%08X 0x%08X)", cpu.reg(0),
          cpu.reg(1), cpu.reg(2));
    CHECK(cpu.reg(13) == sp, "pop write-back: sp restored (got 0x%08X)",
          cpu.reg(13));
}

void testHLEInterruptDispatch(Bus& bus) {
    std::printf("Test: HLE BIOS IRQ dispatch\n");
    ARM7TDMI cpu(bus);
    PPU ppu(bus);
    cpu.reset();

    const uint32_t handler = 0x03000100;
    bus.write32(handler + 0x00, 0xE3A04001);
    bus.write32(handler + 0x04, 0xE3A00301);
    bus.write32(handler + 0x08, 0xE3A01001);
    bus.write32(handler + 0x0C, 0xE5C01202);
    bus.write32(handler + 0x10, 0xE12FFF1E);
    bus.write32(0x03007FFC, handler);

    bus.write16(0x04000004, 1u << 3);
    bus.write16(0x04000200, 0x0001);
    bus.write16(0x04000208, 0x0001);

    const uint32_t spin = 0x02000D00;
    bus.write32(spin, 0xEAFFFFFE);
    cpu.setReg(15, spin);

    bool reachedHandler = false;
    for (int i = 0; i < 80000; ++i) {
        cpu.step();
        ppu.step(4);
        const uint32_t executing = cpu.reg(15) - 8;
        if (executing >= handler && executing <= handler + 0x14) {
            reachedHandler = true;
        }
    }

    CHECK(reachedHandler, "CPU jumped to the handler from 0x03007FFC");
    CHECK(cpu.reg(4) == 1, "handler executed: r4 == 1 (got %u)", cpu.reg(4));
    CHECK(cpu.currentMode() == ARM7TDMI::Mode::System,
          "returned to System mode (got 0x%02X)",
          static_cast<uint32_t>(cpu.currentMode()));
    CHECK(cpu.reg(15) == spin + 8, "resumed at the spin loop (PC=0x%08X)",
          cpu.reg(15));
}

void testARMHalfwordTransfer(Bus& bus) {
    std::printf("Test: ARM halfword/signed transfers\n");
    ARM7TDMI cpu(bus);
    cpu.reset();

    const uint32_t base = 0x02000E00;
    const uint32_t data = 0x02000F00;
    cpu.setReg(0, data);
    cpu.setReg(1, 0x1234BEEF);
    bus.write32(base + 0x00, 0xE1C010B0);
    bus.write32(base + 0x04, 0xE1D020B0);
    bus.write32(base + 0x08, 0xE1D030D1);
    bus.write32(base + 0x0C, 0xE1D040F0);
    bus.write32(base + 0x10, 0xEAFFFFFE);
    cpu.setReg(15, base);
    for (int i = 0; i < 4; ++i) {
        cpu.step();
    }

    CHECK(bus.read16(data) == 0xBEEF && bus.read16(data + 2) == 0,
          "STRH stored only 16 bits (got 0x%04X, 0x%04X)", bus.read16(data),
          bus.read16(data + 2));
    CHECK(cpu.reg(2) == 0xBEEF, "LDRH zero-extended (got 0x%08X)",
          cpu.reg(2));
    CHECK(cpu.reg(3) == 0xFFFFFFBE, "LDRSB sign-extended (got 0x%08X)",
          cpu.reg(3));
    CHECK(cpu.reg(4) == 0xFFFFBEEF, "LDRSH sign-extended (got 0x%08X)",
          cpu.reg(4));
}

void testARMMultiply(Bus& bus) {
    std::printf("Test: ARM multiply\n");
    ARM7TDMI cpu(bus);
    cpu.reset();

    const uint32_t base = 0x02001100;
    cpu.setReg(0, 7);
    cpu.setReg(1, 6);
    cpu.setReg(8, 0x80000000);
    cpu.setReg(9, 4);
    cpu.setReg(10, 0xFFFFFFFE);
    cpu.setReg(11, 3);
    bus.write32(base + 0x00, 0xE0020190);
    bus.write32(base + 0x04, 0xE0232190);
    bus.write32(base + 0x08, 0xE0854998);
    bus.write32(base + 0x0C, 0xE0D76B9A);
    bus.write32(base + 0x10, 0xEAFFFFFE);
    cpu.setReg(15, base);
    for (int i = 0; i < 4; ++i) {
        cpu.step();
    }

    CHECK(cpu.reg(2) == 42, "MUL: 7*6 == 42 (got %u)", cpu.reg(2));
    CHECK(cpu.reg(3) == 84, "MLA: 7*6+42 == 84 (got %u)", cpu.reg(3));
    CHECK(cpu.reg(4) == 0 && cpu.reg(5) == 2,
          "UMULL: 0x80000000*4 == 2:0 (got %u:%u)", cpu.reg(5), cpu.reg(4));
    CHECK(cpu.reg(6) == 0xFFFFFFFA && cpu.reg(7) == 0xFFFFFFFF,
          "SMULL: -2*3 == -6 (got 0x%08X:0x%08X)", cpu.reg(7), cpu.reg(6));
    CHECK(cpu.getCPSR() & ARM7TDMI::FLAG_N,
          "SMULLS set N for negative result (CPSR=0x%08X)", cpu.getCPSR());
}

void testPSRTransfer(Bus& bus) {
    std::printf("Test: MRS/MSR\n");
    ARM7TDMI cpu(bus);
    cpu.reset();

    const uint32_t base = 0x02001200;
    bus.write32(base + 0x00, 0xE3A0020F);
    bus.write32(base + 0x04, 0xE128F000);
    bus.write32(base + 0x08, 0xE10F1000);
    bus.write32(base + 0x0C, 0xE3A0203F);
    bus.write32(base + 0x10, 0xE121F002);
    bus.write32(base + 0x14, 0xE10F3000);
    bus.write32(base + 0x18, 0xEAFFFFFE);
    cpu.setReg(15, base);
    for (int i = 0; i < 6; ++i) {
        cpu.step();
    }

    CHECK((cpu.reg(1) & 0xF0000000) == 0xF0000000,
          "MSR flags field landed in CPSR (MRS got 0x%08X)", cpu.reg(1));
    CHECK((cpu.reg(3) & 0x20) == 0,
          "T bit rejected by MSR control write (got 0x%08X)", cpu.reg(3));
    CHECK((cpu.reg(3) & 0x1F) == 0x1F,
          "mode bits written through control field (got 0x%02X)",
          cpu.reg(3) & 0x1F);
    CHECK(!cpu.inThumbState(), "CPU still executing ARM after MSR");
}

void testTimers(Bus& bus) {
    std::printf("Test: hardware timers\n");
    Timers timers(bus);
    bus.attachTimers(&timers);

    bus.write16(0x04000100, 0xFFF0);
    bus.write16(0x04000102, 0x00C1);
    CHECK(bus.read16(0x04000100) == 0xFFF0,
          "counter loaded from reload on enable (got 0x%04X)",
          bus.read16(0x04000100));

    timers.step(64);
    CHECK(bus.read16(0x04000100) == 0xFFF1,
          "prescaler 64: one tick after 64 cycles (got 0x%04X)",
          bus.read16(0x04000100));

    bus.write16(0x04000104, 0x0000);
    bus.write16(0x04000106, 0x0084);

    timers.step(15 * 64);
    CHECK(bus.read16(0x04000100) == 0xFFF0,
          "timer 0 reloaded on overflow (got 0x%04X)",
          bus.read16(0x04000100));
    CHECK(bus.read16(0x04000104) == 1,
          "timer 1 cascaded on timer 0 overflow (got %u)",
          bus.read16(0x04000104));
    CHECK(bus.read16(0x04000202) & (1u << 3),
          "timer 0 IRQ raised in IF (IF=0x%04X)", bus.read16(0x04000202));
}

void testAPUSquare(Bus& bus) {
    std::printf("Test: APU square channel\n");
    APU apu(bus);
    bus.attachAPU(&apu);

    bus.write16(0x04000084, 0x0080);
    bus.write16(0x04000080, 0xFF77);
    bus.write16(0x04000082, 0x0002);
    bus.write16(0x04000062, 0xF080);
    bus.write16(0x04000064, 0x8400);

    CHECK(bus.read16(0x04000084) & 1,
          "square 1 active flag in SOUNDCNT_X (got 0x%04X)",
          bus.read16(0x04000084));

    apu.step(APU::CYCLES_PER_SAMPLE * 64);
    CHECK(apu.pendingFrames() >= 64, "64 frames generated (got %zu)",
          apu.pendingFrames());
    int16_t frames[128];
    apu.drainSamples(frames, 64);
    bool nonzero = false;
    for (int i = 0; i < 128; ++i) {
        nonzero = nonzero || frames[i] != 0;
    }
    CHECK(nonzero, "square wave produced non-silent samples");

    bus.write16(0x04000062, 0xF0BF);
    bus.write16(0x04000064, 0xC400);
    apu.step(131072);
    CHECK((bus.read16(0x04000084) & 1) == 0,
          "length expiry cleared the active flag (got 0x%04X)",
          bus.read16(0x04000084));
}

void testAPUFifoDMA(Bus& bus) {
    std::printf("Test: APU Direct Sound FIFO + DMA refill\n");
    APU apu(bus);
    bus.attachAPU(&apu);
    DMA dma(bus);
    bus.attachDMA(&dma);
    Timers timers(bus);
    bus.attachTimers(&timers);

    bus.write16(0x04000084, 0x0080);
    bus.write16(0x04000082, 0x0304);

    for (int i = 0; i < 8; ++i) {
        bus.write32(0x040000A0, 0x40404040);
    }
    CHECK(apu.fifoCount(0) == 32, "FIFO A filled by CPU writes (count=%d)",
          apu.fifoCount(0));

    const uint32_t src = 0x02001300;
    for (uint32_t i = 0; i < 16; ++i) {
        bus.write8(src + i, 0x55);
    }
    bus.write32(0x040000BC, src);
    bus.write32(0x040000C0, 0x040000A0);
    bus.write16(0x040000C6, 0xB640);

    bus.write16(0x04000100, 0xFFFF);
    bus.write16(0x04000102, 0x0080);
    timers.step(17);
    CHECK(apu.fifoCount(0) == 31,
          "DMA1 refilled FIFO A at half-empty (count=%d)", apu.fifoCount(0));

    apu.step(APU::CYCLES_PER_SAMPLE);
    int16_t frame[2] = {0, 0};
    apu.drainSamples(frame, 1);
    CHECK(frame[0] != 0 && frame[1] != 0,
          "FIFO sample reached both output channels (got %d, %d)", frame[0],
          frame[1]);
}

}

int main() {
    {
        Bus bus;
        testDataProcessingLoop(bus);
    }
    {
        Bus bus;
        testBranchWithLink(bus);
    }
    {
        Bus bus;
        testConditionCodes(bus);
    }
    {
        Bus bus;
        testPPUMode3(bus);
    }
    {
        Bus bus;
        testThumbSwitch(bus);
    }
    {
        Bus bus;
        testThumbMemory(bus);
    }
    {
        Bus bus;
        testDMAImmediate(bus);
    }
    {
        Bus bus;
        testInterruptFlags(bus);
    }
    {
        Bus bus;
        testMode0AndSprites(bus);
    }
    {
        Bus bus;
        testSRAMPersistence(bus);
    }
    {
        Bus bus;
        testARMSingleDataTransfer(bus);
    }
    {
        Bus bus;
        testARMBlockDataTransfer(bus);
    }
    {
        Bus bus;
        testHLEInterruptDispatch(bus);
    }
    {
        Bus bus;
        testARMHalfwordTransfer(bus);
    }
    {
        Bus bus;
        testARMMultiply(bus);
    }
    {
        Bus bus;
        testPSRTransfer(bus);
    }
    {
        Bus bus;
        testTimers(bus);
    }
    {
        Bus bus;
        testAPUSquare(bus);
    }
    {
        Bus bus;
        testAPUFifoDMA(bus);
    }

    if (failures == 0) {
        std::printf("All tests passed.\n");
        return 0;
    }
    std::printf("%d check(s) FAILED.\n", failures);
    return 1;
}
