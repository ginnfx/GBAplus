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

void testPPUMode4(Bus& bus) {
    std::printf("Test: PPU mode 4 rendering\n");
    PPU ppu(bus);

    bus.write16(0x05000000, 0x7C00);
    bus.write16(0x05000002, 0x001F);
    bus.write16(0x05000004, 0x03E0);

    bus.write8(0x06000000, 1);
    bus.write8(0x0600A000, 2);

    bus.write16(0x04000000, 0x0404);
    ppu.step(PPU::CYCLES_SCANLINE * PPU::LINES_TOTAL);
    ppu.frameReady();
    const auto& fb = ppu.framebuffer();
    CHECK(fb[0] == 0xFF0000FF, "page 0: (0,0) red (got 0x%08X)", fb[0]);
    CHECK(fb[1] == 0x0000FFFF, "index 0 -> backdrop blue (got 0x%08X)",
          fb[1]);

    bus.write16(0x04000000, 0x0414);
    ppu.step(PPU::CYCLES_SCANLINE * PPU::LINES_TOTAL);
    ppu.frameReady();
    CHECK(fb[0] == 0x00FF00FF, "page 1: (0,0) green (got 0x%08X)", fb[0]);
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

void testAffineBackground(Bus& bus) {
    std::printf("Test: affine backgrounds\n");
    PPU ppu(bus);

    bus.write16(0x04000000, 0x0402);
    bus.write16(0x0400000C, 0x0800);

    bus.write16(0x05000000, 0x7C00);
    bus.write16(0x05000002, 0x001F);
    bus.write16(0x05000004, 0x03E0);

    for (uint32_t i = 0; i < 64; ++i) {
        bus.write8(0x06000000 + 64 + i, 0x01);
        bus.write8(0x06000000 + 128 + i, 0x02);
    }
    bus.write8(0x06004000, 0x01);
    bus.write8(0x06004001, 0x02);

    auto setMatrix = [&bus](uint16_t pa, uint16_t pb, uint16_t pc,
                            uint16_t pd, uint32_t x0, uint32_t y0) {
        bus.write16(0x04000020, pa);
        bus.write16(0x04000022, pb);
        bus.write16(0x04000024, pc);
        bus.write16(0x04000026, pd);
        bus.write32(0x04000028, x0);
        bus.write32(0x0400002C, y0);
    };
    auto frame = [&ppu] {
        ppu.step(PPU::CYCLES_SCANLINE * PPU::LINES_TOTAL);
        ppu.frameReady();
    };
    const auto& fb = ppu.framebuffer();

    setMatrix(0x100, 0, 0, 0x100, 0, 0);
    frame();
    CHECK(fb[0] == 0xFF0000FF, "identity: (0,0) red (got 0x%08X)", fb[0]);
    CHECK(fb[8] == 0x00FF00FF, "identity: (8,0) green (got 0x%08X)", fb[8]);
    CHECK(fb[16] == 0x0000FFFF, "identity: (16,0) backdrop (got 0x%08X)",
          fb[16]);

    setMatrix(0x200, 0, 0, 0x100, 0, 0);
    frame();
    CHECK(fb[3] == 0xFF0000FF, "scale: (3,0) tx=6 red (got 0x%08X)", fb[3]);
    CHECK(fb[4] == 0x00FF00FF, "scale: (4,0) tx=8 green (got 0x%08X)",
          fb[4]);
    CHECK(fb[8] == 0x0000FFFF, "scale: (8,0) tx=16 backdrop (got 0x%08X)",
          fb[8]);

    setMatrix(0, 0xFF00, 0x100, 0, 7u << 8, 0);
    frame();
    CHECK(fb[0] == 0xFF0000FF, "rotate: (0,0) red (got 0x%08X)", fb[0]);
    CHECK(fb[8] == 0x0000FFFF, "rotate: (8,0) ty=8 backdrop (got 0x%08X)",
          fb[8]);
    CHECK(fb[8 * 240] == 0x0000FFFF,
          "rotate: (0,8) tx=-1 backdrop (got 0x%08X)", fb[8 * 240]);

    setMatrix(0x100, 0, 0, 0x100, 128u << 8, 0);
    frame();
    CHECK(fb[0] == 0x0000FFFF,
          "overflow clear: (0,0) transparent (got 0x%08X)", fb[0]);
    bus.write16(0x0400000C, 0x2800);
    frame();
    CHECK(fb[0] == 0xFF0000FF, "overflow wrap: (0,0) red (got 0x%08X)",
          fb[0]);
    CHECK(fb[8] == 0x00FF00FF, "overflow wrap: (8,0) green (got 0x%08X)",
          fb[8]);

    bus.write16(0x0400000C, 0x0800);
    setMatrix(0x100, 0, 0, 0x100, 0, 0);
    ppu.step(PPU::CYCLES_SCANLINE);
    bus.write32(0x04000028, 64u << 8);
    ppu.step(PPU::CYCLES_SCANLINE * (PPU::LINES_TOTAL - 1));
    ppu.frameReady();
    CHECK(fb[240] == 0xFF0000FF,
          "mid-frame BG2X write: line 1 unmoved (got 0x%08X)", fb[240]);
    CHECK(fb[5 * 240] == 0xFF0000FF,
          "mid-frame BG2X write: line 5 unmoved (got 0x%08X)",
          fb[5 * 240]);
    frame();
    CHECK(fb[0] == 0x0000FFFF,
          "next frame picks up the new BG2X (got 0x%08X)", fb[0]);

    bus.write16(0x04000000, 0x0802);
    bus.write16(0x0400000E, 0x0800);
    bus.write16(0x04000030, 0x100);
    bus.write16(0x04000032, 0);
    bus.write16(0x04000034, 0);
    bus.write16(0x04000036, 0x100);
    bus.write32(0x04000038, 0);
    bus.write32(0x0400003C, 0);
    frame();
    CHECK(fb[0] == 0xFF0000FF, "mode 2 BG3: (0,0) red (got 0x%08X)", fb[0]);

    bus.write16(0x04000000, 0x0C01);
    bus.write16(0x0400000C, 0x0801);
    bus.write32(0x04000028, 0);
    bus.write32(0x04000038, 8u << 8);
    frame();
    CHECK(fb[0] == 0xFF0000FF, "mode 1: BG3 ignored, BG2 red (got 0x%08X)",
          fb[0]);
}

void testAffineSprites(Bus& bus) {
    std::printf("Test: affine sprites\n");
    PPU ppu(bus);

    bus.write16(0x04000000, 0x1040);
    bus.write16(0x05000000, 0x7C00);
    bus.write16(0x05000202, 0x001F);
    bus.write16(0x05000204, 0x03E0);

    for (uint32_t r = 0; r < 8; ++r) {
        bus.write8(0x06010000 + 2 * 32 + r * 4, 0x21);
        bus.write8(0x06010000 + 2 * 32 + r * 4 + 1, 0x22);
        bus.write8(0x06010000 + 2 * 32 + r * 4 + 2, 0x22);
        bus.write8(0x06010000 + 2 * 32 + r * 4 + 3, 0x22);
    }

    bus.write16(0x07000000, 0x0000);
    bus.write16(0x07000002, 0x0000);
    bus.write16(0x07000004, 0x0002);

    auto frame = [&ppu] {
        ppu.step(PPU::CYCLES_SCANLINE * PPU::LINES_TOTAL);
        ppu.frameReady();
    };
    const auto& fb = ppu.framebuffer();

    frame();
    uint32_t plain[8][10];
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 10; ++c) {
            plain[r][c] = fb[r * 240 + c];
        }
    }
    CHECK(plain[0][0] == 0xFF0000FF && plain[0][1] == 0x00FF00FF,
          "regular sprite drawn (got 0x%08X, 0x%08X)", plain[0][0],
          plain[0][1]);

    bus.write16(0x07000000, 0x0100);
    bus.write16(0x07000006, 0x0100);
    bus.write16(0x0700000E, 0x0000);
    bus.write16(0x07000016, 0x0000);
    bus.write16(0x0700001E, 0x0100);
    frame();
    int mismatches = 0;
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 10; ++c) {
            if (fb[r * 240 + c] != plain[r][c]) {
                ++mismatches;
            }
        }
    }
    CHECK(mismatches == 0,
          "identity affine matches regular twin (%d mismatches)",
          mismatches);

    bus.write16(0x07000006, 0x0000);
    bus.write16(0x0700000E, 0x0100);
    bus.write16(0x07000016, 0xFF00);
    bus.write16(0x0700001E, 0x0000);
    frame();
    CHECK(fb[1] == 0xFF0000FF, "rotate: (1,0) red (got 0x%08X)", fb[1]);
    CHECK(fb[7] == 0xFF0000FF, "rotate: (7,0) red (got 0x%08X)", fb[7]);
    CHECK(fb[240 + 1] == 0x00FF00FF, "rotate: (1,1) green (got 0x%08X)",
          fb[240 + 1]);
    CHECK(fb[0] == 0x0000FFFF,
          "rotate: (0,0) off the texture edge (got 0x%08X)", fb[0]);

    bus.write16(0x07000000, 0x0300);
    bus.write16(0x07000006, 0x0080);
    bus.write16(0x0700000E, 0x0000);
    bus.write16(0x07000016, 0x0000);
    bus.write16(0x0700001E, 0x0080);
    frame();
    CHECK(fb[0] == 0xFF0000FF && fb[1] == 0xFF0000FF,
          "double-size 2x: red column doubled at x 0-1 (got 0x%08X, 0x%08X)",
          fb[0], fb[1]);
    CHECK(fb[15] == 0x00FF00FF, "double-size 2x: (15,0) green (got 0x%08X)",
          fb[15]);
    CHECK(fb[16] == 0x0000FFFF, "double-size 2x: (16,0) backdrop (got "
          "0x%08X)", fb[16]);

    bus.write16(0x07000000, 0x0100);
    frame();
    CHECK(fb[0] == 0x00FF00FF,
          "no double-size: edge clipped, (0,0) green (got 0x%08X)", fb[0]);

    bus.write16(0x07000000, 0x0200);
    frame();
    CHECK(fb[0] == 0x0000FFFF && fb[240 + 1] == 0x0000FFFF,
          "bit 9 alone disables the sprite (got 0x%08X, 0x%08X)", fb[0],
          fb[240 + 1]);
}

