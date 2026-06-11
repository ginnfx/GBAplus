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
    void renderMode3(int line);
    void renderMode4(int line);
    // Shared compositor for the tiled modes: text and affine layers are
    // drawn priority-ordered into the line buffers, then sprites.
    void renderTiledLine(int line, unsigned textMask, unsigned affineMask);
    void renderBackgroundLine(int bg, int line, int priority,
                              uint32_t* colors, int* priorities,
                              const uint8_t* winMask);
    void renderAffineLine(int bg, int priority, uint32_t* colors,
                          int* priorities, const uint8_t* winMask);

    // Per-pixel layer-enable mask from the window registers: returns false
    // when no window is enabled (every layer draws everywhere), otherwise
    // fills mask[x] with BG0-3 in bits 0-3, OBJ in bit 4, and the colour-
    // effects enable in bit 5. A layer draws only where its bit is set.
    // objWin marks pixels covered by OBJ-window sprites for that tier.
    bool computeWindowMask(int line, uint8_t* mask,
                           const uint8_t* objWin) const;

    // Affine reference-point handling: the BG2X/Y (BG3X/Y) registers are
    // latched into internal accumulators when line 0 starts, then advanced
    // by PB/PD after every rendered scanline. Mid-frame register writes
    // do not take effect until the next frame.
    void latchAffineReferences();
    void advanceAffineReferences();
    // When objWin is non-null this runs as the OBJ-window pre-pass: only
    // OBJ-window-mode sprites are scanned and their opaque pixels set
    // objWin[x] instead of drawing. Otherwise it is the normal visible
    // pass, which skips OBJ-window-mode sprites.
    void renderSpritesLine(int line, uint32_t* colors,
                           const int* priorities, const uint8_t* winMask,
                           uint8_t* objWin);

    uint32_t paletteColor(uint32_t index) const;     // BG palette
    uint32_t objPaletteColor(uint32_t index) const;  // sprite palette

    Bus& bus;
    std::array<uint32_t, SCREEN_WIDTH * SCREEN_HEIGHT> fb{};

    int cycleCounter = 0;  // position within the current scanline
    int vcount = 0;
    int32_t affineX[2]{};  // internal BG2/BG3 reference accumulators, 19.8
    int32_t affineY[2]{};
    bool inHBlank = false;
    bool frameDone = false;
};
