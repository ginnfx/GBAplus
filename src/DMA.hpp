#pragma once

#include <cstdint>

class Bus;

class DMA {
public:
    explicit DMA(Bus& bus);

    void onControlWrite(int channel);

    void onVBlank();
    void onHBlank();

    void onFifoRequest(int fifo);

private:
    static constexpr uint16_t CTRL_REPEAT  = 1u << 9;
    static constexpr uint16_t CTRL_WORD32  = 1u << 10;
    static constexpr uint16_t CTRL_IRQ     = 1u << 14;
    static constexpr uint16_t CTRL_ENABLE  = 1u << 15;

    enum Timing : uint16_t {
        TIMING_IMMEDIATE = 0,
        TIMING_VBLANK    = 1,
        TIMING_HBLANK    = 2,
        TIMING_SPECIAL   = 3,
    };

    struct Channel {
        bool active = false;
        uint32_t src = 0;
        uint32_t dst = 0;
    };

    void runForTiming(uint16_t timing);
    void transfer(int channel);

    Bus& bus;
    Channel channels[4];
};