void testWindows(Bus& bus) {
    std::printf("Test: windows\n");
    PPU ppu(bus);

    bus.write16(0x04000008, 0x0800);
    bus.write16(0x05000000, 0x7C00);
    bus.write16(0x05000002, 0x001F);
    bus.write16(0x05000204, 0x03E0);

    for (uint32_t i = 0; i < 32; ++i) {
        bus.write8(0x06000000 + 32 + i, 0x11);
    }
    for (uint32_t i = 0; i < 1024; ++i) {
        bus.write16(0x06004000 + i * 2, 0x0001);
    }

    auto frame = [&ppu] {
        ppu.step(PPU::CYCLES_SCANLINE * PPU::LINES_TOTAL);
        ppu.frameReady();
    };
    const auto& fb = ppu.framebuffer();
    auto at = [&fb](int x, int y) { return fb[y * 240 + x]; };
    const uint32_t RED = 0xFF0000FF, BLUE = 0x0000FFFF, GREEN = 0x00FF00FF;

    bus.write16(0x04000040, 0x0810);
    bus.write16(0x04000044, 0x040C);
    bus.write16(0x04000048, 0x0001);
    bus.write16(0x0400004A, 0x0000);
    bus.write16(0x04000000, 0x2100);
    frame();
    CHECK(at(10, 6) == RED, "WIN0 inside shows BG0 (got 0x%08X)", at(10, 6));
    CHECK(at(15, 11) == RED, "WIN0 inclusive corner (got 0x%08X)", at(15, 11));
    CHECK(at(16, 6) == BLUE, "WIN0 X2 exclusive (got 0x%08X)", at(16, 6));
    CHECK(at(10, 12) == BLUE, "WIN0 Y2 exclusive (got 0x%08X)", at(10, 12));
    CHECK(at(7, 6) == BLUE, "left of WIN0 -> backdrop (got 0x%08X)", at(7, 6));
    CHECK(at(10, 0) == BLUE, "above WIN0 -> backdrop (got 0x%08X)", at(10, 0));

    bus.write16(0x04000048, 0x0000);
    bus.write16(0x0400004A, 0x0001);
    frame();
    CHECK(at(10, 6) == BLUE, "inverted: inside now backdrop (got 0x%08X)",
          at(10, 6));
    CHECK(at(0, 0) == RED, "inverted: outside now BG0 (got 0x%08X)", at(0, 0));

    bus.write16(0x04000040, 0x0818);
    bus.write16(0x04000044, 0x0414);
    bus.write16(0x04000042, 0x0010);
    bus.write16(0x04000046, 0x000C);
    bus.write16(0x04000048, 0x0001);
    bus.write16(0x0400004A, 0x0000);
    bus.write16(0x04000000, 0x6100);
    frame();
    CHECK(at(10, 6) == RED, "overlap follows WIN0 (got 0x%08X)", at(10, 6));
    CHECK(at(2, 2) == BLUE, "WIN1-only disables BG0 (got 0x%08X)", at(2, 2));
    CHECK(at(20, 18) == RED, "WIN0-only enables BG0 (got 0x%08X)", at(20, 18));

    for (uint32_t i = 0; i < 32; ++i) {
        bus.write8(0x06010000 + 2 * 32 + i, 0x22);
    }
    bus.write16(0x07000000, 0x0004);
    bus.write16(0x07000002, 0x0004);
    bus.write16(0x07000004, 0x0002);
    bus.write16(0x04000040, 0x08F0);
    bus.write16(0x04000044, 0x00A0);
    bus.write16(0x04000048, 0x0011);
    bus.write16(0x0400004A, 0x0001);
    bus.write16(0x04000000, 0x3100);
    frame();
    CHECK(at(8, 5) == GREEN, "sprite visible inside WIN0 (got 0x%08X)",
          at(8, 5));
    CHECK(at(7, 5) == RED, "sprite hidden outside WIN0, BG0 shows (got "
          "0x%08X)", at(7, 5));
}

