// Unit test harness (gba_test_harness target). Each test gets a fresh Bus
// so IO/VRAM state cannot leak across tests.

#include <cstdio>
#include <string>

#include <cstdint>
#include <vector>

#include "APU.hpp"
#include "ARM7TDMI.hpp"
#include "Bus.hpp"
#include "DMA.hpp"
#include "Emulator.hpp"
#include "PPU.hpp"
#include "SaveState.hpp"
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

// MOV/ADD/CMP/BNE counting loop: increments r0 until it reaches 5.
void testDataProcessingLoop(Bus& bus) {
    std::printf("Test: data processing loop\n");
    ARM7TDMI cpu(bus);
    cpu.reset();

    const uint32_t base = 0x02000100;
    bus.write32(base + 0,  0xE3A00000);  //       MOV r0, #0
    bus.write32(base + 4,  0xE2800001);  // loop: ADD r0, r0, #1
    bus.write32(base + 8,  0xE3500005);  //       CMP r0, #5
    bus.write32(base + 12, 0x1AFFFFFC);  //       BNE loop
    bus.write32(base + 16, 0xEAFFFFFE);  //       B .  (park)
    cpu.setReg(15, base);

    // 1 MOV + 5 iterations of (ADD, CMP, BNE) = 16 instructions.
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

// BL must stash the return address (next instruction) in r14.
void testBranchWithLink(Bus& bus) {
    std::printf("Test: branch with link\n");
    ARM7TDMI cpu(bus);
    cpu.reset();

    const uint32_t base = 0x02000200;
    bus.write32(base + 0,  0xEB000001);  // BL base+12
    bus.write32(base + 12, 0xEAFFFFFE);  // B .
    cpu.setReg(15, base);
    cpu.step();

    CHECK(cpu.reg(14) == base + 4, "r14 == 0x%08X (got 0x%08X)", base + 4,
          cpu.reg(14));
    CHECK(cpu.reg(15) == base + 12 + 8, "branched to 0x%08X (got PC 0x%08X)",
          base + 12, cpu.reg(15));
}

// Condition codes must suppress execution entirely when they fail.
void testConditionCodes(Bus& bus) {
    std::printf("Test: condition codes\n");
    ARM7TDMI cpu(bus);
    cpu.reset();

    const uint32_t base = 0x02000300;
    bus.write32(base + 0,  0xE3A00007);  // MOV   r0, #7
    bus.write32(base + 4,  0xE3500007);  // CMP   r0, #7    -> Z=1
    bus.write32(base + 8,  0x13A00063);  // MOVNE r0, #99   (skipped)
    bus.write32(base + 12, 0x03A0102A);  // MOVEQ r1, #42   (taken)
    bus.write32(base + 16, 0xEAFFFFFE);  // B .
    cpu.setReg(15, base);
    for (int i = 0; i < 4; ++i) {
        cpu.step();
    }

    CHECK(cpu.reg(0) == 7, "MOVNE skipped, r0 == 7 (got %u)", cpu.reg(0));
    CHECK(cpu.reg(1) == 42, "MOVEQ taken, r1 == 42 (got %u)", cpu.reg(1));
}

// Mode 3 rendering: VRAM pixels must land in the framebuffer as RGBA.
void testPPUMode3(Bus& bus) {
    std::printf("Test: PPU mode 3 rendering\n");
    PPU ppu(bus);

    bus.write16(0x04000000, 0x0403);  // DISPCNT: mode 3, BG2 on

    bus.write16(0x06000000, 0x001F);  // (0,0)   pure red
    bus.write16(0x06000002, 0x03E0);  // (1,0)   pure green
    bus.write16(0x06000004, 0x7C00);  // (2,0)   pure blue
    // (120,80) center pixel: white
    bus.write16(0x06000000 + (80 * 240 + 120) * 2, 0x7FFF);

    // Run one full frame (228 scanlines).
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

// Mode 4: paletted bitmap, page flipping via DISPCNT bit 4, index 0
// transparent (backdrop shows through).
void testPPUMode4(Bus& bus) {
    std::printf("Test: PPU mode 4 rendering\n");
    PPU ppu(bus);

    bus.write16(0x05000000, 0x7C00);  // palette 0: blue (backdrop)
    bus.write16(0x05000002, 0x001F);  // palette 1: red
    bus.write16(0x05000004, 0x03E0);  // palette 2: green

    bus.write8(0x06000000, 1);  // page 0, (0,0): red
    bus.write8(0x0600A000, 2);  // page 1, (0,0): green
    // (1,0) stays index 0 on both pages -> backdrop.

    bus.write16(0x04000000, 0x0404);  // DISPCNT: mode 4, BG2, page 0
    ppu.step(PPU::CYCLES_SCANLINE * PPU::LINES_TOTAL);
    ppu.frameReady();
    const auto& fb = ppu.framebuffer();
    CHECK(fb[0] == 0xFF0000FF, "page 0: (0,0) red (got 0x%08X)", fb[0]);
    CHECK(fb[1] == 0x0000FFFF, "index 0 -> backdrop blue (got 0x%08X)",
          fb[1]);

    bus.write16(0x04000000, 0x0414);  // flip to page 1
    ppu.step(PPU::CYCLES_SCANLINE * PPU::LINES_TOTAL);
    ppu.frameReady();
    CHECK(fb[0] == 0x00FF00FF, "page 1: (0,0) green (got 0x%08X)", fb[0]);
}

// ARM -> BX into Thumb -> additions -> BX back into ARM.
void testThumbSwitch(Bus& bus) {
    std::printf("Test: ARM/Thumb state switching\n");
    ARM7TDMI cpu(bus);
    cpu.reset();

    const uint32_t base = 0x02000400;
    bus.write32(base + 0x00, 0xE28F4010);  // ADD r4, pc, #16  -> 0x418 (ARM)
    bus.write32(base + 0x04, 0xE28F0005);  // ADD r0, pc, #5   -> 0x411 (Thumb)
    bus.write32(base + 0x08, 0xE12FFF10);  // BX r0
    // Thumb block:
    bus.write16(base + 0x10, 0x2014);      // MOV r0, #20
    bus.write16(base + 0x12, 0x3016);      // ADD r0, #22  -> 42
    bus.write16(base + 0x14, 0x4720);      // BX r4
    // Back in ARM:
    bus.write32(base + 0x18, 0xE3A01001);  // MOV r1, #1
    bus.write32(base + 0x1C, 0xEAFFFFFE);  // B .
    cpu.setReg(15, base);

    cpu.step();  // ADD r4
    cpu.step();  // ADD r0
    cpu.step();  // BX r0
    CHECK(cpu.inThumbState(), "T bit set after BX with bit0=1");
    cpu.step();  // MOV r0, #20
    cpu.step();  // ADD r0, #22
    cpu.step();  // BX r4
    CHECK(!cpu.inThumbState(), "T bit cleared after BX with bit0=0");
    cpu.step();  // MOV r1, #1 (ARM again)
    cpu.step();  // B .

    CHECK(cpu.reg(0) == 42, "Thumb additions: r0 == 42 (got %u)", cpu.reg(0));
    CHECK(cpu.reg(1) == 1, "ARM resumed: r1 == 1 (got %u)", cpu.reg(1));
}

// Thumb load/store and register ALU formats.
void testThumbMemory(Bus& bus) {
    std::printf("Test: Thumb load/store and ALU\n");
    ARM7TDMI cpu(bus);
    cpu.reset();

    const uint32_t base = 0x02000600;
    bus.write16(base + 0x0, 0x2055);  // MOV r0, #0x55
    bus.write16(base + 0x2, 0x4902);  // LDR r1, [pc, #8]  -> literal at 0x60C
    bus.write16(base + 0x4, 0x6008);  // STR r0, [r1]
    bus.write16(base + 0x6, 0x680A);  // LDR r2, [r1]
    bus.write16(base + 0x8, 0x1880);  // ADD r0, r0, r2    -> 0xAA
    bus.write16(base + 0xA, 0xE7FE);  // B .
    bus.write32(base + 0xC, 0x02000700);  // literal pool

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

// A Thumb LDR from a non-word-aligned address must rotate the aligned word
// right by the byte offset, exactly like ARM. FireRed's palette-fade loop
// loads 16-bit colours with `ldr` from halfword-aligned addresses and reads
// the low halfword; without the rotation, odd entries read the wrong colour
// (every faded colour duplicated, corrupting 4bpp images).
void testThumbUnalignedLoad(Bus& bus) {
    std::printf("Test: Thumb unaligned word load rotation\n");
    ARM7TDMI cpu(bus);
    cpu.reset();

    bus.write32(0x02000700, 0xAABBCCDD);  // word to be read unaligned

    const uint32_t base = 0x02000600;
    bus.write16(base + 0x0, 0x4902);      // LDR r1, [pc, #8]  -> 0x02000702
    bus.write16(base + 0x2, 0x680A);      // LDR r2, [r1]      (addr & 2)
    bus.write16(base + 0x4, 0xE7FE);      // B .
    bus.write32(base + 0xC, 0x02000702);  // literal: halfword-aligned address

    cpu.setCPSR(cpu.getCPSR() | ARM7TDMI::BIT_T);
    cpu.setReg(15, base);
    for (int i = 0; i < 3; ++i) {
        cpu.step();
    }

    // rotr(0xAABBCCDD, 16) == 0xCCDDAABB; the low halfword is 0xAABB, the
    // actual halfword stored at 0x02000702.
    CHECK(cpu.reg(2) == 0xCCDDAABB,
          "unaligned LDR rotated (got 0x%08X, want 0xCCDDAABB)", cpu.reg(2));
    CHECK((cpu.reg(2) & 0xFFFF) == 0xAABB,
          "low halfword is the one at 0x702 (got 0x%04X)",
          cpu.reg(2) & 0xFFFF);
}

// Save-state serialization round-trips Bus memory and CPU registers exactly.
void testSaveStateComponents() {
    std::printf("Test: save-state component round-trip\n");
    Bus busA;
    ARM7TDMI cpuA(busA);
    cpuA.reset();
    busA.write32(0x02000100, 0xDEADBEEF);  // EWRAM
    busA.write16(0x03000200, 0x1234);      // IWRAM
    busA.write16(0x05000040, 0x7FFF);      // palette
    busA.write16(0x06001000, 0xABCD);      // VRAM
    busA.write16(0x07000010, 0x55AA);      // OAM
    cpuA.setReg(0, 0xCAFEBABE);
    cpuA.setReg(13, 0x03007F00);

    std::vector<uint8_t> buf;
    Serializer s(buf);
    busA.serialize(s);
    cpuA.serialize(s);

    Bus busB;
    ARM7TDMI cpuB(busB);
    Deserializer d(buf.data(), buf.size());
    busB.deserialize(d);
    cpuB.deserialize(d);

    CHECK(d.ok(), "deserializer consumed the buffer cleanly");
    CHECK(busB.read32(0x02000100) == 0xDEADBEEF, "EWRAM restored (0x%08X)",
          busB.read32(0x02000100));
    CHECK(busB.read16(0x03000200) == 0x1234, "IWRAM restored (0x%04X)",
          busB.read16(0x03000200));
    CHECK(busB.read16(0x05000040) == 0x7FFF, "palette restored (0x%04X)",
          busB.read16(0x05000040));
    CHECK(busB.read16(0x06001000) == 0xABCD, "VRAM restored (0x%04X)",
          busB.read16(0x06001000));
    CHECK(busB.read16(0x07000010) == 0x55AA, "OAM restored (0x%04X)",
          busB.read16(0x07000010));
    bool regsMatch = true;
    for (int i = 0; i < 16; ++i) {
        regsMatch = regsMatch && cpuA.reg(i) == cpuB.reg(i);
    }
    CHECK(regsMatch, "all 16 registers restored");
    CHECK(cpuA.getCPSR() == cpuB.getCPSR(), "CPSR restored (0x%08X vs 0x%08X)",
          cpuA.getCPSR(), cpuB.getCPSR());
}

// Emulator save states are idempotent (save->load->save is byte-identical),
// replay deterministically, and reject corrupt/truncated blobs.
void testSaveStateEmulator() {
    std::printf("Test: emulator save-state round-trip\n");
    Emulator emu;
    emu.loadDemo();
    for (int i = 0; i < 8; ++i) emu.runFrame();

    std::vector<uint8_t> a;
    emu.saveState(a);
    CHECK(!a.empty(), "saveState produced bytes (%zu)", a.size());

    CHECK(emu.loadState(a), "loadState accepted its own snapshot");
    std::vector<uint8_t> b;
    emu.saveState(b);
    CHECK(a == b, "save/load/save is byte-identical (%zu vs %zu)", a.size(),
          b.size());

    emu.loadState(a);
    for (int i = 0; i < 5; ++i) emu.runFrame();
    std::vector<uint8_t> c1;
    emu.saveState(c1);
    emu.loadState(a);
    for (int i = 0; i < 5; ++i) emu.runFrame();
    std::vector<uint8_t> c2;
    emu.saveState(c2);
    CHECK(c1 == c2, "restored state replays deterministically");

    std::vector<uint8_t> bad = a;
    bad[0] ^= 0xFF;
    CHECK(!emu.loadState(bad), "corrupt magic rejected");
    std::vector<uint8_t> tooShort(3);
    CHECK(!emu.loadState(tooShort), "truncated blob rejected");
}

// Immediate DMA3 transfer: EWRAM -> EWRAM, 4 words.
void testDMAImmediate(Bus& bus) {
    std::printf("Test: immediate DMA transfer\n");
    DMA dma(bus);
    bus.attachDMA(&dma);

    const uint32_t src = 0x02000800;
    const uint32_t dst = 0x02000900;
    for (uint32_t i = 0; i < 4; ++i) {
        bus.write32(src + i * 4, 0xCAFE0000 + i);
    }
    bus.write32(0x040000D4, src);     // DMA3SAD
    bus.write32(0x040000D8, dst);     // DMA3DAD
    bus.write16(0x040000DC, 4);       // DMA3CNT_L: 4 units
    bus.write16(0x040000DE, 0x8400);  // DMA3CNT_H: enable, 32-bit, immediate

    bool match = true;
    for (uint32_t i = 0; i < 4; ++i) {
        match = match && bus.read32(dst + i * 4) == 0xCAFE0000 + i;
    }
    CHECK(match, "4 words copied 0x%08X -> 0x%08X", src, dst);
    CHECK(!(bus.read16(0x040000DE) & 0x8000),
          "enable bit cleared after one-shot (ctrl=0x%04X)",
          bus.read16(0x040000DE));
}

// VBlank raises IF bit 0 (when enabled in DISPSTAT); IF is write-1-to-clear.
void testInterruptFlags(Bus& bus) {
    std::printf("Test: interrupt registers\n");
    PPU ppu(bus);

    bus.write16(0x04000004, 1u << 3);  // DISPSTAT: VBlank IRQ enable
    bus.write16(0x04000200, 0x0001);   // IE: VBlank
    bus.write16(0x04000208, 0x0001);   // IME: on

    // Run until just past the start of VBlank (161 scanlines).
    ppu.step(PPU::CYCLES_SCANLINE * (PPU::LINES_VISIBLE + 1));

    CHECK(bus.read16(0x04000202) & 1, "VBlank flag raised in IF (IF=0x%04X)",
          bus.read16(0x04000202));

    bus.write16(0x04000202, 0x0001);  // acknowledge
    CHECK((bus.read16(0x04000202) & 1) == 0,
          "IF write-1-to-clear acknowledged (IF=0x%04X)",
          bus.read16(0x04000202));
}

// Mode 0: one red 4bpp tile at (0,0), a green 8x8 sprite at x=4, blue
// backdrop everywhere else.
void testMode0AndSprites(Bus& bus) {
    std::printf("Test: mode 0 tiles and sprites\n");
    PPU ppu(bus);

    bus.write16(0x04000000, 0x1140);  // DISPCNT: mode 0, BG0, OBJ, 1D map
    bus.write16(0x04000008, 0x0800);  // BG0CNT: char base 0, screen base 8

    bus.write16(0x05000000, 0x7C00);  // BG palette 0: blue (backdrop)
    bus.write16(0x05000002, 0x001F);  // BG palette 1: red
    bus.write16(0x05000204, 0x03E0);  // OBJ palette 2: green

    // BG tile 1: all pixels use color index 1 (4bpp -> byte 0x11).
    for (uint32_t i = 0; i < 32; ++i) {
        bus.write8(0x06000000 + 32 + i, 0x11);
    }
    bus.write16(0x06004000, 0x0001);  // map (0,0) -> tile 1

    // Sprite tile 2 (OBJ char block): color index 2 everywhere.
    for (uint32_t i = 0; i < 32; ++i) {
        bus.write8(0x06010000 + 2 * 32 + i, 0x22);
    }
    bus.write16(0x07000000, 0x0000);  // attr0: y=0, 4bpp, square
    bus.write16(0x07000002, 0x0004);  // attr1: x=4, 8x8
    bus.write16(0x07000004, 0x0002);  // attr2: tile 2, priority 0, bank 0

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

// Affine backgrounds (modes 1/2): reference-point latch and per-line
// accumulation, the per-pixel PA/PC walk, one-byte map entries with 8bpp
// tiles, and both overflow behaviors.
void testAffineBackground(Bus& bus) {
    std::printf("Test: affine backgrounds\n");
    PPU ppu(bus);

    bus.write16(0x04000000, 0x0402);  // DISPCNT: mode 2, BG2 on
    bus.write16(0x0400000C, 0x0800);  // BG2CNT: char 0, screen 8, 128px

    bus.write16(0x05000000, 0x7C00);  // palette 0: blue (backdrop)
    bus.write16(0x05000002, 0x001F);  // palette 1: red
    bus.write16(0x05000004, 0x03E0);  // palette 2: green

    // 8bpp tiles: tile 1 all color 1, tile 2 all color 2.
    for (uint32_t i = 0; i < 64; ++i) {
        bus.write8(0x06000000 + 64 + i, 0x01);
        bus.write8(0x06000000 + 128 + i, 0x02);
    }
    // Affine map, one byte per entry, 16 entries per row: texture x 0-7
    // is tile 1 (red), 8-15 is tile 2 (green), everything else tile 0
    // (transparent, backdrop shows).
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

    setMatrix(0x100, 0, 0, 0x100, 0, 0);  // identity
    frame();
    CHECK(fb[0] == 0xFF0000FF, "identity: (0,0) red (got 0x%08X)", fb[0]);
    CHECK(fb[8] == 0x00FF00FF, "identity: (8,0) green (got 0x%08X)", fb[8]);
    CHECK(fb[16] == 0x0000FFFF, "identity: (16,0) backdrop (got 0x%08X)",
          fb[16]);

    setMatrix(0x200, 0, 0, 0x100, 0, 0);  // 2x texture step in x
    frame();
    CHECK(fb[3] == 0xFF0000FF, "scale: (3,0) tx=6 red (got 0x%08X)", fb[3]);
    CHECK(fb[4] == 0x00FF00FF, "scale: (4,0) tx=8 green (got 0x%08X)",
          fb[4]);
    CHECK(fb[8] == 0x0000FFFF, "scale: (8,0) tx=16 backdrop (got 0x%08X)",
          fb[8]);

    // 90-degree rotation: screen (x,y) samples texture (7-y, x).
    setMatrix(0, 0xFF00, 0x100, 0, 7u << 8, 0);
    frame();
    CHECK(fb[0] == 0xFF0000FF, "rotate: (0,0) red (got 0x%08X)", fb[0]);
    CHECK(fb[8] == 0x0000FFFF, "rotate: (8,0) ty=8 backdrop (got 0x%08X)",
          fb[8]);
    CHECK(fb[8 * 240] == 0x0000FFFF,
          "rotate: (0,8) tx=-1 backdrop (got 0x%08X)", fb[8 * 240]);

    // Overflow bit clear: texture x 128 is past the 128px map.
    setMatrix(0x100, 0, 0, 0x100, 128u << 8, 0);
    frame();
    CHECK(fb[0] == 0x0000FFFF,
          "overflow clear: (0,0) transparent (got 0x%08X)", fb[0]);
    bus.write16(0x0400000C, 0x2800);  // overflow bit: wrap
    frame();
    CHECK(fb[0] == 0xFF0000FF, "overflow wrap: (0,0) red (got 0x%08X)",
          fb[0]);
    CHECK(fb[8] == 0x00FF00FF, "overflow wrap: (8,0) green (got 0x%08X)",
          fb[8]);

    // A mid-frame BG2X write must not move this frame's origin (the
    // renderer works from the latched accumulator, not the register) but
    // must land at the next latch.
    bus.write16(0x0400000C, 0x0800);
    setMatrix(0x100, 0, 0, 0x100, 0, 0);
    ppu.step(PPU::CYCLES_SCANLINE);     // line 0 rendered, origin latched
    bus.write32(0x04000028, 64u << 8);  // move the reference point
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

    // BG3 runs on the second accumulator pair.
    bus.write16(0x04000000, 0x0802);  // mode 2, BG3 only
    bus.write16(0x0400000E, 0x0800);  // BG3CNT mirrors BG2CNT
    bus.write16(0x04000030, 0x100);   // BG3 identity matrix
    bus.write16(0x04000032, 0);
    bus.write16(0x04000034, 0);
    bus.write16(0x04000036, 0x100);
    bus.write32(0x04000038, 0);
    bus.write32(0x0400003C, 0);
    frame();
    CHECK(fb[0] == 0xFF0000FF, "mode 2 BG3: (0,0) red (got 0x%08X)", fb[0]);

    // Mode 1 has no BG3: even enabled and at a winning priority it must
    // not render. BG3's origin is moved onto the green tile so a leak
    // would be visible over BG2's red.
    bus.write16(0x04000000, 0x0C01);  // mode 1, BG2+BG3 enabled
    bus.write16(0x0400000C, 0x0801);  // BG2: priority 1
    bus.write32(0x04000028, 0);       // BG2 origin back onto the red tile
    bus.write32(0x04000038, 8u << 8);
    frame();
    CHECK(fb[0] == 0xFF0000FF, "mode 1: BG3 ignored, BG2 red (got 0x%08X)",
          fb[0]);
}

// Affine sprites: OAM parameter groups, the attr0 bit-8/bit-9 decode,
// the double-size bounding box, and the inverse per-pixel transform.
void testAffineSprites(Bus& bus) {
    std::printf("Test: affine sprites\n");
    PPU ppu(bus);

    bus.write16(0x04000000, 0x1040);  // DISPCNT: mode 0, OBJ on, 1D map
    bus.write16(0x05000000, 0x7C00);  // backdrop blue
    bus.write16(0x05000202, 0x001F);  // OBJ palette 1: red
    bus.write16(0x05000204, 0x03E0);  // OBJ palette 2: green

    // OBJ tile 2, 4bpp: texture column 0 is color 1 (red), columns 1-7
    // color 2 (green) — asymmetric so transforms are distinguishable.
    for (uint32_t r = 0; r < 8; ++r) {
        bus.write8(0x06010000 + 2 * 32 + r * 4, 0x21);
        bus.write8(0x06010000 + 2 * 32 + r * 4 + 1, 0x22);
        bus.write8(0x06010000 + 2 * 32 + r * 4 + 2, 0x22);
        bus.write8(0x06010000 + 2 * 32 + r * 4 + 3, 0x22);
    }

    // Sprite 0: regular 8x8 at (0,0), tile 2.
    bus.write16(0x07000000, 0x0000);
    bus.write16(0x07000002, 0x0000);
    bus.write16(0x07000004, 0x0002);

    auto frame = [&ppu] {
        ppu.step(PPU::CYCLES_SCANLINE * PPU::LINES_TOTAL);
        ppu.frameReady();
    };
    const auto& fb = ppu.framebuffer();

    // Snapshot the regular sprite, then re-render it as an affine sprite
    // with an identity matrix: the two must be pixel-identical.
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

    bus.write16(0x07000000, 0x0100);  // affine, group 0
    bus.write16(0x07000006, 0x0100);  // PA = 1.0
    bus.write16(0x0700000E, 0x0000);  // PB
    bus.write16(0x07000016, 0x0000);  // PC
    bus.write16(0x0700001E, 0x0100);  // PD = 1.0
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

    // 90-degree rotation: screen (x,y) samples texture (y, 8-x), so the
    // red texture column lands on screen line 0 and the sample at x=0
    // falls off the texture edge.
    bus.write16(0x07000006, 0x0000);  // PA
    bus.write16(0x0700000E, 0x0100);  // PB
    bus.write16(0x07000016, 0xFF00);  // PC = -1.0
    bus.write16(0x0700001E, 0x0000);  // PD
    frame();
    CHECK(fb[1] == 0xFF0000FF, "rotate: (1,0) red (got 0x%08X)", fb[1]);
    CHECK(fb[7] == 0xFF0000FF, "rotate: (7,0) red (got 0x%08X)", fb[7]);
    CHECK(fb[240 + 1] == 0x00FF00FF, "rotate: (1,1) green (got 0x%08X)",
          fb[240 + 1]);
    CHECK(fb[0] == 0x0000FFFF,
          "rotate: (0,0) off the texture edge (got 0x%08X)", fb[0]);

    // 2x magnification (PA/PD = 0.5) with double-size: the 16x16 box
    // shows the whole texture, red column included, from screen x 0.
    bus.write16(0x07000000, 0x0300);  // affine + double-size
    bus.write16(0x07000006, 0x0080);  // PA = 0.5
    bus.write16(0x0700000E, 0x0000);
    bus.write16(0x07000016, 0x0000);
    bus.write16(0x0700001E, 0x0080);  // PD = 0.5
    frame();
    CHECK(fb[0] == 0xFF0000FF && fb[1] == 0xFF0000FF,
          "double-size 2x: red column doubled at x 0-1 (got 0x%08X, 0x%08X)",
          fb[0], fb[1]);
    CHECK(fb[15] == 0x00FF00FF, "double-size 2x: (15,0) green (got 0x%08X)",
          fb[15]);
    CHECK(fb[16] == 0x0000FFFF, "double-size 2x: (16,0) backdrop (got "
          "0x%08X)", fb[16]);

    // Same scale without double-size: the 8x8 box clips to the texture
    // center, so the red edge column is no longer visible.
    bus.write16(0x07000000, 0x0100);
    frame();
    CHECK(fb[0] == 0x00FF00FF,
          "no double-size: edge clipped, (0,0) green (got 0x%08X)", fb[0]);

    // attr0 bit 9 without bit 8 means disabled, not double-size.
    bus.write16(0x07000000, 0x0200);
    frame();
    CHECK(fb[0] == 0x0000FFFF && fb[240 + 1] == 0x0000FFFF,
          "bit 9 alone disables the sprite (got 0x%08X, 0x%08X)", fb[0],
          fb[240 + 1]);
}

// Windows: the WIN0/WIN1 rectangular bounds, inside-vs-outside layer masks
// from WININ/WINOUT, WIN0's precedence over WIN1, and OBJ gating.
void testWindows(Bus& bus) {
    std::printf("Test: windows\n");
    PPU ppu(bus);

    bus.write16(0x04000008, 0x0800);  // BG0CNT: char base 0, screen base 8
    bus.write16(0x05000000, 0x7C00);  // backdrop blue
    bus.write16(0x05000002, 0x001F);  // BG palette 1: red
    bus.write16(0x05000204, 0x03E0);  // OBJ palette 2: green

    // BG0 tile 1 = color index 1 everywhere; map every entry to it so BG0
    // would cover the whole screen red were it not for the window.
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

    // WIN0 = x[8,16) y[4,12). Inside enables BG0; outside enables nothing.
    bus.write16(0x04000040, 0x0810);  // WIN0H: X1=8, X2=16
    bus.write16(0x04000044, 0x040C);  // WIN0V: Y1=4, Y2=12
    bus.write16(0x04000048, 0x0001);  // WININ: WIN0 -> BG0
    bus.write16(0x0400004A, 0x0000);  // WINOUT: outside -> nothing
    bus.write16(0x04000000, 0x2100);  // mode 0, BG0, WIN0
    frame();
    CHECK(at(10, 6) == RED, "WIN0 inside shows BG0 (got 0x%08X)", at(10, 6));
    CHECK(at(15, 11) == RED, "WIN0 inclusive corner (got 0x%08X)", at(15, 11));
    CHECK(at(16, 6) == BLUE, "WIN0 X2 exclusive (got 0x%08X)", at(16, 6));
    CHECK(at(10, 12) == BLUE, "WIN0 Y2 exclusive (got 0x%08X)", at(10, 12));
    CHECK(at(7, 6) == BLUE, "left of WIN0 -> backdrop (got 0x%08X)", at(7, 6));
    CHECK(at(10, 0) == BLUE, "above WIN0 -> backdrop (got 0x%08X)", at(10, 0));

    // Swap the masks: BG0 disabled inside, enabled outside -> inverted.
    bus.write16(0x04000048, 0x0000);  // WININ: WIN0 -> nothing
    bus.write16(0x0400004A, 0x0001);  // WINOUT: outside -> BG0
    frame();
    CHECK(at(10, 6) == BLUE, "inverted: inside now backdrop (got 0x%08X)",
          at(10, 6));
    CHECK(at(0, 0) == RED, "inverted: outside now BG0 (got 0x%08X)", at(0, 0));

    // WIN0 (x[8,24) y[4,20), BG0 on) over WIN1 (x[0,16) y[0,12), BG0 off):
    // the overlap must follow WIN0, not WIN1.
    bus.write16(0x04000040, 0x0818);  // WIN0H: X1=8, X2=24
    bus.write16(0x04000044, 0x0414);  // WIN0V: Y1=4, Y2=20
    bus.write16(0x04000042, 0x0010);  // WIN1H: X1=0, X2=16
    bus.write16(0x04000046, 0x000C);  // WIN1V: Y1=0, Y2=12
    bus.write16(0x04000048, 0x0001);  // WININ: WIN0->BG0, WIN1->nothing
    bus.write16(0x0400004A, 0x0000);  // WINOUT: outside -> nothing
    bus.write16(0x04000000, 0x6100);  // mode 0, BG0, WIN0, WIN1
    frame();
    CHECK(at(10, 6) == RED, "overlap follows WIN0 (got 0x%08X)", at(10, 6));
    CHECK(at(2, 2) == BLUE, "WIN1-only disables BG0 (got 0x%08X)", at(2, 2));
    CHECK(at(20, 18) == RED, "WIN0-only enables BG0 (got 0x%08X)", at(20, 18));

    // OBJ gating: an 8x8 sprite at x[4,12) straddles WIN0's left edge (X1=8).
    // OBJ enabled inside WIN0 only, so the right half draws, the left hides.
    for (uint32_t i = 0; i < 32; ++i) {
        bus.write8(0x06010000 + 2 * 32 + i, 0x22);  // OBJ tile 2 -> color 2
    }
    bus.write16(0x07000000, 0x0004);  // attr0: y=4, 4bpp, square
    bus.write16(0x07000002, 0x0004);  // attr1: x=4, 8x8
    bus.write16(0x07000004, 0x0002);  // attr2: tile 2, priority 0
    bus.write16(0x04000040, 0x08F0);  // WIN0H: X1=8, X2=240
    bus.write16(0x04000044, 0x00A0);  // WIN0V: Y1=0, Y2=160
    bus.write16(0x04000048, 0x0011);  // WININ: WIN0 -> BG0 + OBJ
    bus.write16(0x0400004A, 0x0001);  // WINOUT: outside -> BG0 only
    bus.write16(0x04000000, 0x3100);  // mode 0, BG0, OBJ, WIN0
    frame();
    CHECK(at(8, 5) == GREEN, "sprite visible inside WIN0 (got 0x%08X)",
          at(8, 5));
    CHECK(at(7, 5) == RED, "sprite hidden outside WIN0, BG0 shows (got "
          "0x%08X)", at(7, 5));
}

// OBJ window: an OBJ-window-mode sprite (attr0 mode 2) marks a region
// rather than drawing; WINOUT's high byte selects which layers draw inside
// that region, its low byte what draws outside.
void testObjWindow(Bus& bus) {
    std::printf("Test: OBJ window\n");
    PPU ppu(bus);

    bus.write16(0x04000008, 0x0800);  // BG0CNT: char base 0, screen base 8
    bus.write16(0x05000000, 0x7C00);  // backdrop blue
    bus.write16(0x05000002, 0x001F);  // BG palette 1: red
    bus.write16(0x05000204, 0x03E0);  // OBJ palette 2: green

    for (uint32_t i = 0; i < 32; ++i) {
        bus.write8(0x06000000 + 32 + i, 0x11);       // BG0 tile 1 -> red
        bus.write8(0x06010000 + 2 * 32 + i, 0x22);   // OBJ tile 2 -> color 2
    }
    for (uint32_t i = 0; i < 1024; ++i) {
        bus.write16(0x06004000 + i * 2, 0x0001);     // BG0 maps to tile 1
    }

    // An 8x8 OBJ-window sprite at (8,8): attr0 mode bits 10-11 = 2.
    bus.write16(0x07000000, 0x0808);  // attr0: y=8, OBJ mode 2 (window)
    bus.write16(0x07000002, 0x0008);  // attr1: x=8, 8x8
    bus.write16(0x07000004, 0x0002);  // attr2: tile 2, priority 0

    // Inside the OBJ window region BG0 draws; outside nothing draws.
    bus.write16(0x04000048, 0x0000);  // WININ unused
    bus.write16(0x0400004A, 0x0100);  // WINOUT: objwin -> BG0, outside none
    bus.write16(0x04000000, 0x9140);  // mode 0, BG0, OBJ, OBJ window, 1D

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

    // With the OBJ window disabled, the mode-2 sprite still never draws and
    // BG0 (no active window) covers the screen.
    bus.write16(0x04000000, 0x1140);  // mode 0, BG0, OBJ, no OBJ window
    ppu.step(PPU::CYCLES_SCANLINE * PPU::LINES_TOTAL);
    ppu.frameReady();
    CHECK(at(10, 10) == RED,
          "OBJ-window sprite never draws as a visible sprite (got 0x%08X)",
          at(10, 10));
    CHECK(at(50, 50) == RED, "no window: BG0 everywhere (got 0x%08X)",
          at(50, 50));
}

// Colour special effects: alpha blend, brightness up/down, the window
// effects-enable gate, and semi-transparent sprites forcing alpha.
void testBlending(Bus& bus) {
    std::printf("Test: blending\n");
    PPU ppu(bus);

    bus.write16(0x04000008, 0x0800);  // BG0CNT: char 0, screen 8, prio 0
    bus.write16(0x0400000A, 0x0901);  // BG1CNT: char 0, screen 9, prio 1
    bus.write16(0x05000000, 0x0000);  // backdrop black
    bus.write16(0x05000002, 0x001F);  // palette 1: red
    bus.write16(0x05000004, 0x7C00);  // palette 2: blue
    bus.write16(0x05000206, 0x7C00);  // OBJ palette 3: blue

    for (uint32_t i = 0; i < 32; ++i) {
        bus.write8(0x06000000 + 32 + i, 0x11);       // BG tile 1 -> color 1
        bus.write8(0x06000000 + 64 + i, 0x22);       // BG tile 2 -> color 2
        bus.write8(0x06010000 + 2 * 32 + i, 0x33);   // OBJ tile 2 -> color 3 (blue)
    }
    for (uint32_t i = 0; i < 1024; ++i) {
        bus.write16(0x06004000 + i * 2, 0x0001);     // BG0 -> tile 1 (red)
        bus.write16(0x06004800 + i * 2, 0x0002);     // BG1 -> tile 2 (blue)
    }

    auto frame = [&ppu] {
        ppu.step(PPU::CYCLES_SCANLINE * PPU::LINES_TOTAL);
        ppu.frameReady();
    };
    const auto& fb = ppu.framebuffer();

    // Alpha: BG0 (red, 1st) over BG1 (blue, 2nd), EVA=EVB=8 -> average.
    bus.write16(0x04000050, 0x0241);  // BLDCNT: alpha, 1st BG0, 2nd BG1
    bus.write16(0x04000052, 0x0808);  // BLDALPHA: EVA=8, EVB=8
    bus.write16(0x04000000, 0x0300);  // mode 0, BG0 + BG1
    frame();
    CHECK(fb[0] == 0x7B007BFF, "alpha (red*0.5 + blue*0.5) (got 0x%08X)",
          fb[0]);

    // Brightness increase: BG0 red toward white by EVY=8 -> pink.
    bus.write16(0x04000050, 0x0081);  // BLDCNT: brighten, 1st BG0
    bus.write16(0x04000054, 0x0008);  // BLDY: EVY=8
    bus.write16(0x04000000, 0x0100);  // mode 0, BG0 only
    frame();
    CHECK(fb[0] == 0xFF7B7BFF, "brighten red toward white (got 0x%08X)",
          fb[0]);

    // Brightness decrease: BG0 red toward black by EVY=8.
    bus.write16(0x04000050, 0x00C1);  // BLDCNT: darken, 1st BG0
    frame();
    CHECK(fb[0] == 0x840000FF, "darken red toward black (got 0x%08X)", fb[0]);

    // Window effects gate: brighten only outside WIN0 (effects bit set
    // there); inside WIN0 the effects bit is clear so red stays raw.
    bus.write16(0x04000050, 0x0081);  // BLDCNT: brighten, 1st BG0
    bus.write16(0x04000040, 0x0078);  // WIN0H: X1=0, X2=120
    bus.write16(0x04000044, 0x00A0);  // WIN0V: full height
    bus.write16(0x04000048, 0x0001);  // WININ: BG0 on, effects off inside
    bus.write16(0x0400004A, 0x0021);  // WINOUT: BG0 on, effects on outside
    bus.write16(0x04000000, 0x2100);  // mode 0, BG0, WIN0
    frame();
    CHECK(fb[10] == 0xFF0000FF, "inside WIN0: effects off, raw red (got "
          "0x%08X)", fb[10]);
    CHECK(fb[200] == 0xFF7B7BFF, "outside WIN0: effects on, brightened (got "
          "0x%08X)", fb[200]);

    // Semi-transparent sprite (OBJ mode 1) alpha-blends over BG0 even with
    // BLDCNT effect mode set to none.
    bus.write16(0x04000040, 0x0000);  // clear windows
    bus.write16(0x04000044, 0x0000);
    bus.write16(0x07000000, 0x0400);  // attr0: y=0, OBJ mode 1 (semi)
    bus.write16(0x07000002, 0x0000);  // attr1: x=0, 8x8
    bus.write16(0x07000004, 0x0002);  // attr2: tile 2, priority 0
    bus.write16(0x04000050, 0x0100);  // BLDCNT: mode none, 2nd target BG0
    bus.write16(0x04000052, 0x0808);  // EVA=8, EVB=8
    bus.write16(0x0400000A, 0x0801);  // BG1 off-screen priority; keep BG0
    bus.write16(0x04000000, 0x1100);  // mode 0, BG0, OBJ
    frame();
    CHECK(fb[0] == 0x7B007BFF,
          "semi-transparent sprite alpha over BG0 (got 0x%08X)", fb[0]);
}

// Mosaic: BG horizontal/vertical block quantization, the BGxCNT gate, and
// OBJ mosaic quantizing relative to the sprite's own origin.
void testMosaic(Bus& bus) {
    std::printf("Test: mosaic\n");
    PPU ppu(bus);

    bus.write16(0x05000000, 0x0000);  // backdrop black
    bus.write16(0x05000002, 0x001F);  // palette 1: red
    bus.write16(0x05000004, 0x03E0);  // palette 2: green
    bus.write16(0x05000006, 0x7C00);  // palette 3: blue
    bus.write16(0x05000202, 0x001F);  // OBJ palette 1: red
    bus.write16(0x05000204, 0x03E0);  // OBJ palette 2: green
    bus.write16(0x05000206, 0x7C00);  // OBJ palette 3: blue

    // Tile 1: columns cycle red/green/blue (per-column colour). Each 4bpp
    // row is bytes (c1<<4|c0)=0x21, (c3<<4|c2)=0x13, 0x32, 0x21.
    for (uint32_t r = 0; r < 8; ++r) {
        bus.write8(0x06000000 + 32 + r * 4 + 0, 0x21);
        bus.write8(0x06000000 + 32 + r * 4 + 1, 0x13);
        bus.write8(0x06000000 + 32 + r * 4 + 2, 0x32);
        bus.write8(0x06000000 + 32 + r * 4 + 3, 0x21);
        // OBJ tile 1: same per-column layout in the OBJ char block.
        bus.write8(0x06010000 + 32 + r * 4 + 0, 0x21);
        bus.write8(0x06010000 + 32 + r * 4 + 1, 0x13);
        bus.write8(0x06010000 + 32 + r * 4 + 2, 0x32);
        bus.write8(0x06010000 + 32 + r * 4 + 3, 0x21);
    }
    // Tile 2: each row is a solid colour (r%3)+1 (per-row colour).
    for (uint32_t r = 0; r < 8; ++r) {
        const uint8_t c = static_cast<uint8_t>((r % 3) + 1);
        const uint8_t b = static_cast<uint8_t>((c << 4) | c);
        for (uint32_t k = 0; k < 4; ++k) {
            bus.write8(0x06000000 + 64 + r * 4 + k, b);
        }
    }
    bus.write16(0x04000008, 0x0800);  // BG0CNT: char 0, screen 8, prio 0
    for (uint32_t i = 0; i < 1024; ++i) {
        bus.write16(0x06004000 + i * 2, 0x0001);  // map -> tile 1
    }

    auto frame = [&ppu] {
        ppu.step(PPU::CYCLES_SCANLINE * PPU::LINES_TOTAL);
        ppu.frameReady();
    };
    const auto& fb = ppu.framebuffer();
    auto at = [&fb](int x, int y) { return fb[y * 240 + x]; };
    const uint32_t RED = 0xFF0000FF, GREEN = 0x00FF00FF, BLUE = 0x0000FFFF;

    // No mosaic: columns render distinctly red/green/blue.
    bus.write16(0x0400004C, 0x0000);  // MOSAIC: none
    bus.write16(0x04000000, 0x0100);  // mode 0, BG0
    frame();
    CHECK(at(0, 0) == RED && at(1, 0) == GREEN && at(2, 0) == BLUE,
          "no mosaic: distinct columns (got 0x%08X 0x%08X 0x%08X)",
          at(0, 0), at(1, 0), at(2, 0));

    // BG H mosaic, 2-pixel blocks: each pair samples the left column.
    bus.write16(0x0400004C, 0x0001);  // MOSAIC: BG H block = 2
    bus.write16(0x04000008, 0x0840);  // BG0CNT mosaic bit set
    frame();
    CHECK(at(0, 0) == RED && at(1, 0) == RED,
          "BG H mosaic: pixels 0,1 both red (got 0x%08X 0x%08X)", at(0, 0),
          at(1, 0));
    CHECK(at(2, 0) == BLUE && at(3, 0) == BLUE,
          "BG H mosaic: pixels 2,3 both blue (got 0x%08X 0x%08X)", at(2, 0),
          at(3, 0));

    // Gate: same MOSAIC value but the BGxCNT bit clear -> no mosaic.
    bus.write16(0x04000008, 0x0800);  // BG0CNT mosaic bit clear
    frame();
    CHECK(at(1, 0) == GREEN, "BGxCNT gate: bit clear disables mosaic (got "
          "0x%08X)", at(1, 0));

    // BG V mosaic, 2-pixel blocks, per-row tile: rows 0,1 share row 0.
    for (uint32_t i = 0; i < 1024; ++i) {
        bus.write16(0x06004000 + i * 2, 0x0002);  // map -> tile 2
    }
    bus.write16(0x0400004C, 0x0010);  // MOSAIC: BG V block = 2
    bus.write16(0x04000008, 0x0840);  // BG0CNT mosaic bit set
    frame();
    CHECK(at(0, 0) == RED && at(0, 1) == RED,
          "BG V mosaic: lines 0,1 both row-0 red (got 0x%08X 0x%08X)",
          at(0, 0), at(0, 1));
    CHECK(at(0, 2) == BLUE,
          "BG V mosaic: line 2 samples row 2 blue (got 0x%08X)", at(0, 2));

    // OBJ mosaic relative to sprite origin: sprite at odd x=5, H block 2.
    // Screen 5,6 are sprite columns 0,1 -> same block -> both red, proving
    // the grid resets at the sprite, not the screen.
    bus.write16(0x07000000, 0x1000);  // attr0: y=0, mosaic bit
    bus.write16(0x07000002, 0x0005);  // attr1: x=5, 8x8
    bus.write16(0x07000004, 0x0001);  // attr2: tile 1, priority 0
    bus.write16(0x0400004C, 0x0100);  // MOSAIC: OBJ H block = 2
    bus.write16(0x04000000, 0x1040);  // mode 0, OBJ on, 1D map, BG off
    frame();
    CHECK(at(5, 0) == RED && at(6, 0) == RED,
          "OBJ mosaic: screen 5,6 same sprite block, red (got 0x%08X "
          "0x%08X)", at(5, 0), at(6, 0));
    CHECK(at(7, 0) == BLUE,
          "OBJ mosaic: screen 7 is sprite column 2, blue (got 0x%08X)",
          at(7, 0));
}

// SRAM writes land in the save buffer and survive a save/load round trip.
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

// Flash backup: ID-string detection, chip ID mode, byte program, bank
// switching, and sector erase.
void testFlashBackup(Bus& bus) {
    std::printf("Test: Flash backup\n");

    // Minimal ROM carrying the FLASH1M_V ID string.
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

    command(0x90);  // enter chip ID mode
    CHECK(bus.read8(0x0E000000) == 0xC2 && bus.read8(0x0E000001) == 0x09,
          "Macronix 128K chip ID (got 0x%02X 0x%02X)",
          bus.read8(0x0E000000), bus.read8(0x0E000001));
    command(0xF0);  // leave chip ID mode

    command(0xA0);  // program one byte
    bus.write8(0x0E001234, 0x5A);
    CHECK(bus.read8(0x0E001234) == 0x5A, "programmed byte (got 0x%02X)",
          bus.read8(0x0E001234));

    command(0xB0);              // bank switch
    bus.write8(0x0E000000, 1);  // to bank 1
    command(0xA0);
    bus.write8(0x0E001234, 0x77);
    CHECK(bus.read8(0x0E001234) == 0x77, "bank 1 byte (got 0x%02X)",
          bus.read8(0x0E001234));
    command(0xB0);
    bus.write8(0x0E000000, 0);  // back to bank 0
    CHECK(bus.read8(0x0E001234) == 0x5A,
          "banks hold distinct data (bank 0 got 0x%02X)",
          bus.read8(0x0E001234));

    command(0x80);              // arm erase
    bus.write8(0x0E005555, 0xAA);
    bus.write8(0x0E002AAA, 0x55);
    bus.write8(0x0E001000, 0x30);  // erase the 4K sector holding 0x1234
    CHECK(bus.read8(0x0E001234) == 0xFF, "sector erased (got 0x%02X)",
          bus.read8(0x0E001234));
}

// EEPROM backup: serial bitstream protocol on 0x0D with both address
// widths, detected from the request stream length.
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
            b.read16(0x0D000000);  // dummy bits
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

    // 512-byte chip (6-bit addressing): write block 5, read it back.
    sendBits(bus, 0b10, 2);                 // write request
    sendBits(bus, 5, 6);                    // block address
    sendBits(bus, 0xA1B2C3D4E5F60718, 64);  // data
    sendBits(bus, 0, 1);                    // terminator
    CHECK(bus.read16(0x0D000000) & 1, "ready after write");

    sendBits(bus, 0b11, 2);  // read request
    sendBits(bus, 5, 6);
    sendBits(bus, 0, 1);
    const uint64_t value = readBlock(bus);
    CHECK(value == 0xA1B2C3D4E5F60718,
          "6-bit round trip (got 0x%016llX)",
          static_cast<unsigned long long>(value));

    sendBits(bus, 0b11, 2);  // untouched block reads erased
    sendBits(bus, 9, 6);
    sendBits(bus, 0, 1);
    CHECK(readBlock(bus) == 0xFFFFFFFFFFFFFFFF, "untouched block is 0xFF");

    // 8 KiB chip (14-bit addressing) on a fresh Bus.
    Bus big;
    big.loadROM(romPath);
    std::remove(romPath.c_str());
    sendBits(big, 0b10, 2);
    sendBits(big, 700, 14);
    sendBits(big, 0x123456789ABCDEF0, 64);
    sendBits(big, 0, 1);
    big.read16(0x0D000000);  // commit
    sendBits(big, 0b11, 2);
    sendBits(big, 700, 14);
    sendBits(big, 0, 1);
    const uint64_t bigValue = readBlock(big);
    CHECK(bigValue == 0x123456789ABCDEF0,
          "14-bit round trip (got 0x%016llX)",
          static_cast<unsigned long long>(bigValue));
}

// ARM single data transfer: pre-index with writeback, post-index, byte ops.
void testARMSingleDataTransfer(Bus& bus) {
    std::printf("Test: ARM LDR/STR\n");
    ARM7TDMI cpu(bus);
    cpu.reset();

    const uint32_t base = 0x02000A00;
    const uint32_t data = 0x02000B00;

    // Set up registers directly; the instructions under test are the
    // transfers, not immediate construction.
    cpu.setReg(0, data);          // base pointer
    cpu.setReg(1, 0x11223344);    // value to store
    bus.write32(base + 0x00, 0xE5A01004);  // STR r1, [r0, #4]!   (pre, wb)
    bus.write32(base + 0x04, 0xE4902008);  // LDR r2, [r0], #8    (post)
    bus.write32(base + 0x08, 0xE5D03000);  // LDRB r3, [r0]
    bus.write32(base + 0x0C, 0xEAFFFFFE);  // B .
    bus.write8(data + 12, 0x5A);           // byte for the LDRB
    cpu.setReg(15, base);  // after the writes: the flush prefetches

    cpu.step();  // STR r1, [r0, #4]!
    CHECK(bus.read32(data + 4) == 0x11223344,
          "STR pre-indexed stored (got 0x%08X)", bus.read32(data + 4));
    CHECK(cpu.reg(0) == data + 4, "writeback: r0 == base+4 (got 0x%08X)",
          cpu.reg(0));

    cpu.step();  // LDR r2, [r0], #8
    CHECK(cpu.reg(2) == 0x11223344, "LDR post-indexed value (got 0x%08X)",
          cpu.reg(2));
    CHECK(cpu.reg(0) == data + 12, "post-index: r0 == base+12 (got 0x%08X)",
          cpu.reg(0));

    cpu.step();  // LDRB r3, [r0]
    CHECK(cpu.reg(3) == 0x5A, "LDRB byte (got 0x%02X)", cpu.reg(3));
}

// ARM block data transfer: STMDB/LDMIA (full-descending push/pop) with
// base register write-back.
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

    bus.write32(base + 0x00, 0xE92D0007);  // STMDB sp!, {r0-r2}  (push)
    bus.write32(base + 0x04, 0xE3A00000);  // MOV r0, #0
    bus.write32(base + 0x08, 0xE3A01000);  // MOV r1, #0
    bus.write32(base + 0x0C, 0xE3A02000);  // MOV r2, #0
    bus.write32(base + 0x10, 0xE8BD0007);  // LDMIA sp!, {r0-r2}  (pop)
    bus.write32(base + 0x14, 0xEAFFFFFE);  // B .
    cpu.setReg(15, base);

    cpu.step();  // STMDB
    CHECK(cpu.reg(13) == sp - 12, "push write-back: sp -= 12 (got 0x%08X)",
          cpu.reg(13));
    CHECK(bus.read32(sp - 12) == 0xAAAA0000 &&
              bus.read32(sp - 8) == 0xBBBB1111 &&
              bus.read32(sp - 4) == 0xCCCC2222,
          "registers stored ascending from lowest address");

    cpu.step();  // MOV r0, #0
    cpu.step();  // MOV r1, #0
    cpu.step();  // MOV r2, #0
    CHECK(cpu.reg(0) == 0 && cpu.reg(1) == 0 && cpu.reg(2) == 0,
          "registers cleared");

    cpu.step();  // LDMIA
    CHECK(cpu.reg(0) == 0xAAAA0000 && cpu.reg(1) == 0xBBBB1111 &&
              cpu.reg(2) == 0xCCCC2222,
          "pop restored r0-r2 (got 0x%08X 0x%08X 0x%08X)", cpu.reg(0),
          cpu.reg(1), cpu.reg(2));
    CHECK(cpu.reg(13) == sp, "pop write-back: sp restored (got 0x%08X)",
          cpu.reg(13));
}

// VBlank IRQ with no BIOS loaded: the CPU must HLE the BIOS stub, read the
// user handler pointer from 0x03007FFC, run the handler, and resume.
void testHLEInterruptDispatch(Bus& bus) {
    std::printf("Test: HLE BIOS IRQ dispatch\n");
    ARM7TDMI cpu(bus);
    PPU ppu(bus);
    cpu.reset();

    // User IRQ handler in IWRAM. It must leave its evidence in r4: the HLE
    // return pops r0-r3/r12 just like the real BIOS stub, so anything the
    // handler puts there is deliberately undone. IF is acknowledged with a
    // byte store — a word STR would force-align down onto REG_IE.
    const uint32_t handler = 0x03000100;
    bus.write32(handler + 0x00, 0xE3A04001);  // MOV  r4, #1
    bus.write32(handler + 0x04, 0xE3A00301);  // MOV  r0, #0x04000000
    bus.write32(handler + 0x08, 0xE3A01001);  // MOV  r1, #1
    bus.write32(handler + 0x0C, 0xE5C01202);  // STRB r1, [r0, #0x202] (ack)
    bus.write32(handler + 0x10, 0xE12FFF1E);  // BX   lr
    bus.write32(0x03007FFC, handler);         // user IRQ vector

    bus.write16(0x04000004, 1u << 3);  // DISPSTAT: VBlank IRQ enable
    bus.write16(0x04000200, 0x0001);   // IE: VBlank
    bus.write16(0x04000208, 0x0001);   // IME: on

    const uint32_t spin = 0x02000D00;
    bus.write32(spin, 0xEAFFFFFE);  // B .
    cpu.setReg(15, spin);

    bool reachedHandler = false;
    // 80000 instructions x 4 cycles covers more than a full frame, so the
    // VBlank at line 160 fires well within the loop.
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

// ARM halfword/signed transfers: STRH stores 16 bits, LDRH zero-extends,
// LDRSB/LDRSH sign-extend.
void testARMHalfwordTransfer(Bus& bus) {
    std::printf("Test: ARM halfword/signed transfers\n");
    ARM7TDMI cpu(bus);
    cpu.reset();

    const uint32_t base = 0x02000E00;
    const uint32_t data = 0x02000F00;
    cpu.setReg(0, data);
    cpu.setReg(1, 0x1234BEEF);
    bus.write32(base + 0x00, 0xE1C010B0);  // STRH  r1, [r0]
    bus.write32(base + 0x04, 0xE1D020B0);  // LDRH  r2, [r0]
    bus.write32(base + 0x08, 0xE1D030D1);  // LDRSB r3, [r0, #1]
    bus.write32(base + 0x0C, 0xE1D040F0);  // LDRSH r4, [r0]
    bus.write32(base + 0x10, 0xEAFFFFFE);  // B .
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

// MUL/MLA and the long multiplies, including the S-bit N flag on SMULL.
void testARMMultiply(Bus& bus) {
    std::printf("Test: ARM multiply\n");
    ARM7TDMI cpu(bus);
    cpu.reset();

    const uint32_t base = 0x02001100;
    cpu.setReg(0, 7);
    cpu.setReg(1, 6);
    cpu.setReg(8, 0x80000000);
    cpu.setReg(9, 4);
    cpu.setReg(10, 0xFFFFFFFE);  // -2
    cpu.setReg(11, 3);
    bus.write32(base + 0x00, 0xE0020190);  // MUL    r2, r0, r1
    bus.write32(base + 0x04, 0xE0232190);  // MLA    r3, r0, r1, r2
    bus.write32(base + 0x08, 0xE0854998);  // UMULL  r4, r5, r8, r9
    bus.write32(base + 0x0C, 0xE0D76B9A);  // SMULLS r6, r7, r10, r11
    bus.write32(base + 0x10, 0xEAFFFFFE);  // B .
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

// MSR field masks: the flags field is writable, the T bit never is.
void testPSRTransfer(Bus& bus) {
    std::printf("Test: MRS/MSR\n");
    ARM7TDMI cpu(bus);
    cpu.reset();

    const uint32_t base = 0x02001200;
    bus.write32(base + 0x00, 0xE3A0020F);  // MOV r0, #0xF0000000
    bus.write32(base + 0x04, 0xE128F000);  // MSR CPSR_f, r0
    bus.write32(base + 0x08, 0xE10F1000);  // MRS r1, CPSR
    bus.write32(base + 0x0C, 0xE3A0203F);  // MOV r2, #0x3F (System + T bit)
    bus.write32(base + 0x10, 0xE121F002);  // MSR CPSR_c, r2
    bus.write32(base + 0x14, 0xE10F3000);  // MRS r3, CPSR
    bus.write32(base + 0x18, 0xEAFFFFFE);  // B .
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

// Hardware timers: prescaler, reload-on-overflow, cascade, and the IRQ bit.
void testTimers(Bus& bus) {
    std::printf("Test: hardware timers\n");
    Timers timers(bus);
    bus.attachTimers(&timers);

    bus.write16(0x04000100, 0xFFF0);  // TM0CNT_L: reload
    bus.write16(0x04000102, 0x00C1);  // TM0CNT_H: enable, IRQ, prescaler 64
    CHECK(bus.read16(0x04000100) == 0xFFF0,
          "counter loaded from reload on enable (got 0x%04X)",
          bus.read16(0x04000100));

    timers.step(64);  // one prescaled tick
    CHECK(bus.read16(0x04000100) == 0xFFF1,
          "prescaler 64: one tick after 64 cycles (got 0x%04X)",
          bus.read16(0x04000100));

    bus.write16(0x04000104, 0x0000);  // TM1CNT_L: reload 0
    bus.write16(0x04000106, 0x0084);  // TM1CNT_H: enable, cascade

    timers.step(15 * 64);  // counter 0xFFF1 + 15 -> overflow
    CHECK(bus.read16(0x04000100) == 0xFFF0,
          "timer 0 reloaded on overflow (got 0x%04X)",
          bus.read16(0x04000100));
    CHECK(bus.read16(0x04000104) == 1,
          "timer 1 cascaded on timer 0 overflow (got %u)",
          bus.read16(0x04000104));
    CHECK(bus.read16(0x04000202) & (1u << 3),
          "timer 0 IRQ raised in IF (IF=0x%04X)", bus.read16(0x04000202));
}

// SWI HLE: Div/DivArm/Sqrt/ArcTan2 register conventions.
void testSWIArithmetic(Bus& bus) {
    std::printf("Test: SWI arithmetic (Div/Sqrt/ArcTan2)\n");
    ARM7TDMI cpu(bus);
    cpu.reset();

    const uint32_t base = 0x02001400;
    bus.write32(base + 0x00, 0xEF060000);  // SWI Div
    bus.write32(base + 0x04, 0xEF080000);  // SWI Sqrt
    bus.write32(base + 0x08, 0xEF0A0000);  // SWI ArcTan2
    bus.write32(base + 0x0C, 0xEAFFFFFE);  // B .
    cpu.setReg(15, base);

    cpu.setReg(0, static_cast<uint32_t>(-7));
    cpu.setReg(1, 2);
    cpu.step();  // Div
    CHECK(cpu.reg(0) == static_cast<uint32_t>(-3) &&
              cpu.reg(1) == static_cast<uint32_t>(-1) && cpu.reg(3) == 3,
          "Div -7/2: q=-3 r=-1 abs=3 (got %d %d %u)",
          static_cast<int32_t>(cpu.reg(0)), static_cast<int32_t>(cpu.reg(1)),
          cpu.reg(3));

    cpu.setReg(0, 0x00010000);
    cpu.step();  // Sqrt
    CHECK(cpu.reg(0) == 0x100, "Sqrt 0x10000 == 0x100 (got 0x%X)",
          cpu.reg(0));

    cpu.setReg(0, 0);       // x
    cpu.setReg(1, 0x1000);  // y > 0
    cpu.step();  // ArcTan2
    CHECK(cpu.reg(0) == 0x4000, "ArcTan2(0, +y) == 0x4000 (got 0x%04X)",
          cpu.reg(0));
}

// SWI CpuSet (copy + fill) and CpuFastSet.
void testSWICpuSet(Bus& bus) {
    std::printf("Test: SWI CpuSet/CpuFastSet\n");
    ARM7TDMI cpu(bus);
    cpu.reset();

    const uint32_t base = 0x02001500;
    bus.write32(base + 0x00, 0xEF0B0000);  // SWI CpuSet
    bus.write32(base + 0x04, 0xEF0B0000);  // SWI CpuSet
    bus.write32(base + 0x08, 0xEF0C0000);  // SWI CpuFastSet
    bus.write32(base + 0x0C, 0xEAFFFFFE);  // B .
    cpu.setReg(15, base);

    const uint32_t src = 0x02002000;
    for (uint32_t i = 0; i < 4; ++i) {
        bus.write32(src + i * 4, 0xCAFE0000 + i);
    }
    cpu.setReg(0, src);
    cpu.setReg(1, 0x02002100);
    cpu.setReg(2, 4 | (1u << 26));  // 4 words, copy
    cpu.step();
    bool match = true;
    for (uint32_t i = 0; i < 4; ++i) {
        match = match && bus.read32(0x02002100 + i * 4) == 0xCAFE0000 + i;
    }
    CHECK(match, "CpuSet copied 4 words");

    bus.write16(0x02002200, 0xBEEF);
    cpu.setReg(0, 0x02002200);
    cpu.setReg(1, 0x02002280);
    cpu.setReg(2, 8 | (1u << 24));  // 8 halfwords, fill
    cpu.step();
    CHECK(bus.read16(0x02002280) == 0xBEEF &&
              bus.read16(0x0200228E) == 0xBEEF,
          "CpuSet halfword fill (got 0x%04X, 0x%04X)",
          bus.read16(0x02002280), bus.read16(0x0200228E));

    bus.write32(0x02002300, 0xDEADBEEF);
    cpu.setReg(0, 0x02002300);
    cpu.setReg(1, 0x02002380);
    cpu.setReg(2, 8 | (1u << 24));  // 8 words, fill
    cpu.step();
    CHECK(bus.read32(0x02002380) == 0xDEADBEEF &&
              bus.read32(0x0200239C) == 0xDEADBEEF,
          "CpuFastSet word fill (got 0x%08X)", bus.read32(0x0200239C));
}

// SWI decompression: LZ77, RL, BitUnPack and Huffman with hand-built
// streams against known plaintext.
void testSWIDecompression(Bus& bus) {
    std::printf("Test: SWI decompression\n");
    ARM7TDMI cpu(bus);
    cpu.reset();

    const uint32_t base = 0x02001600;
    bus.write32(base + 0x00, 0xEF110000);  // SWI LZ77UnCompWram
    bus.write32(base + 0x04, 0xEF140000);  // SWI RLUnCompWram
    bus.write32(base + 0x08, 0xEF100000);  // SWI BitUnPack
    bus.write32(base + 0x0C, 0xEF130000);  // SWI HuffUnComp
    bus.write32(base + 0x10, 0xEAFFFFFE);  // B .
    cpu.setReg(15, base);

    // LZ77: literals A B C D then a back-reference (disp 3, len 4) -> the
    // 8 bytes "ABCDABCD".
    const uint32_t lzSrc = 0x02002400;
    bus.write32(lzSrc, (8u << 8) | 0x10);  // header: type 1, size 8
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

    // RL: run of 5 x 0x77, then 2 literals 0x11 0x22.
    const uint32_t rlSrc = 0x02002600;
    bus.write32(rlSrc, (7u << 8) | 0x30);  // header: type 3, size 7
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

    // BitUnPack: 4-bit units 1..8 widened to bytes with +0x10.
    const uint32_t buSrc = 0x02002800;
    const uint8_t packed[] = {0x21, 0x43, 0x65, 0x87};
    for (uint32_t i = 0; i < 4; ++i) {
        bus.write8(buSrc + i, packed[i]);
    }
    const uint32_t info = 0x02002810;
    bus.write16(info, 4);        // srcLen
    bus.write8(info + 2, 4);     // srcWidth
    bus.write8(info + 3, 8);     // dstWidth
    bus.write32(info + 4, 0x10); // dataOffset
    cpu.setReg(0, buSrc);
    cpu.setReg(1, 0x02002900);
    cpu.setReg(2, info);
    cpu.step();
    CHECK(bus.read32(0x02002900) == 0x14131211 &&
              bus.read32(0x02002904) == 0x18171615,
          "BitUnPack 4->8 bit +0x10 (got 0x%08X 0x%08X)",
          bus.read32(0x02002900), bus.read32(0x02002904));

    // Huffman: 8-bit symbols, two-leaf tree (A left, B right), bitstream
    // 0101 MSB-first -> "ABAB".
    const uint32_t huffSrc = 0x02002A00;
    bus.write32(huffSrc, (4u << 8) | 0x28);  // type 2, 8-bit data, size 4
    bus.write8(huffSrc + 4, 1);              // tree size byte
    bus.write8(huffSrc + 5, 0xC0);           // root: both children are data
    bus.write8(huffSrc + 6, 'A');
    bus.write8(huffSrc + 7, 'B');
    bus.write32(huffSrc + 8, 0x50000000);    // bits 0,1,0,1 MSB-first
    cpu.setReg(0, huffSrc);
    cpu.setReg(1, 0x02002B00);
    cpu.step();
    CHECK(bus.read32(0x02002B00) == 0x42414241,
          "Huffman -> ABAB (got 0x%08X)", bus.read32(0x02002B00));
}

// VBlankIntrWait: the CPU must sleep through the visible frame, wake on the
// VBlank IRQ, run the user handler (which sets the IF-shadow), and resume.
void testSWIIntrWait(Bus& bus) {
    std::printf("Test: SWI VBlankIntrWait\n");
    ARM7TDMI cpu(bus);
    PPU ppu(bus);
    cpu.reset();

    // User handler: acknowledge IF bit 0 and OR it into the shadow at
    // 0x03007FF8, then return.
    const uint32_t handler = 0x03000200;
    bus.write32(handler + 0x00, 0xE3A00301);  // MOV  r0, #0x04000000
    bus.write32(handler + 0x04, 0xE3A01001);  // MOV  r1, #1
    bus.write32(handler + 0x08, 0xE5C01202);  // STRB r1, [r0, #0x202]
    bus.write32(handler + 0x0C, 0xE3A02403);  // MOV  r2, #0x03000000
    bus.write32(handler + 0x10, 0xE3822C7F);  // ORR  r2, r2, #0x7F00
    bus.write32(handler + 0x14, 0xE38220F8);  // ORR  r2, r2, #0xF8
    bus.write32(handler + 0x18, 0xE1D230B0);  // LDRH r3, [r2]
    bus.write32(handler + 0x1C, 0xE3833001);  // ORR  r3, r3, #1
    bus.write32(handler + 0x20, 0xE1C230B0);  // STRH r3, [r2]
    bus.write32(handler + 0x24, 0xE12FFF1E);  // BX   lr
    bus.write32(0x03007FFC, handler);

    bus.write16(0x04000004, 1u << 3);  // DISPSTAT: VBlank IRQ enable
    bus.write16(0x04000200, 0x0001);   // IE: VBlank

    const uint32_t prog = 0x02001700;
    bus.write32(prog + 0x00, 0xEF050000);  // SWI VBlankIntrWait
    bus.write32(prog + 0x04, 0xE3A07001);  // MOV r7, #1
    bus.write32(prog + 0x08, 0xEAFFFFFE);  // B .
    cpu.setReg(7, 0);
    cpu.setReg(15, prog);

    // Run 10000 steps (~40000 cycles): VBlank starts at cycle 197120, so
    // the CPU must still be asleep with r7 unset.
    for (int i = 0; i < 10000; ++i) {
        cpu.step();
        ppu.step(4);
    }
    CHECK(cpu.isHalted() && cpu.reg(7) == 0,
          "CPU asleep mid-frame (halted=%d, r7=%u)", cpu.isHalted(),
          cpu.reg(7));

    // Finish the frame and a bit more: the wait must complete.
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

// SWP/SWPB atomic swaps.
void testSwap(Bus& bus) {
    std::printf("Test: SWP/SWPB\n");
    ARM7TDMI cpu(bus);
    cpu.reset();

    const uint32_t base = 0x02001800;
    const uint32_t addr = 0x02003000;
    bus.write32(addr, 0x11223344);
    bus.write32(base + 0x00, 0xE1002091);  // SWP  r2, r1, [r0]
    bus.write32(base + 0x04, 0xE1403094);  // SWPB r3, r4, [r0]
    bus.write32(base + 0x08, 0xEAFFFFFE);  // B .
    cpu.setReg(0, addr);
    cpu.setReg(1, 0xAABBCCDD);
    cpu.setReg(4, 0xEE);
    cpu.setReg(15, base);

    cpu.step();  // SWP
    CHECK(cpu.reg(2) == 0x11223344 && bus.read32(addr) == 0xAABBCCDD,
          "SWP swapped word (r2=0x%08X mem=0x%08X)", cpu.reg(2),
          bus.read32(addr));

    cpu.step();  // SWPB
    CHECK(cpu.reg(3) == 0xDD && bus.read8(addr) == 0xEE,
          "SWPB swapped byte (r3=0x%02X mem=0x%02X)", cpu.reg(3),
          bus.read8(addr));
}

// PSG square channel: trigger raises the active flag, the mixer produces
// non-silent samples, and the length counter silences the channel.
void testAPUSquare(Bus& bus) {
    std::printf("Test: APU square channel\n");
    APU apu(bus);
    bus.attachAPU(&apu);

    bus.write16(0x04000084, 0x0080);  // SOUNDCNT_X: master enable
    bus.write16(0x04000080, 0xFF77);  // SOUNDCNT_L: all channels, vol 7/7
    bus.write16(0x04000082, 0x0002);  // SOUNDCNT_H: PSG 100%
    bus.write16(0x04000062, 0xF080);  // 50% duty, constant volume 15
    bus.write16(0x04000064, 0x8400);  // trigger, freq 1024

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

    // Re-trigger with length 1 and the length counter enabled: the channel
    // must fall silent within two frame-sequencer steps (<= 131072 cycles).
    bus.write16(0x04000062, 0xF0BF);  // length field 63 -> counter = 1
    bus.write16(0x04000064, 0xC400);  // trigger + length enable
    apu.step(131072);
    CHECK((bus.read16(0x04000084) & 1) == 0,
          "length expiry cleared the active flag (got 0x%04X)",
          bus.read16(0x04000084));
}

// Direct Sound: timer 0 overflows drain FIFO A; once half empty, DMA 1 in
// special mode refills it through the Bus.
void testAPUFifoDMA(Bus& bus) {
    std::printf("Test: APU Direct Sound FIFO + DMA refill\n");
    APU apu(bus);
    bus.attachAPU(&apu);
    DMA dma(bus);
    bus.attachDMA(&dma);
    Timers timers(bus);
    bus.attachTimers(&timers);

    bus.write16(0x04000084, 0x0080);  // master enable
    bus.write16(0x04000082, 0x0304);  // FIFO A: L+R, 100%, timer 0

    for (int i = 0; i < 8; ++i) {
        bus.write32(0x040000A0, 0x40404040);
    }
    CHECK(apu.fifoCount(0) == 32, "FIFO A filled by CPU writes (count=%d)",
          apu.fifoCount(0));

    // DMA1: source in EWRAM, destination FIFO_A, special timing, repeat,
    // 32-bit, destination fixed.
    const uint32_t src = 0x02001300;
    for (uint32_t i = 0; i < 16; ++i) {
        bus.write8(src + i, 0x55);
    }
    bus.write32(0x040000BC, src);
    bus.write32(0x040000C0, 0x040000A0);
    bus.write16(0x040000C6, 0xB640);

    // Timer 0 overflows every cycle (reload 0xFFFF, prescaler 1). After 17
    // overflows the FIFO has dropped to 16 once (refilled +16) and then
    // lost one more: 32 - 17 + 16 = 31.
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

}  // namespace

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
        testThumbUnalignedLoad(bus);
    }
    testSaveStateComponents();
    testSaveStateEmulator();
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
