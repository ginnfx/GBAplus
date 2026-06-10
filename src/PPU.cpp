#include "PPU.hpp"

#include "Bus.hpp"
#include "Log.hpp"

namespace {
// LCD IO registers.
constexpr uint32_t REG_DISPCNT  = 0x04000000;
constexpr uint32_t REG_DISPSTAT = 0x04000004;
constexpr uint32_t REG_VCOUNT   = 0x04000006;
constexpr uint32_t REG_BG0CNT   = 0x04000008;
constexpr uint32_t REG_BG0HOFS  = 0x04000010;

constexpr uint16_t DISPSTAT_VBLANK     = 1u << 0;
constexpr uint16_t DISPSTAT_HBLANK     = 1u << 1;
constexpr uint16_t DISPSTAT_VCOUNT     = 1u << 2;
constexpr uint16_t DISPSTAT_VBLANK_IRQ = 1u << 3;
constexpr uint16_t DISPSTAT_HBLANK_IRQ = 1u << 4;
constexpr uint16_t DISPSTAT_VCOUNT_IRQ = 1u << 5;

constexpr uint16_t DISPCNT_OBJ_1D  = 1u << 6;
constexpr uint16_t DISPCNT_OBJ_ON  = 1u << 12;

constexpr uint32_t VRAM_BASE     = 0x06000000;
constexpr uint32_t VRAM_OBJ_BASE = 0x06010000;
constexpr uint32_t PALETTE_BG    = 0x05000000;
constexpr uint32_t PALETTE_OBJ   = 0x05000200;
constexpr uint32_t OAM_BASE      = 0x07000000;

// Sprite dimensions in pixels, indexed by [shape][size].
constexpr int OBJ_WIDTH[3][4]  = {{8, 16, 32, 64},
                                  {16, 32, 32, 64},
                                  {8, 8, 16, 32}};
constexpr int OBJ_HEIGHT[3][4] = {{8, 16, 32, 64},
                                  {8, 8, 16, 32},
                                  {16, 32, 32, 64}};

constexpr int PRIORITY_BACKDROP = 4;
}  // namespace

PPU::PPU(Bus& bus) : bus(bus) {}

uint32_t PPU::bgr555ToRGBA(uint16_t color) {
    const uint32_t r5 = color & 0x1F;
    const uint32_t g5 = (color >> 5) & 0x1F;
    const uint32_t b5 = (color >> 10) & 0x1F;
    // Expand 5 bits to 8 by replicating the top bits into the bottom,
    // so 0x1F maps to 0xFF and 0x00 maps to 0x00.
    const uint32_t r8 = (r5 << 3) | (r5 >> 2);
    const uint32_t g8 = (g5 << 3) | (g5 >> 2);
    const uint32_t b8 = (b5 << 3) | (b5 >> 2);
    return (r8 << 24) | (g8 << 16) | (b8 << 8) | 0xFF;
}

uint32_t PPU::paletteColor(uint32_t index) const {
    return bgr555ToRGBA(bus.read16(PALETTE_BG + index * 2));
}

uint32_t PPU::objPaletteColor(uint32_t index) const {
    return bgr555ToRGBA(bus.read16(PALETTE_OBJ + index * 2));
}

void PPU::renderScanline(int line) {
    const uint16_t mode = bus.read16(REG_DISPCNT) & 0x7;
    switch (mode) {
        case 0:
            renderMode0(line);
            break;
        case 3:
            renderMode3(line);
            break;
        default:
            TRACE_LOG("renderScanline: unimplemented video mode %u", mode);
            for (int x = 0; x < SCREEN_WIDTH; ++x) {
                fb[line * SCREEN_WIDTH + x] = 0x000000FF;
            }
            break;
    }
}

// Mode 3: single 240x160 16bpp bitmap straight in VRAM.
void PPU::renderMode3(int line) {
    for (int x = 0; x < SCREEN_WIDTH; ++x) {
        const uint32_t addr =
            VRAM_BASE + static_cast<uint32_t>(line * SCREEN_WIDTH + x) * 2;
        fb[line * SCREEN_WIDTH + x] = bgr555ToRGBA(bus.read16(addr));
    }
}