void testObjWindow(Bus& bus) {
    std::printf("Test: OBJ window\n");
    PPU ppu(bus);

    bus.write16(0x04000008, 0x0800);
    bus.write16(0x05000000, 0x7C00);
    bus.write16(0x05000002, 0x001F);
    bus.write16(0x05000204, 0x03E0);

    for (uint32_t i = 0; i < 32; ++i) {
        bus.write8(0x06000000 + 32 + i, 0x11);
        bus.write8(0x06010000 + 2 * 32 + i, 0x22);
    }
    for (uint32_t i = 0; i < 1024; ++i) {
        bus.write16(0x06004000 + i * 2, 0x0001);
    }

    bus.write16(0x07000000, 0x0808);
    bus.write16(0x07000002, 0x0008);
    bus.write16(0x07000004, 0x0002);

    bus.write16(0x04000048, 0x0000);
    bus.write16(0x0400004A, 0x0100);
    bus.write16(0x04000000, 0x9140);

    ppu.step(PPU::CYCLES_SCANLINE * PPU::LINES_TOTAL);
    ppu.frameReady();
    const auto& fb = ppu.framebuffer();
    auto at = [&fb](int x, int y) { return fb[y * 240 + x]; };
    const uint32_t RED = 0xFF0000FF, BLUE = 0x0000FFFF;

    CHECK(at(10, 10) == RED,
          "inside OBJ window: BG0 draws, sprite invisible (got 0x%08X)",
          at(10, 10));
    CHECK(at(16, 10) == BLUE,
          "just outside the sprite box -> backdrop (got 0x%08X)", at(16, 10));
    CHECK(at(50, 50) == BLUE,
          "outside OBJ window -> backdrop (got 0x%08X)", at(50, 50));

    bus.write16(0x04000000, 0x1140);
    ppu.step(PPU::CYCLES_SCANLINE * PPU::LINES_TOTAL);
    ppu.frameReady();
    CHECK(at(10, 10) == RED,
          "OBJ-window sprite never draws as a visible sprite (got 0x%08X)",
          at(10, 10));
    CHECK(at(50, 50) == RED, "no window: BG0 everywhere (got 0x%08X)",
          at(50, 50));
}

