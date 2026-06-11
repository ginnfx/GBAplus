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
constexpr uint32_t REG_BG2PA    = 0x04000020;  // BG3 set at +0x10
constexpr uint32_t REG_BG2X     = 0x04000028;
constexpr uint32_t REG_WIN0H    = 0x04000040;  // WIN1H/0V/1V follow in order
constexpr uint32_t REG_WIN0V    = 0x04000044;
constexpr uint32_t REG_WIN1H    = 0x04000042;
constexpr uint32_t REG_WIN1V    = 0x04000046;
constexpr uint32_t REG_WININ    = 0x04000048;
constexpr uint32_t REG_WINOUT   = 0x0400004A;

constexpr uint16_t DISPSTAT_VBLANK     = 1u << 0;
constexpr uint16_t DISPSTAT_HBLANK     = 1u << 1;
constexpr uint16_t DISPSTAT_VCOUNT     = 1u << 2;
constexpr uint16_t DISPSTAT_VBLANK_IRQ = 1u << 3;
constexpr uint16_t DISPSTAT_HBLANK_IRQ = 1u << 4;
constexpr uint16_t DISPSTAT_VCOUNT_IRQ = 1u << 5;

constexpr uint16_t DISPCNT_OBJ_1D  = 1u << 6;
constexpr uint16_t DISPCNT_OBJ_ON  = 1u << 12;
constexpr uint16_t DISPCNT_WIN0    = 1u << 13;
constexpr uint16_t DISPCNT_WIN1    = 1u << 14;
constexpr uint16_t DISPCNT_OBJWIN  = 1u << 15;

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
            renderTiledLine(line, 0xF, 0x0);
            break;
        case 1:
            renderTiledLine(line, 0x3, 0x4);
            break;
        case 2:
            renderTiledLine(line, 0x0, 0xC);
            break;
        case 3:
            renderMode3(line);
            break;
        case 4:
            renderMode4(line);
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

// Mode 4: 240x160 8bpp paletted bitmap with two pages for double
// buffering; DISPCNT bit 4 selects the displayed page. Palette index 0 is
// transparent, leaving the backdrop visible under sprites.
void PPU::renderMode4(int line) {
    const uint16_t dispcnt = bus.read16(REG_DISPCNT);
    const uint32_t page = (dispcnt & (1u << 4)) ? 0xA000u : 0u;
    const uint16_t bg2cnt = bus.read16(REG_BG0CNT + 4);
    const int bgPriority = bg2cnt & 3;
    const bool bg2on = dispcnt & (1u << 10);

    uint32_t colors[SCREEN_WIDTH];
    int priorities[SCREEN_WIDTH];
    const uint32_t backdrop = paletteColor(0);
    for (int x = 0; x < SCREEN_WIDTH; ++x) {
        colors[x] = backdrop;
        priorities[x] = PRIORITY_BACKDROP;
        if (bg2on) {
            const uint8_t index = bus.read8(
                VRAM_BASE + page +
                static_cast<uint32_t>(line * SCREEN_WIDTH + x));
            if (index != 0) {
                colors[x] = paletteColor(index);
                priorities[x] = bgPriority;
            }
        }
    }

    if (dispcnt & DISPCNT_OBJ_ON) {
        // Windowing in the bitmap modes is not modelled yet; sprites here
        // draw unconditionally.
        renderSpritesLine(line, colors, priorities, nullptr);
    }

    for (int x = 0; x < SCREEN_WIDTH; ++x) {
        fb[line * SCREEN_WIDTH + x] = colors[x];
    }
}

