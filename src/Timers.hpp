#pragma once

#include <cstdint>

class Bus;

class Timers {
public:
    explicit Timers(Bus& bus);

    void onRegisterWrite(uint32_t offset);

    void step(int cycles);

private:
    struct Timer {
        bool enabled = false;
        bool cascade = false;
        bool irqEnabled = false;
        int prescaler = 1;
        int subCount = 0;
        uint32_t counter = 0;
        uint16_t reload = 0;
    };

    void advance(int index, uint32_t ticks);
    void overflowed(int index);
    void publishCounter(int index);

    Bus& bus;
    Timer timers[4];
};
