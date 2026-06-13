#include "PPU.hpp"

#include <algorithm>

#include "Bus.hpp"
#include "Log.hpp"

namespace {
constexpr uint32_t REG_DISPCNT  = 0x04000000;
constexpr uint32_t REG_DISPSTAT = 0x04000004;
constexpr uint32_t REG_VCOUNT   = 0x04000006;
constexpr uint32_t REG_BG0CNT   = 0x04000008;
constexpr uint32_t REG_BG0HOFS  = 0x04000010;
constexpr uint32_t REG_BG2PA    = 0x04000020;
constexpr uint32_t REG_BG2X     = 0x04000028;
constexpr uint32_t REG_WIN0H    = 0x04000040;
constexpr uint32_t REG_WIN0V    = 0x04000044;
constexpr uint32_t REG_WIN1H    = 0x04000042;
constexpr uint32_t REG_WIN1V    = 0x04000046;
constexpr uint32_t REG_WININ    = 0x04000048;
constexpr uint32_t REG_WINOUT   = 0x0400004A;
constexpr uint32_t REG_MOSAIC   = 0x0400004C;
constexpr uint32_t REG_BLDCNT   = 0x04000050;
constexpr uint32_t REG_BLDALPHA = 0x04000052;
constexpr uint32_t REG_BLDY     = 0x04000054;

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

constexpr int OBJ_WIDTH[3][4]  = {{8, 16, 32, 64},
                                  {16, 32, 32, 64},
                                  {8, 8, 16, 32}};
constexpr int OBJ_HEIGHT[3][4] = {{8, 16, 32, 64},
                                  {8, 8, 16, 32},
                                  {16, 32, 32, 64}};

constexpr int PRIORITY_BACKDROP = 4;

constexpr uint16_t TRANSPARENT = 0x8000;

uint16_t alphaBlend(uint16_t a, uint16_t b, int eva, int evb) {
    int r = std::min(31, (((a) & 31) * eva + ((b) & 31) * evb) >> 4);
    int g = std::min(31, (((a >> 5) & 31) * eva + ((b >> 5) & 31) * evb) >> 4);
    int bl = std::min(31, (((a >> 10) & 31) * eva + ((b >> 10) & 31) * evb) >> 4);
    return static_cast<uint16_t>(r | (g << 5) | (bl << 10));
}

uint16_t brighten(uint16_t a, int evy) {
    int r = (a & 31), g = (a >> 5) & 31, b = (a >> 10) & 31;
    r += ((31 - r) * evy) >> 4;
    g += ((31 - g) * evy) >> 4;
    b += ((31 - b) * evy) >> 4;
    return static_cast<uint16_t>(r | (g << 5) | (b << 10));
}

uint16_t darken(uint16_t a, int evy) {
    int r = (a & 31), g = (a >> 5) & 31, b = (a >> 10) & 31;
    r -= (r * evy) >> 4;
    g -= (g * evy) >> 4;
    b -= (b * evy) >> 4;
    return static_cast<uint16_t>(r | (g << 5) | (b << 10));
}
}

PPU::PPU(Bus& bus) : bus(bus) {}

uint32_t PPU::bgr555ToRGBA(uint16_t color) {
    const uint32_t r5 = color & 0x1F;
    const uint32_t g5 = (color >> 5) & 0x1F;
    const uint32_t b5 = (color >> 10) & 0x1F;
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
        case 4:
        case 5:
            renderBitmapLine(line, mode);
            break;
        default:
            TRACE_LOG("renderScanline: unimplemented video mode %u", mode);
            for (int x = 0; x < SCREEN_WIDTH; ++x) {
                fb[line * SCREEN_WIDTH + x] = 0x000000FF;
            }
            break;
    }
}

