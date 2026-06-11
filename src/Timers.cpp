#include "Timers.hpp"

#include "Bus.hpp"
#include "Log.hpp"

namespace {
constexpr uint32_t TIMER_BASE = 0x04000100;
constexpr int PRESCALER[4] = {1, 64, 256, 1024};
}  // namespace

Timers::Timers(Bus& bus) : bus(bus) {}

void Timers::publishCounter(int index) {
    bus.writeIODirect16(TIMER_BASE + static_cast<uint32_t>(index) * 4,
                        static_cast<uint16_t>(timers[index].counter));
}

void Timers::onRegisterWrite(uint32_t offset) {
    const int index = static_cast<int>(offset / 4);
    const uint32_t byte = offset % 4;
    Timer& t = timers[index];

    if (byte == 0 || byte == 1) {
        // TMnCNT_L: capture the written byte as reload, then restore the
        // live counter so reads keep returning it.
        const uint8_t value = bus.read8(TIMER_BASE + offset);
        if (byte == 0) {
            t.reload = static_cast<uint16_t>((t.reload & 0xFF00) | value);
        } else {
            t.reload =
                static_cast<uint16_t>((t.reload & 0x00FF) | (value << 8));
        }
        publishCounter(index);
    } else if (byte == 2) {
        const uint8_t ctrl =
            bus.read8(TIMER_BASE + static_cast<uint32_t>(index) * 4 + 2);
        const bool wasEnabled = t.enabled;
        t.prescaler = PRESCALER[ctrl & 3];
        t.cascade = (index > 0) && (ctrl & 0x04);
        t.irqEnabled = ctrl & 0x40;
        t.enabled = ctrl & 0x80;
        if (!wasEnabled && t.enabled) {
            // Enable edge: load the counter from the reload value.
            t.counter = t.reload;
            t.subCount = 0;
            publishCounter(index);
        }
    }
    // byte == 3: high control byte is unused.
}

void Timers::step(int cycles) {
    for (int i = 0; i < 4; ++i) {
        Timer& t = timers[i];
        if (!t.enabled || t.cascade) {
            continue;  // cascade timers tick only on the previous overflow
        }
        t.subCount += cycles;
        const uint32_t ticks = static_cast<uint32_t>(t.subCount / t.prescaler);
        t.subCount %= t.prescaler;
        if (ticks > 0) {
            advance(i, ticks);
        }
    }
}

void Timers::advance(int index, uint32_t ticks) {
    Timer& t = timers[index];
    uint64_t value = static_cast<uint64_t>(t.counter) + ticks;
    while (value > 0xFFFF) {
        // Overflow: reload and carry the surplus ticks forward.
        value = t.reload + (value - 0x10000);
        overflowed(index);
    }
    t.counter = static_cast<uint32_t>(value);
    publishCounter(index);
}

void Timers::overflowed(int index) {
    const Timer& t = timers[index];
    bus.notifyTimerOverflow(index);  // clocks the APU's Direct Sound FIFOs
    if (t.irqEnabled) {
        // Timer IRQs occupy IF bits 3-6.
        bus.requestInterrupt(static_cast<uint16_t>(Bus::IRQ_TIMER0 << index));
    }
    if (index < 3) {
        Timer& next = timers[index + 1];
        if (next.enabled && next.cascade) {
            advance(index + 1, 1);
        }
    }
}