// Mode 0: up to four text (tiled) backgrounds plus sprites.
void PPU::renderMode0(int line) {
    const uint16_t dispcnt = bus.read16(REG_DISPCNT);

    uint32_t colors[SCREEN_WIDTH];
    int priorities[SCREEN_WIDTH];
    const uint32_t backdrop = paletteColor(0);
    for (int x = 0; x < SCREEN_WIDTH; ++x) {
        colors[x] = backdrop;
        priorities[x] = PRIORITY_BACKDROP;
    }

    // Draw lowest priority first so higher priorities overwrite. Within a
    // priority level, higher BG numbers sit below lower ones.
    for (int priority = 3; priority >= 0; --priority) {
        for (int bg = 3; bg >= 0; --bg) {
            if (!(dispcnt & (1u << (8 + bg)))) {
                continue;
            }
            const uint16_t cnt =
                bus.read16(REG_BG0CNT + static_cast<uint32_t>(bg) * 2);
            if ((cnt & 3) != priority) {
                continue;
            }
            renderBackgroundLine(bg, line, priority, colors, priorities);
        }
    }

    if (dispcnt & DISPCNT_OBJ_ON) {
        renderSpritesLine(line, colors, priorities);
    }

    for (int x = 0; x < SCREEN_WIDTH; ++x) {
        fb[line * SCREEN_WIDTH + x] = colors[x];
    }
}

void PPU::renderBackgroundLine(int bg, int line, int priority,
                               uint32_t* colors, int* priorities) {
    const uint16_t cnt =
        bus.read16(REG_BG0CNT + static_cast<uint32_t>(bg) * 2);
    const uint32_t charBase = VRAM_BASE + ((cnt >> 2) & 3) * 0x4000;
    const uint32_t screenBase = VRAM_BASE + ((cnt >> 8) & 0x1F) * 0x800;
    const bool is8bpp = cnt & 0x80;
    const int size = (cnt >> 14) & 3;
    const int widthTiles = (size & 1) ? 64 : 32;
    const int heightTiles = (size & 2) ? 64 : 32;

    const uint32_t hofsAddr = REG_BG0HOFS + static_cast<uint32_t>(bg) * 4;
    const int hofs = bus.read16(hofsAddr) & 0x1FF;
    const int vofs = bus.read16(hofsAddr + 2) & 0x1FF;

    const int vy = (line + vofs) & (heightTiles * 8 - 1);
    const int tileY = vy >> 3;

    for (int x = 0; x < SCREEN_WIDTH; ++x) {
        const int vx = (x + hofs) & (widthTiles * 8 - 1);
        const int tileX = vx >> 3;

        // Screenblock layout: 64-wide and/or 64-tall maps are built from
        // multiple 32x32-entry blocks of 2 KiB each.
        uint32_t block = 0;
        if (size == 1) block = static_cast<uint32_t>(tileX >> 5);
        if (size == 2) block = static_cast<uint32_t>(tileY >> 5);
        if (size == 3) {
            block = static_cast<uint32_t>((tileX >> 5) + (tileY >> 5) * 2);
        }
        const uint32_t mapAddr =
            screenBase + block * 0x800 +
            static_cast<uint32_t>(((tileY & 31) * 32 + (tileX & 31)) * 2);
        const uint16_t entry = bus.read16(mapAddr);
        const uint32_t tile = entry & 0x3FF;

        int px = vx & 7;
        int py = vy & 7;
        if (entry & 0x400) px = 7 - px;  // horizontal flip
        if (entry & 0x800) py = 7 - py;  // vertical flip

        uint32_t colorIndex;
        if (is8bpp) {
            colorIndex = bus.read8(charBase + tile * 64 +
                                   static_cast<uint32_t>(py * 8 + px));
        } else {
            const uint8_t pair =
                bus.read8(charBase + tile * 32 +
                          static_cast<uint32_t>(py * 4 + px / 2));
            colorIndex = (px & 1) ? (pair >> 4) : (pair & 0xF);
            if (colorIndex != 0) {
                colorIndex += ((entry >> 12) & 0xF) * 16;  // palette bank
            }
        }
        if (colorIndex == 0) {
            continue;  // color 0 is transparent
        }
        colors[x] = paletteColor(colorIndex);
        priorities[x] = priority;
    }
}