void PPU::renderBitmapLine(int line, int mode) {
    (void)line;
    const uint16_t dispcnt = bus.read16(REG_DISPCNT);
    const bool bg2on = dispcnt & (1u << 10);
    const uint16_t bg2cnt = bus.read16(REG_BG0CNT + 4);
    const int bgPriority = bg2cnt & 3;

    const int bmpW = (mode == 5) ? 160 : 240;
    const int bmpH = (mode == 5) ? 128 : 160;
    const bool paletted = (mode == 4);
    const uint32_t page =
        (mode != 3 && (dispcnt & (1u << 4))) ? 0xA000u : 0u;

    const int32_t pa = static_cast<int16_t>(bus.read16(REG_BG2PA));
    const int32_t pc = static_cast<int16_t>(bus.read16(REG_BG2PA + 4));
    int32_t cx = affineX[0];
    int32_t cy = affineY[0];

    uint32_t colors[SCREEN_WIDTH];
    int priorities[SCREEN_WIDTH];
    const uint32_t backdrop = paletteColor(0);
    for (int x = 0; x < SCREEN_WIDTH; ++x, cx += pa, cy += pc) {
        colors[x] = backdrop;
        priorities[x] = PRIORITY_BACKDROP;
        if (!bg2on) {
            continue;
        }
        const int tx = cx >> 8;
        const int ty = cy >> 8;
        if (tx < 0 || ty < 0 || tx >= bmpW || ty >= bmpH) {
            continue;
        }
        if (paletted) {
            const uint8_t index = bus.read8(
                VRAM_BASE + page + static_cast<uint32_t>(ty * bmpW + tx));
            if (index != 0) {
                colors[x] = paletteColor(index);
                priorities[x] = bgPriority;
            }
        } else {
            const uint32_t addr = VRAM_BASE + page +
                static_cast<uint32_t>(ty * bmpW + tx) * 2;
            colors[x] = bgr555ToRGBA(bus.read16(addr));
            priorities[x] = bgPriority;
        }
    }

    if (dispcnt & DISPCNT_OBJ_ON) {
        uint16_t objColor[SCREEN_WIDTH];
        uint8_t objPrio[SCREEN_WIDTH];
        uint8_t objSemi[SCREEN_WIDTH];
        for (int x = 0; x < SCREEN_WIDTH; ++x) objColor[x] = TRANSPARENT;
        renderSpritesLine(line, objColor, objPrio, objSemi, nullptr);
        for (int x = 0; x < SCREEN_WIDTH; ++x) {
            if (objColor[x] != TRANSPARENT && objPrio[x] <= priorities[x]) {
                colors[x] = bgr555ToRGBA(objColor[x]);
            }
        }
    }

    for (int x = 0; x < SCREEN_WIDTH; ++x) {
        fb[line * SCREEN_WIDTH + x] = colors[x];
    }
}

void PPU::renderTiledLine(int line, unsigned textMask, unsigned affineMask) {
    const uint16_t dispcnt = bus.read16(REG_DISPCNT);
    const uint16_t backdrop = bus.read16(PALETTE_BG) & 0x7FFF;

    uint8_t objWinBuf[SCREEN_WIDTH] = {0};
    if ((dispcnt & DISPCNT_OBJWIN) && (dispcnt & DISPCNT_OBJ_ON)) {
        renderSpritesLine(line, nullptr, nullptr, nullptr, objWinBuf);
    }

    uint8_t winMaskBuf[SCREEN_WIDTH];
    const uint8_t* winMask =
        computeWindowMask(line, winMaskBuf, objWinBuf) ? winMaskBuf : nullptr;

    uint16_t bgLine[4][SCREEN_WIDTH];
    bool bgOn[4] = {false, false, false, false};
    int bgPrio[4] = {0, 0, 0, 0};
    for (int bg = 0; bg < 4; ++bg) {
        if (!(dispcnt & (1u << (8 + bg)))) {
            continue;
        }
        const uint16_t cnt =
            bus.read16(REG_BG0CNT + static_cast<uint32_t>(bg) * 2);
        if (affineMask & (1u << bg)) {
            renderAffineLine(bg, bgLine[bg]);
        } else if (textMask & (1u << bg)) {
            renderBackgroundLine(bg, line, bgLine[bg]);
        } else {
            continue;
        }
        bgOn[bg] = true;
        bgPrio[bg] = cnt & 3;
    }

    uint16_t objColor[SCREEN_WIDTH];
    uint8_t objPrio[SCREEN_WIDTH];
    uint8_t objSemi[SCREEN_WIDTH];
    const bool objOn = dispcnt & DISPCNT_OBJ_ON;
    if (objOn) {
        for (int x = 0; x < SCREEN_WIDTH; ++x) objColor[x] = TRANSPARENT;
        renderSpritesLine(line, objColor, objPrio, objSemi, nullptr);
    }

    const uint16_t bldcnt = bus.read16(REG_BLDCNT);
    const int bldMode = (bldcnt >> 6) & 3;
    const uint16_t bldalpha = bus.read16(REG_BLDALPHA);
    const int eva = std::min(16, bldalpha & 0x1F);
    const int evb = std::min(16, (bldalpha >> 8) & 0x1F);
    const int evy = std::min(16, bus.read16(REG_BLDY) & 0x1F);

    for (int x = 0; x < SCREEN_WIDTH; ++x) {
        int k0 = PRIORITY_BACKDROP * 8 + 7, k1 = k0;
        uint16_t c0 = backdrop, c1 = backdrop;
        int l0 = 5, l1 = 5;
        bool semi0 = false;
        auto consider = [&](uint16_t color, int prio, int layer, int rank,
                            bool semi) {
            const int key = prio * 8 + rank;
            if (key < k0) {
                k1 = k0; c1 = c0; l1 = l0;
                k0 = key; c0 = color; l0 = layer; semi0 = semi;
            } else if (key < k1) {
                k1 = key; c1 = color; l1 = layer;
            }
        };
        for (int bg = 0; bg < 4; ++bg) {
            if (bgOn[bg] && bgLine[bg][x] != TRANSPARENT &&
                (!winMask || (winMask[x] & (1u << bg)))) {
                consider(bgLine[bg][x], bgPrio[bg], bg, bg + 1, false);
            }
        }
        if (objOn && objColor[x] != TRANSPARENT &&
            (!winMask || (winMask[x] & (1u << 4)))) {
            consider(objColor[x], objPrio[x], 4, 0, objSemi[x]);
        }

        const bool effects = !winMask || (winMask[x] & 0x20);
        const bool top1st = bldcnt & (1u << l0);
        const bool sec2nd = bldcnt & (1u << (8 + l1));
        uint16_t out;
        if (effects && l0 == 4 && semi0) {
            out = sec2nd ? alphaBlend(c0, c1, eva, evb) : c0;
        } else if (effects && bldMode == 1 && top1st && sec2nd) {
            out = alphaBlend(c0, c1, eva, evb);
        } else if (effects && bldMode == 2 && top1st) {
            out = brighten(c0, evy);
        } else if (effects && bldMode == 3 && top1st) {
            out = darken(c0, evy);
        } else {
            out = c0;
        }
        fb[line * SCREEN_WIDTH + x] = bgr555ToRGBA(out);
    }
}