void testBlending(Bus& bus) {
    std::printf("Test: blending\n");
    PPU ppu(bus);

    bus.write16(0x04000008, 0x0800);
    bus.write16(0x0400000A, 0x0901);
    bus.write16(0x05000000, 0x0000);
    bus.write16(0x05000002, 0x001F);
    bus.write16(0x05000004, 0x7C00);
    bus.write16(0x05000206, 0x7C00);

    for (uint32_t i = 0; i < 32; ++i) {
        bus.write8(0x06000000 + 32 + i, 0x11);
        bus.write8(0x06000000 + 64 + i, 0x22);
        bus.write8(0x06010000 + 2 * 32 + i, 0x33);
    }
    for (uint32_t i = 0; i < 1024; ++i) {
        bus.write16(0x06004000 + i * 2, 0x0001);
        bus.write16(0x06004800 + i * 2, 0x0002);
    }

    auto frame = [&ppu] {
        ppu.step(PPU::CYCLES_SCANLINE * PPU::LINES_TOTAL);
        ppu.frameReady();
    };
    const auto& fb = ppu.framebuffer();

    bus.write16(0x04000050, 0x0241);
    bus.write16(0x04000052, 0x0808);
    bus.write16(0x04000000, 0x0300);
    frame();
    CHECK(fb[0] == 0x7B007BFF, "alpha (red*0.5 + blue*0.5) (got 0x%08X)",
          fb[0]);

    bus.write16(0x04000050, 0x0081);
    bus.write16(0x04000054, 0x0008);
    bus.write16(0x04000000, 0x0100);
    frame();
    CHECK(fb[0] == 0xFF7B7BFF, "brighten red toward white (got 0x%08X)",
          fb[0]);

    bus.write16(0x04000050, 0x00C1);
    frame();
    CHECK(fb[0] == 0x840000FF, "darken red toward black (got 0x%08X)", fb[0]);

    bus.write16(0x04000050, 0x0081);
    bus.write16(0x04000040, 0x0078);
    bus.write16(0x04000044, 0x00A0);
    bus.write16(0x04000048, 0x0001);
    bus.write16(0x0400004A, 0x0021);
    bus.write16(0x04000000, 0x2100);
    frame();
    CHECK(fb[10] == 0xFF0000FF, "inside WIN0: effects off, raw red (got "
          "0x%08X)", fb[10]);
    CHECK(fb[200] == 0xFF7B7BFF, "outside WIN0: effects on, brightened (got "
          "0x%08X)", fb[200]);

    bus.write16(0x04000040, 0x0000);
    bus.write16(0x04000044, 0x0000);
    bus.write16(0x07000000, 0x0400);
    bus.write16(0x07000002, 0x0000);
    bus.write16(0x07000004, 0x0002);
    bus.write16(0x04000050, 0x0100);
    bus.write16(0x04000052, 0x0808);
    bus.write16(0x0400000A, 0x0801);
    bus.write16(0x04000000, 0x1100);
    frame();
    CHECK(fb[0] == 0x7B007BFF,
          "semi-transparent sprite alpha over BG0 (got 0x%08X)", fb[0]);
}