// Modes 0-2: tiled backgrounds plus sprites. The masks say which BGs are
// text and which are affine in the current video mode; a BG still needs
// its DISPCNT enable bit either way.
void PPU::renderTiledLine(int line, unsigned textMask, unsigned affineMask) {
    const uint16_t dispcnt = bus.read16(REG_DISPCNT);

    uint32_t colors[SCREEN_WIDTH];
    int priorities[SCREEN_WIDTH];
    const uint32_t backdrop = paletteColor(0);
    for (int x = 0; x < SCREEN_WIDTH; ++x) {
        colors[x] = backdrop;
        priorities[x] = PRIORITY_BACKDROP;
    }

    // Per-pixel layer-enable mask; nullptr when no window is active, which
    // lets every layer draw everywhere with no per-pixel test.
    uint8_t winMaskBuf[SCREEN_WIDTH];
    const uint8_t* winMask =
        computeWindowMask(line, winMaskBuf) ? winMaskBuf : nullptr;

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
            if (affineMask & (1u << bg)) {
                renderAffineLine(bg, priority, colors, priorities, winMask);
            } else if (textMask & (1u << bg)) {
                renderBackgroundLine(bg, line, priority, colors, priorities,
                                     winMask);
            }
        }
    }

    if (dispcnt & DISPCNT_OBJ_ON) {
        renderSpritesLine(line, colors, priorities, winMask);
    }

    for (int x = 0; x < SCREEN_WIDTH; ++x) {
        fb[line * SCREEN_WIDTH + x] = colors[x];
    }
}

void PPU::renderBackgroundLine(int bg, int line, int priority,
                               uint32_t* colors, int* priorities,
                               const uint8_t* winMask) {
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
        if (winMask && !(winMask[x] & (1u << bg))) {
            continue;  // this BG is disabled inside the active window region
        }
        colors[x] = paletteColor(colorIndex);
        priorities[x] = priority;
    }
}

// Affine BG: starting from the line's internal reference accumulator,
// the texture coordinate advances by the PA/PC matrix column per screen
// pixel. Affine maps differ from text maps: one byte per entry, always
// 8bpp tiles, and square sizes of 128/256/512/1024 pixels.
void PPU::renderAffineLine(int bg, int priority, uint32_t* colors,
                           int* priorities, const uint8_t* winMask) {
    const uint16_t cnt =
        bus.read16(REG_BG0CNT + static_cast<uint32_t>(bg) * 2);
    const uint32_t charBase = VRAM_BASE + ((cnt >> 2) & 3) * 0x4000;
    const uint32_t screenBase = VRAM_BASE + ((cnt >> 8) & 0x1F) * 0x800;
    const bool wrap = cnt & (1u << 13);
    const uint32_t sizeTiles = 16u << ((cnt >> 14) & 3);
    const int sizePx = static_cast<int>(sizeTiles) * 8;

    const uint32_t matrix =
        REG_BG2PA + static_cast<uint32_t>(bg - 2) * 0x10;
    const int32_t pa = static_cast<int16_t>(bus.read16(matrix));
    const int32_t pc = static_cast<int16_t>(bus.read16(matrix + 4));

    int32_t cx = affineX[bg - 2];
    int32_t cy = affineY[bg - 2];
    for (int x = 0; x < SCREEN_WIDTH; ++x, cx += pa, cy += pc) {
        int tx = cx >> 8;
        int ty = cy >> 8;
        if (wrap) {
            tx &= sizePx - 1;
            ty &= sizePx - 1;
        } else if (tx < 0 || ty < 0 || tx >= sizePx || ty >= sizePx) {
            continue;  // overflow bit clear: outside the map is transparent
        }
        const uint8_t tile = bus.read8(
            screenBase + static_cast<uint32_t>(ty >> 3) * sizeTiles +
            static_cast<uint32_t>(tx >> 3));
        const uint32_t colorIndex = bus.read8(
            charBase + tile * 64u +
            static_cast<uint32_t>((ty & 7) * 8 + (tx & 7)));
        if (colorIndex == 0) {
            continue;
        }
        if (winMask && !(winMask[x] & (1u << bg))) {
            continue;  // this BG is disabled inside the active window region
        }
        colors[x] = paletteColor(colorIndex);
        priorities[x] = priority;
    }
}