void PPU::renderBackgroundLine(int bg, int line, uint16_t* out) {
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

    const uint16_t mosaic = bus.read16(REG_MOSAIC);
    const int mosW = (cnt & 0x40) ? (mosaic & 0xF) + 1 : 1;
    const int mosH = (cnt & 0x40) ? ((mosaic >> 4) & 0xF) + 1 : 1;

    const int effLine = (line / mosH) * mosH;
    const int vy = (effLine + vofs) & (heightTiles * 8 - 1);
    const int tileY = vy >> 3;

    for (int x = 0; x < SCREEN_WIDTH; ++x) {
        const int effX = (x / mosW) * mosW;
        const int vx = (effX + hofs) & (widthTiles * 8 - 1);
        const int tileX = vx >> 3;

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
        if (entry & 0x400) px = 7 - px;
        if (entry & 0x800) py = 7 - py;

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
                colorIndex += ((entry >> 12) & 0xF) * 16;
            }
        }
        if (colorIndex == 0) {
            out[x] = TRANSPARENT;
            continue;
        }
        out[x] = bus.read16(PALETTE_BG + colorIndex * 2) & 0x7FFF;
    }
}

void PPU::renderAffineLine(int bg, uint16_t* out) {
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
        out[x] = TRANSPARENT;
        int tx = cx >> 8;
        int ty = cy >> 8;
        if (wrap) {
            tx &= sizePx - 1;
            ty &= sizePx - 1;
        } else if (tx < 0 || ty < 0 || tx >= sizePx || ty >= sizePx) {
            continue;
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
        out[x] = bus.read16(PALETTE_BG + colorIndex * 2) & 0x7FFF;
    }
}

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
        affineX[i] += static_cast<int16_t>(bus.read16(matrix + 2));
        affineY[i] += static_cast<int16_t>(bus.read16(matrix + 6));
    }
}

bool PPU::computeWindowMask(int line, uint8_t* mask,
                            const uint8_t* objWin) const {
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
    const uint8_t obj = (winout >> 8) & 0x3F;

    auto edges = [](uint16_t v, int& lo, int& hi) {
        lo = (v >> 8) & 0xFF;
        hi = v & 0xFF;
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
        } else if (objwin && objWin[x]) {
            mask[x] = obj;
        } else {
            mask[x] = out;
        }
    }
    return true;
}

