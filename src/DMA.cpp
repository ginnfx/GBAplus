#include "DMA.hpp"

#include "Bus.hpp"
#include "Log.hpp"

namespace {
constexpr uint32_t channelBase(int channel) {
    return 0x040000B0 + static_cast<uint32_t>(channel) * 12;
}
}

DMA::DMA(Bus& bus) : bus(bus) {}

void DMA::onControlWrite(int channel) {
    const uint16_t ctrl = bus.read16(channelBase(channel) + 0xA);
    Channel& ch = channels[channel];

    if (!(ctrl & CTRL_ENABLE)) {
        ch.active = false;
        return;
    }
    if (ch.active) {
        return;
    }

    ch.active = true;
    ch.src = bus.read32(channelBase(channel) + 0x0);
    ch.dst = bus.read32(channelBase(channel) + 0x4);

    if (((ctrl >> 12) & 3) == TIMING_IMMEDIATE) {
        transfer(channel);
    }
}

void DMA::onVBlank() {
    runForTiming(TIMING_VBLANK);
}

void DMA::onHBlank() {
    runForTiming(TIMING_HBLANK);
}

void DMA::runForTiming(uint16_t timing) {
    for (int i = 0; i < 4; ++i) {
        if (!channels[i].active) {
            continue;
        }
        const uint16_t ctrl = bus.read16(channelBase(i) + 0xA);
        if ((ctrl & CTRL_ENABLE) && ((ctrl >> 12) & 3) == timing) {
            transfer(i);
        }
    }
}

void DMA::transfer(int channel) {
    const uint32_t base = channelBase(channel);
    const uint16_t ctrl = bus.read16(base + 0xA);
    Channel& ch = channels[channel];

    uint32_t count = bus.read16(base + 0x8);
    const uint32_t maxCount = (channel == 3) ? 0x10000 : 0x4000;
    if (count == 0) {
        count = maxCount;
    }

    const bool word32 = ctrl & CTRL_WORD32;
    const int unit = word32 ? 4 : 2;
    const uint16_t dstAdjust = (ctrl >> 5) & 3;
    const uint16_t srcAdjust = (ctrl >> 7) & 3;

    TRACE_LOG("DMA%d: %u x %d bytes 0x%08X -> 0x%08X (ctrl=0x%04X)", channel,
              count, unit, ch.src, ch.dst, ctrl);

    const uint32_t dstStart = ch.dst;
    for (uint32_t i = 0; i < count; ++i) {
        if (word32) {
            bus.write32(ch.dst, bus.read32(ch.src));
        } else {
            bus.write16(ch.dst, bus.read16(ch.src));
        }
        if (srcAdjust == 0) ch.src += static_cast<uint32_t>(unit);
        if (srcAdjust == 1) ch.src -= static_cast<uint32_t>(unit);
        if (dstAdjust == 0 || dstAdjust == 3) ch.dst += static_cast<uint32_t>(unit);
        if (dstAdjust == 1) ch.dst -= static_cast<uint32_t>(unit);
    }

    if (ctrl & CTRL_IRQ) {
        bus.requestInterrupt(static_cast<uint16_t>(Bus::IRQ_DMA0 << channel));
    }

    const uint16_t timing = (ctrl >> 12) & 3;
    if ((ctrl & CTRL_REPEAT) && timing != TIMING_IMMEDIATE) {
        if (dstAdjust == 3) {
            ch.dst = dstStart;
        }
    } else {
        bus.write16(base + 0xA, ctrl & static_cast<uint16_t>(~CTRL_ENABLE));
    }
}
