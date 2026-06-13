#pragma once

#include <array>
#include <cstdint>

class Bus;
class Serializer;
class Deserializer;

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

    void serialize(Serializer& s) const;
    void deserialize(Deserializer& d);

private:
    void renderScanline(int line);
    void renderMode3(int line);
    void renderMode4(int line);
    void renderTiledLine(int line, unsigned textMask, unsigned affineMask);
    void renderBackgroundLine(int bg, int line, uint16_t* out);
    void renderAffineLine(int bg, uint16_t* out);

    bool computeWindowMask(int line, uint8_t* mask,
                           const uint8_t* objWin) const;

    void latchAffineReferences();
    void advanceAffineReferences();
    void renderSpritesLine(int line, uint16_t* objColor, uint8_t* objPrio,
                           uint8_t* objSemi, uint8_t* objWin);

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