void PPU::renderSpritesLine(int line, uint16_t* objColor, uint8_t* objPrio,
                            uint8_t* objSemi, uint8_t* objWin) {
    const bool map1D = bus.read16(REG_DISPCNT) & DISPCNT_OBJ_1D;
    const bool objWinPass = objWin != nullptr;

    for (int obj = 127; obj >= 0; --obj) {
        const uint32_t entry = OAM_BASE + static_cast<uint32_t>(obj) * 8;
        const uint16_t attr0 = bus.read16(entry);
        const uint16_t attr1 = bus.read16(entry + 2);
        const uint16_t attr2 = bus.read16(entry + 4);

        const bool affine = attr0 & 0x100;
        if (!affine && (attr0 & 0x200)) {
            continue;
        }
        const uint32_t shape = attr0 >> 14;
        if (shape == 3) {
            continue;
        }
        const uint32_t objMode = (attr0 >> 10) & 3;
        if (objWinPass ? (objMode != 2) : (objMode == 2)) {
            continue;
        }
        const uint32_t sizeIdx = attr1 >> 14;
        const int width = OBJ_WIDTH[shape][sizeIdx];
        const int height = OBJ_HEIGHT[shape][sizeIdx];

        const bool doubleSize = affine && (attr0 & 0x200);
        const int boxW = doubleSize ? width * 2 : width;
        const int boxH = doubleSize ? height * 2 : height;

        int y = attr0 & 0xFF;
        if (y >= SCREEN_HEIGHT) {
            y -= 256;
        }
        int row = line - y;
        if (row < 0 || row >= boxH) {
            continue;
        }
        if (!affine && (attr1 & 0x2000)) {
            row = height - 1 - row;
        }

        int x0 = attr1 & 0x1FF;
        if (x0 >= SCREEN_WIDTH) {
            x0 -= 512;
        }

        const bool is8bpp = attr0 & 0x2000;
        const uint32_t baseTile = attr2 & 0x3FF;
        if ((bus.read16(REG_DISPCNT) & 0x7) >= 3 && baseTile < 512) {
            continue;
        }
        const int priority = (attr2 >> 10) & 3;
        const uint32_t palBank = attr2 >> 12;
        const bool hflip = !affine && (attr1 & 0x1000);
        const uint32_t tileStep = is8bpp ? 2 : 1;

        int32_t pa = 0x100, pb = 0, pc = 0, pd = 0x100;
        if (affine) {
            const uint32_t group = OAM_BASE + ((attr1 >> 9) & 0x1F) * 32;
            pa = static_cast<int16_t>(bus.read16(group + 6));
            pb = static_cast<int16_t>(bus.read16(group + 14));
            pc = static_cast<int16_t>(bus.read16(group + 22));
            pd = static_cast<int16_t>(bus.read16(group + 30));
        }

        const bool mosaicObj = attr0 & 0x1000;
        const uint16_t mosaic = bus.read16(REG_MOSAIC);
        const int mosW = mosaicObj ? ((mosaic >> 8) & 0xF) + 1 : 1;
        const int mosH = mosaicObj ? ((mosaic >> 12) & 0xF) + 1 : 1;
        const int mrow = (row / mosH) * mosH;

        for (int i = 0; i < boxW; ++i) {
            const int sx = x0 + i;
            if (sx < 0 || sx >= SCREEN_WIDTH) {
                continue;
            }
            const int mi = (i / mosW) * mosW;
            int col;
            int texRow;
            if (affine) {
                const int dx = mi - boxW / 2;
                const int dy = mrow - boxH / 2;
                col = ((pa * dx + pb * dy) >> 8) + width / 2;
                texRow = ((pc * dx + pd * dy) >> 8) + height / 2;
                if (col < 0 || col >= width || texRow < 0 ||
                    texRow >= height) {
                    continue;
                }
            } else {
                col = hflip ? (width - 1 - mi) : mi;
                texRow = mrow;
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
                continue;
            }
            if (objWinPass) {
                objWin[sx] = 1;
                continue;
            }
            objColor[sx] =
                bus.read16(PALETTE_OBJ + colorIndex * 2) & 0x7FFF;
            objPrio[sx] = static_cast<uint8_t>(priority);
            objSemi[sx] = objMode == 1;
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
                bus.notifyHBlank();
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
            bus.notifyVBlank();
        } else if (vcount == 0) {
            stat &= static_cast<uint16_t>(~DISPSTAT_VBLANK);
        }

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
