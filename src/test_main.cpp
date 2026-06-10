// Unit test harness (gba_test_harness target). Each test gets a fresh Bus
// so IO/VRAM state cannot leak across tests.

#include <cstdio>
#include <string>

#include "ARM7TDMI.hpp"
#include "Bus.hpp"
#include "DMA.hpp"
#include "PPU.hpp"

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

    if (failures == 0) {
        std::printf("All tests passed.\n");
        return 0;
    }
    std::printf("%d check(s) FAILED.\n", failures);
    return 1;
}
