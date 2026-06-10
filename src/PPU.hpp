#pragma once

#include <array>
#include <cstdint>

class Bus;

// Pixel processing unit. Reads VRAM and the LCD IO registers through the
// Bus and renders into an internal RGBA framebuffer.
class PPU {
public:
    static constexpr int SCREEN_WIDTH  = 240;
    static constexpr int SCREEN_HEIGHT = 160;

    // LCD timing (in CPU cycles; 4 cycles per dot).
    static constexpr int CYCLES_HDRAW    = 960;   // 240 dots
    static constexpr int CYCLES_HBLANK   = 272;   // 68 dots
    static constexpr int CYCLES_SCANLINE = 1232;  // 308 dots
    static constexpr int LINES_VISIBLE   = 160;
    static constexpr int LINES_TOTAL     = 228;   // 160 visible + 68 vblank

    explicit PPU(Bus& bus);

    // Advances the PPU by the given number of CPU cycles, rendering
    // scanlines and updating DISPSTAT/VCOUNT as lines complete.
    void step(int cycles);

    // True once a full frame has been rendered; cleared by the call.
    bool frameReady();

    // 240x160 pixels, 0xRRGGBBAA, row-major.
    const std::array<uint32_t, SCREEN_WIDTH * SCREEN_HEIGHT>& framebuffer()
        const {
        return fb;
    }

    // Expands 15-bit BGR (0BBBBBGGGGGRRRRR) to 32-bit RGBA (0xRRGGBBAA).
    static uint32_t bgr555ToRGBA(uint16_t color);

private:
    void renderScanline(int line);
    void renderMode0(int line);
    void renderMode3(int line);
    void renderBackgroundLine(int bg, int line, int priority,
                              uint32_t* colors, int* priorities);
    void renderSpritesLine(int line, uint32_t* colors,
                           const int* priorities);

    uint32_t paletteColor(uint32_t index) const;     // BG palette
    uint32_t objPaletteColor(uint32_t index) const;  // sprite palette

    Bus& bus;
    std::array<uint32_t, SCREEN_WIDTH * SCREEN_HEIGHT> fb{};

    int cycleCounter = 0;  // position within the current scanline
    int vcount = 0;
    bool inHBlank = false;
    bool frameDone = false;
};
