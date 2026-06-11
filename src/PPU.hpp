#pragma once

#include <array>
#include <cstdint>

class Bus;

class PPU {
public:
    static constexpr int SCREEN_WIDTH  = 240;
    static constexpr int SCREEN_HEIGHT = 160;

    static constexpr int CYCLES_HDRAW    = 960;
    static constexpr int CYCLES_HBLANK   = 272;
    static constexpr int CYCLES_SCANLINE = 1232;
    static constexpr int LINES_VISIBLE   = 160;
    static constexpr int LINES_TOTAL     = 228;

    explicit PPU(Bus& bus);

    void step(int cycles);

    bool frameReady();

    const std::array<uint32_t, SCREEN_WIDTH * SCREEN_HEIGHT>& framebuffer()
        const {
        return fb;
    }

    static uint32_t bgr555ToRGBA(uint16_t color);

private:
    void renderScanline(int line);
    void renderMode3(int line);
    void renderMode4(int line);
    void renderTiledLine(int line, unsigned textMask, unsigned affineMask);
    void renderBackgroundLine(int bg, int line, int priority,
                              uint32_t* colors, int* priorities,
                              const uint8_t* winMask);
    void renderAffineLine(int bg, int priority, uint32_t* colors,
                          int* priorities, const uint8_t* winMask);

    bool computeWindowMask(int line, uint8_t* mask,
                           const uint8_t* objWin) const;

    void latchAffineReferences();
    void advanceAffineReferences();
    void renderSpritesLine(int line, uint32_t* colors,
                           const int* priorities, const uint8_t* winMask,
                           uint8_t* objWin);

    uint32_t paletteColor(uint32_t index) const;
    uint32_t objPaletteColor(uint32_t index) const;

    Bus& bus;
    std::array<uint32_t, SCREEN_WIDTH * SCREEN_HEIGHT> fb{};

    int cycleCounter = 0;
    int vcount = 0;
    int32_t affineX[2]{};
    int32_t affineY[2]{};
    bool inHBlank = false;
    bool frameDone = false;
};