// BG2X/Y and BG3X/Y are 28-bit signed 19.8 fixed point. They reload the
// internal accumulators once per frame; the renderer never reads them
// directly, so a mid-frame write waits for the next frame while PB/PD
// keep accumulating from the latched origin.
void PPU::latchAffineReferences() {
    for (int i = 0; i < 2; ++i) {
        const uint32_t ref = REG_BG2X + static_cast<uint32_t>(i) * 0x10;
        affineX[i] = static_cast<int32_t>(bus.read32(ref) << 4) >> 4;
        affineY[i] = static_cast<int32_t>(bus.read32(ref + 4) << 4) >> 4;
    }
}

void PPU::advanceAffineReferences() {
    for (int i = 0; i < 2; ++i) {
        const uint32_t matrix =
            REG_BG2PA + static_cast<uint32_t>(i) * 0x10;
        affineX[i] += static_cast<int16_t>(bus.read16(matrix + 2));  // PB
        affineY[i] += static_cast<int16_t>(bus.read16(matrix + 6));  // PD
    }
}

// Window coordinates: WINxH packs X1 (left, inclusive) in the high byte and
// X2 (right, exclusive) in the low byte; WINxV does the same with Y1/Y2. A
// coordinate is inside when X1 <= p < X2; if the edges are reversed (X1 >
// X2) the region wraps around the screen edge, which also makes a right edge
// past 240 (or bottom past 160) span the whole axis. WIN0 outranks WIN1,
// then the outside region. The OBJ window tier is added with that feature.
bool PPU::computeWindowMask(int line, uint8_t* mask) const {
    const uint16_t dispcnt = bus.read16(REG_DISPCNT);
    const bool win0 = dispcnt & DISPCNT_WIN0;
    const bool win1 = dispcnt & DISPCNT_WIN1;
    const bool objwin = dispcnt & DISPCNT_OBJWIN;
    if (!win0 && !win1 && !objwin) {
        return false;
    }

    const uint16_t winin = bus.read16(REG_WININ);
    const uint16_t winout = bus.read16(REG_WINOUT);
    const uint8_t in0 = winin & 0x3F;
    const uint8_t in1 = (winin >> 8) & 0x3F;
    const uint8_t out = winout & 0x3F;

    auto edges = [](uint16_t v, int& lo, int& hi) {
        lo = (v >> 8) & 0xFF;  // X1/Y1 inclusive
        hi = v & 0xFF;         // X2/Y2 exclusive
    };
    auto inside = [](int p, int lo, int hi) {
        return lo <= hi ? (p >= lo && p < hi) : (p >= lo || p < hi);
    };

    int x0lo, x0hi, y0lo, y0hi, x1lo, x1hi, y1lo, y1hi;
    edges(bus.read16(REG_WIN0H), x0lo, x0hi);
    edges(bus.read16(REG_WIN0V), y0lo, y0hi);
    edges(bus.read16(REG_WIN1H), x1lo, x1hi);
    edges(bus.read16(REG_WIN1V), y1lo, y1hi);
    const bool on0 = win0 && inside(line, y0lo, y0hi);
    const bool on1 = win1 && inside(line, y1lo, y1hi);

    for (int x = 0; x < SCREEN_WIDTH; ++x) {
        if (on0 && inside(x, x0lo, x0hi)) {
            mask[x] = in0;
        } else if (on1 && inside(x, x1lo, x1hi)) {
            mask[x] = in1;
        } else {
            mask[x] = out;
        }
    }
    return true;
}