void PPU::renderSpritesLine(int line, uint32_t* colors,
                            const int* priorities) {
    const bool map1D = bus.read16(REG_DISPCNT) & DISPCNT_OBJ_1D;

    // Iterate high to low so the lower OAM index (higher priority on
    // hardware) is drawn last and wins overlaps.
    for (int obj = 127; obj >= 0; --obj) {
        const uint32_t entry = OAM_BASE + static_cast<uint32_t>(obj) * 8;
        const uint16_t attr0 = bus.read16(entry);
        const uint16_t attr1 = bus.read16(entry + 2);
        const uint16_t attr2 = bus.read16(entry + 4);

        if (attr0 & 0x100) {
            TRACE_LOG("affine sprite %d skipped", obj);
            continue;
        }
        if (attr0 & 0x200) {
            continue;  // disabled
        }
        const uint32_t shape = attr0 >> 14;
        if (shape == 3) {
            continue;  // prohibited shape
        }
        const uint32_t sizeIdx = attr1 >> 14;
        const int width = OBJ_WIDTH[shape][sizeIdx];
        const int height = OBJ_HEIGHT[shape][sizeIdx];

        int y = attr0 & 0xFF;
        if (y >= SCREEN_HEIGHT) {
            y -= 256;  // wraps negative
        }
        int row = line - y;
        if (row < 0 || row >= height) {
            continue;
        }
        if (attr1 & 0x2000) {
            row = height - 1 - row;  // vertical flip
        }

        int x0 = attr1 & 0x1FF;
        if (x0 >= SCREEN_WIDTH) {
            x0 -= 512;
        }

        const bool is8bpp = attr0 & 0x2000;
        const uint32_t baseTile = attr2 & 0x3FF;
        const int priority = (attr2 >> 10) & 3;
        const uint32_t palBank = attr2 >> 12;
        const bool hflip = attr1 & 0x1000;
        const uint32_t tileStep = is8bpp ? 2 : 1;  // tile units of 32 bytes

        for (int i = 0; i < width; ++i) {
            const int sx = x0 + i;
            if (sx < 0 || sx >= SCREEN_WIDTH) {
                continue;
            }
            if (priority > priorities[sx]) {
                continue;  // behind the background at this pixel
            }
            const int col = hflip ? (width - 1 - i) : i;
            const uint32_t tileX = static_cast<uint32_t>(col >> 3);
            const uint32_t tileY = static_cast<uint32_t>(row >> 3);
            const uint32_t rowStride =
                map1D ? static_cast<uint32_t>(width / 8) * tileStep : 32;
            const uint32_t tileNum =
                baseTile + tileY * rowStride + tileX * tileStep;
            const uint32_t tileAddr = VRAM_OBJ_BASE + (tileNum & 0x3FF) * 32;

            const int px = col & 7;
            const int py = row & 7;
            uint32_t colorIndex;
            if (is8bpp) {
                colorIndex = bus.read8(
                    tileAddr + static_cast<uint32_t>(py * 8 + px));
            } else {
                const uint8_t pair = bus.read8(
                    tileAddr + static_cast<uint32_t>(py * 4 + px / 2));
                colorIndex = (px & 1) ? (pair >> 4) : (pair & 0xF);
                if (colorIndex != 0) {
                    colorIndex += palBank * 16;
                }
            }
            if (colorIndex == 0) {
                continue;  // transparent
            }
            colors[sx] = objPaletteColor(colorIndex);
        }
    }
}

void PPU::step(int cycles) {
    cycleCounter += cycles;

    for (;;) {
        if (!inHBlank && cycleCounter >= CYCLES_HDRAW) {
            inHBlank = true;
            const uint16_t stat = bus.read16(REG_DISPSTAT);
            bus.writeIODirect16(REG_DISPSTAT, stat | DISPSTAT_HBLANK);
            if (vcount < LINES_VISIBLE) {
                renderScanline(vcount);
                bus.notifyHBlank();  // HBlank-timed DMA (visible lines only)
            }
            if (stat & DISPSTAT_HBLANK_IRQ) {
                bus.requestInterrupt(Bus::IRQ_HBLANK);
            }
        }
        if (cycleCounter < CYCLES_SCANLINE) {
            break;
        }
        cycleCounter -= CYCLES_SCANLINE;
        inHBlank = false;

        vcount = (vcount + 1) % LINES_TOTAL;
        uint16_t stat = bus.read16(REG_DISPSTAT);
        stat &= static_cast<uint16_t>(~DISPSTAT_HBLANK);

        if (vcount == LINES_VISIBLE) {
            stat |= DISPSTAT_VBLANK;
            frameDone = true;
            if (stat & DISPSTAT_VBLANK_IRQ) {
                bus.requestInterrupt(Bus::IRQ_VBLANK);
            }
            bus.notifyVBlank();  // VBlank-timed DMA
        } else if (vcount == 0) {
            stat &= static_cast<uint16_t>(~DISPSTAT_VBLANK);
        }

        // VCOUNT match against the LYC value in DISPSTAT bits 8-15.
        if (vcount == (stat >> 8)) {
            stat |= DISPSTAT_VCOUNT;
            if (stat & DISPSTAT_VCOUNT_IRQ) {
                bus.requestInterrupt(Bus::IRQ_VCOUNT);
            }
        } else {
            stat &= static_cast<uint16_t>(~DISPSTAT_VCOUNT);
        }

        bus.writeIODirect16(REG_DISPSTAT, stat);
        bus.writeIODirect16(REG_VCOUNT, static_cast<uint16_t>(vcount));
    }
}

bool PPU::frameReady() {
    const bool ready = frameDone;
    frameDone = false;
    return ready;
}