void testMosaic(Bus& bus) {
    std::printf("Test: mosaic\n");
    PPU ppu(bus);

    bus.write16(0x05000000, 0x0000);
    bus.write16(0x05000002, 0x001F);
    bus.write16(0x05000004, 0x03E0);
    bus.write16(0x05000006, 0x7C00);
    bus.write16(0x05000202, 0x001F);
    bus.write16(0x05000204, 0x03E0);
    bus.write16(0x05000206, 0x7C00);

    for (uint32_t r = 0; r < 8; ++r) {
        bus.write8(0x06000000 + 32 + r * 4 + 0, 0x21);
        bus.write8(0x06000000 + 32 + r * 4 + 1, 0x13);
        bus.write8(0x06000000 + 32 + r * 4 + 2, 0x32);
        bus.write8(0x06000000 + 32 + r * 4 + 3, 0x21);
        bus.write8(0x06010000 + 32 + r * 4 + 0, 0x21);
        bus.write8(0x06010000 + 32 + r * 4 + 1, 0x13);
        bus.write8(0x06010000 + 32 + r * 4 + 2, 0x32);
        bus.write8(0x06010000 + 32 + r * 4 + 3, 0x21);
    }
    for (uint32_t r = 0; r < 8; ++r) {
        const uint8_t c = static_cast<uint8_t>((r % 3) + 1);
        const uint8_t b = static_cast<uint8_t>((c << 4) | c);
        for (uint32_t k = 0; k < 4; ++k) {
            bus.write8(0x06000000 + 64 + r * 4 + k, b);
        }
    }
    bus.write16(0x04000008, 0x0800);
    for (uint32_t i = 0; i < 1024; ++i) {
        bus.write16(0x06004000 + i * 2, 0x0001);
    }

    auto frame = [&ppu] {
        ppu.step(PPU::CYCLES_SCANLINE * PPU::LINES_TOTAL);
        ppu.frameReady();
    };
    const auto& fb = ppu.framebuffer();
    auto at = [&fb](int x, int y) { return fb[y * 240 + x]; };
    const uint32_t RED = 0xFF0000FF, GREEN = 0x00FF00FF, BLUE = 0x0000FFFF;

    bus.write16(0x0400004C, 0x0000);
    bus.write16(0x04000000, 0x0100);
    frame();
    CHECK(at(0, 0) == RED && at(1, 0) == GREEN && at(2, 0) == BLUE,
          "no mosaic: distinct columns (got 0x%08X 0x%08X 0x%08X)",
          at(0, 0), at(1, 0), at(2, 0));

    bus.write16(0x0400004C, 0x0001);
    bus.write16(0x04000008, 0x0840);
    frame();
    CHECK(at(0, 0) == RED && at(1, 0) == RED,
          "BG H mosaic: pixels 0,1 both red (got 0x%08X 0x%08X)", at(0, 0),
          at(1, 0));
    CHECK(at(2, 0) == BLUE && at(3, 0) == BLUE,
          "BG H mosaic: pixels 2,3 both blue (got 0x%08X 0x%08X)", at(2, 0),
          at(3, 0));

    bus.write16(0x04000008, 0x0800);
    frame();
    CHECK(at(1, 0) == GREEN, "BGxCNT gate: bit clear disables mosaic (got "
          "0x%08X)", at(1, 0));

    for (uint32_t i = 0; i < 1024; ++i) {
        bus.write16(0x06004000 + i * 2, 0x0002);
    }
    bus.write16(0x0400004C, 0x0010);
    bus.write16(0x04000008, 0x0840);
    frame();
    CHECK(at(0, 0) == RED && at(0, 1) == RED,
          "BG V mosaic: lines 0,1 both row-0 red (got 0x%08X 0x%08X)",
          at(0, 0), at(0, 1));
    CHECK(at(0, 2) == BLUE,
          "BG V mosaic: line 2 samples row 2 blue (got 0x%08X)", at(0, 2));

    bus.write16(0x07000000, 0x1000);
    bus.write16(0x07000002, 0x0005);
    bus.write16(0x07000004, 0x0001);
    bus.write16(0x0400004C, 0x0100);
    bus.write16(0x04000000, 0x1040);
    frame();
    CHECK(at(5, 0) == RED && at(6, 0) == RED,
          "OBJ mosaic: screen 5,6 same sprite block, red (got 0x%08X "
          "0x%08X)", at(5, 0), at(6, 0));
    CHECK(at(7, 0) == BLUE,
          "OBJ mosaic: screen 7 is sprite column 2, blue (got 0x%08X)",
          at(7, 0));
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

void testFlashBackup(Bus& bus) {
    std::printf("Test: Flash backup\n");

    const std::string romPath = "/tmp/gba_emu_flash_test.gba";
    {
        std::FILE* f = std::fopen(romPath.c_str(), "wb");
        const char pad[16] = {};
        std::fwrite(pad, 1, sizeof(pad), f);
        std::fwrite("FLASH1M_V103", 1, 12, f);
        std::fclose(f);
    }
    const bool loaded = bus.loadROM(romPath);
    std::remove(romPath.c_str());
    CHECK(loaded && bus.backupType() == Bus::BackupType::Flash128,
          "FLASH1M_V detected as 128K flash");

    auto command = [&bus](uint8_t cmd) {
        bus.write8(0x0E005555, 0xAA);
        bus.write8(0x0E002AAA, 0x55);
        bus.write8(0x0E005555, cmd);
    };

    command(0x90);
    CHECK(bus.read8(0x0E000000) == 0xC2 && bus.read8(0x0E000001) == 0x09,
          "Macronix 128K chip ID (got 0x%02X 0x%02X)",
          bus.read8(0x0E000000), bus.read8(0x0E000001));
    command(0xF0);

    command(0xA0);
    bus.write8(0x0E001234, 0x5A);
    CHECK(bus.read8(0x0E001234) == 0x5A, "programmed byte (got 0x%02X)",
          bus.read8(0x0E001234));

    command(0xB0);
    bus.write8(0x0E000000, 1);
    command(0xA0);
    bus.write8(0x0E001234, 0x77);
    CHECK(bus.read8(0x0E001234) == 0x77, "bank 1 byte (got 0x%02X)",
          bus.read8(0x0E001234));
    command(0xB0);
    bus.write8(0x0E000000, 0);
    CHECK(bus.read8(0x0E001234) == 0x5A,
          "banks hold distinct data (bank 0 got 0x%02X)",
          bus.read8(0x0E001234));

    command(0x80);
    bus.write8(0x0E005555, 0xAA);
    bus.write8(0x0E002AAA, 0x55);
    bus.write8(0x0E001000, 0x30);
    CHECK(bus.read8(0x0E001234) == 0xFF, "sector erased (got 0x%02X)",
          bus.read8(0x0E001234));
}

void testEEPROMBackup(Bus& bus) {
    std::printf("Test: EEPROM backup\n");

    const std::string romPath = "/tmp/gba_emu_eeprom_test.gba";
    {
        std::FILE* f = std::fopen(romPath.c_str(), "wb");
        const char pad[16] = {};
        std::fwrite(pad, 1, sizeof(pad), f);
        std::fwrite("EEPROM_V124", 1, 11, f);
        std::fclose(f);
    }

    auto sendBits = [](Bus& b, uint64_t value, int count) {
        for (int i = count - 1; i >= 0; --i) {
            b.write16(0x0D000000, static_cast<uint16_t>((value >> i) & 1));
        }
    };
    auto readBlock = [](Bus& b) {
        for (int i = 0; i < 4; ++i) {
            b.read16(0x0D000000);
        }
        uint64_t v = 0;
        for (int i = 0; i < 64; ++i) {
            v = (v << 1) | (b.read16(0x0D000000) & 1);
        }
        return v;
    };

    CHECK(bus.loadROM(romPath) &&
              bus.backupType() == Bus::BackupType::EEPROM,
          "EEPROM_V detected");

    sendBits(bus, 0b10, 2);
    sendBits(bus, 5, 6);
    sendBits(bus, 0xA1B2C3D4E5F60718, 64);
    sendBits(bus, 0, 1);
    CHECK(bus.read16(0x0D000000) & 1, "ready after write");

    sendBits(bus, 0b11, 2);
    sendBits(bus, 5, 6);
    sendBits(bus, 0, 1);
    const uint64_t value = readBlock(bus);
    CHECK(value == 0xA1B2C3D4E5F60718,
          "6-bit round trip (got 0x%016llX)",
          static_cast<unsigned long long>(value));

    sendBits(bus, 0b11, 2);
    sendBits(bus, 9, 6);
    sendBits(bus, 0, 1);
    CHECK(readBlock(bus) == 0xFFFFFFFFFFFFFFFF, "untouched block is 0xFF");

    Bus big;
    big.loadROM(romPath);
    std::remove(romPath.c_str());
    sendBits(big, 0b10, 2);
    sendBits(big, 700, 14);
    sendBits(big, 0x123456789ABCDEF0, 64);
    sendBits(big, 0, 1);
    big.read16(0x0D000000);
    sendBits(big, 0b11, 2);
    sendBits(big, 700, 14);
    sendBits(big, 0, 1);
    const uint64_t bigValue = readBlock(big);
    CHECK(bigValue == 0x123456789ABCDEF0,
          "14-bit round trip (got 0x%016llX)",
          static_cast<unsigned long long>(bigValue));
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

void testSWIArithmetic(Bus& bus) {
    std::printf("Test: SWI arithmetic (Div/Sqrt/ArcTan2)\n");
    ARM7TDMI cpu(bus);
    cpu.reset();

    const uint32_t base = 0x02001400;
    bus.write32(base + 0x00, 0xEF060000);
    bus.write32(base + 0x04, 0xEF080000);
    bus.write32(base + 0x08, 0xEF0A0000);
    bus.write32(base + 0x0C, 0xEAFFFFFE);
    cpu.setReg(15, base);

    cpu.setReg(0, static_cast<uint32_t>(-7));
    cpu.setReg(1, 2);
    cpu.step();
    CHECK(cpu.reg(0) == static_cast<uint32_t>(-3) &&
              cpu.reg(1) == static_cast<uint32_t>(-1) && cpu.reg(3) == 3,
          "Div -7/2: q=-3 r=-1 abs=3 (got %d %d %u)",
          static_cast<int32_t>(cpu.reg(0)), static_cast<int32_t>(cpu.reg(1)),
          cpu.reg(3));

    cpu.setReg(0, 0x00010000);
    cpu.step();
    CHECK(cpu.reg(0) == 0x100, "Sqrt 0x10000 == 0x100 (got 0x%X)",
          cpu.reg(0));

    cpu.setReg(0, 0);
    cpu.setReg(1, 0x1000);
    cpu.step();
    CHECK(cpu.reg(0) == 0x4000, "ArcTan2(0, +y) == 0x4000 (got 0x%04X)",
          cpu.reg(0));
}

void testSWICpuSet(Bus& bus) {
    std::printf("Test: SWI CpuSet/CpuFastSet\n");
    ARM7TDMI cpu(bus);
    cpu.reset();

    const uint32_t base = 0x02001500;
    bus.write32(base + 0x00, 0xEF0B0000);
    bus.write32(base + 0x04, 0xEF0B0000);
    bus.write32(base + 0x08, 0xEF0C0000);
    bus.write32(base + 0x0C, 0xEAFFFFFE);
    cpu.setReg(15, base);

    const uint32_t src = 0x02002000;
    for (uint32_t i = 0; i < 4; ++i) {
        bus.write32(src + i * 4, 0xCAFE0000 + i);
    }
    cpu.setReg(0, src);
    cpu.setReg(1, 0x02002100);
    cpu.setReg(2, 4 | (1u << 26));
    cpu.step();
    bool match = true;
    for (uint32_t i = 0; i < 4; ++i) {
        match = match && bus.read32(0x02002100 + i * 4) == 0xCAFE0000 + i;
    }
    CHECK(match, "CpuSet copied 4 words");

    bus.write16(0x02002200, 0xBEEF);
    cpu.setReg(0, 0x02002200);
    cpu.setReg(1, 0x02002280);
    cpu.setReg(2, 8 | (1u << 24));
    cpu.step();
    CHECK(bus.read16(0x02002280) == 0xBEEF &&
              bus.read16(0x0200228E) == 0xBEEF,
          "CpuSet halfword fill (got 0x%04X, 0x%04X)",
          bus.read16(0x02002280), bus.read16(0x0200228E));

    bus.write32(0x02002300, 0xDEADBEEF);
    cpu.setReg(0, 0x02002300);
    cpu.setReg(1, 0x02002380);
    cpu.setReg(2, 8 | (1u << 24));
    cpu.step();
    CHECK(bus.read32(0x02002380) == 0xDEADBEEF &&
              bus.read32(0x0200239C) == 0xDEADBEEF,
          "CpuFastSet word fill (got 0x%08X)", bus.read32(0x0200239C));
}

void testSWIDecompression(Bus& bus) {
    std::printf("Test: SWI decompression\n");
    ARM7TDMI cpu(bus);
    cpu.reset();

    const uint32_t base = 0x02001600;
    bus.write32(base + 0x00, 0xEF110000);
    bus.write32(base + 0x04, 0xEF140000);
    bus.write32(base + 0x08, 0xEF100000);
    bus.write32(base + 0x0C, 0xEF130000);
    bus.write32(base + 0x10, 0xEAFFFFFE);
    cpu.setReg(15, base);

    const uint32_t lzSrc = 0x02002400;
    bus.write32(lzSrc, (8u << 8) | 0x10);
    const uint8_t lzStream[] = {0x08, 'A', 'B', 'C', 'D', 0x10, 0x03};
    for (uint32_t i = 0; i < sizeof(lzStream); ++i) {
        bus.write8(lzSrc + 4 + i, lzStream[i]);
    }
    cpu.setReg(0, lzSrc);
    cpu.setReg(1, 0x02002500);
    cpu.step();
    CHECK(bus.read32(0x02002500) == 0x44434241 &&
              bus.read32(0x02002504) == 0x44434241,
          "LZ77 -> ABCDABCD (got 0x%08X 0x%08X)", bus.read32(0x02002500),
          bus.read32(0x02002504));

    const uint32_t rlSrc = 0x02002600;
    bus.write32(rlSrc, (7u << 8) | 0x30);
    const uint8_t rlStream[] = {0x82, 0x77, 0x01, 0x11, 0x22};
    for (uint32_t i = 0; i < sizeof(rlStream); ++i) {
        bus.write8(rlSrc + 4 + i, rlStream[i]);
    }
    cpu.setReg(0, rlSrc);
    cpu.setReg(1, 0x02002700);
    cpu.step();
    CHECK(bus.read32(0x02002700) == 0x77777777 &&
              bus.read8(0x02002704) == 0x77 &&
              bus.read8(0x02002705) == 0x11 &&
              bus.read8(0x02002706) == 0x22,
          "RL run + literals (got 0x%08X ...%02X %02X %02X)",
          bus.read32(0x02002700), bus.read8(0x02002704),
          bus.read8(0x02002705), bus.read8(0x02002706));

    const uint32_t buSrc = 0x02002800;
    const uint8_t packed[] = {0x21, 0x43, 0x65, 0x87};
    for (uint32_t i = 0; i < 4; ++i) {
        bus.write8(buSrc + i, packed[i]);
    }
    const uint32_t info = 0x02002810;
    bus.write16(info, 4);
    bus.write8(info + 2, 4);
    bus.write8(info + 3, 8);
    bus.write32(info + 4, 0x10);
    cpu.setReg(0, buSrc);
    cpu.setReg(1, 0x02002900);
    cpu.setReg(2, info);
    cpu.step();
    CHECK(bus.read32(0x02002900) == 0x14131211 &&
              bus.read32(0x02002904) == 0x18171615,
          "BitUnPack 4->8 bit +0x10 (got 0x%08X 0x%08X)",
          bus.read32(0x02002900), bus.read32(0x02002904));

    const uint32_t huffSrc = 0x02002A00;
    bus.write32(huffSrc, (4u << 8) | 0x28);
    bus.write8(huffSrc + 4, 1);
    bus.write8(huffSrc + 5, 0xC0);
    bus.write8(huffSrc + 6, 'A');
    bus.write8(huffSrc + 7, 'B');
    bus.write32(huffSrc + 8, 0x50000000);
    cpu.setReg(0, huffSrc);
    cpu.setReg(1, 0x02002B00);
    cpu.step();
    CHECK(bus.read32(0x02002B00) == 0x42414241,
          "Huffman -> ABAB (got 0x%08X)", bus.read32(0x02002B00));
}

void testSWIIntrWait(Bus& bus) {
    std::printf("Test: SWI VBlankIntrWait\n");
    ARM7TDMI cpu(bus);
    PPU ppu(bus);
    cpu.reset();

    const uint32_t handler = 0x03000200;
    bus.write32(handler + 0x00, 0xE3A00301);
    bus.write32(handler + 0x04, 0xE3A01001);
    bus.write32(handler + 0x08, 0xE5C01202);
    bus.write32(handler + 0x0C, 0xE3A02403);
    bus.write32(handler + 0x10, 0xE3822C7F);
    bus.write32(handler + 0x14, 0xE38220F8);
    bus.write32(handler + 0x18, 0xE1D230B0);
    bus.write32(handler + 0x1C, 0xE3833001);
    bus.write32(handler + 0x20, 0xE1C230B0);
    bus.write32(handler + 0x24, 0xE12FFF1E);
    bus.write32(0x03007FFC, handler);

    bus.write16(0x04000004, 1u << 3);
    bus.write16(0x04000200, 0x0001);

    const uint32_t prog = 0x02001700;
    bus.write32(prog + 0x00, 0xEF050000);
    bus.write32(prog + 0x04, 0xE3A07001);
    bus.write32(prog + 0x08, 0xEAFFFFFE);
    cpu.setReg(7, 0);
    cpu.setReg(15, prog);

    for (int i = 0; i < 10000; ++i) {
        cpu.step();
        ppu.step(4);
    }
    CHECK(cpu.isHalted() && cpu.reg(7) == 0,
          "CPU asleep mid-frame (halted=%d, r7=%u)", cpu.isHalted(),
          cpu.reg(7));

    for (int i = 0; i < 70000; ++i) {
        cpu.step();
        ppu.step(4);
    }
    CHECK(cpu.reg(7) == 1, "resumed after VBlank: r7 == 1 (got %u)",
          cpu.reg(7));
    CHECK(cpu.reg(15) == prog + 8 + 8, "parked after resume (PC=0x%08X)",
          cpu.reg(15));
    CHECK((bus.read16(0x03007FF8) & 1) == 0,
          "IF-shadow bit consumed by IntrWait (shadow=0x%04X)",
          bus.read16(0x03007FF8));
}

void testSwap(Bus& bus) {
    std::printf("Test: SWP/SWPB\n");
    ARM7TDMI cpu(bus);
    cpu.reset();

    const uint32_t base = 0x02001800;
    const uint32_t addr = 0x02003000;
    bus.write32(addr, 0x11223344);
    bus.write32(base + 0x00, 0xE1002091);
    bus.write32(base + 0x04, 0xE1403094);
    bus.write32(base + 0x08, 0xEAFFFFFE);
    cpu.setReg(0, addr);
    cpu.setReg(1, 0xAABBCCDD);
    cpu.setReg(4, 0xEE);
    cpu.setReg(15, base);

    cpu.step();
    CHECK(cpu.reg(2) == 0x11223344 && bus.read32(addr) == 0xAABBCCDD,
          "SWP swapped word (r2=0x%08X mem=0x%08X)", cpu.reg(2),
          bus.read32(addr));

    cpu.step();
    CHECK(cpu.reg(3) == 0xDD && bus.read8(addr) == 0xEE,
          "SWPB swapped byte (r3=0x%02X mem=0x%02X)", cpu.reg(3),
          bus.read8(addr));
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
        testPPUMode4(bus);
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
        testAffineBackground(bus);
    }
    {
        Bus bus;
        testAffineSprites(bus);
    }
    {
        Bus bus;
        testWindows(bus);
    }
    {
        Bus bus;
        testObjWindow(bus);
    }
    {
        Bus bus;
        testBlending(bus);
    }
    {
        Bus bus;
        testMosaic(bus);
    }
    {
        Bus bus;
        testSRAMPersistence(bus);
    }
    {
        Bus bus;
        testFlashBackup(bus);
    }
    {
        Bus bus;
        testEEPROMBackup(bus);
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
        testSWIArithmetic(bus);
    }
    {
        Bus bus;
        testSWICpuSet(bus);
    }
    {
        Bus bus;
        testSWIDecompression(bus);
    }
    {
        Bus bus;
        testSWIIntrWait(bus);
    }
    {
        Bus bus;
        testSwap(bus);
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
