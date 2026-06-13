#pragma once

#include <cstdint>

class Bus;
class Serializer;
class Deserializer;

// The four DMA channels. Register block per channel n at 0x040000B0 + n*12:
//   +0x0  DMAnSAD    source address (32-bit)
//   +0x4  DMAnDAD    destination address (32-bit)
//   +0x8  DMAnCNT_L  word count (16-bit)
//   +0xA  DMAnCNT_H  control (16-bit)
// The Bus notifies this class when a control register is written and when
// the PPU enters HBlank/VBlank.
class DMA {
public:
    explicit DMA(Bus& bus);

    // Called by the Bus after a game writes DMAnCNT_H. Latches the internal
    // source/destination on an enable rising edge and runs immediate
    // transfers right away.
    void onControlWrite(int channel);

    // Timing-triggered transfers.
    void onVBlank();
    void onHBlank();

    // Sound FIFO refill (special timing on channels 1/2): transfers 4 words
    // to the requesting FIFO, ignoring the word count and keeping the
    // destination fixed.
    void onFifoRequest(int fifo);

    // Save-state support: snapshots/restores the four channels' active flags
    // and internal source/destination latches.
    void serialize(Serializer& s) const;
    void deserialize(Deserializer& d);

private:
    // Control register bits.
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
        // Internal address latches; the IO registers keep their written
        // values while these advance during transfers.
        uint32_t src = 0;
        uint32_t dst = 0;
    };

    void runForTiming(uint16_t timing);
    void transfer(int channel);

    Bus& bus;
    Channel channels[4];
};