void PPU::renderSpritesLine(int line, uint32_t* colors,
                            const int* priorities, const uint8_t* winMask) {
    const bool map1D = bus.read16(REG_DISPCNT) & DISPCNT_OBJ_1D;

    // Iterate high to low so the lower OAM index (higher priority on
    // hardware) is drawn last and wins overlaps.
    for (int obj = 127; obj >= 0; --obj) {
        const uint32_t entry = OAM_BASE + static_cast<uint32_t>(obj) * 8;
        const uint16_t attr0 = bus.read16(entry);
        const uint16_t attr1 = bus.read16(entry + 2);
        const uint16_t attr2 = bus.read16(entry + 4);

        // attr0 bit 8 enables rotation/scaling; bit 9 doubles the scanned
        // box in that case but means "disabled" for a regular sprite.
        const bool affine = attr0 & 0x100;
        if (!affine && (attr0 & 0x200)) {
            continue;  // disabled
        }
        const uint32_t shape = attr0 >> 14;
        if (shape == 3) {
            continue;  // prohibited shape
        }
        const uint32_t sizeIdx = attr1 >> 14;
        const int width = OBJ_WIDTH[shape][sizeIdx];
        const int height = OBJ_HEIGHT[shape][sizeIdx];

        // A rotated image swings outside its nominal box; double-size
        // widens the scanned screen area so the corners aren't clipped.
        const bool doubleSize = affine && (attr0 & 0x200);
        const int boxW = doubleSize ? width * 2 : width;
        const int boxH = doubleSize ? height * 2 : height;

        int y = attr0 & 0xFF;
        if (y >= SCREEN_HEIGHT) {
            y -= 256;  // wraps negative
        }
        int row = line - y;
        if (row < 0 || row >= boxH) {
            continue;
        }
        if (!affine && (attr1 & 0x2000)) {
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
        const bool hflip = !affine && (attr1 & 0x1000);
        const uint32_t tileStep = is8bpp ? 2 : 1;  // tile units of 32 bytes

        // Rotation/scaling parameters: attr1 bits 9-13 pick one of the 32
        // groups interleaved through OAM, each PA/PB/PC/PD value sitting
        // in the fourth halfword of four consecutive entries. 8.8 signed
        // fixed point, the same format as the BG matrices.
        int32_t pa = 0x100, pb = 0, pc = 0, pd = 0x100;
        if (affine) {
            const uint32_t group = OAM_BASE + ((attr1 >> 9) & 0x1F) * 32;
            pa = static_cast<int16_t>(bus.read16(group + 6));
            pb = static_cast<int16_t>(bus.read16(group + 14));
            pc = static_cast<int16_t>(bus.read16(group + 22));
            pd = static_cast<int16_t>(bus.read16(group + 30));
        }

        for (int i = 0; i < boxW; ++i) {
            const int sx = x0 + i;
            if (sx < 0 || sx >= SCREEN_WIDTH) {
                continue;
            }
            if (winMask && !(winMask[sx] & (1u << 4))) {
                continue;  // OBJ disabled inside the active window region
            }
            if (priority > priorities[sx]) {
                continue;  // behind the background at this pixel
            }
            int col;
            int texRow;
            if (affine) {
                // Inverse transform: map the offset from the box center
                // back into texture space around the sprite's center; a
                // sample outside the texture is transparent.
                const int dx = i - boxW / 2;
                const int dy = row - boxH / 2;
                col = ((pa * dx + pb * dy) >> 8) + width / 2;
                texRow = ((pc * dx + pd * dy) >> 8) + height / 2;
                if (col < 0 || col >= width || texRow < 0 ||
                    texRow >= height) {
                    continue;
                }
            } else {
                col = hflip ? (width - 1 - i) : i;
                texRow = row;
            }
            const uint32_t tileX = static_cast<uint32_t>(col >> 3);
            const uint32_t tileY = static_cast<uint32_t>(texRow >> 3);
            const uint32_t rowStride =
                map1D ? static_cast<uint32_t>(width / 8) * tileStep : 32;
            const uint32_t tileNum =
                baseTile + tileY * rowStride + tileX * tileStep;
            const uint32_t tileAddr = VRAM_OBJ_BASE + (tileNum & 0x3FF) * 32;

            const int px = col & 7;
            const int py = texRow & 7;
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
                if (vcount == 0) {
                    latchAffineReferences();
                }
                renderScanline(vcount);
                advanceAffineReferences();
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
