#pragma once

#include <cstdint>

class Bus;
class Serializer;
class Deserializer;

// The four hardware timers. Register block per timer n at 0x04000100 + n*4:
//   +0  TMnCNT_L  reading returns the live counter; writing sets the reload
//                 value applied at the next overflow or enable edge
//   +2  TMnCNT_H  control: bits 0-1 prescaler (1/64/256/1024), bit 2
//                 cascade (timers 1-3), bit 6 IRQ enable, bit 7 enable
// The Bus notifies this class of register writes; the live counter is
// pushed back into IO so games read it at TMnCNT_L.
class Timers {
public:
    explicit Timers(Bus& bus);

    // Called by the Bus after a game writes a byte in 0x100-0x10F;
    // `offset` is relative to 0x04000100.
    void onRegisterWrite(uint32_t offset);

    // Advances all running timers by the given number of CPU cycles.
    void step(int cycles);

    // Save-state support: snapshots/restores the four timers' control and
    // counter state.
    void serialize(Serializer& s) const;
    void deserialize(Deserializer& d);

private:
    struct Timer {
        bool enabled = false;
        bool cascade = false;
        bool irqEnabled = false;
        int prescaler = 1;
        int subCount = 0;      // cycles accumulated toward the next tick
        uint32_t counter = 0;  // current 16-bit count
        uint16_t reload = 0;
    };

    void advance(int index, uint32_t ticks);
    void overflowed(int index);
    void publishCounter(int index);

    Bus& bus;
    Timer timers[4];
};
